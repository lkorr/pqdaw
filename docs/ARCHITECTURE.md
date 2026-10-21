# pqdaw architecture

This document fixes the module layout and the interfaces between modules so that several agents can work in parallel without colliding. [SPEC.md](../SPEC.md) says *what* to build. This file says *where it goes and how the parts talk*. If an interface here has to change, change this file in the same commit and log it in `docs/PROGRESS.md`.

## 1. Modules and dependency direction

```
                    ┌──────────┐
                    │   app    │  pqdaw.exe: wires everything together, CLI flags, main loop, TransportPort impl
                    └────┬─────┘
        ┌────────────────┼──────────────────┬─────────────┐
        ▼                │                  ▼             ▼
   ┌─────────┐           │            ┌──────────┐   ┌────────┐
   │  views  │ (T3.7)    │            │  engine  │   │  io    │ (inside engine/ for now: audio + MIDI devices)
   └──┬───┬──┘           │            └────┬─────┘   └────────┘
      │   └──────────┐   │                 ├────────────┐
      ▼              ▼   ▼                 │            ▲
   ┌─────────┐     ┌──────────┐            │        ┌───────┐
   │   ui    │     │ document │            │        │  jit  │
   └────┬────┘     └────┬─────┘            ▼        └───┬───┘
        │               │              ┌───────┐        │
        │               └─────────────▶│ graph │◀───────┘
        │                              └───┬───┘
        │                      ┌───────────┴────────┐
        │                      ▼                    ▼
        │                 ┌────────┐           ┌────────┐
        │                 │ tuning │           │  dsp   │
        │                 └───┬────┘           └───┬────┘
        ▼                     ▼                    ▼
   ┌──────────────────────────────────────────────────────┐
   │                         core                         │
   └──────────────────────────────────────────────────────┘
```

Arrows point at dependencies. A module may only include headers from modules below it. **No cycles.** `jit` sits above `engine` (T1b.1): it produces `engine::ProgramImage`s, so it includes engine headers; the engine never includes `jit` — whoever owns the compiler worker (the app) hands compiled images to the engine (QUESTIONS Q63). `ui` does not depend on `document` or `engine`. `views` (T3.7) composes `ui` + `document` (+ `tuning`, `core`) and never depends on `engine`: it reaches the transport through the plain-data `TransportPort` interface, which `app` implements (a wall-clock `SimulatedTransport` until T3.17 puts it over the engine). `app` depends on `views` + `engine`.

