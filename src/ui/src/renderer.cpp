#include "pq/ui/renderer.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>

namespace pq::ui {

namespace {

// SPIR-V compiled from src/ui/shaders at build time (glslc -mfmt=c).
const uint32_t kVertSpv[] =
#include "ui.vert.spv.inc"
    ;
const uint32_t kFragSpv[] =
#include "ui.frag.spv.inc"
    ;

struct PushConstants {
  float scale[2];
  float translate[2];
};

VkShaderModule makeModule(VkDevice dev, const uint32_t* code, size_t bytes, VkResult& r) {
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = bytes;
  ci.pCode = code;
  VkShaderModule m = VK_NULL_HANDLE;
  r = vkCreateShaderModule(dev, &ci, nullptr, &m);
  return m;
}

constexpr VkDeviceSize kStagingAlign = 16;  // ≥ any texel size; also a friendly copy offset alignment
// Free chunks kept for reuse beyond this many are destroyed on release (a burst does not pin memory).
constexpr size_t kMaxFreeChunks = Renderer::kMaxFramesInFlight + 1;

bool overlaps(uint32_t ax, uint32_t ay, uint32_t aw, uint32_t ah, uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh) noexcept {
  return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}
bool contains(uint32_t ax, uint32_t ay, uint32_t aw, uint32_t ah, uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh) noexcept {
  return bx >= ax && by >= ay && bx + bw <= ax + aw && by + bh <= ay + ah;
}

}  // namespace

Result<std::unique_ptr<Renderer>> Renderer::create(GpuContext& gpu, VkFormat colorFormat, uint32_t framesInFlight) {
  if (framesInFlight == 0 || framesInFlight > kMaxFramesInFlight)
    return Error{ErrorCode::InvalidArgument, "Renderer: framesInFlight must be 1..3", {}};
  std::unique_ptr<Renderer> r(new Renderer());
  r->gpu_ = &gpu;
  r->colorFormat_ = colorFormat;
  r->framesInFlight_ = framesInFlight;
  const VkDevice dev = gpu.device();

  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  slci.bindingCount = 1;
  slci.pBindings = &binding;
  if (VkResult e = vkCreateDescriptorSetLayout(dev, &slci, nullptr, &r->setLayout_); e != VK_SUCCESS)
    return vkError("vkCreateDescriptorSetLayout", e);

  // One set per table slot, allocated on the slot's first use and rewritten when the slot is reused.
  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures};
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = kMaxTextures;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &ps;
  if (VkResult e = vkCreateDescriptorPool(dev, &dpci, nullptr, &r->descPool_); e != VK_SUCCESS)
    return vkError("vkCreateDescriptorPool", e);

  VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants)};
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &r->setLayout_;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  if (VkResult e = vkCreatePipelineLayout(dev, &plci, nullptr, &r->layout_); e != VK_SUCCESS)
    return vkError("vkCreatePipelineLayout", e);

  for (int i = 0; i < 2; ++i) {
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = i == 0 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (VkResult e = vkCreateSampler(dev, &sci, nullptr, &r->samplers_[i]); e != VK_SUCCESS) return vkError("vkCreateSampler", e);
  }

  if (Status s = r->createPipelines(); !s) return std::move(s).error();

  // Page 0 exists from the start (it backs kAtlasTexture); its texels go up with the first frame's transfers.
  r->atlas_ = std::make_shared<Atlas>();
  if (Status s = r->ensureAtlasPages(); !s) return std::move(s).error();
  return r;
}


