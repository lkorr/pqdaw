#pragma once

// Hot swap (T1.9, SPEC §7.5): the engine-side owner of the running ProgramImage. A compiler thread publishes new
// images at any time; the audio thread swaps to them at a block boundary, carries the state of every surviving node
// over (state_transfer.h), keeps voices sounding (VoiceEngine bindings, voices.h), crossfades when the output is
// audible and retires the old image to an EpochReclaimer, which frees it off the audio thread.
//
// ---- Threads -------------------------------------------------------------------------------------------------
//   publish(), collect()           any non-RT thread (serialised by an internal mutex): build, hand over, free
//   process(), callback(), params(), transport(), stats()    the audio thread (or an offline render loop)
// publish() does all the work that allocates: the transfer plan against the previously published image, the param
// remap (by ParamSlot::key), the output scratch and the VoiceBinding; it then pushes the packet through an SPSC
// ring (lock-free). The audio thread pops at most one packet per image block, and none while a crossfade runs (the
// next one waits for the fade to end), so packets become current in publish order and each plan's "from" image is
// exactly the one being replaced.
//
// ---- The swap, at a block boundary on the audio thread ------------------------------------------------------
//   1. audible? (SwapMode::Auto) — the transport is playing, or any voice lane is in use, or the last block's
//      played output (the old image's output scratch / voice mixes, still in place) peaked above silenceDb.
//   2. executeTransfer(plan, old arena, new arena): memcpy of the coalesced ranges (voice ranges for the occupied
//      groups only when VoiceConfig::resetOnNoteOn is on: free lanes are reset when a note starts anyway).
//   3. param values: new[slot] = old[slot'] for every param with the same key; new params keep their defaults.
//   4. VoiceEngine::attach(new binding, audible ? old binding : null): lanes, notes and allocator state carry on.
//   5. audible: crossfade for fadeMs (clamped to SPEC's 5–20 ms) — both images run on the same inputs, events and
//      voice table, out = old + g · (new − old) with g = (k + 1) / fadeFrames linear per sample k (and exactly new
//      from g = 1 on). Linear, not equal-power: the two programs are highly correlated (an edit of one patch),
//      where linear keeps unity gain and equal-power would bulge by up to +3 dB; and old + g · (new − old) is
//      bit-exact wherever old == new, so an unchanged signal path passes through a crossfade untouched.
//      Silent: the old image is retired at once (instant swap).
//   6. retire: the audio thread advances an atomic "retired through" sequence number; collect() hands those packets
//      to the EpochReclaimer (retire), and frees them once the audio thread has passed a quiescent state (it calls
//      quiescent() at the end of every process()). The audio thread never frees anything.
//
// ---- Interface to the device ----------------------------------------------------------------------------------
// Like ImageRunner: device input channel c feeds the image's global root input slot c; device output channel c
// plays the c-th global `out` slot (exports are not played; a mono patch plays on every channel); an image without
// global `out`s plays the VoiceEngine's voice mixes the same way. Params start at their declared defaults;
// transport as set (it advances by beatsPerFrame while playing). Every image must have the host's sample rate and
// maxBlockFrames (and, with voices, maxVoiceGroups); publish() refuses others.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/core/result.h"
#include "pq/core/spsc_ring.h"
#include "pq/engine/audio_block.h"
#include "pq/engine/image_runner.h"
#include "pq/engine/note_events.h"
#include "pq/engine/program_image.h"
#include "pq/engine/state_transfer.h"
#include "pq/engine/voices.h"

namespace pq::engine {

enum class SwapMode : uint8_t {
  Auto,       // crossfade when audible (see above), else instant
  Instant,    // never crossfade (e.g. a swap known to be output-identical)
  Crossfade,  // always crossfade
};

struct SwapPolicy {
  SwapMode mode = SwapMode::Auto;
  float fadeMs = 10.0f;  // clamped to [kMinFadeMs, kMaxFadeMs]
};

inline constexpr float kMinFadeMs = 5.0f;
inline constexpr float kMaxFadeMs = 20.0f;

// Voice slots of the published image (VoiceConfig::doneSlot / monitorMask for this image's io().voiceOut).
struct VoiceSlots {
  uint32_t doneSlot = kNoSlot;
  uint32_t monitorMask = 0;
};

struct HostConfig {
  double sampleRate = 48000.0;     // every image's io().sampleRate
  uint32_t maxBlockFrames = 1024;  // every image's io().maxBlockFrames
  uint32_t numIn = 0, numOut = 2;  // device channels (≤ kMaxChannels)
  uint32_t maxPending = 8;         // packets published but not yet current (rounded up to a power of two)
  float silenceDb = -90.0f;        // SwapMode::Auto: output peaks at or below this are silent
  bool voices = false;             // run images through a VoiceEngine (prepared with the first image's geometry)
  VoiceConfig voice;               // doneSlot / monitorMask are per image (VoiceSlots)
  EventSource events;              // note events per image block (voices only)
  uint32_t eventCapacity = 1024;
};

// What publish() did (non-RT).
struct PublishInfo {
  uint64_t sequence = 0;     // 1 for the first image, then 2, 3, …
  TransferPlan plan;         // a copy of the plan the audio thread will run (empty for the first image)
  uint32_t paramsCarried = 0;
};

// Audio-thread counters (read them on the audio thread, or after it stopped).
struct HostStats {
  uint64_t swaps = 0;          // images made current (the first one included)
  uint64_t instantSwaps = 0;
  uint64_t crossfades = 0;
  uint64_t blocks = 0;         // image blocks run
  uint64_t fadeBlocks = 0;     // image blocks that ran two images
  uint64_t transferBytes = 0;  // last swap: bytes copied
};

class ProgramHost {
public:
  // Non-RT: allocates the rings and the reclaimer, registers the audio thread as its reader.
  explicit ProgramHost(const HostConfig& config);
  // Non-RT, after the audio thread has stopped calling process(). Frees every image.
  ~ProgramHost();
  ProgramHost(const ProgramHost&) = delete;
  ProgramHost& operator=(const ProgramHost&) = delete;

