// Shared main for every test executable. Installs an allocation-trap handler (pq/core/alloc_trap.h) that
// turns a heap allocation/free inside an RtScope into a failure of the running test case, instead of the
// app's default abort, so the rest of the suite still runs and reports.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "pq/core/alloc_trap.h"

namespace {

void failRunningTest(const pq::alloc_trap::Violation& v) noexcept {
  char msg[256];
  pq::alloc_trap::describe(v, msg, sizeof msg);
  FAIL_CHECK(msg);
}

}  // namespace

TEST_CASE("alloc trap is linked into this test executable") {
  CHECK(pq::alloc_trap::installed());
}

int main(int argc, char** argv) {
  pq::alloc_trap::setHandler(&failRunningTest);
  doctest::Context context(argc, argv);
  return context.run();
}
