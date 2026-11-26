// Phantom notes (Refine flavour > PHANTOM): the Huggins cue lands on the note, Malodics mode is mono-safe, nothing
// changes without notes, MIDI lands exactly when scheduled, no clicks, stays bounded.
#include "am/phantom.h"
#include "am/fft.h"
#include <cstdio>
using namespace am;

static int fails = 0;
static void check(bool ok, const char* what) { std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; }

// interaural phase (degrees) at frequency f over [from, from+len)
static double ipd(const std::vector<float>& L, const std::vector<float>& R, size_t from, size_t len, double f, double sr) {
    int n = 1; while (n < (int)len) n <<= 1; FFT fft(n); std::vector<cplx> A(n), B(n);
    for (size_t i = 0; i < len; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / len); A[i] = L[from + i] * w; B[i] = R[from + i] * w; }
    fft.transform(A); fft.transform(B);
    const int k = (int)std::round(f * n / sr); cplx acc(0, 0);
    for (int j = k - 1; j <= k + 1; ++j) acc += A[j] * std::conj(B[j]);
    return std::arg(acc) * 180 / M_PI;
}

int main() {
    const double sr = 48000; const int N = (int)(sr * 3), B = 256;
    std::mt19937 rng(9); std::normal_distribution<double> g(0, 0.1);
    std::vector<float> nz(N); for (auto& x : nz) x = (float)g(rng);
    // a stereo "music" stand-in with some left/right difference
    std::vector<float> mL(N), mR(N); for (int i = 0; i < N; ++i) { mL[i] = (float)g(rng); mR[i] = 0.7f * mL[i] + 0.3f * (float)g(rng); }

    auto render = [&](const std::vector<float>& inL, const std::vector<float>& inR, PhantomParams p, auto&& schedule) {
        PhantomNotes pn; pn.prepare(sr); pn.setParams(p); schedule(pn);
        std::vector<float> L = inL, R = inR;
        for (int pos = 0; pos < N; pos += B) pn.process(L.data() + pos, R.data() + pos, nullptr, nullptr, std::min(B, N - pos));
        return std::make_pair(L, R);
    };
    const int note = 67;                                      // G4 -> 392 Hz (already in the sweet spot)
    const double f0 = PhantomNotes::foldHz(PhantomNotes::noteHz(note));

    {   // 1. no notes: bit-identical
        auto [L, R] = render(mL, mR, PhantomParams{}, [](PhantomNotes&) {});
        bool same = true; for (int i = 0; i < N; ++i) same &= (L[i] == mL[i] && R[i] == mR[i]);
        check(same, "no notes: the audio passes through untouched");
    }
    {   // 2. Huggins cue on identical noise in both ears: half a turn at the note, nothing away from it
        PhantomParams p; p.assist = 0.f; p.carrier = 0.f;
        auto [L, R] = render(nz, nz, p, [&](PhantomNotes& pn) { pn.noteOn(note, 1.f, (int64_t)(0.5 * sr)); });
        const size_t from = (size_t)(1.0 * sr), len = (size_t)(1.5 * sr);
        const double at = ipd(L, R, from, len, f0, sr), away = ipd(L, R, from, len, f0 * 0.62, sr), above = ipd(L, R, from, len, 4000, sr);
        std::printf("    L-R phase: at the note %+.0f deg, at 0.62x %+.0f deg, at 4 kHz %+.0f deg\n", at, away, above);
        check(std::abs(std::abs(at) - 180) < 30, "Huggins: the ears differ by about half a turn at the note");
        check(std::abs(above) < 10, "Huggins: nothing changes far above the note and its harmonics");
    }
    {   // 3. Malodics: the mono sum is untouched
        PhantomParams p; p.huggins = false; p.assist = 1.f;
        auto [L, R] = render(mL, mR, p, [&](PhantomNotes& pn) { pn.noteOn(note, 1.f, 1000); pn.noteOn(note + 4, 0.8f, 1000); pn.noteOff(note, (int64_t)(1.5 * sr)); });
        double md = 0, side = 0; for (int i = 0; i < N; ++i) { md = std::max(md, (double)std::abs((L[i] + R[i]) - (mL[i] + mR[i]))); side += std::pow((L[i] - R[i]) - (mL[i] - mR[i]), 2); }
        std::printf("    Malodics: mono sum change %.2e, added side rms %.4f\n", md, std::sqrt(side / N));
        check(md < 1e-6 && side > 0, "Malodics: notes are added to the stereo side only; the mono sum is unchanged");
    }
    {   // 4. timing: nothing before the scheduled sample, something right after
        PhantomParams p; p.attackMs = 1.f;
        const int64_t at = 30001;                            // deliberately not on a block boundary
        auto [L, R] = render(mL, mR, p, [&](PhantomNotes& pn) { pn.noteOn(note, 1.f, at); });
        int first = -1; for (int i = 0; i < N; ++i) if (L[i] != mL[i] || R[i] != mR[i]) { first = i; break; }
        std::printf("    first changed sample %d (scheduled %lld)\n", first, (long long)at);
        check(first == (int)at, "MIDI: the note starts exactly on the scheduled sample");
    }
    {   // 5. no clicks: the change at note on/off has no sudden step
        PhantomParams p;
        auto [L, R] = render(mL, mR, p, [&](PhantomNotes& pn) { pn.noteOn(note, 1.f, 24000); pn.noteOff(note, 72000); });
        double jump = 0, typical = 0;
        for (int i = 1; i < N; ++i) { const double d0 = (L[i] - mL[i]) - (L[i - 1] - mL[i - 1]); jump = std::max(jump, std::abs(d0)); typical += std::abs(mL[i] - mL[i - 1]); }
        typical /= N;
        std::printf("    largest sample-to-sample step of the change %.4f vs typical signal step %.4f\n", jump, typical);
        check(jump < 3 * typical, "no clicks: the change ramps in and out");
    }
    {   // 6. stays bounded with many loud notes, bends and glides
        PhantomParams p; p.assist = 1.f; p.carrier = 1.f; p.glideMs = 80; p.strength = 1.f;
        std::vector<float> loudL(N), loudR(N); for (int i = 0; i < N; ++i) { loudL[i] = (float)std::sin(i * 0.05) * 0.9f; loudR[i] = loudL[i]; }
        auto [L, R] = render(loudL, loudR, p, [&](PhantomNotes& pn) {
            for (int k = 0; k < 10; ++k) pn.noteOn(40 + 3 * k, 1.f, 2000 + 500 * k);
            pn.pitchBend(1.f, 40000); pn.pitchBend(-1.f, 80000); });
        double pk = 0; bool finite = true; for (int i = 0; i < N; ++i) { pk = std::max({ pk, (double)std::abs(L[i]), (double)std::abs(R[i]) }); finite &= std::isfinite(L[i]) && std::isfinite(R[i]); }
        std::printf("    peak with 10 loud notes, full assist and carrier: %.2f\n", pk);
        check(finite && pk <= 2.0, "safety: never past +6 dBFS, even with 10 loud notes and everything at maximum");
    }
    {   // 7. Purity: more of the resonator's output sits on the note's harmonics
        auto tonalShare = [&](float purity) {
            PhantomParams p; p.huggins = false; p.assist = 1.f; p.purity = purity;
            auto [L, R] = render(mL, mR, p, [&](PhantomNotes& pn) { pn.noteOn(note, 1.f, 1000); });
            const size_t from = (size_t)(1.0 * sr), len = (size_t)(1.5 * sr);
            int n = 1; while (n < (int)len) n <<= 1; FFT fft(n); std::vector<cplx> A(n);
            for (size_t i = 0; i < len; ++i) A[i] = 0.5 * ((L[from + i] - R[from + i]) - (mL[from + i] - mR[from + i]));
            fft.transform(A); double on = 0, off = 0;
            for (int k = 1; k < n / 2; ++k) {
                const double f = k * sr / n;
                if (f < 100 || f > 6000) continue;
                bool h = false;
                for (int m = 1; m <= 8; ++m) h |= std::abs(f - m * f0) < 0.03 * m * f0;
                (h ? on : off) += std::norm(A[k]);
            }
            return 10 * std::log10(on / off);
        };
        const double raw = tonalShare(0.f), pure = tonalShare(1.f);
        std::printf("    resonator on-note vs between-notes energy: purity 0 %+.1f dB, purity 1 %+.1f dB\n", raw, pure);
        check(pure > raw + 3, "Purity: the resonator's noise is removed, leaving the note and its harmonics");
    }
    {   // 8. carrier fills gaps: on a track that already has sound around the note it backs off; on a sparse one it stays
        auto carrierAdded = [&](const std::vector<float>& inL, const std::vector<float>& inR, bool fill) {
            PhantomParams p; p.assist = 0.f; p.fillGaps = fill;
            PhantomParams q = p; q.carrier = 0.f;
            auto a = render(inL, inR, p, [&](PhantomNotes& pn) { pn.noteOn(note, 1.f, 1000); });
            auto b = render(inL, inR, q, [&](PhantomNotes& pn) { pn.noteOn(note, 1.f, 1000); });
            double e = 0; for (int i = (int)sr; i < N; ++i) e += std::pow(a.first[i] - b.first[i], 2) + std::pow(a.second[i] - b.second[i], 2);
            return 10 * std::log10(e + 1e-30);
        };
        // dense: pink-ish noise with plenty of energy at the note; sparse: only highs (a hi-hat stand-in)
        std::vector<float> dense(N), sparse(N); double lp = 0;
        for (int i = 0; i < N; ++i) { lp = 0.98 * lp + 0.02 * g(rng) * 10; dense[i] = (float)lp; }
        { double prev = 0; for (int i = 0; i < N; ++i) { const double x = g(rng); sparse[i] = (float)(x - prev) * 0.5f; prev = x; } }
        const double denseCut = carrierAdded(dense, dense, false) - carrierAdded(dense, dense, true);
        const double sparseCut = carrierAdded(sparse, sparse, false) - carrierAdded(sparse, sparse, true);
        std::printf("    carrier reduced by filling gaps: dense track %.1f dB, sparse track %.1f dB\n", denseCut, sparseCut);
        check(denseCut > 10 && sparseCut < 6, "carrier fills gaps: backs off where the track has sound, stays where it doesn't");
    }
    std::printf(fails ? "%d FAILED\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
