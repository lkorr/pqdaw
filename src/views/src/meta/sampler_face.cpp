// The Sampler engine's face (SM.10, docs/plans/SM.md "Face (👁)" Closed): an oval plate holding the sample's waveform,
// a glowing playhead per sounding voice. The visual family of the Envelope's and the Noise's faces (a dark body, a quiet
// well, the signal in the accent, voices as glowing neon marks). Label-free:
//
//   waveform  the played region (Start … End) of the sample's overview (pq/views/waveforms.h through the target), one
//             min / max bar per world unit of the well, drawn as a STATIC LAYER keyed on the sample, Start, End and the
//             overview's progress (the host's tile: re-rasterised only when one of them changes); no sample: a flat line
//             and "drop a sample"
//   region    along the well's foot, the whole sample as a hairline with the played region (Start … End) lit and the
//             One-Shot / Slice fades as short ramps at its ends (a modulated Start / End / fade shows its ghost here)
//   voices    each sounding voice a vertical playhead with a glow at where it reads (the engine's voice tap, request 1:
//             the playhead or a wired Position, 0 … 1 of the sample), fading in and out over 80 ms
// The controls are the Sampler group's panel's (pq/document/modules.h samplerBlocks: Transpose · Filter · Volume, the
// Expand chevron); this face has none. The group's panel shows it through its `samplewave` element (the well).
// CPU: nothing while no voice sounds (the reader is not polled); a hidden face (Face::setShown false) tells its reader,
// which lets go of the tap (no reader there: the audio side's write is one relaxed load). World units 152 × 84.
//
// SM.11 (docs/plans/SM.md "Face (👁)" Expanded): two more sections, outside the closed face's world (drawn only through
// the Sampler group's panel, each clipped to its section; a paint whose clip misses one skips it):
//   editor  `sampleedit` (kEditor, 328 × 120): the WHOLE sample's waveform (columns cached by sample and overview), the
//           played region lit (Start / End drawn as handles on the flag strip under the waveform), the loop bar
//           (Classic with a loop: its ends and its body drag) with the loop fade shaded before both ends, the One-Shot /
//           Slice fades as shaded ramps, the slice lines (Slice mode, or Beats: drag one; double-click the waveform to
//           add one, a line to remove it), a ruler above (seconds with warp Off; else beats through the warp markers —
//           pq/dsp/warp_voice.h warpBeatOfFrame — or sampleBpm), the markers on it in the stretch modes (drag one along
//           the ruler: its frame moves, its beat stays; double-click the ruler to add one at the beat it reads there,
//           one to remove it), each sounding voice's playhead. Every drag is one undo step (MetaTarget begin … end: the
//           params and the data in one gesture), every double-click one step; the data never recompiles.
//   root    `sampleroot` (kRoot, 120 × 16): the root as "261.63 Hz", or (data `rootText` set) as the nearest identity of
//           the track's tuning and the cents left over ("C3 +2.0¢", MetaTarget::pitchName); click it to type Hz
//           ("440", "440 Hz") or an identity ("A3", "7\31 3 -5¢"), Return commits (one step: root and rootText
//           together); the Hz | ID cells switch the display (an identity keeps the root exactly where it is).

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "faces.h"
#include "pq/dsp/warp_voice.h"
#include "pq/media/waveform.h"
#include "pq/meta/sampler.h"

namespace pq::views {

namespace {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

constexpr float kW = 152.0f, kH = 84.0f;
constexpr Vec2 kCentre{kW * 0.5f, kH * 0.5f};
constexpr float kRx = kW * 0.5f - 0.6f, kRy = kH * 0.5f - 0.6f;
constexpr Rect kWell{24.0f, 8.0f, 128.0f, 42.0f};  // the waveform's well (the panel's samplewave element)
constexpr float kInset = 3.0f;                     // the waveform inside the well
constexpr float kFade = 0.08f;
constexpr int kEllipseSteps = 64;

// SM.11: the expanded editor's and the root field's sections (face units; the panel's `sampleedit` / `sampleroot`).
constexpr Rect kEditor{0.0f, 200.0f, 328.0f, 320.0f};
constexpr float kEdX0 = 6.0f, kEdX1 = 322.0f;            // the whole sample, 0 … 1
constexpr float kRulerY0 = 201.0f, kRulerY1 = 213.0f;    // seconds or beats; the warp markers
constexpr float kWaveY0 = 215.0f, kWaveY1 = 299.0f;      // the waveform (the loop bar along its top)
constexpr float kLoopY1 = 221.0f;
constexpr float kFlagY0 = 300.0f, kFlagY1 = 310.0f;      // the Start / End handles
constexpr Rect kRoot{0.0f, 330.0f, 120.0f, 346.0f};
constexpr Rect kRootText{0.5f, 330.5f, 82.0f, 345.5f}, kRootHz{84.0f, 330.5f, 101.0f, 345.5f}, kRootId{103.0f, 330.5f, 119.5f, 345.5f};
enum EditPart : int32_t {
  kPStart = 0, kPEnd, kPLoopStart, kPLoopEnd, kPLoop, kPWave, kPRuler, kPRootText, kPRootHz, kPRootId,
  kPSlice0 = 1000, kPMarker0 = 100000,
};
bool within(const Rect& r, Vec2 l) noexcept { return l.x >= r.x0 && l.x <= r.x1 && l.y >= r.y0 && l.y <= r.y1; }
float edX(double u) noexcept { return kEdX0 + static_cast<float>(std::clamp(u, 0.0, 1.0)) * (kEdX1 - kEdX0); }
double edU(float x) noexcept { return std::clamp(static_cast<double>((x - kEdX0) / (kEdX1 - kEdX0)), 0.0, 1.0); }
// Whether face rect `r` meets the painter's clip (a section's paint clips the face to it: the others are skipped).
bool meetsClip(ui::FacePainter& p, const Rect& r) noexcept {
  const ui::FaceTransform& t = p.transform();
  const ui::ClipRect c = p.drawList().clip();
  const float x0 = t.origin.x + r.x0 * t.scale, x1 = t.origin.x + r.x1 * t.scale;
  const float y0 = t.origin.y + r.y0 * t.scale, y1 = t.origin.y + r.y1 * t.scale;
  return x1 > static_cast<float>(c.x0) && x0 < static_cast<float>(c.x1) && y1 > static_cast<float>(c.y0) && y0 < static_cast<float>(c.y1);
}
std::string lowerTrim(std::string_view v) {
  std::string out;
  bool space = false;
  for (const char ch : v) {
    if (ch == ' ' || ch == '	') {
      space = !out.empty();
      continue;
    }
    if (space) out.push_back(' ');
    space = false;
    out.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
  }
  return out;
}
std::string centsText(double c) {
  if (std::fabs(c) < 0.05) return {};
  char b[24];
  std::snprintf(b, sizeof b, " %+.1f\xC2\xA2", c);  // "+2.0¢"
  return b;
}

void ellipsePoints(std::vector<Vec2>& out, Vec2 c, float rx, float ry, bool close) {
  out.clear();
  for (int i = 0; i < kEllipseSteps + (close ? 1 : 0); ++i) {
    const float a = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(kEllipseSteps);
    out.push_back({c.x + rx * std::cos(a), c.y + ry * std::sin(a)});
  }
}

using meta::SamplerMode;
using meta::SamplerWarp;

class SamplerFace final : public MetaFace {
public:
  explicit SamplerFace(const meta::Type& t) : MetaFace(t) {}
  ~SamplerFace() override {
    if (said_ != nullptr) said_->forget(this);
  }

