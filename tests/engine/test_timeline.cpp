// T3.16: the engine timeline — transport (play/stop/continue/locate, loop wrap, tempo map), the note sequencer
// (SequenceImage, lazily unrolled clip loops, chance / velocity range, image swaps), the automation renderer
// (evalAutomation, per-sample buffers), and the integration with the VoiceEngine and renderOffline.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/automation_curve.h"
#include "pq/document/automation_curve.h"
#include "pq/document/clip_time.h"
#include "pq/engine/automation.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/note_list.h"
#include "pq/engine/offline_render.h"
#include "pq/engine/sequence.h"
#include "pq/engine/timeline.h"
#include "pq/engine/voices.h"
#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"

namespace {

namespace e = pq::engine;
namespace g = pq::graph;
using pq::Ticks;
using pq::kPpq;
using pq::kTicksPerWhole;

constexpr double kSr = 48000.0;
constexpr Ticks kBar = kTicksPerWhole;  // 4/4

// ---- descriptions ---------------------------------------------------------------------------------------------

e::NoteDesc note(uint32_t id, Ticks start, Ticks length, float hz = 0.0f) {
  e::NoteDesc n;
  n.id = id;
  n.start = start;
  n.length = length;
  n.hz = hz > 0.0f ? hz : 100.0f + static_cast<float>(id);  // distinct per id: events identify their note
  return n;
}

e::ClipDesc clip(uint32_t id, Ticks start, Ticks length, std::vector<e::NoteDesc> notes) {
  e::ClipDesc c;
  c.id = id;
  c.start = start;
  c.length = length;
  c.notes = std::move(notes);
  return c;
}

std::unique_ptr<e::SequenceImage> image(const std::vector<e::TrackSequenceDesc>& tracks, uint64_t seed = 0) {
  e::SequenceBuildOptions o;
  o.seed = seed;
  auto r = e::SequenceImage::build(tracks, o);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
  return std::move(r.value());
}

std::unique_ptr<e::TempoImage> tempoImage(std::vector<pq::TempoPoint> pts, std::vector<pq::TimeSigChange> sigs = {}) {
  auto r = e::TempoImage::create(pts, sigs);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
  return std::move(r.value());
}

e::TransportCommand cmd(e::TransportCommandKind k, Ticks t = 0) {
  e::TransportCommand c;
  c.kind = k;
  c.tick = t;
  return c;
}
e::TransportCommand loopCmd(Ticks a, Ticks b, bool on = true) {
  e::TransportCommand c;
  c.kind = e::TransportCommandKind::SetLoop;
  c.loopStart = a;
  c.loopEnd = b;
  c.loopEnabled = on;
  return c;
}

// ---- driving --------------------------------------------------------------------------------------------------

struct Ev {
  int64_t t;  // engine sample
  uint32_t track;
  e::NoteEvent e;
};

struct Driver {
  e::Timeline& tl;
  int64_t clock = 0;
  std::vector<Ev> events;

  // Runs `frames` frames in blocks of `block`; `before(clock)` runs before each block (publish, apply…).
  template <class F>
  void run(int64_t frames, uint32_t block, F&& before) {
    for (int64_t done = 0; done < frames;) {
      const uint32_t n = static_cast<uint32_t>(std::min<int64_t>(block, frames - done));
      before(clock);
      tl.process(clock, n);
      for (uint32_t k = 0; k < 8; ++k) {
        const e::EventList l = tl.events(k);
        for (uint32_t i = 0; i < l.count; ++i) {
          REQUIRE(l.events[i].frame < n);
          if (i > 0) REQUIRE(l.events[i].frame >= l.events[i - 1].frame);
          events.push_back({clock + l.events[i].frame, k, l.events[i]});
        }
      }
      clock += n;
      done += n;
    }
  }
  void run(int64_t frames, uint32_t block) {
    run(frames, block, [](int64_t) {});
  }
};

// Sounded notes reconstructed from events: on time, off time (−1 if still sounding), Hz, velocity.
struct Played {
  int64_t on, off;
  uint32_t track;
  float hz, velocity;
  bool operator<(const Played& o) const {
    return on != o.on ? on < o.on : hz != o.hz ? hz < o.hz : off < o.off;
  }
  bool operator==(const Played& o) const { return on == o.on && off == o.off && hz == o.hz && track == o.track; }
};

std::vector<Played> played(const std::vector<Ev>& evs) {
  std::vector<Played> out;
  std::map<uint32_t, size_t> open;  // instance id → index
  for (const Ev& v : evs) {
    if (v.e.kind == e::NoteEventKind::NoteOn) {
      REQUIRE(open.count(v.e.noteId) == 0);
      open[v.e.noteId] = out.size();
      out.push_back({v.t, -1, v.track, v.e.hz, v.e.velocity});
    } else if (v.e.kind == e::NoteEventKind::NoteOff) {
      auto it = open.find(v.e.noteId);
      REQUIRE(it != open.end());
      out[it->second].off = v.t;
      open.erase(it);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

const e::TimelineConfig& config() {
  static const e::TimelineConfig c = [] {
    e::TimelineConfig x;
    x.sampleRate = kSr;
    x.maxBlockFrames = 512;
    x.maxTracks = 8;
    return x;
  }();
  return c;
}

// ---- compiled instruments (VoiceEngine / renderOffline tests) -------------------------------------------------

struct Built {
  g::TypedPatch typed;
  g::Schedule sched;
  std::unique_ptr<e::ProgramImage> image;
};

Built build(const char* text, uint32_t maxBlock = 512, uint32_t groups = 3) {
  auto file = g::parse(text, "t.pqp");
  REQUIRE(file);
  g::PatchLibrary lib;
  lib.add(*file);
  g::InferOptions io;
  io.library = &lib;
  auto typed = g::infer(file->patches[0], io);
  REQUIRE_MESSAGE(typed.ok(), (typed.ok() ? "" : typed.error().message));
  g::OptimiseOptions oo;
  oo.sampleRate = kSr;
  auto opt = g::optimise(std::move(typed.value()), oo);
  REQUIRE(opt);
  Built b;
  b.typed = std::move(opt.value());
  g::ScheduleOptions so;
  so.sampleRate = kSr;
  auto s = g::schedule(b.typed, so);
  REQUIRE(s);
  b.sched = std::move(s.value());
  e::InterpretOptions eo;
  eo.maxBlockFrames = maxBlock;
  eo.maxVoiceGroups = groups;
  auto img = e::buildInterpreted(b.sched, eo);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  b.image = std::move(img.value());
  return b;
}

const char* kSine = R"(patch Sine {
  out y : audio voice
  y = sine(phasor(voice.hz)) * gate_to_real(voice.gate) * 0.3
}
)";

const char* kBeat = R"(patch Beat {
  param gain = 1 [0..10]
  out y : audio
  out p : audio
  y = transport.beat * gain
  p = transport.barPhase
}
)";

