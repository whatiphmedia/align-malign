// twotrack: Auto Malign 2-track mode from the command line (a finished L/R mix).
//   twotrack <mix.wav> [--headroom] [--mono] [--out prefix]
//            [--hi Hz] [--slope deg/oct] [--rounds n]              headroom options
//            [--maxipd deg] [--amount 0..1] [--fold Hz] [--coh x]   mono options
// With neither --headroom nor --mono, both run (mono repair first, then headroom on the repaired mix).
// Writes <prefix>_headroom.wav / _mono.wav / _both.wav and, for mono, the mono fold-downs <prefix>_monosum_before/after.wav.
#include "am/studio/twotrack.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace am::studio;
using namespace am::studio::twotrack;

static void printPeaks(const char* label, const PeakStats& p) {
    std::printf("  %-7s peak %6.2f dBFS  rms %6.2f  crest %5.2f dB  block peaks p99 %6.2f p99.9 %6.2f  limiter work (+6 dB) %5.2f dB\n",
                label, p.peakDb, p.rmsDb, p.crestDb, p.p99Db, p.p999Db, p.work6Db);
}
static Stereo monoSum(const Stereo& x) { Stereo m(x.size()); for (size_t i = 0; i < x.size(); ++i) m.L[i] = m.R[i] = 0.5f * (x.L[i] + x.R[i]); return m; }

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: twotrack <mix.wav> [--headroom] [--mono] [--out prefix] [--hi Hz] [--slope deg] [--rounds n] [--maxipd deg] [--amount x] [--fold Hz] [--coh x]\n"); return 2; }
    HeadroomSettings hs; MonoSettings ms; bool doH = false, doM = false; std::string out = "twotrack";
    for (int i = 2; i < argc; ++i) {
        auto is = [&](const char* k) { return !std::strcmp(argv[i], k) && i + 1 < argc; };
        if (!std::strcmp(argv[i], "--headroom")) doH = true;
        else if (!std::strcmp(argv[i], "--mono")) doM = true;
        else if (is("--out")) out = argv[++i];
        else if (is("--hi")) hs.hiHz = std::atof(argv[++i]);
        else if (is("--slope")) hs.maxSlopeDegPerOct = std::atof(argv[++i]);
        else if (is("--rounds")) hs.rounds = std::atoi(argv[++i]);
        else if (is("--maxipd")) ms.maxIpdDeg = std::atof(argv[++i]);
        else if (is("--amount")) ms.amount = std::atof(argv[++i]);
        else if (is("--fold")) ms.foldHz = std::atof(argv[++i]);
        else if (is("--coh")) ms.minCoherence = std::atof(argv[++i]);
        else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!doH && !doM) doH = doM = true;
    Stereo x; WavInfo info;
    if (!readWav(argv[1], x, info)) { std::fprintf(stderr, "%s\n", info.error.c_str()); return 1; }
    const int sr = info.sampleRate;
    std::printf("%s: %d Hz, %d ch, %.1f s\n", argv[1], sr, info.channels, (double)x.size() / sr);
    auto t0 = std::chrono::steady_clock::now(); auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    Stereo cur = x;
    if (doM) {
        if (info.channels < 2) std::printf("mono repair: skipped (the file is mono, nothing can cancel)\n");
        else {
            MonoResult r; cur = monoRepair(x, sr, ms, &r);
            std::printf("mono repair (max IPD %.0f deg, amount %.2f, min coherence %.2f, fold %.0f Hz): %.1f s\n", ms.maxIpdDeg, ms.amount, ms.minCoherence, ms.foldHz, secs());
            std::printf("  whole-song mono sum vs stereo: %.2f -> %.2f dB   worst band (fixable loss): %.2f -> %.2f dB\n", r.monoLossBeforeDb, r.monoLossAfterDb, r.worstBeforeDb, r.worstAfterDb);
            std::printf("  touched %.1f s of the song; channel level change %.3f dB\n", r.secondsTouched, r.channelLevelChangeDb);
            std::printf("  bands that cancel (> 3 dB) at least 2%% of the time:\n");
            for (auto& b : r.bands) if (b.cancelBefore > 0.02 || b.lossBeforeDb < -1)
                std::printf("    %7.0f Hz  loss %6.2f -> %6.2f dB   cancelling %5.1f%% -> %5.1f%% of the time\n", b.hz, b.lossBeforeDb, b.lossAfterDb, 100 * b.cancelBefore, 100 * b.cancelAfter);
            std::printf("  deepest moments (before):"); for (auto& m : r.worstMoments) std::printf(" %.1fs (%.1f dB)", m.first, m.second); std::printf("\n");
            writeWav(out + "_mono.wav", cur, sr);
            writeWav(out + "_monosum_before.wav", monoSum(x), sr); writeWav(out + "_monosum_after.wav", monoSum(cur), sr);
        }
    }
    if (doH) {
        t0 = std::chrono::steady_clock::now();
        auto r = headroomFromPhase(cur, sr, hs);
        std::printf("headroom from phase (nodes %.0f-%.0f Hz, slope cap %.0f deg/oct, %d rounds): %.1f s\n", hs.loHz, hs.hiHz, hs.maxSlopeDegPerOct, r.roundsRun, secs());
        printPeaks("before", r.before); printPeaks("after", r.after);
        std::printf("  gained: peak %.2f dB, p99.9 block peaks %.2f dB; max group delay %.2f ms; %s\n", r.gainedPeakDb, r.gainedP999Db, r.maxGroupDelayMs, r.applied ? "applied" : "no rotation beat the input: left untouched");
        std::printf("  curve:"); for (size_t k = 0; k < r.nodeHz.size(); ++k) std::printf(" %.0fHz:%.0f", r.nodeHz[k], r.thetaDeg[k]); std::printf("\n");
        Stereo y = applyHeadroom(cur, r);
        writeWav(out + (doM && info.channels > 1 ? "_both.wav" : "_headroom.wav"), y, sr);
    }
    return 0;
}
