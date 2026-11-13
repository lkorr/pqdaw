// DT.1 (docs/plans/DT.md "Face"): the TEMPO SLIDER — a horizontal slider for a time control with the note values marked
// at the project's tempo, registered as a generic element kind ("temposlider", the GU control palette) so any module
// with a time (or rate) control can use it; the Delay Tap's whole face.
//
//   slots     0 "time"  the control (its curve maps the knob to seconds; a Hz curve: the markings at 1 / seconds)
//             1 "sync"  optional: a stepped control indexing doc::noteValues() (the synced note value)
//             2 "mode"  optional: a 0 / 1 control (0 the time, 1 the sync drives); with it a small ms / ♪ switch
//   markings  DT.3 (pq/views/panel/tempo_marks.h): above the groove, note values as reduced fractions of a whole note
//             (1/4, 1/12, 3/16, 1/7 …) whose duration at the tempo (PanelTarget::tempoBpm: the playhead's while
//             playing, else the start marker's) lies inside the curve's range, shown by simplicity while there is room
//             on SCREEN (kTickGapPx by tier between ticks, labels that clear each other): a stretched slider or a closer zoom
//             shows more. Tick length by tier (powers of two longest). Candidates placed through the curve's inverse
//             once per tempo / curve change, the visibility once per width / zoom change (cached). Live while playing:
//             the host's frame tick (Face::frameTick, ≤ 60 Hz while the transport plays) compares the playhead's tempo
//             with the one last drawn and repaints only on a change.
//   readout   below the groove, left: "375 ms" / "1.25 s" (time); in sync mode the note value ("3/16", "1/6") and the
//             seconds it gives at the tempo.
//   gravity   a mouse drag of the time (relative, Shift: fine, as the generic slider) sticks to a SHOWN marking while
//             the knob is within kGravityPx logical px of it — it stays there until the pointer has moved past that
//             zone; where markings are closer than that, each side's zone shrinks to 40 % of the gap (room to stop
//             between). Off with the magnet button (the element's own style field "gravity": per instance, saved with
//             the panel, not automatable — one "Edit Panel" step) and while Shift is held. Automation, wires, the wheel
//             and typed values are never snapped.
//   sync      in sync mode the same groove steps through the note values: the knob sits at the note value's place on
//             the time scale (where its marking is), a drag picks the value nearest the pointer (one undo step per
//             drag); values past the range end sit at its end.
//   hit       DT.3: only the groove (the knob's reach) and the two buttons take presses; the markings, the readout and
//             the plate are the block's (a press there moves it).
//   stretch   DT.3: the element widens with its panel (Element::stretchMinWidth: its registered width, 168): a drag of
//             the block's edge (GroupPanelFace::stretchTo) gives the groove more travel; the buttons stay at its end.
// UI thread only.

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>

#include "elements_internal.h"
#include "pq/document/modules.h"
#include "pq/views/panel/tempo_marks.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

// ---- the markings (tempo_marks.h) -----------------------------------------------------------------------------------

namespace {

uint8_t tierOf(uint32_t n, uint32_t d) noexcept {
  const uint32_t on = n >> std::countr_zero(n), od = d >> std::countr_zero(d);  // odd parts
  if (on == 1 && od == 1) return 0;
  if ((on == 1 && od == 3) || (on == 3 && od == 1)) return 1;
  if (on == 1 && od == 5) return 2;
  if (on == 1 && od == 7) return 3;
  return 4;
}

}  // namespace

