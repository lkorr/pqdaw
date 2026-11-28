// FD.2 (docs/plans/FD.md "FD.2"): LANE CABLES through the link (the interpreter; every block inside RtScope: no
// allocation on the audio thread — a width-8 path's N delay rings are allocated off-thread with the unit's state):
//   - Lanes 8 → a Delay Tap whose Time is driven by a lane cable of 8 times (Pack 8, a Units wire) → Merge 8: the eight
//     impulses land at their lanes' own samples, the even lanes on the left, the odd ones on the right, each × 1/√4
//   - a lane cable into a control in every wire mode (Units, Add on a linear curve, Add on a curved one, Travel): the
//     control is per lane
//   - Unpack → Pack is the identity (bit for bit)
//   - a feedback loop on the lane cable (Lanes → Delay Tap → VCA × 0.5 → back) decays 1, 0.5, 0.25 per lane time
//   - VCA, EQ Band, Math and a Delay Tap on a lane cable are those modules per lane: Lanes 4 → … → Merge 4 of a stereo
//     signal is √2 × the stereo chain
//   - a switched-off block on a lane cable passes every lane
//   - the refusals: a lane cable into the mixer, into a module that is stereo by nature (Utility), into a native
//     engine's input (Envelope) and a Merge whose N is not the cable's are CUT with a readable reason; the rest of the
//     project plays
//   - a stereo signal into a control still gives its left channel (the rule before lanes), and a mono Noise broadcast
//     onto a lane cable compiles
// Each case is a group on an audio track (its blocks pasted as the node menu pastes them).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lanes.h"
#include "pq/document/modules.h"
#include "pq/document/store.h"
#include "pq/dsp/curve.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

