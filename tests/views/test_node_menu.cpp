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

#include "../support/blocks.h"
#include "all_free_helpers.h"
#include "pq/core/alloc_trap.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/graph/atoms.h"
#include "pq/meta/registry.h"
#include "pq/document/modules.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/cables.h"
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

// "Keys": the default instrument group → a gain block (`util`: x · g) → a tanh block (`sat`), with a free LFO module whose
// output (its group's `y`) drives the gain block's `g`; "Bass" under it (the default instrument group). GU.11: the stock
// Utility and Saturator were leaves with param pins; their stand-ins here are atom blocks (inputs as pins: x and g, out),
// the LFO is its module (a closed group: its panel, main ports x / y).
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
    synth = doc::mainPath(*st.current(), *doc::findTrackPtr(*st.current(), keys)).blocks.at(0)->id;
    util = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "atom:gain", doc::DeviceKind::AudioEffect)).devices.at(0);
    sat = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "atom:tanh", doc::DeviceKind::AudioEffect)).devices.at(0);
    lfo = test_blocks::addModule(st, keys, "lfo", doc::ops::kOffPath);
    lfoWire = run(doc::ops::connectWire(*st.current(), st.ids(), lfo, "y", util, "g")).wires.at(0);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    view().focusBand(keys, false);
    d.tree().layoutNow();
    fit();
    d.tree().setFocus(&view().canvas());
  }
  // The Keys band with room on its right (empty canvas to place nodes on), zoom 1 at most.
  void fit() {
    view().sync();
    const Band& b = pp().bands[pp().bandOf(keys)];
    const ui::IRect r = view().canvas().rect();
    const double span = static_cast<double>(b.x1 + 420.0f - (b.x0 - 40.0f));
    CircuitCanvas::View v;
    v.zoom = std::min({1.0, static_cast<double>(r.w) / span, static_cast<double>(r.h) / static_cast<double>(b.h + 80.0f)});
    v.x = b.x0 - 40.0f;
    v.y = b.y - 40.0f;
    view().canvas().setView(v);
    d.tree().layoutNow();
    d.step();
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
    // TX.orbit-ports: where the port is drawn (an orbiting main port sits on the block's outline).
    return dev(view().canvas().portWorld(node(id), static_cast<uint32_t>(k), false));
  }
  Vec2 inPin(doc::DeviceId id, std::string_view name) const {
    const Node& n = pp().nodes[node(id)];
    const int32_t k = n.input(name);
    REQUIRE(k >= 0);
    // TX.orbit-ports: where the port is drawn (an orbiting main port sits on the block's outline).
    return dev(view().canvas().portWorld(node(id), static_cast<uint32_t>(k), true));
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
  // The drawn cable of document wire `w`, of any kind (a main link too).
  uint32_t cableOf(doc::WireId w) const {
    for (uint32_t i = 0; i < pp().edges.size(); ++i)
      if (pp().edges[i].wire == w) return i;
    FAIL("no cable for the wire");
    return kNone;
  }
  // Whether nothing but an opened capsule is under device point `m` (its contents are drawn over it).
  bool clear(Vec2 m) const {
    const uint32_t n = view().canvas().nodeAt(m);
    return n == kNone || (pp().nodes[n].group && pp().nodes[n].groupOpen);
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
      if (view().canvas().edgeAt(m) == edge && clear(m)) return m;
    }
    for (float y = y0 - 40.0f; y <= y1 + 40.0f; y += 2.0f)
      for (float x = x0; x <= x1; x += 2.0f) {
        const Vec2 m = dev({x, y});
        if (view().canvas().edgeAt(m) == edge && clear(m)) return m;
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
    // (A module and a native engine may share a name: the module is preferred.)
    int32_t row = -1;
    for (const bool module : {true, false}) {
      for (int32_t i = 0; row < 0 && i < static_cast<int32_t>(m->rows().size()); ++i) {
        const NodeMenu::Row& r = m->rows()[static_cast<size_t>(i)];
        if (r.kind == NodeMenu::Row::Kind::Entry && m->catalog().entries[r.entry].name == name &&
            (!module || m->catalog().entries[r.entry].key.starts_with(doc::kModuleItemPrefix)))
          row = i;
      }
      if (row >= 0) break;
    }
    REQUIRE_MESSAGE(row >= 0, "no entry " << std::string(name));
    m->setHighlighted(row);
    REQUIRE(d.press("Return") == KeyResult::Widget);
    d.step();
    view().sync();
  }
  std::vector<doc::Wire> wires() const { return doc::wiresOf(*ctx.doc()); }
  // TX.all-free: a wire names a main port "" (doc::kMainPort); compared by the port it stands for.
  bool wired(doc::DeviceId from, std::string_view out, doc::DeviceId to, std::string_view param) const {
    for (const doc::Wire& w : wires()) {
      if (w.from != from || w.to != to) continue;
      const std::string outName = w.port.empty() ? std::string(view().mainPorts(from).out) : w.port;
      const std::string into = w.param.empty() ? std::string(view().mainPorts(to).in) : w.param;
      if (outName == out && into == param) return true;
    }
    return false;
  }
  // The track's main path (TX.all-free: what the chain was), in signal order.
  std::vector<doc::DeviceId> path(doc::TrackId t) const {
    std::vector<doc::DeviceId> v;
    for (const doc::Device* x : doc::mainPath(*ctx.doc(), track(t)).blocks) v.push_back(x->id);
    return v;
  }
  doc::DeviceId newest() const {
    doc::DeviceId best;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      for (const doc::DevicePtr& d2 : t.devices)
        if (d2->id.value > best.value) best = d2->id;
    });
    // GU.4: a module spawns its group with its blocks after it (at any depth: an Operator's operators are groups of
    // modules): the newest spawn is the outermost group of the newest block.
    if (const doc::DeviceLoc l = doc::findDevice(*ctx.doc(), best); l.device != nullptr && l.device->group.valid())
      best = doc::outermostGroup(l.track->devices, best);
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
      // GU.7: in a project an engine with a module is spawned as it (the group around the engine)
      if (doc::moduleOfEngine(t->id) != nullptr) {
        CHECK(projectCat->find("meta:" + std::string(t->id)) == nullptr);
        CHECK(projectCat->find("module:" + std::string(t->id)) != nullptr);
      } else {
        CHECK(projectCat->find("meta:" + std::string(t->id)) != nullptr);
      }
    }
  for (const doc::ModuleInfo& m : doc::moduleInfos()) CHECK(projectCat->find("module:" + std::string(m.key)) != nullptr);
  CHECK(projectCat->find("device:" + std::string(doc::kValueDevice)) != nullptr);  // the value block
  CHECK(projectCat->find("rack:" + std::string(doc::kAudioEffectRack)) != nullptr);
  CHECK(projectCat->find("rack:" + std::string(doc::kInstrumentRack)) != nullptr);
  // The analysers are visualisers; instruments have no main input.
  CHECK(projectCat->find("module:spectrum")->category == NodeCategory::Visualisers);
  CHECK(projectCat->find("module:wavetable")->category == NodeCategory::MetaModules);
  CHECK(projectCat->find("module:operator")->category == NodeCategory::Instruments);
  CHECK(projectCat->find("module:operator")->hasIn);  // (GU.11: an instrument is a group: the notes enter by its main input)
  CHECK(projectCat->find("module:utility")->hasIn);
  CHECK(projectCat->find("module:vca")->hasIn);  // GU.4: the VCA is a module (its group preset)
  CHECK(projectCat->find("meta:vca") == nullptr);
  CHECK(projectCat->find("module:phase_acc")->category == NodeCategory::MetaModules);
  // Every atom offered at the project level is an atom block whose patch resolves (GU.11: "atom:<name>").
  size_t atoms = 0;
  for (const NodeEntry& e : projectCat->entries) {
    if (e.item != NodeItem::Atom) continue;
    ++atoms;
    auto patch = specBlockPatch(e.id);
    REQUIRE_MESSAGE(patch.ok(), e.id << ": " << (patch.ok() ? "" : patch.error().message));
    CHECK(*patch == std::string(doc::kAtomDevicePrefix) + e.id);
    doc::Device d;
    d.id = doc::DeviceId{900 + static_cast<uint32_t>(atoms)};
    d.patch = *patch;
    CHECK_MESSAGE(library().patchOf(d) != nullptr, e.id);
    const doc::DeviceInfo* info = library().infoOf(d);
    REQUIRE_MESSAGE(info != nullptr, e.id);
    CHECK_MESSAGE(info->kind == doc::DeviceKind::AudioEffect, e.id);
  }
  CHECK(atoms >= 40);
}