| Module | CMake target | Namespace | Contents |
|---|---|---|---|
| `src/core` | `pq_core` | `pq` | RT-safe primitives: SPSC/MPSC queues, pools, arena, epoch reclamation, allocation trap, denormal guard, aligned buffers, fast math approximations, `Ticks`/time types, stable IDs, non-RT logging plus an RT log ring |
| `src/tuning` | `pq_tuning` | `pq::tuning` | EDO, MOS (port of mos-explorer), Scala, JI (ratios, monzos, naming), pitch identities, resolution to Hz, the tuning-as-signal coefficient form |
| `src/dsp` | `pq_dsp` | `pq::dsp` | Native leaf-atom kernels (scalar + SIMD), FFT, analysers ported from PQ Meter (spectrum grid, NSDF pitch, LUFS…), sample-rate conversion, warp engines later |
| `src/graph` | `pq_graph` | `pq::graph` | Patch IR, patch-language parser/printer, inference (rate/kind/poly/width), optimiser, scheduler, `Program` description (the interpreter backend is in `engine`: it builds a ProgramImage) |
| `src/jit` | `pq_jit` | `pq::jit` | LLVM IR generation from a schedule into an `engine::ProgramImage` (depends on `engine`), ORC LLJIT, AOT compilation of `stdlib/`, compile cache |
| `src/engine` | `pq_engine` | `pq::engine` | Audio thread runtime: ProgramImage ABI, interpreter backend, Program execution, voice allocation, transport, hot swap + state transfer, parameter blocks, taps for meters, audio device I/O (RtAudio), MIDI I/O, offline renderer |
| `src/document` (headers `pq/document/`) | `pq_doc` | `pq::doc` | Immutable project model (tracks, clips, notes, devices, tunings, automation), commands, undo/redo, serialization (JSON + `.pqp`), project → graph lowering |
| `src/ui` | `pq_ui` | `pq::ui` | Window (SDL3), Vulkan renderer, draw lists (ImDrawVert-compatible), text (FreeType/HarfBuzz), widget toolkit, theme tokens, offscreen screenshot path |
| `src/ui/scenes` | `pq_ui_scenes` | `pq::ui` | Named test scenes (`pq/ui/scenes.h`: `--screenshot`, goldens, ui benchmarks, `renderSceneImage`). Linked by `pq_app`, tests and benches; not part of the toolkit. The fonts, theme and textures they paint with (`pq/ui/scene_textures.h`) stay in `pq_ui` |
| `src/views` | `pq_views` | `pq::views` | The DAW's views (T3.7+): `ViewContext` (owns the `DocumentStore`, global actions, view state), `TransportPort`, the main-window shell (control bar, track-area/detail/browser slots, info view, status line), arrangement, mixer, piano roll, MOS wheel (one subtree each), view scenes for screenshots |
| `src/app` | `pq_app` (lib) + `pqdaw` (exe: `main.cpp` only, so tests can link the CLI/commands) | `pq::app` | Main, CLI, the window loop, `ShellApp` (views in the window: SDL file dialogs, text input, title, idle/first-frame measurement), `TransportPort` implementations, later the engine link (T3.17) |
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
// core: realtime-safe single-producer/single-consumer ring (power-of-two capacity; the producer- and consumer-written
// index blocks are kHotLineSeparation = 128 bytes apart, outside each other's adjacent-line prefetch pair)
template <class T, std::size_t HotLineSeparation = kHotLineSeparation> class SpscRing {
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
}

// jit (pq/jit/compiler.h, T1b.1): the LLVM backend: same ProgramImage ABI and results as the interpreter (bit-identical
// with CompileOptions::contract off; ≤ 5.4e-7 on the corpus with fmul+fadd contraction on, the default). One generated
// function per stage; element-wise runs fused into loops (8 frames per <8 x float> for global ops, one frame of 8 voices
// for voice ops; long runs interleave 8 independent chunks per iteration); per-sample loops, Control regions and solve
// bodies inline, including the simple stateful atoms; delay lines, wavetable, noise, shaper, sample_* and stateful
// atoms in block regions call the dsp kernels; exact transcendentals call the kernels' own CRT/SVML functions.
// Host CPU target (AVX-512 where present), a light IR pipeline by default (src/jit/src/codegen.h, compiler.cpp).
namespace pq::jit {
  class Compiler {                                 // one per process; owns LLJIT; images may outlive it
  public:
    static Result<std::unique_ptr<Compiler>> create();
    Result<std::unique_ptr<engine::ProgramImage>> compile(const graph::Schedule&, const CompileOptions& = {},
                                                          CompileStats* = nullptr);  // not RT-safe
  };
}

