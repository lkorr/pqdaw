// AP.5 step lanes and AP.2b note-stage follow-ups (pq/engine/arp.h "Step lanes", "Pulse mode"; note_stage.h
// "Handover"):
//   - lanes: polymetric lengths against the pattern, per-lane on / off, velocity / gate / transpose, Skip, Tie with
//     and without a glide, Chance deterministic and block-size invariant, per-note lanes in Pulse mode, velocity
//     source Step Lane, lane steps read from param slots (a write is the next step's value), RtScope;
//   - Pulse Cycle bars follow the time signature (3/4, 7/8, a change 4/4 → 3/4);
//   - a new degree table handed to a running core (ArpTableMailbox): steps keep their grid, sounding notes retune;
//   - a republished chain's stage adopts the running one's state (NoteStageChain::takeOver): nothing ends or restarts.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/engine/arp.h"
#include "pq/engine/note_stage.h"

namespace e = pq::engine;

namespace {

constexpr double kSr = 48000.0;
constexpr int64_t k16 = 6000;   // 1/16 at 120 BPM
constexpr int64_t kQ = 24000;   // a quarter

struct Ev {
  int64_t t;
  e::NoteEvent ev;
};

e::NoteEvent on(uint32_t id, float hz, float vel = 1.0f) { return e::noteOn(0, id, hz, vel); }
e::NoteEvent jiOn(uint32_t id, float hz, float h) {
  e::NoteEvent n = on(id, hz);
  n.harmonic = h;
  return n;
}

// Runs a stage (one call per block, like NoteStageChain::run) over `total` samples. Song transport from sample 0
// while `playing`, else Free. `before(T)` may change params between blocks.
struct Rig {
  pq::TempoMap tempo;
  const pq::TimeSigMap* sig = nullptr;
  std::vector<float> params;
  bool playing = true;
  uint64_t rtViolations = 0;
  std::function<void(int64_t)> before;

