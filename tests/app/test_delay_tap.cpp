// DT.1 (docs/plans/DT.md): the Delay Tap module through the link (the interpreter; every block inside RtScope: no
// allocation on the audio thread — its 5 s ring is allocated off-thread with the unit's state):
//   - an impulse lands where the time says: a whole number of samples exactly, half a sample as the cubic read's
//     (−1/16, 9/16, 9/16, −1/16); stereo in, stereo out (each channel delayed alike)
//   - sync mode: 1/8 at 120 BPM = 0.25 s, at 90 BPM = 1/3 s (the transport's tempo per block)
//   - a feedback loop (Out → VCA × 0.5 → back into In, summed with the impulse) decays 1, 0.5, 0.25 at D, 2D, 3D
//   - a time change while a 1 kHz sine plays glides (the param ramp): no step in the output, and the new delay is reached
// Each case is a group on an audio track: a source (atoms) → the Delay Tap (nested, as the node menu pastes it) → Out.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <ostream>
#include <memory>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/document/store.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

// Builds the case's outer group: its blocks, wires and the ids that matter.
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
  DeviceId konst(double v) {
    const DeviceId id = ids.device();
    auto d = std::make_shared<Device>(*makeDevice(id, std::string(kValueDevice), DeviceKind::AudioEffect));
    d->name = "k";
    d->group = group;
    d->params = {DeviceValue{std::string(kValueParam), 1.0}};
    d->curves = {ControlCurve{std::string(kValueParam), dsp::curvePreset(dsp::CurvePreset::Linear, v, v)}};
    set.devices.push_back(std::move(d));
    return id;
  }
  void wire(DeviceId from, std::string_view port, DeviceId to, std::string_view param) {
    Wire w;
    w.id = ids.wire();
    w.from = from;
    w.port = std::string(port);
    w.to = to;
    w.param = std::string(param);
    w.law = WireLaw::Add;
    set.wires.push_back(std::move(w));
  }
  void take(BlockSet s) {
    for (DevicePtr& d : s.devices) set.devices.push_back(std::move(d));
    for (Wire& w : s.wires) set.wires.push_back(std::move(w));
  }
  // A one-sample impulse at sample 4096 (past the first image's fade-in): 1 − min(1, 10^6 · |phasor − 1/16|), the phasor
  // stepping exactly 2^16 per sample (hz = 2^16 · 48000 / 2^32), so its phase is n / 65536 exactly: 1/16 at n = 4096
  // only (the next impulse is 65536 samples later, past every render here).
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
  // The Delay Tap nested in the case (In ← `in`'s output), its Out the case's Out.
  DeviceId delayTap(DeviceId in, const char* port = "out", const DelayTapOptions& base = {}) {
    DelayTapOptions o = base;
    o.inGroup = group;
    DelayTapRoles r;
    take(delayTapBlocks(ids, o, &r));
    wire(in, port, r.group, kMainPort);
    return r.group;
  }
  void out(DeviceId from, std::string_view port) { wire(from, port, group, kGroupOutPort); }
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
  void place(const BlockSet& set) {
    auto e = ops::pasteBlocks(*store.current(), store.ids(), track, set);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const Status st = store.dispatch(e->command);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    group = e->devices.at(0);
    auto p = ops::insertOnPath(*store.current(), store.ids(), group, 0);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().message));
    REQUIRE(store.dispatch(p->command).ok());
  }
  // The Delay Tap's value block of `role` (the first Delay Tap on the track).
  DeviceId role(DelayRole r) const {
    DeviceId found;
    forEachDevice(*store.current(), [&](const Device& d, const Track&) {
      if (!found.valid() && d.patch == kValueDevice && delayRole(d) == r) found = d.id;
    });
    REQUIRE(found.valid());
    return found;
  }
  // Writes a value block's knob so that it gives `value` (its curve's units).
  void set(DeviceId v, double value) {
    const Device& d = *findDevice(*store.current(), v).device;
    const dsp::Curve& c = *d.curve(kValueParam);
    REQUIRE(store.dispatch(SetDeviceParam{v, std::string(kValueParam), dsp::curvePosition(c, dsp::curveOutput(c, value), 0.0)}).ok());
  }
  void tempo(double bpm) { REQUIRE(store.dispatch(SetTempoMap{{TempoPoint{0, bpm}}}).ok()); }
};

