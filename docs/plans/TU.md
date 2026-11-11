# TU: Tunings as shared project objects; automation recording (plan)

User decisions 2026-11-10 (SPEC §15 "tunings are project objects"; "automation recording").

## TU.3: no private tunings; every assignment names a project tuning

User: "the tuning shouldnt be applied per track, it should be applied to the entire project file / midi tracks. then if
one wants to have multiple tunings they can create a second tuning … project sets a default, then tracks are set to that
default but can be changed, then clips are set to the track, but also can be changed. They'll be set to tunings you
define, A, B, C, D, etc that other midi channels link to; so each tuning is an object that multiple channels or clips can
be connected to that way you can change the tuning for everything at the same time."

1. **Tunings are project objects** named A, B, C, … (renamable; the existing main tunings, `Project::tunings`). The
   project has a **default tuning** (stored explicitly, a `TuningId`; today it is implicitly the first main tuning).
2. **Assignment chain:** clip → its track → the track's group chain → the project default. Each level is Inherit or one
   named tuning (`TuningMode::Main`). `TuningMode::Private` and `TuningAssignment::privateDef` are deleted with every
   path that creates or reads them (no compatibility: old files with private tunings fail to load or drop them).
3. **Editing edits the shared object.** The piano roll's tuning menu, the MOS wheel, the clip box Generator, the mixer
   strip's tuning chip, the tunings panel: all edit the named tuning the target resolves to (`SetTuningDef` /
   `SetTuningParam`), so every track and clip linked to it changes at once. Today `mos_wheel.cpp:66–67` builds a private
   tuning for clip / track targets: that becomes an edit of the resolved main tuning.
4. **Picking / creating:** every tuning chip / menu (clip, track, project default) lists the project's tunings (A, B, …,
   each with its colour and summary) plus Inherit (clip / track) and "New tuning" (creates the next letter as a copy of
   the current one and assigns it). The project default is set in the tunings panel / control-bar chip.
5. Lowering, pitch resolution, live input (KB.2's `doc::liveClip` stays: a clip may still differ from its track),
   piano roll, project patch bands: only follow the chain; nothing else changes.

## AR.1: automation recording for every automatable parameter

User: "if you adjust the tuning while recording, the tuning generator size should be automated and recorded"; scope
chosen: every automatable parameter (one mechanism), tuning params included.

1. While the transport records (arrangement record), moving any automatable parameter (device knob, mixer volume / pan /
   send, macro, wire amount, tuning generator / period / stretch / equave / reference from any of its controls) writes
   breakpoints into that parameter's automation lane at the playhead (Live's touch behaviour: writes while the control
   is held / moved, overwriting the lane under the playhead over that range; on release the lane's existing automation
   plays again). Not recording: the edit only changes the value, as now.
2. One recording pass is one undo step together with the notes / audio it recorded (the T6.7 recording transaction).
3. Thinning: breakpoints reduced on release (a tolerance per param range), so a drag doesn't leave one point per frame.
4. The lane is created if it doesn't exist; automated controls then turn by themselves (TL.6's follow).
5. Live's Automation Arm toggle in the control bar (on by default), so recording notes without automation stays possible.

Verification: TU.3: document model + lowering + views → `-Affected`. AR.1: document + views + transport → `-Affected`;
gestures through real dispatch (a knob drag while recording writes the lane; Ctrl+Z removes it with the take).
