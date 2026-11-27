// Auto Malign Studio: offline DSP helpers (STFT, FFT convolution, filters, resampling). Framework free.
#pragma once
#include "../fft.h"
#include "audio.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <random>
#include <vector>

namespace am::studio {

inline const FFT& fftOf(int n) {   // cached plans (offline, single owner thread)
    static std::map<int, std::unique_ptr<FFT>> plans;
    auto& p = plans[n];
    if (!p) p = std::make_unique<FFT>(n);
    return *p;
}
inline int nextPow2(size_t n) { int p = 1; while ((size_t)p < n) p <<= 1; return p; }

// Short-time Fourier transform: Hann window, hop = N/4, frames[t][k] for k in [0, N/2].
struct Spectrogram {
    int N = 2048, hop = 512; size_t length = 0;
    std::vector<std::vector<cplx>> frames;
    int bins() const { return N / 2 + 1; }
};
inline Spectrogram stft(const std::vector<float>& x, int N = 2048, int hop = 0) {
    Spectrogram S; S.N = N; S.hop = hop ? hop : N / 4; S.length = x.size();
    const auto w = hann(N); const auto& F = fftOf(N);
    const long first = -(long)N / 2, count = (long)(x.size() + N) / S.hop + 1;
    std::vector<cplx> buf(N);
    for (long t = 0; t < count; ++t) {
        const long s0 = first + t * S.hop;
        for (int i = 0; i < N; ++i) { const long j = s0 + i; buf[i] = (j >= 0 && j < (long)x.size()) ? x[(size_t)j] * w[i] : 0.0; }
        F.transform(buf);
        S.frames.emplace_back(buf.begin(), buf.begin() + N / 2 + 1);
    }
    return S;
}
inline std::vector<float> istft(const Spectrogram& S) {
    const int N = S.N; const auto w = hann(N); const auto& F = fftOf(N);
    const size_t need = (size_t)(N + (long)S.frames.size() * S.hop + N);
    std::vector<double> acc(std::max(need, S.length + 2 * (size_t)N), 0.0), norm(acc.size(), 0.0);
    std::vector<cplx> buf(N);
    for (size_t t = 0; t < S.frames.size(); ++t) {
        const auto& fr = S.frames[t];
        for (int k = 0; k <= N / 2; ++k) buf[k] = fr[k];
        for (int k = 1; k < N / 2; ++k) buf[N - k] = std::conj(fr[k]);
        F.transform(buf, true);
        const long s0 = -(long)N / 2 + (long)t * S.hop + N;   // offset by N so indices stay positive
        for (int i = 0; i < N; ++i) { acc[(size_t)(s0 + i)] += buf[i].real() / N * w[i]; norm[(size_t)(s0 + i)] += w[i] * w[i]; }
    }
    std::vector<float> y(S.length);
    for (size_t i = 0; i < S.length; ++i) { const double nv = norm[i + N]; y[i] = (float)(nv > 1e-9 ? acc[i + N] / nv : 0.0); }
    return y;
}

// Linear convolution by FFT (offline; for reverbs and long IRs).
inline std::vector<float> convolve(const std::vector<float>& x, const std::vector<float>& h, size_t outLen = 0) {
    if (x.empty() || h.empty()) return std::vector<float>(outLen ? outLen : x.size(), 0.f);
    const size_t L = outLen ? outLen : x.size();
    const int B = nextPow2(std::max<size_t>(h.size() * 2, 4096)); const size_t step = (size_t)B - h.size() + 1;
    const auto& F = fftOf(B);
    std::vector<cplx> H(B, 0.0); for (size_t i = 0; i < h.size(); ++i) H[i] = h[i]; F.transform(H);
    std::vector<float> y(L + h.size(), 0.f); std::vector<cplx> X(B);
    for (size_t s = 0; s < x.size(); s += step) {
        std::fill(X.begin(), X.end(), 0.0);
        for (size_t i = 0; i < step && s + i < x.size(); ++i) X[i] = x[s + i];
        F.transform(X); for (int k = 0; k < B; ++k) X[k] *= H[k]; F.transform(X, true);
        for (int i = 0; i < B && s + i < y.size(); ++i) y[s + i] += (float)(X[i].real() / B);
    }
    y.resize(L); return y;
}

// RBJ biquad (direct form I) with simple factories; process whole buffers offline.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    static Biquad make(int type, double f, double sr, double Q = 0.7071) {   // 0 LP, 1 HP, 2 BP
        Biquad q; const double w = 2 * M_PI * std::clamp(f, 5.0, sr * 0.45) / sr, c = std::cos(w), al = std::sin(w) / (2 * Q), a0 = 1 + al;
        if (type == 0) { q.b0 = (1 - c) / 2 / a0; q.b1 = (1 - c) / a0; q.b2 = q.b0; }
        else if (type == 1) { q.b0 = (1 + c) / 2 / a0; q.b1 = -(1 + c) / a0; q.b2 = q.b0; }
        else { q.b0 = al / a0; q.b1 = 0; q.b2 = -al / a0; }
        q.a1 = -2 * c / a0; q.a2 = (1 - al) / a0; return q;
    }
    float run(float x) { const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x; y2 = y1; y1 = y; return (float)y; }
    void apply(std::vector<float>& v) { for (auto& s : v) s = run(s); }
};
inline void lowpass(std::vector<float>& v, double f, double sr) { auto q = Biquad::make(0, f, sr); q.apply(v); }
inline void highpass(std::vector<float>& v, double f, double sr) { auto q = Biquad::make(1, f, sr); q.apply(v); }
inline void bandpass(std::vector<float>& v, double lo, double hi, double sr) { highpass(v, lo, sr); highpass(v, lo, sr); lowpass(v, hi, sr); lowpass(v, hi, sr); }

// Frame RMS in dB on a fixed grid (hop samples).
inline std::vector<float> frameDb(const std::vector<float>& x, int hop) {
    std::vector<float> e(x.size() / hop + 1, -240.f);
    for (size_t t = 0; t * hop < x.size(); ++t) {
        double s = 0; size_t n = 0;
        for (size_t i = t * hop; i < std::min(x.size(), (t + 1) * hop); ++i, ++n) s += (double)x[i] * x[i];
        e[t] = n ? (float)(10 * std::log10(s / n + 1e-24)) : -240.f;
    }
    return e;
}
inline float percentile(std::vector<float> v, double p) {
    if (v.empty()) return 0.f;
    const size_t k = std::min(v.size() - 1, (size_t)std::max(0.0, p / 100.0 * (v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + (long)k, v.end()); return v[k];
}

// Read x at fractional positions (linear), for pitch/speed changes.
inline float sampleAt(const std::vector<float>& x, double pos) {
    if (pos < 0 || pos >= (double)x.size() - 1) return 0.f;
    const size_t i = (size_t)pos; const double f = pos - i; return (float)((1 - f) * x[i] + f * x[i + 1]);
}

inline std::vector<double> raisedFade(size_t L, size_t f) {   // 1 with raised-cosine ends of f samples
    std::vector<double> g(L, 1.0); f = std::min(f, L / 2);
    for (size_t i = 0; i < f; ++i) { const double r = 0.5 - 0.5 * std::cos(M_PI * (double)i / f); g[i] = r; g[L - 1 - i] = r; }
    return g;
}

} // namespace am::studio
