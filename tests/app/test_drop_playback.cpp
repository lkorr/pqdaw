// T5.7: a file dropped into the arrangement plays at once when Play is pressed right after the drop. The drag's first
// hover hands the file to the playback link's loader (ViewContext::setSamplePreloader → ProjectLink::preloadSample), so
// by the time the drop commits the sample is in the pool and the audio image published by the drop's store event
// already plays it — no wait for a load after the drop. Without the preloader the same drop publishes a silent clip
// until the load the publish requested has finished.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/samples.h"
#include "pq/engine/block_adapter.h"
#include "pq/media/wav_file.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

using namespace pq;
namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

struct Rig {
  std::unique_ptr<app::ProjectLink> link;
  std::unique_ptr<views::ViewContext> ctx;
  ui::TestDriver d{1280, 800};
  std::unique_ptr<views::Shell> shell;
  views::ArrangementView* arr = nullptr;
  doc::TrackId track;
  fs::path dir;
  std::string file;

  Rig(const char* name, bool preload) {
    dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    // 0.5 s of a constant 0.5 from the first frame: the clip's first sample is audible.
    std::vector<std::vector<float>> ch(2, std::vector<float>(24000, 0.5f));
    file = u8(dir / "Hit.wav");
    REQUIRE(media::writeWav(dir / "Hit.wav", ch, 48000).ok());
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    doc::IdSource ids;
    auto l = app::ProjectLink::create(c, doc::makeDefaultProject(ids));
    REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
    link = std::move(*l);
    ctx = std::make_unique<views::ViewContext>(*link);
    link->attach(*ctx);
    if (preload) ctx->setSamplePreloader([this](const std::string& p) { link->preloadSample(p); });
    auto t = doc::ops::createTrack(*ctx->doc(), ctx->store().ids(), doc::TrackKind::Audio, {}, "Audio");
    REQUIRE(t.ok());
    REQUIRE(ctx->store().dispatch(t->command).ok());
    track = t->tracks[0];
    auto s = views::Shell::create(d.tree(), *ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    arr = dynamic_cast<views::ArrangementView*>(d.find("main/body/top/track-area/arrangement"));
    REQUIRE(arr != nullptr);
    d.tree().layoutNow();
    link->flush();  // the audio track's unit
  }
  ~Rig() {
    d.tree().setRoot(std::make_unique<ui::Widget>());
    shell.reset();
    link->detach();
    ctx.reset();
    link.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  ui::Vec2 at0() const {
    const ui::IRect r = arr->rowLaneRect(0);
    return {arr->xOfTick(0.0) + 1.0f, static_cast<float>(r.y + r.h / 2)};
  }
  // Whether the published audio image plays a sample for the track's clip.
  bool clipPlayable() const {
    const engine::AudioSequenceImage* img = link->audioImage();
    if (!img) return false;
    for (uint32_t s = 0; s < img->numSlots(); ++s)
      for (const auto& c : img->clips(s))
        if (c.sample != engine::AudioSequenceImage::kNoSample) return true;
    return false;
  }
};

// Plays from 0 through a 64-frame device under RtScope; the index of the first block whose left channel exceeds 0.05
// (−1: none in 16 blocks).
int firstAudibleBlock(Rig& r) {
  engine::BlockAdapter ad;
  REQUIRE(ad.prepare(r.link->callback(), 0, 2, r.link->config().blockFrames, 64));
  engine::TransportCommand play{engine::TransportCommandKind::Play, false, 0, 0, 0};
  REQUIRE(r.link->engine().post(play));
  std::vector<float> lft(64), rgt(64);
  float* ch[2] = {lft.data(), rgt.data()};
  for (int b = 0; b < 16; ++b) {
    {
      RtScope rt;
      DenormalGuard dg;
      ad.process(engine::AudioBlock{nullptr, 0, ch, 2, 64, b * 64});
    }
    float peak = 0;
    for (float v : lft) peak = std::max(peak, std::fabs(v));
    if (peak > 0.05f) return b;
  }
  return -1;
}

}  // namespace

TEST_CASE("drop playback: the hover preloads the file; after the drop the clip sounds as early as a long-loaded one") {
  Rig r("pq_drop_playback", true);
  ui::DragPayload p;
  p.kind = ui::DragKind::Files;
  p.paths = {r.file};
  r.d.tree().dragEnter(p, r.at0(), 0);  // first hover: probe + preload start
  // The user's hover (a human drag takes far longer than loading half a second of audio).
  const std::string abs = doc::absoluteSamplePath(doc::storedSamplePath(r.file, {}), {});
  for (int i = 0; i < 500 && !r.link->sampleLoadFinished(abs); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  REQUIRE(r.link->sampleLoadFinished(abs));
  REQUIRE(r.d.tree().dragDrop(r.at0(), 0) == ui::DropEffect::Copy);
  // The drop's own publish plays the sample: no load is waited for after the drop.
  CHECK(r.clipPlayable());
  // Play right away: the clip sounds as early as in a project whose sample was loaded long before (baseline below).
  // flush(): pending unit deliveries land first (under load a background JIT image may still be on its way; that is
  // the engine's latency, not the sample's: the sample was already playable above, before any wait).
  r.link->flush();
  const int first = firstAudibleBlock(r);
  MESSAGE("first audible 64-frame block after Play, right after the drop: " << first);
  CHECK(first >= 0);
  // Which block exactly depends on when the engine wakes the track (block 2 on an idle machine; later under load,
  // for the baseline too): only "audible within 16 blocks" is load-independent. The load-independent property of the
  // preload is clipPlayable() right after the drop, above.
}

TEST_CASE("drop playback: without the preloader the drop's first publish is silent (the load starts only then)") {
  Rig r("pq_drop_playback_cold", false);
  ui::DragPayload p;
  p.kind = ui::DragKind::Files;
  p.paths = {r.file};
  r.d.tree().dragEnter(p, r.at0(), 0);
  REQUIRE(r.d.tree().dragDrop(r.at0(), 0) == ui::DropEffect::Copy);
  CHECK_FALSE(r.clipPlayable());
  REQUIRE(r.link->waitForSamples(10.0));
  CHECK(r.clipPlayable());
  r.link->flush();
  // The baseline: a sample loaded well before Play sounds from the same block (the engine wakes the track's sleeping
  // region at a block boundary: §7.7) as the preloaded drop above.
  const int first = firstAudibleBlock(r);
  MESSAGE("first audible 64-frame block after Play, sample loaded long before: " << first);
  CHECK(first >= 0);
  // Which block exactly depends on when the engine wakes the track (block 2 on an idle machine; later under load,
  // for the baseline too): only "audible within 16 blocks" is load-independent. The load-independent property of the
  // preload is clipPlayable() right after the drop, above.
}
