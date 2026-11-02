// The Envelope face (TMM.11, docs/plans/MM.md TMM.11; the Phase accumulator's and the Wavetable's visual family: a dark
// body, quiet tracks, the shape in the accent, voices as neon-blue glowing dots). Label-free:
//
//   display   the envelope's shape, drawn from the same continuous-time law the DSP follows (pq/meta/envelope.h): the
//             attack rising to the peak, the decay to the sustain, a short sustain plateau, the release to zero, filled
//             in the accent. Time runs left to right on a log scale PER SEGMENT (each segment's width is its time's
//             control travel: equal steps per decade, 0 – 60 s), the level bottom to top.
//   handles   three breakpoints: the peak (drag left / right: attack), the decay's end (left / right: decay, up / down:
//             sustain) and the release's end (left / right: release); the sustain plateau drags up / down (sustain).
//             A small ring in the middle of each segment bends it (drag up / down: the curve; dragging the segment's
//             line itself does the same); the attack bends both ways, the decay and release from a straight line
//             (up) through msenv's exponential (the middle) to steeper (down).
//   amount    the slider at the right (the output's multiplier), the output leaving past it.
//   voices    each sounding voice is a glowing dot on the shape at its level and stage (the tap: level and released;
//             rising = attack), with a dotted vertical bar and the levels sampled during the last displayed frame as
//             a faint trail. Voices fade in and out over 80 ms.
//
// Every drag is one undo step and never a compile (every control is a runtime param). Double-click a control resets
// it (a breakpoint: both its params, one step). Shift: fine. World units 208 × 128.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

#include "faces.h"
#include "pq/meta/envelope.h"

namespace pq::views {

namespace {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float deg(float d) noexcept { return d * kPi / 180.0f; }

// ---- layout (world units) ----
constexpr float kW = 208.0f, kH = 128.0f;
constexpr float kRadius = 12.0f;
constexpr Rect kDisp{14.0f, 14.0f, 178.0f, 114.0f};  // the display's well
constexpr float kX0 = 20.0f;                          // the shape's start (level 0, time 0)
constexpr float kY0 = 104.0f, kY1 = 26.0f;            // level 0 and level 1
constexpr float kSeg = 44.0f;                         // a time segment's width at full travel (60 s), before the fit
constexpr float kAvail = 136.0f;                      // the width the three time segments may fill (fitted)
constexpr float kMaxFit = 4.0f;
constexpr float kHold = 16.0f;                        // the sustain plateau's width
constexpr float kAmtX = 192.0f, kAmtHalfW = 2.0f, kAmtY0 = 18.0f, kAmtY1 = 110.0f;  // the amount slider
constexpr float kOutY = 64.0f;                        // the output jack (and the `in` jack: the signal passes through)
constexpr float kHandleR = 3.4f, kBendR = 2.3f;
constexpr int kSteps = 40;                            // points per drawn segment (the same at every zoom)
constexpr float kFade = 0.08f;                        // voices fade in / out (seconds)

// ---- parts ----
enum Part : int32_t {
  kPeak = 0,      // attack
  kDecayEnd = 1,  // decay + sustain
  kRelEnd = 2,    // release
  kPlateau = 3,   // sustain
  kBendA = 4, kBendD = 5, kBendR_ = 6,  // the curves
  kAmount = 7,
  kParts = 8,
};

float levelY(double l) noexcept { return kY0 + (kY1 - kY0) * static_cast<float>(std::clamp(l, 0.0, 1.0)); }

class EnvelopeFace final : public MetaFace {
public:
  explicit EnvelopeFace(const meta::Type& t) : MetaFace(t) {}

  Vec2 worldSize() const noexcept override { return {kW, kH}; }

  // Inputs on the left edge: `in` level with the output (the signal passes straight through), the gate at the top, a
  // wired param's pin down the edge in the params' order.
  float inputY(size_t i, size_t n) const noexcept override {
    if (n != type().inputs.size() + type().params.size()) return i == 0 ? kOutY : MetaFace::inputY(i, n);
    static constexpr float kY[] = {kOutY, 24.0f, 36.0f, 46.0f, 76.0f, 86.0f, 96.0f, 106.0f, 116.0f, 54.0f};
    return i < std::size(kY) ? kY[i] : MetaFace::inputY(i, n);
  }
  float outputY(size_t j, size_t) const noexcept override { return j == 0 ? kOutY : kOutY + 16.0f * static_cast<float>(j); }

