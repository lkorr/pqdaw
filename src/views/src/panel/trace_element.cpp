// MX.3 (docs/plans/MX.md "MX.3"; pq/document/panel.h "Trace taps"): the TRACE SCOPE — a "scope" element bound to two or
// three Signals (doc::elementTraces) draws one (or all) of them from the ONE tap the lowering gives those signals
// (signal_tap(beat, s0, s1, s2): one ring, so the traces are the same samples). Any panel can place it; the Math
// module's face places three (A, B, Out) and they line up in time:
//
//   window   every trace scope of one tap SHARES one window (TraceWindow, by the feed): its newest frames of the three
//            signals and ONE frame (start, span) from dsp::ScopeTracker run on slot 0 (PQ Meter's pitch-aligned 52 ms
//            span, the Oscilloscope's defaults) — computed once per feed update, whichever scope polls first; the
//            panel repaints whole after a poll, so all of them draw the same samples.
//   style    `trace` first | second | third | all (which slot it draws; colours: slot 0 the waveform green, 1 salmon,
//            2 the accent blue); `scale` auto | gate (a 0 / 1 trace: 0 at the bottom rule, 1 at the top rule).
//   range    (BR.A) auto: −1 … 1 by default; a value beyond it widens the range SLOWLY to what the drawn signals spanned
//            over the last ~5 s (RangeTracker: the min / max of each frame shown into 20 buckets of 0.25 s, the range
//            eased toward theirs — out with τ 0.4 s, back toward ±1 with τ 1.2 s as old extremes age out). Per frame:
//            one pass over the shown samples (the trace walks them anyway) and 20 compares; no sort, no history of
//            samples.
//   costs    the feed is acquired on the first paint and let go when the scope stops being painted (a poll finds no
//            paint for 0.5 s, asks for one, none comes: the Oscilloscope face's rule), so a panel off screen or a hidden
//            element reads nothing; the tap then costs the audio thread one relaxed load per block. A paint draws the
//            static backdrop from tiles and one trace (≤ one point per device px column: the min / max envelope when
//            denser). UI thread only.

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "elements_internal.h"
#include "pq/dsp/scope.h"
#include "pq/ui/lines.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/panel/trace_scope.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

constexpr std::string_view kTraces[] = {"first", "second", "third", "all"};
constexpr std::string_view kScales[] = {"auto", "gate"};
constexpr float kInset = 3.0f;    // the plot inside the box (units)
constexpr double kEnvelopeSpp = 4.0;  // denser than this: the min / max envelope

// BR.A: the auto range — −1 … 1, widened to the extremes of the last kBuckets · kBucketSeconds, eased.
class RangeTracker {
public:
  static constexpr int kBuckets = 20;
  static constexpr double kBucketSeconds = 0.25;   // 20 · 0.25 s = the 5 s window
  static constexpr double kOutTau = 0.4, kInTau = 1.2;  // seconds: widening, relaxing

  // The extremes of one frame shown at time `now` (seconds).
  void add(float lo, float hi, double now) {
    advance(now);
    Bucket& b = buckets_[static_cast<size_t>(index_ % kBuckets)];
    b.lo = std::min(b.lo, lo);
    b.hi = std::max(b.hi, hi);
  }
  // Eases the range toward the window's extremes (at least −1 … 1); true: it moved visibly (a repaint is due).
  bool step(double now) {
    advance(now);
    float tlo = -1.0f, thi = 1.0f;
    for (const Bucket& b : buckets_) tlo = std::min(tlo, b.lo), thi = std::max(thi, b.hi);
    const double dt = last_ > 0.0 ? std::clamp(now - last_, 0.0, 0.5) : 0.0;
    last_ = now;
    const float lo0 = lo_, hi0 = hi_;
    auto ease = [&](float& v, float target, bool out) {
      const double a = 1.0 - std::exp(-dt / (out ? kOutTau : kInTau));
      v += static_cast<float>((target - v) * a);
      if (std::fabs(v - target) < 1e-3f * std::max(1.0f, std::fabs(target))) v = target;
    };
    ease(lo_, tlo, tlo < lo_);
    ease(hi_, thi, thi > hi_);
    return std::fabs(lo_ - lo0) + std::fabs(hi_ - hi0) > 1e-4f * (hi_ - lo_);
  }
  bool settled() const noexcept {
    float tlo = -1.0f, thi = 1.0f;
    for (const Bucket& b : buckets_) tlo = std::min(tlo, b.lo), thi = std::max(thi, b.hi);
    return lo_ == tlo && hi_ == thi;
  }
  float lo() const noexcept { return lo_; }
  float hi() const noexcept { return hi_; }

private:
  struct Bucket {
    float lo = 0.0f, hi = 0.0f;
  };
  void advance(double now) {
    const auto k = static_cast<int64_t>(std::floor(now / kBucketSeconds));
    if (index_ < 0 || k - index_ >= kBuckets) {
      buckets_.fill(Bucket{});
    } else {
      for (int64_t i = index_ + 1; i <= k; ++i) buckets_[static_cast<size_t>(i % kBuckets)] = Bucket{};
    }
    if (k > index_) index_ = k;
  }
  std::array<Bucket, kBuckets> buckets_{};
  int64_t index_ = -1;
  double last_ = 0.0;
  float lo_ = -1.0f, hi_ = 1.0f;
};

