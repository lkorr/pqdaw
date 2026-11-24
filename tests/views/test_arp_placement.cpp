// AR.1 (docs/plans/AR.md "AR.1"; pq/document/note_devices.h "the cables", wire_types.h notesWireClash): the Arpeggiator
// placed from the Tab node menu and wired by NOTES cables only — through real window events on the project patch view:
//   Tab on empty canvas of a MIDI band, the MIDI effects tab, Enter: a free, unwired note device (one undo step); on an
//   audio band and inside a group it is refused with the reason in the status line;
//   an audio cable dragged onto its input is refused while dragging (NotAllowed, the reason) and on release; the lane's
//   notes into its input and its output into the instrument make it a stage of lowerNoteChains (the direct notes cable
//   replaced in the same undo step); half-wired it is no stage and its cable is still a notes cable;
//   Tab over a notes cable: the insert menu offers it first (Notes in and out) and a pick puts it on that cable; over
//   an audio cable it is not connectable and a pick is refused;
//   a cable out of the lane's notes dropped on empty canvas: the spawn menu, the pick arrives wired.
// The catalog's facts and filters by declared signal; golden: project-node-menu-midi (the menu's MIDI effects tab).
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "all_free_helpers.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/note_devices.h"
#include "pq/document/wire_types.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/node_catalog.h"
#include "pq/views/circuit/node_menu.h"
#include "pq/views/fake_transport.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::circuit;
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

// "Keys" (MIDI): the default instrument group → a gain atom block; "Loop" (audio) under it.
struct Fx {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  doc::TrackId keys, loop;
  doc::DeviceId inst, gain;