  // ---- static layers: body, tracks (the well, the amount slider), frame (outline, the level grid) ----
  uint32_t staticLayerCount() const noexcept override { return 3; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.face.body : (layer == 1 ? th.face.track : th.face.frame);
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    if (layer == 0) {
      p.roundedRect({1.0f, 1.0f, kW - 1.0f, kH - 1.0f}, kRadius, w);
      return;
    }
    if (layer == 1) {
      p.roundedRect(kDisp, 6.0f, ui::mulAlpha(w, 0.3f));
      p.roundedRect({kAmtX - kAmtHalfW, kAmtY0, kAmtX + kAmtHalfW, kAmtY1}, kAmtHalfW, w);
      return;
    }
    outline(p, 0.75f, 1.25f, w);
    // The level grid: full scale and half, faint (quiet guides, no labels); the amount slider's quarters.
    p.line({kDisp.x0 + 4.0f, kY1}, {kDisp.x1 - 4.0f, kY1}, 0.5f, ui::mulAlpha(w, 0.35f));
    const Vec2 half[2] = {{kDisp.x0 + 4.0f, levelY(0.5)}, {kDisp.x1 - 4.0f, levelY(0.5)}};
    p.dashed(half, 0.5f, 1.5f, 2.5f, ui::mulAlpha(w, 0.3f));
    p.line({kDisp.x0 + 4.0f, kY0}, {kDisp.x1 - 4.0f, kY0}, 0.75f, ui::mulAlpha(w, 0.55f));
    for (int i = 0; i <= 4; ++i) {
      const float y = kAmtY1 - (kAmtY1 - kAmtY0) * 0.25f * static_cast<float>(i);
      p.line({kAmtX + kAmtHalfW + 1.5f, y}, {kAmtX + kAmtHalfW + (i % 2 ? 3.0f : 4.0f), y}, 0.9f, ui::mulAlpha(w, 0.8f));
    }
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    const ui::Theme& th = *env.theme;
    const double now = clockNow();
    layout();
    // Leads: `in` into the well and the output from past the amount slider (in the signal colour when wired).
    for (const FaceJack& j : env.jacks) {
      if (!j.visible) continue;
      if (j.input && j.name == "in")
        p.line({8.0f, j.at.y}, {kDisp.x0, j.at.y}, 1.25f,
               j.connected ? ui::mulAlpha(th.face.signal, 0.45f) : ui::mulAlpha(th.face.jackRing, 0.3f));
      else if (!j.input && j.name != "done")
        p.line({kAmtX + 6.0f, j.at.y}, {kW - 8.0f, j.at.y}, 1.25f, ui::mulAlpha(th.face.signal, 0.5f));
    }
    // TMM.13-fix2 (MM review "UI and aesthetic" 11): the gate-done output is named beside its socket (it was an unlabelled
    // grey socket); one text primitive either way.
    {
      const FaceJack* done = nullptr;
      for (const FaceJack& j : env.jacks)
        if (!j.input && j.visible && j.name == "done") done = &j;
      // Above its socket, right-aligned at the edge: clear of the amount slider to its left and of the socket's ring.
      p.textAligned({kW - 0.5f, done != nullptr ? done->at.y - 7.5f : 0.0f}, "done", 6.0f, done != nullptr ? th.face.ink : 0, 1.0f);
    }
    // The shape: fill, then the line (each segment its own polyline so a hovered or dragged one can light).
    p.fillUnder(shape_, kY0, ui::mulAlpha(th.face.value, 0.24f), ui::mulAlpha(th.face.value, 0.03f));
    const Rgba line = drivenAny(env) ? th.face.valueDriven : th.face.value;
    for (int s = 0; s < 4; ++s) {
      const int32_t bend = s == 0 ? kBendA : s == 1 ? kBendD : s == 3 ? kBendR_ : kPlateau;
      const bool hot = hovered_ == bend || drag_ == bend || (s == 2 && (hovered_ == kPlateau || drag_ == kPlateau));
      p.polyline(segment(s), hot ? 2.1f : 1.5f, line);
    }
    // Where the sustain is held: a faint drop from the plateau to the baseline at both ends.
    const Vec2 d0[2] = {{pt_[2].x, pt_[2].y + 3.0f}, {pt_[2].x, kY0}}, d1[2] = {{pt_[3].x, pt_[3].y + 3.0f}, {pt_[3].x, kY0}};
    p.dashed(d0, 0.6f, 1.2f, 2.2f, ui::mulAlpha(th.face.handle, 0.22f));
    p.dashed(d1, 0.6f, 1.2f, 2.2f, ui::mulAlpha(th.face.handle, 0.22f));
    // The bend rings (segment middles), then the breakpoints.
    for (int s = 0; s < 3; ++s) {
      const int32_t part = kBendA + s;
      const bool hot = hovered_ == part || drag_ == part;
      const Vec2 c = bendAt(s);
      p.disc(c, kBendR + 0.6f, th.face.body);
      p.ring(c, kBendR, hot ? 1.4f : 1.0f, hot ? th.face.handle : ui::mulAlpha(th.face.value, 0.8f));
      p.disc(c, 0.9f, hot ? th.face.handle : 0);
    }
    p.disc(pt_[3], 1.6f, ui::mulAlpha(th.face.value, 0.9f));  // the release's start (follows the sustain)
    // The amount slider: the value from the bottom, its handle.
    const float at = static_cast<float>(value(meta::kEnvAmount));
    const float ay = kAmtY1 - (kAmtY1 - kAmtY0) * std::clamp(at, 0.0f, 1.0f);
    const bool amtDrv = driven(env, "amount");
    p.roundedRect({kAmtX - kAmtHalfW, kAmtY0, kAmtX + kAmtHalfW, kAmtY1}, kAmtHalfW, hovered_ == kAmount ? th.face.hover : 0);
    p.roundedRect({kAmtX - kAmtHalfW + 0.75f, ay, kAmtX + kAmtHalfW - 0.75f, kAmtY1}, 1.0f,
                  amtDrv ? th.face.valueDriven : th.face.value);
    p.line({kAmtX - 5.0f, ay}, {kAmtX + 5.0f, ay}, drag_ == kAmount ? 2.5f : 2.0f, amtDrv ? th.face.valueDriven : th.face.handle);
    // Selection / hover of the block.
    outline(p, 0.75f, env.selected ? 1.75f : 1.25f,
            env.selected ? th.circuit.selected : (env.hovered && hovered_ == kFrame ? th.circuit.hover : 0));
    paintVoices(p, th, now);
    // The breakpoints last (TMM.13-fix2, MM review "UI and aesthetic" 11): a voice's dot on one glows under it, so the
    // handle stays a handle (drawn under the dots, the peak's turned blue and read as a voice).
    for (int h = 0; h < 3; ++h) {
      const int32_t part = h;
      const Vec2 c = h == 0 ? pt_[1] : h == 1 ? pt_[2] : pt_[4];
      const bool hot = hovered_ == part || drag_ == part;
      p.disc(c, kHandleR + 0.8f, hot ? ui::mulAlpha(th.face.hover, 1.0f) : 0);
      p.disc(c, kHandleR, th.face.handle);
      p.ring(c, kHandleR - 0.6f, 1.0f, ui::mulAlpha(th.face.value, 0.9f));
    }
  }

