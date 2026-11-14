// DY.4: the Compressor module (pq/document/modules.h "the Compressor"; docs/plans/DY.md "DY.4") through the real lowering
// and the link, every block under RtScope. Its input is a source group on the same audio track: a level schedule
// a0 + Σ dᵢ·[beat ≥ bᵢ] (transport.beat, 120 BPM: a beat is 24000 frames) times DC or a 1 kHz sine, on both channels.
//   static     a sine staircase −40 … 0 dB (instant attack, 3 s release, Peak, Log): the output level of each stair
//              against the formula (threshold −20 dB, 4:1) within 0.1 dB, knee 0 and 12 dB; interpreter == JIT; the Gain
//              Computer's transfer curve (what the panel draws) is the same formula
//   times      a DC step −60 → 0 dB → −60 dB (threshold −20, 4:1, knee 0: 15 dB of reduction): the reduction reaches 63 %
//              one attack (10 ms) after the step up and falls to 37 % one release (100 ms) after the step down
//   lookahead  10 ms: the link reports 480 frames at 48 kHz, and the output is already reduced before the transient
//              arrives in it
//   sidechain  a Kick track's instrument (its own output muted) cabled into a Pad track's Compressor (the cablesource
//              pick, ops::setCableSource with the Sidechain switch): the pad ducks ≥ 10 dB under the kick and is
//              untouched before it (against the same render with Dry/Wet 0)
//   CPU        the stepped switches lower as literals: the default schedules one follower and no window / delay /
//              sidechain / mux; each switch adds only its own path
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "../support/blocks.h"
#include "dynamics_fixture.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/group_response.h"
#include "pq/document/lower.h"
#include "pq/document/modules.h"
#include "pq/document/store.h"
#include "pq/engine/project_compile.h"
#include "pq/graph/atoms.h"

using namespace pq;
using namespace pq::doc;
using namespace dynamics_fixture;

