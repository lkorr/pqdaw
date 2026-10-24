#include "pq/ui/gpu.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pq::ui {

const char* vkResultName(VkResult r) noexcept {
  switch (r) {
#define PQ_VK_CASE(x) \
  case x: return #x;
    PQ_VK_CASE(VK_SUCCESS)
    PQ_VK_CASE(VK_NOT_READY)
    PQ_VK_CASE(VK_TIMEOUT)
    PQ_VK_CASE(VK_INCOMPLETE)
    PQ_VK_CASE(VK_SUBOPTIMAL_KHR)
    PQ_VK_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
    PQ_VK_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
    PQ_VK_CASE(VK_ERROR_INITIALIZATION_FAILED)
    PQ_VK_CASE(VK_ERROR_DEVICE_LOST)
    PQ_VK_CASE(VK_ERROR_MEMORY_MAP_FAILED)
    PQ_VK_CASE(VK_ERROR_LAYER_NOT_PRESENT)
    PQ_VK_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
    PQ_VK_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
    PQ_VK_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
    PQ_VK_CASE(VK_ERROR_TOO_MANY_OBJECTS)
    PQ_VK_CASE(VK_ERROR_FORMAT_NOT_SUPPORTED)
    PQ_VK_CASE(VK_ERROR_SURFACE_LOST_KHR)
    PQ_VK_CASE(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
    PQ_VK_CASE(VK_ERROR_OUT_OF_DATE_KHR)
#undef PQ_VK_CASE
    default: return "VK_ERROR_(other)";
  }
}

Error vkError(const char* what, VkResult r) {
  return Error{ErrorCode::Device, std::string(what) + ": " + vkResultName(r) + " (" + std::to_string(int(r)) + ")", {}};
}

VKAPI_ATTR VkBool32 VKAPI_CALL GpuContext::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                         VkDebugUtilsMessageTypeFlagsEXT,
                                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
  auto* self = static_cast<GpuContext*>(user);
  const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
  if (error) ++self->validationErrors_;
  std::fprintf(stderr, "[vulkan %s] %s\n", error ? "error" : "warning", data->pMessage ? data->pMessage : "");
  return VK_FALSE;
}

namespace {

bool hasLayer(const char* name) {
  uint32_t n = 0;
  vkEnumerateInstanceLayerProperties(&n, nullptr);
  std::vector<VkLayerProperties> layers(n);
  vkEnumerateInstanceLayerProperties(&n, layers.data());
  return std::any_of(layers.begin(), layers.end(), [&](const VkLayerProperties& l) { return std::strcmp(l.layerName, name) == 0; });
}

bool hasDeviceExtension(VkPhysicalDevice pd, const char* name) {
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> ext(n);
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, ext.data());
  return std::any_of(ext.begin(), ext.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

}  // namespace

Result<std::unique_ptr<GpuContext>> GpuContext::create(const GpuOptions& options) {
  std::unique_ptr<GpuContext> ctx(new GpuContext());
  using Clock = std::chrono::steady_clock;
  auto lap = [t = Clock::now()]() mutable {
    const auto now = Clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - t).count();
    t = now;
    return ms;
  };