  void liveRects(std::vector<Rect>& out) const override { out.push_back({kDisp.x0, kDisp.y0, kDisp.x1, kDisp.y1}); }
  bool animating() const noexcept override { return !shown_.empty(); }

  int32_t hitTest(Vec2 l, float pxPerUnit) const noexcept override {
    if (l.x < 0.0f || l.y < 0.0f || l.x > kW || l.y > kH) return kNone;
    layout();
    // The NEAREST target wins: each has a reach (its radius plus a margin), grown at small zooms so a few device px
    // always hit something; between targets, the one whose edge is closest. Breakpoints and bend rings beat the lines
    // they sit on by a small bias.
    const float slack = std::max(0.0f, 5.0f / std::max(pxPerUnit, 1e-6f) - 2.0f);
    int32_t best = kFrame;
    float bestD = slack;
    auto offer = [&](int32_t part, float excess) {
      if (excess < bestD) {
        bestD = excess;
        best = part;
      }
    };
    auto dist = [&](Vec2 c) { return std::sqrt((l.x - c.x) * (l.x - c.x) + (l.y - c.y) * (l.y - c.y)); };
    offer(kDecayEnd, dist(pt_[2]) - (kHandleR + 2.5f) - 0.6f);
    offer(kPeak, dist(pt_[1]) - (kHandleR + 2.5f) - 0.6f);
    offer(kRelEnd, dist(pt_[4]) - (kHandleR + 2.5f) - 0.6f);
    for (int s = 0; s < 3; ++s) offer(kBendA + s, dist(bendAt(s)) - (kBendR + 2.0f) - 0.4f);
    const float ax = std::max(0.0f, std::fabs(l.x - kAmtX) - 6.0f);
    const float ay = std::max({0.0f, kAmtY0 - 4.0f - l.y, l.y - kAmtY1 - 4.0f});
    offer(kAmount, std::sqrt(ax * ax + ay * ay));
    // On a segment's line: the plateau (sustain), else that segment's bend.
    offer(kPlateau, distToSegment(l, pt_[2], pt_[3]) - 3.5f);
    for (int s : {0, 1, 3}) {
      const auto& seg = segPts_[static_cast<size_t>(s)];
      float d = 1e9f;
      for (size_t i = 0; i + 1 < seg.size(); ++i) d = std::min(d, distToSegment(l, seg[i], seg[i + 1]));
      offer(s == 0 ? kBendA : s == 1 ? kBendD : kBendR_, d - 3.5f);
    }
    return best;
  }

