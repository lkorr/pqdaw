# RT: record from a track (Ableton's "Audio From"), set up in the arrangement or by a cable in the circuit

Status: approved by the user 2026-11-27 with the decisions below. RT.1 runs first.

## Decisions (user, 2026-11-27)
- Design approved: Audio In is one wire end on the track; the choosers are a view of it; schema 23.
- One source per Audio In (a new cable or choice replaces the old one, one undo step). Loops are refused.
- **No tap ports.** Mixers aren't special: "it's just an audio output being dragged onto a recording arm input on an
  audio track". Pre FX / Post FX are not ports of the mixer node: to record before or after the effects the user drags
  from that point (the track's source / instrument output, or the last device's output). The mixer node keeps its one
  out pin, and a cable from it is what Live calls Post Mixer. This supersedes the "Taps" section below wherever they
  differ: no `prefx` / `postfx` ports or exports, no symbolic "Post FX follows the chain" (a cable from a device stays
  on that device), `AudioFrom` has no `Tap` kind.
- Monitor defaults unchanged. Ext. In stays a chip on the terminal (no Audio Interface node).

## The ask (user, 2026-11-27)
- "we need a recording from -- feature like in ableton, so i can record on an audio track directly from a particular
  lane … look up exactly how ableton does it, and implement it with that kind of ui"
- "while the way to set it up in arrangement view will be comparable to ableton so there is a quick audio in + record
  thing, the way to set it up in circuit view which will be isomorphic to the arrangement view thing will be to draw an
  output cable from the output mixer module (or from any audio output node frankly) and that should be drawn onto an
  audio track lane onto an audio input thing that writes onto the audio track/lane."

## How Ableton does it (Live 12 manual: "Routing and I/O" §17, "Recording New Clips" §19)
- Each track's In/Out section (View → In/Out, Ctrl+Alt+I) has two stacked choosers per direction. For input:
  **Audio From** (the Input Type chooser) and, under it, the **Input Channel** chooser.
- Input Type entries: `Ext. In`, `Resampling`, `No Input`, then every track that delivers audio, by name. Return tracks
  have no input chooser; their input is their sends.
- Input Channel entries:
  - For `Ext. In`: the interface's channels (`1`, `2`, `1/2`, …), with a meter on each.
  - For a track: `Pre FX` (before its devices; device and mixer edits don't affect it), `Post FX` (after its devices,
    before its mixer) and `Post Mixer` (its final output after volume and pan; soloing the recording track doesn't
    make the tapped track audible).
  - For a track with racks: one entry per chain, `<Rack> | <Chain> | Pre FX / Post FX / Post Mixer` (Drum Racks list
    their return chains too).
  - Multi-output instruments list their extra outputs.
  - A MIDI track's signal is tapped after its MIDI effects, just before the instrument; its audio taps are the
    instrument's audio.
- Monitor: `In` (always the input, the clips are muted), `Auto` (the default for MIDI tracks: on while armed, held off
  while the track plays clips), `Off` (the default for audio tracks). "Keep Monitoring Latency in Recorded Audio" is a
  right-click option on In / Auto.
- `Resampling` takes the Main output. The recording track's own output is excluded, so it never records itself.
- The manual doesn't say how Live handles a track picking itself or a loop. In practice Live leaves the recording track
  out of its own list; we decide the loop rule below.

## pqdaw: one model, two views of it

