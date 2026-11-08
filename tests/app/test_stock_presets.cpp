// GU.10 (docs/plans/GU.md "One hierarchy"): the stock devices are group presets (pq/document/modules.h), the default
// instrument too. Light render checks, each interpreter == JIT:
//   default    a new MIDI track (createTrack: the "Basic Synth" instrument group, PA → Wavetable → Envelope) plays a
//              chord: not silent, below 0 dBFS, and the voices end after the release
//   Utility    after the default instrument, at gain −6 dB, width 50 %, balance 0.3: the reference render
//              tests/golden/audio/gu10_utility.wav (GU.11: recorded from the preset, which GU.10 checked against the
//              retired patch's stand-in copy within 1e-5 of its peak before the stand-ins were deleted)
//   Saturator  drive +12 dB, Analog Clip and Medium Curve: gu10_saturator_type0.wav / _type3.wav (the same way)
//   LFO        its output into an audio track's mixer is the 1 Hz sine it should be
//   GU.f4      an undriven menu / switch choosing a branch lowers as a literal: the optimised program holds only the
//              chosen curve / shape; changing it recompiles and plays the new choice
//   GU.f7      a muted rack chain (its Gate a literal 0) is not in the optimised program
//   Operator   A alone is the default instrument's chain at level 1 (4 × the default's 0.25); B modulating A changes
//              the sound and stays finite
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/engine/wav_file.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/modules.h"
#include "pq/document/racks.h"
#include "pq/engine/project_compile.h"
#include "pq/graph/atoms.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kBlocks = 1200;  // 1.6 s: the chord (1 s at 120 BPM: two beats) and its release

const DeviceLibrary& library() { return playback_fixture::unitsLibrary(); }

golden::Channels render(const Snapshot& s, bool jit, uint32_t blocks = kBlocks) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
  c.sampleRate = kRate;
  auto l = app::ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  app::ProjectLink& link = **l;
  REQUIRE(link.waitForMetaTables());
  if (jit) REQUIRE(link.waitForJit(180.0));
  link.play(0);
  golden::Channels out(2);
  std::vector<float> L(kBlock), R(kBlock);
  int64_t t = 0;
  for (uint32_t b = 0; b < blocks; ++b) {
    float* ch[2] = {L.data(), R.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
    }
    t += kBlock;
    out[0].insert(out[0].end(), L.begin(), L.end());
    out[1].insert(out[1].end(), R.begin(), R.end());
  }
  CHECK(link.lastError().empty());
  return out;
}

double maxDiff(const golden::Channels& a, const golden::Channels& b, double scaleB = 1.0) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = 0; i < a[c].size(); ++i) m = std::max(m, std::fabs(static_cast<double>(a[c][i]) - scaleB * b[c][i]));
  }
  return m;
}
double peakOf(const golden::Channels& a, size_t from = 0, size_t to = SIZE_MAX) {
  double p = 0.0;
  for (const auto& c : a)
    for (size_t i = from; i < std::min(to, c.size()); ++i) p = std::max(p, static_cast<double>(std::fabs(c[i])));
  return p;
}

bool updateGoldens() {
  const char* v = std::getenv("PQ_UPDATE_GOLDENS");
  return v != nullptr && *v != '\0' && *v != '0';
}

// GU.11: `out` against the reference render `name` (tests/golden/audio/<name>.wav, float): within 1e-5 of its peak.
// PQ_UPDATE_GOLDENS=1 writes `out` as it.
void againstReference(const golden::Channels& out, const char* name) {
  const std::filesystem::path wav = golden::goldenDir() / "audio" / (std::string(name) + ".wav");
  if (updateGoldens()) REQUIRE(engine::writeWav(wav, out, static_cast<uint32_t>(kRate)).ok());
  auto gold = engine::readWav(wav);
  REQUIRE_MESSAGE(gold.ok(), (gold.ok() ? std::string() : gold.error().message + " (PQ_UPDATE_GOLDENS=1)"));
  const golden::Channels y = gold->channels;
  const double peak = peakOf(y);
  CHECK(peak > 0.02);
  CHECK(maxDiff(out, y) <= 1e-5 * std::max(peak, 1.0));
}

