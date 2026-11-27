// Auto Malign Studio: learning a session. Per track, features on fixed frame grids along the timeline:
//   level (10 ms), consonant flux (5 ms), pitch (10 ms)
// and what is derived from them: singing/active regions (phrases and gaps), consonant onsets, steady notes.
// Session level: tempo and key. Because every feature lives on the timeline grid, a section can be re-learned
// alone (relearn): only its frames (plus a margin) are recomputed, then the derived lists are rebuilt.
#pragma once
#include "dsp.h"
#include "log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace am::studio {

struct Range { size_t a = 0, b = 0; size_t len() const { return b > a ? b - a : 0; } bool contains(size_t s) const { return s >= a && s < b; } };
struct Note { size_t start = 0, len = 0; int midi = 60; float vel = 1.f; };

struct TrackLearn {
    int sr = 44100, hopL = 441, hopF = 220;          // 10 ms and 5 ms grids
    std::vector<float> levelDb;                      // full-band level per 10 ms
    std::vector<float> hfDb, flux;                   // consonant band (2-10 kHz) level and its rise per 5 ms
    std::vector<float> f0;                           // Hz per 10 ms, 0 = unpitched
    // derived
    float topDb = -240.f;
    std::vector<uint8_t> active;                     // per 10 ms
    std::vector<Range> phrases, gaps;                // samples; gaps include the lead-in before the first phrase
    std::vector<size_t> onsets;                      // consonant onsets (samples)
    std::vector<Note> notes;
    std::vector<float> gapWeight;                    // per gap, set by the triggers (empty = all 1)
    float weight(size_t gi) const { return gi < gapWeight.size() ? gapWeight[gi] : 1.f; }
    double activeShare() const { size_t k = 0; for (auto a : active) k += a; return active.empty() ? 0 : (double)k / active.size(); }
};

struct LearnSettings {
    float onDb = 22.f, offDb = 28.f;    // active when above (top - onDb), stays active until below (top - offDb)
    float minGapS = 0.15f, minPhraseS = 0.12f;
    float onsetPct = 92.f, onsetRangeDb = 35.f, onsetMinS = 0.07f;
    float pitchConf = 0.55f, noteMinS = 0.12f;
};

namespace detail {
inline std::vector<float> hfEnvelope(const std::vector<float>& m, int sr) {   // 2-10 kHz band, rectified, 2 ms smoothing
    std::vector<float> b = m; bandpass(b, 2000, 10000, sr);
    const double a = std::exp(-1.0 / (0.002 * sr)); double y = 0;
    for (auto& v : b) { y = a * y + (1 - a) * std::abs(v); v = (float)y; }
    return b;
}
inline float pitchAt(const std::vector<float>& m, size_t center, int sr, float conf, float gateRms) {
    const int frame = 2048; const long s0 = (long)center - frame / 2;
    if (s0 < 0 || (size_t)(s0 + frame) > m.size()) return 0.f;
    const auto& F = fftOf(2 * frame); std::vector<cplx> X(2 * frame, 0.0);
    static const auto w = hann(frame);
    double e = 0;
    for (int i = 0; i < frame; ++i) { const double v = m[(size_t)(s0 + i)] * w[i]; X[i] = v; e += v * v; }
    if (std::sqrt(e / frame) < gateRms) return 0.f;
    F.transform(X); for (auto& c : X) c = std::norm(c); F.transform(X, true);
    const double r0 = X[0].real() + 1e-20; const int lo = sr / 1000, hi = sr / 70;
    int best = lo; double bv = -1;
    for (int k = lo; k < hi; ++k) { const double v = X[k].real() / r0; if (v > bv) { bv = v; best = k; } }
    if (bv < conf) return 0.f;
    const double a = X[best - 1].real(), b = X[best].real(), c = X[best + 1].real(), d = 0.5 * (a - c) / (a - 2 * b + c + 1e-20);
    return (float)(sr / (best + d));
}
} // namespace detail