  Vec2 worldSize() const noexcept override { return {kW, kH}; }
  FaceShape shape() const noexcept override { return {true, kCentre, kRx, kRy}; }
  float inputY(size_t, size_t) const noexcept override { return kH * 0.5f; }
  float outputY(size_t, size_t) const noexcept override { return kH * 0.5f; }
  bool ownsPlate() const noexcept override { return true; }

  // ---- shown (SM.10) ----
  void setShown(bool shown) override {
    shown_ = shown;
    say();
  }

  // ---- painting ----
  uint32_t staticLayerCount() const noexcept override { return 3; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.face.body : layer == 1 ? th.face.track : ui::mulAlpha(th.face.signal, 0.85f);
  }
  uint64_t staticKey(uint32_t layer) const override {
    if (layer != 2) return 0;
    refresh();
    return cols_.key;
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    if (layer == 0) {
      ellipsePoints(pts_, kCentre, kRx, kRy, false);
      p.convex(pts_, w);
      return;
    }
    if (layer == 1) {
      p.roundedRect(kWell, 5.0f, ui::mulAlpha(w, 0.32f));
      return;
    }
    refresh();
    const float mid = (kWell.y0 + kWell.y1) * 0.5f, half = (kWell.y1 - kWell.y0) * 0.5f - kInset;
    if (cols_.lo.empty()) {  // no sample (or its overview not there yet): the zero line
      p.line({kWell.x0 + kInset, mid}, {kWell.x1 - kInset, mid}, 0.8f, ui::mulAlpha(w, 0.5f));
      return;
    }
    const float x0 = kWell.x0 + kInset, step = (kWell.x1 - kWell.x0 - 2.0f * kInset) / static_cast<float>(cols_.lo.size());
    for (size_t i = 0; i < cols_.lo.size(); ++i) {
      const float x = x0 + step * (static_cast<float>(i) + 0.5f);
      const float a = mid - cols_.hi[i] * half, b = mid - cols_.lo[i] * half;
      p.line({x, std::min(a, b - 0.6f)}, {x, std::max(b, a + 0.6f)}, step * 0.9f, w);
    }
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    if (!said_ || said_ != voices()) say(true);  // painted: on screen (a host that never says counts it shown)
    const ui::Theme& th = *env.theme;
    const double now = clockNow();
    refresh();
    if (cols_.sample == 0)
      p.text({(kWell.x0 + kWell.x1) * 0.5f, (kWell.y0 + kWell.y1) * 0.5f - 6.0f}, "drop a sample", 6.0f, ui::mulAlpha(th.face.ink, 0.55f));
    paintRegion(p, th);
    if (!env.ghost && !shown_v_.empty()) {
      const float y0 = kWell.y0 + 1.5f, y1 = kWell.y1 - 1.5f;
      p.beginGlow();
      for (const Shown& s : shown_v_) {
        const float x = headX(s.v.phase);
        const auto a = static_cast<uint32_t>(150.0f * alphaOf(s, now));
        p.glow({x, (y0 + y1) * 0.5f}, 9.0f, ui::withAlpha(th.face.glow, a));
      }
      p.endGlow();
      for (const Shown& s : shown_v_) {
        const float x = headX(s.v.phase), a = alphaOf(s, now);
        p.line({x, y0}, {x, y1}, 1.3f, ui::mulAlpha(th.face.signal, a));
        p.disc({x, y0}, 1.4f, ui::mulAlpha(ui::rgba(255, 255, 255), a));
      }
    }
    if (env.selected || env.hovered) {
      ellipsePoints(pts_, kCentre, kRx, kRy, true);
      p.polyline(pts_, env.selected ? 1.75f : 1.25f, env.selected ? th.circuit.selected : th.circuit.hover);
    }
    if (!env.ghost && meetsClip(p, kEditor)) paintEditor(p, th, now);  // SM.11
    if (!env.ghost && meetsClip(p, kRoot)) paintRoot(p, th);
  }

  void liveRects(std::vector<Rect>& out) const override { out.push_back({kWell.x0 - 6.0f, kWell.y0 - 4.0f, kWell.x1 + 6.0f, kWell.y1 + 4.0f}); }
  bool animating() const noexcept override { return !shown_v_.empty(); }

  // ---- interaction: the closed face is the frame only (the panel has its controls); SM.11: the editor and the root ----
  int32_t hitTest(Vec2 l, float ppu) const noexcept override {
    if (within(kEditor, l)) return editorPart(l, ppu);
    if (within(kRoot, l)) return within(kRootHz, l) ? kPRootHz : within(kRootId, l) ? kPRootId : kPRootText;
    return shape().contains(l) ? kFrame : kNone;
  }
  bool onPointer(const FacePointer& e) override;
  bool dragging() const noexcept override { return drag_ != kNone; }
  void cancelDrag() override {
    if (drag_ == kNone) return;
    drag_ = kNone;
    target().cancel();
  }
  bool onKey(const ui::KeyEvent& e) override;
  bool onText(const ui::TextEvent& e) override {
    if (!typing_) return false;
    typed_ += e.text();
    return true;
  }
  bool wantsText() const noexcept override { return typing_; }
  void focusLost() override {
    if (typing_) commitRoot();
  }
  ui::Cursor cursor(int32_t part) const noexcept override {
    if (part == kPStart || part == kPEnd || part == kPLoopStart || part == kPLoopEnd || part >= kPSlice0) return ui::Cursor::ResizeEW;
    if (part == kPLoop) return ui::Cursor::Move;
    if (part == kPRootText) return ui::Cursor::Text;
    if (part == kPRootHz || part == kPRootId) return ui::Cursor::Pointer;
    return MetaFace::cursor(part);
  }
  void describe(int32_t part, std::string& title, std::string& body) const override {
    if (part == kFrame || part == kNone) {
      title = "Sampler";
      body = cols_.sample == 0 ? "Drop an audio file on it to load a sample."
                               : "The sample from Start to End; each sounding voice's playhead glows where it reads.";
      return;
    }
    switch (part) {
      case kPStart: title = "Start"; body = "Drag: where a note starts reading the sample."; return;
      case kPEnd: title = "End"; body = "Drag: where the sample ends."; return;
      case kPLoopStart: title = "Loop Start"; body = "Drag: where the loop starts (its end stays)."; return;
      case kPLoopEnd: title = "Loop End"; body = "Drag: where the loop ends."; return;
      case kPLoop: title = "Loop"; body = "Drag: move the loop (its length stays)."; return;
      case kPWave:
        title = "Waveform";
        body = slicesShown() ? "Double-click: add a slice here." : "The whole sample; Start, End and the loop drag here.";
        return;
      case kPRuler:
        title = "Beat Ruler";
        body = "The sample's beats through its warp markers. Double-click: add a warp marker here.";
        return;
      case kPRootText: title = "Root"; body = "The pitch the sample plays at rate 1. Click to type Hz or an identity (A3, 7\\31 3, +cents)."; return;
      case kPRootHz: title = "Root in Hz"; body = "Show the root as Hz."; return;
      case kPRootId: title = "Root as Identity"; body = "Show the root as the nearest pitch of the track's tuning and its cents."; return;
      default: break;
    }
    if (part >= kPMarker0) {
      title = "Warp Marker";
      body = "Drag along the ruler: this beat falls there in the sample. Double-click: remove it.";
      return;
    }
    if (part >= kPSlice0) {
      title = "Slice " + std::to_string(part - kPSlice0 + 1);
      body = "Drag: move the slice. Double-click: remove it.";
      return;
    }
    MetaFace::describe(part, title, body);
  }

