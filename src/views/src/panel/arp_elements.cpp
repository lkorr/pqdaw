// AP.6 (docs/plans/AP.md "AP.6"; pq/document/note_devices.h noteDevicePanel): the element kinds the Arpeggiator's panel
// adds to the panel system. Each is general — bound to params, settings and a note device's status by its bindings and
// style, never to the arpeggiator by name:
//
//   presets     chips that set SEVERAL bindings at once: item k sets slot s to the k-th group of `values` (in the
//               bindings' units, "1 1|2 3|3 2": a tuplet's n and m — Straight, Dotted, Triplet); one undo step. The
//               item whose values all bindings have now is lit (none: a tuplet typed in by hand).
//   rate        AR.4: ONE RATE CONTROL over a synced rate's four numbers (slots: numerator, denominator, tuplet n, m):
//               a box reading "1/16", "1/8 T", "1/8 D", "3/16 5:4" — a click opens the note values 1/1 … 1/128 (sets
//               1 / den in one undo step, the tuplet stays), the wheel steps through them — and an "n:m" chip (view
//               state). Its selection is 1 while the chip is on or the rate is none of the menu's: the panel shows
//               the four number boxes on it, the feel chips (a `presets`) on 0.
//   steplane    a STEP-SEQUENCER LANE (AR.4): slot 0 its on / off switch, slot 1 its length (1 … the steps bound), slot
//               2 a seed (Randomize), slot 3 the pitch unit (a grid's rows; may be unbound), slots 4 … its steps
//               (controls). On the left the NAME COLUMN: the switch with the lane's name, the menu's dots, a readout
//               (the step under the pointer and its value in its unit, else the length). Its selection is the switch:
//               the panel puts a conditional `fold` band (look "band") over the element's part below the name strip,
//               so a lane switched OFF is only that strip and the panel's fit height follows.
//               Looks: `bars` (a bar per step, its height the value; `mark`: a line at the neutral position), `grid`
//               (a TRANSPOSE lane: a row per step of the unit, 0 marked, the track table's equaves ruled in Degrees;
//               the rows shown fit the values — ± 7, else the whole equaves that hold them, at most ± 48; each step a
//               block on its row with a stem from 0 and its value written) or `cells` (Play / Skip / Tie).
//               A press on a step sets it from the pointer's height (a grid: the row under it; the rows are held
//               while the drag runs and go on past the edge) and a drag PAINTS across steps (cells: the state the
//               first press gave) — one store gesture, one undo step, param writes only (runtime params: no
//               compile). Alt+press / drag: the default. Double-click: TYPE the value (bars: percent; a grid:
//               steps). Wheel over a step: 1 % (Shift: 10 %), a grid 1 step (Shift: an equave), cells the next state;
//               a run of notches is one undo step. The LENGTH HANDLE, a grip after the last step in play, drags the
//               length (a setting: committed once at the end) and shows its number; steps past it dim. The MENU (the
//               dots, or a right-click on the name column; a right-click on a step stays the host's): Reset,
//               Randomize (the steps in play, from the seed + the lane + a view count), Shift Left / Right and
//               Reverse (within the length), Copy to (`copy`: "Gate=gateStep,gateLen|…" — the values and the length
//               into another lane of the same note device) — each one undo step.
//               While its device's note stage plays (PanelTarget::noteStatus), the lane's current step is lit (`lane`:
//               which of the status's lanes); a lane that is off reads no status.
//   arppattern  the RESOLVED PATTERN of a note device (slot 0: a Data binding naming it; AP.10): the whole cycle its
//               params make of the held notes (pq/document/note_pattern.h notePattern: the engine's own ArpCore run
//               offline), time across, log pitch up over the unit's grid; the held notes solid, the generated ones
//               (Distance / Steps copies, walk steps, ratio / harmonic steps, layers) translucent and dotted with their
//               identity; each note drawn along its pitch curve (its Into / Out glides). The held set comes from the
//               status (its held notes); nothing held: the last set it saw, else a nominal reference note. The note
//               playing lights (the status's position / copy: the next such note of the cycle). Recomputed only when its
//               input key (the device's values, the track, the held set, the tempo) moved.
//               AR.2: slots 1 … 10 the glide params — a drag on a note's head / tail edits them as the bend view does
//               (up / down the offset, sideways the time, onto the note before / after: from / to that step; a
//               double-click: that side off); every note conforms.
//   arpbend     the BEND VIEW, the editor of a note device's glides (AR.2; doc::noteGlideView): one step's note large
//               between ghosts of its neighbours; drag its head / tail (offset, time, onto a ghost), a knee (time), a
//               curve's middle (curve) — one undo step per drag. The curves: doc::noteRampShape (the voice's).
//   arppulse    the PULSE VIEW, the editor of a note device's polyrhythm (AR.3; doc::notePulseView): a row per held
//               note, a dot per pulse of the Cycle — click a dot (mute), drag a row (its division), Shift+drag (its
//               shift), its menu (Euclid, Rotate, Invert); the status lights each row's last pulse and a playhead.
//
// Cost: the status is read only by an element that is painted (a fold that hides it reads nothing): per host frame tick
// while the transport plays or the stage holds notes, a few relaxed loads; the pattern list is copied only when the
// status's serial moved. UI thread only.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "elements_internal.h"
#include "pq/ui/lines.h"
#include "pq/core/note_stage_status.h"
#include "pq/document/note_devices.h"
#include "pq/document/note_pattern.h"
#include "pq/ui/overlay.h"
#include "pq/views/panel/arp_elements.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

constexpr float kLabelPx = 7.5f;
constexpr std::array<std::string_view, 2> kPresetSlots = {"value 1", "value 2"};
constexpr std::array<std::string_view, 4> kRateSlots = {"num", "den", "tuplet n", "tuplet m"};
constexpr std::array<std::string_view, 5> kLaneSlots = {"on", "length", "seed", "unit", "steps"};
constexpr std::array<std::string_view, 3> kLaneLooks = {"bars", "grid", "cells"};

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

// ---- rate (AR.4) ----------------------------------------------------------------------------------------------------

constexpr int32_t kRateDens[] = {1, 2, 4, 8, 16, 32, 64, 128};

class RateMenu final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"colour", StyleSpec::Type::Token, 0, "value"},
      {"label", StyleSpec::Type::Text, 0, ""},
  };
  static constexpr float kChipW = 24.0f;
  enum Slot : size_t { kNum, kDen, kTupN, kTupM };
  struct Rate {
    int32_t num = 1, den = 16, n = 1, m = 1;
  };

  void paintGhost(ui::FacePainter&, const ElementEnv&) override {}
  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 2; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? ui::lerp(th.face.body, th.face.track, 0.5f) : th.face.frame;
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rect r = box(), c = chip();
    if (layer == 0) {
      p.roundedRect(r, 3.0f, ui::rgba(255, 255, 255));
      p.roundedRect(c, 3.0f, ui::rgba(255, 255, 255));
      return;
    }
    outlineRect(p, r, 3.0f, 1.0f, ui::rgba(255, 255, 255));
    outlineRect(p, c, 3.0f, 1.0f, ui::rgba(255, 255, 255));
    const float cx = r.x1 - 6.0f, cy = (r.y0 + r.y1) * 0.5f;  // the chevron
    p.line({cx - 2.5f, cy - 1.2f}, {cx, cy + 1.3f}, 1.1f, ui::rgba(255, 255, 255));
    p.line({cx, cy + 1.3f}, {cx + 2.5f, cy - 1.2f}, 1.1f, ui::rgba(255, 255, 255));
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const Rect r = box(), c = chip();
    const Rgba accent = editable(t) ? colour(th, "colour", "value", th.face.value) : th.face.ink;
    if (env.hovered && !hoverChip_) outlineRect(p, r, 3.0f, 1.25f, th.face.hover);
    p.textAligned({r.x0 + 5.0f, (r.y0 + r.y1) * 0.5f}, text(rate(t)), 8.5f, accent, 0.0f);
    const bool on = custom(t);
    if (on) p.roundedRect(c, 3.0f, accent);
    else if (env.hovered && hoverChip_) outlineRect(p, c, 3.0f, 1.0f, th.face.hover);
    p.text({(c.x0 + c.x1) * 0.5f, (c.y0 + c.y1) * 0.5f}, "n:m", kLabelPx, on ? th.face.body : th.face.ink);
    const std::string& label = labelText(t);
    if (!label.empty() && r.y0 > 6.0f) p.textAligned({r.x0, 4.5f}, label, kLabelPx, th.face.ink, 0.0f);
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    if (e.kind == K::Down) endWheel(t);
    const Rect c = chip();
    const bool inChip = e.local.x >= c.x0;
    if (e.kind == K::Move) {
      const bool changed = inChip != hoverChip_;
      hoverChip_ = inChip;
      return changed;
    }
    const doc::Binding* nb = binding(kNum);
    const doc::Binding* db = binding(kDen);
    if (nb == nullptr || db == nullptr || !editable(t)) return false;
    if (e.kind == K::Wheel) {  // the next / previous note value (a run of notches: one undo step)
      if (!e.valueGestures || inChip || e.wheel.y == 0.0f || (e.mods & (ui::mod::kCtrl | ui::mod::kAlt | ui::mod::kSuper)) != 0)
        return false;
      const Rate now = rate(t);
      int32_t k = 0;  // the list entry nearest the rate now
      for (int32_t i = 0; i < static_cast<int32_t>(std::size(kRateDens)); ++i)
        if (std::abs(kRateDens[i] * now.num - now.den) < std::abs(kRateDens[k] * now.num - now.den)) k = i;
      k = std::clamp(k + (e.wheel.y > 0.0f ? 1 : -1), 0, static_cast<int32_t>(std::size(kRateDens)) - 1);
      if (!wheel_) {
        wheel_ = true;
        t.begin(*db, "Rate");
      }
      t.setPosition(*nb, positionOf(t, *nb, 1.0));
      t.setPosition(*db, positionOf(t, *db, kRateDens[k]));
      return true;
    }
    if (e.kind != K::Down || e.button != ui::PointerButton::Left) return false;
    if (inChip) {  // the n : m boxes (view state; shown anyway while the rate is not a plain note value)
      const bool on = t.viewValue(desc_.id, "custom", 0.0) >= 0.5;
      t.setViewValue(desc_.id, "custom", on ? 0.0 : 1.0);
      if (on && !simple(rate(t))) t.notice("The rate is not a plain note value: its numbers stay shown");
      return true;
    }
    ui::Widget* host = t.menuHost();
    if (host == nullptr) return true;
    const Rate now = rate(t);
    std::vector<ui::MenuItem> menu;
    PanelTarget* target = &t;
    for (const int32_t den : kRateDens) {
      ui::MenuItem m = ui::MenuItem::item("1/" + std::to_string(den), [target, n = *nb, d = *db, den] {
        target->begin(d, "Rate");  // both in one gesture: one undo step (the tuplet stays)
        target->setPosition(n, positionOf(*target, n, 1.0));
        target->setPosition(d, positionOf(*target, d, den));
        target->end();
      });
      m.checked = now.num == 1 && now.den == den;
      menu.push_back(std::move(m));
    }
    (void)ui::openContextMenu(*host, e.device, std::move(menu));
    return true;
  }
  // 1: the n : m boxes show (asked for, or the rate is none of the menu's), 0: the menu and the feel chips say it all.
  std::optional<double> selection(const PanelTarget& t) const override { return custom(t) ? 1.0 : 0.0; }
  uint64_t liveKey(const PanelTarget& t) const override {
    uint64_t h = Element::liveKey(t);
    if (h == kAnyChange) return h;
    h = hmix(h, (custom(t) ? 1u : 0u) | (hoverChip_ ? 2u : 0u));
    return h == kAnyChange ? h - 1 : h;
  }
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    title = labelText(t).empty() ? std::string("Rate") : labelText(t);
    body = "The step length as a note value. Click: choose 1/1 \xE2\x80\xA6 1/128; wheel: the next / previous one. Dotted and "
           "Triplet are the Feel chips beside it. n:m shows the numbers themselves: any numerator / denominator and any "
           "tuplet n : m (n steps in the time of m).";
  }

private:
  Rate rate(const PanelTarget& t) const {
    auto v = [&](size_t s, int32_t def) {
      const doc::Binding* b = binding(s);
      return b != nullptr && t.info(*b).valid ? static_cast<int32_t>(std::lround(unitsOf(t, *b))) : def;
    };
    return {v(kNum, 1), v(kDen, 16), v(kTupN, 1), v(kTupM, 1)};
  }
  static bool simple(const Rate& r) noexcept {
    const bool den = std::find(std::begin(kRateDens), std::end(kRateDens), r.den) != std::end(kRateDens);
    const bool feel = (r.n == 1 && r.m == 1) || (r.n == 2 && r.m == 3) || (r.n == 3 && r.m == 2);
    return r.num == 1 && den && feel;
  }
  bool custom(const PanelTarget& t) const { return t.viewValue(desc_.id, "custom", 0.0) >= 0.5 || !simple(rate(t)); }
  // "1/16", "1/8 T" (triplet), "1/8 D" (dotted), "3/16 5:4".
  static std::string text(const Rate& r) {
    char s[40];
    if (r.n == r.m) std::snprintf(s, sizeof s, "%d/%d", r.num, r.den);
    else if (r.n == 3 && r.m == 2) std::snprintf(s, sizeof s, "%d/%d T", r.num, r.den);
    else if (r.n == 2 && r.m == 3) std::snprintf(s, sizeof s, "%d/%d D", r.num, r.den);
    else std::snprintf(s, sizeof s, "%d/%d %d:%d", r.num, r.den, r.n, r.m);
    return s;
  }
  float top() const noexcept { return txt("label", "") != "-" && desc_.h >= 26.0f ? std::min(10.0f, desc_.h * 0.4f) : 0.0f; }
  Rect box() const noexcept { return {0.5f, top() + 0.5f, desc_.w - kChipW - 2.5f, desc_.h - 0.5f}; }
  Rect chip() const noexcept { return {desc_.w - kChipW, top() + 0.5f, desc_.w - 0.5f, desc_.h - 0.5f}; }
  bool hoverChip_ = false;
};

// ---- steplane -------------------------------------------------------------------------------------------------------

