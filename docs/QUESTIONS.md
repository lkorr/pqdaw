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

## Q?-AS.2-a · 2026-11-13 · AS.2 · Takes an untitled autosave does not reference; an outdated autosave's folder
Context: a crashed untitled session can hold takes recorded after its last autosave (or removed from the document);
and a titled session's autosave that is older than its origin's project.pqd is not offered (plan), so it never gets a
Discard button. Options: (a) place unreferenced takes on a "Recovered" track (as L13's Open as Project) / leave them
unplaced in the new folder; (b) delete an outdated autosave silently at start / keep it (findStaleUnsaved never deletes).
Choice: (a) place them (one undo step "Recover Recordings"; nothing is hidden). (b) keep it: the folder is offered for
its takes only (Discard then removes the whole session); a session left with only an outdated autosave stays on disk
(small; one project.pqd). Reversible: (b) is one `removeUnsavedFolder` call in ProjectLink::staleRecordings.
Status: open.