std::vector<TempoMark> tempoMarkCandidates(const dsp::Curve& c, double bpm) {
  std::vector<TempoMark> out;
  const double lo = std::min(c.min, c.max), hi = std::max(c.min, c.max);
  const double whole = 4.0 * 60.0 / std::max(bpm, 1e-6);  // seconds
  auto add = [&](uint32_t n, uint32_t d) {
    for (const TempoMark& m : out)
      if (m.num == n && m.den == d) return;
    const double s = whole * static_cast<double>(n) / static_cast<double>(d);
    const double units = c.unit == dsp::CurveUnit::Hz ? 1.0 / s : s;
    if (!(units >= lo && units <= hi)) return;
    TempoMark m;
    m.num = static_cast<uint16_t>(n);
    m.den = static_cast<uint16_t>(d);
    m.tier = tierOf(n, d);
    m.seconds = s;
    m.pos = std::clamp(dsp::curvePosition(c, dsp::curveOutput(c, units), 0.0), 0.0, 1.0);
    m.label = std::to_string(n) + "/" + std::to_string(d);
    out.push_back(std::move(m));
  };
  for (const uint32_t odd : {1u, 3u, 5u, 7u})
    for (uint32_t k = 0; k <= 6; ++k)
      for (uint32_t n = 1; n <= 8; ++n)
        if (const uint32_t d = odd << k; std::gcd(n, d) == 1) add(n, d);
  // Every sync value is a candidate (the sync list's 3/128 lies outside the denominators above).
  for (const doc::NoteValue& v : doc::noteValues()) {
    const std::string_view l = v.label;
    const size_t slash = l.find('/');
    uint32_t n = 0, d = 0;
    if (slash == std::string_view::npos) continue;
    (void)std::from_chars(l.data(), l.data() + slash, n);
    (void)std::from_chars(l.data() + slash + 1, l.data() + l.size(), d);
    if (n > 0 && d > 0 && std::gcd(n, d) == 1) add(n, d);
  }
  // Simplicity: the tier; in tier 1 dotted values (an odd numerator part 3) before triplets; then the numerator, then
  // the denominator.
  std::stable_sort(out.begin(), out.end(), [](const TempoMark& a, const TempoMark& b) {
    if (a.tier != b.tier) return a.tier < b.tier;
    const bool da = a.tier == 1 && (a.num >> std::countr_zero(a.num)) == 3, db = b.tier == 1 && (b.num >> std::countr_zero(b.num)) == 3;
    if (da != db) return da;
    if (a.num != b.num) return a.num < b.num;
    return a.den < b.den;
  });
  return out;
}

float layoutTempoMarks(std::span<TempoMark> marks, float x0, float x1, float width, float pxPerUnit) {
  const float ppu = std::max(1e-6f, pxPerUnit);
  const float text = kMarkTextPx / std::clamp(ppu, 1.0f, 2.0f);
  constexpr float kLabelPad = 1.5f;
  for (size_t i = 0; i < marks.size(); ++i) {
    TempoMark& m = marks[i];
    m.x = x0 + (x1 - x0) * static_cast<float>(m.pos);
    m.half = static_cast<float>(m.label.size()) * text * 0.28f + kLabelPad;
    m.shown = m.labelled = false;
    bool room = true;
    const float gap = kTickGapPx[std::min<size_t>(m.tier, 4)] / ppu;  // panel units
    for (size_t j = 0; j < i && room; ++j) room = !marks[j].shown || std::fabs(marks[j].x - m.x) >= gap;
    if (!room) continue;
    m.shown = true;
    bool clear = m.x - m.half >= 0.0f && m.x + m.half <= width;
    for (size_t j = 0; j < i && clear; ++j)
      clear = !marks[j].labelled || m.x + m.half <= marks[j].x - marks[j].half || m.x - m.half >= marks[j].x + marks[j].half;
    m.labelled = clear;
  }
  return text;
}