class StepLane final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"look", StyleSpec::Type::Choice, 0, "bars", kLaneLooks},
      {"lane", StyleSpec::Type::Number, -1.0, {}, {}, -1.0, 7.0},
      {"colour", StyleSpec::Type::Token, 0, "value"},
      {"name", StyleSpec::Type::Text, 0, ""},
      {"mark", StyleSpec::Type::Number, -1.0, {}, {}, -1.0, 1.0},
      {"copy", StyleSpec::Type::Text, 0, ""},
  };
  enum Slot : size_t { kOn, kLength, kSeed, kUnit, kFirstStep };
  static constexpr float kGutter = 84.0f;   // the name column
  static constexpr float kHandleW = 12.0f;  // room after the last step (the length handle at 32, its number)
  static constexpr float kStrip = 14.0f;    // the name switch's row: all there is of a lane switched off
  static constexpr int32_t kMinRange = 7, kMaxRange = 48;
  enum class Part : int8_t { None, Switch, Menu, Handle, Step };

  void paintGhost(ui::FacePainter&, const ElementEnv&) override {}
  bool interactive() const noexcept override { return true; }
  bool hit(Vec2 l) const noexcept override {
    return l.x >= 0.0f && l.y >= 0.0f && l.y <= desc_.h && (open() ? l.x <= desc_.w : l.x <= kGutter);
  }
  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return ui::lerp(th.face.body, th.face.track, 0.75f); }
  void paintStatic(ui::FacePainter& p, uint32_t) const override {
    if (!open()) return;
    for (int32_t i = 0; i < steps(); ++i) p.roundedRect(cellRect(i), 2.0f, ui::rgba(255, 255, 255));
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const bool on = laneOn(t);
    const Rgba accent = colour(th, "colour", "value", th.face.value);
    const Part hover = env.hovered ? hover_ : Part::None;
    {  // the name switch and the menu's dots
      const Rect s = pill();
      const float rr = (s.y1 - s.y0) * 0.5f;
      p.roundedRect(s, rr, on ? accent : th.face.track);
      if (hover == Part::Switch) outlineRect(p, s, rr, 1.0f, th.face.hover);
      p.disc({on ? s.x1 - rr : s.x0 + rr, (s.y0 + s.y1) * 0.5f}, rr - 1.5f, th.face.handle);
      p.textAligned({s.x1 + 4.0f, kStrip * 0.5f}, name(t), kLabelPx, ui::mulAlpha(th.face.ink, on ? 1.0f : 0.6f), 0.0f);
      const Rgba dots = ui::mulAlpha(th.face.ink, hover == Part::Menu ? 1.0f : 0.45f);
      for (int k = 0; k < 3; ++k) p.disc({kGutter - 13.0f + 3.5f * static_cast<float>(k), kStrip * 0.5f}, 1.0f, dots);
    }
    if (!open()) return;
    const int32_t n = steps(), len = length(t);
    const std::string_view look = txt("look", "bars");
    if (const NoteStageStatus* st = status_.get(t, binding(kOn))) readStatus(*st);
    // The readout under the name: the step under the pointer (or being dragged / typed) and its value, else the length.
    {
      const int32_t s = typing_ ? typeStep_ : (drag_ && !lengthDrag_ ? last_ : (hover == Part::Step ? hoverStep_ : -1));
      const Rgba dim = ui::mulAlpha(th.face.ink, 0.6f);
      char b[48];
      if (desc_.h < 40.0f) {  // one line
        if (s >= 0 && s < n) std::snprintf(b, sizeof b, "%d: %s", s + 1, valueText(t, s).c_str());
        else std::snprintf(b, sizeof b, "%d step%s", len, len == 1 ? "" : "s");
        p.textAligned({1.0f, desc_.h - 5.0f}, b, 6.5f, s >= 0 ? accent : dim, 0.0f);
      } else if (s >= 0 && s < n) {
        p.textAligned({1.0f, kStrip + 8.0f}, valueText(t, s), 8.0f, accent, 0.0f);
        std::snprintf(b, sizeof b, "step %d of %d", s + 1, len);
        p.textAligned({1.0f, kStrip + 20.0f}, b, 6.5f, dim, 0.0f);
      } else {
        std::snprintf(b, sizeof b, "%d step%s", len, len == 1 ? "" : "s");
        p.textAligned({1.0f, kStrip + 8.0f}, b, 6.5f, dim, 0.0f);
      }
    }
    // Beats: a tick between every four steps.
    for (int32_t i = 4; i < n; i += 4) {
      const float x = cellsX0() + static_cast<float>(i) * cellW();
      p.line({x, 1.0f}, {x, desc_.h - 1.0f}, 1.0f, ui::mulAlpha(th.face.ink, 0.5f));
    }
    if (look == "grid") {
      paintGrid(p, th, t, accent, n, len);
    } else {
      const float mark = static_cast<float>(num("mark", -1.0));
      for (int32_t i = 0; i < n; ++i) {
        const Rect c = cellRect(i);
        const doc::Binding* b = step(i);
        if (b == nullptr) continue;
        const float a = i < len ? 1.0f : 0.28f;  // past the length: dimmed
        const Rgba fill = ui::mulAlpha(accent, a);
        if (look == "cells") {
          const int32_t k = kindAt(t, i);
          const float cy = (c.y0 + c.y1) * 0.5f;
          if (k == 1) {  // Skip: a rest
            p.line({c.x0 + 6.0f, cy}, {c.x1 - 6.0f, cy}, 1.4f, ui::mulAlpha(th.face.ink, 0.8f * a));
          } else if (k == 2) {  // Tie: joined to the step before
            p.roundedRect({c.x0 - 3.0f, cy - 3.0f, c.x1 - 3.0f, cy + 3.0f}, 2.0f, fill);
          } else {  // Play
            p.disc({(c.x0 + c.x1) * 0.5f, cy}, 3.2f, fill);
          }
        } else {
          const float v = static_cast<float>(std::clamp(t.position(*b), 0.0, 1.0));
          const float y = c.y1 - v * (c.y1 - c.y0);
          if (c.y1 - y > 0.3f) p.roundedRect({c.x0 + 1.0f, y, c.x1 - 1.0f, c.y1}, 1.5f, fill);
          if (mark > 0.0f && mark < 1.0f) {  // the neutral value (a Gate of × 1)
            const float my = c.y1 - mark * (c.y1 - c.y0);
            p.line({c.x0 + 1.0f, my}, {c.x1 - 1.0f, my}, 0.8f, ui::mulAlpha(th.face.ink, 0.45f * a));
          }
        }
      }
    }
    if (lit_ >= 0 && lit_ < n && on) {  // the step playing now
      const Rect c = cellRect(lit_);
      outlineRect(p, {c.x0 - 0.5f, c.y0 - 0.5f, c.x1 + 0.5f, c.y1 + 0.5f}, 2.5f, 1.3f, th.face.signal);
      p.beginGlow();
      p.glow({(c.x0 + c.x1) * 0.5f, (c.y0 + c.y1) * 0.5f}, 11.0f, ui::mulAlpha(th.face.signal, 0.25f));
      p.endGlow();
    }
    if (hover == Part::Step && hoverStep_ >= 0 && hoverStep_ < n && !drag_ && !typing_) {
      const Rect c = cellRect(hoverStep_);
      outlineRect(p, c, 2.0f, 1.0f, ui::mulAlpha(th.face.hover, 0.9f));
    }
    // The length handle: a grip after the last step in play; its number while it is hot.
    const float hx = handleX(len);
    const bool hot = lengthDrag_ || hover == Part::Handle;
    p.roundedRect({hx - 2.0f, 1.0f, hx + 2.0f, desc_.h - 1.0f}, 1.5f, hot ? th.face.value : th.face.handle);
    if (hot) {
      char s[8];
      std::snprintf(s, sizeof s, "%d", len);
      const float tx = hx + 5.0f, ty = std::min(desc_.h * 0.5f, 12.0f);
      p.roundedRect({tx - 1.5f, ty - 5.5f, tx + (len >= 10 ? 10.5f : 6.5f), ty + 5.5f}, 2.0f, th.face.body);
      p.textAligned({tx, ty}, s, 8.0f, th.face.value, 0.0f);
    }
    if (typing_ && typeStep_ >= 0 && typeStep_ < n) {  // a value being typed into a step
      const Rect c = cellRect(typeStep_);
      const float cy = std::min((c.y0 + c.y1) * 0.5f, c.y0 + 11.0f);
      const Rect box{c.x0 - 3.0f, cy - 7.0f, c.x1 + 3.0f, cy + 7.0f};
      p.roundedRect(box, 2.5f, th.face.body);
      outlineRect(p, box, 2.5f, 1.25f, th.circuit.selected);
      p.text({(box.x0 + box.x1) * 0.5f, cy}, text_ + "|", 8.0f, th.face.ink);
    }
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    if (e.kind == K::Wheel) return onWheel(e, t);
    if (e.kind == K::Move && !drag_) {
      const Part h = partAt(t, e.local);
      const int32_t s = h == Part::Step ? stepAt(e.local.x) : -1;
      const bool changed = h != hover_ || s != hoverStep_;
      hover_ = h;
      hoverStep_ = s;
      return changed;
    }
    if (e.kind == K::Leave) {
      const bool changed = hover_ != Part::None;
      hover_ = Part::None;
      hoverStep_ = -1;
      return changed;
    }
    if (e.kind == K::Down) {
      endWheel(t);
      if (typing_) commit(t);
      const Part part = partAt(t, e.local);
      if (e.button == ui::PointerButton::Right) {  // the lane's menu on its name column; a step: the host's (automation)
        if (e.local.x >= kGutter) return false;
        openMenu(t, e);
        return true;
      }
      if (e.button != ui::PointerButton::Left) return false;
      if (part == Part::Menu) {
        openMenu(t, e);
        return true;
      }
      if (part == Part::Switch) {
        if (const doc::Binding* ob = binding(kOn); ob != nullptr && t.info(*ob).valid && t.info(*ob).editable)
          t.setOnce(*ob, laneOn(t) ? 0.0 : 1.0, "Step Lane");
        return true;
      }
      if (part == Part::Handle) {  // the length
        const doc::Binding* lb = binding(kLength);
        if (lb == nullptr || !t.info(*lb).valid) return true;
        if (e.clicks >= 2) {
          t.reset(*lb);
          return true;
        }
        drag_ = lengthDrag_ = true;
        t.begin(*lb, "Step Lane Length");
        return true;
      }
      if (part != Part::Step) return false;
      const int32_t i = stepAt(e.local.x);
      const doc::Binding* b = step(i);
      if (b == nullptr || !t.info(*b).valid || !t.info(*b).editable) return false;
      const bool cells = txt("look", "bars") == "cells";
      if (e.clicks >= 2 && !cells && (e.mods & ui::mod::kAlt) == 0) {  // type its value
        typing_ = true;
        typeStep_ = i;
        text_.clear();
        return true;
      }
      drag_ = true;
      last_ = i;
      reset_ = (e.mods & ui::mod::kAlt) != 0;
      cellValue_ = -1;
      range_ = fitRange(t);  // held while the drag runs: the row under the pointer stays that row
      if (cells && !reset_) cellValue_ = (kindAt(t, i) + 1) % 3;
      t.begin(*b, "Step Lane");
      paintStep(t, i, e.local.y);
      return true;
    }
    if (e.kind == K::Move && drag_) {
      if (lengthDrag_) {
        const doc::Binding* lb = binding(kLength);
        const int32_t len = std::clamp(static_cast<int32_t>(std::lround((e.local.x - cellsX0()) / cellW())), 1, steps());
        t.setPosition(*lb, positionOf(t, *lb, len));
        return true;
      }
      const int32_t i = std::clamp(static_cast<int32_t>(std::floor((e.local.x - cellsX0()) / cellW())), 0, steps() - 1);
      // Paint every step the pointer crossed since the last move (a fast drag skips none).
      const int32_t from = std::min(last_, i), to = std::max(last_, i);
      for (int32_t k = from; k <= to; ++k) paintStep(t, k, e.local.y);
      last_ = i;
      return true;
    }
    if (e.kind == K::Up && drag_) {
      drag_ = lengthDrag_ = false;
      hoverStep_ = last_;
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
  bool wantsText() const noexcept override { return typing_; }
  bool onText(const ui::TextEvent& e, PanelTarget&) override {
    if (!typing_) return false;
    text_ += e.text();
    return true;
  }
  bool onKey(const ui::KeyEvent& e, PanelTarget& t) override {
    if (!typing_ || !e.down) return false;
    if (e.key == ui::key::kEscape) typing_ = false;
    else if (e.key == ui::key::kBackspace && !text_.empty()) text_.pop_back();
    else if (e.key == ui::key::kReturn) commit(t);
    return true;  // the step holds the keys while typing
  }
  void focusLost(PanelTarget& t) override {
    if (typing_) commit(t);
  }
  ui::Cursor cursor() const noexcept override {
    if (typing_) return ui::Cursor::Text;
    if (hover_ == Part::Handle || lengthDrag_) return ui::Cursor::ResizeEW;
    return hover_ == Part::None && !drag_ ? ui::Cursor::Inherit : ui::Cursor::Pointer;
  }
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    title = name(t);
    const std::string_view look = txt("look", "bars");
    body = "The switch turns the lane on (off: it takes no room). ";
    if (look == "cells")
      body += "Click a step: Play, Skip (a rest), Tie (continues the step before); drag to paint that state across steps; "
              "wheel: the next state.";
    else if (look == "grid")
      body += "A row per step of the pitch unit, 0 in the middle (Degrees: the equaves ruled); the rows fit the values in "
              "use. Click or drag a step to the row it plays, sideways to paint; wheel: one step, Shift+wheel: an equave; "
              "double-click: type it.";
    else
      body += "Drag a step up / down to set it, sideways to paint across steps; wheel: 1 %, Shift+wheel: 10 %; "
              "double-click: type it (percent).";
    body += " Alt+click or Alt+drag: the default. The grip after the last step is the lane's length (steps past it do not "
            "play). The dots or a right-click on the name: Reset, Randomize, Shift, Reverse, Copy. The lit step is playing.";
  }
  void visitControls(const ControlVisitor& fn) const override {
    if (!open()) return;
    for (int32_t i = 0; i < steps(); ++i)
      if (const doc::Binding* b = step(i); b != nullptr && b->kind == doc::BindingKind::Control) fn(*b, cellRect(i));
  }
  const doc::Binding* controlAt(Vec2 l) const override { return open() && partOf(l) == Part::Step ? step(stepAt(l.x)) : nullptr; }
  std::optional<double> selection(const PanelTarget& t) const override { return laneOn(t) ? 1.0 : 0.0; }
  uint64_t liveKey(const PanelTarget& t) const override {
    uint64_t h = Element::liveKey(t);
    if (h == kAnyChange) return h;
    h = hmix(h, static_cast<uint64_t>(lit_ + 1) | (static_cast<uint64_t>(hoverStep_ + 1) << 16) |
                    (static_cast<uint64_t>(hover_) << 32) | (lengthDrag_ ? 1ull << 40 : 0u) | (typing_ ? 1ull << 41 : 0u) |
                    (static_cast<uint64_t>(typeStep_ + 1) << 48));
    h = hmix(h, std::hash<std::string>{}(text_));
    h = hmix(h, std::bit_cast<uint32_t>(desc_.h));
    if (open() && txt("look", "bars") == "grid") h = hmix(h, static_cast<uint64_t>(equave(t)));
    return h == kAnyChange ? h - 1 : h;
  }
  // The status: the lane's current step (a change repaints), ticking while the transport plays or notes are held. A
  // lane that is off reads nothing.
  bool frameTick(PanelTarget& t) override {
    if (!open()) return false;
    playing_ = t.transportPlaying();
    const NoteStageStatus* st = status_.get(t, binding(kOn));
    if (st == nullptr) return false;
    const int32_t was = lit_;
    readStatus(*st);
    return lit_ != was;
  }
  bool ticking() const noexcept override { return open() && status_.s != nullptr && (playing_ || lit_ >= 0); }
  // tests
  int32_t litStep() const noexcept { return lit_; }
  Rect cell(int32_t i) const noexcept { return cellRect(i); }
  int32_t shownRange(const PanelTarget& t) const { return drag_ ? range_ : fitRange(t); }
  float rowY(const PanelTarget& t, int32_t v) const {  // the middle of the grid row of value v
    const int32_t r = shownRange(t);
    const float rowH = (desc_.h - 2.0f) / static_cast<float>(2 * r + 1);
    return 1.0f + (static_cast<float>(r - v) + 0.5f) * rowH;
  }
  bool typing() const noexcept { return typing_; }

private:
  bool open() const noexcept { return desc_.h >= kStrip + 6.0f; }
  int32_t steps() const noexcept {
    return static_cast<int32_t>(desc_.bindings.size() > kFirstStep ? desc_.bindings.size() - kFirstStep : 0);
  }
  const doc::Binding* step(int32_t i) const noexcept {
    return i >= 0 && i < steps() ? binding(kFirstStep + static_cast<size_t>(i)) : nullptr;
  }
  float cellsX0() const noexcept { return kGutter; }
  float cellW() const noexcept { return (desc_.w - kGutter - kHandleW) / static_cast<float>(std::max(steps(), 1)); }
  Rect cellRect(int32_t i) const noexcept {
    const float w = cellW(), x0 = cellsX0() + static_cast<float>(i) * w;
    return {x0 + 1.0f, 1.0f, x0 + w - 1.0f, desc_.h - 1.0f};
  }
  Rect pill() const noexcept { return {0.5f, 2.0f, 20.5f, 12.0f}; }
  float handleX(int32_t len) const noexcept { return cellsX0() + static_cast<float>(len) * cellW(); }
  int32_t stepAt(float x) const noexcept {
    if (x < cellsX0()) return -1;
    const auto i = static_cast<int32_t>(std::floor((x - cellsX0()) / cellW()));
    return i < steps() ? i : -1;
  }
  // What is under `l`, the handle apart (it needs the length).
  Part partOf(Vec2 l) const noexcept {
    if (l.x < kGutter) {
      if (l.y > kStrip) return Part::None;
      return l.x >= kGutter - 18.0f ? Part::Menu : Part::Switch;
    }
    return open() && stepAt(l.x) >= 0 ? Part::Step : Part::None;
  }
  Part partAt(const PanelTarget& t, Vec2 l) const {
    if (open() && l.x >= kGutter - 4.0f && std::fabs(l.x - handleX(length(t))) <= 4.0f) return Part::Handle;
    return partOf(l);
  }
  std::string name(const PanelTarget& t) const {
    if (const std::string_view n = txt("name", ""); !n.empty()) return std::string(n);
    const doc::Binding* on = binding(kOn);
    return on != nullptr ? t.info(*on).label : std::string("Step lane");
  }
  int32_t length(const PanelTarget& t) const {
    const doc::Binding* b = binding(kLength);
    const int32_t n = steps();
    if (b == nullptr || !t.info(*b).valid) return n;
    return std::clamp(static_cast<int32_t>(std::lround(unitsOf(t, *b))), 1, std::max(n, 1));
  }
  bool laneOn(const PanelTarget& t) const {
    const doc::Binding* b = binding(kOn);
    return b == nullptr || !t.info(*b).valid || t.position(*b) >= 0.5;
  }
  int32_t kindAt(const PanelTarget& t, int32_t i) const {
    const doc::Binding* b = step(i);
    return b != nullptr ? std::clamp(static_cast<int32_t>(std::lround(unitsOf(t, *b))), 0, 2) : 0;
  }
  int32_t valueAt(const PanelTarget& t, int32_t i) const {  // a grid step's value
    const doc::Binding* b = step(i);
    return b != nullptr ? static_cast<int32_t>(std::lround(unitsOf(t, *b))) : 0;
  }
  // ---- the grid look (a transpose lane) ----
  // The pitch unit (slot kUnit: 0 Degrees, 1 Ratio, 2 Harmonics; unbound: plain steps, −1).
  int32_t unit(const PanelTarget& t) const {
    const doc::Binding* b = binding(kUnit);
    return b != nullptr && t.info(*b).valid ? static_cast<int32_t>(std::lround(unitsOf(t, *b))) : -1;
  }
  // The unit's steps in an equave (Degrees: the track table's; 0: none). The table is built only when the track's
  // tuning changed.
  int32_t equave(const PanelTarget& t) const {
    if (unit(t) != 0) return 0;
    const doc::Project* pr = t.project();
    const doc::Binding* b = binding(kOn);
    if (pr == nullptr || b == nullptr) return 12;
    const doc::TuningDef* def = doc::noteDeviceTuning(*pr, b->device);
    if (def != tuning_ || equave_ <= 0) {
      tuning_ = def;
      equave_ = doc::tuningEquaveDegrees(def);
    }
    return equave_;
  }
  // The rows shown: ± kMinRange, or — a value beyond it — the whole equaves that hold every value (no equave: sevens),
  // at most ± kMaxRange.
  int32_t fitRange(const PanelTarget& t) const {
    int32_t m = 0;
    for (int32_t i = 0; i < steps(); ++i) m = std::max(m, std::abs(valueAt(t, i)));
    if (m <= kMinRange) return kMinRange;
    const int32_t e = equave(t), chunk = e > 0 ? e : kMinRange;
    return std::min(kMaxRange, ((m + chunk - 1) / chunk) * chunk);
  }
  void paintGrid(ui::FacePainter& p, const ui::Theme& th, PanelTarget& t, Rgba accent, int32_t n, int32_t len) {
    const int32_t r = drag_ && !lengthDrag_ ? range_ : fitRange(t), e = equave(t);
    const float y0 = 1.0f, rowH = (desc_.h - 2.0f) / static_cast<float>(2 * r + 1);
    const float x0 = cellsX0(), x1 = x0 + static_cast<float>(n) * cellW();
    auto top = [&](int32_t v) { return y0 + static_cast<float>(r - v) * rowH; };
    // The rows: 0 marked, the equaves ruled (no equave: a label every few rows), hairlines while rows are tall enough.
    if (rowH >= 4.0f)
      for (int32_t v = -r; v < r; ++v) p.line({x0, top(v) + rowH}, {x1, top(v) + rowH}, 0.5f, ui::mulAlpha(th.face.ink, 0.08f));
    p.roundedRect({x0, top(0), x1, top(0) + rowH}, 0.0f, ui::mulAlpha(th.face.ink, 0.2f));
    const int32_t every = e > 0 ? e : std::max(1, static_cast<int32_t>(std::ceil(9.0f / rowH)));
    for (int32_t v = -r; v <= r; ++v) {
      if (v % every != 0) continue;
      const float cy = top(v) + rowH * 0.5f;
      if (v != 0 && e > 0) p.roundedRect({x0, top(v), x1, top(v) + rowH}, 0.0f, ui::mulAlpha(th.face.ink, 0.11f));
      if (cy < kStrip + 3.0f) continue;  // (the name strip's: the menu's dots are there)
      char s[8];
      std::snprintf(s, sizeof s, v == 0 ? "%d" : "%+d", v);
      p.textAligned({kGutter - 3.0f, std::min(cy, desc_.h - 4.0f)}, s, 5.5f, ui::mulAlpha(th.face.ink, v == 0 ? 0.9f : 0.6f), 1.0f);
    }
    const float zc = top(0) + rowH * 0.5f;
    for (int32_t i = 0; i < n; ++i) {
      const Rect c = cellRect(i);
      if (step(i) == nullptr) continue;
      const float a = i < len ? 1.0f : 0.28f;
      const int32_t v = valueAt(t, i), vc = std::clamp(v, -r, r);
      const float bh = std::max(rowH, 3.0f), by = top(vc) + (rowH - bh) * 0.5f;
      if (v != 0)  // a stem from 0: up and down read at a glance
        p.roundedRect({c.x0 + 1.0f, std::min(by + bh * 0.5f, zc), c.x1 - 1.0f, std::max(by + bh * 0.5f, zc)}, 1.0f,
                      ui::mulAlpha(accent, 0.22f * a));
      p.roundedRect({c.x0 + 1.0f, by, c.x1 - 1.0f, by + bh}, 1.5f, ui::mulAlpha(accent, (v == 0 ? 0.45f : 1.0f) * a));
      if (v != 0) {  // its value, beside the block (outside the stem where there is room)
        char s[8];
        std::snprintf(s, sizeof s, "%+d", v);
        float ty = v > 0 ? by - 4.5f : by + bh + 4.5f;
        if (ty < c.y0 + 4.0f) ty = by + bh + 4.5f;
        if (ty > c.y1 - 4.0f) ty = by - 4.5f;
        p.text({(c.x0 + c.x1) * 0.5f, ty}, s, 6.5f, ui::mulAlpha(th.face.ink, a));
      }
    }
  }
  // Step i's value as text in the lane's unit.
  std::string valueText(const PanelTarget& t, int32_t i) const {
    const doc::Binding* b = step(i);
    if (b == nullptr) return {};
    const std::string_view look = txt("look", "bars");
    char s[40];
    if (look == "cells") {
      static constexpr const char* kKinds[] = {"Play", "Skip", "Tie"};
      return kKinds[kindAt(t, i)];
    }
    if (look == "grid") {
      const int32_t v = valueAt(t, i), u = unit(t);
      const char* what = u == 0 ? " deg" : (u == 1 ? " ratio steps" : (u == 2 ? " harmonics" : ""));
      std::snprintf(s, sizeof s, v == 0 ? "%d%s" : "%+d%s", v, what);
      return s;
    }
    std::snprintf(s, sizeof s, "%.0f %%", unitsOf(t, *b) * 100.0);
    return s;
  }
  // Step i from the pointer's height `y` (cells: the state of the press; Alt: the default).
  void paintStep(PanelTarget& t, int32_t i, float y) {
    const doc::Binding* b = step(i);
    if (b == nullptr || !t.info(*b).valid) return;
    if (reset_) {  // the declared default (inside the gesture: the same undo step)
      t.reset(*b);
      return;
    }
    double pos;
    if (cellValue_ >= 0) {
      pos = positionOf(t, *b, cellValue_);
    } else if (txt("look", "bars") == "grid") {  // the row under the pointer (past the edge: the rows go on)
      const float rowH = (desc_.h - 2.0f) / static_cast<float>(2 * range_ + 1);
      const int32_t v = range_ - static_cast<int32_t>(std::floor((y - 1.0f) / rowH));
      pos = positionOf(t, *b, std::clamp(v, -kMaxRange, kMaxRange));
    } else {
      const Rect c = cellRect(i);
      pos = std::clamp(static_cast<double>((c.y1 - y) / (c.y1 - c.y0)), 0.0, 1.0);
      const BoundInfo& info = t.info(*b);
      if (info.steps >= 2) pos = std::round(pos * (info.steps - 1)) / (info.steps - 1);
    }
    t.setPosition(*b, pos);
  }
  // The wheel over a step: bars 1 % (Shift: 10 %), a grid one step (Shift: an equave), cells the next state. A run of
  // notches is one undo step (the host ends it: endWheel).
  bool onWheel(const FacePointer& e, PanelTarget& t) {
    if (!e.valueGestures || drag_ || typing_ || !open() || e.wheel.y == 0.0f) return false;
    if ((e.mods & (ui::mod::kCtrl | ui::mod::kAlt | ui::mod::kSuper)) != 0 || partOf(e.local) != Part::Step) return false;
    const int32_t i = stepAt(e.local.x);
    const doc::Binding* b = step(i);
    if (b == nullptr || !t.info(*b).valid || !t.info(*b).editable) return false;
    if (!wheel_) {
      wheel_ = true;
      t.begin(*b, "Step Lane Wheel");
    }
    const int32_t dir = e.wheel.y > 0.0f ? 1 : -1;
    const bool coarse = (e.mods & ui::mod::kShift) != 0;
    const std::string_view look = txt("look", "bars");
    double to;
    if (look == "cells") to = std::clamp(kindAt(t, i) + dir, 0, 2);
    else if (look == "grid") to = std::clamp(valueAt(t, i) + dir * (coarse ? std::max(equave(t), 1) : 1), -kMaxRange, kMaxRange);
    else to = std::round((unitsOf(t, *b) + dir * (coarse ? 0.1 : 0.01)) * 100.0) / 100.0;
    t.setPosition(*b, positionOf(t, *b, to));
    hover_ = Part::Step;
    hoverStep_ = i;
    return true;
  }
  // The typed value into its step (bars: percent; a grid: steps): one undo step.
  void commit(PanelTarget& t) {
    typing_ = false;
    const doc::Binding* b = step(typeStep_);
    if (b == nullptr || text_.empty() || !t.info(*b).valid) return;
    char* end = nullptr;
    const double v = std::strtod(text_.c_str(), &end);
    if (end == text_.c_str() || !std::isfinite(v)) {
      t.notice("'" + text_ + "' is not a value");
      return;
    }
    const double units = txt("look", "bars") == "grid" ? std::round(v) : v / 100.0;
    t.setOnce(*b, positionOf(t, *b, units), "Step Lane Value");
  }
  // ---- the lane's menu ----
  // "Gate=gateStep,gateLen|…": the lanes Copy offers (their name, their step params' prefix, their length setting).
  struct Sibling {
    std::string label, steps, length;
  };
  std::vector<Sibling> siblings() const {
    std::vector<Sibling> out;
    for (const std::string& item : splitItems(txt("copy", ""))) {
      const size_t eq = item.find('='), comma = item.find(',');
      if (eq == std::string::npos || comma == std::string::npos || comma < eq) continue;
      out.push_back({item.substr(0, eq), item.substr(eq + 1, comma - eq - 1), item.substr(comma + 1)});
    }
    return out;
  }
  void openMenu(PanelTarget& t, const FacePointer& e) {
    ui::Widget* host = t.menuHost();
    const doc::Binding* first = step(0);
    if (host == nullptr || first == nullptr || !t.info(*first).valid || !t.info(*first).editable) return;
    // (the actions run after this call, perhaps after a rebuild of the panel: they hold copies, never the element)
    auto all = std::make_shared<std::vector<doc::Binding>>();
    for (int32_t i = 0; i < steps(); ++i)
      if (const doc::Binding* b = step(i)) all->push_back(*b);
    PanelTarget* target = &t;
    const auto len = static_cast<size_t>(std::min<int32_t>(length(t), static_cast<int32_t>(all->size())));
    // The steps in play set to `f(their positions now)`: one undo step.
    auto remap = [target, all, len](const char* label, const std::function<void(std::vector<double>&)>& f) {
      std::vector<double> pos(len);
      for (size_t i = 0; i < len; ++i) pos[i] = target->position((*all)[i]);
      f(pos);
      target->begin((*all)[0], label);
      for (size_t i = 0; i < len; ++i) target->setPosition((*all)[i], pos[i]);
      target->end();
    };
    std::vector<ui::MenuItem> menu;
    menu.push_back(ui::MenuItem::item("Reset", [target, all] {
      target->begin((*all)[0], "Reset Step Lane");
      for (const doc::Binding& b : *all) target->reset(b);
      target->end();
    }));
    {  // Randomize: the steps in play, from the device's seed, the lane and how often it was asked (a view count).
      const std::string_view look = txt("look", "bars");
      const int32_t kind = look == "cells" ? 2 : (look == "grid" ? 1 : 0), range = kind == 1 ? fitRange(t) : 0;
      const doc::Binding* sb = binding(kSeed);
      const auto seed = static_cast<uint64_t>(sb != nullptr && t.info(*sb).valid ? std::llround(unitsOf(t, *sb)) : 0);
      const auto lane = static_cast<uint64_t>(std::llround(num("lane", 0.0)) + 1);
      const uint32_t id = desc_.id;
      menu.push_back(ui::MenuItem::item("Randomize", [target, all, remap, seed, lane, id, kind, range] {
        const double count = target->viewValue(id, "random", 0.0);
        target->setViewValue(id, "random", count + 1.0);
        uint64_t s = hmix(hmix(seed, lane), static_cast<uint64_t>(count));
        auto next = [&s] {  // splitmix64 → [0, 1)
          uint64_t z = (s += 0x9E3779B97F4A7C15ull);
          z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
          z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
          return static_cast<double>((z ^ (z >> 31)) >> 11) / 9007199254740992.0;
        };
        remap("Randomize Step Lane", [&](std::vector<double>& pos) {
          for (size_t i = 0; i < pos.size(); ++i) {
            const double u = next();
            if (kind == 0) pos[i] = u;
            else if (kind == 1) pos[i] = positionOf(*target, (*all)[i], std::floor(u * (2 * range + 1)) - range);
            else pos[i] = positionOf(*target, (*all)[i], u < 0.6 ? 0.0 : (u < 0.85 ? 1.0 : 2.0));  // mostly Play
          }
        });
      }));
    }
    menu.push_back(ui::MenuItem::sep());
    menu.push_back(ui::MenuItem::item("Shift Left", [remap] {
      remap("Shift Step Lane", [](std::vector<double>& p) {
        if (p.size() > 1) std::rotate(p.begin(), p.begin() + 1, p.end());
      });
    }));
    menu.push_back(ui::MenuItem::item("Shift Right", [remap] {
      remap("Shift Step Lane", [](std::vector<double>& p) {
        if (p.size() > 1) std::rotate(p.begin(), p.end() - 1, p.end());
      });
    }));
    menu.push_back(ui::MenuItem::item("Reverse", [remap] {
      remap("Reverse Step Lane", [](std::vector<double>& p) { std::reverse(p.begin(), p.end()); });
    }));
    if (const std::vector<Sibling> sib = siblings(); !sib.empty() && binding(kLength) != nullptr) {
      std::vector<ui::MenuItem> to;
      const std::string lengthName = binding(kLength)->name;
      for (const Sibling& s : sib)
        to.push_back(ui::MenuItem::item(s.label, [target, all, s, lengthName] { copyTo(*target, *all, lengthName, s); }));
      menu.push_back(ui::MenuItem::sep());
      menu.push_back(ui::MenuItem::sub("Copy to", std::move(to)));
    }
    (void)ui::openContextMenu(*host, e.device, std::move(menu));
  }
  // Every step's VALUE and the length into lane `to` of the same note device (clamped to its range): one undo step.
  static void copyTo(PanelTarget& t, const std::vector<doc::Binding>& from, const std::string& lengthName, const Sibling& to) {
    (void)t.run([&](const doc::Project& p, doc::IdSource&) -> doc::ops::EditResult {
      doc::Batch batch{"Copy Step Lane", {}};
      auto done = [&] {
        doc::ops::Edit e;
        e.command = doc::Command(std::move(batch));
        return e;
      };
      if (from.empty()) return done();
      const doc::DeviceId dev = from[0].device;
      const doc::Device* d = doc::findDevice(p, dev).device;
      const doc::NoteDeviceType* type = d != nullptr ? doc::noteDeviceTypeOf(*d) : nullptr;
      if (type == nullptr) return done();
      for (size_t i = 0; i < from.size(); ++i) {
        const doc::NoteParamSpec* src = type->param(from[i].name);
        const doc::NoteParamSpec* dst = type->param(to.steps + std::to_string(i + 1));
        if (src == nullptr || dst == nullptr) continue;
        const double v = std::clamp(doc::noteDeviceValue(*d, *src), std::min(dst->min, dst->max), std::max(dst->min, dst->max));
        batch.commands.push_back(doc::SetDeviceParam{dev, dst->name, doc::noteParamPosition(*dst, v)});
      }
      const doc::NoteParamSpec* sl = type->param(lengthName);
      const doc::NoteParamSpec* dl = type->param(to.length);
      if (sl != nullptr && dl != nullptr) batch.commands.push_back(doc::SetDeviceSetting{dev, dl->name, doc::noteDeviceValue(*d, *sl)});
      return done();
    });
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
  int32_t lit_ = -1, last_ = -1, cellValue_ = -1, hoverStep_ = -1, typeStep_ = -1, range_ = kMinRange;
  Part hover_ = Part::None;
  bool lengthDrag_ = false, reset_ = false, playing_ = false, typing_ = false;
  std::string text_;
  mutable const doc::TuningDef* tuning_ = nullptr;
  mutable int32_t equave_ = 0;
};

// ---- AR.2: the glide drag (arpbend and arppattern) ------------------------------------------------------------------

// The glide params a drag on a note sets (pq/engine/arp.h "Glides"), slots 1 … 10 of both elements after the device
// (the document's kGlideParams order).
constexpr std::array<std::string_view, 11> kPatternSlots = {
    "device",     "into offset", "into prev", "into time", "into curve", "into shape",
    "out offset", "out next",    "out time",  "out curve", "out shape"};

namespace glide {

enum Slot : size_t {
  kDevice, kIntoOffset, kIntoPrev, kIntoTime, kIntoCurve, kIntoShape, kOutOffset, kOutNext, kOutTime, kOutCurve, kOutShape,
  kCount
};
// What a press takes: the note's head / tail (its Into / Out: up / down the offset, sideways the time, onto the
// neighbouring step's note: from / to that step), a side's knee (where its glide ends / starts: the time alone), the
// middle of a side's curve (its Curve), a neighbour's ghost (a click: from / to that step, on / off).
enum class Part : int8_t { None = -1, IntoKnee, Head, OutKnee, Tail, IntoCurve, OutCurve, PrevGhost, NextGhost };

constexpr bool isInto(Part p) noexcept {
  return p == Part::Head || p == Part::IntoKnee || p == Part::IntoCurve || p == Part::PrevGhost;
}
constexpr float kDead = 4.0f;  // px a head / tail drag moves before the other axis follows (a straight pull stays straight)
inline float pastDead(float v) noexcept { return std::fabs(v) <= kDead ? 0.0f : (v > 0.0f ? v - kDead : v + kDead); }

struct Drag {
  Part part = Part::None;
  Vec2 press{0.0f, 0.0f};
  doc::NoteGlideView v;      // at the press (the snaps, the side's start values)
  double startCents = 0.0;   // where the head / tail was drawn (cents from the note's own pitch)
  double centsPerPx = 1.0;   // up
  double notePx = 1.0;       // the note's sounding length (a Time of 100 %)
  bool neighbour = false;    // the pointer is on the neighbouring step's note

  bool active() const noexcept { return part != Part::None; }
  static void set(const doc::PanelElement& d, PanelTarget& t, size_t slot, double value) {
    if (const doc::Binding* b = d.binding(slot); b != nullptr && t.info(*b).valid) t.setPosition(*b, positionOf(t, *b, value));
  }
  static void flag(const doc::PanelElement& d, PanelTarget& t, size_t slot, bool on) {  // a switch: its position
    if (const doc::Binding* b = d.binding(slot); b != nullptr && t.info(*b).valid) t.setPosition(*b, on ? 1.0 : 0.0);
  }
  static bool editable(const doc::PanelElement& d, const PanelTarget& t, Part p) {
    const doc::Binding* b = d.binding(isInto(p) ? kIntoOffset : kOutOffset);
    return b != nullptr && t.info(*b).valid && t.info(*b).editable;
  }
  // Starts the gesture (one undo step until end()). False: the params cannot be edited.
  bool begin(const doc::PanelElement& d, PanelTarget& t, Part p, Vec2 at, const doc::NoteGlideView& view, double cents,
             double perPx, double px) {
    if (!editable(d, t, p)) return false;
    part = p;
    press = at;
    v = view;
    startCents = cents;
    centsPerPx = perPx;
    notePx = std::max(px, 1.0);
    neighbour = false;
    const char* label = p == Part::IntoCurve || p == Part::OutCurve ? "Glide Curve"
                        : (p == Part::IntoKnee || p == Part::OutKnee ? "Glide Time" : "Glide");
    t.begin(*d.binding(isInto(p) ? kIntoOffset : kOutOffset), label);
    return true;
  }
  // One move to `l`. `free`: no snap (Shift); `onNeighbour`: the pointer is on the previous (head) / next (tail)
  // step's note.
  void move(const doc::PanelElement& d, PanelTarget& t, Vec2 l, bool free, bool onNeighbour) {
    const bool in = isInto(part);
    const doc::NoteGlideSide& s = in ? v.into : v.out;
    const double time0 = s.timeParam > 0.0 ? s.timeParam : 0.25;
    const double sgn = in ? 1.0 : -1.0;
    const float dx = l.x - press.x, dy = press.y - l.y;
    switch (part) {
      case Part::Head:
      case Part::Tail: {
        neighbour = onNeighbour;
        if (onNeighbour) {  // from / to the neighbouring step: its offset and time stay
          flag(d, t, in ? kIntoPrev : kOutNext, true);
          set(d, t, in ? kIntoOffset : kOutOffset, s.offset);
          set(d, t, in ? kIntoTime : kOutTime, time0);
          return;
        }
        const float up = pastDead(dy);
        if (up != 0.0f) {  // (pulled off a neighbouring step: from where its head / tail was drawn)
          double want = std::clamp(startCents + static_cast<double>(up) * centsPerPx, -v.maxOffset, v.maxOffset);
          if (!free) want = doc::noteGlideSnap(v, want);
          flag(d, t, in ? kIntoPrev : kOutNext, false);
          set(d, t, in ? kIntoOffset : kOutOffset, want);
        } else {
          flag(d, t, in ? kIntoPrev : kOutNext, s.step);
          set(d, t, in ? kIntoOffset : kOutOffset, s.offset);
        }
        set(d, t, in ? kIntoTime : kOutTime, std::clamp(time0 + sgn * static_cast<double>(pastDead(dx)) / notePx, 0.0, 2.0));
        return;
      }
      case Part::IntoKnee:
      case Part::OutKnee:
        set(d, t, in ? kIntoTime : kOutTime, std::clamp(s.timeParam + sgn * static_cast<double>(dx) / notePx, 0.0, 2.0));
        return;
      case Part::IntoCurve:
      case Part::OutCurve: {
        // Up raises the middle of the drawn curve: an Into from below (an Out to above) bends more, one from above less.
        const double dir = in ? (startCents < 0.0 ? 1.0 : -1.0) : (startCents > 0.0 ? 1.0 : -1.0);
        set(d, t, in ? kIntoCurve : kOutCurve, std::clamp(s.curve + dir * static_cast<double>(dy) / 48.0, -1.0, 1.0));
        return;
      }
      default: return;
    }
  }
  void end(PanelTarget& t) {
    if (part == Part::None) return;
    part = Part::None;
    t.end();
  }
  void cancel(PanelTarget& t) {
    if (part == Part::None) return;
    part = Part::None;
    t.cancel();
  }
  // One step: a side off (a double-click on the head / tail), or its "neighbouring step" switched (a ghost's click).
  static bool off(const doc::PanelElement& d, PanelTarget& t, bool into) {
    if (!editable(d, t, into ? Part::Head : Part::Tail)) return false;
    t.begin(*d.binding(into ? kIntoOffset : kOutOffset), "Glide Off");
    flag(d, t, into ? kIntoPrev : kOutNext, false);
    set(d, t, into ? kIntoOffset : kOutOffset, 0.0);
    t.end();
    return true;
  }
  static bool toggleStep(const doc::PanelElement& d, PanelTarget& t, bool into, bool on) {
    if (!editable(d, t, into ? Part::Head : Part::Tail)) return false;
    t.begin(*d.binding(into ? kIntoOffset : kOutOffset), on ? "Glide From Step" : "Glide Off");
    flag(d, t, into ? kIntoPrev : kOutNext, on);
    t.end();
    return true;
  }
};

// What a side reads: "In −2 deg · 30 %", "Out off".
inline std::string readout(const char* name, const doc::NoteGlideSide& s) {
  char b[96];
  if (s.on) std::snprintf(b, sizeof b, "%s %s \xC2\xB7 %.0f %%", name, s.text.c_str(), s.timeParam * 100.0);
  else std::snprintf(b, sizeof b, "%s off", name);
  return b;
}

}  // namespace glide

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
    if (!drag_.active()) map_ = mapNow();  // (held while a glide drag runs: the pointer keeps its note)
    const Map& m = map_;
    auto Y = [&](float l2) { return m.Y(l2); };
    auto X = [&](float s) { return m.X(s); };
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
      // AR.2: the end a press would take (hover) / is dragging.
      if (static_cast<int32_t>(i) == grab_ && grabPart_ != glide::Part::None && !pts.empty()) {
        const Vec2 at = grabPart_ == glide::Part::Head ? pts.front() : pts.back();
        p.disc(at, 3.5f, th.face.signal);
        p.ring(at, 3.5f, 1.0f, th.device.display);
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
    if (grabPart_ != glide::Part::None && env.target != nullptr && env.target->project() != nullptr && binding() != nullptr) {
      const doc::NoteGlideView v = doc::noteGlideView(*env.target->project(), binding()->device, env.target->tempoBpm());
      const bool in = grabPart_ == glide::Part::Head;
      p.textAligned({desc_.w - 4.0f, desc_.h - 6.0f}, glide::readout(in ? "In" : "Out", in ? v.into : v.out), 6.0f, th.face.signal, 1.0f);
    }
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
    h = hmix(h, static_cast<uint64_t>(grab_ + 1) | (static_cast<uint64_t>(static_cast<int>(grabPart_) + 2) << 32) |
                    (drag_.active() ? 1ull << 40 : 0u));
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
           "the last held notes, else a reference note. Drag a note's head (its left half) up / down and sideways: the "
           "Into glide's amount and time, onto the note before it: from that step; its tail: the Out glide; "
           "double-click: that side off (every note conforms: the Glide section's params). ROLL (top right) shows what "
           "the arpeggiator plays from the clip's notes as ghost notes in the piano roll.";
  }
  // AP.7: the ROLL chip takes presses; AR.2: so do the notes (a glide drag on a head / tail).
  bool hit(Vec2 l) const noexcept override { return l.x >= 0.0f && l.y >= 0.0f && l.x <= desc_.w && l.y <= desc_.h; }
  bool interactive() const noexcept override { return true; }
  bool dragging() const noexcept override { return drag_.active(); }
  void cancelDrag(PanelTarget& t) override {
    drag_.cancel(t);
    grabPart_ = glide::Part::None;
  }
  ui::Cursor cursor() const noexcept override {
    return grabPart_ != glide::Part::None ? ui::Cursor::Move : (hoverChip_ ? ui::Cursor::Pointer : ui::Cursor::Inherit);
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    if (!drag_.active() && e.kind != K::Leave) ensure(t);  // (a press finds the notes of the values now, painted or not)
    const Rect c = rollChip();
    const bool in = e.local.x >= c.x0 && e.local.x <= c.x1 && e.local.y >= c.y0 && e.local.y <= c.y1;
    if (drag_.active()) {
      if (e.kind == K::Move) {
        drag_.move(desc_, t, e.local, (e.mods & ui::mod::kShift) != 0, onNeighbour(e.local));
        return true;
      }
      if (e.kind == K::Up) {
        drag_.end(t);
        grabPart_ = glide::Part::None;
        return true;
      }
      return false;
    }
    if (e.kind == K::Move || e.kind == K::Leave) {
      const bool h = in && e.kind == K::Move;
      glide::Part part = glide::Part::None;
      const int32_t n = e.kind == K::Move && !h ? noteAt(e.local, part) : -1;
      const bool changed = h != hoverChip_ || n != grab_ || part != grabPart_;
      hoverChip_ = h;
      grab_ = n;
      grabPart_ = part;
      return changed;
    }
    if (e.kind != K::Down || e.button != ui::PointerButton::Left) return false;
    if (in) {
      if (const std::optional<bool> on = t.notePreview()) t.setNotePreview(!*on);
      return true;
    }
    // AR.2: a note's head / tail.
    glide::Part part = glide::Part::None;
    const int32_t n = noteAt(e.local, part);
    const doc::Project* pr = t.project();
    if (n < 0 || pr == nullptr || binding() == nullptr) return false;
    const bool into = part == glide::Part::Head;
    map_ = mapNow();
    if (e.clicks >= 2) {
      (void)glide::Drag::off(desc_, t, into);
      return true;
    }
    const doc::NotePatternNote& note = pat_.notes[static_cast<size_t>(n)];
    const doc::NoteGlideView v = doc::noteGlideView(*pr, binding()->device, t.tempoBpm());
    // Where its head / tail is drawn, in cents from its own pitch; the notes before / after it (what a drag onto
    // them means: from / to that step).
    const float own = std::log2(note.hz);
    const double cents = 1200.0 * static_cast<double>((into ? note.curve.front().second : note.curve.back().second) - own);
    neighbourY_.clear();
    float edge = into ? -1e30f : 1e30f;
    for (const doc::NotePatternNote& o : pat_.notes)
      if (into ? (o.on < note.on && o.on > edge) : (o.on > note.on && o.on < edge)) edge = o.on;
    for (const doc::NotePatternNote& o : pat_.notes)
      if (o.on == edge) neighbourY_.push_back(map_.Y(std::log2(o.hz)));
    edgeX_ = into ? map_.X(note.on) : map_.X(note.off);
    const double perPx = static_cast<double>(map_.span) * 1200.0 / static_cast<double>(map_.r.y1 - map_.r.y0);
    grab_ = n;
    grabPart_ = part;
    if (!drag_.begin(desc_, t, part, e.local, v, cents, perPx, static_cast<double>(map_.X(note.off) - map_.X(note.on))))
      grabPart_ = glide::Part::None;
    return true;
  }
  // tests
  const NoteStageStatus::Pattern& pattern() const noexcept { return pattern_; }
  int32_t position() const noexcept { return position_; }
  const doc::NotePattern& resolved() const noexcept { return pat_; }
  int32_t litNote() const noexcept { return held_ > 0 ? lit_ : -1; }
  uint64_t computes() const noexcept { return computes_; }
  // Note `i`'s head / tail as drawn (element-local); nullopt: none.
  std::optional<Vec2> noteEnd(const PanelTarget& t, uint32_t i, bool tail) {
    ensure(t);
    if (i >= pat_.notes.size() || pat_.notes[i].curve.empty()) return std::nullopt;
    map_ = mapNow();
    const auto& c = pat_.notes[i].curve;
    return tail ? Vec2{std::min(map_.X(c.back().first), map_.r.x1), map_.Y(c.back().second)}
                : Vec2{map_.X(c.front().first), map_.Y(c.front().second)};
  }

private:
  Rect plot() const noexcept { return {6.0f, 16.0f, desc_.w - 6.0f, desc_.h - 14.0f}; }
  // The plot's scales: x the cycle (steps), y log2 Hz over the pattern's range (at least a third of an octave), padded.
  struct Map {
    Rect r{0.0f, 0.0f, 1.0f, 1.0f};
    float span = 1.0f, mid = 0.0f, cyc = 1.0f;
    float X(float s) const noexcept { return r.x0 + s / cyc * (r.x1 - r.x0); }
    float Y(float l2) const noexcept { return r.y1 - ((l2 - (mid - 0.5f * span)) / span) * (r.y1 - r.y0); }
  };
  Map mapNow() const noexcept {
    return {plot(), std::max(pat_.hi - pat_.lo, 0.34f) * 1.16f, 0.5f * (pat_.lo + pat_.hi),
            static_cast<float>(std::max(pat_.cycle, 1e-3))};
  }
  // AR.2: the note whose curve is under `l` (−1: none; the nearest in pitch) and the end a press takes: its left half
  // the head (Into), its right half the tail (Out).
  int32_t noteAt(Vec2 l, glide::Part& part) const {
    part = glide::Part::None;
    if (!pat_.valid) return -1;
    const Map m = mapNow();
    int32_t best = -1;
    float bd = 6.0f;
    for (size_t i = 0; i < pat_.notes.size(); ++i) {
      const doc::NotePatternNote& n = pat_.notes[i];
      if (n.curve.empty()) continue;
      const float x0 = m.X(n.on), x1 = std::min(m.X(n.off), m.r.x1);
      if (l.x < x0 - 2.0f || l.x > x1 + 2.0f) continue;
      // Its pitch at the pointer's time (the curve's points are in time order).
      const float ts = std::clamp((l.x - m.r.x0) / (m.r.x1 - m.r.x0) * m.cyc, n.curve.front().first, n.curve.back().first);
      float y = n.curve.back().second;
      for (size_t k = 1; k < n.curve.size(); ++k)
        if (ts <= n.curve[k].first) {
          const auto& [ta, ya] = n.curve[k - 1];
          const auto& [tb, yb] = n.curve[k];
          y = tb > ta ? ya + (yb - ya) * (ts - ta) / (tb - ta) : yb;
          break;
        }
      const float d = std::fabs(m.Y(y) - l.y);
      if (d < bd) {
        bd = d;
        best = static_cast<int32_t>(i);
        part = l.x < 0.5f * (x0 + x1) ? glide::Part::Head : glide::Part::Tail;
      }
    }
    return best;
  }
  // A head drag's pointer is on the note before (left of the head, at that note's pitch); a tail's on the note after.
  bool onNeighbour(Vec2 l) const noexcept {
    if (drag_.part == glide::Part::Head ? l.x >= edgeX_ - 2.0f : l.x <= edgeX_ + 2.0f) return false;
    for (const float y : neighbourY_)
      if (std::fabs(l.y - y) <= 6.0f) return true;
    return false;
  }
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
  // AR.2: the glide drag on a note's head / tail: the note and end under the pointer (hover / drag), the scales and
  // the neighbour (its pitch's y, the head's / tail's x) held while it runs.
  Map map_;
  glide::Drag drag_;
  int32_t grab_ = -1;
  glide::Part grabPart_ = glide::Part::None;
  std::vector<float> neighbourY_;
  float edgeX_ = 0.0f;
  // AP.10: the resolved pattern and what it was computed from.
  std::vector<doc::NotePatternHeld> lastHeld_;
  doc::NotePattern pat_;
  uint64_t key_ = 0, computes_ = 0;
  int32_t lit_ = -1;  // the playing note (an index into pat_.notes; its chord / layers light with it)
};