  bool onPointer(const FacePointer& e) override {
    using K = FacePointer::Kind;
    switch (e.kind) {
      case K::Down: return down(e);
      case K::Move:
        if (drag_ < 0) {
          return false;
        }
        move(e);
        return true;
      case K::Up: {
        if (drag_ < 0) return false;
        if (begun_) target().end();
        drag_ = kNone;
        begun_ = false;
        return true;
      }
      case K::Wheel:
      case K::Leave: return false;
    }
    return false;
  }

  bool dragging() const noexcept override { return drag_ >= 0; }
  void cancelDrag() override {
    if (drag_ >= 0 && begun_) target().cancel();
    drag_ = kNone;
    begun_ = false;
  }

  ui::Cursor cursor(int32_t part) const noexcept override {
    switch (part) {
      case kPeak:
      case kRelEnd: return ui::Cursor::ResizeEW;
      case kDecayEnd: return ui::Cursor::Move;
      case kNone:
      case kFrame: return ui::Cursor::Inherit;
      default: return ui::Cursor::ResizeNS;
    }
  }

  std::optional<Rect> paramRect(size_t param) const noexcept override {
    layout();
    auto around = [](Vec2 c, float r) { return Rect{c.x - r, c.y - r, c.x + r, c.y + r}; };
    switch (param) {
      case meta::kEnvAttack: return around(pt_[1], 5.0f);
      case meta::kEnvDecay: return around(pt_[2], 5.0f);
      case meta::kEnvSustain: return Rect{pt_[2].x, pt_[2].y - 3.0f, std::max(pt_[3].x, pt_[2].x + 1.0f), pt_[2].y + 3.0f};
      case meta::kEnvRelease: return around(pt_[4], 5.0f);
      case meta::kEnvACurve: return around(bendAt(0), 4.0f);
      case meta::kEnvDCurve: return around(bendAt(1), 4.0f);
      case meta::kEnvRCurve: return around(bendAt(2), 4.0f);
      case meta::kEnvAmount: return Rect{kAmtX - 5.0f, kAmtY0, kAmtX + 5.0f, kAmtY1};
      default: return std::nullopt;
    }
  }

