// AP.1: note stages (pq/engine/note_stage.h) and the arpeggiator core (pq/engine/arp.h).
//   - styles against a hand-written table (n = 4: every style incl. Converge / Diverge / Pinky / Thumb / Chord
//     Trigger / Play Order), random styles deterministic;
//   - clock: synced steps on the song grid (n:m tuplets exact in samples, a tempo change), free clock when stopped,
//     groove, gate, repeats, offset, retrigger Off / Note / Beat, hold, velocity decay;
//   - transport: loop wrap / locate / stop end the steps at that frame and re-seek;
//   - block-size invariance (32 / 64 / 512 / 37 frames), a 128-note held set, Degrees and Ratio distance with exact
//     identity fields, Rotate, expression carry-over, the generated id space;
//   - RtScope on the stage entry point and through the real Engine + VoiceEngine (install, play, handover).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/arp.h"
#include "pq/engine/engine.h"
#include "pq/engine/note_stage.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/graph/parse.h"
#include "pq/graph/project.h"

namespace e = pq::engine;
using pq::SampleTime;

namespace {

constexpr double kSr = 48000.0;
constexpr int64_t kSixteenth = 6000;  // 1/16 at 120 BPM, 48 kHz

struct Ev {
  int64_t t;
  e::NoteEvent ev;
};

// A transport for the harness: plays from engine time `start` (song sample startPos) until `stop`, with an
// optional loop [loopA, loopB) in song samples and locates (engine time → song sample).
struct Transport {
  bool playing = false;
  int64_t start = 0, stop = INT64_MAX;
  SampleTime startPos = 0;
  SampleTime loopA = 0, loopB = 0;
  std::vector<std::pair<int64_t, SampleTime>> locates;
};

struct Harness {
  pq::TempoMap tempo;
  e::EventBuffer out{4096};
  std::vector<e::ClockSegment> segs;
  uint64_t rtViolations = 0;  // inside ArpCore::process (AP.3)

