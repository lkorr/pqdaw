#pragma once

// Loading and comparing the Node-generated mos-explorer fixtures (tests/tuning/fixtures/mos_*.json, written by
// tools/fixtures/gen_mos_fixtures.mjs from the original JavaScript).

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pq/tuning/mos.h"

namespace mos_fixture {

using Json = nlohmann::json;

// Every numeric output must match the JS within this many cents (T2.3 acceptance).
inline constexpr double kTol = 1e-6;

inline Json load(const char* name) {
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/tuning/fixtures/" + name;
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "missing fixture " << path);
  return Json::parse(in);
}

// A fixture number: JSON number, or "Infinity" / "-Infinity" / "NaN" for non-finite values.
inline double num(const Json& j) {
  if (j.is_string()) {
    const std::string s = j.get<std::string>();
    if (s == "Infinity") return std::numeric_limits<double>::infinity();
    if (s == "-Infinity") return -std::numeric_limits<double>::infinity();
    return std::numeric_limits<double>::quiet_NaN();
  }
  return j.get<double>();
}

inline bool near(double actual, const Json& expected, double tol = kTol) {
  const double e = num(expected);
  if (std::isnan(e)) return std::isnan(actual);
  if (std::isinf(e)) return actual == e;
  return std::fabs(actual - e) <= tol;
}

inline bool nearAll(const std::vector<double>& actual, const Json& expected, double tol = kTol) {
  if (actual.size() != expected.size()) return false;
  for (size_t i = 0; i < actual.size(); ++i)
    if (!near(actual[i], expected[i], tol)) return false;
  return true;
}

inline std::vector<double> doubles(const Json& j) {
  std::vector<double> v;
  for (const Json& x : j) v.push_back(num(x));
  return v;
}

inline std::vector<int32_t> ints(const Json& j) {
  std::vector<int32_t> v;
  for (const Json& x : j) v.push_back(x.get<int32_t>());
  return v;
}

// The region a case had selected (S.selected), or none.
inline std::optional<pq::tuning::RegionHint> hint(const Json& sel) {
  if (sel.is_null()) return std::nullopt;
  return pq::tuning::RegionHint{sel["perPeriodSize"].get<int32_t>(), sel["lowCents"].get<double>(),
                                sel["highCents"].get<double>()};
}

inline std::optional<std::string> optString(const Json& j) {
  if (j.is_null()) return std::nullopt;
  return j.get<std::string>();
}

}  // namespace mos_fixture
