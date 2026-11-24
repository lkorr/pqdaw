// The stereo faces (VZ.5; docs/plans/VZ.md "VZ.5"): the Stereo Field, the Stereo Image and the Phase Scope. Ported from
// PQ Meter (pq meter/src/modules/stereo_field.cpp, stereo_image.cpp, phase_scope.cpp: options, look, numbers) into the
// analyser frame (analyser_face.h: SET sheet, CLEAR, FREEZE, options as the instance's data, held only while painted).
//
//   Stereo Field   the goniometer: L / R plotted mid up, side across (a mono signal is the vertical line, hard left /
//                  right the diagonals, out of phase a horizontal smear), the trace fading with age (lines or dots), auto
//                  or manual gain, the diamond / circle grid, the correlation bar (−1 … +1, or 0 … 1 unipolar). Multiband:
//                  three LR4 bands (Low / Mid / High, crossovers settable) as their own traces in the band colours, each
//                  with its correlation bar, scaled to fill or at their true amplitudes. Reads the feed's raw history
//                  (kWantScope) and runs dsp::StereoFieldState over only the frames that arrived since the last poll
//                  (the bands' filters keep their state across polls). 180 × 170.
//   Stereo Image   per frequency (up the face, log / mel / linear) where it sits between LEFT and RIGHT and how wide:
//                  Ribbon (position ± width, the level as opacity, the position line), Glow (the tracked position's
//                  level deposited in a grid that decays), Heat map (PQ's particle tracker: a dot per prominent partial,
//                  its trail, an additive halo, over the diffuse underlay). Colour: Mono, Band (a five-stop ramp up the
//                  frequency axis at named crossovers: six palettes), Intensity (a colormap by level — Custom: stops in
//                  dB on a ui::LevelAxis, VZ.2), Colormap (by frequency); the width brightens the hue (Glow by width).
//                  Afterglow marks, Y smoothing, X zoom, the hover's frequency and note. Reads the feed's cross-spectrum
//                  (kWantStereo, dsp::CrossSpectrum). 220 × 170.
//   Phase Scope    what each frequency gains or loses summed to mono, filled to the 0 dB line (exponential below it: each
//                  6 dB of loss halves the rest), red past the mono-warn threshold, the coherence ribbon, the broadband
//                  mono figure, the hover's frequency and value; eased per frame. The same analyser, configured long
//                  (8192 Hann, a quarter hop). 280 × 150.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "analyser_face.h"
#include "pq/dsp/stereo.h"
#include "pq/meta/registry.h"
#include "pq/ui/colormap.h"
#include "pq/ui/renderer.h"
#include "pq/ui/texture_pool.h"
#include "pq/ui/theme.h"

namespace pq::views {

namespace {

using namespace analyser;
using sheet::kOnOff;

Rgba rgbF(float r, float g, float b, float a = 1.0f) noexcept {
  auto c = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  return ui::rgba(c(r), c(g), c(b), c(a));
}
struct Col {
  float r = 1, g = 1, b = 1;
};
Col mixC(Col a, Col b, float u) noexcept { return {a.r + (b.r - a.r) * u, a.g + (b.g - a.g) * u, a.b + (b.b - a.b) * u}; }
Col ofRgba(Rgba c) noexcept { return {ui::redOf(c) / 255.0f, ui::greenOf(c) / 255.0f, ui::blueOf(c) / 255.0f}; }
// Width → brightness (PQ widthGlow): mono content sits at `floor` of its hue, fully wide burns floor + amount, past 1
// toward white (the hue kept).
Col widthGlow(Col c, float width, float floor, float amount) noexcept {
  const float gain = floor + std::clamp(width, 0.0f, 1.0f) * amount;
  if (gain <= 1.0f) return {c.r * gain, c.g * gain, c.b * gain};
  const float over = std::clamp(gain - 1.0f, 0.0f, 1.0f);
  return {c.r + (1.0f - c.r) * over, c.g + (1.0f - c.g) * over, c.b + (1.0f - c.b) * over};
}
// VZ.10: colour option o, `def` (exact) while it is the face's own.
Col colOpt(const sheet::Values& v, uint32_t o, Col def) noexcept {
  const Rgba c = v.colour(o, rgbF(def.r, def.g, def.b));
  return v[o] < 0.0 ? def : ofRgba(c);
}
Col lineTint(Col c) noexcept { return {c.r + (1.0f - c.r) * 0.45f, c.g + (1.0f - c.g) * 0.45f, c.b + (1.0f - c.b) * 0.45f}; }

// PQ's band colours (Theme bandLow / bandMid / bandHigh).
constexpr Col kBandLow{0.95f, 0.36f, 0.25f}, kBandMid{1.00f, 0.79f, 0.24f}, kBandHigh{0.43f, 0.85f, 1.00f};

void hzText(float f, char* buf, size_t n) {
  if (f >= 1000.0f) std::snprintf(buf, n, "%.2f kHz", f / 1000.0f);
  else std::snprintf(buf, n, "%.0f Hz", f);
}
// The nearest 12-EDO note and its cents (PQ dsp/note_name.h: A4 = 440).
void noteText(float hz, char* out, size_t n) {
  if (hz <= 0.0f) {
    std::snprintf(out, n, "-");
    return;
  }
  static const char* const kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  const double semis = 12.0 * std::log2(hz / 440.0);
  const int r = static_cast<int>(std::lround(semis));
  const int midi = r + 69;
  std::snprintf(out, n, "%s%d %+.0f c", kNames[((midi % 12) + 12) % 12], midi / 12 - 1, (semis - r) * 100.0);
}

// ---- the Stereo Field (PQ stereo_field) ------------------------------------------------------------------------------

enum FieldOpt : uint32_t { fTrace, fCorrWin, fBands, fX0, fX1, fAuto, fGain, fDraw, fLine, fAlpha, fScaled, fUni, fBar, fGrid,
                           fLabels, fColTrace, fColPos, fColNeg, fColLow, fColMid, fColHigh, fCount };
constexpr const char* kDrawAs[] = {"Lines", "Dots"};
const Opt kFieldOpts[fCount] = {
    {"trace", "Trace length", 60, 10, 200, Opt::LogNumber, nullptr, "%.0f ms"},
    {"corr", "Correlation window", 300, 50, 2000, Opt::LogNumber, nullptr, "%.0f ms"},
    {"bands", "Multiband", 0, 0, 1, Opt::Toggle, kOnOff},
    {"x0", "Low / Mid", 250, 20, 2000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"x1", "Mid / High", 4000, 500, 20000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"auto", "Auto gain", 1, 0, 1, Opt::Toggle, kOnOff},
    {"gain", "Gain", 1, 0.1, 16, Opt::LogNumber, nullptr, "%.1fx"},
    {"draw", "Draw as", 0, 0, 1, Opt::Choice, kDrawAs},
    {"lw", "Line width", 1.2, 0.5, 4, Opt::Number, nullptr, "%.1f", 0.1},
    {"alpha", "Brightness", 0.85, 0.1, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"scaled", "Scaled bands", 1, 0, 1, Opt::Toggle, kOnOff},
    {"uni", "Unipolar", 0, 0, 1, Opt::Toggle, kOnOff},
    {"bar", "Correlation bar", 1, 0, 1, Opt::Toggle, kOnOff},
    {"grid", "Grid", 1, 0, 1, Opt::Toggle, kOnOff},
    {"labels", "Labels", 1, 0, 1, Opt::Toggle, kOnOff},
    // VZ.10: PQ stereo_field's colour slots.
    sheet::colourOpt("ctrace", "Trace"),
    sheet::colourOpt("cpos", "Correlation +"),
    sheet::colourOpt("cneg", "Correlation -"),
    sheet::colourOpt("clow", "Low band"),
    sheet::colourOpt("cmid", "Mid band"),
    sheet::colourOpt("chigh", "High band"),
};

class StereoFieldFace final : public AnalyserFace {
public:
  static constexpr float kW = 180.0f, kH = 170.0f;
  explicit StereoFieldFace(const meta::Type& t) : AnalyserFace(t, kW, kH, kFieldOpts) {}

