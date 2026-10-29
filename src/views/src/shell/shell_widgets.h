#pragma once

// The shell's own widgets (private to pq_views): control bar, transport buttons, position display, info view,
// detail view, status bar, slot hosts.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/time.h"
#include "pq/document/model.h"

#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/widget.h"
#include "pq/views/view_context.h"

namespace pq::views::shell {

// Ableton-style transport / switch button with a drawn icon. Acts on press (Live's transport reacts on mouse
// down). Not focusable: Space must keep reaching the global play/stop action after a click.
class TransportButton : public ui::Control {
public:
  enum class Icon : uint8_t { Play, Stop, Record, Loop };
  TransportButton(std::string name, Icon icon);
  std::function<void()> onPress;
  void setOn(bool on);
  bool on() const noexcept { return on_; }

  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override { setPressed(false); }

private:
  Icon icon_;
  bool on_ = false;
};

// Read-only "bar. beat. sixteenth" readout in a field (Live's arrangement position).
class PositionDisplay : public ui::Control {
public:
  explicit PositionDisplay(std::string name);
  void setText(std::string text);
  const std::string& text() const noexcept { return text_; }
  void paint(ui::PaintContext& ctx) override;

private:
  std::string text_;
};

// The control bar's CPU load readout ("CPU 13 %", Live's transport-bar meter; T3.x-fix-look). It reads
// AudioPrefsPort::cpuLoad() at 2 Hz only while the transport plays (one last read when it stops), so a stopped
// session has no timer: idle stays at zero frames. Without an audio port it shows "CPU –".
class CpuMeter : public ui::Control {
public:
  explicit CpuMeter(ViewContext& ctx);
  void paint(ui::PaintContext& ctx) override;
  void onTimer(uint32_t id) override;
  // The transport started or stopped (the control bar forwards view_change::kTransport).
  void transportChanged();
  const std::string& text() const noexcept { return text_; }

private:
  void poll();
  ViewContext& ctx_;
  std::string text_ = "CPU \xE2\x80\x93";
  uint32_t timer_ = 0;
};

// "1. 1. 1": bar, beat, sixteenth (1-based, Live's position display) at tick t.
std::string formatPosition(const TimeSigMap& sigs, Ticks t);
// "4. 0. 0": a duration in bars, beats and sixteenths of the signature at `at`.
std::string formatDuration(const TimeSigMap& sigs, Ticks at, Ticks length);
// Inverses for type-in ("3.2.1", "3.2", "3"; spaces allowed). nullopt when unparsable.
std::optional<Ticks> parsePosition(const TimeSigMap& sigs, std::string_view text);
std::optional<Ticks> parseDuration(const TimeSigMap& sigs, Ticks at, std::string_view text);

// The control bar (main/control-bar). Values come from the document (tempo, time signature and loop: the first
// tempo point / signature, edited through gestures = one undo step each) and the transport (play state; the
// position display reads the playhead only while playing, on a timer due at the next sixteenth).
class ControlBar : public ui::Widget {
public:
  explicit ControlBar(ViewContext& ctx);
  ~ControlBar() override;
  void paint(ui::PaintContext& ctx) override;
  void onTimer(uint32_t id) override;

  // Re-reads everything from the document and the transport (also called by the subscriptions).
  void refreshDocument();
  void refreshTransport();
  const std::string& positionText() const noexcept { return position_->text(); }
  const std::string& cpuText() const noexcept { return cpu_->text(); }

private:
  void schedulePositionTimer(Ticks tick);
  void refreshTuningChip();
  template <class MakeCommand>
  void bindGesture(ui::ValueControl& c, const char* label, MakeCommand make);

  ViewContext& ctx_;
  ViewContext::Subscription docSub_, viewSub_;
  ui::DragBox* tempo_ = nullptr;
  ui::DragBox* sigNum_ = nullptr;
  ui::DragBox* sigDen_ = nullptr;
  PositionDisplay* position_ = nullptr;
  TransportButton* play_ = nullptr;
  TransportButton* stop_ = nullptr;
  TransportButton* record_ = nullptr;
  TransportButton* loop_ = nullptr;
  ui::DragBox* loopStart_ = nullptr;
  ui::DragBox* loopLength_ = nullptr;
  ui::Label* grid_ = nullptr;
  CpuMeter* cpu_ = nullptr;
  ui::Widget* tuningChip_ = nullptr;  // TuningChip (mos_wheel.h) of the selected track, else the first main tuning
  const void* timeline_ = nullptr;  // the timeline node the controls were last read from
  doc::LoopRegion loopShown_{};
  bool loopShownValid_ = false;
  uint32_t timer_ = 0;
};

// The menu bar (T3.19, M3 review H7): Live's row of menus at the top of the window — File, Edit, Create, View, Options,
// Help — built from ui::Menu. Every item runs an action of the context's registry (greyed while the action is disabled
// or not registered, e.g. a view that is not installed) and shows the chord the keymap binds to it. A click on a title
// opens its menu below it; while one is open, moving over another title switches to it and a click on the open title
// closes it (OverlayOptions::onOutsidePointer). Edit's Cut / Copy / Paste / Duplicate / Delete / Select All act on the
// clip view when the focus is in it, else on the arrangement (the focused editor, as Live). Closed menus do not exist;
// the bar repaints only its own rect, on hover and open changes.
class MenuBar : public ui::Widget {
public:
  static constexpr const char* kTitles[] = {"File", "Edit", "Create", "View", "Options", "Help"};
  static constexpr int32_t kMenus = 6;