uint32_t heldLanes(const e::VoiceEngine& ve) {
  uint32_t n = 0;
  for (uint32_t l = 0; l < ve.lanes(); ++l) n += ve.laneStatus(l) == e::LaneStatus::Held ? 1u : 0u;
  return n;
}

}  // namespace

// ---- the curve law ----------------------------------------------------------------------------------------------

TEST_CASE("evalAutomation: the Q39 law, jumps, ends; equal to the document's automationValueAt at integer ticks") {
  std::vector<e::AutomationPoint> pts{{100, 0.0f, 0.0f}, {200, 1.0f, 0.8f}, {200, 0.25f, -0.5f}, {400, 0.75f, 0.0f},
                                      {500, -2.0f, 0.0f}};
  std::vector<pq::doc::Breakpoint> doc;
  for (const auto& p : pts) {
    pq::doc::Breakpoint b;
    b.tick = p.tick;
    b.value = p.value;
    b.curve = p.curve;
    doc.push_back(b);
  }
  const std::span<const e::AutomationPoint> sp(pts);
  CHECK(e::evalAutomation(sp, 0.0) == 0.0);       // before the first point: the first value
  CHECK(e::evalAutomation(sp, 150.0) == doctest::Approx(0.5));
  CHECK(e::evalAutomation(sp, 199.999) > 0.99);  // arriving at the jump: the first point on the tick
  CHECK(e::evalAutomation(sp, 200.0) == 0.25);    // from the tick on: the last point on it
  CHECK(e::evalAutomation(sp, 1e9) == -2.0);
  CHECK(e::evalAutomation(std::span<const e::AutomationPoint>(), 5.0, 3.0) == 3.0);
  // After the jump (curve −0.5): the closed form; and the shape for curve 0.8.
  const double x = 0.5, k = pq::kAutomationCurveK * 0.8;
  CHECK(e::evalAutomation(sp, 250.0) == doctest::Approx(0.25 + 0.5 * std::expm1(-3.0 * 0.25) / std::expm1(-3.0)));
  CHECK(pq::automationShape(x, 0.8) == doctest::Approx(std::expm1(k * x) / std::expm1(k)));
  for (Ticks t = 0; t <= 600; ++t)
    CHECK(e::evalAutomation(sp, static_cast<double>(t)) == pq::doc::automationValueAt(doc, t));
}

// ---- SequenceImage --------------------------------------------------------------------------------------------

TEST_CASE("SequenceImage: build errors, dropped notes, overlap trimming, findOccurrence") {
  {
    e::TrackSequenceDesc t{1, {clip(1, 0, 0, {})}};
    CHECK_FALSE(e::SequenceImage::build(std::span(&t, 1)).ok());
    t.clips[0] = clip(1, 0, kBar, {note(1, 0, 0)});
    CHECK_FALSE(e::SequenceImage::build(std::span(&t, 1)).ok());
    t.clips[0] = clip(1, 0, kBar, {note(1, 0, 10)});
    t.clips[0].notes[0].hz = -3.0f;
    CHECK_FALSE(e::SequenceImage::build(std::span(&t, 1)).ok());
  }
  e::ClipDesc a = clip(1, 0, 2 * kBar, {note(1, 0, 100), note(2, kBar + 10, 100), note(3, -50, 100)});
  a.notes.push_back(note(4, 500, 100));
  a.notes.back().enabled = false;
  a.notes.push_back(note(5, 600, 100));
  a.notes.back().chance = 0.0f;
  e::ClipDesc b = clip(2, kBar, kBar, {note(10, 0, 100)});  // overwrites a's second bar
  e::ClipDesc off = clip(3, 3 * kBar, kBar, {note(20, 0, 100)});
  off.active = false;
  auto img = image({{7, {b, a, off}}});
  REQUIRE(img->numTracks() == 1);
  REQUIRE(img->clips().size() == 2);
  CHECK(img->clips()[0].end == kBar);  // trimmed
  CHECK(img->notes().size() == 2);     // note 1 (a) and 10 (b): the others are disabled, chance 0, before the start
                                       // marker, or in the overwritten part
  Ticks passEnd = 0;
  REQUIRE(img->findOccurrence(0, 1, 0, &passEnd) != nullptr);
  CHECK(passEnd == kBar);
  CHECK(img->findOccurrence(0, 10, kBar, &passEnd) != nullptr);
  CHECK(img->findOccurrence(0, 10, kBar + 1, &passEnd) == nullptr);
  CHECK(img->findOccurrence(0, 2, kBar + 10, &passEnd) == nullptr);
}

TEST_CASE("noteRandom: deterministic, uniform, independent per note and pass") {
  const auto a = e::noteRandom(1, 42, 3), b = e::noteRandom(1, 42, 3);
  CHECK(a.chance == b.chance);
  CHECK(a.velocity == b.velocity);
  CHECK(e::noteRandom(2, 42, 3).chance != a.chance);
  double sum = 0, sumV = 0;
  int below = 0;
  for (int p = 0; p < 20000; ++p) {
    const auto r = e::noteRandom(0, 7, p);
    REQUIRE(r.chance >= 0.0f);
    REQUIRE(r.chance < 1.0f);
    sum += r.chance;
    sumV += r.velocity;
    below += r.chance < 0.25f ? 1 : 0;
  }
  CHECK(sum / 20000 == doctest::Approx(0.5).epsilon(0.02));
  CHECK(sumV / 20000 == doctest::Approx(0.5).epsilon(0.02));
  CHECK(below / 20000.0 == doctest::Approx(0.25).epsilon(0.05));
}

// ---- note timing ------------------------------------------------------------------------------------------------

