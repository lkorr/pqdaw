// bug-spawn-resize (the user: "resizing a module will move other modules around even if they arent in contact
// sometimes. then also spawning modules sometimes spawns them really far away, and they will sometimes be very large
// relative to the camera zoom"): in the project patch view
//   - an S-resize or an edge stretch moves only the blocks the block comes to overlap (pushed to the gap, the resized
//     block holding its place) — on a band's row, its main path, inside a capsule, the band below, after earlier nudges;
//     two blocks nearer than the gap but apart stay apart where they are;
//   - a spawned block lands at the pointer, and where a block is there at the nearest free spot (nothing else moves);
//   - a spawned block is the same size on screen at any zoom (its own size at zoom 1, clamped 0.05 … 3), on every spawn
//     path (Tab, a dropped cable, Tab and Z over a cable, Z over a port); a paste keeps the copied sizes.
// Gestures through the real window → canvas dispatch (S-drags, edge drags, Tab, Z, cable drags, Ctrl+C / Ctrl+V).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "../support/blocks.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/modules.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/node_menu.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/group_panel_face.h"
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

struct R {
  float x = 0, y = 0, w = 0, h = 0;
  bool operator==(const R&) const = default;
};

// "Keys": the default instrument group → a gain block (`util`) → a tanh block (`sat`) on the main path; free: an LFO
// module driving the gain's `g`, a Delay Tap (a panel whose edge stretches), then five tanh blocks `fa` … `fe` on the row
// under the path (40 apart, in list order). "Bass" under it (its default instrument group).
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  doc::TrackId keys, bass;
  doc::DeviceId synth, util, sat, lfo, tap, fa, fb, fc, fd, fe;  // fa … fe: the free tanh blocks
  doc::WireId lfoWire;

  Ui() {
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    auto run = [&](const doc::ops::EditResult& r) {
      REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
      REQUIRE(st.dispatch(r->command).ok());
      return *r;
    };
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks.at(0);
    bass = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, keys, "Bass")).tracks.at(0);
    synth = doc::mainPath(*st.current(), *doc::findTrackPtr(*st.current(), keys)).blocks.at(0)->id;
    util = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "atom:gain", doc::DeviceKind::AudioEffect)).devices.at(0);
    sat = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "atom:tanh", doc::DeviceKind::AudioEffect)).devices.at(0);
    lfo = test_blocks::addModule(st, keys, "lfo", doc::ops::kOffPath);
    {
      doc::IdSource local;
      doc::DelayTapRoles roles;
      const doc::BlockSet set = doc::delayTapBlocks(local, {}, &roles);
      const doc::ops::Edit pasted = run(doc::ops::pasteBlocks(*st.current(), st.ids(), keys, set));
      for (size_t i = 0; i < set.devices.size(); ++i)
        if (set.devices[i]->id == roles.group) tap = pasted.devices.at(i);
      REQUIRE(tap.valid());
    }
    for (doc::DeviceId* x : {&fa, &fb, &fc, &fd, &fe})
      *x = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "atom:tanh", doc::DeviceKind::AudioEffect, doc::ops::kOffPath)).devices.at(0);
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
    look(1.0, centreOf(fa));
    d.tree().setFocus(&view().canvas());
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }

  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  ProjectCanvas& canvas() const { return view().canvas(); }
  const ProjectPatch& pp() const {
    view().sync();
    return view().patch();
  }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Device* device(doc::DeviceId id) const { return doc::findDevice(*ctx.doc(), id).device; }
  uint32_t node(doc::DeviceId id) const {
    const uint32_t n = pp().nodeOf(id);
    REQUIRE(n != kNone);
    return n;
  }
  R rect(doc::DeviceId id) const {
    const Node& n = pp().nodes[node(id)];
    return {n.x, n.y, n.w, n.h};
  }
  graph::Point2 centreOf(doc::DeviceId id) const {
    const R r = rect(id);
    return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
  }
  Vec2 dev(graph::Point2 w) const { return canvas().toDevice(w); }
  // The camera at `zoom`, centred on world point `mid`.
  void look(double zoom, graph::Point2 mid) {
    CircuitCanvas::View v;
    v.zoom = zoom;
    canvas().setView(v);
    const float s = canvas().deviceScale();
    const ui::IRect r = canvas().rect();
    v.x = mid.x - static_cast<double>(r.w) * 0.5 / s;
    v.y = mid.y - static_cast<double>(r.h) * 0.5 / s;
    canvas().setView(v);
    d.tree().layoutNow();
    d.step();
  }
  // Every node's rect relative to its band's top-left (a band that grows moves the bands below, their blocks with them:
  // not a move of those blocks) and every band's place (kind -1), by what it is (a device's id; a band's mixer / source
  // / Out by its band).
  std::map<std::tuple<int, uint32_t, uint32_t>, R> all() const {
    std::map<std::tuple<int, uint32_t, uint32_t>, R> m;
    const ProjectPatch& p = pp();
    for (const Node& n : p.nodes) {
      if (n.kind == NodeKind::Lane || n.kind == NodeKind::Input || n.band >= p.bands.size()) continue;  // (a lane: the strip)
      const Band& b = p.bands[n.band];
      m[{static_cast<int>(n.kind), n.device.value, n.band}] = {n.x - b.x0, n.y - b.y, n.w, n.h};
    }
    for (uint32_t i = 0; i < p.bands.size(); ++i) m[{-1, p.bands[i].track.value, i}] = {p.bands[i].x0, p.bands[i].y, 0.0f, p.bands[i].h};
    return m;
  }
  // What moved or changed size between two snapshots, but the devices `except`; `blocksOnly`: device blocks only (a
  // spawn that reaches past the bands' right end takes the lanes' end, and TL.7 the mixers there, with it; a block added
  // to a band's row moves the returns' and the master's buses right: layout rules for what is not a block).
  std::vector<std::string> changed(const std::map<std::tuple<int, uint32_t, uint32_t>, R>& was,
                                   const std::map<std::tuple<int, uint32_t, uint32_t>, R>& now,
                                   std::initializer_list<doc::DeviceId> except = {}, bool blocksOnly = false) const {
    std::vector<std::string> out;
    for (const auto& [k, r] : was) {
      const uint32_t dev = std::get<1>(k);
      if (blocksOnly && std::get<0>(k) != static_cast<int>(NodeKind::Device)) continue;
      if (std::get<0>(k) == static_cast<int>(NodeKind::Device) &&
          std::any_of(except.begin(), except.end(), [&](doc::DeviceId x) { return x.value == dev; }))
        continue;
      const auto it = now.find(k);
      if (it == now.end()) {
        out.push_back("gone: kind " + std::to_string(std::get<0>(k)) + " id " + std::to_string(dev));
        continue;
      }
      const R& q = it->second;
      if (std::fabs(q.x - r.x) > 0.5f || std::fabs(q.y - r.y) > 0.5f || std::fabs(q.w - r.w) > 0.5f || std::fabs(q.h - r.h) > 0.5f)
        out.push_back("kind " + std::to_string(std::get<0>(k)) + " id " + std::to_string(dev) + " band " + std::to_string(std::get<2>(k)) +
                      ": " + std::to_string(r.x) + "," + std::to_string(r.y) + " " + std::to_string(r.w) + "x" + std::to_string(r.h) +
                      " -> " + std::to_string(q.x) + "," + std::to_string(q.y) + " " + std::to_string(q.w) + "x" + std::to_string(q.h));
    }
    return out;
  }
  // Block `id`'s top-left to world (x, y) (a nudge, as a drag drops it).
  void place(doc::DeviceId id, float x, float y) {
    const R r = rect(id);
    view().nudge(id.value, x - r.x, y - r.y);
    d.step();
  }
  // An S-drag on `id` from its centre to `k` times its size (its bottom-right corner along the diagonal), S pressed
  // before, released after; the overlap rule then (pushes at once here).
  void sResize(doc::DeviceId id, float k) {
    const R r = rect(id);
    const float s = canvas().deviceScale();
    const float k0 = view().blockScale(id);
    const float dx = (k / k0 - 1.0f) * r.w * s, dy = (k / k0 - 1.0f) * r.h * s;
    const Vec2 c = dev({r.x + r.w * 0.5f, r.y + r.h * 0.5f});
    d.keyDown('s');
    d.move(c);
    d.down(c);
    for (int i = 1; i <= 4; ++i) d.move({c.x + dx * static_cast<float>(i) / 4.0f, c.y + dy * static_cast<float>(i) / 4.0f});
    d.up({c.x + dx, c.y + dy});
    d.keyUp('s');
    d.step();
  }
  NodeMenu* menu() const {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<NodeMenu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
  // The open menu: the entry named `name` (a module before a native engine of that name), Enter.
  void pick(std::string_view query, std::string_view name) {
    NodeMenu* m = menu();
    REQUIRE(m != nullptr);
    m->setQuery(std::string(query));
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
  // Tab at world point `at`, the VCA picked: the new block.
  doc::DeviceId tabVca(graph::Point2 at) {
    const size_t h = history();
    d.move(dev(at));
    REQUIRE(d.press("Tab") == KeyResult::Widget);
    pick("vca", "VCA");
    REQUIRE(history() == h + 1);
    return newest();
  }
  doc::DeviceId newest() const {
    doc::DeviceId best;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      for (const doc::DevicePtr& x : t.devices)
        if (x->id.value > best.value) best = x->id;
    });
    if (const doc::DeviceLoc l = doc::findDevice(*ctx.doc(), best); l.device != nullptr && l.device->group.valid())
      best = doc::outermostGroup(l.track->devices, best);
    return best;
  }
  // Block `v` centred at `at`, or (a block was there) at the free spot beside one: a gap from the nearest block of its
  // level, and its centre no further from `at` than that block's and its own half sizes and the gap.
  bool atPointerOrBeside(doc::DeviceId v, graph::Point2 at) const {
    const R r = rect(v);
    const graph::Point2 c{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    if (std::fabs(c.x - at.x) <= 1.0f && std::fabs(c.y - at.y) <= 1.0f) return true;
    const Node& vn = pp().nodes[node(v)];
    float best = 1e30f;
    const Node* near = nullptr;
    for (const Node& o : pp().nodes) {
      if (&o == &vn || o.band != vn.band || o.capsule != vn.capsule || o.w <= 0.0f) continue;
      if (o.kind != NodeKind::Device && o.kind != NodeKind::Mixer && o.kind != NodeKind::Sum && o.kind != NodeKind::Output) continue;
      const float gx = std::max(o.x - (r.x + r.w), r.x - (o.x + o.w)), gy = std::max(o.y - (r.y + r.h), r.y - (o.y + o.h));
      const float g = std::max(gx, gy);
      if (g < best) best = g, near = &o;
    }
    if (near == nullptr) return false;
    MESSAGE("beside a block " << best << " away; centre " << c.x - at.x << ", " << c.y - at.y << " from the pointer");
    return best >= ProjectPatchView::kPushGap - 1.0f && best <= ProjectPatchView::kPushGap + 1.5f &&
           std::fabs(c.x - at.x) <= (near->w + r.w) * 0.5f + ProjectPatchView::kPushGap + 1.0f + std::fabs(near->x + near->w * 0.5f - at.x) &&
           std::fabs(c.y - at.y) <= (near->h + r.h) * 0.5f + ProjectPatchView::kPushGap + 1.0f + std::fabs(near->y + near->h * 0.5f - at.y);
  }
  bool clear(Vec2 m) const {
    const uint32_t n = canvas().nodeAt(m);
    return (n == kNone || (pp().nodes[n].group && pp().nodes[n].groupOpen)) && !canvas().pinAt(m).valid();
  }
  uint32_t cableOf(doc::WireId w) const {
    for (uint32_t i = 0; i < pp().edges.size(); ++i)
      if (pp().edges[i].wire == w) return i;
    FAIL("no cable for the wire");
    return kNone;
  }
  // A device point on the cable of model edge `edge`, clear of blocks and pins, as near its middle as there is one.
  Vec2 onCable(uint32_t edge) const {
    const Edge& ed = pp().edges[edge];
    REQUIRE(ed.points.size() >= 2);
    float x0 = ed.points.front().x, x1 = x0, y0 = ed.points.front().y, y1 = y0;
    for (const graph::Point2& p : ed.points) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
    const float mx = (x0 + x1) * 0.5f;
    for (float off = 0.0f; off <= (x1 - x0) * 0.5f; off += 2.0f)
      for (const float x : {mx - off, mx + off})
        for (float y = y0 - 60.0f; y <= y1 + 60.0f; y += 1.0f) {
          const Vec2 m = dev({x, y});
          if (canvas().edgeAt(m) == edge && clear(m)) return m;
        }
    FAIL("no point on the cable");
    return {};
  }
};