**The single truth is a cable.** The "record from" setting of audio track B is one wire into B's **Audio In**, a new
input terminal on B's lane. The arrangement / mixer choosers are a view and an editor of that same wire, and the
circuit view draws it. There is no separate routing field the two could disagree on (memory "general mechanisms":
behaviour follows from the wires). `Ext. In` is the one setting that isn't a cable: it stays the device channels
(today's `Mixer::audioIn`), shown as a chip on the Audio In terminal.

### The Audio In terminal (document)
- Every audio track has one. It is B's record and monitor input: what B hears under Monitor In / Auto, and what an
  armed B writes to a take. It replaces today's lowering read of `track.in` (lower.cpp ~3409, the monitor mix) with
  "whatever feeds the Audio In".
- It is fed by exactly one of:
  - nothing (`No Input`);
  - `Ext. In <channels>` (device channels; `Mixer::audioIn` as now);
  - one cable (a `Wire` with a new end kind: `Wire::recordInput` TrackId, like `Wire::mixer`; `to` invalid,
    `param` kMainPort).
- One source, as in Live. Drawing a second cable onto it, or picking Ext. In, replaces the old source in one undo
  step.
- What a cable can start at: any audio output in the project, on any track (TX.all-free ends):
  - a track mixer node's out (Live's Post Mixer);
  - a track's source (clip source / instrument output; Live's Pre FX);
  - any device's audio output (a module or group `out`, an engine's audio export);
  - a rack chain's output or a chain mixer;
  - the master mixer's out (Resampling).
  It must not start at a note output or a control-rate export (refused: "Audio In takes audio").

### Sources: any audio output, no special tap ports (user decision)
The mixer node keeps its one out pin. Live's three taps are reached by where the cable starts:
- Pre FX: the track's source (an audio track's clip source, a MIDI track's instrument output).
- Post FX: the last device's output. This is a cable from that device, so it stays on it; adding a device after it
  doesn't move the cable.
- Post Mixer: the mixer node's out (the existing `post` export: after volume, pan, mute; ignores solo).

### The arrangement / mixer UI (Live's layout)
- The audio track's I/O section in the mixer strip, and in the arrangement track header when it's tall enough with
  In/Out shown, shows **Audio From** over the channel chooser, then Monitor In / Auto / Off. The input meter sits on
  the chooser.
- Audio From lists `Ext. In`, `Resampling`, `No Input`, a separator, then every other track by name (MIDI, audio, group
  and return tracks), with the track's colour chip.
