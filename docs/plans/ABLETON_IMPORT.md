# Ableton Live Set import: research and plan

Status: research + proposal, 2026-10-25. Nothing here is a SPEC decision yet; §11 lists what the user has to decide. Written so a worker can turn §10 into `docs/TASKS.md` entries.

**Goal (user, 2026-10-25):** switch from Ableton to pqdaw and bring every old project across, "ideally with minimum change in audio".

**Short answer.**
- Bitwig shows that a best-effort `.als` importer is practical.
- Bitwig itself is not a high bar for audio fidelity: it rebuilds the structure, roughly approximates stock devices, restores VST state, and drops what it can't map.
- pqdaw can do much better on this corpus. Three facts measured on your own projects (§3) drive the plan:
  1. **51.6 % of your 1.47 M arrangement audio clips can be reproduced sample-exactly.** They are unwarped, or warped at 1:1 in Beats, Tones, Texture or Re-Pitch, with no transpose.
  2. **96 % of projects use third-party plugins** (Serum in 70 % of them). Plugin hosting with state restore is therefore not optional: it is the single largest fidelity item, and SPEC §1 currently puts it out of v1.
  3. **Max for Live is in 78 % of projects**, and the bulk of it is *modulators* (LFO, The Spellbook, MultiMap, Envelope Follower). pqdaw's audio-rate cross-device wires are a better home for these than anything Bitwig has. Racks and Live devices outside the v1 list (OTT/Multiband Dynamics in 87 % of projects, Echo, Vocoder…) are the long tail.

---

## 1. What "minimum change in audio" can mean

There are four fidelity tiers. Every imported element is classified into one of them, and the import report (§8) shows the classification per track.

| Tier | Meaning | Achievable for |
|---|---|---|
| **E (exact)** | Null test against a Live render ≤ −90 dBFS residual. In practice this is bit-identical except for float summation order. | Structure, timing, MIDI, unwarped and 1:1-warped audio, gains, the pan law, sends, routing, and most third-party plugins once their state is restored |
| **N (near)** | Residual below audibility. Spectral comparator ≤ 0.2 dB per semitone band (the `tests/golden/golden_audio.h` metric), loudness within 0.1 LU. | Re-Pitch and other resampling (the interpolation kernel differs), simple stock devices with known maths (Utility, EQ Eight, Delay, Auto Pan), Simpler, Operator |
| **A (approximate)** | Same intent, audibly different on close listening. | Beats and Tones/Texture when stretched, Complex/Complex Pro, Live's nonlinear devices (Saturator curves, Glue, Roar, Amp…), Max for Live ports |
| **X (missing)** | Cannot be reproduced. Kept as a bypassed placeholder with parameters and automation preserved, and flagged. | Devices with no pqdaw equivalent yet, plugins not installed, Max devices with no port, Ableton-codec pack samples |

There is an escape hatch for **A** and **X**: **bake in Live before migrating** (freeze/flatten or consolidate). Live is still installed (12.2.7 Suite), so for archived projects this guarantees E-tier audio at the cost of editability. §9 turns it into a triage workflow.

---

## 2. How Bitwig does it (and everyone else)

