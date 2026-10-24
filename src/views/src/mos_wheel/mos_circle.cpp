// MOS wheel circle view (T3.14): a port of mos-explorer's drawMOSCircle (index.html ~3693–4650) with its mode
// buttons, parent / child arrows, hardness slider and MODMOS dot drag, restyled with the theme's `mosCircle.*`
// tokens. See pq/views/mos_wheel/mos_circle.h.

#include "pq/views/mos_wheel/mos_circle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <string_view>

#include "../common/paint.h"
#include "pq/tuning/js_number.h"
#include "pq/tuning/mos_data.h"
#include "pq/ui/lines.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"

namespace pq::views {

using namespace pq::ui;
namespace mcc = mos_circle_metrics;
namespace tu = pq::tuning;

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kInf = std::numeric_limits<double>::infinity();
// Hardness ticks on the slider (and its snap targets): supersoft 4:3 … superhard 4:1.
constexpr std::array<double, 7> kHardnessTicks = {4.0 / 3.0, 1.5, 5.0 / 3.0, 2.0, 2.5, 3.0, 4.0};

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

// lerpColor / complementaryColor (index.html 3127–3135): per channel, rounded, opaque.
Rgba lerpHex(Rgba a, Rgba b, double t) noexcept {
  auto ch = [&](int shift) {
    const double x = static_cast<double>((a >> shift) & 0xFFu), y = static_cast<double>((b >> shift) & 0xFFu);
    return static_cast<uint32_t>(std::clamp(std::lround(x + (y - x) * t), 0L, 255L));
  };
  return rgba(ch(0), ch(8), ch(16), 255u);
}
Rgba complement(Rgba c) noexcept {
  return rgba(255u - (c & 0xFFu), 255u - ((c >> 8) & 0xFFu), 255u - ((c >> 16) & 0xFFu), 255u);
}

std::string fmt(const char* f, double v) {
  char buf[48];
  const int n = std::snprintf(buf, sizeof buf, f, v);
  return std::string(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

// Exact (unrounded) largest and smallest step of the unrotated MOS.
std::pair<double, double> exactSteps(double gen, int32_t n, double period, const tu::RegionHint& hint) {
  const tu::MosScale sc = tu::buildMosScale(gen, n, period, &hint);
  if (sc.steps.empty()) return {0.0, 0.0};
  const auto [mn, mx] = std::minmax_element(sc.steps.begin(), sc.steps.end());
  return {*mx, *mn};
}

// Whether the region's L = s end is its low end (getEqualizedEnd).
bool equalizedLow(const MosSelection& sel) {
  const double lo = sel.regionLow + 0.01, hi = sel.regionHigh - 0.01;
  return tu::computeHardness(lo, sel.notesPerPeriod(), sel.period).ratio < tu::computeHardness(hi, sel.notesPerPeriod(), sel.period).ratio;
}

// A dashed antialiased line p0 → p1 (dash `on`, gap `off`, device px).
void dashedLine(DrawList& dl, Vec2 p0, Vec2 p1, float on, float off, Rgba col, float thick) {
  const float dx = p1.x - p0.x, dy = p1.y - p0.y;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (!(len > 0.0f) || on <= 0.0f) return;
  const float ux = dx / len, uy = dy / len;
  for (float t = 0.0f; t < len; t += on + off) {
    const float e = std::min(len, t + on);
    const Vec2 pts[2] = {{p0.x + ux * t, p0.y + uy * t}, {p0.x + ux * e, p0.y + uy * e}};
    smoothPolyline(dl, pts, 2, col, thick);
  }
}

void segment(DrawList& dl, Vec2 a, Vec2 b, Rgba col, float thick) {
  const Vec2 pts[2] = {a, b};
  smoothPolyline(dl, pts, 2, col, thick);
}

}  // namespace

// --- hardness ------------------------------------------------------------------------------------------------------

double exactHardness(const MosSelection& sel) {
  const auto [L, s] = exactSteps(sel.generatorCents, sel.notesPerPeriod(), sel.period, sel.hint());
  return s > 1e-9 ? L / s : kInf;
}

std::optional<double> generatorForHardnessIn(const MosSelection& sel, double ratio) {
  if (std::isnan(ratio)) return std::nullopt;
  const int32_t n = sel.notesPerPeriod();
  const double P = sel.period, lo = sel.regionLow, hi = sel.regionHigh, w = hi - lo;
  if (n < 2 || !(w > 0.0)) return std::nullopt;
  const bool eqLow = equalizedLow(sel);
  if (ratio <= 1.0) return eqLow ? lo : hi;  // the ends themselves (the explorer's slider reaches them)
  if (std::isinf(ratio)) return eqLow ? hi : lo;
  // L(g) and s(g) are linear inside a region (the chain's sorted order is fixed between the region's Farey ends), so
  // L − ratio·s = 0 is solved exactly from two interior points: the closed form of mos-calc's generatorForHardness
  // (which bisects 100 times on 0.01-cent-rounded steps, ~0.4 ms, and returns the collapsed end for regions that end
  // at 0 or P/2; Q83). A second step from the result absorbs floating-point rounding.
  const tu::RegionHint hint = sel.hint();
  auto f = [&](double g) {
    const auto [L, s] = exactSteps(g, n, P, hint);
    return L - ratio * s;
  };
  double g0 = lo + 0.25 * w, g1 = lo + 0.75 * w;
  double g = g0;
  for (int it = 0; it < 2; ++it) {
    const double f0 = f(g0), f1 = f(g1);
    if (!(std::fabs(f1 - f0) > 0.0)) break;
    const double next = g0 - f0 * (g1 - g0) / (f1 - f0);
    if (!std::isfinite(next)) break;
    g = std::clamp(next, lo, hi);
    // Second pass: a short secant around the result (inside the region).
    const double h = w * 1e-3;
    g0 = std::clamp(g, lo + h, hi - h);
    g1 = g0 + (g0 + h < hi ? h : -h);
  }
  return g;
}

// --- model -----------------------------------------------------------------------------------------------------------

// Everything drawn that depends on the value (not on the rect): rebuilt only when the selection, the MODMOS
// alterations, the config or the all-intervals toggle change.
struct MosCircle::Model {
  bool valid = false;
  int32_t n = 0, reps = 1;
  double P = 1200.0, displayP = 1200.0;
  double L = 0.0, s = 0.0;                 // display scale's (0.01-rounded) L and s
  std::vector<double> basePitches;         // the rotated, altered scale (one period)
  std::vector<double> pitches, steps, alts;  // displayed (repeated over the equave)
  std::vector<int32_t> origDegs;
  std::vector<Rgba> sliceColour;
  std::vector<std::string> sliceLabel, degreeLabel;
  enum Tone : uint8_t { Tonic, Info, ModeName, Modmos };
  struct Part {
    std::string text;
    Tone tone;
    int drop;  // dropped first: 3, then 2, then 1 (fitCircleTitleParts)
  };
  std::vector<Part> title;
  std::string mosLabel, pattern;
  bool modPattern = false;
  std::string tempName;
  Rgba tempColour = 0;
  bool hasModmos = false;
  std::string modStepStr;
  std::optional<tu::MosKey> parent, child;
  std::string parentLabel, childLabel;
  bool eqLow = true;
  double u = 0.0;  // hardness position: 0 equalized … 1 collapsed
  Rgba sliderColour = 0;
  std::vector<double> extra;  // all-intervals overlay (cents), when shown
};

MosCircle::MosCircle(std::string name, const MosChartConfig& config) : Widget(std::move(name)), config_(config) {
  setFocusable(true);
  palette_.reserve(tu::kMosPalette.size());
  for (const char* c : tu::kMosPalette) palette_.push_back(parseColor(c));
  rebuildRegions();
}

MosCircle::~MosCircle() = default;

void MosCircle::rebuildRegions() {
  config_.maxSize = std::clamp(config_.maxSize, 2, 64);
  config_.maxEdo = std::clamp(config_.maxEdo, 2, 400);
  config_.periodsPerEquave = std::max(1, config_.periodsPerEquave);
  if (!(config_.period > 0.0) || !std::isfinite(config_.period)) config_.period = 1200.0;
  regions_ = tu::buildMosRegions(config_.maxSize, config_.maxSize, config_.period, config_.periodsPerEquave);
  edos_ = tu::edoGenerators(config_.maxEdo, config_.period);
  for (tu::EdoGenerator& g : edos_) g.cents = static_cast<double>(g.step) * config_.period / static_cast<double>(g.edo);
  region_ = sel_ ? findRegion(*sel_) : -1;
}

void MosCircle::setConfig(const MosChartConfig& config) {
  if (config == config_) return;
  config_ = config;
  rebuildRegions();
  invalidate();
}

int32_t MosCircle::findRegion(const MosSelection& s) const noexcept {
  int32_t fallback = -1;
  for (size_t i = 0; i < regions_.regions.size(); ++i) {
    const tu::MosRegion& r = regions_.regions[i];
    if (!(r.perPeriodKey == s.perPeriodKey)) continue;
    if (std::fabs(r.lowCents - s.regionLow) < 0.01 && std::fabs(r.highCents - s.regionHigh) < 0.01) return static_cast<int32_t>(i);
    if (fallback < 0 && s.generatorCents >= r.lowCents && s.generatorCents <= r.highCents) fallback = static_cast<int32_t>(i);
  }
  return fallback;
}

void MosCircle::invalidate() {
  modelDirty_ = true;
  markDirty();
}

void MosCircle::setSelection(const std::optional<MosSelection>& sel) {
  if (sel == sel_) return;
  sel_ = sel;
  region_ = sel_ ? findRegion(*sel_) : -1;
  invalidate();
}

void MosCircle::setModmos(std::vector<double> modmos) {
  if (modmos == modmos_) return;
  modmos_ = std::move(modmos);
  invalidate();
}

std::optional<tu::MosParams> MosCircle::params() const {
  if (!sel_) return std::nullopt;
  tu::MosParams p = sel_->params();
  p.octaveStretch = config_.octaveStretch;
  if (tu::hasModmos(modmos_)) {
    p.modmos.assign(modmos_.begin(), modmos_.begin() + std::min<ptrdiff_t>(static_cast<ptrdiff_t>(modmos_.size()), p.n));
  }
  return p;
}

void MosCircle::emit(MosGesture g) {
  if (onChange_) onChange_(sel_, modmos_, g);
}

void MosCircle::setShowAllIntervals(bool on) {
  if (on == showAll_) return;
  showAll_ = on;
  invalidate();
}

// The selection moves to `region` (index into regions_) at `gen`, with `rotation`.
void MosCircle::selectRegion(int32_t region, double gen, int32_t rotation) {
  const tu::MosRegion& r = regions_.regions[static_cast<size_t>(region)];
  MosSelection s = *sel_;
  s.key = r.key;
  s.perPeriodKey = r.perPeriodKey;
  s.regionLow = r.lowCents;
  s.regionHigh = r.highCents;
  s.mirror = r.isMirror;
  s.generatorCents = gen;
  s.modeRotation = rotation >= 0 && rotation < r.perPeriodSize ? rotation : 0;
  s.edo.reset();
  for (const tu::EdoGenerator& e : edos_)
    if (std::fabs(e.cents - gen) < 1e-9) {
      s.edo = MosSelection::Edo{e.step, e.edo};
      break;
    }
  sel_ = s;
  region_ = region;
  invalidate();
}

bool MosCircle::modeAction(tu::ModeAction a) {
  if (!sel_ || sel_->notesPerPeriod() < 2) return false;
  const tu::RegionHint hint = sel_->hint();
  const tu::ModeChange mc = tu::applyModeAction(a, sel_->generatorCents, sel_->notesPerPeriod(), sel_->modeRotation, modmos_,
                                                sel_->period, &hint);
  if (mc.rotation < 0) return false;
  sel_->modeRotation = mc.rotation;
  modmos_ = mc.modmos;
  invalidate();
  emit(MosGesture::Commit);
  return true;
}

std::optional<tu::MosKey> MosCircle::parentKey() const {
  if (!sel_) return std::nullopt;
  return tu::findParentMos(sel_->key.nL, sel_->key.nS);
}

std::optional<tu::MosKey> MosCircle::childKey() const {
  if (!sel_) return std::nullopt;
  const std::array<tu::MosKey, 2> kids = tu::childMoses(sel_->key);
  const double g = sel_->generatorCents;
  for (const tu::MosKey& ck : kids)
    for (const tu::MosRegion& r : regions_.regions)
      if (r.key == ck && r.size == ck.size() && g >= r.lowCents && g <= r.highCents) return ck;
  return kids[0];
}

bool MosCircle::navigateToParent() {
  const std::optional<tu::MosKey> pk = parentKey();
  if (!pk) return false;
  const double g = sel_->generatorCents;
  for (size_t i = 0; i < regions_.regions.size(); ++i) {
    const tu::MosRegion& r = regions_.regions[i];
    if (r.key == *pk && r.perPeriodSize >= 2 && r.perPeriodSize <= config_.maxSize && g >= r.lowCents && g <= r.highCents &&
        r.lowCents < sel_->regionHigh && r.highCents > sel_->regionLow) {
      const tu::RegionHint hint = r.hint();
      const int32_t rot = tu::transferModeRotation(g, sel_->notesPerPeriod(), sel_->modeRotation, g, r.perPeriodSize, sel_->period, &hint);
      selectRegion(static_cast<int32_t>(i), g, rot);
      emit(MosGesture::Commit);
      return true;
    }
  }
  return false;
}

bool MosCircle::navigateToChild() {
  if (!sel_) return false;
  const double g = sel_->generatorCents;
  for (const tu::MosKey& ck : tu::childMoses(sel_->key))
    for (size_t i = 0; i < regions_.regions.size(); ++i) {
      const tu::MosRegion& r = regions_.regions[i];
      if (r.key == ck && r.perPeriodSize >= 2 && r.perPeriodSize <= config_.maxSize && g >= r.lowCents && g <= r.highCents) {
        const tu::RegionHint hint = r.hint();
        const int32_t rot = tu::transferModeRotation(g, sel_->notesPerPeriod(), sel_->modeRotation, g, r.perPeriodSize, sel_->period, &hint);
        selectRegion(static_cast<int32_t>(i), g, rot);
        emit(MosGesture::Commit);
        return true;
      }
    }
  return false;
}

bool MosCircle::setHardness(double ratio, MosGesture g) { return applyHardness(ratio, g, true); }

bool MosCircle::applyHardness(double ratio, MosGesture g, bool emitUnchanged) {
  if (!sel_) return false;
  std::optional<double> gen = generatorForHardnessIn(*sel_, ratio);
  if (!gen) return false;
  // An EDO generator within 1e-6 cents is taken exactly (L/s = 2 in 5L 2s is exactly 7\12).
  for (const tu::EdoGenerator& e : edos_)
    if (std::fabs(e.cents - *gen) < 1e-6 && e.cents >= sel_->regionLow && e.cents <= sel_->regionHigh) {
      gen = e.cents;
      break;
    }
  const bool changed = *gen != sel_->generatorCents;
  if (changed) {
    sel_->generatorCents = *gen;
    sel_->edo.reset();
    for (const tu::EdoGenerator& e : edos_)
      if (std::fabs(e.cents - *gen) < 1e-9) {
        sel_->edo = MosSelection::Edo{e.step, e.edo};
        break;
      }
    invalidate();
  }
  if (changed || emitUnchanged) emit(g);
  return true;
}

double MosCircle::hardness() const { return sel_ ? exactHardness(*sel_) : 1.0; }

void MosCircle::resetModmos() {
  const bool had = tu::hasModmos(modmos_);
  modmos_.clear();
  invalidate();
  if (had) emit(MosGesture::Commit);
}

void MosCircle::ensureModel() const {
  if (model_ && !modelDirty_) return;
  if (!model_) model_ = std::make_unique<Model>();
  modelDirty_ = false;
  ++stats_.models;
  Model& m = *model_;
  m = Model{};
  if (!sel_ || sel_->notesPerPeriod() < 1) return;
  const MosSelection& S = *sel_;
  const tu::RegionHint hint = S.hint();
  m.valid = true;
  m.n = S.notesPerPeriod();
  m.P = S.period;
  m.reps = std::max(1, S.periodsPerEquave);
  m.displayP = m.P * m.reps;
  const double gen = S.generatorCents;
  const tu::MosScale base = tu::buildMosScale(gen, m.n, m.P, &hint);
  const tu::MosScale rot = tu::rotateMosScale(base, S.modeRotation, m.P);
  m.hasModmos = tu::hasModmos(modmos_);
  std::vector<double> alts(static_cast<size_t>(m.n), 0.0);
  std::vector<int32_t> orig(static_cast<size_t>(m.n));
  for (int32_t i = 0; i < m.n; ++i) orig[static_cast<size_t>(i)] = i;
  std::vector<double> steps;
  if (m.hasModmos) {
    const tu::ModmosScale ms = tu::applyModmos(rot, modmos_, m.P);
    m.basePitches = ms.pitches;
    steps = ms.steps;
    m.L = ms.L;
    m.s = ms.s;
    if (ms.alts.size() == static_cast<size_t>(m.n)) alts = ms.alts;
    if (ms.origDegs.size() == static_cast<size_t>(m.n)) orig = ms.origDegs;
  } else {
    m.basePitches = rot.pitches;
    steps = rot.steps;
    m.L = rot.L;
    m.s = rot.s;
  }
  for (int32_t k = 0; k < m.reps; ++k)
    for (size_t i = 0; i < m.basePitches.size(); ++i) {
      m.pitches.push_back(m.basePitches[i] + k * m.P);
      m.steps.push_back(steps[i]);
      m.alts.push_back(alts[i]);
      m.origDegs.push_back(orig[i]);
    }

  // Step colours (drawMOSCircle): L in the region colour for this hardness, s toward its complement as the scale
  // hardens, MODMOS step sizes interpolated between their neighbours.
  Rgba collapsed = palette_[0], equalized = palette_[0];
  if (region_ >= 0) {
    const tu::MosRegion& r = regions_.regions[static_cast<size_t>(region_)];
    collapsed = palette_[r.collapsedColor % palette_.size()];
    equalized = palette_[r.equalizedColor % palette_.size()];
  }
  const double ratio = m.s > 0.01 ? m.L / m.s : 20.0;
  const double softness = std::clamp(1.0 - (ratio - 1.0) / 2.0, 0.0, 1.0);
  const Rgba Lc = lerpHex(collapsed, equalized, softness);
  const Rgba sc = lerpHex(Lc, complement(Lc), 1.0 - softness);
  auto bucket = [](double v) { return std::round(v * 100.0) / 100.0; };
  std::vector<double> uniq;
  for (double st : m.steps) {
    const double b = bucket(st);
    if (std::find(uniq.begin(), uniq.end(), b) == uniq.end()) uniq.push_back(b);
  }
  std::sort(uniq.begin(), uniq.end(), std::greater<>());
  const double bL = bucket(m.L), bS = bucket(m.s);
  std::vector<std::pair<double, Rgba>> colourOf{{bL, Lc}, {bS, sc}};
  auto known = [&](double b) -> const Rgba* {
    for (const auto& [k, c] : colourOf)
      if (k == b) return &c;
    return nullptr;
  };
  for (double st : uniq) {
    if (st == bL || st == bS) continue;
    double above = bL, below = bS;
    for (double v : uniq) {
      if (v > st && v < above && known(v)) above = v;
      if (v < st && v > below && known(v)) below = v;
    }
    const Rgba ac = known(above) ? *known(above) : Lc, bc = known(below) ? *known(below) : sc;
    const double range = above - below;
    const double t = range > 0.01 ? (st - below) / range : 0.5;
    const Rgba blend = lerpHex(bc, ac, t);
    const double pr = below > 0.01 ? st / below : 20.0;
    const double ps = std::clamp(1.0 - (pr - 1.0) / 2.0, 0.0, 1.0);
    const Rgba distinct = lerpHex(blend, complement(blend), 0.3);
    colourOf.emplace_back(st, lerpHex(distinct, blend, ps));
  }
  static constexpr std::string_view kMid = "MNOPQRTUVWXYZABCDEFGHIJKmno";
  for (double st : m.steps) {
    const double b = bucket(st);
    const Rgba* c = known(b);
    m.sliceColour.push_back(c ? *c : rgba(0x50, 0xC8, 0x78));
    const auto it = std::find(uniq.begin(), uniq.end(), b);
    const size_t idx = static_cast<size_t>(it - uniq.begin());
    if (uniq.size() == 1 || idx == 0) m.sliceLabel.emplace_back("L");
    else if (idx + 1 == uniq.size()) m.sliceLabel.emplace_back("s");
    else m.sliceLabel.emplace_back(idx - 1 < kMid.size() ? std::string(1, kMid[idx - 1]) : "m" + std::to_string(idx - 1));
  }

  // Degree labels: stretched cents, '#' / 'b' for an altered degree.
  for (size_t i = 0; i < m.pitches.size(); ++i) {
    if (i == 0) {
      m.degreeLabel.emplace_back();
      continue;
    }
    std::string t = tu::js::toFixed(tu::stretchCents(m.pitches[i], config_.octaveStretch), 0);
    if (m.alts[i] > 0.01) t += "#";
    else if (m.alts[i] < -0.01) t += "b";
    m.degreeLabel.push_back(std::move(t));
  }

  // Title (buildCircleTitleParts).
  const std::string label = tu::mosDisplayLabel(S.key);
  m.mosLabel = label;
  m.title.push_back({label + "  ", Model::Tonic, 0});
  const tu::Hardness hr = tu::computeHardness(gen, m.n, m.P);
  const std::string hardStr = std::isinf(hr.ratio) ? std::string("\xE2\x88\x9E") : tu::js::toFixed(hr.ratio, 2);
  const auto edoRatio = tu::edoStepRatio(gen, m.n, m.P, edos_);
  m.title.push_back({hardStr + (edoRatio ? " (" + *edoRatio + ")" : std::string()) + "  ", Model::Info, 0});
  if (const auto gmn = tu::computeGmn(gen, m.n, S.modeRotation, m.P, &hint)) {
    const std::string alt = m.hasModmos ? tu::formatModmosAlterations(modmos_) : std::string();
    m.title.push_back({tu::gmnLabel(*gmn) + (alt.empty() ? std::string() : " " + alt) + " \xC2\xB7 ", Model::Info, 0});
  }
  if (const auto udp = tu::computeUdp(gen, m.n, S.modeRotation, m.P, &hint)) m.title.push_back({*udp, Model::Info, 1});
  if (const auto mode = tu::modeName(S.perPeriodKey, gen, S.modeRotation, m.P, &hint)) m.title.push_back({"  " + std::string(*mode), Model::ModeName, 2});
  if (m.hasModmos) m.title.push_back({"  MODMOS", Model::Modmos, 3});

  // Step pattern ("LLsLLLs"; with MODMOS: the altered steps as L / s / cents).
  if (m.hasModmos) {
    m.modPattern = true;
    std::string p;
    for (size_t i = 0; i < static_cast<size_t>(m.n); ++i) {
      if (i) p += ' ';
      const double st = steps[i];
      if (std::fabs(st - rot.L) < 0.5) p += 'L';
      else if (std::fabs(st - rot.s) < 0.5) p += 's';
      else p += tu::js::toFixed(tu::stretchCents(st, config_.octaveStretch), 0);
    }
    m.pattern = p;
    // Centre text (the explorer compares against the altered scale's own L and s there).
    std::string c;
    for (size_t i = 0; i < m.steps.size(); ++i) {
      if (i) c += ' ';
      const double st = m.steps[i];
      if (std::fabs(st - m.L) < 0.5) c += 'L';
      else if (std::fabs(st - m.s) < 0.5) c += 's';
      else c += tu::js::toFixed(tu::stretchCents(st, config_.octaveStretch), 0);
    }
    m.modStepStr = c;
  } else {
    m.pattern = tu::computeStepPattern(gen, m.n, S.modeRotation, m.P, &hint);
  }

  // Temperament at the generator (centre).
  if (std::fabs(config_.octaveStretch) <= 5.0) {
    const auto at = tu::temperamentsAtCursor(S.perPeriodKey, gen, m.P, {S.regionLow, S.regionHigh});
    if (!at.empty()) {
      const tu::Temperament& t = tu::temperaments()[static_cast<size_t>(at.front().index)];
      m.tempName = std::string(t.name);
      const auto col = tu::temperamentColor(t.name);
      m.tempColour = col ? parseColor(*col) : rgba(0x50, 0xC8, 0x78);
    }
  }

  // Arrows.
  m.parent = parentKey();
  m.child = childKey();
  if (m.parent) m.parentLabel = tu::mosDisplayLabel(*m.parent);
  if (m.child) m.childLabel = tu::mosDisplayLabel(*m.child);

  // Hardness slider.
  m.eqLow = equalizedLow(S);
  const auto [xL, xS] = exactSteps(gen, m.n, m.P, hint);
  m.u = xL > 0.0 ? std::clamp(1.0 - xS / xL, 0.0, 1.0) : 0.0;
  m.sliderColour = lerpHex(equalized, collapsed, m.u);

  // All-intervals overlay: every interval between two notes of the scale that is not a degree of this mode.
  if (showAll_) {
    std::vector<long long> modeSet;
    for (double p : m.pitches) modeSet.push_back(std::llround(p * 10.0));
    std::vector<std::pair<long long, double>> seen;
    const size_t bn = m.basePitches.size();
    for (size_t i = 0; i < bn; ++i)
      for (size_t j = 0; j < bn; ++j) {
        if (i == j) continue;
        const double iv = std::fmod(std::fmod(m.basePitches[j] - m.basePitches[i], m.P) + m.P, m.P);
        for (int32_t k = 0; k < m.reps; ++k) {
          const double c = iv + k * m.P;
          if (c >= m.displayP) continue;
          const long long key = std::llround(c * 10.0);
          if (std::find(modeSet.begin(), modeSet.end(), key) != modeSet.end()) continue;
          bool dup = false;
          for (const auto& s : seen) dup = dup || s.first == key;
          if (!dup) seen.emplace_back(key, c);
        }
      }
    for (const auto& s : seen) m.extra.push_back(s.second);
    std::sort(m.extra.begin(), m.extra.end());
  }
}

// JI markers: every n/d with n, d < 20, reduced, n·d ≤ 162, inside the displayed period; simplest first.
void MosCircle::ensureJi() const {
  ensureModel();
  const double dp = model_ && model_->valid ? model_->displayP : config_.period * config_.periodsPerEquave;
  if (dp == jiPeriod_ && config_.octaveStretch == jiStretch_) return;
  jiPeriod_ = dp;
  jiStretch_ = config_.octaveStretch;
  ++stats_.jiBuilds;
  ji_.clear();
  for (int32_t d = 1; d < 20; ++d)
    for (int32_t n = d + 1; n < 20; ++n) {
      int32_t a = n, b = d;
      while (b) {
        const int32_t t = b;
        b = a % b;
        a = t;
      }
      const int32_t rn = n / a, rd = d / a;
      if (rn != n) continue;  // seen in its reduced form
      const double pure = 1200.0 * std::log2(static_cast<double>(rn) / rd);
      const double cents = pure * 1200.0 / tu::stretchedOctave(config_.octaveStretch);
      if (cents > 0 && cents < dp && rn * rd <= 162)
        ji_.push_back({std::to_string(rn) + "/" + std::to_string(rd), cents, rn * rd});
    }
  std::stable_sort(ji_.begin(), ji_.end(), [](const JiMarker& x, const JiMarker& y) { return x.complexity < y.complexity; });
}

// --- geometry ------------------------------------------------------------------------------------------------------

void MosCircle::ensureGeometry() const {
  const float sc = tree() ? tree()->scale() : 1.0f;
  if (!geoDirty_ && geoRect_ == rect() && geoScale_ == sc) return;
  geoDirty_ = false;
  geoRect_ = rect();
  geoScale_ = sc;
  const UiScale s(sc);
  const IRect R = rect();
  const float W = s.toLogical(static_cast<float>(R.w)), H = s.toLogical(static_cast<float>(R.h));
  float r = std::min((H - mcc::kCircleTop - mcc::kBottom) / 2.0f, (W - 2.0f * mcc::kSideMargin) / 2.0f);
  r = std::max(mcc::kMinRadius, r);
  const float extra = std::max(0.0f, H - mcc::kCircleTop - mcc::kBottom - 2.0f * r);
  const float cxL = W / 2.0f, cyL = mcc::kCircleTop + r + extra / 2.0f;
  auto X = [&](float lx) { return static_cast<float>(R.x) + s.exact(lx); };
  auto Y = [&](float ly) { return static_cast<float>(R.y) + s.exact(ly); };
  auto box = [&](float lx, float ly, float lw, float lh) {
    return IRect{R.x + s.px(lx), R.y + s.px(ly), s.px(lx + lw) - s.px(lx), s.px(ly + lh) - s.px(ly)};
  };
  Geometry& g = geo_;
  g.centre = {X(cxL), Y(cyL)};
  g.radius = s.exact(r);
  const float sx = cxL - r - 8.0f, sw = 2.0f * r + 16.0f;
  g.slider = box(sx, mcc::kSliderY, sw, mcc::kSliderH);
  const float by = mcc::kModeRowY - mcc::kModeBtnH / 2.0f;
  g.genDown = box(sx, by, mcc::kModeBtnW, mcc::kModeBtnH);
  g.genUp = box(sx + mcc::kModeBtnW + mcc::kModeLabelW, by, mcc::kModeBtnW, mcc::kModeBtnH);
  g.stepUp = box(sx + sw - mcc::kModeBtnW, by, mcc::kModeBtnW, mcc::kModeBtnH);
  g.stepDown = box(sx + sw - 2.0f * mcc::kModeBtnW - mcc::kModeLabelW, by, mcc::kModeBtnW, mcc::kModeBtnH);
  g.parentArrow = {X(8.0f + mcc::kArrowR), Y(cyL - r + 4.0f)};
  g.childArrow = {X(W - 8.0f - mcc::kArrowR), Y(cyL - r + 4.0f)};
  // The all-intervals toggle in the bottom-right corner; the MODMOS reset button under the centre.
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  const bool text = res && res->text && res->uiFonts;
  const float allW = (text ? s.toLogical(static_cast<float>(res->text->measure(res->uiFonts->small, "all intervals"))) : 70.0f) + 20.0f;
  g.allIntervals = box(W - allW - 6.0f, H - 17.0f - 5.0f, allW, 17.0f);
  const float resetW = (text ? s.toLogical(static_cast<float>(res->text->measure(res->uiFonts->small, "\xC3\x97 reset"))) : 40.0f) + 12.0f;
  g.reset = box(cxL - resetW / 2.0f, cyL + 0.25f * r, resetW, 17.0f);
}

const MosCircle::Geometry& MosCircle::geometry() const {
  ensureGeometry();
  return geo_;
}

double MosCircle::sliderU(double u) const noexcept {
  const bool eqLow = model_ && model_->valid ? model_->eqLow : true;
  return eqLow ? u : 1.0 - u;
}

float MosCircle::sliderXOfHardness(double ratio) const {
  ensureModel();
  ensureGeometry();
  const double u = std::isinf(ratio) ? 1.0 : std::clamp(1.0 - 1.0 / ratio, 0.0, 1.0);
  return static_cast<float>(geo_.slider.x) + static_cast<float>(sliderU(u)) * static_cast<float>(geo_.slider.w);
}

double MosCircle::hardnessAtSliderX(float x, bool snap) const {
  ensureModel();
  ensureGeometry();
  const UiScale s(geoScale_);
  if (snap)
    for (double t : kHardnessTicks)
      if (std::fabs(sliderXOfHardness(t) - x) <= s.exact(mcc::kHardnessSnapPx)) return t;
  const double t = std::clamp(static_cast<double>(x - static_cast<float>(geo_.slider.x)) / std::max(1, geo_.slider.w), 0.0, 1.0);
  const double u = sliderU(t);  // the mapping is its own inverse
  return u >= 1.0 ? kInf : 1.0 / (1.0 - u);
}

int32_t MosCircle::degreeCount() const {
  ensureModel();
  return model_->valid ? static_cast<int32_t>(model_->pitches.size()) : 0;
}

std::span<const double> MosCircle::displayPitches() const {
  ensureModel();
  return model_->pitches;
}

std::span<const std::string> MosCircle::stepLabels() const {
  ensureModel();
  return model_->sliceLabel;
}

std::string MosCircle::titleText() const {
  ensureModel();
  std::string t;
  for (const Model::Part& p : model_->title) t += p.text;
  return t;
}

Vec2 MosCircle::dotPos(int32_t degree) const {
  ensureModel();
  ensureGeometry();
  const Model& m = *model_;
  if (!m.valid || degree < 0 || degree >= static_cast<int32_t>(m.pitches.size())) return {-1.0f, -1.0f};
  const double a = m.pitches[static_cast<size_t>(degree)] / m.displayP * 2.0 * kPi - kPi / 2.0;
  return {geo_.centre.x + static_cast<float>(std::cos(a)) * geo_.radius, geo_.centre.y + static_cast<float>(std::sin(a)) * geo_.radius};
}

int32_t MosCircle::dotAt(Vec2 p) const {
  const int32_t n = degreeCount();
  const float hr = UiScale(geoScale_).exact(mcc::kDotHitR);
  for (int32_t i = 0; i < n; ++i) {
    const Vec2 d = dotPos(i);
    if ((p.x - d.x) * (p.x - d.x) + (p.y - d.y) * (p.y - d.y) <= hr * hr) return i;
  }
  return -1;
}

// --- input ---------------------------------------------------------------------------------------------------------

// MODMOS drag (mousemove with S._modmosDrag): the angle around the centre → an offset from the unaltered pitch,
// snapped to zero, whole chromas and (for an EDO generator) the EDO grid.
void MosCircle::dragModmos(Vec2 p) {
  ensureGeometry();
  const float dx = p.x - geo_.centre.x, dy = p.y - geo_.centre.y;
  if (std::sqrt(dx * dx + dy * dy) < UiScale(geoScale_).exact(10.0f)) return;
  const double displayP = sel_->period * std::max(1, sel_->periodsPerEquave);
  const double angle = std::atan2(static_cast<double>(dy), static_cast<double>(dx));
  const double norm = std::fmod(std::fmod(angle + kPi / 2.0, 2.0 * kPi) + 2.0 * kPi, 2.0 * kPi);
  const double target = norm / (2.0 * kPi) * displayP;
  const ModmosDrag& d = mdrag_;
  const double P = sel_->period, halfP = P / 2.0;
  double offset = std::fmod(std::fmod(target - d.baseCents + halfP, P) + P, P) - halfP;
  const double thresh = d.chroma > 0.5 ? std::min(8.0, d.chroma * 0.15) : 8.0;
  std::optional<double> best;
  double bestDist = thresh;
  if (std::fabs(offset) < bestDist) {
    bestDist = std::fabs(offset);
    best = 0.0;
  }
  if (d.chroma > 0.5) {
    const double k = std::round(offset / d.chroma);
    const double dist = std::fabs(offset - k * d.chroma);
    if (dist < bestDist) {
      bestDist = dist;
      best = k * d.chroma;
    }
  }
  if (d.edoStep > 0.5) {
    const double nearest = std::round((d.baseCents + offset) / d.edoStep) * d.edoStep;
    const double eo = nearest - d.baseCents;
    const bool isChroma = d.chroma > 0.5 && std::fabs(eo - std::round(eo / d.chroma) * d.chroma) < 0.5;
    if (!isChroma && std::fabs(offset - eo) < bestDist) best = eo;
  }
  if (best) offset = *best;
  const auto n = static_cast<size_t>(sel_->notesPerPeriod());
  if (modmos_.size() < n) modmos_.resize(n, 0.0);
  const double v = d.chroma > 0.01 ? offset / d.chroma : 0.0;
  if (modmos_[static_cast<size_t>(d.origDeg)] == v) return;
  modmos_[static_cast<size_t>(d.origDeg)] = v;
  invalidate();
  emit(MosGesture::Live);
}

void MosCircle::cancelDrag() {
  const Drag d = drag_;
  drag_ = Drag::None;
  if (d == Drag::Slider) sel_ = pressSel_;
  if (d == Drag::Modmos) modmos_ = pressModmos_;
  invalidate();
  if (d == Drag::Slider || d == Drag::Modmos) emit(MosGesture::Cancel);
  if (tree() && tree()->captured() == this) tree()->releaseCapture();
}

bool MosCircle::onPointer(const PointerEvent& e) {
  ensureModel();
  ensureGeometry();
  const UiScale s(geoScale_);
  switch (e.type) {
    case PointerType::Down: {
      if (e.button == PointerButton::Right && (drag_ == Drag::Slider || drag_ == Drag::Modmos)) {
        cancelDrag();
        return true;
      }
      if (e.button != PointerButton::Left) return false;
      if (tree() && focusable()) tree()->setFocus(this);
      pressPos_ = e.pos;
      if (!sel_) return false;
      const Geometry& g = geo_;
      if (g.genDown.contains(e.pos)) return modeAction(tu::ModeAction::GenDown), true;
      if (g.genUp.contains(e.pos)) return modeAction(tu::ModeAction::GenUp), true;
      if (g.stepDown.contains(e.pos)) return modeAction(tu::ModeAction::StepDown), true;
      if (g.stepUp.contains(e.pos)) return modeAction(tu::ModeAction::StepUp), true;
      const float pad = s.exact(mcc::kArrowR + 10.0f);
      if (model_->parent && std::fabs(e.pos.x - g.parentArrow.x) < pad && std::fabs(e.pos.y - g.parentArrow.y) < pad)
        return navigateToParent(), true;
      if (model_->child && std::fabs(e.pos.x - g.childArrow.x) < pad && std::fabs(e.pos.y - g.childArrow.y) < pad)
        return navigateToChild(), true;
      const float sp = s.exact(mcc::kSliderPad);
      if (e.pos.x >= static_cast<float>(g.slider.x) - sp && e.pos.x <= static_cast<float>(g.slider.right()) + sp &&
          e.pos.y >= static_cast<float>(g.slider.y) - sp && e.pos.y <= static_cast<float>(g.slider.bottom()) + sp) {
        drag_ = Drag::Slider;
        pressSel_ = sel_;
        setHardness(hardnessAtSliderX(e.pos.x, (e.mods & mod::kAlt) == 0), MosGesture::Live);
        return true;
      }
      if (g.allIntervals.contains(e.pos)) {
        setShowAllIntervals(!showAll_);
        return true;
      }
      if (model_->hasModmos && g.reset.contains(e.pos)) {
        resetModmos();
        return true;
      }
      const int32_t dot = dotAt(e.pos);
      if (dot >= 0) {
        const Model& m = *model_;
        const int32_t n = m.n;
        const int32_t origDeg = m.origDegs[static_cast<size_t>(dot)];
        if (origDeg == 0) {
          if (onAudition) onAudition(m.pitches[static_cast<size_t>(dot)]);
          return true;
        }
        // Start a MODMOS drag of the unaltered degree origDeg (mousedown on a circle dot).
        const tu::RegionHint hint = sel_->hint();
        const tu::MosScale rot = tu::rotateMosScale(tu::buildMosScale(sel_->generatorCents, n, sel_->period, &hint), sel_->modeRotation, sel_->period);
        ModmosDrag d;
        d.origDeg = origDeg;
        d.baseCents = rot.pitches[static_cast<size_t>(origDeg)];
        d.chroma = rot.L - rot.s;
        d.startOffset = static_cast<size_t>(origDeg) < modmos_.size() ? modmos_[static_cast<size_t>(origDeg)] : 0.0;
        for (const tu::EdoGenerator& eg : edos_)
          if (std::fabs(eg.cents - sel_->generatorCents) < 0.5) {
            d.edoStep = sel_->period / eg.edo;
            break;
          }
        const int32_t prev = origDeg == 1 ? 0 : origDeg - 1;
        const int32_t next = origDeg == n - 1 ? n : origDeg + 1;
        d.prevCents = prev == 0 ? 0.0 : rot.pitches[static_cast<size_t>(prev)];
        d.nextCents = next == n ? sel_->period : rot.pitches[static_cast<size_t>(next)];
        mdrag_ = d;
        pressModmos_ = modmos_;
        drag_ = Drag::Modmos;
        invalidate();
        return true;
      }
      return false;
    }
    case PointerType::Move: {
      if (drag_ == Drag::Slider) {
        applyHardness(hardnessAtSliderX(e.pos.x, (e.mods & mod::kAlt) == 0), MosGesture::Live, false);
        return true;
      }
      if (drag_ == Drag::Modmos) {
        dragModmos(e.pos);
        return true;
      }
      const int32_t hov = sel_ ? dotAt(e.pos) : -1;
      if (hov != hoverDot_) {
        hoverDot_ = hov;
        markDirty();
      }
      return false;
    }
    case PointerType::Up: {
      if (e.button != PointerButton::Left) return drag_ != Drag::None;
      const Drag d = drag_;
      drag_ = Drag::None;
      if (d == Drag::Slider && sel_ != pressSel_) emit(MosGesture::Commit);
      if (d == Drag::Modmos) {
        invalidate();
        if (modmos_ != pressModmos_) emit(MosGesture::Commit);
      }
      return d != Drag::None;
    }
    case PointerType::Leave:
      if (hoverDot_ >= 0 && drag_ == Drag::None) {
        hoverDot_ = -1;
        markDirty();
      }
      return false;
    case PointerType::Wheel:
      return false;
  }
  return false;
}

bool MosCircle::onKey(const KeyEvent& e) {
  if (!e.down || e.key != key::kEscape || (drag_ != Drag::Slider && drag_ != Drag::Modmos)) return false;
  cancelDrag();
  return true;
}

void MosCircle::onCaptureLost() {
  // As a release: the dragged value stays.
  const Drag d = drag_;
  drag_ = Drag::None;
  if (d == Drag::Slider && sel_ != pressSel_) emit(MosGesture::Commit);
  if (d == Drag::Modmos) {
    invalidate();
    if (modmos_ != pressModmos_) emit(MosGesture::Commit);
  }
}

void MosCircle::onHoverChanged(bool hovered) {
  if (!hovered && hoverDot_ >= 0 && drag_ == Drag::None) {
    hoverDot_ = -1;
    markDirty();
  }
}

InfoText MosCircle::info() const {
  if (sel_ && hoverDot_ >= 0) {
    ensureModel();
    const Model& m = *model_;
    if (hoverDot_ < static_cast<int32_t>(m.pitches.size())) {
      const double c = tu::stretchCents(m.pitches[static_cast<size_t>(hoverDot_)], config_.octaveStretch);
      const std::string t = "Degree " + std::to_string(hoverDot_) + ": " + fmt("%.2f\xC2\xA2", c);
      return {t, m.origDegs[static_cast<size_t>(hoverDot_)] == 0 ? "Click to audition the tonic."
                                                                   : "Drag around the circle to alter this degree (MODMOS): snaps to whole chromas, the EDO grid and zero. Esc cancels."};
    }
  }
  return {"MOS Circle",
          "The selected MOS around one equave: step slices, degree spokes with cents, JI ratios inside the rim. The slider "
          "sets the hardness L/s (snaps to 4:3 … 4:1, Alt bypasses); gen / step rotate the mode; the arrows go to the "
          "parent or child MOS; drag a dot to alter it (MODMOS)."};
}

// --- paint ---------------------------------------------------------------------------------------------------------

void MosCircle::paint(PaintContext& ctx) {
  const Theme* thp = ctx.res.theme;
  if (!thp) return;
  const Theme::MosCircle& T = thp->mosCircle;
  ensureModel();
  ensureGeometry();
  ensureJi();
  const IRect R = rect();
  const IRect clip = intersect(R, ctx.clip);
  if (clip.empty()) return;
  ++stats_.paints;
  stats_.jiLabels = 0;
  DrawList& dl = ctx.dl;
  const UiScale s = ctx.scale;
  const Model& m = *model_;
  const Geometry& g = geo_;
  TextSystem* ts = ctx.res.text;
  const ThemeFonts* fonts = ctx.res.uiFonts;
  const bool text = ts && fonts;
  const FontId fSmall = fonts ? fonts->small : kNoFont, fStrong = fonts ? fonts->bodyStrong : kNoFont;
  const float rS = g.radius / s.exact(mcc::kRefRadius);  // the explorer's rScale
  const Vec2 C = g.centre;
  const float rad = g.radius;
  // Text centred on (x, y) (capitals centred vertically); returns its rect.
  auto centred = [&](FontId f, std::string_view str, float x, float y, Rgba col) -> Rect {
    if (!text || f == kNoFont || str.empty()) return {};
    const TextRun run = ts->shape(f, str);
    const float cap = 0.727f * static_cast<float>(ts->metrics(f).pixelSize);
    const float x0 = std::round(x - static_cast<float>(run.advance) / 2.0f), base = std::round(y + cap / 2.0f);
    TextSystem::drawRun(dl, run, {x0, base}, col);
    return {x0, base - cap, x0 + static_cast<float>(run.advance), base};
  };
  auto leftText = [&](FontId f, std::string_view str, float x, float y, Rgba col) -> float {
    if (!text || f == kNoFont || str.empty()) return 0.0f;
    const TextRun run = ts->shape(f, str);
    const float cap = 0.727f * static_cast<float>(ts->metrics(f).pixelSize);
    TextSystem::drawRun(dl, run, {std::round(x), std::round(y + cap / 2.0f)}, col);
    return static_cast<float>(run.advance);
  };
  auto ang = [&](double cents) { return cents / m.displayP * 2.0 * kPi - kPi / 2.0; };
  auto at = [&](double a, float r) {
    return Vec2{C.x + static_cast<float>(std::cos(a)) * r, C.y + static_cast<float>(std::sin(a)) * r};
  };
  auto Y = [&](float ly) { return static_cast<float>(R.y) + s.exact(ly); };
  const float px1 = s.exact(1.0f);

  dl.pushClip(clip.toRect());
  dl.addRectFilled(clip.toRect(), T.background);

  // --- title + mode buttons ---
  if (m.valid && text) {
    std::vector<const Model::Part*> parts;
    for (const Model::Part& p : m.title) parts.push_back(&p);
    auto width = [&] {
      int32_t w = 0;
      for (const Model::Part* p : parts) w += ts->measure(fStrong, p->text);
      return w;
    };
    const int32_t budget = std::max(s.px(120.0f), R.w - s.px(16.0f));
    for (int tier : {3, 2, 1}) {
      if (width() <= budget) break;
      std::erase_if(parts, [tier](const Model::Part* p) { return p->drop == tier; });
    }
    const int32_t tw = width();
    float x = std::max(static_cast<float>(R.x) + s.exact(8.0f), C.x - static_cast<float>(tw) / 2.0f);
    for (const Model::Part* p : parts) {
      const Rgba col = p->tone == Model::Tonic ? T.tonic : p->tone == Model::Info ? T.info : p->tone == Model::ModeName ? T.modeName : T.modmos;
      x += leftText(fStrong, p->text, x, Y(mcc::kTitleY), col);
    }
  } else if (text) {
    centred(fStrong, "MOS", C.x, Y(mcc::kTitleY), T.tonic);
  }
  if (m.valid) {
    auto button = [&](const IRect& b, bool right) {
      paint::box(dl, b, s.exact(3.0f), T.button, T.buttonBorder);
      const float cx = static_cast<float>(b.x) + static_cast<float>(b.w) / 2.0f, cy = static_cast<float>(b.y) + static_cast<float>(b.h) / 2.0f;
      const float h = s.exact(3.5f);
      if (right) dl.addTriangleFilled({cx - h * 0.8f, cy - h}, {cx - h * 0.8f, cy + h}, {cx + h, cy}, T.buttonText);
      else dl.addTriangleFilled({cx + h * 0.8f, cy - h}, {cx + h * 0.8f, cy + h}, {cx - h, cy}, T.buttonText);
    };
    button(g.genDown, false);
    button(g.genUp, true);
    button(g.stepDown, false);
    button(g.stepUp, true);
    const float my = Y(mcc::kModeRowY);
    centred(fSmall, "gen", (static_cast<float>(g.genDown.right()) + static_cast<float>(g.genUp.x)) / 2.0f, my, T.dimText);
    centred(fSmall, "step", (static_cast<float>(g.stepDown.right()) + static_cast<float>(g.stepUp.x)) / 2.0f, my, T.dimText);
    // Step pattern between the button pairs ("LLsLLLs", or the MODMOS steps in green).
    if (text && !m.pattern.empty()) {
      const int32_t avail = g.stepDown.x - g.genUp.right() - s.px(8.0f);
      const TextRun run = ts->shapeTruncated(fSmall, m.pattern, std::max(0, avail));
      const float cap = 0.727f * static_cast<float>(ts->metrics(fSmall).pixelSize);
      TextSystem::drawRun(dl, run, {std::round(C.x - static_cast<float>(run.advance) / 2.0f), std::round(my + cap / 2.0f)},
                          m.modPattern ? T.modmos : T.degreeText);
    }
  }

  // --- hardness slider ---
  {
    const Rect tr = g.slider.toRect();
    const float rr = tr.height() / 2.0f;
    dl.addRoundedRectFilled(tr, rr, T.sliderTrack);
    const bool eqLow = m.valid ? m.eqLow : true;
    const float ly = Y(mcc::kSliderLabelY);
    centred(fSmall, "equalized", tr.x0 + tr.width() * (eqLow ? 0.15f : 0.85f), ly, T.dimText);
    centred(fSmall, "collapsed", tr.x0 + tr.width() * (eqLow ? 0.85f : 0.15f), ly, T.dimText);
    if (m.valid) {
      const float thumbX = tr.x0 + static_cast<float>(sliderU(m.u)) * tr.width();
      const float fillW = eqLow ? thumbX - tr.x0 : tr.x1 - thumbX;
      if (fillW > px1) {
        const Rect f = eqLow ? Rect{tr.x0, tr.y0, thumbX, tr.y1} : Rect{thumbX, tr.y0, tr.x1, tr.y1};
        dl.addRoundedRectFilled(f, rr, setAlpha(m.sliderColour, 0.4f));
      }
      // Notches at the named hardnesses (4:3 … 4:1), the slider's snap targets.
      for (double t : kHardnessTicks) dl.addVLine(sliderXOfHardness(t), tr.y0 + s.pxf(4.0f), tr.y1 - s.pxf(4.0f), setAlpha(T.dimText, 0.7f), 1.0f);
      const Vec2 tc{thumbX, (tr.y0 + tr.y1) / 2.0f};
      dl.addCircleFilled(tc, rr + s.exact(2.0f), m.sliderColour);
      dl.addCircleStroke(tc, rr + s.exact(2.0f), T.sliderThumbEdge, s.exact(1.5f));
    }
  }

  // --- parent / child arrows ---
  if (m.valid) {
    const float ar = s.exact(mcc::kArrowR);
    if (m.parent) {
      const Vec2 a = g.parentArrow;
      dl.addTriangleFilled({a.x, a.y - ar}, {a.x - ar, a.y + ar * 0.4f}, {a.x + ar, a.y + ar * 0.4f}, setAlpha(T.parent, 0.7f));
      leftText(fSmall, m.parentLabel, a.x + ar + s.exact(5.0f), a.y, T.parent);
    }
    if (m.child) {
      const Vec2 a = g.childArrow;
      dl.addTriangleFilled({a.x, a.y + ar}, {a.x - ar, a.y - ar * 0.4f}, {a.x + ar, a.y - ar * 0.4f}, setAlpha(T.child, 0.7f));
      if (text) {
        const float w = static_cast<float>(ts->measure(fSmall, m.childLabel));
        leftText(fSmall, m.childLabel, a.x - ar - s.exact(5.0f) - w, a.y, T.child);
      }
    }
  }

  // --- outer ring ---
  dl.addCircleStroke(C, rad, T.ring, s.exact(2.5f));
  if (!m.valid) {
    centred(fSmall, "Select a MOS in the chart", C.x, C.y - s.exact(8.0f), T.dimText);
    centred(fSmall, "or drag the slider above", C.x, C.y + s.exact(10.0f), T.dimText);
    dl.popClip();
    return;
  }
  const size_t N = m.pitches.size();
  const bool mdrag = drag_ == Drag::Modmos;

  // --- step slices ---
  {
    const uint32_t segsFull = DrawList::circleSegments(rad);
    std::vector<Vec2>& pts = scratchPts_;
    for (size_t i = 0; i < N; ++i) {
      const double a1 = ang(m.pitches[i]), a2 = ang(m.pitches[i] + m.steps[i]);
      const Rgba col = m.sliceColour[i];
      const double sweep = a2 - a1;
      if (sweep > 1e-6) {
        // Convex pieces of ≤ 90°: the centre plus the arc points.
        const int pieces = static_cast<int>(std::ceil(sweep / (kPi / 2.0)));
        for (int p = 0; p < pieces; ++p) {
          const double b1 = a1 + sweep * p / pieces, b2 = a1 + sweep * (p + 1) / pieces;
          const auto segs = std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil((b2 - b1) / (2.0 * kPi) * segsFull)));
          pts.clear();
          pts.push_back(C);
          for (uint32_t k = 0; k <= segs; ++k) pts.push_back(at(b1 + (b2 - b1) * k / segs, rad));
          dl.addConvexPolyFilled(pts, setAlpha(col, 0.25f));
        }
        segment(dl, C, at(a1, rad), setAlpha(col, 0.6f), px1);
        segment(dl, C, at(a2, rad), setAlpha(col, 0.6f), px1);
        dl.addArcStroke(C, rad, static_cast<float>(a1), static_cast<float>(a2), setAlpha(col, 0.9f), s.exact(5.0f));
      }
      if (!mdrag) centred(fStrong, m.sliceLabel[i], at(ang(m.pitches[i] + m.steps[i] / 2.0), rad * 0.6f).x,
                          at(ang(m.pitches[i] + m.steps[i] / 2.0), rad * 0.6f).y, col);
    }
  }

