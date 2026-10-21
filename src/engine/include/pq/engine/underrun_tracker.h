#pragma once

#include <cstdint>

namespace pq::engine {

// Output-underrun accounting for a pull-model device (the WASAPI backend), fed once per device wake-up on the
// audio thread, before that wake's write. Pure state machine (no clock reads, no atomics) so tests can drive it
// with a fake clock. RT-safe.
//
// The schedule signal is X = wall clock (in frames) − frames written. While the device plays everything it is
// given, X stays within the device's event jitter of its minimum (plus a slow drift between the device crystal
// and the wall clock). When the device runs dry it keeps time but not data, so X steps up by the time it
// starved and the stream continues on the shifted schedule.
//
// Signals (any one flags the wake; consecutive flagged wakes are one underrun episode):
// - Late (exclusive mode): X − baseline > slackFrames, i.e. the wake came later than the data the device held
//   could cover (one period, the half of the double buffer still playing), and, when the device position is
//   known, the device had taken every frame written. In shared mode lateness is only reported. The
//   baseline is the minimum X over the last two windows of kWindowWakes wakes: drift is followed, a late wake
//   does not move it, and event-phase wander (Focusrite USB: events on a 2 ms grid) stays inside the slack.
//   After an underrun the baseline re-anchors to that wake.
// - Padding (shared mode): the endpoint buffer was empty at the wake although the last fill level was not a
//   whole number of engine periods, i.e. the engine's last pass found less than a period and inserted silence.
//   (When the fill level is a whole number of periods, an exact drain is not a glitch: then the wake must also
//   be later than the whole buffer.)
// - Device clock: the IAudioClock position passed the frames written.
// - A timed-out event wait.
// The first kWarmupWakes wakes after start only seed the baseline (devices fill their pipeline at start, so the
// first wake-ups come irregularly); only timeouts count there.
class UnderrunTracker {
public:
  static constexpr uint32_t kWindowWakes = 512;
  static constexpr uint32_t kWarmupWakes = 16;

  struct Config {
    uint32_t periodFrames = 0;  // frames the device consumes per event (shared: engine period)
    uint32_t slackFrames = 0;   // lateness beyond this is an underrun
    bool paddingValid = false;  // shared mode: padding at the wake is meaningful
  };

  struct Wake {
    int64_t nowFrames = 0;  // wall clock at the wake, in frames at the stream rate (any fixed origin)
    int64_t written = 0;    // frames written to the device before this wake
    int64_t position = -1;  // device clock position in frames, or -1 if unknown
    int32_t padding = -1;   // frames still queued in the endpoint buffer, or -1 if not meaningful
    bool timedOut = false;  // the event wait timed out
  };

  struct Outcome {
    bool underrun = false;       // this wake is (part of) an underrun episode
    bool newEpisode = false;     // first flagged wake of an episode: count it
    int64_t latenessFrames = 0;  // ≥ 0: how far behind the baseline schedule this wake is
    int64_t queueFrames = -1;    // written − device position (-1: position unknown, or still in warm-up)
  };

  void reset(const Config& c) noexcept {  // RT-safe
    cfg_ = c;
    wakes_ = 0;
    windowCount_ = 0;
    curMin_ = prevMin_ = 0;
    lastFill_ = -1;
    inEpisode_ = false;
    episodes_ = 0;
  }

  Outcome onWake(const Wake& w) noexcept {  // RT-safe
    Outcome o;
    const int64_t x = w.nowFrames - w.written;
    if (w.position >= 0 && wakes_ >= kWarmupWakes) o.queueFrames = w.written - w.position;
    bool flagged = w.timedOut;

    if (wakes_ < kWarmupWakes) {
      // Seed the baseline from the last warm-up wake (the earlier ones are the device's pipeline fill).
      curMin_ = prevMin_ = x;
    } else {
      const int64_t base = curMin_ < prevMin_ ? curMin_ : prevMin_;
      o.latenessFrames = x > base ? x - base : 0;
      if (x < curMin_) curMin_ = x;
      if (++windowCount_ >= kWindowWakes) {
        prevMin_ = curMin_;
        curMin_ = x;
        windowCount_ = 0;
      }
      // Exclusive mode only (shared mode has the exact padding signal, and the Windows audio engine's pass
      // schedule wanders by several periods against the wall clock without starving anyone: measured). With a
      // known position, a late wake starved the device only if it had taken everything written; with data
      // still queued, the reserve covered the delay.
      const bool caughtUp = w.position < 0 || w.position >= w.written;
      const bool late =
          !cfg_.paddingValid && o.latenessFrames > static_cast<int64_t>(cfg_.slackFrames) && caughtUp;
      // An empty endpoint buffer is a sure starvation when the last fill was not a whole number of engine
      // periods; when it was, the engine may have drained it exactly, so only a wake also later than the whole
      // buffer counts.
      const bool emptied = cfg_.paddingValid && w.padding == 0 && lastFill_ > 0 && cfg_.periodFrames != 0;
      const bool drained =
          emptied && (lastFill_ % cfg_.periodFrames != 0 || o.latenessFrames > static_cast<int64_t>(cfg_.slackFrames));
      const bool overrun = w.position >= 0 && w.position > w.written;
      if (late || drained || overrun) {
        flagged = true;
        curMin_ = prevMin_ = x;  // the stream continues on the shifted schedule
      }
    }

    o.underrun = flagged;
    o.newEpisode = flagged && !inEpisode_;
    if (o.newEpisode) ++episodes_;
    inEpisode_ = flagged;
    lastFill_ = w.padding;
    ++wakes_;
    return o;
  }

  // After the wake's write: `frames` written in total this wake. Tracks the fill level for the padding check.
  void onWritten(uint32_t frames) noexcept {  // RT-safe
    if (lastFill_ >= 0) lastFill_ += frames;
  }

  uint64_t episodes() const noexcept { return episodes_; }
  uint64_t wakes() const noexcept { return wakes_; }

private:
  Config cfg_{};
  uint64_t wakes_ = 0;
  uint32_t windowCount_ = 0;
  int64_t curMin_ = 0;
  int64_t prevMin_ = 0;
  int64_t lastFill_ = -1;  // endpoint fill level after the last write (shared), -1 = unknown
  bool inEpisode_ = false;
  uint64_t episodes_ = 0;
};

}  // namespace pq::engine
