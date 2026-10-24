#pragma once

// The per-unit compile driver (T1.x-arch1): ProjectGraph (pq/graph/project.h) → partition → per unit: compose →
// infer → optimise → schedule → image (the interpreter by default; any backend through UnitBackend, e.g. the JIT) →
// UnitImage with its bus bindings, ready for ProjectProgram::publish. Not RT-safe: the compiler thread.
//
//   build(graph)          every unit, in dependency order (a consumer's root-input types come from its producers'
//                         compiled bus types). Returns the ProjectLayout for ProjectProgram::create and one image per
//                         unit. buildWhole() compiles the same graph as ONE unit (graph::wholeProject), the reference
//                         a per-unit project must match.
//   update(graph, tracks[, backend])  an edit of those tracks' patches (same tracks, wires, port names): recomposes and recompiles
//                         ONLY the units holding them, scheduled against each unit's previous state layout (so the
//                         swap's state transfer is a few memcpys), and returns their new images. A unit whose
//                         compile-cache key (graph::unitHash of the optimised unit + sample rate, block size, voice
//                         groups; the backend is not part of it: T1b.4's disk cache adds it) is unchanged is reported
//                         `unchanged` without an image. If the edit changed the topology — a different partition, or a
//                         bus whose kind/width changed — it returns ErrorCode::Unsupported: rebuild the project
//                         (T3.17 does a project-level swap then; out of scope here).
//   bind(unit, image)     binds another image of a unit's current schedule (the JIT image compiled in the background
//                         after the interpreter one was published: T1b.5's live swap uses exactly this).
// Front-end and build times are reported per unit (CompiledUnit), measured with the steady clock.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "pq/core/result.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/program_image.h"
#include "pq/engine/project_program.h"
#include "pq/engine/voices.h"
#include "pq/graph/project.h"
#include "pq/graph/schedule.h"

namespace pq::engine {

// Schedule → image. fn null: the interpreter (buildInterpreted). The JIT plugs in with a function that calls
// jit::Compiler::compile (the engine never includes jit).
struct UnitBackend {
  Result<std::unique_ptr<ProgramImage>> (*fn)(void* user, const graph::Schedule& schedule, uint32_t maxBlockFrames,
                                              uint32_t maxVoiceGroups, const Resources* resources) = nullptr;
  void* user = nullptr;
};

// A project param bound to a unit param: `param` is the param's flattened name inside the track patch (e.g.
// "cutoff", or "eq__gain" for a param of the device call `eq`); the unit's name for it is "t<id>__" + param.
struct ProjectParamLink {
  uint32_t track = graph::kNone;
  std::string param;
  uint32_t projectParam = 0;
};

struct ProjectCompileOptions {
  double sampleRate = 48000.0;
  uint32_t maxBlockFrames = 1024;
  VoiceConfig voice;                    // instrument units: polyphony (maxVoiceGroups = voiceGroupsFor(maxVoices))
  const Resources* resources = nullptr; // must outlive the images
  bool visualMode = true;               // delay-free cycles (also between merged tracks) get a z1 + warning
  UnitBackend backend;
  std::vector<ProjectParamLink> paramLinks;
};

struct CompiledUnit {
  std::shared_ptr<const graph::TypedPatch> typed;     // optimised
  std::shared_ptr<const graph::Schedule> schedule;
  uint64_t key = 0;                // compile-cache key
  uint32_t ops = 0;                // schedule ops
  double frontEndMs = 0.0;         // compose + infer + optimise + schedule
  double buildMs = 0.0;            // backend
  std::vector<std::string> warnings;  // scheduler warnings (inserted z1s)
  std::vector<uint32_t> inBus;        // per root `in` ordinal (graph::UnitPatch::inBus)
  std::vector<std::pair<std::string, uint32_t>> links;  // flattened param name → project param
  bool voices = false;
};

struct ProjectBuild {
  graph::ProjectPartition partition;
  ProjectLayout layout;
  std::vector<UnitImage> images;   // per unit (layout order)
};

struct UnitUpdate {
  uint32_t unit = 0;
  bool unchanged = false;          // same compile-cache key: nothing to publish
  UnitImage image;                 // when !unchanged
};

class ProjectCompiler {
public:
  explicit ProjectCompiler(ProjectCompileOptions options);

  Result<ProjectBuild> build(const graph::ProjectGraph& graph);
  Result<ProjectBuild> buildWhole(const graph::ProjectGraph& graph);
  // `backend` (optional) overrides the options' backend for this compile: e.g. the interpreter for the immediate
  // image of an edit, then bind() a JIT image of unit(u).schedule compiled in the background.
  Result<std::vector<UnitUpdate>> update(const graph::ProjectGraph& graph, std::span<const uint32_t> tracks,
                                         const UnitBackend* backend = nullptr);
  Result<UnitImage> bind(uint32_t unit, std::unique_ptr<ProgramImage> image) const;

  const graph::ProjectPartition& partition() const noexcept { return partition_; }
  const CompiledUnit& unit(uint32_t u) const noexcept { return units_[u]; }
  uint32_t numUnits() const noexcept { return static_cast<uint32_t>(units_.size()); }
  const ProjectCompileOptions& options() const noexcept { return opt_; }

private:
  Result<ProjectBuild> buildFrom(const graph::ProjectGraph& graph, graph::ProjectPartition partition);
  Result<CompiledUnit> compileUnit(const graph::ProjectGraph& graph, uint32_t u, const graph::StateLayout* previous);
  Result<UnitImage> makeImage(uint32_t u, const UnitBackend& backend);

  ProjectCompileOptions opt_;
  graph::ProjectPartition partition_;
  std::vector<CompiledUnit> units_;
  std::vector<graph::PortType> busTypes_;
  bool whole_ = false;
};

}  // namespace pq::engine