// Renders `s` with the interpreter and the JIT; they agree (float rounding of the same program).
golden::Channels both(const Snapshot& s) {
  const golden::Channels interp = render(s, false), jit = render(s, true);
  const double peak = peakOf(interp);
  CHECK(maxDiff(interp, jit) <= 1e-5 * std::max(peak, 1.0));
  return interp;
}

struct Song {
  DocumentStore store;
  TrackId keys;
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  // A MIDI track (its default instrument) playing a three-note chord for two beats.
  Song() {
    store.setDeviceLibrary(&library());
    keys = edit(ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Keys")).tracks.at(0);
    std::vector<Note> notes;
    for (const int32_t d : {0, 4, 7}) notes.push_back(playback_fixture::note(store.ids(), 0, 2 * kPpq, d));
    Batch b{"clip", {}};
    b.commands.push_back(AddClip{keys, playback_fixture::clip(store.ids(), 0, 4 * kPpq, notes, "chord")});
    REQUIRE(store.dispatch(b).ok());
  }
  // Pastes `set` (a preset) onto Keys and puts its group on the path after the instrument; returns the group.
  DeviceId insert(const BlockSet& set, uint32_t slot = 1) {
    const DeviceId g = edit(ops::pasteBlocks(*store.current(), store.ids(), keys, set)).devices.at(0);
    edit(ops::insertOnPath(*store.current(), store.ids(), g, slot));
    return g;
  }
  // The block named `name` inside group `g` (any depth).
  DeviceId named(DeviceId g, std::string_view name) const {
    const Track& t = *findTrackPtr(*store.current(), keys);
    for (const Device* d : groupMembersDeep(t.devices, g))
      if (d->name == name) return d->id;
    FAIL("no block " << std::string(name));
    return {};
  }
  // Sets value block `v` to `value` in its curve's units (dB for a gain).
  void set(DeviceId v, double value) {
    const Device& d = *findDevice(*store.current(), v).device;
    REQUIRE(d.curve(kValueParam) != nullptr);
    const dsp::Curve& c = *d.curve(kValueParam);
    REQUIRE(store.dispatch(SetDeviceParam{v, std::string(kValueParam), dsp::curvePosition(c, dsp::curveOutput(c, value), 0.0)}).ok());
  }
};

}  // namespace

TEST_CASE("GU.10 default instrument: a new MIDI track plays its group preset (not silent, below 0 dBFS, voices end)") {
  Song s;
  const golden::Channels out = both(s.store.current());
  const size_t beat = static_cast<size_t>(kRate / 2.0);  // 120 BPM
  const double during = peakOf(out, 0, 2 * beat);
  CHECK(during > 0.05);
  CHECK(during < 1.0);
  // The release (0.25 s) is over well before the end: the last 0.2 s are silent.
  CHECK(peakOf(out, out[0].size() - static_cast<size_t>(0.2 * kRate)) < 1e-4);
}

TEST_CASE("GU.10 Utility: the preset is the retired patch's matrix (gain, width, balance), interpreter == JIT") {
  Song a;
  const DeviceId u = a.insert(utilityBlocks(a.store.ids()));
  a.set(a.named(u, "Gain"), -6.0);
  a.set(a.named(u, "Width"), 0.5);
  a.set(a.named(u, "Balance"), 0.3);
  const golden::Channels x = both(a.store.current());
  againstReference(x, "gu10_utility");
  // Balance 0.3: the left side is quieter than the right.
  CHECK(peakOf(golden::Channels{x[0]}) < peakOf(golden::Channels{x[1]}));
}

TEST_CASE("GU.10 Saturator: drive into Analog Clip and Medium Curve as the retired patch, interpreter == JIT") {
  for (const double type : {0.0, 3.0}) {
    CAPTURE(type);
    Song a;
    const DeviceId sat = a.insert(saturatorBlocks(a.store.ids()));
    a.set(a.named(sat, "Drive"), 12.0);
    a.set(a.named(sat, "Type"), type);
    const golden::Channels x = both(a.store.current());
    CHECK(peakOf(x) > 0.1);
    CHECK(peakOf(x) <= 1.0 + 1e-6);  // both curves saturate at 1
    againstReference(x, type == 0.0 ? "gu10_saturator_type0" : "gu10_saturator_type3");
  }
}

