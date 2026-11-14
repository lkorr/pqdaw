// FZ.2: the freeze service through the playback link (pq/app/freeze_service.h; docs/plans/RB.md D2, D3), on the playback
// fixture (120 BPM: a bar is 2 s, 96 000 frames):
//   null      freezing Lead (a MIDI track through the default instrument, sending to Echo, panned, −3 dB) plays its render:
//             the PDC-trimmed master equals the live master within −100 dB; Lead's patch is one clip player and its
//             mixer (no device calls), its own cables Inactive; unfreezing returns to the very same IR
//   stale     a tempo edit makes the render stale: Lead plays live at once (no slot), re-renders in the background and
//             nulls against the live render at the new tempo; a new link (a load) reuses the cached file (no render);
//             deleting cache/ renders again
//   key       what moves the key: tempo, tunings, a cable from another track into Lead's blocks (Q252: that track joins
//             the render document); what does not: Lead's mixer, name, colour, another track, a cable into Lead's mixer
//   AU.app    H2 a rate change renders to its own path; H3 a cable from a group's sum keys on its contributors (an
//             unrelated edit costs one compare, a feeder burst one plan off the UI thread); M6 Save As copies the
//             playing render on the service thread; M5 the sweep; L7 the executable and the sample files in the key;
//             M2 RF64 past the RIFF limit (a lowered limit)
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/export_render.h"
#include "pq/app/freeze_service.h"
#include "pq/app/project_link.h"
#include "pq/jit/compiler.h"
#include "pq/media/probe.h"
#include "pq/media/wav_file.h"

using namespace pq;
using namespace pq::doc;
namespace fs = std::filesystem;

namespace {

using Channels = std::vector<std::vector<float>>;

fs::path freshDir(const std::string& name) {
  const fs::path d = fs::temp_directory_path() / ("pq_test_freeze_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

std::unique_ptr<app::ProjectLink> makeLink(const Snapshot& s, const fs::path& dir, bool freeze = true) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;  // interpreter images (bit-identical to the JIT's): the JIT's compile time would dominate
  c.freeze = freeze;
  c.projectDir = dir.string();
  auto l = app::ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
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

// A few blocks, so the engine takes the images the last compile published: these tests drive no audio thread, and a
// unit's next publish waits (up to 5 s) for room while the engine has not run since the last one.
void pump(app::ProjectLink& link) {
  Collect out;
  REQUIRE(link.renderOffline(0, 256, out).ok());
}

// The master over [0, to), PDC-trimmed (renderPass).
Channels master(app::ProjectLink& link, const Project& p, Ticks to) {
  Collect out;
  app::PassSpec spec;
  spec.from = 0;
  spec.to = to;
  spec.targets.push_back(app::PassTarget{engine::kNoBus, &out});
  auto r = app::renderPass(link, p, spec);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return out.ch;
}

Channels busOf(app::ProjectLink& link, const Project& p, Ticks to, uint32_t bus) {
  Collect out;
  app::PassSpec spec;
  spec.from = 0;
  spec.to = to;
  spec.targets.push_back(app::PassTarget{bus, &out});
  auto r = app::renderPass(link, p, spec);
  REQUIRE(r.ok());
  return out.ch;
}

// 20·log10(max |a − b| / max |a|).
double nullDb(const Channels& a, const Channels& b) {
  REQUIRE(a.size() == b.size());
  double peak = 0.0, diff = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = 0; i < a[c].size(); ++i) {
      peak = std::max(peak, std::fabs(static_cast<double>(a[c][i])));
      diff = std::max(diff, std::fabs(static_cast<double>(a[c][i]) - static_cast<double>(b[c][i])));
    }
  }
  REQUIRE(peak > 0.01);
  return 20.0 * std::log10(std::max(diff, 1e-30) / peak);
}

const LoweredPatch& patchOf(const LoweredProject& lp, TrackId t) {
  for (size_t i = 0; i < lp.trackIds.size(); ++i)
    if (lp.trackIds[i] == t) return *lp.patches[i];
  FAIL("track " << t.value << " not lowered");
  return *lp.patches.front();
}

}  // namespace

