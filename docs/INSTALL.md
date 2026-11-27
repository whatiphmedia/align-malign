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
**Groups:** see above. **Refine flavour:** changes with the selected flavour. Every flavour has *Amount* (how deep
the malign flavours go at a given knob position, 100% by default), *Movement* and *Mix*; MALODICS adds Malodic notes
(below). **Display:** frame rate and value tags (window only).

## Malodic notes (Refine flavour, MALODICS selected)
Play pitches made of stereo phase with MIDI. Best on headphones.
- **Route MIDI to it.** Live: on a MIDI track, set *MIDI To* to the track with Auto Malign, then pick Auto Malign in
  the second menu. Studio One: set an instrument track's output to the Auto Malign insert. Notes land on the grid
  (they are scheduled one plugin latency later, which the host compensates).
- **Mode:** *Huggins* makes each note out of the difference between your ears (a phase illusion; the cue disappears
  on speakers and in mono). *Ghost* plays resonators tuned to the notes into the stereo side only: clear notes that
  cancel completely in mono.
- **Assist** (25%): a tuned resonator under the Huggins cue so the ear locks on; in Ghost mode the note level.
  **Purity** (70%): removes noise after the notes are made: keeps only the note and its harmonics in the resonator
  and narrows the carrier to the note. **Carrier** (35%): identical sound in both ears around each note for the cue
  to ride on; follows velocity. **Carrier fills gaps only** (on): noise is added only where the track itself has
  little sound around the note, so a full mix gets almost none and sparse sounds (hats) still get it.
  **Strength:** width of the phase sweep and the mono zone. **Fold notes into the sweet spot:** keeps each note name
  but moves it to 250–600 Hz, where the ears follow phase best. **Tame** (60%): a limiter on what the notes add,
  set relative to the track's level, so a note that rings up hard is held at a steady level instead of jumping out.
  **Noise flavour** (Hiss): the texture of the Huggins carrier noise: *Hiss*, *Breath* (softer, darker), *Crackle*
  (sparse grains), *Pitched* (a wavering tone on each harmonic). **Noise release** (120 ms): the Huggins cue (the
  phase effect and its noise) fades this fast after the MIDI note ends, separately from Release, so it stops with
  the note (also with Constant on); Release then only shapes the resonator tail. The flavour is heard most with
  *Carrier fills gaps only* off, since on a full mix that switch keeps the carrier almost silent.
  **Drive** (off): saturates the Malodic notes themselves, never the track (in Huggins mode, the Assist notes).
  **Constant** (off): the last notes keep sounding through gaps until the next note arrives, at a steadier level.
  **Attack** (40 ms) and **Release** (700 ms) fade each note in and out on a smooth S-curve (up to 2 s and 8 s).
  **Harmonics**, **Glide**, **Pitch bend range**; the sustain pedal holds notes.
- **Source:** what feeds the carrier and resonators: *This track + noise* (default), the *Sidechain input* (enable
  it in the host: Live's Sidechain section, Studio One's sidechain button), or *any other track with Auto Malign on
  it* (no routing needed; works while the transport plays).
- **Safety:** the phase cue adds no energy. The resonators are capped, the carrier never exceeds the Carrier setting
  × the track's level, and while notes sound the output is rounded off above 0 dBFS and never passes +6 dBFS.
  Listen at normal levels: turning up to chase a faint effect is the real risk to your ears.

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
| Malodics | Hollows the mid and swings the sides 90°: headphone-wide, mono-safe |
| Unmask | Aligns above 150 Hz only: clarity, low end untouched |
| Envelop | Rotates the sides below 300 Hz: wide low end, mono-safe |
| Curve | Aligned, then bent into a slow phase wave across octaves |
| Disperse | Scatters each track's phase per third-octave band |

## Auto Malign Studio (standalone stem session)
`Auto Malign Studio.exe` works on a folder of stems outside your DAW: it learns the whole song first, so everything is
exact and nothing runs live.

1. **Export your stems** from Live or Studio One as WAV files from the same start point, into one folder. Include the
   instrumental and, if you like, printed effect returns.
2. **Open stems** and choose the folder. Studio learns every track (phrases and gaps, consonants, pitch, tempo, key) and
   which takes double which. Check the roles on the left and change any of them:
   - **Lead:** a voice other takes follow.
   - **Double:** follows its lead (choose which); gets the flam fix and split.
   - **Beat:** gets the phase pocket.
   - **Return:** a printed effect return (mute it if you use the native returns instead).
   - **Other:** anything else.
3. **Mixing / Producer** switches what the right-hand panel shows. Everything you turn on is in the sound, whichever mode
   you are in.
   - **Mixing:** Doubles (flam fix, differences, sides only), delay throws (gated, darkening into the next line), gap
     bloom, phase pocket, triggers, native returns, master.
   - **Producer:** freeze, shimmer (formant, tilt, breath, inside-out), roll (Pre/Post/Both with start and end in tempo or
     ms, plus pitch, filter, drive, level, gate, crush, stereo and reverse modulation), reverse reverb (lands on the
     entrance), call and response (with bleed), Ghost notes (interval, key chord, level), triggers.
4. **Triggers** decide which gaps fire the learned effects, and how hard:
   - **Sensitivity:** what counts as a gap.
   - **Shortest gap** and **Min line:** ignore short gaps, or react only after long lines.
   - **Min entrance:** react only when the voice comes back hard.
   - **Held notes only:** react only after lines that end on a held, sung note.
   - **Every Nth** and **Chance:** variation.
   - **Dynamics:** the effect follows the loudness of the word or entrance it reacts to.
5. **Listen** with **Processed / Original** (A/B, the O key) and **Mono** (the M key). Space plays; Ctrl+Z / Ctrl+Shift+Z undo
   and redo. Ctrl+click or double-click a control to reset it.
6. **Re-learn a section:** after you replace or edit part of a stem, Shift-drag over that part of the timeline and press
   **Re-learn section** (all tracks, or one). Only that section is recomputed.
7. **Export** writes every processed stem, each effect layer and native return, and the mix (24-bit WAV) to a folder. Bring
   them back into your DAW. **Save** keeps the session (`.amsession`: settings, roles, sends and where the stems are).

The timeline colours waveforms by frequency, like KracKill$ Vision: red is lows (under 200 Hz), green is mids, blue is
highs (above 2 kHz).

**Debug log:** the **Log** button opens everything Studio does, with timings. It is also written to
`%APPDATA%\AutoMalignStudio\studio.log`; **Show file** opens that folder.

**Command line:** `amsession.exe <stems folder> [--preset "Name"] [--set id=value] [--out folder]` runs the same engine
without the window. `amsession.exe x --list` lists every preset and parameter.

**Known issue:** on Windows the "never make it worse" guard can leave a few frequency bands (under 1%) slightly worse.
The test allows this for now; it will be revisited with multi-mic drum stems.
