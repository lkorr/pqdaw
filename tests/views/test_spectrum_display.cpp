// TX.spectrum: the analysis feed (input / output / measured response from a 4-channel tap; silence; watching; hidden),
// the reusable spectrum plot (silence draws nothing; what it draws follows the style), the Spectrum face (its feed from
// the host's source, polled while it moves, letting go when its host is hidden), the scenes device-meta-spectrum and
// project-meta-spectrum, and the UI frame cost of 8 displays.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/meta/spectrum.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/image.h"
#include "pq/ui/theme.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/devices/device_view.h"
#include "pq/views/devices/spectrum_plot.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/view_scenes.h"

using namespace pq;
using namespace pq::views;
using namespace pq::views::devices;
using namespace pq::ui;

namespace {

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

// A deterministic white noise block.
struct Noise {
  uint32_t s = 0x2468ACEu;
  float next(float a) {
    s = s * 1664525u + 1013904223u;
    return a * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f);
  }
};

// Writes `seconds` of a 4-channel tap block by block: input = noise · inAmp, output = f(input).
template <class F>
void writeTap(TapRing& tap, double seconds, F f, float inAmp = 0.3f, Noise* n = nullptr) {
  static Noise shared;
  Noise& nz = n != nullptr ? *n : shared;
  std::vector<float> x(512), y(512);
  for (uint32_t b = 0; b < static_cast<uint32_t>(seconds * 48000.0 / 512.0); ++b) {
    for (uint32_t i = 0; i < 512; ++i) {
      x[i] = nz.next(inAmp);
      y[i] = f(x[i]);
    }
    const float* ch[4] = {x.data(), x.data(), y.data(), y.data()};
    tap.write(ch, 4, 512);
  }
}

uint32_t nearestPoint(const SpectrumFeed& f, float hz) {
  uint32_t best = 0;
  for (uint32_t p = 1; p < f.config().points; ++p)
    if (std::fabs(std::log(f.pointHz(p) / hz)) < std::fabs(std::log(f.pointHz(best) / hz))) best = p;
  return best;
}

}  // namespace

TEST_CASE("analysis feed: a 4-channel tap gives the input, the output and the measured response when wanted") {
  TapRing tap(4, 32768, 1024);
  dsp::SpectrumConfig sc;
  SpectrumFeed feed(tap, sc, SpectrumFeed::Manual{});
  CHECK(feed.hasInput());
  feed.acquire(kWantOutput);  // output only: no response computed
  writeTap(tap, 0.5, [](float x) { return 0.5f * x; });
  feed.step(1.0);
  SpectrumSnapshot s;
  bool settled = false;
  feed.snapshot(0, s, settled);
  CHECK(s.out.size() == sc.points);
  CHECK(s.response.empty());
  CHECK(s.in.empty());
  // Wanting the response (and the input): a −6 dB device reads −6 dB where the input has energy.
  feed.acquire(kWantInput | kWantResponse);
  for (int k = 0; k < 8; ++k) {
    writeTap(tap, 0.25, [](float x) { return 0.5f * x; });
    feed.step(1.5 + 0.25 * k);
  }
  const uint64_t seq = feed.snapshot(0, s, settled);
  CHECK(seq > 0);
  REQUIRE(s.response.size() == sc.points);
  REQUIRE(s.confidence.size() == sc.points);
  REQUIRE(s.in.size() == sc.points);
  float worst = 0.0f;
  uint32_t shown = 0;
  for (uint32_t p = 0; p < sc.points; ++p) {
    if (feed.pointHz(p) < 50.0f || feed.pointHz(p) > 20000.0f || s.confidence[p] < 0.99f) continue;
    ++shown;
    worst = std::max(worst, std::fabs(s.response[p] + 6.0206f));
  }
  CHECK(shown > sc.points / 2);
  CHECK(worst < 0.1f);
  // The output's level sits 6 dB under the input's.
  const uint32_t p1k = nearestPoint(feed, 1000.0f);
  CHECK(s.in[p1k] - s.out[p1k] == doctest::Approx(6.02).epsilon(0.05));
  feed.release(kWantInput | kWantResponse);
  feed.release(kWantOutput);
}

