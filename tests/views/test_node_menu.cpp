// TX.node-menu (SPEC §15 2026-11-02 "cables and the node menu"): the catalog (every atom, meta module, stock device and
// rack; project atoms make valid devices), the search (fuzzy, categories, compatibility, typed specs), the menu's keys,
// and every gesture with its undo — Tab spawn (free, at the pointer), a cable dropped on empty space (connected from an
// output), a backwards cable (W) from a param, Z (a VCA at 1) on a chain cable and on a wire, Tab insert (in and out,
// output only; input only at the signature level: no device lacks an output), Shift-splice, dissolve (a free and a
// chain block), Ctrl-release (several targets), push-apart (settled and animated: frames stop), the band growing to hold
// a block, the circuit editor's Tab. Golden: project-node-menu (the menu over the patch view). Perf: opening the menu
// with the full catalog.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/graph/atoms.h"
#include "pq/meta/registry.h"
#include "pq/meta/vca.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/circuit/node_catalog.h"
#include "pq/views/circuit/node_menu.h"
#include "pq/views/fake_transport.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"

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

// "Keys": the default synth → Utility → Saturator, with a free LFO whose `lfo` jack drives the Utility's gain; "Bass" under
// it (the default synth). Plain blocks (panels off): pins are where the model says.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  doc::TrackId keys, bass;
  doc::DeviceId synth, util, sat, lfo;
  doc::WireId lfoWire;

  Ui() {
    clearRecentNodes();
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    auto run = [&](const doc::ops::EditResult& e) {
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
      REQUIRE(st.dispatch(e->command).ok());
      return *e;
    };
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks.at(0);
    bass = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, keys, "Bass")).tracks.at(0);
    synth = doc::findTrackPtr(*st.current(), keys)->devices.at(0)->id;
    util = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "utility", doc::DeviceKind::AudioEffect)).devices.at(0);
    sat = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "saturator", doc::DeviceKind::AudioEffect)).devices.at(0);
    lfo = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "lfo_device", doc::DeviceKind::AudioEffect)).devices.at(0);
    const doc::DeviceId one[1] = {lfo};
    run(doc::ops::setDevicesFree(*st.current(), one, true));
    lfoWire = run(doc::ops::connectWire(*st.current(), st.ids(), lfo, "lfo", util, "gain")).wires.at(0);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPanelsShown(false);
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    view().focusBand(keys, false);
    d.tree().layoutNow();
    // The Keys band with room on its right (empty canvas to place nodes on), zoom 1 at most.
    {
      const Band& b = pp().bands[pp().bandOf(keys)];
      const ui::IRect r = view().canvas().rect();
      const double span = static_cast<double>(b.x1 + 420.0f - (b.x0 - 40.0f));
      CircuitCanvas::View v;
      v.zoom = std::min(1.0, static_cast<double>(r.w) / span);
      v.x = b.x0 - 40.0f;
      v.y = b.y - 40.0f;
      view().canvas().setView(v);
    }
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
  const ProjectPatch& pp() const { return view().patch(); }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Track& track(doc::TrackId t) const { return *doc::findTrackPtr(*ctx.doc(), t); }
  const doc::Device* device(doc::DeviceId id) const { return doc::findDevice(*ctx.doc(), id).device; }
  uint32_t node(doc::DeviceId id) const {
    const uint32_t n = pp().nodeOf(id);
    REQUIRE(n != kNone);
    return n;
  }
  Vec2 dev(graph::Point2 w) const { return view().canvas().toDevice(w); }
  Vec2 centre(doc::DeviceId id) const {
    const Node& n = pp().nodes[node(id)];
    return dev({n.x + n.w * 0.5f, n.y + n.h * 0.5f});
  }
  Vec2 outPin(doc::DeviceId id, std::string_view name) const {
    const Node& n = pp().nodes[node(id)];
    const int32_t k = n.output(name);
    REQUIRE(k >= 0);
    return dev({n.x + n.w, n.y + pinOffset(n, static_cast<size_t>(k))});
  }
  Vec2 inPin(doc::DeviceId id, std::string_view name) const {
    const Node& n = pp().nodes[node(id)];
    const int32_t k = n.input(name);
    REQUIRE(k >= 0);
    return dev(inputEntry(n, static_cast<size_t>(k)));
  }
  // Empty canvas right of the Keys band, level with its spine (world, device).
  graph::Point2 emptyWorld() const {
    const Band& b = pp().bands[pp().bandOf(keys)];
    return {b.x1 + 160.0f, b.y + 60.0f};
  }
  // The model edge of the chain cable a → b, or of a document wire.
  uint32_t chainEdge(doc::DeviceId a, doc::DeviceId b) const {
    for (uint32_t i = 0; i < pp().edges.size(); ++i) {
      const Edge& e = pp().edges[i];
      if (e.kind == EdgeKind::Chain && pp().nodes[e.from].device == a && pp().nodes[e.to].device == b) return i;
    }
    FAIL("no chain cable");
    return kNone;
  }
  uint32_t wireEdge(doc::WireId w) const {
    for (uint32_t i = 0; i < pp().edges.size(); ++i)
      if (pp().edges[i].kind == EdgeKind::Modulation && pp().edges[i].wire == w) return i;
    FAIL("no wire edge");
    return kNone;
  }
  // A device point on the cable of model edge `edge`, found through the canvas's own wire hit test (straight or curved).
  Vec2 onCable(uint32_t edge) const {
    const Edge& e = pp().edges[edge];
    REQUIRE(e.points.size() >= 2);
    float x0 = e.points.front().x, x1 = x0, y0 = e.points.front().y, y1 = y0;
    for (const graph::Point2& p : e.points) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
    // First the route's segments (their middles), then a grid over its box.
    for (size_t i = 0; i + 1 < e.points.size(); ++i) {
      const Vec2 m = dev({(e.points[i].x + e.points[i + 1].x) * 0.5f, (e.points[i].y + e.points[i + 1].y) * 0.5f});
      if (view().canvas().edgeAt(m) == edge && view().canvas().nodeAt(m) == kNone) return m;
    }
    for (float y = y0 - 40.0f; y <= y1 + 40.0f; y += 2.0f)
      for (float x = x0; x <= x1; x += 2.0f) {
        const Vec2 m = dev({x, y});
        if (view().canvas().edgeAt(m) == edge && view().canvas().nodeAt(m) == kNone) return m;
      }
    FAIL("no point on the cable");
    return {};
  }
  NodeMenu* menu() const {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<NodeMenu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
  // Searches the open menu and places the entry named `name` (Enter).
  void pick(std::string_view query, std::string_view name) {
    NodeMenu* m = menu();
    REQUIRE(m != nullptr);
    m->setQuery(std::string(query));
    int32_t row = -1;
    for (int32_t i = 0; i < static_cast<int32_t>(m->rows().size()); ++i) {
      const NodeMenu::Row& r = m->rows()[static_cast<size_t>(i)];
      if (r.kind == NodeMenu::Row::Kind::Entry && m->catalog().entries[r.entry].name == name) {
        row = i;
        break;
      }
    }
    REQUIRE_MESSAGE(row >= 0, "no entry " << std::string(name));
    m->setHighlighted(row);
    REQUIRE(d.press("Return") == KeyResult::Widget);
    d.step();
    view().sync();
  }
  std::vector<doc::Wire> wires() const { return doc::wiresOf(*ctx.doc()); }
  bool wired(doc::DeviceId from, std::string_view out, doc::DeviceId to, std::string_view param) const {
    for (const doc::Wire& w : wires())
      if (w.from == from && w.port == out && w.to == to && w.param == param) return true;
    return false;
  }
  doc::DeviceId newest() const {
    doc::DeviceId best;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      for (const doc::DevicePtr& d2 : t.devices)
        if (d2->id.value > best.value) best = d2->id;
    });
    return best;
  }
};

}  // namespace

