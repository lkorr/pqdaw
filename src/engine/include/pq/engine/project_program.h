#pragma once

// The per-track project runner (T1.x-arch1, SPEC §7.4–§7.6, §15 2026-10-21): runs a project as N compile units, each
// with its own ProgramImage, state arena, swap slot and compile-cache key, in dependency order every block, passing
// signals between them through preallocated bus buffers. An edit recompiles one unit (pq/engine/project_compile.h)
// and publish(unit, …) swaps and crossfades only that unit; every other unit keeps running untouched.
//
// ---- Buses (the inter-unit ABI) ---------------------------------------------------------------------------------
// A bus is `width` planar channels of maxBlockFrames floats owned by the ProjectProgram (fixed addresses for its whole
// life). Exactly one unit writes it (ProjectLayout::busProducer; its image's global `out` slots point straight at the
// bus channels: no copy) and any LATER unit reads it (its image's global root `in` slots point at them). Units run in
// layout order, so a feed-forward path between units adds no latency: a reader sees its producer's output of the
// same block. Cycles never reach the runner: the partition merges them into one unit (pq/graph/project.h). Buses
// read zeros until their producer's first image is current.
//
// ---- Per-unit hot swap (T1.9's protocol, one slot per unit) -----------------------------------------------------
// publish(u, image) (non-RT, serialised) builds the transfer plan against the unit's last published image, the param
// remap by ParamSlot::key, the VoiceBinding (instrument units), the old image's crossfade scratch and the bus
// pointer tables, then pushes the packet through the unit's SPSC ring. At each image block, before the unit runs,
// the audio thread pops at most one packet for it (none while it fades) and swaps:
//   audible (SwapMode::Auto) = transport playing, or a voice of the unit sounding, or the unit's bus outputs of the
//   last block above silenceDb → crossfade for fadeMs: the old image runs beside the new one into its own scratch
//   (same inputs, events and voice table), then each bus channel = old + g · (new − old) (ProgramHost's linear,
//   bit-exact-where-equal fade). Else instant. Only that unit runs twice; the others are not touched, so their
//   outputs are bit-identical to a run without the swap (tested), and the CPU peak of an edit is one unit's cost.
// Retired images are freed off the audio thread (one EpochReclaimer for the project; collect()).
//
// ---- Parameters and the timeline --------------------------------------------------------------------------------
// Each image has its own param block (io().params; defaults, carried over by key at a swap). The project param block
// (ProjectConfig::numProjectParams floats) is what the BlockHook writes (T3.16 Timeline::blockHook: transport and
// automation targets); UnitImage::paramLinks copy project params into unit slots at every block (O(links)). Note
// events come from ProjectConfig::eventSources[UnitLayout::eventSlot] (Timeline::eventSource(k)) into the unit's
// VoiceEngine.
//
// ---- Cost per block -----------------------------------------------------------------------------------------
// Per unit: one ring poll, the param links, one virtual ProgramImage::process (or VoiceEngine::process), nothing else:
// no copies between units. During a unit's fade: its old image once more plus one fused pass over its bus channels.
// Device output: one memcpy per device channel from the output bus.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/core/result.h"
#include "pq/core/spsc_ring.h"
#include "pq/engine/audio_block.h"
#include "pq/engine/image_runner.h"
#include "pq/engine/note_events.h"
#include "pq/engine/program_host.h"
#include "pq/engine/program_image.h"
#include "pq/engine/state_transfer.h"
#include "pq/engine/voices.h"

namespace pq::engine {

inline constexpr uint32_t kNoBus = 0xFFFFFFFFu;

struct BusRef {
  uint32_t bus = kNoBus;
  uint16_t channel = 0;
  friend bool operator==(const BusRef&, const BusRef&) = default;
};

struct UnitLayout {
  std::string name;
  bool voices = false;          // run the unit's images through its own VoiceEngine
  uint32_t eventSlot = kNoSlot; // index into ProjectConfig::eventSources (voices only), or kNoSlot: no events
  VoiceConfig voice;            // doneSlot / monitorMask are per image (UnitImage::voiceSlots)
};

struct ProjectLayout {
  std::vector<UnitLayout> units;      // execution order
  std::vector<uint16_t> busWidth;     // channels per bus (≥ 1)
  std::vector<uint32_t> busProducer;  // writer unit per bus
  uint32_t outputBus = kNoBus;        // played on the device: channel c ← bus channel c (a mono bus on every channel)
};

// One image for one unit, with its bindings (pq/engine/project_compile.h makes them).
struct UnitImage {
  std::unique_ptr<ProgramImage> image;
  std::vector<BusRef> in;   // per io().globalIn slot (kNoBus: reads silence)
  std::vector<BusRef> out;  // per io().globalOut slot (kNoBus: not routed, the write is skipped); must cover every
                            // channel of every bus the unit produces exactly once
  VoiceSlots voiceSlots;
  std::vector<std::pair<uint32_t, uint32_t>> paramLinks;  // (io().params slot, project param index)
  uint64_t key = 0;         // compile-cache key (graph::unitHash), informational
};

struct ProjectConfig {
  double sampleRate = 48000.0;     // every image's io().sampleRate
  uint32_t maxBlockFrames = 1024;  // every image's io().maxBlockFrames
  uint32_t numOut = 2;             // device channels (≤ kMaxChannels)
  uint32_t maxPendingPerUnit = 4;  // packets published but not yet current, per unit (rounded up to a power of two)
  float silenceDb = -90.0f;        // SwapMode::Auto: unit outputs at or below this are silent
  BlockHook hook;                  // per image block, before any unit: transport + project params
  uint32_t numProjectParams = 0;
  std::vector<EventSource> eventSources;  // indexed by UnitLayout::eventSlot
  uint32_t eventCapacity = 1024;          // note events per unit per image block
};

struct UnitStats {
  uint64_t swaps = 0, instantSwaps = 0, crossfades = 0;
  uint64_t fadeBlocks = 0;      // image blocks in which this unit ran two images
  uint64_t transferBytes = 0;   // last swap
  uint64_t sequence = 0;        // the current image's publish sequence (1, 2, …)
};

struct ProjectStats {
  uint64_t blocks = 0;          // image blocks
  uint64_t fadeBlocks = 0;      // image blocks in which any unit faded
  uint32_t maxFadingUnits = 0;  // the most units fading in one block
};

class ProjectProgram {
public:
  // Non-RT. Errors: an invalid layout (no units, a bus without a valid producer, an output bus that does not exist,
  // more device channels than kMaxChannels).
  static Result<std::unique_ptr<ProjectProgram>> create(const ProjectConfig& config, ProjectLayout layout);
  // Non-RT, after the audio thread has stopped calling process(). Frees every image.
  ~ProjectProgram();
  ProjectProgram(const ProjectProgram&) = delete;
  ProjectProgram& operator=(const ProjectProgram&) = delete;