TEST_CASE("GU.10 LFO: its output is a 1 Hz sine (depth 100 %, offset 0), interpreter == JIT") {
  DocumentStore store;
  auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Audio, {}, "Mod");
  REQUIRE(t.ok());
  REQUIRE(store.dispatch(t->command).ok());
  const TrackId mod = t->tracks.at(0);
  auto e = ops::pasteBlocks(*store.current(), store.ids(), mod, lfoBlocks(store.ids()));
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  const DeviceId lfo = e->devices.at(0);
  Wire w;
  w.from = lfo;
  w.port = std::string(kMainPort);
  w.mixer = mod;
  auto c = ops::connect(*store.current(), store.ids(), w);
  REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
  REQUIRE(store.dispatch(c->command).ok());
  const golden::Channels out = both(store.current());
  // One cycle per second: rising through 0 near the start, falling through 0 near 0.5 s, peaks ±1 at 0.25 / 0.75 s.
  const auto at = [&](double sec) { return out[0][static_cast<size_t>(sec * kRate)]; };
  CHECK(at(0.25) > 0.99);
  CHECK(at(0.75) < -0.99);
  size_t fall = 0;
  for (size_t i = static_cast<size_t>(0.3 * kRate); i < static_cast<size_t>(0.7 * kRate) && fall == 0; ++i)
    if (out[0][i - 1] > 0.0f && out[0][i] <= 0.0f) fall = i;
  CHECK(std::fabs(static_cast<double>(fall) / kRate - 0.5) < 0.003);  // (within a few blocks: the mixer's ramp in)
  CHECK(maxDiff(golden::Channels{out[0]}, golden::Channels{out[1]}) == 0.0);  // mono into both sides
}

TEST_CASE("GU.10 Operator: A alone is the default chain at level 1; B modulating A changes it; interpreter == JIT") {
  Song plain, op;
  // Operator in place of the default instrument (its group and blocks dissolved off the path first).
  {
    const Track& t = *findTrackPtr(*op.store.current(), op.keys);
    DeviceId g;
    std::vector<DeviceId> inside;
    for (const DevicePtr& d : t.devices)
      if (isGroupDevice(*d) && !d->group.valid()) g = d->id;
    for (const Device* m : groupMembersDeep(t.devices, g)) inside.push_back(m->id);
    op.edit(ops::deleteDevices(*op.store.current(), inside));
    const DeviceId gs[1] = {g};
    op.edit(ops::dissolveDevices(*op.store.current(), op.store.ids(), &library(), gs));
  }
  const DeviceId opGroup = op.insert(operatorBlocks(op.store.ids()), 0);
  const golden::Channels a = both(op.store.current()), base = render(plain.store.current(), false);
  CHECK(peakOf(base) > 0.02);
  // A alone is 4 × the default chain (its `+ pm` adds B's 0), compared in loudness window by window (release included).
  const size_t w = 1024;
  double worst = 0.0;
  for (size_t at = 0; at + w <= a[0].size(); at += w) {
    double ea = 0.0, eb = 0.0;
    for (size_t i = at; i < at + w; ++i) {
      ea += static_cast<double>(a[0][i]) * a[0][i];
      eb += 16.0 * static_cast<double>(base[0][i]) * base[0][i];
    }
    const double ra = std::sqrt(ea / w), rb = std::sqrt(eb / w);
    if (rb > 0.01 * peakOf(a)) worst = std::max(worst, std::fabs(ra - rb) / rb);
  }
  CHECK(worst < 0.03);
  // B (coarse 2) at level 0.5 into A's phase: FM — a different sound, still finite and bounded by A's level.
  const DeviceId bEnv = [&] {
    const Track& t = *findTrackPtr(*op.store.current(), op.keys);
    for (const Device* d : groupMembersDeep(t.devices, op.named(opGroup, "Envelope B")))
      if (d->patch == "meta:envelope") return d->id;
    return DeviceId{};
  }();
  REQUIRE(bEnv.valid());
  op.edit(ops::setDeviceParamValue(*op.store.current(), library(), bEnv, "amount", 0.5));
  op.set(op.named(opGroup, "Coarse B"), 2.0);
  const golden::Channels fm = both(op.store.current());
  CHECK(maxDiff(fm, a) > 0.01);
  CHECK(peakOf(fm) <= peakOf(a) * 1.001 + 1e-6);
}

