// Per-unit compile driver (T1.x-arch1): pq/engine/project_compile.h describes it.
#include "pq/engine/project_compile.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <string_view>
#include <utility>

#include "pq/dsp/atoms.h"
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

uint32_t voiceGroupsOf(const CompiledUnit& cu) { return cu.voices ? voiceGroupsFor(cu.maxVoices) : 1u; }

// T4.12-fix1: the polyphony of unit u — its instrument track's own (ProjectTrack::maxVoices), else the default.
uint32_t polyphonyOf(const ProjectCompileOptions& o, const graph::ProjectGraph& g, const graph::ProjectUnit& pu) {
  if (pu.voiceTrack == graph::kNone) return 0;
  const uint32_t own = pu.voiceTrack < g.tracks.size() ? g.tracks[pu.voiceTrack].maxVoices : 0u;
  return std::max(own > 0 ? own : o.voice.maxVoices, 1u);
}

// The VoiceEngine configuration of a compiled unit.
VoiceConfig voiceConfigOf(const ProjectCompileOptions& o, const CompiledUnit& cu) {
  VoiceConfig vc = o.voice;
  if (cu.voices) vc.maxVoices = cu.maxVoices;
  return vc;
}

}  // namespace

uint32_t idleAfterBlocks(const graph::TypedPatch& typed, uint32_t maxBlockFrames) {
  using namespace graph;
  const Patch& p = typed.patch;
  const Adjacency adj = p.adjacency();
  const AtomId gain = atomId("gain"), gainAdd = atomId("gain_add"), voiceSum = atomId("voice_sum");
  const AtomId mul = atomId("mul");
  const AtomId clipPlayer = atomId("clipPlayer");  // T5.4: an engine source; silent in a block without clip audio
  auto global = [&](NodeIndex n) {
    for (uint32_t o = 0; o < p.node(n).numOutputs; ++o)
      if (typed.outputType(n, o).poly == Poly::Voice) return false;
    return true;
  };
  // Settling: every global stateful node is a gain with a constant time (in samples after optimise()).
  double maxTime = 0.0;
  for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
    const Node& nd = p.node(n);
    if (nd.kind != NodeKind::Atom || !global(n) || !atomSignature(nd.atom).has(atom_flags::kStateful)) continue;
    if (nd.atom == clipPlayer) continue;  // its state is the engine's (the timeline's clip player)
    if (nd.atom != gain && nd.atom != gainAdd) return 0;
    const uint32_t timeIn = nd.atom == gain ? 2u : 3u;
    const WireIndex w = adj.driver[p.flatInput(n, timeIn)];
    double t = 0.002 * 768000.0;  // unconnected (optimise() wires the default): 2 ms at the highest rate
    if (w != kNone) {
      const NodeIndex d = p.wire(w).from.node;
      if (p.node(d).kind != NodeKind::Const) return 0;
      t = p.constant(d).value();
      if (!typed.durationsInSamples) return 0;
    }
    if (!(t >= 0.0) || t > 1e7) return 0;
    maxTime = std::max(maxTime, t);
  }
  // Zero propagation (memoised; -1 unknown, 0 not zero, 1 zero). Cycles count as not zero. M4 review M9: evaluated on
  // an explicit stack (a deep rack nest is a long chain of mixers), in the recursive version's order, short-circuits
  // included (so a cycle reads "in progress" exactly where it did).
  std::vector<int8_t> zero(p.nodeCount(), -1);
  enum class Op : uint8_t { And, Or };
  struct Eval {
    NodeIndex n;
    Op op = Op::And;
    uint32_t inputs[8] = {};  // the inputs to read, in order (at most 8 here; others use `first`/`count`)
    uint32_t count = 0;       // inputs to read
    bool list = false;        // inputs[] holds them; else 0 … count − 1
    uint32_t pos = 0;
    bool acc = true;
  };
  std::vector<Eval> stack;
  // Starts evaluating `n`: false when its value is known at once (into *out).
  auto enter = [&](NodeIndex n, bool& out) -> bool {
    if (zero[n] >= 0) {
      out = zero[n] == 1;
      return false;
    }
    zero[n] = 0;  // in progress: a cycle is not zero
    const Node& nd = p.node(n);
    Eval e{n};
    bool leaf = true, z = false;
    switch (nd.kind) {
      case NodeKind::Input: z = true; break;  // a root input: its bus is silent
      case NodeKind::Const: z = p.constant(n).value() == 0.0; break;
      case NodeKind::Atom: {
        const std::string_view name = atomSignature(nd.atom).name;
        if (nd.atom == voiceSum || nd.atom == clipPlayer) {
          z = true;  // no voice sounds; no clip of its slot has audio (ProjectConfig::clipActive)
        } else if (nd.atom == mul) {
          leaf = false, e.op = Op::Or, e.acc = false, e.count = 2;
        } else if (nd.atom == gain) {
          leaf = false, e.count = 1;
        } else if (nd.atom == gainAdd) {
          leaf = false, e.count = 2;
        } else if (name == "select") {
          leaf = false, e.list = true, e.inputs[0] = 1, e.inputs[1] = 2, e.count = 2;
        } else if (name == "add" || name == "sub" || name == "neg" || name == "abs" || name == "pack" ||
                   name == "channel" || name == "min" || name == "max") {
          leaf = false, e.count = name == "channel" ? 1u : nd.numInputs;
        }
        break;
      }
      default: break;
    }
    if (leaf || e.count == 0) {
      out = leaf ? z : e.acc;
      zero[n] = out ? 1 : 0;
      return false;
    }
    stack.push_back(e);
    return true;
  };
  // inputZero of input `i` of `n` (an unconnected input reads a default: not 0 in general).
  auto isInputZero = [&](NodeIndex n0, uint32_t i0) -> bool {
    const WireIndex w0 = adj.driver[p.flatInput(n0, i0)];
    if (w0 == kNone) return false;
    bool result = false;
    if (!enter(p.wire(w0).from.node, result)) return result;
    while (!stack.empty()) {
      Eval& e = stack.back();
      const bool done = e.pos == e.count || (e.op == Op::And ? !e.acc : e.acc);
      if (done) {
        result = e.acc;
        zero[e.n] = result ? 1 : 0;
        stack.pop_back();
        if (!stack.empty()) {
          Eval& parent = stack.back();
          parent.acc = parent.op == Op::And ? parent.acc && result : parent.acc || result;
        }
        continue;
      }
      const uint32_t i = e.list ? e.inputs[e.pos] : e.pos;
      ++e.pos;
      const WireIndex w = adj.driver[p.flatInput(e.n, i)];
      bool r = false;
      if (w != kNone && enter(p.wire(w).from.node, r)) continue;  // `e` may move: re-read next turn
      Eval& top = stack.back();
      top.acc = top.op == Op::And ? top.acc && r : top.acc || r;
    }
    return result;
  };
  for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
    const NodeKind k = p.node(n).kind;
    if (k != NodeKind::Output && k != NodeKind::Export) continue;
    if (typed.inputType(n, 0).poly == Poly::Voice) continue;  // voice outputs: not produced without voices
    if (!isInputZero(n, 0)) return 0;
  }
  // |residual| ≤ a few units decays below the 3e-8 snap threshold within 20 time constants (e^-20 ≈ 2e-9).
  const double frames = std::ceil(20.0 * std::max(maxTime, 1.0));
  return static_cast<uint32_t>(std::min(1e6, std::ceil(frames / std::max(maxBlockFrames, 1u)))) + 1;
}

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
  // T4.4-fix1: an inspected track of this unit — its probes (paths from the track patch: the track's call node in
  // front, whose NodeId is the track id) and its instance tree.
  const TrackInspect* insp = nullptr;
  for (const TrackInspect& ti : opt_.inspect)
    for (uint32_t t : partition_.units[u].tracks)
      if (g.tracks[t].id == ti.trackId) insp = &ti;
  std::vector<ProbeTarget> targets;
  if (insp != nullptr)
    for (const ProbeTarget& t : insp->targets) {
      ProbeTarget pt = t;
      pt.path.insert(pt.path.begin(), NodeId{insp->trackId});
      targets.push_back(std::move(pt));
    }
  io.probes = targets;
  auto infd = inferDetailed(up.value().patch, io);
  if (!infd.ok()) {
    auto again = infer(up.value().patch, io);  // the formatted message
    return compileError(again.ok() ? ErrorCode::Type : again.error().code, who + (again.ok() ? std::string("infer") : again.error().message));
  }
  auto inspect = insp != nullptr ? std::make_shared<UnitInspect>() : nullptr;
  std::vector<const Patch*> instPatch;
  if (inspect) {  // while the composed unit and the library are at hand
    inspect->trackId = insp->trackId;
    for (const InstanceInfo& in : infd.typed.instances) {
      inspect->instanceParent.push_back(in.parent);
      inspect->instanceCall.push_back(in.parent != kNone && in.parent < infd.typed.instances.size() &&
                                              infd.typed.instances[in.parent].patch != nullptr
                                          ? infd.typed.instances[in.parent].patch->node(in.callNode).id
                                          : NodeId{});
      instPatch.push_back(in.patch);
    }
  }
  OptimiseOptions oo;
  oo.sampleRate = opt_.sampleRate;
  std::vector<uint32_t> valueMap;
  if (!infd.probeSources.empty()) oo.valueMap = &valueMap;
  auto opt = optimise(std::move(infd.typed), oo);
  if (!opt.ok()) return compileError(opt.error().code, who + opt.error().message);
  ScheduleOptions so;
  so.sampleRate = opt_.sampleRate;
  so.visualMode = opt_.visualMode;
  so.previous = previous;
  so.sleep = opt_.sleep;
  std::vector<ProbePoint> points;
  if (!infd.probeSources.empty()) points = probePoints(opt.value().patch, infd.probeSources, valueMap, insp->ids);
  so.probes = points;
  ScheduleResult sr = scheduleDetailed(opt.value(), so);
  if (!sr.ok()) return compileError(sr.diagnostics.front().code, who + sr.diagnostics.front().message);
  CompiledUnit cu;
  if (inspect) {
    const TypedPatch& scheduled = sr.schedule.rewritten ? *sr.schedule.rewritten : opt.value();
    const size_t nn = scheduled.patch.nodeCount();
    inspect->nodeInstance.assign(nn, kNone);
    inspect->nodeSource.assign(nn, NodeId{});
    inspect->nodeWhat.assign(nn, NodeOrigin::What::Node);
    for (size_t n = 0; n < nn && n < scheduled.origin.size(); ++n) {
      const NodeOrigin& o = scheduled.origin[n];
      inspect->nodeInstance[n] = o.instance;
      inspect->nodeWhat[n] = o.what;
      if (o.instance < instPatch.size() && instPatch[o.instance] && o.node < instPatch[o.instance]->nodeCount())
        inspect->nodeSource[n] = instPatch[o.instance]->node(o.node).id;
    }
    cu.inspect = std::move(inspect);
  }
  for (const Diagnostic& d : sr.schedule.warnings) cu.warnings.push_back(d.message);
  cu.ops = static_cast<uint32_t>(sr.schedule.ops.size());
  cu.inBus = up.value().inBus;
  cu.voices = up.value().voices;
  cu.maxVoices = cu.voices ? polyphonyOf(opt_, g, partition_.units[u]) : 0u;
  for (uint32_t t : partition_.units[u].tracks) cu.memberIds.push_back(g.tracks[t].id);
  std::sort(cu.memberIds.begin(), cu.memberIds.end());
  for (uint32_t b : up.value().inBus) {
    cu.inKeys.push_back(busKeys_[b]);
    cu.inTypes.push_back(busTypes_[b]);
    cu.readKeys.push_back(busKeys_[b]);
    cu.busNumbers.emplace_back(b, busKeys_[b]);
  }
  for (uint32_t b : up.value().outBus) {
    cu.writeKeys.push_back(busKeys_[b]);
    cu.busNumbers.emplace_back(b, busKeys_[b]);
  }
  std::sort(cu.readKeys.begin(), cu.readKeys.end());
  std::sort(cu.writeKeys.begin(), cu.writeKeys.end());
  cu.idleAfterBlocks = idleAfterBlocks(opt.value(), opt_.maxBlockFrames);
  uint64_t salt = std::bit_cast<uint64_t>(opt_.sampleRate) ^ (uint64_t{opt_.maxBlockFrames} << 1) ^
                  (uint64_t{voiceGroupsOf(cu)} << 33) ^ (uint64_t{cu.maxVoices} << 45);
  if (cu.inspect) {  // T4.4-fix1: an inspected unit is another program (probe copies, bound taps): another key
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (const ProbeRef& p : sr.schedule.probes) h = (h ^ (uint64_t{p.id} + 0x632BE59BD9B4E019ull)) * 0x100000001B3ull;
    salt ^= h ^ 0xD6E8FEB86659FD93ull;
  }
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
  ui.idleAfterBlocks = cu.idleAfterBlocks;
  for (const IoSlot& s : io.globalIn) {
    if (s.ordinal >= cu.inBus.size()) return compileError(ErrorCode::InvalidArgument, "bind: an input without a bus");
    ui.in.push_back(BusRef{cu.inBus[s.ordinal], s.channel});
  }
  for (const IoSlot& s : io.globalOut) {
    uint32_t b = s.isExport ? kNoBus : graph::parseBusName(patch.nodeName(s.node));
    if (b != kNoBus && !cu.busRemap.empty()) b = b < cu.busRemap.size() ? cu.busRemap[b] : kNoBus;
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
  if (opt_.keepAlive) {  // T2.6-fix4: the owners of the tuning tables this unit's ops read
    std::vector<uint32_t> seen;
    for (const graph::Op& op : cu.schedule->ops) {
      if (op.kind != graph::OpKind::Kernel || op.resource == graph::kNone) continue;
      if (dsp::kernelInfo(op.kernel).resource != dsp::Resource::Tuning) continue;
      if (std::find(seen.begin(), seen.end(), op.resource) != seen.end()) continue;
      seen.push_back(op.resource);
      if (std::shared_ptr<const void> owner = opt_.keepAlive(dsp::Resource::Tuning, op.resource))
        ui.keepAlive.push_back(std::move(owner));
    }
  }
  if (opt_.onBind)  // T4.4-fix1: probe taps / cost meter; their owner lives as long as the image (M4 review H2, L4)
    if (std::shared_ptr<const void> owner = opt_.onBind(u, cu, *image)) ui.keepAlive.push_back(std::move(owner));
  ui.image = std::move(image);
  return ui;
}

