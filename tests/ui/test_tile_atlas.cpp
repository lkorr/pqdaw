// T3.8-fix1: the tile atlas (pq/ui/tile_atlas.h). Shelf allocation with a transparent border, per-page LRU eviction
// that never clears a page used in the current frame, generations that invalidate a cleared page's tiles, the upload
// queue (dropped on eviction and at the frame boundary), bounded memory; and through the renderer: tiles drawn 1:1
// are texel-exact (the same pixels as the rects they replace), and the tile pages' GPU memory stays at the bound.

#include <doctest/doctest.h>

#include <cstring>
#include <memory>
#include <vector>

#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/tile_atlas.h"

using namespace pq::ui;

TEST_CASE("tile atlas: allocations get a zeroed border, stay on one page and are queued for upload") {
  TileAtlas a(2, 64);
  CHECK(a.maxTileSize() == 62);
  TileAtlas::Tile t1, t2;
  REQUIRE(a.allocate(10, 5, t1));
  REQUIRE(a.allocate(7, 5, t2));
  CHECK(a.pageCount() == 1);
  CHECK(t1.page == 0);
  CHECK(t1.x == 1);
  CHECK(t1.y == 1);
  // Same shelf (heights round up to 4 with the border: 5 + 2 → 8), side by side with both borders.
  CHECK(t2.y == t1.y);
  CHECK(t2.x == t1.x + t1.w + 2);
  uint8_t* p = a.write(t1);
  REQUIRE(p != nullptr);
  const uint32_t pitch = TileAtlas::rowPitch(t1);
  for (uint32_t i = 0; i < pitch * (t1.h + 2); ++i) CHECK(p[i] == 0);
  std::memset(p + pitch + 1, 200, t1.w);  // first content row
  REQUIRE(a.uploads().size() == 1);
  const TileAtlas::Upload u = a.uploads()[0];
  CHECK(u.page == 0);
  CHECK(u.x == t1.x - 1);
  CHECK(u.y == t1.y - 1);
  CHECK(u.w == t1.w + 2);
  CHECK(u.h == t1.h + 2);
  CHECK(u.offset % 4 == 0);
  CHECK(a.uploadBytes()[u.offset + pitch + 1] == 200);
  CHECK(a.uploadBytes()[u.offset + pitch] == 0);  // border
  REQUIRE(a.write(t2) != nullptr);
  CHECK(a.uploads()[1].offset % 4 == 0);
  // Too large for a page; zero-sized.
  TileAtlas::Tile big;
  CHECK_FALSE(a.allocate(63, 4, big));
  CHECK_FALSE(a.allocate(0, 4, big));
  CHECK(a.stats().failures == 2);
  // A frame boundary keeps the queue (nothing staged it yet) and the tiles; the renderer's staging drops it.
  a.beginFrame();
  CHECK(a.uploads().size() == 2);
  a.clearUploads();
  CHECK(a.uploads().empty());
  CHECK(a.uploadBytes().empty());
  CHECK(a.valid(t1));
  CHECK(a.valid(t2));
}

TEST_CASE("tile atlas: LRU page eviction, never a page used this frame; generations invalidate cleared tiles") {
  TileAtlas a(2, 64);  // a 60×60 tile (62 with border, 64 rounded) fills a page
  TileAtlas::Tile p0, p1, p2;
  REQUIRE(a.allocate(60, 60, p0));
  REQUIRE(a.allocate(60, 60, p1));
  CHECK(p0.page != p1.page);
  CHECK(a.pageCount() == 2);
  // Both pages were used in this frame: nothing can be evicted.
  CHECK_FALSE(a.allocate(60, 60, p2));
  CHECK(a.valid(p0));
  CHECK(a.valid(p1));
  // Next frame: page of p1 touched, page of p0 not → p0's page goes and p0 is invalid.
  REQUIRE(a.write(p0) != nullptr);
  a.beginFrame();
  a.touch(p1.page);
  REQUIRE(a.allocate(60, 60, p2));
  CHECK(p2.page == p0.page);
  CHECK_FALSE(a.valid(p0));
  CHECK(a.write(p0) == nullptr);
  CHECK(a.valid(p1));
  CHECK(a.valid(p2));
  CHECK(a.stats().evictions == 1);
  CHECK(a.uploads().empty());  // p0's queued texels went with its page
  // Memory stays at the bound however many tiles come and go.
  for (int frame = 0; frame < 200; ++frame) {
    a.beginFrame();
    for (int i = 0; i < 5; ++i) {
      TileAtlas::Tile t;
      if (a.allocate(10 + static_cast<uint32_t>(i) * 7, 3 + static_cast<uint32_t>(frame % 11), t)) REQUIRE(a.write(t) != nullptr);
    }
    CHECK(a.pageCount() <= a.maxPages());
    CHECK(a.gpuBytes() <= a.maxGpuBytes());
  }
  CHECK(a.maxGpuBytes() == 2u * 64u * 64u);
  CHECK(a.stats().evictions > 1);
}

