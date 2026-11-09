// TX.mixer-look (the user's bug report 2026-11-03: "mixer needs to also look heady and cool"): a track's mixer in the
// project patch drawn as a FACE, in the meta faces' family (a dark body, quiet tracks and frame, values in the accent,
// soft glows) instead of a plain box with bars on it. Hosted by the canvas like a device panel (the view's panelMaker
// makes it for a mixer node's SceneFace; pq/views/meta/meta_face.h Face): drawn under the cables, its static parts as
// coverage tiles, its controls hit-tested and dragged by the canvas's face routing.
//
//   header   the track's colour, "MIXER" ("MASTER"), and the M (the track activator: lit while the track plays) and S
//            (solo; Ctrl+click additive) pills — none on the master.
//   pan      a knob (±135°, its value arc from 12 o'clock); drag up / down (Shift: fine), double-click: centre.
//            Above it the pan law's tag (TX.pan-laws: BAL, -3 dB, -6 dB, STEREO): a click opens the law menu.
//   volume   a horizontal fader on Live's fader law (views/mixer: 0 dB at 0.708) with ticks at +6 … −48 dB (0 dB the
//            long one) and the level in the accent; drag sideways (Shift: fine), double-click: 0 dB. The readout under
//            it, the pan's under the knob.
//   sends    one row per send under the main section, level with its pin on the right edge: the return's letter and a
//            fader in the signal colour (expanded: longer, with the level in dB).
// The jacks are the host's pins (`in` on the left, `out` and the sends on the right). A press on the face outside its
// controls is the block's (the canvas moves the mixer: ProjectCanvas's Drag::MoveMixer). Every edit goes through the
// view (one undo step per click or drag: its value gestures). Values are read from the document at each paint (a
// mixer change repaints, it never rebuilds the scene).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "internal.h"
#include "pq/document/model.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/theme.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/mixer/mixer.h"

namespace pq::views::project_patch {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// Geometry (face-local world units; the width and height are the node's).
constexpr float kR = 8.0f;          // the body's corner radius
constexpr float kHeadH = 18.0f;     // the header strip
constexpr Vec2 kPanC{27.0f, 45.0f};  // the pan knob
constexpr float kPanR = 11.0f, kPanW = 3.0f;
constexpr float kPanSweep = 0.75f * kPi;  // ±135° from 12 o'clock
constexpr float kFx0 = 52.0f, kFy = 41.0f;  // the volume fader's groove (left end, centre line); right end: w − 14
constexpr float kFaderInsetR = 14.0f;
constexpr float kMeterY0 = 27.0f, kMeterY1 = 30.5f;  // the level meter's L / R bars over the fader, on its scale
constexpr float kSx0 = 24.0f;  // a send row's fader (left end); right end: w − 14 (expanded: w − 50, the level after it)
constexpr float kLabelPx = 7.5f, kValuePx = 8.5f;

enum Part : int32_t { kMute = 0, kSolo = 1, kPan = 2, kVolume = 3, kSend0 = 4, kPanLaw = 1000 /* after any send */ };
constexpr Rect kLawRect{8.0f, 20.0f, 46.0f, 28.5f};  // TX.pan-laws: the law's tag, above the knob

float sendY(size_t k) noexcept { return metrics::kMixerSendTop + metrics::kPinPitch * static_cast<float>(k); }
float panAngle(float pan) noexcept { return -0.5f * kPi + kPanSweep * std::clamp(pan, -1.0f, 1.0f); }
Vec2 polar(Vec2 c, float r, float a) noexcept { return {c.x + r * std::cos(a), c.y + r * std::sin(a)}; }
bool in(const Rect& r, Vec2 p) noexcept { return p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1; }
uint64_t mix(uint64_t h, uint64_t v) noexcept {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h * 0xBF58476D1CE4E5B9ull;
}

// A rounded rect's outline (the painter fills rounded rects only): its four edges and four corner arcs.
void outline(ui::FacePainter& p, const Rect& r, float rad, float t, Rgba col) {
  if (ui::alphaOf(col) == 0) return;
  p.line({r.x0 + rad, r.y0}, {r.x1 - rad, r.y0}, t, col);
  p.line({r.x0 + rad, r.y1}, {r.x1 - rad, r.y1}, t, col);
  p.line({r.x0, r.y0 + rad}, {r.x0, r.y1 - rad}, t, col);
  p.line({r.x1, r.y0 + rad}, {r.x1, r.y1 - rad}, t, col);
  p.arc({r.x0 + rad, r.y0 + rad}, rad, kPi, 1.5f * kPi, t, col);
  p.arc({r.x1 - rad, r.y0 + rad}, rad, -0.5f * kPi, 0.0f, t, col);
  p.arc({r.x1 - rad, r.y1 - rad}, rad, 0.0f, 0.5f * kPi, t, col);
  p.arc({r.x0 + rad, r.y1 - rad}, rad, 0.5f * kPi, kPi, t, col);
}

// The hosts poll a face's voice reader (≤ 60 Hz while live: the Spectrum / Scope faces' pattern); this one stands for
// the track's level meter (the view's MeterSource).
class MeterTap final : public VoiceTapReader {
public:
  explicit MeterTap(ProjectPatchView& view) : view_(view) {}
  uint32_t read(std::span<FaceVoice>) override { return 0; }
  uint64_t serial() const noexcept override { return serial_; }
  bool live() const noexcept override { return view_.meterSource() != nullptr && view_.meterSource()->live(); }
  uint64_t serial_ = 0;

private:
  ProjectPatchView& view_;
};

class MixerFace final : public Face {
public:
  MixerFace(ProjectPatchView& view, doc::TrackId track, Vec2 size, bool expanded, bool master, std::vector<MixerSend> sends)
      : view_(view), track_(track), size_(size), expanded_(expanded), master_(master), sends_(std::move(sends)), tap_(view) {}