  // Non-RT (any thread; serialised). Hands `image` to unit `unit`, current from a block boundary (the first image of a
  // unit starts instantly). Errors: bad unit, null image, sample rate / block size / voice geometry other than the
  // unit's, bindings that do not match io() (counts, a bus the unit does not produce or a later unit's bus as input,
  // a channel ≥ the bus width, a produced bus channel left unwritten), a param link out of range, a plan error, too
  // many packets pending (ErrorCode::Overflow). Calls collect() first.
  Result<PublishInfo> publish(uint32_t unit, UnitImage image, const SwapPolicy& policy = {});
  // Non-RT: frees retired images every reader has passed; returns how many.
  size_t collect();
  size_t imagesAlive() const;   // non-RT
  uint64_t imagesFreed() const;
  // Non-RT: the unit's last published image (its stateLayout() is the next compile's ScheduleOptions::previous).
  const ProgramImage* lastPublished(uint32_t unit) const;

  // One device block (any frame count: split into image blocks of maxBlockFrames). RT-safe; run it with FTZ/DAZ.
  void process(const AudioBlock& block) noexcept;
  AudioCallback callback() noexcept { return {&ProjectProgram::onBlock, this}; }

  // Audio thread (or while it is not running). RT-safe.
  std::span<float> projectParams() noexcept { return projectParams_; }
  std::span<float> params(uint32_t unit) noexcept;           // the unit's current image's param block
  TransportSnapshot& transport() noexcept { return transport_; }
  const float* bus(uint32_t b, uint16_t channel) const noexcept {  // the last block's values (frames floats)
    return buses_.data() + (static_cast<size_t>(busFirst_[b]) + channel) * stride_;
  }
  const ProgramImage* current(uint32_t unit) const noexcept;
  bool crossfading(uint32_t unit) const noexcept;
  VoiceEngine* voices(uint32_t unit) noexcept;
  const UnitStats& unitStats(uint32_t unit) const noexcept;
  const ProjectStats& stats() const noexcept { return stats_; }
  uint32_t numUnits() const noexcept { return static_cast<uint32_t>(units_.size()); }
  const ProjectLayout& layout() const noexcept { return layout_; }

  // The audio thread stops / resumes calling process() (lets collect() free images meanwhile). RT-safe.
  void audioOffline() noexcept { reclaimer_.goOffline(reader_); }
  void audioOnline() noexcept { reclaimer_.goOnline(reader_); }

private:
  struct Packet;
  struct Unit;
  ProjectProgram(const ProjectConfig& config, ProjectLayout layout);
  static void onBlock(void* self, const AudioBlock& b) noexcept { static_cast<ProjectProgram*>(self)->process(b); }

  void trySwap(Unit& u) noexcept;
  float unitPeak(const Unit& u) const noexcept;
  bool runUnit(Unit& u, uint32_t frames, int64_t time) noexcept;  // true: it ran two images (a fade)
  float* busChannel(uint32_t b, uint16_t channel) noexcept {
    return buses_.data() + (static_cast<size_t>(busFirst_[b]) + channel) * stride_;
  }

  ProjectConfig cfg_;
  ProjectLayout layout_;
  std::vector<std::unique_ptr<Unit>> units_;
  // Buses: one block of `stride_` floats per bus channel.
  AlignedBuffer<float> buses_;
  std::vector<uint32_t> busFirst_;  // first channel index of each bus
  uint32_t stride_ = 0;
  std::vector<float> projectParams_;
  // Non-RT side.
  mutable std::mutex mutex_;
  std::atomic<uint64_t> freed_{0};
  EpochReclaimer reclaimer_{1};
  EpochReclaimer::ReaderId reader_{0};
  // Audio thread.
  uint32_t lastFrames_ = 0;
  float silence_ = 0.0f;
  TransportSnapshot transport_;
  ProjectStats stats_;
};

}  // namespace pq::engine