struct Circuit {
  IdSource& ids;
  BlockSet set;
  DeviceId group;
  explicit Circuit(IdSource& i) : ids(i) {
    group = ids.device();
    auto g = std::make_shared<Device>(*makeGroup(group));
    g->name = "case";
    set.devices.push_back(std::move(g));
  }
  DeviceId atom(const char* name) {
    const DeviceId id = ids.device();
    auto d = std::make_shared<Device>(*makeDevice(id, std::string(kAtomDevicePrefix) + name, DeviceKind::AudioEffect));
    d->name = name;
    d->group = group;
    set.devices.push_back(std::move(d));
    return id;
  }
  // A value block on `curve` at knob position `position`.
  DeviceId value(const dsp::Curve& curve, double position, const char* name = "v") {
    const DeviceId id = ids.device();
    auto d = std::make_shared<Device>(*makeDevice(id, std::string(kValueDevice), DeviceKind::AudioEffect));
    d->name = name;
    d->group = group;
    d->params = {DeviceValue{std::string(kValueParam), position}};
    d->curves = {ControlCurve{std::string(kValueParam), curve}};
    set.devices.push_back(std::move(d));
    return id;
  }
  DeviceId konst(double v) { return value(dsp::curvePreset(dsp::CurvePreset::Linear, v, v), 1.0, "k"); }
  WireId wire(DeviceId from, std::string_view port, DeviceId to, std::string_view param, WireLaw law = WireLaw::Add,
              double amount = 1.0) {
    Wire w;
    w.id = ids.wire();
    w.from = from;
    w.port = std::string(port);
    w.to = to;
    w.param = std::string(param);
    w.law = law;
    w.amount = amount;
    set.wires.push_back(std::move(w));
    return set.wires.back().id;
  }
  void take(BlockSet s) {
    for (DevicePtr& d : s.devices) set.devices.push_back(std::move(d));
    for (Wire& w : s.wires) set.wires.push_back(std::move(w));
  }
  // A one-sample impulse at sample 4096 (tests/app/test_delay_tap.cpp: the phasor's phase is n / 65536 exactly).
  DeviceId impulse() {
    const DeviceId ph = atom("phasor"), off = atom("sub"), mag = atom("abs"), scaled = atom("mul"), lim = atom("min"), r = atom("sub");
    const DeviceId one = konst(1.0);
    wire(konst(65536.0 * 48000.0 / 4294967296.0), kValueOutput, ph, "hz");
    wire(ph, "out", off, "a");
    wire(konst(1.0 / 16.0), kValueOutput, off, "b");
    wire(off, "out", mag, "x");
    wire(mag, "out", scaled, "a");
    wire(konst(1.0e6), kValueOutput, scaled, "b");
    wire(scaled, "out", lim, "a");
    wire(one, kValueOutput, lim, "b");
    wire(one, kValueOutput, r, "a");
    wire(lim, "out", r, "b");
    return r;
  }
  // (a, a · right) as a stereo signal.
  DeviceId stereo(DeviceId a, double right) {
    const DeviceId r = atom("mul"), p = atom("pack");
    wire(a, "out", r, "a");
    wire(konst(right), kValueOutput, r, "b");
    wire(a, "out", p, "in0");
    wire(r, "out", p, "in1");
    return p;
  }
  // A lane module nested in the case, its main input fed from (`in`, `port`) when `in` is valid.
  LaneModuleRoles lanes(LaneModule kind, uint32_t n, DeviceId in = {}, std::string_view port = "out") {
    LaneModuleOptions o;
    o.inGroup = group;
    o.lanes = n;
    LaneModuleRoles r;
    take(laneModuleBlocks(ids, kind, o, &r));
    if (in.valid()) wire(in, port, r.group, kMainPort);
    return r;
  }
  // A lane cable of the constants `values` (a Pack with a constant on every input): the Pack group.
  DeviceId laneValues(const std::vector<double>& values) {
    const LaneModuleRoles p = lanes(LaneModule::Pack, static_cast<uint32_t>(values.size()));
    for (size_t i = 0; i < values.size(); ++i) wire(konst(values[i]), kValueOutput, p.lane[i], "x");
    return p.group;
  }
  DelayTapRoles delayTap(DeviceId in, std::string_view port, double timeSeconds) {
    DelayTapOptions o;
    o.inGroup = group;
    o.time = dsp::curvePosition(delayTimeCurve(), timeSeconds, 0.0);
    DelayTapRoles r;
    take(delayTapBlocks(ids, o, &r));
    wire(in, port, r.group, kMainPort);
    return r;
  }
  WireId out(DeviceId from, std::string_view port) { return wire(from, port, group, kGroupOutPort); }
};

struct Song {
  DocumentStore store;
  TrackId track;
  DeviceId group;
  Song() {
    store.setDeviceLibrary(&playback_fixture::unitsLibrary());
    auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Audio, {}, "Audio");
    REQUIRE(t.ok());
    REQUIRE(store.dispatch(t->command).ok());
    track = t->tracks.at(0);
  }
  // Pastes `set` (fresh ids: `was` → the pasted id through idOf) and puts its group on the track's main path.
  std::vector<DeviceId> pasted;
  std::vector<DeviceId> source;
  std::vector<std::pair<WireId, WireId>> wires;  // (the set's, the pasted)
  void place(const BlockSet& set) {
    std::vector<WireId> before;
    for (const Wire& w : wiresOf(*store.current())) before.push_back(w.id);
    auto e = ops::pasteBlocks(*store.current(), store.ids(), track, set);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const Status st = store.dispatch(e->command);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    group = e->devices.at(0);
    pasted = e->devices;
    for (const DevicePtr& d : set.devices) source.push_back(d->id);
    // The pasted wires in the set's order (pasteBlocks keeps it: ids ascend alike).
    std::vector<WireId> now;
    for (const Wire& w : wiresOf(*store.current()))
      if (std::find(before.begin(), before.end(), w.id) == before.end()) now.push_back(w.id);
    REQUIRE(now.size() == set.wires.size());
    std::vector<WireId> was;
    for (const Wire& w : set.wires) was.push_back(w.id);
    std::sort(was.begin(), was.end());
    for (size_t i = 0; i < was.size(); ++i) wires.emplace_back(was[i], now[i]);
    auto p = ops::insertOnPath(*store.current(), store.ids(), group, 0);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().message));
    REQUIRE(store.dispatch(p->command).ok());
  }
  DeviceId idOf(DeviceId was) const {
    for (size_t i = 0; i < source.size() && i < pasted.size(); ++i)
      if (source[i] == was) return pasted[i];
    FAIL("not a pasted block");
    return {};
  }
  WireId idOf(WireId was) const {
    for (const auto& [a, b] : wires)
      if (a == was) return b;
    FAIL("not a pasted wire");
    return {};
  }
  // The main-path link out of the case group (into the mixer).
  WireId mixerLink() const {
    for (const Wire& w : wiresOf(*store.current()))
      if (w.toMixer() && w.from == group) return w.id;
    FAIL("no link into the mixer");
    return {};
  }
};