TEST_CASE("node catalog: typed specs — literals, operators with an operand, atoms, params; main ports from the signature") {
  CHECK(nodeSpecValid("tanh", NodeCatalog::Scope::Project));
  CHECK(nodeSpecValid("0.5", NodeCatalog::Scope::Circuit));
  CHECK(nodeSpecValid("440Hz", NodeCatalog::Scope::Circuit));
  CHECK(nodeSpecValid("* 2", NodeCatalog::Scope::Circuit));
  CHECK(nodeSpecValid("param cutoff = 1kHz", NodeCatalog::Scope::Circuit));
  CHECK_FALSE(nodeSpecValid("* 2", NodeCatalog::Scope::Project));  // GU.11: an operand has no block of its own
  CHECK(nodeSpecValid("meta_wavetable", NodeCatalog::Scope::Project));  // (GU.4: was meta_vca, a module now)
  CHECK_FALSE(nodeSpecValid("in x : audio", NodeCatalog::Scope::Project));  // a port, not a node
  CHECK(nodeSpecValid("in x : audio", NodeCatalog::Scope::Circuit));
  CHECK_FALSE(nodeSpecValid("no_such_thing", NodeCatalog::Scope::Project));
  CHECK_FALSE(nodeSpecValid("* x", NodeCatalog::Scope::Circuit));  // an operand is a literal
  CHECK(splitSpec("* 2").node == "*");
  CHECK(splitSpec("* 2").operand == "2");
  CHECK(splitSpec("-3dB").operand.empty());  // a negative literal, not an operator
  // GU.11: the project places a typed spec only as one atom block; a literal or an operand has no block of its own.
  CHECK_FALSE(specBlockPatch("0.5").ok());
  CHECK_FALSE(specBlockPatch("* 2").ok());
  CHECK_FALSE(nodeSpecValid("0.5", NodeCatalog::Scope::Project));
  REQUIRE(specBlockPatch("tanh").ok());
  CHECK(*specBlockPatch("tanh") == "atom:tanh");
  // The value block (what a literal is now): an output-only block, no main input.
  MainPorts mp = mainPortsOf(graph::subcircuitSignature(doc::valuePatch()), &doc::valuePatch(), false);
  CHECK(mp.in.empty());
  CHECK_FALSE(mp.out.empty());
  // An atom block (`mul`): in and out.
  const graph::Patch* mul = doc::atomBlockPatch("mul");
  REQUIRE(mul != nullptr);
  mp = mainPortsOf(graph::subcircuitSignature(*mul), mul, false);
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
  CHECK(first("wvs") == "Wavetable Synth");  // a subsequence, word starts
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
  noteRecentNode("module:vca");
  noteRecentNode("module:utility");
  noteRecentNode("module:vca");
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
  // Its top-left just right of and below the pointer, or (no room on the right) its top-right just left of it
  // (TX.node-menu-fix1: kPointerGap, so the point where the node goes stays in sight): inside the window.
  const float g = NodeMenu::kPointerGap;
  const bool atPointer = std::abs(static_cast<float>(r.x) - (u.dev(at).x + g)) <= 1.0f || std::abs(static_cast<float>(r.right()) - (u.dev(at).x - g)) <= 1.0f;
  CHECK_MESSAGE(atPointer, "menu " << r.x << "," << r.y << " " << r.w << "x" << r.h << " pointer " << u.dev(at).x << "," << u.dev(at).y);
  CHECK((std::abs(static_cast<float>(r.y) - (u.dev(at).y + g)) <= 1.0f || r.bottom() == 900));
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
  // GU.10: "Saturator" is the stock preset (a module: its group) — the tests' stand-in patch of that name comes after it.
  CHECK(doc::isGroupDevice(*u.device(made)));
  CHECK(u.device(made)->name == "Saturator");
  CHECK(all_free::offPath(*u.ctx.doc(), made));
  const Node& n = u.pp().nodes[u.node(made)];
  CHECK(std::abs(n.x + n.w * 0.5f - at.x) <= 2.0f);
  CHECK(n.y <= at.y);  // (a module closes to its panel once placed: centred across, its panel hangs from the spot)
  CHECK(at.y <= n.y + n.h);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  CHECK(recentNodes().front() == "module:saturator");
  REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
}

// TX.node-menu-fix1 (the user's crash, dump pqdaw.exe.15024 at 65b90690): a CLICK on a row places the node and closes
// the menu inside its own onPointer(Down); the tree then captured the press for the menu — a widget already removed
// from the tree (in the overlay's graveyard). The next frame destroyed it and the app's cursor query
// (UiTree::cursor → captured_->cursorAt) called through a freed vtable. Freed memory is poisoned here, and every step
// is followed by the app's own sequence (update, then cursor()), through real event dispatch.
TEST_CASE("node menu: a click on a row places the node — the closed menu is never captured (poisoned frees)") {
  Ui u;
  const graph::Point2 at = u.emptyWorld();
  auto frame = [&] {
    u.d.step();
    (void)u.d.tree().cursor();  // the app's loop: ui->update(now), then applyCursor(ui->cursor())
  };
  auto clickRow = [&](std::string_view query, std::string_view name) {
    NodeMenu* m = u.menu();
    REQUIRE(m != nullptr);
    u.d.type(query);
    frame();
    // GU.10: the tests' stand-in patches share their names with the stock presets (modules): the patch is preferred.
    int32_t row = -1;
    for (const bool device : {true, false}) {
      for (int32_t i = 0; row < 0 && i < static_cast<int32_t>(m->rows().size()); ++i) {
        const NodeMenu::Row& r = m->rows()[static_cast<size_t>(i)];
        if (r.kind == NodeMenu::Row::Kind::Entry && m->catalog().entries[r.entry].name == name &&
            (!device || m->catalog().entries[r.entry].key.starts_with("device:")))
          row = i;
      }
      if (row >= 0) break;
    }
    REQUIRE(row >= 0);
    const ui::IRect rr = m->rowRect(row);
    REQUIRE_FALSE(rr.empty());
    const Vec2 p{static_cast<float>(rr.x + rr.w / 2), static_cast<float>(rr.y + rr.h / 2)};
    u.d.move(p);
    frame();
    const size_t h0 = u.history();
    {
      alloc_trap::PoisonFrees poison;
      u.d.down(p);
      CHECK(u.d.tree().captured() != static_cast<Widget*>(m));
      frame();  // the graveyard empties: the menu is freed (poisoned)
      frame();
      u.d.move({p.x + 3.0f, p.y + 2.0f});
      frame();
      u.d.up({p.x + 3.0f, p.y + 2.0f});
      frame();
      u.d.advance(0.5);  // push-apart / cable frames
      (void)u.d.tree().cursor();
    }
    u.view().sync();
    CHECK(u.history() == h0 + 1);
    CHECK(u.menu() == nullptr);
    CHECK(u.d.tree().focus() == &u.view().canvas());  // the keys go back to the canvas
  };
  // Tab on empty canvas, a click on "Saturator".
  u.d.move(u.dev(at));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  frame();
  clickRow("saturator", "Saturator");
  // Animated push-apart: Tab over the block just placed, a click (the spawn pushes it; frames run while it glides).
  u.view().setPushAnimated(true);
  u.view().canvas().setCablesAnimated(true);
  u.d.move(u.centre(u.newest()));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  frame();
  clickRow("vca", "VCA");
  // Tab over a cable (insert), a click.
  u.d.move(u.onCable(u.wireEdge(u.lfoWire)));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  frame();
  clickRow("saturator", "Saturator");
  // A cable dropped on empty space, a click. TX.block-drag: the spot is found (the blocks placed above were pushed apart
  // where the overlap rule put them; below them, or right of them, is empty).
  Vec2 empty{-1.0f, -1.0f};
  for (int k = 0; k < 24 && empty.x < 0.0f; ++k) {
    const Vec2 c = u.dev({at.x + 40.0f + 160.0f * static_cast<float>(k % 3), at.y + 170.0f + 60.0f * static_cast<float>(k / 3)});
    if (u.view().canvas().rect().contains(c) && u.view().canvas().nodeAt(c) == kNone && !u.view().canvas().pinAt(c).valid() &&
        u.view().canvas().edgeAt(c) == kNone)
      empty = c;
  }
  REQUIRE(empty.x >= 0.0f);
  u.d.drag(u.outPin(u.sat, "out"), empty, 6);
  frame();
  REQUIRE(u.menu() != nullptr);
  clickRow("utility", "Utility");
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
}

