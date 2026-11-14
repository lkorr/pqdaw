#pragma once

// MOS wheel popover (T3.14, SPEC §4.2; TU.1): the region chart (mos_chart.h) and the circle (mos_circle.h) linked in
// one popover, plus an EDO entry, a Scala file pick and a temperament search. It edits the named tuning one target
// resolves to (a clip, a track, a main tuning or the project default; TuningTarget in view_context.h; TU.3: every
// clip and track linked to that tuning changes with it):
//
//   ┌ Tuning · Clip "Riff"                 EDO [  ]  [Scala…]  [⌕ Temperament   ] ┐
//   │ ┌ region chart ─────────────────────────────┐ ┌ circle ──────────────────┐ │
//   │ │                                           │ │                          │ │
//   │ └───────────────────────────────────────────┘ └──────────────────────────┘ │
//   └ 5L 2s · 696.8¢ · Mode 4 (Ionian)                           [Revert] [Done] ┘
//
// TU.1: every change is applied to the DOCUMENT as it happens (TuningEdit below), so notes that are sounding retune
// during a drag, not when the popover closes: a chart or circle drag is one store gesture (one undo step per drag, Esc
// during it restores where it started), a click, a typed EDO, a Scala file or a temperament one command. Revert puts
// back the tuning the target had when the popover opened (one more undo step); Done, Enter, Esc or a press outside
// close it and keep what was set.
//
// Commands (TU.3: the same for every target, on targetTuningId): SetTuningParam for a generator-only change (a param
// write: the tuning node retunes every note that follows it with no recompile, automation included) and SetTuningDef
// otherwise. The reference pitch is kept. To give one clip or track a tuning of its own, pick "New Tuning" in its chip's
// menu first (a copy, assigned to it), then edit.
//
// The temperament search (TU.1, replaces T3.14's 80-item dropdown): type part of a name and press Enter; one match
// (or an exact name) is chosen at once, several open a short menu of them. The chart's temperament rows show them too.
//
// TU.2: the notes that sound in the target's tuning (playback, the computer keyboard, the piano roll's auditions, a click
// on the tonic) light their degree on the circle (src/mos_wheel/circle_notes.h; MosCircle::noteOn).
//
// Opened from a TuningChip (the clip view's clip box; T3.10's headers) or the tuning.openMosWheel action
// (Ctrl+Shift+W, Q15: the clip in the clip view, else the project's default tuning). UI thread only.
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/commands.h"
#include "pq/document/model.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/views/mos_wheel/mos_chart.h"
#include "pq/views/mos_wheel/mos_circle.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