  void describe(int32_t part, std::string& title, std::string& body) const override {
    char buf[200];
    auto ms = [&](size_t p) {
      const double s = value(p);
      char t[32];
      if (s < 1.0) std::snprintf(t, sizeof t, "%.3g ms", s * 1000.0);
      else std::snprintf(t, sizeof t, "%.3g s", s);
      return std::string(t);
    };
    switch (part) {
      case kPeak:
        title = "Attack";
        std::snprintf(buf, sizeof buf, "%s to the peak. Drag left / right (Shift: fine); double-click resets.", ms(meta::kEnvAttack).c_str());
        break;
      case kDecayEnd:
        title = "Decay and sustain";
        std::snprintf(buf, sizeof buf, "Decay %s to sustain %.0f %%. Drag left / right for the decay, up / down for the sustain.",
                      ms(meta::kEnvDecay).c_str(), value(meta::kEnvSustain) * 100.0);
        break;
      case kRelEnd:
        title = "Release";
        std::snprintf(buf, sizeof buf, "%s after the note ends (from full level). Drag left / right; double-click resets.",
                      ms(meta::kEnvRelease).c_str());
        break;
      case kPlateau:
        title = "Sustain";
        std::snprintf(buf, sizeof buf, "%.0f %% while the note is held. Drag up / down; double-click resets.", value(meta::kEnvSustain) * 100.0);
        break;
      case kBendA:
      case kBendD:
      case kBendR_: {
        static constexpr const char* kNames[] = {"Attack curve", "Decay curve", "Release curve"};
        const size_t p = meta::kEnvACurve + static_cast<size_t>(part - kBendA);
        title = kNames[part - kBendA];
        std::snprintf(buf, sizeof buf, "%+.2f. Drag up / down to bend the segment (0: %s); double-click resets.", value(p),
                      part == kBendA ? "straight" : "exponential, as msenv");
        break;
      }
      case kAmount:
        title = "Amount";
        std::snprintf(buf, sizeof buf, "x%.3f on the output. Drag up / down (Shift: fine); double-click resets.", value(meta::kEnvAmount));
        break;
      default:
        MetaFace::describe(part, title, body);
        body = "Attack, decay, sustain and release, each segment bendable; wired to `in`, a VCA. Drag the points and the "
               "rings; double-click the frame to open it to its atoms.";
        return;
    }
    body = buf;
  }

protected:
  bool voicesPolled(bool changed) override;

private:
  // ---- values and the shape ----
  double value(size_t p) const { return p < type().params.size() ? target().value(p) : 0.0; }
  float travel(size_t p) const {
    return static_cast<float>(faceValueToTravel(target(), type(), p, value(p)));  // TX.knob-curves
  }
  float travelOfValue(size_t p, const std::array<double, meta::kEnvParams>& v) const {
    return static_cast<float>(faceValueToTravel(target(), type(), p, v[p]));
  }

  // Recomputes the breakpoints and the drawn segments when a value changed (cheap: 4 · kSteps points).
  void layout() const {
    std::array<double, meta::kEnvParams> v{};
    for (size_t i = 0; i < v.size(); ++i) v[i] = value(i);
    // The fit: the three time segments fill the display (×1 … ×4), frozen while a drag runs (the handle stays under
    // the pointer's travel) except that it shrinks if the shape would leave the display; re-fitted once it ends.
    const float raw = kSeg * (travelOfValue(meta::kEnvAttack, v) + travelOfValue(meta::kEnvDecay, v) +
                              travelOfValue(meta::kEnvRelease, v));
    const float target = std::clamp(kAvail / std::max(raw, 1e-3f), 1.0f, kMaxFit);
    const float fit = drag_ >= 0 && fit_ > 0.0f ? std::min(fit_, target) : target;
    if (v == laidOut_ && fit == fit_ && !segPts_[0].empty()) return;
    laidOut_ = v;
    fit_ = fit;
    attack_ = meta::envAttackLaw(v[meta::kEnvACurve]);
    decay_ = meta::envFallLaw(v[meta::kEnvDCurve]);
    release_ = meta::envFallLaw(v[meta::kEnvRCurve]);
    sus_ = std::clamp(v[meta::kEnvSustain], 0.0, 1.0);
    const float seg = kSeg * fit_;
    const float wa = seg * travelOfValue(meta::kEnvAttack, v), wd = seg * travelOfValue(meta::kEnvDecay, v),
                wr = seg * travelOfValue(meta::kEnvRelease, v);
    wSeg_ = {wa, wd, wr};
    pt_[0] = {kX0, kY0};
    pt_[1] = {kX0 + wa, kY1};
    pt_[2] = {pt_[1].x + wd, levelY(sus_)};
    pt_[3] = {pt_[2].x + kHold, levelY(sus_)};
    pt_[4] = {pt_[3].x + wr, kY0};
    for (auto& s : segPts_) s.clear();
    for (int i = 0; i <= kSteps; ++i) {
      const double u = static_cast<double>(i) / kSteps;
      segPts_[0].push_back({kX0 + wa * static_cast<float>(u), levelY(meta::envAttackLevel(attack_, u))});
      segPts_[1].push_back({pt_[1].x + wd * static_cast<float>(u), levelY(meta::envFallLevel(decay_, 1.0, sus_, 1.0 - sus_, u))});
      segPts_[3].push_back({pt_[3].x + wr * static_cast<float>(u), levelY(meta::envFallLevel(release_, sus_, 0.0, 1.0, u))});
    }
    segPts_[2] = {pt_[2], pt_[3]};
    shape_.clear();
    for (int s = 0; s < 4; ++s)
      for (const Vec2& q : segPts_[static_cast<size_t>(s)]) shape_.push_back(q);
  }
  std::span<const Vec2> segment(int s) const { return segPts_[static_cast<size_t>(s)]; }
  // A segment's bend ring: on the drawn curve at half its time.
  Vec2 bendAt(int s) const {
    const auto& seg = segPts_[s == 0 ? 0 : s == 1 ? 1 : 3];
    return seg[kSteps / 2];
  }

