// T3.16: the engine timeline — transport (play/stop/continue/locate, loop wrap, tempo map), the note sequencer
// (SequenceImage, lazily unrolled clip loops, chance / velocity range, image swaps), the automation renderer
// (evalAutomation, per-sample buffers), and the integration with the VoiceEngine and renderOffline.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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
  // A note's off is at least one sample after its on (T3.17-fix3, M3 review L4: a note shorter than a sample used to
  // end on its own onset sample and never sounded; this test expected that).
  for (const auto& n : notes) {
    const int64_t on = tm.ticksToSamples(n.start, kSr);
    want.push_back({on, std::max(tm.ticksToSamples(n.start + n.length, kSr), on + 1), 0, n.hz, 0.0f});
  }
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
          // ≥ 1 sample long (T3.17-fix3, M3 review L4: the 5-tick note used to end on its onset sample).
          const int64_t onS = tm.ticksToSamples(on, kSr);
          want.push_back({onS, std::max(tm.ticksToSamples(off, kSr), onS + 1), 0, n.hz, 0.0f});
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

TEST_CASE("automation: block-rate lanes give the same param values; taps carry moving buffers (T2.6-fix2)") {
  // M3 review M6: a lane nobody reads per sample renders one value per block (AutomationLaneDesc::perSample false),
  // the same frame-0 value the per-sample render gives, so the param block is unchanged; a lane with an audio-rate
  // consumer (AutomationLaneDesc::tap) hands its buffer over while the block moves and null while it is constant.
  const auto lanes = automationLanes();
  std::vector<e::AutomationLaneDesc> blockLanes = lanes, tapped = lanes;
  for (e::AutomationLaneDesc& l : blockLanes) l.perSample = false;
  const float* taps[5] = {};
  for (size_t l = 0; l < tapped.size(); ++l) {
    tapped[l].perSample = false;  // a tap renders per sample anyway
    tapped[l].tap = &taps[l];
  }
  for (uint32_t block : {37u, 64u, 480u}) {
    CAPTURE(block);
    e::Timeline a(config()), b(config()), c(config());
    for (auto* p : {&a, &b, &c}) {
      p->apply(loopCmd(kBar - 3, 4 * kBar + 11));
      p->apply(cmd(e::TransportCommandKind::Play, kBar / 2));
    }
    REQUIRE(a.publishAutomation(std::move(e::AutomationImage::build(lanes).value())));
    REQUIRE(b.publishAutomation(std::move(e::AutomationImage::build(blockLanes).value())));
    REQUIRE(c.publishAutomation(std::move(e::AutomationImage::build(tapped).value())));
    std::vector<float> pa(16, 0.0f), pb(16, 0.0f), pc(16, 0.0f);
    uint64_t moving = 0, still = 0;
    for (int64_t clock = 0; clock < 600000; clock += block) {
      a.process(clock, block);
      b.process(clock, block);
      c.process(clock, block);
      a.writeAutomatedParams(pa);
      b.writeAutomatedParams(pb);
      c.writeAutomatedParams(pc);
      for (size_t i = 0; i < pa.size(); ++i) {
        REQUIRE(pb[i] == pa[i]);
        REQUIRE(pc[i] == pa[i]);
      }
      for (uint32_t l = 0; l < lanes.size(); ++l) {
        if (c.automation().constant(l)) {
          REQUIRE(taps[l] == nullptr);
          ++still;
        } else {
          REQUIRE(taps[l] == c.automation().buffer(l));
          for (uint32_t i = 0; i < block; ++i) REQUIRE(taps[l][i] == a.automation().buffer(l)[i]);
          ++moving;
        }
      }
    }
    CHECK(moving > 0);
    CHECK(still > 0);
    // A new image releases the old one's taps (its consumers read their params again).
    REQUIRE(c.publishAutomation(std::move(e::AutomationImage::build(lanes).value())));
    c.process(600000 + block, block);
    for (const float* t : taps) CHECK(t == nullptr);
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

// ---- T7.1: per-note pitch and gain curves ----------------------------------------------------------------------------

namespace {

e::NoteDesc curved(e::NoteDesc n, std::vector<e::CurvePoint> pitch, std::vector<e::CurvePoint> gain = {}) {
  n.pitchCurve = std::move(pitch);
  n.gainCurve = std::move(gain);
  return n;
}

// The gate as 0 / 1: the mix is the lane gain.
const char* kGate = R"(patch Gate {
  out y : audio voice
  y = gate_to_real(voice.gate)
}
)";

// A curve as the engine should play it: breakpoints in engine samples (two at one sample: a jump).
struct Seg {
  double s, v;
};
double valueAt(const std::vector<Seg>& p, double t) {
  if (p.empty()) return 0.0;
  if (t < p.front().s) return p.front().v;
  for (size_t i = p.size(); i-- > 0;) {
    if (p[i].s > t) continue;
    if (i + 1 == p.size() || p[i].s == t) return p[i].v;
    return p[i].v + (p[i + 1].v - p[i].v) * (t - p[i].s) / (p[i + 1].s - p[i].s);
  }
  return p.front().v;
}
// Cycles of hz · 2^(cents(t) / 1200) over the samples [a, b] (a continuous cents curve).
double cyclesBetween(const std::vector<Seg>& p, double hz, double a, double b) {
  double sum = 0.0, t = a;
  while (t < b) {
    double nb = b;
    for (const Seg& s : p)
      if (s.s > t) {
        nb = std::min(nb, s.s);
        break;
      }
    const double c0 = valueAt(p, t), c1 = valueAt(p, nb);
    const double f0 = hz * std::exp2(c0 / 1200.0), f1 = hz * std::exp2(c1 / 1200.0);
    const double k = (c1 - c0) / (nb - t);  // cents per sample
    sum += std::fabs(c1 - c0) < 1e-12 ? f0 * (nb - t) : (f1 - f0) * 1200.0 / (k * std::log(2.0));
    t = nb;
  }
  return sum / kSr;
}
double cents(double got, double want) { return 1200.0 * std::log2(got / want); }

// The zero-crossing estimator: the frequency a rendered sine shows over every run of m cycles (between upward zero
// crossings, linearly interpolated), against the analytic curve: the worst difference in cents. The phasor's output
// at sample t is the sum of the increments BEFORE t (dsp/atoms.h), i.e. the integral of the frequency up to t − ½.
double worstCrossingCents(const std::vector<float>& y, int64_t from, int64_t to, const std::vector<Seg>& curve,
                          double hz, int m, int* windows) {
  std::vector<double> tau;
  for (int64_t i = from; i + 1 < to; ++i) {
    const double a = y[static_cast<size_t>(i)], b = y[static_cast<size_t>(i) + 1];
    if (a < 0.0 && b >= 0.0) tau.push_back(static_cast<double>(i) + (0.0 - a) / (b - a));
  }
  double worst = 0.0;
  *windows = 0;
  for (size_t k = 0; k + static_cast<size_t>(m) < tau.size(); k += static_cast<size_t>(m)) {
    const double cycles = cyclesBetween(curve, hz, tau[k] - 0.5, tau[k + static_cast<size_t>(m)] - 0.5);
    worst = std::max(worst, std::fabs(cents(static_cast<double>(m), cycles)));
    ++*windows;
  }
  return worst;
}

float hzAt(float hz, float c) { return static_cast<float>(static_cast<double>(hz) * std::exp2(static_cast<double>(c) * (1.0 / 1200.0))); }

// The timeline's track 0 into a VoiceEngine: per sample the mix of voice out 0, and lane `lane`'s voice.hz and gain.
struct CurveRig {
  Built b;
  e::VoiceEngine ve;
  e::Timeline tl;
  int64_t clock = 0;
  uint32_t lane = 0;
  std::vector<float> mix, hz, gain;
  std::vector<Ev> events;
  uint64_t rtViolations = 0;

  explicit CurveRig(const char* patch, const e::TimelineConfig& cfg = config(), uint32_t maxVoices = 16, uint32_t groups = 3,
                    e::VoiceConfig vc = {})
      : b(build(patch, cfg.maxBlockFrames, groups)), tl(cfg) {
    vc.maxVoices = maxVoices;
    const auto st = ve.prepare(b.image.get(), vc);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? "" : st.error().message));
  }
  template <class F, class G>
  void run(int64_t frames, uint32_t block, F&& before, G&& after) {
    for (int64_t done = 0; done < frames;) {
      const uint32_t n = static_cast<uint32_t>(std::min<int64_t>(block, frames - done));
      before(clock);
      e::EventList list;
      e::ProcessContext ctx;
      ctx.frames = n;
      ctx.sampleRate = kSr;
      ctx.sampleTime = clock;
      rtViolations += pq::alloc_trap::countRtViolations([&] {  // RtScope on both process() calls
        tl.process(clock, n);
        list = tl.events(0);
        ctx.events = &list;
        ve.process(ctx);
      });
      for (uint32_t i = 0; i < list.count; ++i) {
        REQUIRE(list.events[i].frame < n);
        if (i > 0) REQUIRE(list.events[i].frame >= list.events[i - 1].frame);
        events.push_back({clock + list.events[i].frame, 0, list.events[i]});
      }
      mix.insert(mix.end(), ve.mix(0), ve.mix(0) + n);
      const float* h = ve.signal(lane >> 3, e::VoiceSignal::Hz);
      const float* g = ve.gain(lane >> 3);
      for (uint32_t f = 0; f < n; ++f) {
        hz.push_back(h[f * 8 + (lane & 7u)]);
        gain.push_back(g[f * 8 + (lane & 7u)]);
      }
      after(clock, n);
      clock += n;
      done += n;
    }
  }
  template <class F>
  void run(int64_t frames, uint32_t block, F&& before) {
    run(frames, block, before, [](int64_t, uint32_t) {});
  }
  void run(int64_t frames, uint32_t block) {
    run(frames, block, [](int64_t) {});
  }
  // The worst |cents| of lane voice.hz against hz · 2^(curve / 1200) over the samples [from, to).
  double worstHzCents(int64_t from, int64_t to, const std::vector<Seg>& curve, double base) const {
    double worst = 0.0;
    for (int64_t t = from; t < to; ++t)
      worst = std::max(worst, std::fabs(cents(hz[static_cast<size_t>(t)], base * std::exp2(valueAt(curve, static_cast<double>(t)) / 1200.0))));
    return worst;
  }
  // The worst |dB| of the lane gain against `curve` over [from, to); a sample where the curve is 0 must be exactly 0.
  double worstGainDb(int64_t from, int64_t to, const std::vector<Seg>& curve) const {
    double worst = 0.0;
    for (int64_t t = from; t < to; ++t) {
      const double want = valueAt(curve, static_cast<double>(t)), got = gain[static_cast<size_t>(t)];
      worst = std::max(worst, want == 0.0 ? (got == 0.0 ? 0.0 : 1000.0) : std::fabs(20.0 * std::log10(got / want)));
    }
    return worst;
  }
};

