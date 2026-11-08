// TX.all-free-fix1 (SPEC §15 2026-11-03 "everything is free"; QUESTIONS Q208): instruments are ordinary blocks. GU.11
// (one hierarchy): an instrument is an instrument group (the default instrument, pasted where a test wants a synth); it
// plays the notes that reach its main input by a cable (the main path's first link, or any other notes cable from its
// track's source), wherever it is; an instrument nothing feeds notes to plays nothing (and is not "missing"). Several
// instruments on one track each play. A notes cable into an engine off the path gives the input the note's signal of
// its meaning. A cable into another track's mixer is a send-like route (TX.cross-track: one closing a cycle between
// tracks is delayed by a sample, as every loop). (The stock instrument's missing audio input — an effect before it on
// the path feeding nothing — went with the stock instruments: a group before which an effect sits is fed its audio.)
//
// Each case renders a three-note chord through the lowered project (interpreter, under the allocation trap) and through
// the JIT (equal to the interpreter, bit for bit), and compares it with what it must equal: the sum of the instruments
// rendered alone, the voice run it stands for, twice a track.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/meta/registry.h"
#include "../support/blocks.h"
#include "pq/document/modules.h"

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
// IG.1-2: rendered as an older file of the project loads: its meta voice runs in instrument groups (per note), as written.
golden::Channels render(const Project& p0, engine::UnitBackend backend = {}) {
  Project p = p0;
  (void)migrateToInstrumentGroups(p);
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
  for (uint32_t u = 0; u < build->images.size(); ++u) {
    const auto pub = (*prog)->publish(u, std::move(build->images[u]));
    REQUIRE_MESSAGE(pub.ok(), (pub.ok() ? std::string() : pub.error().message));
  }
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
  const Status st = s.dispatch(playback_fixture::inUnits(s, std::move(c)));
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
}

// A MIDI track as ops::createTrack makes it: source → its instrument group (the default instrument) → mixer.
// TX.pan-laws: constant power (−3 dB at the centre: the loop below decays with gain ≈ 0.707).
TrackId midiTrack(DocumentStore& s) {
  const TrackId t = run(s, ops::createTrack(*s.current(), s.ids(), TrackKind::Midi)).tracks.at(0);
  REQUIRE(s.dispatch(SetTrackPanMode{t, PanMode::ConstantPower}).ok());
  return t;
}
// The instrument group heading track `t`.
DeviceId headOf(DocumentStore& s, TrackId t) { return mainPath(*s.current(), *findTrackPtr(*s.current(), t)).blocks.at(0)->id; }
// GU.11: a synth (the default instrument group) pasted off the path of `t`.
DeviceId synthOff(DocumentStore& s, TrackId t) {
  IdSource local;
  return run(s, ops::pasteBlocks(*s.current(), s.ids(), t, defaultInstrumentBlocks(local))).devices.at(0);
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

// A synth's sound made different from the default one's (same envelope: the same voice end): its Pitch a fifth up.
void voiced(DocumentStore& s, DeviceId synth) {
  const DeviceId pitch = test_blocks::member(*s.current(), synth, kPaPitch);
  REQUIRE(pitch.valid());
  run(s, ops::setDeviceParamValue(*s.current(), library(), pitch, std::string(kValueParam), 702.0));
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


}  // namespace

TEST_CASE("free instruments: an instrument group off the main path, its notes and its output cabled, plays beside the path's") {
  DocumentStore s;
  const TrackId t = midiTrack(s);
  const DeviceId b = synthOff(s, t);
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
  const DeviceId b = synthOff(s, t);
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

TEST_CASE("free instruments: notes cabled into a meta module off the path play it as the run at the note source") {
  // The reference: the path source → Wavetable → Wavetable → mixer (a meta voice run; GU.4: a Wavetable where the Phase
  // accumulator was, a group preset now).
  DocumentStore ref;
  const TrackId rt = midiTrack(ref);
  run(ref, ops::dissolveDevices(*ref.current(), ref.ids(), &library(), std::vector<DeviceId>{headOf(ref, rt)}));
  const DeviceId rpa = metaOffPath(ref, rt, "wavetable");
  const DeviceId rwt = metaOffPath(ref, rt, "wavetable");
  run(ref, ops::insertOnPath(*ref.current(), ref.ids(), rpa, 0));
  run(ref, ops::insertOnPath(*ref.current(), ref.ids(), rwt, 1));
  const golden::Channels a = played(*ref.current(), "WT → WT on the path");
  // The same two blocks off the path (the path: source → mixer): the notes into the first one's main input (a phase), its output
  // into the Wavetable's, the Wavetable into the mixer.
  DocumentStore s;
  const TrackId t = midiTrack(s);
  run(s, ops::dissolveDevices(*s.current(), s.ids(), &library(), std::vector<DeviceId>{headOf(s, t)}));
  const DeviceId pa = metaOffPath(s, t, "wavetable");
  const DeviceId wt = metaOffPath(s, t, "wavetable");
  const WireId n = cable(s, notesInto(t, pa));
  cable(s, link(pa, wt));
  cable(s, intoMixer(wt, t));
  REQUIRE(mainPath(*s.current(), *findTrackPtr(*s.current(), t)).blocks.empty());
  CHECK(stateOf(*s.current(), n) == WireStatus::Lowered);
  const golden::Channels b = played(*s.current(), "WT → WT off the path, notes cabled");
  // While the notes are held both are the same signal (the run's 5 ms release ramp after note-off is the path's own;
  // off the path the voices end as the engine ends voices without a `done`).
  const double d = maxDiff(a, b, kFirstOff);
  MESSAGE("notes into an off-path meta module vs the run at the note source, before the first note-off: max |diff| = " << d);
  CHECK(rms(b, 0, kFirstOff) > 1e-3);
  CHECK(d <= 1e-6);
  // Into the Wavetable's x (it reads a phase: the note's phase, phasor(Hz)) — the note's phase into the Wavetable alone
  // plays the note (GU.4: it was compared with the Phase accumulator feeding it; the module's own render equality with
  // the meta module is tests/app/test_module_presets.cpp's).
  DocumentStore s2;
  const TrackId t2 = midiTrack(s2);
  run(s2, ops::dissolveDevices(*s2.current(), s2.ids(), &library(), std::vector<DeviceId>{headOf(s2, t2)}));
  const DeviceId wt2 = metaOffPath(s2, t2, "wavetable");
  const WireId n2 = cable(s2, notesInto(t2, wt2));
  cable(s2, intoMixer(wt2, t2));
  CHECK(stateOf(*s2.current(), n2) == WireStatus::Lowered);
  const golden::Channels c = played(*s2.current(), "notes (the phase) into an off-path Wavetable");
  CHECK(rms(c, 0, kFirstOff) > 1e-3);
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
  const DeviceId util = run(g, ops::addDevice(*g.current(), g.ids(), grp, "module:utility", DeviceKind::AudioEffect)).devices.at(0);
  const golden::Channels before = played(*g.current(), "a track in a group with a Utility");
  const WireId loop = cable(g, intoMixer(util, ga));
  CHECK(stateOf(*g.current(), loop) == WireStatus::Delayed);
  const golden::Channels after = played(*g.current(), "… and a cable from the group's Utility into the track's mixer (a loop)");
  CHECK(maxDiff(before, after) > 1e-3);
  CHECK(peak(after) < peak(before) / (1.0f - 0.7072f));
}

