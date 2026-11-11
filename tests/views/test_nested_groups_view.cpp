// GU.2 (docs/plans/GU.md "Phases" 2): groups in groups in the project patch view — the capsules drawn recursively (an
// opened capsule inside its parent's, a closed one a block there), the instrument group (the outermost group fed notes)
// drawn as an instrument; and every new gesture through real event dispatch (window → canvas, the menu closing itself,
// the scene rebuilding during the event): Ctrl+G inside a capsule nests, Ctrl+Shift+G unnests, Ctrl+C / Ctrl+V copy and
// paste blocks at the pointer, a group preset saved from the block menu spawns from the node menu, an atom spawns as an
// atom block. Golden: project-patch-nested.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/main_path.h"
#include "pq/library/scan.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/browser/library.h"
#include "pq/views/circuit/node_menu.h"
#include "pq/views/fake_transport.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::circuit;
using namespace pq::views::project_patch;

namespace {

namespace fs = std::filesystem;

const doc::DeviceLibrary& devLib() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// The nested project (buildNestedGroupProject) in the shell's project patch view, panels off, pushes at once, a User
// Library in %TEMP%.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  NestedProjectIds ids;
  std::string root;

  Ui() {
    root = views::test::tempDir("nested_groups_view");
    ctx.setDeviceLibrary(&devLib());
    ctx.library().setUserLibrary(root);
    REQUIRE(buildNestedGroupProject(ctx.store(), &ids).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    view().fitAll(false);
    d.tree().layoutNow();
    d.step();
    d.tree().setFocus(&view().canvas());
  }
  ~Ui() {
    d.tree().setRoot(std::make_unique<Widget>());
    std::error_code ec;
    fs::remove_all(fs::path(std::u8string(root.begin(), root.end())), ec);
  }
  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const ProjectPatch& pp() const { return view().patch(); }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Device* device(doc::DeviceId id) const { return doc::findDevice(*ctx.doc(), id).device; }
  uint32_t node(doc::DeviceId id) const {
    const uint32_t n = pp().nodeOf(id);
    REQUIRE(n != kNone);
    return n;
  }
  Vec2 dev(graph::Point2 w) const { return view().canvas().toDevice(w); }
  void selectBlocks(std::initializer_list<doc::DeviceId> ds) {
    std::vector<uint32_t> sn;
    for (const doc::DeviceId d0 : ds) sn.push_back(view().scene().sceneNode[node(d0)]);
    std::sort(sn.begin(), sn.end());
    view().canvas().select(sn, {});
    d.tree().setFocus(&view().canvas());
  }
  NodeMenu* menu() const {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<NodeMenu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
  // Empty canvas on the Bass band, right of its blocks (world).
  graph::Point2 emptyOnBass() const {
    const Band& b = pp().bands[pp().bandOf(ids.bass)];
    float right = b.x0;
    for (const uint32_t n : b.devices) right = std::max(right, pp().nodes[n].x + pp().nodes[n].w);
    return {right + 120.0f, b.y + b.h * 0.5f};
  }
  void reindex() {
    library::ScanRequest req;
    req.places = ctx.library().places();
    library::DiskSource src;
    library::ScanResult r = library::rebuild(nullptr, req, src, library::probeFile);
    ctx.library().setIndex(r.index);
  }
  // The newest device ids of a track (created after `since`).
  std::vector<doc::DeviceId> newOn(doc::TrackId t, uint32_t since) const {
    std::vector<doc::DeviceId> v;
    for (const doc::DevicePtr& x : doc::findTrackPtr(*ctx.doc(), t)->devices)
      if (x->id.value > since) v.push_back(x->id);
    return v;
  }
  uint32_t maxId() const {
    uint32_t m = 0;
    doc::forEachDevice(*ctx.doc(), [&](const doc::Device& x, const doc::Track&) { m = std::max(m, x.id.value); });
    return m;
  }
};

}  // namespace

TEST_CASE("nested groups view: capsules in capsules — the opened one inside its parent's, the closed one a block there; "
          "only the outermost group fed notes is drawn as an instrument; the model matches the graph") {
  Ui u;
  u.view().setGroupOpen(u.ids.level, false);
  u.d.step();
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  const uint32_t g = u.node(u.ids.group), osc = u.node(u.ids.osc), level = u.node(u.ids.level);
  const Node& gn = u.pp().nodes[g];
  const Node& on = u.pp().nodes[osc];
  const Node& ln = u.pp().nodes[level];
  CHECK(gn.group);
  CHECK(gn.groupOpen);
  CHECK(gn.instrument);
  CHECK(gn.voices == doc::kDefaultGroupVoices);
  CHECK(on.group);
  CHECK(on.groupOpen);
  CHECK_FALSE(on.instrument);  // a nested group never voices again
  CHECK(on.capsule == g);
  CHECK(osc > g);  // after its parent: drawn over it, hit before it
  CHECK(u.node(u.ids.wt) > osc);
  CHECK(u.pp().nodes[u.node(u.ids.wt)].capsule == osc);
  // Inside its parent's capsule.
  CHECK(on.x >= gn.x);
  CHECK(on.y >= gn.y);
  CHECK(on.x + on.w <= gn.x + gn.w + 0.01f);
  CHECK(on.y + on.h <= gn.y + gn.h + 0.01f);
  // Closed: one block of its parent's with a miniature; wires to what is inside it end on it.
  CHECK(ln.group);
  CHECK_FALSE(ln.groupOpen);
  CHECK(ln.capsule == g);
  CHECK(std::find(ln.inner.begin(), ln.inner.end(), u.ids.mul) != ln.inner.end());
  CHECK_FALSE(ln.mini.empty());
  CHECK(u.pp().nodeOf(u.ids.mul) == level);
  // Bass: a plain group on the path holding an opened nested capsule; neither is an instrument.
  CHECK_FALSE(u.pp().nodes[u.node(u.ids.fx)].instrument);
  CHECK(u.pp().nodes[u.node(u.ids.tone)].capsule == u.node(u.ids.fx));
  CHECK(u.pp().nodes[u.node(u.ids.util)].capsule == u.node(u.ids.tone));
  // Moving the outer capsule moves everything inside it, nested capsules' contents too.
  const float wx = u.pp().nodes[u.node(u.ids.wt)].x;
  u.view().nudge(u.ids.group.value, 40.0f, 0.0f);
  u.d.step();
  CHECK(u.pp().nodes[u.node(u.ids.wt)].x == doctest::Approx(wx + 40.0f));
}

// PF.2: an opened group's inner layout is cached across builds (ModelCache). A re-layout elsewhere reuses it; a block
// inside it resized (view state) or a wire among its contents deleted (the document) lays it out again, exactly as a
// fresh view of the same project does.
TEST_CASE("nested groups view (PF.2): an opened group's cached layout follows edits inside it, as a fresh view lays it out") {
  struct Placed {
    uint32_t device;
    uint8_t kind;
    float x, y, w, h;
    bool operator<(const Placed& o) const { return device != o.device ? device < o.device : kind < o.kind; }
    bool operator==(const Placed&) const = default;
  };
  // Everything directly in the instrument group's capsule, relative to it.
  auto inner = [](const Ui& u) {
    const uint32_t cap = u.node(u.ids.group);
    const Node& c = u.pp().nodes[cap];
    std::vector<Placed> v;
    for (const Node& n : u.pp().nodes)
      if (n.capsule == cap) v.push_back({n.device.value, static_cast<uint8_t>(n.kind), n.x - c.x, n.y - c.y, n.w, n.h});
    std::sort(v.begin(), v.end());
    return v;
  };
  Ui a;
  const std::vector<Placed> before = inner(a);
  REQUIRE(before.size() >= 4);
  // A re-layout that does not touch the group (a block of another band nudged): the layout comes from the cache.
  const uint64_t hits0 = a.view().layoutCache().hits;
  a.view().nudge(a.ids.fx.value, 0.0f, 30.0f);
  a.d.step();
  CHECK(a.view().layoutCache().hits > hits0);
  CHECK(inner(a) == before);
  // View state: a block inside the capsule scaled. The layout is made again (a miss), as a fresh view makes it.
  const uint64_t misses0 = a.view().layoutCache().misses;
  REQUIRE(a.view().setBlockScale(a.ids.env, 2.0f));
  a.d.step();
  CHECK(a.view().layoutCache().misses > misses0);
  CHECK(inner(a) != before);
  {
    Ui b;
    REQUIRE(b.view().setBlockScale(b.ids.env, 2.0f));
    b.d.step();
    CHECK(inner(a) == inner(b));
  }
  // The document: a wire among the capsule's contents deleted (both ends directly inside the group).
  doc::WireId cut;
  for (const doc::Wire& w : doc::wiresOf(*a.ctx.doc())) {
    if (w.fromNoteSource() || w.toMixer() || cut.valid()) continue;
    const doc::Device* from = a.device(w.from);
    const doc::Device* to = a.device(w.to);
    if (from != nullptr && to != nullptr && from->group == a.ids.group && to->group == a.ids.group && w.from != w.to) cut = w.id;
  }
  REQUIRE(cut.valid());
  const uint64_t misses1 = a.view().layoutCache().misses;
  auto e = doc::ops::deleteWires(*a.ctx.doc(), std::vector<doc::WireId>{cut});
  REQUIRE(e.ok());
  REQUIRE(a.ctx.store().dispatch(e->command).ok());
  a.d.step();
  CHECK(a.view().layoutCache().misses > misses1);
  {
    Ui b;
    REQUIRE(b.view().setBlockScale(b.ids.env, 2.0f));
    auto eb = doc::ops::deleteWires(*b.ctx.doc(), std::vector<doc::WireId>{cut});
    REQUIRE(eb.ok());
    REQUIRE(b.ctx.store().dispatch(eb->command).ok());
    b.d.step();
    CHECK(inner(a) == inner(b));
  }
  CHECK(checkAgainstGraph(a.pp(), *a.ctx.doc(), *a.view().lowered()) == "");
}

TEST_CASE("nested groups view: Ctrl+G inside a capsule nests (one step, the wires as they were), Ctrl+Shift+G unnests "
          "(the same document) — real key dispatch") {
  Ui u;
  const doc::Snapshot before = u.ctx.doc();
  const size_t h0 = u.history();
  u.selectBlocks({u.ids.pa, u.ids.env});
  CHECK(u.d.press("Ctrl+G") != KeyResult::Unhandled);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  const doc::DeviceId n = u.device(u.ids.pa)->group;
  REQUIRE(n.valid());
  CHECK(n != u.ids.group);
  CHECK(u.device(n)->group == u.ids.group);
  CHECK(u.device(u.ids.env)->group == n);
  CHECK(doc::wiresOf(*u.ctx.doc()) == doc::wiresOf(*before));
  CHECK(doc::isInstrumentGroup(*u.ctx.doc(), u.ids.group));
  CHECK_FALSE(doc::isInstrumentGroup(*u.ctx.doc(), n));
  // The scene rebuilt during the event: the new capsule is in the model, inside the instrument group's.
  const uint32_t cn = u.node(n);
  CHECK(u.pp().nodes[cn].group);
  CHECK(u.pp().nodes[cn].capsule == u.node(u.ids.group));
  CHECK(u.pp().nodes[u.node(u.ids.pa)].capsule == cn);
  CHECK(u.view().selectedGroup() == n);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // Ctrl+Shift+G with the new group selected: back as it was.
  CHECK(u.d.press("Ctrl+Shift+G") != KeyResult::Unhandled);
  u.d.step();
  CHECK(u.history() == h0 + 2);
  CHECK(*u.ctx.doc() == *before);
  REQUIRE(u.ctx.undo());
  CHECK(u.device(u.ids.pa)->group == n);
  REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc().get() == before.get());
}