  // --- period / octave boundary markers ---
  auto boundary = [&](double cents, Rgba col) {
    const double a = ang(cents);
    dashedLine(dl, at(a, rad - s.exact(8.0f)), at(a, rad + s.exact(8.0f)), s.exact(3.0f), s.exact(3.0f), col, s.exact(2.0f));
  };
  if (m.reps > 1)
    for (int32_t k = 1; k < m.reps; ++k) boundary(k * m.P, T.periodMark);
  {
    const double stretched = config_.octaveStretch != 0.0 ? tu::stretchedOctave(config_.octaveStretch) : 1200.0;
    if (m.displayP > stretched || config_.octaveStretch != 0.0) {
      const int octaves = config_.octaveStretch != 0.0 ? std::max(1, static_cast<int>(std::floor(m.displayP / stretched)))
                                                        : static_cast<int>(std::floor(m.displayP / 1200.0));
      for (int k = 1; k <= octaves; ++k) {
        const double oc = k * stretched;
        if (oc >= m.displayP) break;
        boundary(oc, T.octaveMark);
      }
    }
  }

  // --- MODMOS drag guides ---
  if (mdrag) {
    const ModmosDrag& d = mdrag_;
    const double P = m.P;
    const double curOff = (static_cast<size_t>(d.origDeg) < modmos_.size() ? modmos_[static_cast<size_t>(d.origDeg)] : 0.0) * d.chroma;
    const float outerR = rad + s.exact(std::round(14.0f * rS)), innerR = rad - s.exact(std::round(14.0f * rS));
    const double visLo = d.prevCents - 2.0, visHi = d.nextCents + 2.0;
    auto inRange = [&](double c) {
      const double w = std::fmod(std::fmod(c, P) + P, P);
      if (visLo >= 0 && visHi <= P) return w >= visLo && w <= visHi;
      const double lo = std::fmod(std::fmod(visLo, P) + P, P), hi = std::fmod(std::fmod(visHi, P) + P, P);
      return lo <= hi ? (w >= lo && w <= hi) : (w >= lo || w <= hi);
    };
    const float band = s.exact(std::round(18.0f * rS));
    if (d.chroma > 0.5)
      dl.addArcStroke(C, outerR + s.exact(std::round(10.0f * rS)), static_cast<float>(ang(visLo)), static_cast<float>(ang(visHi)),
                      setAlpha(T.modmos, 0.16f), band);
    if (d.edoStep > 0.5)
      dl.addArcStroke(C, innerR - s.exact(std::round(10.0f * rS)), static_cast<float>(ang(visLo)), static_cast<float>(ang(visHi)),
                      setAlpha(T.altUp, 0.13f), band);
    for (double b : {d.prevCents, d.nextCents}) {
      const double a = ang(std::fmod(std::fmod(b, m.displayP) + m.displayP, m.displayP));
      dashedLine(dl, at(a, innerR - s.exact(std::round(20.0f * rS))), at(a, outerR + s.exact(std::round(20.0f * rS))), s.exact(3.0f),
                 s.exact(4.0f), setAlpha(T.guide, 0.13f), px1);
    }
    if (d.chroma > 0.5) {
      const int snaps = std::min(static_cast<int>(std::floor(P / d.chroma)), 6);
      for (int k = -snaps; k <= snaps; ++k) {
        if (k == 0) continue;
        const double c = std::fmod(std::fmod(d.baseCents + k * d.chroma, m.displayP) + m.displayP, m.displayP);
        if (!inRange(c)) continue;
        const double a = ang(c);
        const bool hit = std::fabs(curOff - k * d.chroma) < 0.5;
        const float tin = rad - s.exact(std::round(6.0f * rS)), tout = outerR + s.exact(std::round(18.0f * rS));
        segment(dl, at(a, tin), at(a, tout), hit ? T.modmos : setAlpha(T.modmos, 0.33f), s.exact(hit ? 3.0f : 1.5f));
        char buf[16];
        const int len = std::snprintf(buf, sizeof buf, "%s%dc", k > 0 ? "+" : "", k);
        const Vec2 lp = at(a, tout + s.exact(std::round(8.0f * rS)) + s.exact(6.0f));
        centred(hit ? fStrong : fSmall, std::string_view(buf, static_cast<size_t>(std::max(0, len))), lp.x, lp.y,
                hit ? T.modmos : setAlpha(T.modmos, 0.6f));
      }
    }
    if (d.edoStep > 0.5) {
      const int grid = static_cast<int>(std::lround(m.displayP / d.edoStep));
      const int edoN = static_cast<int>(std::lround(P / d.edoStep));
      centred(fStrong, std::to_string(edoN) + "-EDO grid", C.x, C.y - rad + s.exact(std::round(20.0f * rS)), setAlpha(T.altUp, 0.53f));
      const double minSep = 12.0 * m.displayP / (2.0 * kPi * static_cast<double>(s.toLogical(rad)));
      for (int k = 0; k < grid; ++k) {
        const double gc = k * d.edoStep;
        const double off = std::fmod(std::fmod(gc - d.baseCents + P / 2.0, P) + P, P) - P / 2.0;
        if (d.chroma > 0.5 && std::fabs(off - std::round(off / d.chroma) * d.chroma) < minSep) continue;
        const double a = ang(gc);
        const bool hit = std::fabs(curOff - off) < 0.5;
        const float tout = rad + s.exact(std::round(6.0f * rS)), tin = innerR - s.exact(std::round(18.0f * rS));
        segment(dl, at(a, tout), at(a, tin), hit ? T.altUp : setAlpha(T.altUp, 0.25f), s.exact(hit ? 2.5f : 0.8f));
        const Vec2 lp = at(a, tin - s.exact(std::round(8.0f * rS)) - s.exact(4.0f));
        if (hit || edoN <= 31) centred(fSmall, std::to_string(k % std::max(1, edoN)), lp.x, lp.y, hit ? T.altUp : setAlpha(T.altUp, 0.33f));
      }
    }
    {
      const double a = ang(std::fmod(std::fmod(d.baseCents, m.displayP) + m.displayP, m.displayP));
      const bool atZero = std::fabs(curOff) < 0.5;
      const float zin = innerR - s.exact(std::round(18.0f * rS)), zout = outerR + s.exact(std::round(18.0f * rS));
      segment(dl, at(a, zin), at(a, zout), atZero ? T.guide : setAlpha(T.guide, 0.33f), s.exact(atZero ? 3.0f : 2.0f));
      const Vec2 lp = at(a, zout + s.exact(std::round(8.0f * rS)) + s.exact(4.0f));
      centred(atZero ? fStrong : fSmall, "0", lp.x, lp.y, atZero ? T.guide : setAlpha(T.guide, 0.47f));
      if (std::fabs(curOff) > 0.01)
        centred(fStrong, (curOff > 0 ? "+" : "") + fmt("%.1f\xC2\xA2", curOff), C.x, C.y + rad - s.exact(std::round(20.0f * rS)), T.guide);
    }
  }