struct Player {
  std::unique_ptr<app::ProjectLink> link;
  int64_t t = 0;
  explicit Player(const Snapshot& s) {
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = false;
    c.idleUnits = false;  // (an audio track without clips is idle: its unit would be skipped)
    c.sampleRate = kRate;
    auto l = app::ProjectLink::create(c, s);
    REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
    link = std::move(*l);
    link->play(0);
  }
  // `blocks` more blocks, appended to out (2 channels).
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
    CHECK(link->lastError().empty());
  }
};

struct Rendered {
  std::vector<float> L, R;
};
Rendered render(Song& s, uint32_t blocks) {
  Player p(s.store.current());
  Rendered out;
  p.run(blocks, out.L, out.R);
  return out;
}

size_t peakIndex(const std::vector<float>& x, size_t from = 0) {
  size_t best = from;
  for (size_t i = from; i < x.size(); ++i)
    if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
  return best;
}

// Where the case's undelayed impulse lands (the path's own offset, measured: 0 unless the engine adds one).
size_t baseIndex() {
  Song s;
  IdSource ids;
  Circuit c(ids);
  c.out(c.impulse(), "out");
  s.place(c.set);
  const Rendered r = render(s, 80);
  for (size_t i = 0; i < r.L.size(); ++i)
    if (r.L[i] > 0.5f) return i;
  const size_t p = peakIndex(r.L);
  FAIL("no impulse: peak " << r.L[p] << " at " << p << ", L[0..3] " << r.L[0] << " " << r.L[1] << " " << r.L[2] << " " << r.L[3]);
  return 0;
}

}  // namespace

