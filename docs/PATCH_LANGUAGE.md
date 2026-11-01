# The pqdaw patch language (`.pqp`), v0

The text form of the circuit-diagram language (SPEC §7.3). The visual editor and this text are two views of one graph: `parse(print(p)) == p` must always hold, layout included. Stock subcircuits and devices in `stdlib/` are written in this language.

v0 is deliberately small. Extend it by editing this file first, in the same commit as the parser change.

## 1. Lexical structure
- UTF-8. Comments are `// …` and `/* … */` (not nested).
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`. Keywords: `patch import param in out export solve iterations layout true false`.
- Numbers: decimal or scientific (`0.5`, `1e-3`, `440`), with an optional **unit suffix**, no space allowed:

| Suffix | Meaning | Normalised to |
|---|---|---|
| `Hz`, `kHz` | frequency | Hz |
| `ms`, `s` | time | seconds |
| `smp` | samples | samples |
| `dB` | gain | linear amplitude |
| `c` | cents | cents |
| `st` | 12edo semitones | cents (×100) |
| `%` | percent | fraction (×0.01) |

- Ratios are written `3/2` as an expression; the compiler folds them as constants.
- Strings: `"…"` with `\"` and `\\` escapes (used only by `import` and `layout`).

## 2. Grammar (EBNF)

```ebnf
file        = { import | patch } ;
import      = "import" string [ ";" ] ;
patch       = "patch" Ident "{" { member } [ layout ] "}" ;

member      = param | input | output | export | binding | solve | region ;
param       = "param" Ident [ ":" valueKind ] "=" expr [ range ] { attr } ;
range       = "[" expr ".." expr "]" ;
attr        = "@" Ident [ "(" [ argList ] ")" ] ;          (* @unit(Hz) @curve(exp) @label("Cutoff") @steps(4) @hidden *)
input       = "in"  Ident ":" portType [ "=" expr ] ;      (* default used when unconnected *)
output      = "out" Ident ":" portType ;
export      = "export" Ident [ "=" expr ] ;               (* a device jack on top of the device; without a value: not driven yet (T4.4) *)
binding     = target "=" expr ;
target      = Ident | Ident "." Ident ;                    (* output name, or local *)

portType    = valueKind [ rate ] [ poly ] [ width ] ;
valueKind   = "real" | "bool" | "audio" ;                  (* "audio" = real at audio rate *)
rate        = "const" | "param" | "audio" ;                (* upper bound; inference may lower it *)
poly        = "global" | "voice" ;
width       = "mono" | "stereo" | "ch" "(" Int ")" ;

solve       = "solve" "(" "iterations" ":" Int ")" "{" { unknown | equation } "}" ;
region      = "oversample" "(" Int [ "," "phase" ":" valueRef { "," valueRef } ] ")" "{" { binding } "}" ;  (* §2.3 *)
valueRef    = Ident [ "." Ident ] ;                        (* a named value: `ph`, `filt.hp` *)
unknown     = "unknown" Ident [ "=" expr ] ;               (* initial guess, default: previous sample's solution *)
equation    = expr "==" expr ;

expr        = ternary ;
ternary     = logicOr [ "?" expr ":" expr ] ;
logicOr     = logicXor { "||" logicXor } ;
logicXor    = logicAnd { "^^" logicAnd } ;
logicAnd    = compare { "&&" compare } ;
compare     = additive [ ( "<" | "<=" | ">" | ">=" | "==" | "!=" ) additive ] ;
additive    = term { ( "+" | "-" ) term } ;
term        = unary { ( "*" | "/" | "%" ) unary } ;
unary       = [ "-" | "!" ] postfix ;
postfix     = primary { "." Ident | "[" expr "]" } ;      (* .lp on multi-output nodes; [i] channel select *)
primary     = Number | "true" | "false" | Ident | call | "(" expr ")" | tuple ;
call        = Ident "(" [ argList ] ")" ;
argList     = arg { "," arg } ;
arg         = [ Ident ":" ] expr ;                         (* positional args first, then named *)
tuple       = "(" expr "," expr { "," expr } ")" ;          (* stereo/multichannel literal *)

layout      = "layout" "{" { layoutItem } "}" ;
layoutItem  = "node" key "at" point [ "rot" Int ] [ "flip" ]
            | "wire" key "." Ident "->" key "." Ident [ "via" "(" pointList ")" ]   (* the last Ident is the port *)
            | "comment" string "at" point ;
key         = Ident { "." Ident } ;                        (* a node name, then input steps (§2.2) *)
pointList   = point { "," point } ;
point       = "(" coord "," coord ")" ;
coord       = [ "-" ] Number ;                             (* no unit; read as float32 *)
```

`unknown` (in solve blocks) and the layout words `node at rot flip wire via comment` are contextual: they are only special in those positions. So are the port-type words (`real bool audio const global voice mono stereo ch`), and `oversample` / `phase` (TMM.6: `oversample(` at the start of a member opens a region; `oversample = …` is an ordinary binding).

