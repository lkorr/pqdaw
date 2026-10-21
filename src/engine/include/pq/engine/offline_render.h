#pragma once

// Offline rendering (T1.10, SPEC §9.8: "same Program, deterministic"). renderOffline runs a ProgramImage (any
// backend: interpreter or JIT) through exactly the code the audio device runs — ImageRunner (+ VoiceEngine and an
// event source) under DenormalGuard and an RtScope per chunk — but driven by a loop instead of a device clock:
//   - fixed engine block (RenderRequest::blockFrames): the output depends only on the image, the request and the
//     events, never on timing, threads or the wall clock (noise atoms are seeded at compile time);
//   - as fast as the CPU allows (the wall time is measured for RenderStats only);
//   - blocks are rendered into a chunk buffer (chunkFrames) and handed to the AudioSink between chunks, so file
//     I/O never interleaves with the per-block loop, and the loop itself does not allocate (RtScope-checked).
// Global root `in`s read silence. Channel mapping as ImageRunner: channel c plays the c-th global `out` (a mono
// patch: one channel by default), or, for an instrument (voice `out`s only) driven by `voices`, the c-th voice mix.
// Not RT-safe as a whole (the sink does I/O); call it from a render/tool thread.

#include <cstdint>
#include <span>
#include <vector>

#include "pq/core/result.h"
#include "pq/engine/image_runner.h"
#include "pq/engine/program_image.h"
#include "pq/engine/voices.h"

namespace pq::engine {

// Where rendered audio goes. Called from the rendering thread, between chunks.
class AudioSink {
public:
  virtual ~AudioSink() = default;
  virtual Status begin(uint32_t numChannels, uint32_t sampleRate, uint64_t totalFrames) = 0;
  virtual Status write(const float* const* channels, uint32_t frames) = 0;  // planar
  virtual Status end() = 0;
};

// Keeps the render in memory (tests, golden comparisons).
class BufferSink final : public AudioSink {
public:
  Status begin(uint32_t numChannels, uint32_t sampleRate, uint64_t totalFrames) override;
  Status write(const float* const* channels, uint32_t frames) override;
  Status end() override { return Ok{}; }
  std::vector<std::vector<float>> channels;  // planar
  uint32_t sampleRate = 0;
};

struct RenderRequest {
  ProgramImage* image = nullptr;     // required
  uint64_t frames = 0;               // length of the render
  uint32_t blockFrames = 64;         // the fixed engine block, ≤ image->io().maxBlockFrames
  uint32_t numChannels = 0;          // 0: one per played slot (global outs, else voice mixes), at least 1
  float gain = 1.0f;                 // linear; no clipping (golden files keep the raw signal)
  VoiceEngine* voices = nullptr;     // prepared for `image` (VoiceEngine::prepare), or null for a global patch
  EventSource events;                // with `voices`: the note events (e.g. NoteListSource)
  uint32_t eventCapacity = 1024;     // events per engine block (more are dropped and counted by the EventBuffer)
  std::span<const float> params;     // empty: the declared defaults; else one value per io().params slot
  TransportSnapshot transport;       // at frame 0 (advances by beatsPerFrame while playing; ignored with a hook)
  BlockHook hook;                    // per block before the events (T3.16: Timeline::blockHook drives transport,
                                     // params and, through `events`, the notes)
  uint32_t chunkFrames = 8192;       // frames per sink write (rounded up to whole blocks)
};

struct RenderStats {
  uint64_t frames = 0;
  uint64_t blocks = 0;
  uint32_t numChannels = 0;
  uint64_t rejectedBlocks = 0;       // image->rejectedBlocks() during the render (0 unless misconfigured)
  double wallSeconds = 0.0;          // rendering only (sink time excluded)
  double realtimeFactor = 0.0;       // audio seconds per wall second
};

// Errors: no image, blockFrames 0 or above the image's maximum, params of the wrong size, a sink error.
Result<RenderStats> renderOffline(const RenderRequest& request, AudioSink& sink);

}  // namespace pq::engine
