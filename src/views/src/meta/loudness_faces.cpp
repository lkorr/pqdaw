// The loudness faces (VZ.4; docs/plans/VZ.md "VZ.4"): the Level Meter, the LUFS Meter and the Loudness History — each
// module IS its display. Ported from PQ Meter (pq meter/src/modules/level_meter.cpp, lufs_meter.cpp, loudness_graph.cpp:
// their options, look and ballistics) into pqdaw's face frame: a dark body, the display well, chips in the top-right
// corner (SET: the settings sheet, option_sheet.h, ANALYSIS / DISPLAY with SAVE DEFAULT / RESET; CLEAR: the measurement
// starts again; FREEZE: the picture holds while the analysis runs on), the options as the instance's data.
//
//   Level Meter       PQ's portrait level meter: per channel an RMS body (green L / salmon R; amber above −18 dBFS and
//                     red above −6 when Zone colours is on), the bright peak cap, the hold tick, the latched clip lamp
//                     (its clip count when > 1; click the lamps to reset), dB rules every 6 dB labelled every 12 between
//                     the bars, the peak dB under each bar (red once clipped). 100 × 160.
//   LUFS Meter        PQ's tank (the portrait LUFS column) beside the numbers: the momentary fill tinted by its state
//                     against the target (blue under, green within ±1 LU, red over, blended in between), its saturated
//                     level line and glow, the 0 LUFS and target lines, the max short-term hold, the short-term history
//                     riding behind the fill, the level's readout riding the line, the delta at the top and the over-
//                     −1 dBTP dot; the short-term LUFS big with its delta, then M, I, LRA, TP, MAX M, MAX S and the L/R
//                     correlation with its bar. The bar rises fast and falls slowly, the headline eases (PQ advanceAnim).
//                     200 × 160.
//   Loudness History  PQ's loudness graph: momentary / short-term / integrated traces scrolling right to left against
//                     the target line and its tolerance band, the auto-reset's quiet line dotted (both drag: one undo
//                     step), the scale Manual (warped: −15 LUFS and above take 85 %), Auto (frames the traces) or Target
//                     (−10 … +8 LU around it), a grid of 1 / 2 / 5 / 10 LU and time ticks, the S / M / I and delta chips,
//                     the window, target and short-term range below, a hover inspector. 300 × 160.
//
// The analysis: the host binds the instance's feed (Face::setAnalysis). A shown face acquires the loudness
// (kWantLoudness: the feed's worker runs the front end, pq/dsp/loudness.h, with the parts this face draws) and sends its
// analysis options (setLoudness); the hosts poll it at ≤ 60 Hz while the readings move and it stops once they rest; the
// feed's waker (sound arrived) asks the host to poll again. A face polled but no longer painted lets go after one
// unanswered repaint: hidden, it costs nothing.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "faces.h"
#include "option_sheet.h"
#include "pq/dsp/loudness.h"
#include "pq/meta/registry.h"
#include "pq/ui/theme.h"

namespace pq::views {

namespace {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;
using sheet::kOnOff;
using sheet::Opt;
using dsp::kNoLoudness;

constexpr float kRadius = 10.0f;
constexpr float kLabelPx = 6.0f;          // world units
constexpr float kLabelMinDevicePx = 5.0f;  // below this the text is left out (the bars and traces stay)
enum Part : int32_t { kSetPart = 1, kClearPart = 2, kFreezePart = 3, kSheetPart = 4, kLampPart = 5, kPlotPart = 6, kTargetLine = 7,
                      kCutLine = 8, kRowBase = sheet::kRowBase };

bool inside(const Rect& r, Vec2 p) noexcept { return p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1; }
bool valid(float v) noexcept { return v > -119.0f; }

// PQ's three-colour language: cool under, calm on, warm over; everything else a tint of white.
constexpr Rgba kUnder = ui::rgba(104, 176, 255), kOn = ui::rgba(116, 214, 150), kOver = ui::rgba(255, 116, 96);
constexpr Rgba kMomentary = ui::rgba(150, 190, 235), kShortTerm = ui::rgba(120, 225, 175), kIntegrated = ui::rgba(255, 205, 120);
constexpr Rgba kPlate = ui::rgba(10, 12, 16, 200);
// The under / on / over blend: ±1 LU reads fully on target, beyond ±3 LU fully under / over.
Rgba stateColour(float deltaLu) noexcept {
  if (deltaLu >= 0.0f) return ui::lerp(kOn, kOver, std::clamp((deltaLu - 1.0f) * 0.5f, 0.0f, 1.0f));
  return ui::lerp(kOn, kUnder, std::clamp((-deltaLu - 1.0f) * 0.5f, 0.0f, 1.0f));
}
void fmtLufs(float v, char* out, size_t n) {
  if (!valid(v)) std::snprintf(out, n, "--.-");
  else std::snprintf(out, n, "%.1f", v);
}
// Frame-rate independent exponential ease (PQ lufs_meter).
float ease(float cur, float target, float perSecond, float dt) noexcept { return cur + (target - cur) * (1.0f - std::exp(-perSecond * dt)); }

// The delivery targets the presets name (PQ lufs_meter's combo).
constexpr const char* kPresetNames[] = {"Custom", "-14 Stream", "-16 Podcast", "-23 EBU", "-24 ATSC"};
constexpr float kPresetLufs[] = {-14.0f, -16.0f, -23.0f, -24.0f};

// The hosts poll a face's voice reader; this one stands for the feed's readings.
class FeedFrames final : public VoiceTapReader {
public:
  uint32_t read(std::span<FaceVoice>) override { return 0; }
  uint64_t serial() const noexcept override { return serial_; }
  bool live() const noexcept override { return live_; }
  uint64_t serial_ = 0;
  bool live_ = false;
};

// ---- the shared frame: options and their sheet, the feed, the chips, polling --------------------------------------------

class LoudnessFace : public MetaFace {
public:
  ~LoudnessFace() override { letGo(); }

  Vec2 worldSize() const noexcept override { return {w_, h_}; }
  float inputY(size_t, size_t) const noexcept override { return 0.5f * h_; }
  float jackInset() const noexcept override { return 0.0f; }
  float outputY(size_t, size_t) const noexcept override { return 0.5f * h_; }

  void setAnalysis(std::function<devices::SpectrumFeed*()> feedOf, std::function<void()> wakeHost) override {
    feedOf_ = std::move(feedOf);
    wakeHost_ = std::move(wakeHost);
    syncFeed();
  }
  bool wantsAnalysis() const noexcept override { return true; }
  VoiceTapReader* voices() const noexcept override { return feedOf_ ? &frames_ : nullptr; }
  bool animating() const noexcept override { return frames_.live_; }

  bool pollVoices() override {
    syncOptions();
    if (!syncFeed()) return true;  // no tap yet: one repaint (the host asks again at its next wake)
    const double now = clockNow();
    if (now - lastPaint_ > 0.5) {  // not painted lately: its host is hidden — after one unanswered repaint, let go
      if (probe_ == 0.0) {
        probe_ = now;
        return true;
      }
      if (lastPaint_ < probe_ && now - probe_ > 0.05) {
        letGo();
        frames_.live_ = false;
        probe_ = 0.0;
        return false;
      }
    }
    if (frozen_ || !acquired_) {
      frames_.live_ = false;
      return false;
    }
    const bool changed = read(now);
    frames_.live_ = !settled_ || moving_;
    if (changed) ++frames_.serial_;
    return changed;
  }

  // ---- static layers: body; the display's well; frame ----
  uint32_t staticLayerCount() const noexcept override { return 3; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.face.body : (layer == 1 ? th.device.display : th.face.frame);
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    if (layer == 0) p.roundedRect({1.0f, 1.0f, w_ - 1.0f, h_ - 1.0f}, kRadius, w);
    else if (layer == 1) p.roundedRect(well(), 4.0f, w);
    else outline(p, w);
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    const ui::Theme& th = *env.theme;
    syncOptions();
    syncFeed();
    lastPaint_ = clockNow();
    probe_ = 0.0;
    if (feed_ != nullptr && !acquired_) {
      acquire();
      if (!frozen_) (void)read(lastPaint_);  // a display that just appeared draws what the feed has at once
      if (wakeHost_) wakeHost_();
    }
    const bool labels = kLabelPx * p.transform().scale >= kLabelMinDevicePx * env.uiScale;
    p.drawList().pushClip(deviceRect(p, well()));
    paintBody(p, th, labels && !coversBody());
    p.drawList().popClip();
    if (labels) {
      chip(p, setChip(), "SET", sheet_, th);
      if (hasClear()) chip(p, clearChip(), "CLEAR", false, th);
      chip(p, freezeChip(), "FREEZE", frozen_, th);
      if (coversBody()) paintSheet(p, th);
    }
    for (const FaceJack& j : env.jacks) {  // leads in and out: the signal passes through unchanged
      if (!j.visible) continue;
      const float x0 = j.input ? 5.0f : well().x1, x1 = j.input ? well().x0 : w_ - 5.0f;
      p.line({x0, j.at.y}, {x1, j.at.y}, 1.25f, j.connected ? th.face.signal : th.face.jackRing);
    }
    if (env.selected || (env.hovered && hovered_ == kFrame)) outline(p, env.selected ? th.circuit.selected : th.circuit.hover);
  }
  void liveRects(std::vector<Rect>& out) const override { out.push_back({0.0f, 0.0f, w_, h_}); }