// ---- arpbend --------------------------------------------------------------------------------------------------------

// AR.2 (was AP.10): the BEND VIEW, the editor of a note device's glides — one step's note drawn large between GHOSTS of
// the previous and the next step (schematic: a step below, a step above), along the curve the voice plays (doc::
// noteGlideView, doc::noteRampShape: the Into and Out offset curves added, AP.11; faint where a curve passes the note).
// The note is what you drag (glide::Drag; one undo step per drag, param writes only):
//   its HEAD   up / down: the Into offset — snapped to the pitch unit's steps of the nominal note (faint lines), 0 =
//              off, Shift: free cents; sideways: the Into time; onto the previous step's ghost: Into from that step.
//   its TAIL   the same for Out and the next step.
//   a knee     (where a glide ends / starts) sideways: that time alone.
//   a curve    its middle up / down: that side's Curve.
// A press anywhere else on the note takes its nearer end. Double-click the head / tail: that side off. Click a ghost:
// from / to that step, on / off. The readouts ("In −2 deg · 30 %") follow. Slot 0 the device (Data), then the params
// in kBendSlots order.
constexpr std::array<std::string_view, 12> kBendSlots = {
    "device",     "into offset", "into prev", "into time", "into curve", "into shape",
    "out offset", "out next",    "out time",  "out curve", "out shape",  "gate"};