Status Renderer::createPipelines() {
  const VkDevice dev = gpu_->device();
  VkResult e = VK_SUCCESS;
  VkShaderModule vs = makeModule(dev, kVertSpv, sizeof(kVertSpv), e);
  if (e != VK_SUCCESS) return vkError("vkCreateShaderModule(vert)", e);
  VkShaderModule fs = makeModule(dev, kFragSpv, sizeof(kFragSpv), e);
  if (e != VK_SUCCESS) {
    vkDestroyShaderModule(dev, vs, nullptr);
    return vkError("vkCreateShaderModule(frag)", e);
  }

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";

  VkVertexInputBindingDescription vb{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attrs[3] = {
      {0, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, pos))},
      {1, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, uv))},
      {2, 0, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(offsetof(Vertex, col))},
  };
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &vb;
  vi.vertexAttributeDescriptionCount = 3;
  vi.pVertexAttributeDescriptions = attrs;

  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;  // 2D geometry is emitted with either winding
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;  // AA is geometric (SPEC §11): no MSAA
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 2;
  dy.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rci.colorAttachmentCount = 1;
  rci.pColorAttachmentFormats = &colorFormat_;

  Status status = Ok{};
  for (uint32_t mode = 0; mode < kBlendModeCount; ++mode) {
    // Straight-alpha blending as in ImGui / PQ Meter. Alpha channel: srcA + dstA·(1−srcA) in both modes, so
    // an opaque target stays opaque (PQ Meter face.cpp blendAdditive uses the same alpha factors).
    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable = VK_TRUE;
    att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att.dstColorBlendFactor =
        static_cast<BlendMode>(mode) == BlendMode::Additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.colorBlendOp = VK_BLEND_OP_ADD;
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.alphaBlendOp = VK_BLEND_OP_ADD;
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rci;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = layout_;
    if (VkResult pe = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipelines_[mode]); pe != VK_SUCCESS) {
      status = vkError("vkCreateGraphicsPipelines", pe);
      break;
    }
  }
  vkDestroyShaderModule(dev, vs, nullptr);
  vkDestroyShaderModule(dev, fs, nullptr);
  return status;
}

Renderer::~Renderer() {
  if (!gpu_) return;
  const VkDevice dev = gpu_->device();
  vkDeviceWaitIdle(dev);
  for (uint32_t i = 1; i < kMaxTextures; ++i) destroyImage(textures_[i]);  // slot 0 aliases page 0's set
  for (Chunk& c : chunks_)
    if (c.buffer) vmaDestroyBuffer(gpu_->allocator(), c.buffer, c.alloc);
  for (Buffer& b : vertexBuffers_) destroyBuffer(b);
  for (Buffer& b : indexBuffers_) destroyBuffer(b);
  for (VkPipeline p : pipelines_)
    if (p) vkDestroyPipeline(dev, p, nullptr);
  for (VkSampler s : samplers_)
    if (s) vkDestroySampler(dev, s, nullptr);
  if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
  if (descPool_) vkDestroyDescriptorPool(dev, descPool_, nullptr);  // frees the sets
  if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
}

