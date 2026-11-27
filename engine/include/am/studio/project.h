// Auto Malign Studio: the session state (what undo, presets and save files capture), its parameter registry, JSON.
#pragma once
#include "fx.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace am::studio {

struct TrackState {
    enum Role { Other = 0, Lead, Double, Beat, Return };
    std::string name, path;
    double gainDb = 0, pan = 0;            // on top of the stem as exported
    bool mute = false, solo = false;
    int role = Other, ref = -1;            // ref: the track a Double follows
    double sendRevDb = -120, sendDlyDb = -120;
};

struct ProjectState {
    std::vector<TrackState> tracks;
    int mode = 0;                          // 0 Mixing, 1 Producer
    double bpm = 0;                        // 0 = learned
    // division params (throwDiv, rollStartDiv, rollEndDiv, dlyDiv) hold an index: 0 = 1/1 ... 3 = 1/8 ... 6 = 1/64
    int keyTonic = -1, keyMinor = 0;       // -1 = learned
    // Mixing
    int doublesOn = 1; double flamAmount = 1.0, agreement = 1.0, diffWidth = 1.6; int diffSideOnly = 0;
    int throwsOn = 0; double throwDiv = 3, throwDotted = 1, throwFeedback = 0.5, throwLevel = 0.9; int throwFalloff = 0, throwDarken = 1; double throwFadeS = 0.15;
    int gapReverbOn = 0; double gapDuck = 0.85;
    int pocketOn = 0; double pocketAmount = 0.6, pocketAttackMs = 80, pocketReleaseMs = 350;
    // Producer (effects on the vocal source: -1 = all vocals)
    int fxSource = -1; double fxSendRevDb = -6;
    int freezeOn = 0, freezeGaps = 1; double freezeLevel = 0.7;
    int shimmerOn = 0; double shimSemis = 12, shimFormant = -2, shimTilt = 0, shimBreath = 0.3, shimLevel = 0.7; int shimInsideOut = 1;
    int rollOn = 0, rollMode = 1; double rollStartDiv = 3, rollEndDiv = 6, rollStartMs = 0, rollEndMs = 0, rollLevel = 1.0;   // *Ms > 0 overrides the division
    int modPitch = 0; double modPitchA = 0, modPitchB = 12; int modFilter = 0; double modFilterA = 400, modFilterB = 16000;
    int modDrive = 0; double modDriveA = 0, modDriveB = 0.4; int modLevel = 0; double modLevelA = -18, modLevelB = 0;
    int modGate = 0; double modGateA = 1, modGateB = 0.3; int modCrush = 0; double modCrushA = 0, modCrushB = 1;
    int modStereo = 0; double modStereoA = 0, modStereoB = 1; int modReverse = 0;
    int rrOn = 0; double rrSourceS = 0.35, rrMaxPreS = 1.6, rrLevel = 0.35, rrDecayS = 1.8, rrLandMs = -10;
    int responseOn = 0, responseKind = 0; double responseLevel = 0.7, responseAttackMs = 25, responseReleaseS = 0.15, responseBleed = 0, responseDuckDb = -6;
    int ghostOn = 0, ghostRegister = 12, ghostFromKey = 0; double ghostLevelDb = 12, ghostDrive = 0.5, ghostAssist = 0.6;
    // triggers (shared by the learned effects): what counts as a gap, which gaps fire, how hard
    double trigSensitivity = 22, trigMinGapS = 0.15, trigMinPhraseS = 0, trigMinEntranceDb = 0, trigProbability = 1, trigDynamics = 0; int trigSustainedOnly = 0, trigEveryN = 1;
    // native returns
    double revDecayS = 2.7, revLowCut = 400, revHighCut = 9000, revWidth = 0.8, revLevelDb = 0;
    double dlyDiv = 3, dlyDotted = 1, dlyFeedback = 0.35, dlyLevelDb = 0;
    // monitoring (not part of the sound you export)
    int monoCheck = 0, bypass = 0;
};

struct Param {
    std::string id, label, group; double min, max, def; bool isSwitch = false; std::vector<std::string> choices;
    std::function<double(const ProjectState&)> get; std::function<void(ProjectState&, double)> set;
};

