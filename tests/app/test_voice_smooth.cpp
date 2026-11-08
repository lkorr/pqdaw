// AU.voices (user 2026-11-08: "a click near the beginning when pressing play", "notes persist a bit too long after
// being pressed"): MIDI playback through the real engine path (ProjectLink → Engine → Timeline → ProjectProgram →
// VoiceEngine → the default instrument / the Wavetable Synth), measured sample by sample.
//   click detector  the 4th difference |y[n] − 4y[n−1] + 6y[n−2] − 4y[n−3] + y[n−4]|: a sine at f and amplitude A
//                   gives A·(2πf/sr)⁴ (3.4e-7 for the default instrument's C3), a step of h gives up to 6h, an
//                   envelope corner (the 5 ms attack's end, a release start) ≈ 1e-3 at the default level 0.25. The
//                   bounds below sit just above the corners, so a step of ≳ 3e-4 (−70 dBFS) anywhere fails.
//   Play            from a stopped engine, an asleep one (idle gate) and right after creation: no click, and the
//                   asleep / awake renders are bit-identical (nothing in the engine depends on how Play found it)
//   transport       restart while playing, stop + play while the voices release (a same-pitch retrigger), loop
//                   wraps at a note boundary and inside notes: no click
//   edits           a structural edit while a note sounds (its unit recompiles and crossfades; a new track: a
//                   topology change) renders bit-identically to no edit (the state transfer is exact)
//   > 8 voices      a second voice group and its compaction (voices moved between groups): no click
//   voice end       a note's voice ends within one block of where its Envelope's release reaches −80 dB (the done
//                   flag), for single notes, abutting and overlapping same-pitch notes and a chord
//   audition        a piano-roll press sounds while held: releaseAuditions() ends it ≥ 50 ms after its onset, long
//                   before its requested length
// Every render runs its blocks under RtScope (the allocation trap) and DenormalGuard, as the audio thread does.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/modules.h"
#include "pq/engine/project_program.h"
#include "pq/engine/voices.h"
#include "pq/meta/envelope.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr double kBeatSeconds = 0.5;  // 120 BPM

const DeviceLibrary& library() { return playback_fixture::unitsLibrary(); }

// A MIDI track (the default instrument, or the Wavetable Synth) playing `notes` from tick 0.
struct Song {
  DocumentStore store;
  TrackId keys;
  explicit Song(std::vector<Note> notes, bool wavetableSynth = false) {
    store.setDeviceLibrary(&library());
    auto e = ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Keys");
    REQUIRE(e.ok());
    REQUIRE(store.dispatch(e->command).ok());
    keys = e->tracks.at(0);
    if (wavetableSynth) {
      std::vector<DeviceId> heads;
      for (const Device* d : mainPath(*store.current(), *findTrackPtr(*store.current(), keys)).blocks) heads.push_back(d->id);
      auto del = ops::deleteDevices(*store.current(), heads);
      REQUIRE(del.ok());
      REQUIRE(store.dispatch(del->command).ok());
      IdSource local;
      auto paste = ops::pasteBlocksAtNotes(*store.current(), store.ids(), keys, wavetableSynthBlocks(local));
      REQUIRE(paste.ok());
      REQUIRE(store.dispatch(paste->command).ok());
    }
    for (Note& n : notes) n.id = store.ids().note();
    Batch b{"clip", {}};
    b.commands.push_back(AddClip{keys, playback_fixture::clip(store.ids(), 0, 16 * kPpq, std::move(notes), "c")});
    REQUIRE(store.dispatch(b).ok());
  }
};

Note note(Ticks start, Ticks length, int32_t degree) {
  IdSource scratch;  // Song gives the note its id
  return playback_fixture::note(scratch, start, length, degree);
}

struct Action {
  enum Kind { Play, Stop, Loop, Edit, Audition, Release } kind;
  uint32_t block;  // before this block (counted from the first Play)
  Ticks tick = 0, tick2 = 0;
};

struct Opts {
  bool jit = false;
  float sleepMs = 0.0f;         // the engine's idle gate (0: off)
  uint32_t stoppedBlocks = 0;   // blocks rendered stopped before Play
  uint32_t blocks = 1500;       // blocks rendered from Play on
  bool play = true;
  std::vector<Action> actions;
  DocumentStore* store = nullptr;  // Edit actions: edit(store), then the link compiles it
  std::function<void(DocumentStore&)> edit;
};

