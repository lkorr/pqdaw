// T6.4: live MIDI input through the playback link — a live note sounds exactly as the same note drawn in a clip of the
// track (bit for bit) in both note worlds (Keys: Degree identities following the tuning; Hz: Cents identities) across
// 12-EDO, 31-EDO, a MOS whose generator is automated while the note is held, and a Scala scale with its .kbm; routes
// (exclusive / additive arm, monitor, MIDI From port and channel) are rebuilt from the document without a compile; the
// computer keyboard's degrees; the MIDI service over a fake source (open / close / All Notes Off / settings).
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/midi_input.h"
#include "pq/app/project_link.h"
#include "pq/core/rt_scope.h"
#include "pq/document/input_map.h"
#include "pq/tuning/scala.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr double kGenFrom = 700.0, kGenTo = 740.0;  // the MOS case's generator lane over the first bar (2 s)

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;  // interpreted: the comparison is between two renders of the same program
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

enum class Case { Edo12, Edo31, MosAutomated, ScalaKbm };

TuningDef defOf(Case c) {
  TuningDef d;
  switch (c) {
    case Case::Edo12: d.referenceHz = 440.0 / std::exp2(9.0 / 12.0); break;
    case Case::Edo31: d.system = tuning::Edo::make(31).value(); break;
    case Case::MosAutomated: {
      tuning::MosParams m;
      m.n = 7;
      m.period = 1200.0;
      m.generator = kGenFrom;
      d.system = m;
      break;
    }
    case Case::ScalaKbm: {
      auto scl = tuning::parseScl("! t.scl\nfour notes\n 4\n 9/8\n 386.31371386\n 3/2\n 2/1\n");
      REQUIRE(scl.ok());
      auto kbm = tuning::parseKbm("4\n0\n127\n60\n69\n440.0\n4\n0\n1\nx\n3\n");
      REQUIRE(kbm.ok());
      auto s = makeScaleDef(std::move(scl).value(), std::move(kbm).value());
      REQUIRE(s.ok());
      d = std::move(s).value();
      break;
    }
  }
  return d;
}

// One MIDI track in tuning `c` (its main tuning), armed, with the default instrument; the MOS case has a generator lane
// 700 → 740 ¢ over the first bar.
struct LiveFixture {
  DocumentStore store{playback_fixture::unwiredDefaultProject()};
  TrackId track;
  TuningId tuning;
  explicit LiveFixture(Case c, NoteInputMode mode) {
    IdSource& ids = store.ids();
    track = ids.track();
    tuning = ids.tuning();
    Batch b{"live fixture", {}};
    b.commands.push_back(AddTuning{TuningSlot{tuning, "t", Colour{0x2266AA}, defOf(c)}});
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Live"), {}, kAppend});
    b.commands.push_back(SetTrackTuning{track, TuningAssignment::mainTuning(tuning)});
    b.commands.push_back(SetNoteInput{track, NoteInput{mode, InputKeyMap::Tuning}});
    if (c == Case::MosAutomated) {
      AutomationLane lane;
      lane.id = ids.lane();
      lane.param = ParamRef::tuning(ParamKind::MosGenerator, tuning);
      lane.points = {{0, static_cast<float>(kGenFrom), 0.0f, ids.breakpoint()},
                     {kTicksPerWhole, static_cast<float>(kGenTo), 0.0f, ids.breakpoint()}};
      b.commands.push_back(AddAutomationLane{track, std::make_shared<const AutomationLane>(lane)});
    }
    (void)playback_fixture::instrument(b, ids, track, ids.device());
    REQUIRE(store.dispatch(b).ok());
    playback_fixture::wireSeries(store);
    auto arm = ops::armTrack(*store.current(), track, false);
    REQUIRE(arm.ok());
    REQUIRE(store.dispatch(arm->command).ok());
  }
  void drawNote(const tuning::PitchId& id, Ticks length) {
    Note n;
    n.id = store.ids().note();
    n.start = 0;
    n.length = length;
    n.pitch = id;
    n.velocity = 100.0f;
    n.releaseVelocity = 64.0f;
    REQUIRE(store.dispatch(AddClip{track, playback_fixture::clip(store.ids(), 0, 2 * kTicksPerWhole, {n}, "drawn")}).ok());
  }
};