TEST_CASE("nested groups view: Ctrl+C copies a nested group with its contents, Ctrl+V pastes a fresh copy at the "
          "pointer (one step; into the capsule under it) — real key and pointer dispatch") {
  Ui u;
  const size_t h0 = u.history();
  // Copy the Osc capsule (pressing its frame selects the group): its Wavetable and wires come along.
  u.view().selectGroup(u.ids.osc);
  u.d.tree().setFocus(&u.view().canvas());
  CHECK(u.d.press("Ctrl+C") != KeyResult::Unhandled);
  REQUIRE(u.ctx.blockClipboard().devices.size() == 2);
  CHECK(u.history() == h0);  // copying changes nothing
  // Paste on the Bass band, outside any capsule: free there.
  const uint32_t since = u.maxId();
  const graph::Point2 at = u.emptyOnBass();
  u.d.move(u.dev(at));
  CHECK(u.d.press("Ctrl+V") != KeyResult::Unhandled);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  const std::vector<doc::DeviceId> made = u.newOn(u.ids.bass, since);
  REQUIRE(made.size() == 2);
  doc::DeviceId g2, wt2;
  for (const doc::DeviceId id : made) (doc::isGroupDevice(*u.device(id)) ? g2 : wt2) = id;
  REQUIRE(g2.valid());
  REQUIRE(wt2.valid());
  CHECK_FALSE(u.device(g2)->group.valid());
  CHECK(u.device(wt2)->group == g2);
  size_t inner = 0;
  for (const doc::Wire& w : doc::wiresOf(*u.ctx.doc()))
    if ((w.from == g2 || w.from == wt2) && (w.to == g2 || w.to == wt2)) ++inner;
  CHECK(inner == 2);  // In → Wavetable, Wavetable → Out
  // Drawn where the pointer was (the scene rebuilt during the key event), its contents inside it.
  const Node& c = u.pp().nodes[u.node(g2)];
  CHECK(c.group);
  CHECK(u.pp().nodes[u.node(wt2)].capsule == u.node(g2));
  CHECK(std::fabs((c.x + c.w * 0.5f) - at.x) < c.w);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // Pasted again over the FX capsule: into it (a member of FX).
  const uint32_t fx = u.node(u.ids.fx);
  const Node& fn = u.pp().nodes[fx];
  const graph::Point2 inFx{fn.x + fn.w - 30.0f, fn.y + fn.h - 12.0f};
  const uint32_t since2 = u.maxId();
  u.d.move(u.dev(inFx));
  CHECK(u.d.press("Ctrl+V") != KeyResult::Unhandled);
  u.d.step();
  CHECK(u.history() == h0 + 2);
  const std::vector<doc::DeviceId> made2 = u.newOn(u.ids.bass, since2);
  REQUIRE(made2.size() == 2);
  doc::DeviceId g3;
  for (const doc::DeviceId id : made2)
    if (doc::isGroupDevice(*u.device(id))) g3 = id;
  REQUIRE(g3.valid());
  CHECK(u.device(g3)->group == u.ids.fx);
  // One undo step each.
  REQUIRE(u.ctx.undo());
  CHECK(u.device(g3) == nullptr);
  REQUIRE(u.ctx.undo());
  CHECK(u.device(g2) == nullptr);
}

