// The tone preview (pq/dsp/tone_preview.h; VZ.3). Ported from PQ Meter's audio/tone_preview.h (its ToneOsc), with a
// uint32 phase and band-limited tables in place of its per-sample additive synthesis.

#include "pq/dsp/tone_preview.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "pq/core/fast_math.h"

namespace pq::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kCutHz = 20000.0, kFadeHz = 2000.0;  // harmonics fade out between 18 and 20 kHz
constexpr int kMaxTerms = 64;                          // PQ Meter's: the next terms are inaudibly small
constexpr uint32_t kStride = TonePreview::kTableSize + 1;

double bandTop(int b) { return 40.0 * std::exp2(b); }
double weight(double f) { return std::clamp((kCutHz - f) / kFadeHz, 0.0, 1.0); }

std::vector<float> buildTables() {
  constexpr uint32_t N = TonePreview::kTableSize;
  std::vector<float> t(static_cast<size_t>(1 + 2 * TonePreview::kBands) * kStride);
  auto at = [&](int table, uint32_t i) -> float& { return t[static_cast<size_t>(table) * kStride + i]; };
  for (uint32_t i = 0; i <= N; ++i) at(0, i) = static_cast<float>(std::sin(2.0 * kPi * i / N));
  for (int b = 0; b < TonePreview::kBands; ++b) {
    // The band's highest fundamental: its harmonics are weighted there. The fundamental itself always sounds at full
    // level (it is the pitch asked for, at most 20 kHz); only the harmonics k ≥ 2 fade out under the cut.
    const double fb = bandTop(b);
    for (uint32_t i = 0; i <= N; ++i) {
      const double th = 2.0 * kPi * i / N;
      // Semisine (a half-wave rectified sine, DC removed): ½ sin θ − (2/π) Σ cos(2kθ) / (4k² − 1), peak-normalised.
      double semi = 0.5 * std::sin(th);
      for (int k = 1; k <= kMaxTerms; ++k) {
        const double w = weight(2.0 * k * fb);
        if (w <= 0.0) break;
        semi -= (2.0 / kPi) * w * std::cos(2.0 * k * th) / (4.0 * k * k - 1.0);
      }
      at(1 + b, i) = static_cast<float>(semi / (1.0 - 1.0 / kPi));
      // Triangle: (8 / π²) Σ (−1)^n sin(kθ) / k², k = 2n + 1.
      double tri = 0.0;
      for (int n = 0; n < kMaxTerms; ++n) {
        const int k = 2 * n + 1;
        const double w = k == 1 ? 1.0 : weight(k * fb);
        if (w <= 0.0) break;
        tri += ((n & 1) ? -1.0 : 1.0) * w * std::sin(k * th) / (static_cast<double>(k) * k);
      }
      at(1 + TonePreview::kBands + b, i) = static_cast<float>(tri * 8.0 / (kPi * kPi));
    }
  }
  return t;
}

const std::vector<float>& sharedTables() {
  static const std::vector<float> t = buildTables();  // thread-safe static initialisation
  return t;
}

}  // namespace

TonePreview::TonePreview() : tables_(sharedTables().data()) {}

void TonePreview::hold(const void* owner, float hz, float dbfs, ToneWave wave) noexcept {
  owner_ = owner;
  targetHz_.store(std::clamp(hz, 5.0f, 20000.0f), std::memory_order_relaxed);
  targetGain_.store(std::pow(10.0f, std::clamp(dbfs, -90.0f, 0.0f) / 20.0f), std::memory_order_relaxed);
  wave_.store(static_cast<uint8_t>(std::clamp(static_cast<int>(wave), 0, kToneWaveCount - 1)), std::memory_order_relaxed);
  gate_.store(true, std::memory_order_release);
}

void TonePreview::release(const void* owner) noexcept {
  if (owner_ != owner) return;  // another display took the tone over
  owner_ = nullptr;
  gate_.store(false, std::memory_order_release);
}

