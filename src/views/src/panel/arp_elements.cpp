// AP.6 (docs/plans/AP.md "AP.6"; pq/document/note_devices.h noteDevicePanel): the element kinds the Arpeggiator's panel
// adds to the panel system. Each is general — bound to params, settings and a note device's status by its bindings and
// style, never to the arpeggiator by name:
//
//   presets     chips that set SEVERAL bindings at once: item k sets slot s to the k-th group of `values` (in the
//               bindings' units, "1 1|2 3|3 2": a tuplet's n and m — Straight, Dotted, Triplet); one undo step. The
//               item whose values all bindings have now is lit (none: a tuplet typed in by hand).
//   steplane    a STEP-SEQUENCER ROW: slot 0 its length (1 … the steps bound), slot 1 its on / off switch (off: dimmed),
//               slots 2 … its steps (controls). Look `bars` (the default: a bar per step, its height the value), `bipolar`
//               (bars from the middle: a transpose lane, the value written in the cell) or `cells` (a 3-state cell per
//               step: Play / Skip / Tie). A press on a step sets it from the pointer's height and a drag PAINTS across
//               steps (cells: the state the first press gave) — one store gesture, one undo step, param writes only
//               (runtime params: no compile); double-click or Alt+press: the default. The LENGTH HANDLE, a grip after
//               the last step in play, drags the length (a setting: committed once at the end); steps past it dim.
//               While its device's note stage plays (PanelTarget::noteStatus), the lane's current step is lit (`lane`:
//               which of the status's lanes).
//   arppattern  the LIVE PATTERN of a note device (slot 0: a Data binding naming it): its status's pattern list — the
//               held notes in the order the pattern plays them, as bars at their pitch — with the current position lit
//               (Chord Trigger: all), the number held; nothing held: a hint.
//   rampshape   the drawn SHAPE of a glide (slot 0 the shape: Linear / Exponential / S-curve / Overshoot, slot 1 the
//               bend −1 … 1, slot 2 the side's source: 0 = off, dimmed): `side` into (the shape, then the note) or out
//               (the note, then the shape back). The formulas of pq/engine/note_events.h "rampShape" (bend → rampBend
//               = round(127 · bend); Exponential k = rampBend / 16, Overshoot a = 4 · |rampBend| / 127).
//
// Cost: the status is read only by an element that is painted (a fold that hides it reads nothing): per host frame tick
// while the transport plays or the stage holds notes, a few relaxed loads; the pattern list is copied only when the
// status's serial moved. UI thread only.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "elements_internal.h"
#include "pq/core/note_stage_status.h"
#include "pq/views/panel/arp_elements.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

constexpr float kLabelPx = 7.5f;
constexpr std::array<std::string_view, 1> kDataSlot = {"device"};
constexpr std::array<std::string_view, 2> kPresetSlots = {"value 1", "value 2"};
constexpr std::array<std::string_view, 3> kLaneSlots = {"length", "on", "steps"};
constexpr std::array<std::string_view, 3> kShapeSlots = {"shape", "bend", "source"};
constexpr std::array<std::string_view, 3> kLaneLooks = {"bars", "bipolar", "cells"};
constexpr std::array<std::string_view, 2> kSides = {"into", "out"};

uint64_t hmix(uint64_t h, uint64_t v) noexcept {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h;
}

// A binding's value in its units at its position now.
double unitsOf(const PanelTarget& t, const doc::Binding& b) {
  const BoundInfo& i = t.info(b);
  return i.valid ? dsp::evalCurveUnits(i.curve, std::clamp(t.position(b), 0.0, 1.0)) : 0.0;
}
// The position that gives `v` in the binding's units (nearest its current one).
double positionOf(const PanelTarget& t, const doc::Binding& b, double v) {
  const BoundInfo& i = t.info(b);
  return i.valid ? std::clamp(dsp::curvePosition(i.curve, dsp::curveOutput(i.curve, v), t.position(b)), 0.0, 1.0) : 0.0;
}

// The status of the note device a binding names (cached; asked again while there is none).
struct StatusRef {
  std::shared_ptr<const NoteStageStatus> s;
  const NoteStageStatus* get(const PanelTarget& t, const doc::Binding* b) {
    if (!s && b != nullptr && b->device.valid()) s = t.noteStatus(b->device);
    return s.get();
  }
};

