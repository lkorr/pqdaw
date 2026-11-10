// T6.10: auditioning a take through the app's wiring (a ViewContext on the ProjectLink, the Shell): a click on a take
// lane's audition button (real dispatch) makes the link play the take instead of the main lane without a compile
// (a Sequence change: the track's notes are re-sequenced, counted by the link's compile counter).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../../src/views/src/arrangement/internal.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/rt_scope.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;

}  // namespace

TEST_CASE("comping link: a take auditioned from its header plays through the link with no compile (T6.10)") {
  // A MIDI track with an instrument, an EMPTY main lane and one take holding a long note.
  DocumentStore st{playback_fixture::unwiredDefaultProject()};
  IdSource& ids = st.ids();
  const TrackId track = ids.track();
  Batch b{"fixture", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Comp"), {}, kAppend});
  (void)playback_fixture::instrument(b, ids, track, ids.device());
  REQUIRE(st.dispatch(b).ok());
  playback_fixture::wireSeries(st);
  const ClipPtr take = playback_fixture::clip(ids, 0, 4 * kTicksPerWhole, {playback_fixture::note(ids, 0, 4 * kTicksPerWhole, 0)}, "take");
  const ClipSlot slot{take->id, take, nullptr};
  auto add = ops::addTakeLane(*st.current(), ids, track, std::span<const ClipSlot>(&slot, 1));
  REQUIRE(add.ok());
  REQUIRE(st.dispatch(add->command).ok());
  const LaneId lane = st.current()->tracks.back()->takes.at(0)->id;

  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  auto l = app::ProjectLink::create(c, st.current());
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  std::unique_ptr<app::ProjectLink> link = std::move(*l);
  auto ctx = std::make_unique<views::ViewContext>(*link);
  ctx->store().reset(st.current());  // the same snapshot: attach() compiles nothing
  link->attach(*ctx);
  ctx->setDeviceLibrary(&link->devices());
  ui::TestDriver d{1280, 800};
  auto s = views::Shell::create(d.tree(), *ctx);
  REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
  std::unique_ptr<views::Shell> shell = std::move(*s);
  d.tree().layoutNow();
  d.step();
  link->flush();

  auto render = [&](double seconds) {
    link->flush();
    engine::Engine& e = link->engine();
    REQUIRE(e.post(engine::TransportCommand{engine::TransportCommandKind::Play, false, 0, 0, 0}));
    const auto blocks = static_cast<uint32_t>(std::lround(seconds * c.sampleRate / kBlock));
    float peak = 0.0f;
    float lb[kBlock], rb[kBlock];
    float* outs[2] = {lb, rb};
    for (uint32_t i = 0; i < blocks; ++i) {
      {
        RtScope rt;
        e.process(engine::AudioBlock{nullptr, 0, outs, 2, kBlock, 0, 0, 0});
      }
      for (float v : lb) peak = std::max(peak, std::abs(v));
      if ((i & 63) == 0) e.collect();
    }
    REQUIRE(e.post(engine::TransportCommand{engine::TransportCommandKind::Stop, false, 0, 0, 0}));
    for (uint32_t i = 0; i < 1500; ++i) {  // 2 s stopped: every release tail ends before the next render
      e.process(engine::AudioBlock{nullptr, 0, outs, 2, kBlock, 0, 0, 0});
      if ((i & 63) == 0) e.collect();
    }
    e.collect();
    return peak;
  };
  CHECK(render(0.25) == 0.0f);  // the main lane is empty: silence

  auto* arr = dynamic_cast<views::ArrangementView*>(d.find("main/body/top/track-area/arrangement"));
  REQUIRE(arr != nullptr);
  REQUIRE(arr->takeHeaderWidgets().size() == 1);
  views::arrangement::TakeHeader& h = *arr->takeHeaderWidgets()[0];
  const ui::IRect ar = h.auditionRect();
  const uint64_t compiles = link->stats().compiles;
  d.click({static_cast<float>(ar.x) + 0.5f * static_cast<float>(ar.w), static_cast<float>(ar.y) + 0.5f * static_cast<float>(ar.h)});
  d.step();
  CHECK(findTrackPtr(*ctx->doc(), track)->auditionTake == lane);
  CHECK(render(0.25) > 1e-3f);  // the take plays
  CHECK(link->stats().compiles == compiles);  // a Sequence change: nothing compiled
  // Back to the main lane: silent again, still no compile.
  d.click({static_cast<float>(ar.x) + 0.5f * static_cast<float>(ar.w), static_cast<float>(ar.y) + 0.5f * static_cast<float>(ar.h)});
  d.step();
  CHECK_FALSE(findTrackPtr(*ctx->doc(), track)->auditionTake.valid());
  CHECK(render(0.25) == 0.0f);
  CHECK(link->stats().compiles == compiles);

  d.tree().setRoot(std::make_unique<ui::Widget>());
  shell.reset();
  link->detach();
  ctx.reset();
  link.reset();
}