const float* TonePreview::table(ToneWave w, float hz) const noexcept {
  if (w == ToneWave::Sine) return tables_;
  int b = 0;
  while (b < kBands - 1 && static_cast<double>(hz) > bandTop(b)) ++b;
  return tables_ + static_cast<size_t>(1 + (w == ToneWave::Triangle ? kBands : 0) + b) * kStride;
}

void TonePreview::mute() noexcept {  // RT-safe
  if (gate_.load(std::memory_order_acquire)) return;
  gain_ = hz_ = 0.0f;
  goal_ = step_ = 0.0f;
  active_.store(false, std::memory_order_relaxed);
}

void TonePreview::render(float* const* out, uint32_t channels, uint32_t frames, double sampleRate) noexcept {  // RT-safe
  const bool gate = gate_.load(std::memory_order_acquire);
  if (!gate && gain_ <= 0.0f) {
    active_.store(false, std::memory_order_relaxed);
    return;
  }
  active_.store(true, std::memory_order_relaxed);
  const double sr = sampleRate > 1000.0 ? sampleRate : 48000.0;
  const float tHz = std::min(targetHz_.load(std::memory_order_relaxed), static_cast<float>(0.45 * sr));
  const float tGain = targetGain_.load(std::memory_order_relaxed);
  const auto wave = static_cast<ToneWave>(wave_.load(std::memory_order_relaxed));
  const float goal = gate ? tGain : 0.0f;
  if (goal != goal_) {  // a new goal: ramp there from where the gain is, in the attack or release time
    goal_ = goal;
    step_ = std::fabs(goal - gain_) / static_cast<float>((goal > gain_ ? kAttackSeconds : kReleaseSeconds) * sr);
  }
  const float step = std::max(step_, 1e-9f);
  const double glide = 1.0 - std::exp(-1.0 / (kGlideSeconds * sr));
  const double lt = std::log2(static_cast<double>(std::max(tHz, 1.0f)));
  if (gain_ <= 0.0f || hz_ <= 0.0f) log2Hz_ = lt;  // a fresh press starts on its pitch (no glide up from nothing)
  // One table for the block: the band of the higher of where the glide is and where it goes (band-limited for both).
  const float* tbl = table(wave, std::max(fastExp2(static_cast<float>(log2Hz_)), tHz));
  const auto incPerHz = static_cast<float>(4294967296.0 / sr);
  constexpr uint32_t kFracBits = 32 - kTableBits;
  constexpr float kFrac = 1.0f / static_cast<float>(1u << kFracBits);
  // The glide runs in double (in float it parked where the step fell under an ulp of log2 Hz: -0.66 cent at 96 kHz)
  // and snaps onto the target within 1e-5 octave (0.012 cent). On pitch, the increment is the target's, exp2 not run.
  constexpr double kSnap = 1e-5;
  uint32_t ph = phase_;
  double l2 = log2Hz_;
  float g = gain_, hz = l2 == lt ? tHz : fastExp2(static_cast<float>(l2));
  auto inc = static_cast<uint32_t>(hz * incPerHz);
  for (uint32_t f = 0; f < frames; ++f) {
    if (l2 != lt) {
      l2 += (lt - l2) * glide;  // glide in pitch, not Hz
      if (std::fabs(lt - l2) < kSnap) {
        l2 = lt;
        hz = tHz;
      } else {
        hz = fastExp2(static_cast<float>(l2));
      }
      inc = static_cast<uint32_t>(hz * incPerHz);
    }
    if (g < goal) g = std::min(goal, g + step);
    else if (g > goal) g = std::max(goal, g - step);
    const uint32_t i = ph >> kFracBits;
    const float fr = static_cast<float>(ph & ((1u << kFracBits) - 1u)) * kFrac;
    const float v = g * (tbl[i] + fr * (tbl[i + 1] - tbl[i]));
    for (uint32_t c = 0; c < channels; ++c) out[c][f] += v;
    ph += inc;
  }
  phase_ = ph;
  log2Hz_ = l2;
  gain_ = g;
  hz_ = g > 0.0f ? hz : 0.0f;
  if (!gate && g <= 0.0f) active_.store(false, std::memory_order_relaxed);
}

}  // namespace pq::dsp