TEST_CASE("timeline: a note at tick t starts at exactly ticksToSamples(t) for blocks 1/32/37/480, ramps included") {
  // Notes at awkward ticks (tuplets, single ticks, many per block), across tempo steps and ramps.
  std::vector<e::NoteDesc> notes;
  uint32_t id = 1;
  for (Ticks t = 0; t < 10 * kBar; t += 7919) notes.push_back(note(id++, t, 1 + (t % 3001)));
  for (int k = 0; k < 7; ++k) notes.push_back(note(id++, 2 * kBar + pq::tupletPosition(0, kPpq, 7, k), kPpq / 7));
  notes.push_back(note(id++, 5 * kBar + 1, 1));
  notes.push_back(note(id++, 5 * kBar + 2, 1));
  const std::vector<pq::TempoPoint> tp{{0, 120.0, pq::TempoCurve::Linear},
                                       {3 * kBar, 90.0, pq::TempoCurve::Step},
                                       {6 * kBar, 150.0, pq::TempoCurve::Linear},
                                       {9 * kBar, 61.5, pq::TempoCurve::Step}};
  auto tempo = tempoImage(tp);
  const pq::TempoMap& tm = tempo->tempo();
  std::vector<Played> want;
  for (const auto& n : notes)
    want.push_back({tm.ticksToSamples(n.start, kSr), tm.ticksToSamples(n.start + n.length, kSr), 0, n.hz, 0.0f});
  std::sort(want.begin(), want.end());
  const int64_t frames = tm.ticksToSamples(11 * kBar, kSr);

  for (uint32_t block : {1u, 32u, 37u, 480u}) {
    CAPTURE(block);
    e::Timeline tl(config());
    REQUIRE(tl.publishTempo(tempoImage(tp)));
    REQUIRE(tl.publishSequence(image({{1, {clip(1, 0, 12 * kBar, notes)}}})));
    tl.apply(cmd(e::TransportCommandKind::Play, 0));
    Driver d{tl};
    d.run(frames, block);
    const auto got = played(d.events);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < want.size(); ++i) {
      CHECK(got[i].on == want[i].on);
      CHECK(got[i].off == want[i].off);
      CHECK(got[i].hz == want[i].hz);
    }
    CHECK(tl.stats().noteOns == notes.size());
    CHECK(tl.soundingNotes(0) == 0);
  }
}

TEST_CASE("timeline: clip loops unrolled lazily match document clip_time semantics (start marker, folding, cut)") {
  struct Case {
    Ticks start, length, contentStart, loopStart, loopEnd;
    bool loop;
  };
  const Case cases[] = {
      {kBar, 6 * kBar, 0, 0, kBar, true},                        // plain loop
      {kBar / 2, 5 * kBar + 333, kBar / 4, 0, kBar, true},       // start marker inside the loop, odd clip end
      {0, 4 * kBar, 0, kBar / 2, kBar + kBar / 4, true},         // pre-loop part, loop later
      {0, 4 * kBar, 2 * kBar + 100, kBar / 2, kBar, true},       // start marker after loop end: folded
      {2 * kBar, 3 * kBar, kBar / 3, 0, kBar, false},            // no loop, start marker
      {0, 3 * kBar, 0, 100, 100 + kPpq / 5, true},               // a tiny loop: many passes per beat
  };
  // Notes over two bars of content, some crossing the loop end (cut at the wrap), lengths up to 1.5 beats.
  std::vector<e::NoteDesc> notes;
  uint32_t id = 1;
  for (Ticks t = 0; t < 2 * kBar; t += kPpq / 3 + 17) notes.push_back(note(id++, t, kPpq / 2 + (t % (kPpq))));
  notes.push_back(note(id++, 100, 5));
  for (const Case& c : cases) {
    CAPTURE(c.contentStart);
    CAPTURE(c.loopStart);
    e::ClipDesc cd = clip(1, c.start, c.length, notes);
    cd.contentStart = c.contentStart;
    cd.loopEnabled = c.loop;
    cd.loopStart = c.loopStart;
    cd.loopEnd = c.loopEnd;
    // Oracle: pq::doc::forEachClipSegment over the document's clip with the same fields.
    pq::doc::NoteClip dc;
    dc.start = c.start;
    dc.length = c.length;
    dc.contentStart = c.contentStart;
    dc.loop.enabled = c.loop;
    dc.loop.start = c.loopStart;
    dc.loop.end = c.loopEnd;
    std::vector<Played> want;
    const pq::TempoMap tm;
    pq::doc::forEachClipSegment(dc, 0, c.start + c.length, [&](const pq::doc::ClipSegment& s) {
      for (const auto& n : notes)
        if (n.start >= s.contentFrom && n.start < s.contentTo) {
          const Ticks on = s.at + (n.start - s.contentFrom);
          const Ticks off = std::min(on + n.length, s.at + (s.contentTo - s.contentFrom));
          want.push_back({tm.ticksToSamples(on, kSr), tm.ticksToSamples(off, kSr), 0, n.hz, 0.0f});
        }
    });
    std::sort(want.begin(), want.end());
    REQUIRE(!want.empty());
    for (uint32_t block : {37u, 480u}) {
      e::Timeline tl(config());
      REQUIRE(tl.publishSequence(image({{1, {cd}}})));
      tl.apply(cmd(e::TransportCommandKind::Play, 0));
      Driver d{tl};
      d.run(tm.ticksToSamples(c.start + c.length + kBar, kSr), block);
      const auto got = played(d.events);
      REQUIRE(got.size() == want.size());
      for (size_t i = 0; i < want.size(); ++i) {
        CHECK(got[i].on == want[i].on);
        CHECK(got[i].off == want[i].off);
        CHECK(got[i].hz == want[i].hz);
      }
    }
    // Starting in the middle (a seek into a later pass) plays exactly the notes from there on.
    e::Timeline tl(config());
    REQUIRE(tl.publishSequence(image({{1, {cd}}})));
    // Just after the middle, at the first tick of a sample: every earlier tick is on an earlier sample, so "plays
    // from tick `from`" and "sounds at sample ≥ s0" select the same notes (a tick is ~0.11 samples here).
    const Ticks from = tm.firstTickAtOrAfterSample(tm.ticksToSamples(c.start + c.length / 2, kSr) + 1, kSr);
    tl.apply(cmd(e::TransportCommandKind::Play, from));
    Driver d{tl};
    d.run(tm.ticksToSamples(c.start + c.length + kBar, kSr) - tm.ticksToSamples(from, kSr), 64);
    const int64_t s0 = tm.ticksToSamples(from, kSr);
    std::vector<Played> tail;
    for (const Played& p : want)
      if (p.on >= s0) tail.push_back({p.on - s0, p.off - s0, 0, p.hz, 0.0f});
    const auto got = played(d.events);
    REQUIRE(got.size() == tail.size());
    for (size_t i = 0; i < tail.size(); ++i) {
      CHECK(got[i].on == tail[i].on);
      CHECK(got[i].off == tail[i].off);
    }
  }
}