// Plays the transport from 0 and runs `blocks` engine blocks; `before(block)` may push live input first.
template <class F>
std::vector<float> run(ProjectLink& link, uint32_t blocks, F&& before) {
  link.flush();
  engine::Engine& e = link.engine();
  REQUIRE(e.post(engine::TransportCommand{engine::TransportCommandKind::Play, false, 0, 0, 0}));
  std::vector<float> out(static_cast<size_t>(blocks) * kBlock);
  float l[kBlock], r[kBlock];
  float* outs[2] = {l, r};
  for (uint32_t b = 0; b < blocks; ++b) {
    before(b);
    {
      RtScope rt;
      e.process(engine::AudioBlock{nullptr, 0, outs, 2, kBlock, 0, 0, 0});
    }
    std::copy(l, l + kBlock, out.begin() + static_cast<std::ptrdiff_t>(b) * kBlock);
    if ((b & 63) == 0) e.collect();
  }
  return out;
}

engine::RawMidi midi(uint8_t port, uint8_t status, uint8_t key, uint8_t vel) {
  engine::RawMidi m;
  m.port = port;
  m.status = status;
  m.d1 = key;
  m.d2 = vel;
  return m;  // no stamp: it plays at the start of the block that drains it (as a drawn note at a block boundary)
}

// Frequency of a steady stretch [from, to) seconds: upward zero crossings, linearly interpolated.
double hzBetween(const std::vector<float>& x, double from, double to) {
  std::vector<double> at;
  const auto i0 = static_cast<size_t>(from * kRate), i1 = std::min(x.size(), static_cast<size_t>(to * kRate));
  for (size_t i = std::max<size_t>(i0, 1); i < i1; ++i)
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) at.push_back(static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]));
  REQUIRE(at.size() > 4);
  return (static_cast<double>(at.size()) - 1.0) / ((at.back() - at.front()) / kRate);
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
  double m = 0.0;
  for (size_t i = 0; i < a.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
  return m;
}

double peak(const std::vector<float>& a) {
  double m = 0.0;
  for (float v : a) m = std::max(m, static_cast<double>(std::fabs(v)));
  return m;
}

}  // namespace

