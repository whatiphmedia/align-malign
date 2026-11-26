# Auto Malign

Align every track in a session to each other (phase, not just time), then malign it creatively.
A free alternative to Sound Radix Auto-Align Post, built for creative use.

## What it does
Put one copy last on every track and press **Learn** while the song plays. Auto Malign listens to all tracks at once
and works out, frequency by frequency, how each track's phase should turn so the tracks that share sound (two mics on
a drum, a bass DI and amp, layered hits) add up instead of cancelling. Tracks that share nothing are left alone. The
big knob then goes from that alignment (left) through untouched (centre) to deliberate *malignment* (right), shaped by
eight flavours. Groups keep multi-mic sources together; every setting is explained in
[docs/INSTALL.md](docs/INSTALL.md#controls).

- **Plugin:** VST3 for Windows (Ableton Live 12, Studio One / Fender Studio Pro). ARA2 for Studio One is next.
- **Download:** Releases → *Latest build* → `AutoMalign-Windows-x64.zip`. Install and use: [docs/INSTALL.md](docs/INSTALL.md).

## Layout
| Path | What |
|---|---|
| `engine/` | Framework-free C++ engine: session bus, phase solver, Match EQ, FIR builder, partitioned convolver, tests |
| `plugin/Source/` | JUCE 8 plugin (processor, WebView2 editor) |
| `plugin/ui/src/` | The interface: `interface.html` (the full UI, also runs in a browser) and `bridge.js` (connects it to the plugin). `tools/build_ui.py` combines them into `plugin/ui/index.html`, which is embedded in the plugin; that file is generated, not committed |
| `live/AutoMalign/` | Ableton Live control surface (Remote Script): tells each Auto Malign instance how much latency sits in front of it on its track, so the plugin can go last in the chain |
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
