// FZ.4 (docs/plans/RB.md D5, "### FZ.4"; pq/app/render_edits.h): the edits that render first, through the app's
// service on the playback link with a ViewContext (a titled project in a temp folder), on the playback fixture
// (120 BPM: a bar is 2 s, 96 000 frames):
//   flatten      frozen Lead (MIDI, the default instrument) becomes an audio track playing its render: the master and
//                Lead's out bus null against the frozen playback (<= -100 dB); one undo step restores the frozen MIDI
//                track; save / load keeps the new sample (samples/frozen/Lead.wav)
//   bounce       the whole of Lead onto a new audio track below it (same mixer): its out bus nulls against Lead's own
//                out bus; Lead is deactivated in the same step; a time selection bounces exactly that range
//   consolidate  an audio track's clips (gain, a crossfade) over a selection become one clip of its clip player's
//                render: the track's out bus nulls against the original clips'
//   stale        an edit of the track while it renders drops the result (no edit, no file)
// FZ.x-fix: render clips have no edge declick (flatten / bounce null from frame 0, a selection's hard edges included);
// Flatten keeps the blocks that feed another track (Lead's LFO into Mos: Mos's out bus unchanged).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/export_render.h"
#include "pq/document/lower.h"
#include "pq/app/project_link.h"
#include "pq/app/render_edits.h"
#include "pq/document/samples.h"
#include "pq/media/probe.h"
#include "pq/media/wav_file.h"
#include "pq/views/render_edit_port.h"
#include "pq/views/view_context.h"

using namespace pq;
using namespace pq::doc;
namespace fs = std::filesystem;

