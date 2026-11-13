// DT.1 (docs/plans/DT.md): the Delay Tap module through the link (the interpreter; every block inside RtScope: no
// allocation on the audio thread — its 5 s ring is allocated off-thread with the unit's state):
//   - an impulse lands where the time says: a whole number of samples exactly, half a sample as the cubic read's
//     (−1/16, 9/16, 9/16, −1/16); stereo in, stereo out (each channel delayed alike)
//   - sync mode: 1/8 at 120 BPM = 0.25 s, at 90 BPM = 1/3 s (the transport's tempo per block)
//   - a feedback loop (Out → VCA × 0.5 → back into In, summed with the impulse) decays 1, 0.5, 0.25 at D, 2D, 3D
//   - a time change while a 1 kHz sine plays glides (delay_fade's head, 30 ms): no step in the output, one sine
//     throughout (no crossfade), and the new delay is reached
// DT.2: Time at 0 is 1 sample (an impulse, a loop through a VCA at 1 sample); a mode switch and a sync step crossfade:
// their click energy is the ideal equal-power crossfade's, against the old 20 ms param ramp's (measured).
// Each case is a group on an audio track: a source (atoms) → the Delay Tap (nested, as the node menu pastes it) → Out.
#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <ostream>
#include <memory>
#include <numbers>
#include <utility>
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
  explicit Player(const Snapshot& s, bool jit = false) {
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = jit;  // DT.4: the Repitch case renders on both (bit for bit)
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
  // A glide of 10.3 ms over 30 ms reads at ≈ 0.66× meanwhile (a lower pitch): the steps shrink, never a jump.
  CHECK(change < 2.0 * steady);
  // One sine throughout (a glide, not a crossfade: two 1 kHz sines 10.3 periods apart would sum to ≈ 0.83 midway):
  // every 2 ms window of the glide still peaks at 1.
  for (size_t w = at; w + 96 <= at + 1440; w += 96) {
    double peak = 0.0;
    for (size_t i = w; i < w + 96; ++i) peak = std::max(peak, static_cast<double>(std::fabs(L[i])));
    CAPTURE(w - at);
    CHECK(peak > 0.98);
  }
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
  // no choice: they are runtime params); the delay glides there (−16.55 ms over 30 ms: reading ≈ 1.55× as fast
  // meanwhile, inside the octave bound), the line goes on.
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

// ---- DT.2: the 1-sample floor, click-free jumps -------------------------------------------------------------------

namespace {

// The click measure: the energy of the third difference x[n] − 3x[n−1] + 3x[n−2] − x[n−3] (a high-pass of 18 dB / oct:
// a 220 Hz sine passes at 2.4e-5 of its amplitude, a step or a kink in full) over [from, to).
double clickEnergy(const std::vector<float>& x, size_t from, size_t to) {
  double e = 0.0;
  for (size_t n = std::max<size_t>(from, 3); n < to && n < x.size(); ++n) {
    const double d = static_cast<double>(x[n]) - 3.0 * x[n - 1] + 3.0 * x[n - 2] - static_cast<double>(x[n - 3]);
    e += d * d;
  }
  return e;
}
double maxStep(const std::vector<float>& x, size_t from, size_t to) {
  double m = 0.0;
  for (size_t n = std::max<size_t>(from, 1); n < to && n < x.size(); ++n)
    m = std::max(m, static_cast<double>(std::fabs(x[n] - x[n - 1])));
  return m;
}

constexpr double kSineHz = 220.0;
double sineAt(double n, double delaySamples) {
  return std::sin(2.0 * std::numbers::pi * kSineHz * (n - delaySamples) / kRate);
}
// The ideal: the sine delayed by `from` before frame `at`, by `to` after `at + fade`, and between the two read heads'
// equal-power crossfade (as delay_fade: the first frame of the fade at p = 1/fade).
std::vector<float> idealCrossfade(size_t n, size_t at, double from, double to, size_t fade) {
  std::vector<float> y(n);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i);
    if (i < at) y[i] = static_cast<float>(sineAt(t, from));
    else if (i >= at + fade) y[i] = static_cast<float>(sineAt(t, to));
    else {
      const double p = static_cast<double>(i - at + 1) / static_cast<double>(fade);
      y[i] = static_cast<float>(std::sin(std::numbers::pi / 2 * p) * sineAt(t, to) + std::cos(std::numbers::pi / 2 * p) * sineAt(t, from));
    }
  }
  return y;
}

