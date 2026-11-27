// Auto Malign Studio: the standalone session. Load stems, learn them, align/malign, audition, export.
// Every edit goes through set()/setTrack()/applyPreset() so it is undoable; render() rebuilds the audio from the state.
#pragma once
#include "../phantom.h"
#include "doubles.h"
#include "project.h"
#include "undo.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace am::studio {

struct RenderResult {
    Stereo mix, original;                       // processed mix, and the untouched stems summed (A/B)
    std::vector<Stereo> stems;                  // each track after its processing (dry)
    std::vector<std::pair<std::string, Stereo>> layers;   // effect layers and native returns
};

class Studio {
public:
    int sr = 44100;
    std::vector<Stereo> audio;                  // the stems as loaded
    std::vector<TrackLearn> learned;
    TrackLearn vocalsLearned;                   // the vocal stack together (phrases, gaps, notes)
    double learnedBpm = 120; Key learnedKey;
    ProjectState state;
    UndoStack<ProjectState> history;
    LearnSettings learnSettings;

    size_t length() const { return audio.empty() ? 0 : audio[0].size(); }
    double bpm() const { return state.bpm > 0 ? state.bpm : learnedBpm; }
    Key key() const { return state.keyTonic >= 0 ? Key{ state.keyTonic, state.keyMinor != 0, 1 } : learnedKey; }

