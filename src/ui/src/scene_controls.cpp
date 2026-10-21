// The T3.2 `widgets` scene: every base control in normal / hover / pressed / focused / disabled (forced with
// Control::setStateOverride), a live mixer strip (toggles, fader, PQ Meter-style meters, knobs with a
// modulation ring, drag box, dropdown), the clip colour palette, and the overlays: a context menu with an open
// submenu, an anchored popover and a tooltip. Everything is themed from assets/themes/ableton-dark.json.
// The same tree runs live in the window (`pqdaw --scene widgets`); the overrides only change painting.

#include <algorithm>
#include <cstdio>
#include <string>

#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/scenes.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/widget.h"
#include "controls_paint.h"
#include "scenes_internal.h"

namespace pq::ui {

namespace {

// Window background (the gaps between panels).
class Backdrop : public Widget {
public:
  Backdrop() { setLayout(LayoutKind::Column); }
  void paint(PaintContext& ctx) override {
    if (const Theme* th = ctx.res.theme) ctx.dl.addRectFilled(rect().toRect(), th->surface.background);
  }
};

// Header strip: raised fill, a dark divider and a highlight line below it, title and theme name.
class Header : public Widget {
public:
  Header() : Widget("header") { setHeight(Length::fixed(30)); }
  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    if (!th) return;
    const IRect r = rect();
    ctx.dl.addRectFilled(r.toRect(), th->surface.raised);
    ctx.dl.addHLine(static_cast<float>(r.x), static_cast<float>(r.right()), static_cast<float>(r.bottom() - 1), th->surface.divider);
    const ThemeFonts* f = ctx.res.uiFonts;
    if (!f) return;
    detail::drawText(ctx, f->title, r, "pqdaw \xC2\xB7 widgets", th->text.primary, TextAlign::Left, ctx.scale.px(12), 0);
    detail::drawText(ctx, f->small, r, th->name, th->text.secondary, TextAlign::Right, 0, ctx.scale.px(12));
  }
};

// A panel: fill, 1 px divider border, a small title at the top-left. Children go below the title (Column).
class Panel : public Widget {
public:
  Panel(std::string name, std::string title) : Widget(std::move(name)), title_(std::move(title)) {
    setLayout(LayoutKind::Column);
    setPadding({10, 28, 10, 10});
    setGap(6);
  }
  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    if (!th) return;
    const IRect r = rect();
    detail::box(ctx.dl, r, ctx.scale.exact(th->radius.panel), th->surface.panel, th->surface.divider);
    if (const ThemeFonts* f = ctx.res.uiFonts)
      detail::drawText(ctx, f->bodyStrong, IRect{r.x, r.y + ctx.scale.px(4), r.w, ctx.scale.px(18)}, title_, th->text.primary, TextAlign::Left,
                       ctx.scale.px(10), ctx.scale.px(10));
  }

private:
  std::string title_;
};

// Fixed-size cell that centres one control.
Widget& cell(Widget& row, std::string name, float width) {
  auto& c = row.add<Widget>(std::move(name));
  c.setWidth(Length::fixed(width));
  c.setAlign(Align::Center, Align::Center);
  return c;
}

// Lane stripes for the scrollbar demo views.
class Stripes : public Widget {
public:
  Stripes() : Widget("content") { setSize(Length::fixed(84), Length::fixed(200)); }
  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    if (!th) return;
    const IRect r = rect();
    const int32_t h = ctx.scale.px(10);
    for (int32_t i = 0, y = r.y; y < r.bottom(); ++i, y += h)
      ctx.dl.addRectFilled(IRect{r.x, y, r.w, std::min(h, r.bottom() - y)}.toRect(), (i & 1) ? th->track.laneAlt : th->track.lane);
  }
};

// The clip colour chooser grid (14 × 5), one swatch selected.
class Palette : public Widget {
public:
  Palette() : Widget("palette") {}
  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    if (!th || th->clipPalette.size() != kClipPaletteSize) return;
    const IRect r = rect();
    const int32_t gap = ctx.scale.px(2);
    constexpr auto cols = static_cast<int32_t>(kClipPaletteColumns), rows = static_cast<int32_t>(kClipPaletteRows);
    const int32_t w = (r.w - gap * (cols - 1)) / cols;
    const int32_t h = std::min(ctx.scale.px(14), (r.h - gap * (rows - 1)) / rows);
    for (uint32_t row = 0; row < kClipPaletteRows; ++row)
      for (uint32_t col = 0; col < kClipPaletteColumns; ++col) {
        const IRect s{r.x + static_cast<int32_t>(col) * (w + gap), r.y + static_cast<int32_t>(row) * (h + gap), w, h};
        const bool sel = row == 0 && col == 2;
        detail::box(ctx.dl, s, 0.0f, th->clipPalette[row * kClipPaletteColumns + col], sel ? th->clip.selectedBorder : th->clip.border);
      }
  }
};