  // Non-RT. Hands `image` to the audio thread, to become current at a block boundary (policy as above; ignored for
  // the first image, which starts instantly). Errors: null image, a sample rate / block size / voice geometry other
  // than the host's, a plan error, too many packets pending (ErrorCode::Overflow: retry after the audio thread has
  // taken some). Calls collect() first.
  Result<PublishInfo> publish(std::unique_ptr<ProgramImage> image, const SwapPolicy& policy = {},
                              const VoiceSlots& slots = {});
  // Non-RT. Retires the packets the audio thread has let go of and frees those every reader has passed; returns how
  // many images were freed by this call. Call it periodically (the T3.17 controller), or rely on publish().
  size_t collect();
  // Non-RT. Images published and not yet freed (current + pending + retiring).
  size_t imagesAlive() const;
  uint64_t imagesFreed() const;
  // The image published last (non-RT; valid until the next publish + collect). Its stateLayout() is what the next
  // compile passes as ScheduleOptions::previous, so unchanged state keeps its offsets.
  const ProgramImage* lastPublished() const;

  // One device block (any frame count: split into image blocks of maxBlockFrames). RT-safe. Run it with FTZ/DAZ set
  // (CallbackRunner does for device callbacks; offline loops use pq::DenormalGuard).
  void process(const AudioBlock& block) noexcept;
  AudioCallback callback() noexcept { return {&ProgramHost::onBlock, this}; }

  // Audio thread. The current image's param block (indexed by its io().params), empty before the first swap.
  std::span<float> params() noexcept;
  TransportSnapshot& transport() noexcept { return transport_; }
  const HostStats& stats() const noexcept { return stats_; }
  const ProgramImage* current() const noexcept;  // audio thread
  bool crossfading() const noexcept { return fadeFrom_ != nullptr; }
  uint64_t currentSequence() const noexcept;     // audio thread
  VoiceEngine* voices() noexcept { return cfg_.voices ? &voices_ : nullptr; }

  // The audio thread stops / resumes calling process() (device stopped, render paused): lets collect() free images
  // meanwhile. Call from the audio thread, or from any thread while it is not running. RT-safe.
  void audioOffline() noexcept { reclaimer_.goOffline(reader_); }
  void audioOnline() noexcept { reclaimer_.goOnline(reader_); }

private:
  struct Packet;
  static void onBlock(void* self, const AudioBlock& b) noexcept { static_cast<ProgramHost*>(self)->process(b); }

  void trySwap() noexcept;
  void retire(Packet* p) noexcept;
  float playedPeak(const Packet& p, bool fading, uint32_t frames) const noexcept;
  const float* channelSource(const Packet& p, bool fading, uint32_t c) const noexcept;
  void prepareContext(ProcessContext& ctx, Packet& p, const float* const* in, uint32_t numIn, uint32_t frames,
                      int64_t sampleTime) noexcept;

  HostConfig cfg_;
  // Non-RT side (mutex_).
  mutable std::mutex mutex_;
  std::vector<Packet*> inflight_;  // published, not yet retired, in sequence order (owned)
  Packet* last_ = nullptr;         // the last published packet (current or pending)
  uint64_t nextSeq_ = 1;
  std::atomic<uint64_t> freed_{0};
  // Hand-over.
  SpscRing<Packet*> pending_;
  std::atomic<uint64_t> retiredThrough_{0};  // packets with sequence ≤ this are no longer used by the audio thread
  EpochReclaimer reclaimer_{1};
  EpochReclaimer::ReaderId reader_{0};
  // Audio thread.
  Packet* cur_ = nullptr;
  Packet* fadeFrom_ = nullptr;
  uint32_t fadePos_ = 0, fadeLen_ = 0;
  uint32_t lastFrames_ = 0;        // frames of the last image block (the scratch still holds them)
  float silence_ = 0.0f;           // silenceDb as a linear peak
  AlignedBuffer<float> zeros_;     // a silent source for the crossfade
  VoiceEngine voices_;
  EventBuffer events_{1};
  TransportSnapshot transport_;
  HostStats stats_;
};

}  // namespace pq::engine