  // Tests.
  float correlation() const noexcept { return state_.correlation(); }
  float bandCorrelation(uint32_t b) const noexcept { return state_.bandCorrelation(b); }
  size_t tracePoints() const noexcept { return trL_.size(); }

protected:
  uint32_t wantFlags() const noexcept override { return devices::kWantScope; }
  void sendRequest(devices::SpectrumFeed&) const override {}  // raw history: nothing to ask
  bool hasClear() const noexcept override { return false; }
  void rows(int tab, std::vector<uint32_t>& out) const override {
    out.clear();
    if (tab == sheet::kAnalysis)
      for (uint32_t i = fTrace; i <= fGain; ++i) out.push_back(i);
    else if (tab == sheet::kColours)
      for (uint32_t i = fColTrace; i < fCount; ++i) out.push_back(i);
    else
      for (uint32_t i = fDraw; i < fColTrace; ++i) out.push_back(i);
  }
  void optionSet(uint32_t) override { configured_ = false; }
  void feedChanged() override {
    end_ = 0;
    primed_ = false;
    configured_ = false;
  }
  int32_t ownHit(Vec2 l) const noexcept override { return inside(well(), l) ? kPlotPart : kNone; }
  bool ownDescribe(int32_t part, std::string& title, std::string& body) const override {
    if (part != kPlotPart) return false;
    title = "Stereo Field";
    body = "A goniometer: mid up, side across. Mono is the vertical line, hard left / right the diagonals, out-of-phase "
           "content a horizontal smear. The bar: L / R correlation (+1 mono-safe, 0 unrelated, below 0 cancels in mono). "
           "The signal is not changed.";
    return true;
  }

  // The frames that arrived since the last poll through the goniometer state, then the trace window.
  bool readFeed(devices::SpectrumFeed& f, bool& settled) override {
    const uint64_t end = f.scopeEnd(settled);
    const double sr = f.config().sampleRate;
    if (!configured_ || sr != rate_) {
      rate_ = sr;
      state_.configure(sr, static_cast<float>(opts_[fCorrWin]), opts_.on(fBands), static_cast<float>(opts_[fX0]),
                       static_cast<float>(opts_[fX1]));
      configured_ = true;
    }
    if (end == end_ && primed_) return false;
    // Prime from the last half second (a display that just appeared), else only what is new (at most a second).
    const uint64_t lead = static_cast<uint64_t>(sr * (primed_ ? 1.0 : 0.5));
    uint64_t from = primed_ ? std::max(end_, end > lead ? end - lead : 0) : (end > lead ? end - lead : 0);
    if (!primed_) state_.reset();
    primed_ = true;
    float* outs[4] = {nullptr, nullptr, nullptr, nullptr};
    while (from < end) {
      const auto n = static_cast<uint32_t>(std::min<uint64_t>(end - from, 4096));
      bufL_.resize(4096);
      bufR_.resize(4096);
      outs[f.scopeChannels() >= 4 ? 2 : 0] = bufL_.data();
      outs[f.scopeChannels() >= 4 ? 3 : 1] = bufR_.data();
      (void)f.scopeRead(from, n, outs);
      state_.push(bufL_.data(), bufR_.data(), n);
      from += n;
    }
    end_ = end;
    // The trace: the newest traceMs (strided to ≤ 4096 points).
    const auto want = static_cast<uint32_t>(std::clamp(opts_[fTrace] * 0.001 * sr, 64.0, 65536.0));
    const uint32_t stride = std::max<uint32_t>(1, want / 4096);
    const uint64_t t0 = end > want ? end - want : 0;
    const auto have = static_cast<uint32_t>(end - t0);
    wL_.resize(have);
    wR_.resize(have);
    auto pick = [&](std::vector<float>& l, std::vector<float>& r) {
      l.clear();
      r.clear();
      for (uint32_t i = 0; i < have; i += stride) {
        l.push_back(wL_[i]);
        r.push_back(wR_[i]);
      }
    };
    if (state_.bands()) {
      for (uint32_t b = 0; b < dsp::StereoFieldState::kBands; ++b) {
        state_.bandRead(b, state_.bandWritten() - have, have, wL_.data(), wR_.data());
        pick(bandL_[b], bandR_[b]);
      }
    } else {
      outs[0] = outs[1] = outs[2] = outs[3] = nullptr;
      outs[f.scopeChannels() >= 4 ? 2 : 0] = wL_.data();
      outs[f.scopeChannels() >= 4 ? 3 : 1] = wR_.data();
      (void)f.scopeRead(t0, have, outs);
      pick(trL_, trR_);
    }
    return true;
  }

