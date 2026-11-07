// IG.1-2 (SPEC §15 2026-11-04 "instrument groups", 2026-11-06 "single-signal cables"; docs/plans/IG.md; lower.h
// "Voice contexts"): what is per note is what sits inside an instrument group. Every case renders against a reference
// built another way — per-voice references are the sum of single-note renders of the same project (a per-note block
// processes each voice on its own), single-signal references are the same blocks outside the group — through the
// interpreter and the JIT (bit for bit equal), every block under the allocation trap (RtScope).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/document/edit_ops.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/serialize.h"
#include "pq/engine/note_list.h"
#include "pq/engine/offline_render.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/graph/infer.h"
#include "pq/meta/registry.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kFrames = 36000;

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  return **lib;
}

using Notes = std::vector<engine::TimedNote>;
// Three notes, the newest the quietest: velocities 0.8, 0.9, 0.5 (sum 2.2, max 0.9, newest 0.5 once all three sound).
Notes chord() {
  return {{0, 24000, 220.0f, 0.8f, 0.5f}, {3000, 21000, 277.18f, 0.9f, 0.5f}, {9000, 15000, 329.63f, 0.5f, 0.5f}};
}
Notes otherChord() { return {{1500, 20000, 196.0f, 0.7f, 0.5f}, {6000, 16000, 246.94f, 0.6f, 0.5f}}; }

// The notes each MIDI track plays (tracks not listed play none).
struct Score {
  std::vector<std::pair<TrackId, Notes>> tracks;
};

Result<LoweredProject> lowered(const Project& p, const Score& score) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  const Score* s = &score;
  lo.slotOf = [s](TrackId t) {
    for (size_t i = 0; i < s->tracks.size(); ++i)
      if (s->tracks[i].first == t) return static_cast<uint32_t>(i);
    return static_cast<uint32_t>(s->tracks.size());
  };
  return lower(p, lo);
}

// `p` rendered for kFrames, every block under the allocation trap (RtScope: the audio thread allocates nothing).
golden::Channels render(const Project& p, const Score& score, engine::UnitBackend backend = {}) {
  auto low = lowered(p, score);
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
  std::vector<std::unique_ptr<engine::NoteListSource>> sources;
  for (const auto& t : score.tracks) sources.push_back(std::make_unique<engine::NoteListSource>(t.second));
  sources.push_back(std::make_unique<engine::NoteListSource>(Notes{}));
  engine::ProjectConfig pc;
  pc.sampleRate = kRate;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 2;
  pc.numProjectParams = static_cast<uint32_t>(values.size());
  for (auto& s : sources) pc.eventSources.push_back(s->source());
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

double maxDiff(const golden::Channels& a, const golden::Channels& b, size_t from = 0, size_t to = SIZE_MAX) {
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c)
    for (size_t i = from; i < std::min(to, a[c].size()); ++i) {
      const double d = std::fabs(static_cast<double>(a[c][i]) - static_cast<double>(b[c][i]));
      m = std::max(m, std::isnan(d) ? 1e9 : d);
    }
  return m;
}
float peak(const golden::Channels& a, size_t from = 0, size_t to = SIZE_MAX) {
  float p = 0.0f;
  for (const auto& c : a)
    for (size_t i = from; i < std::min(to, c.size()); ++i) p = std::max(p, std::fabs(c[i]));
  return p;
}
golden::Channels plus(const golden::Channels& a, const golden::Channels& b) {
  golden::Channels s = a;
  for (size_t c = 0; c < s.size(); ++c)
    for (size_t i = 0; i < s[c].size(); ++i) s[c][i] = static_cast<float>(static_cast<double>(a[c][i]) + b[c][i]);
  return s;
}

// Interpreter and JIT renders: equal bit for bit (checked); the interpreter's returned.
golden::Channels played(const Project& p, const Score& score, const std::string& what) {
  const golden::Channels a = render(p, score);
  const golden::Channels j = render(p, score, compile::jitUnitBackend());
  const double dj = maxDiff(a, j);
  MESSAGE(what << ": interpreter vs JIT max |diff| = " << dj << ", peak " << peak(a));
  CHECK(dj == 0.0);
  return a;
}

