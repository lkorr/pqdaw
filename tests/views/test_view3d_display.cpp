// VZ.7: the 3D displays — the camera's projection at the named views (known box points land where expected: the
// Waterfall's SPEC / SIDE / TOP, the Stereo Spectrogram canvas's SPEC / IMAGE / PAN, the 3D view's centre, perspective),
// its drags and snaps; the feed's waterfall and cloud stages (no spectrum; nothing while hidden; rest); the faces (held
// while painted, the requests their options make, the pose as data — one step per gesture, a double-click snaps —, idle
// asks for no frames, the canvas re-uploads only lit rows); goldens of both faces at each named view.

#include <doctest/doctest.h>

#include <new>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../../src/views/src/meta/camera3d.h"
#include "../../src/views/src/meta/faces.h"
#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/meta/view3d.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/theme.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/fake_transport.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/panel/element.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/view_context.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::devices;

namespace {

constexpr double kSr = 48000.0, kPi = 3.14159265358979323846;

bool near2(Vec2 a, Vec2 b, float tol = 1e-3f) { return std::fabs(a.x - b.x) <= tol && std::fabs(a.y - b.y) <= tol; }

// A short mix: a centred kick and bass, a lead left, a pad wide (each side detuned its own way), hats wide.
void mix(uint64_t i, float& l, float& r) {
  const double t = static_cast<double>(i) / kSr, tau = 2.0 * kPi;
  const double beat = std::fmod(t, 0.5);
  const double kick = 0.5 * std::exp(-beat / 0.12) * std::sin(tau * (48.0 * beat + 2.1 * (1.0 - std::exp(-beat / 0.03))));
  double lead = 0.0;
  for (int k = 1; k <= 5; ++k) lead += std::sin(tau * 660.0 * k * t * (1.0 + 0.02 * std::sin(tau * 0.5 * t))) / k;
  const double padL = std::sin(tau * 330.0 * 1.004 * t) + std::sin(tau * 554.37 * 1.004 * t);
  const double padR = std::sin(tau * 330.0 * 0.996 * t) + std::sin(tau * 554.37 * 0.996 * t);
  const auto seed = static_cast<uint32_t>(i * 2654435761u);
  const double hat = std::exp(-std::fmod(t + 0.25, 0.5) / 0.03) * ((seed >> 8) / 8388608.0 - 1.0);
  l = static_cast<float>(kick + 0.12 * std::sin(tau * 55.0 * t) + 0.08 * lead + 0.05 * padL + 0.05 * hat);
  r = static_cast<float>(kick + 0.12 * std::sin(tau * 55.0 * t) + 0.02 * lead + 0.05 * padR - 0.04 * hat);
}

void stream(TapRing& tap, SpectrumFeed& feed, double seconds, uint64_t& at, double& clock, bool silent = false) {
  const auto n = static_cast<uint32_t>(seconds * kSr);
  std::vector<float> l(4096), r(4096);
  for (uint32_t i = 0; i < n; i += 4096) {
    const uint32_t k = std::min<uint32_t>(4096, n - i);
    for (uint32_t j = 0; j < k; ++j) {
      if (silent) l[j] = r[j] = 0.0f;
      else mix(at + j, l[j], r[j]);
    }
    const float* ch[4] = {l.data(), r.data(), l.data(), r.data()};
    tap.write(ch, 4, k);
    at += k;
    feed.step(clock += static_cast<double>(k) / kSr);
  }
}

struct Clock {
  static double& now() {
    static double c = 10.0;
    return c;
  }
};

class DataTarget final : public MetaTarget {
public:
  explicit DataTarget(const meta::Type& t) : type_(&t) {}
  const meta::Type* targetType() const noexcept override { return type_; }
  double value(size_t) const override { return 0.0; }
  void begin(const char*) override { gesture_ = true; }
  void set(size_t, double) override {}
  void end() override {
    gesture_ = false;
    ++steps;
  }
  void cancel() override { gesture_ = false; }
  void reset(size_t) override {}
  meta::DataPtr data() const override { return data_ ? data_ : MetaTarget::data(); }
  void setData(meta::DataPtr d) override {
    data_ = std::move(d);
    if (!gesture_) ++steps;
  }
  meta::DataPtr data_;
  int steps = 0;

private:
  const meta::Type* type_;
  bool gesture_ = false;
};

struct FaceRig {
  TapRing tap{4, 32768, 4096};
  SpectrumFeed feed{tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{}};
  std::unique_ptr<MetaFace> face;
  DataTarget* target = nullptr;
  std::unique_ptr<Theme> theme;
  DrawList dl;
  uint64_t at = 0;
  double clock = 1.0;
  explicit FaceRig(const meta::Type& t) : face(makeFace(t)) {
    auto th = loadTheme(defaultThemePath());
    REQUIRE(th.ok());
    theme = std::make_unique<Theme>(*th);
    MetaFace::setClock(+[]() -> double { return Clock::now(); });
    REQUIRE(face != nullptr);
    auto tg = std::make_unique<DataTarget>(t);
    target = tg.get();
    face->bind(std::move(tg), nullptr);
    face->setAnalysis([this] { return &feed; }, [] {});
  }
  ~FaceRig() { MetaFace::setClock(nullptr); }
  void paint() {
    dl.reset(1200, 800);
    FaceEnv env;
    env.theme = theme.get();
    env.uiScale = 1.0f;
    FacePainter p(dl, {{10.0f, 10.0f}, 2.0f});
    face->paintLive(p, env);
  }
  void play(double seconds, bool silent = false) {
    stream(tap, feed, seconds, at, clock, silent);
    Clock::now() += 1.0 / 60.0;
    (void)face->pollVoices();
    paint();
  }
  FacePointer ptr(FacePointer::Kind k, Vec2 local, uint8_t clicks = 1, ui::Mods mods = ui::mod::kNone) const {
    FacePointer e;
    e.kind = k;
    e.local = local;
    e.device = {10.0f + 2.0f * local.x, 10.0f + 2.0f * local.y};
    e.pxPerUnit = 2.0f;
    e.clicks = clicks;
    e.mods = mods;
    return e;
  }
};

}  // namespace

