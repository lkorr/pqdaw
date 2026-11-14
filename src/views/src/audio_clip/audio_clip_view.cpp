// The audio Clip View (T5.6; T6.8 warping): see pq/views/audio_clip/audio_clip_view.h.

#include "pq/views/audio_clip/audio_clip_view.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>

#include "../common/paint.h"
#include "../common/waveform_data.h"
#include "pq/core/automation_curve.h"
#include "pq/document/audio_clip.h"
#include "pq/document/commands.h"
#include "pq/document/pitch_resolve.h"
#include "pq/media/probe.h"
#include "pq/ui/lines.h"
#include "pq/ui/theme.h"
#include "pq/ui/overlay.h"
#include "pq/ui/waveform.h"
#include "pq/views/audio_clip/clip_controls.h"
#include "pq/views/sample_analysis.h"
#include "pq/views/waveforms.h"

namespace pq::views {

using namespace pq::ui;

namespace {

constexpr float kBoxWidth = 384.0f;   // logical px: the three cards (AC.look), stacked in one column
constexpr float kBoxPadX = 8.0f;      // the box's side padding
constexpr float kBoxPadBottom = 8.0f;
constexpr float kHeaderH = 20.0f + 17.0f + 17.0f;  // the name strip, the file and the format lines above the cards
constexpr float kWarpCardW = 290.0f;  // AC.fix: card widths in a column of their own (the cards flowed side by side)
constexpr float kMinDisplayW = 240.0f;  // the waveform keeps at least this when the cards flow into columns
constexpr float kRulerH = 16.0f;      // the loop strip (start marker, loop brace)
constexpr float kTimeH = 16.0f;       // AC.look: the time ruler under it
constexpr float kGrab = 5.0f;          // logical px around the marker / brace ends

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

bool parseNumber(std::string_view s, double& out) {
  s = trim(s);
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc{} && p == s.data() + s.size();
}

std::filesystem::path utf8Path(std::string_view s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

}  // namespace

std::optional<doc::AudioTranspose> parseTranspose(std::string_view text) {
  std::string_view s = trim(text);
  if (s.empty()) return std::nullopt;
  doc::AudioTranspose t;
  auto endsWith = [&](std::string_view suf) { return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf; };
  double v = 0.0;
  if (endsWith("\xC2\xA2") || endsWith("c")) {  // cents
    const std::string_view num = endsWith("c") ? s.substr(0, s.size() - 1) : s.substr(0, s.size() - 2);
    if (!parseNumber(num, v)) return std::nullopt;
    t.unit = doc::TransposeUnit::Cents;
    t.cents = v;
    return t;
  }
  if (endsWith("st")) {  // semitones
    if (!parseNumber(s.substr(0, s.size() - 2), v)) return std::nullopt;
    t.unit = doc::TransposeUnit::Cents;
    t.cents = 100.0 * v;
    return t;
  }
  if (const size_t slash = s.find('/'); slash != std::string_view::npos) {
    int64_t n = 0, d = 0;
    const std::string_view a = trim(s.substr(0, slash)), b = trim(s.substr(slash + 1));
    const auto ra = std::from_chars(a.data(), a.data() + a.size(), n);
    const auto rb = std::from_chars(b.data(), b.data() + b.size(), d);
    if (ra.ec != std::errc{} || rb.ec != std::errc{} || ra.ptr != a.data() + a.size() || rb.ptr != b.data() + b.size() || n <= 0 ||
        d <= 0)
      return std::nullopt;
    t.unit = doc::TransposeUnit::Ratio;
    t.ratio = tuning::Rational(n, d);
    return t;
  }
  int64_t steps = 0;
  std::string_view num = s;
  if (!num.empty() && num.front() == '+') num.remove_prefix(1);
  const auto [p, ec] = std::from_chars(num.data(), num.data() + num.size(), steps);
  if (ec != std::errc{} || p != num.data() + num.size()) return std::nullopt;
  t.unit = doc::TransposeUnit::Steps;
  t.steps = static_cast<int32_t>(std::clamp<int64_t>(steps, -doc::kMaxTransposeSteps, doc::kMaxTransposeSteps));
  return t;
}

std::string formatTranspose(const doc::AudioTranspose& t) {
  char buf[48];
  switch (t.unit) {
    case doc::TransposeUnit::Steps: std::snprintf(buf, sizeof buf, "%d", t.steps); break;
    case doc::TransposeUnit::Cents:  // M6 review L19: "0 st" / "+7 st" for whole semitones, else "386.3 ¢"
      if (t.cents == std::round(t.cents / 100.0) * 100.0) std::snprintf(buf, sizeof buf, "%g st", t.cents / 100.0);
      else std::snprintf(buf, sizeof buf, "%g \xC2\xA2", t.cents);
      break;
    case doc::TransposeUnit::Ratio:
      std::snprintf(buf, sizeof buf, "%lld/%lld", static_cast<long long>(t.ratio.num()), static_cast<long long>(t.ratio.den()));
      break;
  }
  return buf;
}

std::string transposeReadout(const doc::AudioTranspose& t, double cents) {
  char buf[64];
  switch (t.unit) {
    case doc::TransposeUnit::Steps:
      if (t.steps == 0) std::snprintf(buf, sizeof buf, "0 stp");
      else std::snprintf(buf, sizeof buf, "%+d stp", t.steps);
      break;
    case doc::TransposeUnit::Cents:
      if (t.cents == 0.0) std::snprintf(buf, sizeof buf, "0 st");
      else if (t.cents == std::round(t.cents / 100.0) * 100.0) std::snprintf(buf, sizeof buf, "%+g st", t.cents / 100.0);
      else std::snprintf(buf, sizeof buf, "%+.1f \xC2\xA2", t.cents);
      break;
    case doc::TransposeUnit::Ratio:
      std::snprintf(buf, sizeof buf, "%lld/%lld \xC2\xB7 %.1f \xC2\xA2", static_cast<long long>(t.ratio.num()),
                    static_cast<long long>(t.ratio.den()), cents);
      break;
  }
  return buf;
}

double sourceBpmFor(double fileBpm, double durationSeconds) {
  if (fileBpm >= kMinBpm && fileBpm <= kMaxBpm) return fileBpm;
  if (!(durationSeconds > 0.0)) return 120.0;
  // Bars of 4/4 at 120 BPM (2 s each), rounded to a whole number (at least one); the tempo that makes it exact.
  const double bars = std::max(1.0, std::round(durationSeconds / 2.0));
  return std::clamp(bars * 4.0 * 60.0 / durationSeconds, kMinBpm, kMaxBpm);
}

int32_t loopBarsFor(double seconds, double bpm, double beatsPerBar) {
  if (!(seconds > 0.0) || !(beatsPerBar > 0.0)) return 1;
  const auto tempoOf = [&](int32_t bars) { return bars * beatsPerBar * 60.0 / seconds; };
  if (bpm > 0.0) {  // any whole number of bars: the nearest to the detected tempo, kept in range
    const double exact = bpm * seconds / (60.0 * beatsPerBar);
    int32_t best = 1;
    double bestD = 1e300;
    for (const double cand : {std::floor(exact), std::ceil(exact)}) {
      const auto bars = static_cast<int32_t>(std::clamp(cand, 1.0, 1024.0));
      const double t = tempoOf(bars);
      const double d = std::abs(std::log2(t / bpm)) + (t < kMinBpm || t > kMaxBpm ? 1e6 : 0.0);
      if (d < bestD - 1e-9) {
        bestD = d;
        best = bars;
      }
    }
    if (bestD < 1e6) return best;
  }
  int32_t best = 1;
  double bestD = 1e300;
  for (int32_t bars = 1; bars <= 64; bars *= 2) {
    const double t = tempoOf(bars);
    if (t < kMinBpm || t > kMaxBpm) continue;
    const double d = std::abs(std::log2(t / 120.0));
    if (d < bestD - 1e-9) {
      bestD = d;
      best = bars;
    }
  }
  return best;
}

// ---- the sample display ------------------------------------------------------------------------------------------

namespace audio_clip {

namespace {
constexpr float kWarpStripH = 16.0f;     // logical px: the warp markers' strip (warped clips)
constexpr double kEnvelopeSeconds = 0.025;  // AC.look: the waveform body's window (a solid body, not a trace)
constexpr double kConfident = 0.6;       // a detected tempo the warp commands trust (plan: auto-warp threshold)
}  // namespace

// An Off clip shows its whole file on a frame axis; a warped one its content on a content-tick axis
// [axis0_, axis1_] (the file's ends through the warp law, widened to the clip's region and loop), frozen during a warp
// marker drag so the view does not rescale under the pointer.
class SampleDisplay : public Control {
public:
  explicit SampleDisplay(AudioClipView& v) : Control("sample"), v_(v) {
    setFocusable(true);  // a press focuses it, so Esc / Delete reach it (M5 review M8)
    setInfo("Sample", "The sample, the part this clip plays lit. Drag the start marker in the strip above it to choose "
                      "where the clip starts; drag the loop brace or its ends (1/16 notes; Shift: free). Warped clips: "
                      "double-click to add a warp marker, drag one to stretch the audio around it, Ctrl-drag to slide "
                      "the audio under it, double-click or Delete removes it; right-click for Warp From Here and Warp "
                      "as Loop. Each edit is one undo step.");
  }
  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;
  bool onKey(const KeyEvent& e) override;
  void onCaptureLost() override { cancel(); }
  Cursor cursorAt(Vec2 p) const override {
    Drag d = drag_;
    if (d == Drag::None) d = hit(p).drag;
    switch (d) {
      case Drag::None: return Cursor::Arrow;
      case Drag::Brace: return Cursor::Move;
      default: return Cursor::ResizeEW;
    }
  }
  bool dragging() const noexcept { return drag_ != Drag::None; }
  bool cancel() {
    if (drag_ == Drag::None) return false;
    drag_ = Drag::None;
    frozen_ = false;
    if (gestureOpen_ && v_.ctx_.store().inGesture()) (void)v_.ctx_.store().cancelGesture();
    gestureOpen_ = false;
    markDirty();
    return true;
  }
  int32_t selected() const noexcept {
    return v_.clip_ && selected_ >= 0 && static_cast<size_t>(selected_) < v_.clip_->markers.size() ? selected_ : -1;
  }

