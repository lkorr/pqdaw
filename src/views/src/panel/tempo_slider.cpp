// DT.1 (docs/plans/DT.md "Face"): the TEMPO SLIDER — a horizontal slider for a time control with the note values marked
// at the project's tempo, registered as a generic element kind ("temposlider", the GU control palette) so any module
// with a time (or rate) control can use it; the Delay Tap's whole face.
//
//   slots     0 "time"  the control (its curve maps the knob to seconds; a Hz curve: the markings at 1 / seconds)
//             1 "sync"  optional: a stepped control indexing doc::noteValues() (the synced note value)
//             2 "mode"  optional: a 0 / 1 control (0 the time, 1 the sync drives); with it a small ms / ♪ switch
//   markings  above the groove, one tick per note value (doc::noteValues(), 1/64T … 1/1.) whose duration at the tempo
//             (PanelTarget::tempoBpm: the playhead's while playing, else the start marker's) lies inside the curve's
//             range: straight values 1/32 … 1/1 long and labelled ("1/4"), the others (1/64, dotted, triplet) short and
//             unlabelled; a label that would collide with a longer one's is hidden (the shortest values first). Placed
//             through the curve's inverse once per tempo / curve change (cached), drawn per frame.
//   readout   below the groove, left: "375 ms" / "1.25 s" (time); in sync mode the note value ("1/8.", "1/4T") and
//             the seconds it gives at the tempo.
//   gravity   a mouse drag of the time (relative, Shift: fine, as the generic slider) sticks to a marking while the knob
//             is within kGravityPx device px of it — it stays there until the pointer has moved past that zone; where
//             markings are closer than that, each side's zone shrinks to 40 % of the gap (room to stop between). Off
//             with the magnet button (the element's own style field "gravity": per instance, saved with the panel, not
//             automatable — one "Edit Panel" step) and while Shift is held. Automation, wires, the wheel and typed
//             values are never snapped.
//   sync      in sync mode the same groove steps through the note values: the knob sits at the note value's place on
//             the time scale (where its marking is), a drag picks the marking nearest the pointer (one undo step per
//             drag); values past the range end sit at its end.
// UI thread only.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <string>

#include "elements_internal.h"
#include "pq/document/modules.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

constexpr float kTextPx = 7.0f, kReadoutPx = 8.0f;
constexpr double kGravityPx = 6.0;  // device px at ui scale 1
constexpr double kGapShare = 0.4;   // at most this share of the gap to the neighbouring marking
constexpr size_t kTime = 0, kSync = 1, kMode = 2;
constexpr std::array<std::string_view, 3> kSlots = {"time", "sync", "mode"};
constexpr size_t kMarks = 21;

uint64_t mix(uint64_t h, uint64_t v) noexcept { return (h ^ v) * 0x100000001B3ull + (h >> 29); }

std::string secondsText(double s) {
  char b[32];
  if (s >= 1.0 - 1e-9) std::snprintf(b, sizeof b, "%.2f s", s);
  else std::snprintf(b, sizeof b, "%.0f ms", s * 1000.0);
  return b;
}

