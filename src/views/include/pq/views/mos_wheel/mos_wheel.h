#pragma once

// MOS wheel popover (T3.14, SPEC §4.2): the region chart (mos_chart.h) and the circle (mos_circle.h) linked in one
// popover, plus an EDO entry, a Scala file pick and temperament presets. It edits the tuning of one target (a clip, a
// track or a main tuning; TuningTarget in view_context.h):
//
//   ┌ Tuning · Clip "Riff"                 EDO [  ]  [Scala…]  [Temperament ▾] ┐
//   │ ┌ region chart ─────────────────────────────┐ ┌ circle ──────────────────┐ │
//   │ │                                           │ │                          │ │
//   │ └───────────────────────────────────────────┘ └──────────────────────────┘ │
//   └ 5L 2s · 696.8¢ · Mode 4 (Ionian)                         [Cancel] [Apply] ┘
//
// While it is open every change is a *draft*: the context's tuning preview (ViewContext::setTuningPreview) shows it
// live in the piano roll (every Live frame of a chart or circle drag), and nothing touches the document. Closing
// commits the draft as ONE command (one undo step) when it differs from the target's tuning: Apply, Enter, or a press
// outside the popover (Live's popovers keep what was set). Cancel and Esc close it without a command (no history
// entry). Esc during a chart or circle drag cancels only the drag (the widgets handle it first).
//
// Commands: a clip → SetClipTuning (a private tuning: only that clip changes), a track → SetTrackTuning (private),
// a main tuning → SetTuningDef. The reference pitch of the target's tuning is kept.
//
// Opened from a TuningChip (the clip view's clip box; T3.10's track headers) or the tuning.openMosWheel action
// (Ctrl+Shift+W, Q15: the clip in the clip view, else the first main tuning). UI thread only.

#include <cstdint>
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

// The target's current tuning (nullptr when the target no longer exists).
const doc::TuningDef* targetTuning(const doc::Project& p, const TuningTarget& target);
// The command that gives `target` the tuning `def` (see the file comment).
doc::Command tuningCommand(const TuningTarget& target, doc::TuningDef def);
// "Clip “Riff”", "Track “Bass”", "Main tuning “12-EDO”".
std::string tuningTargetLabel(const doc::Project& p, const TuningTarget& target);
// One line describing a tuning: "31-EDO", "5L 2s · 696.77¢ · Mode 4", "Scala: <description>".
std::string tuningSummary(const doc::TuningDef& def);
// What Ctrl+Shift+W edits: the clip in the clip view, else the first main tuning.
TuningTarget defaultTuningTarget(const ViewContext& ctx);

class MosWheelPopover : public ui::Popover {
public:
  static constexpr float kWidth = 920.0f, kHeight = 580.0f;  // logical px
  static constexpr float kCircleWidth = 380.0f;

  MosWheelPopover(ViewContext& ctx, const TuningTarget& target);
  ~MosWheelPopover() override;

  MosChart& chart() noexcept { return *chart_; }
  MosCircle& circle() noexcept { return *circle_; }
  const TuningTarget& target() const noexcept { return target_; }
  const doc::TuningDef& original() const noexcept { return original_; }
  const doc::TuningDef& draft() const noexcept { return draft_; }
  bool changed() const noexcept { return !(draft_ == original_); }

  // --- draft (the header controls; tests) ---
  bool chooseEdo(int32_t n);                       // n-EDO of the target's equave (2/1 unless it was another EDO's)
  Status chooseScalaFile(std::string_view pathUtf8);  // reads and parses a .scl file
  void requestScalaFile();                         // asks ViewContext::fileDialogs() (no-op without dialogs)
  bool chooseTemperament(int32_t index);           // tuning::temperaments()[index]: its MOS at the range's centre
  // A MOS by its per-period key and generator (as a click in the chart, then the circle's mode and MODMOS).
  bool chooseMos(tuning::MosKey perPeriodKey, double generatorCents, int32_t modeRotation = 0, std::vector<double> modmos = {});

  // --- closing ---
  void apply();   // commits (if changed) and closes
  void cancel();  // closes without a command

  bool onKey(const ui::KeyEvent& e) override;
  ui::InfoText info() const override;

  // Called by the overlay when the popover closes for any reason (openMosWheel wires it): commits unless cancelled.
  void closed();

private:
  void setDraft(doc::TuningDef def);
  void onChart(const std::optional<MosSelection>& sel, MosGesture g);
  void onCircle(const std::optional<MosSelection>& sel, std::span<const double> modmos, MosGesture g);
  void draftFromCircle();
  void showDraft();  // chart / circle / fields from the draft (no draft change)
  void focusChart();  // zooms the chart to the selected region (± 1.5 region widths)
  void commit();

  ViewContext& ctx_;
  TuningTarget target_;
  doc::TuningDef original_;
  doc::TuningDef draft_;
  MosChart* chart_ = nullptr;
  MosCircle* circle_ = nullptr;
  ui::Label* title_ = nullptr;
  ui::Label* summary_ = nullptr;
  ui::TextEdit* edo_ = nullptr;
  ui::Dropdown* presets_ = nullptr;
  std::vector<int32_t> presetIndex_;  // dropdown item (after the placeholder) → temperaments() index
  bool done_ = false;                 // committed or cancelled
  bool cancelled_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // false once destroyed (late dialog callbacks)
};

// Opens the wheel for `target` in `from`'s overlay layer, below `anchor` (device px), or centred in the window when
// the anchor is empty. Returns the popover (nullptr without an overlay layer). The popover must not outlive `ctx`.
MosWheelPopover* openMosWheel(ui::Widget& from, ViewContext& ctx, const TuningTarget& target, ui::IRect anchor = {});
// The popover currently open in `w`'s tree, if any.
MosWheelPopover* openMosWheelPopover(const ui::Widget& w);

// Installs the tuning.openMosWheel action handler (Ctrl+Shift+W). One line in installDefaultContent.
void installMosWheel(Shell& shell);

// A tuning chip: a colour swatch (the main tuning's colour; none for a private tuning) and the tuning's name. A
// click opens the MOS wheel for its target. The text follows the document and the live preview.
class TuningChip : public ui::Control {
public:
  TuningChip(std::string name, ViewContext& ctx);
  void setTarget(std::optional<TuningTarget> target);
  const std::optional<TuningTarget>& target() const noexcept { return target_; }
  const std::string& text() const;  // what the chip shows

  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  bool onKey(const ui::KeyEvent& e) override;
  void open();

private:
  void refresh() const;
  ViewContext& ctx_;
  std::optional<TuningTarget> target_;
  ViewContext::Subscription docSub_, viewSub_;
  mutable std::string text_;
  mutable ui::Rgba swatch_ = 0;
  mutable bool hasSwatch_ = false;
  mutable bool stale_ = true;
};

}  // namespace pq::views
