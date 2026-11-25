#!/usr/bin/env python3
"""Builds plugin/ui/index.html (embedded in the plugin) from the UI prototype + the plugin bridge.

    python3 tools/build_ui.py

Changes made for the plugin build (the prototype keeps working in a normal browser):
  - the scene and meters read the plugin's real output level/spectrum when the bridge feeds them (AU.ext)
  - the demo-only controls (Load audio, Sweep) are hidden; a Learn button is added to the status bar
  - bridge.js is appended at the end of the page
"""
import pathlib, sys

root = pathlib.Path(__file__).resolve().parents[1]
src = (root / "plugin/ui/src/prototype.html").read_text(encoding="utf-8")
bridge = (root / "plugin/ui/src/bridge.js").read_text(encoding="utf-8")


def rep(s, a, b):
    if a not in s:
        sys.exit(f"build_ui: anchor not found: {a[:70]!r}")
    return s.replace(a, b, 1)


s = src
# 1. external level/spectrum feed for the scene
s = rep(s, "function audioTick(t){const B=AU.bands;\n  if(AU.an&&AU.el&&!AU.el.paused){",
        "function audioTick(t){const B=AU.bands;\n  if(AU.ext&&performance.now()-AU.extT<600){for(let l=0;l<NL;l++)B[l]=AU.extB[l]||0;AU.level=AU.extL;}\n  else if(AU.an&&AU.el&&!AU.el.paused){")
# 2. Learn button (hidden outside the plugin)
s = rep(s, '<button id="sweep" aria-pressed="false"',
        '<button id="learn" class="plugin-only" aria-pressed="false" title="Play the song with Align Malign on every track; press again to solve">Learn</button><button id="sweep" aria-pressed="false"')
# 3. plugin-mode CSS
s = rep(s, "</style>",
        ".plugin-only{display:none}.in-plugin .plugin-only{display:inline-flex}\n"
        ".in-plugin #aud,.in-plugin #sweep{display:none}\n"
        "#learn[aria-pressed=\"true\"]{background:var(--line);color:#050505}\n</style>")
# 4. bridge inside the main script's closure (the UI's knobs and state are not globals)
end = "\n})();\n</script>"
i = s.rfind(end)
if i < 0:
    sys.exit("build_ui: end of the main UI script not found")
s = s[:i] + "\n/* ---- plugin bridge ---- */\n" + bridge + s[i:]

out = root / "plugin/ui/index.html"
out.write_text(s, encoding="utf-8")
print(f"wrote {out} ({len(s):,} bytes)")
