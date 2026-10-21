#include "pq/app/sim_transport.h"

#include <cmath>
#include <utility>

namespace pq::app {

SimulatedTransport::SimulatedTransport(std::function<double()> now) : now_(std::move(now)) {}

Ticks SimulatedTransport::currentTick() const {
  if (!playing_) return position_;
  const double elapsed = now_() - anchorSeconds_;
  const double t0 = tempo_.ticksToSeconds(position_);
  // +1e-6: the tempo map round trip may land a hair below an exact tick.
  Ticks t = static_cast<Ticks>(std::floor(tempo_.secondsToTicks(t0 + elapsed) + 1e-6));
  if (loop_.enabled && loop_.end > loop_.start && position_ < loop_.end && t >= loop_.end) {
    const Ticks len = loop_.end - loop_.start;
    t = loop_.start + (t - loop_.start) % len;
  }
  return t;
}

void SimulatedTransport::anchor(Ticks tick) {
  position_ = tick;
  anchorSeconds_ = now_();
}

void SimulatedTransport::setTempoMap(const TempoMap& map) {
  const Ticks t = currentTick();
  tempo_ = map;
  if (playing_) anchor(t);
}

void SimulatedTransport::play(Ticks from) {
  playing_ = true;
  anchor(from);
  ++sequence_;
}

void SimulatedTransport::stop() {
  if (!playing_) return;
  position_ = currentTick();
  playing_ = false;
  ++sequence_;
}

void SimulatedTransport::continuePlayback() {
  if (playing_) return;
  play(position_);
}

void SimulatedTransport::locate(Ticks tick) {
  if (playing_)
    anchor(tick);
  else
    position_ = tick;
  ++sequence_;
}

void SimulatedTransport::setLoop(const views::TransportLoop& loop) {
  // Keep a running playhead where it is: re-anchor before the wrap rule changes.
  if (playing_) anchor(currentTick());
  loop_ = loop;
}

views::PlayheadSnapshot SimulatedTransport::playhead() {
  views::PlayheadSnapshot s;
  s.playing = playing_;
  s.tick = currentTick();
  s.sequence = playing_ ? ++sequence_ : sequence_;  // a running clock reports a new position every time
  return s;
}

}  // namespace pq::app
