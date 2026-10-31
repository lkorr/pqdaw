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
// group: a strided copy of the lane's state words; noise state stays with the group. A delay line moves only the
// part of its ring a read can still reach (min(the lane's history, floor(maxDelay) + 2) frames), re-aligned to the
// destination group's write position, and a block moves at most VoiceConfig::moveBudgetFrames ring frames: a group
// whose voices' rings exceed the budget is not emptied (bounded audio-thread work, M1 review M9). FmVoice under
// random traffic: −12 % CPU, engine time unchanged within noise (bench/RESULTS.md).
//
// ---- Sample accuracy (an event at frame k takes effect at sample k) -----------------------------------------
// No sub-block splitting: the block runs once and the voice.* signals carry the timing.
//   voice.hz, voice.cents     audio rate, per lane and sample: the note's Hz from k; Pitch events step or glide
//   voice.gate                lane mask: true from the note-on frame, false from the note-off frame
//   voice.trigger             lane mask: true at the note-on frame only (also on a same-note retrigger)
//   voice.velocity, .release, .ratio, .harmonic, .unisonCount, .index, .noteHz (T4.17: the note-on's Hz)
//                             param rate (8-float pattern, read at frame 0): the values of the lane's latest
//                             note-on / note-off in this block hold for the whole block
// A free lane reads hz = referenceHz (cents 0: finite, so `1 / voice.hz` stays finite in idle lanes), gate and
// trigger false, gain 0. With resetOnNoteOn, a lane that starts at frame k > 0 reads hz 0 (cents kIdleCents)
// before k: its state is reset to StateInit at the start of that block, and with hz 0 an oscillator does not advance
// before the onset, so a voice's output from sample k on does not depend on where block boundaries fall (tested
// for block sizes 1…128). Those pre-onset frames have gain 0 and are excluded from the mix, the level meters and the
// image's reducers/sends by the per-sample gain (VoiceGroupIO::gain), so a NaN there (0 / 0) reaches nothing outside
// the lane; a stateful loop that divides by voice.hz should still use max(voice.hz, ε) (docs/QUESTIONS.md Q62).
// Lanes are reset generically from the image's StateLayout: every per-lane word of each voice state entry (word
// index ≡ lane mod 8, pq/dsp/atom_state.h); the shared words (noise counter, delay-line header, phasor
// incScale/halfSr) are kept; a delay line's ring is reset in O(1) through its per-lane DelayState::start. Any
// backend whose StateArena follows atom_state.h works.
//
// ---- Gain, stealing, mixing ----------------------------------------------------------------------------------
// The engine mixes every voice `out` slot (not exports) into one global buffer per slot: Σ lanes gain · out over
// the lanes with gain ≠ 0 (a select, not a multiply: an idle lane's NaN or inf never reaches the mix), with gain
// per lane and sample: 0 before the onset and while free, 1 while sounding, a raised-cosine fade to 0 over
// stealFadeMs when the voice is stolen. The same gain is handed to the image (VoiceGroupIO::gain): audio-rate
// voice_sum and voice sends inside the patch select and weight lanes by it, voice_last reads its lane only where
// the gain ≠ 0 (docs/QUESTIONS.md Q61 option A); voice_highest / lowest / loudest keep the block lane mask.
// Stealing (live voices ≥ maxVoices): released voices are taken before held ones; among them the oldest
// (StealPolicy::Oldest) or the quietest (StealPolicy::Quietest: lowest peak of the monitored outputs over the last
// block; ties → oldest). The victim fades in its lane; the new note starts in a free lane at its frame. Without a
// free lane (headroom exhausted) the new note waits for the victim's fade and starts at the first block after it
// (VoiceStats::deferredStarts). StealPolicy::None drops notes beyond maxVoices.
// Same-note retrigger (retriggerSameNote): a note-on whose Hz equals (bit for bit) a RELEASED voice's Hz takes over
// that voice when the voice started before this block: no state reset, trigger pulse and gate rise at k (a visible
// edge for rise(voice.gate)), new velocity. The gate is low for ≥ 1 frame first: a note-on at the frame of the
// same pitch's note-off (abutting notes) re-attacks at k + 1 (the next block's frame 0 when k is the last frame).
// A held voice is never taken over: two simultaneous notes at one pitch (unison, SPEC §5) are two voices.
// Note lookup: NoteOff takes the newest held voice with the note id; Pitch the newest held one, else the newest
// released one. voice_last is the newest started voice; when its lane frees or is stolen it moves to the newest
// held or released voice.
// Overflow: events beyond maxEventsPerBlock in one block drop the newest NoteOns / Pitch events; NoteOff,
// AllNotesOff and AllSoundOff are always applied (note_events.h: EventBuffer degrades the same way when full). A
// block longer than io().maxBlockFrames (refused by the image: silence) still applies its events and voice ends.
//
// ---- Voice end (SPEC §7.7) -----------------------------------------------------------------------------------
// Checked at the end of each block; an ended lane is free from the next block on. The voice's TAIL (T1.x-silence) is
// the image's io().voiceTailFrames: the longest delay line of its voice stages (dsp::silenceTail), how long the voice
// can still sound after its outputs fell silent (an echo inside the voice). 0 for voices without delay lines.
//   1. doneSlot (a voice out/export, e.g. `out done : bool voice`): true (mask) or > 0.5 at the last frame of the
//      block, held or released (one-shots end themselves): without a tail the voice ends there; with one, its
//      envelope is done and the tail rings out: it ends once its monitored slots have stayed below silenceDb for the
//      tail (without monitored slots: the tail's length after done);
//   2. after note-off: when the monitored slots (monitorMask; default every voice `out`, not exports) have stayed
//      below silenceDb for max(silenceMs, the tail) (block-peak granular). Held voices never end on silence;
//   3. with neither monitored slots nor a done slot: releaseTimeoutMs after note-off.
// A stolen voice ends when its fade is over.
//
// ---- Hot swap (T1.9) -----------------------------------------------------------------------------------------
// Everything that depends on the image (its output slots, mix buffers, the lane reset/move spans of its state
// layout, its voice state blocks) is a VoiceBinding. prepareLanes() sets up the image-independent part (lanes,
// voice.* buffers, allocator state) from an ImageInterface; makeBinding() builds a binding for any image with the
// same geometry (maxVoiceGroups, maxBlockFrames, sample rate) off the audio thread; attach() switches bindings on
// the audio thread at a block boundary (RT-safe). Lane assignments, the voice.* render state and the allocator
// survive the switch, so voices keep sounding through a program swap (their state words are carried by the swap's
// state transfer, state_transfer.h). While a crossfade runs, attach(new, old) keeps the old image running on the
// same voice table: process(ctx, fadeCtx) runs both, lane resets and compaction moves apply to both images'
// state, and fadeMix(i) holds the old image's mixes. Voice end and levels follow the current (new) binding.
// prepare(image, config) = prepareLanes + makeBinding(config.doneSlot, config.monitorMask) + attach.