  uint32_t loaderVersion = 0;
  if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS || loaderVersion < VK_API_VERSION_1_3)
    return Error{ErrorCode::Unsupported, "Vulkan 1.3 loader not available", {}};

  const char* env = std::getenv("PQ_VK_VALIDATION");
  const bool wantValidation = options.validation || (env && env[0] == '1');
  const bool validation = wantValidation && hasLayer("VK_LAYER_KHRONOS_validation");

  std::vector<const char*> exts;
  for (const auto& e : options.instanceExtensions) exts.push_back(e.c_str());
  if (validation) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  const char* layers[] = {"VK_LAYER_KHRONOS_validation"};

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "pqdaw";
  app.pEngineName = "pqdaw";
  app.apiVersion = VK_API_VERSION_1_3;

  VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  dbg.pfnUserCallback = &GpuContext::debugCallback;
  dbg.pUserData = ctx.get();

  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = static_cast<uint32_t>(exts.size());
  ici.ppEnabledExtensionNames = exts.data();
  if (validation) {
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;
    ici.pNext = &dbg;  // also covers instance creation/destruction
  }
  if (VkResult r = vkCreateInstance(&ici, nullptr, &ctx->instance_); r != VK_SUCCESS) return vkError("vkCreateInstance", r);
  ctx->createTimes_.instance = lap();

  if (validation) {
    auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(ctx->instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (createMessenger) createMessenger(ctx->instance_, &dbg, nullptr, &ctx->debugMessenger_);
  }

  // Pick a device: Vulkan 1.3 + dynamic rendering + sync2 + a graphics queue (+ swapchain if asked).
  // Discrete GPUs win over integrated ones.
  uint32_t nDev = 0;
  vkEnumeratePhysicalDevices(ctx->instance_, &nDev, nullptr);
  std::vector<VkPhysicalDevice> devs(nDev);
  vkEnumeratePhysicalDevices(ctx->instance_, &nDev, devs.data());
  int bestScore = -1;
  for (VkPhysicalDevice pd : devs) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) continue;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2) continue;
    if (options.swapchain && !hasDeviceExtension(pd, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < nq; ++i)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        family = i;
        break;
      }
    if (family == UINT32_MAX) continue;
    const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                      : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                                   : 1;
    if (score > bestScore) {
      bestScore = score;
      ctx->physical_ = pd;
      ctx->queueFamily_ = family;
      ctx->deviceName_ = props.deviceName;
      ctx->pipelineStatistics_ = f2.features.pipelineStatisticsQuery == VK_TRUE;
      ctx->timestampPeriodNs_ = qf[family].timestampValidBits != 0 ? props.limits.timestampPeriod : 0.0f;
    }
  }
  if (!ctx->physical_) return Error{ErrorCode::Unsupported, "no Vulkan 1.3 device with dynamic rendering and a graphics queue", {}};
  ctx->createTimes_.choose = lap();

  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = ctx->queueFamily_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  // Pipeline statistics queries (fragment-shader invocation counts for the partial-redraw measurements,
  // GpuProfiler) when the device has them: enabling the feature costs nothing until a query is used.
  VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  enabled.pNext = &f13;
  enabled.features.pipelineStatisticsQuery = ctx->pipelineStatistics_ ? VK_TRUE : VK_FALSE;
  const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &enabled;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = options.swapchain ? 1u : 0u;
  dci.ppEnabledExtensionNames = devExts;
  if (VkResult r = vkCreateDevice(ctx->physical_, &dci, nullptr, &ctx->device_); r != VK_SUCCESS) return vkError("vkCreateDevice", r);
  ctx->createTimes_.device = lap();
  vkGetDeviceQueue(ctx->device_, ctx->queueFamily_, 0, &ctx->queue_);

  VmaAllocatorCreateInfo aci{};
  aci.vulkanApiVersion = VK_API_VERSION_1_3;
  aci.instance = ctx->instance_;
  aci.physicalDevice = ctx->physical_;
  aci.device = ctx->device_;
  if (VkResult r = vmaCreateAllocator(&aci, &ctx->allocator_); r != VK_SUCCESS) return vkError("vmaCreateAllocator", r);

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = ctx->queueFamily_;
  if (VkResult r = vkCreateCommandPool(ctx->device_, &pci, nullptr, &ctx->oneShotPool_); r != VK_SUCCESS)
    return vkError("vkCreateCommandPool", r);
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (VkResult r = vkCreateFence(ctx->device_, &fci, nullptr, &ctx->oneShotFence_); r != VK_SUCCESS) return vkError("vkCreateFence", r);
  ctx->createTimes_.rest = lap();
  return ctx;
}

GpuContext::~GpuContext() {
  if (device_) {
    vkDeviceWaitIdle(device_);
    if (oneShotFence_) vkDestroyFence(device_, oneShotFence_, nullptr);
    if (oneShotPool_) vkDestroyCommandPool(device_, oneShotPool_, nullptr);
    if (allocator_) vmaDestroyAllocator(allocator_);
    vkDestroyDevice(device_, nullptr);
  }
  if (debugMessenger_) {
    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy) destroy(instance_, debugMessenger_, nullptr);
  }
  if (instance_) vkDestroyInstance(instance_, nullptr);
}

GpuContext::MemoryStats GpuContext::memoryStats() const noexcept {
  VmaTotalStatistics st{};
  vmaCalculateStatistics(allocator_, &st);
  return {st.total.statistics.allocationCount, st.total.statistics.allocationBytes};
}

