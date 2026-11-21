// AP.8a (docs/reviews/AP.md): the arpeggiator core's review fixes, engine side.
//   - H2 / H4: degree tables per tuning node (ArpTables), a new table set re-resolves the held — latched — notes and
//     every sounding step, in each note's own tuning; RtScope;
//   - M1: ArpTableMailbox: a publisher racing the audio thread never leaves the last table untaken;
//   - M2: NoteStageChain::takeOver adopts a stage by key anywhere: a stage inserted / removed before a running arp;
//   - M3: a tempo remap (StageClock::remapped) ends nothing; the next step on the new grid;
//   - M4: 512 sounding notes, Chord Trigger over 128 notes with 3 layers and ties: bounded time per block; RtScope;
//   - L3 / L4: a harmonic beyond 2^24 is none; generated ids skip live ones across the 2^24 wrap.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/engine/arp.h"
#include "pq/engine/note_stage.h"

namespace e = pq::engine;
using pq::SampleTime;

namespace {

constexpr double kSr = 48000.0;
constexpr int64_t k16 = 6000;  // 1/16 at 120 BPM

struct Ev {
  int64_t t;
  e::NoteEvent ev;
};

e::NoteEvent on(uint32_t id, float hz) { return e::noteOn(0, id, hz, 1.0f); }
e::NoteEvent degreeOn(uint32_t id, float hz, int degree, int equave, float tuning) {
  e::NoteEvent n = on(id, hz);
  n.degree = static_cast<float>(degree);
  n.equave = static_cast<float>(equave);
  n.tuning = tuning;
  return n;
}

// Plays a chain block by block (Song from sample 0, 120 BPM unless `tempo` says otherwise). `before(T)` runs before the
// block at T; `remapAt`: the block whose clock is remapped (the song position jumps to `remapPos`).
struct Rig {
  pq::TempoMap tempo;
  double bpm = 120.0;
  uint64_t rtViolations = 0;
  std::function<void(int64_t)> before;
  int64_t remapAt = -1;
  SampleTime remapPos = 0;
  const pq::TempoMap* remapTempo = nullptr;
  double remapBpm = 0.0;

