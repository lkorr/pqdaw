#include "file_io.h"

#include <windows.h>

#include <utility>

namespace pq::media::detail {

Error ioError(const std::filesystem::path& path, const char* what) {
  const DWORD code = GetLastError();
  return Error{ErrorCode::Io, std::string(what) + " " + path.string() + " (Win32 error " + std::to_string(code) + ")", {}};
}

namespace {

Result<FileIdentity> identityOf(HANDLE h, const std::filesystem::path& path) {
  FileIdentity id;
  FILE_ID_INFO fid{};
  if (GetFileInformationByHandleEx(h, FileIdInfo, &fid, sizeof fid)) {
    id.volumeSerial = fid.VolumeSerialNumber;
    static_assert(sizeof fid.FileId.Identifier == 16);
    std::memcpy(id.fileId.data(), fid.FileId.Identifier, 16);
  } else {
    // FAT/exFAT and some network file systems: the 64-bit index from the basic handle information.
    BY_HANDLE_FILE_INFORMATION bh{};
    if (!GetFileInformationByHandle(h, &bh)) return ioError(path, "cannot query");
    id.volumeSerial = bh.dwVolumeSerialNumber;
    const uint64_t index = (uint64_t{bh.nFileIndexHigh} << 32) | bh.nFileIndexLow;
    std::memcpy(id.fileId.data(), &index, 8);
  }
  FILE_BASIC_INFO basic{};
  if (!GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic)) return ioError(path, "cannot query");
  id.mtime = basic.LastWriteTime.QuadPart;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size)) return ioError(path, "cannot query");
  id.size = static_cast<uint64_t>(size.QuadPart);
  return id;
}

}  // namespace

Result<File> File::open(const std::filesystem::path& path, Access access) {
  DWORD flags = FILE_ATTRIBUTE_NORMAL;
  if (access == Access::Sequential) flags |= FILE_FLAG_SEQUENTIAL_SCAN;
  if (access == Access::Overlapped) flags |= FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN;
  if (access == Access::Random) flags |= FILE_FLAG_RANDOM_ACCESS;
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, flags, nullptr);
  if (h == INVALID_HANDLE_VALUE) return ioError(path, "cannot open");
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size)) {
    Error e = ioError(path, "cannot query");
    CloseHandle(h);
    return e;
  }
  File f;
  f.handle_ = h;
  f.size_ = static_cast<uint64_t>(size.QuadPart);
  f.path_ = path;
  return f;
}

File::~File() { close(); }
File::File(File&& o) noexcept
    : handle_(std::exchange(o.handle_, nullptr)), size_(std::exchange(o.size_, 0)), path_(std::move(o.path_)) {}
File& File::operator=(File&& o) noexcept {
  if (this != &o) {
    close();
    handle_ = std::exchange(o.handle_, nullptr);
    size_ = std::exchange(o.size_, 0);
    path_ = std::move(o.path_);
  }
  return *this;
}
void File::close() noexcept {
  if (handle_) CloseHandle(handle_);
  handle_ = nullptr;
}

Result<std::size_t> File::readAt(uint64_t offset, void* dst, std::size_t n) const {
  if (offset >= size_) return std::size_t{0};
  if (n > size_ - offset) n = static_cast<std::size_t>(size_ - offset);
  std::size_t done = 0;
  while (done < n) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(n - done, 1u << 30));
    OVERLAPPED ov{};
    const uint64_t at = offset + done;
    ov.Offset = static_cast<DWORD>(at);
    ov.OffsetHigh = static_cast<DWORD>(at >> 32);
    DWORD got = 0;
    if (!ReadFile(handle_, static_cast<uint8_t*>(dst) + done, chunk, &got, &ov)) {
      const DWORD err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        if (!GetOverlappedResult(handle_, &ov, &got, TRUE)) return ioError(path_, "cannot read");
      } else if (err == ERROR_HANDLE_EOF) {
        break;
      } else {
        return ioError(path_, "cannot read");
      }
    }
    if (got == 0) break;
    done += got;
  }
  return done;
}

Status File::readExact(uint64_t offset, void* dst, std::size_t n) const {
  auto r = readAt(offset, dst, n);
  if (!r) return std::move(r).error();
  if (*r != n) return Error{ErrorCode::Io, "unexpected end of file " + path_.string(), {}};
  return Ok{};
}

Result<FileIdentity> File::identity() const { return identityOf(handle_, path_); }

Result<MappedFile> MappedFile::open(const std::filesystem::path& path) {
  auto f = File::open(path, File::Access::Sequential);
  if (!f) return std::move(f).error();
  auto id = f->identity();
  if (!id) return std::move(id).error();
  MappedFile m;
  m.identity_ = *id;
  m.size_ = static_cast<std::size_t>(f->size());
  if (m.size_ == 0) return m;
  HANDLE mapping = CreateFileMappingW(f->handle(), nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!mapping) return ioError(path, "cannot map");
  const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (!view) {
    Error e = ioError(path, "cannot map");
    CloseHandle(mapping);
    return e;
  }
  m.mapping_ = mapping;
  m.view_ = static_cast<const uint8_t*>(view);
  return m;  // the file handle may close: the mapping keeps the file open
}

MappedFile::~MappedFile() { close(); }
MappedFile::MappedFile(MappedFile&& o) noexcept
    : file_(std::exchange(o.file_, nullptr)),
      mapping_(std::exchange(o.mapping_, nullptr)),
      view_(std::exchange(o.view_, nullptr)),
      size_(std::exchange(o.size_, 0)),
      identity_(o.identity_) {}
MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
  if (this != &o) {
    close();
    file_ = std::exchange(o.file_, nullptr);
    mapping_ = std::exchange(o.mapping_, nullptr);
    view_ = std::exchange(o.view_, nullptr);
    size_ = std::exchange(o.size_, 0);
    identity_ = o.identity_;
  }
  return *this;
}
void MappedFile::close() noexcept {
  if (view_) UnmapViewOfFile(view_);
  if (mapping_) CloseHandle(mapping_);
  view_ = nullptr;
  mapping_ = nullptr;
  size_ = 0;
}

}  // namespace pq::media::detail