// Right-click opens the demo context menu (live window).
std::vector<MenuItem> demoMenu();
class MenuArea : public Widget {
public:
  MenuArea() : Widget("area") {}
  bool onPointer(const PointerEvent& e) override {
    if (e.type != PointerType::Down || e.button != PointerButton::Right) return false;
    openContextMenu(*this, e.pos, demoMenu());
    return true;
  }
};

std::vector<MenuItem> demoMenu() {
  std::vector<MenuItem> grid = {
      MenuItem::item("Narrowest"), MenuItem::item("Narrow"), MenuItem::item("Medium"), MenuItem::item("Wide"), MenuItem::item("Widest"),
      MenuItem::sep(),
      MenuItem::sub("Tuplet", {MenuItem::item("Straight"), MenuItem::item("Triplet"), MenuItem::item("5-tuplet"), MenuItem::item("7-tuplet"),
                               MenuItem::item("11-tuplet")}),
      MenuItem::item("Snap to Grid", {}, "Ctrl+4"),
      MenuItem::item("Fixed Grid", {}, "Ctrl+5"),
  };
  grid[2].checked = true;
  grid[7].checked = true;
  std::vector<MenuItem> m = {
      MenuItem::item("Cut", {}, "Ctrl+X"),
      MenuItem::item("Copy", {}, "Ctrl+C"),
      MenuItem::item("Paste", {}, "Ctrl+V"),
      MenuItem::item("Duplicate", {}, "Ctrl+D"),
      MenuItem::item("Delete", {}, "Del"),
      MenuItem::sep(),
      MenuItem::item("Rename", {}, "Ctrl+R"),
      MenuItem::sub("Grid", std::move(grid)),
      MenuItem::sub("Colour", {MenuItem::item("Red"), MenuItem::item("Amber"), MenuItem::item("Teal")}),
  };
  m[2].enabled = false;  // nothing to paste
  return m;
}

constexpr const char* kStateNames[5] = {"Normal", "Hover", "Pressed", "Focused", "Disabled"};
constexpr uint8_t kStateBits[5] = {vis::kNormal, vis::kHover, vis::kHover | vis::kPressed, vis::kFocus, vis::kDisabled};

void applyState(Control& c, int col) {
  c.setStateOverride(kStateBits[col]);
  if (col == 4) c.setEnabled(false);
}

ValueSpec tempoSpec() {
  ValueSpec s;
  s.min = 20;
  s.max = 999;
  s.def = 120;
  s.step = 0.01;
  s.dragPixels = 600;
  s.decimals = 2;
  return s;
}
ValueSpec volumeSpec() {
  ValueSpec s;
  s.min = -70;
  s.max = 6;
  s.def = 0;
  s.dragPixels = 200;
  s.decimals = 1;
  s.unit = "dB";
  return s;
}
ValueSpec unitSpec(double def) {
  ValueSpec s;
  s.def = def;
  s.decimals = 2;
  return s;
}