namespace {

using Channels = std::vector<std::vector<float>>;

fs::path freshDir(const std::string& name) {
  const fs::path d = fs::temp_directory_path() / ("pq_test_render_edits_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

std::string utf8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

struct Collect final : engine::AudioSink {
  Channels ch;
  Status begin(uint32_t n, uint32_t, uint64_t) override {
    ch.assign(n, {});
    return Ok{};
  }
  Status write(const float* const* x, uint32_t frames) override {
    for (size_t c = 0; c < ch.size(); ++c) ch[c].insert(ch[c].end(), x[c], x[c] + frames);
    return Ok{};
  }
  Status end() override { return Ok{}; }
};

// A bus (kNoBus: the master) over [from, to), PDC-trimmed.
Channels pass(app::ProjectLink& link, const Project& p, Ticks from, Ticks to, uint32_t bus = engine::kNoBus) {
  Collect out;
  app::PassSpec spec;
  spec.from = from;
  spec.to = to;
  spec.targets.push_back(app::PassTarget{bus, &out});
  auto r = app::renderPass(link, p, spec);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return out.ch;
}

// 20·log10(max |a − b| / max |a|) over frames [skip, size − skipEnd).
double nullDb(const Channels& a, const Channels& b, size_t skip = 0, size_t skipEnd = 0) {
  REQUIRE(a.size() == b.size());
  double peak = 0.0, diff = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = skip; i + skipEnd < a[c].size(); ++i) {
      peak = std::max(peak, std::fabs(static_cast<double>(a[c][i])));
      diff = std::max(diff, std::fabs(static_cast<double>(a[c][i]) - static_cast<double>(b[c][i])));
    }
  }
  REQUIRE(peak > 0.01);
  return 20.0 * std::log10(std::max(diff, 1e-30) / peak);
}

// The link, a ViewContext on it (the app's wiring), the project saved into `dir` (titled).
struct App {
  playback_fixture::Fixture f;
  fs::path dir;
  std::unique_ptr<app::ProjectLink> link;
  std::unique_ptr<views::ViewContext> ctx;
  std::unique_ptr<app::RenderEditService> svc;
  explicit App(const std::string& name) : dir(freshDir(name)) {
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = false;
    c.freeze = true;
    c.projectDir = utf8(dir);
    auto l = app::ProjectLink::create(c, f.snap());
    REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
    link = std::move(*l);
    ctx = std::make_unique<views::ViewContext>(*link);
    ctx->store().reset(f.snap());
    link->attach(*ctx);
    REQUIRE(ctx->saveProjectAs(utf8(dir)).ok());
    svc = std::make_unique<app::RenderEditService>(*link, *ctx, false);
  }
  ~App() {
    svc.reset();
    link->detach();
    ctx.reset();
    link.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  const Project& doc() const { return *ctx->doc(); }
  const Track& track(TrackId id) const {
    const Track* t = findTrackPtr(doc(), id);
    REQUIRE(t != nullptr);
    return *t;
  }
  void dispatch(const Command& c) {
    const Status s = ctx->store().dispatch(c);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
    link->flush();
  }
  size_t history() const { return ctx->store().history().cursor(); }
  // Runs `job` to its edit (or its refusal), then lets the link take the new samples and compile.
  void run(const views::RenderEditJob& job) {
    REQUIRE(svc->start(job));
    CHECK(svc->busy());
    REQUIRE(svc->waitIdle());
    REQUIRE(svc->poll());
    CHECK_FALSE(svc->busy());
    settle();
  }
  void settle() {
    link->flush();
    (void)link->waitForSamples();
    link->flush();
  }
  // One pass (EX.x-fix-settle: every pass starts settled, the first like any other).
  Channels bus(Ticks from, Ticks to, uint32_t b = engine::kNoBus) { return pass(*link, doc(), from, to, b); }
};

constexpr Ticks kBar = kTicksPerWhole;  // 4/4

Command must(const ops::EditResult& e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  return e->command;
}

}  // namespace

TEST_CASE("FZ.4: Flatten — frozen Lead becomes audio clips of its render, plays the same (<= -100 dB), one undo step, save / load") {
  App a("flatten");
  const Ticks to = 3 * kBar;
  a.dispatch(SetTrackFrozen{a.f.lead, true});
  REQUIRE(a.link->waitForFreeze());
  REQUIRE(a.link->freezeStatus(a.f.lead).state == app::FreezeState::Ready);
  const Snapshot frozenDoc = a.ctx->doc();
  const Channels master = a.bus(0, to);
  const Channels lead = a.bus(0, to, a.link->trackOutBus(a.f.lead));
  const size_t h = a.history();

  views::RenderEditJob job;
  job.kind = views::RenderEditJob::Kind::Flatten;
  job.tracks = {a.f.lead};
  a.run(job);
  INFO(a.ctx->status());
  REQUIRE(a.history() == h + 1);
  CHECK(a.ctx->store().history().entries()[h].label == "Flatten");
  const Track& t = a.track(a.f.lead);
  CHECK(t.kind == TrackKind::Audio);
  CHECK_FALSE(t.frozen);
  CHECK(t.devices.empty());
  REQUIRE_FALSE(t.clips.empty());
  const fs::path file = a.dir / "samples" / "frozen" / "Lead.wav";
  CHECK(fs::exists(file));
  const SampleRef* s = findSample(a.doc(), t.clips[0].audio->sample);
  REQUIRE(s != nullptr);
  CHECK(s->path == "samples/frozen/Lead.wav");
  CHECK(a.link->freezeStatus(a.f.lead).state == app::FreezeState::None);

  // Lead sounds from song sample 0: the clip at tick 0 has no edge declick (FZ.x-fix: AudioClip::noEdgeFade), as the
  // frozen render's clip: the two are the same from frame 0.
  CHECK(t.clips[0].audio->noEdgeFade);
  const Channels flatMaster = a.bus(0, to);
  const Channels flatLead = a.bus(0, to, a.link->trackOutBus(a.f.lead));
  const double dbMaster = nullDb(master, flatMaster);
  const double dbLead = nullDb(lead, flatLead);
  MESSAGE("flattened vs frozen from frame 0: the master " << dbMaster << " dB, Lead's out " << dbLead << " dB; "
          << t.clips.size() << " clip(s)");
  CHECK(dbMaster <= -100.0);
  CHECK(dbLead <= -100.0);
  double first = 0.0;  // the first 2 ms on their own (what the declick ramped)
  for (size_t c = 0; c < lead.size(); ++c)
    for (size_t k = 0; k < 96; ++k) first = std::max(first, static_cast<double>(std::fabs(flatLead[c][k] - lead[c][k])));
  CHECK(first <= 1e-5 * std::max(1e-3, static_cast<double>(std::fabs(lead[0][95]))));

  // Undo: the frozen MIDI track, exactly; it plays its render again (the cache file stayed).
  REQUIRE(a.ctx->store().undo());
  CHECK(a.ctx->doc() == frozenDoc);
  a.settle();
  REQUIRE(a.link->waitForFreeze());
  CHECK(a.link->freezeStatus(a.f.lead).state == app::FreezeState::Ready);
  CHECK(nullDb(master, a.bus(0, to)) <= -100.0);
  // Redo, save, load: the sample is in the project and resolves.
  REQUIRE(a.ctx->store().redo());
  const SampleId sid = a.track(a.f.lead).clips[0].audio->sample;
  REQUIRE(a.ctx->saveProject().ok());
  REQUIRE(a.ctx->openProject(utf8(a.dir)).ok());
  CHECK(a.track(a.f.lead).kind == TrackKind::Audio);
  CHECK(findSample(a.doc(), sid) != nullptr);
  CHECK_FALSE(a.ctx->samplePath(sid).empty());
}

TEST_CASE("FZ.4: Bounce to New Track — Lead's own render on a new audio track below; nulls against Lead's out bus; Lead deactivated") {
  App a("bounce");
  const Ticks to = 3 * kBar;
  const Channels lead = a.bus(0, to, a.link->trackOutBus(a.f.lead));
  const Snapshot before = a.ctx->doc();
  const size_t h = a.history();
  views::RenderEditJob job;
  job.kind = views::RenderEditJob::Kind::Bounce;
  job.tracks = {a.f.lead};
  a.run(job);
  INFO(a.ctx->status());
  REQUIRE(a.history() == h + 1);
  CHECK(a.ctx->store().history().entries()[h].label == "Bounce to New Track");
  CHECK_FALSE(a.track(a.f.lead).active);
  // The new track: right after Lead, audio, Lead's colour and mixer.
  const std::vector<TrackPtr>& top = a.doc().tracks;
  size_t at = 0;
  while (at < top.size() && top[at]->id != a.f.lead) ++at;
  REQUIRE(at + 1 < top.size());
  const Track& n = *top[at + 1];
  CHECK(n.kind == TrackKind::Audio);
  CHECK(n.name == "Lead Bounce");
  CHECK(n.colour == a.track(a.f.lead).colour);
  CHECK(n.mixer.volumeDb == a.track(a.f.lead).mixer.volumeDb);
  CHECK(n.mixer.sends == a.track(a.f.lead).mixer.sends);
  REQUIRE_FALSE(n.clips.empty());
  CHECK(fs::exists(a.dir / "samples" / "bounced" / "Lead Bounce.wav"));
  const double db = nullDb(lead, a.bus(0, to, a.link->trackOutBus(n.id)));  // from frame 0: no edge declick
  MESSAGE("bounced vs Lead's out (from frame 0): " << db << " dB, " << n.clips.size() << " clip(s)");
  CHECK(db <= -100.0);
  REQUIRE(a.ctx->store().undo());
  CHECK(a.ctx->doc() == before);

  // A time selection: exactly that range, one clip over it, cut into sound at both edges (no edge declick).
  a.settle();
  const Ticks s = kBar / 2 + kPpq / 3, e = 3 * kBar / 2;
  const Channels part = a.bus(s, e, a.link->trackOutBus(a.f.lead));
  job.range = true;
  job.from = s;
  job.to = e;
  a.run(job);
  INFO(a.ctx->status());
  REQUIRE(a.history() == h + 1);
  const std::vector<TrackPtr>& top2 = a.doc().tracks;
  const Track& m = *top2[at + 1];
  REQUIRE(m.clips.size() == 1);
  CHECK(m.clips[0].start() == s);
  CHECK(m.clips[0].end() == e);
  CHECK(m.clips[0].audio->noEdgeFade);
  const double dbPart = nullDb(part, a.bus(s, e, a.link->trackOutBus(m.id)));
  MESSAGE("bounced selection vs Lead's out over it: " << dbPart << " dB (edges included)");
  CHECK(dbPart <= -100.0);
}

TEST_CASE("FZ.4: Consolidate on audio — the clips over the selection become one render clip; nulls against them") {
  App a("consolidate");
  // A stereo test file (two sines), an audio track with two clips: A at −6 dB, B adjacent with a crossfade.
  const fs::path src = a.dir / "samples" / "src.wav";
  fs::create_directories(src.parent_path());
  {
    Channels w(2, std::vector<float>(4 * 48000));
    for (size_t i = 0; i < w[0].size(); ++i) {
      w[0][i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 220.0 * static_cast<double>(i) / 48000.0));
      w[1][i] = 0.4f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 331.0 * static_cast<double>(i) / 48000.0));
    }
    REQUIRE(media::writeWav(src, w, 48000).ok());
  }
  IdSource& ids = a.ctx->store().ids();
  const SampleProbeFn probe = [](std::string_view) -> Result<SampleProbe> { return SampleProbe{4 * 48000, 48000.0, 2}; };
  auto ref = makeSampleRef(ids, utf8(src), utf8(a.dir), probe);
  REQUIRE(ref.ok());
  auto tr = ops::createTrack(a.doc(), ids, TrackKind::Audio, a.f.lead, "Loop");
  REQUIRE(tr.ok());
  a.dispatch(tr->command);
  const TrackId loop = tr->tracks.at(0);
  const ops::NewAudioClip ncs[] = {{loop, kBar / 4, ref->id, {}}, {loop, kBar + kBar / 4, ref->id, {}}};
  auto add = ops::addAudioClips(a.doc(), ids, std::span<const SampleRef>(&*ref, 1), ncs);
  REQUIRE(add.ok());
  a.dispatch(add->command);
  REQUIRE(a.track(loop).clips.size() == 2);  // the first clip (2 s) is trimmed by the second at bar 1.25
  const ClipRef ca{loop, a.track(loop).clips[0].id}, cb{loop, a.track(loop).clips[1].id};
  a.dispatch(must(ops::setAudioClipGain(a.doc(), std::span<const ClipRef>(&ca, 1), -6.0f)));
  a.dispatch(must(ops::setAudioClipCrossfade(a.doc(), cb, kPpq / 2)));
  a.settle();
  const Ticks s = 0, e = 3 * kBar + kBar / 2;  // B ends at bar 3.25: every edge of the selection is quiet
  const Channels before = a.bus(0, e + kBar, a.link->trackOutBus(loop));
  const size_t h = a.history();
  views::RenderEditJob job;
  job.kind = views::RenderEditJob::Kind::Consolidate;
  job.tracks = {loop};
  job.range = true;
  job.from = s;
  job.to = e;
  a.run(job);
  INFO(a.ctx->status());
  REQUIRE(a.history() == h + 1);
  const Track& t = a.track(loop);
  REQUIRE(t.clips.size() == 1);
  CHECK(t.clips[0].start() == s);
  CHECK(t.clips[0].end() == e);
  CHECK(fs::exists(a.dir / "samples" / "consolidated" / "Loop.wav"));
  const double db = nullDb(before, a.bus(0, e + kBar, a.link->trackOutBus(loop)));
  MESSAGE("consolidated vs the clips: " << db << " dB");
  CHECK(db <= -100.0);
  REQUIRE(a.ctx->store().undo());
  CHECK(a.track(loop).clips.size() == 2);
}