TEST_CASE("live input: a live note sounds exactly as the same note drawn (Keys and Hz; 12-EDO, 31-EDO, automated MOS, "
          ".kbm)") {
  constexpr uint32_t kNoteBlocks = 48000 / kBlock;      // 1 s = 2 quarter notes at 120 BPM: a block boundary
  constexpr uint32_t kBlocks = 72000 / kBlock;          // 1.5 s
  struct Want {
    Case c;
    uint8_t key;
    const char* name;
  };
  const Want cases[] = {{Case::Edo12, 69, "12-EDO A4"}, {Case::Edo31, 78, "31-EDO degree 18"},
                        {Case::MosAutomated, 64, "5L2s degree 4, generator automated"},
                        {Case::ScalaKbm, 63, ".kbm key 63 = degree 3"}};
  for (const Want& w : cases)
    for (const NoteInputMode mode : {NoteInputMode::Keys, NoteInputMode::Hz}) {
      const std::string which = std::string(w.name) + (mode == NoteInputMode::Hz ? ", Hz" : ", Keys");
      CAPTURE(which);
      LiveFixture live(w.c, mode);
      // The identity the key gives in this world (what a recording stores), drawn as a note in a second project.
      auto map = buildInputPitchMap(*live.store.current(), live.track);
      REQUIRE(map.ok());
      const InputPitch& ip = (*map)->at(w.key);
      REQUIRE(ip.mapped);
      if (mode == NoteInputMode::Keys) {
        CHECK(ip.id.kind() == tuning::PitchKind::Degree);
        CHECK(ip.follow == (w.c == Case::ScalaKbm ? PitchFollow::Static : PitchFollow::Degree));
      } else {
        CHECK(ip.id.kind() == tuning::PitchKind::Cents);  // never limited to a key grid of its own
        CHECK(ip.follow == (w.c == Case::ScalaKbm ? PitchFollow::Static : PitchFollow::Reference));
      }
      LiveFixture drawn(w.c, mode);
      drawn.drawNote(ip.id, 2 * kPpq);

      auto linkD = makeLink(drawn.store.current());
      const std::vector<float> d = run(*linkD, kBlocks, [](uint32_t) {});
      auto linkL = makeLink(live.store.current());
      linkL->setLivePorts({"Test Port"}, {2.0f});  // port 0 open: the track (All Ins) listens to it
      CHECK(linkL->liveRouteTracks() == 1);
      const uint64_t compiles = linkL->stats().compiles;
      auto& ports = *linkL->livePorts();
      const std::vector<float> l = run(*linkL, kBlocks, [&](uint32_t b) {
        if (b == 0) REQUIRE(ports.push(0, midi(0, 0x90, w.key, 100)));
        if (b == kNoteBlocks) REQUIRE(ports.push(0, midi(0, 0x80, w.key, 64)));
      });
      CHECK(linkL->stats().compiles == compiles);
      CHECK(peak(d) > 0.01);  // something sounded
      CHECK(maxDiff(d, l) == 0.0);  // bit for bit
      CHECK(linkL->engine().liveInput().heldNotes() == 0);
      if (w.c == Case::MosAutomated) {
        // Keys: the held live note follows the generator (degree 4 of 5L2s = 4 generators up, folded); Hz: it keeps its Hz.
        const double early = hzBetween(l, 0.05, 0.15), late = hzBetween(l, 0.85, 0.95);
        const double moved = 1200.0 * std::log2(late / early);
        MESSAGE(which << ": the held live note moved " << moved << " cents with the generator lane");
        if (mode == NoteInputMode::Keys) CHECK(std::fabs(moved) > 5.0);
        else CHECK(std::fabs(moved) < 0.5);
      }
    }
}

// T6.4b (Q?-T6.4-a option B): a bent Keys note keeps its bend while its tuning node is modulated (a generator lane
// moving under the held note): out Hz = the tuned Hz of the drawn (unbent) note × the bend ratio, at every point of the
// sweep, and the bend still glides in and out.
TEST_CASE("live input: a bent Keys note keeps its bend under generator automation (T6.4b)") {
  constexpr uint32_t kBlocks = 48000 / kBlock;  // 1 s, the note held throughout; the lane moves 700 → 720 ¢ meanwhile
  constexpr uint8_t kKey = 64;
  for (const NoteInputMode mode : {NoteInputMode::Keys, NoteInputMode::Hz}) {
    CAPTURE(mode == NoteInputMode::Keys ? "Keys" : "Hz");
    LiveFixture live(Case::MosAutomated, mode);
    auto map = buildInputPitchMap(*live.store.current(), live.track);
    REQUIRE(map.ok());
    LiveFixture drawn(Case::MosAutomated, mode);
    drawn.drawNote((*map)->at(kKey).id, 4 * kPpq);
    auto linkD = makeLink(drawn.store.current());
    const std::vector<float> d = run(*linkD, kBlocks, [](uint32_t) {});
    auto linkL = makeLink(live.store.current());
    linkL->setLivePorts({"Test Port"}, {2.0f});  // ±2 semitones
    auto& ports = *linkL->livePorts();
    // Full-scale up (+200 ¢) from block 0, a quarter-range down (−100 ¢) at 0.5 s.
    const std::vector<float> l = run(*linkL, kBlocks, [&](uint32_t b) {
      if (b == 0) {
        REQUIRE(ports.push(0, midi(0, 0xE0, 0x7F, 0x7F)));  // 16383 → +199.98 ¢
        REQUIRE(ports.push(0, midi(0, 0x90, kKey, 100)));
      }
      if (b == 24000 / kBlock) REQUIRE(ports.push(0, midi(0, 0xE0, 0x00, 0x20)));  // 4096 → −100 ¢
    });
    const double up = 200.0 * (16383.0 - 8192.0) / 8192.0, down = -100.0;
    for (const auto& [from, to, cents] : {std::tuple{0.05, 0.12, up}, std::tuple{0.30, 0.40, up},
                                          std::tuple{0.60, 0.70, down}, std::tuple{0.85, 0.95, down}}) {
      CAPTURE(from);
      const double bent = 1200.0 * std::log2(hzBetween(l, from, to) / hzBetween(d, from, to));
      CHECK(std::fabs(bent - cents) < 0.05);
    }
    // The sweep is heard in Keys (the tuned Hz moves), not in Hz mode.
    const double moved = 1200.0 * std::log2(hzBetween(d, 0.85, 0.95) / hzBetween(d, 0.05, 0.12));
    if (mode == NoteInputMode::Keys) CHECK(std::fabs(moved) > 2.0);
    else CHECK(std::fabs(moved) < 0.5);
  }
}

