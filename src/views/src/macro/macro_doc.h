#pragma once

// Macro mode's document access for just intonation (T8.1; private to pq_views). EVERYTHING macro mode knows about how
// the document stores "a JI note", "the group it belongs to" and "Is Harmonic N" is in these functions, so a change of
// the JI data model is a change of macro_doc.cpp only.
//
// Written against the notes-first groups (T7.13, plan M7 R2): a JI group is `JiGroup {id, anchor, index}` in
// NoteClip::groups: an anchor note of any identity declared "harmonic N", and its members, the Ratio notes anchored to
// that note. A JI note is a group's anchor, a group's member, or any other Ratio note (over the clip's reference, or
// anchored to a note that anchors no group).

#include <cstdint>
#include <optional>

#include "pq/document/edit_ops.h"
#include "pq/document/model.h"
#include "pq/tuning/rational.h"

namespace pq::views::macro {

// What a just note hangs on: the clip's reference pitch, or a note (the group's anchor).
struct JiRoot {
  bool reference = false;
  doc::NoteId note;
  friend bool operator==(const JiRoot&, const JiRoot&) noexcept = default;
};

struct JiMembership {
  JiRoot root;
  tuning::Rational ratio{1};   // the note's exact ratio to the root (1 for the root itself)
  doc::JiGroupId group;        // the JI group the note is in (its anchor or a member); invalid: none
  uint32_t declaredIndex = 0;  // that group's index ("Is Harmonic N"); 0: no group
  // The fundamental as a ratio to the root (a group: 1 / (N · m), doc::groupFundamental; reference Ratio notes: the
  // clip's derived 1/D); nullopt: none (the cursor then steps on the note's own denominator).
  std::optional<tuning::Rational> fundamental;
};

// 1. Is `n` a JI note, and which group is it in? nullopt: a tempered note.
std::optional<JiMembership> jiMembership(const doc::NoteClip& clip, const doc::Note& n);

// 2. A new note at exactly `ratio` to `root` (one undo step): a member of the group `root.note` anchors
// (ops::addGroupNote); a Ratio over the reference; or, when the root note anchors no group (the JI layer from a
// tempered note), a Ratio anchored to that note, which a later "Is Harmonic N" on it makes a member.
doc::ops::EditResult placeJustNote(const doc::Project& p, doc::IdSource& ids, doc::ClipRef clip, const doc::NoteClip& notes,
                                   const JiRoot& root, const tuning::Rational& ratio, Ticks start, Ticks length, float velocity);

// 3. "Is Harmonic N" on a note, as the roll's menu does it (ops::makeJiGroup), and its release: the group `note` is
// in is removed (ops::removeJiGroup: its members keep their pitch). One undo step each.
doc::ops::EditResult declareHarmonic(const doc::Project& p, doc::IdSource& ids, doc::ClipRef clip, doc::NoteKey note, uint32_t index);
doc::ops::EditResult undeclareHarmonic(const doc::Project& p, doc::ClipRef clip, const doc::NoteClip& notes, const doc::Note& note);

}  // namespace pq::views::macro