// ---- GU.f4: structural value blocks (lower.h "Structural value blocks") ---------------------------------------------
namespace {

// Atom name -> count over every optimised unit of `s` (lowered and compiled as the link does, without resources).
std::map<std::string, int> atomsOf(const Snapshot& s) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [](TrackId) { return 0u; };
  auto low = lower(*s, lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  engine::ProjectCompiler compiler(co);
  auto build = compiler.build(low->graph);
  REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
  std::map<std::string, int> out;
  for (uint32_t u = 0; u < compiler.numUnits(); ++u)
    for (const graph::Node& n : compiler.unit(u).typed->patch.nodes())
      if (n.kind == graph::NodeKind::Atom) ++out[std::string(graph::atomSignature(n.atom).name)];
  return out;
}

using MakeBlocks = BlockSet (*)(IdSource&, const StockModuleOptions&);

// An audio track with the preset `make` builds on its path (or, `toMixer`, its group's output wired into the mixer).
struct Bench {
  DocumentStore store;
  TrackId track;
  DeviceId group;
  Bench(MakeBlocks make, bool toMixer) {
    store.setDeviceLibrary(&library());
    auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Audio, {}, "A");
    REQUIRE(t.ok());
    REQUIRE(store.dispatch(t->command).ok());
    track = t->tracks.at(0);
    if (make == nullptr) return;
    auto e = ops::pasteBlocks(*store.current(), store.ids(), track, make(store.ids(), StockModuleOptions{}));
    REQUIRE(e.ok());
    REQUIRE(store.dispatch(e->command).ok());
    group = e->devices.at(0);
    if (toMixer) {
      Wire w;
      w.from = group;
      w.port = std::string(kMainPort);
      w.mixer = track;
      auto c = ops::connect(*store.current(), store.ids(), w);
      REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
      REQUIRE(store.dispatch(c->command).ok());
    } else {
      auto c = ops::insertOnPath(*store.current(), store.ids(), group, 0);
      REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
      REQUIRE(store.dispatch(c->command).ok());
    }
  }
  DeviceId named(std::string_view name) const {
    const Track& t = *findTrackPtr(*store.current(), track);
    for (const Device* d : groupMembersDeep(t.devices, group))
      if (d->name == name) return d->id;
    FAIL("no block " << std::string(name));
    return {};
  }
  bool literal(std::string_view name) const {
    return lowersAsLiteral(*store.current(), *findDevice(*store.current(), named(name)).device);
  }
  void set(std::string_view name, double value) {
    const DeviceId v = named(name);
    const dsp::Curve& c = *findDevice(*store.current(), v).device->curve(kValueParam);
    REQUIRE(store.dispatch(SetDeviceParam{v, std::string(kValueParam), dsp::curvePosition(c, dsp::curveOutput(c, value), 0.0)}).ok());
  }
  // `atom`'s count in this project's program minus an empty audio track's.
  int count(std::string_view atom) const {
    static const std::map<std::string, int> base = [] {
      const Bench empty(nullptr, false);
      return atomsOf(empty.store.current());
    }();
    const std::map<std::string, int> m = atomsOf(store.current());
    const auto at = [&](const std::map<std::string, int>& x) {
      const auto it = x.find(std::string(atom));
      return it == x.end() ? 0 : it->second;
    };
    return at(m) - at(base);
  }
};

}  // namespace

TEST_CASE("GU.f4 Saturator: only the chosen curve is compiled (Type and Soft Clip lower as literals)") {
  Bench b(&saturatorBlocks, false);
  CHECK(b.literal("Type"));
  CHECK(b.literal("Soft Clip"));
  CHECK_FALSE(b.literal("Drive"));  // not stepped
  // Analog Clip (the default): its clip and c^3, nothing of the six others; no mux left, no soft-clip tanh.
  CHECK(b.count("mux") == 0);
  CHECK(b.count("clip") == 1);
  CHECK(b.count("fast_tanh") == 0);
  CHECK(b.count("sine") == 0);
  CHECK(b.count("sqrt") == 0);
  CHECK(b.count("div") == 0);
  CHECK(b.count("abs") == 0);
  b.set("Type", 4.0);  // Hard Curve: v / sqrt(sqrt(1 + v^4))
  CHECK(b.count("sqrt") == 2);
  CHECK(b.count("div") == 1);
  CHECK(b.count("clip") == 0);
  CHECK(b.count("mux") == 0);
  b.set("Type", 3.0);  // Medium Curve, and Soft Clip on: two tanh
  b.set("Soft Clip", 1.0);
  CHECK(b.count("fast_tanh") == 2);
  CHECK(b.count("sqrt") == 0);
  CHECK(b.count("mux") == 0);
  // Its param is a literal of the patch: no runtime param for it.
  const ParamRef type = ParamRef::device(b.named("Type"), kValueParam);
  for (const LoweredParam& p : lowerParams(*b.store.current(), false, library())) CHECK(p.ref != type);
}