TEST_CASE("FZ.2: a frozen track plays its render (master null <= -100 dB, one clip player); unfreeze = the same IR") {
  playback_fixture::Fixture f;
  const fs::path dir = freshDir("null");
  auto link = makeLink(f.snap(), dir);
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  const Ticks to = 2 * kTicksPerWhole;  // Lead's clip
  const std::shared_ptr<const LoweredProject> live = link->lastLowering();
  // (EX.x-fix-settle: a link's first pass equals its later ones — Grp's automated volume settles at its start value —
  // so the references are first passes.)
  const Channels reference = master(*link, *f.snap(), to);
  const Channels leadLive = busOf(*link, *f.snap(), to, link->trackOutBus(f.lead));

  REQUIRE(f.store.dispatch(SetTrackFrozen{f.lead, true}).ok());
  link->flush();
  CHECK(link->freezeStatus(f.lead).state == app::FreezeState::Rendering);
  CHECK(link->frozenSlot(f.lead) == app::ProjectLink::kNoSlot);  // live until the render is in
  REQUIRE(link->waitForFreeze());
  REQUIRE_MESSAGE(link->freezeStatus(f.lead).state == app::FreezeState::Ready, link->freezeStatus(f.lead).error);
  REQUIRE(link->frozenSlot(f.lead) != app::ProjectLink::kNoSlot);
  REQUIRE(link->freezeService()->stats().finished == 1);
  const auto render = link->frozenRender(f.lead);
  REQUIRE(render.has_value());
  CHECK(fs::exists(dir / "cache" / "freeze" / (std::to_string(f.lead.value) + ".json")));
  CHECK(render->channels == 1);  // the own sum's width: the default instrument is mono (the mixer pans it as live)
  CHECK(render->frames >= 2 * 96000u);  // the range, then its tail

  // Lead is one clip player and its mixer: no block is lowered.
  const std::shared_ptr<const LoweredProject> frozen = link->lastLowering();
  const std::string text = patchText(patchOf(*frozen, f.lead));
  CHECK(text.find("clipPlayer") != std::string::npos);
  CHECK(text.find("\n  d") == std::string::npos);  // a block's node is named d<id>
  CHECK(frozen->frozenKeep.empty());
  MESSAGE("Lead's patch: " << patchOf(*live, f.lead).patch.nodes().size() << " nodes live, "
                           << patchOf(*frozen, f.lead).patch.nodes().size() << " frozen");
  // Its own cables are Inactive in the wire states the link reports.
  const Track* lead = findTrackPtr(*f.snap(), f.lead);
  uint32_t own = 0;
  for (const WireState& w : link->wireStates()) {
    const Wire* wire = findWire(*f.snap(), w.id);
    if (wire == nullptr || !ownedByFrozen(*lead, *wire)) continue;
    ++own;
    CHECK(w.status == WireStatus::Inactive);
  }
  CHECK(own > 0);

  const double leadDb = nullDb(leadLive, busOf(*link, *f.snap(), to, link->trackOutBus(f.lead)));
  const double db = nullDb(reference, master(*link, *f.snap(), to));
  MESSAGE("frozen vs live: Lead's out " << leadDb << " dB, the master " << db << " dB");
  CHECK(leadDb <= -100.0);
  CHECK(db <= -100.0);

  // Unfreeze: live again, the IR of before (its blocks kept their meta / curve slot ids), the sound of before.
  REQUIRE(f.store.dispatch(SetTrackFrozen{f.lead, false}).ok());
  link->flush();
  CHECK(link->freezeStatus(f.lead).state == app::FreezeState::None);
  CHECK(link->frozenSlot(f.lead) == app::ProjectLink::kNoSlot);
  CHECK(patchOf(*link->lastLowering(), f.lead).hash == patchOf(*live, f.lead).hash);
  CHECK(patchOf(*link->lastLowering(), f.lead).patch == patchOf(*live, f.lead).patch);
  CHECK(nullDb(reference, master(*link, *f.snap(), to)) <= -120.0);
  CHECK(fs::exists(dir / "cache" / "freeze" / (std::to_string(f.lead.value) + ".json")));  // the file stays
  link.reset();
  fs::remove_all(dir);
}