TEST_CASE("timeline: arrangement loop wraps at the exact sample, with offs then ons there, for any block size") {
  const Ticks L0 = 2 * kBar, L1 = 3 * kBar;
  // A at the loop start, B across the loop end (cut at the wrap), C before the loop (plays once).
  const auto seq = [&] {
    return image({{1, {clip(1, 0, 8 * kBar, {note(1, L0, kBar / 4), note(2, L1 - kBar / 8, kBar / 2), note(3, kBar, 10)})}}});
  };
  const pq::TempoMap tm;
  const int64_t S1 = tm.ticksToSamples(kBar, kSr), LS = tm.ticksToSamples(L0, kSr), LE = tm.ticksToSamples(L1, kSr);
  REQUIRE((LE - S1) % 480 == 0);  // block 480: the first wrap falls exactly on a block boundary
  std::vector<Ev> reference;
  for (uint32_t block : {1u, 32u, 37u, 480u, 512u}) {
    CAPTURE(block);
    e::Timeline tl(config());
    REQUIRE(tl.publishSequence(seq()));
    tl.apply(loopCmd(L0, L1));
    tl.apply(cmd(e::TransportCommandKind::Play, kBar));
    Driver d{tl};
    d.run((LE - S1) + 3 * (LE - LS) + 100, block);
    CHECK(tl.stats().loopWraps == 4);
    // Engine time of the wraps: LE − S1, then every loop length.
    for (int w = 0; w < 3; ++w) {
      const int64_t wrapAt = (LE - S1) + w * (LE - LS);
      std::vector<e::NoteEventKind> at;
      std::vector<float> hz;
      for (const Ev& v : d.events)
        if (v.t == wrapAt) {
          at.push_back(v.e.kind);
          hz.push_back(v.e.hz);
        }
      REQUIRE(at.size() == 2);
      CHECK(at[0] == e::NoteEventKind::NoteOff);  // B cut at the wrap
      CHECK(at[1] == e::NoteEventKind::NoteOn);   // A at the loop start
      CHECK(hz[1] == 101.0f);
    }
    const auto p = played(d.events);
    REQUIRE(p.size() == 1 + 5 + 4);  // C once; A in 5 passes (the last just started), B in 4
    CHECK(p[0].hz == 103.0f);
    CHECK(p[0].on == 0);
    if (block == 1u) {
      reference = d.events;
    } else {
      REQUIRE(d.events.size() == reference.size());
      for (size_t i = 0; i < reference.size(); ++i) {
        CHECK(d.events[i].t == reference[i].t);
        CHECK(d.events[i].e.kind == reference[i].e.kind);
        CHECK(d.events[i].e.hz == reference[i].e.hz);
      }
    }
  }
}

TEST_CASE("timeline: a loop shorter than the block wraps several times inside one block") {
  e::Timeline tl(config());
  REQUIRE(tl.publishSequence(image({{1, {clip(1, 0, kBar, {note(1, 0, 10)})}}})));
  const pq::TempoMap tm;
  const Ticks len = kPpq / 64;  // 433 ticks: 375 samples at 120 BPM
  tl.apply(loopCmd(0, len));
  tl.apply(cmd(e::TransportCommandKind::Play, 0));
  Driver d{tl};
  d.run(512 * 4, 512);
  const int64_t loopSamples = tm.ticksToSamples(len, kSr);
  const auto p = played(d.events);
  REQUIRE(p.size() >= 5);
  for (size_t i = 0; i < p.size(); ++i) CHECK(p[i].on == static_cast<int64_t>(i) * loopSamples);
}

TEST_CASE("timeline: stop releases, continue resumes at the held tick, locate during playback releases every voice") {
  Built b = build(kSine);
  e::VoiceEngine ve;
  e::VoiceConfig vc;
  vc.maxVoices = 16;
  REQUIRE(ve.prepare(b.image.get(), vc));
  e::Timeline tl(config());
  REQUIRE(tl.publishSequence(image({{1, {clip(1, 0, 8 * kBar, {note(1, 0, 4 * kBar), note(2, 100, 4 * kBar),
                                                                note(3, kBar, 4 * kBar), note(4, 6 * kBar, kBar)})}}})));
  tl.apply(cmd(e::TransportCommandKind::Play, 0));
  int64_t clock = 0;
  std::vector<float> out;
  auto block = [&](uint32_t n) {
    tl.process(clock, n);
    const e::EventList l = tl.events(0);
    e::ProcessContext ctx;
    ctx.frames = n;
    ctx.sampleRate = kSr;
    ctx.sampleTime = clock;
    ctx.events = &l;
    ve.process(ctx);
    clock += n;
  };
  const pq::TempoMap tm;
  while (clock < tm.ticksToSamples(kBar + kPpq, kSr)) block(64);
  CHECK(heldLanes(ve) == 3);
  CHECK(tl.soundingNotes(0) == 3);

  // Stop: every sounding note is released at frame 0 of the next block; Continue resumes at the next tick.
  tl.apply(cmd(e::TransportCommandKind::Stop));
  const Ticks held = tl.position();
  block(64);
  CHECK(heldLanes(ve) == 0);
  CHECK(tl.soundingNotes(0) == 0);
  for (int i = 0; i < 10; ++i) block(64);
  CHECK(tl.position() == held);
  CHECK(tl.events(0).count == 0);
  tl.apply(cmd(e::TransportCommandKind::Continue));
  block(64);
  CHECK(tl.position() > held);
  CHECK(heldLanes(ve) == 0);  // notes that started before are not chased

  // Locate back to 0 (notes 1 and 2 start again at ticks 0 / 100), then, while they sound, far ahead: every
  // sounding voice is released at once.
  tl.apply(cmd(e::TransportCommandKind::Locate, 0));
  block(64);
  block(64);
  CHECK(heldLanes(ve) == 2);
  tl.apply(cmd(e::TransportCommandKind::Locate, 5 * kBar));
  block(64);
  CHECK(heldLanes(ve) == 0);
  CHECK(tl.soundingNotes(0) == 0);
  CHECK(tl.position() > 5 * kBar);
  CHECK(ve.stats().unmatchedEvents == 0);
}

