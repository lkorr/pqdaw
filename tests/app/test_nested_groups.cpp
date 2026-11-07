// GU.2 (docs/plans/GU.md "Phases" 2, "Nesting and instrument groups"; lower.h "NESTED GROUPS", "ATOM AND VALUE BLOCKS"):
// groups nest, atom and value blocks lower inline, block sets paste with fresh ids. Every case renders against a
// reference built another way (the same blocks ungrouped, flat, or the original the copy was made from), through the
// interpreter and the JIT (bit for bit equal), every block under the allocation trap (RtScope).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/library_files.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/serialize.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kFrames = 24000;

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  return **lib;
}

using Notes = std::vector<engine::TimedNote>;
Notes chord() {
  return {{0, 16000, 220.0f, 0.8f, 0.5f}, {3000, 14000, 277.18f, 0.9f, 0.5f}, {9000, 10000, 329.63f, 0.5f, 0.5f}};
}
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

double maxDiff(const golden::Channels& a, const golden::Channels& b) {
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c)
    for (size_t i = 0; i < a[c].size(); ++i) {
      const double d = std::fabs(static_cast<double>(a[c][i]) - static_cast<double>(b[c][i]));
      m = std::max(m, std::isnan(d) ? 1e9 : d);
    }
  return m;
}
float peak(const golden::Channels& a) {
  float p = 0.0f;
  for (const auto& c : a)
    for (const float v : c) p = std::max(p, std::fabs(v));
  return p;
}
// Interpreter and JIT renders: equal bit for bit (checked); the interpreter's returned.
golden::Channels played(const Project& p, const Score& score, const std::string& what) {
  const golden::Channels a = render(p, score);
  const golden::Channels j = render(p, score, compile::jitUnitBackend());
  MESSAGE(what << ": interpreter vs JIT max |diff| = " << maxDiff(a, j) << ", peak " << peak(a));
  CHECK(maxDiff(a, j) == 0.0);
  return a;
}

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
TrackId midi(DocumentStore& s) { return run(s, ops::createTrack(*s.current(), s.ids(), TrackKind::Midi)).tracks.at(0); }
DeviceId groupOf(DocumentStore& s, TrackId t) { return mainPath(*s.current(), trackOf(s, t)).blocks.at(0)->id; }
DeviceId synthOf(DocumentStore& s, TrackId t) {
  for (const DevicePtr& d : trackOf(s, t).devices)
    if (d->group.valid() && d->patch == kDefaultInstrument) return d->id;
  FAIL("no synth");
  return {};
}
WireId cable(DocumentStore& s, DeviceId from, std::string port, DeviceId to, std::string param) {
  Wire w;
  w.from = from;
  w.port = std::move(port);
  w.to = to;
  w.param = std::move(param);
  return run(s, ops::connect(*s.current(), s.ids(), std::move(w))).wires.at(0);
}
WireId intoOut(DocumentStore& s, DeviceId g) {
  for (const Wire& w : wiresOf(*s.current()))
    if (w.to == g && w.param == kGroupOutPort) return w.id;
  FAIL("nothing into Out");
  return {};
}
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

// Basic Synth → mul(·, value 0.5) → tanh → Out, the atoms and the value inside the instrument group `g`.
struct AtomChain {
  DeviceId mul, value, tanh;
};
AtomChain atomChain(DocumentStore& s, TrackId t, DeviceId g) {
  const DeviceId syn = synthOf(s, t);
  apply(s, RemoveWire{intoOut(s, g)});
  AtomChain c;
  c.mul = block(s, t, "atom:mul", g);
  c.value = block(s, t, std::string(kValueDevice), g);
  c.tanh = block(s, t, "atom:tanh", g);
  apply(s, SetDeviceParam{c.value, std::string(kValueParam), 0.5});
  cable(s, syn, "", c.mul, "a");
  cable(s, c.value, "", c.mul, "b");
  cable(s, c.mul, "", c.tanh, "x");
  cable(s, c.tanh, "", g, std::string(kGroupOutPort));
  return c;
}

// A canonical form of a block set: fresh ids from one fixed start (remapBlocks is deterministic), so two sets equal
// up to their ids compare equal.
BlockSet canonical(const BlockSet& b) {
  IdSource ids;
  ids.reserveThrough(1u << 20);
  return ops::remapBlocks(b, ids);
}

}  // namespace

