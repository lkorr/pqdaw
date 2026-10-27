// Per-device CPU cost for the project patch view (T4.13; QUESTIONS Q134 item 8): one engine::CostMeter per unit
// schedule, bound to every image of that schedule (the interpreter's and the JIT's) when it is published, and the
// T4.5 region → node attribution (views::circuit::attributeCost) run per member track, so each device call `d<id>`
// of a track gets its whole instance's ns per sample. See pq/app/project_link.h.

#include <chrono>
#include <unordered_map>
#include <unordered_set>

#include "pq/app/project_link.h"
#include "pq/engine/inspect.h"
#include "pq/views/circuit/patch_editor.h"
#include "pq/views/circuit/probes.h"

namespace pq::app {

namespace {
double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint64_t tagOf(const graph::Schedule* s) noexcept { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(s)); }
}  // namespace

struct ProjectLink::CostState {
  // Meters outlive every image bound to them (an image reads its meter's flag each block, also while retired and
  // fading): a meter whose schedule no longer runs is freed kRetireSeconds later.
  static constexpr double kRetireSeconds = 10.0;
  struct Meter {
    std::shared_ptr<const graph::Schedule> schedule;  // keeps the key's address from being reused
    std::unique_ptr<engine::CostMeter> meter;
    double unusedSince = -1.0;
  };
  std::unordered_map<const graph::Schedule*, Meter> meters;  // compileMutex_
  bool on = false;                                           // compileMutex_
  // Attribution of a unit schedule (UI thread, under compileMutex_).
  struct Attribution {
    std::shared_ptr<const graph::Schedule> schedule;
    views::circuit::PatchEditor::PlayedProgram played;
    std::vector<uint32_t> tracks;  // member track ids (the root's instances)
  };
  std::unordered_map<const graph::Schedule*, Attribution> attribution;
  std::unordered_map<uint32_t, float> deviceNs;  // device id → the last reading (UI thread)
};

void ProjectLink::CostStateDeleter::operator()(CostState* s) const noexcept { delete s; }
ProjectLink::CostState* ProjectLink::newCostState() { return new CostState; }

// Compile path / JIT delivery, compileMutex_ held: before `image` is published.
void ProjectLink::bindCost(engine::ProgramImage& image, const std::shared_ptr<const graph::Schedule>& schedule) {
  if (!schedule) return;
  CostState::Meter& m = cost_->meters[schedule.get()];
  if (!m.meter) {
    m.meter = std::make_unique<engine::CostMeter>();
    m.schedule = schedule;
    m.meter->setEnabled(cost_->on);
  }
  m.unusedSince = -1.0;
  m.meter->setTag(tagOf(schedule.get()));
  (void)image.bindInspect(engine::InspectBinding{{}, m.meter.get()});
}

void ProjectLink::setCostMetering(bool on) {
  std::lock_guard lock(compileMutex_);
  cost_->on = on;
  for (auto& [s, m] : cost_->meters) m.meter->setEnabled(on);
  if (!on) cost_->deviceNs.clear();
}

bool ProjectLink::costLive() const noexcept { return !engine_->asleep(); }