TEST_CASE("FZ.2: a tempo edit re-renders while the track plays live; a load reuses the cache; deleting cache/ re-renders") {
  playback_fixture::Fixture f;
  const fs::path dir = freshDir("stale");
  REQUIRE(f.store.dispatch(SetTrackFrozen{f.lead, true}).ok());
  auto link = makeLink(f.snap(), dir);
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  REQUIRE(link->waitForFreeze());
  REQUIRE(link->frozenSlot(f.lead) != app::ProjectLink::kNoSlot);
  REQUIRE(link->freezeService()->stats().started == 1);
  pump(*link);

  // 100 BPM: stale at once (live), then rendered again.
  REQUIRE(f.store.dispatch(SetTempoMap{{TempoPoint{0, 100.0}}}).ok());
  link->flush();
  CHECK(link->frozenSlot(f.lead) == app::ProjectLink::kNoSlot);
  CHECK(link->freezeStatus(f.lead).state == app::FreezeState::Rendering);
  pump(*link);
  REQUIRE(link->waitForFreeze());
  REQUIRE(link->frozenSlot(f.lead) != app::ProjectLink::kNoSlot);
  CHECK(link->freezeService()->stats().started == 2);
  const Ticks to = 2 * kTicksPerWhole;
  const Channels played = master(*link, *f.snap(), to);
  {
    auto plain = makeLink(f.snap(), dir, false);  // the same document, every track live (its first pass)
    const double db = nullDb(master(*plain, *f.snap(), to), played);
    MESSAGE("re-rendered at 100 BPM vs live: " << db << " dB");
    CHECK(db <= -100.0);
  }
  link.reset();

  // A load: the cached render plays without rendering.
  {
    auto loaded = makeLink(f.snap(), dir);
    REQUIRE(loaded->waitForFreeze());
    CHECK(loaded->freezeService()->stats().started == 0);
    CHECK(loaded->freezeService()->stats().cacheHits == 1);
    CHECK(loaded->frozenSlot(f.lead) != app::ProjectLink::kNoSlot);
  }
  // cache/ deleted: rendered again on load.
  fs::remove_all(dir / "cache");
  {
    auto loaded = makeLink(f.snap(), dir);
    REQUIRE(loaded->waitForFreeze());
    CHECK(loaded->freezeService()->stats().started == 1);
    CHECK(loaded->frozenSlot(f.lead) != app::ProjectLink::kNoSlot);
  }
  fs::remove_all(dir);
}