#define AM_P(ID, LABEL, GROUP, MIN, MAX, FIELD) Param{ ID, LABEL, GROUP, MIN, MAX, (double)ProjectState{}.FIELD, false, {}, [](const ProjectState& s) { return (double)s.FIELD; }, [](ProjectState& s, double v) { s.FIELD = (decltype(s.FIELD))v; } }
#define AM_S(ID, LABEL, GROUP, FIELD) Param{ ID, LABEL, GROUP, 0, 1, (double)ProjectState{}.FIELD, true, {}, [](const ProjectState& s) { return (double)s.FIELD; }, [](ProjectState& s, double v) { s.FIELD = v > 0.5; } }
#define AM_C(ID, LABEL, GROUP, FIELD, ...) Param{ ID, LABEL, GROUP, 0, (double)(std::vector<std::string>{ __VA_ARGS__ }.size() - 1), (double)ProjectState{}.FIELD, false, { __VA_ARGS__ }, [](const ProjectState& s) { return (double)s.FIELD; }, [](ProjectState& s, double v) { s.FIELD = (int)std::lround(v); } }

inline const std::vector<Param>& params() {
    static const std::vector<Param> P = {
        AM_C("mode", "Mode", "session", mode, "Mixing", "Producer"),
        AM_P("bpm", "Tempo (0 = learned)", "session", 0, 300, bpm),
        AM_S("doublesOn", "Doubles: flam fix + split", "doubles", doublesOn),
        AM_P("flamAmount", "Flam fix", "doubles", 0, 1, flamAmount),
        AM_P("agreement", "Difference amount", "doubles", 0, 2, agreement),
        AM_S("diffSideOnly", "Difference in the side only", "doubles", diffSideOnly),
        AM_P("diffWidth", "Side width", "doubles", 0, 3, diffWidth),
        AM_S("throwsOn", "Delay throws", "throws", throwsOn),
        AM_C("throwDiv", "Throw time", "throws", throwDiv, "1/1", "1/2", "1/4", "1/8", "1/16"),
        AM_S("throwDotted", "Dotted", "throws", throwDotted),
        AM_P("throwFeedback", "Feedback", "throws", 0, 0.9, throwFeedback),
        AM_P("throwLevel", "Level", "throws", 0, 2, throwLevel),
        AM_C("throwFalloff", "When the voice returns", "throws", throwFalloff, "Gate", "Duck"), AM_P("throwFadeS", "Fade before the entrance (s)", "throws", 0.02, 1, throwFadeS), AM_S("throwDarken", "Darken as it fades", "throws", throwDarken),
        AM_S("gapReverbOn", "Gap-aware reverb", "space", gapReverbOn),
        AM_P("gapDuck", "Duck while singing", "space", 0, 1, gapDuck),
        AM_S("pocketOn", "Phase pocket", "space", pocketOn),
        AM_P("pocketAmount", "Pocket amount", "space", 0, 1, pocketAmount), AM_P("pocketAttackMs", "Pocket attack (ms)", "space", 5, 500, pocketAttackMs), AM_P("pocketReleaseMs", "Pocket release (ms)", "space", 20, 2000, pocketReleaseMs),
        AM_P("fxSource", "Effects source (-1 = all vocals)", "producer", -1, 64, fxSource),
        AM_P("fxSendRevDb", "Effects into the reverb (dB)", "producer", -120, 6, fxSendRevDb),
        AM_S("freezeOn", "Freeze", "freeze", freezeOn), AM_S("freezeGaps", "Only in the gaps", "freeze", freezeGaps), AM_P("freezeLevel", "Level", "freeze", 0, 2, freezeLevel),
        AM_S("shimmerOn", "Shimmer", "shimmer", shimmerOn), AM_P("shimSemis", "Interval (st)", "shimmer", -24, 24, shimSemis), AM_P("shimFormant", "Formant (st)", "shimmer", -12, 12, shimFormant),
        AM_P("shimTilt", "Tilt (dB/oct)", "shimmer", -6, 6, shimTilt), AM_P("shimBreath", "Breath", "shimmer", 0, 1, shimBreath), AM_S("shimInsideOut", "Inside-out", "shimmer", shimInsideOut), AM_P("shimLevel", "Level", "shimmer", 0, 2, shimLevel),
        AM_S("rollOn", "Roll", "roll", rollOn), AM_C("rollMode", "Mode", "roll", rollMode, "Post", "Pre", "Both"),
        AM_C("rollStartDiv", "Start", "roll", rollStartDiv, "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64"),
        AM_C("rollEndDiv", "End", "roll", rollEndDiv, "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64"),
        AM_P("rollStartMs", "Start in ms (0 = tempo)", "roll", 0, 2000, rollStartMs), AM_P("rollEndMs", "End in ms (0 = tempo)", "roll", 0, 2000, rollEndMs), AM_P("rollLevel", "Level", "roll", 0, 2, rollLevel),
        AM_S("modPitch", "Pitch", "roll mods", modPitch), AM_P("modPitchA", "Pitch start (st)", "roll mods", -24, 24, modPitchA), AM_P("modPitchB", "Pitch end (st)", "roll mods", -24, 24, modPitchB),
        AM_S("modFilter", "Filter", "roll mods", modFilter), AM_P("modFilterA", "Filter start (Hz)", "roll mods", 100, 20000, modFilterA), AM_P("modFilterB", "Filter end (Hz)", "roll mods", 100, 20000, modFilterB),
        AM_S("modDrive", "Drive", "roll mods", modDrive), AM_P("modDriveA", "Drive start", "roll mods", 0, 1, modDriveA), AM_P("modDriveB", "Drive end", "roll mods", 0, 1, modDriveB),
        AM_S("modLevel", "Level", "roll mods", modLevel), AM_P("modLevelA", "Level start (dB)", "roll mods", -40, 6, modLevelA), AM_P("modLevelB", "Level end (dB)", "roll mods", -40, 6, modLevelB),
        AM_S("modGate", "Gate", "roll mods", modGate), AM_P("modGateA", "Gate start", "roll mods", 0.05, 1, modGateA), AM_P("modGateB", "Gate end", "roll mods", 0.05, 1, modGateB),
        AM_S("modCrush", "Crush", "roll mods", modCrush), AM_P("modCrushA", "Crush start", "roll mods", 0, 1, modCrushA), AM_P("modCrushB", "Crush end", "roll mods", 0, 1, modCrushB),
        AM_S("modStereo", "Stereo", "roll mods", modStereo), AM_P("modStereoA", "Stereo start", "roll mods", 0, 1, modStereoA), AM_P("modStereoB", "Stereo end", "roll mods", 0, 1, modStereoB),
        AM_S("modReverse", "Reverse every other", "roll mods", modReverse),
        AM_S("rrOn", "Reverse reverb", "reverse reverb", rrOn), AM_P("rrSourceS", "Source length (s)", "reverse reverb", 0.05, 1.5, rrSourceS), AM_P("rrMaxPreS", "Swell length (s)", "reverse reverb", 0.1, 4, rrMaxPreS),
        AM_P("rrDecayS", "Decay (s)", "reverse reverb", 0.3, 6, rrDecayS), AM_P("rrLandMs", "Land before the voice (ms, negative = earlier)", "reverse reverb", -100, 50, rrLandMs), AM_P("rrLevel", "Level", "reverse reverb", 0, 2, rrLevel),
        AM_S("responseOn", "Call and response", "response", responseOn), AM_C("responseKind", "Answer with", "response", responseKind, "Stutter", "Shimmer", "Freeze"), AM_P("responseLevel", "Level", "response", 0, 2, responseLevel),
        AM_P("responseAttackMs", "Attack (ms)", "response", 1, 500, responseAttackMs), AM_P("responseReleaseS", "Release (s)", "response", 0.01, 2, responseReleaseS),
        AM_P("responseBleed", "Bleed over the voice", "response", 0, 1, responseBleed), AM_P("responseDuckDb", "Under the voice (dB)", "response", -40, 0, responseDuckDb),
        AM_S("ghostOn", "Ghost notes", "ghost", ghostOn), AM_P("ghostRegister", "Interval from the voice (st)", "ghost", -24, 24, ghostRegister), AM_S("ghostFromKey", "Hold the key chord", "ghost", ghostFromKey),
        AM_P("ghostLevelDb", "Notes level (dB)", "ghost", -24, 24, ghostLevelDb), AM_P("ghostDrive", "Drive", "ghost", 0, 1, ghostDrive), AM_P("ghostAssist", "Resonance", "ghost", 0, 1, ghostAssist),
        AM_P("trigSensitivity", "Sensitivity: quieter than this below the peak is a gap (dB)", "triggers", 8, 40, trigSensitivity),
        AM_P("trigMinGapS", "Shortest gap (s)", "triggers", 0.03, 3, trigMinGapS), AM_P("trigMinPhraseS", "Only after lines this long (s)", "triggers", 0, 10, trigMinPhraseS),
        AM_P("trigMinEntranceDb", "Only when the voice comes back this hard (dB)", "triggers", 0, 60, trigMinEntranceDb), AM_S("trigSustainedOnly", "Only after a held, sung note", "triggers", trigSustainedOnly),
        AM_P("trigEveryN", "Every Nth gap", "triggers", 1, 8, trigEveryN), AM_P("trigProbability", "Chance per gap", "triggers", 0, 1, trigProbability),
        AM_P("trigDynamics", "Dynamics: follow the word's loudness", "triggers", 0, 1, trigDynamics),
        AM_P("revDecayS", "Reverb decay (s)", "returns", 0.2, 10, revDecayS), AM_P("revLowCut", "Reverb low cut (Hz)", "returns", 20, 2000, revLowCut), AM_P("revHighCut", "Reverb high cut (Hz)", "returns", 1000, 20000, revHighCut),
        AM_P("revWidth", "Reverb width", "returns", 0, 1, revWidth), AM_P("revLevelDb", "Reverb level (dB)", "returns", -60, 12, revLevelDb),
        AM_C("dlyDiv", "Delay time", "returns", dlyDiv, "1/1", "1/2", "1/4", "1/8", "1/16"), AM_S("dlyDotted", "Delay dotted", "returns", dlyDotted),
        AM_P("dlyFeedback", "Delay feedback", "returns", 0, 0.9, dlyFeedback), AM_P("dlyLevelDb", "Delay level (dB)", "returns", -60, 12, dlyLevelDb),
        AM_S("monoCheck", "Mono check", "monitor", monoCheck), AM_S("bypass", "Original (A/B)", "monitor", bypass),
    };
    return P;
}
#undef AM_P
#undef AM_S
#undef AM_C

