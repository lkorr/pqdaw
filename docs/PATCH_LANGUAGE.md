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
export      = "export" Ident "=" expr ;                   (* a device jack on top of the device *)
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
layoutItem  = "node" Ident "at" point [ "rot" Int ] [ "flip" ]
            | "wire" Ident "." Ident "->" Ident "." Ident [ "via" "(" pointList ")" ]
            | "comment" string "at" point ;
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
- **Printing**: `print` writes a canonical form (declarations, bindings, outputs, exports, solve blocks, layout), inlining every unnamed node that has exactly one consumer. `parse(print(f)) == f` for every parsed file, and `print` is stable under a second round trip.

## 3. Semantics
- **Bindings are a dataflow graph, not statements.** Order doesn't matter, and a name may be used before it is bound. Each binding is one node (or a subgraph of nodes).
- **Cycles**:
  - A cycle that goes through `z1(…)`, `delay(…)` or a stateful atom is ordinary feedback.
  - A cycle with no delay is an **error** in text, unless it is inside `solve`. The visual editor inserts an explicit `z1` and shows a warning instead.
  - Zero-delay-feedback filters write their solved form explicitly (see `stdlib/filters/svf.pqp` once it exists).
- **Calls** go to a leaf atom (SPEC §7.2), a patch in the same file, or a patch from an `import`ed file. Calling a patch instantiates it as a subcircuit. Named arguments bind its `in`s and `param`s; anything not bound uses its default.
- **Multi-output nodes** return a record, accessed with `.name` (e.g. `svf(x, cutoff: c, res: r).lp`).
- **Inference**: each port's declared `rate`/`poly` is an *upper bound*. The compiler infers the cheapest rate and poly that are correct (SPEC §7.1, §7.4). Mixing kinds requires explicit conversion, e.g. `gate_to_real(b)` or `x > 0`. There is no implicit bool↔real conversion. A subcircuit's `param`s accept any rate and poly (wiring a modulator into one makes that instance's path audio-rate or per-voice); its `in`s are bounded by their declaration. A per-voice signal into a `global` port is summed over the voices (on bools: any voice); to choose another reduction, call one explicitly: `voice_sum`, `voice_last`, `voice_highest`, `voice_lowest`, `voice_loudest` (QUESTIONS Q28). The rules in detail: `pq/graph/infer.h`.
- **Width**: binary operators broadcast mono over stereo. Two stereo operands combine channel-wise. `[i]` selects a channel.
- **Constants**: unit-normalised literals, constant arithmetic and `const` params are folded at compile time. `param`s are **never** folded, so knob changes never recompile.
- **Durations** (delay `time` and `max`, timer `dur`, pulse `period`) are in seconds. A literal written in `smp` directly at a duration input means that many samples (`delay(x, 1smp, 10ms)` is a one-sample delay); once it goes through arithmetic it is a plain number (QUESTIONS Q35).
- **Built-in namespaces** (read-only inputs, available in any patch that is instantiated in a voice or transport context):
  - `voice.hz`, `voice.cents`, `voice.gate`, `voice.trigger`, `voice.velocity`, `voice.release`, `voice.ratio`, `voice.harmonic`, `voice.unisonCount`, `voice.index`
  - `transport.bpm`, `transport.beat`, `transport.barPhase`, `transport.playing`
  - `sr` (sample rate as a `const` after specialisation)
- **Exports** become wire sources visible outside the device. Anything can be exported: an oscillator output, an envelope, a bool.

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