void buildMatrix(Panel& panel, DragBox*& editingBox) {
  constexpr float kRowLabel = 86, kCell = 128;
  auto& head = panel.add<Widget>("states");
  head.setLayout(LayoutKind::Row).setHeight(Length::fixed(16));
  head.add<Widget>("corner").setWidth(Length::fixed(kRowLabel));
  for (int c = 0; c < 5; ++c)
    head.add<Label>(kStateNames[c], kStateNames[c], FontRoleId::Small, TextTone::Secondary, TextAlign::Center).setWidth(Length::fixed(kCell));

  auto row = [&](const char* name, const char* label, float height) -> Widget& {
    auto& r = panel.add<Widget>(name);
    r.setLayout(LayoutKind::Row).setHeight(Length::fixed(height)).setAlign(Align::Start, Align::Center);
    r.add<Label>("name", label, FontRoleId::Small, TextTone::Secondary).setSize(Length::fixed(kRowLabel), Length::fixed(18));
    return r;
  };
  char nm[16];
  {
    Widget& r = row("label", "Label", 26);
    for (int c = 0; c < 5; ++c) {
      std::snprintf(nm, sizeof nm, "c%d", c);
      auto& l = cell(r, nm, kCell).add<Label>("ctl", "Track Volume", FontRoleId::Small, TextTone::Primary, TextAlign::Center);
      l.setSize(Length::fixed(110), Length::fixed(18));
      applyState(l, c);
    }
  }
  {
    Widget& r = row("button", "Button", 28);
    for (int c = 0; c < 5; ++c) {
      std::snprintf(nm, sizeof nm, "c%d", c);
      auto& b = cell(r, nm, kCell).add<Button>("ctl", "Freeze");
      b.setSize(Length::fixed(76), Length::fixed(18));
      applyState(b, c);
    }
  }
  for (int on = 0; on < 2; ++on) {
    Widget& r = row(on ? "toggles-on" : "toggles-off", on ? "Toggles (on)" : "Toggles (off)", 28);
    for (int c = 0; c < 5; ++c) {
      std::snprintf(nm, sizeof nm, "c%d", c);
      auto& g = cell(r, nm, kCell).add<Widget>("group");
      g.setLayout(LayoutKind::Row).setGap(3).setSize(Length::fixed(22 + 18 + 18 + 40 + 9), Length::fixed(18));
      Toggle* t[4] = {&g.add<Toggle>("activator", "1", ToggleStyle::Activator, on != 0), &g.add<Toggle>("solo", "S", ToggleStyle::Solo, on != 0),
                      &g.add<Toggle>("arm", "", ToggleStyle::Arm, on != 0), &g.add<Toggle>("loop", "Loop", ToggleStyle::Accent, on != 0)};
      t[0]->setWidth(Length::fixed(22));
      t[1]->setWidth(Length::fixed(18));
      t[2]->setWidth(Length::fixed(18));
      t[3]->setWidth(Length::fixed(40));
      for (Toggle* x : t) applyState(*x, c);
    }
  }
  {
    Widget& r = row("dragbox", "Drag box", 28);
    for (int c = 0; c < 5; ++c) {
      std::snprintf(nm, sizeof nm, "c%d", c);
      auto& d = cell(r, nm, kCell).add<DragBox>("ctl", tempoSpec());
      d.setSize(Length::fixed(64), Length::fixed(18));
      d.setValue(128.0);
      applyState(d, c);
      if (c == 3) editingBox = &d;  // shown in type-in mode (focused)
    }
  }
  {
    Widget& r = row("fader", "Fader", 28);
    for (int c = 0; c < 5; ++c) {
      std::snprintf(nm, sizeof nm, "c%d", c);
      auto& f = cell(r, nm, kCell).add<Fader>("ctl", volumeSpec(), Fader::Orientation::Horizontal);
      f.setSize(Length::fixed(110), Length::fixed(16));
      f.setValue(-6.0);
      applyState(f, c);
    }
  }
  {
    Widget& r = row("knob", "Knob", 44);
    for (int c = 0; c < 5; ++c) {
      std::snprintf(nm, sizeof nm, "c%d", c);
      auto& k = cell(r, nm, kCell).add<Knob>("ctl", unitSpec(0.5));
      k.setSize(Length::fixed(40), Length::fixed(40));
      k.setValue(0.62);
      k.setModulation({true, 0.45f, 0.85f, 0.74f});
      applyState(k, c);
    }
  }
  {
    Widget& r = row("textedit", "Text edit", 28);
    for (int c = 0; c < 5; ++c) {
      std::snprintf(nm, sizeof nm, "c%d", c);
      auto& t = cell(r, nm, kCell).add<TextEdit>("ctl", "Bass Track");
      t.setSize(Length::fixed(110), Length::fixed(18));
      if (c == 3) t.buffer().selectWordAt(6);  // "Track" selected, caret at its end
      applyState(t, c);
    }
  }
  {
    Widget& r = row("dropdown", "Dropdown", 28);
    for (int c = 0; c < 5; ++c) {
      std::snprintf(nm, sizeof nm, "c%d", c);
      auto& d = cell(r, nm, kCell).add<Dropdown>("ctl", std::vector<std::string>{"Adaptive", "1/16", "1/8", "Off"});
      d.setSize(Length::fixed(104), Length::fixed(18));
      applyState(d, c);
    }
  }
  {
    Widget& r = row("scrollbar", "Scrollbar", 44);
    for (int c = 0; c < 5; ++c) {
      std::snprintf(nm, sizeof nm, "c%d", c);
      auto& g = cell(r, nm, kCell).add<Widget>("group");
      g.setLayout(LayoutKind::Row).setGap(2).setSize(Length::fixed(96), Length::fixed(40));
      auto& view = g.add<ScrollView>("view", false, true);
      view.add<Stripes>();
      auto& sb = g.add<Scrollbar>("bar", view, Scrollbar::Axis::Vertical);
      sb.setWidth(Length::fixed(10));
      applyState(sb, c);
    }
  }
}

