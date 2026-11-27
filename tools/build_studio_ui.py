# Fills the browser-mock data in app/ui/studio.html (parameter list and presets) from the C++ registry, so opening the
# page in a browser shows exactly the app's controls. Usage: python tools/build_studio_ui.py <defs.txt from the dump tool>
import re, sys
defs, presets = open(sys.argv[1]).read().strip().split('\n')
p = 'app/ui/studio.html'; s = open(p, encoding='utf-8').read()
s = re.sub(r'function MOCK_DEFS\(\) \{ return .*? \}\n', 'function MOCK_DEFS() { return ' + defs.replace('\\', '\\\\') + '; }\n', s, flags=re.S)
s = re.sub(r'function MOCK_PRESETS\(\) \{ return .*? \}\n', 'function MOCK_PRESETS() { return ' + presets.replace('\\', '\\\\') + '; }\n', s, flags=re.S)
open(p, 'w', encoding='utf-8').write(s); print('studio.html mock data updated')
