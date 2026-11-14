// The Spectrogram face (TX.spectrogram; SPEC §15 2026-11-01 "analysis modules", spectrogram added by the user): the
// module IS its picture. PQ Meter's spectrogram (pq meter/src/modules/spectrogram.cpp, read in full) is the reference for
// what it does and how it looks: the analysis is pq/dsp/spectrogram.h on the feed's worker (kWantSpectrogram), this
// file is PQ Meter's display side (uploadDirty, estimateNoise, applyGate, the smooth-scroll servo, the grid, the time
// ruler, the keyboard, the hover readout) in pqdaw's face idiom.
//
//   picture    a ring texture of (visible columns + 4) × (the plot's device px height, rounded up to 64) RGBA texels,
//              one column per analysis hop; only the columns that are new (or whose colour changed) are coloured and
//              uploaded (TexturePool::update, one rect per run of changed columns), the rest stays on the GPU. Drawn as
//              one or two textured quads that scroll by a servo (sub-column smooth); Loop: the columns are placed by
//              the transport's beat over a span (1 beat … 4 bars) and the write head sweeps, the last pass standing.
//   colour     a column's 1024 analysis rows are reduced by MAX into the texel rows (thin lines survive), its level
//              cached per texel; then tilt (dB/oct about 1 kHz), the visual gate (a downward expander keyed to each
//              row's own noise floor, estimated from the visible history and slewed), floor / ceiling, gamma, fade,
//              through one of the ten colormaps by level (or hue by frequency / by the column's brightness). An option
//              or gate move recolours from the cache (no analysis) and uploads only texels that changed.
//   overlays   the frequency rules + labels (decades on log / mel, round steps on linear), the time ruler (seconds;
//              Loop: beat rules), the piano keyboard (12-EDO keys at their pitch, C tinted, the hovered key lit), the
//              track tuning's degrees as guide lines (pqdaw's extra), the hover line with Hz and the nearest note
//              (C3 = 261.626 Hz naming) and degree, chips and the settings sheet.
//   chips      top-left: the channel (MID / L / R / SIDE), the axis (LOG / MEL / LIN), the colormap; top-right: FREEZE
//              (the picture holds; the analysis goes on), SET (every option as a row: a click cycles a choice, a vertical
//              drag sets a number, Shift fine, a double-click resets; RESET puts them all back)
//
// Options are the instance's data (a record of numbers: pq/meta/spectrogram.h), saved, undoable, never recompiling; a
// read-only target keeps them for the face's life. Freeze and the sheet are view state. World units 300 × 170.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "faces.h"
#include "option_sheet.h"
#include "pq/dsp/spectrogram.h"
#include "pq/ui/colormap.h"
#include "pq/ui/renderer.h"
#include "pq/ui/texture_pool.h"
#include "pq/views/devices/analysis.h"

namespace pq::views {

namespace {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;
using dsp::GramScale;
using devices::SpectrumFeed;

constexpr float kW = 300.0f, kH = 170.0f;
constexpr float kRadius = 10.0f;
constexpr Rect kPlot{8.0f, 8.0f, kW - 8.0f, kH - 8.0f};
constexpr float kChipH = 10.0f, kChipY = 12.0f;
constexpr Rect kChChip{12.0f, kChipY, 40.0f, kChipY + kChipH};
constexpr Rect kScaleChip{43.0f, kChipY, 69.0f, kChipY + kChipH};
constexpr Rect kMapChip{72.0f, kChipY, 112.0f, kChipY + kChipH};
constexpr Rect kFreezeChip{kW - 74.0f, kChipY, kW - 41.0f, kChipY + kChipH};
constexpr Rect kSetChip{kW - 38.0f, kChipY, kW - 12.0f, kChipY + kChipH};
constexpr float kLabelPx = 7.0f;           // world units
constexpr float kLabelMinDevicePx = 6.0f;  // below this the text and chips are left out (the picture stays)
constexpr uint32_t kRows = dsp::Spectrogram::kRows;
constexpr uint32_t kMinCols = 64, kMaxCols = 1024;
constexpr uint32_t kRowGranule = 64;
constexpr uint32_t kGateEvery = 8;  // columns between noise-floor estimates (PQ Meter: kCols / 256)
constexpr uint32_t kSpare = 16;     // ring slots past the visible columns: the servo's lag and the blank next slot
constexpr double kMaxLag = 10.0;    // columns the servo may trail the newest before it snaps

enum Part : int32_t {
  kPlotPart = 0, kChPart = 1, kScalePart = 2, kMapPart = 3, kFreezePart = 4, kSetPart = 5, kSheetPart = 6, kRowBase = 100
};

constexpr Rgba kGrid = ui::signal_theme::kGrid, kGridLabel = ui::signal_theme::kGridLabel;
constexpr Rgba kFrozen = ui::rgba(255, 180, 90, 230);
constexpr Rgba kPad = ui::rgba(8, 8, 12, 200);

bool inside(const Rect& r, Vec2 p) noexcept { return p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1; }

// ---- options (the instance's data) -----------------------------------------------------------------------------------

constexpr const char* kChLong[] = {"Mid", "Left", "Right", "Side"};
constexpr const char* kChShort[] = {"MID", "L", "R", "SIDE"};
constexpr const char* kScaleLong[] = {"Logarithmic", "Mel", "Linear"};
constexpr const char* kScaleShort[] = {"LOG", "MEL", "LIN"};
constexpr const char* kMapNames[] = {"Magma", "Inferno", "Viridis", "Grayscale", "Ice", "Gold", "Silver", "Pastel", "Neon", "Copper"};
constexpr const char* kMapShort[] = {"MAGMA", "INFERNO", "VIRIDIS", "GRAY", "ICE", "GOLD", "SILVER", "PASTEL", "NEON", "COPPER"};
constexpr const char* kSourceNames[] = {"Level", "Frequency", "Brightness"};
constexpr const char* kPlayheadNames[] = {"Scroll", "Loop"};
constexpr double kBeatSpans[] = {1.0, 2.0, 4.0, 8.0, 16.0};
constexpr const char* kBeatSpanNames[] = {"1 beat", "2 beats", "1 bar", "2 bars", "4 bars"};
constexpr const char* kKeysNames[] = {"Off", "Left", "Right"};
static_assert(std::size(kMapNames) == static_cast<size_t>(ui::kColormapCount));

using sheet::Opt;
using sheet::kOnOff;
enum O : uint32_t {
  oSpan, oFft, oChannel, oReassign, oMaxDev, oConsensus, oPlayhead, oLoopSpan, oFloor, oCeil, oGamma, oFade, oTilt,
  oGate, oDepth, oThresh, oKnee, oExpand, oScale, oFMin, oFMax, oMap, oSource, oGrid, oRuler, oNote, oKeys, oTuning,
  oKeysW, oKeysOpacity, oKeysTintC, oCount
};
// PQ Meter's defaults (the power user's tuned analysis, the MiniMeters-calibrated level mapping, the gate on): a 2048
// window, reassignment, 3.5 bins, consensus 0.5, floor −100 / ceiling −24 dB, gamma 1.57, fade 0.4, tilt 4.5 dB/oct,
// gate depth 1 / threshold −6 / knee 40 / expand 1.31, log axis from 0 Hz, Magma, grid, ruler and note readout on. The
// span is PQ Meter's 9 s at 1× scaled to a face (6 s: PQ Meter's 9 s are across a whole screen).
const Opt kOpts[oCount] = {
    {"span", "Span", 6.0, 0.5, 60.0, Opt::LogNumber, nullptr, "%.1f s"},
    {"fft", "Window", 11, 8, 14, Opt::Pow2},
    {"ch", "Channel", 0, 0, 3, Opt::Choice, kChLong},
    {"re", "Reassign", 1, 0, 1, Opt::Toggle, kOnOff},
    {"dev", "Max deviation", 3.5, 0.5, 8.0, Opt::Number, nullptr, "%.1f bins", 0.1},
    {"cons", "Consensus", 0.5, 0.0, 2.0, Opt::Number, nullptr, "%.2f bins", 0.05},
    {"ph", "Playhead", 0, 0, 1, Opt::Choice, kPlayheadNames},
    {"loop", "Loop span", 2, 0, 4, Opt::Choice, kBeatSpanNames},
    {"floor", "Floor", -100, -140, -40, Opt::Number, nullptr, "%.0f dB", 1.0},
    {"ceil", "Ceiling", -24, -60, 12, Opt::Number, nullptr, "%.0f dB", 1.0},
    {"gamma", "Gamma", 1.57, 0.3, 3.0, Opt::Number, nullptr, "%.2f", 0.01},
    {"fade", "Fade", 0.4, 0.0, 1.0, Opt::Number, nullptr, "%.2f", 0.01},
    {"tilt", "Tilt", 4.5, -6.0, 6.0, Opt::Number, nullptr, "%.1f dB/oct", 0.1},
    {"gate", "Visual gate", 1, 0, 1, Opt::Toggle, kOnOff},
    {"gdep", "Gate depth", 1.0, 0.0, 1.0, Opt::Number, nullptr, "%.2f", 0.01},
    {"gthr", "Gate threshold", -6, -6, 24, Opt::Number, nullptr, "%+.0f dB", 1.0},
    {"gknee", "Gate knee", 40, 1, 40, Opt::Number, nullptr, "%.0f dB", 1.0},
    {"gexp", "Gate expand", 1.31, 1.0, 4.0, Opt::Number, nullptr, "%.2fx", 0.01},
    {"scale", "Frequency scale", 0, 0, 2, Opt::Choice, kScaleLong},
    {"fmin", "Min frequency", 0, 0, 500, Opt::Number, nullptr, "%.0f Hz", 1.0},
    {"fmax", "Max frequency", 24000, 1000, 24000, Opt::LogNumber, nullptr, "%.0f Hz", 10.0},
    {"map", "Colormap", 0, 0, 9, Opt::Choice, kMapNames},
    {"src", "Colour by", 0, 0, 2, Opt::Choice, kSourceNames},
    {"grid", "Frequency grid", 1, 0, 1, Opt::Toggle, kOnOff},
    {"ruler", "Time ruler", 1, 0, 1, Opt::Toggle, kOnOff},
    {"note", "Note readout", 1, 0, 1, Opt::Toggle, kOnOff},
    {"keys", "Keyboard", 0, 0, 2, Opt::Choice, kKeysNames},
    {"tune", "Scale lines", 0, 0, 1, Opt::Toggle, kOnOff},
    // VZ.1: PQ Meter's keyboard_width / keyboard_opacity / keyboard_tint_c (its 40 px at a face's scale: 22 units).
    {"kw", "Keyboard width", 22, 8, 60, Opt::Number, nullptr, "%.0f", 1.0},
    {"kop", "Keyboard opacity", 0.85, 0.1, 1.0, Opt::Number, nullptr, "%.2f", 0.01},
    {"kc", "Tint the Cs", 1, 0, 1, Opt::Toggle, kOnOff},
};

struct Options : sheet::Values {
  Options() : Values(kOpts) {}
  GramScale scale() const noexcept { return static_cast<GramScale>(std::clamp(choice(oScale), 0, 2)); }
  bool loop() const noexcept { return choice(oPlayhead) == 1; }
  // The level range (the ceiling at least 10 dB over the floor, as PQ Meter's sliders keep it).
  float floorDb() const noexcept { return static_cast<float>((*this)[oFloor]); }
  float ceilDb() const noexcept { return std::max(static_cast<float>((*this)[oCeil]), floorDb() + 10.0f); }
  float fMin() const noexcept { return static_cast<float>((*this)[oFMin]); }
  float fMax() const noexcept { return std::max(static_cast<float>((*this)[oFMax]), 2.0f * fMin()); }
  float keysW() const noexcept { return choice(oKeys) != 0 ? static_cast<float>((*this)[oKeysW]) : 0.0f; }
};

// The rows a sheet tab shows for these options (in order): ANALYSIS what the analysis computes (it re-analyses or
// re-reads), DISPLAY how the columns are coloured and what is drawn over them (recoloured from the cache).
void visibleRows(const Options& o, int tab, std::vector<uint32_t>& out) {
  out.clear();
  if (tab == sheet::kAnalysis) {
    for (const uint32_t i : {oSpan, oFft, oChannel, oReassign}) out.push_back(i);
    if (o.on(oReassign)) {
      out.push_back(oMaxDev);
      out.push_back(oConsensus);
    }
    out.push_back(oPlayhead);
    if (o.loop()) out.push_back(oLoopSpan);
    for (const uint32_t i : {oScale, oFMin, oFMax}) out.push_back(i);
    return;
  }
  for (const uint32_t i : {oFloor, oCeil, oGamma, oFade, oTilt, oGate}) out.push_back(i);
  if (o.on(oGate))
    for (const uint32_t i : {oDepth, oThresh, oKnee, oExpand}) out.push_back(i);
  for (const uint32_t i : {oMap, oSource, oGrid, oRuler, oNote, oTuning, oKeys}) out.push_back(i);
  if (o.choice(oKeys) != 0)
    for (const uint32_t i : {oKeysW, oKeysOpacity, oKeysTintC}) out.push_back(i);
}

// The nearest 12-EDO note of hz (C3 = 261.626 Hz, the spectrum display's naming) and its cents.
void noteName(double hz, char* out, size_t n) {
  static const char* const kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (!(hz > 8.0)) {
    out[0] = '\0';
    return;
  }
  const double semis = 60.0 + 12.0 * std::log2(hz / 261.6256);
  const long midi = std::lround(semis);
  const int cents = static_cast<int>(std::lround((semis - static_cast<double>(midi)) * 100.0));
  const long octave = midi / 12 - 2 - (midi < 0 && midi % 12 != 0 ? 1 : 0);
  std::snprintf(out, n, "%s%ld %+d", kNames[((midi % 12) + 12) % 12], octave, cents);
}

// What the analysis must compute for a display of `cols` columns with these options: the hop that puts the span (Loop
// with a known tempo: the beat span) across the columns.
devices::SpectrogramRequest requestFor(const Options& o, uint32_t cols, double sr, double beatRate) {
  devices::SpectrogramRequest q;
  double hop = o[oSpan] * sr / static_cast<double>(std::max<uint32_t>(1, cols));
  if (o.loop() && beatRate > 0.0) {
    const double spanBeats = kBeatSpans[std::clamp(o.choice(oLoopSpan), 0, 4)];
    hop = spanBeats / (beatRate * static_cast<double>(cols));
  }
  q.config.sampleRate = sr;
  q.config.hop = static_cast<uint32_t>(std::clamp(std::lround(hop), 32L, 8192L));
  q.config.fftSize = 1u << std::clamp(o.choice(oFft), 8, 14);
  q.config.reassign = o.on(oReassign);
  q.config.maxBinDev = static_cast<float>(o[oMaxDev]);
  q.config.consensus = static_cast<float>(o[oConsensus]);
  q.config.floorDb = o.floorDb();
  q.config.scale = o.scale();
  q.config.fMin = o.fMin();
  q.config.fMax = o.fMax();
  q.channel = static_cast<uint32_t>(std::clamp(o.choice(oChannel), 0, 3));
  q.columns = cols;
  return q;
}

// The columns a plot `wPx` device px wide shows: the width in 32-column steps.
uint32_t columnsFor(float wPx) noexcept {
  const auto want = static_cast<uint32_t>(std::clamp(std::lround(wPx), static_cast<long>(kMinCols), static_cast<long>(kMaxCols)));
  return std::clamp<uint32_t>((want + 16) / 32 * 32, kMinCols, kMaxCols);
}

class FeedFrames final : public VoiceTapReader {
public:
  uint32_t read(std::span<FaceVoice>) override { return 0; }
  uint64_t serial() const noexcept override { return serial_; }
  bool live() const noexcept override { return live_; }
  uint64_t serial_ = 0;
  bool live_ = false;
};

// ---- the picture: the column cache, the colouring, the texture ------------------------------------------------------

struct Tone {  // what the colouring of a texel depends on (a change recolours from the cache)
  float floorDb = -100, ceilDb = -24, gamma = 1.57f, fade = 0.4f, tilt = 4.5f;
  bool gate = true;
  float depth = 1, thresh = -6, knee = 40, expand = 1.31f;
  int map = 0, source = 0;
  GramScale scale = GramScale::Log;
  float fMin = 0, fMax = 24000;
  bool operator==(const Tone&) const = default;
};

class Picture {
public:
  uint32_t cols() const noexcept { return cols_; }
  uint32_t texCols() const noexcept { return texCols_; }
  uint32_t texRows() const noexcept { return texRows_; }
  bool sized() const noexcept { return texRows_ > 0; }

