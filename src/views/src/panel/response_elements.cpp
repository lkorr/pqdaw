// EQ.4 (docs/plans/EQ.md "Displays (EQ.4)"): the RESPONSE DISPLAYS — element kinds any group's panel can place:
//
//   response   the analytic magnitude response of a group (slot 0: a Signal binding on the group's output `y`, which
//              names the group and is the analyser's tap; slot 1: optionally its input `input`, the dimmed input
//              spectrum). x = log(1 + f / 10 Hz) over 0 … 20 kHz (0 Hz at the left edge: the band Frequency knob's own
//              travel), y = dB (±6 / 12 / 24 / 48: the zoom chip). Layers: the analyser behind (devices::SpectrumPlot,
//              on the same axis), the grid, each band's own curve (faint, filled toward 0 dB in its colour), the total
//              (bright, glowing; dashed when Approximate, none and "no curve" when Unavailable), the nodes. The bound
//              group decides what is drawn:
//                an EQ (doc::eqInfo)   the ACTIVE CHAIN's total (the chain group's response: Stereo / L/R / M/S alike;
//                                      the Edit Chain block picks it) and its bands as nodes; in L/R and M/S the other
//                                      chain's total and nodes at 35 % alpha behind
//                a band (doc::bandInfo) its own curve and its one node
//                any other group       its In → Out curve, no nodes
//              Each band's own curve is the band group's own GroupResponse (exact for any band circuit, a user's too).
//              EQ.4d: the element is a stack of three AREAS, each opened / closed by its title chip (MAG, PHASE,
//              DELAY; view state), all three open at once if wanted; a closed area keeps a thin bar with its chip, the
//              open ones share the rest (3 : 1 : 1), so the element keeps its size:
//                magnitude   the above
//                phase       the active chain's total as phase in degrees — WRAPPED to ±180° (the default: a fixed
//                            scale, so a 16-stage cut's thousands of degrees never flatten the strip), or UNWRAPPED
//                            (auto-scaled; the WRAP / UNWRAP chip); the selected band's faint behind. Wrapped, a thin
//                            dotted line in the curve's colour joins the two ends of each wrap (at the crossing).
//                delay       group delay τ = −dφ/dω in ms, 0 at the floor, auto-scaled upward; true values (a cut's
//                            negative delay goes below the floor, in a shaded band of at most a quarter of the strip).
//              Group delay is a forward difference over ε_k = 10⁻³ of the grid step at each point, from a second
//              evaluation on the grid shifted by ε (its own GroupResponses key): the phase step 2π·ε·τ stays below π
//              for τ < 1 / (2ε) (≥ 2.5 s at 20 kHz), so it never aliases; the value is τ(f + ε/2) to second order. (The
//              neighbouring-point difference it replaces folded whenever 2π·Δf·τ > π: a 23-stage Q 5 allpass.) The
//              unwrapped phase uses the same τ to pick each step's turn count.
//   bandtype   a band's Type as its glyph and short code (LP, HP, BP, NO, PK, LS, HS, AP) in the band's colour, a
//              menu on click (the EQ's strip columns).
//
// Gestures on `response` (edits through the PanelTarget; one undo step each): double-click empty space adds a Bell
// there (addBand; an EQ only), a node drags frequency (x) and gain (y; Q for the gainless types), the wheel on a node
// turns Q (a run of notches: one step), Alt + wheel steps Stages, Shift: fine, double-click a node resets its gain to
// 0 dB, right-click a node: Type, Stages, Bypass, Solo, Delete; a click selects (PanelTarget::selectGroup: the strip
// column lights up), Delete removes the selected band.
//
// Cost: the curves come from views::GroupResponses (the app's compile's lowering, prepared once per structural
// revision; one evaluation per changed response, shared by the panel's displays); a display re-derives a curve's dB / phase only when that evaluation is
// new, so a drag on one band of 32 re-evaluates that band and its chain, nothing else. The geometry is drawn every
// paint (the curves are live layers); a band's fill and line cover only the span where it is off 0 dB.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "elements_internal.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/ui/lines.h"
#include "pq/ui/overlay.h"
#include "pq/views/devices/spectrum_plot.h"
#include "pq/views/panel/group_responses.h"
#include "pq/views/panel/response_display.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

// ---- the axis ------------------------------------------------------------------------------------------------------
constexpr double kAxisHz = doc::kBandAxisHz, kTopHz = doc::kBandMaxHz;
const double kAxisSpan = std::log1p(kTopHz / kAxisHz);
double tOfHz(double hz) noexcept { return std::log1p(std::max(hz, 0.0) / kAxisHz) / kAxisSpan; }
double hzOfT(double t) noexcept { return kAxisHz * std::expm1(std::clamp(t, 0.0, 1.0) * kAxisSpan); }

// The frequencies every display evaluates at: kPoints uniform in x (one shared evaluation per group: GroupResponses).
constexpr uint32_t kPoints = 768;
constexpr uint64_t kGridKey = kPoints;
const std::vector<double>& grid() {
  static const std::vector<double> g = [] {
    std::vector<double> v(kPoints);
    for (uint32_t k = 0; k < kPoints; ++k) v[k] = hzOfT(static_cast<double>(k) / (kPoints - 1));
    v[0] = hzOfT(0.25 / (kPoints - 1));  // (not DC itself: a cut's response there is 0, a loop's may be singular)
    return v;
  }();
  return g;
}

constexpr std::array<std::string_view, 8> kCodes = {"LP", "HP", "BP", "NO", "PK", "LS", "HS", "AP"};
Rgba bandColour(uint32_t i) noexcept { return ui::signal_theme::kBands[i % std::size(ui::signal_theme::kBands)]; }

// A filter type's glyph in `r` (a 14 × 9 design box).
void typeGlyph(ui::FacePainter& p, const Rect& r, uint32_t type, float thick, Rgba col) {
  struct G {
    uint32_t n;
    Vec2 pt[6];
  };
  static constexpr G kG[8] = {
      {5, {{0, 3}, {7, 3}, {10, 3.6f}, {12, 6}, {14, 9}}},            // LP (High Cut)
      {5, {{0, 9}, {2, 6}, {4, 3.6f}, {7, 3}, {14, 3}}},              // HP (Low Cut)
      {6, {{0, 9}, {3, 7}, {6, 2.5f}, {8, 2.5f}, {11, 7}, {14, 9}}},  // BP
      {6, {{0, 3}, {5, 3}, {6.5f, 8.5f}, {7.5f, 8.5f}, {9, 3}, {14, 3}}},  // NO
      {6, {{0, 6}, {4, 6}, {6, 2}, {8, 2}, {10, 6}, {14, 6}}},        // PK (Bell)
      {4, {{0, 2}, {4, 2}, {10, 6}, {14, 6}}},                        // LS
      {4, {{0, 6}, {4, 6}, {10, 2}, {14, 2}}},                        // HS
      {6, {{0, 4.5f}, {5, 4.5f}, {7, 2}, {9, 7}, {11, 4.5f}, {14, 4.5f}}},  // AP
  };
  const G& g = kG[std::min<uint32_t>(type, 7)];
  Vec2 pts[6];
  for (uint32_t i = 0; i < g.n; ++i) pts[i] = {r.x0 + g.pt[i].x / 14.0f * r.width(), r.y0 + g.pt[i].y / 9.0f * r.height()};
  p.polyline({pts, g.n}, thick, col);
}

// ---- what a display shows, from the document ------------------------------------------------------------------------

struct BandRef {
  doc::DeviceId group;
  doc::Binding freq, q, gain, stages, type;  // the role blocks' values (unbound: the band has none)
  uint32_t index = 0;                        // its place in its chain: its colour and number
  bool off = false, solo = false;
  double hz = 1000.0, db = 0.0;
  uint32_t typeV = 4, stagesV = 1;
  bool hasGain() const noexcept { return gain.bound() && (!type.bound() || doc::bandTypeHasGain(typeV)); }
  bool hasStages() const noexcept { return stages.bound() && (!type.bound() || doc::bandTypeHasStages(typeV)); }
};

doc::Binding valueOf(doc::DeviceId block) {
  return block.valid() ? doc::controlBinding(block, std::string(doc::kValueParam)) : doc::Binding{};
}

// Whether chain group `chain` holds exactly `bands` wired In → b1 → … → bn → Out by main links and nothing else (no
// other member, no other wire into or out of a member): its response is then the product of the bands' own ones.
bool pureSeries(const doc::Project& p, doc::DeviceId chain, const std::vector<doc::BandInfo>& bands) {
  const doc::DeviceLoc loc = doc::findDevice(p, chain);
  if (loc.device == nullptr) return false;
  auto at = [&](doc::DeviceId id) -> size_t {  // 1 … n: the band's place; 0: not a band of it
    for (size_t i = 0; i < bands.size(); ++i)
      if (bands[i].group == id) return i + 1;
    return 0;
  };
  size_t members = 0;
  for (const doc::DevicePtr& d : loc.list())
    if (d->group == chain) {
      if (at(d->id) == 0) return false;
      ++members;
    }
  if (members != bands.size()) return false;
  size_t links = 0;
  const size_t n = bands.size();
  for (const doc::Wire& w : doc::wiresOf(p)) {
    const bool fromIn = w.from == chain && w.port == doc::kGroupInPort;
    const bool toOut = !w.toMixer() && w.to == chain && w.param == doc::kGroupOutPort;
    size_t a = at(w.from), z = w.toMixer() ? 0 : at(w.to);
    if (a != 0 && w.port == doc::kGroupInPort) a = 0;  // (a band's own In / Out terminal: inside it)
    if (z != 0 && w.param == doc::kGroupOutPort) z = 0;
    if (!fromIn && !toOut && a == 0 && z == 0) continue;
    if (w.toMixer() || w.fromNoteSource()) return false;
    const size_t from = fromIn ? 0 : a, to = toOut ? n + 1 : z;
    if ((!fromIn && (a == 0 || w.port != doc::kMainPort)) || (!toOut && (z == 0 || w.param != doc::kMainPort)) || to != from + 1)
      return false;
    ++links;
  }
  return links == n + 1;
}

struct Shown {
  doc::Snapshot snap;
  doc::DeviceId bound;
  bool eq = false;
  uint32_t mode = 0, active = 0;  // EQ: Mode (doc::EqMode) and the chain the Edit Chain block picks (0 in Stereo)
  doc::DeviceId chain[2];         // EQ: the chain groups; else [0] the bound group
  std::vector<std::unique_ptr<BandRef>> bands[2];
  bool series[2] = {false, false};             // a chain that is a pure series of its bands (its total: their product)
  std::vector<doc::DeviceId> members[2];       // its bands' groups, in series order
  std::vector<uint8_t> passes[2];              // per band: switched off (passes through)