// TX.node-menu-fix1: every gesture of the node menu and the cables through real event dispatch (pointer and key events
// into the tree, as the window delivers them), animations on (push-apart glides, swinging cables), freed memory
// poisoned throughout, and the app's frame sequence (update, then the cursor query) after every event: Enter and a click
// in the menu, Z, Tab on a cable, Shift-splice, dissolve, Ctrl-release, W backwards cable → menu, a cable dropped →
// menu → Esc, while playing, at the far zoom, into an opened group.
TEST_CASE("node menu: every gesture through real dispatch, animated, poisoned frees — no stale widget, index or node") {
  Ui u;
  u.view().setPushAnimated(true);
  u.view().canvas().setCablesAnimated(true);
  alloc_trap::PoisonFrees poison;
  auto frame = [&] {
    u.d.step();
    (void)u.d.tree().cursor();
  };
  auto settle = [&] {
    for (int i = 0; i < 90; ++i) frame();
    u.view().sync();
  };
  const graph::Point2 at = u.emptyWorld();
  size_t h = u.history();
  // Tab + Enter on empty canvas.
  u.d.move(u.dev(at));
  frame();
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  frame();
  u.d.type("tanh");
  frame();
  REQUIRE(u.d.keyDown(key::kReturn) == KeyResult::Widget);
  frame();
  u.d.keyUp(key::kReturn);
  frame();
  CHECK(u.menu() == nullptr);
  CHECK(u.d.tree().focus() == &u.view().canvas());
  settle();
  CHECK(u.history() == ++h);
  // Tab again right over that block (push-apart while the next events arrive).
  u.d.move(u.centre(u.newest()));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  u.d.type("abs");
  frame();
  REQUIRE(u.d.press("Return") == KeyResult::Widget);
  for (int i = 0; i < 4; ++i) {
    u.d.move({u.dev(at).x + static_cast<float>(i) * 7.0f, u.dev(at).y});
    frame();
  }
  settle();
  CHECK(u.history() == ++h);
  // Z over the chain cable Utility → Saturator.
  u.d.move(u.onCable(u.chainEdge(u.util, u.sat)));
  frame();
  u.d.keyDown('z');
  frame();
  u.d.keyUp('z');
  frame();
  settle();
  CHECK(u.history() == ++h);
  // Tab on the LFO's wire, Enter.
  u.d.move(u.onCable(u.wireEdge(u.lfoWire)));
  frame();
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  u.d.type("saturator");
  frame();
  REQUIRE(u.d.press("Return") == KeyResult::Widget);
  frame();
  settle();
  CHECK(u.history() == ++h);
  const doc::DeviceId inserted = u.newest();
  // Dissolve it (Ctrl+Delete), a frame between press and release.
  {
    const uint32_t sn = u.view().scene().sceneNode[u.node(inserted)];
    const uint32_t one[1] = {sn};
    u.view().canvas().select(one, {});
    u.d.tree().setFocus(&u.view().canvas());
    REQUIRE(u.d.keyDown(key::kDelete, mod::kCtrl) == KeyResult::Widget);
    frame();
    u.d.keyUp(key::kDelete, mod::kCtrl);
    frame();
    settle();
  }
  CHECK(u.history() == ++h);
  CHECK(u.device(inserted) == nullptr);
  // Shift-splice a free block into the LFO's wire, frames during the drag.
  REQUIRE(u.view().addFreeDevice(u.keys, "module:saturator"));
  settle();
  h = u.history();
  {
    const doc::DeviceId s2 = u.newest();
    doc::WireId lw;
    for (const doc::Wire& w : u.wires())
      if (w.from == u.lfo && w.port == "y") lw = w.id;
    REQUIRE(lw.valid());
    const Vec2 on = u.onCable(u.wireEdge(lw));
    const Vec2 from = u.centre(s2);
    u.d.down(from, PointerButton::Left, mod::kShift);
    frame();
    for (int k = 1; k <= 6; ++k) {
      u.d.move({from.x + (on.x - from.x) * static_cast<float>(k) / 6.0f, from.y + (on.y - from.y) * static_cast<float>(k) / 6.0f}, mod::kShift);
      frame();
    }
    u.d.up(on, PointerButton::Left, mod::kShift);
    frame();
    settle();
    CHECK(u.history() == ++h);
  }
  // Ctrl-release: the LFO's jack onto the Saturator's input with Ctrl held; frames while the cable is held; a click
  // on the Utility's input; then another Ctrl-release and Esc.
  {
    const Vec2 jack = u.outPin(u.lfo, "y");
    const Vec2 satIn = u.inPin(u.sat, "x");
    u.d.down(jack);
    frame();
    u.d.move({(jack.x + satIn.x) * 0.5f, (jack.y + satIn.y) * 0.5f});
    frame();
    u.d.move(satIn, mod::kCtrl);
    frame();
    u.d.up(satIn, PointerButton::Left, mod::kCtrl);
    frame();
    settle();
    CHECK(u.history() == ++h);
    CHECK(u.view().canvas().stickyWire());
    const Vec2 utilIn = u.inPin(u.util, "x");
    u.d.move(utilIn);
    frame();
    u.d.down(utilIn, PointerButton::Left, mod::kCtrl);
    frame();
    u.d.up(utilIn, PointerButton::Left, mod::kCtrl);
    frame();
    settle();
    CHECK(u.history() == ++h);
    CHECK(u.view().canvas().stickyWire());
    u.d.press("Escape");
    frame();
    CHECK_FALSE(u.view().canvas().stickyWire());
  }
  // W + drag out of the Utility's gain onto empty space: the menu (sources first); Enter places one wired.
  {
    const graph::Point2 below{at.x + 60.0f, at.y + 190.0f};
    u.d.keyDown('w');
    const Vec2 g = u.inPin(u.util, "g");
    u.d.down(g);
    frame();
    for (int k = 1; k <= 5; ++k) {
      const Vec2 b = u.dev(below);
      u.d.move({g.x + (b.x - g.x) * static_cast<float>(k) / 5.0f, g.y + (b.y - g.y) * static_cast<float>(k) / 5.0f});
      frame();
    }
    u.d.up(u.dev(below));
    u.d.keyUp('w');
    frame();
    REQUIRE(u.menu() != nullptr);
    u.d.type("lfo");
    frame();
    REQUIRE(u.d.press("Return") == KeyResult::Widget);
    frame();
    settle();
    CHECK(u.history() == ++h);
  }
  // A cable dropped on empty space: the menu; Esc closes it, the canvas has the keys again, nothing changed.
  {
    // (GU.11: the spawned LFO is a module — its panel is large: the drop point is searched clear of every block)
    graph::Point2 drop{at.x + 260.0f, at.y + 40.0f};
    {
      const ui::IRect cr = u.view().canvas().rect();
      auto clear = [&](graph::Point2 w) {
        const Vec2 p = u.dev(w);
        return p.x > cr.x + 20 && p.y > cr.y + 20 && p.x < cr.right() - 20 && p.y < cr.bottom() - 20 &&
               u.view().canvas().nodeAt(p) == kNone && u.view().canvas().edgeAt(p) == kNone;
      };
      bool found = clear(drop);
      for (float dy = 0.0f; !found && dy <= 300.0f; dy += 20.0f)
        for (float dx = 260.0f; !found && dx >= -200.0f; dx -= 20.0f)
          if (clear({at.x + dx, at.y + 40.0f + dy})) drop = {at.x + dx, at.y + 40.0f + dy}, found = true;
      REQUIRE(found);
    }
    u.d.drag(u.outPin(u.sat, "out"), u.dev(drop), 6);
    frame();
    REQUIRE(u.menu() != nullptr);
    u.d.press("Escape");
    frame();
    CHECK(u.menu() == nullptr);
    CHECK(u.d.tree().focus() == &u.view().canvas());
    CHECK(u.history() == h);
  }
  // While playing: Tab + Enter, cables glowing.
  u.port.play(0);
  u.view().canvas().wakeCables();
  u.d.move(u.dev({at.x + 40.0f, at.y - 10.0f}));
  frame();
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  REQUIRE_MESSAGE(u.menu() != nullptr, u.ctx.status());
  u.d.type("vca");
  frame();
  REQUIRE_MESSAGE(u.menu() != nullptr, u.ctx.status());
  CHECK(u.d.tree().focus() == u.menu());
  REQUIRE(u.d.press("Return") == KeyResult::Widget);
  settle();
  CHECK(u.history() == ++h);
  u.port.stop();
  // Far zoom: Tab + Enter.
  {
    CircuitCanvas::View v = u.view().canvas().view();
    v.zoom = 0.12;
    u.view().canvas().setView(v);
    frame();
    const ui::IRect r = u.view().canvas().rect();
    u.d.move({static_cast<float>(r.x + r.w - 60), static_cast<float>(r.y + r.h - 60)});
    frame();
    REQUIRE(u.d.press("Tab") == KeyResult::Widget);
    u.d.type("tanh");
    frame();
    REQUIRE(u.d.press("Return") == KeyResult::Widget);
    settle();
    CHECK(u.history() == ++h);
  }
  // Into an opened group: the series after the instrument grouped, its capsule open; Tab inside it + Enter.
  {
    u.view().focusBand(u.keys, false);
    frame();
    std::vector<doc::DeviceId> sel;  // the series after the instrument (the Utility, Z's VCA, the Saturator)
    for (const doc::DevicePtr& dv : u.track(u.keys).devices)
      if (!all_free::offPath(*u.ctx.doc(), dv->id) && dv->id != u.synth && sel.size() < 3) sel.push_back(dv->id);
    REQUIRE_MESSAGE(u.view().groupDevices(sel), u.ctx.status());
    settle();
    h = u.history();
    uint32_t cap = kNone;
    for (uint32_t i = 0; i < u.pp().nodes.size(); ++i)
      if (u.pp().nodes[i].group) cap = i;
    REQUIRE(cap != kNone);
    u.view().setGroupOpen(u.pp().nodes[cap].device, true);
    settle();
    for (uint32_t i = 0; i < u.pp().nodes.size(); ++i)
      if (u.pp().nodes[i].group) cap = i;
    const Node& c = u.pp().nodes[cap];
    u.d.move(u.dev({c.x + c.w - 20.0f, c.y + c.h - 14.0f}));
    frame();
    REQUIRE(u.d.press("Tab") == KeyResult::Widget);
    u.d.type("tanh");
    frame();
    REQUIRE(u.d.press("Return") == KeyResult::Widget);
    settle();
    CHECK(u.history() == ++h);
  }
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
}

// TX.node-menu-fix1: a document change during a gesture (an undo while a block or a cable from it is dragged) rebuilds
// the model under the gesture's indices. They are found again by identity; a gesture whose block went away ends without
// an edit, and nothing reads a stale index (poisoned frees, app frame sequence).
TEST_CASE("project patch: an undo during a block drag or a cable drag ends the gesture — no stale index") {
  Ui u;
  u.view().canvas().setCablesAnimated(true);
  alloc_trap::PoisonFrees poison;
  auto frame = [&] {
    u.d.step();
    (void)u.d.tree().cursor();
  };
  const graph::Point2 at = u.emptyWorld();
  for (int pass = 0; pass < 2; ++pass) {
    u.d.move(u.dev(at));
    REQUIRE(u.d.press("Tab") == KeyResult::Widget);
    u.d.type("tanh");  // (GU.10: an atom block — its output is `out`; "saturator" is a module now)
    REQUIRE(u.d.press("Return") == KeyResult::Widget);
    frame();
    u.view().sync();
    const size_t h0 = u.history();
    const doc::DeviceId made = u.newest();
    REQUIRE(u.device(made) != nullptr);
    // pass 0: the block itself; pass 1: a cable out of its output.
    const Vec2 from = pass == 0 ? u.centre(made) : u.outPin(made, "out");
    const Vec2 to = u.inPin(u.util, "g");
    u.d.down(from);
    frame();
    for (int k = 1; k <= 3; ++k) {
      u.d.move({from.x + (to.x - from.x) * static_cast<float>(k) / 6.0f, from.y + (to.y - from.y) * static_cast<float>(k) / 6.0f});
      frame();
    }
    CHECK(u.view().canvas().drag() == (pass == 0 ? ProjectCanvas::Drag::MoveDevice : ProjectCanvas::Drag::Wire));
    REQUIRE(u.ctx.undo());  // the block goes (the spawn undone) while it is dragged
    frame();
    CHECK(u.view().canvas().drag() == ProjectCanvas::Drag::None);
    for (int k = 4; k <= 6; ++k) {
      u.d.move({from.x + (to.x - from.x) * static_cast<float>(k) / 6.0f, from.y + (to.y - from.y) * static_cast<float>(k) / 6.0f});
      frame();
    }
    u.d.up(to);
    frame();
    u.d.advance(0.5);
    CHECK(u.history() == h0 - 1);  // nothing made by the release
    CHECK(u.device(made) == nullptr);
    CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  }
  // A block dragged while ANOTHER block goes: the drag goes on with the block found again.
  {
    u.d.move(u.dev(at));
    REQUIRE(u.d.press("Tab") == KeyResult::Widget);
    u.d.type("tanh");
    REQUIRE(u.d.press("Return") == KeyResult::Widget);
    frame();
    u.view().sync();
    const doc::DeviceId other = u.newest();
    const Vec2 from = u.centre(u.lfo);
    u.d.down(from);
    for (int k = 1; k <= 3; ++k) {
      u.d.move({from.x + 10.0f * static_cast<float>(k), from.y + 4.0f * static_cast<float>(k)});
      frame();
    }
    const doc::DeviceId gone[1] = {other};
    auto e = doc::ops::deleteDevices(*u.ctx.doc(), gone);
    REQUIRE(e.ok());
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
    frame();
    CHECK(u.view().canvas().drag() == ProjectCanvas::Drag::MoveDevice);
    u.d.move({from.x + 50.0f, from.y + 20.0f});
    frame();
    u.d.up({from.x + 50.0f, from.y + 20.0f});
    frame();
    u.view().sync();
    const Node& n = u.pp().nodes[u.node(u.lfo)];
    const Vec2 c = u.dev({n.x + n.w * 0.5f, n.y + n.h * 0.5f});
    CHECK(std::abs(c.x - (from.x + 50.0f)) <= 2.0f);  // it moved where it was dropped
  }
}

