#pragma once

// VZ.1: the display modules' settings sheet (Spectrogram, Oscilloscope, Spectrum), private to views. A face keeps its
// options as the instance's data (a record of numbers: only the fields that differ from the factory default) through
// Values over its table of Opt; its SET chip opens the Sheet BESIDE the face (BR.ui, the user: "make it open as an
// adjacent panel so I can see the visual change live"): a floating panel (MetaFace::popupRect) the host draws over
// everything, right of the face, left when the host's area has no room on the right; over the plot only when there is
// room on neither side (or the host has none around the face: setPopupOverFace):
//
//   [ANALYSIS] [DISPLAY]                         [SAVE DEFAULT] [RESET]
//   label ............ value   label ............ value   …
//
// The tabs split the options: ANALYSIS what is measured (window, span, pitch tracking, trigger, …), DISPLAY only how it
// looks (range, colours, line, glow, grid, labels). Number rows drag up / down (Shift: fine), choices click (Shift:
// back), a double-click resets a row. SAVE DEFAULT stores the options as the type's user default (meta::
// saveUserDefaultData: new instances start from it; the app persists it); it reads SAVED while the options are that
// default. RESET goes back to the factory options.
//
// VZ.10: COLOUR rows (Opt::Colour; a third tab, COLOURS, on a table that has them). A colour option is −1 while it
// follows the face's own colour (the theme's, the colormap's: PQ Meter's ColorSlot, resolved at paint, so an untouched
// colour follows a theme change), else 0xRRGGBB (the alpha stays the face's: its fades and tints keep working). Its row
// shows a swatch; a click opens the PICKER inside the sheet (an SV square, a hue strip, the new / old swatches, a hex
// field): a drag on the square or the strip previews live and is one undo step on release; the old swatch puts the
// colour back; a click on the hex field types (#RGB or #RRGGBB, Enter applies); Esc restores the colour the picker opened
// with and closes it. A double-click on the row (as every row) or RESET returns it to the face's own.
//
// UI thread only.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pq/meta/data.h"
#include "pq/ui/colormap.h"
#include "pq/meta/user_defaults.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/input.h"
#include "pq/ui/theme.h"

