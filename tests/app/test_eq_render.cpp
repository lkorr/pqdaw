// EQ.3 (docs/plans/EQ.md "Structure"; pq/document/modules.h "the band and the EQ"): renders through the link
// (interpreter) and the compiled programs —
//   band    a band of each type (set by ops::setBandType: a structural edit) plays exactly what a group holding a plain
//           svf atom with the same inputs plays (its mode and stages as settings, its cutoff / q / gain constants of
//           the band's runtime values): bit for bit
//   EQ      with chain B a copy of chain A, L/R and M/S play Stereo to ≤ −120 dB of the peak (a Utility before it makes
//           L and R differ); Stereo schedules no op of chain B or the matrix (svf 4, no mux / pack / channel), L/R
//           runs both chains mono (svf 8, each one channel)
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/modules.h"
#include "pq/dsp/svf_design.h"
#include "pq/engine/project_compile.h"
#include "pq/graph/atoms.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kBlocks = 500;

const DeviceLibrary& library() { return playback_fixture::unitsLibrary(); }

golden::Channels render(const Snapshot& s) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  c.sampleRate = kRate;
  auto l = app::ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  app::ProjectLink& link = **l;
  REQUIRE(link.waitForMetaTables());
  link.play(0);
  golden::Channels out(2);
  std::vector<float> L(kBlock), R(kBlock);
  int64_t t = 0;
  for (uint32_t b = 0; b < kBlocks; ++b) {
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

double maxDiff(const golden::Channels& a, const golden::Channels& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = 0; i < a[c].size(); ++i) m = std::max(m, std::fabs(static_cast<double>(a[c][i]) - b[c][i]));
  }
  return m;
}
double peakOf(const golden::Channels& a) {
  double p = 0.0;
  for (const auto& c : a)
    for (const float x : c) p = std::max(p, static_cast<double>(std::fabs(x)));
  return p;
}

// A MIDI track (its default instrument) playing a chord.
struct Song {
  DocumentStore store;
  TrackId keys;
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  Song() {
    store.setDeviceLibrary(&library());
    keys = edit(ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Keys")).tracks.at(0);
    std::vector<Note> notes;
    for (const int32_t d : {0, 4, 7}) notes.push_back(playback_fixture::note(store.ids(), 0, 2 * kPpq, d));
    Batch b{"clip", {}};
    b.commands.push_back(AddClip{keys, playback_fixture::clip(store.ids(), 0, 4 * kPpq, notes, "chord")});
    REQUIRE(store.dispatch(b).ok());
  }
  DeviceId insert(const BlockSet& set, uint32_t slot) {
    const DeviceId g = edit(ops::pasteBlocks(*store.current(), store.ids(), keys, set)).devices.at(0);
    edit(ops::insertOnPath(*store.current(), store.ids(), g, slot));
    return g;
  }
  const Device& dev(DeviceId d) const { return *findDevice(*store.current(), d).device; }
  void set(DeviceId v, double value) {
    const dsp::Curve& c = *dev(v).curve(kValueParam);
    REQUIRE(store.dispatch(SetDeviceParam{v, std::string(kValueParam), dsp::curvePosition(c, dsp::curveOutput(c, value), 0.0)}).ok());
  }
  float runtime(DeviceId v) const {
    const graph::ParamDecl* decl = findAnyParamDecl(valuePatch(), kValueParam);
    REQUIRE(decl != nullptr);
    return static_cast<float>(paramValue(dev(v), kValueParam, *decl));
  }
};

// A group In → svf → Out: mode and stages as the atom's settings, cutoff / q / gain constant value blocks.
BlockSet plainSvf(IdSource& ids, float cutoff, float q, float gain, uint32_t mode, uint32_t stages) {
  BlockSet s;
  const DeviceId g = ids.device(), svf = ids.device();
  auto group = std::make_shared<Device>(*makeGroup(g));
  group->name = "plain svf";
  s.devices.push_back(group);
  auto atom = std::make_shared<Device>(*makeDevice(svf, std::string(kAtomDevicePrefix) + "svf", DeviceKind::AudioEffect));
  atom->group = g;
  atom->settings = {DeviceValue{"mode", static_cast<double>(mode)}, DeviceValue{"stages", static_cast<double>(stages)}};
  s.devices.push_back(atom);
  auto wire = [&](DeviceId from, std::string port, DeviceId to, std::string param) {
    Wire w;
    w.id = ids.wire();
    w.from = from;
    w.port = std::move(port);
    w.to = to;
    w.param = std::move(param);
    w.law = WireLaw::Add;
    s.wires.push_back(std::move(w));
  };
  for (const auto& [name, v] : {std::pair<const char*, float>{"cutoff", cutoff}, {"q", q}, {"gain", gain}}) {
    const DeviceId k = ids.device();
    auto d = std::make_shared<Device>(*makeDevice(k, std::string(kValueDevice), DeviceKind::AudioEffect));
    d->group = g;
    d->params = {DeviceValue{std::string(kValueParam), 1.0}};
    d->curves = {ControlCurve{std::string(kValueParam), dsp::curvePreset(dsp::CurvePreset::Linear, v, v)}};
    s.devices.push_back(d);
    wire(k, std::string(kValueOutput), svf, name);
  }
  wire(g, std::string(kGroupInPort), svf, "x");
  wire(svf, "out", g, std::string(kGroupOutPort));
  return s;
}

// Atom name → count over every optimised unit of `s`; `widths` (optional) gets each svf's output width.
std::map<std::string, int> atomsOf(const Snapshot& s, std::vector<uint16_t>* widths = nullptr) {
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
  for (uint32_t u = 0; u < compiler.numUnits(); ++u) {
    const graph::TypedPatch& tp = *compiler.unit(u).typed;
    for (graph::NodeIndex n = 0; n < tp.patch.nodeCount(); ++n) {
      const graph::Node& node = tp.patch.node(n);
      if (node.kind != graph::NodeKind::Atom) continue;
      const std::string name(graph::atomSignature(node.atom).name);
      ++out[name];
      if (widths != nullptr && name == "svf") widths->push_back(tp.types[tp.patch.flatOutput(n, 0)].width);
    }
  }
  return out;
}
int count(const std::map<std::string, int>& m, const char* atom) {
  const auto it = m.find(atom);
  return it == m.end() ? 0 : it->second;
}

}  // namespace

