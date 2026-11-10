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
//   RePitch  f(s) = W(clipContentAt(contentStart, loop, o)) with o = samplesToTicks(s) − start (reverse: length − o)
//            and W the warp law content tick → source frame (T6.3: piecewise linear through the clip's WarpSegments,
//            one implied segment through (0, 0) at framesPerTick without markers), evaluated exactly at the ends of a
//            fixed 64-sample grid and linear in between (the rate is constant within a cell: under a tempo ramp the
//            speed is re-anchored each cell, so nothing drifts), cells split where the loop wraps and where the
//            content crosses a segment boundary (both found from the cell's law, never tested per sample).
//   Beats, Tones, Texture (T6.3, time-stretch): the same law f(s) says WHICH source plays at s, but the source is read
//            at its own pitch (× pitchRatio: rate r = pitchRatio · fileRate / sr), so its duration follows the tempo
//            and its pitch does not:
//     Beats  the content is cut into slices at boundaries B_j (the Beats grid `granulation`, or the sample's
//            transients when it is 0; plus every warp marker and the loop's ends); a slice pass starts at the song
//            sample where the content reaches B_j (its warped time) and reads from W(B_j) at rate r. Slices crossfade
//            over 2 ms (equal gain: the next slice fades in from its entry while the previous one plays on and fades
//            out); a slice the next one cuts before (or within the crossfade of) its natural end (W(B_{j+1}) − W(B_j)
//            frames) reads on naturally through its fade-out (seamless at the source's tempo); one that reaches it
//            first stops with a fade (SliceLoop Off), loops its second half forward (declicked) or back and forth;
//            `envelope` < 100 holds a slice for that % of its gap and fades it to 0 at the next slice. Reverse plays
//            each slice backwards from its end.
//     Tones / Texture  granular overlap-add: grains of N = grainSeconds · sr output frames (even), one every H = N / 2
//            song samples, periodic Hann windows (they sum to 1); grain k covers [bodyStart + (k − 1)·H,
//            bodyStart + (k + 1)·H), is centred at C_k = bodyStart + k·H and reads around P_k = f(C_k) at rate r.
//            Tones moves P_k by the lag (within ±H·r / 2, ≤ 12 ms) whose source best continues grain k − 1
//            (pq/dsp/warp.h warpBestLag, WSOLA), so periodic sources keep their pitch exactly (the lags chain from the
//            voice's first grain); Texture jitters P_k by ±flux % · H · r (warpJitter, seeded per clip), unaligned.
//     Every slice / grain position is anchored at a song sample that depends on the clip alone (its entry, its grain
//     start), so renders are bit-identical whatever the engine block.
//     Complex / Complex Pro (T6.5): a phase vocoder (signalsmith-stretch, pq/engine/stretch_pool.h) only stretches
//            time: it reads the source continuously along the law, unfolded through loop wraps (φ(s) = f(s) + folds ·
//            loop span in frames), and its output frame y is the analysis centred on φ(t(y)), t(y) = tA + y / r. The
//            pitch is a resampling of that output at rate r (the windowed sinc; a copy at r = 1): song sample s reads
//            vocoder frame (s − tA) · r, so the pitch is exact (the library's own transpose estimates each peak's
//            frequency and was 2 ¢ off at +3 steps of 31-EDO, 8 ¢ at +7 st). The vocoder runs in chunks of
//            kStretchCell of its frames on a grid fixed by the voice's start (each chunk's input up to
//            φ(t(chunk end + outputLatency)) + inputLatency), so the rate follows the warp segments and the tempo per
//            chunk and a render is the same whatever the block. Complex Pro adds formant preservation: the vocoder
//            shifts the formants by pitchRatio^(−formants %), which the resampling brings back, over a spectral
//            envelope of order `formantEnvelope`. No free stretcher: the voice plays as Tones (its grain fields are
//            prepared).
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
#include <set>
#include <span>
#include <utility>
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

