#pragma once

// Drives a ProgramImage from the audio callback path (T1.7): device → CallbackRunner → BlockAdapter → ImageRunner →
// ProgramImage::process. A minimal engine for one global patch (no voices, no events; T1.8/T3.16 build the real
// engine on the same pieces):
//   - device input channel c feeds the image's global root input slot c (io().globalIn);
//   - device output channel c plays the c-th global `out` slot (exports are not played); a mono patch plays on
//     every channel; channels without a slot are silent;
//   - the output is multiplied by `gain` and, with `limit`, clamped to [−1, 1] (a safety net for test playback);
//   - the param block holds the declared defaults (writable before start through params());
//   - the transport is stopped at beat 0 unless set before start (it then advances by beatsPerFrame).
// With setVoices (T1.8) the image runs through a VoiceEngine: before each image block the event source fills the
// block's events (frame offsets relative to that block), and when the image has no global `out` the VoiceEngine's
// mixed voice `out`s are played instead (channel c ← mix c; a single mix on every channel).

#include <cstdint>
#include <span>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/engine/audio_block.h"
#include "pq/engine/note_events.h"
#include "pq/engine/program_image.h"
#include "pq/engine/voices.h"

namespace pq::engine {

// Produces the note events of [sampleTime, sampleTime + frames) into `out` (already cleared), frames relative to
// sampleTime. Called on the audio thread: must be RT-safe.
struct EventSource {
  void (*fn)(void* user, int64_t sampleTime, uint32_t frames, EventBuffer& out) noexcept = nullptr;
  void* user = nullptr;
};

// Called at the start of every image block, before the event source and the image (T3.16: Timeline::blockHook
// advances the timeline, writes the transport snapshot the image sees and the automated param values). With a hook
// set, the runner's own transport() is whatever the hook writes (it no longer advances by beatsPerFrame). Called on
// the audio thread: must be RT-safe.
struct BlockHook {
  void (*fn)(void* user, int64_t sampleTime, uint32_t frames, TransportSnapshot& transport,
             std::span<float> params) noexcept = nullptr;
  void* user = nullptr;
};

// The device channel map ImageRunner and ProgramHost share (one rule): channel c plays played[c] (the global `out`
// slots, or the voice mixes, in order); a single played slot plays on every channel; others are graph::kNone.
inline void mapChannels(std::span<const uint32_t> played, uint32_t (&chanSlot)[kMaxChannels]) noexcept {
  for (uint32_t c = 0; c < kMaxChannels; ++c)
    chanSlot[c] = played.empty() ? graph::kNone : played.size() == 1 ? played[0] : c < played.size() ? played[c] : graph::kNone;
}

class ImageRunner {
public:
  // Off the audio thread; the image must outlive the stream. Returns false for channel counts above kMaxChannels.
  bool prepare(ProgramImage* image, uint32_t numIn, uint32_t numOut, float gain = 1.0f, bool limit = false);

  // After prepare, before start: run the image through `voices` (prepared for the same image) with events from
  // `source`. Allocates the event buffer (eventCapacity events per image block).
  void setVoices(VoiceEngine* voices, EventSource source, uint32_t eventCapacity = 1024);
  // After prepare, before start: call `hook` at the start of every image block (BlockHook above).
  void setBlockHook(BlockHook hook) noexcept { hook_ = hook; }

  AudioCallback callback() noexcept { return {&ImageRunner::onBlock, this}; }
  // One engine block (any frame count: blocks above the image's maxBlockFrames are split). RT-safe.
  void process(const AudioBlock& block) noexcept;

  std::span<float> params() noexcept { return params_; }       // set before start
  TransportSnapshot& transport() noexcept { return transport_; }  // set before start
  // Global output slot played on device channel c, or graph::kNone.
  uint32_t slotForChannel(uint32_t c) const noexcept { return c < kMaxChannels ? chanSlot_[c] : graph::kNone; }
  uint64_t blocks() const noexcept { return blocks_; }

private:
  static void onBlock(void* self, const AudioBlock& b) noexcept {  // RT-safe
    static_cast<ImageRunner*>(self)->process(b);
  }

  ProgramImage* image_ = nullptr;
  uint32_t numIn_ = 0, numOut_ = 0;
  float gain_ = 1.0f;
  bool limit_ = false;
  std::vector<float> params_;
  TransportSnapshot transport_;
  AlignedBuffer<float> scratch_;    // one maxBlock buffer per global output slot
  std::vector<float*> slotPtrs_;
  uint32_t chanSlot_[kMaxChannels] = {};
  VoiceEngine* voices_ = nullptr;
  EventSource source_;
  BlockHook hook_;
  EventBuffer events_;
  bool playMixes_ = false;          // channels play VoiceEngine mixes (chanSlot_ indexes mixes)
  uint64_t blocks_ = 0;
};

}  // namespace pq::engine
