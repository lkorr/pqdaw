// FZ.3 (docs/plans/RB.md FZ.3): the freeze UI through real window dispatch over the mixer fixture — the track menu's
// Freeze Track / Unfreeze Track (one undo step each; a group's freezes its members; freezing disarms; the arm button is
// disabled on a frozen track), a drag on a frozen track's clip does nothing and the status line explains with an
// Unfreeze button, the header's rendering progress / failure read from the freeze source only when its generation
// moves; the Device view's veil over a frozen track (a click explains, a kept block is marked running); goldens
// tracks-frozen and device-frozen.
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/graph/types.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "pq/views/view_scenes.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr const char* kArr = "main/body/top/track-area/arrangement";

// A scripted freeze source: statuses by track, a generation the test moves, and how often the context read them.
struct FakeFreeze {
  std::vector<std::pair<doc::TrackId, FreezeView>> status;
  uint64_t gen = 1;
  int reads = 0;
  void set(doc::TrackId t, FreezeView::State s, float progress = 0.0f, std::string error = {}) {
    FreezeView v;
    v.state = s;
    v.progress = progress;
    v.error = std::move(error);
    for (auto& [id, old] : status)
      if (id == t) {
        old = v;
        ++gen;
        return;
      }
    status.emplace_back(t, v);
    ++gen;
  }
  FreezeSource source() {
    return {[this] { return gen; },
            [this](doc::TrackId t) {
              ++reads;
              for (const auto& [id, v] : status)
                if (id == t) return v;
              return FreezeView{};
            },
            [](doc::TrackId) { return graph::kNone; }};
  }
};

struct Fx {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;

  Fx() {
    REQUIRE(ctx.openProject(views::test::fixtureDir("mixer")).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    arr = dynamic_cast<ArrangementView*>(d.find(kArr));
    REQUIRE(arr != nullptr);
    d.tree().layoutNow();
  }
  ~Fx() { d.tree().setRoot(std::make_unique<Widget>()); }

  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Track& track(std::string_view name) const {
    const doc::Track* found = nullptr;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (!found && t.name == name) found = &t;
    });
    REQUIRE_MESSAGE(found != nullptr, name);
    return *found;
  }
  TrackHeader& header(std::string_view name) {
    TrackHeader* h = arr->header(track(name).id);
    REQUIRE(h != nullptr);
    return *h;
  }
  Vec2 title(std::string_view name) {
    d.tree().layoutNow();
    const IRect t = header(name).titleRect();
    return {static_cast<float>(t.x + t.w / 3), static_cast<float>(t.y + t.h / 2)};
  }
  OverlayLayer& layer() {
    OverlayLayer* l = OverlayLayer::of(*arr);
    REQUIRE(l != nullptr);
    return *l;
  }
  // Right-click the track's title and pick `item` from the track menu.
  void menu(std::string_view name, std::string_view item) {
    d.click(title(name), mod::kNone, PointerButton::Right);
    d.step();
    REQUIRE(layer().openCount() >= 1);
    views::test::clickMenuItem(d, layer(), item);
  }
  Control* arm(std::string_view name) {
    Widget& sec = header(name).mixerSection();
    REQUIRE(sec.childCount() == 1);
    return dynamic_cast<Control*>(sec.child(0).find("arm"));
  }
  Button* statusAction() { return dynamic_cast<Button*>(d.find("main/status-bar/status-action")); }
};

}  // namespace

