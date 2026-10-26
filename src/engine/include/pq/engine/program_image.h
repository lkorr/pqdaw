#pragma once

// The audio-thread interface of a compiled patch (T1.7, ARCHITECTURE §3 "ProgramImage ABI"). One ABI for every
// backend: the interpreter (interpreter.h, T1.7) and the LLVM JIT (T1b) both produce a ProgramImage from a
// graph::Schedule, and the engine (voices T1.8, hot swap T1.9, offline render T1.10) only ever talks to this.
//
// ---- One block -----------------------------------------------------------------------------------------------
// process(ctx) runs the whole program once for ctx.frames frames (1 ≤ frames ≤ io().maxBlockFrames; 0 does
// nothing; more is rejected: every output is zeroed and rejectedBlocks() counts it). It is RT-safe: no allocation,
// locks, syscalls or exceptions, one virtual call per block (never per op or per sample). Callers run it with
// FTZ/DAZ set (pq::DenormalGuard; CallbackRunner does this for device callbacks).
//
// ---- Buffers in the context ----------------------------------------------------------------------------------
// Every signal buffer is float32. Global signals are planar: `frames` floats per channel. Voice signals are one
// buffer per voice GROUP (8 voices, the AVX2 lanes): `frames · 8` floats, frame-major with the voices interleaved
// (sample s of lane v at [s·8 + v]) — the kernel layout (pq/dsp/kernel.h), so the program reads and writes them
// in place without copies. Bool signals (voice.gate, a `bool` root input or output) are lane MASKS as in the
// kernels: all bits set = true, +0.0f = false (dsp::boolMask). A param-rate input (a root `in` inferred param, or
// a param-rate voice.* signal such as voice.velocity) is read at frame 0 of the block: its value is constant over
// the block (for voice signals frame 0 of the group buffer is exactly the 8-float per-voice pattern).
// Missing inputs (null pointers, or slots beyond the counts given) read silence / false; missing outputs are
// skipped.
//
// ---- Slots ---------------------------------------------------------------------------------------------------
// io() lists what each slot index means: root `in`s (global and voice), `out`s and `export`s (global and voice),
// one entry per channel, and the parameter slots (graph::ParamSlot, indexed like ProcessContext::params). Params
// are clamped by the program to their declared ranges every block (docs/QUESTIONS.md Q44: loop sub-blocks rely on
// it); NaN reads the default; bool params are any nonzero = true.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pq/graph/schedule.h"

namespace pq::engine {
struct InspectBinding;
}

