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
//   arppattern  the RESOLVED PATTERN of a note device (slot 0: a Data binding naming it; AP.10): the whole cycle its
//               params make of the held notes (pq/document/note_pattern.h notePattern: the engine's own ArpCore run
//               offline), time across, log pitch up over the unit's grid; the held notes solid, the generated ones
//               (Distance / Steps copies, walk steps, ratio / harmonic steps, layers) translucent and dotted with their
//               identity; each note drawn along its pitch curve (its Into / Out glides). The held set comes from the
//               status (its held notes); nothing held: the last set it saw, else a nominal reference note. The note
//               playing lights (the status's position / copy: the next such note of the cycle). Recomputed only when its
//               input key (the device's values, the track, the held set, the tempo) moved.
//   arpbend     the BEND VIEW of a note device's glides (AP.10; doc::noteGlideView): one step large, source → Into →
//               hold → Out → target with the times / offsets dimensioned and the gate's end; handles drag the times,
//               the offsets and (Alt) the bends — one undo step per drag. The curves: doc::noteRampShape (the voice's).
//
// Cost: the status is read only by an element that is painted (a fold that hides it reads nothing): per host frame tick
// while the transport plays or the stage holds notes, a few relaxed loads; the pattern list is copied only when the
// status's serial moved. UI thread only.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "elements_internal.h"
#include "pq/core/note_stage_status.h"
#include "pq/document/note_devices.h"
#include "pq/document/note_pattern.h"
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
constexpr std::array<std::string_view, 3> kLaneLooks = {"bars", "bipolar", "cells"};

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
      if (!on) a *= 0.22f;  // (AP.10, review L13: an off lane reads as off)
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

