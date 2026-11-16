// VZ.4: the loudness displays — the feed's loudness stage (kWantLoudness alone: no spectrum; the readings and the
// history; nothing while hidden; settles at rest; options and resets), the three faces (held while painted, their
// options as the instance's data and as the feed's request, CLEAR, the lamps, let go when hidden, every option paints),
// and the scenes device-meta-loudness / project-meta-loudness.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../../src/views/src/meta/faces.h"  // VZ.4: the loudness faces' state (tests)
#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/meta/loudness.h"
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

// The test signal: a 1 kHz stereo sine at `db` dBFS (0: silence; > 0 dB: clips).
struct Signal {
  double db = -23.0;
  bool on = true;
  float at(uint64_t i) const {
    return on ? static_cast<float>(std::pow(10.0, db / 20.0) * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kSr)) : 0.0f;
  }
};

// `seconds` of the signal into the tap (the signal_tap layout: L, R, L, R), the feed stepped every 4096 frames.
void stream(TapRing& tap, SpectrumFeed& feed, double seconds, const Signal& s, uint64_t& at, double& clock) {
  const auto n = static_cast<uint32_t>(seconds * kSr);
  if (n == 0) feed.step(clock += 0.001);  // no data: one step (it applies requests and resets)
  std::vector<float> x(4096);
  for (uint32_t i = 0; i < n; i += 4096) {
    const uint32_t k = std::min<uint32_t>(4096, n - i);
    for (uint32_t j = 0; j < k; ++j) x[j] = s.at(at + j);
    const float* ch[4] = {x.data(), x.data(), x.data(), x.data()};
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
  int wakes = 0;
  uint64_t at = 0;
  double clock = 1.0;
  float scale = 2.0f;
  explicit FaceRig(const meta::Type& t, meta::DataPtr data = nullptr) : face(makeFace(t)) {
    auto th = loadTheme(defaultThemePath());
    REQUIRE(th.ok());
    theme = std::make_unique<Theme>(*th);
    MetaFace::setClock(+[]() -> double { return Clock::now(); });
    REQUIRE(face != nullptr);
    auto tg = std::make_unique<DataTarget>(t);
    tg->data_ = std::move(data);
    target = tg.get();
    face->bind(std::move(tg), nullptr);
    face->setAnalysis([this] { return &feed; }, [this] { ++wakes; });
  }
  ~FaceRig() { MetaFace::setClock(nullptr); }
  void paint() {
    dl.reset(1000, 600);
    FaceEnv env;
    env.theme = theme.get();
    env.uiScale = 1.0f;
    FacePainter p(dl, {{10.0f, 10.0f}, scale});
    face->paintLive(p, env);
  }
  LoudnessShown play(double seconds, const Signal& s = {}) {
    stream(tap, feed, seconds, s, at, clock);
    Clock::now() += 1.0 / 60.0;
    (void)face->pollVoices();
    paint();
    LoudnessShown out;
    REQUIRE(loudnessShown(*face, out));
    return out;
  }
};

const meta::Type* const kTypes[] = {&meta::levelMeterType(), &meta::lufsMeterType(), &meta::loudnessHistoryType()};

}  // namespace

TEST_CASE("loudness feed: kWantLoudness alone — no spectrum; the readings and the history; nothing while hidden; rests") {
  TapRing tap(4, 32768, 4096);
  SpectrumFeed feed(tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{});
  dsp::LoudnessConfig c;
  c.autoReset = false;
  feed.setLoudness(c);
  feed.acquire(kWantLoudness);
  uint64_t at = 0;
  double clock = 1.0;
  stream(tap, feed, 5.0, {}, at, clock);
  CHECK(feed.hopsAnalysed() == 0);  // no spectrum for a loudness-only consumer
  dsp::LoudnessReadings r;
  bool settled = true;
  std::vector<dsp::LoudnessPoint> pts(64);
  uint32_t got = 0;
  uint64_t seq = feed.loudnessRead(r, settled, 0, 64, pts.data(), &got);
  CHECK_FALSE(settled);
  CHECK(std::fabs(r.integrated + 23.0f) < 0.1f);
  CHECK(std::fabs(r.momentary + 23.0f) < 0.1f);
  CHECK(r.points == 50);
  REQUIRE(got == 50);
  CHECK(std::fabs(pts[49].shortTerm + 23.0f) < 0.1f);
  // Hidden (released): the worker computes nothing.
  feed.release(kWantLoudness);
  stream(tap, feed, 1.0, {}, at, clock);
  CHECK(feed.loudnessRead(r, settled) == seq);
  CHECK(r.points == 50);
  // Shown again: it resumes (the measurement kept); silence data settles it once the meter rests (≈ 6.3 s: the RMS and
  // the falling true peak under −100 dB), and nothing more is computed after that.
  feed.acquire(kWantLoudness);
  stream(tap, feed, 1.0, {}, at, clock);
  seq = feed.loudnessRead(r, settled);
  CHECK(r.points == 60);
  stream(tap, feed, 3.0, {.on = false}, at, clock);
  (void)feed.loudnessRead(r, settled);
  CHECK_FALSE(settled);
  stream(tap, feed, 5.0, {.on = false}, at, clock);
  seq = feed.loudnessRead(r, settled);
  CHECK(settled);
  CHECK(r.momentary == dsp::kNoLoudness);
  // The integrated holds through silence (auto-reset off); the 400 ms blocks straddling the tone's end are above the
  // relative gate and count (BS.1770), so it sits a little under −23.
  CHECK(std::fabs(r.integrated + 23.0f) < 0.25f);
  stream(tap, feed, 2.0, {.on = false}, at, clock);
  CHECK(feed.loudnessRead(r, settled) == seq);  // at rest: silent data computes nothing
  // Sound wakes it.
  stream(tap, feed, 0.2, {.db = -40.0}, at, clock);
  CHECK(feed.loudnessRead(r, settled) != seq);
  CHECK_FALSE(settled);
  feed.release(kWantLoudness);
}