namespace {

double rmsOf(const std::vector<float>& c, size_t from, size_t to) {
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += static_cast<double>(c[i]) * c[i];
  return std::sqrt(s / static_cast<double>(to - from));
}

// The gain computer's formula (modules.h): reduction in dB at level x dB.
double reduction(double x, double t, double r, double w) {
  const double over = x - t;
  const double k = std::clamp(over + w / 2.0, 0.0, w);
  return (1.0 - 1.0 / r) * (k * k / (2.0 * std::max(w, 1e-3)) + std::max(0.0, over - w / 2.0));
}

// An audio track: the source group, then the Compressor (module:compressor) on its path.
struct Rig {
  DocumentStore store;
  TrackId track;
  DeviceId comp;
  ops::Edit edit(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  Rig(double a0, const std::vector<Step>& steps, bool sine) {
    store.setDeviceLibrary(&library());
    track = edit(ops::createTrack(*store.current(), store.ids(), TrackKind::Audio, {}, "A")).tracks.at(0);
    const DeviceId src = edit(ops::pasteBlocks(*store.current(), store.ids(), track, sourceBlocks(store.ids(), a0, steps, sine))).devices.at(0);
    edit(ops::insertOnPath(*store.current(), store.ids(), src, 0));
    comp = test_blocks::addModule(store, track, "compressor");
  }
  DeviceId named(std::string_view name) const {
    const DeviceId d = test_blocks::member(*store.current(), comp, name);
    REQUIRE_MESSAGE(d.valid(), std::string(name));
    return d;
  }
  // Value block `name` set to `value` in its curve's units.
  void set(std::string_view name, double value) {
    const DeviceId v = named(name);
    const dsp::Curve& c = *findDevice(*store.current(), v).device->curve(kValueParam);
    REQUIRE(store.dispatch(SetDeviceParam{v, std::string(kValueParam), dsp::curvePosition(c, dsp::curveOutput(c, value), 0.0)}).ok());
    CHECK(valueBlockUnits(*findDevice(*store.current(), v).device) == doctest::Approx(value).epsilon(1e-6));
  }
};

std::vector<Step> staircase() {  // −35 … 0 dB, a stair every quarter beat (6000 frames) after −40 dB
  std::vector<Step> s;
  for (int i = 1; i <= 8; ++i) s.push_back({0.25 * i, std::pow(10.0, (-40.0 + 5.0 * i) / 20.0)});
  return s;
}

}  // namespace

TEST_CASE("DY.4 static curve: a sine staircase −40 … 0 dB follows the formula within 0.1 dB (knee 0, 12); interp == JIT") {
  for (const double knee : {0.0, 12.0}) {
    CAPTURE(knee);
    Rig r(std::pow(10.0, -2.0), staircase(), true);
    r.set("Threshold", -20.0);
    r.set("Ratio", 4.0);
    r.set("Knee", knee);
    r.set("Attack", 0.00001);
    r.set("Release", 3.0);
    const uint32_t frames = static_cast<uint32_t>(9 * kBeat / 4);
    const golden::Channels out = render(r.store.current(), frames);
    for (int i = 0; i <= 8; ++i) {
      const double level = -40.0 + 5.0 * i;
      const size_t end = static_cast<size_t>((i + 1) * kBeat / 4);
      const double got = db(peakOf(out[0], end - 2400, end - 48));  // the stair's last 50 ms (one frame block off its edge)
      const double want = level - reduction(level, -20.0, 4.0, knee);
      CAPTURE(level);
      CHECK(std::fabs(got - want) <= 0.1);
      CHECK(peakOf(out[1], end - 2400, end - 48) == doctest::Approx(peakOf(out[0], end - 2400, end - 48)));
    }
    if (knee == 12.0) {
      const golden::Channels jit = render(r.store.current(), frames, true);
      double diff = 0.0;
      for (size_t c = 0; c < 2; ++c)
        for (size_t i = 0; i < out[c].size(); ++i) diff = std::max(diff, std::fabs(static_cast<double>(out[c][i]) - jit[c][i]));
      MESSAGE("max |interpreter - JIT| = " << diff);
      CHECK(diff <= 1e-5);
      // The panel's transfer display: the Gain Computer group's static curve is the same formula.
      LowerOptions lo;
      lo.library = &library().library();
      lo.devices = &library();
      lo.clipSlotOf = [](TrackId) { return 0u; };
      auto low = lower(*r.store.current(), lo);
      REQUIRE(low.ok());
      GroupTransfer t;
      REQUIRE(t.prepare(*r.store.current(), *low, library(), r.named("Gain Computer")).ok());
      const std::vector<float> xs = {-60.0f, -30.0f, -26.0f, -20.0f, -15.0f, -14.0f, -6.0f, 0.0f};
      std::vector<float> ys(xs.size() * t.outChannels());
      REQUIRE(t.evaluate(xs, kRate, ys) != graph::ResponseKind::Unavailable);
      for (size_t i = 0; i < xs.size(); ++i) CHECK(ys[i * t.outChannels()] == doctest::Approx(reduction(xs[i], -20.0, 4.0, 12.0)).epsilon(1e-4));
    }
  }
}

TEST_CASE("DY.4 attack and release: a DC step reaches 63 % of the reduction after the attack, 37 % after the release") {
  Rig r(0.001, {{1.0, 1.0}, {2.0, 0.001}}, false);
  r.set("Threshold", -20.0);
  r.set("Ratio", 4.0);
  r.set("Knee", 0.0);
  r.set("Attack", 0.010);
  r.set("Release", 0.100);
  const golden::Channels out = render(r.store.current(), static_cast<uint32_t>(3 * kBeat));
  const std::vector<float>& o = out[0];
  // The reduction (dB) at frame n of input level `in`.
  auto gr = [&](size_t n, double in) { return db(in) - db(std::fabs(o[n])); };
  size_t up = 0;
  while (up < o.size() && std::fabs(o[up]) < 0.01f) ++up;
  REQUIRE(up < o.size());
  CHECK(std::fabs(static_cast<double>(up) - kBeat) <= 1.0);
  size_t n63 = up;
  while (n63 < o.size() && gr(n63, 1.0) < 0.632 * 15.0) ++n63;
  MESSAGE("attack: 63 % after " << n63 - up << " frames");
  CHECK(std::fabs(static_cast<double>(n63 - up) - 480.0) <= 2.0);
  CHECK(gr(up + static_cast<size_t>(kBeat) - 100, 1.0) == doctest::Approx(15.0).epsilon(0.002));  // settled
  size_t down = up + 1000;
  while (down < o.size() && std::fabs(o[down]) > 0.01f) ++down;
  REQUIRE(down < o.size());
  size_t n37 = down;
  while (n37 < o.size() && gr(n37, 0.001) > 0.368 * 15.0) ++n37;
  MESSAGE("release: 37 % after " << n37 - down << " frames");
  CHECK(std::fabs(static_cast<double>(n37 - down) - 4800.0) <= 3.0);
}

TEST_CASE("DY.4 lookahead 10 ms: 480 frames reported at 48 kHz; the reduction starts before the transient") {
  Rig r(0.01, {{1.0, 1.0}}, false);
  r.set("Threshold", -20.0);
  r.set("Ratio", 4.0);
  r.set("Knee", 0.0);
  r.set("Attack", 0.001);
  r.set("Lookahead", 0.010);
  uint32_t latency = 0;
  const golden::Channels out = render(r.store.current(), static_cast<uint32_t>(2 * kBeat), false, &latency);
  CHECK(latency == 480);
  const std::vector<float>& o = out[0];
  size_t t = 0;  // the transient in the output (the input steps at beat 1)
  while (t < o.size() && std::fabs(o[t]) < 0.05f) ++t;
  REQUIRE(t < o.size());
  MESSAGE("transient at " << t << ", the input's step at " << kBeat);
  CHECK(std::fabs(static_cast<double>(t) - (kBeat + 480.0)) <= 1.0);
  CHECK(std::fabs(o[t - 600]) == doctest::Approx(0.01).epsilon(1e-3));  // before the detector heard the step
  CHECK(db(std::fabs(o[t - 1]) / 0.01) < -12.0);                        // already reduced (15 dB target, attack 1 ms)
  CHECK(std::fabs(o[t]) < 0.2f);                                       // the transient arrives reduced
}

TEST_CASE("DY.4 sidechain: a kick on another track ducks a pad through the cable picker") {
  DocumentStore store;
  store.setDeviceLibrary(&library());
  auto edit = [&](const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  };
  auto midi = [&](const char* name, std::vector<int32_t> degrees, Ticks start, Ticks length) {
    const TrackId t = edit(ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, name)).tracks.at(0);
    std::vector<Note> notes;
    for (const int32_t d : degrees) notes.push_back(playback_fixture::note(store.ids(), start, length, d));
    REQUIRE(store.dispatch(AddClip{t, playback_fixture::clip(store.ids(), 0, 4 * kPpq, notes, name)}).ok());
    return t;
  };
  const TrackId kick = midi("Kick", {-12}, 2 * kPpq, kPpq / 2);
  const TrackId pad = midi("Pad", {0, 4, 7}, 0, 4 * kPpq);
  // Kick's own output muted (a Utility after its instrument): the master is the pad alone.
  const DeviceId util = test_blocks::addModule(store, kick, "utility");
  const DeviceId mute = test_blocks::member(*store.current(), util, "Mute");
  REQUIRE(store.dispatch(SetDeviceParam{mute, std::string(kValueParam), 1.0}).ok());
  const DeviceId comp = test_blocks::addModule(store, pad, "compressor");
  auto set = [&](std::string_view name, double value) {
    const DeviceId v = test_blocks::member(*store.current(), comp, name);
    REQUIRE(v.valid());
    const dsp::Curve& c = *findDevice(*store.current(), v).device->curve(kValueParam);
    REQUIRE(store.dispatch(SetDeviceParam{v, std::string(kValueParam), dsp::curvePosition(c, dsp::curveOutput(c, value), 0.0)}).ok());
  };
  set("Threshold", -50.0);
  set("Ratio", 20.0);
  set("Attack", 0.001);
  set("Release", 0.030);
  // The pick: Kick's Pre FX (its instrument) into the Sidechain group's input, the Sidechain switch on.
  const DeviceId on = [&] {
    for (const Device* d : groupMembers(findTrackPtr(*store.current(), pad)->devices, comp))
      if (d->name == "Sidechain" && isValueDevice(*d)) return d->id;
    return DeviceId{};
  }();
  REQUIRE(on.valid());
  DeviceId scGroup;
  for (const Device* d : groupMembers(findTrackPtr(*store.current(), pad)->devices, comp))
    if (d->name == "Sidechain" && isGroupDevice(*d)) scGroup = d->id;
  REQUIRE(scGroup.valid());
  std::optional<Wire> pre;
  for (const ops::CableSource& s : ops::cableSources(*store.current(), pad))
    if (s.track == kick && !s.post) pre = s.ends;
  REQUIRE(pre.has_value());
  edit(ops::setCableSource(*store.current(), store.ids(), &library(), scGroup, std::string(kMainPort), pre, on, std::string(kValueParam)));
  CHECK(valueBlockUnits(*findDevice(*store.current(), on).device) == 1.0);
  const uint32_t frames = static_cast<uint32_t>(3 * kBeat);
  const golden::Channels ducked = render(store.current(), frames);
  set("Dry/Wet", 0.0);
  const golden::Channels dry = render(store.current(), frames);
  const auto window = [&](const golden::Channels& a, double b0, double b1) {
    return db(rmsOf(a[0], static_cast<size_t>(b0 * kBeat), static_cast<size_t>(b1 * kBeat)));
  };
  const double before = window(ducked, 0.5, 1.5) - window(dry, 0.5, 1.5);
  const double during = window(ducked, 2.05, 2.45) - window(dry, 2.05, 2.45);
  MESSAGE("pad before the kick " << before << " dB, under it " << during << " dB");
  CHECK(window(dry, 0.5, 1.5) > -40.0);
  CHECK(std::fabs(before) < 0.01);
  CHECK(during < -10.0);
}

