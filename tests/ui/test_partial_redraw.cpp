// T0.7-fix1: partial redraw. Damage rect bookkeeping (CPU); and on the GPU (offscreen, headless): partial frames
// into a persistent target must give exactly the image a full redraw of the final state gives, after many random
// damage sequences, for raw draw lists, a retained static layer under an animated overlay, and a widget tree
// painted with the damage as clip. Plus the playhead's measured share of shaded pixels.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "pq/ui/damage.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/lines.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scenes.h"
#include "pq/ui/text.h"
#include "pq/ui/widget.h"

using namespace pq::ui;

namespace {

bool disjoint(std::span<const ClipRect> rs) {
  for (size_t i = 0; i < rs.size(); ++i)
    for (size_t j = i + 1; j < rs.size(); ++j) {
      const ClipRect& a = rs[i];
      const ClipRect& b = rs[j];
      if (a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1) return false;
    }
  return true;
}

bool covers(std::span<const ClipRect> rs, int32_t x, int32_t y) {
  for (const ClipRect& r : rs)
    if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) return true;
  return false;
}

// xorshift32: deterministic sequences (no library RNG in tests whose failures must reproduce).
struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
  uint32_t next() noexcept {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  int below(int n) noexcept { return static_cast<int>(next() % static_cast<uint32_t>(n)); }
  float uni(float a, float b) noexcept { return a + (b - a) * static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
  bool chance(int percent) noexcept { return below(100) < percent; }
};

}  // namespace

TEST_CASE("damage: rects are whole pixels, disjoint, and cover everything reported") {
  Rng rng(12345);
  for (int round = 0; round < 200; ++round) {
    Damage d;
    std::vector<Rect> reported;
    const int n = 1 + rng.below(20);
    for (int i = 0; i < n; ++i) {
      const float x = rng.uni(-20, 300), y = rng.uni(-20, 200);
      const Rect r{x, y, x + rng.uni(0.2f, 80), y + rng.uni(0.2f, 60)};
      reported.push_back(r);
      d.add(r);
    }
    REQUIRE(d.needsFrame());
    CHECK_FALSE(d.full());
    CHECK(d.rects().size() <= Damage::kMaxRects);
    CHECK(disjoint(d.rects()));
    // Every pixel any reported rect touches is inside a damage rect.
    bool ok = true;
    for (const Rect& r : reported)
      for (int32_t y = static_cast<int32_t>(std::floor(r.y0)); y < static_cast<int32_t>(std::ceil(r.y1)); ++y)
        for (int32_t x = static_cast<int32_t>(std::floor(r.x0)); x < static_cast<int32_t>(std::ceil(r.x1)); ++x)
          ok = ok && covers(d.rects(), x, y);
    CHECK(ok);
    int64_t area = 0;
    for (const ClipRect& r : d.rects()) area += Damage::area(r);
    CHECK(d.area() == area);
  }
}

TEST_CASE("damage: overlapping and exactly tiling rects merge; separate ones stay separate") {
  Damage d;
  d.add(Rect{10, 0, 12, 100});  // a 2 px playhead column ...
  d.add(Rect{12, 0, 14, 100});  // ... and its next position: one 4 px column
  REQUIRE(d.rects().size() == 1);
  CHECK(d.rects()[0] == ClipRect{10, 0, 14, 100});
  d.add(Rect{200, 0, 202, 100});  // far away: its own rect (not the 192 px bounding box)
  CHECK(d.rects().size() == 2);
  CHECK(d.area() == 4 * 100 + 2 * 100);
  d.add(Rect{0.5f, 50.25f, 300.5f, 50.75f});  // a thin row through both: everything merges
  REQUIRE(d.rects().size() == 1);
  CHECK(d.rects()[0] == ClipRect{0, 0, 301, 100});
  d.addAll();
  CHECK(d.full());
  CHECK(d.rects().empty());
  d.clear();
  CHECK_FALSE(d.needsFrame());
  // More than kMaxRects separate rects fold into kMaxRects without losing coverage.
  for (int i = 0; i < 20; ++i) d.add(Rect{static_cast<float>(i * 10), 0, static_cast<float>(i * 10 + 2), 2});
  CHECK(d.rects().size() <= Damage::kMaxRects);
  CHECK(disjoint(d.rects()));
  for (int i = 0; i < 20; ++i) CHECK(covers(d.rects(), i * 10 + 1, 1));
}