struct Player {
  std::unique_ptr<app::ProjectLink> link;
  int64_t t = 0;
  explicit Player(const Snapshot& s) {
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = false;
    c.idleUnits = false;
    c.sampleRate = kRate;
    auto l = app::ProjectLink::create(c, s);
    REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
    link = std::move(*l);
    link->play(0);
  }
  void run(uint32_t blocks, std::vector<float>& L, std::vector<float>& R) {
    std::vector<float> l(kBlock), r(kBlock);
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        link->engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
      }
      t += kBlock;
      L.insert(L.end(), l.begin(), l.end());
      R.insert(R.end(), r.begin(), r.end());
    }
  }
};

struct Rendered {
  std::vector<float> L, R;
  std::string error;                       // the link's last error
  std::vector<std::string> cut;            // per asked wire: its refusal ("" = not cut)
};
Rendered render(Song& s, uint32_t blocks, std::vector<WireId> ask = {}) {
  Player p(s.store.current());
  Rendered out;
  p.run(blocks, out.L, out.R);
  out.error = p.link->lastError();
  for (const WireId w : ask) out.cut.push_back(p.link->wireError(w));
  return out;
}

double peak(const std::vector<float>& x) {
  double m = 0.0;
  for (const float v : x) m = std::max(m, static_cast<double>(std::fabs(v)));
  return m;
}

// Where an undelayed impulse lands, and the stereo track's gain on a centred signal (the pan law: measured, so the
// cases compare what the lanes do, not the mixer).
struct Base {
  size_t index = 0;
  double gain = 1.0;
};
Base base() {
  Song s;
  IdSource ids;
  Circuit c(ids);
  c.out(c.impulse(), "out");
  s.place(c.set);
  const Rendered r = render(s, 80);
  REQUIRE(r.error.empty());
  Base b;
  for (size_t i = 0; i < r.L.size(); ++i)
    if (r.L[i] > 0.5f) {
      b.index = i;
      b.gain = r.L[i];
      return b;
    }
  FAIL("no impulse");
  return b;
}

// The per-lane delays of the cases: distinct whole numbers of samples.
const std::vector<double> kLaneSamples = {100.0, 230.0, 370.0, 480.0, 610.0, 750.0, 880.0, 990.0};
std::vector<double> laneSeconds() {
  std::vector<double> s;
  for (const double n : kLaneSamples) s.push_back(n / kRate);
  return s;
}

}  // namespace

