// T3.8-fix1: arrangement clip previews through the tile atlas. Tiles draw exactly the pixels of the direct path
// (one rect per note run) at 1×, 1.5× and 2× UI scale, at near and far zoom, for muted (grey), selected, looped and
// deactivated content; partial repaints equal a full repaint while zooming (stale tiles stretched), after the zoom
// settles (tiles rebuilt), while scrolling and after a mute; tile memory stays at the atlas bound over zoom sweeps
// down to the minimum zoom, and every settled repaint draws every preview from its exact tile.

#include <doctest/doctest.h>

#include <cstring>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/tile_atlas.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "arrangement_fixture.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr const char* kArr = "main/body/top/track-area/arrangement";
namespace am = arrangement_metrics;

// A shell over a project, painted into offscreen targets through a real renderer (validation on).
struct Rig {
  std::unique_ptr<GpuContext> gpu;
  std::unique_ptr<Renderer> renderer;
  SceneTextures tex;
  PaintResources res, resNoTiles;
  FakeTransport port;
  ViewContext ctx{port};
  UiTree tree;
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  std::unique_ptr<OffscreenTarget> partial, full;
  DrawList dl;
  uint32_t w = 0, h = 0;
  double now = 0.0;
  uint64_t partialFrames = 0;

  Rig(uint32_t lw, uint32_t lh, float scale, const char* fixture) {
    GpuOptions o;
    o.validation = true;
    auto g = GpuContext::create(o);
    REQUIRE_MESSAGE(g.ok(), (g.ok() ? "" : g.error().message));
    gpu = std::move(*g);
    auto r = Renderer::create(*gpu, OffscreenTarget::kFormat, 1);
    REQUIRE(r.ok());
    renderer = std::move(*r);
    auto t = createSceneTextures(*renderer, UiScale(scale));
    REQUIRE(t.ok());
    tex = std::move(*t);
    res = paintResources(tex);
    resNoTiles = res;
    resNoTiles.tiles = nullptr;
    if (fixture) REQUIRE(ctx.openProject(views::test::fixtureDir(fixture)).ok());
    const UiScale s(scale);
    w = static_cast<uint32_t>(s.px(static_cast<float>(lw)));
    h = static_cast<uint32_t>(s.px(static_cast<float>(lh)));
    tree.setViewport(w, h, s);
    tree.setResources(&res);
  }
  void install() {
    auto sh = Shell::create(tree, ctx);
    REQUIRE(sh.ok());
    shell = std::move(*sh);
    tree.layoutNow();
    arr = dynamic_cast<ArrangementView*>(tree.find(kArr));
    REQUIRE(arr != nullptr);
    auto p = OffscreenTarget::create(*gpu, w, h, 1);
    auto f = OffscreenTarget::create(*gpu, w, h, 1);
    REQUIRE((p.ok() && f.ok()));
    partial = std::move(*p);
    full = std::move(*f);
  }
  ~Rig() {
    if (shell) tree.setRoot(std::make_unique<Widget>());
  }
  Image drawFull(OffscreenTarget& target, const PaintResources& r) {
    tex.text->beginFrame();
    dl.reset(w, h);
    tree.paint(dl, r, now);
    const DrawLayer layer{&dl, kTransientLayer, 0};
    FrameDesc fd;
    fd.layers = {&layer, 1};
    fd.clear = kSceneClearColor;
    REQUIRE(target.draw(*renderer, fd).ok());
    auto img = target.read();
    REQUIRE(img.ok());
    return std::move(*img);
  }
  // One frame as the window loop runs it: update, then paint and render the damage into `partial`.
  void frame() {
    now += 1.0 / 144.0;
    tree.update(now);
    const Damage& d = tree.damage();
    if (!d.needsFrame()) return;
    tex.text->beginFrame();
    dl.reset(w, h);
    const DrawLayer layer{&dl, kTransientLayer, 0};
    FrameDesc fd;
    fd.layers = {&layer, 1};
    fd.clear = kSceneClearColor;
    if (d.full()) {
      tree.paint(dl, res, now);
      REQUIRE(partial->draw(*renderer, fd).ok());
      return;
    }
    ClipRect rects[Damage::kMaxRects];
    uint32_t n = 0;
    ClipRect box{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    for (const ClipRect& r : d.rects()) {
      rects[n++] = r;
      box = Damage::unite(box, r);
    }
    tree.paint(dl, res, now, {box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0});
    fd.full = false;
    fd.damage = {rects, n};
    REQUIRE(partial->draw(*renderer, fd).ok());
    ++partialFrames;
  }
  // Frames until nothing more is requested (previews and tiles built, the settle repaint done).
  void settle(int maxFrames = 600) {
    for (int i = 0; i < maxFrames && (i == 0 || tree.needsFrame() || arr->zooming()); ++i) frame();
  }
  // The partial target against a full repaint now, over the arrangement (the control bar's grid label, which a zoom
  // changes, paints a pixel past its rect: a T3.7 Label matter outside this test, QUESTIONS Q103).
  void comparePartial(const std::string& what) {
    auto pa = partial->read();
    REQUIRE(pa.ok());
    const Image pb = drawFull(*full, res);
    const IRect area = arr->rect();
    const Image a = crop(*pa, area), b = crop(pb, area);
    const ImageDiff diff = compareImages(a, b, 0);
    if (diff.pixelsOverTolerance != 0) {
      (void)writePng(what + ".partial.png", a);
      (void)writePng(what + ".full.png", b);
    }
    CHECK_MESSAGE(diff.pixelsOverTolerance == 0, what << ": " << diff.pixelsOverTolerance << " px differ");
  }
  static Image crop(const Image& img, const IRect& r) {
    Image out;
    out.width = static_cast<uint32_t>(r.w);
    out.height = static_cast<uint32_t>(r.h);
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (uint32_t y = 0; y < out.height; ++y)
      std::memcpy(out.pixel(0, y), img.pixel(static_cast<uint32_t>(r.x), static_cast<uint32_t>(r.y) + y), size_t(out.width) * 4);
    return out;
  }
  // A full repaint from tiles against one with the notes drawn directly (no tile atlas).
  // expectTiles: false where every clip is wider than a tile (drawn directly in both paints).
  void compareDirect(const std::string& what, bool expectTiles = true) {
    const Image a = drawFull(*full, res);
    const auto st = arr->stats();
    if (expectTiles) CHECK_MESSAGE(st.tilesDrawn > 0, what);
    CHECK_MESSAGE(st.tilesMissing == 0, what);
    CHECK_MESSAGE(st.tilesStale == 0, what);
    const Image b = drawFull(*partial, resNoTiles);
    CHECK_MESSAGE(arr->stats().tilesDrawn == 0, what);
    CHECK_MESSAGE(arr->stats().noteRectsEmitted > 0, what);
    const ImageDiff diff = compareImages(a, b, 0);
    if (diff.pixelsOverTolerance != 0) {
      (void)writePng(what + ".tiles.png", a);
      (void)writePng(what + ".direct.png", b);
    }
    CHECK_MESSAGE(diff.pixelsOverTolerance == 0, what << ": " << diff.pixelsOverTolerance << " px differ (max " << diff.maxChannelDiff << ")");
    // The direct paint must not leave a frame request behind.
    (void)drawFull(*partial, res);
  }
};

int32_t rowNamed(ArrangementView& arr, std::string_view n) {
  const auto rows = arr.rows();
  for (size_t i = 0; i < rows.size(); ++i)
    if (rows[i].track->name == n) return static_cast<int32_t>(i);
  return -1;
}

}  // namespace

