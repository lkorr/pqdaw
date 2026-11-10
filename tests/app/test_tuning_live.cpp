// TU.1: live retune through the app's wiring (a ViewContext on the ProjectLink, the Shell, the Tunings panel and the
// MOS wheel, driven by real pointer events): a held note's Hz follows a scripted drag at EVERY intermediate step, not
// only on release, and a continuous drag never compiles.
//   generator   the Tunings panel's Generator box dragged on a 5L 2s main tuning: a drawn Degree note and a held live
//               Keys note (bent +100 ¢: T6.4b) sound at the tuning's Hz (× the bend) after each step; 0 compiles; one
//               undo step
//   automated   the same drag while a generator lane plays: the drag overrides the lane (Live's rule) and the note
//               follows the drag, not the lane
//   wheel       the MOS wheel's chart drag on the main tuning: the note follows each step; 0 compiles
//   edo steps   the Steps box dragged 12 → 15 → 18 on an EDO main tuning: a new table per whole step (discrete: a
//               compile each), the held drawn note retuned at every step with no gap in the sound, then a held live
//               Keys note likewise (LiveInput moves held keys to a new route image's pitch)
//   hz input    M6 review L3: a held live Hz-input note keeps its Hz through a Generator drag (a raw pitch)
//   mos shape   M6 review L4: a MOS size + generator change (a new table): before and after the compile the held
//               note sounds the old or the new pitch, never a third (the old table at the new generator)
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/rt_scope.h"
#include "pq/document/input_map.h"
#include "pq/document/pitch_resolve.h"
#include "pq/ui/test_driver.h"
#include "pq/views/mos_wheel/mos_wheel.h"
#include "pq/views/shell/shell.h"
#include "pq/views/tunings/tunings_panel.h"
#include "pq/views/view_context.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr int32_t kDegree = 4;

TuningDef mosDef(double generator) {
  TuningDef d;
  tuning::MosParams m;
  m.n = 7;
  m.period = 1200.0;
  m.generator = generator;
  d.system = m;
  return d;
}

// One MIDI track following main tuning `def` (the project's first tuning, so the panel shows it first), with the
// default instrument, a drawn Degree note held for two bars, armed (Keys input); a ViewContext on the link and the
// Shell with the Tunings panel shown.
struct LiveApp {
  std::unique_ptr<app::ProjectLink> link;
  std::unique_ptr<views::ViewContext> ctx;
  ui::TestDriver d{1600, 900};
  std::unique_ptr<views::Shell> shell;
  TrackId track;
  TuningId tuning;