  // (tests) the playheads shown and the waveform's columns
  size_t playheads() const noexcept { return shown_v_.size(); }

protected:
  bool voicesPolled(bool changed) override {
    const double now = clockNow();
    if (changed) {
      for (Shown& s : shown_v_) s.seen = false;
      for (uint32_t i = 0; i < voiceCount_; ++i) {
        const FaceVoice& v = voiceBuf_[i];
        Shown* found = nullptr;
        for (Shown& s : shown_v_)
          if (s.v.slot == v.slot) found = &s;
        if (found == nullptr) {
          shown_v_.push_back({});
          found = &shown_v_.back();
          found->born = justBound_ ? now - kFade : now;
        } else if (found->dying) {
          found->born = now - static_cast<double>(alphaOf(*found, now)) * kFade;
        }
        found->v = v;
        found->dying = false;
        found->seen = true;
      }
      for (Shown& s : shown_v_)
        if (!s.seen && !s.dying) {
          s.dying = true;
          s.died = now;
        }
    }
    justBound_ = false;
    const size_t before = shown_v_.size();
    std::erase_if(shown_v_, [&](const Shown& s) { return s.dying && now - s.died >= kFade; });
    bool fading = shown_v_.size() != before;
    for (const Shown& s : shown_v_) fading = fading || s.dying || now - s.born < kFade;
    if (shown_v_.size() > 2 * kMaxVoices) shown_v_.erase(shown_v_.begin(), shown_v_.end() - 2 * kMaxVoices);
    return changed || fading;
  }

private:
  struct Shown {
    FaceVoice v;
    double born = 0.0, died = 0.0;
    bool dying = false, seen = false;
  };
  // The waveform's columns (min / max in −1 … 1), remade when the sample, Start, End or the overview's progress change.
  struct Columns {
    uint32_t sample = 0;
    double start = -1.0, end = -1.0;
    const media::WaveformOverview* overview = nullptr;
    uint64_t built = 0, revision = ~uint64_t{0};
    uint64_t key = 0;
    std::vector<float> lo, hi;
  };
  static float alphaOf(const Shown& s, double now) noexcept {
    const float in = std::clamp(static_cast<float>((now - s.born) / kFade), 0.0f, 1.0f);
    const float out = s.dying ? std::clamp(1.0f - static_cast<float>((now - s.died) / kFade), 0.0f, 1.0f) : 1.0f;
    return std::min(in, out);
  }
  // The region strip: the sample 0 … 1 as a hairline, Start … End lit (as the target reads them now: a ghost pass's
  // target has the modulated values), the fades as ramps (their seconds over the sample's, or over 2 s unknown).
  void paintRegion(ui::FacePainter& p, const ui::Theme& th) const {
    const MetaTarget& t = target();
    const float x0 = kWell.x0 + kInset, x1 = kWell.x1 - kInset, y = kWell.y1 - 2.2f;
    const auto at = [&](double u) { return x0 + static_cast<float>(std::clamp(u, 0.0, 1.0)) * (x1 - x0); };
    const double s = t.value(meta::kSmpStart), e = t.value(meta::kSmpEnd);
    p.line({x0, y}, {x1, y}, 0.5f, ui::mulAlpha(th.face.frame, 0.8f));
    const float a = at(std::min(s, e)), b = at(std::max(s, e));
    p.line({a, y}, {std::max(b, a + 0.5f), y}, 0.9f, ui::mulAlpha(th.face.value, 0.6f));
    const double len = cols_.overview != nullptr && cols_.overview->sampleRate() > 0
                           ? static_cast<double>(cols_.overview->frames()) / cols_.overview->sampleRate()
                           : 2.0;
    const double fi = t.value(meta::kSmpFadeIn) / std::max(len, 1e-3), fo = t.value(meta::kSmpFadeOut) / std::max(len, 1e-3);
    if (fi > 0.0) p.line({a, y + 1.2f}, {at(std::min(s, e) + fi), y - 1.2f}, 0.8f, ui::mulAlpha(th.face.value, 0.8f));
    if (fo > 0.0) p.line({at(std::max(s, e) - fo), y - 1.2f}, {b, y + 1.2f}, 0.8f, ui::mulAlpha(th.face.value, 0.8f));
  }
  // Where a voice reading at `pos` (0 … 1 of the sample) is drawn: its place in the Start … End window of the well.
  float headX(float pos) const noexcept {
    const double s = cols_.start, e = cols_.end > cols_.start ? cols_.end : cols_.start + 1e-6;
    const double u = std::isfinite(pos) ? std::clamp((static_cast<double>(pos) - s) / (e - s), 0.0, 1.0) : 0.0;
    return kWell.x0 + kInset + static_cast<float>(u) * (kWell.x1 - kWell.x0 - 2.0f * kInset);
  }
  // Tells the reader this face is shown or not (once per change; `paint`: the face is being painted, so it is).
  void say(bool paint = false) {
    VoiceTapReader* r = voices();
    if (r != said_) {
      if (said_ != nullptr) said_->forget(this);
      said_ = r;
      if (said_ == nullptr) return;
      if (paint) shown_ = true;
      said_->shown(this, shown_);
      return;
    }
    if (said_ != nullptr) said_->shown(this, shown_);
  }
  void refresh() const {
    const MetaTarget& t = target();
    const uint32_t sample = meta::samplerSample(t.data().get());
    const double start = t.value(meta::kSmpStart), end = t.value(meta::kSmpEnd);
    const uint64_t rev = t.overviewRevision();
    if (sample == cols_.sample && start == cols_.start && end == cols_.end && rev == cols_.revision &&
        (cols_.overview == nullptr || cols_.overview->builtFrames() == cols_.built))
      return;
    cols_.sample = sample;
    cols_.start = start;
    cols_.end = end;
    cols_.revision = rev;
    cols_.overview = sample != 0 ? t.sampleOverview(sample) : nullptr;
    cols_.built = cols_.overview != nullptr ? cols_.overview->builtFrames() : 0;
    cols_.lo.clear();
    cols_.hi.clear();
    uint64_t key = 0x53414D50ull;  // "SAMP"
    auto mixIn = [&key](uint64_t v) { key = (key ^ v) * 0x100000001B3ull; };
    mixIn(sample);
    mixIn(std::bit_cast<uint64_t>(start));
    mixIn(std::bit_cast<uint64_t>(end));
    mixIn(cols_.built);
    mixIn(reinterpret_cast<uintptr_t>(cols_.overview));
    cols_.key = key | 1u;
    const media::WaveformOverview* o = cols_.overview;
    if (o == nullptr || o->frames() == 0 || o->levelCount() == 0 || cols_.built == 0) return;
    const auto n = static_cast<size_t>(kWell.x1 - kWell.x0 - 2.0f * kInset);  // a bar per world unit
    const double f0 = std::clamp(std::min(start, end), 0.0, 1.0) * static_cast<double>(o->frames());
    const double f1 = std::clamp(std::max(start, end), 0.0, 1.0) * static_cast<double>(o->frames());
    if (!(f1 > f0)) return;
    const double perCol = (f1 - f0) / static_cast<double>(n);
    size_t lv = 0;  // the coarsest level whose buckets are no wider than a column
    for (size_t i = 0; i < o->levelCount(); ++i)
      if (static_cast<double>(o->level(i).framesPerBucket) <= std::max(1.0, perCol)) lv = i;
    const media::WaveformOverview::Level& L = o->level(lv);
    const uint64_t valid = o->validBuckets(lv);
    if (L.framesPerBucket == 0 || valid == 0) return;
    cols_.lo.assign(n, 0.0f);
    cols_.hi.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
      const auto b0 = static_cast<uint64_t>((f0 + perCol * static_cast<double>(i)) / static_cast<double>(L.framesPerBucket));
      auto b1 = static_cast<uint64_t>(std::ceil((f0 + perCol * static_cast<double>(i + 1)) / static_cast<double>(L.framesPerBucket)));
      b1 = std::min(std::max(b1, b0 + 1), valid);
      int32_t lo = 32767, hi = -32768;
      for (uint32_t c = 0; c < o->channels(); ++c) {
        const int16_t* mm = o->minMax(lv, c);
        for (uint64_t b = b0; b < b1; ++b) {
          lo = std::min<int32_t>(lo, mm[2 * b]);
          hi = std::max<int32_t>(hi, mm[2 * b + 1]);
        }
      }
      if (lo > hi) lo = hi = 0;
      cols_.lo[i] = static_cast<float>(lo) / 32767.0f;
      cols_.hi[i] = static_cast<float>(hi) / 32767.0f;
    }
  }