struct Render {
  std::vector<float> y;          // left channel, from Play on
  std::vector<uint32_t> live;    // live voices of the track's unit after each block
  uint64_t crossfades = 0, voiceMoves = 0;
};

Render render(const Snapshot& s, const Opts& o) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = o.sleepMs;
  c.jit = o.jit;
  c.sampleRate = kRate;
  auto l = app::ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  app::ProjectLink& link = **l;
  REQUIRE(link.waitForMetaTables());
  if (o.jit) REQUIRE(link.waitForJit(180.0));
  ListenerId sub{};
  if (o.store != nullptr) sub = o.store->subscribe([&](const StoreEvent& e) { link.onDocumentChange(e); });
  engine::ProjectProgram& program = *link.engine().program();
  auto voices = [&]() -> engine::VoiceEngine* {
    for (uint32_t u = 0; u < program.numUnits(); ++u)
      if (engine::VoiceEngine* v = program.voices(u)) return v;
    return nullptr;
  };
  Render r;
  std::vector<float> L(kBlock), R(kBlock);
  int64_t t = 0;
  auto block = [&] {
    float* ch[2] = {L.data(), R.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
    }
    t += kBlock;
  };
  for (uint32_t b = 0; b < o.stoppedBlocks; ++b) block();
  if (o.stoppedBlocks > 0 && o.sleepMs > 0.0f) CHECK(link.engine().asleep());
  if (o.play) link.play(0);
  for (uint32_t b = 0; b < o.blocks; ++b) {
    for (const Action& a : o.actions) {
      if (a.block != b) continue;
      switch (a.kind) {
        case Action::Play: link.play(a.tick); break;
        case Action::Stop: link.stop(); break;
        case Action::Loop: link.setLoop(views::TransportLoop{a.tick, a.tick2, true}); break;
        case Action::Edit:
          o.edit(*o.store);
          link.flush();
          break;
        case Action::Audition: link.auditionHz(440.0f, 100.0f, static_cast<float>(a.tick)); break;
        case Action::Release: link.releaseAuditions(); break;
      }
    }
    block();
    r.y.insert(r.y.end(), L.begin(), L.end());
    const engine::VoiceEngine* v = voices();
    r.live.push_back(v != nullptr ? v->liveVoices() : 0u);
  }
  if (o.store != nullptr) o.store->unsubscribe(sub);
  for (uint32_t u = 0; u < program.numUnits(); ++u) {
    r.crossfades += program.unitStats(u).crossfades;
    if (const engine::VoiceEngine* v = program.voices(u)) r.voiceMoves += v->stats().voiceMoves;
  }
  CHECK(link.lastError().empty());
  return r;
}

// The largest 4th difference in [from, to) and where it is.
struct Peak {
  double value = 0.0;
  size_t at = 0;
};
Peak d4(const std::vector<float>& y, size_t from = 0, size_t to = SIZE_MAX) {
  Peak p;
  for (size_t i = std::max<size_t>(from, 4); i < std::min(to, y.size()); ++i) {
    const double v = std::fabs(static_cast<double>(y[i]) - 4.0 * y[i - 1] + 6.0 * y[i - 2] - 4.0 * y[i - 3] + y[i - 4]);
    if (v > p.value) p = {v, i};
  }
  return p;
}
void checkSmooth(const std::vector<float>& y, double bound, size_t from = 0, size_t to = SIZE_MAX) {
  const Peak p = d4(y, from, to);
  CHECK_MESSAGE(p.value <= bound, "4th difference " << p.value << " at " << (static_cast<double>(p.at) * 1000.0 / kRate) << " ms");
}
double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
  return m;
}
double peak(const std::vector<float>& y, size_t from, size_t to) {
  double m = 0.0;
  for (size_t i = from; i < std::min(to, y.size()); ++i) m = std::max(m, static_cast<double>(std::fabs(y[i])));
  return m;
}
size_t samplesAt(double seconds) { return static_cast<size_t>(std::llround(seconds * kRate)); }

constexpr double kOneVoice = 2e-3;  // the bound for one or two default-instrument voices (corners ≈ 1e-3)