namespace pq::views {

class CircleNotes;  // TU.2: what lights the circle (src/mos_wheel/circle_notes.h)

// The target's current tuning (nullptr when the target no longer exists).
const doc::TuningDef* targetTuning(const doc::Project& p, const TuningTarget& target);
// TU.3: the named tuning the target resolves to (a clip / track: its chain clip → track → groups → project default; a
// main tuning: itself; the project: its default). Invalid when the target no longer exists.
doc::TuningId targetTuningId(const doc::Project& p, const TuningTarget& target);
// The command that gives the target's named tuning the definition `def` (SetTuningDef on targetTuningId).
doc::Command tuningCommand(const doc::Project& p, const TuningTarget& target, doc::TuningDef def);
// "Clip “Riff” · 12-EDO", "Track “Bass” · Tuning B", "Main tuning “12-EDO”", "Project default · 12-EDO": the target and
// (TU.3) the named tuning an edit changes.
std::string tuningTargetLabel(const doc::Project& p, const TuningTarget& target);
// One line describing a tuning: "31-EDO", "5L 2s · 696.77¢ · Mode 4", "Scala: <description>".
std::string tuningSummary(const doc::TuningDef& def);
// The shortest label of a tuning, for narrow chips: "12" (12-EDO), "5L 2s", "Scala" (T3.x-fix-look).
std::string compactTuningLabel(const doc::TuningDef& def);
// What Ctrl+Shift+W edits: the clip in the clip view, else the project's default tuning.
TuningTarget defaultTuningTarget(const ViewContext& ctx);

// TU.3: what a target CHOOSES. Gives `target` the named tuning `id` (invalid: Inherit) as one undo step: SetClipTuning /
// SetTrackTuning, the project's SetDefaultTuning (Inherit is not one of its choices); a main tuning chooses nothing.
Status assignTuning(ViewContext& ctx, const TuningTarget& target, doc::TuningId id);
// "New Tuning": adds the next "Tuning <letter>" (nextTuningName, the palette's next colour) as a copy of the target's
// current tuning and assigns it to the target (a main-tuning target: a copy only), one undo step. Invalid on failure
// (the reason on the status line). `from` supplies the theme's palette (any widget of the tree).
doc::TuningId newTuningFor(const ui::Widget& from, ViewContext& ctx, const TuningTarget& target);
// The menu of a tuning chip (TU.3): Inherit (a clip / track), the project's tunings (each with its colour dot, name and
// summary on the right; checked: the target's assignment), New Tuning; then MOS Wheel…, Save to Library and Hot-Swap
// Tuning…. A main-tuning target lists Set as Project Default instead of the tunings. `projectDefault` adds a "Project
// Default" submenu of the tunings (the control bar's chip).
std::vector<ui::MenuItem> tuningMenuItems(ui::Widget& from, ViewContext& ctx, const TuningTarget& target, bool projectDefault = false);
void openTuningMenu(ui::Widget& from, ViewContext& ctx, const TuningTarget& target, ui::Vec2 at, bool projectDefault = false);

// TU.1: applies one target's tuning edits to the document while they happen (see the file comment). A Live step of a
// drag opens a store gesture on the first one and dispatches the step inside it; Commit dispatches the last value and
// commits the gesture (one undo step; without a gesture it is a single command); Cancel cancels the gesture (the
// document as before the drag). For a main tuning a change of the MOS generator alone (the mode carried as
// SetTuningParam carries it; a region hint that only matters at an exact region boundary is left as it was) is a
// SetTuningParam: a param write, no compile. UI thread only.
class TuningEdit {
public:
  TuningEdit(ViewContext& ctx, TuningTarget target) : ctx_(ctx), target_(target) {}
  ~TuningEdit() { finish(); }
  TuningEdit(const TuningEdit&) = delete;
  TuningEdit& operator=(const TuningEdit&) = delete;

  // Gives the target `def`. Errors: the target is gone, or the document refused the command (also on the status line).
  Status apply(const doc::TuningDef& def, MosGesture g, std::string_view label = "Change Tuning");
  void cancel();   // Cancel of an open drag (no-op without one)
  void finish();   // commits an open drag (a capture lost, the popover closed)
  bool dragging() const noexcept { return gesture_; }
  const TuningTarget& target() const noexcept { return target_; }
  // The command apply() dispatches for `def` while the target's tuning is `current`.
  doc::Command command(const doc::TuningDef& current, const doc::TuningDef& def) const;

private:
  ViewContext& ctx_;
  TuningTarget target_;
  bool gesture_ = false;  // this edit's store gesture is open
};

// BR.tuning-outside: what the wheel's controls do, shared by the popover and the tuning block's wheel (project patch).
// Zooms `chart` to its selected region (± 1.5 region widths; no-op without one).
void focusChartSelection(MosChart& chart);
// tuning::temperaments()[index]: the chart and circle take its period and its MOS at the middle of its generator range
// (the circle's MODMOS cleared). False (both configs as before, the reason on the status line): out of range, or no MOS
// of it within the chart's sizes. The caller applies the circle's params.
bool selectTemperament(ViewContext& ctx, MosChart& chart, MosCircle& circle, int32_t index);
// The temperament search over `text` (MosWheelPopover::matchTemperaments): the only / exact match is chosen at once,
// several open a menu of them under `field` (each item calls `choose`, which must guard its own lifetime). False: none.
bool findTemperament(ui::Widget& field, std::string_view text, std::function<void(int32_t)> choose);
// A Scala file (.scl, UTF-8 path) read and parsed as a tuning at `referenceHz`; the reason on the status line on failure.
Result<doc::TuningDef> readScalaTuning(ViewContext& ctx, std::string_view pathUtf8, double referenceHz);

class MosWheelPopover : public ui::Popover {
public:
  static constexpr float kWidth = 920.0f, kHeight = 580.0f;  // logical px
  static constexpr float kCircleWidth = 380.0f;