TEST_CASE("node catalog: every atom, meta module, stock device and rack; project atoms make valid free devices") {
  const auto circuitCat = nodeCatalog(NodeCatalog::Scope::Circuit, nullptr);
  const auto projectCat = nodeCatalog(NodeCatalog::Scope::Project, &library());
  CHECK(nodeCatalog(NodeCatalog::Scope::Project, &library()) == projectCat);  // built once, shared
  // Every callable atom in the circuit editor's catalog.
  for (const graph::AtomSignature& s : graph::AtomRegistry::builtin().all()) {
    if (s.has(graph::atom_flags::kInternal)) continue;
    CHECK_MESSAGE(circuitCat->find("atom:" + std::string(s.name)) != nullptr, std::string(s.name));
  }
  for (const meta::Type* t : meta::types())
    if (hasFace(*t)) {
      CHECK(circuitCat->find("meta:" + std::string(t->id)) != nullptr);
      CHECK(projectCat->find("meta:" + std::string(t->id)) != nullptr);
    }
  for (const doc::DeviceInfo& d : library().catalog().devices()) CHECK(projectCat->find("device:" + d.patch) != nullptr);
  CHECK(projectCat->find("rack:" + std::string(doc::kAudioEffectRack)) != nullptr);
  CHECK(projectCat->find("rack:" + std::string(doc::kInstrumentRack)) != nullptr);
  // The analysers are visualisers; instruments have no main input.
  CHECK(projectCat->find("meta:spectrum")->category == NodeCategory::Visualisers);
  CHECK(projectCat->find("device:BasicSynth")->category == NodeCategory::Instruments);
  CHECK_FALSE(projectCat->find("device:BasicSynth")->hasIn);
  CHECK(projectCat->find("device:utility")->hasIn);
  CHECK(projectCat->find("meta:vca")->hasIn);
  // Every atom offered at the project level is a device that resolves (a free device of that one atom).
  size_t atoms = 0;
  for (const NodeEntry& e : projectCat->entries) {
    if (e.item != NodeItem::Atom) continue;
    ++atoms;
    auto sd = specDevice(e.id);
    REQUIRE_MESSAGE(sd.ok(), e.id << ": " << (sd.ok() ? "" : sd.error().message));
    doc::Device d;
    d.id = doc::DeviceId{900 + static_cast<uint32_t>(atoms)};
    d.patch = sd->patch;
    d.source = std::make_shared<const doc::UserPatch>(doc::UserPatch{sd->text, nullptr});
    const auto r = library().resolve(d);
    REQUIRE(r != nullptr);
    CHECK_MESSAGE(r->error.empty(), e.id << ": " << r->error << "\n" << sd->text);
    CHECK_MESSAGE(r->info.kind == doc::DeviceKind::AudioEffect, e.id);
  }
  CHECK(atoms >= 40);
}