Sources: Bitwig support article "Importing Ableton Live Sets" (https://www.bitwig.com/support/technical_support/40/), release notes from 4.0 to 5.2 (downloads.bitwig.com), and KVR/Elektronauts user reports. Full report: §12.

- **Since Bitwig 4.0 (13 July 2021), for sets saved in Live 8.1 or later.**
- **Imported:**
  - arrangement clips, tempo, time signatures, cue markers, automation;
  - clip launcher scenes and clips;
  - audio/MIDI/group tracks with output routing;
  - MPE, note chance and velocity spread;
  - clip pitch/volume and warping, with "warp modes mapped to the nearest Bitwig equivalent" (no table published).
- **Not imported:** input routings, AU plugins, and Drum Rack chains whose Receive is "All".
- **Native devices:**
  - Simpler/Sampler become Bitwig Sampler "with rough parameters".
  - Some synths become "a similar Bitwig synth" with **no** parameters.
  - Common effects get "the most important parameters, just to get in the general area".
  - Racks become containers.
  - Anything else becomes an **empty Chain named after the original**.
  - Nothing is ever bounced.
- **Plugins:** VST2/VST3 state is restored and automation imported. Users report VST2 working well, while VST3 automation was "hit or miss" (wrong or unknown parameters, KVR 2024). The likely cause is mapping Live's `ParameterId` onto VST3 `ParamID`s (inference).
- **Fix history:**
  - device chains on groups (4.0.2);
  - sends below −70 dB (4.0.4);
  - negative warp-marker times (4.1.5);
  - nested group outputs (4.2);
  - pitch bend/CC envelopes (4.3);
  - unsorted automation events (4.3.1);
  - volume levels (5.0);
  - **tempo missing from Live 12 sets (5.2, July 2024)**.

  Lesson: the format drifts across Live versions, so the importer needs a per-version regression corpus.
- **Other tools:**
  - **No other major DAW imports `.als` natively.**
  - Ableton does **not** support DAWproject (MIT, Bitwig/PreSonus), so DAWproject is no route.
  - AATranslator (commercial) skips plugins and FX.
  - DawVert (GPL-3) reads Live 11 only.
  - dawtool (BSD-3) reads tempo maps and markers only.
  - **SUBstation** (https://github.com/OpenMusicStuff/SUBstation, **MIT**, C++, active October 2026) is the closest reference implementation. It handles: tempo/time-signature map, tracks/groups/returns, routing and sidechains (including from Drum Rack pads), MIDI loop spans, warp maps, fades, grooves, VST3 (`.vstpreset` built from the two states), VST2 chunks, ~15 native devices, Drum Racks and automation. Its design notes are `docs/app/live-import.md`.
- **Licensing rule for workers:** DawVert, abletoolz and go.dot are **GPL-3**: read them as format documentation, never copy code. SUBstation, AbleSharp, kevinkirsten/ableton-als and dawtool (except its FL file) are MIT/BSD, so code ideas can be reused with attribution (record any copied code in `docs/DEPENDENCIES.md`).

**What pqdaw does differently from Bitwig:**
1. The target is fidelity, not "the general area". Every translated stock device is measured against Live (§7).
2. Live's modulators become native audio-rate wires instead of being dropped.
3. Tuning systems and MPE pitch become pitch identities (EDO degrees, ratios) that can be edited in the piano roll.
4. Untranslatable devices keep their full raw XML, so a later importer version can **re-translate** them without the original `.als`.

---

## 3. Your corpus (measured 2026-10-25, read-only)

Scripts and raw JSON are in the session scratchpad (`als_inventory/`, `warp_ratio/`). They should be promoted to `tools/als/` as part of task AL1 (§10). Scope: **1,236 distinct sets in 1,005 project folders**, all parsed with zero errors. Deduplication drops the E:/F: mirrors and the 8,245 `Backup/` and crash copies. Live installed: 12.2.7 Suite.

**Versions:** Live 9: 77 · Live 10: 526 · Live 11: 258 · Live 12: 375. Dates from 2016-06 to 2026-10.

**Shape:**
- **Tracks** (mean / median / max): 42.5 / 32.5 / 207. Per mean set: 23 audio, 11 MIDI, 6 group, 2.3 return.
- **395 frozen tracks.**
- **Arrangement only:** 1,115 sets use only the arrangement; just 103 touch the session view.
- **Take lanes in 500 sets.**

**Audio clips:** 1.47 M arrangement clips; 4.8 M warp markers; 42 % with a non-default fade; 17 % transposed. Classified by what reproducing them needs:

| Class | Clips | Share | Tier |
|---|---|---|---|
| Unwarped, no transpose | 580,601 | 39.5 % | **E** |
| Warped at exactly 1:1 (Beats/Tones/Texture/Re-Pitch), no transpose | 178,839 | 12.2 % | **E** (Live's audio fact sheet: these modes are bit-transparent at the original tempo) |
| Pure resampling (unwarped + transpose, or Re-Pitch) | 129,827 | 8.8 % | **N** |
| Beats, stretched and/or transposed | 405,452 | 27.6 % | **A → N** with careful work (§6.5) |
| Complex / Complex Pro | 138,224 | 9.4 % | **A** (zplane élastique, licensed; not reproducible) |
| Tones / Texture, stretched | 36,203 | 2.5 % | **A** |
| WarpMode 5 (probably REX) | 2,073 | 0.1 % | **X** until REX is decoded |

How the 1:1 test is computed: every warp-marker segment's ratio (Δbeat·60/bpm)/Δsec is within 0.1 % of 1 at the set's static tempo. The 39 sets with tempo automation are slightly misclassified by this test.

**Samples:**
- 47.9 k unique references; **33 % are missing on disk today**. Stale `D:\BACKUP\…` and recording paths, so relinking is a first-class feature.
- **Only 454 of 1,236 sets have every sample present.**
- Reversed clips point to Live-rendered `… R.wav` files (3,838 of them), so reverse needs no implementation for import.

**MIDI and automation:**
- 11.3 M notes, **6 % deactivated** (must be kept).
- Probability and velocity deviation are effectively unused (one set each).
- Per-note expression in 269 sets (pressure 411 k, pitch 310 k, slide 302 k events).
- 55 k track envelopes (2.1 M events) and 33.6 k clip envelopes (556 sets).
- **Tempo automation in only 39 sets; time-signature changes in 5.**
- Grooves are applied in only 16 sets.

**Native devices** (sets using it / instances, top of 79):

| Device | sets / instances | | Device | sets / instances |
|---|---|---|---|---|
| EQ Eight | 1196 / 33.8k | | Operator | 716 / 3.3k |
| Audio Effect Rack | 1120 / 23.3k | | Arpeggiator | 694 / 2.0k |
| Multiband Dynamics (OTT) | 1070 / 15.3k | | Vocoder | 566 / 3.3k |
| Utility | 1065 / 25.3k | | Erosion | 559 / 2.0k |
| Compressor | 1009 / 11.6k | | Drum Buss | 525 / 1.4k |
| Auto Filter | 992 / 12.6k | | Grain Delay | 484 / 1.3k |
| Saturator | 952 / 6.1k | | Overdrive | 469 / 2.1k |
| Echo | 885 / 4.8k | | Limiter | 446 / 1.0k |
| Auto Pan | 842 / 5.2k | | Gate | 426 / 0.6k |
| Simpler | 825 / 5.4k | | Redux (legacy + 2) | 384+200 |
| Delay | 823 / 7.6k | | Frequency Shifter | 373 / 1.1k |
| Reverb | 807 / 2.0k | | Wavetable | 366 / 1.4k |
| Instrument Rack | 797 / 2.8k | | Simple/Ping Pong Delay (legacy) | 286+216 |
| Glue Compressor | 751 / 4.3k | | Roar, Amp, Corpus, Shifter, Phaser (legacy), Spectral, Resonator, Collision, Sampler… | 85–272 each |

Racks: 26.6 k racks, 49.5 k chains; 84.5 k non-zero macros (heavily used).

**Plugins:**
- 142 distinct; 10,952 VST2 and 7,610 VST3 instances; 1,178 / 1,236 sets.
- **Top:** Serum (870 sets), Ozone 8 Elements (857), ValhallaVintageVerb (749), Wider (476 VST2 + 230 VST3), ST4b (419), Youlean (408), soothe2 (403 VST3 + 187 VST2), bloom (386), Addictive Drums 2 (289), Harmor (279), Pigments (246), Dimension Expander (236), Synplant (197), Reaktor 6 (157), SerumFX (156).
- The same plugin often appears in both formats, so matching has to work across formats (§6.7).
- Only 7 sets use AU, which is irrelevant on Windows.

**Max for Live** (966 sets):

| Device | Instances | What it is | Category |
|---|---|---|---|
| The Spellbook | 2,555 | 8-slot shape/rotation LFO with per-mapping rate, phase, invert and offset (read from its `.amxd`) | Modulator |
| LFO | 1,777 | | Modulator |
| MultiMap | 1,667 | | Modulator |
| Envelope Follower | 968 | | Modulator |
| Expression Control | 28 | | Modulator |
| Shaper | 15 | | Modulator |
| MultiMapper32, XYZmap | | | Modulator |
| **Borrasca Sidechainer 3** | 973 | Already ported as **RMSC**, SPEC §8.1 | DSP |
| Formant | 365 | | DSP |
| Spray | 96 | | DSP |
| Convolution Reverb, Align Delay | | | DSP |
| MPE Control | 134 | | MIDI / pitch |
| Microtuner | 87 | | MIDI / pitch |

**Microtonality: how your xen music is actually built**

| Technique | Live 10 | Live 11 | Live 12 |
|---|---|---|---|
| Live 12 `TuningSystem` (song level) | – | – | **116 sets** |
| Microtuner (Max) | – | 47 | 27 |
| MPE Control (Max) | – | 15 | 101 |
| Per-note pitch-bend lists | – | 21 | 31 |
| Pitch MIDI effect | 4 | 31 | 13 |

- **Tuning systems are almost all EDOs:** 23 (37 sets), 22 (24), 17, 26, 20, 13 and many others. There are also Harmonics 12–24, MOS 11L 12s, Salendro, and one non-octave "8 equal divisions of 833.09 ¢" (that equave is φ).
- 77 of them came from Scale Workshop. Notes are stored in cents; only 4 carry ratio data.
- **No MTS-ESP and no `.scl`/`.kbm` references anywhere.**
- **MPE-enabled plugins:** Serum (237 sets), Pigments (188), Serum 2, ST4b, UVI. These receive the tuning as MPE pitch bend.

---

## 4. The `.als` format (what the importer must read)

There is no official spec: everything is reverse-engineered. Verified facts are below. **[?]** marks an open question that needs a fixture set from Live (§7.3).

**Container and versions**
- gzip XML, UTF-8. A few Live 12 files are plain XML: detect the `1F 8B` magic.
- Size: a 3 MB file inflates to about 55 MB and ~750 k elements, so **stream it**. Never build a DOM of everything. SUBstation skips session slots, freeze sequencers and views, and reads such a set in 2 s using 330 MB. pqdaw should do better with a streaming SAX reader into a typed model.
- Root element: `<Ableton MajorVersion="5" MinorVersion="12.0_12203" Creator="Ableton Live 12.2" …>`. Live 9 is `MajorVersion="4"`.

| Change | Live 9 | Live 10 | Live 11 | Live 12 |
|---|---|---|---|---|
| Master track element | `MasterTrack` | `MasterTrack` | `MasterTrack` | `MainTrack` |
| Master output target | `AudioOut/Master` | `AudioOut/Master` | `AudioOut/Master` | `AudioOut/Main` |
| Where automation lives | Inline `ArrangerAutomation` on each parameter | Track-level `AutomationEnvelopes` | Track-level | Track-level |
| Colour | `ColorIndex` | `ColorIndex` | `Color` | `Color` |
| Sample `FileRef` | Hex `Data` (UTF-16 Windows path) + `SearchHint` | Hex `Data` + `SearchHint` | `Path`, `RelativePath`, `RelativePathType`, `OriginalFileSize`, `OriginalCrc` | Same as Live 11 |

Live 12 also adds tuning systems, `IsTuned`, MPE settings, and `TrackSendHolder/EnabledByUser`.

**Ids and automation**
- `Pointee`, `AutomationTarget`, `ModulationTarget` and `ControllerTargets` ids share one set-wide id space (below `LiveSet/NextPointeeId`).
- A parameter is `<Param><Manual Value/><MidiControllerRange Min Max/><AutomationTarget Id/><ModulationTarget Id/></Param>`.
- Track `AutomationEnvelopes/…/EnvelopeTarget/PointeeId` refers to an `AutomationTarget`. Events are `FloatEvent|BoolEvent|EnumEvent Time Value`, with times in quarter-note beats.
- The first event of every envelope sits at `Time="-63072000"` (pre-roll value).
- `FloatEvent` may carry Bézier `CurveControl1X/1Y/2X/2Y`. **[?]** The exact curve formula.

**Tempo and time**
- Tempo is at `MainTrack/DeviceChain/Mixer/Tempo` (BPM, automatable).
- `TimeSignature` is an enum: `(num − 1) + 99·log2(den)`.
- **[?]** Tempo ramps: dawtool reports (and claims to have matched on more than 10 k sets) that Live holds tempo constant per 1/16 inside a ramp. If true, seconds = a piecewise sum, not the log integral.

**Clips**
- Arrangement clips are in `MainSequencer/ClipTimeable/ArrangerAutomation/Events` (MIDI) and `MainSequencer/Sample/ArrangerAutomation/Events` (audio). Take lanes are in `TakeLanes`.
- `CurrentStart/End` are in arrangement beats. `Loop/LoopStart/LoopEnd/StartRelative/LoopOn` are in content beats; **for unwarped audio they are in seconds**.
- Notes: `KeyTrack{MidiNoteEvent Time Duration Velocity VelocityDeviation OffVelocity Probability IsEnabled NoteId; MidiKey}`.
- Per-note expression: `PerNoteEventStore/PerNoteEventList NoteId CC` (−2 pitch, −1 pressure, 74 slide), with `TimeOffset` in beats. **[?]** The pitch scale factor (DawVert uses 170 per semitone; probably 8192/48).
- Clip envelopes: `ClipEnvelope/EnvelopeTarget/PointeeId`, pointing at an `AutomationTarget` (clip automation) or a `ModulationTarget` (relative modulation).
- MIDI CC lanes: `ControllerTargets.N` (0 = pitch bend, 1 = channel pressure, k + 2 = CC k).

**Audio**
- `SampleRef/FileRef`. `RelativePathType`: 1 = project-relative, 3 = in-project, 5 = pack, 6 = User Library, 7 = Live resources; 0 = absolute.
- `WarpMarkers{SecTime BeatTime}`, `IsWarped`.
- `WarpMode`: 0 Beats, 1 Tones, 2 Texture, 3 Re-Pitch, 4 Complex, 6 Complex Pro, 5 **[?]** (REX).
- Per-mode parameters: `TransientResolution`, `TransientLoopMode`, `TransientEnvelope`, `GranularityTones/Texture`, `FluctuationTexture`, `ComplexProFormants/Envelope`. **[?]** The enum orders.
- Clip parameters: `PitchCoarse/Fine`, `SampleVolume` (linear), `Fade` (≤ 4 ms declick), `Fades/FadeIn/OutLength` + `CurveSkew/Slope` **[?]** (curve law), `HiQ`, `Ram`.
- Pack samples may be AIFF-C with Ableton's own `able` codec, which cannot be decoded outside Live.

**Mixer and routing**
- Volume and sends are linear gain (1 = 0 dB, 1.99526 = +6 dB, 0.000316 = −70 dB ≈ −∞).
- `Pan` −1..1; `PanMode` 1 = split stereo (`SplitStereoPanL/R`).
- `Speaker` is the track activator.
- Pre/post sends are in `LiveSet/SendsPre`.
- Routing `Target` strings: `AudioOut/Main`, `AudioOut/GroupTrack`, `AudioOut/Track.<TrackId>/TrackIn`, `AudioIn/Track.<Id>/{PreFxOut|PostFxOut|TrackOut}`, `…/DeviceOut.<dev>.B<branch>` (from a Drum Rack pad), `AudioOut/External/S0`.
- `TrackGroupId` gives group membership.
- Sidechains are at `<Device>/SideChain/RoutedInput/Routable/Target`. Resolve ids in two passes.

**Devices**
- Native devices sit beside `PluginDevice` and `MxDevice*` in `DeviceChain/Devices`.
- Racks: `Branches/*Branch{DeviceChain, MixerDevice, BranchSelectorRange, ZoneSettings{KeyRange, VelocityRange}}` plus `MacroControls.0–15` (0–127), `MacroDisplayNames`, `MacroVariations`.
- Macro mapping: the mapped parameter carries `KeyMidi{Channel=16, NoteOrController=<macro>}`. **[?]** Whether that is the real encoding, and in which space min/max apply.
- Drum pads: `ReceivingNote` = 128 − note.
- Units are per parameter. Examples: Utility `Gain` is linear; compressor threshold is linear; delay times are in seconds; legacy Auto Filter cutoff is a MIDI note number. **Each translator needs a parameter-unit table**, generated where possible (§7).

**Plugins**
- `PluginDevice/PluginDesc/VstPluginInfo{PlugName, UniqueId, Path, Preset/VstPreset{Type, Buffer(hex), ProgramNumber}}`.
  - `Type` 'FBCh' = an opaque bank chunk → `effSetChunk(isPreset = 0)`; 'FPCh' = a program chunk; 'FxBk' = programs × float parameters, **little-endian**.
- `Vst3PluginInfo{Name, Uid/Fields.0–3 (FUID longs), Preset/Vst3Preset{ProcessorState, ControllerState}}`.
  - Restore (SDK `PresetFile::loadPreset` order), off the audio thread, before `setActive`: `component->setState(P)`, `controller->setComponentState(P)`, then `controller->setState(C)` if C is non-empty.
- Automation: `ParameterList/PluginFloatParameter{ParameterId, ParameterValue{Manual, AutomationTarget}}`, with values normalised 0..1. `ParameterId` is the VST2 index or the VST3 `ParamID`.

**Max for Live**
- `MxDevice{AudioEffect|Instrument|MidiEffect}`: a `FileRef` to the `.amxd`, a `ParameterList` (values plus automation targets), and `BlobSlot/MxDBlob` (saved state, hex).
- **The `.amxd` patcher is readable JSON**, so a device's DSP can be ported faithfully by reading its patch. That is how RMSC was specified.
- **[?]** Where a modulator's *mapping targets* are stored (blob/pattr vs LOM ids). This decides whether modulator wiring can be imported automatically.

**Tuning systems (Live 12)**
- `LiveSet/TuningSystems/TuningSystem{NoteTunings{Cents, TuningRatio}, PseudoOctaveInCents/AsRatio, ReferencePitch{Octave, NoteIndexWithinOctave, FrequencyInHz}, AllowedNoteRange, NoteNames, TuningSystemName}`.
- Notes are still stored as integer `MidiKey`. **[?]** The `MidiKey` → (octave, index) mapping. The working hypothesis is consecutive from the lowest note of `AllowedNoteRange`; verify it before relying on it.

---

## 5. Where it goes in pqdaw

**A new module `src/interop`** (target `pq_interop`, namespace `pq::interop`):
- Sits above `document` and depends on `document`, `tuning` and `core` only. It never includes `engine`, `ui` or `jit`.
- `app` and `tools` call it.
- It must be added to `docs/ARCHITECTURE.md` §1 in the commit that creates it. MIDI-file import (M11) belongs here too.

```
.als ──gunzip (stream)──▶ SAX reader (version-normalising) ──▶ als::LiveSet   (typed, read-only, Live's units)
                                                                    │  translate (pure function, deterministic)
                                                                    ▼
                         doc::Project snapshot  +  ImportReport (per element: tier E/N/A/X, reason, location)
                                                                    │  saveProject (new directory; the .als is never written)
                                                                    ▼
                                    project.pqd  +  samples relinked/collected  +  foreign/<id>.xml blobs
```

**Design rules**
1. **Two stages.**
   - `als::LiveSet` mirrors Live faithfully: Live's names and units, and every Live version normalised to one model.
   - `translate()` is the only place that knows pqdaw.
   - This keeps format archaeology separate from mapping policy, and lets the corpus test check stage 1 alone.
2. **Pure and deterministic.** The same input and importer version give a byte-identical `project.pqd`, so imports can be golden-tested.
3. **Lossless where it can't translate.**
   - An untranslated device becomes a `ForeignDevice` node: Live's tag, its name, its raw XML (stored in `foreign/`), its parameters with their automation lanes (kept as lanes so they still draw), and audio passing through bypassed.
   - It costs nothing at runtime: it lowers to a wire, and the optimiser removes it.
   - **Re-translation:** the project records its importer version, so "Re-run Ableton translation" can upgrade foreign devices in place after a new translator lands. That makes importing everything early and improving later safe.
4. **Provenance.** The project stores the source path, file size, xxh3 hash and Live version. The import report is saved next to it (`import-report.json` + readable `.txt`).
5. **Translate to what is idiomatic in pqdaw.**
   - Constant envelopes become static values: Live writes many envelopes holding only the pre-roll event, and these must not become lanes.
   - Arrangement clip envelopes are unrolled into track automation (§6.3).
   - EDO tunings become `TuningDef` EDOs, not cents tables.
   - This is SPEC §0's "external formats get forced into our shape" and costs nothing in fidelity.
6. **Performance.** Import is offline, but SPEC §0 still applies:
   - Stream-parse with a skip-list for subtrees that are never read.
   - Intern tags.
   - Use one arena for the `LiveSet`.
   - Target: < 1 s for a typical 55 MB set (release build), and memory below 3× the inflated size.
   - The imported project must cost no more CPU than the same project built by hand.

---

## 6. Subsystems: mapping, tier, pqdaw prerequisite

### 6.1 Timeline: tempo, signatures, locators, loop
- Beats become ticks: `round(beat · kPpq)`.
  - PPQ 221,760 holds 1/256 notes and 3-, 5-, 7- and 11-tuplets exactly, so grid-aligned Live data is exact.
  - Off-grid times round to under 1/221,760 of a beat: 2.3 µs at 120 BPM. That is **E**.
- Tempo automation → pqdaw tempo points. If the 1/16 tempo-hold hypothesis (§4) is confirmed:
  - Option A: pqdaw gains a "stepped per 1/16" ramp segment type, used only on import.
  - Option B: the importer emits 1/16 steps as discrete tempo points.
  
  Prefer B: no new engine concept, and it affects 39 sets. Both are **E**.
- Locators, arrangement loop and time-signature changes map directly.
- **Prerequisite:** nothing (M3 has it).

### 6.2 Tracks, groups, returns, master, mixer, routing
- MIDI/audio/group/return/main tracks, nesting, names and colours (Live's 70-colour palette maps to the nearest pqdaw clip colour; record the Live index so it can be exported back).
- Volume and sends: dB = 20·log10(g). Values ≤ 0.000316 become −∞ (pqdaw's −70 dB floor, Q26.4, matches Live).
- **Pan law must equal Live's:** constant power, 0 dB at centre, +3 dB at the extremes: `gL = √2·cos((p+1)π/4)`, `gR = √2·sin((p+1)π/4)`, from Live's audio fact sheet. QUESTIONS has "constant-power pan" as provisional, so fix it to exactly this. Split-stereo mode as well. **E.**
- Activator → mute; solo; pre/post sends; outputs (Main/Group/Track/External/Sends-only).
- Sidechains: Compressor, Gate, Auto Filter, Vocoder, Corpus and plugins.
- **Live has PDC on; pqdaw's PDC (M6) must give the same alignment.** Without it, even E-tier tracks smear.
- **Prerequisites:** Audio tracks (M6), sidechain inputs (M9 devices), PDC (M6), external outputs (T3.18 device manager).

### 6.3 MIDI clips and notes
- Notes are **E**:
  - `MidiKey` + tuning becomes a `PitchId` (§6.4);
  - `Velocity` (float) → velocity; `OffVelocity` → release;
  - `Probability` → chance; `VelocityDeviation` → velocity range;
  - **`IsEnabled=false` → deactivated note** (6 % of notes).
  
  pqdaw's `Note` already has every field (ARCHITECTURE: 64-byte `Note`), apart from a deactivated flag. Check `flags` has room for it.
- Clip loop, start marker, signature and name/colour map onto pqdaw's `ClipLoop` / content start.
- **Per-note expression** (pressure, slide, pitch) → pqdaw per-note expression curves (SPEC §3.1). Bézier segments are subdivided until within tolerance (§6.9).
- **Clip envelopes.** SPEC §1 puts clip envelopes out of v1, so for arrangement clips:
  - *absolute* clip automation is **unrolled into track automation** over the clip's span. Loops are repeated and the clip's start offset applied. This is exact, because arrangement clips have fixed positions.
  - *modulation* envelopes (relative, `ModulationTarget`) become an automation source **summed into the parameter as a modulation wire**. pqdaw's automation is already a signal (SPEC §9.6). Live's modulation-combine law **[?]** (additive in normalised space, scaled by remaining range?) has to be measured.
  - Session clips have nowhere to unroll; see §6.10.
- MIDI CC lanes (pitch bend, CC) go to plugins and Simpler as CC automation. pqdaw has no MIDI CC lanes yet. Plugins need this (§6.7); for native instruments, pitch bend becomes a pitch-offset modulation.
- **Prerequisites:** a deactivated-note flag, expression curves (if they don't exist yet), modulation-wire summation.

### 6.4 Pitch and tuning (the xen part)
Three sources, all turned into pqdaw pitch identities so they can be edited natively:

1. **Live 12 `TuningSystem`** (116 sets):
   - Detect an EDO: equal cents steps within 0.001 ¢, giving `TuningDef::Edo{n, equave}`. A rational equave becomes a `Rational`; a non-rational one such as 833.09 ¢ becomes a cents equave, which needs checking against pqdaw's EDO type.
   - Otherwise a `ScaleDef`: ratios where `TuningRatio` is present (exact JI), cents otherwise.
   - Reference pitch is taken from `ReferencePitch`.
   - Notes become `Degree` identities through the **[?]** `MidiKey` → (octave, index) mapping.
   - Tracks with `IsTuned` follow the tuning. Drum Rack tracks bypass it (Live behaviour).
   - **E** for native instruments.
2. **Microtuner (Max, 87 instances)** — Live 11-era xen sets:
   - Parse its saved scale from the blob/parameters (**[?]** format; Microtuner loads `.scl`, so the text is probably stored).
   - It becomes a track/clip tuning plus Degree identities.
   - The device itself is dropped, because pqdaw tunes natively.
3. **MPE per-note pitch bends and pitch-bend lanes** (Live 11 sets):
   - Notes become `Cents` identities plus a pitch curve.
   - An optional **"snap to tuning"** pass detects the EDO that explains every static pitch within ±1 ¢ and converts the notes to Degree. Report how many notes snapped.
   - Snapping is reversible: the raw cents go in the report.

**Audio caveat: plugins.**
- Live drives MPE plugins (Serum, Pigments…) with **48-semitone 14-bit pitch bend**, a resolution of 0.59 ¢.
- pqdaw will send exact pitch, either through MPE with its own bend range or through CLAP note expressions. The difference is at most 0.3 ¢: inaudible, but it breaks a bit-exact null.
- An "Ableton-compatible MPE" option (same bend range, same quantisation, same channel rotation) gives E-tier nulls for verification. It is cheap, so keep it.
- **Prerequisite:** MPE output to hosted plugins (SPEC §10 already plans MPE output for external synths; reuse it).

### 6.5 Audio clips and warp

| What | pqdaw approach | Tier |
|---|---|---|
| Placement, loop, start, length, gain (`SampleVolume`), Live's 4 ms declick (`Fade`), fades | Direct. **[?]** Fade curve law (skew/slope) needs measuring | E once the curve law is measured |
| Unwarped, no transpose (39.5 %) | Sample-accurate playback. If the file's rate ≠ the project rate, Live resamples, so pqdaw must too | E (N if resampled) |
| Warped 1:1, Beats/Tones/Texture/Re-Pitch, no transpose (12.2 %) | Detect 1:1 → plain playback, no warp engine. This is also a CPU win | **E** |
| Re-Pitch, and unwarped with transpose (8.8 %) | High-quality polyphase/sinc resampling (SPEC §9.4). Live's kernel is unknown (normal vs `HiQ`), so match by measurement | N |
| **Beats, stretched (27.6 %)** | Reimplement Live's Beats algorithm: segment at transients (by `TransientResolution`: transient markers or a fixed grid), play each segment at the original rate from its warped start, apply the transient envelope, and fill gaps per `TransientLoopMode`. **The details are knowable by measurement**: render impulses and tone bursts through Beats in Live at known settings (§7.3) and fit segment boundaries, crossfade lengths and envelope shape. Transient positions: Live uses its own detector, and the `.asd` stores onsets (abletoolz documents the format), so **read Live's onsets from the `.asd` rather than redetecting**. Without that the segment boundaries differ | A → **N** target |
| Tones / Texture, stretched (2.5 %) | Granular (grain size, flux); measure the grain window and overlap. Texture has randomness, so a null is impossible | A |
| Complex / Complex Pro (9.4 %) | signalsmith-stretch (MIT; already in SPEC). Live uses zplane élastique; zplane has no free tier, and SPEC §15 excludes it. **Bake in Live** if it matters (§9) | A |
| WarpMode 5 / `.rx2` (0.1 %) | REX decoding needs the REX SDK (licence unclear), or bake in Live | X |
| Missing samples (33 %) | Relink search: file name + `OriginalFileSize` across configured roots (E:\MUSIC SHIT, F:\…, the User Library, packs), plus `RelativePath` against moved project folders. Batch, cached index, and a report of what is still missing | – |
| Pack samples in the `able` codec | Cannot be decoded; list them, and have Live's "Collect All and Save" convert them during the Live pre-flight (§9) | X → E after pre-flight |

**Prerequisites:** M6 (audio clips, warp, the `clipPlayer` atom), an `.asd` onset reader, and a resampler.

### 6.6 Native devices
Each Live device needs a **translator**: a parameter map from Live's units to the pqdaw device's parameters, with automation re-targeted, plus a **parity measurement** (§7).

Priority is by corpus use. In the table, ✓ means the device is in the pqdaw v1 list (SPEC §1/§8), and **new** means it is not.

| Priority | Live device | pqdaw | Achievable tier | Notes |
|---|---|---|---|---|
| 1 | EQ Eight | ✓ (M4) | N | Biquad/SVF maths is public; match Live's band curves, adaptive Q and oversampling exactly by measurement |
| 1 | Utility | ✓ | E | Gain, width, mono, bass mono, phase, balance (the balance law is the pan law?) |
| 1 | Compressor | ✓ (M9) | N | Peak/RMS/expand, knee, lookahead, sidechain + EQ; fit the detector and gain computer by measurement |
| 1 | Auto Filter (legacy + Live 12 `AutoFilter2`) | ✓ | A→N | Circuit models (OSR/MS2/SMP/PRD) are nonlinear |
| 1 | **Multiband Dynamics** | **new** | N | Used in 87 % of sets (mostly as OTT): 3-band crossover + up/down compression; a well-defined design |
| 1 | Saturator | ✓ | N | Curves are documented; fit them |
| 1 | **Echo** | **new** | A→N | Modulation, reverb, ducking, character |
| 1 | Delay + legacy Simple/Ping Pong Delay | ✓ | N | Legacy delays map onto Delay modes, or onto small dedicated patches |
| 1 | Auto Pan | ✓ | N | |
| 1 | Simpler | ✓ (M5) | N | Classic/one-shot/slice, warp (uses §6.5), filter, envelopes |
| 1 | Reverb | ✓ | A | An FDN cannot null against Live's; match RT60, density and spectrum |
| 1 | Glue Compressor | ✓ | N | |
| 1 | Operator | ✓ (M4) | N | FM maths is exact; match the envelope curves and filter |
| 1 | Racks (Audio/Instrument/Drum) | ✓ (M4/M5) | E (structure) | Chains, zones, chain selector, macros (§6.8) |
| 2 | **Arpeggiator, Velocity, Pitch, Scale** (MIDI effects) | **new** | E | Deterministic MIDI transforms. The Arpeggiator needs Live's exact timing and modes. Pitch/Scale could also be folded into notes at import (bake), which is exact |
| 2 | **Vocoder** | **new** | A→N | 566 sets |
| 2 | **Erosion, Redux / Redux2, Overdrive, Drum Buss, Frequency Shifter, Grain Delay, Shifter, Phaser (legacy), Roar, Amp/Cabinet, Corpus, Spectral Resonator/Time, Wavetable, Collision, Sampler** | **new** | N–A | In usage order. Each is a self-contained device task in the circuit language. Wavetable needs Live's factory wavetables, which can be read from the Live install for personal use but never committed |
| 2 | Limiter, Gate, Resonator, EQ Three, Chorus-Ensemble, Spectrum, Tuner | ✓ | N | |
| 3 | Everything else in the 79 found | new | – | Until translated: `ForeignDevice` bypass, flagged X |

**SPEC impact:** the v1 device list covers roughly the top half by usage. Fidelity for your projects needs about **15 more devices**, with Multiband Dynamics, Echo and the MIDI effects most urgent. Sampler is "later" in SPEC but is in 85 sets.

### 6.7 Third-party plugins (the largest fidelity item)
- **Hosting.** SPEC §1 defers VST3/CLAP to "later, as sealed devices". Your corpus needs **VST3 and VST2**.
  - **VST3:** the SDK has been **MIT-licensed since 29 October 2025** (VST 3.8; Steinberg press release), so it fits pqdaw's licence policy.
  - **VST2:** Steinberg's SDK is withdrawn and its licence is separate. The clean-room header **Xaymar `vst2sdk`** (BSD-3, used by SUBstation and OBS) lets a host load VST2 plugins without Steinberg's files. Whether that is acceptable is a user decision: pqdaw is personal, non-commercial and a public repo (§11).
  - 59 % of plugin instances are VST2. Serum alone has 2,592 VST2 instances.
- **Out-of-process scanning** with a cache: plugin id → path, formats, parameter list.
- **Identity matching:**
  - VST2 by `UniqueId` (+ name for shell plugins: `IsShellClient`, e.g. Waves);
  - VST3 by the class FUID built from `Uid/Fields.0–3`;
  - the fallback is the same product in the other format, by name.
  
  Cross-format state is generally **not** compatible. The exception is JUCE-built plugins (Serum isn't JUCE; many others are): the JUCE VST3 wrapper accepts VST2-style chunks. This is an experiment, not a promise: label it A until it is measured per plugin.
- **State restore:** exactly the sequences in §4. Put this behind a per-plugin quirks table: some plugins need `setActive` before `setState`, and some need the program set after the chunk.
- **Automation:**
  - VST2: `ParameterId` = parameter index.
  - VST3: `ParameterId` = `ParamID`, validated against the plugin's actual parameter list.
  - Where Bitwig fails: check every imported lane's id exists in the plugin and the name matches `ParameterName`. Report mismatches, and never silently retarget.
- **Timing fidelity:**
  - The plugin sees the same audio and the same MIDI/MPE (§6.4).
  - Automation reaches the plugin at the same sample offsets. **[?]** Live's plugin automation granularity (per buffer or sample-accurate VST3 queues) has to be measured.
  - The same block size matters only for plugins that are not block-size invariant.
  - PDC must match.
  - With all of that, deterministic plugins null at E. Plugins with internal randomness (Synplant, analog-noise models) cannot null; verify those by spectrum/loudness only.
- **Missing plugins:** `ForeignDevice` keeps the state blob and automation, so installing the plugin later and re-translating restores it.
- Meter plugins such as Youlean, MiniMeters and PQ Meter Server affect no audio. Import them, and offer "drop analysis-only plugins".
- **Prerequisites:** a new **plugin hosting milestone** (in-process host on a worker thread, state, parameters, automation, sidechain buses, MPE/MIDI in, PDC, crash isolation later). It is the single biggest item in this plan.

### 6.8 Racks and macros
- **Racks become pqdaw racks** (SPEC §8.2): chains, chain mixer, key/velocity/chain-select zones with fades, Drum Rack pads (`ReceivingNote` = 128 − n, choke groups, pad sends/returns), macro values, names and variations.
- **Macro mappings:**
  - Each mapped parameter (via its `KeyMidi` macro reference **[?]**) becomes a macro target with min/max.
  - **The crucial unknown:** Live maps a macro onto the parameter's *control* (normalised, display-curve) space, not its value space. A frequency knob mapped 0–127 sweeps logarithmically.
  - So every translator must export Live's normalisation curve per parameter. These are the same tables as automation (§6.9) and the same measurement session.
- **Instrument Rack pitch zones:** Live's zones are MIDI keys; pqdaw's are Hz or tuning ranges (SPEC §8.2). Convert through the track's tuning at import: exact for 12-EDO, and defined via the `MidiKey` mapping for tuned sets.

### 6.9 Automation curves and parameter spaces
- Event lists become pqdaw breakpoints (ticks, value, curve). Equal-time event pairs give steps.
- **Bézier `CurveControl`** segments don't fit pqdaw's exponential curve family (Q39). Subdivide each one into exponential/linear pieces until it is within ε of the parameter's range. pqdaw curves are closed under cutting, so subdivision is exact at the joins. **N.**
- **Which space Live interpolates in matters audibly:**
  - The volume envelope stores linear gain. Does a ramp from 1.0 to 0.0003 interpolate linearly in gain, in dB, or in fader position? **[?]**
  - pqdaw stores volume in dB, so pick the breakpoints to reproduce Live's curve (subdivide if needed).
  - The same question applies to every native parameter with a non-linear display (frequency, time).
  - This is the most audible systematic error an importer can make on fades and filter sweeps. Measure it once per parameter class (§7.3).
- Plugin parameters are normalised 0..1, so interpolation happens in plugin-normalised space; this is trivially exact.

### 6.10 Session clips, take lanes, freeze, grooves, the rest
- **Session clips** (103 sets; 1.4 k clips): pqdaw has no Session View.
  - Import them into an "Imported session clips" region past the arrangement end (one lane per track, scene rows laid end to end, named after the scene), so nothing is lost.
  - Clip envelopes in session clips are unrolled there in the same way.
  - Mark the region X for playback semantics.
- **Take lanes** (500 sets) become pqdaw take lanes (SPEC §9.5: v1).
- **Frozen tracks** (395):
  - Import live: devices and clips.
  - Offer "use Live's freeze audio" (`Samples/Processed/Freeze/*`) as an E-tier fallback for that track, keeping the devices bypassed underneath for later.
- **Grooves** (16 sets): pqdaw's groove pool is v1 (M10), and `.agr` is simple XML. Alternative: commit the groove into note timing at import, which is exact (Live applies grooves deterministically, except for its Random amount).
- **Live 9 sets** (77): decode the hex `FileRef/Data` (a UTF-16 path) and inline `ArrangerAutomation`.
- **Things to drop on purpose:** view state, browser state, MIDI/key mappings (SPEC §9.7 has its own; import them later), Push/control-surface data.

---

## 7. Proving fidelity: the verification harness

The user wants minimum audio change, so every claim of E or N has to be checked against a Live render.

### 7.1 Corpus tests (local only, never committed)
- Your projects are private and contain third-party content: **never commit them**.
- Add a ctest label `als_corpus`, outside `-Full` and opt-in like `perf`. It reads a root from `PQ_ALS_CORPUS`.
- Stage-1 check: every set parses, the model invariants hold, and the parse time and memory are recorded.
- Stage-2 check: translation succeeds and the report's tier totals are written to a CSV.
- Regressions show up as a tier total getting worse.

### 7.2 Committed fixtures
- Small sets you make in Live specifically for tests, using stock content only and short sine/noise samples generated by a script. They go in `tests/interop/fixtures/` with the generating script in `tools/fixtures/`.
- One fixture set per feature, saved in Live 12. Where version drift matters (paths, master naming, automation layout), the same set re-saved by an older Live, if you still have one installed. Otherwise use your existing Live 10/11 project versions (structure tests only).

### 7.3 Parity renders: measuring Live
- **pqdaw can write `.als` sets** (a small writer in `tools/als/`, test-only) to **generate measurement sets**. Examples:
  - "EQ Eight band 3 bell, 30 tracks at different gains and frequencies, each fed a log sweep";
  - "a volume ramp from 0 dB to −∞ over 4 beats on a DC/sine track";
  - "Beats-mode clip of a click track at 0.8× / 1.25× with each `TransientLoopMode`".
- You open a set, choose **File → Export Audio → All Individual Tracks** once, and drop the stems in a folder. A comparison tool fits curves and null-tests.
- One export per batch of measurements: a few minutes of your time per device.
- Live has no headless render. Recording through Live's API (AbletonOSC) would be realtime and fragile, so don't.
- **This resolves every [?] in §4 and §6.** Q-list for the first measurement batch:
  1. `MidiKey` ↔ tuning degree;
  2. automation interpolation space for volume, frequency and time parameters;
  3. the Bézier curve formula;
  4. fade curve law;
  5. tempo ramp 1/16 hold;
  6. the macro mapping encoding and space;
  7. the modulation-envelope combine law;
  8. Beats segmentation and envelope;
  9. MPE pitch scale;
  10. Live's resampler (normal and HiQ);
  11. Live's plugin automation timing.
- SPEC §15 (2026-10-24) says "no Live renders for now". This plan needs them, but only as batches of generated sets. Ask the user (§11).

### 7.4 Project-level comparison
- For a set you care about: in Live, export the mix + "All Individual Tracks"; in pqdaw, import and render the same range offline.
- `pqc als-compare` aligns the renders (they should already be sample-aligned; check by cross-correlation) and reports per track:
  - null residual (dBFS);
  - LUFS difference;
  - per-band spectral difference (`golden_audio` metric);
  - the tier claimed by the import report.
- **A claimed E that fails its null is a bug.** That is the acceptance test for the whole importer.

---

## 8. User-facing import

- **File → Import Ableton Live Set…** (and dropping an `.als` onto the window). It creates a new untitled project and never modifies the `.als`.
- **The import report panel:**
  - per track, a tier badge (E/N/A/X);
  - a list of issues, each a link to the clip or device;
  - missing samples with "Search folders…";
  - missing plugins;
  - foreign devices;
  - snapped tunings (how many notes snapped, and the residuals);
  - session-clip region notice.
- **"Re-run translation"** on an imported project: upgrades foreign devices and plugins found since (provenance, §5).
- **CLI:**
  - `pqdaw --import-als <set> --out <dir>`;
  - `pqc als-info <set|dir>` (stage 1 only: structure, devices, plugins, tiers);
  - `pqc als-report <root>` (the whole corpus → CSV and an HTML summary);
  - `pqc als-compare <pqdaw render> <live stems dir>`.

---

## 9. Migration workflow for your ~1,000 projects

1. **Triage with `pqc als-report`** over E:\MUSIC SHIT. Each set gets:
   - a fidelity score: the share of the signal path at E/N, weighted by track loudness if renders exist, otherwise by count;
   - missing samples and missing plugins;
   - the list of A/X items.
2. **Decide per project:**
   - **Active** (you'll keep working on it): import natively. Accept A items, or fix them by hand: re-warp a Complex clip, swap a device.
   - **Archive** (you only want it playable and remixable): run the **Live pre-flight** first, then import. Its result is E-tier everywhere it touched.
3. **The Live pre-flight**, done in Live, at most once per project:
   - **Collect All and Save**: fixes missing pack and `able`-codec samples where Live can still find them, and puts everything in the project.
   - **Freeze/flatten** tracks whose chains hold X/A items: Max DSP devices with no port, Complex warps you care about, unported devices.
   - Optional: consolidate (Ctrl+J) Complex/Beats clips you want frozen in time.
   - Then save as a new file, `… (pqdaw).als`, so the original stays untouched.
4. **Keep Live installed** until the report for every active project is all E/N, or you have accepted its A items.

Expected result for a typical recent set once phases B–D exist (§10): structure, MIDI, tunings, mixer, plugins and half the audio at E. Stock devices mostly at N. Stretched Beats at N/A, Complex at A. Max modulators rewired natively at N.

---

## 10. Phased plan (task-sized, in dependency order)

Each phase builds on pqdaw milestones. Acceptance criteria follow the repo's definition of done: dev + release `-Full` green, zero warnings, benchmarks where relevant. In the table, **🔓** means the task can start now and needs no unbuilt milestone.

| Id | Task | Depends on | Acceptance (core) |
|---|---|---|---|
| **AL1** 🔓 | `pq_interop` module; streaming gzip + SAX reader; typed `als::LiveSet` for Live 9–12 (timeline, tracks, routing, mixer, clips, notes, per-note expression, envelopes, devices as tagged parameter bags, plugins, Max devices, tuning systems, samples); `pqc als-info`; corpus tools promoted to `tools/als/`; `als_corpus` ctest label | – | All 1,236 corpus sets parse. Fixture sets for each Live version round-trip into the model with expected values. A 55 MB set in < 1 s (release) and < 3× its size in memory; benchmark in RESULTS. Zero GPL code |
| **AL2** 🔓 | Translate: timeline, tracks/groups/returns/main, mixer (Live pan law), MIDI clips and notes (deactivated flag), track automation (constant envelopes → static), Bézier subdivision, clip-envelope unrolling, locators; `ForeignDevice` + raw-XML preservation; provenance; import report (tiers); `--import-als`; File → Import | AL1, M3 | Deterministic output (golden `project.pqd` for fixtures). MIDI-only fixture renders through a stock synth identically to a hand-built project. Every corpus set imports without error |
| **AL3** 🔓 | Tunings: Live 12 TuningSystem → EDO/Scale/JI; MidiKey→degree; Microtuner blob → tuning; MPE per-note pitch → Cents + snap-to-EDO | AL2, first measurement batch (§7.3 Q1) | All 116 tuning-system sets map, with EDO detection reported. Snapped xen sets play at the same Hz as Live (Tuner/NSDF check ≤ 0.1 ¢ on fixtures) |
| **AL4** | Measurement toolkit: test `.als` writer, measurement-set generators, `pqc als-compare` (alignment, null, LUFS, bands, curve fitting) | AL1 | Generates the §7.3 batch-1 sets. A stems folder from Live goes in and fitted curves and an answer per [?] come out, recorded in QUESTIONS |
| **AL5** | Audio clips: placement, loops (seconds vs beats), gain, declick, fades (measured law), 1:1 detection → plain playback, Re-Pitch/unwarped transpose resampling, sample relinking (index + search), reversed files, Live 9 FileRefs | AL2, M6 | Every E-class clip in the fixtures nulls ≤ −90 dBFS against Live stems. Relinking finds ≥ 95 % of the samples that exist somewhere under the configured roots |
| **AL6** | Beats-mode parity: `.asd` onset reader, Beats segmentation/envelope/loop modes fitted to measurements | AL5, AL4 | Fixture Beats clips at 0.5–2× within the N criteria. Corpus report reclassifies Beats from A to N where it passes |
| **AL7** | **Plugin hosting** (a new milestone: VST3 first, then VST2 if approved): scan, instantiate, state restore, parameters, automation, MIDI/MPE in (incl. Ableton-compatible MPE), sidechain buses, PDC | M6 PDC; user decision §11.1 | The top 20 corpus plugins restore state; a fixture with Serum + Valhalla + soothe2 nulls ≤ −90 dBFS against Live (deterministic plugins). Automation ids validated, mismatches reported |
| **AL8** | Native device translators, in §6.6 priority order, one task per device (or a few small ones together): parameter map + unit/normalisation table + parity measurement | The pqdaw device (M4/M5/M9 or new), AL4 | The device's fixture measurements meet its tier from §6.6. Automation and macro targets retargeted |
| **AL9** | Racks: chains, zones, chain selector, Drum Rack pads/choke, macros + mappings (measured space), variations | AL8 (the first few), M4 T4.8, M5 | Rack fixtures null/meet tier. Every corpus rack imports structurally (no X racks) |
| **AL10** | Max for Live: modulators (LFO, The Spellbook, MultiMap, Envelope Follower, Expression Control, Shaper, MultiMapper, XYZmap) → native modulator devices + wires; Borrasca → RMSC; Formant; MPE Control → expression mapping. Port by reading each `.amxd` patch | M4 T4.7 (wires), measurement of where the mapping targets are stored (§4 [?]) | Modulator fixtures: the modulated parameter's trajectory matches Live's within N (measured through a DC-offset trick: modulate a Utility gain on a DC signal) |
| **AL11** | Session clips region, take lanes, frozen-track freeze-audio fallback, grooves (commit or pool), MIDI effects (Arp/Velocity/Pitch/Scale) | AL2, M6, M10 | Fixtures per feature |
| **AL12** | Migration tooling: `pqc als-report` corpus triage (CSV + HTML), Live pre-flight checklist per project, "Re-run translation" | AL2+ | A report over the whole corpus in < 10 min on 3 cores; per-set scores explained |

**Order:**
- AL1 → AL2 → AL3 → AL4 can start now. They make the xen MIDI projects importable today: notes, tunings, mixer and automation. Instruments are silent foreign devices until their translators or plugin hosting exist.
- AL5/AL6 follow M6, and AL7 follows its own milestone.
- AL8–AL10 run alongside M4/M5/M9, which they share devices with.

Every new pqdaw device task should include its Ableton translator and parity measurement as an acceptance criterion. That costs little and avoids a second pass.

**CPU-efficiency notes:**
- Import-time translation (unrolled envelopes, constant envelopes → static values, 1:1 warp → plain playback, committed grooves, baked MIDI effects) removes runtime work instead of adding it.
- Foreign devices compile to nothing.
- The reader streams and skips what it doesn't read.
- Imported plugins are the only cost pqdaw cannot control.

---

## 11. Decisions for the user (to go to `docs/QUESTIONS.md`)

1. **Plugin hosting in v1?** SPEC §1 defers it, but 96 % of your sets need it, and Serum is in 70 %. Provisional recommendation: make plugin hosting (VST3, then VST2) its own milestone after M6, before Ableton import is called complete.
2. **VST2 hosting via the clean-room `vst2sdk` header** (BSD-3)? This is a legal grey area for a public repo: Steinberg considers VST2 licensed-only. The alternative is VST3/CLAP only, which loses or approximates 59 % of plugin instances (cross-format state). Provisional: yes, behind a CMake option, default on for personal builds.
3. **Live measurement renders.** This reverses "no Live renders for now" (SPEC §15, 2026-10-24) for importer measurements only: generated sets, one "Export All Individual Tracks" per batch, about 10 batches over the project. Provisional: yes.
4. **New devices beyond the v1 list** (Multiband Dynamics, Echo, Vocoder, MIDI effects, Sampler…). Add them to the roadmap in corpus-usage order? Provisional: yes, Multiband Dynamics, Echo and MIDI effects first.
5. **Session clips:** dump them into a region past the arrangement end (provisional), or wait for a Session View?
6. **xen MPE projects:** snap per-note pitch to a detected EDO by default (provisional: yes, with a report and a toggle), or keep raw cents?
7. **Complex/Complex Pro:** accept signalsmith-stretch's sound (A), or treat the Live pre-flight bake as the default for those clips in archive projects? Provisional: A for active projects, bake for archives.
8. **Corpus location for local tests:** `PQ_ALS_CORPUS=E:\MUSIC SHIT`. Fine to read in place, read-only?

---

## 12. Sources

**Bitwig**
- https://www.bitwig.com/support/technical_support/40/
- https://downloads.bitwig.com/4.0/Release-Notes-4.0.html (and 4.0.5, 4.1.6, 4.2.5, 4.3.1, 5.0.11, 5.2)
- https://www.bitwig.com/stories/transferring-projects-between-bitwig-studio-and-other-daws-333/
- https://kvraudio.com/forum/viewtopic.php?p=8870406
- https://www.elektronauts.com/t/the-bitwig-thread/50027?page=20

**DAWproject**
- https://github.com/bitwig/dawproject (MIT)
- Ableton has no DAWproject support: https://www.gearnews.com/ableton-live-12-review/

**Format references**
- SUBstation (MIT, C++): https://github.com/OpenMusicStuff/SUBstation, `docs/app/live-import.md`
- dawtool (BSD-3): https://github.com/offlinemark/dawtool
- AbleSharp schemas (MIT): https://github.com/theokyr/AbleSharp
- ableton-als (MIT): https://github.com/kevinkirsten/ableton-als
- alive FORMAT.md: https://github.com/rueblose/alive
- Read only, GPL-3: DawVert https://github.com/DawVert/DawVert, abletoolz (`doc/VERSION_DIFFS.md`, `asd/FORMAT.md`) https://github.com/elixirbeats/abletoolz
- Paketti notes: https://github.com/esaruoho/paketti/blob/master/ableton-live-format.txt

**Ableton**
- Audio fact sheet (pan law, transparency, declick): https://www.ableton.com/en/live-manual/12/audio-fact-sheet/
- Tuning systems: https://www.ableton.com/en/live-manual/12/using-tuning-systems/ and https://tuning.ableton.com/
- zplane in Live: https://products.zplane.de/blog/gerhard-behles-interview-ableton
- zplane licensing: https://licensing.zplane.de/licensing

**Plugins**
- VST 3.8 MIT: https://sonicstate.com/news/2025/10/30/vst-3-now-available-under-mit-license, https://rekkerd.org/vst-3-now-available-under-mit-license
- VST2 licence remains separate: https://forums.steinberg.net/t/vst-3-sdk-license/201637
- VST3 preset restore order: https://github.com/steinbergmedia/vst3_public_sdk/blob/master/source/vst/vstpresetfile.cpp

**Time-stretching**
- signalsmith-stretch (MIT): https://github.com/Signalsmith-Audio/signalsmith-stretch
- Bungee (MPL-2.0): https://github.com/bungee-audio-stretch/bungee
- Rubber Band (GPL, excluded): https://github.com/breakfastquay/rubberband

**Corpus measurements (2026-10-25):** scripts and JSON in the session scratchpad, `als_inventory/` and `warp_ratio/`. AL1 promotes them to `tools/als/`.