// (engine sample, kind, hz, gain, rampFrames) of every event: what a test compares.
struct Flat {
  int64_t t;
  e::NoteEventKind kind;
  float hz, gain;
  uint32_t ramp;
  bool operator==(const Flat&) const = default;
};
std::vector<Flat> flat(const std::vector<Ev>& evs, int64_t from = INT64_MIN, int64_t to = INT64_MAX, int64_t shift = 0) {
  std::vector<Flat> out;
  for (const Ev& v : evs) {
    if (v.t < from || v.t >= to) continue;
    const bool on = v.e.kind == e::NoteEventKind::NoteOn, p = v.e.kind == e::NoteEventKind::Pitch,
               g = v.e.kind == e::NoteEventKind::Gain;
    out.push_back({v.t - shift, v.e.kind, on || p ? v.e.hz : 0.0f, on || g ? v.e.gain : 0.0f, p || g ? v.e.rampFrames : 0u});
  }
  return out;
}

constexpr int64_t kQ = 24000;  // samples per quarter note at the default 120 BPM, 48 kHz

}  // namespace

TEST_CASE("SequenceImage: note curves — storage, clean-up, curveValue, the hash, errors (T7.1)") {
  CHECK(sizeof(e::SequenceImage::Note) == 72);  // curves cost a note without them nothing
  e::NoteDesc a = curved(note(1, kPpq, 4 * kPpq),
                         {{3 * kPpq, 700.0f}, {kPpq, 0.0f}, {2 * kPpq, 100.0f}, {2 * kPpq, 200.0f}, {2 * kPpq, 300.0f}},  // unsorted, 3 at one tick
                         {{0, 0.5f}, {5 * kPpq, 1.0f}});
  e::NoteDesc plain = note(2, 2 * kPpq, kPpq);
  e::NoteDesc onlyGain = curved(note(3, 3 * kPpq, kPpq), {}, {{3 * kPpq, 0.0f}});
  auto img = image({{1, {clip(1, 0, 8 * kPpq, {a, plain, onlyGain})}}});
  REQUIRE(img->notes().size() == 3);
  const auto& n1 = img->notes()[0];
  const auto& n2 = img->notes()[1];
  const auto& n3 = img->notes()[2];
  REQUIRE(n1.id == 1);
  CHECK(img->curvesOf(n1).pitchCount == 4);  // of the three points at 2·kPpq the first and the last stay
  CHECK(img->curvesOf(n1).gainCount == 2);
  CHECK_FALSE(img->curvesOf(n2).any());
  CHECK(img->curveHash(n2) == 0);
  CHECK(img->pitchCurve(n2).empty());
  CHECK(img->curvesOf(n3).pitchCount == 0);
  CHECK(img->curvesOf(n3).gainCount == 1);
  const auto p = img->pitchCurve(n1);
  const std::vector<e::CurvePoint> want = {{kPpq, 0.0f}, {2 * kPpq, 100.0f}, {2 * kPpq, 300.0f}, {3 * kPpq, 700.0f}};
  CHECK(std::vector<e::CurvePoint>(p.begin(), p.end()) == want);
  CHECK(img->gainCurve(n1).size() == 2);
  CHECK(img->gainCurve(n3)[0].value == 0.0f);
  CHECK(img->curves().size() == 1 + 4 + 2 + 1 + 1);  // a header per curved note
  // curveValue: ends held, linear between, the last point of a tick at a jump, `after` = the first later point.
  uint32_t after = 99;
  CHECK(e::SequenceImage::curveValue(p, 0, 0.0f, &after) == 0.0f);
  CHECK(after == 0);
  CHECK(e::SequenceImage::curveValue(p, kPpq, 0.0f, &after) == 0.0f);
  CHECK(after == 1);
  CHECK(e::SequenceImage::curveValue(p, kPpq + kPpq / 4, 0.0f, &after) == 25.0f);
  CHECK(after == 1);
  CHECK(e::SequenceImage::curveValue(p, 2 * kPpq - 1, 0.0f) == doctest::Approx(100.0).epsilon(1e-5));
  CHECK(e::SequenceImage::curveValue(p, 2 * kPpq, 0.0f, &after) == 300.0f);
  CHECK(after == 3);
  CHECK(e::SequenceImage::curveValue(p, 2 * kPpq + kPpq / 2, 0.0f) == 500.0f);
  CHECK(e::SequenceImage::curveValue(p, 9 * kPpq, 0.0f, &after) == 700.0f);
  CHECK(after == 4);
  CHECK(e::SequenceImage::curveValueAt(p, 1.5 * static_cast<double>(kPpq), 0.0f) == 50.0f);  // a fractional tick
  CHECK(e::SequenceImage::curveValueAt(p, 2.0 * static_cast<double>(kPpq), 0.0f) == 300.0f);
  CHECK(e::SequenceImage::curveValueAt(p, 2.0 * static_cast<double>(kPpq) - 0.5, 0.0f) == doctest::Approx(100.0).epsilon(1e-5));
  CHECK(e::SequenceImage::curveValue({}, 5, 1.0f, &after) == 1.0f);
  CHECK(after == 0);
  // The hash: the same curves (relative to the note's start) hash the same in another image, a change does not.
  auto same = image({{1, {clip(1, 0, 8 * kPpq, {plain, a})}}});
  const e::SequenceImage::Note* s1 = same->findOccurrence(0, 1, kPpq, nullptr);
  REQUIRE(s1 != nullptr);
  CHECK(same->curveHash(*s1) == img->curveHash(n1));
  CHECK(img->curveHash(n1) != 0);
  e::NoteDesc changed = a;
  changed.gainCurve[1].value = 0.9f;
  auto other = image({{1, {clip(1, 0, 8 * kPpq, {changed})}}});
  CHECK(other->curveHash(other->notes()[0]) != img->curveHash(n1));
  // curvesOf over many notes (the bit per note and its rank): every 3rd of 1000 notes has a one-point curve.
  {
    std::vector<e::NoteDesc> lots;
    for (uint32_t i = 0; i < 1000; ++i) {
      e::NoteDesc x = note(i + 1, static_cast<Ticks>(999 - i) * 100, 50);  // descending starts: the build sorts
      if (i % 3 == 0) x.gainCurve = {{0, static_cast<float>(i)}};
      lots.push_back(x);
    }
    auto big = image({{1, {clip(1, 0, kBar, lots)}}});
    REQUIRE(big->notes().size() == 1000);
    size_t wrong = 0;
    for (const auto& x : big->notes()) {
      const uint32_t i = x.id - 1;
      const auto g = big->gainCurve(x);
      wrong += x.start != static_cast<Ticks>(999 - i) * 100 ? 1u : 0u;
      wrong += i % 3 == 0 ? (g.size() != 1 || g[0].value != static_cast<float>(i) ? 1u : 0u) : (big->curvesOf(x).any() ? 1u : 0u);
    }
    CHECK(wrong == 0);
    for (size_t k = 1; k < 1000; ++k) REQUIRE(big->notes()[k - 1].start < big->notes()[k].start);
  }
  // Errors.
  auto bad = [](e::NoteDesc n) { return !e::SequenceImage::build(std::vector<e::TrackSequenceDesc>{{1, {clip(1, 0, kBar, {n})}}}).ok(); };
  CHECK(bad(curved(note(1, 0, kPpq), {{0, std::numeric_limits<float>::quiet_NaN()}})));
  CHECK(bad(curved(note(1, 0, kPpq), {{0, e::kMaxCurveCents + 1.0f}})));
  CHECK(bad(curved(note(1, 0, kPpq), {}, {{0, -0.1f}})));
  CHECK(bad(curved(note(1, 0, kPpq), {}, {{0, std::numeric_limits<float>::infinity()}})));
  std::vector<e::CurvePoint> many;
  for (uint32_t i = 0; i <= e::kMaxCurvePoints; ++i) many.push_back({static_cast<Ticks>(i), 0.0f});
  CHECK(bad(curved(note(1, 0, kPpq), many)));
  many.pop_back();
  CHECK_FALSE(bad(curved(note(1, 0, kPpq), many)));
}

