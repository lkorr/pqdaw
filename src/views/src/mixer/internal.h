#pragma once

// The mixer's private parts (T3.10): the controller shared by every strip (edits, gestures, labels, meters) and the
// Live-styled controls the strips are made of.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/mos_wheel/mos_wheel.h"

namespace pq::views {

// Shared by every strip (header and mixer view) through a shared_ptr, so strips may outlive the mixer view (the
// arrangement's headers are destroyed whenever the tree goes).
class MixerController {
public:
  explicit MixerController(ViewContext& ctx) : ctx(ctx) {}
  ViewContext& ctx;

  // --- edits (each one undo step; failures go to the status line) ---
  bool apply(const doc::ops::EditResult& e, std::string_view what);
  // A value drag: one store gesture from the first change to the release.
  void beginGesture(std::string_view label);
  void cancelGesture();  // Esc / focus lost: the store gesture is dropped (no history entry)
  void change(const doc::Command& c);
  void endGesture();
  // The activator of `id`: toggles every selected track when `id` is selected (like `0`), else just `id`.
  void activatorClicked(doc::TrackId id);
  // Hooks set by installMixer from the arrangement (null without one).
  std::function<std::vector<doc::TrackId>()> selectedTracks;
  std::function<void(doc::TrackId, ui::Mods)> selectTrack;

  // --- labels ---
  // Live's track number (main tree, depth-first, 1-based) or a return's letter; empty for the master. Cached per
  // snapshot.
  std::string_view activatorLabel(const doc::Snapshot& s, doc::TrackId id);

  // --- tuning chip ---
  void openTuningMenu(ui::Widget& chip, doc::TrackId id);

  // --- output chooser ---
  void openOutputMenu(ui::Widget& chooser, doc::TrackId id);

  // --- meters ---
  void addStrip(MixerStrip* s);
  void removeStrip(MixerStrip* s) noexcept;
  MeterSource* source = nullptr;
  MixerView* view = nullptr;  // owner of the poll timer (null once destroyed)
  // Reads every registered strip's meter in one batch; true while the source is live (poll again).
  bool pollMeters();
  struct Stats {
    uint64_t polls = 0;
    uint64_t metersUpdated = 0;
  } stats;

private:
  bool gesture_ = false;
  doc::Snapshot labelsFor_;
  std::vector<std::pair<uint32_t, std::string>> labels_;  // sorted by raw id
  std::vector<MixerStrip*> strips_;
  std::vector<doc::TrackId> ids_;          // poll scratch
  std::vector<ui::MeterLevels> levels_;    // poll scratch
};

namespace mixer {

// Activator / Solo / Arm / Pre-Post in Live's header style (square, 1 px dark border, flat fills). Remembers the
// modifiers of the press that flipped it (Ctrl = additive solo / arm).
class LiveToggle : public ui::Toggle {
public:
  enum class Kind : uint8_t { Activator, Solo, Arm, Post };
  LiveToggle(std::string name, Kind kind);
  void setLabel(std::string_view s);
  ui::Mods pressMods() const noexcept { return mods_; }
  void paint(ui::PaintContext& ctx) override;
  // T4.17 (Live): a press flips the toggle; dragging on across other tracks' toggles of the same kind (activator, solo,
  // arm) sets each one it crosses to the pressed one's new state (solo and arm additively), one undo step each.
  bool onPointer(const ui::PointerEvent& e) override;

private:
  Kind kind_;
  std::string label_;
  ui::Mods mods_ = 0;
  bool target_ = false;  // the state a drag-across sets
};

// A Live number box: the value as a bar (volume and sends from the left, pan from the centre) under the number.
class LiveBox : public ui::DragBox {
public:
  enum class Kind : uint8_t { Volume, Pan, Send };
  LiveBox(std::string name, Kind kind, std::string letter = {});
  void paint(ui::PaintContext& ctx) override;

private:
  Kind kind_;
  std::string letter_;  // a send's return letter, drawn at the left
};

// A text field with a ▾ that opens a menu built on demand (output routing).
class Chooser : public ui::Control {
public:
  explicit Chooser(std::string name);
  void setText(std::string_view s);
  const std::string& text() const noexcept { return text_; }
  std::function<void(ui::Widget& self)> onOpen;
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;

private:
  std::string text_;
};

// A short name for a tuning definition ("12-EDO", "5L 2s", "Scale").
std::string tuningShortName(const doc::TuningDef& d);
// The output chooser's text for a track.
std::string outputLabel(const doc::Project& p, const doc::Track& t);
// T3.14's tuning chip for a track (a left click opens the MOS wheel for the track), plus a right-click menu that
// assigns the track a main tuning or Inherit (the wheel only commits private tunings).
class TrackTuningChip : public TuningChip {
public:
  TrackTuningChip(ViewContext& ctx, std::shared_ptr<MixerController> ctl) : TuningChip("tuning", ctx), ctl_(std::move(ctl)) {}
  void setTrack(doc::TrackId id) { setTarget(TuningTarget::forTrack(id)); }
  bool onPointer(const ui::PointerEvent& e) override;

private:
  std::shared_ptr<MixerController> ctl_;
};

// The arrangement header's tuning chip (in TrackHeader::chipSection, created on first use; MIDI and group tracks
// only, emptied otherwise).
void bindHeaderChip(ui::Widget& section, const std::shared_ptr<MixerController>& ctl, const doc::Project& p, const doc::Track& t);

}  // namespace mixer

}  // namespace pq::views
