// drumcheck: solver regression report on a folder of stems (or a list of wav files).
// Prints the predicted mix change per band and the number of losing 1/6-octave bands for
// ALIGN (guard on), ALIGN (guard off) and FUSE (extended profile).
//   drumcheck a.wav b.wav ...          (AM_RENDER=dir also writes the summed mixes, RMS-matched to the original)
#include "am/engine.h"
#include "am/studio/audio.h"
#include <cstdio>
#include <cstdlib>

using namespace am;
using namespace am::studio;

static void report(const char* name, const CrossSpectra& cs, const std::vector<std::vector<float>>& ph) {
    const int T = cs.T, K = cs.K;
    std::vector<double> d(K, 0.0), e0(K, 0.0), own(K, 0.0);
    for (int k = 1; k < K; ++k)
        for (int i = 0; i < T; ++i) {
            own[k] += cs.at(i, i, k).real();
            for (int j = i + 1; j < T; ++j) {
                const cplx c = cs.at(i, j, k);
                d[k] += 2 * ((c * std::polar(1.0, (double)ph[i][k] - ph[j][k])).real() - c.real());
                e0[k] += 2 * c.real();
            }
            e0[k] += 0; // e0 below = own + cross
        }
    const double edges[] = { 0, 120, 500, 2000, 4000 };
    std::printf("%-18s", name);
    for (int b = 0; b < 4; ++b) {
        double a = 0, z = 0;
        for (int k = 1; k < K; ++k) { const double f = k * cs.binHz; if (f < edges[b] || f >= edges[b + 1]) continue; a += own[k] + e0[k]; z += own[k] + e0[k] + d[k]; }
        std::printf(" %+6.2f", a > 0 && z > 0 ? 10 * std::log10(z / a) : 0.0);
    }
    int worse = 0; double worstDb = 0;
    for (int k = 1; k < K; ++k) {
        const int w = std::max(1, (int)(k * 0.059)); double sd = 0, so = 0, sm = 0;
        for (int q = std::max(1, k - w); q <= std::min(K - 1, k + w); ++q) { sd += d[q]; so += own[q]; sm += own[q] + e0[q]; }
        if (sd < -0.02 * so) { ++worse; if (getenv("AM_VERBOSE")) std::printf("   lose %.0f Hz %+.2f dB\n", k * cs.binHz, 10 * std::log10((sm + sd) / sm)); }
        if (sm > 0 && sm + sd > 0) worstDb = std::min(worstDb, 10 * std::log10((sm + sd) / sm));
    }
    std::printf("   losing %4d / %d   worst band %+.2f dB\n", worse, K, worstDb);
}

// FFT convolution of x with h, h centred at h.size()/2 (the centre delay is removed)
static std::vector<float> convolve(const std::vector<float>& x, const std::vector<float>& h) {
    int N = 1; while (N < (int)(x.size() + h.size())) N <<= 1;
    FFT f(N); std::vector<cplx> a(N), b(N);
    for (size_t i = 0; i < x.size(); ++i) a[i] = x[i];
    for (size_t i = 0; i < h.size(); ++i) b[i] = h[i];
    f.transform(a); f.transform(b);
    for (int i = 0; i < N; ++i) a[i] *= b[i];
    f.transform(a, true);
    std::vector<float> y(x.size()); const size_t c = h.size() / 2;
    for (size_t i = 0; i < y.size(); ++i) y[i] = (float)(a[i + c].real() / N);
    return y;
}

static std::vector<Stereo> g_stems;
static void render(const std::string& path, const CrossSpectra& cs, const std::vector<std::vector<float>>* ph, double refRms, int sr) {
    Stereo mix; mix.L.assign(g_stems[0].size(), 0.f); mix.R = mix.L;
    for (size_t t = 0; t < g_stems.size(); ++t) {
        const Stereo& s = g_stems[t];
        if (!ph) { for (size_t i = 0; i < mix.size() && i < s.size(); ++i) { mix.L[i] += s.L[i]; mix.R[i] += s.R[i]; } continue; }
        const auto h = buildIR((*ph)[t], {}, cs.binHz, sr);
        const auto l = convolve(s.L, h), r = convolve(s.R, h);
        for (size_t i = 0; i < mix.size() && i < s.size(); ++i) { mix.L[i] += l[i]; mix.R[i] += r[i]; }
    }
    double e = 0; for (size_t i = 0; i < mix.size(); ++i) e += mix.L[i] * mix.L[i] + mix.R[i] * mix.R[i];
    const double rms = std::sqrt(e / (2.0 * mix.size()));
    const double g = refRms > 0 ? refRms / rms : 1.0;
    for (size_t i = 0; i < mix.size(); ++i) { mix.L[i] *= (float)g; mix.R[i] *= (float)g; }
    writeWav(path, mix, sr, true);
    std::printf("   wrote %s (%+.2f dB level trim)\n", path.c_str(), 20 * std::log10(g));
}

