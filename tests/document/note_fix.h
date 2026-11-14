#pragma once

// T7.11a: the fixture of the note-bend and note-anchor tests: the document fixture plus an empty 12-EDO clip "N" on
// Bass (4 bars from 0) and lookups of what its notes resolve to.

#include <doctest/doctest.h>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "edit_helpers.h"
#include "pq/document/note_bend.h"
#include "pq/document/pitch_resolve.h"
#include "pq/document/serialize.h"

namespace note_fix {

using namespace edit_test;
using doctest_fixture::Fixture;
using doctest_fixture::makeClip;
using pq::kPpq;
using pq::tuning::PitchId;

inline double centsOf(double a, double b) { return 1200.0 * std::log2(b / a); }

struct BendFix : Fixture {
  ClipRef ref;
  BendFix() {
    ClipPtr c = makeClip(store.ids(), 0, 4 * kBar, {}, "N");
    ref = {bass, c->id};
    REQUIRE(store.dispatch(AddClip{bass, c}).ok());
  }
  const NoteClip& clip() const { return Fixture::clip(ref.track, ref.clip); }
  const NoteClip& clipAt(size_t i) const { return *track(bass).clips.at(i).clip; }
  Note add(Ticks start, PitchId pitch, Ticks length = kPpq) {
    Note n;
    n.id = store.ids().note();
    n.start = start;
    n.length = length;
    n.pitch = pitch;
    REQUIRE(store.dispatch(AddNotes{ref, {n}}).ok());
    return n;
  }
  ClipPitches pitches(const NoteClip& c) const { return resolveClipPitches(doc(), *findTrack(doc(), bass), c); }
  std::optional<double> hzIn(const NoteClip& c, NoteId id) const {
    const ClipPitches cp = pitches(c);
    for (const NotePitch& np : cp.notes)
      if (np.note == id) return np.hz;
    return std::nullopt;
  }
  double hz(NoteId id) const {
    const std::optional<double> v = hzIn(clip(), id);
    REQUIRE(v.has_value());
    return *v;
  }
  // The note of `c` at `start` (the tests keep starts distinct).
  static const Note& noteAt(const NoteClip& c, Ticks start) {
    for (const Note& n : c.notes)
      if (n.start == start) return n;
    FAIL("no note at this tick");
    return *c.notes.begin();
  }
  std::vector<Breakpoint> effective(const NoteClip& c, const Note& n, double tol = 0.05) const {
    return effectiveNoteCurve(c, n, NotePitchIndex(pitches(c)), tol);
  }
  void roundTrips() const {
    pq::Result<std::string> text = serializeProject(doc(), store.view());
    REQUIRE_MESSAGE(text.ok(), (text.ok() ? std::string() : text.error().message));
    pq::Result<LoadedProject> back = parseProject(*text);
    REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
    CHECK(*back->project == doc());
    pq::Result<std::string> again = serializeProject(*back->project, back->view);
    REQUIRE(again.ok());
    CHECK(*again == *text);
  }
};

}  // namespace note_fix
