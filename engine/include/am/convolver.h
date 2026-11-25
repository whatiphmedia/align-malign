// Auto Malign: real-time partitioned convolver with glitch-free impulse-response swaps.
//
// Why not juce::dsp::Convolution: when it receives a new impulse response it starts a fresh engine with an empty
// input history. With our centred FIRs (the energy sits 4096 samples in) the fresh engine outputs near-silence for
// the first ~4096 samples, which was heard as the session "skipping" every time a new map arrived (every solve
// while learning). Here every kernel shares one frequency-domain delay line of input spectra, so a new kernel is
// already "warmed up" the moment it arrives, and the switch is a one-block crossfade between two outputs that are
// both correct.
//
// Uniformly partitioned overlap-save (UPOLS): block P, FFT N = 2P, partitions K = irLen / P. Latency = P samples.
// Audio thread: process() only (no allocation, no locks). Any thread: setKernel() with a kernel from makeKernel().
#pragma once
#include "fft.h"
#include <array>
#include <atomic>
#include <memory>

namespace am {

class PartitionedConvolver {
public:
    static constexpr int P = 512;        // block size == added latency
    static constexpr int N = 2 * P;      // FFT size

    struct Kernel {
        int K = 0;
        std::vector<cplx> H;             // K partitions * N bins
    };

    explicit PartitionedConvolver(int irLen) : K_(std::max(1, (irLen + P - 1) / P)), fft_(N) {
        fdl_.assign((size_t)K_ * N, cplx(0, 0));
        in_.assign(N, 0.0);
        inBlock_.assign(P, 0.f);
        outBlock_.assign(P, 0.f);
        X_.assign(N, cplx(0, 0));
        Y_.assign(N, cplx(0, 0));
        Yold_.assign(N, cplx(0, 0));
        for (auto& r : retired_) r.store(nullptr);
    }
    ~PartitionedConvolver() {
        delete current_; delete fading_; delete pending_.exchange(nullptr);
        for (auto& r : retired_) delete r.exchange(nullptr);
    }

    // Any thread (allocates). h is the full impulse response.
    std::unique_ptr<Kernel> makeKernel(const std::vector<float>& h) const {
        auto k = std::make_unique<Kernel>();
        k->K = K_;
        k->H.assign((size_t)K_ * N, cplx(0, 0));
        std::vector<cplx> buf(N);
        FFT fft(N);
        for (int p = 0; p < K_; ++p) {
            std::fill(buf.begin(), buf.end(), cplx(0, 0));
            for (int i = 0; i < P; ++i) {
                const size_t idx = (size_t)p * P + i;
                if (idx < h.size()) buf[(size_t)i] = cplx(h[idx], 0);
            }
            fft.transform(buf);
            std::copy(buf.begin(), buf.end(), k->H.begin() + (size_t)p * N);
        }
        return k;
    }

    // Any thread. Hands the kernel to the audio thread; frees kernels the audio thread has retired.
    void setKernel(std::unique_ptr<Kernel> k) {
        collect();
        delete pending_.exchange(k.release());
    }

    // Audio thread. In-place, any block size. Output = input convolved with the kernel, delayed by P samples.
    void process(float* io, int n) noexcept {
        for (int i = 0; i < n; ++i) {
            inBlock_[(size_t)pos_] = io[i];
            io[i] = outBlock_[(size_t)pos_];
            if (++pos_ == P) { pos_ = 0; runBlock(); }
        }
    }

    void reset() noexcept {
        std::fill(fdl_.begin(), fdl_.end(), cplx(0, 0));
        std::fill(in_.begin(), in_.end(), 0.0);
        std::fill(inBlock_.begin(), inBlock_.end(), 0.f);
        std::fill(outBlock_.begin(), outBlock_.end(), 0.f);
        pos_ = 0;
    }

    void collect() { for (auto& r : retired_) delete r.exchange(nullptr); }

private:
    void runBlock() noexcept {
        // input spectrum of the last 2P samples into the frequency-domain delay line
        for (int i = 0; i < P; ++i) { in_[(size_t)i] = in_[(size_t)(i + P)]; in_[(size_t)(i + P)] = inBlock_[(size_t)i]; }
        for (int i = 0; i < N; ++i) X_[(size_t)i] = cplx(in_[(size_t)i], 0);
        fft_.transform(X_);
        head_ = (head_ + K_ - 1) % K_;
        std::copy(X_.begin(), X_.end(), fdl_.begin() + (size_t)head_ * N);

        if (Kernel* nk = pending_.exchange(nullptr)) {   // new kernel: crossfade from the current one this block
            if (fading_) retire(fading_);
            fading_ = current_;
            current_ = nk;
        }
        if (!current_) { std::fill(outBlock_.begin(), outBlock_.end(), 0.f); return; }
        accumulate(*current_, Y_);
        if (fading_) {
            accumulate(*fading_, Yold_);
            for (int i = 0; i < P; ++i) {
                const float r = (i + 0.5f) / P;
                outBlock_[(size_t)i] = (float)((1.0f - r) * Yold_[(size_t)(i + P)].real() + r * Y_[(size_t)(i + P)].real());
            }
            retire(fading_);
            fading_ = nullptr;
        } else {
            for (int i = 0; i < P; ++i) outBlock_[(size_t)i] = (float)Y_[(size_t)(i + P)].real();
        }
    }

    void accumulate(const Kernel& k, std::vector<cplx>& Y) noexcept {
        std::fill(Y.begin(), Y.end(), cplx(0, 0));
        for (int p = 0; p < K_; ++p) {
            const cplx* x = &fdl_[(size_t)((head_ + p) % K_) * N];
            const cplx* h = &k.H[(size_t)p * N];
            for (int b = 0; b < N; ++b) Y[(size_t)b] += x[b] * h[b];
        }
        fft_.transform(Y, true);
        const double s = 1.0 / N;
        for (auto& v : Y) v *= s;
    }

    void retire(Kernel* k) noexcept {
        for (auto& r : retired_) { Kernel* expected = nullptr; if (r.compare_exchange_strong(expected, k)) return; }
        // ring full (no thread has collected for a long time): keep it as the fading slot's spare rather than free here
        spare_ = k;
    }

    int K_;
    FFT fft_;
    std::vector<cplx> fdl_, X_, Y_, Yold_;
    std::vector<double> in_;
    std::vector<float> inBlock_, outBlock_;
    int pos_ = 0, head_ = 0;
    Kernel* current_ = nullptr;
    Kernel* fading_ = nullptr;
    Kernel* spare_ = nullptr;   // only used if the retire ring overflows (leaks one kernel at most per overflow)
    std::atomic<Kernel*> pending_{nullptr};
    std::array<std::atomic<Kernel*>, 16> retired_;
};

} // namespace am