TEST_CASE("timeline: a 0 -> +702 c -> 0 pitch curve tracks the analytic frequency within 0.01 c, blocks 1 / 64 / 512 (T7.1)") {
  // The note: on at sample 12000, flat until 24000, up a just fifth over a quarter, back down over the next, off at
  // 84000. Breakpoints on whole samples, so the analytic curve in samples is exact.
  const Ticks start = kPpq / 2;
  const e::NoteDesc n = curved(note(1, start, 3 * kPpq, 220.0f),
                               {{start + kPpq / 2, 0.0f}, {start + 3 * kPpq / 2, 702.0f}, {start + 5 * kPpq / 2, 0.0f}});
  const std::vector<Seg> curve = {{1.0 * kQ, 0.0}, {2.0 * kQ, 702.0}, {3.0 * kQ, 0.0}};
  const float fifth = hzAt(220.0f, 702.0f);
  std::vector<float> ref;
  for (uint32_t block : {1u, 64u, 512u}) {
    INFO("block " << block);
    CurveRig r(kSine);
    REQUIRE(r.tl.publishSequence(image({{1, {clip(1, 0, 2 * kBar, {n})}}})));
    r.tl.apply(cmd(e::TransportCommandKind::Play, 0));
    r.run(90000, block);
    // The events: the work is per breakpoint (two ramps for the whole curve; the flat lead-in and the end cost none).
    using K = e::NoteEventKind;
    const std::vector<Flat> want = {{12000, K::NoteOn, 220.0f, 1.0f, 0},
                                    {24000, K::Pitch, fifth, 0.0f, 24000},
                                    {48000, K::Pitch, 220.0f, 0.0f, 24000},
                                    {84000, K::NoteOff, 0.0f, 0.0f, 0}};
    CHECK(flat(r.events) == want);
    CHECK(r.tl.stats().curveEvents == 2);
    CHECK(r.tl.stats().droppedCurveEvents == 0);
    // voice.hz per sample against the analytic curve.
    const double worstHz = r.worstHzCents(12000, 84000, curve, 220.0);
    CHECK(worstHz <= 0.01);
    CHECK(r.hz[36000] == doctest::Approx(220.0 * std::exp2(351.0 / 1200.0)).epsilon(1e-6));
    CHECK(r.hz[48000] == fifth);  // each ramp ends exactly on its breakpoint
    CHECK(r.hz[72000] == 220.0f);
    // The rendered sine: its zero crossings give the same frequency.
    int windows = 0;
    const double worstZc = worstCrossingCents(r.mix, 12000, 84000, curve, 220.0, 16, &windows);
    CHECK(windows >= 20);
    CHECK(worstZc <= 0.01);
    CHECK(r.rtViolations == 0);
    CHECK(r.ve.stats().unmatchedEvents == 0);
    if (ref.empty()) ref = r.mix;
    else CHECK(std::memcmp(ref.data(), r.mix.data(), sizeof(float) * ref.size()) == 0);  // the same samples for any block
    MESSAGE("block " << block << ": voice.hz worst " << worstHz << " c; zero crossings worst " << worstZc << " c over "
                     << windows << " windows of 16 cycles");
  }
}