  // BR.ui: the sheet beside the face is a floating panel.
  std::optional<Rect> popupRect() const noexcept override {
    if (!sheet_ || !panel_.beside()) return std::nullopt;
    return panel_.area();
  }
  void paintPopup(ui::FacePainter& p, const FaceEnv& env) override {
    if (sheet_ && panel_.beside() && env.theme != nullptr) paintSheet(p, *env.theme);
  }
  void setPopupOverFace(bool on) override { hostOver_ = on; }

  int32_t hitTest(Vec2 l, float) const noexcept override {
    if (sheet_ && panel_.beside()) {
      const int32_t part = panel_.hit(l, rows_.size());
      if (part >= 0) return part == sheet::kBody ? kSheetPart : part;
    }
    if (l.x < 0.0f || l.y < 0.0f || l.x > w_ || l.y > h_) return kNone;
    if (inside(setChip(), l)) return kSetPart;
    if (hasClear() && inside(clearChip(), l)) return kClearPart;
    if (inside(freezeChip(), l)) return kFreezePart;
    if (sheet_) {
      const int32_t part = panel_.hit(l, rows_.size());
      if (part >= 0) return part == sheet::kBody ? kSheetPart : part;
    }
    const int32_t own = ownHit(l);
    if (own != kNone) return own;
    return kFrame;
  }

  bool onPointer(const FacePointer& e) override {
    using K = FacePointer::Kind;
    switch (e.kind) {
      case K::Down: {
        if (e.button != ui::PointerButton::Left) return false;
        const int32_t part = hitTest(e.local, e.pxPerUnit);
        if (part == kFreezePart) {
          frozen_ = !frozen_;
          if (!frozen_ && acquired_) (void)read(clockNow());
          if (wakeHost_) wakeHost_();
          return true;
        }
        if (part == kClearPart) {
          if (feed_ != nullptr) feed_->resetLoudness();
          cleared();
          if (wakeHost_) wakeHost_();
          return true;
        }
        if (part == kSetPart) {
          sheet_ = !sheet_;
          if (sheet_)
            panel_.place(sheet::choosePlace(e.device.x - e.local.x * e.pxPerUnit, e.pxPerUnit, w_, sheetW(), e.room.x0, e.room.x1, hostOver_),
                         w_, h_, sheetW());
          return true;
        }
        if (part >= kRowBase) return rowDown(static_cast<uint32_t>(part - kRowBase), e);
        if (part == sheet::kTabBase || part == sheet::kTabBase + 1) {
          panel_.tab = part - sheet::kTabBase;
          rows(panel_.tab, rows_);
          return true;
        }
        if (part == sheet::kSave) {
          meta::saveUserDefaultData(type().id, opts_.data());
          return true;
        }
        if (part == sheet::kReset) {
          opts_.reset();
          writeOptions();
          return true;
        }
        if (part == kSheetPart) return true;
        return ownDown(part, e);
      }
      case K::Move: {
        if (drag_) {
          const Opt& op = table_[dragOpt_];
          const double k = ((e.mods & ui::mod::kShift) ? 0.1 : 1.0) / 160.0;
          setOption(dragOpt_, sheet::fromTravel(op, std::clamp(pressTravel_ + (pressY_ - e.device.y) * k, 0.0, 1.0)));
          return true;
        }
        if (ownDrag(e)) return true;
        const Vec2 h = !coversBody() && inside(well(), e.local) ? e.local : Vec2{-1.0f, -1.0f};
        if (h.x == hover_.x && h.y == hover_.y) return false;
        hover_ = h;
        return true;
      }
      case K::Leave:
        if (hover_.x < 0.0f) return false;
        hover_ = {-1.0f, -1.0f};
        return true;
      case K::Up: {
        const bool was = drag_ || lineDrag_;
        drag_ = lineDrag_ = false;
        if (was && editing_) {
          target().end();
          editing_ = false;
        }
        return was;
      }
      case K::Wheel: return false;
    }
    return false;
  }
  bool dragging() const noexcept override { return drag_ || lineDrag_; }
  void cancelDrag() override {
    if ((drag_ || lineDrag_) && editing_) {
      target().cancel();
      editing_ = false;
      syncOptions(true);
    }
    drag_ = lineDrag_ = false;
  }
  bool onKey(const ui::KeyEvent& e) override {
    if (!e.down || e.key != ui::key::kEscape || !sheet_) return false;
    sheet_ = false;
    return true;
  }
  ui::Cursor cursor(int32_t part) const noexcept override {
    if (part >= kRowBase) {
      const auto i = static_cast<size_t>(part - kRowBase);
      return i < rows_.size() && sheet::numeric(table_[rows_[i]]) ? ui::Cursor::ResizeNS : ui::Cursor::Pointer;
    }
    if (part == kTargetLine || part == kCutLine) return ui::Cursor::ResizeNS;
    if (part == kSetPart || part == kClearPart || part == kFreezePart || part == kLampPart ||
        (part >= sheet::kTabBase && part <= sheet::kReset))
      return ui::Cursor::Pointer;
    return ui::Cursor::Inherit;
  }

  void describe(int32_t part, std::string& title, std::string& body) const override {
    switch (part) {
      case kSetPart:
        title = "Settings";
        body = sheet_ ? "Click to close the settings." : "Click for every option (ANALYSIS: what is measured; DISPLAY: how it looks).";
        return;
      case kClearPart:
        title = "Clear";
        body = "The measurement starts again: integrated loudness, LRA, peaks, maxima and the history.";
        return;
      case kFreezePart:
        title = "Freeze";
        body = frozen_ ? "Frozen: the picture holds (the signal still passes and is still measured). Click to follow it again."
                       : "Click to hold the picture (the signal still passes and is still measured).";
        return;
      case sheet::kTabBase:
        title = "Analysis";
        body = "What is measured.";
        return;
      case sheet::kTabBase + 1:
        title = "Display";
        body = "Only how it looks.";
        return;
      case sheet::kSave:
        title = "Save as default";
        body = std::string("New ") + std::string(type().name) + "s start with these options. Existing ones keep theirs.";
        return;
      case sheet::kReset:
        title = "Reset";
        body = "Every option back to the factory default.";
        return;
      default:
        if (part >= kRowBase && static_cast<size_t>(part - kRowBase) < rows_.size()) {
          const Opt& op = table_[rows_[static_cast<size_t>(part - kRowBase)]];
          title = op.label;
          body = sheet::numeric(op) ? "Drag up / down to set (Shift: fine); double-click resets."
                                    : "Click to change (Shift: back); double-click resets.";
          return;
        }
        if (ownDescribe(part, title, body)) return;
        MetaFace::describe(part, title, body);
    }
  }

  // Tests.
  const dsp::LoudnessReadings& shownReadings() const noexcept { return r_; }
  bool acquiredForTest() const noexcept { return acquired_; }
  bool frozenForTest() const noexcept { return frozen_; }
  size_t shownPoints() const noexcept { return hist_.size(); }
  dsp::LoudnessConfig configForTest() const { return loudConfig(); }
  void setOptionForTest(std::string_view key, double v) {
    for (uint32_t i = 0; i < table_.size(); ++i)
      if (key == table_[i].key) setOption(i, v);
  }
  void setFrozenForTest(bool f) { frozen_ = f; }
  dsp::LoudnessConfig configFor(const meta::DataValue* d) {
    opts_.load(d);
    return loudConfig();
  }
  void setSheetForTest(bool s) { sheet_ = s; }
  void pressForTest(int32_t part) {
    FacePointer e;
    e.kind = FacePointer::Kind::Down;
    if (part == kClearPart && feed_ != nullptr) feed_->resetLoudness();
    else (void)ownDown(part, e);
  }

protected:
  LoudnessFace(const meta::Type& t, float w, float h, std::span<const Opt> table)
      : MetaFace(t), w_(w), h_(h), table_(table), opts_(table),
        panel_{{8.0f, 23.0f, w - 8.0f, h - 8.0f}, 6.0f} {}

  // ---- what a face defines ----
  virtual dsp::LoudnessConfig loudConfig() const = 0;
  virtual void rows(int tab, std::vector<uint32_t>& out) const = 0;
  virtual void paintBody(ui::FacePainter& p, const ui::Theme& th, bool labels) = 0;
  virtual uint32_t historyPoints() const noexcept { return 0; }  // the points the face draws
  virtual int32_t ownHit(Vec2 /*l*/) const noexcept { return kNone; }
  virtual bool ownDown(int32_t /*part*/, const FacePointer& /*e*/) { return false; }
  virtual bool ownDrag(const FacePointer& /*e*/) { return false; }
  virtual bool ownDescribe(int32_t, std::string&, std::string&) const { return false; }
  virtual bool animate(float /*dt*/, bool /*fresh*/) { return false; }  // true while easing
  virtual void cleared() {}
  virtual bool hasClear() const noexcept { return true; }
  virtual float sheetW() const noexcept { return std::max(w_, 220.0f); }