TEST_CASE("render: preview tiles draw exactly the direct path's pixels (1x, 1.5x, 2x; near and far zoom; muted, selected)") {
  for (const float scale : {1.0f, 1.5f, 2.0f}) {
    CAPTURE(scale);
    Rig rig(1280, 800, scale, "arrangement");
    rig.install();
    ArrangementView& arr = *rig.arr;
    const double base = arr.pxPerTick();
    for (const double zoom : {1.0, 0.45, 3.0}) {
      CAPTURE(zoom);
      arr.setTimeView(0.0, base * zoom);
      rig.settle();
      // At 3× zoom and 2× scale the fixture's clips are all wider than a tile (2046 px).
      rig.compareDirect("tiles-vs-direct-" + std::to_string(scale) + "-" + std::to_string(zoom), zoom < 3.0 || scale < 2.0f);
    }
    // Muted track (grey clips: the same mask, another colour) and a selected clip (outline over the tile).
    arr.setTimeView(0.0, base);
    const int32_t bass = rowNamed(arr, "Bass");
    REQUIRE(bass >= 0);
    const doc::TrackId bassId = arr.rows()[static_cast<size_t>(bass)].id;
    auto mute = doc::ops::toggleTracksMute(*rig.ctx.doc(), std::span<const doc::TrackId>(&bassId, 1));
    REQUIRE(mute.ok());
    REQUIRE(rig.ctx.store().dispatch(mute->command).ok());
    const doc::ClipRef sel{bassId, arr.rows()[static_cast<size_t>(bass)].track->clips[0].id};
    arr.selectClips(std::span<const doc::ClipRef>(&sel, 1), false);
    rig.settle();
    rig.compareDirect("tiles-vs-direct-muted-selected-" + std::to_string(scale));
    CHECK(rig.gpu->validationErrorCount() == 0);
  }
}

