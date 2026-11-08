# GU milestone critique (independent reviewer, 2026-11-07, main e454820d, read-only)

Follow-ups filed as GU.f1 – GU.f9 in docs/TASKS.md.

## One hierarchy
- **high** Racks recognised by block name: `racks.cpp:163-165` (`isRoleGroup`), `racks.h:71-74`, `edit_racks.cpp:118,291,588,675,693` match "Mixer", "Key Zone", "Velocity Zone", "Select Zone", "Chain Select" by name. A user group named "Mixer" is taken as a chain mixer; renaming one drops it. Fix: a stable role setting or derive from wiring. (GU.f1)
- **high** Two panel systems remain: `DevicePanel` over generated `PanelLayout` for leaves (`device_panel.cpp`, `panel_layout.cpp`) beside `GroupPanelFace`. Fix: leaf frame as a synthetic one-element group panel. (GU.f2)
- **med** The track is still its own container kind: main-path series (`main_path.h`) and the Device view's free rack (`free_rack.cpp`). Fix: track block set as a root group (In = source, Out = mixer).
- **med** Dead "stock instrument that is not an engine" checks: `edit_devices.cpp:47,64`, `edit_blocks.cpp:124`, `checks.cpp:301`, lone-stock-instrument branches in `lower.cpp`. `DeviceKind::Instrument` could be derived ("fed notes"). (GU.f7)
- **med** Dead: nested meta-site machinery (`meta.h:38`, `meta.cpp:72`, `lower.cpp:249,3003`), `app::DeviceInspector`, `CircuitEditor::Host`. (GU.f7)
- **med** Retired PA/VCA load conversion still in `modules.cpp:1028-1304` + `meta/retired.*`, contradicting "compatibility is a non-issue". (GU.f7)
- **low** Engine leaves still use chain-level meta vocabulary (`normaliseMetaKinds`, `chainStages`).
- **low** Capsule header says "12 devices" (should be "blocks"), `project-module-circuits.png`.

## UI / aesthetic (from goldens and review PNGs)
- **high** Closed module panels have no title (`project-patch-panels.png`). (GU.f6)
- **high** Closed-panel text at zoom 1 is ≈ 5-6 px, unreadable. (GU.f6)
- **med** Mod cables drawn over panel labels (LFO cable over Utility "Balance"). (GU.f6)
- **med** Off toggles look disabled: grey on grey (`docs/reviews/GU10/*-old-vs-new.png`). (GU.f6)
- **med** Operator panel regression: Coarse "1.00", Level 0-1 not dB, no algorithm / waveform / envelope times, mostly empty. (GU.f6)
- **med** Wavetable Osc tab: icon-only knob captions, cramped "os", double-ring Pitch knob (`wavetable-synth-osc.png`).
- **med** Inspector clipped at the bottom (`panel-window-edit.png`). (GU.f6)
- **med** Value-block captions clipped / missing in opened circuits (`project-module-circuits.png`). (GU.f6)
- **low** LFO old-vs-new review image has no old side, garbled text.
- **low** Utility Balance reads "0.00" not "C".
- **low** Large empty canvas areas in some goldens (initial scroll / fit).

## CPU efficiency
- **high** Saturator computes all 7 curves + 6 muxes + soft-clip tanh per sample (`modules.cpp:765-792`); the optimiser folds only literal mux indices. Fix: undriven stepped value block into a mux index lowers as a literal. (GU.f4)
- **high** LFO: all 5 shapes plus free and synced phase paths per sample (`modules.cpp:822-838`). Same fix. (GU.f4)
- **med** Wavetable Synth LFOs are full Wavetable engines ×2 oversampled, per voice.
- **med** Muted rack chains compute with Gate 0 (T4.8 left them out). (GU.f4)
- **med** No ship rows recorded for the module presets vs the stock patches (GU.10 acceptance). (GU.f9)
- **low** 1-segment linear curve +0.45 ns/frame over mul+add.

## Feature gaps for a module designer
- **high** Group-panel knobs: no wheel, Show Automation, Knob Curve, modulation ring. (GU.f3)
- **high** No filter / delay ingredients (Filter module first, per GU.md "Next native ingredients").
- **high** Per-voice cable into a control ends voices once its tap is silent (Q230). (GU.f5)
- **med** Bool inputs sum wires as reals: select / comparisons unusable. (GU.f8)
- **med** Smaller: panel-less group can't be embedded; break-apart loses the child frame; tab and bound-display state not saved; no schema delete / rename; Wavetable at an instrument In hears Hz; unmapped macros have no block; macro mapping curves dropped.
- **med** Circuit selection doesn't move the Device view to the group (open since GU.3).

## Test debt
- **high** No GU phase ran -Affected / -Full; the milestone -Full on e454820d failed 9 entries (GU.12).
- **high** `pq_bench_inspect`, `pq_bench_silence`, `bench/patches/operator_calibration.pqp` reference retired / deleted files (GU.12).
- **med** Deleted tests not replaced: 20,000-deep nest, chain-circuit bench, trimmed rack tests, GU.5 W-drag / minimise / align. (GU.f9)
- **med** ≈ 30 goldens re-recorded during GU without inspection; they lock in the defects above.
- **med** No test renames a chain's Mixer (GU.f1).
- **low** Some view tests compare knob positions at 1e-12 after float round trips.
