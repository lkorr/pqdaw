// The build line of the bug report (T5.x-bugreport). The only file that includes the generated pq_build_info.h, so a new
// commit recompiles this file alone.
#include "build_info.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>

#include "pq_build_info.h"

namespace pq::app {

BuildInfo buildInfo() {
  BuildInfo b;
  b.commit = PQ_BUILD_COMMIT;
  b.branch = PQ_BUILD_BRANCH;
  b.commitDate = PQ_BUILD_COMMIT_DATE;
  b.dirty = PQ_BUILD_DIRTY != 0;
  b.preset = PQ_BUILD_PRESET;
#ifdef PQ_OPTIMIZED
  b.optimized = true;
#endif
#ifndef NDEBUG
  b.asserts = true;
#endif
  wchar_t path[32768];
  const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (n > 0 && n < std::size(path) && GetFileAttributesExW(path, GetFileExInfoStandard, &a)) {
    FILETIME local{};
    SYSTEMTIME st{};
    if (FileTimeToLocalFileTime(&a.ftLastWriteTime, &local) && FileTimeToSystemTime(&local, &st)) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
      b.builtAt = buf;
    }
  }
  return b;
}

std::string buildInfoLine() {
  const BuildInfo b = buildInfo();
  std::string s = "`" + b.commit + "`" + (b.dirty ? " + uncommitted changes" : "") + " on " + b.branch + " (committed " +
                  b.commitDate + "); " + b.preset + " preset (" + (b.optimized ? "optimised" : "unoptimised") + ", asserts " +
                  (b.asserts ? "on" : "off") + ")";
  if (!b.builtAt.empty()) s += "; exe written " + b.builtAt;
  return s;
}

}  // namespace pq::app
