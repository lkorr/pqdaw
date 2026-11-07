// T5.11: edits that bring user-library items into the project (pq/document/edit_ops.h "User library").
#include <algorithm>

#include "edit_common.h"

namespace pq::doc::ops {

using namespace detail;

EditResult replaceDevice(const Project& p, IdSource& ids, DeviceId device, const Device& with,
                         std::span<const std::string> params) {
  const DeviceLoc loc = findDevice(p, device);
  if (!loc.track) return notFound("device not found");
  if (with.kind != loc.device->kind) return invalid("a preset of another device kind cannot replace this device");
  auto keepsParam = [&](std::string_view name) {
    return std::find(params.begin(), params.end(), name) != params.end();
  };
  (void)ids;  // (nothing inside a device needs fresh ids)
  // The new device: the replaced device's id (its state key and wires follow it).
  auto top = std::make_shared<Device>(with);
  top->id = device;
  top->group = loc.device->group;  // TX.groups: a member stays in its group
  Edit e = makeEdit("Hot-Swap Preset");
  e.devices.push_back(device);
  Batch& b = batchOf(e);
  b.commands.push_back(RemoveDevice{device});
  b.commands.push_back(AddDevice{loc.track->id, std::move(top), loc.index});
  // Wires into a param the new device still has, and every wire out of it (its exports), come back with their ids;
  // TX.all-free: so do the wires into its main input (its place on the path stays).
  for (const Wire& w : wiresOf(p)) {
    if (w.from != device && w.to != device) continue;
    if (w.to == device && !w.param.empty() && !keepsParam(w.param)) continue;
    if (w.from != device && !w.fromNoteSource() && !findDevice(p, w.from).track) continue;  // TMM.10-fix2: notes stay
    b.commands.push_back(AddWire{w});
  }
  // T4.18: its automation lanes stay on the params the new device still has (the device keeps its id: apply keeps
  // those lanes where they are); the lanes of the others go.
  for (const LanePtr& l : loc.track->lanes)
    if (l->param.kind == ParamKind::DeviceParam && l->param.owner == device.value && !keepsParam(l->paramName))
      b.commands.push_back(RemoveAutomationLane{loc.track->id, l->id});
  return e;
}

EditResult placeClip(const Project& p, IdSource& ids, TrackId track, const ClipSlot& clip, Ticks start,
                     const std::optional<SampleRef>& newSample) {
  PQ_DOC_TRY(checkTick(start, "clip start"));
  if (!clip.clip && !clip.audio) return invalid("no clip");
  ClipEdits ed(p);
  Result<ClipEditor*> te = ed.editor(track);
  if (!te) return std::move(te).error();
  ClipEditor& c = **te;
  if (clip.audio && c.track().kind != TrackKind::Audio) return invalid("audio clips live on audio tracks");
  if (clip.clip && c.track().kind != TrackKind::Midi) return invalid("note clips live on MIDI tracks");
  Edit e = makeEdit("Add Clip");
  ClipSlot slot = clip;
  if (clip.audio) {
    AudioClip a = *clip.audio;
    if (newSample) {
      PQ_DOC_TRY(checkSampleRef(*newSample));
      if (findSample(p, newSample->id)) return invalid("sample id already in use");
      batchOf(e).commands.push_back(AddSample{*newSample});
      a.sample = newSample->id;
    } else if (!findSample(p, a.sample)) {
      return notFound("unknown sample id " + std::to_string(a.sample.value));
    }
    a.crossfadeIn = 0;  // no neighbour yet
    slot = ClipSlot{a.id, nullptr, std::make_shared<const AudioClip>(std::move(a))};
  }
  slot = c.moved(cloneSlot(slot, ids, false), start);
  PQ_DOC_TRY(checkTick(slot.end(), "clip end"));
  c.clearRange(slot.start(), slot.end(), ids);
  e.clips.push_back({track, slot.id});
  c.add(std::move(slot));
  ed.emit(batchOf(e));
  return e;
}

}  // namespace pq::doc::ops
