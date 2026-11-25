#pragma once

// A track meter's ballistics (ProjectLink::read): what the peak bar, the hold tick and the clip lamp show for the
// levels read from the engine. Everything is a function of TIME, not of how often the meter is read (OP.2: meters are
// polled on the readout grid, 30 or 60 Hz, and other readers poll single tracks in between):
// - the engine accumulates each bus's peak as the maximum since the previous read, so no peak between two reads is
//   lost at any poll rate (ProjectProgram::readMeters; ProjectLink::readLevels keeps it per reader);
// - the peak bar jumps up to a new peak and falls kFallDbPerSecond from where it was, over the time since THIS meter
//   was last stepped (each meter keeps its own clock: one shared by all readers let a second read in the same poll
//   see dt = 0 and not fall);
// - the hold tick keeps the highest peak for kHoldSeconds, then takes the current one;
// - the clip lamp latches at full scale until reset.
// Plain data; UI thread.

#include <algorithm>

namespace pq::app {

struct MeterBallistics {
  static constexpr float kFallDbPerSecond = 26.0f;  // PQ Meter-like release of the peak bar
  static constexpr double kHoldSeconds = 1.5;
  static constexpr double kMaxStepSeconds = 1.0;    // a meter not read for longer falls as if for this long
  static constexpr float kSilenceDb = -200.0f;

  float peakDb[2] = {kSilenceDb, kSilenceDb};
  float holdDb[2] = {kSilenceDb, kSilenceDb};
  double holdUntil[2] = {0.0, 0.0};
  bool clip[2] = {false, false};
  double last = -1.0;  // when it was last stepped (negative: never)

  // The time since the last step (0 at the first), noting `now`. Call once per read, before the channels.
  double advance(double now) noexcept {
    const double dt = last < 0.0 ? 0.0 : std::clamp(now - last, 0.0, kMaxStepSeconds);
    last = now;
    return dt;
  }
  // One channel's reading: `pkDb` is the peak since the previous read in dB (kSilenceDb: none), `clipped` whether it
  // reached full scale.
  void step(int c, float pkDb, bool clipped, double now, double dt) noexcept {
    peakDb[c] = std::max(pkDb, peakDb[c] - kFallDbPerSecond * static_cast<float>(dt));
    if (pkDb >= holdDb[c] || now >= holdUntil[c]) {
      holdDb[c] = pkDb;
      holdUntil[c] = now + kHoldSeconds;
    }
    if (clipped) clip[c] = true;  // latched until resetClip (a click), as in Live
  }
  // The peak bar's level as shown (everything at or under the silence floor reads as silence).
  float shownPeakDb(int c) const noexcept { return peakDb[c] < kSilenceDb + 1.0f ? kSilenceDb : peakDb[c]; }
};

}  // namespace pq::app
