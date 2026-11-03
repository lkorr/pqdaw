#pragma once

// TX.all-free test helpers for the views tests.

#include "pq/document/main_path.h"
#include "pq/document/model.h"

namespace all_free {

inline bool offPath(const pq::doc::Project& p, pq::doc::DeviceId d) {
  const pq::doc::DeviceLoc l = pq::doc::findDevice(p, d);
  return l.track != nullptr && l.chain == nullptr && !pq::doc::mainPath(p, *l.track).contains(d);
}

// The project's wires that are not main links (the ones a test draws: the main paths' links are the series).
inline pq::doc::WireList drawnWires(const pq::doc::Project& p) {
  pq::doc::WireList out;
  for (const pq::doc::Wire& w : pq::doc::wiresOf(p))
    if (!w.mainLink()) out.push_back(w);
  return out;
}

}  // namespace all_free
