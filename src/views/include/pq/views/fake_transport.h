#pragma once

// FakeTransport (T3.7): a TransportPort for tests and headless screenshots. It records every request and keeps
// the state a real transport would report, without any clock: the playhead moves only when a test sets it
// (setPlayheadTick) or a request moves it. Header-only, UI thread.

#include <cstdint>
#include <vector>

#include "pq/views/transport_port.h"

namespace pq::views {

class FakeTransport final : public TransportPort {
public:
  enum class Call : uint8_t { Play, Stop, Continue, Locate, SetLoop };
  struct Record {
    Call call;
    Ticks tick = 0;          // Play: from; Locate: the target
    TransportLoop loop{};    // SetLoop
  };

  void play(Ticks from) override {
    calls.push_back({Call::Play, from, {}});
    state_.playing = true;
    state_.tick = from;
    ++state_.sequence;
  }
  void stop() override {
    calls.push_back({Call::Stop, state_.tick, {}});
    state_.playing = false;
    ++state_.sequence;
  }
  void continuePlayback() override {
    calls.push_back({Call::Continue, state_.tick, {}});
    state_.playing = true;
    ++state_.sequence;
  }
  void locate(Ticks tick) override {
    calls.push_back({Call::Locate, tick, {}});
    state_.tick = tick;
    ++state_.sequence;
  }
  void setLoop(const TransportLoop& l) override {
    calls.push_back({Call::SetLoop, 0, l});
    loop = l;
  }
  PlayheadSnapshot playhead() override { return state_; }

  // Test control: the position a running transport would report next.
  void setPlayheadTick(Ticks t) noexcept {
    state_.tick = t;
    ++state_.sequence;
  }
  size_t count(Call c) const noexcept {
    size_t n = 0;
    for (const Record& r : calls) n += r.call == c ? 1 : 0;
    return n;
  }

  std::vector<Record> calls;
  TransportLoop loop{};

private:
  PlayheadSnapshot state_{};
};

}  // namespace pq::views
