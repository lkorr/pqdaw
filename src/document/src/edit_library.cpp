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
  // The new device: fresh ids inside, the replaced device's id on top (its state key, wires and mappings follow it).
  DevicePtr fresh = cloneDevice(with, ids);
  auto top = std::make_shared<Device>(*fresh);
  top->id = device;
  Edit e = makeEdit("Hot-Swap Preset");
  e.devices.push_back(device);
  Batch& b = batchOf(e);
  b.commands.push_back(RemoveDevice{device});
  b.commands.push_back(AddDevice{loc.track->id, std::move(top), loc.index, loc.chain ? loc.chain->id : ChainId{}});
  // Wires into a param the new device still has, and every wire out of it (its exports), come back with their ids.
  for (const Wire& w : wiresOf(p)) {
    if (w.from != device && w.to != device) continue;
    if (w.to == device && !keepsParam(w.param)) continue;
    // A wire between the replaced device and something inside it (a rack's own inner devices) is gone with them.
    if (w.from != device && !findDevice(p, w.from).track) continue;
    if (w.from != device && deviceContains(*loc.device, w.from)) continue;
    if (w.to != device && deviceContains(*loc.device, w.to)) continue;
    b.commands.push_back(AddWire{w});
  }
  // Mappings of the racks around it onto its params.
  forEachDeviceIn(loc.track->devices, [&](const Device& r) {
    if (!r.rack || r.id == device || !deviceContains(r, device)) return;
    for (const MacroMapping& m : r.rack->mappings)
      if (m.device == device && keepsParam(m.param)) b.commands.push_back(AddMacroMapping{r.id, m});
  });
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
