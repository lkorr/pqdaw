// T6.9: Capture MIDI through the playback link (pq/app/project_link.h "Capture MIDI", pq/app/midi_capture.h):
//   playing   notes played (MIDI port) while the transport runs land on the song samples they were played on (±1 ms),
//             a second capture takes only what came after the first, a ≥ 2-bar silence starts a new phrase; one undo
//             step each
//   stopped   a one-bar groove at 97 BPM (accented quarters, soft offbeats): the tempo becomes 97 ±1 (a project
//             without clips), a looped one-bar clip at the insert marker's bar, the notes on its half-beat grid; ONE
//             undo step restores the tempo and removes the clip; with a clip in the project the tempo stays
//   identities Keys / Hz on 31-EDO and a MOS: the route table's identities (Keys: the key's Degree; Hz: its Hz frozen
//             as Cents, a note-on bend frozen in within 0.001 cent), the computer keyboard's degrees too
//   dispatch  Ctrl+Shift+C through a ViewContext on the link and the Shell (TestDriver key events), Ctrl+Z undoes it
// Every engine block runs under RtScope (the capture-write path on the audio thread allocates nothing).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/input_map.h"
#include "pq/ui/test_driver.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

enum class World { Edo31, Mos };

TuningDef worldDef(World w) {
  TuningDef d;
  if (w == World::Edo31) {
    d.system = tuning::Edo::make(31).value();
  } else {
    tuning::MosParams m;
    m.n = 7;
    m.period = 1200.0;
    m.generator = 705.0;
    d.system = m;
  }
  return d;
}

// A MIDI track in tuning `w` with note input `mode`, armed (Monitor Auto: it listens to every port and the keyboard).
TrackId midiTrack(DocumentStore& store, World w, NoteInputMode mode) {
  IdSource& ids = store.ids();
  const TrackId track = ids.track();
  const TuningId tuning = ids.tuning();
  Batch b{"midi", {}};
  b.commands.push_back(AddTuning{TuningSlot{tuning, "t", Colour{0x2266AA}, worldDef(w)}});
  b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Keys"), {}, kAppend});
  b.commands.push_back(SetTrackTuning{track, TuningAssignment::mainTuning(tuning)});
  b.commands.push_back(SetNoteInput{track, NoteInput{mode, InputKeyMap::Tuning}});
  (void)playback_fixture::instrument(b, ids, track, ids.device());
  REQUIRE(store.dispatch(b).ok());
  playback_fixture::wireSeries(store);
  auto arm = ops::armTrack(*store.current(), track, true);
  REQUIRE(arm.ok());
  REQUIRE(store.dispatch(arm->command).ok());
  return track;
}

engine::RawMidi midi(uint8_t status, uint8_t d1, uint8_t d2) {
  engine::RawMidi m;
  m.port = 0;
  m.status = status;
  m.d1 = d1;
  m.d2 = d2;
  return m;  // no stamp: it plays at the start of the block that drains it
}

struct Rig {
  DocumentStore store{playback_fixture::unwiredDefaultProject()};
  std::unique_ptr<ProjectLink> link;
  ListenerId sub = 0;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);

  ~Rig() {
    if (link) store.unsubscribe(sub);
    link.reset();
  }
  void start() {
    LinkConfig c;
    c.compileThread = false;
    c.jit = false;
    c.sleepAfterMs = 0.0f;
    auto made = ProjectLink::create(c, store.current());
    REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
    link = std::move(made.value());
    link->setRecordStore(&store);
    sub = store.subscribe([this](const StoreEvent& e) { link->onDocumentChange(e); });
    link->setLivePorts({"Port"}, {2.0f});
    link->flush();
  }
  void block() {
    float* outs[2] = {l.data(), r.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link->engine().process(engine::AudioBlock{nullptr, 0, outs, 2, kBlock, time});
    }
    time += kBlock;
  }
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      block();
      if ((b & 15) == 15) (void)link->playhead();
      if ((b & 255) == 255) link->collect();
    }
    (void)link->playhead();
  }
  // Runs blocks until engine time `t` (the next block starts at or after it).
  void runTo(int64_t t) {
    while (time < t) block();
    (void)link->playhead();
  }
  void push(const engine::RawMidi& m) { REQUIRE(link->livePorts()->push(0, m)); }
  int64_t song() { return link->engine().timeline().songSample(); }  // between blocks: the next block's first sample
  const Track& track(TrackId id) const {
    const Track* t = findTrackPtr(*store.current(), id);
    REQUIRE(t != nullptr);
    return *t;
  }
  std::vector<Note> notesOf(const ClipSlot& c) const { return {c.clip->notes.begin(), c.clip->notes.end()}; }
  int64_t sampleAt(Ticks t) const { return store.current()->timeline->tempoMap.ticksToSamples(t, kRate); }
};

}  // namespace