TEST_CASE("nested groups: atom blocks and a value block lower inline — a group of them renders as the same blocks "
          "ungrouped, nesting them (Ctrl+G inside the capsule) and ungrouping give back the very document and sound") {
  DocumentStore s;
  const TrackId t = midi(s);
  const DeviceId g = groupOf(s, t);
  const AtomChain c = atomChain(s, t, g);
  const Score sc{{{t, chord()}}};
  const Snapshot flat = s.current();
  const golden::Channels ref = played(*flat, sc, "Basic Synth → mul(·, value 0.5) → tanh, ungrouped in the instrument group");
  CHECK(peak(ref) > 1e-2);
  // The atom blocks are the atoms themselves: nothing of a user patch, no generated text.
  {
    auto low = lowered(*flat, sc);
    REQUIRE(low.ok());
    CHECK(low->userDevices.empty());
    const std::string text = patchText(*low->patches.front());
    CHECK(text.find("Atom_mul") != std::string::npos);
    CHECK(text.find("Atom_tanh") != std::string::npos);
  }
  // Nest mul + value + tanh into a group inside the instrument group: one step, wires untouched.
  const DeviceId sel[] = {c.mul, c.value, c.tanh};
  const size_t steps = s.history().cursor();
  const ops::Edit nest = run(s, ops::groupInCircuit(s.current(), s.ids(), library(), sel));
  CHECK(s.history().cursor() == steps + 1);
  const DeviceId inner = nest.devices.at(0);
  const Device* in = findDevice(*s.current(), inner).device;
  REQUIRE(in != nullptr);
  CHECK(isGroupDevice(*in));
  CHECK(in->group == g);
  CHECK(findDevice(*s.current(), c.mul).device->group == inner);
  CHECK(wiresOf(*s.current()) == wiresOf(*flat));
  // Only the outermost group fed notes is the instrument: the nested one is a member of it, as are its members.
  const VoiceContexts vc = voiceContexts(*s.current());
  CHECK(vc.list.size() == 1);
  REQUIRE(vc.blockOf(g) != nullptr);
  CHECK(vc.blockOf(inner) == nullptr);
  CHECK(vc.memberOf(inner) == vc.blockOf(g));
  CHECK(vc.memberOf(c.tanh) == vc.blockOf(g));
  const golden::Channels nested = played(*s.current(), sc, "the same blocks nested in a group inside it");
  MESSAGE("nested vs ungrouped: max |diff| = " << maxDiff(nested, ref));
  CHECK(maxDiff(nested, ref) == 0.0);
  // Ungroup (Ctrl+Shift+G): the members go back to the instrument group, the very document.
  run(s, ops::ungroupInCircuit(s.current(), s.ids(), library(), inner));
  CHECK(*s.current() == *flat);
  CHECK(maxDiff(played(*s.current(), sc, "ungrouped again"), ref) == 0.0);
  // Undo / redo of the nest.
  REQUIRE(s.undo());
  CHECK(findDevice(*s.current(), c.mul).device->group == inner);
  REQUIRE(s.undo());
  CHECK(*s.current() == *flat);
}

TEST_CASE("nested groups: atoms on the main path grouped by Ctrl+G render bit for bit as the atoms in series") {
  // Source → instrument group [Basic Synth] → mul(·, value) → tanh → mixer: the two atoms on the path after the group,
  // then the two grouped (a series: In → mul → tanh → Out as chain links).
  DocumentStore s;
  const TrackId t = midi(s);
  const DeviceId mul = run(s, ops::addDevice(*s.current(), s.ids(), t, "atom:mul", DeviceKind::AudioEffect)).devices.at(0);
  const DeviceId tanh = run(s, ops::addDevice(*s.current(), s.ids(), t, "atom:tanh", DeviceKind::AudioEffect)).devices.at(0);
  const DeviceId value = block(s, t, std::string(kValueDevice));
  apply(s, SetDeviceParam{value, std::string(kValueParam), 0.75});
  cable(s, value, "", mul, "b");
  {
    const MainPath path = mainPath(*s.current(), trackOf(s, t));
    REQUIRE(path.blocks.size() == 3);
    CHECK(path.blocks[1]->id == mul);
    CHECK(path.blocks[2]->id == tanh);
  }
  const Score sc{{{t, chord()}}};
  const Snapshot series = s.current();
  const golden::Channels ref = played(*series, sc, "atoms in series on the path");
  CHECK(peak(ref) > 1e-2);
  const DeviceId sel[] = {mul, tanh};
  const ops::Edit grp = run(s, ops::groupInCircuit(s.current(), s.ids(), library(), sel));
  const golden::Channels grouped = played(*s.current(), sc, "the atoms grouped");
  MESSAGE("grouped vs series: max |diff| = " << maxDiff(grouped, ref));
  CHECK(maxDiff(grouped, ref) == 0.0);
  run(s, ops::ungroupInCircuit(s.current(), s.ids(), library(), grp.devices.at(0)));
  CHECK(*s.current() == *series);
}

