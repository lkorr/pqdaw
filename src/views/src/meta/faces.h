#pragma once

// The built-in faces (TMM.4): their factories, registered by meta_face.cpp. Private to views.

#include <memory>
#include <string_view>
#include <vector>

#include "pq/views/meta/meta_face.h"

namespace pq::views {

std::unique_ptr<MetaFace> makePhaseAccFace(const meta::Type& t);
std::unique_ptr<MetaFace> makeWavetableFace(const meta::Type& t);  // TMM.7
std::unique_ptr<MetaFace> makeEnvelopeFace(const meta::Type& t);   // TMM.11
std::unique_ptr<MetaFace> makeVcaFace(const meta::Type& t);        // TMM.14
std::unique_ptr<MetaFace> makeSpectrumFace(const meta::Type& t);   // TX.spectrum
std::unique_ptr<MetaFace> makeScopeFace(const meta::Type& t);      // TX.scope

// TX.scope (tests): the Oscilloscope face's frame as its last poll computed it. False: not an Oscilloscope face.
struct ScopeShown {
  bool hasFrame = false, note = false, frozen = false, acquired = false;
  uint64_t start = 0;   // the frame's first sample (the feed's history frame number)
  size_t span = 0;      // samples across the width (before the zoom)
  float pitchHz = 0.0f, gain = 1.0f;
  uint32_t trails = 0, timebase = 0, channel = 0;
  double beatPhase = -1.0;
  std::vector<float> samples;  // the shown samples (one more either side of the span)
};
bool scopeShown(const MetaFace& f, ScopeShown& out);
// Sets one display option by its data key ("tb", "ch", "pers", …), or "freeze" / "sheet" (view state).
bool scopeTestControl(MetaFace& f, std::string_view key, double value);
// Pins a measurement over samples [a, b] of the frame.
bool scopeTestMeasure(MetaFace& f, double a, double b);

// TMM.16 (tests): the Wavetable face's main curve as its last paint drew it — the frame as it sounds at `position`
// (kSoundPoints + 1 values, phases i / kSoundPoints), in draw mode or as the highlighted frame of the stack. False:
// not a Wavetable face, or not painted yet.
struct WavetableShown {
  std::vector<float> values;
  float position = 0.0f;
  bool drawMode = false;
  bool heard = false;      // read from the published table (the host's reader knows the slot); else the face's Model
  uint32_t level = 0;      // ... at this mip level
  const dsp::FrameTable* table = nullptr;  // ... of this table (the one the face holds)
  std::vector<std::vector<float>> stack;  // the stacked display's frames (σ-smoothed thumbnails, kPoints + 1 each)
};
bool wavetableShown(const MetaFace& f, WavetableShown& out);

// TMM.17: the Phase accumulator face's voices by frequency (Q?-TMM.17-a). The colour: a continuous log-frequency map
// (red below 100 Hz, purple 100–300 Hz, then blue, cyan, green, yellow from 1 kHz up; 0 Hz and below: red). The orbit
// radius as a multiple of the face's orbit: kPhaseAccOrbitMax at ≤ kPhaseAccLowHz down to kPhaseAccOrbitMin at
// ≥ kPhaseAccHighHz, linear in log-frequency.
inline constexpr double kPhaseAccLowHz = 20.0, kPhaseAccHighHz = 10000.0;
inline constexpr float kPhaseAccOrbitMax = 1.25f, kPhaseAccOrbitMin = 0.25f;
ui::Rgba phaseAccVoiceColour(double hz) noexcept;
float phaseAccOrbitScale(double hz) noexcept;
// The frequency a phase-reading voice plays: |its increment wrapped to ±0.5 cycles| × its rate (FaceVoice::rate), or
// fallbackHz when the rate is unknown (0).
double faceVoiceHz(const FaceVoice& v, double fallbackHz) noexcept;

}  // namespace pq::views