namespace pq::engine {

// voice.* sources (SPEC §7.2 "Events and voices"), the index into VoiceGroupIO::signals.
// Degree / Equave (T2.6): a Degree note's degree and equave in its tuning signal (NaN for other notes), the inputs of
// the tuning node (`tuning_hz`). Tuning (T2.6-fix3): which tuning node of the track patch the note follows (1, 2, …;
// 0: a static pitch).
enum class VoiceSignal : uint8_t {
  Hz, Cents, Gate, Trigger, Velocity, Release, Ratio, Harmonic, UnisonCount, Index, Degree, Equave, Tuning, Count_
};
inline constexpr uint32_t kVoiceSignalCount = static_cast<uint32_t>(VoiceSignal::Count_);

// The transport at frame 0 of the block (transport.* atoms). beat and barPhase are audio rate:
//   beat(t)     = beat + t · beatsPerFrame
//   barPhase(t) = frac((beat(t) − barStartBeat) / beatsPerBar)
// computed in double per frame, rounded to float32.
struct TransportSnapshot {
  double bpm = 120.0;
  double beat = 0.0;           // position in beats at frame 0 of this block
  double beatsPerFrame = 0.0;  // 0 while stopped
  double beatsPerBar = 4.0;    // > 0
  double barStartBeat = 0.0;   // a bar line (bar phase 0) at this beat
  bool playing = false;
};

// One voice group (8 lanes) for this block, filled by the VoiceEngine (voices.h, T1.8) or a test harness.
// A group whose laneMask is 0 is NOT run (T1.8): its state does not advance, its voice outputs are not written and
// it contributes nothing to reducers, so the cost of the voice stages is per occupied group, whatever numGroups is.
// voice.* buffers as the VoiceEngine provides them: Hz, Cents (float), Gate, Trigger (lane masks) audio rate
// (frames · 8); Velocity, Release, Ratio, Harmonic, UnisonCount, Index, Degree, Equave param rate (8 floats: only
// frame 0 exists).
struct VoiceGroupIO {
  uint32_t laneMask = 0;                // bit v: lane v holds a voice (reducers and sends read only these); 0: skip
  const float* const* signals = nullptr;  // [kVoiceSignalCount] voice.* buffers (frames · 8), or null entries
  const float* const* in = nullptr;     // voice root `in` slots (io().voiceIn), frames · 8 each
  uint32_t numIn = 0;
  float* const* out = nullptr;          // voice `out`/`export` slots (io().voiceOut), frames · 8 each
  uint32_t numOut = 0;
  // Per-lane, per-sample gain (frames · 8; the VoiceEngine's: 0 before a note's onset and while free, the steal fade),
  // or null. Audio-rate voice_sum and voice sends add select(gain ≠ 0, x, 0) · gain over the laneMask lanes, and
  // voice_last reads its lane only at frames where its gain ≠ 0 (else the reduction's 0), so a pre-onset or idle
  // lane's NaN never leaves the group (Q61 option A). voice_highest / lowest / loudest and bool reductions keep the
  // block's laneMask (their comparisons skip NaN). Null: the laneMask alone (block granular).
  const float* gain = nullptr;
};

inline constexpr uint32_t kNoVoice = 0xFFFFFFFFu;

struct VoiceTable {
  const VoiceGroupIO* groups = nullptr;
  uint32_t numGroups = 0;               // groups run this block (≤ io().maxVoiceGroups; extra ones are ignored)
  uint32_t lastGroup = kNoVoice;        // the most recently started voice (voice_last), or kNoVoice
  uint32_t lastLane = 0;
};

// A send bus (the `send(x, bus)` atom adds into it; the caller zeroes it before the block).
struct SendBus {
  float* const* channels = nullptr;     // frames floats each
  uint32_t numChannels = 0;
};

struct EventList;  // pq/engine/note_events.h (T1.8): the block's sorted note events; read by the VoiceEngine,
                   // never by a ProgramImage

struct ProcessContext {
  uint32_t frames = 0;
  double sampleRate = 0.0;              // informational: the program is specialised for io().sampleRate
  int64_t sampleTime = 0;               // engine frames before frame 0 of this block
  TransportSnapshot transport;
  const float* const* in = nullptr;     // global root `in` slots (io().globalIn)
  uint32_t numIn = 0;
  float* const* out = nullptr;          // global `out`/`export` slots (io().globalOut)
  uint32_t numOut = 0;
  const float* const* trackIn = nullptr;      // track.in channels
  uint32_t numTrackIn = 0;
  const float* const* sidechainIn = nullptr;  // sidechain.in channels
  uint32_t numSidechainIn = 0;
  const float* params = nullptr;        // the parameter block, indexed by ParamSlot (null: defaults)
  uint32_t numParams = 0;
  const EventList* events = nullptr;
  const VoiceTable* voices = nullptr;   // null or 0 groups: voice stages do not run (voice reducers read 0)
  const SendBus* sends = nullptr;       // indexed by bus number
  uint32_t numSends = 0;
  // ---- Silence (T1.x-silence, SPEC §7.7; optional) ----
  // In: per global in slot (io().globalIn), 1 = the caller knows the slot's buffer is exactly ±0 over this block (a
  // silent bus), so the image need not look at it. Null: the image checks what it needs.
  const uint8_t* inSilent = nullptr;
  // Out: per global out slot (io().globalOut), set by process() to 1 when the slot's data of this block is exactly ±0.
  uint8_t* outSilent = nullptr;
  // Written by process() (false when it refused the block):
  bool tailsDone = false;  // every sleepable region of the image is asleep: no tail is in progress
  bool dormant = false;    // tailsDone, every global out silent, no voice group ran and nothing else in the image keeps
                           // state: the next block, with silent inputs and the same params, would change nothing
};

// T1.x-silence (SPEC §7.7): what an image does with its schedule's sleep plan (pq/graph/schedule.h "Sleep plan";
// pq/engine/image_host.h "sleep"). On: sleeping regions are skipped and their outputs zero-filled. Off: everything
// always runs. Shadow (tests): decisions and tail flushes as On, but every region runs; the interpreter checks that a
// region it would have skipped produced exactly the zeros its fills would have written (SleepStats::violations).
enum class SleepMode : uint8_t { Off, On, Shadow };

// The level below which a decaying tail's STATE is cut (and flushed) when a segment falls asleep: 2^-30 ≈ −180 dBFS,
// so that the output the cut residue would still have produced stays ≤ −120 dBFS through up to 60 dB of resonance or
// feedback gain (a 60 Hz low shelf, a comb at feedback 0.999; tests/stdlib's biquads measure it).
inline constexpr float kSleepEpsilon = 9.313225746154785e-10f;

struct SleepStats {
  uint64_t sleeps = 0;          // segment transitions awake → asleep
  uint64_t wakes = 0;           // asleep → awake
  uint64_t asleepBlocks = 0;    // segment-blocks spent asleep
  uint64_t flushes = 0;         // states set to 0 when falling asleep (a residue ≤ ε that was not 0 yet)
  uint64_t violations = 0;      // Shadow: a region that would have been skipped produced something other than ±0
};

// One slot of the program's interface: a root `in`, `out` or `export` channel.
struct IoSlot {
  graph::NodeIndex node = graph::kNone;  // in the scheduled patch (Schedule::rewritten when set)
  uint16_t channel = 0;
  bool isExport = false;
  graph::Rate rate = graph::Rate::Audio;  // inputs: param-rate slots are read at frame 0
  uint32_t ordinal = 0;                   // inputs: the root `in`'s ordinal (InferOptions::inputs order)
};

// Which backend built an image (informational: tests, diagnostics, the T1b.5 interpreter → JIT swap's statistics).
enum class ImageBackend : uint8_t { Unknown, Interpreter, Jit };

struct ImageInterface {
  ImageBackend backend = ImageBackend::Unknown;
  double sampleRate = 0.0;
  uint32_t maxBlockFrames = 0;
  uint32_t maxVoiceGroups = 0;
  std::vector<IoSlot> globalIn, voiceIn, globalOut, voiceOut;
  std::vector<graph::ParamSlot> params;
  bool hasVoiceStages = false;
  // T1.x-silence: frames a released voice may still sound after its outputs fell silent (the voice stages' longest
  // delay line, graph::SleepPlan::voiceTailFrames): the VoiceEngine's silence window is at least this long.
  uint32_t voiceTailFrames = 0;
};

// Where the program's state lives (for T1.9's state transfer by graph::StateLayout): the global arena and one
// block of StateLayout::voiceBytes per voice group, voiceStride bytes apart.
struct StateArena {
  std::byte* global = nullptr;
  std::byte* voice = nullptr;
  uint32_t voiceStride = 0;
  uint32_t voiceGroups = 0;
};

class ProgramImage {
public:
  virtual ~ProgramImage() = default;
  // One block. RT-safe.
  virtual void process(ProcessContext& ctx) noexcept = 0;
  // RT-safe accessors.
  virtual const graph::StateLayout& stateLayout() const noexcept = 0;
  virtual const ImageInterface& io() const noexcept = 0;
  virtual StateArena stateArena() noexcept = 0;
  // Blocks refused because ctx.frames exceeded io().maxBlockFrames.
  virtual uint64_t rejectedBlocks() const noexcept = 0;
  // T1.x-silence: the image's sleep statistics (null: the image does not sleep). RT-safe.
  virtual const SleepStats* sleepStats() const noexcept { return nullptr; }
  // T4.5 (pq/engine/inspect.h): binds the image's probes (Schedule::probes, by id) to their taps and the image to a
  // cost meter. Not RT-safe: call before the image is published (the bound taps and meter must outlive it). False:
  // the image cannot be inspected (nothing is bound).
  virtual bool bindInspect(const InspectBinding& /*binding*/) { return false; }
};

}  // namespace pq::engine
