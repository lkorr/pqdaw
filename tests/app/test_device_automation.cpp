// T4.18: device automation through the playback link — a filter-cutoff sweep, a macro lane and a chain volume lane
// reach the DSP (and match the static render where they hold a value), a wire-amount lane drives its wire, an automated
// gain jump is click-free (block rate + the `gain` stage's smoothing), a hand-written value never reaches an automated
// slot, and a stopped project with device lanes costs what an idle one does (no automation writes, the engine sleeps).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/engine/timeline.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr Ticks kBar = kTicksPerWhole;  // 120 BPM 4/4: 2 s

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s, float sleepAfterMs = 0.0f) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = sleepAfterMs;
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
  uint32_t blocksFor(double seconds) const { return static_cast<uint32_t>(seconds * kRate / kBlock); }
};

// One MIDI track with BasicSynth holding one note for `bars` bars (bright: a saw-like spectrum; else a sine-like one).
struct Song {
  DocumentStore store;
  TrackId t;
  DeviceId inst;
  explicit Song(bool bright, int bars = 4) {
    IdSource& ids = store.ids();
    t = ids.track();
    inst = ids.device();
    Batch b{"song", {}};
    b.commands.push_back(SetTempoMap{{{0, 120.0, TempoCurve::Step}}});
    b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "T", inst), {}, kAppend});
    b.commands.push_back(SetDeviceParam{inst, "bright", bright ? 1.0 : 0.0});
    b.commands.push_back(SetDeviceParam{inst, "sustain", 1.0});
    std::vector<Note> notes{playback_fixture::note(ids, 0, bars * kBar, 0)};
    b.commands.push_back(AddClip{t, playback_fixture::clip(ids, 0, bars * kBar, notes, "held")});
    must(b);
  }
  void must(Command c) {
    const Status st = store.dispatch(std::move(c));
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  DeviceId add(const char* patch) {
    auto e = ops::addDevice(*store.current(), store.ids(), t, patch, DeviceKind::AudioEffect);
    REQUIRE(e.ok());
    must(e->command);
    return e->devices.at(0);
  }
  // A lane on `r` with `points` ({ticks, value, curve}).
  LaneId lane(ParamRef r, std::string name, std::vector<Breakpoint> points) {
    auto l = std::make_shared<AutomationLane>();
    l->id = store.ids().lane();
    l->param = r;
    l->paramName = std::move(name);
    for (Breakpoint& p : points) p.id = store.ids().breakpoint();
    l->points = std::move(points);
    must(AddAutomationLane{t, l});
    return l->id;
  }
};

double rms(const golden::Channels& a, size_t from, size_t to) {
  double s = 0.0;
  size_t n = 0;
  for (const auto& c : a)
    for (size_t i = from; i < to && i < c.size(); ++i, ++n) s += static_cast<double>(c[i]) * c[i];
  return n != 0 ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}
// RMS of the first difference: a cheap measure of high-frequency energy (a brighter signal has larger steps).
double rmsD1(const golden::Channels& a, size_t from, size_t to) {
  double s = 0.0;
  size_t n = 0;
  for (const auto& c : a)
    for (size_t i = std::max<size_t>(from, 1); i < to && i < c.size(); ++i, ++n) {
      const double d = static_cast<double>(c[i]) - c[i - 1];
      s += d * d;
    }
  return n != 0 ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}
double maxDiff(const golden::Channels& a, const golden::Channels& b, size_t from, size_t to) {
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c)
    for (size_t i = from; i < to && i < a[c].size() && i < b[c].size(); ++i)
      m = std::max(m, std::fabs(static_cast<double>(a[c][i]) - b[c][i]));
  return m;
}
golden::Channels play(Song& s, double seconds) {
  auto link = makeLink(s.store.current());
  Driver d{*link};
  link->play(0);
  d.run(d.blocksFor(seconds));
  CHECK(link->lastError().empty());
  return d.out;
}

}  // namespace