  Vec2 worldSize() const noexcept override { return size_; }

  // TL.3: where its control of `p` is drawn now (face-local): the pan knob, the volume fader's cap, a send's handle.
  std::optional<ControlAnchor> anchorOf(const doc::ParamRef& p) const noexcept {
    const doc::Track* t = track();
    if (t == nullptr || p.owner != track_.value) return std::nullopt;
    switch (p.kind) {
      case doc::ParamKind::TrackPan: return ControlAnchor{kPanC, kPanR + 0.5f * kPanW};
      case doc::ParamKind::TrackVolume: return ControlAnchor{{faderX(t->mixer.volumeDb), kFy}, 6.0f};
      case doc::ParamKind::TrackSend:
        for (size_t k = 0; k < sends_.size(); ++k)
          if (sends_[k].target.value == p.target) return ControlAnchor{{sendX(sendDb(*t, k)), sendY(k)}, 3.4f};
        return std::nullopt;
      default: return std::nullopt;
    }
  }

  // ---- the level meter: polled by the host while the view's meter source is live ----
  VoiceTapReader* voices() const noexcept override { return view_.meterSource() != nullptr ? &tap_ : nullptr; }
  bool pollVoices() override {
    MeterSource* m = view_.meterSource();
    if (m == nullptr) return false;
    const doc::TrackId ids[1] = {track_};
    ui::MeterLevels l;
    m->read(ids, std::span<ui::MeterLevels>(&l, 1));
    // A repaint only when a drawn end moves (a quarter of a world unit).
    std::array<int32_t, 4> q{};
    for (int c = 0; c < 2; ++c) {
      const int ch = l.channels > 1 ? c : 0;
      q[static_cast<size_t>(2 * c)] = static_cast<int32_t>(std::lround(meterX(l.rmsDb[ch]) * 4.0f));
      q[static_cast<size_t>(2 * c + 1)] = static_cast<int32_t>(std::lround(meterX(l.peakDb[ch]) * 4.0f));
    }
    if (q == shown_) return false;
    shown_ = q;
    meter_ = l;
    ++tap_.serial_;
    return true;
  }
  FaceShape shape() const noexcept override { return {false, {0.0f, 0.0f}, kR}; }

