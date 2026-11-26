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

    // ---- Pro page controls
    {   // band: nothing rotated outside lowest..highest
        SolveSettings st; st.loHz = 80; st.hiHz = 300;
        const auto pb = solve(cs, autoAnchor(cs), profilesFor(st).aS);
        double outside = 0;
        for (int t = 0; t < 4; ++t) for (int k = 1; k < cs.K; ++k) { const double f = k * cs.binHz; if (f < 79 || f > 450) outside = std::max(outside, (double)std::fabs(pb[t][k])); }
        check(outside < 1e-6, "nothing is rotated outside the chosen frequency band");
    }
    {   // anchor: the chosen track is never changed
        SolveSettings st;
        const auto pa = solve(cs, 1, profilesFor(st).aS);
        double mx = 0; for (float v : pa[1]) mx = std::max(mx, (double)std::fabs(v));
        double other = 0; for (float v : pa[0]) other = std::max(other, (double)std::fabs(v));
        check(mx < 1e-6 && other > 0.1, "the anchor track stays as recorded while the others move to it");
    }
    {   // never make it worse: with the guard, no more bins clearly lose mix energy than without it (and very few
        // at all), while the real alignment it allows stays (the guard must not block a genuine multi-mic fix)
        auto worseBins = [&](const std::vector<std::vector<float>>& ph) {
            int worse = 0;
            for (int k = 1; k < cs.K; ++k) {
                double e0 = 0, e1 = 0, own = 0;
                for (int i = 0; i < 4; ++i) { own += cs.at(i, i, k).real();
                    for (int j = i + 1; j < 4; ++j) { const cplx c = cs.at(i, j, k); e0 += c.real(); e1 += (c * std::polar(1.0, (double)ph[i][k] - ph[j][k])).real(); } }
                if (e1 < e0 - 0.02 * own) ++worse;
            }
            return worse; };
        auto pairGain = [&](const std::vector<std::vector<float>>& ph) {   // kick in + kick out, 40-200 Hz
            double before = 0, after = 0;
            for (int k = 1; k < cs.K; ++k) { const double f = k * cs.binHz; if (f < 40 || f > 200) continue;
                const cplx c = cs.at(0, 1, k); const double e = cs.at(0, 0, k).real() + cs.at(1, 1, k).real();
                before += e + 2 * c.real(); after += e + 2 * (c * std::polar(1.0, (double)ph[0][k] - ph[1][k])).real(); }
            return 10 * std::log10(after / before); };
        SolveSettings on; on.stiffness = 0.0; SolveSettings off = on; off.guard = false;
        const auto pOn = solve(cs, autoAnchor(cs), profilesFor(on).aS), pOff = solve(cs, autoAnchor(cs), profilesFor(off).aS);
        const int wOn = worseBins(pOn), wOff = worseBins(pOff);
        std::printf("bins clearly worse: guard on %d, guard off %d (of %d); kick pair gain guard on %+.2f dB, off %+.2f dB\n", wOn, wOff, cs.K, pairGain(pOn), pairGain(pOff));
        check(wOn <= wOff && wOn <= cs.K / 500, "never make it worse: no more losing bins than without the guard, and very few");
        check(pairGain(pOn) > pairGain(pOff) - 0.5, "never make it worse doesn't block a real alignment");
    }
    {   // flavour amount 0 = the malign side does nothing; phase mono below keeps side = mid; side rotation turns the side
        Maps mp; mp.K = cs.K; mp.binHz = cs.binHz; mp.alignStd = m; mp.malignStd = m; mp.alignExt = m; mp.malignExt = m;
        Controls c; c.knob = 1.0; c.flavour = Flavour::Phantom; c.flavourAmt = 0.0;
        auto pc = effectivePhase(mp, 0, c);
        double mx = 0; for (size_t k = 0; k < pc.mid.size(); ++k) mx = std::max({ mx, (double)std::fabs(pc.mid[k]), (double)std::fabs(pc.side[k]) });
        check(mx < 1e-6, "flavour amount 0 leaves the malign side neutral");
        c.flavourAmt = 1.0; c.monoBelowHz = 150; c.sideRotRad = M_PI / 4;
        pc = effectivePhase(mp, 0, c);
        double lowDiff = 0, highRot = 0; int nh = 0;
        for (size_t k = 1; k < pc.mid.size(); ++k) { const double f = k * cs.binHz, d = std::remainder((double)pc.side[k] - pc.mid[k], 2 * M_PI);
            if (f < 140) lowDiff = std::max(lowDiff, std::fabs(d));
            if (f > 600 && f < 1500) { highRot += d; ++nh; } }
        check(lowDiff < 1e-5, "phase mono below: the side keeps the mid's phase in the low end");
        check(nh > 0 && std::fabs(highRot / nh - (M_PI / 4 + M_PI / 2)) < 0.05, "side rotation adds to the flavour's side turn above the mono point");
    }
    std::printf(fails ? "%d FAILED\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