TEST_CASE("device automation: a filter-cutoff sweep reaches Auto Filter; where the lane holds, it equals the static setting") {
  Song s(true);
  const DeviceId af = s.add("auto_filter");
  s.must(SetDeviceParam{af, "res", 0.1});
  Song ref(true);  // the same song with the filter fixed at the lane's first value
  const DeviceId afRef = ref.add("auto_filter");
  ref.must(SetDeviceParam{afRef, "res", 0.1});
  ref.must(SetDeviceParam{afRef, "freq", 150.0});
  // 150 Hz for the first bar, then up to 12 kHz over the second (linear in Hz), then held.
  s.lane(ParamRef::device(af, "freq"), "freq", {{0, 150.0f, 0.0f, {}}, {kBar, 150.0f, 0.0f, {}}, {2 * kBar, 12000.0f, 0.0f, {}}});
  const golden::Channels out = play(s, 6.0);
  const golden::Channels st = play(ref, 6.0);
  const auto at = [](double sec) { return static_cast<size_t>(sec * kRate); };
  // Bar 1 (0–2 s): the lane holds 150 Hz: the static render, sample for sample.
  CHECK(maxDiff(out, st, 0, at(1.9)) <= 1e-6);
  // The sweep opens the filter: high-frequency energy grows through the start of the second bar (≈ 450 Hz at 2.06 s,
  // ≈ 1.6 kHz at 2.25 s), and the static filter stays closed.
  const double hf0 = rmsD1(out, at(1.0), at(1.9)), hfA = rmsD1(out, at(2.04), at(2.08)), hf1 = rmsD1(out, at(2.15), at(2.35));
  MESSAGE("HF (first-difference RMS): 150 Hz " << hf0 << ", ~450 Hz " << hfA << ", ~1.6 kHz " << hf1);
  CHECK(hfA > 1.5 * hf0);
  CHECK(hf1 > hfA);
  CHECK(rmsD1(st, at(4.5), at(5.5)) < 1.5 * hf0);
  // Held at 12 kHz (4–6 s), it plays as the filter set to 12 kHz by hand (the filter's memory of the sweep has decayed).
  Song open(true);
  const DeviceId afOpen = open.add("auto_filter");
  open.must(SetDeviceParam{afOpen, "res", 0.1});
  open.must(SetDeviceParam{afOpen, "freq", 12000.0});
  const golden::Channels st12 = play(open, 6.0);
  const double d12 = maxDiff(out, st12, at(4.5), at(5.5));
  MESSAGE("held at 12 kHz vs set to 12 kHz: max diff " << d12);
  CHECK(d12 <= 1e-4);
}

TEST_CASE("device automation: a macro lane drives what the macro maps; a chain volume lane its chain (rack law in the graph)") {
  Song s(false);
  const DeviceId u = s.add("utility");
  auto g = ops::groupDevices(*s.store.current(), s.store.ids(), std::vector{u});
  REQUIRE(g.ok());
  s.must(g->command);
  const DeviceId rack = g->devices.at(0);
  const ChainId chain = findDevice(*s.store.current(), rack).device->rack->chains.at(0)->id;
  auto m = ops::mapMacro(*s.store.current(), s.store.ids(), rack, 0, u, "gain");
  REQUIRE(m.ok());
  s.must(m->command);
  // Macro 1: 127 (full gain travel: +35 dB… clipped by min/max 0 … 1 → the Utility's −70 … +35 dB travel) for a bar,
  // then 64, then 0 (−70 dB: silence).
  s.lane(ParamRef::device(rack, "macro1"), "macro1",
         {{0, 64.0f, 0.0f, {}}, {kBar, 64.0f, 0.0f, {}}, {kBar, 0.0f, 0.0f, {}}});
  // The chain's volume: 0 dB, then −12 dB from bar 0.5.
  s.lane(ParamRef::chain(chain, kChainVolume), "", {{0, 0.0f, 0.0f, {}}, {kBar / 2, 0.0f, 0.0f, {}}, {kBar / 2, -12.0f, 0.0f, {}}});
  const golden::Channels out = play(s, 3.0);
  const auto at = [](double sec) { return static_cast<size_t>(sec * kRate); };
  const double a = rms(out, at(0.5), at(0.95)), b = rms(out, at(1.1), at(1.9)), c = rms(out, at(2.2), at(2.9));
  MESSAGE("RMS: macro 64 at 0 dB " << a << ", macro 64 at -12 dB " << b << ", macro 0 " << c);
  CHECK(a > 1e-3);
  CHECK(b / a == doctest::Approx(std::pow(10.0, -12.0 / 20.0)).epsilon(0.02));  // the chain volume lane: −12 dB
  CHECK(c < 1e-3 * a);                                                          // macro 0: the mapped gain at −70 dB
  // Static: the same macro value and chain volume set by hand render the same as the lanes holding them.
  Song ref(false);
  const DeviceId u2 = ref.add("utility");
  auto g2 = ops::groupDevices(*ref.store.current(), ref.store.ids(), std::vector{u2});
  REQUIRE(g2.ok());
  ref.must(g2->command);
  const DeviceId rack2 = g2->devices.at(0);
  auto m2 = ops::mapMacro(*ref.store.current(), ref.store.ids(), rack2, 0, u2, "gain", 64.0);
  REQUIRE(m2.ok());
  ref.must(m2->command);
  const golden::Channels st = play(ref, 1.0);
  CHECK(maxDiff(out, st, 0, at(0.95)) <= 1e-6);
}

