# GU plan: built GUIs — modules are groups, every block's face is a panel of elements

Status: **approved 2026-11-06 (user: "carry out this plan"), revised after three audits; phases ordered so nothing is
built twice.** From SPEC §15 2026-11-06 "built GUIs", the code on `main` at bc35d549, docs/plans/MM.md and IG.md. The
phases are `GU.n` in TASKS.md.

## Goal

One bottom-up kit from which every effect and instrument is built: **new effect = native DSP ingredients (atoms, a few
native engines) + groups + panels**, with no per-module UI C++. The built-in modules (Phase Accumulator, VCA, Wavetable,
Envelope, the analysers; later Filter, Delay, …) are made with the same kit the user has. Stock Live-parity devices are
out of scope: they will be rebuilt with this kit later; their `panel.json` files stay frozen until then (no work on
them, but nothing may break: they get the new curves like every param). **Meta Operator is deleted** in GU.4 (user
2026-11-06: rebuilt with the kit, with a better GUI, after GU is done; not kept alive meanwhile).

## Concepts (the whole vocabulary)

- **Block**: a native block (an `atom:<name>` block, a **value** block, a native engine `meta:<type>` such as the
  wavetable oscillator, a stock device) or a **group**. A group holds blocks and wires and nests.
- **Control**: a param of a block. It stores its **knob position** t ∈ [0, 1] and owns its **curve** (below).
- **Setting**: a structural value of a block (a choice whose change recompiles: an instrument group's voice count, mono,
  stealing mode). Panels bind settings as well as controls.
- **Panel**: a block's face. A native block has a registered default panel; a group has one the user builds (empty: the
  group is shown as its mini circuit, as today). A group drawn collapsed in the circuit is drawn as its panel.
- **Element**: one thing on a panel: kind, rect, style, **binding**, optional **visibility condition** (shown only while
  a tab / menu / toggle has a given value: the tabs of GU.8). Bindings: a control, a setting, a block's data (harmonic
  table, envelope breakpoints), a signal (an output port, for displays and mod-source handles), or a whole child panel.
  A binding names the block by **DeviceId** (+ meta path for a native engine's inner instance), never by name: ids are
  stable across renames and are remapped on paste.
- **Element registry**: `registerElement(kind, factory)`. Generic kinds: knob, slider (h / v), number, toggle, button,
  menu, XY, label, shape, tabs, curve editor, mod source. Rich kinds (native canvases): harmonic editor, table display,
  draw tools, envelope editor, phase orbit, scope, spectrum, spectrogram, meter. Adding a new rich element = one
  factory.
- **Group preset**: a library file holding a group (members, internal wires, panel). Spawning a module = pasting one.

## Curves

A control's curve maps knob position t → value: **min, max, unit, step, readout format, and a list of up to 16
segments**. Each segment runs from one point (t, y) to the next and has its own **shape** — linear, exponential,
logarithmic, power, S-curve — with its own **bend** (tension, continuous, 0 = straight). Points are draggable, bends are
dragged on the segment (the curve editor element: Vital / Serum style). Presets: linear, exponential, logarithmic,
octave (Hz), the VCA's law (three segments: log ends around a fine ±1 zone). A plain exponential knob is one segment;
a drawn curve is many. Curves need not be monotonic.

Three flags cover what today's laws do beyond a shape (needed for GU.4's identical renders):
- **wrap**: the knob drags around its range and a wire's sum wraps (a phase: today's `@wrap`).
- **signed**: the curve is mirrored for negative positions, so a wire can push the value below zero (the Phase
  Accumulator's frequency: today's `@signed`).
- **output conversion**: a fixed step after the curve, for units the DSP takes in another form (dB → linear gain).

Rules:
- **One evaluator** (`dsp`, plain C++, used by the UI, the lowering and the atom): face, readout and sound agree exactly.
- **The knob position is the stored value** (user 2026-11-06: editing a curve changes what the knob position means; the
  knob stays, the sound follows). Automation and macros record knob positions, so a curve edit reshapes them with the
  knob; lanes display units through the curve.