TEST_CASE("analysis feed: silence shows no response and settles; a watched feed wakes its displays when sound comes") {
  TapRing tap(4, 32768, 1024);
  dsp::SpectrumConfig sc;
  SpectrumFeed feed(tap, sc, SpectrumFeed::Manual{});
  feed.setWatch(true);
  int posts = 0;
  feed.setWakePost([&] { ++posts; });
  int woken = 0;
  feed.addWaker(&woken, [&] { ++woken; });
  feed.acquire(kWantOutput | kWantResponse);
  // Silence flowing (an engine block of zeros): nothing confident, levels at the floor, settled.
  double now = 1.0;
  for (int k = 0; k < 40; ++k) {
    writeTap(tap, 0.1, [](float) { return 0.0f; }, 0.0f);
    feed.step(now += 0.1);
  }
  SpectrumSnapshot s;
  bool settled = false;
  feed.snapshot(0, s, settled);
  CHECK(settled);
  for (const float c : s.confidence) CHECK(c == 0.0f);
  const uint64_t hops = feed.hopsAnalysed();
  // Still silent: nothing is analysed at all (the zeros are skipped).
  writeTap(tap, 0.5, [](float) { return 0.0f; }, 0.0f);
  feed.step(now += 0.5);
  CHECK(feed.hopsAnalysed() == hops);
  CHECK(posts == 0);
  // Sound: the feed leaves rest and posts one wake (the UI thread then runs the wakers).
  writeTap(tap, 0.2, [](float x) { return x; });
  feed.step(now += 0.2);
  feed.snapshot(0, s, settled);
  CHECK_FALSE(settled);
  CHECK(posts == 1);
  feed.runWakers();
  CHECK(woken == 1);
  feed.removeWaker(&woken);
  feed.release(kWantOutput | kWantResponse);
  CHECK_FALSE(tap.wanted());
}

TEST_CASE("analysis feed: unrelated input and output (a generator) show no response") {
  TapRing tap(4, 32768, 1024);
  dsp::SpectrumConfig sc;
  SpectrumFeed feed(tap, sc, SpectrumFeed::Manual{});
  feed.acquire(kWantResponse);
  Noise other;
  other.s = 0x13579BDu;
  for (int k = 0; k < 12; ++k) {
    writeTap(tap, 0.25, [&](float) { return other.next(0.3f); });
    feed.step(1.0 + 0.25 * k);
  }
  SpectrumSnapshot s;
  bool settled = false;
  feed.snapshot(0, s, settled);
  uint32_t shown = 0;
  for (const float c : s.confidence) shown += c > 0.0f ? 1 : 0;
  CHECK(shown <= sc.points / 100);
  feed.release(kWantResponse);
}

TEST_CASE("analysis hub: one watched feed per device tap; a new tap supersedes; no tap, no feed") {
  auto tapA = std::make_shared<TapRing>(4, 16384, 1024);
  std::shared_ptr<TapRing> current = tapA;
  int posted = 0;
  AnalysisHub hub([&](doc::DeviceId d) { return d.value == 7 ? current : nullptr; }, 48000.0,
                  [&](std::function<void()> f) {
                    ++posted;
                    f();
                  });
  CHECK(hub.feed(doc::DeviceId{3}) == nullptr);
  SpectrumFeed* f = hub.feed(doc::DeviceId{7});
  REQUIRE(f != nullptr);
  CHECK(f->watching());
  CHECK(hub.feed(doc::DeviceId{7}) == f);
  f->acquire();
  current = std::make_shared<TapRing>(4, 16384, 1024);  // the device's slot was given a new tap
  SpectrumFeed* g = hub.feed(doc::DeviceId{7});
  REQUIRE(g != nullptr);
  CHECK(g != f);
  f->release();  // the old feed goes once nobody holds it
  CHECK(hub.feed(doc::DeviceId{7}) == g);
}

TEST_CASE("spectrum plot: silence draws no curve; the measured response draws where it is confident") {
  auto theme = loadTheme(defaultThemePath());
  REQUIRE(theme.ok());
  const uint32_t P = 256;
  SpectrumSnapshot s;
  s.out.assign(P, -140.0f);
  s.peaks.assign(P, -140.0f);
  SpectrumPlot plot;
  SpectrumPlotStyle st;
  st.peaks = true;
  st.response = true;
  plot.setStyle(st);
  plot.setShownForTest(s, static_cast<float>(P));
  DrawList dl;
  dl.reset(800, 400);
  const Rect r{10, 10, 610, 210};
  const SpectrumAxis ax{r.x0, r.x1};
  plot.paintSpectrum(dl, r, ax, 1.0f);
  plot.paintResponse(dl, r, ax, 1.0f);
  CHECK(dl.vtxCurrentIdx() == 0);  // nothing at the floor, no response known
  // A signal and a confident response: both draw.
  for (uint32_t p = 0; p < P; ++p) s.out[p] = -30.0f;
  s.response.assign(P, 3.0f);
  s.confidence.assign(P, 1.0f);
  plot.setShownForTest(s, static_cast<float>(P));
  plot.paintSpectrum(dl, r, ax, 1.0f);
  const uint32_t afterSpectrum = dl.vtxCurrentIdx();
  CHECK(afterSpectrum > 0);
  plot.paintResponse(dl, r, ax, 1.0f);
  CHECK(dl.vtxCurrentIdx() > afterSpectrum);
  CHECK(plot.levelAt(1000.0f) == doctest::Approx(-30.0f));
  float conf = 0.0f;
  CHECK(plot.responseAt(1000.0f, &conf) == doctest::Approx(3.0f));
  CHECK(conf == doctest::Approx(1.0f));
  // A response with no confidence draws nothing more.
  s.confidence.assign(P, 0.0f);
  plot.setShownForTest(s, static_cast<float>(P));
  const uint32_t before = dl.vtxCurrentIdx();
  plot.paintResponse(dl, r, ax, 1.0f);
  CHECK(dl.vtxCurrentIdx() == before);
}

