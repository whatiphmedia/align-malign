// Real-music safety of the solver (regression for "phasey at fully aligned"): unrelated tracks share no sound,
// so they must be left as recorded; no frequency may be moved in time by more than the cap; and a genuinely
// delayed second mic of the same source must still be aligned.
#include "am/engine.h"
#include <cstdio>
using namespace am;

int main() {
    const double sr = 48000; const int N = (int)(sr * 30);
    std::mt19937 rng(3); std::normal_distribution<double> g(0, 1);
    // a "kick": decaying 55 Hz thumps every 0.5 s; an unrelated "pad": slow random notes; an unrelated noise "hat"
    std::vector<float> kick(N, 0.f), out(N, 0.f), pad(N, 0.f), hat(N, 0.f);
    for (int i = 0; i < N; ++i) {
        const double t = std::fmod(i / sr, 0.5);
        kick[i] = (float)(std::exp(-t * 9) * std::sin(2 * M_PI * (55 + 80 * std::exp(-t * 30)) * t));
    }
    double f0 = 220, ph = 0;
    for (int i = 0; i < N; ++i) { if (i % (int)(sr * 0.7) == 0) f0 = 110 * std::pow(2.0, (int)(rng() % 24) / 12.0); ph += 2 * M_PI * f0 / sr; pad[i] = (float)(0.3 * std::sin(ph) + 0.15 * std::sin(2 * ph)); }
    for (int i = 0; i < N; ++i) hat[i] = (float)(0.05 * g(rng));
    const int d = (int)(sr * 0.0035);                                    // kick-out mic: 3.5 ms later, a bit darker
    for (int i = d + 1; i < N; ++i) out[i] = 0.8f * (0.6f * kick[i - d] + 0.4f * kick[i - d - 1]);
    std::vector<std::vector<float>> tr = { kick, out, pad, hat };

    CrossSpectra cs; cs.reset(4, sr); FFT fft(kSolveN); auto win = hann(kSolveN); std::vector<cplx> scr;
    std::vector<std::vector<cplx>> bins(4, std::vector<cplx>(cs.K));
    for (int p = 0; p + kSolveN <= N; p += kHop) { std::vector<const cplx*> ptr;
        for (int t = 0; t < 4; ++t) { analyseFrame(fft, win, tr[t].data() + p, scr, bins[t].data(), cs.K); ptr.push_back(bins[t].data()); }
        cs.addFrame(ptr); }
    const auto m = solve(cs, autoAnchor(cs), alignStandard());

    int fails = 0;
    auto check = [&](bool ok, const char* what) { std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };
    double gd = 0, padRot = 0, padW = 0;
    for (int t = 0; t < 4; ++t)
        for (int k = 2; k < cs.K; ++k) {
            gd = std::max(gd, std::fabs(std::remainder((double)m[t][k] - m[t][k - 1], 2 * M_PI)) / (2 * M_PI * cs.binHz) * 1000);
            if (t == 2) { const double w = cs.at(2, 2, k).real(); padRot += w * m[2][k] * m[2][k]; padW += w; }
        }
    std::printf("max time shift %.2f ms, unrelated pad rotation %.3f rad\n", gd, std::sqrt(padRot / padW));
    check(gd <= alignStandard().maxGdMs + 0.5, "no frequency is moved in time by more than the cap");
    check(std::sqrt(padRot / padW) < 0.15, "an unrelated track is left as recorded");
    // kick pair alignment gain at 40-200 Hz
    double before = 0, after = 0;
    for (int k = 1; k < cs.K; ++k) {
        const double f = k * cs.binHz; if (f < 40 || f > 200) continue;
        const cplx c = cs.at(0, 1, k);
        const double e0 = cs.at(0, 0, k).real(), e1 = cs.at(1, 1, k).real();
        before += e0 + e1 + 2 * c.real();
        after += e0 + e1 + 2 * (c * std::polar(1.0, (double)m[0][k] - m[1][k])).real();
    }
    std::printf("kick in + kick out, 40-200 Hz: %+.2f dB\n", 10 * std::log10(after / before));
    check(10 * std::log10(after / before) > 2.0, "a delayed second mic of the same kick is still aligned");
    std::printf(fails ? "%d FAILED\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
