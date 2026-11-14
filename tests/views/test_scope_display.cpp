// TX.scope: the feed's scope history (kWantScope: no spectrum analysed; reads, "none" outside the history; settles on
// silence; a watched feed wakes on sound), the Oscilloscope face (its feed from the host's source, held while painted
// and let go when hidden; the pitch timebase from a detected pitch and from the note; channels; beat slots;
// persistence; freeze; its options as the instance's data, one undo step each), the scenes device-meta-scope and
// project-meta-scope, and the UI frame cost of 4 scopes.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../../src/views/src/meta/faces.h"  // TX.scope: the face's frame (tests)
#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/meta/oscilloscope.h"
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

constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = 48000.0;

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

double bright(double ph) {
  double v = 0.0;
  for (int k = 1; k <= 8; ++k) v += std::sin(2.0 * kPi * k * ph + 0.7 * k * k) / k;
  return 0.45 * v;
}

// Writes `seconds` of the 4-channel scope layout into the tap from frame `at`: beat (120 bpm), note Hz, L, rGain · L.
void writeScope(TapRing& tap, double seconds, float noteHz, double (*sig)(double), uint64_t& at, float rGain = 0.5f) {
  const auto n = static_cast<uint32_t>(seconds * kSr);
  std::vector<float> b(1024), hz(1024), l(1024), r(1024);
  for (uint32_t i = 0; i < n; i += 1024) {
    const uint32_t k = std::min<uint32_t>(1024, n - i);
    for (uint32_t j = 0; j < k; ++j) {
      const double t = static_cast<double>(at + j) / kSr;
      b[j] = static_cast<float>(t * 2.0);
      hz[j] = noteHz;
      l[j] = static_cast<float>(sig(t));
      r[j] = rGain * l[j];
    }
    const float* ch[4] = {b.data(), hz.data(), l.data(), r.data()};
    tap.write(ch, 4, k);
    at += k;
  }
}
double bright220(double t) { return 0.5 * bright(220.0 * t); }
double silence(double) { return 0.0; }

struct Clock {
  static double& now() {
    static double c = 10.0;
    return c;
  }
};

// A target that keeps the instance's data (one "undo step" per setData outside a gesture).
class DataTarget final : public MetaTarget {
public:
  const meta::Type* targetType() const noexcept override { return &meta::oscilloscopeType(); }
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
  bool gesture_ = false;
};

struct FaceRig {
  TapRing tap{4, 32768, 4096};
  SpectrumFeed feed{tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{}};
  std::unique_ptr<MetaFace> face = makeFace(meta::oscilloscopeType());
  std::unique_ptr<Theme> theme;
  DrawList dl;
  int wakes = 0;
  uint64_t at = 0;
  double step = 1.0;
  explicit FaceRig(std::unique_ptr<MetaTarget> target = nullptr) {
    auto th = loadTheme(defaultThemePath());
    REQUIRE(th.ok());
    theme = std::make_unique<Theme>(*th);
    MetaFace::setClock(+[]() -> double { return Clock::now(); });
    REQUIRE(face != nullptr);
    face->bind(std::move(target), nullptr);
    face->setAnalysis([this] { return &feed; }, [this] { ++wakes; });
  }
  ~FaceRig() { MetaFace::setClock(nullptr); }
  void paint() {
    dl.reset(1000, 600);
    FaceEnv env;
    env.theme = theme.get();
    env.uiScale = 1.0f;
    FacePainter p(dl, {{10.0f, 10.0f}, 2.0f});
    face->paintLive(p, env);
  }
  // Plays `seconds` through the tap into the feed's history, then one poll.
  ScopeShown play(double seconds, float noteHz = 0.0f, double (*sig)(double) = bright220) {
    writeScope(tap, seconds, noteHz, sig, at);
    feed.step(step += seconds);
    Clock::now() += 1.0 / 60.0;
    paint();  // painted: shown
    (void)face->pollVoices();
    ScopeShown s;
    REQUIRE(scopeShown(*face, s));
    return s;
  }
};

}  // namespace