// A 220 Hz sine into `make`'s delay, played 0.6 s, then `change` (a document edit), then 0.3 s more. Returns the left
// channel and the frame where the change takes effect (the first block after the flush).
template <class Make, class Change>
std::pair<std::vector<float>, size_t> switchRender(Make make, Change change, bool jit = false) {
  Song s;
  IdSource ids;
  Circuit c(ids);
  const DeviceId ph = c.atom("phasor"), sine = c.atom("sine");
  c.wire(c.konst(kSineHz), kValueOutput, ph, "hz");
  c.wire(ph, "out", sine, "phase");
  make(c, sine);
  s.place(c.set);
  s.tempo(120.0);
  Player p(s.store.current(), jit);
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { p.link->onDocumentChange(e); });
  std::vector<float> L, R;
  p.run(450, L, R);  // 0.6 s
  const uint64_t compiles = (p.link->flush(), p.link->stats().compiles);
  change(s);
  p.link->flush();
  CHECK(p.link->stats().compiles == compiles);  // a param write, no recompile
  const size_t at = L.size();
  p.run(225, L, R);
  s.store.unsubscribe(sub);
  return {std::move(L), at};
}

}  // namespace

TEST_CASE("delay tap: the minimum is 1 sample — Time at 0 delays an impulse by 1; a loop at 1 sample through a VCA") {
  const size_t b0 = baseIndex();
  {
    Song s;
    IdSource ids;
    Circuit c(ids);
    c.out(c.delayTap(c.impulse()), kMainPort);
    s.place(c.set);
    s.set(s.role(DelayRole::Time), 0.0);
    const Rendered r = render(s, 80);
    CHECK(r.L[b0] == 0.0f);
    CHECK(r.L[b0 + 1] == 1.0f);
    CHECK(r.L[b0 + 2] == 0.0f);
  }
  {  // Out → VCA × 0.5 → In (+ the impulse): 1, 0.5, 0.25, … one sample apart (the loop runs per sample)
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
    s.set(s.role(DelayRole::Time), 0.0);
    const Rendered r = render(s, 80);
    CHECK(r.L[b0] == 0.0f);
    for (int k = 1; k <= 8; ++k) {
      CAPTURE(k);
      CHECK(r.L[b0 + static_cast<size_t>(k)] == doctest::Approx(std::ldexp(1.0, 1 - k)).epsilon(1e-4));
    }
  }
}