  // ---- SM.11: the editor ----
  struct Whole {  // the whole sample's columns (the editor's waveform)
    uint32_t sample = 0;
    const media::WaveformOverview* overview = nullptr;
    uint64_t built = ~uint64_t{0}, revision = ~uint64_t{0};
    std::vector<float> lo, hi;
  };
  struct Geometry {  // what the editor reads of the target now
    double frames = 0.0, rate = 48000.0;  // the sample's (0: unknown, no overview yet)
    double start = 0.0, end = 1.0, loopStart = 0.0, loopEnd = 1.0, loopFade = 0.0, fadeIn = 0.0, fadeOut = 0.0;
    double sampleBpm = 120.0;
    SamplerMode mode = SamplerMode::Classic;
    SamplerWarp warp = SamplerWarp::Off;
    bool loop = false;
    std::vector<float> slices, markers;
  };
  Geometry geometry() const;
  bool slicesShown() const;
  bool markersShown() const;
  int32_t editorPart(Vec2 l, float ppu) const noexcept;
  void paintEditor(ui::FacePainter& p, const ui::Theme& th, double now);
  void paintRoot(ui::FacePainter& p, const ui::Theme& th);
  void refreshWhole() const;
  std::string rootLabel() const;
  void commitRoot();
  void editFloats(const char* field, std::vector<float> v);
  void dragTo(float x);