  // ---- static layers: body, tracks (grooves, the knob's track, rules), ticks, frame ----
  uint32_t staticLayerCount() const noexcept override { return 4; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    switch (layer) {
      case 0: return th.face.body;
      case 1: return th.face.track;
      case 2: return th.face.tick;
      default: return th.face.frame;
    }
  }
  uint64_t staticKey(uint32_t /*layer*/) const override {  // by its geometry (every mixer of one size shares its tiles)
    uint64_t h = mix(0x4D49584552ull, static_cast<uint64_t>(std::lround(size_.x * 4.0f)));
    h = mix(h, static_cast<uint64_t>(std::lround(size_.y * 4.0f)));
    h = mix(h, (expanded_ ? 2u : 0u) | (master_ ? 1u : 0u));
    return mix(h, sends_.size()) | 1u;
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    const float W = size_.x;
    if (layer == 0) {
      p.roundedRect({0.0f, 0.0f, W, size_.y}, kR, w);
      return;
    }
    if (layer == 1) {
      p.line({6.0f, kHeadH}, {W - 6.0f, kHeadH}, 1.0f, ui::mulAlpha(w, 0.7f));
      p.arc(kPanC, kPanR, panAngle(-1.0f), panAngle(1.0f), kPanW, w);
      p.roundedRect({kFx0, kFy - 2.0f, faderX1(), kFy + 2.0f}, 2.0f, w);
      for (const float y : {kMeterY0, kMeterY1}) p.roundedRect({kFx0, y - 1.0f, faderX1(), y + 1.0f}, 1.0f, ui::mulAlpha(w, 0.6f));
      if (!sends_.empty()) p.line({6.0f, metrics::kMixerH}, {W - 6.0f, metrics::kMixerH}, 1.0f, ui::mulAlpha(w, 0.7f));
      for (size_t k = 0; k < sends_.size(); ++k) {
        p.roundedRect({kSx0, sendY(k) - 1.5f, sendX1(), sendY(k) + 1.5f}, 1.5f, w);
        p.disc({14.0f, sendY(k)}, 4.75f, ui::mulAlpha(w, 0.8f));  // the letter's chip
      }
      return;
    }
    if (layer == 2) {
      // The fader's scale: +6, 0 (long), −6, −12, −24, −48 dB; the knob's centre detent.
      for (const double db : {6.0, 0.0, -6.0, -12.0, -24.0, -48.0}) {
        const float x = faderX(db);
        const bool unit = db == 0.0;
        p.line({x, kFy + 5.0f}, {x, kFy + (unit ? 10.0f : 8.0f)}, unit ? 1.2f : 0.9f, ui::mulAlpha(w, unit ? 1.0f : 0.7f));
      }
      p.line(polar(kPanC, kPanR + 2.5f, -0.5f * kPi), polar(kPanC, kPanR + 5.0f, -0.5f * kPi), 1.1f, w);
      return;
    }
    outline(p, {0.6f, 0.6f, W - 0.6f, size_.y - 0.6f}, kR - 0.6f, 1.2f, w);
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    const ui::Theme& th = *env.theme;
    const doc::Track* t = track();
    if (t == nullptr) return;
    const float W = size_.x, H = size_.y;
    const bool on = t->active && !t->mixer.mute;
    const Rgba accent = on ? th.face.value : ui::lerp(th.face.track, th.face.value, 0.45f);
    const Rgba signal = on ? th.face.signal : ui::lerp(th.face.track, th.face.signal, 0.45f);

    // ---- header: a sheen, the track's colour, the name, M / S ----
    const Vec2 sheen[2] = {{kR * 0.5f, 1.2f}, {W - kR * 0.5f, 1.2f}};
    p.fillUnder(sheen, kHeadH - 1.0f, ui::rgba(255, 255, 255, 18), ui::rgba(255, 255, 255, 0));
    const Rgba colour = ui::hex(t->colour.rgb);
    p.roundedRect({7.0f, 5.0f, 10.0f, 13.0f}, 1.5f, colour);
    p.textAligned({14.0f, 9.0f}, master_ ? "MASTER" : "MIXER", kLabelPx, th.face.ink, 0.0f);
    if (!master_) {
      pill(p, th, muteRect(), "M", !t->mixer.mute, th.mixer.activatorOn, th.mixer.activatorText, kMute);
      pill(p, th, soloRect(), "S", t->mixer.solo, th.mixer.soloOn, th.mixer.activatorText, kSolo);
    }

    // ---- the pan law's tag (TX.pan-laws) ----
    if (hovered_ == kPanLaw) p.roundedRect(kLawRect, 2.5f, th.face.hover);
    p.text({(kLawRect.x0 + kLawRect.x1) * 0.5f, (kLawRect.y0 + kLawRect.y1) * 0.5f}, std::string(doc::panModeShort(t->mixer.panMode)),
           6.0f, th.face.ink);

    // ---- pan ----
    const float pan = std::clamp(t->mixer.pan, -1.0f, 1.0f);
    const float a0 = panAngle(0.0f), a1 = panAngle(pan);
    const bool panHot = hovered_ == kPan || drag_ == kPan;
    p.ring(kPanC, kPanR, kPanW + 1.5f, panHot ? th.face.hover : 0);
    p.arc(kPanC, kPanR, std::min(a0, a1), std::max(a0, a1), kPanW - 0.5f, accent);
    p.disc(kPanC, kPanR - 3.0f, ui::lerp(th.face.body, th.face.track, 0.55f));
    p.line(polar(kPanC, 2.0f, a1), polar(kPanC, kPanR - 1.0f, a1), panHot ? 2.0f : 1.6f, th.face.handle);
    p.disc(polar(kPanC, kPanR, a1), 2.4f, th.face.handle);
    p.text({kPanC.x, 64.0f}, formatPan(pan), kLabelPx, th.face.ink);

    // ---- the level meter (L over R): the RMS body, the peak's lighter tail, the part over 0 dB in the accent ----
    const float zero = faderX(0.0);
    for (int c = 0; c < 2; ++c) {
      const int ch = meter_.channels > 1 ? c : 0;
      const float y = c == 0 ? kMeterY0 : kMeterY1;
      const float rx = meterX(meter_.rmsDb[ch]), px = std::max(rx, meterX(meter_.peakDb[ch]));
      if (px <= kFx0 + 0.25f) continue;
      p.roundedRect({kFx0, y - 1.0f, px, y + 1.0f}, 1.0f, ui::mulAlpha(th.face.signal, 0.45f));
      if (rx > kFx0 + 0.25f) p.roundedRect({kFx0, y - 1.0f, rx, y + 1.0f}, 1.0f, th.face.signal);
      if (px > zero) p.roundedRect({zero, y - 1.0f, px, y + 1.0f}, 1.0f, th.face.value);
    }

    // ---- volume ----
    const float db = t->mixer.volumeDb;
    const float fx = faderX(db);
    const bool volHot = hovered_ == kVolume || drag_ == kVolume;
    if (fx > kFx0 + 0.5f) {
      p.roundedRect({kFx0, kFy - 2.0f, fx, kFy + 2.0f}, 2.0f, accent);
      if (on) {  // the level's light at the fader's cap
        p.beginGlow();
        p.glow({fx, kFy}, 9.0f, ui::mulAlpha(th.face.glow, volHot ? 0.55f : 0.35f));
        p.endGlow();
      }
    }
    // The cap: a dark rim, a bright face, its grip line.
    p.roundedRect({fx - 4.5f, kFy - 8.5f, fx + 4.5f, kFy + 8.5f}, 2.5f, ui::mulAlpha(th.face.body, 0.9f));
    p.roundedRect({fx - 3.5f, kFy - 7.5f, fx + 3.5f, kFy + 7.5f}, 2.0f, volHot ? ui::lerp(th.face.handle, th.face.value, 0.35f) : th.face.handle);
    p.line({fx, kFy - 4.5f}, {fx, kFy + 4.5f}, 1.2f, th.face.body);
    p.textAligned({W - kFaderInsetR, 63.0f}, formatVolume(db) + " dB", kValuePx, accent, 1.0f);

    // ---- sends ----
    for (size_t k = 0; k < sends_.size(); ++k) {
      const float y = sendY(k);
      const float sdb = sendDb(*t, k);
      const float sx = sendX(sdb);
      const bool hot = hovered_ == kSend0 + static_cast<int32_t>(k) || drag_ == kSend0 + static_cast<int32_t>(k);
      p.text({14.0f, y}, sends_[k].letter, 6.5f, th.face.ink);
      if (sx > kSx0 + 0.5f) p.roundedRect({kSx0, y - 1.5f, sx, y + 1.5f}, 1.5f, signal);
      p.disc({sx, y}, hot ? 3.4f : 2.8f, hot ? ui::lerp(th.face.handle, th.face.signal, 0.35f) : th.face.handle);
      if (expanded_) p.textAligned({W - 12.0f, y}, formatVolume(sdb), kLabelPx, sdb <= doc::kMinVolumeDb ? th.face.ink : signal, 1.0f);
    }

    // A deactivated track: the whole face veiled.
    if (!t->active) p.roundedRect({0.0f, 0.0f, W, H}, kR, ui::mulAlpha(th.face.body, 0.5f));
    // Selection / hover of the block: its rim.
    outline(p, {0.6f, 0.6f, W - 0.6f, H - 0.6f}, kR - 0.6f, env.selected ? 1.75f : 1.25f,
            env.selected ? th.circuit.selected : (env.hovered && hovered_ == kFrame ? th.circuit.hover : 0));
  }