  explicit LiveApp(const TuningDef& def, std::optional<double> laneFrom = std::nullopt) {
    // Built first and handed to the link at creation (its first compile publishes at once; this test runs the engine
    // on its own thread, so a later topology publish would wait for an audio thread that is not running).
    DocumentStore st{playback_fixture::unwiredDefaultProject()};
    IdSource& ids = st.ids();
    track = ids.track();
    tuning = st.current()->tunings->front()->id;
    Batch b{"fixture", {}};
    b.commands.push_back(SetTuningDef{tuning, def});
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Live"), {}, kAppend});
    b.commands.push_back(SetTrackTuning{track, TuningAssignment::mainTuning(tuning)});
    b.commands.push_back(SetNoteInput{track, NoteInput{NoteInputMode::Keys, InputKeyMap::Tuning}});
    if (laneFrom) {
      AutomationLane lane;
      lane.id = ids.lane();
      lane.param = ParamRef::tuning(ParamKind::MosGenerator, tuning);
      lane.points = {{0, static_cast<float>(*laneFrom), 0.0f, ids.breakpoint()},
                     {4 * kTicksPerWhole, static_cast<float>(*laneFrom + 10.0), 0.0f, ids.breakpoint()}};
      b.commands.push_back(AddAutomationLane{track, std::make_shared<const AutomationLane>(lane)});
    }
    (void)playback_fixture::instrument(b, ids, track, ids.device());
    REQUIRE(st.dispatch(b).ok());
    playback_fixture::wireSeries(st);
    Note n = playback_fixture::note(ids, 0, 4 * kTicksPerWhole, kDegree);
    REQUIRE(st.dispatch(AddClip{track, playback_fixture::clip(ids, 0, 4 * kTicksPerWhole, {n}, "held")}).ok());
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
    ctx->store().reset(st.current());  // the same snapshot: attach() then compiles nothing
    link->attach(*ctx);
    ctx->setDeviceLibrary(&link->devices());
    auto s = views::Shell::create(d.tree(), *ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    ctx->setTuningsVisible(true);
    d.tree().layoutNow();
    d.step();
    engine::Engine& e = link->engine();
    REQUIRE(e.post(engine::TransportCommand{engine::TransportCommandKind::Play, false, 0, 0, 0}));
  }
  ~LiveApp() {
    d.tree().setRoot(std::make_unique<ui::Widget>());
    shell.reset();
    link->detach();
    ctx.reset();
    link.reset();
  }

  // Renders `seconds` of audio (the left channel). `compile` false: without compiling first, as the audio thread plays
  // while the app's worker compiles (M6 review L4).
  std::vector<float> run(double seconds, bool compile = true) {
    if (compile) link->flush();  // a structural change compiles here (the app's worker does it after its debounce)
    engine::Engine& e = link->engine();
    const auto blocks = static_cast<uint32_t>(std::lround(seconds * kRate / kBlock));
    std::vector<float> out(static_cast<size_t>(blocks) * kBlock);
    float l[kBlock], r[kBlock];
    float* outs[2] = {l, r};
    for (uint32_t b = 0; b < blocks; ++b) {
      {
        RtScope rt;
        e.process(engine::AudioBlock{nullptr, 0, outs, 2, kBlock, 0, 0, 0});
      }
      std::copy(l, l + kBlock, out.begin() + static_cast<std::ptrdiff_t>(b) * kBlock);
      if ((b & 63) == 0) e.collect();
    }
    return out;
  }

  // The Hz the tuning gives the held Degree note now (doc::resolvePitch in the document's current definition).
  double expectedHz() const {
    const TuningDef& def = doc::findTuning(*ctx->doc(), tuning)->def;
    auto t = doc::buildTuning(def);
    REQUIRE(t.ok());
    const NotePitch p = doc::resolvePitch(tuning::PitchId::fromDegree(kDegree, 0), *t, tuning);
    REQUIRE(p.hz.has_value());
    return *p.hz;
  }

  views::TuningCard& card() {
    auto* panel = dynamic_cast<views::TuningsPanel*>(d.find("main/body/top/tunings/tunings-panel"));
    REQUIRE(panel != nullptr);
    views::TuningCard* c = panel->card(tuning);
    REQUIRE(c != nullptr);
    return *c;
  }
};

// Frequency over the last `seconds` of `x`: upward zero crossings, linearly interpolated.
double hzOfTail(const std::vector<float>& x, double seconds) {
  std::vector<double> at;
  const size_t i0 = x.size() - static_cast<size_t>(seconds * kRate);
  for (size_t i = std::max<size_t>(i0, 1); i < x.size(); ++i)
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) at.push_back(static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]));
  REQUIRE(at.size() > 4);
  return (static_cast<double>(at.size()) - 1.0) / ((at.back() - at.front()) / kRate);
}

double cents(double a, double b) { return 1200.0 * std::log2(a / b); }

ui::Vec2 centre(const ui::IRect& r) { return {static_cast<float>(r.x) + r.w / 2.0f, static_cast<float>(r.y) + r.h / 2.0f}; }

}  // namespace

