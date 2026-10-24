#include "pq/app/pipeline_cache.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace pq::app {

namespace {

constexpr char kMagic[8] = {'P', 'Q', 'V', 'K', 'P', 'C', '0', '1'};
constexpr size_t kHeaderBytes = 8 + 8 + 8;  // magic, payload size, FNV-1a 64 of the payload
constexpr uint64_t kMaxPayload = uint64_t{64} << 20;

uint64_t fnv1a64(std::span<const uint8_t> b) noexcept {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t c : b) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::filesystem::path pathOf(const std::string& utf8) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string utf8Of(const std::filesystem::path& p) {
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

}  // namespace

std::string localCacheDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  std::filesystem::path dir = std::filesystem::path(buf) / L"pqdaw";
#else
  const char* home = std::getenv("HOME");
  if (!home) return {};
  std::filesystem::path dir = std::filesystem::path(home) / ".cache" / "pqdaw";
#endif
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec && !std::filesystem::is_directory(dir, ec)) return {};
  return utf8Of(dir);
}

std::string pipelineCachePath() {
  const std::string dir = localCacheDir();
  return dir.empty() ? std::string() : utf8Of(pathOf(dir) / "vk_pipeline_cache.bin");
}

std::vector<uint8_t> loadPipelineCache(const std::string& path) {
  if (path.empty()) return {};
  std::ifstream f(pathOf(path), std::ios::binary);
  if (!f) return {};
  char head[kHeaderBytes];
  if (!f.read(head, kHeaderBytes)) return {};
  uint64_t size = 0, hash = 0;
  std::memcpy(&size, head + 8, 8);
  std::memcpy(&hash, head + 16, 8);
  if (std::memcmp(head, kMagic, 8) != 0 || size == 0 || size > kMaxPayload) return {};
  std::vector<uint8_t> blob(static_cast<size_t>(size));
  if (!f.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(size))) return {};
  if (f.peek() != std::char_traits<char>::eof()) return {};  // trailing bytes: not what we wrote
  if (fnv1a64(blob) != hash) return {};
  return blob;
}

bool savePipelineCache(const std::string& path, std::span<const uint8_t> blob) {
  if (path.empty() || blob.empty() || blob.size() > kMaxPayload) return false;
  const std::filesystem::path target = pathOf(path);
  std::filesystem::path tmp = target;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    char head[kHeaderBytes];
    const uint64_t size = blob.size(), hash = fnv1a64(blob);
    std::memcpy(head, kMagic, 8);
    std::memcpy(head + 8, &size, 8);
    std::memcpy(head + 16, &hash, 8);
    f.write(head, kHeaderBytes);
    f.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    f.flush();
    if (!f) {
      f.close();
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
      return false;
    }
  }
#ifdef _WIN32
  if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
#else
  std::error_code ec;
  std::filesystem::rename(tmp, target, ec);
  return !ec;
#endif
}

}  // namespace pq::app