TEST_CASE("device automation: an automated gain jump is click-free; the wire amount lane drives its wire") {
  SUBCASE("Utility gain: 0 dB → −12 dB → +6 dB as jumps") {
    Song s(false);
    const DeviceId u = s.add("utility");
    const float k12 = static_cast<float>(std::pow(10.0, -12.0 / 20.0)), k6 = static_cast<float>(std::pow(10.0, 6.0 / 20.0));
    s.lane(ParamRef::device(u, "gain"), "gain",
           {{0, 1.0f, 0.0f, {}}, {kBar / 2, 1.0f, 0.0f, {}}, {kBar / 2, k12, 0.0f, {}}, {kBar, k12, 0.0f, {}}, {kBar, k6, 0.0f, {}}});
    const golden::Channels out = play(s, 3.0);
    const auto at = [](double sec) { return static_cast<size_t>(sec * kRate); };
    auto maxD1 = [&](size_t from, size_t to) {
      double m = 0.0;
      for (size_t c = 0; c < 2; ++c)
        for (size_t i = std::max<size_t>(from, 1); i < to; ++i)
          m = std::max(m, std::fabs(static_cast<double>(out[c][i]) - out[c][i - 1]));
      return m;
    };
    auto peak = [&](size_t from, size_t to) {
      double m = 0.0;
      for (size_t c = 0; c < 2; ++c)
        for (size_t i = from; i < to; ++i) m = std::max(m, std::fabs(static_cast<double>(out[c][i])));
      return m;
    };
    // The note at 0 dB (0.3–0.95 s) is the reference; the jumps at 1 s and 2 s land inside 0.95–2.5 s, where the
    // signal reaches +6 dB (×2): a click would step far beyond the note's own largest step at that level.
    const double steady = maxD1(at(0.3), at(0.95));
    const double jumps = maxD1(at(0.95), at(2.5));
    const double scale = peak(at(0.95), at(2.5)) / peak(at(0.3), at(0.95));
    MESSAGE("largest sample step: steady " << steady << ", over the automated jumps " << jumps << " (level x" << scale << ")");
    CHECK(steady > 0.0);
    CHECK(jumps <= 1.5 * steady * scale);  // the rack crossfade test's bound
    CHECK(rms(out, at(1.2), at(1.9)) / rms(out, at(0.3), at(0.95)) == doctest::Approx(k12).epsilon(0.02));
  }
  SUBCASE("a wire amount lane at 0 plays as the wire at amount 0; at 1 the LFO sweeps the filter") {
    auto build = [](Song& s, bool laneOn) {
      const DeviceId lfo = s.add("lfo_device");
      const DeviceId af = s.add("auto_filter");
      s.must(SetDeviceParam{lfo, "rate", 4.0});
      s.must(SetDeviceParam{af, "freq", 400.0});
      auto w = ops::connectWire(*s.store.current(), s.store.ids(), lfo, "lfo", af, "freq", 0.0);
      REQUIRE(w.ok());
      s.must(w->command);
      if (laneOn)
        s.lane(ParamRef::wireAmount(w->wires.at(0)), "", {{0, 0.0f, 0.0f, {}}, {kBar / 2, 0.0f, 0.0f, {}}, {kBar / 2, 1.0f, 0.0f, {}}});
    };
    Song a(true), b(true);
    build(a, true);
    build(b, false);
    const golden::Channels la = play(a, 2.0), lb = play(b, 2.0);
    const auto at = [](double sec) { return static_cast<size_t>(sec * kRate); };
    CHECK(maxDiff(la, lb, 0, at(0.95)) <= 1e-6);
    CHECK(maxDiff(la, lb, at(1.1), at(2.0)) > 1e-2);  // modulated
  }
}

TEST_CASE("device automation: automation owns its slot; a stopped project with device lanes stays idle") {
  Song s(false, 2);
  const DeviceId u = s.add("utility");
  s.lane(ParamRef::device(u, "gain"), "gain", {{0, 1.0f, 0.0f, {}}, {2 * kBar, 0.25f, 0.0f, {}}});
  auto link = makeLink(s.store.current(), 250.0f);
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  const uint64_t* writes = link->engine().timeline().blockHook().paramWrites;
  REQUIRE(writes != nullptr);
  // Stopped: the lane holds its value at the playhead; after the first block nothing is written, and the engine
  // falls asleep as an idle project does.
  d.run(4);
  const uint64_t w0 = *writes;
  d.run(d.blocksFor(0.5));
  CHECK(*writes == w0);
  CHECK(link->engine().stats().asleepBlocks > 0);
  // Playing: the ramp moves the param every block (one write per block through the generation counter).
  link->play(0);
  const uint64_t w1 = *writes;
  d.run(100);
  CHECK(*writes >= w1 + 95);
  // A hand edit of the automated param (the views override the lane first; here the document change alone) never
  // reaches the slot: the automation keeps writing it.
  const app::LinkStats s0 = link->stats();
  s.must(SetDeviceParam{u, "gain", 2.0});
  link->flush();
  CHECK(link->stats().paramWrites == s0.paramWrites);
  CHECK(link->stats().compiles == s0.compiles);
}