// Division choices map index <-> denominator for the AM_C division params.
inline double divFromIndex(double idx) { static const double d[] = { 1, 2, 4, 8, 16, 32, 64 }; return d[std::clamp((int)std::lround(idx), 0, 6)]; }
inline int indexFromDiv(double div) { static const double d[] = { 1, 2, 4, 8, 16, 32, 64 }; for (int i = 0; i < 7; ++i) if (std::abs(d[i] - div) < 1e-6) return i; return 3; }

inline const Param* findParam(const std::string& id) { for (auto& p : params()) if (p.id == id) return &p; return nullptr; }
inline bool setParam(ProjectState& s, const std::string& id, double v) {
    if (auto* p = findParam(id)) { p->set(s, std::clamp(v, p->min, p->max)); return true; }
    return false;
}

// ---------------------------------------------------------------- minimal JSON (enough for our own files)
struct Json {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null; double n = 0; bool b = false; std::string s; std::vector<Json> a; std::map<std::string, Json> o;
    const Json* get(const std::string& k) const { auto it = o.find(k); return it == o.end() ? nullptr : &it->second; }
    double num(const std::string& k, double d) const { auto* j = get(k); return j && j->t == Num ? j->n : j && j->t == Bool ? j->b : d; }
    std::string str(const std::string& k, const std::string& d = {}) const { auto* j = get(k); return j && j->t == Str ? j->s : d; }
};
inline std::string jsonEscape(const std::string& s) {
    std::string o; for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if (c == '\n') o += "\\n"; else if ((unsigned char)c < 0x20) o += ' '; else o += c; } return o;
}
inline bool parseJson(const std::string& text, Json& out) {
    size_t i = 0;
    std::function<bool(Json&)> val;
    auto ws = [&] { while (i < text.size() && std::isspace((unsigned char)text[i])) ++i; };
    auto str = [&](std::string& s) { if (text[i] != '"') return false; ++i; while (i < text.size() && text[i] != '"') { if (text[i] == '\\' && i + 1 < text.size()) { ++i; s += text[i] == 'n' ? '\n' : text[i]; } else s += text[i]; ++i; } ++i; return true; };
    val = [&](Json& j) -> bool {
        ws(); if (i >= text.size()) return false; const char c = text[i];
        if (c == '{') { j.t = Json::Obj; ++i; ws(); if (text[i] == '}') { ++i; return true; }
            while (true) { ws(); std::string k; if (!str(k)) return false; ws(); if (text[i++] != ':') return false; Json v; if (!val(v)) return false; j.o[k] = v; ws(); if (text[i] == ',') { ++i; continue; } if (text[i] == '}') { ++i; return true; } return false; } }
        if (c == '[') { j.t = Json::Arr; ++i; ws(); if (text[i] == ']') { ++i; return true; }
            while (true) { Json v; if (!val(v)) return false; j.a.push_back(v); ws(); if (text[i] == ',') { ++i; continue; } if (text[i] == ']') { ++i; return true; } return false; } }
        if (c == '"') { j.t = Json::Str; return str(j.s); }
        if (!text.compare(i, 4, "true")) { j.t = Json::Bool; j.b = true; i += 4; return true; }
        if (!text.compare(i, 5, "false")) { j.t = Json::Bool; j.b = false; i += 5; return true; }
        if (!text.compare(i, 4, "null")) { j.t = Json::Null; i += 4; return true; }
        char* e = nullptr; j.n = std::strtod(text.c_str() + i, &e); if (e == text.c_str() + i) return false; j.t = Json::Num; i = (size_t)(e - text.c_str()); return true;
    };
    return val(out);
}
inline std::string num(double v) { char b[32]; std::snprintf(b, sizeof b, "%.6g", v); return b; }

