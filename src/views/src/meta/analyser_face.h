#pragma once

// The frame the analyser faces share (VZ.4 loudness, VZ.5 stereo; private to views): the module IS its display — a dark
// body, the display well, SET / CLEAR / FREEZE chips in the top-right corner, the VZ.1 settings sheet (option_sheet.h:
// ANALYSIS / DISPLAY, SAVE DEFAULT / RESET; beside the face where the host has room) with the options as the instance's
// data, the jacks' leads. The analysis: the host binds the instance's feed (Face::setAnalysis); a painted face acquires
// what it draws (wantFlags) and sends its options (sendRequest); the hosts poll it at <= 60 Hz while the feed moves (or the
// face eases); the feed's waker asks again when sound arrives; a face polled but no longer painted lets go after one
// unanswered repaint, so a hidden display costs nothing. A face defines what it reads (readFeed), paints (paintBody) and
// its own parts (ids from kOwnBase).

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "faces.h"
#include "option_sheet.h"

namespace pq::views::analyser {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;
using sheet::Opt;

inline constexpr float kRadius = 10.0f;
inline constexpr float kLabelPx = 6.0f;          // world units
inline constexpr float kLabelMinDevicePx = 5.0f;  // below this the text is left out (the bars and traces stay)
enum Part : int32_t { kSetPart = 1, kClearPart = 2, kFreezePart = 3, kSheetPart = 4, kPlotPart = 5, kOwnBase = 10,
                      kRowBase = sheet::kRowBase };

inline bool inside(const Rect& r, Vec2 p) noexcept { return p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1; }

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

class AnalyserFace : public MetaFace {
public:
  ~AnalyserFace() override { letGo(); }

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
          if (feed_ != nullptr) clearMeasurement(*feed_);
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
    if (part >= kOwnBase && part < sheet::kTabBase) return ownCursor(part);
    if (part == kSetPart || part == kClearPart || part == kFreezePart || (part >= sheet::kTabBase && part <= sheet::kReset))
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
        body = clearText();
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
  bool acquiredForTest() const noexcept { return acquired_; }
  bool frozenForTest() const noexcept { return frozen_; }
  void setOptionForTest(std::string_view key, double v) {
    for (uint32_t i = 0; i < table_.size(); ++i)
      if (key == table_[i].key) setOption(i, v);
  }
  void setFrozenForTest(bool f) { frozen_ = f; }
  void loadForTest(const meta::DataValue* d) { opts_.load(d); }
  void hoverForTest(Vec2 l) { hover_ = l; }
  void setSheetForTest(bool s) { sheet_ = s; }
  void pressForTest(int32_t part) {
    FacePointer e;
    e.kind = FacePointer::Kind::Down;
    if (part == kClearPart && feed_ != nullptr) clearMeasurement(*feed_);
    else (void)ownDown(part, e);
  }

protected:
  AnalyserFace(const meta::Type& t, float w, float h, std::span<const Opt> table)
      : MetaFace(t), w_(w), h_(h), table_(table), opts_(table),
        panel_{{8.0f, 23.0f, w - 8.0f, h - 8.0f}, 6.0f} {}

  // ---- what a face defines ----
  virtual uint32_t wantFlags() const noexcept = 0;                  // what it acquires (devices::kWant*)
  virtual void sendRequest(devices::SpectrumFeed& f) const = 0;     // its analysis options to the feed
  virtual bool readFeed(devices::SpectrumFeed& f, bool& settled) = 0;  // reads what is new; true: it changed
  virtual void feedChanged() {}                                     // another feed: drop what was read
  virtual void clearMeasurement(devices::SpectrumFeed& /*f*/) {}    // CLEAR
  virtual const char* clearText() const { return "The measurement starts again."; }
  virtual void rows(int tab, std::vector<uint32_t>& out) const = 0;
  virtual void paintBody(ui::FacePainter& p, const ui::Theme& th, bool labels) = 0;
  virtual ui::Cursor ownCursor(int32_t /*part*/) const noexcept { return ui::Cursor::Pointer; }
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
    if (feed_ != nullptr) sendRequest(*feed_);  // (the feed ignores an unchanged request)
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
      primed_ = false;
      feedChanged();
      if (feed_ != nullptr) sendRequest(*feed_);
    }
    return feed_ != nullptr;
  }
  void acquire() {
    if (acquired_ || feed_ == nullptr) return;
    sendRequest(*feed_);
    held_ = wantFlags();  // (released by these flags: letGo runs from the destructor, where wantFlags is gone)
    feed_->acquire(held_);
    feed_->addWaker(this, [this] {
      frames_.live_ = true;
      if (wakeHost_) wakeHost_();
    });
    acquired_ = true;
  }
  void letGo() {
    if (!acquired_) return;
    feed_->removeWaker(this);
    feed_->release(held_);
    acquired_ = false;
  }

  // Reads what the feed has new and advances the face's own easing. True: changed.
  bool read(double now) {
    if (feed_ == nullptr) return false;
    bool settled = true;
    const bool fresh = readFeed(*feed_, settled);
    settled_ = settled;
    const float dt = primed_ ? static_cast<float>(std::clamp(now - lastAnim_, 0.0, 0.1)) : 0.0f;
    lastAnim_ = now;
    moving_ = animate(dt, !primed_);
    primed_ = true;
    return fresh || moving_;
  }

  std::function<devices::SpectrumFeed*()> feedOf_;
  double lastPaint_ = 0.0, probe_ = 0.0, lastAnim_ = 0.0;
  bool primed_ = false;
  uint32_t held_ = 0;
  meta::DataPtr data_;
  bool optionsLoaded_ = false, local_ = false, sheet_ = false, hostOver_ = false;
  std::vector<uint32_t> rows_;
  sheet::Sheet panel_;
  bool drag_ = false;
  uint32_t dragOpt_ = 0;
  float pressY_ = 0.0f;
  double pressTravel_ = 0.0;
};

}  // namespace pq::views::analyser
