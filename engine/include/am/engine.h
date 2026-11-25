// Auto Malign engine core: framework-free, header-only.
//
// Pipeline (static mode, the M0-validated path):
//   1. CrossSpectra  - every linked track is analysed on the same timeline frames (16384-pt Hann STFT).
//                      Per bin we accumulate C_ij = sum_frames X_i * conj(X_j) for bins up to ~4.5 kHz.
//   2. solve()       - per bin, coordinate ascent on the summed-mix energy
//                      E = sum_ij e^{j(th_i - th_j)} C_ij. Align maximises E, malign (anti) minimises it.
//                      An anchor track stays at 0. A track only rotates where it pays (tau of its own energy).
//                      The result is smoothed across frequency (caps group delay) and tapered at the band edge.
//   3. effectivePhase() - the user's knob / flavour turn the solved maps into one phase curve per track
//                      for Mid and one for Side.
//   4. matchGainDb() - the long-term spectrum of the new mix vs the original, so a compensating EQ can make
//                      alignment level- and tone-neutral ("Match").
//   5. buildIR()     - phase curve (+ match EQ) -> linear-phase FIR (centred), loaded into a convolver.
//
// All of this runs on worker threads. Nothing here allocates on, or is called from, the audio thread.
#pragma once
#include "fft.h"
#include <algorithm>
#include <cstdint>
#include <random>
#include <string>

namespace am {

constexpr int kSolveN = 16384;          // analysis FFT (2.9 Hz bins at 48 kHz)
constexpr int kHop = 4096;              // analysis hop (75% overlap)
constexpr double kMaxSolveHz = 4500.0;  // highest bin we keep cross-spectra for
constexpr int kIRLen = 8192;            // FIR length; reported latency = kIRLen/2

// ---------------------------------------------------------------- cross-spectra accumulator
class CrossSpectra {
public:
    void reset(int tracks, double sampleRate) {
        T = tracks;
        sr = sampleRate;
        binHz = sr / kSolveN;
        K = std::min(kSolveN / 2, (int)std::ceil(kMaxSolveHz / binHz) + 2);
        C.assign((size_t)T * T * K, cplx(0, 0));
        frames = 0;
    }

    // X[t] points at K complex bins of track t for one frame (same timeline position for all tracks).
    void addFrame(const std::vector<const cplx*>& X) {
        assert((int)X.size() == T);
        for (int i = 0; i < T; ++i)
            for (int j = i; j < T; ++j) {
                cplx* c = &C[((size_t)i * T + j) * K];
                const cplx* a = X[i];
                const cplx* b = X[j];
                for (int k = 0; k < K; ++k) c[k] += a[k] * std::conj(b[k]);
            }
        ++frames;
    }

    // C_ij at bin k (only i<=j is stored; C_ji = conj(C_ij)).
    cplx at(int i, int j, int k) const {
        if (i <= j) return C[((size_t)i * T + j) * K + k];
        return std::conj(C[((size_t)j * T + i) * K + k]);
    }

