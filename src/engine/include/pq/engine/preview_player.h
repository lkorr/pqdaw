#pragma once

// The browser's preview player (T5.10; SPEC §9.1 "preview through a cue bus"; docs/plans/M5.md T5.10): plays one sample
// file at a time, mixed after the project engine into the main outputs or the cue output pair (the app hands
// audioCallback() to AudioOutput::setCueSource).
//
// It is a one-slot clip player (pq/engine/clip_player.h) over a one-clip AudioSequenceImage, so a preview reads the
// sample exactly as a clip would: resident data in place or the T5.2 pool's streams (the selected item's head is loaded
// on selection, the rest streams), the T5.4 windowed-sinc resampler when the file's rate is not the device's, 2 ms
// declicks at the start (the clip's edge declick), on stop and on a scrub (a locate: the sounding voice fades out while
// the new position fades in). Raw plays the file at its own speed (warp Off); Sync re-pitches it to the project tempo
// when its BPM is known (warp Re-Pitch: the clip follows a tempo map of the project tempo; stretching without a pitch
// change comes with M6).
//
// Threads: play / scrub / stop / setGain / collect on one non-RT thread (the UI): they build the image (allocates) and
// hand it over through a lock-free command ring; process() on the audio thread. When stopped and faded out, process()
// returns after one ring check (the player sleeps: no clip-player work at all), and the app also detaches the callback
// from the output then (state() reports Stopped), so a stopped preview costs nothing.
//
// Lifetime: an image stays alive (UI side) until the audio thread has acknowledged the command that replaced it and no
// voice of the clip player still reads it (the player publishes the images it uses after every block); collect()
// frees the rest. The images keep their samples (media::SampleRef) alive.

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "pq/core/result.h"
#include "pq/core/spsc_ring.h"
#include "pq/core/time.h"
#include "pq/engine/audio_block.h"
#include "pq/engine/audio_sequence.h"
#include "pq/engine/clip_player.h"
#include "pq/media/sample_pool.h"

namespace pq::engine {

struct PreviewConfig {
  double sampleRate = 48000.0;      // the device rate the preview renders at
  uint32_t maxBlockFrames = 1024;   // process() splits longer device buffers
  media::StreamPool* streams = nullptr;  // the pool's streams (null: frames past a streamed sample's head are silence)
};

struct PreviewOptions {
  bool sync = false;        // Re-Pitch to `projectBpm` (needs fileBpm > 0; else Raw)
  double fileBpm = 0.0;
  double projectBpm = 120.0;
};

struct PreviewStats {
  uint64_t blocks = 0;        // process() calls
  uint64_t sleptBlocks = 0;   // calls that returned at once (stopped, nothing fading)
  uint64_t renderedBlocks = 0;
  uint64_t commands = 0;
};

class PreviewPlayer {
public:
  explicit PreviewPlayer(const PreviewConfig& config);  // not RT-safe: allocates the clip player
  ~PreviewPlayer();
  PreviewPlayer(const PreviewPlayer&) = delete;
  PreviewPlayer& operator=(const PreviewPlayer&) = delete;

  // ---- UI thread ----
  // Plays `sample` from `startSeconds` (file time). A playing preview fades out first. Errors: an empty sample, a full
  // command ring (the audio thread is not running: call collect / try again).
  Status play(media::SampleRef sample, double startSeconds, const PreviewOptions& options);
  // Jumps the playing preview to `fileSeconds` (scrub on click): fade out, fade in there. False when nothing plays.
  bool scrub(double fileSeconds);
  void stop();
  void setGain(float linear);  // ramps over one block
  enum class State : uint8_t { Stopped, Playing };
  // Playing from play() until the end of the file (or stop()) has been reached and its fade has finished (the
  // output can stop calling the player then without cutting a fade).
  State state() const noexcept;
  // Where the preview is in the file (seconds), while playing; −1 when stopped.
  double positionSeconds() const noexcept;
  // The file length of the current preview (seconds; 0 when none).
  double lengthSeconds() const noexcept { return lengthSeconds_; }
  // Frees images the audio thread no longer uses. Returns how many are still held.
  size_t collect();
  const PreviewStats& stats() const noexcept { return stats_; }  // written by the audio thread (tests read it after)

  // ---- audio thread ----
  // Adds the preview into out[0], out[1] (a single channel gets the left one). RT-safe.
  void process(float* const* out, uint32_t channels, uint32_t frames) noexcept;
  // An AudioCallback over process() (block.out / numOut / frames). RT-safe.
  AudioCallback audioCallback() noexcept { return {&PreviewPlayer::callback, this}; }

private:
  static void callback(void* self, const AudioBlock& b) noexcept;
  enum class Cmd : uint8_t { Play, Scrub, Stop };
  struct Command {
    Cmd cmd = Cmd::Stop;
    const AudioSequenceImage* image = nullptr;
    SampleTime at = 0;       // song sample to (re)start at
    SampleTime end = 0;      // Play: where the clip ends (song samples)
    uint64_t seq = 0;
  };
  bool apply(const Command& c) noexcept;  // false: retry next block (the clip player is still fading a swap)
  void render(float* const* out, uint32_t channels, uint32_t frames) noexcept;
  SampleTime songAt(double fileSeconds) const noexcept;  // UI thread: the current image's song sample of a file time

  PreviewConfig cfg_;
  ClipPlayer player_;
  SpscRing<Command> ring_{64};
  // Audio thread.
  bool playing_ = false;           // the transport of the preview runs
  SampleTime s_ = 0;               // song sample of the next frame
  SampleTime end_ = 0;
  Command pending_{};              // a command that had to wait for a swap's fade
  bool hasPending_ = false;
  float gain_ = 1.0f;
  PreviewStats stats_;
  // Shared.
  std::atomic<uint64_t> acked_{0};                  // seq of the last command the audio thread applied
  std::atomic<const AudioSequenceImage*> using_[2]{};  // the clip player's current and previous images
  std::atomic<int64_t> pos_{-1};                    // s_ while playing (−1: stopped)
  std::atomic<bool> busy_{false};                   // playing or a voice still fading (state())
  std::atomic<float> targetGain_{1.0f};
  // UI thread.
  struct Held {
    std::unique_ptr<AudioSequenceImage> image;
    uint64_t lastSeq = 0;  // the last command that referenced it
  };
  std::vector<Held> held_;
  const AudioSequenceImage* current_ = nullptr;
  uint64_t seq_ = 0;
  double lengthSeconds_ = 0.0;
  double speed_ = 1.0;     // file seconds per song second (Sync: projectBpm / fileBpm)
  uint32_t clipId_ = 0;
};

}  // namespace pq::engine
