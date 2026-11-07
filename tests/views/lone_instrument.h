#pragma once

// IG.1-2: a new MIDI track holds an instrument group (In → Basic Synth → Out). View tests written for a bare instrument
// heading the track take it out of its group with Ctrl+Shift+G (a lone stock instrument is a voice context of its own:
// the same sound), so their path is the instrument, then the effects, as before.

#include <doctest/doctest.h>

#include "../support/stand_in_synth.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/store.h"

namespace view_fixture {

// GU.4: the group on the track's main path (its instrument group), not a module's group (a Phase Accumulator, a VCA).
inline void loneInstrument(pq::doc::DocumentStore& st, const pq::doc::DeviceLibrary& lib, pq::doc::TrackId t) {
  (void)stand_in::useBasicSynth(st, t);  // (GU.10: the IG.1-2 layout first: Basic Synth in the group)
  for (const pq::doc::Device* d : pq::doc::mainPath(*st.current(), *pq::doc::findTrackPtr(*st.current(), t)).blocks)
    if (pq::doc::isGroupDevice(*d)) {
      auto e = pq::doc::ops::ungroupInCircuit(st.current(), st.ids(), lib, d->id);
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
      REQUIRE(st.dispatch(e->command).ok());
      return;
    }
}

}  // namespace view_fixture