// Compute the raw frame features for frames covering [r.a, r.b) (whole track when r is empty).
inline void learnFrames(const Stereo& x, int sr, TrackLearn& L, Range r = {}) {
    const auto m = x.mono(); L.sr = sr; L.hopL = std::max(1, sr / 100); L.hopF = std::max(1, sr / 200);
    const size_t nL = m.size() / L.hopL + 1, nF = m.size() / L.hopF + 1;
    if (L.levelDb.size() != nL) { L.levelDb.assign(nL, -240.f); L.f0.assign(nL, 0.f); }
    if (L.hfDb.size() != nF) { L.hfDb.assign(nF, -240.f); L.flux.assign(nF, 0.f); }
    if (r.len() == 0) r = { 0, m.size() };
    const size_t margin = (size_t)(0.25 * sr);
    const size_t a = r.a > margin ? r.a - margin : 0, b = std::min(m.size(), r.b + margin);
    // level
    for (size_t t = a / L.hopL; t < nL && t * L.hopL < b; ++t) {
        double s = 0; size_t n = 0;
        for (size_t i = t * L.hopL; i < std::min(m.size(), (t + 1) * L.hopL); ++i, ++n) s += (double)m[i] * m[i];
        L.levelDb[t] = n ? (float)(10 * std::log10(s / n + 1e-24)) : -240.f;
    }
    // consonant band and its rise (computed on the section plus filter warm-up)
    const size_t wa = a > (size_t)sr / 10 ? a - sr / 10 : 0;
    std::vector<float> seg(m.begin() + (long)wa, m.begin() + (long)b);
    const auto hf = detail::hfEnvelope(seg, sr);
    for (size_t t = a / L.hopF; t < nF && t * L.hopF < b; ++t) {
        const size_t i = t * L.hopF; L.hfDb[t] = (i >= wa && i - wa < hf.size()) ? (float)(20 * std::log10(hf[i - wa] + 1e-9)) : -240.f;
    }
    for (size_t t = std::max<size_t>(1, a / L.hopF); t < nF && t * L.hopF < b; ++t) L.flux[t] = std::max(0.f, L.hfDb[t] - L.hfDb[t - 1]);
    // pitch (gate at -60 dBFS)
    for (size_t t = a / L.hopL; t < nL && t * L.hopL < b; ++t) L.f0[t] = detail::pitchAt(m, t * L.hopL, sr, 0.55f, 1e-3f);
}

// Rebuild the derived lists (cheap) from the frame features.
inline void deriveTrack(TrackLearn& L, const LearnSettings& s, size_t lengthSamples) {
    std::vector<float> lv; for (auto v : L.levelDb) if (v > -200) lv.push_back(v);
    L.topDb = lv.empty() ? -240.f : percentile(lv, 95);
    L.active.assign(L.levelDb.size(), 0); bool a = false;
    for (size_t t = 0; t < L.levelDb.size(); ++t) { const float v = L.levelDb[t]; a = a ? v > L.topDb - s.offDb : v > L.topDb - s.onDb; L.active[t] = a; }
    // phrases and gaps (gaps shorter than minGap are bridged)
    L.phrases.clear(); L.gaps.clear();
    const size_t minGap = (size_t)(s.minGapS * 100), minPh = (size_t)(s.minPhraseS * 100);
    std::vector<Range> ph; size_t t = 0;
    while (t < L.active.size()) {
        if (!L.active[t]) { ++t; continue; }
        size_t e = t; while (e < L.active.size() && L.active[e]) ++e;
        if (!ph.empty() && t - ph.back().b < minGap) ph.back().b = e; else ph.push_back({ t, e });
        t = e;
    }
    for (auto& p : ph) if (p.len() >= minPh) L.phrases.push_back({ p.a * L.hopL, std::min(lengthSamples, p.b * L.hopL) });
    size_t prev = 0;
    for (auto& p : L.phrases) { if (p.a > prev) L.gaps.push_back({ prev, p.a }); prev = p.b; }
    if (!L.phrases.empty() && prev < lengthSamples) L.gaps.push_back({ prev, lengthSamples });   // the tail after the last phrase
    // consonant onsets: smoothed rise above its percentile, at least 70 ms apart, not far below the loudest
    std::vector<float> fl(L.flux.size(), 0.f);
    for (size_t i = 0; i < fl.size(); ++i) { float acc = 0; int n = 0; for (int k = -1; k <= 1; ++k) if (i + k < fl.size()) { acc += L.flux[i + k]; ++n; } fl[i] = acc / n; }
    std::vector<float> hv; for (auto v : L.hfDb) if (v > -200) hv.push_back(v);
    const float th = percentile(fl, s.onsetPct), hfTop = hv.empty() ? -240.f : percentile(hv, 99);
    const size_t minSep = (size_t)(s.onsetMinS * 200);
    L.onsets.clear(); size_t last = 0; bool any = false;
    for (size_t i = 1; i + 1 < fl.size(); ++i)
        if (fl[i] >= th && fl[i] > 0 && fl[i] >= fl[i - 1] && fl[i] >= fl[i + 1] && L.hfDb[i] > hfTop - s.onsetRangeDb && (!any || i - last >= minSep)) {
            L.onsets.push_back(i * L.hopF); last = i; any = true;
        }
    // steady notes
    L.notes.clear(); size_t i = 0;
    auto midiOf = [](float f) { return f > 0 ? (int)std::lround(69 + 12 * std::log2(f / 440.0)) : -1; };
    const float topLin = std::pow(10.f, L.topDb / 20.f);
    while (i < L.f0.size()) {
        const int m = midiOf(L.f0[i]); if (m < 0) { ++i; continue; }
        size_t j = i; double lev = 0;
        while (j < L.f0.size() && midiOf(L.f0[j]) == m) { lev += std::pow(10.0, L.levelDb[j] / 20.0); ++j; }
        if ((j - i) >= (size_t)(s.noteMinS * 100)) L.notes.push_back({ i * L.hopL, (j - i) * L.hopL, m, (float)std::clamp(lev / (j - i) / (topLin + 1e-12) * 1.5, 0.3, 1.0) });
        i = j;
    }
}