class BendView final : public Element {
public:
  using Element::Element;
  using Part = glide::Part;
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
    const Rgba accent = colour(th, "colour", "value", th.face.value), ink = th.device.displayText;
    if (env.target == nullptr || !geometry(*env.target)) {
      p.text({desc_.w * 0.5f, desc_.h * 0.5f}, "No glides", 7.5f, ink);
      return;
    }
    const G& g = g_;
    const Rect r = plot();
    const Part hot = drag_.active() ? drag_.part : hover_;
    // The unit's grid (what an offset snaps to), where its lines are not too dense.
    {
      double gap = 1e300;
      for (size_t i = 1; i < g.v.snaps.size(); ++i) gap = std::min(gap, g.v.snaps[i] - g.v.snaps[i - 1]);
      if (gap < 1e300 && static_cast<float>(gap / g.cmax) * (r.y1 - r.y0) * 0.5f >= 5.0f)
        for (const double c : g.v.snaps) {
          if (c == 0.0 || std::fabs(c) > g.cmax) continue;
          p.line({r.x0, Y(c)}, {r.x1, Y(c)}, 0.5f, ui::mulAlpha(th.device.displayGrid, 0.6f));
        }
    }
    // The step boundaries, the note's own pitch, the gate's end.
    p.line({X(0.0), r.y0}, {X(0.0), r.y1}, 0.6f, th.device.displayGridMajor);
    {
      const Vec2 nx[2] = {{X(1.0), r.y0}, {X(1.0), r.y1}};
      p.dashed(nx, 0.6f, 2.0f, 2.0f, th.device.displayGridMajor);
    }
    const Vec2 base[2] = {{r.x0, Y(0.0)}, {r.x1, Y(0.0)}};
    p.dashed(base, 0.6f, 1.5f, 2.5f, ui::mulAlpha(th.device.displayGridMajor, 0.8f));
    if (std::fabs(g.v.gate - 1.0) > 1e-6) {
      const Vec2 gate[2] = {{X(g.v.gate), r.y0}, {X(g.v.gate), r.y1}};
      p.dashed(gate, 0.9f, 3.0f, 2.0f, ui::mulAlpha(accent, 0.55f));
      p.textAligned({X(g.v.gate) + (g.v.gate < 1.0 ? 2.0f : -2.0f), r.y1 - 4.0f}, "gate end", 5.5f, ui::mulAlpha(ink, 0.8f),
                    g.v.gate < 1.0 ? 0.0f : 1.0f);
    }
    // The neighbours' ghosts: the previous step (the Into's source when it comes from the step), the next one.
    auto ghost = [&](const Rect& gr, bool on, bool hov, const char* label, float align) {
      p.roundedRect(gr, 2.0f, ui::mulAlpha(accent, on ? 0.6f : (hov ? 0.35f : 0.12f)));
      const Vec2 edge[2] = {{gr.x0, (gr.y0 + gr.y1) * 0.5f}, {gr.x1, (gr.y0 + gr.y1) * 0.5f}};
      p.dashed(edge, 1.0f, 2.2f, 1.8f, ui::mulAlpha(accent, on || hov ? 1.0f : 0.4f));
      p.textAligned({align == 0.0f ? gr.x0 + 1.0f : gr.x1 - 1.0f, gr.y0 - 4.5f}, label, 5.5f, ui::mulAlpha(ink, on || hov ? 1.0f : 0.75f),
                    align);
    };
    const bool laneIn = g.v.laneSlide;
    ghost(prevRect(), g.v.into.step || laneIn, hot == Part::PrevGhost || (drag_.part == Part::Head && drag_.neighbour),
          "prev step", 0.0f);
    ghost(nextRect(), g.v.out.step, hot == Part::NextGhost || (drag_.part == Part::Tail && drag_.neighbour), "next step", 1.0f);
    // The curve over the note: the Into and Out offsets added (AP.11, each as the voice ramps it); faint outside the
    // note, the parts that do not sound: an Into still travelling at the note's end, an Out begun before the onset.
    const double ti = g.v.into.steps, to = g.v.out.steps, len = g.v.gate;
    // WF.2: a sample per device px of the note (96 at least) and one at each knee (it was 96 equal steps: the knees,
    // between two samples, were cut off while their handles sat at the exact positions).
    static thread_local std::vector<Vec2> pts;
    static thread_local std::vector<double> ts;
    {
      const double knees[2] = {len > 0.0 ? ti / len : 0.0, len > 0.0 ? (len - to) / len : 0.0};
      ui::curveParams(ts, ui::curveSamples((X(len) - X(0.0)) * p.transform().scale, 96, 2048), knees);
      pts.clear();
      for (const double u : ts) pts.push_back({X(len * u), Yc(curveAt(len * u))});
    }
    auto faint = [&](double a, double b) {
      std::vector<Vec2> q;
      for (int i = 0; i <= 32; ++i) {
        const double x = a + (b - a) * static_cast<double>(i) / 32;
        q.push_back({X(x), Yc(drawnAt(x))});
      }
      p.polyline(q, 1.2f, ui::mulAlpha(accent, 0.4f));
    };
    // (each continues the sounding curve: the other side held at its value at that end)
    if (ti > len) faint(len, ti);
    if (to > len) faint(len - to, 0.0);
    // An offset's lead: where the head starts / the tail ends, off the note's own pitch.
    if (g.v.into.on && !g.v.into.step) {
      const Vec2 lead[2] = {{X(0.0) - 12.0f, Yc(g.src)}, {X(0.0), Yc(g.src)}};
      p.dashed(lead, 1.0f, 2.0f, 2.0f, ui::mulAlpha(accent, 0.6f));
    }
    if (g.v.out.on && !g.v.out.step) {
      const Vec2 lead[2] = {{X(len), Yc(g.tgt)}, {X(len) + 12.0f, Yc(g.tgt)}};
      p.dashed(lead, 1.0f, 2.0f, 2.0f, ui::mulAlpha(accent, 0.6f));
    }
    p.polyline(pts, 3.0f, accent);
    // The handles: head and tail (always), the knees and the curves' middles of the sides that glide.
    for (const Part h : {Part::IntoCurve, Part::OutCurve, Part::IntoKnee, Part::OutKnee, Part::Head, Part::Tail}) {
      const std::optional<Vec2> at = handleAt(h);
      if (!at) continue;
      const bool lit = h == hot;
      const bool end = h == Part::Head || h == Part::Tail;
      const float rad = end ? (lit ? 5.0f : 4.0f) : (lit ? 3.5f : 2.5f);
      if (h == Part::IntoCurve || h == Part::OutCurve) {
        p.disc(*at, rad, th.device.display);
        p.ring(*at, rad, 1.0f, lit ? th.face.signal : accent);
      } else {
        p.disc(*at, rad, lit ? th.face.signal : accent);
        p.ring(*at, rad, 1.0f, th.device.display);
      }
    }
    // The readouts.
    std::string in = glide::readout("In", g.v.into), out = glide::readout("Out", g.v.out);
    if (laneIn) in = "In: the Glide lane's steps slide from the previous step";
    else if (!g.v.into.on) in += " \xC2\xB7 drag the note's head";
    if (!g.v.out.on) out += " \xC2\xB7 drag its tail";
    p.textAligned({r.x0, 8.0f}, in, 6.5f, isInto(hot) && hot != Part::None ? th.face.signal : ink, 0.0f);
    p.textAligned({r.x1, 8.0f}, out, 6.5f, !isInto(hot) && hot != Part::None ? th.face.signal : ink, 1.0f);
  }
  bool hit(Vec2 l) const noexcept override { return l.x >= 0.0f && l.y >= 0.0f && l.x <= desc_.w && l.y <= desc_.h; }
  bool interactive() const noexcept override { return true; }
  bool dragging() const noexcept override { return drag_.active(); }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    if (!drag_.active() && (e.kind == K::Move || e.kind == K::Leave)) {
      const Part h = e.kind == K::Move && geometry(t) ? partAt(e.local) : Part::None;
      const bool changed = h != hover_;
      hover_ = h;
      return changed;
    }
    if (e.kind == K::Down) {
      if (e.button != ui::PointerButton::Left || !geometry(t)) return false;
      const Part h = partAt(e.local);
      if (h == Part::None) return false;
      if (h == Part::PrevGhost) return glide::Drag::toggleStep(desc_, t, true, !g_.v.into.step) || true;
      if (h == Part::NextGhost) return glide::Drag::toggleStep(desc_, t, false, !g_.v.out.step) || true;
      const bool in = glide::isInto(h);
      if (e.clicks >= 2 && (h == Part::Head || h == Part::Tail)) return glide::Drag::off(desc_, t, in) || true;
      const Rect r = plot();
      (void)drag_.begin(desc_, t, h, e.local, g_.v, in ? g_.src : g_.tgt, g_.cmax / ((r.y1 - r.y0) * 0.5),
                        (r.x1 - r.x0) / g_.span * g_.v.gate);
      return true;
    }
    if (e.kind == K::Move && drag_.active()) {
      drag_.move(desc_, t, e.local, (e.mods & ui::mod::kShift) != 0, onGhost(drag_.part, e.local));
      return true;
    }
    if (e.kind == K::Up && drag_.active()) {
      drag_.end(t);
      return true;
    }
    return false;
  }
  void cancelDrag(PanelTarget& t) override { drag_.cancel(t); }
  ui::Cursor cursor() const noexcept override {
    const Part h = drag_.active() ? drag_.part : hover_;
    switch (h) {
      case Part::Head:
      case Part::Tail: return ui::Cursor::Move;
      case Part::IntoKnee:
      case Part::OutKnee: return ui::Cursor::ResizeEW;
      case Part::IntoCurve:
      case Part::OutCurve: return ui::Cursor::ResizeNS;
      case Part::PrevGhost:
      case Part::NextGhost: return ui::Cursor::Pointer;
      default: return ui::Cursor::Inherit;
    }
  }
  uint64_t liveKey(const PanelTarget& t) const override {
    uint64_t h = Element::liveKey(t);
    if (h == kAnyChange) return h;
    if (const doc::Project* pr = t.project(); pr != nullptr && binding(glide::kDevice) != nullptr) {  // (the rate, the tuning, the unit)
      const doc::DeviceLoc loc = doc::findDevice(*pr, binding(glide::kDevice)->device);
      h = hmix(hmix(h, reinterpret_cast<uintptr_t>(loc.track)), reinterpret_cast<uintptr_t>(loc.device));
    }
    const double bpm = t.tempoBpm();
    uint64_t bb = 0;
    std::memcpy(&bb, &bpm, sizeof bb);
    h = hmix(hmix(h, bb), static_cast<uint64_t>(static_cast<int>(hover_) + 2) | (static_cast<uint64_t>(static_cast<int>(drag_.part) + 2) << 8) |
                              (drag_.neighbour ? 1u << 16 : 0u));
    return h == kAnyChange ? h - 1 : h;
  }
  void describe(const PanelTarget&, std::string& title, std::string& body) const override {
    title = "Glide";
    body = "One step's note between the previous and the next step, as its glides bend it. Drag the note's head up / "
           "down: where it starts (the Into amount, snapped to the Pitch unit's steps; Shift: free) and sideways: how "
           "long it takes to arrive; drag the head onto the previous step: it glides from that step. The tail does the "
           "same for the Out glide and the next step. Drag the middle of a curve: its bend. The small dots move a time "
           "alone. Double-click the head / tail: that side off. The knobs beside follow; one undo step per drag.";
  }
  // tests: part `h`'s point (element-local; a glide::Part); nullopt: not shown.
  std::optional<Vec2> handlePos(int32_t h) {
    const auto part = static_cast<Part>(h);
    if (part == Part::PrevGhost || part == Part::NextGhost) {
      const Rect g = part == Part::PrevGhost ? prevRect() : nextRect();
      return Vec2{(g.x0 + g.x1) * 0.5f, (g.y0 + g.y1) * 0.5f};
    }
    return part >= Part::IntoKnee && part <= Part::OutCurve ? handleAt(part) : std::nullopt;
  }
  bool prepare(const PanelTarget& t) { return geometry(t); }

