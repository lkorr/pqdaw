#pragma once

// The audio sequence of an audio track (T5.3; plan T5.3 "Lowering"): what the clip player (T5.4) needs to play the
// track's clips, as plain data with every document rule already applied (warp law, transpose and detune resolved in the
// clip's tuning, gain in linear, fades clamped to the clip, crossfade windows, reverse, missing samples). The app turns
// it into the engine's audio sequence image of the track's clip slot (LowerOptions::clipSlotOf), as it turns
// resolveTrackPitches into note sequences; `document` never includes `engine`.
//
// Source position laws (audio_clip.h), per clip, at song time tau (seconds; tau(t) = the tempo map's ticksToSeconds):
//   Off      forward  f = contentStartFrame + (tau - startSeconds) * speed
//            reverse  f = contentStartFrame + (seconds - (tau - startSeconds)) * speed
//            speed = sample rate * rateRatio (source frames per second); exact in seconds, so a tempo edit never moves
//            what plays at a given time of the clip.
//   RePitch  o = (ticks at tau) - start (reverse: length - o); content tick c = clipContentAt(contentStart, loop, o)
//            (pq/core/clip_time.h, loop folded), f = c * framesPerTick. Speed follows the tempo: tempo / sourceBpm.
// Gain at offset o (ticks into the clip): gain * fadeIn(o) * fadeOut(length - o) * crossfade (equal-power, over the
// window centred on a boundary between two adjacent clips: [start - crossfadeIn / 2, start + crossfadeIn - crossfadeIn / 2)
// of the right clip; the left clip plays on past its end through the window, the right one starts early). Edge declick
// fades (2 ms, plan "Decisions") are the player's, not stored.
//
// Not RT code (built on the UI thread, like a note sequence).

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "pq/core/clip_time.h"
#include "pq/core/time.h"
#include "pq/document/model.h"

namespace pq::doc {

struct AudioClipDesc {
  ClipId id;
  SampleId sample;
  Ticks start = 0;
  Ticks length = 1;
  AudioWarp warp = AudioWarp::Off;
  bool reverse = false;
  bool playable = true;  // active, and its sample available (AudioLowerOptions::available); else silent
  // Off.
  double startSeconds = 0.0;       // tau(start)
  double seconds = 0.0;            // the clip's duration
  double contentStartFrame = 0.0;  // the source frame at the clip start (forward)
  double speed = 0.0;              // source frames per second (rate * rateRatio)
  double rateRatio = 1.0;          // transpose x detune (Off; 1 for RePitch)
  // RePitch.
  Ticks contentStart = 0;
  ContentLoop loop;
  double framesPerTick = 0.0;      // source frames per content tick (framesPerContentTick)
  // Envelope.
  float gain = 1.0f;               // linear; 0 at kMinClipGainDb
  Ticks fadeIn = 0, fadeOut = 0;   // clamped: fadeIn <= length, fadeOut <= length - fadeIn
  float fadeInCurve = 0.0f, fadeOutCurve = 0.0f;
  Ticks crossfadeIn = 0;           // with the previous clip (adjacent); clamped to both clips' lengths
  // M5 review H7: where the clip stops sounding (ticks; 0 = its end). Set when the next clip on the track starts before
  // this one ends (a tempo increase lengthens warp-Off clips, which keep their seconds); the content laws still use
  // the whole clip (so `seconds` and the audio at every instant stay), the clip just ends at the next one's start.
  Ticks cutEnd = 0;
  friend bool operator==(const AudioClipDesc&, const AudioClipDesc&) = default;
};

struct AudioTrackDesc {
  TrackId id;
  std::vector<AudioClipDesc> clips;  // in the track's order (start, id)
  friend bool operator==(const AudioTrackDesc&, const AudioTrackDesc&) = default;
};

struct AudioLowerOptions {
  // Whether a sample of the table can be played on this machine (samples.h: the app's resolution). Unset: all can.
  std::function<bool(SampleId)> available;
};

// The audio sequence of the audio track at `path` (an empty one for any other track).
AudioTrackDesc lowerAudioTrack(const Project& p, const TrackPath& path, const AudioLowerOptions& options = {});

// The source position (fractional frames) clip `c` plays at song time `tau` seconds, by the laws above; nullopt
// outside the clip (tau before its start or at / after its end). For tests and the T5.4 player's reference.
std::optional<double> sourceFrameAt(const AudioClipDesc& c, const TempoMap& tempo, double tau);

}  // namespace pq::doc
