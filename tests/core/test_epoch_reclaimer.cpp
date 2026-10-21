#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/epoch_reclaimer.h"

using pq::alloc_trap::countRtViolations;

namespace {
int gDeleted = 0;
void countDelete(void*) noexcept { ++gDeleted; }
int gObj[16];
}  // namespace

TEST_CASE("epoch: retired objects wait for the reader's next quiescent state") {
  gDeleted = 0;
  pq::EpochReclaimer rec(1);
  const auto reader = rec.registerReader();
  REQUIRE(reader.has_value());

  rec.retire(&gObj[0], &countDelete);
  CHECK(rec.pending() == 1);
  CHECK(rec.collect() == 0);  // reader has not passed a quiescent state since the retire
  CHECK(gDeleted == 0);

  rec.quiescent(*reader);
  rec.retire(&gObj[1], &countDelete);  // retired after that quiescent state: must wait for the next
  CHECK(rec.collect() == 1);
  CHECK(gDeleted == 1);
  CHECK(rec.pending() == 1);

  rec.quiescent(*reader);
  CHECK(rec.collect() == 1);
  CHECK(gDeleted == 2);
  CHECK(rec.pending() == 0);
  CHECK(rec.collect() == 0);
}

TEST_CASE("epoch: offline readers and no readers do not block reclamation") {
  gDeleted = 0;
  pq::EpochReclaimer rec(2);
  rec.retire(&gObj[0], &countDelete);
  CHECK(rec.collect() == 1);  // nobody registered

  const auto r = rec.registerReader();
  REQUIRE(r);
  rec.retire(&gObj[1], &countDelete);
  CHECK(rec.collect() == 0);
  rec.goOffline(*r);
  CHECK(rec.collect() == 1);

  rec.goOnline(*r);
  rec.retire(&gObj[2], &countDelete);
  CHECK(rec.collect() == 0);
  rec.unregisterReader(*r);
  CHECK(rec.collect() == 1);
  CHECK(gDeleted == 3);
}

TEST_CASE("epoch: every online reader must pass; slots are bounded and reusable") {
  gDeleted = 0;
  pq::EpochReclaimer rec(2);
  const auto a = rec.registerReader();
  const auto b = rec.registerReader();
  REQUIRE(a);
  REQUIRE(b);
  CHECK_FALSE(rec.registerReader().has_value());

  rec.retire(&gObj[0], &countDelete);
  rec.quiescent(*a);
  CHECK(rec.collect() == 0);  // b is still behind
  rec.quiescent(*b);
  CHECK(rec.collect() == 1);

  rec.unregisterReader(*a);
  const auto c = rec.registerReader();
  CHECK(c.has_value());
}

TEST_CASE("epoch: typed retire deletes; destructor frees whatever is pending") {
  struct Probe {
    explicit Probe(int* c) : counter(c) {}
    ~Probe() { ++*counter; }
    int* counter;
  };
  int destroyed = 0;
  {
    pq::EpochReclaimer rec(1);
    const auto r = rec.registerReader();
    REQUIRE(r);
    rec.retire(new Probe(&destroyed));
    rec.retire(new Probe(&destroyed));
    CHECK(rec.collect() == 0);
    rec.quiescent(*r);
    CHECK(rec.collect() == 2);
    CHECK(destroyed == 2);
    rec.retire(new Probe(&destroyed));
    rec.retire<Probe>(nullptr);  // ignored
    CHECK(rec.pending() == 1);
  }
  CHECK(destroyed == 3);
}

TEST_CASE("epoch: reader-side operations do not allocate (RtScope)") {
  pq::EpochReclaimer rec(1);
  const auto r = rec.registerReader();
  REQUIRE(r);
  CHECK(countRtViolations([&] {
          for (int i = 0; i < 100; ++i) rec.quiescent(*r);
          rec.goOffline(*r);
          rec.goOnline(*r);
          rec.quiescent(*r);
        }) == 0);
}

// A writer keeps publishing fresh objects and retiring old ones while an "audio" thread reads the
// published pointer and announces quiescent states. The "deleter" only marks objects freed, so a reader
// that ever sees a freed object while holding it proves a reclamation bug without touching freed memory.
TEST_CASE("epoch stress: a reader never holds a reclaimed object") {
  struct Obj {
    std::atomic<bool> freed{false};
    uint64_t payload = 0;
  };
  constexpr uint32_t kObjects = 100'000;
  auto objs = std::make_unique<Obj[]>(kObjects);
  for (uint32_t i = 0; i < kObjects; ++i) objs[i].payload = i;

  pq::EpochReclaimer rec(1);
  const auto r = rec.registerReader();
  REQUIRE(r);
  std::atomic<Obj*> published{&objs[0]};
  std::atomic<bool> done{false};
  std::atomic<uint64_t> freedCount{0};
  static std::atomic<uint64_t>* sFreed = nullptr;
  sFreed = &freedCount;
  const pq::EpochReclaimer::Deleter markFreed = [](void* p) noexcept {
    static_cast<Obj*>(p)->freed.store(true, std::memory_order_relaxed);
    sFreed->fetch_add(1, std::memory_order_relaxed);
  };

  uint64_t errors = 0;
  uint64_t reads = 0;
  uint64_t violations = 0;
  std::thread reader([&] {
    pq::alloc_trap::ExpectViolations expect;
    while (!done.load(std::memory_order_acquire)) {
      pq::RtScope rt;  // one "audio block"
      for (int k = 0; k < 4; ++k) {
        Obj* o = published.load(std::memory_order_acquire);
        if (o->freed.load(std::memory_order_relaxed)) ++errors;
        const uint64_t before = o->payload;
        for (int spin = 0; spin < 50; ++spin) reads += o->payload == before;
        if (o->freed.load(std::memory_order_relaxed)) ++errors;
      }
      rec.quiescent(*r);
    }
    rec.goOffline(*r);
    violations = expect.count();
  });

  std::thread writer([&] {
    for (uint32_t i = 1; i < kObjects; ++i) {
      Obj* old = published.exchange(&objs[i], std::memory_order_acq_rel);
      rec.retire(old, markFreed);
      if (i % 16 == 0) rec.collect();
    }
    // Liveness: with the reader still running, its quiescent states must let collect() free things.
    while (freedCount.load() == 0) {
      rec.collect();
      std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
  });

  writer.join();
  reader.join();
  const uint64_t freedDuringRun = freedCount.load();
  rec.collect();  // reader is offline now: everything retired can go

  CHECK(errors == 0);
  CHECK(violations == 0);
  CHECK(reads > 0);
  CHECK(freedDuringRun > 0);  // reclamation made progress while the reader was running
  CHECK(freedCount.load() == kObjects - 1);
  CHECK(rec.pending() == 0);
  CHECK_FALSE(objs[kObjects - 1].freed.load());  // the published object is never retired
}