namespace {

constexpr float kReadoutPx = 8.0f;
constexpr double kGravityPx = 6.0;  // logical px
constexpr double kGapShare = 0.4;   // at most this share of the gap to the neighbouring marking
constexpr size_t kTime = 0, kSync = 1, kMode = 2;
constexpr std::array<std::string_view, 3> kSlots = {"time", "sync", "mode"};
constexpr size_t kSyncs = 21;
constexpr float kDefaultW = 168.0f, kDefaultH = 64.0f;

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
  float stretchMinWidth() const noexcept override { return kDefaultW; }
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
    const Marks& m = marks(t, p.transform().scale / std::max(1e-3f, env.uiScale));
    drawnBpm_ = m.bpm;
    playing_ = t.transportPlaying();
    const bool ok = editable(t, kTime);
    const bool sync = syncMode(t);
    const Rgba accent = ok ? colour(th, "colour", "value", th.face.value) : ui::lerp(th.face.track, th.face.value, 0.35f);
    const double v = shownPosition(t, m);
    const int32_t on = sync ? -1 : markAt(m, v);
    const std::string_view syncLabel = sync ? doc::noteValues()[syncIndex(t)].label : std::string_view();
    // The markings and their labels (the simplest last: drawn over the others).
    const float ty = gy - rr - 2.0f;
    bool syncDrawn = false;
    for (size_t k = m.list.size(); k-- > 0;) {
      const TempoMark& mk = m.list[k];
      if (!mk.shown) continue;
      const bool lit = sync ? mk.label == syncLabel : static_cast<int32_t>(k) == on;
      syncDrawn = syncDrawn || lit;
      const float len = mk.tier == 0 ? 7.0f : mk.tier == 1 ? 5.0f : 3.5f;
      const Rgba c = lit ? accent : ui::mulAlpha(th.face.tick, mk.tier == 0 ? 1.0f : mk.tier == 1 ? 0.8f : 0.6f);
      p.line({mk.x, ty - len}, {mk.x, ty}, lit ? 1.4f : 1.0f, c);
      if (mk.labelled) p.text({mk.x, gy - rr - 15.0f}, mk.label, m.text, lit ? accent : th.face.ink);
    }
    // In sync mode a value whose marking has no room is still marked where the knob is.
    const float hx = g.x0 + (g.x1 - g.x0) * static_cast<float>(v);
    if (sync && !syncDrawn) p.line({hx, ty - 5.0f}, {hx, ty}, 1.4f, accent);
    // The fill and the handle.
    if (t.driven(*binding()))
      outlineRect(p, {g.x0 - 2.5f, g.y0 - 2.5f, g.x1 + 2.5f, g.y1 + 2.5f}, rr + 2.5f, 1.2f, th.face.valueDriven);
    if (hx - g.x0 > 0.2f) p.roundedRect({g.x0, g.y0, hx, g.y1}, rr, accent);
    const bool hot = env.hovered || drag_;
    const Rgba cap = hot ? ui::lerp(th.face.handle, th.face.value, 0.35f) : th.face.handle;
    p.roundedRect({hx - 3.0f, g.y0 - 4.0f, hx + 3.0f, g.y1 + 4.0f}, 2.0f, ui::mulAlpha(th.face.body, 0.9f));
    p.roundedRect({hx - 2.0f, g.y0 - 3.0f, hx + 2.0f, g.y1 + 3.0f}, 1.5f, on >= 0 ? accent : cap);
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
      p.text({(r.x0 + mid) * 0.5f, (r.y0 + r.y1) * 0.5f}, "ms", kMarkTextPx, sync ? th.face.ink : th.face.body);
      noteGlyph(p, {(mid + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f}, sync ? th.face.body : th.face.ink);
    }
    {
      const Rect r = magnetRect();
      const bool gravity = num("gravity", 1.0) >= 0.5;
      p.roundedRect(r, 3.0f, gravity ? ui::mulAlpha(accent, 0.85f) : ui::lerp(th.face.body, th.face.track, 0.6f));
      magnetGlyph(p, r, gravity ? th.face.body : ui::mulAlpha(th.face.ink, sync ? 0.4f : 0.8f));
    }
  }

  // Live tempo: while the transport plays, each host frame compares the playhead's tempo with the one the markings
  // were drawn at (no work at a steady tempo); a change repaints the element.
  bool frameTick(PanelTarget& t) override {
    playing_ = t.transportPlaying();
    return t.tempoBpm() != drawnBpm_;
  }
  bool ticking() const noexcept override { return playing_; }

  uint64_t liveKey(const PanelTarget& t) const override {
    uint64_t h = Element::liveKey(t);
    if (h == kAnyChange) return h;
    h = mix(h, std::bit_cast<uint64_t>(t.tempoBpm()));
    h = mix(h, drag_ ? 1u : 0u);
    return h == kAnyChange ? h - 1 : h;
  }

  bool hit(Vec2 l) const noexcept override {
    const Rect g = groove();
    if (l.x >= g.x0 - 5.0f && l.x <= g.x1 + 5.0f && l.y >= g.y0 - 6.0f && l.y <= g.y1 + 5.0f) return true;  // the knob's reach
    return (binding(kMode) != nullptr && inside(modeRect(), l)) || inside(magnetRect(), l);
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
      if (!hit(e.local)) return false;
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
      dragStart_ = shownPosition(t, marks(t, ppu(e)));
      lastSync_ = sync ? syncIndex(t) : 0;
      t.begin(*binding(slot), sync ? "Delay Sync" : "Delay Time");
      return true;
    }
    if (e.kind == K::Move && drag_) {
      const Marks& m = marks(t, ppu(e));
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
          // Each side of a shown marking holds the knob for kGravityPx, or 40 % of the gap to the next shown marking on
          // that side where they are closer (so dense markings leave room between them to stop).
          const double zone = kGravityPx * std::max(1e-3, static_cast<double>(e.uiScale)) / len;
          double best = 2.0;
          for (size_t k = 0; k < m.byPos.size(); ++k) {
            const double at = m.list[m.byPos[k]].pos;
            const double below = k > 0 ? at - m.list[m.byPos[k - 1]].pos : 1.0;
            const double above = k + 1 < m.byPos.size() ? m.list[m.byPos[k + 1]].pos - at : 1.0;
            const double d = raw - at;
            const double side = std::min(zone, kGapShare * (d < 0.0 ? below : above));
            if (std::abs(d) <= side && std::abs(d) < best) {
              best = std::abs(d);
              p = at;
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
    const Rect g = groove();
    const Rect track{g.x0 - 5.0f, g.y0 - 6.0f, g.x1 + 5.0f, g.y1 + 5.0f};
    if (const doc::Binding* b = binding(kTime)) fn(*b, track);
    if (const doc::Binding* b = binding(kSync)) fn(*b, track);
    if (const doc::Binding* b = binding(kMode)) fn(*b, modeRect());
  }

private:
  // The layout (element-local): the groove 10 in from each side, its centre 26 down (less on a short element); the
  // markings above it, the readout and the buttons below (the buttons at the right end).
  Rect groove() const noexcept {
    const float gy = std::min(desc_.h - 22.0f, 26.0f);
    return {10.0f, gy - 2.0f, std::max(11.0f, desc_.w - 10.0f), gy + 2.0f};
  }

  struct Marks {
    double bpm = -1.0;
    uint64_t gen = ~0ull;
    float width = -1.0f, ppu = -1.0f;  // the layout's
    float text = kMarkTextPx;
    std::vector<TempoMark> list;   // simplicity order
    std::vector<uint32_t> byPos;   // the shown ones, ascending position
    std::array<double, kSyncs> syncPos{};
    std::array<bool, kSyncs> syncIn{}, syncTail{};  // tail: past the range's end
  };

  static float ppu(const FacePointer& e) noexcept { return e.pxPerUnit / std::max(1e-3f, e.uiScale); }

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
    return static_cast<uint32_t>(std::clamp(v, 0.0, static_cast<double>(kSyncs - 1)));
  }
  double syncPosition(const PanelTarget& t, uint32_t index) const {
    const BoundInfo& i = bound(t, kSync);
    return dsp::curvePosition(i.curve, static_cast<double>(index), pos(t, kSync));
  }
  // The knob's place: the time's position, or the synced note value's place on the time scale.
  double shownPosition(const PanelTarget& t, const Marks& m) const {
    if (!syncMode(t)) return pos(t, kTime);
    const uint32_t i = syncIndex(t);
    return m.syncIn[i] ? m.syncPos[i] : placeOf(t, doc::noteValueSeconds(i, m.bpm));
  }
  // The shown marking the knob is exactly on (its index in the list), −1: none.
  static int32_t markAt(const Marks& m, double v) noexcept {
    for (const uint32_t k : m.byPos)
      if (std::abs(m.list[k].pos - v) < 1e-9) return static_cast<int32_t>(k);
    return -1;
  }
  static uint32_t nearestSync(const Marks& m, double raw) noexcept {
    uint32_t best = doc::kNoteValueQuarter;
    double d = 1e9;
    for (uint32_t i = 0; i < kSyncs; ++i) {
      const double x = m.syncIn[i] ? m.syncPos[i] : (m.syncTail[i] ? 1.0 : -1.0);
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

  // The markings at the tempo (placed once per tempo / curve change) laid out for this width and zoom (once per
  // change of either).
  const Marks& marks(const PanelTarget& t, float pxPerUnit) const {
    const double bpm = t.tempoBpm();
    const BoundInfo& info = bound(t, kTime);
    if (bpm != marks_.bpm || info.generation != marks_.gen) {
      marks_ = Marks{};
      marks_.bpm = bpm;
      marks_.gen = info.generation;
      if (info.valid) {
        const dsp::Curve& c = info.curve;
        marks_.list = tempoMarkCandidates(c, bpm);
        const double lo = std::min(c.min, c.max), hi = std::max(c.min, c.max);
        const auto values = doc::noteValues();
        for (size_t i = 0; i < kSyncs; ++i) {
          const double s = values[i].beats * 60.0 / std::max(bpm, 1e-6);
          const double units = c.unit == dsp::CurveUnit::Hz ? 1.0 / s : s;
          marks_.syncTail[i] = units > hi;
          if (units < lo || units > hi) continue;
          marks_.syncIn[i] = true;
          marks_.syncPos[i] = std::clamp(dsp::curvePosition(c, dsp::curveOutput(c, units), 0.0), 0.0, 1.0);
        }
      }
    }
    if (desc_.w != marks_.width || pxPerUnit != marks_.ppu) {
      marks_.width = desc_.w;
      marks_.ppu = pxPerUnit;
      const Rect g = groove();
      marks_.text = layoutTempoMarks(marks_.list, g.x0, g.x1, desc_.w, pxPerUnit);
      marks_.byPos.clear();
      for (uint32_t k = 0; k < marks_.list.size(); ++k)
        if (marks_.list[k].shown) marks_.byPos.push_back(k);
      std::sort(marks_.byPos.begin(), marks_.byPos.end(), [&](uint32_t a, uint32_t b) { return marks_.list[a].pos < marks_.list[b].pos; });
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
  double drawnBpm_ = -1.0;  // the tempo the live layer was last painted at
  bool playing_ = false;
  bool dragSync_ = false;
  double dragStart_ = 0.0;
  uint32_t lastSync_ = 0;
};

}  // namespace

void registerTempoSlider(void (*reg)(std::string_view, std::string_view, std::span<const StyleSpec>, std::span<const std::string_view>,
                                     float, float, ElementFactory)) {
  reg("temposlider", "Tempo Slider", TempoSlider::kStyle, kSlots, kDefaultW, kDefaultH,
      [](const doc::PanelElement& d) -> std::unique_ptr<Element> { return std::make_unique<TempoSlider>(d); });
}

}  // namespace pq::views::panel
