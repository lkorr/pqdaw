// T3.17: project lowering and the playback link (pq/document/lower.h, pq/app/project_link.h, pq/engine/engine.h).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/audibility.h"
#include "pq/document/lower.h"
#include "pq/document/serialize.h"
#include "pq/engine/null_device.h"
#include "pq/engine/wav_file.h"
#include "pq/views/view_context.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

LinkConfig syncConfig(float sleepMs = 0.0f) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = sleepMs;
  return c;
}

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s, LinkConfig c = syncConfig()) {
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

golden::Channels render(const Snapshot& s, double seconds) {
  auto link = makeLink(s);
  engine::BufferSink sink;
  auto st = link->renderOffline(0, static_cast<uint64_t>(std::llround(seconds * kRate)), sink);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  return sink.channels;
}

double maxDiff(const golden::Channels& a, const golden::Channels& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = 0; i < a[c].size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[c][i] - b[c][i])));
  }
  return m;
}

float peak(const golden::Channels& a) {
  float p = 0.0f;
  for (const auto& c : a)
    for (float v : c) p = std::max(p, std::fabs(v));
  return p;
}

// Drives a link's engine block by block like a device (RtScope: the trap fails the test on an allocation).
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
};

bool updateGoldens() {
  const char* v = std::getenv("PQ_UPDATE_GOLDENS");
  return v != nullptr && *v != '\0' && *v != '0';
}

fs::path fixtureDir() { return fs::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / "playback"; }

// The same project with every mute / solo replaced by what effectiveAudible says: a closed output gate becomes no
// output routing (post-fader sends keep flowing, as with a closed gate), a closed send gate makes every send −∞;
// no mute or solo flags remain.
Snapshot audibilityReference(DocumentStore& store) {
  const Project& p = *store.current();
  const AudibilityMap aud = computeAudibility(p);
  Batch b{"reference", {}};
  forEachTrack(p, [&](const Track& t, const Track*) {
    if (t.kind == TrackKind::Master) return;
    const TrackAudibility* a = aud.find(t.id);
    REQUIRE(a != nullptr);
    if (t.mixer.mute) b.commands.push_back(SetTrackMute{t.id, false});
    if (t.mixer.solo) b.commands.push_back(SetTrackSolo{t.id, false});
    if (!a->output) b.commands.push_back(SetTrackOutput{t.id, Output{OutputKind::None, {}}});
    if (!a->sends)
      for (const TrackPtr& r : p.returns) b.commands.push_back(SetSendLevel{t.id, r->id, kMinVolumeDb});
  });
  if (!b.commands.empty()) REQUIRE(store.dispatch(b).ok());
  return store.current();
}

}  // namespace