TEST_CASE("timeline: a gain curve (ramps, a flat stretch, a jump, down to 0) is within 0.01 dB, blocks 1 / 64 / 512 (T7.1)") {
  const Ticks start = kPpq / 2;
  const e::NoteDesc n = curved(note(1, start, 4 * kPpq, 220.0f), {},
                               {{start, 0.25f}, {start + kPpq, 1.0f}, {start + 2 * kPpq, 1.0f}, {start + 2 * kPpq, 0.5f},
                                {start + 3 * kPpq, 0.0f}});
  const std::vector<Seg> curve = {{0.5 * kQ, 0.25}, {1.5 * kQ, 1.0}, {2.5 * kQ, 1.0}, {2.5 * kQ, 0.5}, {3.5 * kQ, 0.0}};
  std::vector<float> ref;
  for (uint32_t block : {1u, 64u, 512u}) {
    INFO("block " << block);
    CurveRig r(kGate);
    REQUIRE(r.tl.publishSequence(image({{1, {clip(1, 0, 2 * kBar, {n})}}})));
    r.tl.apply(cmd(e::TransportCommandKind::Play, 0));
    r.run(5 * kQ, block);
    using K = e::NoteEventKind;
    const std::vector<Flat> want = {{12000, K::NoteOn, 220.0f, 0.25f, 0}, {12000, K::Gain, 0.0f, 1.0f, 24000},
                                    {60000, K::Gain, 0.0f, 0.5f, 0},      {60000, K::Gain, 0.0f, 0.0f, 24000},
                                    {108000, K::NoteOff, 0.0f, 0.0f, 0}};
    CHECK(flat(r.events) == want);
    const double worst = r.worstGainDb(12000, 108000, curve);
    CHECK(worst <= 0.01);
    CHECK(r.gain[11999] == 0.0f);
    CHECK(r.gain[12000] == 0.25f);
    CHECK(r.gain[36000] == 1.0f);
    CHECK(r.gain[59999] == 1.0f);
    CHECK(r.gain[60000] == 0.5f);
    CHECK(r.gain[84000] == 0.0f);  // parked from here to the note-off
    CHECK(r.mix[30000] == r.gain[30000]);  // the mix is gain · gate
    CHECK(r.ve.liveVoices() == 0);
    CHECK(r.rtViolations == 0);
    if (ref.empty()) ref = r.mix;
    else CHECK(std::memcmp(ref.data(), r.mix.data(), sizeof(float) * ref.size()) == 0);
    MESSAGE("block " << block << ": worst gain error " << worst << " dB");
  }
}

TEST_CASE("timeline: a note-on in the middle of its curves (the curve began earlier), from the start and after a seek (T7.1)") {
  // The curves span more than the note: pitch 0 -> +1200 c over ticks [0, 4 q], gain 0.2 -> 1 over [1 q, 3 q]; the
  // note plays [2 q, 4 q): it starts at +600 c and gain 0.6, mid-ramp.
  const e::NoteDesc n = curved(note(1, 2 * kPpq, 2 * kPpq, 200.0f), {{0, 0.0f}, {4 * kPpq, 1200.0f}},
                               {{kPpq, 0.2f}, {3 * kPpq, 1.0f}});
  using K = e::NoteEventKind;
  auto check = [&](CurveRig& r, int64_t on) {  // `on`: the engine sample of the note-on
    const std::vector<Flat> want = {{on, K::NoteOn, hzAt(200.0f, 600.0f), 0.6f, 0},
                                    {on, K::Pitch, 400.0f, 0.0f, 2 * kQ},
                                    {on, K::Gain, 0.0f, 1.0f, kQ},
                                    {on + 2 * kQ, K::NoteOff, 0.0f, 0.0f, 0}};
    const auto got = flat(r.events);
    REQUIRE(got.size() == 4);
    CHECK(got[0].hz == doctest::Approx(want[0].hz).epsilon(1e-7));
    CHECK(got[0].gain == doctest::Approx(0.6f).epsilon(1e-7));
    CHECK(got[1] == want[1]);
    CHECK(got[2] == want[2]);
    CHECK(got[3] == want[3]);
    const double s0 = static_cast<double>(on) - 2.0 * kQ;  // the engine sample of content tick 0
    CHECK(r.worstHzCents(on, on + 2 * kQ, {{s0, 0.0}, {s0 + 4.0 * kQ, 1200.0}}, 200.0) <= 0.01);
    CHECK(r.worstGainDb(on, on + 2 * kQ, {{s0 + 1.0 * kQ, 0.2}, {s0 + 3.0 * kQ, 1.0}}) <= 0.01);
    CHECK(r.rtViolations == 0);
  };
  for (uint32_t block : {1u, 64u, 512u}) {
    INFO("block " << block);
    {  // from the top
      CurveRig r(kGate);
      REQUIRE(r.tl.publishSequence(image({{1, {clip(1, 0, 2 * kBar, {n})}}})));
      r.tl.apply(cmd(e::TransportCommandKind::Play, 0));
      r.run(5 * kQ, block);
      check(r, 2 * kQ);
    }
    {  // play from 1.5 q: inside both curves, before the note
      CurveRig r(kGate);
      REQUIRE(r.tl.publishSequence(image({{1, {clip(1, 0, 2 * kBar, {n})}}})));
      r.tl.apply(cmd(e::TransportCommandKind::Play, 3 * kPpq / 2));
      r.run(4 * kQ, block);
      check(r, kQ / 2);
    }
    {  // a locate while playing, to 1.75 q
      CurveRig r(kGate);
      REQUIRE(r.tl.publishSequence(image({{1, {clip(1, 0, 2 * kBar, {n})}}})));
      r.tl.apply(cmd(e::TransportCommandKind::Play, 0));
      r.run(4096, block);
      r.tl.apply(cmd(e::TransportCommandKind::Locate, 7 * kPpq / 4));
      r.run(4 * kQ, block);
      check(r, 4096 + kQ / 4);
    }
    {  // a seek into the note itself: notes are not chased (timeline.h), so nothing sounds and no curve event is sent
      CurveRig r(kGate);
      REQUIRE(r.tl.publishSequence(image({{1, {clip(1, 0, 2 * kBar, {n})}}})));
      r.tl.apply(cmd(e::TransportCommandKind::Play, 5 * kPpq / 2));
      r.run(3 * kQ, block);
      CHECK(r.events.empty());
      CHECK(r.tl.stats().curveEvents == 0);
    }
  }
}

