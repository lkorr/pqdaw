#pragma once

// What this executable was built from (T5.x-bugreport): the commit, branch and dirty state at build time
// (cmake/PqBuildInfo.cmake, a header generated at every build that only build_info.cpp includes), the preset, and the
// executable's own modification time as the build date.

#include <string>

namespace pq::app {

struct BuildInfo {
  std::string commit;      // short hash, "unknown" without git
  std::string branch;
  std::string commitDate;  // committer date of that commit
  bool dirty = false;      // tracked files differed from the commit when it was built
  std::string preset;      // check / release / ship / dev
  bool optimized = false;  // PQ_OPTIMIZED
  bool asserts = false;    // !NDEBUG
  std::string builtAt;     // the executable's last write time (local), "" when unknown
};
BuildInfo buildInfo();
// One line for report.md: "538193b1f2c3 (dirty) on task/x, committed …; check preset (/O2, asserts on); exe built …".
std::string buildInfoLine();

}  // namespace pq::app