TEST_CASE("T6.9 capture: playing, notes land where they were played (±1 ms); since the last capture; the last phrase") {
  Rig r;
  const TrackId t = midiTrack(r.store, World::Edo31, NoteInputMode::Keys);
  r.start();
  r.run(10);
  r.link->play(0);
  r.run(37);  // not on a bar: the clip starts on the bar before the first note
  REQUIRE(r.link->playhead().playing);
  struct Played {
    int64_t on, off;  // song samples
  };
  std::vector<Played> played;
  auto play = [&](uint8_t key, uint32_t holdBlocks, uint32_t afterBlocks) {
    r.push(midi(0x90, key, 100));
    const int64_t on = r.song();
    r.run(holdBlocks);
    r.push(midi(0x80, key, 50));
    const int64_t off = r.song();
    r.run(afterBlocks);
    played.push_back({on, off});
  };
  play(60, 100, 50);
  play(64, 37, 3);
  play(67, 200, 1);
  CHECK(r.link->captureRing(t) != nullptr);
  const size_t undo0 = r.store.history().cursor();
  REQUIRE(r.link->captureMidi(0));
  CHECK(r.store.history().cursor() == undo0 + 1);  // one undo step
  {
    const Track& tr = r.track(t);
    REQUIRE(tr.clips.size() == 1);
    const NoteClip& c = *tr.clips[0].clip;
    CHECK_FALSE(c.loop.enabled);
    CHECK(c.start == 0);  // the bar of the first note
    CHECK(c.length % kTicksPerWhole == 0);
    const std::vector<Note> notes = r.notesOf(tr.clips[0]);
    REQUIRE(notes.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
      CAPTURE(i);
      const int64_t s0 = r.sampleAt(c.start + notes[i].start), s1 = r.sampleAt(c.start + notes[i].start + notes[i].length);
      CHECK(std::llabs(s0 - played[i].on) <= 48);  // ±1 ms
      CHECK(std::llabs(s1 - played[i].off) <= 48);
      CHECK(notes[i].velocity == 100.0f);
      CHECK(notes[i].releaseVelocity == doctest::Approx(50.0f).epsilon(0.02));
    }
  }
  // Since the last capture: only the new note, 1.5 bars later (the silence is shorter than 2 bars).
  r.run(static_cast<uint32_t>(1.5 * 2.0 * kRate / kBlock));
  played.clear();
  play(62, 20, 5);
  REQUIRE(r.link->captureMidi(0));
  {
    const Track& tr = r.track(t);
    REQUIRE(tr.clips.size() == 2);
    const std::vector<Note> notes = r.notesOf(tr.clips[1]);
    REQUIRE(notes.size() == 1);
    CHECK(std::llabs(r.sampleAt(tr.clips[1].clip->start + notes[0].start) - played[0].on) <= 48);
  }
  // The last phrase: two notes, a 2.5-bar silence, two notes: only the last two are captured.
  r.run(static_cast<uint32_t>(3.0 * 2.0 * kRate / kBlock));
  play(70, 10, 10);
  play(71, 10, 0);
  r.run(static_cast<uint32_t>(2.5 * 2.0 * kRate / kBlock));
  played.clear();
  play(72, 10, 10);
  play(73, 10, 0);
  REQUIRE(r.link->captureMidi(0));
  {
    const Track& tr = r.track(t);
    REQUIRE(tr.clips.size() == 3);
    const std::vector<Note> notes = r.notesOf(tr.clips[2]);
    REQUIRE(notes.size() == 2);
    CHECK(std::llabs(r.sampleAt(tr.clips[2].clip->start + notes[0].start) - played[0].on) <= 48);
    CHECK(std::llabs(r.sampleAt(tr.clips[2].clip->start + notes[1].start) - played[1].on) <= 48);
  }
  CHECK_FALSE(r.link->captureMidi(0));  // nothing new
}

