#pragma once

// SDL3 window + Vulkan surface/swapchain. The frame loop itself (events, damage, pacing) lives in app/.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/renderer.h"

struct SDL_Window;

namespace pq::ui {

struct WindowOptions {
  std::string title = "pqdaw";
  uint32_t width = 1280;  // logical size
  uint32_t height = 800;
  bool hidden = false;
};

class Window {
public:
  // Initialises SDL's video subsystem and opens a resizable Vulkan-capable window. Fails with
  // ErrorCode::Unsupported when there is no display (headless session), so callers can skip cleanly.
  static Result<std::unique_ptr<Window>> create(const WindowOptions& options);
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  SDL_Window* sdl() const noexcept { return window_; }
  uint32_t id() const noexcept;
  // Instance extensions SDL needs for vkCreate*SurfaceKHR.
  std::vector<std::string> requiredInstanceExtensions() const;
  // Drawable size in pixels (0×0 while minimised on Windows).
  void pixelSize(uint32_t& w, uint32_t& h) const noexcept;
  bool minimized() const noexcept;

private:
  Window() = default;
  SDL_Window* window_ = nullptr;
};

class Swapchain {
public:
  static Result<std::unique_ptr<Swapchain>> create(GpuContext& gpu, Window& window, uint32_t framesInFlight = 2);
  ~Swapchain();
  Swapchain(const Swapchain&) = delete;
  Swapchain& operator=(const Swapchain&) = delete;

  VkFormat format() const noexcept { return format_; }
  VkPresentModeKHR presentMode() const noexcept { return presentMode_; }
  uint32_t framesInFlight() const noexcept { return framesInFlight_; }
  uint32_t recreateCount() const noexcept { return recreateCount_; }

  enum class FrameStatus { Ready, Skipped };
  // Waits for the next frame slot and acquires a swapchain image, recreating the swapchain when the surface
  // size changed or it went out of date. Skipped = nothing to draw into (minimised / zero-size); the caller
  // just goes back to waiting for events. On Ready, `extent` is the size to build the draw list for.
  Result<FrameStatus> beginFrame(VkExtent2D& extent);
  // Uploads, records (clear + draw list), submits and presents the frame begun by beginFrame().
  Status endFrame(Renderer& renderer, const DrawList& dl, Rgba clear);
  // Forces a recreate at the next beginFrame (e.g. after a resize event).
  void invalidate() noexcept { dirty_ = true; }

private:
  Swapchain() = default;
  Status recreate(VkExtent2D extent);
  void destroyImages() noexcept;

  GpuContext* gpu_ = nullptr;
  Window* window_ = nullptr;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkColorSpaceKHR colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
  VkExtent2D extent_{0, 0};
  std::vector<VkImage> images_;
  std::vector<VkImageView> views_;
  std::vector<VkSemaphore> renderDone_;  // per swapchain image (reuse is only safe once that image is re-acquired)

  static constexpr uint32_t kMaxSlots = Renderer::kMaxFramesInFlight;
  uint32_t framesInFlight_ = 2;
  uint32_t slot_ = 0;
  uint32_t imageIndex_ = 0;
  bool frameOpen_ = false;
  bool dirty_ = false;
  uint32_t recreateCount_ = 0;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_[kMaxSlots] = {};
  VkFence inFlight_[kMaxSlots] = {};
  VkSemaphore acquired_[kMaxSlots] = {};
};

}  // namespace pq::ui