TEST_CASE("GU.f4 LFO: only the chosen shape and phase path are compiled") {
  Bench b(&lfoBlocks, true);
  CHECK(b.literal("Shape"));
  CHECK(b.literal("Sync"));
  CHECK(b.count("sine") == 1);  // Sine, free-running
  CHECK(b.count("mux") == 0);
  CHECK(b.count("transport.beat") == 0);
  CHECK(b.count("phasor") == 1);
  b.set("Shape", 4.0);  // Square: its own mux on the phase
  b.set("Sync", 1.0);   // synced: the song position, no phasor
  CHECK(b.count("sine") == 0);
  CHECK(b.count("mux") == 1);
  CHECK(b.count("transport.beat") == 1);
  CHECK(b.count("phasor") == 0);
}

TEST_CASE("GU.f4 a driven menu stays a runtime choice (a wire into it: every branch compiled)") {
  Bench b(&saturatorBlocks, false);
  const DeviceId type = b.named("Type"), drive = b.named("Drive");
  Wire w;
  w.from = drive;
  w.port = std::string(kValueOutput);
  w.to = type;
  w.param = std::string(kValueParam);
  auto c = ops::connect(*b.store.current(), b.store.ids(), w);
  REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
  REQUIRE(b.store.dispatch(c->command).ok());
  CHECK_FALSE(b.literal("Type"));
  CHECK(b.count("mux") >= 6);
  CHECK(b.count("sqrt") == 2);
}

TEST_CASE("GU.f4 Saturator: changing Type recompiles and plays the new curve (interpreter and JIT)") {
  for (const bool jit : {false, true}) {
    CAPTURE(jit);
    Song a;
    const DeviceId sat = a.insert(saturatorBlocks(a.store.ids()));
    a.set(a.named(sat, "Drive"), 12.0);
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = jit;
    c.sampleRate = kRate;
    auto l = app::ProjectLink::create(c, a.store.current());
    REQUIRE(l.ok());
    app::ProjectLink& link = **l;
    [[maybe_unused]] const ListenerId sub = a.store.subscribe([&](const StoreEvent& e) { link.onDocumentChange(e); });
    REQUIRE(link.waitForMetaTables());
    const auto s0 = link.stats();
    a.set(a.named(sat, "Type"), 3.0);  // structural: one compile
    link.flush();
    CHECK(link.stats().compiles == s0.compiles + 1);
    a.set(a.named(sat, "Output"), 1.0);  // a smooth knob stays a param write (no compile)
    a.set(a.named(sat, "Output"), 0.0);
    link.flush();
    CHECK(link.stats().compiles == s0.compiles + 1);
    REQUIRE(link.waitForMetaTables());
    if (jit) REQUIRE(link.waitForJit(180.0));
    link.play(0);
    golden::Channels out(2);
    std::vector<float> L(kBlock), R(kBlock);
    int64_t t = 0;
    for (uint32_t blk = 0; blk < kBlocks; ++blk) {
      float* ch[2] = {L.data(), R.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
      }
      t += kBlock;
      out[0].insert(out[0].end(), L.begin(), L.end());
      out[1].insert(out[1].end(), R.begin(), R.end());
    }
    CHECK(link.lastError().empty());
    // The new curve, as a link compiled with it from the start plays it, once the swap's old → new fade (≈ 11 ms: the
    // edit's compile replaces the unit compiled at create, which fades out) is over.
    auto gold = engine::readWav(golden::goldenDir() / "audio" / "gu10_saturator_type3.wav");
    REQUIRE(gold.ok());
    const size_t from = 2048;
    golden::Channels tail(2), want(2);
    for (size_t ch = 0; ch < 2; ++ch) {
      tail[ch].assign(out[ch].begin() + from, out[ch].end());
      want[ch].assign(gold->channels[ch].begin() + from, gold->channels[ch].end());
    }
    CHECK(peakOf(want) > 0.1);
    CHECK(maxDiff(tail, want) <= 1e-5 * std::max(peakOf(want), 1.0));
  }
}