TEST_CASE("T6.9 capture: stopped at 97 BPM, the tempo becomes 97 ±1 and a one-bar loop lands at the insert marker's bar") {
  Rig r;
  const TrackId t = midiTrack(r.store, World::Edo31, NoteInputMode::Keys);
  REQUIRE_FALSE(ops::projectHasClips(*r.store.current()));
  r.start();
  r.run(10);
  const double beat = 60.0 / 97.0 * kRate;  // samples
  const int64_t t0 = r.time + 256;
  // One bar: accented quarters (velocity 120) and soft offbeat eighths (10), each a sixteenth long.
  for (int k = 0; k < 8; ++k) {
    const auto on = t0 + static_cast<int64_t>(std::llround(k * beat / 2.0));
    const auto off = on + static_cast<int64_t>(std::llround(beat / 4.0));
    r.runTo(on);
    r.push(midi(0x90, static_cast<uint8_t>(60 + k), k % 2 == 0 ? 120 : 10));
    r.runTo(off);
    r.push(midi(0x80, static_cast<uint8_t>(60 + k), 64));
  }
  r.run(100);
  REQUIRE_FALSE(r.link->playhead().playing);
  const Snapshot before = r.store.current();
  const Ticks marker = 2 * kTicksPerWhole + kPpq + 17;  // inside bar 3
  REQUIRE(r.link->captureMidi(marker));
  const Project& p = *r.store.current();
  CHECK(p.timeline->tempo.front().bpm == doctest::Approx(97.0).epsilon(1.0 / 97.0));  // ±1 BPM
  const Track& tr = r.track(t);
  REQUIRE(tr.clips.size() == 1);
  const NoteClip& c = *tr.clips[0].clip;
  CHECK(c.start == 2 * kTicksPerWhole);  // the insert marker's bar
  CHECK(c.length == kTicksPerWhole);     // one 4/4 bar
  CHECK(c.loop.enabled);
  CHECK(c.loop.start == 0);
  CHECK(c.loop.end == kTicksPerWhole);
  const std::vector<Note> notes = r.notesOf(tr.clips[0]);
  REQUIRE(notes.size() == 8);
  CHECK(notes[0].start == 0);
  for (int k = 0; k < 8; ++k) {
    CAPTURE(k);
    // On the half-beat grid within the blocks' quantisation (64 frames ≈ 1.3 ms) and the tempo's rounding.
    CHECK(std::llabs(notes[static_cast<size_t>(k)].start - k * kPpq / 2) < kPpq / 50);
  }
  // ONE undo step: the tempo back, the clip gone.
  REQUIRE(r.store.undo());
  CHECK(r.store.current()->timeline->tempo.front().bpm == before->timeline->tempo.front().bpm);
  CHECK(r.track(t).clips.empty());
  // With a clip in the project the tempo stays (the notes keep their real time at it).
  REQUIRE(r.store.redo());
  const double tempo = r.store.current()->timeline->tempo.front().bpm;
  r.run(10);
  const int64_t t1 = r.time;
  for (int k = 0; k < 4; ++k) {
    r.runTo(t1 + static_cast<int64_t>(std::llround(k * 0.5 * kRate)));  // 120 BPM quarters
    r.push(midi(0x90, 60, 100));
    r.run(20);
    r.push(midi(0x80, 60, 64));
  }
  r.run(10);
  REQUIRE(r.link->captureMidi(8 * kTicksPerWhole));
  CHECK(r.store.current()->timeline->tempo.front().bpm == tempo);
  const Track& tr2 = r.track(t);
  REQUIRE(tr2.clips.size() == 2);
  const std::vector<Note> n2 = r.notesOf(tr2.clips[1]);
  REQUIRE(n2.size() == 4);
  const double ticksPerSecond = tempo * kPpq / 60.0;
  CHECK(std::llabs(n2[1].start - static_cast<Ticks>(0.5 * ticksPerSecond)) < kPpq / 50);
}

