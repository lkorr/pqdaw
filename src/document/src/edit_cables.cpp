// DY.2: cable sources — what a sidechain picker offers and its one-step rewire (pq/document/edit_ops.h "Cable sources").
#include <algorithm>
#include <string>
#include <vector>

#include "edit_common.h"
#include "pq/document/main_path.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

Wire sourceEnd(DeviceId from) {
  Wire w;
  w.from = from;
  w.port = std::string(kMainPort);
  return w;
}

// `param` as a wire into `input` names it (its main input: kMainPort).
std::string canonicalParam(const Project& p, const DeviceLibrary* lib, DeviceId input, std::string_view param) {
  Wire w;
  w.to = input;
  w.param = std::string(param);
  return canonicalWire(p, std::move(w), lib).param;
}

}  // namespace

std::vector<CableSource> cableSources(const Project& p, TrackId except) {
  std::vector<CableSource> out;
  const MainLinks links(p);
  forEachTrack(p, [&](const Track& t, const Track*) {
    if (t.id == except || t.kind == TrackKind::Master) return;
    const MainPath path = mainPath(t, links);
    CableSource pre;
    pre.track = t.id;
    if (t.kind == TrackKind::Midi) {
      if (path.blocks.empty()) return;  // notes only: no audio of its own
      pre.ends = sourceEnd(path.blocks.front()->id);
    } else {
      pre.ends.noteSource = t.id;
      pre.ends.port = std::string(kMainPort);
    }
    out.push_back(pre);
    if (path.complete && !path.blocks.empty()) {
      CableSource post;
      post.track = t.id;
      post.post = true;
      post.ends = sourceEnd(path.blocks.back()->id);
      if (!sameSourceEnd(post.ends, pre.ends)) out.push_back(std::move(post));
    }
  });
  return out;
}

bool sameSourceEnd(const Wire& a, const Wire& b) noexcept {
  return a.noteSource == b.noteSource && (a.fromNoteSource() || a.from == b.from) && a.port == b.port;
}

const Track* sourceTrackOf(const Project& p, const Wire& w) noexcept {
  if (w.fromNoteSource()) return findTrackPtr(p, w.noteSource);
  return findDevice(p, w.from).track;
}

std::vector<WireId> crossTrackWiresInto(const Project& p, const DeviceLibrary* lib, DeviceId input, std::string_view param) {
  std::vector<WireId> out;
  const Track* own = findDevice(p, input).track;
  if (own == nullptr) return out;
  const std::string canon = canonicalParam(p, lib, input, param);
  for (const Wire& w : wiresOf(p)) {
    if (w.toMixer() || w.to != input || (w.param != param && w.param != canon)) continue;
    if (const Track* t = sourceTrackOf(p, w); t != nullptr && t != own) out.push_back(w.id);
  }
  return out;
}

EditResult setCableSource(const Project& p, IdSource& ids, const DeviceLibrary* lib, DeviceId input, std::string param,
                          const std::optional<Wire>& source, DeviceId on, std::string onParam) {
  const DeviceLoc target = findDevice(p, input);
  if (target.track == nullptr) return notFound("the input's block was not found");
  const std::vector<WireId> old = crossTrackWiresInto(p, lib, input, param);
  Wire ends;
  if (source) {
    ends.from = source->from;
    ends.noteSource = source->noteSource;
    ends.port = source->port;
    ends.to = input;
    ends.param = std::move(param);
    ends = canonicalWire(p, std::move(ends), lib);
    const Track* from = sourceTrackOf(p, ends);
    if (from == nullptr) return notFound("the cable source was not found");
    if (from == target.track) return invalid("a cable source is another track");
    // Already the one source: nothing changes.
    if (old.size() == 1)
      if (const Wire* w = findWire(p, old.front()); w != nullptr && w->sameEnds(ends)) return makeEdit("Cable Source");
  } else if (old.empty()) {
    return makeEdit("Cable Source");
  }
  Edit e = makeEdit("Cable Source");
  for (const WireId id : old) batchOf(e).commands.push_back(RemoveWire{id});
  if (source) {
    // (The old wires are gone first: a removed wire with these ends is no conflict.)
    ends.law = WireLaw::Add;
    ends.id = ids.wire();
    PQ_DOC_TRY(checkWire(ends));
    e.wires.push_back(ends.id);
    batchOf(e).commands.push_back(AddWire{std::move(ends)});
  }
  if (on.valid() && !onParam.empty()) {
    const double v = source ? 1.0 : 0.0;
    EditResult set = lib != nullptr ? setDeviceParamValue(p, *lib, on, onParam, v) : setDeviceParam(p, on, onParam, v);
    if (!set) return set.error();
    for (Command& c : batchOf(*set).commands) batchOf(e).commands.push_back(std::move(c));
  }
  return e;
}

}  // namespace pq::doc::ops
