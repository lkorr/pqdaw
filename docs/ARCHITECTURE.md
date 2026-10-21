# pqdaw architecture

This document fixes the module layout and the interfaces between modules so that several agents can work in parallel without colliding. [SPEC.md](../SPEC.md) says *what* to build. This file says *where it goes and how the parts talk*. If an interface here has to change, change this file in the same commit and log it in `docs/PROGRESS.md`.

## 1. Modules and dependency direction

```
                    ┌──────────┐
                    │   app    │  pqdaw.exe: wires everything together, CLI flags, main loop
                    └────┬─────┘
        ┌────────────────┼──────────────────┬─────────────┐
        ▼                ▼                  ▼             ▼
   ┌─────────┐     ┌──────────┐       ┌──────────┐   ┌────────┐
   │   ui    │     │ document │       │  engine  │   │  io    │ (inside engine/ for now: audio + MIDI devices)
   └────┬────┘     └────┬─────┘       └────┬─────┘   └────────┘
        │               │       ┌──────────┤
        │               │       ▼          ▼
        │               │   ┌───────┐  ┌───────┐
        │               └──▶│ graph │◀─│  jit  │
        │                   └───┬───┘  └───────┘
        │               ┌───────┴────────┐
        │               ▼                ▼
        │          ┌────────┐       ┌────────┐
        │          │ tuning │       │  dsp   │
        │          └───┬────┘       └───┬────┘
        ▼              ▼                ▼
   ┌───────────────────────────────────────────┐
   │                   core                    │
   └───────────────────────────────────────────┘
```

Arrows point at dependencies. A module may only include headers from modules below it. **No cycles.** `ui` does not depend on `document` or `engine`. Views that need both live in `app/` (or a future `views/` module that sits above `ui` and `document`).

| Module | CMake target | Namespace | Contents |
|---|---|---|---|
| `src/core` | `pq_core` | `pq` | RT-safe primitives: SPSC/MPSC queues, pools, arena, epoch reclamation, allocation trap, denormal guard, aligned buffers, fast math approximations, `Ticks`/time types, stable IDs, non-RT logging plus an RT log ring |
| `src/tuning` | `pq_tuning` | `pq::tuning` | EDO, MOS (port of mos-explorer), Scala, JI (ratios, monzos, naming), pitch identities, resolution to Hz, the tuning-as-signal coefficient form |
| `src/dsp` | `pq_dsp` | `pq::dsp` | Native leaf-atom kernels (scalar + SIMD), FFT, analysers ported from PQ Meter (spectrum grid, NSDF pitch, LUFS…), sample-rate conversion, warp engines later |
| `src/graph` | `pq_graph` | `pq::graph` | Patch IR, patch-language parser/printer, inference (rate/kind/poly/width), optimiser, scheduler, interpreter backend, `Program` description |
| `src/jit` | `pq_jit` | `pq::jit` | LLVM IR generation from a schedule, ORC LLJIT, AOT compilation of `stdlib/`, compile cache |
| `src/engine` | `pq_engine` | `pq::engine` | Audio thread runtime: Program execution, voice allocation, transport, hot swap + state transfer, parameter blocks, taps for meters, audio device I/O (RtAudio), MIDI I/O, offline renderer |
| `src/document` | `pq_doc` | `pq::doc` | Immutable project model (tracks, clips, notes, devices, tunings, automation), commands, undo/redo, serialization (JSON + `.pqp`), project → graph lowering |
| `src/ui` | `pq_ui` | `pq::ui` | Window (SDL3), Vulkan renderer, draw lists (ImDrawVert-compatible), text (FreeType/HarfBuzz), widget toolkit, theme tokens, offscreen screenshot path |
| `src/app` | `pqdaw` (exe) | `pq::app` | Main, CLI, views (arrangement, piano roll, devices, circuit editor, browser) composed from `ui` + `document` + `engine` |
| `tools/` | `pqc` (exe) | – | Patch compiler CLI: parse/check/dump IR/schedule, offline render, AOT |
| `stdlib/` | – | – | Stock subcircuits and devices as `.pqp` text (filters, envelopes, devices) |
| `tests/` | `pq_tests_<module>` | – | doctest unit tests per module, golden renders in `tests/golden/` |
| `bench/` | `pq_bench` | – | Benchmarks; results appended to `bench/RESULTS.md` |

## 2. Threads

| Thread | Realtime? | Owns | Talks via |
|---|---|---|---|
| UI / main | no | document snapshots, UI state, Vulkan | posts `EngineCommand`s (SPSC), receives `EngineReport`s (SPSC, lossy) |
| Audio | **yes** | the current `Program`, voice state, param blocks | lock-free only |
| Compiler worker | no | graph lowering, inference, optimisation, JIT | publishes `ProgramHandle` (atomic pointer swap + epoch) |
| Disk | no | sample streaming, recording writer | SPSC rings per stream |
| Analysis | no | analysers (spectrum, scope, LUFS), indexer, transient/tempo detection | reads tap rings filled by the audio thread |

## 3. Key interfaces (sketches; the real headers are authoritative once written)