  // Geometry: `cols` columns of this display, `rows` texel rows, a ring of at least `ring` columns (+ the spare ones:
  // another display of the instance may decide a finer hop, so this one shows more columns for the same span). Clears
  // the picture when it changes.
  bool resize(uint32_t cols, uint32_t rows, uint32_t ring) {
    const uint32_t tc = std::max(cols, ring) + kSpare;
    if (cols == cols_ && rows == texRows_ && tc == texCols_) return false;
    cols_ = cols;
    texCols_ = tc;
    texRows_ = rows;
    const size_t n = static_cast<size_t>(texCols_) * texRows_;
    cacheDb_.assign(n, 0.0f);
    cacheRow_.assign(n, int16_t{-1});
    mirror_.assign(n, 0u);  // transparent: differs from every colour, so the first colouring uploads everything
    bright_.assign(texCols_, 0.0f);
    filled_.assign(texCols_, 0);
    dirty_.assign(texCols_, 1);
    toneRev_.assign(texCols_, 0u);
    noise_.assign(texRows_, tone_.floorDb);
    noiseValid_ = false;
    noiseSeeded_ = false;
    appliedValid_ = false;
    pending_.assign(texRows_, 0);
    sinceNoise_ = phase_;  // faces estimate on different columns (four faces fed alike do not all estimate in one frame)
    allChanged_ = true;
    return true;
  }
  void clear() {
    std::fill(cacheRow_.begin(), cacheRow_.end(), int16_t{-1});
    std::fill(filled_.begin(), filled_.end(), uint8_t{0});
    std::fill(dirty_.begin(), dirty_.end(), uint8_t{1});
    noiseValid_ = false;
    noiseSeeded_ = false;
  }
  void setTone(const Tone& t) {
    if (t == tone_ && lut_ != nullptr) return;
    const bool first = lut_ == nullptr;
    const bool axis = first || t.scale != tone_.scale || t.fMin != tone_.fMin || t.fMax != tone_.fMax || t.tilt != tone_.tilt;
    const bool level = t.floorDb != tone_.floorDb || t.ceilDb != tone_.ceilDb || t.gate != tone_.gate ||
                       t.depth != tone_.depth || t.thresh != tone_.thresh || t.knee != tone_.knee || t.expand != tone_.expand;
    const bool source = t.source != tone_.source;
    tone_ = t;
    if (axis) {
      tilt_.resize(kRows);
      for (uint32_t r = 0; r < kRows; ++r) {
        if (t.tilt == 0.0f) {
          tilt_[r] = 0.0f;
          continue;
        }
        const float u = 1.0f - static_cast<float>(r) / static_cast<float>(kRows - 1);
        const float f = dsp::gramHz(t.scale, u, t.fMin, t.fMax);
        tilt_[r] = t.tilt * std::log2(std::max(f, 1.0f) / 1000.0f);  // +tilt dB/oct about 1 kHz
      }
    }
    if (level) {
      noiseValid_ = false;  // the floor estimate describes the old settings: the next one snaps
      noiseSeeded_ = false;
      appliedValid_ = false;
    }
    // The two power curves per texel as tables (a recolour of the whole ring is ~200 k texels: no pow per texel).
    gammaLut_.resize(4097);
    expandLut_.resize(4097);
    for (uint32_t i = 0; i <= 4096; ++i) {
      const float x = static_cast<float>(i) / 4096.0f;
      gammaLut_[i] = std::pow(x, t.gamma);
      expandLut_[i] = std::pow(x, 1.0f / std::max(1.0f, t.expand));
    }
    lut_ = &ui::colormapLut(static_cast<ui::Colormap>(std::clamp(t.map, 0, ui::kColormapCount - 1)));
    if (source) std::fill(dirty_.begin(), dirty_.end(), uint8_t{1});  // brightness is computed at the reduce
    ++rev_;  // recolour everything at the next colour()
  }
  const Tone& tone() const noexcept { return tone_; }

  // One analysis column into slot `slot` (reduced by max into the texel rows; its level and source row cached).
  void put(uint32_t slot, const float* rows) {
    float* cdb = cacheDb_.data() + static_cast<size_t>(slot) * texRows_;
    int16_t* crow = cacheRow_.data() + static_cast<size_t>(slot) * texRows_;
    filled_[slot] = rows != nullptr;
    if (rows == nullptr) {
      std::fill_n(crow, texRows_, int16_t{-1});
    } else {
      if (tone_.source == 2) {  // brightness: the energy above 700 Hz over the whole column (PQ Meter: the Waveform's split)
        const float u = dsp::gramUnit(tone_.scale, 700.0f, tone_.fMin, tone_.fMax);
        const auto split = static_cast<uint32_t>(std::clamp((1.0f - u) * static_cast<float>(kRows - 1), 0.0f, static_cast<float>(kRows - 1)));
        double lo = 0.0, hi = 0.0;
        for (uint32_t r = 0; r < split; ++r) hi += rows[r];
        for (uint32_t r = split; r < kRows; ++r) lo += rows[r];
        if (lo + hi > 1e-18) bright_[slot] = static_cast<float>(hi / (lo + hi));
      }
      for (uint32_t y = 0; y < texRows_; ++y) {
        const auto r0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * kRows / texRows_);
        uint32_t r1 = static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * kRows / texRows_);
        if (r1 <= r0) r1 = r0 + 1;
        float best = 0.0f;
        uint32_t bestRow = r0;
        for (uint32_t r = r0; r < r1 && r < kRows; ++r)
          if (rows[r] > best) {
            best = rows[r];
            bestRow = r;
          }
        if (best > 1e-12f) {
          cdb[y] = 10.0f * std::log10(best);
          crow[y] = static_cast<int16_t>(bestRow);
        } else {
          crow[y] = -1;
        }
      }
    }
    dirty_[slot] = 1;
  }
  void copySlot(uint32_t from, uint32_t to) {
    std::copy_n(cacheDb_.begin() + static_cast<std::ptrdiff_t>(from) * texRows_, texRows_,
                cacheDb_.begin() + static_cast<std::ptrdiff_t>(to) * texRows_);
    std::copy_n(cacheRow_.begin() + static_cast<std::ptrdiff_t>(from) * texRows_, texRows_,
                cacheRow_.begin() + static_cast<std::ptrdiff_t>(to) * texRows_);
    bright_[to] = bright_[from];
    filled_[to] = filled_[from];
    dirty_[to] = 1;
  }
  void columnAdded() noexcept { ++sinceNoise_; }
  void setPhase(uint32_t p) noexcept { phase_ = sinceNoise_ = p % kGateEvery; }