// TX.node-menu-fix1: a new connection is drawn as the real cable (the canvas's loose cable): out of the port, trailing
// the pointer and swinging, snapped (highlighted) onto the port or control a release connects; released, the new cable
// keeps its motion (it is awake, not popped to its rest shape) and frames stop after; dropped on empty space it hangs
// to the node menu until the spawned node's cable takes it over; Esc / a cancel removes it; W and Ctrl-release too.
TEST_CASE("node menu: a new connection is the real cable on the pointer — snaps, trails, keeps its motion when dropped") {
  Ui u;
  CircuitCanvas& cv = u.view().canvas();
  cv.setCablesAnimated(true);
  u.view().setPushAnimated(true);
  auto frames = [&](int n) {
    for (int i = 0; i < n; ++i) u.d.step();
  };
  const Vec2 jack = u.outPin(u.lfo, "y");
  const Vec2 satIn = u.inPin(u.sat, "x");
  // Out of the LFO's jack: a cable from the port, its free end at the pointer.
  u.d.down(jack);
  CHECK(cv.looseCable());
  const Vec2 mid{(jack.x + satIn.x) * 0.5f, (jack.y + satIn.y) * 0.5f + 60.0f};
  for (int k = 1; k <= 8; ++k) {
    u.d.move({jack.x + (mid.x - jack.x) * static_cast<float>(k) / 8.0f, jack.y + (mid.y - jack.y) * static_cast<float>(k) / 8.0f});
    u.d.step();
  }
  {
    const auto p = cv.looseCablePoints();
    REQUIRE(p.size() >= 2);
    const graph::Point2 port = p.front(), end = p.back();
    const Node& ln = u.pp().nodes[u.node(u.lfo)];
    // (TX.orbit-ports: the LFO module's main output orbits its outline: the cable starts where the port is drawn)
    const graph::Point2 pw = u.view().canvas().portWorld(u.node(u.lfo), static_cast<uint32_t>(ln.output("y")), false);
    CHECK(std::abs(port.x - pw.x) <= 0.01f);
    CHECK(std::abs(port.y - pw.y) <= 0.01f);
    CHECK(std::abs(u.dev(end).x - mid.x) <= 1.0f);
    CHECK(std::abs(u.dev(end).y - mid.y) <= 1.0f);
    CHECK(cv.looseCableMoving());  // it swings after the pointer
    CHECK_FALSE(cv.looseCableSnapped());
    CHECK(u.view().canvas().drag() == ProjectCanvas::Drag::Wire);
  }
  // Over the Saturator's input: snapped, its end on that pin.
  u.d.move(satIn);
  u.d.step();
  CHECK(cv.looseCableSnapped());
  {
    const Node& sn = u.pp().nodes[u.node(u.sat)];
    // TX.orbit-ports: on the port where it is drawn (the Saturator's main input orbits its panel).
    const graph::Point2 pin = u.view().canvas().portWorld(u.node(u.sat), static_cast<uint32_t>(sn.input("x")), true);
    const graph::Point2 end = cv.looseCablePoints().back();
    CHECK(std::abs(end.x - pin.x) <= 0.01f);
    CHECK(std::abs(end.y - pin.y) <= 0.01f);
  }
  // Released: connected; the new cable has the loose one's motion (awake now, not on its rest shape).
  const size_t h0 = u.history();
  u.d.up(satIn);
  CHECK(u.history() == h0 + 1);
  CHECK_FALSE(cv.looseCable());
  CHECK(cv.cablesMoving());
  // … and frames stop within 2 s.
  frames(1);
  u.d.advance(2.0);
  CHECK_FALSE(cv.cablesMoving());
  const uint64_t idle = u.d.framesDrawn();
  u.d.advance(1.0);
  CHECK(u.d.framesDrawn() == idle);
  // Esc during a drag: the cable goes, nothing is made.
  u.d.down(jack);
  u.d.move({jack.x + 40.0f, jack.y + 30.0f});
  CHECK(cv.looseCable());
  u.d.press("Escape");
  CHECK_FALSE(cv.looseCable());
  u.d.up({jack.x + 40.0f, jack.y + 30.0f});
  CHECK(u.history() == h0 + 1);
  // Dropped on empty space: it hangs to the menu (asleep: no frames while the menu is idle), the spawned node's cable
  // takes it over.
  const Vec2 empty = u.dev({u.emptyWorld().x + 40.0f, u.emptyWorld().y + 150.0f});
  u.d.drag(u.outPin(u.sat, "out"), empty, 6);
  REQUIRE(u.menu() != nullptr);
  CHECK(cv.looseCable());
  u.d.advance(2.0);
  const uint64_t menuIdle = u.d.framesDrawn();
  u.d.advance(0.5);
  CHECK(u.d.framesDrawn() == menuIdle);
  CHECK(cv.looseCable());
  u.d.type("utility");
  REQUIRE(u.d.press("Return") == KeyResult::Widget);
  CHECK(u.history() == h0 + 2);
  CHECK(cv.cablesMoving());  // the spawned node's cable swings on from the loose one
  u.d.step();
  CHECK_FALSE(cv.looseCable());
  // W + drag out of the Utility's gain: a cable out of the param, snapped onto a block's main output.
  u.d.advance(2.0);
  u.d.keyDown('w');
  u.d.down(u.inPin(u.util, "g"));
  CHECK(cv.looseCable());
  u.d.move(u.centre(u.synth));
  u.d.step();
  CHECK(cv.looseCableSnapped());
  u.d.up(u.centre(u.synth));
  u.d.keyUp('w');
  CHECK(u.history() == h0 + 3);
  CHECK_FALSE(cv.looseCable());
  CHECK(cv.cablesMoving());
  // Ctrl-release: the made connection's cable takes the motion, a new loose cable stays on the pointer; Esc drops it.
  u.d.advance(2.0);
  u.d.down(jack);
  u.d.move({jack.x + 20.0f, jack.y + 40.0f});
  u.d.move(u.inPin(u.util, "x"), mod::kCtrl);
  u.d.up(u.inPin(u.util, "x"), PointerButton::Left, mod::kCtrl);
  CHECK(u.history() == h0 + 4);
  CHECK(u.view().canvas().stickyWire());
  CHECK(cv.looseCable());
  CHECK(cv.cablesMoving());
  u.d.move({jack.x + 60.0f, jack.y + 80.0f});
  u.d.step();
  CHECK(cv.looseCable());
  u.d.press("Escape");
  CHECK_FALSE(cv.looseCable());
  CHECK_FALSE(u.view().canvas().stickyWire());
  u.d.advance(2.5);
  const uint64_t f = u.d.framesDrawn();
  u.d.advance(1.0);
  CHECK(u.d.framesDrawn() == f);  // idle again: no frames
}