  // Geometry (device px), from the last layout.
  bool warped() const noexcept { return v_.clip_ && doc::isWarped(v_.clip_->warp); }
  IRect ruler() const noexcept {
    const IRect r = rect();
    const int32_t h = tree() ? tree()->uiScale().px(kRulerH) : static_cast<int32_t>(kRulerH);
    return {r.x, r.y, r.w, std::min(h, r.h)};
  }
  // AC.look: the time ruler (bar.beat / seconds) under the loop strip.
  IRect timeline() const noexcept {
    const IRect r = rect(), u = ruler();
    const int32_t h = tree() ? tree()->uiScale().px(kTimeH) : static_cast<int32_t>(kTimeH);
    return {r.x, u.bottom(), r.w, std::max(0, std::min(h, r.bottom() - u.bottom()))};
  }
  IRect strip() const noexcept {
    const IRect r = rect(), u = timeline();
    if (!warped()) return {r.x, u.bottom(), r.w, 0};
    const int32_t h = tree() ? tree()->uiScale().px(kWarpStripH) : static_cast<int32_t>(kWarpStripH);
    return {r.x, u.bottom(), r.w, std::max(0, std::min(h, r.bottom() - u.bottom()))};
  }
  IRect wave() const noexcept {
    const IRect r = rect(), s = strip();
    return {r.x, s.bottom(), r.w, std::max(0, r.bottom() - s.bottom())};
  }
  double frames() const noexcept {
    const doc::SampleRef* s = sample();
    return s ? static_cast<double>(std::max<int64_t>(1, s->frames)) : 1.0;
  }
  double rate() const noexcept {
    const doc::SampleRef* s = sample();
    return s ? s->rate : 48000.0;
  }
  // Content (warped) of a source frame and back, through the clip's law.
  double contentOf(double frame) const noexcept { return v_.clip_ ? doc::warpContentAt(*v_.clip_, rate(), frame) : 0.0; }
  double frameOf(double content) const noexcept { return v_.clip_ ? doc::warpFrameAt(*v_.clip_, rate(), content) : 0.0; }
  void axis(double& a0, double& a1) const noexcept {
    if (frozen_) {
      a0 = axis0_;
      a1 = axis1_;
      return;
    }
    a0 = 0.0;
    a1 = 1.0;
    if (!v_.clip_ || !sample()) return;
    const doc::AudioClip& c = *v_.clip_;
    // The file, widened to what the clip plays (a looped clip: its brace; else its length from the start marker).
    const bool looped = contentLooped(doc::contentLoop(c));
    const double end = static_cast<double>(looped ? c.loop.end : c.contentStart + c.length);
    a0 = std::min({contentOf(0.0), static_cast<double>(c.contentStart), looped ? static_cast<double>(c.loop.start) : 1e300});
    a1 = std::max(contentOf(frames()), end);
    if (!(a1 > a0)) a1 = a0 + 1.0;
  }
  float xOfContent(double t) const noexcept {
    const IRect w = wave();
    double a0 = 0, a1 = 1;
    axis(a0, a1);
    return static_cast<float>(w.x) + static_cast<float>((t - a0) / (a1 - a0) * static_cast<double>(w.w));
  }
  double contentAtX(float x) const noexcept {
    const IRect w = wave();
    double a0 = 0, a1 = 1;
    axis(a0, a1);
    return w.w > 0 ? a0 + (static_cast<double>(x) - w.x) / static_cast<double>(w.w) * (a1 - a0) : a0;
  }
  float xOf(double frame) const noexcept {
    if (warped()) return xOfContent(contentOf(frame));
    const IRect w = wave();
    return static_cast<float>(w.x) + static_cast<float>(frame / frames() * static_cast<double>(w.w));
  }
  double frameAt(float x) const noexcept {
    if (warped()) return frameOf(contentAtX(x));
    const IRect w = wave();
    return w.w > 0 ? (static_cast<double>(x) - w.x) / static_cast<double>(w.w) * frames() : 0.0;
  }
  const doc::SampleRef* sample() const noexcept {
    return v_.clip_ && v_.snap_ ? doc::findSample(*v_.snap_, v_.clip_->sample) : nullptr;
  }
  double speedRatio() const noexcept {
    if (!v_.clip_ || !v_.snap_) return 1.0;
    const std::optional<doc::TrackPath> path = doc::findTrack(*v_.snap_, v_.ref_.track);
    return path ? doc::audioRateRatio(*v_.snap_, *path, *v_.clip_) : 1.0;
  }
  double markerFrame() const noexcept {
    if (!v_.clip_) return 0.0;
    const doc::AudioClip& c = *v_.clip_;
    return c.warp == doc::AudioWarp::Off ? c.contentStartFrame : frameOf(static_cast<double>(c.contentStart));
  }
  float startX() const noexcept {
    if (!v_.clip_) return 0.0f;
    return warped() ? xOfContent(static_cast<double>(v_.clip_->contentStart)) : xOf(v_.clip_->contentStartFrame);
  }
  float warpMarkerX(size_t i) const noexcept {
    if (!v_.clip_ || i >= v_.clip_->markers.size()) return 0.0f;
    return xOfContent(static_cast<double>(v_.clip_->markers[i].content));
  }
  Menu* openMenu(Vec2 at);

private:
  enum class Drag : uint8_t { None, Marker, LoopStart, LoopEnd, Brace, WarpMove, WarpSlide };
  struct Hit {
    Drag drag = Drag::None;
    int32_t marker = -1;
  };
  float grab() const noexcept { return static_cast<float>(tree() ? tree()->uiScale().px(kGrab) : static_cast<int32_t>(kGrab)); }
  // The warp marker nearest x within reach (−1: none).
  int32_t markerNear(float x) const noexcept {
    if (!warped()) return -1;
    int32_t best = -1;
    float bd = grab() + 0.5f;
    for (size_t i = 0; i < v_.clip_->markers.size(); ++i) {
      const float d = std::abs(warpMarkerX(i) - x);
      if (d < bd) {
        bd = d;
        best = static_cast<int32_t>(i);
      }
    }
    return best;
  }
  Hit hit(Vec2 p) const {
    if (!v_.clip_ || !sample()) return {};
    const IRect u = ruler();
    const float g = grab();
    if (p.y < static_cast<float>(u.y) || p.y >= static_cast<float>(rect().bottom())) return {};
    const doc::AudioClip& c = *v_.clip_;
    if (p.y < static_cast<float>(u.bottom())) {  // the ruler: loop brace, start marker
      if (warped() && c.loop.enabled) {
        const float xa = xOfContent(static_cast<double>(c.loop.start)), xb = xOfContent(static_cast<double>(c.loop.end));
        if (std::abs(p.x - xa) <= g) return {Drag::LoopStart};
        if (std::abs(p.x - xb) <= g) return {Drag::LoopEnd};
        if (p.x > xa && p.x < xb && std::abs(p.x - startX()) > g) return {Drag::Brace};
      }
      if (std::abs(p.x - startX()) <= g) return {Drag::Marker};
      return {};
    }
    if (const int32_t m = markerNear(p.x); m >= 0) return {Drag::WarpMove, m};
    if (std::abs(p.x - startX()) <= g && !warped()) return {Drag::Marker};
    return {};
  }
  void dragTo(const PointerEvent& e);
  void doubleClick(Vec2 p);
  // AC.look: the waveform's columns (per channel: the peak body's [lo, hi] and the RMS, full scale 1), recomputed only
  // when what they depend on changes (the clip, the axis, the width, the overview / window revision): painting reads
  // them, it never rescans the sample.
  struct Columns {
    doc::AudioClipPtr clip;  // a shared pointer: a freed clip's address can never match a new one
    uint64_t revision = ~uint64_t{0};
    int32_t x = 0, w = -1;
    double a0 = 0.0, a1 = 0.0;
    bool samples = false;
    uint32_t channels = 0;
    std::vector<float> lo, hi, rms;  // channel c's column i at [c · w + i]; lo > hi: no audio there
    std::vector<float> sq, sq2;      // scratch: each column's mean square before the window; copies
    std::vector<float> lo0, hi0;     // scratch: the columns' own extremes
    std::vector<uint8_t> half;       // each column's window: ± this many columns (its own frames per column)
  };
  const Columns* columns(const WaveformData& data, bool samples);
  void paintWaveform(PaintContext& ctx, float xa, float xb, Rgba colour);
  void paintTimeline(PaintContext& ctx);
  void paintStretch(PaintContext& ctx);
  void paintFades(PaintContext& ctx, float xa, float xb);

