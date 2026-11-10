#pragma once

// FakeTransport (T3.7): a TransportPort for tests and headless screenshots. It records every request and keeps
// the state a real transport would report, without any clock: the playhead moves only when a test sets it
// (setPlayheadTick) or a request moves it. Header-only, UI thread.

#include <cstdint>
#include <utility>
#include <vector>

#include "pq/views/transport_port.h"

namespace pq::views {

class FakeTransport final : public TransportPort {
public:
  enum class Call : uint8_t { Play, Stop, Continue, Locate, SetLoop, Audition, ReleaseAuditions, LiveKey, Record };
  struct Record {
    Call call;
    Ticks tick = 0;          // Play, Record: from; Locate: the target
    TransportLoop loop{};    // SetLoop
    float hz = 0.0f;         // Audition
    float velocity = 0.0f;   // Audition
    float durationMs = 0.0f; // Audition
    int32_t equave = 0, step = 0;  // LiveKey (T6.4); velocity above, `on` below
    bool on = false;
  };

  void play(Ticks from) override {
    calls.push_back({Call::Play, from, {}});
    state_.playing = true;
    state_.tick = from;
    ++state_.sequence;
  }
  void record(Ticks from) override {  // T6.6
    calls.push_back({Call::Record, from, {}});
    state_.playing = true;
    state_.tick = from;
    ++state_.sequence;
  }
  void stop() override {
    calls.push_back({Call::Stop, state_.tick, {}});
    state_.playing = false;
    state_.countingIn = false;
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
  void auditionHz(float hz, float velocity, float durationMs) override {
    Record r{Call::Audition, state_.tick, {}};
    r.hz = hz;
    r.velocity = velocity;
    r.durationMs = durationMs;
    calls.push_back(r);
  }
  void releaseAuditions() override { calls.push_back({Call::ReleaseAuditions, state_.tick, {}}); }
  void liveKey(int32_t equave, int32_t step, float velocity, bool on) override {
    Record r{Call::LiveKey, state_.tick, {}};
    r.equave = equave;
    r.step = step;
    r.velocity = velocity;
    r.on = on;
    calls.push_back(r);
  }

  double outputLatencyMs() override { return latencyMs; }  // T6.6
  double trackLatencyMs(uint32_t trackId) override {
    for (const auto& [id, ms] : trackLatency)
      if (id == trackId) return ms;
    return 0.0;
  }

  // Test control: the position a running transport would report next.
  // T6.6: a running count-in (beat of beats, beatsPerBar); beats 0 ends it.
  void setCountIn(uint32_t beat, uint32_t beats, uint32_t beatsPerBar) noexcept {
    state_.countingIn = beats != 0;
    state_.countInBeat = beat;
    state_.countInBeats = beats;
    state_.countInBeatsPerBar = beatsPerBar;
    ++state_.sequence;
  }
  void setPlayheadTick(Ticks t) noexcept {
    state_.tick = t;
    ++state_.sequence;
  }
  // The last request of kind c (nullptr: none).
  const Record* last(Call c) const noexcept {
    for (auto it = calls.rbegin(); it != calls.rend(); ++it)
      if (it->call == c) return &*it;
    return nullptr;
  }
  size_t count(Call c) const noexcept {
    size_t n = 0;
    for (const Record& r : calls) n += r.call == c ? 1 : 0;
    return n;
  }

  std::vector<Record> calls;
  TransportLoop loop{};
  double latencyMs = 0.0;                                  // T6.6: what outputLatencyMs reports
  std::vector<std::pair<uint32_t, double>> trackLatency;   // T6.6: trackLatencyMs per track id

private:
  PlayheadSnapshot state_{};
};

}  // namespace pq::views