TEST_CASE("node catalog: typed specs — literals, operators with an operand, atoms, params; main ports from the signature") {
  CHECK(nodeSpecValid("tanh", NodeCatalog::Scope::Project));
  CHECK(nodeSpecValid("0.5", NodeCatalog::Scope::Project));
  CHECK(nodeSpecValid("440Hz", NodeCatalog::Scope::Project));
  CHECK(nodeSpecValid("* 2", NodeCatalog::Scope::Project));
  CHECK(nodeSpecValid("param cutoff = 1kHz", NodeCatalog::Scope::Project));
  CHECK(nodeSpecValid("meta_vca", NodeCatalog::Scope::Project));
  CHECK_FALSE(nodeSpecValid("in x : audio", NodeCatalog::Scope::Project));  // a port, not a node
  CHECK(nodeSpecValid("in x : audio", NodeCatalog::Scope::Circuit));
  CHECK_FALSE(nodeSpecValid("no_such_thing", NodeCatalog::Scope::Project));
  CHECK_FALSE(nodeSpecValid("* x", NodeCatalog::Scope::Project));  // an operand is a literal
  CHECK(splitSpec("* 2").node == "*");
  CHECK(splitSpec("* 2").operand == "2");
  CHECK(splitSpec("-3dB").operand.empty());  // a negative literal, not an operator
  // A literal: an output-only device (its audio input drives nothing: no main input).
  auto half = specDevice("0.5");
  REQUIRE(half.ok());
  doc::Device d;
  d.id = doc::DeviceId{77};
  d.patch = half->patch;
  d.source = std::make_shared<const doc::UserPatch>(doc::UserPatch{half->text, nullptr});
  auto r = library().resolve(d);
  REQUIRE(r != nullptr);
  REQUIRE(r->error.empty());
  MainPorts mp = mainPortsOf(graph::subcircuitSignature(*r->root()), r->root(), false);
  CHECK(mp.in.empty());
  CHECK_FALSE(mp.out.empty());
  // `* 2`: in and out.
  auto twice = specDevice("* 2");
  REQUIRE(twice.ok());
  d.patch = twice->patch;
  d.source = std::make_shared<const doc::UserPatch>(doc::UserPatch{twice->text, nullptr});
  r = library().resolve(d);
  REQUIRE(r != nullptr);
  REQUIRE_MESSAGE(r->error.empty(), r->error);
  mp = mainPortsOf(graph::subcircuitSignature(*r->root()), r->root(), false);
  CHECK_FALSE(mp.in.empty());
  CHECK_FALSE(mp.out.empty());
  // An input-only signature (no device has one: describeDevice needs an output) has no main output.
  graph::SubcircuitRef sink;
  sink.inputs.push_back({"x", graph::portType(graph::Kind::Real, graph::Rate::Audio), false});
  mp = mainPortsOf(sink, nullptr, false);
  CHECK(mp.in == "x");
  CHECK(mp.out.empty());
  // An instrument takes the notes: no main input.
  graph::SubcircuitRef inst;
  inst.inputs.push_back({"x", graph::portType(graph::Kind::Real, graph::Rate::Audio), false});
  inst.outputs.push_back({"out", graph::portType(graph::Kind::Real, graph::Rate::Audio, graph::Poly::Voice), false});
  CHECK(mainPortsOf(inst, nullptr, true).in.empty());
}

TEST_CASE("node search: fuzzy over names, aliases and descriptions; categories; compatible first; Recent") {
  const auto c = nodeCatalog(NodeCatalog::Scope::Project, &library());
  auto first = [&](std::string_view q, NodeTab tab = NodeTab::All, NodeFilter f = NodeFilter::None) -> std::string {
    const auto hits = searchNodes(*c, q, tab, f);
    return hits.empty() ? std::string() : c->entries[hits.front().entry].name;
  };
  CHECK(first("eqe") == "EQ Eight");       // a subsequence, word starts
  CHECK(first("utl") == "Utility");
  CHECK(first("vca") == "VCA");
  CHECK(first("phase acc") == "Phase Accumulator");  // every term
  CHECK(first("*") == "mul");               // an operator's symbol (alias)
  CHECK(first("one-sample delay", NodeTab::Atoms) == "z1");  // descriptions (substrings)
  CHECK(first("spectrum", NodeTab::Visualisers) == "Spectrum");
  CHECK(first("utility", NodeTab::Atoms).empty());  // the tab filters
  CHECK(first("", NodeTab::MidiEffects).empty());   // none yet
  // Compatible first: from an output, nodes with an input before the rest (an instrument has none).
  {
    const auto hits = searchNodes(*c, "", NodeTab::All, NodeFilter::WithInput);
    bool seenIncompatible = false;
    for (const NodeHit& h : hits) {
      if (!h.compatible) seenIncompatible = true;
      else CHECK_FALSE(seenIncompatible);
      CHECK(h.compatible == c->entries[h.entry].hasIn);
    }
    CHECK(seenIncompatible);
  }
  // Recent: newest first.
  clearRecentNodes();
  noteRecentNode("meta:vca");
  noteRecentNode("device:utility");
  noteRecentNode("meta:vca");
  const auto rec = recentNodes();
  const auto hits = searchNodes(*c, "", NodeTab::Recent, NodeFilter::None, rec);
  REQUIRE(hits.size() == 2);
  CHECK(c->entries[hits[0].entry].name == "VCA");
  CHECK(c->entries[hits[1].entry].name == "Utility");
  clearRecentNodes();
}