```cpp
// core: realtime-safe single-producer/single-consumer ring (power-of-two capacity, cache-line padded indices)
template <class T> class SpscRing {
public:
  explicit SpscRing(uint32_t capacityPow2);  // allocates: construct off the audio thread only
  bool tryPush(const T&) noexcept;
  bool tryPop(T&) noexcept;
};

// core: allocation trap. In tests and debug builds, any heap allocation while an RtScope is active fails the test.
struct RtScope { RtScope() noexcept; ~RtScope() noexcept; };   // marks the current thread "realtime"

// tuning: everything resolves to float32 Hz in the end
namespace pq::tuning {
  struct PitchId;                                  // Degree | Ratio | Cents | Harmonic (SPEC §3.1)
  class Tuning;                                    // EDO | MOS | Scale | JI, with reference pitch (default C3 = 261.626 Hz)
  float resolveHz(const PitchId&, const Tuning&, const ResolveContext&) noexcept;
  struct LinearForm { float k, m; int equave; };   // cents = k·g − m·P + equave·P within a MOS region (SPEC §3.3)
}

// graph: the compiler pipeline
namespace pq::graph {
  struct Patch;                                    // IR: nodes, ports, wires, params, exports, layout
  Result<Patch> parse(std::string_view source, std::string_view fileName);
  std::string print(const Patch&);                 // parse(print(p)) == p
  Result<TypedPatch> infer(const Patch&, const InferOptions&);
  TypedPatch optimise(TypedPatch);
  Schedule schedule(const TypedPatch&, const ScheduleOptions&);   // SCCs, blocks, buffer liveness, state layout keyed by NodeId
  std::unique_ptr<ProgramImage> buildInterpreted(const Schedule&); // allocation happens here, never on the audio thread
}

// jit
namespace pq::jit {
  class Compiler {                                 // one per process; owns LLJIT
  public:
    Result<std::unique_ptr<ProgramImage>> compile(const graph::Schedule&, const CompileOptions&);
  };
}

// engine: what the audio thread runs
namespace pq::engine {
  class ProgramImage {                             // interpreted or JIT-compiled, same ABI
  public:
    virtual void process(ProcessContext&) noexcept = 0;   // one block; no allocation, no locks
    virtual StateLayout const& stateLayout() const noexcept = 0;  // for state transfer by NodeId
  };
  class Engine {
  public:
    void publish(std::unique_ptr<ProgramImage>, SwapPolicy);  // non-RT thread; RT side swaps at block boundary
    void post(const EngineCommand&);                          // UI → audio
    bool poll(EngineReport&);                                 // audio → UI
    void renderOffline(const RenderRequest&, AudioSink&);     // same Program, deterministic
  };
}
```

### ProgramImage ABI (shared by interpreter and JIT)
- `ProcessContext` carries: frame count (≤ max block), sample rate, transport snapshot, pointers to input/output channel buffers, the parameter block (`float*`, indexed by `ParamSlot`), the event list for this block (sorted, preallocated), and the voice table.
- All Program state lives in **one arena** allocated when the image is built. The state layout maps `NodeId → (offset, size, type tag)`. Hot swap copies state for every NodeId whose type tag matches (SPEC §7.5).
- Parameter values live in a double-buffered block written by the engine from `EngineCommand`s. Programs read them and never write them.

## 4. Data and file formats
- **Project**: a directory: `project.pqd` (JSON, `"schemaVersion"` field, UTF-8), `patches/*.pqp`, `samples/`, `cache/`.
- **Patches**: `.pqp` text in the patch language ([PATCH_LANGUAGE.md](PATCH_LANGUAGE.md)). The visual layout is stored in the same file in a `layout { }` block.
- **Golden audio** for tests: 32-bit float mono/stereo WAV, compared with a tolerance per test (absolute error and spectral error).

## 5. Build configurations
- **Generator**: Ninja via `CMakePresets.json`. **Compiler**: MSVC v143 (cl.exe), x64. **Language**: C++20 (`/std:c++20`).
- **CRT**: match the prebuilt LLVM. All targets use the same `CMAKE_MSVC_RUNTIME_LIBRARY` (see `cmake/PqToolchain.cmake`). There is **no MSVC Debug configuration**, because the LLVM libs are release builds. The `dev` preset is `RelWithDebInfo` with optimisations reduced; `release` is fully optimised.
- **Flags**: `/W4 /permissive- /Zc:__cplusplus /utf-8 /arch:AVX2 /fp:precise`. Warnings are errors in our code (not third-party). SIMD kernels can be compiled with stricter or looser flags per file where measured.
- **Third-party code** comes in via `FetchContent`, pinned to an exact tag or commit and recorded in `docs/DEPENDENCIES.md`. Only permissive licences: MIT, BSD, zlib, Apache-2.0, Boost, MPL-2.0 (file-level), OFL for fonts. No GPL; LGPL only if dynamically linked and justified.

## 6. Conventions
- Files: `snake_case.h/.cpp`. Types: `PascalCase`. Functions and variables: `camelCase`. Constants: `kPascalCase`. Members: `trailingUnderscore_`.
- Headers: `#pragma once`; public headers in `src/<module>/include/pq/<module>/…`; private code in `src/<module>/src/…`.
- Errors: `pq::Result<T>` (value or `pq::Error{code, message, location}`) on non-RT paths. Exceptions are allowed only at the outermost non-RT boundaries (file I/O, parsing), and are caught there. Everything reachable from the audio thread is `noexcept`.
- No global mutable state except explicit singletons in `app/` (JIT compiler, audio device manager).
- Comments explain *why*. Every RT-path function says in its header comment that it is RT-safe.
