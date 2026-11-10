// The computer MIDI keyboard (T6.4): pq/views/computer_keyboard.h describes it.
#include "pq/views/computer_keyboard.h"

#include <algorithm>

namespace pq::views {

int32_t ComputerKeyboard::stepOf(ui::Key k) noexcept {
  static constexpr char kKeys[kSteps + 1] = "awsedftgyhujkolp;";
  for (int32_t i = 0; i < kSteps; ++i)
    if (k == static_cast<ui::Key>(kKeys[i])) return i;
  return -1;
}

void ComputerKeyboard::setActive(bool on) {
  if (on == active_) return;
  if (!on) releaseAll();
  active_ = on;
  changed();
}

void ComputerKeyboard::releaseAll() {
  for (const Held& h : held_) transport_.liveKey(h.equave, h.step, 64.0f, false);
  held_.clear();
}

void ComputerKeyboard::changed() {
  if (onChanged) onChanged(*this);
}

bool ComputerKeyboard::onTreeKey(const ui::KeyEvent& e) {
  // A release of a key that holds a note: always ours (even after the keyboard was turned off: none is held then).
  if (!e.down) {
    const auto it = std::find_if(held_.begin(), held_.end(), [&](const Held& h) { return h.key == e.key; });
    if (it == held_.end()) return false;
    transport_.liveKey(it->equave, it->step, 64.0f, false);
    held_.erase(it);
    return true;
  }
  if (!active_ || (e.mods & (ui::mod::kCtrl | ui::mod::kAlt | ui::mod::kSuper | ui::mod::kShift)) != 0) return false;
  if (const int32_t step = stepOf(e.key); step >= 0) {
    if (e.repeat || std::any_of(held_.begin(), held_.end(), [&](const Held& h) { return h.key == e.key; }))
      return true;  // auto-repeat: the note is already sounding
    held_.push_back({e.key, equave_, step});
    transport_.liveKey(equave_, step, static_cast<float>(velocity_), true);
    return true;
  }
  int32_t equave = equave_, velocity = velocity_;
  switch (e.key) {
    case 'z': equave = std::max(equave_ - 1, kMinEquave); break;
    case 'x': equave = std::min(equave_ + 1, kMaxEquave); break;
    case 'c': velocity = std::max(velocity_ - kVelocityStep, 1); break;
    case 'v': velocity = std::min(velocity_ == 1 ? kVelocityStep : velocity_ + kVelocityStep, 127); break;
    default: return false;
  }
  if (!e.repeat || equave != equave_ || velocity != velocity_) {
    equave_ = equave;
    velocity_ = velocity;
    changed();
  }
  return true;
}

}  // namespace pq::views
