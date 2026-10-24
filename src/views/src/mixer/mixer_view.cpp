// MixerView (T3.10): the basic mixer view (Ctrl+Alt+M) and installMixer, which also fills the arrangement headers'
// mixer section through ArrangementView::setHeaderDecorator.

#include <algorithm>
#include <cassert>

#include "internal.h"
#include "pq/ui/theme.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/shell/shell.h"

namespace pq::views {

using namespace pq::ui;
namespace mm = mixer_metrics;

namespace {

// The scroll view's content: strips side by side, one kStripWidth each.
class StripRow : public Widget {
public:
  StripRow() : Widget("row") { setLayout(LayoutKind::Custom); }
  ISize contentExtent(ISize viewport) const override {
    const UiScale s = tree() ? tree()->uiScale() : UiScale{};
    return {std::max(viewport.w, s.px(mm::kStripWidth * static_cast<float>(childCount()))), viewport.h};
  }
  void arrange(LayoutPass& pass) override {
    const IRect r = rect();
    const UiScale s(pass.scale());
    for (size_t i = 0; i < childCount(); ++i) {
      const int32_t x0 = r.x + s.px(mm::kStripWidth * static_cast<float>(i));
      const int32_t x1 = r.x + s.px(mm::kStripWidth * static_cast<float>(i + 1));
      pass.place(child(i), {x0, r.y, x1 - x0, r.h});
    }
  }
};

ArrangementView* arrangementIn(const Widget& trackArea) { return dynamic_cast<ArrangementView*>(trackArea.find("arrangement")); }

}  // namespace

MixerView::MixerView(ViewContext& ctx, std::shared_ptr<MixerController> controller)
    : Widget("mixer-view"), ctx_(ctx), ctl_(std::move(controller)) {
  setLayout(LayoutKind::Custom);
  setClipChildren(true);
  setFocusable(true);
  ctl_->view = this;
  scroll_ = &add<ScrollView>("strips", true, false);
  row_ = &scroll_->add<StripRow>();
  add<Scrollbar>("hscroll", *scroll_, Scrollbar::Axis::Horizontal);
  pinned_ = &add<Widget>("main");
  pinned_->setLayout(LayoutKind::Stack);
  docSub_ = ctx_.subscribe([this](const doc::StoreEvent&) {
    stale_ = true;
    if (shown()) sync();
  });
  viewSub_ = ctx_.observe([this](uint32_t bits) {
    if (bits & view_change::kTransport) wakeMeters();
    if (bits & view_change::kAutomation) {  // T3.15: the strips' automation markers (overridden: grey)
      stale_ = true;
      if (shown()) sync();
    }
  });
}

MixerView::~MixerView() {
  if (meterTimer_) cancelTimer(meterTimer_);
  ctl_->view = nullptr;
  // The context keeps the action (disabled without a handler): a rebuilt shell installs a new mixer over it.
  ctx_.clearViewAction("view.toggleMixer");
}

bool MixerView::shown() const noexcept {
  const Widget* slot = parent();
  return slot && slot->visible() && visible();
}

void MixerView::setShown(bool on) {
  Widget* slot = parent();
  if (!slot) return;
  if (on == shown()) return;
  slot->setVisible(on);
  ArrangementView* arr = slot->parent() ? arrangementIn(*slot->parent()) : nullptr;
  if (arr) arr->setVisible(!on);
  if (on) {
    stale_ = true;
    sync();
    if (tree()) tree()->setFocus(this);
  } else if (arr && tree()) {
    tree()->setFocus(arr);
  }
  wakeMeters();  // the meters that show changed
}

MixerStrip* MixerView::strip(doc::TrackId id) const noexcept {
  for (MixerStrip* s : strips_)
    if (s->trackId() == id) return s;
  return nullptr;
}

// Strips follow the document: tracks in arrangement order (members of folded groups hidden), the returns, then the
// master in the pinned host. Strips are reused by position; bind() updates only what changed.
void MixerView::sync() {
  if (!stale_) return;
  stale_ = false;
  const doc::Snapshot snap = ctx_.doc();
  const doc::Project& p = *snap;
  const doc::ViewState& vs = ctx_.store().view();
  std::vector<const doc::Track*> order;
  struct Walk {
    const doc::ViewState& vs;
    std::vector<const doc::Track*>& out;
    void operator()(const std::vector<doc::TrackPtr>& list) {
      for (const doc::TrackPtr& t : list) {
        out.push_back(t.get());
        const auto it = vs.tracks.find(t->id);
        const bool folded = it != vs.tracks.end() && it->second.folded;
        if (t->kind == doc::TrackKind::Group && !folded) (*this)(t->children);
      }
    }
  } walk{vs, order};
  walk(p.tracks);
  for (const doc::TrackPtr& r : p.returns) order.push_back(r.get());

  while (row_->childCount() > order.size()) (void)row_->removeChild(row_->child(row_->childCount() - 1));
  while (row_->childCount() < order.size())
    row_->add<MixerStrip>("strip[" + std::to_string(row_->childCount()) + "]", ctl_, StripLayout::Vertical);
  strips_.clear();
  for (size_t i = 0; i < order.size(); ++i) {
    auto& s = static_cast<MixerStrip&>(row_->child(i));
    s.bind(p, *order[i]);
    strips_.push_back(&s);
  }
  if (p.master) {
    if (pinned_->childCount() == 0) pinned_->add<MixerStrip>("master", ctl_, StripLayout::Vertical);
    auto& m = static_cast<MixerStrip&>(pinned_->child(0));
    m.bind(p, *p.master);
    strips_.push_back(&m);
  }
  row_->invalidateArrange();
  scroll_->invalidateArrange();
  markDirty();
}

void MixerView::setMeterSource(MeterSource* source) {
  ctl_->source = source;
  wakeMeters();
}

void MixerView::wakeMeters() {
  if (meterTimer_ || !ctl_->source) return;
  if (ctl_->pollMeters()) meterTimer_ = requestTimer(mm::kMeterPollSeconds);
}

bool MixerView::pollMeters() { return ctl_->pollMeters(); }

void MixerView::onTimer(uint32_t id) {
  if (id != meterTimer_) return;
  meterTimer_ = 0;
  if (ctl_->pollMeters()) meterTimer_ = requestTimer(mm::kMeterPollSeconds);
}


void MixerView::arrange(LayoutPass& pass) {
  const IRect r = rect();
  const UiScale s(pass.scale());
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  const int32_t sb = s.px(res && res->theme ? res->theme->size.scrollbar : 10.0f);
  const int32_t mw = s.px(mm::kStripWidth);
  const int32_t gap = s.px(4.0f);
  const int32_t sw = std::max(0, r.w - mw - gap);
  pass.place(*scroll_, {r.x, r.y, sw, std::max(0, r.h - sb)});
  pass.place(child(1), {r.x, r.bottom() - sb, sw, std::min(sb, r.h)});
  pass.place(*pinned_, {r.right() - mw, r.y, mw, r.h});
}

void MixerView::paint(PaintContext& ctx) {
  if (const Theme* th = ctx.res.theme) ctx.dl.addRectFilled(intersect(rect(), ctx.clip).toRect(), th->arrangement.background);
}

// --- install ------------------------------------------------------------------------------------------------------------

MixerView& installMixer(Shell& shell) {
  ViewContext& ctx = shell.context();
  auto ctl = std::make_shared<MixerController>(ctx);
  Widget& slot = shell.slot(ShellSlot::Mixer);
  auto& view = slot.add<MixerView>(ctx, ctl);
  if (ArrangementView* arr = arrangementIn(shell.slot(ShellSlot::TrackArea))) {
    ctl->selectedTracks = [arr] { return arr->selection().tracks; };
    ctl->selectTrack = [arr](doc::TrackId id, Mods m) { arr->selectTrack(id, m); };
    arr->setHeaderMixerWidth(mm::kHeaderMixerWidth);
    arr->setHeaderDecorator([ctl, arr](TrackHeader& h, const ArrangementRow& row) {
      Widget& sec = h.mixerSection();
      MixerStrip* s = sec.childCount() ? static_cast<MixerStrip*>(&sec.child(0))
                                       : &sec.add<MixerStrip>("strip", ctl, StripLayout::Header);
      s->bind(*arr->shown(), *row.track);
      mixer::bindHeaderChip(h.chipSection(), ctl, *arr->shown(), *row.track);
    });
  }
  ctx.setViewAction("view.toggleMixer", [&view] { view.toggle(); });
  return view;
}

MixerView* findMixer(const Shell& shell) noexcept {
  return dynamic_cast<MixerView*>(shell.slot(ShellSlot::Mixer).find("mixer-view"));
}

}  // namespace pq::views