// The device a Data binding names, its NotePattern recomputed only when what it is made of changed (AP.10).
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
    if (env.target != nullptr) {
      if (const NoteStageStatus* st = status_.get(*env.target, binding())) (void)read(*st, *env.target);
      ensure(*env.target);
    }
    paintRollChip(p, env);  // AP.7
    const Rgba accent = colour(th, "colour", "value", th.face.value);
    const Rgba ink = th.device.displayText;
    if (!pat_.valid || pat_.notes.empty()) {
      p.text({desc_.w * 0.5f, desc_.h * 0.5f}, "No pattern", 7.5f, ink);
      return;
    }
    const Rect r = plot();
    // y: log Hz over the pattern's range (at least a third of an octave), padded; x: the cycle.
    const float span = std::max(pat_.hi - pat_.lo, 0.34f) * 1.16f, mid = 0.5f * (pat_.lo + pat_.hi);
    auto Y = [&](float l2) { return r.y1 - ((l2 - (mid - 0.5f * span)) / span) * (r.y1 - r.y0); };
    const auto cyc = static_cast<float>(std::max(pat_.cycle, 1e-3));
    auto X = [&](float s) { return r.x0 + s / cyc * (r.x1 - r.x0); };
    // The unit's grid (equaves / octaves major) and the steps.
    for (const auto& [l2, major] : pat_.grid) {
      const float y = Y(l2);
      if (y < r.y0 - 1.0f || y > r.y1 + 1.0f) continue;
      p.line({r.x0, y}, {r.x1, y}, major ? 0.8f : 0.5f, major ? th.device.displayGridMajor : ui::mulAlpha(th.device.displayGrid, 0.7f));
    }
    if (pat_.cycle <= 64.0)
      for (int k = 0; k <= static_cast<int>(std::ceil(pat_.cycle)); ++k) {
        const float x = X(static_cast<float>(k));
        if (x > r.x1 + 0.5f) break;
        p.line({x, r.y0}, {x, r.y1}, 0.5f, ui::mulAlpha(th.device.displayGrid, k == 0 ? 0.9f : 0.45f));
      }
    // The notes: each its pitch curve (glides included). Sources solid, generated ones translucent + dotted.
    const bool live = held_ > 0;
    std::vector<Vec2> pts;
    struct Shown {
      const std::string* label;
      float x, y;
    };
    std::vector<Shown> shown;
    for (size_t i = 0; i < pat_.notes.size(); ++i) {
      const doc::NotePatternNote& n = pat_.notes[i];
      pts.clear();
      for (const auto& [t, l2] : n.curve) pts.push_back({std::min(X(t), r.x1), Y(l2)});
      if (pts.size() == 1) pts.push_back({pts[0].x + 1.0f, pts[0].y});
      const bool lit = live && lit_ >= 0 && n.on == pat_.notes[static_cast<size_t>(lit_)].on;
      if (lit) {
        p.beginGlow();
        p.glow({(pts.front().x + pts.back().x) * 0.5f, (pts.front().y + pts.back().y) * 0.5f}, 9.0f, ui::mulAlpha(accent, 0.4f));
        p.endGlow();
      }
      if (n.source) {
        p.polyline(pts, 3.0f, lit ? accent : ui::mulAlpha(accent, 0.8f));
      } else {
        p.polyline(pts, 3.0f, ui::mulAlpha(accent, lit ? 0.45f : 0.16f));
        p.dashed(pts, 1.0f, 2.2f, 1.8f, ui::mulAlpha(accent, lit ? 1.0f : 0.75f));
      }
      // Its identity in the unit, where it fits (a label narrower than the note).
      // (a repeat of the same label at the same height within ~24 px is left out: a pulse row reads once)
      if (!n.label.empty() && pts.back().x - pts.front().x >= 4.0f * static_cast<float>(n.label.size()) - 2.0f) {
        float top = pts.front().y;  // above the whole curve (a glide does not run through it)
        for (const Vec2& q : pts) top = std::min(top, q.y);
        bool repeat = false;
        for (const Shown& sh : shown)
          repeat = repeat || (*sh.label == n.label && std::fabs(sh.y - top) < 3.0f && pts.front().x - sh.x < 24.0f);
        if (!repeat) {
          p.textAligned({pts.front().x, top - 5.5f}, n.label, 5.5f, ui::mulAlpha(ink, n.source ? 1.0f : 0.8f), 0.0f);
          shown.push_back({&n.label, pts.front().x, top});
        }
      }
    }
    char s[96];
    const char* what = live ? "" : (pat_.nominal ? " \xC2\xB7 nominal note (hold notes)" : " \xC2\xB7 last held");
    std::snprintf(s, sizeof s, "%u held \xC2\xB7 %g step%s%s%s", live ? held_ : heldShown_, pat_.cycle,
                  pat_.cycle == 1.0 ? "" : "s", pat_.truncated ? " (first)" : "", what);
    p.textAligned({4.0f, desc_.h - 6.0f}, s, 6.0f, ink, 0.0f);
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
    uint64_t h = hmix(hmix(hmix(0x5041544Eull, serial_), static_cast<uint64_t>(lit_ + 1)), roll + (hoverChip_ ? 4u : 0u));
    h = hmix(h, inputKey(t));  // AP.10: the params / held set / tempo the pattern is made of
    return h == kAnyChange ? h - 1 : h;
  }
  bool frameTick(PanelTarget& t) override {
    playing_ = t.transportPlaying();
    const NoteStageStatus* st = status_.get(t, binding());
    return st != nullptr && read(*st, t);
  }
  bool ticking() const noexcept override { return status_.s != nullptr && (playing_ || held_ > 0); }
  void describe(const PanelTarget&, std::string& title, std::string& body) const override {
    title = "Pattern";
    body = "The whole cycle the arpeggiator plays from the held notes (pitch up, time across): the held notes solid, what "
           "Distance / Steps, the Pitch unit, Harmonic Walk and Layers make of them dotted with their identity (+k "
           "degrees, h11, 3/2), each note drawn as it bends (Into / Out glides). The lit one is playing. Nothing held: "
           "the last held notes, else a reference note. ROLL (top right) shows what the arpeggiator plays from the "
           "clip's notes as ghost notes in the piano roll.";
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
  const doc::NotePattern& resolved() const noexcept { return pat_; }
  int32_t litNote() const noexcept { return held_ > 0 ? lit_ : -1; }
  uint64_t computes() const noexcept { return computes_; }

private:
  Rect plot() const noexcept { return {6.0f, 16.0f, desc_.w - 6.0f, desc_.h - 14.0f}; }
  // What the pattern is drawn over: the held notes now, else the last ones seen (empty: the nominal note).
  const std::vector<doc::NotePatternHeld>& heldSet() const noexcept { return lastHeld_; }
  // A key of everything the pattern is computed from (cheap: one pass over the device's values).
  uint64_t inputKey(const PanelTarget& t) const {
    const doc::Binding* b = binding();
    const doc::Project* pr = t.project();
    if (b == nullptr || pr == nullptr) return 0;
    const doc::DeviceLoc loc = doc::findDevice(*pr, b->device);
    if (loc.device == nullptr) return 0;
    uint64_t h = hmix(0xA9u, reinterpret_cast<uintptr_t>(loc.track));  // (a track edit: its tuning, its devices)
    h = hmix(h, reinterpret_cast<uintptr_t>(loc.device));
    if (const doc::NoteDeviceType* type = doc::noteDeviceTypeOf(*loc.device))
      for (const doc::NoteParamSpec& s : type->params) {
        const double v = doc::noteDeviceValue(*loc.device, s);
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        h = hmix(h, bits);
      }
    for (const doc::NotePatternHeld& n : heldSet()) {
      uint32_t bits[3] = {};
      std::memcpy(&bits[0], &n.hz, 4);
      std::memcpy(&bits[1], &n.harmonic, 4);
      std::memcpy(&bits[2], &n.noteDegree, 4);
      h = hmix(hmix(hmix(h, bits[0]), bits[1]), bits[2]);
    }
    const double bpm = t.tempoBpm();
    uint64_t bb = 0;
    std::memcpy(&bb, &bpm, sizeof bb);
    return hmix(h, bb);
  }
  // Recomputes the pattern when its key moved (the real ArpCore offline: doc::notePattern).
  void ensure(const PanelTarget& t) {
    const uint64_t k = inputKey(t);
    if (k == key_ && computes_ > 0) return;
    key_ = k;
    ++computes_;
    const doc::Binding* b = binding();
    const doc::Project* pr = t.project();
    if (b == nullptr || pr == nullptr) {
      pat_ = {};
      return;
    }
    pat_ = doc::notePattern(*pr, b->device, heldSet(), t.tempoBpm());
    heldShown_ = static_cast<uint32_t>(heldSet().size());
    lit_ = -1;
  }
  // A new status (its serial moved): the held set, the position (the lit note: the next one in the cycle at that
  // position / copy), and the list when its seqlock read is consistent.
  bool read(const NoteStageStatus& st, const PanelTarget& t) {
    const uint64_t s = st.serial.load(std::memory_order_acquire);
    if (s == serial_) return false;
    serial_ = s;
    const int32_t pos = st.position.load(std::memory_order_relaxed);
    const int32_t cp = st.copy.load(std::memory_order_relaxed);
    const uint64_t steps = st.steps.load(std::memory_order_relaxed);
    NoteStageStatus::Pattern p;
    if (st.readPattern(p)) {
      pattern_ = p;
      held_ = p.held;
      if (p.heldCount > 0) {
        std::vector<doc::NotePatternHeld> h;
        for (uint32_t i = 0; i < p.heldCount; ++i) {
          const NoteStageStatus::HeldNote& n = p.heldNotes[i];
          h.push_back({n.hz, n.velocity, n.ratio, n.harmonic, n.degree, n.equave, n.tuning, n.noteDegree});
        }
        lastHeld_ = std::move(h);
      }
    } else {
      serial_ = 0;  // (a write was in progress: read again next tick)
    }
    position_ = pattern_.count == 0 ? -1 : pos;
    ensure(t);
    if (held_ == 0 || position_ < 0) {
      lit_ = -1;
    } else if (steps != steps_) {  // a step played: the next note of the cycle at its position / copy
      const auto n = static_cast<int32_t>(pat_.notes.size());
      int32_t found = -1;
      for (int32_t k = 1; k <= n && found < 0; ++k) {
        const int32_t i = ((lit_ < 0 ? -1 : lit_) + k + n) % n;
        const doc::NotePatternNote& nn = pat_.notes[static_cast<size_t>(i)];
        if (nn.position == position_ && nn.copy == cp && (lit_ < 0 || nn.on != pat_.notes[static_cast<size_t>(lit_)].on))
          found = i;
      }
      if (found >= 0) lit_ = found;
    }
    steps_ = steps;
    return true;
  }
  StatusRef status_;
  NoteStageStatus::Pattern pattern_;
  uint64_t serial_ = 0, steps_ = 0;
  int32_t position_ = -1;
  uint32_t held_ = 0, heldShown_ = 0;
  bool playing_ = false;
  bool hoverChip_ = false;  // AP.7
  // AP.10: the resolved pattern and what it was computed from.
  std::vector<doc::NotePatternHeld> lastHeld_;
  doc::NotePattern pat_;
  uint64_t key_ = 0, computes_ = 0;
  int32_t lit_ = -1;  // the playing note (an index into pat_.notes; its chord / layers light with it)
};