  // --- all-intervals overlay (dashed spokes to every interval the scale reaches that is not a degree) ---
  if (showAll_ && !mdrag && !m.extra.empty()) {
    const float lr = rad + s.exact(std::round(18.0f * rS));
    const double minSep = 1.6 * 12.0 * m.displayP / (2.0 * kPi * static_cast<double>(lr / s.factor));
    double last = -kInf;
    for (double c : m.extra) {
      const double a = ang(c);
      dashedLine(dl, C, at(a, rad), s.exact(std::max(7.0f, 9.0f * rS)), s.exact(std::max(3.5f, 4.5f * rS)), setAlpha(T.allIntervals, 0.4f),
                 s.exact(std::max(1.75f, 2.0f * rS)));
      const float dr = s.exact(std::max(2.0f, 3.0f * rS));
      dl.addCircleFilled(at(a, rad), dr, setAlpha(T.allIntervals, 0.8f));
      dl.addCircleStroke(at(a, rad), dr, T.dotEdge, px1);
      if (c - last >= minSep) {
        last = c;
        const Vec2 lp = at(a, lr);
        centred(fSmall, tu::js::toFixed(tu::stretchCents(c, config_.octaveStretch), 0), lp.x, lp.y, setAlpha(T.allIntervals, 0.73f));
      }
    }
  }

