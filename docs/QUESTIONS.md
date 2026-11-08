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

## Q?-TX.group-oversample-a · Nested oversampled groups, members with a region of their own, loops through the edge
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

## Q?-TX.group-oversample-b · Every group's Device-view frame now has an inspector column
- **Context**: the task asked that every group's inspector shows the setting; before, only an instrument group had an
  inspector (POLYPHONY).
- **Choice**: every group frame shows one: 96 px with OVERSAMPLE only, 150 px with POLYPHONY on an instrument group.
  This widens every group in the Device view (20 view goldens moved). Alternative if it reads as clutter: show the
  inspector only while the setting is above 1× and offer it from the frame's title-bar menu instead.
- Status: decided (reversible: group_frame.cpp).