Result<UnitImage> ProjectCompiler::makeImage(uint32_t u, const UnitBackend& backend) {
  CompiledUnit& cu = units_[u];
  const auto t0 = std::chrono::steady_clock::now();
  const uint32_t groups = voiceGroupsOf(cu);
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

uint32_t ProjectCompiler::unitOf(const graph::Schedule* s) const noexcept {
  for (uint32_t u = 0; u < units_.size(); ++u)
    if (units_[u].schedule.get() == s) return u;
  return kNoUnit;
}

uint32_t ProjectCompiler::imageVoiceGroups(uint32_t u) const noexcept {
  return u < units_.size() ? voiceGroupsOf(units_[u]) : 1u;
}

std::string ProjectCompiler::busKey(const graph::ProjectGraph& g, const graph::ProjectPort& p) {
  return (p.track < g.tracks.size() ? std::to_string(g.tracks[p.track].id) : std::string("?")) + ":" + p.port;
}

Result<ProjectBuild> ProjectCompiler::buildFrom(const graph::ProjectGraph& g, graph::ProjectPartition partition) {
  partition_ = std::move(partition);
  units_.clear();
  busTypes_.assign(partition_.buses.size(), graph::PortType{});
  busKeys_.clear();
  for (const graph::ProjectBus& b : partition_.buses) busKeys_.push_back(busKey(g, b.source));
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
    ul.voice = voiceConfigOf(opt_, units_[u]);
    ul.eventSlot = pu.voiceTrack != graph::kNone ? g.tracks[pu.voiceTrack].eventSlot : kNoSlot;
    out.layout.units.push_back(std::move(ul));
  }
  for (uint32_t b = 0; b < partition_.buses.size(); ++b) {
    out.layout.busWidth.push_back(std::max<uint16_t>(busTypes_[b].width, 1));
    out.layout.busProducer.push_back(partition_.buses[b].producer);
  }
  out.layout.outputBus = partition_.outputBus;
  out.layout.busKey = busKeys_;
  out.partition = partition_;
  return out;
}

