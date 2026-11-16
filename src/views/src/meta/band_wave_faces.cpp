// The Band Levels and Waveform faces (VZ.6; docs/plans/VZ.md "VZ.6"): each module IS its display. Ported from PQ Meter
// (pq meter/src/modules/multiband_rms.cpp and waveform.cpp: their options, look and readouts) into pqdaw's face frame:
// a dark body, the display well, chips in the top-right corner (SET: the settings sheet, option_sheet.h, ANALYSIS /
// DISPLAY with SAVE DEFAULT / RESET; FREEZE: the picture holds while the analysis runs on), the options as the
// instance's data. 300 × 160 each.
//
//   Band Levels  each band's RMS level over time, oldest left: the fills first (per-column quads, back to front: the
//                lows normally sit highest), then every line on top, broken where a band is silent (no rule along the
//                floor); PQ's blue → amber ramp per band count; the dB axis linear or WARPED (above the knee linear in
//                dB, below it a power curve folds the rest into a thin strip: mbDbToNorm); 6 dB rules thinned where
//                they crowd, the floor always ruled, second ticks labelled −N s; the bands' current levels top left; a
//                hover inspector (time, each band's level there).
//   Waveform     the scrolling peak envelope (min … max per column) with the RMS body inside it, in lanes (mid / side,
//                L / R, mono, or L and R overlaid); the colour the lane's theme colour, its level (heat) or its
//                brightness (the energy above 700 Hz) through a colormap, smoothed across columns weighted by
//                energy; a glow above and below, the outline caps, clip marks; the playhead Scroll (a window of 1 …
//                60 s, columns on an absolute bin grid: no shimmer) or Loop (locked to the tap's beat: 1 beat … 4
//                bars; no clock: a free-running loop of N seconds); auto gain, a dB scale, the time ruler / beat
//                rules, lane labels, the readout and a hover inspector.
//
// The analysis: the host binds the instance's feed (Face::setAnalysis). A shown face acquires its stage (kWantBands /
// kWantWave: the feed's worker splits / bins the tap, pq/dsp/band_levels.h, waveform.h) and sends its analysis options;
// it reads only what is new each poll (columns, bins, the loop slots written since its stamp); the hosts poll it at ≤
// 60 Hz while the feed moves and it stops once settled; the feed's waker (sound arrived) asks again. A face polled but
// no longer painted lets go after one unanswered repaint: hidden, it costs nothing.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "faces.h"
#include "analyser_face.h"
#include "option_sheet.h"
#include "pq/dsp/band_levels.h"
#include "pq/dsp/waveform.h"
#include "pq/meta/band_wave.h"
#include "pq/ui/colormap.h"
#include "pq/ui/lines.h"
#include "pq/ui/theme.h"