// ---- presets --------------------------------------------------------------------------------------------------------

class Presets final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"items", StyleSpec::Type::Text, 0, "A|B"},
      {"values", StyleSpec::Type::Text, 0, ""},
      {"colour", StyleSpec::Type::Token, 0, "value"},
  };
  void paintGhost(ui::FacePainter&, const ElementEnv&) override {}
  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return ui::lerp(th.face.body, th.face.track, 0.6f); }
  void paintStatic(ui::FacePainter& p, uint32_t) const override {
    const size_t n = items().size();
    for (size_t k = 0; k < n; ++k) p.roundedRect(cell(k, n), 3.0f, ui::rgba(255, 255, 255));
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    const std::vector<std::string> it = items();
    const int32_t lit = current(*env.target);
    const Rgba accent = colour(th, "colour", "value", th.face.value);
    for (size_t k = 0; k < it.size(); ++k) {
      const Rect c = cell(k, it.size());
      const bool on = static_cast<int32_t>(k) == lit;
      if (on) p.roundedRect(c, 3.0f, accent);
      else if (env.hovered && static_cast<int32_t>(k) == hover_) outlineRect(p, c, 3.0f, 1.0f, th.face.hover);
      p.text({(c.x0 + c.x1) * 0.5f, (c.y0 + c.y1) * 0.5f}, it[k], kLabelPx, on ? th.face.body : th.face.ink);
    }
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    const size_t n = items().size();
    if (e.kind == FacePointer::Kind::Move) {
      const int32_t h = cellAt(e.local, n);
      const bool changed = h != hover_;
      hover_ = h;
      return changed;
    }
    if (e.kind != FacePointer::Kind::Down || e.button != ui::PointerButton::Left) return false;
    const int32_t k = cellAt(e.local, n);
    if (k < 0 || binding() == nullptr || !editable(t)) return k >= 0;
    const std::vector<double> v = values(static_cast<size_t>(k));
    t.begin(*binding(), "Panel Preset");  // every binding in one gesture: one undo step
    for (size_t s = 0; s < desc_.bindings.size() && s < v.size(); ++s)
      if (const doc::Binding* b = binding(s)) t.setPosition(*b, positionOf(t, *b, v[s]));
    t.end();
    return true;
  }
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    Element::describe(t, title, body);
    body = "Click a preset: it sets " + std::to_string(desc_.bindings.size()) + " values at once (one undo step).";
  }

private:
  std::vector<std::string> items() const {
    std::vector<std::string> v = splitItems(txt("items", "A|B"));
    if (v.empty()) v.emplace_back("A");
    return v;
  }
  // Item k's values ("1 1|2 3": item 1 is {2, 3}).
  std::vector<double> values(size_t k) const {
    std::vector<double> out;
    const std::vector<std::string> groups = splitItems(txt("values", ""));
    if (k >= groups.size()) return out;
    const char* s = groups[k].c_str();
    char* end = nullptr;
    for (double x = std::strtod(s, &end); end != s; x = std::strtod(s, &end)) {
      out.push_back(x);
      s = end;
    }
    return out;
  }
  int32_t current(const PanelTarget& t) const {
    const size_t n = items().size();
    for (size_t k = 0; k < n; ++k) {
      const std::vector<double> v = values(k);
      bool all = !v.empty();
      for (size_t s = 0; all && s < v.size(); ++s) {
        const doc::Binding* b = binding(s);
        all = b != nullptr && t.info(*b).valid && std::fabs(unitsOf(t, *b) - v[s]) < 1e-6;
      }
      if (all) return static_cast<int32_t>(k);
    }
    return -1;
  }
  Rect cell(size_t k, size_t n) const noexcept {
    const float gap = 2.0f, w = (desc_.w - gap * static_cast<float>(n - 1)) / static_cast<float>(n);
    const float x0 = static_cast<float>(k) * (w + gap);
    return {x0, 0.5f, x0 + w, desc_.h - 0.5f};
  }
  int32_t cellAt(Vec2 l, size_t n) const noexcept {
    for (size_t k = 0; k < n; ++k) {
      const Rect c = cell(k, n);
      if (l.x >= c.x0 && l.x <= c.x1 && l.y >= c.y0 && l.y <= c.y1) return static_cast<int32_t>(k);
    }
    return -1;
  }
  int32_t hover_ = -1;
};

