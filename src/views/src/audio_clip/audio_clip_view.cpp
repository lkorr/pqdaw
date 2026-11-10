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
#include "pq/document/audio_clip.h"
#include "pq/document/commands.h"
#include "pq/media/probe.h"
#include "pq/ui/theme.h"
#include "pq/ui/overlay.h"
#include "pq/ui/waveform.h"
#include "pq/views/sample_analysis.h"
#include "pq/views/waveforms.h"

namespace pq::views {

using namespace pq::ui;

namespace {

constexpr float kBoxWidth = 352.0f;   // logical px: two property columns (M5 review UI 2)
constexpr float kRulerH = 16.0f;
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
    case doc::TransposeUnit::Cents: std::snprintf(buf, sizeof buf, "%gc", t.cents); break;
    case doc::TransposeUnit::Ratio:
      std::snprintf(buf, sizeof buf, "%lld/%lld", static_cast<long long>(t.ratio.num()), static_cast<long long>(t.ratio.den()));
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

int32_t loopBarsFor(double seconds, double bpm) {
  if (!(seconds > 0.0)) return 1;
  const double want = bpm > 0.0 ? bpm : 120.0;
  int32_t best = 1;
  double bestD = 1e300;
  for (int32_t bars = 1; bars <= 64; bars *= 2) {
    const double t = bars * 4.0 * 60.0 / seconds;
    if (t < kMinBpm || t > kMaxBpm) continue;
    const double d = std::abs(std::log2(t / want));
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
constexpr float kWarpStripH = 14.0f;     // logical px: the warp markers' strip (warped clips)
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
  IRect strip() const noexcept {
    const IRect r = rect(), u = ruler();
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

  AudioClipView& v_;
  Drag drag_ = Drag::None;
  float pressX_ = 0.0f;
  doc::AudioClipPtr pressClip_;
  int32_t marker_ = -1;    // the warp marker being dragged
  int32_t selected_ = -1;  // the selected warp marker (Delete)
  bool frozen_ = false;
  double axis0_ = 0.0, axis1_ = 1.0;
  bool gestureOpen_ = false;
};

void SampleDisplay::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  DrawList& dl = ctx.dl;
  const IRect r = rect();
  dl.addRectFilled(r.toRect(), th->surface.sunken);
  const IRect u = ruler(), st = strip(), w = wave();
  dl.addRectFilled(u.toRect(), th->surface.panel);
  if (st.h > 0) dl.addRectFilled(st.toRect(), lerp(th->surface.panel, th->surface.sunken, 0.5f));
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
  // The region the clip plays, lit in the clip colour; the rest dimmed.
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
  if (xb > xa) dl.addRectFilled({xa, wy0, xb, wy1}, lerp(colour, rgba(30, 30, 30), 0.30f));
  // Warped: the content's beat grid (bars brighter), where beats are at least 4 px apart.
  if (isWarped) {
    double a0 = 0, a1 = 1;
    axis(a0, a1);
    const double pxPerTick = static_cast<double>(w.w) / (a1 - a0);
    Ticks step = kPpq;
    while (static_cast<double>(step) * pxPerTick < 4.0 && step < (Ticks{1} << 40)) step *= 2;
    for (Ticks t = static_cast<Ticks>(std::ceil(a0 / static_cast<double>(step))) * step; static_cast<double>(t) <= a1; t += step) {
      const float x = std::round(xOfContent(static_cast<double>(t)));
      const bool bar = t % kTicksPerWhole == 0;
      dl.addRectFilled({x, wy0, x + 1.0f, wy1}, bar ? th->grid.bar : th->grid.beat);
    }
  }
  if (const media::WaveformOverview* ov = v_.ctx_.waveforms().overview(c.sample)) {
    WaveformLevel lv[kMaxOverviewLevels];
    WaveformData data = waveformData(*ov, nullptr, lv);
    if (waveformLevelFor(data, frames() / std::max(1, w.w)) < 0)
      if (const media::SampleWindow* sw = v_.ctx_.waveforms().window(c.sample, 0, static_cast<int64_t>(frames())))
        data = waveformData(*ov, sw, lv);
    WaveformDraw d;
    // M5 review UI 2: the sample at unity (the clip gain is a readout in the corner).
    d.gain = 1.0f;
    d.colour = lerp(colour, rgba(0, 0, 0), 0.72f);
    // One piece per segment of the warp law: [0, first marker), between markers, [last marker, end) — each linear in
    // content, so drawn stretched into its x range.
    double cuts[2 + 64];
    size_t n = 0;
    cuts[n++] = 0.0;
    if (isWarped) {
      const auto& m = c.markers;
      // More markers than pieces: every k-th (the pieces between stay close to linear at that zoom).
      const size_t k = std::max<size_t>(1, (m.size() + 63) / 64);
      for (size_t i = 0; i < m.size(); i += k)
        if (m[i].sourceFrame > cuts[n - 1] && m[i].sourceFrame < frames()) cuts[n++] = m[i].sourceFrame;
    }
    cuts[n++] = frames();
    for (size_t i = 0; i + 1 < n; ++i) {
      const float x0 = xOf(cuts[i]), x1 = xOf(cuts[i + 1]);
      if (!(x1 > x0) || x1 < wx0 || x0 > wx1) continue;
      d.frame0 = cuts[i];
      d.frame1 = cuts[i + 1];
      d.rect = {x0, wy0, x1, wy1};
      d.clipX0 = std::max(wx0, x0);
      d.clipX1 = std::min(wx1, x1);
      (void)drawWaveform(dl, data, d);
    }
  } else {
    paint::text(ctx, font, w, "Loading waveform\xE2\x80\xA6", th->text.secondary, TextAlign::Center, 8, 8);
  }
  // The sample's transients: faint lines (one per pixel column at most).
  if (const SampleAnalysisResult* an = v_.ctx_.sampleAnalysis().get(c.sample); an && an->transients) {
    const Rgba tc = withAlpha(th->text.primary, 46);
    float last = -1e9f;
    for (const double f : an->transients->onsets) {
      const float x = std::round(xOf(f));
      if (x < wx0 || x >= wx1 || x == last) continue;
      last = x;
      dl.addRectFilled({x, wy0, x + 1.0f, wy1}, tc);
      if (st.h > 0) dl.addRectFilled({x, static_cast<float>(st.bottom() - st.h / 3), x + 1.0f, static_cast<float>(st.bottom())}, tc);
    }
  }
  // Region edges.
  const Rgba edge = withAlpha(th->text.primary, 150);
  if (xb > xa) {
    dl.addRectFilled({xa, wy0, xa + 1.0f, wy1}, edge);
    dl.addRectFilled({xb - 1.0f, wy0, xb, wy1}, edge);
  }
  // Warped: the loop brace in the ruler.
  if (isWarped) {
    const float la = std::round(xOfContent(static_cast<double>(c.loop.start))), lb = std::round(xOfContent(static_cast<double>(c.loop.end)));
    const Rgba braceCol = c.loop.enabled ? th->accent.primary : withAlpha(th->text.disabled, 160);
    const float by0 = static_cast<float>(u.y + u.h / 4), by1 = static_cast<float>(u.bottom() - u.h / 4);
    if (lb > la) {
      dl.addRectFilled({la, by0, lb, by1}, withAlpha(braceCol, c.loop.enabled ? 200 : 110));
      dl.addRectFilled({la, by0, la + 2.0f, static_cast<float>(u.bottom())}, braceCol);
      dl.addRectFilled({lb - 2.0f, by0, lb, static_cast<float>(u.bottom())}, braceCol);
    }
  }
  // The clip gain as a readout (the waveform is drawn at unity).
  if (c.gainDb != 0.0f && font != kNoFont) {
    char g[32];
    if (c.gainDb <= doc::kMinClipGainDb) std::snprintf(g, sizeof g, "Gain -inf dB");
    else std::snprintf(g, sizeof g, "Gain %+.1f dB", static_cast<double>(c.gainDb));
    paint::text(ctx, font, {w.x, w.y + ctx.scale.px(4.0f), w.w, ctx.scale.px(16.0f)}, g, th->text.primary, TextAlign::Right,
                ctx.scale.px(8.0f), ctx.scale.px(8.0f));
  }
  // Warp markers: a yellow pin in the strip (Live's colour), a line over the waveform; the selected one outlined.
  if (isWarped && st.h > 0) {
    const Rgba pin = th->accent.activator;
    const float half = static_cast<float>(std::max(3, ctx.scale.px(4.5f)));
    const float sy0 = static_cast<float>(st.y + 1), sy1 = static_cast<float>(st.bottom());
    for (size_t i = 0; i < c.markers.size(); ++i) {
      const float x = std::round(warpMarkerX(i));
      if (x < wx0 - half || x > wx1 + half) continue;
      const bool sel = static_cast<int32_t>(i) == selected();
      dl.addRectFilled({x, wy0, x + 1.0f, wy1}, withAlpha(pin, sel ? 255 : 190));
      if (sel) dl.addTriangleFilled({x - half - 1.5f, sy0 - 1.0f}, {x + half + 2.5f, sy0 - 1.0f}, {x + 0.5f, sy1 + 0.5f}, th->text.primary);
      dl.addTriangleFilled({x - half, sy0}, {x + half + 1.0f, sy0}, {x + 0.5f, sy1}, pin);
    }
  }
  // The start marker (M5 review UI 2): a flag in the accent colour filling the ruler, and a 2-px line over the waveform.
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
  const int32_t bars = loopBarsFor((b - a) / rate(), v_.detectedBpm());
  items.push_back(MenuItem::item("Warp as " + std::to_string(bars) + "-Bar Loop", [this] { (void)v_.warpAsLoop(); }));
  if (m >= 0) {
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item("Delete Warp Marker", [this, m] { (void)v_.removeWarpMarker(static_cast<size_t>(m)); }, "Delete"));
  }
  return openContextMenu(*this, at, std::move(items));
}

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

AudioClipView::AudioClipView(ViewContext& ctx) : Widget("audio-clip-view"), ctx_(ctx) {
  setLayout(LayoutKind::Custom);
  setClipChildren(true);
  box_ = &add<Widget>("audio-box");
  box_->setLayout(LayoutKind::Column);
  box_->setAlign(Align::Start, Align::Start);
  box_->setPadding({8, 0, 8, 6});
  name_ = &box_->add<Label>("name", "", FontRoleId::BodyStrong, TextTone::Primary);
  name_->setHeight(Length::fixed(20));
  // An empty label: the space of the title strip, which the view paints in the clip colour with the name.
  file_ = &box_->add<Label>("file", "", FontRoleId::Small, TextTone::Secondary);
  file_->setHeight(Length::fixed(16));
  format_ = &box_->add<Label>("format", "", FontRoleId::Small, TextTone::Secondary);
  format_->setHeight(Length::fixed(18));
  // M5 review UI 2: the properties in two groups side by side, as Live's Sample and Clip boxes (one long form ran
  // down the panel): Clip (gain, transpose, detune, reverse, fades) and Sample (warp, tempo, loop, the mode's settings).
  Widget& props = box_->add<Widget>("props");
  props.setLayout(LayoutKind::Row);
  props.setGap(12);
  props.setHeight(Length::flex(1));
  props.setAlign(Align::Start, Align::Start);
  Widget* clipCol = &props.add<Widget>("clip");
  Widget* sampleCol = &props.add<Widget>("sample");
  for (Widget* col : {clipCol, sampleCol}) {
    col->setLayout(LayoutKind::Column);
    col->setWidth(Length::flex(1));
    col->setAlign(Align::Start, Align::Start);
  }
  Widget* rowParent = clipCol;
  auto row = [&rowParent](const char* name, const char* text) -> Widget& {
    Widget& w = rowParent->add<Widget>(name);
    w.setLayout(LayoutKind::Row);
    w.setHeight(Length::fixed(20));
    w.setPadding({0, 2, 0, 0});
    w.add<Label>("label", text, FontRoleId::Small, TextTone::Secondary).setWidth(Length::flex(1));
    return w;
  };
  auto box = [](Widget& r, const char* name, ValueSpec spec) -> DragBox& {
    DragBox& b = r.add<DragBox>(name, std::move(spec));
    b.setWidth(Length::fixed(84));
    return b;
  };
  ValueSpec gs;
  gs.min = doc::kMinClipGainDb;
  gs.max = doc::kMaxClipGainDb;
  gs.def = 0.0;
  gs.dragPixels = 400.0;
  gs.keyStep = 0.5;
  gs.decimals = 1;
  gs.unit = "dB";
  gain_ = &box(row("gain-row", "Gain"), "gain", gs);
  gain_->formatter = [](double v) {
    if (v <= doc::kMinClipGainDb) return std::string("-inf dB");
    char b[32];
    std::snprintf(b, sizeof b, "%.1f dB", v);
    return std::string(b);
  };
  gain_->setSpec(gs);
  gain_->setInfo("Clip Gain", "The clip's gain (−inf … +24 dB). The waveform follows it.");
  bindGesture(*gain_, "Clip Gain", [this](double v) { (void)setGain(static_cast<float>(v)); });

  Widget& tr = row("transpose-row", "Transpose");
  transpose_ = &tr.add<TextEdit>("transpose");
  transpose_->setWidth(Length::fixed(84));
  transpose_->setInfo("Transpose", "Type 7 for 7 steps of the clip's tuning, 700c for cents, +7st for semitones, 3/2 for a "
                                   "ratio. Every warp mode but Re-Pitch (which follows the tempo).");
  transpose_->onCommit = [this](const std::string& t) {
    if (!setTranspose(t) && clip_) transpose_->setText(formatTranspose(clip_->transpose));
  };
  ValueSpec ds;
  ds.min = -doc::kMaxDetuneCents;
  ds.max = doc::kMaxDetuneCents;
  ds.def = 0.0;
  ds.dragPixels = 300.0;
  ds.keyStep = 1.0;
  ds.decimals = 0;
  ds.unit = "\xC2\xA2";
  detune_ = &box(row("detune-row", "Detune"), "detune", ds);
  detune_->setInfo("Detune", "Fine pitch in cents (±50). Every warp mode but Re-Pitch.");
  bindGesture(*detune_, "Clip Detune", [this](double v) { (void)setDetune(static_cast<float>(v)); });

  reverse_ = &row("reverse-row", "Reverse").add<Toggle>("reverse", "Rev");
  reverse_->setWidth(Length::fixed(84));
  reverse_->setInfo("Reverse", "Plays the clip backwards (the file is not changed).");
  reverse_->onToggle = [this](bool on) { (void)setReverse(on); };

  rowParent = sampleCol;
  warp_ = &row("warp-row", "Warp").add<Dropdown>("warp", std::vector<std::string>(std::begin(kWarpModeNames), std::end(kWarpModeNames)), 0);
  warp_->setWidth(Length::fixed(96));  // "Complex Pro"
  warp_->setFocusable(false);
  warp_->setInfo("Warp", "Off: the file's own speed, its duration in seconds. Re-Pitch: follows the song tempo like a tape "
                         "(speed and pitch together). Beats: slices at transients or the grid, for drums. Tones: grains "
                         "that keep pitched sounds smooth. Texture: grains with randomness, for pads and noise. Complex: "
                         "a phase vocoder for whole mixes. Complex Pro: Complex keeping the formants (voices).");
  warp_->onChange = [this](int32_t i) {
    if (i >= 0 && i <= static_cast<int32_t>(doc::kLastAudioWarp)) (void)setWarp(static_cast<doc::AudioWarp>(i));
  };
  ValueSpec bs;
  bs.min = 20.0;
  bs.max = 999.0;
  bs.def = 120.0;
  bs.dragPixels = 1000.0;
  bs.keyStep = 1.0;
  bs.decimals = 2;
  bpm_ = &box(row("bpm-row", "BPM"), "bpm", bs);
  bpm_->setInfo("Source Tempo", "The tempo the file was recorded at (its tempo tag or name, else its detected tempo, else its "
                                "length). Warp markers override it between them.");
  bindGesture(*bpm_, "Clip Tempo", [this](double v) { (void)setSourceBpm(v); });
  {
    Widget& sr = row("bpm-scale-row", "");
    halve_ = &sr.add<Button>("halve", ":2");
    halve_->setWidth(Length::fixed(41));
    halve_->setInfo("Halve Tempo", "The source tempo ÷ 2 (the clip plays twice as many of its beats per song beat).");
    halve_->onClick = [this] { (void)scaleSourceBpm(0.5); };
    sr.add<Widget>("gap").setWidth(Length::fixed(2));
    double_ = &sr.add<Button>("double", "*2");
    double_->setWidth(Length::fixed(41));
    double_->setInfo("Double Tempo", "The source tempo × 2.");
    double_->onClick = [this] { (void)scaleSourceBpm(2.0); };
  }
  loop_ = &row("loop-row", "Loop").add<Toggle>("loop", "Loop");
  loop_->setWidth(Length::fixed(84));
  loop_->setInfo("Loop", "Warped clips: repeats the part under the loop brace.");
  loop_->onToggle = [this](bool on) { (void)setLoopEnabled(on); };

  // T6.8: each mode's settings (rows shown for their mode).
  auto paramGesture = [this](DragBox& b, std::string label, auto set) {
    bindGesture(b, std::move(label), [this, set](double v) {
      if (!clip_) return;
      doc::WarpParams p = clip_->warpParams;
      set(p, v);
      (void)setWarpParams(p);
    });
  };
  preserveRow_ = &row("preserve-row", "Preserve");
  preserve_ = &preserveRow_->add<Dropdown>("preserve", std::vector<std::string>{"Transients", "1/4", "1/8", "1/16", "1/32"}, 3);
  preserve_->setWidth(Length::fixed(84));
  preserve_->setFocusable(false);
  preserve_->setInfo("Preserve", "Beats: where the sample is cut into slices — at its transients, or on a grid of its beats.");
  preserve_->onChange = [this](int32_t i) {
    if (!clip_) return;
    constexpr Ticks kGrans[] = {0, kPpq, kPpq / 2, kPpq / 4, kPpq / 8};
    doc::WarpParams p = clip_->warpParams;
    p.beatsGranulation = kGrans[std::clamp(i, 0, 4)];
    (void)setWarpParams(p);
  };
  beatsLoopRow_ = &row("beats-loop-row", "Loop Mode");
  beatsLoop_ = &beatsLoopRow_->add<Dropdown>("beats-loop", std::vector<std::string>{"Off", "Forward", "Back-Fwd"}, 1);
  beatsLoop_->setWidth(Length::fixed(84));
  beatsLoop_->setFocusable(false);
  beatsLoop_->setInfo("Transient Loop Mode", "Beats, slowed down: what a slice does when it ends before the next one — "
                                             "stop (Off), loop forward, or loop back and forth.");
  beatsLoop_->onChange = [this](int32_t i) {
    if (!clip_) return;
    doc::WarpParams p = clip_->warpParams;
    p.beatsLoop = static_cast<doc::BeatsLoop>(std::clamp(i, 0, 2));
    (void)setWarpParams(p);
  };
  ValueSpec pct;
  pct.min = 0.0;
  pct.max = 100.0;
  pct.def = 100.0;
  pct.dragPixels = 300.0;
  pct.keyStep = 1.0;
  pct.decimals = 0;
  pct.unit = "%";
  beatsEnvRow_ = &row("beats-env-row", "Envelope");
  beatsEnv_ = &box(*beatsEnvRow_, "beats-env", pct);
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
  grainRow_ = &row("grain-row", "Grain Size");
  grain_ = &box(*grainRow_, "grain", gr);
  grain_->setInfo("Grain Size", "Tones / Texture: the length of the grains (short: tighter timing; long: smoother pitch).");
  paramGesture(*grain_, "Warp Settings", [](doc::WarpParams& p, double v) { p.grainSizeMs = static_cast<float>(v); });
  ValueSpec fx = pct;
  fx.def = 0.0;
  fluxRow_ = &row("flux-row", "Flux");
  flux_ = &box(*fluxRow_, "flux", fx);
  flux_->setInfo("Flux", "Texture: randomness of the grains' positions.");
  paramGesture(*flux_, "Warp Settings", [](doc::WarpParams& p, double v) { p.flux = static_cast<float>(v); });
  formantsRow_ = &row("formants-row", "Formants");
  formants_ = &box(*formantsRow_, "formants", pct);
  formants_->setInfo("Formants", "Complex Pro: how much the formants (a voice's character) stay put when transposed.");
  paramGesture(*formants_, "Warp Settings", [](doc::WarpParams& p, double v) { p.formants = static_cast<float>(v); });
  ValueSpec eo;
  eo.min = doc::kMinWarpEnvelope;
  eo.max = doc::kMaxWarpEnvelope;
  eo.def = 128.0;
  eo.dragPixels = 400.0;
  eo.keyStep = 1.0;
  eo.decimals = 0;
  envOrderRow_ = &row("env-order-row", "Envelope");
  envOrder_ = &box(*envOrderRow_, "env-order", eo);
  envOrder_->setInfo("Envelope", "Complex Pro: the detail of the spectral envelope the formants follow (low: smooth).");
  paramGesture(*envOrder_, "Warp Settings", [](doc::WarpParams& p, double v) { p.envelope = static_cast<float>(v); });

  // The Sample column's controls one width (the chooser fits "Complex Pro").
  for (Widget* w : std::initializer_list<Widget*>{bpm_, loop_, preserve_, beatsLoop_, beatsEnv_, grain_, flux_, formants_, envOrder_})
    w->setWidth(Length::fixed(96));
  halve_->setWidth(Length::fixed(47));
  double_->setWidth(Length::fixed(47));

  rowParent = clipCol;
  ValueSpec fs;
  fs.min = 0.0;
  fs.max = 60000.0;
  fs.def = 0.0;
  fs.dragPixels = 2000.0;
  fs.keyStep = 1.0;
  fs.decimals = 0;
  fs.unit = "ms";
  fadeIn_ = &box(row("fade-in-row", "Fade In"), "fade-in", fs);
  fadeIn_->setInfo("Fade In", "The fade at the clip start, in milliseconds (or drag its handle in the arrangement).");
  bindGesture(*fadeIn_, "Clip Fade", [this](double v) { (void)setFadesMs(v, fadeOut_->value()); });
  fadeOut_ = &box(row("fade-out-row", "Fade Out"), "fade-out", fs);
  fadeOut_->setInfo("Fade Out", "The fade at the clip end, in milliseconds.");
  bindGesture(*fadeOut_, "Clip Fade", [this](double v) { (void)setFadesMs(fadeIn_->value(), v); });

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
  if (!gain_->inGesture()) gain_->setValue(a.gainDb);
  if (!transpose_->focused()) transpose_->setText(formatTranspose(a.transpose));
  if (!detune_->inGesture()) detune_->setValue(a.detuneCents);
  reverse_->setOn(a.reverse);
  warp_->setIndex(static_cast<int32_t>(a.warp));
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
  // T6.8: the mode's rows.
  const doc::WarpParams& wp = a.warpParams;
  const bool beats = a.warp == doc::AudioWarp::Beats, tones = a.warp == doc::AudioWarp::Tones,
             texture = a.warp == doc::AudioWarp::Texture, pro = a.warp == doc::AudioWarp::ComplexPro;
  preserveRow_->setVisible(beats);
  beatsLoopRow_->setVisible(beats);
  beatsEnvRow_->setVisible(beats);
  grainRow_->setVisible(tones || texture);
  fluxRow_->setVisible(texture);
  formantsRow_->setVisible(pro);
  envOrderRow_->setVisible(pro);
  {
    constexpr Ticks kGrans[] = {0, kPpq, kPpq / 2, kPpq / 4, kPpq / 8};
    int32_t gi = -1;
    for (int32_t i = 0; i < 5; ++i)
      if (kGrans[i] == wp.beatsGranulation) gi = i;
    preserve_->setIndex(gi >= 0 ? gi : 3);
  }
  beatsLoop_->setIndex(static_cast<int32_t>(wp.beatsLoop));
  if (!beatsEnv_->inGesture()) beatsEnv_->setValue(wp.beatsEnvelope);
  if (!grain_->inGesture()) grain_->setValue(wp.grainSizeMs);
  if (!flux_->inGesture()) flux_->setValue(wp.flux);
  if (!formants_->inGesture()) formants_->setValue(wp.formants);
  if (!envOrder_->inGesture()) envOrder_->setValue(wp.envelope);
  const TempoMap& tm = cur->timeline->tempoMap;
  const double t0 = tm.ticksToSeconds(a.start), t1 = tm.ticksToSeconds(a.start + a.length);
  const Ticks fi = std::clamp<Ticks>(a.fadeIn.length, 0, a.length), fo = std::clamp<Ticks>(a.fadeOut.length, 0, a.length - fi);
  if (!fadeIn_->inGesture()) fadeIn_->setValue(std::round((tm.ticksToSeconds(a.start + fi) - t0) * 1000.0));
  if (!fadeOut_->inGesture()) fadeOut_->setValue(std::round((t1 - tm.ticksToSeconds(a.start + a.length - fo)) * 1000.0));
  box_->markDirty();
  display_->markDirty();
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
    ctx_.setStatus("Transpose: type 7 (steps), 700c (cents), +7st (semitones) or 3/2 (a ratio)");
    return false;
  }
  if (*t == clip_->transpose) return false;
  return dispatch(doc::ops::setAudioClipTranspose(*ctx_.doc(), std::span<const doc::ClipRef>(&ref_, 1), *t), "Transpose");
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
  const int32_t bars = loopBarsFor((b - a) / s->rate, detectedBpm());
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
  const int32_t boxW = std::min(pass.px(kBoxWidth), r.w / 2);
  const int32_t gap = 1;
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