TEST_CASE("timeline: curves over loop passes — the arrangement loop and a clip loop replay the note and its curve (T7.1)") {
  using K = e::NoteEventKind;
  SUBCASE("arrangement loop") {
    // Loop [0, 4 q). The note plays [2 q, 4 q): +600 c at its start, +900 c at 3 q, +300 c at 3.5 q, then flat; the
    // wrap releases it at the loop end.
    const e::NoteDesc n = curved(note(1, 2 * kPpq, 2 * kPpq, 200.0f),
                                 {{0, 0.0f}, {3 * kPpq, 900.0f}, {7 * kPpq / 2, 300.0f}}, {{2 * kPpq, 0.5f}, {4 * kPpq, 1.0f}});
    for (uint32_t block : {64u, 500u}) {
      INFO("block " << block);
      CurveRig r(kGate);
      REQUIRE(r.tl.publishSequence(image({{1, {clip(1, 0, 2 * kBar, {n})}}})));
      r.tl.apply(loopCmd(0, 4 * kPpq));
      r.tl.apply(cmd(e::TransportCommandKind::Play, 0));
      const int64_t L = 4 * kQ;
      r.run(3 * L + 1000, block);
      const std::vector<Flat> pass = {{2 * kQ, K::NoteOn, hzAt(200.0f, 600.0f), 0.5f, 0},
                                      {2 * kQ, K::Pitch, hzAt(200.0f, 900.0f), 0.0f, kQ},
                                      {2 * kQ, K::Gain, 0.0f, 1.0f, 2 * kQ},
                                      {3 * kQ, K::Pitch, hzAt(200.0f, 300.0f), 0.0f, kQ / 2},
                                      {4 * kQ, K::NoteOff, 0.0f, 0.0f, 0}};
      for (int p = 0; p < 3; ++p) {
        INFO("pass " << p);
        const auto got = flat(r.events, p * L + 1, (p + 1) * L + 1, p * L);  // the wrap's note-off is at the next pass's 0
        REQUIRE(got.size() == pass.size());
        CHECK(got[0].hz == doctest::Approx(pass[0].hz).epsilon(1e-7));
        for (size_t i = 1; i < pass.size(); ++i) CHECK(got[i] == pass[i]);
        const double s0 = static_cast<double>(p * L);
        CHECK(r.worstHzCents(p * L + 2 * kQ, (p + 1) * L, {{s0, 0.0}, {s0 + 3.0 * kQ, 900.0}, {s0 + 3.5 * kQ, 300.0}}, 200.0) <= 0.01);
        CHECK(r.worstGainDb(p * L + 2 * kQ, (p + 1) * L, {{s0 + 2.0 * kQ, 0.5}, {s0 + 4.0 * kQ, 1.0}}) <= 0.01);
      }
      CHECK(r.tl.stats().loopWraps == 3);
      CHECK(r.tl.stats().curveEvents == 9);
      CHECK(r.rtViolations == 0);
      CHECK(r.ve.stats().unmatchedEvents == 0);
    }
  }
  SUBCASE("clip loop") {
    // The clip loops its first two quarters. The note starts at 0.5 q and is cut at each pass's end (2 q); its curve
    // goes on past it (a breakpoint at 3 q that no pass reaches).
    e::ClipDesc c = clip(1, 0, 6 * kPpq,
                         {curved(note(1, kPpq / 2, 3 * kPpq, 200.0f), {{kPpq / 2, 0.0f}, {kPpq, 700.0f}, {3 * kPpq, -500.0f}})});
    c.loopEnabled = true;
    c.loopStart = 0;
    c.loopEnd = 2 * kPpq;
    CurveRig r(kGate);
    REQUIRE(r.tl.publishSequence(image({{1, {c}}})));
    r.tl.apply(cmd(e::TransportCommandKind::Play, 0));
    const int64_t L = 2 * kQ;
    r.run(3 * L + 1000, 64);
    const std::vector<Flat> pass = {{kQ / 2, K::NoteOn, 200.0f, 1.0f, 0},
                                    {kQ / 2, K::Pitch, hzAt(200.0f, 700.0f), 0.0f, kQ / 2},
                                    {kQ, K::Pitch, hzAt(200.0f, -500.0f), 0.0f, 2 * kQ},
                                    {2 * kQ, K::NoteOff, 0.0f, 0.0f, 0}};
    for (int p = 0; p < 3; ++p) {
      INFO("pass " << p);
      CHECK(flat(r.events, p * L + 1, (p + 1) * L + 1, p * L) == pass);
      const double s0 = static_cast<double>(p * L);
      CHECK(r.worstHzCents(p * L + kQ / 2, (p + 1) * L, {{s0 + 0.5 * kQ, 0.0}, {s0 + 1.0 * kQ, 700.0}, {s0 + 3.0 * kQ, -500.0}}, 200.0) <= 0.01);
    }
    CHECK(r.tl.stats().curveEvents == 6);
    CHECK(r.events.size() == 12);
    CHECK(r.rtViolations == 0);
  }
}

