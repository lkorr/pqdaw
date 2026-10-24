#pragma once

// The Vulkan pipeline cache kept between runs (T0.7-fix6): creating the renderer's pipelines compiles their shaders
// unless the driver finds them in the cache. The file wraps the driver's blob (vkGetPipelineCacheData) in our own
// header — magic, payload size, FNV-1a 64 of the payload — so a truncated or corrupt file is rejected here and never
// handed to the driver; the renderer then checks the blob's own header against the device (vendor, device, driver
// UUID), so a GPU or driver change starts an empty cache. A missing or rejected file costs only the compile.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pq::app {

// The per-user directory for machine-local caches: %LOCALAPPDATA%\pqdaw (UTF-8), created if needed; empty when it
// cannot be determined or created. (User settings roam: userSettingsDir() in user_settings.h, %APPDATA%\pqdaw.)
std::string localCacheDir();
// localCacheDir() + "\vk_pipeline_cache.bin", or empty.
std::string pipelineCachePath();
// The blob savePipelineCache wrote, or empty when the file is missing, has the wrong magic or size, or fails its
// checksum.
std::vector<uint8_t> loadPipelineCache(const std::string& path);
// Writes `blob` with the header to a temporary file and moves it over `path` (an interrupted write never leaves a
// half file). False on failure.
bool savePipelineCache(const std::string& path, std::span<const uint8_t> blob);

}  // namespace pq::app