  MosWheelPopover(ViewContext& ctx, const TuningTarget& target);
  ~MosWheelPopover() override;

  MosChart& chart() noexcept { return *chart_; }
  MosCircle& circle() noexcept { return *circle_; }
  const TuningTarget& target() const noexcept { return target_; }
  const doc::TuningDef& original() const noexcept { return original_; }  // the target's tuning when it opened
  const doc::TuningDef& draft() const noexcept { return draft_; }        // what the wheel shows (last applied)
  bool changed() const noexcept { return !(draft_ == original_); }

  // --- edits (the header controls; tests): each is applied to the document at once, one undo step ---
  bool chooseEdo(int32_t n);                       // n-EDO of the target's equave (2/1 unless it was another EDO's)
  Status chooseScalaFile(std::string_view pathUtf8);  // reads and parses a .scl file
  void requestScalaFile();                         // asks ViewContext::fileDialogs() (no-op without dialogs)
  bool chooseTemperament(int32_t index);           // tuning::temperaments()[index]: its MOS at the range's centre
  // The temperament search: the enabled temperaments whose name contains `text` (case-insensitive), an exact name
  // first, at most `limit`; findTemperament chooses the only / exact match, else opens the menu of them (false: none).
  static std::vector<int32_t> matchTemperaments(std::string_view text, size_t limit = 12);
  bool findTemperament(std::string_view text);
  // A MOS by its per-period key and generator (as a click in the chart, then the circle's mode and MODMOS).
  bool chooseMos(tuning::MosKey perPeriodKey, double generatorCents, int32_t modeRotation = 0, std::vector<double> modmos = {});
  void revert();  // the tuning the target had when the wheel opened (one undo step; no-op when unchanged)

  // --- closing (what was set stays) ---
  void apply();   // closes (Done)
  void cancel();  // closes too (kept for the shortcut handler; there is nothing left to discard)

  bool onKey(const ui::KeyEvent& e) override;
  // Dragging the title band moves the popover (T3.x-fix-look, the M3 review's UI §7: at 920 × 580 it covered the very
  // piano roll whose morph it shows). The position is remembered for the next wheel in this session.
  bool onPointer(const ui::PointerEvent& e) override;
  // T4.16a: the move cursor over the title band (what a press there does) and during the move.
  ui::Cursor cursorAt(ui::Vec2 pos) const override;
  ui::InfoText info() const override;
  void onFrame(double now) override;  // TU.2: follows the playhead for the circle's lit notes while playing
  // Where the next wheel opens (logical px from the window's top-left; none: centred / under its chip).
  static std::optional<ui::Vec2> rememberedPosition() noexcept;
  static void forgetPosition() noexcept;

  // Called by openMosWheel once the popover is in the tree: starts what lights the circle (CircleNotes). Not in the
  // constructor: a frame request before the popover is attached is dropped, so a wheel opened during playback (or
  // over held notes) never started following them.
  void opened();
  // Called by the overlay when the popover closes for any reason (openMosWheel wires it): ends an open drag.
  void closed();

private:
  bool setDraft(doc::TuningDef def, MosGesture g);  // applies it to the document (false: refused)
  void onChart(const std::optional<MosSelection>& sel, MosGesture g);
  void onCircle(const std::optional<MosSelection>& sel, std::span<const double> modmos, MosGesture g);
  bool draftFromCircle(MosGesture g);
  void showDraft();  // chart / circle / fields from the draft (no document change)
  void focusChart();  // zooms the chart to the selected region (± 1.5 region widths)
  void syncFooter();  // summary text, Revert enabled