TEST_CASE("live input: an unmapped .kbm key plays nothing; the computer keyboard plays degrees in each track's world") {
  LiveFixture f(Case::ScalaKbm, NoteInputMode::Keys);
  auto link = makeLink(f.store.current());
  link->setLivePorts({"Port"}, {2.0f});
  auto& ports = *link->livePorts();
  const std::vector<float> silent = run(*link, 200, [&](uint32_t b) {
    if (b == 0) REQUIRE(ports.push(0, midi(0, 0x90, 62, 100)));  // 'x' in the .kbm
    if (b == 100) REQUIRE(ports.push(0, midi(0, 0x90, 63, 100)));  // mapped: it sounds (the route works)
  });
  CHECK(peak(std::vector<float>(silent.begin(), silent.begin() + 100 * kBlock)) == 0.0);
  CHECK(peak(silent) > 0.01);
  REQUIRE(ports.push(0, midi(0, 0x80, 63, 0)));
  (void)run(*link, 1, [](uint32_t) {});
  CHECK(link->engine().liveInput().heldNotes() == 0);

  // 31-EDO, Keys and Hz: keyboard step s at equave e = degree e·31 + s of the track's tuning.
  for (const NoteInputMode mode : {NoteInputMode::Keys, NoteInputMode::Hz}) {
    LiveFixture k(Case::Edo31, mode);
    auto lk = makeLink(k.store.current());
    lk->engine().liveInput().setRecording(true);
    lk->liveKey(1, 4, 90.0f, true);
    (void)run(*lk, 2, [](uint32_t) {});
    engine::LiveNoteRecord rec;
    REQUIRE(lk->engine().liveInput().popRecord(rec));
    auto map = buildInputPitchMap(*k.store.current(), k.track);
    REQUIRE(map.ok());
    const InputPitch want = (*map)->pitchForDegree(31 + 4);
    CHECK(rec.pitch.id == want.id);
    CHECK(rec.pitch.hz == static_cast<float>(want.hz));
    CHECK(rec.velocity == 90.0f);
    CHECK(rec.raw.port == engine::kLiveKeyboard);
    if (mode == NoteInputMode::Keys) CHECK(rec.pitch.id == tuning::PitchId::fromDegree(4, 1));
    else CHECK(rec.pitch.id.kind() == tuning::PitchKind::Cents);
    lk->liveKey(1, 4, 0.0f, false);
    (void)run(*lk, 2, [](uint32_t) {});
    CHECK(lk->engine().liveInput().heldNotes() == 0);
  }
}