  bool split() const noexcept { return eq && mode != 0; }
  // Re-read for snapshot `s` (the bands' objects are kept while the band list is the same: their bindings stay where a
  // target caches them, and a drag holds one). True: something may have changed.
  bool sync(const doc::Snapshot& s, doc::DeviceId g) {
    if (s == snap && g == bound) return false;
    snap = s;
    bound = g;
    eq = false;
    mode = active = 0;
    chain[0] = chain[1] = {};
    series[0] = series[1] = false;
    std::vector<doc::BandInfo> found[2];
    auto units = [&](doc::DeviceId id, double def) {
      const doc::Device* d = id.valid() && s ? doc::findDevice(*s, id).device : nullptr;
      return d != nullptr ? doc::valueBlockUnits(*d) : def;
    };
    if (s && g.valid()) {
      if (std::optional<doc::EqInfo> e = doc::eqInfo(*s, g)) {
        eq = true;
        mode = static_cast<uint32_t>(std::clamp<long>(std::lround(units(e->mode, 0.0)), 0, 2));
        active = mode == 0 ? 0u : (std::lround(units(e->editChain, 0.0)) >= 1 ? 1u : 0u);
        chain[0] = e->chain[0];
        chain[1] = e->chain[1];
        for (uint32_t c = 0; c < 2; ++c) series[c] = pureSeries(*s, e->chain[c], e->bands[c]);
        found[0] = std::move(e->bands[0]);
        found[1] = std::move(e->bands[1]);
      } else {
        chain[0] = g;
        if (std::optional<doc::BandInfo> b = doc::bandInfo(*s, g)) found[0].push_back(*b);
      }
    }
    for (uint32_t c = 0; c < 2; ++c) {
      std::vector<std::unique_ptr<BandRef>>& v = bands[c];
      bool same = v.size() == found[c].size();
      for (size_t i = 0; same && i < v.size(); ++i) {
        const doc::BandInfo& b = found[c][i];
        same = v[i]->group == b.group && v[i]->freq.device == b.frequency && v[i]->q.device == b.q && v[i]->gain.device == b.gain &&
               v[i]->stages.device == b.stages && v[i]->type.device == b.type;
      }
      if (!same) {
        v.clear();
        for (const doc::BandInfo& b : found[c]) {
          auto r = std::make_unique<BandRef>();
          r->group = b.group;
          r->freq = valueOf(b.frequency);
          r->q = valueOf(b.q);
          r->gain = valueOf(b.gain);
          r->stages = valueOf(b.stages);
          r->type = valueOf(b.type);
          v.push_back(std::move(r));
        }
      }
      for (size_t i = 0; i < v.size(); ++i) {
        BandRef& r = *v[i];
        r.index = static_cast<uint32_t>(i);
        const doc::Device* gd = doc::findDevice(*s, r.group).device;
        r.off = gd != nullptr && !gd->active;
        const doc::DeviceValue* solo = gd != nullptr ? gd->setting(doc::kBandSoloSetting) : nullptr;
        r.solo = solo != nullptr && solo->value >= 0.5;
        r.hz = units(r.freq.device, 1000.0);
        r.db = units(r.gain.device, 0.0);
        r.typeV = static_cast<uint32_t>(std::clamp<long>(std::lround(units(r.type.device, 4.0)), 0, 7));
        r.stagesV = static_cast<uint32_t>(std::clamp<long>(std::lround(units(r.stages.device, 1.0)), 1, 64));
      }
      members[c].clear();
      passes[c].clear();
      for (const auto& b : v) {
        members[c].push_back(b->group);
        passes[c].push_back(b->off ? 1 : 0);
      }
    }
    return true;
  }
  // The groups whose responses are drawn: a chain's total (the inactive one only when split) — its bands when it is a
  // pure series, else the chain group — and the active chain's bands when `allBands`.
  void groups(std::vector<doc::DeviceId>& out, bool allBands) const {
    out.clear();
    for (const uint32_t c : {active, 1 - active}) {
      if (c != active && !split()) continue;
      if (series[c]) out.insert(out.end(), members[c].begin(), members[c].end());
      else if (chain[c].valid()) out.push_back(chain[c]);
    }
    if (allBands && !series[active])
      for (const auto& b : bands[active]) out.push_back(b->group);
  }
  // Chain c's total.
  const GroupResponses::Curve* total(GroupResponses& r, uint32_t c, std::span<const double> hz, uint64_t key) const {
    if (series[c]) return r.series(chain[c], members[c], passes[c], hz, key);
    return r.curve(chain[c], hz, key);
  }
  const BandRef* find(doc::DeviceId g, uint32_t* chainOf = nullptr) const noexcept {
    for (uint32_t c = 0; c < 2; ++c)
      for (const auto& b : bands[c])
        if (b->group == g) {
          if (chainOf != nullptr) *chainOf = c;
          return b.get();
        }
    return nullptr;
  }
};

// A curve derived from an evaluation (kept until the evaluation is a new one).
struct Derived {
  uint64_t version = 0, version2 = 0;  // the evaluation(s) it was derived from
  graph::ResponseKind kind = graph::ResponseKind::Unavailable;
  std::vector<float> v;  // per grid point
  bool valid() const noexcept { return version != 0 && kind != graph::ResponseKind::Unavailable; }
  void clear() noexcept {
    version = version2 = 0;
    kind = graph::ResponseKind::Unavailable;
  }
};

// |H| in dB (In 0 → Out 0).
void deriveDb(const GroupResponses::Curve* c, Derived& d) {
  if (c == nullptr) return d.clear();
  if (c->version == d.version) return;
  d.version = c->version;
  d.kind = c->kind;
  d.v.resize(kPoints);
  float last = 0.0f;
  for (uint32_t k = 0; k < kPoints; ++k) {
    const double a = std::abs(c->at(k));
    const float db = std::isfinite(a) ? static_cast<float>(20.0 * std::log10(std::max(a, 1e-9))) : last;  // (a singular point)
    d.v[k] = last = db;
  }
}

// The grid shifted by ε_k = 10⁻³ of the step to the next point (the delay area's second evaluation).
constexpr uint64_t kShiftedKey = kPoints + 1;
const std::vector<double>& shiftedGrid() {
  static const std::vector<double> g = [] {
    const std::vector<double>& f = grid();
    std::vector<double> v(kPoints);
    for (uint32_t k = 0; k < kPoints; ++k) v[k] = f[k] + 1e-3 * (k + 1 < kPoints ? f[k + 1] - f[k] : f[k] - f[k - 1]);
    return v;
  }();
  return g;
}

// Group delay in ms at each grid point: −arg(H(f + ε) / H(f)) / (2π·ε) (the principal arg: exact while 2π·ε·τ < π).
// A point where H is 0 or not finite keeps the previous value.
void deriveDelay(const GroupResponses::Curve* c, const GroupResponses::Curve* s, Derived& d) {
  if (c == nullptr || s == nullptr || c->h.size() != s->h.size()) return d.clear();
  if (c->version == d.version && s->version == d.version2) return;
  d.version = c->version;
  d.version2 = s->version;
  d.kind = c->kind == graph::ResponseKind::Unavailable || s->kind == graph::ResponseKind::Unavailable ? graph::ResponseKind::Unavailable
           : c->kind == graph::ResponseKind::Approximate || s->kind == graph::ResponseKind::Approximate ? graph::ResponseKind::Approximate
                                                                                                         : graph::ResponseKind::Exact;
  d.v.resize(kPoints);
  const std::vector<double>& f = grid();
  const std::vector<double>& g = shiftedGrid();
  float last = 0.0f;
  for (uint32_t k = 0; k < kPoints; ++k) {
    const std::complex<double> a = c->at(k), b = s->at(k);
    const std::complex<double> r = b * std::conj(a);
    const double ms = -std::arg(r) / (2.0 * std::numbers::pi * (g[k] - f[k])) * 1000.0;
    d.v[k] = last = std::isfinite(ms) && std::norm(r) > 0.0 ? static_cast<float>(ms) : last;
  }
}

// Phase in degrees: wrapped to (−180°, 180°], or unwrapped — each step's turn count from the group delay `tau` (ms per
// grid point, trapezoid over the step; the phase step between neighbours can be many turns), or without `tau` the
// principal step.
void derivePhase(const GroupResponses::Curve* c, bool unwrap, const Derived* tau, Derived& d) {
  if (c == nullptr || (unwrap && (tau == nullptr || !tau->valid()))) return d.clear();
  const uint64_t key = c->version * 2 + (unwrap ? 1 : 0), key2 = unwrap ? tau->version2 : 0;
  if (key == d.version && key2 == d.version2) return;
  d.version = key;
  d.version2 = key2;
  d.kind = c->kind;
  d.v.resize(kPoints);
  constexpr double kPi = std::numbers::pi;
  if (!unwrap) {
    for (uint32_t k = 0; k < kPoints; ++k) d.v[k] = static_cast<float>(std::arg(c->at(k)) * 180.0 / kPi);
    return;
  }
  const std::vector<double>& f = grid();
  double prev = std::arg(c->at(0)), acc = prev;
  d.v[0] = static_cast<float>(acc * 180.0 / kPi);
  for (uint32_t k = 1; k < kPoints; ++k) {
    const double a = std::arg(c->at(k));
    const double principal = std::remainder(a - prev, 2.0 * kPi);
    const double predicted = -2.0 * kPi * (f[k] - f[k - 1]) * 0.5 * (tau->v[k - 1] + tau->v[k]) * 1e-3;
    const double turns = std::isfinite(predicted) ? std::round((predicted - principal) / (2.0 * kPi)) : 0.0;
    acc += principal + 2.0 * kPi * turns;
    prev = a;
    d.v[k] = static_cast<float>(acc * 180.0 / kPi);
  }
}

std::string fmt(const char* f, double v) {
  char b[48];
  std::snprintf(b, sizeof b, f, v);
  return b;
}

// The knob position of `b` that gives `units` (a value block's curve: Hz, dB through its output conversion, Q).
double positionOf(PanelTarget& t, const doc::Binding& b, double units) {
  const BoundInfo& i = t.info(b);
  if (!i.valid) return t.position(b);
  return std::clamp(dsp::curvePosition(i.curve, dsp::curveOutput(i.curve, units), t.position(b)), 0.0, 1.0);
}

