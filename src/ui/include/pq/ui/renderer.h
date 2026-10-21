#pragma once

// Draw-list renderer: two pipelines (alpha and additive blending) over one shader (vertex colour × texel),
// a texture table (slot 0 = built-in atlas), and per-frame-slot persistently mapped vertex/index buffers.
// "Textured drawing" is any command whose texture is not the atlas; it uses the same pipelines with a
// different descriptor set, so a frame's pipeline binds are bounded by its blend-mode switches.
//
// The renderer owns the atlas page (texture 0, pq/ui/atlas.h). Glyphs added to it by the text system between
// frames are uploaded by upload() (only the changed rows, and only when something changed).
//
// The renderer does not own a render target: callers (OffscreenTarget, Swapchain) begin dynamic rendering
// on an image of colorFormat() and call record().

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "pq/core/result.h"
#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"

struct VmaAllocation_T;

namespace pq::ui {

enum class TextureFilter : uint8_t { Linear, Nearest };

class Renderer {
public:
  static constexpr uint32_t kMaxFramesInFlight = 3;
  static constexpr uint32_t kMaxTextures = 256;

  static Result<std::unique_ptr<Renderer>> create(GpuContext& gpu, VkFormat colorFormat, uint32_t framesInFlight);
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  VkFormat colorFormat() const noexcept { return colorFormat_; }
  // The atlas page behind kAtlasTexture. The text system packs glyphs into it (UI thread).
  const std::shared_ptr<Atlas>& atlas() const noexcept { return atlas_; }
  uint32_t framesInFlight() const noexcept { return framesInFlight_; }

  // Uploads an RGBA8 texture (blocking: staging copy + wait). Not for per-frame use.
  Result<TextureId> createTexture(const Image& img, TextureFilter filter);

  // Copies the draw list's vertices and indices into frame slot `slot` (growing its buffers if needed).
  // The GPU must be done with the slot's previous frame (the caller's per-slot fence guarantees it).
  // If the atlas changed since the last upload, its changed rows are copied to texture 0 first; that waits for
  // the device to go idle (other frames in flight may be sampling the page), which only happens on the frames
  // that introduce new glyphs.
  Status upload(uint32_t slot, const DrawList& dl);

  // Records the draw list into `cb`, which must be inside vkCmdBeginRendering on a colorFormat() target of
  // the draw list's target size, using the buffers filled by upload(slot). Pipeline / descriptor / scissor
  // binds are issued only when the command's state actually changes.
  void record(VkCommandBuffer cb, uint32_t slot, const DrawList& dl) const noexcept;

private:
  Renderer() = default;
  Status createPipelines();
  Status uploadAtlasRows(uint32_t y0, uint32_t y1);

  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation_T* alloc = nullptr;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
  };
  struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation_T* alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
  };
  Status ensureBuffer(Buffer& b, VkDeviceSize need, VkBufferUsageFlags usage);
  void destroyBuffer(Buffer& b) noexcept;

  GpuContext* gpu_ = nullptr;
  VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
  uint32_t framesInFlight_ = 1;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool descPool_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  std::array<VkPipeline, kBlendModeCount> pipelines_{};
  VkSampler samplers_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  std::vector<Texture> textures_;
  std::shared_ptr<Atlas> atlas_;
  uint64_t atlasUploadedVersion_ = 0;
  std::array<Buffer, kMaxFramesInFlight> vertexBuffers_{};
  std::array<Buffer, kMaxFramesInFlight> indexBuffers_{};
};

}  // namespace pq::ui