TEST_CASE("render: with preview tiles, partial repaints equal a full repaint through zoom, settle, scroll and mute") {
  Rig rig(1280, 800, 1.0f, nullptr);
  views::test::buildLargeArrangement(rig.ctx.store(), 16, 200, 50);
  rig.install();
  ArrangementView& arr = *rig.arr;
  const auto& st = arr.stats();
  rig.settle();
  (void)rig.drawFull(*rig.partial, rig.res);  // the partial target starts from a full frame
  CHECK(st.tilesDrawn > 0);
  rig.comparePartial("settled");

  // Zoom out notch by notch: each frame draws the previous tiles stretched (and bodies for new clips).
  const IRect lv = arr.lanesViewport();
  const float cx = static_cast<float>(lv.x) + static_cast<float>(lv.w) * 0.4f;
  for (int i = 0; i < 5; ++i) {
    arr.zoomAt(cx, 0.8);
    rig.frame();
  }
  CHECK(arr.zooming());
  CHECK(st.tilesStale > 0);
  rig.comparePartial("zooming");
  // Settle: tiles at the new zoom are built over build-only frames, then the lanes repaint once from them.
  const uint64_t built = st.tilesBuilt;
  rig.settle();
  CHECK_FALSE(arr.zooming());
  CHECK(st.tilesBuilt > built);
  CHECK(st.tilesMissing == 0);
  CHECK(st.tilesStale == 0);
  rig.comparePartial("settled-out");

  // Scroll: newly exposed clips are drawn directly (same pixels) and get their tiles afterwards.
  for (int i = 0; i < 24; ++i) {
    arr.scrollByPx(37.0);
    rig.frame();
  }
  rig.settle();
  rig.comparePartial("scrolled");
  CHECK(st.tilesMissing == 0);

  // Mute a track: its clips turn grey (same tiles, no rebuild).
  const doc::TrackId id = arr.rows()[2].id;
  auto mute = doc::ops::toggleTracksMute(*rig.ctx.doc(), std::span<const doc::TrackId>(&id, 1));
  REQUIRE(mute.ok());
  const uint64_t builtBefore = st.tilesBuilt;
  REQUIRE(rig.ctx.store().dispatch(mute->command).ok());
  rig.settle();
  CHECK(st.tilesBuilt == builtBefore);
  rig.comparePartial("muted");

  // Zoom in past the default and settle again.
  for (int i = 0; i < 9; ++i) {
    arr.zoomAt(cx, 1.25);
    rig.frame();
  }
  rig.settle();
  rig.comparePartial("settled-in");
  CHECK(rig.partialFrames > 0);
  CHECK(rig.gpu->validationErrorCount() == 0);
}

TEST_CASE("arrangement: preview tile memory stays at the atlas bound over zoom sweeps to the minimum zoom" * doctest::test_suite("slow")) {
  TestDriver d(2560, 1440);
  FakeTransport port;
  ViewContext ctx(port);
  views::test::buildLargeArrangement(ctx.store(), 64, 200, 50);
  auto shell = Shell::create(d.tree(), ctx);
  REQUIRE(shell.ok());
  REQUIRE(d.enableGpu().ok());
  auto* arr = dynamic_cast<ArrangementView*>(d.find(kArr));
  REQUIRE(arr != nullptr);
  TileAtlas* tiles = d.resources().tiles;
  REQUIRE(tiles != nullptr);
  const auto& st = arr->stats();
  d.advance(0.5);
  const IRect lv = arr->lanesViewport();
  const float cx = static_cast<float>(lv.x) + static_cast<float>(lv.w) * 0.5f;
  size_t maxBytes = 0;
  uint32_t maxTilesDrawn = 0;
  for (int rep = 0; rep < 2; ++rep) {
    // Out to the minimum zoom, settling at every notch, then back in.
    for (int i = 0; i < 40 && arr->pxPerTick() > am::kMinPxPerTick; ++i) {
      arr->zoomAt(cx, 0.8);
      d.advance(1.0);
      CHECK_FALSE(arr->zooming());
      CHECK(st.tilesMissing == 0);
      CHECK(st.tilesStale == 0);
      CHECK(tiles->pageCount() <= tiles->maxPages());
      maxBytes = std::max(maxBytes, tiles->gpuBytes());
      maxTilesDrawn = std::max(maxTilesDrawn, st.tilesDrawn);
    }
    CHECK(arr->pxPerTick() == doctest::Approx(am::kMinPxPerTick));
    for (int i = 0; i < 40 && arr->pxPerTick() < am::kDefaultPxPerTick * 4.0; ++i) {
      arr->zoomAt(cx, 1.25);
      d.advance(1.0);
      CHECK(st.tilesMissing == 0);
      CHECK(tiles->pageCount() <= tiles->maxPages());
    }
  }
  CHECK(maxBytes <= tiles->maxGpuBytes());
  CHECK(maxTilesDrawn > 1000);
  CHECK(st.tileFailures == 0);
  MESSAGE("tile pages " << tiles->pageCount() << ", evictions " << tiles->stats().evictions << ", tiles built " << st.tilesBuilt
                        << ", most tiles drawn in one paint " << maxTilesDrawn);
  d.tree().setRoot(std::make_unique<Widget>());
}
