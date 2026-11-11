// The computer MIDI keyboard (T6.4, KB.1): pq/views/computer_keyboard.h describes it.
#include "pq/views/computer_keyboard.h"

#include <algorithm>
#include <string_view>

namespace pq::views {

namespace {

// Index of `k` in `row` (a printable key is its unshifted code point), or −1.
int32_t indexIn(std::string_view row, ui::Key k) noexcept {
  if (k == 0 || k > 0x7F) return -1;
  const size_t i = row.find(static_cast<char>(k));
  return i == std::string_view::npos ? -1 : static_cast<int32_t>(i);
}

// Live's piano layout: the semitone each key plays (the white keys A S D F G H J K L ; on 0 2 4 5 7 9 11 12 14 16).
constexpr std::string_view kPiano = "awsedftgyhujkolp;";
// The mos-explorer's rows (music-website mos-explorer index.html KBD_HOME / BOTTOM / UPPER / TOP, KBD_ABOVE / BELOW).
// A space keeps a position whose key does something else here (M: the keyboard toggle).
constexpr std::string_view kHome = "asdfghjkl;'";
constexpr std::string_view kBottom = "zxcvbn ,./";
constexpr std::string_view kUpper = "qwertyuiop[]\\";
constexpr std::string_view kTop = "1234567890-=";
constexpr std::string_view kAbove = "asdfghjkl;'qwertyuiop[]\\1234567890-=";
constexpr std::string_view kBelow = "/., nbvcxz";  // index i = i + 1 degrees below A

int32_t floorDiv(int32_t a, int32_t b) noexcept { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

}  // namespace

std::optional<ComputerKeyboard::KeyNote> ComputerKeyboard::noteOf(ui::Key k, const Layout& layout, ScaleMode mode) noexcept {
  if (k == ' ') return std::nullopt;
  if (layout.piano) {
    if (const int32_t i = indexIn(kPiano, k); i >= 0) return KeyNote{0, i};
    return std::nullopt;
  }
  if (mode == ScaleMode::Continuous) {
    if (const int32_t i = indexIn(kAbove, k); i >= 0) return KeyNote{0, i};
    if (const int32_t i = indexIn(kBelow, k); i >= 0) return KeyNote{0, -(i + 1)};
    return std::nullopt;
  }
  if (const int32_t i = indexIn(kHome, k); i >= 0) return KeyNote{0, i};
  if (const int32_t i = indexIn(kBottom, k); i >= 0) return KeyNote{-1, i};
  if (const int32_t i = indexIn(kUpper, k); i >= 0) return KeyNote{1, i};
  if (const int32_t i = indexIn(kTop, k); i >= 0) return KeyNote{2, i};
  return std::nullopt;
}

std::optional<ComputerKeyboard::KeyNote> ComputerKeyboard::stepOf(const KeyNote& n, int32_t equave, const Layout& layout) noexcept {
  int32_t e = equave + n.equave, d = n.degree;
  if (d < 0 || d >= kSteps) {  // Continuous's keys below A (and any degree past the table): wrapped by the tuning's size
    const int32_t size = std::max(layout.degrees, 1);
    const int32_t q = floorDiv(d, size);
    e += q;
    d -= q * size;
  }
  if (d < 0 || d >= kSteps || e < kMinEquave || e > kMaxEquave) return std::nullopt;
  return KeyNote{e, d};
}

std::string ComputerKeyboard::layoutName(const Layout& layout, ScaleMode mode) {
  if (layout.piano) return "Piano (12 degrees)";
  return std::string(mode == ScaleMode::OneRow ? "1Row" : "Continuous") + " (" + std::to_string(layout.degrees) +
         (layout.degrees == 1 ? " degree)" : " degrees)");
}

std::string ComputerKeyboard::layoutKeys(const Layout& layout, ScaleMode mode) {
  if (layout.piano)
    return "A S D F G H J K L ; play the white degrees (0 2 4 5 7 9 11 12 14 16), W E T Y U O P the black ones; "
           "Z / X equave down / up, C / V velocity -20 / +20.";
  const std::string n = std::to_string(layout.degrees);
  if (mode == ScaleMode::OneRow)
    return "A S D F G H J K L ; ' play degrees 0-10 of the " + n + "; Z X C V B N , . / the same an equave lower, "
           "Q W E R T Y U I O P [ ] \\ an equave higher, 1 2 3 … - = two higher. Page Up / Page Down equave, "
           "Home / End velocity.";
  return "From A up, A … ' then Q … \\ then 1 … = play 35 consecutive degrees (" + n + " per equave); / . , N B V C "
         "X Z the 9 below A. Page Up / Page Down equave, Home / End velocity.";
}

ComputerKeyboard::Layout ComputerKeyboard::layout() const {
  if (!layoutSource) return {};
  Layout l = layoutSource();
  l.degrees = std::max(l.degrees, 1);
  return l;
}

void ComputerKeyboard::setActive(bool on) {
  if (on == active_) return;
  if (!on) releaseAll();
  active_ = on;
  changed();
}

void ComputerKeyboard::setEquave(int32_t e) {
  e = std::clamp(e, kMinEquave, kMaxEquave);
  if (e == equave_) return;
  equave_ = e;
  changed();
}

void ComputerKeyboard::setVelocity(int32_t v) {
  v = std::clamp(v, 1, 127);
  if (v == velocity_) return;
  velocity_ = v;
  changed();
}

void ComputerKeyboard::setScaleMode(ScaleMode m) {
  if (m == mode_) return;
  mode_ = m;
  changed();
}

void ComputerKeyboard::releaseAll() {
  if (held_.empty()) return;
  for (const Held& h : held_) transport_.liveKey(h.equave, h.step, 64.0f, false);
  held_.clear();
  notes();
}

void ComputerKeyboard::changed() {
  if (onChanged) onChanged(*this);
}

void ComputerKeyboard::notes() {
  if (onNotes) onNotes(*this);
}

bool ComputerKeyboard::onTreeKey(const ui::KeyEvent& e) {
  // A release of a key that holds a note: always ours (even after the keyboard was turned off: none is held then).
  if (!e.down) {
    const auto it = std::find_if(held_.begin(), held_.end(), [&](const Held& h) { return h.key == e.key; });
    if (it == held_.end()) return false;
    transport_.liveKey(it->equave, it->step, 64.0f, false);
    held_.erase(it);
    notes();
    return true;
  }
  if (!active_ || (e.mods & (ui::mod::kCtrl | ui::mod::kAlt | ui::mod::kSuper | ui::mod::kShift)) != 0) return false;
  // The shift keys: the navigation cluster in both layouts (Page Up / Down: equave, Home / End: velocity); Z X C V in
  // the piano layout, where they are no notes (Live's).
  int32_t de = 0, dv = 0;
  switch (e.key) {
    case ui::key::kPageDown: de = -1; break;
    case ui::key::kPageUp: de = 1; break;
    case ui::key::kEnd: dv = -1; break;
    case ui::key::kHome: dv = 1; break;
    default: break;
  }
  const bool navigation = de != 0 || dv != 0;
  const Layout lay = navigation ? Layout{} : layout();
  if (!navigation) {
    if (const std::optional<KeyNote> n = noteOf(e.key, lay, mode_)) {
      if (e.repeat || std::any_of(held_.begin(), held_.end(), [&](const Held& h) { return h.key == e.key; }))
        return true;  // auto-repeat: the note is already sounding
      if (const std::optional<KeyNote> s = stepOf(*n, equave_, lay)) {
        held_.push_back({e.key, s->equave, s->degree});
        transport_.liveKey(s->equave, s->degree, static_cast<float>(velocity_), true);
        notes();
      }
      return true;  // out of range: the key is still the keyboard's (no view action)
    }
    if (!lay.piano) return false;
    switch (e.key) {
      case 'z': de = -1; break;
      case 'x': de = 1; break;
      case 'c': dv = -1; break;
      case 'v': dv = 1; break;
      default: return false;
    }
  }
  const int32_t equave = std::clamp(equave_ + de, kMinEquave, kMaxEquave);
  int32_t velocity = velocity_;
  if (dv < 0) velocity = std::max(velocity_ - kVelocityStep, 1);
  if (dv > 0) velocity = std::min(velocity_ == 1 ? kVelocityStep : velocity_ + kVelocityStep, 127);
  if (!e.repeat || equave != equave_ || velocity != velocity_) {
    equave_ = equave;
    velocity_ = velocity;
    changed();
  }
  return true;
}

}  // namespace pq::views