  std::vector<Shown> shown_v_;
  mutable Whole whole_;
  int32_t drag_ = kNone;   // the part being dragged
  double grab_ = 0.0;      // kPLoop: the pointer's offset from the loop's start (0 … 1)
  bool typing_ = false;    // the root field takes the keys
  std::string typed_;
  mutable std::string rootCache_;
  mutable uint64_t rootKey_ = 0;
  mutable Columns cols_;
  mutable std::vector<Vec2> pts_;
  VoiceTapReader* said_ = nullptr;  // the reader told whether this face is shown
  bool shown_ = true;
};

// ---- SM.11: the expanded editor --------------------------------------------------------------------------------------

SamplerFace::Geometry SamplerFace::geometry() const {
  const MetaTarget& t = target();
  Geometry g;
  refresh();
  if (const media::WaveformOverview* o = cols_.overview; o != nullptr && o->frames() > 0) {
    g.frames = static_cast<double>(o->frames());
    g.rate = o->sampleRate() > 0 ? static_cast<double>(o->sampleRate()) : 48000.0;
  }
  g.start = t.value(meta::kSmpStart);
  g.end = t.value(meta::kSmpEnd);
  g.loopStart = t.value(meta::kSmpLoopStart);
  g.loopEnd = std::min(1.0, g.loopStart + t.value(meta::kSmpLoopLength));
  g.loopFade = t.value(meta::kSmpLoopFade);
  g.fadeIn = t.value(meta::kSmpFadeIn);
  g.fadeOut = t.value(meta::kSmpFadeOut);
  g.sampleBpm = std::max(1.0, t.value(meta::kSmpSampleBpm));
  g.mode = static_cast<SamplerMode>(std::clamp(static_cast<int>(std::lround(t.setting(meta::kSmpMode))), 0, 2));
  g.warp = static_cast<SamplerWarp>(std::clamp(static_cast<int>(std::lround(t.setting(meta::kSmpWarp))), 0, 6));
  g.loop = g.mode == SamplerMode::Classic && std::lround(t.setting(meta::kSmpLoop)) != 0;
  const meta::DataPtr d = t.data();
  g.slices = meta::samplerSlices(d.get());
  g.markers = meta::samplerMarkers(d.get());
  return g;
}

bool SamplerFace::slicesShown() const {
  const MetaTarget& t = target();
  return std::lround(t.setting(meta::kSmpMode)) == static_cast<long>(SamplerMode::Slice) ||
         std::lround(t.setting(meta::kSmpWarp)) == static_cast<long>(SamplerWarp::Beats);
}
bool SamplerFace::markersShown() const {
  const MetaTarget& t = target();
  return std::lround(t.setting(meta::kSmpMode)) != static_cast<long>(SamplerMode::Slice) &&
         std::lround(t.setting(meta::kSmpWarp)) >= static_cast<long>(SamplerWarp::Beats);
}

int32_t SamplerFace::editorPart(Vec2 l, float ppu) const noexcept {
  const float tol = std::max(1.5f, 5.0f / std::max(ppu, 0.1f));  // ≈ 5 device px either side of a line
  const Geometry g = geometry();
  const auto near = [&](double u) { return std::fabs(l.x - edX(u)) <= tol; };
  if (l.y >= kFlagY0 - 1.0f && l.y <= kFlagY1 + 1.0f) {  // the flags (End wins where they meet: it is drawn on top)
    if (near(g.end)) return kPEnd;
    if (near(g.start)) return kPStart;
    return kFrame;
  }
  if (l.y >= kRulerY0 && l.y < kWaveY0 - 0.5f) {
    if (!markersShown() || g.frames <= 0.0) return kFrame;
    for (size_t i = 0; i + 1 < g.markers.size(); i += 2)
      if (near(static_cast<double>(g.markers[i]) / g.frames)) return kPMarker0 + static_cast<int32_t>(i / 2);
    return kPRuler;
  }
  if (l.y >= kWaveY0 && l.y <= kWaveY1) {
    if (g.loop && l.y <= kLoopY1 + 1.0f) {
      if (near(g.loopEnd)) return kPLoopEnd;
      if (near(g.loopStart)) return kPLoopStart;
      if (l.x > edX(g.loopStart) && l.x < edX(g.loopEnd)) return kPLoop;
    }
    if (near(g.end)) return kPEnd;
    if (near(g.start)) return kPStart;
    if (slicesShown() && g.frames > 0.0)
      for (size_t i = 0; i < g.slices.size(); ++i)
        if (near(static_cast<double>(g.slices[i]) / g.frames)) return kPSlice0 + static_cast<int32_t>(i);
    return kPWave;
  }
  return kFrame;
}

void SamplerFace::editFloats(const char* field, std::vector<float> v) {
  const meta::DataPtr d = target().data();
  target().setData(meta::samplerWith(d.get(), field, meta::DataValue::floatArray(std::move(v))));
}

// The nearest guide of the track's tuning to `hz` within a fifth either side (null: none).
static const PitchGuide* nearestGuide(const MetaTarget& t, double hz, std::vector<PitchGuide>& gs) {
  t.pitchGuides(static_cast<float>(hz / 1.5), static_cast<float>(hz * 1.5), gs);
  const PitchGuide* best = nullptr;
  for (const PitchGuide& x : gs)
    if (best == nullptr || std::fabs(std::log2(x.hz / hz)) < std::fabs(std::log2(best->hz / hz))) best = &x;
  return best;
}

bool SamplerFace::onPointer(const FacePointer& e) {
  using K = FacePointer::Kind;
  MetaTarget& t = target();
  if (e.kind == K::Move && drag_ != kNone) {
    dragTo(e.local.x);
    return true;
  }
  if (e.kind == K::Up) {
    if (drag_ == kNone) return false;
    drag_ = kNone;
    t.end();
    return true;
  }
  if (e.kind != K::Down || e.button != ui::PointerButton::Left) return false;
  const int32_t part = hitTest(e.local, e.pxPerUnit);
  if (part != kPRootText && typing_) commitRoot();
  if (part == kFrame || part == kNone) return false;
  if (!t.editable()) return false;
  const Geometry g = geometry();
  const double u = edU(e.local.x);
  switch (part) {
    case kPRootText:
      if (!typing_) {
        typing_ = true;
        typed_.clear();
      }
      return true;
    case kPRootHz:
    case kPRootId: {
      const meta::DataPtr d = t.data();
      const bool id = part == kPRootId;
      if (id == !meta::samplerRootText(d.get()).empty()) return true;  // shown so already
      std::string text;
      if (id) {
        const double root = t.value(meta::kSmpRoot);
        std::vector<PitchGuide> gs;
        const PitchGuide* best = nearestGuide(t, root, gs);
        text = best != nullptr ? t.pitchName(*best) : std::string();
        if (text.empty()) {
          t.notice("The track has no tuning to name the root in");
          return true;
        }
        text += centsText(1200.0 * std::log2(root / best->hz));
      }
      t.setData(meta::samplerWith(d.get(), "rootText", meta::DataValue::str(std::move(text))));
      return true;
    }
    case kPWave:
      if (e.clicks < 2 || !slicesShown() || g.frames <= 0.0) return false;
      {  // a slice here (one step)
        std::vector<float> v = g.slices;
        const float f = static_cast<float>(std::round(u * g.frames));
        if (std::find(v.begin(), v.end(), f) != v.end()) return true;
        v.insert(std::upper_bound(v.begin(), v.end(), f), f);
        editFloats("slices", std::move(v));
      }
      return true;
    case kPRuler:
      if (e.clicks < 2 || g.frames <= 0.0) return false;
      {  // a warp marker at the beat the ruler reads here (one step; nothing moves)
        std::vector<float> m = g.markers;
        dsp::WarpSource src;
        src.fileRate = g.rate;
        src.markers = m.data();
        src.markerCount = static_cast<uint32_t>(m.size() / 2);
        const double fpb = 60.0 * g.rate / g.sampleBpm;
        const auto f = static_cast<float>(std::round(u * g.frames));
        const auto beat = static_cast<float>(dsp::warpBeatOfFrame(src, f, fpb));
        size_t at = 0;
        while (at + 1 < m.size() && m[at] < f) at += 2;
        if (at + 1 < m.size() && m[at] == f) return true;
        if ((at >= 2 && !(m[at - 1] < beat)) || (at + 1 < m.size() && !(beat < m[at + 1]))) return true;  // (not ascending)
        m.insert(m.begin() + static_cast<std::ptrdiff_t>(at), {f, beat});
        editFloats("markers", std::move(m));
      }
      return true;
    default: break;
  }
  if (part >= kPSlice0 && e.clicks >= 2) {  // a slice / marker removed (one step)
    const bool marker = part >= kPMarker0;
    std::vector<float> v = marker ? g.markers : g.slices;
    const auto i = static_cast<size_t>(part - (marker ? kPMarker0 : kPSlice0));
    if (marker && 2 * i + 1 < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(2 * i), v.begin() + static_cast<std::ptrdiff_t>(2 * i + 2));
    else if (!marker && i < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
    editFloats(marker ? "markers" : "slices", std::move(v));
    return true;
  }
  static constexpr const char* kLabels[] = {"Sample Start", "Sample End", "Loop Start", "Loop End", "Move Loop"};
  const char* label = part >= kPMarker0 ? "Move Warp Marker" : part >= kPSlice0 ? "Move Slice" : kLabels[part];
  grab_ = part == kPLoop ? u - g.loopStart : 0.0;
  drag_ = part;
  t.begin(label);
  return true;
}

void SamplerFace::dragTo(float x) {
  MetaTarget& t = target();
  const Geometry g = geometry();
  const double u = edU(x);
  constexpr double kGap = 1e-4;  // Start stays before End, a loop keeps a length
  switch (drag_) {
    case kPStart: t.set(meta::kSmpStart, std::min(u, g.end - kGap)); return;
    case kPEnd: t.set(meta::kSmpEnd, std::max(u, g.start + kGap)); return;
    case kPLoopStart: {
      const double ls = std::min(u, g.loopEnd - kGap);
      t.set(meta::kSmpLoopStart, ls);
      t.set(meta::kSmpLoopLength, g.loopEnd - ls);
      return;
    }
    case kPLoopEnd: t.set(meta::kSmpLoopLength, std::max(u, g.loopStart + kGap) - g.loopStart); return;
    case kPLoop: {
      const double len = g.loopEnd - g.loopStart;
      t.set(meta::kSmpLoopStart, std::clamp(u - grab_, 0.0, std::max(0.0, 1.0 - len)));
      return;
    }
    default: break;
  }
  if (g.frames <= 0.0) return;
  const double f = std::round(u * g.frames);
  if (drag_ >= kPMarker0) {  // its frame between its neighbours' (strictly), its beat kept
    std::vector<float> m = g.markers;
    const auto i = static_cast<size_t>(drag_ - kPMarker0);
    if (2 * i + 1 >= m.size()) return;
    const double lo = i > 0 ? m[2 * i - 2] + 1.0 : 0.0, hi = 2 * i + 2 < m.size() ? m[2 * i + 2] - 1.0 : g.frames;
    m[2 * i] = static_cast<float>(std::clamp(f, lo, std::max(lo, hi)));
    editFloats("markers", std::move(m));
    return;
  }
  if (drag_ >= kPSlice0) {  // between its neighbours (the order is the slice keys')
    std::vector<float> v = g.slices;
    const auto i = static_cast<size_t>(drag_ - kPSlice0);
    if (i >= v.size()) return;
    const double lo = i > 0 ? v[i - 1] + 1.0 : 0.0, hi = i + 1 < v.size() ? v[i + 1] - 1.0 : g.frames - 1.0;
    v[i] = static_cast<float>(std::clamp(f, lo, std::max(lo, hi)));
    editFloats("slices", std::move(v));
  }
}

// The root as shown: Hz, or (rootText set) the nearest identity of the track's tuning + cents (the stored text when the
// target knows no tuning). Cached by the root, the data and the guides' key.
std::string SamplerFace::rootLabel() const {
  const MetaTarget& t = target();
  const double root = t.value(meta::kSmpRoot);
  const meta::DataPtr d = t.data();
  const uint64_t key = ((std::bit_cast<uint64_t>(root) ^ (t.guidesKey() * 0x9E3779B97F4A7C15ull)) +
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d.get()))) | 1u;
  if (key == rootKey_) return rootCache_;
  rootKey_ = key;
  const std::string text = meta::samplerRootText(d.get());
  char b[32];
  std::snprintf(b, sizeof b, root < 1000.0 ? "%.2f Hz" : "%.1f Hz", root);
  rootCache_ = b;
  if (text.empty()) return rootCache_;
  std::vector<PitchGuide> gs;
  const PitchGuide* best = nearestGuide(t, root, gs);
  const std::string name = best != nullptr ? t.pitchName(*best) : std::string();
  rootCache_ = name.empty() ? text : name + centsText(1200.0 * std::log2(root / best->hz));
  return rootCache_;
}

