#include "pq/ui/window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdlib>
#include <string>

namespace pq::ui {

// --- Window --------------------------------------------------------------------------------------------

Result<std::unique_ptr<Window>> Window::create(const WindowOptions& options) {
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    return Error{ErrorCode::Unsupported, std::string("no display: SDL video init failed: ") + SDL_GetError(), {}};
  std::unique_ptr<Window> w(new Window());
  SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  if (options.hidden) flags |= SDL_WINDOW_HIDDEN;
  w->window_ = SDL_CreateWindow(options.title.c_str(), static_cast<int>(options.width), static_cast<int>(options.height), flags);
  if (!w->window_) {
    std::string err = SDL_GetError();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return Error{ErrorCode::Unsupported, "no display: SDL_CreateWindow failed: " + err, {}};
  }
  return w;
}

Window::~Window() {
  if (window_) {
    SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
}

uint32_t Window::id() const noexcept { return SDL_GetWindowID(window_); }

std::vector<std::string> Window::requiredInstanceExtensions() const {
  Uint32 n = 0;
  const char* const* names = SDL_Vulkan_GetInstanceExtensions(&n);
  std::vector<std::string> out;
  for (Uint32 i = 0; names && i < n; ++i) out.emplace_back(names[i]);
  return out;
}

void Window::pixelSize(uint32_t& w, uint32_t& h) const noexcept {
  int iw = 0, ih = 0;
  if (!SDL_GetWindowSizeInPixels(window_, &iw, &ih)) iw = ih = 0;
  w = static_cast<uint32_t>(std::max(iw, 0));
  h = static_cast<uint32_t>(std::max(ih, 0));
}

bool Window::minimized() const noexcept { return (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0; }

float Window::pixelDensity() const noexcept {
  const float d = SDL_GetWindowPixelDensity(window_);
  return d > 0.0f ? d : 1.0f;
}

float Window::displayScale() const noexcept {
  const float s = SDL_GetWindowDisplayScale(window_);
  return s > 0.0f ? s : 1.0f;
}

// --- SDL input adapter -----------------------------------------------------------------------------------

// pq::ui::Key / Scancode / PointerButton use SDL3's numbering, so they translate by copying.
static_assert(SDLK_A == 'a' && SDLK_0 == '0' && SDLK_GRAVE == '`' && SDLK_SPACE == key::kSpace);
static_assert(SDLK_RETURN == key::kReturn && SDLK_ESCAPE == key::kEscape && SDLK_BACKSPACE == key::kBackspace);
static_assert(SDLK_TAB == key::kTab && SDLK_DELETE == key::kDelete && SDLK_CAPSLOCK == key::kCapsLock);
static_assert(SDLK_F1 == key::kF1 && SDLK_F12 == key::kF1 + 11 && SDLK_UP == key::kUp && SDLK_PAGEDOWN == key::kPageDown);
static_assert(SDLK_LCTRL == key::kLCtrl && SDLK_RGUI == key::kRSuper && SDLK_HOME == key::kHome);
static_assert(SDL_SCANCODE_A == static_cast<int>(Scancode::A) && SDL_SCANCODE_0 == static_cast<int>(Scancode::Num0));
static_assert(SDL_SCANCODE_F1 == static_cast<int>(Scancode::F1) && SDL_SCANCODE_UP == static_cast<int>(Scancode::Up));
static_assert(SDL_SCANCODE_LCTRL == static_cast<int>(Scancode::LCtrl) && SDL_SCANCODE_RGUI == static_cast<int>(Scancode::RSuper));
static_assert(SDL_BUTTON_LEFT == 1 && SDL_BUTTON_MIDDLE == 2 && SDL_BUTTON_RIGHT == 3 && SDL_BUTTON_X2 == 5);

namespace {

Mods modsFromSdl(SDL_Keymod m) noexcept {
  uint32_t r = mod::kNone;
  if (m & SDL_KMOD_SHIFT) r |= mod::kShift;
  if (m & SDL_KMOD_CTRL) r |= mod::kCtrl;
  if (m & SDL_KMOD_ALT) r |= mod::kAlt;
  if (m & SDL_KMOD_GUI) r |= mod::kSuper;
  if (m & SDL_KMOD_CAPS) r |= mod::kCapsLock;
  return static_cast<Mods>(r);
}

}  // namespace

std::string SdlClipboard::text() {
  char* t = SDL_GetClipboardText();  // "" (never null) when empty or on error
  std::string out = t ? t : "";
  SDL_free(t);
  return out;
}

void SdlClipboard::setText(std::string_view utf8) { SDL_SetClipboardText(std::string(utf8).c_str()); }

bool translateEvent(const SDL_Event& e, float density, InputEvent& out) noexcept {
  out = InputEvent{};
  PointerEvent& p = out.pointer;
  switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
      out.kind = InputKind::Pointer;
      p.type = PointerType::Move;
      p.pos = {e.motion.x * density, e.motion.y * density};
      p.mods = modsFromSdl(SDL_GetModState());
      return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      out.kind = InputKind::Pointer;
      p.type = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? PointerType::Down : PointerType::Up;
      p.button = e.button.button <= SDL_BUTTON_X2 ? static_cast<PointerButton>(e.button.button) : PointerButton::None;
      p.clickCount = e.button.clicks;
      p.pos = {e.button.x * density, e.button.y * density};
      p.mods = modsFromSdl(SDL_GetModState());
      return true;
    case SDL_EVENT_MOUSE_WHEEL: {
      out.kind = InputKind::Pointer;
      p.type = PointerType::Wheel;
      const float flip = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
      p.wheel = {e.wheel.x * flip, e.wheel.y * flip};
      p.pos = {e.wheel.mouse_x * density, e.wheel.mouse_y * density};
      p.mods = modsFromSdl(SDL_GetModState());
      return true;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      out.kind = InputKind::Pointer;
      p.type = PointerType::Leave;
      return true;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      out.kind = InputKind::Key;
      out.key.down = e.type == SDL_EVENT_KEY_DOWN;
      out.key.repeat = e.key.repeat;
      out.key.mods = modsFromSdl(e.key.mod);
      out.key.scancode = static_cast<Scancode>(e.key.scancode);
      out.key.key = static_cast<Key>(e.key.key);
      return true;
    case SDL_EVENT_TEXT_INPUT:
      out.kind = InputKind::Text;
      out.text = makeTextEvent(e.text.text ? e.text.text : "");
      return true;
    default: return false;
  }
}

// --- Swapchain -----------------------------------------------------------------------------------------

Result<std::unique_ptr<Swapchain>> Swapchain::create(GpuContext& gpu, Window& window, uint32_t framesInFlight) {
  if (framesInFlight == 0 || framesInFlight > kMaxSlots)
    return Error{ErrorCode::InvalidArgument, "Swapchain: framesInFlight must be 1..3", {}};
  std::unique_ptr<Swapchain> sc(new Swapchain());
  sc->gpu_ = &gpu;
  sc->window_ = &window;
  sc->framesInFlight_ = framesInFlight;
  if (!SDL_Vulkan_CreateSurface(window.sdl(), gpu.instance(), nullptr, &sc->surface_))
    return Error{ErrorCode::Device, std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError(), {}};

  const VkPhysicalDevice pd = gpu.physicalDevice();
  VkBool32 presentable = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(pd, gpu.queueFamily(), sc->surface_, &presentable);
  if (!presentable) return Error{ErrorCode::Unsupported, "the graphics queue cannot present to this window", {}};

  uint32_t nf = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(pd, sc->surface_, &nf, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(nf);
  vkGetPhysicalDeviceSurfaceFormatsKHR(pd, sc->surface_, &nf, formats.data());
  if (formats.empty()) return Error{ErrorCode::Unsupported, "surface reports no formats", {}};
  // UNORM (not SRGB): colours and blending stay in gamma space, matching the offscreen path and PQ Meter's GL.
  const VkFormat preferred[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
  sc->format_ = VK_FORMAT_UNDEFINED;
  for (VkFormat want : preferred) {
    for (const auto& f : formats)
      if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        sc->format_ = f.format;
        sc->colorSpace_ = f.colorSpace;
        break;
      }
    if (sc->format_ != VK_FORMAT_UNDEFINED) break;
  }
  if (sc->format_ == VK_FORMAT_UNDEFINED) {
    sc->format_ = formats[0].format;
    sc->colorSpace_ = formats[0].colorSpace;
  }

  // Non-blocking present so pacing is ours (DwmFlush), never a driver vsync spin: MAILBOX, then IMMEDIATE,
  // then FIFO (always available) as the fallback.
  uint32_t nm = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(pd, sc->surface_, &nm, nullptr);
  std::vector<VkPresentModeKHR> modes(nm);
  vkGetPhysicalDeviceSurfacePresentModesKHR(pd, sc->surface_, &nm, modes.data());
  sc->presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
  std::vector<VkPresentModeKHR> order = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
  // PQ_PRESENT_MODE=fifo|immediate|mailbox forces a mode for A/B measurements (like PQ Meter's PQ_SWAP).
  if (const char* env = std::getenv("PQ_PRESENT_MODE")) {
    const std::string m = env;
    if (m == "fifo") order.clear();
    if (m == "immediate") order = {VK_PRESENT_MODE_IMMEDIATE_KHR};
    if (m == "mailbox") order = {VK_PRESENT_MODE_MAILBOX_KHR};
  }
  for (VkPresentModeKHR want : order) {
    if (std::find(modes.begin(), modes.end(), want) != modes.end()) {
      sc->presentMode_ = want;
      break;
    }
  }

  const VkDevice dev = gpu.device();
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = gpu.queueFamily();
  if (VkResult e = vkCreateCommandPool(dev, &pci, nullptr, &sc->pool_); e != VK_SUCCESS) return vkError("vkCreateCommandPool", e);
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = sc->pool_;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = framesInFlight;
  if (VkResult e = vkAllocateCommandBuffers(dev, &cai, sc->cmd_); e != VK_SUCCESS) return vkError("vkAllocateCommandBuffers", e);
  for (uint32_t i = 0; i < framesInFlight; ++i) {
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (VkResult e = vkCreateFence(dev, &fci, nullptr, &sc->inFlight_[i]); e != VK_SUCCESS) return vkError("vkCreateFence", e);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (VkResult e = vkCreateSemaphore(dev, &sci, nullptr, &sc->acquired_[i]); e != VK_SUCCESS) return vkError("vkCreateSemaphore", e);
  }
  sc->dirty_ = true;  // the first beginFrame creates the swapchain at the then-current size
  return sc;
}

Swapchain::~Swapchain() {
  if (!gpu_) return;
  const VkDevice dev = gpu_->device();
  vkDeviceWaitIdle(dev);
  destroyImages();
  if (swapchain_) vkDestroySwapchainKHR(dev, swapchain_, nullptr);
  for (uint32_t i = 0; i < kMaxSlots; ++i) {
    if (inFlight_[i]) vkDestroyFence(dev, inFlight_[i], nullptr);
    if (acquired_[i]) vkDestroySemaphore(dev, acquired_[i], nullptr);
  }
  if (pool_) vkDestroyCommandPool(dev, pool_, nullptr);
  if (surface_) SDL_Vulkan_DestroySurface(gpu_->instance(), surface_, nullptr);
}

void Swapchain::destroyImages() noexcept {
  const VkDevice dev = gpu_->device();
  for (VkImageView v : views_) vkDestroyImageView(dev, v, nullptr);
  for (VkSemaphore s : renderDone_) vkDestroySemaphore(dev, s, nullptr);
  views_.clear();
  renderDone_.clear();
  images_.clear();
}

Status Swapchain::recreate(VkExtent2D extent) {
  const VkDevice dev = gpu_->device();
  vkDeviceWaitIdle(dev);  // resize is rare: simplest correct way to retire the old images and semaphores
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_->physicalDevice(), surface_, &caps);
  uint32_t count = caps.minImageCount + 1;
  if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);

  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = surface_;
  ci.minImageCount = count;
  ci.imageFormat = format_;
  ci.imageColorSpace = colorSpace_;
  ci.imageExtent = extent;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                       : caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
    for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                          VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
      if (caps.supportedCompositeAlpha & a) {
        ci.compositeAlpha = a;
        break;
      }
  ci.presentMode = presentMode_;
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = swapchain_;
  VkSwapchainKHR next = VK_NULL_HANDLE;
  if (VkResult e = vkCreateSwapchainKHR(dev, &ci, nullptr, &next); e != VK_SUCCESS) return vkError("vkCreateSwapchainKHR", e);
  destroyImages();
  if (swapchain_) vkDestroySwapchainKHR(dev, swapchain_, nullptr);
  swapchain_ = next;
  extent_ = extent;

  uint32_t n = 0;
  vkGetSwapchainImagesKHR(dev, swapchain_, &n, nullptr);
  images_.resize(n);
  vkGetSwapchainImagesKHR(dev, swapchain_, &n, images_.data());
  views_.resize(n, VK_NULL_HANDLE);
  renderDone_.resize(n, VK_NULL_HANDLE);
  for (uint32_t i = 0; i < n; ++i) {
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = images_[i];
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format_;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (VkResult e = vkCreateImageView(dev, &vci, nullptr, &views_[i]); e != VK_SUCCESS) return vkError("vkCreateImageView", e);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (VkResult e = vkCreateSemaphore(dev, &sci, nullptr, &renderDone_[i]); e != VK_SUCCESS) return vkError("vkCreateSemaphore", e);
  }
  dirty_ = false;
  ++recreateCount_;
  return Ok{};
}

Result<Swapchain::FrameStatus> Swapchain::beginFrame(VkExtent2D& extent) {
  if (frameOpen_) return Error{ErrorCode::InvalidArgument, "beginFrame called twice without endFrame", {}};
  if (window_->minimized()) return FrameStatus::Skipped;
  const VkDevice dev = gpu_->device();

  for (int attempt = 0; attempt < 2; ++attempt) {
    VkSurfaceCapabilitiesKHR caps;
    if (VkResult e = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_->physicalDevice(), surface_, &caps); e != VK_SUCCESS)
      return vkError("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", e);
    VkExtent2D want = caps.currentExtent;
    if (want.width == UINT32_MAX) {  // surface size is defined by the swapchain: use the window's
      uint32_t w = 0, h = 0;
      window_->pixelSize(w, h);
      want = {std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width),
              std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height)};
    }
    if (want.width == 0 || want.height == 0) return FrameStatus::Skipped;
    if (dirty_ || !swapchain_ || want.width != extent_.width || want.height != extent_.height) {
      if (Status s = recreate(want); !s) return std::move(s).error();
    }