TEST_CASE("timeline: an image swap keeps curve cursors by note id; changed curves, retunes and tempo maps re-synchronise (T7.1)") {
  // The note: on at 12000, +1200 c over the next two quarters (to 60000), back to 0 over two more (to 108000).
  const Ticks start = kPpq / 2;
  const std::vector<e::CurvePoint> up = {{start, 0.0f}, {start + 2 * kPpq, 1200.0f}, {start + 4 * kPpq, 0.0f}};
  const e::NoteDesc n = curved(note(1, start, 6 * kPpq, 220.0f), up);
  const std::vector<Seg> upS = {{0.5 * kQ, 0.0}, {2.5 * kQ, 1200.0}, {4.5 * kQ, 0.0}};
  const int64_t swapAt = 30016;  // a block start (64-frame blocks), mid-ramp, between two ticks
  using K = e::NoteEventKind;
  auto tracks = [](e::NoteDesc x, std::vector<e::NoteDesc> more = {}) {
    more.insert(more.begin(), std::move(x));
    return std::vector<e::TrackSequenceDesc>{{1, {clip(1, 0, 4 * kBar, more)}}};
  };
  // What the playhead tick is at the swap block (the timeline's `now`), and the curve there.
  const pq::TempoMap tm;
  const Ticks now = tm.firstTickAtOrAfterSample(swapAt, kSr);
  auto runSwap = [&](CurveRig& r, std::unique_ptr<e::SequenceImage> next) {
    REQUIRE(r.tl.publishSequence(image(tracks(n))));
    r.tl.apply(cmd(e::TransportCommandKind::Play, 0));
    r.run(7 * kQ, 64, [&](int64_t clock) {
      if (clock == swapAt && next) REQUIRE(r.tl.publishSequence(std::move(next)));
    });
  };
  CurveRig base(kGate);
  runSwap(base, nullptr);
  REQUIRE(base.events.size() == 4);  // on + ramp, the second ramp, off
  CHECK(base.worstHzCents(12000, 156000, upS, 220.0) <= 0.01);

  SUBCASE("unchanged curves (another note edited): the cursor stays, no event, the same stream") {
    CurveRig r(kGate);
    runSwap(r, image(tracks(n, {note(7, 5 * kPpq, kPpq / 4, 500.0f)})));  // rebuilt: other curve offsets, same content
    CHECK(r.tl.stats().sequenceSwaps == 2);
    CHECK(r.tl.stats().keptOnSwap == 1);
    CHECK(r.tl.stats().curveResyncs == 0);
    CHECK(r.tl.stats().retunedOnSwap == 0);
    auto got = flat(r.events);
    got.erase(std::remove_if(got.begin(), got.end(), [](const Flat& f) { return f.hz == 500.0f || (f.kind == K::NoteOff && f.t < 150000); }), got.end());
    CHECK(got == flat(base.events));
    CHECK(std::memcmp(r.hz.data(), base.hz.data(), sizeof(float) * r.hz.size()) == 0);
    CHECK(r.rtViolations == 0);
  }
  SUBCASE("a changed pitch curve: a step to the new curve at the playhead, then its ramp") {
    const std::vector<e::CurvePoint> half = {{start, 0.0f}, {start + 2 * kPpq, 600.0f}, {start + 4 * kPpq, 0.0f}};
    CurveRig r(kGate);
    runSwap(r, image(tracks(curved(n, half))));
    CHECK(r.tl.stats().curveResyncs == 1);
    CHECK(r.tl.stats().keptOnSwap == 1);
    const auto got = flat(r.events, swapAt, swapAt + 1);
    REQUIRE(got.size() == 2);
    const double c = 600.0 * static_cast<double>(swapAt - kQ / 2) / static_cast<double>(2 * kQ);  // at the block's first sample
    CHECK(got[0].kind == K::Pitch);
    CHECK(got[0].ramp == 0);
    CHECK(got[0].hz == doctest::Approx(220.0 * std::exp2(c / 1200.0)).epsilon(1e-7));
    CHECK(got[1] == Flat{swapAt, K::Pitch, hzAt(220.0f, 600.0f), 0.0f, static_cast<uint32_t>(60000 - swapAt)});
    CHECK(r.worstHzCents(12000, swapAt, upS, 220.0) <= 0.01);
    CHECK(r.worstHzCents(swapAt, 156000, {{0.5 * kQ, 0.0}, {2.5 * kQ, 600.0}, {4.5 * kQ, 0.0}}, 220.0) <= 0.01);
    CHECK(r.hz[60000] == hzAt(220.0f, 600.0f));
    CHECK(r.rtViolations == 0);
  }
  SUBCASE("a gain curve appears, the pitch curve goes away") {
    CurveRig r(kGate);
    runSwap(r, image(tracks(curved(n, {}, {{start, 1.0f}, {start + 4 * kPpq, 0.0f}}))));
    CHECK(r.tl.stats().curveResyncs == 1);
    const auto got = flat(r.events, swapAt, swapAt + 1);
    REQUIRE(got.size() == 3);
    CHECK(got[0] == Flat{swapAt, K::Pitch, 220.0f, 0.0f, 0});  // no pitch curve any more: back to the note's Hz
    CHECK(got[1].kind == K::Gain);
    CHECK(got[1].ramp == 0);
    CHECK(got[2] == Flat{swapAt, K::Gain, 0.0f, 0.0f, static_cast<uint32_t>(108000 - swapAt)});
    for (int64_t t = swapAt; t < 156000; ++t) REQUIRE(r.hz[static_cast<size_t>(t)] == 220.0f);
    CHECK(r.worstGainDb(swapAt, 108000 - 2000, {{0.5 * kQ, 1.0}, {4.5 * kQ, 0.0}}) <= 0.01);  // (the last samples near 0: below)
    for (int64_t t = 108000 - 2000; t < 108000; ++t)
      REQUIRE(r.gain[static_cast<size_t>(t)] == doctest::Approx(static_cast<double>(108000 - t) / (4.0 * kQ)).epsilon(1e-4));
    CHECK(r.gain[108000] == 0.0f);
    CHECK(flat(r.events, swapAt + 1).size() == 1);  // only the note-off: the new curves have no later breakpoint with work
  }
  SUBCASE("the note is re-pitched (same curve): one retune at the curve's value, then the ramp from there") {
    e::NoteDesc hi = n;
    hi.hz = 330.0f;
    CurveRig r(kGate);
    runSwap(r, image(tracks(hi)));
    CHECK(r.tl.stats().retunedOnSwap == 1);
    CHECK(r.tl.stats().curveResyncs == 1);
    const double c = 1200.0 * static_cast<double>(swapAt - kQ / 2) / static_cast<double>(2 * kQ);
    size_t at = 0;
    while (at < r.events.size() && r.events[at].t < swapAt) ++at;
    REQUIRE(at + 1 < r.events.size());
    CHECK(r.events[at].e.kind == K::Pitch);
    CHECK((r.events[at].e.flags & e::kNoteEventRetune) != 0);
    CHECK(r.events[at].e.rampFrames == 0);
    CHECK(r.events[at].e.hz == doctest::Approx(330.0 * std::exp2(c / 1200.0)).epsilon(1e-7));
    CHECK(flat(r.events, swapAt, swapAt + 1)[1] == Flat{swapAt, K::Pitch, 660.0f, 0.0f, static_cast<uint32_t>(60000 - swapAt)});
    CHECK(r.worstHzCents(swapAt, 156000, upS, 330.0) <= 0.01);
  }
  SUBCASE("a new tempo map: the running ramp is re-sent with its new length") {
    CurveRig r(kGate);
    REQUIRE(r.tl.publishSequence(image(tracks(n))));
    r.tl.apply(cmd(e::TransportCommandKind::Play, 0));
    auto slow = tempoImage({{0, 60.0, pq::TempoCurve::Step}});
    const pq::TempoMap tm2 = slow->tempo();
    const int64_t left = tm2.ticksToSamples(start + 2 * kPpq, kSr) - tm2.ticksToSamples(now, kSr);  // to the breakpoint
    r.run(4 * kQ, 64, [&](int64_t clock) {
      if (clock == swapAt) REQUIRE(r.tl.publishTempo(std::move(slow)));
    });
    CHECK(r.tl.stats().curveResyncs == 1);
    const auto got = flat(r.events, swapAt, swapAt + 1);
    REQUIRE(got.size() == 1);
    CHECK(got[0] == Flat{swapAt, K::Pitch, 440.0f, 0.0f, static_cast<uint32_t>(left)});
    CHECK(left > 2 * (60000 - swapAt) - 64);  // half the tempo: twice the samples
    CHECK(r.hz[static_cast<size_t>(swapAt + left)] == 440.0f);
    CHECK(r.hz[static_cast<size_t>(swapAt + left) - 1] < 440.0f);
    const double c0 = cents(r.hz[swapAt], 220.0);
    CHECK(r.worstHzCents(swapAt, swapAt + left, {{static_cast<double>(swapAt), c0}, {static_cast<double>(swapAt + left), 1200.0}}, 220.0) <= 0.01);
    CHECK(r.rtViolations == 0);
  }
}