TEST_CASE("nested groups view: Save as Group Preset (the block menu), then the node menu spawns it — Tab at the pointer, "
          "type, Enter: a fresh copy, the menu closed itself, the scene rebuilt; an atom spawns as an atom block") {
  Ui u;
  // The block menu's item, chosen by a click (real dispatch through the overlay).
  {
    const uint32_t osc = u.node(u.ids.osc);
    const Node& n = u.pp().nodes[osc];
    ui::Menu* m = u.view().openNodeMenu(osc, u.dev({n.x + 20.0f, n.y + 6.0f}));
    REQUIRE(m != nullptr);
    u.d.step();
    int32_t item = -1;
    for (size_t i = 0; i < m->items().size(); ++i)
      if (m->items()[i].label == "Save as Group Preset") item = static_cast<int32_t>(i);
    REQUIRE(item >= 0);
    const IRect r = m->itemRect(item);
    u.d.click({static_cast<float>(r.x + r.w / 2), static_cast<float>(r.y + r.h / 2)});
    u.d.step();
    CHECK(u.shell->overlay().openCount() == 0);  // the menu closed itself
  }
  const fs::path file = fs::path(std::u8string(u.root.begin(), u.root.end())) / "Groups" / "Osc.pqpreset";
  REQUIRE(fs::exists(file));
  u.reindex();
  // Tab over empty canvas on Bass, search, Enter.
  const size_t h0 = u.history();
  const uint32_t since = u.maxId();
  u.d.move(u.dev(u.emptyOnBass()));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  u.d.step();
  NodeMenu* m = u.menu();
  REQUIRE(m != nullptr);
  REQUIRE(u.d.type("Osc"));
  u.d.step();
  int32_t row = -1;
  for (int32_t i = 0; i < static_cast<int32_t>(m->rows().size()); ++i) {
    const NodeMenu::Row& r = m->rows()[static_cast<size_t>(i)];
    if (r.kind == NodeMenu::Row::Kind::Entry && m->catalog().entries[r.entry].name == "Osc" &&
        m->catalog().entries[r.entry].item == NodeItem::LibraryFile)
      row = i;
  }
  REQUIRE_MESSAGE(row >= 0, "the group preset is not in the node menu");
  m->setHighlighted(row);
  REQUIRE(u.d.press("Return") == KeyResult::Widget);
  u.d.step();
  CHECK(u.menu() == nullptr);  // the menu closed itself
  CHECK(u.history() == h0 + 1);
  const std::vector<doc::DeviceId> made = u.newOn(u.ids.bass, since);
  REQUIRE(made.size() == 2);
  doc::DeviceId g;
  for (const doc::DeviceId id : made)
    if (doc::isGroupDevice(*u.device(id))) g = id;
  REQUIRE(g.valid());
  CHECK(u.pp().nodeOf(g) != kNone);  // the scene rebuilt
  for (const doc::DeviceId id : made)
    if (id != g) CHECK(u.device(id)->group == g);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // An atom from the menu is an atom block (no generated text).
  const uint32_t since2 = u.maxId();
  u.d.move(u.dev({u.emptyOnBass().x + 200.0f, u.emptyOnBass().y}));
  REQUIRE(u.d.press("Tab") == KeyResult::Widget);
  u.d.step();
  m = u.menu();
  REQUIRE(m != nullptr);
  REQUIRE(u.d.type("tanh"));
  u.d.step();
  row = -1;
  for (int32_t i = 0; i < static_cast<int32_t>(m->rows().size()); ++i) {
    const NodeMenu::Row& r = m->rows()[static_cast<size_t>(i)];
    if (r.kind == NodeMenu::Row::Kind::Entry && m->catalog().entries[r.entry].key == "atom:tanh") row = i;
  }
  REQUIRE(row >= 0);
  m->setHighlighted(row);
  REQUIRE(u.d.press("Return") == KeyResult::Widget);
  u.d.step();
  const std::vector<doc::DeviceId> atom = u.newOn(u.ids.bass, since2);
  REQUIRE(atom.size() == 1);
  CHECK(doc::isAtomDevice(*u.device(atom.front())));
  CHECK(u.device(atom.front())->patch == "atom:tanh");
}