  AudioClipView& v_;
  Columns cols_;
  Drag drag_ = Drag::None;
  float pressX_ = 0.0f;
  doc::AudioClipPtr pressClip_;
  int32_t marker_ = -1;    // the warp marker being dragged
  int32_t selected_ = -1;  // the selected warp marker (Delete)
  bool frozen_ = false;
  double axis0_ = 0.0, axis1_ = 1.0;
  bool gestureOpen_ = false;
};

const SampleDisplay::Columns* SampleDisplay::columns(const WaveformData& data, bool samples) {
  const IRect w = wave();
  double a0 = 0, a1 = 1;
  if (warped()) axis(a0, a1);
  Columns& k = cols_;
  const uint64_t rev = v_.ctx_.waveforms().revision();
  if (k.clip == v_.clip_ && k.revision == rev && k.x == w.x && k.w == w.w && k.a0 == a0 && k.a1 == a1 && k.samples == samples &&
      k.channels == data.channels)
    return &k;
  k.clip = v_.clip_;
  k.revision = rev;
  k.x = w.x;
  k.w = w.w;
  k.a0 = a0;
  k.a1 = a1;
  k.samples = samples;
  k.channels = data.channels;
  const auto cw = static_cast<size_t>(std::max(0, w.w));
  const size_t n = cw * data.channels;
  k.lo.assign(n, 1.0f);  // assign keeps the capacity: no allocation once grown
  k.hi.assign(n, -1.0f);
  k.rms.assign(n, 0.0f);
  k.sq.assign(n, 0.0f);
  k.sq2.assign(n, 0.0f);
  k.lo0.assign(n, 1.0f);
  k.hi0.assign(n, -1.0f);
  k.half.assign(cw, 0);
  if (n == 0) return &k;
  const double total = static_cast<double>(data.frames);
  constexpr float kFull = 1.0f / 32767.0f;
  // Pass 1: each column's [min, max] and mean square over the source frames under it (warped: through the warp law).
  const double envFrames = kEnvelopeSeconds * rate();
  for (size_t i = 0; i < cw; ++i) {
    const float x = static_cast<float>(w.x) + static_cast<float>(i);
    double f0 = frameAt(x), f1 = frameAt(x + 1.0f);
    if (f1 < f0) std::swap(f0, f1);
    // The window in this column's own columns (a stretched segment has fewer frames per column).
    k.half[i] = static_cast<uint8_t>(std::clamp(std::ceil(0.5 * envFrames / std::max(1e-9, f1 - f0)) - 1.0, 0.0, 64.0));
    if (f1 <= 0.0 || f0 >= total) continue;
    f0 = std::max(0.0, f0);
    f1 = std::min(total, std::max(f1, f0 + 1.0));
    const auto a = static_cast<int64_t>(std::floor(f0));
    const int64_t b = std::max(a + 1, static_cast<int64_t>(std::ceil(f1)));
    // The coarsest level whose buckets are no wider than the column (the finest one at least).
    size_t lvl = 0;
    for (size_t L = 0; L < data.levels.size(); ++L)
      if (static_cast<double>(data.levels[L].framesPerBucket) <= std::max(1.0, f1 - f0)) lvl = L;
    for (uint32_t c = 0; c < data.channels; ++c) {
      float mn = 1.0f, mx = -1.0f;
      double s2 = 0.0;
      int64_t cnt = 0;
      if (samples && a >= data.samples.frame0 && b <= data.samples.frame0 + data.samples.frames) {
        const float* p = data.samples.channel[c] + (a - data.samples.frame0);
        for (int64_t j = 0; j < b - a; ++j) {
          mn = std::min(mn, p[j]);
          mx = std::max(mx, p[j]);
          s2 += static_cast<double>(p[j]) * p[j];
        }
        cnt = b - a;
      } else if (!data.levels.empty() && data.levels[lvl].framesPerBucket > 0 && data.levels[lvl].minMax[c]) {
        // The overview keeps no RMS: each bucket counts as a sine over its range (mean square = half range² / 2).
        const WaveformLevel& L = data.levels[lvl];
        const auto fpb = static_cast<int64_t>(L.framesPerBucket);
        const int64_t ka = a / fpb, kb = std::min<int64_t>(static_cast<int64_t>(L.buckets), (b + fpb - 1) / fpb);
        for (int64_t q = ka; q < kb; ++q) {
          const float lo = static_cast<float>(L.minMax[c][2 * q]) * kFull, hi = static_cast<float>(L.minMax[c][2 * q + 1]) * kFull;
          mn = std::min(mn, lo);
          mx = std::max(mx, hi);
          const double h = 0.5 * static_cast<double>(hi - lo);
          s2 += 0.5 * h * h;
          ++cnt;
        }
      }
      if (cnt == 0 || mn > mx) continue;
      const size_t at = c * cw + i;
      k.lo[at] = mn;
      k.hi[at] = mx;
      k.sq[at] = static_cast<float>(s2 / static_cast<double>(cnt));
    }
  }
  // Pass 2: a window of kEnvelopeSeconds of source around each column (at least the column itself), so the peak body is
  // solid where the columns are narrower than a cycle of the sound, and the RMS is that window's.
  const uint8_t* hw = k.half.data();
  auto span = [&](size_t i, size_t h, size_t& j0, size_t& j1) {
    j0 = i >= h ? i - h : 0;
    j1 = std::min(cw - 1, i + h);
  };
  for (uint32_t c = 0; c < data.channels; ++c) {
    float* lo = k.lo.data() + c * cw;
    float* hi = k.hi.data() + c * cw;
    float* rm = k.rms.data() + c * cw;
    float* sq = k.sq.data() + c * cw;
    float* t = k.sq2.data() + c * cw;
    size_t j0 = 0, j1 = 0;
    // RMS over the window (sq: the mean squares), then smoothed over half of it (t: a copy).
    for (size_t i = 0; i < cw; ++i) {
      if (lo[i] > hi[i]) continue;  // no audio under this column: stays empty
      double sum = 0.0;
      size_t m = 0;
      span(i, hw[i], j0, j1);
      for (size_t j = j0; j <= j1; ++j)
        if (lo[j] <= hi[j]) {
          sum += sq[j];
          ++m;
        }
      rm[i] = static_cast<float>(std::sqrt(sum / static_cast<double>(m)));
    }
    for (size_t i = 0; i < cw; ++i) t[i] = rm[i];
    for (size_t i = 0; i < cw; ++i) {
      if (lo[i] > hi[i]) continue;
      double sum = 0.0;
      size_t m = 0;
      span(i, std::max<size_t>(1, hw[i] / 2u), j0, j1);
      for (size_t j = j0; j <= j1; ++j)
        if (lo[j] <= hi[j]) {
          sum += t[j];
          ++m;
        }
      rm[i] = static_cast<float>(sum / static_cast<double>(m));
    }
    // The windowed extremes (empty columns hold lo = 1, hi = −1 and so never widen a body): from copies (sq, t; the
    // originals also in lo0 / hi0).
    float* lo0 = k.lo0.data() + c * cw;
    float* hi0 = k.hi0.data() + c * cw;
    for (size_t i = 0; i < cw; ++i) {
      sq[i] = lo0[i] = lo[i];
      t[i] = hi0[i] = hi[i];
    }
    for (size_t i = 0; i < cw; ++i) {
      if (sq[i] > t[i] || hw[i] == 0) continue;
      float mn = sq[i], mx = t[i];
      span(i, hw[i], j0, j1);
      for (size_t j = j0; j <= j1; ++j) {
        mn = std::min(mn, sq[j]);
        mx = std::max(mx, t[j]);
      }
      lo[i] = mn;
      hi[i] = mx;
    }
    for (size_t i = 0; i < cw; ++i) {
      sq[i] = lo[i];
      t[i] = hi[i];
    }
    // A mean over the window turns the running extremes' steps (a cycle's peak entering or leaving) into a smooth
    // outline. It is capped by the extremes of the columns BEFORE it (2 windows back), so nothing grows ahead of an
    // attack (a warp marker lines up with the transient as drawn), and each column keeps at least its own extremes.
    for (size_t i = 0; i < cw; ++i) {
      if (sq[i] > t[i] || hw[i] == 0) continue;
      double sl = 0.0, sh = 0.0;
      size_t m = 0;
      span(i, hw[i], j0, j1);
      for (size_t j = j0; j <= j1; ++j)
        if (sq[j] <= t[j]) {
          sl += sq[j];
          sh += t[j];
          ++m;
        }
      float pastLo = lo0[i], pastHi = hi0[i];
      for (size_t j = i >= 2u * hw[i] ? i - 2u * hw[i] : 0; j < i; ++j)
        if (lo0[j] <= hi0[j]) {
          pastLo = std::min(pastLo, lo0[j]);
          pastHi = std::max(pastHi, hi0[j]);
        }
      lo[i] = std::min(lo0[i], std::max(pastLo, static_cast<float>(sl / static_cast<double>(m))));
      hi[i] = std::max(hi0[i], std::min(pastHi, static_cast<float>(sh / static_cast<double>(m))));
    }
  }
  return &k;
}

void SampleDisplay::paintWaveform(PaintContext& ctx, float xa, float xb, Rgba colour) {
  const Theme& th = *ctx.res.theme;
  DrawList& dl = ctx.dl;
  const IRect w = wave();
  const media::WaveformOverview* ov = v_.ctx_.waveforms().overview(v_.clip_->sample);
  if (!ov) {
    const FontId font = ctx.res.uiFonts ? ctx.res.uiFonts->small : kNoFont;
    paint::text(ctx, font, w, "Loading waveform\xE2\x80\xA6", th.face.ink, TextAlign::Center, 8, 8);
    return;
  }
  WaveformLevel lv[kMaxOverviewLevels];
  WaveformData data = waveformData(*ov, nullptr, lv);
  bool samples = false;
  if (waveformLevelFor(data, frames() / std::max(1, w.w)) < 0)  // deep zoom (a short file): the samples themselves
    if (const media::SampleWindow* sw = v_.ctx_.waveforms().window(v_.clip_->sample, 0, static_cast<int64_t>(frames()))) {
      data = waveformData(*ov, sw, lv);
      samples = true;
    }
  const Columns* k = columns(data, samples);
  if (!k || k->channels == 0 || k->w <= 0) return;
  const auto cw = static_cast<size_t>(k->w);
  const float wy0 = static_cast<float>(w.y), wy1 = static_cast<float>(w.bottom());
  const float laneH = (wy1 - wy0) / static_cast<float>(k->channels);
  const Rgba bg = th.face.body;
  const Rgba peakIn = lerp(bg, colour, 0.5f), rmsIn = lerp(colour, rgba(255, 255, 255), 0.16f);
  const Rgba peakOut = lerp(bg, colour, 0.17f), rmsOut = lerp(bg, colour, 0.3f);
  for (uint32_t c = 0; c < k->channels; ++c) {
    const float top = wy0 + laneH * static_cast<float>(c);
    const float mid = std::round(top + 0.5f * laneH);
    const float amp = 0.5f * laneH - static_cast<float>(ctx.scale.px(3.0f));
    if (c > 0) dl.addRectFilled({static_cast<float>(w.x), std::round(top), static_cast<float>(w.right()), std::round(top) + 1.0f}, th.face.track);
    dl.addRectFilled({static_cast<float>(w.x), mid, static_cast<float>(w.right()), mid + 1.0f}, withAlpha(th.face.tick, 90));
    const float* lo = k->lo.data() + c * cw;
    const float* hi = k->hi.data() + c * cw;
    const float* rm = k->rms.data() + c * cw;
    for (size_t i = 0; i < cw; ++i) {
      if (lo[i] > hi[i]) continue;
      const float x = static_cast<float>(w.x) + static_cast<float>(i);
      const bool in = x >= xa && x < xb;
      float y0 = std::round(mid - std::clamp(hi[i], -1.0f, 1.0f) * amp), y1 = std::round(mid - std::clamp(lo[i], -1.0f, 1.0f) * amp) + 1.0f;
      dl.addRectFilled({x, y0, x + 1.0f, y1}, in ? peakIn : peakOut);
      const float r = std::min(rm[i], std::max(std::abs(lo[i]), std::abs(hi[i])));
      y0 = std::max(y0, std::round(mid - r * amp));
      y1 = std::min(y1, std::round(mid + r * amp) + 1.0f);
      if (y1 > y0) dl.addRectFilled({x, y0, x + 1.0f, y1}, in ? rmsIn : rmsOut);
    }
  }
}

// The time ruler: warped, bar.beat of the content (1 = content tick 0, 4/4 as the beat grid); Off, seconds of the file.
// Labels at least 40 px apart (beats when they fit, else every 1, 2, 4 … bars); ticks at bars / beats (and 16ths when
// they are 6 px apart).
void SampleDisplay::paintTimeline(PaintContext& ctx) {
  const Theme& th = *ctx.res.theme;
  DrawList& dl = ctx.dl;
  const IRect t = timeline(), w = wave();
  if (t.h <= 0 || w.w <= 0) return;
  const FontId font = ctx.res.uiFonts ? ctx.res.uiFonts->small : kNoFont;
  const float minGap = static_cast<float>(ctx.scale.px(40.0f));
  const float ty1 = static_cast<float>(t.bottom());
  const float major = static_cast<float>(ctx.scale.px(6.0f)), minor = static_cast<float>(ctx.scale.px(3.0f));
  const float tx0 = static_cast<float>(t.x), tx1 = static_cast<float>(t.right());
  char num[32];
  auto label = [&](float x, const char* s, Rgba col) {
    if (font == kNoFont || x < tx0 - 0.5f || x >= tx1 - static_cast<float>(ctx.scale.px(12.0f))) return;
    paint::text(ctx, font, {static_cast<int32_t>(x), t.y, t.right() - static_cast<int32_t>(x), t.h - ctx.scale.px(3.0f)}, s, col,
                TextAlign::Left, ctx.scale.px(3.0f), 0);
  };
  auto tick = [&](float x, float h, Rgba col) {
    if (x >= tx0 && x < tx1) dl.addRectFilled({x, ty1 - h, x + 1.0f, ty1}, col);
  };
  if (warped()) {
    double a0 = 0, a1 = 1;
    axis(a0, a1);
    const double pxPerTick = static_cast<double>(w.w) / (a1 - a0);
    const double pxPerBeat = pxPerTick * static_cast<double>(kPpq);
    // Ticks: 16ths, beats, bars, as dense as they stay apart.
    const Ticks sub = kPpq / 4;
    const Ticks tickStep = pxPerTick * static_cast<double>(sub) >= 6.0 ? sub : pxPerBeat >= 5.0 ? kPpq : kTicksPerWhole;
    for (Ticks k = static_cast<Ticks>(std::ceil(a0 / static_cast<double>(tickStep))) * tickStep; static_cast<double>(k) <= a1; k += tickStep) {
      const float x = std::round(xOfContent(static_cast<double>(k)));
      const bool bar = k % kTicksPerWhole == 0, beat = k % kPpq == 0;
      tick(x, bar ? major : beat ? minor + 1.0f : minor - 1.0f, bar ? th.face.tick : withAlpha(th.face.tick, beat ? 170 : 110));
    }
    if (pxPerBeat >= static_cast<double>(minGap)) {  // every beat: "1", "1.2", "1.3", "1.4", "2"
      for (Ticks k = static_cast<Ticks>(std::ceil(a0 / static_cast<double>(kPpq))) * kPpq; static_cast<double>(k) <= a1; k += kPpq) {
        const int64_t bar = (k >= 0 ? k : k - kTicksPerWhole + 1) / kTicksPerWhole;
        const int64_t beat = (k - bar * kTicksPerWhole) / kPpq;
        if (beat == 0) std::snprintf(num, sizeof num, "%lld", static_cast<long long>(bar + 1));
        else std::snprintf(num, sizeof num, "%lld.%lld", static_cast<long long>(bar + 1), static_cast<long long>(beat + 1));
        label(std::round(xOfContent(static_cast<double>(k))), num, beat == 0 ? th.text.secondary : th.face.ink);
      }
    } else {
      const double pxPerBar = pxPerTick * static_cast<double>(kTicksPerWhole);
      int64_t every = 1;
      while (pxPerBar * static_cast<double>(every) < static_cast<double>(minGap) && every < (int64_t{1} << 30)) every *= 2;
      int64_t first = static_cast<int64_t>(std::ceil(a0 / static_cast<double>(kTicksPerWhole)));
      if (const int64_t rem = ((first % every) + every) % every; rem != 0) first += every - rem;
      for (int64_t bar = first; static_cast<double>(bar * kTicksPerWhole) <= a1; bar += every) {
        std::snprintf(num, sizeof num, "%lld", static_cast<long long>(bar + 1));
        label(std::round(xOfContent(static_cast<double>(bar * kTicksPerWhole))), num, th.text.secondary);
      }
    }
  } else {
    const double secs = frames() / std::max(1.0, rate());
    const double pxPerSec = static_cast<double>(w.w) / std::max(1e-9, secs);
    static constexpr double kSteps[] = {0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
    double every = kSteps[std::size(kSteps) - 1];
    for (const double s : kSteps)
      if (pxPerSec * s >= static_cast<double>(minGap)) {
        every = s;
        break;
      }
    const double mant = every / std::pow(10.0, std::floor(std::log10(every)));
    const double minorEvery = every / (std::abs(mant - 2.0) < 1e-9 ? 4.0 : 5.0);
    if (pxPerSec * minorEvery >= 5.0)
      for (int64_t i = 0; static_cast<double>(i) * minorEvery <= secs; ++i)
        tick(std::round(xOf(static_cast<double>(i) * minorEvery * rate())), minor, withAlpha(th.face.tick, 130));
    for (int64_t i = 0; static_cast<double>(i) * every <= secs + 1e-9; ++i) {
      const double s = static_cast<double>(i) * every;
      const float x = std::round(xOf(s * rate()));
      tick(x, major, th.face.tick);
      if (s >= 60.0) std::snprintf(num, sizeof num, "%d:%02d", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
      else std::snprintf(num, sizeof num, "%gs", s);
      label(x, num, th.text.secondary);
    }
  }
}

// Each segment between warp markers tinted by its speed against the source tempo (faster: accent.arm, slower:
// accent.solo; alpha by |log2 speed|, full at an octave), with its speed in the warp strip when it fits. The parts
// before the first and after the last marker run at their neighbour's slope (warpFrameAt).
void SampleDisplay::paintStretch(PaintContext& ctx) {
  const doc::AudioClip& c = *v_.clip_;
  if (c.markers.size() < 2) return;
  const Theme& th = *ctx.res.theme;
  DrawList& dl = ctx.dl;
  const IRect st = strip(), w = wave();
  const float wx0 = static_cast<float>(w.x), wx1 = static_cast<float>(w.right());
  const double fpt = doc::framesPerContentTick(rate(), c.sourceBpm);
  if (!(fpt > 0.0)) return;
  const FontId font = ctx.res.uiFonts ? ctx.res.uiFonts->small : kNoFont;
  char buf[24];
  for (size_t i = 0; i + 1 < c.markers.size(); ++i) {
    const doc::WarpMarker &m0 = c.markers[i], &m1 = c.markers[i + 1];
    const double dc = static_cast<double>(m1.content - m0.content);
    if (!(dc > 0.0)) continue;
    const double speed = (m1.sourceFrame - m0.sourceFrame) / dc / fpt;
    if (!(speed > 0.0)) continue;
    const double oct = std::abs(std::log2(speed));
    float x0 = std::round(warpMarkerX(i)), x1 = std::round(warpMarkerX(i + 1));
    if (i == 0) x0 = wx0;                          // before the first marker: this segment's slope
    if (i + 2 == c.markers.size()) x1 = wx1;       // after the last: the last segment's
    x0 = std::clamp(x0, wx0, wx1);
    x1 = std::clamp(x1, wx0, wx1);
    if (!(x1 > x0)) continue;
    const Rgba hue = speed > 1.0 ? th.accent.arm : th.accent.solo;
    if (oct > 0.004) {  // the red reads stronger than the blue at one alpha: each its own scale
      const double t = std::min(1.0, oct);
      const auto a = static_cast<uint32_t>(speed > 1.0 ? 12.0 + 40.0 * t : 22.0 + 60.0 * t);
      dl.addRectFilled({x0, static_cast<float>(st.y), x1, static_cast<float>(w.bottom())}, withAlpha(hue, a));
    }
    // The speed, centred between the two markers.
    const float mx0 = std::round(warpMarkerX(i)), mx1 = std::round(warpMarkerX(i + 1));
    if (font != kNoFont && st.h > 0 && mx1 - mx0 >= static_cast<float>(ctx.scale.px(52.0f))) {
      std::snprintf(buf, sizeof buf, "%.2f\xC3\x97", speed);
      paint::text(ctx, font, {static_cast<int32_t>(mx0), st.y, static_cast<int32_t>(mx1 - mx0), st.h}, buf,
                  oct > 0.004 ? lerp(hue, th.face.handle, 0.45f) : th.face.ink, TextAlign::Center, 6, 6);
    }
  }
}

// The fades as curves over the waveform's ends (the arrangement's automation-curve law), the faded part shaded. A
// reversed clip plays the region backwards: its fade-in is at the region's right end. A looped clip's fade-out happens
// in some later pass of the loop, so only its fade-in is drawn.
void SampleDisplay::paintFades(PaintContext& ctx, float xa, float xb) {
  const doc::AudioClip& c = *v_.clip_;
  const Theme& th = *ctx.res.theme;
  DrawList& dl = ctx.dl;
  const IRect w = wave();
  const float wy0 = static_cast<float>(w.y) + 1.0f, wy1 = static_cast<float>(w.bottom()) - 1.0f;
  const Ticks fi = std::clamp<Ticks>(c.fadeIn.length, 0, c.length), fo = std::clamp<Ticks>(c.fadeOut.length, 0, c.length - fi);
  // A fade's width in px from the region's start (in) or end (out).
  auto width = [&](Ticks len, bool atStart) -> float {
    if (len <= 0) return 0.0f;
    if (warped()) {
      const double s = static_cast<double>(c.contentStart);
      const double e = static_cast<double>(c.contentStart + c.length);
      return atStart ? xOfContent(s + static_cast<double>(len)) - xOfContent(s) : xOfContent(e) - xOfContent(e - static_cast<double>(len));
    }
    const TempoMap& tm = v_.snap_->timeline->tempoMap;
    const double secs = atStart ? tm.ticksToSeconds(c.start + len) - tm.ticksToSeconds(c.start)
                                : tm.ticksToSeconds(c.start + c.length) - tm.ticksToSeconds(c.start + c.length - len);
    return xOf(c.contentStartFrame + secs * rate() * speedRatio()) - xOf(c.contentStartFrame);
  };
  const bool looped = warped() && contentLooped(doc::contentLoop(c));
  const float inW = width(fi, true), outW = looped ? 0.0f : width(fo, false);
  const Rgba shade = rgba(0, 0, 0, 96), line = withAlpha(th.face.handle, 150);
  const float lw = std::max(1.0f, ctx.scale.factor);
  Vec2 pts[65];
  auto fade = [&](float x0, float fw, float curve, bool rising) {
    if (fw < 1.0f) return;
    const int n = std::clamp(static_cast<int>(fw / 3.0f), 2, 64);
    for (int i = 0; i <= n; ++i) {
      const double t = static_cast<double>(i) / n;
      const double g = rising ? automationShape(t, curve) : 1.0 - automationShape(t, curve);
      pts[i] = {x0 + fw * static_cast<float>(t), wy1 - static_cast<float>(g) * (wy1 - wy0)};
    }
    for (int i = 0; i < n; ++i) {
      const Vec2 q[4] = {{pts[i].x, wy0}, {pts[i + 1].x, wy0}, pts[i + 1], pts[i]};
      dl.addConvexPolyFilled(q, shade);
    }
    smoothPolyline(dl, pts, static_cast<uint32_t>(n + 1), line, lw);
  };
  // Forward: in at the region's start rising, out at its end falling; reversed: mirrored.
  if (!c.reverse) {
    fade(xa, inW, c.fadeIn.curve, true);
    fade(xb - outW, outW, c.fadeOut.curve, false);
  } else {
    fade(xb - inW, inW, c.fadeIn.curve, false);
    fade(xa, outW, c.fadeOut.curve, true);
  }
}

void SampleDisplay::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  DrawList& dl = ctx.dl;
  const IRect r = rect();
  const Rgba bg = th->face.body;
  dl.addRectFilled(r.toRect(), bg);
  const IRect u = ruler(), tl = timeline(), st = strip(), w = wave();
  dl.addRectFilled(u.toRect(), lerp(bg, th->face.track, 0.75f));
  dl.addRectFilled(tl.toRect(), lerp(bg, th->face.track, 0.4f));
  if (st.h > 0) dl.addRectFilled(st.toRect(), lerp(bg, th->face.track, 0.18f));
  dl.addRectFilled({static_cast<float>(r.x), static_cast<float>(tl.bottom()) - 1.0f, static_cast<float>(r.right()), static_cast<float>(tl.bottom())},
                   th->face.track);
  if (!v_.clip_) return;
  const doc::AudioClip& c = *v_.clip_;
  const doc::SampleRef* s = sample();
  const Rgba colour = hex(c.colour.rgb);
  const FontId font = ctx.res.uiFonts ? ctx.res.uiFonts->small : kNoFont;
  if (!s || !v_.ctx_.sampleAvailable(c.sample)) {
    dl.addRectFilled(w.toRect(), rgba(200, 40, 35, 90));
    paint::text(ctx, font, w, "Sample missing: " + (s ? s->path : std::string("?")), th->text.primary, TextAlign::Center, 8, 8);
    return;
  }
  const bool isWarped = warped();
  const float wx0 = static_cast<float>(w.x), wx1 = static_cast<float>(w.right());
  const float wy0 = static_cast<float>(w.y), wy1 = static_cast<float>(w.bottom());
  // The region the clip plays: subtly lit in the clip colour (the rest stays dark and its waveform dim).
  float xa = 0.0f, xb = 0.0f;
  if (isWarped) {
    const Ticks end = contentLooped(doc::contentLoop(c)) ? c.loop.end : c.contentStart + c.length;
    xa = xOfContent(static_cast<double>(c.contentStart));
    xb = xOfContent(static_cast<double>(end));
  } else {
    xa = xOf(c.contentStartFrame);
    xb = xOf(c.contentStartFrame + c.seconds * s->rate * speedRatio());
  }
  xa = std::clamp(std::round(xa), wx0, wx1);
  xb = std::clamp(std::round(xb), wx0, wx1);
  if (xb > xa) {
    dl.addRectFilled({xa, wy0, xb, wy1}, lerp(bg, colour, 0.075f));
    if (st.h > 0) dl.addRectFilled({xa, static_cast<float>(st.y), xb, static_cast<float>(st.bottom())}, withAlpha(colour, 18));
  }
  if (isWarped) paintStretch(ctx);
  // Warped: the content's beat grid (bars brighter), where beats are at least 6 px apart.
  if (isWarped) {
    double a0 = 0, a1 = 1;
    axis(a0, a1);
    const double pxPerTick = static_cast<double>(w.w) / (a1 - a0);
    Ticks step = kPpq;
    while (static_cast<double>(step) * pxPerTick < 6.0 && step < (Ticks{1} << 40)) step *= 2;
    for (Ticks t = static_cast<Ticks>(std::ceil(a0 / static_cast<double>(step))) * step; static_cast<double>(t) <= a1; t += step) {
      const float x = std::round(xOfContent(static_cast<double>(t)));
      if (x < wx0 || x >= wx1) continue;
      const bool bar = t % kTicksPerWhole == 0;
      dl.addRectFilled({x, wy0, x + 1.0f, wy1}, bar ? withAlpha(th->face.tick, 110) : withAlpha(th->face.tick, 45));
    }
  }
  // The sample's transients: faint lines (one per pixel column at most), a tick at the strip's foot.
  if (const SampleAnalysisResult* an = v_.ctx_.sampleAnalysis().get(c.sample); an && an->transients) {
    const Rgba tc = withAlpha(th->face.handle, 26), tick = withAlpha(th->face.handle, 70);
    float last = -1e9f;
    for (const double f : an->transients->onsets) {
      const float x = std::round(xOf(f));
      if (x < wx0 || x >= wx1 || x == last) continue;
      last = x;
      dl.addRectFilled({x, wy0, x + 1.0f, wy1}, tc);
      if (st.h > 0) dl.addRectFilled({x, static_cast<float>(st.bottom() - st.h / 3), x + 1.0f, static_cast<float>(st.bottom())}, tick);
    }
  }
  paintWaveform(ctx, xa, xb, colour);
  paintFades(ctx, xa, xb);
  paintTimeline(ctx);
  // Region edges.
  if (xb > xa) {
    const Rgba edge = withAlpha(th->face.handle, 110);
    dl.addRectFilled({xa, wy0, xa + 1.0f, wy1}, edge);
    dl.addRectFilled({xb - 1.0f, wy0, xb, wy1}, edge);
  }
  // Warped: the loop brace in the loop strip (lit when on), its ends as grips.
  if (isWarped) {
    const float la = std::round(xOfContent(static_cast<double>(c.loop.start))), lb = std::round(xOfContent(static_cast<double>(c.loop.end)));
    const Rgba braceCol = c.loop.enabled ? th->face.value : th->face.tick;
    const float by0 = static_cast<float>(u.y) + static_cast<float>(ctx.scale.px(4.0f)), by1 = static_cast<float>(u.bottom()) - static_cast<float>(ctx.scale.px(4.0f));
    if (lb > la) {
      dl.addRoundedRectFilled({la, by0, lb, by1}, ctx.scale.exact(2.0f), withAlpha(braceCol, c.loop.enabled ? 150 : 90));
      const float gw = std::max(2.0f, ctx.scale.exact(2.0f));
      dl.addRectFilled({la, static_cast<float>(u.y + 1), la + gw, static_cast<float>(u.bottom() - 1)}, braceCol);
      dl.addRectFilled({lb - gw, static_cast<float>(u.y + 1), lb, static_cast<float>(u.bottom() - 1)}, braceCol);
    }
  }
  // The clip gain as a readout (the waveform is drawn at unity).
  if (c.gainDb != 0.0f && font != kNoFont) {
    char g[32];
    if (c.gainDb <= doc::kMinClipGainDb) std::snprintf(g, sizeof g, "Gain -inf dB");
    else std::snprintf(g, sizeof g, "Gain %+.1f dB", static_cast<double>(c.gainDb));
    paint::text(ctx, font, {w.x, w.y + ctx.scale.px(4.0f), w.w, ctx.scale.px(16.0f)}, g, th->text.secondary, TextAlign::Right,
                ctx.scale.px(8.0f), ctx.scale.px(8.0f));
  }
  // Warp markers: a tag with a point in the strip (Live's yellow), a line over the waveform; the selected one light.
  if (isWarped && st.h > 0) {
    const Rgba pin = th->accent.activator;
    const float hw = std::max(3.0f, ctx.scale.exact(4.5f));
    const float sy0 = static_cast<float>(st.y) + 2.0f, sy1 = static_cast<float>(st.bottom());
    const float neck = sy1 - std::max(3.0f, ctx.scale.exact(4.5f));
    for (size_t i = 0; i < c.markers.size(); ++i) {
      const float x = std::round(warpMarkerX(i));
      if (x < wx0 - hw || x > wx1 + hw) continue;
      const bool sel = static_cast<int32_t>(i) == selected();
      const Rgba body = sel ? th->face.handle : pin;
      dl.addRectFilled({x, wy0, x + 1.0f, wy1}, withAlpha(body, sel ? 230 : 150));
      dl.addRoundedRectFilled({x - hw + 0.5f, sy0, x + hw + 0.5f, neck}, ctx.scale.exact(2.0f), body);
      dl.addTriangleFilled({x - hw + 0.5f, neck - 0.5f}, {x + hw + 0.5f, neck - 0.5f}, {x + 0.5f, sy1}, body);
      dl.addRectFilled({x, sy0 + 2.0f, x + 1.0f, neck - 1.0f}, withAlpha(th->face.body, 120));  // a grip line
    }
  }
  // The start marker (M5 review UI 2): a flag in the accent colour filling the loop strip, and a 2-px line down.
  const float mx = std::round(startX());
  const float fh = static_cast<float>(u.h);
  const float sz = std::max(static_cast<float>(ctx.scale.px(9.0f)), fh * 0.75f);
  const float lw = static_cast<float>(std::max(2, ctx.scale.px(2.0f)));
  const Rgba flag = th->accent.primary;
  dl.addTriangleFilled({mx, static_cast<float>(u.y)}, {mx + sz, static_cast<float>(u.y) + 0.5f * fh}, {mx, static_cast<float>(u.bottom())}, flag);
  dl.addRectFilled({mx, static_cast<float>(u.y), mx + lw, wy1}, flag);
}

bool SampleDisplay::onKey(const KeyEvent& e) {
  if (!e.down) return false;
  if ((e.key == key::kDelete || e.key == key::kBackspace) && drag_ == Drag::None && selected() >= 0) {
    const auto i = static_cast<size_t>(selected());
    selected_ = -1;
    (void)v_.removeWarpMarker(i);
    return true;
  }
  return false;
}

void SampleDisplay::doubleClick(Vec2 p) {
  if (!warped() || p.y < static_cast<float>(ruler().bottom())) return;
  if (const int32_t m = markerNear(p.x); m >= 0) {  // on a marker: remove it (Live)
    selected_ = -1;
    (void)v_.removeWarpMarker(static_cast<size_t>(m));
    return;
  }
  // At a transient within reach, else where clicked.
  double content = contentAtX(p.x);
  if (const SampleAnalysisResult* an = v_.ctx_.sampleAnalysis().get(v_.clip_->sample); an && an->transients) {
    float bd = grab() + 0.5f;
    for (const double f : an->transients->onsets) {
      const float d = std::abs(xOf(f) - p.x);
      if (d < bd) {
        bd = d;
        content = contentOf(f);
      }
    }
  }
  const auto t = static_cast<Ticks>(std::llround(content));
  if (v_.dispatch(doc::ops::addWarpMarker(*v_.ctx_.doc(), v_.ref_, t), "Add Warp Marker") && v_.clip_) {
    for (size_t i = 0; i < v_.clip_->markers.size(); ++i)
      if (v_.clip_->markers[i].content == t) selected_ = static_cast<int32_t>(i);
  }
  markDirty();
}

bool SampleDisplay::onPointer(const PointerEvent& e) {
  switch (e.type) {
    case PointerType::Down: {
      if (e.button == PointerButton::Right) {
        if (e.pos.y >= static_cast<float>(ruler().bottom())) (void)openMenu(e.pos);
        return true;
      }
      if (e.button != PointerButton::Left) return false;
      if (e.clickCount >= 2) {
        doubleClick(e.pos);
        return true;
      }
      const Hit h = hit(e.pos);
      if (h.drag == Drag::None) {
        if (selected_ >= 0) {
          selected_ = -1;
          markDirty();
        }
        return true;
      }
      drag_ = h.drag;
      marker_ = h.marker;
      if (drag_ == Drag::WarpMove) {
        selected_ = marker_;
        if ((e.mods & mod::kCtrl) != 0) drag_ = Drag::WarpSlide;
      }
      pressX_ = e.pos.x;
      pressClip_ = v_.clip_;
      frozen_ = false;
      axis(axis0_, axis1_);
      frozen_ = true;  // the axis stays as pressed until the drag ends
      gestureOpen_ = false;
      markDirty();
      return true;
    }
    case PointerType::Move:
      if (drag_ != Drag::None) dragTo(e);
      return drag_ != Drag::None;
    case PointerType::Up:
      if (drag_ == Drag::None) return e.button == PointerButton::Right;
      drag_ = Drag::None;
      frozen_ = false;
      if (gestureOpen_ && v_.ctx_.store().inGesture()) (void)v_.ctx_.store().commitGesture();
      gestureOpen_ = false;
      markDirty();
      return true;
    default: return false;
  }
}

void SampleDisplay::dragTo(const PointerEvent& e) {
  if (!pressClip_ || !v_.snap_) return;
  const doc::AudioClip& c0 = *pressClip_;
  const doc::SampleRef* s = sample();
  if (!s) return;
  if (!gestureOpen_) {  // the first move opens the gesture: a click alone records nothing
    const char* label = drag_ == Drag::Marker      ? "Clip Start"
                        : drag_ == Drag::WarpMove  ? "Move Warp Marker"
                        : drag_ == Drag::WarpSlide ? "Slide Warp Marker"
                                                   : "Clip Loop";
    (void)v_.ctx_.store().beginGesture(label);
    gestureOpen_ = true;
  }
  const bool free = (e.mods & mod::kShift) != 0;
  const Ticks grid = kPpq / 4;  // 1/16 note of content
  auto snapTicks = [&](double t) {
    const auto v = static_cast<Ticks>(std::llround(t));
    return free ? v : (v + (v >= 0 ? grid / 2 : -grid / 2)) / grid * grid;
  };
  const doc::Snapshot snap = v_.ctx_.doc();
  const double r = rate();
  const double dContent = contentAtX(e.pos.x) - contentAtX(pressX_);  // warped: ticks the pointer moved
  if (drag_ == Drag::WarpMove || drag_ == Drag::WarpSlide) {
    if (marker_ < 0 || static_cast<size_t>(marker_) >= c0.markers.size()) return;
    const doc::WarpMarker m0 = c0.markers[static_cast<size_t>(marker_)];
    if (drag_ == Drag::WarpMove) {
      const Ticks t = snapTicks(static_cast<double>(m0.content) + dContent);
      if (v_.clip_ && static_cast<size_t>(marker_) < v_.clip_->markers.size() && v_.clip_->markers[static_cast<size_t>(marker_)].content == t) return;
      (void)v_.dispatch(doc::ops::moveWarpMarker(*snap, v_.ref_, static_cast<size_t>(marker_), t), "Move Warp Marker");
    } else {
      // The source frame under the pointer follows it: the marker reads earlier source as the pointer moves right.
      const double df = doc::warpFrameAt(c0, r, contentAtX(e.pos.x)) - doc::warpFrameAt(c0, r, contentAtX(pressX_));
      (void)v_.dispatch(doc::ops::slideWarpMarker(*snap, v_.ref_, static_cast<size_t>(marker_), m0.sourceFrame - df),
                        "Slide Warp Marker");
    }
    return;
  }
  if (drag_ == Drag::Marker) {
    doc::AudioClip n = *v_.clip_;
    if (c0.warp == doc::AudioWarp::Off) {
      const double df = frameAt(e.pos.x) - frameAt(pressX_);
      const double span = c0.seconds * s->rate * speedRatio();
      n.contentStartFrame = std::clamp(c0.contentStartFrame + df, 0.0, std::max(0.0, static_cast<double>(s->frames) - span));
      if (n.contentStartFrame == v_.clip_->contentStartFrame) return;
    } else {
      const auto fileTicks = static_cast<Ticks>(std::floor(doc::warpContentAt(c0, r, static_cast<double>(s->frames))));
      const auto firstTick = static_cast<Ticks>(std::ceil(doc::warpContentAt(c0, r, 0.0)));
      n.contentStart = std::clamp<Ticks>(snapTicks(static_cast<double>(c0.contentStart) + dContent), std::min<Ticks>(0, firstTick),
                                         std::max<Ticks>(firstTick, fileTicks - 1));
      if (n.contentStart == v_.clip_->contentStart) return;
    }
    doc::Batch b;
    b.label = "Clip Start";
    b.commands.push_back(doc::SetAudioClip{v_.ref_, std::make_shared<const doc::AudioClip>(std::move(n))});
    if (Status st = v_.ctx_.store().dispatch(doc::Command(std::move(b))); !st) v_.ctx_.setStatus("Clip Start: " + st.error().message);
    return;
  }
  const auto dt = static_cast<Ticks>(std::llround(dContent));
  doc::ClipLoop loop = c0.loop;
  const Ticks minLen = free ? 1 : grid;
  // L12: bounds ordered (a Shift-made loop can be shorter than minLen), and nothing past the file's end.
  const auto fileTicks = std::max<Ticks>(minLen, static_cast<Ticks>(std::floor(doc::warpContentAt(c0, r, static_cast<double>(s->frames)))));
  if (drag_ == Drag::LoopStart) {
    loop.start = std::clamp<Ticks>(snapTicks(static_cast<double>(c0.loop.start + dt)), 0, std::max<Ticks>(0, loop.end - minLen));
  } else if (drag_ == Drag::LoopEnd) {
    loop.end = std::clamp<Ticks>(snapTicks(static_cast<double>(c0.loop.end + dt)), loop.start + minLen,
                                 std::max(fileTicks, loop.start + minLen));
  } else {
    const Ticks len = c0.loop.end - c0.loop.start;
    loop.start = std::clamp<Ticks>(snapTicks(static_cast<double>(c0.loop.start + dt)), 0, std::max<Ticks>(0, fileTicks - len));
    loop.end = loop.start + len;
  }
  if (loop == v_.clip_->loop) return;
  (void)v_.dispatch(doc::ops::setAudioClipLoop(*snap, v_.ref_, loop), "Clip Loop");
}

Menu* SampleDisplay::openMenu(Vec2 at) {
  if (!v_.clip_ || !sample()) return nullptr;
  const int32_t m = markerNear(at.x);
  const Ticks here = m >= 0 ? v_.clip_->markers[static_cast<size_t>(m)].content
                            : static_cast<Ticks>(std::llround(warped() ? contentAtX(at.x) : 0.0));
  std::vector<MenuItem> items;
  MenuItem from = MenuItem::item("Warp From Here", [this, here] { (void)v_.warpFromHere(here); });
  from.enabled = warped();
  items.push_back(std::move(from));
  double a = 0, b = 0;
  {  // the region "Warp as Loop" uses, in seconds of source
    const doc::AudioClip& c = *v_.clip_;
    if (warped()) {
      const bool looped = c.loop.enabled && c.loop.end > c.loop.start;
      a = frameOf(static_cast<double>(looped ? c.loop.start : c.contentStart));
      b = frameOf(static_cast<double>(looped ? c.loop.end : c.contentStart + c.length));
    } else {
      a = c.contentStartFrame;
      b = a + c.seconds * rate() * speedRatio();
    }
  }
  const int32_t bars = loopBarsFor((b - a) / rate(), v_.detectedBpm(), v_.beatsPerBarAtClip());
  items.push_back(MenuItem::item("Warp as " + std::to_string(bars) + "-Bar Loop", [this] { (void)v_.warpAsLoop(); }));
  if (m >= 0) {
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item("Delete Warp Marker", [this, m] { (void)v_.removeWarpMarker(static_cast<size_t>(m)); }, "Delete"));
  }
  return openContextMenu(*this, at, std::move(items));
}

}  // namespace audio_clip

// ---- the transpose knob ----------------------------------------------------------------------------------------------

namespace audio_clip {

// AC.look: the transpose "wheel" (see the header): a ClipKnob whose value is the resolved cents; every change goes
// through `pending`, the exact doc::AudioTranspose the view's onChange stores.
class TransposeKnob final : public ClipKnob {
public:
  TransposeKnob(AudioClipView& v, ValueSpec spec) : ClipKnob("transpose", "Transpose", std::move(spec)), v_(v) {}
  doc::AudioTranspose pending;