TEST_CASE("timeline: two notes with the same onset and curves are bit-identical lanes, with compaction moving them (T7.1)") {
  // Eight fillers fill group 0, the pair lands in group 1; six fillers end and compaction moves the pair mid-curve.
  std::vector<e::NoteDesc> notes;
  for (uint32_t i = 0; i < 8; ++i) notes.push_back(note(10 + i, 0, i < 6 ? kPpq / 2 : 3 * kPpq, 110.0f + 7.0f * static_cast<float>(i)));
  const Ticks start = kPpq / 4;
  for (uint32_t id : {1u, 2u})
    notes.push_back(curved(note(id, start, 2 * kPpq, 220.0f),
                           {{start, 0.0f}, {start + kPpq, 702.0f}, {start + 2 * kPpq, 0.0f}},
                           {{start, 0.5f}, {start + kPpq / 2, 1.0f}, {start + 3 * kPpq / 2, 0.3f}}));
  e::VoiceConfig vc;
  vc.silenceMs = 5.0f;
  CurveRig r(kSine, config(), 16, 3, vc);
  REQUIRE(r.ve.config().compact);
  REQUIRE(r.tl.publishSequence(image({{1, {clip(1, 0, 2 * kBar, notes)}}})));
  r.tl.apply(cmd(e::TransportCommandKind::Play, 0));
  uint32_t inst[2] = {0, 0}, lanes[2] = {e::kNoSlot, e::kNoSlot}, first[2] = {e::kNoSlot, e::kNoSlot};
  bool ended[2] = {false, false};
  std::vector<float> lane[2], gain[2];
  bool moved = false;
  uint64_t movesBefore = 0;
  size_t seen = 0;
  r.run(3 * kQ, 64, [](int64_t) {}, [&](int64_t, uint32_t n) {
    for (; seen < r.events.size(); ++seen) {  // the pair's instance ids: the note-ons at 220 Hz
      const e::NoteEvent& ev = r.events[seen].e;
      if (ev.kind == e::NoteEventKind::NoteOn && ev.hz == 220.0f) inst[inst[0] == 0 ? 0 : 1] = ev.noteId;
    }
    for (int i = 0; i < 2; ++i) {
      if (inst[i] == 0 || ended[i]) continue;
      uint32_t l = lanes[i];
      if (l == e::kNoSlot) {  // started in this block
        l = r.ve.laneOf(inst[i]);
        REQUIRE(l != e::kNoSlot);
        REQUIRE(r.ve.stats().voiceMoves == movesBefore);
        first[i] = l;
      }
      const float* o = r.ve.voiceOut(l >> 3, r.ve.mixSlot(0));
      const float* g = r.ve.gain(l >> 3);
      for (uint32_t f = 0; f < n; ++f) {
        lane[i].push_back(o[f * 8 + (l & 7u)]);
        gain[i].push_back(g[f * 8 + (l & 7u)]);
      }
      lanes[i] = r.ve.laneOf(inst[i]);
      if (lanes[i] == e::kNoSlot) ended[i] = true;
      else if ((lanes[i] >> 3) != (first[i] >> 3)) moved = true;
    }
    movesBefore = r.ve.stats().voiceMoves;
  });
  CHECK(first[0] != first[1]);
  CHECK((first[0] >> 3) == 1);
  CHECK(moved);
  CHECK(r.ve.stats().voiceMoves >= 2);
  REQUIRE(lane[0].size() == lane[1].size());
  REQUIRE(lane[0].size() > static_cast<size_t>(2 * kQ));
  CHECK(std::memcmp(lane[0].data(), lane[1].data(), sizeof(float) * lane[0].size()) == 0);
  CHECK(std::memcmp(gain[0].data(), gain[1].data(), sizeof(float) * gain[0].size()) == 0);
  double energy = 0.0, gmax = 0.0;
  for (float v : lane[0]) energy += static_cast<double>(v) * v;
  for (float v : gain[0]) gmax = std::max(gmax, static_cast<double>(v));
  CHECK(energy > 100.0);
  CHECK(gmax == 1.0);
  CHECK(ended[0]);
  CHECK(ended[1]);
  CHECK(r.tl.stats().curveEvents == 2 * 4);  // per note: two ramps at the onset, then one pitch and one gain breakpoint
  CHECK(r.rtViolations == 0);
}