TEST_CASE("freeze UI: the track menu freezes (one step, disarmed, arm disabled) and unfreezes the track") {
  Fx f;
  REQUIRE(f.track("Bass").mixer.arm);
  Control* arm = f.arm("Bass");
  REQUIRE(arm != nullptr);
  CHECK(arm->enabled());
  const size_t h = f.history();
  f.menu("Bass", "Freeze Track");
  CHECK(f.track("Bass").frozen);
  CHECK_FALSE(f.track("Bass").mixer.arm);  // disarmed in the same step: a frozen track records nothing
  CHECK(f.history() == h + 1);
  CHECK(f.ctx.store().history().entries()[h].label == "Freeze Track");
  CHECK(f.header("Bass").frozen());
  CHECK_FALSE(f.header("Bass").freezeBadgeRect().empty());
  CHECK_FALSE(f.arm("Bass")->enabled());
  // A click on the disabled arm does nothing.
  {
    const IRect r = f.arm("Bass")->rect();
    f.d.click({static_cast<float>(r.x + r.w / 2), static_cast<float>(r.y + r.h / 2)});
    CHECK_FALSE(f.track("Bass").mixer.arm);
    CHECK(f.history() == h + 1);
  }
  // The menu now offers Unfreeze Track (Flatten is FZ.4's: shown, disabled).
  f.d.click(f.title("Bass"), mod::kNone, PointerButton::Right);
  f.d.step();
  auto* m = dynamic_cast<Menu*>(f.layer().top());
  REQUIRE(m != nullptr);
  bool flatten = false, freeze = false;
  for (size_t i = 0; i < m->items().size(); ++i) {
    if (m->items()[i].label == "Flatten") flatten = !m->selectable(static_cast<int32_t>(i));
    if (m->items()[i].label == "Freeze Track") freeze = true;
  }
  CHECK(flatten);
  CHECK_FALSE(freeze);
  views::test::clickMenuItem(f.d, f.layer(), "Unfreeze Track");
  CHECK_FALSE(f.track("Bass").frozen);
  CHECK(f.history() == h + 2);
  CHECK(f.arm("Bass")->enabled());
  CHECK_FALSE(f.header("Bass").frozen());
  // Undo: frozen again (and Ctrl+Z once more: armed and unfrozen).
  CHECK(f.d.press("Ctrl+Z") == KeyResult::Action);
  CHECK(f.track("Bass").frozen);
  CHECK(f.d.press("Ctrl+Z") == KeyResult::Action);
  CHECK_FALSE(f.track("Bass").frozen);
  CHECK(f.track("Bass").mixer.arm);
}

TEST_CASE("freeze UI: a group's menu freezes its MIDI members (one step); returns cannot be frozen") {
  Fx f;
  const size_t h = f.history();
  f.menu("Keys", "Freeze Track");
  CHECK(f.track("Piano").frozen);
  CHECK(f.track("Pad").frozen);
  CHECK_FALSE(f.track("Keys").frozen);  // a group is a container: its members are frozen
  CHECK_FALSE(f.track("Drums").frozen);
  CHECK(f.history() == h + 1);
  CHECK(f.header("Piano").frozen());
  // The group's menu now unfreezes them (every member frozen).
  f.menu("Keys", "Unfreeze Track");
  CHECK_FALSE(f.track("Piano").frozen);
  CHECK_FALSE(f.track("Pad").frozen);
  // A return's menu offers Freeze Track disabled.
  f.d.click(f.title("A Reverb"), mod::kNone, PointerButton::Right);
  f.d.step();
  auto* m = dynamic_cast<Menu*>(f.layer().top());
  REQUIRE(m != nullptr);
  bool found = false;
  for (size_t i = 0; i < m->items().size(); ++i)
    if (m->items()[i].label == "Freeze Track") {
      found = true;
      CHECK_FALSE(m->selectable(static_cast<int32_t>(i)));
    }
  CHECK(found);
}