// The sum of `p` rendered once per note of `track`'s notes alone: what processing each voice on its own plays.
golden::Channels perVoice(const Project& p, TrackId track, const Notes& notes, const std::string& what) {
  golden::Channels sum;
  for (const engine::TimedNote& n : notes) {
    const golden::Channels one = render(p, Score{{{track, Notes{n}}}});
    sum = sum.empty() ? one : plus(sum, one);
  }
  MESSAGE(what << ": per-voice reference (" << notes.size() << " single-note renders summed), peak " << peak(sum));
  return sum;
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
const Track& trackOf(DocumentStore& s, TrackId t) { return *findTrackPtr(*s.current(), t); }
// A new MIDI track (IG.1-2: an instrument group holding Basic Synth heads its path).
TrackId midi(DocumentStore& s) { return run(s, ops::createTrack(*s.current(), s.ids(), TrackKind::Midi)).tracks.at(0); }
DeviceId groupOf(DocumentStore& s, TrackId t) { return mainPath(*s.current(), trackOf(s, t)).blocks.at(0)->id; }
DeviceId synthOf(DocumentStore& s, TrackId t) {
  for (const DevicePtr& d : trackOf(s, t).devices)
    if (d->group.valid() && d->patch == kDefaultInstrument) return d->id;
  FAIL("no synth");
  return {};
}
WireId cable(DocumentStore& s, Wire w) { return run(s, ops::connect(*s.current(), s.ids(), std::move(w))).wires.at(0); }
Wire link(DeviceId from, std::string port, DeviceId to, std::string param) {
  Wire w;
  w.from = from;
  w.port = std::move(port);
  w.to = to;
  w.param = std::move(param);
  return w;
}
Wire intoMixer(DeviceId from, TrackId mixer, std::string port = {}) {
  Wire w;
  w.from = from;
  w.port = std::move(port);
  w.mixer = mixer;
  return w;
}
Wire notesInto(TrackId t, DeviceId to, std::string port = {}, std::string param = {}) {
  Wire w;
  w.noteSource = t;
  w.port = std::move(port);
  w.to = to;
  w.param = std::move(param);
  return w;
}
// A block added off the path of `t`, inside group `g` when given.
DeviceId block(DocumentStore& s, TrackId t, std::string patch, DeviceId g = {}) {
  const DeviceId d = run(s, ops::addDevice(*s.current(), s.ids(), t, std::move(patch), DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  if (g.valid()) apply(s, SetDeviceGroup{d, g});
  return d;
}
DeviceId metaBlock(DocumentStore& s, TrackId t, std::string_view type, DeviceId g = {}) {
  const DeviceId d = run(s, ops::addMetaDevice(*s.current(), s.ids(), t, type, ops::kOffPath)).devices.at(0);
  if (g.valid()) apply(s, SetDeviceGroup{d, g});
  return d;
}
// The cable from the synth into its group's Out (what createTrack wires).
WireId intoOut(DocumentStore& s, DeviceId g) {
  for (const Wire& w : wiresOf(*s.current()))
    if (w.to == g && w.param == kGroupOutPort) return w.id;
  FAIL("nothing into Out");
  return {};
}
// `d` spliced in before the group's Out: the cable into Out now enters `d`, `d` goes to Out.
void beforeOut(DocumentStore& s, DeviceId g, DeviceId d, bool single = false) {
  const Wire old = *findWire(*s.current(), intoOut(s, g));
  apply(s, RemoveWire{old.id});
  const WireId in = cable(s, link(old.from, old.port, d, ""));
  if (single) run(s, ops::setWireSignal(*s.current(), in, true, WireReducer::Sum));
  cable(s, link(d, "", g, std::string(kGroupOutPort)));
}
WireState stateOf(const Project& p, WireId w, const Score& score) {
  auto low = lowered(p, score);
  REQUIRE(low.ok());
  for (const WireState& st : low->wires)
    if (st.id == w) return st;
  FAIL("no state");
  return {};
}
// The poly of the values a device's flattened nodes compute (`d<id>__…`) in track `t`'s typed patch: Voice when any
// of them runs per voice.
graph::Poly polyOf(const Project& p, const Score& score, TrackId t, DeviceId d) {
  auto low = lowered(p, score);
  REQUIRE(low.ok());
  const graph::Patch* tp = nullptr;
  for (size_t j = 0; j < low->trackIds.size(); ++j)
    if (low->trackIds[j] == t) tp = &low->patches[j]->patch;
  REQUIRE(tp != nullptr);
  graph::InferOptions io;
  io.library = low->graph.library;
  auto typed = graph::infer(*tp, io);
  REQUIRE_MESSAGE(typed.ok(), (typed.ok() ? std::string() : typed.error().message));
  const std::string prefix = "d" + std::to_string(d.value) + "__";
  bool any = false;
  graph::Poly poly = graph::Poly::Global;
  for (graph::NodeIndex n = 0; n < typed->patch.nodeCount(); ++n) {
    if (typed->patch.node(n).kind != graph::NodeKind::Atom || !typed->patch.nodeName(n).starts_with(prefix)) continue;
    any = true;
    for (uint32_t o = 0; o < typed->patch.node(n).numOutputs; ++o)
      if (typed->types[typed->patch.flatOutput(n, o)].poly == graph::Poly::Voice) poly = graph::Poly::Voice;
  }
  CHECK(any);
  return poly;
}

}  // namespace

// ---- the default and the model -------------------------------------------------------------------------------------

TEST_CASE("instrument groups: a new MIDI track holds an instrument group (16 voices, steal oldest); polyphony and a "
          "cable's signal are one undo step each and saved") {
  DocumentStore s;
  const TrackId t = midi(s);
  const DeviceId g = groupOf(s, t);
  CHECK(isGroupDevice(*findDevice(*s.current(), g).device));
  CHECK(isInstrumentGroup(*s.current(), g));
  const GroupPolyphony gp = groupPolyphony(*findDevice(*s.current(), g).device);
  CHECK(gp.voices == kDefaultGroupVoices);
  CHECK(gp.steal == GroupSteal::Oldest);
  CHECK(gp.glide == 0.0);
  CHECK(findDevice(*s.current(), synthOf(s, t)).device->group == g);
  const Score sc{{{t, chord()}}};
  {
    auto low = lowered(*s.current(), sc);
    REQUIRE(low.ok());
    CHECK(low->graph.tracks.front().maxVoices == kDefaultGroupVoices);
  }
  // Polyphony: one step; mono plays one voice; quietest stealing reaches the engine.
  const Snapshot before = s.current();
  GroupPolyphony mono;
  mono.voices = 4;
  mono.mono = true;
  mono.steal = GroupSteal::Quietest;
  const size_t steps = s.history().cursor();
  run(s, ops::setGroupPolyphony(*s.current(), g, mono));
  CHECK(s.history().cursor() == steps + 1);
  {
    auto low = lowered(*s.current(), sc);
    REQUIRE(low.ok());
    CHECK(low->graph.tracks.front().maxVoices == 1);
    CHECK(low->graph.tracks.front().steal == 1);
  }
  REQUIRE(s.undo());
  CHECK(s.current() == before);
  REQUIRE(s.redo());
  // A cable's signal (per note / single-signal + its reducer): one step, saved and read back.
  const WireId w = intoOut(s, g);
  const Snapshot b2 = s.current();
  run(s, ops::setWireSignal(*s.current(), w, true, WireReducer::Highest));
  CHECK(findWire(*s.current(), w)->single);
  CHECK(findWire(*s.current(), w)->reducer == WireReducer::Highest);
  auto text = serializeProject(*s.current());
  REQUIRE(text.ok());
  CHECK(text->find("\"single\": true") != std::string::npos);
  auto back = parseProject(*text);
  REQUIRE(back.ok());
  CHECK(*back->project == *s.current());
  REQUIRE(s.undo());
  CHECK(s.current() == b2);
}

// ---- per note ------------------------------------------------------------------------------------------------------

TEST_CASE("instrument groups: a Saturator inside runs per note (= each note saturated on its own); outside it saturates "
          "the sum") {
  DocumentStore s;
  const TrackId t = midi(s);
  const DeviceId g = groupOf(s, t);
  const DeviceId sat = block(s, t, "saturator", g);
  apply(s, SetDeviceParam{sat, "drive", 18.0});
  beforeOut(s, g, sat);
  const Score sc{{{t, chord()}}};
  CHECK(polyOf(*s.current(), sc, t, sat) == graph::Poly::Voice);
  const golden::Channels got = played(*s.current(), sc, "a chord through a per-note Saturator");
  const golden::Channels ref = perVoice(*s.current(), t, chord(), "each note through it alone");
  const double d = maxDiff(got, ref);
  MESSAGE("per-note Saturator vs the per-voice reference: max |diff| = " << d);
  CHECK(peak(ref) > 1e-2);
  CHECK(d <= 2e-6);
  // The same Saturator outside the group (on the path after it) saturates the chord's sum: not the per-voice sound.
  DocumentStore o;
  const TrackId ot = midi(o);
  const DeviceId os = run(o, ops::addDevice(*o.current(), o.ids(), ot, "saturator", DeviceKind::AudioEffect)).devices.at(0);
  apply(o, SetDeviceParam{os, "drive", 18.0});
  const Score osc{{{ot, chord()}}};
  CHECK(polyOf(*o.current(), osc, ot, os) == graph::Poly::Global);
  const golden::Channels outside = played(*o.current(), osc, "the chord through the Saturator after the group");
  MESSAGE("outside vs per note: max |diff| = " << maxDiff(outside, got));
  CHECK(maxDiff(outside, got) > 1e-2);
}

TEST_CASE("instrument groups: the Envelope last decides each voice's end; without it the voices end 5 ms after "
          "note-off") {
  // In → Phase accumulator → Wavetable [→ Envelope] → Out, the Basic Synth taken out.
  auto build = [](DocumentStore& s, bool envelope) {
    const TrackId t = midi(s);
    const DeviceId g = groupOf(s, t);
    const DeviceId syn = synthOf(s, t);
    const DeviceId del[1] = {syn};
    run(s, ops::deleteDevices(*s.current(), del));
    const DeviceId pa = metaBlock(s, t, "phase_acc", g), wt = metaBlock(s, t, "wavetable", g);
    cable(s, link(g, std::string(kGroupInPort), pa, "hz"));
    cable(s, link(pa, "", wt, ""));
    if (envelope) {
      const DeviceId env = metaBlock(s, t, "envelope", g);
      cable(s, link(wt, "", env, ""));
      cable(s, link(env, "", g, std::string(kGroupOutPort)));
    } else {
      cable(s, link(wt, "", g, std::string(kGroupOutPort)));
    }
    return t;
  };
  const Notes one{{0, 12000, 220.0f, 0.8f, 0.5f}};
  DocumentStore e, n;
  const TrackId te = build(e, true), tn = build(n, false);
  const golden::Channels withEnv = played(*e.current(), Score{{{te, one}}}, "PA → WT → Envelope");
  const golden::Channels without = played(*n.current(), Score{{{tn, one}}}, "PA → WT (no Envelope)");
  // Note-off at 12000: without the Envelope the 5 ms ramp (240 samples) ends the voice; the Envelope's 250 ms release
  // sounds on.
  CHECK(peak(without, 6000, 12000) > 1e-2);
  CHECK(peak(without, 12000 + 240 + kBlock, 24000) == 0.0f);
  CHECK(peak(withEnv, 12000 + 2400, 12000 + 4800) > 1e-3);
  {
    auto low = lowered(*e.current(), Score{{{te, one}}});
    REQUIRE(low.ok());
    const std::string text = patchText(*low->patches.front());
    CHECK(text.find("200 / sr") == std::string::npos);  // no ramp: the Envelope's done ends the voice
  }
  {
    auto low = lowered(*n.current(), Score{{{tn, one}}});
    REQUIRE(low.ok());
    CHECK(patchText(*low->patches.front()).find("200 / sr") != std::string::npos);  // kMetaReleaseSeconds = 5 ms
  }
}

TEST_CASE("instrument groups: a cable leaving the group reduces its voices — sum (default), max, newest") {
  // Inside: each note's velocity (a note output, per voice) through a Utility; the Utility's output leaves the group
  // straight into the track's mixer through the cable's reducer. Out has nothing: the track plays only that cable.
  auto build = [](DocumentStore& s, WireReducer r) {
    const TrackId t = midi(s);
    const DeviceId g = groupOf(s, t);
    apply(s, RemoveWire{intoOut(s, g)});
    const DeviceId u = block(s, t, "utility", g);
    cable(s, notesInto(t, u, "velocity", "x"));
    const WireId out = cable(s, intoMixer(u, t));
    if (r != WireReducer::Sum) run(s, ops::setWireReducer(*s.current(), out, r));
    return t;
  };
  auto level = [](const golden::Channels& c) { return static_cast<double>(c[0][12000]); };  // all three held
  double sum = 0.0, max = 0.0, newest = 0.0;
  for (const WireReducer r : {WireReducer::Sum, WireReducer::Highest, WireReducer::Last}) {
    DocumentStore s;
    const TrackId t = build(s, r);
    const double v = level(played(*s.current(), Score{{{t, chord()}}}, "velocities through the reducer"));
    (r == WireReducer::Sum ? sum : r == WireReducer::Highest ? max : newest) = v;
  }
  MESSAGE("velocities 0.8, 0.9, 0.5 held: sum " << sum << ", max " << max << ", newest " << newest);
  REQUIRE(sum > 1e-3);
  CHECK(max / sum == doctest::Approx(0.9 / 2.2).epsilon(1e-5));
  CHECK(newest / sum == doctest::Approx(0.5 / 2.2).epsilon(1e-5));
}

TEST_CASE("instrument groups: layering — two instrument groups on one track play each its own voices") {
  // Track A: its default group, and a second group off the path fed by A's notes cable (a brighter Basic Synth), its
  // `y` into the mixer. The reference: each group alone.
  auto build = [](DocumentStore& s, bool first, bool second) {
    const TrackId t = midi(s);
    if (!first) apply(s, SetDeviceActive{groupOf(s, t), false});
    if (second) {
      const DeviceId g2 = s.ids().device();
      apply(s, AddDevice{t, makeInstrumentGroup(g2), kAppend});
      const DeviceId syn = run(s, ops::addDevice(*s.current(), s.ids(), t, "BasicSynth", DeviceKind::Instrument, ops::kOffPath)).devices.at(0);
      apply(s, SetDeviceGroup{syn, g2});
      apply(s, SetDeviceParam{syn, "bright", 0.9});
      cable(s, notesInto(t, g2));
      cable(s, link(g2, std::string(kGroupInPort), syn, ""));
      cable(s, link(syn, "", g2, std::string(kGroupOutPort)));
      cable(s, intoMixer(g2, t));
    }
    return t;
  };
  DocumentStore both, a, b;
  const TrackId tb = build(both, true, true), ta = build(a, true, false), t2 = build(b, false, true);
  const golden::Channels got = played(*both.current(), Score{{{tb, chord()}}}, "two instrument groups on one track");
  const golden::Channels ref = plus(played(*a.current(), Score{{{ta, chord()}}}, "the first alone"),
                                    played(*b.current(), Score{{{t2, chord()}}}, "the second alone"));
  const double d = maxDiff(got, ref);
  MESSAGE("layered vs each alone summed: max |diff| = " << d);
  CHECK(peak(ref) > 1e-2);
  CHECK(d <= 1e-6);
}

TEST_CASE("instrument groups: cross-track — a group on B fed by A's notes plays with A's voices; B's path goes on "
          "after it; B modulates it; layering with A's own; a non-Sum reducer across") {
  // The reference: A's own path, its group then a Saturator.
  DocumentStore r;
  const TrackId ra = midi(r);
  const DeviceId rs = run(r, ops::addDevice(*r.current(), r.ids(), ra, "saturator", DeviceKind::AudioEffect)).devices.at(0);
  apply(r, SetDeviceParam{rs, "drive", 9.0});
  apply(r, SetDeviceActive{groupOf(r, ra), true});
  const TrackId rb = midi(r);
  apply(r, SetDeviceActive{groupOf(r, rb), false});
  const golden::Channels ref = played(*r.current(), Score{{{ra, chord()}, {rb, {}}}}, "A: group → Saturator");
  // A plays nothing of its own (its group switched off); B's group is fed by A's notes cable: hosted in A's patch,
  // B's path continues from its output into B's Saturator (a block downstream of the cross-played block).
  DocumentStore s;
  const TrackId a = midi(s);
  apply(s, SetDeviceActive{groupOf(s, a), false});
  const TrackId b = midi(s);
  const DeviceId sb = run(s, ops::addDevice(*s.current(), s.ids(), b, "saturator", DeviceKind::AudioEffect)).devices.at(0);
  apply(s, SetDeviceParam{sb, "drive", 9.0});
  const DeviceId gb = groupOf(s, b);
  const WireId n = cable(s, notesInto(a, gb));
  const Score sc{{{a, chord()}, {b, otherChord()}}};  // (B's own notes: not played — A's cable wins)
  CHECK(stateOf(*s.current(), n, sc).status == WireStatus::Lowered);
  const golden::Channels got = played(*s.current(), sc, "A's notes into B's group, B's Saturator after it");
  const double d = maxDiff(got, ref);
  MESSAGE("cross-played group + B's path vs A's own: max |diff| = " << d);
  CHECK(peak(ref) > 1e-2);
  CHECK(d <= 1e-6);
  // B's LFO modulating the synth inside that group (the home modulating a cross-played block): broadcast into every
  // voice, as an outside signal is — the same as that LFO on B modulating A's own synth.
  auto lfoInto = [](DocumentStore& st, TrackId home, DeviceId synth) {
    const DeviceId lfo = block(st, home, "lfo_device");
    apply(st, SetDeviceParam{lfo, "rate", 3.0});
    apply(st, SetDeviceParam{lfo, "depth", 0.4});
    cable(st, link(lfo, "lfo", synth, "bright"));
  };
  lfoInto(s, b, synthOf(s, b));
  lfoInto(r, rb, synthOf(r, ra));
  const golden::Channels mod = played(*s.current(), sc, "B's LFO into the cross-played synth");
  const golden::Channels modRef = played(*r.current(), Score{{{ra, chord()}, {rb, {}}}}, "B's LFO into A's synth");
  const double dm = maxDiff(mod, modRef);
  MESSAGE("home modulation of a cross-played block vs of A's own: max |diff| = " << dm);
  CHECK(maxDiff(mod, got) > 1e-4);  // the modulation is heard
  CHECK(dm <= 1e-6);
  // Layering across tracks: A's own group back on = A's group + B's group, both A's notes.
  apply(s, SetDeviceActive{groupOf(s, a), true});
  DocumentStore r2;
  const TrackId r2a = midi(r2);
  const golden::Channels aAlone = played(*r2.current(), Score{{{r2a, chord()}}}, "A's own group alone");
  const golden::Channels layered = played(*s.current(), sc, "A's group and B's group, A's notes");
  const double dl = maxDiff(layered, plus(mod, aAlone));
  MESSAGE("cross-track layering vs each alone summed: max |diff| = " << dl);
  CHECK(dl <= 1e-6);
  // A non-Sum reducer across tracks: B's group's per-voice velocity (a Utility inside) into B's mixer by max.
  DocumentStore m;
  const TrackId ma = midi(m);
  apply(m, SetDeviceActive{groupOf(m, ma), false});
  const TrackId mb = midi(m);
  const DeviceId mg = groupOf(m, mb);
  apply(m, RemoveWire{intoOut(m, mg)});
  const DeviceId u = block(m, mb, "utility", mg);
  cable(m, notesInto(ma, mg));
  cable(m, notesInto(ma, u, "velocity", "x"));
  const WireId out = cable(m, intoMixer(u, mb));
  const Score ms{{{ma, chord()}, {mb, {}}}};
  const double sumV = played(*m.current(), ms, "A's velocities summed into B's mixer")[0][12000];
  run(m, ops::setWireReducer(*m.current(), out, WireReducer::Highest));
  const double maxV = played(*m.current(), ms, "A's velocities, max, into B's mixer")[0][12000];
  REQUIRE(sumV > 1e-3);
  CHECK(maxV / sumV == doctest::Approx(0.9 / 2.2).epsilon(1e-5));
}

// ---- single-signal cables (SPEC §15 2026-11-06) --------------------------------------------------------------------

TEST_CASE("instrument groups: an Auto Filter behind a single-signal cable runs once — the same filter outside the "
          "group") {
  DocumentStore s;
  const TrackId t = midi(s);
  const DeviceId g = groupOf(s, t);
  const DeviceId f = block(s, t, "auto_filter", g);
  beforeOut(s, g, f, /*single=*/true);
  const Score sc{{{t, chord()}}};
  CHECK(polyOf(*s.current(), sc, t, f) == graph::Poly::Global);
  const golden::Channels got = played(*s.current(), sc, "synth →(single) Auto Filter → Out");
  DocumentStore o;
  const TrackId ot = midi(o);
  const DeviceId of = run(o, ops::addDevice(*o.current(), o.ids(), ot, "auto_filter", DeviceKind::AudioEffect)).devices.at(0);
  (void)of;
  const golden::Channels ref = played(*o.current(), Score{{{ot, chord()}}}, "group → Auto Filter on the path");
  const double d = maxDiff(got, ref);
  MESSAGE("filter behind a single-signal cable vs outside: max |diff| = " << d);
  CHECK(peak(ref) > 1e-2);
  CHECK(d <= 1e-7);
}

TEST_CASE("instrument groups: a block with a per-note and a single-signal input runs per note; Out sums per-note "
          "terms and adds single-signal ones once") {
  DocumentStore s;
  const TrackId t = midi(s);
  const DeviceId g = groupOf(s, t);
  const DeviceId syn = synthOf(s, t);
  const Score sc{{{t, chord()}}};
  const DeviceId sat2 = block(s, t, "saturator", g);
  cable(s, link(syn, "", sat2, ""));  // per note
  const DeviceId flt = block(s, t, "auto_filter", g);
  const WireId fin = cable(s, link(syn, "", flt, ""));
  run(s, ops::setWireSignal(*s.current(), fin, true, WireReducer::Sum));
  const DeviceId mix = block(s, t, "utility", g);
  cable(s, link(sat2, "", mix, ""));      // per note …
  cable(s, link(flt, "", mix, ""));       // … plus a once-running block: still per note
  CHECK(polyOf(*s.current(), sc, t, sat2) == graph::Poly::Voice);
  CHECK(polyOf(*s.current(), sc, t, flt) == graph::Poly::Global);
  CHECK(polyOf(*s.current(), sc, t, mix) == graph::Poly::Voice);
  // Out with both kinds: the synth per note (createTrack's cable) and the filter once. = the synth group alone + the
  // filter (behind a single-signal cable) alone.
  cable(s, link(flt, "", g, std::string(kGroupOutPort)));
  const golden::Channels both = played(*s.current(), sc, "Out: the voices summed + the filter once");
  DocumentStore a;
  const TrackId ta = midi(a);
  const golden::Channels synthAlone = played(*a.current(), Score{{{ta, chord()}}}, "the synth group alone");
  DocumentStore fo;
  const TrackId tf = midi(fo);
  const DeviceId gf = groupOf(fo, tf);
  const DeviceId ff = block(fo, tf, "auto_filter", gf);
  beforeOut(fo, gf, ff, /*single=*/true);
  const golden::Channels filterAlone = played(*fo.current(), Score{{{tf, chord()}}}, "the filter once alone");
  const double d = maxDiff(both, plus(synthAlone, filterAlone));
  MESSAGE("Out with per-note and single-signal terms vs each alone summed: max |diff| = " << d);
  CHECK(d <= 1e-6);
}

TEST_CASE("instrument groups: a single-signal path back into a per-note block is refused (VoiceLoop, IG.3); loops "
          "inside the group and outside play") {
  DocumentStore s;
  const TrackId t = midi(s);
  const DeviceId g = groupOf(s, t);
  const DeviceId syn = synthOf(s, t);
  const DeviceId del[1] = {syn};
  run(s, ops::deleteDevices(*s.current(), del));
  const DeviceId pa = metaBlock(s, t, "phase_acc", g), wt = metaBlock(s, t, "wavetable", g);
  cable(s, link(g, std::string(kGroupInPort), pa, "hz"));
  cable(s, link(pa, "", wt, ""));
  cable(s, link(wt, "", g, std::string(kGroupOutPort)));
  const Score sc{{{t, chord()}}};
  // Inside, per note: the Wavetable back into the accumulator's hz (FM feedback per voice): delayed, plays.
  const WireId inside = cable(s, link(wt, "", pa, "hz"));
  CHECK(stateOf(*s.current(), inside, sc).status == WireStatus::Delayed);
  // Through a single-signal cable (the voices reduced, a Utility once) back into the accumulator: refused.
  const DeviceId u = block(s, t, "utility", g);
  const WireId toU = cable(s, link(wt, "", u, ""));
  run(s, ops::setWireSignal(*s.current(), toU, true, WireReducer::Sum));
  const WireId back = cable(s, link(u, "", pa, "pm"));
  const WireState st = stateOf(*s.current(), back, sc);
  CHECK(st.status == WireStatus::Refused);
  CHECK(st.reason == WireRefusal::VoiceLoop);
  (void)played(*s.current(), sc, "the group with the refused loop");
  // Outside, single-signal: an effect on the path fed back into itself through a Utility: delayed, plays.
  const DeviceId e1 = run(s, ops::addDevice(*s.current(), s.ids(), t, "utility", DeviceKind::AudioEffect)).devices.at(0);
  const DeviceId e2 = block(s, t, "utility");
  cable(s, link(e1, "", e2, ""));
  const WireId outside = cable(s, link(e2, "", e1, ""));
  CHECK(stateOf(*s.current(), outside, sc).status == WireStatus::Delayed);
  (void)played(*s.current(), sc, "a loop outside the group");
}

// ---- bug 2026-11-03_22-46-24: a cable from the notes source into an Oscilloscope changed the sound -----------------

TEST_CASE("instrument groups: a monitoring cable from the note source (an Oscilloscope) does not change the sound "
          "(bug 2026-11-03_22-46-24)") {
  // The reported track, rebuilt: PA → WT into the mixer, PA → WT → VCA → back into the first PA's phase, the note
  // source's Hz into the second PA's freq; no instrument group. Then the user cabled the note source into an
  // Oscilloscope: that made the track "play voices" and gated / changed everything. Under IG nothing here is inside an
  // instrument group: single-signal, the notes read as the newest note — the scope adds a reader, nothing else.
  auto build = [](DocumentStore& s, bool scope) {
    const TrackId t = midi(s);
    const DeviceId g = groupOf(s, t);
    const DeviceId del[1] = {g};
    run(s, ops::deleteDevices(*s.current(), del));  // (no path: the mixer hears the cables into it)
    const DeviceId pa1 = metaBlock(s, t, "phase_acc"), wt1 = metaBlock(s, t, "wavetable");
    const DeviceId pa2 = metaBlock(s, t, "phase_acc"), wt2 = metaBlock(s, t, "wavetable"), vca = metaBlock(s, t, "vca");
    apply(s, SetDeviceParam{pa1, "freq", 110.0});
    cable(s, link(pa1, "", wt1, ""));
    cable(s, intoMixer(wt1, t));
    cable(s, link(pa2, "", wt2, ""));
    cable(s, link(wt2, "", vca, ""));
    cable(s, link(vca, "", pa1, "phase"));
    cable(s, notesInto(t, pa2, "hz", "freq"));
    if (scope) cable(s, notesInto(t, metaBlock(s, t, "oscilloscope")));
    return t;
  };
  DocumentStore without, with;
  const TrackId a = build(without, false), b = build(with, true);
  const golden::Channels x = played(*without.current(), Score{{{a, chord()}}}, "the reported track");
  const golden::Channels y = played(*with.current(), Score{{{b, chord()}}}, "… with the note source cabled into a scope");
  const double d = maxDiff(x, y);
  MESSAGE("with vs without the monitoring cable: max |diff| = " << d);
  CHECK(peak(x) > 1e-2);
  CHECK(d == 0.0);
}

// ---- the migration: an older file's instrument and meta voice run, wrapped -----------------------------------------

TEST_CASE("instrument groups: an older file's stock instrument and meta voice run load into instrument groups; no "
          "Envelope is added") {
  for (const char* rel : {"app/fixtures/all_free/meta_run", "app/fixtures/all_free/chains"}) {
    CAPTURE(rel);
    auto l = loadProject(std::string(PQ_TEST_DATA_DIR) + "/" + rel);
    REQUIRE(l.ok());
    const Project& p = *l->project;
    const Track& t = *p.tracks.front();
    const MainPath path = mainPath(p, t);
    REQUIRE_FALSE(path.blocks.empty());
    CHECK(isGroupDevice(*path.blocks.front()));
    CHECK(isInstrumentGroup(p, path.blocks.front()->id));
    size_t envelopes = 0, members = 0;
    for (const DevicePtr& d : t.devices) {
      envelopes += metaDeviceType(*d) == "envelope" ? 1u : 0u;
      members += d->group == path.blocks.front()->id ? 1u : 0u;
    }
    CHECK(members >= 1);
    CHECK(envelopes <= 1);  // (meta_run had its own Envelope; nothing is added)
    CHECK(groupPolyphony(*path.blocks.front()).voices == 0);  // the link's default, as before
  }
}
