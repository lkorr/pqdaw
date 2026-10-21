// The T3.1 demo scene `widgets-core`: a DAW-shaped widget tree exercising rows/columns (fixed + flex with
// min/max), nested splitters (one anchored at the end, like Live's detail view) and a scroll view whose content
// is a virtualised lane grid that paints only the visible range. Unstyled on purpose: themed controls are
// T3.2's; this scene uses PQ Meter's signal tokens like the other M0 scenes.

#include <algorithm>
#include <cstdio>
#include <string>

#include "pq/ui/scenes.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/widget.h"
#include "scenes_internal.h"

namespace pq::ui {

namespace {

using namespace signal_theme;

// A filled rect with an optional left-aligned label.
class Panel : public Widget {
public:
  Panel(std::string name, Rgba fill, std::string label = {}, Rgba text = kHeaderText)
      : Widget(std::move(name)), fill_(fill), text_(text), label_(std::move(label)) {}
  void paint(PaintContext& ctx) override {
    if (alphaOf(fill_)) ctx.dl.addRectFilled(rect().toRect(), fill_);
    if (label_.empty() || !ctx.res.text || !ctx.res.fonts) return;
    const IRect r = rect();
    const float baseline = static_cast<float>(r.y + r.h / 2 + ctx.scale.px(4));
    ctx.res.text->draw(ctx.dl, ctx.res.fonts->section, {static_cast<float>(r.x + ctx.scale.px(8)), baseline}, label_, text_);
  }

private:
  Rgba fill_, text_;
  std::string label_;
};

// Virtualised arrangement-like content: 40 lanes × 128 bars. Reports its full extent; paints only the lanes,
// beat lines and clips that intersect the clip rect (one widget, no per-clip widgets).
class LaneGrid : public Widget {
public:
  static constexpr int kLanes = 40, kBars = 128;
  static constexpr float kLaneH = 28, kBarW = 96;
  explicit LaneGrid(std::string name) : Widget(std::move(name)) {}

  ISize contentExtent(ISize) const override {
    const UiScale s = tree() ? tree()->uiScale() : UiScale{};
    return {s.px(kBarW) * kBars, s.px(kLaneH) * kLanes};  // the lane/bar pitch paint() uses
  }