TEST_CASE("live input (KB.2): the keyboard's and MIDI keys' tables play the track's live clip's tuning") {
  // A 12-EDO track (Keys) with clip A [0, 4) bars in a named 22-EDO and clip B [4, 8) inheriting; a ViewContext on the
  // link (the clip view's clip and the start marker come from it, as in the app).
  LiveFixture f(Case::Edo12, NoteInputMode::Keys);
  constexpr Ticks kBar = kTicksPerWhole;
  const ClipPtr ca = playback_fixture::clip(f.store.ids(), 0, 4 * kBar, {}, "A");
  const ClipPtr cb = playback_fixture::clip(f.store.ids(), 4 * kBar, 4 * kBar, {}, "B");
  REQUIRE(f.store.dispatch(AddClip{f.track, ca}).ok());
  REQUIRE(f.store.dispatch(AddClip{f.track, cb}).ok());
  TuningDef edo22;
  edo22.system = tuning::Edo::make(22).value();
  const TuningId t22 = f.store.ids().tuning();  // TU.3: a named tuning
  REQUIRE(f.store.dispatch(AddTuning{TuningSlot{t22, "Tuning B", Colour{0x808080}, edo22}}).ok());
  REQUIRE(f.store.dispatch(SetClipTuning{{f.track, ca->id}, TuningAssignment::mainTuning(t22)}).ok());
  auto link = makeLink(f.store.current());
  views::ViewContext ctx(*link);
  ctx.store().reset(f.store.current());
  link->attach(ctx);
  struct Detach {
    ProjectLink& l;
    ~Detach() { l.detach(); }  // before the context goes
  } detach{*link};
  link->setLivePorts({"Port"}, {2.0f});
  engine::LiveInput& in = link->engine().liveInput();
  in.setRecording(true);
  auto blocks = [&](uint32_t n) {  // runs the engine without touching the transport
    float l[kBlock], r[kBlock];
    float* outs[2] = {l, r};
    for (uint32_t b = 0; b < n; ++b) {
      RtScope rt;
      link->engine().process(engine::AudioBlock{nullptr, 0, outs, 2, kBlock, 0, 0, 0});
    }
  };
  // What the keyboard's (equave 0, step 3) and MIDI key 63 play now, and what they should in clip `c`'s tuning (want).
  auto keyboardHz = [&] {
    link->liveKey(0, 3, 100.0f, true);
    blocks(2);
    engine::LiveNoteRecord rec;
    REQUIRE(in.popRecord(rec));
    CHECK(rec.pitch.id == tuning::PitchId::fromDegree(3, 0));
    link->liveKey(0, 3, 0.0f, false);
    blocks(1);
    for (engine::LiveNoteRecord off; in.popRecord(off);) {}
    return rec.pitch.hz;
  };
  auto midiHz = [&] {
    REQUIRE(link->livePorts()->push(0, midi(0, 0x90, 63, 100)));
    blocks(2);
    engine::LiveNoteRecord rec;
    REQUIRE(in.popRecord(rec));
    REQUIRE(link->livePorts()->push(0, midi(0, 0x80, 63, 0)));
    blocks(1);
    for (engine::LiveNoteRecord off; in.popRecord(off);) {}
    return rec.pitch.hz;
  };
  auto want = [&](ClipId c, int64_t degree) {  // invalid: the track's own tuning
    const Track& t = *findTrackPtr(*ctx.doc(), f.track);
    const int32_t i = findClipIndex(t, c);
    const NoteClip* clip = i >= 0 ? t.clips[static_cast<size_t>(i)].clip.get() : nullptr;
    auto map = buildInputPitchMap(*ctx.doc(), f.track, t.noteInput, nullptr, clip);
    REQUIRE(map.ok());
    return static_cast<float>((*map)->pitchForDegree(degree).hz);
  };
  const float hz22 = want(ca->id, 3), hz12 = want(ClipId{}, 3);
  REQUIRE(hz22 != hz12);
  // Stopped, nothing open, the start marker (0) over A: 22-EDO (the keyboard and MIDI key 63 = degree 3 alike).
  CHECK(keyboardHz() == hz22);
  CHECK(midiHz() == hz22);
  // The marker over B: 12-EDO; moving it inside B publishes nothing.
  ctx.setStartMarker(5 * kBar);
  CHECK(keyboardHz() == hz12);
  CHECK(midiHz() == hz12);
  uint64_t publishes = link->liveRoutePublishes();
  ctx.setStartMarker(6 * kBar);
  CHECK(link->liveRoutePublishes() == publishes);
  // A open in the clip view: 22-EDO wherever the marker is; closed again: B's 12-EDO.
  ctx.setDetailClip({f.track, ca->id});
  ctx.setDetailMode(views::DetailMode::Clip);
  CHECK(keyboardHz() == hz22);
  ctx.setDetailVisible(false);
  CHECK(keyboardHz() == hz12);
  // Playing from 0 (over A) with the marker over B: the playhead decides (playhead() re-resolves when it crosses).
  link->play(0);
  (void)link->playhead();
  CHECK(keyboardHz() == hz22);
  link->locate(5 * kBar);
  (void)link->playhead();
  CHECK(keyboardHz() == hz12);
  // Polling while the playhead stays inside B publishes nothing; B retuned while under the playhead: its new tuning.
  publishes = link->liveRoutePublishes();
  (void)link->playhead();
  CHECK(link->liveRoutePublishes() == publishes);
  REQUIRE(ctx.store().dispatch(SetClipTuning{{f.track, cb->id}, TuningAssignment::mainTuning(t22)}).ok());
  CHECK(keyboardHz() == hz22);
  link->stop();
}

