// The bug report's recent input (T5.x-bugreport): pq/views/bug_report/input_log.h.
#include "pq/views/bug_report/input_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace pq::views::bugreport {

void writePathTail(const ui::Widget* w, char* out, size_t n) noexcept {
  if (n == 0) return;
  out[0] = '\0';
  std::string_view parts[48];
  size_t count = 0;
  for (const ui::Widget* p = w; p && count < std::size(parts); p = p->parent())
    if (!p->name().empty()) parts[count++] = p->name();
  // parts[0] is the leaf. Fit as many trailing segments as possible.
  size_t need = 0, take = 0;
  for (; take < count; ++take) {
    const size_t add = parts[take].size() + (take ? 1 : 0);
    if (need + add + 4 > n - 1 && take > 0) break;  // keep room for "…/"
    need += add;
  }
  size_t pos = 0;
  auto put = [&](std::string_view s) {
    const size_t k = std::min(s.size(), n - 1 - pos);
    std::memcpy(out + pos, s.data(), k);
    pos += k;
  };
  if (take < count) put("\xE2\x80\xA6/");  // "…/"
  for (size_t i = take; i-- > 0;) {
    put(parts[i]);
    if (i) put("/");
  }
  out[pos] = '\0';
}

namespace {

void copyText(char* dst, size_t n, std::string_view s) noexcept {
  const size_t k = std::min(s.size(), n - 1);
  std::memcpy(dst, s.data(), k);
  dst[k] = '\0';
}

bool printable(uint32_t key) noexcept { return key >= 0x20 && key < 0x7F; }

}  // namespace

void InputLog::record(const ui::InputEvent& e, const ui::UiTree& tree, double now) noexcept {
  Entry en;
  en.t = now;
  const ui::Widget* w = nullptr;
  switch (e.kind) {
    case ui::InputKind::Pointer: {
      const ui::PointerEvent& p = e.pointer;
      if (p.type == ui::PointerType::Down) en.kind = Kind::Down;
      else if (p.type == ui::PointerType::Up) en.kind = Kind::Up;
      else if (p.type == ui::PointerType::Wheel) en.kind = Kind::Wheel;
      else return;  // moves and leaves: nothing
      en.x = p.pos.x;
      en.y = p.pos.y;
      en.wx = p.wheel.x;
      en.wy = p.wheel.y;
      en.button = static_cast<uint8_t>(p.button);
      en.clicks = static_cast<uint8_t>(std::min<uint32_t>(p.clickCount, 255));
      en.mods = static_cast<ui::Mods>(p.mods & ui::mod::kChordMask);
      w = tree.hitTest(p.pos);
      // A press that started a capture names what took it (a drag keeps its target even off it).
      if (en.kind == Kind::Down && tree.captured()) w = tree.captured();
      break;
    }
    case ui::InputKind::Key: {
      const ui::KeyEvent& k = e.key;
      if (!k.down) return;
      w = tree.focus();
      // Printable keys typed into a text field arrive again as text: log them once, as text.
      const bool typing = w && w->acceptsTextInput() && printable(static_cast<uint32_t>(k.key)) &&
                          (k.mods & (ui::mod::kCtrl | ui::mod::kAlt | ui::mod::kSuper)) == 0;
      if (typing) return;
      en.kind = Kind::Key;
      en.key = static_cast<uint32_t>(k.key);
      en.mods = static_cast<ui::Mods>(k.mods & ui::mod::kChordMask);
      en.repeat = k.repeat;
      break;
    }
    case ui::InputKind::Text:
      en.kind = Kind::Text;
      copyText(en.text, kTextChars, e.text.text());
      w = tree.focus();
      break;
    default: return;
  }
  if (w) {
    en.type = &typeid(*w);
    writePathTail(w, en.path, kPathChars);
  }
  ring_[total_ % kCapacity] = en;
  ++total_;
}

namespace {

const char* buttonName(uint8_t b) {
  switch (b) {
    case 1: return "LMB";
    case 2: return "MMB";
    case 3: return "RMB";
    case 4: return "X1";
    case 5: return "X2";
    default: return "button";
  }
}

std::string modsPrefix(ui::Mods m) {
  std::string s;
  if (m & ui::mod::kCtrl) s += "Ctrl+";
  if (m & ui::mod::kAlt) s += "Alt+";
  if (m & ui::mod::kShift) s += "Shift+";
  if (m & ui::mod::kSuper) s += "Win+";
  return s;
}

std::string shortType(const std::type_info* t) {
  if (!t) return {};
  std::string n = t->name();
  for (const char* p : {"class ", "struct "})
    if (n.rfind(p, 0) == 0) n.erase(0, std::char_traits<char>::length(p));
  if (const size_t c = n.rfind("::"); c != std::string::npos) n.erase(0, c + 2);
  return n;
}

std::string where(const InputLog::Entry& e) {
  std::string s = e.path[0] ? std::string(e.path) : std::string("(unnamed)");
  if (e.type) s += " [" + shortType(e.type) + "]";
  return s;
}

std::string pos(float x, float y) {
  char b[48];
  std::snprintf(b, sizeof b, "(%.0f, %.0f)", static_cast<double>(x), static_cast<double>(y));
  return b;
}

}  // namespace

