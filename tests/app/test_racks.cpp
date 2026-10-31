// T4.8: racks through the lowering and the playback link — an Instrument Rack (key and velocity zones, a macro mapped
// with a curve) followed by an Audio Effect Rack (select zones, a macro mapped to the rack's own selector, a macro on a
// dB param) renders exactly like the hand-built ProjectGraph (diff 0), and the JIT like the interpreter; key zones split
// notes between chains bit-exactly; a macro driving 8 targets is one param write and no compile; chain-select
// crossfades (a selector sweep, a selector jump, a chain volume jump) are click-free; nested racks and per-voice chains
// render the same on both backends.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/graph/parse.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

struct Driver {
  ProjectLink& link;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  golden::Channels out = golden::Channels(2);
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      out[0].insert(out[0].end(), l.begin(), l.end());
      out[1].insert(out[1].end(), r.begin(), r.end());
    }
  }
};

double maxDiff(const golden::Channels& a, const golden::Channels& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = 0; i < a[c].size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[c][i] - b[c][i])));
  }
  return m;
}

double peakOf(const golden::Channels& a) {
  double p = 0.0;
  for (const auto& c : a)
    for (float v : c) p = std::max(p, static_cast<double>(std::fabs(v)));
  return p;
}

// Renders a graph through the per-track compile path (the interpreter, or `backend`) with `notes` on slot 0.
golden::Channels renderGraph(const graph::ProjectGraph& g, const std::vector<engine::ProjectParamLink>& links,
                             const std::vector<float>& values, uint32_t frames, std::vector<engine::TimedNote> notes,
                             engine::UnitBackend backend = {}, uint64_t* asleepBlocks = nullptr) {
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  co.voice.maxVoices = 8;
  co.paramLinks = links;
  co.backend = backend;
  engine::ProjectCompiler compiler(co);
  auto build = compiler.build(g);
  REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
  engine::NoteListSource src(std::move(notes));
  engine::ProjectConfig pc;
  pc.sampleRate = kRate;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 2;
  pc.numProjectParams = static_cast<uint32_t>(values.size());
  pc.eventSources.push_back(src.source());
  auto prog = engine::ProjectProgram::create(pc, build->layout);
  REQUIRE_MESSAGE(prog.ok(), (prog.ok() ? std::string() : prog.error().message));
  for (uint32_t u = 0; u < build->images.size(); ++u) REQUIRE((*prog)->publish(u, std::move(build->images[u])).ok());
  std::copy(values.begin(), values.end(), (*prog)->projectParams().begin());
  golden::Channels out(2);
  std::vector<float> l(kBlock), r(kBlock);
  float* ch[2] = {l.data(), r.data()};
  for (int64_t t = 0; t < frames; t += kBlock) {
    engine::AudioBlock ab{nullptr, 0, ch, 2, kBlock, t};
    CHECK(alloc_trap::countRtViolations([&] {
            DenormalGuard dg;
            (*prog)->process(ab);
          }) == 0);
    out[0].insert(out[0].end(), l.begin(), l.end());
    out[1].insert(out[1].end(), r.begin(), r.end());
  }
  if (asleepBlocks != nullptr) {
    *asleepBlocks = 0;
    for (uint32_t u = 0; u < (*prog)->numUnits(); ++u)
      if (const engine::ProgramImage* im = (*prog)->current(u))
        if (const engine::SleepStats* st = im->sleepStats()) *asleepBlocks += st->asleepBlocks;
  }
  return out;
}

// Lowers `p` and renders it (links and values from the lowered params).
struct Lowered {
  LoweredProject lp;
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
};
Lowered lowerIt(const Project& p, const DeviceLibrary& lib) {
  LowerOptions lo;
  lo.library = &lib.library();
  lo.slotOf = [](TrackId) { return 0u; };
  auto low = lower(p, lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  Lowered out{std::move(*low), {}, {}};
  for (const LoweredParam& q : out.lp.params) {
    out.links.push_back({q.track, q.name, static_cast<uint32_t>(out.values.size())});
    out.values.push_back(q.value);
  }
  return out;
}

std::string num(double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.17g", v);
  std::string s = b;
  if (s.find_first_of(".e") == std::string::npos) s += ".0";
  return s;
}

// The mixer stages of lower.h for a MIDI track (constant-power pan, out / post / pre) as text, after signal `x`.
std::string midiStages(const std::string& x) {
  return "  param volume = 0 [-70..6]\n  param pan = 0 [-1..1]\n  param gate = 1 [0..1]\n  param sendGate = 1 [0..1]\n"
         "  param mute = 0 [0..1]\n"
         "  vg = volume <= -70 ? 0 : db_to_gain(volume)\n"
         "  th = (pan + 1) * 0.78539816339744828\n  pl = sin(th + 1.5707963267948966)\n  pr = sin(th)\n"
         "  un = mute < 0.5\n  go = un ? gate : 0\n  so = un ? sendGate : 0\n"
         "  out = gain(" + x + ", pack(pl * (vg * go), pr * (vg * go)), 0.002)\n"
         "  export post = gain(" + x + ", pack(pl * (vg * so), pr * (vg * so)), 0.002)\n"
         "  export pre = gain(" + x + ", pack(pl * so, pr * so), 0.002)\n";
}

const char* kMainPatch =
    "patch Main {\n  param volume = 0 [-70..6]\n  param pan = 0 [-1..1]\n  in in : audio global stereo\n"
    "  out out : audio global stereo\n  vg = volume <= -70 ? 0 : db_to_gain(volume)\n"
    "  out = gain(in, pack(min(1, 1 - pan) * vg, min(1, 1 + pan) * vg), 0.002)\n}\n";

// A zone's gain coefficients as lower.h defines them, computed here from the definition (not the lowering's code): gain
// = clip(min(v·a1 + b1, v·a2 + b2), 0, 1), a ramp from lo to fadeLo and from fadeHi to hi, ε-inclusive edges; an edge
// at the end of the range opens that side.
struct Coef {
  double a1 = 0.0, b1 = 2.0, a2 = 0.0, b2 = 2.0;
};
Coef coef(double lo, double fadeLo, double fadeHi, double hi, bool openLo, bool openHi, double eps) {
  Coef c;
  if (!openLo) {
    c.a1 = 1.0 / (fadeLo - lo + eps);
    c.b1 = (eps - lo) * c.a1;
  }
  if (!openHi) {
    c.a2 = -1.0 / (hi - fadeHi + eps);
    c.b2 = (hi + eps) / (hi - fadeHi + eps);
  }
  return c;
}

const std::vector<engine::TimedNote> kNotes = {
    {0, 40000, 220.0f, 0.8f, 0.5f}, {3000, 40000, 277.18f, 0.6f, 0.5f}, {9000, 40000, 329.63f, 0.9f, 0.5f}};

}  // namespace

