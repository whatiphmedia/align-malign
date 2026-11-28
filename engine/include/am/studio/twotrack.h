// Auto Malign Studio: 2-track mode (a finished L/R mix). Framework free, offline.
//   1. Headroom from phase: one static phase curve (the same on L and R, so the stereo image is untouched) is
//      optimised over the whole song so the peaks come down. Magnitude stays flat; the group delay is capped.
//   2. Mono-compatibility repair: finds the bands where L and R cancel in the mono sum and, only there and only
//      while it happens, turns L and R towards each other (each channel keeps its own tone). Optional low fold:
//      below a set frequency, align fully and fold the side to mono.
#pragma once
#include "dsp.h"
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string>

namespace am::studio::twotrack {

// ------------------------------------------------------------------------------------------------ peak metrics
struct PeakStats {
    double peakDb = -240, rmsDb = -240, crestDb = 0;   // sample peak over both channels, RMS, peak / RMS
    double p99Db = -240, p999Db = -240;                 // percentiles of the 10 ms block peaks
    double work6Db = 0;                                 // limiter work to push +6 dB into refCeilingDb: mean dB over blocks
};
inline std::vector<float> blockPeaks(const Stereo& x, int hop) {
    std::vector<float> b((x.size() + hop - 1) / hop, 0.f);
    for (size_t i = 0; i < x.size(); ++i) { float& v = b[i / hop]; v = std::max({ v, std::abs(x.L[i]), std::abs(x.R[i]) }); }
    return b;
}
inline PeakStats peakStats(const Stereo& x, int sr, double refCeilingDb) {
    PeakStats s; if (!x.size()) return s;
    double e = 0; float pk = 0;
    for (size_t i = 0; i < x.size(); ++i) { e += 0.5 * ((double)x.L[i] * x.L[i] + (double)x.R[i] * x.R[i]); pk = std::max({ pk, std::abs(x.L[i]), std::abs(x.R[i]) }); }
    s.peakDb = gainToDb(pk); s.rmsDb = 10 * std::log10(e / x.size() + 1e-24); s.crestDb = s.peakDb - s.rmsDb;
    auto b = blockPeaks(x, std::max(1, sr / 100)); std::vector<float> bd(b.size());
    double w = 0; size_t loud = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        bd[i] = (float)gainToDb(b[i]);
        if (bd[i] > s.peakDb - 30) { w += std::max(0.0, bd[i] + 6.0 - refCeilingDb); ++loud; }
    }
    s.work6Db = loud ? w / loud : 0; s.p99Db = percentile(bd, 99); s.p999Db = percentile(bd, 99.9);
    return s;
}

// ------------------------------------------------------------------------------------------------ 1. headroom from phase
struct HeadroomSettings {
    double loHz = 20, hiHz = 4000;      // rotation nodes from loHz to hiHz; above hiHz the last angle holds (no group delay)
    int nodesPerOct = 3;
    double maxSlopeDegPerOct = 120;     // group-delay cap: 120 deg/oct is about one gentle 2nd-order all-pass (~0.48/f s)
    int windowsPerChannel = 250;        // loudest moments watched per round (every sample within +-windowMs)
    double windowMs = 6;
    int rounds = 3, sweeps = 4, power = 10;   // power: 10 or 16 (soft max of the watched samples)
    int fir = 32768;                    // render filter length (latency fir/2, compensated)
};
struct HeadroomResult {
    std::vector<double> nodeHz, thetaDeg;   // the learned curve (node 0 is the fixed DC taper at 0 deg)
    PeakStats before, after;
    double gainedPeakDb = 0, gainedP999Db = 0, maxGroupDelayMs = 0;
    bool applied = false; int roundsRun = 0;
    std::vector<float> fir;                 // the all-pass filter (centred; latency fir/2)
};

