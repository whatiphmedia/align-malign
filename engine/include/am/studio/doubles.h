// Auto Malign Studio: vocal doubles.
//  flamFix: line a double's consonants up with its reference: overall offset first (best consonant agreement within
//           +-40 ms), then each confidently matched syllable is moved whole (cut 15 ms before the onset, 8 ms
//           crossfade). Nothing is stretched or pitch-bent; unmatched syllables are left alone.
//  split:   per time-frequency bin (smoothed ~46 ms and 1/6 octave) the double = common part (its local projection on
//           the reference, phase-locked to it) + difference (the rest). Mono combing lives only in the common part,
//           so locking it removes the comb; the difference sums by power.
#pragma once
#include "learn.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace am::studio {

struct FlamReport { double overallMs = 0; int moved = 0, considered = 0; double alignBefore = 0, alignAfter = 0; };

namespace detail {
inline double corrAt(const std::vector<float>& a, size_t a0, const std::vector<float>& b, long b0, size_t len) {
    if (b0 < 0 || (size_t)b0 + len > b.size() || a0 + len > a.size()) return -2;
    double ma = 0, mb = 0; for (size_t i = 0; i < len; ++i) { ma += a[a0 + i]; mb += b[(size_t)b0 + i]; } ma /= len; mb /= len;
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < len; ++i) { const double x = a[a0 + i] - ma, y = b[(size_t)b0 + i] - mb; sab += x * y; saa += x * x; sbb += y * y; }
    return (saa > 1e-24 && sbb > 1e-24) ? sab / std::sqrt(saa * sbb) : -2;
}
inline double coincide(const std::vector<float>& re, const std::vector<float>& de, const std::vector<size_t>& ons, int sr, long shift = 0) {
    double s = 0; int n = 0; const size_t pre = sr / 50, len = (size_t)(0.07 * sr);
    for (auto o : ons) { if (o < pre) continue; const double c = corrAt(re, o - pre, de, (long)(o - pre) + shift, len); if (c > -2) { s += c; ++n; } }
    return n ? s / n : 0;
}
} // namespace detail

// Move whole syllables of `dub` by `lags` (samples, dub later = positive) at the given reference onsets.
inline std::vector<float> moveSyllables(const std::vector<float>& dub, std::vector<std::pair<size_t, long>> matches, int sr) {
    if (matches.empty()) return dub;
    std::sort(matches.begin(), matches.end());
    const size_t n = dub.size(), xf = (size_t)(0.008 * sr), pre = (size_t)(0.015 * sr);
    struct Seg { size_t a, b; long lag; };
    std::vector<Seg> segs; segs.push_back({ 0, matches[0].first > pre ? matches[0].first - pre : 0, 0 });
    for (size_t k = 0; k < matches.size(); ++k) {
        const size_t a = matches[k].first > pre ? matches[k].first - pre : 0;
        const size_t b = k + 1 < matches.size() ? (matches[k + 1].first > pre ? matches[k + 1].first - pre : 0) : n;
        segs.push_back({ a, b, matches[k].second });
    }
    std::vector<float> out(n, 0.f);
    for (auto& s : segs) {
        if (s.b <= s.a) continue;
        const size_t a0 = s.a > xf ? s.a - xf : 0, b0 = std::min(n, s.b + xf);
        for (size_t i = a0; i < b0; ++i) {
            double w = 1.0;
            if (s.a > 0 && i < s.a) w = (double)(i - a0) / std::max<size_t>(1, s.a - a0);
            if (s.b < n && i >= s.b) w = 1.0 - (double)(i - s.b) / std::max<size_t>(1, b0 - s.b);
            const long j = (long)i + s.lag;
            if (j >= 0 && (size_t)j < n) out[i] += (float)(w * dub[(size_t)j]);
        }
    }
    return out;
}