TEST_CASE("node menu: Tab opens it at the pointer, typing goes in, keys move and switch tabs, Esc closes, Enter spawns") {
  Ui u;
  const graph::Point2 at = u.emptyWorld();
  u.d.move(u.dev(at));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  u.d.step();
  NodeMenu* m = u.menu();
  REQUIRE(m != nullptr);
  CHECK(u.d.tree().focus() == m);
  const ui::IRect r = m->rect();
  // Its top-left at the pointer, or (no room on the right) its top-right: the overlay keeps it inside the window.
  const bool atPointer = std::abs(static_cast<float>(r.x) - u.dev(at).x) <= 1.0f || std::abs(static_cast<float>(r.right()) - u.dev(at).x) <= 1.0f;
  CHECK_MESSAGE(atPointer, "menu " << r.x << "," << r.y << " " << r.w << "x" << r.h << " pointer " << u.dev(at).x << "," << u.dev(at).y);
  CHECK(r.right() <= 1600);
  // Typing goes straight in (no click on the field).
  u.d.type("utl");
  CHECK(m->query() == "utl");
  REQUIRE(m->highlightedEntry() != nullptr);
  CHECK(m->highlightedEntry()->name == "Utility");
  // Down / Up move; Tab / Shift+Tab switch the category.
  const int32_t hi = m->highlighted();
  u.d.press("Down");
  CHECK(m->highlighted() >= hi);
  u.d.press("Up");
  CHECK(m->highlighted() == hi);
  u.d.press("Tab");
  CHECK(m->tab() == NodeTab::Atoms);
  u.d.press("Shift+Tab");
  CHECK(m->tab() == NodeTab::All);
  u.d.press("Backspace");
  CHECK(m->query() == "ut");
  u.d.press("Escape");
  u.d.step();
  CHECK(u.menu() == nullptr);
  // Enter spawns the highlighted node at the pointer: a free block, one undo step.
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();
  u.d.move(u.dev(at));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  u.d.type("saturator");
  REQUIRE(u.menu() != nullptr);
  REQUIRE(u.menu()->highlightedEntry() != nullptr);
  CHECK(u.menu()->highlightedEntry()->name == "Saturator");
  u.d.press("Return");
  u.d.step();
  u.view().sync();
  CHECK(u.menu() == nullptr);
  CHECK(u.history() == h0 + 1);
  const doc::DeviceId made = u.newest();
  REQUIRE(u.device(made) != nullptr);
  CHECK(u.device(made)->patch == "saturator");
  CHECK(u.device(made)->free);
  const Node& n = u.pp().nodes[u.node(made)];
  CHECK(std::abs(n.x + n.w * 0.5f - at.x) <= 2.0f);
  CHECK(std::abs(n.y + n.h * 0.5f - at.y) <= 2.0f);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  CHECK(recentNodes().front() == "device:saturator");
  REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
}

TEST_CASE("node menu: a typed spec places that one node (`0.5`, `* 2`); an instrument goes into the chain") {
  Ui u;
  const graph::Point2 at = u.emptyWorld();
  const size_t h0 = u.history();
  u.d.move(u.dev(at));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  u.d.type("* 2");
  REQUIRE(u.menu() != nullptr);
  CHECK(u.menu()->specOffered());
  CHECK(u.menu()->rows().front().kind == NodeMenu::Row::Kind::Spec);
  u.d.press("Return");
  u.d.step();
  CHECK(u.history() == h0 + 1);
  const doc::DeviceId mul = u.newest();
  REQUIRE(u.device(mul) != nullptr);
  CHECK(u.device(mul)->edited());
  CHECK(u.device(mul)->free);
  CHECK(u.device(mul)->name == "* 2");
  const MainPorts mp = u.view().mainPorts(mul);
  CHECK_FALSE(mp.in.empty());
  CHECK_FALSE(mp.out.empty());
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // A stock instrument: the chain's first device (it replaces the instrument, as in Live), one step.
  u.d.move(u.dev(at));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  u.pick("operator", "Operator");
  CHECK(u.history() == h0 + 2);
  CHECK(u.track(u.keys).devices.front()->patch == "operator");
  CHECK_FALSE(u.track(u.keys).devices.front()->free);
  REQUIRE(u.ctx.undo());
  REQUIRE(u.ctx.undo());
  CHECK(u.history() == h0);
}