namespace detail {
struct Nodes { std::vector<double> hz, u; };   // u = log2(hz)
inline Nodes makeNodes(const HeadroomSettings& s) {
    Nodes n; n.hz.push_back(s.loHz / 4);       // fixed taper node: 0 deg (real filters must keep DC real)
    for (int k = 0;; ++k) { const double f = s.loHz * std::pow(2.0, (double)k / s.nodesPerOct); if (f > s.hiHz * 1.0001) break; n.hz.push_back(f); }
    for (double f : n.hz) n.u.push_back(std::log2(f));
    return n;
}
// piecewise-linear hats in log frequency; the first node owns everything below it, the last everything above it
inline void hats(const Nodes& n, double hz, int& k0, double& w0) {   // weight w0 on node k0, 1-w0 on k0+1
    const int K = (int)n.u.size();
    if (hz <= n.hz[0]) { k0 = 0; w0 = 1; return; }
    if (hz >= n.hz[K - 1]) { k0 = K - 2; w0 = 0; return; }
    const double u = std::log2(hz); int k = 0; while (k + 2 < K && u > n.u[k + 1]) ++k;
    k0 = k; w0 = (n.u[k + 1] - u) / (n.u[k + 1] - n.u[k]);
}
inline double wrapDeg(double a) { a = std::fmod(a + 180.0, 360.0); if (a < 0) a += 360; return a - 180; }

// unit-magnitude response: the node angles unwrapped along frequency (each step the short way round) and
// interpolated linearly in log frequency, so the slope cap is exactly the group-delay cap
inline std::vector<cplx> response(const Nodes& n, const std::vector<double>& thetaDeg, int N, int sr) {
    std::vector<double> un(thetaDeg.size()); un[0] = thetaDeg[0];
    for (size_t k = 1; k < un.size(); ++k) un[k] = un[k - 1] + wrapDeg(thetaDeg[k] - thetaDeg[k - 1]);
    std::vector<cplx> H(N / 2 + 1);
    for (int f = 0; f <= N / 2; ++f) {
        int k; double w; hats(n, (double)f * sr / N, k, w);
        double ph = w * un[k] + (1 - w) * un[k + 1];
        const double hz = (double)f * sr / N, fa = std::min(16000.0, 0.7 * sr / 2);   // ease to a real value at Nyquist
        if (hz > fa) { const double end = un.back(), tgt = 180.0 * std::round(end / 180.0), r = (hz - fa) / (sr / 2.0 - fa); ph = end + (tgt - end) * (0.5 - 0.5 * std::cos(M_PI * r)); }
        H[f] = std::polar(1.0, ph * M_PI / 180);
    }
    H[0] = 1; H[N / 2] = H[N / 2].real() >= 0 ? 1.0 : -1.0;
    return H;
}
inline std::vector<float> designFir(const std::vector<cplx>& H, int N) {
    std::vector<cplx> b(N);
    for (int f = 0; f <= N / 2; ++f) b[f] = H[f];
    for (int f = 1; f < N / 2; ++f) b[N - f] = std::conj(H[f]);
    fftOf(N).transform(b, true);
    std::vector<float> h(N);
    const int taper = N / 4;   // Tukey: flat centre, raised-cosine ends
    for (int i = 0; i < N; ++i) {
        const int j = (i + N / 2) % N; double w = 1;
        if (i < taper) w = 0.5 - 0.5 * std::cos(M_PI * i / taper); else if (i >= N - taper) w = 0.5 - 0.5 * std::cos(M_PI * (N - 1 - i) / taper);
        h[i] = (float)(b[j].real() / N * w);
    }
    return h;
}
inline double maxGroupDelayMs(const std::vector<cplx>& H, int N, int sr, double loHz) {
    double m = 0;
    for (int f = 1; f < N / 2; ++f) {
        const double hz = (double)f * sr / N; if (hz < loHz) continue;
        const double d = std::arg(H[f + 1] / H[f]);           // phase step per bin
        m = std::max(m, std::abs(d) / (2 * M_PI * sr / N) * 1000);
    }
    return m;
}
inline Stereo applyFir(const Stereo& x, const std::vector<float>& h) {
    const size_t lat = h.size() / 2, n = x.size(); Stereo y(n);
    auto L = convolve(x.L, h, n + lat), R = convolve(x.R, h, n + lat);
    for (size_t i = 0; i < n; ++i) { y.L[i] = L[i + lat]; y.R[i] = R[i + lat]; }
    return y;
}
// soft max of the 10 ms block peaks: the real (rendered) score, lower is better
inline double realScore(const Stereo& y, int sr, int p) {
    auto b = blockPeaks(y, std::max(1, sr / 100)); double m = 0; for (float v : b) m = std::max(m, (double)v);
    if (m <= 0) return 0;
    double s = 0; for (float v : b) s += std::pow(v / m, p);
    return m * std::pow(s / b.size(), 1.0 / p);
}
} // namespace detail

