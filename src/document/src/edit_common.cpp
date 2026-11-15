// Internals shared by the edit-operation builders (T3.6).

#include "edit_common.h"

#include "pq/document/note_bend.h"
#include "pq/document/pitch_resolve.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace pq::doc {

double automationValueBefore(std::span<const Breakpoint> pts, Ticks t, double fallback) noexcept {
  if (pts.empty()) return fallback;
  const auto it = std::lower_bound(pts.begin(), pts.end(), t, [](const Breakpoint& b, Ticks v) { return b.tick < v; });
  if (it == pts.end()) return pts.back().value;
  if (it->tick == t || it == pts.begin()) return it->value;
  return automationSegmentValue(*(it - 1), *it, static_cast<double>(t));
}

double automationValueAt(std::span<const Breakpoint> pts, Ticks t, double fallback) noexcept {
  if (pts.empty()) return fallback;
  const auto it = std::upper_bound(pts.begin(), pts.end(), t, [](Ticks v, const Breakpoint& b) { return v < b.tick; });
  if (it == pts.begin()) return pts.front().value;  // before (or on, handled by upper_bound) the first point
  const Breakpoint& a = *(it - 1);                  // last point with tick ≤ t
  if (a.tick == t || it == pts.end()) return a.value;
  return automationSegmentValue(a, *it, static_cast<double>(t));
}