  Rect well() const noexcept { return {6.0f, 6.0f, w_ - 6.0f, h_ - 6.0f}; }
  Rect freezeChip() const noexcept { return {w_ - 45.0f, 10.0f, w_ - 12.0f, 20.0f}; }
  Rect clearChip() const noexcept { return {w_ - 75.0f, 10.0f, w_ - 48.0f, 20.0f}; }
  Rect setChip() const noexcept {
    const float x1 = (hasClear() ? clearChip().x0 : freezeChip().x0) - 3.0f;
    return {x1 - 22.0f, 10.0f, x1, 20.0f};
  }
  bool coversBody() const noexcept { return sheet_ && !panel_.beside(); }
  static Rect deviceRect(const ui::FacePainter& p, const Rect& r) {
    const Vec2 a = p.transform().map({r.x0, r.y0}), b = p.transform().map({r.x1, r.y1});
    return {a.x, a.y, b.x, b.y};
  }

  void setOption(uint32_t o, double v) {
    opts_.set(o, v);
    optionSet(o);
    writeOptions();
  }
  virtual void optionSet(uint32_t /*o*/) {}
  // Starts an edit gesture on the instance's data (a line drag: one undo step).
  void beginEdit(const char* label) {
    if (hasTarget() && target().editable()) {
      target().begin(label);
      editing_ = true;
    }
  }

  devices::SpectrumFeed* feed_ = nullptr;
  bool acquired_ = false, settled_ = true, frozen_ = false, moving_ = false;
  float w_, h_;
  std::span<const Opt> table_;
  sheet::Values opts_;
  dsp::LoudnessReadings r_;
  std::vector<dsp::LoudnessPoint> hist_;  // the points shown, oldest first (≤ historyPoints)
  Vec2 hover_{-1.0f, -1.0f};
  bool lineDrag_ = false, editing_ = false;
  mutable FeedFrames frames_;
  std::function<void()> wakeHost_;

  static void chipAt(ui::FacePainter& p, const Rect& r, const char* label, bool on, const ui::Theme& th) {
    p.roundedRect(r, 3.0f, on ? ui::mulAlpha(th.face.value, 0.3f) : ui::rgba(8, 8, 12, 160));
    p.text({0.5f * (r.x0 + r.x1), 0.5f * (r.y0 + r.y1)}, label, 6.0f, on ? th.face.value : th.device.displayText);
  }

private:
  void chip(ui::FacePainter& p, const Rect& r, const char* label, bool on, const ui::Theme& th) const { chipAt(p, r, label, on, th); }
  void outline(ui::FacePainter& p, Rgba col) const {
    const Vec2 pts[5] = {{1.0f, 1.0f}, {w_ - 1.0f, 1.0f}, {w_ - 1.0f, h_ - 1.0f}, {1.0f, h_ - 1.0f}, {1.0f, 1.0f}};
    p.polyline(pts, 1.25f, col);
  }
  void paintSheet(ui::FacePainter& p, const ui::Theme& th) const {
    sheet::Values saved(table_);
    const bool isSaved = sheet::savedDefault(type().id, saved) && saved == opts_;
    panel_.paint(p, th, opts_, rows_, hovered_, isSaved);
  }

  void syncOptions(bool force = false) {
    const meta::DataPtr d = hasTarget() ? target().data() : nullptr;
    if (!force && d == data_ && optionsLoaded_) return;
    data_ = d;
    optionsLoaded_ = true;
    if (!local_) opts_.load(d.get());
    applyOptions();
  }
  void applyOptions() {
    rows(panel_.tab, rows_);
    if (feed_ != nullptr) feed_->setLoudness(loudConfig());  // (the feed ignores an unchanged request)
  }
  void writeOptions() {
    if (hasTarget() && target().editable()) {
      data_ = opts_.data();
      target().setData(data_);
      data_ = target().data();
    } else {
      local_ = true;  // a read-only target: the face keeps its options itself
    }
    applyOptions();
    if (wakeHost_) wakeHost_();
  }
  bool rowDown(uint32_t row, const FacePointer& e) {
    if (row >= rows_.size()) return false;
    const uint32_t o = rows_[row];
    const Opt& op = table_[o];
    if (e.clicks >= 2) {
      setOption(o, op.def);
      return true;
    }
    if (op.kind == Opt::Choice || op.kind == Opt::Toggle) {
      setOption(o, sheet::cycled(opts_, o, (e.mods & ui::mod::kShift) ? -1 : 1));
      return true;
    }
    drag_ = true;
    dragOpt_ = o;
    pressY_ = e.device.y;
    pressTravel_ = sheet::toTravel(op, opts_[o]);
    beginEdit(op.label);
    return true;
  }

  bool syncFeed() {
    devices::SpectrumFeed* f = feedOf_ ? feedOf_() : nullptr;
    if (f != feed_) {
      letGo();
      feed_ = f;
      seq_ = 0;
      primed_ = false;
      hist_.clear();
      if (feed_ != nullptr) feed_->setLoudness(loudConfig());
    }
    return feed_ != nullptr;
  }
  void acquire() {
    if (acquired_ || feed_ == nullptr) return;
    feed_->setLoudness(loudConfig());
    feed_->acquire(devices::kWantLoudness);
    feed_->addWaker(this, [this] {
      frames_.live_ = true;
      if (wakeHost_) wakeHost_();
    });
    acquired_ = true;
  }
  void letGo() {
    if (!acquired_) return;
    feed_->removeWaker(this);
    feed_->release(devices::kWantLoudness);
    acquired_ = false;
  }

  // Reads the feed's readings (and the history window when they are new) and advances the ballistics. True: changed.
  bool read(double now) {
    if (feed_ == nullptr) return false;
    bool settled = true;
    dsp::LoudnessReadings r;
    const uint64_t seq = feed_->loudnessRead(r, settled);
    settled_ = settled;
    const bool fresh = seq != seq_;
    if (fresh) {
      seq_ = seq;
      r_ = r;
      if (const uint32_t want = historyPoints(); want > 0) {
        const uint64_t start = std::max(r.historyStart, r.points > want ? r.points - want : 0);
        hist_.resize(static_cast<size_t>(r.points - start));
        uint32_t got = 0;
        (void)feed_->loudnessRead(r, settled, start, static_cast<uint32_t>(hist_.size()), hist_.data(), &got);
        hist_.resize(got);
      }
    }
    const float dt = primed_ ? static_cast<float>(std::clamp(now - lastAnim_, 0.0, 0.1)) : 0.0f;
    lastAnim_ = now;
    moving_ = animate(dt, !primed_);
    primed_ = true;
    return fresh || moving_;
  }

  std::function<devices::SpectrumFeed*()> feedOf_;
  double lastPaint_ = 0.0, probe_ = 0.0, lastAnim_ = 0.0;
  uint64_t seq_ = 0;
  bool primed_ = false;
  meta::DataPtr data_;
  bool optionsLoaded_ = false, local_ = false, sheet_ = false, hostOver_ = false;
  std::vector<uint32_t> rows_;
  sheet::Sheet panel_;
  bool drag_ = false;
  uint32_t dragOpt_ = 0;
  float pressY_ = 0.0f;
  double pressTravel_ = 0.0;
};

// ---- the Level Meter (PQ level_meter) -------------------------------------------------------------------------------

enum LevelOpt : uint32_t { lRms, lFall, lHold, lInf, lClip, lClipReset, lFloor, lReadouts, lZones, lCount };
const Opt kLevelOpts[lCount] = {
    {"rms", "RMS window", 300, 50, 1000, Opt::LogNumber, nullptr, "%.0f ms"},
    {"fall", "Peak fall", 20, 5, 60, Opt::LogNumber, nullptr, "%.0f dB/s"},
    {"hold", "Hold time", 2, 0.5, 10, Opt::LogNumber, nullptr, "%.1f s"},
    {"inf", "Infinite hold", 0, 0, 1, Opt::Toggle, kOnOff},
    {"clip", "Clip threshold", 0, -3, 0, Opt::Number, nullptr, "%.1f dBFS", 0.1},
    {"clipreset", "Clip auto reset", 0, 0, 30, Opt::Number, nullptr, "%.0f s", 1.0},
    {"floor", "Floor", -60, -90, -24, Opt::Number, nullptr, "%.0f dB", 1.0},
    {"ro", "Readouts", 1, 0, 1, Opt::Toggle, kOnOff},
    {"zones", "Zone colours", 1, 0, 1, Opt::Toggle, kOnOff},
};

class LevelMeterFace final : public LoudnessFace {
public:
  static constexpr float kW = 100.0f, kH = 160.0f;
  explicit LevelMeterFace(const meta::Type& t) : LoudnessFace(t, kW, kH, kLevelOpts) {}

protected:
  dsp::LoudnessConfig loudConfig() const override {
    dsp::LoudnessConfig c;
    c.parts = dsp::kLoudLevels;
    c.rmsWindowMs = static_cast<float>(opts_[lRms]);
    c.peakFallDbS = static_cast<float>(opts_[lFall]);
    c.holdSeconds = static_cast<float>(opts_[lHold]);
    c.holdInfinite = opts_.on(lInf);
    c.clipThreshDb = static_cast<float>(opts_[lClip]);
    c.clipResetS = static_cast<float>(opts_[lClipReset]);
    c.autoReset = false;
    return c;
  }
  void rows(int tab, std::vector<uint32_t>& out) const override {
    out.clear();
    if (tab == sheet::kAnalysis)
      for (uint32_t i = lRms; i <= lClipReset; ++i) out.push_back(i);
    else
      for (uint32_t i = lFloor; i < lCount; ++i) out.push_back(i);
  }
  bool hasClear() const noexcept override { return false; }  // the lamps are the reset (PQ)

