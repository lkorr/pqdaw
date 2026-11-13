#pragma once

// The form rows of the Preferences window (T3.18), shared with the Export Audio window (EX.2): a section title over a
// 1-px divider, and a row = a secondary label of fixed width followed by its controls. Private to src/views.

#include "pq/ui/controls.h"
#include "pq/ui/theme.h"
#include "pq/ui/widget.h"

namespace pq::views::form {

inline constexpr float kLabelWidth = 190.0f;
inline constexpr float kRowHeight = 22.0f;

// A 1-device-px line under a section title.
class Divider : public ui::Widget {
public:
  Divider() : ui::Widget() { setHeight(ui::Length::fixed(1)); }
  void paint(ui::PaintContext& ctx) override {
    if (const ui::Theme* th = ctx.res.theme)
      ctx.dl.addRectFilled(ui::IRect{rect().x, rect().y, rect().w, 1}.toRect(), th->surface.divider);
  }
};

inline ui::Widget& section(ui::Widget& parent, const char* name, const char* title) {
  auto& w = parent.add<ui::Widget>(name);
  w.setLayout(ui::LayoutKind::Column);
  w.setHeight(ui::Length::fixed(21));
  w.setGap(0);
  auto& l = w.add<ui::Label>("title", title, ui::FontRoleId::BodyStrong, ui::TextTone::Primary);
  l.setHeight(ui::Length::fixed(20));
  w.add<Divider>();
  return w;
}

inline ui::Widget& row(ui::Widget& parent, const char* name, const char* label, ui::Label** labelOut = nullptr) {
  auto& r = parent.add<ui::Widget>(name);
  r.setLayout(ui::LayoutKind::Row);
  r.setHeight(ui::Length::fixed(kRowHeight));
  r.setGap(8);
  auto& l = r.add<ui::Label>("label", label, ui::FontRoleId::Small, ui::TextTone::Secondary);
  l.setWidth(ui::Length::fixed(kLabelWidth));
  if (labelOut) *labelOut = &l;
  return r;
}

}  // namespace pq::views::form