namespace pq::views {

namespace {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;
using sheet::kOnOff;
using sheet::Opt;

constexpr float kW = 300.0f, kH = 160.0f;
constexpr float kLabelPx = 6.0f;           // world units
constexpr float kLabelMinDevicePx = 5.0f;  // below this the text is left out
constexpr Rect kWell{6.0f, 6.0f, kW - 6.0f, kH - 6.0f};
constexpr Rgba kGrid = ui::signal_theme::kGrid, kGridLabel = ui::signal_theme::kGridLabel;
constexpr Rgba kFrozen = ui::rgba(255, 180, 90, 230);
constexpr Rgba kAccent = ui::rgba(120, 190, 255);

Rgba rgbF(float r, float g, float b) noexcept {
  auto q = [](float x) { return static_cast<uint32_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
  return ui::rgba(q(r), q(g), q(b));
}

// ---- the shared frame: VZ.5's AnalyserFace (the sheet, SET / FREEZE, the feed held while painted), adapted ----------

class StageFace : public analyser::AnalyserFace {
public:
  void setHoverForTest(Vec2 l) { hoverForTest(l); }

protected:
  StageFace(const meta::Type& t, std::span<const Opt> table, uint32_t want) : AnalyserFace(t, kW, kH, table), want_(want) {}

  virtual void sendRequestTo(devices::SpectrumFeed& f) const = 0;  // the stage's analysis options
  virtual void paintPlot(ui::FacePainter& p, const Rect& dev, const ui::Theme& th, bool labels) = 0;
  virtual bool readStage() = 0;  // copies what is new from feed_; true: changed
  virtual void restart() {}      // a new feed: the copies start over
  virtual bool moving() const noexcept { return false; }
  virtual const char* plotHelp() const noexcept = 0;

  // A text line at world (x, y) (left-middle anchored), `align` 0 left / 1 right.
  static void label(ui::FacePainter& p, Vec2 at, std::string_view s, Rgba col, float align = 0.0f) {
    p.textAligned(at, s, kLabelPx, col, align);
  }

private:
  uint32_t wantFlags() const noexcept override { return want_; }
  void sendRequest(devices::SpectrumFeed& f) const override { sendRequestTo(f); }
  bool readFeed(devices::SpectrumFeed& f, bool& settled) override {
    settled = f.settled();
    return readStage();
  }
  void feedChanged() override { restart(); }
  bool hasClear() const noexcept override { return false; }
  void paintBody(ui::FacePainter& p, const ui::Theme& th, bool labels) override { paintPlot(p, deviceRect(p, well()), th, labels); }
  int32_t ownHit(Vec2 l) const noexcept override { return analyser::inside(well(), l) ? analyser::kPlotPart : kNone; }
  ui::Cursor ownCursor(int32_t) const noexcept override { return ui::Cursor::Inherit; }
  bool ownDescribe(int32_t part, std::string& title, std::string& body) const override {
    if (part != analyser::kPlotPart) return false;
    title = std::string(type().name);
    body = plotHelp();
    return true;
  }
  bool animate(float, bool) override { return moving(); }

  uint32_t want_;
};

// ---- Band Levels (PQ multiband_rms) -------------------------------------------------------------------------------

// PQ's mbDbToNorm: linear in dB, or warped at the knee (the knee at `frac` of the height; below it a power curve).
float dbToNorm(float db, float floorDb, float ceilDb, bool warp, float knee, float frac, float exponent) noexcept {
  const float span = std::max(1.0f, ceilDb - floorDb);
  if (!warp) return std::clamp((db - floorDb) / span, 0.0f, 1.0f);
  const float k = std::clamp(knee, floorDb + 1.0f, ceilDb - 1.0f);
  const float fr = std::clamp(frac, 0.02f, 0.9f);
  if (db >= k) return fr + std::clamp((db - k) / std::max(1.0f, ceilDb - k), 0.0f, 1.0f) * (1.0f - fr);
  const float u = std::clamp((db - floorDb) / std::max(1.0f, k - floorDb), 0.0f, 1.0f);
  return std::pow(u, std::max(0.1f, exponent)) * fr;
}

constexpr const char* kBandCountNames[] = {"3 bands", "4 bands", "5 bands"};
constexpr const char* kSplitNames[] = {"Crossovers", "Independent"};
constexpr const char* kBandChannelNames[] = {"Mono (L+R)", "Left", "Right", "Mid", "Side"};
constexpr const char* kBallisticsNames[] = {"VU", "Peak", "Custom"};
enum BandOpt : uint32_t {
  bCount, bSplit, bLowHz, bMidHz, bHighHz, bWidth, bX3a, bX3b, bX4a, bX4b, bX4c, bX5a, bX5b, bX5c, bX5d, bChannel, bBal,
  bAttack, bRelease, bRate, bSpan, bFloor, bCeil, bWarp, bKnee, bFrac, bExp, bFill, bLine, bGrid, bLabels, bShow1, bShow2,
  bShow3, bShow4, bShow5, bCol1, bCol2, bCol3, bCol4, bCol5, bCountOpts
};
// PQ Meter's defaults: 3 bands as independent low / band / high (100 Hz, 550 Hz ± 1 octave, 5 kHz), its crossover sets
// per band count, mono, VU, 60 columns a second over 6 s, −72 … 0 dB warped at −45 dB (18 %, exponent 2.2).
const Opt kBandOpts[bCountOpts] = {
    {"bands", "Bands", 0, 0, 2, Opt::Choice, kBandCountNames},
    {"split", "Split (3 bands)", 1, 0, 1, Opt::Choice, kSplitNames},
    {"lo", "Low cutoff", 100, 20, 1000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"mid", "Mid centre", 550, 50, 10000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"hi", "High cutoff", 5000, 500, 18000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"width", "Mid width", 2, 0.25, 6, Opt::Number, nullptr, "%.2f oct", 0.05},
    {"x3a", "Crossover 1", 250, 20, 20000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"x3b", "Crossover 2", 4000, 20, 20000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"x4a", "Crossover 1", 120, 20, 20000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"x4b", "Crossover 2", 800, 20, 20000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"x4c", "Crossover 3", 5000, 20, 20000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"x5a", "Crossover 1", 60, 20, 20000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"x5b", "Crossover 2", 250, 20, 20000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"x5c", "Crossover 3", 2000, 20, 20000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"x5d", "Crossover 4", 8000, 20, 20000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"ch", "Channel", 0, 0, 4, Opt::Choice, kBandChannelNames},
    {"bal", "Ballistics", 0, 0, 2, Opt::Choice, kBallisticsNames},
    {"att", "Attack", 30, 1, 1000, Opt::LogNumber, nullptr, "%.0f ms"},
    {"rel", "Release", 300, 10, 3000, Opt::LogNumber, nullptr, "%.0f ms"},
    {"rate", "Resolution", 60, 10, 240, Opt::LogNumber, nullptr, "%.0f /s"},
    {"span", "History", 6, 1, 60, Opt::LogNumber, nullptr, "%.1f s"},
    {"floor", "Min", -72, -120, -20, Opt::Number, nullptr, "%.0f dB", 1.0},
    {"ceil", "Max", 0, -40, 12, Opt::Number, nullptr, "%.0f dB", 1.0},
    {"warp", "Warped scale", 1, 0, 1, Opt::Toggle, kOnOff},
    {"knee", "Warp knee", -45, -90, -6, Opt::Number, nullptr, "%.0f dB", 1.0},
    {"frac", "Warp strip", 0.18, 0.05, 0.5, Opt::Number, nullptr, "%.2f", 0.01},
    {"exp", "Warp curve", 2.2, 1.0, 4.0, Opt::Number, nullptr, "%.1f", 0.1},
    {"fill", "Fill", 0.28, 0.0, 1.0, Opt::Number, nullptr, "%.2f", 0.01},
    {"lw", "Line width", 1.8, 0.5, 4.0, Opt::Number, nullptr, "%.1f", 0.1},
    {"grid", "Grid", 1, 0, 1, Opt::Toggle, kOnOff},
    {"labels", "Labels", 1, 0, 1, Opt::Toggle, kOnOff},
    {"s1", "Show band 1", 1, 0, 1, Opt::Toggle, kOnOff},
    {"s2", "Show band 2", 1, 0, 1, Opt::Toggle, kOnOff},
    {"s3", "Show band 3", 1, 0, 1, Opt::Toggle, kOnOff},
    {"s4", "Show band 4", 1, 0, 1, Opt::Toggle, kOnOff},
    {"s5", "Show band 5", 1, 0, 1, Opt::Toggle, kOnOff},
    // VZ.10: PQ multiband_rms's per-band colours (over its ramp for the band count).
    sheet::colourOpt("c1", "Band 1"),
    sheet::colourOpt("c2", "Band 2"),
    sheet::colourOpt("c3", "Band 3"),
    sheet::colourOpt("c4", "Band 4"),
    sheet::colourOpt("c5", "Band 5"),
};

const char* const* bandNames(int n) noexcept {
  static const char* const k3[] = {"Low", "Mid", "High"};
  static const char* const k4[] = {"Low", "LoMid", "HiMid", "High"};
  static const char* const k5[] = {"Sub", "LoMid", "Mid", "High", "Ultra"};
  return n == 5 ? k5 : n == 4 ? k4 : k3;
}
// PQ's blue → amber ramp per band count.
Rgba bandColour(int n, int b) noexcept {
  static const Rgba k3[] = {rgbF(0.36f, 0.62f, 1.0f), rgbF(0.95f, 0.33f, 0.30f), rgbF(1.0f, 0.80f, 0.25f)};
  static const Rgba k4[] = {rgbF(0.36f, 0.62f, 1.0f), rgbF(0.45f, 0.85f, 0.70f), rgbF(0.95f, 0.33f, 0.30f), rgbF(1.0f, 0.80f, 0.25f)};
  static const Rgba k5[] = {rgbF(0.55f, 0.45f, 0.95f), rgbF(0.36f, 0.62f, 1.0f), rgbF(0.45f, 0.85f, 0.70f), rgbF(0.95f, 0.33f, 0.30f),
                            rgbF(1.0f, 0.80f, 0.25f)};
  return n == 5 ? k5[b] : n == 4 ? k4[b] : k3[b];
}

class BandLevelsFace final : public StageFace {
public:
  static constexpr int kBands = dsp::BandLevels::kBands;
  explicit BandLevelsFace(const meta::Type& t) : StageFace(t, kBandOpts, devices::kWantBands) {
    ring_.assign(static_cast<size_t>(dsp::BandLevels::kHistory) * kBands, dsp::BandLevels::kFloorDb);
  }

  dsp::BandLevelsConfig config() const {
    dsp::BandLevelsConfig c;
    c.bands = 3 + std::clamp(opts_.choice(bCount), 0, 2);
    c.independent3 = opts_.choice(bSplit) == 1;
    c.indepHz[0] = static_cast<float>(opts_[bLowHz]);
    c.indepHz[1] = static_cast<float>(opts_[bMidHz]);
    c.indepHz[2] = static_cast<float>(opts_[bHighHz]);
    c.indepOctaves = static_cast<float>(opts_[bWidth]);
    const uint32_t first = c.bands == 3 ? bX3a : c.bands == 4 ? bX4a : bX5a;
    for (int i = 0; i < c.bands - 1; ++i) c.crossovers[i] = static_cast<float>(opts_[first + static_cast<uint32_t>(i)]);
    c.channel = static_cast<dsp::BandLevelsConfig::Channel>(std::clamp(opts_.choice(bChannel), 0, 4));
    c.ballistics = static_cast<dsp::BandLevelsConfig::Ballistics>(std::clamp(opts_.choice(bBal), 0, 2));
    c.attackMs = static_cast<float>(opts_[bAttack]);
    c.releaseMs = static_cast<float>(opts_[bRelease]);
    c.columnRate = static_cast<float>(opts_[bRate]);
    return c;
  }
  uint64_t shownColumns() const noexcept { return read_; }
  // VZ.10: band b's colour (its own, else PQ's ramp for n bands).
  Rgba bandCol(int n, int b) const noexcept { return opts_.colour(bCol1 + static_cast<uint32_t>(b), bandColour(n, b)); }
  float shownLevel(int b) const noexcept { return read_ > 0 ? col(read_ - 1)[b] : dsp::BandLevels::kFloorDb; }

protected:
  void sendRequestTo(devices::SpectrumFeed& f) const override { f.setBands(config()); }
  void restart() override { read_ = 0; }
  const char* plotHelp() const noexcept override {
    return "Each band's RMS level over time (the newest at the right). Point at it for the time and every band's level "
           "there.";
  }
  void rows(int tab, std::vector<uint32_t>& out) const override {
    out.clear();
    if (tab == sheet::kAnalysis) {
      out.push_back(bCount);
      const int n = 3 + std::clamp(opts_.choice(bCount), 0, 2);
      if (n == 3) out.push_back(bSplit);
      if (n == 3 && opts_.choice(bSplit) == 1) {
        for (const uint32_t i : {bLowHz, bMidHz, bHighHz, bWidth}) out.push_back(i);
      } else {
        const uint32_t first = n == 3 ? bX3a : n == 4 ? bX4a : bX5a;
        for (int i = 0; i < n - 1; ++i) out.push_back(first + static_cast<uint32_t>(i));
      }
      out.push_back(bChannel);
      out.push_back(bBal);
      if (opts_.choice(bBal) == 2) {
        out.push_back(bAttack);
        out.push_back(bRelease);
      }
      out.push_back(bRate);
      out.push_back(bSpan);
      return;
    }
    if (tab == sheet::kColours) {
      const int n = 3 + std::clamp(opts_.choice(bCount), 0, 2);
      for (int b = 0; b < n; ++b) out.push_back(bCol1 + static_cast<uint32_t>(b));
      return;
    }
    for (const uint32_t i : {bFloor, bCeil, bWarp}) out.push_back(i);
    if (opts_.on(bWarp))
      for (const uint32_t i : {bKnee, bFrac, bExp}) out.push_back(i);
    for (const uint32_t i : {bFill, bLine, bGrid, bLabels}) out.push_back(i);
    const int n = 3 + std::clamp(opts_.choice(bCount), 0, 2);
    for (int b = 0; b < n; ++b) out.push_back(bShow1 + static_cast<uint32_t>(b));
  }
  bool readStage() override {
    const dsp::BandLevels* s = feed_->bands();
    if (s == nullptr) return false;
    const uint64_t end = s->columns();
    if (end == read_) return false;
    const uint64_t from = std::max(read_, end > dsp::BandLevels::kHistory ? end - dsp::BandLevels::kHistory : 0);
    // Straight into the ring (its slots are the stage's: column c at c % kHistory).
    uint64_t at = from;
    while (at < end) {
      const auto slot = static_cast<uint32_t>(at % dsp::BandLevels::kHistory);
      const auto n = static_cast<uint32_t>(std::min<uint64_t>(end - at, dsp::BandLevels::kHistory - slot));
      const uint32_t got = s->read(at, n, ring_.data() + static_cast<size_t>(slot) * kBands);
      if (got == 0) break;
      at += got;
    }
    read_ = at;
    return true;
  }

  void paintPlot(ui::FacePainter& p, const Rect& r, const ui::Theme&, bool labels) override {
    const float w = r.width(), h = r.height();
    const float sc = p.transform().scale;
    const int nBands = 3 + std::clamp(opts_.choice(bCount), 0, 2);
    const float floorDb = static_cast<float>(opts_[bFloor]);
    const float ceilDb = std::max(static_cast<float>(opts_[bCeil]), floorDb + 12.0f);
    const bool warp = opts_.on(bWarp);
    const auto knee = static_cast<float>(opts_[bKnee]), frac = static_cast<float>(opts_[bFrac]), ex = static_cast<float>(opts_[bExp]);
    const float rate = static_cast<float>(opts_[bRate]);
    const int want = std::max(2, static_cast<int>(std::lround(opts_[bSpan] * rate)));
    const int nCols = static_cast<int>(std::min<uint64_t>({static_cast<uint64_t>(want), read_, dsp::BandLevels::kHistory}));
    const float spanSec = static_cast<float>(nCols) / std::max(1.0f, rate);
    auto dbToY = [&](float db) { return r.y1 - dbToNorm(db, floorDb, ceilDb, warp, knee, frac, ex) * (h - 4.0f); };
    ui::DrawList& dl = p.drawList();
    const ui::FaceTransform& t = p.transform();
    auto world = [&](float x, float y) { return t.unmap({x, y}); };
    if (opts_.on(bGrid)) {
      // 6 dB rules from the ceiling down, thinned where they crowd (9 px apart), the warp's strip in 12 dB steps and the
      // floor always (PQ's rule walk).
      float lastY = 1e9f;
      auto rule = [&](float db, bool force) {
        const float y = std::round(dbToY(db));
        if (!force && std::fabs(y - lastY) < 9.0f * sc / 2.0f) return;
        lastY = y;
        dl.addRectFilled({r.x0, y, r.x1, y + 1.0f}, kGrid);
        if (labels && w >= 70.0f * sc / 2.0f) {
          char lb[16];
          std::snprintf(lb, sizeof lb, "%.0f", db);
          const Vec2 wl = world(r.x1 - 3.0f * sc, y);
          if (wl.y - 0.5f * kLabelPx > kWell.y0 + 1.0f && wl.y + 0.5f * kLabelPx < kWell.y1 - kLabelPx - 1.0f)
            label(p, wl, lb, kGridLabel, 1.0f);
        }
      };
      for (float db = std::floor(ceilDb / 6.0f) * 6.0f; db >= floorDb; db -= 6.0f) rule(db, false);
      if (warp) {
        for (float db = std::floor((knee - 12.0f) / 12.0f) * 12.0f; db > floorDb; db -= 12.0f) rule(db, false);
        rule(floorDb, true);
      }
      if (spanSec > 0.0f) {  // a rule a second, labelled where they are ~34 px apart
        const float pxPerSec = w / spanSec;
        int stride = 1;
        while (pxPerSec * static_cast<float>(stride) < 34.0f * sc / 2.0f && stride < static_cast<int>(spanSec) + 1) ++stride;
        for (int s = 1; s <= static_cast<int>(spanSec); ++s) {
          const float x = std::round(r.x1 - static_cast<float>(s) / spanSec * w);
          if (x < r.x0 + 1.0f) break;
          dl.addRectFilled({x, r.y0, x + 1.0f, r.y1}, kGrid);
          if (!labels || s % stride != 0) continue;
          char lb[16];
          std::snprintf(lb, sizeof lb, "-%ds", s);
          const Vec2 wl = world(x + 2.0f * sc, r.y1);
          if (wl.x + 3.0f * kLabelPx < kWell.x1 - 2.0f) label(p, {wl.x, kWell.y1 - 0.5f * kLabelPx - 1.5f}, lb, kGridLabel);
        }
      }
    }
    bool show[kBands];
    for (int b = 0; b < kBands; ++b) show[b] = b < nBands && opts_.on(bShow1 + static_cast<uint32_t>(b));
    if (nCols >= 2) {
      const uint64_t first = read_ - static_cast<uint64_t>(nCols);
      auto xAt = [&](int i) { return r.x0 + static_cast<float>(i) / static_cast<float>(nCols - 1) * w; };
      const float fillA = static_cast<float>(opts_[bFill]);
      // Fills first (per-column quads: no triangulation to fail on a comb of notches), back to front.
      if (fillA > 0.001f) {
        for (int b = 0; b < nBands; ++b) {
          if (!show[b]) continue;
          const Rgba c = ui::setAlpha(bandCol(nBands, b), fillA);
          const uint32_t segs = static_cast<uint32_t>(nCols - 1);
          const ui::DrawList::PrimSpan sp = dl.primReserveSpan(segs * 6, segs * 4);
          const Vec2 uv = dl.whiteUv();
          ui::Vertex* v = sp.v;
          uint32_t* ix = sp.i;
          for (int i = 0; i + 1 < nCols; ++i) {
            const float y0 = dbToY(col(first + static_cast<uint64_t>(i))[b]), y1 = dbToY(col(first + static_cast<uint64_t>(i) + 1)[b]);
            const float x0 = xAt(i), x1 = xAt(i + 1);
            const uint32_t base = sp.base + static_cast<uint32_t>(i) * 4;
            v[0] = {{x0, y0}, uv, c};
            v[1] = {{x1, y1}, uv, c};
            v[2] = {{x1, r.y1}, uv, c};
            v[3] = {{x0, r.y1}, uv, c};
            ix[0] = base;
            ix[1] = base + 1;
            ix[2] = base + 2;
            ix[3] = base;
            ix[4] = base + 2;
            ix[5] = base + 3;
            v += 4;
            ix += 6;
          }
        }
      }
      // Then every line on top, broken where a band is silent (no trace along the floor).
      const float lw = std::max(0.5f, static_cast<float>(opts_[bLine]) * sc * 0.5f);
      for (int b = 0; b < nBands; ++b) {
        if (!show[b]) continue;
        const Rgba c = bandCol(nBands, b);
        pts_.clear();
        auto flush = [&] {
          if (pts_.size() == 1) dl.addRectFilled({pts_[0].x - lw * 0.5f, pts_[0].y - lw * 0.5f, pts_[0].x + lw * 0.5f, pts_[0].y + lw * 0.5f}, c);
          else if (pts_.size() >= 2) ui::smoothPolyline(dl, pts_.data(), static_cast<uint32_t>(pts_.size()), c, lw);
          pts_.clear();
        };
        for (int i = 0; i < nCols; ++i) {
          const float y = dbToY(col(first + static_cast<uint64_t>(i))[b]);
          if (y >= r.y1 - 0.5f) flush();
          else pts_.push_back({xAt(i), y});
        }
        flush();
      }
    }
    if (labels && opts_.on(bLabels)) {  // the current level per band, top left, in its colour
      const char* const* names = bandNames(nBands);
      float ty = kWell.y0 + 4.0f + 0.5f * kLabelPx;
      for (int b = 0; b < nBands; ++b) {
        if (!show[b]) continue;
        const float cur = shownLevel(b);
        char buf[40];
        if (cur <= dsp::BandLevels::kFloorDb + 1.0f) std::snprintf(buf, sizeof buf, "%-5s  -inf", names[b]);
        else std::snprintf(buf, sizeof buf, "%-5s %6.1f dB", names[b], static_cast<double>(cur));
        label(p, {kWell.x0 + 4.0f, ty}, buf, bandCol(nBands, b));
        ty += kLabelPx + 2.0f;
      }
    }
    if (frozen_ && labels) label(p, {setChip().x0 - 32.0f, 15.0f}, "FROZEN", kFrozen);
    // Hover: the time there and each band's level.
    if (hover_.x >= 0.0f && nCols >= 2 && labels) {
      const float tx = std::clamp((hover_.x - kWell.x0) / (kWell.x1 - kWell.x0), 0.0f, 1.0f);
      const int i = std::clamp(static_cast<int>(std::lround(tx * static_cast<float>(nCols - 1))), 0, nCols - 1);
      const float x = t.map({hover_.x, 0.0f}).x;
      dl.addRectFilled({std::round(x), r.y0, std::round(x) + 1.0f, r.y1}, ui::rgba(255, 255, 255, 60));
      const float* c = col(read_ - static_cast<uint64_t>(nCols) + static_cast<uint64_t>(i));
      const float lx = hover_.x > kWell.x1 - 70.0f ? hover_.x - 66.0f : hover_.x + 5.0f;
      float ty = kWell.y0 + 8.0f + (opts_.on(bLabels) ? static_cast<float>(nBands) * (kLabelPx + 2.0f) : 0.0f);
      char buf[40];
      std::snprintf(buf, sizeof buf, "-%.2f s", static_cast<double>((1.0f - tx) * spanSec));
      label(p, {lx, ty}, buf, ui::rgba(255, 255, 255, 210));
      ty += kLabelPx + 2.0f;
      const char* const* names = bandNames(nBands);
      for (int b = 0; b < nBands; ++b) {
        if (!show[b]) continue;
        std::snprintf(buf, sizeof buf, "%-5s %6.1f", names[b], static_cast<double>(c[b]));
        label(p, {lx, ty}, buf, bandCol(nBands, b));
        ty += kLabelPx + 2.0f;
      }
    }
  }

private:
  const float* col(uint64_t c) const noexcept { return ring_.data() + static_cast<size_t>(c % dsp::BandLevels::kHistory) * kBands; }
  std::vector<float> ring_;  // the stage's columns, column c at c % kHistory
  uint64_t read_ = 0;        // columns copied so far
  std::vector<Vec2> pts_;
};

// ---- the Waveform (PQ waveform) -------------------------------------------------------------------------------------

constexpr const char* kPlayheadNames[] = {"Scroll", "Loop"};
constexpr const char* kLoopNames[] = {"1 beat", "2 beats", "1 bar", "2 bars", "4 bars"};
constexpr double kLoopBeats[] = {1.0, 2.0, 4.0, 8.0, 16.0};
constexpr const char* kViewNames[] = {"Mid / Side", "Left / Right", "Mono", "Overlay L+R"};
constexpr const char* kColourNames[] = {"Theme", "Level heat", "Brightness"};
constexpr const char* kMapNames[] = {"Magma", "Inferno", "Viridis", "Grayscale", "Ice", "Gold", "Silver", "Pastel", "Neon", "Copper"};
enum WaveOpt : uint32_t {
  wPlayhead, wWindow, wLoop, wLoopSec, wView, wSwap, wGain, wAuto, wDb, wColour, wMap, wSmooth, wBody, wFill, wOutline, wGlow,
  wFade, wGrid, wRuler, wLanes, wReadout, wClip, wClipDb, wColA, wColB, wColClip, wCountOpts
};
// PQ Meter's defaults: Scroll over 10 s, the loop 1 bar (2 s free run), mid / side, gain ×1, brightness through Magma
// smoothed 0.5, the RMS body, fill 0.4, the outline, glow 0.4, the edge fade, grid, ruler, lane labels and the readout.
const Opt kWaveOpts[wCountOpts] = {
    {"ph", "Playhead", 0, 0, 1, Opt::Choice, kPlayheadNames},
    {"win", "Window", 10, 1, 60, Opt::LogNumber, nullptr, "%.1f s"},
    {"loop", "Loop span", 2, 0, 4, Opt::Choice, kLoopNames},
    {"loops", "Free-run loop", 2, 0.25, 10, Opt::LogNumber, nullptr, "%.2f s"},
    {"view", "Lanes", 0, 0, 3, Opt::Choice, kViewNames},
    {"swap", "Swap lanes", 0, 0, 1, Opt::Toggle, kOnOff},
    {"gain", "Gain", 1, 0.25, 24, Opt::LogNumber, nullptr, "x%.2f"},
    {"auto", "Auto gain", 0, 0, 1, Opt::Toggle, kOnOff},
    {"db", "dB scale", 0, 0, 1, Opt::Toggle, kOnOff},
    {"col", "Colour by", 2, 0, 2, Opt::Choice, kColourNames},
    {"map", "Colormap", 0, 0, 9, Opt::Choice, kMapNames},
    {"smooth", "Colour smoothing", 0.5, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"body", "RMS body", 1, 0, 1, Opt::Toggle, kOnOff},
    {"fill", "Fill", 0.4, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"outline", "Outline", 1, 0, 1, Opt::Toggle, kOnOff},
    {"glow", "Glow", 0.4, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"fade", "Edge fade", 1, 0, 1, Opt::Toggle, kOnOff},
    {"grid", "Grid", 1, 0, 1, Opt::Toggle, kOnOff},
    {"ruler", "Time ruler", 1, 0, 1, Opt::Toggle, kOnOff},
    {"lanes", "Lane labels", 1, 0, 1, Opt::Toggle, kOnOff},
    {"ro", "Readout", 1, 0, 1, Opt::Toggle, kOnOff},
    {"clip", "Clip marks", 0, 0, 1, Opt::Toggle, kOnOff},
    {"clipdb", "Clip level", -0.1, -12, 0, Opt::Number, nullptr, "%.1f dB", 0.1},
    // VZ.10: PQ waveform's lane colours (Colour by Theme) and its clip colour.
    sheet::colourOpt("ca", "First lane"),
    sheet::colourOpt("cb", "Second lane"),
    sheet::colourOpt("cclip", "Clip colour"),
};
constexpr Rgba kLaneA = ui::rgba(120, 219, 161), kLaneB = ui::rgba(255, 150, 120);  // PQ Theme::waveform / waveformR
constexpr Rgba kClip = ui::rgba(255, 56, 56);
constexpr int kMaxCols = 720;

struct ColAgg {
  float mn = 0.0f, mx = 0.0f, rms = 0.0f, hiRatio = 0.0f;
  bool valid = false;
};
struct Fold {
  double ss = 0.0, hs = 0.0, n = 0.0;
  void add(ColAgg& d, const dsp::WaveBin& b, int ch) noexcept {
    if (b.n == 0) return;
    if (!d.valid) {
      d.mn = b.mn[ch];
      d.mx = b.mx[ch];
      d.valid = true;
    } else {
      d.mn = std::min(d.mn, b.mn[ch]);
      d.mx = std::max(d.mx, b.mx[ch]);
    }
    ss += b.ss[ch];
    hs += b.hs[ch];
    n += b.n;
  }
  void finish(ColAgg& d) const noexcept {
    if (n <= 0.0) return;
    d.rms = static_cast<float>(std::sqrt(ss / n));
    d.hiRatio = ss > 1e-12 * n ? static_cast<float>(std::clamp(hs / ss, 0.0, 1.0)) : 0.0f;
  }
};

// A vertical quad strip with shared rows (PQ fillStrip), optional fringe rows fading to alpha 0.
void fillStrip(ui::DrawList& dl, const float* xs, const float* yTop, const float* yBot, const Rgba* colTop, const Rgba* colBot, int n,
               float featherTop = 0.0f, float featherBot = 0.0f) {
  if (n < 2) return;
  const bool fT = featherTop > 0.0f, fB = featherBot > 0.0f;
  const uint32_t rows = 2u + (fT ? 1u : 0u) + (fB ? 1u : 0u);
  const ui::DrawList::PrimSpan sp = dl.primReserveSpan(static_cast<uint32_t>(n - 1) * 6 * (rows - 1), static_cast<uint32_t>(n) * rows);
  const Vec2 uv = dl.whiteUv();
  ui::Vertex* v = sp.v;
  for (int i = 0; i < n; ++i) {
    if (fT) *v++ = {{xs[i], yTop[i] - featherTop}, uv, colTop[i] & 0x00FFFFFFu};
    *v++ = {{xs[i], yTop[i]}, uv, colTop[i]};
    *v++ = {{xs[i], yBot[i]}, uv, colBot[i]};
    if (fB) *v++ = {{xs[i], yBot[i] + featherBot}, uv, colBot[i] & 0x00FFFFFFu};
  }
  uint32_t* ix = sp.i;
  for (int i = 0; i + 1 < n; ++i)
    for (uint32_t r = 0; r + 1 < rows; ++r) {
      const uint32_t b = sp.base + static_cast<uint32_t>(i) * rows + r;
      ix[0] = b;
      ix[1] = b + rows;
      ix[2] = b + rows + 1;
      ix[3] = b;
      ix[4] = b + rows + 1;
      ix[5] = b + 1;
      ix += 6;
    }
}

class WaveformFace final : public StageFace {
public:
  explicit WaveformFace(const meta::Type& t) : StageFace(t, kWaveOpts, devices::kWantWave) {
    slots_.resize(dsp::WaveCapture::kLoopSlots);
    filled_.assign(dsp::WaveCapture::kLoopSlots, 0);
  }

  dsp::WaveConfig config() const {
    dsp::WaveConfig c;
    c.loop = opts_.choice(wPlayhead) == 1;
    c.loopBeats = kLoopBeats[std::clamp(opts_.choice(wLoop), 0, 4)];
    c.loopSeconds = static_cast<float>(opts_[wLoopSec]);
    return c;
  }
  const dsp::WaveLoopState& loopState() const noexcept { return loop_; }
  // Settled once a window (Loop: the free span, at least 8 s: a clocked bar of silence) has been silent.
  float restSeconds() const noexcept {
    const dsp::WaveConfig c = config();
    return c.loop ? std::max(8.0f, c.loopSeconds) : static_cast<float>(opts_[wWindow]);
  }
  uint64_t shownBins() const noexcept { return binsRead_; }
  // Tests: the columns of channel ch (0 L, 1 R, 2 M, 3 S) as the face would draw them `cols` wide.
  void columnsFor(int cols, int ch, std::vector<ColAgg>& out) { snapshot(cols, ch, -1, out, other_); }

protected:
  void sendRequestTo(devices::SpectrumFeed& f) const override { f.setWave(config(), restSeconds()); }
  void restart() override {
    binsRead_ = 0;
    stamp_ = 0;
    std::fill(filled_.begin(), filled_.end(), uint8_t{0});
  }
  bool moving() const noexcept override { return opts_.choice(wPlayhead) == 1 && loop_.moving; }
  // The auto gain eases toward the window's peak with a 0.2 s time constant (held while frozen).
  bool animate(float dt, bool) override {
    bool easing = false;
    if (opts_.on(wAuto) && !frozen_ && gainPrimed_) {
      const float d = targetGain_ - autoGain_;
      if (std::fabs(d) > 0.005f * targetGain_) {
        autoGain_ += d * (1.0f - std::exp(-dt / 0.2f));
        easing = true;
      } else {
        autoGain_ = targetGain_;
      }
    }
    return moving() || easing;
  }
  const char* plotHelp() const noexcept override {
    return "The signal's envelope (peak, with its RMS body inside). Point at it for the time and the levels there.";
  }
  void rows(int tab, std::vector<uint32_t>& out) const override {
    out.clear();
    if (tab == sheet::kAnalysis) {
      out.push_back(wPlayhead);
      if (opts_.choice(wPlayhead) == 1) {
        out.push_back(wLoop);
        out.push_back(wLoopSec);
      } else {
        out.push_back(wWindow);
      }
      return;
    }
    if (tab == sheet::kColours) {  // (the lanes' colours paint the trace under Colour by Theme, and the lane labels)
      for (const uint32_t i : {wColour, wColA, wColB, wColClip}) out.push_back(i);
      return;
    }
    for (const uint32_t i : {wView, wSwap, wGain, wAuto, wDb, wColour}) out.push_back(i);
    if (opts_.choice(wColour) != 0) {
      out.push_back(wMap);
      out.push_back(wSmooth);
    }
    for (const uint32_t i : {wBody, wFill, wOutline, wGlow, wFade, wGrid, wRuler, wLanes, wReadout, wClip}) out.push_back(i);
    if (opts_.on(wClip)) out.push_back(wClipDb);
  }
  bool readStage() override {
    const dsp::WaveCapture* s = feed_->wave();
    if (s == nullptr) return false;
    bool changed = false;
    // The scroll's bins: only the new ones, straight into the face's ring (the stage's slot layout).
    const uint64_t end = s->bins();
    if (bins_.size() != dsp::WaveCapture::kRingBins) bins_.resize(dsp::WaveCapture::kRingBins);
    {
      const uint64_t from = std::max(binsRead_, end > dsp::WaveCapture::kRingBins ? end - dsp::WaveCapture::kRingBins : 0);
      uint64_t at = from;
      while (at < end) {
        const auto slot = static_cast<uint32_t>(at % dsp::WaveCapture::kRingBins);
        const auto n = static_cast<uint32_t>(std::min<uint64_t>(end - at, dsp::WaveCapture::kRingBins - slot));
        const uint32_t got = s->readBins(at, n, bins_.data() + slot, &partial_);
        if (got == 0) break;
        at += got;
      }
      if (at == from) (void)s->readBins(end, 0, nullptr, &partial_);  // the partial bin (the live right edge)
      changed = at != binsRead_ || partial_.n != lastPartialN_;
      lastPartialN_ = partial_.n;
      binsRead_ = at;
    }
    if (opts_.choice(wPlayhead) == 1) {
      const uint64_t stamp = s->readLoop(stamp_, slots_.data(), filled_.data(), loop_);
      changed = changed || stamp != stamp_;
      stamp_ = stamp;
    }
    return changed;
  }

  void paintPlot(ui::FacePainter& p, const Rect& r, const ui::Theme&, bool labels) override {
    laneA_ = opts_.colour(wColA, kLaneA), laneB_ = opts_.colour(wColB, kLaneB), clip_ = opts_.colour(wColClip, kClip);
    const float w = r.width(), h = r.height();
    const float sc = p.transform().scale;
    ui::DrawList& dl = p.drawList();
    const ui::FaceTransform& t = p.transform();
    const bool loopMode = opts_.choice(wPlayhead) == 1;
    const int view = std::clamp(opts_.choice(wView), 0, 3);
    int chA = 2, chB = -1, lanes = 1;
    bool overlay = false;
    switch (view) {
      case 0: chA = 2; chB = 3; lanes = 2; break;
      case 1: chA = 0; chB = 1; lanes = 2; break;
      case 3: chA = 0; chB = 1; overlay = true; break;
      default: break;
    }
    const int cols = std::clamp(static_cast<int>(w), 2, kMaxCols);
    if (loopMode) loopColumns(cols, chA, chB, colA_, colB_);
    else snapshot(cols, chA, chB, colA_, colB_);
    float gain = static_cast<float>(opts_[wGain]);
    if (opts_.on(wAuto)) {  // the window's peak to 0.92, eased (held while frozen)
      float peak = 1e-4f;
      for (int c = 0; c < cols; ++c) {
        if (colA_[static_cast<size_t>(c)].valid)
          peak = std::max({peak, std::fabs(colA_[static_cast<size_t>(c)].mn), std::fabs(colA_[static_cast<size_t>(c)].mx)});
        if (chB >= 0 && colB_[static_cast<size_t>(c)].valid)
          peak = std::max({peak, std::fabs(colB_[static_cast<size_t>(c)].mn), std::fabs(colB_[static_cast<size_t>(c)].mx)});
      }
      // The target here; the easing in animate (time-based, not per paint: L12).
      targetGain_ = std::clamp(0.92f / peak, 0.25f, 24.0f);
      if (!gainPrimed_) autoGain_ = targetGain_, gainPrimed_ = true;
      gain = autoGain_;
    }
    const bool db = opts_.on(wDb);
    auto valueToT = [&](float v) {
      if (db) {
        const float a = std::fabs(v) * gain;
        const float d = a > 1e-6f ? 20.0f * std::log10(a) : -120.0f;
        return std::clamp((d + 60.0f) / 60.0f, 0.0f, 1.0f) * (v < 0.0f ? -1.0f : 1.0f);
      }
      return std::clamp(v * gain, -1.0f, 1.0f);
    };
    const float laneH = h / static_cast<float>(lanes);
    auto laneMid = [&](int lane) { return r.y0 + laneH * (static_cast<float>(lane) + 0.5f); };
    if (opts_.on(wGrid))
      for (int lane = 0; lane < lanes; ++lane) {
        const float midY = std::round(laneMid(lane));
        dl.addRectFilled({r.x0, midY, r.x1, midY + 1.0f}, kGridLabel);
        for (int s = -1; s <= 1; s += 2) {
          const float y = std::round(midY - static_cast<float>(s) * (laneH * 0.5f - 2.0f) * 0.5f);
          dl.addRectFilled({r.x0, y, r.x1, y + 1.0f}, kGrid);
        }
      }
    const bool clipOn = opts_.on(wClip);
    const float clipLin = std::pow(10.0f, static_cast<float>(opts_[wClipDb]) / 20.0f);
    if (clipOn) {
      const float tc = valueToT(clipLin);
      if (tc < 0.999f)
        for (int lane = 0; lane < lanes; ++lane)
          for (int s = -1; s <= 1; s += 2) {
            const float y = std::round(laneMid(lane) - static_cast<float>(s) * tc * (laneH * 0.5f - 2.0f));
            dl.addRectFilled({r.x0, y, r.x1, y + 1.0f}, ui::setAlpha(clip_, 0.3f));
          }
    }
    const float windowSec = static_cast<float>(opts_[wWindow]);
    const double bpm = loop_.bpm;
    const bool tempoKnown = loop_.clocked && bpm > 20.0 && bpm < 999.0;
    if (opts_.on(wRuler)) {
      if (!loopMode) {
        float step = 1.0f;
        for (const float s : {1.0f, 2.0f, 5.0f, 10.0f, 15.0f, 30.0f}) {
          step = s;
          if (w * s / windowSec >= 70.0f * sc / 2.0f) break;
        }
        for (float s = step; s < windowSec - 0.01f; s += step) {
          const float x = std::round(r.x1 - s / windowSec * w);
          dl.addRectFilled({x, r.y0, x + 1.0f, r.y1}, kGrid);
          if (!labels) continue;
          char lb[16];
          std::snprintf(lb, sizeof lb, "-%.0fs", static_cast<double>(s));
          const Vec2 wl = t.unmap({x, r.y1});
          if (wl.x > kWell.x0 + 60.0f && wl.x < kWell.x1 - 10.0f)
            p.textAligned({wl.x, kWell.y1 - 0.5f * kLabelPx - 1.5f}, lb, kLabelPx, kGridLabel, 0.5f);
        }
      } else {
        const int beats = loop_.clocked ? static_cast<int>(kLoopBeats[std::clamp(opts_.choice(wLoop), 0, 4)]) : 0;
        if (beats > 1)
          for (int bt = 1; bt < beats; ++bt) {
            const float x = std::round(r.x0 + static_cast<float>(bt) / static_cast<float>(beats) * w);
            dl.addRectFilled({x, r.y0, x + 1.0f, r.y1}, beats >= 4 && bt % 4 == 0 ? ui::mulAlpha(kGridLabel, 0.45f) : kGrid);
          }
        else if (beats == 0)
          for (int d = 1; d < 8; ++d) {
            const float x = std::round(r.x0 + static_cast<float>(d) / 8.0f * w);
            dl.addRectFilled({x, r.y0, x + 1.0f, r.y1}, d == 4 ? ui::mulAlpha(kGridLabel, 0.35f) : kGrid);
          }
      }
    }
    const int colour = std::clamp(opts_.choice(wColour), 0, 2);
    const ui::ColorLut& lut = ui::colormapLut(static_cast<ui::Colormap>(std::clamp(opts_.choice(wMap), 0, ui::kColormapCount - 1)));
    const bool fade = opts_.on(wFade) && !loopMode;
    xs_.resize(static_cast<size_t>(cols));
    for (int c = 0; c < cols; ++c) xs_[static_cast<size_t>(c)] = r.x0 + (static_cast<float>(c) + 0.5f) / static_cast<float>(cols) * w;
    const float glowAmt = static_cast<float>(opts_[wGlow]);
    const float fillA = static_cast<float>(opts_[wFill]);
    const float feather = 0.75f;
    auto drawTrace = [&](const std::vector<ColAgg>& col, int lane, Rgba laneCol, float alphaScale) {
      const float mid = laneMid(lane), amp = laneH * 0.5f - 2.0f;
      auto Y = [&](float v) { return mid - v * amp; };
      tMin_.resize(static_cast<size_t>(cols));
      tMax_.resize(static_cast<size_t>(cols));
      tRms_.resize(static_cast<size_t>(cols));
      aCol_.resize(static_cast<size_t>(cols));
      for (int c = 0; c < cols; ++c) {
        const auto i = static_cast<size_t>(c);
        const ColAgg& a = col[i];
        tMin_[i] = a.valid ? valueToT(a.mn) : 0.0f;
        tMax_[i] = a.valid ? valueToT(a.mx) : 0.0f;
        if (tMin_[i] > tMax_[i]) std::swap(tMin_[i], tMax_[i]);
        const float rr = a.valid ? std::fabs(valueToT(a.rms)) : 0.0f;
        tRms_[i] = std::min(rr, std::max(std::fabs(tMin_[i]), std::fabs(tMax_[i])));
        float e = 1.0f;
        if (fade) {
          const float u = static_cast<float>(c) / std::max(1.0f, static_cast<float>(cols) * 0.10f);
          e = u >= 1.0f ? 1.0f : 0.12f + 0.88f * u * u;
        }
        aCol_[i] = (a.valid ? 1.0f : 0.0f) * e * alphaScale;
      }
      // Dilate the extremes by their neighbours (PQ: no comb of single-column spikes), smooth the RMS.
      tMaxD_.resize(static_cast<size_t>(cols));
      tMinD_.resize(static_cast<size_t>(cols));
      tRmsS_.resize(static_cast<size_t>(cols));
      for (int c = 0; c < cols; ++c) {
        const auto i = static_cast<size_t>(c), m = static_cast<size_t>(c > 0 ? c - 1 : c), q = static_cast<size_t>(c + 1 < cols ? c + 1 : c);
        tMaxD_[i] = std::min(1.0f, std::max(tMax_[i], 0.5f * (tMax_[m] + tMax_[q])));
        tMinD_[i] = std::max(-1.0f, std::min(tMin_[i], 0.5f * (tMin_[m] + tMin_[q])));
        tRmsS_[i] = 0.25f * tRms_[m] + 0.5f * tRms_[i] + 0.25f * tRms_[q];
      }
      cA_.resize(static_cast<size_t>(cols));
      cB_.resize(static_cast<size_t>(cols));
      zero_.resize(static_cast<size_t>(cols));
      if (colour == 0) {
        std::fill(cA_.begin(), cA_.end(), laneCol);
      } else {  // level heat / brightness through the colormap, smoothed weighted by energy
        tRaw_.resize(static_cast<size_t>(cols));
        wRaw_.resize(static_cast<size_t>(cols));
        for (int c = 0; c < cols; ++c) {
          const ColAgg& a = col[static_cast<size_t>(c)];
          float tv = 0.0f;
          if (a.valid) {
            if (colour == 1) {
              const float pk = std::max(std::fabs(a.mn), std::fabs(a.mx));
              const float d = pk > 1e-6f ? 20.0f * std::log10(pk) : -120.0f;
              tv = std::clamp((d + 60.0f) / 60.0f, 0.0f, 1.0f);
            } else {
              tv = std::clamp(std::sqrt(a.hiRatio) * 1.35f, 0.0f, 1.0f);
            }
          }
          tRaw_[static_cast<size_t>(c)] = tv;
          wRaw_[static_cast<size_t>(c)] = a.valid ? std::max(1e-4f, a.rms) : 1e-4f;
        }
        const int rad = static_cast<int>(static_cast<float>(opts_[wSmooth]) * 0.02f * static_cast<float>(cols) + 0.5f);
        for (int c = 0; c < cols; ++c) {
          float acc = 0.0f, ws = 0.0f;
          for (int k = std::max(0, c - rad); k <= std::min(cols - 1, c + rad); ++k) {
            const float kw = (1.0f - static_cast<float>(std::abs(k - c)) / static_cast<float>(rad + 1)) * wRaw_[static_cast<size_t>(k)];
            acc += tRaw_[static_cast<size_t>(k)] * kw;
            ws += kw;
          }
          cA_[static_cast<size_t>(c)] = ui::lutAt(lut, ws > 0.0f ? acc / ws : tRaw_[static_cast<size_t>(c)]);
        }
      }
      if (clipOn)
        for (int c = 0; c < cols; ++c) {
          const ColAgg& a = col[static_cast<size_t>(c)];
          if (a.valid && std::max(std::fabs(a.mn), std::fabs(a.mx)) >= clipLin) cA_[static_cast<size_t>(c)] = clip_;
        }
      yA_.resize(static_cast<size_t>(cols));
      yB_.resize(static_cast<size_t>(cols));
      if (glowAmt > 0.01f) {  // a soft halo above the peaks and below the troughs
        const float reach = 0.06f + 0.22f * glowAmt;
        for (int c = 0; c < cols; ++c) {
          const auto i = static_cast<size_t>(c);
          zero_[i] = ui::setAlpha(cA_[i], 0.0f);
          cB_[i] = ui::setAlpha(cA_[i], 0.16f * glowAmt * aCol_[i]);
          yA_[i] = Y(std::min(1.0f, tMaxD_[i] + reach));
          yB_[i] = Y(tMaxD_[i]);
        }
        fillStrip(dl, xs_.data(), yA_.data(), yB_.data(), zero_.data(), cB_.data(), cols);
        for (int c = 0; c < cols; ++c) {
          const auto i = static_cast<size_t>(c);
          yA_[i] = Y(tMinD_[i]);
          yB_[i] = Y(std::max(-1.0f, tMinD_[i] - reach));
        }
        fillStrip(dl, xs_.data(), yA_.data(), yB_.data(), cB_.data(), zero_.data(), cols);
      }
      for (int c = 0; c < cols; ++c) {  // the peak envelope
        const auto i = static_cast<size_t>(c);
        cB_[i] = ui::setAlpha(cA_[i], fillA * aCol_[i]);
        yA_[i] = Y(tMaxD_[i]);
        yB_[i] = Y(tMinD_[i]);
      }
      fillStrip(dl, xs_.data(), yA_.data(), yB_.data(), cB_.data(), cB_.data(), cols, feather, feather);
      if (opts_.on(wBody)) {  // the RMS body inside it
        for (int c = 0; c < cols; ++c) {
          const auto i = static_cast<size_t>(c);
          yA_[i] = Y(std::clamp(tRmsS_[i], tMinD_[i], tMaxD_[i]));
          yB_[i] = Y(std::clamp(-tRmsS_[i], tMinD_[i], tMaxD_[i]));
          cB_[i] = ui::setAlpha(cA_[i], 0.92f * aCol_[i]);
        }
        fillStrip(dl, xs_.data(), yA_.data(), yB_.data(), cB_.data(), cB_.data(), cols, feather, feather);
      }
      if (opts_.on(wOutline)) {  // the caps: the extremes traced, brightened
        caps_.resize(static_cast<size_t>(cols));
        for (int c = 0; c < cols; ++c) cB_[static_cast<size_t>(c)] = ui::setAlpha(ui::brighten(cA_[static_cast<size_t>(c)], 0.30f), 0.70f * aCol_[static_cast<size_t>(c)]);
        for (int pass = 0; pass < 2; ++pass) {
          const std::vector<float>& tv = pass == 0 ? tMaxD_ : tMinD_;
          for (int c = 0; c < cols; ++c) caps_[static_cast<size_t>(c)] = {xs_[static_cast<size_t>(c)], Y(tv[static_cast<size_t>(c)])};
          ui::smoothPolylineCols(dl, caps_.data(), cB_.data(), static_cast<uint32_t>(cols), std::max(1.0f, 0.5f * sc));
        }
      }
    };
    const bool canSwap = lanes == 2 && chB >= 0;
    const int laneA = canSwap && opts_.on(wSwap) ? 1 : 0, laneB = canSwap ? 1 - laneA : 0;
    drawTrace(colA_, laneA, laneA_, overlay ? 0.72f : 1.0f);
    if (chB >= 0) drawTrace(colB_, laneB, laneB_, overlay ? 0.72f : 1.0f);
    // The playhead: Loop's sweeping line (dim while the clock stands), Scroll's live right edge.
    if (loopMode) {
      if (loop_.phase >= 0.0) {
        const float x = std::round(r.x0 + static_cast<float>(loop_.phase) * w);
        dl.addRectFilled({x, r.y0, x + 1.0f, r.y1}, ui::setAlpha(kAccent, loop_.moving && !frozen_ ? 0.85f : 0.30f));
      }
    } else {
      dl.addRectFilled({r.x1 - 1.0f, r.y0, r.x1, r.y1}, ui::setAlpha(kAccent, frozen_ ? 0.10f : 0.25f));
    }
    if (!labels) return;
    const char* nameA = view == 0 ? "MID" : view == 2 ? "MONO" : "L";
    const char* nameB = view == 0 ? "SIDE" : "R";
    if (opts_.on(wLanes)) {
      auto laneLabelY = [&](int lane) { return lane == 0 ? kWell.y0 + 4.0f + 0.5f * kLabelPx : kWell.y0 + (kWell.y1 - kWell.y0) * 0.5f + 4.0f + 0.5f * kLabelPx; };
      label(p, {kWell.x0 + 4.0f, laneLabelY(laneA)}, nameA, ui::mulAlpha(laneA_, 0.8f));
      if (chB >= 0) label(p, {kWell.x0 + (overlay ? 16.0f : 4.0f), laneLabelY(overlay ? laneA : laneB)}, nameB, ui::mulAlpha(laneB_, 0.8f));
    }
    if (opts_.on(wReadout)) {
      char info[96];
      if (loopMode) {
        const double spanSec = loop_.spanSamples / std::max(1.0, sampleRate());
        if (tempoKnown)
          std::snprintf(info, sizeof info, "%s  %.1f bpm  %.0f ms%s", kLoopNames[std::clamp(opts_.choice(wLoop), 0, 4)], bpm,
                        spanSec * 1000.0, loop_.moving ? "" : "  paused");
        else std::snprintf(info, sizeof info, "%.2f s loop  free run", static_cast<double>(opts_[wLoopSec]));
      } else {
        std::snprintf(info, sizeof info, "%.0f s  x%.2f%s", static_cast<double>(windowSec), static_cast<double>(gain), opts_.on(wAuto) ? " auto" : "");
      }
      label(p, {kWell.x0 + 4.0f, kWell.y1 - 0.5f * kLabelPx - 1.5f}, info, kGridLabel);
    }
    if (frozen_) label(p, {setChip().x0 - 32.0f, 15.0f}, "FROZEN", kFrozen);
    // Hover: the time (or the beat) and each lane's peak / RMS there.
    if (hover_.x >= 0.0f) {
      const int c = std::clamp(static_cast<int>((hover_.x - kWell.x0) / (kWell.x1 - kWell.x0) * static_cast<float>(cols)), 0, cols - 1);
      const float cx = t.map({hover_.x, 0.0f}).x;
      dl.addRectFilled({std::round(cx), r.y0, std::round(cx) + 1.0f, r.y1}, ui::rgba(255, 255, 255, 60));
      char buf[64];
      const double at = (static_cast<double>(c) + 0.5) / static_cast<double>(cols);
      if (loopMode) {
        const double spanSec = loop_.spanSamples / std::max(1.0, sampleRate());
        if (tempoKnown) std::snprintf(buf, sizeof buf, "beat %.2f", 1.0 + at * spanSec * bpm / 60.0);
        else std::snprintf(buf, sizeof buf, "%.3f s", at * spanSec);
      } else {
        std::snprintf(buf, sizeof buf, "-%.2f s", (1.0 - at) * static_cast<double>(windowSec));
      }
      const float lx = hover_.x > kWell.x1 - 90.0f ? hover_.x - 86.0f : hover_.x + 5.0f;
      float ty = kWell.y0 + 16.0f;
      label(p, {lx, ty}, buf, ui::rgba(255, 255, 255, 210));
      auto laneLine = [&](const ColAgg& a, const char* nm, Rgba colr) {
        ty += kLabelPx + 2.0f;
        if (!a.valid) std::snprintf(buf, sizeof buf, "%-4s  --", nm);
        else {
          auto dbOf = [](float v) { return v > 1e-7f ? 20.0f * std::log10(v) : -140.0f; };
          std::snprintf(buf, sizeof buf, "%-4s pk %6.1f rms %6.1f dB", nm, static_cast<double>(dbOf(std::max(std::fabs(a.mn), std::fabs(a.mx)))),
                        static_cast<double>(dbOf(a.rms)));
        }
        label(p, {lx, ty}, buf, colr);
      };
      laneLine(colA_[static_cast<size_t>(c)], nameA, laneA_);
      if (chB >= 0) laneLine(colB_[static_cast<size_t>(c)], nameB, laneB_);
    }
  }

private:
  Rgba laneA_ = kLaneA, laneB_ = kLaneB, clip_ = kClip;  // VZ.10: as painted (the options over PQ's)
  double sampleRate() const noexcept { return feed_ != nullptr ? feed_->config().sampleRate : 48000.0; }
  // The scroll's columns: the window's bins cut on an ABSOLUTE grid (multiples of a fixed stride from bin 0), so a bin
  // keeps its column while on screen; the newest column absorbs the bins past the grid and the partial one (PQ).
  void snapshot(int cols, int chA, int chB, std::vector<ColAgg>& a, std::vector<ColAgg>& b) {
    a.assign(static_cast<size_t>(cols), ColAgg{});
    b.assign(static_cast<size_t>(cols), ColAgg{});
    const double sr = sampleRate();
    auto want = static_cast<uint64_t>(opts_[wWindow] * sr / dsp::WaveCapture::kBinSize + 0.5);
    want = std::clamp<uint64_t>(want, 4, dsp::WaveCapture::kRingBins - 2);
    const uint64_t end = binsRead_;
    uint64_t nBins = std::min(want, end);
    if (nBins < 2 || bins_.empty()) return;
    const uint64_t stride = std::max<uint64_t>(1, nBins / static_cast<uint64_t>(cols));
    nBins = stride * static_cast<uint64_t>(cols);
    const uint64_t gridEnd = end - end % stride;
    if (gridEnd < nBins) return;
    const uint64_t first = gridEnd - nBins, liveTail = end - gridEnd;
    for (int c = 0; c < cols; ++c) {
      const uint64_t b0 = first + static_cast<uint64_t>(c) * stride;
      uint64_t b1 = b0 + stride;
      if (c == cols - 1) b1 += liveTail;
      Fold fa, fb;
      for (uint64_t i = b0; i < b1; ++i) {
        const dsp::WaveBin& bin = bins_[i % dsp::WaveCapture::kRingBins];
        fa.add(a[static_cast<size_t>(c)], bin, chA);
        if (chB >= 0) fb.add(b[static_cast<size_t>(c)], bin, chB);
      }
      if (c == cols - 1 && !frozen_ && partial_.n > 0) {
        fa.add(a[static_cast<size_t>(c)], partial_, chA);
        if (chB >= 0) fb.add(b[static_cast<size_t>(c)], partial_, chB);
      }
      fa.finish(a[static_cast<size_t>(c)]);
      if (chB >= 0) fb.finish(b[static_cast<size_t>(c)]);
    }
  }
  void loopColumns(int cols, int chA, int chB, std::vector<ColAgg>& a, std::vector<ColAgg>& b) const {
    a.assign(static_cast<size_t>(cols), ColAgg{});
    b.assign(static_cast<size_t>(cols), ColAgg{});
    const uint32_t S = dsp::WaveCapture::kLoopSlots;
    for (int c = 0; c < cols; ++c) {
      const auto s0 = static_cast<uint32_t>(static_cast<uint64_t>(c) * S / static_cast<uint64_t>(cols));
      auto s1 = static_cast<uint32_t>(static_cast<uint64_t>(c + 1) * S / static_cast<uint64_t>(cols));
      if (s1 <= s0) s1 = s0 + 1;
      Fold fa, fb;
      for (uint32_t s = s0; s < s1 && s < S; ++s) {
        if (!filled_[s]) continue;  // unvisited slots stay holes
        fa.add(a[static_cast<size_t>(c)], slots_[s], chA);
        if (chB >= 0) fb.add(b[static_cast<size_t>(c)], slots_[s], chB);
      }
      fa.finish(a[static_cast<size_t>(c)]);
      if (chB >= 0) fb.finish(b[static_cast<size_t>(c)]);
    }
  }

  std::vector<dsp::WaveBin> bins_;  // the stage's bins, bin i at i % kRingBins (sized when first read)
  dsp::WaveBin partial_{};
  uint32_t lastPartialN_ = 0;
  uint64_t binsRead_ = 0;
  std::vector<dsp::WaveBin> slots_;
  std::vector<uint8_t> filled_;
  uint64_t stamp_ = 0;
  dsp::WaveLoopState loop_;
  float autoGain_ = 1.0f, targetGain_ = 1.0f;
  bool gainPrimed_ = false;
  std::vector<ColAgg> colA_, colB_, other_;
  std::vector<float> xs_, yA_, yB_, tMin_, tMax_, tRms_, aCol_, tMaxD_, tMinD_, tRmsS_, tRaw_, wRaw_;
  std::vector<Rgba> cA_, cB_, zero_;
  std::vector<Vec2> caps_;
};

StageFace* asStage(MetaFace& f) noexcept { return dynamic_cast<StageFace*>(&f); }

}  // namespace

std::unique_ptr<MetaFace> makeBandLevelsFace(const meta::Type& t) { return std::make_unique<BandLevelsFace>(t); }
std::unique_ptr<MetaFace> makeWaveformFace(const meta::Type& t) { return std::make_unique<WaveformFace>(t); }

bool bandWaveShown(const MetaFace& f, BandWaveShown& out) {
  out = {};
  if (const auto* b = dynamic_cast<const BandLevelsFace*>(&f)) {
    out.acquired = b->acquiredForTest();
    out.columns = b->shownColumns();
    out.bands = b->config();
    for (int i = 0; i < dsp::BandLevels::kBands; ++i) out.levels[i] = b->shownLevel(i);
    return true;
  }
  if (const auto* w = dynamic_cast<const WaveformFace*>(&f)) {
    out.acquired = w->acquiredForTest();
    out.bins = w->shownBins();
    out.wave = w->config();
    out.loop = w->loopState();
    return true;
  }
  return false;
}

bool bandWaveTestControl(MetaFace& f, std::string_view key, double value) {
  StageFace* s = asStage(f);
  if (s == nullptr) return false;
  if (key == "freeze") s->setFrozenForTest(value != 0.0);
  else if (key == "sheet") s->setSheetForTest(value != 0.0);
  else if (key == "hover") s->setHoverForTest({static_cast<float>(value), 80.0f});
  else s->setOptionForTest(key, value);
  return true;
}

void bandWaveRequest(std::string_view type, const meta::DataValue* data, dsp::BandLevelsConfig& bands, dsp::WaveConfig& wave,
                     float& rest) {
  bands = {};
  wave = {};
  rest = 10.0f;
  if (type == "band_levels") {
    BandLevelsFace f(meta::bandLevelsType());
    f.loadForTest(data);
    bands = f.config();
  } else if (type == "waveform") {
    WaveformFace f(meta::waveformType());
    f.loadForTest(data);
    wave = f.config();
    rest = f.restSeconds();
  }
}

bool waveColumns(MetaFace& f, int cols, int channel, std::vector<float>& mn, std::vector<float>& mx, std::vector<float>& rms) {
  auto* w = dynamic_cast<WaveformFace*>(&f);
  if (w == nullptr) return false;
  std::vector<ColAgg> c;
  w->columnsFor(cols, channel, c);
  mn.resize(c.size());
  mx.resize(c.size());
  rms.resize(c.size());
  for (size_t i = 0; i < c.size(); ++i) {
    mn[i] = c[i].valid ? c[i].mn : 0.0f;
    mx[i] = c[i].valid ? c[i].mx : 0.0f;
    rms[i] = c[i].valid ? c[i].rms : 0.0f;
  }
  return true;
}

}  // namespace pq::views