  Rect lampRow() const noexcept { return {8.0f, 24.0f, kW - 8.0f, 32.0f}; }
  int32_t ownHit(Vec2 l) const noexcept override {
    if (inside({lampRow().x0, lampRow().y0 - 1.0f, lampRow().x1, lampRow().y1 + 2.0f}, l)) return kLampPart;
    return inside(well(), l) ? kPlotPart : kNone;
  }
  bool ownDown(int32_t part, const FacePointer&) override {
    if (part != kLampPart) return false;
    if (feed_ != nullptr) feed_->resetClips();  // click the lamps: the latch (and the holds) clear
    r_.clip[0] = r_.clip[1] = false;
    r_.clipCount[0] = r_.clipCount[1] = 0;
    if (wakeHost_) wakeHost_();
    return true;
  }
  bool ownDescribe(int32_t part, std::string& title, std::string& body) const override {
    if (part == kLampPart) {
      title = "Clip Lamps";
      body = "Light at the first sample at or over the clip threshold and stay lit (with the count of clips) until clicked "
             "(or the auto reset, SET → ANALYSIS). Click: reset the lamps and the peak holds.";
      return true;
    }
    if (part == kPlotPart) {
      title = "Level Meter";
      body = feed_ != nullptr ? "Per channel: the RMS body, the peak cap, the peak hold tick (sample peak, dBFS; inter-sample peaks: "
                                "the LUFS Meter's true peak). The signal is not changed."
                              : "L / R levels of the signal passing through (it is not changed). Shown when the module plays in "
                                "a track's chain.";
      return true;
    }
    return false;
  }

  void paintBody(ui::FacePainter& p, const ui::Theme& th, bool labels) override {
    using namespace ui::signal_theme;
    const float floorDb = static_cast<float>(opts_[lFloor]);
    const bool readouts = opts_.on(lReadouts) && labels, zones = opts_.on(lZones);
    const float span = -floorDb;
    auto frac = [&](float db) { return std::clamp((db - floorDb) / span, 0.0f, 1.0f); };
    auto zone = [&](float db, Rgba base, uint32_t alpha) {
      if (zones && db > kMeterHotDb) return ui::withAlpha(kMeterHot, alpha);
      if (zones && db > kMeterWarnDb) return ui::withAlpha(kMeterWarn, alpha);
      return ui::withAlpha(base, alpha);
    };
    const Rect lamps = lampRow();
    const float top = lamps.y1 + 4.0f, bot = kH - 8.0f - (readouts ? 18.0f : 2.0f);
    const float w = kW - 16.0f, gap = std::clamp(w * 0.18f, 10.0f, 18.0f);
    const float barW = (w - gap - 4.0f) * 0.5f;
    const float x0[2] = {10.0f, 10.0f + barW + gap};
    const Rgba body[2] = {kWaveform, kWaveformR};
    // dB rules across both bars every 6 dB, labelled every 12 in the centre gap.
    for (float g = 0.0f; g >= floorDb - 0.01f; g -= 6.0f) {
      const float y = bot - frac(g) * (bot - top);
      p.line({8.0f, y}, {kW - 8.0f, y}, 0.5f, kGrid);
      if (labels && g < -0.5f && static_cast<int>(g) % 12 == 0 && y > top + 4.0f && y < bot - 4.0f) {
        char lbl[8];
        std::snprintf(lbl, sizeof lbl, "%d", static_cast<int>(g));
        p.text({x0[0] + barW + 0.5f * gap, y}, lbl, 5.0f, kGridLabel);
      }
    }
    for (int c = 0; c < 2; ++c) {
      const Rect well{x0[c], top, x0[c] + barW, bot};
      p.roundedRect(well, 1.5f, kMeterWell);
      if (valid(r_.rmsDb[c]) && r_.rmsDb[c] > floorDb)
        p.roundedRect({well.x0, bot - frac(r_.rmsDb[c]) * (bot - top), well.x1, bot}, 1.5f, zone(r_.rmsDb[c], body[c], kMeterBodyAlpha));
      if (valid(r_.peakDb[c]) && r_.peakDb[c] > floorDb) {
        const float y = bot - frac(r_.peakDb[c]) * (bot - top);
        p.roundedRect({well.x0, y - 0.75f, well.x1, y + 0.75f}, 0.0f, zone(r_.peakDb[c], kMeterPeakCap, 255));
      }
      if (valid(r_.holdDb[c]) && r_.holdDb[c] > floorDb) {
        const float y = bot - frac(r_.holdDb[c]) * (bot - top);
        p.roundedRect({well.x0, y - 0.75f, well.x1, y}, 0.0f, kMeterHold);
      }
      const Rect lamp{well.x0, lamps.y0, well.x1, lamps.y1};
      p.roundedRect(lamp, 1.5f, r_.clip[c] ? kMeterLampOn : kMeterLampOff);
      if (labels && r_.clip[c] && r_.clipCount[c] > 1) {
        char n[8];
        std::snprintf(n, sizeof n, "%u", std::min<uint32_t>(r_.clipCount[c], 99));
        p.text({0.5f * (lamp.x0 + lamp.x1), 0.5f * (lamp.y0 + lamp.y1)}, n, 5.0f, ui::rgba(255, 255, 255));
      }
      if (readouts) {
        char buf[16];
        if (valid(r_.peakDb[c])) std::snprintf(buf, sizeof buf, "%.1f", std::max(r_.peakDb[c], -99.9f));
        else std::snprintf(buf, sizeof buf, "-inf");
        const float cx = 0.5f * (well.x0 + well.x1);
        p.text({cx, bot + 6.0f}, buf, 6.0f, r_.clip[c] ? kMeterLampOn : th.device.displayText);
        p.text({cx, bot + 14.0f}, c == 0 ? "L" : "R", 5.0f, kGridLabel);
      }
    }
  }
};

// ---- the LUFS Meter (PQ lufs_meter) ---------------------------------------------------------------------------------

enum LufsOpt : uint32_t { uPreset, uTarget, uTruePeak, uAuto, uAutoLu, uAutoSec, uLo, uHi, uTrace, uHistory, uCount };
const Opt kLufsOpts[uCount] = {
    {"preset", "Target preset", 0, 0, 4, Opt::Choice, kPresetNames},
    {"target", "Target", -8, -36, 0, Opt::Number, nullptr, "%.1f LUFS", 0.5},
    {"tp", "True peak (4x)", 1, 0, 1, Opt::Toggle, kOnOff},
    {"ar", "Auto-reset", 1, 0, 1, Opt::Toggle, kOnOff},
    {"arlu", "Quiet below target", 12, 3, 40, Opt::Number, nullptr, "-%.1f LU", 0.5},
    {"ars", "Quiet for", 5, 1, 30, Opt::Number, nullptr, "%.0f s", 1.0},
    {"lo", "Scale bottom", -40, -80, -20, Opt::Number, nullptr, "%.0f LUFS", 1.0},
    {"hi", "Scale top", 0, -20, 6, Opt::Number, nullptr, "%.0f LUFS", 1.0},
    {"trace", "History trace", 1, 0, 1, Opt::Toggle, kOnOff},
    {"hist", "Trace length", 60, 10, 180, Opt::Number, nullptr, "%.0f s", 5.0},
};

// The target options shared by the LUFS Meter and the History: a preset sets the target, a target edit leaves the
// preset (Custom).
template <uint32_t Preset, uint32_t Target>
void presetSet(sheet::Values& o, uint32_t changed) {
  if (changed == Preset && o.choice(Preset) > 0) o.set(Target, kPresetLufs[o.choice(Preset) - 1]);
  if (changed == Target) o.set(Preset, 0.0);
}

class LufsMeterFace final : public LoudnessFace {
public:
  static constexpr float kW = 200.0f, kH = 160.0f;
  explicit LufsMeterFace(const meta::Type& t) : LoudnessFace(t, kW, kH, kLufsOpts) {}

protected:
  float targetLufs() const noexcept { return static_cast<float>(opts_[uTarget]); }
  float lo() const noexcept { return static_cast<float>(opts_[uLo]); }
  float hi() const noexcept { return std::max(static_cast<float>(opts_[uHi]), lo() + 10.0f); }
  dsp::LoudnessConfig loudConfig() const override {
    dsp::LoudnessConfig c;
    c.parts = dsp::kLoudLoudness | dsp::kLoudHistory | (opts_.on(uTruePeak) ? dsp::kLoudTruePeak : 0u);
    c.targetLufs = targetLufs();
    c.autoReset = opts_.on(uAuto);
    c.autoResetBelowLu = static_cast<float>(opts_[uAutoLu]);
    c.autoResetSec = static_cast<float>(opts_[uAutoSec]);
    return c;
  }
  void optionSet(uint32_t o) override { presetSet<uPreset, uTarget>(opts_, o); }
  void rows(int tab, std::vector<uint32_t>& out) const override {
    out.clear();
    if (tab == sheet::kAnalysis)
      for (uint32_t i = uPreset; i <= uAutoSec; ++i) out.push_back(i);
    else
      for (uint32_t i = uLo; i < uCount; ++i) out.push_back(i);
  }
  uint32_t historyPoints() const noexcept override { return opts_.on(uTrace) ? static_cast<uint32_t>(opts_[uHistory] * 10.0) : 0u; }
  void cleared() override { anim_.primed = false; }