private:
  struct G {
    doc::NoteGlideView v;
    double src = 0.0, tgt = 0.0;  // cents of the head / the tail from the note's own pitch
    double prev = -200.0, next = 200.0;  // the ghosts' (schematic)
    double lo = -0.15, span = 1.5;  // steps at the plot's left edge, across it (fitted; held while a drag runs)
    double cmax = 100.0;          // cents from the middle to the top (held while a drag runs)
  };
  Rect plot() const noexcept { return {8.0f, 18.0f, desc_.w - 8.0f, desc_.h - 8.0f}; }
  float X(double s) const noexcept {
    const Rect r = plot();
    return r.x0 + static_cast<float>((s - g_.lo) / g_.span) * (r.x1 - r.x0);
  }
  float Y(double c) const noexcept {
    const Rect r = plot();
    return (r.y0 + r.y1) * 0.5f - static_cast<float>(c / g_.cmax) * (r.y1 - r.y0) * 0.5f;
  }
  float Yc(double c) const noexcept {  // (a drag past the plot's scale: drawn at its edge)
    const Rect r = plot();
    return std::clamp(Y(c), r.y0 - 3.0f, r.y1 + 3.0f);
  }
  Rect prevRect() const noexcept {
    const Rect r = plot();
    const float y = Y(g_.prev);
    return {r.x0, y - 3.0f, X(0.0) - 3.0f, y + 3.0f};
  }
  Rect nextRect() const noexcept {
    const Rect r = plot();
    const float y = Y(g_.next);
    return {std::min(X(std::max(1.0, g_.v.gate)) + 3.0f, r.x1 - 16.0f), y - 3.0f, r.x1, y + 3.0f};
  }
  // The device's glides now (false: none to draw).
  bool geometry(const PanelTarget& t) {
    const doc::Binding* b = binding(glide::kDevice);
    const doc::Project* pr = t.project();
    if (b == nullptr || pr == nullptr) return false;
    g_.v = doc::noteGlideView(*pr, b->device, t.tempoBpm());
    if (!g_.v.valid) return false;
    const doc::NoteGlideView& v = g_.v;
    if (!drag_.active()) {
      // The pitch scale: the offsets (at least ± 3 semitones of room to drag into), the ghosts a step inside it.
      const double room = std::max({v.into.step ? 0.0 : std::fabs(v.into.offset), v.out.step ? 0.0 : std::fabs(v.out.offset), 300.0});
      g_.prev = -0.62 * room;
      g_.next = 0.62 * room;
    }
    g_.src = v.into.step || v.laneSlide ? g_.prev : (v.into.on ? v.into.offset : 0.0);
    g_.tgt = v.out.step ? g_.next : (v.out.on ? v.out.offset : 0.0);
    if (v.laneSlide) {  // (drawn as the lane's slide would be: from the previous step over Into time)
      g_.v.into.time = v.into.timeParam;
      g_.v.into.steps = v.into.timeParam * v.gate;
    }
    if (!drag_.active()) {
      double over = 1.0;  // room for an overshoot past the target / source
      if (v.into.shape == 3 || v.out.shape == 3) over = 1.3;
      double sum = 0.0;  // AP.11: the curves add (overlapping, same sign: past either offset)
      for (int i = 0; i <= 64; ++i) sum = std::max(sum, std::fabs(curveAt(v.gate * i / 64.0)));
      g_.cmax = std::max({std::fabs(g_.src), std::fabs(g_.tgt), std::fabs(g_.prev) / 0.62, sum * over}) * 1.15;
      // The time scale: the note, the next onset, an Into past the end, an Out before the start; margins for the ghosts.
      const double x0 = std::min(0.0, v.gate - g_.v.out.steps);
      const double x1 = std::max({1.0, v.gate, g_.v.into.steps});
      const double pad = 0.16 * (x1 - x0);
      g_.lo = x0 - pad;
      g_.span = x1 - x0 + 2.0 * pad;
    }
    return true;
  }
  // AP.11: each side's offset (cents from the note) at x steps from the onset, and the two added (the note's pitch).
  double inAt(double x) const {
    const double ti = g_.v.into.steps;
    return ti > 0.0 && x < ti ? g_.src * (1.0 - doc::noteRampShape(g_.v.into.shape, g_.v.into.curve, x / ti)) : 0.0;
  }
  double outAt(double x) const {
    const double to = g_.v.out.steps, len = g_.v.gate;
    return to > 0.0 && x >= len - to ? g_.tgt * doc::noteRampShape(g_.v.out.shape, g_.v.out.curve, (x - (len - to)) / to) : 0.0;
  }
  double curveAt(double x) const { return inAt(x) + outAt(x); }
  // The drawn curve: the note's, continued faint past its end (an Into: the Out held at its target) and before its
  // onset (an Out: the Into at its source).
  double drawnAt(double x) const {
    if (x > g_.v.gate) return inAt(x) + (g_.v.out.steps > 0.0 ? g_.tgt : 0.0);
    if (x < 0.0) return outAt(x) + (g_.v.into.steps > 0.0 ? g_.src : 0.0);
    return curveAt(x);
  }
  std::optional<Vec2> handleAt(Part h) const {
    const doc::NoteGlideView& v = g_.v;
    if (!v.valid) return std::nullopt;
    const bool in = v.into.on && v.into.steps > 0.0, out = v.out.on && v.out.steps > 0.0;
    switch (h) {
      case Part::Head: return Vec2{X(0.0), Yc(drawnAt(0.0))};
      case Part::Tail: return Vec2{X(v.gate), Yc(drawnAt(v.gate))};
      case Part::IntoKnee: return in ? std::optional<Vec2>(Vec2{X(v.into.steps), Yc(drawnAt(v.into.steps))}) : std::nullopt;
      case Part::OutKnee:
        return out ? std::optional<Vec2>(Vec2{X(v.gate - v.out.steps), Yc(drawnAt(v.gate - v.out.steps))}) : std::nullopt;
      case Part::IntoCurve:
        return in ? std::optional<Vec2>(Vec2{X(v.into.steps * 0.5), Yc(drawnAt(v.into.steps * 0.5))}) : std::nullopt;
      case Part::OutCurve:
        return out ? std::optional<Vec2>(Vec2{X(v.gate - v.out.steps * 0.5), Yc(drawnAt(v.gate - v.out.steps * 0.5))})
                   : std::nullopt;
      default: return std::nullopt;
    }
  }
  static bool inside(const Rect& r, Vec2 l, float pad) noexcept {
    return l.x >= r.x0 - pad && l.x <= r.x1 + pad && l.y >= r.y0 - pad && l.y <= r.y1 + pad;
  }
  // The part a press at `l` takes: the nearest handle within reach (the head / tail win a tie), a ghost, else the
  // note's body (its nearer end).
  Part partAt(Vec2 l) const {
    Part best = Part::None;
    float bd = 8.0f * 8.0f;
    for (const Part h : {Part::Head, Part::Tail, Part::IntoKnee, Part::OutKnee, Part::IntoCurve, Part::OutCurve})
      if (const std::optional<Vec2> at = handleAt(h)) {
        const float d = (at->x - l.x) * (at->x - l.x) + (at->y - l.y) * (at->y - l.y);
        if (d < bd) {
          bd = d;
          best = h;
        }
      }
    if (best != Part::None) return best;
    if (inside(prevRect(), l, 4.0f)) return Part::PrevGhost;
    if (inside(nextRect(), l, 4.0f)) return Part::NextGhost;
    const double len = g_.v.gate;
    if (l.x >= X(0.0) && l.x <= X(len)) {
      const double x = g_.lo + static_cast<double>(l.x - plot().x0) / (plot().x1 - plot().x0) * g_.span;
      if (std::fabs(Yc(curveAt(x)) - l.y) <= 7.0f) return x < len * 0.5 ? Part::Head : Part::Tail;
    }
    return Part::None;
  }
  // A head / tail drag's pointer is on the neighbouring step's ghost.
  bool onGhost(Part drag, Vec2 l) const noexcept {
    if (drag == Part::Head) return l.x < X(0.0) - 3.0f && std::fabs(l.y - Y(g_.prev)) <= 9.0f;
    if (drag == Part::Tail) return l.x > nextRect().x0 && std::fabs(l.y - Y(g_.next)) <= 9.0f;
    return false;
  }
  static bool isInto(Part p) noexcept { return glide::isInto(p); }
  G g_;
  Part hover_ = Part::None;
  glide::Drag drag_;
};