  void paintBody(ui::FacePainter& p, const ui::Theme& th, bool labels) override {
    using namespace ui::signal_theme;
    const bool bands = opts_.on(fBands) && state_.bands(), uni = opts_.on(fUni), bar = opts_.on(fBar);
    const int rows = bar ? (bands ? 3 : 1) : 0;
    const float barH = 5.0f, rowGap = labels ? 8.0f : 2.0f;
    const float barsH = rows > 0 ? rows * (barH + rowGap) : 0.0f;
    const float plotT = 24.0f, plotB = kH - 10.0f - barsH;
    const float radius = std::max(6.0f, std::min(0.5f * kW - 12.0f, 0.5f * (plotB - plotT)));
    const float cx = 0.5f * kW, cy = 0.5f * (plotT + plotB);
    if (opts_.on(fGrid)) {
      const Vec2 top{cx, cy - radius}, bot{cx, cy + radius}, left{cx - radius, cy}, right{cx + radius, cy};
      const Vec2 diamond[5] = {top, right, bot, left, top};
      p.polyline(diamond, 0.5f, kGrid);
      p.ring({cx, cy}, radius, 0.5f, kGrid);
      p.line(top, bot, 0.5f, ui::rgba(255, 255, 255, 40));
      p.line(left, right, 0.5f, ui::rgba(255, 255, 255, 28));
      const float d = radius * 0.5f;
      p.line({cx, cy}, {cx - d, cy - d}, 0.5f, ui::rgba(255, 255, 255, 20));
      p.line({cx, cy}, {cx + d, cy - d}, 0.5f, ui::rgba(255, 255, 255, 20));
    }
    if (labels && radius > 20.0f) {
      p.text({cx - radius * 0.62f - 3.0f, cy - radius * 0.62f}, "L", 6.0f, kGridLabel);
      p.text({cx + radius * 0.62f + 3.0f, cy - radius * 0.62f}, "R", 6.0f, kGridLabel);
    }
    const float gainAll = std::clamp(opts_.on(fAuto) ? (state_.radius() > 1e-4f ? 0.85f / state_.radius() : 1.0f)
                                                     : static_cast<float>(opts_[fGain]), 0.05f, 32.0f);
    constexpr float kRoot2Inv = 0.70710678f;
    const float alpha = static_cast<float>(opts_[fAlpha]), lw = static_cast<float>(opts_[fLine]);
    auto trace = [&](const std::vector<float>& l, const std::vector<float>& r, Col base, float g) {
      const size_t n = l.size();
      if (n < 2) return;
      auto at = [&](size_t i) {
        const float side = std::clamp((l[i] - r[i]) * kRoot2Inv * g, -1.2f, 1.2f), mid = std::clamp((l[i] + r[i]) * kRoot2Inv * g, -1.2f, 1.2f);
        return Vec2{cx + side * radius, cy - mid * radius};
      };
      // The tail fades with age (PQ: 0.08 + 0.92 age²), drawn in 32 runs of one alpha each.
      constexpr size_t kRuns = 32;
      for (size_t run = 0; run < kRuns; ++run) {
        const size_t a = run * (n - 1) / kRuns, b = (run + 1) * (n - 1) / kRuns;
        if (b <= a) continue;
        const float age = static_cast<float>(b) / static_cast<float>(n);
        const Rgba col = rgbF(base.r, base.g, base.b, alpha * (0.08f + 0.92f * age * age));
        if (opts_.choice(fDraw) == 1) {
          for (size_t i = a; i < b; ++i) {
            const Vec2 q = at(i);
            p.roundedRect({q.x - 0.5f, q.y - 0.5f, q.x + 0.5f, q.y + 0.5f}, 0.0f, col);
          }
        } else {
          pts_.clear();
          for (size_t i = a; i <= b; ++i) pts_.push_back(at(i));
          p.polyline(pts_, lw * 0.6f, col);
        }
      }
    };
    const Col bandCol[3] = {colOpt(opts_, fColLow, kBandLow), colOpt(opts_, fColMid, kBandMid), colOpt(opts_, fColHigh, kBandHigh)};
    p.drawList().pushClip(deviceRect(p, {8.0f, plotT - 4.0f, kW - 8.0f, plotB + 4.0f}));
    if (bands) {
      for (uint32_t b = 0; b < 3; ++b) {
        const float g = opts_.on(fScaled) ? std::clamp(state_.bandRadius(b) > 1e-4f ? 0.85f / state_.bandRadius(b) : 1.0f, 0.05f, 32.0f)
                                          : gainAll;
        trace(bandL_[b], bandR_[b], bandCol[b], g);
      }
    } else {
      trace(trL_, trR_, ofRgba(opts_.colour(fColTrace, kWaveform)), gainAll);
    }
    p.drawList().popClip();
    if (rows == 0) return;
    static const char* const kBandNames[3] = {"Low", "Mid", "High"};
    const Rgba pos = opts_.colour(fColPos, kAccent), neg = opts_.colour(fColNeg, ui::rgba(224, 82, 82, 235));
    for (int k = 0; k < rows; ++k) {
      const float c = bands ? state_.bandCorrelation(static_cast<uint32_t>(k)) : state_.correlation();
      const float y1 = kH - 10.0f - static_cast<float>(rows - 1 - k) * (barH + rowGap), y0 = y1 - barH;
      const Rect b{12.0f, y0, kW - 12.0f, y1};
      p.roundedRect(b, 1.5f, ui::rgba(255, 255, 255, 14));
      const Rgba fillPos = bands ? rgbF(bandCol[k].r, bandCol[k].g, bandCol[k].b) : pos;
      const float shown = uni ? std::fabs(c) : c;
      if (uni) {
        p.roundedRect({b.x0 + 0.5f, b.y0 + 0.5f, b.x0 + shown * (b.x1 - b.x0), b.y1 - 0.5f}, 1.5f, fillPos);
        for (const float g : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
          p.line({b.x0 + g * (b.x1 - b.x0), b.y0}, {b.x0 + g * (b.x1 - b.x0), b.y1}, 0.5f, ui::rgba(255, 255, 255, g == 0.0f ? 90 : 40));
      } else {
        const float mid = 0.5f * (b.x0 + b.x1), xc = mid + shown * 0.5f * (b.x1 - b.x0);
        if (std::fabs(xc - mid) > 0.25f) p.roundedRect({std::min(mid, xc), b.y0 + 0.5f, std::max(mid, xc), b.y1 - 0.5f}, 1.5f, shown >= 0.0f ? fillPos : neg);
        for (const float g : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f})
          p.line({mid + g * 0.5f * (b.x1 - b.x0), b.y0}, {mid + g * 0.5f * (b.x1 - b.x0), b.y1}, 0.5f, ui::rgba(255, 255, 255, g == 0.0f ? 90 : 40));
      }
      if (labels) {
        char buf[16];
        std::snprintf(buf, sizeof buf, uni ? "%.2f" : "%+.2f", shown);
        p.textAligned({b.x1, b.y0 - 3.5f}, buf, 5.5f, th.device.displayText, 1.0f);
        p.textAligned({b.x0, b.y0 - 3.5f}, bands ? kBandNames[k] : "CORR", 5.0f, kGridLabel, 0.0f);
      }
    }
  }

private:
  dsp::StereoFieldState state_;
  bool configured_ = false, primed_ = false;
  double rate_ = 0.0;
  uint64_t end_ = 0;
  std::vector<float> bufL_, bufR_, wL_, wR_, trL_, trR_, bandL_[3], bandR_[3];
  std::vector<Vec2> pts_;
};

// ---- the cross-spectrum faces' shared reading ------------------------------------------------------------------------

class SpectralFace : public AnalyserFace {
public:
  const devices::StereoSnapshot& snapshot() const noexcept { return snap_; }
  dsp::CrossSpectrumConfig configForTest() const { return crossConfig(); }
  dsp::CrossSpectrumConfig configFor(const meta::DataValue* d) {
    loadForTest(d);
    return crossConfig();
  }

protected:
  using AnalyserFace::AnalyserFace;
  virtual dsp::CrossSpectrumConfig crossConfig() const = 0;
  uint32_t wantFlags() const noexcept override { return devices::kWantStereo; }
  void sendRequest(devices::SpectrumFeed& f) const override { f.setStereo(crossConfig()); }
  bool hasClear() const noexcept override { return false; }
  void feedChanged() override { seq_ = 0; }
  bool readFeed(devices::SpectrumFeed& f, bool& settled) override {
    const uint64_t seq = f.stereoRead(seq_, snap_, settled);
    if (seq == seq_) return false;
    seq_ = seq;
    return true;
  }
  devices::StereoSnapshot snap_;

private:
  uint64_t seq_ = 0;
};

// ---- the Stereo Image (PQ stereo_image) ------------------------------------------------------------------------------

enum ImageOpt : uint32_t {
  iWin, iPeakWin, iAtt, iRel, iCoh, iHold, iHeatAtt, iHeatDecay, iScale, iFMin, iFMax, iAfter, iAfterDb, iAfterS,
  iMode, iWidth, iZoom, iSmoothY, iFill, iLine, iDot, iGlowR, iGlowS, iFloor, iCeil, iColour, iPalette, iX1, iX2, iX3,
  iMap, iBase, iS2, iS3, iS4, iS5, iGamma, iWGlow, iWFloor, iWAmount, iPosLine, iGrid, iLabels, iNote, iColR1, iColR2, iColR3,
  iColR4, iColR5, iColS1, iColS2, iColS3, iColS4, iColS5, iColS6, iColLine, iCount
};
constexpr const char* kModeNames[] = {"Ribbon", "Glow", "Heat map"};
constexpr const char* kColourModes[] = {"Mono", "Band", "Intensity", "Colormap"};
constexpr const char* kPaletteNames[] = {"Theme", "Aurora", "Ember", "Ice / Fire", "Sub focus", "Mono steel"};
constexpr const char* kMapNames[] = {"Magma", "Inferno", "Viridis", "Grayscale", "Ice", "Gold", "Silver", "Pastel", "Neon",
                                     "Copper", "Custom"};
constexpr const char* kWinNames[] = {"Hann", "Kaiser"};
// PQ's ribbon palettes (stereo_image.cpp kPresets): five stops up the frequency axis and their crossovers.
struct Palette {
  Col stop[5];
  float xover[3];
};
const Palette kPalettes[] = {
    {{kBandLow, mixC(kBandLow, kBandMid, 0.5f), kBandMid, mixC(kBandMid, kBandHigh, 0.5f), kBandHigh}, {200, 1000, 5000}},
    {{{0.13f, 0.20f, 0.55f}, {0.20f, 0.52f, 0.78f}, {0.42f, 0.78f, 0.72f}, {0.72f, 0.55f, 0.85f}, {1.00f, 0.55f, 0.80f}}, {180, 900, 4500}},
    {{{0.45f, 0.05f, 0.08f}, {0.85f, 0.20f, 0.10f}, {1.00f, 0.50f, 0.12f}, {1.00f, 0.75f, 0.30f}, {1.00f, 0.95f, 0.75f}}, {150, 800, 4000}},
    {{{0.35f, 0.80f, 1.00f}, {0.55f, 0.88f, 0.95f}, {0.90f, 0.90f, 0.88f}, {1.00f, 0.66f, 0.35f}, {1.00f, 0.35f, 0.22f}}, {200, 1000, 5000}},
    {{{1.00f, 0.25f, 0.30f}, {1.00f, 0.62f, 0.20f}, {0.95f, 0.90f, 0.35f}, {0.45f, 0.70f, 0.55f}, {0.30f, 0.38f, 0.48f}}, {60, 120, 400}},
    {{{0.22f, 0.26f, 0.32f}, {0.38f, 0.44f, 0.52f}, {0.56f, 0.63f, 0.72f}, {0.75f, 0.81f, 0.88f}, {0.95f, 0.97f, 1.00f}}, {200, 1000, 5000}},
};
const Opt kImageOpts[iCount] = {
    {"win", "Analysis window", 12, 10, 14, Opt::Pow2},
    {"pwin", "Tracker window", 11, 9, 13, Opt::Pow2},
    {"att", "Attack", 60, 1, 400, Opt::LogNumber, nullptr, "%.0f ms"},
    {"rel", "Release", 400, 20, 3000, Opt::LogNumber, nullptr, "%.0f ms"},
    {"coh", "Width smoothing", 400, 20, 3000, Opt::LogNumber, nullptr, "%.0f ms"},
    {"hold", "Hold position", 1, 0, 1, Opt::Toggle, kOnOff},
    {"hatt", "Heat attack", 0.05, 0.01, 0.5, Opt::LogNumber, nullptr, "%.2f s"},
    {"hdec", "Heat decay", 0.6, 0.1, 3, Opt::LogNumber, nullptr, "%.2f s"},
    {"scale", "Frequency scale", 0, 0, 2, Opt::Choice, dsp::kFreqScaleNames},
    {"fmin", "Min frequency", 20, 10, 500, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"fmax", "Max frequency", 20000, 1000, 22050, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"ag", "Afterglow", 0, 0, 1, Opt::Toggle, kOnOff},
    {"agdb", "Glow threshold", -24, -60, -6, Opt::Number, nullptr, "%.0f dB", 1},
    {"ags", "Fade time", 3, 0.5, 10, Opt::Number, nullptr, "%.1f s", 0.1},
    {"mode", "Mode", 0, 0, 2, Opt::Choice, kModeNames},
    {"wscale", "Width scale", 1, 0.25, 3, Opt::Number, nullptr, "%.2fx", 0.01},
    {"zoom", "X zoom", 1, 0.5, 2, Opt::Number, nullptr, "%.2fx", 0.01},
    {"sy", "Y smoothing", 0, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"fill", "Fill alpha", 0.55, 0.05, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"lw", "Line width", 1.4, 0.5, 4, Opt::Number, nullptr, "%.1f", 0.1},
    {"dot", "Dot size", 4, 2, 10, Opt::Number, nullptr, "%.0f", 1},
    {"gr", "Glow radius", 16, 6, 40, Opt::Number, nullptr, "%.0f", 1},
    {"gs", "Glow strength", 0.6, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"floor", "Fade floor", -72, -120, -20, Opt::Number, nullptr, "%.0f dB", 1},
    {"ceil", "Full opacity", -12, -60, 0, Opt::Number, nullptr, "%.0f dB", 1},
    {"col", "Colour mode", 1, 0, 3, Opt::Choice, kColourModes},
    {"pal", "Palette", 0, 0, 5, Opt::Choice, kPaletteNames},
    {"x1", "Low / Low-mid", 200, 20, 20000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"x2", "Low-mid / Mid", 1000, 20, 20000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"x3", "Mid / High", 5000, 20, 20000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"map", "Colormap", 0, 0, 10, Opt::Choice, kMapNames},
    {"base", "Custom colours", 0, 0, 9, Opt::Choice, kMapNames},
    {"s2", "Stop 2", -60, -120, 0, Opt::Number, nullptr, "%.0f dB", 1},
    {"s3", "Stop 3", -45, -120, 0, Opt::Number, nullptr, "%.0f dB", 1},
    {"s4", "Stop 4", -30, -120, 0, Opt::Number, nullptr, "%.0f dB", 1},
    {"s5", "Stop 5", -18, -120, 0, Opt::Number, nullptr, "%.0f dB", 1},
    {"gamma", "Gamma", 1, 0.3, 3, Opt::LogNumber, nullptr, "%.2f"},
    {"wglow", "Glow by width", 1, 0, 1, Opt::Toggle, kOnOff},
    {"wfloor", "Mono brightness", 0.55, 0.15, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"wamt", "Glow amount", 0.75, 0, 1.5, Opt::Number, nullptr, "%.2f", 0.01},
    {"pline", "Position line", 1, 0, 1, Opt::Toggle, kOnOff},
    {"grid", "Grid", 1, 0, 1, Opt::Toggle, kOnOff},
    {"labels", "Labels", 1, 0, 1, Opt::Toggle, kOnOff},
    {"note", "Note readout", 1, 0, 1, Opt::Toggle, kOnOff},
    // VZ.10: PQ stereo_image's colour slots — the ribbon's five (over the chosen palette's), the line — and the Custom
    // ramp's six stop colours.
    sheet::colourOpt("r1", "Ribbon low"),
    sheet::colourOpt("r2", "Ribbon low-mid"),
    sheet::colourOpt("r3", "Ribbon mid"),
    sheet::colourOpt("r4", "Ribbon mid-high"),
    sheet::colourOpt("r5", "Ribbon high"),
    sheet::colourOpt("c1", "Stop 1 colour"),
    sheet::colourOpt("c2", "Stop 2 colour"),
    sheet::colourOpt("c3", "Stop 3 colour"),
    sheet::colourOpt("c4", "Stop 4 colour"),
    sheet::colourOpt("c5", "Stop 5 colour"),
    sheet::colourOpt("c6", "Stop 6 colour"),
    sheet::colourOpt("cline", "Position line"),
};

class StereoImageFace final : public SpectralFace {
public:
  static constexpr float kW = 220.0f, kH = 170.0f;
  explicit StereoImageFace(const meta::Type& t) : SpectralFace(t, kW, kH, kImageOpts) {}
  ~StereoImageFace() override {
    if (tex_ != 0 && pool_) pool_->destroy(tex_);
  }

protected:
  float sheetW() const noexcept override { return 380.0f; }
  dsp::CrossSpectrumConfig crossConfig() const override {
    dsp::CrossSpectrumConfig c;
    c.fftSize = 1u << std::clamp(opts_.choice(iWin), 10, 14);
    c.peakFft = 1u << std::clamp(opts_.choice(iPeakWin), 9, 13);
    c.hop = 256;
    c.window = dsp::SpecWindow::Kaiser;
    c.scale = dsp::freqScaleOf(opts_.choice(iScale));
    c.fMin = static_cast<float>(opts_[iFMin]);
    c.fMax = std::max(static_cast<float>(opts_[iFMax]), c.fMin * 2.0f);
    c.points = 256;
    c.attackMs = static_cast<float>(opts_[iAtt]);
    c.releaseMs = static_cast<float>(opts_[iRel]);
    c.coherenceMs = static_cast<float>(opts_[iCoh]);
    c.holdPos = opts_.on(iHold);
    c.floorDb = static_cast<float>(opts_[iFloor]);
    c.ceilDb = std::max(static_cast<float>(opts_[iCeil]), c.floorDb + 1.0f);
    const int mode = opts_.choice(iMode);
    c.parts = (mode == 1 ? dsp::kStereoGlow : 0u) | (mode == 2 ? dsp::kStereoParticles : 0u) | (opts_.on(iAfter) ? dsp::kStereoAfterglow : 0u);
    c.widthScale = static_cast<float>(opts_[iWidth]);
    c.heatAttackS = static_cast<float>(opts_[iHeatAtt]);
    c.heatDecayS = static_cast<float>(opts_[iHeatDecay]);
    c.afterglowDb = static_cast<float>(opts_[iAfterDb]);
    c.afterglowS = static_cast<float>(opts_[iAfterS]);
    return c;
  }
  void optionSet(uint32_t o) override {
    if (o == iPalette) {  // a palette brings its crossovers (where its transitions belong is part of it)
      const Palette& pal = kPalettes[std::clamp(opts_.choice(iPalette), 0, 5)];
      for (uint32_t i = 0; i < 3; ++i) opts_.set(iX1 + i, pal.xover[i]);
    }
    if (o >= iX1 && o <= iX3) {  // kept sorted
      for (uint32_t i = iX1; i < iX3; ++i)
        if (opts_[i] > opts_[i + 1]) opts_.set(o == i ? i + 1 : i, opts_[o]);
    }
    if (o >= iS2 && o <= iS5) {  // a stop halts 1 dB from its neighbours (ui::stopFence)
      float stops[4], lo, hi;
      for (int i = 0; i < 4; ++i) stops[i] = static_cast<float>(opts_[iS2 + static_cast<uint32_t>(i)]);
      ui::stopFence(stops, static_cast<int>(o - iS2), levelAxis(), lo, hi);
      opts_.set(o, std::clamp(opts_[o], static_cast<double>(lo), static_cast<double>(hi)));
    }
  }
  void rows(int tab, std::vector<uint32_t>& out) const override {
    out.clear();
    if (tab == sheet::kAnalysis) {
      for (uint32_t i = iWin; i <= iAfter; ++i) out.push_back(i);
      if (opts_.on(iAfter)) out.insert(out.end(), {iAfterDb, iAfterS});
      return;
    }
    const int mode = opts_.choice(iMode), col = opts_.choice(iColour);
    if (tab == sheet::kColours) {  // the colours the colour mode paints with
      out.push_back(iColour);
      if (col == 1) out.insert(out.end(), {iPalette, iColR1, iColR2, iColR3, iColR4, iColR5});
      if (col >= 2) {
        out.push_back(iMap);
        if (opts_.choice(iMap) == ui::Gradient::kCustomIndex) out.insert(out.end(), {iBase, iColS1, iColS2, iColS3, iColS4, iColS5, iColS6});
      }
      if (mode == 0) out.push_back(iColLine);
      return;
    }
    out.push_back(iMode);
    if (mode == 0) out.insert(out.end(), {iWidth, iZoom, iSmoothY, iFill, iLine, iPosLine});
    else if (mode == 1) out.insert(out.end(), {iWidth, iZoom, iFill});
    else out.insert(out.end(), {iZoom, iFill, iDot, iGlowR, iGlowS});
    out.insert(out.end(), {iFloor, iCeil, iColour});
    if (col == 1) out.insert(out.end(), {iPalette, iX1, iX2, iX3});
    if (col >= 2) {
      out.push_back(iMap);
      if (opts_.choice(iMap) == ui::Gradient::kCustomIndex) out.insert(out.end(), {iBase, iS2, iS3, iS4, iS5});
      if (col == 2) out.push_back(iGamma);
    }
    if (col != 2) {
      out.push_back(iWGlow);
      if (opts_.on(iWGlow)) out.insert(out.end(), {iWFloor, iWAmount});
    }
    out.insert(out.end(), {iGrid, iLabels, iNote});
  }
  int32_t ownHit(Vec2 l) const noexcept override { return inside(well(), l) ? kPlotPart : kNone; }
  bool ownDescribe(int32_t part, std::string& title, std::string& body) const override {
    if (part != kPlotPart) return false;
    title = "Stereo Image";
    body = "Per frequency (up the face): where it sits between left and right, and how wide (1 - coherence: correlated "
           "content is a point, decorrelated content a wide band). The level is the opacity. Point at it for the "
           "frequency and its note. The signal is not changed.";
    return true;
  }

