#pragma once

// TX.all-free: internals of the main path's edit builders (edit_paths.cpp), shared with the device, group, rack and
// track builders. Private to pq_doc.

#include <memory>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"

namespace pq::doc::ops::detail {

// Builders applied in turn to a scratch snapshot (each reads what the previous made): the steps of one Batch.
class Steps {
public:
  explicit Steps(const Project& p);
  const Project& doc() const noexcept { return *snap_; }
  Status add(const Command& c);
  Status add(const EditResult& e);  // its batch's commands, in order
  std::vector<Command> commands;

private:
  Snapshot snap_;
};

// A wire end: a device, or (device invalid) a track's source (a wire's start) / mixer (its end).
struct End {
  DeviceId device;
  TrackId track;
};
// A main link (kMainPort at both ends) from `from` to `to`, a fresh id.
Wire mainLink(IdSource& ids, const End& from, const End& to);
End endFrom(const Wire& w);
End endTo(const Wire& w);
bool hasEnds(const Project& p, const Wire& w);  // a wire with the same two ends exists
// Splices block `device` (of `track`, off its path) into the track's main path at `slot` (0: right after the source;
// kAppend or past the end: last). No path at all: source → it → mixer.
Status splice(Steps& k, IdSource& ids, TrackId track, DeviceId device, uint32_t slot);
// Takes `device`'s main-port wires away, joining every wire into its main input to every wire out of its main output
// (a path through it stays connected); the device and its other wires stay.
Status detach(Steps& k, IdSource& ids, const DeviceLibrary* lib, DeviceId device);

}  // namespace pq::doc::ops::detail
