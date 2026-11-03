#pragma once

// TX.all-free (SPEC §15 2026-11-03 "everything is free; the chain is just cables"): a track's MAIN PATH.
//
// A track's blocks are a set (Track::devices) and its audio path is its wires (model.h "Modulation wires"). The main
// path is what the main ports follow: from the track's source (its notes, clips or summed input: a wire from the
// source's main output) along wires from a block's main output into the next block's main input (kMainPort at both
// ends) to the track's mixer. It is the one rule everything reads:
//   the lowering   compiles the path's blocks in signal order, joined as the wires join them (lower.h "The main path"),
//                  and every other wire as a wire: one general rule, the series is not special;
//   the views      the Device view lists the path (an off-path block is in the free rack), the circuit draws every
//                  wire as a cable, list edits rewire it (ops::insertOnPath, ops::dissolveDevices).
// A branch (a main output feeding several main inputs) or a merge (several main outputs into one main input): the path
// takes, at each step, the oldest link (lowest wire id) that still reaches the mixer, else the oldest link at all; it
// never visits a block twice. A group's members are never on it (they are inside the group). A path that does not reach
// the mixer is BROKEN: the list shows what is on it, the track is silent.
//
// The MAIN PORTS of a device (what a wire's kMainPort end means, and what the views offer as a block's main in / out):
// the main output is the patch's first output; the main input its first input that is a signal (not a setting, not a
// param nor a param's `_add`, and, with the callee, driving something). An instrument's main input is its notes (no
// signal input: `in` ""). Racks: an Audio Effect Rack's "in" / "out", an Instrument Rack's "out" (notes in).
//
// Library-free: the path follows kMainPort ends only, so the document, the lowering and the views agree without the
// device library; the edit builders and the views write kMainPort for every connection of two main ports
// (canonicalWire), so a cable drawn by hand between them joins the path too.
//
// Not RT code (UI / compile threads). O(blocks + wires) per track.

#include <cstdint>
#include <string>
#include <span>
#include <utility>
#include <vector>

#include "pq/document/model.h"
#include "pq/graph/patch.h"

namespace pq::doc {

class DeviceLibrary;

struct MainPorts {
  std::string in, out;  // "" = none
};
// The main ports of a device patch (graph signature `sig` of the patch a device plays; `callee` the patch itself when
// known). `instrument`: no signal input (its notes are its main input).
MainPorts mainPortsOf(const graph::SubcircuitRef& sig, const graph::Patch* callee, bool instrument);
// The main ports of `d` as `lib` resolves the patch it plays (racks and groups by their kind; null lib: none known).
MainPorts mainPortsOf(const Device& d, const DeviceLibrary* lib);

struct MainPath {
  std::vector<const Device*> blocks;  // on the path, in signal order
  // links[i] is the wire into blocks[i]; when complete, links.back() is the wire into the mixer (blocks.size() + 1).
  std::vector<WireId> links;
  bool complete = false;  // it reaches the mixer
  int32_t indexOf(DeviceId d) const noexcept;  // −1: not on it
  bool contains(DeviceId d) const noexcept { return indexOf(d) >= 0; }
  bool isLink(WireId w) const noexcept;
};
MainPath mainPath(const Project& p, const Track& t);
// The main links of a whole project by the track they start on (its source, or a top-level block of it), in id order:
// one pass over the wires for callers that want every track's path (the lowering, the project patch view), so each
// mainPath reads only its own track's links. Holds pointers into the project's wires (keep the snapshot alive).
class MainLinks {
public:
  explicit MainLinks(const Project& p);
  std::span<const std::pair<uint32_t, const Wire*>> of(TrackId t) const noexcept;
private:
  std::vector<std::pair<uint32_t, const Wire*>> by_;  // (track id, link), by track, each track's in id order
};
MainPath mainPath(const Track& t, const MainLinks& links);

// The track's top-level blocks in FLOW order: the main path's in signal order, then every other block in list order.
std::vector<DevicePtr> flowOrder(const Track& t, const MainPath& path);

// TX.all-free: `w` with kMainPort at each end that is its device's main port by name (so a cable drawn between two main
// ports is a link of the path). `lib` resolves the patches (null: unchanged).
Wire canonicalWire(const Project& p, Wire w, const DeviceLibrary* lib);

}  // namespace pq::doc
