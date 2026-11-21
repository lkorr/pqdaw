// AP.2 (docs/plans/AP.md "Where it sits"; pq/document/note_devices.h; lower.h "Note devices"): an Arpeggiator on a MIDI
// track's notes link plays through the playback link — a chord in a MIDI clip renders as stepped notes (offline render),
// a knob drag (style) writes its param slot without republishing the chain or compiling, and the chain goes away with
// the device. The instrument is the probe instrument group (tests/support/blocks.h: its Out is voice.hz, so the
// rendered track is K · Σ hz of the sounding voices): which note sounds in each step is read off the samples.
// 120 BPM: an eighth note is 0.25 s = 12000 samples.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <variant>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/document/note_devices.h"
#include "pq/document/pitch_resolve.h"
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
  explicit Rig(std::string_view probeAtom = "voice.hz") {
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
    probe = test_blocks::addProbeInstrument(store, track, probeAtom);
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

// SM.1: voice.key through the real path (the lowering's doc::midiKeyOf → the timeline → the arp → the voices): the
// probe's Out is voice.key, so each step's level is K · its key.
TEST_CASE("note device playback (SM.1): voice.key: drawn Degree notes are 60 + degree; arp copies keep it, Degree steps "
          "move it, Ratio steps take the nearest 12-TET key") {
  Rig r("voice.key");
  auto link = makeLink(r.store.current());
  [[maybe_unused]] const ListenerId sub = r.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  auto check = [&](const std::vector<float>& out, const std::vector<double>& keys) {
    const double K = mean(out, 4000, kStep - 200) / keys[0];
    REQUIRE(K > 0.0);
    for (size_t k = 0; k < keys.size(); ++k) {
      CAPTURE(k);
      const double got = mean(out, k * kStep + 4000, (k + 1) * kStep - 200);
      CHECK(got / K == doctest::Approx(keys[k]).epsilon(1e-4));
    }
    return K;
  };
  // Up, Steps 2 / Distance 1 degree: degrees 0 4 7, then 1 5 8.
  r.edit(ops::setDeviceParam(r.doc(), r.arp, "steps", position("steps", 2.0)));
  link->flush();
  const double K = check(render(*link, 6.0 * kStep / kRate), {60, 64, 67, 61, 65, 68});
  // Ratio unit, 3/2: the copies at 392.4, 494.4, 588.0 Hz: static pitches, keys 67, 71, 74.
  r.edit(ops::setDeviceParam(r.doc(), r.arp, "unit", position("unit", 1.0)));
  r.edit(ops::setDeviceParam(r.doc(), r.arp, "ratioP", position("ratioP", 3.0)));
  r.edit(ops::setDeviceParam(r.doc(), r.arp, "ratioQ", position("ratioQ", 2.0)));
  link->flush();
  (void)check(render(*link, 6.0 * kStep / kRate), {60, 64, 67, 67, 71, 74});
  // Switched off: the chord as drawn, all three keys at once.
  const DeviceId one[] = {r.arp};
  r.edit(ops::setDevicesActive(r.doc(), one, false));
  link->flush();
  const std::vector<float> chord = render(*link, 2.0 * kStep / kRate);
  CHECK(mean(chord, kStep, 2 * kStep - 200) / K == doctest::Approx(60.0 + 64.0 + 67.0).epsilon(1e-4));
}

// ---- AP.5 / AP.2b ------------------------------------------------------------------------------------------------

TEST_CASE("note device playback (AP.5): lane switches / lengths are structural, a step drag is a slot write") {
  Rig r;
  auto link = makeLink(r.store.current());
  [[maybe_unused]] const ListenerId sub = r.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  {
    const app::LinkStats s0 = link->stats();
    r.edit(ops::setDeviceSetting(r.doc(), r.arp, "velLane", 1.0));
    link->flush();
    CHECK(link->stats().noteStagePublishes == s0.noteStagePublishes + 1);
  }
  {
    const app::LinkStats s0 = link->stats();
    r.edit(ops::setDeviceSetting(r.doc(), r.arp, "velLen", 5.0));
    link->flush();
    CHECK(link->stats().noteStagePublishes == s0.noteStagePublishes + 1);
  }
  {  // a step's value: its knob slot, nothing compiled or republished
    const app::LinkStats s0 = link->stats();
    r.edit(ops::setDeviceParam(r.doc(), r.arp, "velStep3", position("velStep3", 0.25)));
    link->flush();
    CHECK(link->stats().compiles == s0.compiles);
    CHECK(link->stats().noteStagePublishes == s0.noteStagePublishes);
    CHECK(link->stats().paramWrites == s0.paramWrites + 1);
  }
  {  // transpose step 2 by +12 degrees: the second step of the render is an octave up
    r.edit(ops::setDeviceSetting(r.doc(), r.arp, "transLane", 1.0));
    r.edit(ops::setDeviceSetting(r.doc(), r.arp, "transLen", 2.0));
    r.edit(ops::setDeviceParam(r.doc(), r.arp, "transStep2", position("transStep2", 12.0)));
    link->flush();
    const std::vector<float> out = render(*link, 4.0 * kStep / kRate);
    const double K = mean(out, 4000, kStep - 200) / tuning::kDefaultReferenceHz;
    const double want = K * tuning::kDefaultReferenceHz * std::exp2(16.0 / 12.0);  // degree 4 + 12
    CHECK(std::fabs(mean(out, kStep + 4000, 2 * kStep - 200) - want) / want < 1e-3);
  }
}

TEST_CASE("note device playback (AP.2b): a value block cabled into the arp's Rate Den plays (one block late)") {
  Rig r;
  const DeviceId v = r.edit(ops::addDevice(r.doc(), r.store.ids(), r.track, std::string(kValueDevice),
                                           DeviceKind::AudioEffect, ops::kOffPath))
                         .devices.at(0);
  r.edit(ops::setDeviceParamValue(r.doc(), playback_fixture::unitsLibrary(), v, std::string(kValueParam), 1.0));
  Wire w;
  w.from = v;
  w.port = std::string(kValueOutput);
  w.to = r.arp;
  w.param = "rateDen";
  const WireId cable = r.edit(ops::connect(r.doc(), r.store.ids(), w)).wires.at(0);
  auto link = makeLink(r.store.current());
  const auto states = link->wireStates();
  const auto st = std::find_if(states.begin(), states.end(), [&](const WireState& s) { return s.id == cable; });
  REQUIRE(st != states.end());
  CHECK(st->status == WireStatus::Lowered);
  // Rate Den 8 + 1 = 9: steps of a ninth of a whole note (10666.7 samples), not an eighth (12000).
  const std::vector<float> out = render(*link, 4.0 * kStep / kRate);
  const double hz0 = tuning::kDefaultReferenceHz, hz1 = hz0 * std::exp2(4.0 / 12.0);
  const double K = mean(out, 4000, 10400) / hz0;
  REQUIRE(K > 0.0);
  // The second step starts at 10667 (the first, sounding at the note-on before the cable's value arrived, keeps its
  // eighth's length: both sound until 12000); the third at 21333, the fourth at 32000.
  CHECK(std::fabs(mean(out, 12300, 21100) - K * hz1) / (K * hz1) < 1e-3);
  const double hz2 = hz0 * std::exp2(7.0 / 12.0);
  CHECK(std::fabs(mean(out, 21333 + 400, 32000 - 200) - K * hz2) / (K * hz2) < 1e-3);
  CHECK(std::fabs(mean(out, 32000 + 400, 42667 - 200) - K * hz0) / (K * hz0) < 1e-3);
}

namespace {

// AP.2b, as the user hears it: an arp of a held chord on a MOS main tuning, played by the engine block by block.
struct LiveRig {
  DocumentStore store{playback_fixture::unwiredDefaultProject()};
  TrackId track;
  TuningId tuning;
  DeviceId arp;
  std::unique_ptr<ProjectLink> link;
  ListenerId sub{};

  static TuningDef mos(double generator) {
    TuningDef d;
    tuning::MosParams m;
    m.n = 7;
    m.period = 1200.0;
    m.generator = generator;
    d.system = m;
    return d;
  }
  explicit LiveRig(int32_t steps) {
    IdSource& ids = store.ids();
    track = ids.track();
    tuning = store.current()->tunings->front()->id;
    Batch b{"arp live", {}};
    b.commands.push_back(SetTuningDef{tuning, mos(696.0)});
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Arp"), {}, kAppend});
    b.commands.push_back(SetTrackTuning{track, TuningAssignment::mainTuning(tuning)});
    std::vector<Note> chord;
    for (const int32_t d : {0, 2, 4}) chord.push_back(playback_fixture::note(ids, 0, 16 * kQ, d));
    b.commands.push_back(AddClip{track, playback_fixture::clip(ids, 0, 16 * kQ, std::move(chord), "Chord")});
    REQUIRE(store.dispatch(b).ok());
    playback_fixture::wireSeries(store);
    (void)test_blocks::addProbeInstrument(store, track);
    arp = edit(ops::addDevice(doc(), ids, track, noteDevicePatch("arpeggiator"), DeviceKind::AudioEffect)).devices.at(0);
    edit(ops::setDeviceParam(doc(), arp, "rateDen", position("rateDen", 8.0)));
    edit(ops::setDeviceParam(doc(), arp, "gate", position("gate", 1.0)));
    edit(ops::setDeviceParam(doc(), arp, "steps", position("steps", static_cast<double>(steps))));
    link = makeLink(store.current());
    sub = store.subscribe([this](const StoreEvent& e) { link->onDocumentChange(e); });
    REQUIRE(link->engine().post(engine::TransportCommand{engine::TransportCommandKind::Play, false, 0, 0, 0}));
  }
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  // The Hz of degree `d` in the main tuning at generator `g` (the tuning as it is, with that generator).
  double hzOf(int32_t d, std::optional<double> g = std::nullopt) const {
    TuningDef def = findTuning(doc(), tuning)->def;
    if (g) std::get<tuning::MosParams>(def.system).generator = *g;
    auto t = buildTuning(def);
    REQUIRE(t.ok());
    const NotePitch p = resolvePitch(tuning::PitchId::fromDegree(d, 0), *t, tuning);
    REQUIRE(p.hz.has_value());
    return *p.hz;
  }
  // Plays `samples` (64-frame blocks); `at(sample)` runs before the block starting there.
  void play(std::vector<float>& out, size_t samples, const std::function<void(size_t)>& at) {
    engine::Engine& e = link->engine();
    float l[64], rr[64];
    float* outs[2] = {l, rr};
    for (size_t s = out.size(), end = out.size() + samples; s < end; s += 64) {
      at(s);
      e.process(engine::AudioBlock{nullptr, 0, outs, 2, 64, 0, 0, 0});
      out.insert(out.end(), l, l + 64);
      if (((s / 64) & 63) == 0) e.collect();
    }
  }
};

}  // namespace

TEST_CASE("note device playback (AP.2b): a generator drag while the arp runs: no republish, no restart, the steps retune") {
  LiveRig r(2);  // Up x Steps 2 / Distance 1 degree: degrees 0, 2, 4, 1, 3, 5 (the shifted copy re-resolves)
  const int32_t order[6] = {0, 2, 4, 1, 3, 5};
  const app::LinkStats s0 = r.link->stats();
  // A drag in the middle of steps 2, 3 and 4: four writes 640 samples apart each.
  struct Write {
    size_t at;
    double gen;
  };
  std::vector<Write> writes;
  for (int k = 2; k <= 4; ++k)
    for (int i = 0; i < 4; ++i)
      writes.push_back({(static_cast<size_t>(k) * kStep + 4096 + static_cast<size_t>(i) * 640) / 64 * 64,  // block starts
                        696.0 + 2.0 * (3 * (k - 2) + i + 1)});
  REQUIRE(writes.size() == 12);
  std::vector<float> out;
  std::vector<double> genAt;  // the generator per output sample
  double gen = 696.0;
  size_t applied = 0;
  r.play(out, 7 * kStep, [&](size_t s) {
    for (const Write& w : writes)
      if (w.at == s) {
        ++applied;
        gen = w.gen;
        REQUIRE(r.store.dispatch(SetTuningParam{r.tuning, ParamKind::MosGenerator, gen}).ok());
        r.link->flush();
      }
    genAt.resize(s + 64, gen);
  });
  REQUIRE(applied == writes.size());
  CHECK(r.link->stats().noteStagePublishes == s0.noteStagePublishes);  // never republished
  CHECK(r.link->stats().compiles == s0.compiles);
  // A continuous drag moves the tuning node's live inputs; the static table (its entry) stays: nothing to hand over.
  CHECK(r.link->stats().noteTablePosts == s0.noteTablePosts);
  std::map<std::pair<int32_t, double>, double> cache;
  auto expect = [&](int32_t d, double g) {
    const auto key = std::make_pair(d, g);
    if (!cache.count(key)) cache[key] = r.hzOf(d, g);
    return cache[key];
  };
  const double K = mean(out, 4000, kStep - 200) / expect(0, 696.0);
  REQUIRE(K > 0.0);
  // Every step plays its pattern place (no restart) on its grid at the generator sounding then: checked per sample,
  // outside the 10 ms after each write (the 5 ms retune glide), in each step's settled part.
  for (size_t k = 0; k < 7; ++k) {
    CAPTURE(k);
    size_t bad = 0, checked = 0;
    for (size_t i = k * kStep + 2000; i < (k + 1) * kStep - 200; ++i) {
      bool settled = true;
      for (const Write& w : writes)
        if (i >= w.at && i < w.at + 480) settled = false;
      if (!settled) continue;
      ++checked;
      const double want = K * expect(order[k % 6], genAt[i]);
      if (std::fabs(out[i] - want) / want > 2e-3) ++bad;
    }
    CHECK(checked > 5000);
    CHECK(bad == 0);
  }
}

TEST_CASE("note device playback (AP.2b): a structural edit of the arp while it runs (a lane on) continues its pattern") {
  LiveRig r(1);
  const int32_t order[3] = {0, 2, 4};
  const app::LinkStats s0 = r.link->stats();
  std::vector<float> out;
  const size_t at = 4 * kStep + 3008;
  r.play(out, 7 * kStep, [&](size_t s) {
    if (s != at) return;
    r.edit(ops::setDeviceSetting(r.doc(), r.arp, "velLane", 1.0));
    r.link->flush();
  });
  CHECK(r.link->stats().noteStagePublishes == s0.noteStagePublishes + 1);
  const double K = mean(out, 4000, kStep - 200) / r.hzOf(0);
  for (size_t k = 0; k < 7; ++k) {
    CAPTURE(k);
    const double want = K * r.hzOf(order[k % 3]);
    double lo = 1e30, hi = 0.0;
    for (size_t i = k * kStep + 2000; i < (k + 1) * kStep - 200; ++i) {
      lo = std::min(lo, static_cast<double>(out[i]));
      hi = std::max(hi, static_cast<double>(out[i]));
    }
    CHECK(lo > want * 0.999);  // no note ended (no release dip), no restart (the same note throughout)
    CHECK(hi < want * 1.001);
  }
}

TEST_CASE("note device playback (AP.2b): a new tuning table (MOS 7 -> 5, a compile) while the arp runs: no republish") {
  LiveRig r(2);
  const int32_t order[6] = {0, 2, 4, 1, 3, 5};
  const app::LinkStats s0 = r.link->stats();
  std::vector<float> out;
  const size_t at = (3 * kStep + 4096) / 64 * 64;  // a block start
  r.play(out, 7 * kStep, [&](size_t s) {
    if (s != at) return;
    TuningDef d = LiveRig::mos(720.0);
    std::get<tuning::MosParams>(d.system).n = 5;
    REQUIRE(r.store.dispatch(SetTuningDef{r.tuning, d}).ok());
    r.link->flush();
  });
  CHECK(r.link->stats().noteStagePublishes == s0.noteStagePublishes);
  CHECK(r.link->stats().noteTablePosts == s0.noteTablePosts + 1);
  TuningDef before = LiveRig::mos(696.0);
  auto hzIn = [&](const TuningDef& def, int32_t d) {
    auto t = buildTuning(def);
    REQUIRE(t.ok());
    const NotePitch p = resolvePitch(tuning::PitchId::fromDegree(d, 0), *t, r.tuning);
    REQUIRE(p.hz.has_value());
    return *p.hz;
  };
  const double K = mean(out, 4000, kStep - 200) / hzIn(before, 0);
  for (size_t k = 0; k < 7; ++k) {
    if (k == 3) continue;  // the step the change lands in
    CAPTURE(k);
    const double want = K * (k < 3 ? hzIn(before, order[k % 6]) : r.hzOf(order[k % 6]));
    CHECK(std::fabs(mean(out, k * kStep + 2000, (k + 1) * kStep - 200) - want) / want < 1e-3);  // its pattern place
  }
}