// ---- steplane -------------------------------------------------------------------------------------------------------

class StepLane final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"look", StyleSpec::Type::Choice, 0, "bars", kLaneLooks},
      {"lane", StyleSpec::Type::Number, -1.0, {}, {}, -1.0, 7.0},
      {"colour", StyleSpec::Type::Token, 0, "value"},
  };
  static constexpr size_t kFirstStep = 2;
  static constexpr float kHandleW = 10.0f;

  void paintGhost(ui::FacePainter&, const ElementEnv&) override {}
  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return ui::lerp(th.face.body, th.face.track, 0.75f); }
  void paintStatic(ui::FacePainter& p, uint32_t) const override {
    for (int32_t i = 0; i < steps(); ++i) p.roundedRect(cellRect(i), 2.0f, ui::rgba(255, 255, 255));
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const int32_t n = steps(), len = length(t);
    const bool on = laneOn(t);
    const Rgba accent = colour(th, "colour", "value", th.face.value);
    const std::string_view look = txt("look", "bars");
    if (const NoteStageStatus* st = status_.get(t, binding(0))) readStatus(*st);
    for (int32_t i = 0; i < n; ++i) {
      const Rect c = cellRect(i);
      const doc::Binding* b = binding(kFirstStep + static_cast<size_t>(i));
      if (b == nullptr) continue;
      const double v = std::clamp(t.position(*b), 0.0, 1.0);
      float a = i < len ? 1.0f : 0.28f;  // past the length: dimmed
      if (!on) a *= 0.45f;
      const Rgba fill = ui::mulAlpha(accent, a);
      if (look == "cells") {
        const int32_t k = kindAt(t, i);
        if (k == 1) {  // Skip: a rest
          p.line({c.x0 + 4.0f, (c.y0 + c.y1) * 0.5f}, {c.x1 - 4.0f, (c.y0 + c.y1) * 0.5f}, 1.4f, ui::mulAlpha(th.face.ink, a));
        } else if (k == 2) {  // Tie: joined to the step before
          p.roundedRect({c.x0 - 2.5f, c.y0 + 6.0f, c.x1, c.y1 - 6.0f}, 2.0f, fill);
        } else {  // Play
          p.disc({(c.x0 + c.x1) * 0.5f, (c.y0 + c.y1) * 0.5f}, 2.6f, fill);
        }
      } else if (look == "bipolar") {
        const float mid = (c.y0 + c.y1) * 0.5f, y = c.y1 - static_cast<float>(v) * (c.y1 - c.y0);
        if (std::fabs(y - mid) > 0.3f) p.roundedRect({c.x0 + 1.0f, std::min(y, mid), c.x1 - 1.0f, std::max(y, mid)}, 1.0f, fill);
        p.line({c.x0 + 1.0f, mid}, {c.x1 - 1.0f, mid}, 0.8f, ui::mulAlpha(th.face.ink, 0.5f * a));
        const double u = unitsOf(t, *b);
        if (std::fabs(u) > 0.5) {
          char s[8];
          std::snprintf(s, sizeof s, "%+d", static_cast<int>(std::lround(u)));
          p.text({(c.x0 + c.x1) * 0.5f, u > 0 ? c.y1 - 4.5f : c.y0 + 4.5f}, s, 5.5f, ui::mulAlpha(th.face.ink, a));
        }
      } else {
        const float y = c.y1 - static_cast<float>(v) * (c.y1 - c.y0);
        if (c.y1 - y > 0.3f) p.roundedRect({c.x0 + 1.0f, y, c.x1 - 1.0f, c.y1}, 1.0f, fill);
      }
      if (i == lit_ && on) {  // the step playing now
        outlineRect(p, {c.x0 - 0.5f, c.y0 - 0.5f, c.x1 + 0.5f, c.y1 + 0.5f}, 2.5f, 1.3f, th.face.signal);
        p.beginGlow();
        p.glow({(c.x0 + c.x1) * 0.5f, (c.y0 + c.y1) * 0.5f}, 9.0f, ui::mulAlpha(th.face.signal, 0.25f));
        p.endGlow();
      }
    }
    // The length handle: a grip after the last step in play.
    const float hx = handleX(len);
    const bool hot = lengthDrag_ || hoverHandle_;
    p.roundedRect({hx - 2.0f, 1.0f, hx + 2.0f, desc_.h - 1.0f}, 1.5f, hot ? th.face.value : th.face.handle);
    if (hot || lengthDrag_) {
      char s[8];
      std::snprintf(s, sizeof s, "%d", len);
      p.textAligned({hx + 4.0f, desc_.h * 0.5f}, s, 6.5f, th.face.ink, 0.0f);
    }
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    if (e.kind == K::Move && !drag_) {
      const bool h = nearHandle(t, e.local.x);
      const bool changed = h != hoverHandle_;
      hoverHandle_ = h;
      return changed;
    }
    if (e.kind == K::Leave) {
      const bool changed = hoverHandle_;
      hoverHandle_ = false;
      return changed;
    }
    if (e.kind == K::Down) {
      if (e.button != ui::PointerButton::Left) return false;
      if (nearHandle(t, e.local.x)) {  // the length
        const doc::Binding* lb = binding(0);
        if (lb == nullptr || !t.info(*lb).valid) return true;
        if (e.clicks >= 2) {
          t.reset(*lb);
          return true;
        }
        drag_ = lengthDrag_ = true;
        t.begin(*lb, "Step Lane Length");
        return true;
      }
      const int32_t i = stepAt(e.local.x);
      const doc::Binding* b = i >= 0 ? binding(kFirstStep + static_cast<size_t>(i)) : nullptr;
      if (b == nullptr || !t.info(*b).valid) return false;
      if (e.clicks >= 2) {  // the default (one step)
        t.reset(*b);
        return true;
      }
      drag_ = true;
      last_ = i;
      reset_ = (e.mods & ui::mod::kAlt) != 0;
      cellValue_ = -1;
      if (txt("look", "bars") == "cells" && !reset_) cellValue_ = (kindAt(t, i) + 1) % 3;
      t.begin(*b, "Step Lane");
      paintStep(t, i, e.local.y);
      return true;
    }
    if (e.kind == K::Move && drag_) {
      if (lengthDrag_) {
        const doc::Binding* lb = binding(0);
        const int32_t len = std::clamp(static_cast<int32_t>(std::lround(e.local.x / cellW())), 1, steps());
        t.setPosition(*lb, positionOf(t, *lb, len));
        return true;
      }
      const int32_t i = std::clamp(static_cast<int32_t>(std::floor(e.local.x / cellW())), 0, steps() - 1);
      // Paint every step the pointer crossed since the last move (a fast drag skips none).
      const int32_t from = std::min(last_, i), to = std::max(last_, i);
      for (int32_t k = from; k <= to; ++k) paintStep(t, k, e.local.y);
      last_ = i;
      return true;
    }
    if (e.kind == K::Up && drag_) {
      drag_ = lengthDrag_ = false;
      t.end();
      return true;
    }
    return false;
  }
  void cancelDrag(PanelTarget& t) override {
    if (!drag_) return;
    drag_ = lengthDrag_ = false;
    t.cancel();
  }
  ui::Cursor cursor() const noexcept override { return hoverHandle_ || lengthDrag_ ? ui::Cursor::ResizeEW : ui::Cursor::Pointer; }
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    const doc::Binding* on = binding(1);
    title = on != nullptr ? t.info(*on).label : std::string("Step lane");
    body = txt("look", "bars") == "cells"
               ? "Click a step: Play, Skip (a rest), Tie (continues the step before); drag to paint that state across steps."
               : "Drag a step up / down to set it; drag sideways to paint across steps.";
    body += " Double-click or Alt+click: the default. Drag the grip after the last step: the lane's length (steps past it "
            "do not play). The lit step is the one playing.";
  }
  void visitControls(const ControlVisitor& fn) const override {
    for (int32_t i = 0; i < steps(); ++i)
      if (const doc::Binding* b = binding(kFirstStep + static_cast<size_t>(i)); b != nullptr && b->kind == doc::BindingKind::Control)
        fn(*b, cellRect(i));
  }
  uint64_t liveKey(const PanelTarget& t) const override {
    uint64_t h = Element::liveKey(t);
    if (h == kAnyChange) return h;
    h = hmix(h, static_cast<uint64_t>(lit_ + 1));
    h = hmix(h, (hoverHandle_ ? 1u : 0u) | (lengthDrag_ ? 2u : 0u));
    return h == kAnyChange ? h - 1 : h;
  }
  // The status: the lane's current step (a change repaints), ticking while the transport plays or notes are held.
  bool frameTick(PanelTarget& t) override {
    playing_ = t.transportPlaying();
    const NoteStageStatus* st = status_.get(t, binding(0));
    if (st == nullptr) return false;
    const int32_t was = lit_;
    readStatus(*st);
    return lit_ != was;
  }
  bool ticking() const noexcept override { return status_.s != nullptr && (playing_ || lit_ >= 0); }
  int32_t litStep() const noexcept { return lit_; }