TEST_CASE("lowering: the fixture's tracks, routing, sends and params; unchanged tracks share their patches") {
  auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  playback_fixture::Fixture f;
  LowerOptions lo;
  lo.library = &(*lib)->library();
  lo.externals = (*lib)->signatures();
  uint32_t nextSlot = 0;
  lo.slotOf = [&](TrackId) { return nextSlot++; };
  auto low = lower(*f.snap(), lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  const LoweredProject& lp = low.value();
  REQUIRE(lp.graph.tracks.size() == 6);  // Lead, Grp, Mos, Muted, Echo, Master
  auto idx = [&](TrackId id) {
    for (uint32_t i = 0; i < lp.trackIds.size(); ++i)
      if (lp.trackIds[i] == id) return i;
    FAIL("track missing");
    return 0u;
  };
  CHECK(lp.graph.tracks[idx(f.lead)].role == graph::TrackRole::Instrument);
  CHECK(lp.graph.tracks[idx(f.grp)].role == graph::TrackRole::Group);
  CHECK(lp.graph.tracks[idx(f.echo)].role == graph::TrackRole::Return);
  CHECK(lp.graph.tracks[idx(f.master)].role == graph::TrackRole::Master);
  CHECK(lp.graph.tracks[idx(f.grp)].eventSlot == graph::kNone);
  auto wire = [&](TrackId from, const char* port, TrackId to) {
    for (const auto& w : lp.graph.wires)
      if (w.from.track == idx(from) && w.from.port == port && w.to.track == idx(to) && w.to.port == "in") return true;
    return false;
  };
  auto sendWire = [&](TrackId from, const char* port, TrackId to) {  // T3.17-fix2: sends live in the return
    for (const auto& w : lp.graph.wires)
      if (w.from.track == idx(from) && w.from.port == port && w.to.track == idx(to) &&
          w.to.port == "s" + std::to_string(from.value))
        return true;
    return false;
  };
  CHECK(wire(f.lead, "out", f.master));
  CHECK(wire(f.mos, "out", f.grp));
  CHECK(wire(f.grp, "out", f.master));
  CHECK(wire(f.echo, "out", f.master));
  CHECK(sendWire(f.lead, "post", f.echo));
  CHECK(sendWire(f.muted, "post", f.echo));  // implicit sends at −∞ are params
  CHECK(sendWire(f.grp, "post", f.echo));
  CHECK(lp.graph.wires.size() == 9);
  auto param = [&](const std::string& key) -> const LoweredParam* {
    for (const auto& p : lp.params)
      if (p.key == key) return &p;
    return nullptr;
  };
  const std::string lead = std::to_string(f.lead.value), muted = std::to_string(f.muted.value);
  REQUIRE(param(lead + ":volume") != nullptr);
  CHECK(param(lead + ":volume")->value == doctest::Approx(-3.0));
  CHECK(param(lead + ":pan")->value == doctest::Approx(-0.4));
  CHECK(param(lead + ":send:" + std::to_string(f.echo.value))->value == doctest::Approx(-6.0));
  CHECK(param(lead + ":send:" + std::to_string(f.echo.value))->track == idx(f.echo));  // the return's patch param
  CHECK(param(lead + ":gate")->value == 1.0f);
  // Mute is a param (Q15), its own since T3.17-fix3 (a mute lane plays): the gates are the solo rule without it.
  CHECK(param(muted + ":mute")->value == 1.0f);
  CHECK(param(muted + ":mute")->ref == ParamRef::track(ParamKind::TrackMute, f.muted));
  CHECK(param(muted + ":gate")->value == 1.0f);
  CHECK(param(muted + ":sendGate")->value == 1.0f);
  CHECK(param(lead + ":mute")->value == 0.0f);
  CHECK(param(std::to_string(f.master.value) + ":gate") == nullptr);
  CHECK(param(std::to_string(f.master.value) + ":mute") == nullptr);
  CHECK(paramKey(ParamRef::track(ParamKind::TrackVolume, f.grp)) == std::to_string(f.grp.value) + ":volume");

  // A mute or a volume change does not change any patch text: every patch is shared with the previous lowering.
  REQUIRE(f.store.dispatch(SetTrackMute{f.lead, true}).ok());
  REQUIRE(f.store.dispatch(SetTrackVolume{f.mos, -12.0f}).ok());
  auto again = lower(*f.snap(), lo, &lp);
  REQUIRE(again.ok());
  for (size_t i = 0; i < lp.patches.size(); ++i) CHECK(again.value().patches[i] == lp.patches[i]);
  CHECK(lowerParams(*f.snap(), false, &(*lib)->library()).size() == lp.params.size());

  // Deactivating the group leaves it and its member out (structural, Q15).
  REQUIRE(f.store.dispatch(SetTrackActive{f.grp, false}).ok());
  auto off = lower(*f.snap(), lo, &lp);
  REQUIRE(off.ok());
  CHECK(off.value().graph.tracks.size() == 4);
}

TEST_CASE("golden: the playback fixture (31-EDO, 5L2s, a group with volume automation, a send, a muted track)") {
  playback_fixture::Fixture f;
  const fs::path dir = fixtureDir();
  const fs::path wav = golden::goldenDir() / "audio" / "project_playback.wav";
  constexpr double kSeconds = 2.5;
  if (updateGoldens()) {
    fs::create_directories(dir);
    REQUIRE(saveProject(dir.string(), *f.snap()).ok());
  }
  auto loaded = loadProject(dir.string());
  REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
  CHECK(*loaded->project == *f.snap());  // the committed fixture is the project these tests build

  const golden::Channels out = render(loaded->project, kSeconds);
  REQUIRE(out.size() == 2);
  if (updateGoldens()) {
    fs::create_directories(wav.parent_path());
    REQUIRE(engine::writeWav(wav, out, static_cast<uint32_t>(kRate)).ok());
    MESSAGE("updated " << wav.string());
  }
  auto gold = engine::readWav(wav);
  REQUIRE_MESSAGE(gold.ok(), (gold.ok() ? std::string() : gold.error().message + " (PQ_UPDATE_GOLDENS=1)"));
  const golden::Comparison c = golden::compare(*gold, out, static_cast<uint32_t>(kRate));
  CHECK_MESSAGE(c.ok, c.message);
  // Quiet: the M3 default instrument stays well below full scale.
  CHECK(peak(out) > 0.01f);
  CHECK(peak(out) < 0.5f);
  // Repeatable: a second render is bit-identical.
  CHECK(maxDiff(render(loaded->project, kSeconds), out) == 0.0);
  // The muted track is not heard: unmuting it changes the render.
  REQUIRE(f.store.dispatch(SetTrackMute{f.muted, false}).ok());
  CHECK(maxDiff(render(f.snap(), kSeconds), out) > 0.01);
}

TEST_CASE("solo and mute renders match effectiveAudible") {
  struct Case {
    const char* name;
    std::vector<Command> edits;
    bool audible = true;
  };
  playback_fixture::Fixture ids;  // the ids are the same in every fresh fixture
  std::vector<Case> cases;
  cases.push_back({"solo Lead", {SetTrackSolo{ids.lead, true}}});
  cases.push_back({"solo the group", {SetTrackSolo{ids.grp, true}}});
  cases.push_back({"solo a group member", {SetTrackSolo{ids.mos, true}}});
  cases.push_back({"solo the return only", {SetTrackSolo{ids.echo, true}}});
  cases.push_back({"mute the group", {SetTrackMute{ids.grp, true}}});
  cases.push_back({"solo the muted track", {SetTrackSolo{ids.muted, true}}, false});  // Q89: the activator wins
  cases.push_back({"solo Lead and the return", {SetTrackSolo{ids.lead, true}, SetTrackSolo{ids.echo, true}}});
  for (const Case& k : cases) {
    const std::string name = k.name;
    CAPTURE(name);
    playback_fixture::Fixture f;
    REQUIRE(f.store.dispatch(RemoveAutomationLane{f.grp, f.grpVol}).ok());  // the reference sets volumes
    for (const Command& c : k.edits) REQUIRE(f.store.dispatch(c).ok());
    const Snapshot soloed = f.snap();
    const Snapshot reference = audibilityReference(f.store);
    const golden::Channels a = render(soloed, 1.5), b = render(reference, 1.5);
    CHECK(maxDiff(a, b) <= 1e-7);
    CHECK((peak(a) > 0.001f) == k.audible);
  }
}

TEST_CASE("moving a note or changing a velocity or a volume compiles nothing and takes effect at the next block") {
  playback_fixture::Fixture f;
  auto link = makeLink(f.snap());
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(20);
  const app::LinkStats s0 = link->stats();
  engine::Timeline& tl = link->engine().timeline();
  const uint64_t swaps0 = tl.stats().sequenceSwaps;
  const ClipRef lc{f.lead, f.leadClip};
  const Note first = *findTrackPtr(*f.snap(), f.lead)->clips[0].clip->notes.begin();
  REQUIRE(f.store.dispatch(MoveNote{lc, keyOf(first), first.start + pq::kPpq / 4, first.pitch}).ok());
  link->flush();
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().unitCompiles == s0.unitCompiles);
  CHECK(link->stats().sequencePublishes == s0.sequencePublishes + 1);
  d.run(1);
  CHECK(tl.stats().sequenceSwaps == swaps0 + 1);  // the new sequence plays from the very next block
  bool moved = false;
  for (const auto& n : tl.sequence()->notes()) moved |= n.id == first.id.value && n.start == first.start + pq::kPpq / 4;
  CHECK(moved);

  REQUIRE(f.store.dispatch(SetNoteProperty{lc, {NoteKey{first.start + pq::kPpq / 4, first.id}}, NoteProperty::Velocity, 40.0f}).ok());
  link->flush();
  d.run(1);
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(tl.stats().sequenceSwaps == swaps0 + 2);
  bool velocity = false;
  for (const auto& n : tl.sequence()->notes()) velocity |= n.id == first.id.value && n.velocity == 40.0f;
  CHECK(velocity);

  REQUIRE(f.store.dispatch(SetTrackVolume{f.lead, -20.0f}).ok());
  link->flush();
  d.run(1);
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().unitCompiles == s0.unitCompiles);
  const uint32_t slot = link->paramSlot(std::to_string(f.lead.value) + ":volume");
  REQUIRE(slot != ProjectLink::kNoSlot);
  CHECK(link->engine().program()->projectParams()[slot] == -20.0f);
  // Solo: gates of other tracks change, still no compile.
  REQUIRE(f.store.dispatch(SetTrackSolo{f.lead, true}).ok());
  link->flush();
  d.run(1);
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->engine().program()->projectParams()[link->paramSlot(std::to_string(f.mos.value) + ":gate")] == 0.0f);
}