namespace pq::views::sheet {

// One option: its data field, label, range, default and how a value reads.
struct Opt {
  const char* key;
  const char* label;
  double def, lo, hi;
  enum Kind : uint8_t { Choice, Toggle, Number, LogNumber, Pow2, Colour } kind;
  const char* const* names = nullptr;  // Choice / Toggle
  const char* fmt = "%.2f";            // Number
  double step = 0.0;                   // a quantum (0: continuous)
};

inline constexpr const char* kOnOff[] = {"Off", "On"};

// VZ.10: a colour option (−1: the face's own colour, else 0xRRGGBB).
inline constexpr double kOwnColour = -1.0;
constexpr Opt colourOpt(const char* key, const char* label) noexcept {
  return {key, label, kOwnColour, kOwnColour, 16777215.0, Opt::Colour};
}
// The colour to paint: `fallback` (the face's own) unless the option holds one, then that with fallback's alpha.
inline ui::Rgba resolveColour(double v, ui::Rgba fallback) noexcept {
  return v < 0.0 ? fallback : ui::hex(static_cast<uint32_t>(v), ui::alphaOf(fallback));
}
inline double packColour(ui::Rgba c) noexcept {
  return static_cast<double>((ui::redOf(c) << 16) | (ui::greenOf(c) << 8) | ui::blueOf(c));
}

// The values of a table of options.
class Values {
public:
  explicit Values(std::span<const Opt> table) : t_(table), v_(table.size()), shown_(table.size(), ui::rgba(128, 128, 128)) { reset(); }
  void reset() {
    for (size_t i = 0; i < t_.size(); ++i) v_[i] = t_[i].def;
  }
  std::span<const Opt> table() const noexcept { return t_; }
  double operator[](uint32_t i) const noexcept { return v_[i]; }
  void set(uint32_t i, double v) noexcept { v_[i] = std::clamp(v, t_[i].lo, t_[i].hi); }
  int choice(uint32_t i) const noexcept { return static_cast<int>(std::lround(v_[i])); }
  bool on(uint32_t i) const noexcept { return v_[i] >= 0.5; }
  bool isDefault(uint32_t i) const noexcept { return v_[i] == t_[i].def; }
  // VZ.10: colour option i as painted, `fallback` being the face's own colour (remembered: the sheet's swatch shows it).
  ui::Rgba colour(uint32_t i, ui::Rgba fallback) const noexcept {
    shown_[i] = fallback;
    return resolveColour(v_[i], fallback);
  }
  ui::Rgba shownColour(uint32_t i) const noexcept { return ui::withAlpha(resolveColour(v_[i], shown_[i]), 255); }
  ui::Rgba fallbackColour(uint32_t i) const noexcept { return ui::withAlpha(shown_[i], 255); }
  meta::DataPtr data() const {
    std::vector<std::pair<std::string, meta::DataValue>> f;
    for (size_t i = 0; i < t_.size(); ++i)
      if (v_[i] != t_[i].def) f.emplace_back(t_[i].key, meta::DataValue::num(v_[i]));  // only what differs
    return std::make_shared<const meta::DataValue>(meta::DataValue::record(std::move(f)));
  }
  void load(const meta::DataValue* d) {
    for (size_t i = 0; i < t_.size(); ++i) {
      const meta::DataValue* f = d != nullptr ? d->field(t_[i].key) : nullptr;
      v_[i] = f != nullptr && f->kind == meta::DataValue::Kind::Number ? std::clamp(f->number, t_[i].lo, t_[i].hi) : t_[i].def;
      if (t_[i].kind == Opt::Colour) v_[i] = std::round(v_[i]);
    }
  }
  bool operator==(const Values& o) const noexcept { return v_ == o.v_; }

private:
  std::span<const Opt> t_;
  std::vector<double> v_;
  mutable std::vector<ui::Rgba> shown_;  // the face's own colour of each colour option, as last painted
};

inline bool hasColours(std::span<const Opt> table) noexcept {
  return std::any_of(table.begin(), table.end(), [](const Opt& o) { return o.kind == Opt::Colour; });
}

// VZ.10: the six Custom-ramp stop colours (options first … first + 5) over `base`'s.
inline ui::StopColours stopColours(const Values& v, uint32_t first, int base) noexcept {
  ui::StopColours s;
  const auto m = static_cast<ui::Colormap>(std::clamp(base, 0, ui::kColormapCount - 1));
  for (int i = 0; i < ui::Gradient::kStops; ++i) {
    const auto o = first + static_cast<uint32_t>(i);
    s.c[i] = v.colour(o, ui::levelStopColour(m, i));
    if (v[o] >= 0.0) s.set = static_cast<uint8_t>(s.set | (1u << i));
  }
  return s;
}

inline void valueText(const Values& o, uint32_t i, char* buf, size_t n) {
  const Opt& op = o.table()[i];
  if (op.kind == Opt::Choice || op.kind == Opt::Toggle) {
    std::snprintf(buf, n, "%s", op.names[std::clamp(o.choice(i), static_cast<int>(op.lo), static_cast<int>(op.hi)) - static_cast<int>(op.lo)]);
    return;
  }
  if (op.kind == Opt::Pow2) {
    std::snprintf(buf, n, "%d", 1 << o.choice(i));
    return;
  }
  if (op.kind == Opt::Colour) {
    std::snprintf(buf, n, "#%06X", static_cast<unsigned>(packColour(o.shownColour(i))));
    return;
  }
  std::snprintf(buf, n, op.fmt, o[i]);
}
inline bool numeric(const Opt& op) noexcept { return op.kind == Opt::Number || op.kind == Opt::LogNumber || op.kind == Opt::Pow2; }
inline double toTravel(const Opt& op, double v) noexcept {
  if (op.kind == Opt::LogNumber) return std::log(v / op.lo) / std::log(op.hi / op.lo);
  return (v - op.lo) / (op.hi - op.lo);
}
inline double fromTravel(const Opt& op, double t) noexcept {
  t = std::clamp(t, 0.0, 1.0);
  double v = op.kind == Opt::LogNumber ? op.lo * std::pow(op.hi / op.lo, t) : op.lo + t * (op.hi - op.lo);
  if (op.kind == Opt::Pow2) v = std::round(v);
  else if (op.step > 0.0) v = std::round(v / op.step) * op.step;
  return std::clamp(v, op.lo, op.hi);
}
// A choice / toggle one step on (dir +1) or back (−1), wrapping.
inline double cycled(const Values& o, uint32_t i, int dir) noexcept {
  const Opt& op = o.table()[i];
  const int n = static_cast<int>(op.hi - op.lo) + 1;
  return op.lo + static_cast<double>(((o.choice(i) - static_cast<int>(op.lo) + dir) % n + n) % n);
}

// The sheet's parts (a face's own parts stay below kColoursTab). The tabs: ANALYSIS kTabBase, DISPLAY kTabBase + 1,
// COLOURS kColoursTab (only on a table with colours); the picker's kPickSv … kPickBox.
enum Part : int32_t {
  kColoursTab = 89, kTabBase = 90, kSave = 92, kReset = 93, kBody = 94, kPickSv = 95, kPickHue = 96, kPickOld = 97,
  kPickHex = 98, kPickBox = 99, kRowBase = 100
};
enum Tab : int { kAnalysis = 0, kDisplay = 1, kColours = 2 };
inline constexpr const char* kTabNames[] = {"ANALYSIS", "DISPLAY", "COLOURS"};
inline constexpr int32_t tabPart(int t) noexcept { return t == kColours ? kColoursTab : kTabBase + t; }
inline constexpr bool isTab(int32_t part) noexcept { return part >= kColoursTab && part <= kTabBase + 1; }
inline constexpr int tabOf(int32_t part) noexcept { return part == kColoursTab ? kColours : part - kTabBase; }
// A part of the sheet's head: a tab, SAVE DEFAULT or RESET (a pointer cursor).
inline constexpr bool isHead(int32_t part) noexcept { return part >= kColoursTab && part <= kReset; }
inline constexpr bool isPicker(int32_t part) noexcept { return part >= kPickSv && part <= kPickBox; }

// BR.ui: where a sheet shows — over the plot, or a floating panel right / left of the face.
enum class Place : uint8_t { Over, Right, Left };
inline constexpr float kSideGap = 6.0f;  // face units between the face and a side sheet

// The side with room for a side sheet `panelW` wide (face units) next to a face `faceW` wide whose left edge is at device
// x `faceX0` (`pxPerUnit` device px per unit), inside the host's device x range [roomX0, roomX1]: the right first, then
// the left, else over the plot. `hostOver`: the host has no room around the face (MetaFace::setPopupOverFace).
inline Place choosePlace(float faceX0, float pxPerUnit, float faceW, float panelW, float roomX0, float roomX1, bool hostOver) noexcept {
  if (hostOver) return Place::Over;
  const float side = (kSideGap + panelW) * pxPerUnit;
  if (faceX0 + faceW * pxPerUnit + side <= roomX1) return Place::Right;
  if (faceX0 - side >= roomX0) return Place::Left;
  return Place::Over;
}

// The sheet's geometry and painting over `area` (face-local world units). The face decides which options a tab shows
// (in order) and passes them as `rows`.
class Sheet {
public:
  Sheet(ui::Rect area, float textPx) : over_(area), area_(area), px_(textPx) {}