TEST_CASE("scope feed: the history without any spectrum, none outside it, settles on silence, wakes on sound") {
  TapRing tap(4, 32768, 4096);
  SpectrumFeed feed(tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{});
  CHECK(feed.scopeChannels() == 4);
  feed.setWatch(true);
  int posts = 0;
  feed.setWakePost([&] { ++posts; });
  feed.acquire(kWantScope);
  CHECK(tap.wanted());
  uint64_t at = 0;
  writeScope(tap, 0.25, 196.0f, bright220, at, -1.0f);  // a pure side signal: its mono is 0, it still sounds
  feed.step(1.0);
  CHECK(feed.hopsAnalysed() == 0);  // a scope-only consumer: no spectrum
  bool settled = true;
  CHECK(feed.scopeEnd(settled) == at);
  CHECK_FALSE(settled);
  std::vector<float> c[4];
  for (auto& v : c) v.resize(64);
  float* out[4] = {c[0].data(), c[1].data(), c[2].data(), c[3].data()};
  CHECK(feed.scopeRead(at - 32, 64, out) == 32);  // half of it is not written yet
  CHECK(c[0][0] == doctest::Approx(2.0 * static_cast<double>(at - 32) / kSr));
  CHECK(c[1][0] == 196.0f);
  CHECK(c[2][5] == doctest::Approx(bright220(static_cast<double>(at - 32 + 5) / kSr)));
  CHECK(c[3][5] == -c[2][5]);
  CHECK(std::isnan(c[0][40]));  // not held: no clock, no note, zero signal
  CHECK(std::isnan(c[1][40]));
  CHECK(c[2][40] == 0.0f);
  // Silence for longer than the longest window: settled; then sound posts one wake.
  double now = 1.0;
  for (int k = 0; k < 30; ++k) {
    writeScope(tap, 0.1, 0.0f, silence, at);
    feed.step(now += 0.1);
  }
  feed.scopeEnd(settled);
  CHECK(settled);
  CHECK(posts == 0);
  writeScope(tap, 0.1, 0.0f, bright220, at, -1.0f);
  feed.step(now += 0.1);
  feed.scopeEnd(settled);
  CHECK_FALSE(settled);
  CHECK(posts == 1);
  // Nothing arrives (the unit sleeps): silence is appended (no clock) until it rests.
  for (int k = 0; k < 40; ++k) feed.step(now += 0.1);
  const uint64_t end = feed.scopeEnd(settled);
  CHECK(end > at);
  CHECK(settled);
  CHECK(feed.scopeRead(end - 64, 64, out) == 64);
  CHECK(std::isnan(c[0][10]));
  CHECK(c[2][10] == 0.0f);
  feed.release(kWantScope);
  CHECK_FALSE(tap.wanted());
}

TEST_CASE("scope face: held while painted, a still trace from the detected pitch and from the note, let go when hidden") {
  FaceRig rig;
  REQUIRE(rig.face->wantsAnalysis());
  CHECK_FALSE(rig.tap.wanted());  // not painted yet
  rig.paint();
  CHECK(rig.tap.wanted());
  // A detected 220 Hz (no note): the default timebase (Pitch, Constant 52.2 ms).
  std::vector<double> phases;
  ScopeShown s;
  for (int k = 0; k < 40; ++k) {
    s = rig.play(1.0 / 60.0);
    if (k >= 10) phases.push_back(std::fmod(220.0 * static_cast<double>(s.start) / kSr, 1.0));
  }
  CHECK(s.hasFrame);
  CHECK(s.acquired);
  CHECK(s.pitchHz == doctest::Approx(220.0).epsilon(0.002));
  CHECK_FALSE(s.note);
  CHECK(s.span == static_cast<size_t>(52.2 * 0.001 * kSr));
  for (const double p : phases) {
    double d = p - phases.front();
    d -= std::round(d);
    CHECK(std::fabs(d) * kSr / 220.0 <= 1.0);  // the start sits on the same phase (≤ 1 sample)
  }
  float peak = 0.0f;
  for (const float v : s.samples) peak = std::max(peak, std::fabs(v));
  CHECK(s.gain == doctest::Approx(0.72f / peak).epsilon(0.05));  // auto gain: 0.72 of the visible peak
  // The note (the module per voice): its exact period, Tracking two cycles.
  REQUIRE(scopeTestControl(*rig.face, "lock", 1.0));
  REQUIRE(scopeTestControl(*rig.face, "cyc", 2.0));
  for (int k = 0; k < 5; ++k) s = rig.play(1.0 / 60.0, 196.0f);
  CHECK(s.note);
  CHECK(s.pitchHz == doctest::Approx(196.0f));
  CHECK(s.span == static_cast<size_t>(std::llround(2.0 * kSr / 196.0)));
  // Hidden: polled but not painted — after one unanswered repaint it lets go.
  Clock::now() += 1.0;
  CHECK(rig.face->pollVoices());
  Clock::now() += 0.1;
  CHECK_FALSE(rig.face->pollVoices());
  CHECK_FALSE(rig.tap.wanted());
}