  std::vector<Ev> run(e::NoteStage& st, std::vector<Ev> in, int64_t total, uint32_t block, int64_t from = 0) {
    std::stable_sort(in.begin(), in.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    std::vector<Ev> res;
    e::EventBuffer inBuf(4096), out(4096);
    size_t ii = 0;
    for (int64_t T = from; T < total; T += block) {
      if (before) before(T);
      const auto frames = static_cast<uint32_t>(std::min<int64_t>(block, total - T));
      inBuf.clear();
      while (ii < in.size() && in[ii].t < T + frames) {
        e::NoteEvent ev = in[ii].ev;
        ev.frame = static_cast<uint32_t>(std::max<int64_t>(in[ii].t - T, 0));
        inBuf.push(ev);
        ++ii;
      }
      e::ClockSegment seg{0, frames, true, T};
      e::NoteStageBlock b;
      b.time = T;
      b.frames = frames;
      b.clock.tempo = &tempo;
      b.clock.sampleRate = kSr;
      b.clock.signatures = sig;
      if (playing) {
        b.clock.segments = &seg;
        b.clock.numSegments = 1;
      }
      b.params = params;
      out.clear();
      rtViolations += pq::alloc_trap::countRtViolations([&] { st.processFn(&st, b, inBuf.list(), out); });
      for (uint32_t i = 0; i < out.size(); ++i) res.push_back({T + out.list().events[i].frame, out.list().events[i]});
    }
    return res;
  }
};

std::vector<Ev> kind(const std::vector<Ev>& v, e::NoteEventKind k) {
  std::vector<Ev> r;
  for (const Ev& x : v)
    if (x.ev.kind == k) r.push_back(x);
  return r;
}
std::vector<Ev> ons(const std::vector<Ev>& v) { return kind(v, e::NoteEventKind::NoteOn); }

std::vector<std::string> keyOf(const std::vector<Ev>& v) {
  std::vector<std::string> r;
  for (const Ev& x : v) {
    char buf[200];
    std::snprintf(buf, sizeof buf, "%lld %d %u %.6f %.6f %u", static_cast<long long>(x.t), static_cast<int>(x.ev.kind),
                  x.ev.noteId, x.ev.hz, x.ev.velocity, x.ev.rampFrames);
    r.emplace_back(buf);
  }
  return r;
}

e::ArpParams base(int32_t den = 16) {
  e::ArpParams p;
  p.rate = {true, 1, den, 1, 1, 125.0f};
  p.gate = 0.5f;
  return p;
}

void lane(e::ArpParams& p, e::ArpLane l, int32_t len) {
  p.laneOn[static_cast<size_t>(l)] = true;
  p.laneLength[static_cast<size_t>(l)] = len;
}
e::ArpLaneSteps values(e::ArpLane l, std::initializer_list<float> v) {
  e::ArpLaneSteps s;
  int i = 0;
  for (const float x : v) s.value[static_cast<size_t>(l)][static_cast<size_t>(i++)] = x;
  return s;
}

std::vector<Ev> chord3() { return {{0, on(1, 200.0f)}, {0, on(2, 300.0f)}, {0, on(3, 400.0f)}}; }

}  // namespace

TEST_CASE("arp lanes (AP.5): a velocity lane of length 4 runs against a 3-note pattern (polymetric); off reads 1") {
  const float vals[4] = {1.0f, 0.5f, 0.25f, 0.75f};
  const float hz[3] = {200.0f, 300.0f, 400.0f};
  for (const bool onOff : {true, false}) {
    CAPTURE(onOff);
    e::ArpParams p = base();
    if (onOff) lane(p, e::ArpLane::Velocity, 4);
    e::ArpStage st(p, {}, {}, values(e::ArpLane::Velocity, {1.0f, 0.5f, 0.25f, 0.75f}));
    Rig r;
    const auto o = ons(r.run(st, chord3(), 12 * k16, 64));
    REQUIRE(o.size() == 12);
    for (size_t i = 0; i < 12; ++i) {
      CAPTURE(i);
      CHECK(o[i].t == static_cast<int64_t>(i) * k16);
      CHECK(o[i].ev.hz == hz[i % 3]);
      CHECK(o[i].ev.velocity == doctest::Approx(onOff ? vals[i % 4] : 1.0f));
    }
    CHECK(r.rtViolations == 0);
  }
}

TEST_CASE("arp lanes (AP.5): gate and transpose lanes; velocity source Step Lane") {
  {  // gate × {1, 2} at Gate 50 %: 3000 then 6000 samples
    e::ArpParams p = base();
    lane(p, e::ArpLane::Gate, 2);
    e::ArpStage st(p, {}, {}, values(e::ArpLane::Gate, {1.0f, 2.0f}));
    Rig r;
    const auto v = r.run(st, {{0, on(1, 440.0f)}}, 6 * k16, 64);
    const auto o = ons(v), f = kind(v, e::NoteEventKind::NoteOff);
    REQUIRE(o.size() == 6);
    REQUIRE(f.size() >= 5);
    for (size_t i = 0; i < 5; ++i) CHECK(f[i].t - o[i].t == (i % 2 == 0 ? 3000 : 6000));
  }
  {  // transpose {0, +7} degrees (12-EDO) with the degree identity moved
    e::ArpParams p = base();
    lane(p, e::ArpLane::Transpose, 2);
    e::ArpStage st(p, {}, {}, values(e::ArpLane::Transpose, {0.0f, 7.0f}));
    e::NoteEvent n = on(1, 440.0f);
    n.degree = 9.0f;
    n.equave = 4.0f;
    Rig r;
    const auto o = ons(r.run(st, {{0, n}}, 4 * k16, 64));
    REQUIRE(o.size() == 4);
    CHECK(o[0].ev.hz == 440.0f);
    CHECK(o[1].ev.hz == doctest::Approx(440.0 * std::exp2(7.0 / 12.0)));
    CHECK(o[1].ev.degree == 4.0f);
    CHECK(o[1].ev.equave == 5.0f);
    CHECK(o[2].ev.hz == 440.0f);
  }
  {  // source Step Lane: the lane's value is the velocity (Fixed velocity ignored); lane off: as played
    e::ArpParams p = base();
    p.velocitySource = e::ArpVelocitySource::Lane;
    p.fixedVelocity = 0.8f;
    lane(p, e::ArpLane::Velocity, 1);
    e::ArpStage st(p, {}, {}, values(e::ArpLane::Velocity, {0.3f}));
    Rig r;
    const auto o = ons(r.run(st, {{0, on(1, 440.0f, 0.9f)}}, 2 * k16, 64));
    REQUIRE(o.size() == 2);
    CHECK(o[0].ev.velocity == doctest::Approx(0.3f));
    p.laneOn = {};
    e::ArpStage off(p, {}, {}, values(e::ArpLane::Velocity, {0.3f}));
    Rig r2;
    CHECK(ons(r2.run(off, {{0, on(1, 440.0f, 0.9f)}}, k16, 64))[0].ev.velocity == doctest::Approx(0.9f));
  }
}

TEST_CASE("arp lanes (AP.5): Skip rests; Tie continues the previous note, gliding with Into on") {
  using K = e::ArpStepKind;
  const std::vector<Ev> two{{0, on(1, 200.0f)}, {0, on(2, 300.0f)}};
  {  // Play | Skip: only the even steps (the pattern still advances: always 200)
    e::ArpParams p = base();
    lane(p, e::ArpLane::SkipTie, 2);
    e::ArpStage st(p, {}, {}, values(e::ArpLane::SkipTie, {float(K::Play), float(K::Skip)}));
    Rig r;
    const auto o = ons(r.run(st, two, 8 * k16, 64));
    REQUIRE(o.size() == 4);
    for (size_t i = 0; i < 4; ++i) {
      CHECK(o[i].t == static_cast<int64_t>(2 * i) * k16);
      CHECK(o[i].ev.hz == 200.0f);
    }
    CHECK(st.core().stats().laneRests == 4);
  }
  for (const bool into : {false, true}) {  // Play | Tie
    CAPTURE(into);
    e::ArpParams p = base();
    lane(p, e::ArpLane::SkipTie, 2);
    if (into) p.into = e::ArpGlide::Step;
    e::ArpStage st(p, {}, {}, values(e::ArpLane::SkipTie, {float(K::Play), float(K::Tie)}));
    Rig r;
    const auto v = r.run(st, two, 8 * k16, 64);
    const auto o = ons(v), f = kind(v, e::NoteEventKind::NoteOff), pitch = kind(v, e::NoteEventKind::Pitch);
    REQUIRE(o.size() == 4);  // the tie steps start no note
    REQUIRE(f.size() >= 3);
    for (size_t i = 0; i < 3; ++i) {
      CHECK(o[i].t == static_cast<int64_t>(2 * i) * k16);
      CHECK(f[i].ev.noteId == o[i].ev.noteId);
      CHECK(f[i].t == static_cast<int64_t>(2 * i + 1) * k16 + 3000);  // the tie step's gate end
    }
    CHECK(st.core().stats().ties == 4);
    if (into) {  // at each tie step's onset: a glide to that step's note (300), Glide ms 50 = 2400 frames
      std::vector<Ev> atTies;
      for (const Ev& x : pitch)
        if (x.t % (2 * k16) == k16) atTies.push_back(x);
      REQUIRE(atTies.size() == 4);
      for (size_t i = 0; i < 4; ++i) {
        CHECK(atTies[i].t == static_cast<int64_t>(2 * i + 1) * k16);
        CHECK(atTies[i].ev.noteId == o[i].ev.noteId);
        CHECK(atTies[i].ev.hz == doctest::Approx(300.0f));
        CHECK(atTies[i].ev.rampFrames == 2400);
      }
    } else {
      CHECK(pitch.empty());
    }
  }
}

TEST_CASE("arp lanes (AP.5): Chance is deterministic, block-size invariant and seeded") {
  auto runWith = [](uint32_t block, uint32_t seed) {
    e::ArpParams p = base();
    p.seed = seed;
    lane(p, e::ArpLane::Chance, 1);
    e::ArpStage st(p, {}, {}, values(e::ArpLane::Chance, {0.5f}));
    Rig r;
    auto v = ons(r.run(st, chord3(), 64 * k16, block));
    CHECK(r.rtViolations == 0);
    return keyOf(v);
  };
  const auto a = runWith(64, 0);
  CHECK(a.size() > 16);
  CHECK(a.size() < 48);
  CHECK(runWith(37, 0) == a);
  CHECK(runWith(512, 0) == a);
  CHECK(runWith(64, 1) != a);
}

TEST_CASE("arp lanes (AP.5): Pulse mode reads the lanes per note (its own pulse count)") {
  e::ArpParams p = base(4);
  p.pulse = true;
  lane(p, e::ArpLane::Velocity, 2);
  e::ArpStage st(p, {}, {}, values(e::ArpLane::Velocity, {1.0f, 0.5f}));
  Rig r;
  // h 2 and 3, h_ref = 2: periods 24000 and 16000, cycle 48000.
  const auto o = ons(r.run(st, {{0, jiOn(1, 200.0f, 2.0f)}, {0, jiOn(2, 300.0f, 3.0f)}}, 4 * kQ, 64));
  std::map<float, std::vector<std::pair<int64_t, float>>> by;
  for (const Ev& x : o) by[x.ev.hz].push_back({x.t, x.ev.velocity});
  REQUIRE(by[200.0f].size() == 4);
  REQUIRE(by[300.0f].size() == 6);
  for (size_t i = 0; i < 4; ++i) {
    CHECK(by[200.0f][i].first == static_cast<int64_t>(i) * 24000);
    CHECK(by[200.0f][i].second == doctest::Approx(i % 2 == 0 ? 1.0f : 0.5f));
  }
  for (size_t i = 0; i < 6; ++i) {
    CHECK(by[300.0f][i].first == static_cast<int64_t>(i) * 16000);
    CHECK(by[300.0f][i].second == doctest::Approx(i % 2 == 0 ? 1.0f : 0.5f));
  }
  CHECK(r.rtViolations == 0);
}

TEST_CASE("arp lanes (AP.5): lane steps from param slots: a write is the next step's value, nothing rebuilt") {
  e::ArpParams p = base();
  lane(p, e::ArpLane::Velocity, 2);
  const e::ArpParamInfo& info = e::arpParamInfo(e::arpLaneStepId(e::ArpLane::Velocity, 1));
  CHECK(info.runtime);
  CHECK(e::arpParamInfo(e::ArpParamId::VelocityLane).runtime == false);
  CHECK(e::arpParamInfo(e::ArpParamId::VelocityLength).runtime == false);
  // slot 0 = step 1's knob position (scale / offset as the document maps it), step 0 fixed
  e::ArpStage st(p, {{e::arpLaneStepId(e::ArpLane::Velocity, 1), 0, info.max - info.min, info.min}}, {},
                 values(e::ArpLane::Velocity, {1.0f, 1.0f}));
  Rig r;
  r.params = {0.5f};
  r.before = [&](int64_t T) {
    if (T == 4 * k16) r.params[0] = 0.25f;
  };
  const auto o = ons(r.run(st, {{0, on(1, 440.0f)}}, 8 * k16, 64));
  REQUIRE(o.size() == 8);
  CHECK(o[1].ev.velocity == doctest::Approx(0.5f));
  CHECK(o[3].ev.velocity == doctest::Approx(0.5f));
  CHECK(o[5].ev.velocity == doctest::Approx(0.25f));
  CHECK(o[6].ev.velocity == doctest::Approx(1.0f));
  CHECK(r.rtViolations == 0);
}

TEST_CASE("arp lanes (AP.5): param table rows, setArpParam / setArpLaneStep / sanitized") {
  CHECK(std::size(e::kArpParamInfo) == static_cast<size_t>(e::ArpParamId::Count));
  CHECK(std::string(e::arpParamInfo(e::arpLaneStepId(e::ArpLane::SkipTie, 31)).name) == "tieStep32");
  CHECK(std::string(e::arpParamInfo(e::arpLaneStepId(e::ArpLane::Velocity, 0)).name) == "velStep1");
  e::ArpParams p;
  e::setArpParam(p, e::ArpParamId::GlideLane, 1.0f);
  e::setArpParam(p, e::ArpParamId::ChanceLength, 99.0f);
  e::setArpParam(p, e::ArpParamId::VelocitySource, 2.0f);
  CHECK(p.laneOn[static_cast<size_t>(e::ArpLane::Glide)]);
  CHECK(p.laneLength[static_cast<size_t>(e::ArpLane::Chance)] == 32);
  CHECK(p.velocitySource == e::ArpVelocitySource::Lane);
  e::ArpLaneSteps s;
  CHECK(e::setArpLaneStep(s, e::arpLaneStepId(e::ArpLane::Transpose, 3), 5.0f));
  CHECK(s.value[static_cast<size_t>(e::ArpLane::Transpose)][3] == 5.0f);
  CHECK_FALSE(e::setArpLaneStep(s, e::ArpParamId::Gate, 1.0f));
  p.laneLength[0] = 0;
  CHECK(e::sanitized(p).laneLength[0] == 1);
}

TEST_CASE("arp (AP.2b): Pulse Cycle bars follow the time signature (3/4, 7/8, a change 4/4 -> 3/4)") {
  // One clock (Order: h = 1, h_ref 1) at a half note (48000): restarts at every bar line.
  auto onsets = [](std::vector<pq::TimeSigChange> changes, int64_t total) {
    auto sm = pq::TimeSigMap::create(changes);
    REQUIRE(sm.ok());
    e::ArpParams p = base(2);
    p.pulse = true;
    p.pulseSource = e::ArpPulseSource::Order;
    p.pulseRef = 1;
    p.pulseCycle = e::ArpPulseCycle::Bars;
    p.pulseBars = 1;
    e::ArpStage st(p);
    Rig r;
    r.sig = &sm.value();
    std::vector<int64_t> t;
    for (const Ev& x : ons(r.run(st, {{0, on(1, 440.0f)}}, total, 64))) t.push_back(x.t);
    CHECK(r.rtViolations == 0);
    return t;
  };
  CHECK(onsets({{0, {3, 4}}}, 150000) == std::vector<int64_t>{0, 48000, 72000, 120000, 144000});
  CHECK(onsets({{0, {7, 8}}}, 170000) == std::vector<int64_t>{0, 48000, 84000, 132000, 168000});
  CHECK(onsets({{0, {4, 4}}, {1, {3, 4}}}, 170000) == std::vector<int64_t>{0, 48000, 96000, 144000, 168000});
  CHECK(onsets({{0, {4, 4}}}, 150000) == std::vector<int64_t>{0, 48000, 96000, 144000});
}

TEST_CASE("arp (AP.2b): a new degree table reaches a running core: same grid, sounding steps retune at once") {
  // Up over one note with Steps 2 / Distance 1 (degree +1 every other step), gate 100 %.
  e::ArpParams p = base();
  p.steps = 2;
  p.gate = 1.0f;
  e::NoteEvent n = on(1, 440.0f);
  n.degree = 0.0f;
  n.equave = 4.0f;
  auto box = std::make_shared<e::ArpTableMailbox>();
  e::ArpStage st(p, {}, {}, {}, box);
  e::ArpStage ref(p);
  Rig r, rr;
  const int64_t at = 3 * k16 + 1000;  // inside step 3 (a shifted one)
  e::ArpDegreeTable t19;
  for (int i = 0; i < 19; ++i) t19.cents.push_back(1200.0 * i / 19.0);
  r.before = [&](int64_t T) {
    if (T == at) box->post(t19);
  };
  const auto v = r.run(st, {{0, n}}, 8 * k16, 500);
  const auto w = rr.run(ref, {{0, n}}, 8 * k16, 500);
  const auto o = ons(v), ow = ons(w);
  REQUIRE(o.size() == ow.size());
  for (size_t i = 0; i < o.size(); ++i) CHECK(o[i].t == ow[i].t);  // the pattern keeps its place
  CHECK(kind(v, e::NoteEventKind::NoteOff).size() == kind(w, e::NoteEventKind::NoteOff).size());
  const double up19 = 440.0 * std::exp2(1.0 / 19.0);
  CHECK(o[1].ev.hz == doctest::Approx(440.0 * std::exp2(1.0 / 12.0)));
  CHECK(o[5].ev.hz == doctest::Approx(up19));
  // the sounding step 3 retuned in the block the table arrived (frame 0 of the block at `at`)
  bool retuned = false;
  for (const Ev& x : kind(v, e::NoteEventKind::Pitch))
    if (x.t == at && (x.ev.flags & e::kNoteEventRetune) != 0 && x.ev.noteId == o[3].ev.noteId) {
      retuned = true;
      CHECK(x.ev.hz == doctest::Approx(up19));
    }
  CHECK(retuned);
  CHECK(r.rtViolations == 0);
}

TEST_CASE("arp (AP.2b): a republished chain's stage adopts the running state: nothing ends, nothing restarts") {
  e::ArpParams p = base();
  p.gate = 1.5f;  // overlapping notes: one is always sounding at the handover
  auto make = [&](bool laneOnB) {
    e::ArpParams q = p;
    if (laneOnB) lane(q, e::ArpLane::Velocity, 4);  // a structural change: the lane switched on (all at 1)
    auto c = std::make_unique<e::NoteStageChain>(1024);
    auto s = std::make_unique<e::ArpStage>(q);
    s->key = 42;
    REQUIRE(c->add(std::move(s)));
    return c;
  };
  struct ChainStage : e::NoteStage {  // drives a chain as one stage of the rig
    e::NoteStageChain* c = nullptr;
    e::EventBuffer buf{4096};
    static bool proc(void* self, const e::NoteStageBlock& b, const e::EventList& in, e::EventBuffer& out) noexcept {
      auto& s = *static_cast<ChainStage*>(self);
      s.c->input().clear();
      for (uint32_t i = 0; i < in.count; ++i) s.c->input().push(in.events[i]);
      return s.c->run(b, out);
    }
    ChainStage() { processFn = &proc; }
    void setIndex(uint32_t) override {}
  };
  auto uninterrupted = make(false);
  ChainStage u;
  u.c = uninterrupted.get();
  Rig ru;
  const auto want = ru.run(u, chord3(), 16 * k16, 64);

  auto first = make(false);
  auto second = make(true);
  ChainStage s;
  s.c = first.get();
  Rig r;
  std::vector<Ev> got;
  e::EventBuffer handOut(256), handHeld(256);
  const int64_t swapAt = 5 * k16 + 640;
  r.before = [&](int64_t T) {
    if (T != swapAt) return;
    handOut.clear();
    handHeld.clear();
    const uint64_t v = pq::alloc_trap::countRtViolations([&] { second->takeOver(*first, 0, handOut, handHeld); });
    CHECK(v == 0);
    s.c = second.get();
  };
  got = r.run(s, chord3(), 16 * k16, 64);
  CHECK(handOut.size() == 0);   // no note ended
  CHECK(handHeld.size() == 0);  // no key re-sent: the held set moved with the state
  CHECK(keyOf(got) == keyOf(want));
}