private:
  int32_t steps() const noexcept {
    return static_cast<int32_t>(desc_.bindings.size() > kFirstStep ? desc_.bindings.size() - kFirstStep : 0);
  }
  float cellW() const noexcept { return (desc_.w - kHandleW) / static_cast<float>(std::max(steps(), 1)); }
  Rect cellRect(int32_t i) const noexcept {
    const float w = cellW(), x0 = static_cast<float>(i) * w;
    return {x0 + 1.0f, 1.0f, x0 + w - 1.0f, desc_.h - 1.0f};
  }
  float handleX(int32_t len) const noexcept { return static_cast<float>(len) * cellW(); }
  int32_t stepAt(float x) const noexcept {
    if (x < 0.0f) return -1;
    const auto i = static_cast<int32_t>(std::floor(x / cellW()));
    return i < steps() ? i : -1;
  }
  bool nearHandle(const PanelTarget& t, float x) const { return std::fabs(x - handleX(length(t))) <= 4.0f; }
  int32_t length(const PanelTarget& t) const {
    const doc::Binding* b = binding(0);
    const int32_t n = steps();
    if (b == nullptr || !t.info(*b).valid) return n;
    return std::clamp(static_cast<int32_t>(std::lround(unitsOf(t, *b))), 1, std::max(n, 1));
  }
  bool laneOn(const PanelTarget& t) const {
    const doc::Binding* b = binding(1);
    return b == nullptr || !t.info(*b).valid || t.position(*b) >= 0.5;
  }
  int32_t kindAt(const PanelTarget& t, int32_t i) const {
    const doc::Binding* b = binding(kFirstStep + static_cast<size_t>(i));
    return b != nullptr ? std::clamp(static_cast<int32_t>(std::lround(unitsOf(t, *b))), 0, 2) : 0;
  }
  // Step i from the pointer's height `y` (cells: the state of the press; Alt: the default).
  void paintStep(PanelTarget& t, int32_t i, float y) {
    const doc::Binding* b = binding(kFirstStep + static_cast<size_t>(i));
    if (b == nullptr || !t.info(*b).valid) return;
    if (reset_) {  // the declared default (inside the gesture: the same undo step)
      t.reset(*b);
      return;
    }
    double pos;
    if (cellValue_ >= 0) {
      pos = positionOf(t, *b, cellValue_);
    } else {
      const Rect c = cellRect(i);
      pos = std::clamp(static_cast<double>((c.y1 - y) / (c.y1 - c.y0)), 0.0, 1.0);
      const BoundInfo& info = t.info(*b);
      if (info.steps >= 2) pos = std::round(pos * (info.steps - 1)) / (info.steps - 1);
    }
    t.setPosition(*b, pos);
  }
  void readStatus(const NoteStageStatus& st) {
    const auto l = static_cast<int32_t>(std::lround(num("lane", -1.0)));
    if (l < 0 || l >= static_cast<int32_t>(NoteStageStatus::kLanes)) {
      lit_ = -1;
      return;
    }
    lit_ = st.lane[l].load(std::memory_order_relaxed);
  }

  StatusRef status_;
  int32_t lit_ = -1, last_ = -1, cellValue_ = -1;
  bool lengthDrag_ = false, hoverHandle_ = false, reset_ = false, playing_ = false;
};

