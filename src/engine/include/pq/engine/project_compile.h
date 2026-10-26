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
//                         groups, polyphony; the backend is not part of it: T1b.4's disk cache adds it) is unchanged is
//                         reported `unchanged` without an image. If the edit changed the topology — a different
//                         partition, a bus whose kind/width changed, or (T4.12-fix1) an instrument unit's polyphony —
//                         it returns ErrorCode::Unsupported: rebuild the project (T3.17 does a project-level swap then).
//   bind(unit, image)     binds another image of a unit's current schedule (the JIT image compiled in the background
//                         after the interpreter one was published: T1b.5's live swap uses exactly this).
//   rebuild(graph, dirty) a topology edit (T3.17: tracks added or removed, re-routing, merges): re-partitions and
//                         returns a TopologyBuild for ProjectProgram::publishTopology. A unit whose member tracks,
//                         bus connections (by bus key "<track id>:<port>") and input bus types are unchanged and none
//                         of whose tracks is in `dirty` is REUSED: nothing is compiled, it continues as it runs. Every
//                         other unit is compiled; a unit with the same member tracks as before continues that unit
//                         (its schedule against the previous state layout: a T1.9 swap with state transfer), a unit
//                         with a new member set (a new track, a merge or a split) starts as a new unit (fresh state).
//                         T4.12-fix1: so does an instrument unit whose polyphony changed (a running unit's VoiceEngine
//                         keeps the lanes it was prepared with: the new one gets its own; its voices and state restart).
//                         On error the compiler keeps its previous project.
// Every layout this driver makes carries ProjectLayout::busKey.
// Front-end and build times are reported per unit (CompiledUnit), measured with the steady clock.

#include <cstdint>
#include <functional>
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
#include "pq/graph/infer.h"
#include "pq/graph/inspect.h"
#include "pq/graph/project.h"
#include "pq/graph/schedule.h"

