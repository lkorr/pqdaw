// MM review (views): dynamic probes. Usage: probe_views <repo root> [A|B|C|D ...] (no letters: all)
//  A. Device view, a group's embedded circuit (scoped ProjectPatchView): a member selected inside it, Ctrl+Shift+G —
//     the group the panel shows is ungrouped from its own panel's key handler (the frame and its view go away while
//     ProjectCanvas::onKey is on the stack). Then Ctrl+G with nothing selected inside: swallowed?
//  B. Same panel: a drag in progress on the embedded canvas when the group goes (undo of the grouping elsewhere);
//     further moves and the release.
//  C. Project patch view: a knob drag on a panel face (Utility) when the device is deleted mid-drag; a chooser's menu
//     left open on a panel face whose device is deleted, then its item clicked.
//  D. S-resize: keyHeld after S down, after the tree's cancelInteraction (window focus lost), and S down while the main
//     canvas's focus is a panel face's text field.
//  E. Same panel as A: the group's capsule selected inside its own panel, Delete. With freed memory poisoned (0xDD,
//     never reused) while the key is handled, so a read of the destroyed view after the store dispatch faults.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/overlay.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/devices/panel_face.h"
#include "pq/views/fake_transport.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

// Freed memory poisoned and quarantined while gPoison is set (a cheap page-heap: a use after free reads 0xDD...).
static bool gPoison = false;
void* operator new(size_t n) {
  auto* b = static_cast<unsigned char*>(std::malloc(n + 16));
  if (b == nullptr) throw std::bad_alloc();
  *reinterpret_cast<size_t*>(b) = n;
  return b + 16;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
  if (p == nullptr) return;
  unsigned char* b = static_cast<unsigned char*>(p) - 16;
  if (gPoison) {
    std::memset(p, 0xDD, *reinterpret_cast<size_t*>(b));
    return;  // leaked: never reused while poisoning
  }
  std::free(b);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }

using namespace pq;
using namespace pq::views;
using namespace pq::ui;
using namespace pq::views::project_patch;
namespace fs = std::filesystem;

static std::unique_ptr<doc::DeviceLibrary> gLib;

static void scopedViews(Widget& w, std::vector<ProjectPatchView*>& out) {
  if (auto* v = dynamic_cast<ProjectPatchView*>(&w); v != nullptr && v->scope().valid()) out.push_back(v);
  for (size_t i = 0; i < w.childCount(); ++i) scopedViews(w.child(i), out);
}

struct GroupUi {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  GroupProjectIds ids;
  bool ok = false;
  GroupUi() {
    ctx.setDeviceLibrary(gLib.get());
    if (!buildGroupProject(ctx.store(), &ids).ok()) return;
    auto s = Shell::create(d.tree(), ctx);
    if (!s) return;
    shell = std::move(*s);
    d.tree().layoutNow();
    ok = d.enableGpu().ok();
  }
  ~GroupUi() { d.tree().setRoot(std::make_unique<Widget>()); }
  ProjectPatchView& main() const { return *findProjectPatch(*shell); }
  doc::DeviceId groupOf(doc::DeviceId m) const { return doc::findDevice(*ctx.doc(), m).device->group; }
  ProjectPatchView* embedded() {
    DeviceChainView* chain = findDeviceChain(*shell);
    if (chain == nullptr) return nullptr;
    std::vector<ProjectPatchView*> e;
    scopedViews(*chain, e);
    return e.empty() ? nullptr : e.front();
  }
  bool toDeviceView() {
    main().setShown(true, false);
    d.tree().layoutNow();
    const doc::DeviceId sel[3] = {ids.pa1, ids.wt, ids.pa2};
    if (!main().groupDevices(sel)) return false;
    main().setShown(false, false);
    ctx.setSelectedTrack(ids.keys);
    ctx.setDetailVisible(true);
    ctx.setDetailMode(DetailMode::Device);
    d.tree().layoutNow();
    d.advance(0.3);
    return embedded() != nullptr;
  }
};

static const char* kr(KeyResult r) {
  switch (r) {
    case KeyResult::Unhandled: return "Unhandled";
    case KeyResult::Widget: return "Widget";
    case KeyResult::TextSwallowed: return "TextSwallowed";
    case KeyResult::Focus: return "Focus";
    default: return "Action/Scope";
  }
}