  // Where a level sits on the shape (the voices): attack (rising), decay (held) or release.
  Vec2 placeLevel(double level, bool released, bool rising) const {
    level = std::clamp(level, 0.0, 1.0);
    float x;
    if (released) {
      const double u = level > sus_ ? 0.0 : meta::envFallTime(release_, sus_, 0.0, 1.0, level);
      x = pt_[3].x + wSeg_[2] * static_cast<float>(std::min(u, 1.0));
    } else if (rising) {
      x = kX0 + wSeg_[0] * static_cast<float>(meta::envAttackTime(attack_, level));
    } else {
      const double u = meta::envFallTime(decay_, 1.0, sus_, 1.0 - sus_, std::max(level, sus_));
      x = pt_[1].x + wSeg_[1] * static_cast<float>(std::min(u, 1.0));
    }
    return {x, levelY(level)};
  }

  bool drivenAny(const FaceEnv& env) const {
    for (const FaceJack& j : env.jacks)
      if (j.input && j.connected && j.name != "in" && j.name != "gate" && j.name != "amount") return true;
    return false;
  }
  static bool driven(const FaceEnv& env, std::string_view name) {
    for (const FaceJack& j : env.jacks)
      if (j.input && j.connected && j.name == name) return true;
    return false;
  }

  void outline(ui::FacePainter& p, float inset, float th, Rgba col) const {
    const float x0 = inset, y0 = inset, x1 = kW - inset, y1 = kH - inset, r = kRadius - inset;
    const Vec2 e[4][2] = {{{x0 + r, y0}, {x1 - r, y0}}, {{x1, y0 + r}, {x1, y1 - r}}, {{x1 - r, y1}, {x0 + r, y1}}, {{x0, y1 - r}, {x0, y0 + r}}};
    for (const auto& s : e) p.line(s[0], s[1], th, col);
    p.arc({x1 - r, y0 + r}, r, deg(-90.0f), deg(0.0f), th, col);
    p.arc({x1 - r, y1 - r}, r, deg(0.0f), deg(90.0f), th, col);
    p.arc({x0 + r, y1 - r}, r, deg(90.0f), deg(180.0f), th, col);
    p.arc({x0 + r, y0 + r}, r, deg(180.0f), deg(270.0f), th, col);
  }

  static float distToSegment(Vec2 p, Vec2 a, Vec2 b) noexcept {
    const float vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy;
    const float t = l2 > 0.0f ? std::clamp(((p.x - a.x) * vx + (p.y - a.y) * vy) / l2, 0.0f, 1.0f) : 0.0f;
    const float dx = p.x - (a.x + t * vx), dy = p.y - (a.y + t * vy);
    return std::sqrt(dx * dx + dy * dy);
  }

  // ---- voices ----
  struct Shown {
    FaceVoice v;
    double born = 0.0, died = 0.0;
    bool dying = false, seen = false;
    bool rising = false;
    float lastLevel = -1.0f;
  };
  static float alphaOf(const Shown& s, double now) noexcept {
    const float in = std::clamp(static_cast<float>((now - s.born) / kFade), 0.0f, 1.0f);
    const float out = s.dying ? std::clamp(1.0f - static_cast<float>((now - s.died) / kFade), 0.0f, 1.0f) : 1.0f;
    return std::min(in, out);
  }