  Rect tank() const noexcept { return {10.0f, 10.0f, 60.0f, kH - 10.0f}; }
  int32_t ownHit(Vec2 l) const noexcept override { return inside(well(), l) ? kPlotPart : kNone; }
  bool ownDescribe(int32_t part, std::string& title, std::string& body) const override {
    if (part != kPlotPart) return false;
    title = "LUFS Meter";
    body = feed_ != nullptr
               ? "ITU-R BS.1770-4 loudness of the signal passing through (it is not changed). The column: momentary loudness, "
                 "green within 1 LU of the target, blue under, red over; the lines: 0 LUFS, the target, the loudest short-term "
                 "so far. Beside it: short-term (big), momentary, integrated, loudness range, true peak, maxima, L/R correlation."
               : "Loudness of the signal passing through (it is not changed). Shown when the module plays in a track's chain.";
    return true;
  }

  // PQ advanceAnim: the bar rises fast and falls slowly; the headline and the integrated number ease.
  bool animate(float dt, bool fresh) override {
    const dsp::LoudnessReadings& r = r_;
    if (fresh || !anim_.primed) {
      anim_ = {r.momentary, r.shortTerm, r.integrated, 0.0f, true};
      return false;
    }
    if (dt <= 0.0f) return true;
    const float mTarget = valid(r.momentary) ? r.momentary : lo();
    const float prev = anim_.momentary;
    anim_.momentary = ease(anim_.momentary, mTarget, mTarget > prev ? 26.0f : 7.0f, dt);
    const float surge = std::clamp((anim_.momentary - prev) * 0.9f, 0.0f, 1.0f);
    anim_.glow = std::max(surge, ease(anim_.glow, 0.0f, 4.0f, dt));
    anim_.shortTerm = valid(r.shortTerm) && valid(anim_.shortTerm) ? ease(anim_.shortTerm, r.shortTerm, 9.0f, dt) : r.shortTerm;
    anim_.integrated = valid(r.integrated) && valid(anim_.integrated) ? ease(anim_.integrated, r.integrated, 6.0f, dt) : r.integrated;
    if (std::fabs(anim_.momentary - mTarget) < 0.02f) anim_.momentary = mTarget;
    if (std::fabs(anim_.shortTerm - r.shortTerm) < 0.02f) anim_.shortTerm = r.shortTerm;
    if (std::fabs(anim_.integrated - r.integrated) < 0.02f) anim_.integrated = r.integrated;
    if (anim_.glow < 0.01f) anim_.glow = 0.0f;
    return anim_.momentary != mTarget || anim_.shortTerm != r.shortTerm || anim_.integrated != r.integrated || anim_.glow > 0.0f;
  }

