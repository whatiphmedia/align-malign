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
    double fLo = 2.7;        // below this: untouched (DC cannot be rotated)
    double fHi = 1000.0;     // full strength up to here
    double fTaper = 1500.0;  // fades to zero by here (log-frequency taper)
    double pull = 0.5;       // how strongly each track prefers to stay as recorded (x its own energy): a track only
                             // rotates where the other tracks' shared sound outweighs this, and only as far as it pays
    double smoothOct = 1.0 / 6.0; // cross-spectra are averaged over this bandwidth before solving (statistics + continuity)
    double maxGdMs = 10.0;   // the correction may never move any frequency by more than this in time
    bool anti = false;       // false = align (max energy), true = malign (min energy)
    bool guard = false;      // align only: wherever the final correction would not add to the mix, fade it back to 0
};

// Session-wide analysis settings (Pro page > Alignment), shared by every copy: one Learn, one solve.
struct SolveSettings {
    double loHz = 2.7, hiHz = 1000.0;  // band the alignment works in
    int smoothIdx = 2;                 // 0..4 = 1/24, 1/12, 1/6, 1/3, 1/2 octave
    double stiffness = 0.5;            // 0..1; 0.5 = the default pull
    bool guard = true;                 // "never make it worse"
    int anchorId = -1;                 // slot id of the anchor track; -1 = automatic
    bool operator==(const SolveSettings& o) const {
        return loHz == o.loHz && hiHz == o.hiHz && smoothIdx == o.smoothIdx && stiffness == o.stiffness && guard == o.guard && anchorId == o.anchorId;
    }
    double smoothOct() const { static const double v[] = { 1.0 / 24, 1.0 / 12, 1.0 / 6, 1.0 / 3, 1.0 / 2 }; return v[std::clamp(smoothIdx, 0, 4)]; }
};

// The four solve profiles for a set of session settings.
struct ProfileSet { Profile aS, mS, aE, mE; };
inline ProfileSet profilesFor(const SolveSettings& st) {
    const double k = std::clamp(st.stiffness, 0.0, 1.0) / 0.5;     // 0..2 x the default pull
    const double lo = std::clamp(st.loHz, 2.7, 200.0), hi = std::clamp(st.hiHz, std::max(lo * 2, 200.0), 4000.0);
    ProfileSet p;
    p.aS.fLo = lo; p.aS.fHi = hi; p.aS.fTaper = std::min(4400.0, hi * 1.5); p.aS.pull = 0.5 * k; p.aS.guard = st.guard;
    p.mS = p.aS; p.mS.anti = true; p.mS.guard = false; p.mS.pull = 0.15 * k;
    p.mS.fHi = std::min(4000.0, hi * 1.5); p.mS.fTaper = std::min(4400.0, p.mS.fHi * 1.47);
    p.aE = p.aS; p.aE.fHi = 4000; p.aE.fTaper = 4400; p.aE.pull = 0.3 * k; p.aE.maxGdMs = 15.0;
    p.mE = p.aE; p.mE.anti = true; p.mE.guard = false; p.mE.pull = 0.1 * k;
    for (Profile* q : { &p.aS, &p.mS, &p.aE, &p.mE }) q->smoothOct = st.smoothOct();
    return p;
}

inline Profile alignStandard() { return {}; }
inline Profile malignStandard() { Profile p; p.anti = true; p.fHi = 1500; p.fTaper = 2200; p.pull = 0.15; return p; }
inline Profile alignExtended() { Profile p; p.pull = 0.3; p.fHi = 4000; p.fTaper = 4400; p.maxGdMs = 15.0; return p; }
inline Profile malignExtended() { Profile p = alignExtended(); p.anti = true; p.pull = 0.1; return p; }

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