  // The per-row noise floor (PQ Meter's estimateNoise on the texel rows): a low percentile of each row over the visible
  // slots (every 4th; walked slot by slot, the cache being column-major), then the median over a frequency neighbourhood
  // (noise is broadband, a partial is a line), slewed (up 0.25 dB, down 2 dB per pass; the first real pass snaps).
  void estimateNoise(const std::vector<uint32_t>& visible) {
    const uint32_t rows = texRows_;
    if (noise_.size() != rows) noise_.assign(rows, tone_.floorDb);
    constexpr uint32_t kStride = 4;
    const size_t nSamp = visible.size() / kStride + 1;
    vals_.resize(static_cast<size_t>(rows) * nSamp);
    counts_.assign(rows, 0u);
    for (size_t k = 0; k < visible.size(); k += kStride) {
      const size_t base = static_cast<size_t>(visible[k]) * rows;
      const int16_t* crow = cacheRow_.data() + base;
      const float* cdb = cacheDb_.data() + base;
      for (uint32_t y = 0; y < rows; ++y)
        if (crow[y] >= 0) vals_[static_cast<size_t>(y) * nSamp + counts_[y]++] = cdb[y];
    }
    rowDb_.assign(rows, -300.0f);
    uint32_t measured = 0;
    for (uint32_t y = 0; y < rows; ++y) {
      const uint32_t n = counts_[y];
      if (n < 8) continue;
      float* v = vals_.data() + static_cast<size_t>(y) * nSamp;
      std::nth_element(v, v + n / 4, v + n);  // the 25th percentile over time
      rowDb_[y] = v[n / 4];
      ++measured;
    }
    const auto half = static_cast<int32_t>(std::max<uint32_t>(2, rows / 32));
    target_.assign(noise_.begin(), noise_.end());
    for (int32_t y = 0; y < static_cast<int32_t>(rows); ++y) {
      nb_.clear();
      for (int32_t i = std::max(0, y - half); i <= std::min<int32_t>(static_cast<int32_t>(rows) - 1, y + half); ++i)
        if (rowDb_[static_cast<size_t>(i)] > -300.0f) nb_.push_back(rowDb_[static_cast<size_t>(i)]);
      if (nb_.size() < 4) {
        target_[static_cast<size_t>(y)] = tone_.floorDb;
        continue;
      }
      const size_t m = nb_.size() / 2;
      std::nth_element(nb_.begin(), nb_.begin() + static_cast<std::ptrdiff_t>(m), nb_.end());
      target_[static_cast<size_t>(y)] = nb_[m];
    }
    const float maxNoise = tone_.ceilDb - tone_.thresh - 45.0f;  // the knee never climbs into the top 45 dB
    const bool slew = noiseSeeded_;
    for (uint32_t y = 0; y < rows; ++y) {
      const float d = std::min(target_[y], maxNoise) - noise_[y];
      noise_[y] += slew ? std::clamp(d, -2.0f, 0.25f) : d;
    }
    noiseValid_ = true;
    if (measured >= rows / 4) noiseSeeded_ = true;
    sinceNoise_ = 0;
    // Apply: the colours follow the estimate row by row, once a row's estimate is 0.5 dB from what it is painted with (a
    // fraction of a colormap step: invisible as a step), so a slow drift recolours a few rows now and then instead of
    // the whole picture every pass (PQ Meter recoloured the visible ring; four faces doing that in one frame cost ms).
    if (applied_.size() != rows || !appliedValid_) {
      applied_ = noise_;
      appliedValid_ = true;
      ++rev_;  // everything, at the next colour()
      return;
    }
    if (pending_.size() != rows) pending_.assign(rows, 0);
    for (uint32_t y = 0; y < rows; ++y)
      if (std::fabs(noise_[y] - applied_[y]) >= 0.5f) pending_[y] = 1;
  }
  bool noiseDue() const noexcept { return tone_.gate && (!noiseValid_ || sinceNoise_ >= kGateEvery); }

  // Colours the slots that are dirty or behind the tone revision; returns the slots whose texels changed (in order).
  const std::vector<uint32_t>& colour() {
    changed_.clear();
    if (!sized() || lut_ == nullptr) return changed_;
    const uint32_t rows = texRows_;
    col_.resize(rows);
    for (uint32_t s = 0; s < texCols_; ++s) {
      if (!dirty_[s] && toneRev_[s] == rev_ && !allChanged_) continue;
      dirty_[s] = 0;
      toneRev_[s] = rev_;
      const float* cdb = cacheDb_.data() + static_cast<size_t>(s) * rows;
      const int16_t* crow = cacheRow_.data() + static_cast<size_t>(s) * rows;
      // VZ.1: an empty slot (a rebuild's whole ring until it is re-read) is one colour: no per-texel tonemap.
      if (!filled_[s]) std::fill_n(col_.data(), rows, texelColour(0.0f, int16_t{-1}, 0, s));
      else
        for (uint32_t y = 0; y < rows; ++y) col_[y] = texelColour(cdb[y], crow[y], y, s);
      uint32_t* m = mirror_.data() + static_cast<size_t>(s) * rows;
      if (std::memcmp(col_.data(), m, static_cast<size_t>(rows) * 4) != 0) {  // upload only what changed
        std::memcpy(m, col_.data(), static_cast<size_t>(rows) * 4);
        changed_.push_back(s);
      }
    }
    allChanged_ = false;
    return changed_;
  }
  // Recolours rows the gate's floor moved in (every slot), at most an eighth of the rows per paint (a drift moves many
  // rows at once; spread over a few paints it is invisible and costs a fraction of a millisecond each); returns the rows
  // whose texels changed (ascending).
  const std::vector<uint32_t>& colourMovedRows() {
    changedRows_.clear();
    const uint32_t rows = texRows_;
    if (!sized() || lut_ == nullptr || pending_.size() != rows) return changedRows_;
    take_.clear();
    const uint32_t budget = std::max<uint32_t>(16, rows / 8);
    for (uint32_t k = 0; k < rows && take_.size() < budget; ++k) {
      const uint32_t y = (rowCursor_ + k) % rows;
      if (!pending_[y]) continue;
      pending_[y] = 0;
      applied_[y] = noise_[y];
      take_.push_back(y);
      rowCursor_ = (y + 1) % rows;
    }
    if (take_.empty()) return changedRows_;
    std::sort(take_.begin(), take_.end());
    rowHit_.assign(rows, 0);
    for (uint32_t s = 0; s < texCols_; ++s) {  // slot by slot: the cache and the mirror are column-major
      const size_t base = static_cast<size_t>(s) * rows;
      for (const uint32_t y : take_) {
        const uint32_t c = texelColour(cacheDb_[base + y], cacheRow_[base + y], y, s);
        if (c != mirror_[base + y]) {
          mirror_[base + y] = c;
          rowHit_[y] = 1;
        }
      }
    }
    for (const uint32_t y : take_)
      if (rowHit_[y]) changedRows_.push_back(y);
    return changedRows_;
  }
  // Uploads whole rows (ascending): one rect per run of consecutive rows across the ring.
  void uploadRows(ui::TexturePool& pool, ui::TextureId tex, const std::vector<uint32_t>& rowsChanged) {
    const uint32_t rows = texRows_;
    for (size_t i = 0; i < rowsChanged.size();) {
      size_t j = i + 1;
      while (j < rowsChanged.size() && rowsChanged[j] == rowsChanged[j - 1] + 1) ++j;
      const uint32_t y0 = rowsChanged[i];
      const auto n = static_cast<uint32_t>(j - i);
      staging_.resize(static_cast<size_t>(n) * texCols_);
      for (uint32_t k = 0; k < n; ++k)
        for (uint32_t s = 0; s < texCols_; ++s)
          staging_[static_cast<size_t>(k) * texCols_ + s] = mirror_[static_cast<size_t>(s) * rows + y0 + k];
      (void)pool.update(tex, {0, y0, texCols_, n}, {reinterpret_cast<const uint8_t*>(staging_.data()), staging_.size() * 4});
      i = j;
    }
  }
  // Uploads the slots `changed` (ascending) to `tex`: one rect per run of consecutive slots (the mirror is column-major;
  // a run is transposed into row-major staging, at most 256 columns at a time).
  void upload(ui::TexturePool& pool, ui::TextureId tex, const std::vector<uint32_t>& changed) {
    const uint32_t rows = texRows_;
    for (size_t i = 0; i < changed.size();) {
      size_t j = i + 1;
      while (j < changed.size() && changed[j] == changed[j - 1] + 1 && j - i < 256) ++j;
      const uint32_t s0 = changed[i];
      const auto n = static_cast<uint32_t>(j - i);
      const uint32_t* src = mirror_.data() + static_cast<size_t>(s0) * rows;
      if (n == 1) {
        (void)pool.update(tex, {s0, 0, 1, rows}, {reinterpret_cast<const uint8_t*>(src), static_cast<size_t>(rows) * 4}, 4);
      } else {
        staging_.resize(static_cast<size_t>(n) * rows);
        for (uint32_t k = 0; k < n; ++k)
          for (uint32_t y = 0; y < rows; ++y) staging_[static_cast<size_t>(y) * n + k] = src[static_cast<size_t>(k) * rows + y];
        (void)pool.update(tex, {s0, 0, n, rows}, {reinterpret_cast<const uint8_t*>(staging_.data()), staging_.size() * 4});
      }
      i = j;
    }
  }
  void markAllChanged() noexcept { allChanged_ = true; }

  // Tests: the texel and the cached level of slot s, row y.
  uint32_t texel(uint32_t s, uint32_t y) const noexcept { return mirror_[static_cast<size_t>(s) * texRows_ + y]; }
  int16_t sourceRow(uint32_t s, uint32_t y) const noexcept { return cacheRow_[static_cast<size_t>(s) * texRows_ + y]; }
  float levelDb(uint32_t s, uint32_t y) const noexcept { return cacheDb_[static_cast<size_t>(s) * texRows_ + y]; }
  float noiseDb(uint32_t y) const noexcept { return y < noise_.size() ? noise_[y] : 0.0f; }

private:
  // A texel's colour: its cached level, tilted (the source row's), gated (this row's applied floor), mapped to 0 … 1
  // between floor and ceiling, gamma, then the colormap by level (fade toward black at the quiet end) or by the
  // attribute with the level as brightness.
  uint32_t texelColour(float level, int16_t br, uint32_t y, uint32_t s) const noexcept {
    float t = 0.0f;
    if (br >= 0) {
      const float tilt = tilt_[static_cast<size_t>(br)];
      float db = level + tilt;
      if (tone_.gate) db = gate(db, (y < applied_.size() ? applied_[y] : tone_.floorDb) + tilt);
      t = std::clamp((db - tone_.floorDb) / std::max(1.0f, tone_.ceilDb - tone_.floorDb), 0.0f, 1.0f);
      t = gammaLut_[static_cast<size_t>(t * 4096.0f + 0.5f)];
    }
    const float fa = tone_.fade > 0.0f ? std::clamp(t / tone_.fade, 0.0f, 1.0f) : 1.0f;
    Rgba c;
    if (tone_.source == 0) {
      c = ui::lutAt(*lut_, t);
      if (tone_.fade > 0.0f) c = ui::scaleRgb(c, fa);
    } else {
      // Hue from the attribute (the peak's own row: one hue along a reassigned line; or the column's brightness),
      // level as brightness: an empty texel comes out black either way.
      const float attr = tone_.source == 2 ? bright_[s] : (br >= 0 ? 1.0f - static_cast<float>(br) / static_cast<float>(kRows - 1) : 0.0f);
      c = ui::scaleRgb(ui::lutAt(*lut_, std::clamp(attr, 0.0f, 1.0f)), t * fa);
    }
    return ui::withAlpha(c, 255);
  }

