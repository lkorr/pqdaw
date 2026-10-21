#include "pq/core/time.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace pq {
namespace {

// Seconds from the start of a segment to τ ticks into it. c = seconds per tick at the segment start,
// k = relative BPM slope per tick (0 = constant). See TempoCurve::Linear for the derivation.
double segmentSeconds(double c, double k, double tau) noexcept {  // RT-safe
  const double z = k * tau;
  if (z == 0.0) return tau * c;
  return tau * c * (std::log1p(z) / z);
}

// Inverse of segmentSeconds: ticks into the segment after sigma seconds.
double segmentTicks(double c, double k, double sigma) noexcept {  // RT-safe
  const double y = sigma * k / c;
  if (y == 0.0) return sigma / c;
  return (sigma / c) * (std::expm1(y) / y);
}

int64_t floorDiv(int64_t a, int64_t b) noexcept {  // RT-safe; b > 0
  const int64_t q = a / b;
  return (a % b != 0 && a < 0) ? q - 1 : q;
}

// Index of the last element ≤ v in a sorted array, or 0 if v precedes them all.
template <class T>
std::size_t lastAtOrBefore(const std::vector<T>& sorted, T v) noexcept {  // RT-safe
  const auto it = std::upper_bound(sorted.begin(), sorted.end(), v);
  return it == sorted.begin() ? 0 : static_cast<std::size_t>(it - sorted.begin()) - 1;
}

Error invalid(std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message), {}}; }

}  // namespace

// ---- TempoMap ---------------------------------------------------------------------------------------------------

TempoMap::TempoMap()
    : startTick_{0}, startSec_{0.0}, secPerTick_{60.0 / (static_cast<double>(kPpq) * 120.0)}, rampK_{0.0},
      bpm0_{120.0} {}

Result<TempoMap> TempoMap::create(std::span<const TempoPoint> points) {
  if (points.empty()) return invalid("tempo map needs at least one point");
  if (points[0].tick != 0) return invalid("first tempo point must be at tick 0");
  for (std::size_t i = 0; i < points.size(); ++i) {
    const double bpm = points[i].bpm;
    if (!(bpm >= kMinBpm && bpm <= kMaxBpm))
      return invalid("tempo point " + std::to_string(i) + ": bpm out of range [1, 1000]");
    if (i > 0 && points[i].tick <= points[i - 1].tick)
      return invalid("tempo point " + std::to_string(i) + ": ticks must be strictly increasing");
  }

  TempoMap m;
  m.startTick_.clear();
  m.startSec_.clear();
  m.secPerTick_.clear();
  m.rampK_.clear();
  m.bpm0_.clear();
  const std::size_t n = points.size();
  m.startTick_.reserve(n);
  m.startSec_.reserve(n);
  m.secPerTick_.reserve(n);
  m.rampK_.reserve(n);
  m.bpm0_.reserve(n);

  // Neumaier-compensated running sum of segment durations: segment start times carry ≤ 1 rounding each,
  // independent of how many segments precede them.
  double sum = 0.0;
  double comp = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double b0 = points[i].bpm;
    const double c = 60.0 / (static_cast<double>(kPpq) * b0);
    double k = 0.0;
    if (i + 1 < n && points[i].curve == TempoCurve::Linear) {
      const double len = static_cast<double>(points[i + 1].tick - points[i].tick);
      k = (points[i + 1].bpm - b0) / (len * b0);
    }
    m.startTick_.push_back(points[i].tick);
    m.startSec_.push_back(sum + comp);
    m.secPerTick_.push_back(c);
    m.rampK_.push_back(k);
    m.bpm0_.push_back(b0);
    if (i + 1 < n) {
      const double d = segmentSeconds(c, k, static_cast<double>(points[i + 1].tick - points[i].tick));
      const double t = sum + d;
      comp += std::abs(sum) >= std::abs(d) ? (sum - t) + d : (d - t) + sum;
      sum = t;
    }
  }
  return m;
}

std::size_t TempoMap::segmentForTick(Ticks t) const noexcept { return lastAtOrBefore(startTick_, t); }
std::size_t TempoMap::segmentForSeconds(double s) const noexcept { return lastAtOrBefore(startSec_, s); }

double TempoMap::localSeconds(std::size_t i, double tau) const noexcept {
  return segmentSeconds(secPerTick_[i], rampK_[i], tau);
}

double TempoMap::bpmAt(Ticks t) const noexcept {
  if (t < 0) return bpm0_[0];
  const std::size_t i = segmentForTick(t);
  return bpm0_[i] * (1.0 + rampK_[i] * static_cast<double>(t - startTick_[i]));
}

double TempoMap::ticksToSeconds(Ticks t) const noexcept {
  if (t < 0) return static_cast<double>(t) * secPerTick_[0];
  const std::size_t i = segmentForTick(t);
  return startSec_[i] + localSeconds(i, static_cast<double>(t - startTick_[i]));
}