// The newest frames of one trace tap and the frame every trace scope of it shows.
class TraceWindow {
public:
  static constexpr uint32_t kMaxFrames = 1u << 15;  // the most the window reads (≥ the tracker's need at 48 kHz)

  // The window of `f` (one per feed: made at the first ask, gone with its last scope).
  static std::shared_ptr<TraceWindow> of(devices::SpectrumFeed& f) {
    static std::vector<std::weak_ptr<TraceWindow>> all;
    std::erase_if(all, [](const std::weak_ptr<TraceWindow>& w) { return w.expired(); });
    for (const std::weak_ptr<TraceWindow>& w : all)
      if (std::shared_ptr<TraceWindow> s = w.lock(); s->feed_ == &f && s->serial_ == f.serial()) return s;
    auto s = std::make_shared<TraceWindow>(f);
    all.push_back(s);
    return s;
  }
  explicit TraceWindow(devices::SpectrumFeed& f) : feed_(&f), serial_(f.serial()) {}
  ~TraceWindow() {
    if (users_ > 0) feed_->release(devices::kWantScope);
  }
  TraceWindow(const TraceWindow&) = delete;
  TraceWindow& operator=(const TraceWindow&) = delete;

  bool is(const devices::SpectrumFeed* f) const noexcept { return f == feed_ && f != nullptr && f->serial() == serial_; }
  void acquire() {
    if (users_++ == 0) feed_->acquire(devices::kWantScope);
  }
  void release() {
    if (users_ > 0 && --users_ == 0) {
      feed_->release(devices::kWantScope);
      end_ = 0;
    }
  }
  bool acquired() const noexcept { return users_ > 0; }
  bool live() const noexcept { return feed_->live(); }
  // Reads the newest frames when the feed moved and places the frame; true: a new frame (version() moved).
  bool update() {
    if (users_ == 0) return false;
    bool settled = true;
    const uint64_t end = feed_->scopeEnd(settled);
    if (end == end_ && version_ != 0) return false;
    end_ = end;
    const double sr = feed_->config().sampleRate;
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>({end, kMaxFrames, tracker_.need(sr)}));
    for (auto& c : ch_) c.resize(n);
    float* out[4] = {nullptr, ch_[0].data(), ch_[1].data(), ch_[2].data()};
    const uint32_t got = n > 0 ? feed_->scopeRead(end - n, n, out) : 0;
    (void)got;
    for (auto& c : ch_)  // (frames not held read NaN in a 4-channel tap's channel 1: no signal there)
      for (float& v : c)
        if (!std::isfinite(v)) v = 0.0f;
    frame_ = n >= 2 ? tracker_.frame(ch_[0], sr, 0.0f, false) : dsp::ScopeFrame{};
    if (frame_.start + frame_.span > n) frame_ = dsp::ScopeFrame{};
    ++version_;
    return true;
  }
  uint64_t version() const noexcept { return version_; }
  // Slot k's samples of the frame shown (empty: none).
  std::span<const float> shown(size_t k) const noexcept {
    if (frame_.span < 2 || k >= ch_.size()) return {};
    return std::span<const float>(ch_[k]).subspan(frame_.start, frame_.span);
  }

private:
  devices::SpectrumFeed* feed_;
  uint64_t serial_;
  int users_ = 0;
  uint64_t end_ = 0, version_ = 0;
  std::array<std::vector<float>, 3> ch_;
  dsp::ScopeTracker tracker_;
  dsp::ScopeFrame frame_;
};

class TraceScope final : public Element {
public:
  using Element::Element;
  ~TraceScope() override { letGo(); }