  ui::LevelAxis levelAxis() const noexcept {
    const float lo = static_cast<float>(opts_[iFloor]);
    return {lo, std::max(static_cast<float>(opts_[iCeil]), lo + 1.0f), static_cast<float>(opts_[iGamma])};
  }

  void paintBody(ui::FacePainter& p, const ui::Theme&, bool labels) override {
    using namespace ui::signal_theme;
    const auto& pts = snap_.points;
    const dsp::CrossSpectrumConfig& cc = snap_.config;
    const dsp::FreqAxis axis(cc.scale, cc.fMin, cc.fMax);
    const float floorDb = static_cast<float>(opts_[iFloor]), ceilDb = std::max(static_cast<float>(opts_[iCeil]), floorDb + 1.0f);
    const float span = ceilDb - floorDb, fillA = static_cast<float>(opts_[iFill]), zoom = static_cast<float>(opts_[iZoom]);
    const bool showLabels = labels && opts_.on(iLabels);
    const float plotL = 8.0f, plotR = kW - 8.0f, plotT = 8.0f, plotB = kH - (showLabels ? 16.0f : 8.0f);
    const float plotH = plotB - plotT, cx = 0.5f * (plotL + plotR), halfW = 0.5f * (plotR - plotL) - 1.0f;
    auto yOfT = [&](float t) { return plotB - std::clamp(t, 0.0f, 1.0f) * plotH; };
    auto xOf = [&](float pos) { return cx + std::clamp(pos * zoom, -1.0f, 1.0f) * halfW; };
    if (opts_.on(iGrid)) {
      dsp::FreqGridLine grid[10];
      const int n = dsp::freqGridLines(cc.scale, cc.fMin, cc.fMax, grid, 10);
      analyser::LabelGuard guard;  // (VZ.9: none under the chips, none on another, none past the edge)
      reserveChips(guard);
      for (int i = 0; i < n; ++i) {
        const float y = yOfT(axis.unit(grid[i].hz));
        p.line({plotL, y}, {plotR, y}, 0.5f, kGrid);
        if (!showLabels) continue;
        const Vec2 at{plotR - 1.5f, y - 3.0f};
        const Rect box = analyser::labelBox(p, at, grid[i].label, 5.0f, 1.0f);
        if (guard.place({box.x0, box.y0 - 1.0f, box.x1, box.y1 + 1.0f}, {plotL, plotT, plotR, plotB}))
          p.textAligned(at, grid[i].label, 5.0f, kGridLabel, 1.0f);
      }
      for (const float g : {-1.0f, -0.5f, 0.5f, 1.0f}) p.line({xOf(g), plotT}, {xOf(g), plotB}, 0.5f, kGrid);
    }
    p.line({cx, plotT}, {cx, plotB}, 0.5f, ui::rgba(255, 255, 255, 45));  // the centre: the reference
    // Colour.
    const int colMode = opts_.choice(iColour), map = opts_.choice(iMap);
    Palette pal = kPalettes[std::clamp(opts_.choice(iPalette), 0, 5)];
    for (uint32_t i = 0; i < 5; ++i) pal.stop[i] = colOpt(opts_, iColR1 + i, pal.stop[i]);
    float at[5] = {0.0f, 0, 0, 0, 1.0f};
    for (int i = 0; i < 3; ++i) at[i + 1] = std::clamp(axis.unit(static_cast<float>(opts_[iX1 + static_cast<uint32_t>(i)])), 0.0f, 1.0f);
    const ui::LevelAxis lax = levelAxis();
    const ui::ColorLut* lut = nullptr;
    if (colMode >= 2) {
      if (map == ui::Gradient::kCustomIndex) {
        float stops[4];
        for (int i = 0; i < 4; ++i) stops[i] = static_cast<float>(opts_[iS2 + static_cast<uint32_t>(i)]);
        std::array<double, 14> k = {opts_[iBase], stops[0], stops[1], stops[2], stops[3], opts_[iFloor], opts_[iCeil], opts_[iGamma]};
        for (uint32_t i = 0; i < 6; ++i) k[8 + i] = opts_[iColS1 + i];
        if (k != customKey_) {  // rebuilt when its stops or their colours change, not every paint (L11)
          customKey_ = k;
          const ui::StopColours own = sheet::stopColours(opts_, iColS1, opts_.choice(iBase));
          ui::levelGradient(custom_, static_cast<ui::Colormap>(std::clamp(opts_.choice(iBase), 0, ui::kColormapCount - 1)), stops, lax, &own);
        }
        lut = &custom_.lut;
      } else {
        lut = &ui::colormapLut(static_cast<ui::Colormap>(std::clamp(map, 0, ui::kColormapCount - 1)));
      }
    }
    auto fillAt = [&](float t) -> Col {
      t = std::clamp(t, 0.0f, 1.0f);
      if (colMode == 1) {
        if (t <= at[0]) return pal.stop[0];
        if (t >= at[4]) return pal.stop[4];
        int s = 0;
        while (s < 3 && t > at[s + 1]) ++s;
        const float sp = at[s + 1] - at[s];
        return mixC(pal.stop[s], pal.stop[s + 1], sp > 1e-6f ? (t - at[s]) / sp : 0.0f);
      }
      if (colMode == 3 && lut != nullptr) return ofRgba(ui::lutAt(*lut, t));
      return pal.stop[2];
    };
    auto byLevel = [&](float db) { return ofRgba(ui::lutAt(*lut, lax.posOf(db))); };
    auto levelA = [&](float db) { return std::clamp((db - floorDb) / span, 0.0f, 1.0f); };
    const bool wglow = opts_.on(iWGlow) && colMode != 2;
    const float wFloor = static_cast<float>(opts_[iWFloor]), wAmt = static_cast<float>(opts_[iWAmount]);
    const int mode = opts_.choice(iMode);
    const size_t n = pts.size();
    auto tOf = [&](size_t i) { return n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f; };
    // Y smoothing (display only): a triangular kernel along the axis.
    shown_.assign(pts.begin(), pts.end());
    if (const double sy = opts_[iSmoothY]; sy > 0.01 && n > 2 && mode == 0) {
      const int rad = std::clamp(static_cast<int>(sy * 6.0), 1, 24);
      for (size_t i = 0; i < n; ++i) {
        float sp = 0.0f, sw = 0.0f, sd = 0.0f, wt = 0.0f;
        for (int k = -rad; k <= rad; ++k) {
          const auto& q = pts[static_cast<size_t>(std::clamp(static_cast<int>(i) + k, 0, static_cast<int>(n) - 1))];
          const float d = 1.0f - static_cast<float>(std::abs(k)) / static_cast<float>(rad + 1);
          sp += q.pos * d, sw += q.width * d, sd += q.db * d, wt += d;
        }
        shown_[i].pos = sp / wt, shown_[i].width = sw / wt, shown_[i].db = sd / wt;
      }
    }
    p.drawList().pushClip(deviceRect(p, {plotL, plotT, plotR, plotB}));
    const float pxPerUnit = p.transform().scale;
    auto grid = [&](float alphaScale) {  // the glow / underlay grid: rows merged to about a device pixel
      const auto& heat = snap_.heat;
      constexpr uint32_t kCols = dsp::CrossSpectrum::kHeatCols;
      if (n == 0 || heat.size() != n * kCols) return;
      // M7: one texture (a texel a cell, linear filtering) and one quad, not up to 32 k overlapping rects a frame.
      if (ui::TexturePool* pool = p.images(); pool != nullptr && pool->attached()) {
        if (pool_.get() != pool) {
          if (tex_ != 0 && pool_) pool_->destroy(tex_);
          tex_ = 0;
          pool_ = pool->share();
        }
        if (tex_ == 0 || texRows_ != n) {
          if (tex_ != 0) pool_->destroy(tex_);
          tex_ = 0;
          if (auto t = pool_->create(kCols, static_cast<uint32_t>(n), ui::TextureFilter::Linear)) tex_ = *t;
          texRows_ = n;
        }
        if (tex_ != 0) {
          texels_.resize(n * kCols);
          for (size_t r = 0; r < n; ++r) {
            const Col base = fillAt(tOf(r));
            uint32_t* row = texels_.data() + (n - 1 - r) * kCols;  // texture row 0: the top (fMax)
            for (uint32_t c = 0; c < kCols; ++c) {
              const float h = heat[r * kCols + c];
              if (h < 0.004f) {
                row[c] = 0;
                continue;
              }
              Col col = colMode == 2 ? byLevel(floorDb + h * span) : base;
              if (wglow) col = widthGlow(col, h, wFloor, wAmt);
              row[c] = rgbF(col.r, col.g, col.b, std::clamp(fillA * h * h * alphaScale, 0.0f, 1.0f));
            }
          }
          (void)pool_->update(tex_, ui::TextureRect{0, 0, kCols, static_cast<uint32_t>(n)},
                              {reinterpret_cast<const uint8_t*>(texels_.data()), texels_.size() * 4});
          // Texel centres on the cells' centres: row r at the axis' t = r / (n - 1), column c at (c + 0.5) / kCols.
          const float half = n > 1 ? 0.5f * plotH / static_cast<float>(n - 1) : 0.5f * plotH;
          const Vec2 a = p.transform().map({plotL, yOfT(1.0f) - half}), b = p.transform().map({plotR, yOfT(0.0f) + half});
          p.drawList().addImage(tex_, {a.x, a.y, b.x, b.y}, {0.0f, 0.0f}, {1.0f, 1.0f});
          return;
        }
      }
      // (No texture pool: the cells as rects.)
      const float cellH = plotH / static_cast<float>(n), cellW = (plotR - plotL) / static_cast<float>(kCols);
      const size_t stride = std::max<size_t>(1, static_cast<size_t>(1.0f / std::max(0.05f, cellH * pxPerUnit)));
      for (size_t r0 = 0; r0 < n; r0 += stride) {
        const Col base = fillAt(tOf(r0));
        const float cy = yOfT(tOf(r0));
        const float bh = cellH * 2.5f * static_cast<float>(stride);
        for (uint32_t c = 0; c < kCols; ++c) {
          float h = 0.0f;
          for (size_t r = r0; r < std::min(n, r0 + stride); ++r) h = std::max(h, heat[r * kCols + c]);
          if (h < 0.004f) continue;
          Col col = colMode == 2 ? byLevel(floorDb + h * span) : base;
          if (wglow) col = widthGlow(col, h, wFloor, wAmt);
          const float x = plotL + (static_cast<float>(c) + 0.5f) * cellW, bw = cellW * 2.5f;
          p.roundedRect({x - 0.5f * bw, cy - 0.5f * bh, x + 0.5f * bw, cy + 0.5f * bh}, 0.0f, rgbF(col.r, col.g, col.b, fillA * h * h * alphaScale));
        }
      }
    };
    if (mode == 2) {
      grid(0.45f);
      auto curved = [](float v) { return v >= 0.0f ? std::pow(v, 0.7f) : -std::pow(-v, 0.7f); };
      const float dot = static_cast<float>(opts_[iDot]) * 0.5f, gr = static_cast<float>(opts_[iGlowR]) * 0.5f, gs = static_cast<float>(opts_[iGlowS]);
      auto colourOf = [&](float lf, float e, float& t) {
        t = axis.unit(std::exp2(lf));
        Col col = colMode == 2 ? byLevel(floorDb + e * span) : fillAt(t);
        if (wglow) col = widthGlow(col, e, wFloor, wAmt);
        return col;
      };
      auto each = [&](auto&& fn) {
        for (const dsp::StereoParticle& part : snap_.particles) {
          if (!part.alive) continue;
          for (uint32_t k = 0; k < part.trailCount; ++k) {
            const auto& tr = part.trail[(part.trailHead + dsp::StereoParticle::kTrail - part.trailCount + k) % dsp::StereoParticle::kTrail];
            if (tr.energy >= 0.004f) fn(tr.logF, tr.pan, tr.energy);
          }
          if (part.energy >= 0.004f) fn(part.logF, part.pan, part.energy);
        }
      };
      p.beginGlow();  // the halo (additive)
      each([&](float lf, float pan, float e) {
        float t;
        const Col col = colourOf(lf, e, t);
        if (t < 0.0f || t > 1.0f) return;
        p.glow({xOf(curved(pan)), yOfT(t)}, gr * (0.5f + 0.5f * e), rgbF(col.r, col.g, col.b, fillA * e * gs));
      });
      p.endGlow();
      each([&](float lf, float pan, float e) {  // the core
        float t;
        const Col col = colourOf(lf, e, t);
        if (t < 0.0f || t > 1.0f) return;
        p.disc({xOf(curved(pan)), yOfT(t)}, dot * 0.5f, rgbF(col.r, col.g, col.b, std::clamp(e, 0.0f, 1.0f)));
      });
    } else if (mode == 1) {
      grid(1.0f);
    } else if (n > 1) {
      const float minW = 0.4f / halfW;
      const float ws = static_cast<float>(opts_[iWidth]);
      if (cc.parts & dsp::kStereoAfterglow) {  // afterglow under the live ribbon
        for (size_t i = 0; i + 1 < n; ++i) {
          const auto& a = pts[i];
          const auto& b = pts[i + 1];
          if (a.glowLevel < 0.004f && b.glowLevel < 0.004f) continue;
          const float aw = std::max(minW, std::clamp(a.glowWidth * ws, 0.0f, 1.0f)), bw = std::max(minW, std::clamp(b.glowWidth * ws, 0.0f, 1.0f));
          const Vec2 q[4] = {{xOf(a.glowPos - aw), yOfT(tOf(i))}, {xOf(a.glowPos + aw), yOfT(tOf(i))}, {xOf(b.glowPos + bw), yOfT(tOf(i + 1))},
                             {xOf(b.glowPos - bw), yOfT(tOf(i + 1))}};
          const float avg = 0.5f * (a.glowLevel + b.glowLevel);
          Col c = colMode == 2 ? byLevel(floorDb + avg * span) : fillAt((static_cast<float>(i) + 0.5f) / static_cast<float>(n - 1));
          if (wglow) c = widthGlow(c, 0.5f * (a.glowWidth + b.glowWidth), wFloor, wAmt);
          p.convex(q, rgbF(c.r, c.g, c.b, fillA * avg));
        }
      }
      for (size_t i = 0; i + 1 < n; ++i) {
        const auto& a = shown_[i];
        const auto& b = shown_[i + 1];
        const float aA = levelA(a.db), bA = levelA(b.db);
        if (aA < 0.004f && bA < 0.004f) continue;
        const float aw = std::max(minW, std::clamp(a.width * ws, 0.0f, 1.0f)), bw = std::max(minW, std::clamp(b.width * ws, 0.0f, 1.0f));
        const float ay = yOfT(tOf(i)), by = yOfT(tOf(i + 1));
        const Vec2 q[4] = {{xOf(a.pos - aw), ay}, {xOf(a.pos + aw), ay}, {xOf(b.pos + bw), by}, {xOf(b.pos - bw), by}};
        Col c = colMode == 2 ? byLevel(0.5f * (a.db + b.db)) : fillAt((static_cast<float>(i) + 0.5f) / static_cast<float>(n - 1));
        if (wglow) c = widthGlow(c, 0.5f * (a.width + b.width), wFloor, wAmt);
        p.convex(q, rgbF(c.r, c.g, c.b, fillA * 0.5f * (aA + bA)));
      }
      if (opts_.on(iPosLine)) {
        const float lw = static_cast<float>(opts_[iLine]) * 0.6f;
        const Col lc = colOpt(opts_, iColLine, lineTint(fillAt(0.5f)));  // (its own: every point; else the ramp's tint)
        const bool ownLine = opts_[iColLine] >= 0.0;
        // WF.2: one joined stroke per run of audible rows, a colour per point (it was a ribbon per segment: 255
        // unjoined butt-ended pieces, whose overlaps beaded along a translucent line and gaped at bends).
        linePts_.clear();
        lineCols_.clear();
        auto flushLine = [&] {
          if (linePts_.size() >= 2) p.polylineCols(linePts_, lineCols_, lw);
          linePts_.clear();
          lineCols_.clear();
        };
        for (size_t i = 0; i < n; ++i) {
          const auto& a = shown_[i];
          const float aA = levelA(a.db);
          // (a silent row between audible ones still ends a segment at alpha 0, as the per-segment mean did)
          const bool near = aA >= 0.004f || (i > 0 && levelA(shown_[i - 1].db) >= 0.004f) || (i + 1 < n && levelA(shown_[i + 1].db) >= 0.004f);
          if (!near) {
            flushLine();
            continue;
          }
          const Col c = ownLine ? lc : lineTint(colMode == 2 ? byLevel(a.db) : fillAt(static_cast<float>(i) / static_cast<float>(n - 1)));
          linePts_.push_back({xOf(a.pos), yOfT(tOf(i))});
          lineCols_.push_back(rgbF(c.r, c.g, c.b, aA));
        }
        flushLine();
      }
    }
    p.drawList().popClip();
    if (!showLabels) return;
    p.textAligned({plotL + 2.0f, kH - 11.0f}, "LEFT", 5.5f, kGridLabel, 0.0f);
    p.textAligned({cx, kH - 11.0f}, "0", 5.5f, kGridLabel, 0.5f);
    p.textAligned({plotR - 2.0f, kH - 11.0f}, "RIGHT", 5.5f, kGridLabel, 1.0f);
    if (hover_.x >= plotL && hover_.x <= plotR && hover_.y >= plotT && hover_.y <= plotB) {
      const float f = axis.hz((plotB - hover_.y) / plotH);
      p.line({plotL, hover_.y}, {plotR, hover_.y}, 0.5f, ui::rgba(255, 255, 255, 40));
      char buf[32];
      hzText(f, buf, sizeof buf);
      const float ry = std::clamp(hover_.y, plotT + 14.0f, plotB - 10.0f);
      p.textAligned({plotL + 3.0f, ry - 7.0f}, buf, 6.0f, ui::rgba(255, 255, 255, 210), 0.0f);
      if (opts_.on(iNote)) {
        noteText(f, buf, sizeof buf);
        p.textAligned({plotL + 3.0f, ry + 2.0f}, buf, 6.0f, ui::rgba(160, 220, 255, 190), 0.0f);
      }
    }
  }

private:
  ui::Gradient custom_;
  std::array<double, 14> customKey_{-1.0};
  std::shared_ptr<ui::TexturePool> pool_;  // the glow / heat grid's texture (M7)
  ui::TextureId tex_ = 0;
  size_t texRows_ = 0;
  std::vector<uint32_t> texels_;
  std::vector<dsp::StereoPoint> shown_;
  std::vector<Vec2> linePts_;   // WF.2: the position line's current run and its colours
  std::vector<Rgba> lineCols_;
};

// ---- the Phase Scope (PQ phase_scope) --------------------------------------------------------------------------------

enum PhaseOpt : uint32_t {
  qFft, qWin, qAvg, qFloor, qScale, qFMin, qFMax, qRange, qWarn, qSmooth, qLine, qCoh, qGrid, qLabels, qColMono, qColCoh, qColWarn, qCount
};
const Opt kPhaseOpts[qCount] = {
    {"fft", "FFT size", 13, 9, 14, Opt::Pow2},
    {"win", "Window", 0, 0, 1, Opt::Choice, kWinNames},
    {"avg", "Averaging", 500, 50, 3000, Opt::LogNumber, nullptr, "%.0f ms"},
    {"floor", "Level floor", -80, -120, -20, Opt::Number, nullptr, "%.0f dB", 1},
    {"scale", "Frequency scale", 0, 0, 2, Opt::Choice, dsp::kFreqScaleNames},
    {"fmin", "Low", 20, 10, 500, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"fmax", "High", 20000, 1000, 24000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"range", "Mono range", 12, 6, 40, Opt::Number, nullptr, "%.0f dB", 1},
    {"warn", "Mono warn", -3, -12, 0, Opt::Number, nullptr, "%.0f dB", 0.5},
    {"smooth", "Smoothing", 45, 0, 200, Opt::Number, nullptr, "%.0f ms", 1},
    {"lw", "Line width", 1.5, 0.8, 3, Opt::Number, nullptr, "%.1f", 0.1},
    {"coh", "Coherence ribbon", 0, 0, 1, Opt::Toggle, kOnOff},
    {"grid", "Grid", 1, 0, 1, Opt::Toggle, kOnOff},
    {"labels", "Labels", 1, 0, 1, Opt::Toggle, kOnOff},
    // VZ.10: PQ phase_scope's colour slots (and the warning's).
    sheet::colourOpt("cmono", "Mono loss"),
    sheet::colourOpt("ccoh", "Coherence"),
    sheet::colourOpt("cwarn", "Warning"),
};
constexpr Rgba kMonoCol = ui::rgba(210, 165, 255), kCohCol = ui::rgba(150, 235, 190), kWarnCol = ui::rgba(255, 90, 90);

class PhaseScopeFace final : public SpectralFace {
public:
  static constexpr float kW = 280.0f, kH = 150.0f;
  explicit PhaseScopeFace(const meta::Type& t) : SpectralFace(t, kW, kH, kPhaseOpts) {}
  const std::vector<float>& shownMono() const noexcept { return mono_; }

protected:
  dsp::CrossSpectrumConfig crossConfig() const override {
    dsp::CrossSpectrumConfig c;
    c.fftSize = 1u << std::clamp(opts_.choice(qFft), 9, 14);
    c.hop = std::max(64u, c.fftSize / 4);
    c.window = opts_.choice(qWin) == 1 ? dsp::SpecWindow::Kaiser : dsp::SpecWindow::Hann;
    c.scale = dsp::freqScaleOf(opts_.choice(qScale));
    c.fMin = static_cast<float>(opts_[qFMin]);
    c.fMax = std::max(static_cast<float>(opts_[qFMax]), c.fMin * 2.0f);
    c.points = 512;
    c.attackMs = c.releaseMs = c.coherenceMs = static_cast<float>(opts_[qAvg]);  // one average (PQ: release)
    c.holdPos = false;
    c.floorDb = static_cast<float>(opts_[qFloor]);
    c.ceilDb = c.floorDb + 24.0f;
    return c;
  }
  void rows(int tab, std::vector<uint32_t>& out) const override {
    out.clear();
    if (tab == sheet::kAnalysis)
      for (uint32_t i = qFft; i <= qFMax; ++i) out.push_back(i);
    else if (tab == sheet::kColours)
      for (uint32_t i = qColMono; i < qCount; ++i) out.push_back(i);
    else
      for (uint32_t i = qRange; i < qColMono; ++i) out.push_back(i);
  }
  int32_t ownHit(Vec2 l) const noexcept override { return inside(well(), l) ? kPlotPart : kNone; }
  bool ownDescribe(int32_t part, std::string& title, std::string& body) const override {
    if (part != kPlotPart) return false;
    title = "Phase Scope";
    body = "What each frequency gains or loses when the channels are summed to mono: 0 dB no change, +3 dB identical "
           "channels, down toward -inf where they cancel (red past the warn level). The signal is not changed.";
    return true;
  }
  // The display easing (PQ smoothMs): the curve moves with the frames, not in hop-sized steps.
  bool animate(float dt, bool fresh) override {
    const auto& pts = snap_.points;
    if (fresh || mono_.size() != pts.size()) {
      mono_.resize(pts.size());
      for (size_t i = 0; i < pts.size(); ++i) mono_[i] = pts[i].monoDb;
      return false;
    }
    const double sm = opts_[qSmooth];
    bool moving = false;
    const float a = sm > 0.1 ? 1.0f - std::exp(-dt / static_cast<float>(sm * 0.001)) : 1.0f;
    for (size_t i = 0; i < pts.size(); ++i) {
      const float target = std::max(pts[i].monoDb, -120.0f);
      mono_[i] += (target - mono_[i]) * a;
      if (std::fabs(target - mono_[i]) < 0.01f) mono_[i] = target;
      else moving = true;
    }
    return moving;
  }

