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
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/export_render.h"
#include "pq/app/freeze_service.h"
#include "pq/app/project_link.h"

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
  // (A link's FIRST pass differs from later ones in its first ~26 ms — Grp's automated volume smoother starts from
  // another state: the history dependence of QUESTIONS Q251 — so the references are second passes, as every pass below.)
  (void)master(*link, *f.snap(), to);
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
  (void)master(*link, *f.snap(), to);  // (a link's first pass: see the first test)
  const Channels played = master(*link, *f.snap(), to);
  {
    auto plain = makeLink(f.snap(), dir, false);  // the same document, every track live
    (void)master(*plain, *f.snap(), to);         // (its first pass: see the first test)
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
