#pragma once

// Headless render target: draws a DrawList into a VkImage (no surface, no window) and reads it back as RGBA8.
// Used by `pqdaw --screenshot` and the golden-image tests. With framesInFlight > 1, submit() pipelines frames
// like the swapchain does (per-slot command buffers and fences; waits only for the slot it reuses), for tests
// and benchmarks of streaming uploads.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string_view>

#include "pq/core/result.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/renderer.h"
#include "pq/ui/ui_scale.h"

struct VmaAllocation_T;

namespace pq::ui {

class OffscreenTarget {
public:
  static constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;  // byte order = PNG RGBA, no swizzle on readback

  // framesInFlight 1..Renderer::kMaxFramesInFlight; the renderer passed to draw/submit needs at least as many.
  static Result<std::unique_ptr<OffscreenTarget>> create(GpuContext& gpu, uint32_t width, uint32_t height,
                                                         uint32_t framesInFlight = 1);
  ~OffscreenTarget();
  OffscreenTarget(const OffscreenTarget&) = delete;
  OffscreenTarget& operator=(const OffscreenTarget&) = delete;

  uint32_t width() const noexcept { return width_; }
  uint32_t height() const noexcept { return height_; }

  // Clears to `clear`, draws `dl` (its target size must equal this target's), submits and waits.
  // `renderer` must have been created with kFormat.
  Status draw(Renderer& renderer, const DrawList& dl, Rgba clear);
  // Draws a frame (pq/ui/renderer.h FrameDesc), submits and waits. The image keeps its contents between
  // frames, so a partial frame (frame.full = false) redraws only its damage over the previous frame; the
  // first frame must be full.
  Status draw(Renderer& renderer, const FrameDesc& frame);
  // Like draw() but does not wait for the frame: it waits only for the frame slot it reuses (submitted
  // framesInFlight frames ago), as a swapchain frame loop does.
  Status submit(Renderer& renderer, const DrawList& dl, Rgba clear);
  Status submit(Renderer& renderer, const FrameDesc& frame);
  // Waits for every submitted frame.
  Status finish();
  // Copies the most recent frame back to the CPU (finishes first).
  Result<Image> read();
  // submit() calls that found their slot's previous frame still running (the CPU had to wait for the GPU).
  uint32_t slotStalls() const noexcept { return slotStalls_; }

  // GPU timestamps and fragment-shader invocation counts per frame (T0.7-fix1 measurements). After
  // enableProfiling(), lastFrameStats() finishes and returns the most recent frame's numbers.
  Status enableProfiling();
  Result<GpuFrameStats> lastFrameStats();

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
  static constexpr uint32_t kMaxSlots = Renderer::kMaxFramesInFlight;
  uint32_t framesInFlight_ = 1;
  uint32_t slot_ = 0;
  VkCommandBuffer cmd_[kMaxSlots] = {};
  VkFence fence_[kMaxSlots] = {};
  bool pending_[kMaxSlots] = {};  // a submitted frame whose fence has not been waited for
  uint32_t slotStalls_ = 0;
  uint32_t lastSlot_ = 0;
  bool drawn_ = false;
  std::unique_ptr<GpuProfiler> profiler_;
};

}  // namespace pq::ui
