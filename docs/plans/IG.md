# IG: Instrument groups (plan; not started)

User decision 2026-11-04 (SPEC §15 "instrument groups"). Planned while low on tokens; implement later, in the order below.
Supersedes the planned TX.voice-context and TX.voice-loop (both folded in here).

## The rule

A group whose input is fed notes (a MIDI track's notes, from this track or another) is an **instrument group**:
everything inside it runs **per note** (one copy per sounding voice); its **Out** is where the voices are summed into one
signal. Everything outside any instrument group is an ordinary, single-signal audio / control path. This replaces the
wire-derived voice inference (TMM.10 "voice runs", the stop at the first stock device, TX.cross-track's guest
instances) with one visible structural rule: what is per note is what sits inside an instrument group.

- **Default:** a new MIDI track starts with one instrument group, its In fed by the track's notes, holding a meta
  **Envelope** at the end (before Out). The Envelope can be deleted. When it is the last stage it decides each voice's end
  (meta::Type::voiceEnd); without it voices end 5 ms after note-off (kMetaReleaseSeconds, as today).
- **Several per track:** any group fed notes is an instrument (layering: each plays the notes); a group fed notes from
  another track plays that track's notes (its voices, polyphony and voice stealing are that source's).
- **Stock instruments inside** (Basic Synth, Operator, Instrument Rack): one voice per note (their own voice engine
  collapses; the group does the voicing, stealing and polyphony). A stock instrument that has its own amp envelope does
  not get the default Envelope added (so it sounds as it does now).
- **Stock effects inside** (Saturator, EQ Eight, Auto Filter, …) run per note: true per-note processing, at one copy per
  sounding voice. Outside, they are ordinary.
- **The group's panel** (Device view, the first device of the track; the group's mini-circuit from TX.groups) holds the
  polyphony settings: voice count, mono / legato, glide, stealing mode.

## Signals crossing the group's edge

- **Notes:** the group's In carries the notes (hz, gate, velocity) per voice. A notes cable to a block outside any
  instrument group gives it the newest note only (one pitch / gate / velocity).
- **Into the group from outside** (an LFO, a track's filter output, another track's signal): the same value for every
  voice (a broadcast).
- **Out of the group from inside** (a per-note signal to anything outside, e.g. an envelope onto an effect's knob):
  each such cable has a **reducer the user chooses**: **sum** of the notes (default; what audio needs), **max** (the
  largest value among sounding notes: one envelope however many notes), or **newest** note. Shown on the cable; chosen in
  its menu (and on hover info).
- **Feedback from outside back in** (e.g. the track's filter output cabled into the synth's pitch inside the group):
  exact, **per sample** (SPEC §4: feedback loops run per sample). The scheduler fuses the loop into one per-sample region
  spanning every active voice's part, the voice sum and the outside part; the z⁻¹ goes on the wire that closes it (the
  circuit's rule). This is the former TX.voice-loop; it is part of this plan, not deferred.

## Single-signal cables (user 2026-11-06)

Any cable **inside** an instrument group can be switched from per note to **single signal** (cable menu / click; its own
colour), with the same reducer as an out-of-group cable (sum default, max, newest). The voices are reduced at that
cable, so the block it feeds runs once ("a normal filter on a synth"), and so does everything downstream of it that has
only single-signal inputs. A block with any per-note input runs per note (its single-signal inputs are broadcast to every
voice, as an outside signal is); Out sums its per-note inputs and adds its single-signal inputs once. This is the one
rule behind the group edge too: a cable leaving the group is a reducing cable by default. A single-signal path that
comes back into a per-note block closes a loop across the voice sum: per sample, IG.3 (until then refused with
VoiceLoop, as an outside loop is). Model + lowering: IG.2 (or IG.1-2); menu, colour, status line: IG.4.

## Ramifications

- **Migration (sound-identical):** on load, a MIDI track's instrument (or meta voice run: the meta stages after the
  notes that today run per voice) is wrapped into an instrument group; the effects after it stay outside. Old projects
  get no default Envelope (it would change their sound). Renders diff 0 against the pre-change references,
  interpreter == JIT, for every fixture / stock project.
- **Removed:** voice-run inference (ChainNotes / ChainVoice forms), TX.cross-track guest instances (g<e>_<o>, gp<X>, the
  8-pin limit, TwoInstruments refusal), the VoiceLoop refusal, the "stock device ends the voices" stop. Loops through two
  instrument tracks work (each group's voices are its source's; the loop is per sample).
- **Free devices / main path:** the main path becomes notes → instrument group(s) → effects → mixer; the Device view
  lists the instrument group first. Blocks dropped into the group run per note.
- **Groups (TX.groups):** an instrument group is a TX.groups capsule whose In is fed notes; Ctrl+G / ungroup work as
  now; ungrouping an instrument group (or cutting its notes cable) makes its contents ordinary (they stop being per
  note): say so in the status line.
- **Instrument Racks:** keep working inside an instrument group (their chains per note, as now); a rack's own voicing
  defers to the group's.
- **Views:** per-note cables get their own colour (distinct from single-signal audio); the instrument group's capsule is
  styled as an instrument (title, polyphony badge); the reducer is shown on out-of-group cables; the free rack and the
  node menu place into the group under the pointer.
- **CPU:** cost inside the group scales with sounding voices; idle voices cost nothing; the per-sample loop region runs
  only where a loop crosses the edge (everything else keeps block processing). Measure every phase on ship.
- **Audio tracks:** no instrument group (no notes).

## Phases (one worker each, in order)

1. **IG.1 Model + migration + default.** The instrument-group notion (a group fed notes), the default group with an
   Envelope on new MIDI tracks, the per-cable reducer (sum / max / newest) in the document, polyphony settings on the
   group, schema bump with the sound-identical migration. Accept: migration renders diff 0 (every fixture, interpreter
   == JIT); undo for every new command.
2. **IG.2 Lowering: voice context = instrument group.** Everything inside runs per note (stock effects too; stock
   instruments as one voice per note), Out sums, reducers on out-of-group cables, broadcast into the group, newest-note
   for notes outside, cross-track notes into a group (no guests). Remove voice runs / guests / their refusals. Accept:
   render tests vs references (per-note Saturator = per-voice reference; reducers; layering; cross-track layering;
   Envelope deciding voice end; no Envelope = 5 ms release), migration still diff 0, perf rows (ship).
3. **IG.3 Per-sample loops across the edge** (former TX.voice-loop). Fused per-sample loop region over active voices +
   sum + outside part; voice start / end / stealing inside a block; interpreter and JIT. Accept: a feedback-FM patch
   (inside: PA → WT; outside: Auto Filter → cable into PA's frequency) sample-exact vs a hand-written per-sample
   reference, chords, interpreter == JIT, RtScope; patches without such loops unchanged (identical schedules); cost
   measured.
4. **IG.4 Views.** Instrument-group capsule styling and panel (polyphony), per-note cable colour, reducer on cables (menu,
   info), Device view first slot, node menu / free rack placing into the group, status lines for ungroup / cut notes.
   Tests through real event dispatch; goldens; 👁.

## Open (decide when implementing)

- Max / newest of a bipolar signal: max by value or by magnitude (proposal: by value; "newest" covers the other case).
- Voice stealing and glide defaults for meta-module synths (proposal: 16 voices, steal oldest, no glide).