TEST_CASE("scope face: channels, freeze, persistence, beat slots") {
  FaceRig rig;
  rig.paint();
  ScopeShown s = rig.play(0.2);
  // L = x, R = x / 2: mid 3x / 4, side x / 4.
  auto peakOf = [&](uint32_t ch) {
    REQUIRE(scopeTestControl(*rig.face, "ch", ch));
    const ScopeShown r = rig.play(1.0 / 60.0);
    CHECK(r.channel == ch);
    float pk = 0.0f;
    for (const float v : r.samples) pk = std::max(pk, std::fabs(v));
    return pk;
  };
  const float left = peakOf(1), right = peakOf(2), mid = peakOf(0), side = peakOf(3);
  CHECK(left > 0.3f);
  CHECK(right == doctest::Approx(0.5f * left).epsilon(0.05));
  CHECK(mid == doctest::Approx(0.75f * left).epsilon(0.05));
  CHECK(side == doctest::Approx(0.25f * left).epsilon(0.05));
  REQUIRE(scopeTestControl(*rig.face, "ch", 1.0));
  // Freeze: the picture holds while the signal goes on.
  s = rig.play(1.0 / 60.0);
  const uint64_t start = s.start;
  REQUIRE(scopeTestControl(*rig.face, "freeze", 1.0));
  s = rig.play(0.1);
  CHECK(s.frozen);
  CHECK(s.start == start);
  REQUIRE(scopeTestControl(*rig.face, "freeze", 0.0));
  // Persistence 0.5: up to 12 traces kept.
  REQUIRE(scopeTestControl(*rig.face, "pers", 0.5));
  for (int k = 0; k < 20; ++k) s = rig.play(1.0 / 60.0);
  CHECK(s.trails == 12);
  const uint32_t before = rig.dl.vtxCurrentIdx();
  rig.paint();
  CHECK(rig.dl.vtxCurrentIdx() > before / 2);  // trails drawn
  // Beat: one bar (4 beats) at 120 bpm is 2 s; the playhead is the clock's phase in it.
  REQUIRE(scopeTestControl(*rig.face, "tb", 2.0));
  s = rig.play(0.5);
  CHECK(s.timebase == 2);
  const double beat = 2.0 * static_cast<double>(rig.at - 1) / kSr;
  CHECK(s.beatPhase == doctest::Approx(std::fmod(beat, 4.0) / 4.0).epsilon(1e-3));
  // A measurement paints its card (no crash at any span).
  REQUIRE(scopeTestControl(*rig.face, "tb", 0.0));
  s = rig.play(1.0 / 60.0);
  REQUIRE(scopeTestMeasure(*rig.face, 10.0, 228.0));
  rig.paint();
  REQUIRE(scopeTestControl(*rig.face, "sheet", 1.0));
  rig.paint();
}

