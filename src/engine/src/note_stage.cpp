#include "pq/engine/note_stage.h"

#include <algorithm>
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

void NoteStageChain::takeOver(NoteStageChain& old, uint32_t frame, EventBuffer& out, EventBuffer& held) noexcept {  // RT-safe
  uint32_t n = 0;
  const uint32_t m = std::min(size(), old.size());
  while (n < m) {
    NoteStage& a = *stages_[n];
    NoteStage& b = *old.stages_[n];
    if (a.adoptFn == nullptr || a.adoptFn != b.adoptFn || a.key == 0 || a.key != b.key) break;
    a.adoptFn(&a, &b);  // b keeps a fresh state: nothing to flush
    ++n;
  }
  if (n == 0) {
    old.flush(frame, out, held);
    return;
  }
  // Old stages after the adopted ones end their notes; their held sets (the adopted stage's notes) are dropped.
  for (uint32_t i = n; i < old.size(); ++i) {
    EventBuffer& sink = old.mid_[i - 1];
    sink.clear();
    old.stages_[i]->flushFn(old.stages_[i].get(), frame, out, sink);
    sink.clear();
  }
}

}  // namespace pq::engine
