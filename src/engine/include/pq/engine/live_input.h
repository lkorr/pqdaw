#pragma once

// Live note input (T6.4; docs/plans/M6.md "### T6.4", "The two note worlds"): MIDI ports and the computer keyboard →
// note events of instrument slots, at a constant latency, with exactly the NoteEvent fields the timeline gives the same
// note drawn in a clip (so a live note sounds as the drawn one, tuning-as-signal included).
//
//   LivePorts       the producer side: one SPSC ring of RawMidi per MIDI port (the app's MIDI service pushes from each
//                   port's callback: stamp + push, nothing else) and one for the computer keyboard (the UI thread).
//                   Shared (std::shared_ptr) by the producers and the engine that consumes it, so a new engine (a
//                   sample-rate change) keeps the same rings.
//   LiveRouteImage  immutable, built off the audio thread (the app's ProjectLink, from the document): per (source,
//                   channel) the target slots; per target the 128-key pitch table (doc::InputPitchMap, lowered to the
//                   NoteEvent fields) and the computer keyboard's table. Published by atomic pointer swap; one indexed
//                   load per event.
//   LiveInput       the consumer, owned by the Engine: process() once per engine block drains the rings, maps each
//                   stamp to engine time, routes it, and queues NoteEvents; emit() hands a slot its due events from
//                   Engine::onSlot (after the timeline's and the auditions'). Held notes are tracked per (source,
//                   channel, key, slot), so a note-off reaches the slot its note-on went to whatever was re-routed in
//                   between; held notes survive transport stop (monitoring while stopped).
//
// ---- Time ---------------------------------------------------------------------------------------------------------
// Stamps are host-clock ticks (LiveInputConfig::clockHz; the app: qpcNow100ns, 1e7 Hz). Every device callback stamps
// its start (AudioBlock::hostTime, CallbackRunner); with AudioBlock::hostOffset that pairs a host time with an engine
// time. A first-order clock filter smooths the callback jitter (it tracks the device clock's drift; a jump > 10 ms
// re-anchors). An event plays at engineTime(stamp) + latencyFrames (the app: one device period), so the latency is
// constant (no block-size jitter): frame = that − blockStart, clamped into the block (late events at frame 0). Without
// a host time (offline, tests that do not stamp) events play at the start of the block that drains them.
//
// ---- Never a stuck note ---------------------------------------------------------------------------------------------
// A port ring that overflowed (the engine did not run for a while) releases every held note of that port. The pending
// queue keeps room for every held note's note-off (a note-on is admitted only while there is room for it, and the
// held-note table is bounded). Events stamped more than staleSeconds before the block (rings filled while the device was
// stopped) play their note-offs and drop their note-ons. The EventBuffer itself never loses a note-off (note_events.h).
//
// ---- Note ids -------------------------------------------------------------------------------------------------------
// Live notes use 0xE0000000 | n (n < 2^28); the timeline's instances stay below 0xE0000000 and auditions use
// 0xF0000000 | n, so the three spaces are disjoint in a slot.
//
// ---- Threads --------------------------------------------------------------------------------------------------------
//   LivePorts::push(port)          that port's producer thread (one per port); wait-free
//   publish, collect, setLatency, popRecord, setRecording   the UI thread (one)
//   process, emit                  the audio thread (Engine::process / onSlot). RT-safe.

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "pq/core/result.h"
#include "pq/core/spsc_ring.h"
#include "pq/engine/note_events.h"
#include "pq/tuning/pitch_id.h"

