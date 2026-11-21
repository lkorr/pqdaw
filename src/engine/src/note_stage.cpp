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
  // AP.8a (review M2): each stage adopts the old stage with its key and kind ANYWHERE after the last one adopted (an
  // order-keeping match: a device inserted, removed or switched off before a running arp, or the tail edited, never
  // restarts it; of two swapped stages one adopts, the other starts fresh).
  const uint32_t n = size(), m = old.size();
  bool adopted[kMaxNoteStages] = {};
  uint32_t from = 0, count = 0;
  for (uint32_t i = 0; i < n && from < m; ++i) {
    NoteStage& a = *stages_[i];
    if (a.adoptFn == nullptr || a.key == 0) continue;
    for (uint32_t j = from; j < m; ++j) {
      NoteStage& b = *old.stages_[j];
      if (b.adoptFn != a.adoptFn || b.key != a.key) continue;
      a.adoptFn(&a, &b);  // b keeps a fresh state: nothing to flush
      adopted[j] = true;
      from = j + 1;
      ++count;
      break;
    }
  }
  if (count == 0) {
    old.flush(frame, out, held);
    return;
  }
  // The old stages that were not adopted end their notes. Their generated notes went into old stage j + 1: when that
  // one was adopted, the offs (kNoteEventGone) go into this chain's input (`held`) to reach it (the stages before it
  // pass ids they do not hold); the last stage's go to the voices; otherwise they are dropped with their receiver.
  // Old stage 0's key-held source notes go into this chain's input as note-ons (as in flush), before those offs.
  if (count == m) return;
  EventBuffer& scratch = old.mid_[0];  // (m ≥ 2 here: an adopted stage and a flushed one; old runs no more blocks)
  for (uint32_t j = 0; j < m; ++j) {
    if (adopted[j]) continue;
    EventBuffer& offs = j + 1 == m ? out : (adopted[j + 1] ? held : scratch);
    EventBuffer& ons = j == 0 ? held : scratch;
    scratch.clear();
    old.stages_[j]->flushFn(old.stages_[j].get(), frame, offs, ons);
  }
  scratch.clear();
}

}  // namespace pq::engine