namespace {

// Device, a 3-slot renderer (so the retained layers rotate through real frame slots) and the scene textures.
struct Gpu {
  std::unique_ptr<GpuContext> ctx;
  std::unique_ptr<Renderer> renderer;
  SceneTextures textures;
};

Gpu& gpu() {
  static Gpu g = [] {
    Gpu out;
    GpuOptions o;
    o.validation = true;  // used when the Khronos validation layer is installed
    auto ctx = GpuContext::create(o);
    REQUIRE_MESSAGE(ctx.ok(), (ctx.ok() ? "" : ctx.error().message));
    out.ctx = std::move(*ctx);
    auto r = Renderer::create(*out.ctx, OffscreenTarget::kFormat, 3);
    REQUIRE(r.ok());
    out.renderer = std::move(*r);
    auto t = createSceneTextures(*out.renderer);
    REQUIRE(t.ok());
    out.textures = *t;
    return out;
  }();
  return g;
}

std::unique_ptr<OffscreenTarget> makeTarget(uint32_t w, uint32_t h, uint32_t frames = 1) {
  auto t = OffscreenTarget::create(*gpu().ctx, w, h, frames);
  REQUIRE(t.ok());
  return std::move(*t);
}

Image readBack(OffscreenTarget& t) {
  auto img = t.read();
  REQUIRE(img.ok());
  return std::move(*img);
}

// Exact equality (every channel of every pixel).
void requireIdentical(const Image& partial, const Image& full, const std::string& what) {
  const ImageDiff d = compareImages(partial, full, 0);
  REQUIRE(d.sameSize);
  if (d.pixelsOverTolerance != 0) {
    (void)writePng(what + ".partial.png", partial);
    (void)writePng(what + ".full.png", full);
  }
  CHECK_MESSAGE(d.pixelsOverTolerance == 0, what << ": " << d.pixelsOverTolerance << " px differ (max channel diff " << d.maxChannelDiff << ")");
}

FrameDesc fullFrame(std::span<const DrawLayer> layers) {
  FrameDesc f;
  f.layers = layers;
  f.clear = kSceneClearColor;
  return f;
}

FrameDesc partialFrame(std::span<const DrawLayer> layers, std::span<const ClipRect> damage) {
  FrameDesc f = fullFrame(layers);
  f.full = false;
  f.damage = damage;
  return f;
}

// --- random draw-list scene ---------------------------------------------------------------------------------
enum class Kind : uint8_t { Rect, Rounded, Blob, Ring, Text, Image, Trace, Count };

struct Item {
  Kind kind;
  Rect r;  // the item paints only inside r (it pushes r as its clip)
  Rgba col;
  float phase;
};

Item randomItem(Rng& rng, float W, float H) {
  Item it;
  it.kind = static_cast<Kind>(rng.below(static_cast<int>(Kind::Count)));
  const float w = rng.uni(6, W * 0.4f), h = rng.uni(6, H * 0.4f);
  const float x = rng.uni(-w * 0.3f, W - w * 0.7f), y = rng.uni(-h * 0.3f, H - h * 0.7f);
  // Fractional edges on purpose: AA ramps and outward-rounded clips/damage must agree.
  it.r = {x, y, x + w, y + h};
  it.col = rgba(static_cast<uint32_t>(rng.below(256)), static_cast<uint32_t>(rng.below(256)), static_cast<uint32_t>(rng.below(256)),
                static_cast<uint32_t>(40 + rng.below(216)));
  it.phase = rng.uni(0, 6.28f);
  return it;
}

void drawItem(DrawList& dl, const Item& it) {
  const SceneTextures& tex = gpu().textures;
  const Rect& r = it.r;
  dl.pushClip(r);
  switch (it.kind) {
    case Kind::Rect: dl.addRectFilled({std::round(r.x0), std::round(r.y0), std::round(r.x1), std::round(r.y1)}, it.col); break;
    case Kind::Rounded: dl.addRoundedRectFilled({r.x0 + 0.5f, r.y0 + 0.5f, r.x1 - 0.5f, r.y1 - 0.5f}, 6.0f, it.col); break;
    case Kind::Blob:
      dl.setBlend(BlendMode::Additive);
      dl.addBlob({0.5f * (r.x0 + r.x1), 0.5f * (r.y0 + r.y1)}, 0.6f * std::max(r.width(), r.height()), it.col);
      dl.setBlend(BlendMode::Alpha);
      break;
    case Kind::Ring:
      dl.addCircleStroke({0.5f * (r.x0 + r.x1), 0.5f * (r.y0 + r.y1)}, 0.45f * std::min(r.width(), r.height()), it.col, 2.5f);
      break;
    case Kind::Text: tex.text->draw(dl, tex.fonts.body, {r.x0 + 2, r.y0 + 0.5f * r.height() + 4}, "Partial ≈ full 3/2", it.col); break;
    case Kind::Image: dl.addImage(tex.checker, r, {0, 0}, {1, 1}, withAlpha(it.col, 255)); break;
    case Kind::Trace: {
      std::vector<Vec2> pts;
      for (float x = r.x0; x <= r.x1; x += 2.0f)
        pts.push_back({x, 0.5f * (r.y0 + r.y1) + 0.4f * r.height() * std::sin(it.phase + (x - r.x0) * 0.07f)});
      if (pts.size() >= 2) scopeTrace(dl, pts, it.col, 1.4f, 0.35f);
      break;
    }
    case Kind::Count: break;
  }
  dl.popClip();
}

bool intersects(const Rect& r, std::span<const ClipRect> rs) {
  for (const ClipRect& c : rs)
    if (r.x0 < static_cast<float>(c.x1) && static_cast<float>(c.x0) < r.x1 && r.y0 < static_cast<float>(c.y1) &&
        static_cast<float>(c.y0) < r.y1)
      return true;
  return false;
}

}  // namespace