  void liveRects(std::vector<Rect>& out) const override { out.push_back({0.0f, 0.0f, size_.x, size_.y}); }

  // ---- interaction ----
  int32_t hitTest(Vec2 l, float /*pxPerUnit*/) const noexcept override {
    if (l.x < 0.0f || l.y < 0.0f || l.x > size_.x || l.y > size_.y) return kNone;
    if (!master_) {
      if (in(grow(muteRect(), 1.5f), l)) return kMute;
      if (in(grow(soloRect(), 1.5f), l)) return kSolo;
    }
    if (in(kLawRect, l)) return kPanLaw;
    if (std::hypot(l.x - kPanC.x, l.y - kPanC.y) <= kPanR + 4.0f) return kPan;
    if (in({kFx0 - 6.0f, kFy - 10.0f, faderX1() + 6.0f, kFy + 10.0f}, l)) return kVolume;
    for (size_t k = 0; k < sends_.size(); ++k)
      if (in({kSx0 - 5.0f, sendY(k) - 5.0f, sendX1() + 5.0f, sendY(k) + 5.0f}, l)) return kSend0 + static_cast<int32_t>(k);
    return kFrame;
  }

  bool onPointer(const FacePointer& e) override {
    using K = FacePointer::Kind;
    const doc::Track* t = track();
    switch (e.kind) {
      case K::Down: {
        if (e.button != ui::PointerButton::Left || t == nullptr) return false;
        const int32_t part = hitTest(e.local, e.pxPerUnit);
        if (part < 0) return false;
        const bool reset = e.clicks >= 2;
        const doc::TrackId id = track_;  // (an edit may retire this face: nothing of it is read after one)
        ProjectPatchView& v = view_;
        if (part == kMute) {
          (void)v.toggleMute(id);
          return true;
        }
        if (part == kSolo) {
          (void)v.solo(id, (e.mods & ui::mod::kCtrl) != 0);
          return true;
        }
        if (part == kPanLaw) {
          (void)v.openPanLawMenu(id, e.device);
          return true;
        }
        if (part == kVolume) {
          (void)v.showParamAutomation(id, doc::ParamRef::track(doc::ParamKind::TrackVolume, id));  // TL.2: its sub-lane
          if (reset) {
            set(v, "Set Volume", [&] { v.setVolume(id, 0.0f); });
            return true;
          }
          start(part, e, volumeToFader(t->mixer.volumeDb));
          return true;
        }
        if (part == kPan) {
          (void)v.showParamAutomation(id, doc::ParamRef::track(doc::ParamKind::TrackPan, id));  // TL.2
          if (reset) {
            set(v, "Set Pan", [&] { v.setPan(id, 0.0f); });
            return true;
          }
          start(part, e, t->mixer.pan);
          return true;
        }
        const size_t k = static_cast<size_t>(part - kSend0);
        if (k >= sends_.size()) return false;
        (void)v.showParamAutomation(id, doc::ParamRef::track(doc::ParamKind::TrackSend, id, sends_[k].target));  // TL.2
        if (reset) {
          const doc::TrackId ret = sends_[k].target;
          set(v, "Set Send Level", [&] { v.setSend(id, ret, 0.0f); });
          return true;
        }
        start(part, e, volumeToFader(sendDb(*t, k)));
        return true;
      }
      case K::Move: {
        if (drag_ < 0) return false;
        const double fine = (e.mods & ui::mod::kShift) ? 0.1 : 1.0;
        if (drag_ == kPan) {
          const double dy = (static_cast<double>(e.device.y) - startDev_.y) * fine;
          view_.beginValueGesture("Set Pan");
          view_.setPan(track_, static_cast<float>(std::clamp(start_ - 2.0 * dy / (120.0 * e.uiScale), -1.0, 1.0)));
          return true;
        }
        const bool vol = drag_ == kVolume;
        const double width = std::max(4.0, static_cast<double>((vol ? faderX1() - kFx0 : sendX1() - kSx0) * e.pxPerUnit));
        const double pos = std::clamp(start_ + (static_cast<double>(e.device.x) - startDev_.x) * fine / width, 0.0, 1.0);
        if (vol) {
          view_.beginValueGesture("Set Volume");
          view_.setVolume(track_, static_cast<float>(faderToVolume(pos)));
        } else {
          view_.beginValueGesture("Set Send Level");
          view_.setSend(track_, sends_[static_cast<size_t>(drag_ - kSend0)].target, static_cast<float>(faderToVolume(pos)));
        }
        return true;
      }
      case K::Up:
        if (drag_ < 0) return false;
        drag_ = kNone;
        view_.endValueGesture();
        return true;
      case K::Wheel:
      case K::Leave: return false;
    }
    return false;
  }
  bool dragging() const noexcept override { return drag_ >= 0; }
  void cancelDrag() override {
    if (drag_ < 0) return;
    drag_ = kNone;
    view_.cancelValueGesture();
  }
  ui::Cursor cursor(int32_t part) const noexcept override {
    if (drag_ >= 0) part = drag_;  // a drag keeps its control's cursor wherever the pointer goes
    if (part == kMute || part == kSolo || part == kPanLaw) return ui::Cursor::Pointer;
    if (part == kPan) return ui::Cursor::ResizeNS;
    if (part >= kVolume) return ui::Cursor::ResizeEW;
    return ui::Cursor::Inherit;  // the frame: the block's (the canvas: Move)
  }
  void describe(int32_t part, std::string& title, std::string& body) const override {
    const doc::Track* t = track();
    title = std::string(master_ ? "Master mixer" : "Mixer") + (t != nullptr ? " · " + t->name : std::string());
    if (t == nullptr) {
      body.clear();
      return;
    }
    switch (part) {
      case kMute: body = std::string("Track Activator (") + (t->mixer.mute ? "off" : "on") + "): click to toggle."; break;
      case kSolo: body = "Solo: click (Ctrl+click: additive)."; break;
      case kPan: body = "Pan " + formatPan(t->mixer.pan) + ": drag up / down (Shift: fine), double-click: centre."; break;
      case kPanLaw: body = "Pan law: " + std::string(doc::panModeName(t->mixer.panMode)) + ". Click to choose (also in the mixer's right-click menu)."; break;
      case kVolume: body = "Volume " + formatVolume(t->mixer.volumeDb) + " dB: drag sideways (Shift: fine), double-click: 0 dB."; break;
      default:
        if (part >= kSend0 && static_cast<size_t>(part - kSend0) < sends_.size()) {
          const size_t k = static_cast<size_t>(part - kSend0);
          body = "Send " + sends_[k].letter + " " + formatVolume(sendDb(*t, k)) + " dB: drag sideways (Shift: fine), double-click: 0 dB.";
        } else {
          body = "The track's output: drag to place it (it stays where it is put), double-click to show the sends' levels; "
                 "its menu puts it back at the top right.";
        }
    }
  }

private:
  ProjectPatchView& view_;
  doc::TrackId track_;
  Vec2 size_;
  bool expanded_ = false, master_ = false;
  std::vector<MixerSend> sends_;
  int32_t drag_ = kNone;
  double start_ = 0.0;
  Vec2 startDev_{0.0f, 0.0f};
  mutable MeterTap tap_;
  ui::MeterLevels meter_{};         // the last level read (silence until the meter source is live)
  std::array<int32_t, 4> shown_{};  // its drawn ends (quarter units): a poll that moves none repaints nothing

