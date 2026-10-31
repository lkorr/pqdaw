#pragma once

// The clip player (T5.4; SPEC §7.2 clipPlayer, §7.7, §9.4; docs/plans/M5.md T5.4): the engine source behind every
// `clipPlayer(clip: k)` atom. The timeline owns one; it renders, per engine block, the audio of every clip slot k that
// has a clip under the playhead into the slot's two channels, which the images read through ProcessContext::clipIn
// (pq/engine/image_host.h: a clipPlayer external points at its slot's channel, both backends alike).
//
// ---- Per block (audio thread; the timeline drives it) ---------------------------------------------------------------
//   beginBlock(frames)              the slots written last block are cleared lazily (on their first write)
//   setImage(image)                 an AudioSequenceImage swap (see "Swaps")
//   releaseAll(frame) / seek(s)     stop, locate, arrangement-loop wrap: every sounding voice fades out over the
//                                   declick (2 ms) from that frame on its own clock; after a seek, clips under the
//                                   playhead start where they are, with a 2 ms fade in (no click)
//   play(frame, s0, len)            one run of the playing transport over song samples [s0, s0 + len)
//   idle(frames)                    the transport is stopped: releasing voices finish their fade
//   endBlock()                      publishes which slots have audio: active()[k], channels()[k · 2 + c] (a slot without
//                                   audio points at shared zeros)
// A VOICE is one clip playing (or about to: a streamed clip's voice is created `lookaheadSeconds` before it starts and
// claims its stream then, so the disk thread has filled it when the clip begins). A slot has at most two audible
// voices in normal playback (a crossfade), plus releasing ones after a swap or a locate.
//
// ---- Cost (§7.7 sleep) ------------------------------------------------------------------------------------------------
// With no voice alive and the next clip start (minus the look-ahead) beyond the run, play() is one compare: a track
// whose clip slot has nothing under the playhead costs nothing, its slot stays inactive and its unit can stay idle
// (pq/engine/project_program.h). A voice renders its run in one fused pass per contiguous source piece: the source
// position law (pq/engine/audio_sequence.h) gives a 32.32 start and step; step = ±1 frame from an integer position is a
// straight copy (dsp::gainCopy), anything else the windowed-sinc resampler (dsp::sincResample, stretched for rates
// above 1; above rate 2 on a resident sample's decimated copies when it has them, media::SampleMips, so the cost stays
// that of a read at rate ≤ 2); the envelope is one constant gain over its flat middle, else one per-frame buffer
// computed for the run.
// The block is split at clip boundaries and loop wraps, never tested per sample.
//
// ---- Sources ----------------------------------------------------------------------------------------------------------
// A fully resident sample is read in place (its guard frames cover the filter's reach at the edges; reads further out
// are gathered into a scratch buffer with zeros). A streamed one (media::SamplePool: resident head + disk stream) is
// read through a media::SampleCursor into the voice's history buffer (the frames the filter still needs plus the new
// ones; at most `historyFrames`, runs are cut into chunks that fit), so the ring is read once, in order. A looped
// RePitch voice jumps back at every wrap: `lookaheadSeconds` before a wrap it claims a second cursor at the loop's
// other end (a little before the first frame the filter will need there), which the disk thread fills meanwhile, and
// the wrap adopts it (T5.x-perf-audio, Q151 item 7: the wrap used to claim a fresh stream and underrun). Loops inside
// the resident head need neither. Frames the
// disk thread has not delivered read as silence (counted: media::StreamPool::underruns, ClipPlayerStats::missingFrames).
// Offline renders install a StreamFill that blocks until the stream has the frames (SamplePool::waitFilled), so a
// render never depends on disk speed.
//
// ---- Swaps (click-free edits) -------------------------------------------------------------------------------------------
// A new image is taken at a block start, never while voices of the image before are still fading (swapping()). A
// sounding voice whose clip is still in the new image (same slot and id) with the same source law (same sample, warp,
// direction, anchor / step or start / content start / loop / speed) keeps playing seamlessly, following the new
// envelope; a gain change ramps over the declick. Any other voice (a moved, re-pitched, reversed or deleted clip)
// fades out over the declick while the new image's clips under the playhead start with a fade in. The voices of the
// old image keep it alive until their fades end (the timeline holds it: Timeline's audio hazards).

#include <cstdint>
#include <optional>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/time.h"
#include "pq/engine/audio_sequence.h"
#include "pq/engine/program_image.h"
#include "pq/media/stream.h"