### 2.1 Clarifications (v0, implemented by the T1.2 parser)
Decided while implementing the parser; the open ones are tracked in [QUESTIONS.md](QUESTIONS.md) (Q7–Q10, Q18–Q22).

- **Line breaks** are insignificant except in three places: a port type ends at the end of its line (so `in x : real` followed by a line `mono = 1` or `param y = 2` is not read as one type, Q21); inside a `solve` block each `unknown` or equation is one line, unless it is inside brackets (an equation may start with `(`, `-` or `!`, Q21); and an `@attr(` must open its arguments on the attribute's line.
- **Numbers**: `%` directly after a number is the percent unit (`50%`); with a space it is the modulo operator (`a % 2`). A unit suffix is any identifier glued to a number, and must be one of the table above. `-` followed directly by a number literal is a negative literal (`-3dB` is the literal −3 dB); `-(3)` is the negation of a literal. Only one prefix operator is allowed per operand (`-(-x)`, not `--x`). Comparisons do not chain (`a < b < c` is an error).
- **Params**: the default and the range ends are constant expressions, folded by the parser (QUESTIONS Q10): a plain literal keeps its unit, arithmetic gives the normalised number. Without `: kind`, the kind follows the default (`true`/`false` → bool). A `[` after the default always starts the range. Attribute arguments are literals: a number (with unit, optionally negative), `true`/`false`, a string, or a word; `@hidden` and `@hidden()` are the same.
- **Names**: one namespace per patch for params, ins, outs, exports, unknowns and bindings. `in`/`out` can be names (Q7). The built-in names `voice`, `transport`, `track`, `sidechain` and `sr` cannot be redefined. A plain name that is not bound in the patch may name a built-in atom without inputs (`sr`); others are called (`pulse()`). `voice.hz`, `transport.bpm`, `track.in` etc. are always the built-ins.
- **Bindings**: `y = x` (a name, optionally with `.output`) is an alias with no node of its own; any other right-hand side is a node, and the binding names it. `out = expr` drives the output `out`; an `out` or an `export` cannot be read as a value (bind the value to a name and use that). `node.input = expr` (the `Ident "." Ident` target) connects `expr` to an unconnected input of a named computed node, e.g. `h = samplehold(x)` then `h.trig = rise(g)`; a call's required inputs may be supplied this way (Q20).
- **Calls** resolve to a patch of the same file, then an external patch (imported, or a prelude given by the compiler), then a leaf atom; a patch may not have the name of an atom (Q18). TMM.3: `meta_<type>` names are the meta-module prelude's (§3.2). Positional arguments come first and fill inputs in order (a subcircuit's inputs are its `in`s, then its `param`s); named arguments name an input. A call used as a value gives its first output; `.name` selects another. Repeated ports of variadic atoms are numbered in text: `mux(i, a, b, c)` has inputs `index, in0, in1, in2`; `demux(i, x)` has outputs `out0, out1` (v0 cannot express more, Q19).
- **Solve blocks**: an equation is an expression whose top-level operator is `==`; each side is a real expression.
- **Imports**: the string is recorded as written and resolved relative to the importing file by the compiler's loader (the parser does no file I/O); calls may use any patch of an imported file. `;` after an import is optional. Reading an `out` and negative literals: Q22.
- **Limits**: an expression may be at most 128 levels deep (a sum of 128 terms) and nest at most 129 levels of brackets/arguments; deeper text is an error, never a crash.
- **Exports** (T4.4): `export e` without `= expr` declares a jack that nothing drives yet (validation reports it as a dangling port); the circuit editor adds a jack before the wire into it, and the printer writes an undriven export this way.
- **Printing**: `print` writes a canonical form (declarations, bindings, outputs, exports, solve blocks, layout), inlining every unnamed node that has exactly one consumer. `parse(print(f)) == f` for every parsed file, and `print` is stable under a second round trip. Placing or routing a node never changes how the rest of the patch prints (T4.3: before, a placed anonymous node got a generated `_n` name).