static void probeA() {
  GroupUi u;
  if (!u.ok || !u.toDeviceView()) { std::printf("A: setup failed\n"); return; }
  ProjectPatchView* pv = u.embedded();
  const doc::DeviceId g = u.groupOf(u.ids.wt);
  // Ctrl+G with nothing selected inside the panel (focus in it): does the Device view's Ctrl+G still get it?
  u.d.click(pv->canvas().toDevice({pv->patch().nodes[0].x + 4.0f, pv->patch().nodes[0].y + pv->patch().nodes[0].h - 4.0f}));
  pv->canvas().clearSelection();
  u.d.tree().setFocus(&pv->canvas());
  const size_t h0 = u.ctx.store().history().cursor();
  const KeyResult rg = u.d.press("Ctrl+G");
  std::printf("A: Ctrl+G, embedded canvas focused, nothing selected: %s, history +%zu, status \"%s\"\n", kr(rg),
              u.ctx.store().history().cursor() - h0, std::string(u.ctx.status()).c_str());
  // A member selected inside, Ctrl+Shift+G: ungroups the group the panel itself shows.
  const ProjectPatch& pp = pv->patch();
  const uint32_t n = pp.nodeOf(u.ids.wt);
  if (n == kNone) { std::printf("A: wt not in the embedded patch\n"); return; }
  const uint32_t sn[1] = {pv->scene().sceneNode[n]};
  pv->canvas().select(sn, {});
  u.d.tree().setFocus(&pv->canvas());
  std::printf("A: before: group %s; embedded view %p\n", u.groupOf(u.ids.wt).valid() ? "set" : "none", static_cast<void*>(pv));
  std::fflush(stdout);
  const KeyResult r = u.d.press("Ctrl+Shift+G");
  std::printf("A: Ctrl+Shift+G inside its own panel: %s; member's group now %s (was %u); embedded view now %p\n", kr(r),
              u.groupOf(u.ids.wt).valid() ? "set" : "none", g.value, static_cast<void*>(u.embedded()));
  std::fflush(stdout);
  u.d.advance(0.5);
  std::printf("A: survived 0.5 s of frames after it; focus %p\n", static_cast<void*>(u.d.tree().focus()));
}

static void probeB() {
  GroupUi u;
  if (!u.ok || !u.toDeviceView()) { std::printf("B: setup failed\n"); return; }
  ProjectPatchView* pv = u.embedded();
  const ProjectPatch& pp = pv->patch();
  const Node& c = pp.nodes[0];
  const Node& in = pp.nodes[c.groupIn];
  const Vec2 a = pv->canvas().toDevice({in.x + in.w, in.y + pinOffset(in, 0)});
  u.d.move(a);
  u.d.down(a);
  u.d.move({a.x + 20.0f, a.y + 10.0f});
  u.d.move({a.x + 40.0f, a.y + 20.0f});
  std::printf("B: wire drag in progress (captured %p, embedded canvas %p)\n", static_cast<void*>(u.d.tree().captured()),
              static_cast<void*>(&pv->canvas()));
  std::fflush(stdout);
  const bool undone = u.ctx.undo();  // the grouping goes: the frame and its embedded view with it
  u.d.tree().layoutNow();
  std::printf("B: undo of the grouping mid-drag: %s; embedded view now %p; captured %p\n", undone ? "ok" : "failed",
              static_cast<void*>(u.embedded()), static_cast<void*>(u.d.tree().captured()));
  std::fflush(stdout);
  u.d.move({a.x + 60.0f, a.y + 30.0f});
  u.d.up({a.x + 60.0f, a.y + 30.0f});
  u.d.advance(0.5);
  std::printf("B: moves + release + 0.5 s of frames: survived; history cursor %zu\n", u.ctx.store().history().cursor());
}