  void paintVoices(ui::FacePainter& p, const ui::Theme& th, double now) {
    if (shown_.empty()) return;
    uint32_t total = 0;
    for (const Shown& s : shown_) total += s.dying ? 0u : s.v.sampleCount;
    const float density = std::clamp(1200.0f / static_cast<float>(std::max(total, 1u)), 3.0f, 40.0f) / 40.0f;
    p.beginGlow();
    // The trail: the levels sampled during the last displayed frame (each placed in its own stage).
    for (const Shown& s : shown_) {
      if (s.dying) continue;
      const Rgba gc = ui::withAlpha(th.face.glow, static_cast<uint32_t>(46.0f * density * alphaOf(s, now)));
      float prev = -1.0f;
      for (uint32_t k = 0; k < s.v.sampleCount; ++k) {
        const meta::EnvelopeTap t = meta::decodeEnvelopeTap(s.v.samples[k]);
        const bool rising = !t.released && (prev >= 0.0f ? static_cast<float>(t.level) > prev + 1e-7f : s.rising);
        prev = static_cast<float>(t.level);
        p.glow(placeLevel(t.level, t.released, rising), 3.2f, gc);
      }
    }
    for (const Shown& s : shown_) {
      const meta::EnvelopeTap t = meta::decodeEnvelopeTap(s.v.phase);
      p.glow(placeLevel(t.level, t.released, s.rising), 9.0f, ui::withAlpha(th.face.glow, static_cast<uint32_t>(150.0f * alphaOf(s, now))));
    }
    p.endGlow();
    for (const Shown& s : shown_) {
      const float a = alphaOf(s, now);
      const meta::EnvelopeTap t = meta::decodeEnvelopeTap(s.v.phase);
      const Vec2 d = placeLevel(t.level, t.released, s.rising);
      const Vec2 bar[2] = {{d.x, d.y + 3.0f}, {d.x, kY0 + 3.0f}};  // a drop line to the baseline: where in time
      p.dashed(bar, 0.8f, 1.2f, 1.6f, ui::mulAlpha(th.face.signal, 0.45f * a));
      p.disc(d, 2.4f, ui::mulAlpha(th.face.signal, a));
      p.disc(d, 1.1f, ui::mulAlpha(ui::rgba(255, 255, 255), a));
    }
  }

  // ---- gestures ----
  bool down(const FacePointer& e) {
    if (e.button != ui::PointerButton::Left) return false;  // the host's menu
    const int32_t part = hitTest(e.local, e.pxPerUnit);
    if (part < 0) return false;
    MetaTarget& tg = target();
    if (e.clicks >= 2) {
      if (tg.editable()) {
        std::array<size_t, 2> ps{};
        const size_t n = paramsOf(part, ps);
        tg.begin("Reset");
        for (size_t i = 0; i < n; ++i) tg.set(ps[i], type().params[ps[i]].def);
        tg.end();
      }
      drag_ = kNone;
      return true;
    }
    drag_ = part;
    begun_ = false;
    last_ = e.device;
    std::array<size_t, 2> ps{};
    const size_t n = paramsOf(part, ps);
    for (size_t i = 0; i < 2; ++i) dragT_[i] = i < n ? travel(ps[i]) : 0.0f;
    return true;
  }

  // The params a part edits (the first horizontal for breakpoints with two), in that order.
  static size_t paramsOf(int32_t part, std::array<size_t, 2>& out) noexcept {
    switch (part) {
      case kPeak: out = {meta::kEnvAttack, 0}; return 1;
      case kDecayEnd: out = {meta::kEnvDecay, meta::kEnvSustain}; return 2;
      case kRelEnd: out = {meta::kEnvRelease, 0}; return 1;
      case kPlateau: out = {meta::kEnvSustain, 0}; return 1;
      case kBendA: out = {meta::kEnvACurve, 0}; return 1;
      case kBendD: out = {meta::kEnvDCurve, 0}; return 1;
      case kBendR_: out = {meta::kEnvRCurve, 0}; return 1;
      case kAmount: out = {meta::kEnvAmount, 0}; return 1;
      default: return 0;
    }
  }