  void paintBody(ui::FacePainter& p, const ui::Theme& th, bool labels) override {
    const float target = targetLufs(), lo_ = lo(), hi_ = hi();
    const Rect t = tank();
    auto yOf = [&](float v) { return t.y1 - std::clamp((v - lo_) / (hi_ - lo_), 0.0f, 1.0f) * (t.y1 - t.y0); };
    const Rgba white = ui::rgba(255, 255, 255);
    // ---- the tank: the track, the history behind, the momentary fill, the marks, the riding readout ----
    p.roundedRect(t, 2.0f, ui::rgba(255, 255, 255, 14));
    if (hist_.size() > 2) {  // the short-term trace behind the fill (white: legible over any state)
      pts_.clear();
      const auto maxN = static_cast<float>(std::max<uint32_t>(2, historyPoints()) - 1);
      for (size_t i = 0; i < hist_.size(); ++i) {
        const float v = hist_[i].shortTerm;
        pts_.push_back({t.x1 - static_cast<float>(hist_.size() - 1 - i) / maxN * (t.x1 - t.x0), yOf(valid(v) ? v : lo_)});
      }
      p.polyline(pts_, 0.8f, ui::rgba(255, 255, 255, 105));
    }
    const Rgba fill = stateColour(anim_.momentary - target);
    if (valid(anim_.momentary) && anim_.momentary > lo_) {
      const float fy = yOf(anim_.momentary);
      if (t.y1 - fy > 0.5f) {
        p.roundedRect({t.x0, fy, t.x1, t.y1}, 2.0f, ui::mulAlpha(fill, 0.30f));
        const Vec2 sheen[2] = {{t.x0, fy}, {t.x1, fy}};  // the top's sheen (PQ gradientBarV)
        p.fillUnder(sheen, fy + 0.55f * (t.y1 - fy), ui::rgba(255, 255, 255, 30), ui::rgba(255, 255, 255, 0));
        const float glow = 0.40f + anim_.glow * 0.55f;
        for (int i = 3; i >= 1; --i) {  // the halo round the head (PQ glow)
          const float g = 1.2f * static_cast<float>(i);
          p.roundedRect({t.x0 - g, fy - g, t.x1 + g, std::min(t.y1, fy + 2.0f) + g}, g + 1.0f, ui::mulAlpha(fill, glow * 0.10f / static_cast<float>(i)));
        }
        p.roundedRect({t.x0, fy, t.x1, std::min(t.y1, fy + 1.5f)}, 0.5f, fill);
        p.roundedRect({t.x0, fy, t.x1, std::min(t.y1, fy + 0.75f)}, 0.0f, ui::mulAlpha(white, 0.75f));
      }
    }
    {
      const float zy = std::min(yOf(0.0f) + 0.5f, t.y1 - 0.5f);  // 0 LUFS (often the top: nudged inside)
      p.line({t.x0, zy}, {t.x1, zy}, 0.75f, ui::mulAlpha(kOver, 0.7f));
      const float ty = yOf(target);
      if (ty > t.y0 && ty < t.y1) p.line({t.x0, ty}, {t.x1, ty}, 1.2f, ui::mulAlpha(kOn, 0.95f));
      if (valid(r_.maxShortTerm) && r_.maxShortTerm > lo_) {
        const float my = yOf(r_.maxShortTerm);
        const float inset = 0.2f * (t.x1 - t.x0);
        if (my > t.y0 && my < t.y1) p.line({t.x0 + inset, my}, {t.x1 - inset, my}, 0.75f, ui::rgba(255, 255, 255, 110));
      }
    }
    const float cx = 0.5f * (t.x0 + t.x1);
    const bool head = valid(anim_.shortTerm);
    const float delta = head ? anim_.shortTerm - target : 0.0f;
    const Rgba stCol = head ? stateColour(delta) : ui::rgba(150, 150, 165, 200);
    if (labels) {
      char buf[24];
      fmtLufs(valid(anim_.momentary) && anim_.momentary > lo_ ? anim_.momentary : kNoLoudness, buf, sizeof buf);
      const float fy = valid(anim_.momentary) && anim_.momentary > lo_ ? yOf(anim_.momentary) : t.y1;
      const float ty = std::clamp(fy + 2.0f, t.y0 + 1.0f, t.y1 - 10.0f);  // always under the line (no flipping)
      p.roundedRect({cx - 13.0f, ty, cx + 13.0f, ty + 9.0f}, 1.5f, ui::rgba(10, 12, 16, 190));
      p.text({cx, ty + 4.5f}, buf, 7.0f, stCol);
      if (head) {
        std::snprintf(buf, sizeof buf, "%+.1f", delta);
        p.text({cx - (r_.truePeakDb > -1.0f ? 4.0f : 0.0f), t.y0 + 6.0f}, buf, 6.0f, ui::mulAlpha(stCol, 0.9f));
      }
      // The cursor's level: a hairline and its value.
      if (hover_.x >= t.x0 && hover_.x <= t.x1 && hover_.y >= t.y0 && hover_.y <= t.y1) {
        const float v = lo_ + (t.y1 - hover_.y) / (t.y1 - t.y0) * (hi_ - lo_);
        p.line({t.x0, hover_.y}, {t.x1, hover_.y}, 0.5f, ui::rgba(255, 255, 255, 130));
        std::snprintf(buf, sizeof buf, "%.1f", v);
        const float yy = hover_.y - 9.0f < t.y0 ? hover_.y + 2.0f : hover_.y - 9.0f;
        p.roundedRect({cx - 11.0f, yy, cx + 11.0f, yy + 7.5f}, 1.5f, kPlate);
        p.text({cx, yy + 3.75f}, buf, 5.5f, ui::rgba(255, 255, 255, 220));
      }
    }
    if (r_.truePeakDb > -1.0f) p.disc({t.x1 - 3.5f, t.y0 + 3.5f}, 1.8f, kOver);  // over −1 dBTP
    // ---- the numbers (far out, where the small text is left out, the short-term headline stays) ----
    const float x0 = 68.0f, x1 = kW - 12.0f;
    char big[16], line[40];
    fmtLufs(anim_.shortTerm, big, sizeof big);
    if (!labels) {
      if (!coversBody() && 20.0f * p.transform().scale >= 12.0f) p.textAligned({x0, 70.0f}, big, 20.0f, stCol, 0.0f);
      return;
    }
    p.textAligned({x0, 38.0f}, big, 20.0f, stCol, 0.0f);
    p.textAligned({x1, 42.0f}, "LUFS S", 6.0f, kLabel, 1.0f);
    if (head) std::snprintf(line, sizeof line, "%+.1f LU", delta);
    else std::snprintf(line, sizeof line, "target %.1f", target);
    p.textAligned({x0, 54.0f}, line, 6.5f, ui::mulAlpha(stCol, 0.72f), 0.0f);
    auto row = [&](float y, const char* label, const char* value, Rgba col) {
      p.textAligned({x0, y}, label, 6.0f, kLabel, 0.0f);
      p.textAligned({x1, y}, value, 6.5f, col, 1.0f);
    };
    const Rgba text = th.device.displayText;
    char v[24];
    fmtLufs(r_.momentary, v, sizeof v);
    row(68.0f, "M", v, valid(r_.momentary) ? stateColour(r_.momentary - target) : text);
    fmtLufs(anim_.integrated, v, sizeof v);
    row(79.0f, "I", v, valid(anim_.integrated) ? stateColour(anim_.integrated - target) : text);
    std::snprintf(v, sizeof v, "%.1f LU", r_.lra);
    row(90.0f, "LRA", v, text);
    const bool tp = opts_.on(uTruePeak);
    if (valid(r_.truePeakDb)) std::snprintf(v, sizeof v, "%+.1f %s", r_.truePeakDb, tp ? "dBTP" : "dB");
    else std::snprintf(v, sizeof v, "--.-");
    row(101.0f, tp ? "TP" : "PEAK", v, r_.truePeakDb > -1.0f ? kOver : text);
    fmtLufs(r_.maxMomentary, v, sizeof v);
    row(112.0f, "MAX M", v, text);
    fmtLufs(r_.maxShortTerm, v, sizeof v);
    row(123.0f, "MAX S", v, text);
    std::snprintf(v, sizeof v, "%+.2f", r_.correlation);
    row(134.0f, "CORR", v, r_.correlation < 0.0f ? kOver : text);
    // The correlation bar: −1 … +1, filled from the centre (red where the channels oppose).
    const Rect bar{x0, 140.0f, x1, 144.0f};
    p.roundedRect(bar, 1.0f, ui::rgba(255, 255, 255, 16));
    const float mid = 0.5f * (bar.x0 + bar.x1), cxv = mid + std::clamp(r_.correlation, -1.0f, 1.0f) * 0.5f * (bar.x1 - bar.x0);
    if (std::fabs(cxv - mid) > 0.25f)
      p.roundedRect({std::min(mid, cxv), bar.y0, std::max(mid, cxv), bar.y1}, 1.0f, r_.correlation < 0.0f ? ui::mulAlpha(kOver, 0.85f) : ui::mulAlpha(kOn, 0.85f));
    p.line({mid, bar.y0 - 1.0f}, {mid, bar.y1 + 1.0f}, 0.5f, ui::rgba(255, 255, 255, 120));
  }

private:
  static constexpr Rgba kLabel = ui::rgba(255, 255, 255, 110);
  struct Anim {
    float momentary = kNoLoudness, shortTerm = kNoLoudness, integrated = kNoLoudness, glow = 0.0f;
    bool primed = false;
  } anim_;
  std::vector<Vec2> pts_;
};

// ---- the Loudness History (PQ loudness_graph) -----------------------------------------------------------------------

enum HistOpt : uint32_t { hPreset, hTarget, hTol, hAuto, hAutoLu, hAutoSec, hWindow, hScale, hLo, hHi, hM, hS, hI, hBand, hGrid, hReadouts, hCount };
constexpr const char* kScaleNames[] = {"Manual", "Auto", "Target"};
const Opt kHistOpts[hCount] = {
    {"preset", "Target preset", 0, 0, 4, Opt::Choice, kPresetNames},
    {"target", "Target", -8, -32, 0, Opt::Number, nullptr, "%.1f LUFS", 0.1},
    {"tol", "Tolerance", 2, 0, 6, Opt::Number, nullptr, "%.1f LU", 0.1},
    {"ar", "Auto-reset", 1, 0, 1, Opt::Toggle, kOnOff},
    {"arlu", "Quiet below target", 12, 3, 40, Opt::Number, nullptr, "-%.1f LU", 0.1},
    {"ars", "Quiet for", 5, 1, 30, Opt::Number, nullptr, "%.0f s", 1.0},
    {"hist", "History", 30, 10, 180, Opt::Number, nullptr, "%.0f s", 5.0},
    {"scale", "Scale", 0, 0, 2, Opt::Choice, kScaleNames},
    {"lo", "Floor", -36, -60, -10, Opt::Number, nullptr, "%.0f LUFS", 1.0},
    {"hi", "Ceiling", 0, -30, 6, Opt::Number, nullptr, "%.0f LUFS", 1.0},
    {"m", "Momentary", 1, 0, 1, Opt::Toggle, kOnOff},
    {"s", "Short-term", 1, 0, 1, Opt::Toggle, kOnOff},
    {"i", "Integrated", 1, 0, 1, Opt::Toggle, kOnOff},
    {"band", "Tolerance band", 1, 0, 1, Opt::Toggle, kOnOff},
    {"grid", "Grid", 1, 0, 1, Opt::Toggle, kOnOff},
    {"ro", "Readouts", 1, 0, 1, Opt::Toggle, kOnOff},
};

class LoudnessHistoryFace final : public LoudnessFace {
public:
  static constexpr float kW = 300.0f, kH = 160.0f;
  explicit LoudnessHistoryFace(const meta::Type& t) : LoudnessFace(t, kW, kH, kHistOpts) {}

protected:
  float targetLufs() const noexcept { return static_cast<float>(opts_[hTarget]); }
  float cutoff() const noexcept { return targetLufs() - static_cast<float>(opts_[hAutoLu]); }
  int seconds() const noexcept { return static_cast<int>(std::lround(opts_[hWindow])); }
  dsp::LoudnessConfig loudConfig() const override {
    dsp::LoudnessConfig c;
    c.parts = dsp::kLoudLoudness | dsp::kLoudHistory;
    c.targetLufs = targetLufs();
    c.autoReset = opts_.on(hAuto);
    c.autoResetBelowLu = static_cast<float>(opts_[hAutoLu]);
    c.autoResetSec = static_cast<float>(opts_[hAutoSec]);
    return c;
  }
  void optionSet(uint32_t o) override { presetSet<hPreset, hTarget>(opts_, o); }
  void rows(int tab, std::vector<uint32_t>& out) const override {
    out.clear();
    if (tab == sheet::kAnalysis)
      for (uint32_t i = hPreset; i <= hAutoSec; ++i) out.push_back(i);
    else
      for (uint32_t i = hWindow; i < hCount; ++i) out.push_back(i);
  }
  uint32_t historyPoints() const noexcept override { return static_cast<uint32_t>(seconds()) * 10u; }