TEST_CASE("lanes: 8 impulses through one Delay Tap with 8 per-lane times land at their lanes' samples after Merge") {
  const Base b0 = base();
  Song s;
  IdSource ids;
  Circuit c(ids);
  const LaneModuleRoles lanes = c.lanes(LaneModule::Lanes, 8, c.impulse());
  DelayTapRoles tap = c.delayTap(lanes.group, kMainPort, 0.0);
  const WireId times = c.wire(c.laneValues(laneSeconds()), kMainPort, tap.time, kValueParam, WireLaw::Units);
  const LaneModuleRoles merge = c.lanes(LaneModule::Merge, 8, tap.group, kMainPort);
  c.out(merge.group, kMainPort);
  s.place(c.set);
  // The document's estimate: the lane cables are 8 wide, the merged one is not a lane cable.
  const LaneWidths lw = laneWidths(*s.store.current());
  CHECK(lw.of(s.idOf(times)) == 8);
  CHECK(lw.of(s.mixerLink()) == 0);
  const Rendered r = render(s, 90, {s.idOf(times), s.mixerLink()});
  CAPTURE(r.error);
  CAPTURE(r.cut[0]);
  CAPTURE(r.cut[1]);
  CHECK(r.error.empty());
  CHECK(r.cut[0].empty());
  CHECK(r.cut[1].empty());
  const double g = laneMergeGain(8) * b0.gain;  // 1/√4 per ear
  for (size_t i = 0; i < kLaneSamples.size(); ++i) {
    CAPTURE(i);
    const size_t at = b0.index + static_cast<size_t>(kLaneSamples[i]);
    const std::vector<float>& ear = i % 2 == 0 ? r.L : r.R;    // even lanes left, odd lanes right
    const std::vector<float>& other = i % 2 == 0 ? r.R : r.L;
    CHECK(ear[at] == doctest::Approx(g).epsilon(2e-3));
    CHECK(std::fabs(other[at]) < 2e-3);
  }
  // Nothing but the eight impulses (± the read's interpolation around a time that is not exactly a whole sample).
  double rest = 0.0;
  for (size_t n = 0; n < r.L.size(); ++n) {
    bool near = false;
    for (const double d : kLaneSamples) near = near || std::llabs(static_cast<long long>(n) - static_cast<long long>(b0.index + d)) <= 2;
    if (!near) rest = std::max({rest, static_cast<double>(std::fabs(r.L[n])), static_cast<double>(std::fabs(r.R[n]))});
  }
  CHECK(rest < 2e-3);
}

TEST_CASE("lanes: a lane cable into the mixer is cut with a reason that says to merge; the project still plays") {
  Song s;
  IdSource ids;
  Circuit c(ids);
  const LaneModuleRoles lanes = c.lanes(LaneModule::Lanes, 8, c.impulse());
  c.out(lanes.group, kMainPort);
  s.place(c.set);
  CHECK(laneWidths(*s.store.current()).of(s.mixerLink()) == 8);
  const Rendered r = render(s, 80, {s.mixerLink()});
  CHECK(r.error.empty());
  CHECK(r.cut[0].find("merge") != std::string::npos);
  CHECK(r.cut[0].find("8 lanes") != std::string::npos);
  CHECK(peak(r.L) == 0.0);
  CHECK(peak(r.R) == 0.0);
}

