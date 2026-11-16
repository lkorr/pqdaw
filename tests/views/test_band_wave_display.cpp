// VZ.6: Band Levels and Waveform displays — the feed's stages (kWantBands / kWantWave alone: no spectrum; nothing while
// hidden; settle at rest), the two faces (held while painted, their options as the instance's data and as the feed's
// request, the loop locked to the tap's beat, let go when hidden, every option paints), and the scenes
// device-meta-bandwave / project-meta-bandwave.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../../src/views/src/meta/faces.h"  // VZ.6: the faces' state (tests)
#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/meta/band_wave.h"
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

// The signal: a sine at hz and amp (0: silence) in L and R; the beat (120 bpm from `beat0`, NaN: no clock) in channel 0.
struct Signal {
  double hz = 100.0, amp = 0.5;
  double beat0 = 0.0;
  bool clock = true;
};
void stream(TapRing& tap, SpectrumFeed& feed, double seconds, const Signal& s, uint64_t& at, double& clock) {
  const auto n = static_cast<uint32_t>(seconds * kSr);
  if (n == 0) feed.step(clock += 0.001);
  std::vector<float> x(4096), b(4096), z(4096, 0.0f);
  for (uint32_t i = 0; i < n; i += 4096) {
    const uint32_t k = std::min<uint32_t>(4096, n - i);
    for (uint32_t j = 0; j < k; ++j) {
      const double t = static_cast<double>(at + j) / kSr;
      x[j] = static_cast<float>(s.amp * std::sin(2.0 * kPi * s.hz * t));
      b[j] = s.clock ? static_cast<float>(s.beat0 + t * 2.0) : NAN;
    }
    const float* ch[4] = {b.data(), z.data(), x.data(), x.data()};
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
  int steps = 0;
  meta::DataPtr data_;

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
    dl.reset(1000, 600);
    FaceEnv env;
    env.theme = theme.get();
    env.uiScale = 1.0f;
    FacePainter p(dl, {{10.0f, 10.0f}, 2.0f});
    face->paintLive(p, env);
    // VZ.9: nothing is drawn outside the face (a label past an edge, a chip, a summary): every triangle not clipped
    // to the face lies within it, give or take the antialiasing fringe. (Not with an in-face sheet open: the
    // sheet is wider than a narrow face by design.)
    if (!boundsChecked) return;
    const Vec2 sz = face->worldSize();
    const float s = 2.0f, x1 = 10.0f + sz.x * s, y1 = 10.0f + sz.y * s;
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
  BandWaveShown play(double seconds, const Signal& s = {}) {
    stream(tap, feed, seconds, s, at, clock);
    Clock::now() += 1.0 / 60.0;
    (void)face->pollVoices();
    paint();
    BandWaveShown out;
    REQUIRE(bandWaveShown(*face, out));
    return out;
  }
};

}  // namespace

TEST_CASE("band levels feed: kWantBands alone — no spectrum; a 100 Hz sine in the low band; nothing while hidden; rests") {
  TapRing tap(4, 32768, 4096);
  SpectrumFeed feed(tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{});
  dsp::BandLevelsConfig c;
  c.independent3 = false;
  feed.setBands(c);
  feed.acquire(kWantBands);
  uint64_t at = 0;
  double clock = 1.0;
  stream(tap, feed, 2.0, {.hz = 100.0, .amp = 1.0}, at, clock);
  CHECK(feed.hopsAnalysed() == 0);  // no spectrum for a bands-only consumer
  const dsp::BandLevels* b = feed.bands();
  REQUIRE(b != nullptr);
  CHECK(b->columns() >= 119);
  CHECK(b->level(0) == doctest::Approx(-3.01f).epsilon(0.1));
  CHECK(b->level(1) < -30.0f);
  CHECK_FALSE(feed.settled());
  // Hidden: nothing computed.
  feed.release(kWantBands);
  const uint64_t cols = b->columns();
  stream(tap, feed, 1.0, {}, at, clock);
  CHECK(b->columns() == cols);
  // Shown again, then silence: it settles once every band has fallen to the floor.
  feed.acquire(kWantBands);
  stream(tap, feed, 0.5, {}, at, clock);
  CHECK(b->columns() > cols);
  stream(tap, feed, 10.0, {.amp = 0.0}, at, clock);
  CHECK(b->atRest());
  CHECK(feed.settled());
  feed.release(kWantBands);
}