// T6.3: Beats / Tones / Texture are the time-stretch modes; T6.5 adds Complex / Complex Pro (the phase vocoder).
enum class ClipWarp : uint8_t { Off, RePitch, Beats, Tones, Texture, Complex, ComplexPro };
inline constexpr bool clipComplex(ClipWarp w) noexcept { return w == ClipWarp::Complex || w == ClipWarp::ComplexPro; }
inline constexpr bool clipStretches(ClipWarp w) noexcept {
  return w == ClipWarp::Beats || w == ClipWarp::Tones || w == ClipWarp::Texture || clipComplex(w);
}

// T6.3: one piece of a warped clip's law content tick → source frame: from content tick `content0` on (the first
// segment also before it), frame = frame0 + (content − content0) · framesPerTick (> 0).
struct WarpSegment {
  Ticks content0 = 0;
  double frame0 = 0.0;
  double framesPerTick = 0.0;
  friend constexpr bool operator==(const WarpSegment&, const WarpSegment&) noexcept = default;
};

// T6.3: what a Beats slice does past its natural end (doc::BeatsLoop).
enum class SliceLoop : uint8_t { Off, Forward, BackForward };

// T6.3: the time-stretch modes' settings (doc::WarpParams resolved).
struct WarpSettings {
  double pitchRatio = 1.0;       // the transpose × detune (a pitch shift; the duration follows the tempo)
  Ticks granulation = kPpq / 4;  // Beats: the slice grid in content ticks (0: the transients)
  SliceLoop sliceLoop = SliceLoop::Forward;
  float envelope = 100.0f;       // Beats: % of each slice's gap at full gain before it fades to the next slice
  double grainSeconds = 0.03;    // Tones / Texture
  float flux = 0.0f;             // Texture: grain position jitter, % of the hop
  uint32_t seed = 0;             // Texture: the jitter's seed; Complex: the stretcher's (the app uses the clip id)
  float formants = 100.0f;       // T6.5 Complex Pro: formant preservation, % [0, 100]
  float formantEnvelope = 128.0f;  // T6.5 Complex Pro: the spectral envelope's order [8, 256] (formant base sr / order)
};

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
  // Warped (RePitch and the time-stretch modes).
  Ticks contentStart = 0;
  ContentLoop loop;
  double framesPerTick = 0.0;      // source frames per content tick (the implied segment when `segments` is empty)
  std::vector<WarpSegment> segments;  // T6.3: the warp markers' law (by content0; empty: one implied segment)
  WarpSettings stretch;            // T6.3: Beats / Tones / Texture
  std::span<const double> transients;  // T6.3: Beats with granulation 0: the sample's onsets (source frames, ascending;
                                       // the caller keeps them alive until build returns; empty: the 1/16 grid)
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
    // Warped (RePitch and the time-stretch modes).
    Ticks start = 0, length = 1, contentStart = 0;
    ContentLoop loop;
    double framesPerTick = 0.0;      // the first segment's (informational)
    uint32_t segFirst = 0, segCount = 0;  // segments() (≥ 1 for a warped clip)
    double maxRate = 1.0;            // the largest |rate| the clip can read at (its stretch tables are prepared)
    double peakRate = 1.0;           // the same without the tables' margin (T5.x-perf-audio: which mips it needs)
    // T6.3, the time-stretch modes.
    int64_t pitchStep = 0;           // 32.32 source frames per output frame (r > 0; the direction is `reverse`)
    double pitchRate = 1.0;          // r
    uint32_t grain = 0, hop = 0;     // Tones / Texture: N, H = N / 2 (output frames)
    uint32_t winFirst = 0;           // window(): the Hann window of `grain` frames
    uint32_t alignLags = 0;          // Tones: ±alignLags source frames searched per grain (0: no alignment)
    uint32_t alignLen = 0;           // Tones: source frames compared
    double jitter = 0.0;             // Texture: grain position jitter in source frames at most (flux % · H · r)
    uint32_t seed = 0;
    uint32_t sliceFirst = 0, sliceCount = 0;  // Beats: sliceContent() / sliceFrame()
    SliceLoop sliceLoop = SliceLoop::Off;
    float envelope = 100.0f;
    // T6.5 Complex / Complex Pro (the stretcher's settings, pq/engine/stretch_pool.h StretchParams; the grain fields
    // above are Tones', the fallback when no stretcher is free).
    double formantFactor = 1.0;    // the vocoder's formant shift: pitchRatio^(−formants) (the resampling adds pitchRatio)
    double formantBase = 0.0;      // cycles per sample (1 / formantEnvelope); 0 without formant processing
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
  // T6.3 (RT-safe).
  std::span<const WarpSegment> segments(const Clip& c) const noexcept { return {segments_.data() + c.segFirst, c.segCount}; }
  const float* window(const Clip& c) const noexcept { return windows_.data() + c.winFirst; }
  std::span<const double> sliceContent(const Clip& c) const noexcept {
    return {sliceContent_.data() + c.sliceFirst, c.sliceCount};
  }
  std::span<const double> sliceFrame(const Clip& c) const noexcept { return {sliceFrame_.data() + c.sliceFirst, c.sliceCount}; }
  uint32_t sliceFadeFrames() const noexcept { return sliceFade_; }  // Beats' slice crossfade (2 ms)
  // The warp law W of a warped clip: the source frame of (folded) content tick q. RT-safe.
  double frameAtContent(const Clip& c, double q) const noexcept;
  // Its inverse: the content tick of source frame f. RT-safe.
  double contentAtFrame(const Clip& c, double f) const noexcept;
  // A warped clip's forward-time content position at song sample s, unfolded: contentStart + o (reverse:
  // contentStart + length − o), o = samplesToTicks(s) − start. RT-safe.
  double contentAt(const Clip& c, SampleTime s) const noexcept;
  // W(fold(contentAt(s))): the source frame the law puts at s (any s; the clip's range is not checked). RT-safe.
  double warpFrame(const Clip& c, SampleTime s) const noexcept;
  // The first song sample at or after clip offset o (fractional ticks from its start). RT-safe.
  SampleTime sampleAtOffset(const Clip& c, double o) const noexcept;

  // The source position (fractional frames) clip `c` reads at song sample s (the player's law, without the block
  // linearisation of RePitch; for the time-stretch modes the law f(s), not the slice / grain read positions);
  // nullopt outside [playStart, playEnd). Not RT-safe (tests, prefetch).
  std::optional<double> sourceFrame(const Clip& c, SampleTime s) const;
  // Warped: the clip offset o (fractional ticks from its start) at song sample s, on this image's tempo map. RT-safe.
  double repitchOffset(const Clip& c, SampleTime s) const noexcept;

  static constexpr uint32_t kMaxAlignLen = 2048;  // Tones: source frames compared per grain at most
  static constexpr uint32_t kMaxAlignLags = 960;  // Tones: ± source frames searched at most
  static constexpr size_t kMaxSlices = size_t{1} << 20;  // Beats: slice boundaries per clip at most

private:
  AudioSequenceImage() = default;
  static void stretchClip(AudioSequenceImage& img, Clip& c, const AudioClipDesc& d, double sr, std::set<uint32_t>& qs);
  std::vector<Clip> clips_;
  std::vector<uint32_t> slotFirst_;   // numSlots + 1
  std::vector<ClipSample> samples_;
  std::vector<WarpSegment> segments_;
  std::vector<float> windows_;
  std::vector<double> sliceContent_, sliceFrame_;
  std::vector<std::pair<uint32_t, uint32_t>> windowAt_;  // grain size → windows_ offset (build only)
  uint32_t sliceFade_ = 96;
  TempoMap tempo_;
  double sr_ = 48000.0;
  uint32_t declick_ = 0;
};

}  // namespace pq::engine
