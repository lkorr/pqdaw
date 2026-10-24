// MOS wheel region chart (T3.13): a port of mos-explorer's canvas (index.html drawAxis / drawMOS / drawEDOs /
// drawTemperaments / drawGeneratorMarker / drawSizeScrollbar and the hitTest / handleClick / setGenFromScreenX
// interaction), restyled with the theme's `mosChart.*` tokens. See pq/views/mos_wheel/mos_chart.h.

#include "pq/views/mos_wheel/mos_chart.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>

#include "../common/labels.h"
#include "../common/paint.h"
#include "pq/tuning/mos_data.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"

namespace pq::views {

using namespace pq::ui;
namespace mcm = mos_chart_metrics;
namespace tu = pq::tuning;

namespace {

uint32_t hexDigit(char c) noexcept {
  if (c >= '0' && c <= '9') return static_cast<uint32_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint32_t>(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return static_cast<uint32_t>(c - 'A' + 10);
  return 0;
}

// "#rrggbb" or "#rrggbbaa" (the explorer's colour strings).
Rgba parseColor(std::string_view s) noexcept {
  auto byte = [&](size_t i) { return hexDigit(s[i]) * 16 + hexDigit(s[i + 1]); };
  if (s.size() < 7 || s[0] != '#') return rgba(136, 136, 136);
  return rgba(byte(1), byte(3), byte(5), s.size() >= 9 ? byte(7) : 255u);
}

// hexA(hex, a): the colour's RGB with alpha a (the explorer ignores any alpha in the string here).
Rgba alphaOfFloat(Rgba c, float a) noexcept { return setAlpha(c, a); }

std::string fmtCents(double c, int decimals) {
  char buf[48];
  const int n = std::snprintf(buf, sizeof buf, "%.*f\xC2\xA2", decimals, c);
  return std::string(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

// drawAxis's tick spacing for a visible range of vr cents.
double axisTick(double vr) noexcept {
  if (vr > 800) return 100;
  if (vr > 400) return 50;
  if (vr > 200) return 20;
  if (vr > 80) return 10;
  if (vr > 30) return 5;
  if (vr > 10) return 2;
  if (vr > 3) return 1;
  return 0.5;
}

bool overlaps(const IRect& c, float x0, float y0, float x1, float y1) noexcept {
  return x1 > static_cast<float>(c.x) && x0 < static_cast<float>(c.right()) && y1 > static_cast<float>(c.y) &&
         y0 < static_cast<float>(c.bottom());
}

// Crisp dashed vertical line [y0, y1) at x, only the dashes that meet the clip's rows.
void dashedVLine(DrawList& dl, const IRect& clip, float x, float y0, float y1, float on, float off, Rgba col, float thick) {
  const float period = on + off;
  if (period <= 0.0f || y1 <= y0) return;
  const float first = std::max(0.0f, std::floor((static_cast<float>(clip.y) - y0) / period));
  for (float y = y0 + first * period; y < y1 && y < static_cast<float>(clip.bottom()); y += period)
    dl.addVLine(x, y, std::min(y + on, y1), col, thick);
}

}  // namespace

tu::MosParams MosSelection::params() const {
  tu::MosParams p;
  p.generator = generatorCents;
  p.n = perPeriodKey.size();
  p.period = period;
  p.periodsPerEquave = periodsPerEquave;
  p.rotation = modeRotation;
  p.region = hint();
  return p;
}

// --- construction / config --------------------------------------------------------------------------------------

MosChart::MosChart(std::string name, const MosChartConfig& config) : Widget(std::move(name)), config_(config) {
  setFocusable(true);
  palette_.reserve(tu::kMosPalette.size());
  for (const char* c : tu::kMosPalette) palette_.push_back(parseColor(c));
  rebuild();
  viewMin_ = 0.0;
  viewMax_ = viewLimit();
}

MosChart::~MosChart() = default;

int32_t MosChart::periodReps() const noexcept { return std::max(1, config_.periodsPerEquave); }
double MosChart::viewLimit() const noexcept { return config_.period * periodReps(); }

double MosChart::reduce(double cents) const noexcept {
  if (periodReps() <= 1) return cents;
  const double p = config_.period;
  return std::fmod(std::fmod(cents, p) + p, p);
}

void MosChart::setConfig(const MosChartConfig& config) {
  if (config == config_) return;
  config_ = config;
  hideCard();  // its text and region index refer to the old regions
  rebuild();
  setView(viewMin_, viewMax_);
  invalidate();
}

// recalc(): regions + EDO generators, then keep the selection (exact region, else the same MOS containing the
// generator, else the same MOS nearest the old centre, else none).
void MosChart::rebuild() {
  config_.maxSize = std::clamp(config_.maxSize, 2, 64);
  config_.maxEdo = std::clamp(config_.maxEdo, 2, 400);
  config_.periodsPerEquave = std::max(1, config_.periodsPerEquave);
  if (!(config_.period > 0.0) || !std::isfinite(config_.period)) config_.period = 1200.0;
  ++stats_.rebuilds;
  const std::optional<MosSelection> old = selection_;
  regions_ = tu::buildMosRegions(config_.maxSize, config_.maxSize, config_.period, periodReps());
  edos_ = tu::edoGenerators(config_.maxEdo, config_.period);
  // m·period/n directly (the JS computes (m/n)·period, which can be one ulp off: 7/12·1200 must be exactly 700).
  for (tu::EdoGenerator& g : edos_) g.cents = static_cast<double>(g.step) * config_.period / static_cast<double>(g.edo);

  bySize_.assign(static_cast<size_t>(config_.maxSize) + 1, {});
  const size_t n = regions_.regions.size();
  labels_.assign(n, {});  // formatted on first use (only bands wide enough for a label ever need them)
  for (size_t i = 0; i < n; ++i) {
    const tu::MosRegion& r = regions_.regions[i];
    if (r.perPeriodSize >= 2 && r.perPeriodSize <= config_.maxSize)
      bySize_[static_cast<size_t>(r.perPeriodSize)].push_back(static_cast<int32_t>(i));
  }
  for (auto& v : bySize_)
    std::sort(v.begin(), v.end(), [&](int32_t a, int32_t b) {
      return regions_.regions[static_cast<size_t>(a)].lowCents < regions_.regions[static_cast<size_t>(b)].lowCents;
    });
  firstSize_ = std::clamp(firstSize_, 2, config_.maxSize);
  hovered_ = -1;
  selected_ = -1;
  tempFor_ = -2;
  selection_.reset();
  if (old) {
    int32_t next = -1;
    for (size_t i = 0; i < n && next < 0; ++i) {
      const tu::MosRegion& r = regions_.regions[i];
      if (r.key == old->key && std::fabs(r.lowCents - old->regionLow) < 0.01 && std::fabs(r.highCents - old->regionHigh) < 0.01)
        next = static_cast<int32_t>(i);
    }
    if (next < 0) {
      double best = std::numeric_limits<double>::infinity();
      for (size_t i = 0; i < n; ++i) {
        const tu::MosRegion& r = regions_.regions[i];
        if (r.key != old->key) continue;
        if (old->generatorCents >= r.lowCents && old->generatorCents <= r.highCents) {
          next = static_cast<int32_t>(i);
          break;
        }
        const double d = std::fabs((r.lowCents + r.highCents) / 2 - (old->regionLow + old->regionHigh) / 2);
        if (d < best) {
          best = d;
          next = static_cast<int32_t>(i);
        }
      }
    }
    if (next >= 0) {
      const tu::MosRegion& r = regions_.regions[static_cast<size_t>(next)];
      setSelectedRegion(next, std::clamp(old->generatorCents, r.lowCents, r.highCents), old->modeRotation);
    }
  }
  layoutDirty_ = true;
}

// --- view -------------------------------------------------------------------------------------------------------

void MosChart::setView(double minCents, double maxCents) {
  const double limit = viewLimit();
  double range = std::clamp(maxCents - minCents, mcm::kMinViewCents, limit);
  if (!std::isfinite(range)) range = limit;
  double lo = std::isfinite(minCents) ? minCents : 0.0;
  // clampView
  if (lo < 0) lo = 0;
  if (lo + range > limit) lo = limit - range;
  lo = std::max(0.0, lo);
  const double hi = std::min(limit, lo + range);
  if (lo == viewMin_ && hi == viewMax_) return;
  viewMin_ = lo;
  viewMax_ = hi;
  invalidate();
}

void MosChart::setFirstSize(int32_t size) { setFirstSizeInternal(size, true); }

void MosChart::setFirstSizeInternal(int32_t size, bool emitDamage) {
  size = std::clamp(size, 2, config_.maxSize);
  if (size == firstSize_) return;
  firstSize_ = size;
  if (emitDamage) invalidate();
}

void MosChart::setRowScale(float scale) {
  scale = std::clamp(scale, mcm::kMinRowScale, mcm::kMaxRowScale);
  if (scale == rowScale_) return;
  rowScale_ = scale;
  invalidate();
}

void MosChart::invalidate() {
  layoutDirty_ = true;
  markDirty();
}

// --- selection --------------------------------------------------------------------------------------------------

bool MosChart::sameRegion(int32_t a, int32_t b) const noexcept {
  if (a < 0 || b < 0) return false;
  if (a == b) return true;
  const tu::MosRegion& ra = regions_.regions[static_cast<size_t>(a)];
  const tu::MosRegion& rb = regions_.regions[static_cast<size_t>(b)];
  return ra.key == rb.key && std::fabs(ra.lowCents - rb.lowCents) < 0.01 && std::fabs(ra.highCents - rb.highCents) < 0.01;
}

void MosChart::setSelectedRegion(int32_t region, double gen, int32_t rotation) {
  selected_ = region;
  if (region < 0) {
    selection_.reset();
    return;
  }
  const tu::MosRegion& r = regions_.regions[static_cast<size_t>(region)];
  equalizedEndLow_ = tu::equalizedEnd(r, config_.period) == tu::RegionEnd::Low ? 1 : 0;
  MosSelection s;
  s.key = r.key;
  s.perPeriodKey = r.perPeriodKey;
  s.periodsPerEquave = periodReps();
  s.period = config_.period;
  s.generatorCents = gen;
  s.regionLow = r.lowCents;
  s.regionHigh = r.highCents;
  s.mirror = r.isMirror;
  s.modeRotation = (rotation >= 0 && rotation < r.perPeriodSize) ? rotation : 0;
  selection_ = s;
  refreshSelectionValue();
}

// The EDO identity of the current generator (exact m·period/n within 1e-9 cents).
void MosChart::refreshSelectionValue() {
  if (!selection_) return;
  selection_->edo.reset();
  for (const tu::EdoGenerator& g : edos_)
    if (std::fabs(g.cents - selection_->generatorCents) < 1e-9) {
      selection_->edo = MosSelection::Edo{g.step, g.edo};
      break;
    }
}

bool MosChart::select(tu::MosKey perPeriodKey, double generatorCents, int32_t modeRotation) {
  const double g = reduce(generatorCents);
  int32_t best = -1;
  double bestD = std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < regions_.regions.size(); ++i) {
    const tu::MosRegion& r = regions_.regions[i];
    if (!(r.perPeriodKey == perPeriodKey)) continue;
    const double d = g < r.lowCents ? r.lowCents - g : (g > r.highCents ? g - r.highCents : 0.0);
    if (d < bestD) {
      bestD = d;
      best = static_cast<int32_t>(i);
    }
  }
  if (best < 0) return false;
  const tu::MosRegion& r = regions_.regions[static_cast<size_t>(best)];
  setSelectedRegion(best, std::clamp(g, r.lowCents, r.highCents), modeRotation);
  // ensureSizeVisible: bring the row into view.
  if (r.perPeriodSize < firstSize_) firstSize_ = r.perPeriodSize;
  layoutDirty_ = true;
  ensureLayout();
  if (geo_.rowCount > 0 && r.perPeriodSize >= firstSize_ + geo_.rowCount)
    firstSize_ = std::clamp(r.perPeriodSize - geo_.rowCount + 1, 2, config_.maxSize);
  invalidate();
  return true;
}

void MosChart::clearSelection() {
  if (selected_ < 0) return;
  setSelectedRegion(-1, 0.0, 0);
  invalidate();
}

void MosChart::emit(MosGesture g) {
  if (onChange_) onChange_(selection_, g);
}

ui::Rgba MosChart::regionColor(int32_t region) const noexcept {
  const tu::MosRegion& r = regions_.regions[static_cast<size_t>(region)];
  const Rgba collapsed = palette_[r.collapsedColor % palette_.size()];
  if (region != selected_ || !selection_) return collapsed;
  const Rgba equalized = palette_[r.equalizedColor % palette_.size()];
  const double w = r.highCents - r.lowCents;
  const double genT = equalizedEndLow_ ? (selection_->generatorCents - r.lowCents) / w : (r.highCents - selection_->generatorCents) / w;
  const double soft = std::clamp(1.0 - genT, 0.0, 1.0);
  return lerp(collapsed, equalized, static_cast<float>(soft));
}

// The band label strings of region i, formatted on first use: "5L 2s", "5L2s", "5L 2s (Diatonic)" (empty without a
// family name) and "685.7¢–720.0¢".
const MosChart::Labels& MosChart::labelsOf(int32_t region) {
  Labels& l = labels_[static_cast<size_t>(region)];
  if (l.ready) return l;
  const tu::MosRegion& r = regions_.regions[static_cast<size_t>(region)];
  l.label = tu::mosDisplayLabel(r.key);
  l.compact = std::to_string(r.key.nL) + "L" + std::to_string(r.key.nS) + "s";
  const auto fname = tu::mosDisplayName(r.key, periodReps(), config_.period);
  if (fname) l.name = l.label + " (" + std::string(*fname) + ")";
  char buf[64];
  const int len = std::snprintf(buf, sizeof buf, "%.1f\xC2\xA2\xE2\x80\x93%.1f\xC2\xA2", r.lowCents, r.highCents);
  l.range.assign(buf, len > 0 ? static_cast<size_t>(len) : 0);
  l.ready = true;
  return l;
}

// --- layout -----------------------------------------------------------------------------------------------------

void MosChart::computeGeo(Geo& g) const noexcept {
  const UiScale s = tree() ? tree()->uiScale() : UiScale{};
  g.r = rect();
  const int32_t sb = std::min(g.r.w, s.px(mcm::kScrollbarW));
  g.plot = {g.r.x, g.r.y, std::max(0, g.r.w - sb), g.r.h};
  const double m = s.exact(mcm::kMargin);
  g.x0 = static_cast<double>(g.plot.x) + m;
  g.w = std::max(1.0, static_cast<double>(g.plot.w) - 2.0 * m);
}

void MosChart::ensureLayout() const {
  const float sc = tree() ? tree()->scale() : 1.0f;
  if (layoutDirty_ || layoutRect_ != rect() || layoutScale_ != sc) layoutNow();
}

void MosChart::layoutNow() const {
  ++stats_.layouts;
  layoutDirty_ = false;
  layoutRect_ = rect();
  layoutScale_ = tree() ? tree()->scale() : 1.0f;
  const UiScale s(layoutScale_);
  Geo& g = geo_;
  computeGeo(g);
  const double vr = viewMax_ - viewMin_;
  auto c2x = [&](double c) { return g.x0 + (c - viewMin_) / vr * g.w; };
  const int32_t reps = periodReps();
  const double P = config_.period;
  const float plotL = static_cast<float>(g.plot.x), plotR = static_cast<float>(g.plot.right());

  // EDO markers of the selected region (drawEDOs): lowest n first, a marker closer than 0.65 × 48 px to one
  // already placed is dropped.
  markers_.clear();
  markerLabels_.clear();
  tempBars_.clear();
  tempRows_ = 0;
  if (selected_ >= 0) {
    const tu::MosRegion& sel = regions_.regions[static_cast<size_t>(selected_)];
    const float minGap = s.exact(mcm::kEdoMinPx * 0.65f);
    const double octaveRatio = 1200.0 / P;
    for (const tu::EdoGenerator& e : edos_) {
      if (e.cents < sel.lowCents || e.cents > sel.highCents) continue;
      for (int32_t k = 0; k < reps; ++k) {
        const double oc = e.cents + k * P;
        if (oc < viewMin_ || oc > viewMax_) continue;
        const auto x = static_cast<float>(c2x(oc));
        bool clash = false;
        for (const MosChartEdoMarker& m : markers_)
          if (std::fabs(x - m.x) < minGap) {
            clash = true;
            break;
          }
        if (clash) continue;
        markers_.push_back({x, e.cents, oc, e.step, e.edo});
        const double octEdo = e.edo * octaveRatio;
        const bool clean = std::fabs(octEdo - std::round(octEdo)) < 0.001;
        const int64_t dispEdo = clean ? static_cast<int64_t>(std::llround(octEdo)) : e.edo;
        const int64_t dispStep = clean ? static_cast<int64_t>(std::llround(oc * static_cast<double>(dispEdo) / 1200.0))
                                       : static_cast<int64_t>(e.step) + static_cast<int64_t>(k) * e.edo;
        char buf[48];
        int len = std::snprintf(buf, sizeof buf, "%lld\\%lld", static_cast<long long>(dispStep), static_cast<long long>(dispEdo));
        markerLabels_.emplace_back(buf, len > 0 ? static_cast<size_t>(len) : 0);
        markerLabels_.push_back(fmtCents(oc, 1));
        const tu::Hardness hr = tu::computeHardness(e.cents, sel.perPeriodSize, P);
        if (std::isfinite(hr.ratio) && hr.ratio > 0.5) {
          const double step = P / e.edo;
          len = std::snprintf(buf, sizeof buf, "%lld:%lld", static_cast<long long>(std::llround(hr.L / step)),
                              static_cast<long long>(std::llround(hr.s / step)));
          markerLabels_.emplace_back(buf, len > 0 ? static_cast<size_t>(len) : 0);
        } else {
          markerLabels_.emplace_back();
        }
      }
    }

    // Temperament ranges (drawTemperaments): bars clipped to the region, widest first, packed into rows greedily;
    // row 0 is drawn lowest.
    if (std::fabs(config_.octaveStretch) <= 5.0) {
      if (tempFor_ != selected_) {
        tempFor_ = selected_;
        tempMatches_.clear();
        tempLow_.clear();
        tempHigh_.clear();
        for (const tu::TemperamentMatch& m : tu::temperamentsForMos(sel.perPeriodKey, P, tu::CentsRange{sel.lowCents, sel.highCents})) {
          tempMatches_.push_back(m.index);
          tempLow_.push_back(m.low);
          tempHigh_.push_back(m.high);
        }
      }
      struct Pending {
        size_t match;
        float x1, x2;
        double centsW;
        int32_t row;
      };
      std::vector<Pending> bars;
      for (size_t i = 0; i < tempMatches_.size(); ++i) {
        const double lo = std::max(tempLow_[i], sel.lowCents), hi = std::min(tempHigh_[i], sel.highCents);
        if (hi <= lo) continue;
        for (int32_t k = 0; k < reps; ++k) {
          const double oLo = lo + k * P, oHi = hi + k * P;
          if (oHi < viewMin_ || oLo > viewMax_) continue;
          const float x1 = std::max(plotL, static_cast<float>(c2x(oLo)));
          const float x2 = std::min(plotR, static_cast<float>(c2x(oHi)));
          if (x2 - x1 < 1.0f) continue;
          bars.push_back({i, x1, x2, tempHigh_[i] - tempLow_[i], 0});
        }
      }
      std::stable_sort(bars.begin(), bars.end(), [&](const Pending& a, const Pending& b) {
        if (a.centsW != b.centsW) return a.centsW > b.centsW;
        return tempMatches_[a.match] < tempMatches_[b.match];
      });
      int32_t rows = 0;
      for (size_t i = 0; i < bars.size(); ++i) {
        int32_t row = 0;
        for (; row < rows; ++row) {
          bool clash = false;
          for (size_t j = 0; j < i && !clash; ++j)
            clash = bars[j].row == row && bars[j].x1 < bars[i].x2 && bars[j].x2 > bars[i].x1;
          if (!clash) break;
        }
        bars[i].row = row;
        if (row == rows) ++rows;
      }
      tempRows_ = rows;
      const auto temps = tu::temperaments();
      for (const Pending& b : bars) {
        const float yL = mcm::kTempTop + static_cast<float>(rows - 1 - b.row) * (mcm::kTempBarH + mcm::kTempBarGap);
        const int32_t y = g.r.y + s.px(yL);
        const int32_t idx = tempMatches_[b.match];
        const auto colStr = tu::temperamentColor(temps[static_cast<size_t>(idx)].name);
        const Rgba edge = colStr ? parseColor(*colStr) : rgba(136, 136, 136);
        tempBars_.push_back({idx, b.x1, b.x2, y, g.r.y + s.px(yL + mcm::kTempBarH) - y, alphaOfFloat(edge, 0.5f), edge, 0});
      }
    }
  }

  // Rows (drawMOS): one per size from firstSize, as many as fit.
  const float tempSpace = selected_ >= 0 && tempRows_ ? static_cast<float>(tempRows_) * (mcm::kTempBarH + mcm::kTempBarGap) + 8.0f : 0.0f;
  const float startL = selected_ >= 0 ? mcm::kTempTop + tempSpace : mcm::kRowsTopIdle;
  const float rowHL = std::round(mcm::kRowUnit * rowScale_);
  const float gapL = std::max(2.0f, std::round(2.0f * rowScale_));
  g.rowsTop = g.r.y + s.px(startL);
  g.rowH = std::max(1, s.px(rowHL));
  g.rowPitch = std::max(g.rowH + 1, s.px(rowHL + gapL));
  g.rowCount = 0;
  for (int32_t n = firstSize_; n <= config_.maxSize; ++n) {
    const int32_t y = g.rowsTop + (n - firstSize_) * g.rowPitch;
    if (y + g.rowH > g.r.bottom()) break;
    ++g.rowCount;
  }

  bands_.clear();
  rowFirstBand_.assign(static_cast<size_t>(g.rowCount) + 1, 0);
  for (int32_t row = 0; row < g.rowCount; ++row) {
    rowFirstBand_[static_cast<size_t>(row)] = static_cast<int32_t>(bands_.size());
    const int32_t n = firstSize_ + row;
    const int32_t y = g.rowsTop + row * g.rowPitch;
    for (const int32_t ri : bySize_[static_cast<size_t>(n)]) {
      const tu::MosRegion& r = regions_.regions[static_cast<size_t>(ri)];
      for (int32_t k = 0; k < reps; ++k) {
        const double oLow = r.lowCents + k * P, oHigh = r.highCents + k * P;
        if (oHigh < viewMin_ || oLow > viewMax_) continue;
        const float x1 = std::max(plotL, static_cast<float>(c2x(oLow)));
        const float x2 = std::min(plotR, static_cast<float>(c2x(oHigh)));
        if (x2 - x1 < 1.0f) continue;
        bands_.push_back({ri, x1, x2, y, g.rowH, oLow, oHigh});
      }
    }
  }
  rowFirstBand_[static_cast<size_t>(g.rowCount)] = static_cast<int32_t>(bands_.size());
}

// --- geometry queries ---------------------------------------------------------------------------------------------

double MosChart::centsAtX(float x) const noexcept {
  ensureLayout();
  return viewMin_ + (static_cast<double>(x) - geo_.x0) / geo_.w * (viewMax_ - viewMin_);
}

float MosChart::xOfCents(double cents) const noexcept {
  ensureLayout();
  return static_cast<float>(geo_.x0 + (cents - viewMin_) / (viewMax_ - viewMin_) * geo_.w);
}

int32_t MosChart::sizeAtY(float y) const noexcept {
  ensureLayout();
  const float rel = y - static_cast<float>(geo_.rowsTop);
  if (rel < 0.0f) return -1;
  const auto row = static_cast<int32_t>(rel / static_cast<float>(geo_.rowPitch));
  if (row >= geo_.rowCount) return -1;
  if (rel - static_cast<float>(row * geo_.rowPitch) >= static_cast<float>(geo_.rowH)) return -1;
  return firstSize_ + row;
}

IRect MosChart::rowRect(int32_t size) const noexcept {
  ensureLayout();
  const int32_t row = size - firstSize_;
  if (row < 0 || row >= geo_.rowCount) return {};
  return {geo_.plot.x, geo_.rowsTop + row * geo_.rowPitch, geo_.plot.w, geo_.rowH};
}

IRect MosChart::axisRect() const noexcept {
  ensureLayout();
  const UiScale s(layoutScale_);
  const int32_t top = geo_.r.y + s.px(mcm::kAxisY - mcm::kAxisBarH / 2);
  return {geo_.plot.x, top, geo_.plot.w, geo_.r.y + s.px(mcm::kAxisY + mcm::kAxisBarH / 2) - top};
}

IRect MosChart::plotRect() const noexcept {
  ensureLayout();
  return geo_.plot;
}

IRect MosChart::scrollbarRect() const noexcept {
  ensureLayout();
  const UiScale s(layoutScale_);
  const int32_t top = geo_.rowsTop;
  const int32_t bottom = geo_.r.bottom() - s.px(8.0f);
  return {geo_.plot.right(), top, geo_.r.right() - geo_.plot.right(), std::max(0, bottom - top)};
}

std::span<const MosChartEdoMarker> MosChart::edoMarkers() const noexcept {
  ensureLayout();
  return markers_;
}

int32_t MosChart::regionAt(Vec2 p) const noexcept {
  ensureLayout();
  if (p.x < static_cast<float>(geo_.plot.x) || p.x >= static_cast<float>(geo_.plot.right())) return -1;
  const float rel = p.y - static_cast<float>(geo_.rowsTop);
  if (rel < 0.0f) return -1;
  const auto row = static_cast<int32_t>(rel / static_cast<float>(geo_.rowPitch));
  if (row >= geo_.rowCount || rel - static_cast<float>(row * geo_.rowPitch) > static_cast<float>(geo_.rowH)) return -1;
  // The explorer's test: mx within [c2x(oLow), c2x(oHigh)] (inclusive), the first band found wins.
  for (int32_t i = rowFirstBand_[static_cast<size_t>(row)]; i < rowFirstBand_[static_cast<size_t>(row) + 1]; ++i) {
    const Band& b = bands_[static_cast<size_t>(i)];
    if (p.x >= b.x1 && p.x <= b.x2) return b.region;
  }
  return -1;
}

const MosChartEdoMarker* MosChart::edoMarkerAt(Vec2 p) const noexcept {
  ensureLayout();
  if (selected_ < 0) return nullptr;
  const UiScale s(layoutScale_);
  const float y0 = static_cast<float>(geo_.r.y + s.px(mcm::kEdoHitTop));
  const float y1 = y0 + s.exact(mcm::kEdoHitH);
  if (p.y < y0 || p.y > y1) return nullptr;
  for (const MosChartEdoMarker& m : markers_)
    if (std::fabs(p.x - m.x) < s.exact(mcm::kEdoHitHalfW)) return &m;
  return nullptr;
}

// --- damage -------------------------------------------------------------------------------------------------------

void MosChart::damageRect(const IRect& r) {
  UiTree* t = tree();
  if (!t) return;
  const IRect v = intersect(r, rect());
  if (!v.empty()) t->damage().add(v.toRect());
}

void MosChart::damageRegionBands(int32_t region) {
  if (region < 0) return;
  ensureLayout();
  for (const Band& b : bands_)
    if (b.region == region) {
      const auto x0 = static_cast<int32_t>(std::floor(b.x1)), x1 = static_cast<int32_t>(std::ceil(b.x2));
      damageRect({x0, b.y, x1 - x0, b.h});
    }
}

// Everything the generator value draws: the marker's column(s), the value pill, the selected band (its colour).
void MosChart::damageGeneratorParts() {
  if (!selection_) return;
  ensureLayout();
  const UiScale s(layoutScale_);
  const double P = config_.period;
  const int32_t top = geo_.r.y + s.px(mcm::kAxisY - 4.0f);
  const int32_t half = s.px(mcm::kMarkerGrabPx + 2.0f);
  // The selected band's row is damaged whole below (its colour follows the generator), so the marker column skips
  // it: the pieces above and below stay narrow instead of merging with the band into one tall bounding box.
  const IRect row = selected_ >= 0 ? rowRect(regions_.regions[static_cast<size_t>(selected_)].perPeriodSize) : IRect{};
  bool pill = false;
  for (int32_t k = 0; k < periodReps(); ++k) {
    const float x = xOfCents(selection_->generatorCents + k * P);
    if (x < static_cast<float>(geo_.plot.x) - 10.0f || x > static_cast<float>(geo_.plot.right()) + 10.0f) continue;
    const auto xi = static_cast<int32_t>(std::floor(x));
    const IRect col{xi - half, top, 2 * half + 1, geo_.r.bottom() - top};
    if (row.empty() || row.y >= col.bottom() || row.bottom() <= col.y) {
      damageRect(col);
    } else {
      damageRect({col.x, col.y, col.w, row.y - col.y});
      damageRect({col.x, row.bottom(), col.w, col.bottom() - row.bottom()});
    }
    if (!pill) {
      pill = true;
      damageRect(pillRect(x));
    }
  }
  // The selected band(s), widened by the marker's half width (the marker may sit on a band edge).
  if (selected_ >= 0)
    for (const Band& b : bands_)
      if (b.region == selected_) {
        const auto x0 = static_cast<int32_t>(std::floor(b.x1)) - half, x1 = static_cast<int32_t>(std::ceil(b.x2)) + half;
        damageRect({x0, b.y, x1 - x0, b.h});
      }
}

// The value pill above the axis, centred on x and kept inside the plot. Width from the label (measured with the
// tree's resources; a generous fixed width without them).
IRect MosChart::pillRect(float x) const {
  const UiScale s(layoutScale_);
  int32_t textW = s.px(64.0f);
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  if (res && res->text && res->uiFonts && selection_) {
    char buf[48];
    const int n = std::snprintf(buf, sizeof buf, "%.1f\xC2\xA2", tu::stretchCents(selection_->generatorCents, config_.octaveStretch));
    textW = res->text->measure(res->uiFonts->bodyStrong, std::string_view(buf, n > 0 ? static_cast<size_t>(n) : 0));
  }
  const int32_t w = textW + s.px(10.0f);
  const int32_t h = s.px(16.0f);
  int32_t x0 = static_cast<int32_t>(std::lround(x)) - w / 2;
  x0 = std::clamp(x0, geo_.plot.x, std::max(geo_.plot.x, geo_.plot.right() - w));
  return {x0, geo_.r.y + s.px(15.0f), w, h};
}

// --- generator ----------------------------------------------------------------------------------------------------

void MosChart::setGenerator(double gen) {
  if (!selection_ || selection_->generatorCents == gen) return;
  damageGeneratorParts();
  selection_->generatorCents = gen;
  refreshSelectionValue();
  damageGeneratorParts();
}

// Nearest visible EDO marker within kSnapPx of the pointer: exactly its cents (m·period/n).
double MosChart::snapGenerator(double gen, float pointerX) const noexcept {
  ensureLayout();
  const float lim = UiScale(layoutScale_).exact(mcm::kSnapPx);
  const MosChartEdoMarker* best = nullptr;
  float bestD = lim;
  for (const MosChartEdoMarker& m : markers_) {
    const float d = std::fabs(m.x - pointerX);
    if (d <= bestD) {
      bestD = d;
      best = &m;
    }
  }
  return best ? best->cents : gen;
}

// setGenFromScreenX: follow the pointer inside the selected band; past its ends switch to the adjacent region
// (findAdjacentRegion) and carry the mode across; then snap.
void MosChart::dragGenerator(float x, bool snap) {
  if (selected_ < 0) return;
  const double P = config_.period;
  const double cc = reduce(centsAtX(x));
  const tu::MosRegion& sel = regions_.regions[static_cast<size_t>(selected_)];
  if (cc < sel.lowCents || cc > sel.highCents) {
    const double clamped = std::clamp(cc, 0.0, P);
    const int32_t adj = tu::findAdjacentRegion(regions_.regions, sel, clamped, firstSize_, config_.maxSize);
    if (adj >= 0) {
      const tu::MosRegion& a = regions_.regions[static_cast<size_t>(adj)];
      const double newGen = std::clamp(clamped, a.lowCents, a.highCents);
      int32_t rot = selection_ ? selection_->modeRotation : 0;
      if (selection_ && !(a.key == sel.key)) {
        const tu::RegionHint hint = a.hint();
        rot = tu::transferModeRotation(selection_->generatorCents, sel.perPeriodSize, rot, newGen, a.perPeriodSize, P, &hint);
      }
      setSelectedRegion(adj, newGen, rot);
      invalidate();
      if (snap) {
        const double sg = snapGenerator(newGen, x);
        if (sg != newGen) {
          selection_->generatorCents = sg;
          refreshSelectionValue();
        }
      }
      return;
    }
  }
  double gen = std::clamp(cc, sel.lowCents, sel.highCents);
  if (snap) gen = snapGenerator(gen, x);
  setGenerator(gen);
}

// handleClick: EDO marker → exactly its generator; a band → select it (or set the generator in the selected one);
// elsewhere: set the generator inside the selected range, or clear the selection.
void MosChart::handleClick(Vec2 p) {
  const std::optional<MosSelection> before = selection_;
  if (const MosChartEdoMarker* m = edoMarkerAt(p)) {
    setGenerator(m->cents);
  } else if (const int32_t hit = regionAt(p); hit >= 0) {
    const tu::MosRegion& r = regions_.regions[static_cast<size_t>(hit)];
    const double gen = std::clamp(reduce(centsAtX(p.x)), r.lowCents, r.highCents);
    if (selected_ >= 0 && sameRegion(selected_, hit)) {
      setGenerator(gen);
    } else {
      const bool sameKey = selected_ >= 0 && regions_.regions[static_cast<size_t>(selected_)].key == r.key;
      setSelectedRegion(hit, gen, sameKey && selection_ ? selection_->modeRotation : 0);
      invalidate();
    }
  } else if (selected_ >= 0) {
    const tu::MosRegion& sel = regions_.regions[static_cast<size_t>(selected_)];
    const double cc = reduce(centsAtX(p.x));
    if (cc >= sel.lowCents && cc <= sel.highCents) {
      setGenerator(cc);
    } else {
      setSelectedRegion(-1, 0.0, 0);
      invalidate();
    }
  }
  if (selection_ != before) emit(MosGesture::Commit);
}

// --- hover card ---------------------------------------------------------------------------------------------------

void MosChart::hideCard() {
  if (card_.rect.empty() && card_.lines.empty()) return;
  damageRect(card_.rect);
  card_.rect = {};
  card_.lines.clear();
  infoTitle_.clear();
  infoBody_.clear();
}

// The explorer's tooltip: name, range and size, parent, hardness, UDP at the cursor, temperaments at the cursor,
// the nearest EDO generator, and what a click does.
void MosChart::buildCard(int32_t region, Vec2 p) {
  const tu::MosRegion& r = regions_.regions[static_cast<size_t>(region)];
  const double P = config_.period;
  const double ccRaw = centsAtX(p.x);
  const double cc = reduce(ccRaw);
  std::vector<std::string>& L = card_.lines;
  L.clear();
  const auto fname = tu::mosDisplayName(r.key, periodReps(), P);
  L.push_back(labelsOf(region).label + (fname ? " \xE2\x80\x94 " + std::string(*fname) : std::string()));
  char buf[160];
  int n = std::snprintf(buf, sizeof buf, "%.2f\xC2\xA2 \xE2\x80\x94 %.2f\xC2\xA2 \xC2\xB7 %d notes", r.lowCents, r.highCents, r.size);
  L.emplace_back(buf, n > 0 ? static_cast<size_t>(n) : 0);
  if (r.parent) {
    const auto pn = tu::mosDisplayName(*r.parent, periodReps(), P);
    L.push_back("Parent: " + tu::mosDisplayLabel(*r.parent) + (pn ? " (" + std::string(*pn) + ")" : std::string()));
  }
  const tu::Hardness hr = tu::computeHardness(cc, r.perPeriodSize, P);
  if (std::isfinite(hr.ratio)) {
    n = std::snprintf(buf, sizeof buf, "L:s = %s   (L=%.1f\xC2\xA2, s=%.1f\xC2\xA2)", hr.str.c_str(), hr.L, hr.s);
    L.emplace_back(buf, n > 0 ? static_cast<size_t>(n) : 0);
  }
  const tu::RegionHint hint = r.hint();
  if (const auto udp = tu::computeUdp(cc, r.perPeriodSize, 0, P, &hint)) L.push_back("UDP " + *udp);
  if (std::fabs(config_.octaveStretch) <= 5.0) {
    const auto temps = tu::temperaments();
    std::string t;
    for (const tu::TemperamentMatch& m : tu::temperamentsAtCursor(r.perPeriodKey, cc, P, {r.lowCents, r.highCents})) {
      if (!t.empty()) t += ", ";
      t += temps[static_cast<size_t>(m.index)].name;
    }
    if (!t.empty()) L.push_back(t);
  }
  const tu::EdoGenerator* nearest = nullptr;
  double nd = std::numeric_limits<double>::infinity();
  for (const tu::EdoGenerator& g : edos_) {
    const double d = std::fabs(g.cents - cc);
    if (d < nd && g.cents >= r.lowCents && g.cents <= r.highCents) {
      nd = d;
      nearest = &g;
    }
  }
  if (nearest && nd < (viewMax_ - viewMin_) * 0.02) {
    const double octEdo = nearest->edo * (1200.0 / P);
    const bool clean = std::fabs(octEdo - std::round(octEdo)) < 0.001;
    const double nearK = std::round((ccRaw - nearest->cents) / P);
    const double dispCents = nearest->cents + std::max(0.0, nearK) * P;
    const long long dispEdo = clean ? std::llround(octEdo) : static_cast<long long>(nearest->edo) * periodReps();
    const long long dispStep = clean ? std::llround(dispCents * static_cast<double>(dispEdo) / 1200.0)
                                     : nearest->step + static_cast<long long>(std::max(0.0, nearK)) * nearest->edo;
    n = std::snprintf(buf, sizeof buf, "Nearest: %lld\\%lld = %.2f\xC2\xA2", dispStep, dispEdo, dispCents);
    L.emplace_back(buf, n > 0 ? static_cast<size_t>(n) : 0);
  }
  L.emplace_back(selected_ >= 0 && sameRegion(selected_, region) ? "Click or drag to set the generator" : "Click to select");

  infoTitle_ = L[0];
  infoBody_.clear();
  for (size_t i = 1; i < L.size(); ++i) {
    if (i > 1) infoBody_ += "\n";
    infoBody_ += L[i];
  }

  // Size and place it (below-right of the pointer, flipped to stay inside the chart).
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  if (!res || !res->text || !res->uiFonts) {
    card_.rect = {};
    return;
  }
  const UiScale s(layoutScale_);
  const FontId title = res->uiFonts->bodyStrong, body = res->uiFonts->small;
  const int32_t pad = s.px(7.0f);
  int32_t w = 0, h = pad * 2;
  for (size_t i = 0; i < L.size(); ++i) {
    const FontId f = i == 0 ? title : body;
    w = std::max(w, res->text->measure(f, L[i]));
    h += res->text->metrics(f).lineHeight;
  }
  w = std::min(w + 2 * pad, std::max(s.px(120.0f), rect().w - s.px(8.0f)));
  const IRect R = rect();
  int32_t x = static_cast<int32_t>(p.x) + s.px(15.0f), y = static_cast<int32_t>(p.y) + s.px(15.0f);
  if (x + w > R.right()) x = static_cast<int32_t>(p.x) - w - s.px(20.0f);
  if (y + h > R.bottom()) y = static_cast<int32_t>(p.y) - h - s.px(10.0f);
  x = std::clamp(x, R.x + s.px(4.0f), std::max(R.x, R.right() - w - s.px(4.0f)));
  y = std::clamp(y, R.y + s.px(4.0f), std::max(R.y, R.bottom() - h - s.px(4.0f)));
  card_.rect = {x, y, w, h};
}

void MosChart::updateHover(Vec2 p, bool force) {
  const int32_t hit = regionAt(p);
  if (hit != hovered_) {
    if (selected_ < 0) {  // the hover highlight shows only with nothing selected
      damageRegionBands(hovered_);
      damageRegionBands(hit);
    }
    hovered_ = hit;
  } else if (!force && hit < 0) {
    return;
  }
  if (hit >= 0) {
    damageRect(card_.rect);
    buildCard(hit, p);
    damageRect(card_.rect);
  } else {
    hideCard();
  }
}

void MosChart::onHoverChanged(bool hov) {
  if (!hov && drag_ == Drag::None) {
    if (selected_ < 0) damageRegionBands(hovered_);
    hovered_ = -1;
    hideCard();
  }
}

InfoText MosChart::info() const {
  if (!infoTitle_.empty()) return {infoTitle_, infoBody_};
  return {"MOS Region Chart",
          "Generator (cents) across, MOS size down. Click a band to select its MOS; drag the axis or the marker to "
          "move the generator (snaps to EDO markers, Alt bypasses). Wheel zooms, Ctrl+wheel row height, "
          "Shift+wheel or the right scrollbar scrolls the sizes."};
}

// --- input --------------------------------------------------------------------------------------------------------

bool MosChart::onPointer(const PointerEvent& e) {
  const UiScale s(tree() ? tree()->scale() : 1.0f);
  switch (e.type) {
    case PointerType::Wheel: {
      if (drag_ == Drag::Generator) return true;
      ensureLayout();
      if (e.mods & mod::kCtrl) {
        setRowScale(rowScale_ * static_cast<float>(std::pow(mcm::kZoomStep, static_cast<double>(e.wheel.y))));
      } else if (e.mods & mod::kShift) {
        const float d = e.wheel.y != 0.0f ? e.wheel.y : e.wheel.x;
        setFirstSize(firstSize_ - static_cast<int32_t>(std::lround(d)));
      } else {
        const double mc = centsAtX(e.pos.x);
        const double range = viewMax_ - viewMin_;
        const double nr = std::clamp(range / std::pow(mcm::kZoomStep, static_cast<double>(e.wheel.y)), mcm::kMinViewCents, viewLimit());
        // Keep the cents under the pointer fixed (the explorer's ratio ignored its margins).
        const double ratio = (static_cast<double>(e.pos.x) - geo_.x0) / geo_.w;
        setView(mc - ratio * nr, mc - ratio * nr + nr);
      }
      updateHover(e.pos, true);
      return true;
    }
    case PointerType::Down: {
      ensureLayout();
      if (e.button == PointerButton::Right && drag_ == Drag::Generator) {  // the explorer: right-click cancels a drag
        onKey(KeyEvent{true, false, mod::kNone, Scancode::Escape, key::kEscape});
        return true;
      }
      if (e.button != PointerButton::Left) return false;
      if (tree() && focusable()) tree()->setFocus(this);
      hideCard();
      pressPos_ = e.pos;
      pressViewMin_ = viewMin_;
      pressViewMax_ = viewMax_;
      pressFirstSize_ = firstSize_;
      pressSelection_ = selection_;
      pressSelected_ = selected_;
      const IRect sb = scrollbarRect();
      if (sb.contains(e.pos)) {
        drag_ = Drag::Scrollbar;
        const IRect th = thumbRect();
        pressScrollOffset_ = th.contains(e.pos) ? static_cast<int32_t>(e.pos.y) - th.y : th.h / 2;
        onPointer(PointerEvent{PointerType::Move, PointerButton::None, 0, e.mods, e.pos, {}});
        return true;
      }
      if (selected_ >= 0 && selection_) {
        const tu::MosRegion& sel = regions_.regions[static_cast<size_t>(selected_)];
        const double cc = reduce(centsAtX(e.pos.x));
        const bool onAxis = e.pos.y <= static_cast<float>(geo_.r.y + s.px(mcm::kAxisHitBottom)) && cc >= sel.lowCents &&
                            cc <= sel.highCents;
        bool onMarker = false;
        if (e.pos.y >= static_cast<float>(geo_.r.y + s.px(mcm::kAxisY - 4.0f)))
          for (int32_t k = 0; k < periodReps() && !onMarker; ++k)
            onMarker = std::fabs(xOfCents(selection_->generatorCents + k * config_.period) - e.pos.x) <= s.exact(mcm::kMarkerGrabPx);
        if (onAxis || onMarker) {
          drag_ = Drag::Generator;
          if (onAxis) dragGenerator(e.pos.x, (e.mods & mod::kAlt) == 0);
          if (selection_ != pressSelection_) emit(MosGesture::Live);
          return true;
        }
      }
      drag_ = Drag::Pending;
      return true;
    }
    case PointerType::Move: {
      switch (drag_) {
        case Drag::None:
          updateHover(e.pos, true);
          return false;
        case Drag::Pending: {
          const float slop = s.exact(mcm::kClickSlop);
          if (std::fabs(e.pos.x - pressPos_.x) < slop && std::fabs(e.pos.y - pressPos_.y) < slop) return true;
          drag_ = Drag::Pan;
          [[fallthrough]];
        }
        case Drag::Pan: {
          // Horizontal: pan by the pointer's cents; vertical: 1 size per 20 logical px (the explorer's 0.05).
          const double cpp = (pressViewMax_ - pressViewMin_) / geo_.w;
          const double dx = static_cast<double>(e.pos.x - pressPos_.x);
          setView(pressViewMin_ - dx * cpp, pressViewMax_ - dx * cpp);
          const double dy = static_cast<double>(e.pos.y - pressPos_.y) / static_cast<double>(s.factor);
          setFirstSize(static_cast<int32_t>(std::lround(std::clamp(pressFirstSize_ - dy * 0.05, 2.0, 100.0))));
          return true;
        }
        case Drag::Generator: {
          const std::optional<MosSelection> before = selection_;
          dragGenerator(e.pos.x, (e.mods & mod::kAlt) == 0);
          if (selection_ != before) emit(MosGesture::Live);
          return true;
        }
        case Drag::Scrollbar: {
          const IRect sb = scrollbarRect();
          const IRect th = thumbRect();
          const int32_t travel = sb.h - th.h;
          const int32_t span = config_.maxSize - 2;
          if (travel > 0 && span > 0) {
            const double t = std::clamp(static_cast<double>(static_cast<int32_t>(e.pos.y) - pressScrollOffset_ - sb.y) / travel, 0.0, 1.0);
            setFirstSize(2 + static_cast<int32_t>(std::lround(t * span)));
          }
          return true;
        }
      }
      return false;
    }
    case PointerType::Up: {
      if (e.button != PointerButton::Left) return drag_ != Drag::None;
      const Drag d = drag_;
      drag_ = Drag::None;
      if (d == Drag::Pending) handleClick(e.pos);
      if (d == Drag::Generator && selection_ != pressSelection_) emit(MosGesture::Commit);
      if (d == Drag::Scrollbar || d == Drag::Generator) markDirty();
      updateHover(e.pos, true);
      return d != Drag::None;
    }
    case PointerType::Leave:
      if (drag_ == Drag::None) onHoverChanged(false);
      return false;
  }
  return false;
}

bool MosChart::onKey(const KeyEvent& e) {
  if (!e.down || e.key != key::kEscape || drag_ != Drag::Generator) return false;
  drag_ = Drag::None;
  // Back to the selection at the press.
  if (pressSelected_ != selected_) {
    selected_ = pressSelected_;
    selection_ = pressSelection_;
    if (selected_ >= 0)
      equalizedEndLow_ = tu::equalizedEnd(regions_.regions[static_cast<size_t>(selected_)], config_.period) == tu::RegionEnd::Low ? 1 : 0;
    invalidate();
  } else if (selection_ && pressSelection_) {
    damageGeneratorParts();
    selection_ = pressSelection_;
    damageGeneratorParts();
  }
  emit(MosGesture::Cancel);
  if (tree() && tree()->captured() == this) tree()->releaseCapture();
  return true;
}

void MosChart::onCaptureLost() {
  // The capture ended without a release (window lost focus): a generator drag keeps its value, as on release.
  const Drag d = drag_;
  drag_ = Drag::None;
  if (d == Drag::Generator && selection_ != pressSelection_) emit(MosGesture::Commit);
}

IRect MosChart::thumbRect() const noexcept {
  const IRect sb = scrollbarRect();
  const int32_t total = config_.maxSize - 1;  // sizes 2..maxSize
  const int32_t visible = std::max(1, geo_.rowCount);
  const UiScale s(layoutScale_);
  int32_t h = sb.h;
  if (visible < total) h = std::max(s.px(20.0f), static_cast<int32_t>(static_cast<int64_t>(sb.h) * visible / total));
  h = std::min(h, sb.h);
  const int32_t span = config_.maxSize - 2;
  const int32_t y = span > 0 ? sb.y + static_cast<int32_t>(static_cast<int64_t>(sb.h - h) * (firstSize_ - 2) / span) : sb.y;
  const int32_t inset = std::max(1, s.px(2.0f));
  return {sb.x + inset, y, std::max(1, sb.w - 2 * inset), h};
}

// --- paint ----------------------------------------------------------------------------------------------------------

void MosChart::paint(PaintContext& ctx) {
  const Theme* thp = ctx.res.theme;
  if (!thp) return;
  const Theme::MosChart& T = thp->mosChart;
  ensureLayout();
  const IRect R = rect();
  const IRect clip = intersect(R, ctx.clip);
  if (clip.empty()) return;
  ++stats_.paints;
  stats_.bandsPainted = 0;
  stats_.labelsPainted = 0;
  DrawList& dl = ctx.dl;
  const UiScale s = ctx.scale;
  const Geo& g = geo_;
  TextSystem* ts = ctx.res.text;
  const ThemeFonts* fonts = ctx.res.uiFonts;
  const FontId fSmall = fonts ? fonts->small : kNoFont;
  const FontId fStrong = fonts ? fonts->bodyStrong : kNoFont;
  const FontId fNum = fonts ? fonts->numeric : kNoFont;
  const bool text = ts && fonts;
  const double vr = viewMax_ - viewMin_;
  auto c2x = [&](double c) { return static_cast<float>(g.x0 + (c - viewMin_) / vr * g.w); };
  auto Y = [&](float logical) { return static_cast<float>(g.r.y + s.px(logical)); };
  const float plotL = static_cast<float>(g.plot.x), plotR = static_cast<float>(g.plot.right());
  const float bottom = static_cast<float>(R.bottom());
  const double P = config_.period;
  const int32_t reps = periodReps();
  // T3.x-fix-look (M3 review L18): labels of one row never overprint: each row has a LabelSpace, and a label that
  // would touch one already placed is skipped. Placement runs over every label of the row in priority order (not only
  // those in the clip), so partial and full repaints skip the same ones.
  auto centred = [&](FontId f, std::string_view str, float cx, float baseline, Rgba col, paint::LabelSpace* lane = nullptr) {
    if (!text || f == kNoFont || str.empty()) return;
    const TextRun run = ts->shape(f, str);
    const float x = std::round(cx - static_cast<float>(run.advance) / 2.0f);
    if (lane && !lane->place(static_cast<int32_t>(x), static_cast<int32_t>(x) + run.advance)) return;
    if (!overlaps(clip, x, baseline - static_cast<float>(ts->metrics(f).ascent), x + static_cast<float>(run.advance),
                  baseline + static_cast<float>(ts->metrics(f).descent)))
      return;
    TextSystem::drawRun(dl, run, {x, baseline}, col);
    ++stats_.labelsPainted;
  };

  dl.pushClip(clip.toRect());
  dl.addRectFilled(clip.toRect(), T.background);

  // --- axis (drawAxis) ---
  const double tick = axisTick(vr);
  const float barTop = Y(mcm::kAxisY - mcm::kAxisBarH / 2), barBot = Y(mcm::kAxisY + mcm::kAxisBarH / 2);
  const auto i0 = static_cast<int64_t>(std::ceil(viewMin_ / tick)), i1 = static_cast<int64_t>(std::floor(viewMax_ / tick));
  // Per paint: a few dozen extents at most (the chart repaints on interaction, never per playback frame).
  const int32_t labelGap = s.px(4.0f);
  paint::LabelSpace axisLabels(labelGap);
  paint::LabelSpace edoLabels[3] = {paint::LabelSpace(labelGap), paint::LabelSpace(labelGap), paint::LabelSpace(labelGap)};
  for (int64_t i = i0; i <= i1; ++i) {
    const float x = c2x(static_cast<double>(i) * tick);
    if (overlaps(clip, x - 1, Y(mcm::kAxisY), x + 1, bottom)) dl.addVLine(x, Y(mcm::kAxisY), bottom, T.grid, 1.0f);
  }
  if (overlaps(clip, static_cast<float>(R.x), barTop, static_cast<float>(R.right()), barBot)) {
    const Rect bar{static_cast<float>(R.x), barTop, static_cast<float>(R.right()), barBot};
    dl.addRectFilled(bar, T.axis);
    dl.addHLine(bar.x0, bar.x1, barTop, T.axisBorder);
    dl.addHLine(bar.x0, bar.x1, barBot - 1.0f, T.axisBorder);
  }
  const float tickTop = Y(mcm::kAxisY - mcm::kAxisBarH / 2 - 7), tickBot = Y(mcm::kAxisY + mcm::kAxisBarH / 2 + 7);
  const float labelBase = Y(mcm::kAxisY - mcm::kAxisBarH / 2 - 11);
  for (int64_t i = i0; i <= i1; ++i) {
    const double c = static_cast<double>(i) * tick;
    const float x = c2x(c);
    if (x < plotL - 20 || x > plotR + 20) continue;
    if (overlaps(clip, x - 2, tickTop, x + 2, tickBot)) dl.addVLine(x, tickTop, tickBot, T.axisTick, s.pxf(2.0f));
    const double sc = tu::stretchCents(c, config_.octaveStretch);
    char buf[32];
    const int n = vr > 30 ? std::snprintf(buf, sizeof buf, "%lld\xC2\xA2", static_cast<long long>(std::llround(sc)))
                          : std::snprintf(buf, sizeof buf, "%.1f\xC2\xA2", sc);
    centred(fNum, std::string_view(buf, n > 0 ? static_cast<size_t>(n) : 0), x, labelBase, T.axisText, &axisLabels);
  }
  const double sub = tick / 5;
  if (sub > 0.005) {
    const auto j0 = static_cast<int64_t>(std::ceil(viewMin_ / sub)), j1 = static_cast<int64_t>(std::floor(viewMax_ / sub));
    const float st = Y(mcm::kAxisY - mcm::kAxisBarH / 2 - 3), sbm = Y(mcm::kAxisY + mcm::kAxisBarH / 2 + 3);
    for (int64_t j = j0; j <= j1; ++j) {
      if (j % 5 == 0) continue;  // under a major tick
      const float x = c2x(static_cast<double>(j) * sub);
      if (x < plotL || x > plotR) continue;
      if (overlaps(clip, x - 1, st, x + 1, sbm)) dl.addVLine(x, st, sbm, T.axisSubTick, 1.0f);
    }
  }
  if (reps > 1)
    for (int32_t k = 1; k < reps; ++k) {
      const float x = c2x(k * P);
      if (x >= plotL && x <= plotR && overlaps(clip, x - 1, static_cast<float>(R.y), x + 1, bottom))
        dashedVLine(dl, clip, x, static_cast<float>(R.y), bottom, s.pxf(6.0f), s.pxf(4.0f), T.periodLine, 1.0f);
    }

  // --- EDO markers (drawEDOs) ---
  // Markers come lowest n first (the explorer's priority): a label that would touch a placed one is skipped, row by
  // row ("23\40 7\12 29\49 3\5" and "690.0¢700.0¢710.2¢" ran together at a zoomed view).
  for (size_t i = 0; i < markers_.size(); ++i) {
    const MosChartEdoMarker& m = markers_[i];
    if (overlaps(clip, m.x - 1.0f, Y(mcm::kEdoTop), m.x + 1.0f, Y(mcm::kEdoTop + 18)))
      dl.addVLine(m.x, Y(mcm::kEdoTop), Y(mcm::kEdoTop + 18), T.edoLine, 1.0f);
    centred(fStrong, markerLabels_[3 * i], m.x, Y(88), T.edoText, &edoLabels[0]);
    centred(fSmall, markerLabels_[3 * i + 1], m.x, Y(102), T.edoCents, &edoLabels[1]);
    centred(fSmall, markerLabels_[3 * i + 2], m.x, Y(116), T.edoHardness, &edoLabels[2]);
  }

  // --- temperament ranges (drawTemperaments) ---
  const auto temps = tu::temperaments();
  for (const TempBar& b : tempBars_) {
    if (!overlaps(clip, b.x1, static_cast<float>(b.y), b.x2, static_cast<float>(b.y + b.h))) continue;
    const Rect rr{b.x1, static_cast<float>(b.y), b.x2, static_cast<float>(b.y + b.h)};
    dl.addRectFilled(rr, b.fill);
    dl.addRectStroke(rr, b.edge);
    if (text && b.x2 - b.x1 > s.exact(35.0f)) {
      const std::string_view name = temps[static_cast<size_t>(b.temperament)].name;
      const int32_t maxW = static_cast<int32_t>(b.x2 - b.x1) - s.px(6.0f);
      const TextRun run = ts->shapeTruncated(fStrong, name, maxW);
      const float x = std::round((b.x1 + b.x2) / 2 - static_cast<float>(run.advance) / 2);
      const auto base = static_cast<float>(paint::capCentredBaseline(b.y, b.h, ts->metrics(fStrong).pixelSize));
      TextSystem::drawRun(dl, run, {x, base}, T.bandText);
      ++stats_.labelsPainted;
    }
  }

  // --- MOS bands (drawMOS) ---
  const Rgba grayedBase = T.bandGrayed;
  const uint32_t capStrong = text ? ts->metrics(fStrong).pixelSize : 0, capSmall = text ? ts->metrics(fSmall).pixelSize : 0;
  const float capS = 0.727f * static_cast<float>(capStrong), capT = 0.727f * static_cast<float>(capSmall);
  for (const Band& b : bands_) {
    if (!overlaps(clip, b.x1, static_cast<float>(b.y), b.x2, static_cast<float>(b.y + b.h))) continue;
    ++stats_.bandsPainted;
    const bool isSel = selected_ >= 0 && sameRegion(selected_, b.region);
    const bool isHov = selected_ < 0 && hovered_ == b.region;
    const bool grayed = selected_ >= 0 && !isSel;
    const float alpha = isSel ? 0.85f : grayed ? 0.12f : isHov ? 0.7f : 0.5f;
    const Rgba base = isSel ? regionColor(selected_) : regionColor(b.region);
    const Rgba col = grayed ? grayedBase : base;
    const Rect rr{b.x1, static_cast<float>(b.y), b.x2, static_cast<float>(b.y + b.h)};
    dl.addRectFilled(rr, alphaOfFloat(col, alpha));
    const bool active = isSel || (isHov && !grayed);
    dl.addRectStroke(rr, active ? T.bandActive : alphaOfFloat(col, grayed ? 0.2f : 0.7f), active ? s.pxf(2.0f) : 1.0f);

    // A band's label stays clear of the size column at the left edge (a band running under it read "132L 11s":
    // size 13 + "2L 11s"; T3.x-fix-look, L18): it is centred in the part of the band right of the column.
    const float lx1 = std::max(b.x1, static_cast<float>(g.r.x + s.px(mcm::kSizeLabelRight + 6.0f)));
    const float w = b.x2 - lx1;
    if (!text || !(w > s.exact(24.0f)) || (grayed && !(w > s.exact(50.0f)))) continue;
    const float cx = (lx1 + b.x2) / 2;
    const int32_t maxW = static_cast<int32_t>(w) - s.px(6.0f);
    const Rgba tc = grayed ? T.bandTextDim : T.bandText;
    auto line = [&](FontId f, std::string_view str, float baseline, Rgba c) {
      const TextRun run = ts->shape(f, str);
      if (run.advance > maxW) return false;
      TextSystem::drawRun(dl, run, {std::round(cx - static_cast<float>(run.advance) / 2), baseline}, c);
      ++stats_.labelsPainted;
      return true;
    };
    const float fh = static_cast<float>(b.h);
    const Labels& lb = labelsOf(b.region);
    const bool named = w > s.exact(110.0f) && !lb.name.empty() && !grayed;
    if (named && fh >= capS + capT + s.exact(12.0f)) {
      // Two lines (rows of ≥ ~30 px): "5L 2s (Diatonic)" and the range, the block centred in the row.
      const float spare = fh - capS - capT;
      const float b1 = std::round(static_cast<float>(b.y) + spare * 0.32f + capS);
      const float b2 = std::round(b1 + spare * 0.36f + capT);
      if (!line(fStrong, lb.name, b1, tc)) line(fStrong, lb.label, b1, tc);
      line(fSmall, lb.range, b2, T.bandSubText);
      continue;
    }
    const FontId f = (w > s.exact(50.0f) && !grayed) ? fStrong : fSmall;
    const auto base1 = static_cast<float>(paint::capCentredBaseline(b.y, b.h, ts->metrics(f).pixelSize));
    if (named && line(f, lb.name, base1, tc)) continue;
    if (!line(f, lb.label, base1, tc)) line(f, lb.compact, base1, tc);
  }

  // Size labels (right-aligned at x = 16).
  if (text)
    for (int32_t row = 0; row < g.rowCount; ++row) {
      const int32_t y = g.rowsTop + row * g.rowPitch;
      char buf[16];
      const int n = std::snprintf(buf, sizeof buf, "%d", (firstSize_ + row) * reps);
      const TextRun run = ts->shape(fSmall, std::string_view(buf, n > 0 ? static_cast<size_t>(n) : 0));
      const float x = static_cast<float>(g.r.x + s.px(mcm::kSizeLabelRight) - run.advance);
      const auto base = static_cast<float>(paint::capCentredBaseline(y, g.rowH, ts->metrics(fSmall).pixelSize));
      if (!overlaps(clip, x, base - capT - 2, x + static_cast<float>(run.advance), base + 2)) continue;
      TextSystem::drawRun(dl, run, {x, base}, T.sizeText);
    }

  // --- generator marker (drawGeneratorMarker) ---
  if (selection_) {
    bool labelled = false;
    const float mTop = Y(mcm::kAxisY - 4.0f);
    for (int32_t k = 0; k < reps; ++k) {
      const float x = c2x(selection_->generatorCents + k * P);
      if (x < plotL - 10 || x > plotR + 10) continue;
      const float half = s.exact(mcm::kMarkerGrabPx + 2.0f);
      if (overlaps(clip, x - half, mTop, x + half, bottom)) {
        dashedVLine(dl, clip, x, mTop, bottom - s.pxf(10.0f), s.pxf(4.0f), s.pxf(3.0f), T.marker, s.pxf(2.0f));
        dl.addTriangleFilled({x - s.exact(5.0f), mTop}, {x + s.exact(5.0f), mTop}, {x, mTop + s.exact(8.0f)}, T.marker);
      }
      if (!labelled) {
        labelled = true;
        const IRect pr = pillRect(x);
        if (overlaps(clip, static_cast<float>(pr.x), static_cast<float>(pr.y), static_cast<float>(pr.right()), static_cast<float>(pr.bottom()))) {
          dl.addRoundedRectFilled(pr.toRect(), s.exact(3.0f), T.marker);
          if (text) {
            char buf[48];
            const int n = std::snprintf(buf, sizeof buf, "%.1f\xC2\xA2", tu::stretchCents(selection_->generatorCents, config_.octaveStretch));
            const TextRun run = ts->shape(fStrong, std::string_view(buf, n > 0 ? static_cast<size_t>(n) : 0));
            const auto base = static_cast<float>(paint::capCentredBaseline(pr.y, pr.h, ts->metrics(fStrong).pixelSize));
            TextSystem::drawRun(dl, run, {static_cast<float>(pr.x + (pr.w - run.advance) / 2), base}, T.markerText);
          }
        }
      }
    }
  }

  // --- size scrollbar (drawSizeScrollbar), right of the plot ---
  {
    const IRect sb = scrollbarRect();
    if (!sb.empty() && overlaps(clip, static_cast<float>(sb.x), static_cast<float>(sb.y), static_cast<float>(sb.right()), static_cast<float>(sb.bottom()))) {
      dl.addRectFilled(sb.toRect(), thp->scrollbar.track);
      const IRect th = thumbRect();
      dl.addRoundedRectFilled(th.toRect(), s.exact(3.0f), drag_ == Drag::Scrollbar ? thp->scrollbar.thumbHover : thp->scrollbar.thumb);
      if (text && th.h >= static_cast<int32_t>(capT) + s.px(6.0f)) {
        char buf[16];
        const int n = std::snprintf(buf, sizeof buf, "%d", firstSize_ * reps);
        const TextRun run = ts->shape(fSmall, std::string_view(buf, n > 0 ? static_cast<size_t>(n) : 0));
        if (run.advance <= th.w + s.px(2.0f)) {
          const auto base = static_cast<float>(paint::capCentredBaseline(th.y, std::min(th.h, s.px(20.0f)), ts->metrics(fSmall).pixelSize));
          TextSystem::drawRun(dl, run, {static_cast<float>(th.x + (th.w - run.advance) / 2), base}, thp->text.primary);
        }
      }
    }
  }

  // --- hover card ---
  if (!card_.rect.empty() && text &&
      overlaps(clip, static_cast<float>(card_.rect.x), static_cast<float>(card_.rect.y), static_cast<float>(card_.rect.right()),
               static_cast<float>(card_.rect.bottom()))) {
    paint::box(dl, card_.rect, 0.0f, thp->tooltip.fill, thp->tooltip.border);
    const int32_t pad = s.px(7.0f);
    float y = static_cast<float>(card_.rect.y + pad);
    for (size_t i = 0; i < card_.lines.size(); ++i) {
      const FontId f = i == 0 ? fStrong : fSmall;
      const FontMetrics& fm = ts->metrics(f);
      const Rgba c = i == 0 ? thp->tooltip.text : (i + 1 == card_.lines.size() ? thp->text.disabled : thp->text.secondary);
      ts->drawTruncated(dl, f, {static_cast<float>(card_.rect.x + pad), y + static_cast<float>(fm.ascent)}, card_.lines[i],
                        card_.rect.w - 2 * pad, c);
      y += static_cast<float>(fm.lineHeight);
    }
  }
  dl.popClip();
}

}  // namespace pq::views