  float meterX(float db) const noexcept { return faderX(db); }  // the meter reads on the fader's scale

  const doc::Track* track() const noexcept {
    const doc::Snapshot& s = view_.context().doc();
    return s ? doc::findTrackPtr(*s, track_) : nullptr;
  }
  float faderX1() const noexcept { return size_.x - kFaderInsetR; }
  float faderX(double db) const noexcept { return kFx0 + (faderX1() - kFx0) * static_cast<float>(volumeToFader(db)); }
  float sendX1() const noexcept { return size_.x - (expanded_ ? 50.0f : 14.0f); }
  float sendX(double db) const noexcept { return kSx0 + (sendX1() - kSx0) * static_cast<float>(volumeToFader(db)); }
  Rect muteRect() const noexcept { return {size_.x - 48.0f, 4.0f, size_.x - 31.0f, 15.0f}; }
  Rect soloRect() const noexcept { return {size_.x - 28.0f, 4.0f, size_.x - 11.0f, 15.0f}; }
  static Rect grow(Rect r, float d) noexcept { return {r.x0 - d, r.y0 - d, r.x1 + d, r.y1 + d}; }
  float sendDb(const doc::Track& t, size_t k) const noexcept {
    for (const doc::Send& s : t.mixer.sends)
      if (s.target == sends_[k].target) return s.levelDb;
    return doc::kMinVolumeDb;
  }
  void start(int32_t part, const FacePointer& e, double value) {
    drag_ = part;
    start_ = value;
    startDev_ = e.device;
  }
  template <class F>
  static void set(ProjectPatchView& v, const char* label, F&& f) {
    v.beginValueGesture(label);
    f();
    v.endValueGesture();
  }
  void pill(ui::FacePainter& p, const ui::Theme& th, const Rect& r, const char* glyph, bool lit, Rgba litFill, Rgba litText,
            int32_t part) const {
    if (hovered_ == part) p.roundedRect(grow(r, 1.0f), 3.5f, th.face.hover);
    p.roundedRect(r, 3.0f, lit ? litFill : th.face.track);
    if (lit) {  // lit: a soft halo of its own colour
      p.beginGlow();
      p.glow({(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f}, 10.0f, ui::mulAlpha(litFill, 0.25f));
      p.endGlow();
    }
    p.text({(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f}, glyph, kLabelPx, lit ? litText : th.face.ink);
  }
};

}  // namespace

std::optional<ControlAnchor> mixerControlAnchor(const Face& f, const doc::ParamRef& p) {
  const auto* m = dynamic_cast<const MixerFace*>(&f);
  return m != nullptr ? m->anchorOf(p) : std::nullopt;
}

std::string mixerFacePath(const Node& n) {
  std::string s = std::string(kMixerFacePrefix) + std::to_string(std::lround(n.w)) + "x" + std::to_string(std::lround(n.h));
  for (size_t k = 1; k < n.outputs.size(); ++k) s += ":" + n.outputs[k].name + "=" + n.outputs[k].label;
  return s;
}

std::unique_ptr<Face> makeMixerFace(ProjectPatchView& view, const ProjectPatch& pp, const Node& n) {
  if (n.kind != NodeKind::Mixer || n.band >= pp.bands.size()) return nullptr;
  const Band& b = pp.bands[n.band];
  std::vector<MixerSend> sends;
  for (size_t k = 1; k < n.outputs.size(); ++k) {
    const std::string& port = n.outputs[k].name;  // "s<return id>"
    MixerSend s;
    s.target = doc::TrackId{port.size() > 1 ? static_cast<uint32_t>(std::strtoul(port.c_str() + 1, nullptr, 10)) : 0u};
    s.letter = n.outputs[k].label;
    sends.push_back(std::move(s));
  }
  return std::make_unique<MixerFace>(view, b.track, Vec2{n.w, n.h}, n.expanded, b.kind == doc::TrackKind::Master, std::move(sends));
}

}  // namespace pq::views::project_patch