  bool onPointer(const PointerEvent& e) override {
    switch (e.type) {
      case PointerType::Down:
        if (e.button != PointerButton::Left) return false;
        if (e.clickCount >= 2) {  // the type-in (Delete returns to 0)
          dragEnd();
          beginTypeIn();
          return true;
        }
        if (typingIn()) commitTypeIn();
        dragStart(e.pos);
        return true;
      case PointerType::Move: {
        if (!dragging()) return false;
        const double s = tree() ? tree()->scale() : 1.0;
        const double d = static_cast<double>(last_.y - e.pos.y);
        last_ = e.pos;
        const bool fine = (e.mods & mod::kShift) != 0;
        raw_ = std::clamp(raw_ + d * (spec_.max - spec_.min) / (spec_.dragPixels * s) * (fine ? spec_.fineFactor : 1.0), spec_.min, spec_.max);
        if (fine) {  // free cents, to a tenth
          doc::AudioTranspose t;
          t.unit = doc::TransposeUnit::Cents;
          t.cents = std::round(raw_ * 10.0) / 10.0;
          apply(t);
        } else {  // the nearest step of the clip's tuning
          apply(v_.nearestStep(raw_));
        }
        return true;
      }
      case PointerType::Up:
        if (e.button != PointerButton::Left || !dragging()) return false;
        dragEnd();
        return true;
      case PointerType::Wheel: return wheel(e);
      default: return false;
    }
  }