namespace pq::engine {

// Schedule → image. fn null: the interpreter (buildInterpreted). The JIT plugs in with a function that calls
// jit::Compiler::compile (the engine never includes jit).
struct UnitBackend {
  Result<std::unique_ptr<ProgramImage>> (*fn)(void* user, const graph::Schedule& schedule, uint32_t maxBlockFrames,
                                              uint32_t maxVoiceGroups, const Resources* resources) = nullptr;
  void* user = nullptr;
  // T1b.x-perf (QUESTIONS Q111 option C), optional, for background builds (BackgroundCompiler): a quicker FIRST image
  // of the same output (the JIT's voice groups without pair functions). Sets *refine when the image is such a first
  // one and `fn` would build a better final one; false when it already is the final image (a cache hit, nothing to
  // pair). Null: `fn` only.
  Result<std::unique_ptr<ProgramImage>> (*quick)(void* user, const graph::Schedule& schedule, uint32_t maxBlockFrames,
                                                 uint32_t maxVoiceGroups, const Resources* resources,
                                                 bool* refine) = nullptr;
};

// A project param bound to a unit param: `param` is the param's flattened name inside the track patch (e.g.
// "cutoff", or "eq__gain" for a param of the device call `eq`); the unit's name for it is "t<id>__" + param.
struct ProjectParamLink {
  uint32_t track = graph::kNone;
  std::string param;
  uint32_t projectParam = 0;
};

// T4.4-fix1: inspection of one track inside its unit (the circuit editor on a device of the project, T4.5's probes and
// cost meters through the project). `targets` name values inside the TRACK's patch (paths start at its call nodes, e.g.
// a device call `d<id>`); the unit holding the track compiles them in with the track's call node in front (probes
// change buffer lifetimes only: the audio is bit-identical, pq/graph/inspect.h). Even without targets, the unit is
// compiled with its instance tree kept (CompiledUnit::inspect, for cost attribution) and its images go through
// ProjectCompileOptions::onBind like every image, where the owner binds taps and a cost meter.
struct TrackInspect {
  uint32_t trackId = 0;                     // graph::ProjectTrack::id
  std::vector<graph::ProbeTarget> targets;
  std::vector<uint32_t> ids;                // per target: the id its taps are bound by (Schedule::probes)
};

// What a unit compiled under a TrackInspect keeps of its instance tree, in NodeIds (valid while the text changes): the
// circuit editor's PlayedProgram for its cost attribution.
struct UnitInspect {
  uint32_t trackId = 0;
  std::vector<uint32_t> instanceParent;     // per instance (TypedPatch::instances): its caller (kNone: the root)
  std::vector<graph::NodeId> instanceCall;  // … and the call node in the caller
  std::vector<uint32_t> nodeInstance;       // per node of the scheduled patch: its instance
  std::vector<graph::NodeId> nodeSource;    // … the node it comes from in that instance's patch (invalid: none)
  std::vector<graph::NodeOrigin::What> nodeWhat;
};

struct CompiledUnit;

struct ProjectCompileOptions {
  double sampleRate = 48000.0;
  uint32_t maxBlockFrames = 1024;
  VoiceConfig voice;                    // instrument units: polyphony (maxVoiceGroups = voiceGroupsFor(maxVoices)); a
                                        // unit's instrument track overrides maxVoices with ProjectTrack::maxVoices > 0
  const Resources* resources = nullptr; // must outlive the images
  bool visualMode = true;               // delay-free cycles (also between merged tracks) get a z1 + warning
  bool sleep = true;                    // T1.x-silence: sleep plans (graph::ScheduleOptions::sleep; false: never sleep)
  UnitBackend backend;
  std::vector<ProjectParamLink> paramLinks;
  // T2.6-fix4 (optional): the owner of resource `index` of `kind` (only Tuning is asked), kept alive by every image
  // that reads it (UnitImage::keepAlive), so a table can be freed once no image uses it. Called by bind() (every
  // image goes through it), on whichever thread binds: the compile thread, or a background image's (T1b.5), so the
  // owner must still know the resources of every schedule that may be bound.
  std::function<std::shared_ptr<const void>(dsp::Resource kind, uint32_t index)> keepAlive;
  // T4.4-fix1 (optional): the inspected tracks (see TrackInspect; ProjectCompiler::setInspect), and a hook every image
  // passes through in bind() before it is returned — the compile path's and a background (JIT) image's alike — so the
  // owner can bind probe taps and a cost meter (ProgramImage::bindInspect) to the images of an inspected unit.
  std::vector<TrackInspect> inspect;
  std::function<void(uint32_t unit, const CompiledUnit& cu, ProgramImage& image)> onBind;
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
  uint32_t idleAfterBlocks = 0;   // UnitImage::idleAfterBlocks (idleAfterBlocks())
  bool voices = false;
  uint32_t maxVoices = 0;         // voice units: the polyphony its images and VoiceEngine are built for (0: none)
  // Identity across topology edits (rebuild).
  std::vector<uint32_t> memberIds;          // the member tracks' ids, ascending
  std::vector<std::string> readKeys, writeKeys;  // bus keys read / written, sorted
  std::vector<graph::PortType> inTypes;     // per root `in` ordinal: the bus type it was compiled against
  std::vector<std::string> inKeys;          // per root `in` ordinal: its bus key
  std::vector<std::pair<uint32_t, std::string>> busNumbers;  // (k of a `bus<k>` name in the schedule, its bus key)
  std::vector<uint32_t> busRemap;           // k of a `bus<k>` name → the current bus index (empty: identity)
  // T4.4-fix1: set when the unit holds an inspected track (ProjectCompileOptions::inspect); its probes are in
  // schedule->probes, and the compile-cache key includes them (a probe change recompiles the unit).
  std::shared_ptr<const UnitInspect> inspect;
};

struct ProjectBuild {
  graph::ProjectPartition partition;
  ProjectLayout layout;
  std::vector<UnitImage> images;   // per unit (layout order)
};

struct TopologyBuild {
  graph::ProjectPartition partition;
  ProjectLayout layout;
  std::vector<TopologyUnit> units;  // parallel to layout.units: for ProjectProgram::publishTopology
  uint32_t compiled = 0;            // units compiled by this rebuild
  uint32_t reused = 0;              // units reused without a compile
};

struct UnitUpdate {
  uint32_t unit = 0;
  bool unchanged = false;          // same compile-cache key: nothing to publish
  UnitImage image;                 // when !unchanged
};

// T3.17-fix2 (idle units, pq/engine/project_program.h): whether a unit's outputs are exactly zero while every root
// input is zero and no voice sounds, and its global state then only settles: every global stateful node is a gain /
// gain_add with a constant time, and every global out / export is zero by propagation (root inputs and voice_sum are
// zero; mul / gain zero if their signal input is; add, sub, neg, abs, pack, channel, select, min, max, gain_add zero if
// all their signal inputs are; a constant 0). Returns the blocks of unchanged params after which every such gain has
// snapped onto its target (ceil(20 · time / maxBlockFrames) + 1), or 0 when the unit does not qualify.
uint32_t idleAfterBlocks(const graph::TypedPatch& typed, uint32_t maxBlockFrames);

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
  // See the file comment. `dirtyTracks`: indices into graph.tracks whose patch changed.
  Result<TopologyBuild> rebuild(const graph::ProjectGraph& graph, std::span<const uint32_t> dirtyTracks,
                                const UnitBackend* backend = nullptr);
  // The project params units link to (ProjectParamLink::track indexes the graph passed next).
  void setParamLinks(std::vector<ProjectParamLink> links) { opt_.paramLinks = std::move(links); }
  // T4.4-fix1: the inspected tracks from the next compile on (update() with the track dirty recompiles its unit).
  void setInspect(std::vector<TrackInspect> inspect) { opt_.inspect = std::move(inspect); }
  void setOnBind(std::function<void(uint32_t, const CompiledUnit&, ProgramImage&)> fn) { opt_.onBind = std::move(fn); }
  // The bus key of graph port `p` ("<track id>:<port>").
  static std::string busKey(const graph::ProjectGraph& graph, const graph::ProjectPort& p);

  // T1b.5: the unit whose current schedule is `s` (kNoUnit: none — the unit was recompiled or removed since), and the
  // voice groups its images are built with. The background JIT image of a schedule is bound and published only
  // while unitOf(schedule) finds it.
  uint32_t unitOf(const graph::Schedule* s) const noexcept;
  uint32_t imageVoiceGroups(uint32_t u) const noexcept;

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
  std::vector<std::string> busKeys_;
  bool whole_ = false;
};

}  // namespace pq::engine
