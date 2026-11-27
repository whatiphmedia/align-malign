// Auto Malign Studio (standalone session core): WAV, learning (incl. re-learning a section), doubles, effects,
// project state, presets, undo and the debug log.
#include "am/studio/studio.h"
#include <cstdio>
using namespace am::studio;

static int fails = 0;
static void check(bool ok, const char* what) { std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; }

// a sung-like test voice: harmonic tone with vibrato, consonant bursts at each phrase start, phrases with gaps
static Stereo voice(int sr, double seconds, const std::vector<std::pair<double, double>>& phrases, double f0, double delayS = 0, uint32_t seed = 1, double pan = 0) {
    const size_t n = (size_t)(seconds * sr); Stereo s(n); std::mt19937 rng(seed); std::normal_distribution<double> g(0, 1);
    for (auto [a, b] : phrases) {
        const size_t i0 = (size_t)((a + delayS) * sr), i1 = std::min(n, (size_t)((b + delayS) * sr)); double ph = 0;
        for (size_t i = i0; i < i1; ++i) {
            const double t = (double)(i - i0) / sr, f = f0 * (1 + 0.004 * std::sin(2 * M_PI * 5 * t)); ph += 2 * M_PI * f / sr;
            double v = 0; for (int h = 1; h <= 8; ++h) v += std::sin(h * ph) / h;
            const double env = std::min({ 1.0, t / 0.02, (double)(i1 - i) / (0.03 * sr) });
            double burst = 0; for (double c = 0; c < b - a; c += 0.25) { const double d = t - c; if (d >= 0 && d < 0.03) burst += g(rng) * 0.6 * (1 - d / 0.03); }
            const float x = (float)(0.2 * (v * env + burst));
            s.L[i] = x * (float)std::min(1.0, 1 - pan); s.R[i] = x * (float)std::min(1.0, 1 + pan);
        }
    }
    return s;
}