TEST_CASE("3D camera: known box points land where expected at the named views; perspective enlarges what is near") {
  const Rect plot{0.0f, 0.0f, 200.0f, 100.0f};
  using cam3d::Pose;
  using cam3d::Projector;
  // The Waterfall's box (F, H, T). SPEC (yaw 0): frequency across, level up, time depth (no effect on screen).
  {
    const Projector p(Pose{0, 0, 0, 1, 0, 0}, false, 0.0f, plot);
    CHECK(near2(p(1.0f, 0.0f, 0.3f), {200.0f, 50.0f}));
    CHECK(near2(p(-1.0f, 1.0f, -0.7f), {0.0f, 0.0f}));
    CHECK(near2(p(0.0f, -1.0f, 1.0f), {100.0f, 100.0f}));
  }
  // SIDE (yaw 90): time across (newest right), level up, frequency depth.
  {
    const Projector p(Pose{90, 0, 0, 1, 0, 0}, false, 0.0f, plot);
    CHECK(near2(p(0.6f, 0.0f, 1.0f), {200.0f, 50.0f}));
    CHECK(near2(p(-0.6f, 1.0f, -1.0f), {0.0f, 0.0f}));
  }
  // TOP (pitch 90): frequency across, time up the screen — the newest at the bottom, level depth.
  {
    const Projector p(Pose{0, 90, 0, 1, 0, 0}, false, 0.0f, plot);
    CHECK(near2(p(1.0f, 0.5f, 1.0f), {200.0f, 100.0f}, 1e-2f));
    CHECK(near2(p(-1.0f, -0.5f, -1.0f), {0.0f, 0.0f}, 1e-2f));
  }
  // 3D (45, 15): the box's centre at the plot's centre; autofit keeps every corner on the plot.
  {
    const Projector p(Pose{45, 15, 0, 1, 0, 0}, false, 0.0f, plot);
    CHECK(near2(p(0, 0, 0), {100.0f, 50.0f}));
    for (int i = 0; i < 8; ++i) {
      const Vec2 c = p((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
      CHECK(c.x >= -1e-3f);
      CHECK(c.x <= 200.001f);
      CHECK(c.y >= -1e-3f);
      CHECK(c.y <= 100.001f);
    }
    // Perspective: the near face's corner moves out, the far face's in.
    const Projector q(Pose{0, 0, 0, 1, 0, 0}, false, 1.0f, plot);
    CHECK(q(1.0f, 0.0f, 1.0f).x > 200.0f);  // T = +1 is toward the viewer at yaw 0
    CHECK(q(1.0f, 0.0f, -1.0f).x < 200.0f);
  }
  // The Stereo Spectrogram's canvas (T, F, P): SPEC — the newest at the right, frequency up; IMAGE (yaw 90) — pan
  // across; PAN (pitch 90) — pan down the screen (right at the bottom).
  CHECK(near2(stereoSpectrogramCanvasAt(0, 0, 0.0f, 1.0f, 0.3f, 400, 300), {400.0f, 0.0f}));
  CHECK(near2(stereoSpectrogramCanvasAt(0, 0, 1.0f, 0.0f, -0.9f, 400, 300), {0.0f, 300.0f}));
  CHECK(near2(stereoSpectrogramCanvasAt(90, 0, 0.5f, 0.5f, 1.0f, 400, 300), {400.0f, 150.0f}));
  CHECK(near2(stereoSpectrogramCanvasAt(90, 0, 0.1f, 0.5f, -1.0f, 400, 300), {0.0f, 150.0f}));
  CHECK(near2(stereoSpectrogramCanvasAt(0, 90, 0.5f, 0.2f, 1.0f, 400, 300), {200.0f, 300.0f}, 1e-2f));
  CHECK(near2(stereoSpectrogramCanvasAt(0, 90, 0.5f, 0.9f, -1.0f, 400, 300), {200.0f, 0.0f}, 1e-2f));
}

TEST_CASE("3D camera: orbit drags, the release snap, the eased snap, the double-click's nearest view, the wheel") {
  cam3d::Orbit o;
  o.pose = {3.0f, 2.0f, 0.0f, 1.0f, 0.0f, 0.0f};
  o.begin(cam3d::Orbit::Drag::Orbit, {100.0f, 100.0f});
  o.move({105.0f, 100.0f}, 200.0f, 100.0f);  // +2° yaw
  CHECK(o.pose.yaw == doctest::Approx(5.0f - 0.0f).epsilon(1e-4));
  o.end();  // near flat, |yaw| 5: not < 5 → no snap
  CHECK_FALSE(o.snapping());
  o.begin(cam3d::Orbit::Drag::Orbit, {0.0f, 0.0f});
  o.move({-5.0f, 0.0f}, 200.0f, 100.0f);  // yaw 3
  o.end();
  CHECK(o.snapping());  // releases into SPEC
  int frames = 0;
  while (o.tick(1.0f / 60.0f)) ++frames;
  CHECK(o.pose == cam3d::Pose{0, 0, 0, 1, 0, 0});
  CHECK(frames < 40);
  const cam3d::NamedView views[4] = {{"A", 0, 0}, {"B", 90, 0}, {"C", 0, 90}, {"D", 45, 15}};
  o.pose = {80, 10, 0, 1, 0, 0};
  o.snapNearest(views, 4);
  while (o.tick(0.1f)) {
  }
  CHECK(o.pose.yaw == 90.0f);
  o.snapNearest(views, 4);  // from a named view: the 3D view
  while (o.tick(0.1f)) {
  }
  CHECK(o.pose.yaw == 45.0f);
  CHECK(o.pose.pitch == 15.0f);
  o.wheel(2.0f);
  CHECK(o.pose.zoom == doctest::Approx(std::exp(0.24f)));
}

TEST_CASE("3D feeds: the waterfall and the cloud alone — no spectrum; nothing while hidden; they rest on silence") {
  TapRing tap(4, 32768, 4096);
  SpectrumFeed feed(tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{});
  feed.setWaterfall(dsp::WaterfallConfig{});
  feed.setCloud(dsp::StereoCloudConfig{});
  feed.acquire(kWantWaterfall | kWantCloud);
  uint64_t at = 0;
  double clock = 1.0;
  stream(tap, feed, 2.0, at, clock);
  CHECK(feed.hopsAnalysed() == 0);
  WaterfallState ws;
  std::vector<float> live;
  bool settled = true;
  uint64_t wseq = feed.waterfallRead(0, ws, live, settled);
  CHECK(ws.slices > 50);
  CHECK(live.size() == dsp::WaterfallAnalyzer::kPoints);
  std::vector<float> sl(dsp::WaterfallAnalyzer::kPoints * 4);
  CHECK(feed.waterfallSlices(ws.slices - 4, 4, sl.data()) == 4);
  CloudState cs;
  uint64_t cseq = feed.cloudRead(0, cs, settled);
  CHECK(cs.points > 1000);
  CHECK(cs.slices == static_cast<uint64_t>(2.0 * kSr / 256));
  std::vector<dsp::CloudPoint> pts(16);
  CHECK(feed.cloudPoints(cs.points - 16, 16, pts.data()) == 16);
  feed.release(kWantWaterfall | kWantCloud);
  stream(tap, feed, 1.0, at, clock);
  CHECK(feed.waterfallRead(wseq, ws, live, settled) == wseq);
  CHECK(feed.cloudRead(cseq, cs, settled) == cseq);
  feed.acquire(kWantWaterfall | kWantCloud);
  stream(tap, feed, 8.0, at, clock, true);
  wseq = feed.waterfallRead(wseq, ws, live, settled);
  CHECK(settled);
  stream(tap, feed, 1.0, at, clock, true);
  CHECK(feed.waterfallRead(wseq, ws, live, settled) == wseq);  // at rest: silent data computes nothing
  feed.release(kWantWaterfall | kWantCloud);
}

TEST_CASE("3D faces: held while painted; the request; the pose as data (one step a gesture, double-click snaps); idle") {
  for (const meta::Type* t : {&meta::waterfallType(), &meta::stereoSpectrogramType()}) {
    CAPTURE(t->id);
    const bool wf = t == &meta::waterfallType();
    FaceRig rig(*t);
    rig.paint();
    CHECK(rig.feed.consumers() == 1);
    rig.play(2.0);
    if (wf) {
      WaterfallShown s;
      REQUIRE(waterfallShown(*rig.face, s));
      CHECK(s.acquired);
      CHECK(s.slices > 50);
      CHECK(s.vertices > 10000);  // the terrain's strips
      CHECK(s.config.fftSize == 2048);
      REQUIRE(waterfallTestControl(*rig.face, "chan", 4.0));
      REQUIRE(waterfallShown(*rig.face, s));
      CHECK(s.config.channel == dsp::WaterfallChannel::Side);
    } else {
      StereoSpectrogramShown s;
      REQUIRE(stereoSpectrogramShown(*rig.face, s));
      CHECK(s.acquired);
      CHECK(s.cloud > 1000);
      CHECK(s.ribbon > 50);
      CHECK(s.config.fftSize == 2048);
      CHECK(s.config.loFactor == 2);
    }
    // A drag orbits: one undo step, the pose in the data.
    const int steps0 = rig.target->steps;
    const Vec2 c{150.0f, 100.0f};
    REQUIRE(rig.face->onPointer(rig.ptr(FacePointer::Kind::Down, c)));
    REQUIRE(rig.face->onPointer(rig.ptr(FacePointer::Kind::Move, {c.x + 25.0f, c.y})));  // +50 px: +20°
    REQUIRE(rig.face->onPointer(rig.ptr(FacePointer::Kind::Up, {c.x + 25.0f, c.y})));
    CHECK(rig.target->steps == steps0 + 1);
    REQUIRE(rig.target->data_ != nullptr);
    CHECK(rig.target->data_->field("yaw")->number == doctest::Approx(65.0));
    // A double-click snaps to the nearest named view (3D from yaw 65, pitch 15 is nearest).
    REQUIRE(rig.face->onPointer(rig.ptr(FacePointer::Kind::Down, c, 2)));
    (void)rig.face->onPointer(rig.ptr(FacePointer::Kind::Up, c));
    CHECK(rig.target->data_->field("yaw") == nullptr);  // 45: the default again
    // A chip snaps: SIDE / IMAGE (yaw 90).
    REQUIRE(rig.face->onPointer(rig.ptr(FacePointer::Kind::Down, {wf ? 45.0f : 50.0f, 15.0f})));
    CHECK(rig.target->data_->field("yaw")->number == doctest::Approx(90.0));
    for (int k = 0; k < 60; ++k) {  // the snap eases in (painted: shown)
      Clock::now() += 1.0 / 60.0;
      (void)rig.face->pollVoices();
      rig.paint();
    }
    if (wf) {
      WaterfallShown s;
      REQUIRE(waterfallShown(*rig.face, s));
      CHECK(s.yaw == 90.0f);
      CHECK(s.pitch == 0.0f);
    } else {
      StereoSpectrogramShown s;
      REQUIRE(stereoSpectrogramShown(*rig.face, s));
      CHECK(s.yaw == 90.0f);
    }
    // Idle: silent past the span, the camera at rest — no frames asked.
    rig.play(9.0, true);
    for (int k = 0; k < 5; ++k) {
      Clock::now() += 1.0 / 60.0;
      (void)rig.face->pollVoices();
      rig.paint();
    }
    CHECK_FALSE(rig.face->animating());
  }
}

TEST_CASE("analyser faces: a new feed where the freed one was is another feed (serial, H1) — the face starts over on it") {
  // The hub frees a feed nothing holds when its tap or the rate changes; the next one can land at the same address. A
  // face compared addresses: it kept the old feed's read position and skipped the new one's slices until its count
  // passed the old (and held the old picture meanwhile).
  TapRing tap(4, 32768, 4096);
  alignas(SpectrumFeed) unsigned char storage[sizeof(SpectrumFeed)];
  auto* feed = new (storage) SpectrumFeed(tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{});
  auto theme = loadTheme(defaultThemePath());
  REQUIRE(theme.ok());
  MetaFace::setClock(+[]() -> double { return Clock::now(); });
  auto face = makeFace(meta::waterfallType());
  face->bind(std::make_unique<DataTarget>(meta::waterfallType()), nullptr);
  face->setAnalysis([&] { return feed; }, [] {});
  DrawList dl;
  auto paint = [&] {
    dl.reset(1200, 800);
    FaceEnv env;
    env.theme = &*theme;
    env.uiScale = 1.0f;
    FacePainter p(dl, {{10.0f, 10.0f}, 2.0f});
    face->paintLive(p, env);
  };
  auto frame = [&] {
    Clock::now() += 1.0 / 60.0;
    (void)face->pollVoices();
    paint();
  };
  uint64_t at = 0;
  double clock = 1.0;
  paint();
  stream(tap, *feed, 3.0, at, clock);
  frame();
  WaterfallShown s;
  REQUIRE(waterfallShown(*face, s));
  const size_t before = s.slices;
  CHECK(before > 100);
  // Hidden: it lets go; the feed is freed and another is made in its place.
  Clock::now() += 1.0;
  (void)face->pollVoices();
  Clock::now() += 0.1;
  (void)face->pollVoices();
  REQUIRE(waterfallShown(*face, s));
  REQUIRE_FALSE(s.acquired);
  REQUIRE(feed->consumers() == 0);
  feed->~SpectrumFeed();
  feed = new (storage) SpectrumFeed(tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{});
  paint();  // shown again: it holds the new feed
  stream(tap, *feed, 1.0, at, clock);
  frame();
  WaterfallState ws;
  std::vector<float> live;
  bool settled = false;
  (void)feed->waterfallRead(0, ws, live, settled);
  REQUIRE(waterfallShown(*face, s));
  CHECK(s.acquired);
  CHECK(ws.slices > 0);
  CHECK(s.slices == ws.slices);  // what the new feed has, not the old picture
  face.reset();
  feed->~SpectrumFeed();
  MetaFace::setClock(nullptr);
}

TEST_CASE("Stereo Spectrogram: the canvas scrolls (new points splatted once, leavers repaired, touched texels uploaded), exactly") {
  FaceRig rig(meta::stereoSpectrogramType());
  rig.paint();
  rig.play(1.0);
  REQUIRE(stereoSpectrogramTestControl(*rig.face, "zoom", 0.3));  // the picture shrinks to the middle third of the rows
  StereoSpectrogramShown a, b;
  // (the camera moved: rebuilds — at most ~30 a second while it moves, unscrolled — then the first update after it, to
  // scroll; each uploads the union of the old and the new lit rows)
  for (int k = 0; k < 4; ++k) rig.play(1.0 / 60.0);
  REQUIRE(stereoSpectrogramShown(*rig.face, a));
  REQUIRE(a.scrolling);
  rig.paint();  // nothing new: no update
  REQUIRE(stereoSpectrogramShown(*rig.face, b));
  CHECK(b.redraws == a.redraws);
  for (int k = 0; k < 12; ++k) rig.play(1.0 / 60.0);
  REQUIRE(stereoSpectrogramShown(*rig.face, b));
  CHECK(b.redraws > a.redraws);
  CHECK(b.rebuilds == a.rebuilds);  // new data scrolls, never re-splats
  const uint64_t rows = b.uploadedRows - a.uploadedRows, updates = b.redraws - a.redraws;
  MESSAGE(rows << " rows uploaded over " << updates << " updates of a canvas " << 2 * 184 << " rows tall");
  CHECK(rows < updates * 2 * 184 / 2);
  // Exact: with the span turning over every frame (leavers cleared and their tiles re-splatted), the canvas is what a
  // fresh splat of the live points gives — the default colours, then the age fade and the Time colours (a gain and a
  // reference time that move every texel).
  for (const auto& [k, v] : std::vector<std::pair<const char*, double>>{{"zoom", 1.0}, {"span", 2.0}, {"fade", 0.5}, {"src", 3}}) {
    CAPTURE(k);
    REQUIRE(stereoSpectrogramTestControl(*rig.face, k, v));
    for (int f = 0; f < 4; ++f) rig.play(1.0 / 60.0);  // (after a camera move the first update re-splats to scroll)
    REQUIRE(stereoSpectrogramShown(*rig.face, a));
    REQUIRE(a.scrolling);
    for (int f = 0; f < 150; ++f) rig.play(1.0 / 60.0);  // 2.5 s: past the span
    REQUIRE(stereoSpectrogramShown(*rig.face, b));
    CHECK(b.rebuilds == a.rebuilds);
    CHECK(stereoSpectrogramCanvasCheck(*rig.face) == 0);
  }
  // A camera move rebuilds; perspective and glow rebuild every update (no scrolling).
  REQUIRE(stereoSpectrogramTestControl(*rig.face, "yaw", 30.0));
  rig.play(1.0 / 60.0);
  REQUIRE(stereoSpectrogramShown(*rig.face, b));
  CHECK(b.rebuilds == a.rebuilds + 1);
  CHECK_FALSE(b.scrolling);  // (a moving camera: unscrolled)
  rig.play(1.0 / 60.0);
  REQUIRE(stereoSpectrogramShown(*rig.face, b));
  CHECK(b.scrolling);
  REQUIRE(stereoSpectrogramTestControl(*rig.face, "persp", 0.5));
  rig.play(1.0 / 60.0);
  rig.play(1.0 / 60.0);
  REQUIRE(stereoSpectrogramShown(*rig.face, b));
  CHECK_FALSE(b.scrolling);
  REQUIRE(stereoSpectrogramTestControl(*rig.face, "persp", 0.0));
  // Every option paints (no crash), both modes.
  for (const auto& [k, v] : std::vector<std::pair<const char*, double>>{
           {"mode", 1}, {"sy", 0.6}, {"prec", 0.5}, {"gate", 20}, {"src", 4}, {"src", 2}, {"age", 0}, {"mode", 0}, {"glow", 0.8},
           {"detail", 1}, {"src", 3}, {"map", 10}, {"persp", 0.7}, {"free", 1}, {"view", 3}}) {
    REQUIRE(stereoSpectrogramTestControl(*rig.face, k, v));
    rig.play(0.1);
  }
  FaceRig w(meta::waterfallType());
  w.paint();
  for (const auto& [k, v] : std::vector<std::pair<const char*, double>>{
           {"solid", 0}, {"glow", 0}, {"col", 1}, {"col", 2}, {"col", 3}, {"col", 4}, {"col", 5}, {"map", 10}, {"col", 0},
           {"solid", 1}, {"persp", 1}, {"free", 1}, {"view", 1}, {"view", 2}, {"view", 0}, {"scale", 1}, {"sheet", 1}}) {
    REQUIRE(waterfallTestControl(*w.face, k, v));
    w.play(0.1);
  }
}

namespace {

const doc::DeviceLibrary& devLib() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

void checkImage(const std::string& name, const Image& img) {
  auto ref = readPng(std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + name + ".png");
  if (!ref) {
    (void)writePng(name + ".actual.png", img);
    FAIL_CHECK("no golden " << name << ": wrote " << name << ".actual.png");
    return;
  }
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(name, img, *ref, true, &d));
  MESSAGE(name << ": " << pq::ui::test::describe(d));
}

}  // namespace

TEST_CASE("render: the Waterfall and the Stereo Spectrogram at each named view (module panels over a held 3 s mix)") {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  doc::DocumentStore& st = ctx.store();
  auto run = [&](const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(st.dispatch(e->command).ok());
    return *e;
  };
  const doc::TrackId track = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, {}, "3D")).tracks.at(0);
  doc::EngineRoles roles[2];
  const doc::ModuleKind kinds[2] = {doc::ModuleKind::Waterfall, doc::ModuleKind::StereoSpectrogram};
  for (int i = 0; i < 2; ++i) {
    doc::IdSource local;
    doc::EngineRoles l;
    const doc::ops::Edit e = run(doc::ops::pasteBlocks(*st.current(), st.ids(), track, doc::engineModuleBlocks(local, kinds[i], {}, &l)));
    roles[i] = {e.devices.at(l.group.value - 1), e.devices.at(l.engine.value - 1)};
  }
  // The engines' feeds: the mix analysed now by manual feeds, then held.
  std::shared_ptr<TapRing> taps[2];
  std::shared_ptr<SpectrumFeed> feeds[2];
  for (int i = 0; i < 2; ++i) {
    taps[i] = std::make_shared<TapRing>(4, 32768, 4096);
    feeds[i] = std::make_shared<SpectrumFeed>(*taps[i], AnalysisHub::config(kSr), SpectrumFeed::Manual{});
    if (i == 0) feeds[i]->setWaterfall(waterfallRequest(nullptr));
    else feeds[i]->setCloud(stereoSpectrogramRequest(nullptr));
    feeds[i]->acquire(i == 0 ? kWantWaterfall : kWantCloud);
    uint64_t at = 0;
    double clock = 1.0;
    stream(*taps[i], *feeds[i], 3.0, at, clock);
    feeds[i]->hold();
  }
  const doc::DeviceId e0 = roles[0].engine, e1 = roles[1].engine;
  ctx.setAnalysisSource([e0, e1, taps](doc::DeviceId d) -> std::shared_ptr<TapRing> { return d == e0 ? taps[0] : d == e1 ? taps[1] : nullptr; }, kSr);
  REQUIRE(ctx.analysisHub() != nullptr);
  ctx.analysisHub()->adopt(e0, taps[0], feeds[0]);
  ctx.analysisHub()->adopt(e1, taps[1], feeds[1]);
  struct View {
    const char* name;
    double yaw, pitch;
  };
  const View wfViews[4] = {{"spec", 0, 0}, {"side", 90, 0}, {"top", 0, 90}, {"3d", 45, 15}};
  const View ssViews[4] = {{"spec", 0, 0}, {"image", 90, 0}, {"pan", 0, 90}, {"3d", 45, 15}};
  for (int i = 0; i < 2; ++i)
    for (const View& v : i == 0 ? wfViews : ssViews) {
      CAPTURE(v.name);
      auto data = std::make_shared<const meta::DataValue>(
          meta::DataValue::record({{"yaw", meta::DataValue::num(v.yaw)}, {"pitch", meta::DataValue::num(v.pitch)}}));
      run(doc::ops::setMetaData(*st.current(), roles[i].engine, "", i == 0 ? "waterfall" : "stereo_spectrogram", data));
      const doc::PanelPtr p = doc::findDevice(*ctx.doc(), roles[i].group).device->panel;
      REQUIRE(p != nullptr);
      TestDriver d(static_cast<uint32_t>(p->width * 2.0f), static_cast<uint32_t>(p->height * 2.0f), 2.0f);
      panel::PanelHost* host = nullptr;
      auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, roles[i].group),
                                                          panel::makeDocPanelTarget(ctx, roles[i].group, [&host]() -> Widget* { return host; }));
      host = &d.setRoot<panel::PanelHost>("panel", std::move(face));
      d.tree().layoutNow();
      REQUIRE(d.enableGpu().ok());
      d.advance(0.1);
      (void)d.render();  // (the canvas texture is uploaded during this frame; the next shows it)
      d.advance(0.1);
      auto img = d.render();
      REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
      checkImage(std::string(i == 0 ? "waterfall-" : "stereospectrogram-") + v.name, *img);
      d.tree().setRoot(std::make_unique<Widget>());
    }
}

