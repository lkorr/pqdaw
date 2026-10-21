#include <doctest/doctest.h>

#include <string>

#include "pq/core/result.h"

namespace {
pq::Result<int> parsePositive(int v) {
  if (v <= 0) return pq::Error{pq::ErrorCode::InvalidArgument, "not positive", {"x.pqp", 3, 7}};
  return v;
}
}  // namespace

TEST_CASE("result: value and error paths") {
  auto good = parsePositive(5);
  REQUIRE(good.ok());
  CHECK(*good == 5);

  auto bad = parsePositive(-1);
  REQUIRE_FALSE(bad.ok());
  CHECK(bad.error().code == pq::ErrorCode::InvalidArgument);
  CHECK(bad.error().location.line == 3);
  CHECK(bad.error().location.col == 7);

  pq::Status s = pq::Ok{};
  CHECK(s.ok());
}

TEST_CASE("result: move-only payloads move out") {
  pq::Result<std::string> r = std::string(100, 'x');
  std::string taken = std::move(r).value();
  CHECK(taken.size() == 100);
}
