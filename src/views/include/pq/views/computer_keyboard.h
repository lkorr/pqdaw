#pragma once

// The computer MIDI keyboard (T6.4; docs/plans/M6.md "Decisions taken": Live's letter rows, M toggles it).
//
// While on, the home and upper letter rows play notes into the armed tracks that listen to it (each track in its own
// tuning and note input mode: Keys = degrees of its tuning, Hz = their Hz; ProjectLink builds the tables):
//
//     W E   T Y U   O P                (the upper row: the "black keys" in 12-TET; here simply the odd steps)
//    A S D F G H J K L ;               A = step 0, W = 1, S = 2, E = 3, D = 4, F = 5, T = 6, G = 7, Y = 8, H = 9,
//                                      U = 10, J = 11, K = 12, O = 13, L = 14, P = 15, ; = 16
//
// A step is a DEGREE (consecutive degrees of the track's tuning, not 12-TET keys), above the equave offset: Z / X move it
// down / up one equave (−5 … +5), C / V the velocity by 20 (1 … 127, 100 at start). A key-up ends the note its key-down
// started (same equave and step, whatever Z / X did meanwhile). Turning the keyboard off, or the window losing the
// keyboard focus (releaseAll), ends every held note. Keys with Ctrl, Alt or Super pass through (Ctrl+S still saves), and
// so does Shift; a focused text field still takes its keys (the listener sees presses after the focus chain).
//
// It is the UiTree's key listener (ui::KeyListener; the Shell installs it) and sends TransportPort::liveKey. M8's macro
// mode will replace its scope when active. UI thread only.

#include <cstdint>
#include <functional>
#include <vector>

#include "pq/ui/widget.h"
#include "pq/views/transport_port.h"

namespace pq::views {

class ComputerKeyboard final : public ui::KeyListener {
public:
  static constexpr int32_t kSteps = 17;
  static constexpr int32_t kMinEquave = -5, kMaxEquave = 5;
  static constexpr int32_t kVelocityStep = 20;
  explicit ComputerKeyboard(TransportPort& transport) : transport_(transport) {}

  bool active() const noexcept { return active_; }
  void setActive(bool on);  // off: every held note ends
  int32_t equave() const noexcept { return equave_; }
  int32_t velocity() const noexcept { return velocity_; }
  // The step a key plays (0 … 16), or −1.
  static int32_t stepOf(ui::Key k) noexcept;
  // Every held note ends now (the window lost the keyboard, the keyboard was turned off).
  void releaseAll();
  uint32_t heldKeys() const noexcept { return static_cast<uint32_t>(held_.size()); }

  bool onTreeKey(const ui::KeyEvent& e) override;

  // Called after the state changed (on/off, equave, velocity): the status line names it.
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
  std::vector<Held> held_;
};

}  // namespace pq::views
