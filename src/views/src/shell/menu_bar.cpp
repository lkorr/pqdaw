// The menu bar (T3.19, M3 review H7): File / Edit / Create / View / Options / Help, Live's structure and names, every
// item an action of the context's registry. See shell_widgets.h.

#include <algorithm>
#include <cmath>

#include "../common/paint.h"
#include "pq/ui/theme.h"
#include "shell_widgets.h"

namespace pq::views::shell {

using namespace pq::ui;

namespace {

constexpr float kTitlePad = 8.0f;  // logical px each side of a title
constexpr float kBarInset = 4.0f;  // logical px before the first title

// Whether the focus is inside the clip view (Edit acts on the focused editor).
bool focusInClipView(const UiTree* tree) {
  if (!tree) return false;
  for (const Widget* w = tree->focus(); w; w = w->parent())
    if (w->name() == "clip-view") return true;
  return false;
}

}  // namespace

MenuBar::MenuBar(ViewContext& ctx) : Widget("menu-bar"), ctx_(ctx), alive_(std::make_shared<MenuBar*>(this)) {
  setHeight(Length::fixed(20));
  setFocusable(false);  // the focused editor keeps the focus, so Edit knows what it acts on
}

// The open menu's callbacks reach the bar through alive_ (null from here on). The menu is not closed here: during the
// tree's teardown the overlay layer may already be half destroyed, and it closes (and forgets) its popups itself.
MenuBar::~MenuBar() { *alive_ = nullptr; }

IRect MenuBar::titleRect(int32_t i) const {
  if (i < 0 || i >= kMenus) return {};
  const UiTree* t = tree();
  const PaintResources* res = t ? t->resources() : nullptr;
  const UiScale s = t ? t->uiScale() : UiScale{};
  const IRect r = rect();
  int32_t x = r.x + s.px(kBarInset);
  for (int32_t k = 0; k <= i; ++k) {
    const std::string_view title = kTitles[k];
    const int32_t textW = res && res->text && res->uiFonts ? res->text->measure(res->uiFonts->body, title)
                                                           : static_cast<int32_t>(title.size()) * s.px(7);
    const int32_t w = textW + 2 * s.px(kTitlePad);
    if (k == i) return {x, r.y, w, r.h};
    x += w;
  }
  return {};
}

int32_t MenuBar::titleAt(Vec2 p) const {
  if (!rect().contains(p)) return -1;
  for (int32_t i = 0; i < kMenus; ++i)
    if (titleRect(i).contains(p)) return i;
  return -1;
}

void MenuBar::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  ctx.dl.addRectFilled(r.toRect(), th->surface.background);
  const ThemeFonts* f = ctx.res.uiFonts;
  if (!f) return;
  for (int32_t i = 0; i < kMenus; ++i) {
    const IRect tr = titleRect(i);
    if (intersect(tr, ctx.clip).empty()) continue;
    Rgba text = th->text.primary;
    if (i == open_) {
      ctx.dl.addRectFilled(tr.toRect(), th->selection.fill);
      text = th->selection.text;
    } else if (i == hover_) {
      ctx.dl.addRectFilled(tr.toRect(), th->control.fill);
    }
    paint::text(ctx, f->body, tr, kTitles[i], text, TextAlign::Center, 0, 0);
  }
}

bool MenuBar::onPointer(const PointerEvent& e) {
  const int32_t i = titleAt(e.pos);
  switch (e.type) {
    case PointerType::Move:
      if (i != hover_) {
        hover_ = i;
        markDirty();
      }
      return i >= 0;
    case PointerType::Down:
      if (e.button != PointerButton::Left || i < 0) return false;
      (void)open(i);
      return true;
    case PointerType::Up: return i >= 0;
    default: return false;
  }
}

void MenuBar::onHoverChanged(bool hovered) {
  if (!hovered && hover_ >= 0) {
    hover_ = -1;
    markDirty();
  }
}