TEST_CASE("FZ.2: the render key follows tempo, tunings and cables into the frozen blocks; not the mixer or other tracks") {
  playback_fixture::Fixture f;
  REQUIRE(f.store.dispatch(SetTrackFrozen{f.lead, true}).ok());
  auto keyNow = [&] {
    auto p = app::planFreeze(*f.snap(), f.lead, 48000.0);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().message));
    return p->key;
  };
  const app::FreezeKey k0 = keyNow();
  // The render document: Lead (and the master as an empty container), nothing else.
  {
    const auto d = app::freezeDocument(*f.snap(), f.lead);
    REQUIRE(d.has_value());
    CHECK(d->tracks.size() == 1);
    CHECK(d->returns.empty());
    CHECK(d->master->devices.empty());
  }
  // Not part of Lead's own contribution: its mixer, name, colour, other tracks.
  REQUIRE(f.store.dispatch(SetTrackVolume{f.lead, -12.0f}).ok());
  REQUIRE(f.store.dispatch(SetTrackPan{f.lead, 0.3f}).ok());
  REQUIRE(f.store.dispatch(SetTrackMute{f.lead, true}).ok());
  REQUIRE(f.store.dispatch(RenameTrack{f.lead, "Frozen lead"}).ok());
  REQUIRE(f.store.dispatch(SetTrackColour{f.lead, Colour{0x123456}}).ok());
  REQUIRE(f.store.dispatch(SetTrackVolume{f.mos, -20.0f}).ok());
  REQUIRE(f.store.dispatch(AddTrack{playback_fixture::track(f.store.ids().track(), TrackKind::Midi, "New"), {}, kAppend}).ok());
  CHECK(keyNow() == k0);
  // A cable from Mos's instrument into Lead's mixer stays live (not Lead's own): the same key.
  const Track* mos = findTrackPtr(*f.snap(), f.mos);
  const Track* lead = findTrackPtr(*f.snap(), f.lead);
  REQUIRE(!mos->devices.empty());
  REQUIRE(!lead->devices.empty());
  Wire toMixer;
  toMixer.id = f.store.ids().wire();
  toMixer.from = mos->devices.front()->id;
  toMixer.mixer = f.lead;
  REQUIRE(f.store.dispatch(AddWire{toMixer}).ok());
  CHECK(keyNow() == k0);
  // A cable from Mos into one of Lead's blocks: Mos joins the render document; the key moves (Q252).
  Wire toBlock;
  toBlock.id = f.store.ids().wire();
  toBlock.from = mos->devices.front()->id;
  toBlock.to = lead->devices.back()->id;
  const auto added = f.store.dispatch(AddWire{toBlock});
  REQUIRE_MESSAGE(added.ok(), (added.ok() ? std::string() : added.error().message));
  const app::FreezeKey k1 = keyNow();
  CHECK(k1.content != k0.content);
  CHECK(k1.tempo == k0.tempo);
  {
    const auto d = app::freezeDocument(*f.snap(), f.lead);
    REQUIRE(d.has_value());
    bool hasMos = false;
    forEachTrack(*d, [&](const Track& t, const Track*) { hasMos |= t.id == f.mos; });
    CHECK(hasMos);
  }
  // Tempo and tunings.
  REQUIRE(f.store.dispatch(SetTempoMap{{TempoPoint{0, 90.0}}}).ok());
  const app::FreezeKey k2 = keyNow();
  CHECK(k2.tempo != k1.tempo);
  CHECK(k2.content != k1.content);
  CHECK(k2.tunings == k1.tunings);
  REQUIRE(f.store.dispatch(SetTuningParam{f.t31, ParamKind::TuningReference, 432.0}).ok());
  const app::FreezeKey k3 = keyNow();
  CHECK(k3.tunings != k2.tunings);
  CHECK(k3.content != k2.content);
  // Another rate: another key.
  auto p96 = app::planFreeze(*f.snap(), f.lead, 96000.0);
  REQUIRE(p96.ok());
  CHECK_FALSE(p96->key == k3);
}

// ---- AU.app (docs/reviews/RB-DY.md H2, H3, M2, M5, M6, L7) ----------------------------------------------------------

namespace {

std::string u8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}
fs::path ofU8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

}  // namespace

