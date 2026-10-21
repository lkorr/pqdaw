// Stand-in for a test whose prerequisites are missing on this machine (e.g. the PQ Meter differential test
// without the PQ Meter source tree): prints why and exits with 77, which the test's SKIP_RETURN_CODE turns
// into "Skipped" in CTest. Never reports a pass.

#include <cstdio>

int main(int argc, char** argv) {
  std::fprintf(stderr, "SKIPPED: %s\n", argc > 1 ? argv[1] : "prerequisites missing");
  return 77;
}
