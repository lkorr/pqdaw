// Per-unit compile driver (T1.x-arch1): pq/engine/project_compile.h describes it.
#include "pq/engine/project_compile.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <string_view>
#include <utility>

#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"

namespace pq::engine {

namespace {

Error compileError(ErrorCode code, const std::string& m) { return Error{code, "project compile: " + m, {}}; }

double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

const graph::Patch& scheduledPatch(const CompiledUnit& cu) {
  return cu.schedule->rewritten ? cu.schedule->rewritten->patch : cu.typed->patch;
}

bool sameUnits(const graph::ProjectPartition& a, const graph::ProjectPartition& b) {
  if (a.units.size() != b.units.size() || a.buses.size() != b.buses.size()) return false;
  for (size_t u = 0; u < a.units.size(); ++u)
    if (a.units[u].tracks != b.units[u].tracks) return false;
  for (size_t k = 0; k < a.buses.size(); ++k)
    if (!(a.buses[k].source == b.buses[k].source) || a.buses[k].producer != b.buses[k].producer ||
        a.buses[k].readers != b.buses[k].readers)
      return false;
  return a.outputBus == b.outputBus;
}

uint32_t voiceGroupsOf(const ProjectCompileOptions& o, bool voices) { return voices ? voiceGroupsFor(o.voice.maxVoices) : 1u; }

}  // namespace

ProjectCompiler::ProjectCompiler(ProjectCompileOptions options) : opt_(std::move(options)) {}

Result<CompiledUnit> ProjectCompiler::compileUnit(const graph::ProjectGraph& g, uint32_t u,
                                                  const graph::StateLayout* previous) {
  using namespace graph;
  const auto t0 = std::chrono::steady_clock::now();
  const std::string who = "unit '" + partition_.units[u].name + "': ";
  auto up = composeUnit(g, partition_, u, busTypes_);
  if (!up.ok()) return up.error();
  InferOptions io;
  io.library = &up.value().library;
  io.voiceContext = up.value().voices;
  auto inf = infer(up.value().patch, io);
  if (!inf.ok()) return compileError(inf.error().code, who + inf.error().message);
  OptimiseOptions oo;
  oo.sampleRate = opt_.sampleRate;
  auto opt = optimise(std::move(inf.value()), oo);
  if (!opt.ok()) return compileError(opt.error().code, who + opt.error().message);
  ScheduleOptions so;
  so.sampleRate = opt_.sampleRate;
  so.visualMode = opt_.visualMode;
  so.previous = previous;
  ScheduleResult sr = scheduleDetailed(opt.value(), so);
  if (!sr.ok()) return compileError(sr.diagnostics.front().code, who + sr.diagnostics.front().message);
  CompiledUnit cu;
  for (const Diagnostic& d : sr.schedule.warnings) cu.warnings.push_back(d.message);
  cu.ops = static_cast<uint32_t>(sr.schedule.ops.size());
  cu.inBus = up.value().inBus;
  cu.voices = up.value().voices;
  const uint64_t salt = std::bit_cast<uint64_t>(opt_.sampleRate) ^ (uint64_t{opt_.maxBlockFrames} << 1) ^
                        (uint64_t{voiceGroupsOf(opt_, cu.voices)} << 33);
  cu.key = unitHash(opt.value(), salt);
  for (const ProjectParamLink& l : opt_.paramLinks)
    if (l.track < g.tracks.size() && partition_.unitOf[l.track] == u)
      cu.links.emplace_back("t" + std::to_string(g.tracks[l.track].id) + "__" + l.param, l.projectParam);
  cu.typed = std::make_shared<const TypedPatch>(std::move(opt.value()));
  cu.schedule = std::make_shared<const Schedule>(std::move(sr.schedule));
  cu.frontEndMs = msSince(t0);
  return cu;
}

Result<UnitImage> ProjectCompiler::bind(uint32_t u, std::unique_ptr<ProgramImage> image) const {
  if (u >= units_.size() || image == nullptr) return compileError(ErrorCode::InvalidArgument, "bind: bad unit or image");
  const CompiledUnit& cu = units_[u];
  const graph::Patch& patch = scheduledPatch(cu);
  const ImageInterface& io = image->io();
  UnitImage ui;
  ui.key = cu.key;
  for (const IoSlot& s : io.globalIn) {
    if (s.ordinal >= cu.inBus.size()) return compileError(ErrorCode::InvalidArgument, "bind: an input without a bus");
    ui.in.push_back(BusRef{cu.inBus[s.ordinal], s.channel});
  }
  for (const IoSlot& s : io.globalOut) {
    const uint32_t b = s.isExport ? kNoBus : graph::parseBusName(patch.nodeName(s.node));
    ui.out.push_back(BusRef{b, b == kNoBus ? uint16_t{0} : s.channel});
  }
  if (cu.voices) {
    ui.voiceSlots.doneSlot = findVoiceSlot(io, patch, "done");
    for (uint32_t s = 0; s < io.voiceOut.size() && s < 32; ++s)
      if (std::string_view(patch.nodeName(io.voiceOut[s].node)).starts_with("vmon_")) ui.voiceSlots.monitorMask |= 1u << s;
  }
  for (const auto& [name, param] : cu.links) {
    bool found = false;
    for (uint32_t i = 0; i < io.params.size(); ++i)
      if (patch.nodeName(io.params[i].node) == name) {
        ui.paramLinks.emplace_back(i, param);
        found = true;
      }
    if (!found) return compileError(ErrorCode::NotFound, "bind: no param '" + name + "' (a linked param must exist)");
  }
  ui.image = std::move(image);
  return ui;
}

Result<UnitImage> ProjectCompiler::makeImage(uint32_t u, const UnitBackend& backend) {
  CompiledUnit& cu = units_[u];
  const auto t0 = std::chrono::steady_clock::now();
  const uint32_t groups = voiceGroupsOf(opt_, cu.voices);
  Result<std::unique_ptr<ProgramImage>> img = [&]() -> Result<std::unique_ptr<ProgramImage>> {
    if (backend.fn != nullptr) return backend.fn(backend.user, *cu.schedule, opt_.maxBlockFrames, groups, opt_.resources);
    InterpretOptions io;
    io.maxBlockFrames = opt_.maxBlockFrames;
    io.maxVoiceGroups = groups;
    io.resources = opt_.resources;
    return buildInterpreted(*cu.schedule, io);
  }();
  if (!img.ok()) return compileError(img.error().code, "unit '" + partition_.units[u].name + "': " + img.error().message);
  cu.buildMs = msSince(t0);
  return bind(u, std::move(img.value()));
}

Result<ProjectBuild> ProjectCompiler::buildFrom(const graph::ProjectGraph& g, graph::ProjectPartition partition) {
  partition_ = std::move(partition);
  units_.clear();
  busTypes_.assign(partition_.buses.size(), graph::PortType{});
  ProjectBuild out;
  for (uint32_t u = 0; u < partition_.units.size(); ++u) {
    auto cu = compileUnit(g, u, nullptr);
    if (!cu.ok()) return cu.error();
    for (uint32_t b = 0; b < partition_.buses.size(); ++b)
      if (partition_.buses[b].producer == u) busTypes_[b] = graph::busType(*cu.value().typed, b);
    units_.push_back(std::move(cu.value()));
    auto img = makeImage(u, opt_.backend);
    if (!img.ok()) return img.error();
    out.images.push_back(std::move(img.value()));
    const graph::ProjectUnit& pu = partition_.units[u];
    UnitLayout ul;
    ul.name = pu.name;
    ul.voices = units_[u].voices;
    ul.voice = opt_.voice;
    ul.eventSlot = pu.voiceTrack != graph::kNone ? g.tracks[pu.voiceTrack].eventSlot : kNoSlot;
    out.layout.units.push_back(std::move(ul));
  }
  for (uint32_t b = 0; b < partition_.buses.size(); ++b) {
    out.layout.busWidth.push_back(std::max<uint16_t>(busTypes_[b].width, 1));
    out.layout.busProducer.push_back(partition_.buses[b].producer);
  }
  out.layout.outputBus = partition_.outputBus;
  out.partition = partition_;
  return out;
}

Result<ProjectBuild> ProjectCompiler::build(const graph::ProjectGraph& g) {
  auto part = graph::partitionProject(g);
  if (!part.ok()) return part.error();
  whole_ = false;
  return buildFrom(g, std::move(part.value()));
}

Result<ProjectBuild> ProjectCompiler::buildWhole(const graph::ProjectGraph& g) {
  auto part = graph::wholeProject(g);
  if (!part.ok()) return part.error();
  whole_ = true;
  return buildFrom(g, std::move(part.value()));
}

Result<std::vector<UnitUpdate>> ProjectCompiler::update(const graph::ProjectGraph& g, std::span<const uint32_t> tracks,
                                                        const UnitBackend* backend) {
  if (units_.empty()) return compileError(ErrorCode::InvalidArgument, "update before build");
  // The partition only depends on tracks, wires and roles: O(tracks + wires).
  auto part = whole_ ? graph::wholeProject(g) : graph::partitionProject(g);
  if (!part.ok()) return part.error();
  if (part.value().unitOf.size() != partition_.unitOf.size() || !sameUnits(part.value(), partition_))
    return compileError(ErrorCode::Unsupported, "the edit changed the project topology: rebuild");
  std::vector<uint32_t> dirty;
  for (uint32_t t : tracks) {
    if (t >= partition_.unitOf.size()) return compileError(ErrorCode::InvalidArgument, "update: no track " + std::to_string(t));
    const uint32_t u = partition_.unitOf[t];
    if (std::find(dirty.begin(), dirty.end(), u) == dirty.end()) dirty.push_back(u);
  }
  std::sort(dirty.begin(), dirty.end());
  std::vector<UnitUpdate> out;
  for (uint32_t u : dirty) {
    auto cu = compileUnit(g, u, &units_[u].schedule->state);
    if (!cu.ok()) return cu.error();
    for (uint32_t b = 0; b < partition_.buses.size(); ++b)
      if (partition_.buses[b].producer == u) {
        const graph::PortType t = graph::busType(*cu.value().typed, b);
        if (t.kind != busTypes_[b].kind || t.width != busTypes_[b].width)
          return compileError(ErrorCode::Unsupported, "the edit changed the kind or width of " + graph::busName(b) + ": rebuild");
      }
    UnitUpdate upd;
    upd.unit = u;
    if (cu.value().key == units_[u].key) {
      upd.unchanged = true;
      out.push_back(std::move(upd));
      continue;
    }
    units_[u] = std::move(cu.value());
    auto img = makeImage(u, backend != nullptr ? *backend : opt_.backend);
    if (!img.ok()) return img.error();
    upd.image = std::move(img.value());
    out.push_back(std::move(upd));
  }
  return out;
}

}  // namespace pq::engine
