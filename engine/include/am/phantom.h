// Phantom notes: MIDI-played pitches made of stereo phase (Refine flavour > PHANTOM).
//
// Huggins mode (default): around each note and its harmonics, the track's left/right difference is removed (the
// band is made identical in both ears), a quiet identical carrier is added there, and the left ear's phase is
// swept through a full turn across a narrow band. Over headphones the brain hears a pitch at that frequency (the
// Huggins / dichotic pitch). An Assist resonator, tuned to the note and fed by the music, is added to the side
// only (mono-safe) to help the ear lock on; 25% was the clearest in listening tests on a full mix.
// Ghost mode: only the resonator (clear notes, mono-safe).
//
// Real time, sample accurate, framework-free. The caller schedules MIDI at absolute output-sample times (the
// plugin adds its latency so notes land on the host grid after delay compensation).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace am {

struct PhantomParams {
    bool huggins = true;       // false = Ghost (resonator only)
    float assist = 0.25f;      // resonator level (0..1); in Ghost mode the note level
    float purity = 0.7f;       // 0..1: how much of the resonator's noise is removed, keeping only the note and its harmonics
    float carrier = 0.35f;     // identical noise/source around the notes (0..1), relative to the track
    float strength = 0.5f;     // 0..1: band width of the phase sweep (8..20 % of the note) and of the mono zone
    bool fold = true;          // move every note into 250..600 Hz, where the ears follow phase best
    int partials = 6;          // harmonics per note (1..6), up to 1.5 kHz
    float attackMs = 15.f, releaseMs = 250.f;
    float glideMs = 0.f;       // 0 = jump; else each new note starts from the last pitch and glides
    float bendRange = 2.f;     // semitones for full pitch bend
};

class PhantomNotes {
public:
    static constexpr int kVoices = 8, kPartials = 6;

    void prepare(double sampleRate) {
        sr_ = sampleRate;
        for (auto& v : voices_) v = Voice{};
        for (auto& v : voices_) v.comb.assign(4096, 0.f);
        events_.clear();
        clock_ = 0;
        rmsM_ = rmsSrc_ = 1e-6; carRms_ = 0; carGain_ = 1;
        lastFreq_ = 0;
        bend_ = bendTarget_ = 0;
        sustain_ = false;
    }
    void setParams(const PhantomParams& p) { p_ = p; }
    int64_t clock() const { return clock_; }               // next output sample index

    // MIDI, at absolute output-sample times (>= clock()).
    void noteOn(int note, float velocity, int64_t at) { push({ at, Ev::On, note, velocity }); }
    void noteOff(int note, int64_t at) { push({ at, Ev::Off, note, 0 }); }
    void pitchBend(float v, int64_t at) { push({ at, Ev::Bend, 0, v }); }       // -1..1
    void sustainPedal(bool down, int64_t at) { push({ at, Ev::Sustain, 0, down ? 1.f : 0.f }); }
    void allOff() { for (auto& v : voices_) v.held = false, v.releasing = v.active; events_.clear(); sustain_ = false; }

    bool busy() const { for (auto& v : voices_) if (v.active) return true; return !events_.empty(); }
    // notes currently sounding (for the interface)
    std::vector<int> sounding() const { std::vector<int> n; for (auto& v : voices_) if (v.active && !v.releasing) n.push_back(v.note); return n; }

    // In-place on the (already latency-aligned) output. srcL/srcR: an injected source aligned to the same samples,
    // or null to use this track (resonators) and noise (carrier).
    void process(float* L, float* R, const float* srcL, const float* srcR, int n) {
        int i = 0;
        while (i < n) {
            while (!events_.empty() && events_.front().at <= clock_ + i) { apply(events_.front()); events_.erase(events_.begin()); }
            int end = n;
            if (!events_.empty()) end = (int)std::min<int64_t>(n, std::max<int64_t>(i + 1, events_.front().at - clock_));
            run(L + i, R + i, srcL ? srcL + i : nullptr, srcR ? srcR + i : nullptr, end - i);
            i = end;
        }
        clock_ += n;
    }

    static double noteHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }
    static double foldHz(double f) { while (f >= 600.0) f *= 0.5; while (f < 250.0) f *= 2.0; return f; }