TEST_CASE("nested groups: an instrument group holding a nested group (wired through its terminals) voices once — the "
          "render equals the flat instrument group's, one voice context, the Envelope behind the nested Out ends the "
          "voices") {
  // Flat: In → Phase accumulator → Wavetable → Envelope → Out. Nested: In → PA → [N: x → In_N → Wavetable → Out_N → y]
  // → Envelope → Out, N a plain group inside the instrument group.
  auto build = [](DocumentStore& s, bool nested) {
    const TrackId t = midi(s);
    const DeviceId g = groupOf(s, t);
    const DeviceId del[1] = {synthOf(s, t)};
    run(s, ops::deleteDevices(*s.current(), del));
    const DeviceId pa = metaBlock(s, t, "phase_acc", g);
    const DeviceId env = metaBlock(s, t, "envelope", g);
    cable(s, g, std::string(kGroupInPort), pa, "hz");
    if (nested) {
      const DeviceId n = block(s, t, std::string(kGroupDevice));
      apply(s, SetDeviceGroup{n, g});
      const DeviceId wt = metaBlock(s, t, "wavetable", n);
      cable(s, pa, "", n, "x");
      cable(s, n, std::string(kGroupInPort), wt, "");
      cable(s, wt, "", n, std::string(kGroupOutPort));
      cable(s, n, "y", env, "");
    } else {
      const DeviceId wt = metaBlock(s, t, "wavetable", g);
      cable(s, pa, "", wt, "");
      cable(s, wt, "", env, "");
    }
    cable(s, env, "", g, std::string(kGroupOutPort));
    return t;
  };
  DocumentStore f, n;
  const TrackId tf = build(f, false), tn = build(n, true);
  const VoiceContexts vc = voiceContexts(*n.current());
  CHECK(vc.list.size() == 1);
  const Score sf{{{tf, chord()}}}, sn{{{tn, chord()}}};
  const golden::Channels flat = played(*f.current(), sf, "flat instrument group (PA → WT → Envelope)");
  const golden::Channels nest = played(*n.current(), sn, "the Wavetable in a nested group (through its terminals)");
  CHECK(peak(flat) > 1e-2);
  MESSAGE("nested vs flat: max |diff| = " << maxDiff(nest, flat));
  CHECK(maxDiff(nest, flat) == 0.0);
  {
    auto low = lowered(*n.current(), sn);
    REQUIRE(low.ok());
    size_t ti = low->trackIds.size();
    for (size_t j = 0; j < low->trackIds.size(); ++j)
      if (low->trackIds[j] == tn) ti = j;
    REQUIRE(ti < low->graph.tracks.size());
    CHECK(low->graph.tracks[ti].maxVoices == kDefaultGroupVoices);  // the instrument group's, once
    const std::string text = patchText(*low->patches[ti]);
    CHECK(text.find("GroupVoice") != std::string::npos);  // the nested group passes its signals per voice
    CHECK(text.find("200 / sr") == std::string::npos);    // no 5 ms ramp: the Envelope behind the nested Out ends them
  }
  // A group fed notes inside a PLAIN group is an instrument group of its own (the outermost one fed notes).
  {
    DocumentStore s;
    const TrackId t = midi(s);
    const DeviceId outer = block(s, t, std::string(kGroupDevice));
    const DeviceId g = groupOf(s, t);
    (void)g;
    const DeviceId ig = block(s, t, std::string(kGroupDevice), outer);
    Wire w;
    w.noteSource = t;
    w.to = ig;
    run(s, ops::connect(*s.current(), s.ids(), w));
    const VoiceContexts v = voiceContexts(*s.current());
    CHECK(v.blockOf(ig) != nullptr);
    CHECK(v.blockOf(outer) == nullptr);
  }
}

