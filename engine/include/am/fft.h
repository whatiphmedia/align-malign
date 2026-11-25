// Align/Malign engine: small iterative radix-2 complex FFT (no dependencies).
// Used on worker threads only (analysis and impulse-response builds), never on the audio thread.
#pragma once
#include <cmath>
#include <complex>
#include <vector>
#include <cassert>

namespace am {

using cplx = std::complex<double>;

class FFT {
public:
    explicit FFT(int n) : n_(n), tw_(n / 2), rev_(n) {
        assert(n >= 2 && (n & (n - 1)) == 0);
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            rev_[i] = r;
        }
        for (int i = 0; i < n / 2; ++i)
            tw_[i] = std::polar(1.0, -2.0 * M_PI * i / n);
    }

    int size() const { return n_; }

    // In-place forward transform (sign -1). inverse=true gives the unscaled inverse (sign +1).
    void transform(std::vector<cplx>& x, bool inverse = false) const {
        assert((int)x.size() == n_);
        for (int i = 0; i < n_; ++i)
            if (i < rev_[i]) std::swap(x[i], x[rev_[i]]);
        for (int len = 2; len <= n_; len <<= 1) {
            const int half = len / 2, step = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int j = 0; j < half; ++j) {
                    cplx w = tw_[j * step];
                    if (inverse) w = std::conj(w);
                    const cplx u = x[i + j], v = x[i + j + half] * w;
                    x[i + j] = u + v;
                    x[i + j + half] = u - v;
                }
        }
    }

private:
    int n_;
    std::vector<cplx> tw_;
    std::vector<int> rev_;
};

inline std::vector<double> hann(int n) {
    std::vector<double> w(n);
    for (int i = 0; i < n; ++i) w[i] = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / n);
    return w;
}

} // namespace am
