#pragma once

// UTF-8 ↔ UTF-16 and Win32 path helpers for the library module (not RT code).

#include <cstdint>
#include <string>
#include <string_view>

namespace pq::library::detail {

std::wstring toWide(std::string_view utf8);
std::string toUtf8(std::wstring_view wide);
// An absolute UTF-8 path as a Win32 path with the long-path prefix ("\\?\C:\…", "\\?\UNC\server\share\…"), '/'
// turned into '\\'. Relative paths are only converted.
std::wstring winPath(std::string_view utf8);
// place root + '\\' + rel (rel may be empty), UTF-8.
std::string joinPath(std::string_view root, std::string_view rel);
// Writes `bytes` to `file` atomically: file.tmp, flushed, renamed over `file`. False on any failure.
bool writeFileAtomic(const std::wstring& file, const void* bytes, std::size_t size);
// Monotonic milliseconds.
double nowMs() noexcept;

}  // namespace pq::library::detail