int main() {
    const int sr = 44100;
    const std::string tmp = std::filesystem::temp_directory_path().string() + "/";   // /tmp does not exist on Windows
    Log::get().setLevel(Log::Info);
    {   // 1. WAV round trip (24-bit and float)
        Stereo s(1000); for (size_t i = 0; i < 1000; ++i) { s.L[i] = (float)std::sin(i * 0.01) * 0.5f; s.R[i] = -s.L[i]; }
        writeWav(tmp + "am_t24.wav", s, sr); writeWav(tmp + "am_tf.wav", s, sr, true);
        Stereo a, b; WavInfo ia, ib; const bool ok = readWav(tmp + "am_t24.wav", a, ia) && readWav(tmp + "am_tf.wav", b, ib);
        double e24 = 0, ef = 0; for (size_t i = 0; i < 1000 && ok; ++i) { e24 = std::max(e24, (double)std::abs(a.L[i] - s.L[i])); ef = std::max(ef, (double)std::abs(b.R[i] - s.R[i])); }
        std::printf("    24-bit error %.2e, float error %.2e\n", e24, ef);
        check(ok && ia.bits == 24 && ib.isFloat && e24 < 1e-6 && ef == 0, "WAV: 24-bit and float files round trip");
    }
    const std::vector<std::pair<double, double>> ph = { { 0.5, 1.7 }, { 2.2, 3.4 }, { 4.0, 5.5 } };
    const auto lead = voice(sr, 6.5, ph, 220, 0, 1), dub = voice(sr, 6.5, ph, 220, 0.025, 2, -0.6);
    {   // 2. learning: phrases, gaps, onsets, notes
        const auto L = learnTrack(lead, sr);
        std::printf("    phrases %zu, gaps %zu, onsets %zu, notes %zu\n", L.phrases.size(), L.gaps.size(), L.onsets.size(), L.notes.size());
        bool ph3 = L.phrases.size() == 3; for (size_t i = 0; ph3 && i < 3; ++i) ph3 &= std::abs(L.phrases[i].a / (double)sr - ph[i].first) < 0.05;
        int midiOk = 0; for (auto& n : L.notes) midiOk += n.midi == 57;
        check(ph3 && L.gaps.size() == 4, "learn: three phrases found where they are; gaps: the lead-in, between them, and the tail");
        check(L.onsets.size() >= 9 && midiOk >= 2, "learn: consonant onsets and the sung note (A3) found");
    }
    {   // 2b. entrances are sample-accurate: within 5 ms of where the voice starts
        const auto L = learnTrack(lead, sr); double worst = 0;
        for (size_t k = 0; k < 3; ++k) worst = std::max(worst, std::abs(L.phrases[k].a / (double)sr - ph[k].first) * 1000);
        std::printf("    worst entrance error %.1f ms\n", worst);
        check(worst < 5, "learn: each phrase entrance is placed within 5 ms of where the voice starts");
    }
    {   // 3. re-learning only a section leaves the rest untouched
        auto L = learnTrack(lead, sr); auto edited = lead;
        for (size_t i = (size_t)(4.0 * sr); i < (size_t)(5.5 * sr); ++i) { edited.L[i] = 0; edited.R[i] = 0; }   // the last phrase is removed
        const auto before = L.levelDb; relearn(edited, sr, L, { (size_t)(3.9 * sr), (size_t)(5.6 * sr) });
        size_t changedOutside = 0; for (size_t t = 0; t < (size_t)(3.5 * 100); ++t) changedOutside += L.levelDb[t] != before[t];
        const auto full = learnTrack(edited, sr);
        std::printf("    after re-learning 3.9-5.6 s: %zu phrases (full re-learn: %zu); frames changed before 3.5 s: %zu\n", L.phrases.size(), full.phrases.size(), changedOutside);
        check(L.phrases.size() == 2 && full.phrases.size() == 2 && changedOutside == 0, "re-learn section: only that section is recomputed, and the result matches a full re-learn");
    }
    {   // 4. doubles: the flam fix finds the 25 ms lateness, and the split removes the mono comb
        const auto LL = learnTrack(lead, sr), LD = learnTrack(dub, sr);
        auto mono = [](const Stereo& s) { std::vector<float> m(s.size()); for (size_t i = 0; i < s.size(); ++i) m[i] = s.L[i] + s.R[i]; return m; };
        FlamReport rep; const auto fixedDub = flamFix(mono(dub), mono(lead), LL, LD, sr, 1.0, &rep);
        std::printf("    flam fix: overall %.0f ms, %d/%d syllables, alignment %.2f -> %.2f\n", rep.overallMs, rep.moved, rep.considered, rep.alignBefore, rep.alignAfter);
        check(std::abs(rep.overallMs - 25) <= 5 && rep.alignAfter > rep.alignBefore + 0.2, "flam fix: finds the double's 25 ms lateness and lines the consonants up");
        const auto lm = mono(lead), dm = mono(dub); const auto sp = split(dm, lm);   // split alone, on the late double
        // share of the stack's energy that cancels by more than 6 dB when lead and double are summed (per time-frequency bin)
        auto cancelShare = [&](const std::vector<float>& d) {
            std::vector<float> sum(lm.size()); for (size_t i = 0; i < sum.size(); ++i) sum[i] = lm[i] + d[i];
            const auto S = stft(sum), A = stft(lm), D = stft(d); double cancelled = 0, total = 0;
            for (size_t t = 0; t < S.frames.size(); ++t) for (size_t k = 1; k < (size_t)S.bins(); ++k) {
                const double p = std::norm(A.frames[t][k]) + std::norm(D.frames[t][k]); total += p; if (std::norm(S.frames[t][k]) < p * 0.25) cancelled += p; }
            return 100 * cancelled / (total + 1e-30); };
        std::vector<float> rebuilt(sp.common.size()); for (size_t i = 0; i < rebuilt.size(); ++i) rebuilt[i] = sp.common[i] + sp.difference[i];
        const double cRaw = cancelShare(dm), cSplit = cancelShare(rebuilt);
        std::printf("    mono cancellation of lead + double: raw %.1f%%, after the split %.1f%%\n", cRaw, cSplit);
        check(cSplit < cRaw * 0.5, "split: the mono sum with the lead no longer combs");
    }
    {   // 5. roll: a Pre roll ends exactly on the entrance; the reverse reverb ends there too
        const auto L = learnTrack(lead, sr); RollSettings rs; rs.mode = RollSettings::Pre;
        const auto r = rollFx(lead, L, rs, 120, sr); const size_t ret = L.gaps[1].b;
        double before = 0, after = 0; for (size_t i = ret - sr / 20; i < ret; ++i) before += r.L[i] * r.L[i]; for (size_t i = ret + sr / 50; i < ret + sr / 10; ++i) after += r.L[i] * r.L[i];
        check(before > 0 && after == 0, "roll: Pre roll fills the gap and stops exactly at the entrance");
        const auto rv = reverseReverbFx(lead, L, {}, sr); double pre = 0, post = 0;
        for (size_t i = ret - sr / 5; i < ret; ++i) pre += rv.L[i] * rv.L[i]; for (size_t i = ret + 10; i < ret + sr / 5; ++i) post += rv.L[i] * rv.L[i];
        check(pre > 0 && post < pre * 1e-3, "reverse reverb: swells up to the entrance and stops there");
        ReverseReverbSettings early; early.landMs = -100; const auto rv2 = reverseReverbFx(lead, L, early, sr); double lastBit = 0, before2 = 0;
        for (size_t i = ret - (size_t)(0.09 * sr); i < ret; ++i) lastBit += rv2.L[i] * rv2.L[i]; for (size_t i = ret - (size_t)(0.4 * sr); i < ret - (size_t)(0.11 * sr); ++i) before2 += rv2.L[i] * rv2.L[i];
        check(before2 > 0 && lastBit < before2 * 1e-3, "reverse reverb: Landing -100 ms ends the swell 100 ms before the voice");
    }
    {   // 6. throws live in the gaps; freeze only in the gaps
        const auto L = learnTrack(lead, sr); const auto th = throwsFx(lead, L, {}, 120, sr);
        double inGap = 0, inPhrase = 0; for (auto& g : L.gaps) if (g.a) for (size_t i = g.a; i < g.b; ++i) inGap += th.L[i] * th.L[i] + th.R[i] * th.R[i];
        for (size_t k = 1; k < L.phrases.size(); ++k) for (size_t i = L.phrases[k].a + sr / 5; i < L.phrases[k].b; ++i) inPhrase += th.L[i] * th.L[i] + th.R[i] * th.R[i];
        std::printf("    throws: gap energy %.3g, during the next phrases %.3g\n", inGap, inPhrase);
        check(inGap > 1000 * inPhrase, "throws: echoes of each phrase end live in the gap and are gone when the voice returns");
        const auto fz = freezeFx(lead, L, {}); double fin = 0, fout = 0;
        for (auto& p : L.phrases) for (size_t i = p.a + sr / 10; i < p.b - sr / 10; ++i) fin += fz.L[i] * fz.L[i];
        for (auto& g : L.gaps) if (g.a) for (size_t i = g.a + sr / 20; i < g.b; ++i) fout += fz.L[i] * fz.L[i];
        check(fout > 20 * fin, "freeze: holds the voice in the gaps, silent while it sings");
    }
    {   // 6b. call and response: tight to the gap by default; with bleed it carries on under the voice
        const auto L = learnTrack(lead, sr); ResponseSettings tight; ResponseSettings bleed; bleed.bleed = 0.5;
        const auto a = responseFx(lead, L, tight, 120, sr), b = responseFx(lead, L, bleed, 120, sr);
        auto over = [&](const Stereo& y) { double e = 0; for (size_t k = 1; k < L.phrases.size(); ++k) for (size_t i = L.phrases[k].a + sr / 20; i < L.phrases[k].a + sr / 3; ++i) e += y.L[i] * y.L[i] + y.R[i] * y.R[i]; return e; };
        auto inGap = [&](const Stereo& y) { double e = 0; for (auto& g : L.gaps) if (g.a) for (size_t i = g.a; i < g.b; ++i) e += y.L[i] * y.L[i] + y.R[i] * y.R[i]; return e; };
        std::printf("    response: tight %.3g in the gaps / %.3g over the voice; bleed 0.5: %.3g over the voice\n", inGap(a), over(a), over(b));
        check(inGap(a) > 0 && over(a) == 0 && over(b) > 0, "call and response: tight to the gap by default, carries on over the voice with Bleed");
    }
    {   // 6c. triggers: which gaps fire, and how hard
        auto L = learnTrack(lead, sr); const size_t all = L.gaps.size();
        TriggerSettings every2; every2.everyN = 2; TriggerSettings longLines; longLines.minPhraseS = 1.4; TriggerSettings strong; strong.minEntranceDb = 61; TriggerSettings dyn; dyn.dynamics = 1;
        const auto a = applyTriggers(L, every2), b = applyTriggers(L, longLines), c = applyTriggers(L, strong), d = applyTriggers(L, dyn);
        float wmin = 1; for (auto w : d.gapWeight) wmin = std::min(wmin, w);
        std::printf("    triggers: all %zu gaps; every 2nd %zu; after lines >= 1.4 s %zu; entrance >= 61 dB %zu; dynamics weights min %.2f\n", all, a.gaps.size(), b.gaps.size(), c.gaps.size(), wmin);
        check(a.gaps.size() == (all + 1) / 2 && b.gaps.size() == 2 && c.gaps.size() == 0 && wmin < 1.f, "triggers: every Nth, minimum line length, entrance strength and dynamics select and weight the gaps");
        const auto rAll = rollFx(lead, L, {}, 120, sr), rHalf = rollFx(lead, a, {}, 120, sr);
        check(rHalf.energy() < 0.8 * rAll.energy() && rHalf.energy() > 0, "triggers: an effect only reacts to the gaps that pass");
    }
    {   // 7. pocket: only where the voice is (its frequencies, its moments), smoothly; total stereo energy kept
        Stereo beat(lead.size()); std::mt19937 rng(5); auto gn = [&] { const double u1 = (rng() + 1.0) / 4294967297.0, u2 = rng() / 4294967296.0; return 0.1 * std::sqrt(-2 * std::log(u1)) * std::cos(2 * M_PI * u2); };
        for (size_t i = 0; i < beat.size(); ++i) { beat.L[i] = (float)gn(); beat.R[i] = beat.L[i]; }
        PocketSettings full; full.amount = 1.0; const auto pb = pocketFx(beat, lead, full, sr);
        auto centre = [&](const Stereo& x, double lo, double hi, double t0, double t1) { std::vector<float> m(x.size()); for (size_t i = 0; i < m.size(); ++i) m[i] = x.L[i] + x.R[i];
            bandpass(m, lo, hi, sr); double e = 0; for (size_t i = (size_t)(t0 * sr); i < (size_t)(t1 * sr); ++i) e += m[i] * m[i]; return e; };
        const double voiceBand = 10 * std::log10(centre(pb, 400, 1800, 2.5, 3.2) / centre(beat, 400, 1800, 2.5, 3.2));
        const double highBand = 10 * std::log10(centre(pb, 9000, 14000, 2.5, 3.2) / centre(beat, 9000, 14000, 2.5, 3.2));
        const double inGap = 10 * std::log10(centre(pb, 400, 1800, 1.9, 2.05) / centre(beat, 400, 1800, 1.9, 2.05));
        double t0 = 0, t1 = 0; for (size_t i = (size_t)(2.5 * sr); i < (size_t)(3.2 * sr); ++i) { t0 += beat.L[i] * beat.L[i] + beat.R[i] * beat.R[i]; t1 += pb.L[i] * pb.L[i] + pb.R[i] * pb.R[i]; }
        // smoothness: the centre level of the voice band never jumps by much from one 10 ms step to the next
        std::vector<float> m(pb.size()); for (size_t i = 0; i < m.size(); ++i) m[i] = pb.L[i] + pb.R[i]; bandpass(m, 400, 1800, sr);
        std::vector<float> b0(beat.size()); for (size_t i = 0; i < b0.size(); ++i) b0[i] = beat.L[i] + beat.R[i]; bandpass(b0, 400, 1800, sr);
        double jump = 0, prev = 0; const size_t H = sr / 100;
        for (size_t f = 10; f + 1 < m.size() / H; ++f) { double a = 0, b = 0; for (size_t i = f * H; i < (f + 1) * H; ++i) { a += m[i] * m[i]; b += b0[i] * b0[i]; }
            const double r = 10 * std::log10(a / (b + 1e-20) + 1e-20); if (f > 10) jump = std::max(jump, std::abs(r - prev)); prev = r; }
        std::printf("    pocket: voice band centre %+.1f dB while singing, %+.1f dB in the gap; 9-14 kHz %+.1f dB; stereo total %+.2f dB; largest 10 ms step %.1f dB\n", voiceBand, inGap, highBand, 10 * std::log10(t1 / t0), jump);
        check(voiceBand < -2 && std::abs(highBand) < 0.3 && std::abs(10 * std::log10(t1 / t0)) < 0.5, "pocket: only the voice's frequencies, while it sings; total stereo energy unchanged");
        check(jump < 3.5, "pocket: smooth attack and release (no snapping)");
    }
    {   // 8. project: params, presets, JSON, undo/redo, debug log
        ProjectState s; s.tracks.push_back({ "lead", "/x/lead.wav" }); s.tracks[0].role = TrackState::Lead;
        setParam(s, "flamAmount", 0.4); applyPreset(s, "Inside-out shimmer");
        ProjectState t; const bool ok = fromJson(toJson(s), t);
        check(ok && t.flamAmount == 0.4 && t.shimmerOn == 1 && t.tracks.size() == 1 && t.tracks[0].role == TrackState::Lead && t.tracks[0].path == "/x/lead.wav", "session file: parameters, preset values and tracks survive saving and loading");
        Studio st; st.state = s;
        st.set("agreement", 0.3); st.set("agreement", 0.2); st.history.breakMerge(); st.set("pocketOn", 1); st.applyPresetNamed("Doubles: tight");
        const bool u1 = st.undo() && st.state.pocketOn == 1 && st.state.agreement == 0.2;
        const bool u2 = st.undo() && st.state.pocketOn == 0;
        const bool u3 = st.undo() && st.state.agreement == 1.0;   // the two agreement moves were one step (one knob drag)
        const bool r1 = st.redo() && st.state.agreement == 0.2;
        check(u1 && u2 && u3 && r1, "undo / redo: presets and knob moves undo in order; one knob drag is one step");
        std::vector<std::string> lines; const long last = Log::get().since(0, lines); bool sawUndo = false; for (auto& l : lines) sawUndo |= l.find("undo: Preset: Doubles: tight") != std::string::npos;
        check(last > 0 && sawUndo, "debug log: actions are logged with timestamps and can be read back");
    }
    {   // 9. a whole session from files: load, learn, roles, render, export
        std::filesystem::create_directories(tmp + "am_sess"); writeWav(tmp + "am_sess/song lead.wav", lead, sr); writeWav(tmp + "am_sess/song dub L.wav", dub, sr);
        Stereo beat(lead.size()); for (size_t i = 0; i < beat.size(); ++i) { const double t = (double)i / sr; const double ph = std::fmod(t, 0.5); beat.L[i] = beat.R[i] = (float)(0.5 * std::exp(-ph * 30) * std::sin(2 * M_PI * 60 * t)); }
        writeWav(tmp + "am_sess/song instrumental.wav", beat, sr);
        Studio st; const bool loaded = st.loadFolder(tmp + "am_sess"); st.learnAll();
        int leads = 0, doubles = 0, beats = 0; for (auto& t : st.state.tracks) { leads += t.role == TrackState::Lead; doubles += t.role == TrackState::Double; beats += t.role == TrackState::Beat; }
        std::printf("%s", st.summary().c_str());
        check(loaded && leads == 1 && doubles == 1 && beats == 1 && std::abs(st.bpm() - 120) < 3, "session: stems load, the double is found with its lead, the beat by name, tempo 120 BPM");
        st.applyPresetNamed("Doubles: wide and mono-safe"); st.set("rollOn", 1); st.set("rrOn", 1);
        const auto R = st.render(); const bool ex = st.exportAll(tmp + "am_sess_out", R);
        bool finite = true; for (size_t i = 0; i < R.mix.size(); ++i) finite &= std::isfinite(R.mix.L[i]) && std::isfinite(R.mix.R[i]);
        check(ex && finite && R.mix.peak() > 0 && std::filesystem::exists(tmp + "am_sess_out/Mix.wav") && std::filesystem::exists(tmp + "am_sess_out/FX Roll.wav"), "render + export: processed stems, effect layers and the mix are written");
    }
    std::printf(fails ? "%d FAILED\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
