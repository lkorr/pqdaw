#pragma once

// Input model of the widget toolkit, independent of SDL (T3.1). The SDL adapter (translateEvent in
// pq/ui/window.h) converts SDL events into these; tests synthesise them directly (pq/ui/test_driver.h).
//
// Coordinates are device pixels of the render target (the same space as widget rects and the draw list).
//
// Keys follow the USB HID / SDL3 conventions so the adapter is a plain copy:
//   - Scancode: the physical key position (USB HID usage id, = SDL_Scancode).
//   - Key: the layout-dependent key. Printable keys are their unshifted Unicode code point ('a', '1', '`');
//     other keys are `scancode | kKeyScancodeBit` (= SDL_Keycode and SDL_SCANCODE_TO_KEYCODE).
// Chords (keymaps) match on Key + modifiers, like Ableton's shortcuts; a future macro mode (M8) that wants
// positional keys can match on the scancode carried by the same event.

#include <cstdint>
#include <string>
#include <string_view>

#include "pq/core/result.h"
#include "pq/ui/draw_list.h"

namespace pq::ui {

// --- modifiers ------------------------------------------------------------------------------------------
using Mods = uint16_t;
namespace mod {
inline constexpr Mods kNone = 0;
inline constexpr Mods kShift = 1u << 0;
inline constexpr Mods kCtrl = 1u << 1;
inline constexpr Mods kAlt = 1u << 2;
inline constexpr Mods kSuper = 1u << 3;     // Windows key
inline constexpr Mods kCapsLock = 1u << 4;  // lock state, never part of a chord
// The modifiers a chord is made of (left/right variants are merged by the adapter).
inline constexpr Mods kChordMask = kShift | kCtrl | kAlt | kSuper;
}  // namespace mod

// --- keys -----------------------------------------------------------------------------------------------
enum class Scancode : uint16_t {
  Unknown = 0,
  A = 4, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,  // 4..29
  Num1 = 30, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9, Num0,                   // 30..39
  Return = 40, Escape = 41, Backspace = 42, Tab = 43, Space = 44,
  Minus = 45, Equals = 46, LeftBracket = 47, RightBracket = 48, Backslash = 49,
  Semicolon = 51, Apostrophe = 52, Grave = 53, Comma = 54, Period = 55, Slash = 56,
  CapsLock = 57,
  F1 = 58, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,  // 58..69
  Insert = 73, Home = 74, PageUp = 75, Delete = 76, End = 77, PageDown = 78,
  Right = 79, Left = 80, Down = 81, Up = 82,
  LCtrl = 224, LShift = 225, LAlt = 226, LSuper = 227, RCtrl = 228, RShift = 229, RAlt = 230, RSuper = 231,
};

using Key = uint32_t;
inline constexpr Key kKeyScancodeBit = 1u << 30;
constexpr Key keyFromScancode(Scancode s) noexcept { return static_cast<Key>(s) | kKeyScancodeBit; }

namespace key {
inline constexpr Key kUnknown = 0;
inline constexpr Key kReturn = '\r';
inline constexpr Key kEscape = 0x1B;
inline constexpr Key kBackspace = 0x08;
inline constexpr Key kTab = '\t';
inline constexpr Key kSpace = ' ';
inline constexpr Key kDelete = 0x7F;
inline constexpr Key kCapsLock = keyFromScancode(Scancode::CapsLock);
inline constexpr Key kF1 = keyFromScancode(Scancode::F1);  // kF1 + n - 1 = Fn (n ≤ 12)
inline constexpr Key kInsert = keyFromScancode(Scancode::Insert);
inline constexpr Key kHome = keyFromScancode(Scancode::Home);
inline constexpr Key kPageUp = keyFromScancode(Scancode::PageUp);
inline constexpr Key kEnd = keyFromScancode(Scancode::End);
inline constexpr Key kPageDown = keyFromScancode(Scancode::PageDown);
inline constexpr Key kRight = keyFromScancode(Scancode::Right);
inline constexpr Key kLeft = keyFromScancode(Scancode::Left);
inline constexpr Key kDown = keyFromScancode(Scancode::Down);
inline constexpr Key kUp = keyFromScancode(Scancode::Up);
inline constexpr Key kLCtrl = keyFromScancode(Scancode::LCtrl);
inline constexpr Key kLShift = keyFromScancode(Scancode::LShift);
inline constexpr Key kLAlt = keyFromScancode(Scancode::LAlt);
inline constexpr Key kLSuper = keyFromScancode(Scancode::LSuper);
inline constexpr Key kRCtrl = keyFromScancode(Scancode::RCtrl);
inline constexpr Key kRShift = keyFromScancode(Scancode::RShift);
inline constexpr Key kRAlt = keyFromScancode(Scancode::RAlt);
inline constexpr Key kRSuper = keyFromScancode(Scancode::RSuper);
}  // namespace key

// Ctrl/Shift/Alt/Super (either side) and Caps Lock: keys that are never a chord on their own.
constexpr bool isModifierKey(Key k) noexcept {
  return (k >= key::kLCtrl && k <= key::kRSuper) || k == key::kCapsLock;
}

// --- events ---------------------------------------------------------------------------------------------
enum class PointerButton : uint8_t { None = 0, Left = 1, Middle = 2, Right = 3, X1 = 4, X2 = 5 };  // SDL numbering

enum class PointerType : uint8_t {
  Move,
  Down,
  Up,
  Wheel,
  Leave,  // the pointer left the window: hover ends (capture, if any, stays until the button is released)
};

struct PointerEvent {
  PointerType type = PointerType::Move;
  PointerButton button = PointerButton::None;  // Down/Up
  uint8_t clickCount = 0;  // Down/Up: 1 = single, 2 = double, ... (from the platform's double-click rules)
  Mods mods = mod::kNone;
  Vec2 pos{0, 0};    // device pixels
  Vec2 wheel{0, 0};  // Wheel: notches; +y = away from the user (scroll up / towards the start), +x = right
};

struct KeyEvent {
  bool down = true;
  bool repeat = false;  // auto-repeat of a held key
  Mods mods = mod::kNone;
  Scancode scancode = Scancode::Unknown;
  Key key = key::kUnknown;
};

// Committed text (after the IME / keyboard layout). Fixed buffer: dispatch never allocates.
struct TextEvent {
  char utf8[32] = {};
  uint8_t size = 0;
  std::string_view text() const noexcept { return {utf8, size}; }
};
// Builds a TextEvent (truncates at a UTF-8 boundary to fit the buffer).
TextEvent makeTextEvent(std::string_view utf8) noexcept;

enum class InputKind : uint8_t { None, Pointer, Key, Text };
struct InputEvent {
  InputKind kind = InputKind::None;
  PointerEvent pointer;
  KeyEvent key;
  TextEvent text;
};

// A key that produces text when typed without command modifiers: a text field claims it (SPEC §6: text
// fields swallow printable keys so e.g. `B` does not switch to draw mode while renaming a track).
constexpr bool isPrintableKey(const KeyEvent& e) noexcept {
  return e.key >= 0x20 && e.key < kKeyScancodeBit && e.key != key::kDelete &&
         (e.mods & (mod::kCtrl | mod::kAlt | mod::kSuper)) == 0;
}

// --- chords ---------------------------------------------------------------------------------------------
struct Chord {
  Key key = key::kUnknown;
  Mods mods = mod::kNone;  // subset of mod::kChordMask
  bool operator==(const Chord&) const = default;
};

constexpr Chord chordOf(const KeyEvent& e) noexcept { return {e.key, static_cast<Mods>(e.mods & mod::kChordMask)}; }

// Parses "Ctrl+Shift+T", "B", "Space", "F9", "Shift+Tab", "Ctrl+Alt+B", "Up", "`", "-", "=", "Ctrl+1".
// Modifier names: Ctrl/Control, Shift, Alt, Super/Win/Cmd (case-insensitive). Key names: one printable ASCII
// character (letters are case-insensitive), or Space, Enter/Return, Escape/Esc, Tab, Backspace, Delete/Del,
// Insert, Home, End, PageUp, PageDown, Up, Down, Left, Right, F1..F12, CapsLock, Plus (for '+').
Result<Chord> parseChord(std::string_view text);
// Canonical text: modifiers in the order Ctrl, Alt, Shift, Super; letters upper case ("Ctrl+Shift+T").
std::string formatChord(const Chord& chord);

}  // namespace pq::ui