// ---- GU.f5: a modulation tap is not the voice's sound ---------------------------------------------------------------
TEST_CASE("GU.f5 a per-voice cable into a control of an instrument group keeps the release tail (interpreter == JIT)") {
  Song plain, cabled;
  // In the default instrument: In − In (a per-voice signal that is 0) into the Phase Accumulator's Phase. The control
  // is driven per voice, so the lowering exports its modulation tap per voice; that tap is silent all along.
  {
    Song& s = cabled;
    const Track& t = *findTrackPtr(*s.store.current(), s.keys);
    DeviceId g;
    for (const DevicePtr& d : t.devices)
      if (isGroupDevice(*d) && !d->group.valid()) g = d->id;
    REQUIRE(g.valid());
    const DeviceId phase = s.named(g, "Phase");
    auto sub = std::make_shared<Device>(*makeDevice(s.store.ids().device(), "atom:sub", DeviceKind::AudioEffect));
    sub->group = g;
    REQUIRE(s.store.dispatch(AddDevice{s.keys, sub, kAppend}).ok());
    const auto connect = [&](DeviceId from, std::string_view port, DeviceId to, std::string_view param) {
      Wire w;
      w.from = from;
      w.port = std::string(port);
      w.to = to;
      w.param = std::string(param);
      s.edit(ops::connect(*s.store.current(), s.store.ids(), w));
    };
    connect(g, kGroupInPort, sub->id, "a");
    connect(g, kGroupInPort, sub->id, "b");
    connect(sub->id, "out", phase, kValueParam);
  }
  const golden::Channels a = both(cabled.store.current()), base = render(plain.store.current(), false);
  // The release (after note-off at 1 s) sounds as without the cable, to its natural end — not cut 20 ms after note-off.
  const size_t off = static_cast<size_t>(kRate);
  CHECK(peakOf(base, off + static_cast<size_t>(0.05 * kRate), off + static_cast<size_t>(0.1 * kRate)) > 0.01);
  CHECK(maxDiff(a, base) <= 1e-5 * std::max(peakOf(base), 1.0));
}

TEST_CASE("GU.f7 a muted rack chain is not computed (its Gate lowers as a literal; the choice folds to silence)") {
  Bench b(nullptr, false);
  auto run = [&](ops::EditResult e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(b.store.dispatch(e->command).ok());
    return *e;
  };
  // An Audio Effect Rack of two chains, a Saturator in each (Analog Clip: one `clip` atom each).
  const DeviceId rack = run(ops::addRack(*b.store.current(), b.store.ids(), b.track, false, 0)).devices.at(0);
  (void)run(ops::addToChain(*b.store.current(), b.store.ids(), rack, "module:saturator", DeviceKind::AudioEffect));
  (void)run(ops::addChainWithDevice(*b.store.current(), b.store.ids(), rack, "module:saturator", DeviceKind::AudioEffect));
  auto chains = [&] {
    const std::vector<RackInfo> racks = findRacks(*b.store.current(), *findTrackPtr(*b.store.current(), b.track));
    REQUIRE(racks.size() == 1);
    REQUIRE(racks[0].chains.size() == 2);
    return racks[0].chains;
  };
  const DeviceId second = chains()[1].id;
  const DeviceId gate = chains()[1].gateBlock;
  REQUIRE(gate.valid());
  CHECK(lowersAsLiteral(*b.store.current(), *findDevice(*b.store.current(), gate).device));
  CHECK(b.count("clip") == 2);
  const int gains = b.count("gain");  // per chain: the Saturator's gains and the chain mixer's
  CHECK(gains % 2 == 0);
  CHECK(b.count("mux") == 0);  // (the Gates' choices folded: both chains play)
  run(ops::setChainActive(*b.store.current(), second, false));
  CHECK(b.count("clip") == 1);  // the muted chain's Saturator is gone, its mixer's gain too
  CHECK(b.count("gain") == gains / 2);
  CHECK(b.count("mux") == 0);
  run(ops::setChainActive(*b.store.current(), second, true));
  CHECK(b.count("clip") == 2);
}