// Phrase starts are found on the 10 ms level grid, after the voice has crossed the activity threshold. Refine each to
// the sample where the voice actually rises out of the gap (1 ms envelope above -24 dB of the phrase's opening peak),
// so anything that lands on an entrance (reverse reverb, Pre roll) hits with the voice, not after it.
inline void refineEntrances(const Stereo& x, TrackLearn& L) {
    const int sr = L.sr; const double a = std::exp(-1.0 / (0.001 * sr)); size_t prevEnd = 0;
    for (size_t k = 0; k < L.phrases.size(); ++k) {
        auto& p = L.phrases[k]; const size_t w0 = std::max(prevEnd, p.a > (size_t)(0.12 * sr) ? p.a - (size_t)(0.12 * sr) : 0), w1 = std::min(x.size(), p.a + (size_t)(0.2 * sr));
        std::vector<float> env(w1 - w0); double y = 0, pk = 0;
        for (size_t i = w0; i < w1; ++i) { y = a * y + (1 - a) * std::abs(0.5 * (x.L[i] + x.R[i])); env[i - w0] = (float)y; pk = std::max(pk, y); }
        const double th = pk * 0.063;
        for (size_t i = 0; i < env.size(); ++i) if (env[i] > th) { const size_t na = w0 + i; if (na < p.a + (size_t)(0.03 * sr)) p.a = na; break; }
        prevEnd = p.b;
    }
    L.gaps.clear(); size_t prev = 0;
    for (auto& p : L.phrases) { if (p.a > prev) L.gaps.push_back({ prev, p.a }); prev = p.b; }
    if (!L.phrases.empty() && prev < x.size()) L.gaps.push_back({ prev, x.size() });
}

// ---------------------------------------------------------------- triggers: which gaps fire the effects, and how hard
// Measured per gap from the learned frames: the line before it (length, loudness of its last word, whether it ends on a
// held sung note) and the entrance after it (how hard the voice comes back). Effects then only react to the gaps that
// pass, each scaled by its weight (Dynamics: the effect follows the loudness of the word or entrance it reacts to).
struct TriggerSettings {
    double minPhraseS = 0;        // only after lines at least this long
    double minEntranceDb = 0;     // only when the voice comes back at least this far above the gap
    bool sustainedOnly = false;   // only when the line ends on a held, sung note
    int everyN = 1;               // every Nth gap that passes
    double probability = 1;       // chance per gap (repeatable: the same seed gives the same choice)
    double dynamics = 0;          // 0 = every event full level .. 1 = follows the loudness of its word/entrance
    uint32_t seed = 7;
};
struct GapInfo { double phraseS = 0, lastWordDb = -240, entranceDb = 0; bool sustained = false; };
inline GapInfo gapInfo(const TrackLearn& L, size_t gi) {
    GapInfo g; const auto gap = L.gaps[gi]; const size_t hop = (size_t)L.hopL;
    auto meanDb = [&](size_t a, size_t b) { double e = 0; size_t n = 0; for (size_t t = a / hop; t < std::min(L.levelDb.size(), b / hop); ++t, ++n) e += std::pow(10.0, L.levelDb[t] / 10); return n ? 10 * std::log10(e / n + 1e-24) : -240.0; };
    for (auto& p : L.phrases) if (p.b == gap.a) {
        g.phraseS = p.len() / (double)L.sr;
        const size_t w = (size_t)(0.2 * L.sr), a = p.b > w ? p.b - w : p.a; g.lastWordDb = meanDb(std::max(a, p.a), p.b) - L.topDb;
        const size_t ps = (size_t)(0.15 * L.sr), pa = p.b > ps ? p.b - ps : p.a; size_t v = 0, n = 0;
        for (size_t t = std::max(pa, p.a) / hop; t < std::min(L.f0.size(), p.b / hop); ++t, ++n) v += L.f0[t] > 0;
        g.sustained = n && v >= 0.6 * n;
    }
    const size_t e = (size_t)(0.1 * L.sr), gm = gap.len() > (size_t)(0.06 * L.sr) ? (size_t)(0.03 * L.sr) : 0;
    g.entranceDb = meanDb(gap.b, gap.b + e) - std::max(meanDb(gap.a + gm, gap.b > gm ? gap.b - gm : gap.b), (double)L.topDb - 60);   // silence floors 60 dB under the peak
    return g;
}
inline TrackLearn applyTriggers(const TrackLearn& in, const TriggerSettings& s) {
    TrackLearn L = in; L.gaps.clear(); L.gapWeight.clear();
    std::mt19937 rng(s.seed); int passed = 0;
    for (size_t gi = 0; gi < in.gaps.size(); ++gi) {
        const auto g = gapInfo(in, gi); const bool leadIn = in.gaps[gi].a == 0; const double u = rng() / 4294967296.0;
        if (!leadIn && g.phraseS < s.minPhraseS) continue;
        const bool hasEntrance = in.gaps[gi].b + (size_t)in.hopL < in.levelDb.size() * (size_t)in.hopL;
        if (s.minEntranceDb > 0 && (!hasEntrance || g.entranceDb < s.minEntranceDb)) continue;
        if (s.sustainedOnly && !leadIn && !g.sustained) continue;
        if (u > s.probability) continue;
        if ((passed++ % std::max(1, s.everyN)) != 0) continue;
        const double ref = leadIn ? std::min(0.0, g.entranceDb - 30) : std::min(0.0, g.lastWordDb);   // dB below the loudest
        L.gaps.push_back(in.gaps[gi]); L.gapWeight.push_back((float)std::pow(10.0, s.dynamics * ref / 20));
    }
    return L;
}
inline TrackLearn learnTrack(const Stereo& x, int sr, const LearnSettings& s = {}) { TrackLearn L; learnFrames(x, sr, L); deriveTrack(L, s, x.size()); refineEntrances(x, L); return L; }
inline void relearn(const Stereo& x, int sr, TrackLearn& L, Range r, const LearnSettings& s = {}) {
    learnFrames(x, sr, L, r); deriveTrack(L, s, x.size()); refineEntrances(x, L);
    AM_LOG(Debug) << "relearned " << r.a / (double)sr << "-" << r.b / (double)sr << " s: " << L.phrases.size() << " phrases, " << L.onsets.size() << " onsets";
}

