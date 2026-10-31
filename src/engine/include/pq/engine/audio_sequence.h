#pragma once

// The audio sequence image (T5.4; docs/plans/M5.md T5.4): what the timeline's clip player (pq/engine/clip_player.h)
// plays on each clip slot, as an immutable image built off the audio thread and published like a SequenceImage
// (Timeline::publishAudio). The engine never includes `document`: the app turns doc::lowerAudioTrack's AudioTrackDesc
// into the plain data below (one AudioTrackDesc per clip slot) and hands over the samples the clips read.
//
// ---- Placement (song samples at the engine rate sr; the tempo map the image was built with) ------------------------
// A clip's BODY is [S0, S1) with S0 = ticksToSamples(start), S1 = ticksToSamples(start + length): a clip starts on
// exactly the sample its start tick rounds to, whatever the tempo map (the sample-exact placement criterion).
//   Off      source position (frames, fractional) at song sample s:
//              forward  f(s) = contentStartFrame + (s − S0) · k        k = speed / sr (source frames per output frame)
//              reverse  f(s) = contentStartFrame + (S1 − 1 − s) · k    (the forward frames, mirrored in the sample
//                                                                      domain: at k = 1 exactly the forward frames
//                                                                      backwards; doc::sourceFrameAt's continuous
//                                                                      mirror is within one frame of it)
//            computed in 32.32 fixed point from an anchor and an integer step (no accumulation, so a position depends
//            on s alone: renders are bit-identical whatever the block size).
//   RePitch  f(s) = clipContentAt(contentStart, loop, o) · framesPerTick with o = samplesToTicks(s) − start (reverse:
//            length − o), evaluated exactly at every block end and linear in between (the rate is constant within a
//            block: under a tempo ramp the speed is re-anchored each block, so nothing drifts), the block split where
//            the loop wraps.
// The clip PLAYS over [P0, P1) ⊇ body: a crossfade with the previous clip (crossfadeIn, centred on the boundary)
// starts it early, one with the next clip lets it play on (the source laws simply continue).
//
// ---- Envelope (one product per output frame, fused into the resampler's output pass) ------------------------------
//   gain                                  linear (0 = silence: the clip renders nothing)
//   × fade in   over [S0, S0 + F_in)      the automation curve law (Q39, pq/core/automation_curve.h) from 0 to 1
//   × fade out  over [S1 − F_out, S1)     the same law from 1 to 0
//   × crossfade in / out                  equal power (sin / cos of π/2 · u over the window; the two clips' windows
//                                         are the same samples, so the gains are exactly complementary in power) for
//                                         unrelated material; equal gain (u / 1 − u: complementary in amplitude) when
//                                         the two clips read the same sample with one continuous source law across the
//                                         window (a split crossfaded back together: the material is identical, so
//                                         equal power would swell by +3 dB; M5 review M7, QUESTIONS Q?-T5.x-fix-a)
//   × loop-wrap declick (RePitch loops)   linear over `declick` frames on both sides of every wrap
// Edge declicks (AudioBuildOptions::edgeDeclick, the plan's "create fades on clip edges", 2 ms): an edge without a
// crossfade whose user fade is shorter than the declick gets a linear declick of that length instead. An edge with a
// crossfade has neither a user fade nor a declick: the crossfade replaces both. Fades are clamped to the body
// (doc::lowerAudioTrack clamps them in ticks already).
//
// ---- Samples ----------------------------------------------------------------------------------------------------
// ClipSample: a pool entry (media::PooledSample: its residency is re-read every block, so a demotion to streaming is
// followed) or a fixed in-memory view (tests, previews), plus an owner the image keeps alive. A clip whose sample is
// missing (or not playable) renders nothing.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "pq/core/clip_time.h"
#include "pq/core/result.h"
#include "pq/core/time.h"
#include "pq/media/sample_pool.h"
#include "pq/media/stream.h"

namespace pq::engine {

struct ClipSample {
  std::shared_ptr<const media::PooledSample> pooled;  // non-null: read pooled->view() (current residency)
  media::SampleView fixed;                            // pooled null: these frames ([−guard, frames + guard) readable)
  std::shared_ptr<const void> owner;                  // keeps `fixed`'s frames alive
  // The frames to read now (media::SampleCursor's Sample concept). RT-safe: one acquire load for a pool entry.
  const media::SampleView& view() const noexcept { return pooled ? pooled->view() : fixed; }
  // The same sample in two images (a swap keeps a playing voice only on the same sample).
  bool sameAs(const ClipSample& o) const noexcept {
    return pooled ? pooled == o.pooled : (!o.pooled && fixed.data[0] == o.fixed.data[0] && fixed.frames == o.fixed.frames);
  }
};

enum class ClipWarp : uint8_t { Off, RePitch };

// One clip as the document lowered it (doc::AudioClipDesc, plain data).
struct AudioClipDesc {
  uint32_t id = 0;              // the document's clip id (an image swap keeps a playing clip by id)
  uint32_t sample = 0;          // index into the build's samples (out of range: silent)
  Ticks start = 0;
  Ticks length = 1;
  ClipWarp warp = ClipWarp::Off;
  bool reverse = false;
  bool playable = true;         // false: silent (inactive clip, missing sample)
  // Off.
  double contentStartFrame = 0.0;  // the source frame at the clip start (forward)
  double speed = 0.0;              // source frames per second (the file's rate × transpose × detune)
  // RePitch.
  Ticks contentStart = 0;
  ContentLoop loop;
  double framesPerTick = 0.0;      // source frames per content tick
  // Envelope (ticks; doc::lowerAudioTrack has clamped them).
  float gain = 1.0f;
  Ticks fadeIn = 0, fadeOut = 0;
  float fadeInCurve = 0.0f, fadeOutCurve = 0.0f;
  Ticks crossfadeIn = 0;           // with the previous clip of the slot (adjacent: it ends at `start`)
  Ticks cutEnd = 0;                // > 0: the clip stops sounding here (the next clip's start); content laws keep `length`
};

struct AudioTrackDesc {
  uint32_t id = 0;                    // the track (informational)
  std::vector<AudioClipDesc> clips;   // by start
};

struct AudioBuildOptions {
  double sampleRate = 48000.0;  // the engine rate (the timeline's)
  bool edgeDeclick = true;      // linear fades of declickSeconds at clip edges without a longer fade or a crossfade
  double declickSeconds = 0.002;
};

class AudioSequenceImage {
public:
  static constexpr uint32_t kNoSample = 0xFFFFFFFFu;