- **Typing a value** (or any edit made in units: a preset's value, a default) sets the knob position that gives it. On a
  curve that goes up and back down, two positions can give the same value: the one nearest the knob's current position
  is used. A value the curve never reaches snaps to the nearest one it does.
- **Curves are not automated or modulated** (user 2026-11-06: shelved until much later). The knob position is; the
  curve's shape changes only by editing it. When this returns, only a few fields (e.g. min, max) become params.
- **Wires** add to the knob position (+1 = the whole range, through the curve; SPEC §15 answers to Q186), or, with the
  control's "add in units" switch (default for a linear curve), to the value after the curve.

**Where the curve is evaluated (CPU):**
- **A control with nothing driving it** (no wire, automation lane or macro): the UI thread evaluates the curve whenever
  the knob or the curve changes and writes the resulting value as the runtime param, exactly as a value in units is
  written today. No atom, zero audio-thread cost, inside instrument groups too (where control regions run per voice
  group and are never memoised).
- **A driven control** (whether a control is driven is already structural, `lower.h`): lowering puts one native `curve`
  atom after the sum of knob position and wires (not drawn in the circuit), at the wire's rate. The curve's **class**
  (segment count, which shapes, the flags) is structural and specialises the atom (one segment: no search; all linear:
  no transcendentals); point positions and bends are runtime data, swapped atomically, so dragging a point never
  recompiles; adding / removing a segment or changing a shape does (one recompile, as any structural edit).
- Replaces `doc::KnobCurve` / `ParamCurve`, `meta::Curve`, the per-curve atom chains in `src/document/src/lower.cpp`,
  and (in GU.4, with the VCA type itself) `ParamSpec::unitsInBody` / `noKnobCurve` and the VCA's in-body law.

## Panels and the panel window

- **A new document model, informed by the existing panel code**: `pq/views/devices/panel_desc.h` (views-side, bound by
  param name within one device, with selectors, `repeat` and templating) is the reference for layout and element kinds,
  not something renamed into place. The document Panel is new: stored on the Device, commands with undo, serialised,
  bindings by DeviceId, style fields, the registry, the visibility condition (the `when` / selectors idea, simplified).
  The two coexist: stock `panel.json` files keep loading through the old reader untouched until those devices are
  rebuilt, then the reader is deleted.
- **Every face element is separable and restylable.** Rich elements cover only the canvases that are not plain controls;
  every knob, slider, toggle or number inside a face (the Wavetable's position, the Envelope's times) is a generic
  element, so it can be reshaped like any other.
- **The panel window**: a button in the group's header opens it as a floating window over the canvas (move, resize,
  minimise, reopen from the header). Select a control on any block in the group and send it (button, hotkey, or drag
  onto the window): it appears on the panel and stays where it was; both are the same control. Link lines in the circuit
  show which control each panel element is (while the group / panel is selected, or for the hovered element); the panel
  shows no wires.
- **Edit vs play** on the panel: edit moves, resizes, aligns, snaps (4 px at zoom 1; Alt off) and opens the inspector
  (binding, curve, style, visibility); play interacts.
- **Modulation from the panel**: a mod-source element (an output's handle) dragged onto a panel control makes the real
  cable; the control shows a modulation ring.
- **Nesting**: a child group left whole on its parent's panel is part of that GUI (its panel embedded); **break apart**
  replaces it with copies of its elements (same bindings), each movable, restylable, removable. Any depth.
- **Styles**: per element kind, style fields (a knob's pointer arc / pie / dot / line, track width, colours as theme
  tokens, readout position, label …). Named style schemas (save / apply a set) come after the fields exist (GU.8).
- **Drawing**: through `pq/ui/face_painter.h` with its split: static layers rasterised once per zoom into coverage tiles,
  shared by every element with the same kind + style + size; values, hover and voice dots as live geometry.

## Nesting and instrument groups

Only the **outermost** group fed notes is the instrument (IG): its polyphony settings apply and its Out sums the voices.
A group nested inside it is a plain group and runs per note because its parent does; a nested group never voices again.
Single-signal cables (IG) work unchanged across nested group edges.

## Phases (one worker each, strictly in order)

**Prerequisites:** IG.1–2 merged (done, 58ec7c9e). IG.3 and IG.4 are **not** prerequisites (user 2026-11-06: the
polyphony work is finished as far as GU needs it). What GU needed from IG.4 moves into GU: GU.2 owns the group capsule
and header drawing, including the instrument-group styling (a capsule fed notes reads as an instrument); GU.3 makes the
instrument group's polyphony settings (voices, mono, stealing, glide) bindable settings, edited in the group's
inspector and on panels. IG.3 (per-sample loops across the group edge) and IG.4's remaining items (per-note cable
colour, reducer menu, single-signal toggle, status lines) stay as follow-ups and do not run during GU.

