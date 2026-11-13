// DT.1 (docs/plans/DT.md "Face"): the TEMPO SLIDER — a horizontal slider for a time control with the note values marked
// at the project's tempo, registered as a generic element kind ("temposlider", the GU control palette) so any module
// with a time (or rate) control can use it; the Delay Tap's whole face.
//
//   slots     0 "time"  the control (its curve maps the knob to seconds; a Hz curve: the markings at 1 / seconds)
//             1 "sync"  optional: a stepped control indexing doc::noteValues() (the synced note value)
//             2 "mode"  optional: a 0 / 1 control (0 the time, 1 the sync drives); with it a small ms / ♪ switch
//             3 "glide" DT.4, optional: a 0 / 1 control (0 Repitch, 1 Crossfade: the Delay Tap's Glide); with it a small
//                       toggle left of the ms / ♪ switch showing the mode (a bend: Repitch, a cross: Crossfade)
//   markings  DT.3 (pq/views/panel/tempo_marks.h): above the groove, note values as reduced fractions of a whole note
//             (1/4, 1/12, 3/16, 1/7 …) whose duration at the tempo (PanelTarget::tempoBpm: the playhead's while
//             playing, else the start marker's) lies inside the curve's range, shown by simplicity while there is room
//             on SCREEN (kTickGapPx by tier between ticks, labels that clear each other): a stretched slider or a closer zoom
//             shows more. Tick length by tier (powers of two longest). Candidates placed through the curve's inverse
//             once per tempo / curve change, the visibility once per width / zoom change (cached). Live while playing:
//             the host's frame tick (Face::frameTick, ≤ 60 Hz while the transport plays) compares the playhead's tempo
//             with the one last drawn and repaints only on a change.
//   readout   below the groove, left: "375 ms" / "1.25 s" (time); in sync mode the note value ("3/16", "1/6") and the
//             seconds it gives at the tempo. DT.4: a time on a marking (snapped, typed or automated there: any candidate,
//             shown or not) names it, "26 ms | 1/7"; a time below kSamplesBelow (10 ms) reads in ms to 0.01 and in
//             samples at the engine's rate (PanelTarget::sampleRate), "3.00 ms | 144 smp" (the delay line's floor of 1
//             sample shown as such). The text shrinks to fit left of the buttons (and is the hover info's body).
//   micro     DT.4 (a seconds curve only): a magnifier toggle (left of the buttons) opens a second bar below — 1 … 256
//             SAMPLES, linear, whole samples: a drag (relative; Shift: fine) snaps to whole samples, its number box takes
//             a typed count (click it, type, Enter: one undo step; Esc cancels). Both set the same time (seconds =
//             samples / rate; one undo step per gesture). Opening it is a panel edit (the element's style field
//             "micro", one "Edit Panel" step): the element and its panel grow by kMicroH (a plate spanning the panel
//             with them, elements below move down), so the block re-lays out and its ports and cables follow.
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
#include <functional>
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
constexpr size_t kTime = 0, kSync = 1, kMode = 2, kGlide = 3;
constexpr std::array<std::string_view, 4> kSlots = {"time", "sync", "mode", "glide"};
constexpr size_t kSyncs = 21;
constexpr float kDefaultW = 168.0f, kDefaultH = 64.0f;
constexpr float kMicroH = 24.0f;         // DT.4: the micro bar's rows (the element and its panel grow by it)
constexpr double kMicroMax = 256.0;      // the micro bar's range: 1 … 256 samples
constexpr double kSamplesBelow = 0.010;  // seconds: shorter times read in samples too

uint64_t mix(uint64_t h, uint64_t v) noexcept { return (h ^ v) * 0x100000001B3ull + (h >> 29); }

std::string secondsText(double s) {
  char b[32];
  if (s >= 1.0 - 1e-9) std::snprintf(b, sizeof b, "%.2f s", s);
  else if (s >= kSamplesBelow) std::snprintf(b, sizeof b, "%.0f ms", s * 1000.0);
  else std::snprintf(b, sizeof b, "%.2f ms", s * 1000.0);
  return b;
}
// A sample count: whole ("144"), or to a tenth when the time lies between samples.
std::string samplesText(double n) {
  char b[32];
  const double r = std::round(n);
  if (std::abs(n - r) < 1e-6) std::snprintf(b, sizeof b, "%.0f", r);
  else std::snprintf(b, sizeof b, "%.1f", n);
  return b;
}

