// DY.3 (docs/plans/DY.md "DY.3"): the `transfer` display — the static transfer curve of a memoryless group
// (doc::GroupTransfer through views::GroupResponses), any group's panel can place it:
//
//   binding    slot 0: a Signal binding on a group's In (`input`, doc::kGroupInPort): it names the group, and its tap is
//              the live level the dot shows (pq/document/panel.h "Displays tapped by the lowering").
//   axes       x = input dB, y = output dB, both over `min` … `max` (−60 … +6 by default), a rule every 6 dB (12 dB
//              past a 48 dB span), 0 dB brighter, the 1:1 diagonal dashed.
//   units      what the group's In and Out carry: `linear` (the default: an amplitude, a waveshaper's — In is evaluated
//              at 10^(x/20), Out drawn as 20·log10|y|) or `db` (a level in dB: a compressor's gain computer).
//   output     `value` (the default: Out is the output) or `reduction` (Out is dB of gain reduction, ≥ 0: output dB =
//              input dB − Out) — a compressor's "Gain Computer" child group (In = level dB, Out = reduction dB) draws
//              its compression curve with units db, output reduction; no compressor special case.
//   dot        (1 / 0) a dot on the curve at the live input level: the tap's largest value in its last 50 ms (units db:
//              the level itself; linear: its peak |x| in dB). The feed is acquired only while the display paints
//              (SignalWindow): a panel that is not shown costs nothing.
//   no curve   a group with a stateful atom between In and Out (z1, a filter, `follow`), or one an unknown value drives,
//              has no static curve: the axes and the diagonal with a quiet "no static curve".
//
// Cost: the curve comes from GroupResponses::transferCurve — the group lowered once per structural revision (the app's
// compile's lowering), its curve evaluated (kPoints = 128 points, a few kernel calls each) only when its stamp moves (a
// control it reads changed); the element re-derives its dB points only when that evaluation is new. Backdrop and rules
// are static layers (tiles); the curve, the diagonal and the dot are drawn every paint.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "elements_internal.h"
#include "pq/views/panel/group_responses.h"
#include "pq/views/panel/transfer_display.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

constexpr std::array<std::string_view, 1> kSlots = {"signal"};
constexpr uint32_t kPoints = 128;
constexpr float kPad = 4.0f;

std::string dbLabel(double db) {
  char b[16];
  std::snprintf(b, sizeof b, db > 0.0 ? "+%.0f" : "%.0f", db);
  return b;
}

class TransferDisplay final : public Element {
public:
  using Element::Element;
  static constexpr std::string_view kUnits[] = {"linear", "db"};
  static constexpr std::string_view kOutputs[] = {"value", "reduction"};
  static constexpr StyleSpec kStyle[] = {
      {"min", StyleSpec::Type::Number, -60.0, {}, {}, -120.0, -6.0},
      {"max", StyleSpec::Type::Number, 6.0, {}, {}, -24.0, 24.0},
      {"units", StyleSpec::Type::Choice, 0, "linear", kUnits},
      {"output", StyleSpec::Type::Choice, 0, "value", kOutputs},
      {"dot", StyleSpec::Type::Number, 1.0, {}, {}, 0.0, 1.0},
  };