  Fx() {
    clearRecentNodes();
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    auto run = [&](const doc::ops::EditResult& e) {
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
      REQUIRE(st.dispatch(e->command).ok());
      return *e;
    };
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks.at(0);
    loop = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, keys, "Loop")).tracks.at(0);
    inst = doc::mainPath(*st.current(), *doc::findTrackPtr(*st.current(), keys)).blocks.at(0)->id;
    gain = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "atom:gain", doc::DeviceKind::AudioEffect)).devices.at(0);
    ctx.setSelectedTrack(keys);
    ctx.setDetailVisible(false);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    frame(keys);
    d.tree().setFocus(&view().canvas());
  }
  ~Fx() { d.tree().setRoot(std::make_unique<Widget>()); }

  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  ProjectCanvas& canvas() const { return view().canvas(); }
  const ProjectPatch& pp() const { return view().patch(); }
  const doc::Project& doc() const { return *ctx.doc(); }
  size_t history() const { return ctx.store().history().cursor(); }
  // Band `t` whole in the canvas (zoom 1 at most), with room on its left (the tuning block hangs outside the band).
  void frame(doc::TrackId t) {
    view().sync();
    const Band& b = pp().bands[pp().bandOf(t)];
    const IRect r = canvas().rect();
    CircuitCanvas::View v;
    v.zoom = std::min({1.0, static_cast<double>(r.w) / static_cast<double>(b.x1 - b.x0 + 320.0f),
                       static_cast<double>(r.h) / static_cast<double>(b.h + 100.0f)});
    v.x = b.x0 - 260.0f;
    v.y = b.y - 50.0f;
    canvas().setView(v);
    d.tree().layoutNow();
    d.step();
  }
  uint32_t node(doc::DeviceId id) const {
    const uint32_t n = pp().nodeOf(id);
    REQUIRE(n != kNone);
    return n;
  }
  Vec2 dev(graph::Point2 w) const { return canvas().toDevice(w); }
  Vec2 mainIn(doc::DeviceId id) const {
    const Node& n = pp().nodes[node(id)];
    REQUIRE(n.mainIn >= 0);
    return dev(canvas().portWorld(node(id), static_cast<uint32_t>(n.mainIn), true));
  }
  Vec2 pinIn(doc::DeviceId id, std::string_view name) const {
    const int32_t k = pp().nodes[node(id)].input(name);
    REQUIRE(k >= 0);
    return dev(canvas().portWorld(node(id), static_cast<uint32_t>(k), true));
  }
  Vec2 mainOut(doc::DeviceId id) const { return dev(canvas().portWorld(node(id), 0, false)); }
  // A spot on the band's lane's top edge where no cable leaves it (a press there starts a cable of the track's notes).
  Vec2 laneTop(doc::TrackId t) const {
    const Band& band = pp().bands[pp().bandOf(t)];
    for (float x = pp().laneX0 + 60.0f; x < pp().laneX1; x += 6.0f) {
      const Vec2 p = dev({x, band.laneY});
      if (canvas().rect().contains(p) && canvas().laneEdgeAt(p).kind == ProjectCanvas::LaneEdgeHit::Kind::Top) return p;
    }
    FAIL("no free spot on the lane's top edge");
    return {};
  }
  // An empty point of band `t`'s circuit (no block, pin or cable there), on screen.
  Vec2 emptyPoint(doc::TrackId t) const {
    const Band& b = pp().bands[pp().bandOf(t)];
    for (float y = b.y + metrics::kTitle + 20.0f; y < b.laneY - 12.0f; y += 6.0f)
      for (float x = b.x0 + 40.0f; x < b.x1 + 400.0f; x += 8.0f) {
        const Vec2 p = dev({x, y});
        if (!canvas().rect().contains(p)) continue;
        bool clear = canvas().nodeAt(p) == kNone && !canvas().pinAt(p).valid() && canvas().edgeAt(p) == kNone;
        // (room for the block: nothing within its half size either)
        for (const float dx : {-90.0f, 90.0f})
          for (const float dy : {-30.0f, 30.0f}) clear = clear && canvas().nodeAt({p.x + dx, p.y + dy}) == kNone;
        if (clear) return p;
      }
    FAIL("no empty point in the band");
    return {};
  }
  NodeMenu* menu() const {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<NodeMenu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
  int32_t rowOf(const NodeMenu& m, std::string_view name) const {
    for (int32_t i = 0; i < static_cast<int32_t>(m.rows().size()); ++i) {
      const NodeMenu::Row& r = m.rows()[static_cast<size_t>(i)];
      if (r.kind == NodeMenu::Row::Kind::Entry && m.catalog().entries[r.entry].name == name) return i;
    }
    return -1;
  }
  // Enter on the open menu's row of `name`.
  void pick(std::string_view name) {
    NodeMenu* m = menu();
    REQUIRE(m != nullptr);
    const int32_t row = rowOf(*m, name);
    REQUIRE_MESSAGE(row >= 0, "no entry " << std::string(name));
    m->setHighlighted(row);
    REQUIRE(d.press("Return") == KeyResult::Widget);
    d.step();
    view().sync();
  }
  std::vector<doc::DeviceId> path() const {
    std::vector<doc::DeviceId> v;
    for (const doc::Device* x : doc::mainPath(doc(), *doc::findTrackPtr(doc(), keys)).blocks) v.push_back(x->id);
    return v;
  }
  // The note devices of the project, newest last.
  std::vector<doc::DeviceId> noteDevices() const {
    std::vector<doc::DeviceId> v;
    doc::forEachDevice(doc(), [&](const doc::Device& x, const doc::Track&) {
      if (doc::isNoteDevice(x)) v.push_back(x.id);
    });
    std::sort(v.begin(), v.end(), [](doc::DeviceId a, doc::DeviceId b) { return a.value < b.value; });
    return v;
  }
  const doc::Wire* wire(doc::DeviceId from, doc::TrackId notes, doc::DeviceId to) const {
    for (const doc::Wire& w : doc::wiresOf(doc()))
      if (w.from == from && w.noteSource == notes && w.to == to && w.port.empty() && w.param.empty()) return &w;
    return nullptr;
  }
  // The drawn cable into `to` out of `from` (kNone: the band's tuning block, where a lane's notes cable goes on from).
  uint32_t cable(doc::DeviceId from, doc::DeviceId to) const {
    const uint32_t tn = node(to);
    const uint32_t fn = from.valid() ? node(from) : pp().bands[pp().nodes[tn].band].tuning;
    for (uint32_t i = 0; i < pp().edges.size(); ++i)
      if (pp().edges[i].from == fn && pp().edges[i].to == tn && pp().edges[i].drawnTo == kNone) return i;
    return kNone;
  }
  // A device point on model edge `edge` (the canvas's own wire hit test), the cable brought to the canvas's middle
  // first (hit tests are of what is on screen).
  Vec2 onCable(uint32_t edge) {
    const Edge& e = pp().edges[edge];
    REQUIRE(e.points.size() >= 2);
    float x0 = e.points.front().x, x1 = x0, y0 = e.points.front().y, y1 = y0;
    for (const graph::Point2& p : e.points) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
    {
      CircuitCanvas::View v = canvas().view();
      const IRect r = canvas().rect();
      const float s = canvas().deviceScale();
      v.x = (x0 + x1) * 0.5f - static_cast<float>(r.w) * 0.5f / s;
      v.y = (y0 + y1) * 0.5f - static_cast<float>(r.h) * 0.5f / s;
      canvas().setView(v);
      d.tree().layoutNow();
      d.step();
    }
    const Vec2 a = dev({x0, y0}), z = dev({x1, y1});
    for (float y = a.y - 60.0f; y <= z.y + 60.0f; y += 1.0f)
      for (float x = a.x - 8.0f; x <= z.x + 8.0f; x += 1.0f)
        if (canvas().rect().contains({x, y}) && canvas().edgeAt({x, y}) == edge) return {x, y};
    FAIL("no point on the cable (" << x0 << "," << y0 << " .. " << x1 << "," << y1 << "; device " << a.x << "," << a.y << " .. " << z.x << "," << z.y << ")");
    return {};
  }
  // A cable dragged from `from` to `to` by pointer events, released there. `over`: called at `to` before the release.
  template <class F>
  void dragCable(Vec2 from, Vec2 to, F&& over) {
    d.move(from);
    d.down(from);
    for (int k = 1; k <= 12; ++k) {
      const float t = static_cast<float>(k) / 12.0f;
      d.move({from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t});
    }
    d.move(to);
    INFO("cable drag: " << static_cast<int>(canvas().drag()) << " to " << to.x << "," << to.y << " node " << canvas().nodeAt(to));
    over();
    d.up(to);
    d.step();
    view().sync();
  }
  void dragCable(Vec2 from, Vec2 to) {
    dragCable(from, to, [] {});
  }
  // Tab at `at`, the menu open.
  NodeMenu& tabAt(Vec2 at) {
    d.move(at);
    REQUIRE(d.press("Tab") == KeyResult::Widget);
    d.step();
    NodeMenu* m = menu();
    REQUIRE(m != nullptr);
    return *m;
  }
  doc::DeviceId placeFree() {
    const size_t n0 = noteDevices().size();
    (void)tabAt(emptyPoint(keys));
    pick("Arpeggiator");
    const std::vector<doc::DeviceId> all = noteDevices();
    REQUIRE_MESSAGE(all.size() == n0 + 1, "status: " << ctx.status());
    frame(keys);
    return all.back();
  }
};