TEST_CASE("tuning live: a panel Generator drag retunes held drawn and live notes at every step, no compile (TU.1, T6.4b)") {
  LiveApp a(mosDef(700.0));
  // A live Keys note on the same degree, later bent +100 ¢ (half of the ±2-semitone range: 12288).
  a.link->setLivePorts({"Port"}, {2.0f});
  auto map = buildInputPitchMap(*a.ctx->doc(), a.track);
  REQUIRE(map.ok());
  uint8_t key = 0;
  for (uint8_t k = 1; k < 128; ++k)
    if ((*map)->at(k).mapped && (*map)->at(k).id == tuning::PitchId::fromDegree(kDegree, 0)) key = k;
  REQUIRE(key != 0);
  (void)a.run(0.2);
  const double hz0 = hzOfTail(a.run(0.15), 0.1);
  CHECK(std::fabs(cents(hz0, a.expectedHz())) < 0.05);

  const uint64_t compiles = a.link->stats().compiles;
  const size_t h0 = a.ctx->store().history().cursor();
  ui::DragBox& gen = a.card().row(ParamKind::MosGenerator)->box();
  const ui::Vec2 c = centre(gen.rect());
  a.d.move(c);
  a.d.down(c);
  std::vector<double> heard, wanted;
  for (int i = 1; i <= 8; ++i) {
    a.d.move({c.x, c.y - 6.0f * static_cast<float>(i)});  // 0.5 ¢ per px: +3 ¢ per step
    const double want = a.expectedHz();
    CHECK(a.ctx->doc()->tunings->front()->def.mos()->generator == doctest::Approx(700.0 + 3.0 * i));
    const double got = hzOfTail(a.run(0.12), 0.08);
    heard.push_back(got);
    wanted.push_back(want);
    CHECK(std::fabs(cents(got, want)) < 0.05);  // this step, while the button is still down
  }
  a.d.up({c.x, c.y - 48.0f});
  CHECK(a.link->stats().compiles == compiles);  // every step a param write
  CHECK(a.ctx->store().history().cursor() == h0 + 1);
  CHECK(cents(heard.back(), heard.front()) > 10.0);  // it moved (degree 4 is one generator up: +21 ¢ over the drag)

  // T6.4b: a held live Keys note, bent, keeps its bend while the next drag moves the tuning.
  auto& ports = *a.link->livePorts();
  REQUIRE(ports.push(0, [&] {
    engine::RawMidi m;
    m.port = 0;
    m.status = 0xE0;
    m.d1 = 0x00;
    m.d2 = 0x60;  // 12288 → +100 ¢
    return m;
  }()));
  REQUIRE(ports.push(0, [&] {
    engine::RawMidi m;
    m.port = 0;
    m.status = 0x90;
    m.d1 = key;
    m.d2 = 100;
    return m;
  }()));
  // Mute the drawn note's clip so the live note is heard alone.
  const Track* t = findTrackPtr(*a.ctx->doc(), a.track);
  REQUIRE(a.ctx->store().dispatch(SetClipActive{ClipRef{a.track, t->clips.front().id}, false}).ok());
  (void)a.run(0.1);
  a.d.move(c);
  a.d.down(c);
  for (int i = 1; i <= 4; ++i) {
    a.d.move({c.x, c.y + 10.0f * static_cast<float>(i)});  // −5 ¢ per step
    const double got = hzOfTail(a.run(0.12), 0.08);
    CHECK(std::fabs(cents(got, a.expectedHz()) - 100.0) < 0.05);  // tuned Hz × the bend ratio
  }
  a.d.up({c.x, c.y + 40.0f});
  CHECK(a.link->stats().compiles == compiles);
}

TEST_CASE("tuning live: a drag over a playing generator lane overrides it; the note follows the drag (TU.1)") {
  LiveApp a(mosDef(700.0), 700.0);
  (void)a.run(0.2);
  const uint64_t compiles = a.link->stats().compiles;
  ui::DragBox& gen = a.card().row(ParamKind::MosGenerator)->box();
  const ui::Vec2 c = centre(gen.rect());
  a.d.move(c);
  a.d.down(c);
  for (int i = 1; i <= 5; ++i) {
    a.d.move({c.x, c.y - 10.0f * static_cast<float>(i)});  // +5 ¢ per step, far from the lane's 700 … 710 ¢
    const double got = hzOfTail(a.run(0.12), 0.08);
    CHECK(std::fabs(cents(got, a.expectedHz())) < 0.05);
  }
  a.d.up({c.x, c.y - 50.0f});
  CHECK(a.ctx->automationOverridden(ParamRef::tuning(ParamKind::MosGenerator, a.tuning)));
  CHECK(a.link->stats().compiles == compiles);
}