  void paint(PaintContext& ctx) override {
    const IRect r = rect();
    const IRect vis = intersect(r, ctx.clip);
    if (vis.empty()) return;
    const int32_t laneH = ctx.scale.px(kLaneH), barW = ctx.scale.px(kBarW);
    const int32_t lane0 = (vis.y - r.y) / laneH, lane1 = std::min(kLanes - 1, (vis.bottom() - 1 - r.y) / laneH);
    const int32_t bar0 = (vis.x - r.x) / barW, bar1 = std::min(kBars - 1, (vis.right() - 1 - r.x) / barW);
    for (int32_t l = lane0; l <= lane1; ++l) {
      const float y0 = static_cast<float>(r.y + l * laneH), y1 = y0 + static_cast<float>(laneH);
      ctx.dl.addRectFilled({static_cast<float>(vis.x), y0, static_cast<float>(vis.right()), y1 - 1}, (l & 1) ? kPanel : kHeader);
      for (int32_t b = bar0; b <= bar1; ++b) {
        const uint32_t h = static_cast<uint32_t>(l * 73856093) ^ static_cast<uint32_t>(b * 19349663);
        if (h % 5 >= 2) continue;
        const float x0 = static_cast<float>(r.x + b * barW) + 1, x1 = x0 + static_cast<float>(barW) - 2;
        ctx.dl.addRoundedRectFilled({x0, y0 + 2, x1, y1 - 3}, ctx.scale.exact(3.0f), withAlpha(kBands[l % 6], 200));
      }
    }
    for (int32_t b = bar0; b <= bar1 + 1; ++b)
      for (int32_t q = 0; q < 4; ++q) {
        const float x = static_cast<float>(r.x + b * barW + q * barW / 4);
        if (x < static_cast<float>(vis.x) || x >= static_cast<float>(vis.right())) continue;
        ctx.dl.addRectFilled({x, static_cast<float>(vis.y), x + 1, static_cast<float>(vis.bottom())},
                             q == 0 ? withAlpha(kGridLabel, 60) : kGrid);
      }
  }
};

}  // namespace

std::unique_ptr<Widget> makeWidgetsCoreDemo() {
  auto root = std::make_unique<Widget>();
  root->setLayout(LayoutKind::Column);

  root->add<Panel>("header", kHeader, "pqdaw · widgets-core (T3.1)", rgba(226, 230, 240)).setHeight(Length::fixed(28));

  auto& body = root->add<Splitter>("body", Splitter::Axis::Horizontal, 180.0f);
  body.setLimits(120, 320, 240);
  body.setHandleThickness(4);
  body.setHandleColor(kBackground);

  // Browser: a column of fixed-height rows.
  auto& browser = body.add<Panel>("browser", kPanel);
  browser.setLayout(LayoutKind::Column).setPadding(Insets::all(6)).setGap(2);
  const char* items[] = {"Instruments", "Audio Effects", "MIDI Effects", "Tunings", "Scales", "Samples", "Clips"};
  for (int i = 0; i < 7; ++i) {
    char name[16];
    std::snprintf(name, sizeof name, "item[%d]", i);
    browser.add<Panel>(name, i == 3 ? withAlpha(kAccent, 60) : kHeader, items[i]).setHeight(Length::fixed(22));
  }

  // Main area: arrangement above, detail view anchored at the bottom.
  auto& main = body.add<Splitter>("main", Splitter::Axis::Vertical, 150.0f, Splitter::Anchor::Second);
  main.setLimits(60, 320, 120);
  main.setHandleThickness(4);
  main.setHandleColor(kBackground);

  auto& arrangement = main.add<Widget>("arrangement");
  arrangement.setLayout(LayoutKind::Column);
  auto& toolbar = arrangement.add<Panel>("toolbar", kHeader);
  toolbar.setLayout(LayoutKind::Row).setPadding({4, 4, 4, 4}).setGap(4).setHeight(Length::fixed(30));
  toolbar.add<Panel>("play", withAlpha(kWaveform, 90), "Play").setWidth(Length::fixed(56));
  toolbar.add<Panel>("stop", withAlpha(kWaveformR, 90), "Stop").setWidth(Length::fixed(56));
  toolbar.add<Panel>("tempo", kPanel, "120.00").setWidth(Length::fixed(72));
  toolbar.add<Widget>("spacer");  // flex
  toolbar.add<Panel>("grid", withAlpha(kViolet, 80), "1/16").setWidth(Length::flex(1, 40, 90));

  auto& tracks = arrangement.add<ScrollView>("tracks");
  tracks.add<LaneGrid>("lanes");

  auto& detail = main.add<Panel>("detail", kBackground);
  detail.setLayout(LayoutKind::Row).setPadding(Insets::all(4)).setGap(4);
  detail.add<Panel>("clip", kPanel, "Clip").setWidth(Length::flex(1, 100, 220));
  detail.add<Panel>("notes", kHeader, "Notes").setWidth(Length::flex(2, 120));
  detail.add<Panel>("envelopes", kPanel, "Envelopes").setWidth(Length::flex(1, 80));

  root->add<Panel>("status", kHeader, "status: idle").setHeight(Length::fixed(22));
  return root;
}

namespace detail {

void widgetsScene(DrawList& dl, const SceneTextures& tex, double time) {
  UiTree tree;
  tree.setViewport(dl.targetWidth(), dl.targetHeight(), tex.scale);
  tree.setRoot(makeWidgetsCoreDemo());
  tree.layoutNow();
  if (auto* tracks = dynamic_cast<ScrollView*>(tree.find("body/main/arrangement/tracks"))) {
    tracks->scrollTo(tex.scale.px(130), tex.scale.px(40));  // show that the content is offset and clipped
    tree.layoutNow();
  }
  dl.addRectFilled({0, 0, static_cast<float>(dl.targetWidth()), static_cast<float>(dl.targetHeight())}, kBackground);
  PaintResources res;
  res.text = tex.text.get();
  res.fonts = &tex.fonts;
  tree.paint(dl, res, time);
}

}  // namespace detail

}  // namespace pq::ui