Ordered so nothing is built twice: groups can nest and modules can be groups *before* any panel is made, so every panel
is built once, on the group it ends up on; the VCA's law is not touched until the VCA is replaced; each phase owns its
part of the group drawing.

**Speed and compatibility (user 2026-11-07, overrides the acceptance lists below):** there are no old projects, so
**no migrations, no load conversions, no compatibility code**: schema bumps without a migration; the repo's own
fixtures, presets and goldens are regenerated or deleted instead of converted. Testing is **light**: a few focused tests
per phase on the new behaviour (UI gestures through real dispatch; a render check only where DSP changes), no broad
sweeps, no slow tier, no playback suites for UI phases, no perf benches unless the phase is about performance. Work
fast.

**Acceptance conventions for every phase:**
- *Identical render* means bit-identical (diff 0) where the arithmetic is the same atoms in the same order; where a law
  changes its arithmetic (GU.1's curve vs today's travel code) the phase states its tolerance (default: peak error
  ≤ −120 dBFS against the reference, interpreter == JIT exactly).
- *Panel vs today's face*: a new golden image plus a REVIEW.md entry with both pictures side by side for the user; not an
  automatic pass / fail against the old face.
- *No worse than today* (compile time, frame time, CPU): the baseline is measured on `ship` on `main` **before** the
  phase's change, recorded in bench/RESULTS.md with the system load; pass = within 5 % (or the bench's measured run-to-run
  noise, whichever is larger).

