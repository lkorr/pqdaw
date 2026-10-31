// T5.11: user-library file I/O and paths (pq/document/library_files.h).

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "pq/document/library_files.h"

namespace pq::doc {

namespace fs = std::filesystem;

namespace {

fs::path pathOf(std::string_view utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::string utf8Of(const fs::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

bool isAbsolute(std::string_view p) {
  if (p.size() >= 2 && p[1] == ':') return true;  // "C:/…"
  return p.size() >= 2 && (p[0] == '/' || p[0] == '\\') && (p[1] == '/' || p[1] == '\\');  // UNC
}

}  // namespace

std::string resolveLibraryPath(std::string_view baseDir, std::string_view stored) {
  std::string s(stored);
  std::replace(s.begin(), s.end(), '\\', '/');
  if (isAbsolute(s) || baseDir.empty()) return s;
  return utf8Of((pathOf(baseDir) / pathOf(s)).lexically_normal());
}

std::string relativeLibraryPath(std::string_view baseDir, std::string_view file) {
  const fs::path f = pathOf(file).lexically_normal();
  if (!baseDir.empty()) {
    const fs::path rel = f.lexically_relative(pathOf(baseDir).lexically_normal());
    const std::string r = utf8Of(rel);
    if (!rel.empty() && r.rfind("..", 0) != 0 && !isAbsolute(r)) return r;
  }
  return utf8Of(f);
}

Result<std::string> readTextFile(std::string_view pathUtf8) {
  std::ifstream in(pathOf(pathUtf8), std::ios::binary);
  if (!in) {
    std::error_code ec;
    const bool exists = fs::exists(pathOf(pathUtf8), ec);
    return Error{exists ? ErrorCode::Io : ErrorCode::NotFound, std::string(exists ? "cannot read " : "no file ") +
                                                                   std::string(pathUtf8), {std::string(pathUtf8), 0, 0}};
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) return Error{ErrorCode::Io, "cannot read " + std::string(pathUtf8), {std::string(pathUtf8), 0, 0}};
  return text;
}

Status writeTextFileAtomic(std::string_view pathUtf8, std::string_view text) {
  const fs::path target = pathOf(pathUtf8);
  std::error_code ec;
  if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
  fs::path tmp = target;
  tmp += u8".part";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return Error{ErrorCode::Io, "cannot create " + utf8Of(tmp), {std::string(pathUtf8), 0, 0}};
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) {
      out.close();
      fs::remove(tmp, ec);
      return Error{ErrorCode::Io, "cannot write " + utf8Of(tmp), {std::string(pathUtf8), 0, 0}};
    }
  }
  fs::rename(tmp, target, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return Error{ErrorCode::Io, "cannot replace " + std::string(pathUtf8), {std::string(pathUtf8), 0, 0}};
  }
  return Ok{};
}

}  // namespace pq::doc
