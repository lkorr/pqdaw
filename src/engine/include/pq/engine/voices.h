#pragma once

// Voices (T1.8): note events → voice allocation → per-sample voice.* signals → ProgramImage::process → voice
// outputs mixed with per-lane gain → voice end. One VoiceEngine drives one ProgramImage that has voice stages.
//
// ---- Lanes and groups ----------------------------------------------------------------------------------------
// A voice is one lane of an 8-lane voice group (the AVX2 lanes; pq/dsp/kernel.h layout). The engine has
// io().maxVoiceGroups · 8 lanes. VoiceConfig::maxVoices is the polyphony (the steal threshold); lanes above it
// are headroom where stolen voices fade out while their replacement already plays (voiceGroupsFor() sizes an image
// with one spare group). The image skips groups whose laneMask is 0 (program_image.h), so the cost is per OCCUPIED
// group: a new voice goes to a free lane of the fullest non-full occupied group (best fit), else to the lowest empty
// group. Best fit alone still leaves 15-33 % more occupied groups than ceil(voices / 8) under random traffic
// (bench/RESULTS.md T1.8), so with VoiceConfig::compact (default) the engine empties the least occupied group at the
// end of a block whenever a whole group can be freed: each of its voices moves to a free lane of another occupied
// group: a strided copy of the lane's state words (O(ring) for delay lines, whose rings are re-aligned to the
// destination group's write position); noise state stays with the group. FmVoice under random traffic: −12 % CPU,
// engine time unchanged within noise (bench/RESULTS.md).
//
// ---- Sample accuracy (an event at frame k takes effect at sample k) -----------------------------------------
// No sub-block splitting: the block runs once and the voice.* signals carry the timing.
//   voice.hz, voice.cents     audio rate, per lane and sample: the note's Hz from k; Pitch events step or glide
//   voice.gate                lane mask: true from the note-on frame, false from the note-off frame
//   voice.trigger             lane mask: true at the note-on frame only (also on a same-note retrigger)
//   voice.velocity, .release, .ratio, .harmonic, .unisonCount, .index
//                             param rate (8-float pattern, read at frame 0): the values of the lane's latest
//                             note-on / note-off in this block hold for the whole block
// Before its onset, and while free, a lane reads hz 0, cents kIdleCents, gate and trigger false, and its gain is 0.
// With resetOnNoteOn the lane's state is reset to StateInit at the start of the block in which it starts; with
// hz 0 an oscillator does not advance before the onset, so a voice's output from sample k on does not depend on
// where block boundaries fall (tested for block sizes 1…128). Lanes are reset generically from the image's
// StateLayout: every per-lane word of each voice state entry (word index ≡ lane mod 8, pq/dsp/atom_state.h); the
// shared words (noise counter, delay-line header, phasor incScale/halfSr) are kept. Any backend whose StateArena
// follows atom_state.h works.
//
// ---- Gain, stealing, mixing ----------------------------------------------------------------------------------
// The engine mixes every voice `out` slot (not exports) into one global buffer per slot: Σ lanes gain · out, with
// gain per lane and sample: 0 before the onset and while free, 1 while sounding, a linear ramp to 0 over
// stealFadeMs when the voice is stolen. Voice→global reductions INSIDE the patch (voice_sum …) see the lane mask
// only (block granular): they get neither the pre-onset zero nor the steal fade (docs/QUESTIONS.md Q?-T1.8-a).
// Stealing (live voices ≥ maxVoices): released voices are taken before held ones; among them the oldest
// (StealPolicy::Oldest) or the quietest (StealPolicy::Quietest: lowest peak of the monitored outputs over the last
// block; ties → oldest). The victim fades in its lane; the new note starts in a free lane at its frame. Without a
// free lane (headroom exhausted) the new note waits for the victim's fade and starts at the first block after it
// (VoiceStats::deferredStarts). StealPolicy::None drops notes beyond maxVoices.
// Same-note retrigger (retriggerSameNote): a note-on whose Hz equals (bit for bit) a held or released voice's Hz
// takes over that voice: no state reset, trigger pulse at k, gate true from k (no extra gate edge when it was
// already held), new velocity.
//
// ---- Voice end (SPEC §7.7) -----------------------------------------------------------------------------------
// Checked at the end of each block; an ended lane is free from the next block on.
//   1. doneSlot (a voice out/export, e.g. `out done : bool voice`): the voice ends when the slot's value at the last
//      frame of the block is true (mask) or > 0.5, held or released (one-shots end themselves);
//   2. after note-off: when the monitored slots (monitorMask; default every voice `out`, not exports) have stayed
//      below silenceDb for silenceMs (block-peak granular). Held voices never end on silence;
//   3. with neither monitored slots nor a done slot: releaseTimeoutMs after note-off.
// A stolen voice ends when its fade is over.

