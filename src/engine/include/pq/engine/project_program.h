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
// Device output: one memcpy per device channel from the output bus. Per block one more ring poll for topologies.
//
// ---- Topology changes (T3.17: a track added or removed, a re-routing, a merge) ------------------------------------
// publishTopology(layout, units) replaces the unit list and the bus set while the program runs. Each unit of the new
// layout either CONTINUES a unit of the current layout (TopologyUnit::from: the same runtime unit — its current image,
// state, VoiceEngine and sounding voices — optionally with a new image) or is NEW (its first image). The non-RT call
// allocates everything: bus storage (a bus whose ProjectLayout::busKey and width match a current bus keeps that bus's
// storage, so a continued unit's image keeps reading and writing the same addresses and needs no rebinding), the new
// units, one packet per new image (transfer plan against the continued unit's last published image, as publish()).
// The audio thread switches at an image-block boundary, all at once: the unit order, the buses, and every new image
// of the topology (one T1.9 swap each, crossfaded when audible; a new unit's first image starts instantly). The
// switch waits (block by block) until none of the units that get a new image is fading or has packets still queued,
// so the plans run against exactly the images they were built for.
// A unit that is not continued (a removed track) keeps running as a ZOMBIE after the switch, in an order where it
// still feeds the old images of its readers, until each of those readers has made the topology's new image current
// and finished its crossfade: a removed track fades out through its readers' crossfades instead of cutting off. Then
// it stops and is freed off the audio thread (collect()). If no such order exists (a reversed dependency), zombies are
// dropped at the switch and the buses only they wrote read silence.
// While a topology is pending (published, not yet switched to), publish() refuses (ErrorCode::Overflow: retry).
//
// ---- Idle units (T3.17-fix2, M3 review H1: "silence is free", the unit-level start of T1.x-silence) -------------
// A unit whose image declares UnitImage::idleAfterBlocks (the compiler proves its outputs are zero when its input
// buses are silent and no voice sounds, and that its only state is smoothed gains) is not run in a block where every
// bus it reads is silent, it has no live voice and no note event, it is not fading, and its linked params have been
// unchanged for more than idleAfterBlocks blocks of that kind (so its gains have settled: running it would change
// nothing, the skip is bit-exact). Its bus channels are zeroed once and marked silent, so the units reading them can
// go idle too: a silent project costs a few compares per unit per block. Any event, param change, voice, new image
// or non-silent input runs it again at once.
// Unit indices in every accessor are those of the last PUBLISHED layout.

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
  // Optional (empty, or one per bus): what each bus carries, e.g. "<track id>:<port>". publishTopology() keeps the
  // storage of a bus whose key and width are unchanged. Empty keys never match.
  std::vector<std::string> busKey;
  // Optional (empty, or one per bus): 1 = level-metered (peak and mean square of channels 0–1 per block; T3.17 track
  // meters). Metering costs one pass over the bus's first two channels per block.
  std::vector<uint8_t> busMetered;
};

// Levels of a metered bus since the previous readMeters() (linear; mean square over the frames).
struct BusLevel {
  uint8_t channels = 0;  // 0: not metered / no such bus
  float peak[2] = {0.0f, 0.0f};
  float meanSquare[2] = {0.0f, 0.0f};
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
  // Owners of resources the image points into (T2.6-fix4: its tuning tables), released with the image: when the
  // program frees a retired image (collect(), off the audio thread) or at its destruction.
  std::vector<std::shared_ptr<const void>> keepAlive;
  // T3.17-fix2 (idle units, see "Idle units" above): ≥ 1 when the image's outputs are exactly zero while every bus it
  // reads is silent and no voice sounds, and its state then only settles (smoothed gains); the number of such
  // blocks with unchanged params after which its state no longer changes either. 0: never skipped.
  uint32_t idleAfterBlocks = 0;
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
  bool idleUnits = true;                  // skip idle units ("Idle units"; false: run every unit, for measurements)
};

struct UnitStats {
  uint64_t swaps = 0, instantSwaps = 0, crossfades = 0;
  uint64_t fadeBlocks = 0;      // image blocks in which this unit ran two images
  uint64_t transferBytes = 0;   // last swap
  uint64_t sequence = 0;        // the current image's publish sequence (1, 2, …)
  uint64_t idleBlocks = 0;      // image blocks skipped as an idle unit (T3.17-fix2)
};

struct ProjectStats {
  uint64_t blocks = 0;          // image blocks
  uint64_t fadeBlocks = 0;      // image blocks in which any unit faded
  uint32_t maxFadingUnits = 0;  // the most units fading in one block
  uint64_t topologySwitches = 0;
  uint64_t topologyWaitBlocks = 0;  // image blocks a pending topology waited for fades / queued packets
  uint64_t zombieRuns = 0;          // unit runs of removed units after a switch
};

inline constexpr uint32_t kNoUnit = 0xFFFFFFFFu;

// One unit of a new topology (publishTopology).
struct TopologyUnit {
  uint32_t from = kNoUnit;  // the unit of the current layout this one continues, or kNoUnit: a new unit
  bool hasImage = false;    // a new image (required for a new unit), made current at the switch
  UnitImage image;
  SwapPolicy policy;
};

