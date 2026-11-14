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
// UI thread only.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "pq/meta/data.h"
#include "pq/meta/user_defaults.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/theme.h"

namespace pq::views::sheet {

// One option: its data field, label, range, default and how a value reads.
struct Opt {
  const char* key;
  const char* label;
  double def, lo, hi;
  enum Kind : uint8_t { Choice, Toggle, Number, LogNumber, Pow2 } kind;
  const char* const* names = nullptr;  // Choice / Toggle
  const char* fmt = "%.2f";            // Number
  double step = 0.0;                   // a quantum (0: continuous)
};

inline constexpr const char* kOnOff[] = {"Off", "On"};

// The values of a table of options.
class Values {
public:
  explicit Values(std::span<const Opt> table) : t_(table), v_(table.size()) { reset(); }
  void reset() {
    for (size_t i = 0; i < t_.size(); ++i) v_[i] = t_[i].def;
  }
  std::span<const Opt> table() const noexcept { return t_; }
  double operator[](uint32_t i) const noexcept { return v_[i]; }
  void set(uint32_t i, double v) noexcept { v_[i] = std::clamp(v, t_[i].lo, t_[i].hi); }
  int choice(uint32_t i) const noexcept { return static_cast<int>(std::lround(v_[i])); }
  bool on(uint32_t i) const noexcept { return v_[i] >= 0.5; }
  bool isDefault(uint32_t i) const noexcept { return v_[i] == t_[i].def; }
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
    }
  }
  bool operator==(const Values& o) const noexcept { return v_ == o.v_; }

private:
  std::span<const Opt> t_;
  std::vector<double> v_;
};

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

// The sheet's parts (a face's own parts stay below kTabBase).
enum Part : int32_t { kTabBase = 90, kSave = 92, kReset = 93, kBody = 94, kRowBase = 100 };
enum Tab : int { kAnalysis = 0, kDisplay = 1 };
inline constexpr const char* kTabNames[] = {"ANALYSIS", "DISPLAY"};

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

  // BR.ui: placed over the plot (the area it was made with) or beside a face `faceW` × `faceH`, as wide as the face.
  void place(Place where, float faceW, float faceH) noexcept {
    place_ = where;
    if (where == Place::Right) area_ = {faceW + kSideGap, 0.0f, 2.0f * faceW + kSideGap, faceH};
    else if (where == Place::Left) area_ = {-kSideGap - faceW, 0.0f, -kSideGap, faceH};
    else area_ = over_;
  }
  Place placed() const noexcept { return place_; }
  bool beside() const noexcept { return place_ != Place::Over; }
  ui::Rect area() const noexcept { return area_; }
  ui::Rect tabRect(int t) const noexcept {
    const float w = px_ * 7.6f;
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
  // The part under l (kRowBase + i: row i of n), else kBody inside the sheet, else -1.
  int32_t hit(ui::Vec2 l, size_t n) const noexcept {
    for (int t = 0; t < 2; ++t)
      if (in(tabRect(t), l)) return kTabBase + t;
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
    for (int t = 0; t < 2; ++t) {
      const ui::Rect r = tabRect(t);
      const bool on = tab == t;
      p.roundedRect(r, 2.5f, on ? ui::mulAlpha(th.face.value, 0.3f) : (hovered == kTabBase + t ? ui::rgba(40, 44, 52) : ui::rgba(22, 24, 30)));
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
      if (hovered == kRowBase + static_cast<int32_t>(i)) p.roundedRect(rr, 2.0f, ui::mulAlpha(th.face.value, 0.18f));
      const float cy = 0.5f * (rr.y0 + rr.y1);
      const uint32_t o = rows[i];
      p.textAligned({rr.x0 + 2.0f, cy}, v.table()[o].label, px_, text, 0.0f);
      valueText(v, o, buf, sizeof buf);
      p.textAligned({rr.x1 - 2.0f, cy}, buf, px_, v.isDefault(o) ? dim : th.face.value, 1.0f);
    }
  }

private:
  float headH() const noexcept { return px_ + 4.0f; }
  static bool in(const ui::Rect& r, ui::Vec2 p) noexcept { return p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1; }
  static ui::Vec2 centre(const ui::Rect& r) noexcept { return {0.5f * (r.x0 + r.x1), 0.5f * (r.y0 + r.y1)}; }
  ui::Rect over_, area_;
  float px_;
  Place place_ = Place::Over;
};

// The type's saved default as options (false: none saved).
inline bool savedDefault(std::string_view type, Values& out) {
  const meta::DataPtr d = meta::userDefaultData(type);
  if (d == nullptr) return false;
  out.load(d.get());
  return true;
}

}  // namespace pq::views::sheet