  // --- spokes, dots, degree labels ---
  for (size_t i = 0; i < N; ++i) {
    const double a = ang(m.pitches[i]);
    const Vec2 p = at(a, rad);
    const bool boundaryDeg = m.reps > 1 && (i % static_cast<size_t>(m.n) == 0);
    const double alt = m.alts[i];
    const bool altered = std::fabs(alt) > 0.01;
    const Rgba altCol = alt > 0 ? T.altUp : T.altDown;
    const bool tonic = i == 0 || boundaryDeg;
    const Rgba lineCol = tonic ? T.tonic : altered ? altCol : T.spoke;
    const Rgba dotCol = tonic ? T.tonic : altered ? altCol : T.dot;
    const Rgba labelCol = tonic ? T.tonic : altered ? altCol : T.degreeText;
    segment(dl, C, p, setAlpha(lineCol, 0.53f), px1);
    const float dr = s.exact(hoverDot_ == static_cast<int32_t>(i) || (mdrag && m.origDegs[i] == mdrag_.origDeg) ? 6.5f : mcc::kDotR);
    dl.addCircleFilled(p, dr, dotCol);
    dl.addCircleStroke(p, dr, T.dotEdge, s.exact(1.5f));
    if (i > 0) {
      const Vec2 lp = at(a, rad + s.exact(std::round(18.0f * rS)) + s.exact(4.0f));
      centred(fSmall, m.degreeLabel[i], lp.x, lp.y, mdrag ? setAlpha(labelCol, 0.3f) : labelCol);
    }
  }