// The vertical frequency rules (Hz; major: decades) and the labels, in placement priority.
constexpr double kRules[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
struct FreqLabel {
  double hz;
  std::string_view text;
};
constexpr FreqLabel kLabels[] = {{0, "0"},     {20000, "20k"}, {100, "100"}, {1000, "1k"}, {10000, "10k"}, {20, "20"},
                                 {50, "50"},   {200, "200"},   {500, "500"}, {2000, "2k"}, {5000, "5k"}};

// An area's backdrop, frequency rules and labels (y0 … y1 of the element; `rules`: false for a closed area's bar).
void paintPlate(ui::FacePainter& p, const ui::Theme& th, float w, float y0, float y1, bool rules = true) {
  const float h = y1 - y0;
  p.roundedRect({0.0f, y0, w, y1}, 4.0f, th.device.display);
  if (!rules) return;
  // A faint teal rise from the bottom.
  const Vec2 haze[2] = {{0.0f, y0 + h * 0.45f}, {w, y0 + h * 0.45f}};
  p.fillUnder(haze, y1, ui::withAlpha(th.device.response, 0), ui::mulAlpha(th.device.response, 0.07f));
  for (const double f : kRules) {
    const float x = static_cast<float>(tOfHz(f)) * w;
    const bool major = f == 100 || f == 1000 || f == 10000;
    p.line({x, y0}, {x, y1}, major ? 0.9f : 0.6f, major ? ui::mulAlpha(th.device.responseGrid, 2.0f) : th.device.responseGrid);
  }
}

void paintFreqLabels(ui::FacePainter& p, const ui::Theme& th, float w, float y, float px) {
  float placed[std::size(kLabels)][2];
  size_t n = 0;
  for (const FreqLabel& l : kLabels) {
    const float tw = 0.56f * px * static_cast<float>(l.text.size());
    float x = static_cast<float>(tOfHz(l.hz)) * w, x0 = x - tw * 0.5f, align = 0.5f;
    if (l.hz == 0) x = 3.0f, x0 = 3.0f, align = 0.0f;
    else if (l.hz >= kTopHz) x = w - 3.0f, x0 = w - 3.0f - tw, align = 1.0f;
    const float x1 = x0 + tw;
    bool free = x0 >= 1.0f && x1 <= w - 1.0f;
    for (size_t k = 0; free && k < n; ++k) free = x1 + 5.0f < placed[k][0] || x0 > placed[k][1] + 5.0f;
    if (!free) continue;
    placed[n][0] = x0;
    placed[n][1] = x1;
    ++n;
    p.textAligned({x, y}, l.text, px, th.device.displayText, align);
  }
}

// A small chip (a toggle / chooser drawn on a display): on → tinted.
void chip(ui::FacePainter& p, const ui::Theme& th, const Rect& r, std::string_view text, bool on, bool hot) {
  p.roundedRect(r, 2.5f, on ? ui::mulAlpha(th.device.response, 0.22f) : ui::mulAlpha(th.device.display, 0.85f));
  outlineRect(p, r, 2.5f, 0.8f, on ? ui::mulAlpha(th.device.response, 0.7f) : ui::mulAlpha(th.device.responseGrid, hot ? 4.0f : 2.5f));
  p.text({(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f}, text, 5.5f, on ? th.device.response : th.device.displayText);
}

bool inside(const Rect& r, Vec2 l) noexcept { return l.x >= r.x0 && l.x <= r.x1 && l.y >= r.y0 && l.y <= r.y1; }

double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- response ------------------------------------------------------------------------------------------------------

constexpr std::array<std::string_view, 2> kResponseSlots = {"signal", "input"};
constexpr std::array<std::string_view, 1> kValueSlot = {"value"};
constexpr double kZooms[] = {6.0, 12.0, 24.0, 48.0};

class ResponseDisplay final : public Element {
public:
  using Element::Element;

  bool interactive() const noexcept override { return true; }
  ui::Cursor cursor() const noexcept override {
    if (drag_.on) return ui::Cursor::Move;
    if (hover_ >= 0 || hoverChip_ >= 0) return ui::Cursor::Pointer;
    return hoverMag_ ? ui::Cursor::Crosshair : ui::Cursor::Inherit;
  }

  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    if (!env.hovered && !drag_.on) hover_ = -1, hoverChip_ = -1;
    layout(t);
    zoom_ = t.viewValue(id(), "zoom", 24.0);
    refresh(t);
    for (int32_t a = 0; a < kAreas; ++a) {
      if (!area_[a].open) {
        paintBar(p, th, a);
        continue;
      }
      if (a == kMag) paintMagnitude(p, th, t);
      else if (a == kPhase) paintPhase(p, th, t);
      else paintDelay(p, th, t);
    }
  }

  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    layout(t);
    refresh(t);
    const bool inMag = area_[kMag].open && e.local.y >= area_[kMag].y0 && e.local.y <= area_[kMag].y1;
    switch (e.kind) {
      case K::Move: {
        if (drag_.on) {
          dragTo(e, t);
          return true;
        }
        const int32_t c = chipAt(e.local), n = c < 0 && inMag ? nodeAt(e.local, e.pxPerUnit) : -1;
        const bool changed = n != hover_ || c != hoverChip_;
        hover_ = n;
        hoverChip_ = c;
        hoverMag_ = inMag;
        return changed;
      }
      case K::Down: {
        endWheel(t);
        if (const int32_t c = chipAt(e.local); c >= 0 && e.button == ui::PointerButton::Left) {
          toggleChip(c, t);
          return true;
        }
        if (!inMag) return false;
        const int32_t n = nodeAt(e.local, e.pxPerUnit);
        if (e.button == ui::PointerButton::Right) {
          if (n < 0) return false;
          select(n, t);
          openMenu(n, e, t);
          return true;
        }
        if (e.button != ui::PointerButton::Left) return false;
        if (n >= 0) {
          const BandRef& b = band(n);
          select(n, t);
          if (e.clicks >= 2) {
            if (b.hasGain()) t.setOnce(b.gain, positionOf(t, b.gain, 0.0), "Reset Band Gain");
            return true;
          }
          drag_ = {};
          drag_.on = true;
          drag_.band = n;
          drag_.startDev = e.device;
          drag_.startLocal = nodePos(b);
          drag_.qPos = b.q.bound() ? t.position(b.q) : 0.0;
          t.begin(b.freq, "Move Band");
          return true;
        }
        if (e.clicks >= 2 && shown_.eq) {
          addAt(e.local, t);
          return true;
        }
        t.selectGroup({});
        return true;
      }
      case K::Up:
        if (!drag_.on) return false;
        drag_.on = false;
        t.end();
        return true;
      case K::Wheel: return inMag && wheel(e, t);
      case K::Leave: {
        const bool changed = hover_ >= 0 || hoverChip_ >= 0;
        hover_ = hoverChip_ = -1;
        hoverMag_ = false;
        return changed;
      }
    }
    return false;
  }
  bool dragging() const noexcept override { return drag_.on; }
  void cancelDrag(PanelTarget& t) override {
    if (!drag_.on) return;
    drag_.on = false;
    t.cancel();
  }
  void endWheel(PanelTarget& t) override {
    if (!wheel_) return;
    wheel_ = false;
    t.end();
  }
  bool onKey(const ui::KeyEvent& e, PanelTarget& t) override {
    if (!e.down || (e.mods & ui::mod::kChordMask) != 0) return false;
    if (e.key != ui::key::kDelete && e.key != ui::key::kBackspace) return false;
    uint32_t chain = 0;
    const BandRef* b = shown_.find(t.selectedGroup(), &chain);
    if (b == nullptr || !shown_.eq || chain != shown_.active) return false;
    const doc::DeviceId g = b->group;
    t.selectGroup({});
    t.run([g](const doc::Project& p, doc::IdSource& ids) { return doc::ops::removeBand(p, ids, g); });
    return true;
  }
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    title = shown_.eq ? "EQ Response" : "Response";
    body = "The analytic response of " + (shown_.eq ? std::string("the chain being edited") : std::string("the group")) +
           ": magnitude, phase and group delay, each opened or closed by its title chip (MAG, PHASE, DELAY).";
    if (shown_.eq) body += " Double-click: add a band.";
    if (!shown_.bands[shown_.active].empty())
      body += " Drag a node: frequency and gain (Q for cuts and pass filters; Shift: fine). Wheel: Q, Alt + wheel: stages. "
              "Double-click a node: 0 dB. Right-click: type, stages, bypass, solo, delete.";
    (void)t;
  }

  // ---- the analyser ----
  VoiceTapReader* voiceReader(PanelTarget& t) const override {
    reader_.feed = spectrumOn(t) ? feedOf(t, 0) : nullptr;
    return reader_.feed != nullptr ? &reader_ : nullptr;
  }
  bool pollVoices(PanelTarget& t) override {
    if (!spectrumOn(t)) {
      out_.release();
      in_.release();
      return false;
    }
    syncFeeds(t);
    const double now = nowSeconds();
    bool changed = out_.frame(now, true);
    if (inputOn(t)) changed = in_.frame(now, true) || changed;
    return changed;
  }
  bool animating() const noexcept override { return out_.feed() != nullptr && out_.feed()->live(); }

  // Tests.
  void showSpectrum(devices::SpectrumSnapshot s) { out_.setShownForTest(std::move(s), 256.0f); }
  float drawnDb(float x) const {
    const Derived& d = total_[shown_.active];
    if (!d.valid()) return std::numeric_limits<float>::quiet_NaN();
    const float k = std::clamp(x / desc_.w, 0.0f, 1.0f) * (kPoints - 1);
    return d.v[static_cast<size_t>(std::lround(k))];
  }
  float pointX(uint32_t k) const noexcept { return desc_.w * static_cast<float>(k) / (kPoints - 1); }
  std::optional<Vec2> nodeOf(doc::DeviceId g) const {
    for (const auto& b : shown_.bands[shown_.active])
      if (b->group == g) return nodePos(*b);
    return std::nullopt;
  }
  float yOfDb(double db) const noexcept { return yOf(db); }
  bool series() const noexcept { return shown_.series[shown_.active]; }
  // EQ.4d: the areas (as laid out in the last paint / pointer event), the phase and delay areas' values and scales.
  test::ResponseArea areaOf(int32_t a) const noexcept { return {area_[a].open, area_[a].y0, area_[a].y1}; }
  bool unwrapped(const PanelTarget& t) const { return unwrapOn(t); }
  float stripAt(int32_t a, float x) const {
    const Derived& d = a == kPhase ? phaseTotal_ : delayTotal_;
    if (!d.valid()) return std::numeric_limits<float>::quiet_NaN();
    return d.v[static_cast<size_t>(std::lround(std::clamp(x / desc_.w, 0.0f, 1.0f) * (kPoints - 1)))];
  }
  std::pair<double, double> stripScale(int32_t a) const noexcept {
    return a == kPhase ? std::pair<double, double>{phaseLo_, phaseHi_} : std::pair<double, double>{delayLo_, delayHi_};
  }
  float delayY(double ms) const noexcept { return yOfDelay(ms); }
  const std::vector<float>& wraps() const noexcept { return wraps_; }
  Vec2 chipCentre(int32_t c) const noexcept {
    const Rect r = chipRect(c);
    return {(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f};
  }

private:
  struct Reader final : VoiceTapReader {
    devices::SpectrumFeed* feed = nullptr;
    uint32_t read(std::span<FaceVoice>) override { return 0; }
    uint64_t serial() const noexcept override { return 0; }
    bool live() const noexcept override { return feed != nullptr && feed->live(); }
  };
  struct Drag {
    bool on = false;
    int32_t band = -1;
    Vec2 startDev{0.0f, 0.0f}, startLocal{0.0f, 0.0f};
    double qPos = 0.0;
  };

  // ---- view state and the areas ----
  enum : int32_t { kMag = 0, kPhase = 1, kDelay = 2, kAreas = 3 };
  static constexpr float kGap = 2.0f, kBar = 15.0f;          // between areas; a closed area's bar
  static constexpr float kWeight[kAreas] = {3.0f, 1.0f, 1.0f};  // how the open areas share the height
  static constexpr std::string_view kOpenKey[kAreas] = {"magnitude", "phase", "delay"};
  static constexpr double kOpenDefault[kAreas] = {1.0, 1.0, 0.0};
  struct Span {
    float y0 = 0.0f, y1 = 0.0f;
    bool open = false;
  };
  bool areaOpen(const PanelTarget& t, int32_t a) const { return t.viewValue(id(), kOpenKey[a], kOpenDefault[a]) >= 0.5; }
  bool spectrumOn(const PanelTarget& t) const { return areaOpen(t, kMag) && t.viewValue(id(), "spectrum", 1.0) >= 0.5; }
  bool inputOn(const PanelTarget& t) const { return binding(1) != nullptr && t.viewValue(id(), "input", 0.0) >= 0.5; }
  bool unwrapOn(const PanelTarget& t) const { return t.viewValue(id(), "unwrap", 0.0) >= 0.5; }
  // The areas top to bottom: a closed one a bar, the open ones sharing the rest by kWeight (the element keeps its size).
  void layout(const PanelTarget& t) {
    float free = desc_.h - kGap * (kAreas - 1), weights = 0.0f;
    for (int32_t a = 0; a < kAreas; ++a) {
      area_[a].open = areaOpen(t, a);
      if (area_[a].open) weights += kWeight[a];
      else free -= kBar;
    }
    float y = 0.0f;
    for (int32_t a = 0; a < kAreas; ++a) {
      const float h = area_[a].open ? std::max(free, 0.0f) * kWeight[a] / weights : kBar;
      area_[a].y0 = y;
      area_[a].y1 = y + h;
      y += h + kGap;
    }
  }
  // Chips: 0 SPEC, 1 IN, 2 the dB zoom (on the magnitude area), 3 / 4 / 6 the areas' titles (MAG, PHASE, DELAY: open /
  // close; a closed area's whole bar), 5 WRAP / UNWRAP (on the phase area). test::ResponseChip names them.
  static int32_t titleChip(int32_t a) noexcept { return a == kMag ? 3 : a == kPhase ? 4 : 6; }
  Rect chipRect(int32_t c) const noexcept {
    if (c >= 3) {
      const int32_t a = c == 3 ? kMag : c == 6 ? kDelay : kPhase;
      const float y0 = area_[a].y0 + 3.0f, y1 = area_[a].y0 + 12.0f;
      if (c == 5) return {32.0f, y0, 62.0f, y1};
      return {3.0f, y0, c == 3 ? 23.0f : 29.0f, y1};
    }
    // From the right of the magnitude area: the zoom, IN, SPEC.
    const float wz = 20.0f, wi = 13.0f, ws = 20.0f, y0 = area_[kMag].y0 + 3.0f, y1 = area_[kMag].y0 + 13.0f, r = desc_.w - 3.0f;
    if (c == 2) return {r - wz, y0, r, y1};
    if (c == 1) return {r - wz - 3.0f - wi, y0, r - wz - 3.0f, y1};
    const float x1 = binding(1) != nullptr ? r - wz - wi - 6.0f : r - wz - 3.0f;
    return {x1 - ws, y0, x1, y1};
  }
  int32_t chipAt(Vec2 l) const noexcept {
    for (int32_t a = 0; a < kAreas; ++a) {
      const Span& s = area_[a];
      if (!s.open) {
        if (l.y >= s.y0 && l.y <= s.y1 && l.x >= 0.0f && l.x <= desc_.w) return titleChip(a);
        continue;
      }
      if (inside(chipRect(titleChip(a)), l)) return titleChip(a);
    }
    if (area_[kMag].open)
      for (int32_t c = 0; c < 3; ++c)
        if ((c != 1 || binding(1) != nullptr) && inside(chipRect(c), l)) return c;
    if (area_[kPhase].open && inside(chipRect(5), l)) return 5;
    return -1;
  }
  void toggleChip(int32_t c, PanelTarget& t) {
    if (c == 0) t.setViewValue(id(), "spectrum", t.viewValue(id(), "spectrum", 1.0) >= 0.5 ? 0.0 : 1.0);
    if (c == 1) t.setViewValue(id(), "input", inputOn(t) ? 0.0 : 1.0);
    if (c == 2) {
      const double z = t.viewValue(id(), "zoom", 24.0);
      size_t k = 0;
      while (k < std::size(kZooms) && kZooms[k] <= z + 1e-9) ++k;
      t.setViewValue(id(), "zoom", kZooms[k % std::size(kZooms)]);
    }
    for (int32_t a = 0; a < kAreas; ++a)
      if (c == titleChip(a)) t.setViewValue(id(), kOpenKey[a], areaOpen(t, a) ? 0.0 : 1.0);
    if (c == 5) t.setViewValue(id(), "unwrap", unwrapOn(t) ? 0.0 : 1.0);
    if (!spectrumOn(t)) {
      out_.release();
      in_.release();
    }
  }
  devices::SpectrumFeed* feedOf(PanelTarget& t, size_t slot) const {
    const doc::Binding* b = binding(slot);
    return b != nullptr && b->kind == doc::BindingKind::Signal ? t.signalFeed(*b) : nullptr;
  }
  void syncFeeds(PanelTarget& t) {
    if (!styled_) {
      devices::SpectrumPlotStyle s;
      s.bands = s.grid = s.freqLabels = s.dbLabels = false;
      s.floorDb = -90.0f;
      s.ceilDb = -6.0f;
      s.glow = 0.7f;
      s.fill = 0.45f;
      s.alpha = 0.6f;
      s.lineWidth = 1.2f;
      out_.setStyle(s);
      s.glow = 0.0f;
      s.fill = 0.25f;
      s.alpha = 0.28f;
      in_.setStyle(s);
      styled_ = true;
    }
    if (devices::SpectrumFeed* f = feedOf(t, 0); f != out_.feed()) out_.setFeed(f, {});
    devices::SpectrumFeed* fi = inputOn(t) ? feedOf(t, 1) : nullptr;
    if (fi != in_.feed()) in_.setFeed(fi, {});
  }

  // ---- geometry ----
  float yOf(double db) const noexcept {
    const float top = area_[kMag].y0 + 6.0f, bot = area_[kMag].y1 - 14.0f, mid = (top + bot) * 0.5f, half = (bot - top) * 0.5f;
    return mid - static_cast<float>(db / zoom_) * half;
  }
  double dbOf(float y) const noexcept {
    const float top = area_[kMag].y0 + 6.0f, bot = area_[kMag].y1 - 14.0f, mid = (top + bot) * 0.5f, half = (bot - top) * 0.5f;
    return (mid - y) / std::max(half, 1e-3f) * zoom_;
  }
  float xOf(double hz) const noexcept { return static_cast<float>(tOfHz(hz)) * desc_.w; }
  void curvePoints(const std::vector<float>& db, uint32_t a, uint32_t b, std::vector<Vec2>& out) const {
    out.resize(b - a);
    const float lo = area_[kMag].y0 - 4.0f, hi = area_[kMag].y1 + 4.0f;
    for (uint32_t k = a; k < b; ++k) out[k - a] = {pointX(k), std::clamp(yOf(db[k]), lo, hi)};
  }
  const BandRef& band(int32_t i) const { return *shown_.bands[shown_.active][static_cast<size_t>(i)]; }
  // A node: at its gain for the gain types, else on its own curve at its frequency.
  Vec2 nodePos(const BandRef& b, uint32_t chain = ~0u) const {
    const float x = xOf(b.hz);
    if (b.hasGain()) return {x, yOf(b.db)};
    if (chain == ~0u || chain == shown_.active)
      if (b.index < band_.size() && band_[b.index].valid()) {
        const float k = std::clamp(x / desc_.w, 0.0f, 1.0f) * (kPoints - 1);
        return {x, std::clamp(yOf(band_[b.index].v[static_cast<size_t>(std::lround(k))]), area_[kMag].y0 + 4.0f, area_[kMag].y1 - 4.0f)};
      }
    return {x, yOf(0.0)};
  }
  int32_t nodeAt(Vec2 l, float pxPerUnit) const {
    const float r = std::max(7.0f, 9.0f / std::max(pxPerUnit, 0.1f));
    int32_t best = -1;
    float bestD = r * r;
    const auto& v = shown_.bands[shown_.active];
    for (size_t i = 0; i < v.size(); ++i) {
      const Vec2 c = nodePos(*v[i]);
      const float d = (c.x - l.x) * (c.x - l.x) + (c.y - l.y) * (c.y - l.y);
      if (d <= bestD) best = static_cast<int32_t>(i), bestD = d;
    }
    return best;
  }

  // ---- the model and the curves ----
  void refresh(PanelTarget& t) {
    GroupResponses* r = t.responses();
    const doc::Binding* b = binding();
    if (r == nullptr || b == nullptr) {
      total_[0].clear();
      total_[1].clear();
      band_.clear();
      phaseTotal_.clear();
      phaseSel_.clear();
      delayTotal_.clear();
      delaySel_.clear();
      return;
    }
    shown_.sync(r->doc(), b->device);
    shown_.groups(want_, true);
    r->want(want_);
    const std::vector<double>& hz = grid();
    const uint32_t act = shown_.active;
    // Only what an open area draws is derived (a closed area costs nothing).
    if (area_[kMag].open) {
      deriveDb(shown_.total(*r, act, hz, kGridKey), total_[act]);
      if (shown_.split()) deriveDb(shown_.total(*r, 1 - act, hz, kGridKey), total_[1 - act]);
      else total_[1 - act].clear();
      const auto& bands = shown_.bands[act];
      band_.resize(bands.size());
      for (size_t i = 0; i < bands.size(); ++i) deriveDb(bands[i]->off ? nullptr : r->curve(bands[i]->group, hz, kGridKey), band_[i]);
    }
    const bool phase = area_[kPhase].open, delay = area_[kDelay].open, unwrap = phase && unwrapOn(t);
    if (!phase && !delay) return;
    // The selected band (of the active chain, on), faint behind the total.
    uint32_t chain = 0;
    const BandRef* sel = shown_.find(t.selectedGroup(), &chain);
    if (sel != nullptr && (chain != act || sel->off)) sel = nullptr;
    if (sel != nullptr) selColour_ = bandColour(sel->index);
    const GroupResponses::Curve* tot = shown_.total(*r, act, hz, kGridKey);
    const GroupResponses::Curve* one = sel != nullptr ? r->curve(sel->group, hz, kGridKey) : nullptr;
    // The delay (the delay area, and the unwrapped phase's turn counts): the second evaluation, on the shifted grid.
    if (delay || unwrap) {
      const std::vector<double>& sh = shiftedGrid();
      deriveDelay(tot, tot != nullptr ? shown_.total(*r, act, sh, kShiftedKey) : nullptr, delayTotal_);
      deriveDelay(one, one != nullptr ? r->curve(sel->group, sh, kShiftedKey) : nullptr, delaySel_);
    }
    if (phase) {
      derivePhase(tot, unwrap, &delayTotal_, phaseTotal_);
      derivePhase(one, unwrap, &delaySel_, phaseSel_);
    }
  }

  // ---- the areas ----
  Rect devRect(const ui::FacePainter& p, const Span& a) const noexcept {
    const ui::FaceTransform& tr = p.transform();
    return {tr.origin.x, tr.origin.y + a.y0 * tr.scale, tr.origin.x + desc_.w * tr.scale, tr.origin.y + a.y1 * tr.scale};
  }
  void paintBar(ui::FacePainter& p, const ui::Theme& th, int32_t a) {
    const Span& s = area_[a];
    paintPlate(p, th, desc_.w, s.y0, s.y1, false);
    chip(p, th, chipRect(titleChip(a)), a == kMag ? "MAG" : a == kPhase ? "PHASE" : "DELAY", false, hoverChip_ == titleChip(a));
    outlineRect(p, {0.5f, s.y0 + 0.5f, desc_.w - 0.5f, s.y1 - 0.5f}, 4.0f, 1.0f, ui::mulAlpha(th.device.responseGrid, 1.6f));
  }

  void paintMagnitude(ui::FacePainter& p, const ui::Theme& th, PanelTarget& t) {
    const Span& A = area_[kMag];
    const float w = desc_.w;
    ui::DrawList& dl = p.drawList();
    const Rect dev = devRect(p, A);
    paintPlate(p, th, w, A.y0, A.y1);
    dl.pushClip(dev);
    // The analyser behind: the output, the input dimmed.
    if (spectrumOn(t)) {
      devices::SpectrumAxis ax{dev.x0, dev.x1, static_cast<float>(kAxisHz), static_cast<float>(kTopHz)};
      if (inputOn(t) && in_.feed() != nullptr) {
        in_.wake();
        in_.paintSpectrum(dl, dev, ax, p.transform().scale);
      }
      out_.wake();
      out_.paintSpectrum(dl, dev, ax, p.transform().scale);
    }
    // dB rules.
    const double step = zoom_ <= 6.0 ? 2.0 : zoom_ <= 12.0 ? 3.0 : zoom_ <= 24.0 ? 6.0 : 12.0;
    for (double db = -zoom_; db <= zoom_ + 1e-9; db += step) {
      const float y = yOf(db);
      const bool zero = std::fabs(db) < 1e-9;
      p.line({0.0f, y}, {w, y}, zero ? 0.9f : 0.6f, zero ? ui::mulAlpha(th.device.responseGrid, 2.6f) : th.device.responseGrid);
    }
    const float y0 = yOf(0.0);
    // The inactive chain's total, translucent behind.
    if (shown_.split()) {
      const Derived& o = total_[1 - shown_.active];
      if (o.valid()) {
        curvePoints(o.v, 0, kPoints, pts_);
        p.polyline(pts_, 1.2f, ui::mulAlpha(th.device.response, 0.35f));
      }
    }
    // Each band's own curve, faint, filled toward 0 dB.
    const auto& bands = shown_.bands[shown_.active];
    if (bands.size() > 1 || !shown_.eq)
      for (size_t i = 0; i < bands.size() && i < band_.size(); ++i) {
        const Derived& d = band_[i];
        if (!d.valid() || bands[i]->off) continue;
        uint32_t a = 0, b = kPoints;
        // Only the span where the band does something, its line fading out toward 0 dB (its tails would show as
        // lines along 0 dB under the total).
        while (a < kPoints && std::fabs(d.v[a]) < 0.05f) ++a;
        while (b > a && std::fabs(d.v[b - 1]) < 0.05f) --b;
        if (b <= a) continue;
        a = a > 0 ? a - 1 : 0;
        b = std::min(kPoints, b + 1);
        curvePoints(d.v, a, b, pts_);
        const Rgba c = bandColour(bands[i]->index);
        const bool sel = bands[i]->group == t.selectedGroup();
        p.fillUnder(pts_, y0, ui::mulAlpha(c, sel ? 0.30f : 0.18f), ui::mulAlpha(c, 0.03f));
        const ui::FaceTransform& ft = p.transform();
        dev_.resize(pts_.size());
        cols_.resize(pts_.size());
        for (size_t k = 0; k < pts_.size(); ++k) {
          dev_[k] = ft.map(pts_[k]);
          cols_[k] = ui::mulAlpha(c, (sel ? 0.85f : 0.5f) * std::clamp(std::fabs(d.v[a + k]) / 1.5f, 0.0f, 1.0f));
        }
        ui::gradientPolyline(dl, dev_.data(), cols_.data(), static_cast<uint32_t>(dev_.size()), std::max(0.75f, 0.9f * ft.scale));
      }
    // The total.
    const Derived& tot = total_[shown_.active];
    if (tot.valid()) {
      curvePoints(tot.v, 0, kPoints, pts_);
      const Rgba m = th.device.response;
      p.fillUnder(pts_, y0, ui::mulAlpha(m, 0.16f), ui::mulAlpha(m, 0.02f));
      if (tot.kind == graph::ResponseKind::Approximate) {
        p.dashed(pts_, 1.5f, 4.0f, 3.0f, m);
      } else {
        p.polyline(pts_, 6.0f, ui::mulAlpha(m, 0.06f));
        p.polyline(pts_, 3.2f, ui::mulAlpha(m, 0.14f));
        p.polyline(pts_, 1.5f, m);
      }
    } else {
      p.text({w * 0.5f, A.y0 + 12.0f}, "no curve", 6.5f, th.device.displayText);
    }
    paintNodes(p, th, t);
    // dB labels on the right, frequency labels along the bottom.
    for (double db = -zoom_ + step; db < zoom_ - 1e-9; db += step) {
      if (std::fmod(std::fabs(db), 2.0 * step) > 1e-9 && std::fabs(db) > 1e-9) continue;
      p.textAligned({w - 3.0f, yOf(db) - 3.0f}, db > 0 ? fmt("+%.0f", db) : fmt("%.0f", db), 5.5f, th.device.displayText, 1.0f);
    }
    paintFreqLabels(p, th, w, A.y1 - 5.5f, 5.8f);
    paintReadout(p, th, t);
    // The chips.
    chip(p, th, chipRect(3), "MAG", true, hoverChip_ == 3);
    for (int32_t c = 0; c < 3; ++c) {
      const bool on = c == 0 ? spectrumOn(t) : c == 1 ? inputOn(t) : true;
      std::string label = c == 0 ? "SPEC" : c == 1 ? "IN" : fmt("\xC2\xB1%.0f", zoom_);
      if (c == 1 && binding(1) == nullptr) continue;
      chip(p, th, chipRect(c), label, on && (c != 1 || spectrumOn(t)), hoverChip_ == c);
    }
    dl.popClip();
    outlineRect(p, {0.5f, A.y0 + 0.5f, w - 0.5f, A.y1 - 0.5f}, 4.0f, 1.0f, ui::mulAlpha(th.device.responseGrid, 1.6f));
  }

  // The phase / delay areas' value → y.
  static float stripY(const Span& a, double v, double lo, double hi) noexcept {
    const float top = a.y0 + 4.0f, bot = a.y1 - 4.0f;
    return bot - static_cast<float>((v - lo) / std::max(hi - lo, 1e-9)) * (bot - top);
  }
  // Delay: 0 at the floor; below it (a negative delay) the shaded band, at the same scale when it fits in a quarter of
  // the strip, else squeezed into that quarter.
  float yOfDelay(double ms) const noexcept {
    const Span& a = area_[kDelay];
    const float top = a.y0 + 4.0f, bot = a.y1 - 4.0f;
    const float floorY = delayFloorY_;
    if (ms >= 0.0) return floorY - static_cast<float>(ms / std::max(delayHi_, 1e-12)) * (floorY - top);
    return floorY + static_cast<float>(ms / std::min(delayLo_, -1e-12)) * (bot - floorY);
  }
  void stripPoints(const std::vector<float>& v, uint32_t a, uint32_t b, const Span& s, bool delay, std::vector<Vec2>& out) const {
    out.resize(b - a);
    for (uint32_t k = a; k < b; ++k) {
      const double x = std::isfinite(v[k]) ? v[k] : 0.0;
      const float y = delay ? yOfDelay(x) : stripY(s, x, phaseLo_, phaseHi_);
      out[k - a] = {desc_.w * static_cast<float>(k) / (kPoints - 1), std::clamp(y, s.y0 - 4.0f, s.y1 + 4.0f)};
    }
  }
  // The selected band's curve (faint) and the total's on a strip.
  template <class Runs>
  void paintStripCurves(ui::FacePainter& p, const ui::Theme& th, const Span& s, const Derived& tot, const Derived& sel, bool delay,
                        Runs&& runs) {
    if (sel.valid())
      runs(sel.v, [&](uint32_t a, uint32_t b) {
        stripPoints(sel.v, a, b, s, delay, pts_);
        p.polyline(pts_, 0.9f, ui::mulAlpha(selColour_, 0.55f));
      }, ui::mulAlpha(selColour_, 0.3f), false);
    if (tot.valid()) {
      const Rgba m = th.device.response;
      runs(tot.v, [&](uint32_t a, uint32_t b) {
        stripPoints(tot.v, a, b, s, delay, pts_);
        p.polyline(pts_, 3.0f, ui::mulAlpha(m, 0.12f));
        if (tot.kind == graph::ResponseKind::Approximate) p.dashed(pts_, 1.2f, 4.0f, 3.0f, ui::mulAlpha(m, 0.9f));
        else p.polyline(pts_, 1.2f, ui::mulAlpha(m, 0.9f));
      }, ui::mulAlpha(m, 0.4f), true);
    } else {
      p.text({desc_.w * 0.5f, (s.y0 + s.y1) * 0.5f}, "no curve", 6.0f, th.device.displayText);
    }
  }

  void paintPhase(ui::FacePainter& p, const ui::Theme& th, PanelTarget& t) {
    const Span& A = area_[kPhase];
    const float w = desc_.w;
    const bool wrapped = !unwrapOn(t);
    paintPlate(p, th, w, A.y0, A.y1);
    p.drawList().pushClip(devRect(p, A));
    // The scale: wrapped ±180° (rules at ±90°); unwrapped auto over the total and the selected band, ≤ 3 spans.
    double lo = -180.0, hi = 180.0, step = 90.0;
    if (!wrapped) {
      float vlo = 0.0f, vhi = 0.0f;
      bool any = false;
      for (const Derived* d : {&phaseTotal_, &phaseSel_})
        if (d->valid())
          for (const float v : d->v)
            if (std::isfinite(v)) vlo = any ? std::min(vlo, v) : v, vhi = any ? std::max(vhi, v) : v, any = true;
      static constexpr double kDeg[] = {5, 10, 15, 30, 45, 90, 180, 360, 720, 1440, 2880, 5760, 11520, 23040, 46080};
      step = kDeg[std::size(kDeg) - 1];
      for (const double st : kDeg) {
        lo = std::floor(vlo / st - 1e-6) * st;
        hi = std::ceil(vhi / st + 1e-6) * st;
        if (hi - lo < st) hi = lo + st;
        if ((hi - lo) / st <= 3.0 + 1e-9) {
          step = st;
          break;
        }
      }
      if ((hi - lo) / step > 3.0 + 1e-9) lo = std::floor(vlo / step) * step, hi = std::ceil(vhi / step) * step;
      hi = std::max(hi, lo + step);
    }
    phaseLo_ = lo;
    phaseHi_ = hi;
    for (double v = lo; v <= hi + 1e-9; v += step) {
      const float y = stripY(A, v, lo, hi);
      const bool zero = std::fabs(v) < 1e-9;
      p.line({0.0f, y}, {w, y}, zero ? 0.9f : 0.6f, zero ? ui::mulAlpha(th.device.responseGrid, 2.6f) : th.device.responseGrid);
      if (v > lo + 1e-9 && v < hi - 1e-9)
        p.textAligned({w - 3.0f, y - 2.8f}, std::fabs(v) < 1e-9 ? std::string("0\xC2\xB0") : fmt("%+.0f\xC2\xB0", v), 5.0f,
                      th.device.displayText, 1.0f);
    }
    // A wrapped curve is cut where it wraps (no vertical line across the strip); at each cut a thin dotted line in its
    // colour joins its end at one edge (±180°) to its continuation at the other (EQ.4c), at the interpolated crossing.
    const float yTop = stripY(A, 180.0, lo, hi), yBot = stripY(A, -180.0, lo, hi);
    wraps_.clear();
    paintStripCurves(p, th, A, phaseTotal_, phaseSel_, false, [&](const std::vector<float>& v, auto&& draw, Rgba markerCol, bool total) {
      forRuns(v, wrapped, draw, [&](float k) {
        const float x = w * k / static_cast<float>(kPoints - 1);
        if (total) wraps_.push_back(x);
        const Vec2 seg[2] = {{x, yTop}, {x, yBot}};
        p.dashed(seg, 0.7f, 1.0f, 2.0f, markerCol);
      });
    });
    chip(p, th, chipRect(4), "PHASE", true, hoverChip_ == 4);
    chip(p, th, chipRect(5), wrapped ? "WRAP" : "UNWRAP", !wrapped, hoverChip_ == 5);
    p.drawList().popClip();
    outlineRect(p, {0.5f, A.y0 + 0.5f, w - 0.5f, A.y1 - 0.5f}, 4.0f, 1.0f, ui::mulAlpha(th.device.responseGrid, 1.6f));
  }

  void paintDelay(ui::FacePainter& p, const ui::Theme& th, PanelTarget& t) {
    (void)t;
    const Span& A = area_[kDelay];
    const float w = desc_.w, top = A.y0 + 4.0f, bot = A.y1 - 4.0f;
    paintPlate(p, th, w, A.y0, A.y1);
    p.drawList().pushClip(devRect(p, A));
    // The scale: 0 at the floor up to the largest delay drawn plus a little headroom, in ≤ 4 nice steps (ms).
    float vmin = 0.0f, vmax = 0.0f;
    for (const Derived* d : {&delayTotal_, &delaySel_})
      if (d->valid())
        for (const float v : d->v)
          if (std::isfinite(v)) vmin = std::min(vmin, v), vmax = std::max(vmax, v);
    static constexpr double kMs[] = {0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000};
    const double want = std::max(static_cast<double>(vmax) * 1.08, kMs[0]);
    double step = kMs[std::size(kMs) - 1];
    for (const double st : kMs)
      if (std::ceil(want / st - 1e-9) <= 4.0) {
        step = st;
        break;
      }
    delayHi_ = std::max(std::ceil(want / step - 1e-9), 1.0) * step;
    // True negative values (a cut's group delay near its centre) below the floor: ignored when they are only rounding.
    const bool negative = vmin < -std::max(1e-6, 1e-3 * delayHi_);
    delayLo_ = negative ? static_cast<double>(vmin) : 0.0;
    const double share = negative ? std::min(-delayLo_ / (delayHi_ - delayLo_), 0.25) : 0.0;
    delayFloorY_ = bot - static_cast<float>(share) * (bot - top);
    if (negative) p.roundedRect({0.0f, delayFloorY_, w, bot + 1.0f}, 0.0f, ui::mulAlpha(th.device.responseGrid, 0.9f));
    // The rules (0: the floor, brighter), then the labels on the right: 0 and the top first, the others where they do
    // not crowd a placed one; the unit beside the top; the most negative value inside its band when it has room.
    const char* f = step < 0.01 ? "%.3f" : step < 0.1 ? "%.2f" : step < 1.0 ? "%.1f" : "%.0f";
    const int32_t ticks = static_cast<int32_t>(std::lround(delayHi_ / step));
    for (int32_t i = 0; i <= ticks; ++i) {
      const float y = yOfDelay(step * i);
      p.line({0.0f, y}, {w, y}, i == 0 ? 1.0f : 0.6f, i == 0 ? ui::mulAlpha(th.device.responseGrid, 3.0f) : th.device.responseGrid);
    }
    float placed[24];
    size_t n = 0;
    auto label = [&](float y, const std::string& text, float px, Rgba col) {
      for (size_t k = 0; k < n; ++k)
        if (std::fabs(placed[k] - y) < 6.0f) return;
      if (n < std::size(placed)) placed[n++] = y;
      p.textAligned({w - 3.0f, y}, text, px, col, 1.0f);
    };
    label(delayFloorY_ - 2.8f, "0", 5.0f, th.device.displayText);
    const std::string topText = fmt(f, delayHi_);
    label(yOfDelay(delayHi_) + 3.2f, topText, 5.0f, th.device.displayText);
    p.textAligned({w - 5.0f - 0.56f * 5.0f * static_cast<float>(topText.size()), yOfDelay(delayHi_) + 3.4f}, "ms", 4.2f,
                  ui::mulAlpha(th.device.displayText, 0.7f), 1.0f);
    for (int32_t i = 1; i < ticks; ++i) label(yOfDelay(step * i) - 2.8f, fmt(f, step * i), 5.0f, th.device.displayText);
    if (negative && bot - delayFloorY_ >= 5.0f)
      label((delayFloorY_ + bot) * 0.5f + 0.5f, fmt(-delayLo_ < 0.1 ? "%.3f" : -delayLo_ < 1.0 ? "%.2f" : "%.1f", delayLo_), 4.2f,
            ui::mulAlpha(th.device.displayText, 0.8f));
    paintStripCurves(p, th, A, delayTotal_, delaySel_, true, [&](const std::vector<float>& v, auto&& draw, Rgba, bool) {
      forRuns(v, false, draw, [](float) {});
    });
    chip(p, th, chipRect(6), "DELAY", true, hoverChip_ == 6);
    p.drawList().popClip();
    outlineRect(p, {0.5f, A.y0 + 0.5f, w - 0.5f, A.y1 - 0.5f}, 4.0f, 1.0f, ui::mulAlpha(th.device.responseGrid, 1.6f));
  }

  // Runs [a, b) of points to draw as one line: all of them, or (wrapped) cut where the phase wraps; each cut also
  // reports its crossing (a fractional point index): where the unwrapped phase, linear between the two points, passes
  // the odd multiple of 180° (so it does not move with the grid's resolution).
  template <class F, class W>
  static void forRuns(const std::vector<float>& v, bool wrapped, F&& draw, W&& wrap) {
    uint32_t a = 0;
    for (uint32_t k = 1; k <= kPoints; ++k)
      if (k == kPoints || (wrapped && std::fabs(v[k] - v[k - 1]) > 180.0f)) {
        if (k - a >= 2) draw(a, k);
        a = k;
        if (k == kPoints) continue;
        const float d = v[k] - v[k - 1] + (v[k] < v[k - 1] ? 360.0f : -360.0f);  // the unwrapped step
        const float edge = d > 0.0f ? 180.0f : -180.0f;
        wrap(static_cast<float>(k - 1) + (d != 0.0f ? std::clamp((edge - v[k - 1]) / d, 0.0f, 1.0f) : 0.5f));
      }
  }

  void paintNodes(ui::FacePainter& p, const ui::Theme& th, PanelTarget& t) {
    if (shown_.split())
      for (const auto& b : shown_.bands[1 - shown_.active]) {
        const Vec2 c = nodePos(*b, 1 - shown_.active);
        p.disc(c, 3.6f, ui::mulAlpha(th.device.display, 0.6f));
        p.disc(c, 3.0f, ui::mulAlpha(b->off ? th.device.bandOff : bandColour(b->index), 0.35f));
      }
    const auto& v = shown_.bands[shown_.active];
    for (size_t i = 0; i < v.size(); ++i) {
      const BandRef& b = *v[i];
      const Vec2 c = nodePos(b);
      const Rgba col = b.off ? th.device.bandOff : bandColour(b.index);
      const bool sel = b.group == t.selectedGroup(), hot = static_cast<int32_t>(i) == hover_ || (drag_.on && drag_.band == static_cast<int32_t>(i));
      // Wires drive a control of the band: its modulation (a knob's ring): the swept range along the control's axis,
      // or a ring when the host has no live range.
      paintModulation(p, th, t, b, c);
      if (!b.off) {
        p.beginGlow();
        p.glow(c, sel || hot ? 12.0f : 9.0f, ui::mulAlpha(col, sel || hot ? 0.45f : 0.3f));
        p.endGlow();
      }
      if (sel) p.ring(c, 7.2f, 1.3f, th.device.bandSelected);
      else if (hot) p.ring(c, 7.2f, 1.0f, ui::mulAlpha(th.device.bandSelected, 0.55f));
      p.disc(c, 5.4f, th.device.display);
      if (b.off) p.ring(c, 4.2f, 1.2f, col);
      else p.disc(c, 4.4f, col);
      p.text(c, std::to_string(i + 1), 4.8f, b.off ? col : th.device.bandText);
    }
  }
  void paintModulation(ui::FacePainter& p, const ui::Theme& th, PanelTarget& t, const BandRef& b, Vec2 c) {
    bool any = false;
    auto live = [&](const doc::Binding& x, bool horizontal) {
      if (!x.bound()) return;
      const ui::ModulationRing* m = t.modulation(x);
      if (m == nullptr || !m->active) {
        any = any || t.driven(x);
        return;
      }
      any = true;
      const BoundInfo& i = t.info(x);
      auto at = [&](float pos) {
        const double u = dsp::evalCurveUnits(i.curve, std::clamp(static_cast<double>(pos), 0.0, 1.0));
        return horizontal ? xOf(u) : yOf(u);
      };
      const float a = at(m->lo), z = at(m->hi);
      const Rgba col = th.face.valueDriven;
      if (horizontal) p.line({a, c.y}, {z, c.y}, 2.0f, col);
      else p.line({c.x, a}, {c.x, z}, 2.0f, col);
      if (m->current >= 0.0f) {
        const float q = at(m->current);
        p.disc(horizontal ? Vec2{q, c.y} : Vec2{c.x, q}, 2.0f, th.face.value);
      }
    };
    live(b.freq, true);
    if (b.hasGain()) live(b.gain, false);
    else if (b.q.bound()) any = any || t.driven(b.q);
    if (any) p.ring(c, 9.0f, 1.2f, th.face.valueDriven);
  }
  void paintReadout(ui::FacePainter& p, const ui::Theme& th, PanelTarget& t) {
    int32_t i = drag_.on ? drag_.band : hover_;
    if (i < 0) {
      uint32_t chain = 0;
      const BandRef* s = shown_.find(t.selectedGroup(), &chain);
      if (s == nullptr || chain != shown_.active) return;
      i = static_cast<int32_t>(s->index);
    }
    if (i < 0 || static_cast<size_t>(i) >= shown_.bands[shown_.active].size()) return;
    const BandRef& b = band(i);
    const Rgba col = b.off ? th.device.bandOff : bandColour(b.index);
    std::string s = fmt(b.hz >= 1000.0 ? "%.2f kHz" : "%.0f Hz", b.hz >= 1000.0 ? b.hz / 1000.0 : b.hz);
    if (b.hasGain()) s += "   " + fmt("%+.1f dB", b.db);
    if (b.q.bound()) s += "   Q " + t.format(b.q, t.position(b.q));
    if (b.hasStages() && b.stagesV > 1) s += "   \xC3\x97" + std::to_string(b.stagesV);
    if (b.off) s += "   off";
    const float x = 27.0f, y = area_[kMag].y0 + 8.0f;
    if (b.type.bound()) {
      typeGlyph(p, {x, y - 3.0f, x + 10.0f, y + 3.4f}, b.typeV, 0.9f, col);
      p.textAligned({x + 13.0f, y}, kCodes[b.typeV], 5.8f, col, 0.0f);
    }
    p.textAligned({x + (b.type.bound() ? 26.0f : 0.0f), y}, s, 5.8f, ui::lerp(col, th.device.bandSelected, 0.5f), 0.0f);
  }

  // ---- gestures ----
  void select(int32_t i, PanelTarget& t) { t.selectGroup(band(i).group); }
  void dragTo(const FacePointer& e, PanelTarget& t) {
    if (drag_.band < 0 || static_cast<size_t>(drag_.band) >= shown_.bands[shown_.active].size()) return;
    const BandRef& b = band(drag_.band);
    const float fine = (e.mods & ui::mod::kShift) != 0 ? 0.1f : 1.0f;
    const float k = fine / std::max(e.pxPerUnit, 0.01f);
    const Vec2 l{drag_.startLocal.x + (e.device.x - drag_.startDev.x) * k, drag_.startLocal.y + (e.device.y - drag_.startDev.y) * k};
    const double hz = std::clamp(hzOfT(std::clamp(l.x / desc_.w, 0.0f, 1.0f)), 0.0, kTopHz);
    t.setPosition(b.freq, positionOf(t, b.freq, hz));
    if (b.hasGain()) {
      t.setPosition(b.gain, positionOf(t, b.gain, std::clamp(dbOf(l.y), -zoom_, zoom_)));
    } else if (b.q.bound()) {
      const double per = 200.0 * std::max(e.uiScale, 0.5f);
      t.setPosition(b.q, std::clamp(drag_.qPos - (e.device.y - drag_.startDev.y) * fine / per, 0.0, 1.0));
    }
  }
  bool wheel(const FacePointer& e, PanelTarget& t) {
    if (!e.valueGestures || drag_.on || e.wheel.y == 0.0f) return false;
    const int32_t n = nodeAt(e.local, e.pxPerUnit);
    if (n < 0) return false;
    const BandRef& b = band(n);
    const float dir = e.wheel.y > 0.0f ? 1.0f : -1.0f;
    if ((e.mods & ui::mod::kAlt) != 0) {  // stages, one step per notch
      endWheel(t);
      if (!b.hasStages()) return true;
      const doc::DeviceId g = b.group;
      const uint32_t s = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(b.stagesV) + (dir > 0 ? 1 : -1), 1, 64));
      if (s != b.stagesV) t.run([g, s](const doc::Project& p, doc::IdSource&) { return doc::ops::setBandStages(p, g, s); });
      return true;
    }
    if (!b.q.bound()) return true;
    if (wheel_ && wheelBand_ != b.group) endWheel(t);
    if (!wheel_) {  // a run of notches: one gesture until the host ends it
      wheel_ = true;
      wheelBand_ = b.group;
      wheelPos_ = t.position(b.q);
      t.begin(b.q, "Band Q");
    }
    const double step = (e.mods & ui::mod::kShift) != 0 ? 0.004 : 0.02;
    wheelPos_ = std::clamp(wheelPos_ + step * static_cast<double>(e.wheel.y), 0.0, 1.0);
    t.setPosition(b.q, wheelPos_);
    return true;
  }
  void addAt(Vec2 l, PanelTarget& t) {
    const doc::DeviceId eq = shown_.bound;
    const uint32_t chain = shown_.active;
    const double hz = std::clamp(hzOfT(std::clamp(l.x / desc_.w, 0.0f, 1.0f)), 1.0, kTopHz);
    const double db = std::clamp(dbOf(l.y), -24.0, 24.0);
    std::vector<doc::DeviceId> before;
    for (const auto& b : shown_.bands[chain]) before.push_back(b->group);
    if (!t.run([=](const doc::Project& p, doc::IdSource& ids) { return doc::ops::addBand(p, ids, eq, chain, 4, hz, db); })) return;
    // The new band is selected.
    if (GroupResponses* r = t.responses(); r != nullptr && r->doc())
      if (std::optional<doc::EqInfo> e = doc::eqInfo(*r->doc(), eq))
        for (const doc::BandInfo& b : e->bands[chain])
          if (std::find(before.begin(), before.end(), b.group) == before.end()) t.selectGroup(b.group);
  }
  void openMenu(int32_t n, const FacePointer& e, PanelTarget& t) {
    ui::Widget* host = t.menuHost();
    if (host == nullptr) return;
    const BandRef& b = band(n);
    const doc::DeviceId g = b.group;
    PanelTarget* target = &t;
    auto run = [target](PanelTarget::EditBuilder f) { return [target, f = std::move(f)] { (void)target->run(f); }; };
    std::vector<ui::MenuItem> menu;
    if (b.type.bound()) {
      std::vector<ui::MenuItem> types;
      const std::vector<std::string> names = splitItems(doc::kBandTypeItems);
      for (uint32_t k = 0; k < names.size(); ++k) {
        ui::MenuItem m = ui::MenuItem::item(names[k], run([g, k](const doc::Project& p, doc::IdSource&) { return doc::ops::setBandType(p, g, k); }));
        m.checked = k == b.typeV;
        types.push_back(std::move(m));
      }
      menu.push_back(ui::MenuItem::sub("Type", std::move(types)));
    }
    if (b.hasStages()) {
      std::vector<ui::MenuItem> stages;
      for (const uint32_t s : {1u, 2u, 3u, 4u, 6u, 8u, 12u, 16u, 24u, 32u, 48u, 64u}) {
        ui::MenuItem m = ui::MenuItem::item(std::to_string(s) + (s == 1 ? " stage" : " stages"),
                                            run([g, s](const doc::Project& p, doc::IdSource&) { return doc::ops::setBandStages(p, g, s); }));
        m.checked = s == b.stagesV;
        stages.push_back(std::move(m));
      }
      menu.push_back(ui::MenuItem::sub("Stages", std::move(stages)));
    }
    const bool off = b.off, solo = b.solo;
    ui::MenuItem bypass = ui::MenuItem::item("Bypass", run([g, off](const doc::Project& p, doc::IdSource&) { return doc::ops::setBandBypass(p, g, !off); }));
    bypass.checked = off;
    menu.push_back(std::move(bypass));
    if (shown_.eq) {
      ui::MenuItem so = ui::MenuItem::item("Solo", run([g, solo](const doc::Project& p, doc::IdSource&) { return doc::ops::setBandSolo(p, g, !solo); }));
      so.checked = solo;
      menu.push_back(std::move(so));
      menu.push_back(ui::MenuItem::sep());
      menu.push_back(ui::MenuItem::item("Delete", [target, g] {
        target->selectGroup({});
        (void)target->run([g](const doc::Project& p, doc::IdSource& ids) { return doc::ops::removeBand(p, ids, g); });
      }, "Del"));
    }
    (void)ui::openContextMenu(*host, e.device, std::move(menu));
  }

  Shown shown_;
  std::vector<doc::DeviceId> want_;
  Span area_[kAreas];
  Derived total_[2];
  std::vector<Derived> band_;
  Derived phaseTotal_, phaseSel_, delayTotal_, delaySel_;  // the phase / delay areas: the total, the selected band
  Rgba selColour_ = 0;
  double phaseLo_ = -180.0, phaseHi_ = 180.0, delayLo_ = 0.0, delayHi_ = 1.0;  // their scales (last paint)
  float delayFloorY_ = 0.0f;                                                    // the y of 0 ms (last paint)
  std::vector<float> wraps_;  // the phase total's wrap markers' x (last paint; tests)
  std::vector<Vec2> pts_, dev_;
  std::vector<Rgba> cols_;
  double zoom_ = 24.0;
  int32_t hover_ = -1, hoverChip_ = -1;
  bool hoverMag_ = false;
  Drag drag_;
  bool wheel_ = false;
  doc::DeviceId wheelBand_;
  double wheelPos_ = 0.0;
  devices::SpectrumPlot out_, in_;
  bool styled_ = false;
  mutable Reader reader_;
};