TEST_CASE("racks: an Instrument Rack and an Audio Effect Rack render exactly like the hand-built graph; JIT == interpreter") {
  auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  // Keys: Instrument Rack R1 [cA: BasicSynth, key zone ≤ 250 Hz fading out to 300 Hz, −3 dB, pan −0.5 |
  //                           cB: BasicSynth, velocity zone fading out 90 → 110; macro 1 → its `bright` (0.2 … 0.9,
  //                           curve 0.5)]
  //    → Audio Effect Rack R2 [c1: Saturator, select zone 0 … 40 fading out to 80 | c2: Utility, select zone fading in
  //                            40 → 80, +2 dB; macro 1 → the saturator's drive (0.3 … 0.8 of its dB travel); macro 2 →
  //                            R2's own selector].
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId r1 = ids.device(), sA = ids.device(), sB = ids.device(), r2 = ids.device(), sat = ids.device(),
                 util = ids.device();
  const ChainId cA = ids.chain(), cB = ids.chain(), c1 = ids.chain(), c2 = ids.chain();
  DevicePtr rack1 = makeRack(r1, DeviceKind::Instrument,
                             {makeChain(cA, "A", {makeDefaultInstrument(sA)}), makeChain(cB, "B", {makeDefaultInstrument(sB)})});
  DevicePtr rack2 = makeRack(r2, DeviceKind::AudioEffect,
                             {makeChain(c1, "1", {makeDevice(sat, "saturator", DeviceKind::AudioEffect)}),
                              makeChain(c2, "2", {makeDevice(util, "utility", DeviceKind::AudioEffect)})});
  Batch b{"racks", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys"), {}, kAppend});
  b.commands.push_back(AddDevice{keys, rack1, 0});
  b.commands.push_back(AddDevice{keys, rack2, kAppend});
  b.commands.push_back(SetChainZone{cA, ZoneKind::Key, Zone{1.0, 1.0, 250.0, 300.0}});
  b.commands.push_back(SetChainZone{cB, ZoneKind::Velocity, Zone{0.0, 0.0, 90.0, 110.0}});
  b.commands.push_back(SetChainMixer{cA, -3.0f, -0.5f});
  b.commands.push_back(SetChainZone{c1, ZoneKind::Select, Zone{0.0, 0.0, 40.0, 80.0}});
  b.commands.push_back(SetChainZone{c2, ZoneKind::Select, Zone{40.0, 80.0, 127.0, 127.0}});
  b.commands.push_back(SetChainMixer{c2, 2.0f, 0.0f});
  b.commands.push_back(AddMacroMapping{r1, MacroMapping{ids.mapping(), 0, sB, "bright", 0.2, 0.9, 0.5}});
  b.commands.push_back(AddMacroMapping{r2, MacroMapping{ids.mapping(), 0, sat, "drive", 0.3, 0.8, 0.0}});
  b.commands.push_back(AddMacroMapping{r2, MacroMapping{ids.mapping(), 1, r2, "select", 0.0, 1.0, 0.0}});
  b.commands.push_back(SetDeviceParam{r1, "macro1", 50.0});
  b.commands.push_back(SetDeviceParam{r2, "macro1", 30.0});
  b.commands.push_back(SetDeviceParam{r2, "macro2", 60.0});
  REQUIRE(store.dispatch(b).ok());
  const Lowered low = lowerIt(*store.current(), **lib);
  constexpr uint32_t kFrames = 48000;
  const golden::Channels lowered = renderGraph(low.lp.graph, low.links, low.values, kFrames, kNotes);

  // The same by hand (lower.h "Racks").
  auto travel = [&](const char* patch, const char* name) {
    const graph::Patch* p = (*lib)->library().find(patch);
    REQUIRE(p != nullptr);
    const auto n = p->findByName(name);
    REQUIRE(n.has_value());
    return paramTravel(p->paramDecl(*n));
  };
  const ParamTravel td = travel("saturator", "drive");
  REQUIRE(td.log);
  const double ln = std::log(td.hi / td.lo);
  const Coef kA = coef(std::log(1.0), std::log(1.0), std::log(250.0), std::log(300.0), true, false, 1e-4);
  Coef vB = coef(0.0, 0.0, 90.0, 110.0, true, false, 1e-3);
  vB.a2 *= 127.0;  // the graph reads voice.velocity in 0 … 1
  const Coef s1 = coef(0.0, 0.0, 40.0, 80.0, true, false, 1e-3);
  const Coef s2 = coef(40.0, 80.0, 127.0, 127.0, false, true, 1e-3);
  const double vgA = std::pow(10.0, -3.0 / 20.0), vg2 = std::pow(10.0, 2.0 / 20.0);
  const double k = 6.0 * 0.5;
  std::string text =
      "patch Keys {\n"
      "  param m1a = 50 [0..127]\n  param m1b = 30 [0..127]\n  param m2b = 60 [0..127]\n"
      "  param ka0 = 0 [-1e9..1e9]\n  param ka1 = 0 [-1e9..1e9]\n  param ka2 = 0 [-1e9..1e9]\n  param ka3 = 0 [-1e9..1e9]\n"
      "  param vb0 = 0 [-1e9..1e9]\n  param vb1 = 0 [-1e9..1e9]\n  param vb2 = 0 [-1e9..1e9]\n  param vb3 = 0 [-1e9..1e9]\n"
      "  param s10 = 0 [-1e9..1e9]\n  param s11 = 0 [-1e9..1e9]\n  param s12 = 0 [-1e9..1e9]\n  param s13 = 0 [-1e9..1e9]\n"
      "  param s20 = 0 [-1e9..1e9]\n  param s21 = 0 [-1e9..1e9]\n  param s22 = 0 [-1e9..1e9]\n  param s23 = 0 [-1e9..1e9]\n"
      "  param gla = 1 [0..4]\n  param gra = 1 [0..4]\n  param glb = 1 [0..4]\n  param grb = 1 [0..4]\n"
      "  param gl1 = 1 [0..4]\n  param gr1 = 1 [0..4]\n  param gl2 = 1 [0..4]\n  param gr2 = 1 [0..4]\n"
      "  out out : audio global stereo\n  out done : bool voice mono\n"
      // R1, chain A: key zone on ln(voice.hz)
      "  lnhz = log(voice.hz)\n"
      "  ga = clip(min(lnhz * ka0 + ka1, lnhz * ka2 + ka3), 0, 1)\n"
      "  sa = BasicSynth()\n"
      "  oa = gain(voice_sum(gain(sa.out, ga, 0.002)), pack(gla, gra), 0.002)\n"
      // chain B: velocity zone; macro 1 → bright (curve)
      "  gb = clip(min(voice.velocity * vb0 + vb1, voice.velocity * vb2 + vb3), 0, 1)\n"
      "  bx = m1a * " + num(1.0 / 127.0) + "\n"
      "  bs = (exp(bx * " + num(k) + ") - 1.0) * " + num(1.0 / std::expm1(k)) + "\n"
      "  bp = bs * " + num(0.9 - 0.2) + " + 0.2\n"
      "  bv = bp * 1.0 + 0.0\n"
      "  sb = BasicSynth(bright: bv)\n"
      "  ob = gain(voice_sum(gain(sb.out, gb, 0.002)), pack(glb, grb), 0.002)\n"
      "  r1 = oa + ob\n"
      "  done = and(sa.done, sb.done)\n"
      // R2: macro 2 → its own selector; macro 1 → the saturator's drive (Log travel)
      "  sx = m2b * " + num(1.0 / 127.0) + "\n"
      "  sp = sx * 1.0 + 0.0\n"
      "  sel = sp * 127.0 + 0.0\n"
      "  dx = m1b * " + num(1.0 / 127.0) + "\n"
      "  dp = dx * " + num(0.8 - 0.3) + " + 0.3\n"
      "  dv = exp(dp * " + num(ln) + " + " + num(std::log(td.lo)) + ")\n"
      "  z1 = clip(min(sel * s10 + s11, sel * s12 + s13), 0, 1)\n"
      "  st = saturator(gain(r1, z1, 0.002), drive: dv)\n"
      "  o1 = gain(st.out, pack(gl1, gr1), 0.002)\n"
      "  z2 = clip(min(sel * s20 + s21, sel * s22 + s23), 0, 1)\n"
      "  ut = utility(gain(r1, z2, 0.002))\n"
      "  o2 = gain(ut.out, pack(gl2, gr2), 0.002)\n"
      "  r2 = o1 + o2\n" +
      midiStages("r2") + "}\n" + kMainPatch;
  graph::ParseOptions po;
  po.externals = (*lib)->signatures();
  auto file = graph::parse(text, "hand.pqp", po);
  REQUIRE_MESSAGE(file.ok(), (file.ok() ? std::string() : file.error().message + "\n" + text));
  graph::PatchLibrary handLib;
  handLib.add(*file);
  handLib.addAll((*lib)->library());
  graph::ProjectGraph hand;
  hand.library = &handLib;
  hand.tracks.push_back({keys.value, "Keys", file->find("Keys"), graph::TrackRole::Instrument, 0});
  hand.tracks.push_back({store.current()->master->id.value, "Main", file->find("Main"), graph::TrackRole::Master, graph::kNone});
  hand.wires.push_back({{0, "out"}, {1, "in"}, graph::WireKind::Audio});
  hand.output = {1, "out"};
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
  auto link = [&](uint32_t track, const std::string& name, double v) {
    links.push_back({track, name, static_cast<uint32_t>(values.size())});
    values.push_back(static_cast<float>(v));
  };
  link(0, "m1a", 50.0);
  link(0, "m1b", 30.0);
  link(0, "m2b", 60.0);
  auto zone = [&](const char* p, const Coef& c) {
    link(0, std::string(p) + "0", c.a1);
    link(0, std::string(p) + "1", c.b1);
    link(0, std::string(p) + "2", c.a2);
    link(0, std::string(p) + "3", c.b2);
  };
  zone("ka", kA);
  zone("vb", vB);
  zone("s1", s1);
  zone("s2", s2);
  link(0, "gla", vgA * 1.0);  // balance: pan −0.5 → left 1, right 0.5
  link(0, "gra", vgA * 0.5);
  link(0, "glb", 1.0);
  link(0, "grb", 1.0);
  link(0, "gl1", 1.0);
  link(0, "gr1", 1.0);
  link(0, "gl2", vg2);
  link(0, "gr2", vg2);
  for (const char* n : {"volume", "pan", "gate", "sendGate", "mute"}) link(0, n, std::string(n) == "gate" || std::string(n) == "sendGate" ? 1.0 : 0.0);
  link(1, "volume", 0.0);
  link(1, "pan", 0.0);
  const golden::Channels manual = renderGraph(hand, links, values, kFrames, kNotes);
  CHECK(peakOf(manual) > 0.01);
  const double diff = maxDiff(lowered, manual);
  MESSAGE("lowered racks vs hand-built graph: max |diff| = " << diff);
  CHECK(diff == 0.0);

  // The JIT renders the lowered racks bit for bit like the interpreter.
  const golden::Channels jit = renderGraph(low.lp.graph, low.links, low.values, kFrames, kNotes, compile::jitUnitBackend());
  const double dj = maxDiff(lowered, jit);
  MESSAGE("interpreter vs JIT: max |diff| = " << dj);
  CHECK(dj == 0.0);
}

TEST_CASE("racks: key zones split notes between chains; a note in a fade plays at the fade's gain") {
  auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  // One BasicSynth on a track vs an Instrument Rack of two BasicSynths with complementary key zones (no fades): every
  // note sounds in exactly one chain at gain 1 (and in the other at exactly 0), so the two render the same samples.
  auto build = [&](bool rack, Zone low, Zone high) {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId t = ids.track();
    Batch b{"t", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "T"), {}, kAppend});
    if (rack) {
      const ChainId a = ids.chain(), z = ids.chain();
      b.commands.push_back(AddDevice{t, makeRack(ids.device(), DeviceKind::Instrument,
                                                 {makeChain(a, "Low", {makeDefaultInstrument(ids.device())}),
                                                  makeChain(z, "High", {makeDefaultInstrument(ids.device())})}),
                                     0});
      b.commands.push_back(SetChainZone{a, ZoneKind::Key, low});
      b.commands.push_back(SetChainZone{z, ZoneKind::Key, high});
    } else {
      b.commands.push_back(AddDevice{t, makeDefaultInstrument(ids.device()), 0});
    }
    REQUIRE(store.dispatch(b).ok());
    const Lowered l = lowerIt(*store.current(), **lib);
    return renderGraph(l.lp.graph, l.links, l.values, 24000, kNotes);
  };
  const golden::Channels plain = build(false, {}, {});
  const golden::Channels split = build(true, Zone{1, 1, 250, 250}, Zone{250, 250, 100000, 100000});
  CHECK(peakOf(plain) > 0.01);
  // A program starts its smoothed gains at 0 (they fade in over ≈ 10 ms): the rack's chain mixers add their own fade
  // to the track's, so the renders are compared once both have settled (a settled gain of 1 is an exact multiply).
  auto from = [](const golden::Channels& c, size_t at) {
    golden::Channels out(c.size());
    for (size_t k = 0; k < c.size(); ++k) out[k].assign(c[k].begin() + static_cast<std::ptrdiff_t>(at), c[k].end());
    return out;
  };
  // The voices are summed per chain and then the chains (v1 + (v2 + v3) instead of (v1 + v2) + v3): equal up to the
  // rounding of the re-associated float sum.
  const double d = maxDiff(from(plain, 2400), from(split, 2400));
  MESSAGE("one synth vs a key-split rack of two: max |diff| = " << d);
  CHECK(d < 1e-6);
  // Only the low chain: the 220 Hz note alone; a fade 250 → 300 Hz: 277.18 Hz plays at (ln 300 − ln 277.18) / (ln 300 −
  // ln 250) of its level (linear in log frequency). Compared over the second note's window before the third starts.
  const golden::Channels lowOnly = build(true, Zone{1, 1, 250, 300}, Zone{100000, 100000, 100000, 100000});
  auto energy = [](const golden::Channels& c, size_t from, size_t to) {
    double e = 0.0;
    for (size_t i = from; i < to; ++i) e += static_cast<double>(c[0][i]) * c[0][i];
    return e;
  };
  // 220 Hz alone from 0 to 3000; with 277.18 Hz added (faded) from 3000 to 9000. Subtracting a render of the 220 Hz
  // note alone isolates the faded note: render the plain synth with the first note only.
  auto renderNotes = [&](std::vector<engine::TimedNote> notes) {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId t = ids.track();
    Batch b{"t", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "T", ids.device()), {}, kAppend});
    REQUIRE(store.dispatch(b).ok());
    const Lowered l = lowerIt(*store.current(), **lib);
    return renderGraph(l.lp.graph, l.links, l.values, 24000, std::move(notes));
  };
  const golden::Channels only220 = renderNotes({kNotes[0]});
  const golden::Channels only277 = renderNotes({kNotes[1]});
  golden::Channels faded(2, std::vector<float>(lowOnly[0].size()));
  for (size_t c = 0; c < 2; ++c)
    for (size_t i = 0; i < faded[c].size(); ++i) faded[c][i] = lowOnly[c][i] - only220[c][i];
  const double expected = (std::log(300.0) - std::log(277.18) + 1e-4) / (std::log(300.0) - std::log(250.0) + 1e-4);
  const double ratio = std::sqrt(energy(faded, 3000, 9000) / energy(only277, 3000, 9000));
  MESSAGE("faded note level " << ratio << " (expected " << expected << ")");
  CHECK(ratio == doctest::Approx(expected).epsilon(1e-3));
}