#include <cstdint>
#include <memory>
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
  float stealFadeMs = 3.0f;         // raised-cosine fade of a stolen voice
  float quietestMinAgeMs = 20.0f;   // StealPolicy::Quietest: voices younger than this (still in their attack, their
                                    // block peak not yet representative) are stolen only when every candidate is
                                    // (M1 review L9)
  float silenceDb = -90.0f;         // voice end: below this after release …
  float silenceMs = 20.0f;          // … for this long
  float releaseTimeoutMs = 2000.0f; // voice end without a done slot or monitored outputs
  double referenceHz = 261.626;     // voice.cents = 1200 · log2(hz / referenceHz) (SPEC §3.1 default C3)
  uint32_t monitorMask = 0;         // bit s: voice out/export slot s is monitored for silence and level (0: every
                                    // voice `out`); slots ≥ 32 cannot be named here
  uint32_t doneSlot = kNoSlot;      // index into io().voiceOut, or kNoSlot (findVoiceSlot(io, patch, "done"))
  uint32_t maxEventsPerBlock = 1024; // NoteOns + Pitch events applied per block; the newest beyond it are dropped
                                     // (VoiceStats::droppedEvents). Note-offs are never dropped
  uint32_t moveBudgetFrames = 32768; // compaction: delay-ring frames moved per block at most (bench/RESULTS.md)
};