bool stageOf(const doc::Project& p, doc::DeviceId device) {
  for (const doc::LoweredNoteChain& c : doc::lowerNoteChains(p))
    for (const doc::LoweredNoteStage& s : c.stages)
      if (s.device == device) return true;
  return false;
}

}  // namespace

TEST_CASE("arp placement: the catalog lists the note device types as MIDI effects, by their declared signals") {
  const auto c = nodeCatalog(NodeCatalog::Scope::Project, &library());
  const NodeEntry* e = c->find("device:" + doc::noteDevicePatch("arpeggiator"));
  REQUIRE(e != nullptr);
  CHECK(e->name == "Arpeggiator");
  CHECK(e->category == NodeCategory::MidiEffects);
  CHECK(e->item == NodeItem::Device);
  CHECK(e->id == doc::noteDevicePatch("arpeggiator"));
  CHECK(e->inputs == std::vector<std::string>{"notes"});
  CHECK(e->outputs == std::vector<std::string>{"out"});
  CHECK(e->notesIn);
  CHECK(e->notesOut);
  CHECK_FALSE(e->description.empty());
  CHECK_FALSE(e->params.empty());
  // One entry per registered type, all in that tab; none in the circuit editor's catalog.
  const auto midi = searchNodes(*c, "", NodeTab::MidiEffects, NodeFilter::None);
  CHECK(midi.size() == doc::noteDeviceTypes().size());
  const auto circuit = nodeCatalog(NodeCatalog::Scope::Circuit, &library());
  CHECK(searchNodes(*circuit, "", NodeTab::MidiEffects, NodeFilter::None).empty());
  // The filters: a plain cable never offers a Notes port; a notes cable takes only Notes in and out; a notes output's
  // cable what takes notes (no atom).
  CHECK_FALSE(compatible(*e, NodeFilter::WithInput));
  CHECK_FALSE(compatible(*e, NodeFilter::WithOutput));
  CHECK_FALSE(compatible(*e, NodeFilter::WithBoth));
  CHECK(compatible(*e, NodeFilter::FromNotes));
  CHECK(compatible(*e, NodeFilter::TakesNotes));
  CHECK(compatible(*e, NodeFilter::NotesThrough));
  size_t through = 0, takes = 0, atoms = 0;
  for (const NodeEntry& x : c->entries) {
    through += compatible(x, NodeFilter::NotesThrough) ? 1 : 0;
    takes += compatible(x, NodeFilter::TakesNotes) ? 1 : 0;
    if (x.item == NodeItem::Atom) {
      ++atoms;
      CHECK_FALSE(compatible(x, NodeFilter::TakesNotes));
    }
  }
  CHECK(through == doc::noteDeviceTypes().size());
  CHECK(takes > through);  // instruments, racks, modules
  CHECK(atoms > 0);
}