TEST_CASE("delay tap: an impulse lands at the time — whole samples exactly, half a sample as the cubic read; stereo") {
  const size_t b0 = baseIndex();
  {  // 0.25 s = 12000 samples, mono source into a stereo track: both channels
    Song s;
    IdSource ids;
    Circuit c(ids);
    const DeviceId tap = c.delayTap(c.impulse());
    c.out(tap, kMainPort);
    s.place(c.set);
    s.set(s.role(DelayRole::Time), 0.25);
    const Rendered r = render(s, 270);
    CHECK(peakIndex(r.L) == b0 + 12000);
    CHECK(r.L[b0 + 12000] == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(std::fabs(r.L[b0 + 11999]) < 1e-6f);
    CHECK(std::fabs(r.L[b0 + 12001]) < 1e-6f);
    double before = 0.0;
    for (size_t i = 0; i < b0 + 11999; ++i) before = std::max(before, static_cast<double>(std::fabs(r.L[i])));
    CHECK(before < 1e-6);  // nothing else: no dry signal
  }
  {  // 1000.5 samples: Catmull-Rom at a half sample
    Song s;
    IdSource ids;
    Circuit c(ids);
    c.out(c.delayTap(c.impulse()), kMainPort);
    s.place(c.set);
    s.set(s.role(DelayRole::Time), 1000.5 / kRate);
    const Rendered r = render(s, 90);
    const size_t d = b0 + 1000;
    CHECK(r.L[d - 1] == doctest::Approx(-0.0625).epsilon(0.03));
    CHECK(r.L[d] == doctest::Approx(0.5625).epsilon(0.005));
    CHECK(r.L[d + 1] == doctest::Approx(0.5625).epsilon(0.005));
    CHECK(r.L[d + 2] == doctest::Approx(-0.0625).epsilon(0.03));
  }
  {  // stereo: (impulse, impulse · 0.5) → each channel delayed by 480 samples
    Song s;
    IdSource ids;
    Circuit c(ids);
    const DeviceId imp = c.impulse(), half = c.atom("mul"), pack = c.atom("pack");
    c.wire(imp, "out", half, "a");
    c.wire(c.konst(0.5), kValueOutput, half, "b");
    c.wire(imp, "out", pack, "in0");
    c.wire(half, "out", pack, "in1");
    c.out(c.delayTap(pack), kMainPort);
    s.place(c.set);
    s.set(s.role(DelayRole::Time), 0.01);
    const Rendered r = render(s, 90);
    CHECK(r.L[b0 + 480] == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(r.R[b0 + 480] == doctest::Approx(0.5).epsilon(1e-6));
    CHECK(peakIndex(r.R) == b0 + 480);
  }
}

TEST_CASE("delay tap: sync mode follows the tempo — 1/8 is 0.25 s at 120 BPM, 1/3 s at 90 BPM; the 5 s clamp") {
  const size_t b0 = baseIndex();
  const uint32_t eighth = 10;
  REQUIRE(noteValues()[eighth].label == "1/8");
  for (const double bpm : {120.0, 90.0}) {
    CAPTURE(bpm);
    Song s;
    IdSource ids;
    Circuit c(ids);
    DelayTapOptions o;
    o.mode = 1;
    o.sync = eighth;
    o.time = 0.1;  // (not what plays)
    c.out(c.delayTap(c.impulse(), "out", o), kMainPort);
    s.place(c.set);
    s.tempo(bpm);
    const Rendered r = render(s, 340);
    const size_t want = b0 + static_cast<size_t>(std::lround(0.5 * 60.0 / bpm * kRate));
    CHECK(peakIndex(r.L) == want);
    CHECK(r.L[want] == doctest::Approx(1.0).epsilon(1e-4));
  }
  // Every note value (the circuit's arithmetic table against noteValues()): at 500 BPM a beat is 5760 samples, so each
  // value is a whole number of samples (240 … 34560).
  for (uint32_t i = 0; i < noteValues().size(); ++i) {
    CAPTURE(i);
    Song s;
    IdSource ids;
    Circuit c(ids);
    DelayTapOptions o;
    o.mode = 1;
    o.sync = i;
    c.out(c.delayTap(c.impulse(), "out", o), kMainPort);
    s.place(c.set);
    s.tempo(500.0);
    const size_t want = b0 + static_cast<size_t>(std::lround(noteValues()[i].beats * 5760.0));
    const Rendered r = render(s, static_cast<uint32_t>(want / kBlock + 4));
    CHECK(peakIndex(r.L) == want);
    CHECK(r.L[want] == doctest::Approx(1.0).epsilon(1e-3));
  }
  // The clamp: a dotted whole at 60 BPM is 6 s, played at 5 s (the circuit's min and the face's noteValueSeconds).
  {
    Song s;
    IdSource ids;
    Circuit c(ids);
    DelayTapOptions o;
    o.mode = 1;
    o.sync = 20;
    c.out(c.delayTap(c.impulse(), "out", o), kMainPort);
    s.place(c.set);
    s.tempo(60.0);
    const size_t want = b0 + static_cast<size_t>(kDelayTapMaxSeconds * kRate);
    const Rendered r = render(s, static_cast<uint32_t>(want / kBlock + 4));
    CHECK(peakIndex(r.L) == want);
    CHECK(r.L[want] == doctest::Approx(1.0).epsilon(1e-3));
  }
  CHECK(noteValueSeconds(20, 30.0) == kDelayTapMaxSeconds);
  CHECK(noteValueSeconds(eighth, 120.0) == doctest::Approx(0.25));
  CHECK(noteValueSeconds(eighth, 90.0) == doctest::Approx(1.0 / 3.0));
}

TEST_CASE("delay tap: a feedback loop through a VCA at 0.5 decays 1, 0.5, 0.25 at D, 2D, 3D") {
  const size_t b0 = baseIndex();
  Song s;
  IdSource ids;
  Circuit c(ids);
  const DeviceId imp = c.impulse(), sum = c.atom("add");
  c.wire(imp, "out", sum, "a");
  const DeviceId tap = c.delayTap(sum);
  VcaOptions vo;
  vo.inGroup = c.group;
  vo.value = dsp::curvePosition(vcaCurve(), 0.5, 0.5);
  VcaRoles vr;
  c.take(vcaBlocks(ids, vo, &vr));
  c.wire(tap, kMainPort, vr.group, kMainPort);
  c.wire(vr.group, kMainPort, sum, "b");
  c.out(tap, kMainPort);
  s.place(c.set);
  s.set(s.role(DelayRole::Time), 0.1);  // 4800 samples
  const Rendered r = render(s, 300);
  for (int k = 1; k <= 3; ++k) {
    CAPTURE(k);
    const size_t at = b0 + 4800 * static_cast<size_t>(k);
    CHECK(peakIndex(r.L, at - 100) == at);
    CHECK(r.L[at] == doctest::Approx(std::ldexp(1.0, 1 - k)).epsilon(1e-4));
  }
}

TEST_CASE("delay tap: a time change while a sine plays glides — no step, and the new time is reached") {
  Song s;
  IdSource ids;
  Circuit c(ids);
  const DeviceId ph = c.atom("phasor"), sine = c.atom("sine");
  c.wire(c.konst(1000.0), kValueOutput, ph, "hz");
  c.wire(ph, "out", sine, "phase");
  c.out(c.delayTap(sine), kMainPort);
  s.place(c.set);
  const DeviceId time = s.role(DelayRole::Time);
  s.set(time, 0.1);
  Player p(s.store.current());
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { p.link->onDocumentChange(e); });
  std::vector<float> L, R;
  p.run(150, L, R);  // 0.2 s: the delayed sine is playing
  // 100 → 110.3 ms: 10.3 periods later, a step would jump by up to |sin θ − sin(θ − 0.6π)| ≈ 1.6.
  s.set(time, 0.1103);
  p.link->flush();
  const size_t at = L.size();
  p.run(150, L, R);
  double steady = 0.0, change = 0.0;
  for (size_t i = 6000; i < at; ++i) steady = std::max(steady, static_cast<double>(std::fabs(L[i] - L[i - 1])));
  for (size_t i = at; i < L.size(); ++i) change = std::max(change, static_cast<double>(std::fabs(L[i] - L[i - 1])));
  MESSAGE("max |Δ| steady " << steady << ", across the change " << change);
  CHECK(steady == doctest::Approx(2.0 * std::sin(3.14159265358979 * 1000.0 / kRate)).epsilon(0.01));
  // A glide over 20 ms of 10.3 ms reads ≈ 1.5× faster meanwhile: the steps grow by that, never to a jump.
  CHECK(change < 2.0 * steady);
  // Meanwhile (5 … 15 ms into it) it is neither the old delay nor the new one: a glide, not a step.
  auto delayed = [](size_t n, double d) { return std::sin(2.0 * 3.14159265358979 * 1000.0 * (static_cast<double>(n) - d) / kRate); };
  double offOld = 0.0, offNew = 0.0;
  for (size_t i = at + 240; i < at + 720; ++i) {
    offOld = std::max(offOld, std::fabs(L[i] - delayed(i, 0.1 * kRate)));
    offNew = std::max(offNew, std::fabs(L[i] - delayed(i, 0.1103 * kRate)));
  }
  CHECK(offOld > 0.3);
  CHECK(offNew > 0.3);
  // After it, the sine delayed by 110.3 ms: out(n) = sin(2π·1000·(n − 5294.4)/48000).
  const size_t n = L.size() - 10;
  const double want = delayed(n, 0.1103 * kRate);
  // (the phasor's first sample is the path's base offset: measured as 0 by baseIndex in the other cases)
  CHECK(std::fabs(L[n] - want) < 0.02);
  // A sync step and a mode switch (to 1/32. = 93.75 ms at 120 BPM): param writes, no recompile (Sync and Mode reach
  // no choice: they are runtime params); the delay glides there (−16.55 ms over the 20 ms ramp: reading ≈ 1.8× as
  // fast meanwhile), the line goes on.
  const uint64_t compiles = (p.link->flush(), p.link->stats().compiles);
  s.set(s.role(DelayRole::Sync), 6.0);
  s.set(s.role(DelayRole::Mode), 1.0);
  p.link->flush();
  CHECK(p.link->stats().compiles == compiles);
  const size_t at2 = L.size();
  p.run(150, L, R);
  double jump = 0.0, level = 0.0;
  for (size_t i = at2; i < L.size(); ++i) jump = std::max(jump, static_cast<double>(std::fabs(L[i] - L[i - 1])));
  for (size_t i = at2 + 960; i < at2 + 4800; ++i) level = std::max(level, static_cast<double>(std::fabs(L[i])));
  MESSAGE("sync step + mode switch: max |Δ| " << jump << ", level 20 … 100 ms after " << level);
  CHECK(jump < 2.5 * steady);
  CHECK(level > 0.9);
  const size_t m = L.size() - 10;
  CHECK(std::fabs(L[m] - delayed(m, 0.09375 * kRate)) < 0.02);
  s.store.unsubscribe(sub);
}
