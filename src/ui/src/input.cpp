#include "pq/ui/input.h"

#include <cstring>
#include <string>

namespace pq::ui {

namespace {

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

struct NamedKey {
  std::string_view name;
  Key key;
};
// First entry per key is its canonical name (formatChord).
constexpr NamedKey kNamedKeys[] = {
    {"Space", key::kSpace},       {"Enter", key::kReturn},       {"Return", key::kReturn},
    {"Escape", key::kEscape},     {"Esc", key::kEscape},         {"Tab", key::kTab},
    {"Backspace", key::kBackspace}, {"Delete", key::kDelete},    {"Del", key::kDelete},
    {"Insert", key::kInsert},     {"Home", key::kHome},          {"End", key::kEnd},
    {"PageUp", key::kPageUp},     {"PageDown", key::kPageDown},  {"Up", key::kUp},
    {"Down", key::kDown},         {"Left", key::kLeft},          {"Right", key::kRight},
    {"CapsLock", key::kCapsLock}, {"Plus", '+'},
};

}  // namespace

TextEvent makeTextEvent(std::string_view utf8) noexcept {
  TextEvent t;
  size_t n = utf8.size() < sizeof(t.utf8) ? utf8.size() : sizeof(t.utf8);
  // Do not cut a multi-byte sequence: back up over continuation bytes of a truncated code point.
  if (n < utf8.size())
    while (n > 0 && (static_cast<unsigned char>(utf8[n]) & 0xC0u) == 0x80u) --n;
  std::memcpy(t.utf8, utf8.data(), n);
  t.size = static_cast<uint8_t>(n);
  return t;
}

Result<Chord> parseChord(std::string_view text) {
  auto fail = [&](const char* why) {
    return Error{ErrorCode::Parse, "chord '" + std::string(text) + "': " + why, {}};
  };
  if (text.empty()) return fail("empty");
  Chord c;
  size_t pos = 0;
  while (true) {
    size_t plus = text.find('+', pos);
    // A '+' as the final key: "Ctrl++" or "+".
    if (plus == pos && plus + 1 == text.size()) plus = std::string_view::npos;
    const std::string_view tok = text.substr(pos, plus == std::string_view::npos ? std::string_view::npos : plus - pos);
    if (tok.empty()) return fail("empty part");
    if (plus != std::string_view::npos) {  // a modifier
      Mods m = mod::kNone;
      if (iequals(tok, "ctrl") || iequals(tok, "control")) m = mod::kCtrl;
      else if (iequals(tok, "shift")) m = mod::kShift;
      else if (iequals(tok, "alt")) m = mod::kAlt;
      else if (iequals(tok, "super") || iequals(tok, "win") || iequals(tok, "cmd")) m = mod::kSuper;
      else return fail("unknown modifier");
      if (c.mods & m) return fail("repeated modifier");
      c.mods = static_cast<Mods>(c.mods | m);
      pos = plus + 1;
      continue;
    }
    // The key.
    if (tok.size() == 1) {
      const char ch = tok[0];
      if (ch <= 0x20 || ch >= 0x7F) return fail("not a printable key");
      c.key = static_cast<Key>(static_cast<unsigned char>(lower(ch)));
      return c;
    }
    if ((tok[0] == 'F' || tok[0] == 'f') && tok.size() <= 3) {
      int n = 0;
      bool digits = true;
      for (size_t i = 1; i < tok.size(); ++i) {
        if (tok[i] < '0' || tok[i] > '9') digits = false;
        else n = n * 10 + (tok[i] - '0');
      }
      if (digits && n >= 1 && n <= 12) {
        c.key = key::kF1 + static_cast<Key>(n - 1);
        return c;
      }
    }
    for (const NamedKey& k : kNamedKeys)
      if (iequals(tok, k.name)) {
        c.key = k.key;
        return c;
      }
    return fail("unknown key name");
  }
}

std::string formatChord(const Chord& chord) {
  std::string s;
  if (chord.mods & mod::kCtrl) s += "Ctrl+";
  if (chord.mods & mod::kAlt) s += "Alt+";
  if (chord.mods & mod::kShift) s += "Shift+";
  if (chord.mods & mod::kSuper) s += "Super+";
  const Key k = chord.key;
  if (k >= key::kF1 && k < key::kF1 + 12) {
    s += 'F';
    s += std::to_string(k - key::kF1 + 1);
    return s;
  }
  for (const NamedKey& n : kNamedKeys)
    if (n.key == k && k != '+') {
      s += n.name;
      return s;
    }
  if (k > 0x20 && k < 0x7F) {
    const char ch = static_cast<char>(k);
    s += (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
    return s;
  }
  s += "Key" + std::to_string(k);
  return s;
}

}  // namespace pq::ui