TEST_CASE("adding a track: exactly one compile, a click-free swap; unchanged units are untouched") {
  playback_fixture::Fixture fa, fb;
  auto a = makeLink(fa.snap()), b = makeLink(fb.snap());
  [[maybe_unused]] const ListenerId sub = fa.store.subscribe([&](const StoreEvent& e) { a->onDocumentChange(e); });
  Driver da{*a}, db{*b};
  a->play(0);
  b->play(0);
  da.run(150);  // notes sounding (0.2 s)
  db.run(150);
  const app::LinkStats s0 = a->stats();
  const TrackId added = fa.store.ids().track();
  REQUIRE(fa.store.dispatch(AddTrack{playback_fixture::track(added, TrackKind::Midi, "New", fa.store.ids().device()), {}, kAppend}).ok());
  a->flush();
  CHECK(a->stats().compiles == s0.compiles + 1);
  CHECK(a->stats().topologyChanges == s0.topologyChanges + 1);
  // The new track's unit, plus the two buses it feeds (master: its output; Echo: its implicit send). The other
  // units (Lead, Grp, Mos, Muted) keep running as they are.
  CHECK(a->stats().unitCompiles == s0.unitCompiles + 3);
  da.run(200);
  db.run(200);
  CHECK(a->engine().program()->stats().topologySwitches == 1);
  CHECK(a->engine().program()->numUnits() == 7);
  // The new track is silent: the output through the swap equals the run without the edit, sample for sample.
  CHECK(maxDiff(da.out, db.out) <= 1e-7);
  CHECK(peak(da.out) > 0.01f);
}