  // ---- static layers: the well, the rules (centre and ± half; a gate's 0 and 1) ----
  uint32_t staticLayerCount() const noexcept override { return 2; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.device.display : th.device.displayGrid;
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba white = ui::rgba(255, 255, 255);
    if (layer == 0) {
      p.roundedRect({0.0f, 0.0f, desc_.w, desc_.h}, 4.0f, white);
      return;
    }
    const Rect r = plot();
    if (gate()) {
      for (const float v : {0.0f, 1.0f}) p.line({r.x0, yOf(v)}, {r.x1, yOf(v)}, 0.6f, white);
      return;
    }
    // The quarter rules (±0.5 at the default range; BR.A: divisions of the plot as the range widens).
    for (const float f : {0.25f, 0.75f}) {
      const float y = r.y0 + f * (r.y1 - r.y0);
      p.line({r.x0, y}, {r.x1, y}, 0.5f, white);
    }
  }

  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    const Rect r = plot();
    if (env.target == nullptr || env.ghost) {
      if (!gate()) p.line({r.x0, yOf(0.0f)}, {r.x1, yOf(0.0f)}, 0.7f, th.device.displayGridMajor);
      return;
    }
    painted_ = clock();
    probe_ = 0.0;
    if (!sync(*env.target)) return;
    if (!held_) {
      window_->acquire();
      held_ = true;
      (void)window_->update();  // a scope that just appeared draws its first frame complete
    }
    if (!gate()) {  // BR.A: the range follows what the shown traces spanned
      if (window_->version() != ranged_) {
        ranged_ = window_->version();
        float lo = 0.0f, hi = 0.0f;
        const int which = traceIndex();
        for (int k = 0; k < 3; ++k)
          if (which == 3 || which == k)
            for (const float v : window_->shown(static_cast<size_t>(k))) lo = std::min(lo, v), hi = std::max(hi, v);
        range_.add(lo, hi, painted_);
      }
      (void)range_.step(painted_);
      p.line({r.x0, yOf(0.0f)}, {r.x1, yOf(0.0f)}, 0.7f, th.device.displayGridMajor);
    }
    const ui::FaceTransform& ft = p.transform();
    const Rect d{ft.origin.x + r.x0 * ft.scale, ft.origin.y + r.y0 * ft.scale, ft.origin.x + r.x1 * ft.scale,
                 ft.origin.y + r.y1 * ft.scale};
    ui::DrawList& dl = p.drawList();
    dl.pushClip({d.x0, d.y0 - 2.0f * ft.scale, d.x1, d.y1 + 2.0f * ft.scale});
    const int which = traceIndex();
    for (int k = 0; k < 3; ++k)
      if (which == 3 || which == k) paintTrace(dl, d, ft.scale, window_->shown(static_cast<size_t>(k)), kColours[k]);
    dl.popClip();
    seen_ = window_->version();
  }
  std::pair<float, float> range() const noexcept { return {range_.lo(), range_.hi()}; }  // (tests)
  bool ghosted(const PanelTarget& /*t*/) const override { return false; }
  void paintGhost(ui::FacePainter&, const ElementEnv&) override {}

  // ---- the feed ----
  bool pollVoices(PanelTarget& t) override {
    if (!held_) return false;
    if (!sync(t)) return true;  // (its feed went: one repaint)
    const double now = clock();
    if (now - painted_ > 0.5) {  // not painted lately: hidden — after one unanswered repaint, let go
      if (probe_ == 0.0) {
        probe_ = now;
        return true;
      }
      if (painted_ < probe_ && now - probe_ > 0.05) {
        letGo();
        probe_ = 0.0;
        return false;
      }
    }
    const bool fresh = window_->update();
    return fresh || window_->version() != seen_ || !range_.settled();  // (BR.A: the range still easing)
  }
  bool animating() const noexcept override { return held_ && window_ && (window_->live() || !range_.settled()); }
  // (The host polls while a reader exists: one as soon as the target has the tap, so the first paint is followed.)
  VoiceTapReader* voiceReader(PanelTarget& t) const override {
    reader_.window = held_ ? window_.get() : nullptr;
    return held_ || t.traceFeed(desc_) != nullptr ? &reader_ : nullptr;
  }
  uint64_t liveKey(const PanelTarget& /*t*/) const override { return kAnyChange; }
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    static constexpr const char* kNames[] = {"A", "B", "Out", "Traces"};
    title = std::string("Scope: ") + kNames[traceIndex()];
    body = "The signals on one time base, aligned to the first.";
    (void)t;
  }