// Typed text: Hz ("440", "440 Hz", "1.2k") sets the root and the Hz display; an identity of the track's tuning with
// optional cents ("A3", "7\31 3 -5c", "C4 +12¢") sets the root to it and the identity display. One step.
void SamplerFace::commitRoot() {
  typing_ = false;
  MetaTarget& t = target();
  const std::string in = lowerTrim(typed_);
  typed_.clear();
  if (in.empty()) return;
  const meta::DataPtr d = t.data();
  {
    char* end = nullptr;
    double hz = std::strtod(in.c_str(), &end);
    std::string_view rest(end);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    if (end != in.c_str() && (rest.empty() || rest == "hz" || rest == "k" || rest == "khz")) {
      if (rest == "k" || rest == "khz") hz *= 1000.0;
      if (!(hz >= 8.0 && hz <= 20000.0)) {
        t.notice("The root is 8 … 20000 Hz");
        return;
      }
      t.begin("Sampler Root");
      t.set(meta::kSmpRoot, hz);
      t.setData(meta::samplerWith(d.get(), "rootText", meta::DataValue::str({})));
      t.end();
      return;
    }
  }
  // An identity: "<name>" or "<name> <±cents>[c|¢|cents]".
  std::string name = in;
  double cents = 0.0;
  if (const size_t sp = in.find_last_of(' '); sp != std::string::npos && sp + 1 < in.size() && (in[sp + 1] == '+' || in[sp + 1] == '-')) {
    const std::string tail = in.substr(sp + 1);
    char* end = nullptr;
    const double c = std::strtod(tail.c_str(), &end);
    const std::string_view unit(end);
    if (end != tail.c_str() && (unit.empty() || unit == "c" || unit == "\xC2\xA2" || unit == "cents" || unit == "ct")) {
      cents = c;
      name = in.substr(0, sp);
    }
  }
  std::vector<PitchGuide> gs;
  t.pitchGuides(8.0f, 20000.0f, gs);
  for (const PitchGuide& x : gs) {
    const std::string pn = t.pitchName(x);
    if (lowerTrim(pn) != name) continue;
    const double hz = static_cast<double>(x.hz) * std::exp2(cents / 1200.0);
    if (!(hz >= 8.0 && hz <= 20000.0)) break;
    t.begin("Sampler Root");
    t.set(meta::kSmpRoot, hz);
    t.setData(meta::samplerWith(d.get(), "rootText", meta::DataValue::str(pn + centsText(cents))));
    t.end();
    return;
  }
  t.notice("'" + in + "' is neither Hz nor a pitch of the track's tuning");
}

bool SamplerFace::onKey(const ui::KeyEvent& e) {
  if (!typing_ || !e.down) return false;
  if (e.key == ui::key::kEscape) {
    typing_ = false;
    typed_.clear();
  } else if (e.key == ui::key::kBackspace) {
    if (!typed_.empty()) typed_.pop_back();
  } else if (e.key == ui::key::kReturn) {
    commitRoot();
  }
  return true;  // the field holds the keys while typing
}

void SamplerFace::refreshWhole() const {
  const MetaTarget& t = target();
  const uint32_t sample = meta::samplerSample(t.data().get());
  const media::WaveformOverview* o = sample != 0 ? t.sampleOverview(sample) : nullptr;
  const uint64_t built = o != nullptr ? o->builtFrames() : 0, rev = t.overviewRevision();
  if (sample == whole_.sample && o == whole_.overview && built == whole_.built && rev == whole_.revision) return;
  whole_.sample = sample;
  whole_.overview = o;
  whole_.built = built;
  whole_.revision = rev;
  whole_.lo.clear();
  whole_.hi.clear();
  if (o == nullptr || o->frames() == 0 || o->levelCount() == 0 || built == 0) return;
  const auto n = static_cast<size_t>(kEdX1 - kEdX0);  // a bar per world unit
  const double perCol = static_cast<double>(o->frames()) / static_cast<double>(n);
  size_t lv = 0;  // the coarsest level whose buckets are no wider than a column
  for (size_t i = 0; i < o->levelCount(); ++i)
    if (static_cast<double>(o->level(i).framesPerBucket) <= std::max(1.0, perCol)) lv = i;
  const media::WaveformOverview::Level& L = o->level(lv);
  const uint64_t valid = o->validBuckets(lv);
  if (L.framesPerBucket == 0 || valid == 0) return;
  whole_.lo.assign(n, 0.0f);
  whole_.hi.assign(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    const auto b0 = static_cast<uint64_t>(perCol * static_cast<double>(i) / static_cast<double>(L.framesPerBucket));
    auto b1 = static_cast<uint64_t>(std::ceil(perCol * static_cast<double>(i + 1) / static_cast<double>(L.framesPerBucket)));
    b1 = std::min(std::max(b1, b0 + 1), valid);
    int32_t lo = 32767, hi = -32768;
    for (uint32_t c = 0; c < o->channels(); ++c) {
      const int16_t* mm = o->minMax(lv, c);
      for (uint64_t b = b0; b < b1; ++b) {
        lo = std::min<int32_t>(lo, mm[2 * b]);
        hi = std::max<int32_t>(hi, mm[2 * b + 1]);
      }
    }
    if (lo > hi) lo = hi = 0;
    whole_.lo[i] = static_cast<float>(lo) / 32767.0f;
    whole_.hi[i] = static_cast<float>(hi) / 32767.0f;
  }
}