TEST_CASE("lanes: a lane cable into a control makes it per lane in every wire mode; a stereo cable still gives its left") {
  const Base b0 = base();
  const std::vector<double> u = {0.1, 0.2, 0.3, 0.4};
  const dsp::Curve lin = dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 10.0);
  const dsp::Curve bent = dsp::curvePreset(dsp::CurvePreset::Exp0, 0.0, 10.0, 3.0);
  struct Mode {
    const char* name;
    WireLaw law;
    double amount;
    const dsp::Curve* curve;
  };
  const Mode modes[] = {{"units", WireLaw::Units, 1.0, &lin},
                        {"add, linear curve", WireLaw::Add, 1.0, &lin},
                        {"add, bent curve", WireLaw::Add, 1.0, &bent},
                        {"travel", WireLaw::Travel, 0.5, &lin}};
  constexpr double kKnob = 0.2;
  for (const Mode& m : modes) {
    CAPTURE(m.name);
    auto want = [&](double x) {
      if (m.law == WireLaw::Units) return dsp::evalCurve(*m.curve, kKnob) + x;
      if (m.law == WireLaw::Travel) return dsp::evalCurve(*m.curve, kKnob + m.amount * x);
      return dsp::curveIsLinear(*m.curve) ? dsp::evalCurve(*m.curve, kKnob) + x : dsp::evalCurve(*m.curve, kKnob + x);
    };
    for (const bool lanes : {true, false}) {
      CAPTURE(lanes);
      Song s;
      IdSource ids;
      Circuit c(ids);
      const DeviceId v = c.value(*m.curve, kKnob, "gain"), mul = c.atom("mul");
      c.wire(v, kValueOutput, mul, "b");
      WireId drive;
      if (lanes) {  // 1 on four lanes × the per-lane control, merged
        c.wire(c.lanes(LaneModule::Lanes, 4, c.konst(1.0), kValueOutput).group, kMainPort, mul, "a");
        drive = c.wire(c.laneValues(u), "y", v, kValueParam, m.law, m.amount);  // (a Travel wire names its port)
        c.out(c.lanes(LaneModule::Merge, 4, mul).group, kMainPort);
      } else {  // a stereo cable (0.3, 0.9) into the control: its left (the rule before lanes), on a mono 1
        c.wire(c.konst(1.0), kValueOutput, mul, "a");
        const DeviceId st = c.atom("pack");
        c.wire(c.konst(0.3), kValueOutput, st, "in0");
        c.wire(c.konst(0.9), kValueOutput, st, "in1");
        drive = c.wire(st, "out", v, kValueParam, m.law, m.amount);
        c.out(mul, "out");
      }
      s.place(c.set);
      CHECK((laneWidths(*s.store.current()).of(s.idOf(drive)) == 4) == lanes);
      const Rendered r = render(s, 70, {s.idOf(drive)});
      CAPTURE(r.error);
      CAPTURE(r.cut[0]);
      CHECK(r.error.empty());
      CHECK(r.cut[0].empty());
      const size_t at = 4200;  // settled (param smoothing, the fade-in of the first image)
      if (lanes) {
        const double g = laneMergeGain(4) * b0.gain;
        CHECK(r.L[at] == doctest::Approx(g * (want(u[0]) + want(u[2]))).epsilon(1e-4));
        CHECK(r.R[at] == doctest::Approx(g * (want(u[1]) + want(u[3]))).epsilon(1e-4));
      } else {
        CHECK(r.L[at] == doctest::Approx(b0.gain * want(0.3)).epsilon(1e-4));
        CHECK(r.R[at] == doctest::Approx(b0.gain * want(0.3)).epsilon(1e-4));
      }
    }
  }
}

TEST_CASE("lanes: Unpack -> Pack is the identity, bit for bit") {
  Rendered got[2];
  for (const bool round : {false, true}) {
    Song s;
    IdSource ids;
    Circuit c(ids);
    const LaneModuleRoles lanes = c.lanes(LaneModule::Lanes, 8, c.impulse());
    DelayTapRoles tap = c.delayTap(lanes.group, kMainPort, 0.0);
    c.wire(c.laneValues(laneSeconds()), kMainPort, tap.time, kValueParam, WireLaw::Units);
    DeviceId last = tap.group;
    if (round) {
      const LaneModuleRoles un = c.lanes(LaneModule::Unpack, 8, tap.group, kMainPort);
      const LaneModuleRoles pk = c.lanes(LaneModule::Pack, 8);
      REQUIRE(un.lane.size() == 8);
      REQUIRE(pk.lane.size() == 8);
      for (size_t i = 0; i < 8; ++i) c.wire(un.lane[i], "out", pk.lane[i], "x");
      last = pk.group;
    }
    c.out(c.lanes(LaneModule::Merge, 8, last, kMainPort).group, kMainPort);
    s.place(c.set);
    got[round ? 1 : 0] = render(s, 90);
    CHECK(got[round ? 1 : 0].error.empty());
  }
  CHECK(peak(got[0].L) > 0.1);
  CHECK(got[0].L == got[1].L);
  CHECK(got[0].R == got[1].R);
}

