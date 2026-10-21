#pragma once

// Golden-image comparison shared by the UI test files (T0.7-fix5, M0 review M5). Test-only, header-only.

#include <doctest/doctest.h>

#include <string>
#include <utility>

#include "pq/ui/image.h"

namespace pq::ui::test {

inline std::string describe(const ImageDiff& d) {
  std::string s = "max channel diff " + std::to_string(d.maxChannelDiff) + ", " + std::to_string(d.pixelsOverTolerance) +
                  " px over tolerance (" + std::to_string(d.clusteredOutliers) + " clustered)";
  if (d.boxX1 > d.boxX0)
    s += ", bounding box x [" + std::to_string(d.boxX0) + ", " + std::to_string(d.boxX1) + ") y [" + std::to_string(d.boxY0) +
         ", " + std::to_string(d.boxY1) + ")";
  return s;
}

// The golden comparison (T0.7-fix5, M0 review M5): GoldenTolerance's defaults, i.e. ≤ 2/255 per channel
// everywhere except ≤ 100 isolated pixels per million, none over 24/255. On failure writes <name>.actual.png
// and <name>.diff.png (pq::ui::diffImage: outliers red/yellow, bounding box outlined) into the test's working
// directory (build/<preset>/tests) and reports the bounding box.
inline bool matchesGolden(const std::string& name, const Image& actual, const Image& golden, bool writeOnFailure = true,
                          ImageDiff* diffOut = nullptr) {
  const GoldenTolerance tol;
  const ImageDiff d = compareImages(actual, golden, tol.channel);
  if (diffOut) *diffOut = d;
  const bool pass = passesGolden(d, tol);
  if (!pass && writeOnFailure) {
    const std::string out = name + ".actual.png", diff = name + ".diff.png";
    (void)writePng(out, actual);
    if (d.sameSize) (void)writePng(diff, diffImage(actual, golden, tol.channel, tol.outlierCap));
    MESSAGE(name << " FAILED: " << (d.sameSize ? describe(d) : std::string("size differs")) << "; wrote " << out
                 << (d.sameSize ? " and " + diff : std::string()) << " for inspection");
  }
  return pass;
}

inline Image loadGolden(const std::string& file) {
  auto ref = readPng(std::string(PQ_TEST_DATA_DIR) + "/ui/golden/" + file + ".png");
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  return std::move(*ref);
}

}  // namespace pq::ui::test