// While a menu is open the overlay layer receives the pointer: moving over another title switches to its menu, a click
// on the open title closes it, a click on another title opens that one; anything else dismisses as usual.
bool MenuBar::outside(const PointerEvent& e) {
  const int32_t i = titleAt(e.pos);
  if (i < 0) return false;
  if (e.type == PointerType::Move) {
    if (i != open_) (void)open(i);
    return true;
  }
  if (e.type == PointerType::Down) {
    if (i == open_) close();
    else (void)open(i);
    return true;
  }
  return false;
}

Menu* MenuBar::open(int32_t i) {
  if (i < 0 || i >= kMenus) return nullptr;
  OverlayLayer* layer = OverlayLayer::of(*this);
  if (!layer) return nullptr;
  close();
  auto menu = std::make_unique<Menu>(items(i), "menu-bar-menu");
  menu->setMinWidthPx(titleRect(i).w);
  Menu* raw = menu.get();
  OverlayOptions o;
  o.anchor = titleRect(i);
  o.placement = Placement::Below;
  std::shared_ptr<MenuBar*> alive = alive_;
  o.onClosed = [alive, raw] {
    MenuBar* bar = *alive;
    if (!bar || bar->menu_ != raw) return;
    bar->menu_ = nullptr;
    bar->open_ = -1;
    bar->markDirty();
  };
  o.onOutsidePointer = [alive](const PointerEvent& ev) {
    MenuBar* bar = *alive;
    return bar && bar->outside(ev);
  };
  open_ = i;
  menu_ = raw;
  layer->open(std::move(menu), std::move(o));
  raw->setHighlighted(-1);
  markDirty();
  return raw;
}

void MenuBar::close() {
  if (!menu_) return;
  Menu* m = menu_;
  menu_ = nullptr;
  open_ = -1;
  if (OverlayLayer* layer = OverlayLayer::of(*this)) layer->close(*m);
  markDirty();
}

std::string MenuBar::shortcut(std::string_view action) const {
  for (const Keymap::Binding& b : ctx_.keymap().bindings())
    if (b.action == action) return formatChord(b.chord);
  return {};
}

// An item that runs `action`: greyed when the action is not registered (its view is not installed) or disabled.
MenuItem MenuBar::act(std::string label, std::string action) const {
  MenuItem m;
  m.label = std::move(label);
  m.shortcut = shortcut(action);
  const ActionIndex a = ctx_.actions().find(action);
  m.enabled = a != kNoAction && ctx_.actions().enabled(a);
  m.action = std::move(action);
  return m;
}