TEST_CASE("loudness feed: no data — the levels fall in real time; options and resets reach the worker") {
  TapRing tap(4, 32768, 4096);
  SpectrumFeed feed(tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{});
  dsp::LoudnessConfig c;
  c.autoReset = false;
  c.clipThreshDb = -6.0f;
  feed.setLoudness(c);
  feed.acquire(kWantLoudness);
  uint64_t at = 0;
  double clock = 1.0;
  stream(tap, feed, 3.0, {.db = -3.0}, at, clock);
  dsp::LoudnessReadings r;
  bool settled = false;
  (void)feed.loudnessRead(r, settled);
  CHECK(r.clip[0]);  // (a sine over the threshold: a clip event each half cycle)
  CHECK(std::fabs(r.truePeakDb + 3.0f) < 0.1f);
  // The source stops writing: silence is paced in real time (the momentary empties within 0.4 s, 3 s for short-term).
  for (int k = 0; k < 4 * 60; ++k) feed.step(clock += 1.0 / 60.0);
  (void)feed.loudnessRead(r, settled);
  CHECK(r.momentary == dsp::kNoLoudness);
  CHECK(r.shortTerm == dsp::kNoLoudness);
  CHECK(r.peakDb[0] < -60.0f);
  CHECK(r.clip[0]);  // latched
  feed.resetClips();
  feed.step(clock += 1.0 / 60.0);
  (void)feed.loudnessRead(r, settled);
  CHECK_FALSE(r.clip[0]);
  CHECK(std::fabs(r.integrated + 3.0f) < 0.3f);  // (the blocks straddling the tone's end count: a little under −3)
  feed.resetLoudness();
  feed.step(clock += 1.0 / 60.0);
  (void)feed.loudnessRead(r, settled);
  CHECK(r.integrated == dsp::kNoLoudness);
  CHECK(r.truePeakDb == dsp::kNoLoudness);
  CHECK(r.historyStart == r.points);
  // A new request: the measurement continues, the new threshold applies.
  c.clipThreshDb = 0.0f;
  feed.setLoudness(c);
  stream(tap, feed, 0.5, {.db = -3.0}, at, clock);
  (void)feed.loudnessRead(r, settled);
  CHECK_FALSE(r.clip[0]);
  feed.release(kWantLoudness);
}