TEST_CASE("DY.4 CPU: the switches lower as literals — only the chosen detector, envelope and paths are compiled") {
  Rig r(0.5, {}, false);
  auto count = [&](const char* atom) {
    const std::map<std::string, int> m = atomsOf(r.store.current());
    const auto it = m.find(atom);
    return it == m.end() ? 0 : it->second;
  };
  // Default (Peak, Log, no Auto, no lookahead, sidechain off, Listen off): one follower, no window / lerp / svf / latency,
  // no mux left.
  CHECK(count("follow") == 1);
  CHECK(count("window") == 0);
  CHECK(count("sqrt") == 0);
  CHECK(count("lerp") == 0);
  CHECK(count("svf") == 0);
  CHECK(count("latency") == 0);
  CHECK(count("mux") == 0);
  const int mins = count("min");  // (the mixer's: the Expand cap adds one)
  // RMS, Auto, 10 ms lookahead, the sidechain with its EQ on: the window, two followers, the delay, the lerp, the band.
  r.set("Detector", 1.0);
  r.set("Auto", 1.0);
  r.set("Lookahead", 0.010);
  r.set("Sidechain", 1.0);
  r.set("EQ On", 1.0);
  CHECK(count("window") == 1);
  CHECK(count("sqrt") == 1);
  CHECK(count("follow") == 2);
  CHECK(count("latency") == 1);
  CHECK(count("lerp") == 1);
  CHECK(count("svf") == 1);
  CHECK(count("mux") == 0);
  // Expand (Peak): the cap; Lin: the follower moves before the computer (still two with Auto).
  r.set("Detector", 0.0);
  r.set("Law", 1.0);
  r.set("Envelope", 1.0);
  CHECK(count("min") == mins + 1);
  CHECK(count("window") == 0);
  CHECK(count("follow") == 2);
  CHECK(count("mux") == 0);
  // Q261: RMS with Expand (impossible with the old 3-way Mode): the window and the cap.
  r.set("Detector", 1.0);
  CHECK(count("window") == 1);
  CHECK(count("min") == mins + 1);
  CHECK(count("mux") == 0);
}