std::vector<MenuItem> MenuBar::items(int32_t i) const {
  std::vector<MenuItem> v;
  auto check = [](MenuItem m, bool on) {
    m.checked = on;
    return m;
  };
  switch (i) {
    case 0:  // File
      v.push_back(act("New Set", "file.new"));
      v.push_back(act("Open Set\xE2\x80\xA6", "file.open"));
      v.push_back(MenuItem::sep());
      v.push_back(act("Save Set", "file.save"));
      v.push_back(act("Save Set As\xE2\x80\xA6", "file.saveAs"));
      break;
    case 1: {  // Edit
      const doc::History& h = ctx_.store().history();
      v.push_back(act(h.canUndo() ? "Undo " + std::string(h.undoLabel()) : std::string("Undo"), "edit.undo"));
      v.push_back(act(h.canRedo() ? "Redo " + std::string(h.redoLabel()) : std::string("Redo"), "edit.redo"));
      v.push_back(MenuItem::sep());
      const bool roll = focusInClipView(tree());
      const std::string pre = roll ? "pianoroll." : "arrangement.";
      v.push_back(act("Cut", pre + "cut"));
      v.push_back(act("Copy", pre + "copy"));
      v.push_back(act("Paste", pre + "paste"));
      v.push_back(act("Duplicate", pre + "duplicate"));
      v.push_back(act("Delete", pre + "delete"));
      v.push_back(MenuItem::sep());
      v.push_back(act("Select All", pre + "selectAll"));
      v.push_back(MenuItem::sep());
      if (roll) {
        v.push_back(act("Quantize", "pianoroll.quantize"));
      } else {
        v.push_back(act("Split", "arrangement.split"));
        v.push_back(act("Consolidate", "arrangement.consolidate"));
        v.push_back(MenuItem::sep());
        v.push_back(act("Insert Time", "arrangement.insertTime"));
        v.push_back(act("Duplicate Time", "arrangement.duplicateTime"));
        v.push_back(act("Delete Time", "arrangement.deleteTime"));
        v.push_back(MenuItem::sep());
        v.push_back(act("Rename", "arrangement.rename"));
        v.push_back(act("Group Tracks", "track.group"));
        v.push_back(act("Ungroup Tracks", "track.ungroup"));
      }
      v.push_back(MenuItem::sep());
      v.push_back(check(act("Draw Mode", "edit.toggleDrawMode"), ctx_.drawMode()));
      break;
    }
    case 2:  // Create
      v.push_back(act("Insert MIDI Track", "track.newMidi"));
      v.push_back(act("Insert Return Track", "track.newReturn"));
      v.push_back(MenuItem::sep());
      v.push_back(act("Insert MIDI Clip", "arrangement.insertMidiClip"));
      v.push_back(MenuItem::sep());
      v.push_back(act("New Main Tuning", "tuning.newMain"));
      break;
    case 3:  // View
      v.push_back(act("Arrangement / Mixer", "view.toggleMixer"));
      v.push_back(check(act("Detail View", "view.toggleDetail"), ctx_.detailVisible()));
      v.push_back(act("Clip / Device View", "view.toggleClipDevice"));
      v.push_back(check(act("Tunings", "view.toggleTunings"), ctx_.tuningsVisible()));
      v.push_back(act("MOS Wheel", "tuning.openMosWheel"));
      v.push_back(act("Cables: Show / Fade / Hide", "view.cycleCables"));  // T4.7
      v.push_back(MenuItem::sep());
      v.push_back(act("Zoom In", "arrangement.zoomIn"));
      v.push_back(act("Zoom Out", "arrangement.zoomOut"));
      v.push_back(act("Zoom to Selection", "arrangement.zoomToSelection"));
      v.push_back(act("Zoom Back", "arrangement.zoomBack"));
      v.push_back(MenuItem::sep());
      v.push_back(act("Follow", "view.toggleFollow"));
      v.push_back(act("Automation Mode", "arrangement.toggleAutomationMode"));
      break;
    case 4:  // Options
      v.push_back(act("Narrow Grid", "grid.narrow"));
      v.push_back(act("Widen Grid", "grid.widen"));
      v.push_back(check(act("Triplet Grid", "grid.toggleTriplet"), ctx_.grid().tuplet == 3));
      v.push_back(check(act("Snap to Grid", "grid.toggleSnap"), ctx_.grid().snap));
      v.push_back(check(act("Fixed Grid", "grid.toggleFixed"), ctx_.grid().mode == GridMode::Fixed));
      v.push_back(MenuItem::sep());
      v.push_back(act("Preferences\xE2\x80\xA6", "app.preferences"));
      // T4.4-fix1 (SPEC §15 2026-10-26): the circuit editor works on the project's devices (a device's context menu,
      // or a double-click on its title); loose .pqp files are a developer's tool.
      v.push_back(MenuItem::sep());
      v.push_back(MenuItem::sub("Developer", {act("Open Patch File in Circuit View\xE2\x80\xA6", "view.openPatch"),  // T4.2
                                             act("Preview Device File\xE2\x80\xA6", "view.openDevice")}));     // T4.6
      break;
    case 5: {  // Help
      ViewContext* c = &ctx_;
      v.push_back(MenuItem::item("About pqdaw", [c] {
        c->setStatus("pqdaw \xC2\xB7 a performance-first microtonal DAW (development build)");
      }));
      break;
    }
    default: break;
  }
  return v;
}

}  // namespace pq::views::shell
