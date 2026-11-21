// AP.2 (docs/plans/AP.md "Where it sits"; pq/document/note_devices.h): the Arpeggiator in the browser's Note Effects,
// dragged by real window events (press on its row, move, release) onto a MIDI band's NOTES cable in the project patch:
// inserted there — after the tuning block, before the instrument — in one undo step; the cable out of it is a notes
// cable too, and the block plays (not veiled). Dropped on empty canvas it is refused.
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/note_devices.h"
#include "pq/library/index.h"
#include "pq/ui/drag.h"
#include "pq/ui/test_driver.h"
#include "pq/views/browser/browser_panel.h"
#include "pq/views/browser/demo_index.h"
#include "pq/views/browser/library.h"
#include "pq/views/browser/preview_port.h"
#include "pq/views/fake_transport.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::project_patch;

namespace {

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  FakePreviewPort preview;
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  BrowserPanel* b = nullptr;
  doc::TrackId keys;
  doc::DeviceId inst;
  Fixture() {
    ctx.setDeviceLibrary(&library());
    ctx.library().setPreview(&preview);
    ctx.library().setIndex(browserDemoIndex(views::test::fixtureDir("audio") + "/samples", 0));
    auto t = doc::ops::createTrack(*ctx.doc(), ctx.store().ids(), doc::TrackKind::Midi, {}, "Keys");
    REQUIRE(t.ok());
    REQUIRE(ctx.store().dispatch(t->command).ok());
    keys = t->tracks[0];
    inst = doc::mainPath(*ctx.doc(), *doc::findTrackPtr(*ctx.doc(), keys)).blocks.at(0)->id;
    ctx.setSelectedTrack(keys);
    ctx.setBrowserVisible(true);
    ctx.setDetailVisible(false);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    b = findBrowser(*shell);
    REQUIRE(b != nullptr);
    view().setShown(true, false);
    d.tree().layoutNow();
    d.step();
  }
  ~Fixture() { d.tree().setRoot(std::make_unique<Widget>()); }
  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  std::vector<doc::DeviceId> path() const {
    std::vector<doc::DeviceId> v;
    for (const doc::Device* x : doc::mainPath(*ctx.doc(), *doc::findTrackPtr(*ctx.doc(), keys)).blocks) v.push_back(x->id);
    return v;
  }
  int32_t rowOf(std::string_view n) const {
    for (size_t i = 0; i < b->rows().size(); ++i) {
      const BrowserRow& r = b->rows()[i];
      if (r.device && b->cellText(r, BrowserColumn::Name) == n) return static_cast<int32_t>(i);
    }
    return -1;
  }
  Vec2 rowCentre(int32_t row) {
    d.tree().layoutNow();
    const IRect r = b->rowRect(row);
    return {static_cast<float>(r.x + r.w / 3), static_cast<float>(r.y + r.h / 2)};
  }
  // The notes cable into `to` (from the tuning block when the band has one), framed on the canvas, and a device point
  // the canvas's own hit test puts on it.
  Vec2 onNotesCableInto(doc::DeviceId to) {
    const ProjectPatch& pp = view().patch();
    const uint32_t node = pp.nodeOf(to);
    REQUIRE(node != kNone);
    const Band& band = pp.bands[pp.nodes[node].band];
    uint32_t edge = kNone;
    for (uint32_t i = 0; i < pp.edges.size(); ++i) {
      const Edge& e = pp.edges[i];
      if (e.kind != EdgeKind::Notes || e.to != node || e.drawnTo != 0xFFFFFFFFu) continue;
      if (edge == kNone || e.from == band.tuning) edge = i;
    }
    REQUIRE(edge != kNone);
    const Edge& e = pp.edges[edge];
    REQUIRE(e.points.size() >= 2);
    ProjectCanvas& c = view().canvas();
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = 0.5 * static_cast<double>(e.points.front().x + e.points.back().x) - 300.0;
    v.y = 0.5 * static_cast<double>(e.points.front().y + e.points.back().y) - 200.0;
    c.setView(v);
    d.tree().layoutNow();
    const Rect a = c.deviceRect(e.points.front().x, e.points.front().y, 0.0f, 0.0f);
    const Rect z = c.deviceRect(e.points.back().x, e.points.back().y, 0.0f, 0.0f);
    for (float t = 0.5f; t > 0.1f; t -= 0.05f)
      for (const float u : {t, 1.0f - t}) {
        const float x = a.x0 + (z.x0 - a.x0) * u, y0 = a.y0 + (z.y0 - a.y0) * u;
        for (float dy = 0.0f; dy < 80.0f; dy += 1.0f)
          for (const float s : {1.0f, -1.0f})
            if (c.edgeAt({x, y0 + s * dy}) == edge) return {x, y0 + s * dy};
      }
    FAIL("no point on the notes cable");
    return {};
  }
};

}  // namespace

TEST_CASE("note device drop: the Arpeggiator dragged from the browser onto the notes cable goes before the instrument") {
  Fixture f;
  f.b->setSource({BrowserSource::Kind::Category, static_cast<uint32_t>(library::Category::NoteEffects)});
  const int32_t row = f.rowOf("Arpeggiator");
  REQUIRE(row >= 0);
  const Vec2 from = f.rowCentre(row);
  const Vec2 to = f.onNotesCableInto(f.inst);
  const size_t h0 = f.ctx.store().history().cursor();
  // Empty canvas first: refused (a note effect goes only onto a notes cable).
  const IRect cr = f.view().canvas().rect();
  const Vec2 empty{static_cast<float>(cr.x + 4), static_cast<float>(cr.y + cr.h - 4)};
  f.d.move(from);
  f.d.down(from);
  f.d.move({from.x + 20, from.y});
  REQUIRE(f.d.tree().dragging());
  REQUIRE(f.d.tree().dragPayload()->kind == DragKind::BrowserItems);
  f.d.move(empty);
  CHECK(f.d.tree().dropFeedback().effect == DropEffect::None);
  f.d.move(to);
  CHECK(f.d.tree().dropFeedback().effect == DropEffect::Copy);
  f.d.up(to);
  CHECK(f.ctx.store().history().cursor() == h0 + 1);  // one undo step
  const std::vector<doc::DeviceId> p = f.path();
  REQUIRE(p.size() == 2);
  CHECK(p[1] == f.inst);
  const doc::DeviceLoc arp = doc::findDevice(*f.ctx.doc(), p[0]);
  REQUIRE(arp.device != nullptr);
  CHECK(doc::isNoteDevice(*arp.device));
  // The project patch: its block plays (not veiled), the cable out of it into the instrument is a notes cable.
  f.d.step();
  const ProjectPatch& pp = f.view().patch();
  const uint32_t an = pp.nodeOf(p[0]);
  REQUIRE(an != kNone);
  CHECK(pp.nodes[an].noteStage);
  CHECK(pp.nodes[an].notesOut);
  bool notesOn = false;
  for (const Edge& e : pp.edges) notesOn = notesOn || (e.from == an && e.to == pp.nodeOf(f.inst) && e.kind == EdgeKind::Notes);
  CHECK(notesOn);
  CHECK(pp.nodes[pp.nodeOf(f.inst)].instrument);
  // Undo takes it away in one step.
  REQUIRE(f.ctx.store().undo());
  CHECK(f.path() == std::vector<doc::DeviceId>{f.inst});
}