// Voice groups an image needs for maxVoices plus one group of fade headroom.
inline uint32_t voiceGroupsFor(uint32_t maxVoices) noexcept { return (maxVoices + 7) / 8 + 1; }

// The voice out/export slot whose node is named `name` (e.g. "done") in the scheduled patch (Schedule::rewritten
// when set, else the patch given to schedule()), or kNoSlot. Not RT-safe.
uint32_t findVoiceSlot(const ImageInterface& io, const graph::Patch& patch, std::string_view name);

enum class LaneStatus : uint8_t { Free, Held, Released, Fading, Pending };

// The image-dependent part of a VoiceEngine (see "Hot swap" above). Built by VoiceEngine::makeBinding off the audio
// thread; owned by the caller (or by the engine for prepare()); must outlive its attachment.
struct VoiceBinding {
  struct Span {
    uint32_t firstWord, numWords;     // relative to the group's state block; numWords a multiple of 8
  };
  struct DelaySpan {
    uint32_t firstWord, capacity;     // a voice delay line's DelayState (8 lanes)
    uint32_t reach;                   // frames a read can reach back: floor(maxDelay) + 2
  };
  ProgramImage* image = nullptr;
  uint32_t numOutSlots = 0;
  uint32_t doneSlot = kNoSlot;        // index into io().voiceOut, or kNoSlot
  uint32_t tailFrames = 0;            // io().voiceTailFrames (voice end, T1.x-silence)
  std::vector<uint32_t> mixSlots;     // io().voiceOut index per mix
  std::vector<uint32_t> monitorSlots;
  AlignedBuffer<float> outBuf;        // per group and requested voice out slot: groupStride floats
  AlignedBuffer<float> mixBuf;        // per mix: stride floats
  std::vector<float*> outPtr;         // [g · numOutSlots + slot] (null: not requested)
  std::vector<VoiceGroupIO> gio;
  VoiceTable table;
  // Lane reset: spans of per-lane words in a voice group's state block, and the StateInit template.
  std::vector<Span> resetSpans;
  std::vector<Span> moveSpans;        // per-lane words moved as they are (everything but noise and delay lines)
  std::vector<DelaySpan> delays;
  AlignedBuffer<uint32_t> stateTemplate;
  std::byte* voiceState = nullptr;    // the image's voice state blocks (StateArena), voiceStateStride apart
  uint32_t voiceStateStride = 0;
};

struct VoiceStats {
  uint64_t noteOns = 0, noteOffs = 0, pitchEvents = 0;
  uint64_t retriggers = 0;      // same-note retriggers
  uint64_t steals = 0;          // voices faded out to make room
  uint64_t deferredStarts = 0;  // notes that waited for a fade (no free lane)
  uint64_t droppedNotes = 0;    // StealPolicy::None beyond maxVoices, or a deferred note replaced / released early
  uint64_t unmatchedEvents = 0; // note-off / pitch for a note id that is not sounding
  uint64_t lateEvents = 0;      // frame ≥ block length (clamped)
  uint64_t droppedEvents = 0;   // NoteOn / Pitch beyond VoiceConfig::maxEventsPerBlock
  uint64_t oversizeBlocks = 0;  // blocks longer than io().maxBlockFrames (events applied, image refused: silence)
  uint64_t skippedMoves = 0;    // compactions not done because the group's delay rings exceed moveBudgetFrames
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

  // Hot swap (T1.9). Off the audio thread: lanes, voice.* buffers and allocator state for images with this
  // interface's geometry (no binding is attached: process() does nothing until attach()). Clears all voices.
  Status prepareLanes(const ImageInterface& io, const VoiceConfig& config);
  // Off the audio thread, any time after prepareLanes (reads only fields that stay fixed until the next prepare):
  // a binding for `image`, whose io() must match the prepared geometry. doneSlot / monitorMask as in VoiceConfig,
  // for this image's slots.
  Result<std::unique_ptr<VoiceBinding>> makeBinding(ProgramImage* image, uint32_t doneSlot = kNoSlot,
                                                    uint32_t monitorMask = 0) const;
  // At a block boundary on the audio thread: run `current` from the next block on; `fadingOut` (or null) keeps
  // running beside it (process(ctx, fadeCtx)) until the next attach. RT-safe.
  void attach(VoiceBinding* current, VoiceBinding* fadingOut = nullptr) noexcept;