TEST_CASE("live input: routes follow arm (exclusive / additive), monitor and MIDI From, without a compile") {
  DocumentStore store{playback_fixture::unwiredDefaultProject()};
  IdSource& ids = store.ids();
  const TrackId a = ids.track(), b = ids.track();
  {
    Batch bt{"two tracks", {}};
    bt.commands.push_back(AddTrack{playback_fixture::track(a, TrackKind::Midi, "A"), {}, kAppend});
    bt.commands.push_back(AddTrack{playback_fixture::track(b, TrackKind::Midi, "B"), {}, kAppend});
    (void)playback_fixture::instrument(bt, ids, a, ids.device());
    (void)playback_fixture::instrument(bt, ids, b, ids.device());
    REQUIRE(store.dispatch(bt).ok());
    playback_fixture::wireSeries(store);
  }
  auto link = makeLink(store.current());
  auto apply = [&](const Command& c) {
    const Snapshot before = store.current();
    REQUIRE(store.dispatch(c).ok());
    link->onDocumentChange(StoreEvent{StoreEventKind::Dispatch, before, store.current(), store.lastChanges()});
  };
  auto arm = [&](TrackId t, bool additive) {
    auto r = ops::armTrack(*store.current(), t, additive);
    REQUIRE(r.ok());
    apply(r->command);
  };
  link->setLivePorts({"Port A", "Port B"}, {2.0f, 2.0f});
  CHECK(link->liveRouteTracks() == 0);
  const uint64_t compiles = link->stats().compiles;
  arm(a, false);
  CHECK(link->liveRouteTracks() == 1);
  arm(b, false);  // exclusive: A is disarmed
  CHECK(link->liveRouteTracks() == 1);
  arm(a, true);   // additive
  CHECK(link->liveRouteTracks() == 2);
  apply(SetTrackMidiInput{b, MidiInput{MidiInputKind::Port, "Port B", 2}});
  engine::LiveInput& in = link->engine().liveInput();
  auto& ports = *link->livePorts();
  auto block = [&] { (void)run(*link, 1, [](uint32_t) {}); };
  REQUIRE(ports.push(0, midi(0, 0x90, 60, 100)));  // Port A ch 1: A only
  block();
  CHECK(in.heldNotes() == 1);
  REQUIRE(ports.push(1, midi(1, 0x91, 62, 100)));  // Port B ch 2: A (All) and B
  block();
  CHECK(in.heldNotes() == 3);
  REQUIRE(ports.push(1, midi(1, 0x90, 64, 100)));  // Port B ch 1: A only
  block();
  CHECK(in.heldNotes() == 4);
  // Monitor Off: A stops listening; its held notes still end on their note-offs.
  apply(SetTrackMonitor{a, MonitorMode::Off});
  CHECK(link->liveRouteTracks() == 1);
  REQUIRE(ports.push(0, midi(0, 0x80, 60, 0)));
  REQUIRE(ports.push(1, midi(1, 0x81, 62, 0)));
  REQUIRE(ports.push(1, midi(1, 0x80, 64, 0)));
  block();
  CHECK(in.heldNotes() == 0);
  // Monitor In: an unarmed track listens.
  apply(SetTrackArm{a, false});
  apply(SetTrackArm{b, false});  // nothing armed
  CHECK(link->liveRouteTracks() == 0);
  // KB.1: the auto-armed track (Live's arm on select) listens as if armed (B, monitor Auto); 0 ends it.
  link->setAutoArm(b.value);
  CHECK(link->liveRouteTracks() == 1);
  CHECK(link->autoArm() == b.value);
  link->setAutoArm(0);
  CHECK(link->liveRouteTracks() == 0);
  apply(SetTrackMonitor{a, MonitorMode::In});
  CHECK(link->liveRouteTracks() == 1);
  // An edit that changes no route publishes nothing.
  const uint64_t publishes = link->liveRoutePublishes();
  apply(SetTrackVolume{b, -6.0f});
  CHECK(link->liveRoutePublishes() == publishes);
  // A note input change does (new tables); none of it compiled.
  apply(SetNoteInput{a, NoteInput{NoteInputMode::Hz, InputKeyMap::Tuning}});
  CHECK(link->liveRoutePublishes() == publishes + 1);
  link->flush();
  CHECK(link->stats().compiles == compiles);
}

