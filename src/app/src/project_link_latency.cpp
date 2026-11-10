// T6.2: plugin-delay compensation, the link's side (pq/app/project_link.h "Plugin-delay compensation").
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/engine/latency_plan.h"

namespace pq::app {

ProjectLink::LatencyJob ProjectLink::planLatency() const {  // compile path (compileMutex_)
  const graph::ProjectPartition& part = compiler_->partition();
  engine::ProjectLayout layout;  // what planCompensation reads: units, bus producers, the output
  layout.units.resize(part.units.size());
  for (const graph::ProjectBus& b : part.buses) {
    layout.busWidth.push_back(1);
    layout.busProducer.push_back(b.producer);
  }
  layout.outputBus = part.outputBus;
  LatencyJob job;
  job.plan = engine::planCompensation(layout, compiler_->latencies());
  for (const engine::ProjectParamLink& l : compiler_->options().paramLinks) {
    if (l.track >= part.unitOf.size() || part.unitOf[l.track] >= job.plan.unitLatency.size()) continue;
    const uint32_t a = job.plan.unitLatency[part.unitOf[l.track]];
    if (a == 0) continue;
    if (job.shift.size() <= l.projectParam) job.shift.resize(l.projectParam + 1, 0);
    job.shift[l.projectParam] = a;
  }
  return job;
}

Status ProjectLink::applyLatency(const LatencyJob& job) {  // compile path (compileMutex_)
  engine::ProjectProgram* program = engine_->program();
  if (program == nullptr) return Ok{};
  // A project without latency never makes a plan; an unchanged plan is not sent again.
  const bool same = lastPlan_ ? lastPlan_->sameAs(job.plan) : job.plan.none();
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

uint32_t ProjectLink::outputLatency() const noexcept {
  const engine::ProjectProgram* program = engine_ ? engine_->program() : nullptr;
  return program != nullptr ? program->outputLatency() : 0u;
}

}  // namespace pq::app
