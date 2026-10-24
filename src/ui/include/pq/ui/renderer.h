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
//   wait slot fence → upload(slot, layers) → begin cb → recordTransfers(cb, slot) → begin rendering →
//   record(cb, slot, frame) → end rendering → submit (signals the slot fence).
// Slots must be used round-robin with the same framesInFlight for the renderer's lifetime.
//
// The renderer does not own a render target: callers (OffscreenTarget, Swapchain) begin dynamic rendering
// on an image of colorFormat() and call record().
//
// Partial redraw (T0.7-fix1). Targets keep their colour image between frames (LOAD), so a frame may redraw
// only its damage: FrameDesc::full = false with disjoint damage rects. record() then clears each rect to the
// frame's clear colour and draws every command of every layer once per rect it intersects, scissored to
// (command clip ∩ rect). Every pixel of a rect therefore sees the clear and then exactly the primitives that
// cover it, in draw-list order: the same blend sequence as a full redraw, so the result is bit-identical as
// long as the layers hold everything that touches the rects (a culled list may leave out what does not).
// A frame is an ordered list of layers (DrawLayer). Transient layers are copied into the slot's buffers every
// frame (the usual case: what changed this frame, e.g. widgets painted with the damage as clip). A retained
// layer keeps its vertices on the GPU between frames (per frame slot, re-copied only when its `version`
// changes), so expensive static content (a scene's background, a view's grid) is neither rebuilt nor
// re-uploaded while an overlay animates over it.

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
#include "pq/ui/tile_atlas.h"

struct VmaAllocation_T;

namespace pq::ui {

enum class TextureFilter : uint8_t { Linear, Nearest };

// Texel rectangle of a texture: [x, x + w) × [y, y + h).
struct TextureRect {
  uint32_t x = 0, y = 0, w = 0, h = 0;
};

inline constexpr uint32_t kTransientLayer = UINT32_MAX;

// One draw list of a frame. Layers draw in order; all have the target's size.
struct DrawLayer {
  const DrawList* list = nullptr;
  // kTransientLayer: copied every frame. Otherwise an id < Renderer::kMaxRetainedLayers whose GPU copy is kept
  // and refreshed (per frame slot) only when `version` differs from the version last uploaded under that id.
  uint32_t retained = kTransientLayer;
  uint64_t version = 0;
};

// What a frame draws.
struct FrameDesc {
  std::span<const DrawLayer> layers;
  // true: clear the whole target and draw the layers. false: redraw only `damage` (the rest of the target
  // keeps the previous frame); an empty damage list redraws nothing (the target is presented unchanged).
  bool full = true;
  std::span<const ClipRect> damage;  // disjoint, whole pixels; clamped to the target
  Rgba clear = 0;
};

struct RendererOptions {
  // A pipeline cache blob from an earlier run (vkGetPipelineCacheData; the app keeps it in the user settings
  // directory, T0.7-fix6). Used only if its header names this device (vendor id, device id, pipelineCacheUUID: a
  // driver update changes the UUID), else the cache starts empty. The app checks its own checksum before (a
  // corrupt file never reaches the driver).
  std::span<const uint8_t> pipelineCache;
  // The glyph atlas to use (a text system may already be filling it on another thread: T0.7-fix6 loads the fonts
  // while the device is created). create() does not touch it; its pages get GPU images at the first upload().
  // Null: the renderer makes its own.
  std::shared_ptr<Atlas> atlas;
};

class Renderer {
public:
  static constexpr uint32_t kMaxFramesInFlight = 3;
  static constexpr uint32_t kMaxTextures = 256;  // table size, including kAtlasTexture, the atlas and tile pages
  static constexpr uint32_t kMaxUserTextures = kFirstTileTexture - kFirstUserTexture;
  static_assert(kFirstTileTexture + kMaxTilePages == kMaxTextures, "tile pages are the table's last ids");
  static constexpr VkDeviceSize kStagingChunkBytes = VkDeviceSize(1) << 20;  // larger uploads get their own chunk
  static constexpr uint32_t kMaxRetainedLayers = 4;
  static constexpr uint32_t kMaxFrameLayers = 8;