  // PQ Meter's applyGate: below the knee (noise + threshold) down by depth · 60 dB over the knee width; above it the
  // level is stretched away from the knee (expand) with the ceiling mapping to itself.
  float gate(float db, float noise) const noexcept {
    if (tone_.depth <= 0.0f && tone_.expand == 1.0f) return db;
    const float knee = noise + tone_.thresh;
    if (db >= knee) {
      const float head = std::max(1.0f, tone_.ceilDb - knee);
      const float t = std::min(db - knee, head) / head;
      return knee + head * expandLut_[static_cast<size_t>(t * 4096.0f + 0.5f)];
    }
    const float t = std::clamp((knee - db) / std::max(1.0f, tone_.knee), 0.0f, 1.0f);
    return db - tone_.depth * 60.0f * t;
  }

  uint32_t cols_ = 0, texCols_ = 0, texRows_ = 0;
  std::vector<float> cacheDb_;
  std::vector<int16_t> cacheRow_;
  std::vector<uint32_t> mirror_, col_, staging_;
  std::vector<float> bright_;
  std::vector<uint8_t> filled_;  // the slot holds a column (else every texel is empty)
  std::vector<uint8_t> dirty_;
  std::vector<uint32_t> toneRev_;
  uint32_t rev_ = 1;
  bool allChanged_ = true;
  Tone tone_;
  std::vector<float> tilt_ = std::vector<float>(kRows, 0.0f), gammaLut_, expandLut_;
  const ui::ColorLut* lut_ = nullptr;
  std::vector<float> noise_, applied_, rowDb_, vals_, target_, nb_;
  std::vector<uint32_t> counts_, take_, changedRows_;
  std::vector<uint8_t> pending_, rowHit_;
  uint32_t rowCursor_ = 0;
  bool noiseValid_ = false, noiseSeeded_ = false, appliedValid_ = false;
  uint32_t sinceNoise_ = 0, phase_ = 0;
  std::vector<uint32_t> changed_;
};

// ---- the face --------------------------------------------------------------------------------------------------------

class SpectrogramFace final : public MetaFace {
public:
  explicit SpectrogramFace(const meta::Type& t) : MetaFace(t) {
    static uint32_t made = 0;  // UI thread: faces made so far (their gate estimates land on different columns)
    pic_.setPhase(made++ * 3u);
    visibleRows(opts_, panel_.tab, rows_);
    applyTone();
  }
  ~SpectrogramFace() override {
    letGo();
    dropTexture();
  }

  Vec2 worldSize() const noexcept override { return {kW, kH}; }
  float inputY(size_t, size_t) const noexcept override { return 0.5f * kH; }
  float jackInset() const noexcept override { return 0.0f; }  // TMM.13-fix2 (L23): on the frame's edge, off the plot
  float outputY(size_t, size_t) const noexcept override { return 0.5f * kH; }

  void setAnalysis(std::function<SpectrumFeed*()> feedOf, std::function<void()> wakeHost) override {
    feedOf_ = std::move(feedOf);
    wakeHost_ = std::move(wakeHost);
    syncFeed();
  }
  bool wantsAnalysis() const noexcept override { return true; }
  VoiceTapReader* voices() const noexcept override { return feedOf_ ? &frames_ : nullptr; }
  bool animating() const noexcept override { return frames_.live_; }

  bool pollVoices() override {
    bound_ = true;
    syncOptions();
    if (!syncFeed()) return true;  // no tap yet: one repaint (the host asks again at its next wake)
    const double now = clockNow();
    if (now - lastPaint_ > 0.5) {  // not painted lately: its host is hidden — after one unanswered repaint, let go
      if (probe_ == 0.0) {
        probe_ = now;
        return true;
      }
      if (lastPaint_ < probe_ && now - probe_ > 0.05) {
        letGo();
        dropTexture();
        frames_.live_ = false;
        probe_ = 0.0;
        return false;
      }
    }
    if (frozen_ || !acquired_) {
      frames_.live_ = geomPending_;  // a zoom that rested still gets its rebuild
      return geomPending_;
    }
    bool changed = ingest();
    changed = servo(now) || changed || geomPending_;
    frames_.live_ = !settled_ || moving_ || geomPending_;
    if (changed) ++frames_.serial_;
    return changed;
  }

  // ---- static layers: body; the plot's well; frame ----
  uint32_t staticLayerCount() const noexcept override { return 3; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.face.body : (layer == 1 ? th.device.display : th.face.frame);
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    if (layer == 0) p.roundedRect({1.0f, 1.0f, kW - 1.0f, kH - 1.0f}, kRadius, w);
    else if (layer == 1) p.roundedRect(kPlot, 4.0f, w);
    else outline(p, w);
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    const ui::Theme& th = *env.theme;
    syncOptions();
    syncFeed();
    lastPaint_ = clockNow();
    probe_ = 0.0;
    const ui::FaceTransform& t = p.transform();
    const float scale = t.scale;
    const Vec2 a = t.map({kPlot.x0, kPlot.y0}), b = t.map({kPlot.x1, kPlot.y1});
    const Rect r{a.x, a.y, b.x, b.y};
    // The geometry this paint needs: columns ≈ the plot's device px width (quantised, with hysteresis), texel rows ≥
    // its height (rounded up to 64; shrunk only when 128 rows too many).
    sizeFor(r.x1 - r.x0, r.y1 - r.y0);
    if (feed_ != nullptr && !acquired_) {
      acquire();
      if (!frozen_) (void)ingest();  // a display that just appeared shows what the feed holds at once
      if (wakeHost_) wakeHost_();
    }
    ui::TexturePool* pool = p.images();
    if (pool != nullptr && pool->attached()) {
      if (pool_.get() != pool) {
        dropTexture();
        pool_ = pool->share();
      }
      if (tex_ == 0 || texW_ != pic_.texCols() || texH_ != pic_.texRows()) makeTexture();
    }
    flush();
    ui::DrawList& dl = p.drawList();
    const bool labels = kLabelPx * scale >= kLabelMinDevicePx * env.uiScale;
    dl.pushClip(r);
    // TMM.13-fix1 L17: tex_ is a texture of pool_'s renderer; a painter without that pool (none, or another renderer's)
    // gets the ground only, never another renderer's texture id.
    paintPicture(dl, r, pool != nullptr && pool->attached() && pool_.get() == pool);
    dl.popClip();
    paintOverlays(p, labels && !coversPlot());
    if (labels) {
      chip(p, leftChip(kChChip), kChShort[std::clamp(opts_.choice(oChannel), 0, 3)], false, th);
      chip(p, leftChip(kScaleChip), kScaleShort[static_cast<int>(opts_.scale())], false, th);
      chip(p, leftChip(kMapChip), kMapShort[std::clamp(opts_.choice(oMap), 0, 9)], false, th);
      chip(p, kFreezeChip, "FREEZE", frozen_, th, frozen_ ? kFrozen : 0);
      chip(p, kSetChip, "SET", sheet_, th);
      if (coversPlot()) paintSheet(p, th);
    }
    for (const FaceJack& j : env.jacks) {  // leads in and out: the signal passes through unchanged
      if (!j.visible) continue;
      const float x0 = j.input ? 5.0f : kPlot.x1, x1 = j.input ? kPlot.x0 : kW - 5.0f;
      p.line({x0, j.at.y}, {x1, j.at.y}, 1.25f, j.connected ? th.face.signal : th.face.jackRing);
    }
    if (env.selected || (env.hovered && hovered_ == kFrame)) outline(p, env.selected ? th.circuit.selected : th.circuit.hover);
  }

  // (A side sheet is not live: it changes only through its own presses, which repaint the host.)
  void liveRects(std::vector<Rect>& out) const override { out.push_back({0.0f, 0.0f, kW, kH}); }

  // BR.ui: the sheet beside the face is a floating panel (the picture stays in view while its options change).
  std::optional<Rect> popupRect() const noexcept override {
    if (!sheet_ || !panel_.beside()) return std::nullopt;
    return panel_.area();
  }
  void paintPopup(ui::FacePainter& p, const FaceEnv& env) override {
    if (sheet_ && panel_.beside() && env.theme != nullptr) paintSheet(p, *env.theme);
  }
  void setPopupOverFace(bool on) override { hostOver_ = on; }

  int32_t hitTest(Vec2 l, float) const noexcept override {
    if (sheet_ && panel_.beside()) {
      const int32_t part = panel_.hit(l, rows_.size());
      if (part >= 0) return part == sheet::kBody ? kSheetPart : part;
    }
    if (l.x < 0.0f || l.y < 0.0f || l.x > kW || l.y > kH) return kNone;
    if (inside(leftChip(kChChip), l)) return kChPart;
    if (inside(leftChip(kScaleChip), l)) return kScalePart;
    if (inside(leftChip(kMapChip), l)) return kMapPart;
    if (inside(kFreezeChip, l)) return kFreezePart;
    if (inside(kSetChip, l)) return kSetPart;
    if (sheet_) {
      const int32_t part = panel_.hit(l, rows_.size());
      if (part >= 0) return part == sheet::kBody ? kSheetPart : part;
    }
    if (inside(kPlot, l)) return kPlotPart;
    return kFrame;
  }

