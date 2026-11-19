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
//            2 the accent blue); `scale` auto (bipolar about the centre line: ±1 full height, a larger peak fits the
//            box) | gate (a 0 / 1 trace: 0 at the bottom rule, 1 at the top rule).
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

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

constexpr std::string_view kTraces[] = {"first", "second", "third", "all"};
constexpr std::string_view kScales[] = {"auto", "gate"};
constexpr float kInset = 3.0f;    // the plot inside the box (units)
constexpr double kEnvelopeSpp = 4.0;  // denser than this: the min / max envelope

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
      for (const float v : {0.0f, 1.0f}) p.line({r.x0, yOf(v, 1.0f)}, {r.x1, yOf(v, 1.0f)}, 0.6f, white);
      return;
    }
    for (const float v : {-0.5f, 0.5f}) p.line({r.x0, yOf(v, 1.0f)}, {r.x1, yOf(v, 1.0f)}, 0.5f, white);
  }

  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    const Rect r = plot();
    if (!gate()) p.line({r.x0, yOf(0.0f, 1.0f)}, {r.x1, yOf(0.0f, 1.0f)}, 0.7f, th.device.displayGridMajor);
    if (env.target == nullptr || env.ghost) return;
    painted_ = clock();
    probe_ = 0.0;
    if (!sync(*env.target)) return;
    if (!held_) {
      window_->acquire();
      held_ = true;
      (void)window_->update();  // a scope that just appeared draws its first frame complete
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
    return fresh || window_->version() != seen_;
  }
  bool animating() const noexcept override { return held_ && window_ && window_->live(); }
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
  // The y of value v (gate: 0 … 1 bottom to top; auto: ±peak about the middle), units.
  float yOf(float v, float peak) const noexcept {
    const Rect r = plot();
    if (gate()) return r.y1 - std::clamp(v, -0.1f, 1.1f) * (r.y1 - r.y0);
    const float mid = 0.5f * (r.y0 + r.y1), half = 0.5f * (r.y1 - r.y0);
    return mid - std::clamp(v / peak, -1.05f, 1.05f) * half;
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
    float peak = 1.0f;
    if (!gate())
      for (const float v : x) peak = std::max(peak, std::fabs(v));
    const float w = d.x1 - d.x0;
    const Rect r = plot();
    auto y = [&](float v) { return d.y0 + (yOf(v, peak) - r.y0) * scale; };
    const float lw = 1.1f * scale, glow = 0.35f;
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
  uint64_t seen_ = 0;
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

}  // namespace pq::views::panel