TEST_CASE("delay tap: in Crossfade, a mode switch and a sync step while a sine plays crossfade — no click, unlike the old "
          "20 ms ramp") {
  // 375 ms (Time) and 1/4 = 500 ms (Sync at 120 BPM): ms → ♪ moves the delay by 6000 samples. The old behaviour, for
  // the measure: delay_cubic whose time (a value block) the scheduler's param ramp glides there in 20 ms (the read
  // head running backwards at 5.25×: the click the user heard). DT.4: Glide set to Crossfade (DT.2's behaviour).
  constexpr size_t kFade = static_cast<size_t>(kDelayTapFadeSeconds * kRate);
  const double d375 = 0.375 * kRate, d500 = 0.5 * kRate;
  DelayTapOptions o;
  o.glide = 1;
  o.time = dsp::curvePosition(delayTimeCurve(), dsp::curveOutput(delayTimeCurve(), 0.375), 0.0);
  o.sync = kNoteValueQuarter;
  auto tap = [&](Circuit& c, DeviceId sine) { c.out(c.delayTap(sine, "out", o), kMainPort); };
  const auto [modeL, modeAt] = switchRender(tap, [](Song& s) { s.set(s.role(DelayRole::Mode), 1.0); });
  // A sync step: 1/4 → 3/16 (a dotted eighth, index 12, 375 ms) in sync mode.
  DelayTapOptions os = o;
  os.mode = 1;
  auto tapSync = [&](Circuit& c, DeviceId sine) { c.out(c.delayTap(sine, "out", os), kMainPort); };
  const auto [syncL, syncAt] = switchRender(tapSync, [](Song& s) { s.set(s.role(DelayRole::Sync), 12.0); });
  // The old path: delay_cubic with its time a value block (0 … 5 s), 375 → 500 ms.
  auto ramp = [&](Circuit& c, DeviceId sine) {
    const DeviceId d = c.atom("delay_cubic"), v = c.ids.device();
    auto dev = std::make_shared<Device>(*makeDevice(v, std::string(kValueDevice), DeviceKind::AudioEffect));
    dev->name = "time";
    dev->group = c.group;
    dev->params = {DeviceValue{std::string(kValueParam), 0.375 / 5.0}};
    dev->curves = {ControlCurve{std::string(kValueParam), dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 5.0)}};
    c.set.devices.push_back(std::move(dev));
    for (DevicePtr& x : c.set.devices)
      if (x->id == d) {
        auto y = std::make_shared<Device>(*x);
        y->settings.push_back(DeviceValue{"max", 5.0});
        x = std::move(y);
      }
    c.wire(sine, "out", d, "x");
    c.wire(v, kValueOutput, d, "time");
    c.out(d, "out");
  };
  const auto [rampL, rampAt] = switchRender(ramp, [](Song& s) {
    DeviceId time;  // (pasting gave the blocks fresh ids: found by name)
    forEachDevice(*s.store.current(), [&](const Device& d, const Track&) {
      if (d.patch == kValueDevice && d.name == "time") time = d.id;
    });
    REQUIRE(time.valid());
    s.set(time, 0.5);
  });

  const std::vector<float> idealMode = idealCrossfade(modeL.size(), modeAt, d375, d500, kFade);
  const std::vector<float> idealSync = idealCrossfade(syncL.size(), syncAt, d500, d375, kFade);
  // Before the switch every render is the sine at its delay (the measure's floor).
  CHECK(std::fabs(modeL[modeAt - 5] - idealMode[modeAt - 5]) < 2e-4f);
  CHECK(std::fabs(syncL[syncAt - 5] - idealSync[syncAt - 5]) < 2e-4f);
  // After the fade: the new delay.
  CHECK(std::fabs(modeL[modeAt + kFade + 100] - idealMode[modeAt + kFade + 100]) < 2e-4f);
  CHECK(std::fabs(syncL[syncAt + kFade + 100] - idealSync[syncAt + kFade + 100]) < 2e-4f);
  // Over the switch (10 ms before … 60 ms after): the click energy, against the ideal crossfade and the old ramp.
  const size_t w0 = 480, w1 = 2880;
  const double eMode = clickEnergy(modeL, modeAt - w0, modeAt + w1), eModeIdeal = clickEnergy(idealMode, modeAt - w0, modeAt + w1);
  const double eSync = clickEnergy(syncL, syncAt - w0, syncAt + w1), eSyncIdeal = clickEnergy(idealSync, syncAt - w0, syncAt + w1);
  const double eRamp = clickEnergy(rampL, rampAt - w0, rampAt + w1);
  const double sMode = maxStep(modeL, modeAt - w0, modeAt + w1), sModeIdeal = maxStep(idealMode, modeAt - w0, modeAt + w1);
  const double sSync = maxStep(syncL, syncAt - w0, syncAt + w1), sSyncIdeal = maxStep(idealSync, syncAt - w0, syncAt + w1);
  const double sRamp = maxStep(rampL, rampAt - w0, rampAt + w1);
  MESSAGE("click energy (3rd difference²): mode switch " << eMode << " (ideal crossfade " << eModeIdeal << "), sync step "
          << eSync << " (ideal " << eSyncIdeal << "), old 20 ms ramp " << eRamp);
  MESSAGE("max |Δ|: mode switch " << sMode << " (ideal " << sModeIdeal << "), sync step " << sSync << " (ideal " << sSyncIdeal
          << "), old ramp " << sRamp);
  CHECK(eMode < 1.5 * eModeIdeal);
  CHECK(eSync < 1.5 * eSyncIdeal);
  CHECK(eRamp > 1000.0 * eMode);
  CHECK(sMode < 1.02 * sModeIdeal + 1e-4);
  CHECK(sSync < 1.02 * sSyncIdeal + 1e-4);
  CHECK(sRamp > 3.0 * sMode);
}

