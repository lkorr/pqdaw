// T4.18: device automation through the playback link — a gain ramp reaches the DSP (and matches the static render where
// it holds a value), a wire-amount lane drives its wire, an automated gain jump is click-free (block rate + the `gain`
// stage's smoothing), a hand-written value never reaches an automated slot, and a stopped project with device lanes
// costs what an idle one does (no automation writes, the engine sleeps). GU.11: the lanes automate a block's param — a
// module's control, its value block (Utility's Gain, in dB) — after the default instrument group; the filter sweep went
// with Auto Filter (retired).
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

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s, float sleepAfterMs = 0.0f, bool jit = false) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = sleepAfterMs;
  c.jit = jit;
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

// One MIDI track with the default instrument group holding one note for `bars` bars.
struct Song {
  DocumentStore store;
  TrackId t;
  DeviceId inst;
  explicit Song(int bars = 4) {
    IdSource& ids = store.ids();
    t = ids.track();
    inst = ids.device();
    Batch b{"song", {}};
    b.commands.push_back(SetTempoMap{{{0, 120.0, TempoCurve::Step}}});
    b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "T"), {}, kAppend});
    (void)playback_fixture::instrument(b, ids, t, inst);
    std::vector<Note> notes{playback_fixture::note(ids, 0, bars * kBar, 0)};
    b.commands.push_back(AddClip{t, playback_fixture::clip(ids, 0, bars * kBar, notes, "held")});
    must(b);
    playback_fixture::wireSeries(store);  // TX.all-free: wired in series
  }
  void must(Command c) {
    const Status st = store.dispatch(playback_fixture::inUnits(store, std::move(c)));
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  // A module ("module:<key>") on the path after the instrument group (or off it); its group.
  DeviceId add(const char* key, uint32_t slot = kAppend) {
    auto e = ops::addDevice(*store.current(), store.ids(), t, std::string("module:") + key, DeviceKind::AudioEffect, slot);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    must(e->command);
    return e->devices.at(0);
  }
  // A module's control (its value block).
  DeviceId control(DeviceId module, std::string_view name) const {
    const DeviceId v = test_blocks::member(*store.current(), module, name);
    REQUIRE(v.valid());
    return v;
  }
  // A lane on `r` with `points` ({ticks, value, curve}).
  LaneId lane(ParamRef r, std::string name, std::vector<Breakpoint> points) {
    auto l = std::make_shared<AutomationLane>();
    l->id = store.ids().lane();
    l->param = r;
    l->paramName = std::move(name);
    for (Breakpoint& p : points) p.id = store.ids().breakpoint();
    knob_positions::lanePositions(*store.current(), r, l->paramName, points, playback_fixture::unitsLibrary());  // GU.1
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
double maxDiff(const golden::Channels& a, const golden::Channels& b, size_t from, size_t to) {
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c)
    for (size_t i = from; i < to && i < a[c].size() && i < b[c].size(); ++i)
      m = std::max(m, std::fabs(static_cast<double>(a[c][i]) - b[c][i]));
  return m;
}
// Renders `seconds` from the start after 0.25 s stopped (the devices' smoothers settle on their settings first).
// `jit`: every unit on its JIT image before that (the images become current in the first stopped block).
golden::Channels play(Song& s, double seconds, bool jit = false) {
  auto link = makeLink(s.store.current(), 0.0f, jit);
  Driver d{*link};
  if (jit) {
    REQUIRE(link->waitForJit(120.0));
    const app::JitStats js = link->jitStats();
    CHECK(js.errors == 0);
    CHECK(js.published == link->engine().program()->numUnits());
  }
  d.run(d.blocksFor(0.25));
  if (jit) {
    engine::ProjectProgram* p = link->engine().program();
    uint32_t onJit = 0;
    for (uint32_t u = 0; u < p->numUnits(); ++u)
      onJit += p->current(u) != nullptr && p->current(u)->io().backend == engine::ImageBackend::Jit ? 1u : 0u;
    CHECK(onJit == p->numUnits());
  }
  d.out = golden::Channels(2);
  link->play(0);
  d.run(d.blocksFor(seconds));
  CHECK(link->lastError().empty());
  return d.out;
}

}  // namespace