TEST_CASE("FZ.4: Flatten a frozen audio track with a long silence: one clip per region, the gap left empty; nulls") {
  App a("flatten_regions");
  const fs::path src = a.dir / "samples" / "tone.wav";
  fs::create_directories(src.parent_path());
  {
    Channels w(1, std::vector<float>(48000));  // 1 s of a decaying tone
    for (size_t i = 0; i < w[0].size(); ++i)
      w[0][i] = 0.6f * static_cast<float>(std::exp(-3.0 * static_cast<double>(i) / 48000.0) *
                                          std::sin(2.0 * 3.141592653589793 * 440.0 * static_cast<double>(i) / 48000.0));
    REQUIRE(media::writeWav(src, w, 48000).ok());
  }
  IdSource& ids = a.ctx->store().ids();
  const SampleProbeFn probe = [](std::string_view) -> Result<SampleProbe> { return SampleProbe{48000, 48000.0, 1}; };
  auto ref = makeSampleRef(ids, utf8(src), utf8(a.dir), probe);
  REQUIRE(ref.ok());
  auto tr = ops::createTrack(a.doc(), ids, TrackKind::Audio, a.f.lead, "Tone");
  REQUIRE(tr.ok());
  a.dispatch(tr->command);
  const TrackId tone = tr->tracks.at(0);
  // At bar 0.5 and bar 3 (1.5 s of silence between them, more than kRegionGapSeconds).
  const ops::NewAudioClip ncs[] = {{tone, kBar / 2, ref->id, {}}, {tone, 3 * kBar, ref->id, {}}};
  a.dispatch(must(ops::addAudioClips(a.doc(), ids, std::span<const SampleRef>(&*ref, 1), ncs)));
  a.dispatch(SetTrackFrozen{tone, true});
  REQUIRE(a.link->waitForFreeze());
  REQUIRE(a.link->freezeStatus(tone).state == app::FreezeState::Ready);
  const Ticks to = 4 * kBar;
  const Channels frozen = a.bus(0, to, a.link->trackOutBus(tone));
  views::RenderEditJob job;
  job.kind = views::RenderEditJob::Kind::Flatten;
  job.tracks = {tone};
  a.run(job);
  INFO(a.ctx->status());
  const Track& t = a.track(tone);
  CHECK_FALSE(t.frozen);
  REQUIRE(t.clips.size() == 2);
  // Each region: its clip's sound widened by kRegionPadSeconds (and the tone's own tail above −120 dBFS).
  const TempoMap& tm = a.doc().timeline->tempoMap;
  CHECK(t.clips[0].start() < kBar / 2);
  CHECK(t.clips[0].start() > kBar / 2 - kPpq / 8);
  CHECK(t.clips[0].end() < 3 * kBar);
  CHECK(t.clips[1].start() < 3 * kBar);
  CHECK(t.clips[1].audio->contentStartFrame == static_cast<double>(tm.ticksToSamples(t.clips[1].start(), 48000.0)));
  const double db = nullDb(frozen, a.bus(0, to, a.link->trackOutBus(tone)));
  MESSAGE("flattened (2 regions) vs frozen: " << db << " dB");
  CHECK(db <= -100.0);
}