// The default instrument's Envelope (attack 5 ms, decay 400 ms → sustain 0.6, release 250 ms, curves 0): seconds
// from a note-off at level `from` (the sustain by default) until the level is below 1e-4 (its done flag ends the voice).
double releaseToDone(double from = 0.6) {
  const meta::EnvSegmentLaw law = meta::envFallLaw(0.0);
  return 0.25 * meta::envFallTime(law, from, 0.0, 1.0, 1e-4);
}

std::vector<Note> melody() {
  return {note(0, kPpq, 0),
          note(kPpq, kPpq, 4),
          note(2 * kPpq, kPpq / 2, 7),
          note(2 * kPpq + kPpq / 2, kPpq / 2, 7),  // abutting the same pitch: a retrigger
          note(3 * kPpq, 2 * kPpq, 0),
          note(3 * kPpq + kPpq / 4, kPpq, 0),      // overlapping the same pitch: a second voice
          note(6 * kPpq, kPpq, 0), note(6 * kPpq, kPpq, 4), note(6 * kPpq, kPpq, 7), note(6 * kPpq, kPpq, 11)};
}

}  // namespace

TEST_CASE("AU.voices Play: no click at the first note, from a stopped, an asleep or a new engine; asleep == awake") {
  Song s({note(0, 2 * kPpq, 0)});
  Opts awake;
  awake.stoppedBlocks = 300;
  awake.blocks = 600;
  Opts asleep = awake;
  asleep.sleepMs = 250.0f;  // 300 blocks = 400 ms stopped: asleep when Play comes
  Opts fresh = awake;
  fresh.stoppedBlocks = 0;
  for (bool jit : {false, true}) {
    CAPTURE(jit);
    awake.jit = asleep.jit = fresh.jit = jit;
    const Render a = render(s.store.current(), awake), b = render(s.store.current(), asleep), f = render(s.store.current(), fresh);
    CHECK(maxDiff(a.y, b.y) == 0.0);
    CHECK(peak(a.y, 0, samplesAt(0.1)) > 0.05);
    for (const Render* r : {&a, &b, &f}) checkSmooth(r->y, kOneVoice, 0, samplesAt(0.05));
    checkSmooth(a.y, kOneVoice);
  }
}

TEST_CASE("AU.voices transport: restart while playing, stop + play while releasing, loop wraps: no click") {
  Song s(melody());
  Opts o;
  o.stoppedBlocks = 100;
  o.blocks = 3000;
  const double chord = 4e-3;  // the 4-note chord (degree 11 = 494 Hz) adds its own 4th difference
  o.actions = {{Action::Play, 200, 0}};
  checkSmooth(render(s.store.current(), o).y, chord);
  o.actions = {{Action::Stop, 200}, {Action::Play, 260, 0}};  // 80 ms into the release: the same pitch retriggers
  checkSmooth(render(s.store.current(), o).y, chord);
  o.actions = {{Action::Loop, 0, 0, 2 * kPpq}};
  checkSmooth(render(s.store.current(), o).y, kOneVoice);
  o.actions = {{Action::Loop, 0, kPpq / 2, 2 * kPpq + kPpq / 3}};  // wraps inside notes
  checkSmooth(render(s.store.current(), o).y, kOneVoice);
}

TEST_CASE("AU.voices edits: a structural edit while a note sounds renders bit-identically to no edit") {
  const auto notes = std::vector<Note>{note(0, 2 * kPpq, 0)};
  Song plain(notes);
  Opts ref;
  ref.stoppedBlocks = 100;
  ref.blocks = 1000;
  const Render want = render(plain.store.current(), ref);
  for (int kind = 0; kind < 2; ++kind) {
    CAPTURE(kind);
    Song s(notes);
    Opts o = ref;
    o.store = &s.store;
    o.actions = {{Action::Edit, 40}};  // 53 ms in: the note is in its decay
    const TrackId keys = s.keys;
    o.edit = [kind, keys](DocumentStore& st) {
      if (kind == 0) {  // a Utility (0 dB) on the instrument's path: the track's unit recompiles and crossfades
        auto e = ops::pasteBlocks(*st.current(), st.ids(), keys, utilityBlocks(st.ids()));
        REQUIRE(e.ok());
        REQUIRE(st.dispatch(e->command).ok());
        auto in = ops::insertOnPath(*st.current(), st.ids(), e->devices.at(0), 1);
        REQUIRE(in.ok());
        REQUIRE(st.dispatch(in->command).ok());
      } else {  // a new track: a topology change
        auto e = ops::createTrack(*st.current(), st.ids(), TrackKind::Midi, {}, "M");
        REQUIRE(e.ok());
        REQUIRE(st.dispatch(e->command).ok());
      }
    };
    const Render got = render(s.store.current(), o);
    if (kind == 0) CHECK(got.crossfades >= 1);
    CHECK(maxDiff(got.y, want.y) == 0.0);
  }
}