TEST_CASE("device automation: a gain ramp reaches the Utility's Gain; where the lane holds, it equals the static setting") {
  Song s;
  const DeviceId gain = s.control(s.add("utility"), "Gain");
  Song ref;  // the same song with the Gain fixed at the lane's first value
  ref.must(SetDeviceParam{ref.control(ref.add("utility"), "Gain"), std::string(kValueParam), 0.0});
  // 0 dB for the first bar, then down to −24 dB over the second (linear in dB: the Gain's curve), then held.
  s.lane(ParamRef::device(gain, std::string(kValueParam)), std::string(kValueParam),
         {{0, 0.0f, 0.0f, {}}, {kBar, 0.0f, 0.0f, {}}, {2 * kBar, -24.0f, 0.0f, {}}});
  const golden::Channels out = play(s, 6.0);
  const golden::Channels st = play(ref, 6.0);
  const auto at = [](double sec) { return static_cast<size_t>(sec * kRate); };
  // Bar 1 (0–2 s): the lane holds 0 dB: the static render, sample for sample.
  CHECK(maxDiff(out, st, 0, at(1.9)) <= 1e-6);
  // The ramp lowers the level through the second bar (−6 dB at 2.5 s, −18 dB at 3.5 s); the static one stays.
  const double l0 = rms(out, at(1.0), at(1.9)), lA = rms(out, at(2.45), at(2.55)), l1 = rms(out, at(3.45), at(3.55));
  MESSAGE("RMS: 0 dB " << l0 << ", ~-6 dB " << lA << ", ~-18 dB " << l1);
  CHECK(lA < 0.7 * l0);
  CHECK(l1 < 0.3 * lA);
  CHECK(rms(st, at(3.45), at(3.55)) > 0.7 * l0);
  // Held at −24 dB (4–6 s), it plays as the Gain set to −24 dB by hand.
  Song low;
  low.must(SetDeviceParam{low.control(low.add("utility"), "Gain"), std::string(kValueParam), -24.0});
  const golden::Channels st24 = play(low, 6.0);
  const double d24 = maxDiff(out, st24, at(4.5), at(5.5));
  MESSAGE("held at -24 dB vs set to -24 dB: max diff " << d24);
  CHECK(d24 <= 1e-4);
  // The JIT images follow the same lane.
  const golden::Channels jit = play(s, 6.0, true);
  const double dj = maxDiff(out, jit, 0, at(6.0));
  MESSAGE("interpreter vs JIT: max diff " << dj);
  CHECK(dj <= 1e-4);
}

TEST_CASE("device automation: an automated gain jump is click-free; the wire amount lane drives its wire") {
  SUBCASE("Utility gain: 0 dB → −12 dB → +6 dB as jumps") {
    Song s;
    const DeviceId u = s.control(s.add("utility"), "Gain");
    const float k12 = static_cast<float>(std::pow(10.0, -12.0 / 20.0));
    s.lane(ParamRef::device(u, std::string(kValueParam)), std::string(kValueParam),
           {{0, 0.0f, 0.0f, {}}, {kBar / 2, 0.0f, 0.0f, {}}, {kBar / 2, -12.0f, 0.0f, {}}, {kBar, -12.0f, 0.0f, {}}, {kBar, 6.0f, 0.0f, {}}});
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
  SUBCASE("a wire amount lane at 0 plays as the wire at amount 0; at 1 the LFO sweeps the gain") {
    auto build = [](Song& s, bool laneOn) {
      const DeviceId lfo = s.add("lfo", ops::kOffPath);
      const DeviceId gain = s.control(s.add("utility"), "Gain");
      s.must(SetDeviceParam{s.control(lfo, "Rate"), std::string(kValueParam), 4.0});
      s.must(SetDeviceParam{gain, std::string(kValueParam), -12.0});
      auto w = ops::connectTravelWire(*s.store.current(), s.store.ids(), lfo, "y", gain, std::string(kValueParam), 0.0);
      REQUIRE(w.ok());
      s.must(w->command);
      if (laneOn)
        s.lane(ParamRef::wireAmount(w->wires.at(0)), "", {{0, 0.0f, 0.0f, {}}, {kBar / 2, 0.0f, 0.0f, {}}, {kBar / 2, 1.0f, 0.0f, {}}});
    };
    Song a, b;
    build(a, true);
    build(b, false);
    const golden::Channels la = play(a, 2.0), lb = play(b, 2.0);
    const auto at = [](double sec) { return static_cast<size_t>(sec * kRate); };
    CHECK(maxDiff(la, lb, 0, at(0.95)) <= 1e-6);
    CHECK(maxDiff(la, lb, at(1.1), at(2.0)) > 1e-2);  // modulated
  }
}

TEST_CASE("device automation: automation owns its slot; a stopped project with device lanes stays idle") {
  Song s(2);
  const DeviceId u = s.control(s.add("utility"), "Gain");
  s.lane(ParamRef::device(u, std::string(kValueParam)), std::string(kValueParam), {{0, 0.0f, 0.0f, {}}, {2 * kBar, -12.0f, 0.0f, {}}});
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
  s.must(SetDeviceParam{u, std::string(kValueParam), 6.0});
  link->flush();
  CHECK(link->stats().paramWrites == s0.paramWrites);
  CHECK(link->stats().compiles == s0.compiles);
}