inline std::string paramsJson(const ProjectState& s) {
    std::string o = "{"; bool first = true;
    for (auto& p : params()) { if (p.group == "monitor") continue; o += (first ? "\"" : ",\"") + p.id + "\":" + num(p.get(s)); first = false; }
    return o + "}";
}
inline std::string toJson(const ProjectState& s) {
    std::string o = "{\"format\":\"auto-malign-session\",\"version\":1,\"params\":" + paramsJson(s) + ",\"tracks\":[";
    for (size_t i = 0; i < s.tracks.size(); ++i) {
        const auto& t = s.tracks[i];
        o += (i ? "," : "") + std::string("{\"name\":\"") + jsonEscape(t.name) + "\",\"path\":\"" + jsonEscape(t.path) + "\",\"gainDb\":" + num(t.gainDb) + ",\"pan\":" + num(t.pan)
           + ",\"mute\":" + (t.mute ? "true" : "false") + ",\"solo\":" + (t.solo ? "true" : "false") + ",\"role\":" + num(t.role) + ",\"ref\":" + num(t.ref)
           + ",\"sendRevDb\":" + num(t.sendRevDb) + ",\"sendDlyDb\":" + num(t.sendDlyDb) + "}";
    }
    return o + "]}";
}
inline void applyParams(ProjectState& s, const Json& p) { for (auto& kv : p.o) if (kv.second.t == Json::Num || kv.second.t == Json::Bool) setParam(s, kv.first, kv.second.t == Json::Num ? kv.second.n : kv.second.b); }
inline bool fromJson(const std::string& text, ProjectState& s) {
    Json j; if (!parseJson(text, j) || j.t != Json::Obj) return false;
    ProjectState n; if (auto* p = j.get("params")) applyParams(n, *p);
    if (auto* tr = j.get("tracks"))
        for (auto& t : tr->a) {
            TrackState ts; ts.name = t.str("name"); ts.path = t.str("path"); ts.gainDb = t.num("gainDb", 0); ts.pan = t.num("pan", 0); ts.mute = t.num("mute", 0) > 0.5; ts.solo = t.num("solo", 0) > 0.5;
            ts.role = (int)t.num("role", 0); ts.ref = (int)t.num("ref", -1); ts.sendRevDb = t.num("sendRevDb", -120); ts.sendDlyDb = t.num("sendDlyDb", -120); n.tracks.push_back(ts);
        }
    s = n; return true;
}