TEST_CASE("spectrum face: reads its host's feed, polls while it moves, lets go when its host stops painting it") {
  auto theme = loadTheme(defaultThemePath());
  REQUIRE(theme.ok());
  std::unique_ptr<MetaFace> face = makeFace(meta::spectrumType());
  REQUIRE(face != nullptr);
  REQUIRE(face->wantsAnalysis());
  double clock = 10.0;

  static double* gClock = nullptr;
  gClock = &clock;
  MetaFace::setClock(+[]() -> double { return *gClock; });
  face->bind(nullptr, nullptr);
  CHECK(face->voices() == nullptr);  // no source: drawn as its grid
  TapRing tap(4, 32768, 1024);
  dsp::SpectrumConfig sc;
  SpectrumFeed feed(tap, sc, SpectrumFeed::Manual{});
  SpectrumFeed* source = nullptr;  // the tap appears later (the unit compiles)
  int wakes = 0;
  face->setAnalysis([&] { return source; }, [&] { ++wakes; });
  REQUIRE(face->voices() != nullptr);
  CHECK(face->pollVoices());  // no feed yet: a repaint that asks again
  CHECK_FALSE(tap.wanted());
  source = &feed;
  FaceEnv env;
  env.theme = &*theme;
  env.uiScale = 1.0f;
  DrawList dl;
  dl.reset(800, 400);
  FacePainter p(dl, {{10.0f, 10.0f}, 1.5f});
  face->paintLive(p, env);  // shown: holds the feed
  CHECK(tap.wanted());
  writeTap(tap, 0.3, [](float x) { return x; });
  feed.step(1.0);
  CHECK(face->pollVoices());  // new levels
  CHECK(face->animating());
  // Its host hides it (no more paints): after one unanswered repaint it lets go of the feed.
  clock += 1.0;
  CHECK(face->pollVoices());  // the probe: asks for a repaint
  clock += 0.1;
  CHECK_FALSE(face->pollVoices());
  CHECK_FALSE(tap.wanted());
  MetaFace::setClock(nullptr);
}

TEST_CASE("spectrum meta module: pass-through, a signal tap of request 0") {
  const meta::Type& t = meta::spectrumType();
  CHECK(t.chainInput == 0);
  REQUIRE(t.resources != nullptr);
  const auto req = t.resources({});
  REQUIRE(req.size() == 1);
  CHECK(req[0].kind == meta::ResourceKind::SignalTap);
}

TEST_CASE("render: device-meta-spectrum matches the golden image") { checkScene("device-meta-spectrum"); }

TEST_CASE("render: project-meta-spectrum matches the golden image") { checkScene("project-meta-spectrum"); }

TEST_CASE("spectrum displays: the UI frame of 8 displays (paint of every layer)" * doctest::test_suite("perf")) {
  auto theme = loadTheme(defaultThemePath());
  REQUIRE(theme.ok());
  const uint32_t P = 1024;
  SpectrumSnapshot s;
  s.out.resize(P);
  s.peaks.resize(P);
  s.response.resize(P);
  s.confidence.assign(P, 1.0f);
  for (uint32_t p = 0; p < P; ++p) {
    s.out[p] = -20.0f - 30.0f * static_cast<float>(p) / P + 6.0f * std::sin(0.05f * static_cast<float>(p));
    s.peaks[p] = s.out[p] + 4.0f;
    s.response[p] = 6.0f * std::sin(0.01f * static_cast<float>(p));
  }
  std::vector<std::unique_ptr<SpectrumPlot>> plots;
  for (int i = 0; i < 8; ++i) {
    auto pl = std::make_unique<SpectrumPlot>();
    SpectrumPlotStyle st;
    st.peaks = true;
    st.response = true;
    pl->setStyle(st);
    pl->setShownForTest(s, static_cast<float>(P));
    plots.push_back(std::move(pl));
  }
  DrawList dl;
  PlotText text;  // no text system: the labels' backing pads only
  auto frame = [&] {
    dl.reset(2560, 1440);
    for (int i = 0; i < 8; ++i) {
      const float x = 20.0f + 620.0f * static_cast<float>(i % 4), y = 20.0f + 360.0f * static_cast<float>(i / 4);
      plots[i]->paintAll(dl, {x, y, x + 600.0f, y + 220.0f}, *theme, 2.0f, text);  // a 300 × 110 display at 2x
    }
  };
  for (int i = 0; i < 20; ++i) frame();
  std::vector<double> ms;
  for (int i = 0; i < 200; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    frame();
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(ms.begin(), ms.end());
  MESSAGE("8 spectrum displays (spectrum + peaks + response + grid, 600 x 220 device px each): median " << ms[100] << " ms, p99 "
                                                                                                      << ms[198] << " ms");
#ifdef PQ_OPTIMIZED
  CHECK(ms[100] < 4.0);
#endif
}