TEST_CASE("tuning live: the MOS wheel's chart drag on a main tuning retunes the held note at every step, no compile (TU.1)") {
  LiveApp a(mosDef(700.0));
  (void)a.run(0.2);
  views::MosWheelPopover* w = views::openMosWheel(a.shell->overlay(), *a.ctx, views::TuningTarget::forMain(a.tuning));
  REQUIRE(w != nullptr);
  a.d.tree().layoutNow();
  a.d.step();
  const uint64_t compiles = a.link->stats().compiles;
  views::MosChart& chart = w->chart();
  const ui::IRect ax = chart.axisRect();
  const float y = static_cast<float>(ax.y) + ax.h / 2.0f;
  a.d.down({chart.xOfCents(700.0), y});
  double first = 0.0, last = 0.0;
  for (const double g : {702.3, 705.1, 708.9, 711.4, 714.0}) {
    a.d.move({chart.xOfCents(g), y}, ui::mod::kAlt);  // Alt: no EDO snapping
    a.d.step();
    const double got = hzOfTail(a.run(0.12), 0.08);
    CHECK(std::fabs(cents(got, a.expectedHz())) < 0.05);
    if (first == 0.0) first = got;
    last = got;
  }
  a.d.up({chart.xOfCents(714.0), y}, ui::PointerButton::Left, ui::mod::kAlt);
  CHECK(a.link->stats().compiles == compiles);
  CHECK(cents(last, first) > 10.0);  // 702.3 → 714 ¢: degree 4 of this mode is one generator up (+11.7 ¢)
  w->apply();
  a.d.step();
}

TEST_CASE("tuning live: the Steps box dragged 12 -> 15 swaps a table per whole step; the held note retunes each time (TU.1)") {
  LiveApp a(TuningDef{});  // 12-EDO
  (void)a.run(0.2);
  const uint64_t compiles = a.link->stats().compiles;
  ui::DragBox& steps = a.card().row(std::nullopt)->box();
  const ui::Vec2 c = centre(steps.rect());
  a.d.move(c);
  a.d.down(c);
  uint32_t n = 12;
  std::vector<float> all;
  for (int i = 1; i <= 30 && n < 15; ++i) {
    a.d.move({c.x, c.y - 1.0f * static_cast<float>(i)});  // 600 px per 999 steps: ~0.6 px per step
    const uint32_t now = a.ctx->doc()->tunings->front()->def.edo()->n();
    if (now == n) continue;
    n = now;
    const std::vector<float> x = a.run(0.12);
    all.insert(all.end(), x.begin(), x.end());
    CHECK(std::fabs(cents(hzOfTail(x, 0.08), a.expectedHz())) < 0.05);  // 4\n of the new EDO, during the drag
  }
  a.d.up({c.x, c.y - 30.0f});
  CHECK(n >= 15u);
  CHECK(a.link->stats().compiles > compiles);  // discrete: new tables
  // The note never stopped: no silent stretch of a millisecond in what was rendered across the swaps.
  size_t quiet = 0, worst = 0;
  for (float v : all) {
    quiet = std::fabs(v) < 1e-4f ? quiet + 1 : 0;
    worst = std::max(worst, quiet);
  }
  CHECK(worst < static_cast<size_t>(kRate / 1000.0));

  // A held live Keys note (the drawn clip muted) retunes too: the new route image moves it to its key's new pitch.
  a.link->setLivePorts({"Port"}, {2.0f});
  const Track* t = findTrackPtr(*a.ctx->doc(), a.track);
  REQUIRE(a.ctx->store().dispatch(SetClipActive{ClipRef{a.track, t->clips.front().id}, false}).ok());
  constexpr uint8_t kKey = 64;
  engine::RawMidi on;
  on.port = 0;
  on.status = 0x90;
  on.d1 = kKey;
  on.d2 = 100;
  REQUIRE(a.link->livePorts()->push(0, on));
  (void)a.run(0.1);
  const uint64_t retuned = a.link->engine().liveInput().stats().retuned;
  a.d.move(c);
  a.d.down(c);
  const uint32_t from = n;
  auto map0 = buildInputPitchMap(*a.ctx->doc(), a.track);
  REQUIRE(map0.ok());
  double prev = (*map0)->at(kKey).hz;
  uint64_t moved = 0;  // steps that give the key another pitch
  for (int i = 1; i <= 30 && n < from + 3; ++i) {
    a.d.move({c.x, c.y - 1.0f * static_cast<float>(i)});
    const uint32_t now = a.ctx->doc()->tunings->front()->def.edo()->n();
    if (now == n) continue;
    n = now;
    auto map = buildInputPitchMap(*a.ctx->doc(), a.track);
    REQUIRE(map.ok());
    const double want = (*map)->at(kKey).hz;
    moved += want != prev ? 1u : 0u;
    prev = want;
    CHECK(std::fabs(cents(hzOfTail(a.run(0.12), 0.08), want)) < 0.05);
  }
  a.d.up({c.x, c.y - 30.0f});
  CHECK(n >= from + 3);
  CHECK(moved >= 2u);
  CHECK(a.link->engine().liveInput().stats().retuned == retuned + moved);
}