  // ---- the plot's geometry and scale (recomputed per paint; the line drags use the last one) ----
  struct Scale {
    float lo = -36.0f, hi = 0.0f;
    bool warped = true;
    float plotL = 0, plotR = 0, plotT = 0, plotB = 0;
    float t(float lufs) const noexcept {
      constexpr float knee = -15.0f, frac = 0.85f;
      if (!warped || knee <= lo || knee >= hi) return std::clamp((lufs - lo) / (hi - lo), 0.0f, 1.0f);
      if (lufs >= knee) return (1.0f - frac) + std::clamp((lufs - knee) / (hi - knee), 0.0f, 1.0f) * frac;
      const float u = std::clamp((lufs - lo) / (knee - lo), 0.0f, 1.0f);
      return u * u * (1.0f - frac);
    }
    float y(float lufs) const noexcept { return plotB - t(lufs) * (plotB - plotT); }
    float lufsAt(float yy) const noexcept {  // the inverse of y
      constexpr float knee = -15.0f, frac = 0.85f;
      const float tt = std::clamp((plotB - yy) / std::max(1.0f, plotB - plotT), 0.0f, 1.0f);
      if (!warped || knee <= lo || knee >= hi) return lo + tt * (hi - lo);
      if (tt >= 1.0f - frac) return knee + ((tt - (1.0f - frac)) / frac) * (hi - knee);
      return lo + std::sqrt(std::max(0.0f, tt / (1.0f - frac))) * (knee - lo);
    }
    float slope(float yy) const noexcept { return std::max(0.001f, (lufsAt(yy - 2.0f) - lufsAt(yy + 2.0f)) * 0.25f); }  // LU / unit
  };
  Scale scale() const {
    Scale s;
    const float target = targetLufs();
    const int mode = opts_.choice(hScale);
    s.lo = static_cast<float>(opts_[hLo]);
    s.hi = static_cast<float>(opts_[hHi]);
    s.warped = mode == 0;
    if (mode == 2) {  // Target: −10 … +8 LU hung off the target (dragging it scrolls the scale)
      s.lo = target - 10.0f;
      s.hi = target + 8.0f;
    }
    if (mode == 1 && !hist_.empty()) {  // Auto: the visible short-term (else momentary) range and the target, padded
      float mn = target, mx = target;
      bool any = false;
      for (const dsp::LoudnessPoint& pt : hist_) {
        const float v = opts_.on(hS) && valid(pt.shortTerm) ? pt.shortTerm : valid(pt.momentary) ? pt.momentary : kNoLoudness;
        if (!valid(v)) continue;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
        any = true;
      }
      if (any) {
        s.lo = std::floor(mn) - 3.0f;
        s.hi = std::ceil(mx) + 3.0f;
        if (s.hi - s.lo < 8.0f) s.hi = s.lo + 8.0f;
      }
    }
    if (opts_.on(hAuto) && mode != 0) s.lo = std::min(s.lo, cutoff() - 2.0f);  // keep the quiet line on screen
    if (s.hi - s.lo < 1.0f) s.hi = s.lo + 1.0f;
    const bool readouts = opts_.on(hReadouts);
    s.plotL = 6.0f + (readouts ? 20.0f : 4.0f);
    s.plotR = kW - 8.0f;
    s.plotT = 8.0f;
    s.plotB = kH - (readouts ? 17.0f : 8.0f);  // the footer's strip under the plot
    return s;
  }
  bool cutShown(const Scale& s) const noexcept { return opts_.on(hAuto) && cutoff() >= s.lo && cutoff() <= s.hi; }

  int32_t ownHit(Vec2 l) const noexcept override {
    if (!inside(well(), l)) return kNone;
    const Scale s = scale();
    if (l.x < s.plotL || l.x > s.plotR) return kPlotPart;
    const float dT = std::fabs(l.y - s.y(targetLufs())), dC = std::fabs(l.y - s.y(cutoff()));
    if (dT <= 5.0f && (!cutShown(s) || dT <= dC)) return kTargetLine;  // the nearer line takes the press (ties: the target)
    if (cutShown(s) && dC <= 5.0f) return kCutLine;
    return kPlotPart;
  }
  bool ownDown(int32_t part, const FacePointer& e) override {
    if (part != kTargetLine && part != kCutLine) return false;
    // A RELATIVE drag (PQ): the line moves by the pointer's travel at the scale's LU per unit at the press.
    const Scale s = scale();
    lineDrag_ = true;
    dragCut_ = part == kCutLine;
    dragBase_ = dragCut_ ? cutoff() : targetLufs();
    dragY_ = e.local.y;
    dragSlope_ = s.slope(s.y(dragBase_));
    beginEdit(dragCut_ ? "Auto-reset Line" : "Target");
    return true;
  }
  bool ownDrag(const FacePointer& e) override {
    if (!lineDrag_) return false;
    const float v = dragBase_ + (dragY_ - e.local.y) * dragSlope_;
    if (dragCut_) {  // stored as the offset below the target, at least 3 LU (inside the window it would reset every verse)
      setOption(hAutoLu, std::clamp(std::round((targetLufs() - v) * 10.0f) * 0.1f, 3.0f, 40.0f));
    } else {
      setOption(hTarget, std::clamp(std::round(v * 10.0f) * 0.1f, -32.0f, 0.0f));
    }
    return true;
  }
  bool ownDescribe(int32_t part, std::string& title, std::string& body) const override {
    if (part == kTargetLine) {
      title = "Target";
      body = "The delivery target (green) and its tolerance band. Drag up / down to move it.";
      return true;
    }
    if (part == kCutLine) {
      title = "Auto-reset Line";
      body = "Momentary loudness held under this line for the quiet time (SET → ANALYSIS) clears the integrated measurement. "
             "Drag up / down (at least 3 LU under the target).";
      return true;
    }
    if (part == kPlotPart) {
      title = "Loudness History";
      body = feed_ != nullptr ? "Momentary (blue), short-term (green) and integrated (amber) loudness over time against the target. "
                                "Point at it for the values at that moment. The signal is not changed."
                              : "Loudness over time of the signal passing through (it is not changed). Shown when the module "
                                "plays in a track's chain.";
      return true;
    }
    return false;
  }

