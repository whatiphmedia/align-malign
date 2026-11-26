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
   Pick a Flavour to choose what malign does. With **Session** selected at the top (default), moving a control in
   one window moves it in every copy set to Session.

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
- **Tracks only, not the master or group buses.** Alignment compares tracks against each other; a bus is the sum of
  them. In Live, copies on the master or a group bus are left out of Learn automatically and pass audio through. In
  Studio One, set such a copy to **Off** in Groups.
- **What to expect:** fully aligned does the most where tracks share a source (two mics on one drum, bass DI + amp,
  layered samples of the same hit). On a mix of separate parts there is little to align, and fully aligned should
  sound close to the original. The flavours are where the creative effects are.
- **Neutral:** a new copy starts neutral (main knob 0, Width 50%, Motion 0, Mix 100%): the sound passes through
  unchanged, only delayed by the plugin's latency. In Session mode a new copy takes on the other copies' settings.
- **Latency:** each copy reports 4608 samples (~96 ms at 48 kHz). Both hosts compensate automatically, so
  tracks stay in time with each other.
- The solved map is saved with your project; you only need to Learn again after big edits.
- **Updating from an earlier build:** delete the old `Align Malign.vst3` folder (if any) from
  `C:\Program Files\Common Files\VST3\`, replace `Auto Malign.vst3`, and rescan. The maker is listed as **KracKill$**.
  If you used a build from before 2026-11-26, press Learn again: the alignment from older builds could smear the sound.

## Controls
**Ctrl+click** any knob or slider to reset it to its default.

### Main page
| Control | What it does |
|---|---|
| Main knob | Left of centre aligns (tracks pulled into phase with each other), right of centre maligns with the selected flavour. Centre = untouched. |
| Flavour buttons | What the malign side does (table below). |
| Match | Removes the level and tone change that aligning causes, so you hear phase, not loudness. 100% by default. |
| Width | Scales the side signal. 50% = unchanged. |
| Motion | Breathes between your setting and its mirror (align ↔ malign) every 2 bars, in tempo. |
| Mix | Blends with the original. |
| Learn (bottom right) | Starts and stops the session-wide Learn pass. |
| IN / OUT meters | This track's input and output peaks, -60 to 0 dBFS. |

### Top bar
| Control | What it does |
|---|---|
| Session / This track | **Session:** moving a control here moves it on every copy set to Session. **This track:** this copy's controls stay its own (group members still move together). |
| A/B | Plays the original sound, delayed to line up (a latency-matched bypass). |
| Delta | Plays only what the processing changes. |
| Focus mode | Stops the animations. |
| Main / Pro | Switches pages. |

### Pro page
**Alignment** (shared by the whole session; a change re-solves from what Learn already heard, no new Learn needed)
| Control | What it does |
|---|---|
| Lowest / highest frequency rotated | The band the alignment works in (2.7 Hz–4 kHz). |
| Frequency smoothing | How smoothly the correction changes across frequency (1/24 to 1/2 octave). Wider is safer. |
| Anchor stiffness | How strongly every track stays as recorded. Higher = tracks only move where it clearly pays. |
| Anchor track | A track that is never changed; everything else lines up to it. Auto picks the one with the most low end. |
| Never make it worse | Wherever the correction would not add to the mix, it fades back to the sound as recorded. On by default. |
| Latency before this track | Read from Live (control surface), or typed in by hand. Per copy. |
| Install for Ableton Live | Copies the control surface into your Live User Library. |

**Match:** Match amount (same as the Match slider). **Stereo:** *Phase mono below* keeps the side's phase locked to
the mid below the set frequency, so the low end stays mono-safe whatever the flavour does (Off by default); *Side
rotation* turns the side against the mid above that point, a phase widener; *Width* as on the main page.
**Groups:** see above. **Flavour:** *Amount* sets how deep the malign flavours go at a given knob position (100% by
default); Movement and Mix are the Motion and Mix controls. **Display:** frame rate and value tags (window only).

## Groups
Pro page > Groups lists every linked Auto Malign under its track name. Give tracks recorded from the same source the
same letter (kick in + kick out, bass DI + amp): they keep their phase to each other and are aligned as one, and
their knob and flavour turn together. Regrouping takes effect immediately from what Learn already heard. **Off**
leaves a track out. With the Live control surface, each row also shows where it sits (audio, MIDI, return, group bus,
master); group buses and the master are left out automatically.

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