double TempoMap::secondsToTicks(double seconds) const noexcept {
  if (seconds < 0.0) return seconds / secPerTick_[0];
  const std::size_t i = segmentForSeconds(seconds);
  return static_cast<double>(startTick_[i]) + segmentTicks(secPerTick_[i], rampK_[i], seconds - startSec_[i]);
}

double TempoMap::ticksToSamplesF(Ticks t, double sampleRate) const noexcept {
  return ticksToSeconds(t) * sampleRate;
}

SampleTime TempoMap::ticksToSamples(Ticks t, double sampleRate) const noexcept {
  return static_cast<SampleTime>(std::floor(ticksToSamplesF(t, sampleRate) + 0.5));
}

double TempoMap::samplesToTicks(SampleTime s, double sampleRate) const noexcept {
  return secondsToTicks(static_cast<double>(s) / sampleRate);
}

Ticks TempoMap::firstTickAtOrAfterSample(SampleTime s, double sampleRate) const noexcept {
  // ticksToSamples(t) ≥ s  ⟺  exact samples(t) + 0.5 ≥ s. Start from the analytic inverse, then fix the ≤ 1-tick
  // numeric error with forward evaluations (bounded: the analytic estimate is good to ~1e-6 ticks).
  Ticks t = static_cast<Ticks>(std::ceil(secondsToTicks((static_cast<double>(s) - 0.5) / sampleRate)));
  for (int i = 0; i < 4 && ticksToSamples(t - 1, sampleRate) >= s; ++i) --t;
  for (int i = 0; i < 4 && ticksToSamples(t, sampleRate) < s; ++i) ++t;
  return t;
}

// ---- TimeSigMap -------------------------------------------------------------------------------------------------

TimeSigMap::TimeSigMap() : bar_{0}, startTick_{0}, sig_{TimeSignature{4, 4}} {}

Result<TimeSigMap> TimeSigMap::create(std::span<const TimeSigChange> changes) {
  if (changes.empty()) return invalid("time signature map needs at least one change");
  if (changes[0].bar != 0) return invalid("first time signature must be at bar 0");
  for (std::size_t i = 0; i < changes.size(); ++i) {
    const TimeSignature s = changes[i].sig;
    if (s.numerator < 1 || s.numerator > 1024)
      return invalid("time signature " + std::to_string(i) + ": numerator out of range [1, 1024]");
    if (!noteValueIsExact(s.denominator))
      return invalid("time signature " + std::to_string(i) + ": denominator " + std::to_string(s.denominator) +
                     " is not a whole number of ticks");
    if (i > 0 && changes[i].bar <= changes[i - 1].bar)
      return invalid("time signature " + std::to_string(i) + ": bars must be strictly increasing");
  }
  TimeSigMap m;
  m.bar_.clear();
  m.startTick_.clear();
  m.sig_.clear();
  m.bar_.reserve(changes.size());
  m.startTick_.reserve(changes.size());
  m.sig_.reserve(changes.size());
  Ticks tick = 0;
  for (std::size_t i = 0; i < changes.size(); ++i) {
    if (i > 0) tick += (changes[i].bar - changes[i - 1].bar) * ticksPerBar(changes[i - 1].sig);
    m.bar_.push_back(changes[i].bar);
    m.startTick_.push_back(tick);
    m.sig_.push_back(changes[i].sig);
  }
  return m;
}

std::size_t TimeSigMap::changeForBar(int64_t bar) const noexcept { return lastAtOrBefore(bar_, bar); }
std::size_t TimeSigMap::changeForTick(Ticks t) const noexcept { return lastAtOrBefore(startTick_, t); }

TimeSignature TimeSigMap::signatureAtBar(int64_t bar) const noexcept { return sig_[changeForBar(bar)]; }
TimeSignature TimeSigMap::signatureAt(Ticks t) const noexcept { return sig_[changeForTick(t)]; }

Ticks TimeSigMap::barToTicks(int64_t bar) const noexcept {
  const std::size_t i = changeForBar(bar);
  return startTick_[i] + (bar - bar_[i]) * ticksPerBar(sig_[i]);
}

BarBeatTick TimeSigMap::ticksToBarBeat(Ticks t) const noexcept {
  const std::size_t i = changeForTick(t);
  const Ticks perBar = ticksPerBar(sig_[i]);
  const Ticks perBeat = ticksPerBeat(sig_[i]);
  const Ticks rel = t - startTick_[i];
  const int64_t bars = floorDiv(rel, perBar);
  const Ticks inBar = rel - bars * perBar;
  return BarBeatTick{bar_[i] + bars, inBar / perBeat, inBar % perBeat};
}

Ticks TimeSigMap::barBeatToTicks(const BarBeatTick& p) const noexcept {
  return barToTicks(p.bar) + p.beat * ticksPerBeat(signatureAtBar(p.bar)) + p.tick;
}

}  // namespace pq
