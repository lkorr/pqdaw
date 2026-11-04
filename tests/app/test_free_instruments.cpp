// TX.all-free-fix1 (SPEC §15 2026-11-03 "everything is free"; QUESTIONS Q208): stock instruments and racks are ordinary
// blocks. A stock instrument is a block with a notes input and an audio output, like a meta module that takes notes:
// it plays the notes that reach its notes input by a cable (the main path's first link, or any other notes cable from
// its track's source), wherever it is; a block before it on the path feeds its audio input — it has none, so nothing; an
// instrument nothing feeds notes to plays nothing (and is not "missing"). Several instruments on one track each play
// (the voices end when every one is done). A notes cable into a meta module off the path gives the input the note's
// signal of its meaning. A cable into another track's mixer is a send-like route (TX.cross-track: one closing a cycle
// between tracks is delayed by a sample, as every loop). Racks are blocks too: off the path an Instrument Rack plays the notes cabled into it, an Audio Effect
// Rack takes what is wired into its input; inside a rack each chain is a series with the same rule.
//
// Each case renders a three-note chord through the lowered project (interpreter, under the allocation trap) and through
// the JIT (equal to the interpreter, bit for bit), and compares it with what it must equal: the sum of the instruments
// rendered alone, the meta voice run it stands for, twice a track.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/meta/registry.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kFrames = 72000;    // the chord's note-offs at 30000, 33000 and 39000 frames; their releases inside
constexpr uint32_t kFirstOff = 30000;  // before it every voice is held

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  return **lib;
}

Result<LoweredProject> lowered(const Project& p) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [](TrackId) { return 0u; };
  return lower(p, lo);
}

// The lowered project of `p` (one MIDI event slot) rendered for kFrames with a three-note chord.
golden::Channels render(const Project& p, engine::UnitBackend backend = {}) {
  auto low = lowered(p);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  CHECK(low->missingDevices.empty());
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
  for (const LoweredParam& x : low->params) {
    links.push_back({x.track, x.name, static_cast<uint32_t>(values.size())});
    values.push_back(x.value);
  }
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  co.voice.maxVoices = 8;
  co.paramLinks = links;
  co.backend = backend;
  engine::ProjectCompiler compiler(co);
  auto build = compiler.build(low->graph);
  REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
  engine::NoteListSource notes(std::vector<engine::TimedNote>{
      {0, 30000, 220.0f, 0.8f, 0.5f}, {3000, 30000, 277.18f, 0.6f, 0.5f}, {9000, 30000, 329.63f, 0.9f, 0.5f}});
  engine::ProjectConfig pc;
  pc.sampleRate = kRate;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 2;
  pc.numProjectParams = static_cast<uint32_t>(values.size());
  pc.eventSources.push_back(notes.source());
  auto prog = engine::ProjectProgram::create(pc, build->layout);
  REQUIRE_MESSAGE(prog.ok(), (prog.ok() ? std::string() : prog.error().message));
  for (uint32_t u = 0; u < build->images.size(); ++u) REQUIRE((*prog)->publish(u, std::move(build->images[u])).ok());
  std::copy(values.begin(), values.end(), (*prog)->projectParams().begin());
  golden::Channels out(2);
  std::vector<float> l(kBlock), r(kBlock);
  float* ch[2] = {l.data(), r.data()};
  for (int64_t t = 0; t < kFrames; t += kBlock) {
    engine::AudioBlock ab{nullptr, 0, ch, 2, kBlock, t};
    CHECK(alloc_trap::countRtViolations([&] {
            DenormalGuard dg;
            (*prog)->process(ab);
          }) == 0);
    out[0].insert(out[0].end(), l.begin(), l.end());
    out[1].insert(out[1].end(), r.begin(), r.end());
  }
  return out;
}

double maxDiff(const golden::Channels& a, const golden::Channels& b, size_t to = SIZE_MAX) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = 0; i < std::min(to, a[c].size()); ++i) {
      const double d = std::fabs(static_cast<double>(a[c][i]) - static_cast<double>(b[c][i]));
      m = std::max(m, std::isnan(d) ? 1e9 : d);
    }
  }
  return m;
}