  bool onKey(const KeyEvent& e) override {
    if (typingIn()) {
      if (e.down && (e.key == key::kReturn || e.key == key::kTab)) {
        commitTypeIn();
        return e.key == key::kReturn;  // Tab: focus traversal continues
      }
      return typeInKey(e);  // Esc cancels; the editing keys
    }
    const Mods chord = e.mods & mod::kChordMask;
    if (e.down && (e.key == key::kUp || e.key == key::kDown) && (chord & ~mod::kShift) == 0) {
      if (!enabled()) return false;
      finishWheel();  // a key step is its own gesture
      if (const std::optional<doc::AudioTranspose> t = v_.steppedTranspose(e.key == key::kUp ? 1 : -1, (chord & mod::kShift) != 0)) {
        apply(*t);
        endGesture();
      }
      return true;
    }
    if (e.down && (e.key == key::kDelete || e.key == key::kBackspace) && chord == mod::kNone) {
      pending = doc::AudioTranspose{};
      resetToDefault();
      return true;
    }
    return ClipKnob::onKey(e);  // a digit opens the type-in; Esc cancels a drag
  }

  void onFocusChanged(bool focused) override {
    if (!focused && typingIn()) commitTypeIn();  // focus loss commits
    markDirty();
  }

  void beginTypeIn() {
    if (!enabled()) return;
    finishWheel();
    typing_ = true;
    typeIn_.setText(v_.clip_ ? formatTranspose(v_.clip_->transpose) : std::string());
    typeIn_.selectAll();
    setAcceptsTextInput(true);
    if (tree() && !focused()) tree()->setFocus(this);
    markDirty();
  }

private:
  void apply(const doc::AudioTranspose& t) {
    pending = t;
    userSet(v_.transposeCents(t));
  }
  // The typed text through parseTranspose (the exact unit: "5/4" stays a ratio); one undo step; unparsable: the status
  // line says what parses, nothing changes.
  void commitTypeIn() {
    const std::string text = typeInBuffer().text();
    endTypeIn(false);
    (void)v_.setTranspose(text);
  }
  // The wheel: one step per notch (Shift: an octave), a run of notches one gesture (as ValueControl's wheel).
  bool wheel(const PointerEvent& e) {
    if (!wheelAdjusts() || dragging() || !enabled()) return false;
    if ((e.mods & (mod::kCtrl | mod::kAlt | mod::kSuper)) != 0 || e.wheel.y == 0.0f) return false;
    if (!wheelTimer_) wheelRaw_ = 0.0;  // a new run: the notch accumulator (trackpads send fractions)
    wheelRaw_ += static_cast<double>(e.wheel.y);
    const auto n = static_cast<int>(std::trunc(wheelRaw_));
    if (n != 0) {
      wheelRaw_ -= n;
      if (const std::optional<doc::AudioTranspose> t = v_.steppedTranspose(n > 0 ? 1 : -1, (e.mods & mod::kShift) != 0, std::abs(n)))
        apply(*t);
    }
    if (wheelTimer_) cancelTimer(wheelTimer_);
    wheelTimer_ = requestTimer(kWheelGestureSeconds);  // ends the run (and its gesture, if one began)
    return true;
  }