TEST_CASE("arp placement: Tab places it free on a MIDI band; notes cables at both ends make it a stage; an audio cable "
          "is refused (real dispatch)") {
  Fx f;
  const size_t h0 = f.history();
  // Tab on empty canvas, the MIDI effects tab (a click), Enter.
  NodeMenu& m = f.tabAt(f.emptyPoint(f.keys));
  const IRect tab = m.tabRect(NodeTab::MidiEffects);
  f.d.click({static_cast<float>(tab.x + tab.w / 2), static_cast<float>(tab.y + tab.h / 2)});
  CHECK(m.tab() == NodeTab::MidiEffects);
  REQUIRE(m.highlightedEntry() != nullptr);
  CHECK(m.highlightedEntry()->name == "Arpeggiator");
  REQUIRE(f.d.press("Return") == KeyResult::Widget);
  f.d.step();
  f.view().sync();
  CHECK(f.menu() == nullptr);
  CHECK(f.history() == h0 + 1);
  REQUIRE_MESSAGE(f.noteDevices().size() == 1, "status: " << f.ctx.status());
  const doc::DeviceId arp = f.noteDevices().front();
  CHECK(all_free::offPath(f.doc(), arp));  // free
  for (const doc::Wire& w : doc::wiresOf(f.doc())) CHECK((w.from != arp && w.to != arp));  // unwired
  CHECK(doc::lowerNoteChains(f.doc()).empty());
  CHECK(recentNodes().front() == "device:" + doc::noteDevicePatch("arpeggiator"));
  f.frame(f.keys);
  const std::vector<doc::DeviceId> direct{f.inst, f.gain};
  REQUIRE(f.path() == direct);

  // An audio cable (the gain block's output) onto its input: refused while dragging and on release.
  const std::string notesIn(doc::wireClashText(doc::WireClash::NotesInput));
  f.dragCable(f.mainOut(f.gain), f.mainIn(arp), [&] {
    CHECK(f.canvas().drag() == ProjectCanvas::Drag::Wire);
    CHECK(f.canvas().cursorAt(f.mainIn(arp)) == Cursor::NotAllowed);
    CHECK(f.ctx.status() == notesIn);
  });
  CHECK(f.history() == h0 + 1);
  CHECK(f.ctx.status() == notesIn);
  // Its output onto a param (the gain block's g) and onto the gain block's audio input: refused.
  const std::string notesOut(doc::wireClashText(doc::WireClash::NotesOutput));
  f.ctx.setStatus("");
  f.dragCable(f.mainOut(arp), f.pinIn(f.gain, "g"), [&] { CHECK(f.canvas().cursorAt(f.pinIn(f.gain, "g")) == Cursor::NotAllowed); });
  CHECK(f.history() == h0 + 1);
  CHECK(f.ctx.status() == notesOut);
  f.ctx.setStatus("");
  f.dragCable(f.mainOut(arp), f.mainIn(f.gain));
  CHECK(f.history() == h0 + 1);
  CHECK(f.ctx.status() == notesOut);
  for (const doc::Wire& w : doc::wiresOf(f.doc())) CHECK((w.from != arp && w.to != arp));

  // The lane's notes into its input: a notes cable (amber); the instrument keeps its direct cable, and the notes.
  f.dragCable(f.laneTop(f.keys), f.mainIn(arp), [&] { CHECK(f.canvas().cursorAt(f.mainIn(arp)) != Cursor::NotAllowed); });
  REQUIRE_MESSAGE(f.history() == h0 + 2, "status: " << f.ctx.status());
  REQUIRE(f.wire({}, f.keys, arp) != nullptr);
  CHECK(f.wire({}, f.keys, f.inst) != nullptr);
  CHECK(f.path() == direct);
  CHECK_FALSE(stageOf(f.doc(), arp));
  {
    const uint32_t in = f.cable({}, arp);
    REQUIRE(in != kNone);
    CHECK(f.pp().edges[in].kind == EdgeKind::Notes);
    CHECK_FALSE(f.pp().nodes[f.node(arp)].notesOut);
  }
  // Its output into the instrument: the stage (the direct cable replaced, the same undo step).
  const doc::Snapshot half = f.ctx.doc();
  f.dragCable(f.mainOut(arp), f.mainIn(f.inst), [&] { CHECK(f.canvas().cursorAt(f.mainIn(f.inst)) != Cursor::NotAllowed); });
  REQUIRE_MESSAGE(f.history() == h0 + 3, "status: " << f.ctx.status());
  REQUIRE(f.wire(arp, {}, f.inst) != nullptr);
  CHECK(f.wire({}, f.keys, f.inst) == nullptr);
  const std::vector<doc::DeviceId> through{arp, f.inst, f.gain};
  CHECK(f.path() == through);
  {
    const std::vector<doc::LoweredNoteChain> chains = doc::lowerNoteChains(f.doc());
    REQUIRE(chains.size() == 1);
    CHECK(chains[0].track == f.keys);
    REQUIRE(chains[0].stages.size() == 1);
    CHECK(chains[0].stages[0].device == arp);
    const uint32_t out = f.cable(arp, f.inst);
    REQUIRE(out != kNone);
    CHECK(f.pp().edges[out].kind == EdgeKind::Notes);
    CHECK(f.pp().nodes[f.node(arp)].notesOut);
    CHECK(f.pp().nodes[f.node(f.inst)].instrument);
  }
  REQUIRE(f.ctx.undo());
  CHECK(f.ctx.doc() == half);
  // Only its output wired: a notes cable all the same, no stage, the instrument plays its direct notes.
  REQUIRE(f.ctx.undo());
  f.view().sync();
  f.d.step();
  f.dragCable(f.mainOut(arp), f.mainIn(f.inst));
  REQUIRE_MESSAGE(f.wire(arp, {}, f.inst) != nullptr, "status: " << f.ctx.status());
  CHECK(f.wire({}, f.keys, f.inst) != nullptr);
  CHECK(f.path() == direct);
  CHECK_FALSE(stageOf(f.doc(), arp));
  const uint32_t out = f.cable(arp, f.inst);
  REQUIRE(out != kNone);
  CHECK(f.pp().edges[out].kind == EdgeKind::Notes);
}