    // ------------------------------------------------------------ loading
    bool loadFiles(const std::vector<std::string>& paths) {
        audio.clear(); learned.clear(); state = {}; history.clear(); cache_.clear();
        int firstSr = 0;
        for (auto& p : paths) {
            Stereo s; WavInfo wi;
            if (!readWav(p, s, wi)) { AM_LOG(Warn) << "skipped " << p << ": " << wi.error; continue; }
            if (!firstSr) firstSr = wi.sampleRate;
            if (wi.sampleRate != firstSr) { s = resample(s, wi.sampleRate, firstSr); AM_LOG(Warn) << p << ": resampled " << wi.sampleRate << " -> " << firstSr << " Hz"; }
            TrackState t; t.path = p; t.name = std::filesystem::path(p).stem().string(); t.role = guessRoleByName(t.name);
            audio.push_back(std::move(s)); state.tracks.push_back(t);
            AM_LOG(Info) << "loaded " << t.name << " (" << wi.channels << " ch, " << wi.bits << " bit, " << wi.frames / (double)wi.sampleRate << " s)";
        }
        if (audio.empty()) { AM_LOG(Error) << "no usable audio files"; return false; }
        sr = firstSr; size_t n = 0; for (auto& a : audio) n = std::max(n, a.size()); for (auto& a : audio) a.resize(n);
        trimCommonPrefix();
        return true;
    }
    bool loadFolder(const std::string& dir) {
        std::vector<std::string> files;
        for (auto& e : std::filesystem::directory_iterator(dir)) {
            if (!e.is_regular_file()) continue; auto ext = e.path().extension().string(); for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext == ".wav" || ext == ".wave") files.push_back(e.path().string());
        }
        std::sort(files.begin(), files.end());
        AM_LOG(Info) << "folder " << dir << ": " << files.size() << " WAV files";
        return loadFiles(files);
    }

    // ------------------------------------------------------------ learning
    void learnAll() {
        learned.resize(audio.size());
        for (size_t i = 0; i < audio.size(); ++i) {
            learned[i] = learnTrack(audio[i], sr, learnSettings);
            AM_LOG(Info) << "learned " << state.tracks[i].name << ": " << learned[i].phrases.size() << " phrases, " << learned[i].onsets.size() << " onsets, " << learned[i].notes.size() << " notes, active " << (int)(100 * learned[i].activeShare()) << "%";
        }
        autoRoles(); learnSession();
    }
    // Re-learn only a section (samples) of one track, or of all tracks (track = -1). Effects that depend on it re-render.
    void relearnSection(Range r, int track = -1) {
        if (learned.size() != audio.size()) { learnAll(); return; }
        r.b = std::min(r.b, length());
        for (size_t i = 0; i < audio.size(); ++i) if (track < 0 || (int)i == track) relearn(audio[i], sr, learned[i], r, learnSettings);
        learnSession(r); cache_.clear();
        AM_LOG(Info) << "re-learned " << r.a / (double)sr << "-" << r.b / (double)sr << " s" << (track >= 0 ? " on " + state.tracks[(size_t)track].name : " on all tracks");
    }

    // ------------------------------------------------------------ edits (all undoable)
    bool set(const std::string& id, double v, bool merge = true) {
        auto* p = findParam(id); if (!p) { AM_LOG(Warn) << "unknown parameter " << id; return false; }
        if (p->get(state) == std::clamp(v, p->min, p->max)) return true;
        history.push(state, p->label, merge ? id : std::string());
        p->set(state, v); AM_LOG(Debug) << "set " << id << " = " << v; return true;
    }
    bool setTrack(int i, const std::string& field, double v) {
        if (i < 0 || i >= (int)state.tracks.size()) return false;
        auto& t = state.tracks[(size_t)i]; history.push(state, t.name + ": " + field, "track" + std::to_string(i) + field);
        if (field == "gainDb") t.gainDb = v; else if (field == "pan") t.pan = std::clamp(v, -1.0, 1.0); else if (field == "mute") t.mute = v > 0.5; else if (field == "solo") t.solo = v > 0.5;
        else if (field == "role") t.role = (int)v; else if (field == "ref") t.ref = (int)v; else if (field == "sendRevDb") t.sendRevDb = v; else if (field == "sendDlyDb") t.sendDlyDb = v;
        else { AM_LOG(Warn) << "unknown track field " << field; return false; }
        AM_LOG(Debug) << "track " << t.name << " " << field << " = " << v; return true;
    }
    // After role changes the vocal stack is a different set of tracks: learn it again (worker thread).
    void relearnVocals() { learnSession(); }
    bool applyPresetNamed(const std::string& name) {
        ProjectState before = state; if (!applyPreset(state, name)) { AM_LOG(Warn) << "unknown preset " << name; return false; }
        history.push(before, "Preset: " + name); history.breakMerge(); AM_LOG(Info) << "preset " << name; return true;
    }
    bool undo() { const auto l = history.undoLabel(); if (!history.undo(state)) return false; AM_LOG(Info) << "undo: " << l; return true; }
    bool redo() { const auto l = history.redoLabel(); if (!history.redo(state)) return false; AM_LOG(Info) << "redo: " << l; return true; }

    // ------------------------------------------------------------ save / load the session file
    bool saveProject(const std::string& path) const { std::ofstream f(path); f << toJson(state); AM_LOG(Info) << "saved " << path; return (bool)f; }
    bool loadProject(const std::string& path) {
        std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); ProjectState s;
        if (!fromJson(ss.str(), s)) { AM_LOG(Error) << "cannot read session " << path; return false; }
        std::vector<std::string> paths; for (auto& t : s.tracks) paths.push_back(t.path);
        if (!loadFiles(paths)) return false;
        for (size_t i = 0; i < s.tracks.size() && i < state.tracks.size(); ++i) { auto keepName = state.tracks[i].name; state.tracks[i] = s.tracks[i]; if (state.tracks[i].name.empty()) state.tracks[i].name = keepName; }
        auto tracks = state.tracks; state = s; state.tracks = tracks; learnAll();
        for (size_t i = 0; i < s.tracks.size() && i < state.tracks.size(); ++i) { state.tracks[i].role = s.tracks[i].role; state.tracks[i].ref = s.tracks[i].ref; }
        history.clear(); AM_LOG(Info) << "opened session " << path; return true;
    }

    // ------------------------------------------------------------ render
    RenderResult render() { return render(state); }
    // Renders from a snapshot of the state, so an app can keep editing `state` on its UI thread while a worker renders.
    // (Caches and learned data are only touched by the thread that renders/learns.)
    RenderResult render(const ProjectState& snap) {
        RenderResult R; const size_t n = length(); const ProjectState& s = snap;
        if (cache_.size() > 64) cache_.clear();
        R.mix = Stereo(n); R.original = Stereo(n); R.stems.assign(audio.size(), Stereo(n));
        bool anySolo = false; for (auto& t : s.tracks) anySolo |= t.solo;
        auto audible = [&](size_t i) { return !s.tracks[i].mute && (!anySolo || s.tracks[i].solo); };
        for (size_t i = 0; i < audio.size(); ++i) if (audible(i)) R.original.add(audio[i]);
        const float master = (float)dbToGain(s.masterDb); R.original.scale(master);
        if (s.bypass) { R.mix = R.original; return R; }
        Stereo side(n);   // side-only content (L = +, R = -)
        // tracks with gain/pan
        for (size_t i = 0; i < audio.size(); ++i) {
            const auto& t = s.tracks[i]; if (!audible(i)) continue;
            Stereo x = audio[i]; const float g = (float)dbToGain(t.gainDb), gl = (float)(g * std::min(1.0, 1.0 - t.pan)), gr = (float)(g * std::min(1.0, 1.0 + t.pan));
            for (size_t k = 0; k < n; ++k) { x.L[k] *= gl; x.R[k] *= gr; }
            R.stems[i] = std::move(x);
        }
        // doubles: flam fix + split against their reference
        if (s.doublesOn)
            for (size_t i = 0; i < audio.size(); ++i) {
                const auto& t = s.tracks[i];
                if (t.role != TrackState::Double || t.ref < 0 || t.ref >= (int)audio.size() || !audible(i)) continue;
                const auto& src = R.stems[i]; double el = 0, er = 0; for (size_t k = 0; k < n; ++k) { el += (double)src.L[k] * src.L[k]; er += (double)src.R[k] * src.R[k]; }
                const double gl = std::sqrt(el / n), gr = std::sqrt(er / n), tot = gl + gr + 1e-12; const float pl = (float)(gl / tot), pr = (float)(gr / tot), cg = 0.5f * (pl + pr);
                auto& sp = doublesFor(s, (int)i, t.ref, s.flamAmount, src);
                Stereo y(n);
                for (size_t k = 0; k < n; ++k) {
                    const float c = sp.common[k], d = (float)(sp.difference[k] * s.agreement);
                    if (s.diffSideOnly) { y.L[k] = c * cg; y.R[k] = c * cg; side.L[k] += (pl >= pr ? 1.f : -1.f) * (float)s.diffWidth * d * cg; }
                    else { y.L[k] = c * cg + d * pl; y.R[k] = c * cg + d * pr; }
                }
                R.stems[i] = std::move(y);
            }
        // sums
        Stereo vocals(n), beat(n), revIn(n), dlyIn(n);
        for (size_t i = 0; i < audio.size(); ++i) {
            if (!audible(i)) continue; const auto& t = s.tracks[i]; const auto& x = R.stems[i];
            if (t.role == TrackState::Lead || t.role == TrackState::Double || t.role == TrackState::Other) vocals.add(x);
            if (t.role == TrackState::Beat) beat.add(x);
            if (t.sendRevDb > -100) revIn.add(x, (float)dbToGain(t.sendRevDb));
            if (t.sendDlyDb > -100) dlyIn.add(x, (float)dbToGain(t.sendDlyDb));
        }
        for (size_t k = 0; k < n; ++k) { vocals.L[k] += side.L[k]; vocals.R[k] -= side.L[k]; }
        rederiveIfNeeded(s); const auto& VL = vocalsLearned; const auto singing = singingGate(VL, n);
        // phase pocket on the beat tracks
        if (s.pocketOn && beat.energy() > 0) {
            PocketSettings ps; ps.amount = s.pocketAmount; ps.attackMs = s.pocketAttackMs; ps.releaseMs = s.pocketReleaseMs; auto pb = pocketFx(beat, vocals, ps, sr);
            for (size_t i = 0; i < audio.size(); ++i) if (audible(i) && s.tracks[i].role == TrackState::Beat) R.stems[i] = Stereo(n);
            for (size_t i = 0; i < audio.size(); ++i) if (audible(i) && s.tracks[i].role == TrackState::Beat) { R.stems[i] = pb; break; }
            R.layers.push_back({ "Phase pocket (beat)", pb });
        }
        // producer effects on the vocal source
        Stereo fxSum(n);
        const Stereo src = (s.fxSource >= 0 && s.fxSource < (int)audio.size()) ? R.stems[(size_t)s.fxSource] : vocals;
        rederiveIfNeeded(s);
        TriggerSettings trig; trig.minPhraseS = s.trigMinPhraseS; trig.minEntranceDb = s.trigMinEntranceDb; trig.sustainedOnly = s.trigSustainedOnly; trig.everyN = s.trigEveryN; trig.probability = s.trigProbability; trig.dynamics = s.trigDynamics;
        const TrackLearn SL = applyTriggers((s.fxSource >= 0 && s.fxSource < (int)learned.size()) ? learned[(size_t)s.fxSource] : vocalsLearned, trig);
        const TrackLearn VT = applyTriggers(vocalsLearned, trig);
        auto addLayer = [&](const std::string& name, Stereo y) { fxSum.add(y); R.layers.push_back({ name, std::move(y) }); };
        if (s.freezeOn) { FreezeSettings f; f.gapsOnly = s.freezeGaps; f.level = s.freezeLevel; addLayer("Freeze", freezeFx(src, SL, f)); }
        if (s.shimmerOn) { ShimmerSettings h; h.semitones = s.shimSemis; h.formant = s.shimFormant; h.tiltDbOct = s.shimTilt; h.breath = s.shimBreath; h.insideOut = s.shimInsideOut; h.level = s.shimLevel; addLayer("Shimmer", shimmerFx(src, h, sr)); }
        if (s.rollOn) addLayer("Roll", rollFx(src, SL, rollSettings(s), s.bpm > 0 ? s.bpm : learnedBpm, sr));
        if (s.rrOn) { ReverseReverbSettings rr; rr.sourceS = s.rrSourceS; rr.maxPreS = s.rrMaxPreS; rr.level = s.rrLevel; rr.decayS = s.rrDecayS; rr.landMs = s.rrLandMs; addLayer("Reverse reverb", reverseReverbFx(src, SL, rr, sr)); }
        if (s.responseOn) { ResponseSettings rs; rs.kind = (ResponseSettings::Kind)s.responseKind; rs.level = s.responseLevel; rs.attackMs = s.responseAttackMs; rs.releaseS = s.responseReleaseS; rs.bleed = s.responseBleed; rs.bleedDuckDb = s.responseDuckDb; addLayer("Call and response", responseFx(src, SL, rs, s.bpm > 0 ? s.bpm : learnedBpm, sr)); }
        if (s.ghostOn) addLayer("Ghost notes", ghostNotes(src, SL, s));
        revIn.add(fxSum, (float)dbToGain(s.fxSendRevDb));
        // native returns
        Stereo rev = reverb(s).process(revIn); rev.scale((float)dbToGain(s.revLevelDb));
        if (s.gapReverbOn) for (size_t k = 0; k < n; ++k) { const float d = (float)(1 - s.gapDuck * singing[k]); rev.L[k] *= d; rev.R[k] *= d; }
        DelaySettings ds; ds.time = { true, divFromIndex(s.dlyDiv), false, s.dlyDotted > 0.5 }; ds.feedback = s.dlyFeedback;
        const double tempo = s.bpm > 0 ? s.bpm : learnedBpm;
        Stereo dly = delayFx(dlyIn, ds, tempo, sr); dly.scale((float)dbToGain(s.dlyLevelDb));
        if (revIn.energy() > 0) R.layers.push_back({ "Reverb return", rev });
        if (dlyIn.energy() > 0) R.layers.push_back({ "Delay return", dly });
        if (s.throwsOn) {
            ThrowSettings ts; ts.delay.time = { true, divFromIndex(s.throwDiv), false, s.throwDotted > 0.5 }; ts.delay.feedback = s.throwFeedback; ts.level = s.throwLevel; ts.falloff = (ThrowSettings::Falloff)s.throwFalloff; ts.fadeS = s.throwFadeS; ts.darken = s.throwDarken > 0.5;
            Stereo th = throwsFx(vocals, VT, ts, tempo, sr); R.layers.push_back({ "Delay throws", th }); R.mix.add(th);
        }
        // mix
        for (size_t i = 0; i < audio.size(); ++i) if (audible(i)) R.mix.add(R.stems[i]);
        for (size_t k = 0; k < n; ++k) { R.mix.L[k] += side.L[k]; R.mix.R[k] -= side.L[k]; }
        R.mix.add(fxSum); R.mix.add(rev); R.mix.add(dly); R.mix.scale(master);
        AM_LOG(Debug) << "rendered " << n / (double)sr << " s, " << R.layers.size() << " layers, peak " << gainToDb(R.mix.peak()) << " dBFS";
        return R;
    }

    bool exportAll(const std::string& dir, const RenderResult& R, bool asFloat = false) const {
        std::filesystem::create_directories(dir); bool ok = true; char nm[16];
        for (size_t i = 0; i < R.stems.size(); ++i) { std::snprintf(nm, sizeof nm, "%02d ", (int)i + 1); ok &= writeWav(dir + "/" + nm + safe(state.tracks[i].name) + ".wav", R.stems[i], sr, asFloat); }
        for (auto& l : R.layers) ok &= writeWav(dir + "/FX " + safe(l.first) + ".wav", l.second, sr, asFloat);
        ok &= writeWav(dir + "/Mix.wav", R.mix, sr, asFloat);
        LogLine(ok ? Log::Info : Log::Error) << "exported to " << dir << (ok ? "" : " (some files failed)");
        return ok;
    }

    std::string summary() const {
        std::ostringstream o; o << "tempo " << bpm() << " BPM, key " << key().name() << "\n";
        for (size_t i = 0; i < state.tracks.size(); ++i) {
            const auto& t = state.tracks[i]; static const char* rn[] = { "other", "lead", "double", "beat", "return" };
            o << "  " << i << " " << t.name << ": " << rn[std::clamp(t.role, 0, 4)];
            if (t.role == TrackState::Double && t.ref >= 0) o << " of " << state.tracks[(size_t)t.ref].name;
            o << "\n";
        }
        return o.str();
    }

