# Align Malign

Align every track in a session to each other (phase, not just time), then malign it creatively.
A free alternative to Sound Radix Auto-Align Post, built for creative use.

- **Plugin:** VST3 for Windows (Ableton Live 12, Studio One / Fender Studio Pro). ARA2 for Studio One is next.
- **Download:** Releases → *Latest build* → `AlignMalign-Windows-x64.zip`. Install and use: [docs/INSTALL.md](docs/INSTALL.md).

## Layout
| Path | What |
|---|---|
| `engine/` | Framework-free C++ engine: session bus, static phase solver, Match EQ, FIR builder, tests |
| `plugin/Source/` | JUCE 8 plugin (processor, WebView2 editor) |
| `plugin/ui/` | The interface. `src/prototype.html` is the design prototype, `src/bridge.js` connects it to the plugin; `tools/build_ui.py` builds `index.html`, which is embedded in the plugin |
| `.github/workflows/build.yml` | Windows build: engine test, VST3 + standalone, zip, release |

## Build locally (Windows)
Visual Studio 2022 or newer + CMake 3.22+ and `nuget` on PATH:
```
python tools/build_ui.py
cmake -B build -A x64
cmake --build build --config Release --target AlignMalign_VST3
```

## License
GPL-3.0-or-later (JUCE is used under its GPLv3 option).