TEST_CASE("T6.9 capture: Keys and Hz identities on 31-EDO and a MOS (Hz within 0.001 cent), the computer keyboard too") {
  for (const World w : {World::Edo31, World::Mos})
    for (const NoteInputMode mode : {NoteInputMode::Keys, NoteInputMode::Hz}) {
      CAPTURE(static_cast<int>(w));
      CAPTURE(static_cast<int>(mode));
      Rig r;
      const TrackId t = midiTrack(r.store, w, mode);
      r.start();
      r.run(10);
      const uint8_t k1 = 78, k2 = 55;
      r.push(midi(0xE0, 0, 0x50));  // bend +50 cents (of a 2-semitone range)
      r.run(2);
      r.push(midi(0x90, k1, 100));
      r.run(50);
      r.push(midi(0x80, k1, 40));
      r.push(midi(0xE0, 0, 0x40));  // centre
      r.run(10);
      r.push(midi(0x90, k2, 90));
      r.run(30);
      r.link->liveKey(1, 3, 100.0f, true);  // the computer keyboard: equave +1, step 3
      r.run(30);
      r.link->liveKey(1, 3, 0.0f, false);
      r.run(10);  // k2 still held: it ends at the capture
      REQUIRE(r.link->captureMidi(0));
      const Track& tr = r.track(t);
      REQUIRE(tr.clips.size() == 1);
      std::vector<Note> notes = r.notesOf(tr.clips[0]);
      REQUIRE(notes.size() == 3);
      std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
      auto map = buildInputPitchMap(*r.store.current(), t);
      REQUIRE(map.ok());
      const InputPitch& p1 = (*map)->at(k1);
      const InputPitch& p2 = (*map)->at(k2);
      const InputPitch pk = (*map)->pitchForDegree(1 * (*map)->degreesPerEquave() + 3);
      REQUIRE(p1.mapped);
      REQUIRE(p2.mapped);
      REQUIRE(pk.mapped);
      CHECK(notes[1].pitch == p2.id);
      CHECK(notes[2].pitch == pk.id);
      if (mode == NoteInputMode::Keys) {
        CHECK(notes[0].pitch.kind() == tuning::PitchKind::Degree);
        CHECK(notes[0].pitch == p1.id);  // the bend moved the voice; the identity is the key's degree
        CHECK(notes[2].pitch.kind() == tuning::PitchKind::Degree);
      } else {
        REQUIRE(notes[0].pitch.kind() == tuning::PitchKind::Cents);
        const TuningDef def = worldDef(w);
        const double want = 1200.0 * std::log2(p1.hz * std::exp2(50.0 / 1200.0) / def.referenceHz);
        CHECK(std::fabs(notes[0].pitch.cents() - want) < 0.001);  // the note-on bend frozen in, not a 128-key grid
        CHECK(std::fabs(notes[1].pitch.cents() - 1200.0 * std::log2(p2.hz / def.referenceHz)) < 0.001);
        CHECK(notes[2].pitch.kind() == tuning::PitchKind::Cents);
      }
    }
}

TEST_CASE("T6.9 capture: Ctrl+Shift+C through the window's key dispatch captures; Ctrl+Z undoes it in one step") {
  DocumentStore st{playback_fixture::unwiredDefaultProject()};
  const TrackId t = midiTrack(st, World::Edo31, NoteInputMode::Keys);
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  auto l = ProjectLink::create(c, st.current());
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  std::unique_ptr<ProjectLink> link = std::move(*l);
  auto ctx = std::make_unique<views::ViewContext>(*link);
  ctx->store().reset(st.current());
  link->attach(*ctx);
  ctx->setDeviceLibrary(&link->devices());
  ui::TestDriver d{1280, 800};
  auto s = views::Shell::create(d.tree(), *ctx);
  REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
  std::unique_ptr<views::Shell> shell = std::move(*s);
  d.tree().layoutNow();
  d.step();
  link->setLivePorts({"Port"}, {2.0f});
  link->flush();
  int64_t time = 0;
  float lb[kBlock], rb[kBlock];
  float* outs[2] = {lb, rb};
  auto run = [&](uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      RtScope rt;
      link->engine().process(engine::AudioBlock{nullptr, 0, outs, 2, kBlock, time});
      time += kBlock;
    }
  };
  run(10);
  for (int k = 0; k < 3; ++k) {
    REQUIRE(link->livePorts()->push(0, midi(0x90, static_cast<uint8_t>(60 + k), 100)));
    run(40);
    REQUIRE(link->livePorts()->push(0, midi(0x80, static_cast<uint8_t>(60 + k), 64)));
    run(20);
  }
  auto clipsOf = [&] { return findTrackPtr(*ctx->doc(), t)->clips.size(); };
  REQUIRE(clipsOf() == 0);
  d.press("Ctrl+Shift+C");
  d.step();
  REQUIRE(clipsOf() == 1);
  CHECK(findTrackPtr(*ctx->doc(), t)->clips[0].clip->notes.size() == 3);
  CHECK(ctx->store().history().cursor() >= 1);
  d.press("Ctrl+Z");
  d.step();
  CHECK(clipsOf() == 0);
  // Nothing new: the status line says so, no undo step.
  const size_t undo = ctx->store().history().cursor();
  d.press("Ctrl+Shift+C");
  d.step();
  CHECK(ctx->store().history().cursor() == undo);
  d.tree().setRoot(std::make_unique<ui::Widget>());
  shell.reset();
  link->detach();
  ctx.reset();
  link.reset();
}
