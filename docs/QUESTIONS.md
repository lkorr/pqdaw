# Questions for the user

Agents: add questions here instead of guessing on anything important. Use this format:

```
## Q<n> · <date> · <task> · <short title>
- Context: …
- Options: A) … B) …
- Provisional choice: … (why it's the most reversible)
- Status: open | answered (answer: …)
```

Q1–Q218 are in docs/archive/QUESTIONS-Q1-Q218.md (grep it by number or keyword).

## Q219 · 2026-11-07 · GU.1 · Curves: the choices made where the plan left room
- **Add in position or in units is derived, not a switch.** An Add wire adds in the knob's position unless the knob's curve is linear (`doc::addsInUnits`), where adding in units after the curve is the same sum with one op fewer. The plan's "add-in-position / add-in-units" law is therefore a property of the curve, not a per-wire or per-param setting. If the user wants a curved knob whose wires still add in units, that needs a flag on the param (or the wire); none is offered yet.
- **Automation lanes hold positions, so old RAMPS sound different between their breakpoints.** The migration converts every breakpoint exactly (each lands on its old value), but a ramp between two of them now moves linearly in knob position: on an octave cutoff a 200 Hz → 6 kHz ramp passes the geometric mean (≈ 1.1 kHz) where it passed 3.1 kHz. Holds and jumps are unchanged (the GU.1 render references: `automation_steps` within −144 dBFS; the ramp case `automation` is checked breakpoint by breakpoint). Converting ramps faithfully would need extra breakpoints or a per-lane "interpolate in units" flag; neither was added. The arrangement draws device lanes linearly in position (the knob's travel), with readouts through the curve.
- **Racks keep their macros and selector in 0 … 127 with the old travel code.** Macros are not curves; a macro mapping still maps a 0 … 127 macro onto the target's knob position. Their unification with curves is left to a later GU phase.
- **Old presets are refused.** A preset older than schema 20 that holds params cannot be converted without knowing the device's curves at preset time, so it is refused with a message. A project older than 20 is converted when the device library is given (`LoadOptions::devices`, the store's library through `DocumentStore::setDeviceLibrary`); without it, a file that holds values to convert is refused.
- **The curve atom dispatches per call on the data's class, yet SetParamCurve is still Structural when the class changes.** The kernel could follow any class without a recompile; the compile is kept on a class change as the plan says (GU.md: "adding / removing a segment or changing a shape does [recompile]"), so the JIT and the cost estimate see the class. Dispatching per call means a class change is also safe in the moment between publishing the data and the recompiled image arriving. A curve edit inside its class (the same presets with other bends, ranges, a moved point) publishes the new data atomically: no recompile.
- **Travel wires into a wrap / signed curve now wrap or mirror.** Migrated Travel wires add position · amount before the curve; a wrap curve (a phase) wraps that sum and a signed one (the Phase accumulator's frequency) mirrors it below 0, where T4.7 clipped the travel to [0, 1]. Only projects with a Travel wire into such a param change.
- **Unranged params clamp.** A param with no declared range gets a linear curve over min(0, default) … max(1, default) (the panels' rule) and its knob position clamps to [0, 1]; T4.7's unranged travel did not clip. No stock param is unranged.
- **The driven control's key.** A driven knob's runtime param is `d<id>__<name>@pos` (`doc::kPositionKeySuffix`) so a project link can tell a position from a value (an undriven control's key stays `d<id>__<name>` and carries units).
- **Readback is within float rounding.** A value set in units is stored as its position; reading it back gives the value within about 1 ulp of the range (1299 c may read 1299.0000000000002 c). The inverse keeps the knob where it is when a write gives back the value it already shows (no drift on repeated writes); readouts round as before (the Hz / s thresholds treat a value within 1e-9 of 1000 as 1000, so "1.00 kHz" no longer shows as "1000 Hz").
- **Frequency-driven renders cannot meet −120 dB over long renders.** The `switched` reference case was re-recorded at −30 dB (its output reached 7.5× full scale): the curve atom's float rounding relative to the old atom chain then measures −109.5 dBFS at its +13 dBFS peak, i.e. −123 dB relative to the signal; the check is relative to the reference's peak where that is above full scale. An oscillator whose frequency comes through a changed rounding drifts in phase over a long render, so such cases are kept short. **One fixture misses the −120 dBFS bound**: tests/views/fixtures/meta-cables (a wire into a Phase accumulator's `freq`, Exp0) renders −117.1 dBFS from its pre-GU.1 render (−121 after 0.5 s, exact after the note); both are float approximations of the same curve, so no kernel change makes them agree. Accept, or should driven frequency curves evaluate in double (≈ 2× the atom's cost)?
- Status: open (provisional choices above).

## Q220 · 2026-11-07 · GU.2 · Nested groups, atom and value blocks, block sets: the choices made where the plan left room
- **Nesting is membership only.** Ctrl+G on blocks inside a capsule makes a group there and moves them in; no wire is rewired through the new group's terminals (on the main path Ctrl+G still turns the series into In → … → Out chain links). Inside a group nothing is on a path, and wires name devices, so the sound cannot change (renders diff 0) and Ctrl+Shift+G gives the very document back. Alternative: rewire a series of members through the nested terminals (as on the path) — more "boxed", but it would route signals through In terminals whose width only inference knows (the mid rule) and could change sound.
- **Only the instrument group's own In carries the notes' meanings.** A nested group's In carries what reaches its `x` (a per-note signal, audio); the note's Hz / phase by the meaning of the input (TX.groups-fix1) comes only from the outermost (instrument) group's In. Wire In → a nested block directly (a cable across the nested edge) for a note's pitch.
- **An instrument group is not nested into a new group by Ctrl+G** (refused: it stays outermost; its In would lose the notes' meanings). Dragging one into a capsule is allowed (it then is an instrument group only if still fed notes and no group around it is one).
- **SetDeviceGroup is Structural** (was Meta under TX.groups): since IG.1-2 membership decides what runs per note, so a Meta edit left the audio stale. test_commands' expectation changed with that reason.
- **No schema bump.** Nesting, "atom:<name>" and "Value" devices and a preset's "wires" key are additions; an older build refuses a nested file at its checks ("groups do not nest") and bypasses unknown patches. GU.3 adds the panel to the device object (with a bump).
- **Atom blocks**: every input is a signal input (no knobs: controls are value blocks, GU.4's "in → mul ← value"); an atom of any kind (select, z1, mux …) is a block of reals; voice sources (voice.*) are not blocks even inside instrument groups (needs a voice-context check at placement: later); required inputs left unwired read 0 (as the node menu's one-atom devices always did: an unwired div reads 0 / 0). The node menu's atoms now spawn atom blocks; typed specs (`* 2`, `0.5`) stay one-node user devices.
- **Value block**: the declared curve is linear 0 … 1; its range and law are set by editing its curve (GU.1). Its output is param rate until a wire drives it.
- **Block sets**: the clipboard is the session's (ViewContext), not the OS clipboard; paste puts the top-level blocks at the pointer (a step apart) and lays out what is inside capsules afresh (the copied nudges, view state, are not carried); note-source and mixer cables are never copied. A group preset is a `.pqpreset` with the group first, everything inside it and a "wires" key (no new extension; saved under User Library/Groups/; the browser lists it with the audio effects by its first "kind").
- **The instrument capsule's look**: the instruments' accent (orange), a 1.5× frame, "Instrument · 16 voices" (· mono; "Instrument" alone when the voice count is the link's default) in place of the device count; nested capsule bodies a step lighter per level. Every instrument group in the existing goldens changed accordingly (re-rendered).
- Status: open (provisional choices above).

## Q221 · 2026-11-07 · GU.3 · Group panels: the choices made where the plan left room
- **One command, the whole panel.** `SetGroupPanel{group, panel}` replaces the panel (Meta: a panel is never lowered); the builders (`ops::addPanelElements` / `setPanelElement` / `removePanelElements` / `setPanelSize`) each make one. Simple and undoable; a 200-element panel copies its element vector per edit (a few µs). Alternative: per-element commands (smaller undo entries) — not needed yet.
- **Elements leave with their blocks in apply's final pass.** After any structural command, elements whose bindings no longer hold (the block deleted, moved to another track, taken out of the group at any depth) are removed in the same command (one undo step restores both); a condition naming a removed element is dropped (the element then always shows). SetGroupPanel and the file reader refuse a binding outside the group instead of pruning it. The group's own settings are bindable on its own panel (its polyphony), a Panel binding only names a group inside.
- **Element ids are the panel's own** (1 …, not IdSource ids): paste keeps them (only DeviceIds are remapped, in `ops::remapBlocks`), so visibility conditions survive without a second remap.
- **Style fields are a sorted name → number | text list**, interpreted by the element kind (registry schema with defaults, for GU.5's inspector and GU.8's schemas); unknown names are kept. Colours are theme tokens ("value", "signal", "track", "handle", "ink", "frame", "tick", "hover", "glow", "body", "hub", "driven", "selected", "instrument"), never RGB.
- **Unbound tabs keep their selection in the view** (per face, across rebuilds of the panel; not saved, not undoable): switching a tab is not an edit. A tabs element bound to a setting or control switches that instead (one undo step). Alternative: store the tab in the document (Meta, undoable) — saved, but every tab click would be an undo step.
- **Settings preview, then commit once.** A setting dragged on a number / knob is previewed in the target during the gesture and written by one SetDeviceSetting at the release (one recompile); a toggle / menu click is one step. A `real const` setting of a patch declares no range: its elements use 0 … max(1, 2·|default|) (a menu's "items" field names the choices).
- **The group inspector is a generated panel** (`panel::inspectorPanel`: label + number / toggle / menu per `doc::groupSettings()`), shown at the left of an instrument group's Device-view frame only (a plain group has no settings). GU.5's inspector (binding, curve, style, visibility) can extend the same column.
- **The Device view rule** ("the panel of the selected block's outermost group; with nothing selected the track's instrument group"): the Device view lists the track's main path, whose groups are top-level (outermost) groups; each group frame shows its panel (or its mini circuit when the panel is empty). Nothing else changed: a block selected in the circuit does not yet scroll the Device view to its outermost group (no shared device selection exists). Revisit with GU.5's panel window.
- **A closed group's panel has no title** (the panel is the user's design; the plate carries no header). Its cables keep the closed block's edge pins (a wire into a control inside ends at the edge, not on the element: anchoring to elements is GU.5's link-line work).
- **Curve editor**: a straight segment's handle bends it as a Power segment (a class change: that drag recompiles once); points keep t strictly increasing with the ends at t = 0 / 1; presets keep the curve's unit, digits, step, mode and output conversion.
- **Static layers per element** are tiled in one process-wide cache keyed by (kind + style + size, layer, scale, atlas), built in `prepare()` (the host's frame at a settled zoom), drawn as one quad each; measured, the 200-element frame is dominated by the live layer (text, values), not the static one.
- Status: open (provisional choices above).

## Q222 · 2026-11-07 · GU.4 · The Phase Accumulator and the VCA as group presets: the choices made where the plan left room
- **The VCA's law is a curve shape, exactly.** A new `CurveShape::Joined` (a + e·x′ + b·expm1(c·x′) measured from the segment's flat end, its start slope matched to the neighbour) and `CurvePreset::Vca` (negative outer, linear fine zone, positive outer; arg = the zone) reproduce TMM.14's Log law in double for every range and zone (test: ≤ 4e-11 absolute against the old `vcaValue`, ~2e-15 relative). A Bend VCA converts to an Exp segment with its bend (exact too). Alternative: a fitted multi-segment Exp/Log approximation (no new shape, not exact) — rejected.
- **The VCA's markers are not carried over.** TMM.14's per-VCA markers (typed points on the knob track) have no element in the generic panel; a converted VCA keeps its curve, range and zone (all in the value block's curve) and drops the markers and their lanes. Alternative: a GU.7 rich "marked knob" element — left for GU.7.
- **Renders: the Phase accumulator is bit-exact, the VCA ≤ −110 dB of peak.** The base renders (recorded on 04c59ffe, `tests/app/fixtures/gu4_modules/`) against the converted projects: pa_voice / pa_free / pa_signed diff 0; vca_law −112.4 dB, vca_ranges −136.5 dB, vca_driven −111.8 dB. The old module evaluated e^(K·u) in float atoms; the curve kernel computes the law in double, so the new law is the more exact one. Alternative: emulate the float rounding (no).
- **The VCA multiplies (`mul`), it does not smooth (`gain`, 2 ms).** A value block's knob is a runtime param published per block; with the curve atom between them the multiplier changes at block rate. TMM.14's 2 ms ramp is gone; an audible zipper on fast knob moves would need a `smooth` atom in the preset (one more block) — not added, the renders above include driven modulation (vca_driven) within −110 dB.
- **An unwired VCA (nothing into `in`, in = its default 1) becomes one Value block** — it was a constant source of its value; wires from it keep going from the Value's `y`. Alternative: a VCA module with In unwired (multiplies 0: silent) — wrong.
- **A module inside a rack chain is refused at load** (the file does not open, the message says why). A group lives in a track's circuit; rack chains hold devices, not groups. Macro mappings only exist inside racks, so the "macro" conversion case is that refusal (test `test_module_presets.cpp`). No repo project or fixture had one. Alternative: convert it into the rack chain as a flattened … (no group in a rack: not possible yet).
- **Retired types are kept as state descriptions** (`pq/meta/retired.h`: the PA and VCA ParamSpecs, never registered, no expansion, no face) so decoding and the 18 → 19 / 19 → 20 migrations still read old files. They can be deleted once the minimum readable schema passes 21.
- **A GroupNotes In feeds an atom or group block the note's Hz.** At an instrument group's In, a block that reads no meta param of its own (`lw.paramNode == kNone`) reads the `Frequency` meaning (TX.groups-fix1's "a frequency the note's Hz"), so the Phase Accumulator module at the notes plays the note. Alternative: a dedicated "hz" voice-source block — GU.2 lists voice sources as a gap.
- **The PA's `+ pm` add exists only when the old device's `pm` was wired** (or when it was its chain input); a fresh module has no pm block (one less block per voice). A user adds an `add` where they want phase modulation.
- **The phase orbit / voice dots of the old PA face are gone** until GU.7 (rich elements: a scope-like "phase" element); the knob readouts and arcs are generic.
- **The modules are built in code** (`doc::modules.h`), not `.pqpreset` files: the conversion needs their structure and roles (which value block is which old param), and a file would duplicate it. The browser and node menu list them under Meta Modules by name. Alternative: ship them as group presets in the library and convert by reading them — more moving parts, same result.
- **Groups open by default; the scenes close the modules.** A spawned or converted module is an open capsule (as every group and group preset is since GU.2) — its blocks visible; closed, it is its panel. The view scenes and the orbit / cables tests close them. Alternative: a panelled group (or a module) spawning closed — a view default, easy to change; asked here rather than changed for every group.
- **A module at the head of a MIDI path is itself the instrument group** (the outermost group fed notes), so a meta module appended after it goes INSIDE it (IG.1-2's append rule continues the voice run). The meta-chain sketch therefore puts the Wavetable at the notes (its phase input reads the note's phase) and the Phase Accumulator free beside it (a slow ramp into the warp); a PA → Wavetable voice run is an instrument group In → PA module → Wavetable → Out (`instrumentRun` in `tests/app/test_meta_link.cpp`). Alternative: a group at the notes that is not an instrument (no voices) — a lowering change, out of scope.
- **A converted module's chain links name the group's ports** (`x` / `y`, what `chainPortsOf` and Ctrl+G use), so ungrouping an instrument group holding one still finds its series; `groupSeries` (meta.cpp) also accepts a nested group's named main output.
- **Curves on stock params beyond the declared range are still clamped** (GU.1 follow-up, not GU.4): the Value block's `param value` is now unranged (a runtime param was clamped to [0, 1], silencing every value block whose curve leaves it), but a stock device's param keeps its declared range.
- **Tests deleted or ported**: Meta Operator's tests and fixtures (with it); the PA / VCA faces' tests (`test_meta_vca_face`, `test_vca`, `test_phase_acc`, the face cases of `test_meta_faces`, the VCA marker golden) — their behaviour has no successor until GU.7; every PA / VCA behaviour that still exists was ported to the modules (Frequency drag, signed Frequency (TMM.18), the wrap phase, the VCA's knob glide, curve edits, wires into its knob, In → mul ← Value), and generic meta-module cases moved to the Wavetable / Envelope. `test_project_patch`'s count of params asked for a control dropped from > 300 to 292 (Meta Operator, the PA and the VCA left): its bound is now > 280.
- Status: open (provisional choices above).

## Q223 · 2026-11-07 · GU.5 · The panel window: the choices made where the plan left room
- **The window is a child of the project patch view, not an overlay popup**: it floats over the canvas only (clipped to it), menus open over it, outside clicks never close it. Its state is view state keyed by group (`ViewState::panelWindows`, the view's optional "panels"; no schema bump, as GU.2 added keys).
- **Where a sent control goes**: the innermost group around it whose window is open, else its outermost group (the Device view's group). The window's + sends into its own group (refused, with a message, when the control is outside it).
- **Duplicates are allowed**: sending a control already on the panel adds a second element (two views of one control); undo removes it.
- **Modes**: an empty group's window opens in edit mode; a send switches the window to edit mode with the new elements selected. Groups (panelled or not) still spawn OPEN (GU.4's open question): kept, since link lines need the inner blocks visible; closing is one double-click.
- **Snap**: edges and centres to the other elements' and the panel's within 4 px on screen (4 units at zoom 1), whole units otherwise; Alt: no snap and no rounding. No grid.
- **Selected control**: every left press in the circuit sets it to the control under the pointer (a face / panel control, a param pin, a one-control block like a value block) or clears it; with none, P sends the selected blocks' controls (all params of each).
- **Link lines** go from the element to the control's place in the circuit (a face's control, a pin, the element of a closed group's panel, else the block's centre); shown for all elements while the window is the last pressed or its group is selected, else for the hovered element only.
- Status: open (provisional choices above).

## Q224 · 2026-11-07 · GU.9 · Racks are groups: what is in, what is left out, and the choices made — REVERTED 2026-11-07 (user: racks are parallel groups, not one group; redone as GU.9 v2)
- **In:** a rack = a group whose member groups are fed from its In (`doc::rackChains`, derived from the wires: no flag);
  chains sum at Out; Instrument Rack = instrument group of chain groups (only the outermost fed group voices); 8 macros
  (value blocks 0 … 1, knobs on the rack's panel); mapping = Travel wire (amount max − min, the target knob set to min);
  the chain selector (value block "Chain Select" 0 … 127 stepped, constants "1" / "0", per chain lo / hi value blocks + sub / min / add / clip
  / mul atoms — reals only: atom blocks into bool inputs did not lower; a chain added later gets its gate). Device view: chain list (click shows, "+ Chain" adds,
  Up / Down step), the panel of macros, the shown chain's circuit.
- **Left out (provisional, most reversible):** key and velocity zones (they need the note's Hz / velocity at note-on per
  chain: a gate of voice atoms inside the instrument group — doable, but voice atoms are not blocks yet, GU.2), zone fades,
  the chain mixer (volume / pan: a Utility or VCA in the chain does it), chain solo, macro variations / Rand, Map mode in
  the Device view (wire the macro in the circuit, or `ops::mapMacro`), the old rack goldens and "Open Lowered Rack
  Patch". Options for zones later: (a) voice atoms as blocks + a gate per chain like the selector's; (b) a `zone` atom.
  I'd pick (a).
- **Mapping law:** a Travel wire (position += amount · macro) instead of a new Wire offset field: min is the control's own
  knob. Moving that knob by hand moves the mapping's min (Live keeps min fixed). Alternative: an `offset` on Wire.
- **No compatibility** (user 2026-11-07): projects / presets with old racks do not load; the repo's rack fixtures
  (`all_free/racks`, `gu1_curves/macro`) and goldens (`audio-rack`, `instrument-rack`) were deleted, the `device-automation`
  scene lost its rack (re-recorded).

## Q225 · 2026-11-07 · GU.5b · Module faces and the opened group as a circuit: the choices made where the plan left room
- **Closed by default = "seen first with a panel".** The view closes a group the first time it sees it when the group has a panel (a module, a pasted group preset, a loaded project's panelled group); a group made by hand (Ctrl+G, no panel) opens; a panel made later never closes an opened group. View state only, not saved (as before). Alternative: a saved per-group open flag in the document (undoable view state) — not needed yet.
- **The VCA's markers are points of its curve.** `dsp::CurvePreset::Vca` now splits its log outers at ±10 and ±100 (`dsp::splitCurveAt`: an Exp / Joined segment restricted to part of its width is the same shape with its bend in proportion — the function is unchanged, the tests' 1e-12 bound against the old law still holds), so the default VCA curve has 7 segments. The `dial` element shows EVERY interior point of its control's curve as a marker (generic: any curve's points), the ±1 pair long; dragging a tick moves that point's position (its value kept), its opposite-valued point mirrored (Ctrl: alone), clamped 0.004 inside its neighbours; one undo step, the class unchanged (no recompile); the curve becomes Custom. Alternative: markers as a separate warp on top of the curve (the old meta law's offsets) — a second law, rejected.
- **The popup's presets are curves.** Default (VCA ±20k), 0 → 1, −1 → +1, ×0 … ×2 (linear), Fader (Exponential 0 … 1, k 4: ≈ −18 dB at 12 o'clock, the old Bend 0.5's taper approximately), 20 Hz → 20 kHz (Octave). A chip is lit when the curve equals it exactly. Range ends drag along the default VCA scale; a preset curve is rebuilt over the new range (a Custom one keeps its points). Changing the preset or the range can change the curve's class (one recompile), as the old Log ↔ Bend switch did.
- **The PA's pitch arc keeps the old scale through its curve.** The module's Pitch value block's curve is three straight segments (±1200 cents over the middle half of the travel, the rest of ±4800 outside), so the generic `arc` element draws the old face's scale; straight segments: a wire still adds in cents. Pitch drags move along that curve (not in whole semitones as the old face's did; Up / Down and typing a pitch are not carried over).
- **Voices on the orbit need a tap.** The `orbit` element reads its signal's voices through `PanelTarget::voices(binding)`; nothing lowers a tap for a group's signal yet (GU.7: "a signal-bound display gets its tap from lowering while it is placed"), so in the app the dots do not show yet; the golden shows them through a test reader. Alternative: pull GU.7's lowering tap forward (a structural edit when a display is placed, slot plumbing through the link) — left for GU.7.
- **Opened group = circuit, everywhere.** `graph::autoLayout` (the circuit editor's layouter) lays out every opened capsule; its wires among its own blocks are orthogonal traces (its routes while nothing was moved by hand, else a Z / a detour under both blocks), wires across a capsule's edge stay hanging cables. Blocks inside an opened group keep their pins on their edges (no orbiting ports: the traces leave them straight), so faces inside an instrument group no longer orbit. Atom blocks are their circuit symbols and value blocks small knobs everywhere (free ones too), not only inside groups. Alternative: keep cables and orbiting inside groups and only re-place the blocks — the user's sketch is a circuit.
- **A knob element is hit on its dial only** (its label and readout are the panel's), so a value block drawn as a knob is moved by its label strip; a panel knob's label no longer starts a drag.
- Status: open (provisional choices above).

## Q226 · 2026-11-07 · GU.6 · Embed and break apart: the choices made where the plan left room
- **An embed is one element kind, "panel", bound by a Panel binding.** It draws the child's panel live through a nested panel face that uses the parent's target (every binding inside the child is inside the parent, so nothing re-resolves differently); the child's panel is fitted into the element's rect with its aspect kept and centred (`doc::embedFit`), so resizing the element scales it. Alternative: an embed that only reserves space and lets the parent's face draw the child's elements flattened — no element to select / move as one.
- **Only a group with a panel can be embedded.** `controlsOf(group)` is its Panel binding when it has a non-empty panel, else nothing (P says so). Alternative: embed a panel-less group as its mini circuit or send all its controls — left out (a group without a panel has nothing to show yet).
- **The embed keeps the child's plate (body + frame); a broken-apart copy does not.** Break apart copies exactly the child's elements (the plate is not an element). Alternative: add a shape element for the plate when breaking apart.
- **Break apart keeps the screen as it was**: copies at the embed's fit (scaled when the embed was resized), inserted at the embed's place in the paint order, fresh ids, visibility conditions among them renumbered (a condition naming an element not copied dropped); one undo step; the copies selected. A nested embed stays an embed (one level at a time, as the plan says).
- **Ctrl+Shift+G** in the panel window's edit mode (the circuit's ungroup chord, by analogy), and the editor's right-click menu.
- **Fixed alongside**: the panel editor acted on key releases too (every arrow nudge moved 2 units, Ctrl+D duplicated twice); it now ignores releases.
- **Cables from an embedded mod source** only reach controls of the same (child) panel; dragging one onto a parent's control is not wired. Left for later if wanted.
- Status: open (provisional choices above).

## Q227 · 2026-11-07 · GU.7 · Rich elements and engine modules: the choices made where the plan left room
- **A rich element is a section of the engine's face, not a rewrite.** The Wavetable's, Envelope's and analysers' face classes (`src/views/src/meta/*_face.cpp`) stay as the code that draws and edits them; each rich kind (`drawtools`, `harmonics`, `wavetable`, `warpmode`, `envelope`, `scope`, `spectrum`, `spectrogram`) shows one rect of that face, scaled into its element rect, and routes pointer / wheel / keys there (`engine_elements.cpp`). All elements of a panel bound to one engine share ONE face (the target's cache), so they are one state: the keyframe selected on the table display is the one the harmonic editor edits; a draw tool chosen in the top row switches the display. Alternative: split each face into independent classes with a shared per-engine model — more code for the same picture; a later refactor can replace a section kind without changing documents.
- **The faces stay registered for meta devices that are not modules**: rack chains (GU.9 open) and meta calls in user patches still show them, and the engine leaf inside an OPENED module is drawn as its face (GU.5b's rule "native engines as their faces"). Their `meta::Type`s stay: they are the native engines. Nothing was deleted: no old type was left without a user.
- **The Wavetable's position slider stays inside the table display** (its keyframe markers and gap glyphs live on it); its phase slider and fold / warp / sync knobs are generic elements, the Envelope's amount slider too. The warp-mode glyph switch and the oversampling cell are a small rich kind (`warpmode`, with the fold / sync glyphs of that row): a generic menu would not look like the old glyphs.
- **Differences from the old faces** (closed panels): the generic knobs have no bipolar fill (warp fills from its start, not its centre) and their own hover ring; the phase slider is the generic slider (no per-voice ticks when the phase input is wired); the old face's input / output leads to its jacks are gone (the group's ports); the plate is the panel's (corner from a bottom `shape` with `plate` 1: 12 like the old face, the outline 1.2 vs 1.25 thick).
- **Display elements bind a signal or an analyser engine.** A `scope` / `spectrum` / `spectrogram` / `meter` bound to a SIGNAL (any block's output) gets a tap from the lowering while placed (doc `panelTaps`: kind → Voice for `orbit`, Signal for the displays; slot `metaSlotOf(device, "#voice:<port>" / "#signal:<port>", 0)`; node ids in the tapped device's TuningNode / SendInput spaces: at most 2 signal-tapped outputs and 16 voice-tapped outputs per block); its display state is then the element's own (defaults, not saved). Bound to an analyser ENGINE (Data, what the modules do) it reads that engine's own tap and keeps its state in the engine's data (saved, undoable). Placing / removing / rebinding a tapped display is Structural (SetGroupPanel compares `panelTaps`): one recompile; moving or restyling one is Meta.
- **The signal tap layout is the Oscilloscope's**: `signal_tap(transport.beat, note, L, R)` for every display (the spectrum and the spectrogram read channels 2–3; `note` = `voice_lowest(voice.hz)` where the tapped block runs per voice, else 0).
- **The PA's orbit binds frac's output** (the phase, what the old module tapped), not the × Amplitude output it bound in GU.5b.
- **The meter is new** (no old face): two bars (L / R RMS, peak line) over −60 … 0 dBFS from the last 50 ms of the feed's scope history. No Meter module was added (nothing to reproduce); place a meter on any panel.
- **A Wavetable module at an instrument group's In hears the note's frequency, not its phase** (GU.4's rule: a nested group's main input reads the note's Hz). The old meta device at the notes read the note's phase (`Reads::Phase`). Put a Phase Accumulator before it (the usual patch). Alternative: let a nested group's In read the meaning of the input its In feeds — a lowering change, left out.
- **Spawning**: in a project the node menu, the browser and Add Free Device list the modules (Wavetable, Envelope under Meta Modules; Oscilloscope, Spectrum, Spectrogram under Visualisers) instead of the bare engines; rack chains' Add Device still lists the engines (a chain holds devices, not groups). Existing projects with free `meta:wavetable` / `meta:envelope` / analyser devices load and play unchanged (no conversion: the engines are the same types).
- **Engine faces draw their static layers directly** (no tiles: they depend on the engine's data) once per section shown; a Wavetable panel paints the face 4 times per frame while voices animate. UI thread only; idle costs nothing. If that shows up in frame time, give the section element tiles keyed by the face's static keys.
- Status: open (provisional choices above).
## Q228 · 2026-11-07 · GU.9 v2 · Racks as parallel groups: the choices made where the plan left room
- **What makes a set a rack:** sibling groups with exactly one main-input wire and one main-output wire (into a main input,
  a group's Out terminal or a mixer), sharing both ends; 2+ of them, or 1 that holds a chain mixer group (what Add Rack and
  Ctrl+G make: Live's one-chain rack). A plain group is never a rack. Alternative: any group with macros wired in is a rack
  (rejected: the modules would turn into rack frames).
- **Roles by name:** the Mixer group, its Volume / Pan / Gate value blocks and the zone groups are found by Device::name
  (as GU.4's modules are). Renaming one in the circuit takes that row's control away (the sound is unchanged).
  Alternative: a role setting on the blocks.
- **Mute and solo:** a chain's on / solo are settings of its group; the edits write the Mixer's Gate (0 / 1) from them (and
  from Volume at -70 dB = -inf). A muted chain still computes (T4.8 left it out: zero CPU). Alternative: a generic "a
  switched-off group switches off its contents" rule (today a switched-off group bypasses, which would leak the dry signal
  into the sum).
- **Macros:** a macro is a block only while it maps something (an unmapped macro has nothing that wires it into the rack);
  its value and name go with its last mapping. T4.8's mapping curve (-1 ... 1) is not carried (a Travel wire has none).
  Moving a mapped knob by hand moves the mapping's min (Q224's law). A macro's automation lane is its value block's; the
  macro knob does not follow the lane while it plays (the mapped knobs do).
- **Variations:** `var<k>` settings of the macro blocks, unnamed ("Variation k"); store / recall / overwrite / delete and
  Rand work. Shown macros, the chain list / macro panel toggles and the macro count are view state (not undoable).
- **Zones:** float atoms (in ln Hz for Hz key zones) instead of T4.8's document-side doubles: the velocity-zone render is
  within 3e-8 of the reference. A select zone still gates an Audio Effect Rack chain's input; in an Instrument Rack it is a
  gain stage before the mixer (T4.8 multiplied it into the mixer's gains). The selector exists once a chain zone does.
- **Instrument Rack:** each chain is its own instrument group of the track's notes (one voice engine, each group per
  voice); the instrument stage's out wire is single-signal (T4.8's voice sum) unless Per-Voice Chains is on: renders
  bit-identical to T4.8's. Generic lowering fix this needed: a nested group's Out-terminal
  wires are not its inputs (the per-note fixpoint).
- **Nested racks:** a chain's series treats a nested rack as one stage; moving a rack's frame into a chain moves all its
  chains, macros and selector. The Device view's 20,000-deep nest test (built from the stored `makeRack`) is gone.
- **Gone:** "Open Lowered Rack Patch" (the circuit shows the rack's blocks); the rack's own name; its activator now
  switches every chain. A library item or module dropped into a chain is added off the path and moved in (one undo step).


## Q229 · 2026-11-07 · GU.8 · The Wavetable Synth and style schemas: the choices made where the plan left room
- **The synth is a built-in module built in code** (`doc::wavetableSynthBlocks`, ModuleKind::WavetableSynth, `ModuleInfo::instrument`), not a `.pqpreset` file: the other modules are code too, a file would duplicate them and could not follow their changes. A user saving it gets an ordinary group preset. Listed under Instruments (browser) and as an INSTRUMENT in the node menu, not under Meta Modules.
- **Spawning it takes the notes only on an empty path** (`ops::pasteBlocksAtNotes`): on a MIDI track whose main path has no block it is spliced in at the source (it becomes the instrument group, plays at once); with an instrument already there it lands off the path (cable the notes in, or delete the other). Alternative: replace the track's instrument as `addDevice` does for stock instruments — more destructive for a block set.
- **The LFOs are PA + Wavetable pairs inside the instrument group**, so they run per voice and restart with each note (key-synced). A free-running global LFO would sit outside the group with a single-signal cable in. Their Wavetables use ×2 oversampling (the `os` setting 1, the least the engine offers) instead of the auto ×8: an LFO has nothing to alias. Cost: two extra wavetable reads (×2) and two PAs per voice.
- **Depths are value blocks × the LFO through Units wires**: LFO 1 adds 0 … 1 of the table to the Wavetable's position, LFO 2 adds 0 … 1200 cents to the Osc's Pitch (after its curve, so the depth is in cents). Defaults: 0.25 position at 0.5 Hz, 0 cents at 5 Hz (vibrato off so a chord plays in tune until it is wanted).
- **The tabs are unbound** (the panel's own view state, not saved, no undo step). Alternative: bind them to a setting of the group so the tab is saved with the project.
- **Style schemas live outside the document** (views, `pq/views/panel/style_schemas.h`): a user-level library (`<User Library>\Styles\schemas.json`), not per project; applying one writes plain style fields, so a panel does not depend on the schema it came from. A schema carries the kind's Number / Choice / Token fields only: Text fields (labels, tabs items, button texts) are content and are never saved or overwritten. Built-ins (knob Classic / Minimal / Pie, slider Classic / Minimal) cannot be overwritten (a save under their name becomes "<name> 2"). No delete / rename UI yet.
- **Step 0 (earlier phases' playback failures)**: the PA load conversion now keeps an old Phase Accumulator's pitch in cents through the module's GU.5b Pitch curve (it copied the knob position, which means another pitch on that curve): pa_voice / pa_free and every `gu1_curves` case are back to their references (PA diff 0; `gu1_curves` −137 … −400 dBFS, `switched` −109.5 dBFS at its +13 dBFS peak = −123 relative, within the test's bound), so test_curve_renders needed no bound change. The converted unwired VCA's value block (20 … 20000) now has 2 segments (split at its +100 marker by GU.5b); the test checks the split point is 100 and the law monotonic.

## Q230 · 2026-11-07 · GU.10 · The stock devices as group presets; Auto Filter and EQ Eight retired: the choices made where the plan left room
- **What each rebuild leaves out** (no stubs: each is simply not there; record for the rewrite from modules):
  - *Utility*: bass mono (+ its frequency and Audition) and the DC filter — both need a filter. Everything else is the old
    param-rate matrix term for term with the same smoothed `gain` / `gain_add` per sample (renders equal the retired
    patch within float rounding: tests/app/test_stock_presets.cpp).
  - *Saturator*: Waveshaper (its six knobs), Shaper (a drawn table: the `shaper` atom takes a table resource, not a block),
    colour EQ (a filter), HQ oversampling (the oversample atoms are compiler-internal, not blocks); the transfer-curve
    display. Kept: drive, seven curves, output, soft clip, dry/wet (equal to the retired patch at those settings).
  - *LFO*: the audio pass-through and the `lfo` export are gone: the preset is a modulation SOURCE (its Out is the LFO,
    its panel has a jack: a `modsource` element). Rate, Sync, Beats, Phase, Depth, Offset and the five shapes as before.
  - *Operator*: a smaller honest rebuild — four rows (A … D) of × Coarse → Phase Accumulator (Pitch = Fine) → Wavetable
    (its default table, a sine) → Envelope (Amount = Level), FM by wires D → C → B → A (algorithm 1) into each Phase
    Accumulator's `+ pm` add; only A reaches Out. Missing: the other ten algorithms (rewire by hand), denominators /
    fixed frequency / ratio display, waveforms other than the Wavetable's tables (draw them there), feedback, the filter,
    LFO, pitch envelope, loop / beat envelope modes, velocity / key scaling, spread, transpose, glide, the algorithm
    picker and partial editor; levels are 0 … 1, not dB. The panel: Coarse / Fine / Level per row and the four Envelope
    panels embedded under A … D tabs.
  - *Basic Synth*: the default is the plan's "small PA → Wavetable → Envelope group" (Envelope amount 0.25), not the old
    additive patch rebuilt (its one-pole ADSR has no module; the Envelope's curves differ).
- **Choices are muxes of real indices.** An atom block's bool input sums its wires as reals (the lowering's Add law), so
  `select` / comparisons cannot be wired: switches are value blocks 0 / 1 and every choice a `mux` (in[floor(index)]),
  a menu's Type / Shape a chain of two-input muxes (the block's variadic input is at its minimum). **CPU**: the old
  devices compiled only the chosen curve / shape (a structural constant); the presets compute every curve (Saturator:
  seven per sample, ≈ 30 ops more) and every LFO shape, and choose per sample. Utility's matrix is param-rate as before.
  Proposal: a "switch" block whose index is a SETTING (structural), so only the chosen branch is compiled — or the
  lowering folding a mux whose index is an undriven value block. Not done here.
- **`channel` is an atom block now** (its index a setting): Utility needs L and R apart. `pack` already was one.
- **A driven control's modulation tap ends voices early.** A per-voice cable into a value block (or a meta param) makes
  the lowering export the modulation-display tap per voice; the voice engine monitors every voice output for silence after
  note-off, so a silent modulator (level 0) ended Operator's voices 20 ms after note-off, cutting the release. Operator
  therefore feeds its FM into the PA's `+ pm` atom (no tap). The engine / lowering issue itself is open (taps should not
  be monitored, or should not be voice outs).
- **Nested operator groups** were tried (A … D each a group): with a cable into a group on the path to Out the voice end
  was not found either (the same tap issue); the rows are flat in Operator's group, named "Coarse A", "Phase Accumulator
  A", "Envelope A" ….
- **The stand-in devices.** The app ships no stock patch, but ~60 test files build chains, wires, groups, racks and
  lowerings out of Utility, Saturator, EQ Eight, Auto Filter, the LFO, Operator and Basic Synth. Rather than rewrite them,
  the shared test main points `PQ_STAND_IN_DEVICES` at `tests/stand_in_devices` (copies of the retired patches and panel
  descriptions; EQ Eight's and Auto Filter's displays fall back to a plain spectrum, no analysis tap), and
  `stand_in::useBasicSynth` / `makeLoneInstrument` give those tests the IG.1-2 layout. The retired devices' OWN tests,
  displays and goldens are in `retired/`. Debt: GU.11 should port these tests to blocks and delete the stand-ins.
- **Node menu / browser**: in tests the stand-in patches and the presets share names ("Saturator"); the presets come
  first. In the app only the presets exist.
- **Still in src with no user**: the lowering's analysis tap for analysed stock devices (`analysedDevice`: false for
  every device), the panel-description displays' "eq" / "filter" / "lfo" visuals (removed from `makeDisplay`: such a
  description now shows a plain spectrum), Operator's displays in `operator_displays.cpp` (used by the Operator
  stand-in's panel in tests). GU.11 retires the panel-description system with them.
- **Benches**: `pq_bench_devices` is retired; `bench/patches/operator_calibration.pqp` imports the stand-in Operator;
  `pq_bench_inspect`, `bench_playback`, `bench_silence`, `bench_device_panels`, `bench_project_patch` name stock devices
  at run time and need `PQ_STAND_IN_DEVICES` set (not run here). bench/RESULTS.md keeps its historical rows.
- **AOT seed**: its stock projects are new tracks made by `ops::createTrack` (the default preset); the stock-effect and
  Operator shapes are gone with the stock devices (and their test case).
- Status: open (provisional choices above).

## Q231 · 2026-11-07 · GU.11 · One view, one panel system: the choices made where the plan left room
- **No conversion, no compatibility** (user: "compatibility is a non-issue"): a file naming a non-block patch (a stock
  device, "BasicSynth"), a device "source" (patch text) or "chainInput" is REFUSED on load (checks.cpp, serialize.cpp);
  the schema 2 → 3 step no longer inserts the M3 default instrument (such a file's MIDI tracks load silent). The
  retired-module conversion (GU.4, PA / VCA) stays (it converts into blocks). Old-schema fixtures and their migration
  tests were deleted; the remaining fixtures were regenerated by this version.
- **User devices are retired, not converted**: no project holds one; the circuit editor's per-device mode, the text
  pane of a device, Save as New Device, Revert to Stock, `.pqdevice` files and the library index's Device kind (index
  format 4) are gone. A typed node at the project level becomes one ATOM BLOCK (`specBlockPatch`); a literal, a param or
  an operator with an operand is refused with a hint (place a Value block). Alternative: decompose a spec into an atom
  block plus a value block.
- **The one panel system**: a group's panel is THE panel. A leaf on a track's path (a native engine, an atom or value
  block) still gets a frame in the Device view — `DevicePanel` over a generated `PanelLayout` (an engine's face, else
  one control per param / setting); no description files, no selectors, conditions, tabs, choosers or bespoke displays
  (all retired). Alternative (not done): draw a leaf's frame as a synthetic one-element group panel.
- **Opening**: only a group opens (its capsule); "Open to Atoms" of a leaf, the expanded device scenes and "Edit Device
  Circuit" are gone; opening a band opens its closed groups and closes them again with the band.
- **Modules where stock devices were**: `ops::addDevice("module:<key>")` puts a module's group ON the path (the Device
  view's menus, the browser's Audio Effects drop, `addToChain` for rack chains); an instrument module replaces the
  instrument group heading the path (addDevice) — the browser keeps GU.8's rule (at the notes only on an empty path).
  Browser categories: Instruments = instrument modules, Audio Effects = Utility / Saturator / LFO, Meta Modules = the
  rest and the engines (the library index categorises a Presets/<module> folder the same way). Scenes and demo projects
  use VCA / Saturator modules where EQ Eight / Auto Filter were.
- **Bugs fixed on the way** (found by the ported tests): a switched-off group did not switch off what is inside it
  (its blocks were lowered; an instrument group's nested groups still voiced) — the lowering and the voice contexts now
  treat a block inside a switched-off group as off; the store accepted params, foreign settings and meta state on a
  group / value block (now refused as checkList refuses them on load).
- **Left in place without a user** (honest list): `app::DeviceInspector` (ProjectLink's per-unit probe path; its only
  driver, the project-device editor, is gone), `CircuitEditor::Host`, the lowering's lone-stock-instrument branches
  (unreachable: no block is a non-meta instrument), the nested meta-site machinery in the lowering (`metaSites`: always
  empty), the GU.4 retired-type conversion. Candidates for deletion in a later cleanup.
- **Benches**: `pq_bench_device_panels` retired; `pq_bench_inspect` / `pq_bench_silence` still name retired stdlib
  device files at run time (not run; they fail those cases). `tests/engine/idle_project_fixture.h` (pq_tests_project)
  imports the retired `instruments/basic_synth.pqp` and `devices/eq_eight.pqp` since GU.10 (not run here).
- Status: open (provisional choices above).

## Q232 · 2026-11-07 · GU.13 · One panel system for leaves (Q231 follow-up): the choices made
- **Done as Q231's alternative**: a leaf's Device-view frame is a synthetic panel of the group-panel system
  (`panel::leafPanel`: its engine module's `doc::enginePanel`, else knob / toggle / number elements per param and
  setting, the old generic layout's 64-px columns), bound through `makeDocPanelTarget(ctx, leaf)`, in a `panel::BlockFrame`
  (the old frame's chrome kept: jacks, activator, fold, preset slot, rename). A rack's macro panel is a `BlockFrame` with
  its own `PanelTarget` (macros by index; a macro not mapped yet has no block: dragging it does nothing). Alternative: a
  group-frame title bar for leaves too (one chrome) — not done, the leaf keeps Live's device chrome.
- **The device-panel gestures are the host's choice** (`FacePointer::valueGestures`): `PanelHost` (the Device view's
  frames, the panel window) turns them on; the circuit canvas leaves them off so its wheel keeps panning / zooming and a
  press on a closed group's knob does not take Delete away from the block. Alternative: on everywhere.
- **Macro-mapped controls** are handled by the document target for every panel: it shows where the macro puts the
  control and a drag turns the macro (the old ChainTarget's rule, now group knobs too).
- **Partial repaints**: `Element::liveKey` (positions, resolutions, rings of its bindings) — a document change damages
  only the elements whose key changed; engine sections, embeds and displays repaint on every change (kAnyChange).
- **Dropped**: the face's own in/out jack dots on an engine's leaf frame (the Device view's sockets on the frame edges
  stay); a bool param is no cable drop / Map target on a group panel either (as on the old leaf panels). Rack macros
  get no ring and are no cable targets.
- Status: open (provisional choices above).

## Q233 · 2026-11-07 · GU.f1 · Rack roles: a role setting, not wiring (supersedes Q228 "Roles by name")
- **Chosen:** every block a rack is made of carries the setting `rackRole` (`doc::RackRole`: Mixer, Volume, Pan, Gate,
  Key / Velocity / Select Zone, zone Lo / Fade Lo / Fade Hi / Hi / Value, Selector), written by `chainBlocks`,
  `zoneBlocks` and the selector builder. `findRacks` and the rack edits find them by role only; names are labels. A
  rename keeps the rack (a one-chain rack is one because it holds its role-Mixer group); a user group called "Mixer"
  is a plain group (no rack, or an ordinary chain device). Copies carry the role (Duplicate Chain, paste).
- **Why not from wiring** (the user's preference, considered first): a chain mixer and a zone are gain stages driven by
  value blocks, structurally the same as any user group (Utility is one). Telling them apart from wires would mean
  matching each one's exact atom graph (Volume = the `mul` "a" input …), a per-type rule that breaks on any hand edit;
  the role is one general mechanism (as `macro` already was for macros). The one-chain rack still needs a mark of some
  kind: two parallel groups are a rack by wiring alone, one group is not.
- **Open:** a role block edited by hand keeps its role (e.g. a Mixer group whose Volume block is deleted: the row has no
  volume). Alternative: drop roles whose structure no longer matches. Not done (no known need).
- Status: open (provisional).

## Q234 · 2026-11-07 · GU.f4 / GU.f5 · Structural value blocks and voice monitors: the choices made
- **Which value blocks lower as literals** (lower.h "Structural value blocks", `lowersAsLiteral`): a value block whose
  curve is stepped (step > 0) or constant (min == max: the presets' numbers, e.g. the `1` in `type − 1` of a menu's mux
  chain), with no wire into it and no automation lane on it, whose output reaches a `mux` / `demux` `index` or a `select`
  `cond` through atom blocks only (not through group terminals or engine blocks). Rejected: every stepped block (a
  rack's chain Gate / selector would then recompile on each change for no CPU win) and a new "switch" block type (Q230's
  first proposal: the presets would need a second kind of control; the rule needs none). Reversible: one predicate.
- **A literal's change recompiles**: SetDeviceParam on it (and a curve edit that makes or unmakes one) is
  kChangeStructural, so the link compiles that unit (debounced 50 ms) and the swap fades old → new over ≈ 11 ms (the
  recompile test compares after 2048 samples). A menu scrubbed by drag compiles once per debounce. If that latency is
  unwanted on some control, wiring anything into it (or a lane) keeps it a runtime choice with every branch compiled.
- **Muted rack chains (GU-critique CPU "med")**: NOT folded by this mechanism. The chain mixer's Gate (stepped 0 / 1)
  multiplies the volume (`mul` → `pack` → `gain`), it reaches no choice; and even as a literal 0, `vol · 0` and the
  `gain` of the chain's audio by 0 are not value-exact folds (x · 0 is folded only for x finite with a clear sign bit;
  a param or audio signal is neither), so the chain's devices would still run. Options: (a) the chain mixer selects
  (`select(gate, y, 0)`) and the optimiser treats a literal-false select as dead upstream (already the case) — then Gate
  would be a choice and fold; (b) the lowering leaves a muted chain out like a switched-off group (T4.8's rule). (b) is
  closer to what T4.8 did; (a) is the general mechanism but changes the chain mixer's graph. Provisional: neither (out of
  GU.f4's scope); recommend (a) as a small follow-up.
- **Voice monitors (GU.f5)**: composeUnit skips every port listed in ProjectGraph::taps (analysis taps) when it adds the
  `vmon_` voice monitors, so a voice ends by its `done`, else its monitored audio outputs, else the release timeout —
  never by a silent modulation tap. Operator's FM into the PA's `+ pm` atom (Q230's workaround) can now be a plain
  per-voice cable into a control; left as it is (no preset change here).
- Status: open (provisional choices above).

## Q235 · 2026-11-07 · GU.f7 · Dead-code removal, muted chains, Operator FM: the choices made
- **Deleted** (no compatibility kept): the lone-stock-instrument voice contexts (`VoiceContext::group` is gone: every
  context is an instrument group) and every lowering branch for them (`playInstrument`, `kSilent`, the notes-cable /
  In-terminal cases into a stock instrument, stock-instrument enders and `voices` polyphony, `devicePolyphony`,
  `Cables::notesTo` / `fedNotes`); the stock-instrument cases of `placeOnPath` (its `notes` flag), paste and
  `pasteBlocks`; the nested meta-site machinery (`MetaSite`, `metaSites`, `callsMeta`, `metaParamFlatName`,
  `Callees::metaSites`); `app::DeviceInspector` with `ProjectLink::setDeviceInspect` and the engine's `TrackInspect` /
  `UnitInspect` (nothing else compiled probes into a project unit); `CircuitEditor::Host` with its Track button,
  `closeSoon`, `refreshTitle`, `showStatus`; the GU.4 load conversion of `meta:phase_acc` / `meta:vca` (projects and
  presets) and `pq/meta/retired.*` (such a file is refused: unknown meta module type). The PA's `pm` option / `Feed::Pm`
  went with it (its only users were the conversion and Operator).
- **Fixtures**: the six `gu4_modules` projects, `instrument_groups/group_notes` and `views/fixtures/meta-cables` held
  retired devices; they were loaded with the conversion once and re-saved (same ids, same sound: the GU.4 render
  references still match bit for bit / ≤ −110 dB). The conversion-only test cases were deleted.
- **Kept on purpose**: `checks.cpp`'s "instruments go on MIDI tracks" (a meta device may still be stored with kind
  Instrument, so it is live); `calleeIsInstrument` in `lowerList` (an atom block whose patch reads as an instrument is
  "missing", as before). Not touched (outside this task, but compatibility code by the same rule): the schema migration
  chain in serialize.cpp (Travel / Units wire laws, 16 → 17 chain wiring, 18 → 19 `migrateToInstrumentGroups`, 19 → 20
  knob positions) and the views' remaining stock-instrument checks (`device_chain.cpp`, `rack_frame.cpp`,
  `node_menu_edits.cpp`, `edit_racks.cpp`, `edit_groups.cpp`). Option: one follow-up that raises the minimum schema to
  21 and deletes all of it; `DeviceKind::Instrument` could then be derived (GU-critique).
- **Muted chains (Q234 option a)**: the chain mixer's Out is `mux(Gate, pack(0, 0), gain(In, pack(gl, gr)))`; the
  undriven Gate now lowers as a literal (GU.f4's rule), so a muted chain folds to silence and nothing of it is compiled
  (test: GU.f7 in test_stock_presets.cpp). Unmuted the program is the same (mux index 1 folds to its input; volume ×
  literal 1). Racks saved before keep their old mixer (no conversion). Mute / solo / activator now recompile the unit
  (debounced 50 ms) and switch by the swap's ≈ 11 ms crossfade instead of the gain's 2 ms ramp — click-free. A Gate a
  user wires or automates stays a runtime choice: it then cuts at once (a click on mute; on unmute the gain ramps up
  from 0). Alternative: lower a muted chain out like a switched-off group (option b).
- **Operator FM**: each modulator's Envelope is a plain per-voice cable into the next row's Phase value block (GU.f5:
  a modulation tap no longer ends a voice); the `+ pm` atoms are gone. Phase's curve wraps, so phase + env wraps before
  the phasor's add and `frac`: the same sound up to float rounding of the wrap (identical while B … D are at level 0,
  the default). Golden `project-circuit-device` updated (the operator rows lost their `+ pm` atoms).
- Status: open (provisional choices above).

## Q?-EQ.1-a · svf atom: shelves' matched numerator, the deep-cascade precision, the stdlib name
- **Shelves (matched)**: Vicanek 2016 covers lowpass / highpass / bandpass / peaking only. Chosen: a shelf section is
  designed as the variant with its poles at fc/√A (low-shelf boost, high-shelf cut; the others inverted, poles ↔
  zeros), poles impulse-invariant, |B|² of Vicanek eq. 25 fitted by weighted least squares on 16 log points up to
  Nyquist with B0 exact (DC gain). Worst over 0 … 20 kHz at 48 kHz vs the analog shelf: ≤ 0.6 dB to 15 kHz for Q ≤ 2
  (a 3-point fit DC / fc / Nyquist was up to 1.7 dB, 6 dB at Q 5). Alternatives: a 5-point fit that also moves the
  poles (better fit, poles no longer the analog ones), or bilinear shelves. Cuts of bells are likewise the inverted
  boost (0.38 dB worst at 15 kHz vs 0.99 dB direct).
- **Precision**: a 64-section Butterworth in float is noise near its cutoff (rounding amplified by the high-Q sections
  after it, ~1e9). Chosen: sections interleaved low / high Q (partial gains 1e4 instead of 1e9) and ≥ 17 sections run
  in double (≈ 2× the float cost at 8 lanes; per-section cost ≈ unchanged at 1 lane). Alternative: float everywhere
  and a looser response check at high stage counts.
- **Name**: an atom named `svf` collides with stdlib/filters/svf.pqp's `patch svf` (a patch may not shadow an atom).
  Chosen: the patch is renamed `svf_multi` (every response of one core as an output; the circuit goldens show it).
  Alternative: delete it (the atom covers its responses one mode at a time).
- Status: open (provisional choices above).

## Q?-EQ.3-a · EQ modules: const inputs from value blocks, the EQ panel's Mode menu, PanelTarget::run's shape
- **Const inputs**: an atom's `real const` input could only take a setting, so a band's Type / Stages menu could not
  reach the svf's `mode` / `stages`. Chosen: a stepped / constant value block wired straight into a const input sets it
  to its knob value (a literal at the call); such a block is structural even when driven, and a wire or automation lane
  INTO it is ignored by the const input (it still drives the block's output elsewhere). Alternative: refuse those wires
  (a WireRefusal reason + the views' tooltip) — more code, same sound.
- **Widths**: the EQ's matrix needs A's input to be the stereo In in Stereo and a mono channel in L/R / M/S; the
  optimiser's fold of a typed mux cannot change width (it kept A stereo in L/R: 2× its cost, and pack(A, B) three
  channels). Chosen: a mux block with a literal index is resolved in the lowering (its wires start at the picked
  source). This applies to every module (Saturator / LFO / Utility switches): same programs after optimisation.
- **Mode menu**: the EQ panel's Mode menu is a plain menu on the Mode block, so switching there does NOT copy chain A into
  an empty chain B (only `ops::setEqMode` does). Chosen: leave it to EQ.4, whose elements call setEqMode through
  `PanelTarget::run`. Alternative now: a dedicated element kind for the Mode menu.
- **run()**: `PanelTarget::run(EditBuilder)` takes a builder `(const Project&, IdSource&) → EditResult` rather than a
  ready Edit: an element has no document or id source to build one with. The doc target runs it on the current
  document and dispatches through the store (a refusal: a notice, false).
- **Strip relayout**: the ops rebuild only the elements tagged `eq: strip` (the user's own additions to the EQ panel
  stay; edits to the strip columns themselves are replaced on the next band op).

## Q?-EQ.1b-a · svf ramp segments: the error budget, the curved audio-rate case, the 64-section state size
- **Ramps**: coefficients follow the quadratic through designs now, half a segment and a segment ahead on the inputs'
  line (≤ 32 frames, the relative step of tan(π·fc/sr), q, gain ≤ 0.1 per segment). Measured against a design per
  frame on 20 ms sweeps (20 Hz ↔ 20 kHz, q 0.5 → 10, ±18 dB gain, all three at once; every mode, 1 and 8 stages): worst
  −93.8 dB, no frame off by more than 1e−4 of the signal's peak (test: ≤ −90 dB). Linear interpolation needed a step
  bound of 0.007 for −80 dB (≈ 7× the designs); a segment through 0 dB (where a bell / shelf is the identity, a form
  without recursion) cost −66 dB, so segments stop a frame short of it and start only off it. Alternatives: a tighter
  step bound (more designs) or linear interpolation over shorter segments.
- **Curved audio-rate inputs** (an LFO or audio wire into cutoff) still design per frame, exactly: matched lowpass s1
  ≈ 120 ns/sample, matched low shelf ≈ 530 (the per-frame bookkeeping added 5–15 % there). Not done: a vectorised float
  design across lanes, or segments that predict curvature (a second difference estimated from float inputs is too noisy
  for the 2e−6 line test: segments would break after ~4 frames). Chosen for now: exact per-frame designs; bilinear stays
  the cheaper prewarp for audio-rate sweeps.
- **State size**: sized by the sections (896 B for a 1-section mono filter, was 24.8 KiB), but a section now holds
  22 fields (the 12 forward differences of a ramp), so 64 sections of a voice group are 44.8 KiB (was 24.8).
  Alternative: keep the differences in a per-call scratch (they must survive calls: a ramp spans many blocks).
- Status: open (provisional choices above).

## Q?-EQ.4-a · Response displays: where the lowering comes from, what the total includes, the Mode menu route
- **Lowering** (superseded by EQ.4b, Q?-EQ.4b-a: the app's compile's lowering, shared): the app's current `LoweredProject` lives on ProjectLink's compile path (another thread). The displays
  instead lower the document themselves on the UI thread, once per structural revision, when a display first asks
  (`views::GroupResponses`; 0.9 ms here, the whole frame after a structural edit on a 32-band EQ ≈ 13 ms in check).
  Alternative: hand the compile path's lowering to the UI thread (a shared_ptr swap) — no second lowering, but the
  curve would lag the compile. Chosen: UI-thread lowering (reversible: one function).
- **Total**: the EQ's curve is the ACTIVE CHAIN's response (the chain group, or the product of its bands when they are a
  pure series), without the Output gain and the M/S matrix (in M/S the curve is the Mid or Side filter, not L → L).
  Alternative: the EQ group's In → Out (includes Output; in M/S a mix of both chains). Chosen: the chain (what the user
  edits; Live's L/R / M/S display does the same).
- **Nodes of gainless types** sit on their own curve at their frequency (a cut at −3 dB), the gain types at their gain.
- **Mode menu** (Q?-EQ.3-a): the doc target routes any edit of a value block with `eqRole` Mode through `ops::setEqMode`
  (the panel's plain menu, or any control a user binds to it), instead of a dedicated element kind.
- **Band highlight**: a strip column's plate is a `shape` with a Panel binding on its band (lit while selected);
  `embeddedGroup` now counts Panel bindings on kind `panel` only.
- Status: open (provisional choices above).

## Q?-EQ.4b-a · EQ review fixes: the curve while the compile catches up, labels, the band panel
- **While a structural edit's compile is pending** (the link debounces, then lowers): the groups prepared before keep
  their responses (their knobs still move them), a band new in the edit passes through until its compile arrives.
  Alternatives: freeze the old curve entirely; show "no curve". Chosen: pass-through (the curve stays live, the new
  band appears one compile later). A failed compile keeps the previous structure's curves.
- **Stages label**: "×16" (prefix ×, integer) over "16 st" (shorter in a 56-unit column). Q: "Q 0.71", two decimals at
  any Q ("Q 40.00"). Both through the `number` element's new style fields, so no other panel changes.
- **Phase default**: wrapped ±180°, fixed scale; the WRAP / UNWRAP choice and PHASE / DELAY are separate view state
  (back from DELAY returns to the phase view last used).
- **Band panel**: 240 × 228 (response 100, phase 36, then the column laid flat); its display has no double-click add
  (a band is not an EQ).
- Status: open (provisional choices above).

## Q?-AU.voices-a · The click at Play: the engine renders it clean; the device restart after the idle suspend is left
- **Measured** (tests/app/test_voice_smooth.cpp, the real ProjectLink → VoiceEngine path, 4th-difference click detector,
  bound ≈ a −70 dBFS step): Play from a stopped, an asleep and a new engine, restart / stop + play / loop wraps, a
  structural edit or a new track mid-note (bit-identical to no edit), > 8 voices with compaction, interp and JIT, device
  blocks 64 / 480: no click; voices end within a block of the Envelope's done. No engine cause was found.
- **Not measurable here**: AudioOutput stops the stream once the engine has slept for kSuspendAfterSeconds (1 s), so
  every Play after a short idle restarts the device (the user's FL Studio ASIO, 1024 frames). A driver / hardware start
  transient or a late first switch would be a click "near the beginning", "sometimes" (only after ≥ 1.25 s idle).
  Diagnostic for the user: does it click on Play right after Stop (< 1 s, stream still running)? If never, it is this.
- Options: (A) keep the stream running while a project window is open (asleep engine: a memset per buffer);
  (B) suspend after a long idle (e.g. 60 s); (C) keep 1 s. Chosen for now: (C), unchanged (no evidence yet; A/B are a
  one-constant change). Done: the ASIO halves are zeroed at every start (they kept the last run's buffers).
- **Resolution (AU.stream, 2026-11-08)**: it was the driver restart. Measured on this machine (FL Studio ASIO → the
  default endpoint, Focusrite USB, 48 kHz, 1024 frames): (1) the ASIO layer is clean at every start: no asioMessage at
  all, sample positions +1024 from the first switch, switches on FL's 10 ms grid (20 / 30 ms, mean 21.3 ms), the first
  ≈ 50 ms after start(); idle length (0.3, 1.5, 2, 10 s) makes no difference. (2) The endpoint's loopback with a
  -90 dBFS tone (tests/engine/test_asio.cpp "asio loopback probe") shows FL Studio ASIO dipping its own output at a
  fixed place of every stream: 125.3-130.7 ms into the sound (output samples ≈ 6016-6272, ≈ 220 ms after start()),
  amplitude 75 % then 50 % in two 2.7 ms windows, 8 of 8 starts without a pre-roll, first start included; our own
  WASAPI shared backend on the same endpoint: 0 anomalies in 4 × 8 s. With 250-500 ms of silence first the dip is gone
  (9 of 9 through AudioOutput). (3) Separately FL Studio ASIO drops 5-10 ms (zeros, ±2-7 samples of phase) every few
  seconds in steady state, start or not (WASAPI: none): Q?-AU.stream-a. Done: (A) chosen, plus a long background
  suspend: the stream stops only after 300 s of the engine asleep with the window unfocused / minimised; every driver
  start pre-rolls ≥ max(latency + 4 buffers, 0.25 s) of silence with the engine offline (Play waits), ≤ 1 s; an ASIO
  reset while sounding fades out and drains first, then pre-rolls (ARCHITECTURE "AudioOutput", "Stream trace").
- Status: resolved (AU.stream).

## Q?-AU.stream-a · FL Studio ASIO drops 5-10 ms every few seconds (not a start effect)
- **Measured** (the AU.stream loopback probe, 8 s captures, a -90 dBFS 1500 Hz tone): FL Studio ASIO → Focusrite USB
  at 1024 frames: 1-3 dropouts per 8 s (2.7-8 ms of zeros, then the tone 2-7 samples off), at random times; our
  WASAPI shared backend on the same endpoint: none. Our ASIO callbacks were on time throughout (the stream trace: no
  late switch, no position step), so it is inside the driver (its FIFO / resampler against the endpoint's clock).
- **Options**: (1) use the Focusrite USB ASIO driver (registered on this machine; native, low latency); (2) WASAPI
  exclusive or shared in pqdaw (no wrapper); (3) keep FL Studio ASIO (its buffer is set only in its own panel:
  Preferences → Audio → Hardware Setup; it offers no other size, so the app's 256 request gets 1024, 68.6 ms out).
- **Choice for now**: none in code (a driver choice is the user's); suggested (1), else (2).
- Status: open (needs the user).

## Q?-AU.fold-a · Saturator (and the shaper / tanh / clip atoms): aliasing, no oversampling
- **Measured** (AU.fold, offline renders: the default instrument's sine into the Saturator, Drive +18 dB, non-harmonic
  energy re the total / largest non-harmonic bin below 20 kHz): at 440 Hz Digital Clip −55 dB / −64 dBFS, Bass Shaper
  −74 / −83, Analog Clip −83 / −92, the rest ≤ −93 / −97; at 1760 Hz Digital Clip −38 / −43, Bass Shaper −42 / −45,
  Hard Curve −51 / −56, Analog Clip −53 / −57, Soft Sine −55 / −63, Medium (tanh) −71 / −80, Sinoid Fold −93 / −98 (at
  this drive its sin(u) is gentle; more drive aliases it too). Since GU.10 the curves are plain per-sample atoms: no
  oversampling, no ADAA. The same holds for any patch using `shaper`, `tanh`, `fast_tanh`, `clip`, `sine` as a folder
  outside an `oversample(n)` region.
- **Options**: (1) the Saturator's curves inside an `oversample(n)` region by default (TMM.6's converters; recorded cost
  for tanh: ×1 1.6, ×2 8.1, ×4 16.8, ×8 30.7 ns/sample mono, JIT, ship — so ×4 ≈ +15 ns/sample per channel, about ten
  times the plain curve), with an "HQ" setting to turn it off; (2) first-order ADAA per curve (closed-form antiderivatives
  exist for all seven; ≈ 2× the curve, one division, attenuates aliases ≈ 6 dB/oct more but not to −100 dB, and the
  clips' corners need care); (3) leave it, documented. Also: the Type menu is a mux index (TX.smooth never ramps
  indices), so switching Type while playing steps the waveform (a click per switch).
- **Choice for now**: none (AU.fold did not change the Saturator). Suggested: (1) at ×2 by default with an HQ ×4 /
  off setting, the cheapest that gets the clips' aliases below −80 dBFS at 2 kHz (to be measured with the region).
- Status: open (needs the user: CPU vs aliasing for a stock device).

## Q?-AU.fold-b · A phase that jumps (a Phase Accumulator's Amplitude off an integer) aliases in the Wavetable
- **Measured**: Phase Accumulator → Wavetable at 880 Hz, fold 0.3: Amplitude 1 or 2 → non-harmonic −98 / −93 dB
  (clean); Amplitude 1.5 → −35 dB, largest alias −65 dBFS. frac(1.5·φ) jumps once per cycle (a hard-sync-like reset)
  and `wavetable_pos` reads a phase that comes through a cable as it is: its mip level follows the phase increment, but a
  jump in the read phase is a step in the output that nothing band-limits. The live path's own sync (the Wavetable's
  Sync knob) is corrected with band-limited residuals; a jump arriving on the cable is not.
- **Options**: (1) detect phase jumps in `wavetable_pos` (|Δφ| unlike the increment) and correct them with the live
  path's residuals (value + slope jump, Kaiser half-width 4: a 4-sample output delay and a ring in the state of every
  baked read, a compare per sample on the hot path); (2) leave it: Amplitude is a "repeat the cycle n times" control,
  whole numbers are clean, the Sync knob is the band-limited way; snap Amplitude to integers when it feeds a Wavetable,
  or say so in its tooltip; (3) route a cabled phase that can jump to the live path (×16, ≈ 30× the baked read's cost).
- **Choice for now**: none. Suggested: (2) (cheapest, honest); (1) if non-integer amplitudes are wanted as a sound.
- Status: open (needs the user).

## Q?-TX.bool-real-a · A voice bool into a global real input: count the voices or "any voice"?
- **Context**: TX.bool-real converts a bool reaching a real input to 0 / 1 (`gate_to_real`, graph::infer). When the
  bool is per voice and the input global, the implicit voice sum can run before the conversion (bool `voice_sum` =
  any voice, then 0 / 1) or after it (the voices' 0 / 1 summed = how many voices hold it).
- **Options**: (1) convert, then sum (a count); (2) sum, then convert (0 / 1, "any").
- **Choice**: (1): it is what a note-gate cable already does (`voice_sum(gate_to_real(voice.gate))`, doc lowering), and
  "a bool is a 0 / 1 signal, summing wires is normal" (GU.f8). Into a bool input the sum stays "any voice". Also: a
  kind-polymorphic atom (select, mux, z1, pack, eq …) where a bool meets a real is real, its bool inputs converted.
- Status: decided (reversible: the order is two lines in infer.cpp emit).

## Q236 · 2026-11-08 · TX.group-oversample · Nested oversampled groups, members with a region of their own, loops through the edge
- **Context**: TX.group-oversample runs a group's contents in an oversample region; the language's regions do not nest
  (PATCH_LANGUAGE §2.3), and a feedback loop must lie entirely inside or outside one.
- **Options** (nesting): (1) multiply (inner 2× in outer 4× = 8×: a region inside a region, new converters between two
  oversampled rates); (2) one region per outermost oversampled group at the LARGEST factor of its nest (an inner group
  asking for less is covered; one asking for more raises the whole outer group).
- **Choice**: (2): no new converter kind, no extra half-band pair inside the outer region (an inner region crossed
  through the base rate would also cut the outer's above-Nyquist content). A member whose own patch has a region (the
  Wavetable's live path, Shape mode) cannot sit in another: it stays outside and runs at its own factor (its cables to
  the other members cross down and up). A feedback loop through the group's edge (a member's output fed back into the
  group from outside) is refused by the compiler (the track does not compile); not special-cased.
- Status: decided (reversible: `oversampledGroups` / `assignOversampleRegions` in lower.cpp).

## Q237 · 2026-11-08 · TX.group-oversample · Every group's Device-view frame now has an inspector column
- **Context**: the task asked that every group's inspector shows the setting; before, only an instrument group had an
  inspector (POLYPHONY).
- **Choice**: every group frame shows one: 96 px with OVERSAMPLE only, 150 px with POLYPHONY on an instrument group.
  This widens every group in the Device view (20 view goldens moved). Alternative if it reads as clutter: show the
  inspector only while the setting is above 1× and offer it from the frame's title-bar menu instead.
- Status: decided (reversible: group_frame.cpp).

## Q238 · 2026-11-08 · TL.1 · Track lanes: where a lane's height and fold live, the click's start marker while playing, port chips
- **Context**: TL.1 (docs/plans/TL.md 1, 5, 6, 7). A band lane's height is dragged at its bottom edge; a band's circuit
  folds; a click on empty lane space moves where Space plays from; a MIDI lane still offers notes / Hz / gate / velocity.
- **Choices**: (1) the lane height and the fold are view state on the track's `doc::PatchNudge` entry ("laneH",
  "folded"; saved, not undoable) — like a block's scale, not the arrangement's `TrackView::height` (a different view,
  a different height). (2) Stopped, a click on empty lane space sets the insert marker, the start marker and the playhead
  (both views: the shared Editor); playing, only the insert marker, as the arrangement did ("while playing it behaves as
  the arrangement does now"). (3) The note outputs are four small chips over the left end of the lane's top edge
  (notes, Hz, gate, vel); a cable's default tap stays right of them. The old Notes node's pins are gone. (4) No load
  conversion: a file whose layout placed the Notes node simply has none; its wires keep working (default taps).
- Status: decided (reversible: store.h PatchNudge, editing.cpp Editor::press, project_patch.h lanePortChip).

## Q239 · 2026-11-09 · TL.2 · 2026-11-09 · TL.2 · Which sub-lanes a band shows, and what a control press adds
- **Context**: TL.2 (docs/plans/TL.md 8, 9). A band's automation sub-lanes and the arrangement's lanes under a track must
  agree; a press on a control in a band shows its automation.
- **Choices**: (1) The band's sub-lanes ARE the arrangement's per-track shown lanes (`ArrangementAutomation` TrackState:
  shown flag, lane list, heights): no second state; the band toggle = the arrangement's lanes toggle. A return / the master
  may have shown lanes now (only the band view draws them; the arrangement's pinned rows stay A-mode only). (2) A press on
  a control calls `ViewContext::showAutomation`, which the arrangement already handles by ADDING the param's lane to the
  track's shown lanes (T4.18's "Show Automation"): every newly pressed control adds a sub-lane, and the band grows by one
  lane (one re-layout, lowering kept). Alternative (not done): a Live-like "follow" lane that switches to the last touched
  param unless it is automated / pinned, so twiddling many knobs doesn't stack empty lanes — would change T4.18's
  arrangement behaviour too. (3) Param pins don't count as controls (a press there draws a cable). (4) Sub-lane heights
  are the arrangement's lane heights (logical px = world units); the band view zooms them with the canvas.
- Status: decided (reversible: project_patch/lane_edit.cpp showControlAutomation, ArrangementAutomation::showParam).
- **User 2026-11-09:** option 1, one follow lane per track (TL.5).

## Q240 · 2026-11-09 · TL.4 · A click while playing, the Play button while playing, the mixer size's undo
- **Context**: TL.4 (docs/plans/TL.md TL.4 3, 5). Q238 (2) kept a lane click while playing to the insert marker only; the
  user's report ("click bar 4, press Play, it stays behind") and the plan's rule say the click moves where the next Play
  starts. The Play button did nothing while playing. The plan asks the mixer resize to be "saved, one undo step"; a
  block's S-drag scale is view state (saved, no undo step: TX.resize).
- **Choices**: (1) Any plain click on a lane's time (empty, a clip's body, a clip's title / edge without a drag) sets the
  start marker in both views, stopped or playing; stopped it also locates (supersedes Q238 (2)). (2) The Play button
  while playing restarts from the start marker (Live does the same); Space still stops. (3) The mixer face's size is the
  track's `PatchNudge::scale` (a factor on the 2× default), resized exactly as a block (S-drag, S + double-click resets,
  Esc restores): saved, not an undo step, like every block scale. Making view-state sizes undoable would be one change for
  blocks and mixers together.
- Status: decided (reversible: editing.cpp Editor::placeStart, view_context.cpp ViewContext::play, view.cpp setMixerScale).

## Q241 · 2026-11-09 · TL.5 · Pin semantics of the follow lane, and what is saved
- **Context**: TL.5 (Q239 option 1): one follow lane per track; pinning keeps a parameter as a lane. Undefined: what the
  follow lane shows right after its parameter is pinned, what Hide does on each kind, what is saved.
- **Choices**: (1) While the follow lane's parameter is pinned they are one lane (the pinned one; the next press re-points
  the follow lane below it), so a parameter never shows twice (the TL.3 wire, the lane lookups stay one-per-param).
  Alternative: the follow lane jumps to some other parameter on pin — arbitrary. (2) Hide ("−", Hide This Lane) on a
  pinned lane unpins it; on the follow lane hides it until the next press / Show Automation / lanes re-shown; "+" adds a
  pinned lane. (3) Saved = pins of automated lanes (`LaneView::visible`, as before: the first show after loading pins the
  visible automated lanes); the follow lane's parameter and pins of unautomated parameters are session view state, like
  TL.2's shown flag. Saving them would need a per-track entry in `ViewState` (document schema).
- Status: decided (reversible: ArrangementAutomation::lanesFor / pin / unpin / hideLane, automation_edit.cpp).

## Q?-M6-a · 2026-11-10 · M6 plan · The plan's interaction defaults (taken overnight, the user asleep)
- **Context**: docs/plans/M6.md raised 12 detail questions; the user approved the design and was not available.
- **Choices** (recommended option each, all reversible): (1) Keys mode = consecutive degrees from key 60 (.kbm wins; option
  "12-TET nearest" per track). (2) Hz mode source = the key's pitch in the track's tuning (option 12-TET A4 = 440 + bend).
  (3) Bend moves the sounding voice; stored = note-on identity (Keys drops bend, Hz keeps note-on bend); raw bend kept in
  the capture buffer for M7. (4) Unsaved projects record into %LOCALAPPDATA%\pqdaw\Unsaved\<session>, moved on first save.
  (5) One continuous WAV per input per pass. (6) PDC compensates sidechains. (7) Fractional latencies rounded to whole
  samples. (8) Automation and notes shift with a unit's PDC. (9) Metronome on the main pair (cue optional), never in
  exports. (10) New clips stay warp Off; pref "Auto-Warp long samples" (off) uses Beats; recordings Off. (11) Complex Pro =
  Complex + formants + envelope. (12) Computer keyboard: Live's letter rows → consecutive degrees; `M` implemented in M6.
- Status: decided (reversible; see docs/plans/M6.md "Decisions taken").


## Q?-T6.1-a · 2026-11-10 · T6.1 · Details of the recording document model the plan left open
- **Context**: docs/plans/M6.md "### T6.1" names the fields and ops; a few shapes and classes had to be chosen.
- **Choices** (most reversible): (1) `InputKeyMap` = Tuning (the .kbm when the tuning has one, else consecutive degrees
  from key 60) | Consecutive (ignores a .kbm) | TwelveTetNearest | TwelveTetHz, instead of the plan's
  TuningSteps | Kbm | …: "the .kbm wins" needs no separate value, and a user can still ignore one. (2) Hz-mode keys are
  Cents identities resolved exactly as a drawn Cents note (`resolvePitch`): with a main tuning that has a signal they
  follow its *reference* (PitchFollow::Reference), not the plan's static `PitchSource{}` — otherwise the monitored note
  and its recording would differ under reference automation. Static would be a one-line change in input_map.cpp.
  (3) Two commands, `SetTrackAudioInput` (Structural) and `SetTrackMidiInput` (Meta), instead of one `SetTrackInput`.
  `track.in` has no channel input, so the channels are not an IR constant: they travel in `LoweredProject::audioInputs`
  (a channel change recompiles nothing that differs; the IR is shared). (4) Until T6.7 the monitor gate's Auto = the
  arm (the engine refines it with the transport). (5) `SetRecordSettings` and `SetTrackDelay` are Meta until T6.6 gives
  the metronome / track delay engine values. (6) `PromoteTake` swaps the take's clips with the main lane's (the lane
  keeps its id, name and place). (7) Comp crossfade default `ops::kCompCrossfade` = kPpq / 32 (≈ 16 ms at 120 BPM).
  (8) Time edits move take lanes (replaced whole) and the punch region like the loop.
- Status: decided (reversible).

## Q?-T6.2-a · 2026-11-10 · T6.2 · What PDC does not delay yet: clips of a compensated unit, a merged member's own sources
- **Context**: T6.2 delays a unit's input buses, its note events and its automation by A_u. Two sources are not
  shifted: (1) the clip players a compensated unit reads (an audio track that also receives a mixer cable from a late
  track: its clips play on time, its cable input A_u late); (2) inside a merged unit (sidechain), a member's own
  sources (its clips, voices) when its sidechain / cable input arrives later (a latent kick sidechaining a bass: the key
  is late against the bass). Both need per-source delays beside the bus ones.
- **Options**: (A) leave as is (rare: (1) needs a cable into an audio track; (2) a latent sidechain source); (B) delay
  `ProcessContext::clipIn` per compensated unit through the same reader-owned lines (one line per clip slot it reads);
  for (2) shift the member's event slot / clip slot by its track's internal target T(t) and add T(t) to its bus inputs;
  (C) Live-like: delay the whole member track.
- **Choice**: A for now (most reversible; both are additive later). Revisit with T6.6's latency readouts.
- **Resolved by T6.2b**: B. A unit's clip slots are delayed by A_u + T(member) through clip delay lines, its notes by
  A_u + T(voice track), its members' automation by A_u + T(t) (ARCHITECTURE "Plugin-delay compensation"). The bus
  inputs already carry T(t) through compose's `wireDelay`. Live audio input (`track.in`) stays undelayed.

## Q?-T6.3-a · 2026-11-10 · T6.3 · Warp engine I: what the plan left open
- **Context**: docs/plans/M6.md "### T6.3" names the modes and the acceptance; several behaviours had to be chosen.
- **Choices** (most reversible): (1) Beats slices at the grid (`beatsGranulation`, at least 1/128) or, at 0, at the
  sample's transients, always plus every warp marker and the loop's ends; until the analysis exists the clip uses the
  1/16 grid (a failed analysis is not retried). (2) A slice the next one cuts before its natural end (or within the 2 ms
  crossfade) reads on naturally through its fade-out (seamless at the source tempo); one that ends first stops with a
  fade (Off), loops its second half forward with 2 ms declicks (Forward), or back and forth (BackForward); loop regions
  shorter than 4 ms count as Off. (3) Beats "Envelope" < 100 = the slice holds that share of its gap, then fades
  linearly to 0 at the next slice. (4) **Tones is WSOLA, not plain OLA**: plain overlap-add shifts a stationary tone's
  frequency by (phase step per hop) / hop (a 220 Hz sine at 0.5× read 0.7 ¢ off with whole-frame alignment and up to
  tens of Hz without), so each grain is moved (±min(H·r/2, 12 ms)) to continue the one before, to a fraction of a
  frame; Texture stays plain OLA with Flux jitter ±flux %·H·r (seed = clip id). (5) Grains are anchored on the clip's
  body start (hop N/2, Hann); Tones' alignment chains from the voice's first grain, so a locate or a non-gain edit
  restarts the chain (a different, equally valid alignment). (6) Complex / Complex Pro play as Tones until T6.5.
  (7) Streamed samples: a stretch voice keeps half its history behind its reads; Beats claims the next slice's stream
  ahead on a second reader; jumps beyond the history restart the stream (real time may read late frames as silence,
  counted; offline renders are exact). Grains of hundreds of ms at large ratios on streamed (> 3 min) files can hit
  that. (8) The transient job travels on the link's loader queue with a job bit in the levels word, and its cache
  directory by content hash (project_link.h belongs to other lanes in this phase); a ProjectLink member would be cleaner
  once the file is free. (9) Detector: banded log flux, threshold 5 × the median flux above the local mean.
- Status: decided (reversible).
## Q?-T6.4-a · 2026-11-10 · T6.4 · Live input details the plan left open
- **Context**: docs/plans/M6.md "### T6.4" fixes the pieces; these behaviours had to be chosen.
- **Choices** (most reversible): (1) A track listens while Monitor In, or Monitor Auto and armed (T6.1's Auto = arm;
  T6.7 adds the transport). (2) All Ins = every open port and the computer keyboard; "Computer Keyboard" is a port name.
  (3) A port seen for the first time is on (Live's default for a new controller); remembered ports stay listed
  "(not connected)". (4) While any port is open the audio output does not suspend in the background (a MIDI note into a
  backgrounded pqdaw must sound; waking a suspended stream from the MIDI thread would take a lock). (5) The engine
  handles sustain (CC 64: note-offs wait for the pedal), All Notes Off / All Sound Off (CC 123 / 120); other CCs,
  aftertouch and program changes are drained and ignored until M7 / M11. (6) Stamps older than 1 s when drained (rings
  filled while the device was stopped) play only their note-offs. (7) The computer keyboard uses layout keys (an AZERTY
  user's A is A), consecutive degrees 0 … 16 (a 12-TET key map: semitones), equave offset −5 … +5, velocity steps of 20;
  Shift chords pass through; releases are taken before a focused text field (no stuck note). It is a tree key listener
  (`ui::KeyListener`), not the plan's modal scope: scopes never see key releases. (8) The strip's input
  section (MIDI From, channel, Keys / Hz with the key map, Monitor) is in the mixer view only; the arrangement header
  keeps its two lines. (9) `LiveNoteRecord::songSample` is block-accurate across a loop wrap (engineTime is exact);
  T6.7 maps engine time to song time itself.
- **Open (not chosen, needs the user)**: pitch bend in Keys mode moves the voice through `voice.hz`; a tuning node whose
  params are being modulated (generator automation running, a live tuning edit not yet recompiled) computes a Degree
  note's Hz from its degree and ignores `voice.hz`, so the bend is not heard on such notes then (Hz-mode notes and
  unmodulated tunings bend). Options: (A) leave it (bends during tuning automation are rare); (B) `tuning_hz` multiplies
  its result by voice.hz / voice.noteHz (one multiply per lane per sample; a bend becomes a ratio on top of the morph);
  (C) a separate per-voice bend signal. Recommend B with T6.7 or M7's pitch curves.
- Status: decided (1)–(9) (reversible); the bend question open.

## Q?-T6.5-a · 2026-11-10 · T6.5 · Complex / Complex Pro: what the plan left open
- **Context**: docs/plans/M6.md "### T6.5" names signalsmith-stretch, the pool, priming and the acceptance; several
  behaviours had to be chosen.
- **Choices** (most reversible): (1) **The vocoder only stretches time; the pitch is a sinc resampling of its output**
  (exact: 1e-5 ¢ measured). signalsmith's own transpose estimates each spectral peak's frequency and was 2 ¢ off at +3
  steps of 31-EDO, 8 ¢ at +7 st, which fails the ±1 ¢ rule. Cost: the vocoder runs r× as many frames (≈ +28 % at +3 st,
  2× at +12 st). (2) presetDefault with **split computation** (one interval more latency, primed away): each spectrum's
  work spreads over its interval, so voices started together do not spike in one block. (3) Complex Pro: the vocoder
  shifts the formants by pitchRatio^(−Formants %) (the resampling brings them back); Envelope = the formant analysis'
  fundamental 1 / order cycles per sample (128 → 375 Hz at 48 kHz). (4) Priming: an output latency + a block + the
  resampler's reach (≈ 10k vocoder frames at 48 kHz). A clip reached by playback primes in step with the song while its
  voice is pending (no burst); a voice started inside a clip (Play / locate mid-clip) primes at once, ≈ 3 ms of one core
  per voice in that block (release build estimate from the bench) — several Complex clips under a locate can overrun a
  small device buffer. Options if it matters: spread a mid-clip prime over the first blocks with the voice silent
  meanwhile, or prime only the output latency (signalsmith's `outputSeek` trick). (5) Stream claims: the voice's first
  read is computed exactly (`complexStart`); a loop wrap on a streamed sample restarts its stream (counted, like Tones /
  Texture). (6) Pool: 32 stereo stretchers per clip player ≈ 24 MiB (estimated from the preset's buffer sizes) + 2 MiB
  output buffers, 16 ms to build; mono sources run through a stereo stretcher (2× the needed work). (7) Drum onsets under
  Complex are within ±2.5 ms of the warped grid (2.2 ms measured at 90 BPM; the 120 ms windows smear attacks); the
  ±1 ms criterion stays Beats'. (8) No local patch of the library: the one bounded growth in `process()`
  (`peaks.reserve(bands / 2)`, at most one peak per two bands) cannot overflow, as signalsmith-linear's FFT sizes make
  the band count a multiple of 16; the spike saw no allocation.

## Q?-T6.6-a · 2026-11-10 · T6.6 · Metronome / count-in / Track Delay details the plan left open
- **Context**: docs/plans/M6.md "### T6.6"; these behaviours had to be chosen.
- **Choices** (most reversible): (1) The app counts in on Record only (Live); the engine also honours
  `countInBars` on Play. (2) Count-in length = N bars of the signature at the start tick at the tempo there; a count-in
  is reported as playing (Space / Stop cancel it) and the position display reads "-bars. beat. 1". (3) Record while
  playing does nothing until T6.7 (status line says so). (4) The cue routing is a project setting
  (`RecordSettings::metronomeCue`, the metronome menu), played on Preferences' cue pair; devices with < 4 outputs or
  cue pair 1/2 play it on the main pair. (5) Level is chosen from −18 / −12 / −6 / 0 dB in the metronome menu (no
  continuous control yet). (6) Track Delay → plan offset −d: positive delays the track, negative delays everything else
  (`planCompensation` no longer clamps L(b) at 0). A merged unit takes its members' largest-magnitude delay (members
  cannot be shifted separately inside one image). (7) The strip readout is the track's own latency (Track Delay not
  included); the PDC total is in the CPU meter's info text. (8) The click is a precomputed 15 ms table (2 kHz accent,
  1 kHz × 0.7 beat, τ 3 ms, 2 ms fade-out), not a user sound.
- **Open**: a plan change applies one image block after the hook reads `outputLatency()`, so the first click after a
  latency change may be off by the change once.
- Status: decided (reversible).

## Q?-T6.8-a · 2026-11-10 · T6.8 · Warp UI, tempo detection, auto-warp: what the plan left open
- **Context**: docs/plans/M6.md "### T6.8" names the controls, the detector and auto-warp; several behaviours had to be
  chosen.
- **Choices** (most reversible): (1) **Octave choice**: the IOI comb alone ties a tempo with half of it (a back-beat
  repeats every two beats), so a log-normal prior (σ 0.8 octave) is centred on the median spacing of the loud hits
  (onsets ≥ 0.2 of the strongest RMS rise), not on a fixed 120 BPM (fixed centres missed 2–4 of the 20 loops, which
  changed with the jitter seed); 20/20 on the synthetic set over 7 seeds. Real recordings are untested: :2 / *2 next to
  BPM fix an octave error. (2) Auto-warp's detection may arrive after the drop: it is dispatched with the drop's
  coalescing key (one undo step when nothing happened in between, else its own), and skipped when the clip changed
  meanwhile. Tag / name tempo wins over detection. (3) Auto-warp and Warp as N-Bar Loop re-derive the clip's length
  (the whole file; N bars) but never past the next clip on the track. Bars are 4/4 (content time has no signature).
  (4) Warp From Here uses the detected tempo (confidence ≥ 0.6) else the clip's source BPM; with no earlier marker it
  sets the source BPM (the whole clip follows). (5) Marker drags snap to the 1/16 content grid (Shift: free), not to
  transients; double-click snaps a new marker to a transient within 5 px. The display's axis is frozen during a marker
  drag. (6) The waveform of a warped clip is drawn on the content axis with T6.1's law, so before the first / after the
  last marker the sample follows the nearest segment's slope (Live pins the sample's start instead). (7) "Default Warp
  Mode" applies when a clip is warped from Off by auto-warp or Warp as Loop; the chooser itself picks its mode; switching
  from Off seeds the BPM from tag / name, else the detection, else the length. (8) The views run their own analysis job
  (`views::SampleAnalysis`) over the same media caches as the link's Beats job; `project_link.h` was left alone (the
  link keeps its `kTransientJob` bit): on a cold cache both may analyse one file once each.
- Status: decided (reversible).

## Q?-TU.1-a · 2026-11-10 · TU.1 · Tuning menu rework: what was chosen
- **Context**: user 2026-11-09 — no giant temperament dropdown; adjusting a tuning must retune sounding notes during
  the drag, not on release; the MOS colours clash with the DAW.
- **Choices** (most reversible): (1) **The wheel applies to the document as you go** (no draft / Apply / Cancel): a
  chart or circle drag is one store gesture (one undo step per drag; Esc mid-drag restores where it started); a click,
  typed EDO, Scala file or temperament is one command; footer **Revert** (back to the tuning it had when it opened, one
  step) and **Done**; Esc / Enter / a press outside close and keep. T3.14's view-only tuning preview
  (`ViewContext::setTuningPreview`, `kTuningPreview`) is deleted; the clip box's Generator box is live the same way.
  (2) **Targets keep their meaning**: the clip chip edits that clip (a private tuning), a track chip the track, a main
  tuning (panel, Ctrl+Shift+W without a clip) the main tuning. For a main tuning a generator-only change is
  `SetTuningParam` (a param write: no compile, automation included); a region hint is kept unless the generator sits
  at an end of the new region (only there does it change a pitch; that step is a `SetTuningDef` → a new table). A
  private (clip / track) tuning has no signal node: each step is a `SetClipTuning` / `SetTrackTuning` (Structural, but
  the patch is unchanged, so no unit compiles; sounding drawn notes retune through the timeline's sequence swap).
  Option not taken: retarget the clip chip to the main tuning the clip follows (would make every clip edit global).
  (3) **Temperaments**: a search field ("Temperament…": exact name → chosen; one match → chosen; several → a menu of
  ≤ 12, exact / prefix / substring order) instead of the 80-item dropdown; the chart's temperament rows still show
  them at the generator. (4) **Discrete edits during a drag** (EDO Steps box, a MOS size change): applied at each whole
  step inside one gesture (a new table each: a compile, debounced by the link); held drawn notes retune by the
  timeline's sequence swap, held live (MIDI / computer keyboard) notes by `LiveInput` moving each held key to the new
  route image's pitch (keeping its bend). (5) **Colours**: region bands and temperament ranges take the middle row of
  the theme's clip palette (Live's muted clip colours: `mosDataColour`); the circle's step slices use the piano roll's
  scale-key colours (`roll.keyLarge / keySmall / keyExtra`, the roll's own `stepClassColours`), so a MOS reads the same
  in both; `mosChart.*` / `mosCircle.*` tokens retuned to the amber accent, solo blue and text tones; the hardness
  slider is a Live slider (`control.value` fill, `control.thumb`).
- **Known limit (not chosen, small)**: a MOS shape change (structural `SetTuningDef` on a main tuning) writes the new
  generator param at once but the new table arrives with the compile (~50 ms debounce + compile): for that window a
  sounding Degree note is computed from the old table at the new generator (a short wrong pitch). Options: hold the
  tuning params until the compile publishes, or publish the table with the params. EDO size changes are not affected
  (the period param does not change).
- **T6.4b (Q?-T6.4-a option B)**: already the node's behaviour — `tuning_hz` multiplies by E = hz / hz₀ on every path
  (constant-per-block and per-sample), and a bent Keys note's voice.hz is its static Hz × the bend, so E is the bend
  ratio. T6.4's "the bend is lost" was not reproduced; tests now pin it (playback: `a bent Keys note keeps its bend under
  generator automation`, `tuning live: a panel Generator drag …`). No kernel change.
- Status: decided (reversible).

## Q?-T6.7-a · 2026-11-10 · T6.7 · Recording details the plan left open
- **Context**: docs/plans/M6.md "### T6.7" fixes capture, writer, commit and the acceptance; these behaviours had to be
  chosen.
- **Choices** (most reversible): (1) Every pass makes a take lane, a single pass too, and the last pass is also copied
  into the main lane (`ops::commitRecording`); ONE undo step per session (Record → Stop / Record again / punch-out), not
  per loop pass. (2) Latency at loop wraps: a take covers what was PLAYED (its captured range − the record latency), but
  where the file runs on across a wrap the boundary stays on the loop point and that take's file frames start `offset`
  later, so loop takes are contiguous regions of one file and each covers the loop exactly; the first take loses the
  `offset` frames captured before the start, the last take ends `offset` before the last captured frame. (3) The live
  waveform's min / max per 256 frames is computed by the writer on the disk thread (the plan put it on the audio
  thread): the audio thread does one copy per channel and nothing else. (4) One WAV per armed audio track (two tracks
  on the same input write two files). (5) A punch-out without a loop ends the session by itself (RecordOff once the
  playhead passed punch-out + latency; playback goes on) and commits it then. (6) Record while playing records from
  the playhead (no count-in); Record while recording = RecordOff. (7) Notes: a note belongs to the pass of its note-on;
  one up to a 32nd before its pass start snaps onto it, earlier ones are dropped; a note held at the end ends with its
  pass. (8) Overdub with no clip under a note: a new clip spanning the recorded range inside that gap of the main lane.
  (9) Monitor Auto's "no clip under the playhead" = no clip of the track sounding in that block (`ClipPlayer::active`).
  (10) Inputs: the device opens the input count the audio preferences set (ASIO only; WASAPI capture is not built), not
  the plan's "max used". (11) The writer runs as the sample pool's disk client (`SamplePool::setDiskClient`), not a
  queue of its own.
- **Open (not built)**: deleting an untitled project's recordings folder when the project is discarded (the files stay in
  `%LOCALAPPDATA%\pqdaw\Unsaved\<pid-time>`; the first save moves them into the project); a "Record latency offset"
  field in Preferences (the engine takes `Recorder::setManualOffset`, nothing sets it yet).
- Status: decided (1)–(11) (reversible); the two open items for the orchestrator. (2) and (11) superseded by
  Q?-T6.x-fix-rec-a (frames placed on what was played; the writer's own thread).

## Q?-T6.5b-a · 2026-11-10 · T6.5b · Complex priming without a burst: what was chosen
- **Context**: Q?-T6.5-a (4): a voice started inside a Complex clip primed ≈ 10k vocoder frames in one block (measured
  3.3 ms per voice in release, 13.3 ms for 4). docs/TASKS.md T6.5b left the approach open.
- **Options**: (a) prime off the audio thread and hand the stretcher over (needs a worker with stream access and a
  cross-thread pool; still late on a locate while playing); (b) spread the priming over blocks, the voice silent
  meanwhile; (c) signalsmith's `outputSeek` (less work, still a burst, not the same audio).
- **Choice** (most reversible): (b) on the audio thread with cueing so the common cases are not late: priming runs at
  most 4× the playing rate on a song-time schedule; the timeline cues the playhead while stopped / counting in and
  the loop start before a wrap, so Play and loop wraps start primed and on time. Uncued starts (a locate while playing,
  Play within ≈ 50 ms of stopping or locating) sound ≈ 70 ms late (pitch rate 1), faded in, then exactly as an on-time
  start would. Offline renders prime at once. Mono sources get mono stretchers: `stretchMonoVoices` 32 by default
  (≈ +half the stereo pool's memory, see the T6.5b test's message); the rate (`kStretchPrimeRate` 4) and the pool
  split are constants to revisit if the CPU meter shows Play spikes or memory matters.
- **Update (T6.x-fix-warp, M6 review L14 / L15 / H2)**: the late start was ≈ 70 ms / pitch ratio (145 ms at 0.5); the
  priming rate is now kStretchPrimeRate × max(1, ratio), so it is ≈ 60–80 ms below ratio 1, ≈ 40 ms at 2. The pool is
  sized from the project's Complex clips (no stretcher without them): Q?-T6.x-fix-warp-a (1).

## Q?-T6.10-a · 2026-11-10 · T6.10 · Take lanes and comping: what the plan left open
- **Context**: docs/plans/M6.md "### T6.10" names the gestures; their exact rules and the look had to be chosen.
- **Choices** (most reversible): (1) Take lanes are shown by default (a recording's takes appear at once, as in Live); the
  track's existing fold triangle hides them, stored in `TrackView::folded` (which only groups used) — no new view-state
  field. (2) The comp highlight is a veil (theme `automation.veil`) over every part of a take the main lane does NOT take
  from it, read from the main lane's `ClipSlot::take`; an auditioned take is never veiled; the main lane draws grey while
  the track auditions a take. Fresh takes added without a comp are therefore all veiled. (3) A click on a take swaps in
  the WHOLE main-lane clip under the click (Live's region = the comp segment); in a gap of the main lane, the take clip
  under the click clipped to the gap; a click on a region the take already supplies only selects it. (4) Up / Down skip
  takes that have nothing in the range (comping silence in is a swipe on an empty part, not a key); with no take that
  way, nothing happens (status line). Breakpoint selections keep the arrow keys first, then the comp selection, then
  clips / tracks. (5) A swipe snaps both ends to the grid (Off: exact ticks); one narrower than a grid cell comps
  nothing. (6) Take lane height is fixed (34 px, `kTakeLaneHeight`); no resize, no rename in the header yet (the
  document has `renameTakeLane`), no promote gesture (`promoteTake` exists) — not asked for by the plan.
- Status: decided (reversible).

## Q?-T6.9-a · 2026-11-10 · T6.9 · Capture MIDI: what the plan left open
- **Context**: docs/plans/M6.md "### T6.9" fixes the buffer, the two modes and the acceptance; these had to be chosen.
- **Choices** (most reversible): (1) The ring is written by the audio thread itself (`engine::CaptureRing` per track,
  referenced by the route image), not drained from the T6.7 record tap on the UI thread: the tap is a 4096-entry ring
  that the UI polls only while it repaints, so a stopped, idle window would lose a long jam. 32768 notes per track
  (= 64k on/off events, ≈ 2.6 MB), allocated when the track first listens, kept while the track exists; a track's notes
  are captured only while it still listens (armed / monitored) when Capture is pressed. (2) A phrase ends at a silence
  of ≥ 2 bars between a note's END and the next note-on, measured at the song tempo at the insert marker. (3) The mode
  follows the transport: playing → only notes played while it ran; stopped → the last phrase, placed at its song ticks
  when it was played wholly while the transport ran, else the tempo path. (4) Stopped: the first note-on is the clip's
  downbeat (a pickup is not detected); length = round((last onset − first onset + 1 beat) / bar), at least 1 bar; a
  note past the loop end stays in the clip (unheard); the inferred tempo is rounded to 0.01 BPM and set (first tempo
  point) only in a project without clips (main or take lanes); with clips the notes keep their real time at the song
  tempo; fewer than 4 onsets: no inference, the song tempo. (5) Playing: the clip spans the phrase's whole bars,
  unlooped, overwriting what lies under it (no overdub merge); loop passes overlap on the same ticks. (6) Every
  listening track is captured in the same undo step; a track's "since the last capture" mark moves even when its notes
  are later undone (a second press does not bring back what an undo removed).
- Status: decided (1)–(6) (reversible).

## Q?-T6.2b-a · 2026-11-10 · T6.2b · One automation lane read by units with different compensation (a tuning's generator)
- **Context**: T6.2b shifts automation lanes per sample, taps included, by their track's S(t) = A_u + T(t). A project
  param that several tracks read (a tuning's generator / period / stretch / reference: one lane, one tap, the same
  TuningLiveInputs for every tuning node using that tuning) can sit in units with different shifts; one lane has one
  shift.
- **Options**: (A) the smallest of the shifts (instruments are leaves, A_u = 0 in nearly every project: their notes
  and tuning stay exact; a compensated unit reading the tuning hears its change up to the difference early); (B) the
  largest; (C) per-shift copies of the tap and of the tuning tables' live inputs (a unit's images bind the copy of its
  shift: a recompile whenever a unit's A_u changes, or a program-side indirection).
- **Choice**: A (most reversible; C only if a compensated tuning consumer turns up in practice).

## Q?-T6.7b-a · 2026-11-10 · T6.7b · Recording leftovers: what was chosen
- **Context**: docs/TASKS.md T6.7b (Q?-T6.7-a's open items + WASAPI capture + inputs by use) left these open.
- **Choices** (most reversible): (1) The manual record offset is in ms (±100, 0.01 steps; Live's unit), on a new
  Preferences tab "Record"; frames = round(ms · rate / 1000), kept across a rate change; positive moves recordings
  earlier. (2) "Discarded" = New or Open replacing an untitled project, or a quit that confirmQuit lets go ahead while
  untitled; a session still running then ends WITHOUT a commit. What goes is the whole run's session folder
  `Unsaved\<pid-time>` (one per process, so only that project's), never `Unsaved` itself or another run's; nothing
  cleans folders of earlier crashed runs. A first save whose move AND copy both failed still references the unsaved
  path: the folder is then KEPT for the rest of the run (`ViewContext::unsavedRecordingsKept`, no discard deletes it;
  chosen over per-project subfolders as the smaller change), so a saved project never loses a file. (3) The Input Channels preference is removed
  (`AudioSettings::inputChannels`, the settings key, the chooser): inputs open by use. Every audio track with an input
  counts, armed or not (arming or a monitor change must not reopen the driver); any change of the count reopens the
  stream (a short gap), held back while a session runs. (4) WASAPI capture is shared mode only (exclusive capture not
  built), polled on the render thread (no capture event); a render endpoint as input is captured in loopback (used by
  the opt-in test); an input endpoint that cannot open leaves the stream output-only with a status message rather than
  losing the output. (5) Alignment: the FIFO's delay is measured, not assumed — the round trip from the capture packets'
  QPC times and the render clock's QPC position, averaged since the FIFO primed, replaces the input-latency estimate
  in the recorder. Loopback on the dev machine: measured 1424 vs observed 1440 frames (0.33 ms); a real microphone
  round trip is only checkable by hand (REVIEW).
- Status: decided (1)–(5) (reversible). (2)'s one folder per run and `unsavedRecordingsKept` superseded by
  Q?-T6.x-fix-rec-a / M6 review C1 (a folder per untitled document, saved references pin it).

## Q?-T6.x-fix-rec-a · 2026-11-10 · T6.x-fix-rec · Recording / input review fixes: what was chosen
- **Context**: docs/reviews/M6.md C1, H1, M4–M6, M8, M9, L1, L2, L11, L16 named the faults and suggested fixes; these
  details were open.
- **Choices** (most reversible): (1) M6: instead of widening the punch window across the wrap, the recorder places
  every captured frame on the song sample PLAYED `offset` earlier (a short history of the session's runs), so any jump
  (loop wrap, locate) is handled and the commit needs no latency arithmetic. Consequences: a take's file no longer
  starts with the `offset` frames captured before the session began (they were played before it; the first take
  started `offset` early or in pre-roll), so files are `offset` frames shorter and clips start at content frame 0; a
  negative total offset (a large negative manual offset) counts as 0; an offset that shrinks mid-session leaves a gap
  written as silence. (2) M8: RF64 (a JUNK chunk reserved at open becomes ds64), not a roll-over to a second file; the
  commit takes each file's frames from `media::probe`. (3) M9: the writer has its own thread (condition variable, 2 ms
  polls while a session runs); `SamplePool::setDiskClient` is deleted. (4) C1: folders are
  `Unsaved\<pid-time>\<n>` (WAVs directly inside); stale folders of crashed runs are still never cleaned (L13). The
  "last saved project" check is every path a save of this context wrote inside `Unsaved` (pinned for the run) plus the
  store's snapshots after a New / Open. (5) L16: `transport.panic` on Ctrl+Shift+Space (Live has none), an
  AllNotesOff (voices release) rather than AllSoundOff (fast fade); the transport keeps playing.
- Status: decided (1)–(5) (reversible).

## Q?-T6.x-fix-warp-a · 2026-11-10 · T6.x-fix-warp · Warp / tempo review fixes: what was chosen
- **Context**: docs/reviews/M6.md C2, H2, H3, M10–M12, L14, L15 (Q?-T6.5-a (2) (4) (6), Q?-T6.5b-a, Q?-T6.8-a (1) (3)).
- **Choices** (most reversible): (1) **Stretch pool** (H2, L15): starts empty and grows to `ClipPlayer::stretchDemand`
  of each published image (per slot with Complex clips of a kind: 2 stretchers, 3 with more than one clip), built in
  `Timeline::publishAudio`, adopted lock-free; capacity 256; never shrinks while the clip player lives. A voice that
  still finds none plays as Tones and retries every block; its replacement primes like a locate (late ≈ 70 ms) and the
  two crossfade over 2 ms (a seam, not a click). The CPU meter's info text names the fallback count when non-zero.
  (2) **Phase spread** (M10): not a phase shift of the analysis (that changes the audio: the Complex drum-onset test
  moved from 2.2 to 3.7 ms), but a per-clip READ-AHEAD (`stretchLead`, golden-ratio sequence over the clip seed, within
  one interval) plus staggered priming starts: same audio, spikes spread. Cost: late starts + 0–10 ms. (3) **Priming
  rate** (L14): kStretchPrimeRate × max(1, pitch rate) vocoder frames per song sample, so a late start no longer grows
  as 1 / ratio (145 → 67 ms at 0.5); below ratio 1 a priming block costs up to 4 vocoder frames per sample (as at 1).
  (4) **Tempo** (H3): the length prior is applied only when some supported candidate makes the file a whole number of
  bars (a loop); octave near-ties lower the confidence (×0.4 at a full tie). Consequence: back-beats / four-on-the-floor
  below 100 BPM with 8th or 16th hats now read < 0.6 (they are onset-for-onset half-time grooves at twice the tempo),
  so auto-warp leaves them to the user; T6.8's labelled test asked ≥ 18 / 20 confident and now asks "confident ≥ 100
  BPM, never confidently wrong". A loop of 16th hats alone below ~100 BPM still reads confidently doubled (no measure
  separated it from a back-beat at double tempo). Options if this trades wrong: drop the octave penalty (back to
  confident half-time errors), or a stronger absolute-tempo prior. (5) **N-Bar Loop** (M11): bars of the signature at
  the clip's start; the menu offers the whole number nearest the detected tempo (no detection: the power of two nearest
  120 BPM), no chooser of N.
- Status: decided (reversible).

## Q?-T6.x-fix-warp-b · 2026-11-10 · T6.x-fix-warp · M13: audio/64 rate 1 is +21 % since M5 — mostly PF.4, not M6
- **Context**: M6 review M13 asked to bisect across the M6 merges and remove the idle per-unit cost. Release A/B of
  `pq_bench_playback --filter "audio/64 rate 1"` (interleaved, each binary with its own stdlib, quiet machine): T5.x-fix
  (M5 closed) 14.0 µs · TL.5 14.5 · PF.3 14.2–14.4 · **PF.4 15.6–16.0** · T6.1 15.6–15.7 · T6.2 15.9–16.8 · T6.6
  16.3–16.5 · T6.7 15.9–16.5 · T6.2b 16.5–16.7 · ecbfb622 16.5–16.8. TSC-instrumented split (ticks per 64-frame
  block, 2.5 GHz): unit loop PF.3 28.1–29.1k → PF.4 32.3–32.9k → now 33.2–34.0k; timeline hook 7.6k → 7.7k → 8.4–8.6k
  (the clip player alone 7.6–7.9k of it); everything else M6 added (recorder, metronome, live input) < 0.1k.
- **Findings**: PF.4 adds ≈ 4k ticks (1.6 µs) in the JIT unit loop although its machine code for these units is
  byte-identical to PF.3's (PQ_JIT_DUMP); the cause is probably where / how the code is placed or shared (one object per
  compile, code released between compiles) — not investigated further (JIT, outside this task). M6 adds ≈ 1.9k ticks
  (0.76 µs): ≈ 1.0k in `runUnit` (≈ 16 ticks per unit of T6.2 / T6.7 bookkeeping; a plain-unit fast path for the idle
  check measured no gain, so `project_program.cpp` is unchanged for T6.x-fix-pdc) and ≈ 0.75k in the clip player
  (≈ 12 ticks per voice: the T6.3 / T6.5 voice state and dispatch).
- **Options**: (a) a PF task: find PF.4's placement cost (compare one object per unit vs per compile, page placement of
  shared code); (b) shrink the clip player's hot voice state (split Voice into hot / cold parts). Target ≤ M5 (13.7 µs
  ship) not met by this task.
- Status: open.

## Q?-T6.x-fix-pdc-a · 2026-11-10 · T6.x-fix-pdc · A grown delay line's new delay starts once its history is copied
- **Context**: M6 review M2: a delay line that outgrows its ring now copies its history over blocks (budget 1024 floats
  per frame shared by every line, ≥ 2 blocks' frames per line), the old line playing meanwhile. The new delay's 10 ms
  fade therefore starts when the copy is in: 63 mono lines of 24 064 frames at block 64 take ≈ 23 blocks (31 ms); 63
  stereo ones (a Track Delay −500 → −1000 ms) ≈ 46 blocks (62 ms).
- **Options**: (A) start the fade at once and read the old tap from the old line while copying (two lines read per
  block, more code on the hot path); (B) defer the fade (chosen); (C) a larger budget (more worst-block work).
- **Choice**: B (the delay of a Track Delay edit is not audible as such; worst block 39–53 µs instead of 1063–1124 µs,
  release, probe D). Grown rings are ≥ 1.5× the old one, so a drag copies a few times, not at every step.

## Q?-T6.x-fix-ui-a · 2026-11-10 · T6.x-fix-ui · M6 UI review fixes: what was chosen
- **Context**: docs/reviews/M6.md L3, L4, L13, L17–L23, M7 named the faults and suggested fixes; these details were open.
- **Choices** (most reversible): (1) L3: held live **Hz-input** notes never retune (raw pitches, like drawn Hz notes and
  their recorded identity); Keys notes still follow every new route image (a Generator drag publishes one per step).
  (2) L4: the app keys a tuning param's project slot per node table (`"tuning:<id>:<name>#t<table>"`), so a MOS
  size + generator change keeps the old table AND the old params until the compiled image swaps in: no third pitch
  (measured: a held drawn note on 7@700 → 5@710 sounded 10 ¢ off the new pitch during the compile before; now its
  sequence swap gives it the new pitch at once). (3) L13: stale
  `Unsaved\<pid-time>` folders (process gone, by pid + creation time) with .wav files are offered at start in a small
  popover: Open as Project (a new untitled project: the takes are MOVED into its own Unsaved folder, one after another
  on an audio track "Recovered", one undo step), Show in Explorer, Discard (the only delete), Later. Empty stale
  folders are left alone. (4) L19: transpose shows "0 st" / "+7 st" for whole semitones, else "386.3 ¢"; Complex Pro's
  Envelope shows "128 bins"; Reverse / Loop are self-named switches without a row label; the waveform's region tint is
  `surface.sunken` 22 % toward the clip colour with the waveform in the clip colour (outside the region veiled); bar
  numbers (warped) / seconds (Off) along the top of the waveform; the 200-px column is the shell's Info View (kept:
  it now says "Point at anything to read what it does." while nothing is hovered, instead of looking empty). (5) L20:
  the auditioned-away main lane keeps its track colours under a 150/255 `surface.background` dim (grey read as missing);
  take names in `text.primary`; the comp veil 1.5× automation's alpha. (6) L21: the mixer view's rows are shared by all
  strips (`MixerController::rows`: the most I/O rows any strip has, the Delay row, a latency row only while any strip's
  latency is non-zero, one send row per return), so sends / pan / volume line up and the master's pan / volume too;
  "Delay" is written in the field (the unit dropped when the strip is too narrow). (7) M7: only the latency freeze (the
  recorder takes the device latencies once per session); marking a discontinuity on a WASAPI drop / underflow needs a
  segment split in the writer and the commit — not small, left open.
- Status: decided (1)–(7) (reversible).

## Q?-T6.x-fix-ui-b-a · 2026-11-10 · T6.x-fix-ui-b · WASAPI capture drift: resample, not split
- **Context**: docs/reviews/M6.md M7: two WASAPI devices drift (±50 ppm normal), so `CaptureFifo` dropped a packet's
  worth or inserted silence every few minutes. Options: split the take at each event (writer + commit change, the
  take still has holes), or track the drift with a fractional resampler (no event at all).
- **Choice**: the resampler, inside `CaptureFifo` (the device passes each period's play time). It holds the measured
  round trip (play − capture time, from QPC stamps: no packet-phase sawtooth) at its value 0.5 s after priming; while
  the filtered error stays ≤ 0.5 ms the FIFO stays a bit-exact copy (one clock, or no timestamps), past it the read
  goes through the High windowed sinc (64 taps) at a PI-loop ratio (4 s, critically damped, ±1000 ppm clamp). Cost:
  ≈ 4 µs per 480-frame stereo pull (check preset) = 0.04 % of a core. The target gains `driftReserve` (64 frames +
  1 ms: 2.3 ms more input latency at 48 kHz) because the packet phase slips a whole packet below the primed fill.
  Beyond ±1000 ppm or a stall beyond the reserve the old drop / underflow remains, still unmarked in the take
  (reversible: a split could be added for that rare case).
- Status: decided (reversible).

## Q?-KB.1-a · 2026-11-10 · KB.1 · Arm on select, the keyboard's layout track, the shift keys
- **Context**: KB.1 asks for Live's arm-on-select so `M` + keys play the selected track, and a layout per tuning.
  Options for arming: (a) a document edit (arm the selected track, disarm the last) — every selection would be an undo
  step and dirty the project; (b) Live's Auto-Arm: a transient "auto-armed" track outside the document.
- **Choice**: (b). While no track is armed by hand, the selected MIDI track is auto-armed (`ViewContext::autoArmTrack`,
  `TransportPort::setAutoArm`): it listens and records as if armed, its arm button is lit; arming any track by hand
  ends it (clicking the lit button arms that track by hand). Groups, audio and return tracks are never auto-armed.
  The track whose tuning lays out the keys: the auto-armed one, else the selected armed one, else the first armed MIDI
  track (document order), else the selected MIDI track. Other armed tracks in other tunings still play, each in its own
  tuning, through that layout's (equave, step).
- **Shift keys**: piano layout keeps Live's Z / X (equave) and C / V (velocity); in the scale layout every letter and the
  number row are notes, so Page Down / Page Up move the equave and End / Home the velocity (both work in both layouts).
  `M` stays the toggle, so the bottom row's M key plays nothing in the scale layout. Keys are matched by character (as
  T6.4), not by physical position.
- **Range**: the engine's keyboard table went from 17 steps × equaves ±5 to 36 steps × ±3 (one key byte) so Continuous
  plays its 35 keys above A in any tuning without wrapping; notes past ±3 equaves play nothing.
- Status: decided (reversible).

## Q?-KB.2-a · 2026-11-10 · KB.2 · What "the clip open" is; recordings into a new clip keep the track's tuning
- **Open clip**: the clip view's clip counts only while the detail view is shown in Clip mode
  (`ViewContext::liveEditedClip`); hidden, or in Device mode, the track falls back to its clip under the playhead /
  start marker. Alternative: the selected clip (`detailClip`) whatever the detail view shows. Chosen: shown only
  (reversible, one line).
- **Recording mismatch (open, not fixed)**: live Keys notes store Degree identities of the live clip's tuning. Record
  (non-overdub) makes NEW clips with an Inherit tuning (the track's); overdub puts notes under no clip into new Inherit
  clips; Capture MIDI likewise. So a take played in a 22-EDO clip's tuning on a 12-EDO track records degrees that
  replay in 12-EDO. Also: with a clip open that is not the one under the playhead, overdub writes into the playhead's
  clip, which may be in another tuning. Options: (a) recorded / captured clips copy the tuning assignment of the live
  clip their notes were played in; (b) record into the open clip; (c) leave it. Suggest (a). Hz-mode notes (Cents
  identities) are unaffected.
- Status: open.

## Q?-TU.2-a · Revert the MOS chart's colours too?
- Context: TU.2 reverted the circle (explorer palette, theme `mosCircle.*`); TU.1 also moved the region chart to the
  clip palette (`mosDataColour`, `mosChart.*` retune). The explorer's chart bands and its circle share one palette, so
  now the circle's L colour no longer matches its region's band in the chart.
- Options: (a) revert the chart as well (`kMosPalette`, `temperamentColor`, pre-TU.1 `mosChart.*`); (b) keep it.
- Chosen: (b) for now (the request named the circle; reverting the chart is one small follow-up).
- Status: resolved by TU.2b: (a) — the chart back to kMosPalette / temperamentColor / pre-TU.1 `mosChart.*`, one palette
  with the circle (mos_palette.h).

## Q?-TX.group-resize-a · 2026-11-10 · TX.group-resize · Where an opened group's scale lives
- **Context**: S-drag on an opened group's frame scales its contents. A group already has a stored scale: its nudge's
  `PatchNudge::scale`, which sizes it when closed (its block / panel, TX.resize).
- **Options**: (1) the same `scale`: closed it sizes the block / panel, opened it scales the contents — one value, no file
  change, a group shrunk while open opens and closes at a matching size; (2) a separate `groupScale` on the nudge (file key,
  strict reader): the closed and opened sizes independent.
- **Choice**: (1), the most reversible (no format change; splitting it later is one new field). Reversible: model.cpp
  `storedScale` / `scaleCapsule`.

## Q?-TU.3-a · The default tuning's name and the "A, B, C" letters
Context: TU.3 asks for tunings "named A, B, C…". New tunings are named "Tuning B", "Tuning C", … (nextTuningName), but
the default project's first tuning keeps its name "12-EDO" (renaming it to "Tuning A" changes the control bar chip in
every shell golden and the minimal fixture). Letters are not shown separately (a positional letter would shift when a
tuning is removed). Options: (a) keep as is; (b) default project tuning named "Tuning A" (re-record every shell golden);
(c) store a letter per tuning and show it as a badge on chips / cards. Choice: (a), the most reversible.
Resolution: user 2026-11-10 (orchestrator, from the request): Tuning A — (b): the default project's tuning is named
"Tuning A"; New Tuning keeps naming the next free letter (TU.2b).

## Q?-TU.2b-a · Narrow chips show only "12" for the default tuning
Context: with the default tuning named "Tuning A", TU.3's chip rule gives the control bar "Tuning A · 12" and the
narrow chips (track headers, mixer strips, compactTuningLabel) just "12" where they read "12-EDO" before. Options:
(a) keep TU.3's rule; (b) narrow chips show the name's letter plus the label ("A · 12"); (c) show "12-EDO" (the
summary) in narrow chips. Choice: (a), no rule change (reversible: TuningChip's narrow text).

## Q242 · 2026-11-12 · DT.1 · The Delay Tap: the time's units, the glide, the sync table, the minimum, gravity zones
Context: docs/plans/DT.md; `pq/document/modules.h` "the Delay Tap", `src/views/src/panel/tempo_slider.cpp`. Choices
(each the most reversible found):
1. **Time stored as the knob position, read in SECONDS** (a value block on an Exp0 curve, 0 … 5 s, unit Seconds:
   "375 ms" / "1.25 s"); a wire adds seconds, the delay atom's own unit and every other time's (the Envelope). Option:
   ms units (a wire of ±1 would add ±1 ms) — a curve-unit change, no data migration.
2. **The glide is TX.smooth's param ramp (20 ms, linear), not a dedicated ~30 ms one-pole.** Every param-rate move of
   the delay (drag, automation, sync step, mode switch, tempo) reaches `delay_cubic`'s time through the ramp the
   scheduler already puts on every rampable input: zero cost at rest, one general mechanism (Q187). A one-pole would
   need a per-sample z1 loop of atoms (always running) or a new `slew` atom. An audio-rate wire into Time is not
   smoothed (deliberate modulation). Option: add a one-pole if the linear glide's pitch shape sounds wrong.
3. **Sync and Mode are arithmetic, not muxes.** A stepped value block reaching a mux index lowers as a LITERAL choice
   (lower.h "Structural value blocks"), so a sync step or the ms / ♪ switch recompiled the unit, and the swap jumps
   the time (the new time source's `param_ramp` side state is a fresh entry: it starts at its value, so the change is
   the swap's 20 ms crossfade of two taps, not a glide). The delay line itself IS carried (checked: same state key,
   kernel delay_cubic, ring 240000 frames, 1 MB, matched by engine/src/state_transfer.cpp's key/tag/size match; an
   earlier reading of "the line restarted empty" was wrong — the test had switched to a delay longer than the audio
   played so far, so it read the silence before playback started). Now the beats are 2^g / 16 · f(r) on the index (a
   nested group "Note Value", every value checked against the table by render) and the mode a lerp: runtime params,
   no recompile per sync step, glided. Cost: both the time and the sync path run per block in which an input changes
   (a few param-rate ops).
4. **Minimum delay 2 samples** (the cubic read's floor; the plan says ≥ 1). Option: `delay` (linear) below 2 samples.
   DT.2: superseded — 1 sample (`delay_cubic` reads linearly in [1, 2)); item 2's glide is `delay_fade`'s own head.
5. **Gravity zones shrink between dense markings**: 6 device px per side, but at most 40 % of the gap to the next
   marking (at 120 BPM the 21 markings sit in the middle of a 148-px slider, where 6-px zones would cover all of it and
   the knob could never stop between 1/2T and 1/4.). Option: plain 6 px, or straight values only.
6. **1/64 is a short, unlabelled tick** (the plan labels straight values 1/1 … 1/32); it is in the sync list.
7. **Live tempo**: the markings are placed at the tempo at the playhead (start marker when stopped), re-placed whenever
   the face repaints (its live key includes the tempo). While the transport plays, the hosts (project patch canvas,
   Device view / panel window PanelHost) call `Face::frameTick` at ≤ 60 Hz: a tempo compare per slider, a repaint only
   when the playhead's tempo changed (tests/views/test_delay_tap_face.cpp: no paint at a steady tempo, one when the
   playhead crosses a tempo point).
Status: open (user: 2, 5).

## Q243 · 2026-11-12 · DT.3 · The Delay Tap face: tick spacing by tier, dotted before triplets, the left edge's undo
Context: docs/plans/DT.md "DT.3", `pq/views/panel/tempo_marks.h`, `ProjectCanvas::stretchEdgeAt`. Choices (each the most
reversible found):
1. **The spacing a tick needs grows with its family**: 3 logical px for powers of two and threes, 6 for fives, 8 for
   sevens, 10 for other numerators (the plan: ~4 for all). With 4 px for all, the default slider dropped every triplet
   (each sits ~3.5 px from a dotted value) yet showed 2/7, 4/7, 3/20 in the gaps they left. Option: one value.
2. **Dotted before triplets** within the threes (3/16 is shown over 1/6 when they collide). Option: triplets first.
3. **The stretch is undoable, the left edge's move is not**: the width is the group's document panel (one undo step);
   the block's position is its nudge (view state, never undoable, as every block move). Undoing a left-edge stretch
   narrows the face from its right side (its left edge stays where the drag left it). Option: make nudges undoable.
4. **A plate spanning the whole panel widens with it** (the Delay Tap's background); other non-stretching elements keep
   their width, those right of a stretching element move with its end.
Status: open.

## Q244 · 2026-11-12 · NZ.1 · Fold vs colour: the slope at positive colours and the RMS of coloured uniform noise
Context (measured, tests/dsp/test_noise_step.cpp, tools/fit/noise_colour.py): the colour filter itself meets the plan
(±0.418 dB of the slope over 20 Hz … 20 kHz at 48 kHz, unit power gain within 0.105 dB), but the plan's step 3 folds
its output into [−1, 1]. Coloured noise is near-Gaussian at the drawn RMS (uniform: σ 0.577), so the fold acts on ~8 % of
steps (uniform) / ~0.3 % (Gaussian σ 1/3); its distortion is broadband, which (1) costs coloured UNIFORM noise up to
1.1 dB of RMS after the fold (Gaussian: ≤ 0.17 dB, statistical) and (2) fills the low end of blue / violet noise, whose
own low end is 30–60 dB down: measured after the fold, +3 dB/oct uniform is ~±5.9 dB off its slope, +6 ~±19 dB
(Gaussian: +4.5 ±4.4 dB, +6 ±11 dB); brown / pink stay within ~±0.8 dB. Options: (a) the plan as written (fold always:
every output inside the Range); (b) no fold above 0 dB/oct at audio rate (exact slope, but blue / violet uniform then
exceed ±1 by up to ~3σ, outside the Range); (c) normalise coloured noise to a lower RMS (σ ≈ 0.2) so the fold almost
never acts (exact slope, in range, but a level drop of up to −9 dB sweeping away from white); (d) a monotone map to
uniform (erf) instead of the fold (in range, constant RMS, the slope only approximately kept, no walk reflection).
Choice: (a), the plan's order (reversible: one select in the kernel's step).
Resolution: user 2026-11-12 — scale, don't fold, for positive colours: colours −6 … 0 keep the reflection fold; above 0
the cascade's input is scaled by a per-colour bound, 1 / Σ|h| of the unit-power design (a sixth table column,
tools/fit/noise_colour.py, lowered 2.8 % for the interpolated designs) and 1 / 1.1547 for Gaussian draws, so the output
can never leave [−1, 1] and no fold runs: the slope is the design's exactly (through the module, measured: +3 ±0.44 dB,
+6 ±0.37 dB over 100 Hz … 16 kHz). Cost in level against white (uniform draws): −5.3 dB at +1, −5.8 at +1.5 (the most),
−4.85 at +3, −3.83 at +6 dB/oct; Gaussian a further −1.25 dB (the bound is continuous from 0: no jump sweeping through
white, the drop is over the first ~1 dB/oct).

## Q245 · 2026-11-12 · NZ.1 · On note with Per voice: every note the same pattern, chord notes included
Context: the plan says On note resets "at each note-on: every note plays the same pattern". With Per voice the lanes
are seeded apart; resetting each lane to its own seed would give each voice slot its own pattern (a note's pattern
then depends on which lane it got). Options: (a) On note keys every lane with the node's seed alone (`noise_step`
lanes 0): every note, chord notes too, plays one pattern; (b) keep lane seeds (patterns differ by voice slot, not by
note). Choice: (a), the plan's words (reversible: the `lanes` const in pq/meta/noise.cpp).

## Q246 · 2026-11-12 · NZ.1 · A free module's In: unfed groups leave a native engine's input at its default
Context: a module group's In terminal is wired to its engine's chain input; with nothing reaching the group the In
gave the engine zero, which for the Noise module's clock means "wired" (the clock overrides the clock setting and
nothing would ever step). The lowering now makes such a wire Inactive when nothing reaches the group (no wire into its
main input, or only from an unfed group's In — and only for a FREE group: off its track's path and not an instrument
group, which the path or its notes feed whether or not a wire names its main input), so the input reads its own default — for every native engine: a free
Envelope module now outputs its envelope (`in` default 1) where it gave silence. Options: (a) that rule (one place,
pq/document/lower.h "Groups"); (b) a Noise-only rule (a clock "seen" latch at run time); (c) no In → clock wire in the
Noise module (the user wires the engine's clock inside). Choice: (a): it is what "unwired" means for every engine, and
it matches the Envelope's own description ("unwired, the envelope itself").
Status: open.

## Q247 · 2026-11-12 · DT.4 · Repitch's glide law, the samples threshold, the micro bar's range and toggle
Context: docs/plans/DT.md "DT.4", kernels_memory.cpp `DelayFade`, tempo_slider.cpp. Choices (each the most reversible
found; constants in one place each):
1. **The glide**: a velocity-carrying cubic Hermite arriving at rest over N = |Δ|·2 (longer) / |Δ|/2 (shorter) frames,
   clamped to [fade, fade·100/3] (30 ms … 1 s): the pitch bends at most two octaves either way unless the 1 s cap
   applies (1/16 → 1/1 at 120 BPM, Δ = 1.875 s, bends further: the read rate briefly runs backwards, a tape-zip). Options:
   one octave (|Δ|·3 / |Δ|·0.75: 1/16 → 1/4 takes 1.1 s), a slew of fixed speed, a one-pole (unbounded pitch at the start).
2. **Samples below 10 ms** ("3.00 ms | 144 smp", 480 samples at 48 kHz). Option: only below the micro bar's 256.
3. **The micro bar spans 1 … 256 samples, linear, and opens with a magnifier toggle** (a panel edit: the face grows 24
   units, one undo step), not automatically at small times (an automatic resize would be a document edit mid-drag).
   Option: 1 … 128, or log.
4. **The readout names any candidate fraction the time equals**, shown or not (2/7 s reads "| 1/7" at the default width
   where 1/7 has no tick). Option: only shown markings.
5. **Float read position**: during a glide the head is a float (a 2⁻⁹-sample grid at 24000 samples): a white error below
   −90 dB while it moves (exact at rest), as every delay atom with a moving time. Option: an integer + fraction head in
   `delayTap` (a change to every delay kernel).
Status: open.

## Q248 · 2026-11-13 · AS.1 · Where the Autosave switch lives; what triggers a write
Context: AS.1 needs an on/off preference (ViewPrefs::autosave, on by default) and the plan's trigger is "snapshot differs".
Options: (a) a "Saving" section under Preferences → Record; (b) a new File tab (moves every preferences golden).
(c) also autosave view-state-only changes (folds, lane heights). Choice: (a), and view-state-only edits do not trigger a
write (the next document change writes them along). Reversible: moving the row is one function; (c) is one more compare.
Status: open.

## Q249 · 2026-11-13 · AS.2 · Takes an untitled autosave does not reference; an outdated autosave's folder
Context: a crashed untitled session can hold takes recorded after its last autosave (or removed from the document);
and a titled session's autosave that is older than its origin's project.pqd is not offered (plan), so it never gets a
Discard button. Options: (a) place unreferenced takes on a "Recovered" track (as L13's Open as Project) / leave them
unplaced in the new folder; (b) delete an outdated autosave silently at start / keep it (findStaleUnsaved never deletes).
Choice: (a) place them (one undo step "Recover Recordings"; nothing is hidden). (b) keep it: the folder is offered for
its takes only (Discard then removes the whole session); a session left with only an outdated autosave stays on disk
(small; one project.pqd). Reversible: (b) is one `removeUnsavedFolder` call in ProjectLink::staleRecordings.
Status: open.

## Q250 · 2026-11-13 · WI.2 · Which wires a blamed sum stands for; the cases a document wire can produce
Context: inference blames one track-patch edge (consumer, source). When several wires are summed into one input the
edge's source is the sum node; a node of a modulated param's site carries every wire into that param. The plan also
names a "stereo → mono-bound" wire as an acceptance case.
Options: (a) blame every wire of the sum / site; (b) only the wires whose type could carry the error (needs the
declared types of every term in the link); (c) cut them all, then restore one at a time (more compiles per edit).
Choice: (a) — the sum is blamed as a whole (an edge match in LoweredProject::wireEnds; a site node maps to its wires).
A good wire summed with a bad one is cut with it until an edit of its track re-tries both. Reversible: cutBlamed is one
function. Width: no document wire produces a width error today — the lowering takes the mid of a wider source into a
meta module's input and channel 0 across tracks, every other input takes any width — so the acceptance test uses the
two reachable kinds instead: rate (the Utility's audio into delay_fade's param-bound `fade`) and kind (a main-path link
from an instrument into an `and` block: a path break). Found on the way and fixed (needed for a cut link to play): an
`and` / flip-flop block off the path failed its whole track (its bool input got the free zero, a real; an undriven bool
input of an atom block read a real 0): such inputs are now false.
Status: open.

## Q251 · 2026-11-13 · EX.1 · Stems with return / master FX: a fresh link per solo pass instead of solo param writes
Context: D7 says "include return/master FX = one solo pass per track (param writes only, no recompile)". Measured on the
playback fixture: a solo pass on the link of the previous pass differs from the same track rendered as the first pass
for its first ~20 ms (up to −18.6 dB relative; Lead soloed twice in a row is bit-identical, after a Mos pass it is not),
whatever the settle time (0.05 / 0.5 s stopped) and with idle units off — some param-ramp / gate state only moves while
playing. Options: (a) param writes on one link, accept the history-dependent first 20 ms; (b) an engine state reset
(re-init every unit's state arena and voice table: new engine API); (c) a fresh private link per solo pass, built from
the document with that track soloed, the previous link alive until the new one compiled (the JIT shares the live
images' code). Choice: (c): exact (each solo pass equals the captured bus that carries it, ≤ −120 dB), no engine API;
cost one interpreter compile per track (~50 ms on the fixture) and the JIT's, mostly cache hits. Reversible: runExport's
pass loop is the only place.
Status: superseded by EX.x-fix-settle (Q?-EX.x-fix-settle-a): the bleed was a smoother falling asleep mid-ramp in the
settle; fixed, every pass on one link equals its first bit for bit, and runExport is back to one link (D7).

## Q252 · 2026-11-13 · FZ.1 · What a frozen track's "own wires" are; what the guard lets through from outside
Context: D4 refuses edits of a frozen track's devices, clips, notes and device lanes. Cables are the circuit too
(TX.all-free), and some cross tracks: a cable from another track's LFO into a frozen block, a cable from another track
into the frozen mixer. A tempo edit also re-syncs a frozen audio track's warp-Off clips (their lengths change).
Options: (a) own wires = both ends on the frozen track (its blocks / its source → its blocks / its mixer); cables from
other tracks are inputs from outside, like tempo and tunings: allowed, the render is re-made (D3); (b) every cable
whose target is the frozen track's (also refuses deleting another track whose block modulates it, and any cable edit
there); (c) ChangeSet-based: refuse when `cs.devices / clips / notes / lanes` name the track's entities (refuses
tempo edits on frozen audio tracks, which re-sync their clips).
Choice: (a), content-based (`frozenConflict`: compares the frozen content before / after, the warp-Off re-sync of a
tempo edit compared against the old track synced the same way). A cable from another track into a frozen block plays
nothing until the render is re-made (the block is not lowered; FZ.2's key must cover what feeds the frozen part).
Consequences: recording into a frozen track is refused at commit (FZ.3 may disable arm); a SetSample that changes the
rate of a sample a frozen warp-Off clip plays is refused when the re-sync changes that clip. Reversible: one function.
Status: open.

## Q253 · 2026-11-13 · EX.2 · Export Audio: closing the window during an export
Context: the plan does not say what Close / Esc do while a job runs. Options: (a) cancel the export; (b) close the
window and let the export finish in the background, the status line saying how it ended (the window, reopened, shows
the progress or the result); (c) refuse to close while exporting (modal). Choice: (b): an export can take minutes and
playing / editing go on meanwhile (it renders on a private link); Cancel is one click when wanted. Reversible: the
window's close button and `ExportHost::watch` are the only places.
Status: open.

## Q254 · 2026-11-13 · FZ.2 · The freeze key, the render document and the cache file names
Context: D3 keys a render by (rate, tempo map, tunings, the frozen part's patch hash); Q252 adds what feeds it. A patch
hash needs a lowering (a library, slots); the plan names the file `cache/freeze/<track id>.wav`.
Options: (a) hash the lowered patches of F and its feeders; (b) hash the RENDER DOCUMENT (F, the MIDI / audio tracks
whose blocks or notes feed F's blocks, their ancestors as empty containers, the samples their clips play; everything F's
own contribution cannot depend on cleared: names, colours, mixers, mixer lanes, loop, locators) as `serializeProject`
writes it, with the tempo map and tunings hashed apart for diagnosis; (c) the whole project.
Choice: (b) — library-free, cheap (a document compare decides "unchanged"), and the render document is exactly what is
rendered. A cable from a group's / return's / master's block or source (a sum of tracks, F's own output possibly in it)
falls back to (c). Consequences: the whole tuning list is in the key (editing a tuning F does not play re-renders it);
chance notes freeze one draw (the render link's seed); the WAV is content-named `<id>-<content key>.wav` (a re-render
never overwrites a file the pool may stream; the superseded one is deleted when replaced, best effort) with `<id>.json`
holding the key. Reversible: `freezeDocument` / `planFreeze` and the file names are one place each.
Status: open.

## Q255 · 2026-11-13 · DY.1 · window's op as a number, and what a window holds before its first frame
Context: DY.1's `window(x, time, op)`. The language has no symbolic constants for an atom's const input, and a window
must say what it reads before L inputs have arrived. Options: op (a) a number 0 max / 1 min / 2 mean, (b) three atoms
(`window_max` …); history (c) zero (as the delay lines), (d) only the inputs so far (a shorter window at the start).
Choice: (a) and (c): one atom, like svf's `mode`; zero history is what a delay line and `latency` hold, so a limiter's
`window(r, la, max)` beside `latency(x, 0, la)` sees the same past (r ≥ 0: no difference at the start). NaN reads as 0,
±inf as ±FLT_MAX (a running sum and an envelope never hold a non-finite value). Reversible: kernels_dynamics.cpp's
input clean-up and initState's zero values; op names would be a parser feature.
Status: open.

## Q256 · 2026-11-13 · FZ.3 · Freeze UI: what Freeze does to arm, where Unfreeze is offered, what is veiled
Context: the plan asks for Freeze / Unfreeze in the menus, a disabled arm, an "Unfreeze" offer on a refused edit, and
frozen devices / lanes not offered for edits; it does not say what happens to a track armed when frozen, nor which
editors besides the arrangement and the Device view stop offering edits.
Options: (a) Freeze leaves arm alone (the take is refused at commit); (b) Freeze disarms in the same undo step.
Unfreeze offer: (c) a hint in the status text; (d) a button on the status line. Piano roll of a frozen clip: (e) veiled
like the Device view; (f) left as is, its edits refused by apply with the same Unfreeze button.
Choice: (b), (d) — `StatusAction` on the status line, the generic refusal names the selected frozen track (or the only
one), else appends "(track menu: Unfreeze Track)" —, (f) for now (the clip view, the take lanes and the project patch
view's band of a frozen track are not veiled; apply refuses, the status line offers Unfreeze). Flatten is shown
disabled in both menus until FZ.4. Reversible: `ViewContext::setTracksFrozen` / `setStatus`, one veil class.
Status: open.

## Q257 · 2026-11-13 · DY.2 · what "Pre FX" is on a MIDI track, and what "None" does to the sidechain's On
Context: DY.2's `cablesource` picker offers per other track "Pre FX" (the plan: Wire::noteSource + kMainPort, the
track's own signal) and "Post FX" (the block feeding its mixer), and sets an optional "On" value block when a source is
picked. A MIDI track's own signal is its NOTES (a notes cable), not audio, so a sidechain from it would hear nothing.
Options: Pre FX on a MIDI track = (a) the notes cable as the plan says, (b) the main output of the path's first block
(the instrument), (c) not offered; at None, On (d) left as it is, (e) set to 0.
Choice: (b) and (e): what "the kick before its effects" means to a user is the instrument's audio; None turning the
sidechain off keeps the detector on the module's own input instead of silence (a compressor keyed by nothing would
never compress). Audio / group / return tracks use the own-signal wire (a). Pre equal to Post (no blocks after it) is
offered once, as Pre. Reversible: doc::ops::cableSources and setCableSource (edit_cables.cpp) are the one place.
Status: open.
## Q258 · 2026-11-13 · FZ.4 · Render edits: the song-start declick, what goes with a flattened track, where files go, what a bounce copies
Context: D5 flattens in place and the acceptance asks flatten / bounce / consolidate to null against what they replace.
An audio clip gets the 2 ms edge declick at both edges (a frozen render's clip has none: `AudioClipDesc::declick`
false), the document has no per-clip switch for it, and a clip cannot start before tick 0 without a seek fade. D5 says
"deletes blocks", but a frozen track may keep a block whose output reaches another track. The plan names
`samples/frozen/` only; it does not say where bounces / consolidations go, what a bounced track's mixer is, or what
happens in an untitled project.
Options: declick (a) leave it: a render that sounds within the first 2 ms of the song gets its first 2 ms ramped in
(every other edge lies in ≥ 10 ms of quiet, padded by the regions, and nulls at ≈ −590 dB); (b) an `AudioClip` flag
"no edge declick" set on render clips (model + file + every clip edit deciding whether a piece keeps it). Kept-live
blocks: (c) removed with the rest, their cables too (D5 literal); (d) refuse Flatten while one exists; (e) keep them.
Bounce mixer: (f) a default mixer; (g) the source's volume, pan, pan law, mute, sends, output, Track Delay.
Files: (h) `samples/frozen|bounced|consolidated/`, an untitled project's in its unsaved folder (moved to
`samples/recorded/` at its first save, as recorded takes); the flatten file a COPY of the cache render.
Choice: (a), (c), (g), (h). (a) keeps the model unchanged; the test pins the first 2 ms as the frozen output under the
linear ramp. (g): with the source deactivated the song sounds the same (mixer automation stays on the source). The copy
lets Ctrl+Z play the cached render again without a render (the cache stays deletable). Consolidated audio clips are
warp Off (exact; they no longer follow tempo changes, as recorded takes). Ctrl+J with both MIDI and audio tracks in a
range makes two undo steps (the MIDI one at once, the audio one when its render lands). Reversible: `regionClip` /
`renderRegions` (declick), `flattenTrack`'s removal loop, `bounceToNewTrack`'s mixer copy, `runJob`'s folders.
Status: (a) and (c) reversed by FZ.x-fix (Q?-FZ.x-fix-a): render clips have no edge declick; Flatten keeps the blocks
that feed another track.

## Q259 · 2026-11-13 · DY.3 · the transfer display's live dot: the group's own In, not a second binding
Context: the plan's `transfer` element has a dot "at the live input level (Signal binding)". The group is named by a
binding too. Options: (a) one Signal binding on the group's In (`input`): it names the group and its tap is the level;
(b) slot 0 names the group (on its `y`, like `response`), slot 1 an optional Signal for the level (any port).
Choice: (a): for every group a transfer curve makes sense on (a waveshaper; DY.4's Gain Computer, whose In is the level
in dB) the live x IS its In; one tap, one binding; `dot: 0` hides the dot and acquires no feed (the tap stays while the
element is placed, like a meter's). Reversible: slots are a list (transfer_element.cpp kSlots; panel.cpp panelTaps).
Status: open.

## Q260 · 2026-11-13 · MF.1 · SMF reading choices the plan leaves open; MPE bend resolution vs MF.3's 0.01 ¢
Context: MF.1's plan lists the messages to read, not how they combine. MTS has two independent mechanisms (tuning
programs selected per channel by RPN 3 / 4, filled by 08 01 / 02 / 04 / 07; and per-channel scale / octave offsets,
08 08 / 09); a format-1 file's tracks share one port's 16 channels; the SMF spec cancels running status after meta and
sysex events; a sustained key can be re-struck. MF.3's acceptance asks MPE export → import "within 0.01 ¢", but a 14-bit
bend at the MPE member range of ±48 semitones resolves 4800 / 8192 ≈ 0.59 ¢.
Options: tuning (a) a channel's offset = its program's key table + its scale / octave offset (additive); (b) the most
recent mechanism wins. Channel state in formats 0 / 1: (c) shared by all tracks (tracks merged in time, ties by track
index); (d) per track. Running status: (e) strict (an error after meta / sysex); (f) kept (lenient). Re-strike under the
pedal: (g) ends the held note; (h) overlaps it. MF.3: (i) compare within 0.3 ¢ (half a step) in MPE mode; (j) export a
smaller member range (RPN 0 after the MCM; ±1 semitone → 0.012 ¢ steps, ≤ 0.006 ¢ error, and the nearest key is never
more than 50 ¢ away).
Choice: (a), (c), (f), (g) — each one function in src/document/src/midi_file.cpp (`keyOffset`, `merged`, `parseTrack`,
`noteOn`). Format 2 keeps per-track state. MF.3 decides (i) or (j); the reader honours either (RPN 0 on a member sets
its zone's range).
Status: open.

## Q261 · 2026-11-13 · DY.4 · the Compressor's Expand law and its 3-way Mode
Context: the plan says "Expand: mirror below threshold (downward), capped at 70 dB" and lists Mode chips Peak / RMS /
Expand. A point mirror of the compress curve (slope 1/R below T) would raise quiet signals (upward), not expand.
Options: Expand GR = (a) (R − 1)·body(T − x) (downward, slope R below T), (b) (1 − 1/R)·body
(a gentle expander, slope R/(R − 1)); detection in Expand (c) peak, (d) a separate Peak / RMS switch.
Choice: (a) and (c): one Mode switch as the plan's chips, the same soft knee mirrored about T, min(·, 70 dB); at the
default 4:1 a signal 10 dB under T drops 30 dB more. Reversible: the Gain Computer group's `slope` mux and the `level`
choice in compressorBlocks (modules.cpp).
Status: user 2026-11-13: two switches — Detector (Peak / RMS) and Law (Compress / Expand), every combination (AU.dsp: the Mode chip split; literal muxes, no extra CPU).

## Q262 · 2026-11-13 · DY.4 · Sidechain EQ "on" as a value block, Listen's signal, the GR jack, defaults
Context: the plan's sidechain EQ is "an EQ Band child group off by default" with an "EQ on" control on the panel, but a
panel binding cannot reach a group's active state (panel.h: Control / Setting / Data / Signal / Panel / Input); Listen
"Out = SC signal" and the "GR block's output is a cable source" leave the exact taps open.
Options: EQ on = (a) a stepped "EQ On" value block choosing In / the band through a literal mux (the band group stays
active), (b) a new binding kind for a block's active state; Listen = (c) the detector's signal (In, or lerp(In, SC, Mix)),
(d) the SC group's Out alone; GR = (e) reduction dB ≥ 0 on a modsource jack, (f) the linear gain.
Choice: (a), (c), (e): (a) costs nothing off (the band is unread, not scheduled) and needs no new binding; (c) is what
the detector hears, Mix included; (e) is what the meter shows, a cable source like any output (it can drive another
device's control). Defaults: threshold −20 dB, 4:1, knee 6 dB, attack 2 ms, release 50 ms (it compresses when inserted; Live's
default threshold 0 dB does nothing). Reversible: compressorBlocks only.
Status: open.

## Q263 · 2026-11-13 · MF.2 · MIDI import choices the plan leaves open
Context: MF.2 maps resolved SMF notes onto tracks (docs/plans/DY.md "MF.2", decision 4). Open points: (1) what pitch a
bent / MTS-retuned key means — the key's live-input pitch plus the offset, or the absolute 12-TET(key) · 2^(cents/1200);
(2) a plain key the track's .kbm leaves unmapped (live input plays nothing); (3) whether imported clips loop; (4) where a
mixed drop's MIDI tracks go relative to the audio part's new tracks; (5) tempo before the first tempo event / signature
when a file has some but not at tick 0.
Options: (1a) live-input pitch + offset (follows the track's key map); (1b) absolute pitch (the file's own frequencies).
(2a) drop the note (live input's answer); (2b) keep it at its 12-TET pitch (Degree within 0.5 ¢, else Cents). (3a) not
looped; (3b) looped over the clip. (4a) MIDI first (onto / after the drop's track), then the samples' tracks; (4b) the
reverse. (5a) the SMF defaults (120 BPM, 4/4) from the file's start; (5b) the project's.
Choice: (1b) — MTS and bend files carry absolute pitches, and 31edo MTS files then land on the 31edo degrees; (2b),
counted in the status line; (3a), as a recorded clip with `loop` off; (4a), one undo step for both (the drop's
coalescing key); (5a). All in src/document/src/midi_import.cpp (importMidi) except (4) (arrangement/file_drop.cpp commit).
Status: (1) user 2026-11-13: per FILE — a file with any MTS message, pitch bend or RPN tuning (RPN 0 / 3 / 4) is read as absolute pitch for every note (12-TET key + cents → InputPitchMap::atHz: Degree within 0.5 ¢ in Keys mode, else Cents); otherwise every key goes through the track's key map (AU.dsp, RB-DY M8). (2)–(5) open.

## Q264 · 2026-11-13 · DY.5 · truepeak: the plan's ±0.2 dB against the BS.1770 filter's own ripple
Context: DY.5 asks for `truepeak` to be the BS.1770-4 Annex 2 estimator AND within 0.2 dB of a 16× reference on sines
at 0.24–0.49·sr. The standard's phases 1 and 2 have +0.2 dB of passband gain near sr/4 (−0.24 dB at DC), so with the
standard's coefficients it OVER-reads by up to 0.222 dB at 0.24–0.265·sr (about 1 in 12 random cases above 0.2); it
under-reads by at most a few thousandths of a dB on them (exactly rational frequencies are worse: 1/3·sr with an unlucky phase
under-reads 0.29 dB, the 4× grid's known limit). Options: (a) the standard's coefficients, bound one-sided (under
≤ 0.2 dB, over ≤ 0.23 dB); (b) an own flatter 48-tap filter that meets ±0.2 dB (no longer a BS.1770 meter: readouts
differ from other meters'). Choice: (a). Over-reading only makes the Limiter limit 0.2 dB harder near sr/4; DY.6's
"true peak ≤ ceiling + 0.2 dB" depends on under-reading, which is ~0. Reversible: kTpPhase in kernels_dynamics.cpp
and the test's bound (tests/dsp/test_dynamics.cpp).
Status: open.

## Q265 · 2026-11-13 · MF.3 · MIDI export choices the plan leaves open
Context: MF.3 (docs/plans/DY.md "MF.3") writes clips as SMF in 12edo / MPE / Degrees. Open points: (1) an MPE voice over
15 reuses the oldest channel — the old note keeps sounding under the new bend, or ends; (2) several clips in one MPE
file — one channel pool per MTrk or one for the file; (3) a key outside 0 … 127 (or, in Degrees, a degree only an 'x'
.kbm key would play) — clamp or skip; (4) tempo ramps (SMF has only steps); (5) a deactivated clip or chance < 1 notes;
(6) the menu's mode when its save dialog is cancelled.
Options: (1a) ends at the new note-on (a voice steal; no mid-note bend) / (1b) keeps sounding (its pitch moves);
(2a) per MTrk / (2b) one pool (format 1 tracks share a port's 16 channels: MF.1's reader merges them, Q260 (c));
(3a) clamp / (3b) skip, counted; (4a) a tempo per sixteenth at the step's mean (durations exact to the µs) / (4b) the
ramp's start tempo only; (5a) exported as stored / (5b) left out; (6a) remembered anyway / (6b) only after a write.
Choice: (1a), (2b), (3b), (4a), (5a) — the user picked the clip; every note is written whatever its chance — and (6a).
All in src/document/src/midi_export.cpp (`allocateMpe`, the mode switch, the tempo loop) and src/views/src/
midi_export.cpp (`exportMidiClips`). The MCM + RPN 0 setup is repeated at tick 0 of every clip's MTrk so a track
imported alone still plays as MPE. MPE keeps the ±48 member range (orchestrator); the round trip holds within 0.3 ¢.
Status: open.

## Q266 · 2026-11-13 · DY.6 · the Limiter's Link M-S against the ceiling, and its defaults
Context: DY.6 asks for Link Stereo / L-R / M-S (M/S = the EQ's matrix) and no output sample over the ceiling. Limiting M
and S each to the ceiling does not bound L = M + S, R = M − S (up to +6 dB when both peak together); only a coupled gain
can, and the plan names no defaults.
Options: M-S (a) M and S each limited to the ceiling, the final clip catching the L/R overs; (b) one gain from
|M| + |S| (= max(|L|, |R|): Stereo again); (c) the side's gain ≤ the mid's (narrows the image under limiting).
Choice: (a) as the plan: the safety clip makes the ceiling hold in every mode; on ordinary material |S| ≪ |M| and the
clip rarely acts. Defaults like Live's Limiter: Gain 0 dB (−12 … +24), Ceiling −0.3 dB (−24 … 0), Release 300 ms with
Auto on, Lookahead 3 ms, Stereo, True Peak off. Also: the plan's det = latency(|x|, 6) beside out = latency(x, 6, la) is one
frame short for its proof (a window of L includes its own frame), so the detector runs 7 frames behind (truepeak + 1);
the latency stays L + 6. Reversible: limiterBlocks (modules.cpp).
Status: user 2026-11-13: M-S removed (AU.dsp: Link is Stereo / L-R); the defaults stand.

## Q267 · 2026-11-13 · FZ.x-fix · Flatten with blocks that still feed another track; what a render clip's "no edge fade" means
Context: Q258 chose (c) (kept-live blocks removed) and (a) (render clips keep the 2 ms edge declick); FZ.x-fix reverses
both. Keeping the blocks frozenKeep keeps cannot keep what they need when that is the track's OWN notes / audio (a
notes cable from its source into another track, a kept instrument group played by its notes): a flattened track has
neither. The per-clip flag needs a rule for edits that copy a clip.
Options: refused source feeds: (a) refuse Flatten while its own notes / audio play another track (status line names
it, before any render) / (b) flatten anyway and drop those cables (the other track changes, the bug being fixed) /
(c) keep the note clips on an audio track (not a valid document). Flag: (d) every copy keeps `noEdgeFade` (split,
trim, duplicate, paste) / (e) edits clear it on the pieces.
Choice: (a), (d). (a) keeps every other track unchanged or refuses; (d) is what a split needs (the two pieces of a
render played back to back are seamless; with a declick at the cut they dipped), and a piece the user moves into sound
starts hard, as the render clip itself does. SetTrackKind now allows blocks its source does not play (so kept blocks
survive MIDI → audio). Bounce to New Track still deactivates its source (FZ.4, Q258 (g)): a block of the source that
feeds another track stops then — not in this task's scope. Reversible: `flattenKeep` / `flattenable`
(edit_render.cpp), `regionClip`'s flag.
Status: open.

## Q268 · 2026-11-13 · EX.x-fix-settle · Which pass was wrong, and what settling now guarantees
Context: the task assumed a fresh link's first pass ramps because its smoothers start from defaults. Measured on the
playback fixture (Grp's volume automated from −18 dB at tick 0): the FIRST pass is right (Grp at −18 dB from frame 0);
every later pass faded from 0 dB (the previous pass's end) for ~1650 frames. Cause: in the settle, the locate's
automation value starts a 20 ms param ramp; Grp's input is silent, so its sleep segment fell asleep after one block
(params unchanged, the gain's residual 0 while it followed the ramp) with the gain's target mid-ramp, and only the
next sound woke it. A second cause showed on edited links: work that arrives during the settle (a compile deferred
until the engine ran, a lowering a finished sample load asks for) was published by the pass's own flush, so a new
unit's gains faded in from silence at the pass start.
Options: (a) a settle that resets every smoother's state to its input (needs the Control region's values: an image API
per backend) / (b) the host's sleep rule: a stage in param-ramp mode counts as a param change for its segments (they
fall asleep at the target), plus settleOffline taking and settling late work / (c) keep the second-pass workaround.
Choice: (b): one compare per segment per block, ~20 ms more awake after a param change, nothing at rest; it also
fixes the live case (a stopped locate or a knob move on a silent track no longer leaves a stale gain that fades at the
next sound). Every pass on a link now equals its first bit for bit (tests: automated volume twice; Lead then Mos
soloed == Mos soloed on a fresh link), so EX.1's stems with return / master FX run on one link again (no compile per
track). Reversible: `ImageHost::decideSleep`'s first line, `settleOffline`'s loop, runExport's pass loop.
Status: open.

## Q269 · 2026-11-13 · WI.3 · What the gesture-time type check covers
Context: the plan names "a declared-real output onto a bool-bound input, event ↔ non-event". Ends can be device outputs
(`out` / export), note outputs, inputs, params and mixers; some declare nothing (a group's terminals, exports, a track's
own signal). A real wire onto a bool PARAM is not compiled either (the lowering marks it Invalid). Options: (a) inputs
only, the drop alone; (b) inputs and params, every path that makes a project-patch cable (the drop, the param menu
through `connectEnds`), the reason shown already while hovering; (c) also infer a group's / export's kind from inside.
Choice: (b): one check (`doc::declaredWireClash`) on declarations only — an undeclared end is never refused, WI.2's cut
stays the judge of everything else; note outputs typed as their voice terminals (gate bool, hz / velocity real).
Reversible: wire_types.cpp and its three callers.
Status: open.

## Q270 · 2026-11-13 · AU.app · A frozen track's render ignores solo on the tracks that reach a sum it hears
Context: AU.app H3 replaced the whole-project fallback (a cable from a group's / return's / master's source into a
frozen track's block) by the sum's contributors with their mixers. Live, a soloed track mutes the others (and with them
what reaches the sum); the render document has only part of the project. Options: (a) clear solo in the render
document (the render is the unsoloed mix; soloing never re-renders); (b) keep every track's solo state in the key
(soloing while mixing re-renders and flips the track live); (c) keep the contributors' solo flags only (wrong when a
track outside them is soloed). Choice: (a): a freeze is a mixdown aid, and soloing to listen should not re-render.
Reversible: `normalTrack`'s `full` branch in src/app/src/freeze_service.cpp.
Status: open.

## Q271 · 2026-11-13 · VZ.1 · What "a tab with just the visual settings" and "save setting" mean
- Context: the user asked for an options dropdown on every visual module, "a tab that changes settings to just visual
  components", and a save option that applies to newly spawned modules.
- Options: (a) tabs inside each module's sheet (ANALYSIS / DISPLAY); (b) a Preferences page holding every module type's
  look; (c) both. Save: all options vs the DISPLAY tab only; RESET to the saved default vs the factory.
- Choice: (a), the sheet opening on DISPLAY; SAVE DEFAULT saves every option (both tabs) for that type; RESET goes to the
  factory options (the saved default stays until saved over). The Spectrum's PEAK chip is now its "Peak line" option
  (an undo step, saved with the rest). Most reversible: the store and the sheet are independent of where they are shown.

## Q272 · 2026-11-13 · AU.dsp · How quiet a dB-domain follower must be before its segment sleeps
Context: AU.dsp lets the Compressor and Limiter sleep (RB-DY H4). A state sleeps once it is within ε = 2^-30 of rest in
its own units; that is −180 dBFS for an audio state, but a gain-reduction follower holds dB, where 2^-30 dB is far below
anything audible. A release from R dB takes τ·ln(R / ε) ≈ (21 + ln R)·τ: about 1.5 s from 20 dB at the Compressor's
50 ms, about 30 s with the Limiter's Auto (its slow follower's τ is 4 · 300 ms). Until then the region runs.
Options: (a) keep ε in the state's units for every atom (exact to −180 dB in audio terms, slow dB tails); (b) a looser
ε for dB-domain followers (e.g. 1e-4 dB: an error of ~1e-5 in the gain when sound resumes), declared per block (a
`follow` setting the modules set) or inferred from what feeds it (fast_gain_to_db upstream); (c) flush a follower
once its effect on the output is below −120 dB relative (needs the downstream gain law in the plan).
Choice: (a), the most conservative and reversible (dsp::followStateSmall's eps and the host's kSleepEpsilon); (b)
would cut the Limiter's silent tail to ~3 s.
Status: open.

## Q273 · 2026-11-13 · BR.wt · Shape mode's phase slider reaches only the upper half of the curve
Context (bug report 2026-11-13_17-42-08): in Shape mode x = input + phase and u = clamp((x + 1)/2), but `phase` is a
wrapping 0 … 1 control, so with a small input the bias can place it only on u ∈ [0.5, 1): the left half of the drawn
curve is unreachable without a DC source. (The report's own project was in Phase mode, where the slider spans the
whole cycle; there DSP, tap and display already agreed.)
Options: (a) leave it (bias the input upstream); (b) in Shape mode read the slider as x = 2·phase − 1 (its travel then
spans the display one to one; changes the expansion and every Shape-mode project's sound); (c) a separate bias param.
Choice: (a) for now — most reversible; BR.wt only fixed the idle dot to sit at the true u = (phase + 1)/2.

## Q274 · 2026-11-13 · BR.patch-io · Frame sockets' order and the buses into returns
Context: BR.patch-io gives an opened group's frame one socket per crossing connection, the closed block's ports, and
ends every bus at its band's input socket on the band's left edge.
Options: frame sockets (a) top to bottom in the closed block's port order, or (b) each level with the pin it leads to
inside (fewest crossings; the order can change as blocks move); sends into a return (c) keep their old trace into the
Σ, or (d) end on the return's left socket like routes (one rule for every bus).
Choice: (b) — the set and the keys stay the closed block's, only the rows follow the pins inside; and (d) — a return's
only feeds are sends, so its bus and the master's / groups' read the same. Both are view-only (model.cpp
frameSockets / route), reversible without document changes.
Status: open.

## Q275 · 2026-11-13 · T7.2 · What "Tune to JI" does with the selected notes
Context: `ops::jiFromNotes` makes the object (stop 0 = the notes as written, stop 1 = the proportion). The plan does not
say whether the notes stay.
Options: (a) the notes stay and become the object's inner notes (`Harmonic(object, voice)`), the object a scaffold
(`sounds = false`): ids, timing, velocities and curves are kept, each note follows its voice's morph once T7.3 gives
inner notes their voice's curve; (b) the notes are removed and the object sounds as one block.
Choice: (a) by default (nothing is lost, `deleteJiObject` gives plain notes back); (b) is
`JiFromNotesOptions::replaceNotes`. Until T7.3 lands, an inner note plays the pitch it has at its start (no morph).

## Q276 · 2026-11-13 · T7.2 · A note that follows a JI object the clip does not have is refused
Context: the plan asks for validation (Harmonic names an object of the clip, voice < V). Before T7.2 such notes were
accepted and stayed unresolved (as a Ratio anchored to a missing note still is).
Choice: strict, as planned: AddNotes / UpdateNotes / MoveNote, AddClip and the loader refuse them. Consequences: the
test generators' three dangling notes (object ids 7, 42, 0xFFFFFFFF) now follow real objects and
`fixtures/full/project.pqd` was regenerated; pasting Harmonic notes into another clip with `ops::addNotes` fails as a
whole, so the roll (T7.4 / T7.5) should convert them first (`convertNotes`) or paste the object too. A fundamental and
a tuning stop's pitches may be Degree, Cents or a reference-anchored Ratio only (not anchored to a note).

## Q277 · 2026-11-13 · T7.2 · Stops beyond an object's end are kept
Context: shortening an object could drop the stops past its end, which can lower the voice count and strand inner
notes on voices that no longer exist.
Choice: a stop at or after `length` is valid and simply never reached (lengthening brings it back). Only a start moved
later past a stop drops the earlier ones (that stop becomes stop 0); the op is refused if a note would lose its voice.

## Q278 · 2026-11-13 · T7.1 · Chase long notes on a seek / loop into their middle?
Context: the M7 plan's T7.1 says the curve cursor works "at note-on (also seek / loop into the middle of a note)". The
timeline has never chased notes: a note sounds only when its START lies in the played range (timeline.h "notes that
started earlier are not chased"; clip passes drop notes that start before them). T7.1 keeps that: a note-on reads its
curves at its own tick wherever that lies in them (a curve may begin before its note), but a locate or an arrangement
loop into the middle of a long note — a whole JI object lowered to one note per voice — plays nothing until the next
note starts.
Options: (a) leave it (today's rule for every note); (b) chase in the timeline: on a seek, start the notes whose span
covers the new position (hz / gain from the curve there; needs a per-clip index to find them in bounded time on the
audio thread, and a decision for chance, one-shots and envelopes that restart); (c) T7.3's lowering splits a JI
object's voices into shorter notes at stop holds, so a seek loses at most one segment (unison phase would restart).
Choice: (a) for now — nothing new can break; the engine side of (b) is one more caller of the existing
curve-at-a-tick path (`Timeline::curveLocate` + `curveRamp`).
Answer (T7.3, orchestrator decision): (b), for every note, as Live's "Chase MIDI Notes". A jump of the playhead while
playing (Play / Record at a tick, Continue, a Locate while playing, the end of a count-in, an arrangement loop wrap)
starts the notes of the clip pass under the new position whose start lies before it and whose end after it: a fresh
note-on there (envelope from its start), the pass's chance / velocity decision, Hz and gain from the curves at that
tick. Found through a per-clip max-end tree (`SequenceImage::covering`, O((found + 1) log notes)). Not chased: by a
sequence swap (an edit under the playhead starts nothing), notes of an earlier clip-loop pass (they are cut at the
wrap), one-shot state inside the instrument (an envelope cannot be resumed mid-way). timeline.h "Chase".
Status: answered.

## Q279 · 2026-11-14 · T7.4 · No document builder duplicates a JI object or moves it in pitch
Context: the roll's Ctrl+D and its row drag need "copy this object (with its inner notes)" and "move by (dt, rows)".
`ops` has `moveJiObject` (time only) and `cloneNoteClip` (a whole clip).
Options: (a) the roll composes them from raw commands (`AddJiObject` + `AddNotes` + `SetNoteCurve` in one Batch; the
`SetJiObject` inside `moveJiObject`'s Batch replaced by the pitch-stepped object); (b) add `ops::duplicateJiObject` and a
`steps` argument to `ops::moveJiObject` in `document`.
Choice: (a) in `views/src/piano_roll/roll_ji.cpp` (T7.4 may not touch `document`); (b) is the cleaner home — move it when
T7.5 edits `document` anyway. A pitch move steps the fundamental and every tuning stop's pitches with `ops::stepPitch`.
Status: open.

## Q280 · 2026-11-14 · T7.4 · Where the chord sits in a drawn box
Context: "the fundamental is the pitch at the box's bottom edge". With `ops::createJiObject`'s scale 1, 8:9:10:12:15 on
that fundamental sounds three octaves above the box.
Options: (a) scale 1: the box only marks the fundamental; (b) pin the lowest voice to the bottom row (stop 0's scale =
1 / lowest term; undertone: the largest term), as `ops::jiFromNotes` pins the lowest note: the chord is where the box
is, and the stored fundamental is still the bottom row (a Degree identity).
Choice: (b). The harmonic numbers still read 8, 9, 10…; the true fundamental (jiFundamentalCents) lies below the box.
Status: open (reversible: one line in `RollState::createJiObject`).

## Q281 · 2026-11-14 · T7.3 · "Merge without Sync never dips below −1 dB of the keeper": true over 100 ms, not over 10 ms
Context: the M7 plan accepts the parked merge with "no window below −1 dB of the keeper". Measured on 4:5 → 4 (one
quarter S-glide, sine, against the keeper alone): 100 ms windows −0.1 dB (and exactly the keeper once the merging
voice is parked: no jump); 10 ms windows down to −5.4 dB during the last, slow beats of the glide and about −1 dB in
the 20 ms fade. That is two free-running oscillators converging: they beat, and the troughs of beats slower than the
window are below one voice. No gain rule of the plan removes it without knowing the phases.
Options: (a) accept until T7.7's Sync (the test asserts the 100 ms level, the exact parked level, and bounds the
10 ms figures); (b) fade the merging voice over the whole glide (`JiPlanOptions::gainRamp` = the glide): shallower
troughs (about −3 dB), but the merging voice is quieter through the whole morph; (c) fade it out when it comes within
a few Hz of the keeper.
Choice: (a) — the plan's decision 3 ("park it; T7.7 adds Sync") as written; (b) / (c) are one option of jiVoicePlan.

Answer (T7.7, 2026-11-14): the merging voice's gain is the SQUARE of its remaining distance to the keeper (cents,
relative to the distance where its glide began); at the arrival it is 0, is Synced, and the unison steps to 1/k.
Measured against the keeper alone in 10 ms windows, worst over 16 relative phases of two sines (keeper 220 Hz, merger
from 5/4, 0.5 s S-glide; in brackets from 7/4, and 440 Hz from 3/2 over 2 s): minimum while the voices are less than
20 Hz apart (the slow beats one hears as a wobble) / mean level of the merging voice over the glide —
  full gain, 20 ms fade (T7.3)   −28 dB (−24, −41) / −0.1 dB
  linear in time (option b)      −4.1 dB (−2.2, −1.8) / −4.8 dB
  distance                       −3.7 dB (−1.3, −0.9) / −4.3 dB
  distance² (chosen)             −1.3 dB (−0.2, −0.1) / −5.7 dB
  distance³                      −0.5 dB (−0.0, −0.0) / −6.6 dB
  fade between 60 and 30 Hz (c)  −3.4 dB (−2.1, −2.2) / −4.5 … −1.8 dB (and silent throughout for a close interval)
No shape dips in the mean (the keeper stays at 1: expected power 1 + g² ≥ 1). The square is where the slow troughs
are about 1 dB; the cube buys 0.8 dB for a voice another dB quieter. Two voices a comma apart beat at full depth
under every shape — as they did before the glide began. In the engine (4:5 → 4 at 261.6 Hz, one phase): −0.13 dB in
the first half of the glide, −0.25 dB in the second (was −5.4 dB), and the keeper sample for sample after the Sync.
A split is the mirror image. Reversible: the exponent is one line of `jiVoicePlan` (rise / fall).

## Q282 · 2026-11-14 · T7.3 · Flatten tolerance of lowered curves: 1 cent
Context: every breakpoint is a curve point to store and an event on the audio thread; the count goes with
1 / sqrt(tolerance). At 0.05 c (the plan's default for drawing) the 100 × 8 × 32 lowering made 734 k points.
Choice: 1 c while a pitch moves (`kJiPitchTolerance`; holds and straight lines are exact at any tolerance), plus a
1 ms minimum spacing inside curved spans. Reversible: one constant.
## Q283 · 2026-11-14 · T7.5 · "Is Harmonic N" on a note that is not a just ratio
Context: A4 declares a note the N-th harmonic. The usual note is a 12-EDO Degree; the old lock was a Rational of the
reference, so T7.5 locked at the simplest ratio near the note and the guide passed 13.7 cents beside a 12-EDO E.
Answer (user 2026-11-14, T7.5b): the declared note is the frozen anchor and sits EXACTLY on guide N whatever its
identity; its pitch does not change. `ClipFundamental::anchor {note, index}` replaces the Rational lock, and the
fundamental is derived, not rigid: F = the anchor's pitch / (N · m), m the smallest integer that makes every note
related to the anchor by an exact ratio a whole harmonic (anchor 5 + a note 3/2 above it → 10 and 15; 5 again when
that note is deleted). Notes drawn afterwards are Ratio notes anchored to the anchor note.
Status: closed (T7.5b).

## Q284 · 2026-11-14 · T7.5 · What "Is Subharmonic N" shows
Withdrawn (user 2026-11-14: "i dont care about subharmonics, dont include that yet"): the menu item and its code path
were removed in T7.5b.
Status: withdrawn.

## Q285 · 2026-11-14 · T7.5 · Q279 closed: object duplicate and pitch move are document ops
Context: Q279 left them composed in the roll.
Choice: `ops::moveJiObject(…, steps, displayGrid, cents)`, `ops::pasteJiObject`, `ops::duplicateJiObject`,
`ops::deleteJiObject(…, withNotes)`; the roll's raw-command compositions are deleted.
Status: closed.

## Q286 · 2026-11-14 · T7.7 · A Sync happens at the end of its block; the unison's steps at the next block's first frame
Context: the image runs a block once, so a lane's state can only be copied between blocks. The merging voice is at 0
from the merge tick; the keeper keeps its level until the steps.
Options: (a) copy at the block's end, the conditional steps (and the later events of those notes in that block, in
order) at frame 0 of the next block: up to one block (1.3 ms at 64 frames) late, no level change meanwhile; (b) split
the block at the Sync (sample-exact, but every Sync costs an extra image run for the whole track).
Choice: (a). Nothing audible depends on the tick: before the steps the sum is the keeper alone, after them too.

## Q287 · 2026-11-14 · T7.7 · A split's level is carried by the unison's lowest voice
Context: "split level monotonic". If every voice of a unison rose from 1/k as it left, the sum would fall below one
voice once the voices decorrelate (k voices at little more than 1/k each).
Choice: at the tick the split begins the voices that stay with the lowest voice step to 1/(their number) and the
leaving ones to 0 (identical lanes: the sum does not move), then each leaving voice rises with its squared distance:
expected power 1 + Σ g² only rises. Measured (4:5:6:7 → 1 → 4:5:6:7, sine): 0 → +6.0 dB over the glide, largest fall
between consecutive 50 ms windows 0.22 dB (beats inside the window); in 10 ms windows the first slow beats of the
leaving voices are visible, as on the merge. The cost: for the first part of a split the lowest voice is the loudest.

## Q288 · 2026-11-14 · T7.7 · Voices that keep moving together after a Sync are not re-Synced
Context: a Synced voice gets its own Pitch events afterwards (targets computed from its own note Hz and curve), which
can differ from the keeper's by a float32 ulp when both glide on together or ride a curve: bit-identical while they
hold, within about 1e-7 relative in frequency (a beat period of hours) after a joint move.
Choice: leave it (inaudible); a Sync at every joint arrival would restore bit identity at the price of a state copy
per stop and voice.

## Q289 · 2026-11-14 · T7.6 · A bend between notes when the neighbour later moves
Context: A3: Alt + a note's edge makes it glide in from the previous note / out to the next one. The glide is stored as
the note's own pitch curve (breakpoints), its far end computed from the neighbour's resolved pitch when the gesture is made.
Options: (a) baked: the curve keeps its cents; after the neighbour is moved or retuned the glide still aims at the old
pitch until the gesture (or Ctrl+G) is repeated; (b) derived: the curve names its neighbour and the far end is recomputed
whenever either note changes (a model field on NoteCurves, serialisation, a re-derive pass in every note edit and in the
lowering, which another task owns).
Choice: (a), as the plan says: no model change, what is drawn is exactly what plays, and repeating the gesture is one drag
(Alt + the edge again keeps the bend's shape and re-aims it). (b) is the better end state per "derive from general
mechanisms"; it needs `document` + `app/project_link_ji.cpp`. A tuning morph is the visible case: Degree notes retune,
their baked glides do not.
Status: open (say if (b) is wanted).

## Q290 · 2026-11-14 · T7.6 · Bend shapes: Straight and S-Curve, no Bezier handles
Context: the task asked for Straight / S-Curve / Bezier "reusing the object transition shapes and handles".
Choice: a note's bend is baked points, so a Bezier's two handles have nowhere to live (a model field would be needed, as
in Q?-T7.6-a (b)). The menu offers Straight and S-Curve; a Straight bend is two breakpoints with the diamond between them,
which bends the segment (`Breakpoint::curve`), and every point can be dragged. Bezier handles come with (b) if wanted.
Status: open.

## Q291 · 2026-11-14 · T7.6 · Modifiers and what a resize does to a curve
Choices made (all reversible):
- Alt + an edge = bend (Alt on an edge was free; Alt + body still copies). Shift + body = free detune to Cents notes
  (Shift + click still toggles; a selected note is now taken off on release, so a Shift drag can start on it). Before,
  Shift + drag on an unselected note added it and moved by rows.
- Draw Mode: a click on a note still deletes it; a DRAG that starts on a note now draws its pitch curve (it deleted the
  note at the press before). Alt = vibrato, Ctrl + click = breakpoint, only over a note / an object's lowest voice.
- The ride curve is drawn on the object's lowest voice (the thick line), only while no object is entered (entered,
  that press draws a note on the voice). The ride line under the box is unchanged.
- A resize keeps the lead-in on the start and the lead-out on the end; what lies between keeps its place in time and
  is cut (not scaled: a vibrato keeps its rate). Move, duplicate, copy / paste carry the curve unchanged; a pasted bend
  therefore still aims at the pitch it was made for.
- Ctrl+G bends OUT of each note into the next (the tail of the earlier note moves), not into the start of the later one.
Status: open for review.

## Q292 · 2026-11-14 · T7.11a · Bends are derived (Q289 b): which pitch of the neighbour, and cycles
Context: `NoteCurves::bendIn / bendOut` name a neighbour note; the glide's far end is computed where the curve is read.
Choices: (1) the far end is the neighbour's RESOLVED pitch (its pitch at its own start, `NotePitch::hz`), not the
neighbour's own curve value at the moment of the hand-over (for both edges). Following the neighbour's curve at the
meeting tick is one more lookup in `noteBendTargets`; say if wanted. (2) A bend longer than its note is clamped at evaluation (a resize needs no
re-write). (3) A bend whose neighbour leaves the clip is dropped (delete, split); a copy keeps it when the neighbour is
copied too or is a note of the target clip. (4) Bezier bends are stored and evaluated (`JiShape`), answering Q290 on the
model side. Status: decided (reversible); Q289 → (b).

## Q293 · 2026-11-14 · T7.11a · A note anchored to a moving note (M5): at its start only
Context: a Ratio note anchored to a note that glides (Tune to JI, a bend, a drawn curve).
Options: (a) it takes the anchor's pitch where it starts and keeps it; (b) it also follows the anchor's movement during
its own span (a derived curve on the dependant: lowering + drawing).
Choice: (a): one rule in `pitch_resolve`, no new curve, and it is what makes "draw a 3/2 over the tuned third" pure.
(b) would make a held note wobble with a vibrato on its anchor; it fits the planned groups / warps better than a
per-note rule. A dependant that starts after its anchor ended takes the pitch the anchor ended on. Status: open.

## Q294 · 2026-11-14 · T7.11a · What a note becomes when its anchor is deleted or split away (H3)
Choice: exact reference Ratio when the anchor's pitch is one; else the first dependant (without a curve) is written
as Cents and the others stay exact Ratios anchored to it, and "Is harmonic N" moves to it (N · its ratio, when whole),
so the JI structure and the guide lines survive deleting a tempered reference note; else Cents. The reviewer's other
option for a split (copy the anchor into the other piece as a hidden note) was not taken: a second copy of one note
that sounds again when the piece is extended. Status: decided (reversible).

## Q295 · 2026-11-14 · T7.13 · Warps: what moves, and what happens to a tuned member that is edited
Context: `JiWarp {at, glide, shape, toJi}` in group ticks (from the group's earliest note start).
Choices: (1) the glide STARTS at `at` and ends at `at + glide`; warps may not overlap. (2) Only members with a written
identity move; the anchor and members drawn as JI (no written identity) are JI throughout. (3) Group ticks follow the
group's start, so moving the earliest note moves every warp with it (the plan's definition); when the anchor is
deleted or split away the warps are shifted so they stay where they were in the clip. (4) A member whose pitch is
changed by a step, a drag or a type-in loses its written identity (it is no longer the note that was written); a walk
keeps it. Alternative for (4): re-write the written identity to the nearest degree of the new pitch. (5) A promoted
anchor takes its JI pitch (so the other members keep their Hz); if it was sounding in its written state it moves by the
difference. Status: decided (reversible).

## Q296 · 2026-11-14 · T7.13 · Tune to JI: the candidate ranking and its two constants
Context: R3 wants 4:5:6:7 before 20:25:30:36 and both 10:12:15 and 6:7:9 for a 12-EDO minor triad.
Choice: a term may lie up to 34 ¢ from the written pitch (7/6 is 33.1 ¢ from a 12-EDO minor third; less would lose
6:7:9); rank by mean error + 12 ¢ · the mean Tenney height log2(n·d) of the intervals between every two terms. "Largest
term" as the simplicity measure put readings like 18:23:27:32 ahead of 20:25:30:36, hence interval heights. At most 8
candidates. Without any candidate each pitch takes its simplest ratio (the old Convert to Ratio rule). Status: decided
(reversible; two constants in `edit_ops.h`).

## Q297 · 2026-11-14 · T7.13 · "Is Harmonic N" on a note that already is a member
Context: a note is the anchor of at most one group; a member is a Ratio anchored to its group's anchor.
Options: (a) the member also anchors a second group of its own (a chain: it stays a member of the first); (b) the
first group is re-indexed so that this member reads N; (c) refuse.
Choice: (a), the general mechanism (any note may anchor a group). T7.14 may prefer (b) for the menu. Related: when the
anchor of a group is deleted, a note that was anchored to the PROMOTED member becomes a member of that group (membership
is "anchored to the anchor"). Status: open.

## Q298 · 2026-11-14 · T7.13 · The roll between T7.13 and T7.14
Context: the object UI is deleted; the group UI is T7.14.
State: "Is Harmonic…" makes a group; guides and the plain draw use the ACTIVE group (the first selected note's group,
else the one active last, else the newest); "Remove JI Group" replaced "Unlock Fundamental" and detaches the members
(exact ratio / Cents) instead of leaving them anchored. Warps exist in the model and play, but the roll does not draw
them yet (members are drawn at their JI pitch) and has no gesture for Tune to JI, walk, proportions or warps.
Status: open until T7.14.

## Q299 · 2026-11-14 · T8.1 · `Ctrl+CapsLock` toggles the OS Caps Lock state too
Context: the window gets the key after the OS has flipped the lock (the AHK macro suppresses it with a system hook; SDL
has no such hook). Key codes do not change with the lock, so macro keys and shortcuts are unaffected; text typed into a
field afterwards is upper case until Caps Lock is pressed again.
Options: (a) leave it; (b) the app sends a synthetic Caps Lock press after the chord (Windows `SendInput`; an `app`
change, platform code); (c) another default chord.
Choice: (a): the chord is SPEC §6.2's and is data in the keymap. Status: open.
T7.16b (review M12): (b) is built, for every Ctrl / Alt / Win + Caps Lock chord (`pq/ui/chord_lock.h`
ChordLockGuard in the app's event loop: one SendInput tap flips the lock back, the tap's two events are dropped). Checked
by unit tests of the event logic only; the SendInput itself was not exercised by a test (it would flip the test
machine's lock). Status: decided (reversible).

## Q300 · 2026-11-14 · T8.1 · What the tempered layer measures with, and what Up / Down mean
Choice: an EDO maps the composed interval by a val applied to its monzo (the patent val; a preset may list another
per EDO, SPEC §6.2), so every key always moves the same number of steps and 81/80 is 0 steps in 31-EDO; a MOS / scale
takes the degree nearest to anchor + interval. The error shown is composed − mapped (15/8 in 12-EDO: −11.7 ¢).
`s` / `d`: rows on top of the composed interval in the tempered layer (kept until the note is placed, as the trainer's
cursor), the next harmonic where the placed note would be a Ratio (of the group's fundamental when it has one, else of
the cursor's own denominator: the roll's Up / Down on a Ratio note). SPEC §6.2's separate "scale steps" layer (keys =
generator / L / s / period moves) is not built: the task's two layers replace it. Status: decided (reversible).

## Q301 · 2026-11-14 · T8.1 · The JI layer from a free cursor (no anchor note)
Context: an exact ratio needs something to hang on. Choice: with nothing composed `` ` `` places a Degree note on the
row and makes it the anchor (the root); with a composition, the note is a Ratio over the reference when the row is the
reference in some equave, else a Cents note at the exact pitch (labelled "exact"). Status: decided (reversible).

## Q302 · 2026-11-14 · T8.1 · No "preview" preference exists in the roll
Context: the task says the cursor audition should respect the roll's preview preference; the roll has none (clicks and
draws always audition through `RollState::audition`). Choice: every cursor pitch move and every placement auditions
through the same call. A preview switch would be one flag checked there. Status: open.

## Q303 · 2026-11-14 · T7.14 · When a JI group stays "active" (its guides, drawing on its harmonics)
Context: T7.13 kept the last group active for ever (else the newest), so after one Tune to JI every later plain draw
became a Ratio member of that group and only Shift could draw on the rows.
Choice: active = the selected note's group; with nothing selected the group used last stays (a click on empty space
or a deleted note does not drop it, so double-click / Draw Mode keep adding to it); selecting notes of no group, or
Esc with nothing selected, puts it away (guides gone = rows). Alternative: drop it on any click on empty space
(then a double-click can never add a member, since its first click is one). Status: decided (reversible).

## Q304 · 2026-11-14 · T7.14 · Shift + drag on a member: detune, not "rows"
Context: the task's item 5 says "dragging a member vertically snaps to the group's harmonics (Shift: rows)", item 7
says Shift + drag is the free detune (with a dead zone).
Choice: Shift + drag is the free detune for every note, members included (one meaning per modifier); a member goes
back onto the tuning's rows with Ctrl+Shift+J (the group) or the menu's Degree conversion. "Shift: rows" holds for
DRAWING (Shift + draw / double-click puts a Degree note on a row while a group is active). Status: open (say if a
member should snap to rows with another modifier).

## Q305 · 2026-11-14 · T7.14 · Vibrato: the rate in Hz uses the tempo where the note starts; left / right
Choices: (1) a Hz vibrato's period in ticks is taken from the tempo at the note's start and kept for the note (one
number per note in pitch_resolve; under a tempo ramp inside a long note the rate drifts with the tempo). The Beats
unit is in the model and evaluated; the roll writes Hz. (2) Dragging the ~ grip RIGHT stretches the waves (slower),
left squeezes them (faster): the wavelength follows the pointer; a slider would read the other way round. (3) The
selected notes show their grips only while they are ≤ 16 and each ≥ 36 px wide (or vibrating); the hovered note
always. (4) A new vibrato starts with no fades over the whole note; `end` 0 means "the note's end" so a resize keeps
it. Status: decided (reversible).

## Q306 · 2026-11-14 · T7.14 · Warps: which way a new marker points, and "Here" from the menu
Choices: (1) a corner drag never changes what the rest of the group does: a new FIRST warp leads into the state the
group started in (no warps: tempered → JI), a new LAST one leaves the state it ended in (no warps: JI → tempered).
(2) "Back to Tuning Here" / "Into JI Here" moves the NEXT transition to the pointer when there is one (a single warp
dropped in the middle would have to be followed by a second one to keep the rest unchanged), else adds one.
(3) Only groups with written identities (Tune to JI made them) get corner grips and warp entries: a group drawn as
exact ratios has no tempered pitch to go to. (4) Delete with a marker selected removes the marker even when notes
are selected too (the marker was clicked last). Status: decided (reversible).

## Q307 · 2026-11-15 · T7.16a · Back to Tuning as a state of the group (M7 review M1 / M3)
Context: "switch cleanly between an EDO and JI" must keep the chosen reading and the warps. Options: (a) a flag on the
group (`JiGroup::tuned`): the notes stay Ratio members, resolution plays every note with a written identity as written
(`effectiveJiWarps`), so the lowering needs no change; (b) give the notes back their written identities and store the
reading on the group. Choice (a). Consequences, all reversible: while back in the tuning a member stepped alone is the
tempered note it plays as (its written identity stepped, out of the group); drawing never joins such a group; Remove JI
Group keeps what plays (written); a chord typed or drawn in JI gets the nearest step as its written identity when it
first goes back (M3); an anchor keeps a written identity it had as a member (Is Harmonic on a tuned member, promotion).
Status: decided (reversible).

## Q308 · 2026-11-15 · T7.16a · Which drawn notes join a group, and the header click (M7 review H1 / H3 / H2)
Choices: a note joins a group only when it STARTS inside the group's time span [first start − a 32nd, last end) and the
clip box's Harmonics switch is on; the guides are drawn over that span only, for the group under the pointer (else the
active one). Off: rows everywhere, no guides. A click on a group's header only selects the chord (so Up / Down transpose
it); a DOUBLE-click on the proportion opens the readings (an open chooser would take the arrow keys). Ctrl+Shift+J on a
selection splits it into chords by overlapping time; a selection that holds no group note never untunes a group.
Status: decided (reversible).

## Q309 · 2026-11-15 · T7.16a · The mouse path to "3/2 above harmonic 5" (M7 review M5)
Options: (a) Alt over a group shows the next finer series (×2) and the draw / double-click lands on it, renumbering the
group (5 → 10, the fifth = 15); (b) snap to the simplest ratios n/d (d ≤ 4) of the hovered note between the harmonics
(the reviewer's sketch). Choice (a): one predictable set of lines the user sees before clicking; repeated (a note on the
finer series makes the whole group finer) it reaches any n/2^k. Inside a group Alt no longer draws on the background EDO
(outside it still does). Status: decided (reversible).

## Q310 · 2026-11-15 · T7.16a · Warps count from the anchor's start (M7 review M7)
Choice: group ticks are ticks from the anchor's start (negative for a member that starts earlier); deleting, splitting
off, quantizing or start-resizing any other note leaves the warps where they are in the clip, and the builders that
change the anchor (promotion, Is Harmonic on a member) shift them. Moving the anchor ALONE in time moves the warps with
it. No file migration (no compatibility: an old file's warps are read as anchor-relative). Status: decided (reversible).
## Q311 · 2026-11-15 · T7.16b · MIDI export of pitch movement (vibrato, curves, bends, JI glides)
Context: review M21. `exportMidi` writes every note at its start pitch; since T7.16b it counts the notes whose pitch
moves (`ExportMidiReport::curvesDropped`, doc::effectiveNoteCurve not flat) and the status line says "N notes lost their
pitch movement". Options: (a) leave it reported; (b) MPE mode: per-channel pitch bend following each note's effective
curve (flattened as the lowering does, kJiPitchTolerance, ≤ 1000 events / s per note), so MPE synths play the glides and
vibratos; 12edo / Degrees modes keep the static note (one channel cannot bend notes independently); (c) also write
a vibrato as CC1 + a fixed-rate hint. Choice: (a) for now (full pitch-bend export was out of T7.16b's scope); (b) is
the natural next step and needs no new data. Status: open.

## Q312 · 2026-11-15 · bug-ji-input · The frozen note of a JI group moved alone
Context: the user: dragging a group's anchor (the bottom note) moved the whole chord; it must move only that note, and
the chord moves when all of it is selected (the header click). The model has no "anchor moves, members stay": members
are ratios to the anchor. Options: (a) re-anchor the group onto a staying member first (ops::reanchorJiGroup with that
member's number), then step the old anchor as a member; (b) keep the anchor, give it a Cents / exact pitch at its new
harmonic and re-express every member against it; (c) keep moving the chord. Choice (a), inside `ops::moveNotes` /
`transposeNotes` (so drag, Up / Down and Left / Right agree): the lowest staying member with a number carries the
group (as deleting the anchor promotes it), every staying Hz, number and the fundamental stay, the moved note snaps to
the group's harmonics in a tuned group and moves by rows out of the group in one Back to Tuning, all one undo step. The
carrier becomes a Cents note (or the exact reference ratio) with its written identity kept — the same identity change
"Is Harmonic N" on a member makes. A time-only move re-anchors nothing; the group's warps stay where they are in the
clip (this replaces Q310's "moving the anchor alone in time moves the warps"). Status: decided (reversible).

## Q313 · 2026-11-15 · bug-ji-input · Macro mode's next-note length
Choice: Shift + the preset's left / right keys (Shift+A / Shift+F) and Shift+Left / Shift+Right set the length of the
note macro mode places next, a grid step at a time (from its end, as the roll's Shift+arrows measure), at least one
grid step (a 16th with the grid off); in macro mode these keys are the cursor's, not the selection's. The length is
kept in MacroMode for the app session (every clip, macro mode off and on) and is not a macro undo step; before it is
first set a note is as long as the anchor note, else a grid step. Status: decided (reversible).

## Q314 · 2026-11-15 · bug-roll-playhead · Scrub versus zoom in the time rulers
Context: the user asked for Live's scrub area (click plays from there, drag scrubs) in the piano roll's header and the
same in the arrangement / project view. In every one of these rulers the strip off the brace and the flags already
zoomed on a vertical drag and scrolled on a horizontal one (Live's beat-time ruler); the roll has no free strip for a
separate scrub area (its brace strip is the loop brace and the flags). Options: (a) a separate scrub strip (taller
rulers, a new zone to learn); (b) the direction of the drag's first move decides: sideways = scrub, up / down = zoom,
then sideways scrolls as before; (c) a modifier for one of them. Choice (b), in all three rulers alike, with a click
(no move) playing from there — stopped: Play, playing: a jump; the start marker goes there too (as the old click
located), snapped to the grid, Alt unsnapped. Lost: scrolling by a purely sideways ruler drag (the wheel, Shift + wheel,
the overview and a down-then-sideways drag still scroll). Locator clicks still only jump (Live), a click on the
arrangement's loop brace now plays from there (it located before), the roll's brace / flags keep their drags.
Status: decided (reversible: Ruler::onPointer, RulerWidget::onPointer).

## Q315 · 2026-11-15 · bug-roll-playhead · Where a click in a looped clip's ruler plays from
Context: in a looped clip one content position plays once per pass, and positions before the start marker / outside
the loop never play. Choice: the pass the transport is in when it plays that position, else the first pass that does;
content outside what plays is clamped into it (an unlooped clip: start marker … its end; looped: from the earlier of
the start marker and the loop start to the loop end), so the line always lands inside the clip. Status: decided
(reversible: roll::songAtContent).

## Q?-bug-mod-ghost-a · 2026-11-15 · bug-mod-ghost · Which voice a polyphonic ghost shows; above or under the control
Context: a per-voice modulation (an envelope per note into a filter) has one value per sounding voice; the old tap
summed them over the voices (a 4-note chord swept the ring 4× too far). Options: one ghost of the newest voice; one ghost
per voice (≤ 16 translucent handles); the sum. Choice: the newest sounding voice's (`voice_last` on every tap; on a global
value an alias, no cost), for the ring too. Several ghosts are a later option (a tap per voice lane). Also: a panel
element's ghost is painted OVER it (translucent: its plate, a display's grid would cover a ghost under it), a native
engine face's UNDER its live layer (its solid paint stays the last, so its hit-test layout is the knob's). Status: decided
(reversible: lower.cpp kTVoice / kTUVoice; GroupPanelFace::paintLive; the hosts' paintGhosts order).

## Q?-bug-wire-cut-a · 2026-11-15 · bug-wire-cut · What a cut half does on a block's body, a right click, focus loss
Context: X cuts a cable into two hanging halves; the user named inputs, outputs and empty canvas. Options for a click on
a block's body (no pin or control): (a) the source half opens the block's param menu as a drag does, the target half
takes its main output as W's backwards cable does; (b) nothing but a status line. Choice (b): a menu would open inside
the cut's store gesture and its pick come later, and a body click choosing one of the two halves is a guess. A right
click lets go like empty canvas (the held-cable rule); a middle drag and the wheel still pan and zoom. The canvas
losing focus cancels as Esc does (a store gesture must not stay open while another view starts its own). A re-plugged
half is a new cable (default law, amount, reducer): the old wire's settings are not carried over. Status: decided
(reversible: ProjectCanvas::cutPress, onFocusChanged).

## Q?-bug-wire-cut-b · 2026-11-15 · bug-wire-cut · Z over a port: which cable stays a path, where the VCA goes
Context: Z over an input with several cables puts one VCA in front of it. Choice: the cable spliced through the VCA is
a main-path link if one is among them (else a main input's, else the first), so the path stays a path; the others are
deleted and re-wired into the VCA's main input (Add law, the old wire's law / amount on a modulated param is not kept on
them: the VCA → port cable is the spliced one's). The VCA goes into the innermost group holding all those cables (each
one's doc::wireContainer), its output 40 world units out from the port on its input side; where that overlaps a block
(a gain right after an instrument) the overlap rule moves it aside. A port wins over a cable that passes over it; a
port without cables falls back to the cable under the pointer, then to a status line. Status: decided (reversible:
ProjectPatchView::insertVcaAtPort, ProjectCanvas::onKey).
## Q?-VZ.2-a · 2026-11-15 · VZ.2 · Custom colour stops: their colours, and what PQ Meter has that VZ.2 left out
- Context: PQ Meter's Custom ramp has a colour picker per stop. pqdaw's settings sheet has text rows only (drag a number,
  click a choice); there is no colour picker a face can open.
- Done: Custom = six stops whose colours are a chosen colormap ("Custom colours") sampled at the default stop positions,
  the four middle stops placed in dB (fenced 1 dB from their neighbours; moving the colour floor / the Spectrogram's
  floor keeps them). With the default stops a Custom ramp paints exactly its base colormap. Because every preset starts
  at black, G1 (the fade blends toward the Silence colour instead of black) is implemented but not visible until a stop
  colour can differ from its preset.
- Left out (not in the plan's list): PQ's Spectrum Saturation / Hue rotate, saved colormaps, the palette track (LFO),
  the L/R and M/S overlay channel modes, note tracking.
- Options: (a) a small colour-picker popover for the sheet (rows "Silence colour", "Stop 2 colour" …), stored as numbers
  (0xRRGGBB) in the instance data; (b) leave stop colours to the colormap. Choice for now: (b) (reversible: (a) adds
  rows and fields without changing the existing ones).
- SPEC §11.1 still says "level-weighted 3-pass glow, body, bright core"; the Spectrum now follows PQ Meter 1.0.21 (one
  optional glow pass, off by default, then the core). SPEC left untouched (decisions are the user's): please confirm.
## Q?-VZ.4-a · 2026-11-15 · VZ.4 · The loudness faces' custom colours
Context: PQ Meter's three loudness modules have colour pickers (ColorSlot: L / R bars, zone tints, peak cap, clip lamp;
under / on / over target; the M / S / I traces). pqdaw's option sheet (option_sheet.h) has number, choice and toggle rows,
no colour row. Options: add a colour row type (a picker in the sheet) for these and the other analysers; or keep PQ's
default palette fixed. Choice: the fixed palette (PQ's defaults, `ui::signal_theme` for the level meter) for now; a colour
row would serve every analyser and belongs with VZ.2's colour work. Status: open (reversible: an Opt kind + rows).

## Q?-VZ.4-b · 2026-11-15 · VZ.4 · The history across a long silence
Context: the feed stops feeding the loudness meter once it rests (3.5 s of silence and every falling level under −100 dB:
the worker idles, the CPU rule), so the Loudness History's (and the LUFS trace's) 100 ms points stop during a long
silence and resume with the sound: the silence shows as a ~4 – 6 s gap, not its full length. PQ Meter's graph scrolls
through silence. Options: keep stepping the meter (and the worker, and the UI's repaint) at 10 Hz while a history face is
shown; or pause. Choice: pause (a silent DAW costs nothing). Status: decided (reversible: atRest's condition in
pq/dsp/loudness.h, or the feed's rest test for kWantLoudness).

## Q?-bug-mos-sister-a · 2026-11-15 · bug-mos-sister · The mode across the sister crossing; the slider's mapping
Context: the MOS circle's slider (and a Generator automation lane) now run xL ys collapsed – equalized – yL xs
collapsed on one generator line. Options for the mode past the equalized middle: keep the root's chain position (the
explorer's transferModeRotation: the same rotation index, the same note-by-note scale moving continuously, its pattern
with L and s swapped, so 5L 2s Lydian LLLsLLs becomes 2L 5s sssLssL, its darkest mode; the GMN mode number is unchanged,
the explorer numbers both sisters along the fifths) or keep the brightness (Lydian → the brightest 2L 5s: pitches jump at
the crossing). Choice: the chain position (the only one without a jump; automation needs it). The slider is linear in
1 − s/L per half (as the old one: its ticks are the hardness ratios), not linear in cents. Status: decided
(reversible: MosCircle::applyPosition, tuning::setMosGenerator).

## Q?-bug-spawn-resize-a · 2026-11-15 · bug-spawn-resize · Spawn size by zoom: reference and clamps
Context: a spawned block's world size follows the camera (the user chose this, knowing same-type blocks then differ in
world size). Choice: reference zoom 1 (ProjectPatchView::kSpawnRefZoom: today's default sizes were made at zoom 1;
CircuitCanvas zoom is logical px per world unit), scale = 1 / zoom clamped to the resize range doc::kMinPatchScale …
kMaxPatchScale (0.05 … 3) and rounded as an S-resize is (whole percent, the detent at 1×). So the on-screen size is
equal from zoom 1/3 to the canvas's 8×; zoomed out further a spawn is 3× (at zoom 0.25 it shows at 75 %), never bigger
than a block can be resized to (raising kMaxPatchScale would change the saved-file range too). Applies to Tab, a
dropped cable, Tab / Z on a cable, Z on a port; a paste keeps the copied blocks' scales (top-level blocks: a pasted
group's members get scale 1). Status: decided (reversible: spawnScale).

## Q?-bug-spawn-resize-b · 2026-11-15 · bug-spawn-resize · Nudges are offsets from layout slots: what still shifts
Context: a block's place is its slot in the layout (the main path's series, the free row in list order, a capsule's
circuit) plus its nudge, so any change to a block before it shifts it. This task removed the shifts a resize causes
(the layout uses the blocks' own sizes) and holds the blocks in place around a spawn and an edge stretch
(ProjectPatchView::keepPlaces). Still shifting: blocks after a deleted one on the free row or the path, after a block
moved to another track, and the shifts of a spawn or stretch come back on its undo (nudges are view state, not undone).
Also by design (TL.7 / bus layout, not blocks): the mixers follow the lanes' right end and the returns' / master's
buses sit right of the tracks' slots, so a block placed far right or added to a row moves those. Options: (a) as now;
(b) store a placed block's position relative to its band / capsule (a `placed` flag in doc::PatchNudge, the slot only
for blocks never placed): nothing moves a placed block but a push or the user, undo included — a format change of
the view state and every nudge writer. Choice (a) for this task (scope); (b) is the general fix if those shifts show.
Status: open.
## Q?-VZ.5-a · 2026-11-15 · VZ.5 · What the stereo faces leave out of PQ Meter
Context: PQ's Stereo Image has per-stop colour pickers and a line colour, the Stereo Field per-trace / bar colours, the
Phase Scope mono / coherence colours and six FFT windows; it also computes a group delay it never draws. pqdaw's sheet has
no colour row (Q?-VZ.4-a). Choice: PQ's palettes as fixed choices (the Stereo Image's six ribbon palettes with their
crossovers editable, the ten colormaps + Custom stops in dB on VZ.2's LevelAxis), Hann / Kaiser windows only, no group
delay. Status: open (reversible: Opt rows; dsp::CrossSpectrum can add the delay from its bins).


## Q?-VZ.6-a · 2026-11-16 · VZ.6 · What of PQ Meter's Band Levels / Waveform was left out, and the crossover tree
- Context: PQ Meter's multiband RMS and waveform have colour pickers (per band; the waveform's two lane colours and its
  clip colour), saved colormaps / a palette track, and the waveform's DAW link (bar numbers from the host's clock).
- Done: the colours are PQ's defaults (its per-count band ramp; its theme lane colours; the clip red), the waveform's
  "Custom" colour source is folded into "Theme" (no picker in the sheet: the same reason as Q?-VZ.2-a); the loop's
  clock is the transport's beat on the tap (pqdaw's own, so no link); bar numbers on the scroll are not drawn.
- Crossovers: PQ's bands are each HP(lower) · LP(upper) (no all-pass compensation); pqdaw's is the full LR tree (band k
  = HP(x0 … xk−1) · LP(xk)), whose levels are the same to within the extra high-passes' passband (inaudible on a level
  reading) and whose bands, with each band's all-pass compensation, sum flat (tested; the meter skips the all-passes:
  they change no band's level). Choice: keep the tree (reversible: one function).
- Options for the pickers: as Q?-VZ.2-a (a colour popover for the sheet) or leave the colours fixed.

## Q?-VZ.7-a · 2026-11-16 · VZ.7 · What the 3D faces leave out of PQ Meter
Context: PQ's Waterfall offers six FFT windows and an 8x long window; both 3D modules carry a modulation bank (LFOs on
yaw / pitch / zoom / fade …), a palette track and colour pickers; the plain wheel zooms. Choice: the Waterfall's analysis
is VZ.2's spectrum analysis (its window, its 4x long window, its tilt: one analyser in pqdaw, the Spectrum and the
Waterfall agree at yaw 0); no modulation bank (pqdaw's own modulation system is the place for camera LFOs); colormaps +
Custom stops in dB (VZ.2's LevelAxis) instead of pickers / palette track; Alt+wheel zooms (the plain wheel pans the
circuit / panel host). The Stereo Spectrogram's cloud keeps PQ's stray sidelobe points (≤ −57 dB under a tone, measured).
Status: open (reversible: an analyser window option; a modulation hookup; Opt rows).

## Q?-VZ.10-a · 2026-11-16 · VZ.10 · The colour row: storage, alpha, where the rows live, what is still fixed
- Storage: one number per colour, −1 = the face's own (follows the theme / colormap, as PQ's ColorSlot), else 0xRRGGBB;
  only chosen colours are written to the data. Alpha is the face's (its fades, tints and level opacities keep working),
  so the picker has no alpha strip (PQ's slots have none either). Options: add an alpha strip per row where a colour is
  painted flat. Choice: none (reversible: a flag on Opt and a strip in the picker).
- Rows: a third tab, COLOURS, on faces with colours (the colormap / colour-mode rows repeated there, the Custom stop
  colours when the colormap is Custom; DISPLAY unchanged). Options: the colour rows inside DISPLAY (crowded: the
  Spectrum's DISPLAY already wraps to 3–4 columns). Choice: the tab (reversible).
- Wired: Custom stop colours (Spectrum, Spectrogram — stop 1 is the Silence colour, so G1's fade now shows —, Stereo
  Image, Waterfall, Stereo Spectrogram), Spectrum / Waterfall Solid colour, Level Meter (L, R, both zones, peak cap,
  clip lamp), LUFS Meter (under / on / over, history trace), Loudness History (M / S / I traces, under / on / over),
  Band Levels (bands 1–5), Waveform (two lanes, clip), Stereo Field (trace, correlation + / −, three bands), Stereo
  Image (five ribbon colours over the palette, position line), Phase Scope (mono, coherence, warning). Still fixed: the
  Oscilloscope's traces (PQ cTraceA/B; its face has its own sheet code), the Stereo Spectrogram's band-ramp colours, the
  Spectrum's L/R/M/S (those channel modes were left out, Q?-VZ.2-a). Status: open.

## Q?-VZ.9-a · Double-click on a Choice / Toggle row of an analyser sheet
- Context: review L1: the first click of a double-click cycles the row (one undo step), the second reset it (a second step).
- Options: (a) the double-click's second press does nothing on a choice (one change, one step); (b) merge the reset into
  the first click's step (needs an undo-merge API); (c) keep the reset as a second step.
- Choice: (a). Numeric rows still reset on double-click; Colour rows reset (their first click only opens the picker). The
  hint text no longer promises a reset on choices. Reversible: one branch in `AnalyserFace::rowDown`. Status: open.

## Q?-VZ.9-b · When an analyser face leaves its small text out
- Context: UI note 4: at project-patch zoom the faces drew 6-unit labels at ~5 px (the threshold was 5 device px).
- Options: keep 5 px; 6 px (as the Oscilloscope and Spectrogram already use); scale the text up instead.
- Choice: 6 px, and where the text goes a face shows one legible summary (`AnalyserFace::summary`, ~12 device px): Level
  Meter the louder peak + ticks at 0 / -12 / -24, LUFS Meter the short-term with its unit, Loudness History the
  short-term now. Band Levels and Waveform show only their traces zoomed out. Reversible: one constant. Status: open.
## Q?-bug-wavetable-a · 2026-11-16 · bug-wavetable · Fold ×3: the knob's law, and the bake past the old range
Context: fold now reaches θ = 12π (`dsp::kFoldMaxTheta`, ≈ 24 folds of a full-scale input), 3× TMM.1's 4π. The knob
had to keep the old range usable. Options: a linear knob (the old range on the first third: 3× as twitchy), a
two-segment custom curve (linear to 1/3 at half travel, then steeper: `meta::ParamSpec` declares presets only), or one
exponential segment. Choice: `@curve(exponential, 2·ln 2)` — half the travel is fold 1/3 (exactly the old fold 1), the
start finer than before per unit of θ, the middle ≈ 2.8× the old knob's sensitivity. As every curved param, a wire into
fold now adds in knob travel (the generic rule), so an LFO into fold is finest at the low end. Also: Shape mode's
static bake (the only path still using `frameBakeResolution`; Phase mode bakes through the live band) needs ≈ 320× for
sync 0.5 + fold 1 and is capped at 64× (−72 dB vs 128×, recorded beside sync 1 + pw 1); fold alone at 1 is −189 dB,
and everything up to the old range is unchanged. Status: decided (reversible: the fold ParamSpec's curve;
kMaxAutoBakeResolution).

## Q?-bug-wavetable-b · 2026-11-16 · bug-wavetable · "Per band bending"; how nodes persist
Context: the request says "apply nodes and per band bending". Read as a bend per segment (band) between nodes, the
knob-curve editor's gesture; a spectral reading (bending a band of harmonics) is a different feature, not built.
Nodes are stored per keyframe (`nodes`, a dsp::Curve as floats) only as an editing aid: the 2048 samples stay what
sounds; any other edit drops them, and stored nodes that no longer render the cycle are ignored (refitted). Entering
the node tool fits nodes to the drawing (≤ 12 segments, 3 % of full scale; noisy cycles hit the cap), shown dashed until
the first node edit applies them — rather than a default shape, so entering never changes the sound. Status: decided
(reversible: the face's ensureNodes / fitNodes).

## Q?-MX.2-a · 2026-11-18 · MX.2 · How the Math module stores its op and builds its inputs
Context: the plan's semantics name a string setting `math.op` and "A wire ADDS to the typed value (the wire law)".
Device settings are numbers and names may not hold a '.', so the group setting is `mathOp` holding the op's stable
`code` (MathOpInfo::code; never reused). A and B are child groups ("A": In + value A; "B": its own In — the module's
second input, a cable into the child group — + value B) summed by one `add` each, not wires into the value blocks: the
value block's wire law clamps to the knob's range and costs the curve stage and a modulation tap per sample, the `add`
keeps ±1e18 and costs one op. The op subgraph is the group's direct atoms and unroled value blocks; a hand-edited
circuit keeps showing the stored op (no recognition pass), and picking an op rebuilds it. Logic is written on reals
(min / max / compares): a wire into a bool input lowers Invalid, so `and` = min(A, B) > 0 etc. Status: decided
(reversible: modules.h mathBlocks / mathOpBlocks).

## Q?-MX.2-b · 2026-11-18 · MX.2 · One trace tap instead of three scope engines
Context: the plan asks for three oscilloscopes (A, B, Out) sharing one trigger and timebase, the mechanism the worker's
call. Chosen: a general panel mechanism — a `scope` element bound to two or three Signals is one signal_tap of beat and
channel 0 of each signal (panel.h "Trace taps"): one ring, so the traces are the same samples and one trigger / time
base derived from A applies to all; no Oscilloscope engine devices in the group (an unwatched tap costs one relaxed
load per block plus its inputs: measured equal to an off-screen Oscilloscope). Consequences for MX.3: the views'
signal feed must request `traceTapPath(element)` (today they request "#signal:<port>" for slot 0: the minimal panel's
scope stays empty until MX.3 binds it); traces are mono (channel 0: a stereo A shows its left); the trace tap has no
note channel, so a pitch-locked timebase uses the detector, never the note; in an oversampled Math module the tapped
A and B cross the region edge, so their half-band downsamplers run even unwatched. Status: decided (reversible:
panelTaps / panelTapsOf).

## Q?-MX.2-c · 2026-11-18 · MX.2 · "No NaN / Inf out" and huge inputs
Context: the plan's edge rules (÷, mod, pow, wrap / fold / clip with B = 0) are implemented and tested at ±1e18; pow is
capped at ±FLT_MAX and mod / wrap / fold stay in range when the quotient overflows. A × B, A + B, A − B and hypot are
plain float arithmetic: with |A|, |B| beyond ≈ 1e19 (hypot, ×) or ≈ 1.7e38 (+, −) they reach ±inf as any float
product does. Capping them would cost a clamp per sample on the commonest ops for inputs no audio signal reaches.
Status: open (cheap to add a final clip per op if wanted).

## Q?-MX.3-a · 2026-11-18 · MX.3 · The face's In A / In B / Out terminals are markers, not pins
Context: the sketch puts In A / In B in the top corners and Out bottom centre. A closed group's pins are placed by the
project patch on the node's edges (crossing ports; a wire into a control lands on the element showing it), and B's
input is the child group "B"'s main input, which has no pin on the closed node until a wire crosses into it. Options:
(1) captions + jack dots on the face, pins unchanged; (2) a new "input jack" element that the project patch anchors
pins to (a general mechanism like the control anchors); (3) give B a group-level input port. Chosen: (1), the most
reversible — nothing in the patch model changes. Wiring into B today: open the module and cable into B's In. (2) is
the natural follow-up if the user wants to plug B on the closed face. Status: open.
MX.4 (2026-11-19; user 2026-11-18: "wiring into b is absolutely necessary"): option (2) built — a general panel element
`jack` (Input or Signal binding) that the project patch puts the closed block's pin on; the Math face's three markers are
jacks. Status: resolved.

## Q?-MX.3-b · 2026-11-18 · MX.3 · A multi-signal "scope" element is the trace scope, not the Oscilloscope section
Context: MX.2 made a `scope` element bound to 2–3 Signals one trace tap; the views drew every `scope` as the
Oscilloscope face's section, which reads a single "#signal:" tap (beat, note, L, R). Chosen: the `scope` factory makes
a trace scope when `doc::elementTraces` is non-empty (style `trace` = which slot, `scale` auto / gate), fed by the new
`PanelTarget::traceFeed(element)`; every trace scope of one feed shares one window and one ScopeTracker frame on slot 0
(the alignment). The full Oscilloscope options (timebases, freeze, measure) are not on the trace scope. Also fixed on
the way: the circuit canvas took the key focus back after a face's press even when the press had opened a popup that
took it (the radial menu then got no typing); it now keeps a focus the press moved. Status: decided (reversible:
trace_element.cpp, engine_elements.cpp makeSection).

## Q?-BR.A-a · 2026-11-19 · BR.A · Where a module's oversampling lives once the inspector column is gone
Context: TX.group-oversample gave every group's Device-view frame an inspector column with an Oversample menu; on
modules (EQ, Saturator, Utility … — groups that show their own panel) it doubled the EQ's own menu and squeezed every
panel (bug report 2026-11-19_18-57-24). Options: (1) inspector only on instrument groups and panel-less groups; a
module's oversampling from its panel's menu where it places one (EQ, EQ Band, Math) and from every group's title-bar
context menu (Oversample ▸); (2) add an Oversample menu to every built module panel; (3) keep the inspector, narrower.
Chosen: (1), the most reversible (no preset or panel changes). Saturator / Utility / LFO … now set oversampling only
from the context menu; (2) is the follow-up if the user wants it on their faces. Status: open.

## Q?-BR.A-b · 2026-11-19 · BR.A · A refused feedback loop is cut, not compiled
Context: a feedback loop through an oversample region (an FM loop through a Wavetable at 2×) or across the voices' sum
cannot run per sample; the scheduler refuses it, and that failed the whole project compile (bug report
2026-11-19_19-19-59: every later edit and every new visualiser's tap never played). Options: (1) cut one wire of the
loop as WI.2 cuts a type error (red cable, message, the rest plays); (2) run such a loop with a block of delay; (3) run
the loop's region at the base rate. Chosen: (1) — general, no DSP change; the loop is silent until the user moves it
inside or outside the oversampled part. (2) / (3) would make the patch play but change its sound. Status: open.

## Q?-BR.C-a · 2026-11-19 · BR.C · Which Envelopes behind Out hold the release
Context: an Envelope with a block after it (EQ, fold, Utility) used to get the group's 5 ms ramp at note-off, cutting
its release (bugs 2026-11-19_20-08-09, 19-56-21). Now the ramp waits for every voiceEnd member found back from Out
along per-note SIGNAL inputs (not params, not past an Envelope). That also counts an Envelope into a Wavetable's input
(its phase), so such a voice now sounds through that Envelope's release before the 5 ms ramp (golden
lower_meta_voice_run.txt moved). Options: (a) any signal input (chosen: general, derived from wires); (b) only inputs
a type marks as "amplitude path" (per-type rules); (c) also params. Choice: (a). Reversible: lower.cpp endersBehind.

## Q?-BR.D-a · 2026-11-19 · BR.D · A paste over notes of its pitch overwrites them
Context: Ctrl+C now puts the clip view's insert marker at the copied notes' start, so Ctrl+C, Ctrl+V lands exactly on
the originals (bug report 2026-11-19_20-02-29, Live's behaviour); added as plain notes that stacked a duplicate on
each. Options: (a) keep both; (b) a pasted note overwrites the notes of its pitch it overlaps (Live); (c) refuse exact
duplicates. Chosen (orchestrator): (b) — an existing note within 0.5 ¢ of a pasted note's pitch keeps its first
uncovered part (end or start trimmed, free curve retimed) or is removed when covered whole, in the paste's undo step;
copy + paste in place is then a no-op (the copies replace the originals). The rule is RollState::overwriteCommands
(general: covers + pitch). Note: no existing "drawn / moved note over a same-pitch note" rule was found in the clip
view (moves and draws still stack); they could adopt overwriteCommands. Status: decided.

## Q?-BR.B-a · 2026-11-19 · BR.B · A feedback loop across an oversample region runs inside the region
Context: since BR.A such a loop (an FM loop through a Wavetable's or Math's region, a delay's feedback through an
oversampled group) was cut, silent (Q?-BR.A-b). Options: (1) run the loop at the outer rate per outer sample, its
converters inside the loop (both backends would need mixed-factor loop regions; the converters' latency and phase sit
in the feedback path); (2) move the loop's outside nodes into the region (infer(), before the converters: a loop runs
at one rate), so its delay — the wire's z1 or a delay line — is one INNER sample and the converters stay outside the
loop; (3) keep the cut. Chosen: (2): no backend or kernel change, the loop sounds closer to delay-free feedback; its cost
is the moved nodes at n× (a few ops in an FM loop; a whole delay device when its feedback runs through a 2× Math).
Only a loop through an engine node (a voice reducer, a send, a clip player) is still cut. Status: decided (reversible:
infer.cpp joinLoopsToRegions).

## Q?-BR.B-b · 2026-11-19 · BR.B · Math's clip with B = 0 silences (bug report 2026-11-19_19-50-41)
Context: "math breaks feedback loops with delays; just doesn't feed back at all". The report's loop (Delay Tap → EQ →
VCA → Math → Delay Tap) compiles and runs — no oversampling in it, nothing cut. Its Math is set to `clip` (A to ±|B|)
with B at its default 0: everything is clipped to 0, so nothing comes back. Options: (1) leave it (the op does what it
says); (2) give clip (and other ops where B = 0 is degenerate) a B default of 1 when the op is chosen and B is still at
the default; (3) show the clip range on the face. Chosen: (1) for now — a per-op default is a design choice for the
user; (2) is small (`ops::setMathOp`) if wanted. Status: open.
MX.4 (2026-11-19): (2) built. Each op has `MathOpInfo::bDefault`: 1 where B = 0 makes it a constant or near-constant —
×, ÷, pow (A^1 = A), and / nand (B true), clip / wrap / fold (±1: an audio signal passes), atan2 (atan A: a soft clip);
0 for the rest (compares, +, −, mod (B = 0 → A), avg, |A − B|, min / max, or / nor / xor / xnor, hypot, not A). A Math
spawned with an op starts at its B; `ops::setMathOp` moves B to the new op's default only while B is still the old op's
(compared in units to 1e-6), in the swap's one undo step; a B the user set is kept. Status: resolved.

## Q?-BR.E-a · 2026-11-19 · BR.E · Clip keys in the audio clip view; missing audio clip hotkeys
Context: a double-click on an audio clip focused the piano roll's note area, which is hidden while an audio clip is
shown, so every key after it (Ctrl+E, 0, R in bug report 2026-11-19_20-34-17) did nothing. The audio clip view has no
keymap scope of its own. Options: (a) the arrangement keeps the keys after a double-click on an audio clip (its clip
keys then act on that clip); (b) focus the audio clip view (as the project patch view's band lanes do) and give it the
"arrangement" scope or a scope of its own. Chosen: (a), the most reversible; the project patch view still focuses the
audio clip view (no clip keys there). Also open, not added (each a new binding = a design choice): Live's Crop Clip
(Ctrl+Shift+J — taken by pianoroll.tuneToJi), Create Fades (Ctrl+Alt+F), Quantize audio (Ctrl+U on warp markers),
a warp on/off key, and transpose / gain nudges from the keyboard (today: the clip view's knobs, typed values and the
lane's gain handle). Status: open.

## Q?-MX.4-a · 2026-11-19 · MX.4 · Math B's reset value and jack visibility
Context: (a) a value block's reset (double-click / TX.reset-default) goes to its `defaultPosition`, set when the module
is spawned; no command changes it, so after an op swap B's reset still goes to the spawn op's default (0 for the default
`>`), not the new op's. (b) The project patch reads `jack` elements from the document panel and does not evaluate their
visibility condition. Options for (a): a SetDefaultPosition command (a new document command) so setMathOp also moves
the reset value; or leave it. Chosen: leave both (most reversible; no new command). The jacks of the Math face have no
condition. Status: open.

## Q?-BR.H-a · 2026-11-20 · BR.H · How small blocks dropped into a group may get, and the S-resize undo step
Context: the user asked that a selection put into an instrument group "shrink them down and try to fit them in"; the
task said the group grows only below "a readable minimum scale". There was no such constant. Options: the canvas's text
threshold at the current zoom (kNearZoom / zoom: 0.75 at zoom 1, little shrinking), a fixed fraction, or none. Chosen:
`ProjectPatchView::kFitMinScale` = 0.5 of the blocks' size before (~5.5 px text at zoom 1); below that the group grows
round them. Also: the task asked for an S-resize to be "one undo step"; TX.group-resize (and Q240 (3) for blocks)
decided a scale is saved view state with no undo step — kept as decided.
Decided (user 2026-11-20: "the smallest shrink should be 0.1x"): `kFitMinScale` = 0.1 (BR.J). The S-resize without an
undo step and Q?-BR.H-b stay as chosen (the user did not object). Status: decided.

## Q?-BR.H-b · 2026-11-20 · BR.H · The Device view's group panel shows the groups inside as the circuit does
Context: block places in a group are now stored once per block (view state) and every view honours them. The Device
view's group panel opened every group inside its group (an instrument's modules showed their atoms) while the circuit
shows a module closed as its panel, so one place served two layouts of different sizes (blocks overlapping in one view
or the other — part of the "30x" growth). Options: per-view places (more state), places only in the circuit (moving a
block in the Device view would then not persist), or the group panel showing closed groups closed. Chosen: the last
(one line in `Builder::closedGroup`; the scope group and those holding it stay opened) — the panel now matches the
circuit (11 device goldens re-recorded). Status: open.

## Q?-BR.I-a · 2026-11-20 · BR.I · Mini circuits count toward the Device view's height at zoom 1
Context: panels now draw 1:1 and the Device view's auto height follows the tallest frame, group frames included. A group
without a panel (Basic Synth's instrument group) shows its mini circuit; at zoom 1 it is ~990 × 330 units, so the default
instrument alone takes the view to its cap (half the body) and is wide (the chain scrolls both ways). Options: count the
circuit at zoom 1 (as the design says: "including group panels"); or keep a mini circuit fitted to the view (it zooms
itself with Ctrl+wheel) and count only real panels. Chosen (orchestrator, 2026-11-20): the second — a mini circuit fits
the frame's height (zoom ≤ 1) and does not count toward the auto height; only real panels (and the inspector) draw 1:1
and count. Status: decided.

## Q?-BR.I-b · 2026-11-20 · BR.I · A free-rack cable's end lags its port's orbit
Context: the cable layer computes ends on store events and after layout passes. When a new wire makes a free-rack block's
orbiting port turn toward its cable, nothing lays out afterwards, so the cable starts at the port's old place until the
next layout (seen in "wiring: a drag from a jack onto a knob" while the mini circuit drew at zoom 1 and put the knob right under the
LFO; with Q?-BR.I-a's decision the test's geometry is back and it passes unchanged). Options: the free rack's canvas notifies the layer when its scene's
ports move; or the layer refreshes once per frame after a store event. Chosen (BR.J): the event — `CircuitCanvas::
onPinsMoved` fires when its pins move outside a layout (a new scene, a pan / zoom / fit, a port's slide step, hold or
snap); the free rack forwards it (`DeviceChainView::onRackPinsMoved`) and the cable layer refreshes, only while a drawn
cable ends in the open rack and not mid layout pass (`UiTree::inLayout`: onLayout follows). No per-frame cost at rest.
The project patch view and opened groups draw their own cables inside the canvas (they already follow the slide); the
Device view's group frames and panels end cables on widget rects, which move only by layout. Status: done.

## Q?-AP.1-a · 2026-11-20 · AP.1 · Arp readings of Offset, Steps, the first synced step, one degree table
Context: AP.md leaves four details open. (1) Offset: "rotate the pattern start" vs the clock's "plus Offset". (2) Steps:
copies of the pattern or transpositions beyond the first. (3) Where the first synced step falls when a note arrives
between grid steps. (4) Degrees needs a step table; a track patch may have several tuning nodes. Chosen (reversible, in
arp.h): (1) Offset rotates the cycle by n steps (Live's); the grid stays at multiples of L from tick 0. (2) Steps =
1 … 8 copies, copy j shifted j · Distance (Steps 1 = no transposition). (3) The first grid step at or after the note-on
(stopped: the free clock starts at the note-on). (4) One `ArpDegreeTable` per stage, from the track's main tuning (AP.2
builds it); a note without a Degree identity shifts from round(noteDegree) and plays static once shifted. Status:
(3) **decided** by the orchestrator (2026-11-20, done in AP.3): a note-on into an IDLE arp (nothing held, no generated
note sounding) between grid steps sounds its first step AT the note-on (live feel), gate · L long and, with a gate
≤ 100 %, ended at the next grid step at the latest; later steps stay on the song grid; a note-on exactly on a grid
step is unchanged; a note-on while the arp is busy waits for the grid. (1), (2) open. (4) **decided (overruled)** in
AP.8a (review H4, task): one table per tuning NODE (engine::ArpTables: the event's voice.tuning picks it, the track's
table for notes without a node), so a clip with its own tuning steps in its degrees; a note without a Degree identity
keeps its `tuning` when shifted (a reference-relative note follows the reference) instead of playing static.

## Q?-AP.3-a · 2026-11-20 · AP.3 · Arp pitch: readings of the Harmonics wrap, undertones and glide overlaps
Context: AP.md leaves details of the Harmonics unit and the glides open. Chosen (reversible, documented in arp.h):
(1) Rotate in Harmonics wraps by the series' octave (h → 2h). (2) Harmonics below the first (h + k < 1) is a rest.
(3) Harmonic Walk Under elements carry the exact ratio but harmonic 0 (an undertone is not a harmonic of the anchor).
(4) One Time / Shape / Bend for Into and Out (as the plan says); a note fraction is of the generated note's length;
Into + Out longer than the note get half each. (5) Out → Step aims at the step already sounding (gate > 100 %), else
at what the pattern plays next as known at the Out start (a later retrigger / new note can differ). (6) During the
Out glide the source's later bends do not move the note; during Into they re-aim the glide (same end). Status: open.

## Q?-AP.4-a · 2026-11-20 · AP.4 · Arp pulse mode: readings of h, Steps, the idle rule and Layers
Context: AP.md / the task leave details of Pulse mode open. Chosen (reversible, documented in arp.h "Pulse mode"):
(1) Base is the arp's own Rate (synced or ms), not a second rate param. (2) "Its index in a chosen proportion": the
note's rank by Hz among the held notes (not its scale degree), the list repeating doubled past its length
(prop[r mod len] · 2^(r div len)); Proportion with an empty list falls back to Order (rank + 1); h clamped 1 … 1024.
(3) Steps: pulse j of a cycle plays copy j mod Steps (shifted (j mod Steps) · Distance), restarting with the cycle;
Rotate reads as Shift (no pattern to rotate). Style, Offset, Repeats, Groove and Retrigger Beat do not apply.
(4) The idle-start rule per clock: every newly added note's first pulse sounds at its note-on when that is off its
grid (also when other clocks are running), gate · its period long, ≤ 100 % ended at its next onset. (5) Cycle Bars:
whole notes (4/4; no meter in the tempo map), a clock's last pulse before the restart may be short. (6) Into / Out
→ Step = the same clock's previous / next pulse. (7) Layers apply in both modes (pattern and pulse), at (p/q)^L
of the Ratio unit's p/q, gain = level × the note's gain. (8) A held-set change that moves h_ref (the lowest held)
re-times every clock at that frame (each resumes at its first unplayed onset of the new grid). Status: open.
Decided (AP.2b, 2026-11-20): (5) Cycle Bars follow the song's time signature (StageClock::signatures; a signature
change: cycle c = bars c · n … (c + 1) · n − 1; free clock: the bar at the playhead). The rest stays as chosen.
Decided (AP.8a, review / task, 2026-11-21): (8) Pulse Ref defaults to a FIXED 8 (the Harmonics Base: an 8:9:10:12 chord
plays as before), so releasing the lowest note never re-times every clock; 0 ("the lowest held") stays available.
builds it); a note without a Degree identity shifts from round(noteDegree) and plays static once shifted. Status: open.

## Q?-AP.2-a · 2026-11-20 · AP.2 · Cables into a note device's params; a tuning drag republishes the chain
Context: a note device is not in the graph (the lowering dissolves it; its params are project param slots read at the
stage's block start). (1) A cable (an LFO, a macro's value block) into one of its params would need the graph's
modulated value carried to the stage (a bus the engine hands NoteStageBlock, one block late). (2) The degree table is
built from the track's tuning when the chain is published; a MOS generator drag changes it every step. Options: (1) a
per-param bus read by the stage, or leave cables out; (2) republish per tuning change, or look degrees up live (AP.3
"tuning-table lookup under a modulated tuning"). Chosen (reversible): (1) such a cable is Inactive (drawn dim; automation,
knob drags and type-in work); (2) the link republishes the chain on a tuning param change (the handover keeps held keys
sounding, the pattern restarts) until AP.3's live lookup replaces it. Status: open.
Decided (AP.2b, 2026-11-20): (1) a cable into a runtime non-bool param plays at param rate, ONE BLOCK LATE (the
source exported as a bus, its last sample per block a bus value the stage adds in the param's units at its next block
start; lower.h "Note devices"); settings / bools stay Inactive. (2) a tuning edit never republishes the chain: the new
degree table goes to the running stage (ArpTableMailbox; the table of the definition the tuning node plays
statically), sounding steps retune at once, the pattern keeps its place; a structural republish hands the running
state to the new chain (NoteStageChain::takeOver). Status: decided.

## Q?-AP.2-b · 2026-11-20 · AP.2 · Note-device knobs are linear over the declared range
Context: the stage reads a slot's knob POSITION and maps it with one line (value = min + position · (max − min)); a
custom knob curve would need the curve in the stage. Rate ms (1 … 10000) and Velocity Decay ms (0 … 10000) are coarse on
a linear knob. Options: curves in the stage; per-param exp mapping in the table; linear for now. Chosen: linear, own knob
curves refused on a note device (checks.cpp); AP.6's panel can show ms on an exp scale by adding a curve kind to the
table's mapping. Status: open.

## Q?-AP.5-a · 2026-11-20 · AP.5 · Step lanes: readings of the lanes
Context: AP.md "Step lanes" names the lanes but not their arithmetic. Chosen (reversible, arp.h "Step lanes"):
(1) Velocity, Gate and Glide lanes SCALE (Velocity × 0 … 1, Gate × 0 … 2 then clamped 0.01 … 2, Glide × the glide's
pitch distance in log Hz); velocity source Step Lane uses the lane value itself (lane off: as played). (2) Transpose
adds integer steps of the pitch unit. (3) Skip is a rest: the pattern still advances (lanes and pattern stay on the
grid). (4) Tie: the previous step's notes continue (same rank / layer; Pulse: the same clock), their end moves to the
tie step's gate end; with Into on (any source) they glide to the tie step's pitch over the glide time × the Glide lane
(0: a legato jump), Into off keeps the pitch; a tie with nothing to continue plays normally; a note waiting for a tie
whose step does not play (chance, nothing held, Repeats) ends at that onset. (5) Lane position = grid step index mod
length (synced: the song grid; free: the free clock), not the pattern position: lanes do not restart on a retrigger.
(6) Lane steps are 192 rows of kArpParamInfo (runtime) read only when their step plays (ArpStage routes them to the
core); the generic device panel lists them until AP.6's step editor. Status: open.

## Q?-AP.6-a · 2026-11-21 · AP.6 · Arp panel: what one visibility source cannot say, and what the pattern view shows
Context: a panel element's Visibility names ONE source element (a chain is an AND), so "Base shown for the Harmonics unit
OR Harmonic Walk" cannot be said. Options: an OR in Visibility (document + file format + panel editor), two Base
controls, or one rule. Chosen (most reversible): Base shows with Unit Harmonics only; the walk row (Range / Under /
Order) with style Harmonic Walk — a walk in Degrees / Ratio still uses Base for ungrouped notes, set it by switching the
unit to Harmonics first. Also chosen: (1) the pattern view shows the SOURCE notes' Hz in play order (before Distance /
Steps copies, Transpose lane and Layers; Random styles: the elements by Hz, the drawn one lit; Pulse: the held notes by
Hz, the pulsing one lit), ≤ 64 entries; (2) the proportion's 8 values all show with source Proportion (not only the
first Length of them); (3) a lane's current step is lit only while the lane is on. Status: open.
Decided (orchestrator, 2026-11-21): the Base gap blocks the walk's main use, so Visibility takes several sources
(`more`, any-of; `all`: all-of) for every panel, and Base shows for Unit Harmonics OR style Harmonic Walk. (1)–(3) open.

## Q?-SM.2-a · Sampler engine: readings taken where the plan left room
Context: SM.2 (docs/plans/SM.md) — the slot, `sample_read` on slots, the `sampler` type. Choices (most reversible):
1. **Mip level**: one level per call and slot entry, from the SLOWEST playing lane's |rate| (a voice group of 8 shares
   it): a lane far above the others aliases a little more, none is ever dulled. Per-lane levels need one buffer for all
   levels (gathers from one base); not worth it until heard.
2. **`sampleBpm` is a param**, not data: Re-Pitch needs it in the expansion, and data never reaches a body.
3. **Slice Mono / Poly (choke)** is not in the engine: it is the group's polyphony (SM.3's preset); the engine is per voice.
4. **Classic "gate holds the sustain loop"**: the loop keeps playing through the release (the envelope after the engine
   releases it); no "loop off at note-off" (a per-sample gate into the param-rate loop points would make them audio rate).
5. **`key` is an ordinary input** (default 0: no slice) until SM.3 wires `voice.key` (SM.1). Slice's rate follows a wired
   hz / `voice.noteHz` per voice.
6. **`gateOut` is 1 / 0 (Real)**: a meta type's outputs are Real; the Envelope's gate input needs a compare (SM.3 wires it).
   Named `gateOut` (an output cannot share the input `gate`'s name).
7. **Loop fade** crossfades with the frames BEFORE loopStart (as planned), so it is at most loopStart frames: a loop that
   starts at the sample's first frame has no fade. Ping-pong loops have none (a turn is continuous).
8. **Old entries live while read**: a held note keeps playing the sample it started on however long it holds (the entry
   is retired 2 s after the last read); only more than 16 swaps during one note stop it (silent until its next trigger).
9. Mip levels > 0 do not wrap the loop seam's taps (rates above 2 in a short loop: a seam tap from past the loop end).
10. **Scrub** (`position` wired) reads the newest entry at once (no trigger to wait for).
11. `sample_read` starts STOPPED (was: playing from 0), so an engine with nothing wired is silent (plan "Inputs").
12. **Cost** (bench/RESULTS.md SM.2, ship): stereo Re-Pitch with a param-rate rate is 1.06 – 1.17 × two pre-SM.2 mono
    reads at 8 voices (audio-rate rate 0.96 ×, 1 voice 0.65 – 0.70 ×): at the ≤ 1.1 target, not clearly under it. The
    remainder is per call (entry lookup, lane constants, mip choice: ≈ 3 ns/frame at 64 frames). Status: open.

## Q?-AP.7-a · 2026-11-21 · AP.7 · Roll preview + Flatten: what the offline path covers, what Flatten writes
Context: AP.md "Roll preview + Flatten" leaves the extent, the switch's default and Flatten's document form open.
Chosen (reversible): (1) The switch is ONE view setting (ViewContext::notePreview), on by default: a roll shows ghosts
whenever its clip's track has an active note device; not saved. (2) The preview renders the track's played clips from
song tick 0 (as an offline render from the start: the arp's state at the visible start depends on what came before;
cost ≈ the timeline + ~50 ns/block of arp), up to the visible end + one bar and at least the clip's end, recomputed
when the track, the tempo map or the tunings change (debounced 40 ms, on a worker). (3) Not modelled: automation of
the device's params, cables into them (AP.2b mods), live input, an arrangement loop; a Hold-latched tail past a clip's
end is cut there; a clip's later loop passes are not drawn, and Flatten refuses a clip that repeats its loop. (4)
Flatten writes every played clip of the track and switches off every active note device on it (switching one device
off changes every clip). (5) Source notes are removed, except those a generated ratio is anchored to: they stay as
deactivated notes. Identity: a Degree shift of a Degree note → that Degree; a ratio (Harmonics / Ratio / Walk /
Layers) of a Ratio note → the product to the same anchor, of any other note → the ratio anchored to the source note;
otherwise Cents of its Hz (counted: TrackNoteRender::inexact). (6) Consequences tested as such: Hz is exact; a flattened
shifted Degree note has voice.ratio 1 (the arp sends the interval), an ungrouped Harmonics note voice.harmonic 0 (the
arp sends Base + k). (7) A layer's level is folded into velocity (a document note has no gain); shaped glides become
16 linear curve segments (Linear ones are exact). (8) Ghosts are drawn OVER the clip's notes (an arpeggio plays on the
chord's own pitches). Status: open.

## Q?-AP.8a-a · 2026-11-21 · AP.8a · Arp retune: what the per-node tables and the re-resolution do not cover
Context: AP.8a keys the arp's degree tables by the note event's tuning node and re-resolves held notes itself when a
table set changes. Chosen (reversible, arp.h "Tuning edits", note_events.h "Pitch identity"): (1) A note that follows
no tuning node (event tuning 0: a clip in a private / Scala / MODMOS tuning) steps in the TRACK's table: a static-tuning
clip on a track of another tuning steps in the track's degrees (the event has no room to name a static tuning; a field
or a static node index would be needed). (2) Tables follow node NUMBERS: when the track's signal list changes (a clip
switching tuning renumbers the nodes), a latched note keeps its node number and is re-resolved in whatever tuning now
has it (the track's own tuning keeps node 1, so a track tuning switch moves a latched set to the new tuning — tested).
(3) The re-resolution assumes a table set reaches the stage no later than its producers' retunes (the link posts tables
before it publishes sequences / live routes, and a take never waits behind an uncollected slot in practice: 4 slots,
freed by every post and by ProjectLink::collect); a producer's retune is absolute, so table-then-retune is exact.
(4) The ratio of a generated note rides in its float Hz (E = hz / hz₀ in the node): exact to float32, the integers stay
in ArpExact. (5) A tempo edit re-times the sounding notes' remaining gate by the ratio of the last and this block's
snapshot bpm (exact for step tempos; a ramp is approximated over the remaining gate). Status: open.

## Q?-AP.8b-a · 2026-11-21 · AP.8b · Flatten: what it covers, what it refuses, what the preview leaves out
Context: review H3 / M5 / M6 / M7 asked Flatten to write exactly what plays. Chosen (reversible): (1) AUTOMATION of the
devices' params is followed (the lanes on their slot keys, read once per block as playback does); a hand OVERRIDE of a
lane ("Re-enable Automation" pending) is not known offline: the render follows the lane. (2) A CABLE into a device param
(lane steps included) cannot be reproduced offline — its value comes from the audio graph (free LFO phase, envelopes,
other tracks) — so Flatten REFUSES while one exists ("a cable modulates <device> <param> …") and the preview draws
without it. (3) M6: a deactivated (muted) main-lane clip is flattened as it would play unmuted, in a second render
pass of the muted clips alone (cross-clip arp state between a muted clip and its active neighbours is not modelled;
clips do not overlap and a tail is cut at its clip's end anyway); it stays muted. Take lanes: Flatten REFUSES while one
holds notes (no note edit reaches a take lane, and switching the devices off would change how they play) — comp or
delete them first; while a take is auditioned the main lane is what is flattened (it is what the take does not hide).
(4) M5: a source note is removed when it plays (start in [contentStart, contentStart + length)) or generated a listed
note; others stay untouched. (5) M7: if the document changes while Flatten renders, it renders again from the new
document, at most 3 times, then gives up with a status; one Flatten at a time. Status: open.
## Q?-SM.1-a · voice.key: readings taken
Context: SM.1 (docs/plans/SM.md). Choices (most reversible):
1. **Keys outside 0 … 127 are −1** (unknown), as MIDI export skips them; voice.key is 0 … 127 or −1.
2. **The computer keyboard** carries a drawn note's key (doc::midiKeyOf of what it plays: Keys 60 + degree / the .kbm key,
   Hz mode the nearest 12-TET key); with a 12-TET key map the key it stands for (60 + d). MIDI keys: the raw key.
3. **Arp steps**: a copy keeps its source's key; a step keeping a Degree identity moves the key by the degrees moved (from
   the source's key, so a .kbm's keys stay its own when its mapping is consecutive; a non-consecutive .kbm would need
   the stage's degree table to carry the key map: not done); any other step (Ratio, Harmonics, a static shift) the
   nearest 12-TET key of its Hz.
4. **Held from the note-on**: retunes and Pitch events never move voice.key (as voice.noteDegree).
5. `SequenceImage::Note` grew 72 → 80 bytes and `NoteEvent` 56 → 60 (no free padding left for an int16).

## Q?-SM.3-a · the Sampler group: readings taken
Context: SM.3 (docs/plans/SM.md). Choices (most reversible):
1. **The Envelope multiplies after the filter** (Amp = mul) instead of sitting in series: its `in` / `out` are mono and
   the sample is stereo. The voice still ends by the Envelope (BR.C: an ender behind Out).
2. **No explicit compare block**: the lowering now lets a wire target a Bool input (a gate) and reads sum > 0 there, so
   gateOut is wired straight into the Envelope's gate (one `gt` per voice, the compare the plan asked for).
3. **One-Shot's end**: the engine's `done` is not an ender of its own; the Envelope's gate is gateOut, so a One-Shot voice
   ends at the sample's end + the Envelope's release (Trigger mode).
4. **Filter and LFO switched off by default** (Filter Low Pass at 20 kHz; LFO → Filter Frequency a Travel wire of 0.25):
   a default voice costs the engine + the envelope only. The panel has no on/off toggles for them yet (SM.10's face).
5. **setSamplerSample takes the root as an argument** (document cannot see views' SampleAnalysis): the drop (SM.8) passes
   SampleAnalysisResult::pitch.hz; nullopt (not analysed) → 261.63 Hz. A new sample resets the data's other fields
   (markers, rootText, sliceBy).

## Q?-AP.10-a · Arpeggiator Into / Out split: what stays shared, what the bend view shows
Context: AP.10 (docs/plans/AP.md "AP.10"). Choices (most reversible):
1. **One "Offsets in cents" switch** (`glideCents`) for both sides; time / sync / shape / bend are per side. A per-side
   unit would be two more params and a second toggle for a rare need.
2. ~~Into + Out longer than the note shrink in proportion~~ — **AP.11: removed.** The sides are additive offset curves
   and overlap freely; each Time is 0 – 200 % of the note (Into > 100 %: never arrives; Out > 100 %: begun before the
   onset).
3. **The bend view draws Previous / Next a nominal 200 ¢ away** (labelled): their real pitches depend on the step; the
   Pattern view shows the real curves. Offsets (in the unit or cents) are drawn at their true size.
4. **The pattern view shows one cycle of at most 64 steps** (longer: the first 64, captioned) and a nominal note of the
   track tuning's degree 0 when nothing has been held since the panel opened.
## Q?-SM.8-a · the sample drop on the circuit: readings taken
Context: SM.8 (docs/plans/SM.md "Decisions", Drop). Choices (most reversible):
1. **An unvoiced Sampler plays from Position**: on an audio track no notes reach the group (a plain group, gate false),
   so its Envelope stayed shut and an LFO into Position was silent. `meta:sampler` now raises gateOut while a wired
   Position scrubs, only outside a voice (folded away unwired; voiced Samplers unchanged, SM.3's tests bit-identical).
2. **Auto-wire = ops::pasteBlocksAtNotes's rule**: the path holds no block. A MIDI path with only a note effect (an
   Arpeggiator) counts as not empty, so the Sampler lands unwired there. Alternative: splice after the note run.
3. **Every band takes it**, the master and group bands too (not only MIDI / audio / return): what pasteBlocks accepts.
   The group-panel (scoped) and embedded circuits take no drop yet.
4. **Several files dropped on a Sampler**: the first replaces its sample, the rest become new Samplers under it.
5. **A dropped Sampler is named after its file** ("Kick"), as a dropped track is; a replace keeps the name.
6. **Placement** is the spawn rule: centred on the drop point but never above the band's title strip (a tall block
   dropped near the top has its top there), an overlap goes to the nearest free spot.

## Q?-SM.10-a · the closed Sampler face: readings taken
Context: SM.10 (docs/plans/SM.md "Face (👁)" Closed). Choices (most reversible):
1. **The face is the group's panel** (an oval `shape`, an engine section `samplewave` of a new `sampler` MetaFace, generic
   mini knobs, a `fold` chevron), not a view-side special face: the Device view and the panel window show the same, and
   the bare `meta:sampler` block (inside the opened group) is drawn as the oval face too.
2. **Expand = a fold** (look "chevron", folded by default, Panel::fit): open, the SM.3 tabbed settings show under the
   oval at its width (the embedded Filter / Env / LFO panels scaled to ~0.6). While open the block is a rectangle (a
   round plate shapes a panel with folds only when no open section shows below it). SM.11 replaces the section.
3. **Classic knobs in every mode** (Transpose · Filter · Volume): the Gran set (Position · Size · Density) needs SM.6's
   params. Label-free (the meta faces' style; names on hover).
4. **No Filter / LFO on switches on the face**: a panel has no binding for a block's activator (on / off); adding one
   (a BindingKind or an "active" setting) is left to SM.11 with the expanded panel. The Filter's knob shows the LFO's
   modulation ring (the Travel wire), even while the LFO is off.
5. **Hidden = not on the canvas** (scrolled off, the view hidden, a retired face). The Device view's host says nothing,
   so a Sampler shown there keeps its tap read while the panel exists.

## Q?-SM.4-a · Sampler warp (sampler_warp / dsp::WarpVoice): readings taken where the plan left room
Context: SM.4 (docs/plans/SM.md "Semantics", "SM.4"). Choices (most reversible):
1. **Complex / Complex Pro play as Tones** (the same render, tested) until SM.5. The panel has no warp control yet (SM.11),
   so the fallback is stated in pq/meta/sampler.h and here only.
2. **Slice mode ignores Beats … Complex Pro** (its slices play at their recorded pitch, unwarped); Re-Pitch keeps SM.2's
   `(bpm / sampleBpm) · hz / root` sample_read path, so Off / Re-Pitch keep today's cost.
3. **Loops in the stretch modes** fold the content in beats (Classic, loop on); Ping-pong loops forward; loop fade and
   One-Shot fades in / out do not apply (Beats' crossfades and the 2 ms end fade do).
4. **Reads are cubic** (sample_read's interpolator), not the clip's 64-tap sinc: the Sampler's quality everywhere else.
   The mip level is per grain / slice from its rate at its start, so a fast upward glide inside one long slice aliases
   until the next slice.
5. **Beats in the clock form** (per sample, source frames since the entry) is a second implementation of the pass geometry
   (review: the plan wanted one). The clip player cannot take it without changing its renders: the clock form finds the
   natural end and each loop iteration where the integrated 32.32 position crosses L (+ i·L/2), the closed form at
   T + ceil(L / r + i·λ) in double, and its declicks are per-sample double ramps, the clip's float ramps anchored per
   iteration — different samples / roundings, so the 22 baseline hashes would move. It also reads per-frame positions,
   which the clip's streamed, chunked constant-step sinc reads (and reverse passes) do not take, and costs ≈ 10 ns /
   frame where the closed form is ≈ 1.2. The other way round (the sampler on the closed form while its rate is
   constant) would make a render depend on where glides start and stop relative to blocks. Shared: the pass struct, the
   ramp shapes, Tones / Texture entirely. The next slice's entry is predicted from the current tempo each run and fixed
   once reached.
6. **Markers' law**: no markers = sampleBpm; one marker = sampleBpm through it; beyond the ends the end segments continue.
7. **Tempo is per block** (transport.bpm): a tempo change lands on a block boundary, so renders match across block sizes
   at a constant tempo (or a change on a shared boundary), as any param-rate input.
8. Grain size is taken at the trigger (the grain grid is the voice's); a retrigger restarts hard (as sample_read); no
   reverse.
9. Cost (ship, bench/RESULTS.md SM.4): Tones ≈ the clip's Tones per voice; Beats / Texture ≈ 10 – 17 ns per voice-frame
   (the clip's copy paths are ≈ 1.3 ns: the per-sample clock form and the kernel's per-lane gather / scatter). The
   kernel's scratch (≈ 57 KiB: Tones' alignment buffers) is in the atom's state after the lanes' words (preallocated
   per instance and voice group, not per lane; review: audio threads include driver threads (ASIO) whose stack size
   we do not control, so it is not on the stack); ≈ 5 KiB of per-lane in / out buffers stay on the stack. Status: open.


## Q?-SM.11-a · the expanded Sampler panel: readings taken
Context: SM.11 (docs/plans/SM.md "Face (👁)" Expanded). Choices (most reversible):
1. **No Granular section, no Gran cell** yet: SM.6's granular params / mode are not on main, and SM.13 owns the section.
   Building against guessed names would leave dead controls; the mode row reads its items from the panel (`items`), so
   SM.13 adds "|Gran" and the section in one place.
2. **Nested folds via style fields**, not a new element kind: `parent` (a child fold is gone with its parent),
   `section` (ownership by name, for the mode row beside the oval), a fold's `when` hides it entirely; fit also fits the
   width. One general rule in `doc::foldLayout`; the patch reads a fold condition only when its source is bound to a
   setting (other sources count as holding there).
3. **Activator binding = a Setting binding named "@active"** (`doc::kActiveSetting`), not a new BindingKind: no file
   format change, any block, one undo step + recompile through `ops::setDevicesActive`.
4. **Warp shows in every mode** (Re-Pitch applies to Slice too; Beats … Complex Pro do not — the menu still offers them
   there). Slices shows in Slice mode only; the slice lines also show with warp Beats (its transients).
5. **Root identity** = the piano roll's row label + equave (`C3` for the 12-EDO reference, octave numbering as Live),
   recomputed from the root Hz on display (the stored `rootText` only marks the identity display / keeps the name).
6. **Warp marker drags move the frame, keep the beat** (Live's sample view); a new marker takes the beat the ruler reads
   there, so adding one never changes the sound.

## Q?-SM.12-a · slicing and Slice to Notes: readings taken
Context: SM.12 (docs/plans/SM.md "SM.12"). Choices (most reversible):
1. **Transient sensitivity = a threshold on stored onset strengths**: media::Transients now keeps each onset's strength
   (flux above the local mean in units of the median; the cache file version 2), sensitivity s → threshold
   5 · 2^(3(1 − s)): 1 keeps every detected onset (the drop's slices), 0 only those 8 × stronger. No re-detection, so a
   sensitivity drag is instant; it cannot find onsets weaker than the detector's default threshold.
2. **Slice 1 always starts at frame 0** for Transient / Beat / Region (Live does the same); Manual keeps the table.
3. **Slice to Notes anchors the first slice at the start marker** (leading silence before slice 1 is not reproduced);
   warp Off: seconds through the tempo map from the marker; warp on: content beats (markers / sampleBpm), so the clip
   follows the song tempo as the warped sample would. Notes run to the next slice (the last to the sample's end).
4. **"Insert marker" = the start marker** (ViewContext::startMarker), the only insert position the views keep.
5. **Transient slicing waits for the sample analysis** (SampleAnalysis): before it arrives the click says so and does
   nothing (the analysis is requested).

## Q?-SM.5-a · Sampler Complex / Complex Pro: readings taken where the plan left room
Context: SM.5 (docs/plans/SM.md "SM.5", "Semantics" Complex note-on, open risks 3–5, 9). Choices (most reversible):
1. **The stretchers ride on the SampleSlot**, not a function-pointer resource or a second resource kind: `sampler_warp`
   reads one resource (KernelArgs::resource) and the instance's SampleSlot is already per instance, so the bank is an
   atomic pointer on it (`SampleSlot::setStretch` / `stretch()`), its state outside the JIT arena (risks 3 and 9 do not
   arise). The Sample request carries `stretch` (meta::ResourceRequest) for Complex / Pro outside Slice; the link builds
   the bank in `syncMetaStretchers` before any image of the lowering, rebuilds it when a published sample has another
   channel count, frees it otherwise. StretchPool itself moved to pq_dsp (`pq/dsp/stretch_pool.h`).
2. **A stretcher per voice lane of the track** (voiceGroupsFor(polyphony) · 8, the steal-headroom group included: a
   voice can sit in any lane): 24 voices (the default) = 32 stretchers ≈ 24 MiB stereo. Not a free-list pool: lane
   resets (note-on) would leak claims. Ownership {token, user lane, vHi} handles moves, Syncs and hot-swap transfers
   (the first to run keeps it; the other plays Tones and re-primes on a free one, a hard Complex → Tones switch).
3. **Content timing under glides**: the vocoder's content is placed ≈ 100 ms ahead of the read at the rate of the
   moment (no future rate is known); measured ≈ 1 ms late held, ≈ 5 ms late in a 10 st/s descent, recovering once the
   rate settles (tests/dsp/test_sampler_stretch.cpp). Pitch is exact (Φ): < 0.5 ¢ at 18\31, held after a glide.
4. **Reads of the vocoder output are cubic** (the Sampler's quality, Q?-SM.4-a 4), not the clip's sinc: above rate 2 it
   aliases (no mips of the vocoder output).
5. **Device cost**: the vocoder runs inside the sampler's kernel, so device_cost.h's per-device ns/frame includes it with
   no change; memory is not shown there (SamplerStretch::memoryBytes exists for a later readout).
6. **Open (CPU)**: a voice whose lane the VoiceEngine has freed while its WarpVoice still plays (Classic looping after the
   envelope's release) keeps running — Tones since SM.4, the vocoder now — until the lane is retriggered, whenever its
   group runs for another voice. A per-lane "occupied" mask the kernel could read (as VoiceTap::setLaneMask) would stop
   it; not done here (touches the VoiceEngine / image host). Status: decided (SM.14): a general `KernelArgs::laneMask`
   word (the group's occupied lanes) delivered to `KernelInfo::readsLanes` kernels on both backends; sampler_warp,
   granular and sample_read stop a freed lane (docs/ARCHITECTURE.md leaf-atom kernel ABI "Occupied lanes").

## Q?-SM.6-a · Sampler granulator (granular atom, GrainTap): readings taken where the plan left room
Context: SM.6 (docs/plans/SM.md "Semantics" Granular, "SM.6"). Choices (most reversible):
1. **Granular is a fourth Sampler mode** (`mode` 3), with settings granMode (Classic / Loop / Cloud, default Loop),
   grains (1 … 32, default 16), harmonic, shape, and params grainPosition, scan (−4 … 4 × the file's speed, default
   1), grainSize (5 ms … 1 s, default 80 ms), density (1 … 400 Hz), sprayTime (≤ 0.5 s), sprayPitch (≤ 24 st),
   sprayPan, freeze. Warp, loop and trigger do not apply; the voice ends as Classic's (the gate falls). The panel's mode
   row gained "Gran" (kSamplerModes: the tabs map items onto the setting's range, so the row must list all four; the
   six sampler-panel goldens moved in that row only); its Granular section is SM.13's.
2. **The GrainTap rides on the SampleSlot** (one resource per atom, as SM.5's stretchers): the sampler requests
   `ResourceKind::GrainTap` (index 2) in Granular; the link makes the tap and hangs it on the instance's slot.
3. **A grain's rate and mip level are fixed at its spawn** (a glide moves later grains); Classic reads at the transpose
   ratio × the file's speed, Loop / Cloud at hz / root × ratio. Spray time is ± seconds of source around the voice's
   position; pan is equal power with centre gain 1 (overlapping grains are not normalised: density × size sets the
   level, capped by the grain count).
4. **Slot reuse on a 64-sample grid** of the voice's clock (a spawn takes a slot whose grain ended at or before its grid
   cell's start; none free: skipped). Needed so which grain plays where, and so the render, never depends on the block.
5. **The seed** is StateInit::seed (0 from the sampler body today, so two samplers on the same note draw alike; voices
   differ by their voice slot). A per-node seed input is a one-line follow-up if wanted.
6. **A retrigger keeps the voice's live grains** unless the sample changed (then they stop at once: a click on a swap +
   retrigger). Harmonic ratios come from the data's `harmonics` ((n, d) pairs; default 1, 2, 3/2, 3, 4, 5).
7. Q?-SM.5-a 6 applies here too: a freed lane whose voice never ends keeps spawning grains while its group runs. (Fixed by SM.14.)

## Q?-SM.13-a · the Granular section and grain dots: readings taken
Context: SM.13 (docs/plans/SM.md "Face (👁)", "SM.13"). Choices (most reversible):
1. **Grains reach the face through the voice reader** (`views::VoiceTapReader::grains`, TapVoices with a GrainTap
   resolver; FaceVoiceHub asks the link for request 2), not a second reader per face: one shown / hidden path, so a
   hidden face's TapVoices parks both taps (no reader on either).
2. **A `when` condition reads a source inside a FOLDED section** (GroupPanelFace::conditionHolds; a source hidden by
   its own condition is still "hidden", EQ.3): folding is view state and must not change which controls apply. The
   closed face's two knob sets (Transpose · Filter · Volume / Position · Size · Density) are conditions on the mode row,
   which only the expanded panel shows. Affects only panels with folds (the Sampler's).
3. **Warp is hidden in Gran mode** (Granular warps nothing, Q?-SM.6-a 1) — revises Q?-SM.11-a 4 for that mode only.
4. **Grain region** = [Position in Start … End, + grain size), the spray time shaded either side; a dot's height is its
   pan (left above), its size / glow its window's amplitude. The closed well's region is draggable too (its body and
   right edge take presses there; the rest of the well stays the block's: select / double-click to expand).
5. **Ratio chips**: ≤ 17 ratios in the editor (two rows); the last one cannot be removed (an empty list would silently
   fall back to the default six); + adds the next whole number above the largest ratio; typed n/d is kept as typed
   (4/2 is not reduced). Shown dimmed, still editable, while Harmonic grains is off.
6. **Density** shows only for Loop / Cloud (Classic spawns one grain per period).

## Q?-AR.2-a · arp glide rework: where the build left the plan
Context: AR.2 (docs/plans/AR.md "AR.2"; pq/engine/arp.h "Glides", "Step lanes"). Choices (most reversible):
1. **Onto a ghost, not a vertical snap**: the plan snaps a head drag "onto the previous-step ghost" by height. A
   ghost's height can equal a unit step's (−2 deg), which would make that step unreachable, so `intoPrev` / `outNext`
   are set by dragging the head / tail ONTO the neighbour (left of the onset / right of the note's end, at its
   height) or by clicking the ghost. Pulling the head up / down off the ghost returns to an offset.
2. **Sideways = time, relatively, with a 4 px dead zone** on each axis (a straight pull changes one value); the knee
   dots are kept as a time-only handle. No source chips and no Prev / Next toggle beside the knobs (the ghosts are it).
3. **Shape chips kept** (Curve | S | Spring per side): S-curve and the overshoot are distinct sounds. Curve's value 0
   is the old Linear, ± the old Exponential bend; Spring with curve 0 is a cubic ease-out (the old default Bend 0.5
   overshoot is one drag of the curve's middle away). Option: drop S (subtle on short glides).
4. **The Glide lane's default is 0 always** (not only "when first switched on with both sides off"): one rule — a
   lane that is ON slides only the painted steps; OFF is × 1. So switching the lane on with Into / Out on silences
   their glides until steps are painted (303 behaviour). Its value scales DISTANCE (0.5 = from half way), not time.
5. **Offsets are cents, ± 2400** (two octaves; was ± 4800 unit steps): the engine no longer steps a glide offset
   through the note's own table, so under an unequal tuning a snapped "−2 deg" is the nominal note's interval for
   every note (the plan's rule). Lost on purpose: per-note unit offsets (Harmonics: h → h + k of each note's own h).
6. **Bend view ghosts are schematic** (± 62 % of the view's pitch range, the previous step below, the next above); the
   pattern view shows and uses the real neighbours.

## Q?-AR.1-a · arpeggiator placement and the notes cable rule: readings taken
Context: AR.1 (docs/plans/AR.md "AR.1"). Choices (most reversible):
1. **Wiring both ends replaces the direct notes cable.** A track has one note stream, and the main path takes the
   OLDEST cable out of the notes that reaches the mixer: with the direct notes → instrument cable left in, a free
   arpeggiator wired notes → arp → instrument would be off the path and do nothing. So the cable edit that completes the
   way through note devices removes the parallel cable in the same undo step. Other option: keep both and make the
   main path prefer the way through note devices (then the direct cable would be drawn but carry nothing).
2. **The rule is on the cable edits, not on the `AddWire` command.** The main-path builders still make source → arp →
   mixer on a track without an instrument, and removing the instrument after an arp joins arp → the next block; both
   play through nothing, as before. Refusing those in `apply` would make those device edits fail.
3. **"Takes notes"** (where a Notes output may go): the MAIN input of a note device, a group, a meta module with a
   chain input, an instrument. Not an atom or value block, a param, a second input, a mixer.
4. **A cable out of the lane's notes dropped on empty canvas** still offers every node with a main input (instruments
   and modules, as before), the Arpeggiator among them — not only Notes-in-and-out entries as the plan's wording
   suggests; only Tab over a notes cable narrows to those. Out of the Arpeggiator's output: what takes notes (3).
5. **Inserted on the notes cable**, the block is kept inside its band's left edge (that cable is short, at the band's
   left end, and the panel is ≈ 960 wide: centred on the pointer it hung out over the tuning block).
6. Not changed: the browser drop still goes only onto a notes cable (dropped on empty canvas it is refused), while
   Tab places it free. A loaded project with a wire that breaks the rule loads (the wire is inert).

## Q?-AR.4-a · arpeggiator step lanes and panel pass: readings taken
Context: AR.4 (docs/plans/AR.md "AR.4"). Choices (most reversible):
1. **A lane's switch, name, menu and readout are part of the `steplane` element** (a name column), not a separate
   toggle: the element shrinks to its name strip when off. The collapse uses the existing fold layout — a `fold` of
   look "band" (nothing drawn) with the lane's switch as its condition — instead of a new layout rule.
2. **Double-click types a value** (was: reset); the default is Alt+click / Alt+drag on every lane. The first click of
   the double-click still sets the step from the pointer (its own undo step), as any click does.
3. **Wheel: plain = fine (1 % / 1 step), Shift = coarse (10 % / an equave)** on every lane — the plan's Transpose rule
   made the lanes' rule; the knobs' wheel has Shift = finer.
4. **Transpose rows**: ± 7, then whole equaves that hold every value (12-EDO: ± 12, ± 24 …), not "7 + equaves"; so an
   equave line is always at the edge. Ratio / Harmonics have no equave: sevens, a label every few rows (not every row:
   6 px rows cannot all carry a number; each step's own value is written on it).
5. **Randomize** is seeded by the device seed, the lane and a count kept as view state: the same project gives the same
   sequence of results, and asking again gives a new one. Transpose randomizes within the rows shown; Skip / Tie
   60 / 25 / 15 % Play / Skip / Tie.
6. **Copy to** only between Velocity / Gate / Glide / Chance (by value, clamped, with the length). Transpose and
   Skip / Tie have other units; not offered.
7. **Rate**: "n:m" is a chip on the rate control (view state, not saved); a rate the menu cannot say (3/16, 5:4) shows
   the number boxes whatever the chip says. Dotted reads "D", Triplet "T" in the box.
8. **Top band 200 px** (was 182) to leave AR.3 a 58 px Layers row in the Pitch block; all lanes on, Glide open the
   panel is ≈ 760 px high — taller than a small Device view (the view does not scroll a panel vertically).
9. `viewFoldLayout` (the patch block's size) now reads a note device's settings with their defaults, so a lane that
   was never switched on counts as off there too.

## Q?-AR.3-a · pulse rework: where the build differs from plan AR.3
Context: AR.3 (docs/plans/AR.md, now updated to what was built). Choices made without asking (all reversible):
1. **Cycle unit.** Plan: "num/den of a whole note … follows the signature map as Bars did" (two readings). Options: whole
   notes (ignores the signature) | bars. Choice: **num / den bars** — identical to whole notes in 4/4, and a 3/4 or 7/8 bar
   or a signature change is followed (a fractional cycle = that fraction of its bar). den ≤ 32.
2. **The mask's storage.** A 32-bit value is not exact in a float param, and a stepped setting counts its steps in an
   int32 (2^32 overflows). Choice: **two 16-bit settings per row** (`pulseMaskLo/Hi n`); a click writes one of them, a menu
   action both in one undo step.
3. **Rows shown.** Plan: "nothing held: the first 4 rows, nominal". Built: at least 4 and at most 8 rows always — the held
   notes (or the last held set) from the bottom, the rest dimmed "what a note added here would play" — so rows 5 … 8 are
   reached by holding more notes (as their notes would be). Beyond 8 held notes the rows repeat (the view shows 8).
4. **Shift snap + more menu items.** Shift+drag snaps to 1/24 of the row's period (halves, thirds, quarters, eighths;
   Alt: free); the row menu also has Rotate Later / Earlier and All On (more rhythms from one Euclid pattern).
5. **A masked first pulse.** A note-on between grid onsets sounds at once as the grid pulse before it (the AP.3 idle
   rule): if that pulse is muted in the row's mask it does not sound. Alternative: always sound the note-on. Kept the
   rule (the mask is the rhythm), cheap to flip.
6. **Releasing a note no longer re-times the others** (there is no h_ref): each clock depends only on the Cycle and its
   own division. A note's ROW still follows its rank by Hz, so adding a lower note moves the notes above it up a row
   (in Auto with JI notes nothing moves: the harmonic is the division).
7. **Beat lines** in the view assume 4 beats per bar (the panel target has no signature at the playhead); the engine
   follows the real signature.
8. **Layers** in the Pitch block: a count and up to three level knobs in the row AR.4 left for them (first built as
   number boxes: the block had no room before AR.4).

## Q?-SM.17-a · Sampler panel pass: readings taken
Context: SM.17 (docs/plans/SM.md "SM.17"). Choices (most reversible):
1. **A stored stretch mode in Slice shows as Off** and is kept (Slice plays it unwarped: pq/meta/sampler.cpp); a click on
   Off / Re-Pitch there overwrites it. Back in Classic the stretch mode is still chosen. Other option: write Off when
   entering Slice (loses the choice).
2. **Fewer items than steps = the first steps** (menu and tabs), with tabs style `beyond` (last | first) for a value past
   them. General rule instead of a Sampler-only "Slice warp" element. Before, such items were spread over the range.
3. **Complex has no Warp section** (the plan lists no control for it; whether `grain` reaches the Complex stream was
   not checked here: SM.16 reworks it); Complex Pro shows Formants and Envelope (`formants`, `formantOrder`).
4. **Loop Fade shows only while looping with warp Off / Re-Pitch** (a stretch mode has no loop crossfade, Q?-SM.4-a 3);
   the editor's fade shading follows the same rule.
5. **Slices and Granular are open by default** (they only exist in their mode); Envelope / Filter / LFO stay folded.
6. **Wheel on the pill follows the hosts' value-gesture rule** (Device view and panel windows; on the canvas the wheel
   scrolls the view, as for knobs) rather than taking the wheel everywhere.
7. **Grain Shape shows names**, not drawn window curves: RadialItem takes a text glyph only; a drawn glyph needs a
   painter callback on the menu (not cheap, left out).
8. **Fades have no handles in the editor** (they are knobs; the ramps are drawn only), so there is nothing to hover.
9. **The radial item text `A/B` means category A** in the radial look only: a list of note values ("1/4") must stay a
   list or be given categories.
10. **Live check harness**: `PQ_BUGREPORT_SCRIPT` got drag / wheel / type / double-click / key steps (src/app) so a
    worker can drive its own instance and read F12 shots; an unsaved-changes prompt at quit is a native box the script
    cannot answer (the runner ends its own process and removes the autosave folder that leaves in pqdaw/Unsaved).

## Q?-SM.15-a · one warp core: readings taken (supersedes Q?-SM.4-a item 5)
Context: SM.15 (docs/plans/SM.md "Rework 2026-11-23"). Choices (most reversible):
1. **The enum is `dsp::AudioWarp`**, not `WarpMode` (`dsp::WarpMode` is the Wavetable's Bend / Mirror / Pulse width,
   pq/dsp/wave_warp.h). `doc::AudioWarp` / `doc::BeatsLoop` stay as aliases of the dsp enums (35 files name them);
   `engine::ClipWarp` / `SliceLoop` / `WarpSettings`, `meta::SamplerWarp`, `dsp::WarpVoiceMode`, `ComplexRun` are gone.
   `sampler_warp`'s `mode` input is now the enum's value (2 Beats … 6 Complex Pro; was 0 … 4).
2. **Beats, the one law** (pq/dsp/warp_voice.h top). Where the two old forms differed: fades and the loop rule scale by
   the step at the pass's ENTRY (the clock form used the current rate); before its natural end a slice is "cut" when
   t + ⌈clock left / entry step⌉ ≥ next − X; at pitch rate 1 the natural length and the looped half are whole frames
   (loops stay straight copies); the clip's events sit on the integer clock instead of ceil(T + L / r + i·λ) in double
   (≤ 1 sample apart). A voice whose rate moved inside a slice evaluates that slice per sample until the next one.
3. **Pass enumeration stays per host** (the plan says so): the clip's slices are in song time on its image, the voice's
   follow the live tempo. The renderer, pass struct and gains are shared.
4. **Window**: `warpHannGain` computes sin²(πj / N) per frame (integer reduction to an eighth, one polynomial; grains
   sum to 1 within one rounding; ≤ 2e-7 from the cos form). The clip's image still caches it per grain size (built
   by that function) because that is cheaper per frame; the Sampler's grain size is a param taken at the trigger on the
   audio thread, so no table can be built ahead: it calls the function per grain piece. Alternative: both call it
   per piece (clip Tones / Texture ≈ +0.3 ns per frame and grain).
5. **Sampler Re-Pitch through the markers is inside `sample_read`** (inputs `tempo`, `sampleBpm`): the local slope
   depends on the playhead, and a graph atom reading `pos` back into `rate` is a cycle the scheduler only takes with a
   z1 (per-sample scheduling of the loop). Cost with no markers: none per frame (a per-lane constant); with markers two
   compares per frame. Lanes read the law of the entry they play.
6. **The first Beats slice's mip level** now comes from the rate at the trigger's first sample (was level 0).
Status: open.

## Q?-WF.1-a · one waveform renderer: readings taken
Context: docs/plans/WF.md. Choices made where the plan left room or the code said otherwise (all reversible):
- **Samples from 512 frames per px, not 256.** A column the size of one overview bucket straddles two, so it drew up
  to twice its frames (a slow wave as a thick stepped band). Levels are now chosen so a column reads ≥ 2 buckets, and
  the samples take over below 2 × 256. Cost: sample reads at 256 … 512 frames per px (≤ 1.3 M frames for 2560 px).
- **Audio clip view keeps its peak / RMS body** (AC.look, an envelope: not the signal's min / max) down to 16 frames per
  px, drawn with the shared column geometry (antialiased); the signal itself (shared renderer) from 8, cross-faded
  between. Option not taken: replace the body with the plain waveform at every zoom.
- **The audio clip view had no zoom at all** (the plan says "follow its gestures"): both editors got wheel = zoom,
  Shift+wheel = scroll, ruler drag = scroll / zoom, ruler double-click = fit.
- **Sampler ruler double-click** adds a warp marker when markers are shown (SM.11, kept); it fits only without markers.
  Fit in every mode: double-click the strip under the waveform.
- **Sampler editor shows stereo as two lanes** (the closed well merges them into one lane, as before).
- **Arrangement limit 64 px per tick** (fixed): ≥ 32 px per sample from 7 bpm at 48 kHz, which is 640 px per sample at
  120 bpm. Option: a limit that follows the tempo and the clips' rates.
- **No stems** under the sample dots (decided by look: the curve already passes through them).
- Sample windows: up to 4 per sample (each view reads its own part; one window made two views read in turn for ever).
- Seen, not changed: a scripted instance under PQ_SETTINGS_DIR still offers the user's `%LOCALAPPDATA%\pqdaw\Unsaved`
  recovery (I chose Later; nothing was touched). Worth isolating for test runs.
## Q?-SM.16-a · one Complex stream + sampler warp CPU: readings taken
Context: SM.16 (docs/plans/SM.md "Rework 2026-11-23"). Choices (most reversible):
1. **The stream is a template over its host** (`dsp::ComplexStream`, pq/dsp/complex_stream.h), not a virtual
   interface: the host supplies U(y) (its law at vocoder frame y), the loop, the direction, start / end and `read`.
   The priming SCHEDULE stays per host (the plan says so: the clip's pre-roll / cue in song time, the voice's
   per-sample budget); `complexPrimeRate` (the rate both schedules use) is shared.
2. **One input-gather size**: 32768 frames per channel (the clip's; the Sampler's bank had 16384). Only a chunk that
   consumes more than that (a read rate above ≈ 128×) differs; + 128 KiB per stereo bank.
3. **The seek's rate** is |U(y0 + chunk) − U(y0)| / chunk for both (the clip evaluated the second point as
   t(y0) + chunk / r: one rounding apart, and the library keeps it as a float). No clip or sampler hash moved.
4. **The lane state stays interleaved** (`w[word][lane]`, LaneState::PerLane: the VoiceEngine moves and resets lanes
   by that layout, and so does the hot-swap transfer). So an 8-lane group still copies a lane's words in and out and
   scatters its four outputs; what changed is how much (only the mode's words: 35 Tones, 63 Beats, 65 Complex of 109;
   nothing for an idle lane) and the 1-lane kernel, whose buffers are contiguous, reads and writes in place. A
   lane-major state layout (a new LaneState kind) would remove the rest: VoiceEngine + state transfer + JIT state
   access; not done here. Alternative for the output scatter: render all lanes of a window into lane-major scratch
   and transpose once (8-wide stores); est. ≤ 1 ns / voice-frame.
5. **Tones after a lost stretcher** starts its grains afresh (`VoiceHost::kCatchUp` 1): the first grain is at the
   law's position, unaligned, like a note-on's. The clip keeps 64 (its runs position grains in order; unchanged).
6. **A test probe** (`dsp::tComplexProbe`, a thread-local pointer read once per chunk) lets a test see every chunk of
   both hosts; null outside tests.
Status: open.

## Q?-WF.2-a · curve displays: readings taken
Context: docs/plans/WF.md "WF.2". Choices made where the plan left room (all reversible):
- **A waveform's stroke weighs what every other line does** (a 1 px core + the 0.75 px feather: about 1.75 px of ink)
  instead of the 1 px box line. That is what removes the roping; the option not taken was a 1.25 … 1.5 px box line.
  Bodies keep exact (box) edges; the feather is 0.75 px down the column whatever the slope.
- **Moiré**: a column whose neighbours' midpoints alternate three times running takes the union of its neighbours'
  ranges, in proportion (full from an alternation of 35 % of the union, none under 10 %, so a dense body's jitter is
  left alone). A tone near 2 px per cycle becomes its envelope with up to ≈ 20 % of the beat left; 3 px per cycle and
  slower are untouched (they show as a wave). Option: an exact detector from the samples' turning points (more reads).
- **drawWaveform is 2.0× the pre-WF.1 cost for the overview, not ≈ 1.5×** (bench/RESULTS.md: 27.5 → 59.5 → 55.8 µs;
  1.7× / 1.6× in the sample regimes). The antialiased strip is 152 B a column against 64 B and the draw list has no
  instancing; reading the columns and the strips alone are 43 µs. The cached path (rasterWaveform: the arrangement's
  tiles) is 0.64× for overview tiles. Option if the direct sites matter (Sampler, clip view, browser, drag previews):
  draw them from coverage tiles too — a task of its own.
- **strokeSeries picks the picture by the data**: sparse → spline; dense and smooth → the simplified ribbon; dense and
  not → columns. The thresholds (1.5 px a point; 3 vertices per 4 px after simplifying) are judgement.
- **Ruler**: labels stop at the sixteenths and the view's own sixteenth is named when no line is in view; the time
  ruler carries the fine positions (to 10 µs). Option not taken: a fourth field (ticks) on the beat ruler.
- **Left, with reasons**: the automation line stays `smoothPolyline` (bevelled joins overlap, and an envelope is also
  drawn from coverage tiles: a partial repaint no longer equalled a full one — its sharp reversals can still spike);
  Stereo Image's ribbon (a quad per row); the Waterfall's terrain silhouette; the response grid ignoring band centres;
  the arp sketch's 16 points per glide.
- **Hard-cornered glyphs are even-width now** (the wavetable's double saw reads heavier). Option: thinner glyph strokes.
- No bench or paint timer exists for the live faces, so their per-frame cost was not measured; by construction a
  trace's vertices now follow its width (4 per px) or drop (Math scopes: 3 ribbons of 7), except the Waterfall's ridge
  (twice its points) and its lines when zoomed in (points × zoom, as the look needs).

## Q?-OP.0-a · 2026-11-24 · OP.0 · Animation rate of meters, faces and analysers
- Context: while playing, the UI thread costs 7 – 15 % of a core (docs/plans/OP.md §2.4, F1, F2), as much as or more
  than the audio engine. OP.1 bounds it at one present per display refresh without any visible change. What remains
  scales with the frame rate (and with the display's refresh rate on 120 / 144 Hz screens).
- Options: A. everything at the display's rate (after OP.1). B. meters, panel faces and analysers at 30 Hz, playhead
  and gestures at the display's rate. C. a preference (30 / 60 / display), default B.
- Choice (provisional): A until answered (OP.1 only); OP.2's rate cap waits for the answer.
- Status: decided (user, 2026-11-24): C with two rates. Meters, visualiser faces and analysers at 30 Hz by default,
  60 Hz as a setting (Preferences → Display); the playhead and whatever is dragged stay at the display's rate. Done in
  OP.2: one readout grid in the widget tree (`pq/ui/readout_clock.h`), a whole number of refresh periods per tick (at
  100 Hz: every 3rd pass = 33.3 Hz, or every 2nd = 50 Hz for the 60 Hz setting).

## Q?-OP.0-b · 2026-11-24 · OP.0 · Engine block larger than 64 frames
- Context: back to back, a session costs 7.4 % of a core at 64 frames, 6.6 % at 128, 5.7 % at 256 (per-block overhead
  ≈ 1 µs per sounding unit); at a device's cadence the difference is inside the noise (OP.md §2.3, F11). A larger
  block changes param-rate stepping (live would no longer equal a 64-frame offline render bit for bit) and adds
  adapter latency on device buffers smaller than the block.
- Options: A. keep 64. B. the engine block follows the device buffer up to 256.
- Choice (provisional): A; OP.4 removes the cheap part of the per-block overhead instead.
- Status: open

## Q?-OP.3-a · 2026-11-24 · OP.3 · The unwatched tap's stall is not the field's place in the ring
- Context: OP.md F3 asked to put `TapRing::readers_` on a line `signal_tap` already touches. Unwatched, the kernel
  reads exactly one line of the ring whichever it is; with the count on the ring's first line (done: the ring is 256 B
  instead of 384, a watched write reads one line less) the profile is unchanged: 193 of 360 samples still sit after
  the load of the count, ≈ 20 ns per tap per block (an L3-latency load), and 92 more on the kernel's first
  instruction (the indirect call). A line read once per block does not stay in L1 / L2 in `fx/eq x8` (cause not
  established; the same shows on other once-per-block loads). The −2 % of an EQ is therefore not delivered.
- Options: A. leave it (≈ 40 ns per analysed device per block). B. the host skips the call: a "no tap of this image is
  wanted" word in the run record the generated code already loads (as SM.14's lane mask), set in `begin()` from
  `segTaps_`. C. find what evicts once-per-block lines (per-block footprint of the engine) first: it would pay for every
  kernel's first touch, not only the tap's.
- Choice (provisional): A; C looks the more valuable follow-up (OP.4 touches the same per-block overhead).
- Status: open

## Q?-OP.1-a · 2026-11-24 · OP.1 · The audit's "60 Hz display" is a 100 Hz one
- Context: OP.md F1 expected 85 → 60 and 109 → 60 frames per second. This machine's primary display (and so DwmFlush
  and the app window) runs at 100 Hz; the second one at 60 Hz. One present per refresh therefore gives 85 → 84 and
  110 → 100 here: almost nothing with the arrangement. On a display slower than the pacer (the window dragged to the
  60 Hz monitor) the loop now paces by time: 59 – 60 frames per second, UI thread −30 % with the arrangement.
- Consequence for Q?-OP.0-a: at 100 Hz the UI's playback cost is what F2 / the animation-rate answer decide, not F1.
- Status: note (nothing to decide)

## Q?-OP.4-a · 2026-11-24 · OP.4 · The master's sum over silent inputs is not done
- Context: F8's third item. The sum is `add` nodes in the Master unit's compiled patch, so skipping the inputs
  `inSilent` marks means a silence-aware sum in the schedule and both backends (`schedule.cpp`, `codegen.cpp`,
  `interpreter.cpp`: OP.5's files), and it has to keep the sum's order (x + 0 is exact, a −0 is not kept). Nothing in
  `project_program.cpp` can do it. Worth ≈ 24 ns per frame at 64 tracks with 4 sounding (0.1 % of a core).
- Options: A. leave it. B. a task after OP.5 / OP.7 (same files): an n-ary sum op whose silent operands the sleep plan
  lets the host mask per block.
- Choice (provisional): A.
- Status: open

## Q?-OP.4-b · 2026-11-24 · OP.4 · The disk thread still polls while the transport runs (every 50 ms, not never)
- Context: F7 proposed no timeout when no stream is active, "with the existing wake event for new claims". There is
  no such event for a claim: the audio thread claims lock-free and never signals (stream.h). With no timeout a clip
  that starts while playing would never be read. So: 2 ms for 250 ms after a wake (engage, prefetch, wake; `locate`
  while playing now calls `wake`), 2 ms while reads are issued, otherwise 50 ms or a quarter of the time the emptiest
  ring lasts at 768 k frames / s. A clip's voice claims its stream 0.5 s before it starts, so 50 ms is early enough.
- Left: a claim that reads at once without a wake before it waits up to 50 ms instead of 2 (none known: Play, locate
  and previews wake; samplers pin their samples resident).
- Status: note

## Q?-OP.6-a · 2026-11-24 · OP.6 · Shaped glides cannot be vectorised with the same bits (F9 not done)
- Context: `VoiceEngine::renderShaped` (Exponential / S-curve / Overshoot) writes `hz0 · std::exp2(k · s)` per frame.
  The curve `s` and the cents are closed forms that vectorise exactly; the cost is the scalar UCRT `exp2` (≈ 5 of the
  6 – 7 ns per voice-frame: 25 – 30 µs per block for 64 gliding voices, 8.7 µs linear). No vector exp2 gives the UCRT's
  bits: SVML's `_mm256_exp2_pd` differed from `std::exp2` in 37 % of 20 M random arguments (by ≤ 2 ulp of the
  double). After the float32 rounding of the Hz value none of the 20 M differed (expected rate ≈ 1 in 10^8 … 10^9
  samples), but that is a rate, not a proof, and the double state a re-targeted glide starts from differs too.
- Options: A. leave the scalar loop (today). B. 4 frames per `_mm256_exp2_pd` call, the remainder through the same
  function on a broadcast (the S1 = S8 rule of the exact atoms): ≈ 3× on gliding blocks; a glide's Hz may move by one
  float ulp about once per 10^8 gliding voice-samples, so a hash / golden over a long glide can move once and is then
  re-recorded knowingly. C. the tuning kernel's own `exp2Scalar` / `exp2Vec` pair (same arithmetic per lane by
  construction, no SVML): the same speed class, every glide's double changes in the last ulps once.
- Choice (provisional): A until the user picks; B is the cheap one (the atoms already rely on SVML being the same
  function at 1 and 8 lanes).
- Decided (user, 2026-11-24): bit exactness of the audio output is not required; bits may move, audible quality may
  not. Done in OP.9, as neither B nor C exactly: a group without ops in the block walks its glides as rows (8 lanes
  per frame, `VoiceEngine::renderGlideRows`), 2^x by an inline polynomial of its own (`glideExp2` in voices.cpp:
  round, Taylor degree 10 as an Estrin tree, exponent add; < 3.1e-13 relative; the scalar form is the vector's lane
  0, so the per-lane walk of a block with ops gives the same bits). SVML and the tuning kernel's Horner pair were
  measured first (per lane, 4 frames per call: both ≈ 2× only, the 40-cycle Horner chain / the call bound the
  overlap). Linear glides, every shape's cents and the recursions keep their bits; only the shaped Hz moved: 5 of
  3.4 · 10^7 shaped glide points differ from the std::exp2 walk, by 1 float ulp (≤ 0.0002 cents); ends exact.
- Status: decided (OP.9)

## Q?-OP.6-b · 2026-11-24 · OP.6 · What of OP.md F2 / F6 / F12 held, and what is left in OP.5's files
- F6 held for cents → Hz only (Wavetable Synth −14 … −16 %, session −3.6 %). Not there: the default instrument has no
  cents → Hz kernel in its profile (expected −3 %: 0); `tuningHz8` in the scenarios is on its unmodulated COPY path
  (every sample of it falls in the check and the two copy loops; `tuningCoefficients` / `exp2Scalar` do not appear),
  so "its constant path does eight exp2 and log2 per group per block" is not what costs 3 – 8 %. Removing that copy
  means the schedule aliasing the node's outputs to its inputs while the tuning is unmodulated (`schedule.cpp` /
  `codegen.cpp`, a run-time condition: not a kernel change).
- Not tagged for the reuse, measured: exp / log / pow / tan (2 – 7 ns per 8 values; a moving input paid +6 … +25 %).
- The JIT's own helpers (`rt::centsToHzV`, `dbToGainV`, … in `runtime.cpp`: 1 – 2 % of the session and the Wavetable
  Synth) are stateless `void(float* io)` calls inside generated loops: reusing a repeated vector needs a per-call-site
  slot (or hoisting the call out of the loop for a block-constant operand) in `codegen.cpp`. Left for OP.5 / OP.7.
- F12 phasor (Hz → increment through double per sample): emitted inline by `codegen.cpp` (Atom::Phasor); nothing in
  the kernels or the runtime is involved. Left.
- F12 `reduceKernel` (image_host.cpp, OP.5's file; 2 – 6 % of every synth profile): left. A transposed 8-frame sum
  CAN keep the bits, but not in `mixOutputs`' hadd order: `horizontalSum` adds ((v0+v4)+(v2+v6)) + ((v1+v5)+(v3+v7)),
  so the 8-frame form must add each frame's low and high halves first (two frames per ymm through permute2f128),
  then the movehl and movehdup steps as in-lane shuffles, then gather the 8 sums and add them to `out` in one vector.
- F2 (b) "the spectrum's FFT runs per painted frame" does not hold: `SpectrumFeed::work` runs on the feed's own
  thread, reads only frames the tap has written since its last pass (`pos_ ≥ written` → nothing), transforms once per
  hop, and the faces copy a snapshot only when its sequence moved (`snapshot(seq, …)`, `scopeEnd`, `stereoRead`, …).
  Its thread was 0.0 % of the samples with a scope playing. Nothing changed.
- F2 (a) held: the site is `ScopeTracker::fftOf`, through the frame's retry on a third of the window when nothing is
  pitched (two padded lengths per frame); bench/RESULTS.md.
- Status: note

## Q?-OP.5-a · 2026-11-24 · OP.5 · What the fusion left, and the extras that were not done
- Context: OP.5's order rule is greedy (stay with the form of the last item, calls first at a stage's start); it has no
  lookahead. A call that needs one inline value (`delay(gate_to_real(gate))`) still splits a stage's inline items in
  two, where "that value, the call, then everything inline" would not. Not seen in the Operator, the default
  instrument or the Wavetable Synth.
- The Operator's fused envelope chain is throughput-bound (177 instructions per frame for four envelopes). What would
  shrink it is in the patch, not in the loop structure: each envelope has its own `rise(gate)` and its own selects on
  the gate (common subexpressions across module instances: the optimiser, with stateful CSE for `rise`), and F10
  (envelopes at a fixed point still run) now takes most of what is left.
- Extras offered with the bit-exactness change, measured or estimated, NOT done:
  A. `reduceKernel` as an eight-frame sum: built bit-identical, measured 0 ± 1 % in every synth row, reverted.
  B. Phasor Hz → increment hoisted for a block-constant Hz: in these instruments Hz is a block (the tuning kernel's
     output times a ratio), so "constant" is a run-time property: a compare with the previous frame and a branch
     inside the fused chain's body. The default instrument's phasor now runs in the envelope's shadow (0 to gain); in
     the Operator it is ≈ 40 of the 300 cost units of a throughput-bound chain: ≤ 1.5 % of the unit. A float
     increment would be cheaper still but breaks the exact pitch ratios (rule 3).
  C. Reuse slots for the stateless `runtime.cpp` helpers: a per-call-site slot in the host layout; 1 – 2 % of the
     session by OP.6's profile. Not started.
  D. The master / bus sum skipping silent inputs (Q?-OP.4-a): an n-ary sum op in the schedule and both backends for
     ≈ 0.1 % of a core at 64 tracks. Not started.
- Options: 1. leave all four. 2. C as its own small task after OP.7 (same files). 3. the cross-instance CSE as an
  optimiser task (it also helps every multi-envelope patch on the interpreter).
- Choice (provisional): 1 now; 3 is the one with a real return.
- Status: open

## Q?-SX.3-a · 2026-11-27 · SX.3 · The clip gain's dB readout left the title strip
- Context: the plan replaces the small badge text (mode, BPM, transpose, Rev, gain dB) with the warped tag. The tag
  carries the mode and the pitch details; the source BPM and the gain dB had no place in it.
- Options: (a) drop both (the waveform's height shows the gain; the clip panel shows both numbers); (b) keep the gain dB
  as plain text left of the tag; (c) show it only while the gain handle is hovered / dragged.
- Choice: (a), the most reversible; (c) would be the natural follow-up if the number is missed.

## Q?-RT.1-a · 2026-11-27 · RT.1 · What counts as "B feeds" for the Audio From loop rule; how a mixer-out start is stored
- Context: the plan refuses a source B feeds "through outputs, sends, cables or B's group". Every track implicitly sends
  to every return at −∞ (only non-default sends are stored), and Resampling is "the master minus B's own bus". The
  model also had no wire start for a mixer's output (Post Mixer / Resampling).
- Options: sends — (a) every implicit send feeds (no track could record a return), (b) only stored sends feed, with
  the check re-run when a send level changes. Resampling — (a) always allowed, (b) refused when B reaches the master
  other than through its own output route (a stored send, a cable, another track's Audio In). Mixer out — (a) a
  reserved port on `Wire::noteSource`, (b) its own field `Wire::mixerOut`.
- Choice: sends (b) (matches the lowering's "a send that closes a loop is lowered only while stored"; turning such a
  send up from −∞ is then refused with "Audio From would create a loop"); Resampling (b); mixer out (b), allowed only
  onto an Audio In. Group membership is followed through outputs (a child routed past its group does not feed it).
- Status: open

## Q?-RT.3-a · Where the audio track's input section sits in the arrangement header
- Context: plan RT.3 puts Audio From / channel / Monitor in the arrangement header "when tall enough with In/Out shown";
  pqdaw has no In/Out toggle and the header strip is 180 px of fixed lines (volume / activator; pan / output; sends).
- Options: (a) lines 2-3 (Audio From | channel, then Monitor), sends from line 4 on audio tracks; (b) after the sends;
  (c) an In/Out toggle that adds a column, as in Live.
- Choice: (a): record-from is the quick path the user asked for, so it shows at the default height; lines that do not
  fit are hidden as before. Cost: an audio track's sends sit two lines lower than a MIDI track's.
- Status: open

## Q?-RT.3-b · Labels and defaults in the Audio From choosers
- Context: Live's Pre FX / Post FX / Post Mixer are, by the RT decisions, plain outputs: the source, a block, the mixer.
- Choice: the source reads "Clips" (audio track) / "Input" (group, return); a block its name (a rack chain
  "<Rack> | <Chain>", the rack named "Audio Effect Rack" / "Instrument Rack": racks have no name of their own); the mixer
  "Mixer". Picking Ext. In starts at 1/2 (1 when the device has one input). Picking a track starts at its last block
  before the mixer, else its source, else its mixer. A cable from a block off the main path reads back and is listed
  at its place before "Mixer". Until RT.2 lowers Audio In cables they are drawn dim (not lowered), like any unlowered
  wire.
- Status: open

## Q?-RT.2-a · 2026-11-27 · RT.2 · How an Audio In cable reaches the engine; what Resampling and the capture offset are
- Context: the plan says "read like the DY.2 sidechain" (WireKind::Sidechain merges the two tracks into one unit), "a
  second master sum" for Resampling, and "offset = graph latency compensation only".
- Options: wire kind — (a) Sidechain (merge A and B), (b) Audio (a bus like a send: compensated the same, no merge).
  Resampling — (a) the master's input sum without B, before the master's devices, (b) the master again without B: every
  group on B's route and the master lowered as SHADOW graph tracks (same patch and params) reading all but B's route.
  Offset — (a) B's unit arrival A_u, (b) A_u + T(B) always.
- Choice: kind (b) (no unit merge, so recording never couples compiles; both are latency-compensated). Resampling (b):
  exactly Live's "Main output minus the recording track", master devices included; costs a second master (and route
  group) chain only while a track resamples, and raises the output latency by the master chain's own latency then.
  Offset: A_u, plus T(B) only when the source is in B's unit (no bus between them). Takes from a cable are stereo files
  (a mono source duplicated), named "From <track>" / "Resampling".

## Q?-BR.J-a · What the MOS circle's reset resets
- Context: bug 2026-11-27_16-16-48 asked for a reset button "when you mess up your mos circle intervals". The circle
  already had a "× reset" for MODMOS alterations, but it was small, sat among the centre's labels, and only appeared
  while something was altered.
- Options: (a) reset clears the MODMOS alterations only: the MOS's own degrees, with generator, mode and region kept;
  (b) it also returns the mode to rotation 0; (c) it also sets the generator to a default hardness, e.g. L/s = 2;
  (d) it restores the tuning as it was when the face or wheel opened (the popover's Revert already does this).
- Choice: (a), moved into a permanent "reset intervals" button in the bottom-left corner. It is the most reversible
  option, and the generator and mode are the user's own choices, not "intervals".
- Status: open

## Q?-RT.4-a · Where automation mode's parameter chooser goes in the condensed header
- Context: RT.4 put volume + pan on the name column's line 1 and an audio track's Audio From + channel (a MIDI
  track's / group's tuning chip) on line 2. The automation-mode parameter chooser used to sit under the chip.
- Options: (a) the chooser takes the name column's line 2 while automation mode is on (the chip / input choosers
  hide; Monitor stays); (b) it goes on line 3 and the sends move down a line in automation mode; (c) it replaces
  volume + pan.
- Choice: (a). Volume and pan stay visible (the user's point), the change only lasts while A is on, and it is one
  branch in TrackHeader::arrange plus MixerStrip::setInputLineHidden. Related: returns now default to two lines (40
  px, was one) so their volume stays visible under the name.

## Q?-BR.K-a · Out of a group at its head when other blocks read the group's In too
- Context: bug 2026-11-28_00-43-14. Taking a block out of a group now keeps its signal path through the group's ports
  (edit_paths.cpp setDeviceGroup): at the group's head it moves before the group (the group's sources feed it, it feeds
  the group's main input, and what it fed inside reads the group's In). When other blocks inside also read the group's
  In (e.g. an LFO or a second oscillator reading the notes), moving the sources away would starve them, and one In
  cannot carry both the notes and the block's audio.
- Options: (a) the block reads what the In carried, directly; the group keeps its sources; what the block fed inside
  keeps the wire straight from it (it crosses the boundary); (b) put it in series anyway and let the other readers read
  its audio; (c) refuse the move.
- Choice: (a). Nothing that played before stops playing, nothing is mixed into the In, and it is one undo step away.
- Status: open

## Q?-FD.2-a · 2026-11-28 · FD.2 · A cable into a control: a lane cable is per lane, a stereo cable still gives its left
- Context: the plan lists "a wire into a param carries channel 0 only" as a stereo assumption to remove ("a width-N
  wire into a param makes that param per-lane"). With N = 2 that would change every existing stereo → control cable:
  the control and all it feeds become stereo, and a Utility's output fed back into its own Gain becomes a 4-channel
  error (tests/app/test_groups.cpp "groups-fix1: a feedback loop…" failed that way). The plan also says stereo
  behaviour must not move.
- Options: (A) every channel, always. (B) a lane cable (wider than stereo) carries every lane; a mono / stereo cable its
  first channel as before. (C) a per-wire switch.
- Choice: B. The lowering decides before inference, from the document's estimate (`lanes.h laneWidths`). A 2-lane
  cable (Lanes at N = 2) is a stereo cable in every respect. Across tracks a cable into a control carries its first lane
  (a control's bus is one channel). Reversible: A is one condition in `lowerSites` and one in `laneWidths`.

## Q?-FD.2-b · 2026-11-28 · FD.2 · What refuses a lane cable, and what does not yet
- Context: "stereo-only consumers refuse width > 2 with a readable reason". The mixer does (a `StereoOnly` bound per lane
  cable into it). For modules I first put a setting on the stereo-by-nature groups; it moved two project fixtures and
  the lowering text goldens (every Utility lowered as another callee), so I derived it instead: a group is bound to the
  width it takes when every cable out of its In ends on a `channel` block or a declared-width engine
  (`LaneWidths::bound`), and only while a cable of another width reaches it. That covers the Utility (pan, width, mid /
  side), Lanes, a Merge / Unpack of another N and the engine modules (Wavetable, Envelope, Noise, Sampler) with no
  per-module rule, and leaves every stereo project's IR as it was.
- Not refused, and wrong on a lane cable: an EQ in L/R or M/S mode and the Compressor's / Limiter's detector read the
  first two lanes (they also pass In on, so the rule does not bind them; the Limiter then does not limit lanes 2 …).
  Noise (and every native engine) is one mono instance: broadcast onto a lane cable it is the same noise on every lane —
  a per-lane Noise needs a seed per channel inside the engine (a kernel change, out of FD.2's scope).
- Options for those: (1) a "stereo only" setting on those three modules (document change, their fixtures re-recorded);
  (2) build their detectors per lane (Limiter: max over all lanes); (3) leave. Choice: 3 for now — nothing in FD.3 – FD.6
  puts them on a lane cable; (1) is a two-line change in `LaneWidths::bound` when wanted.

## Q?-FD.2-c · 2026-11-28 · FD.2 · graph: a bound param takes any width; two widths at an atom are blamed
- Context: `infer.cpp` bound a callee `param` connected at a call to mono (why the lowering took channel 0), so no
  control could be per lane. And two widths meeting at an element-wise atom had no blame: a stereo cable and an 8-lane
  cable into one Math module failed the whole unit (silence, a status line) instead of cutting a cable.
- Options: keep the mono bound and lower a lane-driven value block as another patch (an `in` instead of a `param`); or
  drop the bound.
- Choice: dropped (infer.h rules): a signal of N channels into a param makes it per channel, as into any element-wise
  input — one rule for every module. In the patch language `f(cutoff: stereoSignal)` now makes the callee stereo where
  it was a width error. The meet error is blamed on the edges that brought every width but the atom's first wide
  input's (input order), only with `InferOptions::blameDepth > 0` (the link's compile): the cable that brought the
  second width is cut and says "8 lanes and a stereo signal meet here".