TEST_CASE("waveform feed: kWantWave alone — bins; the loop on the tap's beat; settles after a silent window") {
  TapRing tap(4, 32768, 4096);
  SpectrumFeed feed(tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{});
  dsp::WaveConfig c;
  c.loop = true;
  c.loopBeats = 4.0;
  feed.setWave(c, 2.0f);
  feed.acquire(kWantWave);
  uint64_t at = 0;
  double clock = 1.0;
  stream(tap, feed, 3.0, {.hz = 220.0, .amp = 0.5, .beat0 = 5.0}, at, clock);
  CHECK(feed.hopsAnalysed() == 0);
  const dsp::WaveCapture* w = feed.wave();
  REQUIRE(w != nullptr);
  CHECK(w->bins() == static_cast<uint64_t>(3.0 * kSr) / dsp::WaveCapture::kBinSize);
  std::vector<dsp::WaveBin> slots(dsp::WaveCapture::kLoopSlots);
  std::vector<uint8_t> filled(dsp::WaveCapture::kLoopSlots, 0);
  dsp::WaveLoopState st;
  (void)w->readLoop(0, slots.data(), filled.data(), st);
  CHECK(st.clocked);
  CHECK(st.bpm == doctest::Approx(120.0).epsilon(1e-3));
  // The playhead is the beat's phase in the bar: 5 + 3 s · 2 beats/s = beat 11 → 3 of 4.
  CHECK(st.phase == doctest::Approx(0.75).epsilon(2e-3));
  stream(tap, feed, 2.5, {.amp = 0.0}, at, clock);
  CHECK(feed.settled());
  feed.release(kWantWave);
}

TEST_CASE("band levels face: held while painted, its options are the feed's request, the levels shown, let go when hidden") {
  FaceRig r(meta::bandLevelsType());
  r.paint();
  BandWaveShown s = r.play(1.0, {.hz = 5000.0, .amp = 1.0});
  CHECK(s.acquired);
  CHECK(s.bands.bands == 3);
  CHECK(s.bands.independent3);  // PQ Meter's default
  CHECK(s.columns > 50);
  CHECK(s.levels[2] > s.levels[0] + 10.0f);  // 5 kHz: the high band
  // 5 bands on crossovers: one step of the instance's data, and the feed's request.
  REQUIRE(bandWaveTestControl(*r.face, "bands", 2.0));
  CHECK(r.target->steps == 1);
  s = r.play(1.0, {.hz = 30.0, .amp = 1.0});
  CHECK(s.bands.bands == 5);
  CHECK(r.feed.bands()->config().bands == 5);
  CHECK(s.levels[0] > s.levels[2] + 15.0f);  // 30 Hz: the sub band
  // Every option paints (a sheet open, hover, warp off, bands hidden).
  for (const char* key : {"warp", "grid", "labels", "s1", "s3"}) REQUIRE(bandWaveTestControl(*r.face, key, 0.0));
  REQUIRE(bandWaveTestControl(*r.face, "hover", 150.0));
  REQUIRE(bandWaveTestControl(*r.face, "sheet", 1.0));
  r.paint();
  // Hidden: one unanswered repaint, then it lets go.
  Clock::now() += 1.0;
  (void)r.face->pollVoices();
  Clock::now() += 0.1;
  (void)r.face->pollVoices();
  REQUIRE(bandWaveShown(*r.face, s));
  CHECK_FALSE(s.acquired);
}