void SamplerFace::paintEditor(ui::FacePainter& p, const ui::Theme& th, double now) {
  refreshWhole();
  const Geometry g = geometry();
  const Rgba white = ui::rgba(255, 255, 255);
  // The wells: the ruler's and the waveform's.
  p.roundedRect({kEdX0 - 4.0f, kRulerY0, kEdX1 + 4.0f, kRulerY1}, 3.0f, ui::mulAlpha(th.face.track, 0.55f));
  p.roundedRect({kEdX0 - 4.0f, kWaveY0, kEdX1 + 4.0f, kWaveY1}, 5.0f, ui::mulAlpha(th.face.track, 0.85f));
  const float mid = (kWaveY0 + kWaveY1) * 0.5f, half = (kWaveY1 - kWaveY0) * 0.5f - 4.0f;
  if (whole_.lo.empty()) {
    p.line({kEdX0, mid}, {kEdX1, mid}, 0.8f, ui::mulAlpha(th.face.ink, 0.4f));
    p.text({(kEdX0 + kEdX1) * 0.5f, mid - 10.0f}, cols_.sample == 0 ? "drop a sample" : "reading the sample", 7.0f,
           ui::mulAlpha(th.face.ink, 0.55f));
  } else {
    const float step = (kEdX1 - kEdX0) / static_cast<float>(whole_.lo.size());
    const double a = std::min(g.start, g.end), b = std::max(g.start, g.end);
    for (size_t i = 0; i < whole_.lo.size(); ++i) {
      const float x = kEdX0 + step * (static_cast<float>(i) + 0.5f);
      const double u = (static_cast<double>(i) + 0.5) / static_cast<double>(whole_.lo.size());
      const float y0 = mid - whole_.hi[i] * half, y1 = mid - whole_.lo[i] * half;
      const Rgba col = u >= a && u <= b ? ui::mulAlpha(th.face.signal, 0.9f) : ui::mulAlpha(th.face.ink, 0.28f);
      p.line({x, std::min(y0, y1 - 0.6f)}, {x, std::max(y1, y0 + 0.6f)}, step * 0.9f, col);
    }
  }
  // The ruler: seconds (warp Off) or the sample's beats through its markers (bars every 4 beats, numbered).
  if (g.frames > 0.0) {
    const double len = g.frames / g.rate;
    const Rgba tick = ui::mulAlpha(th.face.ink, 0.45f), strong = ui::mulAlpha(th.face.ink, 0.8f);
    const float labelY = (kRulerY0 + kRulerY1) * 0.5f;
    if (g.warp == SamplerWarp::Off) {
      double stepS = 60.0;  // the finest step leaving ≥ 28 units between ticks
      for (const double c : {0.01, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0})
        if (static_cast<double>(kEdX1 - kEdX0) * c / len >= 28.0) {
          stepS = c;
          break;
        }
      for (int k = 0; k * stepS <= len + 1e-9 && k < 4000; ++k) {
        const float x = edX(k * stepS / len);
        p.line({x, kRulerY1 - 4.0f}, {x, kRulerY1 - 0.5f}, 0.6f, tick);
        char lb[16];
        std::snprintf(lb, sizeof lb, stepS < 1.0 ? "%.2f" : "%.0f", k * stepS);
        if (x + 14.0f < kEdX1) p.textAligned({x + 1.5f, labelY}, lb, 5.5f, tick, 0.0f);
      }
    } else {
      dsp::WarpSource src;
      src.fileRate = g.rate;
      const bool useMarkers = markersShown();
      src.markers = useMarkers ? g.markers.data() : nullptr;
      src.markerCount = useMarkers ? static_cast<uint32_t>(g.markers.size() / 2) : 0;
      const double fpb = 60.0 * g.rate / g.sampleBpm;
      const double b0 = dsp::warpBeatOfFrame(src, 0.0, fpb), b1 = dsp::warpBeatOfFrame(src, g.frames, fpb);
      const double perBeat = static_cast<double>(kEdX1 - kEdX0) / std::max(1e-6, b1 - b0);
      const double every = perBeat >= 6.0 ? 1.0 : perBeat >= 1.5 ? 4.0 : 16.0;
      for (double q = std::ceil(b0 / every) * every; q <= b1 + 1e-9 && q - b0 < 4096.0; q += every) {
        const float x = edX(dsp::warpFrameOfBeat(src, q, fpb) / g.frames);
        const bool bar = std::fabs(std::fmod(q, 4.0)) < 1e-9;
        p.line({x, bar ? kRulerY0 + 2.0f : kRulerY1 - 4.0f}, {x, kRulerY1 - 0.5f}, bar ? 0.8f : 0.6f, bar ? strong : tick);
        if (bar && perBeat * 4.0 >= 14.0 && x + 10.0f < kEdX1) {
          char lb[16];
          std::snprintf(lb, sizeof lb, "%d", static_cast<int>(std::floor(q / 4.0)) + 1);
          p.textAligned({x + 1.5f, labelY}, lb, 5.5f, strong, 0.0f);
        }
      }
      if (useMarkers)  // the warp markers: a pin on the ruler, a faint line through the waveform
        for (size_t i = 0; i + 1 < g.markers.size(); i += 2) {
          const float x = edX(static_cast<double>(g.markers[i]) / g.frames);
          const int32_t part = kPMarker0 + static_cast<int32_t>(i / 2);
          const bool hot = hovered_ == part || drag_ == part;
          p.line({x, kWaveY0}, {x, kWaveY1}, 0.6f, ui::mulAlpha(th.face.value, 0.35f));
          p.triangle({x - 3.5f, kRulerY0 + 1.0f}, {x + 3.5f, kRulerY0 + 1.0f}, {x, kRulerY1 - 1.0f}, hot ? th.face.hover : th.face.value);
        }
    }
  }
  // The loop (Classic with a loop): its bar along the waveform's top, its fade shaded before both ends.
  if (g.loop) {
    const float a = edX(g.loopStart), b = edX(g.loopEnd);
    if (g.loopFade > 0.0 && g.frames > 0.0) {
      const float w = static_cast<float>(g.loopFade / (g.frames / g.rate)) * (kEdX1 - kEdX0);
      for (const float e : {a, b})
        p.roundedRect({std::max(kEdX0, e - w), kLoopY1, std::max(kEdX0 + 0.5f, e), kWaveY1 - 1.0f}, 0.0f, ui::mulAlpha(th.face.value, 0.12f));
    }
    const bool hotBar = hovered_ == kPLoop || drag_ == kPLoop;
    p.roundedRect({a, kWaveY0 + 0.5f, std::max(b, a + 1.0f), kLoopY1}, 2.0f, ui::mulAlpha(th.face.value, hotBar ? 0.75f : 0.5f));
    for (const auto& [x, part] : {std::pair{a, kPLoopStart}, std::pair{b, kPLoopEnd}}) {
      const bool hot = hovered_ == part || drag_ == part;
      p.line({x, kWaveY0 + 0.5f}, {x, kWaveY1 - 0.5f}, hot ? 1.2f : 0.8f, ui::mulAlpha(th.face.value, hot ? 1.0f : 0.7f));
    }
  }
  // One-Shot / Slice fades (warp Off / Re-Pitch): shaded ramps after Start and before End (the sample's seconds).
  if (g.mode != SamplerMode::Classic && g.frames > 0.0 && g.warp <= SamplerWarp::RePitch) {
    const double len = g.frames / g.rate;
    const float sx = edX(g.start), ex = edX(g.end);
    if (g.fadeIn > 0.0) {
      const float fx = edX(g.start + g.fadeIn / len);
      const Vec2 tri[3] = {{sx, kWaveY0 + 1.0f}, {fx, kWaveY0 + 1.0f}, {sx, kWaveY1 - 1.0f}};
      p.convex(tri, ui::mulAlpha(th.face.body, 0.55f));
      p.line({sx, kWaveY1 - 1.0f}, {fx, kWaveY0 + 1.0f}, 0.8f, ui::mulAlpha(th.face.value, 0.8f));
    }
    if (g.fadeOut > 0.0) {
      const float fx = edX(g.end - g.fadeOut / len);
      const Vec2 tri[3] = {{fx, kWaveY0 + 1.0f}, {ex, kWaveY0 + 1.0f}, {ex, kWaveY1 - 1.0f}};
      p.convex(tri, ui::mulAlpha(th.face.body, 0.55f));
      p.line({fx, kWaveY0 + 1.0f}, {ex, kWaveY1 - 1.0f}, 0.8f, ui::mulAlpha(th.face.value, 0.8f));
    }
  }
  // Slice lines, numbered (slice n plays on key 36 + n − 1).
  if (slicesShown() && g.frames > 0.0)
    for (size_t i = 0; i < g.slices.size(); ++i) {
      const float x = edX(static_cast<double>(g.slices[i]) / g.frames);
      const int32_t part = kPSlice0 + static_cast<int32_t>(i);
      const bool hot = hovered_ == part || drag_ == part;
      p.line({x, kWaveY0 + 1.0f}, {x, kWaveY1 - 1.0f}, hot ? 1.3f : 0.9f, hot ? th.face.hover : ui::mulAlpha(white, 0.75f));
      const float nx = i + 1 < g.slices.size() ? edX(static_cast<double>(g.slices[i + 1]) / g.frames) : kEdX1;
      if (nx - x >= 9.0f) p.textAligned({x + 1.5f, kWaveY1 - 5.0f}, std::to_string(i + 1), 5.5f, ui::mulAlpha(th.face.ink, 0.8f), 0.0f);
    }
  // Start / End: a line through the waveform and a flag below it.
  for (const auto& [u, part] : {std::pair{g.start, kPStart}, std::pair{g.end, kPEnd}}) {
    const float x = edX(u);
    const bool hot = hovered_ == part || drag_ == part;
    const Rgba col = hot ? th.face.hover : th.face.handle;
    p.line({x, kWaveY0 + 0.5f}, {x, kWaveY1 - 0.5f}, hot ? 1.4f : 1.0f, col);
    const float d = part == kPStart ? 6.0f : -6.0f;
    p.triangle({x, kFlagY0}, {x + d, kFlagY0 + 4.5f}, {x, kFlagY1}, col);
  }
  // The voices' playheads (0 … 1 of the sample).
  for (const Shown& sv : shown_v_) {
    const float a = alphaOf(sv, now);
    const float x = edX(std::isfinite(sv.v.phase) ? static_cast<double>(sv.v.phase) : 0.0);
    p.line({x, kWaveY0 + 1.0f}, {x, kWaveY1 - 1.0f}, 1.2f, ui::mulAlpha(th.face.signal, a));
    p.disc({x, kWaveY0 + 1.5f}, 1.4f, ui::mulAlpha(white, a));
  }
  // The info line: the region's times, or the slices and their keys.
  if (g.frames > 0.0) {
    const double len = g.frames / g.rate;
    char b[96];
    if (g.mode == SamplerMode::Slice)
      std::snprintf(b, sizeof b, "%zu slices  \xC2\xB7  keys 36 \xE2\x80\xA6 %zu", g.slices.size(), 35 + std::max<size_t>(g.slices.size(), 1));
    else
      std::snprintf(b, sizeof b, "%.3f s \xE2\x80\x93 %.3f s  of %.3f s", g.start * len, g.end * len, len);
    p.textAligned({kEdX0, kFlagY1 + 5.0f}, b, 6.0f, ui::mulAlpha(th.face.ink, 0.75f), 0.0f);
  }
}

