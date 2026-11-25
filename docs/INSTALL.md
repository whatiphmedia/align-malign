# Auto Malign — install and use (Windows, v0.1)

## Install
1. Download `AutoMalign-Windows-x64.zip` from the repo's **Releases → Latest build** page and unzip it.
2. Copy the whole **`Auto Malign.vst3`** folder to `C:\Program Files\Common Files\VST3\`.
3. The window uses Microsoft Edge WebView2. It ships with Windows 10 and 11; if the plugin window stays blank,
   install the "Evergreen WebView2 Runtime" from Microsoft.
4. Optional: `Auto Malign (standalone).exe` opens the interface on its own, handy for checking it runs.

## Ableton Live 12
1. Options → Preferences → Plug-Ins: turn on **Use VST3 Plug-in System Folders**, then **Rescan**.
2. Put **Auto Malign** on every track you want aligned (drums, bass, keys, vocals...). Every copy links up with
   the others automatically; the window shows how many are linked.
3. Press **Learn** in any copy, play the song (or the busiest 30–60 seconds), then press **Stop learning**.
   The alignment map is solved for every track at once.
4. Turn the big knob: left = align (tracks pulled into phase), right = malign (the creative side).
   Pick a Flavour to choose what malign does. With the scope set to **Session** (default), moving a control in
   one window moves it in every linked copy.

## Studio One / Fender Studio Pro
1. Options → Locations → VST Plug-ins: make sure `C:\Program Files\Common Files\VST3` is listed, then rescan.
2. Same steps as Live: insert on each track, **Learn**, play, **Stop learning**, then turn the knob.
3. Keep plug-in sandboxing **off** for Auto Malign so all copies run in one process and can link.
   (ARA2 mode for Studio One, which skips the Learn pass, is the next milestone.)

## Good to know
- **Put Auto Malign last** on each track, after your synth and effects, so it hears (and aligns) the finished sound.
  It needs to know how much latency is in front of it on each track:
  - **Ableton Live:** install the Auto Malign control surface once (Pro page > Alignment > *Install for Ableton Live*,
    or copy the `Ableton Live control surface/AutoMalign` folder from the download into
    `Documents\Ableton\User Library\Remote Scripts\`). Restart Live and choose **AutoMalign** under
    Settings > Link, Tempo & MIDI > Control Surface. Every instance then reads its own latency automatically;
    the Pro page shows what it found.
  - **Studio One (or Live without the control surface):** type the latency of the synth and plugins in front of
    Auto Malign into Pro page > Alignment > *Latency before this track (manual)*.
- **Renamed:** the plugin is now **Auto Malign**. If you installed an earlier build, delete the old
  `Align Malign.vst3` folder from `C:\Program Files\Common Files\VST3\` before copying the new one.
- **Ctrl+click** any knob or slider to reset it to its default (the main knob goes back to 0).
- **Latency:** each copy reports 4608 samples (~96 ms at 48 kHz). Both hosts compensate automatically, so
  tracks stay in time with each other. Put it on all tracks you care about so they share the same delay.
- **Match** (on by default) removes the level and tone change alignment causes, so you hear only the phase
  difference. **A/B** plays the original; **Delta** plays only what changed.
- **Motion** breathes between your setting and its mirror (align ↔ malign) every 2 bars, in tempo.
- **Width** scales the side signal (50% = unchanged). **Mix** blends with the original.
- The solved map is saved with your project; you only need to Learn again after big edits.
- Pro page settings other than the four macros are interface-only in this version.

## Flavours (what the malign side does)
| Flavour | What it does |
|---|---|
| Fuse | Over-aligns a wider band (to 4 kHz): thick, glued |
| Erase | Cancels tracks exactly where they overlap |
| Ghost | Outputs only the interaction between tracks (use Mix) |
| Phantom | Hollows the mid and swings the sides 90°: headphone-wide, mono-safe |
| Unmask | Aligns above 150 Hz only: clarity, low end untouched |
| Envelop | Rotates the sides below 300 Hz: wide low end, mono-safe |
| Curve | Aligned, then bent into a slow phase wave across octaves |
| Disperse | Scatters each track's phase per third-octave band |
