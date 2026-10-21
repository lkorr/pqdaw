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
layoutItem  = "node" Ident "at" "(" Number "," Number ")" [ "rot" Int ] [ "flip" ]
            | "wire" Ident "." Ident "->" Ident "." Ident [ "via" "(" pointList ")" ]
            | "comment" string "at" "(" Number "," Number ")" ;
```

## 3. Semantics
- **Bindings are a dataflow graph, not statements.** Order doesn't matter, and a name may be used before it is bound. Each binding is one node (or a subgraph of nodes).
- **Cycles**:
  - A cycle that goes through `z1(…)`, `delay(…)` or a stateful atom is ordinary feedback.
  - A cycle with no delay is an **error** in text, unless it is inside `solve`. The visual editor inserts an explicit `z1` and shows a warning instead.
  - Zero-delay-feedback filters write their solved form explicitly (see `stdlib/filters/svf.pqp` once it exists).
- **Calls** go to a leaf atom (SPEC §7.2), a patch in the same file, or a patch from an `import`ed file. Calling a patch instantiates it as a subcircuit. Named arguments bind its `in`s and `param`s; anything not bound uses its default.
- **Multi-output nodes** return a record, accessed with `.name` (e.g. `svf(x, cutoff: c, res: r).lp`).
- **Inference**: each port's declared `rate`/`poly` is an *upper bound*. The compiler infers the cheapest rate and poly that are correct (SPEC §7.1, §7.4). Mixing kinds requires explicit conversion, e.g. `gate_to_real(b)` or `x > 0`. There is no implicit bool↔real conversion.
- **Width**: binary operators broadcast mono over stereo. Two stereo operands combine channel-wise. `[i]` selects a channel.
- **Constants**: unit-normalised literals, constant arithmetic and `const` params are folded at compile time. `param`s are **never** folded, so knob changes never recompile.
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