TEST_CASE("AU.voices > 8 voices: a second voice group and its compaction are click-free") {
  std::vector<Note> n;  // 12 notes a sixteenth apart; the even ones of the first 8 end after a beat
  for (int k = 0; k < 12; ++k) n.push_back(note(k * kPpq / 16, (k < 8 && k % 2 == 0 ? 1 : 6) * kPpq, 2 * k));
  Song s(n);
  Opts o;
  o.stoppedBlocks = 100;
  o.blocks = 3000;
  for (bool jit : {false, true}) {
    CAPTURE(jit);
    o.jit = jit;
    const Render r = render(s.store.current(), o);
    CHECK(r.voiceMoves > 0);
    checkSmooth(r.y, 3e-3);  // 12 voices up to 1 kHz: their own 4th difference ≈ 1.5e-4, onsets ≈ 2e-3
  }
}

TEST_CASE("AU.voices voice end: within a block of the Envelope's release reaching −80 dB (default instrument, WT synth)") {
  const double rel = releaseToDone();
  CHECK(rel == doctest::Approx(0.236).epsilon(0.01));
  const double block = kBlock / kRate;
  for (bool synth : {false, true}) {
    CAPTURE(synth);
    Song s(melody(), synth);
    Opts o;
    o.stoppedBlocks = 50;
    o.blocks = 3500;
    const Render r = render(s.store.current(), o);
    // The last note-off: the chord at beat 6 for one beat. Every voice has ended within a block of its done time.
    const double lastOff = 7 * kBeatSeconds;
    size_t ended = r.live.size();
    for (size_t b = r.live.size(); b > 0 && r.live[b - 1] == 0; --b) ended = b - 1;
    const double endSeconds = static_cast<double>(ended) * block;
    if (!synth) {
      CHECK(endSeconds >= lastOff + rel - block);
      CHECK(endSeconds <= lastOff + rel + 2 * block);
      // Before the chord: the overlapping same-pitch pair (3 → 5 and 3.25 → 4.25 beats) has ended by 5 beats + rel.
      const size_t pairEnd = static_cast<size_t>((5 * kBeatSeconds + rel + 2 * block) / block);
      CHECK(r.live[pairEnd] == 0);
    } else {
      CHECK(endSeconds <= lastOff + 1.0);  // its own envelope (the module's defaults) ends it
    }
    CHECK(peak(r.y, samplesAt(endSeconds + block), r.y.size()) == 0.0);
  }
}

TEST_CASE("AU.voices audition: a press sounds while held; releaseAuditions ends it ≥ 50 ms after its onset") {
  Song s({});
  Opts o;
  o.play = false;
  o.stoppedBlocks = 0;
  o.blocks = 600;  // 0.8 s
  o.actions = {{Action::Audition, 10, 2000}};
  const Render held = render(s.store.current(), o);
  const size_t onset = 10 * kBlock;
  CHECK(peak(held.y, onset + samplesAt(0.6), onset + samplesAt(0.7)) > 0.01);  // a 2 s audition still sounds at 0.6 s
  o.actions = {{Action::Audition, 10, 2000}, {Action::Release, 12}};  // let go 2.7 ms after the press
  const Render let = render(s.store.current(), o);
  CHECK(maxDiff(std::vector<float>(held.y.begin(), held.y.begin() + onset + samplesAt(0.05)),
                std::vector<float>(let.y.begin(), let.y.begin() + onset + samplesAt(0.05))) == 0.0);  // the first 50 ms
  CHECK(peak(let.y, onset + samplesAt(0.06), onset + samplesAt(0.07)) < peak(held.y, onset + samplesAt(0.06), onset + samplesAt(0.07)));
  CHECK(peak(let.y, onset + samplesAt(0.05 + releaseToDone(1.0)) + kBlock, let.y.size()) == 0.0);  // over (released in its decay: below 1.0)
  checkSmooth(let.y, kOneVoice * 2.0);  // 440 Hz: (2π·440/sr)⁴ is 8 × C3's
}