TEST_CASE("AU.app H2: a rate change re-renders a frozen track to a path of its own at the new rate, and it plays that") {
  playback_fixture::Fixture f;
  const fs::path dir = freshDir("rate");
  REQUIRE(f.store.dispatch(SetTrackFrozen{f.lead, true}).ok());
  auto link = makeLink(f.snap(), dir);
  REQUIRE(link->waitForFreeze());
  const auto at48 = link->frozenRender(f.lead);
  REQUIRE(at48.has_value());
  CHECK(at48->rate == 48000);
  CHECK(u8(ofU8(at48->path).filename()).find("-48000-") != std::string::npos);
  pump(*link);

  REQUIRE(link->setSampleRate(44100.0).ok());
  CHECK(link->frozenSlot(f.lead) == app::ProjectLink::kNoSlot);  // stale at once: live
  REQUIRE(link->waitForFreeze());
  pump(*link);
  REQUIRE(link->waitForFreeze());
  const auto at44 = link->frozenRender(f.lead);
  REQUIRE_MESSAGE(at44.has_value(), link->freezeStatus(f.lead).error);
  CHECK(at44->rate == 44100);
  CHECK(at44->path != at48->path);  // never renamed onto the file the pool held
  CHECK(u8(ofU8(at44->path).filename()).find("-44100-") != std::string::npos);
  CHECK(link->frozenSlot(f.lead) != app::ProjectLink::kNoSlot);
  CHECK(link->freezeStatus(f.lead).state == app::FreezeState::Ready);
  CHECK(link->freezeService()->stats().started == 2);
  CHECK(link->freezeService()->stats().failed == 0);
  link.reset();
  fs::remove_all(dir);
}

TEST_CASE("AU.app H3: a frozen track cabled from a group's sum keys on what reaches it; an unrelated edit neither "
          "re-plans nor flips it live; a feeder burst plans once, off the UI thread") {
  playback_fixture::Fixture f;
  f.store.setDeviceLibrary(&playback_fixture::unitsLibrary());
  const fs::path dir = freshDir("sum");
  // "Duck the frozen Lead from Grp": Grp's summed input (Mos) into the second input of a block on Lead's path.
  const DeviceId sub = test_blocks::appendAfter(f.store, f.lead, "atom:sub");
  Wire w;
  w.noteSource = f.grp;
  w.port = std::string(kMainPort);
  w.to = sub;
  w.param = "b";
  test_blocks::dispatchEdit(f.store, ops::connect(*f.snap(), f.store.ids(), w));
  {  // The render document: Lead, Grp (its blocks, no mixer, no volume lane) and Mos with its whole mixer; not Muted.
    const auto d = app::freezeDocument(*f.snap(), f.lead);
    REQUIRE(d.has_value());
    const Track* mos = findTrackPtr(*d, f.mos);
    const Track* grp = findTrackPtr(*d, f.grp);
    REQUIRE(mos != nullptr);
    REQUIRE(grp != nullptr);
    CHECK(mos->mixer.pan == doctest::Approx(0.5f));
    CHECK(grp->mixer == Mixer{});
    CHECK(grp->lanes.empty());
    CHECK(findTrackPtr(*d, f.muted) == nullptr);
    CHECK(d->returns.empty());
  }
  REQUIRE(f.store.dispatch(SetTrackFrozen{f.lead, true}).ok());
  auto link = makeLink(f.snap(), dir);
  [[maybe_unused]] const ListenerId lsub = f.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  REQUIRE(link->waitForFreeze());
  REQUIRE_MESSAGE(link->frozenSlot(f.lead) != app::ProjectLink::kNoSlot, link->freezeStatus(f.lead).error);
  pump(*link);
  const app::FreezeService::Stats s0 = link->freezeService()->stats();

  // Unrelated: Muted's fader (a drag of 20 steps), Lead's own fader, Grp's fader (after the sum Lead hears).
  for (int i = 0; i < 20; ++i) {
    REQUIRE(f.store.dispatch(SetTrackVolume{f.muted, -1.0f - static_cast<float>(i)}).ok());
    link->flush();
    CHECK(link->frozenSlot(f.lead) != app::ProjectLink::kNoSlot);
  }
  REQUIRE(f.store.dispatch(SetTrackVolume{f.lead, -9.0f}).ok());
  REQUIRE(f.store.dispatch(SetTrackVolume{f.grp, -6.0f}).ok());
  link->flush();
  {
    const app::FreezeService::Stats s = link->freezeService()->stats();
    CHECK(s.synced == s0.synced);    // each edit: one render-document compare, equal
    CHECK(s.planned == s0.planned);  // nothing serialised
    CHECK(s.started == s0.started);
  }
  CHECK(link->frozenSlot(f.lead) != app::ProjectLink::kNoSlot);
  CHECK(link->freezeStatus(f.lead).state == app::FreezeState::Ready);

  // What reaches the sum: Mos's fader. Stale at once (live), and the burst is planned and rendered once.
  for (int i = 0; i < 5; ++i) REQUIRE(f.store.dispatch(SetTrackVolume{f.mos, -2.0f - static_cast<float>(i)}).ok());
  link->flush();
  CHECK(link->frozenSlot(f.lead) == app::ProjectLink::kNoSlot);
  CHECK(link->freezeService()->stats().synced == s0.synced + 5);
  REQUIRE(link->waitForFreeze());
  pump(*link);
  REQUIRE(link->waitForFreeze());
  const app::FreezeService::Stats s1 = link->freezeService()->stats();
  CHECK(s1.planned == s0.planned + 1);
  CHECK(s1.started == s0.started + 1);
  CHECK(link->frozenSlot(f.lead) != app::ProjectLink::kNoSlot);
  link.reset();
  fs::remove_all(dir);
}

