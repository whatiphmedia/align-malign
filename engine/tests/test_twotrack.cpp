// Auto Malign 2-track mode: headroom from phase (flat magnitude, capped group delay, real peak reduction on an
// unlimited mix) and mono-compatibility repair (fixes coherent anti-phase only, leaves decorrelated width alone).
#include "am/studio/twotrack.h"
#include <cstdio>
#include <random>
using namespace am::studio;
using namespace am::studio::twotrack;
using am::cplx;

static int fails = 0;
static void check(bool ok, const char* what) { std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; }

static double bandEnergy(std::vector<float> v, double lo, double hi, int sr) { bandpass(v, lo, hi, sr); double e = 0; for (float s : v) e += (double)s * s; return e; }

int main() {
    const int sr = 44100; std::mt19937 rng(7); std::normal_distribution<double> g(0, 1);
    // ---- headroom: a pre-limiter style mix (asymmetric kick on a sawtooth bass and hats), peaks stacking in phase
    {
        const size_t n = (size_t)(8.0 * sr); Stereo x(n);
        for (size_t i = 0; i < n; ++i) {
            const double t = (double)i / sr, beat = std::fmod(t, 0.5);
            const double kick = beat < 0.25 ? std::exp(-beat * 18) * std::sin(2 * M_PI * (50 * beat + 120 * (1 - std::exp(-beat * 30)) / 30)) : 0;
            double bass = 0; for (int h = 1; h <= 8; ++h) bass += 0.12 * std::cos(2 * M_PI * 55 * h * t) / h;   // synth bass, harmonics in cosine phase
            const double hat = std::fmod(t, 0.125) < 0.02 ? 0.01 * g(rng) : 0;
            const float v = (float)(0.55 * kick + bass + hat); x.L[i] = v; x.R[i] = 0.9f * v;
        }
        HeadroomSettings hs; hs.rounds = 2; hs.windowsPerChannel = 120;
        auto r = headroomFromPhase(x, sr, hs);
        std::printf("    curve:"); for (size_t k = 0; k < r.nodeHz.size(); ++k) std::printf(" %.0f:%.0f", r.nodeHz[k], r.thetaDeg[k]); std::printf("\n");
        std::printf("    peak %.2f -> %.2f dB, p99.9 %.2f -> %.2f, group delay %.2f ms\n", r.before.peakDb, r.after.peakDb, r.before.p999Db, r.after.p999Db, r.maxGroupDelayMs);
        check(r.applied && r.gainedPeakDb > 1.0, "headroom: at least 1 dB lower peaks on an unlimited kick + synth bass mix");
        check(std::abs(r.after.rmsDb - r.before.rmsDb) < 0.1, "headroom: loudness (RMS) unchanged");
        // flat magnitude of the filter
        std::vector<cplx> H(r.fir.size()); for (size_t i = 0; i < r.fir.size(); ++i) H[i] = r.fir[i]; fftOf((int)H.size()).transform(H);
        double dev = 0; for (size_t f = (size_t)(20.0 * H.size() / sr); f < H.size() / 2; ++f) dev = std::max(dev, std::abs(20 * std::log10(std::abs(H[f]))));
        std::printf("    magnitude deviation %.3f dB\n", dev);
        check(dev < 0.1, "headroom: the rotation is an all-pass (|H| flat within 0.1 dB above 20 Hz)");
        check(r.maxGroupDelayMs < 0.5 / 20 * 1000 * 1.5, "headroom: group delay stays within the cap");
        // the stereo image is untouched: L/R ratio preserved
        auto y = applyHeadroom(x, r); double e = 0, d = 0; for (size_t i = sr; i < n - sr; ++i) { d += std::pow(y.R[i] - 0.9 * y.L[i], 2); e += (double)y.R[i] * y.R[i]; }
        check(d < e * 1e-8, "headroom: same rotation on L and R (image untouched)");
    }
    // ---- headroom on a hard-limited signal must never make it worse
    {
        const size_t n = (size_t)(4.0 * sr); Stereo x(n);
        for (size_t i = 0; i < n; ++i) { const float v = (float)std::clamp(0.6 * g(rng), -0.5, 0.5); x.L[i] = v; x.R[i] = v; }
        HeadroomSettings hs; hs.rounds = 1; hs.windowsPerChannel = 60; auto r = headroomFromPhase(x, sr, hs);
        check(r.after.peakDb <= r.before.peakDb + 1e-6 || !r.applied, "headroom: never returns a louder peak than the input");
    }
    // ---- mono repair
    {
        const size_t n = (size_t)(6.0 * sr); Stereo x(n);
        std::vector<float> pad(n), centre(n), wideL(n), wideR(n);
        for (size_t i = 0; i < n; ++i) {
            const double t = (double)i / sr;
            pad[i] = (float)(0.2 * (std::sin(2 * M_PI * 330 * t) + 0.7 * std::sin(2 * M_PI * 440 * t)));
            centre[i] = (float)(0.2 * std::sin(2 * M_PI * 110 * t));
            wideL[i] = (float)(0.03 * g(rng)); wideR[i] = (float)(0.03 * g(rng));
        }
        bandpass(wideL, 3000, 6000, sr); bandpass(wideR, 3000, 6000, sr);
        for (size_t i = 0; i < n; ++i) {
            const bool inv = i > n / 3 && i < 2 * n / 3;                        // the pad goes anti-phase only in the middle third
            x.L[i] = centre[i] + pad[i] + wideL[i]; x.R[i] = centre[i] + (inv ? -0.95f : 1.f) * pad[i] + wideR[i];
        }
        MonoSettings ms; MonoResult r; auto y = monoRepair(x, sr, ms, &r);
        auto msum = [](const Stereo& s, size_t a, size_t b) { std::vector<float> m(b - a); for (size_t i = a; i < b; ++i) m[i - a] = s.L[i] + s.R[i]; return m; };
        const size_t a = n / 3 + sr / 2, b = 2 * n / 3 - sr / 4;
        const double padBefore = bandEnergy(msum(x, a, b), 250, 600, sr), padAfter = bandEnergy(msum(y, a, b), 250, 600, sr);
        std::printf("    pad in mono: %.1f dB more after repair; worst band %.1f -> %.1f dB, touched %.1f s\n", 10 * std::log10(padAfter / padBefore), r.worstBeforeDb, r.worstAfterDb, r.secondsTouched);
        check(padAfter > padBefore * 30, "mono: the anti-phase pad comes back in the mono sum (> 15 dB)");
        const size_t q0 = sr / 2, q1 = n / 3 - sr / 2; double dd = 0, ee = 0;
        for (size_t i = q0; i < q1; ++i) { dd += std::pow(y.L[i] - x.L[i], 2) + std::pow(y.R[i] - x.R[i], 2); ee += (double)x.L[i] * x.L[i]; }
        check(dd < ee * 1e-6, "mono: untouched (bit-near identical) where nothing cancels");
        std::vector<float> xl(x.L.begin() + a, x.L.begin() + b), yl(y.L.begin() + a, y.L.begin() + b);
        const double wb = bandEnergy(xl, 3000, 6000, sr), wa = bandEnergy(yl, 3000, 6000, sr);
        check(std::abs(10 * std::log10(wa / wb)) < 0.2, "mono: decorrelated wide highs left alone");
        check(std::abs(r.channelLevelChangeDb) < 0.3, "mono: each channel keeps its level");
        MonoSettings mf; mf.foldHz = 150; auto yf = monoRepair(x, sr, mf);
        double side = 0, sideX = 0; std::vector<float> sY(n), sX(n); for (size_t i = 0; i < n; ++i) { sY[i] = yf.L[i] - yf.R[i]; sX[i] = x.L[i] - x.R[i]; }
        side = bandEnergy(sY, 60, 140, sr); sideX = bandEnergy(sX, 60, 140, sr) + 1e-12;
        check(side <= sideX + 1e-9, "mono: low fold never adds side below the fold frequency");
    }
    std::printf(fails ? "twotrack: %d FAILED\n" : "twotrack: all passed\n", fails);
    return fails ? 1 : 0;
}
