// VZ.5: the stereo displays — the feed's cross-spectrum stage (kWantStereo alone: no spectrum; a panned sine's pan, the
// mono loss of antiphase; nothing while hidden; rests), the three faces (held while painted, the requests their options
// make, what they read: the Stereo Field's correlation and bands from the raw history, the Stereo Image's pan, the Phase
// Scope's eased mono curve; let go when hidden; every option paints) and the scene device-meta-stereo.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../../src/views/src/meta/faces.h"  // VZ.5: the stereo faces' state (tests)
#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/meta/stereo.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/image.h"
#include "pq/ui/theme.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/view_scenes.h"

using namespace pq;
using namespace pq::views;
using namespace pq::views::devices;
using namespace pq::ui;

namespace {

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

void checkScene(const std::string& name) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(name, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(name, {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  auto ref = readPng(std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + name + ".png");
  if (!ref) {
    (void)writePng(name + ".actual.png", *img);
    FAIL_CHECK("no golden " << name << ": wrote " << name << ".actual.png");
    return;
  }
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(name, *img, *ref, true, &d));
  MESSAGE(name << ": " << pq::ui::test::describe(d));
}

// The test signal: a 1 kHz sine panned to `pan` (power balance), or in antiphase, or silence.
struct Signal {
  double pan = 0.0;
  bool anti = false, on = true;
  void at(uint64_t i, float& l, float& r) const {
    const double v = on ? 0.4 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kSr) : 0.0;
    l = static_cast<float>(v * std::sqrt((1.0 - pan) / 2.0) * (anti ? std::sqrt(2.0) : 1.0));
    r = anti ? -l : static_cast<float>(v * std::sqrt((1.0 + pan) / 2.0));
  }
};

void stream(TapRing& tap, SpectrumFeed& feed, double seconds, const Signal& s, uint64_t& at, double& clock) {
  const auto n = static_cast<uint32_t>(seconds * kSr);
  if (n == 0) feed.step(clock += 0.001);
  std::vector<float> l(4096), r(4096);
  for (uint32_t i = 0; i < n; i += 4096) {
    const uint32_t k = std::min<uint32_t>(4096, n - i);
    for (uint32_t j = 0; j < k; ++j) s.at(at + j, l[j], r[j]);
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
  void end() override { gesture_ = false; }
  void cancel() override { gesture_ = false; }
  void reset(size_t) override {}
  meta::DataPtr data() const override { return data_ ? data_ : MetaTarget::data(); }
  void setData(meta::DataPtr d) override { data_ = std::move(d); }
  meta::DataPtr data_;

private:
  const meta::Type* type_;
  bool gesture_ = false;
};

struct FaceRig {
  TapRing tap{4, 32768, 4096};
  SpectrumFeed feed{tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{}};
  std::unique_ptr<MetaFace> face;
  std::unique_ptr<Theme> theme;
  DrawList dl;
  uint64_t at = 0;
  double clock = 1.0;
  float scale = 2.0f;
  explicit FaceRig(const meta::Type& t) : face(makeFace(t)) {
    auto th = loadTheme(defaultThemePath());
    REQUIRE(th.ok());
    theme = std::make_unique<Theme>(*th);
    MetaFace::setClock(+[]() -> double { return Clock::now(); });
    REQUIRE(face != nullptr);
    face->bind(std::make_unique<DataTarget>(t), nullptr);
    face->setAnalysis([this] { return &feed; }, [] {});
  }
  ~FaceRig() { MetaFace::setClock(nullptr); }
  void paint() {
    dl.reset(1000, 600);
    FaceEnv env;
    env.theme = theme.get();
    env.uiScale = 1.0f;
    FacePainter p(dl, {{10.0f, 10.0f}, scale});
    face->paintLive(p, env);
    // VZ.9: nothing is drawn outside the face (a label past an edge, a chip, a summary): every triangle not clipped
    // to the face lies within it, give or take the antialiasing fringe. (Not with an in-face sheet open: the
    // sheet is wider than a narrow face by design.)
    if (!boundsChecked) return;
    const Vec2 sz = face->worldSize();
    const float s = scale, x1 = 10.0f + sz.x * s, y1 = 10.0f + sz.y * s;
    size_t outside = 0;
    const auto vtx = dl.vertices();
    const auto idx = dl.indices();
    for (const DrawCmd& c : dl.commands()) {
      if (c.clip.x0 >= 7 && c.clip.y0 >= 7 && static_cast<float>(c.clip.x1) <= x1 + 3.0f && static_cast<float>(c.clip.y1) <= y1 + 3.0f)
        continue;  // clipped to the face
      for (uint32_t k = c.idxOffset; k < c.idxOffset + c.idxCount; ++k) {
        const Vec2 q = vtx[idx[k]].pos;
        if (q.x < 7.0f || q.y < 7.0f || q.x > x1 + 3.0f || q.y > y1 + 3.0f) {
          if (outside++ == 0) MESSAGE("outside at " << q.x << ", " << q.y << " (the face to " << x1 << ", " << y1 << ")");
        }
      }
    }
    CHECK(outside == 0);
  }
  bool boundsChecked = true;
  StereoShown play(double seconds, const Signal& s = {}) {
    stream(tap, feed, seconds, s, at, clock);
    Clock::now() += 1.0 / 60.0;
    (void)face->pollVoices();
    paint();
    StereoShown out;
    REQUIRE(stereoShown(*face, out));
    return out;
  }
};

const dsp::StereoPoint& near(const std::vector<dsp::StereoPoint>& pts, float hz) {
  REQUIRE_FALSE(pts.empty());
  const dsp::StereoPoint* best = &pts[0];
  for (const auto& p : pts)
    if (std::fabs(std::log(p.hz / hz)) < std::fabs(std::log(best->hz / hz))) best = &p;
  return *best;
}

const meta::Type* const kTypes[] = {&meta::stereoFieldType(), &meta::stereoImageType(), &meta::phaseScopeType()};

}  // namespace

TEST_CASE("stereo feed: kWantStereo alone — no spectrum; a panned sine's pan; nothing while hidden; rests on silence") {
  TapRing tap(4, 32768, 4096);
  SpectrumFeed feed(tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{});
  feed.setStereo(dsp::CrossSpectrumConfig{});
  feed.acquire(kWantStereo);
  uint64_t at = 0;
  double clock = 1.0;
  stream(tap, feed, 2.0, {.pan = 0.5}, at, clock);
  CHECK(feed.hopsAnalysed() == 0);
  StereoSnapshot s;
  bool settled = true;
  uint64_t seq = feed.stereoRead(0, s, settled);
  CHECK_FALSE(settled);
  REQUIRE(s.points.size() == 256);
  CHECK(std::fabs(near(s.points, 1000.0f).pos - 0.5f) <= 0.02f);
  CHECK(s.heat.empty());
  // Hidden: nothing computed.
  feed.release(kWantStereo);
  stream(tap, feed, 1.0, {}, at, clock);
  CHECK(feed.stereoRead(seq, s, settled) == seq);
  // Silence: it rests (every level under the floor), then silent data computes nothing.
  feed.acquire(kWantStereo);
  stream(tap, feed, 8.0, {.on = false}, at, clock);
  seq = feed.stereoRead(seq, s, settled);
  CHECK(settled);
  stream(tap, feed, 1.0, {.on = false}, at, clock);
  CHECK(feed.stereoRead(seq, s, settled) == seq);
  feed.release(kWantStereo);
}

TEST_CASE("stereo faces: held while painted, the request their options make, what they read; let go when hidden") {
  for (const meta::Type* t : kTypes) {
    CAPTURE(t->id);
    FaceRig rig(*t);
    rig.paint();
    CHECK(rig.feed.consumers() == 1);
    StereoShown s = rig.play(2.0, {.anti = true});
    CHECK(s.acquired);
    if (t == &meta::stereoFieldType()) {
      CHECK(s.correlation == doctest::Approx(-1.0).epsilon(1e-3));
      CHECK(s.tracePoints > 1000);  // 60 ms
      REQUIRE(stereoTestControl(*rig.face, "bands", 1.0));
      s = rig.play(1.0);  // in phase now: every band correlates (the 1 kHz tone sits in Mid)
      CHECK(s.bandCorrelation[1] == doctest::Approx(1.0).epsilon(1e-3));
    } else if (t == &meta::stereoImageType()) {
      CHECK(s.config.fftSize == 4096);
      CHECK(s.config.parts == 0u);
      CHECK(near(s.points, 1000.0f).coherence > 0.99f);
      s = rig.play(2.0, {.pan = -0.7});
      CHECK(std::fabs(near(s.points, 1000.0f).pos + 0.7f) <= 0.02f);
      REQUIRE(stereoTestControl(*rig.face, "mode", 2.0));  // heat map: the tracker
      s = rig.play(1.0, {.pan = -0.7});
      CHECK((s.config.parts & dsp::kStereoParticles) != 0);
      CHECK(s.particles >= 1);
      REQUIRE(stereoTestControl(*rig.face, "mode", 1.0));  // glow: the grid
      s = rig.play(0.5, {.pan = -0.7});
      CHECK(s.heat);
    } else {
      CHECK(s.config.fftSize == 8192);
      CHECK(s.config.window == dsp::SpecWindow::Hann);
      CHECK(near(s.points, 1000.0f).monoDb < -60.0f);  // antiphase: gone in mono
      // The drawn curve eases toward the measurement (45 ms): a few frames later it is there.
      for (int k = 0; k < 30; ++k) {
        Clock::now() += 1.0 / 60.0;
        (void)rig.face->pollVoices();
      }
      REQUIRE(stereoShown(*rig.face, s));
      REQUIRE(s.mono.size() == s.points.size());
      CHECK(s.mono[256] == doctest::Approx(s.points[256].monoDb).epsilon(0.01));
      REQUIRE(stereoTestControl(*rig.face, "fft", 11.0));
      REQUIRE(stereoShown(*rig.face, s));
      CHECK(s.config.fftSize == 2048);
      CHECK(s.config.hop == 512);
    }
    Clock::now() += 1.0;
    (void)rig.face->pollVoices();
    Clock::now() += 0.1;
    (void)rig.face->pollVoices();
    REQUIRE(stereoShown(*rig.face, s));
    CHECK_FALSE(s.acquired);
    CHECK(rig.feed.consumers() == 0);
  }
}

TEST_CASE("stereo faces: every option and state paints") {
  for (const meta::Type* t : kTypes) {
    CAPTURE(t->id);
    FaceRig rig(*t);
    rig.paint();
    (void)rig.play(1.5, {.pan = 0.3});
    std::vector<std::pair<std::string, double>> opts;
    if (t == &meta::stereoFieldType())
      opts = {{"bands", 1}, {"draw", 1}, {"uni", 1}, {"scaled", 0}, {"auto", 0}, {"bar", 0}, {"grid", 0}};
    else if (t == &meta::stereoImageType())
      opts = {{"col", 0}, {"col", 2}, {"map", 10}, {"col", 3}, {"col", 1}, {"pal", 4}, {"mode", 1}, {"mode", 2}, {"wglow", 0},
              {"ag", 1}, {"mode", 0}, {"sy", 0.5}, {"zoom", 2}, {"scale", 1}, {"scale", 2}, {"note", 0}};
    else
      opts = {{"coh", 1}, {"range", 40}, {"warn", -12}, {"scale", 2}, {"smooth", 0}, {"win", 1}, {"grid", 0}};
    for (const auto& [k, v] : opts) {
      REQUIRE(stereoTestControl(*rig.face, k, v));
      (void)rig.play(0.2, {.pan = 0.3});
    }
    REQUIRE(stereoTestControl(*rig.face, "hoverx", 100.0));
    rig.paint();
    REQUIRE(stereoTestControl(*rig.face, "sheet", 1.0));
    rig.boundsChecked = false;
    rig.paint();
    rig.scale = 0.4f;
    rig.paint();
    REQUIRE(stereoTestControl(*rig.face, "freeze", 1.0));
    StereoShown s;
    REQUIRE(stereoShown(*rig.face, s));
    CHECK(s.frozen);
  }
}

// VZ.10: the colour options reach the paint (each chosen colour's RGB in the draw list; −1: gone again).
TEST_CASE("stereo faces (VZ.10): a chosen colour is painted; −1 is the face's own") {
  constexpr double kPick = 0x0A141E;  // rgb(10, 20, 30): nothing else paints it
  auto painted = [](const DrawList& dl) {
    for (const Vertex& v : dl.vertices())
      if (ui::redOf(v.col) == 10 && ui::greenOf(v.col) == 20 && ui::blueOf(v.col) == 30 && ui::alphaOf(v.col) > 0) return true;
    return false;
  };
  auto check = [&](FaceRig& rig, auto control, const std::string& key, const std::vector<std::pair<std::string, double>>& needs) {
    CAPTURE(key);
    for (const auto& [k, v] : needs) REQUIRE(control(*rig.face, k, v));
    if (!needs.empty()) (void)rig.play(0.5);  // (what the option shows, read)
    rig.paint();
    CHECK_FALSE(painted(rig.dl));
    REQUIRE(control(*rig.face, key, kPick));
    rig.paint();
    CHECK(painted(rig.dl));
    REQUIRE(control(*rig.face, key, -1.0));
    rig.paint();
    CHECK_FALSE(painted(rig.dl));
  };
  auto control = [](MetaFace& f, std::string_view k, double v) { return stereoTestControl(f, k, v); };
  {
    FaceRig rig(meta::stereoFieldType());
    rig.paint();
    (void)rig.play(1.0);
    check(rig, control, "ctrace", {});
    check(rig, control, "cpos", {});
    check(rig, control, "clow", {{"bands", 1}});
  }
  {
    FaceRig rig(meta::stereoImageType());
    rig.paint();
    (void)rig.play(1.0);
    check(rig, control, "cline", {});
  }
  {
    FaceRig rig(meta::phaseScopeType());
    rig.paint();
    (void)rig.play(1.0);
    check(rig, control, "cmono", {});
  }
}

TEST_CASE("render: device-meta-stereo matches the golden image") { checkScene("device-meta-stereo"); }