TEST_CASE("scope face: options are the instance's data — one step per change, read back by a new face") {
  auto target = std::make_unique<DataTarget>();
  DataTarget* t = target.get();
  {
    FaceRig rig(std::move(target));
    rig.paint();
    REQUIRE(scopeTestControl(*rig.face, "tb", 0.0));
    REQUIRE(scopeTestControl(*rig.face, "glow", 0.8));
    CHECK(t->steps == 2);
    REQUIRE(t->data_ != nullptr);
    CHECK(meta::oscilloscopeType().dataProblem(*t->data_).empty());
    const meta::DataValue* g = t->data_->field("glow");
    REQUIRE(g != nullptr);
    CHECK(g->number == doctest::Approx(0.8));
    CHECK(t->data_->field("ch") == nullptr);  // defaults are not written
    // Undo (the data goes back): the face follows.
    t->data_ = std::make_shared<const meta::DataValue>(meta::DataValue::record({{"tb", meta::DataValue::num(2.0)}}));
    ScopeShown s = rig.play(0.1);
    CHECK(s.timebase == 2);
    // Keep the data for the next face.
    auto kept = std::make_unique<DataTarget>();
    kept->data_ = t->data_;
    FaceRig rig2(std::move(kept));
    s = rig2.play(0.1);
    CHECK(s.timebase == 2);
  }
}

TEST_CASE("render: device-meta-scope matches the golden image") { checkScene("device-meta-scope"); }

TEST_CASE("render: project-meta-scope matches the golden image") { checkScene("project-meta-scope"); }

