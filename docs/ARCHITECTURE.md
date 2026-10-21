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

// core: RT log ring. One RtLog per realtime thread (SPSC of POD records: static "{}" format, code, ≤ 4 numeric
// args, rdtsc, sequence); RtLogger drains every 10 ms on its own thread, formats, writes lines; drops are counted
// exactly and reported as a line plus a sequence gap.
class RtLog { public: explicit RtLog(uint32_t capacityPow2); template <class... A> bool log(uint32_t code, const char* fmt, A...) noexcept; };
class RtLogger { public: void add(RtLog&, std::string name); void start(); void stop(); size_t drainOnce(); };

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
  // pq/graph/infer.h (T1.3). Flattens subcircuits per call site (callees from InferOptions::library) and infers
  // kind/rate/poly/width with declared types as upper bounds. TypedPatch = { Patch patch (flat: no Subcircuit nodes,
  // inserted voice_sum reducers, bool eq/ne rewritten), types[] (inferred, parallel to patch.portTypes()),
  // stateKey[] (per node: hash of the NodeId call path, the hot-swap key), origin[], instances[] }. inferDetailed()
  // returns every Diagnostic, located at root-patch nodes via ParseResult::nodeSpans.
  Result<TypedPatch> infer(const Patch&, const InferOptions&);
  // pq/graph/optimise.h (T1.4). Durations → samples (`· sr` inserted, TypedPatch::durationsInSamples), then a
  // fixpoint of constant folding (by the atom's own dsp kernel, never params), exact algebraic simplification, CSE
  // and DCE; then rate refinement (pure atoms = max of their inputs' rates) and a·b + c → muladd fusion at equal
  // rates. Stateful nodes keep stateKey, atom and types; replacements never change kind/width/poly. Error only for a
  // folded channel index out of range. OptimiseOptions: sampleRate (sr folds when known), maxPasses, fuseMulAdd.
  Result<TypedPatch> optimise(TypedPatch, const OptimiseOptions& = {}, OptimiseStats* = nullptr);
  // pq/graph/schedule.h (T1.5). Tarjan SCCs on wires + implicit equation→unknown edges of solve blocks; feedback SCCs
  // become Loop regions with z1/delay split into read (in place) + write (loop end), run per sample or, when every
  // cut is a delay line with a constant lower bound, in sub-blocks of the shortest tap age; a delay-free cycle is an
  // error (text) or gets a z1 + warning (ScheduleOptions::visualMode, Schedule::rewritten). Stages alternate global
  // (even, lanes 1) / voice (odd, lanes 8, run per voice group); a stage = Control region (param/const ops once per
  // block on 8-float patterns, param-rate solves) + Block/Loop regions. Global audio read by voice ops is broadcast;
  // voice reducers accumulate in the voice stage. Flat output: ops {kind, dsp kernel, lanes, Spl mask, operands
  // (Buffer | Param slot | Const | External), state entry, resource}, regions, stages, solve blocks, buffers (interval
  // reuse, in-place element-wise ops; the 100-node test chains need 1–2), params, externals, outputs, StateLayout. dump() is
  // the `pqc dump-schedule` text.
  ScheduleResult scheduleDetailed(const TypedPatch&, const ScheduleOptions& = {});  // + warnings in the Schedule
  Result<Schedule> schedule(const TypedPatch&, const ScheduleOptions& = {});
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
  class AudioDevice {                       // NullDevice (tests/headless), WasapiDevice (own WASAPI: exclusive + shared,
                                            // output only), RtAudioDevice (RtAudio WASAPI shared; ASIO opt-in)
    virtual Result<std::vector<DeviceInfo>> enumerate();                       // non-RT virtuals
    virtual Result<StreamInfo> open(const StreamConfig&, AudioCallback);
    virtual Status start(); virtual Status stop(); virtual const CallbackStats& stats() const noexcept;
  };
  class CallbackRunner;   // the one per-callback path every backend calls: RtScope + DenormalGuard + rdtsc stats
  class BlockAdapter;     // device buffer of any size → engine at a fixed internal block (direct when divisible, else FIFO +block latency)
  class UnderrunTracker;  // per device wake: lateness vs the wall-clock schedule, empty shared buffer, device clock past
                          // frames written → CallbackStats::outputUnderflows (one per episode) + recordWake (lateness, queue)
  void planarToInterleaved(...);  // planar float → interleaved float32/int32/24-in-32/int24/int16 (AVX2 mono/stereo)
}
```
`Backend::Wasapi` is our WasapiDevice (the default); `Backend::RtAudioWasapi` keeps RtAudio's WASAPI for comparison and input. WasapiDevice's audio thread joins MMCSS "Pro Audio" once, waits on the device event, and per event runs the callback once per period on planar scratch, then converts once into the device buffer (no ring).
The engine's process entry will be an `AudioCallback` behind a `BlockAdapter`; `ProcessContext` frame counts are therefore always the fixed internal block on the realtime path.

### ProgramImage ABI (shared by interpreter and JIT)
- `ProcessContext` carries: frame count (≤ max block), sample rate, transport snapshot, pointers to input/output channel buffers, the parameter block (`float*`, indexed by `ParamSlot`), the event list for this block (sorted, preallocated), and the voice table.
- All Program state lives in **one arena** allocated when the image is built (plus one block per voice group for voice state). The state layout (`graph::StateLayout`, T1.5) maps `(TypedPatch::stateKey, channel, arena) → (offset, size, type tag, StateInit)`; the tag is (kernel, lanes, ring frames for delay lines). Hot swap copies state for every key whose tag and size match (SPEC §7.5). Given the previous layout, `schedule()` keeps every unchanged entry at its old offset (new entries fill holes, then append; compacted when holes exceed 2× the live size + 4 KiB), so the transfer is a few coalesced memcpys.
- Parameter values live in a double-buffered block written by the engine from `EngineCommand`s. Programs read them and never write them.

### Leaf-atom kernel ABI (`pq/dsp/kernel.h`, `pq/dsp/atoms.h`; T1.6)
The interpreter's op record is `{KernelFn, KernelArgs}`; the JIT reproduces the same semantics inline.
- `using KernelFn = void (*)(const KernelArgs&) noexcept;` with `KernelArgs { const float* const* in; float* const* out; void* state; const void* resource; uint32_t frames; uint16_t numIn, numOut; uint32_t splMask; }`.
- **Lanes**: each kernel is compiled for L = 1 (global signal) or L = 8 (one voice group). A block buffer holds `frames · L` floats, frame-major with voices interleaved (`[s·8 + v]`, one frame = one `__m256`). Global stateless kernels are vectorised over time.
- **Rates**: an input is a block buffer (audio rate) or a Spl pattern of 8 floats constant over the block (param rate; per-voice values for L = 8, 8 copies of the value for L = 1). The Spl mask is a template specialisation chosen by `findKernel(atom, lanes, splMask)`, so unmodulated inputs cost one vector load per block and no per-sample branch.
- **Bools** are 32-bit lane masks (all ones / zero) in float slots, for both L.
- **State**: one POD block per stateful node (`atom_state.h`, 32-byte aligned, 8 lanes even for L = 1), sized by `stateBytes(atom, StateInit)` and written by `initState` (no allocation). Its type tag for hot swap is the `Atom` id (plus the lane count for delay lines, whose ring follows the header).
- **Resources** (wavetables, shaper curves, sample data) are immutable tables built off-thread (`tables.h`) and passed by pointer.
- **Registry ↔ kernel**: `pq/graph/kernel_map.h` (`kernelMapping(AtomId)`) is the one mapping from language atoms to kernels: the kernel, and per registry input whether it is a kernel input (durations: `· sr` inserted by `optimise()`, after which `TypedPatch::durationsInSamples` is set), a resource, or a state-init const (`seed`, `max` delay, z1 `init`). Atoms without a kernel are marked Structural (`channel`, `pack`), Constant (`sr`) or Engine (`voice.*`, `transport.*`, I/O, `voice_*` reducers). Registry `stateBytes` = the kernel's state block. Tested against both tables (QUESTIONS Q14).
- **Feedback**: `z1` and the delay lines also have split kernels (`z1:read`/`z1:write`, `delay*:read`/`delay:write`) sharing the atom's state, so a feedback SCC scheduled per sample can read at the start of the sample and write at the end.

### Document model (`pq/document/*.h`, T3.4)
- `Snapshot = shared_ptr<const Project>`. `Project`: timeline (tempo + time-signature points and their built maps), main tunings (`TuningSlot`: name, colour, `TuningDef` = EDO | MOS params | `ScaleDef` (Scala .scl + optional .kbm) + reference Hz), locators, loop, track tree (`tracks`, Midi + nested Group), `returns`, `master`. `Track`: kind, name, colour, `TuningAssignment` (Inherit | Main | Private), `Mixer` (volume dB, pan, mute, solo, arm, non-default sends, output), active, children, clips (`ClipSlot {id, ClipPtr}` sorted by start), automation lanes (`ParamRef` + breakpoints). `NoteClip`: start, length, content start, loop, signature, tuning, active, `NoteSeq`. `Note`: 64 bytes (start, length, `PitchId`, id, velocity, release, chance, velocity range, flags).
- Every shareable node is `shared_ptr<const T>`; an edit path-copies from the changed node to the root, everything else stays pointer-equal. `NoteSeq` stores notes sorted by `(start, id)` in immutable chunks of ≤ 128 notes, so a one-note edit copies one chunk.
- `Command` = variant of ~50 primitive structs + `Batch`; `apply(snapshot, command, ChangeSet&) → Result<Snapshot>` is pure (ids pre-allocated from the store's `IdSource`), returns the input pointer for a no-op. T3.6 builds higher-level edits as Batches.
- Edit operations (`pq/document/edit_ops.h`, T3.6, namespace `pq::doc::ops`): pure builders (snapshot + selection + parameters + the store's `IdSource`) → `Edit { Batch command; selection after }`, one history entry each; views call these instead of composing primitives. Track, clip, note, identity-aware Up/Down (`stepPitch`), time-selection (insert/delete/duplicate time) and automation edits. Helpers: `clip_time.h` (where clip content plays: loops, start marker) and `automation_curve.h` (the breakpoint curve law, to move to `core` when T3.16 evaluates automation; Q39).
- `ChangeSet`: touched ids + class flags `Meta | Param | Sequence | Automation | Timeline | Structural` (`paramOnly()`, `sequenceOnly()`, …) and the changed `ParamRef`s; drives T3.17 (recompile only on Structural).
- `DocumentStore` (UI thread): current snapshot, `dispatch`, `History` (snapshot pairs; undo/redo swap pointers), gestures (`begin/commit/cancelGesture`: live notifications, one history entry), listeners (`StoreEvent`), and `ViewState` (fold/heights, not undoable; Q26).
- Project files (`pq/document/serialize.h`, T3.5): `serializeProject`/`parseProject` (text) and `saveProject`/`loadProject`/`openProject` (directory, UTF-8 paths, atomic temp + flush + `MoveFileExW`). Snapshot + `ViewState`, ids preserved; strict reader with JSON-path errors; `Migration` table (n → n+1) on an nlohmann DOM for old files.
- `document` depends on `tuning`, `core` and nlohmann/json (migrations only). The audio thread never sees it.

### Widget toolkit (ui, T3.1: `pq/ui/widget.h`, `input.h`, `actions.h`, `test_driver.h`)
- **Tree**: `UiTree` owns one root `Widget`; widgets own their children (`add<W>(...)`). Ids are paths of names (`arrangement/track[3]/header/mute`; unnamed widgets are transparent). Override `paint`/`paintOver`, `onPointer`/`onKey`/`onText`, `onHoverChanged`/`onFocusChanged`/`onCaptureLost`, `onFrame`/`onTimer`, `contentExtent`, `arrange`.
- **Layout**: one top-down pass, no measure pass. `Length` = Fixed px | Flex weight, with min/max; containers are Row / Column / Stack / Custom (`Splitter`, `ScrollView`, views). Lengths are **logical px**; rects are **device px** = round(logical × `UiTree::uiScale()`), so text stays at integer pixel sizes. Only dirty subtrees are revisited.
- **HiDPI (`pq/ui/ui_scale.h`, T0.7-fix3)**: `UiScale` is the one logical → device conversion (`px`, `rect` snap each edge; `exact` for AA geometry; `fontPx(role) = round(role × scale)`). Source of truth: the app reads `SDL_GetWindowDisplayScale` at startup and on `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED` / `DISPLAY_CHANGED` (or `--scale`), then pushes it to `UiTree::setViewport` and `setSceneScale` (which re-resolves the `FontRole`s; new sizes are created there, never in paint); widgets read `PaintContext::scale` and `res.fonts`, scenes read `SceneTextures::scale`. Contract: layout and positions in logical px through `UiScale`; text through font roles (`TextSystem::font(FontRole, UiScale)`), never a hard-coded px size; the draw list, `lines.h` line widths, AA feathers and hairlines are device px and never read the scale; `TextSystem` measures and draws in device px. Pointer events are device px. `pqdaw --screenshot … --size WxH --scale S` renders W×H logical px at round(W·S)×round(H·S) device px.
- **Dense content** (clips, notes, grid) is drawn by one widget inside a `ScrollView`: it reports `contentExtent()` and paints only `PaintContext::clip`. Never one widget per note.
- **Redraw**: `markDirty()` adds the widget's clipped rect to the tree's `Damage`; layout adds the old and new rects of whatever moved. `requestFrame()` / `requestTimer()` are the only ways to animate; idle means no requests, and the loop blocks until the next event or `nextWakeTime()`. **Partial redraw (T0.7-fix1)**: the frame loop reads `damage().rects()` after layout, paints with `UiTree::paint(dl, res, now, region)` where region = their bounding box (only widgets intersecting it paint; `PaintContext::clip` starts as region ∩ target, so virtualised views draw only that range) and renders the list with those rects as the frame's damage. Contract for widgets and views: **paint only inside your rect** (push it as a clip if geometry could spill, e.g. text, glow, AA feathers past the edge), and report every visual change with `markDirty()` (or layout); anything drawn outside the rect, or changed without damage, goes stale on screen.
- **Input**: SDL-free `PointerEvent` / `KeyEvent` / `TextEvent` (`translateEvent` in `pq/ui/window.h` adapts SDL). A handled Down captures the pointer until every button is released; hover is frozen while captured. Keys go: focused widget → its ancestors (a focused text-entry widget swallows printable keys) → modal scopes (newest first; `ScopeRules` passthrough modifiers / swallow unbound, which is how macro mode works) → `keyScope`s of the focus chain → global → Tab traversal. `ActionRegistry` holds string ids → handler + enabled predicate; a `Keymap` is data (scope, chord → action id) and is resolved to indices in `UiTree::setKeymap`. Dispatch does not allocate.
- **Tests**: `TestDriver` builds a tree at a size and scale, injects events, runs a fake clock through update → paint, and renders through `OffscreenTarget`.

### Theme, controls and overlays (ui, T3.2: `pq/ui/theme.h`, `controls.h`, `overlay.h`, `keymap_file.h`)
- **Theme as data**: `Theme` (surface/control/field/text/accent/selection/menu/tooltip/track/clip/grid/scrollbar colours, font roles 12/13/14, spacing, radii, sizes, 70-colour clip palette) is read from `assets/themes/ableton-dark.json` by a strict reader (a missing/unknown/duplicate token is an error naming it; `serializeTheme` writes the canonical file). `SceneTextures` owns it (`theme`, `uiFonts` = the theme's font roles resolved at the scale, re-resolved by `setSceneScale`/`setSceneTheme`); widgets read `PaintContext::res.theme` / `res.uiFonts` (plain member loads). Dev builds hot-reload the file (`ThemeFileWatcher`, one file-time query per 0.5 s on the UI thread). PQ Meter's palette stays in `signal_theme` and only signal displays use it (meters, modulation rings).
- **Controls** derive from `Control` (tooltip, info text, `VisualState` normal/hover/pressed/focused/disabled, `setStateOverride` for galleries): `Label`, `Button`, `Toggle` (Accent/Activator/Solo/Arm), `ValueControl` → `DragBox` (Live's number box), `Fader`, `Knob` (`ModulationRing` hook), `TextEdit` (on `TextBuffer`), `Dropdown`, `Scrollbar` (a `ScrollView::Dependent`), `Meter` (`MeterLevels` plain struct, repaints only when a drawn pixel changes). A value edit is a gesture: `onGestureBegin` (lazily, at the first change) → `onChange`* → `onGestureEnd`, one per drag / type-in / reset / key = one undo step (T3.4 `History` gestures).
- **Overlays**: an `OverlayLayer` as the root's last child hosts popups (`Menu` with nested submenus and keyboard navigation, `Popover`, tooltips); it is hittable only while a dismissable popup is open, so a press outside closes them and is consumed. Popups take and restore focus; closed popups are destroyed at the next frame.
- **Tree services** (`UiTree`): `setResources` (what widgets measure text with outside paint), `clipboard()` (`SdlClipboard` in the app, in-process otherwise), `TreeObserver`s (hover changes, presses: tooltips, the T3.7 info view via `infoFor(hovered)`), `focusVisible()` (focus came from the keyboard), `actions()`. Widgets expose `info()` / `tooltip()`. Custom layouts are arranged even without children.
- **Keymaps**: `assets/keymaps/ableton.json` (scope, keys, action id, status `implemented` | `deferred:M<n>`); `applyKeymapFile` binds implemented entries and fails on an unregistered one. `docs/HOTKEYS.md` lists every binding.

### Renderer, textures and glyph atlas (ui, T0.7 / T0.8; T0.8-fix1, T0.7-fix4: `pq/ui/renderer.h`, `atlas.h`, `text.h`)
- **Frame protocol** (Swapchain::endFrame, OffscreenTarget::submit): wait the slot's fence → `Renderer::upload(slot, layers)` → begin the command buffer → `recordTransfers(cb, slot)` → begin rendering → `record(cb, slot, frame)` → submit. Slots are used round-robin. Nothing in the renderer waits for the GPU (no `vkDeviceWaitIdle`, no `submitAndWait`); the slot fence wait is the only CPU–GPU sync.
- **Partial redraw (T0.7-fix1)**: a frame is a `FrameDesc` = ordered `DrawLayer`s + `full` or a list of disjoint whole-pixel damage rects (`Damage` keeps ≤ 8, merging overlapping/tiling ones) + the clear colour. Targets keep a persistent colour image (OffscreenTarget's own image; the Swapchain renders into its own persistent image and copies it whole into the acquired swapchain image, since swapchain images are not guaranteed to keep their contents). Full frames CLEAR; partial frames LOAD, `vkCmdClearAttachments` the rects and draw every command once per rect it reaches, scissored to clip ∩ rect, so each damaged pixel sees the clear then exactly the primitives covering it in list order: bit-identical to a full redraw (tested over 100 random sequences each for draw lists, a retained layer and a widget tree). A transient layer is copied into the slot's buffers each frame; a retained layer (`retained` id < 4, `version`) stays on the GPU per slot and is re-copied only when its version changes, and is indexed in runs of 64 triangles with vertex bounds so a partial frame draws only the runs reaching the damage. `Swapchain::preserved()` is false after a recreate (resize): the next frame must be full. The app draws in full when the damage covers > 50 % of the target, on resize/scale/theme changes and for scenes that cannot report damage.
- **Staging ring**: texture bytes are copied into persistently mapped 1 MiB chunks when they are handed over (`createTexture`, `updateTexture`, atlas dirty rects at `upload`), queued, bound to the next frame at `upload(slot)`, recorded as buffer → image copies between two batched barriers, and recycled when that slot comes round again. Overlapping copies to one texture in one frame are ordered (a covered copy is dropped, a partial overlap goes into a later batch behind a barrier).
- **Textures** (`TextureId`): `kAtlasTexture` (0) = the shared sprites (white texel, glow blob), valid on any atlas page; `atlasPageTexture(p)` (1..kMaxAtlasPages) = glyph page p; user textures from `kFirstUserTexture`. `createTexture` (async), `updateTexture(id, rect, rgba, rowPitch)` (async), `destroyTexture(id)` (deferred until every frame slot in flight at destroy time has been recycled; the id is then reused, so a draw list must not use a destroyed id). The draw list merges `kAtlasTexture` with any atlas page, so fills and labels stay one draw command.
- **Glyph atlas**: up to 4 pages of 1024² R8 coverage (each also holds the shared sprites), sampled through the view swizzle (1, 1, 1, R) so one pipeline/shader serves glyphs, sprites and RGBA images. Pages open on demand; when all are full the least recently used page not touched by the current frame is cleared (generation++); glyphs are re-rasterised on next use. If every page is in use this frame the glyph is dropped, counted (`Stats::atlasFull`) and reported once on stderr; its run is rebuilt on a later frame.
- **Text frames**: `TextSystem::beginFrame()` once per frame (the app loop and `TestDriver` call it). A `TextRun` stays valid until the next `beginFrame()`/`clearRunCache()`: the run cache is trimmed only there (M8). `TextRun` carries per-cluster caret stops (`carets`, `caretX`, `hitTest`, LTR) and `shapeTruncated`/`drawTruncated` cut at a cluster boundary and append "…" (`truncated`, `visibleBytes`).

## 4. Data and file formats
- **Project**: a directory: `project.pqd` (JSON, UTF-8, `"schemaVersion"` first; layout in `pq/document/serialize.h` and the golden `tests/document/fixtures/full/project.pqd`: ticks and ids as integers, rationals as `"n/d"`, floating point as shortest round-trip numbers, embedded tuning definitions), `patches/*.pqp`, `samples/`, `cache/`.
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
