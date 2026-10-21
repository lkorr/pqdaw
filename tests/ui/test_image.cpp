#include <doctest/doctest.h>

#include <random>
#include <string>

#include "pq/ui/damage.h"
#include "pq/ui/image.h"

using namespace pq::ui;

namespace {
Image noiseImage(uint32_t w, uint32_t h, uint32_t seed) {
  Image img;
  img.width = w;
  img.height = h;
  img.rgba.resize(size_t(w) * h * 4);
  std::mt19937 rng(seed);
  for (size_t i = 0; i < img.rgba.size(); ++i) {
    // Mix of smooth and noisy content so every PNG filter type gets chosen somewhere.
    const size_t px = i / 4;
    img.rgba[i] = (px / w) % 3 == 0 ? static_cast<uint8_t>(rng()) : static_cast<uint8_t>((px % w) * 3 + (px / w));
  }
  return img;
}
}  // namespace

TEST_CASE("png: encode/decode round trip is lossless") {
  for (uint32_t seed = 1; seed <= 3; ++seed) {
    const Image a = noiseImage(37 + seed * 11, 23 + seed * 5, seed);
    const std::vector<uint8_t> bytes = encodePng(a);
    REQUIRE(!bytes.empty());
    auto b = decodePng(bytes.data(), bytes.size());
    REQUIRE(b.ok());
    CHECK(b->width == a.width);
    CHECK(b->height == a.height);
    CHECK(b->rgba == a.rgba);
  }
}

TEST_CASE("png: corrupt input is rejected, not crashed on") {
  const Image a = noiseImage(16, 16, 9);
  std::vector<uint8_t> bytes = encodePng(a);
  {
    auto bad = bytes;
    bad[0] = 0;
    CHECK_FALSE(decodePng(bad.data(), bad.size()).ok());
  }
  {
    auto bad = bytes;
    bad[40] ^= 0x55;  // inside IDAT: CRC mismatch
    auto r = decodePng(bad.data(), bad.size());
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == pq::ErrorCode::Parse);
  }
  for (size_t cut : {size_t(0), size_t(8), size_t(20), bytes.size() / 2, bytes.size() - 1})
    CHECK_FALSE(decodePng(bytes.data(), cut).ok());
}

TEST_CASE("png: decodes files written by another encoder (GDI+)") {
  const std::string dir = std::string(PQ_TEST_DATA_DIR) + "/ui/fixtures/";
  {
    auto img = readPng(dir + "gdiplus_rgb_5x3.png");  // colour type 2 (RGB)
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    REQUIRE(img->width == 5);
    REQUIRE(img->height == 3);
    for (uint32_t y = 0; y < 3; ++y)
      for (uint32_t x = 0; x < 5; ++x) {
        const uint8_t* p = img->pixel(x, y);
        CHECK(p[0] == (x * 50 + y * 7) % 256);
        CHECK(p[1] == (y * 90 + x * 3) % 256);
        CHECK(p[2] == 255 - x * 40 - y * 20);
        CHECK(p[3] == 255);
      }
  }
  {
    auto img = readPng(dir + "gdiplus_rgba_4x2.png");  // colour type 6 (RGBA)
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    REQUIRE(img->width == 4);
    REQUIRE(img->height == 2);
    for (uint32_t y = 0; y < 2; ++y)
      for (uint32_t x = 0; x < 4; ++x) {
        const uint8_t* p = img->pixel(x, y);
        CHECK(p[0] == 200 - x * 30);
        CHECK(p[1] == y * 100 + 20);
        CHECK(p[2] == x * 50);
        CHECK(p[3] == x * 60 + y * 10 + 5);
      }
  }
  CHECK_FALSE(readPng(dir + "does-not-exist.png").ok());
}

TEST_CASE("image compare: tolerance and counts") {
  Image a = noiseImage(10, 10, 4);
  Image b = a;
  ImageDiff d = compareImages(a, b, 0);
  CHECK(d.sameSize);
  CHECK(d.maxChannelDiff == 0);
  CHECK(d.pixelsOverTolerance == 0);
  CHECK(d.pixelCount == 100);
  b.pixel(3, 3)[1] = static_cast<uint8_t>(b.pixel(3, 3)[1] ^ 0x80);
  b.pixel(4, 4)[0] = static_cast<uint8_t>(a.pixel(4, 4)[0] < 255 ? a.pixel(4, 4)[0] + 1 : 254);
  d = compareImages(a, b, 2);
  CHECK(d.maxChannelDiff == 128);
  CHECK(d.pixelsOverTolerance == 1);
  b.width = 5;
  CHECK_FALSE(compareImages(a, b, 2).sameSize);
}