inline HeadroomResult headroomFromPhase(const Stereo& x, int sr, const HeadroomSettings& s = {}) {
    using namespace detail;
    HeadroomResult R; const auto nodes = makeNodes(s); const int K = (int)nodes.hz.size();
    R.nodeHz = nodes.hz; R.thetaDeg.assign(K, 0.0);
    const double ref = peakStats(x, sr, 0).peakDb; R.before = peakStats(x, sr, ref);
    auto slopeOk = [&](const std::vector<double>& th, int k) {
        for (int j : { k - 1, k + 1 }) if (j >= 0 && j < K) {
            const double oct = std::abs(nodes.u[k] - nodes.u[j]);
            if (std::abs(wrapDeg(th[k] - th[j])) > s.maxSlopeDegPerOct * oct + 1e-9) return false;
        }
        return true;
    };
    // analysis: per-bin hat weights; bins up to the last node use a direct sum, the rest (last band) = analytic - others
    const int N = 32768; const int lastBin = std::min(N / 2, (int)std::ceil(nodes.hz[K - 1] * N / sr) + 1);
    std::vector<int> bk(lastBin); std::vector<double> bw(lastBin);
    for (int f = 0; f < lastBin; ++f) { int k; double w; hats(nodes, (double)f * sr / N, k, w); bk[f] = k; bw[f] = w; }
    std::vector<double> tw(N); for (int i = 0; i < N; ++i) { const int t = N / 4; tw[i] = i < t ? 0.5 - 0.5 * std::cos(M_PI * i / t) : i >= N - t ? 0.5 - 0.5 * std::cos(M_PI * (N - 1 - i) / t) : 1.0; }
    const int half = std::max(8, (int)(s.windowMs / 1000 * sr)), W = 2 * half + 1;

    // z[k][sample]: analytic band values of the ORIGINAL signal at every watched sample (x = Re sum_k z_k)
    std::vector<std::vector<std::complex<float>>> z(K); std::vector<std::vector<char>> have(2, std::vector<char>((x.size() + 63) / 64 + 1, 0));
    std::vector<cplx> X(N), A(N), v(lastBin), r(lastBin); std::vector<cplx> zk(K);
    for (int f = 0; f < lastBin; ++f) r[f] = std::polar(1.0, 2 * M_PI * f / N);
    auto watch = [&](int ch, size_t at) {
        const auto& sig = ch ? x.R : x.L; const long s0 = (long)at - N / 2;
        for (int i = 0; i < N; ++i) { const long j = s0 + i; X[i] = (j >= 0 && j < (long)sig.size()) ? sig[(size_t)j] * tw[i] : 0.0; }
        fftOf(N).transform(X);
        for (int f = 0; f < N; ++f) A[f] = f == 0 || f == N / 2 ? X[f] : f < N / 2 ? 2.0 * X[f] : cplx(0);
        fftOf(N).transform(A, true);                                                   // analytic signal of the segment
        for (int f = 0; f < lastBin; ++f) v[f] = (f == 0 ? 1.0 : 2.0) * X[f] * std::polar(1.0, 2 * M_PI * f * (double)(N / 2 - half) / N);
        for (int m = 0; m < W; ++m) {
            std::fill(zk.begin(), zk.end(), cplx(0));
            for (int f = 0; f < lastBin; ++f) { zk[bk[f]] += bw[f] * v[f]; zk[bk[f] + 1] += (1 - bw[f]) * v[f]; v[f] *= r[f]; }
            cplx rest = A[(size_t)(N / 2 - half + m)];
            for (int k = 0; k < K - 1; ++k) { zk[k] /= (double)N; rest -= zk[k] * (double)N; }
            zk[K - 1] = zk[K - 1] / (double)N + rest / (double)N;
            for (int k = 0; k < K; ++k) z[k].push_back(std::complex<float>(zk[k]));
        }
    };

    Stereo best = x; double bestScore = realScore(x, sr, s.power); std::vector<double> theta(K, 0.0), bestTheta = theta;
    Stereo cur = x;
    for (int round = 0; round < s.rounds; ++round) {
        // watch the loudest moments of the current render (new peaks appear near the old ones as the waveform turns)
        for (int ch = 0; ch < 2; ++ch) {
            const auto& sig = ch ? cur.R : cur.L; const size_t hop = (size_t)W; std::vector<std::pair<float, size_t>> pk;
            for (size_t b0 = 0; b0 < sig.size(); b0 += hop) { size_t bi = b0; for (size_t i = b0; i < std::min(sig.size(), b0 + hop); ++i) if (std::abs(sig[i]) > std::abs(sig[bi])) bi = i; pk.push_back({ std::abs(sig[bi]), bi }); }
            std::sort(pk.begin(), pk.end(), [](auto& p, auto& q) { return p.first > q.first; });
            int added = 0;
            for (auto& [val, at] : pk) {
                if (added >= s.windowsPerChannel) break;
                const size_t c = at / 64; bool near = false; for (long d = -2; d <= 2; ++d) { const long q = (long)c + d; near |= q >= 0 && q < (long)have[ch].size() && have[ch][(size_t)q]; }
                if (near) continue;
                have[ch][c] = 1; watch(ch, at); ++added;
            }
        }
        const size_t C = z[0].size();
        auto model = [&](const std::vector<double>& th, std::vector<float>& y) {
            y.assign(C, 0.f);
            for (int k = 0; k < K; ++k) { const std::complex<float> p = std::polar(1.f, (float)(th[k] * M_PI / 180)); for (size_t c = 0; c < C; ++c) y[c] += (p * z[k][c]).real(); }
        };
        std::vector<float> y; model(theta, y);
        float T = 0; for (float q : y) T = std::max(T, std::abs(q)); if (T <= 0) break;
        const float iT = 1.f / T;
        auto pw = [&](float q) { q = std::abs(q) * iT; const float q2 = q * q, q4 = q2 * q2, q8 = q4 * q4; return s.power >= 16 ? q8 * q8 : q8 * q2; };
        auto J = [&](const std::vector<float>& yy) { double j = 0; for (float q : yy) j += pw(q); return j; };
        if (round == 0) {   // best constant rotation first (a classic "phase rotator")
            double bj = J(y); std::vector<double> bt = theta;
            for (int a = 15; a < 360; a += 15) { std::vector<double> th(K, (double)a); th[0] = 0; if (!slopeOk(th, 1)) continue; std::vector<float> yy; model(th, yy); const double j = J(yy); if (j < bj) { bj = j; bt = th; } }
            theta = bt; model(theta, y);
        }
        double jCur = J(y); std::vector<float> base(C);
        for (int sw = 0; sw < s.sweeps; ++sw) {
            const double step = sw < 2 ? 10.0 : 2.5; const int span = sw < 2 ? 18 : 4;   // coarse full circle, then fine
            for (int k = 1; k < K; ++k) {
                const std::complex<float> p0 = std::polar(1.f, (float)(theta[k] * M_PI / 180));
                for (size_t c = 0; c < C; ++c) base[c] = y[c] - (p0 * z[k][c]).real();
                double bestJ = jCur, bestA = theta[k];
                for (int i = -span; i <= span; ++i) {
                    if (!i) continue;
                    std::vector<double> th = theta; th[k] = wrapDeg(theta[k] + i * step);
                    if (!slopeOk(th, k)) continue;
                    const std::complex<float> p = std::polar(1.f, (float)(th[k] * M_PI / 180)); double j = 0;
                    for (size_t c = 0; c < C && j < bestJ; ++c) j += pw(base[c] + (p * z[k][c]).real());
                    if (j < bestJ - 1e-9) { bestJ = j; bestA = th[k]; }
                }
                if (bestA != theta[k]) { theta[k] = bestA; const std::complex<float> p = std::polar(1.f, (float)(bestA * M_PI / 180)); for (size_t c = 0; c < C; ++c) y[c] = base[c] + (p * z[k][c]).real(); jCur = bestJ; }
            }
        }
        // render for real and keep the best real result (never worse than the input)
        const auto H = response(nodes, theta, s.fir, sr); const auto h = designFir(H, s.fir);
        cur = applyFir(x, h); const double sc = realScore(cur, sr, s.power); R.roundsRun = round + 1;
        if (std::getenv("AM_DEBUG")) { float my = 0; for (float q : y) my = std::max(my, std::abs(q)); std::fprintf(stderr, "round %d: %zu watched samples, model max %.4f (was %.4f), real score %.4f (best %.4f), real peak %.4f\n", round, C, my, T, sc, bestScore, std::pow(10, peakStats(cur, sr, 0).peakDb / 20)); }
        if (sc < bestScore) { bestScore = sc; best = cur; bestTheta = theta; R.fir = h; }
    }
    R.thetaDeg = bestTheta; R.applied = !R.fir.empty();
    R.after = peakStats(best, sr, ref);
    R.gainedPeakDb = R.before.peakDb - R.after.peakDb; R.gainedP999Db = R.before.p999Db - R.after.p999Db;
    if (R.applied) R.maxGroupDelayMs = maxGroupDelayMs(response(nodes, bestTheta, s.fir, sr), s.fir, sr, s.loHz);
    return R;
}
inline Stereo applyHeadroom(const Stereo& x, const HeadroomResult& r) { return r.applied ? detail::applyFir(x, r.fir) : x; }