// ---- arpbend --------------------------------------------------------------------------------------------------------

// AP.10: the BEND VIEW of a note device's glides — one step drawn large (doc::noteGlideView: the device's values): the
// source (the previous step / an offset) → the Into curve → the hold → the Out curve → the target (the next step / an
// offset), with the times and offsets dimensioned and the gate's end marked. Handles drag a side's time (sideways: AP.11
// % of the note, 0 … 200 %; the curves add, so they may overlap and pass the note — drawn faint outside it), its offset (up / down, with source / target Offset: whole units, or cents) and,
// with Alt, its bend (up / down) — one undo step per drag, param writes only. The curves are the voice's (doc::
// noteRampShape). Slot 0 the device (Data), then the params in kBendSlots order.
constexpr std::array<std::string_view, 13> kBendSlots = {
    "device", "into", "into offset", "into time", "into shape", "into bend", "out", "out offset", "out time",
    "out shape", "out bend", "cents", "gate"};

class BendView final : public Element {
public:
  using Element::Element;
  static constexpr StyleSpec kStyle[] = {
      {"colour", StyleSpec::Type::Token, 0, "value"},
  };
  enum Slot : size_t {
    kDevice, kInto, kIntoOffset, kIntoTime, kIntoShape, kIntoBend, kOut, kOutOffset, kOutTime, kOutShape, kOutBend,
    kCents, kGate
  };
  enum class Handle : int8_t { None = -1, IntoTime, IntoOffset, OutTime, OutOffset };
  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return th.device.display; }
  void paintStatic(ui::FacePainter& p, uint32_t) const override {
    p.roundedRect({0.0f, 0.0f, desc_.w, desc_.h}, 4.0f, ui::rgba(255, 255, 255));
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    const Rgba accent = colour(th, "colour", "value", th.face.value), ink = th.device.displayText;
    if (env.target == nullptr || !geometry(*env.target)) {
      p.text({desc_.w * 0.5f, desc_.h * 0.5f}, "No glides", 7.5f, ink);
      return;
    }
    const G& g = g_;
    const Rect r = plot();
    // The step boundaries, the note's own pitch, the gate's end.
    p.line({X(0.0), r.y0}, {X(0.0), r.y1}, 0.6f, th.device.displayGridMajor);
    if (X(1.0) < r.x1) {
      const Vec2 nx[2] = {{X(1.0), r.y0}, {X(1.0), r.y1}};
      p.dashed(nx, 0.6f, 2.0f, 2.0f, th.device.displayGridMajor);
      if (std::fabs(X(1.0) - X(g_.v.gate)) > 44.0f)  // (a gate near 100 %: its label says it)
        p.textAligned({X(1.0) + 2.0f, r.y1 - 4.0f}, "next onset", 5.5f, ui::mulAlpha(ink, 0.8f), 0.0f);
    }
    const Vec2 base[2] = {{r.x0, Y(0.0)}, {r.x1, Y(0.0)}};
    p.dashed(base, 0.6f, 1.5f, 2.5f, ui::mulAlpha(th.device.displayGridMajor, 0.8f));
    const Vec2 gate[2] = {{X(g.v.gate), r.y0}, {X(g.v.gate), r.y1}};
    p.dashed(gate, 0.9f, 3.0f, 2.0f, ui::mulAlpha(accent, 0.55f));
    p.textAligned({X(g.v.gate) - 2.0f, r.y1 - 4.0f}, "gate end", 5.5f, ui::mulAlpha(ink, 0.9f), 1.0f);
    // The curve over the note: the Into and Out offsets added (AP.11, each as the voice ramps it); faint outside the
    // note, the parts that do not sound: an Into still travelling at the note's end, an Out begun before the onset.
    const double ti = g.v.into.steps, to = g.v.out.steps, len = g.v.gate;
    std::vector<Vec2> pts;
    constexpr int kN = 96;
    for (int i = 0; i <= kN; ++i) {
      const double x = len * static_cast<double>(i) / kN;
      pts.push_back({X(x), Y(curveAt(x))});
    }
    auto ghost = [&](double a, double b, auto&& f) {
      std::vector<Vec2> q;
      for (int i = 0; i <= 32; ++i) {
        const double x = a + (b - a) * static_cast<double>(i) / 32;
        q.push_back({X(x), Y(f(x))});
      }
      p.polyline(q, 1.2f, ui::mulAlpha(accent, 0.4f));
    };
    // (each continues the sounding curve: the other side held at its value at that end)
    if (ti > len) ghost(len, ti, [&](double x) { return drawnAt(x); });
    if (to > len) ghost(len - to, 0.0, [&](double x) { return drawnAt(x); });
    // Where the source / target come from: a lead-in at the source's pitch (the previous step / the offset's), a
    // lead-out to the target's.
    if (g.v.into.mode != 0) {
      const Vec2 lead[2] = {{r.x0, Y(g.src)}, {X(0.0), Y(g.src)}};
      p.dashed(lead, 1.0f, 2.0f, 2.0f, ui::mulAlpha(accent, 0.6f));
      p.textAligned({r.x0 + 1.0f, Y(g.src) + (g.src > 0 ? -5.0f : 5.0f)},
                    g.v.into.mode == 1 ? std::string("previous step") : g.v.into.text, 5.5f, ink, 0.0f);
    }
    if (g.v.out.mode != 0) {
      const Vec2 lead[2] = {{X(len), Y(g.tgt)}, {r.x1, Y(g.tgt)}};
      p.dashed(lead, 1.0f, 2.0f, 2.0f, ui::mulAlpha(accent, 0.6f));
      p.textAligned({r.x1 - 1.0f, Y(g.tgt) + (g.tgt > 0 ? -5.0f : 5.0f)},
                    g.v.out.mode == 1 ? std::string("next step") : g.v.out.text, 5.5f, ink, 1.0f);
    }
    p.polyline(pts, 2.2f, accent);
    // The time spans dimensioned: Into above the plot, Out below (they may overlap).
    auto dim = [&](double a, double b, float y, float ty, const char* name, const doc::NoteGlideSide& s) {
      const float x0 = X(a), x1 = X(b);
      p.line({x0, y - 2.5f}, {x0, y + 2.5f}, 0.8f, ink);
      p.line({x1, y - 2.5f}, {x1, y + 2.5f}, 0.8f, ink);
      p.line({x0, y}, {x1, y}, 0.8f, ink);
      char t[48];
      std::snprintf(t, sizeof t, "%s %.0f %%", name, s.time * 100.0);
      p.text({(x0 + x1) * 0.5f, ty}, t, 5.5f, ink);
    };
    if (ti > 0.0) dim(0.0, ti, r.y0 - 4.0f, r.y0 - 10.0f, "In", g.v.into);
    else p.textAligned({X(0.0) + 2.0f, r.y0 - 7.0f}, g.v.into.mode == 0 ? "Into off" : "Into: none", 5.5f, ui::mulAlpha(ink, 0.7f), 0.0f);
    if (to > 0.0) dim(len - to, len, r.y1 + 5.0f, r.y1 + 11.0f, "Out", g.v.out);
    else p.textAligned({X(len) - 2.0f, r.y1 + 9.0f}, g.v.out.mode == 0 ? "Out off" : "Out: none", 5.5f, ui::mulAlpha(ink, 0.7f), 1.0f);
    // The handles.
    for (const Handle h : {Handle::IntoTime, Handle::IntoOffset, Handle::OutTime, Handle::OutOffset}) {
      const std::optional<Vec2> at = handleAt(h);
      if (!at) continue;
      const bool hot = h == drag_ || (drag_ == Handle::None && h == hover_);
      p.disc(*at, hot ? 4.0f : 3.0f, hot ? th.face.signal : accent);
      p.ring(*at, hot ? 4.0f : 3.0f, 1.0f, th.device.display);
    }
  }
  bool hit(Vec2 l) const noexcept override { return l.x >= 0.0f && l.y >= 0.0f && l.x <= desc_.w && l.y <= desc_.h; }
  bool interactive() const noexcept override { return true; }
  bool dragging() const noexcept override { return drag_ != Handle::None; }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    if (drag_ == Handle::None && (e.kind == K::Move || e.kind == K::Leave)) {
      const Handle h = e.kind == K::Move && geometry(t) ? nearest(e.local) : Handle::None;
      const bool changed = h != hover_;
      hover_ = h;
      return changed;
    }
    if (e.kind == K::Down) {
      if (e.button != ui::PointerButton::Left || !geometry(t)) return false;
      const Handle h = nearest(e.local);
      if (h == Handle::None) return false;
      const doc::Binding* b = binding(h == Handle::IntoTime || h == Handle::IntoOffset ? kIntoTime : kOutTime);
      if (b == nullptr || !t.info(*b).valid || !t.info(*b).editable) return true;
      drag_ = h;
      bendDrag_ = (e.mods & ui::mod::kAlt) != 0;
      press_ = e.local;
      startBend_ = into() ? g_.v.into.bend : g_.v.out.bend;
      startCents_ = into() ? g_.src : g_.tgt;
      perPx_ = g_.cmax / ((plot().y1 - plot().y0) * 0.5);
      unitCents_.clear();  // the cents of each whole unit offset −48 … 48 (an offset drag snaps to the nearest)
      if (const doc::Project* pr = t.project(); pr != nullptr && !g_.v.cents && (h == Handle::IntoOffset || h == Handle::OutOffset))
        for (int k = -48; k <= 48; ++k) unitCents_.push_back(doc::noteGlideOffsetCents(*pr, binding(kDevice)->device, k));
      t.begin(*b, bendDrag_ ? "Glide Bend" : "Glide Handle");
      return true;
    }
    if (e.kind == K::Move && drag_ != Handle::None) {
      apply(t, e.local);
      return true;
    }
    if (e.kind == K::Up && drag_ != Handle::None) {
      drag_ = Handle::None;
      t.end();
      return true;
    }
    return false;
  }
  void cancelDrag(PanelTarget& t) override {
    if (drag_ == Handle::None) return;
    drag_ = Handle::None;
    t.cancel();
  }
  ui::Cursor cursor() const noexcept override {
    const Handle h = drag_ != Handle::None ? drag_ : hover_;
    if (h == Handle::IntoTime || h == Handle::OutTime) return ui::Cursor::ResizeEW;
    if (h != Handle::None) return ui::Cursor::ResizeNS;
    return ui::Cursor::Inherit;
  }
  uint64_t liveKey(const PanelTarget& t) const override {
    uint64_t h = Element::liveKey(t);
    if (h == kAnyChange) return h;
    if (const doc::Project* pr = t.project(); pr != nullptr && binding(kDevice) != nullptr) {  // (the rate, the tuning)
      const doc::DeviceLoc loc = doc::findDevice(*pr, binding(kDevice)->device);
      h = hmix(hmix(h, reinterpret_cast<uintptr_t>(loc.track)), reinterpret_cast<uintptr_t>(loc.device));
    }
    const double bpm = t.tempoBpm();
    uint64_t bb = 0;
    std::memcpy(&bb, &bpm, sizeof bb);
    h = hmix(hmix(h, bb), static_cast<uint64_t>(static_cast<int>(hover_) + 2) | (static_cast<uint64_t>(static_cast<int>(drag_) + 2) << 8));
    return h == kAnyChange ? h - 1 : h;
  }
  void describe(const PanelTarget&, std::string& title, std::string& body) const override {
    title = "Bend";
    body = "One step's pitch as the glides bend it: from the source (the previous step, or the Into offset) along the "
           "Into curve to the note, held, then along the Out curve over the end of the gate to the target (the next "
           "step, or the Out offset). The two curves add, so they may overlap; each Time is a % of the note (up to "
           "200 %: an Into still gliding at the note's end, an Out begun before its start — drawn faint). Drag a handle "
           "on the note sideways: that side's time; the end handles up / down: its offset; Alt+drag up / down: its "
           "bend. One undo step per drag.";
  }
  // tests: a handle's position (element-local); nullopt: not shown.
  std::optional<Vec2> handlePos(int32_t h) {
    return static_cast<Handle>(h) >= Handle::IntoTime && static_cast<Handle>(h) <= Handle::OutOffset
               ? handleAt(static_cast<Handle>(h))
               : std::nullopt;
  }
  bool prepare(const PanelTarget& t) { return geometry(t); }