// Grouping: tracks recorded from one source (kick in + kick out, bass DI + amp) are solved as ONE variable, so the
// phase between them stays exactly as recorded and the group moves as a unit. varOf[t] is track t's variable
// (0..V-1); a variable's cross-spectrum with another is the sum over every member pair.
inline CrossSpectra groupSpectra(const CrossSpectra& cs, const std::vector<int>& varOf, int V) {
    CrossSpectra r;
    r.T = V; r.K = cs.K; r.sr = cs.sr; r.binHz = cs.binHz; r.frames = cs.frames;
    r.C.assign((size_t)V * V * cs.K, cplx(0, 0));
    for (int i = 0; i < cs.T; ++i)
        for (int j = 0; j < cs.T; ++j) {
            const int a = varOf[(size_t)i], b = varOf[(size_t)j];
            if (a > b) continue;                     // stored as conj of (b, a)
            cplx* c = &r.C[((size_t)a * V + b) * cs.K];
            for (int k = 0; k < cs.K; ++k) c[k] += cs.at(i, j, k);
        }
    return r;
}

// Returns phase[t][k] (radians) for every track t and solve bin k (size cs.K).
//
// What makes this safe on real music (a whole song of unrelated parts, not test tones):
//  - Cross-spectra are averaged over a fraction of an octave first, and each pair only counts as far as the two
//    tracks are actually coherent there (above what chance gives for this much audio). Unrelated parts - a kick and
//    a synth bell - share nothing, and are left alone instead of being rotated to chase noise.
//  - Every track is pulled towards its recorded phase, so it only moves where that clearly pays.
//  - Bins are solved low to high, each starting from the previous bin's answer, so the result is continuous.
//  - The result is unwrapped, smoothed, and its slope (= time shift per frequency) is capped at maxGdMs.
inline std::vector<std::vector<float>> solve(const CrossSpectra& cs, int anchor, const Profile& p, int sweeps = 8) {
    const int T = cs.T, K = cs.K;
    std::vector<std::vector<float>> out(T, std::vector<float>(K, 0.f));
    if (T < 2 || cs.frames == 0 || K < 4) return out;

    // 1) fractional-octave smoothed cross-spectra (prefix sums per pair)
    const int P = T * (T + 1) / 2;
    auto pid = [T](int i, int j) { return i * T - i * (i - 1) / 2 + (j - i); };      // i <= j
    std::vector<std::vector<cplx>> pre(P, std::vector<cplx>(K + 1));
    for (int i = 0; i < T; ++i)
        for (int j = i; j < T; ++j) {
            auto& v = pre[pid(i, j)];
            for (int k = 0; k < K; ++k) v[k + 1] = v[k] + (k >= 1 ? cs.at(i, j, k) : cplx(0, 0));
        }
    const double ratio = std::pow(2.0, p.smoothOct / 2);
    auto win = [&](int k, int& lo, int& hi) {
        const double f = k * cs.binHz;
        const int h = std::max(2, (int)std::round((f * ratio - f) / cs.binHz));
        lo = std::max(1, k - h); hi = std::min(K - 1, k + h);
    };
    // chance coherence for this many (overlapping Hann) frames and bins
    const double framesEff = std::max(1.0, cs.frames / 1.9);

    std::vector<double> th(T, 0.0), own(T);
    std::vector<cplx> Cs((size_t)T * T);
    for (int k = 1; k < K; ++k) {
        const double f = k * cs.binHz;
        if (taperWeight(p, f) <= 0) { std::fill(th.begin(), th.end(), 0.0); continue; }
        int lo, hi; win(k, lo, hi);
        const double nEff = framesEff * (hi - lo + 1);
        const double g0 = std::min(0.5, 4.0 / nEff + 0.03);
        for (int i = 0; i < T; ++i) own[i] = (pre[pid(i, i)][hi + 1] - pre[pid(i, i)][lo]).real();
        for (int i = 0; i < T; ++i)
            for (int j = i + 1; j < T; ++j) {
                const cplx c = pre[pid(i, j)][hi + 1] - pre[pid(i, j)][lo];
                const double den = own[i] * own[j];
                const double g2 = den > 0 ? std::norm(c) / den : 0.0;
                const double w = std::clamp((g2 - g0) / (1.0 - g0), 0.0, 1.0);   // only real shared sound counts
                Cs[(size_t)i * T + j] = w * c;
                Cs[(size_t)j * T + i] = std::conj(w * c);
            }
        for (int s = 0; s < sweeps; ++s)                                         // warm start: th from bin k-1
            for (int i = 0; i < T; ++i) {
                if (own[i] <= 0 || i == anchor) { th[i] = 0; continue; }   // the anchor track is never changed
                cplx si(0, 0);
                for (int j = 0; j < T; ++j)
                    if (j != i) si += Cs[(size_t)i * T + j] * std::polar(1.0, -th[j]);
                const cplx z = p.anti ? cplx(p.pull * own[i], 0) - si : cplx(p.pull * own[i], 0) + si;
                th[i] = std::abs(z) > 0 ? -std::arg(z) : 0.0;
            }
        for (int t = 0; t < T; ++t) out[t][k] = (float)th[t];
    }

    // 2) unwrap, smooth (energy weighted, same bandwidth), cap the time shift, taper
    const double maxStep = 2 * M_PI * cs.binHz * p.maxGdMs * 1e-3;
    std::vector<double> u(K), sm(K);
    for (int t = 0; t < T; ++t) {
        u[0] = 0;
        for (int k = 1; k < K; ++k) u[k] = u[k - 1] + std::remainder((double)out[t][k] - (k > 1 ? out[t][k - 1] : 0.0), 2 * M_PI);
        std::vector<double> pw(K + 1, 0.0), pu(K + 1, 0.0);
        for (int k = 0; k < K; ++k) {
            const double w = (k >= 1 ? cs.at(t, t, k).real() : 0.0) + 1e-30;
            pw[k + 1] = pw[k] + w; pu[k + 1] = pu[k] + w * u[k];
        }
        for (int k = 0; k < K; ++k) { int lo, hi; win(std::max(1, k), lo, hi); sm[k] = (pu[hi + 1] - pu[lo]) / (pw[hi + 1] - pw[lo]); }
        sm[0] = 0;
        // start from zero at the bottom and follow the smoothed curve no faster than maxGdMs allows
        double cur = 0;
        for (int k = 1; k < K; ++k) {
            cur += std::clamp(sm[k] - cur, -maxStep, maxStep);
            out[t][k] = (float)(cur * taperWeight(p, k * cs.binHz));
        }
    }

    // 3) never make it worse (align): where the correction would clearly lose mix energy (more than 1% of the
    //    tracks' own energy there), scale it back by the amount that keeps the most gain. The choices are smoothed
    //    across frequency and kept within the time-shift cap in both directions. A single pass: small, harmless
    //    losses are left alone, so one marginal band can't shut down its neighbours.
    if (p.guard && !p.anti) {
        const std::vector<std::vector<float>> full = out;
        auto gainAt = [&](double sc, int k, double& own) {   // smoothed mix energy change at bin k with sc x correction
            int lo, hi; win(k, lo, hi);
            double e0 = 0, e1 = 0; own = 0;
            for (int i = 0; i < T; ++i) {
                own += (pre[pid(i, i)][hi + 1] - pre[pid(i, i)][lo]).real();
                for (int j = i + 1; j < T; ++j) {
                    const cplx c = pre[pid(i, j)][hi + 1] - pre[pid(i, j)][lo];
                    e0 += c.real();
                    e1 += (c * std::polar(1.0, sc * ((double)full[i][k] - full[j][k]))).real();
                }
            }
            return 2 * (e1 - e0);
        };
        std::vector<double> sc(K, 1.0);
        for (int k = 1; k < K; ++k) {
            double own = 0;
            if (gainAt(1.0, k, own) >= -0.01 * own) continue;          // fine, or only a negligible loss
            double best = 0, bestG = 0;
            for (double c : { 0.25, 0.5, 0.75 }) { double o; const double g = gainAt(c, k, o); if (g > bestG) { bestG = g; best = c; } }
            sc[k] = best;
        }
        auto build = [&](const std::vector<double>& scale) {
        std::vector<double> ps(K + 1, 0.0);
        for (int k = 0; k < K; ++k) ps[k + 1] = ps[k] + scale[k];
        for (int t = 0; t < T; ++t) {
            std::vector<double> tgt(K, 0.0);
            for (int k = 1; k < K; ++k) { int lo, hi; win(k, lo, hi); tgt[k] = full[t][k] * std::min(scale[k], (ps[hi + 1] - ps[lo]) / (hi - lo + 1)); }
            // follow the target within the time-shift cap, forwards then backwards (so fades start before a dip)
            double cur = 0;
            for (int k = 1; k < K; ++k) { cur += std::clamp(tgt[k] - cur, -maxStep, maxStep); tgt[k] = cur; }
            for (int k = K - 2; k >= 1; --k) tgt[k] = std::clamp(tgt[k], tgt[k + 1] - maxStep, tgt[k + 1] + maxStep);
            for (int k = 1; k < K; ++k) out[t][k] = (float)tgt[k];
        }
        };
        build(sc);
        // check pass: anything that still clearly loses (the smoothing or the cap can cause it) goes back to as
        // recorded, and the result is rebuilt once
        auto lossAt = [&](int k) { int lo, hi; win(k, lo, hi); double e0 = 0, e1 = 0, own = 0;
            for (int i = 0; i < T; ++i) { own += (pre[pid(i, i)][hi + 1] - pre[pid(i, i)][lo]).real();
                for (int j = i + 1; j < T; ++j) { const cplx c = pre[pid(i, j)][hi + 1] - pre[pid(i, j)][lo];
                    e0 += c.real(); e1 += (c * std::polar(1.0, (double)out[i][k] - out[j][k])).real(); } }
            return 2 * (e1 - e0) < -0.01 * own; };
        bool again = false;
        for (int k = 1; k < K; ++k) if (lossAt(k)) { sc[k] = 0; again = true; }
        if (again) build(sc);
    }
    return out;
}