  void move(const FacePointer& e) {
    const float dx = e.device.x - last_.x, dy = -(e.device.y - last_.y);
    last_ = e.device;
    MetaTarget& tg = target();
    if ((dx == 0.0f && dy == 0.0f) || !tg.editable()) return;
    const float fine = (e.mods & ui::mod::kShift) ? 0.1f : 1.0f;
    // Travel per device px: the control's extent on screen, never less than 150 logical px for the whole range.
    auto span = [&](float extent) { return std::max(extent * e.pxPerUnit, 150.0f * e.uiScale); };
    std::array<size_t, 2> ps{};
    const size_t n = paramsOf(drag_, ps);
    const bool horizontalFirst = drag_ == kPeak || drag_ == kDecayEnd || drag_ == kRelEnd;
    bool changed = false;
    for (size_t i = 0; i < n; ++i) {
      const bool horiz = horizontalFirst && i == 0;
      const float d = horiz ? dx : dy;
      if (d == 0.0f) continue;
      const float extent = horiz ? kSeg * fit_ : (drag_ == kAmount ? kAmtY1 - kAmtY0 : (kY0 - kY1));
      dragT_[i] = std::clamp(dragT_[i] + d / span(extent) * fine, 0.0f, 1.0f);
      if (!begun_) {
        static constexpr const char* kLabels[] = {"Set Attack", "Set Decay / Sustain", "Set Release", "Set Sustain",
                                                  "Bend Attack", "Bend Decay", "Bend Release", "Set Amount"};
        tg.begin(kLabels[drag_]);
        begun_ = true;
      }
      tg.set(ps[i], faceTravelToValue(tg, type(), ps[i], dragT_[i]));
      changed = true;
    }
    (void)changed;
  }

  int32_t drag_ = kNone;
  bool begun_ = false;
  Vec2 last_{0.0f, 0.0f};
  std::array<float, 2> dragT_{};
  std::vector<Shown> shown_;

  // The shape, recomputed when a value changes.
  mutable std::array<double, meta::kEnvParams> laidOut_{};
  mutable meta::EnvSegmentLaw attack_, decay_, release_;
  mutable double sus_ = 0.0;
  mutable std::array<float, 3> wSeg_{};
  mutable float fit_ = 0.0f;
  mutable std::array<Vec2, 5> pt_{};
  mutable std::array<std::vector<Vec2>, 4> segPts_;
  mutable std::vector<Vec2> shape_;
};

bool EnvelopeFace::voicesPolled(bool changed) {
  const double now = clockNow();
  if (changed) {
    for (Shown& s : shown_) s.seen = false;
    for (uint32_t i = 0; i < voiceCount_; ++i) {
      const FaceVoice& v = voiceBuf_[i];
      Shown* found = nullptr;
      for (Shown& s : shown_)
        if (s.v.slot == v.slot) found = &s;
      if (found == nullptr) {
        shown_.push_back({});
        found = &shown_.back();
        found->born = justBound_ ? now - kFade : now;  // sounding when the face appeared: shown at once
      } else if (found->dying) {
        found->born = now - static_cast<double>(alphaOf(*found, now)) * kFade;
      }
      // Rising (the attack): the samples since the last read climb, else the level rose since then; a new voice below
      // its sustain is attacking.
      const meta::EnvelopeTap t = meta::decodeEnvelopeTap(v.phase);
      bool rising;
      if (t.released) rising = false;
      else if (v.sampleCount >= 2) rising = v.samples[v.sampleCount - 1] > v.samples[0] + 1e-7f;
      else if (found->lastLevel >= 0.0f) rising = static_cast<float>(t.level) > found->lastLevel + 1e-7f;
      else rising = t.level < 0.999;
      found->rising = rising;
      found->lastLevel = static_cast<float>(t.level);
      found->v = v;
      found->dying = false;
      found->seen = true;
    }
    for (Shown& s : shown_)
      if (!s.seen && !s.dying) {
        s.dying = true;
        s.died = now;
        s.v.sampleCount = 0;
      }
  }
  justBound_ = false;
  const size_t before = shown_.size();
  std::erase_if(shown_, [&](const Shown& s) { return s.dying && now - s.died >= kFade; });
  bool fading = shown_.size() != before;
  for (const Shown& s : shown_) fading = fading || s.dying || now - s.born < kFade;
  if (shown_.size() > 2 * kMaxVoices) shown_.erase(shown_.begin(), shown_.end() - 2 * kMaxVoices);
  return changed || fading;
}

}  // namespace

std::unique_ptr<MetaFace> makeEnvelopeFace(const meta::Type& t) { return std::make_unique<EnvelopeFace>(t); }

}  // namespace pq::views