namespace ops {

bool Edit::empty() const noexcept { return batch().commands.empty(); }
const Batch& Edit::batch() const noexcept { return std::get<Batch>(command.v); }

namespace detail {

Edit makeEdit(std::string label) {
  Edit e;
  e.command = Batch{std::move(label), {}};
  return e;
}

Batch& batchOf(Edit& e) { return std::get<Batch>(e.command.v); }

// ---- Lookups ---------------------------------------------------------------------------------------------------

Result<ClipFound> findAnyClip(const Project& p, const ClipRef& r) {
  const auto path = findTrack(p, r.track);
  if (!path) return notFound("track not found");
  const Track& t = trackAt(p, *path);
  const int32_t i = findClipIndex(t, r.clip);
  if (i < 0) return notFound("clip not found on this track");
  const ClipSlot& s = t.clips[static_cast<size_t>(i)];
  return ClipFound{*path, &t, s.clip, s.audio};
}

Result<ClipFound> findClip(const Project& p, const ClipRef& r) {
  Result<ClipFound> f = findAnyClip(p, r);
  if (f && !f->clip) return invalid("not a note clip");
  return f;
}

Result<const Track*> findClipTrack(const Project& p, TrackId id) {
  const Track* t = findTrackPtr(p, id);
  if (!t) return notFound("track not found");
  if (t->kind != TrackKind::Midi && t->kind != TrackKind::Audio) return invalid("clips live on MIDI and audio tracks");
  return t;
}

Result<const Track*> findMidiTrack(const Project& p, TrackId id) {
  const Track* t = findTrackPtr(p, id);
  if (!t) return notFound("track not found");
  if (t->kind != TrackKind::Midi) return invalid("note clips live on MIDI tracks");
  return t;
}

const AutomationLane* findLane(const Track& t, LaneId id) {
  for (const LanePtr& l : t.lanes)
    if (l->id == id) return l.get();
  return nullptr;
}

ClipSlot cloneSlot(const ClipSlot& c, IdSource& ids, bool onlyPlaying) {
  if (c.audio) {
    AudioClip a = *c.audio;
    a.id = ids.clip();
    return ClipSlot{a.id, nullptr, std::make_shared<const AudioClip>(std::move(a))};
  }
  auto n = std::make_shared<const NoteClip>(cloneClip(*c.clip, ids, onlyPlaying));
  return ClipSlot{n->id, std::move(n), nullptr};
}

Result<tuning::Tuning> clipTuning(const Project& p, const ClipFound& c) {
  const TuningDef* def = effectiveTuning(p, c.path, c.clip.get());
  if (!def) return notFound("the clip's tuning was not found");
  return buildTuning(*def);
}

// ---- Clips as values ---------------------------------------------------------------------------------------------

bool sameNotes(const NoteSeq& a, const NoteSeq& b) noexcept {
  const auto ca = a.chunks();
  const auto cb = b.chunks();
  if (ca.size() != cb.size()) return false;
  for (size_t i = 0; i < ca.size(); ++i)
    if (ca[i].notes != cb[i].notes || ca[i].count != cb[i].count) return false;
  return true;
}

NoteSeq seqOf(std::vector<Note> notes) {
  Result<NoteSeq> s = NoteSeq::fromNotes(std::move(notes));
  assert(s.ok() && "seqOf: notes must be valid with unique ids");
  return s.ok() ? std::move(s).value() : NoteSeq{};
}

namespace {
// T7.16a (M7 review M8): a curve record that moves the pitch a note anchored to it takes (a free curve, a bend); a
// vibrato alone does not (pitch_resolve: an ornament is not a pitch to tune to).
bool movesPitch(const NoteCurves* c) noexcept { return c != nullptr && (!c->pitch.empty() || c->bendIn.set() || c->bendOut.set()); }

Ticks floorMod(Ticks a, Ticks m) {
  const Ticks r = a % m;
  return r < 0 ? r + m : r;
}
}  // namespace

NoteClip withBounds(const NoteClip& c, Ticks start, Ticks end) {
  NoteClip n = withNotes(c, c.notes);
  if (start >= c.start) {
    n.contentStart = clipContentAt(c, start - c.start);
  } else {
    const Ticks d = c.start - start;
    if (clipLooped(c) && c.contentStart >= c.loop.start && c.contentStart < c.loop.end) {
      // Unroll the loop backwards, so the part that already played keeps its content.
      const Ticks len = c.loop.end - c.loop.start;
      n.contentStart = c.loop.start + floorMod(c.contentStart - d - c.loop.start, len);
    } else {
      n.contentStart = c.contentStart - d;
    }
  }
  n.start = start;
  n.length = end - start;
  return n;
}

std::pair<NoteClip, NoteClip> splitClipAt(const NoteClip& c, Ticks t, ClipId rightId, IdSource& ids, const Project& p,
                                          const TrackPath& path) {
  NoteClip left = withNotes(c, c.notes);
  left.length = t - c.start;
  if (clipLooped(c)) {
    NoteClip right = cloneClip(c, ids, false);
    right.id = rightId;
    right.start = t;
    right.length = clipEnd(c) - t;
    right.contentStart = clipContentAt(c, t - c.start);
    return {std::move(left), std::move(right)};
  }
  const Ticks sc = c.contentStart + (t - c.start);
  std::vector<Note> moved;
  std::vector<NoteKey> keys;
  for (auto [it, end] = c.notes.range(sc, kMaxTicks + 1); it != end; ++it) {
    moved.push_back(*it);
    keys.push_back(keyOf(*it));
  }
  if (!keys.empty()) left.notes = c.notes.withRemoved(keys);
  NoteClip right = withNotes(c, seqOf(std::move(moved)));
  right.id = rightId;
  right.start = t;
  right.length = clipEnd(c) - t;
  right.contentStart = sc;
  // T7.11a (H3): a note parted from its anchor by the cut keeps its pitch (reanchorWithout), in either piece; T7.13:
  // a group parted from its anchor promotes a member there, so a group cut in two is a group in each piece.
  if (left.notes.size() != c.notes.size() && right.notes.size() != c.notes.size()) {
    std::vector<NoteId> l, r;
    for (const Note& n : left.notes) l.push_back(n.id);
    for (const Note& n : right.notes) r.push_back(n.id);
    std::sort(l.begin(), l.end());
    std::sort(r.begin(), r.end());
    applyReanchored(left, reanchorWithout(p, path, c, r));
    applyReanchored(right, reanchorWithout(p, path, c, l));
  }
  pruneCurves(left);
  pruneCurves(right);
  pruneJi(left);
  pruneJi(right);
  if (left.groups && right.groups) {  // ids name one entity: a group in both pieces is a new group on the right
    JiGroupList list = *right.groups;
    bool any = false;
    for (JiGroup& g : list)
      if (findJiGroup(left, g.id) != nullptr) {
        g.id = ids.jiGroup();
        any = true;
      }
    if (any) {
      std::sort(list.begin(), list.end(), [](const JiGroup& a, const JiGroup& b) { return a.id < b.id; });
      right.groups = std::make_shared<const JiGroupList>(std::move(list));
    }
  }
  return {std::move(left), std::move(right)};
}

NoteIdMap noteIdMap(std::span<const NoteId> sources, std::span<const Note> copies) {
  NoteIdMap map;
  map.reserve(sources.size());
  for (size_t i = 0; i < sources.size(); ++i) map.emplace_back(sources[i], copies[i].id);
  std::sort(map.begin(), map.end());
  return map;
}

NoteId remapNote(NoteId id, const NoteIdMap& map) noexcept {
  const auto it = std::lower_bound(map.begin(), map.end(), id, [](const auto& e, NoteId x) { return e.first < x; });
  return it != map.end() && it->first == id ? it->second : NoteId{};
}

NoteCurves remapNoteCurves(const NoteCurves& nc, NoteId note, const NoteIdMap& map, const NoteClip* target) {
  NoteCurves out = nc;
  out.note = note;
  auto point = [&](NoteBend& b) {
    if (!b.set()) return;
    const NoteId to = remapNote(b.other, map);
    if (to.valid()) b.other = to;
    else if (target == nullptr || b.other == note || target->notes.findById(b.other) == nullptr) b = {};
  };
  point(out.bendIn);
  point(out.bendOut);
  return out;
}

Result<std::optional<SetNoteCurves>> copiedCurves(ClipRef clip, const NoteClip& target, std::span<const NoteCurves> curves,
                                                  std::span<const NoteId> sources, std::span<const Note> copies) {
  if (curves.empty() || copies.empty()) return std::optional<SetNoteCurves>{};
  std::vector<const NoteCurves*> byNote;
  byNote.reserve(curves.size());
  for (const NoteCurves& nc : curves) byNote.push_back(&nc);
  std::sort(byNote.begin(), byNote.end(), [](const NoteCurves* x, const NoteCurves* y) { return x->note < y->note; });
  const NoteIdMap map = noteIdMap(sources, copies);
  SetNoteCurves set{clip, {}};
  for (size_t i = 0; i < copies.size(); ++i) {
    if (!sources[i].valid()) continue;
    const auto it = std::lower_bound(byNote.begin(), byNote.end(), sources[i],
                                     [](const NoteCurves* x, NoteId id) { return x->note < id; });
    if (it == byNote.end() || (*it)->note != sources[i]) continue;
    NoteCurves copy = remapNoteCurves(**it, copies[i].id, map, &target);
    if (copy.empty()) continue;
    PQ_DOC_TRY(checkNoteCurveRecord(copy));
    set.curves.push_back(std::move(copy));
  }
  if (set.curves.empty()) return std::optional<SetNoteCurves>{};
  return std::optional<SetNoteCurves>(std::move(set));
}

Result<CopiedJi> copyJi(ClipRef clip, const NoteClip& target, const JiSource& source, std::span<const NoteId> sources,
                        std::span<Note> copies, IdSource& ids) {
  CopiedJi out;
  bool anchored = false;
  for (const Note& n : copies) anchored = anchored || noteAnchorOf(n.pitch).valid();
  if (!anchored) return out;  // no member among the copies: a group is not copied for its anchor alone
  const NoteIdMap map = noteIdMap(sources, copies);
  // An identity anchored to a note that was copied too names the copy; to one that was not, it is left as it is (a
  // note of the target clip: a copy inside its own clip, still a member of the original group).
  for (Note& n : copies)
    if (const NoteId to = remapNote(noteAnchorOf(n.pitch), map); to.valid())
      n.pitch = tuning::PitchId::fromRatio(n.pitch.ratio(), tuning::AnchorKind::Note, to.value);
  // A group whose anchor was copied together with a member is copied (fresh id, same index and warps).
  JiGroupList groups;
  for (const JiGroup& g : source.groups) {
    const NoteId to = remapNote(g.anchor, map);
    if (!to.valid() || std::ranges::none_of(copies, [&](const Note& n) { return noteAnchorOf(n.pitch) == to; })) continue;
    JiGroup copy = g;
    copy.id = ids.jiGroup();
    copy.anchor = to;
    PQ_DOC_TRY(checkJiGroup(copy));
    out.made.push_back(copy.id);
    groups.push_back(std::move(copy));
  }
  // A copied member has its source's written identity.
  NoteWrittenList written;
  if (!source.written.empty()) {
    std::vector<const NoteWritten*> byNote;
    byNote.reserve(source.written.size());
    for (const NoteWritten& w : source.written) byNote.push_back(&w);
    std::sort(byNote.begin(), byNote.end(), [](const NoteWritten* x, const NoteWritten* y) { return x->note < y->note; });
    for (size_t i = 0; i < copies.size(); ++i) {
      if (!sources[i].valid() || !noteAnchorOf(copies[i].pitch).valid()) continue;
      const auto it = std::lower_bound(byNote.begin(), byNote.end(), sources[i],
                                       [](const NoteWritten* x, NoteId id) { return x->note < id; });
      if (it == byNote.end() || (*it)->note != sources[i]) continue;
      PQ_DOC_TRY(checkWrittenPitch((*it)->written));
      written.push_back(NoteWritten{copies[i].id, (*it)->written});
    }
  }
  if (!groups.empty()) {
    groups.insert(groups.begin(), jiGroupsOf(target).begin(), jiGroupsOf(target).end());
    out.groups = SetJiGroups{clip, std::move(groups)};
  }
  if (!written.empty()) {
    written.insert(written.begin(), notesWrittenOf(target).begin(), notesWrittenOf(target).end());
    out.written = SetNotesWritten{clip, std::move(written)};
  }
  return out;
}

void pruneCurves(NoteClip& c) {
  if (!c.curves) return;
  std::vector<NoteId> have;
  have.reserve(c.notes.size());
  for (const Note& n : c.notes) have.push_back(n.id);
  std::sort(have.begin(), have.end());
  auto has = [&](NoteId id) { return std::binary_search(have.begin(), have.end(), id); };
  // T7.11a: a bend whose neighbour is not in the clip goes too (NoteBend), and a record left empty with it.
  auto lost = [&](const NoteBend& b) { return b.set() && !has(b.other); };
  bool all = true;
  for (const NoteCurves& nc : *c.curves) all = all && has(nc.note) && !lost(nc.bendIn) && !lost(nc.bendOut);
  if (all) return;
  NoteCurveList out;
  out.reserve(c.curves->size());
  for (const NoteCurves& nc : *c.curves) {
    if (!has(nc.note)) continue;
    out.push_back(nc);
    NoteCurves& k = out.back();
    if (lost(k.bendIn)) k.bendIn = {};
    if (lost(k.bendOut)) k.bendOut = {};
    if (k.empty()) out.pop_back();
  }
  c.curves = out.empty() ? nullptr : std::make_shared<const NoteCurveList>(std::move(out));
}

void pruneJi(NoteClip& c) {
  if (!c.groups && !c.written) return;
  // (note id, the note it is anchored to), sorted by id: which notes there are and which are members.
  std::vector<std::pair<NoteId, NoteId>> have;
  have.reserve(c.notes.size());
  for (const Note& n : c.notes) have.emplace_back(n.id, noteAnchorOf(n.pitch));
  std::sort(have.begin(), have.end());
  auto find = [&](NoteId id) {
    const auto it = std::lower_bound(have.begin(), have.end(), std::pair<NoteId, NoteId>{id, NoteId{}});
    return it != have.end() && it->first == id ? &*it : nullptr;
  };
  if (c.groups && !std::ranges::all_of(*c.groups, [&](const JiGroup& g) { return find(g.anchor) != nullptr; })) {
    JiGroupList kept;
    for (const JiGroup& g : *c.groups)
      if (find(g.anchor)) kept.push_back(g);
    c.groups = kept.empty() ? nullptr : std::make_shared<const JiGroupList>(std::move(kept));
  }
  if (c.written) {
    // A written identity means something for a member or an anchor of a group only (T7.16a: an anchor's plays while
    // its group is Back to Tuning).
    auto member = [&](const NoteWritten& w) {
      const auto* e = find(w.note);
      return e != nullptr && ((e->second.valid() && findJiGroupByAnchor(c, e->second) != nullptr) || findJiGroupByAnchor(c, w.note) != nullptr);
    };
    if (!std::ranges::all_of(*c.written, member)) {
      NoteWrittenList kept;
      for (const NoteWritten& w : *c.written)
        if (member(w)) kept.push_back(w);
      c.written = kept.empty() ? nullptr : std::make_shared<const NoteWrittenList>(std::move(kept));
    }
  }
}

NoteClip cloneClip(const NoteClip& c, IdSource& ids, bool onlyPlaying) {
  std::vector<Note> notes;
  std::vector<NoteId> sources;  // sources[i]: the note notes[i] copies
  bool anchored = false;        // a copied note is anchored to a note
  auto take = [&](const Note& n) {
    Note m = n;
    m.id = ids.note();
    anchored = anchored || noteAnchorOf(n.pitch).valid();
    sources.push_back(n.id);
    notes.push_back(m);
  };
  if (onlyPlaying && !clipLooped(c)) {
    for (auto [it, end] = c.notes.range(c.contentStart, c.contentStart + c.length); it != end; ++it) take(*it);
  } else {
    notes.reserve(c.notes.size());
    sources.reserve(c.notes.size());
    for (const Note& n : c.notes) take(n);
  }
  // T7.11a (M7 review H2): whatever names a note is re-pointed to its copy (old → new note ids) — the identities
  // anchored to a note, the harmonic steps, the curves' bends, and (T7.13) the JI groups' anchors and the written
  // identities. The map is only built for a clip that has any of them.
  NoteIdMap noteMap;
  NoteCurveList curves;
  JiGroupList groups;
  NoteWrittenList written;
  ClipFundamental fundamental = c.fundamental;
  if (c.curves || c.groups || c.written || anchored || !fundamental.empty()) noteMap = noteIdMap(sources, notes);
  if (anchored)
    for (Note& m : notes)
      if (const NoteId to = remapNote(noteAnchorOf(m.pitch), noteMap); to.valid())
        m.pitch = tuning::PitchId::fromRatio(m.pitch.ratio(), tuning::AnchorKind::Note, to.value);
  if (!fundamental.steps.empty()) {
    for (HarmonicStep& st : fundamental.steps) st.note = remapNote(st.note, noteMap);
    std::erase_if(fundamental.steps, [](const HarmonicStep& st) { return !st.note.valid(); });
    std::sort(fundamental.steps.begin(), fundamental.steps.end(),
              [](const HarmonicStep& x, const HarmonicStep& y) { return x.note < y.note; });
  }
  if (c.curves) {
    for (size_t i = 0; i < notes.size(); ++i)  // fresh ids ascend: the list stays sorted
      if (const NoteCurves* nc = findNoteCurves(c, sources[i]))
        if (NoteCurves copy = remapNoteCurves(*nc, notes[i].id, noteMap); !copy.empty()) curves.push_back(std::move(copy));
  }
  for (const JiGroup& g : jiGroupsOf(c)) {  // sorted by id, and fresh ids ascend: the copy stays sorted
    const NoteId to = remapNote(g.anchor, noteMap);
    if (!to.valid()) continue;  // its anchor was not copied
    groups.push_back(g);
    groups.back().id = ids.jiGroup();
    groups.back().anchor = to;
  }
  for (const NoteWritten& w : notesWrittenOf(c))
    if (const NoteId to = remapNote(w.note, noteMap); to.valid()) written.push_back(NoteWritten{to, w.written});
  std::sort(written.begin(), written.end(), [](const NoteWritten& x, const NoteWritten& y) { return x.note < y.note; });
  NoteClip n = withNotes(c, seqOf(std::move(notes)));
  n.id = ids.clip();
  n.fundamental = std::move(fundamental);
  n.curves = curves.empty() ? nullptr : std::make_shared<const NoteCurveList>(std::move(curves));  // ids ascend
  n.groups = groups.empty() ? nullptr : std::make_shared<const JiGroupList>(std::move(groups));
  n.written = written.empty() ? nullptr : std::make_shared<const NoteWrittenList>(std::move(written));
  return n;
}

std::optional<tuning::Rational> exactReferenceRatio(const NoteClip& clip, const NotePitchIndex& hz, const Note* n) {
  tuning::Rational r{1};
  for (int link = 0; n != nullptr && link <= 8; ++link) {
    if (n->pitch.kind() != tuning::PitchKind::Ratio || movesPitch(findNoteCurves(clip, n->id)) || hz.warped(n->id) != nullptr)
      return std::nullopt;
    const std::optional<tuning::Rational> m = tuning::mul(r, n->pitch.ratio());
    if (!m || !m->isPositive()) return std::nullopt;
    r = *m;
    if (n->pitch.anchorKind() == tuning::AnchorKind::Reference) return r;
    n = clip.notes.findById(NoteId{n->pitch.anchorId()});
  }
  return std::nullopt;
}

std::optional<RootedRatio> rootedRatio(const NoteClip& clip, const NotePitchIndex& hz, const Note* n) {
  tuning::Rational r{1};
  for (int link = 0; n != nullptr && link <= 8; ++link) {
    if (movesPitch(findNoteCurves(clip, n->id)) || hz.warped(n->id) != nullptr) return std::nullopt;
    if (!noteAnchorOf(n->pitch).valid()) return RootedRatio{n->id, r};
    const std::optional<tuning::Rational> m = tuning::mul(r, n->pitch.ratio());
    if (!m || !m->isPositive()) return std::nullopt;
    r = *m;
    n = clip.notes.findById(noteAnchorOf(n->pitch));
  }
  return std::nullopt;
}

std::vector<JiWarp> shiftedWarps(std::span<const JiWarp> warps, Ticks by) {
  std::vector<JiWarp> out(warps.begin(), warps.end());
  if (by == 0) return out;
  for (JiWarp& w : out) w.at = std::clamp<Ticks>(w.at - by, -kMaxTicks, kMaxTicks);
  // (Clamped only at the ends of the timeline, where two warps could meet: keep the order valid.)
  for (size_t i = 1; i < out.size(); ++i)
    if (out[i].at < out[i - 1].at + std::max<Ticks>(out[i - 1].glide, 1)) out[i].at = std::min<Ticks>(out[i - 1].at + std::max<Ticks>(out[i - 1].glide, 1), kMaxTicks);
  return out;
}

// ---- Notes that lose their anchor (T7.11a, M7 review H3; T7.13 groups) -----------------------------------------------

Reanchored reanchorWithout(const Project& p, const TrackPath& path, const NoteClip& clip, std::span<const NoteId> gone) {
  Reanchored out;
  if (gone.empty()) return out;
  auto isGone = [&](NoteId id) { return std::binary_search(gone.begin(), gone.end(), id); };
  // The cheap part, for every removal: is any remaining note anchored to a leaving one, does the fundamental name one?
  std::vector<const Note*> lost;
  for (const Note& n : clip.notes)
    if (const NoteId a = noteAnchorOf(n.pitch); a.valid() && isGone(a) && !isGone(n.id)) lost.push_back(&n);
  const ClipFundamental& cf = clip.fundamental;
  bool fundamental = false;
  for (const HarmonicStep& st : cf.steps) fundamental = fundamental || isGone(st.note);
  if (lost.empty() && !fundamental) return out;

  ClipFundamental nf = cf;
  std::erase_if(nf.steps, [&](const HarmonicStep& st) { return isGone(st.note); });
  if (!lost.empty()) {
    const ClipPitches cp = resolveClipPitches(p, path, clip);
    const NotePitchIndex hz(cp);
    Result<tuning::Tuning> tun = cp.tuning.def ? buildTuning(*cp.tuning.def) : Result<tuning::Tuning>(notFound("no tuning"));
    std::vector<std::pair<NoteId, const Note*>> byId;  // the clip's notes by id (a NoteSeq is ordered by start)
    byId.reserve(clip.notes.size());
    for (const Note& n : clip.notes) byId.emplace_back(n.id, &n);
    std::sort(byId.begin(), byId.end());
    auto noteOf = [&](NoteId id) -> const Note* {
      const auto it = std::lower_bound(byId.begin(), byId.end(), std::pair<NoteId, const Note*>{id, nullptr});
      return it != byId.end() && it->first == id ? it->second : nullptr;
    };
    // A note's pitch as an exact ratio to the reference: a chain of note anchors that ends on a reference-anchored
    // Ratio, none of its notes with a curve record or a warp (they move what is anchored to them: pitch_resolve.h).
    auto exactRatio = [&](const Note* n) -> std::optional<tuning::Rational> {
      tuning::Rational r{1};
      for (int link = 0; n != nullptr && link <= 8; ++link) {
        if (n->pitch.kind() != tuning::PitchKind::Ratio || movesPitch(findNoteCurves(clip, n->id)) || hz.warped(n->id) != nullptr)
          return std::nullopt;
        const std::optional<tuning::Rational> m = tuning::mul(r, n->pitch.ratio());
        if (!m || !m->isPositive()) return std::nullopt;
        r = *m;
        if (n->pitch.anchorKind() == tuning::AnchorKind::Reference) return r;
        if (n->pitch.anchorKind() != tuning::AnchorKind::Note) return std::nullopt;
        n = noteOf(NoteId{n->pitch.anchorId()});
      }
      return std::nullopt;
    };
    // Cents at `v` Hz.
    auto centsAt = [&](std::optional<double> v) -> std::optional<tuning::PitchId> {
      if (!v || !tun) return std::nullopt;  // it did not sound before either: left as it is
      const double c = 1200.0 * std::log2(*v / tun->referenceHz());
      return std::isfinite(c) ? std::optional<tuning::PitchId>(tuning::PitchId::fromCents(c)) : std::nullopt;
    };
    // A member's JI pitch (a warped member's start pitch may be its written one).
    auto jiHz = [&](NoteId id) -> std::optional<double> {
      if (const NoteWarpState* w = hz.warped(id)) return w->jiHz;
      return hz.hz(id);
    };
    std::vector<NoteId> unwritten;  // notes whose written identity no longer applies
    // `anchors`: the note becomes its group's anchor (it keeps its written identity: T7.16a).
    auto set = [&](const Note& n, const tuning::PitchId& pitch, bool anchors) {
      Note m = n;
      m.pitch = pitch;
      out.updates.push_back(m);
      if (!anchors && !noteAnchorOf(pitch).valid() && findNoteWritten(clip, n.id) != nullptr) unwritten.push_back(n.id);
    };
    JiGroupList groups(jiGroupsOf(clip).begin(), jiGroupsOf(clip).end());
    bool groupsChanged = false;
    std::stable_sort(lost.begin(), lost.end(), [](const Note* x, const Note* y) { return x->pitch.anchorId() < y->pitch.anchorId(); });
    for (size_t i = 0; i < lost.size();) {
      size_t j = i;
      while (j < lost.size() && lost[j]->pitch.anchorId() == lost[i]->pitch.anchorId()) ++j;
      const NoteId anchorId{lost[i]->pitch.anchorId()};
      const Note* anchor = noteOf(anchorId);
      const auto git = std::find_if(groups.begin(), groups.end(), [&](const JiGroup& g) { return g.anchor == anchorId; });
      JiGroup* group = git != groups.end() ? &*git : nullptr;
      const std::optional<tuning::Rational> exact = exactRatio(anchor);
      // The anchor sounds at one pitch for all of them: no curve record, no warp of its own.
      const bool constant = anchor != nullptr && !movesPitch(findNoteCurves(clip, anchorId)) && hz.warped(anchorId) == nullptr;
      // The LEADER takes the anchor's place: the lowest remaining dependant without a curve of its own. It carries the
      // pitch (the exact reference Ratio when the anchor's pitch is one, else Cents) and the others keep their exact
      // ratios to it. For a JI group: always (T7.13: the group lives on, the leader its anchor). Without a group: only
      // under a tempered anchor with two or more dependants (an exact anchor makes them all exact reference Ratios).
      const Note* leader = nullptr;
      if ((exact || constant) && (group != nullptr || (!exact && j - i > 1)))
        for (size_t k = i; k < j; ++k) {
          const Note& n = *lost[k];
          if (movesPitch(findNoteCurves(clip, n.id)) || (!exact && !(jiHz(n.id) && tun))) continue;
          if (leader == nullptr || n.pitch.ratio() < leader->pitch.ratio()) leader = &n;
        }
      for (size_t k = i; k < j; ++k) {
        const Note& n = *lost[k];
        std::optional<tuning::PitchId> pitch;
        if (leader != nullptr && &n != leader) {
          if (const std::optional<tuning::Rational> r = tuning::div(n.pitch.ratio(), leader->pitch.ratio()); r && r->isPositive())
            pitch = tuning::PitchId::fromRatio(*r, tuning::AnchorKind::Note, leader->id.value);
        } else if (exact) {
          if (const std::optional<tuning::Rational> r = tuning::mul(*exact, n.pitch.ratio()); r && r->isPositive())
            pitch = tuning::PitchId::fromRatio(*r);
        } else if (&n == leader) {
          pitch = centsAt(jiHz(n.id));
        }
        if (!pitch) pitch = centsAt(hz.hz(n.id));
        if (pitch) set(n, *pitch, group != nullptr && &n == leader);
      }
      if (group != nullptr && leader != nullptr) {
        // The group's numbering stays: the leader keeps the number it had (the anchor's × its ratio); reduced by what
        // all the remaining numbers share when that is beyond the limit; 1 when it had none.
        int64_t index = 1;
        if (const std::optional<GroupHarmonics> gh = groupHarmonics(clip, *group)) {
          int64_t own = 0, shared = 0;
          for (const GroupMember& m : gh->members) {
            if (isGone(m.note) || m.harmonic < 1) continue;
            shared = std::gcd(shared, m.harmonic);
            if (m.note == leader->id) own = m.harmonic;
          }
          if (own > static_cast<int64_t>(kMaxHarmonicIndex) && shared > 0) own /= shared;
          if (own >= 1 && own <= static_cast<int64_t>(kMaxHarmonicIndex)) index = own;
        }
        // The warps stay where they are in the clip: group ticks count from the anchor's start (M7 review M7).
        if (!group->warps.empty() && anchor != nullptr) group->warps = shiftedWarps(group->warps, leader->start - anchor->start);
        group->anchor = leader->id;
        group->index = static_cast<uint32_t>(index);
        groupsChanged = true;
        // (The leader keeps its written identity: an anchor does not warp, but Back to Tuning plays it as written.)
      }
      i = j;
    }
    if (groupsChanged) {
      // A group that found no leader goes with its anchor (RemoveNotes / pruneJi drop it).
      out.groups = std::move(groups);
    }
    if (!unwritten.empty()) {
      std::sort(unwritten.begin(), unwritten.end());
      NoteWrittenList kept;
      for (const NoteWritten& w : notesWrittenOf(clip))
        if (!std::binary_search(unwritten.begin(), unwritten.end(), w.note)) kept.push_back(w);
      out.written = std::move(kept);
    }
  }
  if (!(nf == cf)) out.fundamental = std::move(nf);
  return out;
}

void dropRepitchedWritten(Batch& b, const ClipRef& clip, const NoteClip& before) {
  if (!before.written) return;
  std::vector<NoteId> gone;
  for (const Command& c : b.commands)
    if (const UpdateNotes* u = std::get_if<UpdateNotes>(&c.v))
      for (const NoteUpdate& up : u->updates) {
        if (findNoteWritten(before, up.note.id) == nullptr) continue;
        const Note* was = before.notes.locate(up.key);
        if (was != nullptr && !(was->pitch == up.note.pitch)) gone.push_back(up.note.id);
      }
  if (gone.empty()) return;
  std::sort(gone.begin(), gone.end());
  NoteWrittenList kept;
  for (const NoteWritten& w : *before.written)
    if (!std::binary_search(gone.begin(), gone.end(), w.note)) kept.push_back(w);
  b.commands.push_back(SetNotesWritten{clip, std::move(kept)});
}

void applyReanchored(NoteClip& c, Reanchored r) {
  if (!r.updates.empty()) {
    std::sort(r.updates.begin(), r.updates.end(), [](const Note& x, const Note& y) { return x.id < y.id; });
    std::vector<Note> notes(c.notes.begin(), c.notes.end());
    for (Note& n : notes) {
      const auto it = std::lower_bound(r.updates.begin(), r.updates.end(), n.id, [](const Note& x, NoteId id) { return x.id < id; });
      if (it != r.updates.end() && it->id == n.id) n.pitch = it->pitch;
    }
    c.notes = seqOf(std::move(notes));
  }
  if (r.fundamental) c.fundamental = std::move(*r.fundamental);
  if (r.groups) c.groups = r.groups->empty() ? nullptr : std::make_shared<const JiGroupList>(std::move(*r.groups));
  if (r.written) c.written = r.written->empty() ? nullptr : std::make_shared<const NoteWrittenList>(std::move(*r.written));
}

void appendReanchored(Batch& b, const ClipRef& clip, Reanchored r) {
  if (!r.updates.empty()) {
    UpdateNotes u{clip, {}};
    u.updates.reserve(r.updates.size());
    for (const Note& n : r.updates) u.updates.push_back({keyOf(n), n});
    b.commands.push_back(std::move(u));
  }
  if (r.fundamental) b.commands.push_back(SetClipFundamental{clip, std::move(*r.fundamental)});
  if (r.groups) b.commands.push_back(SetJiGroups{clip, std::move(*r.groups)});
  if (r.written) b.commands.push_back(SetNotesWritten{clip, std::move(*r.written)});
}

// ---- ClipEditor ----------------------------------------------------------------------------------------------------

ClipEditor::ClipEditor(const Project& p, const Track& t) : p_(&p), track_(&t) {
  if (const auto path = findTrack(p, t.id)) path_ = *path;
  clips_ = t.clips;
  clips_.reserve(t.clips.size() + 4);
}

const ClipSlot* ClipEditor::find(ClipId id) const {
  for (const ClipSlot& c : clips_)
    if (c.id == id) return &c;
  return nullptr;
}

void ClipEditor::remove(ClipId id) {
  std::erase_if(clips_, [&](const ClipSlot& c) { return c.id == id; });
}

void ClipEditor::replace(ClipId id, ClipSlot c) {
  for (ClipSlot& x : clips_)
    if (x.id == id) {
      x = std::move(c);
      return;
    }
}

double ClipEditor::rateOf(const AudioClip& c) const noexcept {
  const SampleRef* r = findSample(*p_, c.sample);
  return r != nullptr ? r->rate : 48000.0;
}

double ClipEditor::ratioOf(const AudioClip& c) const { return audioRateRatio(*p_, path_, c); }

ClipSlot ClipEditor::bounded(const ClipSlot& c, Ticks s, Ticks e) const {
  if (c.audio) {
    auto a = std::make_shared<const AudioClip>(
        audioWithBounds(*p_->timeline, *c.audio, s, e, rateOf(*c.audio), ratioOf(*c.audio)));
    return ClipSlot{c.id, nullptr, std::move(a), c.take};  // T6.1: a piece keeps its provenance
  }
  return ClipSlot{c.id, std::make_shared<const NoteClip>(withBounds(*c.clip, s, e)), nullptr, c.take};
}

std::pair<ClipSlot, ClipSlot> ClipEditor::split(const ClipSlot& c, Ticks t, ClipId rightId, IdSource& ids) const {
  if (c.audio) {
    auto [l, r] = splitAudioAt(*p_->timeline, *c.audio, t, rightId, rateOf(*c.audio), ratioOf(*c.audio));
    return {ClipSlot{l.id, nullptr, std::make_shared<const AudioClip>(std::move(l)), c.take},
            ClipSlot{r.id, nullptr, std::make_shared<const AudioClip>(std::move(r)), c.take}};
  }
  auto [l, r] = splitClipAt(*c.clip, t, rightId, ids, *p_, path_);
  return {ClipSlot{l.id, std::make_shared<const NoteClip>(std::move(l)), nullptr, c.take},
          ClipSlot{r.id, std::make_shared<const NoteClip>(std::move(r)), nullptr, c.take}};
}

ClipSlot ClipEditor::clonePlaying(const ClipSlot& c, IdSource& ids) const {
  if (c.audio || clipLooped(*c.clip)) return cloneSlot(c, ids, true);
  // T7.11a (H3): the hidden notes are dropped; a playing note anchored to one keeps its pitch.
  const NoteClip& src = *c.clip;
  std::vector<Note> playing;
  for (auto [it, end] = src.notes.range(src.contentStart, src.contentStart + src.length); it != end; ++it) playing.push_back(*it);
  if (playing.size() == src.notes.size()) return cloneSlot(c, ids, false);
  std::vector<NoteId> kept, gone;
  for (const Note& n : playing) kept.push_back(n.id);
  std::sort(kept.begin(), kept.end());
  for (const Note& n : src.notes)
    if (!std::binary_search(kept.begin(), kept.end(), n.id)) gone.push_back(n.id);
  std::sort(gone.begin(), gone.end());
  NoteClip visible = withNotes(src, seqOf(std::move(playing)));
  applyReanchored(visible, reanchorWithout(*p_, path_, src, gone));
  pruneCurves(visible);
  pruneJi(visible);
  auto n = std::make_shared<const NoteClip>(cloneClip(visible, ids, false));
  return ClipSlot{n->id, std::move(n), nullptr};
}

ClipSlot ClipEditor::moved(const ClipSlot& c, Ticks start) const {
  if (c.audio)
    return ClipSlot{c.id, nullptr, std::make_shared<const AudioClip>(audioMovedTo(*p_->timeline, *c.audio, start)), c.take};
  NoteClip n = withNotes(*c.clip, c.clip->notes);
  n.start = start;
  return ClipSlot{c.id, std::make_shared<const NoteClip>(std::move(n)), nullptr, c.take};
}

void ClipEditor::clearRange(Ticks a, Ticks b, IdSource& ids, std::span<const ClipId> keep) {
  if (b <= a) return;
  std::vector<ClipSlot> out;
  out.reserve(clips_.size() + 1);
  for (ClipSlot& c : clips_) {
    const Ticks s = c.start();
    const Ticks e = c.end();
    if (e <= a || s >= b || std::find(keep.begin(), keep.end(), c.id) != keep.end()) {
      out.push_back(std::move(c));
    } else if (a <= s && e <= b) {
      // covered: dropped
    } else if (s < a && e <= b) {
      out.push_back(bounded(c, s, a));
    } else if (a <= s) {
      out.push_back(bounded(c, b, e));
    } else {  // s < a, b < e: keep both ends
      auto [l, r] = split(c, b, ids.clip(), ids);
      out.push_back(bounded(l, s, a));
      out.push_back(std::move(r));
    }
  }
  clips_ = std::move(out);
}

void ClipEditor::emit(Batch& removes, Batch& updates, Batch& adds) const {
  struct E {
    ClipId id;
    const ClipSlot* slot;
  };
  auto byId = [](const E& x, const E& y) { return x.id < y.id; };
  std::vector<E> before;
  before.reserve(track_->clips.size());
  for (const ClipSlot& s : track_->clips) before.push_back({s.id, &s});
  std::vector<E> after;
  after.reserve(clips_.size());
  for (const ClipSlot& c : clips_) after.push_back({c.id, &c});
  std::sort(before.begin(), before.end(), byId);
  std::sort(after.begin(), after.end(), byId);
  const TrackId tid = track_->id;
  size_t i = 0, j = 0;
  while (i < before.size() || j < after.size()) {
    if (j == after.size() || (i < before.size() && before[i].id < after[j].id)) {
      removes.commands.push_back(RemoveClip{{tid, before[i].id}});
      ++i;
    } else if (i == before.size() || after[j].id < before[i].id) {
      adds.commands.push_back(AddClip{tid, after[j].slot->clip, after[j].slot->audio, after[j].slot->take});
      ++j;
    } else if (after[j].slot->audio) {  // T5.3: an audio clip is replaced whole
      if (before[i].slot->audio != after[j].slot->audio && !(*before[i].slot->audio == *after[j].slot->audio))
        updates.commands.push_back(SetAudioClip{{tid, after[j].id}, after[j].slot->audio});
      ++i;
      ++j;
    } else {
      const NoteClip& o = *before[i].slot->clip;
      const NoteClip& n = *after[j].slot->clip;
      if (before[i].slot->clip.get() != after[j].slot->clip.get()) {
        const bool boundsOnly = o.name == n.name && o.colour == n.colour && o.loop == n.loop &&
                                o.signature == n.signature && o.tuning == n.tuning && o.active == n.active &&
                                sameNotes(o.notes, n.notes) && o.groups == n.groups && o.written == n.written &&
                                o.curves == n.curves;
        if (boundsOnly) {
          if (o.start != n.start || o.length != n.length || o.contentStart != n.contentStart)
            updates.commands.push_back(SetClipBounds{{tid, n.id}, n.start, n.length, n.contentStart});
        } else {
          removes.commands.push_back(RemoveClip{{tid, o.id}});
          adds.commands.push_back(AddClip{tid, after[j].slot->clip, nullptr, after[j].slot->take});
        }
      }
      ++i;
      ++j;
    }
  }
}

Result<ClipEditor*> ClipEdits::editor(TrackId t) {
  for (auto& e : eds_)
    if (e->id() == t) return e.get();
  Result<const Track*> tr = findClipTrack(p_, t);
  if (!tr) return std::move(tr).error();
  eds_.push_back(std::make_unique<ClipEditor>(p_, **tr));
  return eds_.back().get();
}

ClipEditor& ClipEdits::editor(const Track& t) {
  for (auto& e : eds_)
    if (e->id() == t.id) return *e;
  eds_.push_back(std::make_unique<ClipEditor>(p_, t));
  return *eds_.back();
}

void ClipEdits::emit(Batch& out) const {
  Batch removes, updates, adds;
  for (const auto& e : eds_) e->emit(removes, updates, adds);
  for (Batch* b : {&removes, &updates, &adds})
    for (Command& c : b->commands) out.commands.push_back(std::move(c));
}

// ---- Automation envelopes ----------------------------------------------------------------------------------------

namespace {
size_t firstAtOrAfter(std::span<const Breakpoint> pts, Ticks t) {
  return static_cast<size_t>(
      std::lower_bound(pts.begin(), pts.end(), t, [](const Breakpoint& b, Ticks v) { return b.tick < v; }) -
      pts.begin());
}
float segX(const Breakpoint& a, const Breakpoint& b, Ticks t) {
  return static_cast<float>(static_cast<double>(t - a.tick) / static_cast<double>(b.tick - a.tick));
}
}  // namespace

Points envBefore(std::span<const Breakpoint> pts, Ticks t, IdSource& ids, bool force) {
  const size_t n = pts.size();
  const size_t i = firstAtOrAfter(pts, t);
  Points out(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(i));
  if (n == 0 || (!force && (i == n || i == 0))) return out;
  double v = 0.0;
  if (i == n) {
    v = pts[n - 1].value;  // after the last point: the held last value
  } else if (pts[i].tick == t || i == 0) {
    v = pts[i].value;
  } else {
    const Breakpoint& a = pts[i - 1];
    v = automationSegmentValue(a, pts[i], static_cast<double>(t));
    out.back().curve = a.curve * segX(a, pts[i], t);
  }
  out.push_back(Breakpoint{t, static_cast<float>(v), 0.0f, ids.breakpoint()});
  return out;
}

Points envFrom(std::span<const Breakpoint> pts, Ticks t, IdSource& ids) {
  const size_t n = pts.size();
  const size_t i = firstAtOrAfter(pts, t);
  Points out;
  out.reserve(n - i + 1);
  if (i > 0 && (i == n || pts[i].tick > t)) {
    double v = pts[n - 1].value;
    float c = 0.0f;
    if (i < n) {
      const Breakpoint& a = pts[i - 1];
      v = automationSegmentValue(a, pts[i], static_cast<double>(t));
      c = a.curve * (1.0f - segX(a, pts[i], t));
    }
    out.push_back(Breakpoint{t, static_cast<float>(v), c, ids.breakpoint()});
  } else if (i == 0 && n > 0 && pts[0].tick > t) {
    // The constant run before the first point: start it at t too, so a slice is self-contained.
    out.push_back(Breakpoint{t, pts[0].value, 0.0f, ids.breakpoint()});
  }
  out.insert(out.end(), pts.begin() + static_cast<std::ptrdiff_t>(i), pts.end());
  return out;
}

Points envSlice(std::span<const Breakpoint> pts, Ticks a, Ticks b, IdSource& ids, bool freshIds) {
  if (pts.empty()) return {};
  const Points left = envBefore(pts, b, ids, true);
  Points out = envFrom(left, a, ids);
  if (freshIds)
    for (Breakpoint& p : out) p.id = ids.breakpoint();
  return out;
}

void shiftPoints(Points& pts, Ticks dt) {
  for (Breakpoint& p : pts) p.tick += dt;
}

void append(Points& dst, const Points& src) { dst.insert(dst.end(), src.begin(), src.end()); }

void simplify(Points& pts, uint32_t firstNew, IdSource& ids, uint32_t keepLo, uint32_t keepHi) {
  // Construction order is the envelope order; only ticks need to be ordered (stable: same-tick points keep it).
  std::stable_sort(pts.begin(), pts.end(), [](const Breakpoint& a, const Breakpoint& b) { return a.tick < b.tick; });
  auto isNew = [&](const Breakpoint& b) {
    return b.id.value >= firstNew && !(b.id.value >= keepLo && b.id.value < keepHi);
  };
  bool changed = true;
  while (changed) {
    changed = false;
    // Same tick, same value: one point is enough. It carries the later point's outgoing curve and, if either is
    // an existing point, that point's id.
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
      Breakpoint& p = pts[i];
      Breakpoint& q = pts[i + 1];
      if (p.tick == q.tick && p.value == q.value && (isNew(p) || isNew(q))) {
        if (isNew(q)) {
          p.curve = q.curve;
          pts.erase(pts.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        } else {
          pts.erase(pts.begin() + static_cast<std::ptrdiff_t>(i));
        }
        changed = true;
        break;
      }
    }
    if (changed) continue;
    const size_t n = pts.size();
    for (size_t i = 0; i < n && n >= 2; ++i) {
      const Breakpoint& p = pts[i];
      if (!isNew(p)) continue;
      const bool prevEq = i == 0 || pts[i - 1].value == p.value;
      const bool nextEq = i + 1 == n || pts[i + 1].value == p.value;
      // A created point inside a constant run (or at a constant end) changes nothing.
      if (prevEq && nextEq && !(i > 0 && pts[i - 1].tick == p.tick) && !(i + 1 < n && pts[i + 1].tick == p.tick)) {
        pts.erase(pts.begin() + static_cast<std::ptrdiff_t>(i));
        changed = true;
        break;
      }
    }
  }
  // Points on one tick are ordered by id in a lane: where construction order disagrees, the later points of that
  // tick get fresh (larger) ids.
  for (size_t i = 1; i < pts.size(); ++i) {
    if (pts[i].tick != pts[i - 1].tick || pts[i - 1].id < pts[i].id) continue;
    for (size_t k = i; k < pts.size() && pts[k].tick == pts[i - 1].tick; ++k) pts[k].id = ids.breakpoint();
  }
}

void emitLaneDiff(Batch& out, TrackId track, const AutomationLane& lane, Points pts) {
  std::sort(pts.begin(), pts.end(), pointLess);
  if (pts == lane.points) return;
  auto byId = [](const Breakpoint& a, const Breakpoint& b) { return a.id < b.id; };
  Points before = lane.points;
  std::sort(before.begin(), before.end(), byId);
  std::sort(pts.begin(), pts.end(), byId);
  RemoveBreakpoints rem{track, lane.id, {}};
  UpdateBreakpoints upd{track, lane.id, {}};
  AddBreakpoints add{track, lane.id, {}};
  size_t i = 0, j = 0;
  while (i < before.size() || j < pts.size()) {
    if (j == pts.size() || (i < before.size() && before[i].id < pts[j].id)) {
      rem.points.push_back(before[i++].id);
    } else if (i == before.size() || pts[j].id < before[i].id) {
      add.points.push_back(pts[j++]);
    } else {
      if (!(before[i] == pts[j])) upd.points.push_back(pts[j]);
      ++i;
      ++j;
    }
  }
  if (!rem.points.empty()) out.commands.push_back(std::move(rem));
  if (!upd.points.empty()) out.commands.push_back(std::move(upd));
  if (!add.points.empty()) out.commands.push_back(std::move(add));
}

}  // namespace detail
}  // namespace ops
}  // namespace pq::doc