Status GpuContext::submitAndWaitImpl(void (*fn)(VkCommandBuffer, void*), void* user) {
  ++blockingSubmits_;
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = oneShotPool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  if (VkResult r = vkAllocateCommandBuffers(device_, &ai, &cb); r != VK_SUCCESS) return vkError("vkAllocateCommandBuffers", r);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);
  fn(cb, user);
  vkEndCommandBuffer(cb);
  VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cbi.commandBuffer = cb;
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si.commandBufferInfoCount = 1;
  si.pCommandBufferInfos = &cbi;
  VkResult r = vkQueueSubmit2(queue_, 1, &si, oneShotFence_);
  if (r == VK_SUCCESS) r = vkWaitForFences(device_, 1, &oneShotFence_, VK_TRUE, UINT64_MAX);
  vkResetFences(device_, 1, &oneShotFence_);
  vkFreeCommandBuffers(device_, oneShotPool_, 1, &cb);
  if (r != VK_SUCCESS) return vkError("one-shot submit", r);
  return Ok{};
}

// --- GpuProfiler ------------------------------------------------------------------------------------------

Result<std::unique_ptr<GpuProfiler>> GpuProfiler::create(GpuContext& gpu, uint32_t slots) {
  if (slots == 0 || slots > 4) return Error{ErrorCode::InvalidArgument, "GpuProfiler: 1..4 slots", {}};
  std::unique_ptr<GpuProfiler> p(new GpuProfiler());
  p->gpu_ = &gpu;
  p->slots_ = slots;
  if (gpu.timestampPeriodNs() > 0.0f) {
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = 3 * slots;
    if (VkResult e = vkCreateQueryPool(gpu.device(), &ci, nullptr, &p->stamps_); e != VK_SUCCESS)
      return vkError("vkCreateQueryPool(timestamps)", e);
  }
  if (gpu.pipelineStatistics()) {
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    ci.queryCount = slots;
    ci.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
    if (VkResult e = vkCreateQueryPool(gpu.device(), &ci, nullptr, &p->stats_); e != VK_SUCCESS)
      return vkError("vkCreateQueryPool(pipeline statistics)", e);
  }
  return p;
}

GpuProfiler::~GpuProfiler() {
  if (!gpu_) return;
  if (stamps_) vkDestroyQueryPool(gpu_->device(), stamps_, nullptr);
  if (stats_) vkDestroyQueryPool(gpu_->device(), stats_, nullptr);
}

void GpuProfiler::begin(VkCommandBuffer cb, uint32_t slot) noexcept {
  if (slot >= slots_) return;
  if (stamps_) {
    vkCmdResetQueryPool(cb, stamps_, 3 * slot, 3);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, stamps_, 3 * slot);
  }
  if (stats_) {
    vkCmdResetQueryPool(cb, stats_, slot, 1);
    vkCmdBeginQuery(cb, stats_, slot, 0);
  }
  recorded_[slot] = true;
}

void GpuProfiler::drawn(VkCommandBuffer cb, uint32_t slot) noexcept {
  if (slot >= slots_) return;
  if (stats_) vkCmdEndQuery(cb, stats_, slot);
  if (stamps_) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, stamps_, 3 * slot + 1);
}

void GpuProfiler::end(VkCommandBuffer cb, uint32_t slot) noexcept {
  if (slot >= slots_) return;
  if (stamps_) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, stamps_, 3 * slot + 2);
}

bool GpuProfiler::read(uint32_t slot, GpuFrameStats& out) noexcept {
  if (slot >= slots_ || !recorded_[slot]) return false;
  GpuFrameStats s;
  if (stamps_) {
    uint64_t t[3] = {};
    if (vkGetQueryPoolResults(gpu_->device(), stamps_, 3 * slot, 3, sizeof t, t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) !=
        VK_SUCCESS)
      return false;
    const double ns = gpu_->timestampPeriodNs();
    s.drawMs = static_cast<double>(t[1] - t[0]) * ns * 1e-6;
    s.copyMs = static_cast<double>(t[2] - t[1]) * ns * 1e-6;
    s.hasTimes = true;
  }
  if (stats_) {
    uint64_t f = 0;
    if (vkGetQueryPoolResults(gpu_->device(), stats_, slot, 1, sizeof f, &f, sizeof f, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
      return false;
    s.fragments = f;
    s.hasFragments = true;
  }
  out = s;
  return true;
}

}  // namespace pq::ui