  std::vector<Ev> run(e::NoteStageChain& chain, std::vector<Ev> in, int64_t total, uint32_t block) {
    std::stable_sort(in.begin(), in.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    std::vector<Ev> res;
    e::EventBuffer out(8192);
    size_t ii = 0;
    SampleTime pos = 0;
    const pq::TempoMap* tm = &tempo;
    for (int64_t T = 0; T < total; T += block) {
      if (before) before(T);
      const auto frames = static_cast<uint32_t>(std::min<int64_t>(block, total - T));
      chain.input().clear();
      while (ii < in.size() && in[ii].t < T + frames) {
        e::NoteEvent ev = in[ii].ev;
        ev.frame = static_cast<uint32_t>(std::max<int64_t>(in[ii].t - T, 0));
        chain.input().push(ev);
        ++ii;
      }
      bool remapped = false;
      if (T == remapAt) {
        pos = remapPos;
        tm = remapTempo;
        bpm = remapBpm;
        remapped = true;
      }
      e::ClockSegment seg{0, frames, true, pos};
      e::NoteStageBlock b;
      b.time = T;
      b.frames = frames;
      b.clock.tempo = tm;
      b.clock.sampleRate = kSr;
      b.clock.bpm = bpm;
      b.clock.segments = &seg;
      b.clock.numSegments = 1;
      b.clock.remapped = remapped;
      out.clear();
      rtViolations += pq::alloc_trap::countRtViolations([&] { (void)chain.run(b, out); });
      for (uint32_t i = 0; i < out.size(); ++i) res.push_back({T + out.list().events[i].frame, out.list().events[i]});
      pos += frames;
    }
    return res;
  }
};

std::vector<Ev> ofKind(const std::vector<Ev>& v, e::NoteEventKind k) {
  std::vector<Ev> r;
  for (const Ev& x : v)
    if (x.ev.kind == k) r.push_back(x);
  return r;
}

e::ArpParams upEighths() {
  e::ArpParams p;
  p.rate = {true, 1, 8, 1, 1, 125.0f};
  p.gate = 1.0f;
  return p;
}

// An EDO table of n steps (equave 1200) at reference `ref`.
e::ArpDegreeTable edo(int n, double ref) {
  e::ArpDegreeTable t;
  for (int k = 0; k < n; ++k) t.cents.push_back(1200.0 * k / n);
  t.referenceHz = ref;
  return t;
}
double edoHz(int n, double ref, int d) { return ref * std::exp2(static_cast<double>(d) / n); }

std::unique_ptr<e::NoteStageChain> chainOf(std::unique_ptr<e::ArpStage> a, uint64_t key) {
  auto c = std::make_unique<e::NoteStageChain>(4096);
  a->key = key;
  REQUIRE(c->add(std::move(a)));
  return c;
}

}  // namespace

TEST_CASE("arp core (AP.8a H4): each note steps in its own tuning node's table; a static note in the track's") {
  // node 1: 12-EDO, node 2: 31-EDO; the track table (tuning 0): 5-EDO. Distance 1, Steps 2: copy 1 is one degree up
  // in the note's own table.
  e::ArpTables tabs(edo(5, 200.0));
  tabs.nodes = {edo(12, 200.0), edo(31, 200.0)};
  e::ArpParams p = upEighths();
  p.steps = 2;
  auto chain = chainOf(std::make_unique<e::ArpStage>(p, std::vector<e::ArpStage::Mapping>{}, tabs), 1);
  Rig r;
  const std::vector<Ev> in = {{0, degreeOn(1, static_cast<float>(edoHz(12, 200.0, 0)), 0, 0, 1.0f)},
                              {0, degreeOn(2, static_cast<float>(edoHz(31, 200.0, 3)), 3, 0, 2.0f)},
                              {0, on(3, 1000.0f)}};
  const auto ons = ofKind(r.run(*chain, in, 6 * 12000, 64), e::NoteEventKind::NoteOn);
  REQUIRE(ons.size() >= 6);
  // Up by Hz: 200 (12-EDO deg 0), 213.8 (31-EDO deg 3), 1000 (static), then the copies one degree up.
  CHECK(ons[3].ev.hz == doctest::Approx(edoHz(12, 200.0, 1)).epsilon(1e-6));
  CHECK(ons[3].ev.degree == 1.0f);
  CHECK(ons[3].ev.tuning == 1.0f);
  CHECK(ons[4].ev.hz == doctest::Approx(edoHz(31, 200.0, 4)).epsilon(1e-6));
  CHECK(ons[4].ev.degree == 4.0f);
  CHECK(ons[4].ev.tuning == 2.0f);
  CHECK(ons[5].ev.hz == doctest::Approx(1000.0 * std::exp2(1.0 / 5.0)).epsilon(1e-6));  // the track's 5-EDO
}

TEST_CASE("arp core (AP.8a H2): a new table set re-resolves a latched set and its sounding steps, at their places") {
  // Hold on, a 12-EDO chord {0, 4, 7} released at once: latched. Ratio 3/2 Steps 2. After 2.5 steps the tables
  // become 19-EDO (ref moved too): the latched notes are degrees 0, 4, 7 of 19-EDO at the new ref, the steps × 3/2.
  auto box = std::make_shared<e::ArpTableMailbox>();
  e::ArpTables t12;
  t12.nodes = {edo(12, 200.0)};
  e::ArpParams p = upEighths();
  p.hold = true;
  p.unit = e::ArpUnit::Ratio;
  p.ratioP = 3;
  p.ratioQ = 2;
  p.steps = 2;
  auto chain = chainOf(std::make_unique<e::ArpStage>(p, std::vector<e::ArpStage::Mapping>{}, t12, e::ArpLaneSteps{}, box), 1);
  std::vector<Ev> in;
  const int deg[3] = {0, 4, 7};
  for (uint32_t i = 0; i < 3; ++i) {
    in.push_back({0, degreeOn(i + 1, static_cast<float>(edoHz(12, 200.0, deg[i])), deg[i], 0, 1.0f)});
    in.push_back({100, e::noteOff(0, i + 1)});
  }
  Rig r;
  const int64_t at = 2 * 12000 + 6016;
  r.before = [&](int64_t T) {
    if (T != at) return;
    e::ArpTables t19;
    t19.nodes = {edo(19, 210.0)};
    box->post(t19);
  };
  const auto out = r.run(*chain, in, 7 * 12000, 64);
  CHECK(r.rtViolations == 0);
  const auto ons = ofKind(out, e::NoteEventKind::NoteOn);
  REQUIRE(ons.size() >= 7);
  for (size_t k = 0; k < 7; ++k) {
    CAPTURE(k);
    CHECK(ons[k].t == static_cast<int64_t>(k) * 12000);  // on the grid: nothing restarted
    const double ratio = (k / 3) % 2 == 1 ? 1.5 : 1.0;
    const double want = k < 3 ? edoHz(12, 200.0, deg[k % 3]) * ratio : edoHz(19, 210.0, deg[k % 3]) * ratio;
    CHECK(ons[k].ev.hz == doctest::Approx(want).epsilon(1e-6));
    CHECK(ons[k].ev.degree == static_cast<float>(deg[k % 3]));  // (H1) the degree identity, at its ratio
  }
  // the sounding step 2 (degree 7 at 12-EDO) moved at the table's block: a retune to degree 7 of 19-EDO
  bool retuned = false;
  for (const Ev& x : ofKind(out, e::NoteEventKind::Pitch))
    if (x.t == at && (x.ev.flags & e::kNoteEventRetune) != 0)
      retuned = retuned || std::fabs(x.ev.hz - edoHz(19, 210.0, 7)) < 1e-3;
  CHECK(retuned);
  // nothing ended off the grid
  for (const Ev& x : ofKind(out, e::NoteEventKind::NoteOff)) CHECK(x.t % 12000 == 0);
}

TEST_CASE("arp core (AP.8a M1): a publisher racing the audio thread never leaves the newest table untaken") {
  for (int round = 0; round < 50; ++round) {
    auto box = std::make_shared<e::ArpTableMailbox>();
    std::atomic<bool> done{false};
    std::atomic<double> seen{0.0};
    std::thread audio([&] {
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (std::chrono::steady_clock::now() < until) {
        if (const e::ArpTables* t = box->take()) seen.store(t->track.referenceHz);
        if (done.load() && seen.load() == 1000.0) return;
      }
    });
    for (int i = 1; i <= 200; ++i) {
      e::ArpTables t(edo(12, i == 200 ? 1000.0 : static_cast<double>(i)));
      box->post(std::move(t));
      if ((i & 7) == 0) box->collect();  // what ProjectLink::collect does while the UI polls
    }
    done.store(true);
    for (int i = 0; i < 1000 && seen.load() != 1000.0; ++i) {  // the link keeps collecting
      box->collect();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    audio.join();
    CHECK(seen.load() == 1000.0);
  }
}

TEST_CASE("arp core (AP.8a M2): a stage inserted before / removed before / after a running arp: it keeps playing") {
  e::ArpParams p = upEighths();
  p.steps = 2;
  // The arp (key 7) runs alone; then an arp (key 3, chord trigger) is put before it; then removed again.
  auto a1 = std::make_unique<e::ArpStage>(p);
  e::ArpStage* first = a1.get();
  auto current = chainOf(std::move(a1), 7);
  std::vector<std::unique_ptr<e::NoteStageChain>> retired;
  e::EventBuffer handOut(4096);
  std::vector<Ev> handed;
  uint64_t handAllocs = 0;
  auto swapTo = [&](std::unique_ptr<e::NoteStageChain> next, int64_t T) {
    handOut.clear();
    next->input().clear();
    handAllocs += pq::alloc_trap::countRtViolations([&] { next->takeOver(*current, 0, handOut, next->input()); });
    for (uint32_t i = 0; i < handOut.size(); ++i) handed.push_back({T, handOut.list().events[i]});
    retired.push_back(std::move(current));
    current = std::move(next);
  };
  Rig r;
  const std::vector<Ev> in = {{0, on(1, 200.0f)}, {0, on(2, 250.0f)}, {0, on(3, 300.0f)}};
  std::vector<Ev> out;  // three legs, the chain swapped between them
  auto leg = [&](const std::vector<Ev>& evs, int64_t from, int64_t to) {  // blocks [from, to) of the current chain
    e::EventBuffer o(8192);
    size_t ii = 0;
    for (int64_t T = from; T < to; T += 64) {
      // (a chain swapped in keeps what its takeOver put into its input for this block)
      if (T != from || from == 0) current->input().clear();
      while (ii < evs.size() && evs[ii].t < T + 64) {
        e::NoteEvent ev = evs[ii].ev;
        ev.frame = static_cast<uint32_t>(std::max<int64_t>(evs[ii].t - T, 0));
        current->input().push(ev);
        ++ii;
      }
      e::ClockSegment seg{0, 64, true, T};
      e::NoteStageBlock b;
      b.time = T;
      b.frames = 64;
      b.clock.tempo = &r.tempo;
      b.clock.sampleRate = kSr;
      b.clock.segments = &seg;
      b.clock.numSegments = 1;
      o.clear();
      r.rtViolations += pq::alloc_trap::countRtViolations([&] { (void)current->run(b, o); });
      for (uint32_t i = 0; i < o.size(); ++i) out.push_back({T + o.list().events[i].frame, o.list().events[i]});
    }
  };
  leg(in, 0, 2 * 12000 + 6016);
  {
    e::ArpParams q = upEighths();
    q.style = e::ArpStyle::ChordTrigger;
    auto next = std::make_unique<e::NoteStageChain>(4096);
    auto x = std::make_unique<e::ArpStage>(q);
    x->key = 3;
    REQUIRE(next->add(std::move(x)));
    auto y = std::make_unique<e::ArpStage>(p);
    y->key = 7;
    REQUIRE(next->add(std::move(y)));
    swapTo(std::move(next), 2 * 12000 + 6016);
    CHECK(handAllocs == 0);
  }
  CHECK(static_cast<e::ArpStage*>(current->stage(1))->core().heldCount() == 3);  // adopted: the held chord
  CHECK(first->core().heldCount() == 0);
  leg({}, 2 * 12000 + 6016, 4 * 12000 + 6016);
  {
    auto next = std::make_unique<e::NoteStageChain>(4096);
    auto y = std::make_unique<e::ArpStage>(p);
    y->key = 7;
    REQUIRE(next->add(std::move(y)));
    swapTo(std::move(next), 4 * 12000 + 6016);
  }
  CHECK(static_cast<e::ArpStage*>(current->stage(0))->core().heldCount() == 3);
  leg({}, 4 * 12000 + 6016, 7 * 12000);
  CHECK(r.rtViolations == 0);
  CHECK(handed.empty());  // nothing ended: the new upstream arp held nothing, the adopted one kept its notes
  const auto ons = ofKind(out, e::NoteEventKind::NoteOn);
  REQUIRE(ons.size() == 8);  // steps 0 … 7 (the last block reaches 84000)
  // Up over 200 / 250 / 300, copy 1 a 12-EDO step up: the pattern place goes on through both swaps
  const float base[3] = {200.0f, 250.0f, 300.0f};
  for (size_t k = 0; k < 8; ++k) {
    CAPTURE(k);
    CHECK(ons[k].t == static_cast<int64_t>(k) * 12000);
    const double want = base[k % 3] * ((k / 3) % 2 == 1 ? std::exp2(1.0 / 12.0) : 1.0);
    CHECK(ons[k].ev.hz == doctest::Approx(want).epsilon(1e-6));
  }
  for (const Ev& x : ofKind(out, e::NoteEventKind::NoteOff)) CHECK(x.t % 12000 == 0);
}

TEST_CASE("arp core (AP.8a M2): an upstream arp removed: its notes leave the adopted arp (Hold on too), no restart") {
  // [X (chord trigger), A (Up, Hold)]: A holds X's generated notes. X removed: X's key-held source notes go into the
  // new chain's input first, then X's generated notes end as kNoteEventGone: A swaps them without emptying (no
  // restart) and does not latch them.
  e::ArpParams q = upEighths();
  q.style = e::ArpStyle::ChordTrigger;
  q.gate = 2.0f;  // overlapping: X's notes always sound
  e::ArpParams p = upEighths();
  p.hold = true;
  p.rate = {true, 1, 16, 1, 1, 125.0f};
  auto old = std::make_unique<e::NoteStageChain>(4096);
  auto x = std::make_unique<e::ArpStage>(q);
  x->key = 3;
  REQUIRE(old->add(std::move(x)));
  auto a = std::make_unique<e::ArpStage>(p);
  a->key = 7;
  REQUIRE(old->add(std::move(a)));
  Rig r;
  const std::vector<Ev> in = {{0, on(1, 200.0f)}, {0, on(2, 300.0f)}};
  (void)r.run(*old, in, 3 * 12000 + 3008, 64);
  e::ArpCore& oldA = static_cast<e::ArpStage*>(old->stage(1))->core();
  const uint32_t before = oldA.heldCount();  // X's sounding notes (keys) + those it released earlier (latched)
  REQUIRE(before >= 2);
  auto next = std::make_unique<e::NoteStageChain>(4096);
  auto a2 = std::make_unique<e::ArpStage>(p);
  a2->key = 7;
  REQUIRE(next->add(std::move(a2)));
  e::EventBuffer voices(4096);
  next->input().clear();
  next->takeOver(*old, 0, voices, next->input());
  // into the new input: X's two key-held notes, then X's generated notes' offs (gone)
  const e::EventList& li = next->input().list();
  REQUIRE(li.count >= 3);
  CHECK(li.events[0].kind == e::NoteEventKind::NoteOn);
  CHECK(li.events[1].kind == e::NoteEventKind::NoteOn);
  for (uint32_t i = 2; i < li.count; ++i) {
    CHECK(li.events[i].kind == e::NoteEventKind::NoteOff);
    CHECK((li.events[i].flags & e::kNoteEventGone) != 0);
  }
  const uint32_t gone = li.count - 2;
  e::ArpCore& newA = static_cast<e::ArpStage*>(next->stage(0))->core();
  const uint64_t steps0 = newA.stats().steps;
  e::EventBuffer o(4096);
  e::ClockSegment seg{0, 64, true, 3 * 12000 + 3008};
  e::NoteStageBlock b;
  b.time = 3 * 12000 + 3008;
  b.frames = 64;
  b.clock.tempo = &r.tempo;
  b.clock.sampleRate = kSr;
  b.clock.segments = &seg;
  b.clock.numSegments = 1;
  (void)next->run(b, o);
  CHECK(newA.heldCount() == before - gone + 2);  // + the two source notes; X's sounding ones gone (not latched)
  CHECK(newA.stats().steps == steps0);  // no immediate (restart) step in this block
  for (uint32_t i = 0; i < o.size(); ++i) CHECK(o.list().events[i].kind != e::NoteEventKind::NoteOn);
}

TEST_CASE("arp core (AP.8a M3): a tempo remap is not a locate: nothing ends, the next step sits on the new grid") {
  e::ArpParams p = upEighths();
  p.hold = true;
  auto chain = chainOf(std::make_unique<e::ArpStage>(p), 1);
  Rig r;
  // At block 40064 (tick = 3.33867 eighths at 120 BPM), the map becomes 100 BPM: that tick is song sample 48077.
  const std::vector<pq::TempoPoint> pts{{0, 100.0, pq::TempoCurve::Step}};
  auto slow = pq::TempoMap::create(pts);
  REQUIRE(slow.ok());
  r.remapAt = 40064;
  const double ticks = r.tempo.samplesToTicks(40064, kSr);
  r.remapPos = slow.value().ticksToSamples(static_cast<pq::Ticks>(std::ceil(ticks)), kSr);
  r.remapTempo = &slow.value();
  r.remapBpm = 100.0;
  const auto out = r.run(*chain, {{0, on(1, 200.0f)}, {0, on(2, 300.0f)}}, 8 * 12000, 64);
  CHECK(r.rtViolations == 0);
  auto* st = static_cast<e::ArpStage*>(chain->stage(0));
  CHECK(st->core().stats().transportJumps == 0);
  CHECK(st->core().stats().tempoRemaps == 1);
  const auto ons = ofKind(out, e::NoteEventKind::NoteOn);
  REQUIRE(ons.size() >= 6);
  // steps 0 … 3 at 12000 each; step 4 at song sample 4 · 14400 = 57600 = engine time 40064 + (57600 − remapPos)
  for (size_t k = 0; k < 4; ++k) CHECK(ons[k].t == static_cast<int64_t>(k) * 12000);
  const int64_t step4 = 40064 + (57600 - r.remapPos);
  CHECK(ons[4].t == step4);
  CHECK(ons[5].t == step4 + 14400);
  CHECK(ons[4].ev.hz == 200.0f);  // the pattern place goes on (Up over 200, 300: step 4 is 200)
  // step 3's note (gate 100 %) ends at step 4's onset, re-timed by the tempo ratio — not cut at the remap
  for (const Ev& x : ofKind(out, e::NoteEventKind::NoteOff)) {
    CAPTURE(x.t);
    CHECK(x.t != 40064);
    if (x.t > 40064 && x.t < step4 + 100) CHECK(std::abs(x.t - step4) <= 2);
  }
}

TEST_CASE("arp core (AP.8a M4): a full sounding table (Chord Trigger 128 x 4 layers, gate 200 %, ties) costs bounded work") {
  e::ArpParams p;
  p.style = e::ArpStyle::ChordTrigger;
  p.rate = {true, 1, 32, 1, 1, 125.0f};
  p.gate = 2.0f;
  p.layers = 3;
  p.laneOn[static_cast<size_t>(e::ArpLane::SkipTie)] = true;
  p.laneLength[static_cast<size_t>(e::ArpLane::SkipTie)] = 2;
  e::ArpLaneSteps lanes;
  lanes.value[static_cast<size_t>(e::ArpLane::SkipTie)][1] = static_cast<float>(e::ArpStepKind::Tie);
  auto chain = chainOf(std::make_unique<e::ArpStage>(p, std::vector<e::ArpStage::Mapping>{}, e::ArpTables{}, lanes), 1);
  std::vector<Ev> in;
  for (uint32_t i = 0; i < 128; ++i) in.push_back({0, on(i + 1, 100.0f + 7.0f * static_cast<float>(i))});
  Rig r;
  double worstUs = 0.0, stepUs = 0.0;
  int stepBlocks = 0;
  const auto out = [&] {
    std::vector<Ev> res;
    e::EventBuffer o(8192);
    size_t ii = 0;
    for (int64_t T = 0; T < 48000; T += 64) {
      chain->input().clear();
      while (ii < in.size() && in[ii].t < T + 64) chain->input().push(in[ii++].ev);
      e::ClockSegment seg{0, 64, true, T};
      e::NoteStageBlock b;
      b.time = T;
      b.frames = 64;
      b.clock.tempo = &r.tempo;
      b.clock.sampleRate = kSr;
      b.clock.segments = &seg;
      b.clock.numSegments = 1;
      o.clear();
      const auto t0 = std::chrono::steady_clock::now();
      r.rtViolations += pq::alloc_trap::countRtViolations([&] { (void)chain->run(b, o); });
      const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
      if (T > 0) worstUs = std::max(worstUs, us);
      if (T > 0 && (T + 63) / 3000 != (T - 1) / 3000) {  // a block with a step (512 notes start)
        stepUs += us;
        ++stepBlocks;
      }
      for (uint32_t i = 0; i < o.size(); ++i) res.push_back({T + o.list().events[i].frame, o.list().events[i]});
    }
    return res;
  }();
  CHECK(r.rtViolations == 0);
  auto& core = static_cast<e::ArpStage*>(chain->stage(0))->core();
  CHECK(core.soundingCount() == e::kArpMaxSounding);  // the table is full throughout
  CHECK(core.stats().cutSounding > 0);
  CHECK(core.stats().ties > 0);
  // no id sounds twice
  std::set<uint32_t> live;
  for (const Ev& x : out) {
    if (x.ev.kind == e::NoteEventKind::NoteOn) CHECK(live.insert(x.ev.noteId).second);
    if (x.ev.kind == e::NoteEventKind::NoteOff) live.erase(x.ev.noteId);
  }
  REQUIRE(stepBlocks > 10);
  MESSAGE("step blocks: mean " << stepUs / stepBlocks << " us, worst block " << worstUs << " us");
#ifdef PQ_OPTIMIZED
  // A step starting 512 notes into a full table: before AP.8a ≈ 512 · 2 · 512 compares + a rescan per tie (measured
  // 2 – 15 ms per step block here); now O(512) per step (≈ 50 µs). The mean over the step blocks (a shared machine:
  // one block can be preempted).
  CHECK(stepUs / stepBlocks < 500.0);
#endif
}

TEST_CASE("arp core (AP.8a L3 / L4): a harmonic beyond 2^24 is none; ids skip live ones across the 2^24 wrap") {
  SUBCASE("L3") {
    e::ArpParams p = upEighths();
    p.unit = e::ArpUnit::Ratio;
    p.ratioP = 3;
    p.ratioQ = 1;
    p.steps = 8;
    auto chain = chainOf(std::make_unique<e::ArpStage>(p), 1);
    e::NoteEvent n = on(1, 1.0f);
    n.harmonic = 999999.0f;  // x 3^7 = 2186997813: past 2^24, not a float integer
    Rig r;
    const auto ons = ofKind(r.run(*chain, {{0, n}}, 7 * 12000 + 64, 64), e::NoteEventKind::NoteOn);
    REQUIRE(ons.size() >= 8);
    CHECK(ons[0].ev.harmonic == 999999.0f);
    CHECK(ons[1].ev.harmonic == 2999997.0f);
    CHECK(ons[7].ev.harmonic == 0.0f);
  }
  SUBCASE("L4") {
    // Pulse, Proportion {1, 64}, h_ref 64: clock A (h 1) one pulse per 64 of clock B; gate 100 % — A's first note
    // (id counter 0) sounds while B's notes run the counter through 2^24.
    e::ArpParams p;
    p.pulse = true;
    p.pulseSource = e::ArpPulseSource::Proportion;
    p.proportionLen = 2;
    p.proportion[0] = 1;
    p.proportion[1] = 64;
    p.pulseRef = 64;
    p.rate = {true, 1, 64, 1, 1, 125.0f};
    p.gate = 1.0f;
    auto st = std::make_unique<e::ArpStage>(p);
    e::ArpStage* stage = st.get();
    auto chain = chainOf(std::move(st), 1);
    Rig r;
    r.before = [&](int64_t T) {
      if (T == 64) stage->core().setNoteCounter(0x00FFFFFEu);
    };
    const auto out = r.run(*chain, {{0, on(1, 100.0f)}, {0, on(2, 200.0f)}}, 20000, 64);
    std::set<uint32_t> live;
    size_t ons = 0;
    bool wrapped = false;
    for (const Ev& x : out) {
      if (x.ev.kind == e::NoteEventKind::NoteOn) {
        ++ons;
        CHECK(live.insert(x.ev.noteId).second);  // never an id that still sounds
        wrapped = wrapped || (x.ev.noteId & 0x00FFFFFFu) < 16u;
      }
      if (x.ev.kind == e::NoteEventKind::NoteOff) live.erase(x.ev.noteId);
    }
    CHECK(ons > 4);
    CHECK(wrapped);
  }
}

TEST_CASE("arp core (AP.8a L1): a retune moves the glide state — an Out Offset glide aims at the retuned pitch") {
  // Degrees, Out = Offset +2 degrees over the last 50 ms of a 100 % gate. A note of 12-EDO degree 0 (node 1); at 3000
  // samples the node's table becomes 19-EDO: the Out glide (at 12000 − 2400) goes to degree 2 of 19-EDO.
  auto box = std::make_shared<e::ArpTableMailbox>();
  e::ArpTables t12;
  t12.nodes = {edo(12, 200.0)};
  e::ArpParams p = upEighths();
  p.out = e::ArpGlide::Offset;
  p.outOffset = 2.0f;
  p.glideMs = 50.0f;
  auto chain = chainOf(std::make_unique<e::ArpStage>(p, std::vector<e::ArpStage::Mapping>{}, t12, e::ArpLaneSteps{}, box), 1);
  Rig r;
  r.before = [&](int64_t T) {
    if (T != 2944) return;
    e::ArpTables t19;
    t19.nodes = {edo(19, 200.0)};
    box->post(t19);
  };
  const auto out = r.run(*chain, {{0, degreeOn(1, 200.0f, 0, 0, 1.0f)}}, 12000, 64);
  CHECK(r.rtViolations == 0);
  bool aimed = false;
  for (const Ev& x : ofKind(out, e::NoteEventKind::Pitch))
    if (x.t == 12000 - 2400 && (x.ev.flags & e::kNoteEventRetune) == 0) {
      CHECK(x.ev.hz == doctest::Approx(edoHz(19, 200.0, 2)).epsilon(1e-6));  // not 12-EDO's degree 2
      aimed = true;
    }
  CHECK(aimed);
}