TEST_CASE("AU.app M6, M5, L7: a new cache folder gets a copy of the playing render on the service thread (it keeps "
          "playing); the first job sweeps strays; another build's cache or a rewritten sample moves the key") {
  playback_fixture::Fixture f;
  REQUIRE(f.store.dispatch(SetTrackFrozen{f.lead, true}).ok());
  const fs::path a = freshDir("copy_a"), b = freshDir("copy_b");
  const fs::path ca = a / "cache" / "freeze", cb = b / "cache" / "freeze";
  fs::create_directories(ca);
  std::ofstream(ca / "99-48000-0123456789abcdef.wav") << "stray";     // no .json names it
  std::ofstream(ca / "7-48000-0123456789abcdef.wav.part") << "half";  // an interrupted render
  app::FreezeService::Options o;
  o.jit = false;
  auto svc = std::make_unique<app::FreezeService>(o);
  (void)svc->sync(f.snap(), u8(a), u8(a), true);
  REQUIRE(svc->waitIdle(120.0));
  CHECK(svc->takeResults());
  std::vector<app::FreezeService::Render> r = svc->renders();
  REQUIRE(r.size() == 1);
  CHECK(r[0].current);
  const fs::path inA = ofU8(r[0].path);
  CHECK(inA.parent_path() == ca);
  CHECK_FALSE(fs::exists(ca / "99-48000-0123456789abcdef.wav"));
  CHECK_FALSE(fs::exists(ca / "7-48000-0123456789abcdef.wav.part"));
  CHECK(svc->stats().swept == 2);
  CHECK(svc->stats().started == 1);

  // Save As: the folder moves, the document does not. The render keeps playing; the copy happens on the thread.
  CHECK_FALSE(svc->sync(f.snap(), u8(b), u8(b), false));
  CHECK(svc->renders().at(0).current);
  CHECK(svc->status(f.lead).state == app::FreezeState::Ready);
  REQUIRE(svc->waitIdle(120.0));
  CHECK(svc->takeResults());
  r = svc->renders();
  REQUIRE(r.size() == 1);
  CHECK(r[0].current);
  CHECK(ofU8(r[0].path).parent_path() == cb);
  CHECK(fs::exists(cb / (std::to_string(f.lead.value) + ".json")));
  CHECK(fs::exists(inA));  // the old folder's cache keeps its file
  CHECK(svc->stats().started == 1);
  CHECK(svc->stats().cacheHits == 1);
  svc.reset();

  // L7: the key names the executable: a `.json` another build wrote does not match.
  const auto plan = app::planFreeze(*f.snap(), f.lead, 48000.0);
  REQUIRE(plan.ok());
  CHECK(plan->key.engine == jit::binaryHash());
  CHECK(plan->key.engine != 0);
  {
    const fs::path meta = cb / (std::to_string(f.lead.value) + ".json");
    std::ifstream in(meta);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const size_t at = text.find(app::hex64(plan->key.engine));
    REQUIRE(at != std::string::npos);
    text.replace(at, 16, "0000000000000001");
    std::ofstream(meta, std::ios::trunc) << text;
  }
  svc = std::make_unique<app::FreezeService>(o);
  (void)svc->sync(f.snap(), u8(b), u8(b), true);
  REQUIRE(svc->waitIdle(120.0));
  CHECK(svc->stats().cacheHits == 0);
  CHECK(svc->stats().started == 1);
  svc.reset();

  // L7: a sample overwritten in place moves the key (its size and write time), not the document's hash.
  {
    const fs::path wav = a / "x.wav";
    REQUIRE(media::writeWav(wav, {std::vector<float>(100, 0.1f)}, 48000).ok());
    SampleRef s;
    s.id = f.store.ids().sample();
    s.path = "x.wav";
    s.size = fs::file_size(wav);
    s.frames = 100;
    s.channels = 1;
    DocumentStore st(f.snap());
    REQUIRE(st.dispatch(AddSample{s}).ok());
    const auto k1 = app::planDocument(st.current(), 48000.0, u8(a));
    REQUIRE(k1.ok());
    REQUIRE(media::writeWav(wav, {std::vector<float>(200, 0.2f)}, 48000).ok());
    const auto k2 = app::planDocument(st.current(), 48000.0, u8(a));
    REQUIRE(k2.ok());
    CHECK(k1->key.content == k2->key.content);
    CHECK(k1->key.samples != k2->key.samples);
  }
  fs::remove_all(a);
  fs::remove_all(b);
}

