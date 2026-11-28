#pragma once

// The built-in faces (TMM.4): their factories, registered by meta_face.cpp. Private to views.

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "pq/ui/waveform.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/devices/spectrum_plot.h"
#include "pq/views/meta/meta_face.h"

namespace pq::views {

std::unique_ptr<MetaFace> makeWavetableFace(const meta::Type& t);  // TMM.7
std::unique_ptr<MetaFace> makeEnvelopeFace(const meta::Type& t);   // TMM.11
std::unique_ptr<MetaFace> makeSpectrumFace(const meta::Type& t);   // TX.spectrum
std::unique_ptr<MetaFace> makeScopeFace(const meta::Type& t);      // TX.scope
std::unique_ptr<MetaFace> makeSpectrogramFace(const meta::Type& t);  // TX.spectrogram
std::unique_ptr<MetaFace> makeNoiseFace(const meta::Type& t);        // NZ.1
std::unique_ptr<MetaFace> makeLevelMeterFace(const meta::Type& t);       // VZ.4
std::unique_ptr<MetaFace> makeLufsMeterFace(const meta::Type& t);        // VZ.4
std::unique_ptr<MetaFace> makeLoudnessHistoryFace(const meta::Type& t);  // VZ.4
std::unique_ptr<MetaFace> makeStereoFieldFace(const meta::Type& t);      // VZ.5
std::unique_ptr<MetaFace> makeStereoImageFace(const meta::Type& t);      // VZ.5
std::unique_ptr<MetaFace> makePhaseScopeFace(const meta::Type& t);       // VZ.5
std::unique_ptr<MetaFace> makeBandLevelsFace(const meta::Type& t);       // VZ.6
std::unique_ptr<MetaFace> makeWaveformFace(const meta::Type& t);         // VZ.6
std::unique_ptr<MetaFace> makeWaterfallFace(const meta::Type& t);          // VZ.7
std::unique_ptr<MetaFace> makeStereoSpectrogramFace(const meta::Type& t);  // VZ.7
std::unique_ptr<MetaFace> makeSamplerFace(const meta::Type& t);            // SM.10
// SM.10 (tests): the playheads a Sampler face shows now (0: none, or not a Sampler face).
size_t samplerFacePlayheads(const MetaFace& f);
// SM.13 (tests): the grain dots it shows now (0: none, or not a Sampler face).
size_t samplerFaceGrainDots(const MetaFace& f);
// WF.1 (tests): a Sampler face's editor view (fractions of the sample) and what its closed well's and its editor's last
// paints drew (ui::WaveformStats: the level, approx while the samples load, the curve). False: not a Sampler face.
struct SamplerFaceWave {
  double viewA = 0.0, viewB = 1.0;
  ui::WaveformStats well, editor;
};
bool samplerFaceWave(const MetaFace& f, SamplerFaceWave& out);
// BR.J (tests): the Granular grainstream as its last paint computed it — the span (seconds across the plot), a grain's
// length, each grain's onset (seconds from the plot's left edge) and whether the kernel skips it. False: not a Sampler.
struct SamplerFaceStream {
  double span = 0.0, size = 0.0;
  std::vector<std::pair<float, bool>> grains;
};
bool samplerFaceStream(const MetaFace& f, SamplerFaceStream& out);
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

// VZ.4 (tests): a loudness face (Level Meter, LUFS Meter, Loudness History) as its last poll left it. False: not one.
struct LoudnessShown {
  dsp::LoudnessReadings readings;  // what it shows
  dsp::LoudnessConfig config;      // the analysis its options ask for
  size_t points = 0;               // history points held for drawing
  bool acquired = false, frozen = false;
};
bool loudnessShown(const MetaFace& f, LoudnessShown& out);
// Sets one option by its data key ("target", "floor", …), or "freeze" / "sheet" (view state), "clear" (the CLEAR chip),
// "lamps" (a click on the Level Meter's lamps).
bool loudnessTestControl(MetaFace& f, std::string_view key, double value);
// The analysis a loudness face of type `type` with these options (its data) asks its feed for (scenes: a manual feed
// measures before the face first paints). Defaults for an unknown type.
dsp::LoudnessConfig loudnessRequest(std::string_view type, const meta::DataValue* data);

// VZ.5 (tests): a stereo face (Stereo Field, Stereo Image, Phase Scope) as its last poll left it. False: not one.
struct StereoShown {
  bool acquired = false, frozen = false;
  dsp::CrossSpectrumConfig config;       // Stereo Image / Phase Scope: the analysis its options ask for
  std::vector<dsp::StereoPoint> points;  // ... the newest it read
  size_t particles = 0;                  // ... live heat-map particles
  bool heat = false;                     // ... a glow grid was published
  std::vector<float> mono;               // Phase Scope: the eased mono curve it draws
  float correlation = 0.0f;              // Stereo Field: broadband and per band
  float bandCorrelation[3] = {0.0f, 0.0f, 0.0f};
  size_t tracePoints = 0;                // Stereo Field: points of the (broadband) trace
};
bool stereoShown(const MetaFace& f, StereoShown& out);
// Sets one option by its data key, or "freeze" / "sheet" (view state), "hoverx" (the pointer at that face x).
bool stereoTestControl(MetaFace& f, std::string_view key, double value);
// The analysis a Stereo Image / Phase Scope face with these options asks its feed for (scenes).
dsp::CrossSpectrumConfig stereoRequest(std::string_view type, const meta::DataValue* data);

// VZ.6 (tests): a Band Levels / Waveform face as its last poll left it. False: neither.
struct BandWaveShown {
  bool acquired = false;
  uint64_t columns = 0;                        // Band Levels: columns copied
  float levels[dsp::BandLevels::kBands] = {};  // the newest column (dB)
  dsp::BandLevelsConfig bands;                 // its analysis options
  uint64_t bins = 0;                           // Waveform: bins copied
  dsp::WaveConfig wave;
  dsp::WaveLoopState loop;
};
bool bandWaveShown(const MetaFace& f, BandWaveShown& out);
// Sets one option by its data key, or "freeze" / "sheet" (view state), "hover" (the pointer at world x).
bool bandWaveTestControl(MetaFace& f, std::string_view key, double value);
// The analysis a Band Levels / Waveform face with these options (its data) asks its feed for (scenes: a manual feed
// analyses before the face first paints); `rest`: the waveform's settle time. Defaults for another type.
void bandWaveRequest(std::string_view type, const meta::DataValue* data, dsp::BandLevelsConfig& bands, dsp::WaveConfig& wave,
                     float& rest);
// The Waveform face's columns of `channel` (0 L, 1 R, 2 mid, 3 side) `cols` wide, as it draws them (Scroll).
bool waveColumns(MetaFace& f, int cols, int channel, std::vector<float>& mn, std::vector<float>& mx, std::vector<float>& rms);
// VZ.7 (tests): the 3D faces as their last poll / paint left them. False: not one.
struct WaterfallShown {
  bool acquired = false, frozen = false;
  float yaw = 0, pitch = 0, roll = 0, zoom = 1;  // the camera drawn
  size_t slices = 0;                             // slices held
  uint32_t vertices = 0;                         // mesh vertices the last paint emitted
  float newestMaxDb = -140.0f;                   // the loudest point of the newest slice held
  dsp::WaterfallConfig config;                   // the analysis its options ask for
};
bool waterfallShown(const MetaFace& f, WaterfallShown& out);
struct StereoSpectrogramShown {
  bool acquired = false, frozen = false;
  float yaw = 0, pitch = 0, roll = 0, zoom = 1;
  size_t cloud = 0, ribbon = 0;            // points / ribbon slices held
  uint64_t uploadedRows = 0, redraws = 0;  // canvas rows handed to the texture pool; canvas updates
  uint64_t rebuilds = 0;                   // full re-splats (camera / option / size changes, or every update unscrolled)
  bool scrolling = false;                  // the canvas scrolls (new points splatted once)
  dsp::StereoCloudConfig config;
};
bool stereoSpectrogramShown(const MetaFace& f, StereoSpectrogramShown& out);
// Option keys, or "freeze" / "sheet", or "view" (snap to named view 0 … 3 at once).
bool waterfallTestControl(MetaFace& f, std::string_view key, double value);
bool stereoSpectrogramTestControl(MetaFace& f, std::string_view key, double value);
dsp::WaterfallConfig waterfallRequest(const meta::DataValue* data);
dsp::StereoCloudConfig stereoSpectrogramRequest(const meta::DataValue* data);
// The scrolling canvas against a fresh splat of its live points: the texels that differ (0 when unscrolled).
size_t stereoSpectrogramCanvasCheck(MetaFace& f);
// Where the canvas splat puts a point (time age u 0 … 1, frequency unit 0 … 1, pan −1 … 1) on a w × h canvas, autofit,
// orthographic, roll 0, zoom 1 (projection tests).
ui::Vec2 stereoSpectrogramCanvasAt(float yaw, float pitch, float u, float fUnit, float pan, int w, int h);

// VZ.1 (tests): the Spectrum face's plot style (its DISPLAY options). False: not a Spectrum face.
bool spectrumStyle(const MetaFace& f, devices::SpectrumPlotStyle& out);

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
  float idleAxis = 0.0f;  // BR.wt: the no-voice dot's x-axis value (Phase mode: a read phase; Shape mode: u)
};
bool wavetableShown(const MetaFace& f, WavetableShown& out);


}  // namespace pq::views
