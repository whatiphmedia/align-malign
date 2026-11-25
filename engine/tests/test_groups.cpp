// Groups: tracks in one group get the same correction (their phase to each other stays as recorded), regrouping
// re-solves from what Learn already heard, and master/group buses (or "off") are left out and pass through unchanged.
#include "am/session.h"
#include <cstdio>
using namespace am;

static double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double d = 0; for (size_t i = 0; i < a.size() && i < b.size(); ++i) d = std::max(d, (double)std::fabs(a[i] - b[i])); return d;
}
static double maxAbs(const std::vector<float>& a) { double d = 0; for (float x : a) d = std::max(d, (double)std::fabs(x)); return d; }
// how far an IR is from a pure centred delay
static double offDelta(const std::vector<float>& h) {
    double e = 0; for (size_t i = 0; i < h.size(); ++i) { const double want = (i == (size_t)kIRLen / 2) ? 1.0 : 0.0; e = std::max(e, std::fabs(h[i] - want)); } return e;
}

int main() {
    const double sr = 48000; const int N = (int)(sr * 16), B = 512;
    std::mt19937 rng(5); std::normal_distribution<double> g(0, 1);
    std::vector<std::vector<float>> tr(4, std::vector<float>(N));
    for (int i = 0; i < N; ++i) {
        const double t = i / sr;
        const double kick = 0.6 * std::sin(2 * M_PI * 58 * t) + 0.3 * std::sin(2 * M_PI * 117 * t);
        tr[0][i] = (float)(kick + 0.02 * g(rng));                                                   // kick in
        tr[1][i] = (float)(0.8 * (0.6 * std::sin(2 * M_PI * 58 * t + 1.1) + 0.3 * std::sin(2 * M_PI * 117 * t + 2.0)) + 0.02 * g(rng)); // kick out
        tr[2][i] = (float)(0.5 * std::sin(2 * M_PI * 58 * t - 2.2) + 0.4 * std::sin(2 * M_PI * 117 * t + 2.7) + 0.02 * g(rng)); // bass
    }
    for (int i = 0; i < N; ++i) tr[3][i] = tr[0][i] + tr[1][i] + tr[2][i];                       // master bus

    auto& bus = SessionBus::get();
    std::vector<std::shared_ptr<Slot>> s;
    std::vector<std::shared_ptr<const IRSet>> ir(4);
    std::mutex m;
    for (int t = 0; t < 4; ++t) {
        auto sl = bus.join(sr);
        sl->onIR = [&, t](std::shared_ptr<const IRSet> x) { std::lock_guard<std::mutex> l(m); ir[(size_t)t] = x; };
        s.push_back(sl);
    }
    bus.setKind(s[3], "master");
    bus.setGroup(s[0]->id(), "A");
    bus.setGroup(s[1]->id(), "A");
    bus.setLearning(true);
    for (int pos = 0; pos + B <= N; pos += B) {
        for (int t = 0; t < 4; ++t) s[(size_t)t]->write(tr[(size_t)t].data() + pos, B, pos, true, true);
        if ((pos / B) % 32 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    bus.setLearning(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    for (auto& sl : s) { sl->controls.knob = -1.f; sl->controls.match = 0.f; }
    bus.controlsChanged();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    int fails = 0;
    auto check = [&](bool ok, const char* what) { std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };
    Maps m0, m1, m2, m3; std::vector<float> d;
    bus.snapshot(s[0], m0, d); bus.snapshot(s[1], m1, d); bus.snapshot(s[2], m2, d); bus.snapshot(s[3], m3, d);
    check(m0.K > 0 && maxDiff(m0.alignStd[0], m1.alignStd[0]) < 1e-6, "kick in and kick out (group A) get the same correction");
    check(m0.K > 0 && std::max(maxAbs(m0.alignStd[0]), maxAbs(m2.alignStd[0])) > 0.3, "the group still moves against the bass");
    { std::lock_guard<std::mutex> l(m); check(ir[3] && offDelta(ir[3]->mid) < 1e-4, "the master bus passes through unchanged"); }
    const auto tracks = bus.tracks();
    check(tracks.size() == 4 && tracks[3].excluded && !tracks[0].excluded && tracks[0].group == "A", "track list reports groups and the left-out master");

    // ungroup: re-solve from the same learn, no new pass
    bus.setGroup(s[1]->id(), "");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    bus.snapshot(s[0], m0, d); bus.snapshot(s[1], m1, d);
    check(maxDiff(m0.alignStd[0], m1.alignStd[0]) > 0.2, "after ungrouping, kick out gets its own correction (no new learn)");

    for (auto& sl : s) bus.leave(sl);
    std::printf(fails ? "%d FAILED\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