namespace {

// A scripted MIDI source: ports by name, and each open port's callback to inject messages.
struct FakeSource final : app::MidiSource {
  struct Port final : app::MidiInPort {
    FakeSource* src;
    std::string name;
    app::MidiCallback cb;
    void* user;
    ~Port() override { std::erase(src->opened, this); }
  };
  std::vector<std::string> names;
  std::vector<Port*> opened;
  std::vector<std::string> portNames() override { return names; }
  Result<std::unique_ptr<app::MidiInPort>> open_(const std::string& n, app::MidiCallback cb, void* user) {
    auto p = std::make_unique<Port>();
    p->src = this;
    p->name = n;
    p->cb = cb;
    p->user = user;
    opened.push_back(p.get());
    return std::unique_ptr<app::MidiInPort>(std::move(p));
  }
  Result<std::unique_ptr<app::MidiInPort>> open(const std::string& n, app::MidiCallback cb, void* user) override {
    return open_(n, cb, user);
  }
  Port* port(const std::string& n) {
    for (Port* p : opened)
      if (p->name == n) return p;
    return nullptr;
  }
};

}  // namespace

TEST_CASE("live input: the MIDI service opens enabled ports, stamps and pushes, closes with All Notes Off, saves") {
  LiveFixture f(Case::Edo12, NoteInputMode::Keys);
  auto link = makeLink(f.store.current());
  auto src = std::make_unique<FakeSource>();
  FakeSource* fake = src.get();
  fake->names = {"Keys 49", "Pads"};
  std::vector<app::MidiPortSetting> saved;
  int opened = 0;
  {
    app::MidiInputService svc(*link, std::move(src), {{"Pads", false, 2.0f}},
                              [&](const std::vector<app::MidiPortSetting>& v) { saved = v; });
    svc.setOpenListener([&](bool on) { opened += on ? 1 : -1; });
    svc.refresh();
    CHECK(opened == 1);
    REQUIRE(fake->opened.size() == 1);
    CHECK(svc.openPorts()[0] == "Keys 49");
    REQUIRE(saved.size() == 2);  // the new port remembered (on)
    CHECK(saved[1] == app::MidiPortSetting{"Keys 49", true, 2.0f});
    // The callback: stamped and pushed into ring 0; system messages ignored.
    const uint8_t on[3] = {0x90, 69, 100}, clock[1] = {0xF8};
    FakeSource::Port* p = fake->port("Keys 49");
    REQUIRE(p != nullptr);
    {
      RtScope rt;
      p->cb(p->user, clock, 1);
      p->cb(p->user, on, 3);
    }
    (void)run(*link, 1, [](uint32_t) {});
    CHECK(link->engine().liveInput().heldNotes() == 1);
    CHECK(link->engine().liveInput().stats().messages == 1);
    // Unplugged: closed, and its held note released (All Notes Off on every channel).
    fake->names = {"Pads"};
    svc.refresh();
    CHECK(fake->opened.empty());
    CHECK(opened == 0);
    (void)run(*link, 1, [](uint32_t) {});
    CHECK(link->engine().liveInput().heldNotes() == 0);
    // Preferences: the remembered port is listed (not connected); switching Pads on opens it.
    const std::vector<views::MidiPortEntry> list = svc.ports();
    REQUIRE(list.size() == 2);
    CHECK(list[0].name == "Pads");
    CHECK(list[0].connected);
    CHECK_FALSE(list[0].enabled);
    CHECK(list[1].name == "Keys 49");
    CHECK_FALSE(list[1].connected);
    svc.setPort("Pads", true, 12.0f);
    CHECK(fake->opened.size() == 1);
    CHECK(saved[0] == app::MidiPortSetting{"Pads", true, 12.0f});
  }
  CHECK(opened == 0);
}