TEST_CASE("arp placement: refused on an audio band and inside a group, with the reason (real dispatch)") {
  Fx f;
  const doc::Snapshot start = f.ctx.doc();
  const size_t h0 = f.history();
  // An audio band: no notes reach it.
  f.frame(f.loop);
  (void)f.tabAt(f.emptyPoint(f.loop));
  f.d.type("arpeggiator");
  REQUIRE(f.menu()->highlightedEntry() != nullptr);
  CHECK(f.menu()->highlightedEntry()->name == "Arpeggiator");
  f.d.press("Return");
  f.d.step();
  CHECK(f.ctx.doc() == start);
  CHECK(f.history() == h0);
  CHECK(f.ctx.status().find("MIDI track") != std::string::npos);
  // Inside the opened instrument group.
  f.view().setGroupOpen(f.inst, true);
  f.frame(f.keys);
  const uint32_t cap = f.node(f.inst);
  REQUIRE(f.pp().nodes[cap].groupOpen);
  Vec2 inside{};
  bool found = false;
  const Node& c = f.pp().nodes[cap];
  for (float y = c.y + metrics::kCapsuleTitle + 16.0f; !found && y < c.y + c.h - 16.0f; y += 6.0f)
    for (float x = c.x + 64.0f; !found && x < c.x + c.w - 16.0f; x += 6.0f) {
      const Vec2 p = f.dev({x, y});
      if (f.canvas().rect().contains(p) && f.canvas().nodeAt(p) == cap && !f.canvas().pinAt(p).valid() && f.canvas().edgeAt(p) == kNone) {
        inside = p;
        found = true;
      }
    }
  REQUIRE(found);
  f.ctx.setStatus("");
  (void)f.tabAt(inside);
  f.pick("Arpeggiator");
  CHECK(f.ctx.doc() == start);
  CHECK(f.history() == h0);
  CHECK_MESSAGE(f.ctx.status().find("not in a group") != std::string::npos, "status: " << f.ctx.status());
}