float onScreenW(const Ui& u, doc::DeviceId id) { return u.rect(id).w * static_cast<float>(u.canvas().view().zoom); }

bool overlaps(const R& p, const R& q) { return p.x < q.x + q.w && q.x < p.x + p.w && p.y < q.y + q.h && q.y < p.y + p.h; }

}  // namespace

TEST_CASE("bug-spawn-resize: an S-resize moves no block it does not come to overlap — the row, the path, the band "
          "below, after earlier nudges; one it overlaps is pushed to the gap, the resized one holding its place") {
  Ui u;
  // Room in the band under the row (the LFO far below it: the band grows to hold it), so a grown row block stays inside.
  u.place(u.lfo, u.rect(u.lfo).x, u.rect(u.lfo).y + 300.0f);
  // Near but apart (4 units: inside the push gap), d and e: no push may touch them.
  {
    const R rd = u.rect(u.fd);
    u.place(u.fe, rd.x + rd.w + 4.0f, rd.y);
  }
  // An earlier nudge: b moved 10 right (30 from a, 30 from c).
  u.place(u.fb, u.rect(u.fb).x + 10.0f, u.rect(u.fb).y);
  u.look(1.0, u.centreOf(u.fa));
  const R a0 = u.rect(u.fa), b0 = u.rect(u.fb);
  REQUIRE(b0.x - (a0.x + a0.w) == doctest::Approx(50.0f));
  CHECK(u.view().overlappingBlocks().empty());

  SUBCASE("a row block grown short of its neighbour: nothing else moves") {
    const auto was = u.all();
    const float k = 1.0f + 40.0f / a0.w;  // 40 wider: 10 short of b
    u.sResize(u.fa, k);
    CHECK(u.view().blockScale(u.fa) == doctest::Approx(k).epsilon(0.01));
    const R a1 = u.rect(u.fa);
    CHECK(a1.x == a0.x);  // its top-left kept
    CHECK(a1.y == a0.y);
    CHECK(a1.w == doctest::Approx(a0.w * k).epsilon(0.01));
    const std::vector<std::string> moved = u.changed(was, u.all(), {u.fa});
    CHECK_MESSAGE(moved.empty(), (moved.empty() ? std::string() : moved.front()));  // (the bands too: none grew)
  }
  SUBCASE("a block of the main path grown: the path after it and the mixer stay") {
    const auto was = u.all();
    const R g0 = u.rect(u.util);
    u.sResize(u.util, 1.25f);  // (its growth well short of the 40 to the next block)
    CHECK(u.view().blockScale(u.util) == doctest::Approx(1.25f));
    CHECK(u.rect(u.util).x == g0.x);
    const std::vector<std::string> moved = u.changed(was, u.all(), {u.util});
    CHECK_MESSAGE(moved.empty(), (moved.empty() ? std::string() : moved.front()));
  }
  SUBCASE("grown into its neighbour: that one pushed right to the gap, level; the rest stays, near pairs too") {
    const auto was = u.all();
    const float k = 1.0f + 58.0f / a0.w;  // 58 wider: 8 into b
    u.sResize(u.fa, k);
    const R a1 = u.rect(u.fa), b1 = u.rect(u.fb);
    CHECK(a1.x == a0.x);
    CHECK(a1.y == a0.y);
    CHECK(b1.x - (a1.x + a1.w) >= ProjectPatchView::kPushGap - 1.0f);
    CHECK(b1.x - (a1.x + a1.w) <= ProjectPatchView::kPushGap + 1.5f);
    CHECK(b1.y == b0.y);
    CHECK(u.view().overlappingBlocks().empty());
    const std::vector<std::string> moved = u.changed(was, u.all(), {u.fa, u.fb});
    CHECK_MESSAGE(moved.empty(), (moved.empty() ? std::string() : moved.front()));
    CHECK(u.rect(u.fe).x - (u.rect(u.fd).x + u.rect(u.fd).w) == doctest::Approx(4.0f));  // still 4 apart
  }
  SUBCASE("a resize past the band's bottom: the band grows by what it needs, the band below moves with it, by as much") {
    const uint32_t kb = u.pp().bandOf(u.keys), bb = u.pp().bandOf(u.bass);
    // (Its circuit's bottom: the band ends in its lane, under the circuit.)
    auto circuitBottom = [&] { return u.pp().bands[kb].y + u.pp().bands[kb].h - laneBlock(u.pp().bands[kb]); };
    const float bandBottom = circuitBottom(), bassY = u.pp().bands[bb].y;
    // c far right, its bottom at the circuit's (its pad under it): grown 2x, it reaches past by its height.
    const R c0 = u.rect(u.fc);
    u.place(u.fc, c0.x + 400.0f, bandBottom - metrics::kBandPad - c0.h);
    REQUIRE(circuitBottom() == doctest::Approx(bandBottom));
    u.look(1.0, u.centreOf(u.fc));
    const auto was = u.all();
    u.sResize(u.fc, 2.0f);
    CHECK(u.view().blockScale(u.fc) == doctest::Approx(2.0f));
    const R c1 = u.rect(u.fc);
    const float grew = circuitBottom() - bandBottom;
    INFO("c " << c0.x << "," << c0.y << " " << c0.w << "x" << c0.h << " -> " << c1.x << "," << c1.y << " " << c1.w << "x" << c1.h
              << "; circuit bottom " << bandBottom);
    CHECK(grew > 0.0f);
    CHECK(circuitBottom() >= c1.y + c1.h + metrics::kBandPad);
    CHECK(grew <= c1.y + c1.h + metrics::kBandPad - bandBottom + 4.0f);  // (whole 4s)
    CHECK(u.pp().bands[bb].y - bassY == doctest::Approx(grew));
    // Every other block where it was in its band (the Bass band's moved down with it, by as much).
    const std::vector<std::string> moved = u.changed(was, u.all(), {u.fc}, true);
    CHECK_MESSAGE(moved.empty(), (moved.empty() ? std::string() : moved.front()));
  }
}

