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

  r->atlas_ = std::make_shared<Atlas>();
  auto atlas = r->createTexture(r->atlas_->image(), TextureFilter::Linear);
  if (!atlas) return std::move(atlas).error();
  r->atlasUploadedVersion_ = r->atlas_->version();
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
  for (Texture& t : textures_) {
    if (t.view) vkDestroyImageView(dev, t.view, nullptr);
    if (t.image) vmaDestroyImage(gpu_->allocator(), t.image, t.alloc);
  }
  for (Buffer& b : vertexBuffers_) destroyBuffer(b);
  for (Buffer& b : indexBuffers_) destroyBuffer(b);
  for (VkPipeline p : pipelines_)
    if (p) vkDestroyPipeline(dev, p, nullptr);
  for (VkSampler s : samplers_)
    if (s) vkDestroySampler(dev, s, nullptr);
  if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
  if (descPool_) vkDestroyDescriptorPool(dev, descPool_, nullptr);
  if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
}

Result<TextureId> Renderer::createTexture(const Image& img, TextureFilter filter) {
  if (img.width == 0 || img.height == 0 || img.rgba.size() != size_t(img.width) * img.height * 4)
    return Error{ErrorCode::InvalidArgument, "createTexture: empty or malformed image", {}};
  if (textures_.size() >= kMaxTextures) return Error{ErrorCode::Overflow, "createTexture: texture table full", {}};
  const VkDevice dev = gpu_->device();
  VmaAllocator alloc = gpu_->allocator();

  Texture t;
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;
  ici.extent = {img.width, img.height, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (VkResult e = vmaCreateImage(alloc, &ici, &aci, &t.image, &t.alloc, nullptr); e != VK_SUCCESS)
    return vkError("vmaCreateImage(texture)", e);
  textures_.push_back(t);  // owned from here, so the destructor frees it on any later failure
  Texture& tex = textures_.back();

  // Staging copy.
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = img.rgba.size();
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VmaAllocationCreateInfo sci{};
  sci.usage = VMA_MEMORY_USAGE_AUTO;
  sci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation stagingAlloc = nullptr;
  VmaAllocationInfo info{};
  if (VkResult e = vmaCreateBuffer(alloc, &bci, &sci, &staging, &stagingAlloc, &info); e != VK_SUCCESS)
    return vkError("vmaCreateBuffer(staging)", e);
  std::memcpy(info.pMappedData, img.rgba.data(), img.rgba.size());
  vmaFlushAllocation(alloc, stagingAlloc, 0, VK_WHOLE_SIZE);

  const VkImage image = tex.image;
  const uint32_t w = img.width, h = img.height;
  Status s = gpu_->submitAndWait([&](VkCommandBuffer cb) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cb, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cb, &di);
  });
  vmaDestroyBuffer(alloc, staging, stagingAlloc);
  if (!s) return std::move(s).error();

  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = VK_FORMAT_R8G8B8A8_UNORM;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (VkResult e = vkCreateImageView(dev, &vci, nullptr, &tex.view); e != VK_SUCCESS) return vkError("vkCreateImageView", e);

  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = descPool_;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &setLayout_;
  if (VkResult e = vkAllocateDescriptorSets(dev, &dai, &tex.set); e != VK_SUCCESS) return vkError("vkAllocateDescriptorSets", e);
  VkDescriptorImageInfo dii{samplers_[filter == TextureFilter::Linear ? 0 : 1], tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  wds.dstSet = tex.set;
  wds.descriptorCount = 1;
  wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  wds.pImageInfo = &dii;
  vkUpdateDescriptorSets(dev, 1, &wds, 0, nullptr);
  return static_cast<TextureId>(textures_.size() - 1);
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

Status Renderer::uploadAtlasRows(uint32_t y0, uint32_t y1) {
  const Image& img = atlas_->image();
  const VkDevice dev = gpu_->device();
  VmaAllocator alloc = gpu_->allocator();
  // Frames still in flight may be sampling the page. New glyphs are rare (a warm UI introduces none), so a
  // full wait here is cheaper overall than double-buffering a 4 MB texture.
  if (VkResult e = vkDeviceWaitIdle(dev); e != VK_SUCCESS) return vkError("vkDeviceWaitIdle", e);
  const size_t rowBytes = size_t(img.width) * 4;
  const size_t bytes = rowBytes * (y1 - y0);
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VmaAllocationCreateInfo sci{};
  sci.usage = VMA_MEMORY_USAGE_AUTO;
  sci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation stagingAlloc = nullptr;
  VmaAllocationInfo info{};
  if (VkResult e = vmaCreateBuffer(alloc, &bci, &sci, &staging, &stagingAlloc, &info); e != VK_SUCCESS)
    return vkError("vmaCreateBuffer(atlas staging)", e);
  std::memcpy(info.pMappedData, img.rgba.data() + rowBytes * y0, bytes);
  vmaFlushAllocation(alloc, stagingAlloc, 0, VK_WHOLE_SIZE);
  const VkImage image = textures_[kAtlasTexture].image;
  const uint32_t w = img.width;
  Status s = gpu_->submitAndWait([&](VkCommandBuffer cb) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, static_cast<int32_t>(y0), 0};
    region.imageExtent = {w, y1 - y0, 1};
    vkCmdCopyBufferToImage(cb, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cb, &di);
  });
  vmaDestroyBuffer(alloc, staging, stagingAlloc);
  return s;
}

Status Renderer::upload(uint32_t slot, const DrawList& dl) {
  if (slot >= framesInFlight_) return Error{ErrorCode::InvalidArgument, "Renderer::upload: bad frame slot", {}};
  if (atlas_->version() != atlasUploadedVersion_) {
    const Atlas::RowRange rows = atlas_->dirtyRowsSince(atlasUploadedVersion_);
    if (rows.y1 > rows.y0)
      if (Status s = uploadAtlasRows(rows.y0, rows.y1); !s) return s;
    atlasUploadedVersion_ = atlas_->version();
  }
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
  TextureId boundTex = UINT32_MAX;
  ClipRect boundClip{-1, -1, -1, -1};
  for (const DrawCmd& c : dl.commands()) {
    if (c.idxCount == 0) continue;
    ClipRect clip{std::max(c.clip.x0, 0), std::max(c.clip.y0, 0), std::min(c.clip.x1, tw), std::min(c.clip.y1, th)};
    if (clip.x1 <= clip.x0 || clip.y1 <= clip.y0) continue;
    if (static_cast<uint32_t>(c.blend) != boundBlend) {
      boundBlend = static_cast<uint32_t>(c.blend);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[boundBlend]);
    }
    const TextureId tex = c.texture < textures_.size() ? c.texture : kAtlasTexture;
    if (tex != boundTex) {
      boundTex = tex;
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &textures_[tex].set, 0, nullptr);
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