  // One block: ctx.frames ≤ io().maxBlockFrames, ctx.events (may be null) as in note_events.h. Fills ctx.voices
  // (the caller's global I/O, params, transport and sends pass through), runs the image, mixes the voice outs and
  // ends finished voices. With a fading-out binding attached and fadeCtx given, also runs the old image with
  // *fadeCtx (its own global I/O and params; frames, events and voices are taken from ctx) and mixes its voice
  // outs into fadeMix(). RT-safe.
  void process(ProcessContext& ctx, ProcessContext* fadeCtx = nullptr) noexcept;

  // The mixed voice `out` slots of the last block (frames floats each), in io().voiceOut order of the non-export
  // slots; mixSlot(i) is the io().voiceOut index of mix i. fadeMix: the same for the fading-out binding.
  uint32_t numMixes() const noexcept { return b_ ? static_cast<uint32_t>(b_->mixSlots.size()) : 0u; }
  const float* mix(uint32_t i) const noexcept { return b_->mixBuf.data() + static_cast<size_t>(i) * stride_; }
  uint32_t mixSlot(uint32_t i) const noexcept { return b_->mixSlots[i]; }
  const VoiceBinding* binding() const noexcept { return b_; }
  const VoiceBinding* fadingBinding() const noexcept { return fade_; }
  const float* fadeMix(uint32_t i) const noexcept { return fade_->mixBuf.data() + static_cast<size_t>(i) * stride_; }
  // Per group: bit l = lane l holds a voice (groups() entries). RT-safe.
  const uint32_t* laneMasks() const noexcept { return laneMask_.data(); }

  // Inspection (tests, UI reports). RT-safe.
  uint32_t lanes() const noexcept { return lanes_; }
  uint32_t groups() const noexcept { return groups_; }
  LaneStatus laneStatus(uint32_t lane) const noexcept { return status_[lane]; }
  uint32_t laneNote(uint32_t lane) const noexcept { return noteId_[lane]; }
  uint32_t laneOf(uint32_t noteId) const noexcept;   // the newest held, else released, else fading lane playing
                                                     // noteId, or kNoSlot
  uint32_t liveVoices() const noexcept { return live_; }  // held + released + deferred (waiting for a fade)
  // T3.17-fix2 (idle units, pq/engine/project_program.h): a block with no live voice and no event that the image is
  // not run for: only the engine clock moves (nothing else changes when no lane is in use). RT-safe.
  void advanceIdle(uint32_t frames) noexcept {
    clock_ += frames;
    ++stats_.blocks;
  }
  bool hasDeferredNote(uint32_t lane) const noexcept { return hasPending_[lane] != 0; }
  uint32_t activeGroups() const noexcept;            // groups with a non-free lane
  uint32_t groupsRunLastBlock() const noexcept { return groupsRun_; }
  // The voice.* buffer group g passed to the image in the last block (audio-rate: frames · 8; param: 8 floats),
  // the per-lane gain (frames · 8), and voice out slot s of group g (frames · 8, null if not requested).
  const float* signal(uint32_t g, VoiceSignal s) const noexcept { return sigPtr_[g * kVoiceSignalCount + static_cast<uint32_t>(s)]; }
  const float* gain(uint32_t g) const noexcept { return gainBuf_.data() + static_cast<size_t>(g) * groupStride_; }
  const float* voiceOut(uint32_t g, uint32_t slot) const noexcept { return b_->outPtr[g * b_->numOutSlots + slot]; }
  float laneLevel(uint32_t lane) const noexcept { return level_[lane]; }
  uint32_t lastLane() const noexcept { return lastLane_; }  // voice_last's lane, or kNoVoice
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
  uint32_t findNote(uint32_t noteId, bool released) const noexcept;
  uint32_t newestLive() const noexcept;
  uint32_t moveCost(uint32_t lane) const noexcept;
  void skipBlock(const EventList* events, uint32_t frames) noexcept;
  uint32_t findFreeLane() const noexcept;
  uint32_t pickVictim() const noexcept;
  void addOp(uint32_t frame, uint32_t lane, OpKind kind, const NoteEvent& e) noexcept;
  void setStatus(uint32_t lane, LaneStatus s) noexcept;
  // Rendering (per lane, signal buffers).
  void renderGroup(uint32_t g, uint32_t frames) noexcept;
  void renderSegment(uint32_t lane, uint32_t from, uint32_t to, const uint32_t* fill) noexcept;
  void applyOp(const LaneOp& op, bool writeTrigger = true) noexcept;
  void freeLane(uint32_t lane) noexcept;
  void moveLane(uint32_t from, uint32_t to) noexcept;
  void compact() noexcept;
  void resetLaneState(uint32_t lane) noexcept;
  // After the image.
  void mixOutputs(VoiceBinding& b, uint32_t frames) noexcept;
  void measure(uint32_t frames) noexcept;
  void endVoices(uint32_t frames, bool imageRan = true) noexcept;

