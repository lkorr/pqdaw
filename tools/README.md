# tools

| What | Where | Built by CMake |
|---|---|---|
| `pqc`: the patch compiler CLI (T1.10) | `tools/pqc/` (`pq_pqc` library + `pqc.exe`) | yes |
| Benchmark CSV → `bench/RESULTS.md` rows (T1.11) | `tools/bench/append_results.py` | no (Python 3) |
| Fixture generators from the JS references | `tools/fixtures/` | no (Node) |
| Fast-math coefficient fits | `tools/fit/` | no (Python) |
| Theme generator | `tools/theme/` | no (Python) |

## pqc

`build/<preset>/tools/pqc/pqc.exe`. The library `pq_pqc` (`tools/pqc/include/pq/pqc/pqc.h`) holds everything but
`main`, so `tests/tools/test_pqc.cpp` runs exactly what the executable runs.

```
pqc check         <file.pqp> [--patch NAME] [--rate HZ] [--visual]
pqc dump-ir       <file.pqp> [--patch NAME] [--rate HZ] [--stage parsed|inferred|optimised] [-o out.txt]
pqc dump-schedule <file.pqp> [--patch NAME] [--rate HZ] [--visual] [--keys] [-o out.txt]
pqc render        <file.pqp> [--patch NAME] [--notes "…"] [--seconds S] [--rate HZ] [--block N]
                  [--backend interp|jit] [--voices N] [--param NAME=VALUE]… [--gain DB] -o out.wav
pqc help
```

The pipeline is the compiler's (SPEC §7.4): the file is read with its imports (paths relative to the importing file,
recursively), parsed, inferred, optimised for `--rate` (default 48000), scheduled, built into a ProgramImage by the
interpreter or the LLVM JIT, and rendered offline.