double rms(const golden::Channels& a, size_t from, size_t to) {
  double s = 0.0;
  size_t n = 0;
  for (const auto& c : a)
    for (size_t i = from; i < std::min(to, c.size()); ++i, ++n) s += static_cast<double>(c[i]) * c[i];
  return n == 0 ? 0.0 : std::sqrt(s / static_cast<double>(n));
}

float peak(const golden::Channels& a) {
  float p = 0.0f;
  for (const auto& c : a)
    for (float v : c) p = std::max(p, std::fabs(v));
  return p;
}

golden::Channels plus(const golden::Channels& a, const golden::Channels& b, float kb = 1.0f) {
  golden::Channels s = a;
  for (size_t c = 0; c < s.size(); ++c)
    for (size_t i = 0; i < s[c].size(); ++i) s[c][i] = static_cast<float>(static_cast<double>(a[c][i]) + kb * static_cast<double>(b[c][i]));
  return s;
}

// The renders of `p` through the interpreter and the JIT: equal bit for bit (checked); the interpreter's returned.
golden::Channels played(const Project& p, const std::string& what) {
  const golden::Channels a = render(p);
  const golden::Channels j = render(p, compile::jitUnitBackend());
  const double dj = maxDiff(a, j);
  MESSAGE(what << ": interpreter vs JIT max |diff| = " << dj << ", peak " << peak(a));
  CHECK(dj == 0.0);
  return a;
}

// ---- building projects --------------------------------------------------------------------------------------------

template <class E>
E run(DocumentStore& s, Result<E> e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(s.dispatch(e->command).ok());
  return *e;
}

void apply(DocumentStore& s, Command c) {
  const Status st = s.dispatch(std::move(c));
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
}

// A MIDI track as ops::createTrack makes it: source → BasicSynth → mixer.
TrackId midiTrack(DocumentStore& s) { return run(s, ops::createTrack(*s.current(), s.ids(), TrackKind::Midi)).tracks.at(0); }
DeviceId headOf(DocumentStore& s, TrackId t) { return mainPath(*s.current(), *findTrackPtr(*s.current(), t)).blocks.at(0)->id; }

DeviceId offPath(DocumentStore& s, TrackId t, std::string patch, DeviceKind kind) {
  return run(s, ops::addDevice(*s.current(), s.ids(), t, std::move(patch), kind, ops::kOffPath)).devices.at(0);
}

// A meta module off the path (raw: the type's defaults, the same settings wherever it goes).
DeviceId metaOffPath(DocumentStore& s, TrackId t, std::string_view type) {
  const meta::Type* mt = meta::findType(type);
  REQUIRE(mt != nullptr);
  const DeviceId id = s.ids().device();
  auto d = std::make_shared<Device>(*makeDevice(id, std::string(kMetaDevicePrefix) + std::string(type), DeviceKind::AudioEffect));
  d->meta = std::make_shared<const MetaMap>(MetaMap{MetaEntry{"", defaultMetaInstance(*mt)}});
  apply(s, AddDevice{t, std::move(d), kAppend});
  return id;
}

WireId cable(DocumentStore& s, Wire w) { return run(s, ops::connect(*s.current(), s.ids(), std::move(w))).wires.at(0); }
Wire notesInto(TrackId t, DeviceId to, std::string param = {}) {
  Wire w;
  w.noteSource = t;
  w.to = to;
  w.param = std::move(param);
  return w;
}
Wire link(DeviceId from, DeviceId to, std::string port = {}, std::string param = {}) {
  Wire w;
  w.from = from;
  w.port = std::move(port);
  w.to = to;
  w.param = std::move(param);
  return w;
}
Wire intoMixer(DeviceId from, TrackId mixer) {
  Wire w;
  w.from = from;
  w.mixer = mixer;
  return w;
}

// A BasicSynth's sound made different from the default one's (same envelope: the same voice end).
void voiced(DocumentStore& s, DeviceId synth) {
  apply(s, SetDeviceParam{synth, "bright", 0.9});
  apply(s, SetDeviceParam{synth, "level", 0.13});
}

WireStatus stateOf(const Project& p, WireId w) {
  auto low = lowered(p);
  REQUIRE(low.ok());
  for (const WireState& st : low->wires)
    if (st.id == w) return st.status;
  FAIL("no state for the wire");
  return WireStatus::Invalid;
}

