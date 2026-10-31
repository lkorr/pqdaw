#include "win_util.h"

#include <windows.h>

#include <algorithm>
#include <chrono>

namespace pq::library::detail {

std::wstring toWide(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
  return w;
}

std::string toUtf8(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int n =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring winPath(std::string_view utf8) {
  std::wstring w = toWide(utf8);
  for (wchar_t& c : w)
    if (c == L'/') c = L'\\';
  if (w.rfind(L"\\\\?\\", 0) == 0) return w;
  if (w.size() >= 3 && w[1] == L':' && w[2] == L'\\') return L"\\\\?\\" + w;
  if (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\') return L"\\\\?\\UNC\\" + w.substr(2);
  return w;
}

std::string joinPath(std::string_view root, std::string_view rel) {
  std::string p(root);
  while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
  if (!rel.empty()) {
    p.push_back('\\');
    p.append(rel);
  }
  return p;
}

bool writeFileAtomic(const std::wstring& file, const void* bytes, std::size_t size) {
  const std::wstring tmp = file + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const auto* p = static_cast<const uint8_t*>(bytes);
  std::size_t left = size;
  bool ok = true;
  while (left > 0 && ok) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(left, 1u << 30));
    DWORD written = 0;
    ok = WriteFile(h, p, chunk, &written, nullptr) && written == chunk;
    p += chunk;
    left -= chunk;
  }
  ok = ok && FlushFileBuffers(h);
  CloseHandle(h);
  if (ok) ok = MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  if (!ok) DeleteFileW(tmp.c_str());
  return ok;
}

double nowMs() noexcept {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace pq::library::detail