TEST_CASE("partial redraw: 100 random damage sequences of a draw-list scene equal a full redraw") {
  constexpr uint32_t W = 320, H = 200;
  Rng rng(0xC0FFEEu);
  std::vector<Item> items;
  for (int i = 0; i < 40; ++i) items.push_back(randomItem(rng, W, H));
  auto partialTarget = makeTarget(W, H);
  auto fullTarget = makeTarget(W, H);
  Renderer& renderer = *gpu().renderer;
  DrawList dl;
  auto build = [&](std::span<const ClipRect> cull) {
    gpu().textures.text->beginFrame();
    dl.reset(W, H);
    for (const Item& it : items)
      if (cull.empty() || intersects(it.r, cull)) drawItem(dl, it);
  };
  const DrawLayer layer{&dl, kTransientLayer, 0};
  build({});
  REQUIRE(partialTarget->draw(renderer, fullFrame({&layer, 1})).ok());

  uint64_t partialFrames = 0, culledFrames = 0, multiRectFrames = 0, damagedPx = 0, retainedFrames = 0, retainedVersion = 0;
  for (int seq = 0; seq < 100; ++seq) {
    const int steps = 1 + rng.below(8);
    for (int s = 0; s < steps; ++s) {
      Damage dmg;
      const int changes = 1 + rng.below(4);
      for (int c = 0; c < changes; ++c) {
        const int op = rng.below(6);
        const size_t i = static_cast<size_t>(rng.below(static_cast<int>(items.size())));
        if (op == 0 && items.size() > 4) {  // remove
          dmg.add(items[i].r);
          items.erase(items.begin() + static_cast<ptrdiff_t>(i));
        } else if (op == 1) {  // add on top
          items.push_back(randomItem(rng, W, H));
          dmg.add(items.back().r);
        } else if (op == 2) {  // raise to the top
          dmg.add(items[i].r);
          Item it = items[i];
          items.erase(items.begin() + static_cast<ptrdiff_t>(i));
          items.push_back(it);
        } else if (op == 3) {  // recolour
          items[i].col = randomItem(rng, W, H).col;
          dmg.add(items[i].r);
        } else {  // move / resize (sometimes by a fraction of a pixel)
          dmg.add(items[i].r);
          const float dx = rng.chance(30) ? rng.uni(-1, 1) : rng.uni(-40, 40), dy = rng.chance(30) ? rng.uni(-1, 1) : rng.uni(-30, 30);
          items[i].r = {items[i].r.x0 + dx, items[i].r.y0 + dy, items[i].r.x1 + dx + rng.uni(-3, 3), items[i].r.y1 + dy + rng.uni(-3, 3)};
          if (items[i].r.x1 < items[i].r.x0 + 2) items[i].r.x1 = items[i].r.x0 + 2;
          if (items[i].r.y1 < items[i].r.y0 + 2) items[i].r.y1 = items[i].r.y0 + 2;
          dmg.add(items[i].r);
        }
      }
      if (rng.chance(20)) dmg.add(Rect{rng.uni(0, W), rng.uni(0, H), rng.uni(0, W) + 3, rng.uni(0, H) + 3});  // spurious
      REQUIRE(disjoint(dmg.rects()));
      // Half of the frames carry only what touches the damage (as widget paint does); the rest the whole scene.
      // A third of the frames (unculled) go through a retained layer, whose draws are cut to the index runs
      // that reach the damage.
      const bool cull = rng.chance(50);
      const bool retain = !cull && rng.chance(66);
      build(cull ? dmg.rects() : std::span<const ClipRect>{});
      const DrawLayer retained{&dl, 2, ++retainedVersion};
      REQUIRE(partialTarget->draw(renderer, partialFrame({retain ? &retained : &layer, 1}, dmg.rects())).ok());
      ++partialFrames;
      retainedFrames += retain ? 1 : 0;
      culledFrames += cull ? 1 : 0;
      multiRectFrames += dmg.rects().size() > 1 ? 1 : 0;
      damagedPx += static_cast<uint64_t>(dmg.area());
    }
    build({});
    REQUIRE(fullTarget->draw(renderer, fullFrame({&layer, 1})).ok());
    requireIdentical(readBack(*partialTarget), readBack(*fullTarget), "partial-drawlist-seq" + std::to_string(seq));
  }
  MESSAGE(partialFrames << " partial frames (" << culledFrames << " culled, " << retainedFrames << " retained, " << multiRectFrames << " with several rects), mean damage "
                        << (100.0 * static_cast<double>(damagedPx) / static_cast<double>(partialFrames) / (W * H)) << " % of the target");
  CHECK(gpu().ctx->validationErrorCount() == 0);
}