namespace pq::engine {

inline constexpr uint32_t kLivePorts = 16;                // MIDI input ports
inline constexpr uint32_t kLiveKeyboard = kLivePorts;     // the computer keyboard's source index
inline constexpr uint32_t kLiveSources = kLivePorts + 1;  // ports + the keyboard
inline constexpr uint32_t kLiveChannels = 16;
// The computer keyboard (T6.4: Live's letter rows): 17 steps (degrees 0 … 16 above the equave offset) at equave
// offsets kKeyboardMinEquave … kKeyboardMaxEquave. A keyboard event's key byte is keyboardKey(equave, step).
inline constexpr int32_t kKeyboardSteps = 17;
inline constexpr int32_t kKeyboardMinEquave = -5, kKeyboardMaxEquave = 5;
inline constexpr uint32_t kKeyboardKeys = static_cast<uint32_t>((kKeyboardMaxEquave - kKeyboardMinEquave + 1) * kKeyboardSteps);
constexpr uint8_t keyboardKey(int32_t equave, int32_t step) noexcept {
  return static_cast<uint8_t>((equave - kKeyboardMinEquave) * kKeyboardSteps + step);
}
constexpr int32_t keyboardEquave(uint8_t key) noexcept { return key / kKeyboardSteps + kKeyboardMinEquave; }
constexpr int32_t keyboardStep(uint8_t key) noexcept { return key % kKeyboardSteps; }

// One raw MIDI message as a port's callback saw it (16 bytes). Running status is resolved by the producer; SysEx,
// clock and other system messages are not pushed. The computer keyboard pushes 0x90 / 0x80 with key = keyboardKey().
struct RawMidi {
  int64_t stamp = 0;  // host clock ticks (LiveInputConfig::clockHz)
  uint8_t port = 0;   // source index (< kLiveSources)
  uint8_t status = 0, d1 = 0, d2 = 0;
  uint32_t pad_ = 0;
};
static_assert(sizeof(RawMidi) == 16);

class LivePorts {
public:
  explicit LivePorts(uint32_t ringCapacity = 4096);
  // The port's producer thread (one per port; the keyboard: the UI thread). Wait-free. False: the ring is full (the
  // message is lost; the consumer then releases that port's held notes).
  bool push(uint32_t port, const RawMidi& m) noexcept;  // RT-safe
  SpscRing<RawMidi>& ring(uint32_t port) noexcept { return *rings_[port]; }
  // Messages lost to a full ring, per port (monotonic).
  uint64_t overflows(uint32_t port) const noexcept { return overflow_[port].load(std::memory_order_acquire); }
  // Bumped (release) after every push or overflow: the consumer skips the rings while it is unchanged, so an idle block
  // costs one load instead of one per ring.
  uint64_t activity() const noexcept { return activity_.load(std::memory_order_acquire); }

private:
  std::array<std::unique_ptr<SpscRing<RawMidi>>, kLiveSources> rings_;
  std::array<std::atomic<uint64_t>, kLiveSources> overflow_{};
  alignas(64) std::atomic<uint64_t> activity_{0};
};

// A key's pitch on one target track: the NoteEvent fields the timeline sets for the same note drawn in that track
// (Timeline::setIdentity), and the identity a recording stores.
struct LivePitch {
  tuning::PitchId id;      // Keys: a Degree identity; Hz: the key's Hz frozen as Cents (before a note-on bend)
  float hz = 0.0f;         // static Hz; 0: the key plays nothing
  float degree = std::numeric_limits<float>::quiet_NaN();  // voice.degree (follow Degree), else NaN
  float equave = std::numeric_limits<float>::quiet_NaN();  // voice.equave
  float tuning = 0.0f;     // voice.tuning: the track patch's tuning node the note follows (0: static)
  float noteDegree = std::numeric_limits<float>::quiet_NaN();  // voice.noteDegree
  bool mapped() const noexcept { return hz > 0.0f; }
};

struct LiveTrackDesc {
  uint32_t slot = 0;                   // instrument event slot
  bool hzMode = false;                 // doc::NoteInputMode::Hz (informational: the table already holds the mode)
  uint32_t sources = 0;                // bit s: listens to source s (bit kLiveKeyboard: the computer keyboard)
  uint8_t channel = 0;                 // 0: every channel; 1 … 16: that MIDI channel only (the keyboard is channel 1)
  std::vector<LivePitch> keys;         // 128 entries (MIDI keys)
  std::vector<LivePitch> keyboard;     // kKeyboardKeys entries (empty: the keyboard plays nothing here)
};
struct LiveRouteDesc {
  std::vector<LiveTrackDesc> tracks;
  std::array<float, kLiveSources> bendSemitones{};  // per source: the pitch bend range (0: default 2 semitones)
};

class LiveRouteImage {
public:
  struct Target {
    uint32_t slot;
    const LivePitch* keys;      // 128
    const LivePitch* keyboard;  // kKeyboardKeys, or null
  };
  // Off the audio thread (allocates). Errors: a track with a key table that is not 128 entries, a keyboard table of the
  // wrong size, a channel > 16.
  static Result<std::unique_ptr<const LiveRouteImage>> build(const LiveRouteDesc& desc);
  // The targets of (source, channel 0 … 15). RT-safe.
  std::span<const Target> targets(uint32_t source, uint32_t channel) const noexcept {
    const Range r = ranges_[source * kLiveChannels + (channel & 15u)];
    return {targetList_.data() + r.begin, r.end - r.begin};
  }
  float bendCents(uint32_t source) const noexcept { return bendCents_[source]; }  // full-scale bend in cents
  uint32_t numTracks() const noexcept { return static_cast<uint32_t>(tables_.size()); }

private:
  struct Range {
    uint32_t begin = 0, end = 0;
  };
  std::vector<std::vector<LivePitch>> tables_;  // per track: 128 keys then the keyboard's
  std::vector<Target> targetList_;
  std::array<Range, kLiveSources * kLiveChannels> ranges_{};
  std::array<float, kLiveSources> bendCents_{};
};

// A note the live input played, for recording (T6.7) and Capture MIDI (T6.9): written only while setRecording(true).
struct LiveNoteRecord {
  enum class Kind : uint8_t { On, Off };
  Kind kind = Kind::On;
  uint32_t slot = 0;
  int64_t engineTime = 0;    // when it plays (engine samples)
  int64_t songSample = -1;   // the transport's song sample then (-1: stopped); block-accurate across a loop wrap
  float velocity = 0.0f;     // 1 … 127 (note-off: release velocity 0 … 127)
  float bendCents = 0.0f;    // the source's pitch bend at the event
  LivePitch pitch;           // On: the key's pitch on that track (Hz mode: freeze the bend with InputPitchMap::atNoteOn)
  RawMidi raw;               // the message as received (port, status, key, velocity, stamp)
};

struct LiveInputConfig {
  double sampleRate = 48000.0;
  double clockHz = 1e7;        // stamp ticks per second (the app: qpcNow100ns)
  uint32_t maxSlots = 512;     // instrument event slots (Engine: maxSlots)
  uint32_t maxHeld = 1024;     // held (source, channel, key, slot) notes
  uint32_t maxPending = 4096;  // queued events besides the held notes' note-offs
  uint32_t recordRing = 4096;  // LiveNoteRecords in flight (power of two)
  double staleSeconds = 1.0;   // events older than this when drained: note-offs only
  double bendRampMs = 2.0;     // a pitch-bend step glides over this (no zipper noise)
};

struct LiveInputStats {
  uint64_t messages = 0;      // raw messages drained
  uint64_t noteOns = 0, noteOffs = 0, bends = 0;  // events queued (per target)
  uint64_t droppedOns = 0;    // note-ons not played (held table / pending queue full, stale)
  uint64_t lateEvents = 0;    // events that came after their time (played at frame 0)
  uint64_t overflowReleases = 0;  // held notes released because their port's ring overflowed
  uint64_t recordDrops = 0;   // LiveNoteRecords lost to a full record ring
  uint64_t retuned = 0;       // TU.1: held notes moved to a new route image's pitch (a tuning that needed a new table)
};

class LiveInput {
public:
  LiveInput(const LiveInputConfig& config, std::shared_ptr<LivePorts> ports);
  ~LiveInput();
  LiveInput(const LiveInput&) = delete;
  LiveInput& operator=(const LiveInput&) = delete;