class TempoSlider final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"colour", StyleSpec::Type::Token, 0, "value"},
      {"gravity", StyleSpec::Type::Number, 1.0, {}, {}, 0.0, 1.0},
  };

  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return th.face.track; }
  void paintStatic(ui::FacePainter& p, uint32_t) const override {
    const Rect g = groove();
    p.roundedRect(g, (g.y1 - g.y0) * 0.5f, ui::rgba(255, 255, 255));
  }

  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const Rect g = groove();
    const float gy = (g.y0 + g.y1) * 0.5f, rr = (g.y1 - g.y0) * 0.5f;
    const Marks& m = marks(t);
    const bool ok = editable(t, kTime);
    const bool sync = syncMode(t);
    const Rgba accent = ok ? colour(th, "colour", "value", th.face.value) : ui::lerp(th.face.track, th.face.value, 0.35f);
    const double v = shownPosition(t, m);
    const int32_t on = markAt(m, v);
    const int32_t chosen = sync ? static_cast<int32_t>(syncIndex(t)) : on;
    // The markings and their labels.
    for (size_t i = 0; i < kMarks; ++i) {
      if (!m.in[i]) continue;
      const float x = g.x0 + (g.x1 - g.x0) * static_cast<float>(m.pos[i]);
      const bool lit = static_cast<int32_t>(i) == chosen;
      const Rgba c = lit ? accent : ui::mulAlpha(th.face.tick, m.longTick[i] ? 1.0f : 0.7f);
      p.line({x, gy - rr - 2.0f - (m.longTick[i] ? 7.0f : 4.0f)}, {x, gy - rr - 2.0f}, lit ? 1.4f : 1.0f, c);
      if (m.labelled[i]) p.text({x, gy - rr - 15.0f}, doc::noteValues()[i].label, kTextPx, lit ? accent : th.face.ink);
    }
    // The fill and the handle.
    const float hx = g.x0 + (g.x1 - g.x0) * static_cast<float>(v);
    if (t.driven(*binding()))
      outlineRect(p, {g.x0 - 2.5f, g.y0 - 2.5f, g.x1 + 2.5f, g.y1 + 2.5f}, rr + 2.5f, 1.2f, th.face.valueDriven);
    if (hx - g.x0 > 0.2f) p.roundedRect({g.x0, g.y0, hx, g.y1}, rr, accent);
    const bool hot = env.hovered || drag_;
    const Rgba cap = hot ? ui::lerp(th.face.handle, th.face.value, 0.35f) : th.face.handle;
    p.roundedRect({hx - 3.0f, g.y0 - 4.0f, hx + 3.0f, g.y1 + 4.0f}, 2.0f, ui::mulAlpha(th.face.body, 0.9f));
    p.roundedRect({hx - 2.0f, g.y0 - 3.0f, hx + 2.0f, g.y1 + 3.0f}, 1.5f, on >= 0 && !sync ? accent : cap);
    // The readout.
    const float ry = g.y1 + 12.0f;
    if (sync) {
      const doc::NoteValue& nv = doc::noteValues()[syncIndex(t)];
      p.textAligned({g.x0, ry}, nv.label, kReadoutPx, accent, 0.0f);
      const float lw = static_cast<float>(nv.label.size()) * kReadoutPx * 0.56f + 6.0f;
      p.textAligned({g.x0 + lw, ry}, secondsText(doc::noteValueSeconds(syncIndex(t), m.bpm)), kReadoutPx, th.face.ink, 0.0f);
    } else {
      p.textAligned({g.x0, ry}, readoutText(t, v, kTime), kReadoutPx, ok ? accent : th.face.ink, 0.0f);
    }
    // The ms / ♪ switch (with a mode binding) and the magnet.
    if (binding(kMode) != nullptr) {
      const Rect r = modeRect();
      const float mid = (r.x0 + r.x1) * 0.5f;
      p.roundedRect(r, 3.0f, ui::lerp(th.face.body, th.face.track, 0.6f));
      p.roundedRect(sync ? Rect{mid, r.y0, r.x1, r.y1} : Rect{r.x0, r.y0, mid, r.y1}, 3.0f, ui::mulAlpha(accent, 0.85f));
      p.text({(r.x0 + mid) * 0.5f, (r.y0 + r.y1) * 0.5f}, "ms", kTextPx, sync ? th.face.ink : th.face.body);
      noteGlyph(p, {(mid + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f}, sync ? th.face.body : th.face.ink);
    }
    {
      const Rect r = magnetRect();
      const bool gravity = num("gravity", 1.0) >= 0.5;
      p.roundedRect(r, 3.0f, gravity ? ui::mulAlpha(accent, 0.85f) : ui::lerp(th.face.body, th.face.track, 0.6f));
      magnetGlyph(p, r, gravity ? th.face.body : ui::mulAlpha(th.face.ink, sync ? 0.4f : 0.8f));
    }
  }

  uint64_t liveKey(const PanelTarget& t) const override {
    uint64_t h = Element::liveKey(t);
    if (h == kAnyChange) return h;
    h = mix(h, std::bit_cast<uint64_t>(t.tempoBpm()));
    h = mix(h, drag_ ? 1u : 0u);
    return h == kAnyChange ? h - 1 : h;
  }

  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    noteGestures(e, t);
    if (!syncMode(t) && wheelStep(e, t)) return true;
    if (e.kind == K::Down) {
      if (e.button != ui::PointerButton::Left) return false;
      if (binding(kMode) != nullptr && inside(modeRect(), e.local)) {
        if (editable(t, kMode)) {
          const double to = e.local.x >= (modeRect().x0 + modeRect().x1) * 0.5f ? 1.0 : 0.0;
          if (to != (syncMode(t) ? 1.0 : 0.0)) t.setOnce(*binding(kMode), to, "Delay Mode");
        }
        return true;
      }
      if (inside(magnetRect(), e.local)) {
        toggleGravity(t);
        return true;
      }
      const bool sync = syncMode(t);
      const size_t slot = sync ? kSync : kTime;
      if (binding(slot) == nullptr || !editable(t, slot)) return false;
      if (e.clicks >= 2 && !sync) {
        t.reset(*binding(kTime));
        return true;
      }
      drag_ = true;
      dragSync_ = sync;
      startDev_ = e.device;
      dragStart_ = shownPosition(t, marks(t));
      lastSync_ = sync ? syncIndex(t) : 0;
      t.begin(*binding(slot), sync ? "Delay Sync" : "Delay Time");
      return true;
    }
    if (e.kind == K::Move && drag_) {
      const Marks& m = marks(t);
      const Rect g = groove();
      const double len = std::max(4.0, static_cast<double>(g.x1 - g.x0) * e.pxPerUnit);
      const bool fine = (e.mods & ui::mod::kShift) != 0;
      const double raw = std::clamp(dragStart_ + (static_cast<double>(e.device.x) - startDev_.x) / len * (fine ? 0.1 : 1.0), 0.0, 1.0);
      if (dragSync_) {
        const uint32_t i = nearestSync(m, raw);
        if (i != lastSync_) {
          lastSync_ = i;
          t.setPosition(*binding(kSync), syncPosition(t, i));
        }
      } else {
        double p = raw;
        if (!fine && num("gravity", 1.0) >= 0.5) {
          // Each side of a marking holds the knob for kGravityPx, or 40 % of the gap to the next marking on that side
          // where they are closer (so dense markings leave room between them to stop).
          const double zone = kGravityPx * std::max(1e-3, static_cast<double>(e.uiScale)) / len;
          double best = 2.0;
          for (size_t i = 0; i < kMarks; ++i) {
            if (!m.in[i]) continue;
            const double d = raw - m.pos[i];
            const double side = std::min(zone, kGapShare * (d < 0.0 ? m.gapBelow[i] : m.gapAbove[i]));
            if (std::abs(d) <= side && std::abs(d) < best) {
              best = std::abs(d);
              p = m.pos[i];
            }
          }
        }
        t.setPosition(*binding(kTime), p);
      }
      return true;
    }
    if (e.kind == K::Up && drag_) {
      endDrag(t);
      return true;
    }
    return false;
  }
  bool onKey(const ui::KeyEvent& e, PanelTarget& t) override { return !syncMode(t) && valueKey(e, t); }
  ui::Cursor cursor() const noexcept override { return ui::Cursor::ResizeEW; }
  std::optional<ControlAnchor> anchor() const override {
    const Rect g = groove();
    return ControlAnchor{{(g.x0 + g.x1) * 0.5f, (g.y0 + g.y1) * 0.5f}, 0.0f, desc_.w * 0.5f};
  }
  void visitControls(const ControlVisitor& fn) const override {
    if (const doc::Binding* b = binding(kTime)) fn(*b, {0.0f, 0.0f, desc_.w, magnetRect().y0});
    if (const doc::Binding* b = binding(kSync)) fn(*b, {0.0f, 0.0f, desc_.w, magnetRect().y0});
    if (const doc::Binding* b = binding(kMode)) fn(*b, modeRect());
  }

