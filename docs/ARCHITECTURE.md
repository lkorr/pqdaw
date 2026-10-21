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
| `src/document` (headers `pq/document/`) | `pq_doc` | `pq::doc` | Immutable project model (tracks, clips, notes, devices, tunings, automation), commands, undo/redo, serialization (JSON + `.pqp`), project → graph lowering |
| `src/ui` | `pq_ui` | `pq::ui` | Window (SDL3), Vulkan renderer, draw lists (ImDrawVert-compatible), text (FreeType/HarfBuzz), widget toolkit, theme tokens, offscreen screenshot path |
| `src/app` | `pq_app` (lib) + `pqdaw` (exe: `main.cpp` only, so tests can link the CLI/commands) | `pq::app` | Main, CLI, views (arrangement, piano roll, devices, circuit editor, browser) composed from `ui` + `document` + `engine` |
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
  class MosLinearTable;                            // per chain length n: (k_i, m_i) for every constant-order interval of g
  MosLinearSpan MosLinearTable::lookup(double g, double P) const noexcept;  // cents_i = k_i·g − m_i·P (+ equave·P), SPEC §3.3
}

// graph: the compiler pipeline
namespace pq::graph {
  class Patch;          // IR (pq/graph/patch.h): flat node/port/wire arrays, dense NodeIndex + stable NodeId,
                        // params with attrs, in/out/export declarations, subcircuit nodes, solve blocks, layout;
                        // validate() for structure + declared types; operator== is structural (ignores NodeIds/order)
  struct PatchFile;     // a .pqp file: imports + patches (pq/graph/patch_file.h)
  class AtomRegistry;   // leaf atoms + operators: name → signature (pq/graph/atoms.h)
  // pq/graph/parse.h. A file holds several patches. Options: externals (prelude signatures), an ImportLoader for
  // `import`s (graph does no file I/O), validate. parseDetailed() returns every Diagnostic (message, file:line:col,
  // byte span); parse() formats them `file:line:col: error: …` + source line + caret into the Error.
  Result<PatchFile> parse(std::string_view source, std::string_view fileName, const ParseOptions& = {});
  ParseResult parseDetailed(std::string_view source, std::string_view fileName, const ParseOptions& = {});
  std::string print(const PatchFile&);             // parse(print(f)) == f
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

### Audio device I/O (engine, T0.6)
```cpp
namespace pq::engine {
  struct AudioBlock { const float* const* in; uint32_t numIn; float* const* out; uint32_t numOut; uint32_t frames; int64_t frameTime; };
  using AudioCallbackFn = void (*)(void* user, const AudioBlock&) noexcept;    // once per block, planar float
  class AudioDevice {                       // NullDevice (tests/headless), RtAudioDevice (WASAPI shared; ASIO opt-in)
    virtual Result<std::vector<DeviceInfo>> enumerate();                       // non-RT virtuals
    virtual Result<StreamInfo> open(const StreamConfig&, AudioCallback);
    virtual Status start(); virtual Status stop(); virtual const CallbackStats& stats() const noexcept;
  };
  class CallbackRunner;   // the one per-callback path every backend calls: RtScope + DenormalGuard + rdtsc stats
  class BlockAdapter;     // device buffer of any size → engine at a fixed internal block (direct when divisible, else FIFO +block latency)
}
```
The engine's process entry will be an `AudioCallback` behind a `BlockAdapter`; `ProcessContext` frame counts are therefore always the fixed internal block on the realtime path.

### ProgramImage ABI (shared by interpreter and JIT)
- `ProcessContext` carries: frame count (≤ max block), sample rate, transport snapshot, pointers to input/output channel buffers, the parameter block (`float*`, indexed by `ParamSlot`), the event list for this block (sorted, preallocated), and the voice table.
- All Program state lives in **one arena** allocated when the image is built. The state layout maps `NodeId → (offset, size, type tag)`. Hot swap copies state for every NodeId whose type tag matches (SPEC §7.5).
- Parameter values live in a double-buffered block written by the engine from `EngineCommand`s. Programs read them and never write them.

### Leaf-atom kernel ABI (`pq/dsp/kernel.h`, `pq/dsp/atoms.h`; T1.6)
The interpreter's op record is `{KernelFn, KernelArgs}`; the JIT reproduces the same semantics inline.
- `using KernelFn = void (*)(const KernelArgs&) noexcept;` with `KernelArgs { const float* const* in; float* const* out; void* state; const void* resource; uint32_t frames; uint16_t numIn, numOut; uint32_t splMask; }`.
- **Lanes**: each kernel is compiled for L = 1 (global signal) or L = 8 (one voice group). A block buffer holds `frames · L` floats, frame-major with voices interleaved (`[s·8 + v]`, one frame = one `__m256`). Global stateless kernels are vectorised over time.
- **Rates**: an input is a block buffer (audio rate) or a Spl pattern of 8 floats constant over the block (param rate; per-voice values for L = 8, 8 copies of the value for L = 1). The Spl mask is a template specialisation chosen by `findKernel(atom, lanes, splMask)`, so unmodulated inputs cost one vector load per block and no per-sample branch.
- **Bools** are 32-bit lane masks (all ones / zero) in float slots, for both L.
- **State**: one POD block per stateful node (`atom_state.h`, 32-byte aligned, 8 lanes even for L = 1), sized by `stateBytes(atom, StateInit)` and written by `initState` (no allocation). Its type tag for hot swap is the `Atom` id (plus the lane count for delay lines, whose ring follows the header).
- **Resources** (wavetables, shaper curves, sample data) are immutable tables built off-thread (`tables.h`) and passed by pointer.
- **Feedback**: `z1` and the delay lines also have split kernels (`z1:read`/`z1:write`, `delay*:read`/`delay:write`) sharing the atom's state, so a feedback SCC scheduled per sample can read at the start of the sample and write at the end.

### Document model (`pq/document/*.h`, T3.4)
- `Snapshot = shared_ptr<const Project>`. `Project`: timeline (tempo + time-signature points and their built maps), main tunings (`TuningSlot`: name, colour, `TuningDef` = EDO | MOS params + reference Hz), locators, loop, track tree (`tracks`, Midi + nested Group), `returns`, `master`. `Track`: kind, name, colour, `TuningAssignment` (Inherit | Main | Private), `Mixer` (volume dB, pan, mute, solo, arm, non-default sends, output), active, children, clips (`ClipSlot {id, ClipPtr}` sorted by start), automation lanes (`ParamRef` + breakpoints). `NoteClip`: start, length, content start, loop, signature, tuning, active, `NoteSeq`. `Note`: 64 bytes (start, length, `PitchId`, id, velocity, release, chance, velocity range, flags).
- Every shareable node is `shared_ptr<const T>`; an edit path-copies from the changed node to the root, everything else stays pointer-equal. `NoteSeq` stores notes sorted by `(start, id)` in immutable chunks of ≤ 128 notes, so a one-note edit copies one chunk.
- `Command` = variant of ~50 primitive structs + `Batch`; `apply(snapshot, command, ChangeSet&) → Result<Snapshot>` is pure (ids pre-allocated from the store's `IdSource`), returns the input pointer for a no-op. T3.6 builds higher-level edits as Batches.
- `ChangeSet`: touched ids + class flags `Meta | Param | Sequence | Automation | Timeline | Structural` (`paramOnly()`, `sequenceOnly()`, …) and the changed `ParamRef`s; drives T3.17 (recompile only on Structural).
- `DocumentStore` (UI thread): current snapshot, `dispatch`, `History` (snapshot pairs; undo/redo swap pointers), gestures (`begin/commit/cancelGesture`: live notifications, one history entry), listeners (`StoreEvent`), and `ViewState` (fold/heights, not undoable; Q23).
- `document` depends on `tuning` and `core` only. The audio thread never sees it.

## 4. Data and file formats
- **Project**: a directory: `project.pqd` (JSON, `"schemaVersion"` field, UTF-8), `patches/*.pqp`, `samples/`, `cache/`.
- **Patches**: `.pqp` text in the patch language ([PATCH_LANGUAGE.md](PATCH_LANGUAGE.md)). The visual layout is stored in the same file in a `layout { }` block.
- **Golden audio** for tests: 32-bit float mono/stereo WAV, compared with a tolerance per test (absolute error and spectral error).

## 5. Build configurations
- **Generator**: Ninja via `CMakePresets.json`. **Compiler**: MSVC v143 (cl.exe), x64. **Language**: C++20 (`/std:c++20`).
- **CRT**: match the prebuilt LLVM. All targets use the same `CMAKE_MSVC_RUNTIME_LIBRARY` (see `cmake/PqToolchain.cmake`). There is **no MSVC Debug configuration**, because the LLVM libs are release builds. The `dev` preset is `RelWithDebInfo` with optimisations reduced; `release` is fully optimised.
- **Flags**: `/W4 /permissive- /Zc:__cplusplus /utf-8 /arch:AVX2 /fp:precise`. Warnings are errors in our code (not third-party). SIMD kernels can be compiled with stricter or looser flags per file where measured.
- **Allocation trap** (`pq/core/alloc_trap.h`): the OBJECT library `pq_alloc_trap` replaces global `operator new/delete`. `pq_add_test` links it into every test executable (both presets), where an allocation or free inside an `RtScope` fails the running doctest case; use `pq::alloc_trap::countRtViolations([&]{ … })` to assert that an RT entry point does not allocate. `pq_add_executable` links it only when `PQ_ALLOC_TRAP_IN_APPS` is ON (default ON for dev, OFF for release), where a violation prints a stack trace and aborts. `malloc`/`free` and OS allocators are not interposable under `/MT` and are not caught.
- **Third-party code** comes in via `FetchContent`, pinned to an exact tag or commit and recorded in `docs/DEPENDENCIES.md`. Only permissive licences: MIT, BSD, zlib, Apache-2.0, Boost, MPL-2.0 (file-level), OFL for fonts. No GPL; LGPL only if dynamically linked and justified.

## 6. Conventions
- Files: `snake_case.h/.cpp`. Types: `PascalCase`. Functions and variables: `camelCase`. Constants: `kPascalCase`. Members: `trailingUnderscore_`.
- Headers: `#pragma once`; public headers in `src/<module>/include/pq/<module>/…`; private code in `src/<module>/src/…`.
- Errors: `pq::Result<T>` (value or `pq::Error{code, message, location}`) on non-RT paths. Exceptions are allowed only at the outermost non-RT boundaries (file I/O, parsing), and are caught there. Everything reachable from the audio thread is `noexcept`.
- No global mutable state except explicit singletons in `app/` (JIT compiler, audio device manager).
- Comments explain *why*. Every RT-path function says in its header comment that it is RT-safe.