  std::vector<Ev> run(e::ArpCore& arp, const e::ArpParams& p, std::vector<Ev> in, const Transport& tr, int64_t total,
                      uint32_t block) {
    std::stable_sort(in.begin(), in.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    std::vector<Ev> res;
    e::EventBuffer inBuf(4096);
    size_t ii = 0;
    SampleTime pos = tr.startPos;
    for (int64_t T = 0; T < total; T += block) {
      const auto frames = static_cast<uint32_t>(std::min<int64_t>(block, total - T));
      segs.clear();
      for (uint32_t f = 0; f < frames; ++f) {
        const int64_t t = T + f;
        const bool play = tr.playing && t >= tr.start && t < tr.stop;
        for (const auto& l : tr.locates)
          if (l.first == t) pos = l.second;
        if (play && tr.loopB > tr.loopA && pos == tr.loopB) pos = tr.loopA;
        if (!segs.empty() && segs.back().song == play && (!play || segs.back().pos + segs.back().len == pos)) {
          ++segs.back().len;
        } else {
          segs.push_back({f, 1, play, play ? pos : 0});
        }
        if (play) ++pos;
      }
      inBuf.clear();
      while (ii < in.size() && in[ii].t < T + frames) {
        e::NoteEvent ev = in[ii].ev;
        ev.frame = static_cast<uint32_t>(in[ii].t - T);
        inBuf.push(ev);
        ++ii;
      }
      e::NoteStageBlock b;
      b.time = T;
      b.frames = frames;
      b.clock.tempo = &tempo;
      b.clock.sampleRate = kSr;
      b.clock.bpm = 120.0;
      b.clock.segments = segs.data();
      b.clock.numSegments = static_cast<uint32_t>(segs.size());
      out.clear();
      rtViolations += pq::alloc_trap::countRtViolations([&] { arp.process(p, b, inBuf.list(), out); });
      for (uint32_t i = 0; i < out.size(); ++i) res.push_back({T + out.list().events[i].frame, out.list().events[i]});
    }
    return res;
  }
};

e::NoteEvent on(uint32_t id, float hz, float vel = 1.0f) { return e::noteOn(0, id, hz, vel); }
e::NoteEvent off(uint32_t id) { return e::noteOff(0, id); }

std::vector<Ev> ons(const std::vector<Ev>& v) {
  std::vector<Ev> r;
  for (const Ev& x : v)
    if (x.ev.kind == e::NoteEventKind::NoteOn) r.push_back(x);
  return r;
}

e::ArpParams freeParams(e::ArpStyle style) {
  e::ArpParams p;
  p.style = style;
  p.rate = {true, 1, 16, 1, 1, 125.0f};
  return p;
}

// The indices (into 200 < 250 < 300 < 400) of the first `count` steps of `style` over four held notes pressed in
// the order 300, 200, 400, 250 (stopped transport: the free clock from the first note-on).
std::vector<std::vector<int>> stepsOf(e::ArpStyle style, int count, uint32_t seed = 0) {
  const float hz[4] = {300.0f, 200.0f, 400.0f, 250.0f};
  std::map<float, int> index{{200.0f, 0}, {250.0f, 1}, {300.0f, 2}, {400.0f, 3}};
  std::vector<Ev> in;
  for (uint32_t i = 0; i < 4; ++i) in.push_back({0, on(i + 1, hz[i])});
  e::ArpCore arp;
  Harness h;
  e::ArpParams p = freeParams(style);
  p.seed = seed;
  const auto out = ons(h.run(arp, p, in, {}, kSixteenth * count, 64));
  std::vector<std::vector<int>> steps;
  int64_t last = -1;
  for (const Ev& x : out) {
    if (x.t != last) steps.emplace_back();
    last = x.t;
    steps.back().push_back(index.at(x.ev.hz));
  }
  return steps;
}

std::vector<int> flat(const std::vector<std::vector<int>>& s) {
  std::vector<int> r;
  for (const auto& v : s) r.insert(r.end(), v.begin(), v.end());
  return r;
}

std::vector<int> repeat(std::vector<int> base, size_t n) {
  std::vector<int> r;
  while (r.size() < n) r.insert(r.end(), base.begin(), base.end());
  r.resize(n);
  return r;
}

}  // namespace

TEST_CASE("arp: every style against a hand-written table (four held notes)") {
  struct Row {
    e::ArpStyle style;
    std::vector<int> cycle;
  };
  const Row rows[] = {
      {e::ArpStyle::Up, {0, 1, 2, 3}},
      {e::ArpStyle::Down, {3, 2, 1, 0}},
      {e::ArpStyle::UpDown, {0, 1, 2, 3, 2, 1}},
      {e::ArpStyle::DownUp, {3, 2, 1, 0, 1, 2}},
      {e::ArpStyle::UpAndDown, {0, 1, 2, 3, 3, 2, 1, 0}},
      {e::ArpStyle::DownAndUp, {3, 2, 1, 0, 0, 1, 2, 3}},
      {e::ArpStyle::Converge, {0, 3, 1, 2}},
      {e::ArpStyle::Diverge, {2, 1, 3, 0}},
      {e::ArpStyle::ConAndDiverge, {0, 3, 1, 2, 1, 3}},
      {e::ArpStyle::PinkyUp, {0, 3, 1, 3, 2, 3}},
      {e::ArpStyle::PinkyUpDown, {0, 3, 1, 3, 2, 3, 1, 3}},
      {e::ArpStyle::ThumbUp, {0, 1, 0, 2, 0, 3}},
      {e::ArpStyle::ThumbUpDown, {0, 1, 0, 2, 0, 3, 0, 2}},
      {e::ArpStyle::PlayOrder, {2, 0, 3, 1}},  // pressed 300, 200, 400, 250
  };
  for (const Row& r : rows) {
    CAPTURE(static_cast<int>(r.style));
    const auto steps = stepsOf(r.style, 20);
    REQUIRE(steps.size() == 20);
    for (const auto& s : steps) CHECK(s.size() == 1);
    CHECK(flat(steps) == repeat(r.cycle, 20));
  }
  const auto chord = stepsOf(e::ArpStyle::ChordTrigger, 5);
  REQUIRE(chord.size() == 5);
  for (auto s : chord) {
    std::sort(s.begin(), s.end());
    CHECK(s == std::vector<int>{0, 1, 2, 3});
  }
}

TEST_CASE("arp: random styles draw from (seed, grid step): deterministic, Random Other / Once permute") {
  const auto r1 = flat(stepsOf(e::ArpStyle::Random, 64, 7));
  CHECK(r1 == flat(stepsOf(e::ArpStyle::Random, 64, 7)));
  CHECK(r1 != flat(stepsOf(e::ArpStyle::Random, 64, 8)));
  CHECK(std::set<int>(r1.begin(), r1.end()).size() == 4);
  const auto other = flat(stepsOf(e::ArpStyle::RandomOther, 40, 3));
  bool differs = false;
  for (size_t c = 0; c + 4 <= other.size(); c += 4) {
    std::vector<int> cyc(other.begin() + static_cast<long>(c), other.begin() + static_cast<long>(c) + 4);
    std::sort(cyc.begin(), cyc.end());
    CHECK(cyc == std::vector<int>{0, 1, 2, 3});  // every note once per cycle
    if (c > 0 && !std::equal(other.begin() + static_cast<long>(c), other.begin() + static_cast<long>(c) + 4, other.begin()))
      differs = true;
  }
  CHECK(differs);
  const auto once = flat(stepsOf(e::ArpStyle::RandomOnce, 40, 3));
  std::vector<int> first(once.begin(), once.begin() + 4);
  CHECK(once == repeat(first, 40));
  std::sort(first.begin(), first.end());
  CHECK(first == std::vector<int>{0, 1, 2, 3});
}

TEST_CASE("arp: free clock (stopped): steps from the first note-on, gate, groove, repeats, offset, velocity decay") {
  std::vector<Ev> in = {{1000, on(1, 200.0f, 1.0f)}, {1000, on(2, 300.0f, 1.0f)}, {1000, on(3, 400.0f, 1.0f)}};
  Harness h;
  {
    e::ArpCore arp;
    e::ArpParams p = freeParams(e::ArpStyle::Up);
    p.gate = 0.5f;
    p.groove = 1.0f;
    const auto out = h.run(arp, p, in, {}, 1000 + kSixteenth * 6, 64);
    std::vector<int64_t> onT, offT;
    for (const Ev& x : out) (x.ev.kind == e::NoteEventKind::NoteOn ? onT : offT).push_back(x.t);
    REQUIRE(onT.size() == 6);
    for (int k = 0; k < 6; ++k) {
      const int64_t expect = 1000 + k * kSixteenth + (k % 2 == 1 ? kSixteenth / 2 : 0);
      CHECK(onT[static_cast<size_t>(k)] == expect);
    }
    REQUIRE(offT.size() >= 5);
    CHECK(offT[0] == 1000 + kSixteenth / 2);
  }
  {
    e::ArpCore arp;
    e::ArpParams p = freeParams(e::ArpStyle::Up);
    p.repeats = 2;
    p.offset = 1;
    const auto out = ons(h.run(arp, p, in, {}, 1000 + kSixteenth * 12, 64));
    REQUIRE(out.size() == 6);  // two cycles of three, then silence
    const float expect[6] = {300.0f, 400.0f, 200.0f, 300.0f, 400.0f, 200.0f};
    for (int i = 0; i < 6; ++i) CHECK(out[static_cast<size_t>(i)].ev.hz == expect[i]);
  }
  {
    e::ArpCore arp;
    e::ArpParams p = freeParams(e::ArpStyle::Up);
    p.velocityDecayMs = 1000.0f;
    p.velocityTarget = 0.0f;
    p.gate = 1.5f;  // overlapping notes
    const auto all = h.run(arp, p, in, {}, 1000 + kSixteenth * 4, 64);
    const auto out = ons(all);
    REQUIRE(out.size() == 4);
    for (int k = 0; k < 4; ++k) CHECK(out[static_cast<size_t>(k)].ev.velocity == doctest::Approx(1.0 - 0.125 * k).epsilon(1e-6));
    // gate 150 %: step 0's off comes after step 1's on
    int64_t off0 = -1;
    for (const Ev& x : all)
      if (x.ev.kind == e::NoteEventKind::NoteOff && x.ev.noteId == out[0].ev.noteId) off0 = x.t;
    CHECK(off0 == 1000 + kSixteenth * 3 / 2);
  }
}

TEST_CASE("arp: synced steps sit on the song grid; n:m tuplets exact in samples; a tempo change") {
  struct Case {
    int32_t num, den, n, m;
  };
  for (const Case c : {Case{1, 16, 1, 1}, Case{1, 8, 3, 2}, Case{1, 8, 2, 3}, Case{1, 4, 5, 4}, Case{1, 16, 7, 8},
                       Case{1, 16, 13, 8}}) {
    CAPTURE(c.den);
    CAPTURE(c.n);
    CAPTURE(c.m);
    e::ArpCore arp;
    Harness h;
    e::ArpParams p;
    p.rate = {true, c.num, c.den, c.n, c.m, 100.0f};
    Transport tr;
    tr.playing = true;
    const auto out = ons(h.run(arp, p, {{0, on(1, 440.0f)}}, tr, 48000 * 3, 64));
    REQUIRE(out.size() > 4);
    // step k at llround(k · L ticks in samples), L = whole · num · m / (den · n): exact rationals
    const int64_t num = pq::kTicksPerWhole * c.num * c.m * 24000, den = static_cast<int64_t>(c.den) * c.n * pq::kPpq;
    for (size_t k = 0; k < out.size(); ++k) {
      const int64_t x = static_cast<int64_t>(k) * num;
      CHECK(out[k].t == (2 * x + den) / (2 * den));
    }
  }
  // A tempo map (120 → 90 BPM at bar 2): steps at ticksToSamples(k · L), the timeline's own conversion.
  pq::TempoPoint pts[2] = {{0, 120.0}, {4 * pq::kPpq, 90.0}};
  auto tm = pq::TempoMap::create(pts);
  REQUIRE(tm.ok());
  e::ArpCore arp;
  Harness h;
  h.tempo = tm.value();
  e::ArpParams p;
  p.rate = {true, 1, 8, 3, 2, 100.0f};  // L = kPpq / 3 ticks
  Transport tr;
  tr.playing = true;
  const auto out = ons(h.run(arp, p, {{0, on(1, 440.0f)}}, tr, 48000 * 5, 128));
  REQUIRE(out.size() > 24);
  for (size_t k = 0; k < out.size(); ++k)
    CHECK(out[k].t == h.tempo.ticksToSamples(static_cast<pq::Ticks>(k) * (pq::kPpq / 3), kSr));
  // AP.3 (Q?-AP.1-a (3), decided): a note-on between steps into an idle arp sounds at once; the later steps stay on
  // the grid; the first one's gate (50 % of 6000) ends before the next grid step.
  e::ArpCore late;
  p.rate = {true, 1, 16, 1, 1, 100.0f};
  Harness hl;
  const auto all2 = hl.run(late, p, {{7000, on(1, 440.0f)}}, tr, 30000, 64);
  const auto o2 = ons(all2);
  REQUIRE(o2.size() >= 3);
  CHECK(o2[0].t == 7000);
  CHECK(o2[1].t == 12000);
  CHECK(o2[2].t == 18000);
  for (const Ev& x : all2)
    if (x.ev.kind == e::NoteEventKind::NoteOff && x.ev.noteId == o2[0].ev.noteId) CHECK(x.t == 10000);
  // Gate 150 %: the first note keeps its full gate length (9000) and overlaps the next grid step.
  e::ArpCore late2;
  p.gate = 1.5f;
  const auto all3 = Harness{}.run(late2, p, {{7000, on(1, 440.0f)}}, tr, 30000, 64);
  const auto o3 = ons(all3);
  REQUIRE(o3.size() >= 2);
  CHECK(o3[0].t == 7000);
  CHECK(o3[1].t == 12000);
  for (const Ev& x : all3)
    if (x.ev.kind == e::NoteEventKind::NoteOff && x.ev.noteId == o3[0].ev.noteId) CHECK(x.t == 16000);
  // Not idle (a generated note still sounding): a new note-on after a release waits for the grid.
  e::ArpCore busy;
  p.gate = 1.9f;
  const auto o4 = ons(Harness{}.run(busy, p, {{0, on(1, 440.0f)}, {7000, off(1)}, {8000, on(2, 550.0f)}}, tr, 30000, 64));
  REQUIRE(o4.size() >= 3);
  CHECK(o4[0].t == 0);
  CHECK(o4[1].t == 6000);
  CHECK(o4[2].t == 12000);
  CHECK(o4[2].ev.hz == 550.0f);
  // A note-on exactly on a grid step is that step.
  e::ArpCore onGrid;
  p.gate = 0.5f;
  const auto o5 = ons(Harness{}.run(onGrid, p, {{12000, on(1, 440.0f)}}, tr, 30000, 64));
  REQUIRE(o5.size() >= 2);
  CHECK(o5[0].t == 12000);
  CHECK(o5[1].t == 18000);
}

TEST_CASE("arp: the same events at any block size (32 / 64 / 512 / 37 frames)") {
  std::vector<Ev> in = {{100, on(1, 220.0f, 0.9f)},   {130, on(2, 330.0f, 0.7f)},   {2000, on(3, 275.0f, 0.5f)},
                        {9000, off(2)},               {15011, on(4, 500.0f, 0.6f)}, {30000, off(1)},
                        {41000, off(3)},              {52000, on(5, 260.0f, 0.8f)}, {70000, off(4)},
                        {90000, off(5)}};
  e::NoteEvent bend = e::notePitch(0, 3, 300.0f, 5000);
  in.push_back({20000, bend});
  e::ArpParams p;
  p.style = e::ArpStyle::RandomOther;
  p.rate = {true, 1, 16, 3, 2, 100.0f};
  p.gate = 1.3f;
  p.groove = 0.4f;
  p.steps = 3;
  p.distance = 5;
  p.retrigger = e::ArpRetrigger::Beat;
  p.retriggerRate = {true, 1, 2, 1, 1, 100.0f};
  p.velocityDecayMs = 700.0f;
  p.seed = 11;
  Transport tr;
  tr.playing = true;
  tr.start = 500;
  tr.stop = 80000;
  tr.loopA = 0;
  tr.loopB = 48000;  // a bar at 120 BPM
  tr.locates = {{60000, 7777}};
  auto key = [](const std::vector<Ev>& v) {
    std::vector<std::string> r;
    for (const Ev& x : v) {
      char buf[160];
      std::snprintf(buf, sizeof buf, "%lld %d %u %.6f %.6f %.6f %u", static_cast<long long>(x.t), static_cast<int>(x.ev.kind),
                    x.ev.noteId, x.ev.hz, x.ev.velocity, x.ev.gain, x.ev.rampFrames);
      r.emplace_back(buf);
    }
    return r;
  };
  std::vector<std::string> ref;
  for (uint32_t block : {64u, 32u, 512u, 37u}) {
    CAPTURE(block);
    e::ArpCore arp;
    Harness h;
    const auto out = key(h.run(arp, p, in, tr, 100000, block));
    if (ref.empty()) {
      ref = out;
      CHECK(ref.size() > 40);
    } else {
      CHECK(out == ref);
    }
  }
}

TEST_CASE("arp: loop wrap / locate / stop end the steps at that frame and re-seek the clock") {
  e::ArpParams p;
  p.rate = {true, 1, 4, 1, 1, 100.0f};  // a quarter: 24000 samples
  p.gate = 1.0f;
  p.hold = true;
  Transport tr;
  tr.playing = true;
  tr.loopA = 0;
  tr.loopB = 30000;  // a loop that cuts the second step after 6000 samples
  tr.stop = 75000;
  // A key pressed and released (latched by Hold) and one held through.
  std::vector<Ev> in = {{0, on(1, 200.0f)}, {0, on(2, 300.0f)}, {100, off(1)}};
  e::ArpCore arp;
  Harness h;
  const auto out = h.run(arp, p, in, tr, 120000, 64);
  std::vector<std::pair<int64_t, int>> seq;  // (t, kind)
  for (const Ev& x : out) seq.emplace_back(x.t, static_cast<int>(x.ev.kind));
  // ons at 0 (200), 24000 (300); wrap at 30000: off of the sounding step at 30000, then a step at 30000 (song 0,
  // 200 again: Hold kept the latched note across the wrap); 54000 (300); wrap at 60000 → 60000 (200);
  // stop at 75000: the step ends, the latched note is dropped, the free clock plays the held key from 75000 on.
  std::vector<Ev> o = ons(out);
  REQUIRE(o.size() >= 7);
  CHECK(o[0].t == 0);
  CHECK(o[0].ev.hz == 200.0f);
  CHECK(o[1].t == 24000);
  CHECK(o[2].t == 30000);
  CHECK(o[2].ev.hz == 200.0f);
  CHECK(o[3].t == 54000);
  CHECK(o[4].t == 60000);
  CHECK(o[5].t == 75000);
  CHECK(o[5].ev.hz == 300.0f);
  CHECK(o[6].t == 75000 + 24000);
  CHECK(o[6].ev.hz == 300.0f);  // only the key-held note is left
  int offsAtWrap = 0, offsAtStop = 0;
  for (const Ev& x : out) {
    if (x.ev.kind != e::NoteEventKind::NoteOff) continue;
    if (x.t == 30000) ++offsAtWrap;
    if (x.t == 75000) ++offsAtStop;
  }
  CHECK(offsAtWrap == 1);
  CHECK(offsAtStop == 1);
  CHECK(arp.stats().transportJumps == 3);
  // A locate while playing: the step ends there, the next one is the first grid step at or after the new position.
  e::ArpCore arp2;
  Transport t2;
  t2.playing = true;
  t2.locates = {{10000, 100000}};  // to song 100000: next quarter at 120000 → engine 30000
  const auto o2 = ons(Harness{}.run(arp2, p, {{0, on(1, 200.0f)}}, t2, 40000, 64));
  REQUIRE(o2.size() == 2);
  CHECK(o2[1].t == 30000);
}

TEST_CASE("arp: hold latches; a new chord replaces the latched set, a note added while a key is down joins") {
  e::ArpParams p = freeParams(e::ArpStyle::Up);
  p.hold = true;
  std::vector<Ev> in = {{0, on(1, 200.0f)},     {0, on(2, 300.0f)},     {10, off(1)},  {20, off(2)},
                        {30000, on(3, 500.0f)}, {40000, on(4, 600.0f)}, {50000, off(3)}};
  e::ArpCore arp;
  const auto o = ons(Harness{}.run(arp, p, in, {}, 72000, 64));
  std::vector<float> hz;
  for (const Ev& x : o) hz.push_back(x.ev.hz);
  // 0: 200, 6000: 300, 12000: 200, 18000: 300, 24000: 200; 30000: 500 replaces (clock restarts at the note-on);
  // 36000: 500; 40000: 600 joins (pattern goes on); 42000 …
  REQUIRE(hz.size() >= 10);
  CHECK(std::vector<float>(hz.begin(), hz.begin() + 5) == std::vector<float>{200, 300, 200, 300, 200});
  CHECK(o[5].t == 30000);
  CHECK(hz[5] == 500.0f);
  CHECK(hz[6] == 500.0f);
  CHECK(o[7].t == 42000);
  CHECK(hz[7] == 500.0f);  // pos 2 of [500, 600]
  CHECK(hz[8] == 600.0f);
  // Hold switched off: the latched notes no key holds go.
  CHECK(arp.heldCount() == 2);
  p.hold = false;
  Harness h;
  e::ArpCore arp3;
  e::ArpParams ph = p;
  ph.hold = true;
  h.run(arp3, ph, {{0, on(1, 200.0f)}, {0, on(2, 300.0f)}, {10, off(1)}}, {}, 1000, 64);
  CHECK(arp3.heldCount() == 2);
  h.run(arp3, p, {}, {}, 64, 64);
  CHECK(arp3.heldCount() == 1);
}

TEST_CASE("arp: retrigger Off / Note / Beat") {
  auto run = [](e::ArpRetrigger r, bool join) {
    e::ArpParams p = freeParams(e::ArpStyle::Up);
    p.retrigger = r;
    p.retriggerRate = {true, 1, 4, 1, 1, 100.0f};  // a quarter = 4 steps
    std::vector<Ev> in = {{0, on(1, 200.0f)}, {0, on(2, 300.0f)}, {0, on(3, 400.0f)}};
    if (join) in.push_back({13000, on(4, 100.0f)});
    e::ArpCore arp;
    std::vector<float> hz;
    for (const Ev& x : ons(Harness{}.run(arp, p, in, {}, kSixteenth * 9, 64))) hz.push_back(x.ev.hz);
    return hz;
  };
  // Off: 200 300 400 | 200 (100 joins at 13000: [100 200 300 400], pos 3 → 400) …
  CHECK(run(e::ArpRetrigger::Off, true) == std::vector<float>{200, 300, 400, 400, 100, 200, 300, 400, 100});
  CHECK(run(e::ArpRetrigger::Off, false) == std::vector<float>{200, 300, 400, 200, 300, 400, 200, 300, 400});
  // Note: the pattern restarts at the note-on: 18000 plays index 0.
  CHECK(run(e::ArpRetrigger::Note, true) == std::vector<float>{200, 300, 400, 100, 200, 300, 400, 100, 200});
  // Beat: at every quarter (4 steps) the pattern restarts.
  CHECK(run(e::ArpRetrigger::Beat, false) == std::vector<float>{200, 300, 400, 200, 200, 300, 400, 200, 200});
}

TEST_CASE("arp: a 128-note held set; the 129th note-on is dropped and its note-off passes through") {
  std::vector<Ev> in;
  for (uint32_t i = 0; i < 129; ++i) in.push_back({0, on(i + 1, 1000.0f - static_cast<float>(i))});
  in.push_back({100, off(129)});
  e::ArpCore arp;
  const auto all = Harness{}.run(arp, freeParams(e::ArpStyle::Up), in, {}, kSixteenth * 128, 64);
  CHECK(arp.stats().droppedHeld == 1);
  CHECK(arp.heldCount() == 128);
  const auto o = ons(all);
  REQUIRE(o.size() == 128);
  for (size_t i = 0; i < o.size(); ++i) CHECK(o[i].ev.hz == 1000.0f - 127.0f + static_cast<float>(i));
  bool passed = false;
  for (const Ev& x : all) passed = passed || (x.ev.kind == e::NoteEventKind::NoteOff && x.ev.noteId == 129);
  CHECK(passed);
}

TEST_CASE("arp: Distance / Steps in Degrees and Ratio carry exact identities; Rotate") {
  SUBCASE("Degrees in 12-EDO: degree + 7 per copy, equave carries over, tuning kept") {
    e::NoteEvent n = on(1, 440.0f);
    n.degree = 5.0f;
    n.equave = 4.0f;
    n.tuning = 1.0f;
    n.noteDegree = 53.0f;
    e::ArpParams p = freeParams(e::ArpStyle::Up);
    p.steps = 3;
    p.distance = 7;
    e::ArpCore arp;
    const auto o = ons(Harness{}.run(arp, p, {{0, n}}, {}, kSixteenth * 3, 64));
    REQUIRE(o.size() == 3);
    const float deg[3] = {5, 0, 7}, eq[3] = {4, 5, 5};
    for (int j = 0; j < 3; ++j) {
      const e::NoteEvent& x = o[static_cast<size_t>(j)].ev;
      CHECK(x.degree == deg[j]);
      CHECK(x.equave == eq[j]);
      CHECK(x.tuning == 1.0f);
      CHECK(x.noteDegree == 53.0f + 7.0f * j);
      CHECK(x.hz == static_cast<float>(440.0 * std::exp2(7.0 * j / 12.0)));
    }
  }
  SUBCASE("Degrees in a table (a 5-note scale): cents of the table, negative distance") {
    e::ArpDegreeTable t;
    t.cents = {0.0, 200.0, 350.0, 700.0, 900.0};
    t.equaveCents = 1210.0;
    e::NoteEvent n = on(1, 300.0f);
    n.degree = 1.0f;
    n.equave = 0.0f;
    e::ArpParams p = freeParams(e::ArpStyle::Up);
    p.steps = 3;
    p.distance = -2;
    e::ArpCore arp(0, t);
    const auto o = ons(Harness{}.run(arp, p, {{0, n}}, {}, kSixteenth * 3, 64));
    REQUIRE(o.size() == 3);
    CHECK(o[1].ev.degree == 4.0f);  // D = 1 − 2 = −1 → equave −1, degree 4
    CHECK(o[1].ev.equave == -1.0f);
    CHECK(o[1].ev.hz == static_cast<float>(300.0 * std::exp2((900.0 - 1210.0 - 200.0) / 1200.0)));
    CHECK(o[2].ev.degree == 2.0f);  // D = −3 → equave −1, degree 2
    CHECK(o[2].ev.hz == static_cast<float>(300.0 * std::exp2((350.0 - 1210.0 - 200.0) / 1200.0)));
  }
  SUBCASE("Ratio 3/2: harmonics stay integers, ratio exact, Hz from the fraction") {
    e::NoteEvent n = on(1, 440.0f);
    n.harmonic = 4.0f;
    n.ratio = 1.25f;
    n.degree = 2.0f;
    n.equave = 4.0f;
    n.tuning = 1.0f;
    e::ArpParams p = freeParams(e::ArpStyle::Up);
    p.unit = e::ArpUnit::Ratio;
    p.ratioP = 3;
    p.ratioQ = 2;
    p.steps = 4;
    e::ArpCore arp;
    const auto o = ons(Harness{}.run(arp, p, {{0, n}}, {}, kSixteenth * 4, 64));
    REQUIRE(o.size() == 4);
    CHECK(o[0].ev.harmonic == 4.0f);
    CHECK(o[0].ev.degree == 2.0f);  // copy 0 keeps the identity
    CHECK(o[1].ev.harmonic == 6.0f);
    CHECK(o[2].ev.harmonic == 9.0f);
    CHECK(o[3].ev.harmonic == 0.0f);  // 27/2: no harmonic of the anchor
    CHECK(o[2].ev.hz == static_cast<float>(440.0 * 9.0 / 4.0));
    CHECK(o[3].ev.hz == static_cast<float>(440.0 * 27.0 / 8.0));
    CHECK(o[2].ev.ratio == static_cast<float>(1.25 * 9.0 / 4.0));
    // AP.8a (review H1): a ratio step keeps the source's degree identity (the tuning node moves it at its ratio)
    CHECK(o[1].ev.degree == 2.0f);
    CHECK(o[1].ev.equave == n.equave);
    CHECK(o[1].ev.tuning == n.tuning);
  }
  SUBCASE("Rotate: copy j starts j notes up the held set, up an equave per wrap") {
    std::vector<Ev> in = {{0, on(1, 200.0f)}, {0, on(2, 250.0f)}, {0, on(3, 300.0f)}};
    e::ArpParams p = freeParams(e::ArpStyle::Up);
    p.transpose = e::ArpTranspose::Rotate;
    p.steps = 2;
    p.distance = 1;
    e::ArpCore arp;
    std::vector<float> hz;
    for (const Ev& x : ons(Harness{}.run(arp, p, in, {}, kSixteenth * 6, 64))) hz.push_back(x.ev.hz);
    CHECK(hz == std::vector<float>{200, 250, 300, 250, 300, 400});
    p.unit = e::ArpUnit::Ratio;  // the equave is p/q = 3/2
    p.ratioP = 3;
    p.ratioQ = 2;
    e::ArpCore arp2;
    hz.clear();
    for (const Ev& x : ons(Harness{}.run(arp2, p, in, {}, kSixteenth * 6, 64))) hz.push_back(x.ev.hz);
    CHECK(hz == std::vector<float>{200, 250, 300, 250, 300, 300});
  }
}

TEST_CASE("arp: a held note's Pitch / Gain curve carries over to the steps it produces") {
  e::ArpParams p = freeParams(e::ArpStyle::Up);
  p.gate = 1.0f;
  std::vector<Ev> in = {{0, on(1, 400.0f)},
                        {1000, e::notePitch(0, 1, 800.0f, 6000)},
                        {7000, e::noteGain(0, 1, 0.5f, 0)},
                        {14000, e::noteGain(0, 1, 0.25f, 8000)}};
  e::ArpCore arp;
  const auto out = Harness{}.run(arp, p, in, {}, 19000, 64);
  std::vector<Ev> ex;
  for (const Ev& x : out)
    if (x.ev.kind != e::NoteEventKind::NoteOff) ex.push_back(x);
  REQUIRE(ex.size() >= 9);
  const uint32_t id0 = ex[0].ev.noteId;
  CHECK(ex[1].t == 1000);
  CHECK(ex[1].ev.kind == e::NoteEventKind::Pitch);
  CHECK(ex[1].ev.noteId == id0);
  CHECK(ex[1].ev.hz == 800.0f);
  CHECK(ex[1].ev.rampFrames == 6000);
  // Step 1 at 6000, 5/6 into the ramp: starts there and ramps on for the remaining 1000 samples.
  CHECK(ex[2].t == 6000);
  CHECK(ex[2].ev.kind == e::NoteEventKind::NoteOn);
  CHECK(ex[2].ev.hz == doctest::Approx(400.0 * std::exp2(5.0 / 6.0)).epsilon(1e-6));
  CHECK(ex[3].ev.kind == e::NoteEventKind::Pitch);
  CHECK(ex[3].ev.hz == 800.0f);
  CHECK(ex[3].ev.rampFrames == 1000);
  CHECK(ex[4].t == 7000);
  CHECK(ex[4].ev.kind == e::NoteEventKind::Gain);
  CHECK(ex[4].ev.noteId == ex[2].ev.noteId);
  CHECK(ex[4].ev.gain == 0.5f);
  CHECK(ex[5].t == 12000);  // step 2: bent fully, gain 0.5
  CHECK(ex[5].ev.hz == 800.0f);
  CHECK(ex[5].ev.gain == 0.5f);
  CHECK(ex[6].t == 14000);
  CHECK(ex[6].ev.kind == e::NoteEventKind::Gain);
  CHECK(ex[6].ev.rampFrames == 8000);
  CHECK(ex[7].t == 18000);  // step 3, halfway through the gain ramp
  CHECK(ex[7].ev.gain == doctest::Approx(0.375));
  CHECK(ex[8].ev.kind == e::NoteEventKind::Gain);
  CHECK(ex[8].ev.rampFrames == 4000);
  CHECK(ex[8].ev.gain == 0.25f);
  // A retune re-derives the identity of the sounding step.
  e::ArpCore arp2;
  e::NoteEvent n = on(1, 440.0f);
  n.degree = 9.0f;
  n.equave = 4.0f;
  e::NoteEvent rt = e::notePitch(0, 1, 466.0f, 0);
  rt.flags = e::kNoteEventRetune;
  rt.degree = 10.0f;
  rt.equave = 4.0f;
  const auto o2 = Harness{}.run(arp2, p, {{0, n}, {100, rt}}, {}, 200, 64);
  REQUIRE(o2.size() == 2);
  CHECK(o2[1].ev.kind == e::NoteEventKind::Pitch);
  CHECK((o2[1].ev.flags & e::kNoteEventRetune) != 0);
  CHECK(o2[1].ev.degree == 10.0f);
  CHECK(o2[1].ev.hz == 466.0f);
}

TEST_CASE("arp: generated ids live in the stage's own space and never collide among sounding notes") {
  e::ArpParams p = freeParams(e::ArpStyle::ChordTrigger);
  p.gate = 2.0f;
  p.rate = {true, 1, 64, 1, 1, 100.0f};
  std::vector<Ev> in;
  for (uint32_t i = 0; i < 16; ++i) in.push_back({0, on(0x7FFFFFF0u + i, 100.0f + i)});
  e::ArpCore arp(3);
  const auto out = Harness{}.run(arp, p, in, {}, 48000, 64);
  std::set<uint32_t> sounding;
  size_t n = 0;
  for (const Ev& x : out) {
    CHECK(e::isStageNoteId(x.ev.noteId));
    CHECK((x.ev.noteId >> 24) == 0x83u);
    if (x.ev.kind == e::NoteEventKind::NoteOn) {
      CHECK(sounding.insert(x.ev.noteId).second);
      ++n;
    } else if (x.ev.kind == e::NoteEventKind::NoteOff) {
      CHECK(sounding.erase(x.ev.noteId) == 1);
    }
  }
  CHECK(n == 16 * 32);  // 32 chords of 16 at 1/64, two chords overlapping (gate 200 %)
  CHECK(arp.stats().cutSounding == 0);
  CHECK(!e::isStageNoteId(0x7FFFFFFFu));
  CHECK(!e::isStageNoteId(0xE0000001u));
  CHECK(!e::isStageNoteId(0xF0000001u));
}

TEST_CASE("note stages: a chain of two arps (ids per stage), RtScope on the stage entry point") {
  e::NoteStageChain chain(1024);
  e::ArpParams p = freeParams(e::ArpStyle::Up);
  REQUIRE(chain.add(std::make_unique<e::ArpStage>(p)));
  e::ArpParams p2 = freeParams(e::ArpStyle::Up);
  p2.rate = {true, 1, 64, 1, 1, 100.0f};
  p2.gate = 0.25f;
  REQUIRE(chain.add(std::make_unique<e::ArpStage>(p2)));
  e::EventBuffer out(1024);
  pq::TempoMap tempo;
  e::ClockSegment seg{0, 64, true, 0};
  const uint64_t violations = pq::alloc_trap::countRtViolations([&] {
    for (int64_t T = 0; T < 48000; T += 64) {
      e::EventBuffer& in = chain.input();
      in.clear();
      if (T == 0) {
        in.push(e::noteOn(3, 1, 300.0f, 0.8f));
        in.push(e::noteOn(3, 2, 400.0f, 0.8f));
      }
      if (T == 24000) in.push(e::noteOff(0, 1));
      seg.pos = T;
      e::NoteStageBlock b;
      b.time = T;
      b.frames = 64;
      b.clock.tempo = &tempo;
      b.clock.sampleRate = kSr;
      b.clock.segments = &seg;
      b.clock.numSegments = 1;
      out.clear();
      chain.run(b, out);
      for (uint32_t i = 0; i < out.size(); ++i) CHECK((out.list().events[i].noteId >> 24) == 0x81u);  // stage 1's
    }
  });
  CHECK(violations == 0);
  // the voices see stage 1's notes only
  auto& s1 = static_cast<e::ArpStage*>(chain.stage(1))->core();
  // stage 0: a step at the note-ons (frame 3: idle, AP.3) ending at 3003, then 6000 … 42000 on the grid; stage 1
  // plays the first note at 3, 1500, 3000 (idle again: at once) and each later 3000-sample note twice at 1/64
  CHECK(static_cast<e::ArpStage*>(chain.stage(0))->core().stats().notes == 8);
  CHECK(s1.stats().notes == 17);
  // flush: offs for the sounding notes, the key-held source note (id 2) as a note-on
  e::EventBuffer held(64);
  out.clear();
  chain.flush(0, out, held);
  REQUIRE(held.size() == 1);
  CHECK(held.list().events[0].noteId == 2);
  CHECK(held.list().events[0].hz == 400.0f);
  for (uint32_t i = 0; i < out.size(); ++i) {
    CHECK(out.list().events[i].kind == e::NoteEventKind::NoteOff);
  }
}

TEST_CASE("note stages: runtime params from the project param block (ArpStage::map)") {
  e::ArpStage st(freeParams(e::ArpStyle::Up), {{e::ArpParamId::Style, 0}, {e::ArpParamId::RateDen, 1}});
  float params[2] = {static_cast<float>(e::ArpStyle::Down), 8.0f};
  e::EventBuffer in(64), out(64);
  in.push(e::noteOn(0, 1, 200.0f, 1.0f));
  in.push(e::noteOn(0, 2, 300.0f, 1.0f));
  std::vector<Ev> res;
  for (int64_t T = 0; T < 24000; T += 64) {
    e::NoteStageBlock b;
    b.time = T;
    b.frames = 64;
    b.clock.sampleRate = kSr;
    b.params = params;
    out.clear();
    st.processFn(&st, b, T == 0 ? in.list() : e::EventList{}, out);
    for (uint32_t i = 0; i < out.size(); ++i)
      if (out.list().events[i].kind == e::NoteEventKind::NoteOn) res.push_back({T + out.list().events[i].frame, out.list().events[i]});
  }
  REQUIRE(res.size() == 2);  // 1/8: steps at 0 and 12000, Down
  CHECK(res[0].ev.hz == 300.0f);
  CHECK(res[1].t == 12000);
  CHECK(res[1].ev.hz == 200.0f);
}

// ---- Through the real engine ----------------------------------------------------------------------------------

namespace {

const char* kDevices = R"(
patch Synth {
  param level = 0.3 [0..1]
  out   out : audio voice mono
  target = voice.gate ? 1.0 : 0.0
  env = z1(env + (target - env) * 0.02)
  out = sine(phasor(voice.hz)) * env * level
}

patch Pass {
  in    x : audio global mono
  out   y : audio global mono
  y = x
}
)";

const char* kTracks = R"(
patch Track {
  out   out : audio global mono
  out = Pass(Synth())
}

patch Master {
  in    in : audio global mono
  out   out : audio global mono
  out = tanh(in)
}
)";