TEST_CASE("racks: a macro driving 8 targets is one param write and no compile") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId t = ids.track();
  const DeviceId rack = ids.device(), u1 = ids.device(), u2 = ids.device();
  const ChainId a = ids.chain(), z = ids.chain();
  Batch b{"rack", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "T", ids.device()), {}, kAppend});
  b.commands.push_back(AddDevice{t, makeRack(rack, DeviceKind::AudioEffect,
                                             {makeChain(a, "A", {makeDevice(u1, "utility", DeviceKind::AudioEffect)}),
                                              makeChain(z, "B", {makeDevice(u2, "utility", DeviceKind::AudioEffect)})}),
                                 kAppend});
  uint8_t i = 0;
  for (const DeviceId d : {u1, u2})
    for (const char* p : {"gain", "width", "balance", "ms"})
      b.commands.push_back(AddMacroMapping{rack, MacroMapping{ids.mapping(), 0, d, p, 0.1 * (i % 4), 0.9, 0.25 * (i++ % 3)}});
  REQUIRE(store.dispatch(b).ok());
  REQUIRE(findDevice(*store.current(), rack).device->rack->mappings.size() == 8);
  auto link = makeLink(store.current());
  [[maybe_unused]] const ListenerId sub = store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(20);
  const app::LinkStats s0 = link->stats();
  REQUIRE(store.dispatch(ops::setMacroValue(*store.current(), rack, 0, 100.0)->command).ok());
  link->flush();
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().paramWrites == s0.paramWrites + 1);
  CHECK(link->paramSlot("dev" + std::to_string(rack.value) + ":macro1") != ProjectLink::kNoSlot);
  // The mapped params have no slots of their own: the macro drives them.
  CHECK(link->paramSlot("dev" + std::to_string(u1.value) + ":gain") == ProjectLink::kNoSlot);
  d.run(20);
  CHECK(link->lastError().empty());
  // A chain volume drag is a param write too (two derived gains), no compile.
  const app::LinkStats s1 = link->stats();
  REQUIRE(store.dispatch(ops::setChainMixer(*store.current(), a, -6.0f, 0.0f)->command).ok());
  link->flush();
  CHECK(link->stats().compiles == s1.compiles);
  CHECK(link->stats().paramWrites == s1.paramWrites + 2);
}

