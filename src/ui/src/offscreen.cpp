#include "pq/ui/offscreen.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "pq/ui/scenes.h"

namespace pq::ui {

Result<std::unique_ptr<OffscreenTarget>> OffscreenTarget::create(GpuContext& gpu, uint32_t width, uint32_t height,
                                                                 uint32_t framesInFlight) {
  if (width == 0 || height == 0 || width > 16384 || height > 16384)
    return Error{ErrorCode::InvalidArgument, "offscreen target size must be 1..16384 per side", {}};
  if (framesInFlight == 0 || framesInFlight > kMaxSlots)
    return Error{ErrorCode::InvalidArgument, "offscreen target: framesInFlight must be 1..3", {}};
  std::unique_ptr<OffscreenTarget> t(new OffscreenTarget());
  t->gpu_ = &gpu;
  t->framesInFlight_ = framesInFlight;
  t->width_ = width;
  t->height_ = height;
  const VkDevice dev = gpu.device();

  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = kFormat;
  ici.extent = {width, height, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (VkResult e = vmaCreateImage(gpu.allocator(), &ici, &aci, &t->image_, &t->imageAlloc_, nullptr); e != VK_SUCCESS)
    return vkError("vmaCreateImage(offscreen)", e);

  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = t->image_;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = kFormat;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (VkResult e = vkCreateImageView(dev, &vci, nullptr, &t->view_); e != VK_SUCCESS) return vkError("vkCreateImageView", e);

  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = VkDeviceSize(width) * height * 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo rci{};
  rci.usage = VMA_MEMORY_USAGE_AUTO;
  rci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;  // cached for CPU reads
  VmaAllocationInfo info{};
  if (VkResult e = vmaCreateBuffer(gpu.allocator(), &bci, &rci, &t->readback_, &t->readbackAlloc_, &info); e != VK_SUCCESS)
    return vkError("vmaCreateBuffer(readback)", e);
  t->readbackMapped_ = info.pMappedData;

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = gpu.queueFamily();
  if (VkResult e = vkCreateCommandPool(dev, &pci, nullptr, &t->pool_); e != VK_SUCCESS) return vkError("vkCreateCommandPool", e);
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = t->pool_;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = framesInFlight;
  if (VkResult e = vkAllocateCommandBuffers(dev, &cai, t->cmd_); e != VK_SUCCESS) return vkError("vkAllocateCommandBuffers", e);
  for (uint32_t i = 0; i < framesInFlight; ++i) {
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (VkResult e = vkCreateFence(dev, &fci, nullptr, &t->fence_[i]); e != VK_SUCCESS) return vkError("vkCreateFence", e);
  }
  return t;
}

OffscreenTarget::~OffscreenTarget() {
  if (!gpu_) return;
  const VkDevice dev = gpu_->device();
  vkDeviceWaitIdle(dev);
  for (VkFence f : fence_)
    if (f) vkDestroyFence(dev, f, nullptr);
  if (pool_) vkDestroyCommandPool(dev, pool_, nullptr);
  if (readback_) vmaDestroyBuffer(gpu_->allocator(), readback_, readbackAlloc_);
  if (view_) vkDestroyImageView(dev, view_, nullptr);
  if (image_) vmaDestroyImage(gpu_->allocator(), image_, imageAlloc_);
}

Status OffscreenTarget::draw(Renderer& renderer, const DrawList& dl, Rgba clear) {
  if (Status s = submit(renderer, dl, clear); !s) return s;
  return finish();
}

Status OffscreenTarget::finish() {
  for (uint32_t i = 0; i < framesInFlight_; ++i) {
    if (!pending_[i]) continue;
    const VkResult r = vkWaitForFences(gpu_->device(), 1, &fence_[i], VK_TRUE, UINT64_MAX);
    vkResetFences(gpu_->device(), 1, &fence_[i]);
    pending_[i] = false;
    if (r != VK_SUCCESS) return vkError("offscreen frame wait", r);
  }
  return Ok{};
}

Status OffscreenTarget::submit(Renderer& renderer, const DrawList& dl, Rgba clear) {
  if (renderer.colorFormat() != kFormat) return Error{ErrorCode::InvalidArgument, "renderer format does not match offscreen target", {}};
  if (renderer.framesInFlight() < framesInFlight_)
    return Error{ErrorCode::InvalidArgument, "renderer has fewer frame slots than the offscreen target", {}};
  if (dl.targetWidth() != width_ || dl.targetHeight() != height_)
    return Error{ErrorCode::InvalidArgument, "draw list target size does not match offscreen target", {}};
  const uint32_t slot = slot_;
  if (pending_[slot]) {
    // The slot's previous frame must be finished before its command buffer and the renderer's slot are reused.
    if (vkGetFenceStatus(gpu_->device(), fence_[slot]) != VK_SUCCESS) ++slotStalls_;
    const VkResult r = vkWaitForFences(gpu_->device(), 1, &fence_[slot], VK_TRUE, UINT64_MAX);
    vkResetFences(gpu_->device(), 1, &fence_[slot]);
    pending_[slot] = false;
    if (r != VK_SUCCESS) return vkError("offscreen frame wait", r);
  }
  if (Status s = renderer.upload(slot, dl); !s) return s;

  VkCommandBuffer cb = cmd_[slot];
  vkResetCommandBuffer(cb, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);
  renderer.recordTransfers(cb, slot);

  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;  // the previous frame's readback copy
  b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // contents are cleared anyway
  b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image_;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = 1;
  di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);

  VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  att.imageView = view_;
  att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.clearValue.color = {{redOf(clear) / 255.0f, greenOf(clear) / 255.0f, blueOf(clear) / 255.0f, alphaOf(clear) / 255.0f}};
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {width_, height_}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &att;
  vkCmdBeginRendering(cb, &ri);
  renderer.record(cb, slot, dl);
  vkCmdEndRendering(cb);

  b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  vkCmdPipelineBarrier2(cb, &di);
  vkEndCommandBuffer(cb);

  VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cbi.commandBuffer = cb;
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si.commandBufferInfoCount = 1;
  si.pCommandBufferInfos = &cbi;
  if (VkResult r = vkQueueSubmit2(gpu_->queue(), 1, &si, fence_[slot]); r != VK_SUCCESS) return vkError("offscreen submit", r);
  pending_[slot] = true;
  slot_ = (slot + 1) % framesInFlight_;
  drawn_ = true;
  return Ok{};
}

Result<Image> OffscreenTarget::read() {
  if (!drawn_) return Error{ErrorCode::InvalidArgument, "OffscreenTarget::read before draw", {}};
  if (Status s = finish(); !s) return std::move(s).error();
  const VkImage image = image_;
  const VkBuffer buffer = readback_;
  const uint32_t w = width_, h = height_;
  Status s = gpu_->submitAndWait([&](VkCommandBuffer cb) {
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    bb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    bb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    bb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = buffer;
    bb.size = VK_WHOLE_SIZE;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.bufferMemoryBarrierCount = 1;
    di.pBufferMemoryBarriers = &bb;
    vkCmdPipelineBarrier2(cb, &di);
  });
  if (!s) return std::move(s).error();
  vmaInvalidateAllocation(gpu_->allocator(), readbackAlloc_, 0, VK_WHOLE_SIZE);
  Image img;
  img.width = w;
  img.height = h;
  img.rgba.resize(size_t(w) * h * 4);
  std::memcpy(img.rgba.data(), readbackMapped_, img.rgba.size());
  return img;
}

Result<Image> renderSceneImage(std::string_view scene, uint32_t width, uint32_t height, const GpuOptions& options,
                               UiScale scale) {
  if (!hasScene(scene)) return Error{ErrorCode::NotFound, "unknown scene '" + std::string(scene) + "'", {}};
  auto gpu = GpuContext::create(options);
  if (!gpu) return std::move(gpu).error();
  auto renderer = Renderer::create(**gpu, OffscreenTarget::kFormat, 1);
  if (!renderer) return std::move(renderer).error();
  if (width == 0 || height == 0) return Error{ErrorCode::InvalidArgument, "renderSceneImage: zero size", {}};
  // Logical size → device px (at least 1 × 1).
  const uint32_t dw = static_cast<uint32_t>(std::max(1, scale.px(static_cast<float>(width))));
  const uint32_t dh = static_cast<uint32_t>(std::max(1, scale.px(static_cast<float>(height))));
  auto target = OffscreenTarget::create(**gpu, dw, dh);
  if (!target) return std::move(target).error();
  auto textures = createSceneTextures(**renderer, scale);
  if (!textures) return std::move(textures).error();
  DrawList dl;
  dl.reset(dw, dh);
  buildScene(scene, dl, *textures, 0.0);
  if (Status s = (*target)->draw(**renderer, dl, kSceneClearColor); !s) return std::move(s).error();
  auto img = (*target)->read();
  if (img && (*gpu)->validationErrorCount() != 0)
    return Error{ErrorCode::Device, std::to_string((*gpu)->validationErrorCount()) + " Vulkan validation error(s)", {}};
  return img;
}

}  // namespace pq::ui
