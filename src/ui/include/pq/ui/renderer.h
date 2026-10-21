#pragma once

// Draw-list renderer: two pipelines (alpha and additive blending) over one shader (vertex colour × texel),
// a texture table (kAtlasTexture + glyph atlas pages + user textures, see draw_list.h) and per-frame-slot
// persistently mapped vertex/index buffers. "Textured drawing" is any command whose texture is not an atlas
// page; it uses the same pipelines with a different descriptor set, so a frame's pipeline binds are bounded by
// its blend-mode switches.
//
// Uploads never wait for the GPU (T0.8-fix1 / T0.7-fix4). Texture data (createTexture, updateTexture, glyphs
// the text system added to the atlas) is copied into a staging ring of persistently mapped chunks at once and
// queued; the next frame's upload(slot) binds the queued copies and chunks to that frame, recordTransfers()
// records them (buffer → image copies between two batched barriers) into the frame's own command buffer ahead
// of its render pass, and the chunks return to the ring when the slot comes round again (the caller's per-slot
// fence wait proves that frame finished). destroyTexture() is deferred the same way: the image is freed, and
// its id reused, once every frame slot that might still sample it has been recycled.
//
// Frame protocol (Swapchain::endFrame, OffscreenTarget::submit):
//   wait slot fence → upload(slot, dl) → begin cb → recordTransfers(cb, slot) → begin rendering →
//   record(cb, slot, dl) → end rendering → submit (signals the slot fence).
// Slots must be used round-robin with the same framesInFlight for the renderer's lifetime.
//
// The renderer does not own a render target: callers (OffscreenTarget, Swapchain) begin dynamic rendering
// on an image of colorFormat() and call record().

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "pq/core/result.h"
#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"

struct VmaAllocation_T;

namespace pq::ui {

enum class TextureFilter : uint8_t { Linear, Nearest };

// Texel rectangle of a texture: [x, x + w) × [y, y + h).
struct TextureRect {
  uint32_t x = 0, y = 0, w = 0, h = 0;
};

class Renderer {
public:
  static constexpr uint32_t kMaxFramesInFlight = 3;
  static constexpr uint32_t kMaxTextures = 256;  // table size, including kAtlasTexture and the atlas pages
  static constexpr uint32_t kMaxUserTextures = kMaxTextures - kFirstUserTexture;
  static constexpr VkDeviceSize kStagingChunkBytes = VkDeviceSize(1) << 20;  // larger uploads get their own chunk

  static Result<std::unique_ptr<Renderer>> create(GpuContext& gpu, VkFormat colorFormat, uint32_t framesInFlight);
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  VkFormat colorFormat() const noexcept { return colorFormat_; }
  // The glyph atlas behind kAtlasTexture / atlasPageTexture(p). The text system packs glyphs into it (UI thread).
  const std::shared_ptr<Atlas>& atlas() const noexcept { return atlas_; }
  uint32_t framesInFlight() const noexcept { return framesInFlight_; }

  // Creates an RGBA8 texture and queues its pixels for the next frame (no GPU wait). The id can be drawn from
  // the next frame on. Ids of destroyed textures are reused.
  Result<TextureId> createTexture(const Image& img, TextureFilter filter);
  // Queues `rect` of texture `id` to be overwritten with `rgba` (rect.w × rect.h RGBA8 texels, rows rowPitch
  // bytes apart; 0 = tightly packed). The bytes are copied into the staging ring now and reach the texture
  // before the next frame's draws; frames already submitted keep sampling the old texels. Never waits.
  Status updateTexture(TextureId id, const TextureRect& rect, std::span<const uint8_t> rgba, uint32_t rowPitch = 0);
  // Destroys a texture once every frame submitted so far has finished (deferred by the frame-slot fences);
  // queued uploads to it are dropped. Draw lists must not use the id afterwards: it is reused by createTexture.
  Status destroyTexture(TextureId id);
  // Textures created and not destroyed, and destroyed ones still waiting for their frames to finish.
  uint32_t liveTextures() const noexcept { return liveTextures_; }
  uint32_t retiringTextures() const noexcept { return static_cast<uint32_t>(retiring_.size()); }

  // Starts frame slot `slot`. The GPU must be done with the slot's previous frame (the caller's per-slot fence
  // guarantees it). Recycles that frame's staging chunks and finished deferred destroys, stages the atlas's
  // changed rectangles, binds every queued upload to this frame, and copies the draw list's vertices and indices
  // into the slot's buffers (growing them if needed).
  Status upload(uint32_t slot, const DrawList& dl);
  // Records the texture uploads bound by upload(slot): one batched barrier to TRANSFER_DST, the buffer → image
  // copies, one batched barrier back to SHADER_READ_ONLY. Call it outside rendering, before record().
  void recordTransfers(VkCommandBuffer cb, uint32_t slot) noexcept;
  // Records the draw list into `cb`, which must be inside vkCmdBeginRendering on a colorFormat() target of
  // the draw list's target size, using the buffers filled by upload(slot). Pipeline / descriptor / scissor
  // binds are issued only when the command's state actually changes.
  void record(VkCommandBuffer cb, uint32_t slot, const DrawList& dl) const noexcept;