// ------------------------------------------------------------------------------------------------ 2. mono-compatibility repair
struct MonoSettings {
    double maxIpdDeg = 60;      // coherent L/R phase differences beyond this get turned back to it (60 deg: -1.25 dB in mono)
    double amount = 1;          // 0..1
    double minCoherence = 0.6;  // below this L and R are just different (wide, reverb, decorrelated): left alone
    double engageDb = -3;       // act only where the coherent part loses more than this in mono (lets go at half of it)
    double maxSlewDegPerS = 720;   // how fast the correction angle may move
    double foldHz = 0;          // >0: below this, align fully and fold the side to mono (1-octave crossover above it)
    double smoothMs = 120;      // statistics smoothing (symmetric)
    double attackMs = 20, releaseMs = 400;   // how fast the repair engages and lets go
    int bandsPerOct = 6, N = 8192;
};
struct MonoBand { double hz = 0, lossBeforeDb = 0, lossAfterDb = 0, cancelBefore = 0, cancelAfter = 0; };
struct MonoResult {
    std::vector<MonoBand> bands;            // per 1/6-octave band: energy-weighted fixable mono loss, share of time cancelling (> 3 dB)
    double monoLossBeforeDb = 0, monoLossAfterDb = 0;   // whole-song mono sum vs. stereo energy (0 dB = fully coherent, -3 dB = decorrelated)
    double worstBeforeDb = 0, worstAfterDb = 0;         // worst band (fixable loss)
    double secondsTouched = 0, channelLevelChangeDb = 0;
    std::vector<std::pair<double, double>> worstMoments;   // (time s, loss dB) of the deepest cancellations, before
};