  bool onPointer(const FacePointer& e) override {
    using K = FacePointer::Kind;
    const bool shift = (e.mods & ui::mod::kShift) != 0;
    switch (e.kind) {
      case K::Down: {
        if (e.button != ui::PointerButton::Left) return false;
        const int32_t part = hitTest(e.local, e.pxPerUnit);
        if (part == kChPart || part == kScalePart || part == kMapPart) {
          const uint32_t o = part == kChPart ? oChannel : part == kScalePart ? oScale : oMap;
          setOption(o, sheet::cycled(opts_, o, shift ? -1 : 1));
          return true;
        }
        if (part == kFreezePart) {
          frozen_ = !frozen_;
          if (!frozen_) {
            servoPrev_ = -1.0;  // re-lock instead of measuring the frozen span as one frame's motion
            (void)ingest();
            if (wakeHost_) wakeHost_();
          }
          return true;
        }
        if (part == kSetPart) {
          sheet_ = !sheet_;
          if (sheet_)
            panel_.place(sheet::choosePlace(e.device.x - e.local.x * e.pxPerUnit, e.pxPerUnit, kW, kW, e.room.x0, e.room.x1, hostOver_), kW, kH);
          return true;
        }
        if (part >= kRowBase) return rowDown(static_cast<uint32_t>(part - kRowBase), e);
        if (part == sheet::kTabBase || part == sheet::kTabBase + 1) {
          panel_.tab = part - sheet::kTabBase;
          visibleRows(opts_, panel_.tab, rows_);
          return true;
        }
        if (part == sheet::kSave) {
          meta::saveUserDefaultData(type().id, opts_.data());
          return true;
        }
        if (part == sheet::kReset) {
          resetOptions();
          return true;
        }
        if (part == kSheetPart) return true;
        return false;  // the plot: the host's (select, move)
      }
      case K::Move: {
        if (drag_) {
          const Opt& op = kOpts[dragOpt_];
          const double k = (shift ? 0.1 : 1.0) / 160.0;
          setOption(dragOpt_, sheet::fromTravel(op, std::clamp(pressTravel_ + (pressY_ - e.device.y) * k, 0.0, 1.0)));
          return true;
        }
        const Vec2 h = inside(kPlot, e.local) && !coversPlot() && hitTest(e.local, e.pxPerUnit) == kPlotPart ? e.local : Vec2{-1.0f, -1.0f};
        if (h.x == hover_.x && h.y == hover_.y) return false;
        hover_ = h;
        return true;
      }
      case K::Up: {
        const bool was = drag_;
        drag_ = false;
        if (was && editing_) {
          target().end();
          editing_ = false;
        }
        return was;
      }
      case K::Wheel: return false;
      case K::Leave:
        if (hover_.x < 0.0f) return false;
        hover_ = {-1.0f, -1.0f};
        return true;
    }
    return false;
  }
  bool dragging() const noexcept override { return drag_; }
  void cancelDrag() override {
    if (drag_ && editing_) {
      target().cancel();
      editing_ = false;
      syncOptions(true);
    }
    drag_ = false;
  }
  bool onKey(const ui::KeyEvent& e) override {
    if (!e.down || e.key != ui::key::kEscape || !sheet_) return false;
    sheet_ = false;
    return true;
  }
  ui::Cursor cursor(int32_t part) const noexcept override {
    if (part == kChPart || part == kScalePart || part == kMapPart || part == kFreezePart || part == kSetPart) return ui::Cursor::Pointer;
    if (part >= kRowBase) {
      const auto i = static_cast<size_t>(part - kRowBase);
      if (i < rows_.size() && sheet::numeric(kOpts[rows_[i]])) return ui::Cursor::ResizeNS;
      return ui::Cursor::Pointer;
    }
    if (part >= sheet::kTabBase && part <= sheet::kReset) return ui::Cursor::Pointer;
    return ui::Cursor::Inherit;
  }

  void describe(int32_t part, std::string& title, std::string& body) const override {
    switch (part) {
      case kChPart:
        title = "Channel";
        body = "Click to cycle (Shift: back): MID (L + R) / 2, L, R, SIDE (L − R) / 2 — what cancels in a mono fold: reverb "
               "tails and wideners light up, centred sound vanishes.";
        return;
      case kScalePart:
        title = "Frequency scale";
        body = "Click to cycle: LOG (every octave the same height; from 0 Hz linear below 40 Hz), MEL (near-linear below "
               "1 kHz), LIN (the FFT's own spacing). The picture starts over.";
        return;
      case kMapPart:
        title = "Colormap";
        body = "Click to cycle the ten maps (Shift: back): Magma, Inferno, Viridis, Grayscale, Ice, Gold, Silver, Pastel, "
               "Neon, Copper. The picture is recoloured, not recomputed.";
        return;
      case kFreezePart:
        title = "Freeze";
        body = frozen_ ? "Frozen: the picture holds (the signal still passes). Click to follow again."
                       : "Click to hold the picture (the signal still passes).";
        return;
      case kSetPart:
        title = "Settings";
        body = sheet_ ? "Click to close the settings."
                      : "Click for every option: span, window, reassignment, playhead, level mapping, visual gate, range, "
                        "colours, grid, ruler, keyboard, the track tuning's degrees.";
        return;
      case kPlotPart:
        title = "Spectrogram";
        body = feed_ != nullptr
                   ? "The signal passing through, as it sounds over time (it is not changed): reassigned, so partials are "
                     "thin lines and transients are sharp. Point at it for the frequency and its nearest note."
                   : "The signal passing through, over time (it is not changed). Shown when the module plays in a track's "
                     "chain.";
        return;
      case sheet::kTabBase:
        title = "Analysis";
        body = "The options of what is analysed: span, window, channel, reassignment, playhead, frequency axis.";
        return;
      case sheet::kTabBase + 1:
        title = "Display";
        body = "Only how it looks: level range, gamma, fade, tilt, the visual gate, colours, grid, ruler, keyboard.";
        return;
      case sheet::kSave:
        title = "Save as default";
        body = "New Spectrograms start with these options (every tab). Saved with pqdaw's settings; existing ones keep theirs.";
        return;
      case sheet::kReset:
        title = "Reset";
        body = "Every option back to the factory default.";
        return;
      default:
        if (part >= kRowBase && static_cast<size_t>(part - kRowBase) < rows_.size()) {
          const Opt& op = kOpts[rows_[static_cast<size_t>(part - kRowBase)]];
          title = op.label;
          body = sheet::numeric(op)
                     ? "Drag up / down to set (Shift: fine); double-click resets."
                     : "Click to change (Shift: back); double-click resets.";
          return;
        }
        MetaFace::describe(part, title, body);
    }
  }

  // ---- tests ----
  void shown(GramShown& out) const {
    out.acquired = acquired_;
    out.frozen = frozen_;
    out.texture = tex_ != 0;
    out.cols = pic_.cols();
    out.texCols = pic_.texCols();
    out.texRows = pic_.texRows();
    out.read = lastRead_;
    out.display = disp_;
    out.hop = state_.hop;
    out.mapping = state_.mapping;
    out.loop = opts_.loop();
    out.loopHead = loopHead_;
    out.requests = requests_;
    out.request = sent_;
    out.uploads = uploads_;
    out.uploadBytes = uploadBytes_;
    out.rebuilds = rebuilds_;
  }
  bool texel(uint64_t column, uint32_t y, Rgba& rgba, float& db, int& row) const {
    if (!pic_.sized() || y >= pic_.texRows() || column >= lastRead_ || lastRead_ - column > ringCols()) return false;
    const auto s = static_cast<uint32_t>(column % pic_.texCols());
    rgba = pic_.texel(s, y);
    row = pic_.sourceRow(s, y);
    db = pic_.levelDb(s, y);
    return true;
  }
  bool slotTexel(uint32_t slot, uint32_t y, Rgba& rgba) const {
    if (!pic_.sized() || y >= pic_.texRows() || slot >= pic_.texCols()) return false;
    rgba = pic_.texel(slot, y);
    return true;
  }
  float noiseDb(uint32_t y) const { return pic_.noiseDb(y); }
  void setOptionForTest(std::string_view key, double v) {
    for (uint32_t i = 0; i < oCount; ++i)
      if (key == kOpts[i].key) setOption(i, v);
  }
  void setFrozenForTest(bool f) { frozen_ = f; }
  void setSheetForTest(bool s) { sheet_ = s; }
  void setHoverForTest(Vec2 l) { hover_ = l; }

private:
  // ---- options ----
  void syncOptions(bool force = false) {
    const meta::DataPtr d = bound_ ? target().data() : nullptr;
    if (!force && d == data_ && optionsLoaded_) return;
    data_ = d;
    optionsLoaded_ = true;
    if (!local_) opts_.load(d.get());
    applyOptions();
  }
  void applyOptions() {
    visibleRows(opts_, panel_.tab, rows_);
    const bool loop = opts_.loop();
    if (loop != wasLoop_) {  // the columns are addressed another way: start over
      wasLoop_ = loop;
      pic_.clear();
      loopHead_ = -1;
    }
    applyTone();
    sendRequest();
  }
  void applyTone() {
    Tone t;
    t.floorDb = opts_.floorDb();
    t.ceilDb = opts_.ceilDb();
    t.gamma = static_cast<float>(opts_[oGamma]);
    t.fade = static_cast<float>(opts_[oFade]);
    t.tilt = static_cast<float>(opts_[oTilt]);
    t.gate = opts_.on(oGate);
    t.depth = static_cast<float>(opts_[oDepth]);
    t.thresh = static_cast<float>(opts_[oThresh]);
    t.knee = static_cast<float>(opts_[oKnee]);
    t.expand = static_cast<float>(opts_[oExpand]);
    t.map = opts_.choice(oMap);
    t.source = opts_.choice(oSource);
    t.scale = opts_.scale();
    // The axis the analysis maps (its fMax is clamped to the Nyquist frequency): the rows' frequencies.
    t.fMin = state_.running ? state_.fMin : opts_.fMin();
    t.fMax = state_.running ? state_.fMax : opts_.fMax();
    pic_.setTone(t);
  }
  void setOption(uint32_t o, double v) {
    opts_.set(o, v);
    if (bound_ && target().editable()) {
      data_ = opts_.data();
      target().setData(data_);
      data_ = target().data();
    } else {
      local_ = true;
    }
    applyOptions();
    if (wakeHost_) wakeHost_();
  }
  void resetOptions() {
    opts_.reset();
    if (bound_ && target().editable()) {
      target().setData(opts_.data());
      data_ = target().data();
    } else {
      local_ = true;
    }
    applyOptions();
  }
  bool rowDown(uint32_t row, const FacePointer& e) {
    if (row >= rows_.size()) return false;
    const uint32_t o = rows_[row];
    const Opt& op = kOpts[o];
    if (e.clicks >= 2) {
      setOption(o, op.def);
      return true;
    }
    if (op.kind == Opt::Choice || op.kind == Opt::Toggle) {
      setOption(o, sheet::cycled(opts_, o, (e.mods & ui::mod::kShift) ? -1 : 1));
      return true;
    }
    drag_ = true;
    dragOpt_ = o;
    pressY_ = e.device.y;
    pressTravel_ = sheet::toTravel(op, opts_[o]);
    if (bound_ && target().editable()) {
      target().begin(op.label);
      editing_ = true;
    }
    return true;
  }

