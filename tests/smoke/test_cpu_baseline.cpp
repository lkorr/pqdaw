// The x86-64-v3 baseline query and the PQ_TEST_CPU_DISABLE hook (pq/core/cpu_baseline.h). The end-to-end
// check (pqdaw exits 1 with the message) is the pqdaw_cpu_check_* ctests (tests/app/cpu_baseline_check.cmake).

#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

#include "pq/core/cpu.h"
#include "pq/core/cpu_baseline.h"

namespace {

PqCpuBaseline allOn() {
  PqCpuBaseline b{};
  b.avx = b.avx2 = b.fma = b.bmi1 = b.bmi2 = b.lzcnt = b.f16c = b.movbe = true;
  return b;
}

std::string missing(const PqCpuBaseline& b, int* count = nullptr) {
  char buf[128];
  const int n = pq_missing_cpu_baseline(&b, buf, sizeof(buf));
  if (count) *count = n;
  return buf;
}

}  // namespace

TEST_CASE("cpu baseline: the host meets x86-64-v3 (the dev machine is an i7-11700F)") {
  REQUIRE_MESSAGE(std::getenv(PQ_CPU_DISABLE_ENV_VAR) == nullptr, "unset PQ_TEST_CPU_DISABLE to run this test");
  int n = -1;
  CHECK(missing(pq_query_cpu_baseline(), &n) == "");
  CHECK(n == 0);
  // cpuFeatures() takes AVX2/FMA from the same query.
  CHECK(pq::cpuFeatures().avx2);
  CHECK(pq::cpuFeatures().fma);
}

TEST_CASE("cpu baseline: missing features are listed in a fixed order") {
  PqCpuBaseline b = allOn();
  int n = -1;
  CHECK(missing(b, &n) == "");
  CHECK(n == 0);
  b.avx2 = false;
  CHECK(missing(b, &n) == "AVX2");
  CHECK(n == 1);
  b.fma = false;
  b.lzcnt = false;
  CHECK(missing(b, &n) == "AVX2, FMA, LZCNT");
  CHECK(n == 3);
  CHECK(missing(PqCpuBaseline{}, &n) == "AVX, AVX2, FMA, BMI1, BMI2, LZCNT, F16C, MOVBE");
  CHECK(n == 8);

  char small[6];
  const PqCpuBaseline none{};
  CHECK(pq_missing_cpu_baseline(&none, small, sizeof(small)) == 8);  // truncated, still terminated
  CHECK(std::string(small) == "AVX, ");
}

TEST_CASE("cpu baseline: PQ_TEST_CPU_DISABLE list syntax") {
  PqCpuBaseline b = allOn();
  pq_apply_cpu_disable_list(&b, nullptr);
  pq_apply_cpu_disable_list(&b, "");
  pq_apply_cpu_disable_list(&b, "nonsense,avx22,av");
  CHECK(missing(b) == "");

  pq_apply_cpu_disable_list(&b, "AVX2");
  CHECK(missing(b) == "AVX2");

  b = allOn();
  pq_apply_cpu_disable_list(&b, " fma , bmi2,,movbe");
  CHECK(missing(b) == "FMA, BMI2, MOVBE");

  b = allOn();
  pq_apply_cpu_disable_list(&b, "all");
  CHECK(missing(b) == "AVX, AVX2, FMA, BMI1, BMI2, LZCNT, F16C, MOVBE");
}

TEST_CASE("cpu baseline: pq_query_cpu_baseline applies the environment hook") {
  REQUIRE(_putenv_s(PQ_CPU_DISABLE_ENV_VAR, "avx2") == 0);
  const PqCpuBaseline forced = pq_query_cpu_baseline();
  REQUIRE(_putenv_s(PQ_CPU_DISABLE_ENV_VAR, "") == 0);  // an empty value removes the variable
  CHECK_FALSE(forced.avx2);
  CHECK(forced.avx);
  CHECK(forced.fma);
  CHECK(pq_query_cpu_baseline().avx2);
}