TEST_CASE("scope displays: the UI frame of 4 scopes (poll + paint)" * doctest::test_suite("perf")) {
  std::vector<std::unique_ptr<FaceRig>> rigs;
  for (int i = 0; i < 4; ++i) {
    rigs.push_back(std::make_unique<FaceRig>());
    rigs.back()->paint();
    (void)rigs.back()->play(0.5);
  }
  REQUIRE(scopeTestControl(*rigs[1]->face, "ch", 4.0));    // L / R lanes
  REQUIRE(scopeTestControl(*rigs[2]->face, "pers", 0.5));  // trails
  REQUIRE(scopeTestControl(*rigs[3]->face, "win", 500.0)); // the envelope
  REQUIRE(scopeTestControl(*rigs[3]->face, "tb", 0.0));
  DrawList dl;
  auto theme = loadTheme(defaultThemePath());
  REQUIRE(theme.ok());
  auto frame = [&] {
    for (auto& r : rigs) {
      writeScope(r->tap, 1.0 / 60.0, 0.0f, bright220, r->at);
      r->feed.step(r->step += 1.0 / 60.0);
    }
    Clock::now() += 1.0 / 60.0;
    const auto t0 = std::chrono::steady_clock::now();
    dl.reset(2560, 1440);
    for (size_t i = 0; i < rigs.size(); ++i) {
      (void)rigs[i]->face->pollVoices();
      FaceEnv env;
      env.theme = &*theme;
      env.uiScale = 1.0f;
      FacePainter p(dl, {{20.0f + 620.0f * static_cast<float>(i % 2), 20.0f + 400.0f * static_cast<float>(i / 2)}, 2.3f});
      rigs[i]->face->paintLive(p, env);  // a 260 × 160 face at 2.3x: 600 × 370 device px
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  };
  for (int i = 0; i < 20; ++i) frame();
  std::vector<double> ms;
  for (int i = 0; i < 200; ++i) ms.push_back(frame());
  std::sort(ms.begin(), ms.end());
  MESSAGE("4 scopes (pitch, L/R lanes, persistence, 500 ms envelope), poll + paint: median " << ms[100] << " ms, p99 " << ms[198]
                                                                                            << " ms");
#ifdef PQ_OPTIMIZED
  CHECK(ms[100] < 4.0);
#endif
}

TEST_CASE("scope face: pointer — chips cycle, a row drag is one step, Alt+wheel zooms, Ctrl+drag measures") {
  auto target = std::make_unique<DataTarget>();
  DataTarget* t = target.get();
  FaceRig rig(std::move(target));
  rig.paint();
  (void)rig.play(0.3);
  auto ev = [&](FacePointer::Kind k, float x, float y, ui::Mods mods = ui::mod::kNone, uint8_t clicks = 1) {
    FacePointer e;
    e.kind = k;
    e.local = {x, y};
    e.device = {2.0f * x, 2.0f * y};
    e.pxPerUnit = 2.0f;
    e.mods = mods;
    e.clicks = clicks;
    return rig.face->onPointer(e);
  };
  // The timebase chip: Pitch → Beat → Fixed (Shift: back).
  CHECK(ev(FacePointer::Kind::Down, 20.0f, 17.0f));
  ScopeShown s = rig.play(1.0 / 60.0);
  CHECK(s.timebase == 2);
  CHECK(ev(FacePointer::Kind::Down, 20.0f, 17.0f, ui::mod::kShift));
  s = rig.play(1.0 / 60.0);
  CHECK(s.timebase == 1);
  CHECK(t->steps == 2);
  // SET (it opens on DISPLAY), the ANALYSIS tab, then drag the Window row (Pitch: Timebase, Pitch lock, Pitch from,
  // Window …) up: one undo step (the tab is view state: no step).
  auto find = [&](int32_t part) {  // VZ.1: the centre of a part, found through hitTest
    float sx = 0.0f, sy = 0.0f;
    int n = 0;
    for (float y = 0.0f; y < 200.0f; y += 0.5f)
      for (float x = 0.0f; x < 540.0f; x += 0.5f)  // (BR.ui: the sheet opens right of the face)
        if (rig.face->hitTest({x, y}, 2.0f) == part) {
          sx += x;
          sy += y;
          ++n;
        }
    REQUIRE(n > 0);
    return Vec2{sx / static_cast<float>(n), sy / static_cast<float>(n)};
  };
  CHECK(ev(FacePointer::Kind::Down, 230.0f, 17.0f));
  const Vec2 tab = find(90);  // sheet::kTabBase + kAnalysis
  CHECK(ev(FacePointer::Kind::Down, tab.x, tab.y));
  const Vec2 row = find(103);
  CHECK(ev(FacePointer::Kind::Down, row.x, row.y));
  CHECK(rig.face->dragging());
  CHECK(ev(FacePointer::Kind::Move, row.x, row.y - 20.0f));
  CHECK(ev(FacePointer::Kind::Move, row.x, row.y - 40.0f));
  CHECK(ev(FacePointer::Kind::Up, row.x, row.y - 40.0f));
  CHECK(t->steps == 3);
  const meta::DataValue* win = t->data_->field("win");
  REQUIRE(win != nullptr);
  CHECK(win->number > 52.2);
  CHECK(ev(FacePointer::Kind::Down, 230.0f, 17.0f));  // close the sheet
  // Alt+wheel over the plot zooms (the canvas keeps the plain wheel).
  FacePointer w;
  w.kind = FacePointer::Kind::Wheel;
  w.local = {130.0f, 80.0f};
  w.wheel = {0.0f, 3.0f};
  CHECK_FALSE(rig.face->onPointer(w));
  w.mods = ui::mod::kAlt;
  CHECK(rig.face->onPointer(w));
  // Ctrl+drag measures; Esc clears; a plain drag pans while zoomed; a double-click resets the zoom.
  CHECK(ev(FacePointer::Kind::Down, 60.0f, 80.0f, ui::mod::kCtrl));
  CHECK(ev(FacePointer::Kind::Move, 120.0f, 80.0f, ui::mod::kCtrl));
  CHECK(ev(FacePointer::Kind::Up, 120.0f, 80.0f, ui::mod::kCtrl));
  ui::KeyEvent esc;
  esc.key = ui::key::kEscape;
  CHECK(rig.face->onKey(esc));
  CHECK_FALSE(rig.face->onKey(esc));
  CHECK(ev(FacePointer::Kind::Down, 100.0f, 80.0f));
  CHECK(ev(FacePointer::Kind::Move, 80.0f, 80.0f));
  CHECK(ev(FacePointer::Kind::Up, 80.0f, 80.0f));
  CHECK(ev(FacePointer::Kind::Down, 100.0f, 80.0f, ui::mod::kNone, 2));
  CHECK_FALSE(ev(FacePointer::Kind::Down, 100.0f, 80.0f));  // not zoomed: the host's (select, move)
}