  void paintBody(ui::FacePainter& p, const ui::Theme&, bool labels) override {
    const Rgba monoCol = opts_.colour(qColMono, kMonoCol), cohCol = opts_.colour(qColCoh, kCohCol), warnCol = opts_.colour(qColWarn, kWarnCol);
    using namespace ui::signal_theme;
    const auto& pts = snap_.points;
    const dsp::CrossSpectrumConfig& cc = snap_.config;
    const dsp::FreqAxis axis(cc.scale, cc.fMin, cc.fMax);
    const bool showLabels = labels && opts_.on(qLabels), grid = opts_.on(qGrid);
    const float plotL = showLabels ? 24.0f : 8.0f, plotR = kW - 8.0f, top = 24.0f, bot = kH - (showLabels ? 16.0f : 8.0f);
    const float range = static_cast<float>(opts_[qRange]), warn = static_cast<float>(opts_[qWarn]);
    const float floorDb = static_cast<float>(opts_[qFloor]), lw = static_cast<float>(opts_[qLine]) * 0.6f;
    auto xOf = [&](float hz) { return plotL + std::clamp(axis.unit(std::max(1e-3f, hz)), 0.0f, 1.0f) * (plotR - plotL); };
    if (grid || showLabels) {
      dsp::FreqGridLine rules[16];
      const int nr = dsp::freqGridLines(cc.scale, cc.fMin, cc.fMax, rules, 16);
      float lastR = -1e9f;
      for (int i = 0; i < nr; ++i) {
        const float x = xOf(rules[i].hz);
        if (grid) p.line({x, top}, {x, bot}, 0.5f, kGrid);
        const float tw = 3.0f * static_cast<float>(std::strlen(rules[i].label));
        if (showLabels && x - 0.5f * tw >= lastR + 3.0f && x + 0.5f * tw <= plotR) {
          p.textAligned({x, bot + 6.0f}, rules[i].label, 5.0f, kGridLabel, 0.5f);
          lastR = x + 0.5f * tw;
        }
      }
    }
    // The mono lane: +6 dB headroom linear above 0; below, each 6 dB of loss takes half the remaining space.
    const float lt = top + 2.0f, lb = bot - 2.0f, mHi = 6.0f;
    const float yZero = lb - 6.0f / (6.0f + range) * (lb - lt);
    auto yOf = [&](float v) {
      if (v >= 0.0f) return yZero - std::clamp(v / mHi, 0.0f, 1.0f) * (yZero - lt);
      return yZero + (1.0f - std::pow(2.0f, v / 6.0f)) * (lb - yZero);
    };
    float lastLab = -1e9f;
    for (const float d : {3.0f, 0.0f, -6.0f, -12.0f, -18.0f, -24.0f}) {
      const float y = yOf(d);
      if (y > lb - 2.0f || y - lastLab < 6.0f) continue;  // the squeezed tail keeps one label per text height
      lastLab = y;
      if (grid) p.line({plotL, y}, {plotR, y}, 0.5f, d == 0.0f ? ui::mulAlpha(kGridLabel, 0.9f) : kGrid);
      if (showLabels) {
        char lab[8];
        std::snprintf(lab, sizeof lab, "%+.0f", d);
        p.textAligned({plotL - 3.0f, y}, lab, 5.0f, kGridLabel, 1.0f);
      }
    }
    p.line({plotL, yOf(warn)}, {plotR, yOf(warn)}, 0.5f, ui::mulAlpha(warnCol, 0.35f));
    p.drawList().pushClip(deviceRect(p, {plotL, lt, plotR, lb}));
    // The area to the 0 dB line, one strip per run of visible points of one state (normal / past the warn level: red),
    // then the curve segment by segment at its level's opacity.
    // WF.2: the curve is one joined stroke per run with a colour per point (ui::smoothPolylineCols through the
    // painter); it was a two-point ribbon per segment — unjoined, the butt ends overlapping in a translucent colour and
    // leaving wedges on the steep sides of a notch.
    bool runWarn = false;
    float runA = 0.0f;
    pts_.clear();
    linePts_.clear();
    lineCols_.clear();
    auto flushLine = [&] {
      if (linePts_.size() >= 2) p.polylineCols(linePts_, lineCols_, lw);
      linePts_.clear();
      lineCols_.clear();
    };
    auto flush = [&] {
      if (pts_.size() >= 2) {
        const Rgba c = ui::mulAlpha(runWarn ? warnCol : monoCol, 0.18f * runA / static_cast<float>(pts_.size()));
        p.fillUnder(pts_, yZero, c, c);
      }
      pts_.clear();
      runA = 0.0f;
    };
    for (size_t i = 0; i < pts.size(); ++i) {
      const auto& pt = pts[i];
      const float a = std::clamp((pt.db - floorDb) / 24.0f, 0.0f, 1.0f);
      if (pt.db < floorDb || a <= 0.01f) {
        flush();
        flushLine();
        continue;
      }
      const float v = std::clamp(i < mono_.size() ? mono_[i] : pt.monoDb, -120.0f, mHi);
      const Vec2 cur{xOf(pt.hz), yOf(v)};
      const bool warnHere = v < warn;
      if (!pts_.empty() && warnHere != runWarn) {  // the strips meet at the boundary point
        const Vec2 last = pts_.back();
        flush();
        pts_.push_back(last);
      }
      runWarn = warnHere;
      pts_.push_back(cur);
      runA += a;
      linePts_.push_back(cur);
      lineCols_.push_back(ui::mulAlpha(warnHere ? warnCol : monoCol, a));
    }
    flush();
    flushLine();
    p.drawList().popClip();
    if (opts_.on(qCoh)) {  // how much of the picture to believe
      const float rb = bot - 1.0f, ribH = std::min(14.0f, (bot - top) * 0.12f);
      pts_.clear();
      for (const auto& pt : pts)
        if (pt.hz >= cc.fMin && pt.hz <= cc.fMax) pts_.push_back({xOf(pt.hz), rb - std::clamp(pt.coherence, 0.0f, 1.0f) * ribH});
      if (pts_.size() >= 2) {
        p.fillUnder(pts_, rb, ui::mulAlpha(cohCol, 0.10f), ui::mulAlpha(cohCol, 0.10f));
        p.polyline(pts_, 0.6f, ui::mulAlpha(cohCol, 0.45f));
      }
    }
    if (!showLabels) return;
    {  // the broadband figure, summed in power (a quiet band cannot drag it down as hard as the bass)
      double tot = 0.0, sum = 0.0;
      for (size_t i = 0; i < pts.size(); ++i)
        if (pts[i].db > floorDb) {
          const double lin = std::pow(10.0, pts[i].db / 10.0);
          tot += lin;
          sum += lin * std::pow(10.0, (i < mono_.size() ? mono_[i] : pts[i].monoDb) / 10.0);
        }
      if (tot > 1e-24) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "mono %+.1f dB", 10.0 * std::log10(std::max(sum, 1e-12 * tot) / tot));
        p.textAligned({plotL + 3.0f, 15.0f}, buf, 6.0f, ui::rgba(255, 255, 255, 170), 0.0f);
      }
    }
    if (hover_.x >= plotL && hover_.x <= plotR && hover_.y >= top && hover_.y <= bot) {
      p.line({hover_.x, top}, {hover_.x, bot}, 0.5f, ui::rgba(255, 255, 255, 60));
      const float f = axis.hz((hover_.x - plotL) / (plotR - plotL));
      char fb[32], vb[32];
      hzText(f, fb, sizeof fb);
      float db;
      if (hover_.y <= yZero) db = (yZero - hover_.y) / (yZero - lt) * mHi;
      else {
        const float frac = 1.0f - (hover_.y - yZero) / (lb - yZero);
        db = frac > 1e-6f ? 6.0f * std::log2(frac) : -120.0f;
      }
      std::snprintf(vb, sizeof vb, "%+.1f dB", db);
      const bool left = hover_.x + 50.0f > plotR;
      const float tx = left ? hover_.x - 4.0f : hover_.x + 4.0f;
      p.textAligned({tx, top + 4.0f}, fb, 6.0f, ui::rgba(255, 255, 255, 210), left ? 1.0f : 0.0f);
      p.textAligned({tx, top + 12.0f}, vb, 6.0f, ui::rgba(255, 255, 255, 190), left ? 1.0f : 0.0f);
    }
  }

private:
  std::vector<float> mono_;
  std::vector<Vec2> pts_;
  std::vector<Vec2> linePts_;   // WF.2: the curve's current run and its colours
  std::vector<Rgba> lineCols_;
};

}  // namespace

