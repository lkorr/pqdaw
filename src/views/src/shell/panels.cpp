// The shell's panels (T3.7): info view, slot hosts, detail view, status line.

#include <algorithm>

#include "../common/paint.h"
#include "shell_widgets.h"

namespace pq::views::shell {

using namespace pq::ui;

// --- InfoView -------------------------------------------------------------------------------------------

InfoView::InfoView() : Widget("info") {}

InfoView::~InfoView() {
  if (observing_) observing_->removeObserver(this);
}

void InfoView::attachTo(UiTree& tree) {
  if (observing_ == &tree) return;
  if (observing_) observing_->removeObserver(this);
  observing_ = &tree;
  tree.addObserver(this);
}

void InfoView::onHoverChanged(Widget* hovered) { show(infoFor(hovered)); }

// Copies the text (the hovered widget owns the views and may go away); repaints only when it changed.
void InfoView::show(const InfoText& t) {
  if (t.title == title_ && t.body == body_) return;
  title_.assign(t.title);
  if (t.body != body_) {
    body_.assign(t.body);
    wrapWidth_ = -1;
  }
  markDirty();
}

void InfoView::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  ctx.dl.addRectFilled(r.toRect(), th->surface.panel);
  const ThemeFonts* f = ctx.res.uiFonts;
  TextSystem* ts = ctx.res.text;
  if (!f || !ts || (title_.empty() && body_.empty())) return;
  const int32_t pad = ctx.scale.px(8);
  ctx.dl.pushClip(r.toRect());
  int32_t y = r.y + ctx.scale.px(6);
  if (!title_.empty()) {
    const int32_t h = ts->metrics(f->bodyStrong).lineHeight;
    paint::text(ctx, f->bodyStrong, {r.x, y, r.w, h}, title_, th->text.primary, TextAlign::Left, pad, pad);
    y += h + ctx.scale.px(4);
  }
  // Greedy word wrap of the body at the current width, cached until the text, font or width changes.
  const int32_t width = r.w - 2 * pad;
  if (width > 0 && !body_.empty()) {
    if (width != wrapWidth_ || f->small != wrapFont_) {
      wrapWidth_ = width;
      wrapFont_ = f->small;
      lines_.clear();
      const std::string_view b = body_;
      uint32_t start = 0;
      while (start < b.size()) {
        uint32_t end = start, fit = start;
        while (end < b.size()) {
          uint32_t next = end;
          while (next < b.size() && b[next] == ' ') ++next;
          while (next < b.size() && b[next] != ' ') ++next;
          if (ts->measure(f->small, b.substr(start, next - start)) > width && fit > start) break;
          fit = end = next;
        }
        if (fit == start) fit = static_cast<uint32_t>(b.size());  // nothing fits: the line is truncated when drawn
        lines_.push_back(start);
        lines_.push_back(fit);
        start = fit;
        while (start < b.size() && b[start] == ' ') ++start;
      }
    }
    const int32_t lh = ts->metrics(f->small).lineHeight;
    for (size_t i = 0; i + 1 < lines_.size() && y < r.bottom(); i += 2, y += lh) {
      const std::string_view line = std::string_view(body_).substr(lines_[i], lines_[i + 1] - lines_[i]);
      paint::text(ctx, f->small, {r.x, y, r.w, lh}, line, th->text.secondary, TextAlign::Left, pad, pad);
    }
  }
  ctx.dl.popClip();
}

// --- SlotHost -------------------------------------------------------------------------------------------

SlotHost::SlotHost(std::string name, Fill fill, std::string hint) : Widget(std::move(name)), fill_(fill), hint_(std::move(hint)) {}

void SlotHost::setHint(std::string hint) {
  if (hint == hint_) return;
  hint_ = std::move(hint);
  markDirty();
}

void SlotHost::paint(PaintContext& ctx) {
  for (size_t i = 0; i < childCount(); ++i)
    if (child(i).visible()) return;  // an installed view covers the slot
  const Theme* th = ctx.res.theme;
  if (!th || fill_ == Fill::None) return;
  const IRect r = rect();
  ctx.dl.addRectFilled(r.toRect(), fill_ == Fill::Lane ? th->track.lane : th->surface.panel);
  if (!hint_.empty())
    if (const ThemeFonts* f = ctx.res.uiFonts) paint::text(ctx, f->small, r, hint_, th->text.disabled, TextAlign::Center, 8, 8);
}

// --- DetailView -----------------------------------------------------------------------------------------

DetailView::DetailView(ViewContext& ctx) : Widget("detail"), ctx_(ctx) {
  setLayout(LayoutKind::Stack);
  clip_ = &add<SlotHost>("clip", SlotHost::Fill::Panel);
  device_ = &add<SlotHost>("device", SlotHost::Fill::Panel);
  sub_ = ctx_.observe([this](uint32_t what) {
    if (what & view_change::kLayout) refresh();
  });
  refresh();
}

void DetailView::refresh() {
  const bool clipMode = ctx_.detailMode() == DetailMode::Clip;
  clip_->setVisible(clipMode);
  device_->setVisible(!clipMode);
}

void DetailView::paint(PaintContext&) {}

// --- StatusBar ------------------------------------------------------------------------------------------

StatusBar::StatusBar(ViewContext& ctx) : Widget("status-bar"), ctx_(ctx) {
  setLayout(LayoutKind::Row);
  setHeight(Length::fixed(22));
  setPadding({8, 3, 4, 3});
  setGap(4);
  setAlign(Align::Start, Align::Center);
  message_ = &add<Label>("message", "", FontRoleId::Small, TextTone::Secondary);
  message_->setSize(Length::flex(1), Length::fixed(16));
  project_ = &add<Label>("project", "", FontRoleId::Small, TextTone::Secondary, TextAlign::Right);
  project_->setSize(Length::fixed(240), Length::fixed(16));
  project_->setInfo("Project", "The project's name; \"*\" means it has unsaved changes (Ctrl+S saves).");
  auto selector = [this](const char* name, const char* text, DetailMode mode) {
    auto& t = add<Toggle>(name, text, ToggleStyle::Accent);
    t.setSize(Length::fixed(48), Length::fixed(16));
    t.setFocusable(false);  // Space stays the transport's
    t.onToggle = [this, mode](bool on) {
      if (on) ctx_.setDetailMode(mode);
      else ctx_.setDetailVisible(false);
      refresh();
    };
    return &t;
  };
  clipSel_ = selector("view-clip", "Clip", DetailMode::Clip);
  clipSel_->setInfo("Clip View Selector", "Shows the clip view in the detail view (Shift+Tab switches clip / device; "
                                          "Ctrl+Alt+L hides or shows the detail view).");
  deviceSel_ = selector("view-device", "Device", DetailMode::Device);
  deviceSel_->setInfo("Device View Selector", "Shows the device view in the detail view (Shift+Tab switches clip / device).");
  sub_ = ctx_.observe([this](uint32_t what) {
    if (what & (view_change::kLayout | view_change::kStatus | view_change::kProject)) refresh();
  });
  refresh();
}

void StatusBar::refresh() {
  message_->setText(ctx_.status());
  project_->setText(ctx_.projectName() + (ctx_.dirty() ? " *" : ""));
  clipSel_->setOn(ctx_.detailVisible() && ctx_.detailMode() == DetailMode::Clip);
  deviceSel_->setOn(ctx_.detailVisible() && ctx_.detailMode() == DetailMode::Device);
}

void StatusBar::paint(PaintContext& ctx) {
  if (const Theme* th = ctx.res.theme) ctx.dl.addRectFilled(rect().toRect(), th->surface.panel);
}

}  // namespace pq::views::shell