#include <cstdint>
#include <span>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/result.h"
#include "pq/engine/note_events.h"
#include "pq/engine/program_image.h"
#include "pq/graph/patch.h"

namespace pq::engine {

enum class StealPolicy : uint8_t { Oldest, Quietest, None };

inline constexpr float kIdleCents = -100000.0f;  // voice.cents of a free or not-yet-started lane (≈ 0 Hz)
inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

struct VoiceConfig {
  uint32_t maxVoices = 64;          // polyphony: live (held + released) voices before stealing; ≤ lanes
  StealPolicy steal = StealPolicy::Oldest;
  bool retriggerSameNote = true;
  bool resetOnNoteOn = true;        // lane state ← StateInit at note-on (Q29: per-voice oscillators restart)
  bool compact = true;              // move voices out of the least occupied group when a group can be freed
  float stealFadeMs = 3.0f;         // linear fade of a stolen voice
  float silenceDb = -90.0f;         // voice end: below this after release …
  float silenceMs = 20.0f;          // … for this long
  float releaseTimeoutMs = 2000.0f; // voice end without a done slot or monitored outputs
  double referenceHz = 261.626;     // voice.cents = 1200 · log2(hz / referenceHz) (SPEC §3.1 default C3)
  uint32_t monitorMask = 0;         // bit s: voice out/export slot s is monitored for silence and level (0: every
                                    // voice `out`); slots ≥ 32 cannot be named here
  uint32_t doneSlot = kNoSlot;      // index into io().voiceOut, or kNoSlot (findVoiceSlot(io, patch, "done"))
  uint32_t maxEventsPerBlock = 1024; // events beyond this in one block are dropped (VoiceStats::droppedEvents)
};

// Voice groups an image needs for maxVoices plus one group of fade headroom.
inline uint32_t voiceGroupsFor(uint32_t maxVoices) noexcept { return (maxVoices + 7) / 8 + 1; }

// The voice out/export slot whose node is named `name` (e.g. "done") in the scheduled patch (Schedule::rewritten
// when set, else the patch given to schedule()), or kNoSlot. Not RT-safe.
uint32_t findVoiceSlot(const ImageInterface& io, const graph::Patch& patch, std::string_view name);

enum class LaneStatus : uint8_t { Free, Held, Released, Fading, Pending };

struct VoiceStats {
  uint64_t noteOns = 0, noteOffs = 0, pitchEvents = 0;
  uint64_t retriggers = 0;      // same-note retriggers
  uint64_t steals = 0;          // voices faded out to make room
  uint64_t deferredStarts = 0;  // notes that waited for a fade (no free lane)
  uint64_t droppedNotes = 0;    // StealPolicy::None beyond maxVoices, or a deferred note replaced / released early
  uint64_t unmatchedEvents = 0; // note-off / pitch for a note id that is not sounding
  uint64_t lateEvents = 0;      // frame ≥ block length (clamped)
  uint64_t droppedEvents = 0;   // beyond VoiceConfig::maxEventsPerBlock
  uint64_t voicesEnded = 0;
  uint64_t voiceMoves = 0;      // compaction: voices moved to another group
  uint64_t blocks = 0;
};

class VoiceEngine {
public:
  VoiceEngine() = default;
  VoiceEngine(const VoiceEngine&) = delete;
  VoiceEngine& operator=(const VoiceEngine&) = delete;

  // Off the audio thread. The image must have voice stages' groups (io().maxVoiceGroups ≥ 1) and outlive the
  // engine's use; maxVoices must be in [1, lanes]. Allocates every buffer. Clears all voices.
  Status prepare(ProgramImage* image, const VoiceConfig& config);

