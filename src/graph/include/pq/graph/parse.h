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
#include "pq/graph/id_match.h"
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
  // The previous parse of this text (T1.x-text-ids): when set, the result's nodes inherit the NodeIds of their
  // counterparts in it (inheritNodeIds, id_match.h; patches paired by name), so hot-swap state keys survive the
  // edit. Applied to every result whose lowering succeeded (ParseResult::lowered; validation errors do not change
  // the graph's structure, so a circuit editor keeps its NodeIds while the text has a dangling port, T4.4).
  const PatchFile* previous = nullptr;
  // T4.3: layout entries whose key names no node, port or wire (a stale line after a text edit) are dropped and
  // counted in ParseResult::layoutDropped instead of being errors. The layout is never read by the compiler, so a
  // text pane can keep editing an expression whose layout key moved.
  bool lenientLayout = false;
  // T4.4: a call without one of its required arguments is not a lowering error; validation reports the input as a
  // dangling port instead (located at the node). The circuit editor prints graphs whose required inputs are not wired
  // yet (`t = tanh()`), and their text must still lower to the same graph.
  bool deferMissingInputs = false;
};

// Where a patch sits in its source text (T4.3: spliceLayout).
struct PatchTextSpans {
  SourceSpan layout;        // `layout { … }` from the keyword to its '}' (offset kNone: no layout block)
  uint32_t close = kNone;   // offset of the patch's closing '}'
  uint32_t begin = kNone;   // offset of the `patch` keyword (T4.4: a circuit edit reprints [begin, close])
};

struct ParseResult {
  PatchFile file;                        // complete only when diagnostics is empty
  std::vector<Diagnostic> diagnostics;   // in source order within each phase (syntax, names, validation)
  // [patch][node]: the source span of every node of every lowered patch (offset kNone for nodes without one), so
  // later phases (infer()) can report file:line:col. Empty when lowering did not run.
  std::vector<std::vector<SourceSpan>> nodeSpans;
  IdMatchStats idMatch;                  // with ParseOptions::previous: what kept its NodeId
  std::vector<PatchTextSpans> patchSpans;  // [patch], filled when the syntax is valid
  uint32_t layoutDropped = 0;            // ParseOptions::lenientLayout: layout entries dropped
  // T4.4: syntax, names and lowering succeeded, so `file` holds the whole graph; any diagnostics are validation
  // errors (dangling ports, declared types, signatures).
  bool lowered = false;
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
// consumers, dead code, a cycle with no named node, nesting deeper than kMaxExprDepth) gets a generated name
// `_n<index>`, so the output always parses, but parse(print(p)) == p holds only for patches text can express (which
// every parsed patch is). Layout lines name an inlined node by its key (T4.3, PATCH_LANGUAGE §2.2): the key of its
// consumer, a dot, and the consumer's input it drives (`out.in.a`); a node under a solve equation starts at `_e<k>`
// (the k-th equation). Placing or routing a node never changes how the rest of the patch prints.
std::string print(const PatchFile& file);
std::string print(const Patch& patch);

// The layout block of one patch exactly as print() writes it ("  layout {\n … \n  }\n"), or "" when the patch has
// no layout (no placed node, routed wire or comment).
std::string printLayout(const Patch& patch);

// T4.3: `source` with each patch's layout block replaced by the one printLayout() writes for laidOut.patches[i] —
// inserted before the patch's closing '}' when it had none, removed when the new one is empty — and every other byte
// unchanged (comments, formatting, the order of members). `parsed` is parseDetailed(source) (its patchSpans);
// `laidOut` has the same patches in the same order (normally parsed.file with layouts changed). Text → layout → text
// is byte-stable: splicing the layout a parse of the result reads back gives the result again.
Result<std::string> spliceLayout(std::string_view source, const ParseResult& parsed, const PatchFile& laidOut);

// Signatures of every patch in a file (what an ImportLoader returns for it).
std::vector<SubcircuitRef> signatures(const PatchFile& file);

inline constexpr uint32_t kMaxDiagnostics = 50;
// Limits that keep parsing and printing within a small, fixed stack (deep nesting is an error, not a crash).
// The printer keeps every expression within kMaxExprDepth, and printed text nests at most one level per tree level,
// so printed text always parses.
inline constexpr uint32_t kMaxExprDepth = 128;                // depth of one expression tree (a sum of 128 terms)
inline constexpr uint32_t kMaxNesting = kMaxExprDepth + 1;    // nested (), [], call arguments, ternary branches

}  // namespace pq::graph