  // ---- the feed ----
  bool syncFeed() {
    SpectrumFeed* f = feedOf_ ? feedOf_() : nullptr;
    if (f != feed_) {
      letGo();
      feed_ = f;
      lastRead_ = 0;
      state_ = {};
      pic_.clear();
      disp_ = servoPrev_ = -1.0;
    }
    return feed_ != nullptr;
  }
  void acquire() {
    if (acquired_ || feed_ == nullptr) return;
    feed_->acquire(devices::kWantSpectrogram);
    feed_->addWaker(this, [this] {
      frames_.live_ = true;
      if (wakeHost_) wakeHost_();
    });
    acquired_ = true;
    sent_ = {};
    sendRequest();
  }
  void letGo() {
    if (!acquired_) return;
    feed_->removeWaker(this);
    feed_->clearSpectrogram(this);
    feed_->release(devices::kWantSpectrogram);
    acquired_ = false;
  }
  // What the analysis must compute for this display: the options, the hop that puts the span (Loop: the beat span at the
  // tempo the columns show) across the visible columns.
  // The columns the span covers at the hop in force (the feed's: a display of the same instance with more columns may
  // have decided it), at most the ring's: what scroll mode shows across the plot.
  uint32_t shownCols() const noexcept {
    if (opts_.loop() || !state_.running || state_.hop == 0 || feed_ == nullptr) return pic_.cols();  // Loop: one slot per column
    const double n = opts_[oSpan] * feed_->config().sampleRate / static_cast<double>(state_.hop);
    return static_cast<uint32_t>(std::clamp<long>(std::lround(n), 16L, static_cast<long>(kMaxCols)));
  }
  uint32_t ringCols() const noexcept { return pic_.texCols() > kSpare ? pic_.texCols() - kSpare : 0u; }
  uint32_t drawCols() const noexcept { return std::min(shownCols(), ringCols()); }
  void sendRequest() {
    if (!acquired_ || feed_ == nullptr || pic_.cols() == 0) return;
    const devices::SpectrogramRequest q = requestFor(opts_, pic_.cols(), feed_->config().sampleRate, beatRate_);
    if (q == sent_) return;
    sent_ = q;
    ++requests_;
    feed_->setSpectrogram(this, q);
  }
  // VZ.1 (PQ Meter f3cacd6's rule: never a whole-picture repaint in one frame of a gesture): a new geometry rebuilds the
  // picture — a new texture, the ring re-read, reduced, recoloured and uploaded, and (columns) a new analysis hop. While
  // the plot's device size moves (a zoom, a resize) the picture is stretched instead; the rebuild waits until the size
  // has rested kSettle seconds (the face keeps asking for frames meanwhile: geomPending_).
  void sizeFor(float wPx, float hPx) {
    constexpr double kSettle = 0.2;
    if (pic_.sized()) {
      const double now = clockNow();
      if (std::fabs(wPx - lastW_) > 0.5f || std::fabs(hPx - lastH_) > 0.5f) {
        lastW_ = wPx;
        lastH_ = hPx;
        sizeMoved_ = now;
      }
      geomPending_ = false;
      if (now - sizeMoved_ < kSettle) {
        // Still moving: decide once it rests (nothing pending when the geometry would not change anyway).
        geomPending_ = wantGeometry(wPx, hPx) != std::pair{pic_.cols(), pic_.texRows()};
        return;
      }
    } else {
      lastW_ = wPx;
      lastH_ = hPx;
    }
    const auto [cols, rows] = wantGeometry(wPx, hPx);
    if (pic_.resize(cols, rows, std::max(cols, shownCols()))) {
      ++rebuilds_;
      loopHead_ = -1;
      // Re-read what the feed still holds at the new geometry.
      lastRead_ = 0;
      disp_ = servoPrev_ = -1.0;
      sendRequest();
    }
  }
  // The geometry a plot of wPx × hPx device px wants from the current one (with the hysteresis).
  std::pair<uint32_t, uint32_t> wantGeometry(float wPx, float hPx) const noexcept {
    uint32_t cols = pic_.cols();
    // Columns follow the width in 32-column steps, only when it moved by more than 12 % (a zoom does not re-analyse).
    if (cols == 0 || std::fabs(std::clamp(wPx, static_cast<float>(kMinCols), static_cast<float>(kMaxCols)) - static_cast<float>(cols)) >
                         0.12f * static_cast<float>(cols))
      cols = columnsFor(wPx);
    const auto need = static_cast<uint32_t>(std::clamp(std::lround(hPx), 16L, static_cast<long>(kRows)));
    uint32_t rows = pic_.texRows();
    if (rows == 0 || need > rows || need + 2 * kRowGranule <= rows) rows = std::min(kRows, (need + kRowGranule - 1) / kRowGranule * kRowGranule);
    return {cols, rows};
  }

  // Reads the columns not seen yet into the picture (UI thread, at most once per displayed frame). True: it changed.
  bool ingest() {
    if (feed_ == nullptr || !pic_.sized()) return false;
    const devices::GramState st = feed_->gramState();
    settled_ = st.settled;
    const bool axisChanged = st.running && (!state_.running || st.fMin != state_.fMin || st.fMax != state_.fMax);
    if (st.running && state_.running && st.mapping != state_.mapping) {
      pic_.clear();  // the rows map other frequencies: start over
      loopHead_ = -1;
    }
    state_ = st;
    if (axisChanged) applyTone();
    if (!opts_.loop() && shownCols() > ringCols() && pic_.sized()) {
      // The hop in force needs more columns than the ring holds: grow it and read again what the feed holds.
      (void)pic_.resize(pic_.cols(), pic_.texRows(), shownCols());
      lastRead_ = 0;
      disp_ = servoPrev_ = -1.0;
    }
    if (!st.running || st.end == lastRead_) return false;
    const uint64_t texCols = pic_.texCols();
    uint64_t from = std::max({lastRead_, st.oldest, st.end > texCols - 2 ? st.end - (texCols - 2) : uint64_t{0}});
    bool any = false;
    const bool loop = opts_.loop();
    const double spanBeats = kBeatSpans[std::clamp(opts_.choice(oLoopSpan), 0, 4)];
    constexpr uint32_t kBatch = 16;
    buf_.resize(static_cast<size_t>(kBatch) * kRows);
    info_.resize(kBatch);
    while (from < st.end) {
      const auto want = static_cast<uint32_t>(std::min<uint64_t>(kBatch, st.end - from));
      const uint32_t got = feed_->gramRead(from, want, buf_.data(), info_.data());
      if (got == 0) break;
      for (uint32_t i = 0; i < got; ++i) {
        const float* rows = buf_.data() + static_cast<size_t>(i) * kRows;
        const devices::GramColumn& ci = info_[i];
        trackBeat(ci);
        if (loop) placeLoop(rows, ci, spanBeats);
        else {
          pic_.put(static_cast<uint32_t>((from + i) % texCols), rows);
          pic_.put(static_cast<uint32_t>((from + i + 1) % texCols), nullptr);  // the next slot shows nothing yet
        }
        pic_.columnAdded();
      }
      from += got;
      any = true;
    }
    lastRead_ = st.end;
    if (loop) sendRequest();  // the tempo may have moved the hop
    return any;
  }
  void trackBeat(const devices::GramColumn& ci) {
    // The tempo the columns show, in beats per analysis sample (from consecutive columns' tapped beats).
    if (std::isfinite(ci.beat) && std::isfinite(lastBeat_) && ci.centre > lastCentre_) {
      const double rate = (static_cast<double>(ci.beat) - lastBeat_) / (ci.centre - lastCentre_);
      if (rate > 1e-7 && rate < 1e-2) {
        if (beatRate_ <= 0.0 || std::fabs(rate - beatRate_) > 0.03 * beatRate_) beatRate_ = rate;  // moved: follow at once
        else beatRate_ += (rate - beatRate_) * 0.05;
      }
    }
    lastBeat_ = ci.beat;
    lastCentre_ = ci.centre;
  }
  // Loop: a column lands on the slot of its beat's phase in the span; a small forward jump fills the slots between.
  void placeLoop(const float* rows, const devices::GramColumn& ci, double spanBeats) {
    if (!std::isfinite(ci.beat)) return;  // no clock: nothing to place by
    const uint32_t cols = pic_.cols();
    double ph = std::fmod(static_cast<double>(ci.beat), spanBeats);
    if (ph < 0.0) ph += spanBeats;
    const auto slot = static_cast<int64_t>(std::min<double>(static_cast<double>(cols) - 1.0, std::floor(ph / spanBeats * cols)));
    pic_.put(static_cast<uint32_t>(slot), rows);
    if (loopHead_ >= 0) {
      const auto n = static_cast<int64_t>(cols);
      const int64_t d = ((slot - loopHead_) % n + n) % n;
      if (d > 1 && d <= n / 2)
        for (int64_t k = 1; k < d; ++k) pic_.copySlot(static_cast<uint32_t>(slot), static_cast<uint32_t>((loopHead_ + k) % cols));
    }
    loopHead_ = slot;
    loopPhase_ = ph / spanBeats;
  }

  // The smooth scroll (PQ Meter's servo): the drawn position advances at a smoothed estimate of the column rate, leans
  // one column short of the measured position and never passes it. True: it moved.
  bool servo(double now) {
    if (opts_.loop() || !state_.running) {
      moving_ = false;
      return false;
    }
    const double target = std::min(state_.position, static_cast<double>(lastRead_) + 1.0);
    const double dt = std::clamp(lastServo_ > 0.0 ? now - lastServo_ : 1.0 / 60.0, 1.0 / 1000.0, 0.1);
    lastServo_ = now;
    const double before = disp_;
    const double sr = feed_ != nullptr ? feed_->config().sampleRate : 48000.0;
    // PQ Meter eases across a jump of up to 0.25 s; here at most kMaxLag columns (the ring keeps kSpare spare slots).
    const double snapAhead = std::min(kMaxLag, std::max(6.0, 0.25 * sr / std::max<double>(1.0, state_.hop)));
    if (disp_ < 0.0 || servoPrev_ < 0.0 || target < disp_ - 1.0 || target > disp_ + snapAhead) {
      disp_ = target;
      vel_ = 0.0;
    } else {
      const double vMeas = std::max(0.0, (target - servoPrev_) / dt);
      vel_ += (vMeas - vel_) * (1.0 - std::exp(-dt / 0.25));
      double next = disp_ + vel_ * dt;
      next += (target - 1.0 - next) * (1.0 - std::exp(-dt / 0.12));
      disp_ = std::clamp(next, disp_, target);
    }
    servoPrev_ = target;
    moving_ = disp_ != before && vel_ > 1e-3;
    return disp_ != before;
  }

  // Colours what changed and uploads it (once per paint; the first paint after a new texture uploads everything).
  void flush() {
    if (!pic_.sized()) return;
    if (pic_.noiseDue()) {
      visibleSlots();
      pic_.estimateNoise(slots_);
    }
    const std::vector<uint32_t>& changed = pic_.colour();
    const std::vector<uint32_t>& rows = pic_.colourMovedRows();
    if ((changed.empty() && rows.empty()) || tex_ == 0 || !pool_ || !pool_->attached()) return;
    const uint64_t before = pool_->bytesUpdated();
    pic_.upload(*pool_, tex_, changed);
    pic_.uploadRows(*pool_, tex_, rows);
    uploadBytes_ += pool_->bytesUpdated() - before;
    ++uploads_;
  }
  void visibleSlots() {
    slots_.clear();
    const uint32_t texCols = pic_.texCols();
    if (opts_.loop()) {
      for (uint32_t s = 0; s < pic_.cols(); ++s) slots_.push_back(s);
      return;
    }
    const uint32_t cols = drawCols();
    const uint64_t end = lastRead_;
    for (uint64_t c = end > cols ? end - cols : 0; c < end; ++c) slots_.push_back(static_cast<uint32_t>(c % texCols));
  }
  void makeTexture() {
    if (!pool_ || !pool_->attached() || !pic_.sized()) return;
    if (tex_ != 0) pool_->destroy(tex_);
    tex_ = 0;
    auto t = pool_->create(pic_.texCols(), pic_.texRows(), ui::TextureFilter::Linear);
    if (!t) return;
    tex_ = *t;
    texW_ = pic_.texCols();
    texH_ = pic_.texRows();
    pic_.markAllChanged();
  }
  void dropTexture() {
    if (tex_ != 0 && pool_) pool_->destroy(tex_);
    tex_ = 0;
  }