    int T = 0, K = 0;
    double sr = 48000, binHz = 48000.0 / kSolveN;
    int64_t frames = 0;
    std::vector<cplx> C;
};

// Window + FFT one analysis frame of a single track and return its first K bins.
inline void analyseFrame(const FFT& fft, const std::vector<double>& win, const float* frame,
                         std::vector<cplx>& scratch, cplx* outK, int K) {
    const int n = fft.size();
    scratch.resize(n);
    for (int i = 0; i < n; ++i) scratch[i] = cplx(frame[i] * win[i], 0.0);
    fft.transform(scratch);
    for (int k = 0; k < K; ++k) outK[k] = scratch[k];
}

// ---------------------------------------------------------------- solver
struct Profile {
    double fLo = 2.7;       // below this: untouched (DC cannot be rotated)
    double fHi = 1000.0;    // full strength up to here
    double fTaper = 1500.0; // fades to zero by here (log-frequency taper)
    double tau = 0.05;      // rotate only where |gain| > tau * own energy
    double smoothHz = 12.0; // complex smoothing width across frequency
    bool anti = false;      // false = align (max energy), true = malign (min energy)
};

inline Profile alignStandard() { return {}; }
inline Profile malignStandard() { Profile p; p.anti = true; p.fHi = 1500; p.fTaper = 2200; return p; }
inline Profile alignExtended() { Profile p; p.tau = 0.0; p.fHi = 4000; p.fTaper = 4400; p.smoothHz = 18; return p; }
inline Profile malignExtended() { Profile p = alignExtended(); p.anti = true; return p; }

inline double taperWeight(const Profile& p, double f) {
    if (f < p.fLo) return 0.0;
    if (f <= p.fHi) return 1.0;
    if (f >= p.fTaper) return 0.0;
    const double u = std::log(f / p.fHi) / std::log(p.fTaper / p.fHi);
    return 0.5 + 0.5 * std::cos(M_PI * u);
}

// Picks the track with the most energy below fHi as the anchor.
inline int autoAnchor(const CrossSpectra& cs, double fHi = 1000.0) {
    int best = 0;
    double bestE = -1;
    const int kHi = std::min(cs.K, (int)(fHi / cs.binHz));
    for (int t = 0; t < cs.T; ++t) {
        double e = 0;
        for (int k = 1; k < kHi; ++k) e += cs.at(t, t, k).real();
        if (e > bestE) { bestE = e; best = t; }
    }
    return best;
}

// Returns phase[t][k] (radians) for every track t and solve bin k (size cs.K).
inline std::vector<std::vector<float>> solve(const CrossSpectra& cs, int anchor, const Profile& p, int sweeps = 8) {
    const int T = cs.T, K = cs.K;
    std::vector<std::vector<float>> out(T, std::vector<float>(K, 0.f));
    if (T < 2 || cs.frames == 0) return out;
    std::vector<double> th(T);
    for (int k = 1; k < K; ++k) {
        const double f = k * cs.binHz;
        if (taperWeight(p, f) <= 0) continue;
        std::fill(th.begin(), th.end(), 0.0);
        for (int s = 0; s < sweeps; ++s)
            for (int i = 0; i < T; ++i) {
                if (i == anchor) continue;
                cplx si(0, 0);
                for (int j = 0; j < T; ++j)
                    if (j != i) si += cs.at(i, j, k) * std::polar(1.0, -th[j]);
                const double own = cs.at(i, i, k).real();
                if (std::abs(si) <= p.tau * own || std::abs(si) == 0.0) { th[i] = 0.0; continue; }
                th[i] = p.anti ? M_PI - std::arg(si) : -std::arg(si);
            }
        for (int t = 0; t < T; ++t) out[t][k] = (float)std::remainder(th[t], 2 * M_PI);
    }
    // Smooth across frequency on the unit circle (energy weighted), then taper.
    const int half = std::max(1, (int)std::round(p.smoothHz / cs.binHz / 2));
    std::vector<float> tmp(K);
    for (int t = 0; t < T; ++t) {
        for (int k = 0; k < K; ++k) {
            cplx acc(0, 0);
            for (int d = -half; d <= half; ++d) {
                const int q = k + d;
                if (q < 1 || q >= K) continue;
                const double w = cs.at(t, t, q).real() + 1e-30;
                acc += w * std::polar(1.0, (double)out[t][q]);
            }
            tmp[k] = std::abs(acc) > 0 ? (float)std::arg(acc) : 0.f;
        }
        for (int k = 0; k < K; ++k) out[t][k] = (float)(tmp[k] * taperWeight(p, k * cs.binHz));
    }
    return out;
}

// ---------------------------------------------------------------- user controls -> phase curves
enum class Flavour { Fuse, Erase, Ghost, Phantom, Unmask, Envelop, Curve, Disperse };

inline Flavour flavourFromString(const std::string& s) {
    static const char* names[] = {"FUSE", "ERASE", "GHOST", "PHANTOM", "UNMASK", "ENVELOP", "CURVE", "DISPERSE"};
    for (int i = 0; i < 8; ++i) if (s == names[i]) return (Flavour)i;
    return Flavour::Fuse;
}

struct Maps { // solved per-track phase maps for one session solve
    std::vector<std::vector<float>> alignStd, malignStd, alignExt, malignExt;
    double binHz = 48000.0 / kSolveN;
    int K = 0;
};

struct Controls {
    double knob = 0.0;     // -1 = full align, 0 = neutral (as recorded), +1 = full malign
    Flavour flavour = Flavour::Fuse;
    uint32_t seed = 1;     // per-track seed for Disperse
};

struct PhaseCurves { std::vector<float> mid, side; }; // per solve bin

// Align side always runs the standard alignment. The flavour decides what the malign side does.
inline PhaseCurves effectivePhase(const Maps& m, int track, const Controls& c) {
    PhaseCurves pc;
    pc.mid.assign(m.K, 0.f);
    pc.side.assign(m.K, 0.f);
    if (m.K == 0 || track < 0 || track >= (int)m.alignStd.size()) return pc;
    const double a = std::clamp(c.knob, -1.0, 1.0);
    if (a <= 0) { // align
        for (int k = 0; k < m.K; ++k) pc.mid[k] = pc.side[k] = (float)(-a * m.alignStd[track][k]);
        return pc;
    }
    const double amt = a;
    auto bandW = [](double f, double lo, double hi) { // soft band window on log f
        if (f <= 0) return 0.0;
        const double e = 0.25; // octaves of fade
        const double l = std::log2(f / lo), h = std::log2(hi / f);
        return std::clamp(std::min(l, h) / e + 0.5, 0.0, 1.0);
    };
    std::mt19937 rng(c.seed * 2654435761u + 17);
    std::uniform_real_distribution<double> uni(-1.0, 1.0);
    std::vector<double> scatter(40);
    for (auto& v : scatter) v = uni(rng);
    for (int k = 0; k < m.K; ++k) {
        const double f = k * m.binHz;
        double mid = 0, side = 0;
        switch (c.flavour) {
            case Flavour::Fuse:    mid = side = amt * m.alignExt[track][k]; break;           // hyper-align, wider band, glued
            case Flavour::Erase:   mid = side = amt * m.malignExt[track][k]; break;          // cancel exactly where tracks overlap
            case Flavour::Ghost:   mid = side = amt * m.malignStd[track][k]; break;          // + processor outputs the interaction layer
            case Flavour::Phantom: mid = amt * m.malignStd[track][k];                        // hollow mid + side pushed 90 deg (mono-safe)
                                   side = mid + amt * (M_PI / 2) * bandW(f, 200, 4000); break;
            case Flavour::Unmask:  mid = side = amt * m.alignExt[track][k] * bandW(f, 150, 4400); break; // clarity, low end untouched
            case Flavour::Envelop: mid = -amt * 0.0;                                         // mid untouched, side rotated below 300 Hz
                                   side = amt * (M_PI / 2) * (1.0 - bandW(f, 300, 20000)) * (f > 20 ? 1.0 : 0.0); break;
            case Flavour::Curve: { const double bend = std::sin(M_PI * std::log2(std::max(f, 1.0) / 60.0) / 3.0);
                                   mid = side = m.alignStd[track][k] + amt * (M_PI / 2) * bend * (f > 30 && f < 4400 ? 1.0 : 0.0); break; }
            case Flavour::Disperse: { const double oct = std::log2(std::max(f, 20.0) / 20.0) * 3.0; // third-octave index
                                   const int b = std::clamp((int)oct, 0, 38); const double fr = oct - b;
                                   const double v = scatter[b] * (1 - fr) + scatter[b + 1] * fr;
                                   mid = side = amt * M_PI * v * (f > 20 && f < 4400 ? 1.0 : 0.0); break; }
        }
        pc.mid[k] = (float)std::remainder(mid, 2 * M_PI);
        pc.side[k] = (float)std::remainder(side, 2 * M_PI);
    }
    return pc;
}

// ---------------------------------------------------------------- match EQ (level and tone neutral)
// gain in dB per solve bin that undoes the energy change alignment causes in the summed mix.
inline std::vector<float> matchGainDb(const CrossSpectra& cs, const std::vector<std::vector<float>>& midPhase) {
    const int T = cs.T, K = cs.K;
    std::vector<float> db(K, 0.f);
    if (T < 2 || cs.frames == 0) return db;
    std::vector<double> raw(K, 0.0);
    for (int k = 1; k < K; ++k) {
        double e0 = 0, e1 = 0;
        for (int i = 0; i < T; ++i)
            for (int j = 0; j < T; ++j) {
                const cplx c = cs.at(i, j, k);
                e0 += c.real();
                e1 += (c * std::polar(1.0, (double)midPhase[i][k] - (double)midPhase[j][k])).real();
            }
        raw[k] = (e0 > 1e-20 && e1 > 1e-20) ? 10.0 * std::log10(e0 / e1) : 0.0;
    }
    // smoothing: +-1 Hz below 100 Hz, 1/12 octave above
    for (int k = 1; k < K; ++k) {
        const double f = k * cs.binHz;
        const double w = f < 100 ? 1.0 : f * (std::pow(2.0, 1.0 / 24.0) - 1.0);
        const int h = std::max(0, (int)std::round(w / cs.binHz));
        double s = 0; int n = 0;
        for (int q = std::max(1, k - h); q <= std::min(K - 1, k + h); ++q) { s += raw[q]; ++n; }
        db[k] = (float)std::clamp(n ? s / n : 0.0, -6.0, 6.0);
    }
    return db;
}

// ---------------------------------------------------------------- FIR from a phase curve (+ gain)
// phase/gainDb are at solve resolution (binHz). Output: kIRLen taps, linear-phase centred at kIRLen/2.
inline std::vector<float> buildIR(const std::vector<float>& phase, const std::vector<float>& gainDb,
                                  double binHz, double sampleRate, double gainAmount = 1.0) {
    const int N = kIRLen;
    static thread_local FFT fft(N);
    std::vector<cplx> H(N, cplx(0, 0));
    const double irBin = sampleRate / N;
    const int K = (int)phase.size();
    auto sample = [&](const std::vector<float>& v, double f, bool wrapPhase) -> double {
        if (v.empty()) return 0.0;
        const double pos = f / binHz;
        if (pos >= K - 1) return wrapPhase ? 0.0 : 0.0;
        const int k0 = (int)pos; const double fr = pos - k0;
        if (!wrapPhase) return v[k0] * (1 - fr) + v[k0 + 1] * fr;
        const cplx a = std::polar(1.0, (double)v[k0]), b = std::polar(1.0, (double)v[k0 + 1]);
        return std::arg(a * (1 - fr) + b * fr);
    };
    for (int k = 0; k <= N / 2; ++k) {
        const double f = k * irBin;
        const double ph = (k == 0 || k == N / 2) ? 0.0 : sample(phase, f, true);
        const double g = std::pow(10.0, gainAmount * sample(gainDb, f, false) / 20.0);
        // centre the impulse at N/2: multiply by e^{-j*pi*k} = (-1)^k
        const double sign = (k & 1) ? -1.0 : 1.0;
        H[k] = std::polar(g, ph) * sign;
        if (k > 0 && k < N / 2) H[N - k] = std::conj(H[k]);
    }
    fft.transform(H, true);
    std::vector<float> h(N);
    // Tukey window (10% tapers) keeps the edges clean after smoothing-limited group delay.
    const int taper = N / 10;
    for (int n = 0; n < N; ++n) {
        double w = 1.0;
        if (n < taper) w = 0.5 - 0.5 * std::cos(M_PI * n / taper);
        else if (n > N - 1 - taper) w = 0.5 - 0.5 * std::cos(M_PI * (N - 1 - n) / taper);
        h[n] = (float)(H[n].real() / N * w);
    }
    return h;
}

} // namespace am