TEST_CASE("waveform face: scroll columns hold the peak / RMS; Loop locks its playhead to the tap's beat; every option paints") {
  FaceRig r(meta::waveformType());
  r.paint();
  BandWaveShown s = r.play(4.0, {.hz = 220.0, .amp = 0.5});
  CHECK(s.acquired);
  CHECK_FALSE(s.wave.loop);
  CHECK(s.bins > 1400);
  // The newest columns of the mid lane: peak 0.5, RMS 0.354 (a 300-column panel over the 10 s window: 12+ bins each).
  std::vector<float> mn, mx, rms;
  REQUIRE(waveColumns(*r.face, 300, 2, mn, mx, rms));
  CHECK(mx[299] == doctest::Approx(0.5f).epsilon(0.01));
  CHECK(mn[299] == doctest::Approx(-0.5f).epsilon(0.01));
  CHECK(rms[299] == doctest::Approx(0.3536f).epsilon(0.02));
  CHECK(mx[0] == doctest::Approx(0.5f).epsilon(0.01));  // (4 s held of a 10 s window: the panel shows what there is, PQ's)
  REQUIRE(waveColumns(*r.face, 300, 3, mn, mx, rms));
  CHECK(mx[299] == 0.0f);  // L = R: nothing in the side lane
  // Loop over a bar: the playhead at the beat's phase.
  REQUIRE(bandWaveTestControl(*r.face, "ph", 1.0));
  s = r.play(1.0, {.hz = 220.0, .amp = 0.5, .beat0 = 2.0});
  CHECK(s.wave.loop);
  CHECK(s.loop.clocked);
  const double beatNow = 2.0 + static_cast<double>(r.at) / kSr * 2.0;
  const double d = std::fabs(s.loop.phase - std::fmod(beatNow, 4.0) / 4.0);
  CHECK(std::min(d, 1.0 - d) < 2e-3);  // (circular: the bar's end is its start)
  // No clock: the free run.
  s = r.play(0.5, {.clock = false});
  CHECK_FALSE(s.loop.clocked);
  for (const char* key : {"auto", "db", "clip", "swap"}) REQUIRE(bandWaveTestControl(*r.face, key, 1.0));
  for (double view : {1.0, 2.0, 3.0}) {
    REQUIRE(bandWaveTestControl(*r.face, "view", view));
    r.paint();
  }
  REQUIRE(bandWaveTestControl(*r.face, "col", 1.0));
  REQUIRE(bandWaveTestControl(*r.face, "hover", 150.0));
  REQUIRE(bandWaveTestControl(*r.face, "sheet", 1.0));
  r.paint();
  REQUIRE(bandWaveTestControl(*r.face, "ph", 0.0));
  r.paint();
}

TEST_CASE("band / wave: the worker's cost and the UI frame per face (5 bands; a 60 s waveform)" * doctest::test_suite("perf")) {
  FaceRig b(meta::bandLevelsType());
  REQUIRE(bandWaveTestControl(*b.face, "bands", 2.0));
  REQUIRE(bandWaveTestControl(*b.face, "split", 0.0));
  FaceRig w(meta::waveformType());
  REQUIRE(bandWaveTestControl(*w.face, "win", 60.0));
  b.paint();
  w.paint();
  auto worker = [](FaceRig& r) {
    const auto t0 = std::chrono::steady_clock::now();
    (void)r.play(10.0, {.hz = 220.0, .amp = 0.5});
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 10.0;
  };
  const double wb = worker(b), ww = worker(w);
  (void)w.play(50.0, {.hz = 220.0, .amp = 0.5});
  auto frame = [](FaceRig& r) {
    std::vector<double> ms;
    for (int i = 0; i < 60; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      (void)r.face->pollVoices();
      r.paint();
      ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return ms[30];
  };
  const double fb = frame(b), fw = frame(w);
  MESSAGE("worker per second of audio (tap read + stage + the step): band levels " << wb << " ms, waveform " << ww << " ms");
  MESSAGE("UI frame (poll + paint, 300 x 160 at 2x): band levels " << fb << " ms, waveform (60 s window) " << fw << " ms");
  CHECK(fb > 0.0);
}

// VZ.10: the colour options reach the paint (each chosen colour's RGB in the draw list; −1: gone again).
TEST_CASE("band / wave faces (VZ.10): a chosen colour is painted; −1 is the face's own") {
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
  auto control = [](MetaFace& f, std::string_view k, double v) { return bandWaveTestControl(f, k, v); };
  {
    FaceRig rig(meta::bandLevelsType());
    rig.paint();
    (void)rig.play(1.0);
    check(rig, control, "c1", {});
    check(rig, control, "c3", {});
  }
  {
    FaceRig rig(meta::waveformType());
    rig.paint();
    (void)rig.play(1.0);
    check(rig, control, "ca", {{"col", 0}});
  }
}

TEST_CASE("render: device-meta-bandwave matches the golden image") { checkScene("device-meta-bandwave"); }
TEST_CASE("render: project-meta-bandwave matches the golden image") { checkScene("project-meta-bandwave"); }
