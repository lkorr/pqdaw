// AR.1 (docs/plans/TU.md "AR.1"): automation recording through the app's wiring (a ViewContext on the ProjectLink, the
// Shell, real pointer and key events). Arrangement Record with Automation Arm on: a drag of the Tunings panel's
// Generator box writes the MOS generator's lane (made on the selected track) at the engine's record ticks while a live
// note is recorded; the drag plays at once (the held note follows it, no compile); Stop commits the take and the lane as
// ONE undo step (Ctrl+Z removes both). Automation Arm off (the control bar's button): the same pass records the notes
// and no automation.
#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/rt_scope.h"
#include "pq/document/automation_curve.h"
#include "pq/ui/test_driver.h"
#include "pq/views/shell/shell.h"
#include "pq/views/tunings/tunings_panel.h"
#include "pq/views/view_context.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

TuningDef mosDef(double generator) {
  TuningDef d;
  tuning::MosParams m;
  m.n = 7;
  m.period = 1200.0;
  m.generator = generator;
  d.system = m;
  return d;
}

engine::RawMidi midi(uint8_t status, uint8_t d1, uint8_t d2) {
  engine::RawMidi m;
  m.port = 0;
  m.status = status;
  m.d1 = d1;
  m.d2 = d2;
  return m;
}

// One armed MIDI track (Keys input, the default instrument) following a 5L 2s main tuning; a ViewContext on the link,
// the Shell with the Tunings panel shown; the track selected (a tuning lane's new home).
struct App {
  std::unique_ptr<app::ProjectLink> link;
  std::unique_ptr<views::ViewContext> ctx;
  ui::TestDriver d{1600, 900};
  std::unique_ptr<views::Shell> shell;
  TrackId track;
  TuningId tuning;

  App() {
    DocumentStore st{playback_fixture::unwiredDefaultProject()};
    IdSource& ids = st.ids();
    track = ids.track();
    tuning = st.current()->tunings->front()->id;
    Batch b{"fixture", {}};
    b.commands.push_back(SetTuningDef{tuning, mosDef(700.0)});
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Live"), {}, kAppend});
    b.commands.push_back(SetTrackTuning{track, TuningAssignment::mainTuning(tuning)});
    b.commands.push_back(SetNoteInput{track, NoteInput{NoteInputMode::Keys, InputKeyMap::Tuning}});
    (void)playback_fixture::instrument(b, ids, track, ids.device());
    REQUIRE(st.dispatch(b).ok());
    playback_fixture::wireSeries(st);
    auto arm = ops::armTrack(*st.current(), track, false);
    REQUIRE(arm.ok());
    REQUIRE(st.dispatch(arm->command).ok());

    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = false;
    auto l = app::ProjectLink::create(c, st.current());
    REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
    link = std::move(*l);
    ctx = std::make_unique<views::ViewContext>(*link);
    ctx->store().reset(st.current());
    link->attach(*ctx);
    ctx->setDeviceLibrary(&link->devices());
    auto s = views::Shell::create(d.tree(), *ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    ctx->setTuningsVisible(true);
    ctx->setSelectedTrack(track);
    d.tree().layoutNow();
    d.step();
    link->setLivePorts({"Port"}, {2.0f});
    link->flush();
  }
  ~App() {
    d.tree().setRoot(std::make_unique<ui::Widget>());
    shell.reset();
    link->detach();
    ctx.reset();
    link.reset();
  }

  // `seconds` of engine blocks (under RtScope), the UI's playhead poll (the session's notes and its end) as it goes.
  std::vector<float> run(double seconds) {
    link->flush();
    engine::Engine& e = link->engine();
    const auto blocks = static_cast<uint32_t>(std::lround(seconds * kRate / kBlock));
    std::vector<float> out;
    float l[kBlock], r[kBlock];
    float* outs[2] = {l, r};
    for (uint32_t b = 0; b < blocks; ++b) {
      {
        RtScope rt;
        e.process(engine::AudioBlock{nullptr, 0, outs, 2, kBlock, 0, 0, 0});
      }
      out.insert(out.end(), l, l + kBlock);
      if ((b & 15) == 15) (void)link->playhead();
      if ((b & 63) == 0) e.collect();
    }
    (void)link->playhead();
    return out;
  }
  void stop() {
    ctx->stopButton();
    (void)run(0.05);
  }

  ui::DragBox& generator() {
    auto* panel = dynamic_cast<views::TuningsPanel*>(d.find("main/body/top/tunings/tunings-panel"));
    REQUIRE(panel != nullptr);
    views::TuningCard* c = panel->card(tuning);
    REQUIRE(c != nullptr);
    return c->row(ParamKind::MosGenerator)->box();
  }
  const AutomationLane* generatorLane() const {
    const ParamRef g = ParamRef::tuning(ParamKind::MosGenerator, tuning);
    const AutomationLane* out = nullptr;
    forEachTrack(*ctx->doc(), [&](const Track& t, const Track*) {
      for (const LanePtr& l : t.lanes)
        if (l->param == g) out = l.get();
    });
    return out;
  }
  const Track& theTrack() const { return *findTrackPtr(*ctx->doc(), track); }
  double gen() const { return findTuning(*ctx->doc(), tuning)->def.mos()->generator; }
};

ui::Vec2 centre(const ui::IRect& r) { return {static_cast<float>(r.x) + r.w / 2.0f, static_cast<float>(r.y) + r.h / 2.0f}; }

}  // namespace

