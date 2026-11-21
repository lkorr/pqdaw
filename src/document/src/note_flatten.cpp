// AP.7: Flatten to clip (pq/document/note_flatten.h).
#include "pq/document/note_flatten.h"

#include <algorithm>
#include <unordered_set>

#include "edit_common.h"
#include "pq/document/note_devices.h"

namespace pq::doc::ops {

using namespace detail;

EditResult flattenNoteStages(const Project& p, IdSource& ids, TrackId track, std::span<const ClipGenerated> clips) {
  Result<const Track*> tr = findMidiTrack(p, track);
  if (!tr) return std::move(tr).error();
  const Track* t = *tr;
  Edit e = makeEdit("Flatten to Clip");
  Batch& b = batchOf(e);
  for (const ClipGenerated& cg : clips) {
    const ClipRef ref{track, cg.clip};
    Result<ClipFound> f = findClip(p, ref);
    if (!f) return std::move(f).error();
    const NoteClip& c = *f->clip;
    if (c.loop.enabled && c.loop.end > c.loop.start)
      return invalid("Flatten: clip \"" + c.name + "\" loops (switch its loop off first)");
    // The anchors the exact ratios need stay, deactivated; every other playing source note goes.
    std::unordered_set<uint32_t> anchors;
    for (const GeneratedNote& g : cg.notes)
      if (g.pitch.kind() == tuning::PitchKind::Ratio && g.pitch.anchorKind() == tuning::AnchorKind::Note)
        anchors.insert(g.pitch.anchorId());
    std::vector<NoteKey> gone, kept;
    for (const Note& n : c.notes) {
      if (!n.enabled()) continue;  // never played: nothing to replace
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
