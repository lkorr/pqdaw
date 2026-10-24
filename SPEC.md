# pqdaw — Specification (draft 3)

Status: planning. Date: 2026-10-20. Platform: Windows first, portable code.

A C++ DAW that works like Ableton Live's Arrangement View (layout, hotkeys, workflow, stock devices), but with:

1. A pitch system that is not MIDI: any EDO, any MOS/scale (morphable), Scala tunings, and adaptive JI with virtual fundamentals and proportional "JI objects" (after Zheanna Erose's work).
2. A low-level modular DSP language under every device. Every stock device is a patch of primitive "atoms", and **any output can modulate any parameter at audio rate**, across tracks.
3. A performance-first engine (after SeptaBee/Lost Robot): no wasted bytes or cycles, strictly realtime-safe, and patches recompiled to minimal code after edits.
4. Optional keyboard macro mode (Luke's prime-harmonic interval macro) for fast note entry in EDO/MOS/JI.

MIDI, MPE, MTS-ESP and Scala are **compatibility layers**, used for import/export and for driving external synths. The internal model never depends on them.

---

## 0. Principles

- **Data costs only what it carries.** A 4-byte parameter modulated at audio rate costs 4 bytes per sample. An unmodulated one costs 4 bytes total. There are no per-event headers on the modulation path.
- **Pay only for what is used.** Rates, polyphony and channel counts are inferred per connection. The compiler emits the cheapest code that is correct for the current patch.
- **Hospital-grade realtime safety.** The audio thread does no allocation, no locks, no syscalls, no I/O and nothing unbounded. Everything it needs is preallocated and handed over through lock-free structures.
- **Silence is free.** Voices, devices and tracks that are provably silent (input silent and tail finished) do not run.
- **External formats get forced into our shape**, never the reverse.
- **Standing rule for every step of the build**: ask whether what's being built is as CPU-efficient as it can possibly be without an obviously bad sacrifice (correctness, audible quality, realtime safety, or a big maintainability cost). Every design review and PR answers this explicitly.

---

## 1. Scope

### v1 includes
pqdaw must be a **fully functional DAW**, not a tech demo. §9 lists every fundamental feature.
- Arrangement View: tracks, clips, automation, locators, loop, time signature and tempo (with tempo automation).
- Track types: Instrument/MIDI (note), Audio, **Group (nestable, with its own device chain = group processing)**, Return, Master. Mixer with sends, pan, volume, mute/solo/arm, routing, sidechain, and latency compensation.
- Note clips ("MIDI clips"; internally our own note format) edited in the new piano roll (EDO / MOS / Scala / JI), with the MOS wheel selector and JI objects.
- Grid: fixed and **adaptive**, with **straight / triplet / 5-, 7-, 11-tuplet** modes from the right-click menu (§9.2).
- Audio clips with **warping**: Beats, Tones, Texture, Re-Pitch, Complex, Complex Pro; warp markers; auto-warp.
- Recording: audio (multi-input), notes from MIDI controllers or the computer keyboard, **take lanes + comping**, **Capture MIDI**.
- **Browser + user sample library** (§9.1): search, tags, preview in tempo/tuning, drag in and out.
- **Racks** (§8.2): Audio Effect Rack, Instrument Rack, Drum Rack, with chains, zones and macros.
- Instruments, all built from atoms: **Operator, Simpler**, plus Instrument Rack and Drum Rack.
- Audio effects, all built from atoms: **Utility, EQ Eight, EQ Three, Auto Filter, Saturator, Compressor, Glue Compressor, Limiter, Gate, Delay, Auto Pan, Chorus-Ensemble, Phaser-Flanger, Reverb, Resonator, Spectrum, Tuner.**
- Groove pool / swing; MIDI and key mapping; freeze/flatten; resample/bounce; mixdown + stem export; collect-and-save.
- **Patch editor drawn as a circuit diagram** (§7.3), plus a text form of the same patch language. Both are equal views of one graph.
- Cross-device wiring: drag from an output port on top of a device to any parameter anywhere in the project.
- Ableton hotkeys throughout, plus a toggleable macro mode.
- Scala `.scl`/`.kbm` import, used to set tunings.

### v1 compatibility layers (present but not central)
- MIDI file import/export; MIDI controller input (including MPE).
- MTS-ESP master and MPE output to external synths/DAWs.

### Not in v1
- Session View (the data model stays open to adding it).
- Third-party plugin hosting (VST3/CLAP). Planned later as "sealed" devices: block-rate modulation only, CLAP note-expression tuning.
- Sampler (multisample instrument), clip envelopes, Max-for-Live equivalents, video, macOS/Linux builds.

---

## 2. System architecture

```
 UI thread (Vulkan) ──edits──▶ Document (immutable snapshots, undo history)
                                   │ diff
                                   ▼
                            Compiler worker ──▶ Program (JIT; interpreter while compiling)
                                   │ atomic publish (lock-free)
                                   ▼
 Audio thread (RT) ◀── commands (SPSC queues) ── transport, knob values
      │  ▲
      ▼  │ meters / scopes / playhead (SPSC, lossy)
 Disk thread (sample streaming, recording writer)
 Analysis worker (transients, tempo detection, waveform overviews)
```

- **Document**: an immutable, structurally shared tree (project → tracks → clips/devices → patches). Each edit is a command that produces a new snapshot. Undo/redo swaps snapshots.
- **Compiler**: turns the document's whole-project signal graph into a `Program` (§7). It runs off the audio thread. Programs are published atomically, and old ones are reclaimed on a non-RT thread (epoch/RCU).
- **Parameter values** (knob turns, automation constants) do **not** trigger recompiles. They live in a parameter block the audio thread reads. Only structural changes recompile: add/remove nodes or wires, rate changes, polyphony changes.
- **Threading v1**: one RT audio thread. v2: parallel execution of independent strongly-connected components (SCCs), with a work-stealing pool pinned to cores.
- **Audio I/O**: RtAudio (ASIO + WASAPI exclusive/shared). Default 48 kHz, 64–256 sample buffers. The internal block size is decoupled from the device buffer.

### Libraries (proposed)
| Need | Choice | License |
|---|---|---|
| Window/input | SDL3 (or raw Win32) | zlib |
| GPU | Vulkan + VMA | MIT |
| Text | FreeType (hinted, integer px sizes) + HarfBuzz, bitmap glyph atlas | FTL/MIT |
| Audio I/O | RtAudio + Steinberg ASIO SDK | MIT / proprietary-free |
| Audio files | dr_libs (wav/flac/mp3) + libsndfile optional | PD / LGPL |
| Time-stretch (Complex/Pro) | signalsmith-stretch | MIT |
| Microtuning out | MTS-ESP | 0BSD |
| JIT + AOT | LLVM ORC (core dependency). Minimal static build: X86 target only, ORC + the passes we use, LTO/dead-stripped. Target ≤ ~30 MB on disk (measure in M1b). Fallback if size or compile latency disappoints: asmjit (zlib, ~1 MB) with our own IR optimisation and hand-written SIMD codegen | Apache-2.0 w/ LLVM exc. |
| Tests | doctest/Catch2 | MIT/BSL |
| Build | CMake + vcpkg, C++23, clang-cl/MSVC, AVX2 baseline w/ SSE4.1 fallback | |

---

## 3. Pitch & tuning model

### 3.1 Note pitch identity
Every note has exactly one **pitch identity**, which decides how it behaves when the tuning changes:

| Identity | Stored as | On tuning change |
|---|---|---|
| `Degree` | `(degree:int, equave:int)` within the clip's tuning context | Moves with the scale (MOS morphing) |
| `Ratio` | exact rational (int64 n/d, also kept as a monzo up to a prime limit) relative to an **anchor** (another note, a JI object's fundamental, or the clip reference) | Stays at true pitch; the EDO only changes display/snapping |
| `Cents` | absolute cents from the reference (default **C3 = 261.626 Hz**, Ableton's middle-C naming; changeable per project and per clip) | Never moves |
| `Harmonic` | `(jiObjectId, voiceIndex)` | Follows the JI object (§5) |

- **Every note is ultimately one Hz value (float32).** The identity is the *rule* that sets that value. The engine only ever sees the resolved Hz signal (constant per voice, or a per-sample signal if tuning or expression is modulated). The identity is kept in the document, so retuning, snapping and editing stay exact.
- **Precision rules** (float32 everywhere, with three exceptions where float32 would cause audible or structural error):
  - *Final pitch as float32 Hz*: relative precision ≈ 6·10⁻⁸ ≈ 0.0001 ¢. Fine.
  - *Oscillator phase*: **uint32 fixed-point** accumulators, the same 4 bytes as a float. A float32 phase accumulator mistunes low notes by about 0.1 ¢ (20 Hz at 48 kHz), which audibly shifts JI beat rates. A fixed-point accumulator is exact, wraps for free with no branch, and is cheaper. Phase increment = `round(hz · 2³² / sr)`.
  - *Timeline positions*: **int64** ticks/samples. float32 seconds stop being sample-accurate after about 6 minutes (2²⁴ samples at 48 kHz).
  - *Ratio identities*: stored as integers (n/d), so composition never piles up rounding error. They become Hz only at the final step.
- Notes can be converted between identities explicitly (snap ratio→degree, freeze degree→cents, and so on).
- Each note also carries per-note expression curves: pitch offset (cents), gain, pan, timbre. These are curves, so a pitch glide is part of the note, not a separate pitch-bend stream.
- Time: int64 ticks, PPQ = 221,760 (2⁶·3²·5·7·11: straight notes exact down to 1/256, tuplets up to 11 exact; decided 2026-10-21, was 27,720), plus a tempo map.
- No note-count limit. Pitch range is unbounded (MIDI's 128 is irrelevant internally).

### 3.2 Tunings: named, shared, independently automatable
- A project has **any number of named "main" tunings** (e.g. "Tuning A: 5L2s meantone", "Tuning B: 22edo"). Each is a project-level object with its own automation lane and modulation inputs.
- Each **track follows one main tuning** (or has its own private one). Each **clip** inherits its track's tuning by default and may override it (a private tuning, or a different main tuning).
- Every main tuning is a separate signal node, so automating or modulating Tuning A re-voices every track/clip that follows A, while tracks on Tuning B transform independently at the same time. Moving a track to another tuning is one reassignment, with no note edits.
- The reference pitch (default C3 = 261.626 Hz) is a parameter of each tuning, so it can differ per project, per tuning and per clip.
- UI: a tuning chip on every track header and clip, coloured per main tuning; a "Tunings" panel lists main tunings with their automation lanes.

A tuning is one of:
- `EDO { n, equave = 2/1 (any ratio or cents) }`
- `MOS { nL, nS, period, generator (cents), mode rotation, modmos[], octave stretch }`: a port of the mos-explorer math (`buildMOSScale`, `buildMOSRegions` Stern–Brocot, UDP/mode naming, hardness, `transferModeRotation`, parent/child navigation, and the ~80 `TEMPERAMENTS` presets with their generator ranges).
- `Scale { cents[], equave }`: from Scala `.scl` (+ `.kbm` mapping: reference note/frequency, degree mapping).
- `JI { prime limit, reference }`: lattice context for ratio entry and labelling.
- **Display grid** (independent of note identity): any EDO overlaid for snapping and visual reference.

### 3.3 Tuning as a signal (audio-rate)
- The tuning context is a node in the signal graph. Its continuous parameters (MOS generator, period, stretch, EDO equave, reference frequency) are ordinary parameters: automatable and modulatable at audio rate by any source.
- **Cheap evaluation**: within a fixed MOS region, each degree's pitch is linear in the generator: `cents_i = k_i·g − m_i·P (+ equave·P)`. The coefficients `(k_i, m_i)` change only when the generator crosses a region boundary (the MOS changes size/ordering). So:
  - region/ordering is resolved at block rate (or at the sample where a crossing occurs),
  - per-voice pitch is one fused multiply-add per sample.
- When a sweep changes the MOS size, held degree-notes remap with the mos-explorer's `updateHeldNotes`/`transferModeRotation` logic (configurable: snap to nearest degree / hold last pitch).
- External outputs (MTS-ESP, MPE) can only follow tuning changes at block rate. This is a protocol limit and is documented in the UI.

---

## 4. Piano roll

Layout is Ableton's (clip view at the bottom, fold, scale highlighting, velocity lane, draw mode `B`, grid hotkeys), with these changes.

### 4.1 Rows and markers
- **EDO mode**: one row per step. Row height is uniform or proportional to cents (the same thing for an EDO). **Equave/octave markers** are drawn as strong horizontal lines, labelled with an equave index (`C4`-style naming optional for 12edo-compatible EDOs, otherwise `0\31`, `1\31`… with equave number).
- **Scale/MOS mode**: one row per scale degree, with two layouts from mos-explorer:
  - **Equidistant**: rows are uniform, like a chromatic roll in the scale's own steps.
  - **Proportional** (default): rows sit at true cents. Note blocks use the mos-explorer asymmetric-half-height rule, so as a MOS collapses its s-step rows shrink to slivers. Small-step notes are drawn under L-step notes.
  - Gutter keys are coloured by step class (tonic / L / s / extra steps for MODMOS), and the L/s colour blend follows hardness (mos-explorer softness formula).
- **Background EDO grid** can be overlaid on a scale view for snapping (e.g. 5L2s shown over 31edo).
- **Live morph**: changing the generator (wheel, slider, automation) moves rows and degree-notes continuously at frame rate. During playback the roll follows the audio-rate tuning signal (decimated to frame rate for display).

### 4.2 MOS wheel selector
A popover (hotkey TBD, also on the clip's tuning chip) that ports mos-explorer's two linked views:
- **Region chart**: generator (cents) × MOS size, with Stern–Brocot regions, EDO snap markers (m\n up to a configurable max EDO) and temperament ranges.
- **Circle**: step slices (L/s/M…), degree spokes with cents, tonic, JI markers, hardness slider (equalized ↔ collapsed), mode rotation by step/by generator, parent/child arrows, MODMOS alterations.
- Output: a `MOS` tuning context for the clip, or for the track/project default. Also selectable: an EDO number, a Scala file, or a temperament preset.

### 4.3 JI view layer
- A **virtual fundamental** is drawn as a translucent horizontal line in pitch space (it may lie far below the visible notes). Its harmonics (or subharmonics) are shown as faint guide lines, labelled by index.
- Notes with `Ratio`/`Harmonic` identity show their ratio or harmonic number. Optional labels: cents, deviation from the display EDO, interval name (colour notation / FJS, from the trainer's naming tables).

---

## 5. JI objects (after Zheanna Erose)

A **JI object** is a region in a note clip, drawn as a box over the global roll, that holds a proportional chord and its own inner timeline.

- **Definition**: a proportion `a:b:c:…` (e.g. `8:9:10:12:15`) plus a fundamental, given as an absolute pitch or anchored to a note/degree. An **overtone/undertone flag** on the same object type switches to subharmonic proportions (`1/a : 1/b : …`). Morph, walk, arp and chord sets all work in both modes.
- **Inner piano roll**: notes inside the object have `Harmonic` identity. Their rows are the object's harmonics, and the inner roll follows the object as it moves or morphs. Notes drawn over an object follow its curves (they "ride" it).
- **Morph**: a sequence of proportions (e.g. rotations `8:9:10:12:15 → 9:10:12:15:16 → …`, or `7:8:9:10:11:12:13 → 5:6:7:8:9 → 4:5:6:7 → 3:4:5 → 2:3 → 1`). Each transition has an editable time and curve shape per voice (linear-in-log-frequency, S-curve, custom bezier).
- **Voice add/remove**: the object always holds `max(voice count)` voices. Voices that are not needed sit in unison with a neighbour. Gain per voice is `1/k` where `k` is the number of voices at that pitch, so unisons add up coherently without level jumps. **Phase coherence**: voices in unison share an oscillator phase, so splits are smooth and there is no cancellation (this answers matea's question in the reference chat).
- **Walk a term**: keep one term fixed (usually the lowest, the root) and step another term `n → n±1`. The other voices re-proportion (`22:26:30:33:39:50:59 → 21:… → 20:…`).
- **Ride a curve**: the object's fundamental follows a drawn pitch curve, and all voices and inner notes move with it.
- **Pulse arpeggiator**: rhythmic patterns acting on the object's voices (rate, tuplet, order: up/down/random/shuffled, gate, duplicated octave/twelfth layers).
- **Chord sets**: a set of proportions played in shuffled rounds.
- **Pen tool modes** (modifier-held): free draw, curve, vibrato painting (hold Alt; depth/rate defined per stroke).
- **Scaffold**: an object can be "flattened" into ordinary notes that keep their identities, for further hand editing.
- **Output**: JI objects produce voices with per-sample pitch signals into the instrument. External synths get MPE or MTS-ESP.

---

## 6. Keyboard: Ableton hotkeys + macro mode

### 6.1 Default: Ableton Live hotkeys
Copy Live 12's Arrangement/clip-view shortcuts (to be checked against the Live 12 manual during implementation). Examples:
- Space play/stop; Shift+Space continue; F9 record.
- Ctrl+T / Ctrl+Shift+T new MIDI/audio track; Ctrl+G group; Ctrl+R rename; 0 activate/deactivate.
- Ctrl+E split; Ctrl+J consolidate; Ctrl+D duplicate; Ctrl+L loop selection.
- Ctrl+1/2/3/4/5 grid narrow/widen/triplet/snap/fixed; B draw mode; A automation mode; Z / X zoom to selection / back.
- Ctrl+Alt+B browser; Shift+Tab clip/device detail; Alt+drag copy; Ctrl+Shift+M insert clip.
- M toggles the computer keyboard as an instrument. In our system it plays the current tuning (degree layout or isomorphic layout).
- **Microtonal adaptation**: Up/Down move a note by one display-grid step (one EDO step, or one degree in scale mode). `Ratio`/`Harmonic` notes move to the **next harmonic** of their fundamental: `n/d → (n±1)/d`. Shift+Up/Down move by one equave.

### 6.2 Macro mode (toggle: `Ctrl+CapsLock`, configurable)
This is a native, in-DAW version of Luke's AHK macro and the JI trainer's piano-roll mode. When it is on, it takes over the keys below. Other Ableton hotkeys that use Ctrl/Alt pass through (same rule as the AHK `*` wildcard).

**State**: a **cursor** (time, pitch), an **anchor** (save point), and a **composition** (the running interval product, starting at 1/1).

| Key | Action |
|---|---|
| `1 2 3 4 5 6 7 8 9 0` | multiply composition by 2/1 3/2 5/4 7/4 11/8 13/8 17/16 19/16 23/16 29/16 (cursor preview moves) |
| `q w e r t y u i o p` | multiply by the reciprocals (1/2 2/3 4/5 …) |
| `` ` `` | place note at cursor, reset composition, return cursor to anchor |
| `Tab` | place note, set anchor here (chaining) |
| `Backspace` | clear composition / delete note at cursor if composition empty |
| `a / f` | cursor left / right one grid step (Shift: extend selection) |
| `s / d` | cursor up / down one display step |
| `-` / `=` | undo / redo (macro-level) |

Interval mapping comes from a configurable keymap (presets as in `midi_config.ini`: EDO, slot ratios, keys).

**Layers** (switchable):
- **Tempered harmonics** (default in EDO/MOS): each key is the tuning's mapping of that prime (patent val, overridable per EDO). Notes are placed with `Degree` identity. The composition is still tracked as an exact ratio, and its tempering error is shown.
- **Scale steps**: in MOS/scale contexts, `1`=+1 degree, `2`=+generator, `3`=+L, `4`=+s, `5`=+period, `6`–`0`=+2…+6 degrees. The bottom row `q`–`p` makes the same moves downward.
- **Pure JI**: notes are placed with `Ratio` identity relative to the anchor.

**Virtual-fundamental lock** (`b` toggles):
- `b` + number locks onto the note under the cursor and declares its harmonic index: `b5` = "this note is the 5th harmonic". Multi-digit indices work (`b15`); the index is committed by `Enter` or by the next non-digit key. `b` on its own reuses the last index. The virtual fundamental = note / N, drawn in pitch space on the roll.
- While locked, the same `asdf` keys keep their meaning, but the pitch axis is harmonic:
  - `s / d`: cursor to the next / previous harmonic (n → n±1).
  - `a / f`: time left/right, as before.
  - `` ` `` / `Tab` place notes as before, with `Harmonic`/`Ratio` identity relative to the locked fundamental.
- `b` again unlocks (back to anchor/interval entry). Number keys keep their interval meaning while locked, so you can jump by 3/2 and land on the corresponding harmonic.
- Later additions, not v1-critical: subharmonic lock (undertone series), walking the root term (Zhea's walk) from the keyboard, converting a selection of harmonics into a JI object.
- Fewest-keypress labelling: the BFS from the trainer shows the shortest key sequence for any target step under a chosen prime limit.

---

## 7. Engine & patch language

### 7.1 Signal model
Every port has:
- **Rate**: `const` (compile-time), `param` (runtime scalar; updated by UI/automation at block rate), `audio` (one value per sample), `event` (note on/off, voice allocation).
- **Kind**: `real` (float32) or `bool` (logic level). Bool signals are stored as SIMD lane masks: one bit per voice per sample in packed form, expanded to 0/1 floats only where a real-valued input consumes them. Logic is nearly free, and a `bool` that only changes at block boundaries compiles to control flow (skip/branch per block) instead of per-sample work.
- **Width**: mono / stereo / N channels.
- **Polyphony**: `global` (one instance) or `voice` (one per active voice).

Rates and polyphony are **inferred**. A parameter input accepts any rate, and the compiler specialises each node for the rate actually connected. A filter cutoff with nothing wired in is one scalar load per block. Wire Operator's oscillator into it and it becomes one float per sample. This is the "4 bytes per 4 bytes" rule.

Modulation wires have a **mode** per target parameter: add (in the parameter's native unit, e.g. Hz or cents), multiply, or replace. They also have depth and an optional range map. Pitch/frequency parameters offer linear (through-zero) FM or exponential (cents) FM.

### 7.2 Atoms (primitive nodes)
**Only true circuit elements are native leaf atoms**: memory, math, logic, sample/IO access. Everything else, including **filters, envelopes, LFOs, oscillators beyond phasor/sine, and every stock device**, is a subcircuit made of these atoms. After compilation a primitive-built filter is the same machine code as a hand-written one: the compiler inlines it, keeps state in registers, and hoists param-rate coefficient math (e.g. `tan()` for cutoff) out of the per-sample loop. Leaf atoms are native and SIMD across voices:
- **Generators**: `phasor` (saw/phase 0..1, with sync/reset), `sine` (from phase or frequency), `wavetable` (user-drawable table read by phase; basic shapes; band-limited mipmaps), `noise`, `const`.
- **Math**: `+ − × ÷`, `min max clip abs sign floor frac mod`, `tan tanh exp log pow sqrt sin` (each with exact and `fast_` approximation variants, so audio-rate cutoff/pitch modulation stays cheap), `cents↔hz`, `db↔gain`, `select`, `lerp`.
- **Shaping**: `shaper` (a wavetable read by input value: drawable transfer curves).
- **Memory**: `z⁻¹` (1-sample delay), `delayline` (fractional read: linear/allpass/cubic), `sample&hold`, `latch`.
- **Filters are stock subcircuits, not atoms**: `onepole`, `svf` (TPT/ZDF; LP/BP/HP/notch/peak/shelf), `biquad`, `allpass`, `dcblock`, `comb`, ladder models. All are made of `z⁻¹`, math and (for nonlinear models) `solve`, and all can be opened and edited.
- **Zero-delay feedback**: filters solve their feedback loop within the same sample. Subcircuits write the solved form explicitly (e.g. TPT SVF: `g = tan(π·fc/fs)`, `h = 1/(1+2Rg+g²)`, …). The compiler treats this as ordinary acyclic math. It inserts `z⁻¹` (and warns) only for genuine cycles the user drew.
- **`solve { … } iterations: n`**: fixed-iteration Newton solving of an implicit per-sample equation (nonlinear ZDF: ladder/MS2/PRD models with `tanh` in the loop). Unrolled by the compiler; bounded, so it's realtime-safe.
- **Dynamics are subcircuits too**: `envfollow` (peak/RMS, attack/release) = abs/square + one-pole subcircuit; lookahead = `delayline`.
- **Logic (boolean)**: `and or not xor nand nor`, `>` `<` `≥` `≤` `==` comparators (real→bool), `schmitt` (hysteresis comparator), `rise`/`fall`/`change` edge detectors (bool→trigger), `sr`/`d`/`t` flip-flops, `counter` (up/down, wrap, reset), `clockdiv`, `mux`/`demux` (n-way select by index), `gate→real` (0/1 conversion), `pulse` (one-sample trigger), `timer` (bool high for N samples/ms).
- **Envelopes and control**: `adsr`/multi-stage `env`, `lfo` (phasor + shapes, tempo sync), `slew`. These are *composites*: stock patches built from phasor, logic, math and memory atoms, compiled away like any other patch. Only the leaf atoms above are native code.
- **Events and voices**: `voice.hz` (float32 Hz, per-sample, resolved from pitch identity × tuning), `voice.cents` (derived), `voice.gate` (bool), `voice.trigger` (bool pulse), `voice.velocity`, `voice.release`, `voice.ratio`, `voice.harmonic`, `voice.unisonCount`, `voice.index`.
- **Samples**: `sampleRead` (interpolated read of a sample buffer at a position/rate signal; loop points; per-voice; disk-streamed for long files). Simpler and Drum Rack are built from it.
- **I/O**: `track.in`, `sidechain.in`, `out`, `send`, `clipPlayer` (native warp engine, §9.4), `transport` (tempo, beat phase, bar phase, playing bool).
- **Structure**: `oversample { … }` region (2×/4×/8×, polyphase), `voicechain { … }` (effects processed per voice), `fft`/`ifft` (v1.x; for the spectrum analyzer and possible spectral atoms).

### 7.3 The patch language: a circuit diagram
The low-level language is **visual first and looks like a circuit schematic**. The text form is an equal, two-way-synced view of the same graph.

**Schematic editor**
- **Components use schematic symbols**: logic gates in standard gate shapes (AND/OR/NOT/XOR), comparators and the summing amp as op-amp triangles, `z⁻¹`/delay lines as boxes labelled with the delay, filters as boxes with a response glyph, oscillators as a circle with the waveform, flip-flops/counters as IC-style rectangles with labelled pins, sources as `const`/`param` terminals.
- **Wires are orthogonal (Manhattan-routed)**, with junction dots where they branch and crossings without dots where they don't. Wires are coloured by kind and rate:
  - bool: thin, green-ish
  - param: thin, grey
  - audio: thicker, signal colour
  - poly: doubled line
  - modulation into a parameter: dashed
- **Live probing**: hover any wire for a value, logic level or mini-scope. Bool wires light up when high, like an LED logic trace.
- **Hierarchy**: select components → "make subcircuit", which becomes a block with pins. Stock devices are subcircuits you can open all the way down to leaf atoms. Ableton-style device panels are a front panel over a subcircuit, and panel controls are its `param` pins.
- **Layout is stored**, with an auto-route and tidy command. The CPU cost meter (ns/sample) is shown per subcircuit.

**Text form** (diffable, hand-editable, how stock devices are authored; sketch):

```
patch SimpleFM {
  param ratio  = 2.0   [0.25..16]
  param index  = 1.0   [0..20]
  in    hz     : audio voice          // from voice.hz
  in    gate   : bool  voice          // from voice.gate
  out   out    : audio voice

  mod   = sine(phasor(hz * ratio))
  osc   = sine(phasor(hz + mod * index * hz))
  env   = adsr(gate, a: 5ms, d: 200ms, s: 0.7, r: 300ms)
  out   = osc * env
}
```

Devices expose `export`ed ports. These are the "cute little" output jacks on top of the device, and any parameter can be a wire target.

### 7.4 Compilation pipeline
1. **Flatten**: whole-project graph (all tracks, devices, cross-track wires).
2. **Infer**: rates, widths, polyphony. Poly→global defaults to **sum**, with a per-wire override: last, highest, lowest, loudest voice. Per-voice processing happens inside `voicechain`.
3. **Optimise**: constant folding (`const` only, never `param`, so knob turns stay cheap), dead-node elimination (unconnected outputs, muted/silent paths), CSE, op fusion, strength reduction (e.g. `×1`, `+0`, unmodulated `audio`→`param`).
4. **Schedule**: topological sort, SCC detection. Feedback cycles get an implicit `z⁻¹` (warning shown). Non-feedback regions run in blocks of 16–64. Feedback SCCs run per sample (or at the smallest block size that keeps them correct). Cross-track SCCs pull the involved tracks into one scheduling region.
5. **Lay out memory**: struct-of-arrays, voice-major SIMD lanes (8 voices per AVX2 register), and buffer liveness analysis to reuse scratch buffers. All state goes in one arena per Program, with **stable state slots keyed by node ID**.
6. **Backend**:
   - **LLVM ORC JIT (primary, built early, M1–M2)**: generates IR from the schedule, compiles on a worker thread, targets the host CPU (AVX2/AVX-512 as available). Everything-from-primitives depends on it.
   - **Interpreter (fallback)**: a flat array of op records `{fn*, in-buffer idx…, out idx, state ptr}` with block-wise kernels. It rebuilds in microseconds and plays only during the ~50–200 ms while a fresh compile is running after an edit. Feedback loops in the interpreter run per sample at roughly 10× compiled cost (estimate), acceptable for that window.
   - **AOT stock library**: all stock subcircuits (filters, envelopes, every stock device) are compiled by the same compiler at pqdaw build time and shipped as native kernels. They are fast from first launch and never wait on the JIT. Opening and editing one switches that instance to a JIT-compiled copy.
   - **Compile cache**: JIT objects are cached on disk keyed by graph hash + CPU features, so reopening a project doesn't recompile.

### 7.5 Hot swap (recompile without dropouts)
- A structural edit triggers a debounced (≈50 ms) compile.
- The new Program receives **state transfer** by node ID from the old one: phases, delay buffers, filter states, envelope stages and voice allocations. Nodes whose state layout changed start fresh.
- The swap happens at a block boundary. If any changed node was audible, the old and new programs crossfade for 5–20 ms (double CPU only during the fade). If the transport is stopped and the graph is silent, the swap is instant.
- **Background optimisation**: while the user isn't editing, the compiler may re-run at a higher optimisation level (JIT -O3, wider fusion) and swap again seamlessly. This replaces the "only recompile when not playing" idea: recompiles are always safe, so they don't need to wait for silence.

### 7.6 Cross-track modulation & latency
- Wires may cross tracks. Signals are **latency-compensated by default** (delayed to match plugin delay compensation (PDC) alignment). An option per wire selects raw/zero-latency (only valid when it creates no causality violation).
- A UI meter shows the cost of each patch and wire (ns/sample), similar to SeptaBee's DSP meter.

### 7.7 Silence & sleep
- Each node reports `silent-in → silent-out after tail T`. Voices end when their envelope finishes. Effects sleep when their input has been silent for longer than their tail. Tracks with no active clips and no tail cost zero. Modulation sources that are sleeping output a constant, and the compiler-specialised path handles that without branching per sample (the region switches kernels at block boundaries).

---

## 8. Stock devices

All are atom patches with an Ableton-style panel. The goal is **functional parity**: the same controls, ranges, modes and behaviour, validated by ear and by measurement against Ableton renders (frequency responses, compression curves, saturation transfer curves, decay times). Bit-exact output is not a goal and is not possible. Every internal node output can be exported as a wire source.

| Device | Key parity targets | Notes |
|---|---|---|
| **Operator** | 4 oscillators, 11 algorithms, ratio/fixed frequency, per-osc envelopes, pitch envelope, LFO, filter (Operator's filter types), user waveforms (partial editor → our wavetable draw), glide, spread, voices | Each oscillator's output is exported. Coarse ratios can be set as JI harmonics. Pitch follows the voice pitch signal (any tuning). |
| **Utility** | gain, width, mono, bass mono (freq), phase invert L/R, balance, channel modes, DC | |
| **EQ Eight** | 8 bands; LC 12/48, LS, bell, notch, HS, HC 12/48; stereo/L-R/M-S; adaptive Q; oversampling (HQ); scale; spectrum | Every band freq/gain/Q is audio-rate modulatable (SVF-based for stability under modulation). |
| **Auto Filter** | Live 12 filter types and circuit models (Clean, OSR, MS2, SMP, PRD), drive, morph, LFO, envelope follower, sidechain | Circuit models approximated; listening tests needed. |
| **Saturator** | Analog Clip, Soft Sine, Bass Shaper, Medium/Hard Curve, Sinoid Fold, Digital Clip, Waveshaper (drive/lin/curve/damp/depth/period), Shaper (drawable), color EQ, soft clip, HQ | `shaper` atom + `oversample` region. |
| **Compressor** | threshold, ratio, attack, release, auto-release, knee, **lookahead (0 / 1 / 10 ms)**, Peak/RMS/Expand, lin/log envelope, makeup, dry/wet, **sidechain** (from any track/return/group, pre-FX / post-FX / post-mixer tap, gain, mix with own input, **sidechain EQ** with listen) | Lookahead latency is reported to PDC. The sidechain is just another input port, so the detector's envelope is also exportable as a wire source. |
| **Delay** | L/R sync/time, link, ping-pong, feedback, filter, LFO mod, Repitch/Fade/Jump modes, freeze | |
| **Auto Pan** (Live 12 Auto Pan-Tremolo) | amount, rate/sync, phase, spin, offset, shape, invert, modes | |
| **Reverb** | predelay, input filter, early reflections (spin, shape), diffusion network (size, decay, density, scale, HF/LF damping), chorus, freeze/flat/cut, stereo, reflect/diffuse levels, dry/wet | FDN + allpass diffusers. |
| **Resonator** | 5 resonators (I–V), mode A/B, decay, const, color, width, input filter, per-resonator gain/pitch | **Tuning-aware**: resonator pitches are specified in the current tuning (EDO steps, degrees, JI ratios) and follow audio-rate tuning. |
| **Simpler** | Classic / One-Shot / Slice modes; start/end/loop/fade; warp (uses the clip warp engine); filter, envelopes, LFO, glide, voices; slicing by transient/beat/region/manual → notes | Root pitch is given in Hz or as an identity; playback rate = `voice.hz / rootHz`, so it plays correctly in any tuning. "Slice to new Drum Rack". |
| **EQ Three** | low/mid/high gain with kill, FreqLo/FreqHi, 24/48 dB slopes | |
| **Glue Compressor** | SSL-style bus comp: threshold, ratio 2/4/10, attack, release (incl. auto), makeup, range, soft clip, sidechain, dry/wet | |
| **Limiter** | ceiling, gain, release (auto), lookahead 1.5/3/6 ms, stereo/L-R/M-S link, true-peak option | `lookahead` + `envfollow` atoms. |
| **Gate** | threshold, return (hysteresis), attack, hold, release, floor, flip, lookahead, sidechain | Logic atoms (`schmitt`, `timer`) are the core. |
| **Chorus-Ensemble** | Classic / Ensemble / Vibrato modes, rate, amount, feedback, width, warmth, HP, dry/wet | Modulated `delayline`s. |
| **Phaser-Flanger** | Phaser / Flanger / Doubler modes, notches, center, spread, feedback, LFO + envelope, safe bass, dry/wet | |
| **Spectrum** | PQ Meter spectrum (§11.1), with the tuning-grid overlay | Analysis only; no audio-path cost beyond a ring-buffer copy. |
| **Tuner** | pitch readout as Hz, cents, **nearest note in the track's tuning** (EDO step / MOS degree), and **nearest JI ratio/harmonic** relative to a chosen fundamental | Uses PQ Meter's NSDF pitch tracker. |
| **RMSC** (RM sidechain) | Native port of the user's *Borrasca Sidechainer 3 (1-band)* M4L device, with its defects fixed. See §8.1 | 1-band in v1; multiband (Linkwitz-Riley crossover) later. |

**Sidechain routing (all dynamics and modulation devices)**: Compressor, Glue Compressor, Gate, Limiter, Auto Filter, Auto Pan, RMSC and Spectrum take a sidechain input from any track, return or group. Each source has a tap point (pre-FX, post-FX, post-mixer), gain, and filter/EQ with listen. Sidechain wires are latency-compensated like any other wire (§7.6). Cyclic sidechains (A↔B) get the usual one-sample delay, with a warning.

### 8.1 RMSC: transient-triggered shaper + audio-rate "RM" sidechain
A port of *Borrasca Sidechainer 3 (1-band)*. The private analysis is in `reference/inspiration/borrasca/` (git-ignored, third-party). Written in the circuit language like every device.

**Behaviour**
1. **Trigger**:
   - Onsets come from a FluCoMa-style amplitude-difference detector on the detection source (sidechain, or the main input when SC Input is off): high-pass at 2 kHz, then |x| in dB clamped at Floor, then fast follower (up 3 / down 383 samples) minus slow follower (up 2205 / down 2205 samples).
   - That difference feeds a Schmitt trigger (on and off thresholds set by Sensitivity) with a Lockout (minimum slice, ms), which emits a 1-sample trigger.
   - **MIDI Input** mode triggers on note-on instead.
   - Follower ramps are specified in samples at 44.1 kHz and must be scaled to the actual sample rate.
2. **Two one-shot MSEG envelopes of equal length**, restarted on each trigger:
   - Length is 1–2000 ms, or tempo-synced (1/64 … 4/1, including 1/12, 1/7, 1/6, 1/5, 1/3).
   - **Volume MSEG**: `V = E1·EnvDepth + (1 − EnvDepth)`.
   - **RM MSEG** (E2): sets over time how strongly the RM stage acts. 1 = none, 0 = full.
3. **RM stage** (rectified, audio-rate ducking; no bipolar mode, by decision):
   - `q = 1 − (1 − clamp(|sc|, 0, 1))^RMGain` (RMGain 1–10)
   - `F = slide(q, up = RMSmooth·0.2 ms, down = RMSmooth·5 ms)`
   - `G = Gate ? 1 − F : F`
   - `gain = V · (1 − RMDepth·(1 − E2)·G)`
   - `out = in · gain` (per channel; the sidechain is used per channel).
4. **Pre-fade** (0–10 ms lookahead):
   - Delays audio, F and the envelopes by the pre-fade time and reports that latency to PDC.
   - On each trigger, crossfades from the held previous envelope value to the new envelope's first point using `0.5·(1 − cos(π·t))`.
   - **The same correct formula is used for both envelopes.** The original's RM pre-fade has a step bug, which is not reproduced.
5. **Clipper**: off / soft / hard. Soft is a true `tanh` (or `fastTanh`), not the original's unbounded Padé approximation.

**Controls**:
- Env Depth, RM Depth, RM Gain, RM Smooth, Gate
- Ms Length / Tempo Sync / Synced Length
- Pre-fade, Clip, Clip Knee, SC Input, MIDI Input
- Sensitivity, Lockout, Floor

**MSEG editor**:
- Volume/RM tabs, with the inactive envelope shown ghosted behind.
- Grid (1/2…1/16), snap.
- Presets: Init, S-shape, Flip (y → 1 − y), Copy/Paste between tabs.
- Curved segments (our own documented curve law, applied per segment).
- Rendered to a table at a resolution that is independent of sample rate.

**Displays**: the two scopes from the original, implemented with PQ Meter rendering. Scope 1 is the applied gain over the envelope length. Scope 2 is the rectified sidechain, trigger-synced.

**Improvements over the original**:
- |sc| is clamped (the original's `pow` gives NaN for |sc| > 1).
- The soft clipper is bounded.
- Detector ramps scale with sample rate.
- The detector's own latency can optionally be compensated.
- Every internal signal (trigger, E1, E2, F, gain) is an exported wire source, so RMSC's envelope can modulate anything else at audio rate.

### 8.2 Racks
Racks are Ableton-style containers. Under the hood each is a subcircuit, so the same rack can be built and edited from the rack UI *or* in the circuit editor.
- **Audio Effect Rack**: parallel **chains**, each a device chain with its own volume/pan/mute/solo. A chain mixer. **Chain-select zones** (0–127 equivalent, crossfade ranges) and **macros**.
- **Instrument Rack**: parallel instrument chains with **pitch zones** (defined in Hz, or as a range in the track's tuning; not MIDI note numbers), velocity zones and chain-select zones, all with fades.
- **Drum Rack**: a 4×4 pad view (scrollable), each pad = a chain triggered by a pitch identity (default: consecutive degrees of the track's tuning, or explicit Hz/ratio). Choke groups, per-pad internal sends and returns, "receive"/"play" pitch per pad, drag samples onto pads (creates Simpler).
- **Macros**: up to 16 macro knobs per rack. A macro is a `param` that drives any number of targets, each with a min/max range and curve. Since macros are ordinary signals, a macro can itself be audio-rate modulated, and modulating it modulates all its targets. **Macro variations**: snapshots of all macro values (store/recall/randomize, with exclusions).
- Racks nest arbitrarily. Rack chains can be per-voice (`voicechain`) when the rack sits inside an instrument after a polyphonic source.
- Rack presets save to the user library (§9.1).

---

## 9. DAW fundamentals

Ableton's layout: control bar (top), browser (left), track area (centre), detail view (bottom: clip/device), info view (bottom-left help text on hover).

### 9.1 Browser & user library
- **Places**: user library, project folder, any number of added folders (sample packs), plus categories (Instruments, Audio Effects, Racks, Samples, Clips, Grooves, Tunings, Patches/Subcircuits).
- **Indexing** runs on a background thread with an incremental on-disk index (path, mtime, format, length, sample rate, channels, detected BPM/key/pitch, tags). It is watched for changes with `ReadDirectoryChangesW`.
- **Search** is instant, incremental and fuzzy over names and tags. Filters: type, duration, BPM, detected pitch.
- **Tags/collections**: colour collections (Ableton-style) and free tags.
- **Preview**: click to audition through a dedicated **cue/preview output**. Raw, or **synced to project tempo** (warped) and **transposed into the current tuning**. A scrub-on-click waveform with preview volume. Pre-listen starts within one buffer from a RAM-cached head of the file.
- **Drag in**: into the arrangement (creates an audio track/clip), onto a Simpler/Drum Rack pad, onto a device (hot-swap).
- **Drag out**:
  - clips from the arrangement into the browser (saves a clip/sample to the library);
  - **to Windows Explorer or other apps**, using OLE drag-and-drop with a rendered/consolidated WAV;
  - from Explorer into pqdaw.
- **Hot-swap mode**: swap a device preset, sample or tuning in place while auditioning.
- **Saving to the library**: device presets, rack presets, subcircuits, tunings (.scl export), grooves, clips, track templates. Default presets per device.

### 9.2 Time, tempo & grid
- **Tempo**: project BPM, tap tempo, tempo nudge, tempo automation (ramps). Time-signature changes on the timeline. Metronome (sound, volume, accent, only-while-recording, count-in 1/2/4 bars).
- **Grid** (arrangement and piano roll):
  - **Adaptive** (grid density follows zoom; Ctrl+1 narrow / Ctrl+2 widen) or **fixed** (1/1 … 1/64). Ctrl+3 triplet, Ctrl+4 snap on/off, Ctrl+5 fixed/adaptive.
  - **Right-click grid menu**: Adaptive (Narrowest…Widest) / Fixed (values) / **Straight, Triplet (3), 5-tuplet, 7-tuplet, 11-tuplet** / Snap / Off.
  - Tuplet *n* divides each straight grid cell's parent span into *n* (e.g. 7-tuplet at 1/16 = 7 notes per quarter).
  - All of 3, 5, 7, 11 and their products land exactly on integer ticks, as do straight notes down to 1/256 (PPQ 221,760 = 2⁶·3²·5·7·11). Unsupported primes (13+) still work, with sub-tick rounding.
- **Quantize** (Ctrl+U) to the current grid, including tuplet grids, with amount %, start/end, and quantize to groove.
- **Groove pool**: extract grooves from audio/MIDI clips, a stock groove library, per-clip groove with base/quantize/timing/random/velocity amounts, a global groove amount, and "commit groove".

### 9.3 Tracks, groups & mixer
- **Track types**: Instrument/MIDI, Audio, Group, Return, Master (plus a hidden Cue/Preview bus).
- **Groups**: Ctrl+G groups selected tracks. Groups nest. A group is a summing bus with its **own device chain (group processing)**, volume/pan/sends, fold/unfold, and solo/mute of its members.
- **Mixer**: volume/pan (balance/split stereo), sends (pre/post), returns, mute/solo/arm/monitor (In/Auto/Off), I/O routing (inputs, other tracks, resampling, specific outputs), sidechain sources, track delay, full PDC including modulation wires (§7.6), meters (peak/RMS, PQ Meter style). Mixer section in the arrangement plus a dedicated mixer view.
- **Track management**: colour, rename, resize, fold, freeze, flatten, deactivate (`0`), duplicate, delete, track templates.

### 9.4 Clips & editing
- **Note clips** (MIDI clips): loop on/off, start/end/loop brace, signature, tuning chip, groove. Notes have velocity, release velocity, **chance (probability)**, **velocity range (random)**, and per-note expression curves (§3.1). The editing tools are Ableton's:
  - draw, select, stretch notes, Ctrl+J consolidate, Ctrl+E split
  - legato, reverse, invert (mirrored in tuning space), duplicate loop, crop
  - fold to used notes / fold to scale
  - **Live 12-style transformations and generators** (arpeggiate, strum, ornament, recombine, quantize, span, connect; rhythm/seed/stack generators), all tuning-aware
  - expression lanes (velocity, chance, pitch curves, gain, timbre)
  - **MIDI clip export** (§10)
- **Audio clips**: gain, transpose (cents and tuning steps), detune, reverse, fades in/out, **automatic crossfades between adjacent clips**, warp on/off, consolidate, normalize-on-consolidate, crop.
- **Arrangement editing**: time selection and Ctrl+Shift+D **duplicate time**, Ctrl+I **insert time**, **delete/cut time**, split, consolidate (Ctrl+J), nudge, follow/scroll, overview strip, zoom to selection (Z/X), locators (set/jump/rename), arrangement loop, punch in/out.
- **Undo/redo**: unlimited, covers every edit including parameter changes, with an undo history panel.
- **Warp**: warp markers, BPM detection, transient markers, auto-warp. Modes:
  - **Re-Pitch**: resampling (high-quality polyphase/sinc).
  - **Beats**: transient slicing with granulation settings (preserve, transient loop mode, envelope).
  - **Tones / Texture**: granular (grain size, flux).
  - **Complex / Complex Pro**: signalsmith-stretch (formants, envelope). Equivalent behaviour, not Ableton's exact sound (Ableton licenses zplane élastique). Low priority.
  - Warp is a native `clipPlayer` atom. Its transpose/rate are modulatable (audio-rate where the algorithm allows; block-rate for stretchers).
### 9.5 Recording
- **Audio** goes to WAV (32-bit float default, FLAC optional) through a disk-writer thread (preallocated ring buffer), multi-input. Count-in, punch in/out, **recording latency offset** (automatic from the driver, plus manual adjustment).
- **Notes** from MIDI controllers/MPE/computer keyboard are stored either **quantised to the tuning** (`Degree` identity) or **raw** (`Cents`), with an option to snap later. Overdub mode. Optional record quantization.
- **Take lanes + comping**: loop recording creates a take lane per pass, for audio and note clips. Swipe-select regions across lanes to build the comp. Comp edits are non-destructive, and you can audition a lane.
- **Capture MIDI**: an always-on input buffer per armed/monitored track (preallocated, last N minutes). Capture creates a clip of what was just played and infers the tempo if the transport was stopped.

### 9.6 Automation
- Breakpoint lanes per parameter (show/hide, A toggles automation mode), with **curved segments** (Alt-drag) and shapes (insert ramp/sine/square/etc. from a menu).
- **Recording**: arm, record knob moves (touch/latch behaviour), and a "Re-enable automation" button after manual override.
- Automation follows clips when moved (optional, Ableton's "lock envelopes" toggle).
- Automation **is a signal source**: rendered per sample (incrementally, near-zero cost) and treated exactly like a modulation wire, so automation and modulation share one mechanism. Automating a **tuning** (§3.2) is the same as automating any parameter.

### 9.7 Control mapping
- **MIDI Map mode** (Ctrl+M) and **Key Map mode** (Ctrl+K), as in Ableton: click a parameter, then move a controller or press a key. Per mapping: min/max, takeover mode (pickup/value scaling/none), toggle/momentary for keys and buttons.
- Mappings are signals too, so a controller knob can be an audio-rate-smoothed modulation source.
- Computer MIDI keyboard (M), with octave/equave and velocity keys, mapped into the current tuning.

### 9.8 Render, export & project management
- **Export audio** (Ctrl+Shift+R): master or **all tracks individually (stems)**, a selected time range or the loop. WAV/FLAC/MP3, bit depth, sample rate, normalize, dither (TPDF/noise-shaped), include return/master effects, render as loop (tail wraps into the start). Offline faster than realtime, using the same Program.
- **Freeze** (renders the track to a temporary audio file; frozen devices cost 0 CPU) and **flatten** (replaces the track with audio).
- **Resample / bounce**: "Resampling" input routing, bounce to new track, consolidate time selection to audio (Ctrl+J on audio across tracks).
- **Collect all and save**, **missing-file search/relink**, save-as with samples, project templates, autosave and crash recovery (§12).
- **Preferences**: audio device, driver type (ASIO/WASAPI), sample rate, buffer size, input/output channel configuration, cue output, latency readout, MIDI ports, look (theme/zoom/scale), library locations, CPU/thread settings.
- **Status**: CPU meter (DSP load per Program region, overall), disk overload indicator, buffer-underrun counter.

---

## 10. Interoperability (compatibility layers)

- **Scala**: `.scl`/`.kbm` import to set a clip/track/project tuning (primary use). Export any tuning (EDO/MOS/JI set) to `.scl`/`.kbm`.
- **MIDI files**: import (12edo; MTS sysex; pitch-bend + channel per note → `Cents`), export as plain 12edo, as "EDO mapped" (mos-explorer's `listValidEdos`/`edoDegreeSteps`), as MPE, or with MTS.
- **MIDI input**: controllers, MPE, isomorphic layouts (Lumatone/Exquis-style mappings into the current tuning).
- **External synths** (only when needed, e.g. a synth that only takes MIDI): MTS-ESP master (block-rate tuning tables), MPE output (≤15 voices/zone, configurable bend range), MIDI 2.0 per-note pitch later.

---

## 11. UI & rendering

- **Vulkan renderer**, custom UI toolkit (retained widget tree; dirty-rect/damage-driven redraw; render only on change or animation).
- **Draw-list layer compatible with PQ Meter**: widgets emit triangles into a draw list whose vertex format is `{pos float2, uv float2, rgba8}` with **32-bit indices**. This is ImGui's `ImDrawVert` layout, so PQ Meter's primitives (`smooth_line.h`, `gradientPolyline`/`gradientBand`/`splineSmooth`, `gradient.h`, `palette_track.h`) port by swapping `PrimWriteVtx` for our writer. Separate pipelines handle alpha blending, additive blending (glow sprites/heat dots) and textured quads (spectrogram, waveform overviews).
- **Antialiasing is geometric, as in PQ Meter**: lines are feathered ribbons (opaque core plus a 0.75 px fade on each side), miters clamped at 2.5×, Catmull-Rom subdivision. No MSAA.
- **Other primitives**: SDF rounded rects/borders, bezier wires and automation curves (as feathered ribbons), waveform/overview textures (mip-chain computed on the analysis thread), image atlas.
- **Text**: crisp hinted bitmap glyphs at integer pixel sizes (body 13 px / title 14 / section 12, PQ Meter's sizes). Do not use MSDF or unhinted vector text; PQ Meter tried that and rejected it as blurry.
- **Frame pacing**: on Windows, wait on `DwmFlush()` rather than driver vsync (same reasoning as PQ Meter: it avoids the NVIDIA busy-wait). Redraw on damage only.
- **Look**: Ableton's default dark theme as a token set (colours, font sizes, spacing). Themes are data files.
- **Wiring**: output jacks along the top edge of each device. Drag to any knob/parameter (including on other tracks, auto-scrolling). Wires drawn as bezier cables over the UI, coloured by rate (param vs audio vs voice), with a show/hide/fade toggle. A modulated knob shows a live ring of its modulation range.
- **Patch editor**: opens any device or rack into its circuit-diagram view (§7.3). A side panel shows the text form (two-way synced).
- **Performance targets**: 144 Hz capable; idle UI ≈ 0% CPU/GPU; first frame < 300 ms after launch.

### 11.1 Visualisers (from PQ Meter)
All signal displays reuse PQ Meter's DSP, ballistics and look, which the owner has already tuned. Analysis runs on the analysis thread and reads audio from per-tap lock-free rings. These are taps placed on the audio thread: no allocation, they just copy into the ring. The UI thread draws from snapshots.

- **Where they appear**: EQ Eight's spectrum, Auto Filter / Compressor / Saturator displays, track and master meters, Spectrum and Oscilloscope utility devices (droppable anywhere in a chain), patch-editor probes (a scope on any wire), and the Resonator/Operator wave displays.
- **Spectrum** (port `spectrum.cpp` analysis plus `dsp/spectrum_grid.h`):
  - FFT 2048 with hop 512 (Hann), plus a 4× low window crossfaded below 500 Hz.
  - 1024 log points using the tone-peak / noise-level reader.
  - Optional 1/N-octave smoothing.
  - Ballistics: attack 25 ms, release 350 ms. Peak hold 1.5 s, then 12 dB/s fall. A 45 ms ease on the UI side.
  - Tilt −12…+12 dB/oct pivoted at 1 kHz, default 4.5.
  - dB range −78…0 with a 12 dB grid.
  - Layering: fill fading to the bottom, level-weighted 3-pass glow, body, bright core.
- **Oscilloscope** (port `oscilloscope.cpp`):
  - McLeod NSDF pitch tracking with whole-shape alignment sync against the previous frame. Edge-trigger fallback.
  - Constant-span lock, 52.2 ms default.
  - Catmull-Rom up to 4 samples per pixel; a min/max band above that.
  - Phosphor trails and glow.
  - **Pitch-aware addition**: inside the DAW the scope can sync to the *known* voice pitch from the engine, which is exact even for JI or morphing tunings, instead of estimating it.
- **Pitch overlays**: spectrum and scope grids can show the current tuning (EDO steps, MOS degrees, the harmonics of a JI object's fundamental) instead of, or alongside, Hz gridlines.
- **Also available to port**: spectrogram (reassigned), stereo field/goniometer, LUFS (BS.1770-4), multiband RMS, waveform envelope. The `pq_tests` reference-signal tests come along with the code.
- **Theme tokens** start from PQ Meter's `theme.h` and `settings_theme.h`:
  - Backgrounds: #0A0A0E / #101016 / #181820.
  - Accent #78BEFF, violet #AC8EFF.
  - Grid white at α14, grid labels white at α70.
  - Band colours.
  - The 10 colormaps, plus the user's custom map "so sick".
  - **Decided**: the whole DAW uses Ableton's dark-grey theme. PQ Meter's palette, glow and line rendering apply **only to signal displays** (spectra, scopes, meters, waveforms, modulation rings, wires).
- **Porting work**: split PQ Meter's module classes into analyser (no UI) + view (draw-list only). Replace the GL texture uploads and blend-switch callbacks with Vulkan pipelines. PQ Meter itself can later move onto the shared analyser library.

---

## 12. Project format

- A project is a directory: `project.pqd` (text, versioned schema, human-readable), `patches/*.pqp` (patch-language text), `samples/`, `cache/` (analysis, waveform overviews, JIT objects; deletable).
- Undo history is in memory (optionally persisted).
- Autosave + crash recovery.

---

## 13. Performance & safety rules (non-negotiable)

- The audio thread: no `new`/`malloc`, no mutex, no syscalls, no logging I/O, no exceptions, no virtual dispatch inside per-sample loops (dispatch per op per block only).
- All UI↔audio communication goes through SPSC/MPSC lock-free queues or atomically published immutable data. Reclamation happens off the audio thread.
- Denormals flushed (FTZ/DAZ). Buffers aligned to 64 bytes. No false sharing between threads.
- CI benchmarks per atom and per stock device (ns/sample/voice), with regression thresholds.
- Offline render path = the same Program, deterministic. Golden-file tests compare renders across versions.
- Example targets (to calibrate after M1): Operator 64 voices < 10% of one core at 48 kHz; an unmodulated EQ Eight band ≈ cost of one biquad; an idle project ≈ 0%.

---

## 14. Milestones

| M | Deliverable |
|---|---|
| M0 | Window + Vulkan renderer + text; RtAudio ASIO/WASAPI; RT-safe scaffolding (queues, arenas, epoch reclamation); sine out |
| M1 | Graph IR, rate/kind/poly inference, interpreter backend, hot swap with state transfer, leaf atoms (incl. logic, `solve`), offline render + golden tests, benchmarks |
| M1b | LLVM ORC JIT backend, AOT build of the stock subcircuit library, compile cache; filters/envelopes as primitive subcircuits benchmarked against hand-written C++ reference implementations (target: within ~10%) |
| M2 | Tuning library: EDO, MOS (mos-explorer port, verified against JS outputs), Scala, JI; pitch identities; multiple main tunings; tuning-as-signal |
| M3 | Arrangement view, tracks + groups + mixer basics, note clips, piano roll (EDO / proportional MOS / equidistant), MOS wheel, adaptive/tuplet grid, Ableton hotkeys, automation lanes, undo |
| M4 | Circuit-diagram patch editor + text form; device panels; cross-device wiring; racks (Audio Effect / Instrument) + macros; Operator, Utility, EQ Eight, Auto Filter, Saturator |
| M5 | Browser + user library (index, search, preview, drag in/out); sample atom; Simpler; Drum Rack |
| M6 | Audio clips, warp modes, recording, take lanes/comping, Capture MIDI, PDC, metronome |
| M7 | JI objects (definition, inner roll, morph, voice add/remove, walk, ride curve, pulse arp, chord sets, pen modes) |
| M8 | Macro mode (layers, virtual-fundamental lock, keymap presets); MIDI/key mapping |
| M9 | Compressor, Glue, Limiter, Gate, EQ Three, Delay, Auto Pan, Chorus-Ensemble, Phaser-Flanger, Reverb, Resonator, Spectrum, Tuner; parity measurement suite against Live 12 renders |
| M10 | Groove pool, note transformations/generators, export/stems, freeze/flatten, resample/bounce, collect & save |
| M11 | Interop: MIDI files, controller input/MPE, MTS-ESP, MPE out, Scala export |
| M12 | Background re-optimisation passes; multithreaded SCC scheduling |

---

## 15. Decisions log

- Name: **pqdaw**.
- Non-commercial, personal use; no commercial licences (no élastique; Complex/Pro via signalsmith-stretch, low priority).
- Live 12 Suite available for parity reference renders.
- Theme: Ableton dark grey throughout; PQ Meter styling only on signal displays.
- Font: hinted Ableton-like sans (Inter or IBM Plex Sans) at 12/13/14 px as bitmap glyphs; PQ Meter's font stays on signal readouts.
- Reference pitch: C3 = 261.626 Hz (Ableton naming), per project/tuning/clip.
- Multiple named main tunings; tracks follow one; each automatable independently.
- Ratio-note Up/Down = next harmonic. Fundamental lock = `b` + index (`b5`).
- Scale-step layer: 1=degree, 2=generator, 3=L, 4=s, 5=period, 6–0=+2…+6 degrees.
- Poly→global modulation default = sum (per-wire override).
- Undertone JI objects = flag on the same object type.
- v1 instruments: Operator, Simpler, Instrument Rack, Drum Rack (Sampler later). v1 effects: the original 10 + EQ Three, Glue, Limiter, Gate, Chorus-Ensemble, Phaser-Flanger, Spectrum, Tuner, all built in the circuit language.
- Take lanes/comping, Capture MIDI, groove pool, MIDI/key mapping, full export/freeze/bounce/collect are v1.
- RMSC: full port of Borrasca Sidechainer 3 (1-band), rectified RM only, multiband later. Compressor has sidechain (any source, tap point, EQ) and lookahead 0/1/10 ms.
- The low-level language is a circuit diagram (schematic symbols, orthogonal wires, boolean logic atoms).

- 2026-10-21 (user answers to docs/QUESTIONS.md):
  - PPQ = 221,760 (Q3): exact straight notes to 1/256 plus 3/5/7/9/11-tuplets.
  - ASIO is required (Q12): own clean-room ASIO host; audio preferences let the user choose driver type (ASIO / WASAPI shared / exclusive), device, sample rate, buffer size and I/O channels, as in every DAW.
  - Ratio notes share a virtual fundamental derived from what's entered; it updates as notes change (Q40).
  - Default project is empty; mute/deactivate as in Ableton: the track activator mutes, `0` toggles activator on tracks and deactivates clips (Q15).
  - Theme reference: the user's Live 12 Arrangement screenshot (Q47; kept out of the repo).
- 2026-10-21 (user, M1 review H6): compile granularity is **per track**: each track's device chain is compiled and optimised as its own unit; an edit recompiles and crossfades only that unit. Units are executed in dependency order within each block (no added latency); tracks joined by cross-track modulation/sidechain wires are merged into one unit (as §7.4 already says for cross-track SCCs). A whole-project re-optimisation pass when the user pauses editing (§7.5 background optimisation) is planned for later (M12), out of scope for now.
- 2026-10-24 (user, sketch "VISUAL PROGRAMMING VIEW / INTERPRETATION OF PROJECT FILE"): **project patch view**: the whole project can be shown as one circuit-language patch, as a view mode of the arrangement, so any routing and modulation is visible and editable in one place.
  - **Model**: it draws the project graph the engine runs (per-track units, T1.x-arch1; lowering, T3.17): each track is a band with a source node (MIDI input node playing that lane's clips, audio input node playing its audio clips) → its device chain → a compact mixer-strip node; sends are wires to the return bands; groups are containers around their members with their own chain; master on the right.
  - **Fully editable**, two-way with the Ableton view (one model, like text ⇄ diagram): wiring any output to any parameter anywhere (cross-device modulation), re-routing a track to another group or return, adding/removing devices on a band all change the project.
  - **Layout mirrors the arrangement**: bands in lane order, groups as containers, signal left → right, auto-laid out with optional stored manual nudges.
  - **Semantic zoom**: zoomed in on one band you edit its chain (down to subcircuits and atoms, §7.3); zoomed out you see the whole project's routing.
  - **Transition**: an animated morph from the arrangement — lanes keep their vertical position and unfold into their bands (clips collapse into the source node), and back.
  - **Mixer**: a compact, expandable mixer-strip node at the end of each band (volume, pan, mute/solo, sends as visible wires).
- 2026-10-24 (user answers to docs/plans/M4.md):
  - **Device parity references**: no Live renders for now; stock devices are built and checked against known formulas, published behaviour and memory, and adjusted later.
  - **Device panel look**: our call for now (Live-like where known), adjustable later.
  - **Adding devices before the M5 browser**: a device menu / search popup on the device chain.
  - **Cross-device and cross-track wires add no delay.** This replaces §7.6's "latency-compensated by default" for wires. The one exception: a wire that would make the graph cyclic (closing a loop that has no delay in it) gets a **1-sample delay (z⁻¹)** on that wire, automatically, shown in the UI. Explicit in-patch feedback keeps its own semantics (z⁻¹ / `solve`, §7.1). Track-level plugin delay compensation of audio paths (M6) is unaffected.

No open questions remain for draft 1.