TEST_CASE("tile atlas: the renderer's default cache is 4 pages of 2048² R8 (16 MiB)") {
  TileAtlas a;
  CHECK(a.maxPages() == kMaxTilePages);
  CHECK(a.pageSize() == kTilePageSize);
  CHECK(a.maxGpuBytes() == size_t{16} << 20);
  CHECK(TileAtlas::texture(0) == kFirstTileTexture);
  CHECK(tilePageTexture(kMaxTilePages - 1) == Renderer::kMaxTextures - 1);
  CHECK_FALSE(isAtlasPageTexture(TileAtlas::texture(0)));
}

TEST_CASE("render: a tile drawn 1:1 gives exactly the pixels of the rects it holds; GPU memory stays at the bound") {
  GpuOptions o;
  o.validation = true;
  auto gpu = GpuContext::create(o);
  REQUIRE_MESSAGE(gpu.ok(), (gpu.ok() ? "" : gpu.error().message));
  auto renderer = Renderer::create(**gpu, OffscreenTarget::kFormat, 1);
  REQUIRE(renderer.ok());
  TileAtlas& tiles = *(*renderer)->tiles();
  constexpr uint32_t W = 96, H = 48;
  auto target = OffscreenTarget::create(**gpu, W, H);
  REQUIRE(target.ok());
  const Rgba bg = rgba(40, 60, 80), body = rgba(200, 120, 90), note = rgba(20, 30, 40);

  // The pattern: opaque runs, a translucent run (alpha 90) and a one-column translucent seam (alpha 70).
  struct Run {
    uint32_t x0, x1, y0, y1;
    uint8_t a;
  };
  const Run runs[] = {{2, 9, 1, 4}, {11, 30, 6, 9}, {31, 33, 12, 15}, {5, 20, 16, 19}, {25, 26, 0, 20}};
  constexpr uint32_t TW = 40, TH = 20;
  // Reference: the rects themselves over the body.
  DrawList ref;
  ref.reset(W, H);
  ref.addRectFilled({10, 10, 10 + TW, 10 + TH}, body);
  for (const Run& r : runs)
    ref.addRectFilled({10.0f + static_cast<float>(r.x0), 10.0f + static_cast<float>(r.y0), 10.0f + static_cast<float>(r.x1),
                       10.0f + static_cast<float>(r.y1)},
                      withAlpha(note, r.a));
  REQUIRE((*target)->draw(**renderer, ref, bg).ok());
  auto want = (*target)->read();
  REQUIRE(want.ok());

  // The same as a tile: coverage = the runs' alphas; drawn with the note colour.
  TileAtlas::Tile t;
  REQUIRE(tiles.allocate(TW, TH, t));
  uint8_t* px = tiles.write(t);
  REQUIRE(px != nullptr);
  const uint32_t pitch = TileAtlas::rowPitch(t);
  for (const Run& r : runs)
    for (uint32_t y = r.y0; y < r.y1; ++y)
      for (uint32_t x = r.x0; x < r.x1; ++x) px[(y + 1) * pitch + x + 1] = r.a;
  DrawList dl;
  dl.reset(W, H);
  dl.addRectFilled({10, 10, 10 + TW, 10 + TH}, body);
  const float inv = tiles.uvScale();
  // Drawn in two pieces (as a partly visible clip would be): still texel-exact.
  dl.addImage(TileAtlas::texture(t.page), {10, 10, 27, 10 + TH}, {static_cast<float>(t.x) * inv, static_cast<float>(t.y) * inv},
              {static_cast<float>(t.x + 17) * inv, static_cast<float>(t.y + TH) * inv}, note);
  dl.addImage(TileAtlas::texture(t.page), {27, 10, 10 + TW, 10 + TH},
              {static_cast<float>(t.x + 17) * inv, static_cast<float>(t.y) * inv},
              {static_cast<float>(t.x + TW) * inv, static_cast<float>(t.y + TH) * inv}, note);
  REQUIRE((*target)->draw(**renderer, dl, bg).ok());
  CHECK(tiles.uploads().empty());  // staged and dropped by the renderer's upload
  auto got = (*target)->read();
  REQUIRE(got.ok());
  const ImageDiff d = compareImages(*want, *got, 0);
  CHECK_MESSAGE(d.pixelsOverTolerance == 0, d.pixelsOverTolerance << " px differ, max " << d.maxChannelDiff);

  // Fill every page and keep allocating over many frames: the pages' GPU memory stays at the bound.
  const VkDeviceSize page = VkDeviceSize(kTilePageSize) * kTilePageSize;
  for (int frame = 0; frame < 24; ++frame) {
    for (int i = 0; i < 8; ++i) {
      TileAtlas::Tile big;
      if (tiles.allocate(2000, 500, big)) REQUIRE(tiles.write(big) != nullptr);
    }
    DrawList empty;
    empty.reset(W, H);
    REQUIRE((*target)->draw(**renderer, empty, bg).ok());
    CHECK(tiles.pageCount() <= kMaxTilePages);
    CHECK((*renderer)->tilesGpuBytes() >= VkDeviceSize(tiles.pageCount()) * page);
    CHECK((*renderer)->tilesGpuBytes() <= VkDeviceSize(kMaxTilePages) * page + VkDeviceSize(kMaxTilePages) * 65536);
  }
  CHECK(tiles.pageCount() == kMaxTilePages);
  CHECK(tiles.stats().evictions > 0);
  CHECK((*gpu)->validationErrorCount() == 0);
}