void ProjectLink::readDeviceCosts(std::span<const doc::DeviceId> devices, std::span<float> out) {
  for (float& f : out) f = -1.0f;
  std::unique_lock lock(compileMutex_, std::try_to_lock);  // a compile in progress: this poll shows the last readings
  if (lock.owns_lock() && cost_->on && compiler_ && lowered_) {
    CostState& cs = *cost_;
    // Patches whose nodes may be read: the current lowering's track patches and the device library's patches (a unit
    // patch and the composition's identity helper are gone once composed).
    std::unordered_map<const graph::Patch*, uint32_t> trackOf;
    for (size_t i = 0; i < lowered_->patches.size(); ++i) trackOf[&lowered_->patches[i]->patch] = lowered_->trackIds[i].value;
    std::unordered_set<const graph::Patch*> library;
    for (const graph::SubcircuitRef& r : devices_->signatures())
      if (const graph::Patch* p = devices_->library().find(r.patch)) library.insert(p);
    // T4.4-fix1: user devices' patches (and the stdlib files their imports loaded), held alive by the lowering.
    for (const auto& rd : lowered_->userDevices) {
      if (!rd || !rd->graph) continue;
      for (const graph::Patch& p : rd->graph->file.patches) library.insert(&p);
      for (const graph::PatchFile* f : rd->extraFiles)
        if (f != nullptr)
          for (const graph::Patch& p : f->patches) library.insert(&p);
    }
    auto alive = [&](const graph::Patch* p) { return p != nullptr && (trackOf.count(p) != 0 || library.count(p) != 0); };
    // Device ids by their call's NodeId, per track.
    std::unordered_map<uint64_t, uint32_t> deviceOf;  // (track id << 32 | call NodeId) → device id
    if (current_)
      doc::forEachDevice(*current_, [&](const doc::Device& d, const doc::Track& t) {  // T4.8: inside racks too
        deviceOf[(uint64_t{t.id.value} << 32) | doc::nodeIdFor(doc::NodeRole::Device, d.id.value, 0)] = d.id.value;
      });
    std::unordered_set<const graph::Schedule*> running;
    for (uint32_t u = 0; u < compiler_->numUnits(); ++u) {
      const engine::CompiledUnit& cu = compiler_->unit(u);
      if (!cu.schedule || !cu.typed) continue;
      running.insert(cu.schedule.get());
      const auto mit = cs.meters.find(cu.schedule.get());
      if (mit == cs.meters.end()) continue;
      engine::CostMeter::Reading r;
      if (!mit->second.meter->read(r) || r.tag != tagOf(cu.schedule.get())) continue;
      CostState::Attribution& a = cs.attribution[cu.schedule.get()];
      if (a.schedule != cu.schedule) {  // first reading of this schedule: what plays, in NodeIds
        a = {};
        a.schedule = cu.schedule;
        const graph::TypedPatch& scheduled = cu.schedule->rewritten ? *cu.schedule->rewritten : *cu.typed;
        views::circuit::PatchEditor::PlayedProgram& pp = a.played;
        pp.generation = tagOf(cu.schedule.get());
        pp.schedule = *cu.schedule;
        const std::vector<graph::InstanceInfo>& inst = scheduled.instances;
        for (size_t i = 0; i < inst.size(); ++i) {
          pp.instanceParent.push_back(inst[i].parent);
          graph::NodeId call{};
          if (inst[i].parent == 0) {  // a member track: its call's NodeId is its id (graph::composeUnit)
            if (const auto t = trackOf.find(inst[i].patch); t != trackOf.end()) {
              call = graph::NodeId{t->second};
              a.tracks.push_back(t->second);
            }
          } else if (inst[i].parent < inst.size() && alive(inst[inst[i].parent].patch) &&
                     inst[i].callNode < inst[inst[i].parent].patch->nodeCount()) {
            call = inst[inst[i].parent].patch->node(inst[i].callNode).id;
          }
          pp.instanceCall.push_back(call);
        }
        const size_t nn = scheduled.patch.nodeCount();
        pp.nodeInstance.assign(nn, graph::kNone);
        pp.nodeSource.assign(nn, graph::NodeId{});
        pp.nodeWhat.assign(nn, graph::NodeOrigin::What::Node);
        for (size_t n = 0; n < nn && n < scheduled.origin.size(); ++n) {
          const graph::NodeOrigin& o = scheduled.origin[n];
          pp.nodeInstance[n] = o.instance;
          pp.nodeWhat[n] = o.what;
          if (o.instance < inst.size() && alive(inst[o.instance].patch) && o.node < inst[o.instance].patch->nodeCount())
            pp.nodeSource[n] = inst[o.instance].patch->node(o.node).id;
        }
      }
      views::circuit::PatchPlayer::CostReading cr;
      cr.generation = a.played.generation;
      cr.totalNsPerFrame = r.totalNsPerFrame;
      cr.regionNsPerFrame = r.regionNsPerFrame;
      cr.units = r.units;
      for (const uint32_t track : a.tracks) {
        views::circuit::CostMap map;
        const graph::NodeId path[1] = {graph::NodeId{track}};
        if (!views::circuit::attributeCost(a.played, cr, path, map)) continue;
        for (const views::circuit::CostMap::Entry& e : map.nodes)
          if (const auto d = deviceOf.find((uint64_t{track} << 32) | e.node.value); d != deviceOf.end())
            cs.deviceNs[d->second] = static_cast<float>(e.ns);
      }
    }
    // Schedules that stopped running: their attribution goes now, their meter after kRetireSeconds.
    const double now = nowSeconds();
    for (auto it = cs.attribution.begin(); it != cs.attribution.end();)
      it = running.count(it->first) ? std::next(it) : cs.attribution.erase(it);
    for (auto it = cs.meters.begin(); it != cs.meters.end();) {
      if (running.count(it->first)) {
        it->second.unusedSince = -1.0;
        ++it;
      } else if (it->second.unusedSince < 0.0) {
        it->second.unusedSince = now;
        ++it;
      } else {
        it = now - it->second.unusedSince > CostState::kRetireSeconds ? cs.meters.erase(it) : std::next(it);
      }
    }
  }
  for (size_t i = 0; i < devices.size() && i < out.size(); ++i)
    if (const auto it = cost_->deviceNs.find(devices[i].value); it != cost_->deviceNs.end()) out[i] = it->second;
}

}  // namespace pq::app