// The track's default synth alone, with the varied sound when `varied`.
golden::Channels alone(bool varied, const std::string& what) {
  DocumentStore s;
  const TrackId t = midiTrack(s);
  if (varied) voiced(s, headOf(s, t));
  return played(*s.current(), what);
}

// An Instrument Rack of `chains` (each a list of BasicSynths, `varied` the first chain's sound) heading a MIDI track's path
// alone (its chain mixers smooth from their first value: what a rack sounds like; the reference for racks).
struct RackOf {
  std::vector<size_t> chains;  // BasicSynths per chain
  bool varied = false;
};
golden::Channels rackAlone(const RackOf& r, const std::string& what) {
  DocumentStore s;
  const TrackId t = midiTrack(s);
  run(s, ops::dissolveDevices(*s.current(), s.ids(), &library(), std::vector<DeviceId>{headOf(s, t)}));
  const DeviceId rack = s.ids().device();
  std::vector<ChainPtr> chains;
  std::vector<DeviceId> first;
  for (const size_t n : r.chains) {
    std::vector<DevicePtr> synths;
    for (size_t i = 0; i < n; ++i) synths.push_back(makeDefaultInstrument(s.ids().device()));
    first.push_back(synths.front()->id);
    chains.push_back(makeChain(s.ids().chain(), "Chain", std::move(synths)));
  }
  apply(s, AddDevice{t, makeRack(rack, DeviceKind::Instrument, std::move(chains)), kAppend});
  if (r.varied) voiced(s, first.front());
  run(s, ops::insertOnPath(*s.current(), s.ids(), rack, 0));
  return played(*s.current(), what);
}

}  // namespace

TEST_CASE("free instruments: a stock instrument off the main path, its notes and its output cabled, plays beside the path's") {
  DocumentStore s;
  const TrackId t = midiTrack(s);
  const DeviceId b = offPath(s, t, "BasicSynth", DeviceKind::Instrument);
  voiced(s, b);
  {  // unconnected it plays nothing, and is not missing
    auto low = lowered(*s.current());
    REQUIRE(low.ok());
    CHECK(low->missingDevices.empty());
  }
  const WireId n = cable(s, notesInto(t, b));
  const WireId m = cable(s, intoMixer(b, t));
  CHECK_FALSE(mainPath(*s.current(), *findTrackPtr(*s.current(), t)).contains(b));
  CHECK(stateOf(*s.current(), n) == WireStatus::Lowered);
  CHECK(stateOf(*s.current(), m) == WireStatus::Lowered);
  const golden::Channels both = played(*s.current(), "path synth + off-path synth");
  const golden::Channels a = alone(false, "the path's synth alone");
  const golden::Channels bb = alone(true, "the off-path synth's sound alone");
  const double d = maxDiff(both, plus(a, bb));
  MESSAGE("both vs the sum of each alone: max |diff| = " << d);
  CHECK(rms(bb, 0, kFirstOff) > 1e-3);
  CHECK(d <= 1e-6);
}

TEST_CASE("free instruments: two instruments fed by the same notes, both off the path, each into the mixer") {
  DocumentStore s;
  const TrackId t = midiTrack(s);
  const DeviceId a = headOf(s, t);
  run(s, ops::takeOffPath(*s.current(), s.ids(), &library(), std::vector<DeviceId>{a}));  // the path: source → mixer
  REQUIRE(mainPath(*s.current(), *findTrackPtr(*s.current(), t)).blocks.empty());
  {  // nothing feeds the synth: silence, nothing missing
    auto low = lowered(*s.current());
    REQUIRE(low.ok());
    CHECK(low->missingDevices.empty());
    CHECK(peak(played(*s.current(), "a synth no notes reach")) == 0.0f);
  }
  const DeviceId b = offPath(s, t, "BasicSynth", DeviceKind::Instrument);
  voiced(s, b);
  cable(s, notesInto(t, a));
  cable(s, notesInto(t, b));
  cable(s, intoMixer(a, t));
  cable(s, intoMixer(b, t));
  const golden::Channels both = played(*s.current(), "two off-path synths");
  const double d = maxDiff(both, plus(alone(false, "a alone"), alone(true, "b alone")));
  MESSAGE("two off-path synths vs each alone: max |diff| = " << d);
  CHECK(d <= 1e-6);
}