TEST_CASE("freeze UI: a drag on a frozen clip does nothing; the status line explains and its Unfreeze unfreezes") {
  Fx f;
  const doc::Track& drums = f.track("Drums");
  REQUIRE_FALSE(drums.clips.empty());
  const doc::ClipRef beat{drums.id, drums.clips[0].id};
  const Ticks start0 = drums.clips[0].start();
  f.menu("Drums", "Freeze Track");
  REQUIRE(f.track("Drums").frozen);
  const size_t h = f.history();
  f.d.tree().layoutNow();
  const IRect box = f.arr->clipRect(beat);
  REQUIRE_FALSE(box.empty());
  const Vec2 grab{static_cast<float>(box.x) + 20.0f, static_cast<float>(box.y) + 6.0f};  // the title strip: a move
  f.d.drag(grab, {grab.x + 120.0f, grab.y}, 6);
  CHECK(f.history() == h);
  CHECK(f.track("Drums").clips[0].start() == start0);
  CHECK(f.ctx.status() == doc::kFrozenRefusal);
  CHECK(f.arr->clipSelected(beat.clip));  // the press still selects (view state)
  // The status line offers Unfreeze: a click unfreezes the track (one step); then the same drag moves the clip.
  f.d.step();
  f.d.tree().layoutNow();
  Button* act = f.statusAction();
  REQUIRE(act != nullptr);
  REQUIRE(act->visible());
  CHECK(act->text() == "Unfreeze");
  const IRect ar = act->rect();
  f.d.click({static_cast<float>(ar.x + ar.w / 2), static_cast<float>(ar.y + ar.h / 2)});
  CHECK_FALSE(f.track("Drums").frozen);
  CHECK(f.history() == h + 1);
  f.d.step();
  f.d.tree().layoutNow();
  CHECK_FALSE(f.statusAction()->visible());
  f.d.drag(grab, {grab.x + 120.0f, grab.y}, 6);
  CHECK(f.track("Drums").clips[0].start() != start0);
  // A refused key edit (Delete on the frozen clip) names the selected frozen track and offers Unfreeze too.
  REQUIRE(f.ctx.store().undo());
  REQUIRE(f.ctx.store().dispatch(doc::SetTrackFrozen{drums.id, true}).ok());
  f.arr->selectClips(std::vector<doc::ClipRef>{beat}, false);
  f.ctx.setSelectedTrack(drums.id);
  const size_t h2 = f.history();
  (void)f.arr->deleteSelection();
  CHECK(f.history() == h2);
  CHECK(f.ctx.status().find(doc::kFrozenRefusal) != std::string::npos);
  CHECK(static_cast<bool>(f.ctx.statusAction()));
}

TEST_CASE("freeze UI: the header shows rendering progress and a failure from the source, read only when its generation moves") {
  Fx f;
  FakeFreeze src;
  f.ctx.setFreezeSource(src.source());
  const doc::TrackId bass = f.track("Bass").id;
  f.menu("Bass", "Freeze Track");
  src.set(bass, FreezeView::State::Rendering, 0.25f);
  f.d.advance(0.3);  // the arrangement polls at 10 Hz while a render is not final
  CHECK(f.header("Bass").freezeShown().state == FreezeView::State::Rendering);
  CHECK(f.header("Bass").freezeShown().progress == doctest::Approx(0.25f));
  CHECK(std::string(f.header("Bass").tooltip()).find("25 %") != std::string::npos);
  // Nothing moves: the source is not read again (one atomic read of the generation per poll).
  const int reads = src.reads;
  f.d.advance(0.5);
  CHECK(src.reads == reads);
  src.set(bass, FreezeView::State::Rendering, 0.75f);
  f.d.advance(0.2);
  CHECK(f.header("Bass").freezeShown().progress == doctest::Approx(0.75f));
  src.set(bass, FreezeView::State::Failed, 0.0f, "disk full");
  f.d.advance(0.2);
  CHECK(f.header("Bass").freezeShown().state == FreezeView::State::Failed);
  CHECK(std::string(f.header("Bass").tooltip()) == "Freeze failed: disk full (the track plays live)");
  // Final: the polling stops (no timer, no reads).
  CHECK_FALSE(f.ctx.freezeBusy());
  const int r2 = src.reads;
  src.gen += 0;  // unchanged
  f.d.advance(1.0);
  CHECK(src.reads == r2);
  // The link reports a result itself (ProjectLink calls pollFreeze): Ready shows at once.
  src.set(bass, FreezeView::State::Ready, 1.0f);
  CHECK(f.ctx.pollFreeze());
  CHECK(f.header("Bass").freezeShown().state == FreezeView::State::Ready);
  CHECK(std::string(f.header("Bass").tooltip()).find("plays its render") != std::string::npos);
  // No frozen track: no slot function (the cable views lower as unfrozen).
  CHECK(static_cast<bool>(f.ctx.frozenSlotOf()));
  REQUIRE(f.ctx.store().dispatch(doc::SetTrackFrozen{bass, false}).ok());
  (void)f.ctx.pollFreeze();
  CHECK_FALSE(static_cast<bool>(f.ctx.frozenSlotOf()));
  CHECK(f.header("Bass").freezeShown().state == FreezeView::State::None);
}