    if (VkResult e = vkWaitForFences(dev, 1, &inFlight_[slot_], VK_TRUE, UINT64_MAX); e != VK_SUCCESS)
      return vkError("vkWaitForFences", e);
    const VkResult r = vkAcquireNextImageKHR(dev, swapchain_, UINT64_MAX, acquired_[slot_], VK_NULL_HANDLE, &imageIndex_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
      dirty_ = true;  // retry once with a fresh swapchain
      continue;
    }
    if (r == VK_SUBOPTIMAL_KHR) {
      dirty_ = true;  // usable this frame; recreate next time
    } else if (r != VK_SUCCESS) {
      return vkError("vkAcquireNextImageKHR", r);
    }
    frameOpen_ = true;
    extent = extent_;
    return FrameStatus::Ready;
  }
  return FrameStatus::Skipped;  // still out of date after a recreate (mid-resize): try again next event
}

Status Swapchain::endFrame(Renderer& renderer, const DrawList& dl, Rgba clear) {
  if (!frameOpen_) return Error{ErrorCode::InvalidArgument, "endFrame without a successful beginFrame", {}};
  frameOpen_ = false;
  if (renderer.colorFormat() != format_ || renderer.framesInFlight() < framesInFlight_)
    return Error{ErrorCode::InvalidArgument, "renderer format / frame slots do not match the swapchain", {}};
  if (dl.targetWidth() != extent_.width || dl.targetHeight() != extent_.height)
    return Error{ErrorCode::InvalidArgument, "draw list size does not match the swapchain extent", {}};
  const VkDevice dev = gpu_->device();
  if (Status s = renderer.upload(slot_, dl); !s) return s;

  VkCommandBuffer cb = cmd_[slot_];
  vkResetCommandBuffer(cb, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);
  renderer.recordTransfers(cb, slot_);  // texture / glyph uploads of this frame, ahead of the render pass
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;  // chained to the acquire semaphore wait
  b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = images_[imageIndex_];
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = 1;
  di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);

  VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  att.imageView = views_[imageIndex_];
  att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.clearValue.color = {{redOf(clear) / 255.0f, greenOf(clear) / 255.0f, blueOf(clear) / 255.0f, alphaOf(clear) / 255.0f}};
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, extent_};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &att;
  vkCmdBeginRendering(cb, &ri);
  renderer.record(cb, slot_, dl);
  vkCmdEndRendering(cb);

  b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
  b.dstAccessMask = VK_ACCESS_2_NONE;
  b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  vkCmdPipelineBarrier2(cb, &di);
  vkEndCommandBuffer(cb);

  VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  wait.semaphore = acquired_[slot_];
  wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signal.semaphore = renderDone_[imageIndex_];
  signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cbi.commandBuffer = cb;
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si.waitSemaphoreInfoCount = 1;
  si.pWaitSemaphoreInfos = &wait;
  si.commandBufferInfoCount = 1;
  si.pCommandBufferInfos = &cbi;
  si.signalSemaphoreInfoCount = 1;
  si.pSignalSemaphoreInfos = &signal;
  vkResetFences(dev, 1, &inFlight_[slot_]);  // only now: an aborted frame must leave the fence signalled
  if (VkResult e = vkQueueSubmit2(gpu_->queue(), 1, &si, inFlight_[slot_]); e != VK_SUCCESS) return vkError("vkQueueSubmit2", e);

  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &renderDone_[imageIndex_];
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain_;
  pi.pImageIndices = &imageIndex_;
  const VkResult r = vkQueuePresentKHR(gpu_->queue(), &pi);
  slot_ = (slot_ + 1) % framesInFlight_;
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
    dirty_ = true;
  } else if (r != VK_SUCCESS) {
    return vkError("vkQueuePresentKHR", r);
  }
  return Ok{};
}

}  // namespace pq::ui