1. **GU.1 Curves.**
   - The curve model (segments, shapes, bends, wrap / signed / output conversion) and the one evaluator.
   - Knob position stored for every param (schema bump, nothing old to migrate beyond the repo's own files): stock
     device defaults (`.pqp` values in units become positions through the nearest-position rule at load), presets,
     automation lanes, macro mappings (their ranges in position), nested meta-instance params
     (`dev<id>:<path>.<name>`); today's Travel / Units wire laws become "add in position" / "add in units".
   - Undriven controls evaluated on the UI thread; driven ones get the `curve` atom (interpreter + JIT, specialised by
     class, points / bends as runtime data).
   - The patch language's `@curve` holds a preset name and the range only (full segment lists are document data);
     docs/PATCH_LANGUAGE.md follows.
   - Today's faces keep working (their curve menus switch presets). The VCA keeps its own law untouched until GU.4.
   - Removed: `doc::KnobCurve` / `ParamCurve`, `meta::Curve`, the per-curve atom chains in `lower.cpp`.
   - Accept: presets reproduce today's knob curves (evaluator tests, incl. wrap, signed, dB); typed-value tests incl. a
     non-monotonic curve; renders of driven / automated / undriven controls vs references (tolerance above);
     interpreter == JIT; a schedule assertion that an undriven control has **no op**, checked inside an instrument group
     too; `curve` atom bench (ship) for one-segment linear, one-segment exp and a 16-segment curve, each per voice at
     audio rate; RtScope on the param write path.
2. **GU.2 Nested groups, atom and value blocks, group presets.**
   - Groups in groups (the model allows it, `model.h:591`; lift the refusals in `checks.cpp:333`,
     `edit_groups.cpp:150`); the instrument rule above; recursive capsule drawing.
   - A lean `atom:<name>` block (lowers inline, no generated text) and a value block (one control → its value).
   - Copy / paste of a subgraph with its wires (and, from GU.3 on, its panel), ids remapped; the library format for group
     presets; the node menu spawns presets. No panels yet.
   - Accept: nest / unnest and paste round trips identical document and audio; undo; id remap tests; a group of atoms
     renders identical to the same atoms ungrouped; an instrument group holding a nested group voices once (render vs
     the flat equivalent); compile time no worse.
3. **GU.3 Panel model, generic elements, collapsed group drawn as its panel.** 👁
   - The document Panel on every group (DeviceId bindings to controls, settings, data, signals; visibility condition;
     styles; registry; commands; undo; save / load; carried by presets and paste).
   - Generic element kinds including the curve editor and tabs.
   - A collapsed group is drawn as its panel (empty: its mini circuit); the Device view shows it (rule in Decided).
   - Panels are authored as data in this phase (preset files, tests).
   - Accept: element goldens; interaction tests through real dispatch, incl. a setting bound on a panel (an edit
     recompiles once) and a tab hiding / showing elements; frame cost of 200 elements (ship).
4. **GU.4 Phase Accumulator and VCA as group presets; Meta Operator deleted.** 👁
   - Each becomes a preset (atoms + value blocks) with its panel built from generic elements; their `meta::Type`s,
     expansions and bespoke faces deleted, with `ParamSpec::unitsInBody` / `noKnobCurve` and the VCA's in-body law
     (its law becomes the VCA preset's curve).
   - Meta Operator deleted (`stdlib/devices/meta_operator.pqp`, its `panel.json`, its catalogue entry, its tests and
     fixtures), since it is built on these types.
   - The repo's own projects and fixtures that hold a Phase Accumulator or VCA (e.g. `tests/app/fixtures/all_free/`)
     are converted on load: the device becomes its preset group, and its wires, automation lanes and macro mappings
     point at the inner value blocks. A test per kind (wire, lane, macro).
   - Accept: renders diff 0 vs the meta modules they replace (interpreter == JIT); opened VCA = `in → mul ← value`;
     the load conversion tests; panel goldens + REVIEW entry; compile time no worse.
5. **GU.5 The panel window and send-to-panel.** 👁 *(First phase where the user builds a GUI.)*
   - Header button, floating window, send by button / hotkey / drag, link lines, edit / play, inspector (binding, curve,
     style, visibility), mod-source drags making cables.
   - Accept: every gesture tested through real dispatch, goldens, frame cost with a panel open.
5a. **GU.5b Modules look as before; an opened group is a circuit.** 👁 *(user 2026-11-07, after seeing GU.4: "while the
   new modules are groups, they should look exactly the same as before; expanding them open … would show the group
   view … effectively the circuit diagram intermingled with previous meta modules".)* Runs after GU.5 (it uses GU.5's
   styling) and before GU.6.
   - **Closed = the old face.** The Phase Accumulator and VCA modules spawn closed, and their panels reproduce the
     pre-GU.4 faces (goldens at 04c59ffe: meta-phase-acc, meta-vca, project-vca-markers, project-vca-panel), with
     the VCA's round 360° knob, readout, ±1 / ±10 / ±100 markers (as curve points of its Joined curve: dragging a
     marker edits the curve) and floating range / curve panel, and the PA's phase orbit with voice dots (pulled
     forward from GU.7 as rich elements). Generic elements gain the style fields this needs; nothing per-module in C++
     beyond registered elements.
   - **Open = a circuit diagram**, for every group (a general rule, not per module): atom blocks drawn as compact
     circuit symbols (the circuit editor's symbol set: ×, +, phasor, frac …), value blocks as their knob, nested
     groups and native engines as their faces / panels, laid out by signal flow (layers left to right, few
     crossings, the circuit editor's cable routing), the group's In / Out at the edges. Not the device-chain row of
     boxes with cables crossing the whole capsule.
   - Accept: closed-panel goldens vs the old faces side by side in REVIEW (near-identical, differences listed);
     open-group goldens (PA, VCA, a nested group); marker drag through real dispatch (one undo step, no recompile);
     open / close frame cost no worse.
6. **GU.6 Embed and break apart.**
   - A child panel as an element; break apart into its elements; any depth.
   - Accept: gesture tests, golden, the bindings survive renames and moves.
7. **GU.7 Rich elements; Wavetable, Envelope and analysers as group presets.**
   - Their faces split into rich elements (harmonic editor, table display, draw tools, envelope editor, phase orbit,
     scope, spectrum, spectrogram, meter) plus generic elements for their plain controls.
   - The Wavetable and Envelope become presets around their native engines, the analysers presets around their taps;
     their `meta::Type`s and faces deleted (nothing else uses them once Meta Operator is gone); the repo's projects and
     fixtures converted on load as in GU.4.
   - A signal-bound display gets its tap from lowering while it is placed (placing one is a structural edit: one
     recompile).
   - Accept: each element working alone on a panel, renders diff 0, load conversion tests, panel goldens + REVIEW
     entry, frame time no worse than today's faces (ship).
8. **GU.8 The example instrument, style schemas.** 👁
   - The user's wavetable synth (PA → Wavetable → Envelope, two LFO pairs, tabs, pitch on the panel) as a library
     preset: render test, golden, perf.
   - Style schemas.
   - Outline the next native ingredients (Filter, Delay), the modules built from them (EQ, Auto Filter) and Meta
     Operator rebuilt with the kit.

### One hierarchy (user 2026-11-07): GU.9 – GU.11

"Racks are parallel groups with macros, just viewed in the device viewer instead of the circuit viewer. I want one
hierarchy." After GU.11 there is **one container, the group**. Leaves are atom blocks, value blocks and native
engines (`meta:<type>` that are real DSP, e.g. the wavetable oscillator, the envelope, taps): those have faces but no
inside. Every other block is a group (a module, a rack, a stock device) and opens to show its blocks. The patch
language stays as the stdlib's atom / function library and the lowering's output; it is no longer a second container.

Retired code: code that cannot be rebuilt yet (no Filter module: Auto Filter, EQ Eight) is **moved to `retired/`**
(top level, not built, with a README saying what each file was and why it left) instead of commented out in place: it
stays readable for the rewrite, never rots inside live files, and greps of `src/` stay clean. Git history keeps
everything as well.

9. **GU.9 Racks are parallel groups; the rack is their Device view.** *(user 2026-11-07: "racks still exist but only
   as a visual indicator … in the circuit view racks are PARALLEL GROUPS, not a singular group … racks were just a way
   to visualize and interact with the thing in device view, though it's still 1-1 isomorphic with parallel groups".
   A first attempt that wrapped the chains in one group and dropped rack features was reverted.)*
   - **Data:** each chain is a group. A rack is **derived from wiring** (user's choice): sibling groups fed from one
     point and summed into one point are one rack; no flag, no tag, no container. Wiring two groups in parallel by hand
     makes a rack. Macros are value blocks wired into the rack's chains, shown at the rack's head in the Device view.
     The `Rack` / `Chain` structs go; everything they held lives in groups, value blocks, atom blocks and wires.
   - **Feature parity:** the Device view keeps today's rack UI working over those groups: chain list, chain volume /
     pan / mute / solo, chain selector, key / velocity zones and fades, macros, Map mode, macro variations. Each is
     made of blocks inside / around the chains (a chain's mixer = value blocks + atoms in its group; zones = voice
     atoms as blocks), not a lowering special case.
   - The circuit shows the chains as parallel group capsules with their shared input and sum; the Device view shows
     the same thing as a rack. Same objects, two views.
   - Light tests: renders vs today's rack (diff 0 where arithmetic is the same), Device-view gestures via real
     dispatch, goldens + REVIEW 👁.
10. **GU.10 Stock devices as group presets; the unbuildable ones retired.**
   - Utility, Saturator, the LFO device and Operator rebuilt as group presets from atom blocks, value blocks and the
     modules (PA, VCA, Wavetable, Envelope), each with a panel from elements (looking like their faces today where
     elements allow; differences listed in REVIEW). Patch-language functions they use (e.g. `sat_mix`) stay as atoms
     or become nested groups, whichever keeps the circuit readable.
   - Auto Filter and EQ Eight are retired to `retired/` (their `.pqp`, `panel.json`, tests, fixtures, catalogue
     entries, bench rows) until a Filter module exists; projects / fixtures holding them load with the device
     replaced by an empty pass-through group named after it (a status line says so).
   - The default instrument on new MIDI tracks becomes a group preset (no stock instrument patch left).
   - Accept: renders vs today's devices (GU tolerance; list any deliberate change), load conversion tests,
     goldens + REVIEW 👁, compile / CPU no worse than the stock patches (ship rows).
11. **GU.11 One view, one panel system.**
   - The circuit editor and text pane of a device's patch (user devices, `Device::source`) merge into the group view:
     opening any block that has an inside is opening a group; the text pane shows the group's lowered patch
     (read-only), or is retired if nothing needs it. User devices with their own patch text are converted on load
     (the text's blocks become a group) or retired with the reason recorded.
   - The old panel system (`panel_desc.h`, the `panel.json` reader, stock bespoke faces) retired; `Device::source`,
     `Device::rack`, `chainInput` and other per-kind fields that no longer have users removed.
   - Accept: the model has exactly blocks, groups and native engines (a test asserting it over every fixture and
     library preset); ARCHITECTURE rewritten around the one hierarchy; goldens + REVIEW 👁.

## CPU efficiency

The GUI is UI-thread only. An undriven control costs nothing on the audio thread (its curve is evaluated on the UI
thread when it changes); a driven control costs one specialised curve evaluation at its wire's rate. Displays cost
nothing without a reader (TapRing). Atom blocks lower inline, so a module built as a group schedules the same atoms the
meta module's expansion did (GU.4 and GU.7 accept: identical render, compile time no worse). Static element layers are
shared per kind + style + size.

## Out of scope

Auto Filter and EQ Eight rebuilt (they wait for a Filter module; GU.10 retires them); Meta Operator as such (GU.10's
Operator preset replaces it); drawn / imported knob graphics; automating or modulating curves. (Racks and stock devices
were out of scope until the user's 2026-11-07 "one hierarchy" decision: now GU.9 – GU.11.)

## Decided (user 2026-11-06, or decided by the planner where the answer was clear)

- A new group's panel is empty; the Device view shows the mini circuit until something is placed.
- Link lines only in the circuit.
- Wires add in knob position by default; "add in units" is the default only for linear curves.
- Curves are edited, never automated or modulated (shelved, user).
- Meta Operator is deleted rather than kept alive on the old types (user).
- Deleting a block removes the panel elements bound to it (one undo step brings both back). A panel binds only controls
  and settings inside its group (any depth); a block moved out of the group takes its elements off that panel.
- The Device view shows the panel of the selected block's outermost group; with nothing selected, the track's
  instrument group.
- IG.4's "group panel" (voice count, mono / legato, glide, stealing) becomes the instrument group's own settings and
  controls: GU.3 shows them in the group's inspector and binds them to panels like any other (IG.4 no longer does).
- Only the outermost group fed notes is an instrument; nested groups never voice again.

## Next native ingredients (outline, GU.8; not yet tasks)

The kit after GU.8 makes oscillators, envelopes, VCAs and displays. What it cannot make yet is filtering and time
(memory beyond one sample). Each ingredient below is a **native engine leaf** (a `meta:<type>` with real DSP and no
inside, like the Wavetable) or a plain atom, wrapped as a module group with a panel from elements; the modules after
them are then group presets with no new C++.

- *Filter and EQ: superseded by docs/plans/EQ.md (2026-11-07): an `svf` atom and band groups, not an engine leaf.*
- **Filter** (engine leaf `meta:filter`): the SVF (stdlib `filters/svf.pqp`, `svf_fast.pqp`: LP / BP / HP / notch /
  peak / shelves from one state, cheap per-sample cutoff modulation) and a ladder (4-pole, resonance to
  self-oscillation, drive). One engine, the topology a structural setting (each mode specialises the kernel); cutoff
  and resonance controls with octave / linear curves, audio-rate inputs for cutoff and resonance so a wire is
  per-sample without a live-path switch. Voice-lane SIMD inside instrument groups (one filter per note is the common
  case). Its rich element: a response display (magnitude curve from the exact transfer function, as EQ Eight's view
  computes today), drag cutoff / resonance on it.
- **Delay** (engine leaf `meta:delay`, on the `delay` / `delay_cubic` / `delay_allpass` atoms' memory): time in ms
  or synced to the tempo (beats: a setting), feedback with an optional filter in the loop (a nested Filter module on
  the feedback wire, the loop's z⁻¹ handled as the existing per-sample loops), ping-pong as two lines cross-wired.
  Smoothly modulated time (no zipper: the cubic read); memory preallocated for the maximum time off-thread.
- **EQ** (group preset, retires EQ Eight from `retired/`): N Filter modules in series, each with mode / frequency /
  gain / Q value blocks, their panels embedded behind a shared response display that sums the bands. Band count is
  how many modules the preset holds; a band switched off is an inactive group (no ops).
- **Auto Filter** (group preset): Filter module + an LFO pair (Phase Accumulator + Wavetable, as in the Wavetable
  Synth) and an envelope follower (needs a small `follower` atom: rectify → one-pole attack / release) into its
  cutoff through value blocks for depth. Retires Auto Filter from `retired/`.
- **Meta Operator, rebuilt from the kit**: four (or N) operator groups, each Phase Accumulator → Wavetable → Envelope
  → VCA, the phase-modulation input being the PA's `+ pm` add fed by other operators' outputs (the algorithm = the
  wires; a few algorithm presets rewire them in one step), feedback as a wire with the per-sample loop. Its panel:
  tabs per operator (embedded operator panels), an algorithm display (a rich element drawing the operators' wiring),
  and the shared pitch / envelope controls bound across the operators. A user can open it and change any of it.

Order when they become tasks: Filter (unblocks EQ and Auto Filter, and GU.10's retirements) → Auto Filter → EQ →
Delay → Meta Operator. Each: renders vs the stock device it replaces (GU tolerance), interpreter == JIT, a ship bench
row per engine (per voice and free), goldens + REVIEW.