// ---------------------------------------------------------------- presets (Aligned / Maligned starting points)
struct Preset { std::string name, family, mode, info; std::vector<std::pair<std::string, double>> values; };
inline const std::vector<Preset>& presets() {
    static const std::vector<Preset> P = {
        { "Doubles: tight", "Aligned", "Mixing", "Flam fix + split, differences kept low: one solid voice, clean in mono.", { { "mode", 0 }, { "doublesOn", 1 }, { "flamAmount", 1 }, { "agreement", 0.5 }, { "diffSideOnly", 0 } } },
        { "Doubles: wide and mono-safe", "Aligned", "Mixing", "Common voice in the centre, the takes' differences in the side only: big in stereo, one clean voice in mono.", { { "mode", 0 }, { "doublesOn", 1 }, { "flamAmount", 1 }, { "agreement", 1 }, { "diffSideOnly", 1 }, { "diffWidth", 1.6 } } },
        { "Doubles: human", "Aligned", "Mixing", "Half the flam fix and full differences: tighter, still loose.", { { "mode", 0 }, { "doublesOn", 1 }, { "flamAmount", 0.5 }, { "agreement", 1.3 }, { "diffSideOnly", 0 } } },
        { "Vocal space", "Aligned", "Mixing", "Delay throws on phrase ends, reverb that blooms only in the gaps, and a phase pocket in the beat.", { { "mode", 0 }, { "throwsOn", 1 }, { "gapReverbOn", 1 }, { "pocketOn", 1 }, { "pocketAmount", 0.6 } } },
        { "Freeze tails", "Maligned", "Producer", "The voice's spectrum held through every gap, into the reverb.", { { "mode", 1 }, { "freezeOn", 1 }, { "freezeGaps", 1 }, { "fxSendRevDb", -6 } } },
        { "Inside-out shimmer", "Maligned", "Producer", "An octave up, darker formant, breathy, only in the stereo side.", { { "mode", 1 }, { "shimmerOn", 1 }, { "shimSemis", 12 }, { "shimFormant", -2 }, { "shimBreath", 0.3 }, { "shimInsideOut", 1 } } },
        { "Glitch entrances", "Maligned", "Producer", "Pre roll into each entrance: riser, filter opening, drive and swell.", { { "mode", 1 }, { "rollOn", 1 }, { "rollMode", 1 }, { "rollStartDiv", 3 }, { "rollEndDiv", 6 }, { "modPitch", 1 }, { "modPitchA", 0 }, { "modPitchB", 12 }, { "modFilter", 1 }, { "modDrive", 1 }, { "modDriveB", 0.4 }, { "modLevel", 1 }, { "modLevelA", -15 } } },
        { "Crushed entrances", "Maligned", "Producer", "Pre roll that swells in, crushes down and spreads out across the stereo field.", { { "mode", 1 }, { "rollOn", 1 }, { "rollMode", 1 }, { "rollStartDiv", 3 }, { "rollEndDiv", 6 }, { "modLevel", 1 }, { "modLevelA", -18 }, { "modCrush", 1 }, { "modStereo", 1 } } },
        { "Swell entrances", "Maligned", "Producer", "Reverse reverb landing on every entrance.", { { "mode", 1 }, { "rrOn", 1 }, { "rrMaxPreS", 1.6 } } },
        { "Answer back", "Maligned", "Producer", "The last word answers itself in the next gap from the other side.", { { "mode", 1 }, { "responseOn", 1 }, { "responseKind", 0 } } },
        { "Ghost harmony", "Maligned", "Producer", "Ghost notes an octave above the voice, driven, into the reverb.", { { "mode", 1 }, { "ghostOn", 1 }, { "ghostRegister", 12 }, { "ghostDrive", 0.5 }, { "ghostLevelDb", 12 } } },
    };
    return P;
}
inline bool applyPreset(ProjectState& s, const std::string& name) {
    for (auto& p : presets()) if (p.name == name) { for (auto& kv : p.values) setParam(s, kv.first, kv.second); return true; }
    return false;
}

} // namespace am::studio