TEST_CASE("removing a sounding track fades it out through its readers' crossfades") {
  playback_fixture::Fixture f;
  auto link = makeLink(f.snap());
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(100);
  const size_t at = d.out[0].size();
  REQUIRE(f.store.dispatch(RemoveTrack{f.lead}).ok());
  link->flush();
  d.run(200);
  CHECK(link->engine().program()->stats().topologySwitches == 1);
  CHECK(link->engine().program()->stats().zombieRuns > 0);
  float step = 0.0f;
  for (int c = 0; c < 2; ++c)
    for (size_t i = at; i + 1 < d.out[c].size(); ++i) step = std::max(step, std::fabs(d.out[c][i + 1] - d.out[c][i]));
  CHECK(step < 0.05f);  // no jump: the synth's own slope at these pitches is ~0.02 per sample at most
}

TEST_CASE("NullDevice end to end: open the fixture, Space, after 1 s the playhead reported to views is within a block") {
  auto link = makeLink(playback_fixture::Fixture().snap(), syncConfig(250.0f));
  views::ViewContext ctx(*link);
  REQUIRE(ctx.openProject(fixtureDir().string()).ok());
  link->attach(ctx);
  link->flush();
  engine::NullDevice dev;
  engine::StreamConfig sc;
  sc.sampleRate = 48000;
  sc.bufferFrames = 256;
  sc.outputChannels = 2;
  REQUIRE(dev.open(sc, link->callback()).ok());
  ctx.actions().invoke(ctx.actions().find("transport.playStop"));  // Space
  CHECK(link->playhead().playing);  // at once, before the audio thread took the command
  const uint32_t blocks = 188;      // 188 × 256 = 48128 frames ≥ 1 s
  views::PlayheadSnapshot ph;
  for (uint32_t i = 0; i < blocks; ++i) {
    dev.processBlock();
    if (i % 8 == 0) ph = link->playhead();  // the views poll on their timers
  }
  ph = link->playhead();
  CHECK(ph.playing);
  const double seconds = ctx.doc()->timeline->tempoMap.ticksToSeconds(ph.tick);
  const double end = blocks * 256.0 / 48000.0;
  MESSAGE("playhead " << seconds << " s after " << end << " s of blocks");
  CHECK(seconds <= end + 1e-9);
  CHECK(end - seconds <= 256.0 / 48000.0 + 1e-9);  // within one device block of where the audio is
  CHECK(link->live());
  // Meters: Lead was heard, the muted track was not.
  std::vector<TrackId> ids;
  forEachTrack(*ctx.doc(), [&](const Track& t, const Track*) { ids.push_back(t.id); });
  std::vector<ui::MeterLevels> levels(ids.size());
  link->read(ids, levels);
  bool heard = false;
  for (size_t i = 0; i < ids.size(); ++i) {
    const Track* t = findTrackPtr(*ctx.doc(), ids[i]);
    if (t->name == "Lead") heard = levels[i].peakDb[0] > -40.0f;
    if (t->name == "Muted") CHECK(levels[i].peakDb[0] < -150.0f);
  }
  CHECK(heard);
  // Stop: the engine falls asleep after its idle time and costs nothing per block while asleep.
  ctx.actions().invoke(ctx.actions().find("transport.playStop"));
  for (uint32_t i = 0; i < 200 && !link->engine().asleep(); ++i) dev.processBlock();
  CHECK(link->engine().asleep());
  const uint64_t asleep0 = link->engine().stats().asleepBlocks;
  for (int i = 0; i < 20; ++i) dev.processBlock();
  CHECK(link->engine().stats().asleepBlocks == asleep0 + 20);
  // Audition (a piano-roll click) wakes it and sounds while stopped.
  link->auditionHz(440.0f, 100.0f, 100.0f);
  float pk = 0.0f;
  for (int i = 0; i < 10; ++i) {
    dev.processBlock();
    for (uint32_t s = 0; s < 256; ++s) pk = std::max(pk, std::fabs(dev.output(0)[s]));
  }
  CHECK(pk > 0.001f);
  CHECK_FALSE(link->engine().asleep());
  dev.close();
  link->detach();
}

TEST_CASE("a structural edit on the compile thread is debounced into one compile") {
  playback_fixture::Fixture f;
  LinkConfig c = syncConfig();
  c.compileThread = true;
  auto link = makeLink(f.snap(), c);
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  engine::NullDevice dev;
  engine::StreamConfig sc;
  sc.sampleRate = 48000;
  sc.bufferFrames = 256;
  REQUIRE(dev.open(sc, link->callback()).ok());
  REQUIRE(dev.start().ok());  // a real-time audio thread takes the new program
  const app::LinkStats s0 = link->stats();
  // Three structural edits in quick succession (well inside the debounce): one compile.
  for (int i = 0; i < 3; ++i)
    REQUIRE(f.store.dispatch(AddTrack{playback_fixture::track(f.store.ids().track(), TrackKind::Midi, "T", f.store.ids().device()), {}, kAppend}).ok());
  link->flush();
  CHECK(link->stats().compiles == s0.compiles + 1);
  CHECK(link->stats().compileErrors == 0);
  REQUIRE(dev.stop().ok());
  dev.close();
}