static void probeE() {
  GroupUi u;
  if (!u.ok || !u.toDeviceView()) { std::printf("E: setup failed\n"); return; }
  ProjectPatchView* pv = u.embedded();
  // model node 0: the capsule (the group itself), clicked on its frame (as probe A: that selects the group)
  const Node& cap = pv->patch().nodes[0];
  u.d.click(pv->canvas().toDevice({cap.x + 4.0f, cap.y + cap.h - 4.0f}));
  u.d.tree().setFocus(&pv->canvas());
  const doc::DeviceId g = u.groupOf(u.ids.wt);
  std::printf("E: capsule selected in its own panel (selected devices: %zu); Delete with freed memory poisoned...\n",
              pv->selectedDevices().size());
  std::fflush(stdout);
  gPoison = true;
  const KeyResult r = u.d.keyDown(key::kDelete);
  gPoison = false;
  u.d.keyUp(key::kDelete);
  std::printf("E: Delete: %s; group %u %s; embedded view now %p\n", kr(r), g.value,
              doc::findDevice(*u.ctx.doc(), g).device ? "still there" : "deleted", static_cast<void*>(u.embedded()));
  std::fflush(stdout);
  u.d.advance(0.3);
  // A, poisoned: Ctrl+Shift+G from inside the panel (undo the delete first).
  (void)u.ctx.undo();
  u.d.tree().layoutNow();
  u.d.advance(0.3);
  pv = u.embedded();
  if (pv == nullptr) { std::printf("E: no panel after undo\n"); return; }
  const uint32_t n = pv->patch().nodeOf(u.ids.wt);
  const uint32_t sn2[1] = {pv->scene().sceneNode[n]};
  pv->canvas().select(sn2, {});
  u.d.tree().setFocus(&pv->canvas());
  gPoison = true;
  const KeyResult r2 = u.d.keyDown('g', static_cast<Mods>(mod::kCtrl | mod::kShift));
  gPoison = false;
  u.d.keyUp('g', static_cast<Mods>(mod::kCtrl | mod::kShift));
  std::printf("E: Ctrl+Shift+G (poisoned): %s; embedded view now %p\n", kr(r2), static_cast<void*>(u.embedded()));
}

struct SketchUi {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  bool ok = false;
  SketchUi() {
    ctx.setDeviceLibrary(gLib.get());
    if (!buildSketchProject(ctx.store()).ok()) return;
    auto s = Shell::create(d.tree(), ctx);
    if (!s) return;
    shell = std::move(*s);
    d.tree().layoutNow();
    ok = d.enableGpu().ok();
  }
  ~SketchUi() { d.tree().setRoot(std::make_unique<Widget>()); }
  ProjectPatchView& view() const { return *findProjectPatch(*shell); }
};

static Vec2 controlAt(SketchUi& u, doc::DeviceId dev, std::string_view id, bool& ok) {
  ok = false;
  devices::PanelFace* f = u.view().panelOf(dev);
  if (f == nullptr || f->layoutScale() <= 0.0f) return {};
  Widget* w = f->panel().control(id);
  if (w == nullptr || !w->visible()) return {};
  const IRect r = w->rect();
  const Node& n = u.view().patch().nodes[u.view().patch().nodeOf(dev)];
  const float k = n.scale / f->layoutScale();
  ok = true;
  return u.view().canvas().toDevice({n.x + (static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f) * k,
                                     n.y + (static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f) * k});
}

static doc::DeviceId utilityOf(const doc::Project& p) {
  doc::DeviceId out;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    if (t.name == "Group")
      for (const auto& dv : t.devices)
        if (dv->patch == "utility") out = dv->id;
  });
  return out;
}

static void frameOn(SketchUi& u, doc::DeviceId dev) {
  const Node& n = u.view().patch().nodes[u.view().patch().nodeOf(dev)];
  circuit::CircuitCanvas::View cv;
  cv.zoom = 1.2;
  cv.x = n.x - 120.0;
  cv.y = n.y - 80.0;
  u.view().canvas().setView(cv);
  u.d.advance(0.5);
}

static Menu* openMenu(SketchUi& u) {
  for (size_t i = 0; i < u.shell->overlay().childCount(); ++i)
    if (auto* m = dynamic_cast<Menu*>(&u.shell->overlay().child(i))) return m;
  return nullptr;
}