  // ---- painting ----
  void paintPicture(ui::DrawList& dl, const Rect& r, bool texHere) {
    if (!pic_.sized() || !state_.running) return;
    dl.addRectFilled(r, ui::lutAt(ui::colormapLut(static_cast<ui::Colormap>(std::clamp(opts_.choice(oMap), 0, 9))), 0.0f));  // the ground: silence
    if (tex_ == 0 || !texHere) return;
    const uint32_t texCols = pic_.texCols();
    const float tw = static_cast<float>(texCols);
    if (opts_.loop()) {
      dl.addImage(tex_, r, {0.0f, 0.0f}, {static_cast<float>(pic_.cols()) / tw, 1.0f});
      return;
    }
    const uint32_t cols = drawCols();
    const float w = r.x1 - r.x0, px = w / static_cast<float>(cols);
    const double d = disp_ >= 0.0 ? disp_ : static_cast<double>(lastRead_);
    const auto c1 = static_cast<int64_t>(std::min<double>(std::ceil(d) - 1.0, static_cast<double>(lastRead_)));  // newest drawn
    const int64_t c0 = std::max<int64_t>(0, static_cast<int64_t>(std::floor(d)) - static_cast<int64_t>(cols) - 1);
    if (c1 < c0) return;
    auto bx = [&](double c) { return r.x1 - static_cast<float>((d - c) * px); };
    // Up to two runs (the ring wraps).
    int64_t c = c0;
    while (c <= c1) {
      const auto s = static_cast<uint32_t>(c % texCols);
      const int64_t run = std::min<int64_t>(c1 - c + 1, static_cast<int64_t>(texCols - s));
      dl.addImage(tex_, {bx(static_cast<double>(c)), r.y0, bx(static_cast<double>(c + run)), r.y1},
                  {static_cast<float>(s) / tw, 0.0f}, {static_cast<float>(s + run) / tw, 1.0f});
      c += run;
    }
  }

  float yOf(float hz) const noexcept {
    const Tone& t = pic_.tone();
    const float u = dsp::gramUnit(t.scale, std::clamp(hz, t.fMin, t.fMax), t.fMin, t.fMax);
    return kPlot.y1 - u * (kPlot.y1 - kPlot.y0);
  }
  float hzAt(float y) const noexcept {
    const Tone& t = pic_.tone();
    return dsp::gramHz(t.scale, (kPlot.y1 - y) / (kPlot.y1 - kPlot.y0), t.fMin, t.fMax);
  }

  void paintOverlays(ui::FacePainter& p, bool labels) {
    const Tone& tone = pic_.tone();
    const float px1 = 1.0f / std::max(1e-3f, p.transform().scale);  // one device px in world units
    const float keysW = opts_.keysW();
    const bool keysLeft = opts_.choice(oKeys) == 1;
    // The frequency rules and labels: the decade ladder on log / mel (on a short plot only 100, 1k, 5k labelled),
    // round steps on linear.
    if (opts_.on(oGrid)) {
      float f[16];
      char lab[16][8];
      int n = 0;
      if (tone.scale == GramScale::Linear) {
        const float span = std::max(1.0f, tone.fMax - tone.fMin);
        static const float kSteps[] = {100, 200, 500, 1000, 2000, 5000, 10000};
        float step = kSteps[6];
        for (const float s : kSteps)
          if (span / s <= 9.0f) {
            step = s;
            break;
          }
        for (float x = std::ceil(tone.fMin / step) * step; x <= tone.fMax && n < 16; x += step) {
          f[n] = x;
          if (x >= 1000.0f) std::snprintf(lab[n], 8, "%gk", x / 1000.0f);
          else std::snprintf(lab[n], 8, "%g", x);
          ++n;
        }
      } else {
        static const float kDec[] = {10, 20, 30, 50, 100, 200, 300, 500, 1000, 2000, 3000, 5000, 10000, 15000, 20000};
        static const char* const kDecL[] = {"10", "20", "30", "50", "100", "200", "300", "500", "1k", "2k", "3k", "5k", "10k", "15k", "20k"};
        for (int i = 0; i < 15; ++i) {
          f[n] = kDec[i];
          std::snprintf(lab[n], 8, "%s", kDecL[i]);
          ++n;
        }
      }
      bool sparse = false;
      if (tone.scale != GramScale::Linear) {
        float prev = -1e9f;
        for (int i = n - 1; i >= 0; --i) {
          if (f[i] < tone.fMin * 1.02f || f[i] > tone.fMax * 0.98f) continue;
          const float y = yOf(f[i]);
          if (y - prev < kLabelPx + 2.0f) {
            sparse = true;
            break;
          }
          prev = y;
        }
      }
      for (int i = 0; i < n; ++i) {
        if (f[i] < tone.fMin * 1.02f || f[i] > tone.fMax * 0.98f) continue;
        const float y = yOf(f[i]);
        p.line({kPlot.x0, y}, {kPlot.x1, y}, px1, kGrid);
        if (!labels) continue;
        if (sparse && f[i] != 100.0f && f[i] != 1000.0f && f[i] != 5000.0f) continue;
        float ty = y - 0.5f * kLabelPx - 1.5f;
        if (ty - 0.5f * kLabelPx < kChipY + kChipH + 1.0f) {
          if (!sparse) continue;
          ty = y + 0.5f * kLabelPx + 1.5f;
        }
        if (ty + 0.5f * kLabelPx > kPlot.y1 - 1.0f) continue;
        // On a backing pad: a label over a bright line stays legible (the spectrum display's labels do the same).
        const float lx = kPlot.x0 + 3.0f + (keysLeft ? keysW : 0.0f);
        const float lw = 0.55f * kLabelPx * static_cast<float>(std::strlen(lab[i]));
        p.roundedRect({lx - 1.5f, ty - 0.5f * kLabelPx - 1.0f, lx + lw + 1.5f, ty + 0.5f * kLabelPx + 1.0f}, 2.0f, kPad);
        p.textAligned({lx, ty}, lab[i], kLabelPx, ui::rgba(255, 255, 255, 120), 0.0f);
      }
    }
    // The track tuning's degrees (pqdaw's extra): a faint rule per degree, the equave's (degree 0) brighter; degrees
    // closer than 3 device px to the last drawn are skipped (a 53-EDO far out shows its equaves).
    if (opts_.on(oTuning) && bound_) {
      const uint64_t key = target().guidesKey() ^ (static_cast<uint64_t>(std::bit_cast<uint32_t>(tone.fMax)) << 1) ^
                           std::bit_cast<uint32_t>(tone.fMin);
      if (key != guidesKey_ || !guidesValid_) {
        target().pitchGuides(std::max(tone.fMin, 8.0f), tone.fMax, guides_);
        guidesKey_ = key;
        guidesValid_ = true;
      }
      float last = 1e9f;
      for (const PitchGuide& g : guides_) {
        const float y = yOf(g.hz);
        const bool equave = g.degree == 0;
        if (!equave && std::fabs(last - y) < 3.0f * px1) continue;
        p.line({kPlot.x0 + (keysLeft ? keysW : 0.0f), y}, {kPlot.x1 - (opts_.choice(oKeys) == 2 ? keysW : 0.0f), y}, px1,
               equave ? ui::rgba(150, 230, 255, 70) : ui::rgba(150, 230, 255, 30));
        last = y;
      }
    }
    // The time axis: Scroll, the seconds ruler along the bottom; Loop, the span's beat rules (bars brighter) and the
    // write head.
    const float plotW = kPlot.x1 - kPlot.x0;
    if (opts_.loop()) {
      if (opts_.on(oRuler)) {
        const int beats = static_cast<int>(kBeatSpans[std::clamp(opts_.choice(oLoopSpan), 0, 4)]);
        for (int bt = 1; bt < beats; ++bt) {
          const float x = kPlot.x0 + static_cast<float>(bt) / static_cast<float>(beats) * plotW;
          const bool bar = beats >= 4 && bt % 4 == 0;
          p.line({x, kPlot.y0}, {x, kPlot.y1}, px1, ui::rgba(255, 255, 255, bar ? 55 : 26));
        }
      }
      if (loopHead_ >= 0) {
        const float x = kPlot.x0 + static_cast<float>(loopPhase_) * plotW;
        p.line({x, kPlot.y0}, {x, kPlot.y1}, px1, ui::rgba(255, 255, 255, frozen_ || settled_ ? 70 : 190));
      }
    } else if (opts_.on(oRuler) && state_.running && state_.hop > 0 && labels) {
      const double sr = feed_ != nullptr ? feed_->config().sampleRate : 48000.0;
      const double secPerCol = static_cast<double>(state_.hop) / sr;
      const double spanSec = secPerCol * static_cast<double>(drawCols());
      double step = 1.0;
      while (spanSec / step > 8.0) step *= 2.0;
      while (spanSec / step < 3.0 && step > 0.05) step *= 0.5;
      const float colW = plotW / static_cast<float>(std::max<uint32_t>(1, drawCols()));
      const float baseY = kPlot.y1 - 1.5f, textY = kPlot.y1 - 2.5f - 0.5f * kLabelPx - 3.0f;
      float claimed = kPlot.x1;
      char buf[16];
      for (double s = step; s < spanSec; s += step) {
        const float x = kPlot.x1 - static_cast<float>(s / secPerCol) * colW;
        if (x < kPlot.x0) break;
        p.line({x, baseY - 4.0f}, {x, baseY}, px1, kGridLabel);
        std::snprintf(buf, sizeof buf, "-%.2gs", s);
        const float tw = 0.55f * kLabelPx * static_cast<float>(std::strlen(buf));
        if (x + 2.0f + tw + 3.0f > claimed || x + 2.0f + tw > kPlot.x1) continue;
        claimed = x + 2.0f;
        p.roundedRect({x + 0.5f, textY - 0.5f * kLabelPx - 1.0f, x + 3.5f + tw, textY + 0.5f * kLabelPx + 1.0f}, 2.0f, kPad);
        p.textAligned({x + 2.0f, textY}, buf, kLabelPx, ui::rgba(255, 255, 255, 120), 0.0f);
      }
    }
    // The keyboard (12-EDO keys at their pitch on this axis; the hovered key lit).
    const bool hovering = hover_.x >= 0.0f;
    const float hz = hovering ? hzAt(hover_.y) : 0.0f;
    if (opts_.choice(oKeys) != 0) paintKeys(p, keysLeft, hovering ? hz : 0.0f, labels);
    // Hover: the line, its frequency, nearest note and (with the scale lines) the nearest degree.
    if (hovering) {
      p.line({kPlot.x0, hover_.y}, {kPlot.x1, hover_.y}, px1, ui::rgba(255, 255, 255, 60));
      if (labels) {
        char buf[64];
        if (hz >= 1000.0f) std::snprintf(buf, sizeof buf, "%.2f kHz", hz / 1000.0f);
        else std::snprintf(buf, sizeof buf, "%.1f Hz", hz);
        const bool below = hover_.y - 2.0f * kLabelPx - 4.0f < kPlot.y0;
        const float y0 = below ? hover_.y + 0.5f * kLabelPx + 3.0f : hover_.y - 1.5f * kLabelPx - 4.0f;
        const float x0 = std::min(hover_.x + 8.0f, kPlot.x1 - 60.0f);
        int lines = 1;
        char note[32] = "", deg[48] = "";
        if (opts_.on(oNote)) {
          noteName(hz, note, sizeof note);
          if (note[0] != '\0') ++lines;
        }
        if (opts_.on(oTuning) && !guides_.empty()) {
          const auto it = std::min_element(guides_.begin(), guides_.end(), [&](const PitchGuide& a, const PitchGuide& b) {
            return std::fabs(std::log2(a.hz / hz)) < std::fabs(std::log2(b.hz / hz));
          });
          const int cents = static_cast<int>(std::lround(1200.0 * std::log2(hz / it->hz)));
          std::snprintf(deg, sizeof deg, "deg %d.%d %+d", it->equave, it->degree, cents);
          ++lines;
        }
        const float lh = kLabelPx + 2.0f;
        p.roundedRect({x0 - 2.0f, y0 - 0.5f * lh, x0 + 56.0f, y0 - 0.5f * lh + lh * static_cast<float>(lines)}, 2.0f, kPad);
        p.textAligned({x0, y0}, buf, kLabelPx, ui::rgba(255, 255, 255, 210), 0.0f);
        float y = y0 + lh;
        if (note[0] != '\0') {
          p.textAligned({x0, y}, note, kLabelPx, ui::rgba(160, 220, 255, 200), 0.0f);
          y += lh;
        }
        if (deg[0] != '\0') p.textAligned({x0, y}, deg, kLabelPx, ui::rgba(150, 230, 255, 200), 0.0f);
      }
    }
    if (feed_ != nullptr && acquired_ && state_.running && lastRead_ == 0 && labels)
      p.textAligned({kPlot.x0 + 6.0f, kChipY + kChipH + 8.0f}, "waiting for audio", kLabelPx, kGridLabel, 0.0f);
  }