// ---------------------------------------------------------------- user controls -> phase curves
enum class Flavour { Fuse, Erase, Ghost, Phantom, Unmask, Envelop, Curve, Disperse };

inline Flavour flavourFromString(const std::string& s) {
    static const char* names[] = {"FUSE", "ERASE", "GHOST", "MALODICS", "UNMASK", "ENVELOP", "CURVE", "DISPERSE"};
    for (int i = 0; i < 8; ++i) if (s == names[i]) return (Flavour)i;
    return Flavour::Fuse;
}

struct Maps { // solved per-track phase maps for one session solve
    std::vector<std::vector<float>> alignStd, malignStd, alignExt, malignExt;
    double binHz = 48000.0 / kSolveN;
    int K = 0;
};

struct PhaseCurves { std::vector<float> mid, side; }; // per solve bin

struct Controls {
    double knob = 0.0;     // -1 = full align, 0 = neutral (as recorded), +1 = full malign
    Flavour flavour = Flavour::Fuse;
    uint32_t seed = 1;     // per-track seed for Disperse
    double flavourAmt = 1.0;   // depth of the malign flavours (Pro > Flavour > Amount)
    double monoBelowHz = 0.0;  // below this the side keeps the mid's phase (0 = off)
    double sideRotRad = 0.0;   // extra rotation of the side above the mono point
};