// ---- arppulse -------------------------------------------------------------------------------------------------------

// AR.3: the PULSE VIEW, the editor of a note device's Pulse mode (pq/engine/arp.h "Pulse mode"; doc::notePulseView): the
// polyrhythm drawn — one ROW per held note (the lowest at the bottom; the held set from the status as the pattern view
// reads it, nothing held: the last set seen; at least 4 rows, the ones without a note dimmed: what a note added there
// would play), the Cycle across, a DOT per pulse (filled: it plays, a ring: muted), the cycle's beats as faint lines,
// onsets that coincide across rows joined, and while the stage plays each row's last pulse lit and a playhead at its
// place in the cycle (NoteStageStatus pulse fields). The rows are what you edit:
//   click a dot          mutes / unmutes that pulse (the row's mask: a setting, one undo step)
//   drag a row sideways  its division (÷1 … ÷64; the wheel: a step) — a row whose note Auto clocks by its JI harmonic
//                        ("h9") is locked
//   Shift+drag           its shift, of its own period (snapped to 1/24; Alt: free)
//   ⋯ / right-click      Euclid k of n, Rotate, Invert, All on, Reset
// One undo step per gesture. Slot 0 the device (Data), then Auto, the Cycle's num / den, and per row its division,
// shift, mask lo, mask hi (kPulseSlots). Recomputed only when its inputs moved (the device, its bound values, the
// held set); the status is read only while it is painted or ticking.
constexpr std::array<std::string_view, 5> kPulseSlots = {"device", "auto", "cycle num", "cycle den", "rows"};