  // PQ Meter's drawKeyboard: every MIDI key at its true pitch on the axis (white keys tile it between the black keys'
  // centres), C keys tinted green, the hovered key lit, C labels where they fit.
  void paintKeys(ui::FacePainter& p, bool left, float hoverHz, bool labels) const {
    const float back = left ? kPlot.x0 : kPlot.x1, dir = left ? 1.0f : -1.0f;
    const float kw = opts_.keysW();
    const float front = back + dir * kw, blackEnd = back + dir * kw * 0.6f;
    const float xw0 = std::min(back, front), xw1 = std::max(back, front);
    const float xb0 = std::min(back, blackEnd), xb1 = std::max(back, blackEnd);
    const float px1 = 1.0f / std::max(1e-3f, p.transform().scale);
    auto yOfMidi = [&](float m) { return yOf(440.0f * std::exp2((m - 69.0f) / 12.0f)); };
    auto isBlack = [](int pc) { return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; };
    const int hoverMidi = hoverHz > 0.0f ? static_cast<int>(std::lround(69.0 + 12.0 * std::log2(hoverHz / 440.0))) : -1;
    const auto kA = static_cast<float>(opts_[oKeysOpacity]);  // PQ Meter's keyboard opacity (0.85)
    auto col = [kA](uint32_t r, uint32_t g, uint32_t b, float a) { return ui::rgba(r, g, b, static_cast<uint32_t>(a * kA * 255.0f + 0.5f)); };
    const Rgba lit = ui::withAlpha(ui::signal_theme::kAccent, static_cast<uint32_t>(kA * 255.0f));
    const Rgba white = col(232, 232, 226, 1.0f);
    // C tint: 45 % toward PQ Meter's default green (0.35, 0.85, 0.45).
    const Rgba cKey = col(static_cast<uint32_t>(232 + (89 - 232) * 0.45f), static_cast<uint32_t>(232 + (217 - 232) * 0.45f),
                          static_cast<uint32_t>(226 + (115 - 226) * 0.45f), 1.0f);
    for (int m = 0; m <= 143; ++m) {
      const int pc = m % 12;
      if (isBlack(pc)) continue;
      const float lo = isBlack((pc + 11) % 12) ? static_cast<float>(m) - 1.0f : static_cast<float>(m) - 0.5f;
      const float hi = isBlack((pc + 1) % 12) ? static_cast<float>(m) + 1.0f : static_cast<float>(m) + 0.5f;
      const float yt = yOfMidi(hi), yb = yOfMidi(lo);
      if (yb - yt < px1) continue;
      box(p, {xw0, yt, xw1, yb}, m == hoverMidi ? lit : pc == 0 && opts_.on(oKeysTintC) ? cKey : white);
      if (yb - yt >= 3.0f * px1) p.line({xw0, yb}, {xw1, yb}, px1, col(0, 0, 0, 0.35f));
    }
    for (int m = 0; m <= 143; ++m) {
      if (!isBlack(m % 12)) continue;
      const float yt = yOfMidi(static_cast<float>(m) + 0.5f), yb = yOfMidi(static_cast<float>(m) - 0.5f);
      if (yb - yt < px1) continue;
      box(p, {xb0, yt, xb1, yb}, m == hoverMidi ? lit : col(22, 22, 26, 1.0f));
    }
    if (labels) {
      char buf[8];
      for (int m = 0; m <= 143; m += 12) {
        const float sh = yOfMidi(static_cast<float>(m) - 0.5f) - yOfMidi(static_cast<float>(m) + 0.5f);
        const float fs = std::min(6.0f, sh * 1.4f);
        if (fs * p.transform().scale < 6.0f) continue;
        std::snprintf(buf, sizeof buf, "C%d", m / 12 - 2);  // C3 = MIDI 60 (pqdaw's naming)
        p.textAligned({left ? front - 1.5f : front + 1.5f, yOfMidi(static_cast<float>(m))}, buf, fs,
                      m == hoverMidi ? col(255, 255, 255, 1.0f) : col(70, 70, 80, 1.0f), left ? 1.0f : 0.0f);
      }
    }
    p.line({front, kPlot.y0}, {front, kPlot.y1}, px1, col(0, 0, 0, 0.5f));
  }

  // ---- the settings sheet (option_sheet.h: two tabs beside the face, or over the plot) ----
  bool coversPlot() const noexcept { return sheet_ && !panel_.beside(); }
  void paintSheet(ui::FacePainter& p, const ui::Theme& th) const {
    Options saved;
    const bool isSaved = sheet::savedDefault(type().id, saved) && saved == opts_;
    panel_.paint(p, th, opts_, rows_, hovered_, isSaved);
  }

  static void box(ui::FacePainter& p, const Rect& r, Rgba col) {
    const Vec2 pts[4] = {{r.x0, r.y0}, {r.x1, r.y0}, {r.x1, r.y1}, {r.x0, r.y1}};
    p.convex(pts, col);
  }
  static void outline(ui::FacePainter& p, Rgba col) {
    const Vec2 pts[5] = {{1.0f, 1.0f}, {kW - 1.0f, 1.0f}, {kW - 1.0f, kH - 1.0f}, {1.0f, kH - 1.0f}, {1.0f, 1.0f}};
    p.polyline(pts, 1.25f, col);
  }
  // TMM.13-fix2 (MM review contrast table: 2.92:1, the keyboard covered "MID"): the left chips start right of a keyboard
  // on the left, on a solid backing (the picture's streaks ran through the translucent one), their text lifted.
  Rect leftChip(const Rect& r) const noexcept {
    const float dx = opts_.choice(oKeys) == 1 ? opts_.keysW() : 0.0f;
    return {r.x0 + dx, r.y0, r.x1 + dx, r.y1};
  }
  void chip(ui::FacePainter& p, const Rect& r, const char* label, bool on, const ui::Theme& th, Rgba onCol = 0) const {
    const Rgba c = onCol != 0 ? onCol : th.face.value;
    p.roundedRect(r, 3.0f, ui::rgba(11, 17, 22));
    if (on) p.roundedRect(r, 3.0f, ui::mulAlpha(c, 0.3f));
    else p.roundedRect(r, 3.0f, 0);  // the same primitives either way
    p.text({0.5f * (r.x0 + r.x1), 0.5f * (r.y0 + r.y1)}, label, 6.0f, on ? c : ui::rgba(255, 255, 255, 176));
  }

  SpectrumFeed* feed_ = nullptr;
  bool acquired_ = false, settled_ = true, moving_ = false;
  mutable FeedFrames frames_;
  std::function<SpectrumFeed*()> feedOf_;
  std::function<void()> wakeHost_;
  double lastPaint_ = 0.0, probe_ = 0.0;
  bool bound_ = false;

  Options opts_;
  meta::DataPtr data_;
  bool optionsLoaded_ = false, local_ = false, wasLoop_ = false;
  std::vector<uint32_t> rows_;

  devices::GramState state_;
  devices::SpectrogramRequest sent_;
  uint64_t requests_ = 0;
  Picture pic_;
  uint64_t lastRead_ = 0;
  std::vector<float> buf_;
  std::vector<devices::GramColumn> info_;
  std::vector<uint32_t> slots_;
  // Loop.
  int64_t loopHead_ = -1;
  double loopPhase_ = 0.0, beatRate_ = 0.0, lastBeat_ = std::nan(""), lastCentre_ = -1.0;
  // The servo.
  double disp_ = -1.0, vel_ = 0.0, servoPrev_ = -1.0, lastServo_ = 0.0;
  // The texture.
  std::shared_ptr<ui::TexturePool> pool_;
  ui::TextureId tex_ = 0;
  uint32_t texW_ = 0, texH_ = 0;
  uint64_t uploads_ = 0, uploadBytes_ = 0, rebuilds_ = 0;
  // VZ.1: the plot's device size last painted, when it last moved, and a rebuild waiting for it to rest.
  float lastW_ = 0.0f, lastH_ = 0.0f;
  double sizeMoved_ = 0.0;
  bool geomPending_ = false;
  // Guides.
  std::vector<PitchGuide> guides_;
  uint64_t guidesKey_ = 0;
  bool guidesValid_ = false;

  bool frozen_ = false, sheet_ = false, hostOver_ = false;
  sheet::Sheet panel_{{kPlot.x0 + 1.0f, kChipY + kChipH + 3.0f, kPlot.x1 - 1.0f, kPlot.y1 - 1.0f}, 5.5f};
  Vec2 hover_{-1.0f, -1.0f};
  bool drag_ = false, editing_ = false;
  uint32_t dragOpt_ = 0;
  float pressY_ = 0.0f;
  double pressTravel_ = 0.0;
};

}  // namespace

std::unique_ptr<MetaFace> makeSpectrogramFace(const meta::Type& t) { return std::make_unique<SpectrogramFace>(t); }

devices::SpectrogramRequest gramRequest(const meta::DataValue* data, float plotDevicePx, double sampleRate) {
  Options o;
  o.load(data);
  return requestFor(o, columnsFor(plotDevicePx), sampleRate, 0.0);
}

bool gramShown(const MetaFace& f, GramShown& out) {
  const auto* s = dynamic_cast<const SpectrogramFace*>(&f);
  if (s == nullptr) return false;
  s->shown(out);
  return true;
}

bool gramTexel(const MetaFace& f, uint64_t column, uint32_t y, ui::Rgba& rgba, float& db, int& row) {
  const auto* s = dynamic_cast<const SpectrogramFace*>(&f);
  return s != nullptr && s->texel(column, y, rgba, db, row);
}

bool gramSlotTexel(const MetaFace& f, uint32_t slot, uint32_t y, ui::Rgba& rgba) {
  const auto* s = dynamic_cast<const SpectrogramFace*>(&f);
  return s != nullptr && s->slotTexel(slot, y, rgba);
}

float gramNoiseDb(const MetaFace& f, uint32_t y) {
  const auto* s = dynamic_cast<const SpectrogramFace*>(&f);
  return s != nullptr ? s->noiseDb(y) : 0.0f;
}

bool gramTestControl(MetaFace& f, std::string_view key, double value) {
  auto* s = dynamic_cast<SpectrogramFace*>(&f);
  if (s == nullptr) return false;
  if (key == "freeze") s->setFrozenForTest(value != 0.0);
  else if (key == "sheet") s->setSheetForTest(value != 0.0);
  else s->setOptionForTest(key, value);
  return true;
}

bool gramTestHover(MetaFace& f, ui::Vec2 local) {
  auto* s = dynamic_cast<SpectrogramFace*>(&f);
  if (s == nullptr) return false;
  s->setHoverForTest(local);
  return true;
}

}  // namespace pq::views