TEST_CASE("bug-spawn-resize: a resize inside an opened capsule moves none of its other members") {
  Ui u;
  // The Keys instrument group opened: its members are a circuit; one of them grown a little.
  u.view().setGroupOpen(u.synth, true);
  u.view().sync();
  const uint32_t cap = u.node(u.synth);
  REQUIRE(u.pp().nodes[cap].groupOpen);
  doc::DeviceId member;
  for (const uint32_t k : u.pp().nodes[cap].members)
    if (!(u.pp().nodes[k].group && u.pp().nodes[k].groupOpen) && !u.pp().nodes[k].expanded && !member.valid()) member = u.pp().nodes[k].device;
  REQUIRE(member.valid());
  u.look(1.0, u.centreOf(member));
  const auto was = u.all();
  const R m0 = u.rect(member);
  u.sResize(member, 1.2f);
  const R m1 = u.rect(member);
  CHECK(m1.x == m0.x);
  CHECK(m1.y == m0.y);
  CHECK(m1.w == doctest::Approx(m0.w * 1.2f).epsilon(0.01));
  // The capsule's other contents stay (its frame may grow round the bigger block: only the capsule's own rect changes).
  const std::vector<std::string> moved = u.changed(was, u.all(), {member, u.synth});
  CHECK_MESSAGE(moved.empty(), (moved.empty() ? std::string() : moved.front()));
}