TEST_CASE("racks: chain-select crossfades are click-free (a sweep, a jump, a chain volume jump)") {
  // A held sine-like note (BasicSynth, bright 0) into an Audio Effect Rack whose two chains cross-fade over the
  // selector: c1 a Utility at −12 dB, c2 a Utility phase-inverted at +6 dB. The selector sweeps 0 → 127 over 0.5 s,
  // then jumps back to 0; then c1's volume jumps by 6 dB. Clicks would show as sample steps far above those of the held
  // note at the same level.
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId t = ids.track();
  const DeviceId inst = ids.device(), rack = ids.device(), u1 = ids.device(), u2 = ids.device();
  const ChainId c1 = ids.chain(), c2 = ids.chain();
  Batch b{"xfade", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "T", inst), {}, kAppend});
  b.commands.push_back(SetDeviceParam{inst, "bright", 0.0});
  b.commands.push_back(SetDeviceParam{inst, "sustain", 1.0});
  b.commands.push_back(AddDevice{t, makeRack(rack, DeviceKind::AudioEffect,
                                             {makeChain(c1, "1", {makeDevice(u1, "utility", DeviceKind::AudioEffect)}),
                                              makeChain(c2, "2", {makeDevice(u2, "utility", DeviceKind::AudioEffect)})}),
                                 kAppend});
  b.commands.push_back(SetDeviceParam{u1, "gain", std::pow(10.0, -12.0 / 20.0)});
  b.commands.push_back(SetDeviceParam{u2, "gain", 2.0});
  b.commands.push_back(SetDeviceParam{u2, "invL", 1.0});
  b.commands.push_back(SetChainZone{c1, ZoneKind::Select, Zone{0, 0, 40, 90}});
  b.commands.push_back(SetChainZone{c2, ZoneKind::Select, Zone{40, 90, 127, 127}});
  std::vector<Note> notes{playback_fixture::note(ids, 0, 16 * kPpq, 0)};
  b.commands.push_back(AddClip{t, playback_fixture::clip(ids, 0, 16 * kPpq, notes, "held")});
  REQUIRE(store.dispatch(b).ok());
  auto link = makeLink(store.current());
  [[maybe_unused]] const ListenerId sub = store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(static_cast<uint32_t>(0.3 * kRate / kBlock));  // the attack settles
  const size_t steadyFrom = d.out[0].size();
  d.run(static_cast<uint32_t>(0.2 * kRate / kBlock));
  const size_t steadyTo = d.out[0].size();
  // The sweep: one selector write per block (Live's knob drag), 0 → 127 over 0.5 s.
  const uint32_t sweep = static_cast<uint32_t>(0.5 * kRate / kBlock);
  const app::LinkStats s0 = link->stats();
  for (uint32_t k = 0; k <= sweep; ++k) {
    REQUIRE(store.dispatch(ops::setDeviceParam(*store.current(), rack, "select", 127.0 * k / sweep)->command).ok());
    link->flush();
    d.run(1);
  }
  CHECK(link->stats().compiles == s0.compiles);  // a selector move never compiles
  // The jump back, then a chain volume jump.
  REQUIRE(store.dispatch(ops::setDeviceParam(*store.current(), rack, "select", 0.0)->command).ok());
  link->flush();
  d.run(static_cast<uint32_t>(0.1 * kRate / kBlock));
  REQUIRE(store.dispatch(ops::setChainMixer(*store.current(), c1, 6.0f, 0.0f)->command).ok());
  link->flush();
  d.run(static_cast<uint32_t>(0.1 * kRate / kBlock));
  CHECK(link->stats().compiles == s0.compiles);
  // A click is a step: a sample-to-sample change far beyond what the note itself does at that level. A gain that
  // jumps by g on a signal of peak A steps by up to g·A; the smoothed gain stage spreads it over ≈ 2 ms.
  auto maxD1 = [&](size_t from, size_t to) {
    double m = 0.0;
    for (size_t c = 0; c < 2; ++c)
      for (size_t i = std::max<size_t>(from, 1); i < to; ++i)
        m = std::max(m, std::fabs(static_cast<double>(d.out[c][i]) - d.out[c][i - 1]));
    return m;
  };
  auto maxAbs = [&](size_t from, size_t to) {
    double m = 0.0;
    for (size_t c = 0; c < 2; ++c)
      for (size_t i = from; i < to; ++i) m = std::max(m, std::fabs(static_cast<double>(d.out[c][i])));
    return m;
  };
  const double steady = maxD1(steadyFrom, steadyTo);
  const double changes = maxD1(steadyTo, d.out[0].size());
  // Relative to the level: the loudest part of the change region is up to 4× (+12 dB) the steady level.
  const double scale = maxAbs(steadyTo, d.out[0].size()) / maxAbs(steadyFrom, steadyTo);
  MESSAGE("largest sample step: steady " << steady << ", during crossfades and jumps " << changes << " (level ×" << scale << ")");
  CHECK(steady > 0.0);
  CHECK(changes <= 1.5 * steady * scale);
}