TEST_CASE("AU.dsp H4: a Compressor on a track that falls silent sleeps after its tail (Peak, RMS, Expand, Lin, Auto) and wakes") {
  struct Case {
    const char* name;
    double detector, law, envelope, autoRelease;
  };
  for (const Case& c : {Case{"Peak Log", 0, 0, 0, 0}, Case{"RMS", 1, 0, 0, 0}, Case{"Expand Log", 0, 1, 0, 0},
                        Case{"Expand Lin", 0, 1, 1, 0}, Case{"RMS Expand", 1, 1, 0, 0}, Case{"Peak Auto", 0, 0, 0, 1},
                        Case{"Lin Auto", 0, 0, 1, 1}}) {
    CAPTURE(std::string(c.name));
    NoteRig r("compressor", {{0, kPpq / 4}, {8 * kPpq, kPpq / 4}});
    r.set("Threshold", -30.0);  // the synth's note is compressed: a tail of reduction to release
    r.set("Release", 0.02);     // to ε = 2^-30 dB: ≈ 24 τ, Auto's slow τ 4 · Release
    r.set("Detector", c.detector);
    r.set("Law", c.law);
    r.set("Envelope", c.envelope);
    r.set("Auto", c.autoRelease);
    checkSleeps(r.store.current(), 8.0, 3.0);
  }
}