  AudioClipView& v_;
};

}  // namespace audio_clip

// ---- the view ---------------------------------------------------------------------------------------------------

bool AudioClipView::dragging() const noexcept { return display_ != nullptr && display_->dragging(); }

bool AudioClipView::onKey(const KeyEvent& e) {
  if (e.down && e.key == key::kEscape && display_ != nullptr && display_->cancel()) {
    if (tree() && tree()->captured()) tree()->releaseCapture();
    return true;
  }
  return false;
}

namespace {
constexpr Ticks kGrans[] = {0, kPpq, kPpq / 2, kPpq / 4, kPpq / 8};  // Preserve: Transients, 1/4, 1/8, 1/16, 1/32
constexpr float kKnobH = 84.0f;      // logical px: label, dial, readout
constexpr float kClipKnobH = 96.0f;  // the Clip card's (the transpose dial the largest)
}  // namespace

AudioClipView::AudioClipView(ViewContext& ctx) : Widget("audio-clip-view"), ctx_(ctx) {
  using audio_clip::Chip;
  using audio_clip::ClipKnob;
  using audio_clip::FillBox;
  using audio_clip::Section;
  setLayout(LayoutKind::Custom);
  setClipChildren(true);
  box_ = &add<Widget>("audio-box");
  box_->setLayout(LayoutKind::Column);
  box_->setAlign(Align::Start, Align::Start);
  box_->setPadding({static_cast<int16_t>(kBoxPadX), 0, static_cast<int16_t>(kBoxPadX), static_cast<int16_t>(kBoxPadBottom)});
  name_ = &box_->add<Label>("name", "", FontRoleId::BodyStrong, TextTone::Primary);
  name_->setHeight(Length::fixed(20));
  // An empty label: the space of the title strip, which the view paints in the clip colour with the name.
  file_ = &box_->add<Label>("file", "", FontRoleId::Small, TextTone::Secondary);
  file_->setHeight(Length::fixed(17));
  format_ = &box_->add<Label>("format", "", FontRoleId::Small, TextTone::Secondary);
  format_->setHeight(Length::fixed(17));
  // AC.look: three cards in the device panels' look (M5 review UI 2's Clip / Sample groups): Clip (pitch, level,
  // fades), Warp (the mode, the source tempo, the loop) and the mode's settings.
  // AC.fix: they flow into columns side by side when the panel is too short for the stack (arrange widens the box).
  flow_ = &box_->add<audio_clip::CardFlow>("props");
  flow_->setHeight(Length::flex(1));
  audio_clip::CardFlow& props = *flow_;
  auto knob = [](Widget& parent, const char* name, const char* label, ValueSpec spec, float width, float height = kKnobH) -> ClipKnob& {
    ClipKnob& k = parent.add<ClipKnob>(name, label, std::move(spec));
    k.setWidth(Length::fixed(width));
    k.setHeight(Length::fixed(height));
    return k;
  };
  auto chip = [](Widget& parent, const char* name, const char* text) -> Chip& {
    Chip& c = parent.add<Chip>(name, text);
    c.setHeight(Length::fixed(20));
    return c;
  };
  auto row = [](Widget& parent, const char* name) -> Widget& {
    Widget& w = parent.add<Widget>(name);
    w.setLayout(LayoutKind::Row);
    w.setGap(3);
    w.setHeight(Length::fixed(20));
    return w;
  };

  // ---- Clip: Transpose (the big knob), Detune, Gain; Reverse and the fades beside them.
  Section& clipCard = props.add<Section>("clip", "Clip");
  props.setCardWidth(clipCard, kBoxWidth - 2 * kBoxPadX);
  clipCard.setLayout(LayoutKind::Row);
  clipCard.setGap(4);
  clipCard.setHeight(Length::fixed(audio_clip::Section::kTitleH + kClipKnobH + 7));
  ValueSpec ts;
  ts.min = -doc::kMaxTransposeCents;
  ts.max = doc::kMaxTransposeCents;
  ts.def = 0.0;
  ts.dragPixels = 768.0;  // ~8 px per 100 c
  ts.fineFactor = 0.1;
  ts.decimals = 1;
  ts.unit = "\xC2\xA2";
  transpose_ = &clipCard.add<audio_clip::TransposeKnob>(*this, ts);
  transpose_->setWidth(Length::fixed(100));
  transpose_->setHeight(Length::fixed(kClipKnobH));
  transpose_->setInfo("Transpose", "Drag in steps of the clip's tuning (Shift: free cents). Up / Down: one step; Shift+Up / "
                                   "Down: an octave (12 semitones, whatever the tuning). Type 7 for 7 steps, 386.3c for "
                                   "cents, +7st for semitones, 5/4 for an exact ratio (double-click or start typing). Every "
                                   "warp mode but Re-Pitch (which follows the tempo).");
  bindGesture(*transpose_, "Transpose", [this](double) { (void)setTranspose(transpose_->pending); });
  ValueSpec ds;
  ds.min = -doc::kMaxDetuneCents;
  ds.max = doc::kMaxDetuneCents;
  ds.def = 0.0;
  ds.dragPixels = 300.0;
  ds.keyStep = 1.0;
  ds.decimals = 0;
  ds.unit = "\xC2\xA2";
  detune_ = &knob(clipCard, "detune", "Detune", ds, 54, kClipKnobH);
  detune_->formatter = [](double v) {
    char b[32];
    if (std::round(v) == 0.0) std::snprintf(b, sizeof b, "0 \xC2\xA2");
    else std::snprintf(b, sizeof b, "%+.0f \xC2\xA2", v);
    return std::string(b);
  };
  detune_->setSpec(ds);
  detune_->setInfo("Detune", "Fine pitch in cents (±50). Every warp mode but Re-Pitch.");
  bindGesture(*detune_, "Clip Detune", [this](double v) { (void)setDetune(static_cast<float>(v)); });
  ValueSpec gs;
  gs.min = doc::kMinClipGainDb;
  gs.max = doc::kMaxClipGainDb;
  gs.def = 0.0;
  gs.dragPixels = 400.0;
  gs.keyStep = 0.5;
  gs.decimals = 1;
  gs.unit = "dB";
  gain_ = &knob(clipCard, "gain", "Gain", gs, 54, kClipKnobH);
  gain_->formatter = [](double v) {
    if (v <= doc::kMinClipGainDb) return std::string("-inf dB");
    char b[32];
    std::snprintf(b, sizeof b, "%.1f dB", v);
    return std::string(b);
  };
  gain_->setSpec(gs);
  gain_->setInfo("Clip Gain", "The clip's gain (−inf … +24 dB).");
  bindGesture(*gain_, "Clip Gain", [this](double v) { (void)setGain(static_cast<float>(v)); });
  {
    Widget& side = clipCard.add<Widget>("side");
    side.setLayout(LayoutKind::Column);
    side.setGap(4);
    side.setWidth(Length::flex(1));
    side.setPadding({4, static_cast<int16_t>((kClipKnobH - 3 * 20 - 2 * 4) / 2), 0, 0});
    reverse_ = &chip(side, "reverse", "Reverse");
    reverse_->setInfo("Reverse", "Plays the clip backwards (the file is not changed).");
    reverse_->onPress = [this] {
      if (clip_) (void)setReverse(!clip_->reverse);
    };
    ValueSpec fs;
    fs.min = 0.0;
    fs.max = 60000.0;  // refresh: the clip's length (a fade cannot be longer), so the fill is the fade's share of it
    fs.def = 0.0;
    fs.dragPixels = 200.0;
    fs.keyStep = 1.0;
    fs.decimals = 0;
    fs.unit = "ms";
    fadeIn_ = &side.add<FillBox>("fade-in", "Fade In", fs, true);
    fadeIn_->setHeight(Length::fixed(20));
    fadeIn_->setInfo("Fade In", "The fade at the clip start in milliseconds (or drag its handle in the arrangement); the fill "
                                "is its share of the clip.");
    bindGesture(*fadeIn_, "Clip Fade", [this](double v) { (void)setFadesMs(v, fadeOut_->value()); });
    fadeOut_ = &side.add<FillBox>("fade-out", "Fade Out", fs, true);
    fadeOut_->setHeight(Length::fixed(20));
    fadeOut_->setInfo("Fade Out", "The fade at the clip end in milliseconds; the fill is its share of the clip.");
    bindGesture(*fadeOut_, "Clip Fade", [this](double v) { (void)setFadesMs(fadeIn_->value(), v); });
  }

  // ---- Warp: the seven modes (a 4 + 3 grid, Complex Pro two cells wide), the source tempo, the loop.
  Section& warpCard = props.add<Section>("warp", "Warp");
  props.setCardWidth(warpCard, kWarpCardW);
  warpCard.setLayout(LayoutKind::Column);
  warpCard.setGap(4);
  warpCard.setHeight(Length::fixed(audio_clip::Section::kTitleH + 3 * 20 + 2 * 4 + 7));
  {
    static constexpr const char* kChipNames[] = {"off", "repitch", "beats", "tones", "texture", "complex", "pro"};
    Widget& m1 = row(warpCard, "modes-1");
    Widget& m2 = row(warpCard, "modes-2");
    for (size_t i = 0; i < std::size(kChipNames); ++i) {
      Chip& c = chip(i < 4 ? m1 : m2, kChipNames[i], kWarpModeNames[i]);
      c.setWidth(Length::flex(i == 6 ? 2.0f : 1.0f));
      c.setInfo("Warp", "Off: the file's own speed, its duration in seconds. Re-Pitch: follows the song tempo like a tape "
                        "(speed and pitch together). Beats: slices at transients or the grid, for drums. Tones: grains "
                        "that keep a single pitched voice (monophonic) exactly in tune; chords drift, use Complex. Texture: "
                        "grains with randomness, for pads and noise. Complex: a phase vocoder for whole mixes. Complex "
                        "Pro: Complex keeping the formants (voices).");
      c.onPress = [this, i] { (void)setWarp(static_cast<doc::AudioWarp>(i)); };
      warp_[i] = &c;
    }
    // Row 2 has 4 cells' worth (1 + 1 + 2): the grid's columns line up with row 1.
    Widget& tempo = row(warpCard, "tempo");
    ValueSpec bs;
    bs.min = 20.0;
    bs.max = 999.0;
    bs.def = 120.0;
    bs.dragPixels = 1000.0;
    bs.keyStep = 1.0;
    bs.decimals = 2;
    bpm_ = &tempo.add<FillBox>("bpm", "BPM", bs, false);
    bpm_->setWidth(Length::flex(2));
    bpm_->setInfo("Source Tempo", "The tempo the file was recorded at (its tempo tag or name, else its detected tempo, else its "
                                  "length). Warp markers override it between them.");
    bindGesture(*bpm_, "Clip Tempo", [this](double v) { (void)setSourceBpm(v); });
    halve_ = &chip(tempo, "halve", ":2");
    halve_->setWidth(Length::flex(0.5f));
    halve_->setInfo("Halve Tempo", "The source tempo ÷ 2 (the clip plays twice as many of its beats per song beat).");
    halve_->onPress = [this] { (void)scaleSourceBpm(0.5); };
    double_ = &chip(tempo, "double", "\xC3\x97" "2");
    double_->setWidth(Length::flex(0.5f));
    double_->setInfo("Double Tempo", "The source tempo × 2.");
    double_->onPress = [this] { (void)scaleSourceBpm(2.0); };
    loop_ = &chip(tempo, "loop", "Loop");
    loop_->setWidth(Length::flex(1));
    loop_->setInfo("Loop", "Warped clips: repeats the part under the loop brace.");
    loop_->onPress = [this] {
      if (clip_) (void)setLoopEnabled(!clip_->loop.enabled);
    };
  }

  // ---- The mode's settings (T6.8), titled with the mode.
  mode_ = &props.add<Section>("mode", "");
  mode_->setLayout(LayoutKind::Row);
  mode_->setGap(6);
  mode_->setHeight(Length::fixed(audio_clip::Section::kTitleH + kKnobH + 7));
  auto paramGesture = [this](ValueControl& b, std::string label, auto set) {
    bindGesture(b, std::move(label), [this, set](double v) {
      if (!clip_) return;
      doc::WarpParams p = clip_->warpParams;
      set(p, v);
      (void)setWarpParams(p);
    });
  };
  {
    beatsCol_ = &mode_->add<Widget>("beats");
    beatsCol_->setLayout(LayoutKind::Column);
    beatsCol_->setWidth(Length::flex(1));
    beatsCol_->setGap(2);
    auto caption = [](Widget& parent, const char* name, const char* text) {
      parent.add<audio_clip::Caption>(name, text).setHeight(Length::fixed(15));
    };
    caption(*beatsCol_, "preserve-label", "Preserve");
    Widget& pr = row(*beatsCol_, "preserve");
    static constexpr const char* kPreserve[][2] = {{"transients", "Trans"}, {"1-4", "1/4"}, {"1-8", "1/8"}, {"1-16", "1/16"}, {"1-32", "1/32"}};
    for (size_t i = 0; i < 5; ++i) {
      Chip& c = chip(pr, kPreserve[i][0], kPreserve[i][1]);
      c.setWidth(Length::flex(i == 0 ? 1.3f : 1.0f));
      c.setInfo("Preserve", "Beats: where the sample is cut into slices — at its transients (Trans), or on a grid of its "
                            "beats.");
      c.onPress = [this, i] {
        if (!clip_) return;
        doc::WarpParams p = clip_->warpParams;
        p.beatsGranulation = kGrans[i];
        (void)setWarpParams(p);
      };
      preserve_[i] = &c;
    }
    caption(*beatsCol_, "loop-label", "Transient Loop");
    Widget& lr = row(*beatsCol_, "beats-loop");
    static constexpr const char* kLoops[][2] = {{"off", "Off"}, {"forward", "\xE2\x86\x92"}, {"back-fwd", "\xE2\x86\x94"}};
    for (size_t i = 0; i < 3; ++i) {
      Chip& c = chip(lr, kLoops[i][0], kLoops[i][1]);
      c.setWidth(Length::flex(1));
      c.setInfo("Transient Loop Mode", "Beats, slowed down: what a slice does when it ends before the next one — stop (Off), "
                                       "loop forward (→), or loop back and forth (↔).");
      c.onPress = [this, i] {
        if (!clip_) return;
        doc::WarpParams p = clip_->warpParams;
        p.beatsLoop = static_cast<doc::BeatsLoop>(i);
        (void)setWarpParams(p);
      };
      beatsLoop_[i] = &c;
    }
  }
  ValueSpec pct;
  pct.min = 0.0;
  pct.max = 100.0;
  pct.def = 100.0;
  pct.dragPixels = 300.0;
  pct.keyStep = 1.0;
  pct.decimals = 0;
  pct.unit = "%";
  beatsEnv_ = &knob(*mode_, "beats-env", "Envelope", pct, 64);
  beatsEnv_->setInfo("Transient Envelope", "Beats: how long each slice holds before it fades (100: the whole gap).");
  paramGesture(*beatsEnv_, "Warp Settings", [](doc::WarpParams& p, double v) { p.beatsEnvelope = static_cast<float>(v); });
  ValueSpec gr;
  gr.min = doc::kMinGrainMs;
  gr.max = doc::kMaxGrainMs;
  gr.def = 30.0;
  gr.dragPixels = 400.0;
  gr.keyStep = 1.0;
  gr.decimals = 0;
  gr.unit = "ms";
  grain_ = &knob(*mode_, "grain", "Grain Size", gr, 72);
  grain_->setInfo("Grain Size", "Tones / Texture: the length of the grains (short: tighter timing; long: smoother pitch).");
  paramGesture(*grain_, "Warp Settings", [](doc::WarpParams& p, double v) { p.grainSizeMs = static_cast<float>(v); });
  ValueSpec fx = pct;
  fx.def = 0.0;
  flux_ = &knob(*mode_, "flux", "Flux", fx, 72);
  flux_->setInfo("Flux", "Texture: randomness of the grains' positions.");
  paramGesture(*flux_, "Warp Settings", [](doc::WarpParams& p, double v) { p.flux = static_cast<float>(v); });
  formants_ = &knob(*mode_, "formants", "Formants", pct, 72);
  formants_->setInfo("Formants", "Complex Pro: how much the formants (a voice's character) stay put when transposed.");
  paramGesture(*formants_, "Warp Settings", [](doc::WarpParams& p, double v) { p.formants = static_cast<float>(v); });
  ValueSpec eo;
  eo.min = doc::kMinWarpEnvelope;
  eo.max = doc::kMaxWarpEnvelope;
  eo.def = 128.0;
  eo.dragPixels = 400.0;
  eo.keyStep = 1.0;
  eo.decimals = 0;
  eo.unit = "bins";  // M6 review L19: a count, not Beats' "Envelope" percentage
  envOrder_ = &knob(*mode_, "env-order", "Envelope", eo, 72);
  envOrder_->setInfo("Envelope", "Complex Pro: the detail of the spectral envelope the formants follow, in frequency bins "
                                 "(low: smooth).");
  paramGesture(*envOrder_, "Warp Settings", [](doc::WarpParams& p, double v) { p.envelope = static_cast<float>(v); });

  display_ = &add<audio_clip::SampleDisplay>(*this);

  docSub_ = ctx_.subscribe([this](const doc::StoreEvent&) { refresh(); });
  viewSub_ = ctx_.observe([this](uint32_t bits) {
    if (bits & (view_change::kDetailClip | view_change::kSamples)) refresh();
    if (bits & (view_change::kWaveforms | view_change::kSamples | view_change::kSampleAnalysis)) display_->markDirty();
  });
  refresh();
}

AudioClipView::~AudioClipView() = default;

void AudioClipView::bindGesture(ValueControl& c, std::string label, std::function<void(double)> apply) {
  c.onGestureBegin = [this, label] { (void)ctx_.store().beginGesture(label); };
  c.onChange = std::move(apply);
  c.onGestureEnd = [this] {
    if (ctx_.store().inGesture()) (void)ctx_.store().commitGesture();
  };
  c.onGestureCancel = [this] {
    if (ctx_.store().inGesture()) (void)ctx_.store().cancelGesture();
    refresh();
  };
}

void AudioClipView::refresh() {
  const doc::Snapshot cur = ctx_.doc();
  const doc::ClipRef want = ctx_.detailClip();
  doc::AudioClipPtr c;
  if (want.clip.valid())
    if (const doc::Track* t = doc::findTrackPtr(*cur, want.track)) {
      const int32_t i = doc::findClipIndex(*t, want.clip);
      if (i >= 0) c = t->clips[static_cast<size_t>(i)].audio;
    }
  const bool changed = c != clip_ || cur != snap_ || !(want.clip == ref_.clip && want.track == ref_.track);
  ref_ = want;
  clip_ = c;
  snap_ = cur;
  setVisible(clip_ != nullptr);
  if (!clip_) return;
  if (!changed) return;
  const doc::AudioClip& a = *clip_;
  const doc::SampleRef* s = doc::findSample(*cur, a.sample);
  const std::string path = ctx_.samplePath(a.sample);
  if (path != probedPath_) {  // headers only (≤ a few reads), once per file: bits, tempo tag
    probedPath_ = path;
    bits_ = 0;
    fileBpm_ = 0.0;
    container_.clear();
    if (!path.empty())
      if (Result<media::SampleInfo> info = media::probe(utf8Path(path))) {
        bits_ = info->bitsPerSample;
        fileBpm_ = info->bpm;
        container_ = std::string(media::containerName(info->container));
      }
  }
  if (s) {
    const std::u8string f = utf8Path(s->path).filename().u8string();
    const std::string fileName(reinterpret_cast<const char*>(f.data()), f.size());
    file_->setText(path.empty() ? fileName + " (missing)" : fileName);
    char fmt[128];
    const double secs = static_cast<double>(s->frames) / std::max(1.0, s->rate);
    if (bits_ > 0)
      std::snprintf(fmt, sizeof fmt, "%.1f kHz \xC2\xB7 %u-bit \xC2\xB7 %s \xC2\xB7 %.2f s", s->rate / 1000.0, bits_,
                    s->channels == 1 ? "mono" : s->channels == 2 ? "stereo" : "multi", secs);
    else
      std::snprintf(fmt, sizeof fmt, "%.1f kHz \xC2\xB7 %s \xC2\xB7 %.2f s", s->rate / 1000.0,
                    s->channels == 1 ? "mono" : s->channels == 2 ? "stereo" : "multi", secs);
    format_->setText(fmt);
  }
  // AC.look: the clip's tuning (the transpose knob's steps; a Steps transpose resolves in it).
  tuning_.reset();
  if (const std::optional<doc::TrackPath> tp = doc::findTrack(*cur, ref_.track))
    if (const doc::TuningDef* def = doc::audioClipTuning(*cur, *tp, a))
      if (Result<tuning::Tuning> t = doc::buildTuning(*def)) tuning_ = std::move(*t);
  const double cents = transposeCents(a.transpose);
  if (!transpose_->inGesture()) transpose_->setValue(cents);
  transpose_->setReadout(transposeReadout(a.transpose, cents));
  if (!gain_->inGesture()) gain_->setValue(a.gainDb);
  if (!detune_->inGesture()) detune_->setValue(a.detuneCents);
  reverse_->setOn(a.reverse);
  for (size_t i = 0; i < std::size(warp_); ++i) warp_[i]->setOn(static_cast<size_t>(a.warp) == i);
  if (!bpm_->inGesture()) bpm_->setValue(a.sourceBpm);
  loop_->setOn(a.loop.enabled);
  const bool warped = doc::isWarped(a.warp);
  const bool transposes = doc::warpTransposes(a.warp);
  transpose_->setEnabled(transposes);  // Re-Pitch ignores transpose and detune (Q145 item 2): greyed, as in Live
  detune_->setEnabled(transposes);
  bpm_->setEnabled(warped);
  halve_->setEnabled(warped);
  double_->setEnabled(warped);
  loop_->setEnabled(warped);
  // T6.8: the mode's card, with its controls; hidden for the modes without settings (Off, Re-Pitch, Complex).
  const doc::WarpParams& wp = a.warpParams;
  const bool beats = a.warp == doc::AudioWarp::Beats, tones = a.warp == doc::AudioWarp::Tones,
             texture = a.warp == doc::AudioWarp::Texture, pro = a.warp == doc::AudioWarp::ComplexPro;
  mode_->setVisible(beats || tones || texture || pro);
  mode_->setTitle(beats || tones || texture || pro ? kWarpModeNames[static_cast<size_t>(a.warp)] : "");
  beatsCol_->setVisible(beats);
  beatsEnv_->setVisible(beats);
  grain_->setVisible(tones || texture);
  flux_->setVisible(texture);
  formants_->setVisible(pro);
  envOrder_->setVisible(pro);
  // AC.fix: the card's width in a column of its own (its controls' widths + the card's padding); the box's width
  // depends on it when the cards flow into columns.
  if (const float w = beats ? 330.0f : tones ? 112.0f : texture || pro ? 176.0f : 0.0f; w != modeW_) {
    modeW_ = w;
    flow_->setCardWidth(*mode_, w);
    invalidateArrange();
  }
  {
    int32_t gi = 3;  // an unknown granulation reads as 1/16
    for (int32_t i = 0; i < 5; ++i)
      if (kGrans[i] == wp.beatsGranulation) gi = i;
    for (int32_t i = 0; i < 5; ++i) preserve_[i]->setOn(i == gi);
  }
  for (size_t i = 0; i < 3; ++i) beatsLoop_[i]->setOn(static_cast<size_t>(wp.beatsLoop) == i);
  if (!beatsEnv_->inGesture()) beatsEnv_->setValue(wp.beatsEnvelope);
  if (!grain_->inGesture()) grain_->setValue(wp.grainSizeMs);
  if (!flux_->inGesture()) flux_->setValue(wp.flux);
  if (!formants_->inGesture()) formants_->setValue(wp.formants);
  if (!envOrder_->inGesture()) envOrder_->setValue(wp.envelope);
  const TempoMap& tm = cur->timeline->tempoMap;
  const double t0 = tm.ticksToSeconds(a.start), t1 = tm.ticksToSeconds(a.start + a.length);
  // The fades' boxes range over the clip's length (what a fade can be), so their fill is the fade's share of the clip.
  const double clipMs = std::max(1.0, std::floor((t1 - t0) * 1000.0));
  for (audio_clip::FillBox* b : {fadeIn_, fadeOut_})
    if (!b->inGesture() && b->spec().max != clipMs) {
      ValueSpec fs = b->spec();
      fs.max = clipMs;
      b->setSpec(fs);
    }
  const Ticks fi = std::clamp<Ticks>(a.fadeIn.length, 0, a.length), fo = std::clamp<Ticks>(a.fadeOut.length, 0, a.length - fi);
  if (!fadeIn_->inGesture()) fadeIn_->setValue(std::round((tm.ticksToSeconds(a.start + fi) - t0) * 1000.0));
  if (!fadeOut_->inGesture()) fadeOut_->setValue(std::round((t1 - tm.ticksToSeconds(a.start + a.length - fo)) * 1000.0));
  box_->markDirty();
  display_->markDirty();
}

double AudioClipView::transposeCents(const doc::AudioTranspose& t) const {
  if (t.unit == doc::TransposeUnit::Steps) {
    if (tuning_) return doc::transposeCents(t, &*tuning_);
    return 100.0 * static_cast<double>(t.steps);  // no tuning resolves: semitones
  }
  return doc::transposeCents(t, nullptr);
}

namespace {
doc::AudioTranspose stepsOf(int64_t n) {
  doc::AudioTranspose t;
  t.unit = doc::TransposeUnit::Steps;
  t.steps = static_cast<int32_t>(std::clamp<int64_t>(n, -doc::kMaxTransposeSteps, doc::kMaxTransposeSteps));
  return t;
}
}  // namespace

// The clip's tuning's degree n in cents above degree 0, its degrees per period and the period in cents (no tuning:
// 12-EDO semitones).
static double stepCentsOf(const std::optional<tuning::Tuning>& tun, int64_t n) {
  if (!tun) return 100.0 * static_cast<double>(n);
  return doc::transposeCents(stepsOf(n), &*tun);
}

doc::AudioTranspose AudioClipView::nearestStep(double cents) const {
  const int64_t dpe = tuning_ ? std::max<int64_t>(1, doc::degreesPerEquave(*tuning_)) : 12;
  const double period = stepCentsOf(tuning_, dpe);
  const double mean = period > 0.0 ? period / static_cast<double>(dpe) : 100.0;
  const auto n0 = static_cast<int64_t>(std::llround(cents / mean));
  int64_t best = n0;
  double bd = 1e300;
  const int64_t reach = std::min<int64_t>(64, dpe / 2 + 2);  // an uneven scale's degree sits within half a period
  for (int64_t n = n0 - reach; n <= n0 + reach; ++n) {
    const double d = std::abs(stepCentsOf(tuning_, n) - cents);
    if (d < bd - 1e-9) {
      bd = d;
      best = n;
    }
  }
  return stepsOf(best);
}

std::optional<doc::AudioTranspose> AudioClipView::steppedTranspose(int dir, bool octave, int count) const {
  if (!clip_ || dir == 0 || count <= 0) return std::nullopt;
  const doc::AudioTranspose cur = clip_->transpose;
  const int64_t dpe = tuning_ ? std::max<int64_t>(1, doc::degreesPerEquave(*tuning_)) : 12;
  const int64_t sign = dir > 0 ? 1 : -1;
  doc::AudioTranspose t = cur;
  if (octave) {
    // AC.fix: an octave (2/1, 12 semitones) whatever the tuning, the unit kept where it can be: Steps ± the degrees
    // per equave when the equave is the octave, else cents; a Ratio × or ÷ 2 (cents if that overflows).
    constexpr double kOctave = 1200.0;
    const double octaves = static_cast<double>(sign * count);
    switch (cur.unit) {
      case doc::TransposeUnit::Steps:
        if (std::abs(stepCentsOf(tuning_, dpe) - kOctave) < 1e-6) {
          t = stepsOf(static_cast<int64_t>(cur.steps) + sign * count * dpe);
        } else {
          t = doc::AudioTranspose{};
          t.cents = transposeCents(cur) + octaves * kOctave;
        }
        break;
      case doc::TransposeUnit::Cents: t.cents = cur.cents + octaves * kOctave; break;
      case doc::TransposeUnit::Ratio: {
        std::optional<tuning::Rational> r = cur.ratio;
        for (int i = 0; r && i < count; ++i) r = sign > 0 ? tuning::mul(*r, tuning::Rational(2)) : tuning::div(*r, tuning::Rational(2));
        if (r) {
          t.ratio = *r;
        } else {
          t = doc::AudioTranspose{};
          t.cents = transposeCents(cur) + octaves * kOctave;
        }
        break;
      }
    }
  } else if (cur.unit == doc::TransposeUnit::Steps) {
    t = stepsOf(static_cast<int64_t>(cur.steps) + sign * count);
  } else {
    // The next degree strictly above (below) the current pitch, then count − 1 more.
    const double c = transposeCents(cur);
    int64_t n = nearestStep(c).steps;
    for (int guard = 0; guard < 4096 && (sign > 0 ? stepCentsOf(tuning_, n) <= c + 1e-6 : stepCentsOf(tuning_, n) >= c - 1e-6); ++guard)
      n += sign;
    for (int guard = 0; guard < 4096 && (sign > 0 ? stepCentsOf(tuning_, n - 1) > c + 1e-6 : stepCentsOf(tuning_, n + 1) < c - 1e-6); ++guard)
      n -= sign;
    t = stepsOf(n + sign * (count - 1));
  }
  if (std::abs(transposeCents(t)) > doc::kMaxTransposeCents + 1e-6) return std::nullopt;
  return t;
}

bool AudioClipView::dispatch(const doc::ops::EditResult& e, std::string_view what) {
  if (!e) {
    ctx_.setStatus(std::string(what) + ": " + e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (Status s = ctx_.store().dispatch(e->command); !s) {
    ctx_.setStatus(std::string(what) + ": " + s.error().message);
    return false;
  }
  return true;
}

bool AudioClipView::setGain(float db) {
  if (!clip_ || db == clip_->gainDb) return false;
  return dispatch(doc::ops::setAudioClipGain(*ctx_.doc(), std::span<const doc::ClipRef>(&ref_, 1), db), "Clip Gain");
}

bool AudioClipView::setTranspose(std::string_view text) {
  if (!clip_) return false;
  const std::optional<doc::AudioTranspose> t = parseTranspose(text);
  if (!t) {
    ctx_.setStatus("Transpose: type 7 (steps), 386.3c (cents), +7st (semitones) or 5/4 (a ratio)");
    return false;
  }
  return setTranspose(*t);
}

bool AudioClipView::setTranspose(const doc::AudioTranspose& t) {
  if (!clip_ || t == clip_->transpose) return false;
  return dispatch(doc::ops::setAudioClipTranspose(*ctx_.doc(), std::span<const doc::ClipRef>(&ref_, 1), t), "Transpose");
}

bool AudioClipView::setDetune(float cents) {
  if (!clip_ || cents == clip_->detuneCents) return false;
  return dispatch(doc::ops::setAudioClipDetune(*ctx_.doc(), std::span<const doc::ClipRef>(&ref_, 1), cents), "Detune");
}

bool AudioClipView::setReverse(bool on) {
  if (!clip_ || on == clip_->reverse) return false;
  return dispatch(doc::ops::setAudioClipsReverse(*ctx_.doc(), std::span<const doc::ClipRef>(&ref_, 1), on), "Reverse");
}

double AudioClipView::detectedBpm() {
  if (!clip_) return 0.0;
  const SampleAnalysisResult* an = ctx_.sampleAnalysis().get(clip_->sample);
  if (!an || !an->tempo || an->tempo->estimate.confidence < audio_clip::kConfident) return 0.0;
  return an->tempo->estimate.bpm;
}

bool AudioClipView::setWarp(doc::AudioWarp warp) {
  if (!clip_ || warp == clip_->warp) return false;
  double bpm = clip_->sourceBpm;
  if (!doc::isWarped(clip_->warp) && doc::isWarped(warp)) {  // from Off: seed the source tempo
    const doc::SampleRef* s = doc::findSample(*ctx_.doc(), clip_->sample);
    const double detected = fileBpm_ >= kMinBpm && fileBpm_ <= kMaxBpm ? 0.0 : detectedBpm();
    bpm = sourceBpmFor(detected > 0.0 ? detected : fileBpm_, s ? static_cast<double>(s->frames) / std::max(1.0, s->rate) : 0.0);
  }
  return dispatch(doc::ops::setAudioClipWarp(*ctx_.doc(), std::span<const doc::ClipRef>(&ref_, 1), warp, bpm), "Warp");
}

bool AudioClipView::setSourceBpm(double bpm) {
  if (!clip_ || !doc::isWarped(clip_->warp) || bpm == clip_->sourceBpm) return false;
  return dispatch(doc::ops::setAudioClipWarp(*ctx_.doc(), std::span<const doc::ClipRef>(&ref_, 1), clip_->warp, bpm),
                  "Clip Tempo");
}

bool AudioClipView::scaleSourceBpm(double factor) {
  if (!clip_ || !doc::isWarped(clip_->warp)) return false;
  const double bpm = clip_->sourceBpm * factor;
  if (bpm < kMinBpm || bpm > kMaxBpm) return false;
  return setSourceBpm(bpm);
}

bool AudioClipView::setLoopEnabled(bool on) {
  if (!clip_ || !doc::isWarped(clip_->warp) || on == clip_->loop.enabled) return false;
  doc::ClipLoop l = clip_->loop;
  l.enabled = on;
  if (l.end <= l.start) {  // a fresh loop: the clip's own span of content
    l.start = clip_->contentStart;
    l.end = clip_->contentStart + clip_->length;
  }
  return dispatch(doc::ops::setAudioClipLoop(*ctx_.doc(), ref_, l), "Loop");
}

bool AudioClipView::setFadesMs(double inMs, double outMs) {
  if (!clip_) return false;
  const doc::AudioClip& a = *clip_;
  const TempoMap& tm = ctx_.doc()->timeline->tempoMap;
  const double t0 = tm.ticksToSeconds(a.start), t1 = tm.ticksToSeconds(a.start + a.length);
  doc::AudioFade fi = a.fadeIn, fo = a.fadeOut;
  fi.length = std::clamp<Ticks>(static_cast<Ticks>(std::llround(tm.secondsToTicks(t0 + std::max(0.0, inMs) / 1000.0))) - a.start, 0, a.length);
  fo.length = std::clamp<Ticks>(a.start + a.length - static_cast<Ticks>(std::llround(tm.secondsToTicks(t1 - std::max(0.0, outMs) / 1000.0))),
                                0, a.length);
  if (fi == a.fadeIn && fo == a.fadeOut) return false;
  return dispatch(doc::ops::setAudioClipFades(*ctx_.doc(), ref_, fi, fo), "Clip Fade");
}

bool AudioClipView::setWarpParams(const doc::WarpParams& p) {
  if (!clip_ || p == clip_->warpParams) return false;
  return dispatch(doc::ops::setAudioClipWarpParams(*ctx_.doc(), std::span<const doc::ClipRef>(&ref_, 1), p), "Warp Settings");
}

bool AudioClipView::warpFromHere(Ticks content) {
  if (!clip_ || !doc::isWarped(clip_->warp)) return false;
  const double bpm = detectedBpm();
  return dispatch(doc::ops::warpFromHere(*ctx_.doc(), ref_, content, bpm > 0.0 ? bpm : clip_->sourceBpm), "Warp From Here");
}

double AudioClipView::beatsPerBarAtClip() const {  // M11: quarter notes per bar of the signature at the clip's start
  if (!clip_ || !ctx_.doc()) return 4.0;
  return static_cast<double>(ticksPerBar(ctx_.doc()->timeline->sigMap.signatureAt(clip_->start))) /
         static_cast<double>(kPpq);
}

bool AudioClipView::warpAsLoop() {
  if (!clip_) return false;
  const doc::SampleRef* s = doc::findSample(*ctx_.doc(), clip_->sample);
  if (!s || !(s->rate > 0.0)) return false;
  const doc::AudioClip& c = *clip_;
  double a = 0.0, b = 0.0;
  if (doc::isWarped(c.warp)) {
    const bool looped = c.loop.enabled && c.loop.end > c.loop.start;
    a = doc::warpFrameAt(c, s->rate, static_cast<double>(looped ? c.loop.start : c.contentStart));
    b = doc::warpFrameAt(c, s->rate, static_cast<double>(looped ? c.loop.end : c.contentStart + c.length));
  } else {
    a = c.contentStartFrame;
    b = a + c.seconds * s->rate * display_->speedRatio();
  }
  const int32_t bars = loopBarsFor((b - a) / s->rate, detectedBpm(), beatsPerBarAtClip());
  return dispatch(doc::ops::warpAsLoop(*ctx_.doc(), ref_, bars, ctx_.viewPrefs().defaultWarpMode), "Warp as Loop");
}

bool AudioClipView::removeWarpMarker(size_t index) {
  if (!clip_ || index >= clip_->markers.size()) return false;
  return dispatch(doc::ops::removeWarpMarker(*ctx_.doc(), ref_, index), "Delete Warp Marker");
}

IRect AudioClipView::sampleRect() const noexcept { return display_->wave(); }
IRect AudioClipView::rulerRect() const noexcept { return display_->ruler(); }
IRect AudioClipView::warpStripRect() const noexcept { return display_->strip(); }
float AudioClipView::xOfFrame(double frame) const noexcept { return display_->xOf(frame); }
float AudioClipView::xOfContent(double content) const noexcept { return display_->xOfContent(content); }
float AudioClipView::warpMarkerX(size_t i) const noexcept { return display_->warpMarkerX(i); }
int32_t AudioClipView::selectedWarpMarker() const noexcept { return display_->selected(); }
double AudioClipView::startMarkerFrame() const noexcept { return display_->markerFrame(); }
Menu* AudioClipView::openSampleMenu(Vec2 at) { return display_->openMenu(at); }

void AudioClipView::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  ctx.dl.addRectFilled(rect().toRect(), th->surface.background);
  const IRect b = box_->rect();
  ctx.dl.addRectFilled(b.toRect(), th->surface.panel);
  if (!clip_) return;
  // The title strip in the clip colour with its name (as the piano roll's clip box).
  const Rgba colour = hex(clip_->colour.rgb);
  const IRect strip{b.x, b.y, b.w, ctx.scale.px(20.0f)};
  ctx.dl.addRectFilled(strip.toRect(), colour);
  if (const ThemeFonts* f = ctx.res.uiFonts)
    paint::text(ctx, f->bodyStrong, strip, clip_->name, clipTextFor(*th, colour), TextAlign::Left, ctx.scale.px(6.0f), ctx.scale.px(6.0f));
}

void AudioClipView::arrange(LayoutPass& pass) {
  const IRect r = rect();
  // One column of cards: kBoxWidth (at most half the view). AC.fix: when the view is too short for the stack, the
  // cards flow into columns and the box widens to hold them, leaving the waveform at least kMinDisplayW.
  const int32_t single = std::min(pass.px(kBoxWidth), r.w / 2);
  int cols = 1;
  const float want = flow_->plan(static_cast<float>(r.h) / pass.scale() - kHeaderH - kBoxPadBottom,
                                 kBoxWidth - 2 * kBoxPadX, &cols) + 2 * kBoxPadX;
  const int32_t gap = 1;
  const int32_t boxW = cols <= 1 ? single : std::min(pass.px(want), std::max(single, r.w - gap - pass.px(kMinDisplayW)));
  pass.place(*box_, {r.x, r.y, boxW, r.h});
  pass.place(*display_, {r.x + boxW + gap, r.y, std::max(0, r.w - boxW - gap), r.h});
}

AudioClipView& installAudioClipView(Shell& shell) {
  return shell.slot(ShellSlot::ClipDetail).add<AudioClipView>(shell.context());
}

AudioClipView* findAudioClipView(Shell& shell) {
  return dynamic_cast<AudioClipView*>(shell.slot(ShellSlot::ClipDetail).find("audio-clip-view"));
}

}  // namespace pq::views