struct Project {
  pq::graph::PatchFile devices, tracks;
  pq::graph::PatchLibrary library;
  pq::graph::ProjectGraph graph;
};

std::unique_ptr<Project> makeProject() {
  auto p = std::make_unique<Project>();
  auto d = pq::graph::parseDetailed(kDevices, "devices.pqp");
  REQUIRE(d.ok());
  p->devices = std::move(d.file);
  p->library.add(p->devices);
  pq::graph::ParseOptions po;
  const auto sigs = pq::graph::signatures(p->devices);
  po.externals = sigs;
  auto t = pq::graph::parseDetailed(kTracks, "tracks.pqp", po);
  REQUIRE(t.ok());
  p->tracks = std::move(t.file);
  pq::graph::ProjectGraph& g = p->graph;
  g.library = &p->library;
  g.tracks.push_back({1, "T0", &p->tracks.patches[0], pq::graph::TrackRole::Instrument, 0});
  g.tracks.push_back({101, "Master", &p->tracks.patches[1], pq::graph::TrackRole::Master, pq::graph::kNone});
  g.wires.push_back({{0, "out"}, {1, "in"}, pq::graph::WireKind::Audio});
  g.output = {1, "out"};
  return p;
}

}  // namespace

TEST_CASE("note stages: an arp installed on a slot plays through the Engine and its VoiceEngine (RtScope, handover)") {
  e::EngineConfig ec;
  ec.sampleRate = kSr;
  ec.blockFrames = 64;
  ec.maxSlots = 2;
  ec.sleepAfterMs = 10.0f;
  auto engine = e::Engine::create(ec);
  REQUIRE(engine.ok());
  e::Engine& en = *engine.value();
  auto proj = makeProject();
  e::ProjectCompileOptions co;
  co.sampleRate = kSr;
  co.maxBlockFrames = 64;
  co.voice.maxVoices = 16;
  co.voice.releaseTimeoutMs = 20.0f;
  e::ProjectCompiler compiler(co);
  auto built = compiler.build(proj->graph);
  REQUIRE(built.ok());
  e::ProjectConfig pc = en.projectConfig();
  auto prog = e::ProjectProgram::create(pc, built.value().layout);
  REQUIRE(prog.ok());
  for (uint32_t u = 0; u < built.value().images.size(); ++u)
    REQUIRE(prog.value()->publish(u, std::move(built.value().images[u])).ok());
  en.setProgram(std::move(prog.value()));
  e::VoiceEngine* voices = nullptr;
  for (uint32_t u = 0; u < 4 && voices == nullptr; ++u) voices = en.program()->voices(u);
  REQUIRE(voices != nullptr);

  auto chain = std::make_unique<e::NoteStageChain>(pc.eventCapacity);
  e::ArpParams p = freeParams(e::ArpStyle::Up);
  p.hold = true;
  REQUIRE(chain->add(std::make_unique<e::ArpStage>(p)));
  e::ArpStage* arp = static_cast<e::ArpStage*>(chain->stage(0));
  REQUIRE(en.setNoteStages(0, std::move(chain)));

  std::vector<float> l(64);
  float* outs[1] = {l.data()};
  double peak = 0.0;
  auto run = [&](int blocks) {
    return pq::alloc_trap::countRtViolations([&] {
      for (int i = 0; i < blocks; ++i) {
        e::AudioBlock ab;
        ab.out = outs;
        ab.numOut = 1;
        ab.frames = 64;
        pq::DenormalGuard dg;
        en.process(ab);
        for (float x : l) peak = std::max(peak, static_cast<double>(std::fabs(x)));
      }
    });
  };
  CHECK(run(2) == 0);
  // A 0.1 s audition (a held key), latched by Hold: the arp plays it on the free clock (stopped transport) …
  REQUIRE(en.audition(0, 440.0f, 100.0f, 4800));
  CHECK(run(375) == 0);  // 0.5 s: steps at 0, 6000, …, 18000 → 4 (+1 at 24000 within the window)
  CHECK(arp->core().stats().notes >= 4);
  CHECK(voices->stats().noteOns == arp->core().stats().notes);
  CHECK(peak > 0.01);
  CHECK(arp->core().heldCount() == 1);
  CHECK_FALSE(en.asleep());  // the latched note keeps the engine awake
  CHECK(run(375) == 0);
  CHECK_FALSE(en.asleep());
  const uint64_t ons = voices->stats().noteOns;
  // … removing the chain ends its notes; no key holds the latched note, so nothing is handed on.
  REQUIRE(en.setNoteStages(0, nullptr));
  CHECK(run(2) == 0);
  CHECK(voices->stats().noteOns == ons);
  en.collect();
  CHECK(run(2000) == 0);
  CHECK(en.asleep());
  // A key held through a replace goes on into the new chain, and back to the voices when the chain is removed.
  auto c2 = std::make_unique<e::NoteStageChain>(pc.eventCapacity);
  REQUIRE(c2->add(std::make_unique<e::ArpStage>(freeParams(e::ArpStyle::Up))));
  e::ArpStage* arp2 = static_cast<e::ArpStage*>(c2->stage(0));
  REQUIRE(en.setNoteStages(0, std::move(c2)));
  REQUIRE(en.audition(0, 330.0f, 100.0f, 48000));
  CHECK(run(100) == 0);
  CHECK(arp2->core().heldCount() == 1);
  const uint64_t ons2 = voices->stats().noteOns;
  CHECK(ons2 > ons);
  auto c3 = std::make_unique<e::NoteStageChain>(pc.eventCapacity);
  REQUIRE(c3->add(std::make_unique<e::ArpStage>(freeParams(e::ArpStyle::Up))));
  e::ArpStage* arp3 = static_cast<e::ArpStage*>(c3->stage(0));
  REQUIRE(en.setNoteStages(0, std::move(c3)));
  CHECK(run(1) == 0);
  CHECK(arp3->core().heldCount() == 1);
  CHECK(arp3->core().stats().notes == 1);  // its free clock starts at the handed-over note-on
  en.collect();
  REQUIRE(en.setNoteStages(0, nullptr));
  CHECK(run(1) == 0);
  CHECK(voices->stats().noteOns == ons2 + 2);  // arp3's step, then the source note itself
  CHECK(run(800) == 0);  // the audition's own note-off releases it
  en.collect();
  CHECK(run(2000) == 0);
  CHECK(en.asleep());
}