// ---- band type -----------------------------------------------------------------------------------------------------

class BandType final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"items", StyleSpec::Type::Text, 0, ""},
      {"band", StyleSpec::Type::Number, 0.0, {}, {}, 0.0, 63.0},
      {"label", StyleSpec::Type::Text, 0, "-"},
  };
  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 2; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.device.display : ui::mulAlpha(th.device.responseGrid, 3.0f);
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rect r{0.5f, 0.5f, desc_.w - 0.5f, desc_.h - 0.5f};
    if (layer == 0) {
      p.roundedRect(r, 3.0f, ui::rgba(255, 255, 255));
      return;
    }
    outlineRect(p, r, 3.0f, 1.0f, ui::rgba(255, 255, 255));
    const float cx = r.x1 - 6.0f, cy = (r.y0 + r.y1) * 0.5f;  // the chevron
    p.line({cx - 2.0f, cy - 1.0f}, {cx, cy + 1.1f}, 1.0f, ui::rgba(255, 255, 255));
    p.line({cx, cy + 1.1f}, {cx + 2.0f, cy - 1.0f}, 1.0f, ui::rgba(255, 255, 255));
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const Rect r{0.5f, 0.5f, desc_.w - 0.5f, desc_.h - 0.5f};
    if (env.hovered) outlineRect(p, r, 3.0f, 1.2f, ui::mulAlpha(th.device.response, 0.6f));
    if (binding() == nullptr) return;
    const uint32_t type = static_cast<uint32_t>(positionIndex(pos(t), std::max<size_t>(items().size(), 2)));
    const Rgba col = editable(t) ? bandColour(static_cast<uint32_t>(std::max(0.0, num("band", 0.0)))) : th.face.ink;
    const float cy = (r.y0 + r.y1) * 0.5f, gh = std::min(7.0f, desc_.h - 8.0f);
    typeGlyph(p, {r.x0 + 5.0f, cy - gh * 0.5f, r.x0 + 5.0f + gh * 14.0f / 9.0f, cy + gh * 0.5f}, type, 1.1f, col);
    p.textAligned({r.x0 + 8.0f + gh * 14.0f / 9.0f, cy}, type < kCodes.size() ? kCodes[type] : "?", 7.0f, col, 0.0f);
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    if (e.kind != FacePointer::Kind::Down || e.button != ui::PointerButton::Left || !editable(t)) return false;
    ui::Widget* host = t.menuHost();
    const doc::Binding* b = binding();
    const std::vector<std::string> it = items();
    if (host == nullptr || b == nullptr || it.empty()) return true;
    const size_t now = positionIndex(pos(t), it.size());
    std::vector<ui::MenuItem> menu;
    PanelTarget* target = &t;
    const doc::Binding bound = *b;
    for (size_t k = 0; k < it.size(); ++k) {
      ui::MenuItem m = ui::MenuItem::item(it[k], [target, bound, k, n = it.size()] { target->setOnce(bound, indexPosition(k, n), "Band Type"); });
      m.checked = k == now;
      menu.push_back(std::move(m));
    }
    (void)ui::openContextMenu(*host, e.device, std::move(menu));
    return true;
  }
  std::optional<double> selection(const PanelTarget& t) const override {
    if (binding() == nullptr) return std::nullopt;
    return static_cast<double>(positionIndex(pos(t), std::max<size_t>(items().size(), 2)));
  }