// TX.group-resize (user 2026-11-09: "you should be able to shrink them holding s and shrinking them will comparatively
// shrink everything else within it"): S + drag on an opened group's frame scales the group and everything in it, one
// factor composed with each member's own (a nested group's contents too); view state, not undoable; Esc restores; S +
// double-click resets; saved with the project.
TEST_CASE("nested groups view (TX.group-resize): S-drag on an opened group's frame scales it and its contents; Esc, "
          "S + double-click") {
  Ui u;
  ProjectPatchView& v = u.view();
  ProjectCanvas& cv = v.canvas();
  v.focusBand(u.ids.bass, false);
  u.d.tree().layoutNow();
  u.d.step();
  REQUIRE(v.setBlockScale(u.ids.tone, 0.5f));  // the nested group's own factor (composes with its parent's)
  struct Snap {
    float satX, satW, satH, satScale, toneX, toneW, utilW, utilScale, fxW, fxH;
  };
  auto snap = [&] {
    const ProjectPatch& pp = u.pp();
    const Node &sat = pp.nodes[u.node(u.ids.sat)], &tone = pp.nodes[u.node(u.ids.tone)], &util = pp.nodes[u.node(u.ids.util)];
    const Node& fx = pp.nodes[u.node(u.ids.fx)];
    REQUIRE(fx.group);
    REQUIRE(fx.groupOpen);
    REQUIRE(tone.groupOpen);
    CHECK(sat.capsule == u.node(u.ids.fx));
    CHECK(util.capsule == u.node(u.ids.tone));
    return Snap{sat.x, sat.w, sat.h, sat.scale, tone.x, tone.w, util.w, util.scale, fx.w, fx.h};
  };
  // A point on the group's frame (inside it, on none of its contents), in device px.
  auto frame = [&] {
    const Node& fx = u.pp().nodes[u.node(u.ids.fx)];
    for (float y = fx.y + 2.0f; y < fx.y + fx.h; y += 2.0f)
      for (float x = fx.x + 2.0f; x < fx.x + fx.w; x += 2.0f)
        if (const Vec2 p = u.dev({x, y}); cv.nodeAt(p) == u.node(u.ids.fx)) return p;
    FAIL("no frame point");
    return Vec2{};
  };
  const Snap s0 = snap();
  CHECK(s0.utilScale == doctest::Approx(0.5f));
  const size_t h0 = u.history();
  // S held over the frame: ⤡ and the group's hint.
  Vec2 p = frame();
  u.d.keyDown('s');
  u.d.move(p);
  CHECK(u.d.tree().cursor() == Cursor::ResizeNWSE);
  CHECK(std::string(cv.info().title) == "Resize group");
  // Up-left by a quarter of its diagonal: 0.75×.
  const float z = cv.deviceScale();
  const Vec2 to{p.x - 0.25f * s0.fxW * z, p.y - 0.25f * s0.fxH * z};
  u.d.down(p);
  CHECK(cv.drag() == ProjectCanvas::Drag::Resize);
  for (int i = 1; i <= 4; ++i) u.d.move({p.x + (to.x - p.x) * static_cast<float>(i) / 4.0f, p.y + (to.y - p.y) * static_cast<float>(i) / 4.0f});
  u.d.up(to);
  u.d.keyUp('s');
  u.d.step();
  const float k = v.blockScale(u.ids.fx);
  CHECK(k == doctest::Approx(0.75f).epsilon(0.02));
  CHECK(u.history() == h0);  // view state: no undo step …
  CHECK(u.ctx.dirty());      // … but saved
  const Snap s1 = snap();
  // Every member's size and their distances by the factor (a nested group's contents composed with its own).
  CHECK(s1.satW == doctest::Approx(s0.satW * k).epsilon(0.01));
  CHECK(s1.satH == doctest::Approx(s0.satH * k).epsilon(0.01));
  CHECK(s1.satScale == doctest::Approx(s0.satScale * k));
  CHECK(s1.utilW == doctest::Approx(s0.utilW * k).epsilon(0.01));
  CHECK(s1.utilScale == doctest::Approx(0.5f * k));
  CHECK(s1.toneX - s1.satX == doctest::Approx((s0.toneX - s0.satX) * k).epsilon(0.02));
  CHECK(s1.fxW < s0.fxW);
  CHECK(s1.fxH < s0.fxH);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *v.lowered()) == "");
  // Esc during a drag: back to the factor at the press.
  p = frame();
  u.d.keyDown('s');
  u.d.move(p);
  u.d.down(p);
  u.d.move({p.x + 60.0f, p.y + 40.0f});
  u.d.move({p.x + 120.0f, p.y + 80.0f});
  CHECK(v.blockScale(u.ids.fx) > k);
  u.d.press("Escape");
  u.d.up({p.x + 120.0f, p.y + 80.0f});
  u.d.keyUp('s');
  u.d.step();
  CHECK(v.blockScale(u.ids.fx) == doctest::Approx(k));
  CHECK(u.pp().nodes[u.node(u.ids.sat)].w == doctest::Approx(s1.satW));
  // (Saved and loaded: groups view (TX.group-resize) — this fixture's grouped series does not pass the strict reader.)
  // S + double-click on the frame: its own size again (the nested group keeps its own factor).
  ProjectCanvas& cv2 = u.view().canvas();
  p = [&] {
    const Node& fx = u.pp().nodes[u.node(u.ids.fx)];
    for (float y = fx.y + 2.0f; y < fx.y + fx.h; y += 2.0f)
      for (float x = fx.x + 2.0f; x < fx.x + fx.w; x += 2.0f)
        if (const Vec2 q = cv2.toDevice({x, y}); cv2.nodeAt(q) == u.node(u.ids.fx)) return q;
    FAIL("no frame point");
    return Vec2{};
  }();
  u.d.keyDown('s');
  u.d.move(p);
  u.d.doubleClick(p);
  u.d.keyUp('s');
  u.d.step();
  CHECK(u.view().blockScale(u.ids.fx) == 1.0f);
  const Snap s2 = snap();
  CHECK(s2.satW == doctest::Approx(s0.satW));
  CHECK(s2.utilScale == doctest::Approx(0.5f));
  CHECK(s2.toneX - s2.satX == doctest::Approx(s0.toneX - s0.satX).epsilon(0.01));
}

TEST_CASE("render: project-patch-nested matches the golden image") {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize("project-patch-nested", w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene("project-patch-nested", {}, w, h, {}, o, &devLib());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/project-patch-nested.png";
  auto ref = readPng(path);
  if (!ref) (void)writePng("project-patch-nested.actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden("project-patch-nested", *img, *ref, true, &d));
  MESSAGE(pq::ui::test::describe(d));
}