TEST_CASE("partial redraw: a playhead over a retained `lines` layer equals the full scene (100 random sequences)") {
  constexpr uint32_t W = kDefaultSceneWidth, H = kDefaultSceneHeight;
  Renderer& renderer = *gpu().renderer;
  const SceneTextures& tex = gpu().textures;
  auto partialTarget = makeTarget(W, H, 3);  // three frames in flight: each slot keeps its own retained copy
  auto fullTarget = makeTarget(W, H);
  DrawList staticLayer, overlay, full;
  staticLayer.reset(W, H);
  REQUIRE(buildSceneStatic(kPlayheadScene, staticLayer, tex));
  uint64_t version = 1;
  Rng rng(777);
  double t = 0.0;
  overlay.reset(W, H);
  REQUIRE(buildSceneOverlay(kPlayheadScene, overlay, tex, t));
  DrawLayer layers[2] = {{&staticLayer, 0, version}, {&overlay, kTransientLayer, 0}};
  REQUIRE(partialTarget->submit(renderer, fullFrame(layers)).ok());
  ClipRect last = playheadRect(W, H, t);
  const uint64_t retainedBefore = renderer.stats().retainedUploads;
  uint64_t frames = 0;
  for (int seq = 0; seq < 100; ++seq) {
    const int steps = 1 + rng.below(6);
    for (int s = 0; s < steps; ++s) {
      // Mostly small steps (a frame of playback), sometimes a jump (locate), sometimes no movement at all.
      t += rng.chance(15) ? rng.uni(0, 4) : rng.chance(10) ? 0.0 : rng.uni(0.002f, 0.03f);
      const ClipRect now = playheadRect(W, H, t);
      Damage dmg;
      if (!(now == last)) {
        dmg.addPixels(last);
        dmg.addPixels(now);
      }
      overlay.reset(W, H);
      buildSceneOverlay(kPlayheadScene, overlay, tex, t);
      REQUIRE(partialTarget->submit(renderer, partialFrame(layers, dmg.rects())).ok());
      last = now;
      ++frames;
    }
    REQUIRE(partialTarget->finish().ok());  // the renderer's frame slots are shared by both targets
    full.reset(W, H);
    REQUIRE(buildScene(kPlayheadScene, full, tex, t));
    const DrawLayer fl{&full, kTransientLayer, 0};
    REQUIRE(fullTarget->draw(renderer, fullFrame({&fl, 1})).ok());
    requireIdentical(readBack(*partialTarget), readBack(*fullTarget), "partial-playhead-seq" + std::to_string(seq));
  }
  // The static layer went to the GPU once per frame slot, never again while only the playhead moved.
  CHECK(renderer.stats().retainedUploads - retainedBefore <= 2);
  MESSAGE(frames << " partial playhead frames; retained uploads after the first frame: " << (renderer.stats().retainedUploads - retainedBefore));

  // A new static version is uploaded again (per slot) and drawn.
  staticLayer.reset(W, H);
  REQUIRE(buildScene("lines", staticLayer, tex, 1.25));  // a different static frame
  layers[0].version = ++version;
  const uint64_t before = renderer.stats().retainedUploads;
  for (int i = 0; i < 3; ++i) REQUIRE(partialTarget->submit(renderer, fullFrame(layers)).ok());
  CHECK(renderer.stats().retainedUploads - before == 3);
  REQUIRE(partialTarget->finish().ok());
  full.reset(W, H);
  buildScene("lines", full, tex, 1.25);
  buildSceneOverlay(kPlayheadScene, full, tex, t);
  const DrawLayer fl{&full, kTransientLayer, 0};
  REQUIRE(fullTarget->draw(renderer, fullFrame({&fl, 1})).ok());
  requireIdentical(readBack(*partialTarget), readBack(*fullTarget), "partial-playhead-newstatic");
  CHECK(gpu().ctx->validationErrorCount() == 0);
}