Result<TopologyBuild> ProjectCompiler::rebuild(const graph::ProjectGraph& g, std::span<const uint32_t> dirtyTracks,
                                               const UnitBackend* backend) {
  if (units_.empty()) return compileError(ErrorCode::InvalidArgument, "rebuild before build");
  if (whole_) return compileError(ErrorCode::Unsupported, "rebuild of a whole-project build");
  auto part = graph::partitionProject(g);
  if (!part.ok()) return part.error();
  // Keep the previous project until everything compiled.
  graph::ProjectPartition oldPartition = std::move(partition_);
  std::vector<CompiledUnit> oldUnits = std::move(units_);
  std::vector<graph::PortType> oldTypes = std::move(busTypes_);
  std::vector<std::string> oldKeys = std::move(busKeys_);
  units_.clear();
  auto restore = [&] {
    partition_ = std::move(oldPartition);
    units_ = std::move(oldUnits);
    busTypes_ = std::move(oldTypes);
    busKeys_ = std::move(oldKeys);
  };
  partition_ = std::move(part.value());
  busTypes_.assign(partition_.buses.size(), graph::PortType{});
  busKeys_.clear();
  for (const graph::ProjectBus& b : partition_.buses) busKeys_.push_back(busKey(g, b.source));
  std::vector<uint32_t> dirtyIds;
  for (uint32_t t : dirtyTracks)
    if (t < g.tracks.size()) dirtyIds.push_back(g.tracks[t].id);
  auto keyIndex = [&](const std::string& k) -> uint32_t {
    for (uint32_t b = 0; b < busKeys_.size(); ++b)
      if (busKeys_[b] == k) return b;
    return kNoBus;
  };
  auto oldKeyIndex = [&](const std::string& k) -> uint32_t {
    for (uint32_t b = 0; b < oldKeys.size(); ++b)
      if (oldKeys[b] == k) return b;
    return kNoBus;
  };
  const UnitBackend& be = backend != nullptr ? *backend : opt_.backend;
  TopologyBuild out;
  std::vector<uint8_t> oldTaken(oldUnits.size(), 0);
  for (uint32_t u = 0; u < partition_.units.size(); ++u) {
    const graph::ProjectUnit& pu = partition_.units[u];
    std::vector<uint32_t> members;
    for (uint32_t t : pu.tracks) members.push_back(g.tracks[t].id);
    std::sort(members.begin(), members.end());
    std::vector<std::string> reads, writes;
    for (uint32_t b = 0; b < partition_.buses.size(); ++b) {
      const graph::ProjectBus& pb = partition_.buses[b];
      if (pb.producer == u) writes.push_back(busKeys_[b]);
      if (std::find(pb.readers.begin(), pb.readers.end(), u) != pb.readers.end()) reads.push_back(busKeys_[b]);
    }
    std::sort(reads.begin(), reads.end());
    std::sort(writes.begin(), writes.end());
    // T4.12-fix1: an instrument unit whose polyphony changed does not continue its old unit (a new VoiceEngine).
    const uint32_t poly = polyphonyOf(opt_, g, pu);
    uint32_t from = kNoUnit;
    for (uint32_t o = 0; o < oldUnits.size() && from == kNoUnit; ++o)
      if (!oldTaken[o] && oldUnits[o].memberIds == members && oldUnits[o].maxVoices == poly) from = o;
    bool reuse = from != kNoUnit && oldUnits[from].readKeys == reads && oldUnits[from].writeKeys == writes;
    for (uint32_t id : members) reuse = reuse && std::find(dirtyIds.begin(), dirtyIds.end(), id) == dirtyIds.end();
    if (reuse) {  // its inputs must still have the types it was compiled against
      const CompiledUnit& ou = oldUnits[from];
      for (size_t k = 0; k < ou.inKeys.size() && reuse; ++k) {
        const uint32_t b = keyIndex(ou.inKeys[k]);
        reuse = b != kNoBus && busTypes_[b].kind == ou.inTypes[k].kind && busTypes_[b].width == ou.inTypes[k].width;
      }
    }
    if (reuse) {  // and the same param links (the running image's links cannot change without a new image)
      std::vector<std::pair<std::string, uint32_t>> links;
      for (const ProjectParamLink& l : opt_.paramLinks)
        if (l.track < g.tracks.size() && partition_.unitOf[l.track] == u)
          links.emplace_back("t" + std::to_string(g.tracks[l.track].id) + "__" + l.param, l.projectParam);
      auto old = oldUnits[from].links;
      std::sort(links.begin(), links.end());
      std::sort(old.begin(), old.end());
      reuse = links == old;
    }
    TopologyUnit tu;
    tu.from = from;
    if (from != kNoUnit) oldTaken[from] = 1;
    if (reuse) {
      CompiledUnit cu = oldUnits[from];  // a copy: the old project stays intact until the build succeeds
      for (size_t k = 0; k < cu.inKeys.size(); ++k) cu.inBus[k] = keyIndex(cu.inKeys[k]);
      uint32_t maxNum = 0;
      for (const auto& [num, key] : cu.busNumbers) maxNum = std::max(maxNum, num);
      cu.busRemap.assign(maxNum + 1, kNoBus);
      for (const auto& [num, key] : cu.busNumbers) cu.busRemap[num] = keyIndex(key);
      for (uint32_t b = 0; b < partition_.buses.size(); ++b)
        if (partition_.buses[b].producer == u) {
          const uint32_t ob = oldKeyIndex(busKeys_[b]);
          busTypes_[b] = ob != kNoBus ? oldTypes[ob] : graph::PortType{};
        }
      units_.push_back(std::move(cu));
      ++out.reused;
    } else {
      const graph::StateLayout* previous = from != kNoUnit ? &oldUnits[from].schedule->state : nullptr;
      auto cu = compileUnit(g, u, previous);
      if (!cu.ok()) {
        restore();
        return cu.error();
      }
      for (uint32_t b = 0; b < partition_.buses.size(); ++b)
        if (partition_.buses[b].producer == u) busTypes_[b] = graph::busType(*cu.value().typed, b);
      units_.push_back(std::move(cu.value()));
      auto img = makeImage(u, be);
      if (!img.ok()) {
        restore();
        return img.error();
      }
      tu.hasImage = true;
      tu.image = std::move(img.value());
      ++out.compiled;
    }
    UnitLayout ul;
    ul.name = pu.name;
    ul.voices = units_[u].voices;
    ul.voice = voiceConfigOf(opt_, units_[u]);
    ul.eventSlot = pu.voiceTrack != graph::kNone ? g.tracks[pu.voiceTrack].eventSlot : kNoSlot;
    out.layout.units.push_back(std::move(ul));
    out.units.push_back(std::move(tu));
  }
  for (uint32_t b = 0; b < partition_.buses.size(); ++b) {
    out.layout.busWidth.push_back(std::max<uint16_t>(busTypes_[b].width, 1));
    out.layout.busProducer.push_back(partition_.buses[b].producer);
  }
  out.layout.outputBus = partition_.outputBus;
  out.layout.busKey = busKeys_;
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
  // T4.12-fix1: a changed polyphony needs a new VoiceEngine for that unit: a topology change (before anything compiles).
  for (uint32_t u = 0; u < partition_.units.size(); ++u)
    if (units_[u].voices && polyphonyOf(opt_, g, partition_.units[u]) != units_[u].maxVoices)
      return compileError(ErrorCode::Unsupported, "the polyphony of unit '" + partition_.units[u].name + "' changed: rebuild");
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