Status Renderer::createImage(Texture& t, uint32_t w, uint32_t h, bool coverage, TextureFilter filter) {
  const VkDevice dev = gpu_->device();
  const VkFormat format = coverage ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {w, h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (VkResult e = vmaCreateImage(gpu_->allocator(), &ici, &aci, &t.image, &t.alloc, nullptr); e != VK_SUCCESS)
    return vkError("vmaCreateImage(texture)", e);
  t.width = w;
  t.height = h;
  t.bytesPerTexel = coverage ? 1 : 4;
  t.initialized = false;

  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = t.image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = format;
  // Coverage pages read as (1, 1, 1, coverage): the shader's vertex colour × texel is unchanged from the RGBA
  // page (white texels, coverage in alpha), so glyphs and sprites need no pipeline or shader variant.
  if (coverage) vci.components = {VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R};
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (VkResult e = vkCreateImageView(dev, &vci, nullptr, &t.view); e != VK_SUCCESS) {
    destroyImage(t);
    return vkError("vkCreateImageView", e);
  }
  if (!t.set) {
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descPool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &setLayout_;
    if (VkResult e = vkAllocateDescriptorSets(dev, &dai, &t.set); e != VK_SUCCESS) {
      destroyImage(t);
      return vkError("vkAllocateDescriptorSets", e);
    }
  }
  // The set is free to rewrite: a slot is only reused after every frame that bound it has finished.
  VkDescriptorImageInfo dii{samplers_[filter == TextureFilter::Linear ? 0 : 1], t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  wds.dstSet = t.set;
  wds.descriptorCount = 1;
  wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  wds.pImageInfo = &dii;
  vkUpdateDescriptorSets(dev, 1, &wds, 0, nullptr);
  return Ok{};
}

void Renderer::destroyImage(Texture& t) noexcept {
  if (t.view) vkDestroyImageView(gpu_->device(), t.view, nullptr);
  if (t.image) vmaDestroyImage(gpu_->allocator(), t.image, t.alloc);
  t.view = VK_NULL_HANDLE;
  t.image = VK_NULL_HANDLE;
  t.alloc = nullptr;
  t.initialized = false;
}

uint8_t* Renderer::stage(VkDeviceSize bytes, VkBuffer& buffer, VkDeviceSize& offset) {
  if (!openChunks_.empty()) {
    Chunk& c = chunks_[openChunks_.back()];
    const VkDeviceSize at = (c.used + kStagingAlign - 1) & ~(kStagingAlign - 1);
    if (at + bytes <= c.size) {
      c.used = at + bytes;
      buffer = c.buffer;
      offset = at;
      return c.mapped + at;
    }
  }
  // Next chunk: a free one that is large enough, else a new one (a hole in chunks_ if there is one).
  uint32_t index = UINT32_MAX;
  for (size_t i = 0; i < freeChunks_.size(); ++i)
    if (chunks_[freeChunks_[i]].size >= bytes) {
      index = freeChunks_[i];
      freeChunks_.erase(freeChunks_.begin() + static_cast<ptrdiff_t>(i));
      break;
    }
  if (index == UINT32_MAX) {
    Chunk c;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = std::max(kStagingChunkBytes, bytes);
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(gpu_->allocator(), &bci, &aci, &c.buffer, &c.alloc, &info) != VK_SUCCESS) return nullptr;
    c.mapped = static_cast<uint8_t*>(info.pMappedData);
    c.size = bci.size;
    for (uint32_t i = 0; i < chunks_.size() && index == UINT32_MAX; ++i)
      if (!chunks_[i].buffer) index = i;
    if (index == UINT32_MAX) {
      index = static_cast<uint32_t>(chunks_.size());
      chunks_.push_back(c);
    } else {
      chunks_[index] = c;
    }
    ++stats_.stagingChunks;
    stats_.stagingBytes += c.size;
  }
  openChunks_.push_back(index);
  Chunk& c = chunks_[index];
  c.used = bytes;
  buffer = c.buffer;
  offset = 0;
  return c.mapped;
}

void Renderer::releaseChunk(uint32_t index) noexcept {
  Chunk& c = chunks_[index];
  c.used = 0;
  if (c.size > kStagingChunkBytes || freeChunks_.size() >= kMaxFreeChunks) {
    --stats_.stagingChunks;
    stats_.stagingBytes -= c.size;
    vmaDestroyBuffer(gpu_->allocator(), c.buffer, c.alloc);
    c = Chunk{};
    return;
  }
  freeChunks_.push_back(index);
}

void Renderer::queueCopy(const Copy& in) {
  Copy c = in;
  // Copies within one frame run unordered unless a barrier separates them. A queued copy that the new one
  // covers is dropped (its texels would be overwritten anyway); one that only overlaps forces the new copy
  // into a later batch. Disjoint copies (a spectrogram's columns, glyph rects) share batch 0.
  for (size_t i = 0; i < queued_.size();) {
    const Copy& q = queued_[i];
    if (q.tex == c.tex && overlaps(q.x, q.y, q.w, q.h, c.x, c.y, c.w, c.h)) {
      if (contains(c.x, c.y, c.w, c.h, q.x, q.y, q.w, q.h) && q.batch <= c.batch) {
        queued_.erase(queued_.begin() + static_cast<ptrdiff_t>(i));
        continue;
      }
      c.batch = std::max(c.batch, q.batch + 1);
    }
    ++i;
  }
  queued_.push_back(c);
}

Result<TextureId> Renderer::createTexture(const Image& img, TextureFilter filter) {
  if (img.width == 0 || img.height == 0 || img.rgba.size() != size_t(img.width) * img.height * 4)
    return Error{ErrorCode::InvalidArgument, "createTexture: empty or malformed image", {}};
  uint32_t id = 0;
  if (!freeIds_.empty()) {
    id = freeIds_.back();
  } else if (nextTextureId_ < kMaxTextures) {
    id = nextTextureId_;
  } else {
    return Error{ErrorCode::Overflow, "createTexture: texture table full", {}};
  }
  Texture& t = textures_[id];
  if (Status s = createImage(t, img.width, img.height, false, filter); !s) return std::move(s).error();
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
  uint8_t* dst = stage(img.rgba.size(), buffer, offset);
  if (!dst) {
    destroyImage(t);
    return Error{ErrorCode::Device, "createTexture: staging allocation failed", {}};
  }
  std::memcpy(dst, img.rgba.data(), img.rgba.size());
  stats_.bytesStaged += img.rgba.size();
  if (!freeIds_.empty()) {
    freeIds_.pop_back();
  } else {
    ++nextTextureId_;
  }
  t.state = TexState::Live;
  ++liveTextures_;
  queueCopy({id, 0, buffer, offset, 0, 0, img.width, img.height});
  return static_cast<TextureId>(id);
}

Status Renderer::updateTexture(TextureId id, const TextureRect& rect, std::span<const uint8_t> rgba, uint32_t rowPitch) {
  if (id < kFirstUserTexture || id >= kMaxTextures || textures_[id].state != TexState::Live)
    return Error{ErrorCode::InvalidArgument, "updateTexture: not a live user texture", {}};
  const Texture& t = textures_[id];
  if (rect.w == 0 || rect.h == 0) return Ok{};
  if (rect.x > t.width || rect.w > t.width - rect.x || rect.y > t.height || rect.h > t.height - rect.y)
    return Error{ErrorCode::InvalidArgument, "updateTexture: rect outside the texture", {}};
  const size_t rowBytes = size_t(rect.w) * 4;
  const size_t pitch = rowPitch ? rowPitch : rowBytes;
  if (pitch < rowBytes || rgba.size() < pitch * (rect.h - 1) + rowBytes)
    return Error{ErrorCode::InvalidArgument, "updateTexture: data smaller than the rect", {}};
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
  uint8_t* dst = stage(rowBytes * rect.h, buffer, offset);
  if (!dst) return Error{ErrorCode::Device, "updateTexture: staging allocation failed", {}};
  if (pitch == rowBytes) {
    std::memcpy(dst, rgba.data(), rowBytes * rect.h);
  } else {
    for (uint32_t y = 0; y < rect.h; ++y) std::memcpy(dst + rowBytes * y, rgba.data() + pitch * y, rowBytes);
  }
  stats_.bytesStaged += rowBytes * rect.h;
  queueCopy({id, 0, buffer, offset, rect.x, rect.y, rect.w, rect.h});
  return Ok{};
}

Status Renderer::destroyTexture(TextureId id) {
  if (id < kFirstUserTexture || id >= kMaxTextures || textures_[id].state != TexState::Live)
    return Error{ErrorCode::InvalidArgument, "destroyTexture: not a live user texture", {}};
  Texture& t = textures_[id];
  // Queued (not yet bound) uploads die with it; ones bound to a submitted frame complete before the image goes.
  std::erase_if(queued_, [id](const Copy& c) { return c.tex == id; });
  t.state = TexState::Retiring;
  t.retireAfter = slotSerial_;
  --liveTextures_;
  retiring_.push_back(id);
  return Ok{};
}

void Renderer::retireSlot(uint32_t slot) noexcept {
  // The slot's previous frame has finished: its staging chunks are free again.
  for (uint32_t c : slotChunks_[slot]) releaseChunk(c);
  slotChunks_[slot].clear();
  slotCopies_[slot].clear();
  slotSerial_[slot] = ++serial_;
  // A destroyed texture goes once every slot that had a frame in flight at destroy time has come round again.
  for (size_t i = 0; i < retiring_.size();) {
    Texture& t = textures_[retiring_[i]];
    bool done = true;
    for (uint32_t s = 0; s < kMaxFramesInFlight; ++s)
      if (t.retireAfter[s] != 0 && slotSerial_[s] <= t.retireAfter[s]) done = false;
    if (!done) {
      ++i;
      continue;
    }
    destroyImage(t);
    t.state = TexState::Free;
    freeIds_.push_back(retiring_[i]);
    retiring_[i] = retiring_.back();
    retiring_.pop_back();
  }
}

Status Renderer::ensureAtlasPages() {
  while (atlasPagesCreated_ < atlas_->pageCount()) {
    Texture& t = textures_[atlasPageTexture(atlasPagesCreated_)];
    if (Status s = createImage(t, kAtlasWidth, kAtlasHeight, true, TextureFilter::Linear); !s) return s;
    t.state = TexState::Live;
    if (atlasPagesCreated_ == 0) {
      textures_[kAtlasTexture].set = t.set;  // the shared sprites: any page will do, page 0 always exists
      textures_[kAtlasTexture].state = TexState::Live;
    }
    ++atlasPagesCreated_;
  }
  return Ok{};
}

Status Renderer::stageAtlas() {
  if (Status s = ensureAtlasPages(); !s) return s;
  for (uint32_t p = 0; p < atlas_->pageCount(); ++p) {
    const CoverageImage& img = atlas_->pageImage(p);
    for (const Atlas::DirtyRect& d : atlas_->dirtyRects(p)) {
      const uint32_t w = d.x1 - d.x0, h = d.y1 - d.y0;
      VkBuffer buffer = VK_NULL_HANDLE;
      VkDeviceSize offset = 0;
      uint8_t* dst = stage(VkDeviceSize(w) * h, buffer, offset);
      if (!dst) return Error{ErrorCode::Device, "atlas upload: staging allocation failed", {}};
      for (uint32_t y = 0; y < h; ++y) std::memcpy(dst + size_t(w) * y, img.at(d.x0, d.y0 + y), w);
      stats_.bytesStaged += size_t(w) * h;
      stats_.atlasBytesStaged += size_t(w) * h;
      queueCopy({atlasPageTexture(p), 0, buffer, offset, d.x0, d.y0, w, h});  // the rects are disjoint: batch 0
    }
    atlas_->takeDirty(p);
  }
  return Ok{};
}

Status Renderer::ensureBuffer(Buffer& b, VkDeviceSize need, VkBufferUsageFlags usage) {
  if (b.size >= need) return Ok{};
  destroyBuffer(b);
  VkDeviceSize size = 64 * 1024;
  while (size < need) size *= 2;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = usage;
  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
  // Written once per frame by the CPU, read once by the GPU: persistently mapped system memory. Measured on
  // the RTX 3060 Ti (bench/RESULTS.md): device-local host-visible (ReBAR) memory made the CPU memcpy 4-6×
  // slower (~6.7 GB/s over PCIe) and the whole 10k-rect frame 1.6× slower than letting the GPU pull the
  // vertices from system RAM.
  aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VmaAllocationInfo info{};
  if (VkResult e = vmaCreateBuffer(gpu_->allocator(), &bci, &aci, &b.buffer, &b.alloc, &info); e != VK_SUCCESS)
    return vkError("vmaCreateBuffer(draw list)", e);
  b.mapped = info.pMappedData;
  b.size = size;
  return Ok{};
}

void Renderer::destroyBuffer(Buffer& b) noexcept {
  if (b.buffer) vmaDestroyBuffer(gpu_->allocator(), b.buffer, b.alloc);
  b = Buffer{};
}

Status Renderer::upload(uint32_t slot, const DrawList& dl) {
  if (slot >= framesInFlight_) return Error{ErrorCode::InvalidArgument, "Renderer::upload: bad frame slot", {}};
  retireSlot(slot);
  ++stats_.framesUploaded;
  if (Status s = stageAtlas(); !s) return s;
  // Bind every queued upload (and the chunks holding its bytes) to this frame. Swaps keep the capacities.
  for (uint32_t c : openChunks_) vmaFlushAllocation(gpu_->allocator(), chunks_[c].alloc, 0, chunks_[c].used);
  slotChunks_[slot].swap(openChunks_);
  slotCopies_[slot].swap(queued_);
  std::sort(slotCopies_[slot].begin(), slotCopies_[slot].end(), [](const Copy& a, const Copy& b) {
    if (a.batch != b.batch) return a.batch < b.batch;
    if (a.tex != b.tex) return a.tex < b.tex;
    return a.buffer < b.buffer;
  });

  const auto v = dl.vertices();
  const auto i = dl.indices();
  if (i.empty()) return Ok{};
  const VkDeviceSize vBytes = v.size_bytes(), iBytes = i.size_bytes();
  if (Status s = ensureBuffer(vertexBuffers_[slot], vBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); !s) return s;
  if (Status s = ensureBuffer(indexBuffers_[slot], iBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT); !s) return s;
  std::memcpy(vertexBuffers_[slot].mapped, v.data(), vBytes);
  std::memcpy(indexBuffers_[slot].mapped, i.data(), iBytes);
  // No-ops on HOST_COHERENT memory (the usual case); required for correctness otherwise.
  vmaFlushAllocation(gpu_->allocator(), vertexBuffers_[slot].alloc, 0, vBytes);
  vmaFlushAllocation(gpu_->allocator(), indexBuffers_[slot].alloc, 0, iBytes);
  return Ok{};
}

// RT-safe in the sense of the UI thread's frame path: no allocation once the scratch vectors have grown, no
// waits.
void Renderer::recordTransfers(VkCommandBuffer cb, uint32_t slot) noexcept {
  if (slot >= framesInFlight_) return;
  const std::vector<Copy>& copies = slotCopies_[slot];
  if (copies.empty()) return;

  // Distinct textures (copies are sorted by batch, then texture): one layout transition each, both ways.
  barriers_.clear();
  for (const Copy& c : copies) {
    const Texture& t = textures_[c.tex];
    if (t.state == TexState::Free || !t.image) continue;
    bool seen = false;
    for (const VkImageMemoryBarrier2& b : barriers_) seen = seen || b.image == t.image;
    if (seen) continue;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    // Earlier frames' fragment shaders may still be sampling the texels about to be replaced (write-after-read:
    // an execution dependency suffices); a first upload discards the undefined contents.
    b.srcStageMask = t.initialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE;
    b.srcAccessMask = VK_ACCESS_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = t.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barriers_.push_back(b);
  }
  if (barriers_.empty()) return;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = static_cast<uint32_t>(barriers_.size());
  di.pImageMemoryBarriers = barriers_.data();
  vkCmdPipelineBarrier2(cb, &di);

  // One vkCmdCopyBufferToImage per (batch, texture, staging buffer) group.
  uint32_t batch = 0;
  for (size_t i = 0; i < copies.size();) {
    const Copy& first = copies[i];
    if (first.batch != batch) {
      // Overlapping copies of a later batch must land after the earlier ones (write-after-write).
      VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      VkDependencyInfo md{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      md.memoryBarrierCount = 1;
      md.pMemoryBarriers = &mb;
      vkCmdPipelineBarrier2(cb, &md);
      batch = first.batch;
    }
    regions_.clear();
    size_t j = i;
    for (; j < copies.size() && copies[j].batch == first.batch && copies[j].tex == first.tex && copies[j].buffer == first.buffer; ++j) {
      const Copy& c = copies[j];
      VkBufferImageCopy r{};
      r.bufferOffset = c.offset;
      r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      r.imageOffset = {static_cast<int32_t>(c.x), static_cast<int32_t>(c.y), 0};
      r.imageExtent = {c.w, c.h, 1};
      regions_.push_back(r);
    }
    const Texture& t = textures_[first.tex];
    if (t.state != TexState::Free && t.image) {
      vkCmdCopyBufferToImage(cb, first.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             static_cast<uint32_t>(regions_.size()), regions_.data());
      stats_.copiesRecorded += regions_.size();
    }
    i = j;
  }

  for (VkImageMemoryBarrier2& b : barriers_) {
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  vkCmdPipelineBarrier2(cb, &di);
  for (const Copy& c : copies) textures_[c.tex].initialized = textures_[c.tex].image != VK_NULL_HANDLE;
}

VkDeviceSize Renderer::atlasGpuBytes() const noexcept {
  VkDeviceSize total = 0;
  for (uint32_t p = 0; p < atlasPagesCreated_; ++p) {
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(gpu_->allocator(), textures_[atlasPageTexture(p)].alloc, &info);
    total += info.size;
  }
  return total;
}

void Renderer::record(VkCommandBuffer cb, uint32_t slot, const DrawList& dl) const noexcept {
  if (dl.empty() || slot >= framesInFlight_ || !vertexBuffers_[slot].buffer) return;
  const float w = static_cast<float>(dl.targetWidth()), h = static_cast<float>(dl.targetHeight());
  if (w <= 0.0f || h <= 0.0f) return;

  const VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffers_[slot].buffer, &zero);
  vkCmdBindIndexBuffer(cb, indexBuffers_[slot].buffer, 0, VK_INDEX_TYPE_UINT32);
  const VkViewport viewport{0.0f, 0.0f, w, h, 0.0f, 1.0f};
  vkCmdSetViewport(cb, 0, 1, &viewport);
  const PushConstants pc{{2.0f / w, 2.0f / h}, {-1.0f, -1.0f}};
  vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, &pc);

  const int32_t tw = static_cast<int32_t>(dl.targetWidth()), th = static_cast<int32_t>(dl.targetHeight());
  uint32_t boundBlend = UINT32_MAX;
  VkDescriptorSet boundSet = VK_NULL_HANDLE;
  ClipRect boundClip{-1, -1, -1, -1};
  for (const DrawCmd& c : dl.commands()) {
    if (c.idxCount == 0) continue;
    ClipRect clip{std::max(c.clip.x0, 0), std::max(c.clip.y0, 0), std::min(c.clip.x1, tw), std::min(c.clip.y1, th)};
    if (clip.x1 <= clip.x0 || clip.y1 <= clip.y0) continue;
    if (static_cast<uint32_t>(c.blend) != boundBlend) {
      boundBlend = static_cast<uint32_t>(c.blend);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[boundBlend]);
    }
    // Unknown or destroyed ids fall back to the shared sprites (page 0). kAtlasTexture and page 0 share a
    // set, so switching between them binds nothing.
    const TextureId tex = c.texture < kMaxTextures && textures_[c.texture].state != TexState::Free ? c.texture : kAtlasTexture;
    const VkDescriptorSet set = textures_[tex].set;
    if (set != boundSet) {
      boundSet = set;
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    }
    if (!(clip == boundClip)) {
      boundClip = clip;
      const VkRect2D sc{{clip.x0, clip.y0}, {static_cast<uint32_t>(clip.x1 - clip.x0), static_cast<uint32_t>(clip.y1 - clip.y0)}};
      vkCmdSetScissor(cb, 0, 1, &sc);
    }
    vkCmdDrawIndexed(cb, c.idxCount, 1, c.idxOffset, 0, 0);
  }
}

}  // namespace pq::ui