TEST_CASE("loudness faces: held while painted, the feed's readings shown, the request their options make; let go when hidden") {
  for (const meta::Type* t : kTypes) {
    CAPTURE(t->id);
    FaceRig rig(*t);
    rig.paint();
    CHECK(rig.feed.consumers() == 1);
    LoudnessShown s = rig.play(4.0, {.db = -14.0});
    CHECK(s.acquired);
    if (t == &meta::levelMeterType()) {
      CHECK(s.config.parts == dsp::kLoudLevels);
      CHECK(s.readings.peakDb[0] == doctest::Approx(-14.0).epsilon(0.002));
      CHECK(s.readings.rmsDb[1] == doctest::Approx(-17.01).epsilon(0.002));
    } else {
      CHECK((s.config.parts & dsp::kLoudLoudness) != 0);
      CHECK((s.config.parts & dsp::kLoudHistory) != 0);
      CHECK(std::fabs(s.readings.integrated + 14.0f) < 0.1f);
      CHECK(std::fabs(s.readings.shortTerm + 14.0f) < 0.1f);
      CHECK(s.points == 40);  // 4 s of 100 ms points (the History's 30 s / the LUFS trace's 60 s window)
      CHECK(s.config.targetLufs == -8.0f);  // PQ's default target
      REQUIRE(loudnessTestControl(*rig.face, "preset", 3.0));  // EBU R128
      REQUIRE(loudnessShown(*rig.face, s));
      CHECK(s.config.targetLufs == -23.0f);
      REQUIRE(loudnessTestControl(*rig.face, "target", -16.5));  // a target edit is Custom
      REQUIRE(loudnessShown(*rig.face, s));
      CHECK(s.config.targetLufs == -16.5f);
      REQUIRE(rig.target->data_ != nullptr);
      CHECK(rig.target->data_->field("preset") == nullptr);  // back at the default (Custom): not written
      CHECK(rig.target->data_->field("target")->number == -16.5);
      CHECK(t->dataProblem(*rig.target->data_).empty());
      // CLEAR: the measurement starts again.
      REQUIRE(loudnessTestControl(*rig.face, "clear", 1.0));
      s = rig.play(0.0);
      CHECK(s.readings.integrated == dsp::kNoLoudness);
      CHECK(s.points == 0);
    }
    // Hidden: polled but not painted — after one unanswered repaint it lets go.
    Clock::now() += 1.0;
    (void)rig.face->pollVoices();
    Clock::now() += 0.1;
    (void)rig.face->pollVoices();
    REQUIRE(loudnessShown(*rig.face, s));
    CHECK_FALSE(s.acquired);
    CHECK(rig.feed.consumers() == 0);
  }
}

TEST_CASE("analyser faces: a face that let go sends nothing to the feed it had (H1) — the options go to the one bound now") {
  // The hub frees a feed nothing holds when its tap or the rate changes. A hidden face (let go) whose instance's data
  // changed (undo, or its other face's edit) sent the new options to the cached, freed feed on its next poll / paint.
  // Here the old feed stays alive (the test holds it) so what reaches it is visible.
  FaceRig rig(meta::lufsMeterType());
  SpectrumFeed other{rig.tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{}};
  SpectrumFeed* bound = &rig.feed;
  rig.face->setAnalysis([&] { return bound; }, [&] { ++rig.wakes; });
  rig.paint();
  REQUIRE(rig.feed.consumers() == 1);
  Clock::now() += 1.0;  // hidden: it lets go
  (void)rig.face->pollVoices();
  Clock::now() += 0.1;
  (void)rig.face->pollVoices();
  REQUIRE(rig.feed.consumers() == 0);
  const dsp::LoudnessConfig before = rig.feed.loudnessRequest();
  // The instance's data changes elsewhere: a new target.
  meta::DataPtr edited;
  {
    FaceRig elsewhere(meta::lufsMeterType());
    elsewhere.paint();
    REQUIRE(loudnessTestControl(*elsewhere.face, "target", -16.5));
    edited = elsewhere.target->data_;
  }
  MetaFace::setClock(+[]() -> double { return Clock::now(); });  // (the other rig's destructor cleared it)
  rig.target->data_ = edited;
  bound = &other;  // the feed it had is replaced (the hub would free it now)
  (void)rig.face->pollVoices();
  CHECK(rig.feed.loudnessRequest() == before);
  rig.paint();
  CHECK(rig.feed.loudnessRequest() == before);  // nothing reached the old feed
  CHECK(rig.feed.consumers() == 0);
  CHECK(other.consumers() == 1);
  CHECK(other.loudnessRequest().targetLufs == -16.5f);
  rig.face.reset();  // (it holds `other`, which goes first)
}

TEST_CASE("loudness faces: the LUFS Meter's true peak option, the Level Meter's lamps and its analysis options") {
  {
    FaceRig rig(meta::lufsMeterType());
    rig.paint();
    LoudnessShown s;
    REQUIRE(loudnessShown(*rig.face, s));
    CHECK((s.config.parts & dsp::kLoudTruePeak) != 0);
    REQUIRE(loudnessTestControl(*rig.face, "tp", 0.0));
    REQUIRE(loudnessShown(*rig.face, s));
    CHECK((s.config.parts & dsp::kLoudTruePeak) == 0);
    s = rig.play(1.0, {.db = -6.0});
    CHECK(s.readings.truePeakDb == s.readings.samplePeakDb);  // off: the sample peak
  }
  FaceRig rig(meta::levelMeterType());
  rig.paint();
  REQUIRE(loudnessTestControl(*rig.face, "clip", -3.0));
  REQUIRE(loudnessTestControl(*rig.face, "rms", 100.0));
  LoudnessShown s;
  REQUIRE(loudnessShown(*rig.face, s));
  CHECK(s.config.clipThreshDb == -3.0f);
  CHECK(s.config.rmsWindowMs == 100.0f);
  s = rig.play(0.5, {.db = -1.0});
  CHECK(s.readings.clip[0]);
  REQUIRE(loudnessTestControl(*rig.face, "lamps", 1.0));  // a click on the lamps
  s = rig.play(0.05, {.on = false});
  CHECK_FALSE(s.readings.clip[0]);
  CHECK(s.readings.holdDb[0] < -1.5f);  // the holds clear with the lamps
}