TEST_CASE("timeline: swapping the image mid-note keeps unchanged notes and stops removed / changed ones next block") {
  const pq::TempoMap tm;
  e::ClipDesc a = clip(1, 0, 8 * kBar,
                       {note(1, 0, 4 * kBar), note(2, 0, 4 * kBar), note(3, kBar, kBar), note(5, 0, 4 * kBar)});
  e::ClipDesc bClip = clip(1, 0, 8 * kBar, {note(1, 0, 4 * kBar), note(3, kBar, kBar, 450.0f), note(4, 0, kBar),
                                            note(5, 0, 2 * kBar)});
  e::Timeline tl(config());
  REQUIRE(tl.publishSequence(image({{7, {a}}})));
  tl.apply(cmd(e::TransportCommandKind::Play, 0));
  Driver d{tl};
  const int64_t mid = tm.ticksToSamples(kBar / 2, kSr) + 13;
  d.run(mid, 37);
  CHECK(tl.soundingNotes(0) == 3);
  const size_t before = d.events.size();
  REQUIRE(tl.publishSequence(image({{7, {bClip}}})));
  d.run(64, 64);  // the first block of the new image
  // Exactly one event: note 2's off at frame 0.
  REQUIRE(d.events.size() == before + 1);
  CHECK(d.events.back().e.kind == e::NoteEventKind::NoteOff);
  CHECK(d.events.back().t == d.clock - 64);
  CHECK(tl.stats().keptOnSwap == 2);
  CHECK(tl.stats().stoppedOnSwap == 1);

  // Swap again with the track moved to slot 1 behind a new track (the general remap path).
  const size_t before2 = d.events.size();
  REQUIRE(tl.publishSequence(image({{9, {}}, {7, {bClip}}})));
  d.run(64, 64);
  CHECK(d.events.size() == before2);  // nothing stopped
  CHECK(tl.soundingNotes(1) == 2);
  CHECK(tl.soundingNotes(0) == 0);
  d.run(tm.ticksToSamples(5 * kBar, kSr) - d.clock, 480);

  const auto p = played(d.events);
  auto find = [&](float hz) {
    std::vector<Played> r;
    for (const Played& x : p)
      if (x.hz == hz) r.push_back(x);
    return r;
  };
  REQUIRE(find(101.0f).size() == 1);
  CHECK(find(101.0f)[0].off == tm.ticksToSamples(4 * kBar, kSr));  // kept through both swaps, off in slot 1
  CHECK(find(101.0f)[0].track == 0);                                // (started in slot 0)
  REQUIRE(find(102.0f).size() == 1);
  CHECK(find(102.0f)[0].off == mid);                                 // removed: off at the swap block
  CHECK(find(103.0f).empty());                                       // changed before it started
  REQUIRE(find(450.0f).size() == 1);
  CHECK(find(450.0f)[0].on == tm.ticksToSamples(kBar, kSr));
  CHECK(find(104.0f).empty());                                       // added under the playhead: not chased
  REQUIRE(find(105.0f).size() == 1);
  CHECK(find(105.0f)[0].off == tm.ticksToSamples(2 * kBar, kSr));   // shortened: the new length
}

TEST_CASE("timeline: inactive clips and disabled notes emit nothing; tracks route to their own event slots") {
  e::ClipDesc off = clip(2, kBar, kBar, {note(2, 0, 10)});
  off.active = false;
  e::ClipDesc on = clip(1, 0, kBar, {note(1, 0, 10), note(3, 20, 10)});
  on.notes[1].enabled = false;
  e::Timeline tl(config());
  REQUIRE(tl.publishSequence(image({{1, {on, off}}, {2, {clip(3, 0, kBar, {note(9, 5, 10)})}}})));
  tl.apply(cmd(e::TransportCommandKind::Play, 0));
  Driver d{tl};
  d.run(pq::TempoMap().ticksToSamples(3 * kBar, kSr), 128);
  const auto p = played(d.events);
  REQUIRE(p.size() == 2);
  CHECK(p[0].hz == 101.0f);
  CHECK(p[0].track == 0);
  CHECK(p[1].hz == 109.0f);
  CHECK(p[1].track == 1);
}

TEST_CASE("timeline: chance and velocity range decide per note and pass, reproducibly") {
  e::ClipDesc c = clip(1, 0, 400 * kPpq, {note(1, 0, 10)});
  c.loopEnabled = true;
  c.loopStart = 0;
  c.loopEnd = kPpq;  // 400 passes
  c.notes[0].chance = 0.5f;
  c.notes[0].velocity = 60.0f;
  c.notes[0].velocityRange = 40.0f;
  auto runOnce = [&](uint64_t seed) {
    e::Timeline tl(config());
    REQUIRE(tl.publishSequence(image({{1, {c}}}, seed)));
    tl.apply(cmd(e::TransportCommandKind::Play, 0));
    Driver d{tl};
    d.run(pq::TempoMap().ticksToSamples(400 * kPpq, kSr), 480);
    return played(d.events);
  };
  const auto a = runOnce(5), b = runOnce(5), other = runOnce(6);
  CHECK(a.size() > 160);
  CHECK(a.size() < 240);
  REQUIRE(a.size() == b.size());
  bool differs = a.size() != other.size();
  for (size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].on == b[i].on);
    CHECK(a[i].velocity == b[i].velocity);
    CHECK(a[i].velocity >= 60.0f / 127.0f - 1e-6f);
    CHECK(a[i].velocity <= 100.0f / 127.0f + 1e-6f);
    if (i < other.size() && other[i].on != a[i].on) differs = true;
  }
  CHECK(differs);
  std::set<float> vels;
  for (const auto& x : a) vels.insert(x.velocity);
  CHECK(vels.size() > 50);
}

TEST_CASE("timeline: capacities — sounding overflow cuts the earliest note, a full event list drops note-ons only") {
  e::TimelineConfig cfg = config();
  cfg.maxSoundingPerTrack = 4;
  cfg.maxEventsPerTrack = 16;
  std::vector<e::NoteDesc> notes;
  for (uint32_t i = 0; i < 40; ++i) notes.push_back(note(i + 1, 0, kBar));  // 40 notes on one tick
  for (uint32_t i = 0; i < 6; ++i) notes.push_back(note(100 + i, kBar + 1000 * i, 4 * kBar));
  e::Timeline tl(cfg);
  REQUIRE(tl.publishSequence(image({{1, {clip(1, 0, 8 * kBar, notes)}}})));
  tl.apply(cmd(e::TransportCommandKind::Play, 0));
  Driver d{tl};
  d.run(pq::TempoMap().ticksToSamples(8 * kBar, kSr), 256);
  CHECK(tl.stats().droppedNoteOns > 0);
  CHECK(tl.stats().earlyOffs > 0);
  CHECK(tl.soundingNotes(0) == 0);
  const auto p = played(d.events);  // every note-on got exactly one note-off (no stuck notes)
  for (const auto& x : p) CHECK(x.off >= x.on);
}

// ---- tempo and transport reports --------------------------------------------------------------------------------

