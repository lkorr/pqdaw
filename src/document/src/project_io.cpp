// Project directory I/O (T3.5): UTF-8 paths → UTF-16 for the Windows API, atomic save (temp file + flush + rename).

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "pq/document/serialize.h"

namespace pq::doc {

namespace {

Error ioError(std::string what, const std::string& pathUtf8, DWORD code) {
  return Error{ErrorCode::Io, std::move(what) + " (Windows error " + std::to_string(code) + ")", {pathUtf8, 0, 0}};
}

Result<std::wstring> widen(std::string_view utf8) {
  if (utf8.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                                    nullptr, 0);
  if (n <= 0) return Error{ErrorCode::InvalidArgument, "path is not valid UTF-8", {}};
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
  return w;
}

std::string joinUtf8(std::string_view dir, std::string_view name) {
  std::string s(dir);
  if (!s.empty() && s.back() != '/' && s.back() != '\\') s += '\\';
  s += name;
  return s;
}

struct Handle {
  HANDLE h = INVALID_HANDLE_VALUE;
  ~Handle() {
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
  bool close() {
    const BOOL ok = CloseHandle(h);
    h = INVALID_HANDLE_VALUE;
    return ok != 0;
  }
};

Status writeAll(const std::wstring& path, const std::string& pathUtf8, std::string_view data, bool failMidway) {
  Handle f;
  f.h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f.h == INVALID_HANDLE_VALUE) return ioError("cannot create " + pathUtf8, pathUtf8, GetLastError());
  size_t done = 0;
  const size_t limit = failMidway ? data.size() / 2 : data.size();
  while (done < limit) {
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(limit - done, 1u << 30));
    DWORD written = 0;
    if (!WriteFile(f.h, data.data() + done, chunk, &written, nullptr) || written == 0)
      return ioError("cannot write " + pathUtf8, pathUtf8, GetLastError());
    done += written;
  }
  if (failMidway) return ioError("simulated write failure on " + pathUtf8, pathUtf8, ERROR_DISK_FULL);
  // Flush before the rename so a crash cannot leave a renamed but empty/partial file.
  if (!FlushFileBuffers(f.h)) return ioError("cannot flush " + pathUtf8, pathUtf8, GetLastError());
  if (!f.close()) return ioError("cannot close " + pathUtf8, pathUtf8, GetLastError());
  return Ok{};
}

}  // namespace

Status saveProject(std::string_view dirUtf8, const Project& p, const ViewState& view, const SaveOptions& options) {
  Result<std::string> text = serializeProject(p, view);
  if (!text) return std::move(text).error();

  Result<std::wstring> dir = widen(dirUtf8);
  if (!dir) return std::move(dir).error();
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(*dir), ec);
  if (ec) return Error{ErrorCode::Io, "cannot create directory: " + ec.message(), {std::string(dirUtf8), 0, 0}};

  const std::string finalUtf8 = joinUtf8(dirUtf8, kProjectFileName);
  const std::string tmpUtf8 = finalUtf8 + ".tmp";
  Result<std::wstring> finalW = widen(finalUtf8);
  Result<std::wstring> tmpW = widen(tmpUtf8);
  if (!finalW || !tmpW) return Error{ErrorCode::InvalidArgument, "path is not valid UTF-8", {}};

  Status w = writeAll(*tmpW, tmpUtf8, *text, options.failAt == SaveFailPoint::WhileWriting);
  if (w && options.failAt == SaveFailPoint::BeforeRename)
    w = ioError("simulated failure before renaming " + tmpUtf8, tmpUtf8, ERROR_WRITE_FAULT);
  if (!w) {
    DeleteFileW(tmpW->c_str());
    return w;
  }
  if (!MoveFileExW(tmpW->c_str(), finalW->c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD code = GetLastError();
    DeleteFileW(tmpW->c_str());
    return ioError("cannot replace " + finalUtf8, finalUtf8, code);
  }
  return Ok{};
}

Result<LoadedProject> loadProject(std::string_view dirUtf8, const LoadOptions& options) {
  const std::string pathUtf8 = joinUtf8(dirUtf8, kProjectFileName);
  Result<std::wstring> path = widen(pathUtf8);
  if (!path) return std::move(path).error();
  Handle f;
  f.h = CreateFileW(path->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                    nullptr);
  if (f.h == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
      return Error{ErrorCode::NotFound, "no project file at " + pathUtf8, {pathUtf8, 0, 0}};
    return ioError("cannot open " + pathUtf8, pathUtf8, code);
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(f.h, &size)) return ioError("cannot read the size of " + pathUtf8, pathUtf8, GetLastError());
  if (size.QuadPart > (int64_t{1} << 31))
    return Error{ErrorCode::Io, pathUtf8 + " is larger than 2 GB", {pathUtf8, 0, 0}};
  std::string text(static_cast<size_t>(size.QuadPart), '\0');
  size_t done = 0;
  while (done < text.size()) {
    DWORD got = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(text.size() - done, 1u << 30));
    if (!ReadFile(f.h, text.data() + done, chunk, &got, nullptr)) return ioError("cannot read " + pathUtf8, pathUtf8, GetLastError());
    if (got == 0) break;
    done += got;
  }
  text.resize(done);
  LoadOptions o = options;
  if (o.fileName.empty()) o.fileName = pathUtf8;
  return parseProject(text, o);
}

Status saveProject(std::string_view dirUtf8, const DocumentStore& store, const SaveOptions& options) {
  return saveProject(dirUtf8, *store.current(), store.view(), options);
}

Status openProject(std::string_view dirUtf8, DocumentStore& store, const LoadOptions& options) {
  Result<LoadedProject> r = loadProject(dirUtf8, options);
  if (!r) return std::move(r).error();
  store.reset(r->project);
  store.view() = std::move(r->view);
  return Ok{};
}

}  // namespace pq::doc
