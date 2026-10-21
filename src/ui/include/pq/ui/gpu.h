#pragma once

// Vulkan instance + device + queue + VMA allocator, shared by the offscreen and windowed render paths.
// Requires Vulkan 1.3 (dynamic rendering, synchronization2): every desktop GPU driver since 2022 has it.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "pq/core/result.h"

struct VmaAllocator_T;

namespace pq::ui {

struct GpuOptions {
  std::vector<std::string> instanceExtensions;  // e.g. surface extensions reported by SDL
  bool swapchain = false;                       // enable VK_KHR_swapchain on the device
  // Enable VK_LAYER_KHRONOS_validation if it is installed (also forced on by env PQ_VK_VALIDATION=1).
  // Validation errors are counted (validationErrorCount) and printed to stderr.
  bool validation = false;
};

class GpuContext {
public:
  static Result<std::unique_ptr<GpuContext>> create(const GpuOptions& options);
  ~GpuContext();
  GpuContext(const GpuContext&) = delete;
  GpuContext& operator=(const GpuContext&) = delete;

  VkInstance instance() const noexcept { return instance_; }
  VkPhysicalDevice physicalDevice() const noexcept { return physical_; }
  VkDevice device() const noexcept { return device_; }
  uint32_t queueFamily() const noexcept { return queueFamily_; }
  VkQueue queue() const noexcept { return queue_; }
  VmaAllocator_T* allocator() const noexcept { return allocator_; }
  const std::string& deviceName() const noexcept { return deviceName_; }
  bool validationEnabled() const noexcept { return debugMessenger_ != VK_NULL_HANDLE; }
  uint32_t validationErrorCount() const noexcept { return validationErrors_; }
  // Pipeline statistics queries are enabled (GpuProfiler counts fragment-shader invocations).
  bool pipelineStatistics() const noexcept { return pipelineStatistics_; }
  // Nanoseconds per timestamp tick on the graphics queue; 0 when it has no timestamps.
  float timestampPeriodNs() const noexcept { return timestampPeriodNs_; }

  // Live VMA allocations (every buffer and image the UI allocated through the allocator) and their bytes.
  // For leak tests; walks the allocator's blocks, so not for per-frame use.
  struct MemoryStats {
    uint32_t allocations = 0;
    uint64_t bytes = 0;
  };
  MemoryStats memoryStats() const noexcept;

  // Records a one-off command buffer with `record`, submits it and waits for completion. For uploads and
  // readback (not per-frame work).
  template <class F>
  Status submitAndWait(F&& record) {
    using Fn = std::remove_reference_t<F>;
    return submitAndWaitImpl([](VkCommandBuffer cb, void* user) { (*static_cast<Fn*>(user))(cb); },
                             const_cast<void*>(static_cast<const void*>(&record)));
  }

private:
  GpuContext() = default;
  Status submitAndWaitImpl(void (*fn)(VkCommandBuffer, void*), void* user);
  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT types,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  VmaAllocator_T* allocator_ = nullptr;
  VkCommandPool oneShotPool_ = VK_NULL_HANDLE;
  VkFence oneShotFence_ = VK_NULL_HANDLE;
  std::string deviceName_;
  uint32_t validationErrors_ = 0;
  bool pipelineStatistics_ = false;
  float timestampPeriodNs_ = 0.0f;
};

// GPU cost of a frame (T0.7-fix1 measurements): timestamps around the drawing and around the copy of the
// persistent target to the swapchain image, and the fragment-shader invocations of the drawing.
struct GpuFrameStats {
  double drawMs = 0;           // transfers + render pass (clears and draws)
  double copyMs = 0;           // persistent target → swapchain image (0 offscreen)
  uint64_t fragments = 0;      // fragment-shader invocations while drawing
  bool hasTimes = false;       // the queue has timestamps
  bool hasFragments = false;   // the device has pipeline statistics queries
};

// Per-frame-slot query pools for GpuFrameStats. A target calls begin() at the start of a slot's command
// buffer, drawn() after its render pass, end() after its present copy (or right after drawn()), and read()
// once the slot's fence has signalled (non-blocking; false when the results are not available).
class GpuProfiler {
public:
  static Result<std::unique_ptr<GpuProfiler>> create(GpuContext& gpu, uint32_t slots);
  ~GpuProfiler();
  GpuProfiler(const GpuProfiler&) = delete;
  GpuProfiler& operator=(const GpuProfiler&) = delete;

  void begin(VkCommandBuffer cb, uint32_t slot) noexcept;
  void drawn(VkCommandBuffer cb, uint32_t slot) noexcept;
  void end(VkCommandBuffer cb, uint32_t slot) noexcept;
  bool read(uint32_t slot, GpuFrameStats& out) noexcept;
  // The slot's next frame is recorded without queries: read() returns false for it.
  void forget(uint32_t slot) noexcept {
    if (slot < slots_) recorded_[slot] = false;
  }

private:
  GpuProfiler() = default;
  GpuContext* gpu_ = nullptr;
  VkQueryPool stamps_ = VK_NULL_HANDLE;  // 3 per slot
  VkQueryPool stats_ = VK_NULL_HANDLE;   // 1 per slot
  uint32_t slots_ = 0;
  bool recorded_[4] = {};
};

// Human-readable name for a VkResult (error messages).
const char* vkResultName(VkResult r) noexcept;

// Error helper: "<what>: VK_ERROR_…".
Error vkError(const char* what, VkResult r);

}  // namespace pq::ui