private:
  // The layout (element-local): the groove 10 in from each side, its centre 26 down (less on a short element); the
  // markings above it, the readout and the buttons below.
  Rect groove() const noexcept {
    const float gy = std::min(desc_.h - 22.0f, 26.0f);
    return {10.0f, gy - 2.0f, std::max(11.0f, desc_.w - 10.0f), gy + 2.0f};
  }

  struct Marks {
    double bpm = -1.0;
    uint64_t gen = ~0ull;
    std::array<double, kMarks> pos{};
    std::array<double, kMarks> gapBelow{}, gapAbove{};  // to the nearest marking on each side (1: none)
    std::array<bool, kMarks> in{}, tail{}, longTick{}, labelled{};  // tail: past the range's end
  };

  Rect modeRect() const noexcept {
    const Rect g = groove();
    return {desc_.w - 58.0f, g.y1 + 6.0f, desc_.w - 28.0f, g.y1 + 18.0f};
  }
  Rect magnetRect() const noexcept {
    const Rect g = groove();
    return {desc_.w - 24.0f, g.y1 + 6.0f, desc_.w - 10.0f, g.y1 + 18.0f};
  }
  static bool inside(const Rect& r, Vec2 p) noexcept { return p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1; }

  bool syncMode(const PanelTarget& t) const { return binding(kMode) != nullptr && binding(kSync) != nullptr && pos(t, kMode) >= 0.5; }
  uint32_t syncIndex(const PanelTarget& t) const {
    const BoundInfo& i = bound(t, kSync);
    if (!i.valid) return doc::kNoteValueQuarter;
    const double v = std::round(dsp::evalCurve(i.curve, pos(t, kSync)));
    return static_cast<uint32_t>(std::clamp(v, 0.0, static_cast<double>(kMarks - 1)));
  }
  double syncPosition(const PanelTarget& t, uint32_t index) const {
    const BoundInfo& i = bound(t, kSync);
    return dsp::curvePosition(i.curve, static_cast<double>(index), pos(t, kSync));
  }
  // The knob's place: the time's position, or the synced note value's place on the time scale.
  double shownPosition(const PanelTarget& t, const Marks& m) const {
    if (!syncMode(t)) return pos(t, kTime);
    const uint32_t i = syncIndex(t);
    return m.in[i] ? m.pos[i] : placeOf(t, doc::noteValueSeconds(i, m.bpm));
  }
  int32_t markAt(const Marks& m, double v) const noexcept {
    for (size_t i = 0; i < kMarks; ++i)
      if (m.in[i] && std::abs(m.pos[i] - v) < 1e-9) return static_cast<int32_t>(i);
    return -1;
  }
  uint32_t nearestSync(const Marks& m, double raw) const noexcept {
    uint32_t best = doc::kNoteValueQuarter;
    double d = 1e9;
    for (uint32_t i = 0; i < kMarks; ++i) {
      const double x = m.in[i] ? m.pos[i] : (m.tail[i] ? 1.0 : -1.0);
      if (x < 0.0) continue;
      if (std::abs(x - raw) < d - 1e-12) {  // ties (several values past the end): the shortest
        d = std::abs(x - raw);
        best = i;
      }
    }
    return best;
  }
  // The time knob's position for `seconds` (a Hz curve: 1 / seconds), clamped to the travel.
  double placeOf(const PanelTarget& t, double seconds) const {
    const dsp::Curve& c = bound(t, kTime).curve;
    const double units = c.unit == dsp::CurveUnit::Hz ? 1.0 / seconds : seconds;
    return std::clamp(dsp::curvePosition(c, dsp::curveOutput(c, units), 0.0), 0.0, 1.0);
  }

  const Marks& marks(const PanelTarget& t) const {
    const double bpm = t.tempoBpm();
    const BoundInfo& info = bound(t, kTime);
    if (bpm == marks_.bpm && info.generation == marks_.gen) return marks_;
    marks_ = Marks{};
    marks_.bpm = bpm;
    marks_.gen = info.generation;
    if (!info.valid) return marks_;
    const dsp::Curve& c = info.curve;
    const double lo = std::min(c.min, c.max), hi = std::max(c.min, c.max);
    const auto values = doc::noteValues();
    for (size_t i = 0; i < kMarks; ++i) {
      const double s = values[i].beats * 60.0 / std::max(bpm, 1e-6);
      const double units = c.unit == dsp::CurveUnit::Hz ? 1.0 / s : s;
      marks_.tail[i] = units > hi;
      if (units < lo || units > hi) continue;
      marks_.in[i] = true;
      marks_.pos[i] = std::clamp(dsp::curvePosition(c, dsp::curveOutput(c, units), 0.0), 0.0, 1.0);
      marks_.longTick[i] = values[i].kind == doc::NoteValueKind::Straight && values[i].beats >= 0.125 - 1e-12;
    }
    for (size_t i = 0; i < kMarks; ++i) {
      marks_.gapBelow[i] = marks_.gapAbove[i] = 1.0;
      for (size_t j = 0; j < kMarks; ++j) {
        if (j == i || !marks_.in[i] || !marks_.in[j]) continue;
        const double d = marks_.pos[j] - marks_.pos[i];
        if (d > 0.0) marks_.gapAbove[i] = std::min(marks_.gapAbove[i], d);
        if (d < 0.0) marks_.gapBelow[i] = std::min(marks_.gapBelow[i], -d);
      }
    }
    // Labels: the straight values, longest first; one that would overlap a placed one is hidden.
    const Rect g = groove();
    std::array<std::pair<float, float>, kMarks> placed{};
    size_t n = 0;
    for (size_t k = kMarks; k-- > 0;) {
      if (!marks_.longTick[k]) continue;
      const float x = g.x0 + (g.x1 - g.x0) * static_cast<float>(marks_.pos[k]);
      const float half = static_cast<float>(values[k].label.size()) * kTextPx * 0.28f + 1.5f;
      bool clear = x - half >= 0.0f && x + half <= desc_.w;
      for (size_t j = 0; j < n && clear; ++j) clear = x + half <= placed[j].first || x - half >= placed[j].second;
      if (!clear) continue;
      marks_.labelled[k] = true;
      placed[n++] = {x - half, x + half};
    }
    return marks_;
  }

  void toggleGravity(PanelTarget& t) {
    const doc::Binding* b = binding(kTime);
    if (b == nullptr) return;
    const doc::PanelElement me = desc_;
    const double to = num("gravity", 1.0) >= 0.5 ? 0.0 : 1.0;
    (void)t.run([me, to](const doc::Project& p, doc::IdSource&) -> doc::ops::EditResult {
      // The group whose panel holds this element: the bound block's, or a group round it (a panel the element was
      // copied into).
      for (doc::DeviceId g = doc::findDevice(p, me.bindings.front().device).device != nullptr
                                 ? doc::findDevice(p, me.bindings.front().device).device->group
                                 : doc::DeviceId{};
           g.valid();) {
        const doc::Device* d = doc::findDevice(p, g).device;
        if (d == nullptr) break;
        if (d->panel != nullptr)
          if (const doc::PanelElement* e = d->panel->find(me.id); e != nullptr && e->kind == me.kind && e->bindings == me.bindings) {
            doc::PanelElement copy = *e;
            copy.setField(doc::styleNumber("gravity", to));
            return doc::ops::setPanelElement(p, g, std::move(copy));
          }
        g = d->group;
      }
      return Error{ErrorCode::InvalidArgument, "The magnet's panel was not found", {}};
    });
  }

  static void noteGlyph(ui::FacePainter& p, Vec2 c, Rgba col) {
    p.disc({c.x - 1.4f, c.y + 2.4f}, 1.9f, col);
    p.line({c.x + 0.4f, c.y + 2.4f}, {c.x + 0.4f, c.y - 3.6f}, 0.9f, col);
    p.line({c.x + 0.4f, c.y - 3.6f}, {c.x + 2.6f, c.y - 1.6f}, 0.9f, col);
  }
  static void magnetGlyph(ui::FacePainter& p, const Rect& r, Rgba col) {
    const Vec2 c{(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f - 0.5f};
    constexpr float kR = 3.0f, kPi = 3.14159265f;
    p.arc(c, kR, 0.0f, kPi, 1.6f, col);  // the U (its bow down)
    p.line({c.x - kR, c.y}, {c.x - kR, c.y - 3.0f}, 1.6f, col);
    p.line({c.x + kR, c.y}, {c.x + kR, c.y - 3.0f}, 1.6f, col);
  }

  mutable Marks marks_;
  bool dragSync_ = false;
  double dragStart_ = 0.0;
  uint32_t lastSync_ = 0;
};

}  // namespace

void registerTempoSlider(void (*reg)(std::string_view, std::string_view, std::span<const StyleSpec>, std::span<const std::string_view>,
                                     float, float, ElementFactory)) {
  reg("temposlider", "Tempo Slider", TempoSlider::kStyle, kSlots, 168, 64,
      [](const doc::PanelElement& d) -> std::unique_ptr<Element> { return std::make_unique<TempoSlider>(d); });
}

}  // namespace pq::views::panel
