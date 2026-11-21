// AP.2 (docs/plans/AP.md "Where it sits"; pq/document/note_devices.h; lower.h "Note devices"): an Arpeggiator on a MIDI
// track's notes link plays through the playback link — a chord in a MIDI clip renders as stepped notes (offline render),
// a knob drag (style) writes its param slot without republishing the chain or compiling, and the chain goes away with
// the device. The instrument is the probe instrument group (tests/support/blocks.h: its Out is voice.hz, so the
// rendered track is K · Σ hz of the sounding voices): which note sounds in each step is read off the samples.
// 120 BPM: an eighth note is 0.25 s = 12000 samples.
#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/document/note_devices.h"
#include "pq/engine/arp.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr double kRate = 48000.0;
constexpr Ticks kQ = kPpq;
constexpr size_t kStep = 12000;  // an eighth at 120 BPM

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

std::vector<float> render(ProjectLink& link, double seconds) {
  engine::BufferSink sink;
  auto st = link.renderOffline(0, static_cast<uint64_t>(std::llround(seconds * kRate)), sink);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  REQUIRE(sink.channels.size() == 2);
  return sink.channels[0];
}

double mean(const std::vector<float>& x, size_t from, size_t to) {
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += x[i];
  return s / static_cast<double>(to - from);
}
double peak(const std::vector<float>& x, size_t from, size_t to) {
  double m = 0.0;
  for (size_t i = from; i < to; ++i) m = std::max(m, std::fabs(static_cast<double>(x[i])));
  return m;
}

// A position of an arp param (its declared line: note_devices.h noteParamPosition).
double position(const char* name, double value) {
  const NoteParamSpec* s = findNoteDeviceType("arpeggiator")->param(name);
  REQUIRE(s != nullptr);
  return noteParamPosition(*s, value);
}

struct Rig {
  DocumentStore store{playback_fixture::unwiredDefaultProject()};
  TrackId track;
  DeviceId probe, arp;
  Rig() {
    IdSource& ids = store.ids();
    track = ids.track();
    Batch b{"arp fixture", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Arp"), {}, kAppend});
    // A C major triad (12-EDO degrees 0, 4, 7) held for two bars.
    std::vector<Note> chord;
    for (const int32_t d : {7, 0, 4}) chord.push_back(playback_fixture::note(ids, 0, 8 * kQ, d));
    b.commands.push_back(AddClip{track, playback_fixture::clip(ids, 0, 8 * kQ, std::move(chord), "Chord")});
    REQUIRE(store.dispatch(b).ok());
    playback_fixture::wireSeries(store);
    probe = test_blocks::addProbeInstrument(store, track);
    arp = edit(ops::addDevice(doc(), ids, track, noteDevicePatch("arpeggiator"), DeviceKind::AudioEffect)).devices.at(0);
    // Up, an eighth (1/8 synced), gate 100 % (each step sounds its whole length).
    edit(ops::setDeviceParam(doc(), arp, "rateDen", position("rateDen", 8.0)));
    edit(ops::setDeviceParam(doc(), arp, "gate", position("gate", 1.0)));
  }
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
};

}  // namespace

TEST_CASE("note device playback: a chord through an Arpeggiator renders stepped notes; a knob drag republishes nothing") {
  Rig r;
  const MainPath path = mainPath(r.doc(), *findTrackPtr(r.doc(), r.track));
  REQUIRE(path.blocks.size() == 2);
  CHECK(path.blocks[0]->id == r.arp);  // after the tuning block, before the instrument
  CHECK(path.blocks[1]->id == r.probe);
  auto link = makeLink(r.store.current());
  [[maybe_unused]] const ListenerId sub = r.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  CHECK(link->stats().noteStagePublishes == 1);
  const double hz[3] = {tuning::kDefaultReferenceHz, tuning::kDefaultReferenceHz * std::exp2(4.0 / 12.0),
                        tuning::kDefaultReferenceHz * std::exp2(7.0 / 12.0)};
  // Step k sounds ONE note (Up: by Hz), its whole eighth: K · hz in the settled part of each step (the voice's de-click
  // fade-in and the previous note's 5 ms release before it).
  auto check = [&](const std::vector<float>& out, const int order[3]) {
    const double K = mean(out, 4000, kStep - 200) / hz[order[0]];
    REQUIRE(K > 0.0);
    for (size_t k = 0; k < 6; ++k) {
      CAPTURE(k);
      const double want = K * hz[order[k % 3]];
      const double got = mean(out, k * kStep + 4000, (k + 1) * kStep - 200);
      CHECK(std::fabs(got - want) / want < 1e-3);
      // one voice, not the chord: the step is never the sum of two notes
      CHECK(peak(out, k * kStep + 4000, (k + 1) * kStep - 200) < 1.02 * want);
    }
  };
  const std::vector<float> up = render(*link, 6.0 * kStep / kRate);
  const int upOrder[3] = {0, 1, 2};
  check(up, upOrder);
  // A knob drag (style → Down): its slot, no republish, no compile; the next render steps downwards.
  {
    const app::LinkStats s0 = link->stats();
    r.edit(ops::setDeviceParam(r.doc(), r.arp, "style", position("style", static_cast<double>(engine::ArpStyle::Down))));
    link->flush();
    CHECK(link->stats().compiles == s0.compiles);
    CHECK(link->stats().noteStagePublishes == s0.noteStagePublishes);
    CHECK(link->stats().paramWrites == s0.paramWrites + 1);
  }
  const std::vector<float> down = render(*link, 6.0 * kStep / kRate);
  const int downOrder[3] = {2, 1, 0};
  check(down, downOrder);
  // A structural setting republishes the track's chain.
  {
    const app::LinkStats s0 = link->stats();
    r.edit(ops::setDeviceSetting(r.doc(), r.arp, "seed", 3.0));
    link->flush();
    CHECK(link->stats().noteStagePublishes == s0.noteStagePublishes + 1);
  }
  // Switched off it passes the notes: the chord sounds (all three voices at once); the chain is gone (null published).
  {
    const app::LinkStats s0 = link->stats();
    const DeviceId one[] = {r.arp};
    r.edit(ops::setDevicesActive(r.doc(), one, false));
    link->flush();
    CHECK(link->stats().noteStagePublishes == s0.noteStagePublishes + 1);
    const std::vector<float> chord = render(*link, 6.0 * kStep / kRate);
    const double K = mean(up, 4000, kStep - 200) / hz[0];
    const double want = K * (hz[0] + hz[1] + hz[2]);
    CHECK(std::fabs(mean(chord, 2 * kStep + 4000, 3 * kStep) - want) / want < 1e-3);
  }
}