namespace pq::engine {

struct ClipPlayerConfig {
  uint32_t slots = 128;             // clip slots (ProcessContext::numClipSlots)
  uint32_t maxBlockFrames = 1024;   // the engine block
  double sampleRate = 48000.0;
  uint32_t maxVoices = 512;         // voices alive at once over every slot (beyond: a clip does not start, counted)
  uint32_t streamBuffers = 64;      // streamed voices at once (each has a history buffer)
  uint32_t historyFrames = 8192;    // per streamed voice (stereo planar floats: 64 KiB)
  media::StreamPool* streams = nullptr;  // null: frames past a streamed sample's head read as silence
  double lookaheadSeconds = 0.5;    // a clip's voice exists (and claims its stream) this long before it starts
  double declickSeconds = 0.002;    // stop / locate / wrap / swap fades
};

struct ClipPlayerStats {
  uint64_t voicesStarted = 0, voicesEnded = 0;
  uint64_t voicesRefused = 0;     // no free voice: the clip stayed silent
  uint64_t noHistory = 0;         // a streamed voice found no free history buffer: silent
  uint64_t missingFrames = 0;     // frames read as silence because the disk thread was late
  uint64_t swaps = 0, kept = 0, faded = 0;   // image swaps; voices kept / faded out by them
  uint64_t copyFrames = 0, sincFrames = 0;   // output frames rendered by each path (per voice)
  uint64_t scans = 0;             // runs that looked for clips to start (the rest cost one compare)
  uint64_t loopClaims = 0;        // looped RePitch voices on streamed samples: next loop passes claimed ahead (Q151)
  uint64_t loopAdopts = 0;        // ... and taken over at their wrap
  uint64_t mipFrames = 0;         // output frames read from a sample's decimated copies (Q151 item 1)
};

// Offline renders: called before a voice reads `frames` frames that a stream (still) has to deliver; it should block
// until they are there (media::SamplePool::waitFilled). Called on the rendering thread.
using StreamFill = void (*)(void* user, media::StreamId id, int64_t frames);

class ClipPlayer {
public:
  // Not RT-safe: allocates every buffer and voice, prepares the resampler's base tables.
  explicit ClipPlayer(const ClipPlayerConfig& config);
  ~ClipPlayer();  // non-RT, after the audio thread stopped: releases every claimed stream
  ClipPlayer(const ClipPlayer&) = delete;
  ClipPlayer& operator=(const ClipPlayer&) = delete;

  // ---- audio thread (all RT-safe) ----
  void beginBlock(uint32_t frames) noexcept;
  // A voice of the previous image still fades: take no newer image yet.
  bool swapping() const noexcept { return prevUsers_ != 0; }
  // The image to play from this block on (null: none). `s` = the song sample at frame 0, `playing` the transport.
  void setImage(const AudioSequenceImage* next, SampleTime s, bool playing) noexcept;
  const AudioSequenceImage* image() const noexcept { return img_; }
  const AudioSequenceImage* previousImage() const noexcept { return prevUsers_ != 0 ? prev_ : nullptr; }
  void play(uint32_t frame, SampleTime s0, uint32_t len) noexcept;
  void releaseAll(uint32_t frame) noexcept;
  void seek(SampleTime s) noexcept;
  void idle(uint32_t frames) noexcept;
  void endBlock() noexcept;

  const float* const* channels() const noexcept { return ptrs_.data(); }  // [slot · kClipChannels + c]
  const uint8_t* active() const noexcept { return active_.data(); }       // per slot: audio this block
  uint32_t numSlots() const noexcept { return cfg_.slots; }
  uint32_t liveVoices() const noexcept { return static_cast<uint32_t>(live_.size()); }
  bool anyActive() const noexcept { return !touched_.empty(); }
  const ClipPlayerStats& stats() const noexcept { return stats_; }

  // Non-RT, while the audio thread does not run (offline renders): see StreamFill. Null removes it.
  void setStreamFill(StreamFill fn, void* user) noexcept {
    fill_ = fn;
    fillUser_ = user;
  }

