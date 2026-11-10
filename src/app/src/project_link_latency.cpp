// T6.2: plugin-delay compensation, the link's side (pq/app/project_link.h "Plugin-delay compensation").
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/engine/latency_plan.h"

namespace pq::app {

ProjectLink::LatencyJob ProjectLink::planLatency(const doc::LoweredProject& lp) {  // compile path (compileMutex_)
  const graph::ProjectPartition& part = compiler_->partition();
  engine::ProjectLayout layout;  // what planCompensation reads: units, bus producers, the output
  layout.units.resize(part.units.size());
  for (const graph::ProjectBus& b : part.buses) {
    layout.busWidth.push_back(1);
    layout.busProducer.push_back(b.producer);
  }
  layout.outputBus = part.outputBus;
  // T6.6: Track Delay → the unit's offset, negated (see the file comment of project_link.h); a merged unit takes the
  // delay of the largest magnitude among its members.
  std::vector<int32_t> offset;
  {
    std::lock_guard lock(mutex_);
    latencyReplan_ = false;  // this plan reads the delays as they are now
    if (!trackDelayMs_.empty()) {
      const double rate = engine_->config().sampleRate;
      offset.assign(part.units.size(), 0);
      for (uint32_t i = 0; i < lp.trackIds.size() && i < part.unitOf.size(); ++i) {
        const auto it = trackDelayMs_.find(lp.trackIds[i].value);
        const uint32_t u = part.unitOf[i];
        if (it == trackDelayMs_.end() || u >= offset.size()) continue;
        const auto d = static_cast<int32_t>(std::lround(static_cast<double>(it->second) * 1e-3 * rate));
        if (std::abs(d) > std::abs(offset[u])) offset[u] = -d;
      }
    }
  }
  LatencyJob job;
  const std::vector<engine::UnitLatency> lat = compiler_->latencies();
  job.plan = engine::planCompensation(layout, lat, offset);
  job.inputs = unitInputs(lp);  // T6.7: the units' track inputs
  // A param's lane is shifted by its track's S(t) = A_u + T(t) (T6.2b: T > 0 in a merged unit whose member waits for a
  // later sidechain inside it; latency_plan.h). A project param several tracks read (a tuning's generator: one lane, one
  // tap for every tuning node) takes the smallest of their shifts (QUESTIONS Q?-T6.2b-a).
  std::vector<uint8_t> seen;
  bool any = false;
  for (const engine::ProjectParamLink& l : compiler_->options().paramLinks) {
    if (l.track >= part.unitOf.size() || part.unitOf[l.track] >= job.plan.arrival.size()) continue;
    const uint32_t u = part.unitOf[l.track];
    double t = 0.0;
    if (u < lat.size() && l.track < lp.trackIds.size())
      for (const auto& [id, shift] : lat[u].trackShift)
        if (id == lp.trackIds[l.track].value) t = shift;
    const auto a = static_cast<uint32_t>(std::max(0L, std::lround(job.plan.arrival[u] + t)));
    if (job.shift.size() <= l.projectParam) {
      job.shift.resize(l.projectParam + 1, 0);
      seen.resize(l.projectParam + 1, 0);
    }
    job.shift[l.projectParam] = seen[l.projectParam] != 0 ? std::min(job.shift[l.projectParam], a) : a;
    seen[l.projectParam] = 1;
  }
  for (const uint32_t a : job.shift) any = any || a != 0;
  if (!any) job.shift.clear();  // no shift anywhere: the lanes stay as they are
  return job;
}

Status ProjectLink::applyLatency(const LatencyJob& job) {  // compile path (compileMutex_)
  engine::ProjectProgram* program = engine_->program();
  if (program == nullptr) return Ok{};
  program->setUnitInputs(job.inputs);  // T6.7
  {
    std::lock_guard lock(mutex_);  // T6.6: the strips' readouts
    busLatency_ = job.plan.busLatency;
  }
  // A project without latency never makes a plan; an unchanged plan is not sent again — unless a topology came in
  // between (M6 review L6: a plan queued for the older one may have been dropped unused, and equal indices name other
  // buses; for a program without a plan an empty one costs nothing).
  const bool same = !planStale_ && (lastPlan_ ? lastPlan_->sameAs(job.plan) : job.plan.none());
  if (!same) {
    // Plans the audio thread has not taken yet (a stopped output) stay queued in the program up to its limit; past it
    // the newest plan waits here and drainDeferred hands it over later (never an error: nothing is lost but time).
    Status st = program->setCompensation(job.plan);
    if (!st.ok() && st.error().code == ErrorCode::Overflow) {
      pendingLatency_ = job;
      anyDeferred_.store(true, std::memory_order_release);
    } else if (!st.ok()) {
      return st;
    } else {
      lastPlan_ = job.plan;
      planStale_ = false;
      pendingLatency_.reset();
    }
  }
  bool changed = false;
  {
    std::lock_guard lock(mutex_);
    if (paramShift_ != job.shift) {
      paramShift_ = job.shift;
      changed = true;
    }
  }
  if (changed) {
    shiftsChanged_.store(true, std::memory_order_release);
    std::function<void(std::function<void()>)> post;
    {
      std::lock_guard lock(loadMutex_);
      post = post_;
    }
    if (post) {
      std::weak_ptr<int> alive = alive_;
      post([this, alive] {
        if (alive.lock()) takeLatencyShifts();
      });
    }
  }
  return Ok{};
}

void ProjectLink::takeLatencyShifts() {  // UI thread
  if (shiftsChanged_.exchange(false, std::memory_order_acq_rel) && current_) publishAutomation(*current_);
}

void ProjectLink::replanLatency() {  // compile path (compileMutex_ held)
  {
    std::lock_guard lock(mutex_);
    if (!latencyReplan_) return;
  }
  // Only for the layout the program runs: a compile result still queued carries its own plan (made after the edit,
  // or followed by this one once it is in).
  if (!deferred_.empty() || !lowered_ || engine_->program() == nullptr) return;
  (void)applyLatency(planLatency(*lowered_));
}

void ProjectLink::syncRecording(const doc::Project& p) {  // UI thread
  engine::Metronome& m = engine_->metronome();
  m.setEnabled(p.record.metronome);
  m.setLevelDb(p.record.metronomeDb);
  m.setCue(p.record.metronomeCue);
  std::unordered_map<uint32_t, float> delays;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    if (t.kind != doc::TrackKind::Master && t.mixer.trackDelayMs != 0.0f) delays[t.id.value] = t.mixer.trackDelayMs;
  });
  {
    std::lock_guard lock(mutex_);
    if (delays == trackDelayMs_) return;
    trackDelayMs_ = std::move(delays);
    latencyReplan_ = true;
    anyDeferred_.store(true, std::memory_order_release);  // the worker drains, then plans again
  }
  cv_.notify_all();
}

double ProjectLink::outputLatencyMs() {
  return static_cast<double>(outputLatency()) * 1e3 / engine_->config().sampleRate;
}

double ProjectLink::trackLatencyMs(uint32_t trackId) {
  // The track's own latency (its devices and what it receives): L(out bus) without its Track Delay's offset.
  const double rate = engine_->config().sampleRate;
  std::lock_guard lock(mutex_);
  const auto it = meterBus_.find(trackId);
  if (it == meterBus_.end() || it->second >= busLatency_.size()) return 0.0;
  double frames = busLatency_[it->second];
  if (const auto d = trackDelayMs_.find(trackId); d != trackDelayMs_.end())
    frames += static_cast<double>(std::lround(static_cast<double>(d->second) * 1e-3 * rate));
  return frames * 1e3 / rate;
}

uint32_t ProjectLink::outputLatency() const noexcept {
  const engine::ProjectProgram* program = engine_ ? engine_->program() : nullptr;
  return program != nullptr ? program->outputLatency() : 0u;
}

}  // namespace pq::app