TEST_CASE("bug-spawn-resize: an edge stretch moves no block it does not come to overlap (the row after it stays)") {
  Ui u;
  u.look(1.0, u.centreOf(u.tap));
  const auto was = u.all();
  const R t0 = u.rect(u.tap);
  REQUIRE(u.rect(u.fa).x - (t0.x + t0.w) == doctest::Approx(metrics::kNodeGap));  // the row: a right after it
  // Its right edge 20 wider (short of a): the face widens, nothing else moves (before: the row after it shifted).
  const float s = u.canvas().deviceScale();
  const Vec2 edge = u.dev({t0.x + t0.w - 1.5f, t0.y + 12.0f});
  u.d.move(edge);
  u.d.step();
  REQUIRE(u.canvas().cursorAt(edge) == Cursor::ResizeEW);
  u.d.drag(edge, {edge.x + 20.0f * s, edge.y}, 6);
  u.d.step();
  const R t1 = u.rect(u.tap);
  CHECK(t1.w == doctest::Approx(t0.w + 20.0f).epsilon(0.02));
  CHECK(t1.x == t0.x);
  const std::vector<std::string> moved = u.changed(was, u.all(), {u.tap});
  CHECK_MESSAGE(moved.empty(), (moved.empty() ? std::string() : moved.front()));
  // Undo: back to its width; the others still where they are.
  REQUIRE(u.ctx.undo());
  CHECK(u.rect(u.tap).w == doctest::Approx(t0.w));
}