TEST_CASE("timeline: a new tempo map keeps the musical position; reports and snapshot follow") {
  e::Timeline tl(config());
  REQUIRE(tl.publishSequence(image({{1, {clip(1, 0, 8 * kBar, {note(1, 2 * kBar, 10)})}}})));
  REQUIRE(tl.post(cmd(e::TransportCommandKind::Play, 0)));
  Driver d{tl};
  const pq::TempoMap t120;
  d.run(t120.ticksToSamples(kBar, kSr), 64);
  const Ticks at = tl.position();
  e::TransportReport r;
  uint32_t reports = 0;
  bool sawPlaying = false;
  while (tl.poll(r)) {
    ++reports;
    sawPlaying = sawPlaying || r.playing;
  }
  CHECK(reports > 0);
  CHECK(sawPlaying);
  CHECK(tl.stats().droppedReports > 0);  // 750 blocks into a 64-entry ring: lossy
  CHECK(tl.snapshot().bpm == 120.0);
  CHECK(tl.snapshot().playing);

  auto t60 = tempoImage({{0, 60.0, pq::TempoCurve::Step}}, {{0, {3, 4}}});
  const pq::TempoMap tm60 = t60->tempo();
  REQUIRE(tl.publishTempo(std::move(t60)));
  const int64_t swapClock = d.clock;
  d.run(64, 64);
  CHECK(tl.snapshot().bpm == 60.0);
  CHECK(tl.snapshot().beatsPerBar == 3.0);
  CHECK(tl.snapshot().beat == doctest::Approx(static_cast<double>(at) / kPpq).epsilon(1e-5));
  CHECK(tl.snapshot().beatsPerFrame == doctest::Approx(1.0 / kSr));
  while (tl.poll(r)) {
  }
  d.run(tm60.ticksToSamples(3 * kBar, kSr), 480);
  const auto p = played(d.events);
  REQUIRE(p.size() == 1);
  CHECK(p[0].on == swapClock + tm60.ticksToSamples(2 * kBar, kSr) - tm60.ticksToSamples(at, kSr));
  REQUIRE(tl.poll(r));
  CHECK(r.bpm == 60.0);
  CHECK(r.playing);
}

// ---- automation -------------------------------------------------------------------------------------------------

namespace {

std::vector<e::AutomationLaneDesc> automationLanes() {
  std::vector<e::AutomationLaneDesc> lanes(5);
  lanes[0].target = 0;
  lanes[0].points = {{kPpq / 2, 0.0f, 0.0f},        {kBar, 1.0f, 0.7f},           {kBar, 0.2f, -0.4f},
                     {2 * kBar + 5, 0.9f, 3e-6f},   {2 * kBar + 999, 0.1f, 0.0f}, {3 * kBar, 0.1f, -1.0f},
                     {3 * kBar + 7, 0.6f, 1.0f},    {4 * kBar, -0.5f, 0.0f}};
  for (Ticks t = 4 * kBar + 50; t < 5 * kBar; t += 211)  // dense: several points per block
    lanes[0].points.push_back({t, static_cast<float>(std::sin(static_cast<double>(t) * 1e-3)), (t % 3 == 0) ? 0.3f : 0.0f});
  lanes[1].target = 1;
  lanes[1].points = {{0, 0.5f, 0.0f}};  // constant
  lanes[2].target = 2;
  lanes[2].emptyValue = 0.25f;          // empty lane
  lanes[3].target = 3;                  // a wide-range Hz-like target with strong curves
  lanes[3].points = {{0, 20.0f, 1.0f}, {2 * kBar, 20000.0f, -0.8f}, {5 * kBar, 200.0f, 0.0f}};
  lanes[4].target = 9;                  // beyond the param block
  lanes[4].points = {{kBar, 3.0f, 0.0f}, {2 * kBar, 4.0f, 0.0f}};
  return lanes;
}

void checkAutomation(e::Timeline& tl, const std::vector<e::AutomationLaneDesc>& lanes, uint32_t block,
                     const pq::TempoMap& tm, int64_t frames) {
  int64_t clock = 0;
  double worst = 0.0;
  while (clock < frames) {
    const uint32_t n = static_cast<uint32_t>(std::min<int64_t>(block, frames - clock));
    const int64_t song = tl.songSample();
    tl.process(clock, n);
    const auto& ar = tl.automation();
    REQUIRE(ar.numLanes() == lanes.size());
    for (uint32_t l = 0; l < lanes.size(); ++l) {
      const float* buf = ar.buffer(l);
      for (uint32_t i = 0; i < n; ++i) {
        const double want = lanes[l].points.empty()
                                ? lanes[l].emptyValue
                                : e::evalAutomation(std::span<const e::AutomationPoint>(lanes[l].points),
                                                    tm.samplesToTicks(song + i, kSr));
        const double err = std::abs(buf[i] - want) / std::max(1.0, std::abs(want));
        worst = std::max(worst, err);
        if (err > 1e-6) {
          CAPTURE(l);
          CAPTURE(song + i);
          CHECK(buf[i] == doctest::Approx(want));
          return;
        }
      }
    }
    clock += n;
  }
  MESSAGE("block ", block, ": worst relative error ", worst);
  CHECK(worst <= 1e-6);
}

}  // namespace

TEST_CASE("automation: buffers match evalAutomation within 1e-6 per sample (constant tempo and ramps, any block)") {
  const auto lanes = automationLanes();
  for (bool ramps : {false, true}) {
    for (uint32_t block : {1u, 37u, 480u}) {
      CAPTURE(ramps);
      CAPTURE(block);
      e::Timeline tl(config());
      std::vector<pq::TempoPoint> tp{{0, 120.0, pq::TempoCurve::Step}};
      if (ramps) tp = {{0, 100.0, pq::TempoCurve::Linear}, {2 * kBar, 170.0, pq::TempoCurve::Step},
                       {3 * kBar, 80.0, pq::TempoCurve::Linear}, {6 * kBar, 81.0, pq::TempoCurve::Step}};
      auto ti = tempoImage(tp);
      const pq::TempoMap tm = ti->tempo();
      REQUIRE(tl.publishTempo(std::move(ti)));
      auto ai = e::AutomationImage::build(lanes);
      REQUIRE(ai);
      REQUIRE(tl.publishAutomation(std::move(ai.value())));
      tl.apply(cmd(e::TransportCommandKind::Play, 0));
      checkAutomation(tl, lanes, block, tm, tm.ticksToSamples(6 * kBar, kSr));
    }
  }
}

