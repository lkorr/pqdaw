// AP.7: Flatten to clip (pq/document/note_flatten.h).
#include "pq/document/note_flatten.h"

#include <algorithm>
#include <string>
#include <unordered_set>

#include "edit_common.h"
#include "pq/document/note_devices.h"

namespace pq::doc::ops {

using namespace detail;

namespace {
// A clip that plays its loop more than once: its generated notes would not repeat with it (a loop that only spans the
// clip, the default, plays once: fine).
bool repeatsLoop(const NoteClip& c) noexcept {
  return c.loop.enabled && c.loop.end > c.loop.start && c.contentStart + c.length > c.loop.end;
}
}  // namespace

std::string flattenRefusal(const Project& p, TrackId track) {
  Result<const Track*> tr = findMidiTrack(p, track);
  if (!tr) return tr.error().message;
  // AP.8b (review M6): a take lane's notes would lose the devices (switched off for the whole track) and no note edit
  // reaches them: refused rather than changing how they sound.
  for (const TakeLanePtr& l : (*tr)->takes)
    for (const ClipSlot& s : l->clips)
      if (s.clip && !s.clip->notes.empty())
        return "take lane \"" + l->name + "\" has notes the note devices play (comp or delete the takes first)";
  for (const ClipSlot& s : (*tr)->clips)
    if (s.clip && repeatsLoop(*s.clip))
      return "clip \"" + s.clip->name + "\" repeats its loop (shorten the clip or switch the loop off first)";
  return {};
}

EditResult flattenNoteStages(const Project& p, IdSource& ids, TrackId track, std::span<const ClipGenerated> clips) {
  Result<const Track*> tr = findMidiTrack(p, track);
  if (!tr) return std::move(tr).error();
  const Track* t = *tr;
  if (std::string why = flattenRefusal(p, track); !why.empty()) return invalid("Flatten: " + why);
  Edit e = makeEdit("Flatten to Clip");
  Batch& b = batchOf(e);
  for (const ClipGenerated& cg : clips) {
    const ClipRef ref{track, cg.clip};
    Result<ClipFound> f = findClip(p, ref);
    if (!f) return std::move(f).error();
    const NoteClip& c = *f->clip;
    // A clip that plays its loop more than once: the generated notes do not repeat with it (a loop that only spans the
    // clip, the default, plays once: fine).
    if (repeatsLoop(c))
      return invalid("Flatten: clip \"" + c.name + "\" repeats its loop (shorten the clip or switch the loop off first)");
    // The anchors the exact ratios need stay, deactivated; every other source note goes — AP.8b (review M5): only the
    // notes that play (start within the clip's window: sequence.h "Clip semantics") or generated something. A note
    // outside the window never plays, with or without the devices: it stays as it is.
    std::unordered_set<uint32_t> anchors, sources;
    for (const GeneratedNote& g : cg.notes) {
      sources.insert(g.source.value);
      if (g.pitch.kind() == tuning::PitchKind::Ratio && g.pitch.anchorKind() == tuning::AnchorKind::Note)
        anchors.insert(g.pitch.anchorId());
    }
    const Ticks windowEnd = c.contentStart + std::max<Ticks>(c.length, 1);
    std::vector<NoteKey> gone, kept;
    for (const Note& n : c.notes) {
      if (!n.enabled()) continue;  // never played: nothing to replace
      const bool plays = n.start >= c.contentStart && n.start < windowEnd;
      if (!plays && sources.count(n.id.value) == 0 && anchors.count(n.id.value) == 0) continue;
      (anchors.count(n.id.value) != 0 ? kept : gone).push_back(keyOf(n));
    }
    if (!gone.empty()) b.commands.push_back(RemoveNotes{ref, std::move(gone)});
    if (!kept.empty()) b.commands.push_back(SetNoteProperty{ref, std::move(kept), NoteProperty::Enabled, 0.0f});
    std::vector<Note> add;
    std::vector<NoteCurves> curves;
    add.reserve(cg.notes.size());
    for (const GeneratedNote& g : cg.notes) {
      Note n;
      n.start = g.start;
      n.length = std::max<Ticks>(g.length, 1);
      n.pitch = g.pitch;
      n.id = ids.note();
      n.velocity = std::clamp(g.velocity, 1.0f, 127.0f);
      add.push_back(n);
      e.notes.push_back(keyOf(n));
      if (!g.curve.empty()) {
        NoteCurves nc;
        nc.note = n.id;
        nc.pitch = g.curve;
        curves.push_back(std::move(nc));
      }
    }
    if (!add.empty()) b.commands.push_back(AddNotes{ref, std::move(add)});
    if (!curves.empty()) {
      std::sort(curves.begin(), curves.end(), [](const NoteCurves& x, const NoteCurves& y) { return x.note.value < y.note.value; });
      b.commands.push_back(SetNoteCurves{ref, std::move(curves)});
    }
    e.clips.push_back(ref);
    e.noteClip = ref;
  }
  // The track's note devices off (deactivated, not deleted): the clips now play what they played.
  for (const DevicePtr& d : t->devices)
    if (d->active && isNoteDevice(*d)) {
      b.commands.push_back(SetDeviceActive{d->id, false});
      e.devices.push_back(d->id);
    }
  return e;
}

}  // namespace pq::doc::ops