TEST_CASE("lanes: a feedback loop on the lane cable (Lanes -> Delay Tap -> VCA x 0.5 -> back) decays per lane") {
  const Base b0 = base();
  Song s;
  IdSource ids;
  Circuit c(ids);
  const LaneModuleRoles lanes = c.lanes(LaneModule::Lanes, 4, c.impulse());
  const DeviceId sum = c.atom("add");
  c.wire(lanes.group, kMainPort, sum, "a");
  DelayTapRoles tap = c.delayTap(sum, "out", 0.0);
  // Per-lane loop times: 300, 420, 540, 660 samples.
  const std::vector<double> d = {300.0, 420.0, 540.0, 660.0};
  std::vector<double> sec;
  for (const double n : d) sec.push_back(n / kRate);
  c.wire(c.laneValues(sec), kMainPort, tap.time, kValueParam, WireLaw::Units);
  VcaOptions vo;
  vo.inGroup = c.group;
  vo.value = dsp::curvePosition(vcaCurve(), 0.5, 0.5);
  VcaRoles vr;
  c.take(vcaBlocks(ids, vo, &vr));
  c.wire(tap.group, kMainPort, vr.group, kMainPort);
  const WireId back = c.wire(vr.group, kMainPort, sum, "b");
  c.out(c.lanes(LaneModule::Merge, 4, tap.group, kMainPort).group, kMainPort);
  s.place(c.set);
  CHECK(laneWidths(*s.store.current()).of(s.idOf(back)) == 4);
  const Rendered r = render(s, 110, {s.idOf(back)});
  CAPTURE(r.error);
  CAPTURE(r.cut[0]);
  CHECK(r.error.empty());
  CHECK(r.cut[0].empty());
  const double g = laneMergeGain(4) * b0.gain;
  for (size_t i = 0; i < d.size(); ++i) {
    CAPTURE(i);
    const std::vector<float>& ear = i % 2 == 0 ? r.L : r.R;
    double amp = g;
    for (int k = 1; k <= 3; ++k, amp *= 0.5) {
      CAPTURE(k);
      CHECK(ear[b0.index + static_cast<size_t>(d[i]) * static_cast<size_t>(k)] == doctest::Approx(amp).epsilon(5e-3));
    }
  }
}

namespace {

// VCA x 2 -> EQ Band (low pass 2 kHz) -> Math (x 0.5 typed) -> Delay Tap (240 samples) after (`in`, `port`): the last
// block and its output port.
std::pair<DeviceId, std::string> moduleChain(Circuit& c, DeviceId in, std::string_view port) {
  VcaOptions vo;
  vo.inGroup = c.group;
  vo.value = dsp::curvePosition(vcaCurve(), 2.0, 0.7);
  VcaRoles vr;
  c.take(vcaBlocks(c.ids, vo, &vr));
  c.wire(in, port, vr.group, kMainPort);
  EqBandOptions eo;
  eo.inGroup = c.group;
  eo.type = 0;  // low pass
  eo.freqHz = 2000.0;
  BlockSet band = eqBandBlocks(c.ids, eo);
  const DeviceId bandGroup = band.devices.front()->id;
  c.take(std::move(band));
  c.wire(vr.group, kMainPort, bandGroup, kMainPort);
  MathOptions mo;
  mo.inGroup = c.group;
  mo.op = "mul";
  mo.b = 0.5;
  MathRoles mr;
  c.take(mathBlocks(c.ids, mo, &mr));
  c.wire(bandGroup, kMainPort, mr.group, kMainPort);
  const DelayTapRoles tap = c.delayTap(mr.group, kMainPort, 240.0 / kRate);
  return {tap.group, std::string(kMainPort)};
}

}  // namespace

