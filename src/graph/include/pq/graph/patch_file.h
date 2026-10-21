#pragma once

// A `.pqp` file: imports and patches (PATCH_LANGUAGE §2 `file`), plus the subcircuit signature of a patch.

#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/graph/patch.h"

namespace pq::graph {

struct PatchFile {
  std::vector<std::string> imports;  // `import "path"` strings, in order
  std::vector<Patch> patches;

  const Patch* find(std::string_view name) const noexcept;

  // Every patch's own problems (Patch::validateAll), plus: valid and unique patch names, non-empty import paths,
  // subcircuit nodes whose callee is in this file carry exactly the callee's current signature, and no patch
  // instantiates itself directly or through other patches of this file. Callees from imports are checked once the
  // importing compiler has loaded them (T1.2).
  std::vector<Error> validateAll() const;
  Status validate() const;

  friend bool operator==(const PatchFile&, const PatchFile&) = default;
};

// The ports a call to `callee` exposes: inputs = its `in`s (optional when they have a default) then its `param`s
// (always optional); outputs = its `out`s then its `export`s, each in declaration (node) order.
SubcircuitRef subcircuitSignature(const Patch& callee);

}  // namespace pq::graph
