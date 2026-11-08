#pragma once

// IG.1-2: a loaded meta patch (schema 16) has its meta voice run wrapped in an instrument group. View tests written for
// the run heading the track take it out of its group with Ctrl+Shift+G, so their path is the run, then the rest, as
// before. (GU.11: no stand-in synth; a new MIDI track's default instrument group is left to the tests that use it.)

#include <doctest/doctest.h>

#include <string>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/store.h"

namespace view_fixture {

// GU.4: the group on the track's main path (its instrument group), not a module's group (a Phase Accumulator, a VCA).
inline void loneInstrument(pq::doc::DocumentStore& st, const pq::doc::DeviceLibrary& lib, pq::doc::TrackId t) {
  for (const pq::doc::Device* d : pq::doc::mainPath(*st.current(), *pq::doc::findTrackPtr(*st.current(), t)).blocks)
    if (pq::doc::isGroupDevice(*d)) {
      auto e = pq::doc::ops::ungroupInCircuit(st.current(), st.ids(), lib, d->id);
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
      REQUIRE(st.dispatch(e->command).ok());
      return;
    }
}

}  // namespace view_fixture