  static Result<std::unique_ptr<Renderer>> create(GpuContext& gpu, VkFormat colorFormat, uint32_t framesInFlight,
                                                   const RendererOptions& options = {});
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  VkFormat colorFormat() const noexcept { return colorFormat_; }
  // The pipeline cache's contents (to save for the next start; empty on failure), and whether create() used the
  // blob it was given.
  std::vector<uint8_t> pipelineCacheData() const;
  bool pipelineCacheReused() const noexcept { return pipelineCacheReused_; }
  // Whether a pipeline cache blob's header (VkPipelineCacheHeaderVersionOne) matches the device.
  static bool pipelineCacheCompatible(std::span<const uint8_t> blob, const VkPhysicalDeviceProperties& device) noexcept;
  // The glyph atlas behind kAtlasTexture / atlasPageTexture(p). The text system packs glyphs into it (UI thread).
  const std::shared_ptr<Atlas>& atlas() const noexcept { return atlas_; }
  uint32_t framesInFlight() const noexcept { return framesInFlight_; }
  // The tile cache behind tilePageTexture(p) (T3.8-fix1): widgets rasterise tiles into it (UI thread); every
  // upload() creates the GPU images of newly opened pages, stages the tiles written since the last frame and calls
  // its beginFrame().
  const std::shared_ptr<TileAtlas>& tiles() const noexcept { return tiles_; }

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
  // changed rectangles, binds every queued upload to this frame, and copies the transient layers' vertices and
  // indices into the slot's buffers (growing them if needed) and every retained layer whose version changed
  // into its own buffers for this slot. At most kMaxFrameLayers layers.
  Status upload(uint32_t slot, std::span<const DrawLayer> layers);
  // One transient layer.
  Status upload(uint32_t slot, const DrawList& dl);
  // Records the texture uploads bound by upload(slot): one batched barrier to TRANSFER_DST, the buffer → image
  // copies, one batched barrier back to SHADER_READ_ONLY. Call it outside rendering, before record().
  void recordTransfers(VkCommandBuffer cb, uint32_t slot) noexcept;
  // Records the frame into `cb`, which must be inside vkCmdBeginRendering on a colorFormat() target of the
  // layers' target size, using the buffers filled by upload(slot, frame.layers) (same layers, same order).
  // Full frames rely on the target's LOAD_OP_CLEAR; partial frames clear their damage rects here
  // (vkCmdClearAttachments) and scissor every draw to them. Pipeline / descriptor / buffer / scissor binds are
  // issued only when the state actually changes.
  void record(VkCommandBuffer cb, uint32_t slot, const FrameDesc& frame) const noexcept;
  // A full frame of one transient layer (after upload(slot, dl)).
  void record(VkCommandBuffer cb, uint32_t slot, const DrawList& dl) const noexcept;

  struct Stats {
    uint64_t framesUploaded = 0;
    uint64_t vertexBytesUploaded = 0;    // draw-list vertices + indices copied into GPU buffers (all layers)
    uint64_t retainedUploads = 0;        // retained layers (re)copied for a slot
    uint64_t copiesRecorded = 0;   // buffer → image regions
    uint64_t bytesStaged = 0;      // through the staging ring
    uint64_t atlasBytesStaged = 0; // ... of which glyph atlas pages
    uint32_t stagingChunks = 0;    // chunks currently allocated (in flight + free)
    VkDeviceSize stagingBytes = 0; // their total size
  };
  const Stats& stats() const noexcept { return stats_; }
  // GPU memory of the glyph atlas pages (VMA allocation sizes).
  VkDeviceSize atlasGpuBytes() const noexcept;
  // GPU memory of the tile pages created so far (VMA allocation sizes; at most TileAtlas::maxGpuBytes() + alignment).
  VkDeviceSize tilesGpuBytes() const noexcept;

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
  Status stageTiles();        // GPU images for opened tile pages + queue the tiles written this frame

  GpuContext* gpu_ = nullptr;
  VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
  uint32_t framesInFlight_ = 1;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool descPool_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  std::array<VkPipeline, kBlendModeCount> pipelines_{};
  VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
  bool pipelineCacheReused_ = false;
  VkSampler samplers_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

  std::array<Texture, kMaxTextures> textures_{};
  uint32_t nextTextureId_ = kFirstUserTexture;  // never-used ids start here
  std::vector<uint32_t> freeIds_;
  std::vector<uint32_t> retiring_;
  uint32_t liveTextures_ = 0;

  std::shared_ptr<Atlas> atlas_;
  uint32_t atlasPagesCreated_ = 0;
  std::shared_ptr<TileAtlas> tiles_;
  uint32_t tilePagesCreated_ = 0;

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

  std::array<Buffer, kMaxFramesInFlight> vertexBuffers_{};  // transient layers, concatenated
  std::array<Buffer, kMaxFramesInFlight> indexBuffers_{};
  // A run of up to kRunIndices consecutive indices of one command and the bounding box of its vertices: partial
  // frames draw only the runs of a retained layer that reach a damage rect (in order, consecutive runs merged
  // into one draw), so a 2 px playhead over a 77k-vertex static scene does not re-process the whole scene.
  static constexpr uint32_t kRunIndices = 3 * 64;
  struct IndexRun {
    uint32_t first, count;
    float x0, y0, x1, y1;
  };
  struct Retained {
    std::array<Buffer, kMaxFramesInFlight> vertices{}, indices{};
    std::array<uint64_t, kMaxFramesInFlight> version{};
    std::array<bool, kMaxFramesInFlight> valid{};
    uint64_t runsVersion = 0;
    bool runsValid = false;
    std::vector<IndexRun> runs;
    std::vector<uint32_t> cmdRuns;  // command k's runs: [cmdRuns[k], cmdRuns[k + 1])
  };
  std::array<Retained, kMaxRetainedLayers> retained_{};
  // Where upload(slot) put each layer: the record() side of the frame.
  struct LayerBinding {
    VkBuffer vertices = VK_NULL_HANDLE, indices = VK_NULL_HANDLE;
    int32_t vertexBase = 0;
    uint32_t indexBase = 0;
    const Retained* retained = nullptr;  // retained layers: the run index for partial frames
  };
  std::array<std::array<LayerBinding, kMaxFrameLayers>, kMaxFramesInFlight> bindings_{};
  std::array<uint32_t, kMaxFramesInFlight> bindingCount_{};
  Stats stats_;
};

}  // namespace pq::ui