  int tab = kDisplay;  // opens on the look: what a user changes most
  int tabs = 2;        // VZ.10: 3 with the COLOURS tab (a table with colour options)

  // BR.ui: placed over the plot (the area it was made with) or beside a face `faceW` × `faceH`, as wide as the face
  // (VZ.4: or `panelW` wide, for a narrow face such as the Level Meter's).
  void place(Place where, float faceW, float faceH, float panelW = 0.0f) noexcept {
    place_ = where;
    const float pw = panelW > 0.0f ? panelW : faceW;
    if (where == Place::Right) area_ = {faceW + kSideGap, 0.0f, faceW + kSideGap + pw, faceH};
    else if (where == Place::Left) area_ = {-kSideGap - pw, 0.0f, -kSideGap, faceH};
    else area_ = over_;
    closePicker();
  }
  Place placed() const noexcept { return place_; }
  bool beside() const noexcept { return place_ != Place::Over; }
  ui::Rect area() const noexcept { return area_; }
  ui::Rect tabRect(int t) const noexcept {
    const float w = px_ * (tabs > 2 ? 6.0f : 7.6f);  // (three tabs: narrower, so the head fits a 220-unit sheet)
    const float x0 = area_.x0 + 3.0f + static_cast<float>(t) * (w + 2.0f);
    return {x0, area_.y0 + 2.0f, x0 + w, area_.y0 + 2.0f + headH()};
  }
  ui::Rect resetRect() const noexcept {
    const float w = px_ * 5.4f;
    return {area_.x1 - 3.0f - w, area_.y0 + 2.0f, area_.x1 - 3.0f, area_.y0 + 2.0f + headH()};
  }
  ui::Rect saveRect() const noexcept {
    const float w = px_ * 11.0f;
    const ui::Rect r = resetRect();
    return {r.x0 - 2.0f - w, r.y0, r.x0 - 2.0f, r.y1};
  }
  ui::Rect rowRect(size_t i, size_t n) const noexcept {
    const float top = area_.y0 + 4.0f + headH(), avail = area_.y1 - 2.0f - top;
    const float minH = px_ + 3.0f;
    size_t colsN = 2;
    while (colsN < 4 && static_cast<float>((n + colsN - 1) / colsN) * minH > avail) ++colsN;
    const size_t per = std::max<size_t>(1, (n + colsN - 1) / colsN);
    const float rowH = std::min(px_ + 4.5f, avail / static_cast<float>(per));
    const size_t col = i / per, row = i % per;
    const float colW = (area_.x1 - area_.x0 - 6.0f) / static_cast<float>(colsN);
    const float x0 = area_.x0 + 3.0f + static_cast<float>(col) * colW;
    const float y0 = top + static_cast<float>(row) * rowH;
    return {x0, y0, x0 + colW - 2.0f, y0 + rowH - 0.5f};
  }
  // The part under l (kRowBase + i: row i of n; the picker's parts over everything while it is open), else kBody inside
  // the sheet, else -1.
  int32_t hit(ui::Vec2 l, size_t n) const noexcept {
    if (pick_.open && in(pick_.box, l)) {
      if (in(svRect(), l)) return kPickSv;
      if (in(hueRect(), l)) return kPickHue;
      if (in(oldRect(), l)) return kPickOld;
      if (in(hexRect(), l)) return kPickHex;
      return kPickBox;
    }
    for (int t = 0; t < tabs; ++t)
      if (in(tabRect(t), l)) return tabPart(t);
    if (in(saveRect(), l)) return kSave;
    if (in(resetRect(), l)) return kReset;
    for (size_t i = 0; i < n; ++i)
      if (in(rowRect(i, n), l)) return kRowBase + static_cast<int32_t>(i);
    return in(area_, l) ? kBody : -1;
  }