TEST_CASE("arp placement: Tab over a notes cable inserts it there; over an audio cable it is refused (real dispatch)") {
  Fx f;
  const size_t h0 = f.history();
  const uint32_t notes = f.cable({}, f.inst);
  REQUIRE(notes != kNone);
  REQUIRE(f.pp().edges[notes].kind == EdgeKind::Notes);
  NodeMenu& m = f.tabAt(f.onCable(notes));
  // The insert menu: what has Notes in and out first, everything else not connectable.
  REQUIRE(m.highlightedEntry() != nullptr);
  CHECK(m.highlightedEntry()->name == "Arpeggiator");
  size_t connectable = 0;
  for (const NodeMenu::Row& r : m.rows())
    if (r.kind == NodeMenu::Row::Kind::Entry && r.compatible) {
      ++connectable;
      CHECK(m.catalog().entries[r.entry].notesIn);
      CHECK(m.catalog().entries[r.entry].notesOut);
    }
  CHECK(connectable == doc::noteDeviceTypes().size());
  REQUIRE(f.d.press("Return") == KeyResult::Widget);
  f.d.step();
  f.view().sync();
  REQUIRE_MESSAGE(f.noteDevices().size() == 1, "status: " << f.ctx.status());
  const doc::DeviceId arp = f.noteDevices().front();
  CHECK(f.history() == h0 + 1);  // one undo step
  const std::vector<doc::DeviceId> through{arp, f.inst, f.gain};
  CHECK(f.path() == through);
  CHECK(stageOf(f.doc(), arp));
  CHECK(f.wire({}, f.keys, arp) != nullptr);
  CHECK(f.wire(arp, {}, f.inst) != nullptr);
  CHECK(f.wire({}, f.keys, f.inst) == nullptr);
  f.frame(f.keys);
  {  // In its band: not hanging out of its left edge over the tuning block (the cable it went into is short).
    const Band& bb = f.pp().bands[f.pp().bandOf(f.keys)];
    const Node& an = f.pp().nodes[f.node(arp)];
    CHECK(an.x >= bb.x0);
    CHECK(an.y >= bb.y + metrics::kTitle);
  }
  // A second one on the cable out of the first (a notes cable too).
  const uint32_t on = f.cable(arp, f.inst);
  REQUIRE(on != kNone);
  REQUIRE(f.pp().edges[on].kind == EdgeKind::Notes);
  (void)f.tabAt(f.onCable(on));
  f.pick("Arpeggiator");
  REQUIRE_MESSAGE(f.noteDevices().size() == 2, "status: " << f.ctx.status());
  const doc::DeviceId arp2 = f.noteDevices().back();
  const std::vector<doc::DeviceId> two{arp, arp2, f.inst, f.gain};
  CHECK(f.path() == two);
  CHECK(f.history() == h0 + 2);
  REQUIRE(f.ctx.undo());
  REQUIRE(f.ctx.undo());
  f.view().sync();
  f.frame(f.keys);
  CHECK(f.noteDevices().empty());
  // The audio cable instrument → gain: not connectable there, and a pick is refused with the reason.
  const doc::Snapshot before = f.ctx.doc();
  const uint32_t audio = f.cable(f.inst, f.gain);
  REQUIRE(audio != kNone);
  REQUIRE(f.pp().edges[audio].kind == EdgeKind::Chain);
  NodeMenu& m2 = f.tabAt(f.onCable(audio));
  const int32_t row = f.rowOf(m2, "Arpeggiator");
  REQUIRE(row >= 0);
  CHECK_FALSE(m2.rows()[static_cast<size_t>(row)].compatible);
  f.pick("Arpeggiator");
  CHECK(f.ctx.doc() == before);
  CHECK_MESSAGE(f.ctx.status().find(std::string(doc::wireClashText(doc::WireClash::NotesInput))) != std::string::npos,
                "status: " << f.ctx.status());
}