namespace detail {
struct BandGrid { std::vector<int> b0, b1; std::vector<double> hz; };
inline BandGrid monoBands(int N, int sr, int perOct) {
    BandGrid g; const double binHz = (double)sr / N; int last = 1;
    for (double f = 20; f < sr * 0.45; f *= std::pow(2.0, 1.0 / perOct)) {
        const int hi = std::max(last + 1, (int)std::lround(f * std::pow(2.0, 1.0 / perOct) / binHz));
        if (hi > N / 2) break;
        if (hi - last < 1) continue;
        g.b0.push_back(last); g.b1.push_back(hi); g.hz.push_back(std::sqrt((double)last * hi) * binHz); last = hi;
    }
    return g;
}
struct MonoStats { std::vector<std::vector<cplx>> C; std::vector<std::vector<double>> P; std::vector<std::vector<double>> S; };   // [frame][band]: cross, |L|^2+|R|^2, |L+R|^2
template <class F> inline void frames(const Stereo& x, int N, F&& f) {
    const int hop = N / 4; const auto w = hann(N); std::vector<cplx> a(N), b(N);
    const long count = (long)(x.size() + N) / hop + 1;
    for (long t = 0; t < count; ++t) {
        const long s0 = -(long)N / 2 + t * hop;
        for (int i = 0; i < N; ++i) { const long j = s0 + i; const bool in = j >= 0 && j < (long)x.size(); a[i] = in ? x.L[(size_t)j] * w[i] : 0.0; b[i] = in ? x.R[(size_t)j] * w[i] : 0.0; }
        fftOf(N).transform(a); fftOf(N).transform(b); f(t, s0, a, b);
    }
}
inline MonoStats monoStats(const Stereo& x, const BandGrid& g, int N) {
    MonoStats m;
    frames(x, N, [&](long, long, std::vector<cplx>& a, std::vector<cplx>& b) {
        std::vector<cplx> c(g.hz.size()); std::vector<double> p(g.hz.size()), s(g.hz.size());
        for (size_t k = 0; k < g.hz.size(); ++k) for (int f = g.b0[k]; f < g.b1[k]; ++f) { c[k] += a[f] * std::conj(b[f]); p[k] += std::norm(a[f]) + std::norm(b[f]); s[k] += std::norm(a[f] + b[f]); }
        m.C.push_back(c); m.P.push_back(p); m.S.push_back(s);
    });
    return m;
}
inline void smoothFrames(std::vector<std::vector<cplx>>& C, std::vector<std::vector<double>>& P, double a) {   // symmetric one-pole (forward + backward)
    const size_t T = C.size(); if (!T) return; const size_t B = C[0].size();
    for (int dir = 0; dir < 2; ++dir) for (size_t k = 0; k < B; ++k) {
        cplx c = dir ? C[T - 1][k] : C[0][k]; double p = dir ? P[T - 1][k] : P[0][k];
        for (size_t i = 0; i < T; ++i) { const size_t t = dir ? T - 1 - i : i; c = a * c + (1 - a) * C[t][k]; p = a * p + (1 - a) * P[t][k]; C[t][k] = c; P[t][k] = p; }
    }
}
// fixable loss of the coherent part: 10 log10((1 + rho cos d) / (1 + rho)); 0 when in phase or decorrelated
inline double fixableLossDb(cplx C, double P) { if (P <= 1e-30) return 0; const double rho = std::min(1.0, 2 * std::abs(C) / P); return 10 * std::log10(std::max(1e-6, (1 + rho * std::cos(std::arg(C))) / (1 + rho))); }

inline void monoReport(const Stereo& x, const BandGrid& g, int N, int sr, double smoothMs, std::vector<double>& loss, std::vector<double>& cancel, double& worst, std::vector<std::pair<double, double>>* moments) {
    auto m = monoStats(x, g, N); const double a = std::exp(-(double)(N / 4) / (sr * smoothMs / 1000));
    smoothFrames(m.C, m.P, a);
    const size_t B = g.hz.size(), T = m.C.size(); loss.assign(B, 0); cancel.assign(B, 0); worst = 0;
    double peakP = 0; for (auto& r : m.P) for (double v : r) peakP = std::max(peakP, v);
    std::vector<std::pair<double, double>> mom;
    for (size_t k = 0; k < B; ++k) {
        double wsum = 0, l = 0, c = 0, bandPeak = 0; for (size_t t = 0; t < T; ++t) bandPeak = std::max(bandPeak, m.P[t][k]);
        for (size_t t = 0; t < T; ++t) {
            const double P = m.P[t][k]; if (P < bandPeak * 1e-4 || P < peakP * 1e-7) continue;   // band within 40 dB of its loudest, and audible
            const double d = fixableLossDb(m.C[t][k], P); l += P * d; c += P * (d < -3 ? 1 : 0); wsum += P;
            if (moments && d < -6) mom.push_back({ (double)t * (N / 4) / sr, d });
        }
        if (wsum > 0) { loss[k] = l / wsum; cancel[k] = c / wsum; }
        worst = std::min(worst, loss[k]);
    }
    if (moments) { std::sort(mom.begin(), mom.end(), [](auto& p, auto& q) { return p.second < q.second; }); for (auto& p : mom) { bool near = false; for (auto& q : *moments) near |= std::abs(q.first - p.first) < 2; if (!near) moments->push_back(p); if (moments->size() >= 5) break; } }
}
inline double wholeMonoLossDb(const Stereo& x) { double s = 0, e = 0; for (size_t i = 0; i < x.size(); ++i) { const double m = (double)x.L[i] + x.R[i]; s += m * m; e += 2 * ((double)x.L[i] * x.L[i] + (double)x.R[i] * x.R[i]); } return 10 * std::log10((s + 1e-30) / (e + 1e-30)); }
} // namespace detail