  // `saved`: the options are the type's saved default (SAVE DEFAULT reads SAVED).
  void paint(ui::FacePainter& p, const ui::Theme& th, const Values& v, std::span<const uint32_t> rows, int32_t hovered,
             bool saved) const {
    const ui::Rgba dim = ui::rgba(150, 162, 166), text = th.device.displayText;
    if (beside()) {  // a panel of its own: a shadow and the faces' frame round it
      p.roundedRect({area_.x0 + 1.5f, area_.y0 + 2.5f, area_.x1 + 1.5f, area_.y1 + 2.5f}, 7.0f, ui::rgba(0, 0, 0, 90));
      p.roundedRect(area_, 7.0f, th.face.frame);
      p.roundedRect({area_.x0 + 1.25f, area_.y0 + 1.25f, area_.x1 - 1.25f, area_.y1 - 1.25f}, 5.75f, ui::rgba(10, 10, 14, 255));
    } else {
      p.roundedRect(area_, 4.0f, ui::rgba(10, 10, 14, 248));
    }
    for (int t = 0; t < tabs; ++t) {
      const ui::Rect r = tabRect(t);
      const bool on = tab == t;
      p.roundedRect(r, 2.5f, on ? ui::mulAlpha(th.face.value, 0.3f) : (hovered == tabPart(t) ? ui::rgba(40, 44, 52) : ui::rgba(22, 24, 30)));
      p.text(centre(r), kTabNames[t], px_, on ? th.face.value : dim);
    }
    {
      const ui::Rect r = saveRect();
      p.roundedRect(r, 2.5f, hovered == kSave ? ui::rgba(40, 44, 52) : ui::rgba(22, 24, 30));
      p.text(centre(r), saved ? "SAVED" : "SAVE DEFAULT", px_, saved ? dim : text);
    }
    {
      const ui::Rect r = resetRect();
      p.roundedRect(r, 2.5f, hovered == kReset ? ui::rgba(40, 44, 52) : ui::rgba(22, 24, 30));
      p.text(centre(r), "RESET", px_, text);
    }
    char buf[48];
    for (size_t i = 0; i < rows.size(); ++i) {
      const ui::Rect rr = rowRect(i, rows.size());
      const uint32_t o = rows[i];
      const bool picking = pick_.open && pick_.opt == o;
      if (hovered == kRowBase + static_cast<int32_t>(i) || picking) p.roundedRect(rr, 2.0f, ui::mulAlpha(th.face.value, 0.18f));
      const float cy = 0.5f * (rr.y0 + rr.y1);
      p.textAligned({rr.x0 + 2.0f, cy}, v.table()[o].label, px_, text, 0.0f);
      if (v.table()[o].kind == Opt::Colour) {  // the swatch, ringed in the value colour once it replaces the face's own
        const float hh = std::min(0.5f * (rr.y1 - rr.y0) - 0.75f, 0.5f * px_);
        const ui::Rect sw{rr.x1 - 2.0f - 2.6f * px_, cy - hh, rr.x1 - 2.0f, cy + hh};
        p.roundedRect({sw.x0 - 0.75f, sw.y0 - 0.75f, sw.x1 + 0.75f, sw.y1 + 0.75f}, 2.0f, v.isDefault(o) ? ui::rgba(70, 76, 84) : th.face.value);
        p.roundedRect(sw, 1.5f, v.shownColour(o));
        continue;
      }
      valueText(v, o, buf, sizeof buf);
      p.textAligned({rr.x1 - 2.0f, cy}, buf, px_, v.isDefault(o) ? dim : th.face.value, 1.0f);
    }
    if (pick_.open) paintPicker(p, th, v, hovered);
  }

