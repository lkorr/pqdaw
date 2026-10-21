#pragma once

// SimulatedTransport (T3.7): the app-side TransportPort until T3.17 links the views to the engine's transport. It
// keeps the transport state on the UI thread and derives the playhead from a clock and the project's tempo map, so
// Space, the position display and the loop behave as they will with the engine (minus sound). Tests drive it with
// a fake clock.
//
// The loop wraps in ticks once the playhead reaches the loop end (if playback started before it), as the engine's
// transport does (T3.16).

#include <functional>

#include "pq/core/time.h"
#include "pq/views/transport_port.h"

namespace pq::app {

class SimulatedTransport final : public views::TransportPort {
public:
  // `now`: monotonic seconds.
  explicit SimulatedTransport(std::function<double()> now);

  // The tempo map positions are computed with. Re-anchors a running playhead at its current tick.
  void setTempoMap(const TempoMap& map);

  void play(Ticks from) override;
  void stop() override;
  void continuePlayback() override;
  void locate(Ticks tick) override;
  void setLoop(const views::TransportLoop& loop) override;
  views::PlayheadSnapshot playhead() override;
  // Nothing sounds before T3.17 puts the port over the engine: auditions are ignored.
  void auditionHz(float, float, float) override {}

private:
  Ticks currentTick() const;
  void anchor(Ticks tick);

  std::function<double()> now_;
  TempoMap tempo_;
  views::TransportLoop loop_{};
  bool playing_ = false;
  Ticks position_ = 0;       // stopped: where continue resumes; playing: the anchor tick
  double anchorSeconds_ = 0;  // playing: clock time at the anchor
  uint64_t sequence_ = 0;
};

}  // namespace pq::app