TEST_CASE("partial redraw: a 2 px playhead over `lines` shades <= 2 % of the pixels per frame") {
  constexpr uint32_t W = kDefaultSceneWidth, H = kDefaultSceneHeight;
  if (!gpu().ctx->pipelineStatistics() && gpu().ctx->timestampPeriodNs() == 0.0f) {
    MESSAGE("no pipeline statistics or timestamps on this device: not measurable");
    return;
  }
  Renderer& renderer = *gpu().renderer;
  const SceneTextures& tex = gpu().textures;
  auto target = makeTarget(W, H);
  REQUIRE(target->enableProfiling().ok());
  DrawList staticLayer, overlay, full;
  staticLayer.reset(W, H);
  buildSceneStatic(kPlayheadScene, staticLayer, tex);
  DrawLayer layers[2] = {{&staticLayer, 1, 1}, {&overlay, kTransientLayer, 0}};
  double t = 0.0;
  overlay.reset(W, H);
  buildSceneOverlay(kPlayheadScene, overlay, tex, t);
  REQUIRE(target->draw(renderer, fullFrame(layers)).ok());
  ClipRect last = playheadRect(W, H, t);
  std::vector<double> partialFrags, partialMs, fullFrags, fullMs, damage;
  for (int i = 0; i < 60; ++i) {
    t += 1.0 / 144.0;  // 144 Hz playback: the playhead moves ~1.4 px per frame
    const ClipRect now = playheadRect(W, H, t);
    Damage dmg;
    dmg.addPixels(last);
    dmg.addPixels(now);
    last = now;
    overlay.reset(W, H);
    buildSceneOverlay(kPlayheadScene, overlay, tex, t);
    REQUIRE(target->draw(renderer, partialFrame(layers, dmg.rects())).ok());
    auto st = target->lastFrameStats();
    REQUIRE(st.ok());
    if (st->hasFragments) partialFrags.push_back(static_cast<double>(st->fragments));
    if (st->hasTimes) partialMs.push_back(st->drawMs);
    damage.push_back(static_cast<double>(dmg.area()));
    // The same frame through the full path (rebuild everything into one transient list, clear, draw).
    full.reset(W, H);
    buildScene(kPlayheadScene, full, tex, t);
    const DrawLayer fl{&full, kTransientLayer, 0};
    REQUIRE(target->draw(renderer, fullFrame({&fl, 1})).ok());
    st = target->lastFrameStats();
    REQUIRE(st.ok());
    if (st->hasFragments) fullFrags.push_back(static_cast<double>(st->fragments));
    if (st->hasTimes) fullMs.push_back(st->drawMs);
  }
  auto median = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
  };
  const double pixels = static_cast<double>(W) * H;
  const double maxDamage = *std::max_element(damage.begin(), damage.end());
  MESSAGE("damage per frame: max " << maxDamage << " px = " << 100.0 * maxDamage / pixels << " % of the target");
  CHECK(maxDamage <= 0.02 * pixels);
  if (!partialFrags.empty()) {
    const double worst = *std::max_element(partialFrags.begin(), partialFrags.end());
    MESSAGE("fragment-shader invocations per frame: partial median " << median(partialFrags) << " (worst " << worst << " = "
                                                                      << 100.0 * worst / pixels << " % of the pixels), full median "
                                                                      << median(fullFrags) << " (" << 100.0 * median(fullFrags) / pixels << " %)");
    CHECK(worst <= 0.02 * pixels);
  }
  if (!partialMs.empty())
    MESSAGE("GPU draw time (back to back): partial median " << median(partialMs) << " ms, full median " << median(fullMs) << " ms");
  CHECK(gpu().ctx->validationErrorCount() == 0);
}

TEST_CASE("partial redraw: a partial frame needs a previous frame; an empty damage list changes nothing") {
  Renderer& renderer = *gpu().renderer;
  auto target = makeTarget(64, 32);
  DrawList dl;
  dl.reset(64, 32);
  dl.addRectFilled({0, 0, 64, 32}, rgba(200, 10, 10));
  const DrawLayer layer{&dl, kTransientLayer, 0};
  const ClipRect r{4, 4, 8, 8};
  CHECK_FALSE(target->draw(renderer, partialFrame({&layer, 1}, {&r, 1})).ok());
  REQUIRE(target->draw(renderer, fullFrame({&layer, 1})).ok());
  const Image before = readBack(*target);
  dl.reset(64, 32);
  dl.addRectFilled({0, 0, 64, 32}, rgba(10, 200, 10));
  REQUIRE(target->draw(renderer, partialFrame({&layer, 1}, {})).ok());
  requireIdentical(readBack(*target), before, "partial-empty");
  // Only the damaged pixels take the new colour; damage outside the target is clipped away.
  const ClipRect rs[2] = {{4, 4, 8, 8}, {60, 28, 90, 90}};
  REQUIRE(target->draw(renderer, partialFrame({&layer, 1}, rs)).ok());
  const Image after = readBack(*target);
  CHECK(after.pixel(5, 5)[1] == 200);
  CHECK(after.pixel(63, 31)[1] == 200);
  CHECK(after.pixel(8, 8)[0] == 200);
  CHECK(after.pixel(59, 31)[0] == 200);
}

