# Progress log

Newest entries at the bottom. Format is in CLAUDE.md ("Logging your work").

Entries up to 2026-11-06 are in docs/archive/PROGRESS-2026-10-20_to_11-06.md.

## 2026-11-07 · GU.1 Curves · task/GU.1-curves (worker; merged by the orchestrator)
- Did: one curve model + evaluator (pq/dsp/curve.h: min/max/unit/step, ≤ 16 segments Linear/Exp/Log/Power/S-curve with bends, wrap/signed/dB→gain; presets Linear, Octave (exp alias), Exponential, Logarithmic, Exp0; inverse curvePosition nearest to the current position); native `curve(x, convert, slot)` atom specialised per class (1 linear, 1 Exp, multi-segment), data swapped atomically, interpreter == JIT bit for bit; schema 20: every non-rack param stores its knob position (presets, lanes, macros, nested meta params; migration 19→20 through the library's curves); undriven controls = param with no op (also in instrument groups), driven = position + Add/Travel wires → curve atom → legacy Units wires; 4096 curve slots on the link; faces' curve menus switch presets; doc::KnobCurve / ParamCurve / meta::Curve / per-curve atom chains removed; VCA law untouched; PATCH_LANGUAGE.md, ARCHITECTURE.md (Curves). Q219.
- Verified (lighter, user request): build.ps1 -NoTests (check, all targets) 0 warnings; ctest dsp/jit/graph/document/views (default + slow) pass; playback 5/5 incl. slow after fixing a double conversion in test_mod_all / test_meta_wavetable helpers; default tier meta 45/45, engine 128/128, app 35/35, stdlib 72/72, library 19/19, render 16/16, project 14/14, audio 17/17, smoke 8/8; ship pq_bench_dsp links. No -Affected / -Full.
- Perf: ship, load 3 %, 8 voices audio rate, ns/frame: linear 1-seg 1.51 (old mul+add ≈ 1.05), Octave 1-seg 3.32 (old ≈ 3.2), Exp 16-seg 20.2, mixed 16-seg 67.8, dB 13.9 (old ≈ 12.3). bench/RESULTS.md.
- CPU-efficiency check: undriven knobs cost nothing per sample; single Exp through zero runs as a·e^(cx) on plain exp (SVML expm1 was 3.5×); class branch per call. Follow-up: the 1-segment linear path is +0.45 ns/frame over the old mul+add (worth folding into an affine op in the lowering).
- Notes/blockers: meta-cables fixture −117.1 dBFS vs its pre-GU.1 render (worst at start; float approximations of the same curve): accepted by the orchestrator as inaudible. Automation ramps now interpolate in knob position (a 200 Hz → 6 kHz ramp passes ≈ 1.1 kHz at mid, was 3.1 kHz). Racks / macros still use travel code (0…127). Some view tests compare at 1e-12 (positions read back within float rounding). 12 view goldens re-rendered (1 kHz readout "1.00 kHz", device-automation ramp); all-free IR hashes re-recorded, two render hashes (free_wires, meta-cables) re-measured. tests/support/knob_positions.h converts unit values for tests.

## 2026-11-07 · GU.2 Nested groups, atom and value blocks, group presets · task/GU.2-nested-groups (worker; merged by the orchestrator)
- Did: groups nest (checkList refuses only cycles and depth > kMaxGroupDepth 16; remove / move take descendants); Ctrl+G inside a capsule groups by membership only (no rewiring), Ctrl+Shift+G hands members to the parent; a path group can be grouped again; instrument rule: voiceContexts decides outer groups first, only the outermost fed group voices, nested ones play GroupVoice; SetDeviceGroup now Structural (membership decides per-note since IG.1-2); atom blocks `atom:<name>` (patch built from the signature with the graph API, cached, all allowed atoms in the DeviceLibrary; node-menu atoms spawn them); Value block (one control → output, GU.1 curve); doc::BlockSet copyBlocks / remapBlocks / pasteBlocks (one undo); group presets (.pqpreset with "wires"; browser + node menu); views: recursive capsules, drops into the innermost open capsule, instrument capsule look (orange accent, "Instrument · 16 voices"), Ctrl+C / Ctrl+V, Copy / Save as Group Preset; ARCHITECTURE, REVIEW 👁, Q220.
- Verified (lighter, user request; default tier only): document 213 cases, playback 4/4, views, app, library, meta, project, render: pass, 0 warnings; new test_blocks, test_nested_groups (render diff 0, interp == JIT: nested = ungrouped, atoms grouped twice = series, instrument with nested group = flat, paste = original, undo, preset round trip), test_nested_groups_view (real dispatch: Ctrl+G / Ctrl+Shift+G, copy/paste into a capsule, menu click, Tab spawn of a preset). views perf label: lane re-layout 2.017 ms vs 2.0 bound once (A/B unchanged).
- Perf: ship pq_bench_project_patch 64 tracks, base vs branch interleaved ×3: lower 6.10 → 6.02 ms, model 1.236 → 1.215, view rebuild 1.774 → 1.769, lane re-layout 1.844 → 1.834 (noise; load not recorded).
- CPU-efficiency check: atom / value blocks flatten to the atom / a param (undriven Value: no op); nested plain group = identity call; voice contexts computed only for nested groups (a first version cost +5 % model build).
- Notes/blockers: goldens: new project-patch-nested; 8 re-rendered for the instrument capsule look (REVIEW 👁). No schema bump. Atom blocks have no knobs; voice.* atoms not blocks yet; clipboard per session; group presets listed with audio-effect presets.

## 2026-11-07 · GU.3 Panel model, generic elements, collapsed group drawn as its panel 👁 · task/GU.3-panels (worker; merged by the orchestrator)
- Did: document panels on groups (pq/document/panel.h: Panel / PanelElement {id, kind, rect, style fields, bindings by DeviceId + meta path: Control|Setting|Data|Signal|Panel, visibility condition}; Device::panel; SetGroupPanel (Meta) + ops::setGroupPanel / addPanelElements / setPanelElement / removePanelElements / setPanelSize; bindings only inside the group or its own settings; elements leave with their blocks (one undo restores both); schema 21 + migration; remapBlocks remaps bindings; doc::groupSettings() for polyphony). Views (pq/views/panel/): registerElement(schema, factory) with style schemas and slots; generic kinds knob (arc/pie/dot/line…), slider h/v, number, toggle, button, menu, XY, label, shape, tabs, curve editor (points, bends, add/remove, shapes + presets via setParamCurve), mod source; GroupPanelFace (static layers from a process-wide tile cache keyed by kind+style+size); DocPanelTarget; PanelHost. A closed group with a panel is drawn as it; the Device view's group frame shows the panel and an instrument group's polyphony inspector (voices, mono, legato, glide, steal). ARCHITECTURE (Group panels, schema 21), REVIEW 👁, Q221.
- Verified (lighter): build.ps1 -Target pq_tests_document,views,ui,app,library,playback (check): 0 warnings, all pass (playback 6/6 incl. slow + perf after fixing the worker's own wrong expectation); new test_panels (6), test_group_panels (10, real dispatch + goldens), test_panel_link.
- Perf: ship pq_bench_group_panel, load 12–21 %: 200-element panel repaint 0.109 ms settled, 0.126 ms with a value written (≈ 0.55 µs / element).
- CPU-efficiency check: UI thread only; panels are Meta (never lowered); one tile per kind+style+size per zoom; resolutions keyed by block identity, readouts cached by (position, generation) (value frame 0.198 → 0.128 ms).
- Notes/blockers: no in-app panel editor (GU.5); circuit selection doesn't scroll the Device view to its group; wires into a control on a closed panel end at the edge pins; unbound tabs keep their selection per view. Goldens new: panel-elements(-xy, -2x), project-patch-group-panel, device-group-panel-elements.

## 2026-11-07 · GU.4 PA and VCA as group presets; Meta Operator deleted 👁 · task/GU.4-pa-vca-presets (worker; merged by the orchestrator)
- Did: Phase Accumulator and VCA are group modules of atom + value blocks built in code (pq/document/modules.h) with generic-element panels (PA: In → add ← Frequency (signed Exp0) → mul ← cents_to_hz(Pitch) → phasor → + Phase (wrap) → frac → × Amplitude → Out; VCA: In → mul ← Value → Out, 360° knob + curve editor). VCA law = CurveShape::Joined / CurvePreset::Vca (double; ≤ 4e-11 abs vs TMM.14). Deleted: PA / VCA meta types, expansions, faces; unitsInBody / noKnobCurve / unitsLaw / Builder::units; Meta Operator (pqp, panel, catalogue, benches, tests, fixtures). Retired specs kept for decoding (pq/meta/retired.h). Load conversion convertRetiredModules (parseProject after migrations, parsePreset): old ids kept, wires / lanes onto inner value blocks, unwired VCA → Value block, modules in rack chains refused at load. Value block param unranged (engine clamped it to [0,1]). Spawning via node menu / Add Free Device / browser. ARCHITECTURE, PATCH_LANGUAGE, REVIEW (docs/reviews/GU4/*-old-vs-new.png), Q222.
- Verified (lighter, user request): check build of 8 test exes 0 warnings; meta 28/28, document 214/214, app 35/35, engine 128/128, library 19/19, views 602/602, stdlib pass; playback 27/28 entries → slow fixed (stale IR hashes, a double grouping in mod_all), each fixed part re-run alone; test_module_presets 4/4; interpreter == JIT on gu4 fixtures; ship pq_bench_playback / pq_bench_dsp link. SKIPPED (user: "we dont need any playback or audio testing"): whole playback slow entry after the mod_all fix; playback shards after the final rebuild.
- Perf: ship compile interp / JIT ms base → branch: pa_voice 12.65→12.27 / 78.6→76.5, pa_free 12.58→11.81 / 63.1→51.4, vca_law 15.37→11.62 / 170→47, vca_driven 15.09→11.81 / 147→56; play JIT ns/block: pa_voice ≈710→734 (noise), vca_law 408→442, vca_driven 1760→989 (−44 %). Load 4–13 %.
- CPU-efficiency check: undriven knob folds to a constant; driven = one curve atom; PA's + pm only when pm was wired; VCA uses mul, not the 2 ms gain.
- Notes/blockers: renders: PA bit-identical; VCA −112 / −136 / −112 dB re peak (not diff 0: the old law was float). VCA markers, PA phase orbit / voice dots gone until GU.7 rich elements. Modules spawn open (closed shows the panel). test_project_patch param-count bound 300 → 280 (fewer params exist). 23 goldens re-recorded, 2 new, 4 deleted (REVIEW 👁).

## 2026-11-07 · GU.5 The panel window and send-to-panel 👁 · task/GU.5-panel-window (worker; merged by the orchestrator)
- Did: floating panel window (pq/views/panel/panel_window.h; project_patch/panel_windows.cpp) from the capsule title-strip button or block menu: move, resize (zoom to fit), minimise, close, reopen; state in ViewState::panelWindows. Send to panel: selected control (face/panel control, pin, value block) via P, the window's +, block drop, or cable drop; default kind by type; one undo step. Edit mode: select / frame / move / 8-handle resize / snap 4 px with guides (Alt free) / align / distribute / reorder / Delete / Ctrl+D / arrows; one SetGroupPanel on release. Generated inspector (kind, binding, visibility, rect, schema style fields, curve editor). Play mode: mod-source drag makes the cable (PanelTarget::connect), modulation ring. Link lines in the circuit. Wires into a closed panelled group end on the element. ARCHITECTURE, REVIEW 👁, Q223.
- Verified (light): build.ps1 -Target pq_tests_views,pq_tests_ui,pq_tests_document,pqdaw (check) 0 warnings; ctest views/ui/document 3/3; test_panel_window 8 cases real dispatch. Lane re-layout perf-label check 2.25 ms vs 2.0 under load (not re-measured).
- Perf: not measured (user: light testing).
- CPU-efficiency check: idle = no frames; drags don't touch the document until release; link lines only while a window is active / hovered / its group selected.
- Notes/blockers: dropped: ship bench, exe smoke test, W-drag test, minimise-by-double-click and align-button tests; circuit selection → Device view still open. Goldens: 5 new panel-window-*, 9 re-recorded (5 not inspected by the worker).

## 2026-11-07 · GU.9 Racks are groups · task/GU.9-racks-are-groups (worker; merged by the orchestrator)
- Did: Rack / Chain / ChainId / MappingId / MacroMapping / variations / zones / ParamKind::ChainMixer, the rack lowering path, rack serialization and rack-only commands deleted (old racks refused on load). A rack = a group whose chains are child groups fed from its In (doc::rackChains, derived from wires), summed into Out by wires; Instrument Rack = instrument group of chain groups. 8 macro value blocks per rack on its panel; ops::mapMacro = Travel wire (amount max − min, knob at min). ops::addChainSelector: "Chain Select" value block + per-chain gate from real atoms. ops::addRack / addChain / addToChain; rack spawns build the group form. Lowering: Travel wires from any output; instrument voices end when every branch to Out ends at an ender. Device view rack_view.cpp (chain list, + Chain, Up/Down, macro panel, chain circuit). ARCHITECTURE, REVIEW 👁, Q224.
- Verified (light): build.ps1 -Target document, views, app, playback, pqdaw (check) 0 warnings, 7/7 ctest entries; test_racks (one-chain rack = default group, two chains = sum, selector gates, macro = hand knob; all diff 0, interp == JIT, RtScope); test_racks_view (real dispatch). Not run: engine / library / others, slow tier, benches.
- CPU-efficiency check: rack = group identity call; undriven macros no op; mapping one Travel term; selector gates param-rate + one mul per chain.
- Notes/blockers: zones (need voice atoms as blocks), zone fades, chain volume / pan / solo, macro variations, Map mode, selector UI button left out. Many rack-only tests deleted with the feature (device affordances down to 2 cases). Goldens audio-rack / instrument-rack deleted, device-automation re-recorded.
- GU.9 (effcd6dc) reverted (7eff8172, f8f3979c): the user rejected racks as one group of chain groups and the stripped rack features; to be redone as parallel groups with the Device-view rack kept as a view.

## 2026-11-07 · GU.5b Modules look as before; an opened group is a circuit 👁 · task/GU.5b-module-faces (worker; rebased onto the GU.9 revert and merged by the orchestrator)
- Did: groups with a panel close the first time the view sees them (modules spawn closed). Rich elements dial (VCA 360° knob, interior curve points as mirrored draggable markers, Ctrl single, typed place, chevron popup with range / curve / presets), arc (PA controls on arcs with ticks), orbit (PA centre: ring, amplitude-lit hub, voice dots via PanelTarget::voices) in src/views/src/panel/rich_elements.cpp; Element outline (round panels → orbiting ports), popups, voice polling. VCA preset split at ±10 / ±100 (dsp::splitCurveAt; law unchanged at 1e-12); PA Pitch ±12 st over the middle half. Opened groups laid out by graph::autoLayout (In left, Out right), straight traces inside, atom blocks as circuit symbols, value blocks as small knobs. ARCHITECTURE, REVIEW 👁 (docs/reviews/GU5b/), Q225.
- Verified (light): worker: views 585/585, ui, document, dsp curve 11/11, test_module_faces (real dispatch: marker drag one undo no recompile, knob, popup + preset, PA arc, open / close by double-click). Orchestrator after rebasing onto the GU.9 revert: build.ps1 -Target views, ui, document, pqdaw (check) 0 warnings, 3/3.
- Perf: not measured.
- CPU-efficiency check: layout once per opened group per model rebuild; dial re-derives only on curve hash change; marker drags stay within the curve class (no recompile); the VCA's driven path evaluates 4 more segments.
- Notes/blockers: PA voice dots need GU.7's lowering tap; no captions / PA leads; pitch continuous; Fader preset approximated; pq_tests_app (VCA render at −110 dB with the 7-segment curve) not run. ~25 goldens re-recorded.

## 2026-11-07 · GU.6 Embed and break apart · task/GU.6-embed (worker; merged by the orchestrator)
- Did: a child group with a panel sent to its parent's panel (P, +, drop) becomes an embed element (kind "panel", bound by DeviceId) drawing the child's panel live, passing pointer / keys / popups / voices / outline through; any depth; wires into an embedded control end on it. ops::breakApartPanelElements (one undo, "Break Apart"; right-click or Ctrl+Shift+G in edit mode): copies at the same places, same bindings, fresh ids, visibility renumbered, one level at a time. Fixed: the GU.5 panel editor acted on key releases (double nudge / duplicate). ARCHITECTURE, REVIEW 👁, Q226.
- Verified (light): build.ps1 -Target views, ui, document, pqdaw (check) 0 warnings, 3/3; test_panel_embed 3 cases real dispatch (send, embedded knob edit, rename survival, break apart + undo, move out + undo, depth 2). Golden panel-embed.
- Perf: not measured.
- CPU-efficiency check: UI thread only, never lowered; embed rebuilt only when the child's panel pointer changes; artwork from the shared tile cache.
- Notes/blockers: groups without a panel can't be embedded; break-apart copies lose the child's frame; a mod-source cable from an embed reaches only that child's controls.

## 2026-11-07 · GU.7 Rich elements; Wavetable, Envelope and analysers as group presets 👁 · task/GU.7-rich-elements (worker; merged by the orchestrator)
- Did: Wavetable, Envelope, Oscilloscope, Spectrum, Spectrogram are groups In → native engine (leaf) → Out (engineModuleBlocks / enginePanel / moduleOfEngine), spawn closed looking like the old faces; project spawn lists show the modules. Rich element kinds drawtools, harmonics, wavetable, warpmode, envelope, scope, spectrum, spectrogram, meter (src/views/src/panel/engine_elements.cpp) as sections of the existing face classes; plain controls generic; elements on one engine share one face. Data / Signal bindings; a placed display gets a lowering tap (structural: one recompile); PA orbit binds frac and its voice dots show. ViewContext::setFaceVoices / setSignalTapSource; GroupPanelFace passes wheel, polls live readers; bottom shape with plate = panel corners. ARCHITECTURE, REVIEW 👁 (docs/reviews/GU7/), Q227.
- Verified (light): build.ps1 -NoTests (check, all) 0 warnings; ui, meta, document, views pass; test_engine_elements 5 (real dispatch harmonic / breakpoint drags, one undo, no recompile); test_engine_modules 3/3 (Wavetable / Envelope modules = bare engines bit for bit, interp + JIT; PA orbit tap). test_node_menu updated (module:<id> entries). Open, from earlier phases: test_module_presets pa_voice / pa_free (−2 dB: GU.5b Pitch curve) and segments == 1 (GU.5b VCA split); test_curve_renders −117 vs −120 (GU.1, accepted).
- Perf: not measured.
- CPU-efficiency check: unplaced displays cost nothing; modules add no ops (render diff 0); engine sections draw static layers directly (no tiles: depend on engine data).
- Notes/blockers: meta types and faces not deleted (rack chains, user-patch meta calls, engine leaves still use them); Wavetable at an instrument In hears Hz not phase; signal-bound display state not saved.

## 2026-11-07 · GU.9 v2 Racks are parallel groups; the rack is their Device view 👁 · task/GU.9-parallel-groups (worker; merged by the orchestrator)
- Did: Rack / Chain / MacroMapping, ParamKind::ChainMixer, rack lowering and file format removed. A rack is derived from wiring (pq/document/racks.h doc::findRacks): sibling groups with one shared feed and one shared sum (≥ 2, or one holding a chain mixer). Each chain holds a Mixer group (Volume / Pan / Gate value blocks + atoms, same arithmetic); zone groups (key / velocity / chain) from value blocks + voice-atom blocks, only while on. Macros = value blocks (mapping = Travel wire, min / max knob positions); selector a value block; variations = var<k> settings; mute / solo drive the Gate. Edit builders (one undo each): addRack, addChain, addToChain, moveIntoChain, deleteChains, duplicateChain, setChainMixer / Active / Solo / Zone, mapMacro, variations, Rand, Per-Voice Chains. Voice terminals and pack are atom blocks; a nested group's Out-terminal wires no longer count as inputs (fixed Instrument Rack 2×). Device view: today's rack frame re-pointed at derived racks (chain list + mixer, zone editor, macros, Map mode, variations). Circuit: other chains stacked under the path chain. ARCHITECTURE, REVIEW 👁, Q228.
- Verified (light): reference renders of the old racks recorded first (tests/app/fixtures/gu9_racks): test_racks AE rack + macro diff 0, + chain mixer diff 0, Instrument Rack diff 0, velocity zone 3e-8, interp == JIT, RT trap clean; test_racks_view (ported T4.8 view tests, real dispatch); audio-rack / instrument-rack goldens unchanged; build.ps1 -NoTests 0 warnings; document, views, app, playback: 5/7 after rebase — s1 / s2 fail in test_curve_renders / test_module_presets, the GU.5b / GU.1 leftovers GU.8 is fixing.
- Perf: n/a.
- CPU-efficiency check: off zones have no blocks; mixer / zone math param-rate; one smoothed gain per chain; regression: a muted chain computes (Gate 0) where T4.8 left it out.
- Notes/blockers: "Open Lowered Rack Patch" viewer, 20,000-deep nest test and chain-circuit bench deleted; tests trimmed by the reverted attempt not restored. Unmapped macros have no block; mapping curves dropped; unnamed variations.

## 2026-11-07 · GU.8 The example instrument, style schemas, outline 👁 · task/GU.8-example-instrument (worker; merged by the orchestrator)
- Did: (0) the "three playback failures" were one bug: convertPa copied an old PA's pitch knob position onto GU.5b's new Pitch curve (≈ −12 dB everywhere); it now keeps the pitch in cents (pa_voice / pa_free diff 0, gu1_curves −137 … −400 dBFS; test_curve_renders bound unchanged); test_module_presets expects the VCA split at +100 (2 segments, checked). (1) Wavetable Synth (doc::wavetableSynthBlocks): instrument group Osc PA → Wavetable → Envelope + two LFO pairs (free PA → Wavetable → mul ← Depth → Units wire into position / Pitch); panel tabs Osc | Env | LFO 1 | LFO 2, Pitch always shown, child panels embedded per tab; listed under Instruments; ops::pasteBlocksAtNotes. Fixed: embeds didn't forward engineFace / signalFeed (GU.6 × GU.7 placeholders). (2) Style schemas (pq/views/panel/style_schemas.h): built-in knob Classic / Minimal / Pie, slider Classic / Minimal; inspector Schema apply (one step) / Save as; user schemas in <User Library>\Styles\schemas.json. (3) GU.md "Next native ingredients". ARCHITECTURE, REVIEW 👁, Q229.
- Verified (light): playback shards 4/4 after (0); final after rebase: views, ui, document, playback, pqdaw (check) 0 warnings; views re-run 604/604 after re-recording panel-window-inspector / -edit (Schema row); test_wavetable_synth (chord peak 0.79, interp == JIT), test_wavetable_synth_panel (4 tab goldens by real clicks), style-schema tests.
- Perf: not measured.
- CPU-efficiency check: only existing atoms / engines; LFO Wavetables at ×2 oversampling; depths one add after the curve; schemas / tabs UI only.
- Notes/blockers: LFOs run per voice and restart per note; tab not saved; no schema delete / rename UI.

## 2026-11-07 · GU.10 · orchestrator (merge of task/GU.10-stock-presets)
- Did: fast-forwarded main to fc964e34. Utility, Saturator, LFO and Operator are group presets (`modules.cpp`); the default instrument is a PA → Wavetable → Envelope "Basic Synth" group used by `createTrack`; the seven stock .pqp/panel.json files, the Auto Filter and EQ Eight displays/math, their tests, six device-face goldens and pq_bench_devices moved to `retired/` (README). Tests keep stand-in copies under `tests/stand_in_devices/` via `PQ_STAND_IN_DEVICES` (test debt for GU.11). Q230.
- Verified (worker, light): check build 0 warnings; document, stdlib, library, app, views, playback default tier 12/12 entries; Utility / Saturator match the retired patches within 1e-5 of peak; interp == JIT.
- Perf: not measured. Known regression: Saturator computes all seven curves and the LFO all five shapes per sample (no setting-indexed switch block yet; Q230).
- CPU-efficiency check: see the regression above; Utility unchanged.
- Notes/blockers: a per-voice cable into a control creates a modulation tap that ends voices once silent (open engine bug, Q230); Operator FM routed through `+ pm` to avoid it.

## 2026-11-07 · GU.11 · orchestrator (merge of task/GU.11-one-view)
- Did: fast-forwarded main to 7408bed5. The model holds only groups, atom blocks, value blocks and native engines (checkList refuses anything else); Device::source / chainInput / user patches / .pqdevice, the panel_desc + panel.json system, stock displays, device preview and the project-device circuit editor are gone or in retired/; ops::addDevice takes "module:<key>"; the GU.10 stand-in devices and the PQ_STAND_IN_DEVICES hook are deleted, tests ported to modules; new test_one_hierarchy over every fixture and module preset; a switched-off group now switches off its contents. Orchestrator fix: tests/engine/idle_project_fixture.h imported retired stdlib files (pq_tests_project failing since GU.10); it now uses a test copy of the retired Basic Synth and a biquad lowpass in place of EQ Eight. Q231.
- Verified: worker: check build all targets 0 warnings; document, views, ui, app, library, stdlib, playback, audio, meta default tier 15/15. Orchestrator: pq_tests_project 1/1 after the fixture fix.
- Perf: not measured.
- CPU-efficiency check: nothing new on the audio thread; lowering does less; switched-off groups no longer compute.
- Notes/blockers: bench_inspect names retired stdlib files at run time; code without callers left (app::DeviceInspector, CircuitEditor::Host, lone-stock-instrument lowering branches, metaSites); group-panel knobs lack wheel / Show Automation / Knob Curve / modulation ring; test_mod_all slow sweep not run.

## 2026-11-07 · GU.12 · orchestrator (milestone -Full; merge of task/GU.12-full-suite-fix)
- Did: ./scripts/build.ps1 -Full on main e454820d failed 9 entries (playback_slow, graph, engine, jit_s0/s2/s3, jit_slow_s0/s2, stdlib_slow_s0): tests reading stock patches GU.10 retired, plus the slow wavetable alias suite's held note ending mid-suite (the GU.11 instrument group stops at note-off; note lengthened 256 → 1024 whole notes). Worker fix merged as 3a0612c9: tests/retired_devices/ (test copies of the six retired patches, imports to the live stdlib) for the corpus sweeps, structure, probe and click-sweep tests and pq_bench_inspect / operator_calibration; pq_bench_silence links without EQ Eight (6 links per unit; old "links" rows not comparable).
- Verified: worker: build.ps1 -NoTests (check, all targets) 0 warnings; the 9 entries + stdlib_s0..s3 13/13; bench smoke runs exit 0. Milestone review: docs/reviews/GU-critique.md, follow-ups GU.f1–f9 (GU.f2+f3 running as GU.13).
- Perf: n/a.
- CPU-efficiency check: no product code changed.
- Notes/blockers: none.

## 2026-11-07 · GU.13 (GU.f2 + GU.f3) · orchestrator (merge of task/GU.13-one-panel)
- Did: fast-forwarded main to cc0793db. One panel system: a leaf in the Device view is a synthetic group-system panel (panel::leafPanel, drawn in panel::BlockFrame, which keeps jacks, title bar, activator, fold, preset slot, rename); the rack macro panel is a BlockFrame; DevicePanel, PanelLayout and their targets deleted. Knob parity in the panel system: wheel (one undo step per run; host opt-in, the circuit keeps its wheel pan), type-in, Delete default, Up/Down step, curve glyph, modulation ring, automation envelope at the playhead and LEDs, Show Automation / Knob Curve, cable drops, macro Map mode. Q232.
- Verified (worker): build.ps1 -NoTests (check) 0 warnings; views 572, ui 186, app 34, document 184 cases pass; new test_panel_gestures (real dispatch on a leaf and a group knob). 6 goldens re-recorded.
- Perf: n/a. CPU-efficiency check: per-element damage via Element::liveKey (a value change damages only its control); voice polling only while live.
- Notes/blockers: rack macros get no ring and are not cable targets; unmapped macros have no block.

## 2026-11-07 · GU milestone · orchestrator
- Verified: ./scripts/build.ps1 -Full on main 87c1f687 (check, default + slow; CMAKE_BUILD_PARALLEL_LEVEL=6, CTEST_PARALLEL_LEVEL=4 after an earlier run was stopped for low memory): 0 warnings, 88/88 entries passed (436 s).

## 2026-11-07 · GU.f1 · orchestrator (merge of task/GU.f1-rack-roles)
- Did: fast-forwarded main to 82f7e995. Rack blocks carry a `rackRole` setting (Mixer, Volume, Pan, Gate, zones, Selector) written when made; every by-name lookup in racks.cpp / edit_racks.cpp goes by role; names are labels only. Roles from wiring were not feasible without per-shape matching (Q233). Supersedes Q228 "roles by name".
- Verified (worker): build.ps1 -NoTests 0 warnings; document / views / ui / app default tier 4/4; rack renders diff 0; rack goldens diff 0; new rename / user-"Mixer" tests.
- Perf: n/a. CPU-efficiency check: document-side only; lowered graph identical.
- Notes/blockers: a hand-edited role block keeps its role (Q233).

## 2026-11-07 · GU.f4 + GU.f5 · orchestrator (merge of task/GU.f45-mux-voice-end)
- Did: fast-forwarded main to 7a227722. GU.f4: an undriven value block with a stepped or constant curve that reaches a mux/demux index or select cond lowers as a literal (doc::lowersAsLiteral); changing it is structural (one recompile, swap fade). Saturator ≈ 53 → 14 schedule ops (Analog Clip); LFO 10 / 14 ops. GU.f5: composeUnit adds no voice monitors on modulation taps; voices end by done / audio / release timeout. Q234.
- Verified (worker): build.ps1 -NoTests 0 warnings; default tier graph, engine, project, timeline, render, jit, stdlib, playback, app, document, meta 20/20; ship pq_bench_dsp linked; new op-count, recompile and voice-end tests; GU.10 Saturator / LFO goldens unchanged.
- Perf: op counts above (no preset bench yet, GU.f9). CPU-efficiency check: unchosen branches removed at compile time.
- Notes/blockers: muted rack chains still compute (gain by Gate is not a choice; follow-up: chain mixer via select(gate, y, 0)); Operator's + pm workaround could now be a plain cable.

## 2026-11-07 · GU.f7 · orchestrator (merge of task/GU.f7-cleanup)
- Did: fast-forwarded main to a25f45bc. Dead code deleted (src +193 −1529): lone-stock-instrument lowering and placement, nested meta sites, app::DeviceInspector and engine inspect plumbing, CircuitEditor::Host, the GU.4 PA/VCA load conversion (fixtures re-saved first). Chain mixer Out = mux(Gate, 0, gain(...)): an undriven Gate folds, so a muted chain is not computed (mute/solo now recompile with the ≈ 11 ms swap fade). Operator FM is a plain per-voice cable into the next row's Phase. Q235.
- Verified (worker): build.ps1 -NoTests 0 warnings; default tier document, app, playback s0–s3, views, ui, meta, graph, engine 11/11; PA renders diff 0, VCA ≤ −110 dB, rack renders pass; golden project-circuit-device updated (Operator + pm atoms gone).
- Perf: muted rack chain 0 ops (was the full chain). CPU-efficiency check: reuses the GU.f4 literal fold.
- Notes/blockers: the user stopped GU.f6 and ended the GU follow-ups after GU.f7; GU.f8 dropped (bool wires summing is normal circuit behaviour); GU.f6 and GU.f9 not done.

## 2026-11-08 · EQ.2 · orchestrator (merge of task/EQ.2-linear-response)
- Did: fast-forwarded main to 35f42cbd. Optional per-atom response rules (pq/graph/response_rule.h; 23 atoms incl. z1, the three delay interpolators exactly, mux/select with a known choice, channel/pack; tanh/sin/clip approximate, shaper none); graph::LinearResponse on the inferred patch, loops solved per frequency by complex elimination over the SCC, every (in ch, out ch) pair, evaluate() allocation-free; doc::GroupResponse maps a group's In→Out on a LoweredProject, update() re-reads knob positions through the curves without relowering.
- Verified (worker): engine tests: 40 random linear circuits + M/S matrix vs interpreted impulse DFT within 0.01 dB / 0.1°; document tests (VCA, z1 feedback group, update == relower); build.ps1 -Affected (whole suite, default tier) 45/45.
- Perf: 32 × 9-atom chain, 1024 points: 0.60 ms per evaluate (check preset; 8 points per SIMD lane group). CPU-efficiency check: no audio-thread code.
- Notes/blockers: svf rule waits for EQ.1; no curve through solve blocks, oversample regions, voice reducers or audio-rate-modulated params; a group nothing feeds returns an error.

## 2026-11-08 · EQ.1 · orchestrator (merge of task/EQ.1-svf-atom)
- Did: fast-forwarded main to af2c065f. dsp/svf_design.h (8 modes, matched Vicanek / bilinear, 1–64 stages, Butterworth spread, shelves gain/N; svfResponse from the kernel's own float coefficients), the `svf` atom (kernels_svf.cpp, interpreter and JIT, 8 lanes per AVX2 vector, ≤ 16 sections float, 17–64 double), `atom:svf` blocks, the svf response rule; stdlib `svf` patch renamed `svf_multi`. Fixed on the way: playback_slow's 16,384-params test checked Utility Mute, which has no slot since GU.f4 (literal fold); it checks Gain at −70 dB now. Q?-EQ.1-a filed.
- Verified: orchestrator -Affected -Slow on caa19227: 3 failed (atom inventory test, 3 goldens from the rename / node menu, the playback test above); worker fixed, re-ran graph + views + playback default + slow 9/9; svf tests dsp 10/10, jit 3/3, engine linear response 6/6 (impulse vs response ≤ 0.01 dB / 0.1° where within 60 dB of peak). Goldens re-recorded: circuit-svf, circuit-editor-cost (title / text only), project-node-menu (lists the svf atom).
- Perf (ship, bench/RESULTS.md EQ.1): 1 section 2.3 ns/sample (L8 ≈ 5.3 ns/frame), 8 sections ≈ 12, 64 ≈ 93; audio-rate cutoff 66 (bilinear) / 110 (matched lp) / 490 (matched lowshelf) ns/sample → EQ.1b.
- Notes/blockers: EQ.1b filed (state size, silence rule, audio-rate / ramp design cost, response-rule cache).

## 2026-11-08 · EQ.3 · orchestrator (merge of task/EQ.3-eq-modules)
- Did: fast-forwarded main to b2445ddd. module:eq_band (In → svf → Out; value blocks with bandRole; Frequency one Exp segment, knob travel = display axis log(1 + f/10)/log 2001), module:eq (Mode / Output / Edit Chain, chains A / B with bandChain, matrix as muxes on the literal Mode). Ops in edit_eq.cpp (addBand in frequency order, ≤ 32, removeBand, setBandType / Stages, setEqMode copying A → B, bypass, solo with restore), one undo step each. Lowering: a stepped value block into an atom's const input sets it (wires into it ignored); a mux on a literal block resolved before inference (L/R / M/S chains run mono). PanelTarget::run(EditBuilder). Strip of ordinary elements; kWhenHidden visibility.
- Verified (worker): document EQ.3* 5/5, playback 2/2, views 1/1 (band == svf bit for bit, GroupResponse == matrix of svfResponse products within 1e-9, M/S with B = A == Stereo ≤ −120 dB, Stereo schedules none of B); -Affected -Slow 85/88, the 3 = goldens re-recorded by design (lowering fixtures audio-track.pqp, lower_meta_voice_run.txt; project-node-menu), re-run pass.
- Perf: Stereo 4 stereo svf, 0 matrix ops; L/R and M/S mono chains (half the cost of stereo A). CPU-efficiency check: Type / Stages / Mode fold at compile time.
- Notes/blockers: Mode menu is a plain binding (no A → B copy) until EQ.4; Q?-EQ.3-a.

## 2026-11-08 · EQ.1b · orchestrator (merge of task/EQ.1b-svf-cost)
- Did: fast-forwarded main to f0e15b66. svf state sized by its sections (1 section mono 896 B, was 24.8 KiB; 64 × 8 lanes 44.8 KiB with ramp differences); svf sleeps (Memory atom, SleepStateKind::Decay); straight-line inputs (param ramps) run ≤ 32-frame segments with quadratic coefficient interpolation (≤ −93.8 dB vs per-frame design on 20 ms sweeps, exact design once still); svf response rule designs once per evaluate (AtomSignature::responseCacheBytes).
- Verified (worker): -Affected -Slow 76/77, the 1 = document wavetable save/load timing (22.4 vs 20 ms, load-sensitive), rerun 193/193; ship pq_bench_dsp linked, ^svf/ rows (EQ.1b) in bench/RESULTS.md.
- Perf (release, ns/sample L1): ramp lowpass s1 113 → 21, lowshelf s1 506 → 47, lowshelf s8 3463 → 300; evaluate() 32-svf chain × 1024 pts 12.0 → 1.2 ms; param rate unchanged (≈ 2.4); curved audio-rate inputs +5–15 %.
- Notes/blockers: curved audio-rate modulation still designs per frame (vectorised float design not done); Q?-EQ.1b-a.

## 2026-11-08 · EQ.4 · orchestrator (merge of task/EQ.4-eq-displays)
- Did: fast-forwarded main to 6a33e3bc. `response` element (axis log(1 + f/10 Hz) 0–20 kHz, zoom ±6–48, SpectrumPlot analyser behind with SPEC / IN chips, band curves filled, total with glow, dashed when approximate, inactive chain at 35 %, numbered nodes), all plan gestures through PanelTarget (one undo step each), `phase` element (unwrapped / group delay), `bandtype` element; EQ panel; views::GroupResponses (lowered once per structural revision, prepareMany; total = product of band curves for a plain series chain).
- Verified: worker: pq_tests_views default tier 582 cases incl. test_eq_displays (drawn total == GroupResponse, real-dispatch gestures). Orchestrator: build.ps1 -Affected on 6a33e3bc (worker's run was killed for low memory) passed. Goldens added: eq-default, eq-eight-bands, eq-lr-mode, eq-phase-delay, eq-band-response.
- Perf (check): 32-band drag frame 0.95–1.3 ms mean, worst 1.9 ms; first frame after a structural edit ≈ 13 ms (UI-thread lowering 0.9 ms).
- Notes/blockers: strip Stages box shows "16.0" / "1.00" unlabeled; unwrapped phase of deep cuts flattens the strip; lowering on the UI thread, not the app's compile path; Q?-EQ.4-a.

## 2026-11-08 · TX.insert-group · orchestrator (merge of task/TX.insert-group)
- Did: merged 5bac9256. doc::wireContainer(project, wire) → {track, group}: the innermost group holding both ends (a group's In / Out terminal from inside counts as inside, its main ports as outside; mixer / note-source / cross-track wires: the target track's top level). insertImpl (Z and Tab on a cable) uses it; the target-device group rule is gone. Splice, dissolve and cable-drop spawn checked: not affected.
- Verified (worker): pq_tests_views 1/1 incl. new real-dispatch Z / Tab cases (into a PA control / input, PA out, inside the PA, into its Out terminal, nested); -Affected 24/24.
- Perf: n/a (UI edit path). CPU-efficiency check: ≤ kMaxGroupDepth steps per insert.
- Notes/blockers: none.

## 2026-11-08 · EQ.4b · orchestrator (merge of task/EQ.4b-eq-fixes)
- Did: merged 94596c06. Displays read the app's own LoweredProject (ProjectLink → ViewContext::setLowering, shared; no UI-thread lowering; curves held until the compile arrives, view_change::kResponses); phase wrapped ±180° by default + WRAP / UNWRAP chip; number element decimals / prefix / suffix ("Q 0.71", "×16"); EQ Band panel with response + analyser + phase; embedded panels pass curves and edits through; curve cache keyed on sample rate.
- Verified: worker: pq_tests_views default tier, pq_tests_playback EQ.4b* 2/2 (real ProjectLink), new circuit open/close regression; headless eq-circuit-roundtrip scene in release. Orchestrator: build.ps1 -Affected on 94596c06 passed (worker's run was killed for low memory). Goldens re-recorded: eq-default, eq-eight-bands, eq-lr-mode, eq-phase-delay, eq-band-response.
- Perf (check): first frame after adding a 32nd band 9.2 → 2.5 ms, one 7.8 ms frame when the compile arrives (32 bands prepared); drag 1–1.4 ms.
- Notes/blockers: the user's open/close no-curve bug did not reproduce (candidate causes removed); the worker's live-GUI attempt touched the user's own pqdaw window (rule: workers drive only processes they launched).

## 2026-11-08 · AU.voices · orchestrator (merge of task/AU.voices)
- Did: fast-forwarded main to 57be5cb6. Offline jump-detector renders (≈ −70 dBFS step sensitivity, proven by a deliberate compaction break) ruled out stale voice state, attack, block alignment, Play-triggered swaps, param ramps, voice-end DC and late voice end (abutting / overlapping same pitch, chords, Stop, restart, loop wrap). Fixed: the piano-roll preview played a clicked note for its full length (≤ 2 s) / a key for a beat; releaseAuditions() ends it on mouse-up (≥ 50 ms). ASIO output halves zeroed at every stream start.
- Verified (worker): playback AU.voices* 6/6, engine and views default tier; -Affected -Slow 88/88. Goldens: none.
- Perf: no hot path changed.
- Notes/blockers: the Play click not reproduced in the engine; suspect the app stopping the audio stream after 1 s of engine sleep (every Play from idle restarts the ASIO driver). Q?-AU.voices-a; diagnostic for the user: Play within 1 s of Stop.

## 2026-11-08 · AU.fold · orchestrator (merge of task/AU.fold)
- Did: fast-forwarded main to 8f93a477. Cause: an unwired fold / warp / sync is baked into the table; a drag publishes ≈ 30 tables/s and each swap crossfaded 64 samples (1.3 ms), so the wave stepped. Swaps now fade over 25 ms and a table arriving mid-fade waits (recent tables kept ≤ 0.5 s / 64). Live path: the per-block ramp restarted from the pre-smoothing value (a jump back per move); fixed. Ruled out: the fold curve (static ≤ −109.7 dBFS non-harmonic), phase-domain fold, PA wrap at integer Amplitude, mips, cable-driven fold.
- Verified: worker: AU.fold* 2/2 (fail before), meta wavetable incl. slow alias suites 19/19, dsp wavetable/frame-table 25/25, jit 12/12. Orchestrator: build.ps1 -Affected -Slow on 8f93a477 passed; ship pq_bench_dsp linked. Goldens: none.
- Perf: non-harmonic energy during a fold drag −29.4 → −45.9 dB (warp −42.7 → −59.3, sync −19.8 → −36.8); live steps −32.2 → −48.1 dB; idle cost unchanged, ≈ 0.08 % of a core during a 30 Hz drag.
- Notes/blockers: Q?-AU.fold-a (Saturator aliases: Digital Clip −38 dB at 1760 Hz +18 dB; ×4 oversampling ≈ +15 ns/sample/channel; Type switch clicks), Q?-AU.fold-b (non-integer PA Amplitude jumps per cycle).

## 2026-11-08 · EQ.4c · orchestrator (merge of task/EQ.4c-wrap-lines)
- Did: merged 89a8b5b2. Wrapped phase draws a dotted vertical line (0.7 px, curve colour 40 % / band 30 %) at each ±180° crossing, interpolated in the existing cut pass (no extra evaluation).
- Verified (worker): pq_tests_views default tier 588/588 (16-stage low cut: 8 wraps, each within 1 px of a finer reference). Golden eq-eight-bands re-recorded (only the 8 lines).
- Perf: n/a. Notes: deferred idea recorded in docs/plans/EQ.md (FIR phase lead, after PDC).

## 2026-11-08 · AU.stream · orchestrator (merge of task/AU.stream)
- Did: merged 4501d720. Evidence: ASIO layer clean on every start (no asioMessage, positions +1024 exactly); WASAPI loopback of a −90 dBFS probe tone through FL Studio ASIO shows a dip (75 % → 50 %, ≈ 5 ms) 125.3–130.7 ms into every start without pre-roll (8/8); our WASAPI path 0 anomalies. Fix: no suspend while in use (only after 300 s asleep + unfocused/minimised; foreground restarts at once; WASAPI too); every driver start pre-rolls silence ≥ max(latency + 4 buffers, 0.25 s) with the last 4 xrun-free, bounded 1 s, 5 ms ramp-in, Play queued (≈ 280 ms on FL ASIO, cold starts only); 5 ms fade + drain before app-initiated stops; lock-free stream trace (pq/engine/stream_trace.h) in the F12 Context.
- Verified (worker): audio and engine default tier; -Affected 32/33 → pqdaw_idle_shell budget assumed a stopped stream (audio thread now < 0.5 %, rest 0.1 %), re-run passed. Probes: AudioOutput on the real driver, dip gone 9/9.
- Perf: idle with stream running: FL ASIO 0.62 % of one core (callback 3.5 µs / 21.3 ms); WASAPI audio thread 0.25 %.
- Notes/blockers: Q?-AU.stream-a: FL Studio ASIO also drops 5–10 ms every few seconds in steady playback (driver-internal); suggest Focusrite USB ASIO or WASAPI.

## 2026-11-08 · EQ.4d · orchestrator (merge of task/EQ.4d-delay-view)
- Did: merged 608ac318. Cause: delay was the difference of neighbouring points' unwrapped phase; a 23-stage Q 5.16 allpass at 1.27 kHz turns 4.6 rad per 12.3 Hz point, so turns were lost (−36 ms drawn at a 59.6 ms peak). Fix: a second evaluation on a grid shifted by 1/1000 of a step (exact to 2.5 s at 20 kHz); the unwrapped phase counts turns from it too; GroupResponses caches both. MAG / PHASE / DELAY are three areas in one display, each collapsible by its chip (3 : 1 : 1, display size kept; view state); the `phase` element kind and the PHASE / DELAY toggle removed; delay axis 0 ms at the bottom, real negative delay (a cut bell, −0.95 ms) drawn below the floor, not clamped.
- Verified (worker): -Affected 24/24; views 591/591 (the report's allpass vs analytic τ at 768 points ≤ 0.1 % + 1e-4 ms; 8 open/closed combinations via real clicks). Goldens re-recorded: eq-default, eq-eight-bands, eq-lr-mode, eq-band-response, eq-phase-delay; new eq-all-views.
- Perf (check, 32-band drag): 0.82 ms delay closed, 1.28–1.36 ms all open.
- Notes/blockers: wrapped phase of a fast-turning allpass is a thicket of segments and wrap markers (several turns per point).

## 2026-11-08 · TX.bool-real · orchestrator (merge of task/TX.bool-real)
- Did: merged eb580ed1. graph::infer inserts gate_to_real wherever a bool reaches a real input (atom inputs, subcircuit `in x : real`, root out, kind-polymorphic atoms meeting a real), before the voice sum; validate / edit::connect accept bool → real; bool outputs are cable sources (converted at the source). real → bool stays an error.
- Verified (worker): -Affected -Slow 87/88 → test_pqc's type-error example (voice.gate into a real) is valid now, changed to real → bool, pq_tests_render default + slow 2/2; graph / document / engine pass; new playback bool real* 2/2 on the report's project (interpreter == JIT).
- Perf: one AND per lane per converted port; CSE merges duplicates.
- Notes/blockers: TX.wire-isolation filed (other wire-kind errors still fail a whole unit); Q?-TX.bool-real-a.

## 2026-11-08 · TX.detent · orchestrator (merge of task/TX.detent)
- Did: merged 2192fdfc. CurvePoint::detent (saved as "detents"); knob / slider / number / arc / dial drags hold at a detent (2·zone crossing, ±zone from a start on one; zone ±6 px at 200 px travel, 4.5–9 px), no jump; Shift ignores; wheel / arrow steps land on it; curve editor shows and toggles detents. PA Amplitude 0…1 | 1…4 straight halves, 1.0 at t = 0.5 (detent, default). Detents on PA Pitch 0, VCA ×1 / ×−1, Utility Gain / Balance / Mid-Side / Width, Saturator Drive / Output, LFO Offset, EQ Band Gain, EQ Output.
- Verified (worker): dsp / document / views default tiers; -Affected 37/38 → pq_tests_library test_progressive (filesystem timing) passed alone; PA fixture renders bit-identical to the meta module; playback / tuning WAV goldens byte-identical. 28 view goldens re-recorded (PA Amplitude at centre).
- Perf: UI-thread only; one extra curve segment.
- Notes/blockers: TX.reset-default filed (Delete / double-click resets value blocks to position 0, not the module default).

## 2026-11-08 · TX.reset-default · orchestrator (merge of task/TX.reset-default)
- Did: merged 03fab80b. Device::defaultPosition on value blocks (saved "default"; module builders set it; rack macros their made-at position; user blocks 0); every reset path (Delete, double-click, macro reset) goes to it. Arc / Dial now handle Delete / Up / Down (Delete on a focused PA arc deleted the module). Detents: VCA ×0 / ±1 / ±2 (curve split, law unchanged to 1e-12), EQ band Q 0.7071.
- Verified (worker): views 598/598, document 196/196, dsp 180/180; -Affected 42/45 → stale VCA point counts in test_curve and two playback fixtures, fixed, re-run passed. Goldens: module-vca-panel, project-meta-faces, project-patch-overview, project-patch-band, project-circuit-device (VCA ticks). Playback WAVs unchanged.
- Perf: n/a. Notes: Toggle / Menu / XyPad / BandType still ignore Delete.

## 2026-11-08 · TX.group-oversample · orchestrator (merge of task/TX.group-oversample)
- Did: merged 501d6732 (rebased by the orchestrator onto TX.reset-default, clean). Group setting `oversample` (1×/2×/4×/8×); the lowering runs an oversampled group's contents in one oversample region (nested: the outermost, largest factor; per voice in instruments); analytic response through regions (up·inner(ω/n)·down). Oversample menu in every group's inspector (96 px column) and on the EQ / EQ Band panels, each factor's latency shown (3.62 / 4.74 / 5.17 samples).
- Verified (orchestrator, after the rebase): -Affected -Slow 88/88; -Preset ship -Target pq_bench_dsp -NoTests links.
- Perf: 4-band stereo EQ 2.4 / 9.3 / 16.3 / 26.3 µs per block at 1× / 2× / 4× / 8× (ship, indicative). 15 kHz bell error vs analog 0.35 / 0.009 / 0.0005 dB; tanh aliasing −19.5 / −42 / −83 / −131 dB.
- Notes: latency not compensated until PDC (M6); feedback through an oversampled group's edge is refused. Q?-TX.group-oversample-a/-b numbered at merge.

## 2026-11-08 · TX.mod-bands · orchestrator (merge of task/TX.mod-bands)
- Did: merged a5775900. graph::LinearResponse "set ports" (caller-supplied values; their drivers not evaluated, the signal pass cut there) — a wire-driven control no longer makes the response Unavailable, any modulator alike. CurveSite records each driven control's final-value node (post-lowering id); GroupResponse holds it at the control's curve at the live ring position (setLive) or the knob. The display draws band / total curves and nodes at the modulated value, with a faint ring at the knob position while displaced.
- Verified (worker): engine linear response 10/10 (set port == impulse at a fixed cutoff, 0.01 dB / 0.1°; evaluate allocation-free), views eq displays 17/17 (LFO cable keeps the curve; ring 2.5 kHz / 800 Hz moves node and peak with no relowering), document group_response 2/2; -Affected 37/37. Goldens: none.
- Perf: values only per change (no lowering / prepare / allocation), re-evaluated only when a live value changes, at the UI ring poll (30 Hz).
- Notes: live position = the ring's current (mid of the span swept since the last poll); per-voice: the ring's newest voice; no ring (stopped) → knob position.

## 2026-11-08 · TX.cable-look · task/TX.cable-look
- Did: wires between blocks of an opened group (terminals included) are hanging cables; only routes, sends, mixer outputs stay traces; dead GU.5b inner-route code removed; closed-group miniature wires curve. `addedControl`: a cable into a closed panelled group's main input whose In feeds one `add` with one value block lands on that value's control (Port::anchorKey) — the gold notes cable ends on the PA's Frequency arc. Visual only.
- Verified: pq_tests_views default tier passed; new anchoring test (PA anchored, unwired PA orbits, VCA not anchored); 32 goldens re-recorded (project patch, device views, panels).
- Perf / CPU-efficiency check: anchoring at model build only (two passes over a closed panelled group's wires); miniature uses the cached catenary table, 8 points per wire; nothing new per frame.
- Notes/blockers: blocks inside opened groups don't orbit their ports (tried, broke a real-dispatch drag test; reverted, in REVIEW.md).

## 2026-11-08 · TX.drag-grow · task/TX.drag-grow
- Did: a dragged block / selection / opened capsule / mixer grows (or shrinks back) the band and opened capsules it will drop into every frame by the layout's own rule; bands below slide down, capsule terminals stay at its edges; drop = one edit landing on what was shown, Esc restores. Target decided from the pre-drag layout (no jitter). CircuitCanvas::setNodeShift (nodes drawn / hit-tested / cabled at an offset). Fixed: cables on an opened capsule's frame edge now follow the capsule.
- Verified: new real-dispatch test "groups view (TX.drag-grow)"; pq_tests_views default tier 602/602 before and after rebase; goldens unchanged.
- Perf / CPU-efficiency check: per move linear in nodes/bands, no re-layout, no edit, no allocation after the drag's sets are built; one extra paint pass per distinct shift.
- Notes/blockers: push-apart not previewed (needs a per-frame re-layout); leaving band closes up only at drop; routes between shifted/unshifted bands drawn as a Z during the drag.

## 2026-11-08 · TL.1 · task/TL.1-lanes
- Did: Notes / Audio node gone; every band ends in its track's lane (one x range; group / return / master lanes empty strips for TL.2); hidden NodeKind::Lane keeps edges / lowering unchanged; one time window (ViewContext::timeWindow) shared with the arrangement, one Ruler over the lanes; arrangement lane hotkeys; empty-lane click sets insert marker + (stopped) start marker and playhead in both views; lane resize, band fold; cables leave the lane top at Wire::tap (SetWireTap, one undo step, double-click = default, drag up from the edge or a notes / Hz / gate / vel chip = new cable).
- Verified: pq_tests_views default tier 606/606 (real-dispatch tests: Space plays from clicked tick in both views, ruler, resize / fold, tap slide + undo, chip cable); -Affected 23/24 → every-command test needed a SetWireTap case, fixed, pq_tests_document passes; 36 goldens re-recorded (project-*, panel-window-*, panel-embed, instrument-rack).
- Perf / CPU-efficiency check: time zoom / scroll repaints lanes + ruler only (64 tracks: zero re-layouts); a tap slide skips re-lowering; lane paint culls to visible ticks.
- Notes/blockers: Q238. Ruler overlays the canvas top 40 px; taps reset to default on wire-rebuilding ops other than splice / insert, and on sticky cables after Ctrl-release.

## 2026-11-09 · TL.2 · task/TL.2-automation
- Did: band sub-lanes = the arrangement's per-track shown lanes / heights (returns + master too); LaneEditSurface fills laneRows / laneRect so ArrangementAutomation edits them (one selection / undo); Band::subH, fold keeps sub-lanes, header toggle, band menu Show / Hide / Add, sub-lane resize, Hide This Lane; a press on a face / panel control, value block or mixer control calls showAutomation; subLaneRect / controlDevice for TL.3. Fixed: envelope menu read the wrong surface's lane index; playhead damage on Σ lanes; sub-lane double-click opened a clip.
- Verified: pq_tests_views default tier passed after rebase; pqdaw compiles; 2 new real-dispatch tests; 3 no-rebuild drag tests now allow one re-layout per newly opened sub-lane (by design); 29 goldens re-recorded.
- Perf / CPU-efficiency check: zero cost with no sub-lanes shown; rects cached at model build; re-layout only when the shown set / heights change (lowering kept).
- Notes/blockers: Q239 (pressed controls stack sub-lanes). Not done: auto-scroll to an off-screen sub-lane, sub-lanes empty while a band is dragged, floating panel windows.

## 2026-11-09 · TL.3 · task/TL.3-auto-wire
- Did: thin red automation wire (automation.line colour, glow, ring at the control, dot + tick at the lane) from the last pressed band control (face / panel control, value block, mixer volume / pan / send) to the nearest point of its sub-lane, clamped to the visible x range; cable-look catenary at 0.75 sag; follows drags / drag-grow / fold / sub-lane resize; gone on Esc, press elsewhere, hidden sub-lane, deleted device; closed group anchors at the panel element or the block edge. Draw-only (ProjectPatchView::automationWire()).
- Verified: pq_tests_views default tier passed after rebase; new real-dispatch test "lane edit (TL.3)"; new golden project-patch-auto-wire; panel-window-{inspector,links,open,edit} re-recorded (only the wire strip changed).
- Perf / CPU-efficiency check: zero cost without a wire; on paint only, stack buffers, damages only its bounding box when moving alone.
- Notes/blockers: the nearest point is usually straight below, so the wire is mostly a vertical drop; floating panel windows draw none (TL.2 opens no sub-lanes there).

## 2026-11-09 · TL.6 · task/TL.6-auto-knobs
- Did: one AutomationFollow (arrangement/automation_follow.h) used by the Device view and the band view: meta face controls (MetaTarget::showPosition), closed / embedded group panels and value blocks (PanelTarget::showPosition), the mixer face (mixerShow) show the envelope at the playhead, move while playing, jump on locate; grab = override until Re-enable. Fixed: Device-view native-engine faces inside panels didn't follow automation.
- Verified: pq_tests_views default tier 612/612 after rebase; new test_band_automation.cpp; golden project-patch-lane-edit re-recorded (mixer shows the automated volume).
- Perf / CPU-efficiency check: nothing runs without a shown automated control; 30 Hz poll only while playing; no per-tick allocation; repaints only moved controls (CircuitCanvas::damageFace).
- Notes/blockers: band controls get no automation LED; pq_tests_views_perf run by accident: 3 timing failures (shared machine) + `legs == 32` (test_project_patch.cpp:2343) unverified on main.

## 2026-11-09 · TL.4 · task/TL.4-look
- Did: lanes paint the arrangement background (alternate bars, grid), inset frame, track-colour bar, arrangement clip painters; compact Ruler strip per lane (locate, playhead, markers); any plain click on a lane's time (empty, clip body, title) sets the start marker in both views, moves the playhead when stopped, Play restarts from it while playing; automation wire lands on the envelope line beside the control, thin half-transparent sagging curve; mixer faces 2× and S-drag resizable (saved, not undoable).
- Verified: -Affected after rebase 19/20 → project-patch-lane-edit re-recorded for TL.6, passes; 3 new real-dispatch tests; confirmed in a launched app; 39 goldens re-recorded (project-*, panel-window-*, panel-embed, arrangement-selection, device-path-broken).
- Perf / CPU-efficiency check: one compact ruler for all lanes, no per-frame allocation, LOD below 6 px / 5 px; wire samples 2×48 envelope points only while shown. 3 perf-label failures (64-track paint, S-drag step, mod-all wire count) also fail on origin/main.
- Notes/blockers: Q240. Root cause of play-from: a click into a clip set only the insert marker.

## 2026-11-09 · TL.5 · task/TL.5-follow-lane
- Did: per track pinned lanes + one follow lane below; a band control press, Show Automation (arrangement / Device view / tunings panel) re-points the follow lane or reveals a pinned one; pin button on arrangement lane headers and band sub-lane labels, Pin / Unpin Lane menu items; "+" adds a pinned lane; the TL.3 wire lands on whichever lane shows the param.
- Verified: pq_tests_views default tier 617/617; new real-dispatch tests "lane edit (TL.5)", "automation (TL.5)"; 9 goldens re-recorded (pin glyph, label shift, one sub-lane instead of a stack).
- Perf / CPU-efficiency check: re-pointing the follow lane at an equal height updates sub-lanes in place (generation counter), no band re-layout.
- Notes/blockers: Q241. Follow-lane param and pins of unautomated params are session-only (no ViewState entry).

## 2026-11-09 · PF.2 · task/PF.2-patch-perf
- Did: project wires indexed per track per build; ModelCache (document-derived tables reused while snapshot + lowering are the same; opened groups' autoLayout keyed on input bytes); group-chain hash walks; no unseen miniatures; Device-view cable layer skips wires off the shown chain; far-zoom panel stand-in under 6 px; the view takes ProjectLink's lowering when it is of the same snapshot.
- Verified: pq_tests_views default tier passed after rebase, no golden moved; new layout-cache invalidation test; perf (release, 64 tracks): paint 8.8–20.6 → 0.59–0.91 ms, S-drag step 795–858 → 6.3–9.7 ms quiet (11–14 under load), structural edit model + scene ~540 → 24–40 ms.
- Perf / CPU-efficiency check: per group one track's wires instead of all 8.5k; far-zoom vertices 774k → 70k per frame.
- Notes/blockers: lowering 111–200 ms per structural edit remains (PF.6); after an edit while shown the view lowers itself (the link's lowering arrives ≥ 50 ms later); the shared-lowering path has no test (no link in views tests).

## 2026-11-09 · PF.1 · task/PF.1-jit-share
- Did: cache key hashes "has a resource", not the slot (codegen never bakes it; new bit-identity test vs separate compiles + interpreter, fails on the old key); `-disable-lsr` in the machine pipeline, IR-level LSR kept on functions ≤ 2000 instructions; executable hash on a background thread at startup; TX.mod-all counts modulation wires only.
- Verified: -Affected -Slow after rebase 48/48; ship pq_bench_dsp links; rows in bench/RESULTS.md.
- Perf / CPU-efficiency check (ship, interleaved): 64x8 notes jit 130 → 6 compiles, 7.1–10.1 → 0.62–0.85 s, 193.8 → 134.7 µs/block; fixture per-unit JIT 8175 → 6471 ms; run-time rows within noise (render ≤ 1.3 %).
- Notes/blockers: a slot equal to a literal in the unit doesn't share (constants are deduped by value); the 2000-instruction cutoff tuned on one fixture + two microbenchmarks.

## 2026-11-09 · PF.6 · task/PF.6-lowering
- Did: lowering linear in the project: per-track wire index, wires grouped per input once, per-lowering lookups (wires by target, devices by id, literal value blocks, call signatures), wire-planner cycle check without per-wire clears; new pq_bench_lower (wide64, heavy30); ARCHITECTURE Lowering cost note.
- Verified: -Affected -Slow after rebase 88/88; new test_lower_incremental.cpp (7 edits, incremental == fresh field by field); whole-lowering digest identical before / after on both fixtures (release + ship).
- Perf / CPU-efficiency check (ship): wide64 105–121 → 22–25 ms; heavy30 498–509 → 47–65 ms; was lowerSites (all wires per track per input), makeCallees, paramDriven, findDevice.
- Notes/blockers: per-track reuse not done; remaining time mostly project-wide (engine / group expansion 9 ms, wire planning 8, main paths 7, routing 7); next: shared indexes (~5 ms), cache native-engine expansions by meta::expansionKey.

## 2026-11-10 · PF.3 · task/PF.3-ramp-copy
- Did: one stage entry with a `ramp` argument; ramp-free units emitted once for both modes; each span of ramp-reading units branches once per block to its own noinline `<stage>_ramp<k>` copy (pointer args captures(none)); cache kFormat 3; new interleaved-ramp tests + IR bound.
- Verified: -Affected -Slow after rebase 46/46; slow ramp sweep 680 runs interpreter == JIT; ship pq_bench_dsp links.
- Perf / CPU-efficiency check (ship): fixture IR 384k → 260k; per-unit JIT 5579–6647 → 3899–3907 ms; largest unit 231 → 163 ms; at-rest run time equal, moving +0.5 %.
- Notes/blockers: voice stage IR unchanged (every unit reads a ramp); all-param chains ~5 % slower to compile; one-function variant compiled 15 % slower (rejected).

## 2026-11-10 · PF.4 · task/PF.4-parallel-jit
- Did: BackgroundCompiler worker pool (min(6, cores − 2), below-normal priority, PQ_JIT_THREADS / LinkConfig::jitThreads), edited unit first, refinements last, one worker per schedule; per-compile LLVM context + leased target machine, SimpleCompiler + addObjectFile; same-key compiles wait and share; modules ≥ 3000 IR instructions split into 2 objects (parallel only when idle); disk cache format 4. FastISel quick image rejected (stack probes > 4 KiB frames on stock devices).
- Verified: -Affected -Slow 88/88; ship pq_bench_dsp links; swap / cache / pool / live-JIT / AOT tests 10× each, 0 failures; cache bytes identical at 1 and 6 threads; split images == interpreter bit for bit.
- Perf / CPU-efficiency check (ship): 30-track cold open first images 3.49 → 0.80–0.91 s, final 7.31 → 1.65–1.84 s (6 threads); largest unit 163 → 113 ms; 64x8 final 484–489 → 350–375 ms; steady state 382.5 → 383 µs/block (equal); peak RAM 234 / 258 / 322 / 364 MB at 1 / 2 / 4 / 6 threads.
- Notes/blockers: quick first image barely beats the final on big units (111.5 vs 112.7 ms); an LLVM OOM in one compile could leave locks held for concurrent compiles.

## 2026-11-10 · T6.1 · task/T6.1-doc-model (merged 9ee53c10 by the orchestrator)
- Did: Mixer monitor (Auto/In/Off; arm + monitor are Params, arm on audio tracks), audio/MIDI inputs, track delay; audio input lowers `gain(clips, 1−mon) + gain(track.in, mon)`; `Track::noteInput` + `input_map.h` (128 entries, Keys/Hz, one `resolvePitch` shared with resolveTrackPitches); take lanes (audition, promote, compRegion), punch, record settings; warp modes + markers + WarpParams; schema 22.
- Verified: pq_tests_document 206/206; -Affected default tier all pass except pq_tests_views, which hit the known NVIDIA SIGSEGV flake 3× in different GPU tests, then 618/618.
- Perf / CPU-efficiency check: arm/monitor = param writes, no compile (patch object shared, tested); input map build 7.2 µs (target 50); unheard takes never lowered; no input → no track.in, no gate.
- Notes/blockers: app plays new warp modes as Re-Pitch until T6.3/T6.5; metronome / track delay stored but Meta until T6.6. Q?-T6.1-a.

## 2026-11-10 · T6.2 · task/T6.2-pdc (merged c601362b by the orchestrator)
- Did: `graph::pathLatency` + atom `latency` rule and `latency(x, frames)` atom; `engine::planCompensation`; reader-owned preallocated delay lines (10 ms crossfade on change, silent inputs skip the copy); merged units aligned at compose (`wireDelay`); compensated instruments' notes delayed by A_u, automation via `AutomationLaneDesc::shiftFrames`; ProjectLink plans after each compile, `outputLatency()`.
- Verified: -Affected -Slow after rebase all pass (after a `.gitattributes` -text rule for T6.1's new golden, which failed under autocrlf); test_pdc 15 cases (groups, sends/returns, 3-deep nests, 64 tracks total 480, 4× oversampled ≤ 0.5 sample, no-latency bit-identical, live change ≤ −60 dBFS, sidechain merge, modulation undelayed, notes, automation, RtScope, per-unit == whole-project); ship pq_bench_dsp links. One views NVIDIA flake pre-rebase, passed on re-run.
- Perf / CPU-efficiency check: pq_bench_pdc (release) 86 ns per compensated stereo input per block (target 40; 166 before caching the slot → line map; rest is ring memory traffic); plan 12 µs @128 tracks; no latency = one pointer compare per unit per block.
- Notes/blockers: T6.2b filed (clip players in compensated units, tap automation lanes, per-input cost, stale comments). Q?-T6.2-a.

## 2026-11-10 · T6.3 · task/T6.3-warp-1 (merged 9fc2db89 by the orchestrator)
- Did: engine ClipWarp += Beats/Tones/Texture; WarpSegments + settings + transients on AudioClipDesc; Re-Pitch through markers (grid cells split at segments; M5 single segment bit-identical); Beats (grid/transient slices, 2 ms crossfades, Off/Fwd/BackFwd, Envelope); Tones = WSOLA-aligned Hann grains; Texture = Hann OLA + seeded Flux; dsp/warp.* AVX2; media/transients.* (band log-flux, cached in <project>/cache/analysis). Complex / Complex Pro play as Tones until T6.5.
- Verified: new tests timeline warp 8/8, transients 2/2, dsp warp 4/4, playback link 2/2 (Beats ±0.15 ms, centroid 1.6 %, Tones 0.004 ¢, 31-EDO +3 exact, blocks 64 vs 1000 and streamed vs resident bit-identical, RtScope clean); -Affected -Slow 87/88: pq_tests_document "save+load ≤ 20 ms" timing check at 22 ms under 100 % CPU (document untouched; 17.8/28.1/18.0 on reruns — load-sensitive). Ship pq_bench_dsp links.
- Perf / CPU-efficiency check (ship, per stereo voice): beats 1.48 ns/frame (0.007 %), +3 st 14.3 ns, tones 30.1 ns (0.14 %, target 0.6), texture 1.41 ns, repitch markers 13.7 ns.
- Notes/blockers: Q?-T6.3-a. The wavetable save+load timing assertion is load-sensitive (should carry the `perf` label).

## 2026-11-10 · T6.4 · task/T6.4-midi-input (merged c65fe531 by the orchestrator)
- Did: RtMidi 6.0.0 (WinMM, pinned, pq_rtmidi, pq_app only); engine LiveInput (16 port rings + keyboard ring, QPC stamps scheduled one period late with a clock filter, held-note routing so note-offs reach their slot, bend 2 ms glide, sustain, All Notes Off, overflow/stale handling, record tap); ProjectLink route image from arm/monitor/MIDI From/note mode, no compile, pitches via buildInputPitchMap (same tuning table as lowerTrack); MidiInputService (enumerate, hot-plug, ANO on close, prefs); Preferences MIDI tab; mixer-view MIDI From/channel/Keys-Hz/Monitor; `M` computer keyboard.
- Verified: live == drawn max diff 0 in Keys and Hz across 12-EDO, 31-EDO, 5L2s + generator automation, .kbm; exact frames at blocks 1…1024; 10k ev/s × 60 s 0 drops / 0 stuck, RtScope clean; -Affected -Slow 88/88 after rebase (first try lost to an sccache server crash).
- Perf / CPU-efficiency check: idle 4.6 ns per block; per event one lookup + one push; callback lock- and allocation-free; ±1 ms jitter → worst 13 frames.
- Notes/blockers: Q?-T6.4-a; T6.4b filed (bend lost on Keys notes under a modulated tuning). Goldens: mixer-view, audio-prefs(-wasapi), library-prefs re-recorded; midi-prefs new.

## 2026-11-10 · T6.5 · task/T6.5-complex (merged 887160e2 by the orchestrator)
- Did: signalsmith-stretch 1.4.0 + signalsmith-linear 0.6.4 (MIT, pinned, unpatched); engine::StretchPool (32 stereo, built off-thread, lock-free claim, Tones fallback counted and click-free, per-claim reseed → deterministic); ClipWarp Complex / ComplexPro; stretcher does time only, pitch by resampling its output (signalsmith's transpose was 2–8 ¢ off); latency primed away → sample-aligned, 0 to PDC; Pro = formant preservation + Envelope.
- Verified: RtScope spike 10k blocks clean; pq_tests_timeline 58/58 (220 Hz within 1e-5 ¢ at 0.5×/2× and +3 steps of 31-EDO, source tempo aligned to the frame, Pro formant shift 0.75 % vs 49.75 %, blocks and streamed/resident identical); -Affected -Slow 88/88 after rebase; ship pq_bench_dsp links.
- Perf / CPU-efficiency check (ship, stereo voice): complex 334 ns/frame = 1.60 % (budget 3), +3 st 2.28 %, complexPro 3.26 % (budget 5); pool ≈ 24 MiB per clip player, 16 ms build.
- Notes/blockers: T6.5b filed (priming burst at Play inside a Complex clip; mono through stereo). Drum timing ±2.5 ms in Complex. Q?-T6.5-a.

## 2026-11-10 · T6.6 · task/T6.6-metronome (merged 1112712b by the orchestrator)
- Did: engine::Metronome (15 ms click 2 kHz / 1 kHz, beats at ticksToSamples, delayed by the master PDC total, main or cue pair, never offline); count-in (Record + countInBars, exact start frame, Stop cancels); Track Delay → planCompensation (bus latency no longer clamped at 0; re-plan without compile); PDC total in the CPU meter info, "Lat x ms" per strip; control-bar metronome switch + menu (count-in, level, cue); Record / F9.
- Verified: test_metronome 9 cases (96 000-sample count-in at blocks 64/37/1000, tempo ramp + signature changes, PDC 480 alignment, RtScope), Track Delay ±480 (engine + link, no compile), views by real dispatch; -Affected -Slow 86/88 — pq_tests_timeline (preview two-thread waitFor) and pq_tests_library (indexer wakeup count) failed under load, pass alone (untouched code).
- Perf / CPU-efficiency check: off = one test per image block; on 10.6 ns per 64-frame block (target 1 µs).
- Notes/blockers: Q?-T6.6-a. 86 goldens re-recorded (control bar y 26–46; mixer-view two rows per strip).

## 2026-11-10 · T6.8 · task/T6.8-warp-ui (merged feeca5c7 by the orchestrator)
- Did: media/tempo_detect.* (weighted IOI histogram 60–200 BPM, prior on loud-hit spacing, confidence, hash-cached); doc ops slideWarpMarker / warpFromHere / warpAsLoop / autoWarpClips; views::SampleAnalysis off-thread; audio Clip View 7-mode chooser + per-mode settings, :2 / *2, content-axis waveform with marker strip, beat grid, faint transients; marker gestures (add/drag/Ctrl-slide/Delete/Esc, one undo step each); Warp From Here / Warp as N-Bar Loop; auto-warp on drop (pref off by default; default mode Beats); Preferences → Warp; arrangement badge names the mode.
- Verified: pq_tests_media tempo 3/3 (20/20 loops ±1 BPM, 7 jitter seeds); document recording 9/9; views default tier by real dispatch; -Affected 45/45; after the orchestrator-requested re-rebase onto T6.6 the views default tier passes (7 goldens re-recorded with both changes).
- Perf / CPU-efficiency check: nothing on the audio thread; detection once per file, off-thread, cached; ≤ 64 waveform pieces per view, ≤ 1 transient line per pixel column.
- Notes/blockers: Q?-T6.8-a. Goldens: new audio-clip-warp-beats / -markers; re-recorded clip-view-audio, audio-prefs(-wasapi), library-prefs, midi-prefs.


## 2026-11-10 · TU.1 + T6.4b merged · main
- Did: merged task/TU.1-tuning-menu (f6ec6e65): tuning menu edits apply live (MOS wheel, clip-box Generator, panel Steps), one undo step per drag, main-tuning generator drags are SetTuningParam (no compile); 80-item temperament dropdown replaced by a search field; MOS colours from the theme; LiveInput retunes held live keys on a new route image. T6.4b: no code change, pinned by a test.
- Verified: worker -Affected -Slow 88/88 after rebase onto 3db933f6; goldens moved: mos-chart-default/-zoomed, mos-circle-5L2s/-modmos, mos-wheel-popover(-1.5x).
- Perf / CPU-efficiency check: drag step = one param write; tuning kernel untouched.
- Notes/blockers: Q?-TU.1-a; private clip tunings don't follow automation or held live MIDI keys (as before).

## 2026-11-10 · T6.7 merged · main
- Did: merged task/T6.7-recording (de14edc6): engine::Recorder (lock-free rings, punch shifted by record latency, monitor gates In/Off/Auto), ASIO inputs into `track.in` via setUnitInputs (no compile), RecordingService writes WAVs on the disk thread, ops::commitRecording (take lane per pass, last pass in main, MIDI overdub merge, one undo step), Record/F9 records, live overlay, input chooser + Monitor + audio arm button.
- Verified: worker -Affected -Slow 87/88 then views goldens (arm column) re-recorded and passing; after re-rebase onto 15963b21: views default 100 %, playback T6.7 + TU.1 live tests 10/10, timeline T6.7 5/5.
- Perf / CPU-efficiency check: unarmed = one flag test per block; one memcpy per used channel; peaks off the audio thread.
- Notes/blockers: Q?-T6.7-a; leftovers filed as T6.7b. REVIEW asks for a real ASIO multi-input hand check.

## 2026-11-10 · T6.5b merged · main
- Did: merged task/T6.5b-complex-priming (6da4365e): Complex/Pro priming at most 4× play rate on a song-time schedule; ClipPlayer::cue primes silent voices at the playhead (stopped / count-in) and the loop start before a wrap, seek() takes them over; an unprimed start is ~70 ms late with a fade-in, then sample-identical; offline renders prime at once; StretchPool mono stretchers.
- Verified: worker -Affected -Slow 53/55 after rebase, the 2 failures known flakes (timeline preview waitFor 87/87 alone; window selftest gallery segfault under load, passed alone); ship pq_bench_dsp links.
- Perf / CPU-efficiency check: worst block after mid-clip start 3306 → 392 µs (1 voice), 13283 → 1607 µs (4 voices); mono 316 → 179 ns/frame; stereo steady unchanged.
- Notes/blockers: Q?-T6.5b-a. ~13.7 MiB extra per clip player for 32 mono stretchers (estimate) — check in the M6 review.

## 2026-11-10 · T6.10 merged · main
- Did: merged task/T6.10-comping (5b04d78e): take lanes in the arrangement (headers with audition / delete, fold), arrangement::Comper (swipe → ops::compRegion snapped to grid, click-swap, Up/Down, Esc), audition via setAuditionTake (no compile), veil on take parts the main lane doesn't use; song end counts take clips.
- Verified: worker pq_tests_views 642/642, test_comping_link (playback) passes, -Affected 45/45 after rebase; new goldens arrangement-takes, arrangement-comp, none moved.
- Perf / CPU-efficiency check: 8 tracks × 8 takes × 50 clips paint 0.24 ms median (budget 2 ms); folded/take-less tracks add nothing.
- Notes/blockers: Q?-T6.10-a; take rename from header, lane resize and a promote gesture not built (ops exist).

## 2026-11-10 · T6.9 merged · main
- Did: merged task/T6.9-capture-midi (247c8d46): engine::CaptureRing per armed/monitored MIDI track, app::planCapture (playing → exact placement; stopped → tempo estimate, looped clip at the marker's bar), ops::captureClip, transport.captureMidi Ctrl+Shift+C + control-bar button.
- Verified: worker playback T6.9 4/4, timeline live input 15/15 (RtScope), views default 100 %; -Affected after rebase: 90 goldens moved only in the control-bar band (new button) and re-recorded; views SIGSEGV and playback_s3 wavetable timing = known flakes, passed alone.
- Perf / CPU-efficiency check: note-on = slot write + atomic store; idle block = one relaxed store; reading/planning on the UI thread only.
- Notes/blockers: Q?-T6.9-a; ~2–3 MB per listening track (estimate); no pickup detection.

## 2026-11-10 · T6.2b merged · main
- Did: merged task/T6.2b-pdc-leftovers (c2e1174c): per-member internal shift T(t) for merged units (clip delay lines, note delay, automation shift), tap lanes rendered per sample at t − shift, mirrored-tail delay rings read in place, stale "not compensated" comments fixed.
- Verified: worker pq_tests_project 35/35, -Affected -Slow 76/76, ship pq_bench_dsp links; after re-rebase onto a3af785f: pdc 21/21, playback T6.9+T6.7 10/10, timeline live input 15/15.
- Perf / CPU-efficiency check: per compensated stereo input @64: 78–80 → 53–61 ns (target 40 not reached; breakdown in Q?-T6.2b-a / worker rdtsc); plan 12 µs @128 tracks; no-latency projects unchanged.
- Notes/blockers: shared params take the smallest shift; loop-wrap lane start — for the M6 review.

## 2026-11-10 · T6.7b merged · main
- Did: merged task/T6.7b-recording-leftovers (8258f6cf): Record prefs tab with a record latency offset, untitled-discard cleanup of the run's Unsaved folder (guarded; kept when a save couldn't move a recording), WASAPI shared-mode capture input, inputs opened by use; Input Channels pref removed.
- Verified: worker -Affected -Slow 56/56 on cbc70a70 (parallel 4); T6.7b recording cases 5/5 incl. failed-move-then-discard; loopback click 1440 vs measured 1424 frames.
- Perf / CPU-efficiency check: no inputs = nothing extra; one conversion pass per capture packet + one copy per channel per period; RtScope clean.
- Notes/blockers: Q?-T6.7b-a; goldens audio-prefs(-wasapi), library-prefs, midi-prefs re-recorded; real mic / ASIO reopen hand checks in REVIEW.

## 2026-11-10 · T6.11 merged · main
- Did: merged review/M6 (b3c21922): docs/reviews/M6.md + probes in docs/reviews/M6/, 21 ship bench rows (T6.11), hand checks appended to REVIEW. Filed T6.x-fix-rec, -warp, -pdc, -ui.
- Verified: reviewer release -Full 88/88, 0 warnings; ship pq_bench_dsp/pdc/timeline/playback linked.
- Perf / CPU-efficiency check: 64 audio tracks at rate 1 regressed 13.7 → ~17 µs per block (M13); stretch pool 56.9 MiB @48 kHz allocated eagerly (L15).
- Notes/blockers: M6 stays open until C1, C2, H1–H3 are fixed.

## 2026-11-10 · T6.x-fix-rec merged · main
- Did: merged task/T6.x-fix-rec (23c16eaa): C1, H1, M4, M5, M6, M8, M9, L1, L2, L11, L16 from docs/reviews/M6.md fixed (see TASKS note).
- Verified: worker -Affected -Slow 64/67 after rebase; timeline preview waitFor + playback_s3 wavetable = known flakes, playback_s2 loop-test rig overflow fixed (pump every 64 blocks); the 3 re-run alone 3/3; media 48/48; recording timing tests ×5.
- Perf / CPU-efficiency check: recorder adds a 256-entry run ring per block, RtScope clean; writer thread no longer blocks stream refills.
- Notes/blockers: files are `offset` frames shorter (pre-session frames not written); L13 stale-folder recovery still open (T6.x-fix-ui).

## 2026-11-10 · T6.x-fix-warp merged · main
- Did: merged task/T6.x-fix-warp (9ca6deaf): Complex re-prime from the next read's need (C2), lazy image-sized stretch pool with Tones→Complex retry + fallback count in CPU info (H2, L15), staggered priming (M10), priming at 4×max(1, ratio) (L14), tempo length prior + near-tie confidence (H3 partly), signature-aware N-bar loop (M11), Tones documented monophonic + chord test (M12), M13 bisected (filed PF.4b).
- Verified: worker -Affected -Slow 76/76 after rebase (one sccache crash retried); ship pq_bench_dsp + pq_bench_playback link. Tempo test's "≥ 18/20 confident" became "confident ≥ 100 BPM 11/11, never confidently wrong" (sub-100 loops are ambiguous half-time; tempo still 20/20 right) — Q?-T6.x-fix-warp-a.
- Perf / CPU-efficiency check: worst Complex block 16 tracks 9.99 → 3.81 Mcycles; clip player memory without Complex 56.9 → 5.1 MiB; audio/64 rate 1 still 16.8 µs.
- Notes/blockers: PF.4b filed for the playback regression.

## 2026-11-10 · T6.x-fix-pdc merged · main
- Did: merged task/T6.x-fix-pdc (c73ccd71): M1 loop-wrap shifted lanes from the previous pass, M2 grown lines copied in memcpy spans over blocks (no fresh-ring clear), M3 mid-fade lines carried forward, L6/L7 plan sent with/after each topology and stale lines dropped (PF.5-safe), L9 sorted click queue, L10 note-offs never dropped, L12 atomic recorder stats.
- Verified: worker project + timeline default pass; -Affected -Slow 53/54 (ASIO fake-driver flake, 155/155 alone); ship pq_bench_dsp links; each new test failed on the old logic.
- Perf / CPU-efficiency check: grow switch block 1063–1124 → 39–53 µs, next 400 blocks ≤ 35 µs; no-latency path untouched; pq_bench_pdc ~52–66 ns both (loaded machine).
- Notes/blockers: none.

## 2026-11-10 · T6.x-fix-ui merged · main
- Did: merged task/T6.x-fix-ui (334288c7): MOS popover within the workspace (OverlayOptions::within), one mode numbering + M letter, Clip View units/tint/bar numbers, take-lane contrast and dimmed auditioned-away clips, mixer row alignment / In: / Delay / Lat only when non-zero, Hz live notes not retuned by drags, per-table tuning param slots (no third pitch), crashed-run Unsaved recovery dialog, fixtures copied to the build dir, append_results porcelain fix; record latency frozen per session.
- Verified: worker -Affected -Slow 61/61 on 3b79d344; after re-rebase onto 3c440ba3: timeline 99/99, project pdc 35/35, playback recording link 17/17, L4 test.
- Perf / CPU-efficiency check: UI-thread-only additions; latency freeze two integer copies per session.
- Notes/blockers: M7 take split filed as T6.x-fix-ui-b.

## 2026-11-10 · T6.x-fix-ui-b merged · main
- Did: merged task/T6.x-fix-ui-b (73f8b9fd): CaptureFifo holds the capture→play delay measured after priming using QPC play times; drift past 0.5 ms engages a PI-controlled 64-tap sinc resampler; driftReserve added to the WASAPI capture target.
- Verified: worker test_capture_drift (±20/±100/±200 ppm × 10 min, jitter, same-clock bit-exact, RtScope), -Affected -Slow 88/88.
- Perf / CPU-efficiency check: ~4 µs per 480-frame stereo pull while resampling (~0.04 % core); same-clock path two memcpys.
- Notes/blockers: real two-device hand check in REVIEW.

## 2026-11-10 · PF.4b merged · M6 closed · main
- Did: merged task/PF.4b-playback-cost (ef7d3d43): RT heap (pq/core/rt_heap.h) for JIT/interp images, host tables and unit packets so placement no longer depends on compile threads; hot fields first in ImageHost / Unit / Voice; shared per-block ProcessContext in runUnit. M6 closed.
- Verified: worker -Affected -Slow 88/88 on 4b6b4197, no golden moved; ship pq_bench_dsp links.
- Perf / CPU-efficiency check: audio/64 rate 1 ship 16.3 → 14.0 µs (M5 13.6–13.8), release 16.6 → 14.4; 1 vs 6 JIT threads now equal.
- Notes/blockers: ~0.3 µs over M5 remains; H3 tempo partly fixed.

## 2026-11-10 · KB.1 · task/KB.1-computer-keys
- Did: ui_tree gives presses to the computer keyboard before the focused widget (text fields excepted); layout by the playing track's tuning: 12 degrees → Live's piano layout (white keys where the piano roll draws white), else mos-explorer 1Row (default) / Continuous; control-bar keyboard button (lit, "E+1 V100", menu: 1Row / Continuous, equave, velocity); Live's arm on select (unsaved, not undoable); engine key range 17 × ±5 → 36 × ±3.
- Verified: real-dispatch tests (17 note keys with each main view focused, F doesn't fit; 12-EDO, 22-EDO, 5L 2s in 31-EDO, 31-EDO; button + menu; arm on select; live routes); -Affected after rebase 36/36; goldens: 2 new control-bar-keyboard-{off,on}, 92 full-window re-recorded (control bar row shift + lit arm button).
- Perf / CPU-efficiency check: UI thread only; tuning rebuilt only when its definition changes; button repaints on change; engine key table 187 → 252 entries per listening track, built on cache miss.
- Notes/blockers: Q?-KB.1-a; M stays the toggle so its MOS bottom-row position plays nothing; keys matched by character, not physical position.


## 2026-11-10 · KB.2 merged · main
- Did: ff-merged task/KB.2-live-clip-tuning (5b38e03e): doc::liveClip (the clip view's clip on that track, else the clip under the playhead / start marker, else the track) decides the computer keyboard's layout and the live note tables (keyboard + hardware MIDI).
- Verified (worker): -Affected 45/45; pq_tests_views KB* 6/6; pq_tests_playback "live input*" 7/7.
- Notes: Q?-KB.2-a: recorded / captured notes into a new clip inherit the track's tuning (mismatch when played in a retuned clip); proposed fix: new clips copy the live clip's tuning.

## 2026-11-10 · TU.2 merged · main
- Did: ff-merged task/TU.2-mos-circle-look (12f3268f): explorer region palette back on the MOS circle (pre-TU.1 theme, L22 opacity step undone), colour easing at the explorer's 8/s rate (frames only while easing), notes light their degree (explorer envelope, Phase Accumulator Hz colour, shared voice_colour.h) from playback, the computer keyboard, roll auditions.
- Verified (worker): pq_tests_views default tier 100 % after rebase; goldens mos-circle-5L2s (= pre-TU.1), mos-circle-modmos, mos-wheel-popover(-1.5x) re-recorded, mos-circle-lit new.
- Notes: hardware MIDI-in doesn't light (no UI feed); chart colours still TU.1's (Q?-TU.2-a) → TU.2b.

## 2026-11-10 · KB.3 merged · main
- Did: ff-merged task/KB.3-meters-after-idle (31c74d9d, rebased by the orchestrator onto TU.2, clean): Engine::setWakeListener + MIDI-in → ProjectLink::signalActivity → view_change::kSignal re-arms meters, cable glow, faces, cost labels, modulation rings; live() holds 2 s after activity.
- Verified: worker -Affected -Slow 88/88 before the rebase; after it pq_tests_views default tier 1/1 (orchestrator).

## 2026-11-10 · TL.7 · task/TL.7-mixer-right
- Did: unplaced mixers at the lanes' right end, level with the main path (mixer pin joins the spine; blocks sit 24 lower); placed mixers stay. Lane default taps at the left end (audio 12 in; MIDI right of the port chips; several 12 apart: notes/Hz/gate/vel). Slack 1 + 0.06/(1+(D/260)²): short cables ≈ 15 % sag, long ones ≤ ~40 units. Rest table angle × 9 slack levels, no solve on paint paths; automation wire unchanged. ARCHITECTURE updated.
- Verified: new real-dispatch TL.7 tests (empty audio track tap / mixer / sag < 45, Z inserts a VCA, MIDI tap), tension tests (monotone, short within 8 %, table vs solve within 1 %); orchestrator after rebase onto fd1c7d93: pq_tests_views default tier passed; 46 goldens re-recorded (bands, cables).
- Perf / CPU-efficiency check: rest shape a 4-corner table blend (≈ 2× multiply-adds, no allocation, 0.4 MB table built once); layout additions O(bands + lane edges). S-drag step perf A/B under 2 other agents' builds: quiet rounds main 7.0 / 7.9 ms vs branch 7.4 / 7.6 ms; loaded rounds 7–77 ms on both, no clear regression.
- Notes/blockers: group / return / master Σ stay where their chains start (member routes run back left; REVIEW asks).

## 2026-11-10 · TX.group-resize · task/TL.7-mixer-right
- Did: S-drag on an opened group's frame stores its nudge scale; contents laid out at their sizes then scaled (members, terminals, spacing); nested compose; Shift steps, Esc, S+double-click / Reset Size. Fixed atom pin rows ignoring scale; a project opened while the patch view is shown now re-lays out on view_change::kProject.
- Verified: real-dispatch nested test (0.75×, nested 0.5×0.75, no history entry, Esc, reset) + save/load; pq_tests_views passed; no goldens moved.
- Perf / CPU-efficiency check: only groups with scale ≠ 1, after the cached inner layout; nothing per frame.
- Notes/blockers: Q?-TX.group-resize-a (closed and opened sizes are one value).

## 2026-11-10 · TU.3 merged · main
- Did: ff-merged task/TU.3-shared-tunings (e9fcb3a5): private tunings deleted; Project::defaultTuning + SetDefaultTuning; every tuning control edits the named tuning its target resolves to (no recompile for generator edits from a clip / track); one chip menu (Inherit, project tunings, New Tuning, wheel, library, hot-swap); clip files carry their tuning.
- Verified (worker): -Affected 45/45 after rebase onto ac7328de; 8 goldens re-recorded (chip colour dot, wheel title).
- Notes: Q?-TU.3-a (default tuning named "12-EDO") overruled → TU.2b.

## 2026-11-11 · AR.1 merged · main
- Did: ff-merged task/AR.1-automation-recording (691fa66e): one touch-recording mechanism in ViewContext::onStoreEvent for every automatable param while Arrangement Record + Automation Arm; held = override (heard live), thinned per param range, committed with the take in one "Record" undo step at Stop; Automation Arm button (on by default). 92 control-bar goldens re-recorded (diffs confined to the bar row).
- Verified (worker): -Affected after rebase onto TU.3: 44/45 then pq_tests_views 1/1 after the bar goldens.
- Notes: lanes written at Stop, not live. Removed fixture_copies/ (test output committed by KB.3 / TU.3) and git-ignored it.

## 2026-11-11 · TU.2b merged · main
- Did: ff-merged task/TU.2b-wheel-followups (db59bcda): MOS chart back to the explorer palette (shared mos_palette.h with the circle); hardware MIDI-in notes light the circle (per-port SPSC rings, UI-side drain mapped through the live routes); default tuning "Tuning A".
- Verified (worker): -Affected after rebase, 4 exes failed on old "12-EDO" fixtures/goldens, fixed, re-run 4/4; 62 goldens re-recorded (chips / chart).

## 2026-11-12 · DT.1 merged · main
- Did: merged task/DT.1-delay-tap (1777d4bb): Delay Tap module (In → delay_cubic 5 s → Out; time on 62.5 ms·(e^(ln81·x)−1), 21-step sync from transport bpm capped at 5 s, ms / ♪ mode as arithmetic so steps never recompile); reusable `temposlider` panel element (note markings at the playhead tempo, gravity 6 px with magnet + Shift bypass, live during playback via Face::frameTick).
- Verified (worker): -Affected -Slow 87/88 (pq_tests_timeline preview timing flake, 2/2 alone); follow-up pq_tests_views default tier pass (one SIGSEGV first run, not reproduced in 3 solo runs: NVIDIA flake). Goldens new: module-delay-tap-panel, module-delay-tap-sync.
- Perf: no new kernel; per sample = existing cubic delay + param ramp only while moving; the face does one compare per frame while playing.
- Notes: Q242. The delay line IS carried across recompiles (state_transfer verified); the worker's earlier "restart" was a test artefact.

## 2026-11-12 · DT.3 merged · main
- Did: merged task/DT.3-delay-face (cbf32457): body-drag moves the Delay Tap (tempo slider and generic slider took presses on their whole rect; now only groove/buttons); general horizontal stretch (Face::stretchX, edge drag in the patch canvas, undoable); whole-note fraction names; tempo_marks.h ranks n/d marks by simplicity, shown by screen-px room (stretch and zoom), gravity only to shown ticks.
- Verified (worker): pq_tests_views 685/685; -Affected 23/24 then pq_tests_document fixed (old label test) and passing. Goldens: delay-tap panel/sync re-recorded, -stretched new.
- Notes: Q243. Not done: edge stretch in the Device view's PanelHost.

## 2026-11-12 · DT.2 merged · main
- Did: merged task/DT.2-delay-sound (9f2be478): delay_cubic minimum 1 sample (linear read in [1,2), Hermite from 2); new delay_fade atom (glide within ±1 octave, larger jumps crossfade a second head at the old time, 30 ms equal power); the Delay Tap uses it, Time 0 = 1 sample. DelayState 96 → 352 B.
- Verified (worker): pq_tests_dsp default tier pass (test_delay_fade 8 cases), pq_tests_playback delay tap 6/6; click metric mode switch 1.48e-6 = ideal crossfade vs old ramp 0.1125. -Affected -Slow and benches stopped/skipped at the user's request; compiled after merge by the release pqdaw build.
- Perf: delay_cubic before 1.257 ns/sample (release, 1 lane); after not measured (+1 compare/fma/blend); delay_fade at rest = one compare per block.

## 2026-11-12 · NZ.1 merged · main
- Did: merged task/NZ.1-noise (444028bc): `noise_step` atom (value per step: rate 0.01 Hz … SR / trig / sync grid / note; colour filter on the drawn sequence, 4 sections, ±0.42 dB; fold for colours ≤ 0, bound-scaling above; density, steps, smoothstep glide; seed / restart / voices), `meta:noise` engine, `module:noise` panel + rich display. Lowering: a FREE group nothing reaches leaves its engine's inputs at their defaults (Q246).
- Verified: first post-rebase -Affected -Slow found ~45 failures from an over-broad unfed-group rule (fed series groups counted unfed: planned wires name the input `x`); fixed; then pq_tests_document / playback (4 shards) / views / dsp / meta default tier pass, noise slow cases dsp 16/16 meta 7/7. After the orchestrator's rebase onto DT.2 (atom-list conflicts, both kept): pq_tests_dsp default tier pass. No second -Affected -Slow (user: don't rerun 30-minute compiles).
- Perf (ship, pre-fix build, ns/sample 1 lane / ns per 8-voice frame; plain noise 0.19 / 1.4): white 0.34 / 1.47, 10 Hz hold 0.21 / 0.63, glide 3.8 / 4.0, pink 4.9 / 8.1, gaussian 1.6 / 7.0, sync 6.1 / 6.5. Not in bench/RESULTS.md.
- Notes: Q244 (blue/violet: scale not fold, the user's choice), Q245 (On note: one pattern for all notes), Q246 (unfed free groups). Goldens new: module-noise-panel, module-noise-panel-sync-trace.

## 2026-11-12 · TX.ports-always merged · main
- Did: merged task/TX.ports-always (cb295833): cause — the Envelope was placed inside the opened "Basic Synth" group, where blocks kept edge pins (GU.5b) and a panel block draws no jacks, so an unwired one showed none. Now every closed block's main ports orbit wherever it sits (value knobs too); a port over a capsule takes the press; symbol leads outside the body are pressable.
- Verified (worker): pq_tests_views 689/689 after rebase; new real-dispatch tests (Tab-place Envelope in the group + cable drags both ways; 237-placement catalog sweep). 32 goldens re-recorded (orbiting ports in opened groups).
- Perf: orbit solve now also for blocks in opened groups (same per-block cost).

## 2026-11-12 · TX.lane-drop merged · main
- Did: merged task/TX.lane-drop (2a44f3a7): FileDrop generalised over EditSurface (dropSpotAt / dropInsert); ProjectCanvas forwards dragOver / drop / leave to it; ghosts drawn by ProjectPatchView::paintLaneDrop. Same rules as the arrangement (sample_drop.h).
- Verified (worker): pq_tests_views default tier pass; 4 new real drag-dispatch tests; arrangement drop tests unchanged. Orchestrator: pq_tests_views on the merged main (both branches touch canvas.cpp).
- Notes: browser device drops onto the patch canvas are not handled (none today).

## 2026-11-12 · DT.4 merged · main
- Did: merged task/DT.4-repitch-samples (8a395136): delay_fade `crossfade` input (param rate, no recompile): Repitch (default) glides the head for every change along a velocity-carrying Hermite; Crossfade = DT.2 bit for bit. Delay Tap Glide param + face toggle; readout "667 ms | 1/3" on a marking, "3.00 ms | 144 smp" below 10 ms; micro bar (1 … 256 samples, drag / type) opened by a toggle. Cause of "no repitch on snap": DT.2 crossfaded any change beyond −30 / +15 ms of the head; snaps jump further.
- Verified: worker (before rebase) dsp delay 24/24, views delay 21/21 (real dispatch), playback delay tap 7/7 (interp == JIT). Orchestrator after rebase: -Affected 1 exe failed (pq_tests_graph: delay_fade missing from the atom list / kernel count since DT.2, schedule dumps' DelayState size since DT.2 + DT.4), fixed, pq_tests_graph pass; pq_tests_dsp default + slow pass; ship pq_bench_dsp links.
- Perf: release A/B, Crossfade per-sample jumps 19.9 / 30.3 → 20.0 / 30.8 ns (L1 / L8), at rest unchanged ≈ 1.3; Repitch per-sample jumps 16.5 / 21.5. DelayState 352 → 480 B.
- Notes: Q247. Goldens: delay-tap panel / sync / stretched re-recorded, micro / crossfade new.

## 2026-11-13 · AS.1 merged · main
- Did: ff-merged task/AS.1-autosave (1ac36f9a): app/autosave Autosaver (below-normal worker thread, latest-wins job) writes <session>/autosave/project.pqd + autosave.json; policy 10 s quiet / 60 s max, not mid-gesture; deleted when the document equals the saved snapshot, pref off or clean quit; SDL one-shot timer so an idle window still writes; Preferences → Record → Autosave.
- Verified (worker): pq_tests_app 39/39 (5 new), pq_tests_views pass (one SIGSEGV in test_meta_envelope_face first run: known NVIDIA flake). Golden re-recorded: record-prefs.
- Perf: idle tick 3 pointer compares; per write the UI pays a ViewState copy, serialisation and I/O on the worker.
- Notes: Q248. No live-app check; no autosave without the playback link. AS.2 next.

## 2026-11-13 · WI.1 merged · main
- Did: merged task/WI.1-blame (09a69c68): InferOptions::blameDepth / InferResult::blames (≤ 4 consumer/source edges per type diagnostic, upstream walk only along the offending field); ProjectCompiler infers at depth 2 and keeps lastTypeFailures(). Fixed: a failed update() kept never-published unit compiles (next update could call a unit "unchanged"); update() is now all-or-nothing.
- Verified (worker): pq_tests_graph + pq_tests_project new cases; -Affected default tier pass except the pq_tests_views NVIDIA SIGSEGV (passed alone).
- Perf: the walk runs only after a type error; a clean infer does no extra work or allocation.

## 2026-11-13 · AS.2 merged · main
- Did: ff-merged task/AS.2-recovery (5defae25): findStaleUnsaved returns dead sessions with an autosave (offered when untitled, or when it supersedes the origin's project.pqd); the recover popover's Recover mode ([Recover] [Show in Explorer] [Discard] [Later]) restores through guardUnsaved → ViewContext::recoverAutosave (dirty, empty history; untitled takes moved and relinked, unreferenced ones on a "Recovered" track; saved projects reopen under their folder).
- Verified (worker): pq_tests_app 40/40, pq_tests_views 701/701 (3 new real-click recovery tests). No goldens.
- Perf: start-up only; one stat + one small JSON read per stale session; takes moved by rename.
- Notes: Q249. REVIEW entry: kill the app mid-edit, restart, Recover.

## 2026-11-13 · WI.2 merged · main
- Did: merged task/WI.2-cut-wires (2c65e319): LowerOptions::cutWires + LoweredProject::wireEnds; compileNow maps lastTypeFailures to wires, cuts and recompiles (≤ 8 rounds), structural edits re-try affected cuts; Device view / project patch draw cut wires dashed red with the message; status line "A wire is not played: … The rest plays." Fixed: free bool inputs got a real zero and unconnected atom-block bool inputs read real 0 (a free `and` block used to fail its track).
- Verified (worker): pq_tests_document cut-wire cases 46/46, pq_tests_playback WI.2 2/2 (bit-exact vs the project without the wires), -Affected after rebase 45/45 (one earlier views NVIDIA flake). No goldens.
- Perf: clean projects lower with no cut set and never retry (typeRetries == 0 tested); a few wireEnds entries per wire.
- Notes: Q250. Messages still name atoms by internal label ("atom delay_fade, #0"). REVIEW entry for the red-wire look.

## 2026-11-13 · EX.1 merged · main
- Did: merged task/EX.1-export-core (fbb26630): Engine::renderOffline bus capture (ProjectProgram::setCapture, preallocated chunk buffers, beforeWrite hook); media::WavWriter streams PCM16/24/32f; app/export_render (runExport, makeOfflineLink, renderPass: PDC trim, loop wrap, tail, normalize, TPDF/shaped dither, cancel); `--render` on the same core.
- Verified (worker): pq_tests_engine EX.1 2/2 (RtScope), pq_tests_playback EX.1 8/8 (bit-exact vs --render; stems sum to master −141 dB; PDC trim; loop wrap); -Affected -Slow 87/88 (pq_tests_library watcher timing under load, 22/22 alone).
- Perf: one memcpy per captured channel per block; nothing when no capture; 32f exact-length export renames instead of rewriting.
- Notes: Q251 (fresh link per solo pass instead of a solo param write).

## 2026-11-13 · FZ.1 merged · main
- Did: merged task/FZ.1-frozen-tracks (86136237): Track::frozen + SetTrackFrozen (groups set each MIDI/audio member, one undo step); the frozen-content rule as one check at the end of apply() ("Frozen: unfreeze to edit"); LowerOptions::frozenSlotOf / captureOwn (`export own`), LoweredProject::frozenKeep, dropped wires Inactive.
- Verified (worker): pq_tests_document 222/222 (frozen 7/7); -Affected after rebase 45/45.
- Perf: unfrozen projects lower to the same IR; a frozen track with nothing kept = clip player + mixer (IR text checked).
- Notes: Q252. doc::wireStates / lowerParams don't know slots yet (FZ.2 reads LoweredProject::wires); recording into a frozen track fails at commit (FZ.3: disable arm).

## 2026-11-13 · EX.2 merged · main
- Did: merged task/EX.2-export-dialog (af0c3225): Export Audio window (ui::Popover over ExportPort; shared prefs form rows moved to prefs/form.h), File → Export Audio… / Ctrl+Shift+R, app ExportService (runExport on its own thread; whole-song tail 10 s), choices remembered in ViewPrefs::exportAudio.
- Verified (worker): pq_tests_views default tier pass (7 new real-dispatch tests, golden export-audio new, Preferences goldens 0 px), pq_tests_app pass (tiny interpreter export, cancel, mapping, settings).
- Perf: no window object when closed; ~30 Hz poll only while a job runs.
- Notes: Q253. REVIEW entry for the hand check.

## 2026-11-13 · FZ.2 merged · main
- Did: merged task/FZ.2-freeze-service (b99478da): app/freeze_service (planFreeze prunes the render document to the frozen track and the tracks cabling into it; renderTrackOwn renders `<id>:own` on a private link, tail to −90 dBFS ≤ 30 s, below-normal thread, 0.25 s debounce), cache/freeze/<id>-<key>.wav, the link publishes the render in a slot and frozenSlotOf names it only while the current key is loaded (stale → live); doc::wireStates with frozen slots; LowerOptions::frozenMono; per-clip AudioClipDesc::declick.
- Verified (worker): pq_tests_playback FZ.2 3/3 (frozen vs live −589 dB, re-render at a new tempo, unfreeze same patch hash, cache reuse / re-render); -Affected 45/45 (first run killed for low memory, re-run).
- Perf: frozen track = clip player + mixer (43 vs 82 nodes); renders off UI/audio threads.
- Notes: Q254. A fresh link's first ~26 ms depend on smoother history → EX.x-fix-settle. Self-feeding through a group/return/master falls back to the whole project, untested.

## 2026-11-13 · DY.1 merged · main
- Did: merged task/DY.1-dynamics-atoms: `follow(x, attack, release)` (retention per block when the times are block-rate, per frame when audio-rate; 1 lane = 8 lanes bit for bit), `window(x, time, op)` (van Herk–Gil-Werman max/min, chunked running-sum mean; L from the const time via ArgUse::Consumed → StateInit), `latency(x, frames, time)` (time folded into frames by optimise(); the scheduler refuses an unfolded time); envfollow.pqp on follow; JIT cache digest covers the new StateInit fields.
- Verified: worker: pq_tests_dsp / graph / jit (interpreter == JIT, RtScope, latency at 44.1/48/96 kHz) / stdlib envfollow 1.07e−6. Its -Affected -Slow was killed for low memory (two whole-suite runs at once); orchestrator re-ran after the rebase onto FZ.2: -Affected -Slow 88/88; ship link of pq_bench_dsp OK.
- Perf (ship, indicative, FZ.3 building): follow 1.88–2.29 ns/sample L1, 1.77–8.4 ns/frame L8; window max 0.53–0.78 / 1.7–2.7; mean 1.8 / 3.9–5.9; rows in bench/RESULTS.md.
- Notes: Q255.

## 2026-11-13 · FZ.3 merged · main
- Did: merged task/FZ.3-freeze-ui (3ec5aac2): Freeze / Unfreeze Track in the track and clip-area menus (group → members; disarms in the same step; Flatten shown disabled until FZ.4); header snowflake + render progress / failure; arm disabled; icy clips that refuse drags with an explanation; device lanes greyed; Device view veil with "running" pills on kept blocks; status line "Frozen: unfreeze to edit" [Unfreeze]; wiring.cpp uses the frozen-slot wire states; theme token track.frozen.
- Verified (worker): pq_tests_views default tier pass (5 new real-dispatch tests, goldens tracks-frozen / device-frozen new), pq_tests_app pass, after rebase.
- Perf: statuses re-read only when freezeGeneration moves; 10 Hz poll only while a render is unfinished.
- Notes: Q256. REVIEW entry. freeze_service doesn't bump the generation on progress (the bar reads per-track status directly; not exercised against the real service).

## 2026-11-13 · DY.2 merged · main
- Did: merged task/DY.2-sidechain (83c1a0cc): a cross-track cable into a signal input lowers as W::Sidechain (every channel, latency-compensated through the bus plan; units not merged); into a param / _add stays Modulation. A non-MIDI track's own signal can feed another track's block. Project patch draws Sidechain wires as modulation edges. BindingKind::Input; doc ops cableSources / crossTrackWiresInto / setCableSource; panel element `cablesource`.
- Verified (worker): pq_tests_document DY.2 2/2, pq_tests_playback DY.2 (Kick latency 480 → Bass: aligned exactly, outputLatency 480), pq_tests_views pass (picker by real dispatch); -Affected 41/45: 3 re-run passes (playback_s3 timing, library watcher, window selftest SegFault), pq_tests_ui failing on main since FZ.3 (theme token count) — fixed by the orchestrator (test counts + track.frozen).
- Perf: no new runtime code (the PDC ring); the element recomputes text only on snapshot change.
- Notes: Q257. Master not offered as a sidechain source.

## 2026-11-13 · FZ.4 merged · main
- Did: merged task/FZ.4-flatten-bounce (04bb751b): SetTrackKind; edit_render.cpp flattenTrack (frozen → audio in place, one warp-Off clip per sounding region, WAV copied from the freeze cache into samples/frozen/), bounceToNewTrack (new audio track below with the source's colour and mixer; source deactivated), consolidateAudio (Ctrl+J over audio tracks); ProjectLink::renderClips; app RenderEditService + views RenderEditPort (status-line progress and Cancel; result dropped if the track changed).
- Verified (worker): pq_tests_document FZ.4 5/5, pq_tests_views 8/8, pq_tests_playback FZ.4 5/5 (nulls −589…−595 dB; first 2 ms at song start ramped); -Affected 44/45 (library watcher flake, 22/22 alone).
- Perf: renders off the UI thread, below-normal, one job at a time; renderClips loads only clips sounding in the range.
- Notes: Q258 → FZ.x-fix (flatten drops kept-running blocks; 2 ms edge fade on render clips). Ctrl+J over MIDI + audio = two undo steps.

## 2026-11-13 · DY.3 merged · main
- Did: merged task/DY.3-transfer (ecda848a): graph::ControlEval factored out of LinearResponse; graph::StaticTransfer (stateful path → Unavailable); doc::GroupEvaluator base for GroupResponse and the new GroupTransfer; panel element `transfer` (units db, output value | reduction, live dot from the group's In) and meter `mode: reduction` (range 12/24/48); panel::SignalWindow shared signal reads.
- Verified (worker): pq_tests_graph static transfer 3/3, pq_tests_document group transfer 3/3, pq_tests_views DY.3 4/4 (goldens transfer-saturator / transfer-reduction new); -Affected 45/45.
- Perf: Hard Curve 8 ops × 128 points ≈ 37 µs per evaluate (check build), only when a value the curve reads changes.
- Notes: Q259. REVIEW entry.

## 2026-11-13 · MF.1 merged · main
- Did: merged task/MF.1-smf (4f0603ab): pq/document/midi_file (parseMidiFile: bounds-checked, running status, sysex, meta, RMID, unknown chunks; writeMidiFile; MidiTrack builders; resolveNotes: FIFO pairing, sustain, RPN 0, MPE zones, MTS 08 01/02/04/07/08/09).
- Verified (worker): pq_tests_document default tier (12 new cases incl. every-prefix truncation, 600 fuzz mutations, round trips).
- Perf: 10 MB / 3.7 M events parse ≈ 21 ms (check, idle); resolveNotes ≈ 155 ms.
- Notes: Q260. MPE at ±48 has 0.59 ¢ bend steps: MF.3's "within 0.01 ¢" can't hold at ±48. The merge commit cfc00c99 carried QUESTIONS.md conflict markers (orchestrator script error); fixed in the next commit.

## 2026-11-13 · DY.4 merged · main
- Did: merged task/DY.4-compressor (a0ed45b2): module:compressor from atom / value blocks only: detector mux (In / Sidechain child group: EQ Band low cut + gain / mix), Peak or RMS (window 5 ms mean) → dB → "Gain Computer" child group (soft knee; Expand downward, 70 dB cap) → follow (Log / Lin; Auto release max(fast, slow − 3 dB)); latency lookahead 0 / 1 / 10 ms; Dry/Wet gain_add; Listen; GR meter + modsource jack; panel 520×182 with transfer display, reduction meter, cablesource.
- Verified (worker): test_compressor_module 5/5 (static curve ≤ 1e-4 dB, interp == JIT; attack/release 479/4798 frames vs 480/4800; lookahead 480 reported and reduction before the transient; sidechain ducks −31.7 dB); after rebase pq_tests_playback / document / views pass (playback re-run after a low-memory kill). Golden compressor-panel new.
- Perf: defaults ≈ 4–5 ns per stereo sample, RMS + Auto + 10 ms ≈ 12 ns (ship, ~70 % load: indicative; idle re-measure at the audit). Only the chosen path is scheduled.
- Notes: Q261, Q262. Audio Effects browser list is hard-coded (Utility / Saturator / LFO) in 3 files: Compressor sits under Meta Modules.

## 2026-11-13 · MF.2 merged · main
- Did: merged task/MF.2-midi-import (481a1604): doc::ops::importMidi (one "Import MIDI" batch: tempo / signatures into a clipless project, createTrack, captureClip; exact int64 ticks; format 0 → one track, format 1 → one per MTrk; plain keys through the track's InputPitchMap, bent / MTS keys via InputPitchMap::atHz: Degree within 0.5 ¢ in Keys mode else Cents); views importMidiFiles used by FileDrop (arrangement and band lanes; .mid + audio = one undo step), loadClipItem and the browser.
- Verified: worker: pq_tests_document midi import 6/6, pq_tests_views file / lane drop 19/19, user library clips 1/1; its final -Affected was killed for low memory; orchestrator rebased onto DY.4 and re-ran -Affected: 45/45.
- Perf: 100k notes (⅔ bent) build ≈ 40 ms, dispatch 0.7 ms (check).
- Notes: Q263. test_file_drop's "refused MIDI" case now expects a broken .mid to report "could not import" (behaviour change, not weakened).

## 2026-11-13 · DY.5 merged · main
- Did: merged task/DY.5-truepeak (dda5378e): `truepeak(x)` atom (BS.1770-4 Annex 2 4× polyphase, 48 taps; fixed group delay 6, reports no latency); AVX2 kernel computing all 4 phases in one pass from mirrored sums / differences (38 vector ops per 8 samples).
- Verified (worker): pq_tests_dsp / graph / jit (1 lane == 8 lanes, block-size independent, impulse lag 6, 400 sines vs 16× reference −0.0016 … +0.222 dB, interp == JIT, RtScope); -Affected -Slow 76/77 → pq_tests_views alias fuzzy-match fixed, re-run pass; ship link OK.
- Perf: ship L1 0.750 ns/sample, L8 4.46 ns/frame, load 13 % (baseline rows).
- Notes: Q264 (the plan's ±0.2 dB is one-sided in practice).

## 2026-11-13 · MF.3 merged · main
- Did: merged task/MF.3-midi-export (fa193ed9): doc::exportMidi (format 1; tempo map / signatures track; loops unrolled; resolveClipPitches split out of resolveTrackPitches; modes 12edo, MPE lower zone ±48 with round-robin member channels (oldest stolen past 15), Degrees with .kbm inverse); clip menu Export MIDI ▸ MPE / 12edo / Degrees (ViewPrefs::midiExportMode); app::dragOutClips writes <clip>.mid.
- Verified (worker): pq_tests_document midi export 7/7 (MPE worst 0.2927 ¢; Degrees / .kbm identical through MF.2 import), pq_tests_app drag-out 1/1, pq_tests_views menu 1/1; -Affected 45/45 (before DY.5 landed; merged with a QUESTIONS append conflict only).
- Notes: Q265.

## 2026-11-13 · DY.6 merged · main
- Did: merged task/DY.6-limiter (d68381c8): module:limiter (atom blocks: gain, linked |x| or truepeak detector 7 frames behind, window max → follow → window mean → db gain on latency(x, 6, la), safety clip; Link Stereo / L-R / M-S; lookahead 1.5 / 3 / 6 ms); shared Follower (Auto release) used by the Compressor too (golden unchanged); panel 228×156; tests/app/dynamics_fixture.h shared.
- Verified (worker): test_limiter_module 6/6 (peak −0.999999 dB vs ceiling −1 dB without the clip at every lookahead; True Peak −0.88 dB true peak; latency 78 / 150 / 294; release within 0.00014 dB; L-R untouched channel bit-identical; interp == JIT); after rebase pq_tests_playback 4/4, views 1/1, document 1/1 on re-run (MF.1 parse-timing under load). Golden limiter-panel new.
- Perf: ship 9.1 ns per stereo sample (default), 12.0 with True Peak; fast_gain_to_db / fast_db_to_gain instead of the exact pair (13 ns).
- Notes: Q266. REVIEW entry.

## 2026-11-13 · FZ.x-fix + EX.x-fix-settle merged · main
- Did: merged task/FZ.x-fix-flatten (5a861327). FZ.x-fix: Flatten keeps the blocks frozenKeep keeps for other tracks (device lanes, cables), refused while the track's own source plays another track; AudioClip::noEdgeFade (saved only when set) on flatten / bounce / consolidate clips. EX.x-fix-settle: the bug was in later passes (a sleeping volume segment froze mid-ramp); the image host keeps a segment awake while its stage's param ramps move; settleOffline settles work arriving during the settle; runExport back to one link with solo param writes (D7).
- Verified: worker: pq_tests_document flatten 4/4, pq_tests_playback EX / FZ 18 cases (flatten / bounce null from frame 0, automated volume twice on one link bit-identical, solo passes == fresh links); its -Affected -Slow was killed for low memory. Orchestrator after the rebase onto DY.6: -Affected -Slow 62/63, pq_tests_timeline preview-player two-thread timing failed under load, re-run alone (default + slow) 2/2.
- Perf: one compare per sleep segment per block; stem exports save one compile per track.
- Notes: Q267, Q268. Live fix too: no stale gain fading in after a locate or a knob move on a silent track.

## 2026-11-13 · EX.3 + WI.3 merged · main
- Did: merged task/EX.3-flac (3af580f7). EX.3: libFLAC 1.5.0 (BSD-3, SHA-256 checked) built from its C sources as static pq_flac (cmake/flac/config.h, no Ogg / threads); media::FlacWriter (16/24-bit, level 5, streaming; quantizePcm shared with WavWriter); ExportFormat::Flac; dialog File Type WAV / FLAC. WI.3: doc::declaredWireClash (real → bool, event ↔ non-event on declared ends only) refuses drops in the Device view and the circuit view (no-drop cursor, red ring, status line) and in connectEnds.
- Verified (worker): pq_tests_media FLAC round trip bit-exact 16/24, pq_tests_app FLAC export == WAV export samples, pq_tests_views export 9/9 and WI.3 2/2; -Affected -Slow 88/88 (retried with 4 build jobs after a low-memory kill).
- Notes: Q269. All RB and DY tasks are merged; audit next (critique agent + -Full), then M7 after the user's go.

## 2026-11-13 · RB / DY audit · main
- Did: release-free audit at f1e6a932: `./scripts/build.ps1 -Full` (check preset, default + slow) 88/88, no warnings; independent review docs/reviews/RB-DY.md (0 C / 4 H / 11 M / 14 L; nothing built by the reviewer); idle ship re-measure of the dynamics (cost = row − eq oversample/none, per stereo sample): Compressor peak 5.4 ns, RMS + Auto + 10 ms 10.0 ns, Limiter default 10.4 ns, True Peak 12.5 ns (rows "RB/DY audit" in bench/RESULTS.md).
- Notes: fixes filed as AU.app / AU.dsp; AU.decide waits on the user (M3, M8, Q261). QUESTIONS hygiene (M11): missing Status lines added (Q246, Q250, Q252, Q254, Q256), Q258's doubled heading fixed. M7 starts only after the AU fixes and the user's go.

## 2026-11-13 · AU.app merged · main
- Did: merged task/AU.app-fixes (67f90e6a): H1 (a recovered autosave stays until this session's first write / save / New / Open), H2 (freeze files `<id>-<rate>-<hex>.wav`), H3 (freeze planning on the service thread, debounced; sum-fed sources key on what reaches the sum, whole-project fallback removed; the old render keeps playing until the new one loads), M2 (RF64 everywhere we write long WAVs), M5 (cache-only dead sessions swept), M6 (Save As copies renders off the UI thread), M10 (single-clock autosave offer), L1, L2, L3, L4 (Replace confirm), L6, L7 (key includes the executable hash and sample file size / mtime).
- Verified (worker): one regression test per finding (L2 by reading); -Affected 35/35.
- Perf: an unrelated edit while a track is frozen costs one render-document compare (was a whole-project serialisation on the UI thread + a live flip).
- Notes: Q270. AS.2's two recover tests had asserted the H1 bug (deletion at Recover): updated. User decisions M3 / M8 / Q261 sent to the AU.dsp worker.

## 2026-11-13 · VZ.1 · task/VZ.1-visual-settings (user request, merged fed0594e)
- Did: Spectrogram zoom: the picture stretches while the plot's size moves and rebuilds once it rests 0.2 s (was a full rebuild — texture, ring re-read, recolour, upload, new hop — at every 64-row / 12 %-column step of a zoom); empty slots skip the tonemap. One settings sheet (option_sheet.h) for Spectrum / Oscilloscope / Spectrogram: ANALYSIS / DISPLAY tabs, SAVE DEFAULT (meta::userDefaultData → doc::defaultMetaInstance; settings.json "metaDefaults"), RESET. Spectrum: 17 options (SpectrumFeed::setTuning retunes its analysers). Spectrogram: keyboard width / opacity / C tint.
- Verified: -Affected 33/34 then the one failure (module-spectrum-panel golden: the SET chip) re-recorded; after the rebase onto aed23b9f: pqdaw built, pq_tests_audio 1/1, pq_tests_app 1/1, pq_tests_views 1/1 (one known driver SIGSEGV in an EQ test on the first run; passed 3× alone and on the re-run).
- Perf / CPU-efficiency check: zoom of 1.5→4.9x: worst frame 10.6 → 0.7 ms, 10 rebuilds → 1 at rest (20.7 → 7.5 ms, empty-slot fast path); the sheet costs nothing unless open; a feed retune happens only when the tuning changes.
- Notes: Q271. Goldens re-recorded: device-meta-spectrum, project-meta-spectrum, module-spectrum-panel (the SET chip). Load-sensitive perf test "spectrogram feed: the UI's reads never wait" failed twice under machine load (p99 1.28 ms vs 1.0) — not touched by this change.

## 2026-11-13 · TU.2 wheel fix (the user's uncommitted change) · main
- Did: committed the user's working-tree fix: MosWheelPopover::opened() (called by openMosWheel once the popover is in the tree) creates CircleNotes; in the constructor its frame request was dropped, so a wheel opened during playback never lit the playing note. Test "a MOS wheel opened during playback lights the playing clip note" (test_tuning_live.cpp).
- Verified: pq_tests_playback default tier (one meta-wavetable generation check failed under another checkout's build load, passed 3/3 alone; the new case passes), pq_tests_views pass (one NVIDIA SIGSEGV first run).

## 2026-11-13 · AU.dsp merged · main · RB / DY closed
- Did: merged task/AU.dsp-fixes (b5b7fbde): H4 (sleep plan bounds values and treats provable 0 as silent; settled followers / smoothers sleep; non-finite params fall back to their default; undriven constant value blocks lower as literals), M1 (a param ramp wakes only the segments that read it), M4 (follow / window read inf as 0), M7 (linear MIDI pairing; imports > 32 MB refused), M9 (Expand / RMS / Lin / Auto value tests), L10, L11, L13 (Compressor GR + Out meters labelled, dB scales, Auto under Release); user decisions: Limiter M-S removed (Q266), MIDI per-file pitch rule (Q263), Compressor Detector / Law chips (Q261). SPEC Limiter row + §15 updated.
- Verified (worker, after rebase onto bccfafe3 = AU.app + TU.2): -Affected -Slow 75/76; the device-chains param-count slow test resized (fewer params now that constants are literals: 192 → 224 tracks), re-run passes; ship pq_bench_dsp links. Goldens re-recorded: compressor-panel, limiter-panel; lowering fixtures lower_meta_voice_run, audio-track (two constants now literals).
- Perf (ship, indicative): dynamics rows within ±5 % of the audit's idle numbers; the gain is sleeping on silent tracks.
- Notes: Q272. RB and DY are done and audited; open lows L5, L8, L9, L12, L14 (docs/reviews/RB-DY.md). M7 deferred by the user.


## 2026-11-13 · BR.ui (user bug reports) · task/BR.ui-playhead-mixer (merged e707c4dd)
- Did: playhead trail in the patch view lanes (the per-frame column repaint drew the click's insert marker at its left edge; same in the arrangement lanes), partial repaints clipping ruler labels, start-marker moves not damaging lane rulers; plain click on an automation sub-lane / group / return / master lane locates; any socket a face draws is pressable where drawn (mixer out was out of the 8 px edge band when zoomed); Spectrum / Scope / Spectrogram settings open as a side panel beside the face (right, flip left, over the plot only if neither fits).
- Verified (worker): pq_tests_views default tier 739/739; new real-dispatch tests (partial-vs-full render through playback, sub-lane / group locate, mixer cable drag, side sheet); the fixes reverted made the new tests fail.
- Perf: per-frame cost unchanged; the side sheet is outside the faces' live damage.
- Notes: 11 project-* goldens re-recorded (ruler left-end labels only). Bug reports 2026-11-13_17-17-10, _17-39-14 → fixed/.

## 2026-11-13 · BR.wt (user bug report) · task/BR.wt-shaper-range (merged c30dac00)
- Did: report 2026-11-13_17-42-08 measured on its project: the Wavetable was in Phase mode with a ±0.09 sweep round the sine's trough, so the near-DC output is the curve there (DSP, tap and display agree; the Operator adds no DC). Real mismatch fixed in Shape mode: the idle dot sat at u = phase, the DSP reads (phase + 1)/2; one mapping dsp::shapeTablePosition for kernel tap and face.
- Verified (worker): pq_tests_dsp BR.wt, pq_tests_views wavetable face 22/22; -Affected -Slow 76/76 after the rebase; ship pq_bench_dsp linked.
- Perf: kernel expression unchanged (same scalar, now an inline function).
- Notes: Q273 (Shape mode's phase slider only biases into the upper half). Duplicate Q271 heading fixed: AU.dsp's is Q272.

## 2026-11-13 · BR.patch-io (user suggestion 2026-11-13_19-10-25 + requests) · task/BR.patch-io
- Did: opened groups lose the In/Out pentagons: frame sockets drawn like module ports (main In/Out + one per crossing connection, the closed block's ports), crossing wires as two cables meeting at the socket; an audio track with a recording input gets a left-edge band socket and a cable to where its clips enter the chain (dim while not monitoring); routes / sends run down a right-hand trunk, along the gap above their band and into its left socket, then a hanging cable to the Σ (groups, returns, master); Ctrl+Alt+P fits the project's width with the first visible arrangement row's band at the top (the morph uses the same camera). Dragging from a closed group's crossing output port wires from the inner block (was broken).
- Verified (worker): pq_tests_views default tier 741/741 (frame sockets through real drags, input cable + master socket, scrolled-arrangement morph camera); -Affected after rebase onto 83130931: 20/20.
- Perf / CPU-efficiency check: UI-thread scene build / paint only; socket→cable lookup built once per scene, no per-frame allocation.
- Notes: 47 goldens re-recorded (project-patch-*, project-cables*, device-* group panels, panel-window-*, meta faces in the patch). Q274 (socket order follows inner pins; sends end on the return's left socket). Open: the bus along band edges can read like a band outline.

## 2026-11-13 · BR.wires · task/BR.wires
- Did: thinner wires (audio traces 1 px at 100 %, cables 4.5·zoom clamped 1.25…12 px); flow arrowheads on traces (world multiples of a power-of-two 140…280 px spacing, shared runs coincide) and one mid-length arrow per cable; each bus trunk / detour column per destination, kBusColumn = 8 apart, sorted by the target band's y (lower further out), no `% 3` wrap.
- Verified: pq_tests_views default tier 742/742 after re-recording 74 goldens (every scene drawing wires; project-mixer, project-cables, project-patch-overview checked by eye).
- Perf / CPU-efficiency check: arrows are ≤ a few AA triangles per visible segment, none at Far LOD (cached chunks untouched); column sort is per layout, O(targets log targets).
- Notes/blockers: none.

## 2026-11-13 · BR.bug-close · task/bug-close
- Did: the F12 panel's ticket rows show a × while hovered; clicking it closes the ticket: `closeTicket` moves the folder to `closed/` beside it (bug_reports/closed/, suggestions/closed/; `_2` suffix on a taken name), `listTickets` skips `closed/`. A picked ticket goes back to the new report first (its text kept); the list keeps its scroll; the message line says where it went. Nothing is deleted (user request).
- Verified: pq_tests_views `--test-case="bug report*"`: 6/6 (new: closeTicket on disk; the panel's × by real dispatch: pick, close, back to new report, row body only picks). No golden renders the panel.
- Perf / CPU-efficiency check: one rename + one re-list per click; nothing on paint paths beyond one hover rect.

## 2026-11-13 · BR.input-port (user requests 2026-11-13) · task/BR.input-port
- Did: every audio track gets its left-edge input socket on the spine; its lane's chain link is drawn as a trace out of the lane's left end, round the band's left edge, into the socket, and one long cable (the same wire: Z / insert / Delete act on it) runs on to the first block or mixer, no longer dimmed by monitoring (it carries the clips). Group bands have no lane (no timeline strip, no sub-lanes); their members' buses into the group's socket feed them.
- Verified: pq_tests_views default tier 742/742 (BR.patch-io input test rewritten for the lane trace; TL.7 Z-insert now through the socket cable; lane-edit locate click moved to the master's lane).
- Perf / CPU-efficiency check: layout-time only (one 4-point trace per audio lane, no new per-frame work); group bands paint no lane at all.
- Notes: 10 goldens re-recorded (project-patch-overview/band/lanes/lane-edit/auto-wire, project-mixer, project-circuit-device, project-meta-scope/spectrum/spectrogram): the group lost its lane, the audio lanes' trace in project-patch-lanes.

## 2026-11-13 · AC.look (user request) · task/AC.look-clip-view
- Did: audio clip view redone in the device panels' look. Clip / Warp / mode cards: arc knobs (Detune, Gain, mode settings, lit by value), chip rows replace every dropdown (warp mode, Preserve, Transient Loop), lit toggle chips (Reverse, Loop, :2, ×2), fill boxes for the fades. Transpose is a knob: Up/Down = one tuning step, Shift = one period, typed steps / cents / exact ratios ("5/4 · 386.3 ¢"). Sample display: filled peak + RMS waveform on a dark field (playing region lit), bar.beat / seconds ruler, warp segments tinted by speed with the factor written, marker tags, fade curves. New reusable controls in clip_controls.h/.cpp.
- Verified: pq_tests_views default tier 744 cases (new real-dispatch tests: chips, transpose keys / Shift / type-in / wheel / drag, knob fill); after the rebase `-Affected` 20/20. 3 goldens re-recorded (audio-clip-warp-beats, audio-clip-warp-markers, clip-view-audio); no other golden moved.
- Perf / CPU-efficiency check: waveform columns cached per (clip, range, width, overview); paint reads the cache, no per-frame allocation or sample rescans.
- Notes: steps read "+7 stp" (typed "+7st" means semitones); for long files the RMS core is estimated from the overview's peaks.