// TX.node-menu-fix1: the menu opens with a short fade (frames for kOpenSeconds, then none while it is idle), beside the
// pointer with the spawn point marked (gone when it closes); Tab over a cable shows that cable selected while the menu
// is open (the selection comes back on Esc); a node spawned from a dropped cable meets it with its input pin; the menu
// fits a small window.
TEST_CASE("node menu: open animation then idle; spawn mark; the insert cable shown; a dropped cable meets the input; fits") {
  Ui u;
  const graph::Point2 at = u.emptyWorld();
  u.d.move(u.dev(at));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  NodeMenu* m = u.menu();
  REQUIRE(m != nullptr);
  CHECK(m->openProgress() < 1.0f);
  REQUIRE(u.view().spawnMark().has_value());
  CHECK(std::abs(u.view().spawnMark()->x - at.x) <= 0.5f);
  const uint64_t f0 = u.d.framesDrawn();
  u.d.advance(0.25);
  CHECK(m->openProgress() == 1.0f);
  CHECK(u.d.framesDrawn() > f0 + 2);  // it animated
  const uint64_t f1 = u.d.framesDrawn();
  u.d.advance(0.5);
  CHECK(u.d.framesDrawn() == f1);  // then idle: no frames
  u.d.press("Escape");
  u.d.step();
  CHECK(u.menu() == nullptr);
  CHECK_FALSE(u.view().spawnMark().has_value());
  CHECK(u.d.tree().focus() == &u.view().canvas());
  // Tab over the LFO's wire: that cable selected while the menu is open; Esc gives the selection back.
  {
    const uint32_t sn = u.view().scene().sceneNode[u.node(u.sat)];
    const uint32_t one[1] = {sn};
    u.view().canvas().select(one, {});
    const uint32_t edge = u.wireEdge(u.lfoWire);
    u.d.move(u.onCable(edge));
    REQUIRE(u.d.press("Tab") == KeyResult::Widget);
    REQUIRE(u.menu() != nullptr);
    REQUIRE(u.view().canvas().selection().wires.size() == 1);
    CHECK(u.view().scene().modelEdge[u.view().canvas().selection().wires[0]] == edge);
    CHECK(u.view().canvas().selection().nodes.empty());
    u.d.press("Escape");
    CHECK(u.view().canvas().selection().nodes.size() == 1);
    CHECK(u.view().canvas().selection().wires.empty());
  }
  // A cable from the Saturator's output dropped on empty space: the Utility it spawns has its input pin there.
  {
    // (GU.11: the Utility module is taller than the stock panel was: dropped lower, it fits below the band's title strip
    // — placeAt never puts a block above it)
    const graph::Point2 drop{at.x + 40.0f, at.y + 140.0f};
    u.d.drag(u.outPin(u.sat, "out"), u.dev(drop), 6);
    REQUIRE(u.menu() != nullptr);
    u.pick("utility", "Utility");
    const doc::DeviceId made = u.newest();
    const Node& n = u.pp().nodes[u.node(made)];
    const int32_t k = n.input(u.view().mainPorts(made).in);
    REQUIRE(k >= 0);
    const graph::Point2 pin = inPortPoint(n, static_cast<size_t>(k));  // TX.orbit-ports: its orbiting input, settled
    CAPTURE(pin.x);
    CAPTURE(pin.y);
    CAPTURE(drop.x);
    CAPTURE(drop.y);
    // (an orbiting port settles in whole-unit nudges: within a unit and a half)
    CHECK(std::abs(pin.x - drop.x) <= 1.5f);
    CHECK(std::abs(pin.y - drop.y) <= 1.5f);
  }
  // A small window: the menu fits inside it.
  u.d.resize(640, 420);
  u.d.tree().layoutNow();
  u.d.move({300.0f, 200.0f});
  u.d.tree().setFocus(&u.view().canvas());
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  REQUIRE(u.menu() != nullptr);
  u.d.tree().layoutNow();
  const ui::IRect r = u.menu()->rect();
  CHECK(r.x >= 0);
  CHECK(r.y >= 0);
  CHECK(r.right() <= 640);
  CHECK(r.bottom() <= 420);
  u.d.press("Escape");
}
TEST_CASE("node menu: a typed spec places that one node (an atom; `* 2` has none); an instrument is a block like any (TX.all-free)") {
  Ui u;
  const graph::Point2 at = u.emptyWorld();
  const size_t h0 = u.history();
  u.d.move(u.dev(at));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  u.d.type("* 2");
  REQUIRE(u.menu() != nullptr);
  CHECK_FALSE(u.menu()->specOffered());  // GU.11: an operand has no block of its own (wire a Value block)
  // A typed atom (an operator's symbol: no entry is named so): its atom block.
  for (int i = 0; i < 2; ++i) u.d.press("Backspace");  // "*"
  u.d.step();
  REQUIRE(u.menu()->specOffered());
  CHECK(u.menu()->rows().front().kind == NodeMenu::Row::Kind::Spec);
  u.menu()->setHighlighted(0);
  u.d.press("Return");
  u.d.step();
  CHECK(u.history() == h0 + 1);
  const doc::DeviceId mul = u.newest();
  REQUIRE(u.device(mul) != nullptr);
  CHECK(u.device(mul)->patch == "atom:mul");
  CHECK(all_free::offPath(*u.ctx.doc(), mul));
  const MainPorts mp = u.view().mainPorts(mul);
  CHECK_FALSE(mp.in.empty());
  CHECK_FALSE(mp.out.empty());
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // An instrument module: TX.all-free — a block like any other, placed where the menu opened, off the path until
  // cabled (the path keeps its instrument). One step.
  u.d.move(u.dev(at));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  u.pick("operator", "Operator");
  CHECK(u.history() == h0 + 2);
  const doc::DeviceId op = u.newest();
  REQUIRE(u.device(op) != nullptr);
  CHECK(u.device(op)->name == "Operator");
  CHECK(doc::isGroupDevice(*u.device(op)));
  CHECK(all_free::offPath(*u.ctx.doc(), op));
  CHECK(u.path(u.keys).front() == u.synth);
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
  CHECK(doc::isGroupDevice(*u.device(vca)));  // GU.4: the VCA module's group
  CHECK(u.device(vca)->name == "VCA");
  CHECK(all_free::offPath(*u.ctx.doc(), vca));
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
  const Vec2 gain = u.inPin(u.util, "g");
  u.d.keyDown('w');
  u.d.drag(gain, u.outPin(u.sat, "out"), 6);
  u.d.keyUp('w');
  CHECK(u.history() == h0 + 1);
  CHECK(u.wired(u.sat, "out", u.util, "g"));
  // Onto empty space: the menu offers sources first; the spawned node's main output drives the param.
  u.d.keyDown('w');
  u.d.drag(u.inPin(u.util, "g"), u.dev(u.emptyWorld()), 6);
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
  CHECK(doc::isGroupDevice(*u.device(acc)));  // GU.4: the Phase Accumulator module's group
  CHECK(u.device(acc)->name == "Phase Accumulator");
  CHECK(u.wired(acc, u.view().mainPorts(acc).out, u.util, "g"));
  for (int i = 0; i < 2; ++i) REQUIRE(u.ctx.undo());
  CHECK(u.history() == h0);
}

TEST_CASE("node menu: Z over a chain cable inserts a VCA at 1 in the chain; over a wire between its ends; one step each") {
  Ui u;
  const size_t h0 = u.history();
  const doc::Snapshot start = u.ctx.doc();
  // A VCA at its default is x1 (the sound is unchanged): GU.4, its module's knob at 0.7 on the VCA's law.
  CHECK(dsp::evalCurve(doc::vcaCurve(), 0.7) == doctest::Approx(1.0).epsilon(1e-9));
  // The chain cable Utility → Saturator.
  u.d.move(u.onCable(u.chainEdge(u.util, u.sat)));
  u.d.keyDown('z');
  u.d.keyUp('z');
  u.view().sync();
  CHECK(u.history() == h0 + 1);
  const std::vector<doc::DeviceId> list = u.path(u.keys);  // TX.all-free: spliced into the main path
  REQUIRE(list.size() == 4);
  CHECK(list[1] == u.util);
  REQUIRE(u.device(list[2]) != nullptr);
  CHECK(doc::isGroupDevice(*u.device(list[2])));  // the VCA module (its knob at 0.7: x1)
  CHECK(u.device(list[2])->name == "VCA");
  CHECK(list[3] == u.sat);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // The LFO's wire into the Utility's gain: LFO → VCA → gain, the VCA free.
  u.d.move(u.onCable(u.wireEdge(u.lfoWire)));
  u.d.keyDown('z');
  u.d.keyUp('z');
  u.view().sync();
  CHECK(u.history() == h0 + 2);
  const doc::DeviceId vca = u.newest();
  REQUIRE(u.device(vca) != nullptr);
  CHECK(all_free::offPath(*u.ctx.doc(), vca));
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) == nullptr);
  const MainPorts mp = u.view().mainPorts(vca);
  CHECK(u.wired(u.lfo, "y", vca, mp.in));
  CHECK(u.wired(vca, mp.out, u.util, "g"));
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // Z over nothing does nothing.
  u.d.move(u.dev(u.emptyWorld()));
  u.d.keyDown('z');
  u.d.keyUp('z');
  CHECK(u.history() == h0 + 2);
  for (int i = 0; i < 2; ++i) REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
}

TEST_CASE("node menu: Tab over a cable inserts the chosen node — in and out, output only; the note cable is a path link (TX.all-free)") {
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
  CHECK(u.wired(u.lfo, "y", s2, mp.in));
  CHECK(u.wired(s2, mp.out, u.util, "g"));
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) == nullptr);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // Output only: a Value block put into the cable Saturator → g: the value drives g, the Saturator's wire goes.
  doc::WireId into;
  for (const doc::Wire& w : u.wires())
    if (w.from == s2 && w.to == u.util) into = w.id;
  REQUIRE(into.valid());
  u.d.move(u.onCable(u.wireEdge(into)));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  REQUIRE(u.menu() != nullptr);
  u.pick("value", "Value");
  CHECK(u.history() == h0 + 2);
  const doc::DeviceId half = u.newest();
  REQUIRE(u.device(half) != nullptr);
  CHECK(u.view().mainPorts(half).in.empty());
  CHECK(u.wired(half, u.view().mainPorts(half).out, u.util, "g"));
  for (const doc::Wire& w : u.wires()) CHECK_FALSE((w.from == s2 && w.to == u.util));
  for (const doc::Wire& w : u.wires()) CHECK_FALSE(w.to == half);  // nothing into it
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // TX.all-free: the note cable (the source → the instrument) is a link of the main path like any: a VCA goes in at
  // the head of the path (what the notes reach first), one step.
  uint32_t notes = kNone;
  for (uint32_t i = 0; i < u.pp().edges.size(); ++i)
    if (u.pp().edges[i].kind == EdgeKind::Notes && u.pp().nodes[u.pp().edges[i].to].device == u.synth) notes = i;
  REQUIRE(notes != kNone);
  CHECK(u.view().insertVca(notes));
  u.view().sync();
  CHECK(u.history() == h0 + 3);
  REQUIRE(u.device(u.path(u.keys).front()) != nullptr);
  CHECK(u.device(u.path(u.keys).front())->name == "VCA");
  CHECK(u.path(u.keys).at(1) == u.synth);
  for (int i = 0; i < 3; ++i) REQUIRE(u.ctx.undo());
  CHECK(u.history() == h0);
}

