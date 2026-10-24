// M3 review probe: does crossing a constant-order boundary of the same size keep the mode rotation in the explorer
// (transferModeRotation), as pqdaw's fixed-rotation tuning node assumes? Not product code.
#include <cstdio>

#include "pq/tuning/mos.h"

using namespace pq::tuning;

int main() {
  struct Case { int n; double below, above; };
  const Case cases[] = {{7, 719.9, 720.1}, {7, 685.6, 685.8}, {5, 719.9, 720.1}, {12, 699.9, 700.1}, {7, 599.9, 600.1}};
  for (const Case& c : cases) {
    std::printf("n=%d crossing %.1f -> %.1f:", c.n, c.below, c.above);
    int changed = 0;
    for (int r = 0; r < c.n; ++r) {
      const int t = transferModeRotation(c.below, c.n, r, c.above, c.n, 1200.0, nullptr);
      std::printf(" %d->%d", r, t);
      changed += t != r;
    }
    std::printf("   (%d of %d rotations change)\n", changed, c.n);
  }
  return 0;
}