TEST_CASE("tuning live: a held Hz-input live note keeps its Hz through a Generator drag (M6 review L3)") {
  LiveApp a(mosDef(700.0));
  a.link->setLivePorts({"Port"}, {2.0f});
  REQUIRE(a.ctx->store().dispatch(SetNoteInput{a.track, NoteInput{NoteInputMode::Hz, InputKeyMap::Tuning}}).ok());
  const Track* t = findTrackPtr(*a.ctx->doc(), a.track);
  REQUIRE(a.ctx->store().dispatch(SetClipActive{ClipRef{a.track, t->clips.front().id}, false}).ok());
  constexpr uint8_t kKey = 64;
  auto map = buildInputPitchMap(*a.ctx->doc(), a.track);
  REQUIRE(map.ok());
  REQUIRE((*map)->at(kKey).mapped);
  const double keyHz = (*map)->at(kKey).hz;
  engine::RawMidi on;
  on.port = 0;
  on.status = 0x90;
  on.d1 = kKey;
  on.d2 = 100;
  REQUIRE(a.link->livePorts()->push(0, on));
  (void)a.run(0.1);
  CHECK(std::fabs(cents(hzOfTail(a.run(0.12), 0.08), keyHz)) < 0.05);
  const uint64_t retuned = a.link->engine().liveInput().stats().retuned;
  ui::DragBox& gen = a.card().row(ParamKind::MosGenerator)->box();
  const ui::Vec2 c = centre(gen.rect());
  a.d.move(c);
  a.d.down(c);
  for (int i = 1; i <= 4; ++i) {
    a.d.move({c.x, c.y - 10.0f * static_cast<float>(i)});  // +5 ¢ of generator per step: the key's map moves
    CHECK(std::fabs(cents(hzOfTail(a.run(0.12), 0.08), keyHz)) < 0.05);  // the note does not
  }
  a.d.up({c.x, c.y - 40.0f});
  CHECK(a.ctx->doc()->tunings->front()->def.mos()->generator > 715.0);
  auto moved = buildInputPitchMap(*a.ctx->doc(), a.track);
  REQUIRE(moved.ok());
  CHECK(std::fabs(cents((*moved)->at(kKey).hz, keyHz)) > 1.0);  // a new note-on would sound elsewhere
  CHECK(a.link->engine().liveInput().stats().retuned == retuned);
}

TEST_CASE("tuning live: a MOS size + generator change sounds the old or the new pitch while it compiles, never a third "
          "(M6 review L4)") {
  LiveApp a(mosDef(700.0));  // 5L 2s at 700 ¢, the held drawn note on degree 4
  (void)a.run(0.2);
  const double oldHz = a.expectedHz();
  CHECK(std::fabs(cents(hzOfTail(a.run(0.12), 0.08), oldHz)) < 0.05);
  // 7@700 -> 5@710 in one command (the wheel's click on another region): a new table, so a compile.
  TuningDef next = a.ctx->doc()->tunings->front()->def;
  tuning::MosParams m = *next.mos();
  m.n = 5;
  m.generator = 710.0;
  m.region.reset();
  next.system = m;
  REQUIRE(a.ctx->store().dispatch(SetTuningDef{a.tuning, next}).ok());
  const double newHz = a.expectedHz();
  REQUIRE(std::fabs(cents(newHz, oldHz)) > 5.0);
  // The audio thread plays on while the worker compiles. The drawn note's sequence swap is published at once; with
  // the playing image keeping its table AND its params it already sounds the new pitch (before: the old table at the
  // new generator, 10 ¢ off the new pitch here, up to 540 ¢ for other shapes). Either pitch is fine, a third is not.
  const double during = hzOfTail(a.run(0.15, false), 0.12);
  MESSAGE("while compiling: " << cents(during, oldHz) << " c from the old pitch, " << cents(during, newHz) << " c from the new");
  CHECK((std::fabs(cents(during, oldHz)) < 0.05 || std::fabs(cents(during, newHz)) < 0.05));
  // Published: straight to the new pitch.
  CHECK(std::fabs(cents(hzOfTail(a.run(0.12), 0.08), newHz)) < 0.05);
}