  // ---- static layers: the backdrop, the rules ----
  uint32_t staticLayerCount() const noexcept override { return 2; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.device.display : th.device.displayGrid;
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba white = ui::rgba(255, 255, 255);
    if (layer == 0) {
      p.roundedRect({0.0f, 0.0f, desc_.w, desc_.h}, 4.0f, white);
      return;
    }
    const Rect r = plot();
    const double lo = minDb(), hi = maxDb(), step = stepDb();
    for (double db = std::ceil(lo / step) * step; db <= hi + 1e-9; db += step) {
      if (std::fabs(db) < 1e-9) continue;  // (0 dB: brighter, live)
      const float x = xOf(db), y = yOf(db);
      p.line({x, r.y0}, {x, r.y1}, 0.6f, white);
      p.line({r.x0, y}, {r.x1, y}, 0.6f, white);
    }
  }

  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    if (env.target != nullptr) {
      if (dotOn()) window_.acquire(*env.target, binding());
      refresh(*env.target);
    }
    const Rect r = plot();
    const double lo = minDb(), hi = maxDb();
    // 0 dB, brighter.
    if (lo < 0.0 && hi > 0.0) {
      const Rgba major = th.device.displayGridMajor;
      p.line({xOf(0.0), r.y0}, {xOf(0.0), r.y1}, 0.8f, major);
      p.line({r.x0, yOf(0.0)}, {r.x1, yOf(0.0)}, 0.8f, major);
    }
    // The 1:1 diagonal.
    const Vec2 diag[2] = {{xOf(lo), yOf(lo)}, {xOf(hi), yOf(hi)}};
    p.dashed(diag, 0.8f, 3.0f, 2.5f, th.device.displayGridMajor);
    // dB labels: along the left edge and the bottom, every other rule.
    const double step = stepDb() * 2.0;
    for (double db = std::ceil(lo / step) * step; db <= hi + 1e-9; db += step) {
      if (db <= lo + 1e-9) continue;
      p.textAligned({r.x0 + 1.5f, yOf(db) - 2.5f}, dbLabel(db), 5.0f, th.device.displayText, 0.0f);
      if (db < hi - 1e-9) p.textAligned({xOf(db), r.y1 - 2.5f}, dbLabel(db), 5.0f, th.device.displayText, 0.5f);
    }
    ui::DrawList& dl = p.drawList();
    const ui::FaceTransform& ft = p.transform();
    dl.pushClip({ft.origin.x + r.x0 * ft.scale, ft.origin.y + r.y0 * ft.scale, ft.origin.x + r.x1 * ft.scale,
                 ft.origin.y + r.y1 * ft.scale});
    if (curveValid_) {
      const Rgba c = th.device.curve;
      p.polyline(pts_, 5.0f, ui::mulAlpha(c, 0.07f));
      p.polyline(pts_, 2.8f, ui::mulAlpha(c, 0.16f));
      p.polyline(pts_, 1.4f, c);
      if (const std::optional<double> lv = liveLevel(); lv && *lv > lo && dotOn()) {
        const double out = outAt(*lv);
        if (std::isfinite(out)) {
          const Vec2 d{xOf(*lv), yOf(out)};
          // (light on the curve's colour: it reads apart from the line it sits on)
          p.glow(d, 8.0f, ui::mulAlpha(c, 0.45f));
          p.disc(d, 3.0f, th.face.ink);
          p.ring(d, 3.0f, 1.0f, c);
        }
      }
    } else {
      p.text({(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f}, "no static curve", 6.0f, th.device.displayText);
    }
    dl.popClip();
  }

  VoiceTapReader* voiceReader(PanelTarget& t) const override { return dotOn() ? window_.reader(t, binding()) : nullptr; }
  bool pollVoices(PanelTarget& t) override {
    if (!dotOn() || !window_.poll(t, binding())) return false;
    float best = -std::numeric_limits<float>::infinity();
    const bool db = unitsDb();
    for (int c = 0; c < 2; ++c)
      for (const float v : window_.channel(c)) best = std::max(best, db ? v : std::fabs(v));  // (a NaN is skipped)
    const double level = db ? static_cast<double>(best) : 20.0 * std::log10(std::max(static_cast<double>(best), 1e-9));
    if (level == level_) return false;
    level_ = level;
    return true;
  }
  bool animating() const noexcept override { return dotOn() && window_.live(); }