  // ---- VZ.10: the colour picker ----
  // Opens on colour option `o` (row `row` of `n`), over the half of the sheet away from the row.
  void openPicker(uint32_t o, size_t row, size_t n, const Values& v) noexcept {
    const ui::Rect rr = rowRect(row, n);
    const float top = area_.y0 + 4.0f + headH();
    // (at most half the sheet: the row's column, the row's swatch, stay in view)
    const float w = std::min(0.5f * (area_.x1 - area_.x0) - 4.0f, px_ * 24.0f), h = std::min(area_.y1 - 3.0f - top, px_ * 14.0f);
    const bool right = 0.5f * (rr.x0 + rr.x1) < 0.5f * (area_.x0 + area_.x1);
    const float x0 = right ? area_.x1 - 3.0f - w : area_.x0 + 3.0f;
    pick_ = {};
    pick_.open = true;
    pick_.opt = o;
    pick_.orig = v[o];
    pick_.box = {x0, top, x0 + w, top + h};
    syncPicker(v);
  }
  void closePicker() noexcept {
    pick_.open = pick_.typing = false;
    pick_.drag = 0;
  }
  bool pickerOpen() const noexcept { return pick_.open; }
  uint32_t pickerOpt() const noexcept { return pick_.opt; }
  double pickerOrigin() const noexcept { return pick_.orig; }
  bool picking() const noexcept { return pick_.drag != 0; }
  bool typing() const noexcept { return pick_.typing; }
  ui::Rect pickerRect() const noexcept { return pick_.box; }
  ui::Rect pickerPartRect(int32_t part) const noexcept {
    switch (part) {
      case kPickSv: return svRect();
      case kPickHue: return hueRect();
      case kPickOld: return oldRect();
      case kPickHex: return hexRect();
      default: return pick_.box;
    }
  }
  // A press on a picker part: the value it sets now (none: it sets nothing — the box; the hex field, which starts
  // typing). A press on the square or the strip starts a drag (pickDrag, pickRelease).
  std::optional<double> pickPress(int32_t part, ui::Vec2 l, const Values& v) noexcept {
    syncPicker(v);
    pick_.typing = false;
    if (part == kPickSv || part == kPickHue) {
      pick_.drag = part == kPickSv ? 1 : 2;
      return pickDrag(l);
    }
    if (part == kPickOld) return pick_.orig;
    if (part == kPickHex) {
      pick_.typing = true;
      pick_.typed[0] = '\0';
    }
    return std::nullopt;
  }
  double pickDrag(ui::Vec2 l) noexcept {
    if (pick_.drag == 1) {
      const ui::Rect r = svRect();
      pick_.s = std::clamp((l.x - r.x0) / (r.x1 - r.x0), 0.0f, 1.0f);
      pick_.v = 1.0f - std::clamp((l.y - r.y0) / (r.y1 - r.y0), 0.0f, 1.0f);
    } else if (pick_.drag == 2) {
      const ui::Rect r = hueRect();
      pick_.h = std::clamp((l.y - r.y0) / (r.y1 - r.y0), 0.0f, 1.0f);
    }
    pick_.rgb = hsv(pick_.h, pick_.s, pick_.v);
    return packColour(pick_.rgb);
  }
  void pickRelease() noexcept { pick_.drag = 0; }
  // Typing into the hex field: hex digits (anything else, a '#', skipped), at most six.
  bool typeText(std::string_view t) noexcept {
    if (!pick_.typing) return false;
    size_t n = std::strlen(pick_.typed);
    for (const char c : t)
      if (std::isxdigit(static_cast<unsigned char>(c)) != 0 && n < 6) {
        pick_.typed[n++] = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        pick_.typed[n] = '\0';
      }
    return true;
  }
  void typeBackspace() noexcept {
    const size_t n = std::strlen(pick_.typed);
    if (n > 0) pick_.typed[n - 1] = '\0';
  }
  void stopTyping() noexcept { pick_.typing = false; }
  // Enter: the typed colour (#RGB or #RRGGBB; none for anything else: typing goes on).
  std::optional<double> typedColour() noexcept {
    const size_t n = std::strlen(pick_.typed);
    if (n != 3 && n != 6) return std::nullopt;
    auto x = static_cast<uint32_t>(std::strtoul(pick_.typed, nullptr, 16));
    if (n == 3) x = ((x >> 8 & 0xFu) * 0x110000u) | ((x >> 4 & 0xFu) * 0x1100u) | ((x & 0xFu) * 0x11u);
    pick_.typing = false;
    return static_cast<double>(x);
  }

private:
  struct Pick {
    bool open = false, typing = false;
    int drag = 0;  // 1: the square, 2: the strip
    uint32_t opt = 0;
    double orig = kOwnColour;
    ui::Rect box{};
    float h = 0.0f, s = 0.0f, v = 0.0f;
    ui::Rgba rgb = 1;  // the colour h / s / v give (an option colour that differs, set elsewhere, re-reads them)
    char typed[8] = {};
  };