  // One clip, everything the player needs in song samples (see the file comment).
  struct Clip {
    uint32_t id = 0;
    uint32_t sample = kNoSample;     // index into samples(); kNoSample: silent (never started)
    ClipWarp warp = ClipWarp::Off;
    bool reverse = false;
    bool looped = false;             // RePitch with an active loop
    SampleTime bodyStart = 0, bodyEnd = 0;
    SampleTime playStart = 0, playEnd = 0;
    // Envelope: fade in over [bodyStart, inEnd) (curve inCurve; 0 = linear), fade out over [outStart, bodyEnd),
    // crossfade windows [xinStart, xinEnd) (in) and [xoutStart, xoutEnd) (out; empty: none), equal power unless
    // xinLinear / xoutLinear (equal gain: the two clips continue one source law).
    bool xinLinear = false, xoutLinear = false;
    float gain = 1.0f;
    float inCurve = 0.0f, outCurve = 0.0f;
    SampleTime inEnd = 0, outStart = 0;
    SampleTime xinStart = 0, xinEnd = 0, xoutStart = 0, xoutEnd = 0;
    SampleTime flatStart = 0, flatEnd = 0;  // the static envelope is exactly `gain` on [flatStart, flatEnd)
    // Off: f(s) = anchor + (s − bodyStart) · step in 32.32 (reverse: step < 0, anchor = f(bodyStart)).
    int64_t anchor = 0;
    int64_t step = 0;
    // RePitch.
    Ticks start = 0, length = 1, contentStart = 0;
    ContentLoop loop;
    double framesPerTick = 0.0;
    double maxRate = 1.0;            // the largest |rate| the clip can read at (its stretch tables are prepared)
    double peakRate = 1.0;           // the same without the tables' margin (T5.x-perf-audio: which mips it needs)
  };

  // Not RT-safe. `slots[k]` = clip slot k's clips; `samples` are shared by every slot. Prepares every resampler table
  // the clips can need (pq/dsp/resample.h), reading the sample itself or, above rate 2, a mip level (rate / 2^L for
  // every level up to the one the rate wants: the player uses whatever levels the sample has). Errors: a clip with a
  // non-positive length or speed.
  static Result<std::unique_ptr<AudioSequenceImage>> build(std::span<const AudioTrackDesc> slots,
                                                            std::vector<ClipSample> samples, const TempoMap& tempo,
                                                            const AudioBuildOptions& options);

  uint32_t numSlots() const noexcept { return static_cast<uint32_t>(slotFirst_.size()) - 1; }  // RT-safe
  std::span<const Clip> clips(uint32_t slot) const noexcept {  // RT-safe; by playStart
    if (slot >= numSlots()) return {};
    return {clips_.data() + slotFirst_[slot], clips_.data() + slotFirst_[slot + 1]};
  }
  const ClipSample& sample(uint32_t i) const noexcept { return samples_[i]; }  // RT-safe
  uint32_t numSamples() const noexcept { return static_cast<uint32_t>(samples_.size()); }
  const TempoMap& tempo() const noexcept { return tempo_; }
  double sampleRate() const noexcept { return sr_; }
  uint32_t declickFrames() const noexcept { return declick_; }
  size_t totalClips() const noexcept { return clips_.size(); }

  // The source position (fractional frames) clip `c` reads at song sample s (the player's law, without the block
  // linearisation of RePitch); nullopt outside [playStart, playEnd). Not RT-safe (tests, prefetch).
  std::optional<double> sourceFrame(const Clip& c, SampleTime s) const;
  // RePitch: the clip offset o (fractional ticks from its start) at song sample s, on this image's tempo map. RT-safe.
  double repitchOffset(const Clip& c, SampleTime s) const noexcept;

private:
  AudioSequenceImage() = default;
  std::vector<Clip> clips_;
  std::vector<uint32_t> slotFirst_;   // numSlots + 1
  std::vector<ClipSample> samples_;
  TempoMap tempo_;
  double sr_ = 48000.0;
  uint32_t declick_ = 0;
};

}  // namespace pq::engine