### 2.2 The layout block (T4.3)
The circuit diagram's layout (SPEC §7.3 "Layout is stored") is the `layout { … }` block at the end of a patch. The compiler never reads it; the printer keeps it; the circuit view (T4.2) draws it and the auto-layout / tidy command (T4.3, `pq/graph/layout.h`) writes it.
- **Keys**: a layout line names its node by a *key*. A named node (a declaration or a binding) is its name. An anonymous node — one written inline inside an expression, the usual case (`y = tanh(x * g)` has two) — is the key of the node that consumes it, a dot, and the consumer's input it drives: in `y = tanh(x * g)` the `tanh` is `y.in` (the output `y`'s input is `in`) and the multiply is `y.in.x` (tanh's input is `x`). Input names are the ones calls use (`a`/`b` for binary operators, `in0`… for variadic ports, an `in`'s default is `name.default`, an `unknown`'s guess `name.init`). Nodes under a solve equation, which has no name, start at `_e<k>`: the k-th equation of the patch in text order (`_e0.lhs`, `_e1.rhs.x`); if the patch has a node of that name the printer appends `_` until it is free (`_e0_`), and the parser reads `_e<digits>` followed by any `_` as that equation whenever no node has the name.
- **Wires**: `wire <key>.<output> -> <key>.<input> via ((x, y), …)` gives a wire's routing points (the pins are the ends; a wire with no `wire` line is drawn straight or as a Z). The last part of each end is the port.
- **Order**: the printer writes node lines in a canonical order (each statement in print order, then depth-first the anonymous nodes inlined into it, input by input) and wire lines by target, so text → layout → text is byte-stable.
- **Coordinates** are world units (the canvas's logical px at zoom 1), top-left corners; `rot` (0/90/180/270) and `flip` are kept but not drawn yet (QUESTIONS Q119).
- **Stale keys**: a key that names nothing is an error, or with `ParseOptions::lenientLayout` dropped and counted (a text pane edit must not fail on a layout line whose expression moved, T4.4); `graph::carryLayout` then gives nodes that kept their NodeId (T1.x-text-ids) their previous position.
- **Hand-written files**: `graph::spliceLayout(source, parse, laidOut)` replaces only each patch's layout block in the original text (inserting one before the closing `}` where there was none), so the author's comments and formatting stay.

### 2.3 Oversample regions (TMM.6; docs/plans/MM.md, SPEC §7.2 "Structure")
`oversample(n) { … }` computes the bindings inside it at n times the sample rate (n ∈ {1, 2, 4, 8, 16}); the rest of the patch is unchanged. Example: `oversample(4, phase: ph) { y = fold(wavetable_pos(warp(ph), pos)) }`.
- **What is inside**: every node the region's bindings create — the right-hand sides, the anonymous expressions in them, the calls (a subcircuit called inside runs inside, all of it). Names bound inside are ordinary names of the patch: readable outside, `out = …` may be written inside. Declarations (`param`, `in`, `out`, `export`), solve blocks and other regions cannot be inside one (a subcircuit with a solve block can be called inside; a subcircuit holding a region cannot: **regions do not nest**). An alias (`y = x`) inside has no node and so changes nothing.
- **Crossing the boundary** (infer() inserts the converters, pq/graph/infer.h, pq/dsp/oversample.h): an outer **audio-rate** value read inside is upsampled — through polyphase IIR half-band filters (one stage per ×2, ≥ 116 dB stopband over everything that would fold below 20 kHz at 44.1 kHz and above, passband flat within 1e−11 dB to 20 kHz); a value named in `phase:` by unwrapped linear interpolation (the shortest way round each wrap, exact for ramps: a phasor stays a phasor); a **bool** by holding it (a gate stays a level; a trigger becomes high for its whole frame, which `rise()` inside turns back into one pulse). A value computed inside and read outside is decimated through the same half-bands (a bool: high when any sub-sample is). `param` and constant values pass as they are (they are the same at every rate). Engine inputs (`voice.*`, `transport.*`, `track.in`, `sidechain.in`, clip players) are base-rate values, and sends and voice reducers (`voice_sum` …, including the implicit ones) always run at the base rate: written inside, they are computed outside with converters around them. One converter per value and region, shared by its readers.
- **Inside**: `sr` is n·sr, and durations (delay times, `max`, `timer`, `pulse`, smoothing times) count the region's samples, so stateful atoms and filters behave exactly as they would at that rate (a `phasor(440)` inside sounds 440 Hz; an `smp` literal inside is a sub-sample).
- **Latency**: the half-bands' group delay (up + down, base-rate samples, DC / 10 kHz / 20 kHz at 48 kHz): ×2 3.62 / 4.58 / 12.5, ×4 4.74 / 5.76 / 13.9, ×8 5.17 / 6.19 / 14.3, ×16 5.31 / 6.33 / 14.5 (44.1 kHz: ×2 3.62 / 4.80 / 19.2 … ×16 5.31 / 6.56 / 21.2). It is part of the sound, not reported as latency (no PDC), like Saturator's HQ half-bands. A phase input arrives (n − 1)/n base samples late (its last sub-sample is the current value).
- **Limits**: a feedback loop must be entirely inside a region or entirely outside (a loop through the boundary is an error). `oversample(1)` is the plain nodes. A value inside a region is not offered to live probes, and a region never sleeps (SPEC §7.7: its filters ring on; the patch around it still does).
- **Printing**: each region is printed after the patch's other bindings, with its bindings (and the outputs whose expression is computed inside) in a block: `oversample(8, phase: ph, filt.hp) {` … `}`. A phase input is printed as the named node it resolves to (an alias is followed). An expression is written in its consumer's region; a node in another region than its consumer gets a statement (a generated `_n` name) in its own.
- **CPU**: the region's ops run block-wise on n·frames (both backends: a region is a Block / Loop region with a factor, pq/graph/schedule.h); converters are native AVX2 kernels over the 8 voice lanes; nothing outside the region pays. Costs in bench/RESULTS.md (TMM.6).

## 3. Semantics
- **Bindings are a dataflow graph, not statements.** Order doesn't matter, and a name may be used before it is bound. Each binding is one node (or a subgraph of nodes).
- **Cycles**:
  - A cycle that goes through `z1(…)`, `delay(…)` or a stateful atom is ordinary feedback.
  - A cycle with no delay is an **error** in text, unless it is inside `solve`. The visual editor inserts an explicit `z1` and shows a warning instead: T4.4's editor puts the `z1` on the wire that would close the loop (`pq/graph/edit.h` connect), so the saved text is valid and shows the delay; the language's rule does not change. (SPEC §15 2026-10-24's automatic z⁻¹ on cross-device wires is the same rule one level up.)
  - For now a cycle whose only delay is `phasor`'s or `sine_hz`'s `hz` (or a sample player's `rate`) is reported the same way: those atoms have no split kernel yet, so put a `z1` into such a loop (QUESTIONS Q43). Loops through `z1` and delay lines are scheduled per sample, or in sub-blocks as long as the shortest delay allows (T1.5).
  - Zero-delay-feedback filters write their solved form explicitly (see `stdlib/filters/svf.pqp` and `onepole.pqp`).
- **Calls** go to a leaf atom (SPEC §7.2), a patch in the same file, or a patch from an `import`ed file. Calling a patch instantiates it as a subcircuit. Named arguments bind its `in`s and `param`s; anything not bound uses its default.
- **Multi-output nodes** return a record, accessed with `.name` (e.g. `svf(x, cutoff: c, q: r).lp`).
- **Free-running sources** (T1.3-fix1, QUESTIONS Q29): a stateful atom whose inputs are all global and at most param rate — `noise()`, `pulse()`, `phasor(0.5Hz)`, `sine_hz(rate)`, an LFO subcircuit with a rate knob — is **per voice** when its patch runs in a voice context and its value feeds voice processing (reaches a per-voice node or a `… voice` output): one instance per voice, so noise is decorrelated between voices (seeded per lane) and an LFO's phase starts at its voice's note-on (a per-voice `phasor`/`sine_hz` made this way is reset by `voice.trigger`, at the onset sample). A source that only feeds global processing (a global effect, a `… global` output) stays global. To keep **one shared instance** for all voices, mark the expression that computes it: `lfo = shared(sine(phasor(0.5Hz)))`, `n = shared(noise())`. Every free-running source that reaches voice processing only through a `shared(…)` node stays global (so the whole `sine(phasor(…))` cone is one instance); the result is bit for bit what the pre-T1.3-fix1 default (`InferOptions::freeRunningPoly = Global`) gave. `shared` is not a node: it marks the node of its argument (an atom or operator expression; not a name, a literal or a subcircuit call), so adding it changes no state key or noise seed, and the printer writes it back as `shared(…)`. `shared` is reserved as a call name. Delay lines are never made per-voice this way.
- **Inference**: each port's declared `rate`/`poly` is an *upper bound*. The compiler infers the cheapest rate and poly that are correct (SPEC §7.1, §7.4). Mixing kinds requires explicit conversion, e.g. `gate_to_real(b)` or `x > 0`. There is no implicit bool↔real conversion. A subcircuit's `param`s accept any rate and poly (wiring a modulator into one makes that instance's path audio-rate or per-voice); its `in`s are bounded by their declaration. A per-voice signal into a `global` port is summed over the voices (on bools: any voice); to choose another reduction, call one explicitly: `voice_sum`, `voice_last`, `voice_highest`, `voice_lowest`, `voice_loudest` (QUESTIONS Q28). The rules in detail: `pq/graph/infer.h`.
- **Width**: binary operators broadcast mono over stereo. Two stereo operands combine channel-wise. `[i]` selects a channel.
- **Constants**: unit-normalised literals, constant arithmetic and `const` params are folded at compile time. `param`s are **never** folded, so knob changes never recompile.
- **Durations** (delay `time` and `max`, timer `dur`, pulse `period`) are in seconds. A literal written in `smp` directly at a duration input means that many samples (`delay(x, 1smp, 10ms)` is a one-sample delay); once it goes through arithmetic it is a plain number, and such a value reaching a duration input is an error (`delay(x, 5smp * 2, …)` would otherwise mean 10 s). A `param` default or range cannot be in `smp` (QUESTIONS Q35).
- **Built-in namespaces** (read-only inputs, available in any patch that is instantiated in a voice or transport context):
  - `voice.hz`, `voice.cents`, `voice.gate`, `voice.trigger`, `voice.velocity`, `voice.release`, `voice.ratio`, `voice.harmonic`, `voice.unisonCount`, `voice.index`, `voice.degree`, `voice.equave` (T2.6: a Degree note's degree and equave in its tuning signal — for a Ratio note anchored to a Degree note, its root's —, NaN for other notes), `voice.tuning` (T2.6-fix3: which of the track patch's tuning nodes the note follows, 1, 2, …; 0 for a static pitch), `voice.noteHz` (T4.17: the note's Hz at its note-on, param rate — glides, pitch events and retunes do not move it, and `tune_voices` does not rebind it; an Instrument Rack's Hz key zones read it, so a zone is decided at note-on), `voice.noteDegree` (T4.17, M4 review M7: the note's absolute degree, equave · size + degree, in its own tuning — a clip's tuning included — at its note-on, param rate; between two degrees, fractional in log frequency, for a Ratio / Cents pitch; NaN when unknown; an Instrument Rack's Degree key zones read it)
  - `transport.bpm`, `transport.beat`, `transport.barPhase`, `transport.playing`
  - `sr` (sample rate as a `const` after specialisation; n·sr inside an `oversample(n)` region, §2.3)
- **Tuning as a signal** (T2.6, SPEC §3.3): `tuning_hz(degree, equave, hz, cents, generator, period, stretch, reference, table: k)` is the pitch of a degree in tuning table `k` (an EDO or MOS; `period` is an EDO's equave) at modulatable generator / period / stretch / reference, with outputs `.hz` and `.cents`; lanes without a degree pass `hz` / `cents` through. T2.6-fix3 adds `select:` and `node:` (both default 0): with `node: i` ≥ 1 the node takes only the lanes whose `select` (normally `voice.tuning`) is i, and a lane of it without a degree follows the reference (`hz · reference / reference₀`: a Ratio on the reference or a Cents note); a Degree lane's `hz` over its static degree pitch (a note's ratio to its root Degree note, a glide) multiplies the computed pitch, and `.cents` is the input `cents` moved by what the tuning did (so it includes the note's own ratio and glides). A track patch chains one node per tuning signal its notes follow (`tun2` takes `tun1.hz` / `tun1.cents` as its fallback). `tv = tune_voices(t.hz, t.cents)` (no outputs; `cents` optional) makes `voice.hz` (and `voice.cents`) read those values in every subcircuit the patch instantiates, recursively; the patch's own `voice.hz` stays the engine's (that is the fallback input), and an inner `tune_voices` wins over an outer one. One per patch.
- **Smoothed gains** (T3.17-fix2, the mixer): `gain(x, g, time: 2ms)` is `x · g` with `g` smoothed by a one-pole of time constant `time` when it changes (a param gain is applied at block rate and smoothed per sample only while it moves; settled, it costs one multiply per sample); `gain_add(acc, x, g, time: 2ms)` is `acc + x · g` the same way (settled at 0 it is `acc`: a chain of them sums sends that cost nothing while closed). A new instance fades in from 0, unless its `start` input (a const, default 0) is 1: then its first block starts at the target (T4.x-fix-dsp: the stock devices' gain-like params, so a device is at its knob values from the first sample and a jump of a knob or an automation step glides over 2 ms instead of clicking); `gain(x, c, start: 1)` with a literal `c` compiles to `x · c` (a mix coefficient a structural constant fixes costs nothing extra). An audio-rate `g` is used as it is. `time` is a duration (seconds, `smp` literals at the input).
- **Exports** become wire sources visible outside the device. Anything can be exported: an oscillator output, an envelope, a bool.
- **Silence and sleep** (T1.x-silence, SPEC §7.7): nothing to write. Every leaf atom declares "silent in → silent out after tail T" (`dsp::silenceInfo` / `dsp::silenceTail`, checked against its kernel by `tests/dsp/test_silence.cpp`), and every subcircuit — every stock filter, delay, reverb and device — gets its declaration from its atoms, so a user patch sleeps exactly like a stock one:
  - stateless atoms: `x · y`, `and` are silent when either input is; `+ − min max lerp(a, b, t) select(c, a, b) mux or xor > < ≠` when all their signal inputs are; `neg abs sign floor frac sqrt sin tan tanh sine(phase) ÷ (the dividend) gate_to_real demux` when their signal input is; `clip(x, lo, hi)` when `lo ≤ 0 ≤ hi` are constants; `muladd(a, b, c)` when (`a` or `b`) and `c` are. Everything else (generators, `exp`, `cos`-like offsets, `db_to_gain`, `≥ ≤ ==`, `not`, tables, held values: sample&hold, latch, flip-flops, counters, schmitt) promises nothing, so a region holding it stays awake;
  - memory atoms: `z1`, `rise` / `fall` / `change`: silent one sample after their input is; a delay line `max` + 3 samples after (its oldest tap). The tail of a **feedback** structure (a comb, an allpass diffuser, a reverb, an IIR filter) is not a fixed number: its loop carries the signal on, so the host measures it — the region sleeps once what it has written into its memory over the last tail has stayed ≤ −180 dBFS (2^-30) and its input is silent, and that residue is then set to 0 (the tail is cut; what the residue would still have produced stays ≤ −120 dBFS through up to 60 dB of resonance or feedback gain);
  - smoothed gains (`gain`, `gain_add`) sleep once settled (exactly at their target) and wake when their target or time param changes; a `z1` of a param-derived value (a de-zippered coefficient) counts as equal to it once the params have been unchanged for a block, so `x − z1(x)` is silent then (Utility's smoothing form).
  A region asleep costs one compare per block and writes zeros into the buffers others read from it (the constant-output kernel); it wakes for the whole block in which its input is no longer silent, so the first sample after silence is exact. A patch whose voices hold delay lines (an echo inside the voice) declares that tail too: the voice keeps sounding that long after its envelope's `done` (pq/engine/voices.h).

## 3.1 The stock library (`stdlib/`, T1b.3)
Stock subcircuits are ordinary patches in this language; a patch imports the file it calls (paths relative to the importing file: `import "filters/svf.pqp"` from the stdlib root, `import "../stdlib/filters/svf.pqp"` from a sibling directory). Every one of them has a hand-written C++ reference in `tests/stdlib/stdlib_reference.h` that the compiled subcircuit matches within 1e-5 on both backends.

| File | Patches | Outputs (first = the call's value) |
|---|---|---|
| `filters/onepole.pqp` | `onepole(x, cutoff)`: TPT one-pole | `lp`, `hp`, `allpass` |
| `filters/svf.pqp` | `svf(x, cutoff, q, gain)`: TPT state-variable filter; `svf_core(x, g, k)`: its loop | `lp`, `bp`, `hp`, `notch`, `peak`, `allpass`, `bell`, `lowshelf`, `highshelf` (`gain` is for the last three; unread outputs cost nothing) |
| `filters/biquad.pqp` | `biquad(x, b0, b1, b2, a1, a2)` (TDF-II); RBJ designers `biquad_lowpass`, `_highpass`, `_bandpass`, `_notch`, `_allpass` (`cutoff`, `q`), `_peak`, `_lowshelf`, `_highshelf` (+ `gain`) | `out` |
| `filters/allpass.pqp` | `allpass(x, cutoff)`: first order; `allpass_delay(x, maxTime, time, g)`: Schroeder | `out` |
| `filters/dcblock.pqp` | `dcblock(x, cutoff)` | `out` |
| `filters/comb.pqp` | `comb(x, maxTime, time, fb)`; `comb_lp(x, maxTime, time, fb, damp)`: damped (linear-phase, stays in tune) | `out` |
| `envelopes/adsr.pqp` | `adsr(gate, a, d, s, r)` | `out`, `done` |
| `control/lfo.pqp` | `lfo(reset, rate, sync, beats, phase)` | `sine`, `tri`, `saw`, `square` (export `cycle`) |
| `control/slew.pqp`, `smooth.pqp` | `slew(x, rise, fall)`, `smooth(x, time)` | `out` |
| `dynamics/envfollow.pqp` | `envfollow(x, attack, release, rms)` | `out` |
| `filters/svf_fast.pqp` (T4.9) | `svf_core_fast(x, g, k)`: svf_core with the state update two multiply-adds deep (same filter; the devices use it) | `band`, `low` |
| `util/oversample.pqp` (T4.9) | polyphase IIR half-band stages: `os_up2(x)` / `os_down2(s0, s1)` (48 ↔ 96 kHz), `os_up2_4` / `os_down4_2`, `os_up4_8` / `os_down8_4` (later stages take the samples of the stage before, in time order); `hb_ap1/2/4`: one allpass over 1, 2, 4 samples per tick | `s0, s1` / `o0 …` / `y` |
| `devices/utility.pqp` (T4.9) | `utility(x)`: Live's Utility (stereo); constants `bassMono`, `audition`, `dc` | `out` (exports `mid`, `side`, `bass`) |
| `devices/saturator.pqp` (T4.9) | `saturator(x)`: Live's Saturator (stereo); constants `type` 0–8, `hq` 1/2/4/8, `color`, `softClip`; `sat_shape`: one curve sample | `out` (exports `driven`, `wet`) |
| `devices/eq_eight.pqp` (T4.10) | `eq_eight(x)`: Live's EQ Eight (stereo); constants `mode` (Stereo, L/R, M/S), `hq` 1/2, `typeN` / `onN` (and the B set) | `out` (exports `a`, `b`, `band1` … `band8`) |
| `envelopes/msenv.pqp` (T4.12) | `msenv(gate, mode, soft, a, d, r, init, peak, sus, end, rep)`: multi-stage envelope, Initial → Peak (linear) → Sustain → End (exponential, −80 dB per stage time); constant `mode` 0 None, 1 Loop, 2 Beat, 3 Sync, 4 Trigger (Live Operator's loop modes); `soft` 1: a retrigger starts from the current level when it is above Initial | `out`, `done` |
| `devices/operator.pqp` (T4.12) | `operator()`: Live's Operator, an instrument (no audio input; `voice.hz` from the tuning signal); constants `algorithm` 1–11, `onA` … `onD`, `waveA` … (0–22), `loopA` …, `pitchEnvOn`, `pitchLoop`, `lfoOn`, `lfoWave`, `lfoRetrig`, `filterOn`, `filterType`, `filterSlope`, `filterLoop`, `glideOn`, `spreadOn`, `voices`; helpers `op_bank`, `op_wave`, `op_partials`, `op_filter`, `lin` | `out` (voice stereo), `done` (exports `opA` … `opD`, `lfo`, `pitchEnv`, `filterEnv`) |
| `devices/auto_filter.pqp` (T4.11) | `auto_filter(x, sc)`: Live's Auto Filter (stereo; `sc` the sidechain input, silence by default); constants `type` 0–9, `circuit` (Clean, OSR, MS2, SMP, PRD), `slope` 12/24 dB, `lfo`, `lfoShape`, `env`, `scOn`; nonlinear models with `solve` | `out` (exports `cutoff`, `lfoSignal`, `envelope`) |
| `devices/lfo_device.pqp` (T4.7) | `lfo_device(x)`: a modulation source (the LFO device): passes the audio through; constant `shape` (Sine, Triangle, Saw Up, Saw Down, Square), params `rate`, `sync`, `beats`, `phase`, `depth`, `offset` | `out` (export `lfo` = offset + depth · wave) |

Devices (`devices/`, T4.9/T4.10) take and return `audio stereo` (an instrument, T4.12 Operator, takes no audio and returns `audio voice stereo` plus `done`); their structural settings (curve type, oversampling factor, band types, modes) are `in … : real const` inputs set by literal arguments at the call site, so only the chosen path is compiled (a change recompiles, SPEC §7.5); every knob is a `param` (any rate). The choices behind each device are in QUESTIONS Q121 / Q123 / Q131 / Q133. Auto Filter also takes a second audio input, `sc` (its sidechain, silence when unconnected). Idiom (Q132): a structural select whose unused branch is a literal of another poly does not fold (`on ? perVoice : 0` keeps `perVoice`), so devices switch per-voice parts off by choosing between two per-voice values (`acc1 = on ? acc0 + x : acc0`). Built-in waveforms (T4.12): `wavetable(p, table: 65536 + k)` reads `dsp::builtinWavetable(k)` (band-limited Saw / Square 3–64 partials, Triangle, Noise Loop; `pq/dsp/tables.h`) whatever the program's resources.

Conventions: cutoffs are in Hz and kept below 0.49·sr; `q` is the quality factor (0.7071 = Butterworth); `gain` is a dB param (so linear inside); times are seconds: `adsr` stage times are full-scale times (attack 0 → 1 linear; decay/release exponential, −80 dB after the stage time), `envfollow`/`smooth` times are time constants (63 %), `slew` times are per unit of change. Delay-line subcircuits take their buffer size as a constant `in maxTime : real const` (the second positional argument) and keep the delay ≥ 0.1 ms, so a feedback loop through them runs in sub-blocks whenever the delay has a known lower bound.

## 3.2 Meta modules (TMM.3; docs/plans/MM.md, SPEC §15 2026-11-01)
A **meta module** is a subcircuit with its own interactive face whose state lives in the project (`pq/meta/type.h`, `pq/document/model.h` "Meta modules"). In text it is an ordinary call of a reserved name, **`meta_<type>(…)`**; no grammar is added.
- **Names.** The prefix `meta_` is reserved for meta modules: a patch should not be named with it. The compiler's loaders (`pqc`, the device library, the circuit editor) supply every registered type's default expansion as a prelude patch `meta_<type>`, so a call checks and renders anywhere; a standalone file plays the **default state**.
- **Bind it to a name**: `acc1 = meta_phase_acc(hz: voice.hz * 3/2)`. The name path — the call binding names from the device's root patch, joined by `.` through named helper calls (`opA.acc`) — is the key of the instance's state in the project, so renaming the binding in the editor carries the state along (`ops::renameMetaCall`, or a hand edit through `ParseOptions::previous`). An unnamed call always plays the default state.
- **Arguments are wires.** The call's inputs are the type's inputs, then its params (as for any subcircuit). An unwired input reads its default (e.g. `hz` = `voice.hz` in a voice context, else 0; a modulation input 0). An unwired param takes the instance's value from the project as a runtime param (`dev<id>:<path>.<name>`): a face drag never recompiles. A wired param (a signal or a literal: a literal is a constant wire, shown fixed on the face) is that signal. **Settings** (structural choices) and **data** (tables, drawings) are never arguments: they live in the state only, settings changing the expansion, data never (an edit of it never recompiles).
- **Expansion.** In a project, each call is replaced by the instance's own expansion `meta_<type>__d<device>_<path>_<key>` (and the patches on the way to it by renamed copies), built deterministically from atoms; opened in the circuit view it shows those atoms, read-only (TMM.4). A chain-level meta device (Device view: a device whose patch is `meta:<type>`) is the same expansion as the device itself (an instrument form with no audio input whose voices end at note-off, or an audio-effect form).
- **Resources** (TMM.2). An expansion reads its live tables and writes its face's dots through two atoms whose last argument is a resource slot id (a constant the lowering allocates per instance and request, `LowerOptions::metaSlotOf`; 0 outside a project: the engine's defaults): `wavetable_pos(phase, position, table)` reads the slot's frame table (`pq/dsp/frame_table.h`; position 0 – 1, default 0) and crossfades over 64 samples when a new table is published into the slot — data edits never recompile; `voice_tap(x, slot)` is a sink (no output) that samples `x` per voice for the face, and costs one load per block while no face shows it. Both are ordinary atoms (usable by hand: `t = voice_tap(ph, 2)`); `voice_tap` is not a voice reducer despite its prefix.
- **Face laws** appear as param attributes of the expansion: `@curve(exp0, k)` is exponential from exactly the range's low end — value = lo + (hi − lo)·(2^(k·t) − 1)/(2^k − 1) for knob travel t — the Phase accumulator's frequency (k = 14); `doc::paramTravel` reads it (automation, wires and macros on a chain device move along it).

| Type | Call | Inputs | Params | Output |
|---|---|---|---|---|
| Phase accumulator (`phase_acc`, `pq/meta/phase_acc.h`) | `meta_phase_acc(hz, pm, freq, phase, amp)` | `hz` (default `voice.hz`, 0 outside a voice context), `pm` (added to the phase, default 0) | `freq` 0 – 20 kHz `@curve(exp0, 14)` (added to `hz`: 0 plays the note exactly), `phase` 0 – 1 (the start phase: the per-voice phasor restarts at note-on), `amp` 0 – 4 (multiplies the output) | `out` = amp · frac(phasor(hz + freq) + phase + pm) (TMM.5: and `tap = voice_tap(ph, <slot>)` of that phase, the face's dots) |
| Wavetable (`wavetable`, `pq/meta/wavetable.h`; TMM.5) | `meta_wavetable(x, pos, phase, position, fold, warp, sync)` | `x` the phase read (default: its own `phasor(voice.hz)` in a voice context — a lone Wavetable is an oscillator —, else 0), `pos` (added to the position, default 0) | `phase` 0 – 1 (wraps; added to `x`), `position` 0 – 1 (between the keyframes), `fold` 0 – 1, `warp` −1 – 1, `sync` 0 – 1 (fold / warp / sync are **baked** into the table off-thread: a change rebuilds the table, never a compile; until the live path they cannot be wired, automated or macro-mapped: a wired one is refused with a reason). Settings `mode` (0 Phase; 1 Shape: not yet), `warpMode` (0 Bend, 1 Mirror, 2 Pulse width). Data: 1 – 256 keyframes (position + a 2048-sample cycle), a gap mode each (spectral / crossfade), the dense frame count (64), removeDc, normalise | `out` = wavetable_pos(frac(x + phase), clip(position + pos, 0, 1), <its table slot>) |

## 4. Example

```
// stdlib/examples/fm_voice.pqp
patch FmVoice {
  param ratio = 2.0 [0.25..16]        @label("Ratio")
  param index = 1.0 [0..20]           @label("Index")
  param attack  = 5ms   [0ms..10s]    @curve(exp)
  param release = 300ms [1ms..30s]    @curve(exp)
  out   out : audio voice mono

  mod = sine(phasor(voice.hz * ratio))
  car = sine(phasor(voice.hz + mod * index * voice.hz))
  env = adsr(voice.gate, a: attack, d: 200ms, s: 0.7, r: release)
  out = car * env

  export modulator = mod              // wire this to any parameter anywhere
  export envelope  = env

  layout {
    node mod at (40, 60)
    node car at (200, 60)
    node env at (200, 160)
  }
}
```

## 5. Open items for later versions
- Arrays/generics (`n`-voice unison, `for` expansion at compile time).
- Tables as first-class values (wavetables, shaper curves), loaded from the drawable editors.
- Per-sample loops beyond `solve` (avoid unless profiling justifies them).