inline std::vector<float> flamFix(const std::vector<float>& dub, const std::vector<float>& ref, const TrackLearn& refL, const TrackLearn& dubL,
                                  int sr, double amount, FlamReport* rep = nullptr) {
    // reference onsets inside the regions where the double actually sings
    std::vector<size_t> ons;
    for (auto o : refL.onsets) for (auto& p : dubL.phrases) if (o >= p.a && o < p.b) { ons.push_back(o); break; }
    if (ons.size() < 3 || amount <= 0) { if (rep) *rep = {}; return dub; }
    const auto re = detail::hfEnvelope(ref, sr), de = detail::hfEnvelope(dub, sr);
    int bestMs = 0; double bestC = -3;
    for (int ms = -40; ms <= 40; ms += 5) { const double c = detail::coincide(re, de, ons, sr, (long)(ms * 1e-3 * sr)); if (c > bestC) { bestC = c; bestMs = ms; } }
    const long g = (long)(bestMs * 1e-3 * sr * amount);
    std::vector<float> f(dub.size(), 0.f);
    for (size_t i = 0; i < dub.size(); ++i) { const long j = (long)i + g; if (j >= 0 && (size_t)j < dub.size()) f[i] = dub[(size_t)j]; }
    const auto fe = detail::hfEnvelope(f, sr);
    std::vector<std::pair<size_t, long>> mt;
    const long L = (long)(0.05 * sr), step = std::max(1L, (long)(0.0005 * sr)); const size_t pre = sr / 50, len = (size_t)(0.07 * sr);
    for (auto o : ons) {
        if (o < pre + (size_t)L || o + len + (size_t)L > ref.size()) continue;
        double bc = -3; long bl = 0;
        for (long k = -L; k <= L; k += step) { const double c = detail::corrAt(re, o - pre, fe, (long)(o - pre) + k, len); if (c > bc) { bc = c; bl = k; } }
        if (bc > 0.6) mt.push_back({ o, (long)std::lround(bl * amount) });
    }
    auto out = moveSyllables(f, mt, sr);
    if (rep) {
        rep->overallMs = bestMs * amount; rep->moved = (int)mt.size(); rep->considered = (int)ons.size();
        rep->alignBefore = detail::coincide(re, de, ons, sr); rep->alignAfter = detail::coincide(re, detail::hfEnvelope(out, sr), ons, sr);
    }
    return out;
}

// Smooth a per-bin, per-frame field over time (two-pass one-pole, ~frames) and frequency (box of `oct` octaves).
template <class T>
inline void smoothTF(std::vector<std::vector<T>>& A, double frames = 4, double oct = 1.0 / 6) {
    if (A.empty()) return;
    const size_t Tn = A.size(), K = A[0].size(); const double a = std::exp(-1.0 / frames);
    for (size_t k = 0; k < K; ++k) {
        T y = A[0][k]; for (size_t t = 0; t < Tn; ++t) { y = (T)(a * y + (1 - a) * A[t][k]); A[t][k] = y; }
        y = A[Tn - 1][k]; for (size_t t = Tn; t-- > 0;) { y = (T)(a * y + (1 - a) * A[t][k]); A[t][k] = y; }
    }
    const double r = std::pow(2.0, oct / 2) - 1; std::vector<T> c(K + 1);
    for (auto& row : A) {
        c[0] = T(0); for (size_t k = 0; k < K; ++k) c[k + 1] = c[k] + row[k];
        for (size_t k = 0; k < K; ++k) { const size_t w = std::max<size_t>(1, (size_t)(k * r)); const size_t lo = k > w ? k - w : 0, hi = std::min(K, k + w + 1); row[k] = (c[hi] - c[lo]) / (double)(hi - lo); }
    }
}

struct SplitResult { std::vector<float> common, difference; };
inline SplitResult split(const std::vector<float>& dub, const std::vector<float>& ref) {
    const auto D = stft(dub), X = stft(ref);
    const size_t Tn = std::min(D.frames.size(), X.frames.size()), K = (size_t)D.bins();
    std::vector<std::vector<cplx>> Sdx(Tn, std::vector<cplx>(K)); std::vector<std::vector<double>> Sxx(Tn, std::vector<double>(K));
    for (size_t t = 0; t < Tn; ++t) for (size_t k = 0; k < K; ++k) { Sdx[t][k] = D.frames[t][k] * std::conj(X.frames[t][k]); Sxx[t][k] = std::norm(X.frames[t][k]); }
    smoothTF(Sdx); smoothTF(Sxx);
    Spectrogram C = D, E = D;
    for (size_t t = 0; t < Tn; ++t)
        for (size_t k = 0; k < K; ++k) {
            const double sx = Sxx[t][k] + 1e-12; const cplx proj = Sdx[t][k] / sx * X.frames[t][k];
            C.frames[t][k] = std::abs(Sdx[t][k]) / sx * X.frames[t][k];   // common part, locked to the reference
            E.frames[t][k] = D.frames[t][k] - proj;                       // difference
        }
    return { istft(C), istft(E) };
}

} // namespace am::studio