  ViewContext& ctx_;
  TuningTarget target_;
  doc::TuningDef original_;
  doc::TuningDef draft_;
  MosChart* chart_ = nullptr;
  MosCircle* circle_ = nullptr;
  ui::Label* title_ = nullptr;
  ui::Label* summary_ = nullptr;
  ui::TextEdit* edo_ = nullptr;
  ui::TextEdit* search_ = nullptr;   // the temperament search
  ui::Button* revert_ = nullptr;
  std::unique_ptr<TuningEdit> edit_;
  std::unique_ptr<CircleNotes> notes_;  // TU.2
  bool moving_ = false;               // a title drag in progress
  ui::Vec2 moveGrab_{};               // pointer − top-left at the press (device px)
  bool done_ = false;                 // closed
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // false once destroyed (late dialog callbacks)
};

// Opens the wheel for `target` in `from`'s overlay layer, below `anchor` (device px), or centred in the window when
// the anchor is empty. Returns the popover (nullptr without an overlay layer). The popover must not outlive `ctx`.
MosWheelPopover* openMosWheel(ui::Widget& from, ViewContext& ctx, const TuningTarget& target, ui::IRect anchor = {});
// The popover currently open in `w`'s tree, if any.
MosWheelPopover* openMosWheelPopover(const ui::Widget& w);

// Installs the tuning.openMosWheel action handler (Ctrl+Shift+W). One line in installDefaultContent.
void installMosWheel(Shell& shell);

// A tuning chip: a colour swatch and the name of the named tuning its target resolves to (TU.3; followed by its compact
// label when the name does not contain it, for an EDO / MOS: "Tuning B · 5L 2s"). A click opens the MOS
// wheel for its target (editing that shared tuning); a right-click opens tuningMenuItems (choose a tuning, Inherit, New
// Tuning; T5.11: Save to Library and Hot-Swap Tuning…, the browser lists the library's tunings and selecting one gives
// the target's tuning that definition). The text follows the document and the live preview.
class TuningChip : public ui::Control {
public:
  TuningChip(std::string name, ViewContext& ctx);
  void setTarget(std::optional<TuningTarget> target);
  const std::optional<TuningTarget>& target() const noexcept { return target_; }
  const std::string& text() const;  // what the chip shows

  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  bool onKey(const ui::KeyEvent& e) override;
  // T4.16a: the pointer hand while a click opens the wheel (a target is set).
  ui::Cursor cursorAt(ui::Vec2 pos) const override { return target_ ? ui::Cursor::Pointer : Control::cursorAt(pos); }
  void open();
  void openMenu(ui::Vec2 at);  // the right-click menu (tuningMenuItems)
  // The control bar's chip: its menu also sets the project's default tuning (a "Project Default" submenu).
  void setOffersProjectDefault(bool on) noexcept { projectDefault_ = on; }

  // Whether a document event can change what a chip shows (a tuning, an assignment, the tree: Structural; tuning
  // params). Param / Sequence / Automation / Timeline / Meta events of tracks cannot.
  static bool tuningMayChange(const doc::StoreEvent& e) noexcept;

private:
  void refresh() const;
  void recheck();  // re-resolves now and repaints only if the text or swatch changed
  ViewContext& ctx_;
  std::optional<TuningTarget> target_;
  ViewContext::Subscription docSub_;
  mutable std::string text_;
  mutable std::string compact_;  // shown when text_ does not fit (compactTuningLabel)
  mutable ui::Rgba swatch_ = 0;
  mutable bool hasSwatch_ = false;
  mutable bool stale_ = true;
  bool projectDefault_ = false;
};

}  // namespace pq::views
