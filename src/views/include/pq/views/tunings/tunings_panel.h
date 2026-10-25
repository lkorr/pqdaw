#pragma once

// The Tunings panel (T3.19, SPEC §3.2 "a Tunings panel lists main tunings with their automation lanes"; M3 review H7):
// a column left of the track area (ShellSlot::Tunings; View > Tunings, Create > New Main Tuning) with one card per
// main tuning:
//
//   ┌ Tunings                                    [New] ┐
//   │ ● [Tuning B: 22edo              ]          [×]  │  colour (click: the palette), name (type, Enter), remove
//   │ 22-EDO                                 [Wheel…]  │  the definition; the MOS wheel for this main tuning
//   │   Steps                               [   22  ]  │  EDO: size (structural, committed at the end of the drag /
//   │ • Equave                              [  2/1  ]  │  type-in) and equave (cents drag, or a ratio typed "3/1")
//   │   Reference                        [261.63 Hz]   │  MOS: Generator, Period, Stretch; every kind: Reference
//   │ Default · 2 tracks, 1 clip                       │  who uses it; • = automated (a lane exists)
//   └──────────────────────────────────────────────────┘
//
// Continuous parameters (reference, equave, generator, period, stretch) are store gestures of SetTuningParam: Param
// changes, so dragging them never recompiles (SPEC §3.3), one undo step per drag or type-in. The EDO size and an exact
// ratio equave are SetTuningDef. Right-click a parameter: Show Automation Lane (the lane under the track that holds it,
// else the selected track, else the first track following the tuning) — "the Tunings panel lists main tunings with
// their automation lanes": the lane itself is the arrangement's. Remove reassigns what used the tuning to the first
// other one (ops::removeMainTuning, one undo step); the last tuning cannot be removed.
//
// Cost: the panel re-reads the document only on events that can change a tuning or who uses it (TuningChip's filter:
// Reset, Structural, a tuning touched, a tuning parameter), and while hidden it only marks itself stale. Cards are
// rebuilt only when the list of tunings or a tuning's kind changes; otherwise values are set in place.
//
// UI thread only.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pq/document/model.h"
#include "pq/ui/controls.h"
#include "pq/ui/widget.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

namespace pq::views {

class TuningsPanel;

// "Tuning B", "Tuning C", …: the first "Tuning <letter>" no main tuning is named yet.
std::string nextTuningName(const doc::Project& p);

// One row "Label  [value box]" of a card: a continuous parameter or the EDO size. Paints a red dot before the label
// when the parameter is automated; right-click opens its menu (Show Automation Lane).
class TuningParamRow : public ui::Widget {
public:
  TuningParamRow(std::string name, TuningsPanel& panel, doc::TuningId tuning, std::optional<doc::ParamKind> param,
                 std::string label, ui::ValueSpec spec);
  ui::DragBox& box() const noexcept { return *box_; }
  const std::optional<doc::ParamKind>& param() const noexcept { return param_; }
  void setAutomated(bool on);
  bool automated() const noexcept { return automated_; }
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;

private:
  TuningsPanel& panel_;
  doc::TuningId tuning_;
  std::optional<doc::ParamKind> param_;  // nullopt: the EDO size
  ui::DragBox* box_ = nullptr;
  bool automated_ = false;
};

class TuningCard : public ui::Widget {
public:
  TuningCard(std::string name, TuningsPanel& panel, const doc::TuningSlot& slot);
  doc::TuningId id() const noexcept { return id_; }
  size_t kind() const noexcept { return kind_; }  // TuningDef::system index the rows were built for
  float logicalHeight() const noexcept { return height_; }
  // Values from the document (programmatic: no callbacks; boxes in a gesture and a focused name field keep theirs).
  // `full`: also who uses it and which parameters are automated (skipped for a parameter drag).
  void refresh(const doc::Project& p, const doc::TuningSlot& slot, size_t index, bool full = true);

  ui::TextEdit& nameField() const noexcept { return *name_; }
  ui::Control& swatch() const noexcept { return *swatch_; }
  ui::Button& removeButton() const noexcept { return *remove_; }
  ui::Button& wheelButton() const noexcept { return *wheel_; }
  const std::string& summary() const noexcept { return summary_->text(); }
  const std::string& usage() const noexcept { return usage_->text(); }
  // The row of a parameter (nullopt: the EDO size), or nullptr when this kind has none.
  TuningParamRow* row(std::optional<doc::ParamKind> param) const noexcept;
  void paint(ui::PaintContext& ctx) override;

private:
  friend class TuningsPanel;
  TuningsPanel& panel_;
  doc::TuningId id_;
  size_t kind_ = 0;
  float height_ = 0.0f;
  ui::Control* swatch_ = nullptr;
  ui::TextEdit* name_ = nullptr;
  ui::Button* remove_ = nullptr;
  ui::Label* summary_ = nullptr;
  ui::Button* wheel_ = nullptr;
  ui::Label* usage_ = nullptr;
  std::vector<TuningParamRow*> rows_;
  uint32_t colour_ = 0;
};

class TuningsPanel : public ui::Widget {
public:
  static constexpr float kWidth = 236.0f;  // logical px (the slot's width)

  explicit TuningsPanel(Shell& shell);
  ~TuningsPanel() override;

  ViewContext& context() const noexcept { return ctx_; }
  Shell& shell() const noexcept { return shell_; }

  // Create > New Main Tuning: a 12-EDO main tuning named nextTuningName(), the palette's next colour, one undo step;
  // shows the panel and puts the cursor in the new card's name field (its text selected). Returns its id.
  doc::TuningId addTuning();
  // Removes the tuning (ops::removeMainTuning: its users move to the first other tuning; one undo step).
  bool removeTuning(doc::TuningId id);
  // Shows the automation lane of a tuning parameter in the arrangement (see the file comment). False without one.
  bool showAutomationLane(const doc::ParamRef& p);

  // Scrolls the list so the tuning's card is in view (a new tuning's card is revealed this way).
  void reveal(doc::TuningId id);

  size_t cardCount() const noexcept { return cards_.size(); }
  TuningCard* card(doc::TuningId id) const noexcept;
  TuningCard* cardAt(size_t i) const noexcept { return i < cards_.size() ? cards_[i] : nullptr; }
  // Re-reads the document now (also what the subscriptions do; cheap when nothing changed). `full = false` (a parameter
  // drag): values only, not the usage counts and automation markers.
  void refresh(bool full = true);
  // The same at the next frame (from a value box's own gesture callback, which runs while the box is still in its
  // gesture and is skipped by refresh).
  void refreshLater();
  void onFrame(double now) override;

  void paint(ui::PaintContext& ctx) override;
  ui::InfoText info() const override;

private:
  void rebuild(const doc::Project& p);
  Shell& shell_;
  ViewContext& ctx_;
  ViewContext::Subscription docSub_, viewSub_;
  ui::Widget* list_ = nullptr;  // the ScrollView's content column
  std::vector<TuningCard*> cards_;
  std::vector<std::pair<uint32_t, size_t>> built_;  // (tuning id, kind) per card
  bool stale_ = true;
  bool staleFull_ = true;
  bool refreshPending_ = false;
};

// Fills ShellSlot::Tunings; the panel installs the tuning.newMain view action. One line in installDefaultContent.
TuningsPanel& installTuningsPanel(Shell& shell);

}  // namespace pq::views
