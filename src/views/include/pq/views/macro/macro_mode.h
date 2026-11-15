#pragma once

// Macro mode (T8.1, SPEC §6.2; docs/plans/M7.md "Macro mode un-deferred" M1–M3): interval entry from the computer
// keyboard into the note clip open in the clip view. A native version of the JI trainer's piano-roll mode and the AHK
// interval macro.
//
// State: a CURSOR (a content tick and a pitch), an ANCHOR (the save point: a note of the clip, or a free position on a
// row) and a COMPOSITION (the running product of the interval keys pressed, an exact Rational; empty = 1/1). The
// cursor's pitch is always derived: anchor ∘ composition (∘ rows moved with Up / Down in the tempered layer).
//
// Keys come from a preset (macro_preset.h; default: SPEC §6.2's table):
//   interval keys   multiply the composition (`1`…`0` = 2/1 3/2 5/4 7/4 11/8 13/8 17/16 19/16 23/16 29/16, `q`…`p` the
//                   reciprocals); the cursor moves and is auditioned
//   place (`)       a note at the cursor; the composition is cleared and the cursor returns to the anchor. From a free
//                   position with nothing composed the new note becomes the anchor.
//   placeChain (Tab) the same, and the new note becomes the anchor
//   back (Backspace) clears the composition; with nothing composed deletes the note at the cursor
//   left / right (a / f)  the cursor one grid step earlier / later
//   Shift + left / right (Shift+A / Shift+F, and Shift+Left / Shift+Right)  the next note one grid step shorter /
//                         longer (bug-ji-input; held: auto-repeat). The length is kept for the session (every clip, on
//                         and off) until changed again; never shorter than one grid step (with the grid off: a 16th).
//                         Before the first change a note is as long as the anchor note, else one grid step.
//   up / down (s / d)     one display step: a row in the tempered layer, the next harmonic where the note would be just
//   undo / redo (- / =)   macro-level: the last macro step, with its document edit when the document is still as that
//                         step left it (a ring of kUndoRing steps)
//   layer (g)       Tempered ↔ JI
//   lock (b)        `b` + digits (Enter or the next macro key commits; no digits: the last index) = "Is Harmonic N" on
//                   the anchor note, whatever it is: a note outside every group gets a group, a group's anchor its new
//                   index, a MEMBER becomes its group's anchor with index N (ops::reanchorJiGroup; every note keeps its
//                   pitch). `b` `0` `Enter` removes the anchor note's group (its notes keep their pitch). A key macro
//                   mode does not bind cancels the number and goes on as usual (T7.16b, review H7 / L10).
// Only keys the preset binds are taken, and only without Ctrl / Alt / Super / Shift (Shift: only the length keys above):
// everything else passes through (Ctrl+S saves, Space plays; in macro mode Shift+Left / Right are the next note's
// length, not the selected notes'). Keys are taken only while the clip view (the note area or anything in it) has the
// keyboard focus or the pointer (T7.16b, review M11): with the focus in the arrangement, the browser, a text field, an
// open popup or the help panel and the pointer elsewhere they pass through, and the lamp says "MACRO (click the
// notes)". While a pointer drag is in progress (a widget holds the pointer, or the document is in a gesture) macro keys
// are refused with a status, so nothing joins the drag's undo entry (review M19). Without a note clip open in the clip
// view nothing is taken.
//
// Layers (M1):
//   Tempered (default)  the placed note is a Degree note on the tuning's step for the composed interval measured from
//                       the anchor: composed exactly, mapped once — an EDO by the preset's val (default: the patent val;
//                       3/2 in 12-EDO = 7 steps, 15/8 = 11 steps), a MOS / scale to the nearest degree. The tempering
//                       error (composed − step) is shown.
//   JI                  the placed note is an exact Ratio to the anchor.
//   In EITHER layer, when the anchor is a JI note (macro_doc.h: a Ratio note, or the note declared "harmonic N") the
//   placed note is just: a Ratio in that note's group, the composed ratio from it.
// The anchor follows the pointer and the selection (M2): with nothing composed, moving the pointer onto a note (else
// selecting a single note) makes it the anchor and puts the cursor at its start. Only a real pointer move counts
// (T7.16b, review H8): a note that a scroll (the cursor's auto-scroll, the wheel) slides under a resting pointer is not
// adopted, so keyboard cursor moves win until the mouse moves. A click on empty space puts a free cursor there (grid
// cell, row).
//
// It is the UiTree's key listener while on (the Shell swaps it with the computer MIDI keyboard: turning one on turns
// the other off) and a TreeObserver for the click. The clip view owns the overlay (macro_overlay.h) and tells it when
// the roll changed. UI thread only. Off: one branch per key press, nothing per frame.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/model.h"
#include "pq/tuning/fewest_keypress.h"
#include "pq/tuning/pitch_id.h"
#include "pq/tuning/rational.h"
#include "pq/ui/widget.h"
#include "pq/views/macro/macro_preset.h"