TEST_CASE("AU.app M2: a freeze render and an export past the RIFF limit are RF64 and read back whole") {
  app::setLongWavLimitForTests(64 * 1024);  // stands for 4 GiB
  struct Restore {
    ~Restore() { app::setLongWavLimitForTests(0); }
  } restore;
  playback_fixture::Fixture f;
  const fs::path dir = freshDir("rf64");
  const auto plan = app::planFreeze(*f.snap(), f.lead, 48000.0);
  REQUIRE(plan.ok());
  app::OwnRenderRequest rq;
  rq.project = plan->project;
  rq.track = f.lead;
  rq.to = 2 * kTicksPerWhole;
  rq.tailMaxSeconds = 0.0;
  rq.file = dir / "own.wav";
  rq.jit = false;
  const auto own = app::renderTrackOwn(rq);
  REQUIRE_MESSAGE(own.ok(), (own.ok() ? std::string() : own.error().message));
  const auto info = media::probe(rq.file);
  REQUIRE(info.ok());
  CHECK(info->container == media::Container::Rf64);
  CHECK(info->frames == own->frames);
  const auto back = media::readWav(rq.file);
  REQUIRE(back.ok());
  CHECK(back->frames() == own->frames);

  // An export (32f, normalized: pass 2 reads the RF64 temp through the decoder and writes an RF64 file).
  app::ExportRequest e;
  e.snapshot = f.snap();
  e.to = 2 * kTicksPerWhole;
  e.bits = app::ExportBits::Float32;
  e.normalize = true;
  e.folder = dir;
  e.baseName = "Song";
  e.jit = false;
  const auto rep = app::runExport(e);
  REQUIRE_MESSAGE(rep.ok(), (rep.ok() ? std::string() : rep.error().message));
  REQUIRE(rep->files.size() == 1);
  const auto out = media::probe(rep->files[0].path);
  REQUIRE(out.ok());
  CHECK(out->container == media::Container::Rf64);
  const auto wav = media::readWav(rep->files[0].path);
  REQUIRE(wav.ok());
  CHECK(wav->frames() == rep->frames);
  fs::remove_all(dir);
}