TEST_CASE("node menu: a cable from an output dropped on empty space spawns a node wired from it; one undo step") {
  Ui u;
  const size_t h0 = u.history();
  const doc::Snapshot start = u.ctx.doc();
  const graph::Point2 at = u.emptyWorld();
  u.d.drag(u.outPin(u.sat, "out"), u.dev(at), 6);
  NodeMenu* m = u.menu();
  REQUIRE(m != nullptr);
  CHECK(u.history() == h0);  // nothing yet
  // Compatible first: the first entry has an input.
  for (const NodeMenu::Row& r : m->rows())
    if (r.kind == NodeMenu::Row::Kind::Entry) {
      CHECK(r.compatible);
      CHECK(m->catalog().entries[r.entry].hasIn);
      break;
    }
  u.pick("vca", "VCA");
  CHECK(u.history() == h0 + 1);
  const doc::DeviceId vca = u.newest();
  REQUIRE(u.device(vca) != nullptr);
  CHECK(u.device(vca)->patch == "meta:vca");
  CHECK(u.device(vca)->free);
  const MainPorts mp = u.view().mainPorts(vca);
  REQUIRE_FALSE(mp.in.empty());
  CHECK(u.wired(u.sat, "out", vca, mp.in));
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
}

TEST_CASE("node menu: W + drag out of a param pin is a backwards cable — onto an output, or a spawned source's output") {
  Ui u;
  const size_t h0 = u.history();
  // The Utility's gain has a pin (its LFO wire); W held, dragged onto the Saturator's output: that output drives it.
  const Vec2 gain = u.inPin(u.util, "gain");
  u.d.keyDown('w');
  u.d.drag(gain, u.outPin(u.sat, "out"), 6);
  u.d.keyUp('w');
  CHECK(u.history() == h0 + 1);
  CHECK(u.wired(u.sat, "out", u.util, "gain"));
  // Onto empty space: the menu offers sources first; the spawned node's main output drives the param.
  u.d.keyDown('w');
  u.d.drag(u.inPin(u.util, "gain"), u.dev(u.emptyWorld()), 6);
  u.d.keyUp('w');
  NodeMenu* m = u.menu();
  REQUIRE(m != nullptr);
  for (const NodeMenu::Row& r : m->rows())
    if (r.kind == NodeMenu::Row::Kind::Entry) {
      CHECK(m->catalog().entries[r.entry].hasOut);
      break;
    }
  u.pick("phase acc", "Phase Accumulator");
  CHECK(u.history() == h0 + 2);
  const doc::DeviceId acc = u.newest();
  REQUIRE(u.device(acc) != nullptr);
  CHECK(u.device(acc)->patch == "meta:phase_acc");
  CHECK(u.wired(acc, u.view().mainPorts(acc).out, u.util, "gain"));
  for (int i = 0; i < 2; ++i) REQUIRE(u.ctx.undo());
  CHECK(u.history() == h0);
}

TEST_CASE("node menu: Z over a chain cable inserts a VCA at 1 in the chain; over a wire between its ends; one step each") {
  Ui u;
  const size_t h0 = u.history();
  const doc::Snapshot start = u.ctx.doc();
  // A VCA at its default is x1 (the sound is unchanged).
  {
    const meta::VcaLaw law = meta::vcaLaw(meta::VcaCurve::Log, -meta::kVcaRange, meta::kVcaRange, 0.4, 0.0);
    CHECK(meta::vcaValue(law, meta::kVcaDefaultPos) == doctest::Approx(1.0));
  }
  // The chain cable Utility → Saturator.
  u.d.move(u.onCable(u.chainEdge(u.util, u.sat)));
  u.d.keyDown('z');
  u.d.keyUp('z');
  u.view().sync();
  CHECK(u.history() == h0 + 1);
  const auto& list = u.track(u.keys).devices;
  REQUIRE(list.size() == 5);
  CHECK(list[1]->id == u.util);
  CHECK(list[2]->patch == "meta:vca");
  CHECK_FALSE(list[2]->free);
  CHECK(list[2]->params.empty());  // the type's default: value 0.7 (x1)
  CHECK(list[3]->id == u.sat);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // The LFO's wire into the Utility's gain: LFO → VCA → gain, the VCA free.
  u.d.move(u.onCable(u.wireEdge(u.lfoWire)));
  u.d.keyDown('z');
  u.d.keyUp('z');
  u.view().sync();
  CHECK(u.history() == h0 + 2);
  const doc::DeviceId vca = u.newest();
  REQUIRE(u.device(vca) != nullptr);
  CHECK(u.device(vca)->free);
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) == nullptr);
  const MainPorts mp = u.view().mainPorts(vca);
  CHECK(u.wired(u.lfo, "lfo", vca, mp.in));
  CHECK(u.wired(vca, mp.out, u.util, "gain"));
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // Z over nothing does nothing.
  u.d.move(u.dev(u.emptyWorld()));
  u.d.keyDown('z');
  u.d.keyUp('z');
  CHECK(u.history() == h0 + 2);
  for (int i = 0; i < 2; ++i) REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
}