void buildMixer(Panel& p) {
  auto& head = p.add<Widget>("head");
  head.setLayout(LayoutKind::Row).setGap(3).setHeight(Length::fixed(18));
  auto& act = head.add<Toggle>("activator", "1", ToggleStyle::Activator, true);
  act.setWidth(Length::fixed(22));
  act.setInfo("Track Activator", "Turns the track on or off.");
  auto& solo = head.add<Toggle>("solo", "S", ToggleStyle::Solo, false);
  solo.setWidth(Length::fixed(18));
  solo.setTooltip("Solo (Ctrl-click: add to solo)");
  head.add<Toggle>("arm", "", ToggleStyle::Arm, true).setWidth(Length::fixed(18));
  head.add<Widget>("gap").setWidth(Length::fixed(8));
  head.add<Dropdown>("out", std::vector<std::string>{"Master", "Sends Only", "1-Bass"}).setWidth(Length::flex(1));

  p.add<Widget>("tip-room").setHeight(Length::fixed(20));  // where the Solo tooltip shows
  auto& strip = p.add<Widget>("strip");
  strip.setLayout(LayoutKind::Row).setGap(6).setHeight(Length::fixed(150));
  auto& fader = strip.add<Fader>("volume", volumeSpec(), Fader::Orientation::Vertical);
  fader.setWidth(Length::fixed(22));
  fader.setValue(-6.0);
  auto meter = [&](const char* name, MeterLevels l) {
    auto& m = strip.add<Meter>(name);
    m.setWidth(Length::fixed(11));
    m.setLevels(l);
  };
  MeterLevels quiet;
  quiet.rmsDb[0] = -24.0f, quiet.rmsDb[1] = -26.5f;
  quiet.peakDb[0] = -19.0f, quiet.peakDb[1] = -21.0f;
  quiet.holdDb[0] = -15.0f, quiet.holdDb[1] = -16.5f;
  MeterLevels warm;
  warm.rmsDb[0] = -14.0f, warm.rmsDb[1] = -16.0f;
  warm.peakDb[0] = -9.0f, warm.peakDb[1] = -10.5f;
  warm.holdDb[0] = -7.5f, warm.holdDb[1] = -8.0f;
  MeterLevels hot;
  hot.rmsDb[0] = -4.5f, hot.rmsDb[1] = -5.0f;
  hot.peakDb[0] = -0.4f, hot.peakDb[1] = -1.2f;
  hot.holdDb[0] = 0.0f, hot.holdDb[1] = -0.6f;
  hot.clip[0] = true;
  meter("meter-quiet", quiet);
  meter("meter-warm", warm);
  meter("meter-hot", hot);
  auto& knobs = strip.add<Widget>("knobs");
  knobs.setLayout(LayoutKind::Column).setGap(2);
  ValueSpec pan;
  pan.min = -50, pan.max = 50, pan.def = 0, pan.step = 1, pan.decimals = 0;
  pan.dragPixels = 150;
  auto& k1 = knobs.add<Knob>("pan", pan);
  k1.setHeight(Length::fixed(40));
  k1.setValue(-18);
  k1.setInfo("Track Pan", "Pans the track. Delete resets to centre.");
  knobs.add<Label>("pan-label", "Pan", FontRoleId::Small, TextTone::Secondary, TextAlign::Center).setHeight(Length::fixed(14));
  auto& k2 = knobs.add<Knob>("send", unitSpec(0.0));
  k2.setHeight(Length::fixed(40));
  k2.setValue(0.4);
  k2.setModulation({true, 0.25f, 0.7f, 0.55f});
  knobs.add<Label>("send-label", "Send A", FontRoleId::Small, TextTone::Secondary, TextAlign::Center).setHeight(Length::fixed(14));
  auto& tempo = knobs.add<DragBox>("tempo", tempoSpec());
  tempo.setHeight(Length::fixed(18));
  knobs.add<Label>("tempo-label", "BPM", FontRoleId::Small, TextTone::Secondary, TextAlign::Center).setHeight(Length::fixed(14));
}

}  // namespace