  float headH() const noexcept { return px_ + 4.0f; }
  float lineH() const noexcept { return px_ + 3.0f; }
  ui::Rect svRect() const noexcept {
    const ui::Rect& b = pick_.box;
    const float side = std::max(8.0f, std::min(b.y1 - b.y0 - 6.0f - lineH(), 0.55f * (b.x1 - b.x0)));
    return {b.x0 + 3.0f, b.y0 + 3.0f + lineH(), b.x0 + 3.0f + side, b.y0 + 3.0f + lineH() + side};
  }
  ui::Rect hueRect() const noexcept {
    const ui::Rect sv = svRect();
    return {sv.x1 + 3.0f, sv.y0, sv.x1 + 3.0f + 1.6f * px_, sv.y1};
  }
  ui::Rect newRect() const noexcept {
    const ui::Rect hu = hueRect();
    const float x1 = std::min(pick_.box.x1 - 3.0f, hu.x1 + 3.0f + 5.0f * px_);
    return {hu.x1 + 3.0f, hu.y0, x1, 0.5f * (hu.y0 + hu.y1) - 1.0f};
  }
  ui::Rect oldRect() const noexcept {
    const ui::Rect n = newRect(), hu = hueRect();
    return {n.x0, n.y1 + 2.0f, n.x1, hu.y1};
  }
  ui::Rect hexRect() const noexcept {
    const ui::Rect& b = pick_.box;
    return {b.x1 - 3.0f - 6.4f * px_, b.y0 + 2.0f, b.x1 - 3.0f, b.y0 + 1.0f + lineH()};
  }
  static ui::Rgba hsv(float h, float s, float v) noexcept {
    const float hh = (h >= 1.0f ? 0.0f : h) * 6.0f;
    const int i = static_cast<int>(hh);
    const float f = hh - static_cast<float>(i), p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    float r = v, g = t, b = p;
    switch (i) {
      case 1: r = q, g = v, b = p; break;
      case 2: r = p, g = v, b = t; break;
      case 3: r = p, g = q, b = v; break;
      case 4: r = t, g = p, b = v; break;
      case 5: r = v, g = p, b = q; break;
      default: break;
    }
    auto q8 = [](float x) { return static_cast<uint32_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return ui::rgba(q8(r), q8(g), q8(b));
  }
  // h / s / v from the option's colour when it changed elsewhere (a reset, an undo, the hex field, the old swatch); kept
  // otherwise, so the hue survives a grey and the saturation a black.
  void syncPicker(const Values& v) const noexcept {
    const ui::Rgba c = v.shownColour(pick_.opt);
    if (c == pick_.rgb) return;
    pick_.rgb = c;
    const float r = static_cast<float>(ui::redOf(c)) / 255.0f, g = static_cast<float>(ui::greenOf(c)) / 255.0f,
                b = static_cast<float>(ui::blueOf(c)) / 255.0f;
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    pick_.v = mx;
    if (mx > 0.0f) pick_.s = d / mx;
    if (d > 0.0f) {
      float h = mx == r ? (g - b) / d : (mx == g ? 2.0f + (b - r) / d : 4.0f + (r - g) / d);
      h /= 6.0f;
      pick_.h = h < 0.0f ? h + 1.0f : h;
    }
  }
  void paintPicker(ui::FacePainter& p, const ui::Theme& th, const Values& v, int32_t hovered) const {
    syncPicker(v);
    const ui::Rgba dim = ui::rgba(150, 162, 166);
    const ui::Rect& b = pick_.box;
    p.roundedRect({b.x0 + 1.0f, b.y0 + 1.5f, b.x1 + 1.0f, b.y1 + 1.5f}, 4.0f, ui::rgba(0, 0, 0, 120));
    p.roundedRect(b, 4.0f, th.face.frame);
    p.roundedRect({b.x0 + 0.75f, b.y0 + 0.75f, b.x1 - 0.75f, b.y1 - 0.75f}, 3.25f, ui::rgba(16, 17, 22));
    p.textAligned({b.x0 + 3.0f, b.y0 + 1.5f + 0.5f * lineH()}, v.table()[pick_.opt].label, px_, th.device.displayText, 0.0f);
    // The hex field (typing: what is typed and a caret).
    const ui::Rect hx = hexRect();
    p.roundedRect(hx, 2.0f, pick_.typing ? ui::mulAlpha(th.face.value, 0.3f) : (hovered == kPickHex ? ui::rgba(40, 44, 52) : ui::rgba(28, 30, 36)));
    char buf[12];
    if (pick_.typing) std::snprintf(buf, sizeof buf, "#%s_", pick_.typed);
    else std::snprintf(buf, sizeof buf, "#%06X", static_cast<unsigned>(packColour(pick_.rgb)));
    p.text(centre(hx), buf, px_, pick_.typing ? th.face.value : th.device.displayText);
    // The SV square: an 8 × 8 grid of the hue's colours (value up, saturation right).
    constexpr uint32_t kN = 8, kRow = kN + 1;
    const ui::Rect sv = svRect();
    ui::Vec2 pts[kRow * kRow];
    ui::Rgba cols[kRow * kRow];
    uint32_t idx[kN * kN * 6];
    for (uint32_t j = 0; j <= kN; ++j)
      for (uint32_t i = 0; i <= kN; ++i) {
        const float u = static_cast<float>(i) / kN, w = static_cast<float>(j) / kN;
        pts[j * kRow + i] = {sv.x0 + u * (sv.x1 - sv.x0), sv.y0 + w * (sv.y1 - sv.y0)};
        cols[j * kRow + i] = hsv(pick_.h, u, 1.0f - w);
      }
    uint32_t k = 0;
    for (uint32_t j = 0; j < kN; ++j)
      for (uint32_t i = 0; i < kN; ++i) {
        const uint32_t a = j * kRow + i, c = a + kRow;
        for (const uint32_t x : {a, a + 1, c + 1, a, c + 1, c}) idx[k++] = x;
      }
    p.mesh(pts, cols, idx);
    const ui::Vec2 at{sv.x0 + pick_.s * (sv.x1 - sv.x0), sv.y0 + (1.0f - pick_.v) * (sv.y1 - sv.y0)};
    p.ring(at, 2.2f, 1.6f, ui::rgba(0, 0, 0, 200));
    p.ring(at, 2.2f, 0.8f, ui::rgba(255, 255, 255));
    // The hue strip (red at the top, round to red at the bottom).
    const ui::Rect hu = hueRect();
    ui::Vec2 hp[14];
    ui::Rgba hc[14];
    uint32_t hi[36];
    for (uint32_t i = 0; i <= 6; ++i) {
      const float y = hu.y0 + static_cast<float>(i) / 6.0f * (hu.y1 - hu.y0);
      hp[2 * i] = {hu.x0, y};
      hp[2 * i + 1] = {hu.x1, y};
      hc[2 * i] = hc[2 * i + 1] = hsv(static_cast<float>(i % 6) / 6.0f, 1.0f, 1.0f);
    }
    for (uint32_t i = 0, n = 0; i < 6; ++i)
      for (const uint32_t x : {2 * i, 2 * i + 1, 2 * i + 3, 2 * i, 2 * i + 3, 2 * i + 2}) hi[n++] = x;
    p.mesh(hp, hc, hi);
    const float hy = hu.y0 + pick_.h * (hu.y1 - hu.y0);
    p.roundedRect({hu.x0 - 1.0f, hy - 1.0f, hu.x1 + 1.0f, hy + 1.0f}, 0.5f, ui::rgba(255, 255, 255));
    p.roundedRect({hu.x0, hy - 0.4f, hu.x1, hy + 0.4f}, 0.0f, ui::rgba(0, 0, 0));
    // New over old (a press on the old puts it back).
    const ui::Rect nw = newRect(), od = oldRect();
    p.roundedRect(nw, 1.5f, pick_.rgb);
    if (hovered == kPickOld) p.roundedRect({od.x0 - 0.75f, od.y0 - 0.75f, od.x1 + 0.75f, od.y1 + 0.75f}, 2.0f, th.face.value);
    p.roundedRect(od, 1.5f, ui::withAlpha(resolveColour(pick_.orig, v.fallbackColour(pick_.opt)), 255));
    if (nw.x1 + 2.0f + 2.0f * px_ < b.x1) {
      p.textAligned({nw.x1 + 2.0f, centre(nw).y}, "new", px_ * 0.85f, dim, 0.0f);
      p.textAligned({od.x1 + 2.0f, centre(od).y}, "old", px_ * 0.85f, dim, 0.0f);
    }
  }
  static bool in(const ui::Rect& r, ui::Vec2 p) noexcept { return p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1; }
  static ui::Vec2 centre(const ui::Rect& r) noexcept { return {0.5f * (r.x0 + r.x1), 0.5f * (r.y0 + r.y1)}; }
  ui::Rect over_, area_;
  float px_;
  Place place_ = Place::Over;
  mutable Pick pick_;
};

// ---- VZ.10: the picker's share of a face's events (the face forwards; true: consumed) ----------------------------------

// What the picker asks of its face: set an option (live), and an edit gesture on the instance's data (a drag on the
// square or the strip is one undo step; a set outside a gesture is a step of its own).
class PickerHost {
public:
  virtual void pickSet(uint32_t o, double v) = 0;
  virtual void pickBegin(const char* label) = 0;
  virtual void pickEnd() = 0;
  virtual void pickCancel() = 0;  // the gesture's edits undone, the options re-read

protected:
  ~PickerHost() = default;
};

// A press on a picker part (isPicker).
inline bool pickerDown(Sheet& s, const Values& v, PickerHost& h, int32_t part, ui::Vec2 l) {
  const std::optional<double> val = s.pickPress(part, l, v);
  if (s.picking()) h.pickBegin(v.table()[s.pickerOpt()].label);
  if (val && *val != v[s.pickerOpt()]) h.pickSet(s.pickerOpt(), *val);
  return true;
}
inline bool pickerMove(Sheet& s, const Values& v, PickerHost& h, ui::Vec2 l) {
  if (!s.picking()) return false;
  const double val = s.pickDrag(l);
  if (val != v[s.pickerOpt()]) h.pickSet(s.pickerOpt(), val);
  return true;
}
inline bool pickerUp(Sheet& s, PickerHost& h) {
  if (!s.picking()) return false;
  s.pickRelease();
  h.pickEnd();
  return true;
}
inline bool pickerCancel(Sheet& s, PickerHost& h) {
  if (!s.picking()) return false;
  s.pickRelease();
  h.pickCancel();
  return true;
}
// Keys while the picker is open: typing in the hex field (Enter applies, Backspace, Esc stops typing; every other key is
// the field's), else Esc restores the colour the picker opened with and closes it.
inline bool pickerKey(Sheet& s, const Values& v, PickerHost& h, const ui::KeyEvent& e) {
  if (!s.pickerOpen()) return false;
  if (s.typing()) {
    if (!e.down) return true;
    if (e.key == ui::key::kEscape) s.stopTyping();
    else if (e.key == ui::key::kBackspace) s.typeBackspace();
    else if (e.key == ui::key::kReturn || e.key == ui::keyFromScancode(ui::Scancode::KpEnter)) {
      if (const std::optional<double> c = s.typedColour(); c && *c != v[s.pickerOpt()]) h.pickSet(s.pickerOpt(), *c);
    }
    return true;
  }
  if (!e.down || e.key != ui::key::kEscape) return false;
  if (s.picking()) pickerCancel(s, h);
  const uint32_t o = s.pickerOpt();
  const double orig = s.pickerOrigin();
  s.closePicker();
  if (v[o] != orig) h.pickSet(o, orig);
  return true;
}
// The picker parts' tooltip.
inline bool pickerDescribe(int32_t part, std::string& title, std::string& body) {
  switch (part) {
    case kPickSv: title = "Saturation / value"; body = "Drag: saturation left to right, brightness bottom to top (live; one undo step)."; return true;
    case kPickHue: title = "Hue"; body = "Drag to choose the hue (live; one undo step)."; return true;
    case kPickOld: title = "Old colour"; body = "Click: back to the colour the picker opened with."; return true;
    case kPickHex: title = "Hex"; body = "Click and type a colour (#RGB or #RRGGBB), Enter to apply."; return true;
    case kPickBox: title = "Colour"; body = "Esc: back to the colour it opened with, and close. A click elsewhere keeps it."; return true;
    default: return false;
  }
}

// The type's saved default as options (false: none saved).
inline bool savedDefault(std::string_view type, Values& out) {
  const meta::DataPtr d = meta::userDefaultData(type);
  if (d == nullptr) return false;
  out.load(d.get());
  return true;
}

}  // namespace pq::views::sheet