TEST_CASE("automation: all-constant blocks are skipped, and still pick up the next jump / ramp exactly") {
  // Every lane flat for long stretches (the O(1) whole-block skip), then a jump, a flat run, a ramp.
  std::vector<e::AutomationLaneDesc> lanes(3);
  lanes[0].points = {{kBar, 0.0f, 0.0f}, {kBar, 1.0f, 0.0f}, {3 * kBar, 1.0f, 0.0f}, {4 * kBar, 0.0f, 0.5f}};
  lanes[1].points = {{2 * kBar + 17, 0.5f, 0.0f}, {2 * kBar + 17, -0.5f, 0.0f}};
  lanes[2].target = 2;
  lanes[2].emptyValue = 7.0f;
  for (uint32_t block : {1u, 64u, 333u}) {
    CAPTURE(block);
    e::Timeline tl(config());
    auto ai = e::AutomationImage::build(lanes);
    REQUIRE(ai);
    REQUIRE(tl.publishAutomation(std::move(ai.value())));
    tl.apply(cmd(e::TransportCommandKind::Play, 0));
    const pq::TempoMap tm;
    checkAutomation(tl, lanes, block, tm, tm.ticksToSamples(5 * kBar, kSr));
  }
  // With a loop: the wrap jumps back over the jump at bar 1 (the skip must not survive a backwards move).
  e::Timeline tl(config());
  auto ai = e::AutomationImage::build(lanes);
  REQUIRE(ai);
  REQUIRE(tl.publishAutomation(std::move(ai.value())));
  tl.apply(loopCmd(kBar / 2, 2 * kBar));
  tl.apply(cmd(e::TransportCommandKind::Play, kBar / 2));
  const pq::TempoMap tm;
  const int64_t LS = tm.ticksToSamples(kBar / 2, kSr), LE = tm.ticksToSamples(2 * kBar, kSr);
  int64_t song = LS, clock = 0;
  for (int b = 0; b < 6000; ++b) {  // 768000 samples: 5 wraps of the 144000-sample loop
    tl.process(clock, 128);
    for (uint32_t i = 0; i < 128; ++i, ++song) {
      if (song == LE) song = LS;
      const double want = e::evalAutomation(std::span<const e::AutomationPoint>(lanes[0].points), tm.samplesToTicks(song, kSr));
      REQUIRE(tl.automation().buffer(0)[i] == static_cast<float>(want));
    }
    clock += 128;
  }
  CHECK(tl.stats().loopWraps >= 3);
}

TEST_CASE("automation: across a loop wrap, while stopped (value at the playhead), and into the param block") {
  const auto lanes = automationLanes();
  e::Timeline tl(config());
  auto ai = e::AutomationImage::build(lanes);
  REQUIRE(ai);
  REQUIRE(tl.publishAutomation(std::move(ai.value())));
  const pq::TempoMap tm;
  tl.apply(loopCmd(kBar - 3, 2 * kBar + 11));
  tl.apply(cmd(e::TransportCommandKind::Play, kBar / 2));
  const int64_t LS = tm.ticksToSamples(kBar - 3, kSr), LE = tm.ticksToSamples(2 * kBar + 11, kSr);
  int64_t song = tm.ticksToSamples(kBar / 2, kSr);
  int64_t clock = 0;
  double worst = 0.0;
  for (int b = 0; b < 1500; ++b) {
    tl.process(clock, 333);
    for (uint32_t i = 0; i < 333; ++i) {
      if (song == LE) song = LS;
      for (uint32_t l = 0; l < lanes.size(); ++l) {
        const double want = lanes[l].points.empty() ? lanes[l].emptyValue
                                                    : e::evalAutomation(std::span<const e::AutomationPoint>(lanes[l].points),
                                                                        tm.samplesToTicks(song, kSr));
        const double err = std::abs(tl.automation().buffer(l)[i] - want) / std::max(1.0, std::abs(want));
        worst = std::max(worst, err);
      }
      ++song;
    }
    clock += 333;
  }
  CHECK(tl.stats().loopWraps >= 2);
  CHECK(worst <= 1e-6);

  std::vector<float> params(4, -1.0f);
  tl.writeAutomatedParams(params);
  for (uint32_t l = 0; l < 4; ++l) CHECK(params[lanes[l].target] == tl.automation().buffer(l)[0]);

  // Stopped: every lane holds its value at the playhead; constant() reports it.
  tl.apply(cmd(e::TransportCommandKind::Locate, 3 * kBar + 3));
  tl.apply(cmd(e::TransportCommandKind::Stop));
  tl.process(clock, 100);
  clock += 100;
  tl.process(clock, 512);
  for (uint32_t l = 0; l < lanes.size(); ++l) {
    const double want = lanes[l].points.empty() ? lanes[l].emptyValue
                                                : e::evalAutomation(std::span<const e::AutomationPoint>(lanes[l].points),
                                                                    static_cast<double>(3 * kBar + 3));
    CHECK(tl.automation().constant(l));
    for (uint32_t i = 0; i < 512; ++i) REQUIRE(tl.automation().buffer(l)[i] == static_cast<float>(want));
  }
}

// ---- RT safety ----------------------------------------------------------------------------------------------------