TEST_CASE("lanes: VCA, EQ Band, Math and Delay Tap on a lane cable are those modules per lane") {
  Rendered got[2];
  for (const bool lanes : {false, true}) {
    Song s;
    IdSource ids;
    Circuit c(ids);
    const DeviceId src = c.stereo(c.impulse(), -0.5);
    if (lanes) {
      const LaneModuleRoles fan = c.lanes(LaneModule::Lanes, 4, src);
      const auto [last, port] = moduleChain(c, fan.group, kMainPort);
      c.out(c.lanes(LaneModule::Merge, 4, last, port).group, kMainPort);
    } else {
      const auto [last, port] = moduleChain(c, src, "out");
      c.out(last, port);
    }
    s.place(c.set);
    got[lanes ? 1 : 0] = render(s, 100);
    CAPTURE(got[lanes ? 1 : 0].error);
    CHECK(got[lanes ? 1 : 0].error.empty());
  }
  // Lanes 4 puts L on two lanes and R on two; Merge 4 sums each pair x 1/sqrt 2: sqrt 2 x the stereo chain.
  REQUIRE(peak(got[0].L) > 0.01);
  REQUIRE(peak(got[0].R) > 0.005);
  double worst = 0.0;
  const double k = 2.0 * laneMergeGain(4);
  for (size_t i = 0; i < got[0].L.size(); ++i) {
    worst = std::max(worst, std::fabs(static_cast<double>(got[1].L[i]) - k * got[0].L[i]));
    worst = std::max(worst, std::fabs(static_cast<double>(got[1].R[i]) - k * got[0].R[i]));
  }
  CHECK(worst < 1e-6);
}

TEST_CASE("lanes: a switched-off module on a lane cable passes every lane") {
  const Base b0 = base();
  Song s;
  IdSource ids;
  Circuit c(ids);
  const LaneModuleRoles lanes = c.lanes(LaneModule::Lanes, 8, c.stereo(c.impulse(), -0.5));
  DelayTapOptions o;
  o.inGroup = c.group;
  o.active = false;
  DelayTapRoles tap;
  c.take(delayTapBlocks(ids, o, &tap));
  c.wire(lanes.group, kMainPort, tap.group, kMainPort);
  c.out(c.lanes(LaneModule::Merge, 8, tap.group, kMainPort).group, kMainPort);
  s.place(c.set);
  const Rendered r = render(s, 70);
  CAPTURE(r.error);
  CHECK(r.error.empty());
  const double g = 4.0 * laneMergeGain(8) * b0.gain;  // four lanes an ear
  CHECK(r.L[b0.index] == doctest::Approx(g).epsilon(1e-5));
  CHECK(r.R[b0.index] == doctest::Approx(-0.5 * g).epsilon(1e-5));
}