namespace pq::views {

class ViewContext;
namespace roll {
class RollState;
}

class MacroMode final : public ui::KeyListener, public ui::TreeObserver {
public:
  enum class Layer : uint8_t { Tempered, Ji };
  static constexpr size_t kUndoRing = 128;

  // Where the next note lands.
  struct Target {
    bool ok = false;        // false: the anchor or the product cannot be resolved (nothing is placed)
    tuning::PitchId pitch;  // the identity the placed note gets
    double cents = 0.0;     // the cursor's pitch from the clip's reference
    bool just = false;      // an exact ratio (a Ratio note; from a free position off the reference: a Cents note)
    int64_t steps = 0;      // tempered: the steps from the anchor's row (rows moved with Up / Down included)
    double errorCents = 0.0;  // tempered: the composed interval minus the interval of the steps it mapped to
    uint64_t harmonic = 0;  // just, in a group with a fundamental: the cursor's harmonic number (0: none / not whole)
  };

  explicit MacroMode(ViewContext& ctx);
  ~MacroMode() override;

  // --- switch ---
  bool on() const noexcept { return on_; }
  void setOn(bool on);
  void toggle() { setOn(!on_); }
  // On, and a note clip is open in the clip view (the overlays show).
  bool active() const noexcept;
  // Active, and the clip view has the keyboard focus or the pointer, no text field is focused and no popup is open:
  // macro keys are taken (review M11).
  bool takingKeys() const;
  Layer layer() const noexcept { return layer_; }
  void setLayer(Layer l);

  // --- state (the overlay, the lamp, tests) ---
  Ticks tick() const noexcept { return s_.tick; }
  doc::NoteId anchorNote() const noexcept { return s_.anchor; }
  int64_t anchorDegree() const noexcept { return s_.anchorDegree; }
  const tuning::Rational& composition() const noexcept { return s_.comp; }
  bool composing() const noexcept { return !s_.factors.empty() || s_.rowOffset != 0; }
  Target target() const;
  // The anchor's pitch in cents from the reference and the length a placed note gets (placed, auditioned, the cursor's
  // box): the length set with Shift + left / right, else the anchor note's, else the grid step at the cursor.
  std::optional<double> anchorCents() const;
  Ticks noteLength() const;
  // The length set with Shift + left / right (0: none set yet). Kept for the session.
  Ticks setLength() const noexcept { return length_; }
  // Whether the anchor is a note declared "harmonic N" or a member of its group (Up / Down then step its harmonics).
  bool locked() const;
  bool lockPending() const noexcept { return lockPending_; }
  // "3/2 · 5/4 = 15/8" (nothing composed: "1/1").
  std::string compositionText() const;
  // "→ 11 steps, −11.7 ¢" (tempered), "just · harmonic 6" / "just" (a Ratio), "exact" (a Cents note); while `b` waits
  // for its number: "Is Harmonic 15_".
  std::string detailText() const;
  // "MACRO" (off, or no clip), "MACRO · JI", "MACRO · 12-EDO"; on but not taking keys: "MACRO (click the notes)".
  std::string lampText() const;

  // --- presets ---
  const MacroPreset& preset() const noexcept { return preset_; }
  // Loads assets/macro/<id>.json. On an error the current preset stays and the status line says why.
  Status usePreset(std::string_view id);
  void setPreset(MacroPreset p);

  // --- fewest-keypress labels (the trainer's BFS; EDOs of the octave only) ---
  // The label of the row `steps` steps from the anchor's row: the keys to press ("23"; presets with longer key names:
  // the ratio "15/8"). Empty: none (not an EDO, out of ±2 equaves, unreachable in 6 presses, the anchor's own row).
  // The table is built once per (EDO, preset) and kept (kKeypressCache tables).
  std::string_view keypressLabel(int64_t steps);
  const tuning::FewestKeypressMap* keypressMap();
  uint64_t keypressBuilds() const noexcept { return keypressBuilds_; }
  double lastKeypressBuildMs() const noexcept { return keypressBuildMs_; }
  static constexpr size_t kKeypressCache = 8;