TEST_CASE("loudness faces: options read back by a new face; every option and state paints") {
  for (const meta::Type* t : kTypes) {
    CAPTURE(t->id);
    meta::DataPtr kept;
    {
      FaceRig rig(*t);
      rig.paint();
      REQUIRE(loudnessTestControl(*rig.face, t == &meta::levelMeterType() ? "floor" : "lo", -50.0));
      CHECK(rig.target->steps == 1);
      kept = rig.target->data_;
    }
    FaceRig rig(*t, kept);
    rig.paint();
    REQUIRE(rig.target->data_ != nullptr);
    (void)rig.play(3.0, {.db = 1.0});  // over full scale
    // Every option at both ends paints (no crash, nothing outside the draw list's reach).
    std::vector<std::pair<std::string, double>> opts;
    if (t == &meta::levelMeterType()) opts = {{"floor", -90}, {"floor", -24}, {"ro", 0}, {"zones", 0}, {"inf", 1}, {"clipreset", 30}};
    else if (t == &meta::lufsMeterType()) opts = {{"lo", -80}, {"hi", 6}, {"hi", -20}, {"trace", 0}, {"hist", 180}, {"tp", 0}};
    else opts = {{"scale", 1}, {"scale", 2}, {"hist", 180}, {"hist", 10}, {"band", 0}, {"grid", 0}, {"ro", 0}, {"m", 0}, {"tol", 6}};
    for (const auto& [k, v] : opts) {
      REQUIRE(loudnessTestControl(*rig.face, k, v));
      rig.paint();
    }
    REQUIRE(loudnessTestControl(*rig.face, "sheet", 1.0));
    rig.paint();
    rig.scale = 0.4f;  // far out: no text, the bars and traces stay
    rig.paint();
    REQUIRE(loudnessTestControl(*rig.face, "freeze", 1.0));
    LoudnessShown s;
    REQUIRE(loudnessShown(*rig.face, s));
    CHECK(s.frozen);
  }
}

// VZ.10: the colour options reach the paint — each chosen colour (sheet::colourOpt, 0xRRGGBB) is painted (its RGB in
// the draw list, the face's alpha kept) with a loud signal (over full scale: the hot zone, the lamps, over target), and
// back on the face's own (−1) it is gone; the defaults paint as before (the goldens).
TEST_CASE("loudness faces (VZ.10): a chosen colour is painted; −1 is the face's own") {
  constexpr double kPick = 0x0A141E;  // rgb(10, 20, 30): nothing else paints it
  auto painted = [](const DrawList& dl) {
    for (const Vertex& v : dl.vertices())
      if (ui::redOf(v.col) == 10 && ui::greenOf(v.col) == 20 && ui::blueOf(v.col) == 30 && ui::alphaOf(v.col) > 0) return true;
    return false;
  };
  for (const meta::Type* t : kTypes) {
    CAPTURE(t->id);
    FaceRig rig(*t);
    rig.paint();
    (void)rig.play(4.0, {.db = 1.0});
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, double>>>> cases;  // key, the options it needs
    if (t == &meta::levelMeterType())
      cases = {{"chot", {}}, {"cclip", {}}, {"cl", {{"zones", 0}}}, {"cr", {{"zones", 0}}}, {"ccap", {{"zones", 0}}}};
    else if (t == &meta::lufsMeterType())
      cases = {{"cover", {}}, {"ctrace", {}}};
    else
      cases = {{"cm", {}}, {"cs", {}}, {"ci", {}}, {"cover", {}}, {"con", {}}};
    for (const auto& [key, needs] : cases) {
      CAPTURE(key);
      for (const auto& [k, v] : needs) REQUIRE(loudnessTestControl(*rig.face, k, v));
      rig.paint();
      CHECK_FALSE(painted(rig.dl));
      REQUIRE(loudnessTestControl(*rig.face, key, kPick));
      rig.paint();
      CHECK(painted(rig.dl));
      REQUIRE(loudnessTestControl(*rig.face, key, -1.0));
      for (const auto& [k, v] : needs) REQUIRE(loudnessTestControl(*rig.face, k, k == "zones" ? 1.0 : 0.0));
      rig.paint();
      CHECK_FALSE(painted(rig.dl));
    }
  }
}

TEST_CASE("render: device-meta-loudness matches the golden image") { checkScene("device-meta-loudness"); }

TEST_CASE("render: project-meta-loudness matches the golden image") { checkScene("project-meta-loudness"); }
