#include "pq/document/change_set.h"

#include <algorithm>

namespace pq::doc {

namespace {

template <class T>
void append(std::vector<T>& dst, const std::vector<T>& src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

template <class T>
void sortUnique(std::vector<T>& v) {
  if (v.size() < 2) return;
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
}

}  // namespace

void ChangeSet::merge(const ChangeSet& o) {
  flags = static_cast<uint8_t>(flags | o.flags);
  append(tracks, o.tracks);
  append(clips, o.clips);
  append(notes, o.notes);
  append(lanes, o.lanes);
  append(tunings, o.tunings);
  append(locators, o.locators);
  append(devices, o.devices);
  append(wires, o.wires);
  append(params, o.params);
}

void ChangeSet::normalize() {
  sortUnique(tracks);
  sortUnique(clips);
  sortUnique(notes);
  sortUnique(lanes);
  sortUnique(tunings);
  sortUnique(locators);
  sortUnique(devices);
  sortUnique(wires);
  sortUnique(params);
}

void ChangeSet::clear() noexcept {
  flags = 0;
  tracks.clear();
  clips.clear();
  notes.clear();
  lanes.clear();
  tunings.clear();
  locators.clear();
  devices.clear();
  wires.clear();
  params.clear();
}

}  // namespace pq::doc
