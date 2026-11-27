// Auto Malign Studio: native effects (offline, lightweight). All "learned" effects take the phrases/gaps of a track.
#pragma once
#include "learn.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

namespace am::studio {

// ---------------------------------------------------------------- timing: a tempo division or milliseconds
struct Timing {
    bool tempo = true; double div = 8; bool triplet = false, dotted = false; double ms = 100;   // div: 4 = 1/4, 8 = 1/8 ...
    size_t samples(double bpm, int sr) const {
        if (!tempo) return (size_t)std::max(1.0, ms * 1e-3 * sr);
        double beats = 4.0 / div; if (triplet) beats *= 2.0 / 3.0; if (dotted) beats *= 1.5;
        return (size_t)std::max(1.0, beats * 60.0 / bpm * sr);
    }
};
template <class T> struct Sweep { T start{}, end{}; bool on = false; T at(double x) const { return (T)(start + (end - start) * x); } };

// ---------------------------------------------------------------- reverb (generated stereo impulse response)
struct ReverbSettings { double decayS = 2.7, lowCutHz = 400, highCutHz = 9000, width = 0.8, predelayMs = 20; uint32_t seed = 11; };
struct Reverb {
    std::vector<float> irL, irR;
    void build(const ReverbSettings& s, int sr) {
        const size_t L = (size_t)(std::max(0.2, s.decayS * 1.2) * sr), pd = (size_t)(s.predelayMs * 1e-3 * sr);
        std::mt19937 rng(s.seed); std::normal_distribution<double> g(0, 1);
        irL.assign(L + pd, 0.f); irR.assign(L + pd, 0.f);
        const double c = std::clamp(1.0 - 2.0 * s.width, -0.95, 0.95);   // width 0.5 = uncorrelated, 1 = anti-correlated
        for (size_t i = 0; i < L; ++i) {
            const double e = std::exp(-6.91 * i / (s.decayS * sr)), a = g(rng), b = g(rng);
            irL[pd + i] = (float)(a * e); irR[pd + i] = (float)((c * a + std::sqrt(1 - c * c) * b) * e);
        }
        highpass(irL, s.lowCutHz, sr); highpass(irR, s.lowCutHz, sr); lowpass(irL, s.highCutHz, sr); lowpass(irR, s.highCutHz, sr);
        double n = 0; for (size_t i = 0; i < irL.size(); ++i) n += (double)irL[i] * irL[i] + (double)irR[i] * irR[i];
        const float k = (float)(1.0 / std::sqrt(n / 2 + 1e-20)); for (auto& v : irL) v *= k; for (auto& v : irR) v *= k;
    }
    Stereo process(const Stereo& x) const {
        Stereo y(x.size());
        const auto ll = convolve(x.L, irL, x.size()), rr = convolve(x.R, irR, x.size()), lr = convolve(x.R, irL, x.size()), rl = convolve(x.L, irR, x.size());
        for (size_t i = 0; i < x.size(); ++i) { y.L[i] = ll[i] + 0.3f * lr[i]; y.R[i] = rr[i] + 0.3f * rl[i]; }
        return y;
    }
};

// ---------------------------------------------------------------- tempo delay (ping-pong, filtered feedback)
struct DelaySettings { Timing time{ true, 8, false, true }; double feedback = 0.45, lowCutHz = 250, highCutHz = 5000; bool pingPong = true; int repeats = 10; };
inline Stereo delayFx(const Stereo& x, const DelaySettings& s, double bpm, int sr) {
    const size_t d = s.time.samples(bpm, sr), n = x.size(); auto m = x.mono();
    highpass(m, s.lowCutHz, sr); lowpass(m, s.highCutHz, sr);
    Stereo y(n); double g = 1.0;
    for (int k = 1; k <= s.repeats; ++k) {
        const size_t off = d * k; if (off >= n) break;
        for (size_t i = off; i < n; ++i) { const float v = (float)(g * m[i - off]); if (!s.pingPong) { y.L[i] += v; y.R[i] += v; } else if (k % 2) y.L[i] += v * 1.41f; else y.R[i] += v * 1.41f; }
        g *= s.feedback;
    }
    return y;
}

// ---------------------------------------------------------------- smooth gate following "singing"
inline std::vector<float> singingGate(const TrackLearn& L, size_t n, double attS = 0.03, double relS = 0.25) {
    std::vector<float> g(n, 0.f); double y = 0; const double aa = std::exp(-1.0 / (attS * L.sr)), ar = std::exp(-1.0 / (relS * L.sr));
    for (size_t i = 0; i < n; ++i) { const size_t t = i / L.hopL; const double tg = (t < L.active.size() && L.active[t]) ? 1.0 : 0.0; y = tg + (y - tg) * (tg > y ? aa : ar); g[i] = (float)y; }
    return g;
}

// ---------------------------------------------------------------- freeze: hold the voice's spectrum (in its gaps, or as a held ghost)
struct FreezeSettings { bool gapsOnly = true; double hold = 0.7, level = 0.7, fadeS = 0; };
inline Stereo freezeFx(const Stereo& x, const TrackLearn& L, const FreezeSettings& s) {
    Stereo out(x.size()); std::mt19937 rng(3); std::uniform_real_distribution<double> ph(-M_PI, M_PI);
    for (int c = 0; c < 2; ++c) {
        auto S = stft(c ? x.R : x.L, 4096); std::vector<double> hold((size_t)S.bins(), 0.0);
        for (size_t t = 0; t < S.frames.size(); ++t) {
            const size_t samp = t * S.hop, lt = samp / L.hopL; const bool singing = lt < L.active.size() && L.active[lt];
            float inGap = 0.f; for (size_t gi = 0; gi < L.gaps.size(); ++gi) if (L.gaps[gi].contains(samp)) { inGap = L.weight(gi); break; }
            if (s.gapsOnly) {
                if (singing || inGap == 0.f) { for (size_t k = 0; k < hold.size(); ++k) hold[k] = s.hold * hold[k] + (1 - s.hold) * std::abs(S.frames[t][k]); for (auto& v : S.frames[t]) v = 0; }
                else { for (size_t k = 0; k < hold.size(); ++k) { S.frames[t][k] = std::polar(hold[k] * inGap, ph(rng)); hold[k] *= 0.995; } }
            } else {
                for (size_t k = 0; k < hold.size(); ++k) { hold[k] = 0.96 * hold[k] + 0.04 * std::abs(S.frames[t][k]); S.frames[t][k] = std::polar(hold[k], ph(rng)); }
            }
        }
        auto y = istft(S); for (auto& v : y) v *= (float)s.level; (c ? out.R : out.L) = y;
    }
    return out;
}

// ---------------------------------------------------------------- shimmer: pitch shift with formant, tilt, breath; optional inside-out
struct ShimmerSettings { double semitones = 12, formant = -2, tiltDbOct = 0, breath = 0.3; bool insideOut = true; double level = 0.7; };
inline std::vector<float> pitchShiftFormant(const std::vector<float>& x, const ShimmerSettings& s, int sr) {
    auto S = stft(x, 4096); const size_t K = (size_t)S.bins(), Tn = S.frames.size(); const int N = S.N;
    const double r = std::pow(2.0, s.semitones / 12), q = std::pow(2.0, s.formant / 12);
    const auto& F = fftOf(N); std::mt19937 rng(1); std::uniform_real_distribution<double> uph(-M_PI, M_PI);
    std::vector<double> prevPh(K, 0.0), acc(K, 0.0), M(K), E(K), Fi(K); std::vector<cplx> cep(N);
    auto warp = [&](const std::vector<double>& A, double ratio, double k) { const double src = k / ratio; const size_t lo = (size_t)src; if (lo + 1 >= K) return 0.0; const double f = src - lo; return (1 - f) * A[lo] + f * A[lo + 1]; };
    double eIn = 0, eOut = 0;
    for (size_t t = 0; t < Tn; ++t) {
        auto& fr = S.frames[t]; std::vector<double> ph(K), dph(K);
        for (size_t k = 0; k < K; ++k) { M[k] = std::abs(fr[k]); ph[k] = std::arg(fr[k]); double d = ph[k] - prevPh[k]; d -= 2 * M_PI * std::round(d / (2 * M_PI)); dph[k] = d; prevPh[k] = ph[k]; eIn += M[k] * M[k]; }
        // spectral envelope by cepstral smoothing (lifter 30)
        for (int i = 0; i < N; ++i) cep[(size_t)i] = std::log(M[(size_t)(i <= N / 2 ? i : N - i)] + 1e-9);
        F.transform(cep, true); for (int i = 30; i <= N - 30; ++i) cep[(size_t)i] = 0; F.transform(cep);
        for (size_t k = 0; k < K; ++k) { E[k] = std::exp(cep[k].real() / N); Fi[k] = std::min(M[k] / (E[k] + 1e-12), 3.0); }
        for (size_t k = 0; k < K; ++k) {
            const double fk = (double)k * sr / N;
            const double tilt = std::pow(std::max(fk, 50.0) / 1000.0, s.tiltDbOct / 6.02);
            const double mag = warp(Fi, r, (double)k) * warp(E, q, (double)k) * tilt;
            const double src = k / r; const size_t lo = (size_t)src; acc[k] += (lo + 1 < K ? dph[lo] : 0.0) * r;
            cplx v = std::polar(mag, acc[k]);
            if (s.breath > 0) v = (1 - s.breath) * v + s.breath * std::polar(warp(E, q, (double)k) * tilt, uph(rng));
            fr[k] = v; eOut += std::norm(v);
        }
    }
    auto y = istft(S); const float g = (float)std::sqrt(eIn / (eOut + 1e-20)); for (auto& v : y) v *= g; return y;
}
inline Stereo shimmerFx(const Stereo& x, const ShimmerSettings& s, int sr) {
    const auto y = pitchShiftFormant(x.mono(), s, sr); Stereo o(x.size());
    for (size_t i = 0; i < o.size() && i < y.size(); ++i) { const float v = (float)(s.level * y[i]); if (s.insideOut) { o.L[i] = 0.5f * v; o.R[i] = -0.5f * v; } else { o.L[i] = v; o.R[i] = v; } }
    return o;
}

// ---------------------------------------------------------------- roll / stutter with per-repeat modulation (Start -> End across the roll)
struct RollMods {
    Sweep<double> pitch{ 0, 12 }, filterHz{ 400, 16000 }, drive{ 0, 0.4 }, levelDb{ -18, 0 }, gate{ 1.0, 0.3 }, crush{ 0, 1 }, stereo{ 0, 1 };
    bool reverse = false;
};
struct RollSettings {
    enum Mode { Post, Pre, Both } mode = Pre;
    Timing start{ true, 8 }, end{ true, 64 }, preStart{ false, 0, false, false, 30 }, preEnd{ true, 8 };
    double leadInMs = 0, minGapS = 0.12, level = 1.0;
    RollMods mods;
};
namespace detail {
inline void modulate(std::vector<float>& L, std::vector<float>& R, double x, int k, const RollMods& M, int sr) {
    const size_t n = L.size();
    if (M.pitch.on) {
        const double r = std::pow(2.0, M.pitch.at(x) / 12); std::vector<float> a(n), b(n);
        for (size_t i = 0; i < n; ++i) { a[i] = sampleAt(L, i * r); b[i] = sampleAt(R, i * r); } L.swap(a); R.swap(b);
    }
    if (M.reverse && k % 2) { std::reverse(L.begin(), L.end()); std::reverse(R.begin(), R.end()); }
    if (M.filterHz.on) { const double fc = std::exp(std::log(M.filterHz.start) + (std::log(M.filterHz.end) - std::log(M.filterHz.start)) * x); lowpass(L, fc, sr); lowpass(R, fc, sr); lowpass(L, fc, sr); lowpass(R, fc, sr); }
    if (M.drive.on) {
        const double d = M.drive.at(x);
        if (d > 0.001) { float pk = 1e-9f; for (size_t i = 0; i < n; ++i) pk = std::max({ pk, std::abs(L[i]), std::abs(R[i]) });
            const double kd = 1 + 19 * d, norm = pk / std::tanh(kd) * (1 - 0.3 * d);
            for (size_t i = 0; i < n; ++i) { L[i] = (float)(std::tanh(kd * L[i] / pk) * norm); R[i] = (float)(std::tanh(kd * R[i] / pk) * norm); } }
    }
    if (M.crush.on) {
        const double c = M.crush.at(x);
        if (c > 0.01) { const size_t hold = std::max<size_t>(1, (size_t)std::lround(1 + 7 * c)); const double q = std::pow(2.0, 15 - 10 * c);
            float pk = 1e-9f; for (size_t i = 0; i < n; ++i) pk = std::max({ pk, std::abs(L[i]), std::abs(R[i]) });
            for (size_t i = 0; i < n; ++i) { const size_t j = i / hold * hold; L[i] = (float)(std::round(L[j] / pk * q) / q * pk); R[i] = (float)(std::round(R[j] / pk * q) / q * pk); } }
    }
    if (M.gate.on) { const size_t m = (size_t)(n * std::clamp(M.gate.at(x), 0.05, 1.0)); const double a = std::exp(-1.0 / (0.002 * sr)); double g = 0;
        for (size_t i = 0; i < n; ++i) { g = a * g + (1 - a) * (i < m ? 1.0 : 0.0); L[i] *= (float)g; R[i] *= (float)g; } }
    if (M.levelDb.on) { const float g = (float)dbToGain(M.levelDb.at(x)); for (size_t i = 0; i < n; ++i) { L[i] *= g; R[i] *= g; } }
    if (M.stereo.on) { const double w = M.stereo.at(x); const float sgn = k % 2 ? 1.f : -1.f;
        for (size_t i = 0; i < n; ++i) { const float m = 0.5f * (L[i] + R[i]), s = (float)(sgn * w) * m; L[i] = m + s; R[i] = m - s; } }
}
} // namespace detail
inline Stereo rollFx(const Stereo& x, const TrackLearn& Lr, const RollSettings& s, double bpm, int sr) {
    Stereo out(x.size()); const size_t n = x.size(), xf = (size_t)(0.003 * sr), lead = (size_t)(s.leadInMs * 1e-3 * sr);
    auto lensFor = [&](const Timing& a, const Timing& b, size_t span) {
        std::vector<size_t> v; const double La = (double)a.samples(bpm, sr), Lb = (double)b.samples(bpm, sr); size_t pos = 0;
        while (pos < span) { const size_t L = std::max<size_t>((size_t)(La * std::pow(Lb / La, (double)pos / span)), (size_t)(0.005 * sr)); v.push_back(L); pos += L; }
        return v;
    };
    double wgt = 1.0;
    auto put = [&](const Stereo& src, size_t from, size_t L, size_t at, double xpos, int k) {
        std::vector<float> l(L, 0.f), r(L, 0.f);
        for (size_t i = 0; i < L && from + i < src.size(); ++i) { l[i] = src.L[from + i]; r[i] = src.R[from + i]; }
        detail::modulate(l, r, xpos, k, s.mods, sr); const auto w = raisedFade(L, xf);
        for (size_t i = 0; i < L && at + i < n; ++i) { out.L[at + i] += (float)(w[i] * l[i] * s.level * wgt); out.R[at + i] += (float)(w[i] * r[i] * s.level * wgt); }
    };
    for (size_t gi = 0; gi < Lr.gaps.size(); ++gi) {
        const auto gap = Lr.gaps[gi]; wgt = Lr.weight(gi);
        if (gap.len() < (size_t)(s.minGapS * sr) || gap.b >= x.size() && s.mode == RollSettings::Pre) continue;
        const bool hasBefore = gap.a > 0;   // the lead-in before the first phrase has nothing to repeat after it
        const size_t cut = gap.a > lead ? gap.a - lead : 0, ret = gap.b;
        std::vector<std::pair<size_t, size_t>> parts;   // [a, b) for post, then pre
        if (s.mode == RollSettings::Post && hasBefore) parts = { { cut, ret } };
        else if (s.mode == RollSettings::Pre) parts = { { cut, ret } };
        else if (s.mode == RollSettings::Both) { const size_t mid = cut + (ret - cut) / 2; parts = { { cut, mid }, { mid, ret } }; if (!hasBefore) parts = { { cut, ret } }; }
        for (size_t pi = 0; pi < parts.size(); ++pi) {
            const bool post = (s.mode == RollSettings::Post) || (s.mode == RollSettings::Both && pi == 0 && hasBefore);
            const auto [a, b] = parts[pi]; if (b <= a) continue;
            const auto lens = (s.mode == RollSettings::Both && !post) ? lensFor(s.preStart, s.preEnd, b - a) : lensFor(s.start, s.end, b - a);
            const size_t mx = *std::max_element(lens.begin(), lens.end());
            if (post) {
                const size_t endSrc = cut > (size_t)(0.02 * sr) ? cut - (size_t)(0.02 * sr) : 0, from = endSrc > mx ? endSrc - mx : 0;
                size_t pos = a; int k = 0;
                for (auto L : lens) { if (pos >= b) break; put(x, from, L, pos, lens.size() > 1 ? (double)k / (lens.size() - 1) : 1.0, k); pos += L; ++k; }
            } else {
                std::vector<std::pair<size_t, size_t>> seq; size_t pos = b;
                for (auto it = lens.rbegin(); it != lens.rend(); ++it) { if (pos < a + *it) break; pos -= *it; seq.push_back({ pos, *it }); }
                std::reverse(seq.begin(), seq.end());
                for (size_t k = 0; k < seq.size(); ++k) put(x, ret, seq[k].second, seq[k].first, seq.size() > 1 ? (double)k / (seq.size() - 1) : 1.0, (int)k);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------- reverse reverb: lands exactly on each entrance
struct ReverseReverbSettings { double sourceS = 0.35, maxPreS = 1.6, level = 0.35, decayS = 1.8, landMs = -10; bool capToGap = false; };   // landMs < 0: the swell ends that much before the voice
inline Stereo reverseReverbFx(const Stereo& x, const TrackLearn& L, const ReverseReverbSettings& s, int sr) {
    Reverb rv; ReverbSettings rs; rs.decayS = s.decayS; rs.lowCutHz = 300; rs.highCutHz = 12000; rs.predelayMs = 0; rs.seed = 21; rv.build(rs, sr);
    Stereo out(x.size()); const size_t Ls = (size_t)(s.sourceS * sr);
    for (size_t gi = 0; gi < L.gaps.size(); ++gi) {
        const auto gap = L.gaps[gi]; const float wgt = L.weight(gi);
        if (gap.b >= x.size()) continue;
        const long landAt = (long)gap.b + (long)(s.landMs * 1e-3 * sr); if (landAt <= 0) continue; const size_t land = (size_t)landAt, ret = gap.b;
        Stereo chunk(std::min(Ls, x.size() - ret));
        for (size_t i = 0; i < chunk.size(); ++i) { const double w = 0.5 + 0.5 * std::cos(M_PI * (double)i / chunk.size()); chunk.L[i] = (float)(x.L[ret + i] * w); chunk.R[i] = (float)(x.R[ret + i] * w); }
        Stereo padded(chunk.size() + rv.irL.size()); padded.add(chunk);
        auto wet = rv.process(padded);
        size_t pre = std::min({ wet.size(), (size_t)(s.maxPreS * sr), s.capToGap ? gap.len() : land });
        const size_t f = std::min<size_t>((size_t)(0.2 * sr), pre);
        for (size_t j = 0; j < pre; ++j) {   // reversed: the end of the reversed tail lands on the entrance
            const size_t src = pre - 1 - j; double fade = 1.0; if (j < f) fade = std::pow((double)j / f, 2.0);
            out.L[land - pre + j] += (float)(wet.L[src] * fade * wgt); out.R[land - pre + j] += (float)(wet.R[src] * fade * wgt);
        }
    }
    const double g = std::sqrt(x.energy() / (out.energy() + 1e-20)) * s.level; out.scale((float)g); return out;
}

// ---------------------------------------------------------------- automatic delay throws on phrase ends (they live in the gap)
// Each throw is gated by the entrance it runs into: its echoes fade out on an S-curve that ends where the next phrase
// starts, while a low-pass closes over the same stretch, so the tail tucks under the entrance instead of being cut.
struct ThrowSettings {
    DelaySettings delay{ { true, 8, false, true }, 0.5, 250, 5000, true, 10 };
    double lastWordS = 0.28, level = 0.9;
    enum Falloff { Gate, Duck } falloff = Gate; double fadeS = 0.15, duckDb = -12; bool darken = true;
};
inline Stereo throwsFx(const Stereo& x, const TrackLearn& L, const ThrowSettings& s, double bpm, int sr) {
    const size_t n = x.size(); Stereo out(n);
    for (size_t gi = 0; gi < L.gaps.size(); ++gi) {
        const auto gap = L.gaps[gi]; if (gap.a == 0) continue;   // nothing to throw before the first line
        const double wgt = L.weight(gi); const size_t c = gap.a, wl = (size_t)(s.lastWordS * sr), a = c > wl ? c - wl : 0; if (c <= a) continue;
        const size_t next = std::min(n, gap.b);
        const size_t fade = std::min((size_t)(s.fadeS * sr), next > c ? next - c : 0), f0 = next - fade;
        const size_t until = s.falloff == ThrowSettings::Gate ? std::min(n, next + (size_t)(0.002 * sr)) : n;
        if (until <= a) continue;
        Stereo send(until - a); const auto w = raisedFade(c - a, (size_t)(0.01 * sr));
        for (size_t i = a; i < c; ++i) { send.L[i - a] = (float)(x.L[i] * w[i - a]); send.R[i - a] = (float)(x.R[i] * w[i - a]); }
        auto y = delayFx(send, s.delay, bpm, sr);
        for (size_t i = 0; i < y.size(); ++i) {
            const size_t at = a + i; double g = s.level * wgt;
            if (at >= f0) {
                const double u = fade ? std::min(1.0, (double)(at - f0) / fade) : 1.0, sc = u * u * (3 - 2 * u);   // S-curve
                g *= s.falloff == ThrowSettings::Gate ? 1.0 - sc : 1.0 + (dbToGain(s.duckDb) - 1.0) * sc;
            }
            y.L[i] *= (float)g; y.R[i] *= (float)g;
        }
        if (s.darken && fade > 16 && f0 > a) {   // close a low-pass over the fade: the tail goes dull as it goes quiet
            Biquad ql, qr; const size_t i0 = f0 - a;
            for (size_t i = i0; i < y.size(); ++i) {
                if ((i - i0) % 32 == 0) { const double u = std::min(1.0, (double)(i - i0) / fade); const double fc = 8000 * std::pow(1500.0 / 8000.0, u);
                    auto nl = Biquad::make(0, fc, sr); nl.x1 = ql.x1; nl.x2 = ql.x2; nl.y1 = ql.y1; nl.y2 = ql.y2; ql = nl; auto nr = Biquad::make(0, fc, sr); nr.x1 = qr.x1; nr.x2 = qr.x2; nr.y1 = qr.y1; nr.y2 = qr.y2; qr = nr; }
                y.L[i] = ql.run(y.L[i]); y.R[i] = qr.run(y.R[i]);
            }
        }
        out.add(y, 1.f, a);
    }
    out.resize(n); return out;
}

// ---------------------------------------------------------------- phase pocket: while the voice sings, the beat's centre moves to the sides,
// but only at the frequencies the voice is using at that moment, following it with a slow attack and release (no snapping)
struct PocketSettings { double amount = 0.6, loHz = 150, hiHz = 8000, rangeDb = 30, attackMs = 80, releaseMs = 350; };
inline Stereo pocketFx(const Stereo& beat, const Stereo& voice, const PocketSettings& s, int sr) {
    std::vector<float> mid(beat.size()), side(beat.size());
    for (size_t i = 0; i < beat.size(); ++i) { mid[i] = 0.5f * (beat.L[i] + beat.R[i]); side[i] = 0.5f * (beat.L[i] - beat.R[i]); }
    auto M = stft(mid), S = stft(side); const auto V = stft(voice.mono()); const int N = M.N; const size_t K = (size_t)M.bins();
    const double fa = std::exp(-M.hop / (s.attackMs * 1e-3 * sr)), fr = std::exp(-M.hop / (s.releaseMs * 1e-3 * sr));
    // the voice's energy per 1/3 octave, per frame; the mask follows where it is within rangeDb of its loudest band
    std::vector<std::vector<float>> vb(M.frames.size(), std::vector<float>(K)); std::vector<double> c(K + 1); double vocalTop = 1e-30;
    for (size_t t = 0; t < M.frames.size(); ++t) {
        const auto& vf = V.frames[std::min(t, V.frames.size() - 1)];
        c[0] = 0; for (size_t k = 0; k < K; ++k) c[k + 1] = c[k] + std::norm(vf[k]);
        for (size_t k = 0; k < K; ++k) { const size_t w = std::max<size_t>(1, (size_t)(k * 0.12)); const size_t lo = k > w ? k - w : 0, hi = std::min(K, k + w + 1);
            vb[t][k] = (float)((c[hi] - c[lo]) / (hi - lo)); vocalTop = std::max(vocalTop, (double)vb[t][k]); }
    }
    std::vector<double> mask(K, 0.0);
    for (size_t t = 0; t < M.frames.size(); ++t) {
        double frameTop = 1e-30; for (size_t k = 0; k < K; ++k) frameTop = std::max(frameTop, (double)vb[t][k]);
        const double present = std::clamp((10 * std::log10(frameTop / vocalTop) + s.rangeDb) / (s.rangeDb / 3), 0.0, 1.0);   // the voice is there now (full within 2/3 of the range of its loudest)
        for (size_t k = 0; k < K; ++k) {
            const double db = 10 * std::log10(vb[t][k] / frameTop + 1e-30);   // where it is now, relative to its loudest band at this moment

            const double f = (double)k * sr / N; const double band = std::clamp(std::min(std::log2(std::max(f, 1.0) / s.loHz), std::log2(s.hiHz / std::max(f, 1.0))) / 0.5 + 0.5, 0.0, 1.0);
            const double target = band * present * std::clamp((db + s.rangeDb) / (s.rangeDb * 0.5), 0.0, 1.0);
            mask[k] = target + (mask[k] - target) * (target > mask[k] ? fa : fr);
            const double th = s.amount * (M_PI / 2) * 0.8 * mask[k]; const cplx m = M.frames[t][k];
            M.frames[t][k] = m * std::cos(th); S.frames[t][k] += m * std::sin(th) * cplx(0, 1);
        }
    }
    const auto m2 = istft(M), s2 = istft(S); Stereo o(beat.size());
    for (size_t i = 0; i < o.size(); ++i) { o.L[i] = m2[i] + s2[i]; o.R[i] = m2[i] - s2[i]; }
    return o;
}

// ---------------------------------------------------------------- call and response: the last word answers itself in the next gap, from the other side
struct ResponseSettings {
    enum Kind { Stutter, Shimmer, Freeze } kind = Stutter; double level = 0.7, spread = 0.8;
    double attackMs = 25, releaseS = 0.15;   // S-curve fades
    double bleed = 0;                        // 0 = tight to the gap (gone as the voice returns) .. 1 = carries on 1 s over the next phrase
    double bleedDuckDb = -6;                 // how far it tucks under the voice while bleeding over it
};
inline Stereo responseFx(const Stereo& x, const TrackLearn& L, const ResponseSettings& s, double bpm, int sr) {
    Stereo out(x.size()); int idx = 0;
    for (size_t gi = 0; gi < L.gaps.size(); ++gi) {
        const auto gap = L.gaps[gi]; const double wgt = L.weight(gi);
        if (gap.a == 0 || gap.len() < (size_t)(0.12 * sr)) continue;
        const size_t c = gap.a, r = std::min(gap.b, x.size()), extra = (size_t)(std::clamp(s.bleed, 0.0, 1.0) * sr), span = std::min(x.size() - c, r - c + extra);
        const size_t wl = (size_t)(0.25 * sr), a = c > wl ? c - wl : 0;
        Stereo word(c - a); for (size_t i = 0; i < word.size(); ++i) { word.L[i] = x.L[a + i]; word.R[i] = x.R[a + i]; }
        Stereo resp(span);
        if (s.kind == ResponseSettings::Stutter) {
            size_t Lsl = std::max<size_t>((size_t)(0.03 * sr), (size_t)(60.0 / bpm / 8 * sr)); size_t pos = 0; double g = 1.0;
            while (pos < span && Lsl > 16) { const size_t from = word.size() > Lsl ? word.size() - Lsl : 0; const auto w = raisedFade(Lsl, (size_t)(0.003 * sr));
                for (size_t i = 0; i < Lsl && pos + i < span && from + i < word.size(); ++i) { resp.L[pos + i] += (float)(word.L[from + i] * w[i] * g); resp.R[pos + i] += (float)(word.R[from + i] * w[i] * g); }
                pos += Lsl; g *= 0.85; Lsl = std::max<size_t>((size_t)(0.03 * sr), (size_t)(Lsl * 0.8)); }
        } else if (s.kind == ResponseSettings::Shimmer) {
            Stereo pad(word.size() + span); pad.add(word); ShimmerSettings sh; sh.insideOut = false; sh.level = 1.5;
            auto y = shimmerFx(pad, sh, sr); for (size_t i = 0; i < span && i < y.size(); ++i) { resp.L[i] = y.L[i]; resp.R[i] = y.R[i]; }
        } else {
            TrackLearn none; none.sr = sr; none.hopL = L.hopL; FreezeSettings fs; fs.gapsOnly = false; fs.level = 1.0;
            Stereo pad(std::min(span, (size_t)(1.2 * sr) + extra)); pad.add(word);
            auto y = freezeFx(pad, none, fs); for (size_t i = 0; i < y.size() && i < span; ++i) { const float f = (float)std::pow(1.0 - (double)i / y.size(), 1.5); resp.L[i] = y.L[i] * f; resp.R[i] = y.R[i] * f; }
        }
        // envelope: S-curve attack; tucked under the voice while bleeding over it; S-curve release that ends at the voice's
        // return (bleed 0) or at the end of the bleed
        const size_t att = std::max<size_t>(1, (size_t)(s.attackMs * 1e-3 * sr)), rel = std::max<size_t>(1, (size_t)(s.releaseS * sr));
        const size_t endAt = (r - c) + extra, relStart = endAt > rel ? endAt - rel : 0, entrance = r - c;
        const double duck = dbToGain(s.bleedDuckDb); const size_t duckLen = (size_t)(0.05 * sr);
        const float side = (idx++ % 2 == 0) ? 1.f : -1.f;
        for (size_t i = 0; i < span; ++i) {
            auto sc = [](double u) { u = std::clamp(u, 0.0, 1.0); return u * u * (3 - 2 * u); };
            double g = sc((double)i / att) * (1.0 - sc((double)((long)i - (long)relStart) / rel));
            if (extra > 0 && i + duckLen > entrance) g *= 1.0 + (duck - 1.0) * sc((double)((long)i + (long)duckLen - (long)entrance) / duckLen);
            const float m = (float)(resp.mid(i) * s.level * g * wgt);
            out.L[c + i] += m * (1 + side * (float)s.spread); out.R[c + i] += m * (1 - side * (float)s.spread);
        }
    }
    return out;
}

} // namespace am::studio