  // --- the roll (ClipView) ---
  void attachRoll(roll::RollState* roll);
  void detachRoll(roll::RollState* roll);
  // The document, the clip shown or the view changed: the anchor is validated (a deleted anchor note leaves a free
  // cursor on its row) and the pointer / selection anchor rule is applied.
  void rollChanged();
  // The pointer / selection rule alone (the pointer moved over the roll). Returns whether the state changed.
  bool sync();
  // Whether sync() would change the state (the overlay asks from paint and syncs on the next frame).
  bool syncPending() const;
  // The absolute degree of the anchor's row (the fewest-keypress labels count from it).
  int64_t anchorRow() const;

  // The clip view's overlay: asked for a frame after a click, when the focus may have moved (the lamp re-reads
  // takingKeys() then). nullptr: none.
  void setFrameWidget(ui::Widget* w) noexcept { frameWidget_ = w; }

  // ui::KeyListener / ui::TreeObserver
  bool onTreeKey(const ui::KeyEvent& e) override;
  void onPointerDown(ui::Vec2 pos) override;
  void onHoverChanged(ui::Widget* hovered) override;

  // Called after anything the lamp, the overlay or the key listener choice shows changed.
  std::function<void()> onChanged;

private:
  struct State {
    Ticks tick = 0;
    doc::NoteId anchor;        // invalid: a free position
    int64_t anchorDegree = 0;  // the free position's row; for a note: its (nearest) row, where the cursor stays if it goes
    std::vector<tuning::Rational> factors;
    tuning::Rational comp{1};
    int32_t rowOffset = 0;     // tempered: rows moved with Up / Down since the composition began
    friend bool operator==(const State&, const State&) = default;
  };
  struct Step {
    State before, after;
    bool edit = false;
    doc::Snapshot docBefore, docAfter;
  };
  struct Anchor;  // macro_mode.cpp: the resolved anchor
  struct KeypressTable {
    uint32_t edo = 0;
    uint64_t presetSerial = 0;
    std::optional<tuning::FewestKeypressMap> map;
    std::vector<std::string> labels;  // index: step + map.maxRange()
  };

  Anchor resolve(const State& s) const;
  Target targetOf(const State& s) const;
  bool applySync(State& s, uint32_t& seenHover, uint32_t& seenSel, ui::Vec2& seenPointer) const;
  uint32_t hoveredNote() const;
  uint32_t selectedNote() const;
  void adopt(State& s, const doc::Note& n) const;
  bool blocked() const;  // a text field has the focus, or a popup is open
  bool inClipView(const ui::Widget* w) const noexcept;
  bool dragging() const;  // a widget holds the pointer, or the document is in a gesture
  void checkTaking();     // takingKeys() flipped: changed()
  void markSeen();        // what is under the pointer and selected now is no new anchor

  template <class F>
  void step(F&& f);
  void run(const MacroBinding& b, bool repeat);
  void pressInterval(const tuning::Rational& r);
  void moveTime(int dir);
  void changeLength(int dir);
  int lengthKey(const ui::KeyEvent& e) const;
  void movePitch(int dir);
  void place(bool chain);
  void back();
  void undo();
  void redo();
  void pressLock();
  void commitLock();
  bool dispatch(const doc::ops::EditResult& e, std::string_view what);
  void afterMove(bool audition);
  void reveal();
  void changed();
  void resetState();

  ViewContext& ctx_;
  roll::RollState* roll_ = nullptr;
  bool on_ = false;
  Layer layer_ = Layer::Tempered;
  State s_;
  Ticks length_ = 0;  // bug-ji-input: the next note's length set with Shift + left / right (0: the default)
  uint32_t seenHover_ = 0, seenSel_ = 0;
  ui::Vec2 seenPointer_{-1e9f, -1e9f};  // the pointer where the anchor rule last looked (a real move changes it)
  bool taking_ = false;                 // takingKeys() when the lamp was last told
  bool keyboardWasOn_ = false;          // the computer MIDI keyboard was on when macro mode took over (review L14)
  ui::Widget* frameWidget_ = nullptr;
  bool inStep_ = false;
  bool lockPending_ = false;
  std::string lockDigits_;
  uint32_t lastIndex_ = 1;
  std::vector<Step> ring_;
  size_t ringPos_ = 0;
  doc::ClipRef ringClip_{};
  MacroPreset preset_;
  uint64_t presetSerial_ = 1;
  bool presetLoaded_ = false;  // the preferences' preset was read from its file (first use, or a change)
  std::vector<KeypressTable> keypress_;
  uint64_t keypressBuilds_ = 0;
  double keypressBuildMs_ = 0.0;
};

}  // namespace pq::views