class TempoSlider final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"colour", StyleSpec::Type::Token, 0, "value"},
      {"gravity", StyleSpec::Type::Number, 1.0, {}, {}, 0.0, 1.0},
      {"micro", StyleSpec::Type::Number, 0.0, {}, {}, 0.0, 1.0},  // DT.4: the micro (samples) bar open
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
    // The readout (shrunk to fit left of the buttons).
    const float ry = g.y1 + 12.0f;
    const float room = buttonsLeft(t) - 4.0f - g.x0;
    auto fit = [room](size_t chars) { return std::clamp(room / (static_cast<float>(chars) * 0.56f), 5.0f, kReadoutPx); };
    if (sync) {
      const doc::NoteValue& nv = doc::noteValues()[syncIndex(t)];
      const std::string secs = secondsText(doc::noteValueSeconds(syncIndex(t), m.bpm));
      const float px = fit(nv.label.size() + secs.size() + 1);
      p.textAligned({g.x0, ry}, nv.label, px, accent, 0.0f);
      const float lw = static_cast<float>(nv.label.size()) * px * 0.56f + 6.0f;
      p.textAligned({g.x0 + lw, ry}, secs, px, th.face.ink, 0.0f);
    } else {
      const std::string text = timeReadout(t, m, v);
      p.textAligned({g.x0, ry}, text, fit(text.size()), ok ? accent : th.face.ink, 0.0f);
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
    // DT.4: Repitch / Crossfade (the glyph shows the mode) and the micro bar's magnifier.
    const Rgba off = ui::lerp(th.face.body, th.face.track, 0.6f);
    if (binding(kGlide) != nullptr) {
      const Rect r = glideRect();
      p.roundedRect(r, 3.0f, off);
      glideGlyph(p, r, crossfade(t), th.face.ink);
    }
    if (hasMicro(t)) {
      const Rect r = microRect();
      const bool open = microOpen();
      p.roundedRect(r, 3.0f, open ? ui::mulAlpha(accent, 0.85f) : off);
      magnifierGlyph(p, r, open ? th.face.body : ui::mulAlpha(th.face.ink, 0.8f));
      if (open) paintMicro(p, t, th, accent, ok);
    }
  }

  // DT.4: the readout and the hover info's body.
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    ValueElement::describe(t, title, body);
    if (binding(kTime) == nullptr || !bound(t, kTime).valid) return;
    const Marks& m = marks(t, marks_.ppu > 0.0f ? marks_.ppu : 1.0f);
    if (syncMode(t)) {
      const uint32_t i = syncIndex(t);
      body = std::string(doc::noteValues()[i].label) + " | " + secondsText(doc::noteValueSeconds(i, m.bpm)) + ".";
    } else {
      body = timeReadout(t, m, pos(t, kTime)) + ".";
    }
    body += " Drag: the time (Shift: fine; it sticks to the markings unless the magnet is off).";
    if (binding(kGlide) != nullptr)
      body += crossfade(t) ? " Crossfade: a jump of the time crossfades two read heads (no pitch)."
                           : " Repitch: a change of the time bends the pitch as the read head glides (tape).";
    if (microOpen()) body += " The bar below: whole samples (click the number to type one).";
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
    h = mix(h, std::bit_cast<uint64_t>(t.sampleRate()));
    h = mix(h, typing_ ? std::hash<std::string>{}(text_) | 1u : 0u);
    return h == kAnyChange ? h - 1 : h;
  }

  bool hit(Vec2 l) const noexcept override {
    const Rect g = groove();
    if (l.x >= g.x0 - 5.0f && l.x <= g.x1 + 5.0f && l.y >= g.y0 - 6.0f && l.y <= g.y1 + 5.0f) return true;  // the knob's reach
    if (binding(kGlide) != nullptr && inside(glideRect(), l)) return true;
    if (inside(microRect(), l) || (microOpen() && (inside(microReach(), l) || inside(boxRect(), l)))) return true;
    return (binding(kMode) != nullptr && inside(modeRect(), l)) || inside(magnetRect(), l);
  }

  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    noteGestures(e, t);
    if (!syncMode(t) && wheelStep(e, t)) return true;
    if (e.kind == K::Down) {
      if (typing_) commitTyping(t);
      if (e.button != ui::PointerButton::Left) return false;
      // DT.4: the Glide toggle, the micro bar's magnifier, its number box and its groove.
      if (binding(kGlide) != nullptr && inside(glideRect(), e.local)) {
        if (editable(t, kGlide)) t.setOnce(*binding(kGlide), crossfade(t) ? 0.0 : 1.0, "Delay Glide");
        return true;
      }
      if (hasMicro(t) && inside(microRect(), e.local)) {
        toggleMicro(t);
        return true;
      }
      if (microOpen() && hasMicro(t) && inside(boxRect(), e.local)) {
        if (editable(t, kTime)) {
          typing_ = true;
          text_.clear();
        }
        return true;
      }
      if (microOpen() && hasMicro(t) && inside(microReach(), e.local)) {
        if (!editable(t, kTime)) return false;
        drag_ = true;
        dragMicro_ = true;
        dragSync_ = false;
        startDev_ = e.device;
        lastSamples_ = std::clamp(std::round(samplesOf(t, pos(t, kTime))), 1.0, kMicroMax);
        dragStart_ = lastSamples_;
        t.begin(*binding(kTime), "Delay Samples");
        return true;
      }
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
    if (e.kind == K::Move && drag_ && dragMicro_) {
      const Rect mg = microGroove();
      const double len = std::max(4.0, static_cast<double>(mg.x1 - mg.x0) * e.pxPerUnit);
      const bool fine = (e.mods & ui::mod::kShift) != 0;
      const double moved = (static_cast<double>(e.device.x) - startDev_.x) / len * (kMicroMax - 1.0) * (fine ? 0.1 : 1.0);
      const double n = std::clamp(std::round(dragStart_ + moved), 1.0, kMicroMax);
      if (n != lastSamples_) {
        lastSamples_ = n;
        t.setPosition(*binding(kTime), placeOfSamples(t, n));
      }
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
      dragMicro_ = false;
      endDrag(t);
      return true;
    }
    return false;
  }
  void cancelDrag(PanelTarget& t) override {
    dragMicro_ = false;
    ValueElement::cancelDrag(t);
  }
  // DT.4: the micro bar's type-in (digits; Enter commits one undo step, Esc cancels, a click elsewhere commits).
  bool wantsText() const noexcept override { return typing_; }
  bool onText(const ui::TextEvent& e, PanelTarget&) override {
    if (!typing_) return false;
    for (const char c : e.text())
      if ((c >= '0' && c <= '9') || c == '.') text_ += c;
    return true;
  }
  bool onKey(const ui::KeyEvent& e, PanelTarget& t) override {
    if (typing_) {
      if (!e.down) return true;
      if (e.key == ui::key::kReturn) commitTyping(t);
      else if (e.key == ui::key::kEscape) typing_ = false;
      else if (e.key == ui::key::kBackspace && !text_.empty()) text_.pop_back();
      return true;
    }
    return !syncMode(t) && valueKey(e, t);
  }
  void focusLost(PanelTarget& t) override {
    if (typing_) commitTyping(t);
  }
  ui::Cursor cursor() const noexcept override { return typing_ ? ui::Cursor::Text : ui::Cursor::ResizeEW; }
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
    if (const doc::Binding* b = binding(kGlide)) fn(*b, glideRect());
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
  // DT.4: the Glide toggle and the micro bar's magnifier, left of the ms / ♪ switch (each 14 wide, 4 apart).
  Rect glideRect() const noexcept {
    const Rect g = groove();
    return {desc_.w - 76.0f, g.y1 + 6.0f, desc_.w - 62.0f, g.y1 + 18.0f};
  }
  Rect microRect() const noexcept {
    const Rect g = groove();
    const float x1 = binding(kGlide) != nullptr ? desc_.w - 80.0f : desc_.w - 62.0f;
    return {x1 - 14.0f, g.y1 + 6.0f, x1, g.y1 + 18.0f};
  }
  // The leftmost button's left edge (the readout's room ends 4 before it).
  float buttonsLeft(const PanelTarget& t) const {
    if (hasMicro(t)) return microRect().x0;
    if (binding(kGlide) != nullptr) return glideRect().x0;
    return binding(kMode) != nullptr ? modeRect().x0 : magnetRect().x0;
  }
  // The micro bar (when open): its labels, ticks, groove and number box below the buttons' row.
  float microTop() const noexcept { return groove().y1 + 22.0f; }
  Rect microGroove() const noexcept {
    const float y = microTop() + 18.0f;
    return {groove().x0, y - 2.0f, std::max(groove().x0 + 1.0f, desc_.w - 58.0f), y + 2.0f};
  }
  Rect microReach() const noexcept {
    const Rect mg = microGroove();
    return {mg.x0 - 5.0f, mg.y0 - 6.0f, mg.x1 + 5.0f, mg.y1 + 5.0f};
  }
  Rect boxRect() const noexcept {
    const float y = microTop() + 18.0f;
    return {desc_.w - 52.0f, y - 6.0f, desc_.w - 10.0f, y + 6.0f};
  }
  bool microOpen() const noexcept { return num("micro", 0.0) >= 0.5 && desc_.h >= microTop() + 24.0f; }
  // The micro bar needs a time in seconds (not a rate).
  bool hasMicro(const PanelTarget& t) const {
    const BoundInfo& i = bound(t, kTime);
    return i.valid && i.curve.unit == dsp::CurveUnit::Seconds;
  }
  bool crossfade(const PanelTarget& t) const { return binding(kGlide) != nullptr && pos(t, kGlide) >= 0.5; }
  // The delay in samples at the engine's rate for the time knob at `position` (the delay line's floor: 1).
  double samplesOf(const PanelTarget& t, double position) const {
    return std::max(1.0, dsp::evalCurve(bound(t, kTime).curve, position) * t.sampleRate());
  }
  // The time knob's position for `n` samples: the curve's inverse, refined by bisection so the time is n / rate to the
  // last bit the curve gives (the inverse alone can be an ulp or two off).
  double placeOfSamples(const PanelTarget& t, double n) const {
    const dsp::Curve& c = bound(t, kTime).curve;
    const double want = n / t.sampleRate();
    const double p = placeOf(t, want);
    double lo = std::max(0.0, p - 1e-9), hi = std::min(1.0, p + 1e-9);
    if (!(dsp::evalCurve(c, lo) <= want && dsp::evalCurve(c, hi) >= want)) return p;
    for (int k = 0; k < 64 && lo < hi; ++k) {
      const double mid = 0.5 * (lo + hi);
      if (mid <= lo || mid >= hi) break;
      if (dsp::evalCurve(c, mid) < want) lo = mid;
      else hi = mid;
    }
    const double eLo = std::abs(dsp::evalCurve(c, lo) - want), eHi = std::abs(dsp::evalCurve(c, hi) - want);
    const double eP = std::abs(dsp::evalCurve(c, p) - want);
    return eP <= std::min(eLo, eHi) ? p : eLo <= eHi ? lo : hi;
  }
  // DT.4: the time readout: "375 ms", "26 ms | 1/7" on a marking, "3.00 ms | 144 smp" below kSamplesBelow.
  std::string timeReadout(const PanelTarget& t, const Marks& m, double v) const {
    const BoundInfo& i = bound(t, kTime);
    if (!i.valid) return {};
    std::string out;
    if (i.curve.unit == dsp::CurveUnit::Seconds) {
      const double sr = t.sampleRate();
      double s = dsp::evalCurve(i.curve, v);
      if (s < kSamplesBelow) {
        const double n = std::max(1.0, s * sr);
        s = n / sr;
        out = secondsText(s) + " | " + samplesText(n) + " smp";
      } else {
        out = secondsText(s);
      }
    } else {
      out = readoutText(t, v, kTime);
    }
    if (const int32_t k = markOn(t, m, v); k >= 0) out += " | " + m.list[static_cast<size_t>(k)].label;
    return out;
  }
  // The marking the time is exactly on — any candidate, shown or not (the simplest first) —, −1: none.
  int32_t markOn(const PanelTarget& t, const Marks& m, double v) const {
    const dsp::Curve& c = bound(t, kTime).curve;
    const double s = dsp::evalCurve(c, v);
    const bool hz = c.unit == dsp::CurveUnit::Hz;
    for (size_t k = 0; k < m.list.size(); ++k) {
      const TempoMark& mk = m.list[k];
      const double units = hz ? 1.0 / mk.seconds : mk.seconds;
      if (std::abs(mk.pos - v) < 1e-9 || std::abs(units - s) <= 1e-9 * std::abs(units)) return static_cast<int32_t>(k);
    }
    return -1;
  }
  void commitTyping(PanelTarget& t) {
    typing_ = false;
    double n = 0.0;
    const auto [end, ec] = std::from_chars(text_.data(), text_.data() + text_.size(), n);
    if (ec != std::errc() || end == text_.data() || binding(kTime) == nullptr || !editable(t, kTime)) return;
    const BoundInfo& i = bound(t, kTime);
    const double most = std::max(1.0, std::floor(std::max(i.curve.min, i.curve.max) * t.sampleRate()));
    t.setOnce(*binding(kTime), placeOfSamples(t, std::clamp(std::round(n), 1.0, most)), "Delay Samples");
  }
  void paintMicro(ui::FacePainter& p, const PanelTarget& t, const ui::Theme& th, Rgba accent, bool ok) const {
    const Rect mg = microGroove();
    const float gy = (mg.y0 + mg.y1) * 0.5f, rr = (mg.y1 - mg.y0) * 0.5f;
    auto xOf = [&](double n) { return mg.x0 + (mg.x1 - mg.x0) * static_cast<float>((n - 1.0) / (kMicroMax - 1.0)); };
    p.roundedRect(mg, rr, th.face.track);
    // Ticks every 16 samples, labelled every 64 (and 1).
    for (int n = 0; n <= static_cast<int>(kMicroMax); n += 16) {
      const double at = std::max(1, n);
      const bool major = n % 64 == 0;
      const float x = xOf(at);
      p.line({x, gy - rr - 2.0f - (major ? 5.0f : 3.0f)}, {x, gy - rr - 2.0f}, 1.0f, ui::mulAlpha(th.face.tick, major ? 1.0f : 0.6f));
      if (major) {
        char b[8];
        std::snprintf(b, sizeof b, "%d", static_cast<int>(at));
        p.text({x, gy - rr - 12.0f}, b, kMarkTextPx * 0.85f, th.face.ink);
      }
    }
    const double n = samplesOf(t, pos(t, kTime));
    const bool beyond = n > kMicroMax + 1e-9;
    const float hx = xOf(std::clamp(n, 1.0, kMicroMax));
    if (hx - mg.x0 > 0.2f) p.roundedRect({mg.x0, mg.y0, hx, mg.y1}, rr, ui::mulAlpha(accent, beyond ? 0.35f : 1.0f));
    p.roundedRect({hx - 3.0f, mg.y0 - 4.0f, hx + 3.0f, mg.y1 + 4.0f}, 2.0f, ui::mulAlpha(th.face.body, 0.9f));
    p.roundedRect({hx - 2.0f, mg.y0 - 3.0f, hx + 2.0f, mg.y1 + 3.0f}, 1.5f, beyond ? ui::lerp(th.face.handle, th.face.track, 0.5f) : th.face.handle);
    // The number box: the count (or the type-in).
    const Rect box = boxRect();
    p.roundedRect(box, 2.5f, ui::lerp(th.face.body, th.face.track, 0.6f));
    if (typing_) outlineRect(p, box, 2.5f, 1.0f, th.circuit.selected);
    const std::string text = typing_ ? text_ + "|" : samplesText(n) + " smp";
    p.text({(box.x0 + box.x1) * 0.5f, (box.y0 + box.y1) * 0.5f}, text, kMarkTextPx, typing_ ? th.face.ink : (ok ? accent : th.face.ink));
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

  // DT.4: open / close the micro bar — the element's "micro" field, its height and its panel's (a plate spanning the
  // panel grows with it, elements below it move): one "Edit Panel" step.
  void toggleMicro(PanelTarget& t) {
    const doc::Binding* b = binding(kTime);
    if (b == nullptr) return;
    const doc::PanelElement me = desc_;
    const bool open = num("micro", 0.0) >= 0.5;
    const float dh = open ? -kMicroH : kMicroH;
    (void)t.run([me, open, dh](const doc::Project& p, doc::IdSource&) -> doc::ops::EditResult {
      for (doc::DeviceId g = doc::findDevice(p, me.bindings.front().device).device != nullptr
                                 ? doc::findDevice(p, me.bindings.front().device).device->group
                                 : doc::DeviceId{};
           g.valid();) {
        const doc::Device* d = doc::findDevice(p, g).device;
        if (d == nullptr) break;
        if (d->panel != nullptr)
          if (const doc::PanelElement* e = d->panel->find(me.id); e != nullptr && e->kind == me.kind && e->bindings == me.bindings) {
            auto next = std::make_shared<doc::Panel>(*d->panel);
            const float bottom = e->y + e->h, height = next->height;
            for (doc::PanelElement& x : next->elements) {
              if (x.id == me.id) {
                x.setField(doc::styleNumber("micro", open ? 0.0 : 1.0));
                x.h = std::max(1.0f, x.h + dh);
              } else if (x.y <= 0.5f && x.y + x.h >= height - 0.5f) {
                x.h = std::max(1.0f, x.h + dh);  // a plate spanning the panel
              } else if (x.y >= bottom - 0.5f) {
                x.y = std::max(0.0f, x.y + dh);  // below the slider
              }
            }
            next->height = std::max(1.0f, height + dh);
            for (const doc::PanelElement& x : next->elements) next->height = std::max(next->height, x.y + x.h);
            return doc::ops::setGroupPanel(p, g, std::move(next));
          }
        g = d->group;
      }
      return Error{ErrorCode::InvalidArgument, "The micro bar's panel was not found", {}};
    });
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
  // Repitch: a bend (the pitch glides down and back); Crossfade: two crossing lines.
  static void glideGlyph(ui::FacePainter& p, const Rect& r, bool crossfade, Rgba col) {
    const Vec2 c{(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f};
    if (crossfade) {
      p.line({c.x - 4.0f, c.y - 3.0f}, {c.x + 4.0f, c.y + 3.0f}, 1.2f, col);
      p.line({c.x - 4.0f, c.y + 3.0f}, {c.x + 4.0f, c.y - 3.0f}, 1.2f, col);
      return;
    }
    std::array<Vec2, 9> pts;
    for (size_t i = 0; i < pts.size(); ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(pts.size() - 1);
      pts[i] = {c.x - 4.5f + 9.0f * u, c.y - 3.0f + 6.0f * std::sin(3.14159265f * u)};
    }
    p.polyline(pts, 1.2f, col);
  }
  static void magnifierGlyph(ui::FacePainter& p, const Rect& r, Rgba col) {
    const Vec2 c{(r.x0 + r.x1) * 0.5f - 1.0f, (r.y0 + r.y1) * 0.5f - 1.0f};
    p.arc(c, 2.8f, 0.0f, 2.0f * 3.14159265f, 1.2f, col);
    p.line({c.x + 2.0f, c.y + 2.0f}, {c.x + 4.2f, c.y + 4.2f}, 1.4f, col);
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
  bool dragMicro_ = false;  // DT.4: the drag is the micro bar's (dragStart_ / lastSamples_ in samples)
  double dragStart_ = 0.0;
  double lastSamples_ = 0.0;
  uint32_t lastSync_ = 0;
  bool typing_ = false;  // DT.4: the micro bar's type-in is open
  std::string text_;
};

}  // namespace

void registerTempoSlider(void (*reg)(std::string_view, std::string_view, std::span<const StyleSpec>, std::span<const std::string_view>,
                                     float, float, ElementFactory)) {
  reg("temposlider", "Tempo Slider", TempoSlider::kStyle, kSlots, kDefaultW, kDefaultH,
      [](const doc::PanelElement& d) -> std::unique_ptr<Element> { return std::make_unique<TempoSlider>(d); });
}

}  // namespace pq::views::panel