  std::unique_ptr<VoiceBinding> own_;  // prepare()'s binding
  VoiceBinding* b_ = nullptr;          // current
  VoiceBinding* fade_ = nullptr;       // fading out (hot swap crossfade), or null
  VoiceConfig cfg_;
  uint32_t groups_ = 0, lanes_ = 0, maxBlock_ = 0, stride_ = 0, groupStride_ = 0;
  uint32_t fadeFrames_ = 1, silenceFrames_ = 1;
  int64_t releaseTimeoutFrames_ = 0;
  int64_t quietestMinAgeFrames_ = 0;
  float silenceLevel_ = 0.0f;
  float idleHz_ = 261.626f;           // voice.hz of a free lane (referenceHz)
  std::vector<float> fadeTable_;      // raised cosine: fadeFrames_ + 1 values from 1 down to 0
  double sampleRate_ = 48000.0;

  // Buffers handed to the image.
  AlignedBuffer<float> audioSig_;   // per group: hz, cents, gate, trigger (groupStride_ floats each)
  AlignedBuffer<float> paramSig_;   // per group: velocity, release, ratio, harmonic, unisonCount, index (8 each)
  AlignedBuffer<float> gainBuf_;    // per group: groupStride_
  std::vector<const float*> sigPtr_;  // [g · kVoiceSignalCount + s]
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
  std::vector<uint8_t> doneTail_;     // done fired with a tail to ring out (T1.x-silence)
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
  std::vector<uint32_t> fadeLeft_;    // > 0 while fading: frames to go
  std::vector<float> fadeFrom_;       // gain when the fade started
  std::vector<float> velocity_, release_, ratio_, harmonic_, unison_, index_, degree_, equave_, tuning_;
  std::vector<float> onsetHz_;  // T4.17: voice.noteHz, the Hz of the lane's latest note-on

  // Per block scratch.
  std::vector<LaneOp> ops_, sortedOps_;
  std::vector<LaneOp> carry_;         // ops for frame 0 of the next block (addOp)
  uint32_t numCarry_ = 0;
  uint32_t curFrames_ = 0;            // the block being allocated
  std::vector<uint32_t> floor_;       // per lane: no op before this frame in this block
  std::vector<uint8_t> hasCarry_;     // per lane: ops carried to the next block (no voice end, no move meanwhile)
  std::vector<uint32_t> opBegin_;     // per lane + 1 (counting sort)
  uint32_t numOps_ = 0;
  uint32_t numReady_ = 0;             // lanes in status Pending (start at the next block)
  bool opsLastBlock_ = true;          // opBegin_ holds non-zero offsets from the last block
  int64_t clock_ = 0;                 // engine frames before frame 0 of the current block (ages, timeouts)
  uint32_t live_ = 0;
  uint32_t lastLane_ = kNoVoice;
  uint32_t groupsRun_ = 0;
  VoiceStats stats_;
};

}  // namespace pq::engine
