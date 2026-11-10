#pragma once

// The metronome (T6.6, docs/plans/M6.md T6.6): a synthesised click on the transport's beats, mixed by the engine after
// the project's output (pq/engine/engine.h "Per block").
//
// Beats: the beat grid of the time-signature map (beat = the signature's note value, ticksPerBeat; an accent on the
// first beat of every bar), at exactly TempoMap::ticksToSamples(beat tick) — the sample a note on that tick starts on,
// so a click and a note on the same beat coincide across tempo ramps and signature changes. schedule() reads the runs
// the timeline played in one image block (Timeline::runs: song runs while playing, the count-in run while counting
// in) and queues a click for every beat inside them: song beats while the metronome is on, count-in beats always
// (a count-in clicks with the metronome off).
// Latency: a click is queued at its beat's engine time plus `latency` (ProjectProgram::outputLatency, the master's
// PDC total), so it sounds together with what the project plays on that beat.
// Sound: a 15 ms decaying sine (2 kHz on the accent, 1 kHz otherwise), precomputed per sample rate at construction;
// rendering adds the table times the level. Clicks may overlap (fast tempos); the queue holds kMaxClicks.
// Cost: with the metronome off and no count-in, Engine tests one flag per image block and one per device block;
// a sounding click costs one multiply-add per frame and channel.
//
// Threads: setEnabled / setLevelDb / setCue from any thread (relaxed atomics); everything else on the audio thread.

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

#include "pq/engine/timeline.h"

namespace pq::engine {

struct MetronomeStats {
  uint64_t clicks = 0;          // clicks queued
  uint64_t accents = 0;
  uint64_t countInClicks = 0;
  uint64_t dropped = 0;         // the queue was full
};

class Metronome {
public:
  static constexpr uint32_t kMaxClicks = 64;
  static constexpr double kClickSeconds = 0.015;
  static constexpr double kAccentHz = 2000.0, kBeatHz = 1000.0;

  // Not RT-safe (builds the click tables).
  explicit Metronome(double sampleRate);

  // ---- any thread ----
  void setEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); }
  bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
  void setLevelDb(float db) noexcept;
  float level() const noexcept { return gain_.load(std::memory_order_relaxed); }
  // The click goes to the output's cue pair instead of the main pair (the app's audio output routes it; Engine).
  void setCue(bool cue) noexcept { cue_.store(cue, std::memory_order_relaxed); }
  bool cue() const noexcept { return cue_.load(std::memory_order_relaxed); }

  // ---- audio thread (RT-safe) ----
  // Queues the clicks of one image block starting at engine time `blockTime`. `songBeats`: the song runs' beats
  // (the metronome is on). RT-safe.
  void schedule(int64_t blockTime, std::span<const TransportRun> runs, const TempoImage& tempo,
                const CountInState& countIn, double sampleRate, uint32_t latency, bool songBeats) noexcept;
  // Adds the clicks sounding in engine frames [start, start + frames) into `out` (numOut channels). RT-safe.
  void render(int64_t start, uint32_t frames, float* const* out, uint32_t numOut) noexcept;
  // Clicks queued or sounding.
  bool pending() const noexcept { return head_ != tail_; }
  // Forgets every queued click and the beat cache (offline renders start clean). RT-safe.
  void clear() noexcept;
  const MetronomeStats& stats() const noexcept { return stats_; }
  // The next song beat the cache holds (tests).
  Ticks nextBeatTick() const noexcept { return nextTick_; }

private:
  struct Click {
    int64_t time;   // engine time of its first frame
    bool accent;
  };
  void push(int64_t time, bool accent) noexcept;
  void seek(Ticks t, const TempoImage& tempo, double sampleRate) noexcept;
  void advance(const TempoImage& tempo, double sampleRate) noexcept;

  std::atomic<bool> enabled_{false};
  std::atomic<bool> cue_{false};
  std::atomic<float> gain_{0.5f};
  std::vector<float> accentTable_, beatTable_;
  uint32_t length_ = 0;
  Click queue_[kMaxClicks];
  uint32_t head_ = 0, tail_ = 0;  // [head, tail) mod kMaxClicks: chronological
  // Song beat cache: the next beat at or after the expected run position.
  const TempoImage* tempo_ = nullptr;
  SampleTime expect_ = INT64_MIN;  // the song sample the next run should start at for the cache to hold
  Ticks nextTick_ = 0;
  SampleTime nextSample_ = 0;
  bool nextAccent_ = false;
  size_t seg_ = 0;          // its signature segment
  Ticks segEnd_ = 0;        // the next segment's start tick (INT64_MAX: none)
  Ticks beatTicks_ = 0;
  int64_t beatInBar_ = 0;   // beats since its bar started
  int64_t beatsPerBar_ = 4;
  MetronomeStats stats_;
};

}  // namespace pq::engine