class PulseView final : public Element {
public:
  using Element::Element;
  enum Slot : size_t { kDevice, kAuto, kCycleNum, kCycleDen, kFirstRow };
  enum RowSlot : size_t { kDiv, kShift, kMaskLo, kMaskHi, kPerRow };
  static constexpr StyleSpec kStyle[] = {
      {"colour", StyleSpec::Type::Token, 0, "value"},
  };
  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return th.device.display; }
  void paintStatic(ui::FacePainter& p, uint32_t) const override {
    p.roundedRect({0.0f, 0.0f, desc_.w, desc_.h}, 4.0f, ui::rgba(255, 255, 255));
  }
  void paintGhost(ui::FacePainter&, const ElementEnv&) override {}
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    const Rgba accent = colour(th, "colour", "value", th.face.value), ink = th.device.displayText;
    if (env.target != nullptr) {
      if (const NoteStageStatus* st = status_.get(*env.target, binding(kDevice))) (void)read(*st);
      ensure(*env.target);
    }
    if (!view_.valid || view_.rows.empty()) {
      p.text({desc_.w * 0.5f, desc_.h * 0.5f}, "No pulse rows", 7.5f, ink);
      return;
    }
    const Rect r = plot();
    const auto rows = static_cast<int32_t>(view_.rows.size());
    const float W = r.x1 - r.x0;
    // The cycle's beats (quarter notes of a 4/4 bar; bars stronger), its start and end.
    const double beats = 4.0 * view_.cycleNum / view_.cycleDen;
    if (beats <= 64.0)
      for (int k = 1; static_cast<double>(k) < beats - 1e-9; ++k) {
        const float x = r.x0 + static_cast<float>(k / beats) * W;
        p.line({x, r.y0}, {x, r.y1}, 0.5f, k % 4 == 0 ? th.device.displayGridMajor : ui::mulAlpha(th.device.displayGrid, 0.7f));
      }
    p.line({r.x0, r.y0}, {r.x0, r.y1}, 0.8f, th.device.displayGridMajor);
    p.line({r.x1, r.y0}, {r.x1, r.y1}, 0.8f, th.device.displayGridMajor);
    const bool live = held_ > 0;
    const int32_t hotRow = mode_ != Mode::None ? pressRow_ : (wheel_ ? wheelRow_ : hoverRow_);
    // Coinciding onsets: a line through the rows that play together there.
    for (const Link& l : links_) {
      const float x = r.x0 + l.x * W;
      p.line({x, rowY(l.lo)}, {x, rowY(l.hi)}, 1.0f, ui::mulAlpha(accent, 0.3f));
    }
    for (int32_t i = 0; i < rows; ++i) {
      const doc::NotePulseRow& row = view_.rows[static_cast<size_t>(i)];
      const float y = rowY(i);
      const bool hot = i == hotRow;
      const float a = row.held || !live ? (row.held ? 1.0f : 0.6f) : 0.4f;  // (a row without a note: dimmed)
      p.line({r.x0, y}, {r.x1, y}, 0.5f, ui::mulAlpha(th.device.displayGrid, hot ? 1.0f : 0.6f));
      char s[24];
      if (row.locked) std::snprintf(s, sizeof s, "h%d", row.division);
      else std::snprintf(s, sizeof s, "\xC3\xB7%d", row.division);  // ÷
      p.textAligned({r.x0 - 6.0f, y}, s, 7.5f, hot ? th.face.signal : ui::mulAlpha(ink, std::max(a, 0.75f)), 1.0f);
      const int32_t n = row.division;
      const float rad = dotRadius(i);
      if (W / static_cast<float>(n) < 2.5f) {  // too dense to tell apart (a high harmonic): a band
        p.roundedRect({r.x0, y - 2.0f, r.x1, y + 2.0f}, 1.0f, ui::mulAlpha(accent, 0.5f * a));
      } else {
        const int32_t lit = live && static_cast<uint32_t>(i) < NoteStageStatus::kPulseRows ? lit_[static_cast<size_t>(i)] : -1;
        for (int32_t j = 0; j < n; ++j) {
          const Vec2 c{dotX(i, j), y};
          const bool on = ((row.mask >> (static_cast<uint32_t>(j) & 31u)) & 1u) != 0;
          if (j == lit) {
            p.beginGlow();
            p.glow(c, rad + 6.0f, ui::mulAlpha(th.face.signal, on ? 0.45f : 0.2f));
            p.endGlow();
          }
          if (on) p.disc(c, rad, j == lit ? th.face.signal : ui::mulAlpha(accent, a));
          else p.ring(c, rad, 1.0f, ui::mulAlpha(j == lit ? th.face.signal : ink, 0.55f * a));
          if (hot && j == hoverDot_ && mode_ == Mode::None) p.ring(c, rad + 2.0f, 1.0f, th.face.signal);
        }
      }
      // The row's menu chip.
      const Rect m = menuChip(i);
      const bool mh = hoverMenu_ == i;
      for (int k = -1; k <= 1; ++k)
        p.disc({(m.x0 + m.x1) * 0.5f + 3.5f * static_cast<float>(k), y}, 1.0f, mh ? th.face.signal : ui::mulAlpha(ink, 0.7f));
    }
    if (live && phase_ >= 0.0f) {  // the playhead: the last pulse's place in the cycle
      const float x = r.x0 + std::clamp(phase_, 0.0f, 1.0f) * W;
      p.line({x, r.y0 - 2.0f}, {x, r.y1 + 2.0f}, 1.0f, ui::mulAlpha(th.face.signal, 0.8f));
    }
    // The readouts: the hot row, else what the view is.
    std::string left;
    if (hotRow >= 0 && hotRow < rows) left = rowText(hotRow);
    else left = "Click a dot: mute \xC2\xB7 drag a row: its division \xC2\xB7 Shift+drag: its shift \xC2\xB7 right-click: Euclid, Rotate, Invert";
    p.textAligned({r.x0, 8.0f}, left, 6.5f, hotRow >= 0 ? th.face.signal : ink, 0.0f);
    char s[96];
    if (live) std::snprintf(s, sizeof s, "%u held \xC2\xB7 cycle %d/%d bar", held_, view_.cycleNum, view_.cycleDen);
    else
      std::snprintf(s, sizeof s, "%s \xC2\xB7 cycle %d/%d bar", lastHeld_.empty() ? "hold notes: a row each, lowest at the bottom" : "last held",
                    view_.cycleNum, view_.cycleDen);
    p.textAligned({r.x1, 8.0f}, s, 6.5f, ink, 1.0f);
  }
  bool hit(Vec2 l) const noexcept override { return l.x >= 0.0f && l.y >= 0.0f && l.x <= desc_.w && l.y <= desc_.h; }
  bool interactive() const noexcept override { return true; }
  bool dragging() const noexcept override { return mode_ != Mode::None; }
  void cancelDrag(PanelTarget& t) override {
    if (mode_ == Mode::Division || mode_ == Mode::Shift) t.cancel();
    mode_ = Mode::None;
  }
  void endWheel(PanelTarget& t) override {
    if (!wheel_) return;
    wheel_ = false;
    t.end();
  }
  ui::Cursor cursor() const noexcept override {
    if (mode_ == Mode::Division || mode_ == Mode::Shift) return ui::Cursor::ResizeEW;
    return hoverRow_ >= 0 || hoverMenu_ >= 0 ? ui::Cursor::Pointer : ui::Cursor::Inherit;
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    if (e.kind == K::Leave) {
      const bool changed = hoverRow_ >= 0 || hoverMenu_ >= 0;
      hoverRow_ = hoverDot_ = hoverMenu_ = -1;
      return changed;
    }
    if (mode_ == Mode::None) ensure(t);  // (a press finds the rows of the values now, painted or not)
    if (e.kind == K::Wheel) return wheel(e, t);
    if (e.kind == K::Move && mode_ == Mode::None) {
      const int32_t m = menuAt(e.local), row = m >= 0 ? -1 : rowAt(e.local), dot = row >= 0 ? dotAt(row, e.local.x) : -1;
      const bool changed = m != hoverMenu_ || row != hoverRow_ || dot != hoverDot_;
      hoverMenu_ = m;
      hoverRow_ = row;
      hoverDot_ = dot;
      return changed;
    }
    if (e.kind == K::Down) {
      endWheel(t);
      const int32_t m = menuAt(e.local), row = m >= 0 ? m : rowAt(e.local);
      if (row < 0) return false;
      if (e.button == ui::PointerButton::Right || (m >= 0 && e.button == ui::PointerButton::Left)) {
        openMenu(t, row, e);
        return true;
      }
      if (e.button != ui::PointerButton::Left) return false;
      mode_ = Mode::Pending;
      pressRow_ = row;
      pressDot_ = dotAt(row, e.local.x);
      press_ = e.local;
      pressDev_ = e.device;
      return true;
    }
    if (e.kind == K::Move) {
      if (pressRow_ < 0 || static_cast<size_t>(pressRow_) >= view_.rows.size()) return false;
      const doc::NotePulseRow& row = view_.rows[static_cast<size_t>(pressRow_)];
      if (mode_ == Mode::Pending) {
        if (std::fabs(e.device.x - pressDev_.x) <= 3.0f * std::max(e.uiScale, 1.0f)) return false;
        const bool shift = (e.mods & ui::mod::kShift) != 0;
        const doc::Binding* b = rowBinding(pressRow_, shift ? kShift : kDiv);
        if (b == nullptr || !t.info(*b).valid || !t.info(*b).editable) {
          mode_ = Mode::Locked;
          return true;
        }
        if (!shift && row.locked) {  // Auto: the note's harmonic is its division
          mode_ = Mode::Locked;
          t.notice("Auto: this row pulses at its note's harmonic (switch Auto off to set its division)");
          return true;
        }
        mode_ = shift ? Mode::Shift : Mode::Division;
        startDiv_ = row.division;
        startShift_ = row.shift;
        t.begin(*b, shift ? "Pulse Shift" : "Pulse Division");
      }
      const float dx = e.local.x - press_.x;
      if (mode_ == Mode::Division) {
        const int32_t n = std::clamp(startDiv_ + static_cast<int32_t>(std::lround(dx / kDivPx)), 1, view_.maxDivision);
        setUnits(t, rowBinding(pressRow_, kDiv), n);
        return true;
      }
      if (mode_ == Mode::Shift) {  // the dots follow the pointer: a period is W / n wide
        const Rect r = plot();
        double s = startShift_ + static_cast<double>(dx) / ((r.x1 - r.x0) / static_cast<float>(std::max(startDiv_, 1)));
        if ((e.mods & ui::mod::kAlt) == 0) s = std::round(s * 24.0) / 24.0;
        setUnits(t, rowBinding(pressRow_, kShift), std::clamp(s, -view_.maxShift, view_.maxShift));
        return true;
      }
      return mode_ == Mode::Locked;
    }
    if (e.kind == K::Up && mode_ != Mode::None) {
      const Mode was = mode_;
      mode_ = Mode::None;
      if (was == Mode::Division || was == Mode::Shift) t.end();
      else if (was == Mode::Pending && pressDot_ >= 0) toggle(t, pressRow_, pressDot_);
      return true;
    }
    return false;
  }
  uint64_t liveKey(const PanelTarget& t) const override {
    uint64_t h = hmix(hmix(0x50554C53ull, serial_), inputKey(t));
    h = hmix(h, static_cast<uint64_t>(hoverRow_ + 1) | (static_cast<uint64_t>(hoverDot_ + 1) << 8) |
                    (static_cast<uint64_t>(hoverMenu_ + 1) << 24) | (static_cast<uint64_t>(mode_) << 32) |
                    (static_cast<uint64_t>(pressRow_ + 1) << 36) | (wheel_ ? 1ull << 44 : 0u));
    return h == kAnyChange ? h - 1 : h;
  }
  bool frameTick(PanelTarget& t) override {
    playing_ = t.transportPlaying();
    const NoteStageStatus* st = status_.get(t, binding(kDevice));
    return st != nullptr && read(*st);
  }
  bool ticking() const noexcept override { return status_.s != nullptr && (playing_ || held_ > 0); }
  void describe(const PanelTarget&, std::string& title, std::string& body) const override {
    title = "Pulse";
    body = "The polyrhythm: a row per held note (lowest at the bottom), the Cycle across, a dot per pulse. Every row "
           "cuts the Cycle into its division; with Auto a note of a JI chord pulses at its harmonic (h8, h9 \xE2\x80\xA6: "
           "8 : 9 : 10 : 12 held is that polyrhythm), other notes at their row's division. Click a dot: mute / unmute "
           "that pulse. Drag a row sideways (or the wheel): its division. Shift+drag: shift the whole row against the "
           "others (offbeats, flams; Alt: free). Right-click a row (or the dots at its right end): Euclid k of n, Rotate, Invert, All on, "
           "Reset. Lines join the pulses that land together; the lit dots and the playhead follow playback.";
  }
  // tests
  const doc::NotePulseView& rows(const PanelTarget& t) {
    ensure(t);
    return view_;
  }
  std::optional<Vec2> dotPos(const PanelTarget& t, int32_t row, int32_t j) {
    ensure(t);
    if (row < 0 || static_cast<size_t>(row) >= view_.rows.size() || j < 0 || j >= view_.rows[static_cast<size_t>(row)].division)
      return std::nullopt;
    return Vec2{dotX(row, j), rowY(row)};
  }
  std::optional<Vec2> menuPos(const PanelTarget& t, int32_t row) {
    ensure(t);
    if (row < 0 || static_cast<size_t>(row) >= view_.rows.size()) return std::nullopt;
    const Rect m = menuChip(row);
    return Vec2{(m.x0 + m.x1) * 0.5f, (m.y0 + m.y1) * 0.5f};
  }
  int32_t litDot(int32_t row) const noexcept {
    return held_ > 0 && row >= 0 && static_cast<uint32_t>(row) < NoteStageStatus::kPulseRows ? lit_[static_cast<size_t>(row)] : -1;
  }
  float playhead() const noexcept { return held_ > 0 ? phase_ : -1.0f; }
  uint32_t linkCount() const noexcept { return static_cast<uint32_t>(links_.size()); }
  uint64_t computes() const noexcept { return computes_; }