private:
  std::vector<std::string> items() const {
    std::vector<std::string> v = splitItems(txt("items", ""));
    if (v.empty()) v = splitItems(doc::kBandTypeItems);
    return v;
  }
  static double indexPosition(size_t index, size_t count) noexcept {
    return count <= 1 ? 0.0 : static_cast<double>(index) / static_cast<double>(count - 1);
  }
  static size_t positionIndex(double p, size_t count) noexcept {
    if (count <= 1) return 0;
    return static_cast<size_t>(std::clamp(std::lround(p * static_cast<double>(count - 1)), 0l, static_cast<long>(count - 1)));
  }
  std::string hint() const override { return " Click to choose the band's type."; }
};

template <class T>
std::unique_ptr<Element> make(const doc::PanelElement& d) {
  return std::make_unique<T>(d);
}

}  // namespace

void registerResponseElements(void (*reg)(std::string_view, std::string_view, std::span<const StyleSpec>, std::span<const std::string_view>,
                                          float, float, ElementFactory)) {
  reg("response", "Response", {}, kResponseSlots, 400, 156, &make<ResponseDisplay>);
  reg("bandtype", "Band Type", BandType::kStyle, kValueSlot, 56, 18, &make<BandType>);
}

// ---- tests (pq/views/panel/response_display.h) ------------------------------------------------------------------------