// TX.insert-group (user bug: Z on a cable into a Phase Accumulator put the VCA inside it): a block inserted on a cable
// goes where the cable is drawn — the innermost group holding both its ends (doc::wireContainer). A cable from outside
// into a module's control or its main input, or from its output, stays outside; one between two of a group's blocks, or
// into its Out terminal, is inside it; one from a nested group's block into a module nested beside it is in their
// common group. Z and the Tab menu, through real key dispatch; each one undo step, the cable's two ends kept (the VCA
// at its default: × 1).
TEST_CASE("node menu: Z and Tab on a cable insert into the cable's group — not into the module the cable reaches") {
  Ui u;
  doc::DocumentStore& st = u.ctx.store();
  const doc::DeviceId pa = test_blocks::addModule(st, u.keys, "phase_acc", doc::ops::kOffPath);
  auto in = [&](std::string_view name) {
    const doc::DeviceId id = test_blocks::member(*u.ctx.doc(), pa, name);
    REQUIRE_MESSAGE(id.valid(), std::string(name));
    return id;
  };
  const doc::DeviceId freq = in(doc::kPaFrequency), phasor = in("phasor"), addPhase = in("+ phase"), amp = in("× amp");
  auto connect = [&](doc::DeviceId from, std::string_view port, doc::DeviceId to, std::string_view param) {
    auto e = doc::ops::connectWire(*st.current(), st.ids(), from, std::string(port), to, std::string(param));
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
    REQUIRE(st.dispatch(e->command).ok());
    return e->wires.at(0);
  };
  auto find = [&](doc::DeviceId from, std::string_view port, doc::DeviceId to, std::string_view param) {
    for (const doc::Wire& w : u.wires())
      if (w.from == from && w.port == port && w.to == to && w.param == param) return w.id;
    FAIL("no such wire");
    return doc::WireId{};
  };
  const doc::WireId intoFreq = connect(u.lfo, "y", freq, doc::kValueParam);
  const doc::WireId intoMain = connect(u.lfo, "y", pa, doc::kMainPort);
  const doc::WireId fromPa = connect(pa, doc::kMainPort, u.sat, "x");
  const doc::WireId inner = find(phasor, "out", addPhase, "a");
  const doc::WireId intoOut = find(amp, "out", pa, doc::kGroupOutPort);
  // The newest VCA module (Ui::newest gives the outermost group of the newest block: the PA when the VCA is inside it).
  auto newestVca = [&] {
    doc::DeviceId best;
    for (const doc::DevicePtr& d : u.track(u.keys).devices)
      if (doc::isGroupDevice(*d) && d->name == "VCA" && d->id.value > best.value) best = d->id;
    return best;
  };
  struct Case {
    const char* what;
    doc::WireId wire;
    doc::DeviceId group;  // where the inserted block belongs (invalid: the track's top level)
  };
  const Case cases[] = {
      {"LFO -> the PA's Frequency control", intoFreq, {}},
      {"LFO -> the PA's main input", intoMain, {}},
      {"the PA's output -> Saturator", fromPa, {}},
      {"inside the PA, between two of its blocks", inner, pa},
      {"inside the PA, a block -> its Out terminal", intoOut, pa},
  };
  u.view().setGroupOpen(pa, true);
  u.fit();
  for (const Case& c : cases) {
    // The helper first, then the gestures.
    const doc::Wire w = *doc::findWire(*u.ctx.doc(), c.wire);
    const doc::WireContainer at = doc::wireContainer(*u.ctx.doc(), w);
    CHECK_MESSAGE(at.track == u.keys, c.what);
    CHECK_MESSAGE(at.group == c.group, c.what);
    for (const bool tab : {false, true}) {
      const std::string label = std::string(c.what) + (tab ? " (Tab)" : " (Z)");
      INFO(label);
      u.view().sync();
      const doc::Snapshot start = u.ctx.doc();
      const size_t h0 = u.history();
      u.d.move(u.onCable(u.cableOf(c.wire)));
      if (tab) {
        REQUIRE(u.d.press("Tab") == KeyResult::Widget);
        u.pick("vca", "VCA");
      } else {
        u.d.keyDown('z');
        u.d.keyUp('z');
        u.view().sync();
      }
      REQUIRE(u.history() == h0 + 1);
      const doc::DeviceId vca = newestVca();
      REQUIRE(u.device(vca) != nullptr);
      CHECK(u.device(vca)->group == c.group);
      CHECK(doc::findDevice(*u.ctx.doc(), vca).track->id == u.keys);
      // The cable's two ends kept: its source -> the VCA -> its target (the VCA at its default: × 1).
      CHECK(doc::findWire(*u.ctx.doc(), c.wire) == nullptr);
      const MainPorts mp = u.view().mainPorts(vca);
      bool fromSource = false, intoTarget = false;
      for (const doc::Wire& x : u.wires()) {
        fromSource = fromSource || (x.from == w.from && x.port == w.port && x.to == vca && (x.param.empty() || x.param == mp.in));
        intoTarget = intoTarget || (x.from == vca && (x.port.empty() || x.port == mp.out) && x.to == w.to && x.param == w.param);
      }
      CHECK(fromSource);
      CHECK(intoTarget);
      CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
      REQUIRE(u.ctx.undo());
      CHECK(u.ctx.doc() == start);
    }
  }
  // Nested: the PA inside the LFO's group; a cable from an LFO block into the PA's Phase is drawn in the LFO's group
  // (their common one), not in the PA.
  REQUIRE(u.view().setDeviceGroup(pa, u.lfo));
  doc::DeviceId lfoOut;
  for (const doc::Wire& w : u.wires())
    if (w.to == u.lfo && w.param == doc::kGroupOutPort) lfoOut = w.from;
  REQUIRE(lfoOut.valid());
  const doc::WireId nested = connect(lfoOut, "out", in(doc::kPaPhase), doc::kValueParam);
  CHECK(doc::wireContainer(*u.ctx.doc(), *doc::findWire(*u.ctx.doc(), nested)).group == u.lfo);
  u.view().setGroupOpen(u.lfo, true);
  u.fit();
  const size_t h0 = u.history();
  u.d.move(u.onCable(u.cableOf(nested)));
  u.d.keyDown('z');
  u.d.keyUp('z');
  u.view().sync();
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.device(newestVca()) != nullptr);
  CHECK(u.device(newestVca())->group == u.lfo);
}

