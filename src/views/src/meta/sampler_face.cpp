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

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "faces.h"
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

void ellipsePoints(std::vector<Vec2>& out, Vec2 c, float rx, float ry, bool close) {
  out.clear();
  for (int i = 0; i < kEllipseSteps + (close ? 1 : 0); ++i) {
    const float a = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(kEllipseSteps);
    out.push_back({c.x + rx * std::cos(a), c.y + ry * std::sin(a)});
  }
}

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
  }

  void liveRects(std::vector<Rect>& out) const override { out.push_back({kWell.x0 - 6.0f, kWell.y0 - 4.0f, kWell.x1 + 6.0f, kWell.y1 + 4.0f}); }
  bool animating() const noexcept override { return !shown_v_.empty(); }

  // ---- interaction: the frame only (the panel has the controls) ----
  int32_t hitTest(Vec2 l, float) const noexcept override { return shape().contains(l) ? kFrame : kNone; }
  bool onPointer(const FacePointer&) override { return false; }
  bool dragging() const noexcept override { return false; }
  void cancelDrag() override {}
  void describe(int32_t part, std::string& title, std::string& body) const override {
    if (part == kFrame || part == kNone) {
      title = "Sampler";
      body = cols_.sample == 0 ? "Drop an audio file on it to load a sample."
                               : "The sample from Start to End; each sounding voice's playhead glows where it reads.";
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

  std::vector<Shown> shown_v_;
  mutable Columns cols_;
  mutable std::vector<Vec2> pts_;
  VoiceTapReader* said_ = nullptr;  // the reader told whether this face is shown
  bool shown_ = true;
};

}  // namespace

std::unique_ptr<MetaFace> makeSamplerFace(const meta::Type& t) { return std::make_unique<SamplerFace>(t); }

size_t samplerFacePlayheads(const MetaFace& f) {
  const auto* s = dynamic_cast<const SamplerFace*>(&f);
  return s != nullptr ? s->playheads() : 0;
}

}  // namespace pq::views