- **check**: parse + infer + optimise + schedule every patch of the file (or `--patch`). Prints one `ok` line per
  patch (nodes before/after optimisation, ops, stages, block buffers, params, state bytes) on stdout; every
  diagnostic of the failing phase on stderr as `file:line:col: error: …` with the source line and a caret.
  `--visual`: a delay-free cycle gets a z1 and a `warning:` instead of an error (the visual editor's mode).
- **dump-ir**: the patch IR as patch-language text (`graph::print`). `--stage parsed` is the patch as written;
  `inferred` the flattened patch (subcircuits instantiated, reducers inserted); `optimised` (default) after constant
  folding, CSE, DCE, duration lowering and muladd fusion. The output always parses.
- **dump-schedule**: `graph::dump` of the schedule (stages, regions, ops with operands, solve blocks, buffers,
  externals, params, outputs, state layout); `--keys` adds the hot-swap state keys.
- **render**: an offline render (`engine::renderOffline`) to a 32-bit float WAV, one channel per global `out`
  (a mono patch gives a mono file), or for an instrument (voice `out`s only) one per voice mix. Deterministic: a fixed
  engine block (`--block`, default 64), seeded noise, no wall clock; the same command always writes the same bytes.
  Root `in`s read silence. Patches with voice stages play `--notes` through the VoiceEngine (polyphony `--voices`,
  default 16). `--param NAME=VALUE` sets a param in normalised units (seconds, Hz, linear), `--gain DB` scales the
  output (no clipping: golden files keep the raw signal). Prints the render speed (× realtime) and voice counts.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | the patch has errors (parse, inference, optimisation, scheduling); diagnostics on stderr |
| 2 | usage error: unknown command or option, bad value, unknown `--patch` / `--param`, `--notes` on a patch without voices |
| 3 | I/O error: the file (or an output) cannot be read or written |
| 4 | backend error: the image could not be built or rendered (JIT failure, a patch with nothing to play) |

### Notes (`--notes`)

```
notes    = note { sep note }              sep: spaces, tabs or commas
note     = pitch [ "@" time ] [ ":" time ] [ "*" velocity ]
pitch    = number                         12-TET key number: 69 = A4 = 440 Hz, 60 = C4; fractions allowed (60.5)
         | number ( "hz" | "Hz" )         a frequency
time     = number [ "s" | "ms" ]          seconds by default
velocity = number in [0, 1]               default 0.7
```

- A **plain pitch list** (`"60 64 67 72"`, `"440hz 550hz"`) is the `pqdaw --play` syntax and plays the same
  arpeggio: note k starts at k · 0.25 s, lasts 0.125 s, velocity 0.7, pitch k mod n, repeated to the end of the
  render. Default: `"60 64 67 72"`.
- With **any timing**, the list is played once: `@start` (seconds from 0), `:length` (default 0.125 s), `*velocity`.
  A note without `@` starts 0.25 s after the previous note's start (the first at 0). Chords: equal starts,
  `"60@0:1 64@0:1 67@0:1"`. Example: `"60@0:1 64@500ms:0.25*0.9 67 440hz@1.5:2"`.
- Pitches must resolve to 1 Hz … 20 kHz. At equal times note-offs come before note-ons.

### Golden renders (T1.10)

Committed under `tests/golden/audio/` (32-bit float WAV, 48 kHz, < 1 MB each), rendered from the self-contained
patches in `tests/golden/patches/`. `pq_tests_render` renders each with **both** backends and compares with
`tests/golden/golden_audio.h` (absolute 1e-4 and spectral 0.2 dB per semitone band, floor −80 dB; both must pass),
and checks that mutated patches, notes and simulated engine regressions fail.

| Golden | Command |
|---|---|
| `fm_voice.wav` (mono, 1 s) | `pqc render tests/golden/patches/fm_voice.pqp --seconds 1 --notes "60@0:0.4 64@0.1:0.4 67@0.2:0.5*0.9 72@0.45:0.35*0.6 55@0.6:0.3" -o fm_voice.wav` |
| `fm_voice_arp.wav` (mono, 1 s) | `pqc render tests/golden/patches/fm_voice.pqp --seconds 1 --notes "60 64 67 72" -o fm_voice_arp.wav` |
| `pluck.wav` (mono, 1 s) | `pqc render tests/golden/patches/pluck.pqp --seconds 1 --notes "48@0:0.5 55@0.15:0.5*0.8 64@0.3:0.4*0.9 67@0.5:0.4" -o pluck.wav` |
| `drive_zdf.wav` (mono, 0.5 s) | `pqc render tests/golden/patches/drive_zdf.pqp --seconds 0.5 -o drive_zdf.wav` |
| `stereo_echo.wav` (stereo, 1 s) | `pqc render tests/golden/patches/stereo_echo.pqp --seconds 1 -o stereo_echo.wav` |
| `noise_tables.wav` (mono, 0.5 s) | `pqc render tests/golden/patches/noise_tables.pqp --seconds 0.5 -o noise_tables.wav` |

The list lives in `tests/golden/test_golden_render.cpp` (`kCases`). To regenerate after a deliberate change (listen to
the new files first): `$env:PQ_UPDATE_GOLDENS='1'; build/dev/tests/pq_tests_render.exe -tc="golden renders: interp*"`
writes the interpreter renders, then compares both backends against them as usual. Commit the WAVs with the change
that explains them.

## Benchmarks (T1.11)

Runner benches (`bench/include/pq_bench.h`: `pq::bench::Runner`) share one command line:

```
pq_bench_<suite> [--filter REGEX] [--list] [--repeats N] [--warmup N] [--min-time MS] [--csv FILE] [--no-pin]
```

- `--filter`: ECMAScript regex searched in benchmark names (`--list` prints them); a benchmark's setup runs only when
  it is selected. Exit 1 if nothing matches, 2 for a bad option.
- Every measurement: `--warmup` untimed rounds (default 3), then `--repeats` timed rounds (default 15), each round
  calling the code until it lasts `--min-time` ms (default 2). Reported: median, min, p99 (nearest rank: with fewer
  than 100 repeats it is the maximum; use `--repeats 101` for a real p99) and max, per op.
- The main thread is pinned to physical core 1 at raised priority (`--no-pin` to skip).
- `--csv FILE` writes `suite,benchmark,name,unit,median,min,p99,max,samples,load_percent,cpu,build` (load = system
  busy % while that benchmark ran: how shared the machine was).

Runner benches so far:

| Executable | Benchmarks |
|---|---|
| `pq_bench_core` | `spsc/…`, `mpsc/…`, `rtlog/…`, `baseline/…`, `cross/…` (pinned cross-thread throughput and round trips; `--runs N` per variant, default 11), `pool/…`, `arena/…`, `epoch/…`. The old `pq_bench_core cross [runs] [substring]` still works. |
| `pq_bench_dsp` | `atom/<name>[ (spl…)]`: every leaf atom kernel at L1 (ns/sample) and L8 (ns/frame of 8 voices), plus the markdown table; `--pad N` as before. |
| `pq_bench_render` | `patch32/interp xN`, `patch32/jit xN` (N = 1, 8, 64): a 32-op per-voice patch through the VoiceEngine, ns/sample and ns/voice-sample; `render/<golden> interp|jit`: offline render of each golden patch, ns/frame and × realtime. |

Recording results (release build only; RESULTS.md is append-only):

```
build/release/bench/pq_bench_render.exe --csv render.csv
python tools/bench/append_results.py render.csv --label T1.11 --config "block 64, 48 kHz" --note "what changed"
```

The script appends one row per CSV row: date, commit (`git rev-parse --short HEAD`, `-dirty` when tracked files other
than RESULTS.md changed, or `<label> (on <sha>)`), name, build + suite + `--config`, `median (min, p99, n)`, and the
machine (CPU from the CSV, logical CPUs, OS) with the system load and `--note`. `--dry-run` prints instead;
`--filter REGEX` keeps some rows; dev-build CSVs are refused unless `--allow-dev`.

**Migrating another bench** (the rest still print BENCH lines by hand): replace `main`'s body with a Runner, one
`add(name, fn)` per measurement or per group, and keep the existing code inside the lambdas. `report()` /
`reportStats()` calls made while a benchmark runs are recorded as its rows, so code that measures with
`pq::bench::measure` and reports by hand needs no other change; switching to `ctx.measure(f, ops, unit, suffix)`
additionally makes `--repeats/--warmup/--min-time` apply. Keep any extra flags of the bench by filtering them out of
`argv` before `runner.main` (as `pq_bench_dsp --pad` does).