  // ---- UI thread ----
  // The routes from now on (null: none). The previous image is freed by collect() once the audio thread left it.
  void publish(std::unique_ptr<const LiveRouteImage> image);
  // Frees retired images the audio thread no longer reads (all of them when `audioStopped`).
  void collect(bool audioStopped);
  // Frames from a stamp's engine time to when it plays (the app: one device period). Any thread.
  void setLatency(uint32_t frames) noexcept { latency_.store(frames, std::memory_order_relaxed); }
  uint32_t latency() const noexcept { return latency_.load(std::memory_order_relaxed); }
  // Recording tap (audio → UI): written only while on.
  void setRecording(bool on) noexcept { recording_.store(on, std::memory_order_release); }
  bool popRecord(LiveNoteRecord& out) noexcept { return record_.tryPop(out); }
  LivePorts& ports() noexcept { return *ports_; }
  const std::shared_ptr<LivePorts>& sharedPorts() const noexcept { return ports_; }

  // ---- Audio thread ----
  struct Block {
    int64_t start = 0;        // engine time of the block's first frame
    uint32_t frames = 0;
    int64_t hostTime = 0;     // AudioBlock::hostTime (0: none)
    int32_t hostOffset = 0;   // AudioBlock::hostOffset
    bool playing = false;     // the transport, for LiveNoteRecord::songSample
    int64_t songSample = 0;   // the transport's song sample at `start`
  };
  // Drains the rings and queues the block's events. True when anything arrived (activity for the idle gate). RT-safe.
  bool process(const Block& b) noexcept;
  // Pushes slot `slot`'s events due in [sampleTime, sampleTime + frames) into `out`. RT-safe.
  void emit(uint32_t slot, int64_t sampleTime, uint32_t frames, EventBuffer& out) noexcept;
  // Nothing held, nothing queued (the engine may sleep). RT-safe.
  bool idle() const noexcept { return numHeld_ == 0 && numPending_ == 0; }
  uint32_t heldNotes() const noexcept { return numHeld_; }
  const LiveInputStats& stats() const noexcept { return stats_; }  // audio thread (or after it)
  // Engine time of host time `stamp` (the filter's current map; before any host time: blockStart). Audio thread.
  int64_t engineTimeOf(int64_t stamp) const noexcept;

private:
  struct Held {
    uint8_t source, channel, key;
    bool sustained;  // released while the sustain pedal was down: the note-off waits for the pedal
    uint32_t slot, noteId;
    float hz;        // the key's unbent static Hz (a bend moves the voice from it)
  };
  struct Pending {
    int64_t time;
    uint32_t slot;
    uint32_t done;   // emitted
    NoteEvent e;
  };
  void handle(const RawMidi& m, int64_t time, bool stale, const LiveRouteImage* img, const Block& b) noexcept;
  void onNoteOn(const RawMidi& m, int64_t time, const LiveRouteImage& img, const Block& b) noexcept;
  void onNoteOff(uint32_t source, uint32_t channel, uint32_t key, float releaseVelocity, int64_t time, const RawMidi& m,
               const Block& b) noexcept;
  void releaseHeld(uint32_t i, float releaseVelocity, int64_t time, const RawMidi* m, const Block& b) noexcept;
  void releaseSource(uint32_t source, int64_t time, const Block& b) noexcept;
  void bend(uint32_t source, uint32_t channel, float cents, int64_t time) noexcept;
  bool queue(int64_t time, uint32_t slot, const NoteEvent& e, bool off) noexcept;
  void record(LiveNoteRecord::Kind kind, uint32_t slot, int64_t time, float velocity, const LivePitch* pitch,
              const RawMidi& m, const Block& b) noexcept;
  void updateClock(const Block& b) noexcept;
  void retuneHeld(const LiveRouteImage& img, const Block& b) noexcept;

