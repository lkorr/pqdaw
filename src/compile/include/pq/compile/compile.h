#pragma once

// pq_compile (T1.x-arch2, QUESTIONS Q73 option B): the ONE file → Schedule → ProgramImage pipeline, used by `pqc`
// (tools/pqc) and `pqdaw --play` (src/app), and the process's JIT compiler for every client (the DAW's background JIT
// images too). Before it, the loader and the graph pipeline existed twice (pqc and the app) and `--play` had no JIT.
//
//   SourceFile::load    a .pqp file with everything it imports (relative to the importing file, recursively), parsed
//                       and kept alive (the PatchLibrary points into it)
//   compilePatch        one patch: infer → optimise (specialised for the sample rate) → schedule, keeping every stage
//   buildImage          a Schedule → a ProgramImage on the interpreter or the LLVM JIT (bit-identical by default)
//   jitCompiler         the process's jit::Compiler (one LLJIT; created on first use, thread-safe; compiles are
//                       serialised inside it)
//   jitUnitBackend      an engine::UnitBackend that compiles units with it (ProjectCompiler, T1b.5)
// Not RT-safe anywhere (file I/O, compiling).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/program_image.h"
#include "pq/engine/project_compile.h"
#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"
#include "pq/jit/compiler.h"

namespace pq::compile {

// A .pqp file with everything it imports, parsed and kept alive (the PatchLibrary points into it).
class SourceFile {
public:
  // Errors: Io (unreadable file or import), Parse (every diagnostic of the failing file, formatted with
  // file:line:col, the source line and a caret, one after the other).
  static Result<std::unique_ptr<SourceFile>> load(const std::filesystem::path& path);
  ~SourceFile();

  const graph::PatchFile& file() const noexcept;
  const std::string& text() const noexcept;
  const std::string& fileName() const noexcept;
  // Index of the patch named `name` (empty: the first patch), or an error (NotFound) naming the patches there are.
  Result<size_t> findPatch(std::string_view name) const;
  // Callees: this file's patches, then every imported file's.
  const graph::PatchLibrary& library() const noexcept;
  // Source spans of patch `index`'s nodes (for file:line:col diagnostics in later phases).
  std::span<const graph::SourceSpan> nodeSpans(size_t index) const noexcept;

  struct Impl;

private:
  explicit SourceFile(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

struct CompileOptions {
  double sampleRate = 48000.0;
  bool visualMode = false;  // a delay-free cycle gets a z1 and a warning instead of an error
};

// One patch through the whole graph pipeline.
struct Compiled {
  std::string patchName;
  graph::TypedPatch inferred;    // after infer() (durations in seconds)
  graph::TypedPatch optimised;   // after optimise()
  graph::OptimiseStats optimiseStats;
  graph::Schedule schedule;
  std::vector<std::string> warnings;  // formatted (visualMode z1 insertions)
  // The patch every NodeIndex of the schedule refers to.
  const graph::TypedPatch& scheduled() const noexcept { return schedule.rewritten ? *schedule.rewritten : optimised; }
};

// Errors carry every diagnostic of the failing phase, formatted against the source; their code is Type (any phase
// after parsing).
Result<Compiled> compilePatch(const SourceFile& source, size_t patchIndex, const CompileOptions& options = {});

// ---- Images ----------------------------------------------------------------------------------------------------
enum class Backend : uint8_t { Interpreter, Jit };
const char* backendLabel(Backend b) noexcept;                  // "interpreter", "jit"
Result<Backend> parseBackend(std::string_view name);          // "interp" | "interpreter" | "jit"

struct ImageOptions {
  uint32_t maxBlockFrames = 1024;
  uint32_t maxVoiceGroups = 8;
  const engine::Resources* resources = nullptr;  // must outlive the image
  jit::CompileOptions jit;                       // the JIT's own options (its geometry / resources fields are
                                                 // overwritten by the three above)
};

// Errors: the backend's (prefixed "interpreter: " / "jit: ").
Result<std::unique_ptr<engine::ProgramImage>> buildImage(const graph::Schedule& schedule, Backend backend,
                                                         const ImageOptions& options = {});

// The process's JIT compiler (ARCHITECTURE §6: an explicit singleton): created on the first call (LLVM target
// initialisation, ~ms), never destroyed before exit; its images may outlive any use. Thread-safe.
Result<jit::Compiler*> jitCompiler();
// T1b.4: the process compiler's cache (pq/jit/cache.h). Applied when jitCompiler() creates it, or at once if it
// exists; without a call the process compiler shares code in memory only (tests, pqc). pqdaw calls it at startup
// (app::configureJitCache). Thread-safe.
void setJitCache(const jit::CacheConfig& cache);
// ProjectCompiler's backend through jitCompiler() (UnitBackend::user unused).
engine::UnitBackend jitUnitBackend() noexcept;
// The same through a given compiler (tests, the AOT seed: their own Compiler and cache); it must outlive the backend.
engine::UnitBackend jitUnitBackend(jit::Compiler* compiler) noexcept;

// UTF-8 string → path (command-line arguments are UTF-8).
std::filesystem::path pathFromUtf8(std::string_view s);

}  // namespace pq::compile