TEST_CASE("bug-spawn-resize: a spawn lands at the pointer at the same size on screen at zoom 0.5, 1, 2, 4 (3x at most: "
          "zoom 0.25); nothing else moves") {
  Ui u;
  // Room below the Keys band's blocks: the LFO placed far down (the band grows to hold it); the pointer right of
  // everything, high in the band.
  u.place(u.lfo, u.rect(u.lfo).x, u.rect(u.lfo).y + 700.0f);
  const Band& kb = u.pp().bands[u.pp().bandOf(u.keys)];
  const graph::Point2 at{kb.x1 + 600.0f, kb.y + 380.0f};
  float ref = 0.0f;  // its width on screen at zoom 1
  for (const double z : {1.0, 0.5, 2.0, 4.0, 0.25}) {
    INFO("zoom " << z);
    u.look(z, at);
    const auto was = u.all();
    const doc::DeviceId v = u.tabVca(at);
    REQUIRE(u.device(v) != nullptr);
    CHECK(u.device(v)->name == "VCA");
    const float k = std::min(1.0f / static_cast<float>(z), doc::kMaxPatchScale);
    CHECK(u.view().blockScale(v) == doctest::Approx(k).epsilon(0.01));
    const graph::Point2 c = u.centreOf(v);
    CHECK(std::fabs(c.x - at.x) <= 1.0f);
    CHECK(std::fabs(c.y - at.y) <= 1.0f);
    if (z == 1.0) ref = onScreenW(u, v);
    else if (z >= 1.0 / 3.0) CHECK(onScreenW(u, v) == doctest::Approx(ref).epsilon(0.015));
    else CHECK(onScreenW(u, v) == doctest::Approx(ref * doc::kMaxPatchScale * static_cast<float>(z)).epsilon(0.015));
    // Its panel drawn at its scale (the face follows the block).
    if (Face* f = u.canvas().panelOn(u.view().scene().sceneNode[u.node(v)])) {
      auto* pf = dynamic_cast<panel::GroupPanelFace*>(f);
      REQUIRE(pf != nullptr);
      CHECK(pf->worldSize().x * u.view().blockScale(v) == doctest::Approx(u.rect(v).w).epsilon(0.01));
    }
    const std::vector<std::string> moved = u.changed(was, u.all(), {v}, true);
    CHECK_MESSAGE(moved.empty(), (moved.empty() ? std::string() : moved.front()));
    REQUIRE(u.ctx.undo());
  }
}