  // ---- tests (pq/views/panel/transfer_display.h) ----
  void showLevel(double db) noexcept { level_ = db; }
  std::optional<double> drawnAt(double inDb) const { return curveValid_ ? outAt(inDb) : std::numeric_limits<double>::quiet_NaN(); }
  bool curveValid() const noexcept { return curveValid_; }

private:
  double minDb() const noexcept { return std::clamp(num("min", -60.0), -120.0, -6.0); }
  double maxDb() const noexcept { return std::max(std::clamp(num("max", 6.0), -24.0, 24.0), minDb() + 6.0); }
  double stepDb() const noexcept { return maxDb() - minDb() > 48.0 + 1e-9 ? 12.0 : 6.0; }
  bool unitsDb() const noexcept { return txt("units", "linear") == "db"; }
  bool reductionOut() const noexcept { return txt("output", "value") == "reduction"; }
  bool dotOn() const noexcept { return num("dot", 1.0) >= 0.5; }
  Rect plot() const noexcept { return {kPad, kPad, desc_.w - kPad, desc_.h - kPad}; }
  float xOf(double db) const noexcept {
    const Rect r = plot();
    return r.x0 + static_cast<float>((db - minDb()) / (maxDb() - minDb())) * r.width();
  }
  float yOf(double db) const noexcept {
    const Rect r = plot();
    return r.y1 - static_cast<float>((db - minDb()) / (maxDb() - minDb())) * r.height();
  }
  double inDbAt(uint32_t k) const noexcept { return minDb() + (maxDb() - minDb()) * static_cast<double>(k) / (kPoints - 1); }
  // The output dB drawn at input dB `x` (linear between the grid points).
  double outAt(double x) const noexcept {
    const double u = std::clamp((x - minDb()) / (maxDb() - minDb()) * (kPoints - 1), 0.0, static_cast<double>(kPoints - 1));
    const uint32_t k = std::min<uint32_t>(static_cast<uint32_t>(u), kPoints - 2);
    const double f = u - k;
    return outDb_[k] + (outDb_[k + 1] - outDb_[k]) * f;
  }
  std::optional<double> liveLevel() const noexcept {
    if (!std::isfinite(level_)) return std::nullopt;
    return level_;
  }
  // The curve from the group's transfer (re-derived only when its evaluation is new).
  void refresh(PanelTarget& t) {
    GroupResponses* r = t.responses();
    const doc::Binding* b = binding();
    const GroupResponses::TransferCurve* c =
        r != nullptr && b != nullptr && b->device.valid() ? r->transferCurve(b->device, grid(), gridKey()) : nullptr;
    if (c == nullptr || c->kind == graph::ResponseKind::Unavailable) {
      curveValid_ = false;
      version_ = 0;
      return;
    }
    if (c->version == version_) return;
    version_ = c->version;
    const bool reduction = reductionOut(), db = unitsDb();
    outDb_.resize(kPoints);
    pts_.clear();
    bool finite = true;
    for (uint32_t k = 0; k < kPoints; ++k) {
      const double x = inDbAt(k), y = c->y[static_cast<size_t>(k) * c->out];
      const double out = reduction ? x - y : db ? y : 20.0 * std::log10(std::max(std::fabs(y), 1e-9));
      finite = finite && std::isfinite(out);
      outDb_[k] = out;
      // (far outside the plot: held just past its edge, so the clipped line keeps its slope into the edge)
      const double span = maxDb() - minDb();
      pts_.push_back({xOf(x), yOf(std::clamp(out, minDb() - span, maxDb() + span))});
    }
    curveValid_ = finite;
  }
  // The In values the curve is evaluated at: kPoints uniform in input dB (as amplitudes for units linear).
  const std::vector<float>& grid() {
    if (xs_.empty()) {
      const bool db = unitsDb();
      xs_.resize(kPoints);
      for (uint32_t k = 0; k < kPoints; ++k) {
        const double x = inDbAt(k);
        xs_[k] = static_cast<float>(db ? x : std::pow(10.0, x / 20.0));
      }
    }
    return xs_;
  }
  // The grid's name for GroupResponses' cache (its range and units).
  uint64_t gridKey() const noexcept {
    const auto q = [](double v) { return static_cast<uint64_t>(static_cast<int64_t>(std::lround(v * 16.0)) & 0xFFFF); };
    return 0x5452000000000000ull | q(minDb()) << 24 | q(maxDb()) << 8 | (unitsDb() ? 1u : 0u);
  }

  SignalWindow window_;
  std::vector<float> xs_;
  std::vector<double> outDb_;
  std::vector<Vec2> pts_;
  uint64_t version_ = 0;
  bool curveValid_ = false;
  double level_ = std::numeric_limits<double>::quiet_NaN();
};

std::unique_ptr<Element> makeTransfer(const doc::PanelElement& d) { return std::make_unique<TransferDisplay>(d); }

}  // namespace

void registerTransferElement(void (*reg)(std::string_view, std::string_view, std::span<const StyleSpec>, std::span<const std::string_view>,
                                         float, float, ElementFactory)) {
  reg("transfer", "Transfer Curve", TransferDisplay::kStyle, kSlots, 120, 120, &makeTransfer);
}

namespace test {

bool showTransferLevel(Element& e, double db) {
  auto* t = dynamic_cast<TransferDisplay*>(&e);
  if (t == nullptr) return false;
  t->showLevel(db);
  return true;
}

std::optional<double> transferDrawn(const Element& e, double inDb) {
  const auto* t = dynamic_cast<const TransferDisplay*>(&e);
  if (t == nullptr) return std::nullopt;
  return t->drawnAt(inDb);
}

}  // namespace test

}  // namespace pq::views::panel