- The channel chooser lists:
  - for `Ext. In`: `1`, `2`, …, `1/2`, `3/4`, … ("(not available)" as now);
  - for a track: every audio output on it in signal order: its source (clips / instrument; Live's Pre FX), each device
    in its chain, each rack chain as `<Rack> | <Chain>`, and `Mixer` last (Live's Post Mixer);
  - for `Resampling`: nothing (hidden).
- Choosing an entry writes the cable from that output to B's Audio In. Choosing `No Input` removes the cable.
- **Isomorphism.** Every chooser entry is a cable start, and every cable drawn in the circuit reads back as
  `<Track> | <block name>`. There is never a state one view can't show.
- The default when a track is first picked is its last device (Live's Post FX), or its source when it has none.
- Quick record: arm and record as now. Monitor's defaults are unchanged (pqdaw's current defaults).

### The circuit view (project patch) UI
- The audio track's lane shows an **Audio In** terminal: an input pin at the lane's left edge, labelled "Audio In",
  with a chip showing its source (`Ext. In 1/2`, `No Input`, or nothing when a cable is drawn into it) and the input
  meter.
- Drawing a cable from any audio output (above) onto it sets the source, using the same gesture and refusal statuses as
  any wire. The cable is drawn as an audio cable to the Audio In. Deleting the cable means `No Input`.
- Right-click on the terminal: the same Audio From / channel menu as the arrangement (one menu builder shared by both
  views).
- Tests go through real window and overlay events (memory "UI tests via real dispatch").

### Loops
- The recording track B itself is never offered and is refused as a source of its own Audio In.
- A source that B feeds (through outputs, sends, cables or B's group) is refused with "Audio From would create a loop"
  (checks.cpp, beside the output-cycle check at ~695) and left out of the menu. Live gives no rule here.
  (User decision: refuse.)
- Resampling: the master minus B's own contribution. While B is armed and monitoring, or recording with Resampling,
  its post-mixer contribution to the master's tap is left out, as in Live. Implementation: the master tap that B reads
  is the master sum built without B's bus (a second sum only when a track resamples; no cost otherwise).

### Lowering and engine (RT rules)
- An Audio In cable is a cross-track read like the DY.2 sidechain: export `m<wire>` from A's unit, import into B's
  (lower.cpp crossSidechain / export–import ~2786, `WireKind::Audio`). Sidechain reads already get latency
  compensation (latency_plan.h `compensated`), so what B records is aligned with A on the timeline.
- A cable from a mixer out uses A's existing `post` export (Post Mixer ignores solo, as in Live).
- Units: sidechain wires merge tracks into one unit (graph/project.cpp:280). Today `unitInputs()` keeps one device
  input per unit (project_link_record.cpp:188). RT.2 must lift that limit so two merged tracks can each have their own
  Ext. In.
- Monitoring: the Audio In signal enters B's monitor mix where `track.in` does now (clips × (1 − monitor) + input ×
  monitor), with the same Auto / In / Off rules.
- Recording: the recorder gains internal capture sources beside device channels. The CaptureSet lists (unit, buffer)
  pairs for internal sources; per block the engine copies B's Audio In signal into the recorder ring (RT-safe,
  preallocated, no allocation, `noexcept`, RtScope test).
- The capture offset for an internal source is only the graph's own latency compensation: no device input or output
  latency and no manual offset, because the signal is already on the song timeline. Everything after capture (the WAV
  writer, passes, take lanes, comping, the live waveform) is the existing T6.7 path.
- CPU: no new node unless a track's Audio In has a cable. Resampling adds a second master sum only while some track
  resamples.

### Model and serialisation
- `Wire::recordInput` (TrackId). Commands: the existing connect / disconnect, plus a menu helper
  `ops::setAudioFrom(p, ids, track, AudioFrom)` that replaces the source in one Batch. `AudioFrom` is {None,
  Ext(channels), Output(the wire start: a device / chain / track-mixer / master audio output), Resampling}.
- `doc::audioFromOf(p, track)` reads back the same value from the wire, for the choosers.
- Schema 22 → 23. No migration code beyond the version bump (CLAUDE.md rule 6: no user projects). ARCHITECTURE
  "Modulation wires" / "Recording" sections are updated in the same commit.

## Tasks (each a worker; row of the verification table in brackets)
- **RT.1 document**: `Wire::recordInput`, `setAudioFrom` / `audioFromOf`, the loop and kind checks, serialisation,
  schema 23. Tests: round-trips for every AudioFrom kind; the replace semantics; loop and kind refusals; a cable from a
  device's audio output, a track's mixer out, a rack chain and the master each read back correctly. [-Affected]
- **RT.2 lowering + engine + recording link**: the Audio In signal into B's monitor mix, PDC, Resampling without self,
  internal capture sources, the one-input-per-unit limit lifted.
  - Tests (render, app): B recording A's last device equals that device's render (≤ 1e-6, aligned to the sample); B
    recording A's mixer out follows A's volume; B recording A's source ignores A's devices; a rack chain source;
    Resampling excludes B; monitor Auto / In / Off.
  - RtScope on the capture path.
  - [-Affected -Slow; link pq_bench_dsp ship once]
- **RT.3 UI (👁)**: the two choosers in the mixer strip and arrangement header, the circuit's Audio In terminal, one
  shared menu builder.
  - Tests through real dispatch: pick `A | <device>` in the chooser and the cable appears in the circuit; draw a cable
    from A's mixer out to B's Audio In and the chooser reads `A | Mixer`; draw from a device and it reads
    `A | <device>`; Ext. In chip.
  - Goldens: tracks-mixer, mixer-view, a new project-patch-audio-in.
  - A REVIEW.md entry.
  - [module test exes, default tier]

## Out of scope (ask before adding)
- MIDI From another MIDI track (Live's MIDI tapping).
- Multi-output instruments beyond rack chains.
- "Keep Monitoring Latency in Recorded Audio".
- Several summed sources into one Audio In.

## Review questions
Answered by the user 2026-11-27; see Decisions at the top.