  LiveInputConfig cfg_;
  std::shared_ptr<LivePorts> ports_;
  // Images: the UI thread publishes (current_, epoch_); the audio thread acknowledges the epoch it read at its block
  // start in done_ once that block's process() is over.
  std::atomic<const LiveRouteImage*> current_{nullptr};
  std::atomic<uint64_t> epoch_{0};
  std::atomic<uint64_t> done_{0};
  struct Retired {
    std::unique_ptr<const LiveRouteImage> image;
    uint64_t until;  // freed once done_ ≥ until
  };
  std::unique_ptr<const LiveRouteImage> owned_;  // UI thread: the current image
  std::vector<Retired> retired_;                 // UI thread
  std::atomic<uint32_t> latency_{0};
  std::atomic<bool> recording_{false};
  SpscRing<LiveNoteRecord> record_;
  // Audio thread.
  std::vector<Held> held_;
  uint32_t numHeld_ = 0;
  std::vector<Pending> pending_;
  uint32_t numPending_ = 0, numPendingOffs_ = 0;
  std::array<float, kLiveSources * kLiveChannels> bendCents_{};  // current bend per (source, channel), in cents
  std::array<uint8_t, kLiveSources * kLiveChannels> sustain_{};  // pedal down per (source, channel)
  std::array<uint64_t, kLiveSources> seenOverflow_{};
  uint64_t seenActivity_ = 0;
  const LiveRouteImage* seenRoute_ = nullptr;  // TU.1: the image of the last block (a new one retunes held notes)
  uint32_t nextNote_ = 0;
  // Clock filter: engine time anchorTime_ ↔ host time anchorHost_ (double: sub-tick).
  bool anchored_ = false;
  int64_t anchorTime_ = 0;
  double anchorHost_ = 0.0;
  int64_t lastHost_ = 0;
  double ticksPerFrame_ = 0.0, framesPerTick_ = 0.0;
  int64_t blockStart_ = 0;
  uint32_t bendRamp_ = 0;
  int64_t staleFrames_ = 0;
  LiveInputStats stats_;
};

}  // namespace pq::engine