  // One block: ctx.frames ≤ io().maxBlockFrames, ctx.events (may be null) as in note_events.h. Fills ctx.voices
  // (the caller's global I/O, params, transport and sends pass through), runs the image, mixes the voice outs and
  // ends finished voices. RT-safe.
  void process(ProcessContext& ctx) noexcept;

  // The mixed voice `out` slots of the last block (frames floats each), in io().voiceOut order of the non-export
  // slots; mixSlot(i) is the io().voiceOut index of mix i.
  uint32_t numMixes() const noexcept { return static_cast<uint32_t>(mixSlots_.size()); }
  const float* mix(uint32_t i) const noexcept { return mixBuf_.data() + static_cast<size_t>(i) * stride_; }
  uint32_t mixSlot(uint32_t i) const noexcept { return mixSlots_[i]; }

  // Inspection (tests, UI reports). RT-safe.
  uint32_t lanes() const noexcept { return lanes_; }
  uint32_t groups() const noexcept { return groups_; }
  LaneStatus laneStatus(uint32_t lane) const noexcept { return status_[lane]; }
  uint32_t laneNote(uint32_t lane) const noexcept { return noteId_[lane]; }
  uint32_t laneOf(uint32_t noteId) const noexcept;   // a held/released/fading lane playing noteId, or kNoSlot
  uint32_t liveVoices() const noexcept { return live_; }  // held + released + deferred (waiting for a fade)
  bool hasDeferredNote(uint32_t lane) const noexcept { return hasPending_[lane] != 0; }
  uint32_t activeGroups() const noexcept;            // groups with a non-free lane
  uint32_t groupsRunLastBlock() const noexcept { return groupsRun_; }
  // The voice.* buffer group g passed to the image in the last block (audio-rate: frames · 8; param: 8 floats),
  // the per-lane gain (frames · 8), and voice out slot s of group g (frames · 8, null if not requested).
  const float* signal(uint32_t g, VoiceSignal s) const noexcept { return sigPtr_[g * kVoiceSignalCount + static_cast<uint32_t>(s)]; }
  const float* gain(uint32_t g) const noexcept { return gainBuf_.data() + static_cast<size_t>(g) * groupStride_; }
  const float* voiceOut(uint32_t g, uint32_t slot) const noexcept { return outPtr_[g * numOutSlots_ + slot]; }
  float laneLevel(uint32_t lane) const noexcept { return level_[lane]; }
  const VoiceStats& stats() const noexcept { return stats_; }
  const VoiceConfig& config() const noexcept { return cfg_; }

private:
  enum class OpKind : uint8_t { Start, Retrigger, Release, Pitch, Fade };
  struct LaneOp {
    uint32_t frame;
    uint16_t lane;
    OpKind kind;
    uint8_t pad_;
    NoteEvent ev;  // the note-on (Start/Retrigger), note-off (Release) or pitch event
  };

  // Allocation (logical state, time order).
  void allocate(const EventList* events, uint32_t frames) noexcept;
  void onNoteOn(const NoteEvent& e, uint32_t frame) noexcept;
  uint32_t findFreeLane() const noexcept;
  uint32_t pickVictim() const noexcept;
  void addOp(uint32_t frame, uint32_t lane, OpKind kind, const NoteEvent& e) noexcept;
  void setStatus(uint32_t lane, LaneStatus s) noexcept;
  // Rendering (per lane, signal buffers).
  void renderGroup(uint32_t g, uint32_t frames) noexcept;
  void renderSegment(uint32_t lane, uint32_t from, uint32_t to, const uint32_t* fill) noexcept;
  void applyOp(const LaneOp& op) noexcept;
  void freeLane(uint32_t lane) noexcept;
  void moveLane(uint32_t from, uint32_t to) noexcept;
  void compact() noexcept;
  void resetLaneState(uint32_t lane) noexcept;
  // After the image.
  void mixAndMeasure(uint32_t frames) noexcept;
  void endVoices(uint32_t frames) noexcept;

  ProgramImage* image_ = nullptr;
  VoiceConfig cfg_;
  uint32_t groups_ = 0, lanes_ = 0, maxBlock_ = 0, stride_ = 0, groupStride_ = 0;
  uint32_t numOutSlots_ = 0;
  uint32_t fadeFrames_ = 1, silenceFrames_ = 1;
  int64_t releaseTimeoutFrames_ = 0;
  float silenceLevel_ = 0.0f;
  double sampleRate_ = 48000.0;