TEST_CASE("AU.dsp M4: one inf sample does not hold the Compressor's reduction") {
  // A −12 dB sine compressed (threshold −30 dB) with one inf sample at beat 1: after the release the output is the
  // clean render's (the follower read the inf as 0 — one frame of release, recovered at the 2 ms attack —; before, as
  // FLT_MAX: ~10^38 dB of reduction).
  auto renderWith = [](bool bad) {
    DocumentStore store;
    store.setDeviceLibrary(&library());
    auto e = ops::createTrack(*store.current(), store.ids(), TrackKind::Audio, {}, "A");
    REQUIRE(e.ok());
    REQUIRE(store.dispatch(e->command).ok());
    const TrackId t = e->tracks.at(0);
    auto p = ops::pasteBlocks(*store.current(), store.ids(), t, spikeBlocks(store.ids(), 0.25, bad, 1e38));
    REQUIRE(p.ok());
    REQUIRE(store.dispatch(p->command).ok());
    auto ins = ops::insertOnPath(*store.current(), store.ids(), p->devices.at(0), 0);
    REQUIRE(ins.ok());
    REQUIRE(store.dispatch(ins->command).ok());
    const DeviceId comp = test_blocks::addModule(store, t, "compressor");
    const DeviceId th = test_blocks::member(*store.current(), comp, "Threshold");
    const dsp::Curve& c = *findDevice(*store.current(), th).device->curve(kValueParam);
    REQUIRE(store.dispatch(SetDeviceParam{th, std::string(kValueParam), dsp::curvePosition(c, -30.0, 0.0)}).ok());
    return render(store.current(), static_cast<uint32_t>(2 * kBeat));
  };
  const golden::Channels clean = renderWith(false), inf = renderWith(true);
  const size_t from = static_cast<size_t>(kBeat + 0.02 * kRate);  // (the inf itself passes: a compressor has no clip)
  double d = 0.0;
  for (size_t i = from; i < clean[0].size(); ++i) d = std::max(d, std::fabs(static_cast<double>(inf[0][i]) - clean[0][i]));
  CHECK(d < 1e-6);
}

namespace {

// The Expand law (modules.h): below T, GR = min((R − 1)·body, 70 dB) with s = T − x.
double expansion(double x, double t, double r, double w) {
  const double under = t - x;
  const double k = std::clamp(under + w / 2.0, 0.0, w);
  return std::min((r - 1.0) * (k * k / (2.0 * std::max(w, 1e-3)) + std::max(0.0, under - w / 2.0)), kCompressorExpandCapDb);
}

std::vector<Step> dcStaircase() {  // −40 … 0 dB DC, a stair every quarter beat
  std::vector<Step> s;
  for (int i = 1; i <= 8; ++i) s.push_back({0.25 * i, std::pow(10.0, (-40.0 + 5.0 * i) / 20.0)});
  return s;
}

}  // namespace