TEST_CASE("node menu: Shift-drag a block over a cable highlights it and splices the block in; Ctrl+Delete dissolves it") {
  Ui u;
  REQUIRE(u.view().addFreeDevice(u.keys, "module:saturator"));
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
  CHECK(u.wired(u.lfo, "y", s2, mp.in));
  CHECK(u.wired(s2, mp.out, u.util, "g"));
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
  CHECK(u.wired(u.lfo, "y", u.util, "g"));
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // A chain block: its output's wire is fed by the block before it in the chain (the chain closes up by itself).
  {
    auto e = doc::ops::connectWire(*u.ctx.doc(), u.ctx.store().ids(), u.util, "out", u.lfo, "x");
    REQUIRE(e.ok());
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
    REQUIRE(u.view().dissolve(u.util));
    CHECK(u.device(u.util) == nullptr);
    CHECK(u.wired(u.synth, "y", u.lfo, "x"));
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
  const Vec2 jack = u.outPin(u.lfo, "y");
  const Vec2 satIn = u.inPin(u.sat, "x");
  u.d.down(jack);
  u.d.move({(jack.x + satIn.x) * 0.5f, (jack.y + satIn.y) * 0.5f});
  u.d.move(satIn, mod::kCtrl);
  u.d.up(satIn, PointerButton::Left, mod::kCtrl);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  CHECK(u.wired(u.lfo, "y", u.sat, "x"));
  CHECK(u.view().canvas().stickyWire());
  // A plain click on the Utility's input wires it too and ends.
  const Vec2 utilIn = u.inPin(u.util, "x");
  u.d.move(utilIn);
  u.d.click(utilIn);
  u.d.step();
  CHECK(u.history() == h0 + 2);
  CHECK(u.wired(u.lfo, "y", u.util, "x"));
  CHECK_FALSE(u.view().canvas().stickyWire());
  // A plain release ends (no cable kept).
  u.d.drag(u.outPin(u.lfo, "y"), u.inPin(u.synth, "x") , 4);
  CHECK_FALSE(u.view().canvas().stickyWire());
  // (GU.11: the instrument group's main input takes it: wired, one more step)
  while (u.history() > h0) REQUIRE(u.ctx.undo());
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
  // A meta module: its call. (GU.4: the Envelope; the VCA is a module now, a project block.)
  d.move(at);
  REQUIRE(d.press("Tab") == KeyResult::Widget);
  d.type("envelope");
  REQUIRE(menuOf() != nullptr);
  REQUIRE(menuOf()->highlightedEntry() != nullptr);
  d.press("Return");
  d.step();
  CHECK_MESSAGE(ed->document().text().find("meta_envelope(") != std::string::npos, ed->status() << " | " << ed->document().text());
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

// ---- TL.7 (user 2026-11-09: "an empty track will basically just have a long cable stretch out … then you can clearly
// just fit in modules and audio effects inbetween") --------------------------------------------------------------------

TEST_CASE("TL.7: an empty audio track is one long, nearly straight cable from its lane's left end to its mixer at the "
          "right end; Z on it inserts an effect between them; a MIDI lane's cable leaves right of its port chips") {
  Ui u;
  doc::DocumentStore& st = u.ctx.store();
  auto made = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, u.bass, "Loop");
  REQUIRE(made.ok());
  REQUIRE(st.dispatch(made->command).ok());
  const doc::TrackId loop = made->tracks.at(0);
  u.view().sync();
  u.d.tree().layoutNow();
  const ProjectPatch& pp = u.pp();
  const uint32_t bi = pp.bandOf(loop);
  REQUIRE(bi != kNone);
  const Band& b = pp.bands[bi];
  REQUIRE(b.source != kNone);
  REQUIRE(pp.nodes[b.source].kind == NodeKind::Lane);
  REQUIRE(b.mixer != kNone);
  const Node& m = pp.nodes[b.mixer];
  // The mixer: at the lanes' right end, its input on the band's spine (nothing else on it: its own pin row sets it).
  CHECK(m.x + m.w == doctest::Approx(pp.laneX1 + metrics::kLaneInset - 12.0f));
  CHECK(m.y + pinOffset(m, 0) == doctest::Approx(b.y + metrics::kTitle + std::max(metrics::kPinTop, pinOffset(m, 0))));
  // BR.input-port: the lane's link is drawn into the band's input socket at its left edge; the one cable goes on from
  // the socket into the mixer (the same wire).
  REQUIRE(b.input != kNone);
  uint32_t trace = kNone, cable = kNone;
  for (uint32_t i = 0; i < pp.edges.size(); ++i) {
    if (pp.edges[i].from == b.source) {
      CHECK(trace == kNone);
      trace = i;
    }
    if (pp.edges[i].from == b.input) {
      CHECK(cable == kNone);
      cable = i;
    }
  }
  REQUIRE(trace != kNone);
  REQUIRE(cable != kNone);
  CHECK(pp.edges[trace].to == b.mixer);
  CHECK(pp.edges[trace].drawnTo == b.input);
  CHECK_FALSE(pp.edges[trace].cable);
  const Edge& e = pp.edges[cable];
  CHECK(e.to == b.mixer);
  CHECK(e.cable);
  CHECK(e.wire == pp.edges[trace].wire);
  CHECK(e.points.front().x == doctest::Approx(b.x0 + metrics::kInputInset));
  REQUIRE(e.points.size() == 2);
  const graph::Point2 a = e.points.front(), z = e.points.back();
  const float chord = std::hypot(z.x - a.x, z.y - a.y);
  CHECK(chord > 600.0f);  // it stretches across
  // Nearly straight: its rest shape hangs a few tens of world units at most.
  const uint32_t n = cablePointCount(chord);
  std::vector<graph::Point2> rest(n);
  cableRest(a, z, n, rest.data());
  float sag = 0.0f;
  for (const graph::Point2 q : rest) sag = std::max(sag, std::fabs((q.x - a.x) * (z.y - a.y) - (q.y - a.y) * (z.x - a.x)) / chord);
  CHECK(sag < 45.0f);
  CHECK(sag < 0.06f * chord);

  // Z over that cable (real dispatch): an effect on the track's main path, between the lane's left end and the mixer.
  {
    const Band bb = b;  // (the view may rebuild its model below)
    const ui::IRect r = u.view().canvas().rect();
    const double span = static_cast<double>(bb.x1 + 40.0f - (bb.x0 - 40.0f));
    CircuitCanvas::View v;
    v.zoom = std::min({1.0, static_cast<double>(r.w) / span, static_cast<double>(r.h) / static_cast<double>(bb.h + 80.0f)});
    v.x = bb.x0 - 40.0f;
    v.y = bb.y - 40.0f;
    u.view().canvas().setView(v);
    u.d.tree().layoutNow();
    u.d.step();
  }
  const size_t h0 = u.history();
  u.d.move(u.onCable(cable));
  u.d.keyDown('z');
  u.d.keyUp('z');
  u.view().sync();
  CHECK(u.history() == h0 + 1);
  const std::vector<doc::DeviceId> list = u.path(loop);
  REQUIRE(list.size() == 1);
  REQUIRE(u.device(list[0]) != nullptr);
  CHECK(u.device(list[0])->name == "VCA");
  {
    const ProjectPatch& q = u.pp();
    const Band& lb = q.bands[q.bandOf(loop)];
    const Node& vca = q.nodes[u.node(list[0])];
    const Node& mx = q.nodes[lb.mixer];
    CHECK(vca.x > q.laneX0);
    CHECK(vca.x + vca.w < mx.x);
    CHECK(mx.x + mx.w == doctest::Approx(q.laneX1 + metrics::kLaneInset - 12.0f));  // the mixer stayed at the end
    CHECK(vca.y >= lb.y + metrics::kTitle);  // in its band (Z places it where the cable was, on the pointer)
    CHECK(vca.y + vca.h <= lb.laneY);
    // The lane's cable now into the VCA, still from the lane's left end.
    bool found = false;
    for (const Edge& f : q.edges)
      if (f.from == lb.source) {
        CHECK(f.to == u.node(list[0]));
        CHECK(f.tapX == doctest::Approx(q.laneX0 + 12.0f));
        found = true;
      }
    CHECK(found);
  }
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  REQUIRE(u.ctx.undo());

  // A MIDI lane: BR.tuning-block — its notes cable goes out of the lane's left end into the band's tuning block
  // (BR.tuning-outside: hitched outside the band; a press on a chip still starts a new cable), and a cable of the same
  // wire on from the block.
  {
    const ProjectPatch& q = u.pp();
    const Band& kb = q.bands[q.bandOf(u.keys)];
    REQUIRE(kb.tuning != kNone);
    const Node& lane = q.nodes[kb.source];
    bool found = false;
    for (const Edge& f : q.edges)
      if (f.from == kb.source && f.kind == EdgeKind::Notes) {
        CHECK(f.drawnTo == kb.tuning);
        REQUIRE(f.points.size() == 2);
        CHECK(f.points.front().x == doctest::Approx(lane.x));
        CHECK(f.points.back().x < kb.x0);
        bool on = false;
        for (const Edge& g : q.edges) on = on || (g.from == kb.tuning && g.wire == f.wire && g.to == f.to);
        CHECK(on);
        found = true;
      }
    CHECK(found);
  }
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

TEST_CASE("render: project-cable-drag matches the golden image") {
  const char* name = "project-cable-drag";
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

TEST_CASE("node menu (GU.10): the stock presets spawn as their group presets — Utility closed as its panel, one undo step "
          "— real dispatch") {
  Ui u;
  const graph::Point2 at = u.emptyWorld();
  const size_t h0 = u.history();
  u.d.move(u.dev(at));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  u.d.type("utility");
  REQUIRE(u.menu() != nullptr);
  // (The tests' stand-in Utility patch is listed too: walk down to the module.)
  for (int k = 0; k < 8 && (u.menu()->highlightedEntry() == nullptr || u.menu()->highlightedEntry()->key != "module:utility"); ++k)
    u.d.press("Down");
  REQUIRE(u.menu()->highlightedEntry() != nullptr);
  REQUIRE(u.menu()->highlightedEntry()->key == "module:utility");
  u.d.press("Return");
  u.d.step();
  u.view().sync();
  CHECK(u.menu() == nullptr);
  CHECK(u.history() == h0 + 1);
  const doc::DeviceId made = u.newest();
  const doc::Device* g = u.device(made);
  REQUIRE(g != nullptr);
  CHECK(doc::isGroupDevice(*g));
  CHECK(g->name == "Utility");
  REQUIRE(g->panel != nullptr);
  CHECK(g->panel->elements.size() == 12);
  CHECK(doc::groupMembersDeep(u.track(u.keys).devices, made).size() > 40);  // its atom and value blocks
  const Node& n = u.pp().nodes[u.node(made)];
  CHECK(n.group);
  CHECK_FALSE(n.groupOpen);  // a panelled group spawns closed: its panel
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  REQUIRE(u.ctx.undo());
  CHECK(u.device(made) == nullptr);
}

// ---- TX.ports-always (bug report 2026-11-12_21-24-59: "a placed envelope doesnt have an input or output node … make sure
// that literally everything has a clean, clickable and interactable input/output node") -----------------------------------
// The cause: a closed block inside an OPENED group (the user placed the Envelope with the pointer over the instrument
// group's capsule) kept its main ports on its edges instead of orbiting, and a block drawn as a panel or a group's panel
// draws no jacks: nothing showed its ports until a cable's plug sat there. Now every closed block's main ports orbit,
// wherever it sits; only an atom block's circuit symbol keeps its leads (stubs the canvas draws).

namespace {

// A world point inside the opened instrument group's capsule, clear of its blocks, pins and cables.
graph::Point2 insideCapsule(const Ui& u) {
  const uint32_t c = u.node(u.synth);
  const Node& cap = u.pp().nodes[c];
  REQUIRE(cap.group);
  REQUIRE(cap.groupOpen);
  for (float y = cap.y + metrics::kCapsuleTitle + 16.0f; y < cap.y + cap.h - 24.0f; y += 8.0f)
    // (BR.patch-io: clear of the frame's socket column on its left, where the In terminal was: a block placed centred
    // at the frame's very edge would hang out of the band)
    for (float x = cap.x + 64.0f; x < cap.x + cap.w - 24.0f; x += 8.0f) {
      const Vec2 p = u.dev({x, y});
      if (u.view().canvas().nodeAt(p) == c && !u.view().canvas().pinAt(p).valid() && u.view().canvas().edgeAt(p) == kNone)
        return {x, y};
    }
  FAIL("no empty point in the capsule");
  return {};
}

// Block `id`'s main input / output (as its document signature says it has them): in the scene, drawn (an orbiting port
// the canvas paints, or a lead stub: a symbol's, an opened group's In / Out terminal's), hit there by the canvas (hover)
// and by the press's pin test (the view centred on it first: hit tests are of what is on screen).
void checkMainPorts(Ui& u, doc::DeviceId id, std::string_view whatV) {
  const std::string what(whatV);
  const uint32_t i = u.node(id);
  const doc::MainPorts mp = u.view().mainPorts(id);
  for (const bool input : {true, false}) {
    if ((input ? mp.in : mp.out).empty()) continue;
    const std::string side = what + (input ? " in" : " out");
    // A closed block: its own main port. An opened group (a capsule, no block): its In terminal's output, its Out
    // terminal's input (where cables reach its inside).
    uint32_t at = i;
    int32_t k = -1;
    bool pinInput = input;
    const Node& n = u.pp().nodes[i];
    if (n.group && n.groupOpen) {
      for (uint32_t t = 0; t < u.pp().nodes.size(); ++t) {
        const Node& m = u.pp().nodes[t];
        if (m.kind != NodeKind::Terminal || m.device != id || (input ? m.outputs.empty() : m.inputs.empty())) continue;
        at = t;
        k = 0;
        pinInput = !input;
      }
    } else {
      k = input ? n.mainIn : (n.outputs.empty() ? -1 : 0);
    }
    REQUIRE_MESSAGE(k >= 0, side << ": no port");
    const uint32_t p = u.view().canvas().scenePin(at, static_cast<uint32_t>(k), pinInput);
    REQUIRE_MESSAGE(p != kNone, side << ": no scene pin");
    const circuit::ScenePin& pin = (*u.view().canvas().scene()).pins[p];
    CHECK_MESSAGE(pin.visible, side << ": invisible");
    CHECK_MESSAGE((pin.orbit || pin.stub > 0.0f || pin.socket != 0), side << ": nothing draws it");  // (BR.patch-io: a frame socket)
    if (pin.orbit) CHECK_MESSAGE((pinInput ? u.pp().nodes[at].orbitIn.on : u.pp().nodes[at].orbitOut.on), side);
    const graph::Point2 w = u.view().canvas().portWorld(at, static_cast<uint32_t>(k), pinInput);
    {
      CircuitCanvas::View v = u.view().canvas().view();
      const ui::IRect r = u.view().canvas().rect();
      const float s = u.view().canvas().deviceScale();
      v.x = w.x - static_cast<float>(r.w) * 0.5f / s;
      v.y = w.y - static_cast<float>(r.h) * 0.5f / s;
      u.view().canvas().setView(v);
    }
    const Vec2 dv = u.dev(w);
    const circuit::CanvasHit h = u.view().canvas().hitTest(dv);
    CHECK_MESSAGE((h.kind == circuit::CanvasHit::Kind::Pin && h.index == p),
                  side << ": not hit (kind " << static_cast<int>(h.kind) << " index " << h.index << ", pin " << p << ")");
    const ProjectCanvas::PinHit ph = u.view().canvas().pinAt(dv);
    // BR.patch-io: an opened group's main Out is a socket on its frame — the Out terminal's input inside, the capsule's
    // own output outside, at one point: a press (no cable being drawn) is the output's (it draws from the group).
    const bool outSocket = n.group && n.groupOpen && !input;
    const uint32_t wantNode = outSocket ? i : at;
    const bool wantInput = outSocket ? false : pinInput;
    CHECK_MESSAGE((ph.node == wantNode && ph.port == (outSocket ? 0u : static_cast<uint32_t>(k)) && ph.input == wantInput),
                  side << ": not pressable (got node " << ph.node << " port " << ph.port << " input " << ph.input << ", want " << wantNode << " " << k << ")");
    // A real press there (window → canvas): an output starts a cable, an orbiting input slides; Esc cancels.
    if (!wantInput || pin.orbit) {
      u.d.move(dv);
      u.d.down(dv);
      CHECK_MESSAGE(u.view().canvas().drag() == (wantInput ? ProjectCanvas::Drag::Port : ProjectCanvas::Drag::Wire),
                    side << ": a press there is not the port's (drag " << static_cast<int>(u.view().canvas().drag()) << ")");
      u.d.press("Escape");
      u.d.up(dv);
    }
  }
}

// A real cable drag (window → canvas): pressed on `from`, out along `dir` (off its block's edge), onto `to`, released.
void dragCable(Ui& u, Vec2 from, Vec2 dir, Vec2 to) {
  u.d.move(from);
  u.d.down(from);
  for (int k = 1; k <= 4; ++k) u.d.move({from.x + dir.x * 12.0f * static_cast<float>(k), from.y + dir.y * 12.0f * static_cast<float>(k)});
  CHECK(u.view().canvas().drag() == ProjectCanvas::Drag::Wire);
  for (int k = 1; k <= 8; ++k) {
    const float t = static_cast<float>(k) / 8.0f;
    const Vec2 a{from.x + dir.x * 48.0f, from.y + dir.y * 48.0f};
    u.d.move({a.x + (to.x - a.x) * t, a.y + (to.y - a.y) * t});
  }
  u.d.up(to);
  u.d.step();
  u.view().sync();
}

}  // namespace

TEST_CASE("node menu (TX.ports-always): an Envelope placed in an opened group with Tab shows its ports; cables out of "
          "and into it — real dispatch") {
  Ui u;
  // As the user did: the pointer over the instrument group's capsule, Tab, "enve", Enter.
  const graph::Point2 at = insideCapsule(u);
  const size_t h0 = u.history();
  u.d.move(u.dev(at));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  u.d.type("enve");
  REQUIRE(u.menu() != nullptr);
  REQUIRE(u.menu()->highlightedEntry() != nullptr);
  CHECK(u.menu()->highlightedEntry()->key == "module:envelope");
  u.d.press("Return");
  u.d.step();
  u.view().sync();
  REQUIRE(u.history() == h0 + 1);
  // The new module: the newest group directly inside the opened instrument group, as in the report.
  doc::DeviceId env;
  for (const doc::DevicePtr& d : u.track(u.keys).devices)
    if (doc::isGroupDevice(*d) && d->group == u.synth && d->id.value > env.value) env = d->id;
  REQUIRE(u.device(env) != nullptr);
  CHECK(u.device(env)->name == "Envelope");
  {
    const Node& n = u.pp().nodes[u.node(env)];
    CHECK(n.capsule == u.node(u.synth));
    CHECK(n.groupPanel);  // closed: its panel
    CHECK(n.orbitIn.on);
    CHECK(n.orbitOut.on);
  }
  checkMainPorts(u, env, "Envelope");
  u.fit();
  // Hovering its output port rings it (the canvas's hover is that pin).
  const uint32_t ei = u.node(env);
  const uint32_t outPin = u.view().canvas().scenePin(ei, 0, false);
  u.d.move(u.dev(u.view().canvas().portWorld(ei, 0, false)));
  u.d.step();
  CHECK(u.view().canvas().hovered().kind == circuit::CanvasHit::Kind::Pin);
  CHECK(u.view().canvas().hovered().index == outPin);

  // A target with a free main input: a VCA module placed at the top level.
  u.d.move(u.dev(u.emptyWorld()));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  u.pick("vca", "VCA");
  const doc::DeviceId vca = u.newest();
  REQUIRE(vca != env);
  checkMainPorts(u, vca, "VCA");
  u.fit();
  const std::string envIn = u.view().mainPorts(env).in, envOut = u.view().mainPorts(env).out;
  const std::string vcaIn = u.view().mainPorts(vca).in, lfoOut = u.view().mainPorts(u.lfo).out;

  // The Envelope's output → the VCA's input: one wire, one undo step.
  auto portDev = [&](doc::DeviceId d, bool input) {
    const uint32_t i = u.node(d);
    const Node& n = u.pp().nodes[i];
    const Vec2 p = u.dev(u.view().canvas().portWorld(i, input ? static_cast<uint32_t>(n.mainIn) : 0u, input));
    REQUIRE(u.view().canvas().rect().contains(p));
    return p;
  };
  auto outward = [&](doc::DeviceId d, Vec2 p) {  // off the block's outline, from its centre through the port
    const Node& n = u.pp().nodes[u.node(d)];
    const Vec2 c = u.dev(n.outline.centre());
    const float len = std::max(1e-3f, std::hypot(p.x - c.x, p.y - c.y));
    return Vec2{(p.x - c.x) / len, (p.y - c.y) / len};
  };
  size_t h1 = u.history();
  {
    const Vec2 from = portDev(env, false);
    dragCable(u, from, outward(env, from), portDev(vca, true));
  }
  CHECK(u.history() == h1 + 1);
  CHECK(u.wired(env, envOut, vca, vcaIn));
  // Another block's output (the free LFO module's) → the Envelope's input: one wire, one undo step.
  h1 = u.history();
  {
    const Vec2 from = portDev(u.lfo, false);
    dragCable(u, from, outward(u.lfo, from), portDev(env, true));
  }
  CHECK(u.history() == h1 + 1);
  CHECK(u.wired(u.lfo, lfoOut, env, envIn));
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // Both undo, one step each.
  REQUIRE(u.ctx.undo());
  CHECK_FALSE(u.wired(u.lfo, lfoOut, env, envIn));
  CHECK(u.wired(env, envOut, vca, vcaIn));
  REQUIRE(u.ctx.undo());
  CHECK_FALSE(u.wired(env, envOut, vca, vcaIn));
}

TEST_CASE("node menu (TX.ports-always): every catalog entry placed unwired — top level and in an opened group — shows its "
          "main ports, hit where drawn") {
  Ui u;
  const std::shared_ptr<const NodeCatalog> cat = nodeCatalog(NodeCatalog::Scope::Project, u.ctx);
  REQUIRE(cat != nullptr);
  REQUIRE(cat->entries.size() > 20);
  // (Found again for each placement: a spawn pushes the blocks it lands on apart, view state that its undo keeps.)
  size_t placed = 0, refused = 0;
  for (const NodeEntry& e : cat->entries) {
    for (const bool inside : {false, true}) {
      u.fit();
      const graph::Point2 at = inside ? insideCapsule(u) : u.emptyWorld();
      const doc::Snapshot before = u.ctx.doc();
      const doc::DeviceId d = u.view().spawnNode(ProjectPatchView::choiceOf(e), at);
      if (!d.valid()) {  // only an instrument may be refused (inside the instrument group)
        CHECK_MESSAGE(e.instrument, e.key << " refused: " << u.ctx.status());
        MESSAGE(e.key << std::string(inside ? " refused in the group: " : " refused: ") << u.ctx.status());
        ++refused;
        continue;
      }
      u.view().sync();
      ++placed;
      const doc::Device* dv = u.device(d);
      REQUIRE(dv != nullptr);
      CHECK_MESSAGE((dv->group == u.synth) == inside, e.key << std::string(inside ? " not placed in the group, but in " : " placed in group ") << dv->group.value);
      // A module closes to its panel once placed (the first time the view sees it).
      if (doc::isGroupDevice(*dv) && dv->hasPanel()) CHECK_FALSE_MESSAGE(u.view().groupOpen(d), e.key);
      checkMainPorts(u, d, e.key);
      REQUIRE(u.ctx.undo());
      REQUIRE(u.ctx.doc() == before);
    }
  }
  MESSAGE(placed << " placed, " << refused << " refused, of " << cat->entries.size() << " entries");
  CHECK(placed + refused == 2 * cat->entries.size());  // (an instrument may be refused inside another instrument)
  CHECK(placed > refused);
}
