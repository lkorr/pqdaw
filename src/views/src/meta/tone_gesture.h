#pragma once

// VZ.3: the faces' half of the tone preview (PQ Meter ui/tone_controls.h, b721058 / 5dab446), private to views: the
// shared options (snap, level, shape) and the press-hold-drag gesture of the Spectrum's plot and the Spectrogram's keys
// / Ctrl+click. The tone goes out through the target's TonePort (MetaTarget::tonePort: the app's monitor output), never
// into the project.
//
//   snap   Off: the frequency under the pointer (a drag sweeps); 12-EDO: the nearest A4 = 440 Hz semitone; Tuning (the
//          default; the microtonal extra): the nearest pitch of the instance's track tuning (MetaTarget::pitchGuides,
//          cached by guidesKey), 12-EDO where no tuning is known
//   press  hold(snapped hz) while the button is down; every move retunes (the tone glides); up / a cancelled drag /
//          a hidden face releases (the tone's 30 ms release ramp: no click)
//
// UI thread only.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "pq/views/meta/meta_face.h"
#include "pq/views/tone_port.h"

namespace pq::views {

enum class ToneSnap : uint8_t { Off, Edo12, Tuning };
inline constexpr const char* kToneSnapNames[] = {"Off", "12-EDO", "Tuning"};

// The nearest A4 = 440 Hz semitone.
inline float snapTo12Edo(float hz) noexcept {
  if (!(hz > 0.0f)) return hz;
  const double m = std::round(69.0 + 12.0 * std::log2(static_cast<double>(hz) / 440.0));
  return static_cast<float>(440.0 * std::exp2((m - 69.0) / 12.0));
}

// The guide nearest to hz in pitch (guides ascending); hz itself when there are none.
inline float snapToGuides(float hz, const std::vector<PitchGuide>& g) noexcept {
  if (g.empty() || !(hz > 0.0f)) return hz;
  auto it = std::lower_bound(g.begin(), g.end(), hz, [](const PitchGuide& a, float f) { return a.hz < f; });
  if (it == g.end()) return g.back().hz;
  if (it == g.begin()) return it->hz;
  const float above = it->hz, below = (it - 1)->hz;
  return std::log(above / hz) < std::log(hz / below) ? above : below;
}

class ToneGesture {
public:
  bool held() const noexcept { return held_; }
  float hz() const noexcept { return hz_; }  // the sounding (snapped) pitch while held

  float snapped(const MetaTarget* t, ToneSnap snap, float hz) {
    if (snap == ToneSnap::Off) return hz;
    if (snap == ToneSnap::Tuning && t != nullptr) {
      const uint64_t key = t->guidesKey();
      if (!guidesValid_ || key != guidesKey_) {
        t->pitchGuides(8.0f, 24000.0f, guides_);
        guidesKey_ = key;
        guidesValid_ = true;
      }
      if (!guides_.empty()) return snapToGuides(hz, guides_);
    }
    return snapTo12Edo(hz);
  }
  // A press (or a move while held): the tone at the pointer's frequency.
  void hold(TonePort* port, const void* owner, const MetaTarget* t, ToneSnap snap, float pointerHz, float dbfs,
            dsp::ToneWave wave) {
    if (port == nullptr || !(pointerHz > 0.0f)) return;
    held_ = true;
    hz_ = snapped(t, snap, pointerHz);
    port->hold(owner, hz_, dbfs, wave);
  }
  void release(TonePort* port, const void* owner) {
    if (!held_) return;
    held_ = false;
    hz_ = 0.0f;
    if (port != nullptr) port->release(owner);
  }

private:
  bool held_ = false;
  float hz_ = 0.0f;
  std::vector<PitchGuide> guides_;
  uint64_t guidesKey_ = 0;
  bool guidesValid_ = false;
};

}  // namespace pq::views