TEST_CASE("timeline: curves at the event-volume limit — dropped curve events are counted and heal, offs are never lost (T7.1)") {
  SUBCASE("64 voices with a dense vibrato: more curve events than a track's list holds") {
    // Every note: pitch and gain breakpoints every 100 samples for two quarters (alternating values), then two sparse
    // ones. In 512-frame blocks that is ~650 curve events per block for a list of 512 (64 slots kept for the offs).
    const Ticks step = kPpq / 240;  // 100 samples
    std::vector<e::NoteDesc> notes;
    for (uint32_t i = 0; i < 64; ++i) {
      std::vector<e::CurvePoint> p, g;
      for (Ticks k = 0; k * step < 2 * kPpq; ++k) {
        p.push_back({k * step, k % 2 == 0 ? -20.0f : 20.0f});
        g.push_back({k * step, k % 2 == 0 ? 0.8f : 1.0f});
      }
      p.push_back({3 * kPpq, 50.0f});
      g.push_back({3 * kPpq, 0.5f});
      p.push_back({7 * kPpq / 2, 0.0f});
      g.push_back({7 * kPpq / 2, 1.0f});
      notes.push_back(curved(note(1 + i, 0, 4 * kPpq, 100.0f + static_cast<float>(i)), p, g));
    }
    CurveRig r(kGate, config(), 64, e::voiceGroupsFor(64));
    REQUIRE(config().maxEventsPerTrack == 512);
    REQUIRE(r.tl.publishSequence(image({{1, {clip(1, 0, 2 * kBar, notes)}}})));
    r.tl.apply(cmd(e::TransportCommandKind::Play, 0));
    uint32_t maxEvents = 0;
    bool finalOk = true;
    r.run(4 * kQ + 4096, 512, [](int64_t) {}, [&](int64_t clock, uint32_t n) {
      maxEvents = std::max(maxEvents, r.tl.events(0).count);
      if (clock <= 7 * kQ / 2 + 600 && 7 * kQ / 2 + 600 < clock + n) {  // after the last breakpoint: every voice is exact
        const uint32_t f = static_cast<uint32_t>(7 * kQ / 2 + 600 - clock);
        for (uint32_t l = 0; l < 64; ++l) {
          const float h = r.ve.signal(l >> 3, e::VoiceSignal::Hz)[f * 8 + (l & 7u)];
          const float g = r.ve.gain(l >> 3)[f * 8 + (l & 7u)];
          finalOk = finalOk && g == 1.0f && h >= 100.0f && h <= 163.0f && h == std::floor(h);
        }
      }
    });
    const e::TimelineStats& st = r.tl.stats();
    CHECK(maxEvents == 512 - 64);  // room is kept for the 64 pending note-offs
    CHECK(st.droppedCurveEvents > 1000);
    CHECK(st.curveEvents > 10000);
    CHECK(st.curveCollapses == 0);
    CHECK(st.noteOns == 64);
    CHECK(st.noteOffs == 64);  // never dropped for curve events
    CHECK(st.droppedNoteOns == 0);
    CHECK(finalOk);  // the sparse breakpoints after the storm put every voice back on its curve, exactly
    CHECK(r.ve.stats().droppedEvents == 0);
    CHECK(r.ve.stats().noteOffs == 64);
    CHECK(r.ve.liveVoices() == 0);
    CHECK(r.rtViolations == 0);
    MESSAGE("curve events " << st.curveEvents << ", dropped " << st.droppedCurveEvents);
  }
  SUBCASE("more breakpoints in a block than the look limit: one step per note at the block's end") {
    e::TimelineConfig cfg = config();
    cfg.maxEventsPerTrack = 16;  // the look limit is 4 x this
    // 400 breakpoints ~1 sample apart from the note's start: 100 flat ones, then +1 c each; then down to 0 at 3000.
    std::vector<e::CurvePoint> p;
    for (Ticks i = 0; i < 400; ++i) p.push_back({10 * i, i < 100 ? 0.0f : static_cast<float>(i - 100)});
    p.push_back({kPpq / 8, 0.0f});
    const pq::TempoMap tm;
    CurveRig r(kGate, cfg);
    REQUIRE(r.tl.publishSequence(image({{1, {clip(1, 0, kBar, {curved(note(1, 0, kPpq, 300.0f), p)})}}})));
    r.tl.apply(cmd(e::TransportCommandKind::Play, 0));
    r.run(4096, 512);
    CHECK(r.tl.stats().curveCollapses == 1);
    CHECK(r.tl.events(0).count <= 16);
    // From the second block on the voice is on the curve again (the first block's last frame stepped onto it).
    // The step at the block's last frame lands on the curve (its value at that sample), then the ramp goes on.
    const std::vector<Seg> tail = {{tm.ticksToSamplesF(3990, kSr), 299.0}, {3000.0, 0.0}};
    CHECK(r.worstHzCents(512, 3001, tail, 300.0) <= 0.01);
    CHECK(r.hz[3000] == 300.0f);
    CHECK(r.hz[4000] == 300.0f);
    CHECK(r.rtViolations == 0);
  }
}

TEST_CASE("timeline: RtScope-clean with curved notes on 8 tracks under loops, swaps, locates and tempo changes (T7.1)") {
  auto descs = [](uint32_t variant) {
    std::vector<e::TrackSequenceDesc> tracks(8);
    uint32_t id = 1;
    for (uint32_t k = 0; k < 8; ++k) {
      tracks[k].id = 100 + k;
      for (uint32_t c = 0; c < 8; ++c) {
        e::ClipDesc cd = clip(c, static_cast<Ticks>(c) * kBar, kBar, {});
        if ((c + k) % 3 == 0) {
          cd.loopEnabled = true;
          cd.loopStart = 0;
          cd.loopEnd = 2 * kPpq;
        }
        for (uint32_t i = 0; i < 6; ++i) {
          e::NoteDesc n = note(id++, static_cast<Ticks>(i) * kPpq / 2 + k * 977, kPpq + i * 5000);
          const Ticks s = n.start;
          const float bend = static_cast<float>((i + k + ((i % 2) == 0 ? variant : 0)) % 7) * 100.0f;
          if (i % 3 != 2) n.pitchCurve = {{s - kPpq / 4, 0.0f}, {s + kPpq / 8, bend}, {s + kPpq / 4, bend}, {s + kPpq / 4, -bend}, {s + kPpq, 0.0f}};
          if (i % 2 == 0) n.gainCurve = {{s, 1.0f}, {s + kPpq / 3, 0.0f}, {s + kPpq / 2, 0.0f}, {s + kPpq, 0.7f}};
          cd.notes.push_back(n);
        }
        tracks[k].clips.push_back(std::move(cd));
      }
    }
    return tracks;
  };
  e::Timeline tl(config());
  REQUIRE(tl.publishSequence(image(descs(0))));
  REQUIRE(tl.post(loopCmd(2 * kBar, 4 * kBar + 7)));
  REQUIRE(tl.post(cmd(e::TransportCommandKind::Play, kBar)));
  int64_t clock = 0;
  uint64_t events = 0;
  bool ordered = true;
  auto blocks = [&](int count, uint32_t frames) {
    return pq::alloc_trap::countRtViolations([&] {
      for (int b = 0; b < count; ++b) {
        tl.process(clock, frames);
        for (uint32_t k = 0; k < 8; ++k) {
          const e::EventList l = tl.events(k);
          events += l.count;
          for (uint32_t i = 1; i < l.count; ++i) ordered = ordered && l.events[i].frame >= l.events[i - 1].frame;
        }
        clock += frames;
      }
    });
  };
  CHECK(blocks(1500, 256) == 0);
  for (uint32_t v = 1; v <= 6; ++v) {  // swaps mid-note with changed curves (built and published off the RT scope)
    REQUIRE(tl.publishSequence(image(descs(v))));
    CHECK(blocks(40 + 7 * static_cast<int>(v), 100) == 0);
  }
  CHECK(tl.stats().curveResyncs > 0);
  const uint64_t resyncs = tl.stats().curveResyncs;
  REQUIRE(tl.publishTempo(tempoImage({{0, 97.0, pq::TempoCurve::Linear}, {8 * kBar, 150.0, pq::TempoCurve::Step}})));
  CHECK(blocks(700, 256) == 0);
  CHECK(tl.stats().curveResyncs > resyncs);  // the tempo map re-sent running ramps
  REQUIRE(tl.post(cmd(e::TransportCommandKind::Locate, 5 * kBar + kPpq / 3)));
  REQUIRE(tl.post(loopCmd(0, 0, false)));
  CHECK(blocks(800, 512) == 0);
  REQUIRE(tl.post(cmd(e::TransportCommandKind::Stop)));
  CHECK(blocks(4, 64) == 0);
  CHECK(ordered);
  CHECK(events > 2000);
  CHECK(tl.stats().curveEvents > 1000);
  CHECK(tl.stats().droppedCurveEvents == 0);
  CHECK(tl.stats().loopWraps > 0);
  for (uint32_t k = 0; k < 8; ++k) CHECK(tl.soundingNotes(k) == 0);
  tl.collect();
}
