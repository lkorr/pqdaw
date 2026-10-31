#pragma once

// Helpers for the library tests: synthetic directory trees in %TEMP% (always deleted afterwards) and small files.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "pq/library/index.h"

namespace pq::library::test {

namespace fs = std::filesystem;

inline std::string utf8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

// A fresh directory under %TEMP%, removed recursively by the destructor.
class TempTree {
public:
  explicit TempTree(const char* tag) {
    static std::atomic<uint32_t> counter{0};
    root_ = fs::temp_directory_path() /
            ("pq_library_" + std::string(tag) + "_" + std::to_string(GetCurrentProcessId()) + "_" +
             std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    fs::remove_all(root_, ec);
    fs::create_directories(root_);
  }
  ~TempTree() { remove(); }
  TempTree(const TempTree&) = delete;
  TempTree& operator=(const TempTree&) = delete;

  void remove() {
    if (root_.empty()) return;
    // A directory handle (the watcher's) may close a moment after the indexer stops: retry briefly.
    for (int attempt = 0; attempt < 50; ++attempt) {
      std::error_code ec;
      fs::remove_all(root_, ec);
      if (!fs::exists(root_, ec)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    root_.clear();
  }
  const fs::path& root() const noexcept { return root_; }
  std::string rootUtf8() const { return utf8(root_); }

private:
  fs::path root_;
};

inline void writeBytes(const fs::path& p, const void* data, std::size_t n) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD w = 0;
  WriteFile(h, data, static_cast<DWORD>(n), &w, nullptr);
  CloseHandle(h);
}

inline void writeText(const fs::path& p, std::string_view text) { writeBytes(p, text.data(), text.size()); }

// A 16-bit PCM WAV with `frames` frames of a ramp.
inline std::vector<uint8_t> wavBytes(uint32_t frames, uint32_t rate = 48000, uint16_t channels = 2) {
  const uint32_t dataBytes = frames * channels * 2;
  std::vector<uint8_t> b(44 + dataBytes);
  auto put32 = [&](std::size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); };
  auto put16 = [&](std::size_t at, uint16_t v) { std::memcpy(b.data() + at, &v, 2); };
  std::memcpy(b.data(), "RIFF", 4);
  put32(4, 36 + dataBytes);
  std::memcpy(b.data() + 8, "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 1);
  put16(22, channels);
  put32(24, rate);
  put32(28, rate * channels * 2);
  put16(32, static_cast<uint16_t>(channels * 2));
  put16(34, 16);
  std::memcpy(b.data() + 36, "data", 4);
  put32(40, dataBytes);
  for (uint32_t i = 0; i < frames * channels; ++i) {
    const auto s = static_cast<int16_t>((i * 37) & 0x7FFF);
    std::memcpy(b.data() + 44 + 2 * i, &s, 2);
  }
  return b;
}

inline void writeWav(const fs::path& p, uint32_t frames, uint32_t rate = 48000, uint16_t channels = 2) {
  const std::vector<uint8_t> b = wavBytes(frames, rate, channels);
  writeBytes(p, b.data(), b.size());
}

// Every entry as (path, record), for comparing two indexes regardless of string-pool layout.
inline std::vector<std::pair<std::string, EntryRecord>> rows(const Index& idx) {
  std::vector<std::pair<std::string, EntryRecord>> out;
  out.reserve(idx.entryCount());
  for (uint32_t e = 0; e < idx.entryCount(); ++e) out.emplace_back(idx.path(e), idx.record(e));
  return out;
}
inline std::vector<std::string> dirRows(const Index& idx) {
  std::vector<std::string> out;
  for (uint32_t d = 0; d < idx.dirCount(); ++d) out.push_back(idx.dirPath(d));
  return out;
}

inline double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace pq::library::test