// KB.3: a MIDI-in note while the engine sleeps (the views' meter polls stopped) notifies the views once, from the MIDI
// callback (no allocation there: RtScope), and live() holds until the audio thread has drained the note.
TEST_CASE("live input (KB.3): a MIDI note into a sleeping engine re-arms the views' signal polls, once") {
  LiveFixture f(Case::Edo12, NoteInputMode::Keys);
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 20.0f;
  c.jit = false;
  auto made = ProjectLink::create(c, f.store.current());
  REQUIRE(made.ok());
  ProjectLink& link = *made.value();
  link.flush();
  engine::Engine& e = link.engine();
  float l[kBlock], r[kBlock];
  float* outs[2] = {l, r};
  auto block = [&] { e.process(engine::AudioBlock{nullptr, 0, outs, 2, kBlock, 0, 0, 0}); };
  auto src = std::make_unique<FakeSource>();
  FakeSource* fake = src.get();
  fake->names = {"Keys 49"};
  int notified = 0;
  {
    app::MidiInputService svc(link, std::move(src), {});
    svc.refresh();  // the routes publish (a wake)
    for (int b = 0; b < 400 && !e.asleep(); ++b) block();
    REQUIRE(e.asleep());
    (void)link.live();  // the views' poll sees the last activity (the publish) …
    std::this_thread::sleep_for(std::chrono::duration<double>(ProjectLink::kSignalWakeSeconds + 0.1));  // … and its grace ends
    REQUIRE_FALSE(link.live());  // the views stop polling here
    link.setSignalNotify([](void* u) { ++*static_cast<int*>(u); }, &notified);
    FakeSource::Port* p = fake->port("Keys 49");
    REQUIRE(p != nullptr);
    const uint8_t on[3] = {0x90, 69, 100}, off[3] = {0x80, 69, 0};
    {
      RtScope rt;
      p->cb(p->user, on, 3);
      p->cb(p->user, off, 3);
    }
    CHECK(notified == 1);
    CHECK(link.live());  // before the audio thread ran: the polls keep going
    block();
    CHECK_FALSE(e.asleep());  // the note woke it
  }
  link.setSignalNotify(nullptr, nullptr);
}

TEST_CASE("live input: the WinMM source enumerates without opening anything") {
  // Opening a port would take the user's controller (a WinMM input is exclusive): only the list is read here.
  auto src = app::makeWinMmSource();
  const std::vector<std::string> names = src->portNames();
  MESSAGE(names.size() << " MIDI input port(s) on this machine");
  for (const std::string& n : names) CHECK_FALSE(n.empty());
}
