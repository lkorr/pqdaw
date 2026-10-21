// Helper process for test_alloc_trap.cpp: allocates inside an RtScope with the default (app) handler,
// which must print a stack trace and abort. Never reaches the final print.
#include <cstdio>
#include <cstdlib>

#include "pq/core/rt_scope.h"

namespace {
void* volatile gSink = nullptr;
}

int main() {
  // No "abort() has been called" box and no Windows Error Reporting dialog in tests.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  {
    pq::RtScope rt;
    gSink = ::operator new(64);
  }
  std::puts("probe: unreachable");
  return 0;
}