struct TopologyInfo {
  uint64_t sequence = 0;            // 1 for the first topology change, then 2, …
  uint32_t newUnits = 0, continued = 0, newImages = 0, zombies = 0;
  uint32_t reusedBuses = 0, newBuses = 0;
  bool zombiesDropped = false;      // no order kept the removed units feeding their readers (see above)
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
  // Non-RT (serialised with publish). See "Topology changes". `units` is parallel to layout.units. Errors: an invalid
  // layout (as create()), a `from` out of range or used twice, a continued unit whose `voices` differs, a new unit
  // without an image, an image whose bindings do not match (as publish()), a topology already pending (Overflow).
  Result<TopologyInfo> publishTopology(ProjectLayout layout, std::vector<TopologyUnit> units);
  // Non-RT: a published topology the audio thread has not switched to yet.
  bool topologyPending() const;
  // Non-RT: publish(unit, …) would not be refused for lack of room (no topology pending, the unit's ring not full).
  // A refused publish destroys the image it was given: wait for this first.
  bool canPublish(uint32_t unit) const;
  // Non-RT: images published to the unit that the audio thread has not taken yet (approximate).
  uint32_t queued(uint32_t unit) const;
  // Non-RT (one reader thread): the levels of buses of the last published layout since the previous call, then a reset
  // request (the audio thread restarts the accumulation at its next block).
  void readMeters(std::span<const uint32_t> buses, std::span<BusLevel> out);
  // RT-safe (audio thread): after the last process(), no unit was fading, no image or topology was waiting and no
  // voice was live in any running unit. Computed inside process() before the program tells its reclaimer that the
  // audio thread is quiescent (T3.17-fix1, M3 review C1): read after that, the units of the current topology's dead
  // zombie slots may already be freed by a concurrent collect(). False before the first process().
  bool quiet() const noexcept { return quiet_; }
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
  // The last block's values (frames floats) of a bus of the topology the audio thread runs.
  const float* bus(uint32_t b, uint16_t channel) const noexcept;
  uint32_t numBuses() const noexcept;  // of the topology the audio thread runs
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
  struct BusStore;
  struct Slot;
  struct Topology;
  ProjectProgram(const ProjectConfig& config, ProjectLayout layout);
  static void onBlock(void* self, const AudioBlock& b) noexcept { static_cast<ProjectProgram*>(self)->process(b); }

  // Non-RT (mutex_ held).
  Result<std::unique_ptr<Packet>> makePacket(const Unit& U, uint32_t unitIndex, const ProjectLayout& layout,
                                             const std::vector<std::shared_ptr<BusStore>>& stores,
                                             const std::vector<float*>& ownChannels, UnitImage ui,
                                             const SwapPolicy& policy, PublishInfo& info);
  std::unique_ptr<Unit> makeUnit(const UnitLayout& cfg);
  void collectLocked();
  // RT.
  void trySwap(Unit& u, const Slot& s) noexcept;
  void install(Unit& u, const Slot& s, Packet* p) noexcept;
  void maybeSwitchTopology() noexcept;
  float unitPeak(const Slot& s) const noexcept;
  bool computeQuiet() const noexcept;
  bool runUnit(Unit& u, const Slot& s, uint32_t frames, int64_t time) noexcept;  // true: it ran two images (a fade)

  ProjectConfig cfg_;
  ProjectLayout layout_;                            // last published
  std::vector<std::unique_ptr<Unit>> units_;        // last published layout order
  std::vector<std::vector<float*>> ownChannels_;    // per unit of units_: the bus channels it produces
  std::vector<std::shared_ptr<BusStore>> stores_;   // last published layout's buses
  std::vector<std::pair<uint64_t, std::unique_ptr<Unit>>> zombies_;  // removed units (topology sequence that removed them)
  uint32_t stride_ = 0;
  AlignedBuffer<float> zeros_;                      // one block of zeros, never written
  std::vector<float> projectParams_;
  // Non-RT side.
  mutable std::mutex mutex_;
  std::atomic<uint64_t> freed_{0};
  EpochReclaimer reclaimer_{1};
  EpochReclaimer::ReaderId reader_{0};
  std::vector<Topology*> topoInflight_;             // published and not retired (owned), in sequence order
  uint64_t nextTopoSeq_ = 1;
  SpscRing<Topology*> topoRing_{4};
  std::atomic<uint64_t> topoCurrent_{0};            // sequence of the topology the audio thread runs
  std::atomic<uint32_t> meterGen_{0};               // readMeters() requests a restart of the accumulation
  // Audio thread.
  Topology* topo_ = nullptr;
  Topology* topoPending_ = nullptr;                 // popped, waiting for fades / queued packets
  uint32_t lastFrames_ = 0;
  bool quiet_ = false;                              // computeQuiet() at the end of the last process()
  float silence_ = 0.0f;
  TransportSnapshot transport_;
  ProjectStats stats_;
};

}  // namespace pq::engine