TEST_CASE("node menu: Tab over a cable inserts the chosen node — in and out, output only; notes are not cut") {
  Ui u;
  const size_t h0 = u.history();
  // In and out: a Saturator into the LFO's wire.
  u.d.move(u.onCable(u.wireEdge(u.lfoWire)));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  REQUIRE(u.menu() != nullptr);
  u.pick("saturator", "Saturator");
  CHECK(u.history() == h0 + 1);
  const doc::DeviceId s2 = u.newest();
  const MainPorts mp = u.view().mainPorts(s2);
  REQUIRE_FALSE(mp.in.empty());
  CHECK(u.wired(u.lfo, "lfo", s2, mp.in));
  CHECK(u.wired(s2, mp.out, u.util, "gain"));
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) == nullptr);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // Output only: `0.5` typed into the cable Saturator → gain: the constant drives the gain, the Saturator's wire goes.
  doc::WireId into;
  for (const doc::Wire& w : u.wires())
    if (w.from == s2 && w.to == u.util) into = w.id;
  REQUIRE(into.valid());
  u.d.move(u.onCable(u.wireEdge(into)));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  u.d.type("0.5");
  REQUIRE(u.menu() != nullptr);
  REQUIRE(u.menu()->specOffered());
  u.d.press("Return");
  u.d.step();
  u.view().sync();
  CHECK(u.history() == h0 + 2);
  const doc::DeviceId half = u.newest();
  REQUIRE(u.device(half) != nullptr);
  CHECK(u.view().mainPorts(half).in.empty());
  CHECK(u.wired(half, u.view().mainPorts(half).out, u.util, "gain"));
  for (const doc::Wire& w : u.wires()) CHECK_FALSE((w.from == s2 && w.to == u.util));
  for (const doc::Wire& w : u.wires()) CHECK_FALSE(w.to == half);  // nothing into it
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // The note wire (notes → the instrument) is the chain's own: refused, nothing changes.
  uint32_t notes = kNone;
  for (uint32_t i = 0; i < u.pp().edges.size(); ++i)
    if (u.pp().edges[i].kind == EdgeKind::Notes && u.pp().nodes[u.pp().edges[i].to].device == u.synth) notes = i;
  REQUIRE(notes != kNone);
  CHECK(u.view().openInsertMenu(notes, u.dev(u.emptyWorld())) == nullptr);
  CHECK_FALSE(u.view().insertVca(notes));
  CHECK(u.history() == h0 + 2);
  for (int i = 0; i < 2; ++i) REQUIRE(u.ctx.undo());
  CHECK(u.history() == h0);
}

TEST_CASE("node menu: Shift-drag a block over a cable highlights it and splices the block in; Ctrl+Delete dissolves it") {
  Ui u;
  REQUIRE(u.view().addFreeDevice(u.keys, "saturator"));
  u.d.step();
  u.view().sync();
  const doc::DeviceId s2 = u.newest();
  const size_t h0 = u.history();
  const uint32_t edge = u.wireEdge(u.lfoWire);
  const Vec2 on = u.onCable(edge);
  const Vec2 from = u.centre(s2);
  u.d.down(from, PointerButton::Left, mod::kShift);
  u.d.move({(from.x + on.x) * 0.5f, (from.y + on.y) * 0.5f}, mod::kShift);
  u.d.move(on, mod::kShift);
  CHECK(u.view().canvas().spliceEdge() == edge);
  CHECK(u.view().canvas().selection().wires.size() == 1);  // the cable shown selected
  u.d.up(on, PointerButton::Left, mod::kShift);
  u.d.step();
  u.view().sync();
  CHECK(u.history() == h0 + 1);
  const MainPorts mp = u.view().mainPorts(s2);
  CHECK(u.wired(u.lfo, "lfo", s2, mp.in));
  CHECK(u.wired(s2, mp.out, u.util, "gain"));
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) == nullptr);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // Dissolve (Ctrl+Delete on the selected block): it goes; the LFO drives the gain again. One step.
  {
    const uint32_t sn = u.view().scene().sceneNode[u.node(s2)];
    const uint32_t one[1] = {sn};
    u.view().canvas().select(one, {});
    u.d.tree().setFocus(&u.view().canvas());
    u.d.press("Ctrl+Delete");
    u.d.step();
    u.view().sync();
  }
  CHECK(u.history() == h0 + 2);
  CHECK(u.device(s2) == nullptr);
  CHECK(u.wired(u.lfo, "lfo", u.util, "gain"));
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // A chain block: its output's wire is fed by the block before it in the chain (the chain closes up by itself).
  {
    auto e = doc::ops::connectWire(*u.ctx.doc(), u.ctx.store().ids(), u.util, "out", u.lfo, "x");
    REQUIRE(e.ok());
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
    REQUIRE(u.view().dissolve(u.util));
    CHECK(u.device(u.util) == nullptr);
    CHECK(u.wired(u.synth, "out", u.lfo, "x"));
    CHECK(u.history() == h0 + 4);
    REQUIRE(u.ctx.undo());
    REQUIRE(u.ctx.undo());
  }
  for (int i = 0; i < 2; ++i) REQUIRE(u.ctx.undo());
  CHECK(u.history() == h0);
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) != nullptr);
}

