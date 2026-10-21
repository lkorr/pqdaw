#pragma once

// pqc (T1.10): the patch compiler CLI as a library, so tests drive exactly what the executable runs.
//   pqc check         <file.pqp> [--patch NAME] [--rate HZ] [--visual]
//   pqc dump-ir       <file.pqp> [--patch NAME] [--rate HZ] [--stage parsed|inferred|optimised] [-o out.txt]
//   pqc dump-schedule <file.pqp> [--patch NAME] [--rate HZ] [--visual] [--keys] [-o out.txt]
//   pqc render        <file.pqp> [--patch NAME] [--notes "…"] [--seconds S] [--rate HZ] [--block N]
//                     [--backend interp|jit] [--voices N] [--param NAME=VALUE]… [--gain DB] -o out.wav
// Usage and exit codes: tools/README.md (and usage()).
//
// Pipeline (SPEC §7.4): read the file (imports relative to the importing file, recursively) → parse → infer →
// optimise (specialised for --rate) → schedule → ProgramImage (interpreter, or the LLVM JIT) → offline render
// (engine::renderOffline: fixed block, deterministic) → 32-bit float WAV. Not RT-safe anywhere (tool code); the
// render loop itself is the engine's RT path.

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pq/core/result.h"
#include "pq/engine/note_list.h"
#include "pq/engine/offline_render.h"
#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"

namespace pq::pqc {

// Process exit codes of `pqc` (documented in tools/README.md).
enum ExitCode : int {
  kExitOk = 0,
  kExitCompileError = 1,  // the patch has errors (parse, inference, optimisation, scheduling); diagnostics on stderr
  kExitUsage = 2,         // bad command line
  kExitIo = 3,            // a file could not be read or written
  kExitBackend = 4,       // the patch compiled but could not be built or rendered (JIT failure, nothing to play…)
};

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
  // Index of the patch named `name` (empty: the first patch), or an error naming the patches there are.
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

// ---- Notes (--notes) -------------------------------------------------------------------------------------------
// See tools/README.md "Notes". A plain pitch list ("60 64 67 72", the `pqdaw --play` syntax) is the --play
// arpeggio: note k starts at k·0.25 s, lasts 0.125 s, velocity 0.7, pitch k mod n, repeated to the end of the render.
// Any timing turns it into a one-shot list: pitch[@start][:length][*velocity] per note.
struct NotesSpec {
  std::vector<float> hz;           // per token
  std::vector<double> start, length, velocity;  // seconds, seconds, 0..1 (per token, defaults filled in)
  bool arpeggio = false;           // plain list: repeat cyclically (start/length hold the first cycle)
};
inline constexpr double kArpStepSeconds = 0.25;
inline constexpr double kArpGateSeconds = 0.125;
inline constexpr float kDefaultVelocity = 0.7f;
inline constexpr std::string_view kDefaultNotes = "60 64 67 72";

Result<NotesSpec> parseNotes(std::string_view text);
// The notes of a spec over [0, frames) at `sampleRate` (an arpeggio is unrolled to cover the length).
std::vector<engine::TimedNote> timedNotes(const NotesSpec& spec, double sampleRate, uint64_t frames);

// ---- Rendering -------------------------------------------------------------------------------------------------
enum class Backend : uint8_t { Interpreter, Jit };

struct RenderOptions {
  std::string patch;                // empty: the first patch
  CompileOptions compile;
  Backend backend = Backend::Interpreter;
  double seconds = 2.0;
  uint32_t blockFrames = 64;        // the fixed engine block
  uint32_t maxVoices = 16;          // polyphony for patches with voice stages
  std::string notes;                // empty: kDefaultNotes (patches with voice stages only)
  std::vector<std::pair<std::string, float>> params;  // by param name, normalised units (seconds, Hz, linear)
  float gainDb = 0.0f;
};

struct RenderInfo {
  std::string patchName;
  uint32_t ops = 0;
  bool voices = false;              // the patch has voice stages (notes were played)
  bool instrument = false;          // no global `out`: the voice mixes were rendered
  size_t notes = 0;
  engine::RenderStats stats;
  engine::VoiceStats voiceStats;
  double compileMs = 0.0;           // graph pipeline + image build (JIT included)
};

// Compiles and renders `path` into `sink`. Errors carry an ExitCode-worthy ErrorCode: Parse/Type → compile error,
// Io → I/O, others → backend.
Result<RenderInfo> renderFile(const std::filesystem::path& path, const RenderOptions& options, engine::AudioSink& sink);

// ---- The CLI ---------------------------------------------------------------------------------------------------
std::string usage();
// Runs one command line (args without the program name). Output to `out`, diagnostics to `err`; returns the exit code.
int run(std::span<const std::string> args, std::ostream& out, std::ostream& err);

// UTF-8 string → path (the CLI's arguments are UTF-8).
std::filesystem::path pathFromUtf8(std::string_view s);

}  // namespace pq::pqc