private:
    struct Biquad {                                       // transposed direct form II
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float run(float x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return (float)y; }
        void allpass(double f, double Q, double sr) { set(f, Q, sr, true); }
        void bandpass(double f, double Q, double sr) { set(f, Q, sr, false); }
        void set(double f, double Q, double sr, bool ap) {
            const double w = 2 * M_PI * std::clamp(f, 10.0, sr * 0.45) / sr, al = std::sin(w) / (2 * Q), c = std::cos(w), a0 = 1 + al;
            if (ap) { b0 = (1 - al) / a0; b1 = -2 * c / a0; b2 = 1; } else { b0 = al / a0; b1 = 0; b2 = -al / a0; }
            if (ap) b2 = (1 + al) / a0;
            a1 = -2 * c / a0; a2 = (1 - al) / a0;
        }
        void reset() { z1 = z2 = 0; }
    };
    struct Voice {
        bool active = false, held = false, releasing = false;
        int note = 0; float vel = 1; double env = 0;
        double f = 440, fTarget = 440, fApplied = 0;
        std::array<Biquad, kPartials> ap, collapse, car;   // phase sweep (left), mono zone (side), carrier band
        int nPart = 0;
        Biquad hp, lp;                                      // resonator input band
        std::array<Biquad, kPartials> clean;                // narrow bands on the note's harmonics (Purity)
        int nClean = 0; double fClean = 0, purityApplied = -1, purityCar = -1;
        std::vector<float> comb; int cw = 0; double lp1 = 0;
        double ry = 1e-9, gain = 0;                         // resonator level control
        uint64_t age = 0;
    };
    struct Ev { int64_t at; enum Kind { On, Off, Bend, Sustain } kind; int note; float v; };

    void push(Ev e) {
        e.at = std::max<int64_t>(e.at, clock_);
        auto it = std::upper_bound(events_.begin(), events_.end(), e, [](const Ev& a, const Ev& b) { return a.at < b.at; });
        events_.insert(it, e);
    }
    void apply(const Ev& e) {
        if (e.kind == Ev::Bend) { bendTarget_ = std::clamp(e.v, -1.f, 1.f); return; }
        if (e.kind == Ev::Sustain) { sustain_ = e.v > 0.5f; if (!sustain_) for (auto& v : voices_) if (v.active && !v.held) v.releasing = true; return; }
        if (e.kind == Ev::Off || (e.kind == Ev::On && e.v <= 0)) {
            for (auto& v : voices_) if (v.active && v.note == e.note && v.held) { v.held = false; if (!sustain_) v.releasing = true; }
            return;
        }
        Voice* v = nullptr;
        for (auto& c : voices_) if (c.active && c.note == e.note) { v = &c; break; }      // retrigger
        if (!v) for (auto& c : voices_) if (!c.active) { v = &c; break; }
        if (!v) { v = &voices_[0]; for (auto& c : voices_) if (c.age < v->age) v = &c; }  // steal the oldest
        const bool fresh = !v->active;
        double f = noteHz(e.note);
        if (p_.fold) f = foldHz(f);
        v->note = e.note; v->vel = std::clamp(e.v, 0.f, 1.f); v->held = true; v->releasing = false; v->fTarget = f;
        if (fresh) {
            v->f = (p_.glideMs > 0 && lastFreq_ > 0) ? lastFreq_ : f;
            v->env = 0; v->fApplied = 0; v->gain = 0; v->ry = 1e-9; v->lp1 = 0; v->cw = 0;
            std::fill(v->comb.begin(), v->comb.end(), 0.f);
            for (auto* bank : { &v->ap, &v->collapse, &v->car, &v->clean }) for (auto& b : *bank) b.reset();
            v->fClean = 0; v->purityApplied = -1;
            v->hp.reset(); v->lp.reset();
            highpass(v->hp, 150); lowpass(v->lp, 6000);
        }
        v->active = true; v->age = ++ageCounter_;
        lastFreq_ = f;
    }
    void highpass(Biquad& b, double f) { const double w = 2 * M_PI * f / sr_, al = std::sin(w) / (2 * 0.707), c = std::cos(w), a0 = 1 + al;
        b.b0 = (1 + c) / 2 / a0; b.b1 = -(1 + c) / a0; b.b2 = (1 + c) / 2 / a0; b.a1 = -2 * c / a0; b.a2 = (1 - al) / a0; }
    void lowpass(Biquad& b, double f) { const double w = 2 * M_PI * std::min(f, sr_ * 0.45) / sr_, al = std::sin(w) / (2 * 0.707), c = std::cos(w), a0 = 1 + al;
        b.b0 = (1 - c) / 2 / a0; b.b1 = (1 - c) / a0; b.b2 = (1 - c) / 2 / a0; b.a1 = -2 * c / a0; b.a2 = (1 - al) / a0; }

    void retune(Voice& v, double f) {
        const double bw = 0.08 + 0.12 * std::clamp((double)p_.strength, 0.0, 1.0);   // 8..20 % of the note
        const int maxP = std::clamp(p_.partials, 1, kPartials);
        v.nPart = 0;
        for (int k = 1; k <= maxP; ++k) {
            const double fk = f * k;
            if (fk > 1500.0 || fk > sr_ * 0.4) break;
            v.ap[(size_t)v.nPart].allpass(fk, 1.0 / bw, sr_);           // a full turn across ~bw of the note
            v.collapse[(size_t)v.nPart].bandpass(fk, 1.0 / (5.0 * bw), sr_);  // mono zone, ~5x the sweep band
            // carrier band: 3x the sweep band at Purity 0, narrowing to ~0.8x at full Purity (less hiss between notes)
            v.car[(size_t)v.nPart].bandpass(fk, 1.0 / ((3.0 - 2.2 * std::clamp((double)p_.purity, 0.0, 1.0)) * bw), sr_);
            ++v.nPart;
        }
        v.fApplied = f; v.purityCar = p_.purity;
    }

    void retuneClean(Voice& v, double f, double pur) {
        const double Q = 8.0 + 72.0 * pur;                 // narrower with more Purity
        v.nClean = 0;
        for (int k = 1; k <= kPartials; ++k) {
            const double fk = f * k;
            if (fk > 6000.0 || fk > sr_ * 0.4) break;
            v.clean[(size_t)v.nClean++].bandpass(fk, Q, sr_);
        }
        v.fClean = f; v.purityApplied = pur;
    }

    void run(float* L, float* R, const float* sL, const float* sR, int n) {
        if (n <= 0) return;
        bool any = false;
        for (auto& v : voices_) any |= v.active;
        if (!any) return;                                   // untouched when nothing sounds
        const double att = 1.0 / std::max(1.0, p_.attackMs * 1e-3 * sr_), rel = 1.0 / std::max(1.0, p_.releaseMs * 1e-3 * sr_);
        const double glideA = p_.glideMs > 0 ? 1.0 - std::exp(-1.0 / (p_.glideMs * 1e-3 * sr_)) : 1.0;
        const double bendA = 1.0 - std::exp(-1.0 / (0.005 * sr_));
        const double rmsA = 1.0 - std::exp(-1.0 / (0.3 * sr_));
        const double carA = 1.0 - std::exp(-1.0 / (0.03 * sr_));
        for (int i = 0; i < n; ++i) {
            bend_ += (bendTarget_ - bend_) * bendA;
            const double bendMul = std::pow(2.0, bend_ * p_.bendRange / 12.0);
            float l = L[i], r = R ? R[i] : L[i];
            double M = 0.5 * (l + r), S = 0.5 * (l - r);
            rmsM_ += (M * M - rmsM_) * rmsA;
            const double xs = sL ? 0.5 * ((double)sL[i] + (sR ? sR[i] : sL[i])) : M;   // what feeds carrier and resonators
            if (sL) rmsSrc_ += (xs * xs - rmsSrc_) * rmsA;
            const double srcNorm = sL ? std::min(8.0, std::sqrt(rmsM_ / std::max(rmsSrc_, 1e-12))) : 1.0;
            const double noise = nd_(rng_) * std::sqrt(rmsM_);
            double assistSide = 0, carrierMid = 0;
            double Lh = 0; bool hug = false;
            for (auto& v : voices_) {
                if (!v.active) continue;
                // envelope
                if (v.releasing) { v.env -= rel; if (v.env <= 0) { v.env = 0; v.active = false; continue; } }
                else v.env = std::min(1.0, v.env + att);
                // pitch: glide + bend, retune the filters every 32 samples if it moved
                v.f += (v.fTarget - v.f) * glideA;
                const double f = v.f * bendMul;
                if ((std::abs(f - v.fApplied) > 1e-4 * f || std::abs(p_.purity - v.purityCar) > 0.02) && ((i & 31) == 0 || v.fApplied == 0)) retune(v, f);
                const double e = v.env, depth = 0.3 + 0.7 * v.vel;
                // resonator (both modes): tuned comb fed by the music, added to the side only
                const double ex = v.lp.run(v.hp.run((float)(xs * srcNorm))) * e;
                const double D = std::min(sr_ / f, (double)v.comb.size() - 3); const int d0 = (int)D; const double fr = D - d0; const int N = (int)v.comb.size();
                const double a = v.comb[(size_t)((v.cw - d0 + N) % N)], b = v.comb[(size_t)((v.cw - d0 - 1 + N) % N)];
                v.lp1 = 0.65 * ((1 - fr) * a + fr * b) + 0.35 * v.lp1;
                const double y = ex + 0.93 * v.lp1;
                v.comb[(size_t)v.cw] = (float)y; v.cw = (v.cw + 1) % N;
                // only the ringing (not the music that excites it), then Purity keeps just the note's harmonics
                const double ring = y - ex;
                const double pur = std::clamp((double)p_.purity, 0.0, 1.0);
                if (std::abs(f - v.fClean) > 1e-4 * f && ((i & 31) == 0 || v.fClean == 0)) retuneClean(v, f, pur);
                else if (std::abs(pur - v.purityApplied) > 0.02 && (i & 31) == 0) retuneClean(v, f, pur);
                double tonal = 0;
                for (int k = 0; k < v.nClean; ++k) tonal += v.clean[(size_t)k].run((float)ring);
                const double out = ring + pur * (tonal - ring);
                v.ry += (out * out - v.ry) * rmsA;
                if (!v.releasing) {                         // level: Assist x 0.29 x the music's level (held while releasing)
                    const double want = 0.29 * std::sqrt(rmsM_) / std::sqrt(std::max(v.ry, 1e-12));
                    v.gain += (std::min(16.0, want) - v.gain) * 0.001;
                }
                assistSide += std::clamp(out * v.gain * p_.assist * depth, -1.0, 1.0);
                if (!p_.huggins) continue;
                hug = true;
                // Huggins: make the note's zone identical in both ears, add the carrier there, sweep the left phase
                for (int k = 0; k < v.nPart; ++k) {
                    const double gk = 1.0 / std::pow(k + 1.0, 0.3);
                    S -= e * v.collapse[(size_t)k].run((float)S);
                    const double c = v.car[(size_t)k].run((float)(sL ? xs * srcNorm : noise));
                    carrierMid += c * p_.carrier * depth * e * gk;
                }
            }
            // cap the total carrier at Carrier x the track's level however many notes overlap (a single note sits
            // far below this, so it only acts on big chords)
            carRms_ += (carrierMid * carrierMid - carRms_) * carA;
            const double cap = std::max(p_.carrier, 0.f) * std::sqrt(rmsM_);
            const double cg = carRms_ > cap * cap ? cap / std::sqrt(carRms_) : 1.0;
            carGain_ += (cg - carGain_) * (cg < carGain_ ? 0.05 : 0.0005);
            const double c = carrierMid * carGain_, lim = 3.0 * cap + 1e-9;   // and round off its noise spikes
            M += lim * std::tanh(c / lim);
            double outL = M + S, outR = M - S;
            if (hug) {
                Lh = outL;
                for (auto& v : voices_) {
                    if (!v.active || v.env <= 0) continue;
                    for (int k = 0; k < v.nPart; ++k) Lh += v.env * (v.ap[(size_t)k].run((float)Lh) - Lh);
                }
                outL = Lh;
            }
            const double as = std::clamp(assistSide, -1.0, 1.0);   // safety ceiling on what the resonators add
            // safety ceiling while notes sound: untouched up to 0 dBFS, rounded off above, never past +6 dBFS
            auto ceil = [](double x) { const double ax = std::abs(x); return ax <= 1.0 ? x : std::copysign(1.0 + std::tanh(ax - 1.0), x); };
            L[i] = (float)ceil(outL + as);
            if (R) R[i] = (float)ceil(outR - as);
        }
    }

    double sr_ = 48000;
    PhantomParams p_;
    std::array<Voice, kVoices> voices_{};
    std::vector<Ev> events_;
    int64_t clock_ = 0;
    uint64_t ageCounter_ = 0;
    double carRms_ = 0, carGain_ = 1;
    double rmsM_ = 1e-6, rmsSrc_ = 1e-6, lastFreq_ = 0, bend_ = 0, bendTarget_ = 0;
    bool sustain_ = false;
    std::mt19937 rng_{ 1234 };
    std::normal_distribution<double> nd_{ 0.0, 1.0 };
};

} // namespace am
