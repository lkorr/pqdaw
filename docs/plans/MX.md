# MX: the Math module and the radial menu

User 2026-11-18: "a boolean / combining / math meta module, that receives two inputs, you change the sign in the
middle of the module by clicking on it to open up a nested popup menu of boolean values (< > == etc) or general math
functions … all of the important sound related functions one would possibly want in patching … two input functions,
oscilloscopes to show the input audio and an oscilloscope to show the outputting audio … clicking on the symbol should
open up options like a sims bubble text thing, where options open in a circle around (boolean, math etc) which open up
more options … one should be able to just manually enter values as well in an enterable value right below inputs."

Decisions (asked 2026-11-18):
- **A group preset** (one hierarchy, SPEC §15 2026-11-07; like the VCA, `pq/document/modules.h`): `ModuleKind::Math`,
  catalogue item `module:math`, name "Math". No new container, no meta type.
- **Op change swaps atoms**: picking an op rewrites the group's op subgraph (one undo step, recompile + click-free
  hot-swap). Zero runtime cost for the ops not chosen; the opened circuit shows the real op. Not automatable.
- **Categories in v1**: Compare, Logic, Arithmetic, Range, Geometry. (Audio / mod — ring mod, AM, crossfade, S&H,
  gate-by — left out by the user; don't add them.)
- **Oversampling** Off / 2× / 4× on the face, off by default: the group's own `oversample` setting
  (`kGroupOversample`, TX.group-oversample), not a new mechanism.
- **Layout** (user's sketch):
  ```
    (A)      [~ out scope ~]      (B)        ← In terminals A, B top corners
    [0.50]                      [0.00]       ← value entry boxes right below the inputs
   [~ A ~]     (    >    )      [~ B ~]      ← input scopes left / right, the op symbol in a big circle
                   (out)                     ← Out bottom centre
  ```

## Semantics (defaults chosen by the orchestrator; the most reversible choice — note any change as a Q)
- **Inputs**: A = In terminal A + value block "A"; B = second input + value block "B". A wire ADDS to the typed value
  (the wire law, PATCH_LANGUAGE §3.2), so unwired B with 0.5 typed is "compare against 0.5". Defaults A 0, B 0.
- **Truth on audio**: logic ops read a signal > 0 as true (±1 squares and 0/1 gates both work).
- **Compare and Logic output gates** (bool; reaching a real input it is 0 / 1 through the existing `gate_to_real`).
- **== is equal within a Tolerance**: |A − B| ≤ Tolerance, a third value block shown only while == or ≠ is the op
  (default 1e-3; 0 = exact, useful on stepped / integer control signals). ≠ is its negation.
- **Ops** (two-input unless marked; the symbol drawn in the circle in brackets):
  - Compare: A < B, A > B, A ≤ B, A ≥ B, A == B, A ≠ B (== / ≠ with tolerance)
  - Logic: and, or, xor, nand, nor, xnor, not A (unary)
  - Arithmetic: +, −, ×, ÷, mod, pow, average (A+B)/2, |A − B|
  - Range: min, max, clip A to ±|B|, wrap A into ±|B|, fold A into ±|B|
  - Geometry: atan2(A, B), hypot(A, B)
- **No NaN / Inf out** (an audio module must not blow up a mix): ÷ by |B| < 1e-9 → 0; mod by 0 → A; mod is floored
  (result has B's sign; `mod` atom semantics if they match, else built from `floor`); pow = sign(A)·|A|^B, and 0^negative → 0;
  wrap / fold / clip with B = 0 → 0. Write the rule into the module's header comment.
- **Ops are subgraphs of existing atoms** wherever possible (`gt lt ge le and or xor nand nor not add sub mul div mod pow
  abs sign floor frac clip min max sqrt select`); hypot = sqrt(A² + B²). **atan2 is a new stateless atom** (DSP kernel,
  JIT codegen, silence info: None; fast polynomial approximation, ≤ 1e-5 rad error, both backends), added to
  PATCH_LANGUAGE and the atom tables.
- **Op identity**: the group stores the op key as a setting (`math.op`, a string key like "gt", "hypot"); picking an op
  rebuilds the op subgraph from that key. If a user hand-edits the circuit inside, the face shows the stored op
  (no recognition pass); record that as a Q.
- **Scopes**: three oscilloscopes — A (after the value is added), B, Out — sharing ONE trigger (from A) and ONE timebase,
  so the three line up in time. Reuse the existing oscilloscope engine/face code (`pq/meta/oscilloscope.h`,
  `views/src/meta/scope_face.cpp`), reshaped: the input scopes small, the output scope the wide top box. How to share
  the trigger is the worker's call (a sync input on the scope engine, or one tap with three channels); no
  per-instance special case in the scope code that another module couldn't use. A closed / off-screen face must cost
  what an off-screen Oscilloscope costs today (measure the module with its face hidden vs a bare op).

## MX.1 Radial menu widget (ui) 👁
A general nested radial ("bubble") menu in `ui` (`pq/ui/radial_menu.h`), not Math-specific: any face can open it with a
tree of items (label, optional glyph, children).
- Opens centred on an anchor point (the clicked symbol). Level 1: category bubbles in a ring around the anchor. Hovering
  (or clicking) a category opens its children as a second ring / arc outward from that category; the open category
  stays highlighted. Clicking a leaf picks it and closes the menu. Esc, a click outside, or right-click closes it (Esc
  in a sub-ring first goes back one level).
- Typing filters: typed text (">", "max", "hyp") highlights matching leaves across all categories; Enter picks the
  best; Backspace edits. The current item is marked when opened.
- Rings stay on screen: clamp / rotate the arc at window edges. Text legible at the views' scales; colours from the theme
  tokens; a short open animation (≤ 120 ms, none while reduced-motion is set if that exists).
- Keyboard: arrows move around a ring, Right/Enter go in, Left/Esc go out.
- Tests through **real event dispatch** (window → overlay → widget), every path: hover-open, click-pick, type-pick,
  Esc out, click outside, the menu destroying itself and the scene rebuilding inside the pick handler (the TX.node-menu
  Tab-place crash came from exactly this). Goldens: closed / level 1 / level 2 / filtered, 1× and 2×.
- CPU: no allocation per frame while open (build the layout once per open / per level change); paint only while open.

## MX.2 Math module: preset, ops, atan2 (document + dsp + jit)
`ModuleKind::Math` in `pq/document/modules.h` / `modules.cpp`: `mathBlocks(ids, MathOptions, MathRoles*)` building the
group (In A, In B, value blocks A / B / Tolerance, the op subgraph, the three scope engines, Out), `mathOpInfos()` (key,
category, symbol, display name, arity, output is gate), and the edit that swaps the op subgraph (one undo step; keeps
the value blocks, wires into A / B and from Out). Spawned everywhere the VCA is (node menu, Add Free Device, browser,
`library_load`). The atan2 atom. Tests: every op against a C++ reference over a sweep incl. the edge cases above (0,
±tiny, ±huge, NaN-free), on both backends; op swap round-trips through save / load and undo; oversample setting lowers
to the region. **Does not do the face** (MX.3).

## MX.3 Math face 👁 (after MX.1 and MX.2)
The closed panel per the layout above: two value entry boxes (typed numbers; drag-to-adjust if the panel's number
element already supports it), the op circle (the symbol large, centred; click opens the MX.1 radial menu with the five
categories; picking applies the MX.2 op swap), the three scopes, the Off / 2× / 4× selector, the Tolerance box only
while == or ≠. Gate-output ops draw the output scope as a 0/1 trace. Real-dispatch tests: click symbol → pick op through the
menu → the circuit changed and undo restores it; type a value → it lowers. Goldens for a few ops; REVIEW entry.

## Lanes
- Lane w1: MX.1 → MX.3. Lane w2: MX.2 (MX.3 starts after both have merged).
