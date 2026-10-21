#include "pq/graph/patch_file.h"

#include <algorithm>
#include <utility>

namespace pq::graph {

const Patch* PatchFile::find(std::string_view name) const noexcept {
  for (const Patch& p : patches)
    if (p.name() == name) return &p;
  return nullptr;
}

SubcircuitRef subcircuitSignature(const Patch& callee) {
  SubcircuitRef ref;
  ref.patch = callee.name();
  const Adjacency adj = callee.adjacency();
  auto collect = [&](NodeKind kind, std::vector<SubcircuitPort>& out) {
    for (NodeIndex n = 0; n < callee.nodeCount(); ++n) {
      const Node& nd = callee.node(n);
      if (nd.kind != kind) continue;
      SubcircuitPort port;
      port.name = callee.nodeName(n);
      switch (kind) {
        case NodeKind::Input:
          port.type = callee.outputType(n, 0);
          port.optional = adj.driver[callee.flatInput(n, 0)] != kNone;  // `in x : t = default`
          break;
        case NodeKind::Param:
          port.type = callee.outputType(n, 0);
          port.optional = true;
          break;
        default: port.type = callee.inputType(n, 0); break;  // out / export
      }
      out.push_back(std::move(port));
    }
  };
  collect(NodeKind::Input, ref.inputs);
  collect(NodeKind::Param, ref.inputs);
  collect(NodeKind::Output, ref.outputs);
  collect(NodeKind::Export, ref.outputs);
  return ref;
}

std::vector<Error> PatchFile::validateAll() const {
  std::vector<Error> errors;
  auto fail = [&](ErrorCode code, std::string msg) { errors.push_back(Error{code, std::move(msg), {}}); };

  for (const std::string& imp : imports)
    if (imp.empty()) fail(ErrorCode::InvalidArgument, "empty import path");

  std::vector<std::string_view> names;
  for (const Patch& p : patches) {
    std::vector<Error> e = p.validateAll();
    errors.insert(errors.end(), std::make_move_iterator(e.begin()), std::make_move_iterator(e.end()));
    names.push_back(p.name());
  }
  std::sort(names.begin(), names.end());
  for (size_t i = 1; i < names.size(); ++i)
    if (names[i] == names[i - 1]) fail(ErrorCode::InvalidArgument, "patch '" + std::string(names[i]) + "' is defined twice");

  // Local call graph: callees[i] lists the indices of local patches that patch i instantiates.
  const size_t np = patches.size();
  std::vector<std::vector<size_t>> callees(np);
  for (size_t i = 0; i < np; ++i) {
    const Patch& p = patches[i];
    for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
      if (p.node(n).kind != NodeKind::Subcircuit) continue;
      const SubcircuitRef& ref = p.subcircuit(n);
      const Patch* callee = find(ref.patch);
      if (!callee) continue;  // from an import
      callees[i].push_back(static_cast<size_t>(callee - patches.data()));
      if (!(ref == subcircuitSignature(*callee)))
        fail(ErrorCode::Type, "patch '" + p.name() + "': " + p.describe(n) +
                                  " does not match the current ports of patch '" + ref.patch + "'");
    }
  }

  // Recursion: iterative DFS with colours (0 white, 1 on stack, 2 done).
  std::vector<uint8_t> colour(np, 0);
  std::vector<std::pair<size_t, size_t>> stack;  // (patch, next callee position)
  for (size_t root = 0; root < np; ++root) {
    if (colour[root]) continue;
    stack.emplace_back(root, 0);
    colour[root] = 1;
    while (!stack.empty()) {
      auto& [v, next] = stack.back();
      if (next == callees[v].size()) {
        colour[v] = 2;
        stack.pop_back();
        continue;
      }
      const size_t c = callees[v][next++];
      if (colour[c] == 1) {
        fail(ErrorCode::InvalidArgument,
             "patch '" + patches[c].name() + "' instantiates itself (through '" + patches[v].name() + "')");
      } else if (colour[c] == 0) {
        colour[c] = 1;
        stack.emplace_back(c, 0);
      }
    }
  }
  return errors;
}

Status PatchFile::validate() const {
  std::vector<Error> errors = validateAll();
  if (errors.empty()) return Ok{};
  return std::move(errors.front());
}

}  // namespace pq::graph