private:
  enum class Mode : uint8_t { None, Pending, Division, Shift, Locked };
  struct Link {
    float x;
    int32_t lo, hi;
  };
  static constexpr float kDivPx = 12.0f;  // a division step of a row drag

  Rect plot() const noexcept { return {40.0f, 18.0f, desc_.w - 22.0f, desc_.h - 4.0f}; }
  float rowH() const noexcept {
    const Rect r = plot();
    return (r.y1 - r.y0) / static_cast<float>(std::max<size_t>(view_.rows.size(), 1));
  }
  float rowY(int32_t i) const noexcept { return plot().y1 - (static_cast<float>(i) + 0.5f) * rowH(); }
  // Pulse j of row i in the cycle, 0 … 1 (its shift wraps round the cycle), and where it is drawn.
  double dotPhase(int32_t i, int32_t j) const noexcept {
    const doc::NotePulseRow& row = view_.rows[static_cast<size_t>(i)];
    const double u = (static_cast<double>(j) + row.shift) / static_cast<double>(std::max(row.division, 1));
    return u - std::floor(u);
  }
  float dotX(int32_t i, int32_t j) const noexcept {
    const Rect r = plot();
    return r.x0 + static_cast<float>(dotPhase(i, j)) * (r.x1 - r.x0);
  }
  float dotRadius(int32_t i) const noexcept {
    const Rect r = plot();
    const float gap = (r.x1 - r.x0) / static_cast<float>(std::max(view_.rows[static_cast<size_t>(i)].division, 1));
    return std::clamp(std::min(gap * 0.36f, rowH() * 0.3f), 1.0f, 4.5f);
  }
  Rect menuChip(int32_t i) const noexcept {
    const float y = rowY(i), h = std::min(rowH() * 0.5f, 7.0f);
    return {desc_.w - 20.0f, y - h, desc_.w - 2.0f, y + h};
  }
  int32_t rowAt(Vec2 l) const noexcept {
    const Rect r = plot();
    if (view_.rows.empty() || l.y < r.y0 || l.y >= r.y1 || l.x < 0.0f || l.x > r.x1 + 2.0f) return -1;
    const auto i = static_cast<int32_t>(std::floor((r.y1 - l.y) / rowH()));
    return i >= 0 && static_cast<size_t>(i) < view_.rows.size() ? i : -1;
  }
  int32_t menuAt(Vec2 l) const noexcept {
    for (size_t i = 0; i < view_.rows.size(); ++i) {
      const Rect m = menuChip(static_cast<int32_t>(i));
      if (l.x >= m.x0 && l.x <= m.x1 && l.y >= m.y0 - 1.0f && l.y <= m.y1 + 1.0f) return static_cast<int32_t>(i);
    }
    return -1;
  }
  // The dot of row i under x (−1: none): the nearest pulse, within reach.
  int32_t dotAt(int32_t i, float x) const noexcept {
    const Rect r = plot();
    const doc::NotePulseRow& row = view_.rows[static_cast<size_t>(i)];
    const int32_t n = std::max(row.division, 1);
    const float W = r.x1 - r.x0;
    if (W / static_cast<float>(n) < 2.5f || x < r.x0 - 6.0f || x > r.x1 + 6.0f) return -1;
    const double u = static_cast<double>(x - r.x0) / W;
    auto j = static_cast<int32_t>(std::lround(u * n - row.shift));
    j = ((j % n) + n) % n;
    float d = std::fabs(dotX(i, j) - x);
    d = std::min(d, W - d);  // (across the cycle's edge)
    return d <= std::max(dotRadius(i) + 2.0f, std::min(5.0f, W / static_cast<float>(n) * 0.5f)) ? j : -1;
  }
  const doc::Binding* rowBinding(int32_t row, RowSlot s) const noexcept {
    return binding(kFirstRow + static_cast<size_t>(row % static_cast<int32_t>(doc::kNotePulseRows)) * kPerRow + s);
  }
  static void setUnits(PanelTarget& t, const doc::Binding* b, double v) {
    if (b != nullptr && t.info(*b).valid) t.setPosition(*b, positionOf(t, *b, v));
  }
  std::string rowText(int32_t i) const {
    const doc::NotePulseRow& row = view_.rows[static_cast<size_t>(i)];
    int32_t on = 0;
    for (int32_t j = 0; j < row.division; ++j) on += static_cast<int32_t>((row.mask >> (static_cast<uint32_t>(j) & 31u)) & 1u);
    char s[128];
    int k = row.locked ? std::snprintf(s, sizeof s, "Row %d \xC2\xB7 h%d (Auto)", i + 1, row.division)
                       : std::snprintf(s, sizeof s, "Row %d \xC2\xB7 \xC3\xB7%d", i + 1, row.division);
    if (on != row.division) k += std::snprintf(s + k, sizeof s - static_cast<size_t>(k), " \xC2\xB7 %d of %d play", on, row.division);
    if (std::fabs(row.shift) > 1e-6)
      k += std::snprintf(s + k, sizeof s - static_cast<size_t>(k), " \xC2\xB7 shift %+.0f %%", row.shift * 100.0);
    if (!row.held && held_ > 0) std::snprintf(s + k, sizeof s - static_cast<size_t>(k), " \xC2\xB7 no note");
    return s;
  }
  // The row's mask written (both halves: one undo step inside the caller's gesture, or one of its own).
  static void writeMask(PanelTarget& t, const doc::Binding* lo, const doc::Binding* hi, uint32_t mask) {
    setUnits(t, lo, static_cast<double>(mask & 0xFFFFu));
    setUnits(t, hi, static_cast<double>(mask >> 16));
  }
  void toggle(PanelTarget& t, int32_t row, int32_t j) {
    if (row < 0 || static_cast<size_t>(row) >= view_.rows.size()) return;
    const uint32_t bit = static_cast<uint32_t>(j) & 31u;
    const doc::Binding* b = rowBinding(row, bit < 16 ? kMaskLo : kMaskHi);
    if (b == nullptr || !t.info(*b).valid || !t.info(*b).editable) return;
    const uint32_t mask = view_.rows[static_cast<size_t>(row)].mask ^ (1u << bit);
    t.setOnce(*b, positionOf(t, *b, static_cast<double>(bit < 16 ? mask & 0xFFFFu : mask >> 16)), "Pulse Mute");
  }
  bool wheel(const FacePointer& e, PanelTarget& t) {
    if (!e.valueGestures || mode_ != Mode::None || e.wheel.y == 0.0f) return false;
    if ((e.mods & (ui::mod::kCtrl | ui::mod::kAlt | ui::mod::kSuper)) != 0) return false;
    const int32_t row = rowAt(e.local);
    if (row < 0) return false;
    const doc::Binding* b = rowBinding(row, kDiv);
    if (view_.rows[static_cast<size_t>(row)].locked || b == nullptr || !t.info(*b).valid || !t.info(*b).editable) return true;
    if (!wheel_ || wheelRow_ != row) {  // a new run: one gesture until the host ends it (endWheel)
      endWheel(t);
      wheel_ = true;
      wheelRow_ = row;
      wheelDiv_ = view_.rows[static_cast<size_t>(row)].division;
      t.begin(*b, "Pulse Division");
    }
    wheelDiv_ = std::clamp(wheelDiv_ + (e.wheel.y > 0.0f ? 1 : -1), 1, view_.maxDivision);
    setUnits(t, b, wheelDiv_);
    return true;
  }
  void openMenu(PanelTarget& t, int32_t row, const FacePointer& e) {
    ui::Widget* host = t.menuHost();
    const doc::Binding *lo = rowBinding(row, kMaskLo), *hi = rowBinding(row, kMaskHi), *sh = rowBinding(row, kShift);
    if (host == nullptr || lo == nullptr || hi == nullptr || sh == nullptr || !t.info(*lo).valid || !t.info(*lo).editable) return;
    const doc::NotePulseRow r = view_.rows[static_cast<size_t>(row)];
    const int32_t n = std::min(r.division, 32);
    PanelTarget* target = &t;
    const doc::Binding bl = *lo, bh = *hi, bs = *sh;
    auto mask = [target, bl, bh](uint32_t m, const char* label) {
      return [target, bl, bh, m, label] {
        target->begin(bl, label);
        writeMask(*target, &bl, &bh, m);
        target->end();
      };
    };
    // What plays of the first n pulses (the pattern a mask repeats).
    auto same = [n](uint32_t a, uint32_t b) {
      const uint32_t keep = n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1u;
      return (a & keep) == (b & keep);
    };
    std::vector<ui::MenuItem> euclid;
    for (int32_t k = 1; k <= n; ++k) {
      const uint32_t m = doc::notePulseEuclid(k, n);
      ui::MenuItem it = ui::MenuItem::item(std::to_string(k) + " of " + std::to_string(n), mask(m, "Pulse Euclid"));
      it.checked = same(m, r.mask);
      euclid.push_back(std::move(it));
    }
    std::vector<ui::MenuItem> items;
    items.push_back(ui::MenuItem::sub("Euclid", std::move(euclid)));
    items.push_back(ui::MenuItem::item("Rotate Later", mask(doc::notePulseRotate(r.mask, n, 1), "Pulse Rotate")));
    items.push_back(ui::MenuItem::item("Rotate Earlier", mask(doc::notePulseRotate(r.mask, n, -1), "Pulse Rotate")));
    items.push_back(ui::MenuItem::item("Invert", mask(~r.mask, "Pulse Invert")));
    items.push_back(ui::MenuItem::item("All On", mask(0xFFFFFFFFu, "Pulse All On")));
    items.push_back(ui::MenuItem::sep());
    items.push_back(ui::MenuItem::item("Reset Row (all on, no shift)", [target, bl, bh, bs] {
      target->begin(bs, "Pulse Reset");
      writeMask(*target, &bl, &bh, 0xFFFFFFFFu);
      setUnits(*target, &bs, 0.0);
      target->end();
    }));
    (void)ui::openContextMenu(*host, e.device, std::move(items));
  }
  // A key of everything the rows are made of: the device (any of its values), the bound values as the target shows
  // them, the held set.
  uint64_t inputKey(const PanelTarget& t) const {
    const doc::Binding* b = binding(kDevice);
    const doc::Project* pr = t.project();
    if (b == nullptr || pr == nullptr) return 0;
    uint64_t h = hmix(0xB7u, reinterpret_cast<uintptr_t>(doc::findDevice(*pr, b->device).device));
    for (size_t s = kAuto; s < desc_.bindings.size(); ++s)
      if (const doc::Binding* x = binding(s)) {
        const double v = t.position(*x);
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        h = hmix(h, bits);
      }
    for (const doc::NotePatternHeld& n : lastHeld_) {
      uint32_t bits[2] = {};
      std::memcpy(&bits[0], &n.hz, 4);
      std::memcpy(&bits[1], &n.harmonic, 4);
      h = hmix(hmix(h, bits[0]), bits[1]);
    }
    return h;
  }
  void ensure(const PanelTarget& t) {
    const uint64_t k = inputKey(t);
    if (k == key_ && computes_ > 0) return;
    key_ = k;
    ++computes_;
    const doc::Binding* b = binding(kDevice);
    const doc::Project* pr = t.project();
    view_ = b != nullptr && pr != nullptr ? doc::notePulseView(*pr, b->device, lastHeld_) : doc::NotePulseView{};
    // The onsets that coincide across rows (rows too dense to draw: left out).
    links_.clear();
    struct On {
      double x;
      int32_t row;
    };
    std::vector<On> ons;
    for (size_t i = 0; i < view_.rows.size(); ++i) {
      const doc::NotePulseRow& row = view_.rows[i];
      if (row.division > 128) continue;
      for (int32_t j = 0; j < row.division; ++j)
        if (((row.mask >> (static_cast<uint32_t>(j) & 31u)) & 1u) != 0) ons.push_back({dotPhase(static_cast<int32_t>(i), j), static_cast<int32_t>(i)});
    }
    std::sort(ons.begin(), ons.end(), [](const On& a, const On& c) { return a.x < c.x; });
    for (size_t a = 0; a < ons.size();) {
      size_t e = a + 1;
      int32_t lo = ons[a].row, hi = ons[a].row;
      while (e < ons.size() && ons[e].x - ons[a].x < 1e-6) {
        lo = std::min(lo, ons[e].row);
        hi = std::max(hi, ons[e].row);
        ++e;
      }
      if (hi > lo) links_.push_back({static_cast<float>(ons[a].x), lo, hi});
      a = e;
    }
  }
  // A new status (its serial moved): the held set (the rows), each row's last pulse, the playhead.
  bool read(const NoteStageStatus& st) {
    const uint64_t s = st.serial.load(std::memory_order_acquire);
    if (s == serial_) return false;
    serial_ = s;
    NoteStageStatus::Pattern p;
    if (st.readPattern(p)) {
      held_ = p.held;
      if (p.heldCount > 0) {
        lastHeld_.clear();
        for (uint32_t i = 0; i < p.heldCount; ++i) {
          const NoteStageStatus::HeldNote& n = p.heldNotes[i];
          lastHeld_.push_back({n.hz, n.velocity, n.ratio, n.harmonic, n.degree, n.equave, n.tuning, n.noteDegree});
        }
      }
    } else {
      serial_ = 0;  // (a write was in progress: read again next tick)
    }
    for (uint32_t i = 0; i < NoteStageStatus::kPulseRows; ++i) lit_[i] = st.pulse[i].load(std::memory_order_relaxed);
    phase_ = st.pulsePhase.load(std::memory_order_relaxed);
    return true;
  }

  StatusRef status_;
  uint64_t serial_ = 0;
  uint32_t held_ = 0;
  bool playing_ = false;
  std::array<int32_t, NoteStageStatus::kPulseRows> lit_ = [] {
    std::array<int32_t, NoteStageStatus::kPulseRows> a;
    a.fill(-1);
    return a;
  }();
  float phase_ = -1.0f;
  std::vector<doc::NotePatternHeld> lastHeld_;
  doc::NotePulseView view_;
  std::vector<Link> links_;
  uint64_t key_ = 0, computes_ = 0;
  int32_t hoverRow_ = -1, hoverDot_ = -1, hoverMenu_ = -1;
  Mode mode_ = Mode::None;
  int32_t pressRow_ = -1, pressDot_ = -1, startDiv_ = 1;
  double startShift_ = 0.0;
  Vec2 press_{0.0f, 0.0f}, pressDev_{0.0f, 0.0f};
  bool wheel_ = false;
  int32_t wheelRow_ = -1, wheelDiv_ = 1;
};

template <class T>
std::unique_ptr<Element> make(const doc::PanelElement& d) {
  return std::make_unique<T>(d);
}

}  // namespace

void registerArpElements(void (*reg)(std::string_view, std::string_view, std::span<const StyleSpec>, std::span<const std::string_view>,
                                     float, float, ElementFactory)) {
  reg("presets", "Presets", Presets::kStyle, kPresetSlots, 120, 16, &make<Presets>);
  reg("rate", "Rate Menu", RateMenu::kStyle, kRateSlots, 80, 28, &make<RateMenu>);
  reg("steplane", "Step Lane", StepLane::kStyle, kLaneSlots, 600, 44, &make<StepLane>);
  reg("arppattern", "Pattern View", PatternView::kStyle, kPatternSlots, 160, 120, &make<PatternView>);
  reg("arpbend", "Bend View", BendView::kStyle, kBendSlots, 300, 110, &make<BendView>);
  reg("arppulse", "Pulse View", PulseView::kStyle, kPulseSlots, 400, 120, &make<PulseView>);
}

namespace test {

int32_t stepLaneLit(const Element& e) {
  const auto* s = dynamic_cast<const StepLane*>(&e);
  return s != nullptr ? s->litStep() : -2;
}

std::optional<ui::Rect> stepLaneCell(const Element& e, int32_t step) {
  const auto* s = dynamic_cast<const StepLane*>(&e);
  return s != nullptr ? std::optional<ui::Rect>(s->cell(step)) : std::nullopt;
}

int32_t stepLaneRange(const Element& e, const PanelTarget& t) {
  const auto* s = dynamic_cast<const StepLane*>(&e);
  return s != nullptr ? s->shownRange(t) : 0;
}

float stepLaneRowY(const Element& e, const PanelTarget& t, int32_t value) {
  const auto* s = dynamic_cast<const StepLane*>(&e);
  return s != nullptr ? s->rowY(t, value) : 0.0f;
}

bool stepLaneTyping(const Element& e) {
  const auto* s = dynamic_cast<const StepLane*>(&e);
  return s != nullptr && s->typing();
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

std::optional<ui::Vec2> patternNoteEnd(Element& e, const PanelTarget& t, uint32_t note, bool tail) {
  auto* s = dynamic_cast<PatternView*>(&e);
  return s != nullptr ? s->noteEnd(t, note, tail) : std::nullopt;
}

uint32_t pulseRows(Element& e, const PanelTarget& t) {
  auto* s = dynamic_cast<PulseView*>(&e);
  return s != nullptr ? static_cast<uint32_t>(s->rows(t).rows.size()) : 0;
}

PulseRow pulseRow(Element& e, const PanelTarget& t, uint32_t row) {
  auto* s = dynamic_cast<PulseView*>(&e);
  if (s == nullptr || row >= s->rows(t).rows.size()) return {};
  const doc::NotePulseRow& r = s->rows(t).rows[row];
  return {r.division, r.locked, r.held, r.mask, r.shift};
}

std::optional<ui::Vec2> pulseDot(Element& e, const PanelTarget& t, uint32_t row, int32_t j) {
  auto* s = dynamic_cast<PulseView*>(&e);
  return s != nullptr ? s->dotPos(t, static_cast<int32_t>(row), j) : std::nullopt;
}

std::optional<ui::Vec2> pulseMenu(Element& e, const PanelTarget& t, uint32_t row) {
  auto* s = dynamic_cast<PulseView*>(&e);
  return s != nullptr ? s->menuPos(t, static_cast<int32_t>(row)) : std::nullopt;
}

int32_t pulseLit(const Element& e, uint32_t row) {
  const auto* s = dynamic_cast<const PulseView*>(&e);
  return s != nullptr ? s->litDot(static_cast<int32_t>(row)) : -2;
}

float pulsePlayhead(const Element& e) {
  const auto* s = dynamic_cast<const PulseView*>(&e);
  return s != nullptr ? s->playhead() : -2.0f;
}

uint32_t pulseLinks(const Element& e) {
  const auto* s = dynamic_cast<const PulseView*>(&e);
  return s != nullptr ? s->linkCount() : 0;
}

uint64_t pulseComputes(const Element& e) {
  const auto* s = dynamic_cast<const PulseView*>(&e);
  return s != nullptr ? s->computes() : 0;
}

}  // namespace test

}  // namespace pq::views::panel