TEST_CASE("racks: nested racks, per-voice chains and degree zones render the same on the JIT and the interpreter") {
  auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId t = ids.track();
  const DeviceId outer = ids.device(), inner = ids.device(), s1 = ids.device(), s2 = ids.device(), sat = ids.device(),
                 aer = ids.device(), util = ids.device(), eq = ids.device();
  const ChainId co1 = ids.chain(), co2 = ids.chain(), ci1 = ids.chain(), ca1 = ids.chain(), ca2 = ids.chain();
  // Outer Instrument Rack (per-voice chains) [co1: inner Instrument Rack [ci1: BasicSynth] → Saturator |
  //                                          co2: BasicSynth → Audio Effect Rack [ca1: Utility | ca2: EQ Eight]].
  DevicePtr innerRack = makeRack(inner, DeviceKind::Instrument, {makeChain(ci1, "i", {makeDefaultInstrument(s1)})});
  DevicePtr aeRack = makeRack(aer, DeviceKind::AudioEffect,
                              {makeChain(ca1, "u", {makeDevice(util, "utility", DeviceKind::AudioEffect)}),
                               makeChain(ca2, "e", {makeDevice(eq, "eq_eight", DeviceKind::AudioEffect)})});
  DevicePtr outerRack = makeRack(outer, DeviceKind::Instrument,
                                 {makeChain(co1, "a", {innerRack, makeDevice(sat, "saturator", DeviceKind::AudioEffect)}),
                                  makeChain(co2, "b", {makeDefaultInstrument(s2), aeRack})});
  Batch b{"nested", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "T"), {}, kAppend});
  b.commands.push_back(AddDevice{t, outerRack, 0});
  RackOptions o = rackOptions(*outerRack->rack);
  o.voiceChains = true;
  o.keyUnit = KeyZoneUnit::Degree;
  b.commands.push_back(SetRackOptions{outer, o});
  b.commands.push_back(SetChainZone{co1, ZoneKind::Key, Zone{-24, -24, 3, 6}});  // 12-EDO degrees around C3
  b.commands.push_back(SetChainZone{co2, ZoneKind::Key, Zone{3, 6, 1024, 1024}});
  b.commands.push_back(SetChainZone{ca2, ZoneKind::Select, Zone{0, 0, 127, 127}});  // off (full): no node
  b.commands.push_back(SetChainZone{ca1, ZoneKind::Select, Zone{0, 0, 60, 100}});
  b.commands.push_back(AddMacroMapping{outer, MacroMapping{ids.mapping(), 0, inner, "macro1", 0.0, 1.0, -0.4}});
  b.commands.push_back(AddMacroMapping{inner, MacroMapping{ids.mapping(), 0, s1, "bright", 0.1, 0.6, 0.0}});
  b.commands.push_back(AddMacroMapping{outer, MacroMapping{ids.mapping(), 1, aer, "select", 0.0, 1.0, 0.0}});
  b.commands.push_back(SetDeviceParam{outer, "macro1", 90.0});
  b.commands.push_back(SetDeviceParam{outer, "macro2", 70.0});
  REQUIRE(store.dispatch(b).ok());
  const Lowered low = lowerIt(*store.current(), **lib);
  CHECK(low.lp.missingDevices.empty());
  const std::vector<engine::TimedNote> notes = {
      {0, 30000, 220.0f, 0.8f, 0.5f}, {2000, 30000, 261.626f, 0.6f, 0.5f}, {6000, 30000, 329.63f, 0.9f, 0.5f}};
  const golden::Channels interp = renderGraph(low.lp.graph, low.links, low.values, 32000, notes);
  const golden::Channels jit = renderGraph(low.lp.graph, low.links, low.values, 32000, notes, compile::jitUnitBackend());
  CHECK(peakOf(interp) > 0.01);
  const double dj = maxDiff(interp, jit);
  MESSAGE("nested racks, interpreter vs JIT: max |diff| = " << dj);
  CHECK(dj == 0.0);
  // The inner macro is mapped by the outer one: no param of its own; the outer macros are.
  auto keyed = [&](const std::string& k) {
    return std::count_if(low.lp.params.begin(), low.lp.params.end(), [&](const LoweredParam& p) { return p.key == k; });
  };
  CHECK(keyed("dev" + std::to_string(inner.value) + ":macro1") == 0);
  CHECK(keyed("dev" + std::to_string(outer.value) + ":macro1") == 1);
  CHECK(keyed("dev" + std::to_string(outer.value) + ":macro2") == 1);
  CHECK(keyed("chain" + std::to_string(co1.value) + ":k2") == 1);
  CHECK(keyed("chain" + std::to_string(ca2.value) + ":s0") == 0);
}