TEST_CASE("image compare: golden tolerance caps outliers and rejects clusters (T0.7-fix5)") {
  // 200×100 = 20,000 px → 2 outliers allowed per image at the default 100 per million.
  Image a;
  a.width = 200;
  a.height = 100;
  a.rgba.assign(size_t(a.width) * a.height * 4, 100);
  auto bump = [](Image& img, uint32_t x, uint32_t y, int by) {
    uint8_t* p = img.pixel(x, y);
    p[1] = static_cast<uint8_t>(int(p[1]) + by);
  };
  const GoldenTolerance t;
  {
    Image b = a;
    const ImageDiff d = compareImages(a, b, t.channel);
    CHECK(passesGolden(d, t));
    CHECK(d.boxX0 == d.boxX1);  // empty box
    CHECK_FALSE(passesGolden(compareImages(a, b, 3), t));  // computed with another tolerance
  }
  {
    // Noise within the tolerance everywhere, plus two isolated outliers ≤ cap: passes.
    Image b = a;
    for (uint32_t y = 0; y < b.height; ++y)
      for (uint32_t x = 0; x < b.width; ++x) bump(b, x, y, static_cast<int>((x * 7 + y * 3) % 5) - 2);
    bump(b, 10, 10, 20);
    bump(b, 150, 80, -22);  // with the noise there (-2): 24, exactly the cap
    const ImageDiff d = compareImages(a, b, t.channel);
    CHECK(d.pixelsOverTolerance == 2);
    CHECK(d.clusteredOutliers == 0);
    CHECK(d.maxChannelDiff == 24);
    CHECK(d.boxX0 == 10);
    CHECK(d.boxY0 == 10);
    CHECK(d.boxX1 == 151);
    CHECK(d.boxY1 == 81);
    CHECK(passesGolden(d, t));
    // A third isolated outlier exceeds the per-image count.
    bump(b, 100, 50, 10);
    CHECK_FALSE(passesGolden(compareImages(a, b, t.channel), t));
  }
  {
    // One isolated pixel over the cap fails on its own.
    Image b = a;
    bump(b, 50, 50, 25);
    const ImageDiff d = compareImages(a, b, t.channel);
    CHECK(d.maxChannelDiff == 25);
    CHECK(d.pixelsOverTolerance == 1);
    CHECK_FALSE(passesGolden(d, t));
  }
  {
    // Two small outliers touching diagonally: a cluster, fails even though both are ≤ cap and ≤ the count.
    Image b = a;
    bump(b, 30, 30, 5);
    bump(b, 31, 31, 5);
    const ImageDiff d = compareImages(a, b, t.channel);
    CHECK(d.pixelsOverTolerance == 2);
    CHECK(d.clusteredOutliers == 2);
    CHECK_FALSE(passesGolden(d, t));
    // Edge of the image: neighbours are only looked up inside it.
    Image c = a;
    bump(c, 0, 0, 5);
    bump(c, 199, 99, 5);
    CHECK(compareImages(a, c, t.channel).clusteredOutliers == 0);
  }
  {
    // Diff image: unchanged pixels dimmed grey, outliers red (> cap) / yellow (≤ cap), box outline magenta.
    Image b = a;
    bump(b, 20, 20, 3);    // just over the tolerance: an outlier ≤ cap
    bump(b, 40, 30, 100);  // over the cap
    bump(b, 5, 5, 1);      // within the tolerance
    const Image v = diffImage(b, a, t.channel, t.outlierCap);
    REQUIRE(v.width == a.width);
    REQUIRE(v.height == a.height);
    CHECK(v.pixel(0, 99)[0] == v.pixel(0, 99)[2]);  // grey
    CHECK(v.pixel(0, 99)[0] < 50);
    CHECK(v.pixel(5, 5)[2] == 200);  // blue: within tolerance
    CHECK(v.pixel(20, 20)[0] == v.pixel(20, 20)[1]);  // yellow
    CHECK(v.pixel(20, 20)[2] == 0);
    CHECK(v.pixel(40, 30)[1] == 0);  // red
    CHECK(v.pixel(40, 30)[0] > 128);
    for (uint32_t x = 19; x <= 41; ++x) {  // outline: box [20, 41) × [20, 31) grown by one pixel
      CHECK(v.pixel(x, 19)[1] == 0);
      CHECK(v.pixel(x, 19)[0] == 255);
      CHECK(v.pixel(x, 31)[2] == 255);
    }
    CHECK(v.pixel(19, 25)[0] == 255);
    CHECK(v.pixel(41, 25)[2] == 255);
    Image wrong = a;
    wrong.width = 100;
    CHECK(diffImage(wrong, a, 2).width == 0);
  }
}

TEST_CASE("damage: redraw only when something changed or animates") {
  Damage d;
  CHECK_FALSE(d.needsFrame());
  d.add({10, 10, 10, 20});  // empty rect: no damage
  CHECK_FALSE(d.needsFrame());
  d.add({10, 10, 20, 20});
  d.add({0, 15, 12, 30});
  CHECK(d.needsFrame());
  CHECK_FALSE(d.full());
  CHECK(d.bounds().x0 == 0);
  CHECK(d.bounds().y0 == 10);
  CHECK(d.bounds().x1 == 20);
  CHECK(d.bounds().y1 == 30);
  d.clear();
  CHECK_FALSE(d.needsFrame());
  d.addAll();
  CHECK(d.full());
  d.clear();
  d.setAnimating(true);
  CHECK(d.needsFrame());
  d.clear();
  CHECK(d.needsFrame());  // animation keeps every frame damaged
  d.setAnimating(false);
  CHECK_FALSE(d.needsFrame());
}
