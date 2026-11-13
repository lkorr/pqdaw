// DY.2 (docs/plans/DY.md "DY.2"): the `cablesource` panel element through REAL event dispatch (window → canvas → face →
// element → the overlay's menu): a group on Bass shows a picker bound to its main input (and an "On" value block);
// picking "Kick: Post FX" is ONE undo step (the cable from Kick's last block, On = 1), switching to "Pad: Pre FX"
// replaces it in one step, "None" clears it (On = 0); a cable drawn by hand from another track shows as the source.
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/panel.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/element.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../support/blocks.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::project_patch;

namespace {

const doc::DeviceLibrary& devLib() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

doc::ops::Edit apply(doc::DocumentStore& s, const doc::ops::EditResult& e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(s.dispatch(e->command).ok());
  return *e;
}

// Kick (MIDI: instrument → latency), Pad (audio, no blocks), Bass (MIDI) with a group "Sidechain" off its path holding a
// value block "On"; the group's panel is one cablesource element bound to the group's main input and On.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  doc::TrackId kick, pad, bass;
  doc::DeviceId lat, group, on;

  Ui() {
    ctx.setDeviceLibrary(&devLib());
    doc::DocumentStore& st = ctx.store();
    kick = apply(st, doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Kick")).tracks.at(0);
    pad = apply(st, doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, {}, "Pad")).tracks.at(0);
    bass = apply(st, doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Bass")).tracks.at(0);
    lat = test_blocks::appendAfter(st, kick, "atom:latency");
    group = apply(st, doc::ops::addDevice(*st.current(), st.ids(), bass, std::string(doc::kGroupDevice), doc::DeviceKind::AudioEffect,
                                          doc::ops::kOffPath)).devices.at(0);
    apply(st, doc::ops::renameDevice(*st.current(), group, "Sidechain"));
    on = apply(st, doc::ops::addDevice(*st.current(), st.ids(), bass, std::string(doc::kValueDevice), doc::DeviceKind::AudioEffect,
                                       doc::ops::kOffPath)).devices.at(0);
    apply(st, doc::ops::setDeviceGroup(*st.current(), st.ids(), &devLib(), on, group));
    doc::PanelElement e;
    e.kind = "cablesource";
    e.x = 8, e.y = 8, e.w = 140, e.h = 30;
    e.bindings = {doc::inputBinding(group), doc::controlBinding(on, std::string(doc::kValueParam))};
    apply(st, doc::ops::addPanelElements(*st.current(), group, {e}));
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    view().setGroupOpen(group, false);
    d.tree().layoutNow();
    const Node& n = node();
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = n.x - 40.0;
    v.y = n.y - 40.0;
    view().canvas().setView(v);
    d.tree().layoutNow();
    d.step();
    d.tree().setFocus(&view().canvas());
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }
  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const Node& node() const {
    const uint32_t n = view().patch().nodeOf(group);
    REQUIRE(n != kNone);
    return view().patch().nodes[n];
  }
  panel::GroupPanelFace& face() const {
    const uint32_t mn = view().patch().nodeOf(group);
    REQUIRE(mn != kNone);
    Face* f = view().canvas().panelOn(view().scene().sceneNode[mn]);
    REQUIRE(f != nullptr);
    auto* g = dynamic_cast<panel::GroupPanelFace*>(f);
    REQUIRE(g != nullptr);
    (void)g->sync();
    return *g;
  }
  const panel::Element& element() const {
    panel::GroupPanelFace& f = face();
    for (size_t i = 0; i < f.elementCount(); ++i)
      if (f.element(i).desc().kind == "cablesource") return f.element(i);
    FAIL("no cablesource element");
    return f.element(0);
  }
  Vec2 centre() const {
    const doc::PanelElement& e = element().desc();
    const Node& n = node();
    return view().canvas().toDevice({n.x + e.x + e.w * 0.5f, n.y + e.y + e.h * 0.5f});
  }
  std::string shown() const {
    std::string title, body;
    element().describe(face().target(), title, body);
    return body.substr(0, body.find('.'));
  }
  size_t history() const { return ctx.store().history().cursor(); }
  Menu* menu() const {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<Menu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
  void pick(std::string_view label) {
    d.click(centre());
    d.step();
    Menu* m = menu();
    REQUIRE(m != nullptr);
    int32_t item = -1;
    for (size_t i = 0; i < m->items().size(); ++i)
      if (m->items()[i].label == label) item = static_cast<int32_t>(i);
    REQUIRE_MESSAGE(item >= 0, "no menu item " << label);
    const IRect r = m->itemRect(item);
    d.click({static_cast<float>(r.x + r.w / 2), static_cast<float>(r.y + r.h / 2)});
    d.step();
  }
  std::vector<doc::WireId> sources() const { return doc::ops::crossTrackWiresInto(*ctx.doc(), &devLib(), group, ""); }
  double onValue() const { return doc::findDevice(*ctx.doc(), on).device->param(doc::kValueParam)->value; }
};

}  // namespace

TEST_CASE("DY.2 cablesource: pick / switch / clear a source through real window events, one undo step each; a cable "
          "drawn by hand shows") {
  Ui u;
  CHECK(u.shown() == "None");
  // The menu: None, then Kick's Pre / Post FX and Pad's Pre FX (not Bass: the input's own track; not the master).
  u.d.click(u.centre());
  u.d.step();
  Menu* m = u.menu();
  REQUIRE(m != nullptr);
  std::vector<std::string> labels;
  for (const MenuItem& it : m->items())
    if (!it.separator) labels.push_back(it.label);
  CHECK(labels == std::vector<std::string>{"None", "Kick: Pre FX", "Kick: Post FX", "Pad: Pre FX"});
  CHECK(m->items()[0].checked);
  u.d.press("Escape");
  u.d.step();

  const size_t h0 = u.history();
  u.pick("Kick: Post FX");
  CHECK(u.history() == h0 + 1);
  std::vector<doc::WireId> w = u.sources();
  REQUIRE(w.size() == 1);
  CHECK(doc::findWire(*u.ctx.doc(), w[0])->from == u.lat);
  CHECK(u.onValue() == doctest::Approx(1.0));
  CHECK(u.shown() == "Kick: Post FX");

  u.pick("Pad: Pre FX");  // the old cable goes in the same step
  CHECK(u.history() == h0 + 2);
  w = u.sources();
  REQUIRE(w.size() == 1);
  CHECK(doc::findWire(*u.ctx.doc(), w[0])->noteSource == u.pad);
  CHECK(u.shown() == "Pad: Pre FX");

  u.pick("None");
  CHECK(u.history() == h0 + 3);
  CHECK(u.sources().empty());
  CHECK(u.onValue() == doctest::Approx(0.0));
  CHECK(u.shown() == "None");
  REQUIRE(u.ctx.undo());
  u.d.step();
  CHECK(u.sources().size() == 1);
  CHECK(u.shown() == "Pad: Pre FX");

  // A cable drawn elsewhere (the project patch view's connect: canonical ends) from Kick's instrument shows too.
  REQUIRE(u.ctx.undo());
  REQUIRE(u.ctx.undo());
  u.d.step();
  CHECK(u.sources().empty());
  doc::Wire hand;
  hand.from = doc::mainPath(*u.ctx.doc(), *doc::findTrackPtr(*u.ctx.doc(), u.kick)).blocks.front()->id;
  hand.to = u.group;
  hand.param = "x";  // the group's main input by name: canonicalWire makes it kMainPort
  hand = doc::canonicalWire(*u.ctx.doc(), std::move(hand), &devLib());
  apply(u.ctx.store(), doc::ops::connect(*u.ctx.doc(), u.ctx.store().ids(), hand));
  u.d.step();
  CHECK(u.shown() == "Kick: Pre FX");
}