// Session tempo from the summed consonant/level rises of the given tracks (BPM 70..180, the strongest periodicity).
inline double learnTempo(const std::vector<const TrackLearn*>& tracks) {
    if (tracks.empty()) return 120.0;
    size_t n = 0; for (auto* t : tracks) n = std::max(n, t->levelDb.size());
    std::vector<double> o(n, 0.0);
    for (auto* t : tracks) for (size_t i = 1; i < t->levelDb.size(); ++i) o[i] += std::max(0.f, t->levelDb[i] - t->levelDb[i - 1]);
    double mean = 0; for (auto v : o) mean += v; mean /= std::max<size_t>(1, n); for (auto& v : o) v -= mean;
    double best = -1e300, bpm = 120;
    for (int lag = (int)(6000 / 180.0); lag <= (int)(6000 / 70.0); ++lag) {
        double c = 0; for (size_t i = 0; i + lag < n; ++i) c += o[i] * o[i + lag];
        if (c > best) { best = c; bpm = 6000.0 / lag; }
    }
    return bpm;
}

// Key from a chroma profile (Krumhansl-Schmuckler). Returns tonic pitch class (0 = C) and minor flag.
struct Key { int tonic = 0; bool minor = false; double score = 0; std::string name() const {
    static const char* n[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }; return std::string(n[tonic]) + (minor ? " minor" : " major"); } };
inline Key learnKey(const std::vector<float>& mono, int sr) {
    const int N = 8192; const auto S = stft(mono, N, N / 2);
    std::array<double, 12> pc{};
    for (auto& fr : S.frames)
        for (int k = 1; k < N / 2; ++k) {
            const double f = (double)k * sr / N; if (f < 60 || f > 2000) continue;
            const int p = (((int)std::lround(12 * std::log2(f / 440.0)) % 12) + 12 + 9) % 12;   // 0 = C
            pc[(size_t)p] += std::norm(fr[(size_t)k]);
        }
    static const double maj[] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double mnr[] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
    auto corr = [&](const double* prof, int k) {
        double ma = 0, mb = 0; for (int i = 0; i < 12; ++i) { ma += pc[(size_t)i]; mb += prof[i]; } ma /= 12; mb /= 12;
        double sab = 0, saa = 0, sbb = 0;
        for (int i = 0; i < 12; ++i) { const double x = pc[(size_t)((i + k) % 12)] - ma, y = prof[i] - mb; sab += x * y; saa += x * x; sbb += y * y; }
        return sab / std::sqrt(saa * sbb + 1e-30);
    };
    Key best; best.score = -2;
    for (int k = 0; k < 12; ++k) { const double a = corr(maj, k), b = corr(mnr, k); if (a > best.score) best = { k, false, a }; if (b > best.score) best = { k, true, b }; }
    return best;
}

} // namespace am::studio