TEST_CASE("node menu: a cable released with Ctrl held keeps a new one on the pointer — several targets, one step each") {
  Ui u;
  const size_t h0 = u.history();
  // The LFO's jack onto the Saturator's input pin, Ctrl held: connected, and a cable from the jack stays on the pointer.
  const Vec2 jack = u.outPin(u.lfo, "lfo");
  const Vec2 satIn = u.inPin(u.sat, "x");
  u.d.down(jack);
  u.d.move({(jack.x + satIn.x) * 0.5f, (jack.y + satIn.y) * 0.5f});
  u.d.move(satIn, mod::kCtrl);
  u.d.up(satIn, PointerButton::Left, mod::kCtrl);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  CHECK(u.wired(u.lfo, "lfo", u.sat, "x"));
  CHECK(u.view().canvas().stickyWire());
  // A plain click on the Utility's input wires it too and ends.
  const Vec2 utilIn = u.inPin(u.util, "x");
  u.d.move(utilIn);
  u.d.click(utilIn);
  u.d.step();
  CHECK(u.history() == h0 + 2);
  CHECK(u.wired(u.lfo, "lfo", u.util, "x"));
  CHECK_FALSE(u.view().canvas().stickyWire());
  // A plain release ends (no cable kept).
  u.d.drag(u.outPin(u.lfo, "lfo"), u.inPin(u.synth, "notes") , 4);
  CHECK_FALSE(u.view().canvas().stickyWire());
  for (int i = 0; i < 2; ++i) REQUIRE(u.ctx.undo());
  CHECK(u.history() == h0);
}

TEST_CASE("node menu: push-apart — a spawn onto a block pushes them apart (settled, animated: frames stop); bands grow") {
  Ui u;
  auto rectOf = [&](doc::DeviceId id) {
    const Node& n = u.pp().nodes[u.node(id)];
    return std::array<float, 4>{n.x, n.y, n.x + n.w, n.y + n.h};
  };
  auto apart = [&](doc::DeviceId a, doc::DeviceId b) {
    const auto ra = rectOf(a), rb = rectOf(b);
    return ra[2] + ProjectPatchView::kPushGap - 1.0f <= rb[0] || rb[2] + ProjectPatchView::kPushGap - 1.0f <= ra[0] ||
           ra[3] + ProjectPatchView::kPushGap - 1.0f <= rb[1] || rb[3] + ProjectPatchView::kPushGap - 1.0f <= ra[1];
  };
  // Onto the free LFO: both move apart (settled at once while animations are off).
  const Node& ln = u.pp().nodes[u.node(u.lfo)];
  const graph::Point2 onLfo{ln.x + ln.w * 0.5f, ln.y + ln.h * 0.5f};
  const doc::DeviceId a = u.view().spawnNode(ProjectPatchView::specChoice("tanh"), onLfo);
  REQUIRE(a.valid());
  CHECK_FALSE(u.view().pushing());
  CHECK(apart(a, u.lfo));
  // Animated: frames while it moves, none once it has settled.
  u.view().setPushAnimated(true);
  const doc::DeviceId b = u.view().spawnNode(ProjectPatchView::specChoice("tanh"), onLfo);
  REQUIRE(b.valid());
  CHECK(u.view().pushing());
  const uint64_t f0 = u.d.framesDrawn();
  u.d.advance(2.0);
  CHECK_FALSE(u.view().pushing());
  CHECK(u.d.framesDrawn() > f0);
  CHECK(u.view().pushFrames() > 2);
  CHECK(u.view().pushFrames() < 240);  // settled well before the cap
  CHECK(apart(b, u.lfo));
  CHECK(apart(b, a));
  const uint64_t f1 = u.d.framesDrawn();
  u.d.advance(1.0);
  CHECK(u.d.framesDrawn() == f1);  // idle: no frames
  // The band grows to hold a block placed past its bottom edge; the band below moves down by as much.
  const uint32_t kb = u.pp().bandOf(u.keys), bb = u.pp().bandOf(u.bass);
  const float h0 = u.pp().bands[kb].h, y0 = u.pp().bands[bb].y;
  const Node& an = u.pp().nodes[u.node(a)];
  const float past = u.pp().bands[kb].y + h0 + 80.0f - (an.y + an.h);
  u.view().nudge(a.value, 0.0f, past);
  const Node& moved = u.pp().nodes[u.node(a)];
  const Band& grown = u.pp().bands[kb];
  CHECK(grown.y + grown.h >= moved.y + moved.h + metrics::kBandPad);
  CHECK(u.pp().bands[bb].y - y0 == doctest::Approx(grown.h - h0));
  CHECK(u.pp().bands[bb].y >= grown.y + grown.h);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
}