TEST_CASE("racks: a muted chain is not in the program; the selector gates a chain's input (sleep recorded)") {
  auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  // A held note → (a group) Audio Effect Rack [c1: Saturator, select zone 0 … 63 | c2: EQ Eight → Saturator, select zone
  // 64 … 127] at selector 0: c2 hears silence (its input gain stage is settled at 0) and falls asleep after its tail.
  auto render = [&](bool zones, bool mute, uint64_t& asleep, size_t& nodes) {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId g = ids.track(), t = ids.track();
    const ChainId c1 = ids.chain(), c2 = ids.chain();
    const DeviceId eq = ids.device();
    Batch b{"sleep", {}};
    // The rack on a group (a unit of its own, global: its regions can sleep) fed by a MIDI track.
    b.commands.push_back(AddTrack{playback_fixture::track(g, TrackKind::Group, "G"), {}, kAppend});
    b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "T", ids.device()), g, kAppend});
    b.commands.push_back(AddDevice{g, makeRack(ids.device(), DeviceKind::AudioEffect,
                                               {makeChain(c1, "1", {makeDevice(ids.device(), "saturator", DeviceKind::AudioEffect)}),
                                                makeChain(c2, "2", {makeDevice(eq, "eq_eight", DeviceKind::AudioEffect),
                                                                    makeDevice(ids.device(), "saturator", DeviceKind::AudioEffect)})}),
                                   kAppend});
    b.commands.push_back(SetDeviceSetting{eq, "on1", 1.0});
    if (zones) {
      b.commands.push_back(SetChainZone{c1, ZoneKind::Select, Zone{0, 0, 63, 63}});
      b.commands.push_back(SetChainZone{c2, ZoneKind::Select, Zone{64, 64, 127, 127}});
    }
    if (mute) b.commands.push_back(SetChainActive{c2, false});
    REQUIRE(store.dispatch(b).ok());
    const Lowered l = lowerIt(*store.current(), **lib);
    nodes = l.lp.patches[0]->patch.nodeCount();  // the group (first in track order)
    const golden::Channels out = renderGraph(l.lp.graph, l.links, l.values, 48000, {{0, 48000, 220.0f, 0.8f, 0.5f}}, {}, &asleep);
    CHECK(peakOf(out) > 0.01);
  };
  uint64_t awake = 0, gated = 0, muted = 0;
  size_t nAwake = 0, nGated = 0, nMuted = 0;
  render(false, false, awake, nAwake);
  render(true, false, gated, nGated);
  render(false, true, muted, nMuted);
  // Recorded, not asserted: whether the gated chain's regions get a sleep segment of their own depends on the
  // scheduler's segmenting (T1.x-silence), which today keeps the rack's chains in one segment with the sum (0 / 0
  // measured): QUESTIONS Q?-T4.8-a item 5.
  MESSAGE("asleep segment-blocks over 1 s: both chains playing " << awake << ", chain 2 left by the selector " << gated
                                                                 << ", chain 2 muted " << muted);
  CHECK(nMuted < nAwake);  // the muted chain's devices are not lowered at all: it costs nothing
}