void SamplerFace::paintRoot(ui::FacePainter& p, const ui::Theme& th) {
  const meta::DataPtr d = target().data();
  const bool id = !meta::samplerRootText(d.get()).empty();
  p.roundedRect(kRootText, 3.0f, ui::lerp(th.face.body, th.face.track, 0.5f));
  const Rgba frame = typing_ ? th.circuit.selected : hovered_ == kPRootText ? th.face.hover : th.face.frame;
  const Vec2 ring[5] = {{kRootText.x0, kRootText.y0}, {kRootText.x1, kRootText.y0}, {kRootText.x1, kRootText.y1},
                        {kRootText.x0, kRootText.y1}, {kRootText.x0, kRootText.y0}};
  p.polyline(ring, 0.8f, frame);
  const Vec2 c{(kRootText.x0 + kRootText.x1) * 0.5f, (kRootText.y0 + kRootText.y1) * 0.5f};
  if (typing_) p.text(c, typed_ + "|", 7.0f, th.face.ink);
  else p.text(c, rootLabel(), 7.0f, th.face.value);
  struct Cell {
    Rect r;
    bool on;
    const char* label;
    int32_t part;
  };
  for (const Cell& k : {Cell{kRootHz, !id, "Hz", kPRootHz}, Cell{kRootId, id, "ID", kPRootId}}) {
    p.roundedRect(k.r, 3.0f, k.on ? th.face.value : ui::lerp(th.face.body, th.face.track, hovered_ == k.part ? 0.9f : 0.6f));
    p.text({(k.r.x0 + k.r.x1) * 0.5f, (k.r.y0 + k.r.y1) * 0.5f}, k.label, 6.0f, k.on ? th.face.body : th.face.ink);
  }
}

}  // namespace

std::unique_ptr<MetaFace> makeSamplerFace(const meta::Type& t) { return std::make_unique<SamplerFace>(t); }

size_t samplerFacePlayheads(const MetaFace& f) {
  const auto* s = dynamic_cast<const SamplerFace*>(&f);
  return s != nullptr ? s->playheads() : 0;
}

}  // namespace pq::views
