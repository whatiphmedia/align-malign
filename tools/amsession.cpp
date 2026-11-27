// amsession: the Auto Malign Studio session core from the command line (no UI). Useful for batch work and testing.
//   amsession <stems folder | session.amsession> [--preset "Name"]... [--set id=value]... [--role i=lead|double:ref|beat|return|other]
//             [--relearn start-end] [--out folder] [--save file.amsession] [--float] [--log file] [--list]
#include "am/studio/studio.h"
#include <cstdio>
#include <cstring>
using namespace am::studio;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: amsession <stems folder | session file> [--preset Name] [--set id=v] [--role i=lead|double:ref|beat|return|other] [--relearn a-b] [--track i.field=value] [--out dir | --mix-only --out file.wav] [--save file] [--float] [--log file] [--list]\n"); return 2; }
    Log::get().setEcho(true); Log::get().setLevel(Log::Info);
    for (int i = 2; i < argc; ++i) if (!std::strcmp(argv[i], "--log") && i + 1 < argc) Log::get().setFile(argv[++i]);
    for (int i = 2; i < argc; ++i) if (!std::strcmp(argv[i], "--list")) {
        std::printf("presets:\n"); for (auto& p : presets()) std::printf("  [%s/%s] %s: %s\n", p.family.c_str(), p.mode.c_str(), p.name.c_str(), p.info.c_str());
        std::printf("parameters:\n"); for (auto& p : params()) std::printf("  %-18s %-40s [%s] %g..%g (default %g)\n", p.id.c_str(), p.label.c_str(), p.group.c_str(), p.min, p.max, p.def);
        return 0;
    }
    Studio st; const std::string in = argv[1];
    const bool ok = std::filesystem::is_directory(in) ? (st.loadFolder(in) && (st.learnAll(), true)) : st.loadProject(in);
    if (!ok) return 1;
    std::string out, save; bool asFloat = false, mixOnly = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i]; auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--preset") st.applyPresetNamed(next());
        else if (a == "--set") { const auto kv = next(); const auto eq = kv.find('='); if (eq != std::string::npos) st.set(kv.substr(0, eq), std::atof(kv.c_str() + eq + 1), false); }
        else if (a == "--role") {
            const auto kv = next(); const auto eq = kv.find('='); if (eq == std::string::npos) continue;
            const int t = std::atoi(kv.substr(0, eq).c_str()); const std::string r = kv.substr(eq + 1);
            if (r.rfind("double:", 0) == 0) { st.setTrack(t, "role", TrackState::Double); st.setTrack(t, "ref", std::atoi(r.c_str() + 7)); }
            else st.setTrack(t, "role", r == "lead" ? TrackState::Lead : r == "beat" ? TrackState::Beat : r == "return" ? TrackState::Return : TrackState::Other);
        }
        else if (a == "--relearn") { const auto v = next(); double s0 = 0, s1 = 0; if (std::sscanf(v.c_str(), "%lf-%lf", &s0, &s1) == 2) st.relearnSection({ (size_t)(s0 * st.sr), (size_t)(s1 * st.sr) }); }
        else if (a == "--track") { const auto v = next(); int t = 0; char field[64] = {}; double val = 0; if (std::sscanf(v.c_str(), "%d.%63[^=]=%lf", &t, field, &val) == 3) st.setTrack(t, field, val); }
        else if (a == "--mix-only") mixOnly = true;
        else if (a == "--out") out = next();
        else if (a == "--save") save = next();
        else if (a == "--float") asFloat = true;
        else if (a == "--log") next();
    }
    std::printf("%s", st.summary().c_str());
    if (!save.empty()) st.saveProject(save);
    if (!out.empty()) {
        const auto R = st.render();
        if (mixOnly) { std::filesystem::create_directories(std::filesystem::path(out).parent_path().empty() ? "." : std::filesystem::path(out).parent_path()); if (!writeWav(out, R.mix, st.sr, asFloat)) return 1; }
        else if (!st.exportAll(out, R, asFloat)) return 1;
    }
    return 0;
}