TEST_CASE("free instruments: an effect before the instrument feeds its audio input (none): the notes cable plays it") {
  DocumentStore s;
  const TrackId t = midiTrack(s);
  const DeviceId synth = headOf(s, t);
  const DeviceId sat = offPath(s, t, "saturator", DeviceKind::AudioEffect);
  apply(s, SetDeviceParam{sat, "drive", 9.0});
  run(s, ops::insertOnPath(*s.current(), s.ids(), sat, 0));  // source → Saturator → BasicSynth → mixer
  {
    const MainPath path = mainPath(*s.current(), *findTrackPtr(*s.current(), t));
    REQUIRE(path.blocks.size() == 2);
    CHECK(path.blocks[0]->id == sat);
    CHECK(path.blocks[1]->id == synth);
  }
  // The Saturator takes the notes (it reads nothing of them) and feeds the synth's audio input, which it has not: no
  // notes reach the synth. It plays nothing and is not missing.
  CHECK(peak(played(*s.current(), "Saturator → BasicSynth")) == 0.0f);
  // A notes cable from the source into the synth: it plays them, exactly as alone (the Saturator's output goes nowhere).
  const WireId n = cable(s, notesInto(t, synth));
  CHECK(stateOf(*s.current(), n) == WireStatus::Lowered);
  const golden::Channels with = played(*s.current(), "Saturator → BasicSynth, notes cabled to the synth");
  const double d = maxDiff(with, alone(false, "the synth alone"));
  MESSAGE("effect before the instrument (notes cabled) vs the synth alone: max |diff| = " << d);
  CHECK(d <= 1e-6);
  // A meta module before it (a VCA at the note source: a voice run of its own, the synth after it fed its audio): the
  // same — silent, then with the notes cabled the synth exactly (the voices end when both are done: the synth last).
  DocumentStore m;
  const TrackId mt = midiTrack(m);
  const DeviceId msynth = headOf(m, mt);
  const DeviceId vca = metaOffPath(m, mt, "vca");
  run(m, ops::insertOnPath(*m.current(), m.ids(), vca, 0));
  REQUIRE(mainPath(*m.current(), *findTrackPtr(*m.current(), mt)).blocks.at(1)->id == msynth);
  CHECK(peak(played(*m.current(), "VCA → BasicSynth")) == 0.0f);
  cable(m, notesInto(mt, msynth));
  const double dm = maxDiff(played(*m.current(), "VCA → BasicSynth, notes cabled to the synth"), alone(false, "the synth alone"));
  MESSAGE("a meta module before the instrument (notes cabled) vs the synth alone: max |diff| = " << dm);
  CHECK(dm <= 1e-6);
}

TEST_CASE("free instruments: notes cabled into a meta module off the path play it as the run at the note source") {
  // The reference: the path source → Phase accumulator → Wavetable → mixer (a meta voice run).
  DocumentStore ref;
  const TrackId rt = midiTrack(ref);
  run(ref, ops::dissolveDevices(*ref.current(), ref.ids(), &library(), std::vector<DeviceId>{headOf(ref, rt)}));
  const DeviceId rpa = metaOffPath(ref, rt, "phase_acc");
  const DeviceId rwt = metaOffPath(ref, rt, "wavetable");
  run(ref, ops::insertOnPath(*ref.current(), ref.ids(), rpa, 0));
  run(ref, ops::insertOnPath(*ref.current(), ref.ids(), rwt, 1));
  const golden::Channels a = played(*ref.current(), "PA → WT on the path");
  // The same two blocks off the path (the path: source → mixer): the notes into the Phase accumulator's main input
  // (its frequency: the note's Hz), its output into the Wavetable's (a phase), the Wavetable into the mixer.
  DocumentStore s;
  const TrackId t = midiTrack(s);
  run(s, ops::dissolveDevices(*s.current(), s.ids(), &library(), std::vector<DeviceId>{headOf(s, t)}));
  const DeviceId pa = metaOffPath(s, t, "phase_acc");
  const DeviceId wt = metaOffPath(s, t, "wavetable");
  const WireId n = cable(s, notesInto(t, pa));
  cable(s, link(pa, wt));
  cable(s, intoMixer(wt, t));
  REQUIRE(mainPath(*s.current(), *findTrackPtr(*s.current(), t)).blocks.empty());
  CHECK(stateOf(*s.current(), n) == WireStatus::Lowered);
  const golden::Channels b = played(*s.current(), "PA → WT off the path, notes cabled");
  // While the notes are held both are the same signal (the run's 5 ms release ramp after note-off is the path's own;
  // off the path the voices end as the engine ends voices without a `done`).
  const double d = maxDiff(a, b, kFirstOff);
  MESSAGE("notes into an off-path meta module vs the run at the note source, before the first note-off: max |diff| = " << d);
  CHECK(rms(b, 0, kFirstOff) > 1e-3);
  CHECK(d <= 1e-6);
  // Into an input of no note meaning (the Wavetable's x reads a phase: the note's phase, phasor(Hz)) — the note's phase
  // into the Wavetable alone plays the same as the accumulator feeding it.
  DocumentStore s2;
  const TrackId t2 = midiTrack(s2);
  run(s2, ops::dissolveDevices(*s2.current(), s2.ids(), &library(), std::vector<DeviceId>{headOf(s2, t2)}));
  const DeviceId wt2 = metaOffPath(s2, t2, "wavetable");
  const WireId n2 = cable(s2, notesInto(t2, wt2));
  cable(s2, intoMixer(wt2, t2));
  CHECK(stateOf(*s2.current(), n2) == WireStatus::Lowered);
  const golden::Channels c = played(*s2.current(), "notes (the phase) into an off-path Wavetable");
  const double d2 = maxDiff(a, c, kFirstOff);
  MESSAGE("notes into an off-path Wavetable (its phase) vs PA → WT on the path, before the first note-off: max |diff| = " << d2);
  CHECK(d2 <= 1e-4);
}

