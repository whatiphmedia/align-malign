// End-to-end engine test with synthetic stems: three "tracks" are fed through the session bus exactly the way
// three plugin instances would feed it (block by block, by timeline position), then we check that
//   - Align raises the summed low-end energy where the tracks share content,
//   - Malign (Erase) lowers it,
//   - Match brings the long-term level back to the original,
//   - an untouched track/neutral knob is (numerically) a pure delay.
#include "am/session.h"
#include <cstdio>
#include <map>

using namespace am;

static std::vector<float> convolve(const std::vector<float>& x, const std::vector<float>& h) {
    int n = 1; while (n < (int)(x.size() + h.size())) n <<= 1;
    FFT fft(n);
    std::vector<cplx> X(n), H(n);
    for (size_t i = 0; i < x.size(); ++i) X[i] = x[i];
    for (size_t i = 0; i < h.size(); ++i) H[i] = h[i];
    fft.transform(X); fft.transform(H);
    for (int i = 0; i < n; ++i) X[i] *= H[i];
    fft.transform(X, true);
    std::vector<float> y(x.size());
    for (size_t i = 0; i < x.size(); ++i) y[i] = (float)(X[i].real() / n);
    return y;
}

static double bandEnergyDb(const std::vector<float>& x, double sr, double lo, double hi, size_t from, size_t len) {
    int n = 1; while (n < (int)len) n <<= 1;
    FFT fft(n);
    std::vector<cplx> X(n);
    for (size_t i = 0; i < len; ++i) X[i] = x[from + i];
    fft.transform(X);
    double e = 0;
    for (int k = 1; k < n / 2; ++k) { const double f = k * sr / n; if (f >= lo && f < hi) e += std::norm(X[k]); }
    return 10 * std::log10(e + 1e-30);
}

int main() {
    const double sr = 48000;
    const int secs = 24, N = (int)(sr * secs);
    std::mt19937 rng(7);
    std::normal_distribution<double> g(0, 1);
    std::vector<std::vector<float>> tr(3, std::vector<float>(N));
    for (int i = 0; i < N; ++i) {
        const double t = i / sr;
        // kick-ish bass and a bass guitar sharing 55 / 82.4 Hz, out of phase in places; a pad with its own content
        tr[0][i] = (float)(0.5 * std::sin(2 * M_PI * 55 * t) + 0.3 * std::sin(2 * M_PI * 82.4 * t) + 0.02 * g(rng));
        tr[1][i] = (float)(0.45 * std::sin(2 * M_PI * 55 * t + 2.6) + 0.3 * std::sin(2 * M_PI * 82.4 * t + 1.9) + 0.02 * g(rng));
        tr[2][i] = (float)(0.2 * std::sin(2 * M_PI * 196 * t + 0.4) + 0.15 * std::sin(2 * M_PI * 55 * t - 2.2) + 0.02 * g(rng));
    }

    auto& bus = SessionBus::get();
    std::vector<std::shared_ptr<Slot>> slots;
    std::map<int, std::shared_ptr<const IRSet>> latestIR;
    std::mutex irm;
    for (int t = 0; t < 3; ++t) {
        auto s = bus.join(sr);
        s->name = "Track " + std::to_string(t + 1);
        s->onIR = [&, id = t](std::shared_ptr<const IRSet> ir) { std::lock_guard<std::mutex> l(irm); latestIR[id] = ir; };
        slots.push_back(s);
    }

    // ---- learn: feed blocks like a host would (interleaving tracks per block)
    bus.setLearning(true);
    const int block = 512;
    for (int pos = 0; pos + block <= N; pos += block) {
        for (int t = 0; t < 3; ++t) slots[t]->write(tr[t].data() + pos, block, pos, true, true);
        if ((pos / block) % 32 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    bus.setLearning(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const auto st = bus.status();
    std::printf("linked=%d frames=%lld (%.1f s analysed) solved=%d anchor=%d\n", st.linked, (long long)st.frames, st.seconds, st.solved, st.anchor);
    int fails = 0;
    auto check = [&](bool ok, const char* what) { std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };
    check(st.solved && st.frames > 200, "session solved from timeline-aligned frames of all three tracks");

    auto render = [&](float knob, int flavour, float match) {
        for (auto& s : slots) { s->controls.knob = knob; s->controls.flavour = flavour; s->controls.match = match; s->controls.motionOn = false; }
        { std::lock_guard<std::mutex> l(irm); latestIR.clear(); }
        bus.controlsChanged();
        for (int w = 0; w < 200; ++w) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); std::lock_guard<std::mutex> l(irm); if (latestIR.size() == 3) break; }
        std::vector<float> sum(N, 0.f);
        for (int t = 0; t < 3; ++t) {
            std::shared_ptr<const IRSet> ir; { std::lock_guard<std::mutex> l(irm); ir = latestIR[t]; }
            if (!ir) { std::printf("missing IR for %d\n", t); continue; }
            auto y = convolve(tr[t], ir->mid);
            for (int i = 0; i < N; ++i) sum[i] += y[i];
        }
        return sum;
    };
    std::vector<float> orig(N, 0.f);
    for (int t = 0; t < 3; ++t) for (int i = 0; i < N; ++i) orig[i] += tr[t][i];
    // processed output is delayed by kIRLen/2; compare matching windows
    const size_t lat = kIRLen / 2, from = (size_t)(2 * sr), len = (size_t)(16 * sr);
    auto bandDb = [&](const std::vector<float>& x, size_t off, double lo, double hi) { return bandEnergyDb(x, sr, lo, hi, from + off, len); };
    const double o40 = bandDb(orig, 0, 40, 130), oHi = bandDb(orig, 0, 150, 1000);

    auto neutral = render(0.f, 0, 0.f);
    double maxErr = 0;
    for (size_t i = from; i < from + len; ++i) maxErr = std::max(maxErr, (double)std::fabs(neutral[i + lat] - orig[i]));
    std::printf("neutral knob: max |out - delayed in| = %.2e\n", maxErr);
    check(maxErr < 2e-3, "neutral knob = pure delay of kIRLen/2 (transparent)");

    auto al = render(-1.f, 0, 0.f);
    const double a40 = bandDb(al, lat, 40, 130);
    std::printf("align  : 40-130 Hz %+.2f dB vs original\n", a40 - o40);
    check(a40 - o40 > 1.0, "align raises shared low-end energy");

    auto er = render(1.f, 1, 0.f);
    const double e40 = bandDb(er, lat, 40, 130);
    std::printf("erase  : 40-130 Hz %+.2f dB vs original\n", e40 - o40);
    check(e40 - o40 < -1.0, "malign (Erase) cancels shared low end");

    auto am = render(-1.f, 0, 1.f);
    const double m40 = bandDb(am, lat, 40, 130), mHi = bandDb(am, lat, 150, 1000);
    std::printf("align+match: 40-130 Hz %+.2f dB, 150-1000 Hz %+.2f dB vs original\n", m40 - o40, mHi - oHi);
    // Match is capped at +-6 dB (spec); this synthetic case gains more than that, so expect the cap to be reached.
    const double expectLeft = std::max(0.0, (a40 - o40) - 6.0);
    check(std::fabs((m40 - o40) - expectLeft) < 0.75, "Match pulls the low end back (to the +-6 dB cap where needed)");

    for (auto& s : slots) bus.leave(s);
    std::printf(fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