TEST_CASE("bug-spawn-resize: a spawn onto blocks lands at the nearest free spot beside them, never far; nothing else "
          "moves — zoom 1 and 4") {
  Ui u;
  for (const double z : {1.0, 4.0}) {
    INFO("zoom " << z);
    const graph::Point2 at = u.centreOf(u.fb);
    u.look(z, at);
    const auto was = u.all();
    const doc::DeviceId v = u.tabVca(at);
    const R r = u.rect(v), rb = u.rect(u.fb);
    CHECK(u.view().overlappingBlocks().empty());
    // Beside b (its nearest free spot): a gap from it on one side, not a block's length further.
    const graph::Point2 c = u.centreOf(v);
    CHECK(std::fabs(c.x - at.x) <= (rb.w + r.w) * 0.5f + ProjectPatchView::kPushGap + 1.0f);
    CHECK(std::fabs(c.y - at.y) <= (rb.h + r.h) * 0.5f + ProjectPatchView::kPushGap + 1.0f);
    for (const doc::DeviceId o : {u.fa, u.fb, u.fc, u.fd, u.fe, u.lfo, u.util, u.sat, u.tap}) CHECK_FALSE(overlaps(r, u.rect(o)));
    const std::vector<std::string> moved = u.changed(was, u.all(), {v}, true);
    CHECK_MESSAGE(moved.empty(), (moved.empty() ? std::string() : moved.front()));
    REQUIRE(u.ctx.undo());
  }
}