namespace {

// A widget that paints only inside its rect (the partial-redraw contract): a fill, an AA inset, an additive
// glow and a label, all under its own clip.
class Box : public Widget {
public:
  Box(std::string name, Rgba col, std::string label = {}) : Widget(std::move(name)), col_(col), label_(std::move(label)) {}
  void setColor(Rgba c) {
    col_ = c;
    markDirty();
  }
  void paint(PaintContext& ctx) override {
    const Rect r = rect().toRect();
    ctx.dl.pushClip(r);
    ctx.dl.addRectFilled(r, col_);
    ctx.dl.addRoundedRectFilled({r.x0 + 3.5f, r.y0 + 2.5f, r.x1 - 3.5f, r.y1 - 2.5f}, 4.0f, withAlpha(rgba(255, 255, 255), 40));
    ctx.dl.setBlend(BlendMode::Additive);
    ctx.dl.addBlob({r.x1 - 6.0f, r.y0 + 6.0f}, 9.0f, withAlpha(col_, 120));
    ctx.dl.setBlend(BlendMode::Alpha);
    if (!label_.empty() && ctx.res.text && ctx.res.fonts)
      ctx.res.text->draw(ctx.dl, ctx.res.fonts->body, {r.x0 + 6.0f, r.y0 + 15.0f}, label_, rgba(240, 240, 245));
    ctx.dl.popClip();
  }

private:
  Rgba col_;
  std::string label_;
};

// A box with a drop shadow outside its rect (like T3.2's popups), declared with setVisualOverflow.
class ShadowBox : public Box {
public:
  using Box::Box;
  explicit ShadowBox(std::string name, Rgba col) : Box(std::move(name), col) { setVisualOverflow({1, 1, 1, 3}); }
  void paint(PaintContext& ctx) override {
    const Rect r = rect().toRect();
    ctx.dl.addRoundedRectFilled({r.x0 - 1.0f, r.y0 + 1.0f, r.x1 + 1.0f, r.y1 + 3.0f}, 3.0f, rgba(0, 0, 0, 110));
    Box::paint(ctx);
  }
};

// Virtualised content: a large grid that paints only the cells inside PaintContext::clip.
class Grid : public Widget {
public:
  explicit Grid(std::string name) : Widget(std::move(name)) {}
  ISize contentExtent(ISize) const override { return {40 * kCell, 60 * kCell}; }
  void setSeed(uint32_t s) {
    seed_ = s;
    markDirty();
  }
  void paint(PaintContext& ctx) override {
    const IRect r = rect();
    const IRect vis = intersect(r, ctx.clip);
    if (vis.empty()) return;
    ctx.dl.pushClip(vis.toRect());
    const int32_t c0 = (vis.x - r.x) / kCell, c1 = (vis.right() - 1 - r.x) / kCell;
    const int32_t r0 = (vis.y - r.y) / kCell, r1 = (vis.bottom() - 1 - r.y) / kCell;
    for (int32_t row = r0; row <= r1; ++row)
      for (int32_t col = c0; col <= c1; ++col) {
        const uint32_t h = (static_cast<uint32_t>(row) * 73856093u) ^ (static_cast<uint32_t>(col) * 19349663u) ^ seed_;
        const float x = static_cast<float>(r.x + col * kCell), y = static_cast<float>(r.y + row * kCell);
        ctx.dl.addRoundedRectFilled({x + 1.5f, y + 1.5f, x + kCell - 1.5f, y + kCell - 1.5f}, 3.0f,
                                    rgba(40 + (h & 127), 40 + ((h >> 8) & 127), 60 + ((h >> 16) & 127)));
      }
    ctx.dl.popClip();
  }

private:
  static constexpr int32_t kCell = 18;
  uint32_t seed_ = 1;
};

}  // namespace

