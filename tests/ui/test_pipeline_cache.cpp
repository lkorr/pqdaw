// T0.7-fix6: the renderer's Vulkan pipeline cache. A blob from a previous renderer on the same device is reused;
// a blob whose header names another device or driver (vendor, device id, pipelineCacheUUID), a truncated header or
// garbage is ignored (empty cache), and the renderer still works.

#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"

using namespace pq::ui;

TEST_CASE("render: the pipeline cache is reused on the same device and ignored when foreign or corrupt") {
  GpuOptions o;
  o.validation = true;
  auto gpu = GpuContext::create(o);
  REQUIRE_MESSAGE(gpu.ok(), (gpu.ok() ? "" : gpu.error().message));
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties((*gpu)->physicalDevice(), &props);

  std::vector<uint8_t> blob;
  {
    auto r = Renderer::create(**gpu, OffscreenTarget::kFormat, 1);
    REQUIRE(r.ok());
    CHECK_FALSE((*r)->pipelineCacheReused());
    blob = (*r)->pipelineCacheData();
  }
  REQUIRE(blob.size() >= 32);
  CHECK(Renderer::pipelineCacheCompatible(blob, props));

  auto drawsSomething = [&](Renderer& r) {
    auto target = OffscreenTarget::create(**gpu, 8, 8);
    REQUIRE(target.ok());
    DrawList dl;
    dl.reset(8, 8);
    dl.addRectFilled({0, 0, 8, 8}, rgba(10, 200, 30));
    REQUIRE((*target)->draw(r, dl, rgba(0, 0, 0)).ok());
    auto img = (*target)->read();
    REQUIRE(img.ok());
    CHECK(img->pixel(4, 4)[1] == 200);
  };
  {
    RendererOptions ro;
    ro.pipelineCache = blob;
    auto r = Renderer::create(**gpu, OffscreenTarget::kFormat, 1, ro);
    REQUIRE(r.ok());
    CHECK((*r)->pipelineCacheReused());
    drawsSomething(**r);
  }
  // Another driver (UUID), another device id, a truncated header, garbage: all ignored.
  std::vector<std::vector<uint8_t>> bad;
  bad.push_back(blob);
  bad.back()[16] ^= 0xFF;  // pipelineCacheUUID
  bad.push_back(blob);
  bad.back()[12] ^= 0x01;  // deviceID
  bad.push_back(std::vector<uint8_t>(blob.begin(), blob.begin() + 20));
  bad.push_back(std::vector<uint8_t>(4096, 0xAB));
  bad.push_back({});
  for (const auto& b : bad) {
    CHECK_FALSE(Renderer::pipelineCacheCompatible(b, props));
    RendererOptions ro;
    ro.pipelineCache = b;
    auto r = Renderer::create(**gpu, OffscreenTarget::kFormat, 1, ro);
    REQUIRE(r.ok());
    CHECK_FALSE((*r)->pipelineCacheReused());
    drawsSomething(**r);
  }
  CHECK((*gpu)->validationErrorCount() == 0);
}