// ---- AP.3: pitch (Harmonics unit, Harmonic Walk, Into / Out glides) --------------------------------------------------

namespace {

e::NoteEvent jiOn(uint32_t id, float hz, float harmonic, float ratio = 1.0f) {
  e::NoteEvent n = on(id, hz);
  n.harmonic = harmonic;
  n.ratio = ratio;
  return n;
}

std::vector<Ev> pitchesOf(const std::vector<Ev>& v, uint32_t id) {
  std::vector<Ev> r;
  for (const Ev& x : v)
    if (x.ev.kind == e::NoteEventKind::Pitch && x.ev.noteId == id) r.push_back(x);
  return r;
}

}  // namespace

TEST_CASE("arp (AP.3): Harmonics unit — h + k over the anchor, exact ratio / harmonic; Base; Rotate doubles; rests") {
  SUBCASE("a JI note (harmonic 8, ratio 2/1 of its anchor): 8, 9, 10 per copy") {
    e::NoteEvent n = jiOn(1, 400.0f, 8.0f, 2.0f);
    n.degree = 3.0f;
    n.equave = 4.0f;
    n.tuning = 1.0f;
    n.noteDegree = 51.0f;
    e::ArpParams p = freeParams(e::ArpStyle::Up);
    p.unit = e::ArpUnit::Harmonics;
    p.steps = 3;
    e::ArpCore arp;
    const auto o = ons(Harness{}.run(arp, p, {{0, n}}, {}, kSixteenth * 3, 64));
    REQUIRE(o.size() == 3);
    CHECK(o[0].ev.harmonic == 8.0f);  // copy 0: the source unchanged
    CHECK(o[0].ev.degree == 3.0f);
    for (int j = 1; j < 3; ++j) {
      const e::NoteEvent& x = o[static_cast<size_t>(j)].ev;
      CHECK(x.harmonic == 8.0f + static_cast<float>(j));
      CHECK(x.hz == static_cast<float>(400.0 * (8 + j) / 8.0));
      CHECK(x.ratio == static_cast<float>(2.0 * (8 + j) / 8.0));
      CHECK(x.degree == 3.0f);  // AP.8a (review H1): the source's degree identity, moved by the node at the ratio
      CHECK(x.equave == n.equave);
      CHECK(x.tuning == n.tuning);
      CHECK(x.noteDegree == doctest::Approx(51.0 + 12.0 * std::log2((8.0 + j) / 8.0)).epsilon(1e-6));
    }
  }
  SUBCASE("a note without a group counts as harmonic Base (5) of itself; distance -2") {
    e::ArpParams p = freeParams(e::ArpStyle::Up);
    p.unit = e::ArpUnit::Harmonics;
    p.harmonicBase = 5;
    p.steps = 3;
    p.distance = -2;
    e::ArpCore arp;
    const auto o = ons(Harness{}.run(arp, p, {{0, on(1, 500.0f)}}, {}, kSixteenth * 3, 64));
    REQUIRE(o.size() == 3);
    CHECK(o[0].ev.harmonic == 0.0f);
    CHECK(o[1].ev.hz == 300.0f);  // 500 · 3/5
    CHECK(o[1].ev.harmonic == 3.0f);
    CHECK(o[1].ev.ratio == 0.6f);
    CHECK(o[2].ev.hz == 100.0f);  // 500 · 1/5
    CHECK(o[2].ev.harmonic == 1.0f);
  }
  SUBCASE("below the first harmonic: a rest") {
    e::ArpParams p = freeParams(e::ArpStyle::Up);
    p.unit = e::ArpUnit::Harmonics;
    p.harmonicBase = 2;
    p.steps = 3;
    p.distance = -1;
    e::ArpCore arp;
    const auto all = Harness{}.run(arp, p, {{0, on(1, 200.0f)}}, {}, kSixteenth * 3, 64);
    const auto o = ons(all);
    REQUIRE(o.size() == 2);  // 200 (h 2), 100 (h 1), then h 0: nothing
    CHECK(o[1].ev.hz == 100.0f);
    CHECK(arp.stats().steps == 3);
  }
  SUBCASE("Rotate: a wrap is the series' octave (h -> 2h)") {
    e::ArpParams p = freeParams(e::ArpStyle::Up);
    p.unit = e::ArpUnit::Harmonics;
    p.transpose = e::ArpTranspose::Rotate;
    p.steps = 2;
    p.distance = 1;
    e::ArpCore arp;
    const auto o = ons(Harness{}.run(arp, p, {{0, jiOn(1, 200.0f, 4.0f)}, {0, jiOn(2, 250.0f, 5.0f)}}, {},
                                     kSixteenth * 4, 64));
    REQUIRE(o.size() == 4);
    // copy 0: [h4 h5]; copy 1: elements 1, 2 -> h5, h4 one wrap up = h8
    CHECK(o[2].ev.hz == 250.0f);
    CHECK(o[3].ev.hz == 400.0f);
    CHECK(o[3].ev.harmonic == 8.0f);
    CHECK(o[3].ev.ratio == 2.0f);
  }
}