namespace {

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

void checkGolden(const char* scene, const char* fixture, const doc::DeviceLibrary* lib = nullptr) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, fixture != nullptr ? views::test::fixtureDir(fixture) : std::string(), w, h, {}, o, lib);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(scene) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &d));
  MESSAGE(std::string(scene) << ": " << pq::ui::test::describe(d));
}

}  // namespace

TEST_CASE("freeze UI: the Device view veils a frozen track's blocks; a click explains; a kept block is marked running") {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&library());
  TestDriver d{2200, 800};
  auto built = buildViewScene("device-frozen", d.tree(), ctx);
  REQUIRE_MESSAGE(built.ok(), (built.ok() ? "" : built.error().message));
  std::unique_ptr<Shell> shell = std::move(*built);
  d.tree().layoutNow();
  DeviceChainView* v = findDeviceChain(*shell);
  REQUIRE(v != nullptr);
  const doc::Track* keys = nullptr;
  doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (t.name == "Keys") keys = &t;
  });
  REQUIRE(keys != nullptr);
  REQUIRE(keys->frozen);
  REQUIRE(v->track() == keys->id);
  // The kept blocks: the Saturator (cabled into Mod) and the instrument feeding it; the Utility after it is not.
  const doc::FrozenKeep keep = doc::frozenKeep(*ctx.doc(), *keys);
  const std::vector<doc::DeviceId> order = v->shownOrder();
  REQUIRE(order.size() >= 3);
  const doc::DeviceId util = order.back();
  CHECK(std::find(keep.blocks.begin(), keep.blocks.end(), order[order.size() - 2]) != keep.blocks.end());
  CHECK(std::find(keep.blocks.begin(), keep.blocks.end(), util) == keep.blocks.end());
  Widget* veil = v->find("scroll/chain/frozen-veil");
  REQUIRE(veil != nullptr);
  CHECK(veil->visible());
  // A click on the Utility's frame reaches the veil, not the panel: nothing changes, the status line explains.
  const size_t h = ctx.store().history().cursor();
  Widget* uf = v->frameOf(util);
  REQUIRE(uf != nullptr);
  const IRect ur = uf->rect();
  REQUIRE(ur.right() <= v->chainViewport().right());  // in view
  d.click({static_cast<float>(ur.x + ur.w / 2), static_cast<float>(ur.y + ur.h / 2)});
  CHECK(ctx.status() == doc::kFrozenRefusal);
  d.press("Delete");  // the Device view's Delete: refused by the apply rule (nothing selected to delete, or refused)
  CHECK(ctx.store().history().cursor() == h);
  CHECK(ctx.status().find(doc::kFrozenRefusal) != std::string::npos);
  REQUIRE(static_cast<bool>(ctx.statusAction()));
  ctx.runStatusAction();  // Unfreeze: the veil goes, the blocks are editable again
  CHECK_FALSE(doc::findTrackPtr(*ctx.doc(), keys->id)->frozen);
  d.step();
  d.tree().layoutNow();
  CHECK(v->find("scroll/chain/frozen-veil") == nullptr);
  d.tree().setRoot(std::make_unique<Widget>());
}

TEST_CASE("render: tracks-frozen over tests/views/fixtures/mixer matches the golden image") { checkGolden("tracks-frozen", "mixer"); }

TEST_CASE("render: device-frozen matches the golden image") { checkGolden("device-frozen", nullptr, &library()); }