TEST_CASE("automation recording: a Generator drag while recording writes the tuning's lane at the record ticks; Stop "
          "commits take + lane as one undo step; Ctrl+Z removes both; Automation Arm off writes none (AR.1)") {
  App a;
  REQUIRE(a.ctx->doc()->record.automationArm);
  engine::LivePorts& ports = *a.link->livePorts();
  const size_t h0 = a.ctx->store().history().cursor();
  const uint64_t commits0 = a.link->recordCommits();

  a.ctx->record();
  (void)a.run(0.1);
  REQUIRE(a.link->playhead().recording);
  REQUIRE(ports.push(0, midi(0x90, 64, 100)));
  (void)a.run(0.1);

  // The drag: each step's generator at the tick the engine records at (what the link reports when the step lands).
  const uint64_t compiles = a.link->stats().compiles;
  ui::DragBox& gen = a.generator();
  const ui::Vec2 c = centre(gen.rect());
  a.d.move(c);
  a.d.down(c);
  std::vector<std::pair<Ticks, double>> written;
  for (int i = 1; i <= 6; ++i) {
    a.d.move({c.x, c.y - 6.0f * static_cast<float>(i)});  // 0.5 ¢ per px: +3 ¢ per step
    const std::optional<Ticks> at = a.link->recordingTick();
    REQUIRE(at.has_value());
    written.emplace_back(*at, a.gen());
    (void)a.run(0.05);
  }
  a.d.up({c.x, c.y - 36.0f});
  CHECK(a.link->stats().compiles == compiles);  // the drag plays at once: param writes, no compile
  CHECK(a.ctx->recordingAutomation());
  CHECK(a.generatorLane() == nullptr);  // nothing in the document while recording
  REQUIRE(ports.push(0, midi(0x80, 64, 0)));
  (void)a.run(0.1);
  const Ticks after = *a.link->recordingTick();
  a.stop();

  REQUIRE(a.link->recordCommits() == commits0 + 1);
  CHECK_FALSE(a.ctx->recordingAutomation());
  REQUIRE(a.theTrack().takes.size() == 1);  // the notes' take
  const AutomationLane* lane = a.generatorLane();
  REQUIRE(lane != nullptr);
  bool onTrack = false;
  for (const LanePtr& l : a.theTrack().lanes) onTrack = onTrack || l.get() == lane;
  CHECK(onTrack);  // the selected track
  CHECK(lane->points.size() <= written.size() + 1);  // thinned (the drag is a straight ramp per step)
  CHECK(automationValueBefore(lane->points, written.front().first) == doctest::Approx(700.0));  // before the touch
  for (const auto& [tick, value] : written) {
    CAPTURE(tick);
    CHECK(std::fabs(automationValueAt(lane->points, tick) - value) <= 0.05 + 1e-3);
  }
  CHECK(automationValueAt(lane->points, after) == doctest::Approx(written.back().second));  // held after the release
  CHECK(a.ctx->store().history().cursor() == h0 + 2);  // the drag's own step, then "Record"

  // Ctrl+Z: the take and the lane go together.
  REQUIRE(a.d.press("Ctrl+Z") == ui::KeyResult::Action);
  CHECK(a.theTrack().takes.empty());
  CHECK(a.generatorLane() == nullptr);

  // Automation Arm off (the control bar's button): the next pass records its notes and no automation.
  ui::Widget* arm = a.d.find("main/control-bar/automation-arm");
  REQUIRE(arm != nullptr);
  a.d.click(centre(arm->rect()));
  REQUIRE_FALSE(a.ctx->doc()->record.automationArm);
  a.ctx->record();
  (void)a.run(0.1);
  REQUIRE(ports.push(0, midi(0x90, 64, 100)));
  (void)a.run(0.05);
  ui::DragBox& gen2 = a.generator();
  const ui::Vec2 c2 = centre(gen2.rect());
  a.d.move(c2);
  a.d.down(c2);
  a.d.move({c2.x, c2.y - 12.0f});
  (void)a.run(0.05);
  a.d.up({c2.x, c2.y - 12.0f});
  REQUIRE(ports.push(0, midi(0x80, 64, 0)));
  (void)a.run(0.05);
  CHECK_FALSE(a.ctx->recordingAutomation());
  a.stop();
  REQUIRE(a.link->recordCommits() == commits0 + 2);
  CHECK(a.theTrack().takes.size() == 1);
  CHECK(a.generatorLane() == nullptr);
}
