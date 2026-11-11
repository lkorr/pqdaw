#pragma once

// The computer MIDI keyboard (T6.4; docs/plans/M6.md "Decisions taken": Live's letter rows, M toggles it; KB.1: the
// layout follows the target track's tuning, the control bar's keyboard button).
//
// While on, the letter rows play notes into the armed tracks that listen to it (each track in its own tuning and note
// input mode: Keys = degrees of its tuning, Hz = their Hz; ProjectLink builds the tables). KB.2: a track's tuning here
// is that of its live clip (doc::liveClip): the clip open in the clip view when it is on the track, else its clip under
// the playhead (stopped: the start marker), else the track's own tuning — MIDI keys from a port play in it too. A note
// is (equave, step): degree `step` above equave offset `equave` of each listening track's tuning
// (TransportPort::liveKey). The LAYOUT — which key plays which (equave, step) — follows the target track's (layoutSource:
// ViewContext::keyboardLayout, the auto-armed / selected armed / first armed MIDI track):
//
//   Piano (a tuning of exactly 12 degrees per equave, or a 12-TET key map; Live's layout): the white keys are the
//   degrees the piano roll draws white for a 12-degree tuning (0 2 4 5 7 9 11), the upper row the black ones:
//
//       W E   T Y U   O P             W = 1, E = 3, T = 6, Y = 8, U = 10, O = 13, P = 15
//      A S D F G H J K L ;            A = 0, S = 2, D = 4, F = 5, G = 7, H = 9, J = 11, K = 12, L = 14, ; = 16
//
//   Z / X move the equave offset down / up, C / V the velocity by 20.
//
//   Scale (any other size: a MOS, an EDO, a scale; the mos-explorer's keyboard, music-website mos-explorer
//   `getKeyMapping`), the tuning's degrees in rows:
//     1Row (default): home row A S D F G H J K L ; ' = degrees 0 … 10; the bottom row Z X C V B N , . / the same
//       degrees an equave below; the upper row Q W E R T Y U I O P [ ] \ an equave above; the number row 1 … 0 - =
//       two above.
//     Continuous: A … ' then Q … \ then 1 … = are 35 consecutive degrees from A up; / . , N B V C X Z the 9 below A
//       (/ = one below).
//   M stays the toggle in both modes, so the bottom row's M key plays nothing (1Row: degree 6 an equave down;
//   Continuous: 4 below A).
//   Every letter is a note here: Page Up / Page Down move the equave offset, Home / End the velocity (they work in
//   the piano layout too).
//
// A key-up ends the note its key-down started (same equave and step, whatever the shift keys or the layout did
// meanwhile). Turning the keyboard off, or the window losing the keyboard focus (releaseAll), ends every held note.
// Keys with Ctrl, Alt, Super or Shift pass through (Ctrl+S still saves); a focused text field still takes its keys (the
// tree asks the listener after a focused text field, before any other focused widget: KB.1).
//
// It is the UiTree's key listener (ui::KeyListener; the Shell installs it) and sends TransportPort::liveKey. M8's macro
// mode will replace its scope when active. UI thread only (a few comparisons per key press; the layout is queried on a
// note-key press, cached by the source).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pq/ui/widget.h"
#include "pq/views/transport_port.h"

namespace pq::views {

class ComputerKeyboard final : public ui::KeyListener {
public:
  // The (equave, step) range a note can have (= the engine's keyboard table, engine::kKeyboardSteps / Min / MaxEquave;
  // project_link_input.cpp checks they match).
  static constexpr int32_t kSteps = 36;
  static constexpr int32_t kMinEquave = -3, kMaxEquave = 3;
  static constexpr int32_t kVelocityStep = 20;

  // What the target track's tuning makes of the keys.
  struct Layout {
    bool piano = true;     // Live's piano layout (12 degrees per equave); else the scale layout over `degrees`
    int32_t degrees = 12;  // degrees per equave (≥ 1)
    friend bool operator==(const Layout&, const Layout&) = default;
  };
  enum class ScaleMode : uint8_t { OneRow, Continuous };  // the scale layout's two mos-explorer modes
  // A key's note in a layout, before the equave offset: `degree` may be negative or ≥ the tuning's size.
  struct KeyNote {
    int32_t equave = 0, degree = 0;
    friend bool operator==(const KeyNote&, const KeyNote&) = default;
  };

  explicit ComputerKeyboard(TransportPort& transport) : transport_(transport) {}

  bool active() const noexcept { return active_; }
  void setActive(bool on);  // off: every held note ends
  int32_t equave() const noexcept { return equave_; }
  int32_t velocity() const noexcept { return velocity_; }
  ScaleMode scaleMode() const noexcept { return mode_; }
  void setEquave(int32_t e);    // clamped to kMinEquave … kMaxEquave
  void setVelocity(int32_t v);  // clamped to 1 … 127
  void setScaleMode(ScaleMode m);
  // The layout the next note key uses (layoutSource, else the piano layout).
  Layout layout() const;
  std::function<Layout()> layoutSource;

  // The note a key plays in `layout` (nullopt: not a note key there).
  static std::optional<KeyNote> noteOf(ui::Key k, const Layout& layout, ScaleMode mode) noexcept;
  // The note a key plays at equave offset `equave` as the (equave, step) TransportPort::liveKey takes: a degree outside
  // 0 … kSteps − 1 is wrapped by the layout's size into the equave; nullopt when the result is out of range.
  static std::optional<KeyNote> stepOf(const KeyNote& n, int32_t equave, const Layout& layout) noexcept;
  // "Piano (12 degrees)", "1Row (7 degrees)", …: the control bar's readouts and the status line.
  static std::string layoutName(const Layout& layout, ScaleMode mode);
  // The keys of a layout, for the tooltip.
  static std::string layoutKeys(const Layout& layout, ScaleMode mode);

  // Every held note ends now (the window lost the keyboard, the keyboard was turned off).
  void releaseAll();
  uint32_t heldKeys() const noexcept { return static_cast<uint32_t>(held_.size()); }

  bool onTreeKey(const ui::KeyEvent& e) override;

  // Called after the state changed (on/off, equave, velocity, scale mode): the status line and the control bar.
  std::function<void(const ComputerKeyboard&)> onChanged;

private:
  struct Held {
    ui::Key key;
    int32_t equave, step;
  };
  void changed();
  TransportPort& transport_;
  bool active_ = false;
  int32_t equave_ = 0;
  int32_t velocity_ = 100;
  ScaleMode mode_ = ScaleMode::OneRow;
  std::vector<Held> held_;
};

}  // namespace pq::views