  struct Stats {
    uint64_t framesUploaded = 0;
    uint64_t copiesRecorded = 0;   // buffer → image regions
    uint64_t bytesStaged = 0;      // through the staging ring
    uint64_t atlasBytesStaged = 0; // ... of which glyph atlas pages
    uint32_t stagingChunks = 0;    // chunks currently allocated (in flight + free)
    VkDeviceSize stagingBytes = 0; // their total size
  };
  const Stats& stats() const noexcept { return stats_; }
  // GPU memory of the glyph atlas pages (VMA allocation sizes).
  VkDeviceSize atlasGpuBytes() const noexcept;

private:
  Renderer() = default;
  Status createPipelines();

  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation_T* alloc = nullptr;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
  };
  enum class TexState : uint8_t { Free, Live, Retiring };
  struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation_T* alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;  // kept across reuse of the slot
    uint32_t width = 0, height = 0;
    uint32_t bytesPerTexel = 4;
    TexState state = TexState::Free;
    bool initialized = false;  // first upload recorded: the image is in SHADER_READ_ONLY_OPTIMAL
    std::array<uint64_t, kMaxFramesInFlight> retireAfter{};  // per-slot frame serials at destroy time
  };
  struct Chunk {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation_T* alloc = nullptr;
    uint8_t* mapped = nullptr;
    VkDeviceSize size = 0;
    VkDeviceSize used = 0;
  };
  struct Copy {
    uint32_t tex;
    uint32_t batch;  // copies to overlapping rects of one texture go in later batches (ordered by a barrier)
    VkBuffer buffer;
    VkDeviceSize offset;
    uint32_t x, y, w, h;
  };

  Status ensureBuffer(Buffer& b, VkDeviceSize need, VkBufferUsageFlags usage);
  void destroyBuffer(Buffer& b) noexcept;
  Status createImage(Texture& t, uint32_t w, uint32_t h, bool coverage, TextureFilter filter);
  void destroyImage(Texture& t) noexcept;
  // Reserves `bytes` in the open staging chunk (16-byte aligned). nullptr when a chunk cannot be allocated.
  uint8_t* stage(VkDeviceSize bytes, VkBuffer& buffer, VkDeviceSize& offset);
  void queueCopy(const Copy& c);
  void releaseChunk(uint32_t index) noexcept;
  void retireSlot(uint32_t slot) noexcept;
  Status ensureAtlasPages();  // a GPU image per atlas page (kAtlasTexture shares page 0's descriptor set)
  Status stageAtlas();        // ensureAtlasPages + queue each page's dirty rect

  GpuContext* gpu_ = nullptr;
  VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
  uint32_t framesInFlight_ = 1;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool descPool_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  std::array<VkPipeline, kBlendModeCount> pipelines_{};
  VkSampler samplers_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

  std::array<Texture, kMaxTextures> textures_{};
  uint32_t nextTextureId_ = kFirstUserTexture;  // never-used ids start here
  std::vector<uint32_t> freeIds_;
  std::vector<uint32_t> retiring_;
  uint32_t liveTextures_ = 0;

  std::shared_ptr<Atlas> atlas_;
  uint32_t atlasPagesCreated_ = 0;

  // Staging ring.
  std::vector<Chunk> chunks_;  // a destroyed chunk leaves a hole (buffer == null) for the next one
  std::vector<uint32_t> freeChunks_;
  std::vector<uint32_t> openChunks_;  // chunks holding queued copies; the last one is being filled
  std::array<std::vector<uint32_t>, kMaxFramesInFlight> slotChunks_;
  std::vector<Copy> queued_;
  std::array<std::vector<Copy>, kMaxFramesInFlight> slotCopies_;
  std::vector<VkImageMemoryBarrier2> barriers_;  // recordTransfers scratch
  std::vector<VkBufferImageCopy> regions_;
  uint64_t serial_ = 0;
  std::array<uint64_t, kMaxFramesInFlight> slotSerial_{};

  std::array<Buffer, kMaxFramesInFlight> vertexBuffers_{};
  std::array<Buffer, kMaxFramesInFlight> indexBuffers_{};
  Stats stats_;
};

}  // namespace pq::ui