// ---- arppattern -----------------------------------------------------------------------------------------------------

class PatternView final : public Element {
public:
  using Element::Element;
  static constexpr StyleSpec kStyle[] = {
      {"colour", StyleSpec::Type::Token, 0, "value"},
  };
  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return th.device.display; }
  void paintStatic(ui::FacePainter& p, uint32_t) const override {
    p.roundedRect({0.0f, 0.0f, desc_.w, desc_.h}, 4.0f, ui::rgba(255, 255, 255));
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    if (env.target != nullptr)
      if (const NoteStageStatus* st = status_.get(*env.target, binding())) (void)read(*st);
    const uint32_t n = pattern_.count;
    paintRollChip(p, env);  // AP.7
    if (n == 0) {
      p.text({desc_.w * 0.5f, desc_.h * 0.5f - 5.0f}, "Hold notes", 7.5f, th.device.displayText);
      p.text({desc_.w * 0.5f, desc_.h * 0.5f + 6.0f}, "to see the pattern", 6.5f, th.device.displayText);
      return;
    }
    // Pitch (log Hz) up, play order across; the range of the notes held, padded.
    float lo = 1e30f, hi = -1e30f;
    for (uint32_t i = 0; i < n; ++i)
      if (pattern_.hz[i] > 0.0f) {
        const float c = std::log2(pattern_.hz[i]);
        lo = std::min(lo, c);
        hi = std::max(hi, c);
      }
    if (lo > hi) lo = hi = 0.0f;
    const float span = std::max(hi - lo, 0.25f), mid = 0.5f * (lo + hi);
    const Rect r{6.0f, 8.0f, desc_.w - 6.0f, desc_.h - 16.0f};
    const float w = (r.x1 - r.x0) / static_cast<float>(n), bh = std::clamp((r.y1 - r.y0) / 12.0f, 2.0f, 6.0f);
    const Rgba accent = colour(th, "colour", "value", th.face.value);
    for (uint32_t i = 0; i < n; ++i) {
      if (!(pattern_.hz[i] > 0.0f)) continue;
      const float u = (std::log2(pattern_.hz[i]) - mid) / span + 0.5f;
      const float y = r.y1 - bh * 0.5f - u * (r.y1 - r.y0 - bh);
      const float x0 = r.x0 + static_cast<float>(i) * w;
      const Rect bar{x0 + std::min(1.0f, w * 0.15f), y - bh * 0.5f, x0 + w - std::min(1.0f, w * 0.15f), y + bh * 0.5f};
      const bool lit = position_ >= 0 && (pattern_.chord || static_cast<uint32_t>(position_) == i);
      if (lit) {
        p.beginGlow();
        p.glow({(bar.x0 + bar.x1) * 0.5f, y}, std::max(6.0f, bh * 2.0f), ui::mulAlpha(accent, 0.35f));
        p.endGlow();
        p.roundedRect(bar, 1.5f, accent);
      } else {
        p.roundedRect(bar, 1.5f, ui::mulAlpha(th.device.curve, 0.55f));
      }
    }
    char s[48];
    std::snprintf(s, sizeof s, "%u held \xC2\xB7 %u step%s", pattern_.held, n, n == 1 ? "" : "s");
    p.textAligned({4.0f, desc_.h - 6.0f}, s, 6.0f, th.device.displayText, 0.0f);
  }
  // AP.7: the ROLL chip (top right): the roll preview's switch (ViewContext::notePreview, the clip box's Ghosts).
  Rect rollChip() const noexcept { return {desc_.w - 34.0f, 3.0f, desc_.w - 4.0f, 13.0f}; }
  void paintRollChip(ui::FacePainter& p, const ElementEnv& env) const {
    if (env.target == nullptr) return;
    const std::optional<bool> on = env.target->notePreview();
    if (!on) return;
    const ui::Theme& th = *env.theme;
    const Rect c = rollChip();
    const Rgba accent = colour(th, "colour", "value", th.face.value);
    if (*on) p.roundedRect(c, 2.5f, accent);
    else outlineRect(p, c, 2.5f, 1.0f, hoverChip_ ? th.face.hover : th.device.displayText);
    p.text({(c.x0 + c.x1) * 0.5f, (c.y0 + c.y1) * 0.5f}, "ROLL", 6.0f, *on ? th.face.body : th.device.displayText);
  }
  uint64_t liveKey(const PanelTarget& t) const override {
    const uint64_t roll = t.notePreview().value_or(false) ? 2u : 1u;  // AP.7: the ROLL chip
    return hmix(hmix(hmix(0x5041544Eull, serial_), static_cast<uint64_t>(position_ + 1)), roll + (hoverChip_ ? 4u : 0u));
  }
  bool frameTick(PanelTarget& t) override {
    playing_ = t.transportPlaying();
    const NoteStageStatus* st = status_.get(t, binding());
    return st != nullptr && read(*st);
  }
  bool ticking() const noexcept override { return status_.s != nullptr && (playing_ || pattern_.held > 0); }
  void describe(const PanelTarget&, std::string& title, std::string& body) const override {
    title = "Pattern";
    body = "The held notes in the order the pattern plays them (pitch up, time across); the lit one is playing. "
           "ROLL (top right) shows what the arpeggiator plays from the clip's notes as ghost notes in the piano roll.";
  }
  // AP.7: only the ROLL chip takes presses (the rest is a display).
  bool hit(Vec2 l) const noexcept override {
    const Rect c = rollChip();
    return l.x >= c.x0 && l.x <= c.x1 && l.y >= c.y0 && l.y <= c.y1;
  }
  bool interactive() const noexcept override { return true; }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    const Rect c = rollChip();
    const bool in = e.local.x >= c.x0 && e.local.x <= c.x1 && e.local.y >= c.y0 && e.local.y <= c.y1;
    if (e.kind == FacePointer::Kind::Move || e.kind == FacePointer::Kind::Leave) {
      const bool h = in && e.kind == FacePointer::Kind::Move;
      const bool changed = h != hoverChip_;
      hoverChip_ = h;
      return changed;
    }
    if (e.kind != FacePointer::Kind::Down || e.button != ui::PointerButton::Left || !in) return false;
    if (const std::optional<bool> on = t.notePreview()) t.setNotePreview(!*on);
    return true;
  }
  // tests
  const NoteStageStatus::Pattern& pattern() const noexcept { return pattern_; }
  int32_t position() const noexcept { return position_; }