TEST_CASE("FZ.x-fix: Flatten keeps Lead's LFO that modulates Mos: Mos sounds the same, the LFO and its cable stay") {
  App a("flatten_keep");
  DocumentStore& st = a.ctx->store();
  auto setValue = [&](DeviceId group, std::string_view member, double units) {
    auto e = ops::setDeviceParamValue(a.doc(), playback_fixture::unitsLibrary(), test_blocks::member(a.doc(), group, member),
                                      std::string(kValueParam), units);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    a.dispatch(e->command);
  };
  // An LFO on Lead (off its path; synced to the song, so its phase is the song's), into the Gain of a Utility on Mos.
  const DeviceId lfo = test_blocks::addModule(st, a.f.lead, "lfo", ops::kOffPath);
  setValue(lfo, "Sync", 1.0);
  setValue(lfo, "Depth", 0.25);
  const DeviceId util = test_blocks::addModule(st, a.f.mos, "utility");
  Wire mod;
  mod.from = lfo;
  mod.to = test_blocks::member(a.doc(), util, "Gain");
  mod.param = std::string(kValueParam);
  REQUIRE(mod.to.valid());
  auto connected = ops::connect(a.doc(), st.ids(), mod);
  REQUIRE_MESSAGE(connected.ok(), (connected.ok() ? std::string() : connected.error().message));
  a.dispatch(connected->command);
  const WireId cable = connected->wires.at(0);
  a.dispatch(SetTrackFrozen{a.f.lead, true});
  REQUIRE(a.link->waitForFreeze());
  REQUIRE(a.link->freezeStatus(a.f.lead).state == app::FreezeState::Ready);
  const FrozenKeep keep = frozenKeep(a.doc(), a.track(a.f.lead));
  REQUIRE_FALSE(keep.blocks.empty());
  const Ticks to = 2 * kBar;
  const Channels mos = a.bus(0, to, a.link->trackOutBus(a.f.mos));
  const Channels master = a.bus(0, to);

  views::RenderEditJob job;
  job.kind = views::RenderEditJob::Kind::Flatten;
  job.tracks = {a.f.lead};
  a.run(job);
  INFO(a.ctx->status());
  const Track& t = a.track(a.f.lead);
  REQUIRE(t.kind == TrackKind::Audio);
  std::vector<DeviceId> left;
  for (const DevicePtr& d : t.devices) left.push_back(d->id);
  CHECK(left == keep.blocks);
  CHECK(findWire(a.doc(), cable) != nullptr);
  const double dbMos = nullDb(mos, a.bus(0, to, a.link->trackOutBus(a.f.mos)));
  const double dbMaster = nullDb(master, a.bus(0, to));
  MESSAGE("flattened vs frozen: Mos's out " << dbMos << " dB, the master " << dbMaster << " dB");
  CHECK(dbMos <= -120.0);
  CHECK(dbMaster <= -100.0);
  // Without the cable the LFO would not run, and Mos would differ: the null above is the LFO kept, not a no-op.
  const Channels mosLive = a.bus(0, to, a.link->trackOutBus(a.f.mos));
  a.dispatch(RemoveWire{cable});
  CHECK(nullDb(mosLive, a.bus(0, to, a.link->trackOutBus(a.f.mos))) > -60.0);
}