private:
  static constexpr Rgba kColours[3] = {ui::signal_theme::kWaveform, ui::signal_theme::kWaveformR, ui::signal_theme::kAccent};
  struct Reader final : VoiceTapReader {
    TraceWindow* window = nullptr;
    uint32_t read(std::span<FaceVoice>) override { return 0; }
    uint64_t serial() const noexcept override { return 0; }
    bool live() const noexcept override { return window != nullptr && window->live(); }
  };

  static double clock() { return MetaFace::clockNow(); }
  bool gate() const noexcept { return txt("scale", "auto") == "gate"; }
  int traceIndex() const noexcept {
    const std::string_view s = txt("trace", "first");
    for (int k = 0; k < 4; ++k)
      if (s == kTraces[k]) return k;
    return 0;
  }
  Rect plot() const noexcept { return {kInset, kInset, desc_.w - kInset, desc_.h - kInset}; }
  // The y of value v (gate: 0 … 1 bottom to top; auto: the range lo … hi bottom to top, −1 … 1 by default), units.
  float yOf(float v) const noexcept {
    const Rect r = plot();
    if (gate()) return r.y1 - std::clamp(v, -0.1f, 1.1f) * (r.y1 - r.y0);
    const float lo = range_.lo(), hi = range_.hi();
    const float u = (v - lo) / std::max(hi - lo, 1e-6f);  // 0 … 1
    return r.y1 - std::clamp(u, -0.025f, 1.025f) * (r.y1 - r.y0);
  }
  // Follows the target's feed for this element (a new one: a new window). True: there is one.
  bool sync(PanelTarget& t) {
    devices::SpectrumFeed* f = t.traceFeed(desc_);
    if (window_ && window_->is(f)) return true;
    letGo();
    window_.reset();
    if (f == nullptr) return false;
    window_ = TraceWindow::of(*f);
    return true;
  }
  void letGo() {
    if (held_ && window_) window_->release();
    held_ = false;
  }
  void paintTrace(ui::DrawList& dl, const Rect& d, float scale, std::span<const float> x, Rgba col) {
    if (x.size() < 2) return;
    const float w = d.x1 - d.x0;
    const Rect r = plot();
    auto y = [&](float v) { return d.y0 + (yOf(v) - r.y0) * scale; };
    // WF.2: no glow passes (glow was 0.35: the two passes came out at alpha 4 / 255 and 1 / 255 — four of an envelope's
    // seven ribbons, drawn every frame for nothing visible).
    const float lw = 1.1f * scale, glow = 0.0f;
    const double spp = static_cast<double>(x.size()) / std::max(1.0f, w);
    if (spp > kEnvelopeSpp) {
      const int cols = std::max(2, static_cast<int>(w));
      up_.clear();
      dn_.clear();
      for (int c = 0; c < cols; ++c) {
        const size_t a = static_cast<size_t>(static_cast<double>(c) * spp);
        const size_t b = std::min(x.size(), static_cast<size_t>(static_cast<double>(c + 1) * spp));
        if (a >= b) continue;
        float lo = x[a], hi = x[a];
        for (size_t i = a + 1; i < b; ++i) {
          lo = std::min(lo, x[i]);
          hi = std::max(hi, x[i]);
        }
        float yHi = y(hi), yLo = y(lo);
        if (yLo - yHi < 0.5f) {
          const float m = 0.5f * (yHi + yLo);
          yHi = m - 0.25f;
          yLo = m + 0.25f;
        }
        const float px = d.x0 + static_cast<float>(c) + 0.5f;
        up_.push_back({px, yHi});
        dn_.push_back({px, yLo});
      }
      if (up_.size() >= 2) ui::scopeEnvelope(dl, up_.data(), dn_.data(), static_cast<uint32_t>(up_.size()), col, lw, glow);
      return;
    }
    pts_.clear();
    const float step = w / static_cast<float>(x.size() - 1);
    for (size_t i = 0; i < x.size(); ++i) pts_.push_back({d.x0 + static_cast<float>(i) * step, y(x[i])});
    ui::scopeTrace(dl, pts_, col, lw, glow);
  }

  std::shared_ptr<TraceWindow> window_;
  bool held_ = false;
  double painted_ = 0.0, probe_ = 0.0;
  uint64_t seen_ = 0, ranged_ = 0;
  RangeTracker range_;  // BR.A: the auto range
  mutable Reader reader_;
  std::vector<Vec2> up_, dn_, pts_;
};

}  // namespace

std::span<const StyleSpec> traceScopeStyle() noexcept {
  static constexpr StyleSpec kStyle[] = {
      {"trace", StyleSpec::Type::Choice, 0, "first", kTraces},
      {"scale", StyleSpec::Type::Choice, 0, "auto", kScales},
  };
  return kStyle;
}

std::unique_ptr<Element> makeTraceScope(const doc::PanelElement& desc) { return std::make_unique<TraceScope>(desc); }

namespace test {
std::optional<std::pair<float, float>> traceRange(const Element& e) {
  const auto* t = dynamic_cast<const TraceScope*>(&e);
  if (t == nullptr) return std::nullopt;
  return t->range();
}
}  // namespace test

}  // namespace pq::views::panel