// M4 review M9: a project whose racks nest thousands deep opens and plays (the link lowers and compiles it: the
// document walks, the lowering and the compile passes run without deep recursion). A held note through an Instrument
// Rack nest 2,000 deep, then an Audio Effect Rack nest 20,000 deep with a Utility at the bottom (−6 dB): the output is
// the plain track's output (every chain mixer at unity), so nothing in the nest is lost. One track patch holds at most
// 65,535 inputs and params (graph::kMaxPorts: two mixer params per chain), so 40,000 chains on one track are refused
// with an error, not a crash.
TEST_CASE("racks: a project with racks nested 20,000 deep opens and plays like the flat chain (M9)" * doctest::test_suite("slow")) {
  constexpr int kInstrumentDepth = 2000, kEffectDepth = 20000;
  auto project = [&](bool nested, int instrumentDepth, int effectDepth) {
    auto store = std::make_unique<DocumentStore>();
    IdSource& ids = store->ids();
    const TrackId t = ids.track();
    DevicePtr synth = makeDefaultInstrument(ids.device());
    Device u = *makeDevice(ids.device(), "utility", DeviceKind::AudioEffect);
    u.params = {DeviceValue{"gain", 0.5}};  // linear (−6 dB)
    DevicePtr util = std::make_shared<const Device>(std::move(u));
    if (nested) {
      for (int k = 0; k < instrumentDepth; ++k)
        synth = makeRack(ids.device(), DeviceKind::Instrument, {makeChain(ids.chain(), "I", {synth})});
      for (int k = 0; k < effectDepth; ++k) util = makeRack(ids.device(), DeviceKind::AudioEffect, {makeChain(ids.chain(), "A", {util})});
    }
    Track tr = makeTrack(t, TrackKind::Midi, "Deep");
    tr.devices = {synth, util};
    Batch b{"deep", {}};
    b.commands.push_back(AddTrack{std::make_shared<const Track>(std::move(tr)), {}, kAppend});
    b.commands.push_back(AddClip{t, playback_fixture::clip(ids, 0, 16 * kPpq, {playback_fixture::note(ids, 0, 16 * kPpq, 0)}, "n")});
    REQUIRE(store->dispatch(b).ok());
    return store;
  };
  auto flat = project(false, 0, 0), deep = project(true, kInstrumentDepth, kEffectDepth);
  auto lf = makeLink(flat->current()), ld = makeLink(deep->current());
  Driver df{*lf}, dd{*ld};
  lf->play(0);
  ld->play(0);
  df.run(100);
  dd.run(100);
  float peak = 0.0f;
  double diff = 0.0;
  // After the chain mixers' first fades (each level's gain stage starts from 0 over ~2 ms; thousands of them in series).
  const size_t from = df.out[0].size() / 2;
  for (size_t c = 0; c < 2; ++c)
    for (size_t i = from; i < df.out[c].size(); ++i) {
      peak = std::max(peak, std::fabs(df.out[c][i]));
      diff = std::max(diff, static_cast<double>(std::fabs(df.out[c][i] - dd.out[c][i])));
    }
  MESSAGE("flat vs nested racks: peak " << peak << ", max |diff| " << diff);
  CHECK(peak > 0.01f);
  CHECK(diff < 1e-4 * peak);  // unity chain mixers (gains of 1, centre pan) leave the signal as the flat chain makes it
  // Too many chains for one track patch: an error (the link is not made), no crash.
  auto huge = project(true, 20000, 20000);
  LinkConfig c;
  c.compileThread = false;
  c.jit = false;
  auto l = ProjectLink::create(c, huge->current());
  REQUIRE_FALSE(l.ok());
  CHECK(l.error().message.find("inputs and params") != std::string::npos);
}