void buildWidgetsGallery(UiTree& tree) {
  auto root = std::make_unique<Widget>();  // Stack: the app, then the overlay layer on top
  auto& app = root->add<Backdrop>();
  app.add<Header>();
  auto& body = app.add<Widget>("body");
  body.setLayout(LayoutKind::Row).setPadding(Insets::all(12)).setGap(12);

  auto& left = body.add<Widget>("left");
  left.setLayout(LayoutKind::Column).setGap(12);
  auto& matrix = left.add<Panel>("states", "Controls \xC2\xB7 states");
  matrix.setHeight(Length::fixed(392)).setGap(2);
  DragBox* editing = nullptr;
  buildMatrix(matrix, editing);
  auto& bottom = left.add<Widget>("bottom");
  bottom.setLayout(LayoutKind::Row).setGap(12);
  auto& colours = bottom.add<Panel>("colours", "Clip colours");
  colours.setWidth(Length::fixed(300));
  colours.add<Palette>().setHeight(Length::fixed(78));
  auto& menus = bottom.add<Panel>("menus", "Menus (right-click)");
  menus.add<MenuArea>();

  auto& right = body.add<Widget>("right");
  right.setLayout(LayoutKind::Column).setGap(12).setWidth(Length::fixed(232));
  auto& mixer = right.add<Panel>("mixer", "1 Bass");
  mixer.setHeight(Length::fixed(258));
  buildMixer(mixer);
  auto& pop = right.add<Panel>("popover", "Popover");
  auto& anchorRow = pop.add<Widget>("row");
  anchorRow.setLayout(LayoutKind::Row).setHeight(Length::fixed(18));
  auto& tuning = anchorRow.add<Button>("tuning", "Tuning\xE2\x80\xA6");
  tuning.setWidth(Length::fixed(84));

  auto& overlay = root->add<OverlayLayer>("overlay");
  tree.setRoot(std::move(root));
  tree.layoutNow();
  for (int c = 0; c < 5; ++c) {  // scrolled part-way, so the thumbs sit mid-track
    char path[64];
    std::snprintf(path, sizeof path, "body/left/states/scrollbar/c%d/group/view", c);
    if (auto* v = dynamic_cast<ScrollView*>(tree.find(path))) v->scrollTo(0, tree.uiScale().px(60));
  }
  tree.layoutNow();

  // Popover anchored under the button (Esc or a press outside closes it). Re-opened by the button.
  auto openPopover = [&tree, &overlay, &tuning] {
    auto p = std::make_unique<Popover>("tuning-popover");
    p->setSize(Length::fixed(196), Length::fixed(104));
    p->add<Label>("title", "Clip tuning", FontRoleId::Small, TextTone::Secondary).setHeight(Length::fixed(14));
    p->add<Dropdown>("tuning", std::vector<std::string>{"31-EDO", "12-EDO", "5L 2s (MOS)", "Scala\xE2\x80\xA6"}).setHeight(Length::fixed(18));
    auto& row = p->add<Widget>("row");
    row.setLayout(LayoutKind::Row).setGap(6).setHeight(Length::fixed(18));
    ValueSpec cents;
    cents.min = -50, cents.max = 50, cents.def = 0, cents.decimals = 1, cents.unit = "ct";
    row.add<DragBox>("offset", cents).setWidth(Length::fixed(70));
    row.add<Toggle>("snap", "Snap", ToggleStyle::Accent, true).setWidth(Length::fixed(44));
    OverlayOptions o;
    o.anchor = tuning.rect();
    o.placement = Placement::Below;
    overlay.open(std::move(p), std::move(o));
    (void)tree;
  };
  tuning.onClick = openPopover;

  // The overlays as they look open. Menu: opened at a point in the Menus panel, "Grid" submenu open.
  const IRect area = tree.find("body/left/bottom/menus/area") ? tree.find("body/left/bottom/menus/area")->rect() : IRect{};
  if (Menu* m = openContextMenu(*tree.find("body/left/bottom/menus/area"), {static_cast<float>(area.x + tree.uiScale().px(6)),
                                                                            static_cast<float>(area.y + tree.uiScale().px(2))},
                                demoMenu())) {
    tree.layoutNow();
    m->setHighlighted(7);
    if (Menu* sub = m->openSubmenu()) {
      tree.layoutNow();
      sub->setHighlighted(2);
    }
  }
  openPopover();
  tree.layoutNow();
  if (Widget* solo = tree.find("body/right/mixer/head/solo")) {
    const IRect s = solo->rect();
    overlay.showTooltip(solo->tooltip(), {static_cast<float>(s.x + s.w / 2), static_cast<float>(s.bottom() + tree.uiScale().px(4))});
  }
  if (editing) editing->beginEdit();  // last: it takes focus
  tree.layoutNow();
}

namespace detail {

void controlsScene(DrawList& dl, const SceneTextures& tex, double time) {
  UiTree tree;
  tree.setViewport(dl.targetWidth(), dl.targetHeight(), tex.scale);
  const PaintResources res = paintResources(tex);
  tree.setResources(&res);
  buildWidgetsGallery(tree);
  tree.paint(dl, res, time);
}

}  // namespace detail

}  // namespace pq::ui