int main(int argc, char** argv) {
    std::vector<std::vector<float>> mono; int sr = 0; size_t n = SIZE_MAX;
    for (int a = 1; a < argc; ++a) {
        Stereo s; WavInfo wi;
        if (!readWav(argv[a], s, wi)) { std::fprintf(stderr, "can't read %s: %s\n", argv[a], wi.error.c_str()); return 1; }
        sr = wi.sampleRate; mono.push_back(s.mono()); g_stems.push_back(s); n = std::min(n, mono.back().size());
    }
    const int T = (int)mono.size();
    if (T < 2) { std::fprintf(stderr, "need at least two stems\n"); return 2; }
    CrossSpectra cs; cs.reset(T, sr);
    FFT fft(kSolveN); const auto win = hann(kSolveN); std::vector<cplx> scratch;
    std::vector<std::vector<cplx>> X(T, std::vector<cplx>(cs.K));
    std::vector<const cplx*> ptr(T);
    for (size_t p = 0; p + kSolveN <= n; p += kHop) {
        for (int t = 0; t < T; ++t) { analyseFrame(fft, win, mono[t].data() + p, scratch, X[t].data(), cs.K); ptr[t] = X[t].data(); }
        cs.addFrame(ptr);
    }
    const int anchor = autoAnchor(cs);
    std::printf("%d stems, %d Hz, %lld frames, anchor %s\n", T, sr, (long long)cs.frames, argv[anchor + 1]);
    std::printf("%-18s %6s %6s %6s %6s\n", "mode", "<120", "-500", "-2k", "-4k");
    SolveSettings on; SolveSettings off = on; off.guard = false;
    report("ALIGN guard on", cs, solve(cs, anchor, profilesFor(on).aS));
    report("ALIGN guard off", cs, solve(cs, anchor, profilesFor(off).aS));
    report("FUSE", cs, solve(cs, anchor, profilesFor(on).aE));
    if (const char* dir = getenv("AM_RENDER")) {
        const std::string d = dir;
        render(d + "/1_original.wav", cs, nullptr, 0, sr);
        Stereo chk; WavInfo wi; readWav(d + "/1_original.wav", chk, wi);
        double eo = 0; for (size_t i = 0; i < chk.size(); ++i) eo += chk.L[i] * chk.L[i] + chk.R[i] * chk.R[i];
        const double ref = std::sqrt(eo / (2.0 * chk.size()));
        const auto a = solve(cs, anchor, profilesFor(on).aS);
        render(d + "/2_align_new.wav", cs, &a, ref, sr);
        Profile q = profilesFor(on).aS; q.maxGdMs = 15;
        const auto b = solve(cs, anchor, q);
        render(d + "/3_align_new_15ms.wav", cs, &b, ref, sr);
        const auto c = solve(cs, anchor, profilesFor(on).aE);
        render(d + "/4_fuse_new.wav", cs, &c, ref, sr);
    }
    if (getenv("AM_EXPERIMENT")) {
        auto run = [&](const char* nm, auto f) { Profile q = profilesFor(off).aS; f(q); report(nm, cs, solve(cs, anchor, q)); };
        run("std gd15", [](Profile& q) { q.maxGdMs = 15; });
        run("std gd30", [](Profile& q) { q.maxGdMs = 30; });
        run("std pull.3", [](Profile& q) { q.pull = 0.3; });
        run("std hi4k", [](Profile& q) { q.fHi = 4000; q.fTaper = 4400; });
        run("std 1/12oct", [](Profile& q) { q.smoothOct = 1.0 / 12; });
        run("std 1/3oct", [](Profile& q) { q.smoothOct = 1.0 / 3; });
        { auto ph = solve(cs, anchor, profilesFor(off).aS); ph[T - 2].assign(cs.K, 0.f); ph[T - 1].assign(cs.K, 0.f); report("std, x+y zeroed", cs, ph); }
        { Profile q = profilesFor(off).aS; auto u = solve(cs, anchor, q); q.maxGdMs = 60; auto v = solve(cs, anchor, q);
          for (int t = 0; t < T; ++t) { double mx = 0, mf = 0; for (int k = 2; k < cs.K; ++k) { const double gd = -(v[t][k] - v[t][k - 1]) / (2 * M_PI * cs.binHz) * 1e3; if (std::fabs(gd) > std::fabs(mx)) { mx = gd; mf = k * cs.binHz; } }
            double err = 0; for (int k = 1; k < cs.K; ++k) err = std::max(err, std::fabs(std::remainder((double)u[t][k] - v[t][k], 2 * M_PI)));
            std::printf("   track %d: uncapped peak group delay %+.1f ms at %.0f Hz; 10 ms cap error up to %.0f deg\n", t, mx, mf, err * 180 / M_PI); } }
        run("fuse gd10", [&](Profile& q) { q = profilesFor(off).aE; q.maxGdMs = 10; });
        run("fuse pull.5", [&](Profile& q) { q = profilesFor(off).aE; q.pull = 0.5; });
    }
    return 0;
}