std::unique_ptr<MetaFace> makeStereoFieldFace(const meta::Type& t) { return std::make_unique<StereoFieldFace>(t); }
std::unique_ptr<MetaFace> makeStereoImageFace(const meta::Type& t) { return std::make_unique<StereoImageFace>(t); }
std::unique_ptr<MetaFace> makePhaseScopeFace(const meta::Type& t) { return std::make_unique<PhaseScopeFace>(t); }

bool stereoShown(const MetaFace& f, StereoShown& out) {
  const auto* a = dynamic_cast<const AnalyserFace*>(&f);
  if (a == nullptr) return false;
  out = StereoShown{};
  out.acquired = a->acquiredForTest();
  out.frozen = a->frozenForTest();
  if (const auto* s = dynamic_cast<const SpectralFace*>(&f)) {
    out.config = s->configForTest();
    out.points = s->snapshot().points;
    for (const auto& p : s->snapshot().particles) out.particles += p.alive ? 1 : 0;
    out.heat = !s->snapshot().heat.empty();
    if (const auto* ph = dynamic_cast<const PhaseScopeFace*>(&f)) out.mono = ph->shownMono();
    return true;
  }
  if (const auto* g = dynamic_cast<const StereoFieldFace*>(&f)) {
    out.correlation = g->correlation();
    for (uint32_t b = 0; b < 3; ++b) out.bandCorrelation[b] = g->bandCorrelation(b);
    out.tracePoints = g->tracePoints();
    return true;
  }
  return false;
}

bool stereoTestControl(MetaFace& f, std::string_view key, double value) {
  auto* a = dynamic_cast<AnalyserFace*>(&f);
  if (a == nullptr || (dynamic_cast<SpectralFace*>(&f) == nullptr && dynamic_cast<StereoFieldFace*>(&f) == nullptr)) return false;
  if (key == "freeze") a->setFrozenForTest(value != 0.0);
  else if (key == "sheet") a->setSheetForTest(value != 0.0);
  else if (key == "hoverx") a->hoverForTest({static_cast<float>(value), 80.0f});
  else a->setOptionForTest(key, value);
  return true;
}

dsp::CrossSpectrumConfig stereoRequest(std::string_view type, const meta::DataValue* data) {
  const meta::Type* t = meta::findType(type);
  if (t == nullptr) return {};
  std::unique_ptr<MetaFace> f = makeFace(*t);
  auto* s = f ? dynamic_cast<SpectralFace*>(f.get()) : nullptr;
  return s != nullptr ? s->configFor(data) : dsp::CrossSpectrumConfig{};
}

}  // namespace pq::views