TEST_CASE("arp (AP.3): Harmonic Walk — over / under, orders, two anchors merged by Hz (duplicates once)") {
  auto walk = [](e::ArpParams p, std::vector<Ev> in, int count) {
    e::ArpCore arp;
    std::vector<e::NoteEvent> r;
    for (const Ev& x : ons(Harness{}.run(arp, p, std::move(in), {}, kSixteenth * count, 64))) r.push_back(x.ev);
    return r;
  };
  auto hzOf = [](const std::vector<e::NoteEvent>& v) {
    std::vector<float> r;
    for (const auto& x : v) r.push_back(x.hz);
    return r;
  };
  e::ArpParams p = freeParams(e::ArpStyle::HarmonicWalk);
  p.walkRange = 4;
  SUBCASE("one anchor (h 4 of 50 Hz): an overtone run, Up / Down / UpDown") {
    const std::vector<Ev> in = {{0, jiOn(1, 200.0f, 4.0f)}};
    const auto up = walk(p, in, 6);
    CHECK(hzOf(up) == std::vector<float>{200, 250, 300, 350, 400, 200});
    for (size_t i = 0; i < 5; ++i) {
      CHECK(up[i].harmonic == static_cast<float>(4 + i));
      CHECK(up[i].ratio == static_cast<float>((4.0 + i) / 4.0));
    }
    p.walkOrder = e::ArpWalkOrder::Down;
    CHECK(hzOf(walk(p, in, 5)) == std::vector<float>{400, 350, 300, 250, 200});
    p.walkOrder = e::ArpWalkOrder::UpDown;
    CHECK(hzOf(walk(p, in, 9)) == std::vector<float>{200, 250, 300, 350, 400, 350, 300, 250, 200});
  }
  SUBCASE("Under: the undertones 1/4 … 1/8 of the note, by Hz") {
    p.walkUnder = true;
    const auto o = walk(p, {{0, jiOn(1, 200.0f, 4.0f)}}, 5);
    CHECK(hzOf(o) == std::vector<float>{100.0f, static_cast<float>(200.0 * 4 / 7), static_cast<float>(200.0 * 4 / 6),
                                         160.0f, 200.0f});
    CHECK(o[0].ratio == 0.5f);
    CHECK(o[0].harmonic == 0.0f);
    CHECK(o[4].harmonic == 4.0f);  // the note itself
  }
  SUBCASE("two anchors (h 4 and h 6 over 50 Hz, Range 3): one merged run, 300 / 350 once") {
    p.walkRange = 3;
    const auto o = walk(p, {{0, jiOn(1, 300.0f, 6.0f)}, {0, jiOn(2, 200.0f, 4.0f)}}, 7);
    CHECK(hzOf(o) == std::vector<float>{200, 250, 300, 350, 400, 450, 200});
    for (size_t i = 0; i < 6; ++i) CHECK(o[i].harmonic == static_cast<float>(4 + i));
  }
  SUBCASE("two unrelated notes: the union interleaves by Hz; Random draws from it deterministically") {
    p.walkRange = 2;
    const std::vector<Ev> in = {{0, jiOn(1, 200.0f, 2.0f)}, {0, on(2, 330.0f)}};  // note 2: Base 8
    const float h9 = static_cast<float>(330.0 * 9 / 8), h10 = static_cast<float>(330.0 * 10 / 8);
    CHECK(hzOf(walk(p, in, 6)) == std::vector<float>{200, 300, 330, h9, 400, h10});
    p.walkOrder = e::ArpWalkOrder::Random;
    p.seed = 3;
    const auto a = hzOf(walk(p, in, 24)), b = hzOf(walk(p, in, 24));
    CHECK(a == b);
    const std::set<float> allowed{200.0f, 300.0f, 330.0f, h9, h10, 400.0f};
    std::set<float> seen;
    for (float x : a) {
      CHECK(allowed.count(x) == 1);
      seen.insert(x);
    }
    CHECK(seen.size() >= 4);
  }
  SUBCASE("Steps / Distance shift the walk in the unit (Harmonics: + 1 per copy)") {
    p.walkRange = 1;
    p.unit = e::ArpUnit::Harmonics;
    p.steps = 2;
    const auto o = walk(p, {{0, jiOn(1, 200.0f, 4.0f)}}, 4);
    CHECK(hzOf(o) == std::vector<float>{200, 250, 250, 300});
    CHECK(o[3].harmonic == 6.0f);
  }
}

TEST_CASE("arp (AP.3): Into / Out glides — sources, targets, time, shape on the Pitch events") {
  const std::vector<Ev> in = {{0, on(1, 200.0f)}, {0, on(2, 300.0f)}};
  e::ArpParams p = freeParams(e::ArpStyle::Up);  // steps of 6000, gate 3000
  p.into.curve = p.out.curve = 0.5f;  // (shape Curve: an exponential bend)
  SUBCASE("Into Step: from the previous step's note; none before the first step") {
    p.into.step = true;
    p.into.time = p.out.time = 0.16f;  // 480 of the 3000-frame note
    e::ArpCore arp;
    const auto all = Harness{}.run(arp, p, in, {}, kSixteenth * 3, 64);
    const auto o = ons(all);
    REQUIRE(o.size() == 3);
    CHECK(o[0].ev.hz == 200.0f);
    CHECK(pitchesOf(all, o[0].ev.noteId).empty());
    CHECK(o[1].ev.hz == 200.0f);  // starts at the previous step's pitch
    const auto g1 = pitchesOf(all, o[1].ev.noteId);
    REQUIRE(g1.size() == 1);
    CHECK(g1[0].t == o[1].t);
    CHECK(g1[0].ev.hz == 300.0f);
    CHECK(g1[0].ev.rampFrames == 480);
    CHECK(g1[0].ev.rampShape == static_cast<uint8_t>(e::RampShape::Exponential));
    CHECK(static_cast<int8_t>(g1[0].ev.rampBend) == 64);
    CHECK(o[2].ev.hz == 300.0f);
    CHECK(arp.stats().glides == 2);
  }
  // AR.2 (rewritten: there is no mode and no unit offset — an offset is cents, 0 is off).
  SUBCASE("Into offset in cents; Time a fraction of the note; an offset under half a cent is off") {
    p.into.offset = -100.0f;
    p.into.time = p.out.time = 0.5f;  // 1500 of the 3000-frame note
    e::ArpCore arp;
    const auto all = Harness{}.run(arp, p, {{0, on(1, 200.0f)}}, {}, kSixteenth, 64);
    const auto o = ons(all);
    REQUIRE(o.size() == 1);
    CHECK(o[0].ev.hz == static_cast<float>(200.0 * std::exp2(-100.0 / 1200.0)));
    const auto g = pitchesOf(all, o[0].ev.noteId);
    REQUIRE(g.size() == 1);
    CHECK(g[0].ev.hz == 200.0f);
    CHECK(g[0].ev.rampFrames == 1500);
    p.into.offset = 0.3f;
    CHECK_FALSE(e::sanitized(p).into.on());
    e::ArpCore arp2;
    const auto all2 = Harness{}.run(arp2, p, {{0, on(1, 200.0f)}}, {}, kSixteenth, 64);
    const auto o2 = ons(all2);
    REQUIRE(o2.size() == 1);
    CHECK(o2[0].ev.hz == 200.0f);
    CHECK(pitchesOf(all2, o2[0].ev.noteId).empty());
    CHECK(arp2.stats().glides == 0);
  }
  SUBCASE("Out Step: over the last Time of the gate toward the next step; an offset (9/8 in cents)") {
    p.out.step = true;
    p.into.time = p.out.time = 0.32f;  // 960 frames
    e::ArpCore arp;
    const auto all = Harness{}.run(arp, p, in, {}, kSixteenth * 2, 64);
    const auto o = ons(all);
    REQUIRE(o.size() == 2);
    const auto g = pitchesOf(all, o[0].ev.noteId);
    REQUIRE(g.size() == 1);
    CHECK(g[0].t == 3000 - 960);
    CHECK(g[0].ev.hz == 300.0f);
    CHECK(g[0].ev.rampFrames == 960);
    p.out.step = false;
    p.out.offset = static_cast<float>(1200.0 * std::log2(9.0 / 8.0));  // what the panel snaps h 8 -> 9 to
    e::ArpCore arp2;
    const auto all2 = Harness{}.run(arp2, p, {{0, on(1, 200.0f)}}, {}, kSixteenth, 64);
    const auto g2 = pitchesOf(all2, ons(all2)[0].ev.noteId);
    REQUIRE(g2.size() == 1);
    CHECK(g2[0].ev.hz == doctest::Approx(225.0).epsilon(1e-6));
  }
  SUBCASE("Out Step with a gate > 100 %: the next step already sounds or is still ahead") {
    p.out.step = true;
    p.gate = 1.5f;  // 9000 frames
    p.into.time = p.out.time = 1.0f;  // the whole note
    e::ArpCore arp;
    const auto all = Harness{}.run(arp, p, in, {}, kSixteenth * 3, 64);
    const auto o = ons(all);
    REQUIRE(o.size() >= 2);
    const auto g0 = pitchesOf(all, o[0].ev.noteId);
    REQUIRE(g0.size() == 1);  // Out only, the whole note
    CHECK(g0[0].t == 0);
    CHECK(g0[0].ev.rampFrames == 9000);
    CHECK(g0[0].ev.hz == 300.0f);  // the step at 6000 (still ahead at frame 0)
    // A short Out (500) starts after the next step has begun (gate 150 %): it aims at that sounding step.
    p.out.time = 500.0f / 9000.0f;
    e::ArpCore arp2;
    const auto all2 = Harness{}.run(arp2, p, in, {}, kSixteenth * 3, 64);
    const auto o2 = ons(all2);
    REQUIRE(o2.size() >= 3);
    const auto h1 = pitchesOf(all2, o2[1].ev.noteId);
    REQUIRE(h1.size() == 1);
    CHECK(h1[0].t == 6000 + 9000 - 500);
    CHECK(h1[0].ev.hz == 200.0f);
  }
  SUBCASE("the source's bend carries over on top of the Into glide") {
    p.into.offset = 100.0f;
    p.into.time = p.out.time = 0.16f;  // 480 of the 3000-frame note
    e::ArpCore arp;
    // the held note bends +1200 cents over 960 frames from frame 0
    const auto all = Harness{}.run(arp, p, {{0, on(1, 200.0f)}, {0, e::notePitch(0, 1, 400.0f, 960)}}, {}, 6000, 64);
    const auto o = ons(all);
    REQUIRE(o.size() == 1);
    const auto g = pitchesOf(all, o[0].ev.noteId);
    REQUIRE(g.size() == 2);
    CHECK(g[0].t == 0);
    CHECK(g[0].ev.rampFrames == 480);
    CHECK(g[0].ev.hz == doctest::Approx(200.0 * std::exp2(0.5)).epsilon(1e-6));  // the bend at the glide's end
    CHECK(g[1].t == 480);  // then the rest of the bend
    CHECK(g[1].ev.rampFrames == 480);
    CHECK(g[1].ev.hz == 400.0f);
    CHECK(g[1].ev.rampShape == 0);
  }
}

TEST_CASE("arp (AP.3): glides and Harmonic Walk give the same events at block sizes 32 / 64 / 512 / 37; RtScope") {
  e::ArpParams p;
  p.style = e::ArpStyle::HarmonicWalk;
  p.rate = {true, 1, 16, 3, 2, 100.0f};
  p.walkRange = 5;
  p.walkOrder = e::ArpWalkOrder::UpDown;
  p.into.step = true;
  p.out.offset = -50.0f;
  p.into.time = p.out.time = 0.3f;
  p.into.shape = p.out.shape = e::ArpGlideShape::Spring;
  p.into.curve = p.out.curve = 0.5f;
  p.gate = 0.9f;
  Transport tr;
  tr.playing = true;
  const std::vector<Ev> in = {{1000, jiOn(1, 200.0f, 4.0f)}, {9000, jiOn(2, 300.0f, 6.0f)},
                              {30000, e::notePitch(0, 1, 210.0f, 3000)}, {50000, off(1)}, {70000, off(2)}};
  std::vector<std::vector<Ev>> runs;
  for (uint32_t block : {32u, 64u, 512u, 37u}) {
    e::ArpCore arp;
    Harness h;
    const std::vector<Ev> r = h.run(arp, p, in, tr, 80000, block);
    CHECK(h.rtViolations == 0);
    CHECK(arp.stats().glides > 10);
    runs.push_back(r);
  }
  REQUIRE(runs[0].size() > 30);
  for (size_t i = 1; i < runs.size(); ++i) {
    REQUIRE(runs[i].size() == runs[0].size());
    for (size_t j = 0; j < runs[0].size(); ++j) {
      CHECK(runs[i][j].t == runs[0][j].t);
      CHECK(runs[i][j].ev.kind == runs[0][j].ev.kind);
      CHECK(runs[i][j].ev.noteId == runs[0][j].ev.noteId);
      CHECK(runs[i][j].ev.hz == runs[0][j].ev.hz);
      CHECK(runs[i][j].ev.rampFrames == runs[0][j].ev.rampFrames);
    }
  }
}