// engine: what the audio thread runs (pq/engine/program_image.h, interpreter.h, image_runner.h; T1.7)
namespace pq::engine {
  class ProgramImage {                             // interpreted or JIT-compiled, same ABI
  public:
    virtual void process(ProcessContext&) noexcept = 0;   // one block; no allocation, no locks
    virtual StateLayout const& stateLayout() const noexcept = 0;  // for state transfer by NodeId
    virtual const ImageInterface& io() const noexcept = 0;  // slots: root ins, outs/exports (global, voice), params
    virtual StateArena stateArena() noexcept = 0;            // global arena + one block per voice group
    virtual uint64_t rejectedBlocks() const noexcept = 0;    // blocks above io().maxBlockFrames (outputs zeroed)
  };
  // The interpreter backend lives in engine (graph may not include engine): flat records {KernelFn, KernelArgs}
  // with every pointer resolved here (allocation happens here, never on the audio thread).
  Result<std::unique_ptr<ProgramImage>> buildInterpreted(const graph::Schedule&, const InterpretOptions& = {});
  class ImageRunner;  // AudioCallback (BlockAdapter's engine side) → ProgramImage::process, for one global patch
  class VoiceEngine;  // note events → voice lanes, voice.* buffers, mixing, stealing, voice end (T1.8, voices.h)
  class ProgramHost { // T1.9 (program_host.h): owns the running image; hot swap with state transfer + crossfade
  public:
    Result<PublishInfo> publish(std::unique_ptr<ProgramImage>, const SwapPolicy& = {}, const VoiceSlots& = {});  // non-RT
    size_t collect();                          // non-RT: frees retired images (EpochReclaimer)
    void process(const AudioBlock&) noexcept;  // audio thread (an AudioCallback); swaps at image-block boundaries
  };
  class Engine {   // T3.17 builds it around ProgramHost
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
                                            // output only), AsioDevice (own ASIO host, duplex), RtAudioDevice
                                            // (RtAudio WASAPI shared, for comparison and input)
    virtual Result<std::vector<DeviceInfo>> enumerate();                       // non-RT virtuals
    virtual Result<StreamInfo> open(const StreamConfig&, AudioCallback);
    virtual Status start(); virtual Status stop(); virtual const CallbackStats& stats() const noexcept;
  };
  class CallbackRunner;   // the one per-callback path every backend calls: RtScope + DenormalGuard + rdtsc stats
  class BlockAdapter;     // device buffer of any size → engine at a fixed internal block (direct when divisible, else FIFO +block latency)
  class UnderrunTracker;  // per device wake: lateness vs the wall-clock schedule, empty shared buffer, device clock past
                          // frames written → CallbackStats::outputUnderflows (one per episode) + recordWake (lateness, queue)
  void planarToInterleaved(...);  // planar float → interleaved float32/int32/24-in-32/int24/int16 (AVX2 mono/stereo)
  ToDeviceFn asioToDeviceFn(type); FromDeviceFn asioFromDeviceFn(type);  // per-channel ASIO conversions (AVX2)
}
```
`Backend::Wasapi` is our WasapiDevice (the default); `Backend::RtAudioWasapi` keeps RtAudio's WASAPI for comparison and input. WasapiDevice's audio thread joins MMCSS "Pro Audio" once, waits on the device event, and per event runs the callback once per period on planar scratch, then converts once into the device buffer (no ring).
`Backend::Asio` is AsioDevice (T0.6-fix2): a clean-room host of the ASIO driver ABI (`asio_abi.h`; no Steinberg SDK). A control thread per device (COM STA, hidden window, message pump) makes every non-audio driver call and handles driver requests (reset / buffer-size / rate change → stop, dispose, release, reload, recreate, restart; latencies re-queried) that the RT-safe message callback only flags. The driver's thread calls bufferSwitch(TimeInfo) → convert inputs → CallbackRunner → convert outputs into the driver's half (float32 channels zero-copy) → outputReady(). One driver per process (the ABI's callbacks carry no context).
The engine's process entry will be an `AudioCallback` behind a `BlockAdapter`; `ProcessContext` frame counts are therefore always the fixed internal block on the realtime path.

### ProgramImage ABI (shared by interpreter and JIT)
- `ProcessContext` carries: frame count (≤ max block), sample rate, transport snapshot, pointers to input/output channel buffers, the parameter block (`float*`, indexed by `ParamSlot`), the event list for this block (sorted, preallocated), and the voice table.
- Concretely (pq/engine/program_image.h, T1.7): global signals planar (`frames` floats per slot: root `in`s, `out`s/`export`s, track.in, sidechain.in, send bus channels); voice signals per 8-lane group in the kernel layout (`frames · 8`, voices interleaved): `VoiceGroupIO { laneMask, signals[VoiceSignal] (voice.*), in[], out[] }`, `VoiceTable { groups, numGroups, lastGroup/lastLane }`. Bools are lane masks everywhere. Param-rate inputs are read at frame 0. The image clamps params to their declared ranges every block (Q44). Slot meanings come from `io()` (`ImageInterface`: global/voice in and out slots with node, channel, rate; `ParamSlot`s; sample rate; maxBlockFrames; maxVoiceGroups). process() runs global stages once and voice stages once per group (`numGroups`) **whose `laneMask` is non-zero** (T1.8: a group without voices is skipped: its state holds, its voice outputs are not written; the JIT must do the same, so voice cost is per occupied group); reducers combine the groups' active lanes; callers run it under `DenormalGuard` (CallbackRunner does). voice.* buffers as the VoiceEngine fills them: `Hz`, `Cents` (float) and `Gate`, `Trigger` (masks) are audio rate (`frames · 8`); `Velocity`, `Release`, `Ratio`, `Harmonic`, `UnisonCount`, `Index` are param rate, 8 floats only (frame 0). `ProcessContext::events` (`pq/engine/note_events.h`) is never read by an image.
- All Program state lives in **one arena** allocated when the image is built (plus one block per voice group for voice state). The state layout (`graph::StateLayout`, T1.5) maps `(TypedPatch::stateKey, channel, arena) → (offset, size, type tag, StateInit)`; the tag is (kernel, lanes, ring frames for delay lines). Hot swap copies state for every key whose tag and size match (SPEC §7.5). Given the previous layout, `schedule()` keeps every unchanged entry at its old offset (new entries fill holes, then append; compacted when holes exceed 2× the live size + 4 KiB), so the transfer is a few coalesced memcpys.
- Parameter values live in a double-buffered block written by the engine from `EngineCommand`s. Programs read them and never write them.

### Voices and note events (engine, T1.8: `pq/engine/note_events.h`, `voices.h`)
- **Events**: `NoteEvent { frame, kind (NoteOn | NoteOff | Pitch | AllNotesOff | AllSoundOff), noteId, hz (float32, resolved upstream), velocity (note-on; release velocity for note-off), ratio, harmonic, unisonCount, unisonIndex, rampFrames }`. `EventBuffer` (preallocated; RT-safe `push` keeps frame order, ties in push order; `clear`) exposes an `EventList { events, count }` for `ProcessContext::events`. Producers (T3.16 sequencer, MIDI, JI objects) push events with frame offsets inside the block; a frame ≥ the block length is clamped. `Pitch` moves a note to a new Hz as a step or as a glide linear in cents over `rampFrames` (the hook for T2.6 and pitch curves).
- **VoiceEngine** drives one ProgramImage: `prepare(image, VoiceConfig)` off-thread; `process(ProcessContext&)` (RT-safe) allocates voices from `ctx.events`, renders the voice.* buffers, fills `ctx.voices`, runs the image, mixes each voice `out` slot as `Σ lanes gain · out` (`mix(i)`), ends voices and compacts. `VoiceConfig { maxVoices (polyphony; lanes above it are fade headroom: voiceGroupsFor(n) = ceil(n/8) + 1 groups), steal (Oldest | Quietest | None), retriggerSameNote, resetOnNoteOn, compact, stealFadeMs, silenceDb/silenceMs, releaseTimeoutMs, referenceHz (voice.cents), monitorMask, doneSlot (findVoiceSlot(io, patch, "done")), maxEventsPerBlock }`. Inspection: laneStatus/laneOf/liveVoices/activeGroups/groupsRunLastBlock, signal(g, VoiceSignal), gain(g), stats().
- **Sample accuracy without sub-blocks**: an event at frame k changes the per-sample voice.* buffers at k (gate, trigger, hz, cents) and the lane gain (0 before the onset); param-rate voice signals take the lane's latest value for the whole block. A lane starting in a block has its state reset to StateInit at the block start (generic: per-lane words of every voice `StateEntry`, from a template built with `dsp::initState`; noise counters and delay-line headers are shared and kept) and reads hz 0 until the onset, so its output from k on does not depend on block boundaries.
- **Allocation**: best fit (a free lane of the fullest non-full occupied group, else the lowest empty group), plus compaction (`VoiceConfig::compact`, default on): at a block end, when the occupied groups exceed ceil(occupied lanes / 8), the least occupied group's voices move to free lanes of other groups (per-lane state words copied; delay rings re-aligned to the destination group's write position; noise state stays with the group). This relies on the voice state layout rule of `atom_state.h` (per-lane fields are [8] arrays, word ≡ lane mod 8; shared words only in the noise counter, delay header and phasor incScale/halfSr), which every backend's StateArena must keep. Stealing above maxVoices: released before held, then oldest or quietest (peak of the monitored outs over the last block); the victim fades linearly over stealFadeMs in its lane (engine-mixed outs only) while the new note starts in a free lane; with no free lane it waits for the fade (deferred start at the next block's frame 0). Same-note retrigger (equal Hz) takes over a held/released voice without a reset.
- **Voice end** (checked at block end): a `done` voice out/export (true or > 0.5 at the last frame), the monitored outs below silenceDb for silenceMs after release, or releaseTimeoutMs after release when there is neither. Internal `voice_*` reducers see lane masks only (block granular): QUESTIONS Q61.
- `ImageRunner::setVoices(VoiceEngine*, EventSource, capacity)`: the runner calls the event source per image block, then the VoiceEngine; an image without global `out`s plays the voice mixes (`pqdaw --play <instrument> --notes "60 64 67"`).
- **Image bindings (T1.9)**: everything image-dependent (voice out slots, mix buffers, lane reset/move spans, the image's voice state) is a `VoiceBinding`. `prepareLanes(io, cfg)` sets up lanes and voice.* buffers for a geometry (maxVoiceGroups, maxBlockFrames, sample rate); `makeBinding(image, doneSlot, monitorMask)` (non-RT) binds any image of that geometry; `attach(current, fadingOut)` (RT) switches at a block boundary, so lanes, notes and allocator state survive a program swap. While a crossfade runs, `process(ctx, fadeCtx)` runs both images on the same voice table, lane resets and compaction moves apply to both, `fadeMix(i)` holds the old image's mixes. `prepare(image, cfg)` = prepareLanes + makeBinding + attach.

### Hot swap (engine, T1.9: `pq/engine/program_host.h`, `state_transfer.h`)
- **ProgramHost** owns the running ProgramImage (any backend: it uses only the ABI). `publish(image, SwapPolicy{mode: Auto | Instant | Crossfade, fadeMs}, VoiceSlots)` (non-RT, internally serialised) builds everything that allocates — the transfer plan against the previously *published* image, the param remap (new slot ← old slot with the same `ParamSlot::key`), output scratch, the VoiceBinding — and pushes a packet through an SpscRing. The audio thread pops at most one packet per image block, never during a crossfade, so packets become current in publish order and each plan's source is exactly the image it replaces. Images must have the host's sample rate and maxBlockFrames (and voice geometry); `lastPublished()->stateLayout()` is the next compile's `ScheduleOptions::previous`. Device mapping as ImageRunner (input c → global root in c; output c ← c-th global `out`, mono on all, else the voice mixes).
- **State transfer** (`buildTransferPlan` off-thread, `executeTransfer` RT): a new `StateEntry` takes the old entry with the same (stateKey, channel, arena) when tag and size match; else it keeps StateInit (`TransferPlan::matched/fresh/mismatched/dropped`). Ranges contiguous in both arenas are coalesced (with T1.5's stable offsets an edit is usually one range per arena). It runs on the audio thread at the swap, right before the new image's first block, from the old image's current state; voice ranges are copied per group, only for occupied groups when `resetOnNoteOn` (free lanes are reset at note start anyway). `ProgramImage::stateLayout()`/`stateArena()`/`io()` must be callable from the publishing thread while the image runs (they return immutable data / fixed pointers). State keys come from NodeIds, so edits must be made on the Patch IR (ids kept); re-parsing edited text renumbers nodes (declarations first) and loses the match.
- **Swap at the block boundary**: audible (Auto) = transport playing, or any voice lane in use, or the last block's played output above `HostConfig::silenceDb` (−90 dBFS; read from the old image's scratch, no per-block metering). Audible → **linear** crossfade over fadeMs (clamped 5–20 ms): both images run on the same inputs/events/voice table, `out = old + g·(new − old)`, g = (k+1)/N, exactly `new` from g = 1 (bit-exact where old == new: an unchanged path passes untouched; linear because the two programs are correlated, equal-power would bulge up to +3 dB). Silent → instant. Params carried, VoiceEngine re-attached.
- **Reclamation**: the audio thread only advances an atomic "retired through" sequence (instantly, or when the fade ends) and calls `EpochReclaimer::quiescent` at the end of every `process()`; `collect()` (and `publish()`) retire those packets to the reclaimer, which frees them after the next quiescent state. The audio thread never frees. `audioOffline()/audioOnline()` while the stream is stopped.

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
- **Redraw**: `markDirty()` adds the widget's clipped rect to the tree's `Damage`; layout adds the old and new rects of whatever moved. `requestFrame()` / `requestTimer()` are the only ways to animate; idle means no requests, and the loop blocks until the next event or `nextWakeTime()`. **Partial redraw (T0.7-fix1)**: the frame loop reads `damage().rects()` after layout, paints with `UiTree::paint(dl, res, now, region)` where region = their bounding box (only widgets intersecting it paint; `PaintContext::clip` starts as region ∩ target, so virtualised views draw only that range) and renders the list with those rects as the frame's damage. Contract for widgets and views: **paint only inside your rect** (push it as a clip if geometry could spill, e.g. text, glow, AA feathers past the edge) or declare the overflow with `setVisualOverflow(Insets)` (popup drop shadows do: damage and culling then use the grown rect), and report every visual change with `markDirty()` (or layout); anything drawn outside, or changed without damage, goes stale on screen.
- **Input**: SDL-free `PointerEvent` / `KeyEvent` / `TextEvent` (`translateEvent` in `pq/ui/window.h` adapts SDL). A handled Down captures the pointer until every button is released; hover is frozen while captured. Keys go: focused widget → its ancestors (a focused text-entry widget swallows printable keys) → modal scopes (newest first; `ScopeRules` passthrough modifiers / swallow unbound, which is how macro mode works) → `keyScope`s of the focus chain → global → Tab traversal. `ActionRegistry` holds string ids → handler + enabled predicate; a `Keymap` is data (scope, chord → action id) and is resolved to indices in `UiTree::setKeymap`. Dispatch does not allocate.
- **Tests**: `TestDriver` builds a tree at a size and scale, injects events, runs a fake clock through update → paint, and renders through `OffscreenTarget`.

### Theme, controls and overlays (ui, T3.2: `pq/ui/theme.h`, `controls.h`, `overlay.h`, `keymap_file.h`)
- **Theme as data**: `Theme` (surface/control/field/text/accent/selection/menu/tooltip/track/clip/grid/scrollbar colours, font roles 12/13/14, spacing, radii, sizes, 70-colour clip palette) is read from `assets/themes/ableton-dark.json` by a strict reader (a missing/unknown/duplicate token is an error naming it; `serializeTheme` writes the canonical file). `SceneTextures` owns it (`theme`, `uiFonts` = the theme's font roles resolved at the scale, re-resolved by `setSceneScale`/`setSceneTheme`); widgets read `PaintContext::res.theme` / `res.uiFonts` (plain member loads). Dev builds hot-reload the file (`ThemeFileWatcher` compares its write time; the window loop runs that check only when `app`'s `FileChangeNotifier` — a helper thread blocked on the directory's change-notification handle — posts an SDL event, so idle does no periodic work; T3.7 review). PQ Meter's palette stays in `signal_theme` and only signal displays use it (meters, modulation rings).
- **Controls** derive from `Control` (tooltip, info text, `VisualState` normal/hover/pressed/focused/disabled, `setStateOverride` for galleries): `Label`, `Button`, `Toggle` (Accent/Activator/Solo/Arm), `ValueControl` → `DragBox` (Live's number box), `Fader`, `Knob` (`ModulationRing` hook), `TextEdit` (on `TextBuffer`), `Dropdown`, `Scrollbar` (a `ScrollView::Dependent`), `Meter` (`MeterLevels` plain struct, repaints only when a drawn pixel changes). A value edit is a gesture: `onGestureBegin` (lazily, at the first change) → `onChange`* → `onGestureEnd`, one per drag / type-in / reset / key = one undo step (T3.4 `History` gestures).
- **Overlays**: an `OverlayLayer` as the root's last child hosts popups (`Menu` with nested submenus and keyboard navigation, `Popover`, tooltips); it is hittable only while a dismissable popup is open, so a press outside closes them and is consumed. Popups take and restore focus; closed popups are destroyed at the next frame.
- **Tree services** (`UiTree`): `setResources` (what widgets measure text with outside paint), `clipboard()` (`SdlClipboard` in the app, in-process otherwise), `TreeObserver`s (hover changes, presses: tooltips, the T3.7 info view via `infoFor(hovered)`), `focusVisible()` (focus came from the keyboard), `actions()`. Widgets expose `info()` / `tooltip()`. Custom layouts are arranged even without children.
- **Keymaps**: `assets/keymaps/ableton.json` (scope, keys, action id, status `implemented` | `deferred:M<n>`); `applyKeymapFile` binds implemented entries and fails on an unregistered one. `docs/HOTKEYS.md` lists every binding.

### Views and the app shell (views, T3.7: `pq/views/view_context.h`, `transport_port.h`, `shell/shell.h`, `view_scenes.h`)
- **Directory layout** (one subtree per view family so parallel tasks do not collide): `include/pq/views/` shared API; `shell/` (T3.7), `arrangement/` (T3.8–T3.10, T3.15), `mixer/` (T3.10), `piano_roll/` (T3.11–T3.12), `mos_wheel/` (T3.13–T3.14) under both `include/pq/views/` and `src/`; `src/common/paint.h` private drawing helpers. A subtree lists its sources in `src/<subtree>/sources.cmake` (`set(PQ_VIEWS_<SUBTREE>_SOURCES …)`), included by `src/views/CMakeLists.txt` when present.
- **`ViewContext`** (UI thread): owns the `DocumentStore`; views edit only through commands (`pq::doc::ops` builders, `dispatch`, `begin/commitGesture` for drags = one undo step) and subscribe with `ctx.subscribe(listener)` (document `StoreEvent`s; the context's own listener runs first) and `ctx.observe(fn)` (`view_change` bits: transport, grid, layout, project, status); both return RAII `Subscription`s, and the context must outlive every widget holding one. It also holds the view state outside the document: start marker, the shared `GridSpec` (+ the focused editor's resolved division for the grid display), detail view visible/mode, browser visibility, status line. Project I/O: `newProject` / `openProject(dir)` / `saveProject` / `saveProjectAs(dir)`; `dirty()` compares the current snapshot pointer with the last saved/loaded one (undo back to it is clean). The `file.*` actions go through `FileDialogs` (the app's SDL dialogs; tests script it), with save/discard/cancel on unsaved changes.
- **Actions and keymap**: the context registers the global actions (`transport.playStop`, `transport.continue`, `edit.undo`, `edit.redo`, `file.new/open/save/saveAs`, `view.toggleClipDevice`, `view.toggleDetail`); views register theirs in `ctx.actions()` when installed and set `ctx.scope("arrangement")`-style key scopes (ids stable for the context's lifetime). `applyKeymap` rebinds from the keymap file and can be repeated.
- **`TransportPort`**: `play(from)`, `stop()`, `continuePlayback()`, `locate(t)`, `setLoop({start, end, enabled})`, `playhead()` → `PlayheadSnapshot {playing, tick, sequence}`. Plain data; views read the playhead only while playing (the control bar's position readout wakes on a timer at the next sixteenth, 4–100 ms, and damages only its own rect). The context keeps the port's loop equal to the document's `LoopRegion`. `FakeTransport` (views, header-only) records calls for tests.
- **Shell** (`Shell::create(tree, ctx)`): root = backdrop + `main` (Column: `control-bar`, `body` splitter [`top`: `browser` slot (hidden until M5), `track-area` slot (+ hidden `mixer` slot); `bottom` splitter: `info` view | `detail` (`clip` / `device` slots)], `status-bar`) + `overlay` (last child). `ShellSlot` names the hosts; `installDefaultContent(shell)` (src/views/src/shell/default_content.cpp) is where each view installs itself (one line per view). The shell binds the keymap to the tree and feeds the info view from a `TreeObserver` (`infoFor(hovered)`). Every shell widget paints inside its rect and reports changes through `markDirty`/layout, so the window redraws only damage (T0.7-fix1).
- **View scenes** (`view_scenes.h`): a table of named scenes (`shell`, later `arrangement-basic`, …) = the shell over a project plus optional setup; `renderViewScene(scene, projectDir, w, h, scale)` renders one headless (`pqdaw --screenshot <scene> out.png --project <dir>`, the goldens in `tests/views/golden/`).
- **App** (`src/app/src/shell_app.*`): the window loop treats the shell as a widget tree and calls `ShellApp::handleEvent` (SDL file-dialog results marshalled through a user event, quit confirmation, `SDL_StartTextInput`/`StopTextInput` following text focus; Esc does not quit), `onFramePresented` (first-frame log against `main()` entry and process creation; `--idle-check`) and `exitCode`. `pqdaw --project <dir>` opens it; `--scene shell` starts with a new project.

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
- **CPU baseline check** (`pq/core/cpu_baseline.h`, T0.1-fix1): `/arch:AVX2` code may use the whole x86-64-v3 set (AVX, AVX2, FMA, BMI1/2, LZCNT, F16C, MOVBE). `pq_add_executable` links the OBJECT library `pq_cpu_check`: a CRT initialiser in `.CRT$XCB` that runs before every C++ static initialiser and `main`, and exits 1 with a message naming the missing features. It and the query it calls (`pq_cpu_baseline`, archived into `pq_core`) are C (no inline code shared with `/arch:AVX2` C++), compiled without `/arch:AVX2` and without `/GL` (`pq_target_baseline_defaults`). Test hook: `PQ_TEST_CPU_DISABLE=avx2[,fma,…]` forces features off in `pq_query_cpu_baseline()`, and so in `cpuFeatures()`.
- **Allocation trap** (`pq/core/alloc_trap.h`): the OBJECT library `pq_alloc_trap` replaces global `operator new/delete`. `pq_add_test` links it into every test executable (both presets), where an allocation or free inside an `RtScope` fails the running doctest case; use `pq::alloc_trap::countRtViolations([&]{ … })` to assert that an RT entry point does not allocate. `pq_add_executable` links it only when `PQ_ALLOC_TRAP_IN_APPS` is ON (default ON for dev, OFF for release), where a violation prints a stack trace and aborts. `malloc`/`free` and OS allocators are not interposable under `/MT` and are not caught.
- **Third-party code** comes in via `FetchContent` as release archives pinned by SHA-256 (`URL_HASH`), so configuring needs no VCS, and is recorded in `docs/DEPENDENCIES.md`. All of it is linked statically (`BUILD_SHARED_LIBS` is forced OFF). Only permissive licences: MIT, BSD, zlib, Apache-2.0, Boost, MPL-2.0 (file-level), OFL for fonts. No GPL; LGPL only if dynamically linked and justified.

## 6. Conventions
- Files: `snake_case.h/.cpp`. Types: `PascalCase`. Functions and variables: `camelCase`. Constants: `kPascalCase`. Members: `trailingUnderscore_`.
- Headers: `#pragma once`; public headers in `src/<module>/include/pq/<module>/…`; private code in `src/<module>/src/…`.
- Errors: `pq::Result<T>` (value or `pq::Error{code, message, location}`) on non-RT paths. Exceptions are allowed only at the outermost non-RT boundaries (file I/O, parsing), and are caught there. Everything reachable from the audio thread is `noexcept`.
- No global mutable state except explicit singletons in `app/` (JIT compiler, audio device manager).
- Comments explain *why*. Every RT-path function says in its header comment that it is RT-safe.
