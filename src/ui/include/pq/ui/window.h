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
#include "pq/ui/input.h"
#include "pq/ui/renderer.h"
#include "pq/ui/widget.h"

struct SDL_Window;
union SDL_Event;
struct VmaAllocation_T;

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
  // Device pixels per SDL window coordinate (mouse positions arrive in window coordinates).
  float pixelDensity() const noexcept;
  // The UI scale of this window's display (Windows per-monitor DPI: 1.0 at 100 %, 1.5 at 150 %); the widget
  // tree's logical → device scale (UiTree::setViewport).
  float displayScale() const noexcept;

private:
  Window() = default;
  SDL_Window* window_ = nullptr;
};

// SDL adapter for the widget toolkit's input model (pq/ui/input.h): mouse motion / buttons / wheel, window
// mouse-leave, key down/up and text input become an InputEvent with positions in device pixels (window
// coordinates × pixelDensity). Returns false (out.kind = None) for events that are not input. Text events
// only arrive between SDL_StartTextInput / SDL_StopTextInput (the app toggles that with text focus).
bool translateEvent(const SDL_Event& e, float pixelDensity, InputEvent& out) noexcept;

// The desktop clipboard through SDL (UiTree::setClipboard). Needs SDL's video subsystem (a Window).
class SdlClipboard final : public Clipboard {
public:
  std::string text() override;
  void setText(std::string_view utf8) override;
};

// Partial redraw (T0.7-fix1): frames render into a persistent colour image owned by the swapchain (the
// swapchain images themselves are not guaranteed to keep their contents), which keeps the previous frame, so a
// frame may redraw only its damage (FrameDesc::full = false). The whole persistent image is then copied into
// the acquired swapchain image (one vkCmdCopyImage; measured in bench/RESULTS.md) and presented. A recreate
// (resize, out of date) discards the persistent image: preserved() is false until the next full frame.
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
  // Uploads, records, submits and presents the frame begun by beginFrame(). A partial frame needs preserved().
  Status endFrame(Renderer& renderer, const FrameDesc& frame);
  // A full frame of one transient draw list.
  Status endFrame(Renderer& renderer, const DrawList& dl, Rgba clear);
  // The persistent image holds the last frame at the current extent, so the next frame may be partial.
  // Check after beginFrame() (which may recreate the swapchain).
  bool preserved() const noexcept { return preserved_; }
  // GPU timestamps / fragment counts per frame (see GpuProfiler) while on; lastFrameStats() is the most recent
  // profiled frame whose slot has been waited for (updated in beginFrame, no extra waits).
  Status setProfiling(bool on);
  const GpuFrameStats& lastFrameStats() const noexcept { return lastStats_; }
  // Forces a recreate at the next beginFrame (e.g. after a resize event).
  void invalidate() noexcept { dirty_ = true; }

private:
  Swapchain() = default;
  Status recreate(VkExtent2D extent);
  void destroyImages() noexcept;
  Status createPersistent();

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
  // The persistent colour image frames render into (format_, extent_).
  VkImage persistent_ = VK_NULL_HANDLE;
  VmaAllocation_T* persistentAlloc_ = nullptr;
  VkImageView persistentView_ = VK_NULL_HANDLE;
  bool preserved_ = false;
  std::unique_ptr<GpuProfiler> profiler_;
  bool profiling_ = false;
  GpuFrameStats lastStats_{};
};

}  // namespace pq::ui