TEST_CASE("arp (AP.3): the new params through setArpParam / sanitized") {
  e::ArpParams p;
  e::setArpParam(p, e::ArpParamId::Unit, 2.0f);
  CHECK(p.unit == e::ArpUnit::Harmonics);
  e::setArpParam(p, e::ArpParamId::Style, 18.0f);
  CHECK(p.style == e::ArpStyle::HarmonicWalk);
  e::setArpParam(p, e::ArpParamId::HarmonicBase, 12.4f);
  CHECK(p.harmonicBase == 12);
  e::setArpParam(p, e::ArpParamId::WalkRange, 99.0f);
  CHECK(p.walkRange == e::kArpMaxWalkRange);
  e::setArpParam(p, e::ArpParamId::WalkUnder, 1.0f);
  CHECK(p.walkUnder);
  e::setArpParam(p, e::ArpParamId::WalkOrder, 3.0f);
  CHECK(p.walkOrder == e::ArpWalkOrder::Random);
  // AR.2: a side is an offset (cents; 0: off) | the neighbouring step, a time, a curve and a shape — no mode.
  CHECK_FALSE(p.into.on());
  CHECK_FALSE(p.out.on());
  e::setArpParam(p, e::ArpParamId::IntoPrev, 1.0f);
  CHECK(p.into.step);
  CHECK(p.into.on());
  e::setArpParam(p, e::ArpParamId::IntoOffset, -300.0f);
  e::setArpParam(p, e::ArpParamId::OutOffset, 9000.0f);
  CHECK(p.into.offset == -300.0f);
  CHECK(p.out.on());
  CHECK_FALSE(p.out.step);
  e::setArpParam(p, e::ArpParamId::OutNext, 1.0f);
  CHECK(p.out.step);
  // AP.11: the time one param, a fraction 0 … 2 of the note.
  e::setArpParam(p, e::ArpParamId::IntoTime, 9.0f);
  e::setArpParam(p, e::ArpParamId::OutTime, 1.5f);
  e::setArpParam(p, e::ArpParamId::IntoShape, 2.0f);
  e::setArpParam(p, e::ArpParamId::OutShape, 1.0f);
  e::setArpParam(p, e::ArpParamId::IntoCurve, 5.0f);
  e::setArpParam(p, e::ArpParamId::OutCurve, -0.25f);
  CHECK(p.into.shape == e::ArpGlideShape::Spring);
  CHECK(p.out.shape == e::ArpGlideShape::S);
  const e::ArpParams s = e::sanitized(p);
  CHECK(s.out.offset == e::kArpMaxGlideOffset);
  CHECK(s.into.time == e::kArpMaxGlideTime);
  CHECK(s.out.time == 1.5f);
  CHECK(e::sanitized([] { e::ArpParams q; q.into.time = -1.0f; return q; }()).into.time == 0.0f);
  CHECK(s.into.curve == 1.0f);
  CHECK(s.out.curve == -0.25f);
  // The voice's ramp of a side: Curve 0 is a line, ± an exponential; S; Spring.
  CHECK(e::ArpGlideSide{}.ramp() == e::RampShape::Linear);
  CHECK(e::ArpGlideSide{0.0f, false, 0.25f, -0.3f, e::ArpGlideShape::Curve}.ramp() == e::RampShape::Exponential);
  CHECK(s.into.ramp() == e::RampShape::Overshoot);
  CHECK(s.out.ramp() == e::RampShape::SCurve);
  CHECK(std::string(e::arpParamInfo(e::ArpParamId::IntoShape).choices) == "Curve|S|Spring");
  CHECK(e::arpParamInfo(e::ArpParamId::IntoOffset).def == 0.0f);  // both sides off by default
  // The knob's taper (ArpParamInfo::taper, the ArpStage mapping): 100 % at log2(2.5) / 2 ≈ 66 % of the travel, the
  // ends exact.
  for (const e::ArpParamInfo& r : e::kArpParamInfo)
    if (r.id == e::ArpParamId::IntoTime) {
      CHECK(r.unit == "%");
      CHECK(r.def == 0.25f);
      CHECK(e::arpTaper(r.taper, 0.0f) == 0.0f);
      CHECK(e::arpTaper(r.taper, 1.0f) == 1.0f);
      CHECK(r.min + (r.max - r.min) * e::arpTaper(r.taper, static_cast<float>(std::log2(2.5) / 2.0)) == doctest::Approx(1.0).epsilon(1e-5));
      CHECK(e::arpUntaper(r.taper, e::arpTaper(r.taper, 0.3f)) == doctest::Approx(0.3).epsilon(1e-6));
    }
}

TEST_CASE("arp (AP.10): Into and Out each with their own time, shape and bend on the Pitch events") {
  const std::vector<Ev> in = {{0, on(1, 200.0f)}, {0, on(2, 300.0f)}};
  e::ArpParams p = freeParams(e::ArpStyle::Up);  // steps of 6000, gate 3000
  SUBCASE("Into only: its shape / bend / time; Out's own settings change nothing while Out is off") {
    p.into = {0.0f, true, 0.16f, 0.0f, e::ArpGlideShape::S};  // 480 frames
    p.out.shape = e::ArpGlideShape::Spring;
    p.out.time = 0.5f;
    e::ArpCore arp;
    const auto all = Harness{}.run(arp, p, in, {}, kSixteenth * 2, 64);
    const auto o = ons(all);
    REQUIRE(o.size() == 2);
    CHECK(pitchesOf(all, o[0].ev.noteId).empty());
    const auto g = pitchesOf(all, o[1].ev.noteId);
    REQUIRE(g.size() == 1);
    CHECK(g[0].t == 6000);
    CHECK(g[0].ev.rampFrames == 480);
    CHECK(g[0].ev.rampShape == static_cast<uint8_t>(e::RampShape::SCurve));
  }
  SUBCASE("Out only: its shape / bend / synced time; Into's own settings change nothing while Into is off") {
    p.out = {0.0f, true, 0.2f, -0.6f, e::ArpGlideShape::Spring};  // 600 of the 3000
    p.into.curve = 0.5f;
    p.into.time = 0.9f;
    e::ArpCore arp;
    const auto all = Harness{}.run(arp, p, in, {}, kSixteenth * 2, 64);
    const auto o = ons(all);
    REQUIRE(o.size() == 2);
    CHECK(o[1].ev.hz == 300.0f);  // no Into
    const auto g = pitchesOf(all, o[0].ev.noteId);
    REQUIRE(g.size() == 1);
    CHECK(g[0].t == 3000 - 600);
    CHECK(g[0].ev.hz == 300.0f);
    CHECK(g[0].ev.rampFrames == 600);
    CHECK(g[0].ev.rampShape == static_cast<uint8_t>(e::RampShape::Overshoot));
    CHECK(static_cast<int8_t>(g[0].ev.rampBend) == -76);
  }
  SUBCASE("both on one step, asymmetric, apart: one shaped ramp each") {
    p.into = {0.0f, true, 0.16f, 0.5f, e::ArpGlideShape::Curve};       // 480
    p.out = {-100.0f, false, 0.08f, 1.0f, e::ArpGlideShape::Spring};  // 240
    e::ArpCore arp;
    const auto all = Harness{}.run(arp, p, in, {}, kSixteenth * 2, 64);
    const auto o = ons(all);
    REQUIRE(o.size() == 2);
    const auto g = pitchesOf(all, o[1].ev.noteId);
    REQUIRE(g.size() == 2);
    CHECK(g[0].t == 6000);
    CHECK(g[0].ev.rampFrames == 480);
    CHECK(g[0].ev.rampShape == static_cast<uint8_t>(e::RampShape::Exponential));
    CHECK(static_cast<int8_t>(g[0].ev.rampBend) == 64);
    CHECK(g[1].t == 9000 - 240);
    CHECK(g[1].ev.rampFrames == 240);
    CHECK(g[1].ev.hz == static_cast<float>(300.0 * std::exp2(-100.0 / 1200.0)));
    CHECK(g[1].ev.rampShape == static_cast<uint8_t>(e::RampShape::Overshoot));
    CHECK(static_cast<int8_t>(g[1].ev.rampBend) == 127);
  }
}

// AP.11: the events of the additive curves (the sampled pitches: test_voices "AP.11").
TEST_CASE("arp (AP.11): Into / Out Time a % of the note; overlapping sides add; > 100 % passes the note's ends") {
  const std::vector<Ev> in = {{0, on(1, 200.0f)}, {0, on(2, 300.0f)}};
  e::ArpParams p = freeParams(e::ArpStyle::Up);  // steps of 6000, gate 3000
  auto cents = [](double hz, double ref) { return 1200.0 * std::log2(hz / ref); };
  SUBCASE("both Linear, overlapping: linear pieces (exact), the sum of the two curves at each piece's end") {
    p.into = {-300.0f, false, 0.6f, 0.0f, e::ArpGlideShape::Curve};  // 1800: −300 ¢ → 0 over [0, 1800)
    p.out = {200.0f, false, 0.6f, 0.0f, e::ArpGlideShape::Curve};    // 1800: 0 → +200 ¢ over [1200, 3000)
    e::ArpCore arp;
    const auto all = Harness{}.run(arp, p, {{0, on(1, 200.0f)}}, {}, kSixteenth, 64);
    const auto o = ons(all);
    REQUIRE(o.size() == 1);
    CHECK(cents(o[0].ev.hz, 200.0) == doctest::Approx(-300.0).epsilon(1e-6));
    const auto g = pitchesOf(all, o[0].ev.noteId);
    REQUIRE(g.size() == 3);  // Into from the onset, the overlap to the Into's end, the Out's rest
    CHECK(g[0].t == 0);
    CHECK(g[0].ev.rampFrames == 1800);
    CHECK(g[1].t == 1200);
    CHECK(g[1].ev.rampFrames == 600);
    CHECK(cents(g[1].ev.hz, 200.0) == doctest::Approx(200.0 * 600.0 / 1800.0).epsilon(1e-5));  // In 0 + Out 66.7
    CHECK(g[2].t == 1800);
    CHECK(g[2].ev.rampFrames == 1200);
    CHECK(cents(g[2].ev.hz, 200.0) == doctest::Approx(200.0).epsilon(1e-5));
    CHECK(arp.stats().glides == 2);
  }
  SUBCASE("shaped and overlapping: linear-in-cents pieces on the summed curve, 32 per the shorter one") {
    p.into = {-300.0f, false, 0.6f, 0.0f, e::ArpGlideShape::S};
    p.out = {200.0f, false, 0.6f, 0.5f, e::ArpGlideShape::Curve};
    e::ArpCore arp;
    const auto all = Harness{}.run(arp, p, {{0, on(1, 200.0f)}}, {}, kSixteenth, 64);
    const auto g = pitchesOf(all, ons(all).at(0).ev.noteId);
    REQUIRE(g.size() > 10);
    CHECK(g[0].ev.rampShape == static_cast<uint8_t>(e::RampShape::SCurve));  // the Into alone until 1200
    CHECK(g[1].t == 1200);
    const int8_t kb = e::rampBendOf(0.5f);
    for (size_t i = 1; i < g.size(); ++i) {
      CAPTURE(i);
      const int64_t end = g[i].t + g[i].ev.rampFrames;
      CHECK(g[i].ev.rampFrames >= 24);  // ≥ half a 1-ms piece (a last one is ≥ half a piece)
      CHECK(end <= 3000);
      const double u = std::min(1.0, static_cast<double>(end) / 1800.0), v = static_cast<double>(end - 1200) / 1800.0;
      const double want = (end < 1800 ? -300.0 * (1.0 - e::rampShapeAt(e::RampShape::SCurve, 0, u)) : 0.0) +
                          200.0 * e::rampShapeAt(e::RampShape::Exponential, kb, v);
      if (g[i].ev.rampShape == 0) CHECK(cents(g[i].ev.hz, 200.0) == doctest::Approx(want).epsilon(1e-4));
    }
    CHECK(g.back().t + g.back().ev.rampFrames == 3000);  // the last: the Out alone to its target
    CHECK(cents(g.back().ev.hz, 200.0) == doctest::Approx(200.0).epsilon(1e-5));
  }
  SUBCASE("Into 150 %: one ramp past the note's end (never arrives); Out 150 %: begun before the onset") {
    p.into = {-300.0f, false, 1.5f, 0.0f, e::ArpGlideShape::Curve};
    e::ArpCore arp;
    const auto all = Harness{}.run(arp, p, {{0, on(1, 200.0f)}}, {}, kSixteenth, 64);
    const auto g = pitchesOf(all, ons(all).at(0).ev.noteId);
    REQUIRE(g.size() == 1);
    CHECK(g[0].ev.rampFrames == 4500);
    p.into = {};
    p.out = {0.0f, true, 1.5f, 0.0f, e::ArpGlideShape::Curve};  // 4500 ending at 3000: from −1500
    e::ArpCore arp2;
    const auto all2 = Harness{}.run(arp2, p, in, {}, kSixteenth, 64);
    const auto o2 = ons(all2);
    REQUIRE(o2.size() == 1);
    const auto g2 = pitchesOf(all2, o2[0].ev.noteId);
    REQUIRE(g2.size() == 2);  // at the onset: a step to 1/3 of the way, then the rest to the next step
    CHECK(g2[0].t == 0);
    CHECK(g2[0].ev.rampFrames == 0);
    CHECK(cents(g2[0].ev.hz, 200.0) == doctest::Approx(cents(300.0, 200.0) / 3.0).epsilon(1e-5));
    CHECK(g2[1].t == 0);
    CHECK(g2[1].ev.rampFrames == 3000);
    CHECK(g2[1].ev.hz == 300.0f);
  }
}

