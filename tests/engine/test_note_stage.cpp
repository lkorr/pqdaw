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
      arp.process(p, b, inBuf.list(), out);
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
  // A note-on between steps waits for the next grid step.
  e::ArpCore late;
  p.rate = {true, 1, 16, 1, 1, 100.0f};
  const auto o2 = ons(Harness{}.run(late, p, {{7000, on(1, 440.0f)}}, tr, 30000, 64));
  REQUIRE(!o2.empty());
  CHECK(o2[0].t == 12000);
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
    CHECK(std::isnan(o[1].ev.degree));
    CHECK(o[1].ev.tuning == 0.0f);
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
  // stage 0: steps at 6000 … 42000 (the grid step after the note-ons at frame 3); stage 1 plays each 3000-sample note
  // twice at 1/64
  CHECK(static_cast<e::ArpStage*>(chain.stage(0))->core().stats().notes == 7);
  CHECK(s1.stats().notes == 14);
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
