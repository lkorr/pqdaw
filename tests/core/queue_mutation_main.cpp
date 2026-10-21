// T0.2-fix1 mutation check (manual tool, not a CTest test).
//
// tests/CMakeLists.txt generates mutated copies of spsc_ring.h and mpsc_queue.h at configure time: the
// first std::memory_order_acquire inside tryPop (the load of the producer's index / the cell sequence) is
// replaced by std::memory_order_relaxed. The shipping headers are untouched; this executable puts the
// mutant directory first on its include path and runs the 64-byte-payload stress drivers from
// queue_stress.h against the mutants. Any torn or stale slot means the tests can see the mutation.
//
// Run from the release build (/O2): build/release/tests/pq_queue_mutation.exe [rounds]

#include <cstdio>
#include <cstdlib>

#include "pq/core/mpsc_queue.h"
#include "pq/core/spsc_ring.h"
#include "queue_stress.h"

#if !defined(PQ_QUEUE_MUTANT_SPSC_TRYPOP_RELAXED) || !defined(PQ_QUEUE_MUTANT_MPSC_TRYPOP_RELAXED)
#error "queue_mutation_main.cpp must compile against the generated mutant headers (see tests/CMakeLists.txt)"
#endif

int main(int argc, char** argv) {
  const int rounds = argc > 1 ? std::atoi(argv[1]) : 20;
  uint64_t spscBad = 0, spscFrontBad = 0, mpscBad = 0, items = 0;
  for (int i = 0; i < rounds; ++i) {
    {
      pq::SpscRing<pq::test::WidePayload> r(16);
      const auto a = pq::test::spscWideStress(r, 4'000'000, false);
      spscBad += a.torn + a.seqErrors;
      pq::SpscRing<pq::test::WidePayload> r2(16);
      const auto b = pq::test::spscWideStress(r2, 4'000'000, true);
      spscFrontBad += b.torn + b.seqErrors;
      items += a.received + b.received;
    }
    {
      pq::MpscQueue<pq::test::WidePayload> q(16);
      const auto c = pq::test::mpscWideStress(q, 4, 1'000'000);
      mpscBad += c.torn + c.seqErrors;
      items += c.received;
    }
    std::printf("round %2d: spsc tryPop bad %llu, spsc tryPop+front bad %llu, mpsc bad %llu\n", i,
                static_cast<unsigned long long>(spscBad), static_cast<unsigned long long>(spscFrontBad),
                static_cast<unsigned long long>(mpscBad));
  }
  std::printf("MUTATION %s: %llu items checked over %d rounds; spsc %llu, mpsc %llu bad items\n",
              spscBad + spscFrontBad + mpscBad ? "DETECTED" : "NOT DETECTED", static_cast<unsigned long long>(items),
              rounds, static_cast<unsigned long long>(spscBad + spscFrontBad), static_cast<unsigned long long>(mpscBad));
  return 0;
}