TEST_CASE("node menu: the circuit editor — Tab at the pointer, an atom, a meta module, `* 2` (the operator and its operand)") {
  TestDriver d{1200, 700};
  auto& root = d.setRoot<Widget>("root");
  root.setLayout(LayoutKind::Stack);
  (void)root.add<OverlayLayer>("overlay");
  d.tree().layoutNow();
  auto doc = PatchEditor::fromText(std::string(PQ_TEST_DATA_DIR) + "/../stdlib/editor_ui_test.pqp",
                                   "patch Drive {\n  in x : audio\n  out y : audio\n  y = tanh(x)\n}\n");
  REQUIRE(doc.ok());
  CircuitEditor* ed = openCircuitEditor(root, std::move(*doc));
  REQUIRE(ed != nullptr);
  d.tree().layoutNow();
  d.tree().setFocus(&ed->canvas());
  const size_t n0 = ed->document().drawn(0).nodeCount();
  const ui::IRect r = ed->canvas().rect();
  const Vec2 at{static_cast<float>(r.x + r.w / 3), static_cast<float>(r.y + r.h / 3)};
  auto menuOf = [&]() -> NodeMenu* {
    auto* layer = OverlayLayer::of(root);
    for (size_t i = layer->childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<NodeMenu*>(&layer->child(i))) return m;
    return nullptr;
  };
  d.move(at);
  REQUIRE(d.press("Tab") == KeyResult::Widget);
  NodeMenu* m = menuOf();
  REQUIRE(m != nullptr);
  CHECK(m->catalog().scope == NodeCatalog::Scope::Circuit);
  d.type("noise");
  REQUIRE(m->highlightedEntry() != nullptr);
  CHECK(m->highlightedEntry()->name == "noise");
  d.press("Return");
  d.step();
  CHECK(ed->document().drawn(0).nodeCount() == n0 + 1);
  {
    const graph::Patch& p = ed->document().drawn(0);
    bool found = false;
    for (graph::NodeIndex k = 0; k < p.nodeCount(); ++k)
      if (p.node(k).kind == graph::NodeKind::Atom && graph::atomSignature(p.node(k).atom).name == "noise") {
        found = true;
        CHECK(std::abs(p.layout(k).x - ed->canvas().toWorld(at).x) <= 4.0f);  // at the pointer
      }
    CHECK(found);
  }
  // A meta module: its call.
  d.move(at);
  REQUIRE(d.press("Tab") == KeyResult::Widget);
  d.type("vca");
  REQUIRE(menuOf() != nullptr);
  REQUIRE(menuOf()->highlightedEntry() != nullptr);
  d.press("Return");
  d.step();
  CHECK_MESSAGE(ed->document().text().find("meta_vca(") != std::string::npos, ed->status() << " | " << ed->document().text());
  // `* 2`: the multiply and its literal operand, one history step.
  const size_t steps = ed->document().historySize();
  d.move(at);
  REQUIRE(d.press("Tab") == KeyResult::Widget);
  d.type("* 2");
  REQUIRE(menuOf() != nullptr);
  CHECK(menuOf()->specOffered());
  d.press("Return");
  d.step();
  CHECK(ed->document().historySize() == steps + 1);
  CHECK_MESSAGE(ed->document().text().find("mul(b: 2)") != std::string::npos, ed->status() << " | " << ed->document().text());
  // A wire from `x`'s output released on empty space: the menu (nodes with an input first); the node arrives wired from
  // `x`, one history step.
  {
    const CircuitScene& sc = *ed->canvas().scene();
    const graph::Patch& p = ed->document().drawn(0);
    const auto xi = p.findByName("x");
    REQUIRE(xi.has_value());
    Vec2 from{};
    bool found = false;
    for (const ScenePin& pin : sc.pins)
      if (pin.node == *xi && !pin.input) {
        from = ed->canvas().toDevice({pin.x, pin.y});
        found = true;
      }
    REQUIRE(found);
    const Vec2 to{static_cast<float>(r.x + r.w / 2), static_cast<float>(r.bottom() - 60)};
    REQUIRE(ed->canvas().hitTest(to).kind == CanvasHit::Kind::None);
    const size_t before = ed->document().historySize();
    d.drag(from, to, 6);
    NodeMenu* wm = menuOf();
    REQUIRE(wm != nullptr);
    for (const NodeMenu::Row& row : wm->rows())
      if (row.kind == NodeMenu::Row::Kind::Entry) {
        CHECK(wm->catalog().entries[row.entry].hasIn);
        break;
      }
    d.type("abs");
    REQUIRE(wm->highlightedEntry() != nullptr);
    CHECK(wm->highlightedEntry()->name == "abs");
    d.press("Return");
    d.step();
    CHECK(ed->document().historySize() == before + 1);
    CHECK_MESSAGE(ed->document().text().find("abs(x)") != std::string::npos, ed->status() << " | " << ed->document().text());
  }
  d.tree().setRoot(std::make_unique<Widget>());
}

TEST_CASE("node menu: opening it with the full catalog is fast (within a 60 Hz frame)" * doctest::test_suite("perf")) {
  Ui u;
  const Vec2 at = u.dev(u.emptyWorld());
  double worst = 0.0;
  for (int i = 0; i < 20; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    NodeMenu* m = u.view().openSpawnMenu(at);
    u.d.tree().layoutNow();
    u.d.step();  // the frame that shows it
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(m != nullptr);
    worst = std::max(worst, i == 0 ? 0.0 : ms);  // the first one may build the catalog (cached after)
    u.d.press("Escape");
    u.d.step();
  }
  MESSAGE("node menu open + layout + step (the painted frame: pq_bench_project_patch), worst of 19: " << worst << " ms");
#if PQ_OPTIMIZED
  CHECK(worst <= 16.0);
#endif
}

TEST_CASE("render: project-node-menu matches the golden image") {
  const char* name = "project-node-menu";
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