// ---- DT.4: Repitch (the default) ------------------------------------------------------------------------------------

TEST_CASE("delay tap: Repitch (the default) — a sync step 1/16 → 1/4 while a sine plays bends the pitch, no click, no "
          "recompile; Glide switches without one; the JIT renders it bit for bit") {
  // 1/16 = 125 ms → 1/4 = 500 ms at 120 BPM: the read head glides 18000 samples over 0.75 s (delay_fade's Repitch:
  // N = 2·Δ), the pitch down to 1/4 rate at the middle. switchRender checks no recompile.
  DelayTapOptions o;
  o.mode = 1;
  o.sync = 7;  // 1/16
  REQUIRE(noteValues()[7].label == "1/16");
  auto tap = [&](Circuit& c, DeviceId sine) { c.out(c.delayTap(sine, "out", o), kMainPort); };
  auto step = [](Song& s) { s.set(s.role(DelayRole::Sync), static_cast<double>(kNoteValueQuarter)); };
  const auto [L, at] = switchRender(tap, step);
  const auto [J, atJ] = switchRender(tap, step, true);
  // Interpreter == JIT, bit for bit, through the switch and the glide.
  REQUIRE(atJ == at);
  REQUIRE(J.size() == L.size());
  size_t differ = 0;
  for (size_t i = 0; i < L.size(); ++i) differ += std::bit_cast<uint32_t>(L[i]) != std::bit_cast<uint32_t>(J[i]);
  CHECK(differ == 0);
  // Click-free: over the switch (10 ms before … 60 ms after) the high-pass energy and the largest step stay the steady
  // sine's (measured over as long a window before it); a crossfade would sum two sines 82.5 periods apart (cancelling
  // midway) and the old ramp's 20 ms sweep is the click the user heard (the Crossfade case above: > 1000× the energy).
  const size_t w0 = 480, w1 = 2880, n = w0 + w1;
  const double e = clickEnergy(L, at - w0, at + w1), eSteady = clickEnergy(L, at - w0 - n, at - w0);
  const double st = maxStep(L, at - w0, at + w1), stSteady = maxStep(L, at - w0 - n, at - w0);
  MESSAGE("Repitch sync step: click energy " << e << " (steady sine " << eSteady << "), max |Δ| " << st << " (steady " << stSteady << ")");
  // What the glide adds is the float read position's rounding (a 2⁻¹¹ … 2⁻⁹-sample grid at 6000 … 24000 samples: an
  // error ≤ w·2⁻¹⁰ ≈ 3e-5, below −90 dB, white, so it fills the high-pass: a few × the pure sine's tiny energy). A click is
  // of another order: one step of the read rate (1 → 1/4, a linear glide's start) kinks the sine's slope by 0.75·w, a
  // third difference of ≈ 0.02 — 4e-4 of energy in one sample, ~400× the steady window's.
  CHECK(e < 10.0 * eSteady);
  CHECK(st < 1.02 * stSteady + 1e-4);
  // The pitch bends: 200 … 300 ms in (the read rate ~0.3 … 0.4), one sine at full level (one head) whose zero crossings
  // come far slower than 220 Hz's.
  size_t crossings = 0;
  double peak = 0.0;
  for (size_t i = at + 9600; i < at + 14400 && i < L.size(); ++i) {
    crossings += (L[i - 1] < 0.0f) != (L[i] < 0.0f);
    peak = std::max(peak, static_cast<double>(std::fabs(L[i])));
  }
  MESSAGE("200 … 300 ms into the glide: " << crossings << " zero crossings (220 Hz unbent: 44), peak " << peak);
  CHECK(crossings < 25);
  CHECK(peak > 0.95);
  // Glide → Crossfade is a param write too (switchRender: no recompile), and the output goes on unbroken.
  auto toCrossfade = [](Song& s) { s.set(s.role(DelayRole::Glide), 1.0); };
  const auto [X, atX] = switchRender(tap, toCrossfade);
  CHECK(maxStep(X, atX - w0, atX + w1) < 1.02 * stSteady + 1e-4);
}