TEST_CASE("bug-spawn-resize: every spawn path makes the block camera-sized at the pointer — a dropped cable, Tab and Z "
          "over a cable, Z over a port; a paste keeps the copied size") {
  Ui u;
  const double z = 2.0;
  const float k = 0.5f;  // 1 / zoom
  SUBCASE("a cable dropped on empty space (zoom 0.5: 2x)") {
    u.place(u.lfo, u.rect(u.lfo).x, u.rect(u.lfo).y + 400.0f);  // (room in the band below its title strip)
    const Band& kb = u.pp().bands[u.pp().bandOf(u.keys)];
    const graph::Point2 drop{kb.x1 + 300.0f, kb.y + 300.0f};
    u.look(0.5, {(u.centreOf(u.fe).x + drop.x) * 0.5f, (u.centreOf(u.fe).y + drop.y) * 0.5f});
    const uint32_t en = u.node(u.fe);
    const Vec2 from = u.dev(u.canvas().portWorld(en, 0, false));
    const size_t h = u.history();
    u.d.drag(from, u.dev(drop), 8);
    REQUIRE(u.menu() != nullptr);
    u.pick("vca", "VCA");
    REQUIRE(u.history() == h + 1);
    const doc::DeviceId v = u.newest();
    CHECK(u.view().blockScale(v) == doctest::Approx(2.0f));
    // It meets the cable where it was dropped: placed with its input there, then its orbiting input turns toward the
    // cable's source (TX.orbit-ports) — the block's edge stays at the drop point (within the port's ring and a gap).
    const R r = u.rect(v);
    CHECK(drop.x >= r.x - 12.0f);
    CHECK(drop.x <= r.x + r.w + 12.0f);
    CHECK(drop.y >= r.y - 12.0f);
    CHECK(drop.y <= r.y + r.h + 12.0f);
    CHECK(std::min(std::fabs(drop.x - r.x), std::fabs(drop.x - (r.x + r.w))) <= 12.0f);
  }
  SUBCASE("Tab over a cable: the block inserted at the pointer (before: the middle of the cable's ends)") {
    u.look(z, u.centreOf(u.util));
    const uint32_t cable = u.cableOf(u.lfoWire);
    const Vec2 p = u.onCable(cable);
    const graph::Point2 at = u.canvas().toWorld(p);
    const auto was = u.all();
    u.d.move(p);
    REQUIRE(u.d.press("Tab") == KeyResult::Widget);
    u.pick("saturator", "Saturator");
    const doc::DeviceId v = u.newest();
    CHECK(u.view().blockScale(v) == doctest::Approx(k));
    CHECK(u.atPointerOrBeside(v, at));
    CHECK(u.view().overlappingBlocks().empty());
    const std::vector<std::string> moved = u.changed(was, u.all(), {v}, true);
    CHECK_MESSAGE(moved.empty(), (moved.empty() ? std::string() : moved.front()));
  }
  SUBCASE("Z over a chain cable: the VCA on the path at the pointer; the path after it keeps its place") {
    u.look(z, u.centreOf(u.util));
    // The chain cable util → sat.
    uint32_t cable = kNone;
    for (uint32_t i = 0; i < u.pp().edges.size(); ++i)
      if (u.pp().edges[i].kind == EdgeKind::Chain && u.pp().nodes[u.pp().edges[i].from].device == u.util &&
          u.pp().nodes[u.pp().edges[i].to].device == u.sat)
        cable = i;
    REQUIRE(cable != kNone);
    const Vec2 p = u.onCable(cable);
    const auto was = u.all();
    u.d.move(p);
    u.d.keyDown('z');
    u.d.keyUp('z');
    u.d.step();
    const doc::DeviceId v = u.newest();
    REQUIRE(u.device(v) != nullptr);
    CHECK(u.device(v)->name == "VCA");
    CHECK(u.view().blockScale(v) == doctest::Approx(k));
    CHECK(u.view().overlappingBlocks().empty());
    // Every block that was there where it was (the mixer follows the band's end: not a block).
    const std::vector<std::string> moved = u.changed(was, u.all(), {v}, true);
    CHECK_MESSAGE(moved.empty(), (moved.empty() ? std::string() : moved.front()));
  }
  SUBCASE("Z over a port with a cable: the VCA camera-sized beside it") {
    u.look(z, u.centreOf(u.util));
    const uint32_t un = u.node(u.util);
    const int32_t g = u.pp().nodes[un].input("g");
    REQUIRE(g >= 0);
    const Vec2 p = u.dev(u.canvas().portWorld(un, static_cast<uint32_t>(g), true));
    u.d.move(p);
    u.d.keyDown('z');
    u.d.keyUp('z');
    u.d.step();
    const doc::DeviceId v = u.newest();
    REQUIRE(u.device(v) != nullptr);
    CHECK(u.device(v)->name == "VCA");
    CHECK(u.view().blockScale(v) == doctest::Approx(k));
    CHECK(u.view().overlappingBlocks().empty());
  }
  SUBCASE("Ctrl+C / Ctrl+V: the paste at the pointer keeps the copied block's size, whatever the zoom") {
    REQUIRE(u.view().setBlockScale(u.fa, 1.7f));
    u.view().sync();
    u.look(z, u.centreOf(u.fa));
    const uint32_t sn = u.view().scene().sceneNode[u.node(u.fa)];
    const uint32_t one[1] = {sn};
    u.canvas().select(one, {});
    u.d.tree().setFocus(&u.canvas());
    CHECK(u.d.press("Ctrl+C") != KeyResult::Unhandled);
    const Band& kb = u.pp().bands[u.pp().bandOf(u.keys)];
    u.d.move(u.dev({kb.x1 + 200.0f, kb.y + 80.0f}));
    const size_t h = u.history();
    CHECK(u.d.press("Ctrl+V") != KeyResult::Unhandled);
    u.d.step();
    REQUIRE(u.history() == h + 1);
    const doc::DeviceId v = u.newest();
    REQUIRE(v != u.fa);
    CHECK(u.view().blockScale(v) == doctest::Approx(1.7f));
  }
}
