// GU.3 (docs/plans/GU.md "Phases" 3): the frame cost of a group panel of 200 elements (pq/views/panel/
// group_panel_face.h) in a PanelHost filling a 1200 × 640 window — 120 knobs (arc / pie / dot / line), 40 sliders,
// 20 number boxes and 20 toggles bound to 20 value blocks and the group's settings:
//   panel.200 settled        a whole repaint with every static layer from the shared element tiles (one raster per
//                            kind + style + size) — what a frame costs while nothing changes but the host repaints
//   panel.200 direct         the same without a tile atlas (every static layer tessellated: no GPU / goldens)
//   panel.200 settled, text as bars   the settled frame without text shaping (what the text costs)
//   panel.200 value frame    a knob value written to the document (a param write: a new snapshot), then the repaint
//                            (the target re-indexes the group's blocks, every binding re-resolved, positions read)
// CPU only (the draw list); with a GPU the scene textures provide the atlas. Results → bench/RESULTS.md (ship).

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/view_context.h"
#include "pq_bench.h"

using namespace pq;

namespace {

struct Rig {
  views::FakeTransport port;
  views::ViewContext ctx{port};
  std::unique_ptr<doc::DeviceLibrary> lib;
  ui::TestDriver d{1200, 640};
  views::panel::PanelHost* host = nullptr;
  doc::DeviceId group;
  std::vector<doc::DeviceId> values;
  bool gpu = false;
  ui::DrawList dl;

  bool build() {
    auto l = doc::loadDeviceLibrary(std::string(PQ_SOURCE_DIR) + "/stdlib");
    if (!l) return false;
    lib = std::move(*l);
    ctx.setDeviceLibrary(lib.get());
    doc::DocumentStore& s = ctx.store();
    auto run = [&](const doc::ops::EditResult& e) { return e && s.dispatch(e->command).ok(); };
    auto t = doc::ops::createTrack(*s.current(), s.ids(), doc::TrackKind::Midi, {}, "Keys");
    if (!t || !s.dispatch(t->command).ok()) return false;
    const doc::Track* tr = doc::findTrackPtr(*s.current(), t->tracks[0]);
    group = doc::mainPath(*s.current(), *tr).blocks.at(0)->id;
    for (int i = 0; i < 20; ++i) {
      auto e = doc::ops::addDevice(*s.current(), s.ids(), t->tracks[0], "Value", doc::DeviceKind::AudioEffect, doc::ops::kOffPath);
      if (!e || !s.dispatch(e->command).ok()) return false;
      values.push_back(e->devices[0]);
      if (!run(doc::ops::setDeviceGroup(*s.current(), s.ids(), lib.get(), values.back(), group))) return false;
    }
    std::vector<doc::PanelElement> es;
    static constexpr const char* kPointers[] = {"arc", "pie", "dot", "line"};
    for (int i = 0; i < 200; ++i) {
      doc::PanelElement e;
      const int col = i % 20, row = i / 20;
      e.x = 8.0f + 58.0f * static_cast<float>(col);
      e.y = 8.0f + 62.0f * static_cast<float>(row);
      const doc::DeviceId v = values[static_cast<size_t>(i % 20)];
      if (row < 6) {
        e.kind = "knob";
        e.w = 52, e.h = 58;
        e.setField(doc::styleText("pointer", kPointers[row % 4]));
        e.bindings.push_back(doc::controlBinding(v, "value"));
      } else if (row < 8) {
        e.kind = "slider";
        e.w = 52, e.h = 26;
        e.bindings.push_back(doc::controlBinding(v, "value"));
      } else if (row == 8) {
        e.kind = "number";
        e.w = 52, e.h = 22;
        e.bindings.push_back(doc::controlBinding(v, "value"));
      } else {
        e.kind = "toggle";
        e.w = 52, e.h = 16;
        e.bindings.push_back(col % 2 == 0 ? doc::settingBinding(group, "mono") : doc::controlBinding(v, "value"));
      }
      es.push_back(std::move(e));
    }
    if (!run(doc::ops::addPanelElements(*s.current(), group, std::move(es)))) return false;
    gpu = d.enableGpu().ok();
    auto face = std::make_unique<views::panel::GroupPanelFace>(views::panel::docPanelSource(ctx, group),
                                                               views::panel::makeDocPanelTarget(ctx, group, [] { return nullptr; }));
    host = &d.setRoot<views::panel::PanelHost>("panel", std::move(face));
    d.tree().layoutNow();
    d.advance(0.1);  // tiles built at this scale
    return true;
  }
  void paint(const ui::PaintResources& res) {
    dl.reset(1200, 640);
    host->markDirty();
    d.tree().paint(dl, res, 0.0);
  }
};

}  // namespace

int main(int argc, char** argv) {
  pq::bench::Runner r("pq_bench_group_panel");
  static Rig rig;
  static bool built = false;
  auto ready = [] {
    if (!built) built = rig.build();
    return built;
  };
  r.add("panel.200 settled", [&](pq::bench::Context& c) {
    if (!ready()) return;
    const ui::PaintResources& res = rig.d.resources();
    c.measure([&] { rig.paint(res); }, 1, "ns/frame", rig.gpu ? "(tiles)" : "(no GPU: direct)");
    const auto& st = static_cast<views::panel::GroupPanelFace&>(rig.host->face()).stats();
    std::printf("  elements drawn %u, static layers from tiles %u, drawn directly %u\n", st.drawn, st.fromTiles, st.direct);
  });
  r.add("panel.200 direct", [&](pq::bench::Context& c) {
    if (!ready()) return;
    ui::PaintResources res = rig.d.resources();
    res.tiles = nullptr;
    c.measure([&] { rig.paint(res); }, 1, "ns/frame");
  });
  // Attribution: the same settled frame with text drawn as bars (no shaping).
  r.add("panel.200 settled, text as bars", [&](pq::bench::Context& c) {
    if (!ready()) return;
    ui::PaintResources res = rig.d.resources();
    res.text = nullptr;
    c.measure([&] { rig.paint(res); }, 1, "ns/frame");
  });
  r.add("panel.200 value frame", [&](pq::bench::Context& c) {
    if (!ready()) return;
    const ui::PaintResources& res = rig.d.resources();
    doc::DocumentStore& s = rig.ctx.store();
    double v = 0.0;
    (void)s.beginGesture("bench");  // (one gesture: the history does not grow per write)
    c.measure(
        [&] {
          v = v > 0.9 ? 0.0 : v + 0.01;
          auto e = doc::ops::setDeviceParam(*s.current(), rig.values[3], "value", v);
          (void)s.dispatch(e->command);
          rig.paint(res);
        },
        1, "ns/frame");
    (void)s.cancelGesture();
  });
  return r.main(argc, argv);
}