TEST_CASE("nested groups: copy / paste of an instrument group with a nested group — fresh ids, the copy renders as "
          "the original, undo removes it all; a group preset file round trips") {
  DocumentStore s;
  const TrackId a = midi(s);
  const DeviceId g = groupOf(s, a);
  const AtomChain c = atomChain(s, a, g);
  const DeviceId sel[] = {c.mul, c.value, c.tanh};
  const DeviceId inner = run(s, ops::groupInCircuit(s.current(), s.ids(), library(), sel)).devices.at(0);
  // The copy: the group with everything inside it (any depth) and the wires among them; no note source or mixer.
  const DeviceId one[] = {g};
  Result<BlockSet> copied = ops::copyBlocks(*s.current(), one);
  REQUIRE(copied.ok());
  CHECK(copied->devices.size() == 6);  // the group, Basic Synth, the nested group, mul, value, tanh
  for (const Wire& w : copied->wires) {
    CHECK_FALSE(w.fromNoteSource());
    CHECK_FALSE(w.toMixer());
  }
  CHECK(copied->devices.front()->id == g);
  CHECK_FALSE(copied->devices.front()->group.valid());
  // Into a second MIDI track whose own group is deleted; its notes cable into the copy's main input.
  const TrackId b = midi(s);
  const DeviceId bg[1] = {groupOf(s, b)};
  run(s, ops::deleteDevices(*s.current(), bg));
  const Snapshot beforePaste = s.current();
  const size_t steps = s.history().cursor();
  const ops::Edit pasted = run(s, ops::pasteBlocks(*s.current(), s.ids(), b, *copied));
  CHECK(s.history().cursor() == steps + 1);
  REQUIRE(pasted.devices.size() == 6);
  const DeviceId g2 = pasted.devices.front();
  // Every id is fresh and every reference follows the copies.
  std::vector<DeviceId> old;
  for (const DevicePtr& d : copied->devices) old.push_back(d->id);
  for (const DeviceId id : pasted.devices) CHECK(std::find(old.begin(), old.end(), id) == old.end());
  const Track& tb = trackOf(s, b);
  for (const DeviceId id : pasted.devices) {
    const Device* d = findDevice(*s.current(), id).device;
    REQUIRE(d != nullptr);
    if (id == g2) continue;
    CHECK(groupContains(tb.devices, g2, id));
  }
  for (const WireId w : pasted.wires) {
    const Wire* x = findWire(*s.current(), w);
    REQUIRE(x != nullptr);
    CHECK(std::find(pasted.devices.begin(), pasted.devices.end(), x->from) != pasted.devices.end());
    CHECK(std::find(pasted.devices.begin(), pasted.devices.end(), x->to) != pasted.devices.end());
  }
  CHECK(pasted.wires.size() == copied->wires.size());
  {
    const DeviceId two[] = {g2};
    Result<BlockSet> again = ops::copyBlocks(*s.current(), two);
    REQUIRE(again.ok());
    CHECK(canonical(*again) == canonical(*copied));
  }
  Wire notes;
  notes.noteSource = b;
  notes.to = g2;
  notes.param = "x";
  run(s, ops::connect(*s.current(), s.ids(), notes));
  Wire out;
  out.from = g2;
  out.port = "y";
  out.mixer = b;
  run(s, ops::connect(*s.current(), s.ids(), out));
  CHECK(isInstrumentGroup(*s.current(), g2));
  CHECK(voiceContexts(*s.current()).memberOf(pasted.devices.at(2)) != nullptr);
  const golden::Channels orig = played(*s.current(), Score{{{a, chord()}}}, "the original on track A");
  const golden::Channels copy = played(*s.current(), Score{{{b, chord()}}}, "the pasted copy on track B");
  CHECK(peak(orig) > 1e-2);
  MESSAGE("copy vs original: max |diff| = " << maxDiff(copy, orig));
  CHECK(maxDiff(copy, orig) == 0.0);
  // Undo: the paste (and the two cables) leave nothing behind.
  REQUIRE(s.undo());
  REQUIRE(s.undo());
  REQUIRE(s.undo());
  CHECK(*s.current() == *beforePaste);
  // A group preset holds the same block set: written, read back equal; pasted it is the same group again.
  auto text = serializeGroupPreset(*copied, "Atoms");
  REQUIRE(text.ok());
  auto back = parsePreset(*text, "Atoms.pqpreset");
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(back->groupPreset());
  CHECK(back->name == "Atoms");
  CHECK(back->blocks == *copied);
  CHECK(back->device->id == g);
  CHECK(inner.valid());
}
