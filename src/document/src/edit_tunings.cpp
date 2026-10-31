// Main-tuning edit operations (T3.19, SPEC §3.2): add, and remove with reassignment of what used the tuning.

#include <algorithm>

#include "edit_common.h"

namespace pq::doc::ops {

using namespace detail;

EditResult addMainTuning(const Project& p, IdSource& ids, std::string name, TuningDef def, Colour colour) {
  (void)p;
  if (name.empty()) return invalid("a main tuning needs a name");
  if (Result<tuning::Tuning> t = buildTuning(def); !t) return std::move(t).error();
  Edit e = makeEdit("Add Tuning");
  TuningSlot slot;
  slot.id = ids.tuning();
  slot.name = std::move(name);
  slot.colour = colour;
  slot.def = std::move(def);
  e.tunings.push_back(slot.id);
  batchOf(e).commands.push_back(AddTuning{std::move(slot)});
  return e;
}

EditResult removeMainTuning(const Project& p, TuningId tuning, TuningId replacement) {
  if (!findTuning(p, tuning)) return notFound("tuning not found");
  if (p.tunings->size() < 2) return invalid("a project needs at least one main tuning");
  if (!replacement.valid() || replacement == tuning) {
    replacement = {};
    for (const TuningSlotPtr& s : *p.tunings)
      if (s->id != tuning) {
        replacement = s->id;
        break;
      }
  }
  if (!findTuning(p, replacement)) return notFound("replacement tuning not found");
  Edit e = makeEdit("Delete Tuning");
  Batch& b = batchOf(e);
  auto uses = [&](const TuningAssignment& a) { return a.mode == TuningMode::Main && a.main == tuning; };
  forEachTrack(p, [&](const Track& t, const Track*) {
    bool touched = false;
    if (uses(t.tuning)) {
      b.commands.push_back(SetTrackTuning{t.id, TuningAssignment::mainTuning(replacement)});
      touched = true;
    }
    for (const ClipSlot& c : t.clips)
      if (uses(c.audio ? c.audio->tuning : c.clip->tuning)) {
        b.commands.push_back(SetClipTuning{{t.id, c.id}, TuningAssignment::mainTuning(replacement)});
        touched = true;
      }
    if (touched) e.tracks.push_back(t.id);
  });
  b.commands.push_back(RemoveTuning{tuning});
  e.tunings.push_back(replacement);
  return e;
}

}  // namespace pq::doc::ops
