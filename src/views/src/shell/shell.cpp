#include "pq/views/shell/shell.h"

#include "pq/ui/theme.h"
#include "shell_widgets.h"

namespace pq::views {

using namespace pq::ui;
using shell::SlotHost;

namespace {

// The window background: the dark gaps between the panels.
class Backdrop : public Widget {
public:
  Backdrop() = default;
  void paint(PaintContext& ctx) override {
    if (const Theme* th = ctx.res.theme) ctx.dl.addRectFilled(rect().toRect(), th->surface.background);
  }
};

}  // namespace

struct Shell::Impl {
  ViewContext::Subscription layoutSub;
  SlotHost* trackArea = nullptr;
  SlotHost* mixer = nullptr;
  SlotHost* browser = nullptr;
  SlotHost* tunings = nullptr;
  Widget* bottom = nullptr;
  shell::DetailView* detail = nullptr;
  shell::InfoView* info = nullptr;
  shell::ControlBar* controlBar = nullptr;
};

Shell::Shell(UiTree& tree, ViewContext& ctx) : tree_(tree), ctx_(ctx), impl_(std::make_unique<Impl>()) {}

Shell::~Shell() = default;

Result<std::unique_ptr<Shell>> Shell::create(UiTree& tree, ViewContext& ctx, const ShellOptions& options) {
  std::unique_ptr<Shell> s(new Shell(tree, ctx));
  Impl& m = *s->impl_;

  // T3.x-fix-look: the panels (browser, track area, info, detail) sit on the window colour with Live's gutters between
  // them (theme metric size.gutter, 6 px sampled from Live 12) instead of butting together; the splitter handles are the
  // gutters (transparent: the backdrop shows through).
  const PaintResources* res = tree.resources();
  const float gut = res && res->theme ? res->theme->size.gutter : 6.0f;
  const auto g = static_cast<int16_t>(gut);
  auto& root = tree.emplaceRoot<Backdrop>();
  auto& main = root.add<Widget>("main");
  main.setLayout(LayoutKind::Column);
  main.setGap(0);
  main.add<shell::MenuBar>(ctx);
  m.controlBar = &main.add<shell::ControlBar>(ctx);

  auto& panels = main.add<Widget>();  // unnamed (transparent in widget paths): the gutter around the panels
  panels.setLayout(LayoutKind::Column);
  panels.setPadding({g, g, g, 0});
  panels.setHeight(Length::flex(1));
  auto& body = panels.add<Splitter>("body", Splitter::Axis::Vertical, 240.0f, Splitter::Anchor::Second);
  body.setHeight(Length::flex(1));
  body.setLimits(80, kUnbounded, 80);
  body.setHandleThickness(g);
  auto& top = body.add<Widget>("top");
  top.setLayout(LayoutKind::Row);
  top.setGap(g);
  m.browser = &top.add<SlotHost>("browser", SlotHost::Fill::Panel, "Browser");
  m.browser->setWidth(Length::fixed(220));
  m.tunings = &top.add<SlotHost>("tunings", SlotHost::Fill::Panel, "Tunings");
  m.tunings->setWidth(Length::fixed(236));
  m.tunings->setClipChildren(true);
  m.trackArea = &top.add<SlotHost>("track-area", SlotHost::Fill::Lane);
  m.trackArea->setWidth(Length::flex(1));
  m.trackArea->setClipChildren(true);
  m.mixer = &m.trackArea->add<SlotHost>("mixer", SlotHost::Fill::Panel);
  m.mixer->setVisible(false);

  auto& bottom = body.add<Splitter>("bottom", Splitter::Axis::Horizontal, 200.0f, Splitter::Anchor::First);
  bottom.setLimits(120, 480, 200);
  bottom.setHandleThickness(g);
  m.bottom = &bottom;
  m.info = &bottom.add<shell::InfoView>(ctx);
  m.detail = &bottom.add<shell::DetailView>(ctx);
  m.detail->setClipChildren(true);

  auto& below = main.add<Widget>();  // the gutter under the panels, above the status bar
  below.setHeight(Length::fixed(gut));
  below.setHittable(false);
  main.add<shell::StatusBar>(ctx);
  s->overlay_ = &root.add<OverlayLayer>("overlay");

  Impl* impl = &m;
  auto applyLayout = [impl, &ctx] {
    impl->bottom->setVisible(ctx.detailVisible());
    impl->browser->setVisible(ctx.browserVisible());
    impl->tunings->setVisible(ctx.tuningsVisible());
  };
  applyLayout();
  m.layoutSub = ctx.observe([applyLayout](uint32_t what) {
    if (what & view_change::kLayout) applyLayout();
  });

  if (options.installContent) installDefaultContent(*s);

  Status bound = options.keymap ? ctx.applyKeymap(*options.keymap) : ctx.applyDefaultKeymap();
  if (!bound) return std::move(bound).error();
  tree.setKeymap(&ctx.actions(), &ctx.keymap());
  m.info->attachTo(tree);
  tree.layoutNow();
  return s;
}

Widget& Shell::slot(ShellSlot s) const noexcept {
  switch (s) {
    case ShellSlot::TrackArea: return *impl_->trackArea;
    case ShellSlot::Mixer: return *impl_->mixer;
    case ShellSlot::ClipDetail: return impl_->detail->clip();
    case ShellSlot::DeviceDetail: return impl_->detail->device();
    case ShellSlot::Browser: return *impl_->browser;
    case ShellSlot::Tunings: return *impl_->tunings;
  }
  return *impl_->trackArea;
}

void Shell::cancelInteraction() {
  tree_.cancelInteraction();
  // Whatever still holds a document gesture open (a controller whose widget had no capture) is cancelled too: undo
  // must not stay disabled behind an abandoned drag.
  if (ctx_.store().inGesture()) (void)ctx_.store().cancelGesture();
}

const std::string& Shell::infoTitle() const noexcept { return impl_->info->title(); }
const std::string& Shell::cpuText() const noexcept { return impl_->controlBar->cpuText(); }
const std::string& Shell::infoBody() const noexcept { return impl_->info->body(); }
const std::string& Shell::positionText() const noexcept { return impl_->controlBar->positionText(); }

}  // namespace pq::views
