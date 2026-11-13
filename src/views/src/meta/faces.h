#pragma once

// The built-in faces (TMM.4): their factories, registered by meta_face.cpp. Private to views.

#include <memory>
#include <string_view>
#include <vector>

#include "pq/views/devices/analysis.h"
#include "pq/views/meta/meta_face.h"

namespace pq::views {

std::unique_ptr<MetaFace> makeWavetableFace(const meta::Type& t);  // TMM.7
std::unique_ptr<MetaFace> makeEnvelopeFace(const meta::Type& t);   // TMM.11
std::unique_ptr<MetaFace> makeSpectrumFace(const meta::Type& t);   // TX.spectrum
std::unique_ptr<MetaFace> makeScopeFace(const meta::Type& t);      // TX.scope
std::unique_ptr<MetaFace> makeSpectrogramFace(const meta::Type& t);  // TX.spectrogram
std::unique_ptr<MetaFace> makeNoiseFace(const meta::Type& t);        // NZ.1
// NZ.1: the Noise face's tint of a colour (dB/oct): brown −6, pink −3, white 0, blue +3, violet +6, blended between.
ui::Rgba noiseColourTint(double colour) noexcept;

// TX.spectrogram (tests): the Spectrogram face's picture as its last poll / paint left it. False: not a Spectrogram face.
struct GramShown {
  bool acquired = false, frozen = false, texture = false, loop = false;
  uint32_t cols = 0, texCols = 0, texRows = 0, hop = 0, mapping = 0;
  uint64_t read = 0;            // columns read from the feed (the newest is read − 1)
  double display = -1.0;        // the servo's right edge, in columns
  int64_t loopHead = -1;        // Loop: the slot written last
  uint64_t requests = 0;        // analysis requests sent to the feed
  devices::SpectrogramRequest request;  // the last one
  uint64_t uploads = 0, uploadBytes = 0;  // paints that uploaded, bytes handed to the texture pool
  uint64_t rebuilds = 0;  // picture geometry changes (each one re-reads and recolours the whole ring)
};
bool gramShown(const MetaFace& f, GramShown& out);
// Texel row y of column `column` (of the feed's sequence; scroll mode) as last coloured, its cached level (dB, before tilt
// and gate) and the analysis row it came from (−1: empty). False: not held.
bool gramTexel(const MetaFace& f, uint64_t column, uint32_t y, ui::Rgba& rgba, float& db, int& row);
bool gramSlotTexel(const MetaFace& f, uint32_t slot, uint32_t y, ui::Rgba& rgba);  // by ring slot (Loop)
float gramNoiseDb(const MetaFace& f, uint32_t y);  // the gate's noise floor estimate of texel row y
// Sets one display option by its data key ("span", "ch", "map", …), or "freeze" / "sheet" (view state).
bool gramTestControl(MetaFace& f, std::string_view key, double value);
bool gramTestHover(MetaFace& f, ui::Vec2 local);  // the pointer over the plot (face-local; x < 0: none)
// The analysis request a Spectrogram face with these options (its data) sends for a plot `plotDevicePx` wide (scenes:
// a manual feed computes the columns before the face first paints). The plot is 284 world units wide.
devices::SpectrogramRequest gramRequest(const meta::DataValue* data, float plotDevicePx, double sampleRate);

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


}  // namespace pq::views
