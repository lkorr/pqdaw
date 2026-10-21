#include <doctest/doctest.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/rt_scope.h"

namespace {

// Volatile sink: the compiler may not elide an allocation whose result escapes here.
void* volatile gSink = nullptr;

using pq::alloc_trap::countRtViolations;
using pq::alloc_trap::ExpectViolations;
using pq::alloc_trap::Op;

}  // namespace

TEST_CASE("alloc trap: allocating outside an RtScope is fine") {
  REQUIRE(pq::alloc_trap::installed());
  ExpectViolations expect;
  CHECK_FALSE(pq::inRtScope());
  auto* p = new int(5);
  gSink = p;
  delete p;
  std::vector<int> v(1000, 1);
  v.push_back(2);
  std::string s(200, 'x');
  CHECK(expect.count() == 0);
}

TEST_CASE("alloc trap: allocation and free inside an RtScope are each caught") {
  REQUIRE(pq::alloc_trap::installed());
  const uint64_t before = pq::alloc_trap::violationCount();
  int* p = nullptr;

  ExpectViolations expect;
  {
    pq::RtScope rt;
    CHECK(pq::inRtScope());
    p = new int(7);
    gSink = p;
  }
  CHECK(expect.count() == 1);
  CHECK(expect.last().op == Op::New);
  CHECK(expect.last().size == sizeof(int));
  // The allocation still succeeded, so the test can continue.
  CHECK(*p == 7);

  {
    pq::RtScope rt;
    delete p;
  }
  CHECK(expect.count() == 2);
  CHECK(expect.last().op == Op::Delete);
  CHECK(expect.last().ptr == static_cast<void*>(p));
  CHECK(pq::alloc_trap::violationCount() - before == 2);
}

TEST_CASE("alloc trap: every replaceable operator new/delete overload is trapped") {
  auto rt = [](auto&& f) { return countRtViolations(f); };
  void* p = nullptr;
  constexpr auto kAl = std::align_val_t{128};

  CHECK(rt([&] { p = ::operator new(24); }) == 1);
  CHECK(rt([&] { ::operator delete(p); }) == 1);
  CHECK(rt([&] { p = ::operator new[](24); }) == 1);
  CHECK(rt([&] { ::operator delete[](p); }) == 1);
  CHECK(rt([&] { p = ::operator new(24, std::nothrow); }) == 1);
  CHECK(rt([&] { ::operator delete(p, std::nothrow); }) == 1);
  CHECK(rt([&] { p = ::operator new[](24, std::nothrow); }) == 1);
  CHECK(rt([&] { ::operator delete[](p, std::nothrow); }) == 1);
  CHECK(rt([&] { p = ::operator new(24); }) == 1);
  CHECK(rt([&] { ::operator delete(p, std::size_t{24}); }) == 1);
  CHECK(rt([&] { p = ::operator new[](24); }) == 1);
  CHECK(rt([&] { ::operator delete[](p, std::size_t{24}); }) == 1);

  CHECK(rt([&] { p = ::operator new(24, kAl); }) == 1);
  CHECK(reinterpret_cast<uintptr_t>(p) % 128 == 0);
  CHECK(rt([&] { ::operator delete(p, kAl); }) == 1);
  CHECK(rt([&] { p = ::operator new[](24, kAl); }) == 1);
  CHECK(rt([&] { ::operator delete[](p, kAl); }) == 1);
  CHECK(rt([&] { p = ::operator new(24, kAl, std::nothrow); }) == 1);
  CHECK(rt([&] { ::operator delete(p, kAl, std::nothrow); }) == 1);
  CHECK(rt([&] { p = ::operator new[](24, kAl, std::nothrow); }) == 1);
  CHECK(rt([&] { ::operator delete[](p, kAl, std::nothrow); }) == 1);
  CHECK(rt([&] { p = ::operator new(24, kAl); }) == 1);
  CHECK(rt([&] { ::operator delete(p, std::size_t{24}, kAl); }) == 1);
  CHECK(rt([&] { p = ::operator new[](24, kAl); }) == 1);
  CHECK(rt([&] { ::operator delete[](p, std::size_t{24}, kAl); }) == 1);

  // Deleting null frees nothing, so it is not a violation.
  CHECK(rt([&] { ::operator delete(nullptr); }) == 0);
}

TEST_CASE("alloc trap: library allocations (vector growth, string) inside an RtScope are caught") {
  std::vector<int> v;
  CHECK(countRtViolations([&] { v.push_back(1); }) >= 1);
  std::string s;
  CHECK(countRtViolations([&] { s.assign(100, 'y'); }) >= 1);
  // Work that does not allocate passes.
  v.reserve(64);
  CHECK(countRtViolations([&] {
          for (int i = 0; i < 32; ++i) v.push_back(i);
        }) == 0);
}

TEST_CASE("alloc trap: RtScope nests and is per-thread") {
  CHECK_FALSE(pq::inRtScope());
  {
    pq::RtScope a;
    {
      pq::RtScope b;
      CHECK(pq::inRtScope());
    }
    CHECK(pq::inRtScope());
    // Another thread is not realtime just because this one is.
    bool otherRt = true;
    {
      // Thread creation allocates: leave the scope's effect to the trap, which we expect to fire here.
      ExpectViolations expect;
      std::thread t([&] { otherRt = pq::inRtScope(); });
      t.join();
    }
    CHECK_FALSE(otherRt);
  }
  CHECK_FALSE(pq::inRtScope());
}

// Proves the test-binary wiring end to end: with no ExpectViolations, the handler installed by
// doctest_main.cpp must fail the running test case. doctest::should_fail inverts the verdict, so this case
// passes only if the trap really failed it.
TEST_CASE("alloc trap: an unexpected RT allocation fails the running test" * doctest::should_fail()) {
  pq::RtScope rt;
  auto* p = new int(1);
  gSink = p;
  delete p;
}

namespace {
std::atomic<int> gHandlerCalls{0};
std::atomic<uint32_t> gHandlerThread{0};
void countingHandler(const pq::alloc_trap::Violation& v) noexcept {
  gHandlerCalls.fetch_add(1);
  gHandlerThread.store(v.threadId);
}
}  // namespace

TEST_CASE("alloc trap: violations on other threads reach the handler") {
  const pq::alloc_trap::Handler previous = pq::alloc_trap::setHandler(&countingHandler);
  gHandlerCalls = 0;
  std::thread t([] {
    pq::RtScope rt;
    auto* p = new double(1.0);
    gSink = p;
    delete p;
  });
  t.join();
  pq::alloc_trap::setHandler(previous);
  CHECK(gHandlerCalls.load() == 2);
  CHECK(gHandlerThread.load() != 0);
}

#ifdef PQ_ALLOC_TRAP_PROBE
// The default handler (used by the app) must print the violation and a stack trace, then abort.
// Runs the probe executable, which allocates inside an RtScope without installing a handler.
TEST_CASE("alloc trap: default handler aborts with a stack trace") {
  const std::string cmd = std::string("\"\"") + PQ_ALLOC_TRAP_PROBE + "\" 2>&1\"";
  FILE* pipe = _popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  std::string out;
  char buf[512];
  while (std::fgets(buf, sizeof buf, pipe)) out += buf;
  const int status = _pclose(pipe);
  INFO("probe output:\n" << out);
  CHECK(status != 0);
  CHECK(out.find("heap allocation of 64 bytes") != std::string::npos);
  CHECK(out.find("inside RtScope") != std::string::npos);
  CHECK(out.find("stack trace") != std::string::npos);
  CHECK(out.find("probe: unreachable") == std::string::npos);
}
#endif