private:
  // A new status (its serial moved): the position, and the list when its seqlock read is consistent.
  bool read(const NoteStageStatus& st) {
    const uint64_t s = st.serial.load(std::memory_order_acquire);
    if (s == serial_) return false;
    serial_ = s;
    const int32_t pos = st.position.load(std::memory_order_relaxed);
    NoteStageStatus::Pattern p;
    const bool got = st.readPattern(p);
    if (got) pattern_ = p;
    else serial_ = 0;  // (a write was in progress: read again next tick)
    position_ = pattern_.count == 0 ? -1 : pos;
    return true;
  }
  StatusRef status_;
  NoteStageStatus::Pattern pattern_;
  uint64_t serial_ = 0;
  int32_t position_ = -1;
  bool playing_ = false;
  bool hoverChip_ = false;  // AP.7
};

// ---- rampshape ------------------------------------------------------------------------------------------------------

class RampShapeView final : public Element {
public:
  using Element::Element;
  static constexpr StyleSpec kStyle[] = {
      {"side", StyleSpec::Type::Choice, 0, "into", kSides},
      {"colour", StyleSpec::Type::Token, 0, "value"},
  };
  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return th.device.display; }
  void paintStatic(ui::FacePainter& p, uint32_t) const override {
    p.roundedRect({0.0f, 0.0f, desc_.w, desc_.h}, 3.0f, ui::rgba(255, 255, 255));
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    const PanelTarget& t = *env.target;
    const int32_t shape = slotUnits(t, 0, 0.0) < 0.5 ? 0 : static_cast<int32_t>(std::lround(slotUnits(t, 0, 0.0)));
    const double bend = slotUnits(t, 1, 0.5);
    const bool on = binding(2) == nullptr || slotUnits(t, 2, 1.0) >= 0.5;
    const bool into = txt("side", "into") == "into";
    // y: 0 at the source pitch, 1 at the target (an overshoot goes past it).
    const float x0 = 4.0f, x1 = desc_.w - 4.0f, y0 = desc_.h - 5.0f, y1 = 6.0f;
    auto at = [&](float u, double s) { return Vec2{x0 + u * (x1 - x0), y0 + static_cast<float>(s) * (y1 - y0)}; };
    constexpr int kN = 32;
    std::array<Vec2, kN + 2> pts;
    // Into: the glide over the first 60 %, then the note; Out: the note, then the glide over the last 60 % (drawn
    // falling back: the target below).
    for (int i = 0; i <= kN; ++i) {
      const float u = static_cast<float>(i) / kN;
      const double s = rampShape(shape, bend, u);
      pts[static_cast<size_t>(i)] = into ? at(u * 0.6f, s) : at(0.4f + u * 0.6f, 1.0 - s);
    }
    pts[kN + 1] = into ? at(1.0f, 1.0) : at(0.0f, 1.0);
    const Rgba c = on ? colour(th, "colour", "value", th.face.value) : ui::mulAlpha(th.device.displayText, 0.6f);
    const Vec2 ends[2] = {at(0.0f, 1.0), at(1.0f, 1.0)};
    p.dashed(ends, 0.6f, 2.0f, 2.0f, th.device.displayGridMajor);
    if (into) {
      p.polyline(std::span<const Vec2>(pts.data(), kN + 1), 1.4f, c);
      p.line(pts[kN], pts[kN + 1], 1.4f, c);
    } else {
      p.line(pts[kN + 1], pts[0], 1.4f, c);
      p.polyline(std::span<const Vec2>(pts.data(), kN + 1), 1.4f, c);
    }
  }
  bool hit(Vec2) const noexcept override { return false; }
  void describe(const PanelTarget&, std::string& title, std::string& body) const override {
    title = txt("side", "into") == "into" ? "Into glide" : "Out glide";
    body = "The glide's shape (Shape and Bend). Dimmed while this side is off.";
  }
  // pq/engine/note_events.h "rampShape": s(u) of shape `shape` with bend −1 … 1.
  static double rampShape(int32_t shape, double bend, double u) noexcept {
    const double rb = std::clamp(std::round(127.0 * std::clamp(bend, -1.0, 1.0)), -127.0, 127.0);
    switch (shape) {
      case 1: {  // Exponential
        const double k = rb / 16.0;
        if (std::fabs(k) < 1e-6) return u;
        return (1.0 - std::exp(-k * u)) / (1.0 - std::exp(-k));
      }
      case 2: return u * u * (3.0 - 2.0 * u);  // S-curve
      case 3: {                                 // Overshoot
        const double a = 4.0 * std::fabs(rb) / 127.0, v = u - 1.0;
        return 1.0 + (a + 1.0) * v * v * v + a * v * v;
      }
      default: return u;  // Linear (in cents)
    }
  }