  void paintBody(ui::FacePainter& p, const ui::Theme& th, bool labels) override {
    const Scale s = scale();
    const float target = targetLufs(), tol = static_cast<float>(opts_[hTol]), cut = cutoff();
    const bool readouts = opts_.on(hReadouts) && labels;
    const int secs = seconds();
    const float plotL = s.plotL, plotR = s.plotR, plotT = s.plotT, plotB = s.plotB;
    const Rgba grid = ui::signal_theme::kGrid, gridLabel = ui::signal_theme::kGridLabel;
    if (opts_.on(hBand) && tol > 0.01f)
      p.roundedRect({plotL, s.y(target + tol), plotR, s.y(target - tol)}, 0.0f, ui::mulAlpha(kOn, 0.10f));
    if (opts_.on(hGrid)) {
      // Rules every 1 / 2 / 5 / 10 / 20 LU, the first with ≥ 14 units between (sized on the dense part of a warped
      // scale); a rule within 6 units of the last drawn is dropped, a label within a text height of the last one too.
      const float dense = s.warped && -15.0f > s.lo && -15.0f < s.hi ? s.hi + 15.0f : s.hi - s.lo;
      float step = 1.0f;
      for (const float st : {1.0f, 2.0f, 5.0f, 10.0f, 20.0f}) {
        step = st;
        if ((plotB - plotT) * 0.85f * st / std::max(1.0f, dense) >= 14.0f) break;
      }
      float lastY = -1e9f, lastLab = -1e9f;
      for (float v = std::ceil(s.lo / step) * step; v <= s.hi + 0.001f; v += step) {
        const float y = s.y(v);
        if (std::fabs(y - lastY) < 6.0f) continue;
        lastY = y;
        p.line({plotL, y}, {plotR, y}, 0.5f, grid);
        if (readouts && std::fabs(y - lastLab) >= 8.0f && y > plotT + 3.0f && y < plotB - 3.0f) {
          lastLab = y;
          char lab[12];
          std::snprintf(lab, sizeof lab, "%.0f", v);
          p.textAligned({plotL - 3.0f, y}, lab, 5.0f, gridLabel, 1.0f);
        }
      }
      float tick = 10.0f;
      for (const float st : {10.0f, 15.0f, 30.0f, 60.0f, 120.0f}) {
        tick = st;
        if ((plotR - plotL) * st / static_cast<float>(secs) >= 40.0f) break;
      }
      for (float sec = tick; sec < static_cast<float>(secs) - 0.01f; sec += tick) {
        const float x = plotR - sec / static_cast<float>(secs) * (plotR - plotL);
        if (x > plotL) p.line({x, plotT}, {x, plotB}, 0.5f, grid);
      }
    }
    const bool hotT = hovered_ == kTargetLine || (lineDrag_ && !dragCut_);
    const bool hotC = hovered_ == kCutLine || (lineDrag_ && dragCut_);
    p.line({plotL, s.y(target)}, {plotR, s.y(target)}, hotT ? 1.6f : 1.0f, ui::mulAlpha(kOn, hotT ? 1.0f : 0.75f));
    if (opts_.on(hAuto) && (lineDrag_ && dragCut_ ? true : cutShown(s))) {
      const Vec2 cl[2] = {{plotL, s.y(cut)}, {plotR, s.y(cut)}};
      p.dashed(cl, hotC ? 1.6f : 1.0f, 3.5f, 2.5f, ui::mulAlpha(kOver, hotC ? 1.0f : 0.6f));
    }
    // ---- the traces: the newest at the right edge, a partial history starts partway (constant scroll rate) ----
    const size_t cap = static_cast<size_t>(secs) * 10;
    auto xOf = [&](size_t idx) {
      const size_t age = hist_.size() - 1 - idx;
      return plotR - static_cast<float>(age) / static_cast<float>(std::max<size_t>(1, cap - 1)) * (plotR - plotL);
    };
    p.drawList().pushClip(deviceRect(p, {plotL, plotT, plotR, plotB}));
    auto trace = [&](float dsp::LoudnessPoint::*field, Rgba col, float thick, bool fill) {
      if (hist_.size() < 2) return;
      pts_.clear();
      auto flush = [&] {
        if (pts_.size() >= 2) {
          if (fill) p.fillUnder(pts_, plotB, ui::mulAlpha(col, 0.14f), ui::mulAlpha(col, 0.02f));
          p.polyline(pts_, thick, col);
        }
        pts_.clear();
      };
      for (size_t i = 0; i < hist_.size(); ++i) {
        const float v = hist_[i].*field;
        if (!valid(v)) {  // a gap (pre-roll, silence under the gate) breaks the line instead of diving to the floor
          flush();
          continue;
        }
        const float x = xOf(i);
        if (x < plotL - 2.0f) continue;
        pts_.push_back({x, s.y(v)});
      }
      flush();
    };
    if (opts_.on(hM)) trace(&dsp::LoudnessPoint::momentary, ui::mulAlpha(kMomentary, 0.55f), 0.7f, false);
    if (opts_.on(hS)) trace(&dsp::LoudnessPoint::shortTerm, kShortTerm, 1.2f, true);
    if (opts_.on(hI)) trace(&dsp::LoudnessPoint::integrated, ui::mulAlpha(kIntegrated, 0.9f), 1.0f, false);
    p.drawList().popClip();
    p.line({plotR - 0.5f, plotT}, {plotR - 0.5f, plotB}, 0.5f, ui::mulAlpha(th.face.value, frozen_ ? 0.10f : 0.25f));  // the live edge
    if (!labels) return;
    char buf[48], num[16];
    // The hover inspector: a hairline and the three values there (whatever traces are shown) and its age.
    if (hover_.x >= plotL && hover_.x <= plotR && hover_.y >= plotT && hover_.y <= plotB && !hist_.empty() && !hotT && !hotC) {
      const float tt = std::clamp((plotR - hover_.x) / std::max(1.0f, plotR - plotL), 0.0f, 1.0f);
      const auto age = static_cast<size_t>(std::lround(tt * static_cast<float>(std::max<size_t>(1, cap - 1))));
      if (age < hist_.size()) {
        const size_t idx = hist_.size() - 1 - age;
        const dsp::LoudnessPoint& pt = hist_[idx];
        const float hx = xOf(idx);
        p.line({hx, plotT}, {hx, plotB}, 0.5f, ui::rgba(255, 255, 255, 90));
        const float boxW = 44.0f, boxH = 34.0f;
        float bx = hx + 5.0f;
        if (bx + boxW > plotR - 1.0f) bx = hx - 5.0f - boxW;
        const float by = std::clamp(hover_.y - 0.5f * boxH, plotT + 1.0f, plotB - boxH - 1.0f);
        p.roundedRect({bx, by, bx + boxW, by + boxH}, 2.0f, kPlate);
        const char* names[3] = {"M", "S", "I"};
        const float vals[3] = {pt.momentary, pt.shortTerm, pt.integrated};
        const Rgba cols[3] = {ui::mulAlpha(kMomentary, 0.9f), kShortTerm, kIntegrated};
        for (int i = 0; i < 3; ++i) {
          fmtLufs(vals[i], num, sizeof num);
          std::snprintf(buf, sizeof buf, "%s %s", names[i], num);
          p.textAligned({bx + 3.0f, by + 5.0f + 8.0f * static_cast<float>(i)}, buf, 5.5f, cols[i], 0.0f);
        }
        std::snprintf(buf, sizeof buf, "-%.1f s", static_cast<float>(age) * 0.1f);
        p.textAligned({bx + 3.0f, by + 29.0f}, buf, 5.0f, gridLabel, 0.0f);
      }
    }
    if (hotT) {
      std::snprintf(buf, sizeof buf, "target %.1f", target);
      paintTag(p, buf, s.y(target), kOn, plotL, plotR, plotT);
    } else if (hotC) {
      std::snprintf(buf, sizeof buf, "reset below %.1f", cut);
      paintTag(p, buf, s.y(cut), kOver, plotL, plotR, plotT);
    }
    if (!readouts) return;
    // The header: S, M, I and the integrated's distance from the target (the number a mastering pass steers).
    const dsp::LoudnessPoint last = hist_.empty() ? dsp::LoudnessPoint{} : hist_.back();
    float x = plotL + 4.0f;
    const float y = 15.0f, chipX = 300.0f - 120.0f;
    auto chipText = [&](const char* label, float v, Rgba col) {
      fmtLufs(v, num, sizeof num);
      std::snprintf(buf, sizeof buf, "%s %s", label, num);
      const float tw = 3.6f * static_cast<float>(std::strlen(buf));
      if (x + tw > chipX) return;
      p.textAligned({x, y}, buf, 6.0f, col, 0.0f);
      x += tw + 7.0f;
    };
    if (opts_.on(hS)) chipText("S", last.shortTerm, kShortTerm);
    if (opts_.on(hM)) chipText("M", last.momentary, ui::mulAlpha(kMomentary, 0.9f));
    if (opts_.on(hI)) chipText("I", last.integrated, kIntegrated);
    if (valid(last.integrated)) {
      std::snprintf(buf, sizeof buf, "%+.1f LU", last.integrated - target);
      if (x + 3.6f * static_cast<float>(std::strlen(buf)) <= chipX) p.textAligned({x, y}, buf, 6.0f, stateColour(last.integrated - target), 0.0f);
    }
    // The footer: the window, the target and the short-term range.
    const float fy = plotB + 5.0f;
    if (secs >= 60) std::snprintf(buf, sizeof buf, "%d min", secs / 60);
    else std::snprintf(buf, sizeof buf, "%d s", secs);
    p.textAligned({plotL + 4.0f, fy}, buf, 5.5f, gridLabel, 0.0f);
    std::snprintf(buf, sizeof buf, "target %.1f", target);
    p.textAligned({0.5f * (plotL + plotR), fy}, buf, 5.5f, ui::mulAlpha(kOn, 0.8f), 0.5f);
    if (valid(r_.minShortTerm) && valid(r_.maxShortTerm)) {
      std::snprintf(buf, sizeof buf, "S %.1f ... %.1f", r_.minShortTerm, r_.maxShortTerm);
      p.textAligned({plotR - 4.0f, fy}, buf, 5.5f, gridLabel, 1.0f);
    }
  }

private:
  static void paintTag(ui::FacePainter& p, const char* text, float y, Rgba col, float plotL, float plotR, float plotT) {
    const float tw = 3.3f * static_cast<float>(std::strlen(text)) + 6.0f;
    const float bx = std::max(plotL + 2.0f, plotR - tw - 6.0f);
    const float by = y - 10.0f > plotT ? y - 10.0f : y + 2.0f;
    p.roundedRect({bx, by, bx + tw, by + 8.0f}, 1.5f, kPlate);
    p.textAligned({bx + 3.0f, by + 4.0f}, text, 5.5f, col, 0.0f);
  }
  bool dragCut_ = false;
  float dragBase_ = 0.0f, dragY_ = 0.0f, dragSlope_ = 0.0f;
  std::vector<Vec2> pts_;
};

LoudnessFace* asLoudness(MetaFace& f) { return dynamic_cast<LoudnessFace*>(&f); }

}  // namespace

std::unique_ptr<MetaFace> makeLevelMeterFace(const meta::Type& t) { return std::make_unique<LevelMeterFace>(t); }
std::unique_ptr<MetaFace> makeLufsMeterFace(const meta::Type& t) { return std::make_unique<LufsMeterFace>(t); }
std::unique_ptr<MetaFace> makeLoudnessHistoryFace(const meta::Type& t) { return std::make_unique<LoudnessHistoryFace>(t); }

bool loudnessShown(const MetaFace& f, LoudnessShown& out) {
  const auto* l = dynamic_cast<const LoudnessFace*>(&f);
  if (l == nullptr) return false;
  out.readings = l->shownReadings();
  out.acquired = l->acquiredForTest();
  out.frozen = l->frozenForTest();
  out.points = l->shownPoints();
  out.config = l->configForTest();
  return true;
}

dsp::LoudnessConfig loudnessRequest(std::string_view type, const meta::DataValue* data) {
  const meta::Type* t = meta::findType(type);
  if (t == nullptr) return {};
  std::unique_ptr<MetaFace> f = makeFace(*t);
  LoudnessFace* l = f ? asLoudness(*f) : nullptr;
  return l != nullptr ? l->configFor(data) : dsp::LoudnessConfig{};
}

bool loudnessTestControl(MetaFace& f, std::string_view key, double value) {
  LoudnessFace* l = asLoudness(f);
  if (l == nullptr) return false;
  if (key == "freeze") l->setFrozenForTest(value != 0.0);
  else if (key == "sheet") l->setSheetForTest(value != 0.0);
  else if (key == "clear") l->pressForTest(kClearPart);
  else if (key == "lamps") l->pressForTest(kLampPart);
  else l->setOptionForTest(key, value);
  return true;
}

}  // namespace pq::views