  // --- centre: temperament name, MODMOS steps, reset ---
  if (!m.tempName.empty()) centred(fStrong, m.tempName, C.x, C.y - rad * 0.15f, m.tempColour);
  if (m.hasModmos) {
    if (text) {
      const TextRun run = ts->shapeTruncated(fSmall, m.modStepStr, static_cast<int32_t>(rad * 1.5f));
      const float cap = 0.727f * static_cast<float>(ts->metrics(fSmall).pixelSize);
      TextSystem::drawRun(dl, run, {std::round(C.x - static_cast<float>(run.advance) / 2.0f), std::round(C.y + rad * 0.12f + cap / 2.0f)}, T.modmos);
    }
    paint::box(dl, g.reset, s.exact(3.0f), T.button, T.modmos);
    centred(fSmall, "\xC3\x97 reset", static_cast<float>(g.reset.x) + static_cast<float>(g.reset.w) / 2.0f,
            static_cast<float>(g.reset.y) + static_cast<float>(g.reset.h) / 2.0f, T.modmos);
  }

  // --- JI markers: ticks for all, labels for the simple ones that do not collide ---
  {
    const float maxFont = std::round(14.0f * rS), minFont = std::max(4.0f, std::round(6.0f * rS));
    std::vector<Rect>& placed = scratchRects_;
    placed.clear();
    const float capS = text ? 0.727f * static_cast<float>(ts->metrics(fSmall).pixelSize) : 0.0f;
    for (const JiMarker& j : ji_) {
      const float fsz = std::round(maxFont - (maxFont - minFont) * std::min(1.0f, static_cast<float>(j.complexity - 6) / 119.0f));
      const bool major = fsz > 8.0f * rS;
      const float tick = s.exact(std::round((major ? 13.0f : 8.0f) * rS));
      const double a = ang(j.cents);
      const float dim = mdrag ? 0.4f : 1.0f;
      segment(dl, at(a, rad - tick), at(a, rad + s.exact(3.0f * rS)), setAlpha(T.ji, (major ? 0.6f : 0.33f) * dim),
              s.exact(fsz > 9.0f * rS ? 1.5f : 1.0f));
      if (!major || !text) continue;
      const FontId f = fsz > 9.0f * rS ? fStrong : fSmall;
      const Vec2 lp = at(a, rad - tick - s.exact(std::round(8.0f * rS)) - s.exact(5.0f));
      const float w = static_cast<float>(ts->measure(f, j.ratio));
      const Rect box{lp.x - w / 2.0f - px1, lp.y - capS / 2.0f - 2.0f * px1, lp.x + w / 2.0f + px1, lp.y + capS / 2.0f + 2.0f * px1};
      bool clash = false;
      for (const Rect& q : placed) clash = clash || (box.x0 < q.x1 && q.x0 < box.x1 && box.y0 < q.y1 && q.y0 < box.y1);
      if (clash) continue;
      placed.push_back(box);
      const float alpha = (0.4f + 0.6f * (fsz - minFont) / std::max(1.0f, maxFont - minFont)) * dim;
      centred(f, j.ratio, lp.x, lp.y, setAlpha(T.ji, std::clamp(alpha, 0.0f, 1.0f)));
      ++stats_.jiLabels;
    }
  }

  // --- "all intervals" toggle ---
  {
    const IRect b = g.allIntervals;
    paint::box(dl, b, s.exact(3.0f), showAll_ ? setAlpha(T.allIntervals, 0.22f) : T.button, showAll_ ? T.allIntervals : T.buttonBorder);
    const Vec2 sw{static_cast<float>(b.x) + s.exact(8.0f), static_cast<float>(b.y) + static_cast<float>(b.h) / 2.0f};
    dl.addCircleFilled(sw, s.exact(3.0f), showAll_ ? T.allIntervals : setAlpha(T.buttonText, 0.33f));
    leftText(fSmall, "all intervals", static_cast<float>(b.x) + s.exact(14.0f), sw.y, showAll_ ? T.allIntervals : T.buttonText);
  }
  dl.popClip();
}

}  // namespace pq::views
