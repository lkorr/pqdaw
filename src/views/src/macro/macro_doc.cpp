// Macro mode's document access for just intonation (T8.1): see macro_doc.h.

#include "macro_doc.h"

#include <vector>

namespace pq::views::macro {

std::optional<JiMembership> jiMembership(const doc::NoteClip& clip, const doc::Note& n) {
  JiMembership m;
  if (const doc::JiGroup* g = doc::jiGroupOfNote(clip, n)) {  // a group's anchor (any identity) or one of its members
    m.root.note = g->anchor;
    if (n.id != g->anchor) m.ratio = n.pitch.ratio();
    m.group = g->id;
    m.declaredIndex = g->index;
    if (const std::optional<doc::GroupFundamental> f = doc::groupFundamental(clip, *g)) m.fundamental = f->fundamental;
    return m;
  }
  if (n.pitch.kind() != tuning::PitchKind::Ratio) return std::nullopt;
  // A Ratio note outside every group: follow note anchors to its root, a note that is no Ratio, or the reference.
  tuning::PitchId p = n.pitch;
  for (int depth = 0;; ++depth) {
    if (depth >= 16) return std::nullopt;  // a cycle
    const std::optional<tuning::Rational> r = tuning::mul(m.ratio, p.ratio());
    if (!r) return std::nullopt;
    m.ratio = *r;
    if (p.anchorKind() == tuning::AnchorKind::Reference) {
      m.root.reference = true;
      m.fundamental = doc::virtualFundamental(clip);
      return m;
    }
    if (p.anchorKind() != tuning::AnchorKind::Note) return std::nullopt;
    const doc::Note* a = clip.notes.findById(doc::NoteId{p.anchorId()});
    if (!a) return std::nullopt;
    if (a->pitch.kind() != tuning::PitchKind::Ratio) {
      m.root.note = a->id;
      return m;
    }
    p = a->pitch;
  }
}

doc::ops::EditResult placeJustNote(const doc::Project& p, doc::IdSource& ids, doc::ClipRef clip, const doc::NoteClip& notes,
                                   const JiRoot& root, const tuning::Rational& ratio, Ticks start, Ticks length, float velocity) {
  if (!ratio.isPositive()) return Error{ErrorCode::InvalidArgument, "the ratio must be positive", {}};
  if (!root.reference)
    if (const doc::JiGroup* g = doc::findJiGroupByAnchor(notes, root.note))
      return doc::ops::addGroupNote(p, ids, clip, g->id, doc::ops::GroupNotePitch{ratio, 0}, start, length, velocity);
  doc::Note n;
  n.start = start;
  n.length = length;
  n.velocity = velocity;
  n.pitch = root.reference ? tuning::PitchId::fromRatio(ratio)
                           : tuning::PitchId::fromRatio(ratio, tuning::AnchorKind::Note, root.note.value);
  return doc::ops::addNotes(p, ids, clip, std::vector<doc::Note>{n});
}

doc::ops::EditResult declareHarmonic(const doc::Project& p, doc::IdSource& ids, doc::ClipRef clip, const doc::NoteClip& notes,
                                     const doc::Note& note, uint32_t index) {
  // A member of a group becomes that group's anchor (the menu's rule, Q297 b): no second, chained group.
  if (!doc::findJiGroupByAnchor(notes, note.id))
    if (const doc::JiGroup* of = doc::jiGroupOfNote(notes, note))
      return doc::ops::reanchorJiGroup(p, clip, of->id, doc::keyOf(note), index);
  return doc::ops::makeJiGroup(p, ids, clip, doc::keyOf(note), index);
}

doc::ops::EditResult undeclareHarmonic(const doc::Project& p, doc::ClipRef clip, const doc::NoteClip& notes, const doc::Note& note) {
  const doc::JiGroup* g = doc::jiGroupOfNote(notes, note);
  if (!g) return Error{ErrorCode::NotFound, "the note is in no JI group", {}};
  return doc::ops::removeJiGroup(p, clip, g->id);
}

}  // namespace pq::views::macro