  // Buffers handed to the image.
  AlignedBuffer<float> audioSig_;   // per group: hz, cents, gate, trigger (groupStride_ floats each)
  AlignedBuffer<float> paramSig_;   // per group: velocity, release, ratio, harmonic, unisonCount, index (8 each)
  AlignedBuffer<float> gainBuf_;    // per group: groupStride_
  AlignedBuffer<float> outBuf_;     // per group and requested voice out slot: groupStride_
  AlignedBuffer<float> mixBuf_;     // per mix: stride_
  std::vector<const float*> sigPtr_;  // [g · kVoiceSignalCount + s]
  std::vector<float*> outPtr_;        // [g · numOutSlots_ + slot] (null: not requested)
  std::vector<VoiceGroupIO> gio_;
  VoiceTable table_;
  std::vector<uint32_t> mixSlots_;    // io().voiceOut index per mix
  std::vector<uint32_t> monitorSlots_;
  std::vector<uint32_t> validFrames_; // per group: frames for which the buffers hold the steady state
  std::vector<uint8_t> groupDirty_;   // per group: re-render this block
  std::vector<uint8_t> carryDirty_;   // per group: re-render next block (pulses, onsets, fades, glides)
  std::vector<uint16_t> groupCount_;  // per group: non-free lanes
  std::vector<uint32_t> laneMask_;    // per group: bit l = lane l is not free
  std::vector<uint32_t> activeList_;  // scratch: occupied groups of the block
  std::vector<const float*> mixOut_, mixGain_;  // scratch: per occupied group, the mixed slot and its gain

  // Per lane: logical (allocator) state.
  std::vector<LaneStatus> status_;
  std::vector<uint32_t> noteId_;
  std::vector<int64_t> startTime_, releaseTime_, fadeEnd_;
  std::vector<float> noteHz_;         // the note's Hz for same-note matching (latest note-on / pitch target)
  std::vector<float> level_;          // peak of the monitored outs over the last block
  std::vector<uint32_t> silentFrames_;
  std::vector<uint8_t> hasPending_;
  std::vector<NoteEvent> pending_;    // deferred note-on per lane (status Fading / Pending)
  // Per lane: render state (signal values at the start of the next frame to render).
  std::vector<double> hz_, cents_;    // current (double: glides accumulate)
  std::vector<double> hzMul_, centsStep_;
  std::vector<float> hzTarget_;
  std::vector<double> centsTarget_;
  std::vector<uint32_t> rampLeft_;
  std::vector<float> gateMask_;       // 0 or all-ones mask
  std::vector<float> laneGain_;
  std::vector<float> fadeStep_;       // > 0 while fading
  std::vector<float> velocity_, release_, ratio_, harmonic_, unison_, index_;

  // Per block scratch.
  std::vector<LaneOp> ops_, sortedOps_;
  std::vector<uint32_t> opBegin_;     // per lane + 1 (counting sort)
  uint32_t numOps_ = 0;
  // Lane reset: spans of per-lane words in a voice group's state block, and the StateInit template.
  struct ResetSpan {
    uint32_t firstWord, numWords;  // relative to the group's block; numWords a multiple of 8
  };
  std::vector<ResetSpan> resetSpans_;
  std::vector<ResetSpan> moveSpans_;  // per-lane words moved as they are (everything but noise and delay lines)
  struct DelaySpan {
    uint32_t firstWord, capacity;     // a voice delay line's DelayState (8 lanes)
  };
  std::vector<DelaySpan> delays_;
  AlignedBuffer<uint32_t> stateTemplate_;
  std::byte* voiceState_ = nullptr;   // the image's voice state blocks (StateArena), voiceStateStride_ apart
  uint32_t voiceStateStride_ = 0;
  uint32_t numReady_ = 0;             // lanes in status Pending (start at the next block)
  bool opsLastBlock_ = true;          // opBegin_ holds non-zero offsets from the last block
  int64_t clock_ = 0;                 // engine frames before frame 0 of the current block (ages, timeouts)
  uint32_t live_ = 0;
  uint32_t lastLane_ = kNoVoice;
  uint32_t groupsRun_ = 0;
  VoiceStats stats_;
};

}  // namespace pq::engine