// ---- AP.4: Pulse mode (per-note clocks, phase lock, layers) ------------------------------------------------------

namespace {

e::ArpParams pulseParams(int32_t den = 4) {
  e::ArpParams p;
  p.pulse = true;
  p.pulseRef = 0;  // the lowest held (these tests were written for it; AP.8a made a fixed 8 the default)
  p.rate = {true, 1, den, 1, 1, 100.0f};
  p.gate = 0.5f;
  return p;
}

// Note-on times per Hz.
std::map<float, std::vector<int64_t>> onsetsByHz(const std::vector<Ev>& v) {
  std::map<float, std::vector<int64_t>> r;
  for (const Ev& x : v)
    if (x.ev.kind == e::NoteEventKind::NoteOn) r[x.ev.hz].push_back(x.t);
  return r;
}

// Note-off time per generated id.
std::map<uint32_t, int64_t> offsById(const std::vector<Ev>& v) {
  std::map<uint32_t, int64_t> r;
  for (const Ev& x : v)
    if (x.ev.kind == e::NoteEventKind::NoteOff) r[x.ev.noteId] = x.t;
  return r;
}

int64_t roundDiv(int64_t num, int64_t den) { return (2 * num + den) / (2 * den); }  // num, den > 0: half up

std::vector<std::string> keyOf(const std::vector<Ev>& v) {
  std::vector<std::string> r;
  for (const Ev& x : v) {
    char buf[200];
    std::snprintf(buf, sizeof buf, "%lld %d %u %.6f %.6f %.6f %.6f %u", static_cast<long long>(x.t),
                  static_cast<int>(x.ev.kind), x.ev.noteId, x.ev.hz, x.ev.velocity, x.ev.gain, x.ev.ratio,
                  x.ev.rampFrames);
    r.emplace_back(buf);
  }
  return r;
}

const int64_t kHs[4] = {8, 9, 10, 12};

std::vector<Ev> chord8_9_10_12(int64_t t = 0) {
  std::vector<Ev> in;
  for (int64_t h : kHs)
    in.push_back({t, jiOn(static_cast<uint32_t>(h), 100.0f * static_cast<float>(h), static_cast<float>(h))});
  return in;
}

}  // namespace

TEST_CASE("arp (AP.4): pulse 8:9:10:12 onsets exact in samples over 4 bars, at block sizes 32 / 64 / 512 / 37") {
  // Base 1/4 (24000 samples at 120 BPM), h_ref = 8 (the lowest): note h pulses every 24000 * 8 / h; the cycle is
  // 8 quarters (192000) where every clock restarts.
  const e::ArpParams p = pulseParams(4);
  Transport tr;
  tr.playing = true;
  const int64_t total = 4 * 96000;
  std::vector<std::string> ref;
  for (uint32_t block : {64u, 32u, 512u, 37u}) {
    CAPTURE(block);
    e::ArpCore arp;
    Harness h;
    const auto out = h.run(arp, p, chord8_9_10_12(), tr, total, block);
    CHECK(h.rtViolations == 0);
    const auto by = onsetsByHz(out);
    const auto offs = offsById(out);
    for (int64_t hh : kHs) {
      CAPTURE(hh);
      std::vector<int64_t> want, wantOff;
      for (int64_t c = 0; c < 2; ++c)
        for (int64_t j = 0; j < hh; ++j) {
          want.push_back(roundDiv(c * 192000 * hh + j * 192000, hh));
          wantOff.push_back(roundDiv(c * 192000 * hh + j * 192000 + 96000, hh));  // gate 50 % of 192000 / h
        }
      const auto it = by.find(100.0f * static_cast<float>(hh));
      REQUIRE(it != by.end());
      CHECK(it->second == want);
      std::vector<int64_t> gotOff;
      for (const Ev& x : out)
        if (x.ev.kind == e::NoteEventKind::NoteOn && x.ev.hz == 100.0f * static_cast<float>(hh)) {
          const auto o = offs.find(x.ev.noteId);
          gotOff.push_back(o == offs.end() ? -1 : o->second);
        }
      gotOff.pop_back();  // the last one may end after the run
      wantOff.pop_back();
      CHECK(gotOff == wantOff);
    }
    CHECK(arp.stats().pulses == 2 * (8 + 9 + 10 + 12));
    const auto k = keyOf(out);
    if (ref.empty()) ref = k;
    else CHECK(k == ref);
  }
}

TEST_CASE("arp (AP.4): pulse 8:9:10:12 exact across a tempo change (120 -> 90 at bar 2), any block size") {
  pq::TempoPoint pts[2] = {{0, 120.0}, {4 * pq::kPpq, 90.0}};
  auto tm = pq::TempoMap::create(pts);
  REQUIRE(tm.ok());
  const e::ArpParams p = pulseParams(4);
  Transport tr;
  tr.playing = true;
  const int64_t total = 96000 + 3 * 128000;  // 4 bars: 1 at 120, 3 at 90
  constexpr int64_t P = pq::kPpq;
  std::vector<std::string> ref;
  for (uint32_t block : {64u, 32u, 512u, 37u}) {
    CAPTURE(block);
    e::ArpCore arp;
    Harness h;
    h.tempo = tm.value();
    const auto out = h.run(arp, p, chord8_9_10_12(), tr, total, block);
    const auto by = onsetsByHz(out);
    for (int64_t hh : kHs) {
      CAPTURE(hh);
      // onset ticks * h = c * 8P * h + j * 8P; 24000 samples per P before tick 4P, 32000 per P after
      std::vector<int64_t> want;
      for (int64_t c = 0; c < 2; ++c)
        for (int64_t j = 0; j < hh; ++j) {
          const int64_t th = c * 8 * P * hh + j * 8 * P;
          const int64_t den = hh * P;
          want.push_back(th <= 4 * P * hh ? roundDiv(th * 24000, den)
                                          : roundDiv(96000 * den + (th - 4 * P * hh) * 32000, den));
        }
      const auto it = by.find(100.0f * static_cast<float>(hh));
      REQUIRE(it != by.end());
      CHECK(it->second == want);
    }
    const auto k = keyOf(out);
    if (ref.empty()) ref = k;
    else CHECK(k == ref);
  }
}

TEST_CASE("arp (AP.4): pulse with a stopped transport: the free clock from the note-on, the same ratios") {
  e::ArpCore arp;
  Harness h;
  const auto by = onsetsByHz(h.run(arp, pulseParams(4), chord8_9_10_12(1000), {}, 200000, 64));
  for (int64_t hh : kHs) {
    CAPTURE(hh);
    std::vector<int64_t> want;
    for (int64_t j = 0; j < hh; ++j) want.push_back(1000 + roundDiv(j * 192000, hh));
    want.push_back(1000 + 192000);
    CHECK(by.at(100.0f * static_cast<float>(hh)) == want);
  }
}

TEST_CASE("arp (AP.4): phase lock every Base * h_ref or every n bars") {
  // Order: three plain notes are h = 1, 2, 3 (by Hz); h_ref fixed 3: periods 72000, 36000, 24000 (Base 1/4).
  e::ArpParams p = pulseParams(4);
  p.pulseSource = e::ArpPulseSource::Order;
  p.pulseRef = 3;
  Transport tr;
  tr.playing = true;
  std::vector<Ev> in = {{0, on(1, 200.0f)}, {0, on(2, 300.0f)}, {0, on(3, 400.0f)}};
  SUBCASE("Cycle Base: 3 quarters, every clock fills it exactly") {
    e::ArpCore arp;
    const auto by = onsetsByHz(Harness{}.run(arp, p, in, tr, 200000, 64));
    CHECK(by.at(200.0f) == std::vector<int64_t>{0, 72000, 144000});
    CHECK(by.at(300.0f) == std::vector<int64_t>{0, 36000, 72000, 108000, 144000, 180000});
    CHECK(by.at(400.0f).size() == 9);  // every 24000
  }
  SUBCASE("Cycle Bars 1: all restart at every bar (96000); the last pulse before it is short") {
    p.pulseCycle = e::ArpPulseCycle::Bars;
    p.pulseBars = 1;
    e::ArpCore arp;
    const auto by = onsetsByHz(Harness{}.run(arp, p, in, tr, 200000, 64));
    CHECK(by.at(200.0f) == std::vector<int64_t>{0, 72000, 96000, 168000, 192000});
    CHECK(by.at(300.0f) == std::vector<int64_t>{0, 36000, 72000, 96000, 132000, 168000, 192000});
    CHECK(by.at(400.0f) == std::vector<int64_t>{0, 24000, 48000, 72000, 96000, 120000, 144000, 168000, 192000});
  }
  SUBCASE("Cycle Bars 2, stopped (the free clock from the note-on at 500)") {
    p.pulseCycle = e::ArpPulseCycle::Bars;
    p.pulseBars = 2;
    e::ArpCore arp;
    std::vector<Ev> late = in;
    for (Ev& x : late) x.t = 500;
    const auto by = onsetsByHz(Harness{}.run(arp, p, late, {}, 200500, 64));
    CHECK(by.at(200.0f) == std::vector<int64_t>{500, 72500, 144500, 192500});
  }
}

TEST_CASE("arp (AP.4): h from the harmonic, the proportion or the order") {
  Transport tr;
  tr.playing = true;
  auto periods = [&](const e::ArpParams& p, const std::vector<Ev>& in) {
    e::ArpCore arp;
    std::map<float, int64_t> r;
    for (const auto& [hz, ts] : onsetsByHz(Harness{}.run(arp, p, in, tr, 100000, 64))) {
      REQUIRE(ts.size() >= 2);
      r[hz] = ts[1] - ts[0];
    }
    return r;
  };
  e::ArpParams p = pulseParams(4);
  SUBCASE("harmonic 4:5:6 (JI notes): Base * 4 / h") {
    const auto r = periods(p, {{0, jiOn(1, 400.0f, 4.0f)}, {0, jiOn(2, 500.0f, 5.0f)}, {0, jiOn(3, 600.0f, 6.0f)}});
    CHECK(r.at(400.0f) == 24000);
    CHECK(r.at(500.0f) == 19200);
    CHECK(r.at(600.0f) == 16000);
  }
  const std::vector<Ev> plain = {{0, on(1, 300.0f)}, {0, on(2, 200.0f)}, {0, on(3, 250.0f)}};
  SUBCASE("proportion 4:5:6 by Hz rank (also the Harmonic source's fallback for notes without a group)") {
    p.proportionLen = 3;
    p.proportion[0] = 4;
    p.proportion[1] = 5;
    p.proportion[2] = 6;
    for (auto src : {e::ArpPulseSource::Proportion, e::ArpPulseSource::Harmonic}) {
      p.pulseSource = src;
      const auto r = periods(p, plain);
      CHECK(r.at(200.0f) == 24000);
      CHECK(r.at(250.0f) == 19200);
      CHECK(r.at(300.0f) == 16000);
    }
    // beyond its length the proportion repeats doubled: 2:3, then 4
    p.proportionLen = 2;
    p.proportion[0] = 2;
    p.proportion[1] = 3;
    const auto r = periods(p, plain);
    CHECK(r.at(200.0f) == 24000);
    CHECK(r.at(250.0f) == 16000);
    CHECK(r.at(300.0f) == 12000);
  }
  SUBCASE("order: 1, 2, 3 (also Proportion with no proportion)") {
    for (auto src : {e::ArpPulseSource::Order, e::ArpPulseSource::Proportion}) {
      p.pulseSource = src;
      const auto r = periods(p, plain);
      CHECK(r.at(200.0f) == 24000);
      CHECK(r.at(250.0f) == 12000);
      CHECK(r.at(300.0f) == 8000);
    }
  }
}

