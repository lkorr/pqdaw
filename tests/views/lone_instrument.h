#pragma once

// IG.1-2: a new MIDI track holds an instrument group (In → Basic Synth → Out). View tests written for a bare instrument
// heading the track take it out of its group with Ctrl+Shift+G (a lone stock instrument is a voice context of its own:
// the same sound), so their path is the instrument, then the effects, as before.

#include <doctest/doctest.h>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/store.h"

namespace view_fixture {

inline void loneInstrument(pq::doc::DocumentStore& st, const pq::doc::DeviceLibrary& lib, pq::doc::TrackId t) {
  for (const pq::doc::DevicePtr& d : pq::doc::findTrackPtr(*st.current(), t)->devices)
    if (pq::doc::isGroupDevice(*d)) {
      auto e = pq::doc::ops::ungroupInCircuit(st.current(), st.ids(), lib, d->id);
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
      REQUIRE(st.dispatch(e->command).ok());
      return;
    }
}

}  // namespace view_fixture
