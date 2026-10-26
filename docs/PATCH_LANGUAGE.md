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

member      = param | input | output | export | binding | solve ;
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

`unknown` (in solve blocks) and the layout words `node at rot flip wire via comment` are contextual: they are only special in those positions. So are the port-type words (`real bool audio const global voice mono stereo ch`).

### 2.1 Clarifications (v0, implemented by the T1.2 parser)
Decided while implementing the parser; the open ones are tracked in [QUESTIONS.md](QUESTIONS.md) (Q7–Q10, Q18–Q22).

- **Line breaks** are insignificant except in three places: a port type ends at the end of its line (so `in x : real` followed by a line `mono = 1` or `param y = 2` is not read as one type, Q21); inside a `solve` block each `unknown` or equation is one line, unless it is inside brackets (an equation may start with `(`, `-` or `!`, Q21); and an `@attr(` must open its arguments on the attribute's line.
- **Numbers**: `%` directly after a number is the percent unit (`50%`); with a space it is the modulo operator (`a % 2`). A unit suffix is any identifier glued to a number, and must be one of the table above. `-` followed directly by a number literal is a negative literal (`-3dB` is the literal −3 dB); `-(3)` is the negation of a literal. Only one prefix operator is allowed per operand (`-(-x)`, not `--x`). Comparisons do not chain (`a < b < c` is an error).
- **Params**: the default and the range ends are constant expressions, folded by the parser (QUESTIONS Q10): a plain literal keeps its unit, arithmetic gives the normalised number. Without `: kind`, the kind follows the default (`true`/`false` → bool). A `[` after the default always starts the range. Attribute arguments are literals: a number (with unit, optionally negative), `true`/`false`, a string, or a word; `@hidden` and `@hidden()` are the same.
- **Names**: one namespace per patch for params, ins, outs, exports, unknowns and bindings. `in`/`out` can be names (Q7). The built-in names `voice`, `transport`, `track`, `sidechain` and `sr` cannot be redefined. A plain name that is not bound in the patch may name a built-in atom without inputs (`sr`); others are called (`pulse()`). `voice.hz`, `transport.bpm`, `track.in` etc. are always the built-ins.
- **Bindings**: `y = x` (a name, optionally with `.output`) is an alias with no node of its own; any other right-hand side is a node, and the binding names it. `out = expr` drives the output `out`; an `out` or an `export` cannot be read as a value (bind the value to a name and use that). `node.input = expr` (the `Ident "." Ident` target) connects `expr` to an unconnected input of a named computed node, e.g. `h = samplehold(x)` then `h.trig = rise(g)`; a call's required inputs may be supplied this way (Q20).
- **Calls** resolve to a patch of the same file, then an external patch (imported, or a prelude given by the compiler), then a leaf atom; a patch may not have the name of an atom (Q18). Positional arguments come first and fill inputs in order (a subcircuit's inputs are its `in`s, then its `param`s); named arguments name an input. A call used as a value gives its first output; `.name` selects another. Repeated ports of variadic atoms are numbered in text: `mux(i, a, b, c)` has inputs `index, in0, in1, in2`; `demux(i, x)` has outputs `out0, out1` (v0 cannot express more, Q19).
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
  - `voice.hz`, `voice.cents`, `voice.gate`, `voice.trigger`, `voice.velocity`, `voice.release`, `voice.ratio`, `voice.harmonic`, `voice.unisonCount`, `voice.index`, `voice.degree`, `voice.equave` (T2.6: a Degree note's degree and equave in its tuning signal — for a Ratio note anchored to a Degree note, its root's —, NaN for other notes), `voice.tuning` (T2.6-fix3: which of the track patch's tuning nodes the note follows, 1, 2, …; 0 for a static pitch)
  - `transport.bpm`, `transport.beat`, `transport.barPhase`, `transport.playing`
  - `sr` (sample rate as a `const` after specialisation)
- **Tuning as a signal** (T2.6, SPEC §3.3): `tuning_hz(degree, equave, hz, cents, generator, period, stretch, reference, table: k)` is the pitch of a degree in tuning table `k` (an EDO or MOS; `period` is an EDO's equave) at modulatable generator / period / stretch / reference, with outputs `.hz` and `.cents`; lanes without a degree pass `hz` / `cents` through. T2.6-fix3 adds `select:` and `node:` (both default 0): with `node: i` ≥ 1 the node takes only the lanes whose `select` (normally `voice.tuning`) is i, and a lane of it without a degree follows the reference (`hz · reference / reference₀`: a Ratio on the reference or a Cents note); a Degree lane's `hz` over its static degree pitch (a note's ratio to its root Degree note, a glide) multiplies the computed pitch, and `.cents` is the input `cents` moved by what the tuning did (so it includes the note's own ratio and glides). A track patch chains one node per tuning signal its notes follow (`tun2` takes `tun1.hz` / `tun1.cents` as its fallback). `tv = tune_voices(t.hz, t.cents)` (no outputs; `cents` optional) makes `voice.hz` (and `voice.cents`) read those values in every subcircuit the patch instantiates, recursively; the patch's own `voice.hz` stays the engine's (that is the fallback input), and an inner `tune_voices` wins over an outer one. One per patch.
- **Smoothed gains** (T3.17-fix2, the mixer): `gain(x, g, time: 2ms)` is `x · g` with `g` smoothed by a one-pole of time constant `time` when it changes (a param gain is applied at block rate and smoothed per sample only while it moves; settled, it costs one multiply per sample); `gain_add(acc, x, g, time: 2ms)` is `acc + x · g` the same way (settled at 0 it is `acc`: a chain of them sums sends that cost nothing while closed). A new instance fades in from 0; an audio-rate `g` is used as it is. `time` is a duration (seconds, `smp` literals at the input).
- **Exports** become wire sources visible outside the device. Anything can be exported: an oscillator output, an envelope, a bool.

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

Devices (`devices/`, T4.9/T4.10) take and return `audio stereo`; their structural settings (curve type, oversampling factor, band types, modes) are `in … : real const` inputs set by literal arguments at the call site, so only the chosen path is compiled (a change recompiles, SPEC §7.5); every knob is a `param` (any rate). The choices behind each device are in QUESTIONS Q121 / Q123.

Conventions: cutoffs are in Hz and kept below 0.49·sr; `q` is the quality factor (0.7071 = Butterworth); `gain` is a dB param (so linear inside); times are seconds: `adsr` stage times are full-scale times (attack 0 → 1 linear; decay/release exponential, −80 dB after the stage time), `envfollow`/`smooth` times are time constants (63 %), `slew` times are per unit of change. Delay-line subcircuits take their buffer size as a constant `in maxTime : real const` (the second positional argument) and keep the delay ≥ 0.1 ms, so a feedback loop through them runs in sub-blocks whenever the delay has a known lower bound.

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