private:
  struct G {
    doc::NoteGlideView v;
    double src = 0.0, tgt = 0.0;  // cents of the source / target from the note's own pitch
    double lo = -0.15, span = 1.5;  // steps at the plot's left edge, across it (fitted; held while a handle drags)
    double cmax = 100.0;          // cents from the middle to the top
  };
  Rect plot() const noexcept { return {6.0f, 17.0f, desc_.w - 6.0f, desc_.h - 18.0f}; }
  float X(double s) const noexcept {
    const Rect r = plot();
    return r.x0 + static_cast<float>((s - g_.lo) / g_.span) * (r.x1 - r.x0);
  }
  float Y(double c) const noexcept {
    const Rect r = plot();
    return (r.y0 + r.y1) * 0.5f - static_cast<float>(c / g_.cmax) * (r.y1 - r.y0) * 0.5f;
  }
  // The device's glides now (false: none to draw).
  bool geometry(const PanelTarget& t) {
    const doc::Binding* b = binding(kDevice);
    const doc::Project* pr = t.project();
    if (b == nullptr || pr == nullptr) return false;
    g_.v = doc::noteGlideView(*pr, b->device, t.tempoBpm());
    if (!g_.v.valid) return false;
    constexpr double kStep = 200.0;  // the previous / next step drawn this far (cents) from the note: a placeholder
    const double si = g_.v.into.mode == 2 ? g_.v.into.cents : (g_.v.into.mode == 1 ? -kStep : 0.0);
    const double so = g_.v.out.mode == 2 ? g_.v.out.cents : (g_.v.out.mode == 1 ? kStep : 0.0);
    g_.src = std::isfinite(si) ? si : 0.0;
    g_.tgt = std::isfinite(so) ? so : 0.0;
    double over = 1.0;  // room for an overshoot past the target / source
    if (g_.v.into.shape == 3 || g_.v.out.shape == 3) over = 1.35;
    double sum = 0.0;  // AP.11: the curves add (overlapping, same sign: past either offset)
    for (int i = 0; i <= 64; ++i) sum = std::max(sum, std::fabs(curveAt(g_.v.gate * i / 64.0)));
    g_.cmax = std::max({std::fabs(g_.src), std::fabs(g_.tgt), sum, 50.0}) * over * 1.12;
    if (drag_ == Handle::None) {  // AP.11: fitted to the note, the next onset, an Into past the end, an Out before the start
      const double x0 = std::min(0.0, g_.v.gate - g_.v.out.steps);
      const double x1 = std::max({1.0, g_.v.gate, g_.v.into.steps});
      const double pad = 0.12 * (x1 - x0);
      g_.lo = x0 - pad;
      g_.span = x1 - x0 + 2.0 * pad;
    }
    return true;
  }
  bool into() const noexcept { return drag_ == Handle::IntoTime || drag_ == Handle::IntoOffset; }
  // AP.11: each side's offset (cents from the note) at x steps from the onset, and the two added (the note's pitch).
  double inAt(double x) const {
    const double ti = g_.v.into.steps;
    return ti > 0.0 && x < ti ? g_.src * (1.0 - doc::noteRampShape(g_.v.into.shape, g_.v.into.bend, x / ti)) : 0.0;
  }
  double outAt(double x) const {
    const double to = g_.v.out.steps, len = g_.v.gate;
    return to > 0.0 && x >= len - to ? g_.tgt * doc::noteRampShape(g_.v.out.shape, g_.v.out.bend, (x - (len - to)) / to) : 0.0;
  }
  double curveAt(double x) const { return inAt(x) + outAt(x); }
  // The drawn curve: the note's, continued faint past its end (an Into: the Out held at its target) and before its
  // onset (an Out: the Into at its source).
  double drawnAt(double x) const {
    if (x > g_.v.gate) return inAt(x) + (g_.v.out.steps > 0.0 ? g_.tgt : 0.0);
    if (x < 0.0) return outAt(x) + (g_.v.into.steps > 0.0 ? g_.src : 0.0);
    return curveAt(x);
  }
  std::optional<Vec2> handleAt(Handle h) const {
    const doc::NoteGlideView& v = g_.v;
    if (!v.valid) return std::nullopt;
    switch (h) {
      case Handle::IntoTime:  // (on the curve: AP.11)
        return v.into.mode != 0 ? std::optional<Vec2>(Vec2{X(v.into.steps), Y(drawnAt(v.into.steps))}) : std::nullopt;
      case Handle::IntoOffset: return v.into.mode == 2 ? std::optional<Vec2>(Vec2{X(0.0), Y(g_.src)}) : std::nullopt;
      case Handle::OutTime:
        return v.out.mode != 0 ? std::optional<Vec2>(Vec2{X(v.gate - v.out.steps), Y(drawnAt(v.gate - v.out.steps))})
                               : std::nullopt;
      case Handle::OutOffset: return v.out.mode == 2 ? std::optional<Vec2>(Vec2{X(v.gate), Y(g_.tgt)}) : std::nullopt;
      default: return std::nullopt;
    }
  }
  Handle nearest(Vec2 l) const {
    Handle best = Handle::None;
    float bd = 7.0f * 7.0f;
    for (const Handle h : {Handle::IntoOffset, Handle::OutOffset, Handle::IntoTime, Handle::OutTime})
      if (const std::optional<Vec2> at = handleAt(h)) {
        const float d = (at->x - l.x) * (at->x - l.x) + (at->y - l.y) * (at->y - l.y);
        if (d < bd) {
          bd = d;
          best = h;
        }
      }
    return best;
  }
  void set(PanelTarget& t, size_t slot, double v) {
    if (const doc::Binding* b = binding(slot); b != nullptr && t.info(*b).valid) t.setPosition(*b, positionOf(t, *b, v));
  }
  // One drag move: the time (sideways), the offset (up / down), or with Alt the bend (up / down).
  void apply(PanelTarget& t, Vec2 l) {
    const bool in = into();
    const doc::NoteGlideSide& s = in ? g_.v.into : g_.v.out;
    if (bendDrag_) {
      set(t, in ? kIntoBend : kOutBend, std::clamp(startBend_ + static_cast<double>(press_.y - l.y) / 40.0, -1.0, 1.0));
      return;
    }
    const Rect r = plot();
    if (drag_ == Handle::IntoTime || drag_ == Handle::OutTime) {
      const double pxPerStep = (r.x1 - r.x0) / g_.span;
      double steps = in ? (l.x - X(0.0)) / pxPerStep : (X(g_.v.gate) - l.x) / pxPerStep;
      const double frac = g_.v.gate > 0.0 ? steps / g_.v.gate : 0.0;  // AP.11: % of the note
      set(t, in ? kIntoTime : kOutTime, std::clamp(frac, 0.0, 2.0)  /* engine kArpMaxGlideTime */);
      return;
    }
    // The offset: the cents the pointer's height means (from the press, at the press's scale), in cents or the
    // nearest whole unit.
    const double want = startCents_ + static_cast<double>(press_.y - l.y) * perPx_;
    double v = want;
    if (!g_.v.cents) {
      double bd = 1e300;
      v = s.offset;
      for (size_t i = 0; i < unitCents_.size(); ++i)
        if (std::isfinite(unitCents_[i]) && std::fabs(unitCents_[i] - want) < bd) {
          bd = std::fabs(unitCents_[i] - want);
          v = static_cast<double>(i) - 48.0;
        }
    }
    set(t, in ? kIntoOffset : kOutOffset, v);
  }
  G g_;
  Handle hover_ = Handle::None, drag_ = Handle::None;
  bool bendDrag_ = false;
  Vec2 press_{0.0f, 0.0f};
  double startBend_ = 0.0, startCents_ = 0.0, perPx_ = 1.0;
  std::vector<double> unitCents_;
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
  reg("arpbend", "Bend View", BendView::kStyle, kBendSlots, 300, 110, &make<BendView>);
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

uint32_t resolvedCount(const Element& e) {
  const auto* s = dynamic_cast<const PatternView*>(&e);
  return s != nullptr ? static_cast<uint32_t>(s->resolved().notes.size()) : 0;
}

uint32_t resolvedGenerated(const Element& e) {
  const auto* s = dynamic_cast<const PatternView*>(&e);
  if (s == nullptr) return 0;
  uint32_t n = 0;
  for (const doc::NotePatternNote& x : s->resolved().notes) n += x.source ? 0u : 1u;
  return n;
}

int32_t resolvedLit(const Element& e) {
  const auto* s = dynamic_cast<const PatternView*>(&e);
  return s != nullptr ? s->litNote() : -1;
}

uint64_t resolvedComputes(const Element& e) {
  const auto* s = dynamic_cast<const PatternView*>(&e);
  return s != nullptr ? s->computes() : 0;
}

std::optional<ui::Vec2> bendHandle(Element& e, const PanelTarget& t, int32_t h) {
  auto* s = dynamic_cast<BendView*>(&e);
  if (s == nullptr || !s->prepare(t)) return std::nullopt;
  return s->handlePos(h);
}

}  // namespace test

}  // namespace pq::views::panel
