#pragma once

// Tests only (T1.x-text-ids): binding renames in .pqp text, each checked to be exactly that rename of the IR (the
// text helper is a plain identifier replacement; a rename that would also hit a named argument is skipped).

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "pq/graph/parse.h"
#include "text_edits.h"
#include "text_util.h"

namespace rename_edits {

namespace fs = std::filesystem;
using namespace pq::graph;

inline bool isDeclaration(NodeKind k) {
  return k == NodeKind::Param || k == NodeKind::Input || k == NodeKind::Output || k == NodeKind::Export ||
         k == NodeKind::Unknown;
}

// (patch, node) of every named binding (a named node that is not a declaration).
inline std::vector<std::pair<size_t, NodeIndex>> bindings(const PatchFile& f) {
  std::vector<std::pair<size_t, NodeIndex>> out;
  for (size_t k = 0; k < f.patches.size(); ++k)
    for (NodeIndex v = 0; v < f.patches[k].nodeCount(); ++v)
      if (!f.patches[k].nodeName(v).empty() && !isDeclaration(f.patches[k].node(v).kind)) out.emplace_back(k, v);
  return out;
}

inline bool parsesTo(const std::string& text, const fs::path& path, const PatchFile& expect) {
  text_util::DirLoader loader(path.parent_path());
  ParseOptions po;
  po.imports = &loader;
  const ParseResult r = parseDetailed(text, path.string(), po);
  return r.ok() && r.file == expect;
}

// `text` (parsed: `file`) with binding (k, v) renamed to name + "_renamed", or "" if the text rename is not exactly
// that IR rename.
inline std::string renameOne(const std::string& text, const fs::path& path, const PatchFile& file, size_t k, NodeIndex v) {
  const std::vector<std::string> lines = text_edits::splitLines(text);
  const auto patchLines = text_edits::patchLines(text_edits::lineStates(text), lines.size());
  if (k >= patchLines.size()) return {};
  const std::string& from = file.patches[k].nodeName(v);
  const std::string edited = text_edits::renameIdent(text, from, from + "_renamed", patchLines[k].open, patchLines[k].close);
  PatchFile expect = file;
  expect.patches[k].setNodeName(v, from + "_renamed", file.patches[k].node(v).nameOutput);
  return parsesTo(edited, path, expect) ? edited : std::string();
}

// Renames up to `limit` bindings at once (every one that renames cleanly): only structure can pair them then.
inline std::string renameAll(const std::string& text, const fs::path& path, const PatchFile& file, size_t limit, size_t* count = nullptr) {
  std::string cur = text;
  PatchFile curFile = file;
  size_t n = 0;
  for (const auto& [k, v] : bindings(file)) {
    if (n == limit) break;
    const std::string next = renameOne(cur, path, curFile, k, v);
    if (next.empty()) continue;
    curFile.patches[k].setNodeName(v, file.patches[k].nodeName(v) + "_renamed", file.patches[k].node(v).nameOutput);
    cur = next;
    ++n;
  }
  if (count) *count = n;
  return cur;
}

}  // namespace rename_edits