static void probeC() {
  {
    SketchUi u;
    if (!u.ok) { std::printf("C: setup failed\n"); return; }
    u.view().setShown(true, false);
    u.d.tree().layoutNow();
    const doc::DeviceId util = utilityOf(*u.ctx.doc());
    frameOn(u, util);
    bool ok = false;
    const Vec2 k = controlAt(u, util, "gain", ok);
    if (!ok) { std::printf("C: no gain control\n"); return; }
    u.d.move(k);
    u.d.down(k);
    u.d.move({k.x, k.y - 20.0f});
    const size_t h0 = u.ctx.store().history().cursor();
    auto e = doc::ops::deleteDevices(*u.ctx.doc(), std::vector<doc::DeviceId>{util});
    const bool del = e && u.ctx.store().dispatch(e->command).ok();
    std::printf("C1: Utility deleted mid knob-drag: %s\n", del ? "ok" : "failed");
    std::fflush(stdout);
    u.d.move({k.x, k.y - 40.0f});
    u.d.up({k.x, k.y - 40.0f});
    u.d.advance(0.5);
    std::printf("C1: moves + release + frames survived; history +%zu (1 = only the delete)\n", u.ctx.store().history().cursor() - h0);
  }
  {
    SketchUi u;
    if (!u.ok) return;
    u.view().setShown(true, false);
    u.d.tree().layoutNow();
    const doc::DeviceId util = utilityOf(*u.ctx.doc());
    frameOn(u, util);
    bool ok = false;
    const Vec2 c = controlAt(u, util, "chan", ok);
    if (!ok) { std::printf("C2: no chan control\n"); return; }
    u.d.click(c);
    Menu* m = openMenu(u);
    std::printf("C2: chooser menu open: %s\n", m ? "yes" : "no");
    if (m == nullptr) return;
    auto e = doc::ops::deleteDevices(*u.ctx.doc(), std::vector<doc::DeviceId>{util});
    const bool del = e && u.ctx.store().dispatch(e->command).ok();
    const size_t h0 = u.ctx.store().history().cursor();
    u.d.step();  // the paint that destroys retired faces
    Menu* m2 = openMenu(u);
    std::printf("C2: Utility deleted with its menu open: %s; menu still open after a frame: %s\n", del ? "ok" : "failed", m2 ? "YES" : "no");
    std::fflush(stdout);
    if (m2 != nullptr) {
      const IRect r = m2->rect();
      u.d.click({static_cast<float>(r.x) + 20.0f, static_cast<float>(r.y) + static_cast<float>(r.h) - 8.0f});
      u.d.advance(0.3);
      std::printf("C2: clicked an item of the orphan menu: survived; history +%zu\n", u.ctx.store().history().cursor() - h0);
    }
  }
}

static void probeD() {
  SketchUi u;
  if (!u.ok) return;
  u.view().setShown(true, false);
  u.d.tree().layoutNow();
  auto& cv = static_cast<ProjectCanvas&>(u.view().canvas());
  u.d.tree().setFocus(&cv);
  u.d.keyDown('s');
  std::printf("D: S down: resizeHeld %d\n", cv.resizeHeld());
  u.shell->cancelInteraction();
  std::printf("D: after cancelInteraction (focus lost): resizeHeld %d\n", cv.resizeHeld());
  u.d.keyUp('s');
  u.d.keyDown('s');
  u.d.keyDown('s', mod::kNone, true);
  u.d.keyUp('s');
  std::printf("D: S down, repeat, up: resizeHeld %d\n", cv.resizeHeld());
  // 9 keys held: the 9th and later are not tracked; S released after them is.
  for (char c = 'a'; c < 'a' + 8; ++c) u.d.keyDown(static_cast<Key>(c));
  u.d.keyDown('s');
  std::printf("D: S down with 8 other keys held: resizeHeld %d\n", cv.resizeHeld());
  for (char c = 'a'; c < 'a' + 8; ++c) u.d.keyUp(static_cast<Key>(c));
  u.d.keyUp('s');
}

int main(int argc, char** argv) {
  if (argc < 2) { std::printf("usage: probe_views <repo root> [A B C D]\n"); return 2; }
  const fs::path root = argv[1];
  auto lib = doc::loadDeviceLibrary((root / "stdlib").string());
  if (!lib) { std::printf("no device library\n"); return 2; }
  gLib = std::move(*lib);
  auto want = [&](const char* k) {
    if (argc < 3) return true;
    for (int i = 2; i < argc; ++i)
      if (std::strcmp(argv[i], k) == 0) return true;
    return false;
  };
  if (want("A")) probeA();
  if (want("B")) probeB();
  if (want("C")) probeC();
  if (want("D")) probeD();
  if (want("E")) probeE();
  std::printf("probes done\n");
  return 0;
}