TEST_CASE("FZ.4: an edit of the track while it renders drops the result: no edit, no file; cancel likewise") {
  App a("stale");
  views::RenderEditJob job;
  job.kind = views::RenderEditJob::Kind::Bounce;
  job.tracks = {a.f.lead};
  const size_t h = a.history();
  REQUIRE(a.svc->start(job));
  CHECK_FALSE(a.svc->start(job));  // one at a time
  // An edit of what Lead's own contribution depends on (its clip): the render is stale.
  a.dispatch(RemoveClip{{a.f.lead, a.f.leadClip}});
  REQUIRE(a.svc->waitIdle());
  REQUIRE(a.svc->poll());
  CHECK(a.history() == h + 1);  // the RemoveClip only
  CHECK(a.ctx->status().find("changed") != std::string::npos);
  CHECK_FALSE(fs::exists(a.dir / "samples" / "bounced" / "Lead Bounce.wav"));
  // A cancel drops it.
  REQUIRE(a.ctx->store().undo());
  REQUIRE(a.svc->start(job));
  a.svc->cancel();
  REQUIRE(a.svc->waitIdle());
  REQUIRE(a.svc->poll());
  CHECK(a.ctx->status().find("cancelled") != std::string::npos);
  CHECK_FALSE(fs::exists(a.dir / "samples" / "bounced" / "Lead Bounce.wav"));
}