private:
    struct SplitCache { std::vector<float> common, difference; };
    std::map<std::string, SplitCache> cache_;

    static std::string safe(std::string s) { for (auto& c : s) if (std::string("/\\:*?\"<>|").find(c) != std::string::npos) c = '_'; return s; }
    static Stereo resample(const Stereo& x, int from, int to) {
        const double r = (double)from / to; Stereo y((size_t)(x.size() / r));
        for (size_t i = 0; i < y.size(); ++i) { y.L[i] = sampleAt(x.L, i * r); y.R[i] = sampleAt(x.R, i * r); }
        return y;
    }
    void trimCommonPrefix() {
        // stems exported from a DAW share a name prefix ("mean w hook 2 lead ..."): drop it for display
        if (state.tracks.size() < 2) return;
        std::string p = state.tracks[0].name;
        for (auto& t : state.tracks) { size_t k = 0; while (k < p.size() && k < t.name.size() && p[k] == t.name[k]) ++k; p.resize(k); }
        const size_t cut = p.find_last_of(' '); if (cut == std::string::npos || cut < 3) return;
        for (auto& t : state.tracks) {
            if (t.name.size() > cut + 1) t.name = t.name.substr(cut + 1);
            size_t k = 0; while (k < t.name.size() && std::isdigit((unsigned char)t.name[k])) ++k;   // a leftover take/version number ("2 lead ...")
            if (k && k < t.name.size() && t.name[k] == ' ') t.name = t.name.substr(k + 1);
        }
    }
    static int guessRoleByName(std::string n) {
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        auto has = [&](const char* k) { return n.find(k) != std::string::npos; };
        if (has("reverb") || has("delay") || has("return") || has("valhalla") || has("verb")) return TrackState::Return;
        if (has("instrumental") || has("beat") || has("inst") || has("music") || has("band")) return TrackState::Beat;
        return TrackState::Other;
    }
    // Doubles detection: vocal tracks that sing the same notes at the same time are grouped; in each group the most
    // centred, most active track is the reference (Lead) and the others are Doubles of it. Octave layers form their own groups.
    void autoRoles() {
        std::vector<size_t> voc; for (size_t i = 0; i < state.tracks.size(); ++i) if (state.tracks[i].role == TrackState::Other && learned[i].activeShare() > 0.02) voc.push_back(i);
        auto centred = [&](size_t i) { double el = 0, er = 0; for (size_t k = 0; k < audio[i].size(); k += 4) { el += (double)audio[i].L[k] * audio[i].L[k]; er += (double)audio[i].R[k] * audio[i].R[k]; } return std::abs(10 * std::log10((el + 1e-20) / (er + 1e-20))); };
        // d doubles r when it sings the same notes (>= 40% of the frames both are pitched) AND mostly while r sings (>= 60% of d's active time)
        auto doubles = [&](size_t d, size_t r) {
            const auto &A = learned[d], &B = learned[r]; size_t both = 0, same = 0, act = 0, ov = 0;
            for (size_t t = 0; t < std::min(A.active.size(), B.active.size()); ++t) { if (A.active[t]) { ++act; if (B.active[t]) ++ov; }
                if (t < A.f0.size() && t < B.f0.size() && A.f0[t] > 0 && B.f0[t] > 0) { ++both; if (std::abs(1200 * std::log2(A.f0[t] / B.f0[t])) < 50) ++same; } }
            return both > 30 && (double)same / both >= 0.4 && act && (double)ov / act >= 0.6; };
        std::vector<size_t> left = voc;
        while (!left.empty()) {
            size_t ref = left[0]; double best = -1e9;
            for (auto i : left) { const double score = learned[i].activeShare() * 10 - centred(i) * 0.3; if (score > best) { best = score; ref = i; } }
            state.tracks[ref].role = TrackState::Lead; std::vector<size_t> rest;
            for (auto i : left) if (i != ref) { if (doubles(i, ref)) { state.tracks[i].role = TrackState::Double; state.tracks[i].ref = (int)ref; } else rest.push_back(i); }
            left.swap(rest);
        }
        AM_LOG(Info) << "roles:\n" << summary();
    }
    void learnSession(Range r = {}) {
        // the vocal stack together
        Stereo voc(length()); bool any = false;
        for (size_t i = 0; i < audio.size(); ++i) { const int ro = state.tracks[i].role; if (ro == TrackState::Lead || ro == TrackState::Double || ro == TrackState::Other) { voc.add(audio[i]); any = true; } }
        if (any) { if (r.len() && vocalsLearned.levelDb.size()) relearn(voc, sr, vocalsLearned, r, learnSettings); else vocalsLearned = learnTrack(voc, sr, learnSettings); vocAudio_ = voc; }
        // tempo from the beat if there is one, else from everything; key likewise
        std::vector<const TrackLearn*> tl; Stereo keySrc(length());
        for (size_t i = 0; i < audio.size(); ++i) if (state.tracks[i].role == TrackState::Beat) { tl.push_back(&learned[i]); keySrc.add(audio[i]); }
        if (tl.empty()) for (size_t i = 0; i < audio.size(); ++i) if (state.tracks[i].role != TrackState::Return) { tl.push_back(&learned[i]); keySrc.add(audio[i]); }
        learnedBpm = learnTempo(tl); learnedKey = learnKey(keySrc.mono(), sr);
        AM_LOG(Info) << "session: tempo " << learnedBpm << " BPM, key " << learnedKey.name() << ", vocal phrases " << vocalsLearned.phrases.size() << ", gaps " << vocalsLearned.gaps.size();
    }
    SplitCache& doublesFor(const ProjectState& st, int d, int ref, double flam, const Stereo& srcNow) {
        const std::string k = std::to_string(d) + "|" + std::to_string(ref) + "|" + num(flam) + "|" + num(st.tracks[(size_t)d].gainDb) + "|" + num(st.tracks[(size_t)d].pan);
        auto it = cache_.find(k); if (it != cache_.end()) return it->second;
        std::vector<float> m(srcNow.size()), rm(srcNow.size());
        for (size_t i = 0; i < m.size(); ++i) { m[i] = srcNow.L[i] + srcNow.R[i]; rm[i] = audio[(size_t)ref].L[i] + audio[(size_t)ref].R[i]; }
        FlamReport rep; auto f = flamFix(m, rm, learned[(size_t)ref], learned[(size_t)d], sr, flam, &rep);
        auto sp = split(f, rm);
        AM_LOG(Info) << "doubles: " << st.tracks[(size_t)d].name << " -> " << st.tracks[(size_t)ref].name << ": overall " << rep.overallMs << " ms, " << rep.moved << "/" << rep.considered
                     << " syllables moved, consonant alignment " << rep.alignBefore << " -> " << rep.alignAfter;
        return cache_[k] = { std::move(sp.common), std::move(sp.difference) };
    }
    RollSettings rollSettings(const ProjectState& s) const {
        RollSettings r; r.mode = (RollSettings::Mode)s.rollMode; r.level = s.rollLevel;
        r.start = s.rollStartMs > 0 ? Timing{ false, 8, false, false, s.rollStartMs } : Timing{ true, divFromIndex(s.rollStartDiv) };
        r.end = s.rollEndMs > 0 ? Timing{ false, 8, false, false, s.rollEndMs } : Timing{ true, divFromIndex(s.rollEndDiv) };
        auto& M = r.mods;
        M.pitch = { s.modPitchA, s.modPitchB, s.modPitch > 0 }; M.filterHz = { s.modFilterA, s.modFilterB, s.modFilter > 0 }; M.drive = { s.modDriveA, s.modDriveB, s.modDrive > 0 };
        M.levelDb = { s.modLevelA, s.modLevelB, s.modLevel > 0 }; M.gate = { s.modGateA, s.modGateB, s.modGate > 0 }; M.crush = { s.modCrushA, s.modCrushB, s.modCrush > 0 };
        M.stereo = { s.modStereoA, s.modStereoB, s.modStereo > 0 }; M.reverse = s.modReverse > 0;
        return r;
    }
    const Reverb& reverb(const ProjectState& s) {
        const std::string k = num(s.revDecayS) + num(s.revLowCut) + num(s.revHighCut) + num(s.revWidth);
        if (k != revKey_) { ReverbSettings rs; rs.decayS = s.revDecayS; rs.lowCutHz = s.revLowCut; rs.highCutHz = s.revHighCut; rs.width = s.revWidth; rev_.build(rs, sr); revKey_ = k; }
        return rev_;
    }
    Stereo ghostNotes(const Stereo& src, const TrackLearn& SL, const ProjectState& s) {
        PhantomNotes pn; pn.prepare(sr); PhantomParams p; p.huggins = false; p.assist = (float)s.ghostAssist; p.drive = (float)s.ghostDrive; p.constant = true; p.attackMs = 60; p.releaseMs = 1200; pn.setParams(p);
        size_t notes = 0;
        if (s.ghostFromKey) {
            const Key k = s.keyTonic >= 0 ? Key{ s.keyTonic, s.keyMinor != 0, 1 } : learnedKey; const int root = 60 + k.tonic; for (int iv : { 0, k.minor ? 3 : 4, 7 }) { pn.noteOn(root + iv, 0.8f, 0); ++notes; }
        } else for (auto& nt : SL.notes) { const int m = nt.midi + (int)std::lround(s.ghostRegister); pn.noteOn(m, nt.vel, (int64_t)nt.start); pn.noteOff(m, (int64_t)(nt.start + nt.len)); ++notes; }
        Stereo y = src; const int B = 512;
        for (size_t pos = 0; pos < y.size(); pos += B) pn.process(y.L.data() + pos, y.R.data() + pos, nullptr, nullptr, (int)std::min<size_t>(B, y.size() - pos));
        for (size_t i = 0; i < y.size(); ++i) { y.L[i] -= src.L[i]; y.R[i] -= src.R[i]; }
        y.scale((float)dbToGain(s.ghostLevelDb)); AM_LOG(Debug) << "ghost notes: " << notes << " notes"; return y;
    }
    Reverb rev_; std::string revKey_; Stereo vocAudio_; std::string deriveKey_;
    // the sensitivity and gap/phrase lengths only change what is derived from the learned frames: re-derive, no re-learn
    void rederiveIfNeeded(const ProjectState& st) {
        const std::string k = num(st.trigSensitivity) + "|" + num(st.trigMinGapS);
        if (k == deriveKey_ || learned.size() != audio.size()) return;
        learnSettings.onDb = (float)st.trigSensitivity; learnSettings.offDb = (float)st.trigSensitivity + 6; learnSettings.minGapS = (float)st.trigMinGapS;
        for (size_t i = 0; i < audio.size(); ++i) { deriveTrack(learned[i], learnSettings, audio[i].size()); refineEntrances(audio[i], learned[i]); }
        if (vocAudio_.size()) { deriveTrack(vocalsLearned, learnSettings, vocAudio_.size()); refineEntrances(vocAudio_, vocalsLearned); }
        deriveKey_ = k; AM_LOG(Info) << "triggers: sensitivity " << st.trigSensitivity << " dB, shortest gap " << st.trigMinGapS << " s -> " << vocalsLearned.gaps.size() << " gaps";
    }
};

} // namespace am::studio
