// Partitioned convolver: matches direct convolution (delayed by P), and swapping kernels never drops the signal.
#include "am/convolver.h"
#include "am/engine.h"
#include <cstdio>
using namespace am;

int main() {
    const int L = kIRLen, n = 48000 * 3;
    std::mt19937 rng(3);
    std::normal_distribution<float> g(0, 0.3f);
    std::vector<float> x(n), h1(L), h2(L, 0.f);
    for (auto& v : x) v = g(rng);
    for (int i = 0; i < L; ++i) h1[i] = (i == L / 2) ? 1.f : 0.f;            // neutral (centred delta)
    for (int i = 0; i < L; ++i) h2[i] = 0.5f * std::exp(-std::abs(i - L / 2) / 40.f) * std::cos(i * 0.05f);
    auto direct = [&](const std::vector<float>& h, int i) { double s = 0; for (int k = 0; k < L; ++k) if (i - k >= 0) s += h[k] * x[i - k]; return s; };

    PartitionedConvolver c(L);
    c.setKernel(c.makeKernel(h1));
    std::vector<float> y(x);
    std::uniform_int_distribution<int> bs(1, 1500);
    int fails = 0, swapAt = n / 3, swap2At = 2 * n / 3;
    bool s1 = false, s2 = false;
    for (int i = 0; i < n;) {
        if (!s1 && i >= swapAt) { c.setKernel(c.makeKernel(h1)); s1 = true; }   // same kernel again: must be seamless
        if (!s2 && i >= swap2At) { c.setKernel(c.makeKernel(h2)); s2 = true; }  // new kernel: crossfade, no gap
        const int b = std::min(bs(rng), n - i);
        c.process(y.data() + i, b);
        i += b;
    }
    const int P = PartitionedConvolver::P;
    double e1 = 0;
    for (int i = L + P; i < swap2At - 2 * P; ++i) e1 = std::max(e1, std::abs(y[i] - direct(h1, i - P)));
    std::printf("neutral kernel incl. a re-load mid-stream: max error %.2e\n", e1);
    if (e1 > 1e-4) { std::puts("  [FAIL] re-loading an identical kernel changed the output"); ++fails; } else std::puts("  [PASS] identical kernel reload is seamless");
    double e2 = 0, gap = 1e9;
    for (int i = swap2At + 3 * P; i < swap2At + 3 * P + 20000; ++i) e2 = std::max(e2, std::abs(y[i] - direct(h2, i - P)));
    for (int i = swap2At - P; i < swap2At + 3 * P; i += 64) {                // energy never drops out around the swap
        double s = 0; for (int k = 0; k < 256; ++k) s += y[i + k] * y[i + k];
        gap = std::min(gap, std::sqrt(s / 256));
    }
    std::printf("after switching kernels: max error %.2e, min RMS across the switch %.3f\n", e2, gap);
    if (e2 > 1e-4) { std::puts("  [FAIL] new kernel output wrong"); ++fails; } else std::puts("  [PASS] new kernel exact after the crossfade");
    if (gap < 0.05) { std::puts("  [FAIL] dropout during the switch"); ++fails; } else std::puts("  [PASS] no dropout during the switch");
    std::printf(fails ? "%d FAILED\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