TEST_CASE("3D faces: the UI frame per face at 1440p (a face ~1000 x 600 device px; perf)" * doctest::test_suite("perf")) {
  for (const meta::Type* t : {&meta::waterfallType(), &meta::stereoSpectrogramType()}) {
    FaceRig rig(*t);
    rig.paint();
    rig.play(4.0);
    const bool wf = t == &meta::waterfallType();
    // One frame: a 60th of a second of new data, the poll, the paint (`yaw`: the camera moving, as in a drag).
    auto frame = [&](double yaw) {
      if (yaw != 0.0) REQUIRE((wf ? waterfallTestControl(*rig.face, "yaw", yaw) : stereoSpectrogramTestControl(*rig.face, "yaw", yaw)));
      stream(rig.tap, rig.feed, 1.0 / 60.0, rig.at, rig.clock);
      Clock::now() += 1.0 / 60.0;
      const auto t0 = std::chrono::steady_clock::now();
      (void)rig.face->pollVoices();
      rig.dl.reset(3200, 2000);
      FaceEnv env;
      env.theme = rig.theme.get();
      env.uiScale = 1.0f;
      FacePainter p(rig.dl, {{10.0f, 10.0f}, 3.2f});
      rig.face->paintLive(p, env);
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    (void)frame(0.0);  // (the new size)
    constexpr int kFrames = 60;
    double steady = 0.0, drag = 0.0;
    for (int k = 0; k < kFrames; ++k) steady += frame(0.0);
    for (int k = 0; k < kFrames; ++k) drag += frame(45.0 + 0.5 * (k + 1));
    MESSAGE(std::string(wf ? "Waterfall" : "Stereo Spectrogram") << " (3D view), ms per UI frame at scale 3.2: " << steady / kFrames
                                                                 << " with data arriving, " << drag / kFrames << " while the camera moves");
  }
}
