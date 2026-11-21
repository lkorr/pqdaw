#include "pq/engine/note_stage.h"

#include <utility>

namespace pq::engine {

NoteStageChain::NoteStageChain(uint32_t eventCapacity) : in_(eventCapacity), capacity_(eventCapacity) {}

bool NoteStageChain::add(std::unique_ptr<NoteStage> stage) {
  if (stage == nullptr || stages_.size() >= kMaxNoteStages) return false;
  stage->setIndex(static_cast<uint32_t>(stages_.size()));
  if (!stages_.empty()) mid_.emplace_back(capacity_);
  stages_.push_back(std::move(stage));
  return true;
}

bool NoteStageChain::run(const NoteStageBlock& block, EventBuffer& out) noexcept {  // RT-safe
  bool busy = false;
  const auto n = static_cast<uint32_t>(stages_.size());
  const EventBuffer* in = &in_;
  for (uint32_t i = 0; i < n; ++i) {
    NoteStage& s = *stages_[i];
    EventBuffer& o = i + 1 < n ? mid_[i] : out;
    if (i + 1 < n) o.clear();
    busy = s.processFn(&s, block, in->list(), o) || busy;
    in = &o;
  }
  return busy;
}

void NoteStageChain::flush(uint32_t frame, EventBuffer& out, EventBuffer& held) noexcept {  // RT-safe
  // Stage i > 0 held stage i − 1's generated notes, which are ending: only stage 0's key-held notes are carried over.
  EventBuffer* sink = &held;
  for (auto& s : stages_) {
    if (sink != &held) sink->clear();
    s->flushFn(s.get(), frame, out, *sink);
    if (sink == &held && !mid_.empty()) sink = &mid_[0];  // later stages' held sets are dropped
  }
  if (!mid_.empty()) mid_[0].clear();
}

}  // namespace pq::engine