TEST_CASE("free instruments: a cable into another track's mixer is a send-like route; one closing a cycle is a loop") {
  DocumentStore s;
  const TrackId a = midiTrack(s);
  // B: a MIDI track without blocks (source → mixer): its mixer the same law as A's.
  const TrackId b = midiTrack(s);
  run(s, ops::dissolveDevices(*s.current(), s.ids(), &library(), std::vector<DeviceId>{headOf(s, b)}));
  const golden::Channels one = played(*s.current(), "track A alone (and an empty MIDI track)");
  const WireId w = cable(s, intoMixer(headOf(s, a), b));  // A's synth also into B's mixer
  CHECK(stateOf(*s.current(), w) == WireStatus::Lowered);
  const golden::Channels two = played(*s.current(), "A's synth into A's and B's mixers");
  const double d = maxDiff(two, plus(one, one));
  MESSAGE("A's synth through both mixers vs twice A: max |diff| = " << d);
  CHECK(rms(one, 0, kFirstOff) > 1e-3);
  CHECK(d == 0.0);
  // A group around A with a Utility on its path; the Utility's output into A's mixer: A → the group → the Utility → A's
  // mixer is a cycle between tracks. TX.cross-track (SPEC §15 2026-11-03 "tracks are not walls"): no longer refused —
  // it is lowered with the z⁻¹ on the cable and runs per sample (tests/app/test_cross_track.cpp checks such loops
  // against hand-written per-sample references); here: it plays (the loop is heard) and decays (loop gain ≈ 0.707).
  DocumentStore g;
  const TrackId ga = midiTrack(g);
  const TrackId grp = run(g, ops::groupTracks(*g.current(), g.ids(), std::vector<TrackId>{ga})).tracks.at(0);
  const DeviceId util = run(g, ops::addDevice(*g.current(), g.ids(), grp, "utility", DeviceKind::AudioEffect)).devices.at(0);
  const golden::Channels before = played(*g.current(), "a track in a group with a Utility");
  const WireId loop = cable(g, intoMixer(util, ga));
  CHECK(stateOf(*g.current(), loop) == WireStatus::Delayed);
  const golden::Channels after = played(*g.current(), "… and a cable from the group's Utility into the track's mixer (a loop)");
  CHECK(maxDiff(before, after) > 1e-3);
  CHECK(peak(after) < peak(before) / (1.0f - 0.7072f));
}