TEST_CASE("timeline: RtScope-clean with 64 tracks x 200 clips, loops, swaps, locates and automation") {
  e::TimelineConfig cfg;
  cfg.sampleRate = kSr;
  cfg.maxBlockFrames = 256;
  cfg.maxTracks = 64;
  cfg.maxAutomationLanes = 64;
  auto descs = [](uint32_t variant) {
    std::vector<e::TrackSequenceDesc> tracks(64);
    uint32_t id = 1;
    for (uint32_t k = 0; k < 64; ++k) {
      tracks[k].id = 1000 + k;
      for (uint32_t c = 0; c < 200; ++c) {
        e::ClipDesc cd = clip(c, static_cast<Ticks>(c) * kBar, kBar, {});
        if ((c + k) % 3 == 0) {
          cd.loopEnabled = true;
          cd.loopStart = 0;
          cd.loopEnd = kPpq;
        }
        for (uint32_t i = 0; i < 4; ++i) {
          e::NoteDesc n = note(id++, static_cast<Ticks>(i) * kPpq + (k * 1301 + (k % 2 == 0 ? variant : 0)) % kPpq, kPpq / 2 + i * 300);
          n.chance = i == 3 ? 0.6f : 1.0f;
          n.velocityRange = 20.0f;
          cd.notes.push_back(n);
        }
        tracks[k].clips.push_back(std::move(cd));
      }
    }
    return tracks;
  };
  e::Timeline tl(cfg);
  REQUIRE(tl.publishSequence(image(descs(0))));
  std::vector<e::AutomationLaneDesc> lanes(32);
  for (uint32_t l = 0; l < 32; ++l) {
    lanes[l].target = l;
    for (Ticks t = 0; t < 200 * kBar; t += kBar / 2 + l * 31) lanes[l].points.push_back({t, static_cast<float>(t % 7), 0.3f});
  }
  auto ai = e::AutomationImage::build(lanes);
  REQUIRE(ai);
  REQUIRE(tl.publishAutomation(std::move(ai.value())));
  REQUIRE(tl.publishTempo(tempoImage({{0, 128.0, pq::TempoCurve::Linear}, {100 * kBar, 90.0, pq::TempoCurve::Step}})));
  REQUIRE(tl.post(loopCmd(10 * kBar, 12 * kBar + 5)));
  REQUIRE(tl.post(cmd(e::TransportCommandKind::Play, 9 * kBar)));
  std::vector<float> params(32);
  int64_t clock = 0;
  uint64_t events = 0;
  auto blocks = [&](int count) {
    return pq::alloc_trap::countRtViolations([&] {
      for (int b = 0; b < count; ++b) {
        tl.process(clock, 256);
        for (uint32_t k = 0; k < 64; ++k) events += tl.events(k).count;
        tl.writeAutomatedParams(params);
        clock += 256;
      }
    });
  };
  CHECK(blocks(1500) == 0);
  uint32_t soundingAtSwap = 0;
  for (uint32_t k = 0; k < 64; ++k) soundingAtSwap += tl.soundingNotes(k);
  CHECK(soundingAtSwap > 0);
  REQUIRE(tl.publishSequence(image(descs(1))));  // swap mid-notes (built and published off the RT scope)
  CHECK(blocks(300) == 0);
  REQUIRE(tl.post(cmd(e::TransportCommandKind::Locate, 150 * kBar)));
  REQUIRE(tl.post(loopCmd(0, 0, false)));
  CHECK(blocks(300) == 0);
  REQUIRE(tl.post(cmd(e::TransportCommandKind::Stop)));
  CHECK(blocks(10) == 0);
  CHECK(events > 1000);
  CHECK(tl.stats().loopWraps > 0);
  CHECK(tl.stats().keptOnSwap + tl.stats().stoppedOnSwap > 0);
  tl.collect();
}

// ---- integration --------------------------------------------------------------------------------------------------

TEST_CASE("renderOffline with the timeline (hook + event source) equals the same notes as a note list, bit for bit") {
  std::vector<e::NoteDesc> notes;
  uint32_t id = 1;
  for (Ticks t = 0; t < 3 * kBar; t += kPpq / 3 + 101) notes.push_back(note(id++, t, kPpq / 2, 200.0f + static_cast<float>(t % 500)));
  auto tempo = tempoImage({{0, 123.0, pq::TempoCurve::Linear}, {2 * kBar, 97.0, pq::TempoCurve::Step}});
  const pq::TempoMap tm = tempo->tempo();
  const uint64_t frames = static_cast<uint64_t>(tm.ticksToSamples(4 * kBar, kSr));

  Built b1 = build(kSine, 128, 3);
  e::VoiceEngine v1;
  e::VoiceConfig vc;
  vc.maxVoices = 16;
  REQUIRE(v1.prepare(b1.image.get(), vc));
  e::Timeline tl(config());
  REQUIRE(tl.publishTempo(std::move(tempo)));
  REQUIRE(tl.publishSequence(image({{1, {clip(1, 0, 8 * kBar, notes)}}})));
  tl.apply(cmd(e::TransportCommandKind::Play, 0));
  e::RenderRequest rq;
  rq.image = b1.image.get();
  rq.frames = frames;
  rq.blockFrames = 100;
  rq.voices = &v1;
  rq.events = tl.eventSource(0);
  rq.hook = tl.blockHook();
  e::BufferSink s1;
  REQUIRE(e::renderOffline(rq, s1));  // RtScope around the block loop: the timeline path allocates nothing

  std::vector<e::TimedNote> list;
  for (const auto& n : notes) {
    const int64_t on = tm.ticksToSamples(n.start, kSr);
    list.push_back({on, tm.ticksToSamples(n.start + n.length, kSr) - on, n.hz, n.velocity * (1.0f / 127.0f),
                    n.releaseVelocity * (1.0f / 127.0f)});
  }
  e::NoteListSource src(list);
  Built b2 = build(kSine, 128, 3);
  e::VoiceEngine v2;
  REQUIRE(v2.prepare(b2.image.get(), vc));
  rq.image = b2.image.get();
  rq.voices = &v2;
  rq.events = src.source();
  rq.hook = {};
  e::BufferSink s2;
  REQUIRE(e::renderOffline(rq, s2));
  REQUIRE(s1.channels.size() == 1);
  REQUIRE(s1.channels[0].size() == s2.channels[0].size());
  size_t diffs = 0;
  double energy = 0;
  for (size_t i = 0; i < s1.channels[0].size(); ++i) {
    diffs += s1.channels[0][i] != s2.channels[0][i] ? 1u : 0u;
    energy += std::abs(s1.channels[0][i]);
  }
  CHECK(diffs == 0);
  CHECK(energy > 100.0);
  CHECK(v1.stats().noteOns == notes.size());
}

TEST_CASE("timeline: the transport snapshot reaches ProcessContext (transport.beat / barPhase) through the hook") {
  Built b = build(kBeat, 128);
  e::Timeline tl(config());
  REQUIRE(tl.publishTempo(tempoImage({{0, 90.0, pq::TempoCurve::Step}}, {{0, {7, 8}}})));
  std::vector<e::AutomationLaneDesc> lanes(1);
  lanes[0].target = 0;  // the `gain` param
  lanes[0].points = {{0, 2.0f, 0.0f}};
  auto ai = e::AutomationImage::build(lanes);
  REQUIRE(ai);
  REQUIRE(tl.publishAutomation(std::move(ai.value())));
  tl.apply(cmd(e::TransportCommandKind::Play, kBar));
  e::RenderRequest rq;
  rq.image = b.image.get();
  rq.frames = 48000;
  rq.blockFrames = 128;
  rq.hook = tl.blockHook();
  e::BufferSink s;
  REQUIRE(e::renderOffline(rq, s));
  REQUIRE(s.channels.size() == 2);
  // transport.beat · gain (gain automated to 2): beats run from 4 at 90 BPM = 1.5 beats per second.
  const auto& y = s.channels[0];
  CHECK(y[0] == doctest::Approx(2.0 * 4.0).epsilon(1e-6));
  CHECK(y[47999] == doctest::Approx(2.0 * (4.0 + 1.5 * 47999.0 / kSr)).epsilon(1e-5));
  // 7/8: a bar is 3.5 quarter notes; bar phase of beat 4 is (4 − 3.5) / 3.5.
  CHECK(s.channels[1][0] == doctest::Approx(0.5 / 3.5).epsilon(1e-5));
}
