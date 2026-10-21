#pragma once

// Parser and printer of the patch language (docs/PATCH_LANGUAGE.md, v0), T1.2.
//
//   parse(text, "fm.pqp")  → PatchFile, or diagnostics `fm.pqp:3:9: error: …` with the source line and a caret
//   print(file)            → canonical text; parse(print(f)) == f for every file the parser produced
//
// Calls resolve to (in this order) a patch of the same file, an external patch (ParseOptions: imported files and a
// prelude such as the stdlib), or a leaf atom; a patch may not be named like an atom. The graph module does no file
// I/O: imports are recorded in PatchFile::imports and, if an ImportLoader is given, loaded through it.
//
// Not RT-safe: runs on the UI/compiler threads. The lexer works on string_views into the source (no per-token
// allocation) and the parser builds the IR directly from a small flat AST.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/graph/patch_file.h"

namespace pq::graph {

// One problem in a source file. `offset`/`length` are the byte span the caret marks (length ≥ 1 unless the span is
// the end of the file); `location` has the same position as 1-based line and column (column in code points).
struct Diagnostic {
  std::string message;  // without location or "error:" prefix, e.g. "unknown unit suffix 'xs' (units: …)"
  SourceLocation location;
  ErrorCode code = ErrorCode::Parse;  // Parse; Type for declared-type errors reported by validation
  uint32_t offset = 0;
  uint32_t length = 0;
};

// A byte range of a source file (offset kNone = unknown).
struct SourceSpan {
  uint32_t offset = kNone;
  uint32_t length = 0;
};

// `file:line:col: error: message`, the source line, and a caret line (`^~~~` under the span).
std::string formatDiagnostic(std::string_view source, const Diagnostic& d);

// A diagnostic at `span` of `source` with line/column filled in (for later phases that report against the text,
// e.g. inference through ParseResult::nodeSpans). An unknown span reports at the start of the file.
Diagnostic makeDiagnostic(std::string_view source, std::string_view fileName, SourceSpan span, std::string message,
                          ErrorCode code);

// Supplies the patches of an imported file. `path` is the import string as written; `importer` is the file name
// passed to parse() (for relative paths). Returns the signatures (subcircuitSignature) of the file's patches.
class ImportLoader {
public:
  virtual ~ImportLoader() = default;
  virtual Result<std::vector<SubcircuitRef>> load(std::string_view path, std::string_view importer) = 0;
};

struct ParseOptions {
  // Patches callable without an import in this file (e.g. a stdlib prelude), by signature.
  std::span<const SubcircuitRef> externals;
  // Loads `import`ed files. Without a loader, imports are only recorded and calls into them are unknown.
  ImportLoader* imports = nullptr;
  // Run PatchFile::validateAll() on the result and report its errors at the offending node's source position.
  bool validate = true;
};

struct ParseResult {
  PatchFile file;                        // complete only when diagnostics is empty
  std::vector<Diagnostic> diagnostics;   // in source order within each phase (syntax, names, validation)
  // [patch][node]: the source span of every node of every lowered patch (offset kNone for nodes without one), so
  // later phases (infer()) can report file:line:col. Empty when lowering did not run.
  std::vector<std::vector<SourceSpan>> nodeSpans;
  bool ok() const noexcept { return diagnostics.empty(); }
};

// Parses a whole file and reports every problem it finds (up to kMaxDiagnostics). Lowering to the IR runs only if
// the syntax is valid, and validation only if lowering succeeded, so later phases never report follow-on errors.
ParseResult parseDetailed(std::string_view source, std::string_view fileName, const ParseOptions& options = {});

// The file, or an Error (the first diagnostic's code) whose message is every
// diagnostic formatted with formatDiagnostic, joined by newlines, and whose location is the first diagnostic's.
Result<PatchFile> parse(std::string_view source, std::string_view fileName, const ParseOptions& options = {});

// Canonical text of a file: imports, then each patch (declarations, bindings in node order, exports, solve blocks,
// layout). Anonymous nodes with exactly one consumer are written inline; any other unnamed computed node (several
// consumers, dead code, a cycle with no named node, an endpoint of a routed wire, nesting deeper than
// kMaxExprDepth) gets a generated name `_n<index>`, so the output always parses, but parse(print(p)) == p holds
// only for patches text can express (which every parsed patch is).
std::string print(const PatchFile& file);
std::string print(const Patch& patch);

// Signatures of every patch in a file (what an ImportLoader returns for it).
std::vector<SubcircuitRef> signatures(const PatchFile& file);

inline constexpr uint32_t kMaxDiagnostics = 50;
// Limits that keep parsing and printing within a small, fixed stack (deep nesting is an error, not a crash).
// The printer keeps every expression within kMaxExprDepth, and printed text nests at most one level per tree level,
// so printed text always parses.
inline constexpr uint32_t kMaxExprDepth = 128;                // depth of one expression tree (a sum of 128 terms)
inline constexpr uint32_t kMaxNesting = kMaxExprDepth + 1;    // nested (), [], call arguments, ternary branches

}  // namespace pq::graph