TEST_CASE("free instruments: an Instrument Rack off the path plays the notes cabled into it; an Audio Effect Rack takes its wires") {
  DocumentStore s;
  const TrackId t = midiTrack(s);
  // An Instrument Rack (one chain: a BasicSynth with the varied sound), unconnected: it plays nothing, not missing.
  const DeviceId rack = s.ids().device(), inner = s.ids().device();
  const ChainId chain = s.ids().chain();
  apply(s, AddDevice{t, makeRack(rack, DeviceKind::Instrument, {makeChain(chain, "Chain 1", {makeDefaultInstrument(inner)})}), kAppend});
  voiced(s, inner);
  {
    auto low = lowered(*s.current());
    REQUIRE(low.ok());
    CHECK(low->missingDevices.empty());
  }
  const WireId n = cable(s, notesInto(t, rack));
  const WireId m = cable(s, intoMixer(rack, t));
  CHECK(stateOf(*s.current(), n) == WireStatus::Lowered);
  CHECK(stateOf(*s.current(), m) == WireStatus::Lowered);
  const golden::Channels both = played(*s.current(), "path synth + off-path Instrument Rack");
  const double d = maxDiff(both, plus(alone(false, "a alone"), rackAlone({{1}, true}, "the rack alone on a path")));
  MESSAGE("path synth + off-path Instrument Rack vs each alone: max |diff| = " << d);
  CHECK(d <= 1e-6);
  // An empty Audio Effect Rack off the path (it passes its input) fed by the path's synth, into the mixer: twice it.
  DocumentStore e;
  const TrackId et = midiTrack(e);
  const DeviceId aer = e.ids().device();
  apply(e, AddDevice{et, makeRack(aer, DeviceKind::AudioEffect), kAppend});
  const WireId in = cable(e, link(headOf(e, et), aer));
  const WireId out = cable(e, intoMixer(aer, et));
  CHECK(stateOf(*e.current(), in) == WireStatus::Lowered);
  CHECK(stateOf(*e.current(), out) == WireStatus::Lowered);
  const golden::Channels twice = played(*e.current(), "synth → mixer and → empty Audio Effect Rack → mixer");
  const golden::Channels one = alone(false, "the synth alone");
  const double d2 = maxDiff(twice, plus(one, one));
  MESSAGE("off-path empty Audio Effect Rack (pass) beside the path vs twice the synth: max |diff| = " << d2);
  CHECK(d2 <= 1e-6);
}

TEST_CASE("free instruments: inside a rack each chain is a series with the same rule (no instrument is missing)") {
  // An Audio Effect Rack on the path after the synth, its one chain holding a BasicSynth: the chain feeds the synth's
  // audio input (none) and no notes reach it: that chain is silent (before fix1 the synth was missing and bypassed).
  DocumentStore s;
  const TrackId t = midiTrack(s);
  const DeviceId rack = s.ids().device(), inner = s.ids().device();
  apply(s, AddDevice{t, makeRack(rack, DeviceKind::AudioEffect, {makeChain(s.ids().chain(), "Chain 1", {makeDefaultInstrument(inner)})}), kAppend});
  run(s, ops::insertOnPath(*s.current(), s.ids(), rack, kAppend));
  CHECK(peak(played(*s.current(), "synth → Audio Effect Rack [BasicSynth]")) == 0.0f);
  // An Instrument Rack whose chain is [BasicSynth, BasicSynth]: the second is fed the first's audio: it plays nothing,
  // and the chain's end is it: silent; the chain [BasicSynth] beside it plays.
  DocumentStore r;
  const TrackId rt = midiTrack(r);
  run(r, ops::dissolveDevices(*r.current(), r.ids(), &library(), std::vector<DeviceId>{headOf(r, rt)}));
  const DeviceId ir = r.ids().device(), s1 = r.ids().device(), s2 = r.ids().device(), s3 = r.ids().device();
  apply(r, AddDevice{rt,
                     makeRack(ir, DeviceKind::Instrument,
                              {makeChain(r.ids().chain(), "Two", {makeDefaultInstrument(s1), makeDefaultInstrument(s2)}),
                               makeChain(r.ids().chain(), "One", {makeDefaultInstrument(s3)})}),
                     kAppend});
  run(r, ops::insertOnPath(*r.current(), r.ids(), ir, 0));
  const golden::Channels both = played(*r.current(), "Instrument Rack [synth, synth] + [synth]");
  const double d = maxDiff(both, rackAlone({{1}, false}, "an Instrument Rack [synth]"));
  MESSAGE("rack chains [synth, synth] + [synth] vs the rack [synth]: max |diff| = " << d);
  CHECK(d <= 1e-6);
}