private:
  double slotUnits(const PanelTarget& t, size_t slot, double def) const {
    const doc::Binding* b = binding(slot);
    return b != nullptr && t.info(*b).valid ? unitsOf(t, *b) : def;
  }
};

template <class T>
std::unique_ptr<Element> make(const doc::PanelElement& d) {
  return std::make_unique<T>(d);
}

}  // namespace

void registerArpElements(void (*reg)(std::string_view, std::string_view, std::span<const StyleSpec>, std::span<const std::string_view>,
                                     float, float, ElementFactory)) {
  reg("presets", "Presets", Presets::kStyle, kPresetSlots, 120, 16, &make<Presets>);
  reg("steplane", "Step Lane", StepLane::kStyle, kLaneSlots, 300, 22, &make<StepLane>);
  reg("arppattern", "Pattern View", PatternView::kStyle, kDataSlot, 160, 120, &make<PatternView>);
  reg("rampshape", "Glide Shape", RampShapeView::kStyle, kShapeSlots, 56, 28, &make<RampShapeView>);
}

namespace test {

int32_t stepLaneLit(const Element& e) {
  const auto* s = dynamic_cast<const StepLane*>(&e);
  return s != nullptr ? s->litStep() : -2;
}

int32_t patternPosition(const Element& e) {
  const auto* s = dynamic_cast<const PatternView*>(&e);
  return s != nullptr ? s->position() : -2;
}

uint32_t patternCount(const Element& e) {
  const auto* s = dynamic_cast<const PatternView*>(&e);
  return s != nullptr ? s->pattern().count : 0;
}

}  // namespace test

}  // namespace pq::views::panel