TEST_CASE("EQ.3 band: each type (set by setBandType) plays a plain svf atom with the same inputs, bit for bit") {
  for (uint32_t type = 0; type < dsp::kSvfModeCount; ++type) {
    CAPTURE(type);
    Song a;
    const DeviceId band = a.insert(eqBandBlocks(a.store.ids()), 1);
    const BandInfo b = *bandInfo(*a.store.current(), band);
    a.edit(ops::setBandType(*a.store.current(), band, type));
    if (type != 4) CHECK(a.store.lastChanges().structural());  // (4: the default Bell, an empty edit)
    a.edit(ops::setBandStages(*a.store.current(), band, 3));
    a.set(b.frequency, 1200.0);
    a.set(b.q, 1.3);
    a.set(b.gain, 6.0);
    const golden::Channels x = render(a.store.current());
    Song r;
    r.insert(plainSvf(r.store.ids(), a.runtime(b.frequency), a.runtime(b.q), a.runtime(b.gain), type, 3), 1);
    const golden::Channels y = render(r.store.current());
    CHECK(peakOf(y) > 1e-4);  // (a chord around middle C through a 1.2 kHz high-pass of 3 stages is quiet)
    CHECK(maxDiff(x, y) == 0.0);
  }
}

TEST_CASE("EQ.3 EQ: with chain B a copy of A, L/R and M/S play Stereo (≤ −120 dB); Stereo schedules nothing of chain B") {
  Song s;
  const DeviceId util = s.insert(utilityBlocks(s.store.ids()), 1);
  for (const Device* d : groupMembersDeep(findDevice(*s.store.current(), util).track->devices, util))
    if (d->name == "Balance") s.set(d->id, 0.6);  // L quieter than R: S ≠ 0
  const DeviceId eq = s.insert(eqBlocks(s.store.ids()), 2);
  const double gains[] = {6.0, -4.0, 5.0, -8.0};
  for (size_t i = 0; i < 4; ++i) s.set(eqInfo(*s.store.current(), eq)->bands[0][i].gain, gains[i]);
  const golden::Channels stereo = render(s.store.current());
  const double peak = peakOf(stereo);
  CHECK(peak > 0.01);
  CHECK(std::fabs(peakOf(golden::Channels{stereo[0]}) - peakOf(golden::Channels{stereo[1]})) > 0.01 * peak);
  s.edit(ops::setEqMode(*s.store.current(), s.store.ids(), eq, EqMode::LeftRight));  // B = a copy of A
  REQUIRE(eqInfo(*s.store.current(), eq)->bands[1].size() == 4);
  std::vector<uint16_t> widths;
  const std::map<std::string, int> lr = atomsOf(s.store.current(), &widths);
  CHECK(count(lr, "svf") == 8);
  CHECK(widths == std::vector<uint16_t>(8, 1));  // each chain on one channel
  CHECK(maxDiff(render(s.store.current()), stereo) <= 1e-6 * peak);
  s.edit(ops::setEqMode(*s.store.current(), s.store.ids(), eq, EqMode::MidSide));
  CHECK(count(atomsOf(s.store.current()), "svf") == 8);
  CHECK(maxDiff(render(s.store.current()), stereo) <= 1e-6 * peak);
  // Back to Stereo (B keeps its bands): nothing of B or the matrix is scheduled; A runs on the stereo signal.
  s.edit(ops::setEqMode(*s.store.current(), s.store.ids(), eq, EqMode::Stereo));
  widths.clear();
  const std::map<std::string, int> st = atomsOf(s.store.current(), &widths);
  CHECK(count(st, "svf") == 4);
  CHECK(widths == std::vector<uint16_t>(4, 2));
  // (the Utility before the EQ schedules its own channel / pack atoms: none more)
  Song bare;
  const DeviceId u2 = bare.insert(utilityBlocks(bare.store.ids()), 1);
  (void)u2;
  const std::map<std::string, int> base = atomsOf(bare.store.current());
  for (const char* atom : {"mux", "pack", "channel", "add", "sub", "mul"}) {
    CAPTURE(atom);
    CHECK(count(st, atom) == count(base, atom));
  }
  CHECK(maxDiff(render(s.store.current()), stereo) == 0.0);
}