TEST_CASE("lanes: stereo-only and mismatched consumers refuse a lane cable with a readable reason") {
  const Base b0 = base();
  SUBCASE("a module that is stereo by nature (Utility)") {
    Song s;
    IdSource ids;
    Circuit c(ids);
    const LaneModuleRoles lanes = c.lanes(LaneModule::Lanes, 8, c.impulse());
    StockModuleOptions o;
    o.inGroup = c.group;
    BlockSet util = utilityBlocks(ids, o);
    const DeviceId u = util.devices.front()->id;
    c.take(std::move(util));
    const WireId in = c.wire(lanes.group, kMainPort, u, kMainPort);
    c.out(u, kMainPort);
    s.place(c.set);
    const Rendered r = render(s, 70, {s.idOf(in)});
    CAPTURE(r.error);
    CHECK(r.error.empty());
    CHECK(r.cut[0] == "8 lanes into a stereo input: merge the lanes first (a Merge module)");
    CHECK(peak(r.L) == 0.0);
  }
  SUBCASE("a native engine module (the Envelope: mono)") {
    Song s;
    IdSource ids;
    Circuit c(ids);
    const LaneModuleRoles lanes = c.lanes(LaneModule::Lanes, 4, c.impulse());
    EngineModuleOptions o;
    o.inGroup = c.group;
    EngineRoles er;
    c.take(engineModuleBlocks(ids, ModuleKind::Envelope, o, &er));
    const WireId in = c.wire(lanes.group, kMainPort, er.group, kMainPort);
    c.out(er.group, kMainPort);
    s.place(c.set);
    const Rendered r = render(s, 70, {s.idOf(in)});
    CAPTURE(r.error);
    CHECK(r.error.empty());
    CHECK(r.cut[0] == "4 lanes into a stereo input: merge the lanes first (a Merge module)");
  }
  SUBCASE("a bare engine block's input") {
    Song s;
    IdSource ids;
    Circuit c(ids);
    const LaneModuleRoles lanes = c.lanes(LaneModule::Lanes, 4, c.impulse());
    const DeviceId env = ids.device();
    {
      auto d = std::make_shared<Device>(*makeDevice(env, std::string(kMetaDevicePrefix) + "envelope", DeviceKind::AudioEffect));
      d->group = c.group;
      c.set.devices.push_back(std::move(d));
    }
    const WireId in = c.wire(lanes.group, kMainPort, env, kMainPort);
    c.out(env, kMainPort);
    s.place(c.set);
    const Rendered r = render(s, 70, {s.idOf(in)});
    CAPTURE(r.error);
    CHECK(r.error.empty());
    CHECK(r.cut[0].find("4 lanes into a mono input") != std::string::npos);
  }
  SUBCASE("a Merge set to another lane count") {
    Song s;
    IdSource ids;
    Circuit c(ids);
    const LaneModuleRoles lanes = c.lanes(LaneModule::Lanes, 4, c.impulse());
    LaneModuleOptions mo;
    mo.inGroup = c.group;
    mo.lanes = 8;
    LaneModuleRoles merge;
    c.take(laneModuleBlocks(ids, LaneModule::Merge, mo, &merge));
    const WireId in = c.wire(lanes.group, kMainPort, merge.group, kMainPort);
    c.out(merge.group, kMainPort);
    s.place(c.set);
    const Rendered r = render(s, 70, {s.idOf(in)});
    CAPTURE(r.error);
    CHECK(r.error.empty());
    CHECK(r.cut[0] == "4 lanes into a module set to 8 lanes: set its lane count to 4");
  }
  SUBCASE("two widths at one block: the second is cut, the first plays") {
    Song s;
    IdSource ids;
    Circuit c(ids);
    const DeviceId imp = c.impulse();
    const LaneModuleRoles lanes = c.lanes(LaneModule::Lanes, 4, imp);
    const DeviceId sum = c.atom("add");
    const WireId wide = c.wire(lanes.group, kMainPort, sum, "a");
    const WireId narrow = c.wire(c.stereo(imp, 1.0), "out", sum, "b");
    c.out(c.lanes(LaneModule::Merge, 4, sum).group, kMainPort);
    s.place(c.set);
    const Rendered r = render(s, 70, {s.idOf(wide), s.idOf(narrow)});
    CAPTURE(r.error);
    CHECK(r.error.empty());
    CHECK(r.cut[0].empty());
    CHECK(r.cut[1].find("4 lanes and a stereo signal meet here") != std::string::npos);
    CHECK(r.L[b0.index] == doctest::Approx(2.0 * laneMergeGain(4) * b0.gain).epsilon(1e-5));
  }
}

TEST_CASE("lanes: a lane cable into another track's mixer is cut there; both tracks play") {
  const Base b0 = base();
  Song s;
  IdSource ids;
  Circuit c(ids);
  const DeviceId imp = c.impulse();
  const LaneModuleRoles lanes = c.lanes(LaneModule::Lanes, 8, imp);
  c.out(c.lanes(LaneModule::Merge, 8, lanes.group, kMainPort).group, kMainPort);
  s.place(c.set);
  auto t = ops::createTrack(*s.store.current(), s.store.ids(), TrackKind::Audio, {}, "Other");
  REQUIRE(t.ok());
  REQUIRE(s.store.dispatch(t->command).ok());
  Wire w;
  w.id = s.store.ids().wire();
  w.from = s.idOf(lanes.group);
  w.mixer = t->tracks.at(0);
  const Status st = s.store.dispatch(AddWire{w});
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  const Rendered r = render(s, 70, {w.id});
  CAPTURE(r.error);
  CHECK(r.error.empty());
  CHECK(r.cut[0] == "8 lanes into a stereo input: merge the lanes first (a Merge module)");
  CHECK(r.L[b0.index] == doctest::Approx(4.0 * laneMergeGain(8) * b0.gain).epsilon(1e-5));  // the first track's own path
}