namespace test {

bool showSpectrum(Element& e, devices::SpectrumSnapshot s) {
  auto* r = dynamic_cast<ResponseDisplay*>(&e);
  if (r == nullptr) return false;
  r->showSpectrum(std::move(s));
  return true;
}
std::optional<float> drawnDb(const Element& e, float x) {
  const auto* r = dynamic_cast<const ResponseDisplay*>(&e);
  if (r == nullptr) return std::nullopt;
  return r->drawnDb(x);
}
uint32_t responsePoints() noexcept { return kPoints; }
double responseHz(uint32_t k) noexcept { return grid()[std::min(k, kPoints - 1)]; }
float responseX(const Element& e, uint32_t k) {
  const auto* r = dynamic_cast<const ResponseDisplay*>(&e);
  return r != nullptr ? r->pointX(k) : 0.0f;
}
std::optional<ui::Vec2> nodeOf(const Element& e, doc::DeviceId band) {
  const auto* r = dynamic_cast<const ResponseDisplay*>(&e);
  return r != nullptr ? r->nodeOf(band) : std::nullopt;
}
std::optional<float> yOfDb(const Element& e, double db) {
  const auto* r = dynamic_cast<const ResponseDisplay*>(&e);
  if (r == nullptr) return std::nullopt;
  return r->yOfDb(db);
}
bool drawsSeries(const Element& e) {
  const auto* r = dynamic_cast<const ResponseDisplay*>(&e);
  return r != nullptr && r->series();
}
std::optional<ResponseArea> area(const Element& e, int32_t a) {
  const auto* r = dynamic_cast<const ResponseDisplay*>(&e);
  if (r == nullptr || a < 0 || a > 2) return std::nullopt;
  return r->areaOf(a);
}
ui::Vec2 chip(const Element& e, ResponseChip c) {
  const auto* r = dynamic_cast<const ResponseDisplay*>(&e);
  return r != nullptr ? r->chipCentre(static_cast<int32_t>(c)) : ui::Vec2{0.0f, 0.0f};
}
bool phaseUnwrapped(const Element& e, const PanelTarget& t) {
  const auto* r = dynamic_cast<const ResponseDisplay*>(&e);
  return r != nullptr && r->unwrapped(t);
}
float phaseAt(const Element& e, float x) {
  const auto* r = dynamic_cast<const ResponseDisplay*>(&e);
  return r != nullptr ? r->stripAt(1, x) : std::numeric_limits<float>::quiet_NaN();
}
std::pair<double, double> phaseScale(const Element& e) {
  const auto* r = dynamic_cast<const ResponseDisplay*>(&e);
  return r != nullptr ? r->stripScale(1) : std::pair<double, double>{0.0, 0.0};
}
std::vector<float> phaseWraps(const Element& e) {
  const auto* r = dynamic_cast<const ResponseDisplay*>(&e);
  return r != nullptr ? r->wraps() : std::vector<float>{};
}
float delayAt(const Element& e, float x) {
  const auto* r = dynamic_cast<const ResponseDisplay*>(&e);
  return r != nullptr ? r->stripAt(2, x) : std::numeric_limits<float>::quiet_NaN();
}
std::pair<double, double> delayScale(const Element& e) {
  const auto* r = dynamic_cast<const ResponseDisplay*>(&e);
  return r != nullptr ? r->stripScale(2) : std::pair<double, double>{0.0, 0.0};
}
float delayY(const Element& e, double ms) {
  const auto* r = dynamic_cast<const ResponseDisplay*>(&e);
  return r != nullptr ? r->delayY(ms) : 0.0f;
}
double axisHz(double t) noexcept { return hzOfT(t); }
double axisT(double hz) noexcept { return tOfHz(hz); }

}  // namespace test

}  // namespace pq::views::panel
