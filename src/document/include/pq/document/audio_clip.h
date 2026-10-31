#pragma once

// Where an audio clip's source plays (T5.3; model.h "Samples and audio clips"): the math shared by the commands (the
// derived length of a warp-Off clip), the edit operations (trim, split, move, warp switch: every source frame keeps the
// absolute time it plays at) and the lowering (audio_sequence.h). Not RT code: the engine gets plain data
// (audio_sequence.h) and does its own per-block arithmetic (T5.4).
//
// Time of an Off clip is seconds: tau(t) = the tempo map's ticksToSeconds(t). Its source position at song time tau is
//   forward   contentStartFrame + (tau - tau(start)) * rate * k
//   reverse   contentStartFrame + (seconds - (tau - tau(start))) * rate * k     (the window read backwards)
// with rate = the sample's rate and k = audioRateRatio (transpose x detune). A RePitch clip maps arrangement ticks to
// content ticks exactly as a note clip does (pq/core/clip_time.h: start marker, loop) and content tick c to source
// frame c * framesPerContentTick(rate, sourceBpm); reverse mirrors the offset (o -> length - o).

#include <utility>

#include "pq/core/clip_time.h"
#include "pq/core/time.h"
#include "pq/document/model.h"

namespace pq::doc {

inline ContentLoop contentLoop(const AudioClip& c) noexcept { return {c.loop.enabled, c.loop.start, c.loop.end}; }
inline Ticks clipEnd(const AudioClip& c) noexcept { return c.start + c.length; }

// The length an Off clip of `seconds` starting at `start` has under `tempo`: the nearest tick to
// tau(start) + seconds, minus start, at least 1.
Ticks offClipLength(const TempoMap& tempo, Ticks start, double seconds) noexcept;
// `c` with its derived fields brought in line with `timeline` (Off: length; RePitch: unchanged). Every command that
// stores an audio clip stores it synced, and a tempo map edit re-syncs every Off clip.
AudioClip syncedClip(const Timeline& timeline, AudioClip c);
// A track subtree with every warp-Off clip synced to `timeline` (the same pointer when nothing changes; clips re-sorted).
TrackPtr syncedTrack(const Timeline& timeline, const TrackPtr& t);

// Source frames per content tick of a RePitch clip (the sample's rate at sourceBpm).
double framesPerContentTick(double rate, double sourceBpm) noexcept;

// The tuning a clip's Steps transpose resolves in: the clip's own assignment, else its track's effective tuning.
const TuningDef* audioClipTuning(const Project& p, const TrackPath& path, const AudioClip& c) noexcept;
// The transpose x detune speed ratio of an Off clip (1 for RePitch). Steps: hz(degree n) / hz(degree 0) of the
// clip's tuning (1 when it cannot be resolved). The whole ratio is clamped to +-kMaxTransposeCents (+ the detune).
double audioRateRatio(const Project& p, const TrackPath& path, const AudioClip& c);
double transposeCents(const AudioTranspose& t, const tuning::Tuning* tuning);  // Steps without a tuning: 0

// The clip with new arrangement bounds [s, e) (e > s), every source position keeping its absolute time: the left edge
// moves the start marker (Off: contentStartFrame by the time it moved x rate x k; RePitch: contentStart as a note
// clip's, unrolling a loop backwards), a reversed clip mirrored. `rate` = the sample's rate, `ratio` =
// audioRateRatio. The fades are kept (the lowering clamps them), the crossfade is the commands' business.
AudioClip audioWithBounds(const Timeline& timeline, const AudioClip& c, Ticks s, Ticks e, double rate, double ratio);
// Splits at tick t (start < t < end): the left piece keeps the id, the right one gets `rightId`, both play exactly
// what the clip played there. The right piece takes the fade-out, the left the fade-in and the crossfade.
std::pair<AudioClip, AudioClip> splitAudioAt(const Timeline& timeline, const AudioClip& c, Ticks t, ClipId rightId,
                                             double rate, double ratio);
// The clip moved to `start` (Off: its length re-derived there).
AudioClip audioMovedTo(const Timeline& timeline, AudioClip c, Ticks start);
// The clip switched to `warp` with the audio at its start kept: Off -> RePitch takes `sourceBpm` (the start marker
// becomes the nearest content tick, the length in ticks stays, the loop off); RePitch -> Off keeps the start marker's
// frame and the clip's current duration in seconds.
AudioClip withWarp(const Timeline& timeline, const AudioClip& c, AudioWarp warp, double sourceBpm, double rate);

}  // namespace pq::doc