// AU.app L6 (docs/reviews/RB-DY.md): the project is saved elsewhere while a Bounce renders: the result lands in the
// folder the document belongs to when it is applied (a relative ref), not in the one it started in.
TEST_CASE("AU.app L6: a Bounce whose project is saved elsewhere while it renders lands in the new project's folder") {
  App a("l6");
  const fs::path moved = freshDir("l6_moved");
  views::RenderEditJob job;
  job.kind = views::RenderEditJob::Kind::Bounce;
  job.tracks = {a.f.lead};
  REQUIRE(a.svc->start(job));
  REQUIRE(a.svc->waitIdle());
  REQUIRE(a.ctx->saveProjectAs(utf8(moved)).ok());  // before the result is taken
  REQUIRE(a.svc->poll());
  INFO(a.ctx->status());
  const Track* bounced = nullptr;
  for (const TrackPtr& t : a.doc().tracks)
    if (t->name == "Lead Bounce") bounced = t.get();
  REQUIRE(bounced != nullptr);
  REQUIRE_FALSE(bounced->clips.empty());
  REQUIRE(bounced->clips[0].audio != nullptr);
  const SampleRef* ref = findSample(a.doc(), bounced->clips[0].audio->sample);
  REQUIRE(ref != nullptr);
  CHECK(ref->path == "samples/bounced/Lead Bounce.wav");  // relative to the project it belongs to now
  CHECK(fs::exists(moved / "samples" / "bounced" / "Lead Bounce.wav"));
  CHECK_FALSE(fs::exists(a.dir / "samples" / "bounced" / "Lead Bounce.wav"));
  a.ctx->store().reset(a.f.snap());
  std::error_code ec;
  fs::remove_all(moved, ec);
}