std::string InputLog::format(double now, size_t maxLines, uint64_t since) const {
  const uint64_t first = std::max<uint64_t>(since, total_ > kCapacity ? total_ - kCapacity : 0);
  struct Line {
    double t;
    std::string text;
  };
  std::vector<Line> lines;
  struct Pending {
    size_t line = SIZE_MAX;
    uint64_t entry = 0;
  };
  Pending down[8];
  size_t lastWheel = SIZE_MAX, lastText = SIZE_MAX, lastKey = SIZE_MAX;
  uint64_t wheelEntry = 0, keyEntry = 0;
  int wheelCount = 0, keyRepeats = 0;
  float wheelX = 0, wheelY = 0;
  std::string typed;
  auto stamp = [&](double t) {
    char b[32];
    std::snprintf(b, sizeof b, "%+.3f s  ", t - now);
    return std::string(b);
  };
  for (uint64_t i = first; i < total_; ++i) {
    const Entry& e = entry(i);
    if (e.kind != Kind::Wheel) lastWheel = SIZE_MAX;
    if (e.kind != Kind::Text) lastText = SIZE_MAX;
    if (e.kind != Kind::Key) lastKey = SIZE_MAX;
    switch (e.kind) {
      case Kind::Down: {
        const uint8_t b = std::min<uint8_t>(e.button, 7);
        std::string t = modsPrefix(e.mods) + buttonName(e.button) + (e.clicks >= 2 ? " double-press " : " down ") + pos(e.x, e.y) + " on " +
                        where(e) + ", still held at F12";
        down[b] = {lines.size(), i};
        lines.push_back({e.t, std::move(t)});
        break;
      }
      case Kind::Up: {
        const uint8_t b = std::min<uint8_t>(e.button, 7);
        if (down[b].line == SIZE_MAX) {
          lines.push_back({e.t, modsPrefix(e.mods) + buttonName(e.button) + " up " + pos(e.x, e.y) + " on " + where(e)});
          break;
        }
        const Entry& d = entry(down[b].entry);
        const float dx = e.x - d.x, dy = e.y - d.y;
        std::string& t = lines[down[b].line].text;
        if (dx * dx + dy * dy <= 16.0f) {
          t = modsPrefix(d.mods) + buttonName(d.button) + (d.clicks >= 2 ? " double-click " : " click ") + pos(d.x, d.y) + " on " + where(d);
        } else {
          char dur[32];
          std::snprintf(dur, sizeof dur, ", %.2f s", e.t - d.t);
          t = modsPrefix(d.mods) + buttonName(d.button) + " drag " + pos(d.x, d.y) + " \xE2\x86\x92 " + pos(e.x, e.y) + " on " + where(d);
          if (std::strcmp(d.path, e.path) != 0) t += ", released over " + where(e);
          t += dur;
        }
        down[b] = {};
        break;
      }
      case Kind::Wheel: {
        if (lastWheel != SIZE_MAX && std::strcmp(entry(wheelEntry).path, e.path) == 0 && e.t - entry(wheelEntry).t < 0.6 &&
            entry(wheelEntry).mods == e.mods) {
          ++wheelCount;
          wheelX += e.wx;
          wheelY += e.wy;
        } else {
          lastWheel = lines.size();
          wheelCount = 1;
          wheelX = e.wx;
          wheelY = e.wy;
          lines.push_back({e.t, {}});
        }
        wheelEntry = i;
        char b[96];
        std::snprintf(b, sizeof b, "wheel x%d (dy %+.1f%s) at ", wheelCount, static_cast<double>(wheelY),
                      wheelX != 0.0f ? (", dx " + std::to_string(static_cast<int>(std::lround(wheelX)))).c_str() : "");
        lines[lastWheel].text = modsPrefix(e.mods) + b + pos(e.x, e.y) + " on " + where(e);
        break;
      }
      case Kind::Key: {
        const ui::Chord c{static_cast<ui::Key>(e.key), e.mods};
        if (lastKey != SIZE_MAX && e.repeat && entry(keyEntry).key == e.key && entry(keyEntry).mods == e.mods) {
          ++keyRepeats;
        } else {
          lastKey = lines.size();
          keyRepeats = 1;
          lines.push_back({e.t, {}});
        }
        keyEntry = i;
        std::string t = "key " + ui::formatChord(c);
        if (keyRepeats > 1) t += " x" + std::to_string(keyRepeats) + " (held)";
        t += "  (focus: " + (e.path[0] || e.type ? where(e) : std::string("none")) + ")";
        lines[lastKey].text = std::move(t);
        break;
      }
      case Kind::Text: {
        if (lastText != SIZE_MAX && typed.size() < 120) {
          typed += e.text;
        } else {
          lastText = lines.size();
          typed = e.text;
          lines.push_back({e.t, {}});
        }
        lines[lastText].text = "typed \"" + typed + "\" in " + where(e);
        break;
      }
    }
  }
  const size_t from = lines.size() > maxLines ? lines.size() - maxLines : 0;
  std::string out;
  for (size_t i = from; i < lines.size(); ++i) out += "- " + stamp(lines[i].t) + lines[i].text + "\n";
  return out;
}

}  // namespace pq::views::bugreport