TEST_CASE("arp (AP.4): layers at (p/q)^L with exact ratio / harmonic and their own levels; Steps; Into per clock") {
  Transport tr;
  tr.playing = true;
  e::ArpParams p = pulseParams(4);
  p.layers = 2;
  p.ratioP = 3;
  p.ratioQ = 2;
  p.layerLevel = {0.5f, 0.25f, 1.0f};
  e::NoteEvent n = jiOn(1, 400.0f, 8.0f, 2.0f);
  n.gain = 0.8f;
  e::ArpCore arp;
  Harness h;
  const auto out = h.run(arp, p, {{0, n}, {30000, e::noteGain(0, 1, 0.4f)}}, tr, 40000, 64);
  CHECK(h.rtViolations == 0);
  const auto o = ons(out);
  REQUIRE(o.size() == 6);  // pulses at 0 and 24000, three notes each
  for (size_t s = 0; s < 2; ++s) {
    CHECK(o[3 * s].ev.hz == 400.0f);
    CHECK(o[3 * s + 1].ev.hz == 600.0f);
    CHECK(o[3 * s + 2].ev.hz == 900.0f);
    CHECK(o[3 * s].ev.ratio == 2.0f);
    CHECK(o[3 * s + 1].ev.ratio == 3.0f);
    CHECK(o[3 * s + 2].ev.ratio == 4.5f);
    CHECK(o[3 * s].ev.harmonic == 8.0f);
    CHECK(o[3 * s + 1].ev.harmonic == 12.0f);
    CHECK(o[3 * s + 2].ev.harmonic == 18.0f);
    CHECK(o[3 * s].ev.gain == doctest::Approx(0.8f));
    CHECK(o[3 * s + 1].ev.gain == doctest::Approx(0.4f));
    CHECK(o[3 * s + 2].ev.gain == doctest::Approx(0.2f));
    for (size_t i = 0; i < 3; ++i) CHECK(o[3 * s + i].t == static_cast<int64_t>(s) * 24000);
  }
  // the source's gain curve reaches the sounding layers times their levels (the 24000 pulse sounds until 36000)
  std::vector<float> gains;
  for (const Ev& x : out)
    if (x.ev.kind == e::NoteEventKind::Gain && x.t == 30000) gains.push_back(x.ev.gain);
  REQUIRE(gains.size() == 3);
  CHECK(gains[0] == doctest::Approx(0.4f));
  CHECK(gains[1] == doctest::Approx(0.2f));
  CHECK(gains[2] == doctest::Approx(0.1f));

  // Steps 2, Distance 12 (Degrees, 12-EDO): pulse j of the cycle plays copy j mod 2; Into Step glides from the same
  // clock's previous pulse.
  e::ArpParams q = pulseParams(4);
  q.steps = 2;
  q.distance = 12;
  q.into.step = true;
  q.into.time = q.out.time = 0.04f;  // 480 of the 12000-frame gate
  e::ArpCore arp2;
  const auto out2 = Harness{}.run(arp2, q, {{0, jiOn(1, 400.0f, 8.0f)}}, tr, 200000, 64);
  const auto o2 = ons(out2);
  REQUIRE(o2.size() == 9);
  CHECK(o2[0].ev.hz == 400.0f);  // j = 0, no previous pulse: no glide
  CHECK(pitchesOf(out2, o2[0].ev.noteId).empty());
  CHECK(o2[1].ev.hz == 400.0f);  // j = 1 starts at the previous pulse's pitch ...
  const auto g1 = pitchesOf(out2, o2[1].ev.noteId);
  REQUIRE(g1.size() == 1);
  CHECK(g1[0].ev.hz == 800.0f);  // ... and glides to its copy (+12 degrees)
  CHECK(g1[0].ev.rampFrames == 480u);
  CHECK(o2[2].ev.hz == 800.0f);  // j = 2: from 800 to 400
  CHECK(pitchesOf(out2, o2[2].ev.noteId).at(0).ev.hz == 400.0f);
  CHECK(o2[8].t == 192000);  // the cycle restarts: j = 0 (copy 0), from j = 7's 800
  CHECK(o2[8].ev.hz == 800.0f);
  CHECK(pitchesOf(out2, o2[8].ev.noteId).at(0).ev.hz == 400.0f);
}

TEST_CASE("arp (AP.4): notes added / released mid-cycle; the first pulse of a new clock sounds at its note-on") {
  e::ArpParams p = pulseParams(4);
  Transport tr;
  tr.playing = true;
  std::vector<Ev> in = {{0, jiOn(8, 800.0f, 8.0f)},
                        {0, jiOn(12, 1200.0f, 12.0f)},
                        {50000, jiOn(10, 1000.0f, 10.0f)},
                        {100000, off(12)},
                        {150000, off(8)}};
  std::vector<std::string> ref;
  for (uint32_t block : {64u, 37u, 512u}) {
    CAPTURE(block);
    e::ArpCore arp;
    Harness h;
    const auto out = h.run(arp, p, in, tr, 230000, block);
    const auto by = onsetsByHz(out);
    // 8: every 24000 until its release
    CHECK(by.at(800.0f) == std::vector<int64_t>{0, 24000, 48000, 72000, 96000, 120000, 144000});
    // 12: every 16000 until 100000
    CHECK(by.at(1200.0f) == std::vector<int64_t>{0, 16000, 32000, 48000, 64000, 80000, 96000});
    // 10: at its note-on (50000, between its grid onsets 38400 / 57600), ended at 57600; then every 19200; once 8
    // is released h_ref = 10: every 24000 on the new cycle (240000) grid
    const auto& ten = by.at(1000.0f);
    REQUIRE(ten.size() >= 9);
    CHECK(ten[0] == 50000);
    for (size_t i = 1; i <= 5; ++i) CHECK(ten[i] == 57600 + 19200 * static_cast<int64_t>(i - 1));
    CHECK(ten[6] == 168000);
    CHECK(ten[7] == 192000);
    CHECK(ten[8] == 216000);
    const auto offs = offsById(out);
    for (const Ev& x : out)
      if (x.ev.kind == e::NoteEventKind::NoteOn && x.t == 50000) CHECK(offs.at(x.ev.noteId) == 57600);
    const auto k = keyOf(out);
    if (ref.empty()) ref = k;
    else CHECK(k == ref);
  }
  // A whole chord off the grid (7000): every clock sounds at once, then on its own grid.
  e::ArpCore arp;
  const auto by = onsetsByHz(Harness{}.run(arp, p, chord8_9_10_12(7000), tr, 30000, 64));
  CHECK(by.at(800.0f) == std::vector<int64_t>{7000, 24000});
  CHECK(by.at(900.0f) == std::vector<int64_t>{7000, 21333});
  CHECK(by.at(1000.0f) == std::vector<int64_t>{7000, 19200});
  CHECK(by.at(1200.0f) == std::vector<int64_t>{7000, 16000});
}

TEST_CASE("arp (AP.4): pulse with hold; loop wrap and locate re-seek every clock") {
  Transport tr;
  tr.playing = true;
  SUBCASE("hold: released keys keep pulsing; a new chord replaces them") {
    e::ArpParams p = pulseParams(4);
    p.hold = true;
    e::ArpCore arp;
    std::vector<Ev> in = {{0, jiOn(8, 800.0f, 8.0f)}, {0, jiOn(12, 1200.0f, 12.0f)}, {1000, off(8)},
                          {1000, off(12)},           {200000, jiOn(10, 1000.0f, 10.0f)}};
    const auto by = onsetsByHz(Harness{}.run(arp, p, in, tr, 260000, 64));
    CHECK(by.at(800.0f).back() == 192000);
    CHECK(by.at(1200.0f).back() == 192000);
    // 10 alone: h_ref 10, every 24000; its note-on (200000) is off that grid: at once, then 216000
    CHECK(by.at(1000.0f) == std::vector<int64_t>{200000, 216000, 240000});
  }
  SUBCASE("loop [0, 120000): every clock restarts at the wrap; a locate seeks the first onsets after it") {
    tr.loopA = 0;
    tr.loopB = 120000;
    tr.locates = {{200000, 7777}};
    e::ArpCore arp;
    const auto out = Harness{}.run(arp, pulseParams(4), chord8_9_10_12(), tr, 240000, 64);
    const auto by = onsetsByHz(out);
    for (int64_t hh : kHs) {
      CAPTURE(hh);
      const auto& v = by.at(100.0f * static_cast<float>(hh));
      CHECK(std::count(v.begin(), v.end(), 120000) == 1);
      // after the locate (engine 200000 = song 7777): the first onset at or after 7777
      const int64_t first = roundDiv(192000, hh) >= 7777 ? roundDiv(192000, hh) : roundDiv(2 * 192000, hh);
      const auto it = std::find_if(v.begin(), v.end(), [](int64_t t) { return t >= 200000; });
      REQUIRE(it != v.end());
      CHECK(*it == 200000 + first - 7777);
    }
    CHECK(arp.stats().transportJumps == 2);
  }
}

TEST_CASE("arp (AP.4): a 128-note held set pulses with bounded work; RtScope") {
  e::ArpParams p = pulseParams(4);
  Transport tr;
  tr.playing = true;
  std::vector<Ev> in;
  for (uint32_t i = 0; i < 128; ++i)
    in.push_back({0, jiOn(i + 1, 25.0f * static_cast<float>(8 + i), static_cast<float>(8 + i))});
  e::ArpCore arp;
  Harness h;
  const auto out = h.run(arp, p, in, tr, 96000, 64);
  CHECK(h.rtViolations == 0);
  CHECK(arp.heldCount() == 128);
  // h = 8 ... 135, h_ref 8: one bar is half the cycle: ceil(h / 2) pulses each
  uint64_t want = 0;
  for (int64_t hh = 8; hh < 136; ++hh) want += static_cast<uint64_t>((hh + 1) / 2);
  CHECK(arp.stats().pulses == want);
  CHECK(arp.stats().notes == want);
  CHECK(arp.stats().cutSounding == 0);
  CHECK(ons(out).size() == want);
}

TEST_CASE("arp (AP.4): pulse / layer params through setArpParam / sanitized; pulse off by default") {
  e::ArpParams p;
  CHECK_FALSE(p.pulse);
  CHECK(p.layers == 0);
  e::setArpParam(p, e::ArpParamId::Pulse, 1.0f);
  CHECK(p.pulse);
  e::setArpParam(p, e::ArpParamId::PulseSource, 2.0f);
  CHECK(p.pulseSource == e::ArpPulseSource::Order);
  e::setArpParam(p, e::ArpParamId::PulseRef, 5000.0f);
  CHECK(p.pulseRef == e::kArpMaxPulseH);
  e::setArpParam(p, e::ArpParamId::PulseCycle, 1.0f);
  CHECK(p.pulseCycle == e::ArpPulseCycle::Bars);
  e::setArpParam(p, e::ArpParamId::PulseBars, 99.0f);
  CHECK(p.pulseBars == e::kArpMaxPulseBars);
  e::setArpParam(p, e::ArpParamId::ProportionLen, 3.0f);
  e::setArpParam(p, e::ArpParamId::Proportion1, 7.0f);
  e::setArpParam(p, e::ArpParamId::Proportion8, 0.0f);
  CHECK(p.proportionLen == 3);
  CHECK(p.proportion[0] == 7);
  CHECK(p.proportion[7] == 1);
  e::setArpParam(p, e::ArpParamId::Layers, 9.0f);
  CHECK(p.layers == e::kArpMaxLayers);
  e::setArpParam(p, e::ArpParamId::LayerLevel3, 2.0f);
  CHECK(e::sanitized(p).layerLevel[2] == 1.0f);
}

TEST_CASE("arp (AP.4): Out -> Step aims at the same clock's next pulse (its copy; copy 0 after the cycle's end)") {
  Transport tr;
  tr.playing = true;
  e::ArpParams q = pulseParams(4);
  q.steps = 2;
  q.distance = 12;
  q.out.step = true;
  q.into.time = q.out.time = 0.04f;  // 480 of the 12000-frame gate
  e::ArpCore arp;
  const auto out = Harness{}.run(arp, q, {{0, jiOn(1, 400.0f, 8.0f)}}, tr, 200000, 64);
  const auto o = ons(out);
  REQUIRE(o.size() == 9);
  for (size_t i = 0; i < 8; ++i) {
    CAPTURE(i);
    const auto g = pitchesOf(out, o[i].ev.noteId);
    REQUIRE(g.size() == 1);
    CHECK(g[0].t == static_cast<int64_t>(i) * 24000 + 12000 - 480);  // the last 10 ms of its 12000-sample gate
    CHECK(g[0].ev.hz == (i % 2 == 0 ? 800.0f : 400.0f));  // odd j (also j = 7: next is j = 0) -> copy 0
  }
}

TEST_CASE("arp (AP.4): Pulse and h_ref as runtime params (ArpStage::map) switch at a block start") {
  e::ArpParams base = pulseParams(4);
  base.pulse = false;
  e::ArpStage st(base, {{e::ArpParamId::Pulse, 0}, {e::ArpParamId::PulseRef, 1}});
  float params[2] = {0.0f, 0.0f};
  pq::TempoMap tempo;
  e::EventBuffer in(64), out(256);
  in.push(jiOn(1, 800.0f, 8.0f));
  in.push(jiOn(2, 1200.0f, 12.0f));
  std::map<float, std::vector<int64_t>> by;
  for (int64_t T = 0; T < 192000; T += 64) {
    if (T == 48000) params[0] = 1.0f;   // pulse on
    if (T == 96000) params[1] = 12.0f;  // h_ref 12: the 12 clock pulses every 24000, the 8 one every 36000
    e::ClockSegment seg{0, 64, true, T};
    e::NoteStageBlock b;
    b.time = T;
    b.frames = 64;
    b.clock.tempo = &tempo;
    b.clock.sampleRate = kSr;
    b.clock.segments = &seg;
    b.clock.numSegments = 1;
    b.params = params;
    out.clear();
    st.processFn(&st, b, T == 0 ? in.list() : e::EventList{}, out);
    for (uint32_t i = 0; i < out.size(); ++i)
      if (out.list().events[i].kind == e::NoteEventKind::NoteOn) by[out.list().events[i].hz].push_back(T + out.list().events[i].frame);
  }
  // pattern mode first (Up at 1/4: 800, 1200 alternating), then pulse from 48000 on the cycle grid
  CHECK(by.at(800.0f) == std::vector<int64_t>{0, 48000, 72000, 108000, 144000, 180000});
  CHECK(by.at(1200.0f) == std::vector<int64_t>{24000, 48000, 64000, 80000, 96000, 120000, 144000, 168000});
}