TEST_CASE("AU.dsp M9: static curves by value — Expand (Peak and RMS) and RMS Compress follow the laws within 0.05 dB") {
  struct Case {
    const char* name;
    double detector, law;
    bool sine;  // RMS reads a sine 3.01 dB under its peak; Peak reads DC exactly
  };
  for (const Case& c : {Case{"Peak Expand", 0, 1, false}, Case{"RMS Expand", 1, 1, true}, Case{"RMS Compress", 1, 0, true}}) {
    CAPTURE(std::string(c.name));
    Rig r(std::pow(10.0, -2.0), c.sine ? staircase() : dcStaircase(), c.sine);
    r.set("Detector", c.detector);
    r.set("Law", c.law);
    r.set("Threshold", -20.0);
    r.set("Ratio", c.law > 0 ? 2.0 : 4.0);
    r.set("Knee", 6.0);
    r.set("Attack", 0.00001);
    r.set("Release", 0.001);
    const golden::Channels out = render(r.store.current(), static_cast<uint32_t>(9 * kBeat / 4));
    for (int i = 0; i <= 8; ++i) {
      const double level = -40.0 + 5.0 * i;  // the peak (sine) or DC level
      const double detected = c.sine ? level + 20.0 * std::log10(std::sqrt(0.5)) : level;
      const double gr = c.law > 0 ? expansion(detected, -20.0, 2.0, 6.0) : reduction(detected, -20.0, 4.0, 6.0);
      const size_t end = static_cast<size_t>((i + 1) * kBeat / 4);
      const double got = db(peakOf(out[0], end - 2400, end - 48));
      CAPTURE(level);
      CHECK(std::fabs(got - (level - gr)) <= 0.05);
    }
  }
}

TEST_CASE("AU.dsp M9: the Lin envelope follows the linear level (63 % of a DC step after one attack), then the law") {
  Rig r(0.001, {{1.0, 1.0}}, false);
  r.set("Threshold", -20.0);
  r.set("Ratio", 4.0);
  r.set("Knee", 0.0);
  r.set("Attack", 0.010);
  r.set("Release", 0.100);
  r.set("Envelope", 1.0);
  const golden::Channels out = render(r.store.current(), static_cast<uint32_t>(2 * kBeat));
  const std::vector<float>& o = out[0];
  size_t up = 0;
  while (up < o.size() && std::fabs(o[up]) < 0.01f) ++up;
  REQUIRE(up < o.size());
  // k frames into the step the level envelope is 1 − 0.999·ra^k (ra = e^(−1/480)); the gain follows it at once.
  for (const size_t k : {size_t{48}, size_t{480}, size_t{2400}}) {
    const double env = 1.0 - 0.999 * std::exp(-static_cast<double>(k) / 480.0);
    const double want = -reduction(db(env), -20.0, 4.0, 0.0);
    CAPTURE(k);
    CHECK(std::fabs(db(std::fabs(o[up + k - 1])) - want) <= 0.05);
  }
  // (the Log envelope at one attack: 63 % of the 15 dB reduction, 9.5 dB; the Lin one already 12.0 dB)
  CHECK(db(std::fabs(o[up + 479])) < -11.9);
}

TEST_CASE("AU.dsp M9: Auto release — max(fast at rel / 4, slow at rel · 4 − 3 dB) after a step down, by value") {
  Rig r(1.0, {{1.0, 0.001}}, false);  // 0 dB (15 dB of reduction, settled), then −60 dB at beat 1
  r.set("Threshold", -20.0);
  r.set("Ratio", 4.0);
  r.set("Knee", 0.0);
  r.set("Attack", 0.00001);
  r.set("Release", 0.100);
  r.set("Auto", 1.0);
  const golden::Channels out = render(r.store.current(), static_cast<uint32_t>(4 * kBeat));
  const std::vector<float>& o = out[0];
  size_t down = static_cast<size_t>(kBeat) - 10;
  while (down < o.size() && std::fabs(o[down]) > 0.01f) ++down;
  REQUIRE(down < o.size());
  for (const double t : {0.005, 0.05, 0.2, 0.5}) {
    const double k = std::round(t * kRate) + 1.0;  // release updates by then
    const double fast = 15.0 * std::exp(-k / (0.025 * kRate)), slow = 15.0 * std::exp(-k / (0.4 * kRate));
    const double gr = std::max({fast, slow - kCompressorAutoOffsetDb, 0.0});
    const double got = db(0.001) - db(std::fabs(o[down + static_cast<size_t>(k) - 1]));
    CAPTURE(t);
    MESSAGE("Auto release at " << t << " s: " << got << " dB (law " << gr << ")");
    CHECK(std::fabs(got - gr) <= 0.05);
  }
}
