#pragma once

// Headless render target: draws a DrawList into a VkImage (no surface, no window) and reads it back as RGBA8.
// Used by `pqdaw --screenshot` and the golden-image tests.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string_view>

#include "pq/core/result.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/renderer.h"

struct VmaAllocation_T;

namespace pq::ui {

class OffscreenTarget {
public:
  static constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;  // byte order = PNG RGBA, no swizzle on readback

  static Result<std::unique_ptr<OffscreenTarget>> create(GpuContext& gpu, uint32_t width, uint32_t height);
  ~OffscreenTarget();
  OffscreenTarget(const OffscreenTarget&) = delete;
  OffscreenTarget& operator=(const OffscreenTarget&) = delete;

  uint32_t width() const noexcept { return width_; }
  uint32_t height() const noexcept { return height_; }

  // Clears to `clear`, draws `dl` (its target size must equal this target's), submits and waits.
  // `renderer` must have been created with kFormat.
  Status draw(Renderer& renderer, const DrawList& dl, Rgba clear);
  // Copies the most recent draw() back to the CPU.
  Result<Image> read();

private:
  OffscreenTarget() = default;
  GpuContext* gpu_ = nullptr;
  uint32_t width_ = 0, height_ = 0;
  VkImage image_ = VK_NULL_HANDLE;
  VmaAllocation_T* imageAlloc_ = nullptr;
  VkImageView view_ = VK_NULL_HANDLE;
  VkBuffer readback_ = VK_NULL_HANDLE;
  VmaAllocation_T* readbackAlloc_ = nullptr;
  void* readbackMapped_ = nullptr;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool drawn_ = false;
};

// One-call headless screenshot: creates a GPU context, renderer and target, builds `scene` (see scenes.h)
// at width×height and returns the pixels. Fails for unknown scene names or if no Vulkan 1.3 device exists.
Result<Image> renderSceneImage(std::string_view scene, uint32_t width, uint32_t height, const GpuOptions& options = {});

}  // namespace pq::ui