TEST_CASE("arp placement: the lane's notes dropped on empty canvas open the menu; the pick arrives wired (real dispatch)") {
  Fx f;
  const size_t h0 = f.history();
  const Vec2 to = f.emptyPoint(f.keys);
  f.dragCable(f.laneTop(f.keys), to);
  NodeMenu* m = f.menu();
  REQUIRE_MESSAGE(m != nullptr, "status: " << f.ctx.status());
  const int32_t row = f.rowOf(*m, "Arpeggiator");
  REQUIRE(row >= 0);
  CHECK(m->rows()[static_cast<size_t>(row)].compatible);
  f.pick("Arpeggiator");
  REQUIRE_MESSAGE(f.noteDevices().size() == 1, "status: " << f.ctx.status());
  const doc::DeviceId arp = f.noteDevices().front();
  CHECK(f.history() == h0 + 1);
  CHECK(f.wire({}, f.keys, arp) != nullptr);
  CHECK(f.wire({}, f.keys, f.inst) != nullptr);  // half-wired: the instrument keeps its notes
  CHECK_FALSE(stageOf(f.doc(), arp));
  f.frame(f.keys);
  // Its output dropped on empty canvas: what takes notes is connectable (no atom, no mixer-bound effect atom).
  const Vec2 to2 = f.emptyPoint(f.keys);
  f.dragCable(f.mainOut(arp), to2);
  m = f.menu();
  REQUIRE_MESSAGE(m != nullptr, "status: " << f.ctx.status());
  for (const NodeMenu::Row& r : m->rows())
    if (r.kind == NodeMenu::Row::Kind::Entry) CHECK(r.compatible == m->catalog().entries[r.entry].takesNotes);
  CHECK(m->rows()[static_cast<size_t>(f.rowOf(*m, "Arpeggiator"))].compatible);
  f.d.press("Escape");
  f.d.step();
  CHECK(f.menu() == nullptr);
  CHECK(f.history() == h0 + 1);
}

TEST_CASE("render: project-node-menu-midi matches the golden image") {
  const char* name = "project-node-menu-midi";
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(name, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(name, {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + name + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(name) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(name, *img, *ref, true, &d));
  MESSAGE(std::string(name) << ": " << pq::ui::test::describe(d));
}
