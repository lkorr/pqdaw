// Shared main for every test executable. Installs an allocation-trap handler (pq/core/alloc_trap.h) that
// turns a heap allocation/free inside an RtScope into a failure of the running test case, instead of the
// app's default abort, so the rest of the suite still runs and reports.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <string>
#include <system_error>

#include <windows.h>

#include "pq/core/alloc_trap.h"

namespace {

void failRunningTest(const pq::alloc_trap::Violation& v) noexcept {
  char msg[256];
  pq::alloc_trap::describe(v, msg, sizeof msg);
  FAIL_CHECK(msg);
}

// --pq-shard=<k>/<n> (cmake/PqModule.cmake, T0.x-buildspeed2): keep only this process's share of the registered
// test cases, so CTest can run n processes of one executable in parallel. Within each doctest test suite, the
// cases are taken in doctest's registration order (a std::set, the same in every process of one binary) and dealt
// out round-robin: case i of a suite runs in shard i % n. Every case runs in exactly one shard of each entry, and
// the suite filters on the command line (--test-suite=slow, --test-suite-exclude=...) still apply afterwards.
void keepShard(int argc, char** argv) {
  int k = 0, n = 1;
  for (int i = 1; i < argc; ++i)
    if (std::strncmp(argv[i], "--pq-shard=", 11) == 0 && std::sscanf(argv[i] + 11, "%d/%d", &k, &n) != 2) n = 1;
  if (n <= 1 || k < 0 || k >= n) return;
  auto& tests = doctest::detail::getRegisteredTests();
  std::map<std::string, int> seen;
  for (auto it = tests.begin(); it != tests.end();) {
    const int i = seen[it->m_test_suite ? it->m_test_suite : ""]++;
    it = (i % n == k) ? std::next(it) : tests.erase(it);
  }
}

// One temporary directory per test process (%TEMP%\pq_test_<pid>), set as TMP/TEMP before any test runs and
// removed afterwards: many tests use fixed names under the temp directory, and CTest runs the shards of one
// executable (and other worktrees' suites) at the same time.
std::filesystem::path isolateTempDir() {
  wchar_t base[MAX_PATH + 1];
  const DWORD len = GetTempPathW(MAX_PATH + 1, base);
  if (len == 0 || len > MAX_PATH) return {};
  std::filesystem::path dir = std::filesystem::path(base) / (L"pq_test_" + std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  if (!std::filesystem::create_directories(dir, ec) || ec) return {};
  SetEnvironmentVariableW(L"TMP", dir.c_str());
  SetEnvironmentVariableW(L"TEMP", dir.c_str());
  _wputenv_s(L"TMP", dir.c_str());
  _wputenv_s(L"TEMP", dir.c_str());
  return dir;
}

}  // namespace

TEST_CASE("alloc trap is linked into this test executable") {
  CHECK(pq::alloc_trap::installed());
}

int main(int argc, char** argv) {
  pq::alloc_trap::setHandler(&failRunningTest);
  const std::filesystem::path tempDir = isolateTempDir();
  keepShard(argc, argv);
  doctest::Context context;
  // File names without directories in reports: with the shared compiler cache (cmake/PqCompilerCache.cmake)
  // an object may come from another worktree, and its __FILE__ would name that worktree's copy.
  context.setOption("no-path-filenames", true);
  context.applyCommandLine(argc, argv);
  const int result = context.run();
  if (!tempDir.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
  }
  return result;
}