TEST_CASE("partial redraw: a widget tree painted with the damage as clip equals a full repaint (100 random sequences)") {
  constexpr uint32_t W = 480, H = 320;
  Renderer& renderer = *gpu().renderer;
  const SceneTextures& tex = gpu().textures;
  PaintResources res;
  res.text = tex.text.get();
  res.fonts = &tex.fonts;

  UiTree tree;
  tree.setViewport(W, H);
  Widget& root = tree.emplaceRoot<Widget>();
  root.setLayout(LayoutKind::Column).setGap(2);
  Widget& header = root.add<Widget>("header");
  header.setLayout(LayoutKind::Row).setHeight(Length::fixed(26)).setGap(2);
  std::vector<Box*> boxes;
  for (int i = 0; i < 6; ++i)
    boxes.push_back(&header.add<Box>("h" + std::to_string(i), rgba(60 + 20 * i, 70, 90), i % 2 ? "Track" : ""));
  for (int i = 0; i < 2; ++i) boxes.push_back(&header.add<ShadowBox>("s" + std::to_string(i), rgba(120, 90, 160)));
  for (Box* b : boxes) b->setWidth(Length::fixed(60));
  auto& split = root.add<Splitter>("split", Splitter::Axis::Horizontal, 160.0f);
  split.setLimits(40, 400, 40);
  Widget& left = split.add<Widget>("left");
  left.setLayout(LayoutKind::Column).setGap(1);
  for (int i = 0; i < 8; ++i) {
    Box& b = left.add<Box>("l" + std::to_string(i), rgba(90, 50 + 15 * i, 70), "Device " + std::to_string(i));
    b.setHeight(Length::fixed(22));
    boxes.push_back(&b);
  }
  auto& scroll = split.add<ScrollView>("scroll");
  Grid& grid = scroll.add<Grid>("grid");
  Widget& footer = root.add<Widget>("footer");
  footer.setLayout(LayoutKind::Row).setHeight(Length::fixed(24));
  for (int i = 0; i < 4; ++i) boxes.push_back(&footer.add<Box>("f" + std::to_string(i), rgba(50, 60 + 25 * i, 60), "Clip"));

  auto partialTarget = makeTarget(W, H);
  auto fullTarget = makeTarget(W, H);
  DrawList dl;
  const DrawLayer layer{&dl, kTransientLayer, 0};
  double now = 0.0;
  // First frame: full.
  tree.update(now);
  tex.text->beginFrame();
  dl.reset(W, H);
  tree.paint(dl, res, now);
  REQUIRE(partialTarget->draw(renderer, fullFrame({&layer, 1})).ok());

  Rng rng(4242);
  uint64_t frames = 0, fullFrames = 0, painted = 0, paintedFull = 0;
  for (int seq = 0; seq < 100; ++seq) {
    const int steps = 1 + rng.below(6);
    for (int s = 0; s < steps; ++s) {
      const int changes = 1 + rng.below(3);
      for (int c = 0; c < changes; ++c) {
        Box& b = *boxes[static_cast<size_t>(rng.below(static_cast<int>(boxes.size())))];
        switch (rng.below(6)) {
          case 0: b.setColor(rgba(static_cast<uint32_t>(rng.below(256)), static_cast<uint32_t>(rng.below(256)), 90)); break;
          case 1: b.setVisible(!b.visible()); break;
          case 2:
            if (b.parent() == &left) {
              b.setHeight(Length::fixed(static_cast<float>(14 + rng.below(24))));
            } else {
              b.setWidth(Length::fixed(static_cast<float>(30 + rng.below(80))));
            }
            break;
          case 3: split.setPosition(static_cast<float>(60 + rng.below(300))); break;
          case 4: scroll.scrollTo(rng.below(600), rng.below(900)); break;
          default: grid.setSeed(rng.next()); break;
        }
      }
      now += 1.0 / 144.0;
      tree.update(now);  // layout: moved widgets damage their old and new rects
      tex.text->beginFrame();
      dl.reset(W, H);
      const Damage& d = tree.damage();
      if (!d.needsFrame()) continue;
      if (d.full()) {
        tree.paint(dl, res, now);
        REQUIRE(partialTarget->draw(renderer, fullFrame({&layer, 1})).ok());
        ++fullFrames;
        continue;
      }
      ClipRect rects[Damage::kMaxRects];
      uint32_t n = 0;
      ClipRect box{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
      for (const ClipRect& r : d.rects()) {
        rects[n++] = r;
        box = Damage::unite(box, r);
      }
      tree.paint(dl, res, now, {box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0});
      painted += tree.stats().widgetsPainted;
      REQUIRE(partialTarget->draw(renderer, partialFrame({&layer, 1}, {rects, n})).ok());
      ++frames;
    }
    // Reference: a full repaint of the final state on a fresh clear.
    tex.text->beginFrame();
    dl.reset(W, H);
    tree.paint(dl, res, now);
    paintedFull += tree.stats().widgetsPainted;
    REQUIRE(fullTarget->draw(renderer, fullFrame({&layer, 1})).ok());
    requireIdentical(readBack(*partialTarget), readBack(*fullTarget), "partial-widgets-seq" + std::to_string(seq));
  }
  REQUIRE(frames > 0);
  MESSAGE(frames << " partial frames (" << fullFrames << " full), widgets painted per partial frame "
                 << static_cast<double>(painted) / static_cast<double>(frames) << " vs " << static_cast<double>(paintedFull) / 100.0
                 << " in a full repaint");
  CHECK(gpu().ctx->validationErrorCount() == 0);
}

TEST_CASE("partial redraw: the T3.2 control gallery under random input equals a full repaint (100 random sequences)") {
  uint32_t W = 0, H = 0;
  sceneDefaultSize("widgets", W, H);
  Renderer& renderer = *gpu().renderer;
  const SceneTextures& tex = gpu().textures;
  const PaintResources res = paintResources(tex);
  UiTree tree;
  tree.setViewport(W, H, tex.scale);
  tree.setResources(&res);
  buildWidgetsGallery(tree);  // opens a context menu + submenu, a popover and a tooltip (drop shadows)

  auto partialTarget = makeTarget(W, H);
  auto fullTarget = makeTarget(W, H);
  DrawList dl;
  const DrawLayer layer{&dl, kTransientLayer, 0};
  double now = 0.0;
  tree.update(now);
  tex.text->beginFrame();
  dl.reset(W, H);
  tree.paint(dl, res, now);
  REQUIRE(partialTarget->draw(renderer, fullFrame({&layer, 1})).ok());

  Rng rng(2026);
  auto pointer = [&](PointerType type, Vec2 p, PointerButton b = PointerButton::None) {
    PointerEvent e;
    e.type = type;
    e.pos = p;
    e.button = b;
    e.clickCount = b == PointerButton::None ? 0 : 1;
    if (type == PointerType::Wheel) e.wheel = {0, rng.chance(50) ? 1.0f : -1.0f};
    tree.dispatchPointer(e);
  };
  auto key = [&](Key k) {
    KeyEvent e;
    e.key = k;
    tree.dispatchKey(e);
    e.down = false;
    tree.dispatchKey(e);
  };
  const Key keys[] = {key::kTab, key::kEscape, key::kUp, key::kDown, key::kLeft, key::kRight, key::kReturn, key::kSpace};
  uint64_t frames = 0, fullFrames = 0, events = 0;
  for (int seq = 0; seq < 100; ++seq) {
    const int steps = 1 + rng.below(6);
    for (int s = 0; s < steps; ++s) {
      const int n = 1 + rng.below(3);
      for (int i = 0; i < n; ++i, ++events) {
        const Vec2 p{rng.uni(0, static_cast<float>(W)), rng.uni(0, static_cast<float>(H))};
        switch (rng.below(7)) {
          case 0:
          case 1: pointer(PointerType::Move, p); break;
          case 2:
            pointer(PointerType::Move, p);
            pointer(PointerType::Down, p, PointerButton::Left);
            pointer(PointerType::Up, p, PointerButton::Left);
            break;
          case 3: {  // drag (faders, knobs, scrollbars, drag boxes)
            pointer(PointerType::Move, p);
            pointer(PointerType::Down, p, PointerButton::Left);
            const Vec2 q{p.x + rng.uni(-60, 60), p.y + rng.uni(-60, 60)};
            pointer(PointerType::Move, q);
            pointer(PointerType::Up, q, PointerButton::Left);
            break;
          }
          case 4: pointer(PointerType::Wheel, p); break;
          case 5:
            pointer(PointerType::Down, p, PointerButton::Right);
            pointer(PointerType::Up, p, PointerButton::Right);
            break;
          default: key(keys[rng.below(static_cast<int>(std::size(keys)))]); break;
        }
      }
      // Sometimes long enough for hover tooltips and caret blinks to fire.
      now += rng.chance(25) ? static_cast<double>(rng.uni(0.3f, 1.2f)) : 1.0 / 144.0;
      tree.update(now);
      if (!tree.needsPaint()) continue;
      tex.text->beginFrame();
      dl.reset(W, H);
      const Damage& d = tree.damage();
      if (d.full()) {
        tree.paint(dl, res, now);
        REQUIRE(partialTarget->draw(renderer, fullFrame({&layer, 1})).ok());
        ++fullFrames;
        continue;
      }
      ClipRect rects[Damage::kMaxRects];
      uint32_t nr = 0;
      ClipRect box{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
      for (const ClipRect& r : d.rects()) {
        rects[nr++] = r;
        box = Damage::unite(box, r);
      }
      tree.paint(dl, res, now, {box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0});
      REQUIRE(partialTarget->draw(renderer, partialFrame({&layer, 1}, {rects, nr})).ok());
      ++frames;
    }
    tex.text->beginFrame();
    dl.reset(W, H);
    tree.paint(dl, res, now);
    REQUIRE(fullTarget->draw(renderer, fullFrame({&layer, 1})).ok());
    requireIdentical(readBack(*partialTarget), readBack(*fullTarget), "partial-gallery-seq" + std::to_string(seq));
  }
  MESSAGE(events << " random input events, " << frames << " partial frames, " << fullFrames << " full");
  CHECK(frames > 0);
  CHECK(gpu().ctx->validationErrorCount() == 0);
}