  explicit MenuBar(ViewContext& ctx);
  ~MenuBar() override;
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onHoverChanged(bool hovered) override;
  // T4.16a: the pointer hand over a title (the hovered title, kept by onPointer: O(1)).
  ui::Cursor cursorAt(ui::Vec2 pos) const override;

  // Opens menu i (0 = File … 5 = Help) below its title, closing any other; returns it (nullptr without an overlay).
  ui::Menu* open(int32_t i);
  void close();
  int32_t openIndex() const noexcept { return open_; }
  ui::Menu* openMenu() const noexcept { return menu_; }
  // Device-px rect of title i (from the last layout).
  ui::IRect titleRect(int32_t i) const;
  // The items menu i shows right now (state-dependent: checks, labels, the focused editor). Tests read them.
  std::vector<ui::MenuItem> items(int32_t i) const;

private:
  int32_t titleAt(ui::Vec2 p) const;
  bool outside(const ui::PointerEvent& e);
  std::string shortcut(std::string_view action) const;
  ui::MenuItem act(std::string label, std::string action) const;

  ViewContext& ctx_;
  int32_t open_ = -1;
  int32_t hover_ = -1;
  ui::Menu* menu_ = nullptr;
  std::shared_ptr<MenuBar*> alive_;
};

// Info view (bottom-left, SPEC §9): title + wrapped body of whatever is under the pointer (infoFor(hovered)), re-read
// when the hovered widget's text changes in place (view_change::kInfo, T3.19: the piano roll's pitch readout).
class InfoView : public ui::Widget, private ui::TreeObserver {
public:
  explicit InfoView(ViewContext& ctx);
  ~InfoView() override;
  void attachTo(ui::UiTree& tree);
  void show(const ui::InfoText& t);
  const std::string& title() const noexcept { return title_; }
  const std::string& body() const noexcept { return body_; }
  void paint(ui::PaintContext& ctx) override;

private:
  void onHoverChanged(ui::Widget* hovered) override;
  ViewContext::Subscription sub_;
  std::string title_, body_;
  // Word-wrap cache: line starts in body_ for (font, width).
  std::vector<uint32_t> lines_;
  int32_t wrapWidth_ = -1;
  ui::FontId wrapFont_ = ui::kNoFont;
  ui::UiTree* observing_ = nullptr;
};

// A slot host (Stack). With no children it paints its placeholder background; once a view is installed it paints
// nothing (the view covers it).
class SlotHost : public ui::Widget {
public:
  enum class Fill : uint8_t { Lane, Panel, None };
  SlotHost(std::string name, Fill fill, std::string hint = {});
  void setHint(std::string hint);
  void paint(ui::PaintContext& ctx) override;

private:
  Fill fill_;
  std::string hint_;
};

// Detail view (bottom): a title strip and the clip / device hosts, one visible at a time.
class DetailView : public ui::Widget {
public:
  explicit DetailView(ViewContext& ctx);
  SlotHost& clip() const noexcept { return *clip_; }
  SlotHost& device() const noexcept { return *device_; }
  void refresh();
  void paint(ui::PaintContext& ctx) override;

private:
  ViewContext& ctx_;
  ViewContext::Subscription sub_;
  SlotHost* clip_ = nullptr;
  SlotHost* device_ = nullptr;
  ui::Label* title_ = nullptr;
};

// Status line (bottom): the context's status text, the project name (with "*" when unsaved) and Live's clip /
// device view selectors.
class StatusBar : public ui::Widget {
public:
  explicit StatusBar(ViewContext& ctx);
  void refresh();
  void paint(ui::PaintContext& ctx) override;

private:
  ViewContext& ctx_;
  ViewContext::Subscription sub_;
  ui::Label* message_ = nullptr;
  ui::Label* project_ = nullptr;
  ui::Toggle* clipSel_ = nullptr;
  ui::Toggle* deviceSel_ = nullptr;
};

}  // namespace pq::views::shell