// Stereo controls on top of a phase pair: mono-safe low end, then a side rotation above it.
inline void applyStereo(PhaseCurves& pc, double binHz, const Controls& c) {
    if (c.monoBelowHz <= 0 && c.sideRotRad == 0) return;
    for (size_t k = 0; k < pc.side.size(); ++k) {
        const double f = k * binHz;
        // 0 below the mono point, 1 a third of an octave above it
        const double w = c.monoBelowHz <= 0 ? 1.0 : std::clamp(std::log2(std::max(f, 1e-3) / c.monoBelowHz) * 3.0, 0.0, 1.0);
        const double side = pc.mid[k] + w * (std::remainder((double)pc.side[k] - pc.mid[k], 2 * M_PI) + c.sideRotRad);
        pc.side[k] = (float)std::remainder(side, 2 * M_PI);
    }
}


// Align side always runs the standard alignment. The flavour decides what the malign side does.
inline PhaseCurves effectivePhase(const Maps& m, int track, const Controls& c) {
    PhaseCurves pc;
    pc.mid.assign(m.K, 0.f);
    pc.side.assign(m.K, 0.f);
    if (m.K == 0 || track < 0 || track >= (int)m.alignStd.size()) return pc;
    const double a = std::clamp(c.knob, -1.0, 1.0);
    if (a <= 0) { // align
        for (int k = 0; k < m.K; ++k) pc.mid[k] = pc.side[k] = (float)(-a * m.alignStd[track][k]);
        applyStereo(pc, m.binHz, c);
        return pc;
    }
    const double amt = a * std::clamp(c.flavourAmt, 0.0, 1.0);
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
    applyStereo(pc, m.binHz, c);
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