  // Where a voice of clip `c` (of `img`) starting at song sample s claims its stream: the cursor's first frame and
  // direction (media::SampleCursor::start), exactly as the player computes it, for SamplePool::prefetch. nullopt: the
  // clip is not under s, or has no sample. `blockFrames` = the engine block (RePitch rates are per block). RT-safe
  // (the player computes its own claims with it).
  struct StreamStart {
    int64_t frame = 0;
    media::Direction dir = media::Direction::Forward;
  };
  static std::optional<StreamStart> streamStart(const AudioSequenceImage& img, const AudioSequenceImage::Clip& c,
                                                SampleTime s, uint32_t blockFrames) noexcept;  // RT-safe

private:
  enum class State : uint8_t { Pending, Playing, Releasing };
  struct Reader {
    uint32_t buf = 0xFFFFFFFFu;  // history buffer index
    int64_t lo = 0, hi = 0;      // file frames [lo, hi) held, starting at buffer offset `off`
    uint32_t off = 0;
    media::Direction dir = media::Direction::Forward;
    bool started = false;
    media::SampleCursor<media::PooledSample> cursor;  // streamed samples are pool entries (stable across swaps)
    // Looped RePitch (Q151 item 7): the next loop pass, claimed ahead of a wrap (valid until a wrap adopts it; every
    // pass starts at the same place, so a claim no wrap needed stays good for the next one). Forward: it reads from
    // aheadFrame on; reverse: its reads end at aheadFrame.
    media::SampleCursor<media::PooledSample> ahead;
    int64_t aheadFrame = 0;
    media::Direction aheadDir = media::Direction::Forward;
    bool aheadValid = false;
  };
  struct Voice {
    const AudioSequenceImage* img = nullptr;
    const AudioSequenceImage::Clip* clip = nullptr;
    uint32_t slot = 0;
    State state = State::Pending;
    SampleTime vs = 0;                 // song sample of the voice's next output frame (its own clock once releasing)
    uint32_t inPos = 0, inLen = 0;     // a start in the middle of the clip: fade in
    uint32_t relPos = 0, relLen = 0;   // releasing: fade out
    float gainFrom = 1.0f;             // a swap that changed the gain: ramp from gainFrom over rampLen
    uint32_t rampPos = 0, rampLen = 0;
    SampleTime lastWrap = INT64_MIN;   // RePitch loops: the last wrap at or before vs (declick)
    SampleTime nextWrap = INT64_MAX;   // ... and the next one the last run saw coming
    Reader reader;
  };
  // One contiguous piece of source positions for a run of output frames.
  struct Piece {
    uint32_t at = 0, frames = 0;  // output frames [at, at + frames) of the run
    int64_t pos = 0, step = 0;    // 32.32 source position of the first frame, per frame
    bool wrap = false;            // RePitch: the piece starts at a loop wrap
  };

  bool hasVoice(uint32_t slot, const AudioSequenceImage::Clip* c) const noexcept;
  void startDue(SampleTime s0, SampleTime s1) noexcept;
  void startVoice(uint32_t slot, const AudioSequenceImage::Clip& c, SampleTime s0) noexcept;
  void endVoice(uint32_t liveIndex) noexcept;
  void releaseVoice(Voice& v) noexcept;
  // Renders [frame, frame + n) of the block from the voice's clock; false when the voice is over.
  bool renderVoice(Voice& v, uint32_t frame, uint32_t len) noexcept;
  void renderRun(Voice& v, uint32_t frame, SampleTime s, uint32_t n) noexcept;
  uint32_t pieces(Voice& v, SampleTime s, uint32_t n, Piece* out, SampleTime* nextWrap) noexcept;
  void read(Voice& v, const media::SampleView& view, const Piece& p, float* const* out, const float* env, float gain) noexcept;
  bool ensure(Voice& v, const media::SampleView& view, int64_t first, int64_t last, bool forward, const float** base,
              int64_t* baseFrame) noexcept;
  void claimAhead(Voice& v, const media::SampleView& view, int64_t step) noexcept;  // Q151 item 7
  static void stopReader(Reader& r) noexcept;
  float* touch(uint32_t slot) noexcept;  // the slot's channel 0 (channel 1 follows), cleared on its first write
  void seekCursors(SampleTime s) noexcept;

  ClipPlayerConfig cfg_;
  uint32_t declick_ = 0;
  SampleTime lookahead_ = 0;
  const AudioSequenceImage* img_ = nullptr;
  const AudioSequenceImage* prev_ = nullptr;
  uint32_t prevUsers_ = 0;              // live voices still reading prev_
  std::vector<Voice> voices_;
  std::vector<uint32_t> free_;          // voice indices
  std::vector<uint32_t> live_;          // voice indices, in start order
  std::vector<uint32_t> freeBuffers_;   // history buffers
  AlignedBuffer<float> history_;        // streamBuffers × 2 × historyFrames
  AlignedBuffer<float> scratch_;        // 2 × historyFrames: gathered resident reads
  AlignedBuffer<float> out_;            // slots × 2 × maxBlockFrames
  AlignedBuffer<float> env_;            // maxBlockFrames
  AlignedBuffer<float> zeros_;          // maxBlockFrames
  std::vector<const float*> ptrs_;      // slots × 2
  std::vector<uint8_t> active_;         // slots
  std::vector<uint8_t> touchedFlag_;    // slots
  std::vector<uint32_t> touched_, touchedBefore_;
  std::vector<uint32_t> cursor_;        // per slot of img_: the first clip not yet started
  SampleTime nextStart_ = INT64_MIN;    // min over slots of the cursor's playStart (INT64_MIN: scan at the next run)
  uint32_t frames_ = 0;
  StreamFill fill_ = nullptr;
  void* fillUser_ = nullptr;
  ClipPlayerStats stats_;
};

}  // namespace pq::engine
