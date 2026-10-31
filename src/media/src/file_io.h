#pragma once

// Win32 file access for media (T5.1/T5.2): positional reads, the NTFS identity, read-only mappings. Not RT-safe.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "pq/core/result.h"
#include "pq/media/sample_info.h"

namespace pq::media::detail {

Error ioError(const std::filesystem::path& path, const char* what);  // message includes GetLastError's code

class File {
public:
  enum class Access : uint8_t {
    Random,      // positional reads, no read-ahead hint
    Sequential,  // FILE_FLAG_SEQUENTIAL_SCAN (aggressive read-ahead)
    Overlapped,  // FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN: the disk thread's asynchronous reads
  };

  File() = default;
  static Result<File> open(const std::filesystem::path& path, Access access);
  ~File();
  File(File&& o) noexcept;
  File& operator=(File&& o) noexcept;
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  bool isOpen() const noexcept { return handle_ != nullptr; }
  void* handle() const noexcept { return handle_; }
  uint64_t size() const noexcept { return size_; }
  const std::filesystem::path& path() const noexcept { return path_; }

  // Reads up to n bytes at `offset` (fewer only at the end of the file). Works on every access mode (an
  // overlapped handle waits for its read).
  Result<std::size_t> readAt(uint64_t offset, void* dst, std::size_t n) const;
  // Exactly n bytes, or an error.
  Status readExact(uint64_t offset, void* dst, std::size_t n) const;
  // Volume serial + 128-bit file ID, size, last-write time.
  Result<FileIdentity> identity() const;

private:
  void close() noexcept;
  void* handle_ = nullptr;
  uint64_t size_ = 0;
  std::filesystem::path path_;
};

// A whole file mapped read-only (empty files map to an empty span).
class MappedFile {
public:
  MappedFile() = default;
  static Result<MappedFile> open(const std::filesystem::path& path);
  ~MappedFile();
  MappedFile(MappedFile&& o) noexcept;
  MappedFile& operator=(MappedFile&& o) noexcept;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  std::span<const uint8_t> bytes() const noexcept { return {view_, size_}; }
  const FileIdentity& identity() const noexcept { return identity_; }

private:
  void close() noexcept;
  void* file_ = nullptr;
  void* mapping_ = nullptr;
  const uint8_t* view_ = nullptr;
  std::size_t size_ = 0;
  FileIdentity identity_;
};

}  // namespace pq::media::detail
