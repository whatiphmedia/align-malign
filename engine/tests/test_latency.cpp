// Upstream latency: a track whose audio reaches Auto Malign late (a synth or plugin with latency in front of it)
// must still be learned at its true song position. Learning with the latency compensated should give the same
// alignment as if there were no latency; ignoring it should give a worse one.
#include "am/session.h"
#include <cstdio>
using namespace am;

static std::vector<float> convolve(const std::vector<float>& x, const std::vector<float>& h) {
    int n = 1; while (n < (int)(x.size() + h.size())) n <<= 1;
    FFT fft(n); std::vector<cplx> X(n), H(n);
    for (size_t i = 0; i < x.size(); ++i) X[i] = x[i];
    for (size_t i = 0; i < h.size(); ++i) H[i] = h[i];
    fft.transform(X); fft.transform(H);
    for (int i = 0; i < n; ++i) X[i] *= H[i];
    fft.transform(X, true);
    std::vector<float> y(x.size()); for (size_t i = 0; i < x.size(); ++i) y[i] = (float)(X[i].real() / n);
    return y;
}
static double lowDb(const std::vector<float>& x, double sr, size_t from, size_t len) {
    int n = 1; while (n < (int)len) n <<= 1; FFT fft(n); std::vector<cplx> X(n);
    for (size_t i = 0; i < len; ++i) X[i] = x[from + i];
    fft.transform(X); double e = 0;
    for (int k = 1; k < n / 2; ++k) { const double f = k * sr / n; if (f >= 40 && f < 400) e += std::norm(X[k]); }
    return 10 * std::log10(e + 1e-30);
}

// returns the 40-400 Hz gain of the aligned sum vs the original sum, measured on the TRUE (undelayed) signals
static double run(const std::vector<std::vector<float>>& tr, double sr, int lat, bool compensate) {
    auto& bus = SessionBus::get();
    std::vector<std::shared_ptr<Slot>> s;
    std::vector<std::shared_ptr<const IRSet>> ir(tr.size());
    std::mutex m;
    for (size_t t = 0; t < tr.size(); ++t) {
        auto sl = bus.join(sr);
        sl->onIR = [&, t](std::shared_ptr<const IRSet> x) { std::lock_guard<std::mutex> l(m); ir[t] = x; };
        s.push_back(sl);
    }
    if (compensate) s[1]->setUpstreamLatency(lat);
    bus.setLearning(true);
    const int N = (int)tr[0].size(), B = 512;
    std::vector<float> late(N, 0.f);                       // what reaches track 1's plugin: its audio, `lat` samples late
    for (int i = lat; i < N; ++i) late[i] = tr[1][i - lat];
    for (int pos = 0; pos + B <= N; pos += B) {
        s[0]->write(tr[0].data() + pos, B, pos, true, true);
        s[1]->write(late.data() + pos, B, pos, true, true);
        if ((pos / B) % 32 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    bus.setLearning(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    for (auto& sl : s) { sl->controls.knob = -1.f; sl->controls.match = 0.f; }
    bus.controlsChanged();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::vector<float> sum(N, 0.f), orig(N, 0.f);
    for (size_t t = 0; t < tr.size(); ++t) {
        std::shared_ptr<const IRSet> h; { std::lock_guard<std::mutex> l(m); h = ir[t]; }
        auto y = h ? convolve(tr[t], h->mid) : tr[t];
        for (int i = 0; i < N; ++i) { sum[i] += y[i]; orig[i] += tr[t][i]; }
    }
    for (auto& sl : s) bus.leave(sl);
    const size_t lat0 = kIRLen / 2, from = (size_t)(2 * sr), len = (size_t)(14 * sr);
    return lowDb(sum, sr, from + lat0, len) - lowDb(orig, sr, from, len);
}

int main() {
    const double sr = 48000; const int N = (int)(sr * 20);
    std::mt19937 rng(11); std::normal_distribution<double> g(0, 1);
    std::vector<std::vector<float>> tr(2, std::vector<float>(N));
    for (int i = 0; i < N; ++i) {
        const double t = i / sr;
        tr[0][i] = (float)(0.5 * std::sin(2 * M_PI * 61 * t) + 0.3 * std::sin(2 * M_PI * 123 * t) + 0.3 * std::sin(2 * M_PI * 247 * t) + 0.02 * g(rng));
        tr[1][i] = (float)(0.5 * std::sin(2 * M_PI * 61 * t + 2.4) + 0.3 * std::sin(2 * M_PI * 123 * t + 2.0) + 0.3 * std::sin(2 * M_PI * 247 * t - 2.6) + 0.02 * g(rng));
    }
    const int lat = 1777;                                   // ~37 ms of synth/plugin latency in front of track 2
    const double base = run(tr, sr, 0, false), ignored = run(tr, sr, lat, false), comp = run(tr, sr, lat, true);
    std::printf("align gain 40-400 Hz: no latency %+.2f dB | latency ignored %+.2f dB | latency compensated %+.2f dB\n", base, ignored, comp);
    int fails = 0;
    if (std::fabs(comp - base) > 0.5) { std::puts("  [FAIL] compensated learn differs from the no-latency learn"); ++fails; }
    else std::puts("  [PASS] with the latency compensated, learning matches the no-latency case");
    if (!(ignored < base - 1.0)) { std::puts("  [FAIL] expected ignoring the latency to hurt"); ++fails; }
    else std::puts("  [PASS] ignoring the latency gives a worse alignment (the bug this fixes)");
    std::printf(fails ? "%d FAILED\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