inline Stereo monoRepair(const Stereo& x, int sr, const MonoSettings& s, MonoResult* res = nullptr) {
    using namespace detail;
    const int N = s.N, hop = N / 4; const auto g = monoBands(N, sr, s.bandsPerOct); const size_t B = g.hz.size();
    // pass 1: smoothed band statistics -> wanted rotation per frame and band (unwrapped in time so it never flips sides)
    auto m = monoStats(x, g, N); smoothFrames(m.C, m.P, std::exp(-(double)hop / (sr * s.smoothMs / 1000)));
    const size_t T = m.C.size(); std::vector<std::vector<double>> corr(T, std::vector<double>(B, 0.0));
    const double aAtt = std::exp(-(double)hop / (sr * s.attackMs / 1000)), aRel = std::exp(-(double)hop / (sr * s.releaseMs / 1000));
    const double slew = s.maxSlewDegPerS * M_PI / 180 * hop / sr;
    double peakP = 0; for (auto& r : m.P) for (double v : r) peakP = std::max(peakP, v);
    for (size_t k = 0; k < B; ++k) {
        const double fold = s.foldHz > 0 ? (g.hz[k] <= s.foldHz ? 1.0 : g.hz[k] >= 2 * s.foldHz ? 0.0 : 0.5 + 0.5 * std::cos(M_PI * std::log2(g.hz[k] / s.foldHz))) : 0.0;
        const double lim = s.maxIpdDeg * (1 - fold) * M_PI / 180; double c = 0; bool on = false;
        for (size_t t = 0; t < T; ++t) {
            const double P = m.P[t][k]; const cplx C = m.C[t][k];
            const double rho = P > peakP * 1e-9 ? std::min(1.0, 2 * std::abs(C) / P) : 0.0, d = std::arg(C);
            const double loss = fixableLossDb(C, P);
            // engage only on real cancellation (coherent and losing), let go with hysteresis; the fold region always aligns
            if (rho >= s.minCoherence && (loss < s.engageDb || fold > 0.5)) on = true; else if (rho < s.minCoherence * 0.8 || (loss > s.engageDb * 0.5 && fold <= 0.5)) on = false;
            double target = 0;
            if (on) {
                const double coh = std::clamp((rho - s.minCoherence) / 0.15 + 0.3, 0.0, 1.0);
                target = coh * s.amount * (d - std::clamp(d, -lim, lim));
                if (std::abs(d) > 0.75 * M_PI) {   // near +-180 deg: turn the way the repair is already turning (no side flips)
                    const double d2 = d > 0 ? d - 2 * M_PI : d + 2 * M_PI, t2 = coh * s.amount * (d2 - std::clamp(d2, -lim, lim));
                    if (std::abs(t2 - c) < std::abs(target - c)) target = t2;
                }
            }
            const double a = std::abs(target) > std::abs(c) ? aAtt : aRel;   // engage fast, let go slowly
            const double next = a * c + (1 - a) * target;
            c += std::clamp(next - c, -slew, slew); corr[t][k] = c;
        }
    }
    // pass 2: rotate L by -c/2 and R by +c/2 per bin (c interpolated across bands in log frequency); output = input + change
    std::vector<double> dL(x.size() + 3 * (size_t)N, 0.0), dR(dL.size(), 0.0); const auto w = hann(N); size_t touched = 0;
    std::vector<int> bk(N / 2 + 1); std::vector<double> bw(N / 2 + 1);
    for (int f = 0; f <= N / 2; ++f) {
        const double hz = (double)f * sr / N; int k = 0; while (k + 1 < (int)B && g.hz[k + 1] < hz) ++k;
        bk[f] = k; bw[f] = (k + 1 < (int)B && hz > g.hz[k]) ? 1 - std::log2(hz / g.hz[k]) / std::log2(g.hz[k + 1] / g.hz[k]) : 1.0;
    }
    std::vector<double> foldW(N / 2 + 1, 0.0);
    if (s.foldHz > 0) for (int f = 1; f <= N / 2; ++f) { const double hz = (double)f * sr / N; foldW[f] = hz <= s.foldHz ? 1.0 : hz >= 2 * s.foldHz ? 0.0 : 0.5 + 0.5 * std::cos(M_PI * std::log2(hz / s.foldHz)); }
    frames(x, N, [&](long t, long s0, std::vector<cplx>& a, std::vector<cplx>& b) {
        bool any = false; for (size_t k = 0; k < B; ++k) any |= std::abs(corr[(size_t)t][k]) > 1e-4;
        if (!any) return;
        ++touched;
        std::vector<cplx> da(N, 0.0), db(N, 0.0);
        for (int f = 1; f < N / 2; ++f) {
            const int k = bk[f]; const double c = bw[f] * corr[(size_t)t][k] + (1 - bw[f]) * (k + 1 < (int)B ? corr[(size_t)t][k + 1] : 0.0);
            cplx L2 = a[f] * std::polar(1.0, -c / 2), R2 = b[f] * std::polar(1.0, c / 2);
            if (foldW[f] > 0) { const cplx M = 0.5 * (L2 + R2), S = 0.5 * (L2 - R2) * (1 - foldW[f] * s.amount); L2 = M + S; R2 = M - S; }
            da[f] = L2 - a[f]; db[f] = R2 - b[f]; da[N - f] = std::conj(da[f]); db[N - f] = std::conj(db[f]);
        }
        fftOf(N).transform(da, true); fftOf(N).transform(db, true);
        for (int i = 0; i < N; ++i) { const long j = s0 + i + N; dL[(size_t)j] += da[i].real() / N * w[i] / 1.5; dR[(size_t)j] += db[i].real() / N * w[i] / 1.5; }
    });
    Stereo y = x; for (size_t i = 0; i < x.size(); ++i) { y.L[i] += (float)dL[i + N]; y.R[i] += (float)dR[i + N]; }
    if (res) {
        MonoResult& r = *res; r = {}; std::vector<double> lb, cb, la, ca;
        monoReport(x, g, N, sr, s.smoothMs, lb, cb, r.worstBeforeDb, &r.worstMoments);
        monoReport(y, g, N, sr, s.smoothMs, la, ca, r.worstAfterDb, nullptr);
        for (size_t k = 0; k < B; ++k) r.bands.push_back({ g.hz[k], lb[k], la[k], cb[k], ca[k] });
        r.monoLossBeforeDb = wholeMonoLossDb(x); r.monoLossAfterDb = wholeMonoLossDb(y);
        r.secondsTouched = (double)touched * hop / sr;
        double ex = 0, ey = 0; for (size_t i = 0; i < x.size(); ++i) { ex += (double)x.L[i] * x.L[i] + (double)x.R[i] * x.R[i]; ey += (double)y.L[i] * y.L[i] + (double)y.R[i] * y.R[i]; }
        r.channelLevelChangeDb = 10 * std::log10((ey + 1e-30) / (ex + 1e-30));
    }
    return y;
}

} // namespace am::studio::twotrack
