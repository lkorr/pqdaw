// TX.spectrogram: the feed's spectrogram columns (kWantSpectrogram: no spectrum analysed; the request with the most
// columns decides; settles after a display's worth of silent columns; a watched feed wakes on sound), the Spectrogram
// face (its feed from the host, held while painted and let go when hidden; a sine on its texel rows; the colour options
// recolour from the cache without re-analysing; a new axis starts over; Loop places columns by the tapped beat; options
// are the instance's data), the ring texture (only new columns are uploaded per frame), the scenes
// device-meta-spectrogram and project-meta-spectrogram, and the UI frame of 4 spectrograms.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../../src/views/src/meta/faces.h"  // TX.spectrogram: the face's picture (tests)
#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/dsp/spectrogram.h"
#include "pq/meta/spectrogram.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/texture_pool.h"
#include "pq/ui/theme.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/fake_transport.h"
#include "pq/views/meta/face_targets.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/view_context.h"
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

double sine1k(double t) { return 0.5 * std::sin(2.0 * kPi * 1000.0 * t); }
double silence(double) { return 0.0; }
double chord(double t) {
  double v = 0.0;
  for (const double f : {220.0, 277.18, 329.63})
    for (int h = 1; h <= 4; ++h) v += 0.12 / h * std::sin(2.0 * kPi * f * h * t + h);
  return v;
}

// `seconds` of the Spectrogram's tap layout from frame `at`: beat (120 bpm), 0, L, R = L.
void writeGram(TapRing& tap, double seconds, double (*sig)(double), uint64_t& at) {
  const auto n = static_cast<uint32_t>(seconds * kSr);
  std::vector<float> b(1024), z(1024, 0.0f), l(1024);
  for (uint32_t i = 0; i < n; i += 1024) {
    const uint32_t k = std::min<uint32_t>(1024, n - i);
    for (uint32_t j = 0; j < k; ++j) {
      const double t = static_cast<double>(at + j) / kSr;
      b[j] = static_cast<float>(t * 2.0);
      l[j] = static_cast<float>(sig(t));
    }
    const float* ch[4] = {b.data(), z.data(), l.data(), l.data()};
    tap.write(ch, 4, k);
    at += k;
  }
}

// The same through `feed`, stepped every 4096 frames (as its worker reads the tap: it never falls behind the tap's ring).
void stream(TapRing& tap, SpectrumFeed& feed, double seconds, double (*sig)(double), uint64_t& at, double& clock) {
  double left = seconds;
  while (left > 1e-9) {
    const double chunk = std::min(left, 4096.0 / kSr);
    writeGram(tap, chunk, sig, at);
    feed.step(clock += chunk);
    left -= chunk;
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
  const meta::Type* targetType() const noexcept override { return &meta::spectrogramType(); }
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

struct Gpu {
  std::unique_ptr<GpuContext> ctx;
  std::unique_ptr<Renderer> renderer;
  static Gpu& get() {
    static Gpu g = [] {
      Gpu x;
      GpuOptions o;
      o.validation = true;
      auto c = GpuContext::create(o);
      REQUIRE_MESSAGE(c.ok(), (c.ok() ? "" : c.error().message));
      x.ctx = std::move(*c);
      auto r = Renderer::create(*x.ctx, OffscreenTarget::kFormat, 1);
      REQUIRE(r.ok());
      x.renderer = std::move(*r);
      return x;
    }();
    return g;
  }
};

struct FaceRig {
  TapRing tap{4, 32768, 4096};
  SpectrumFeed feed{tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{}};
  std::unique_ptr<MetaFace> face = makeFace(meta::spectrogramType());
  std::unique_ptr<Theme> theme;
  DrawList dl;
  TexturePool* pool = nullptr;
  int wakes = 0;
  uint64_t at = 0;
  double step = 1.0;
  float scale = 2.0f;
  explicit FaceRig(std::unique_ptr<MetaTarget> target = nullptr, TexturePool* images = nullptr) : pool(images) {
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
    FacePainter p(dl, {{10.0f, 10.0f}, scale});
    p.setImages(pool);
    face->paintLive(p, env);
  }
  GramShown play(double seconds, double (*sig)(double) = sine1k) {
    stream(tap, feed, seconds, sig, at, step);
    Clock::now() += 1.0 / 60.0;
    (void)face->pollVoices();
    paint();
    GramShown s;
    REQUIRE(gramShown(*face, s));
    return s;
  }
};

uint32_t luma(Rgba c) { return redOf(c) + greenOf(c) + blueOf(c); }

// The texel row the analysis row of `hz` falls in (the face's reduce: row r covers texel y = r · texRows / 1024).
uint32_t texelRowOf(float hz, uint32_t texRows, float fMax) {
  const float r = (1.0f - dsp::gramUnit(dsp::GramScale::Log, hz, 0.0f, fMax)) * 1023.0f;
  return static_cast<uint32_t>(std::lround(r)) * texRows / 1024u;
}

}  // namespace

TEST_CASE("spectrogram feed: columns without any spectrum, the largest request decides, settles on silence, wakes") {
  TapRing tap{4, 32768, 4096};
  SpectrumFeed feed(tap, AnalysisHub::config(kSr), SpectrumFeed::Manual{});
  int posts = 0;
  feed.setWatch(true);
  feed.setWakePost([&] { ++posts; });
  SpectrogramRequest small, big;
  small.config.hop = 1000;
  small.columns = 128;
  big.config.hop = 500;
  big.columns = 256;
  int keyA = 0, keyB = 0;
  feed.setSpectrogram(&keyA, small);
  feed.acquire(kWantSpectrogram);
  CHECK(tap.wanted());
  uint64_t at = 0;
  double clock = 1.0;
  stream(tap, feed, 1.0, sine1k, at, clock);
  GramState st = feed.gramState();
  REQUIRE(st.running);
  CHECK(st.hop == 1000);
  CHECK(feed.hopsAnalysed() == 0);  // a spectrogram-only consumer: no spectrum
  CHECK(st.end >= 40);
  // A second display with more columns decides; its removal gives the first one back.
  feed.setSpectrogram(&keyB, big);
  stream(tap, feed, 0.5, sine1k, at, clock);
  CHECK(feed.gramState().hop == 500);
  const uint64_t before = feed.gramState().end;
  feed.clearSpectrogram(&keyB);
  stream(tap, feed, 0.5, sine1k, at, clock);
  st = feed.gramState();
  CHECK(st.hop == 1000);
  CHECK(st.end > before);  // the sequence continued through both changes
  // Reads: what is held, its centres ascending, a bright row on the sine's.
  std::vector<float> rows(4 * SpectrumFeed::kGramRows);
  std::vector<GramColumn> info(4);
  REQUIRE(feed.gramRead(st.end - 4, 4, rows.data(), info.data()) == 4);
  CHECK(info[1].centre > info[0].centre);
  CHECK(info[2].peak > 0.1f);  // −6 dB
  CHECK(feed.gramRead(st.end, 4, rows.data(), info.data()) == 0);  // not yet made
  // Silence: settles once the display's columns (128) of silence have passed; sound wakes it.
  stream(tap, feed, 0.2, silence, at, clock);
  CHECK_FALSE(feed.gramState().settled);
  stream(tap, feed, 3.0, silence, at, clock);
  CHECK(feed.gramState().settled);
  stream(tap, feed, 0.5, silence, at, clock);
  CHECK(posts == 0);
  stream(tap, feed, 0.1, sine1k, at, clock);
  CHECK_FALSE(feed.gramState().settled);
  CHECK(posts == 1);
  feed.release(kWantSpectrogram);
  CHECK_FALSE(tap.wanted());
}

TEST_CASE("spectrogram face: held while painted, a sine on its texel rows, let go when hidden") {
  FaceRig rig;
  rig.paint();  // shown: acquires and asks for its columns
  GramShown s;
  REQUIRE(gramShown(*rig.face, s));
  CHECK(s.acquired);
  CHECK(s.cols == 576);       // 284 world units at 2x: 568 px, in 32-column steps
  CHECK(s.texRows == 320);    // 154 world units at 2x: 308 px, rounded up to 64
  CHECK(s.texCols == 576 + 16);
  CHECK(s.request.columns == 576);
  CHECK(s.request.config.hop == 500);  // 6 s across 576 columns at 48 kHz
  CHECK_FALSE(s.texture);              // no GPU: the picture is coloured, not uploaded
  s = rig.play(2.0);
  CHECK(s.hop == 500);
  REQUIRE(s.read > 150);
  const uint32_t y = texelRowOf(1000.0f, s.texRows, 24000.0f);
  Rgba c = 0;
  float db = 0.0f;
  int row = -1;
  REQUIRE(gramTexel(*rig.face, s.read - 10, y, c, db, row));
  CHECK(row >= 0);
  CHECK(std::fabs(db + 6.02f) < 1.0f);  // the cached level: −6 dB (before tilt and gate)
  const uint32_t lit = luma(c);
  CHECK(lit > 300);
  // Far from the line the picture is dark.
  for (const int dy : {-40, -20, 20, 40}) {
    Rgba o = 0;
    REQUIRE(gramTexel(*rig.face, s.read - 10, static_cast<uint32_t>(static_cast<int>(y) + dy), o, db, row));
    CHECK(luma(o) < lit / 8);
  }
  // The servo's right edge follows the newest column.
  CHECK(s.display >= static_cast<double>(s.read) - 12.0);
  CHECK(s.display <= static_cast<double>(s.read) + 1.0);
  // Freeze: nothing new is read; unfreeze reads on.
  REQUIRE(gramTestControl(*rig.face, "freeze", 1.0));
  const uint64_t held = s.read;
  s = rig.play(0.5);
  CHECK(s.read == held);
  REQUIRE(gramTestControl(*rig.face, "freeze", 0.0));
  s = rig.play(0.1);
  CHECK(s.read > held);
  // Hidden: no paint for a while — after one unanswered repaint it lets go of the feed.
  Clock::now() += 1.0;
  CHECK(rig.face->pollVoices());  // the probe repaint
  Clock::now() += 0.1;
  CHECK_FALSE(rig.face->pollVoices());
  REQUIRE(gramShown(*rig.face, s));
  CHECK_FALSE(s.acquired);
  CHECK_FALSE(rig.tap.wanted());
}

TEST_CASE("spectrogram face: colours recolour from the cache, a new axis starts over, Loop places by the beat") {
  FaceRig rig;
  rig.paint();
  REQUIRE(gramTestControl(*rig.face, "gate", 0.0));  // a fixed mapping for the comparisons
  GramShown s = rig.play(1.5, chord);
  const uint32_t y = texelRowOf(220.0f, s.texRows, 24000.0f);
  const uint64_t col = s.read - 8;
  Rgba magma = 0, inferno = 0, raised = 0;
  float db = 0.0f;
  int row = -1;
  REQUIRE(gramTexel(*rig.face, col, y, magma, db, row));
  const float level = db;
  const uint64_t requests = s.request.columns > 0 ? s.requests : 0;
  REQUIRE(gramTestControl(*rig.face, "map", 1.0));  // Inferno: recoloured, not re-analysed
  rig.paint();
  REQUIRE(gramTexel(*rig.face, col, y, inferno, db, row));
  CHECK(inferno != magma);
  CHECK(db == level);
  REQUIRE(gramShown(*rig.face, s));
  CHECK(s.requests == requests);  // the analysis request did not change
  REQUIRE(gramTestControl(*rig.face, "ceil", -40.0));  // a lower ceiling: brighter
  rig.paint();
  REQUIRE(gramTexel(*rig.face, col, y, raised, db, row));
  CHECK(luma(raised) >= luma(inferno));
  // A new axis: the request changes, the feed's mapping moves on and the picture starts over.
  const uint32_t mapping = s.mapping;
  REQUIRE(gramTestControl(*rig.face, "scale", 1.0));  // mel
  s = rig.play(0.3, chord);
  CHECK(s.request.config.scale == dsp::GramScale::Mel);
  CHECK(s.mapping == mapping + 1);
  REQUIRE(gramTexel(*rig.face, col, y, raised, db, row));
  CHECK(row == -1);  // the old column was cleared
  // Loop over one bar: a column lands on the slot of its beat's phase (120 bpm: a bar is 2 s).
  REQUIRE(gramTestControl(*rig.face, "scale", 0.0));
  REQUIRE(gramTestControl(*rig.face, "ph", 1.0));
  s = rig.play(1.0, chord);
  CHECK(s.loop);
  REQUIRE(s.loopHead >= 0);
  // The newest column's centre is about half a window plus the reach behind the newest sample; its beat (the tap's)
  // places it: the slot is the bar phase of a time a little before `at`.
  const double tEnd = static_cast<double>(rig.at) / kSr;
  const double phase = std::fmod(tEnd * 2.0, 4.0) / 4.0;
  const double slot = phase * s.cols;
  CHECK(static_cast<double>(s.loopHead) <= slot + 1.0);
  CHECK(static_cast<double>(s.loopHead) >= slot - 0.15 * kSr / s.hop - 2.0);
  // The hop follows the tempo: one bar across the columns (2 s / 576 columns at 48 kHz ≈ 167).
  s = rig.play(0.2, chord);
  CHECK(s.request.config.hop == doctest::Approx(2.0 * kSr / s.cols).epsilon(0.03));
  Rgba lit = 0;
  REQUIRE(gramSlotTexel(*rig.face, static_cast<uint32_t>(s.loopHead), y, lit));
  CHECK(luma(lit) > 100);
}

TEST_CASE("spectrogram face: options are the instance's data — one step per change, read back by a new face") {
  auto target = std::make_unique<DataTarget>();
  DataTarget* t = target.get();
  FaceRig rig(std::move(target));
  rig.paint();
  REQUIRE(gramTestControl(*rig.face, "map", 4.0));
  REQUIRE(gramTestControl(*rig.face, "span", 3.0));
  CHECK(t->steps == 2);
  REQUIRE(t->data_ != nullptr);
  CHECK(meta::spectrogramType().dataProblem(*t->data_).empty());
  const meta::DataValue* f = t->data_->field("span");
  REQUIRE(f != nullptr);
  CHECK(f->number == doctest::Approx(3.0));
  CHECK(t->data_->field("ch") == nullptr);  // defaults are not written
  GramShown s = rig.play(0.2);
  CHECK(s.request.config.hop == 250);  // 3 s across 576 columns
  // Another face reads them back.
  auto kept = std::make_unique<DataTarget>();
  kept->data_ = t->data_;
  FaceRig rig2(std::move(kept));
  rig2.paint();
  REQUIRE(gramShown(*rig2.face, s));
  CHECK(s.request.config.hop == 250);
  // A sheet and the keyboard paint (no crash at any option).
  REQUIRE(gramTestControl(*rig2.face, "keys", 2.0));
  REQUIRE(gramTestControl(*rig2.face, "tune", 1.0));
  REQUIRE(gramTestHover(*rig2.face, {150.0f, 80.0f}));
  rig2.paint();
  REQUIRE(gramTestControl(*rig2.face, "sheet", 1.0));
  rig2.paint();
}

TEST_CASE("spectrogram face: the ring texture — created once, then only the new columns are uploaded per frame") {
  Gpu& g = Gpu::get();
  TexturePool* pool = g.renderer->images().get();
  REQUIRE(pool != nullptr);
  FaceRig rig(nullptr, pool);
  rig.paint();
  REQUIRE(gramTestControl(*rig.face, "gate", 0.0));  // the gate's floor estimate recolours history now and then
  GramShown s = rig.play(1.0, chord);
  REQUIRE(s.texture);
  const uint64_t full = static_cast<uint64_t>(s.texCols) * s.texRows * 4;
  CHECK(s.uploadBytes >= full);  // the first paint uploads the whole ring
  const uint32_t live = g.renderer->liveTextures();
  uint64_t bytes = s.uploadBytes, cols = s.read, frames = 0;
  for (int i = 0; i < 60; ++i) {
    s = rig.play(1.0 / 60.0, chord);
    ++frames;
  }
  const uint64_t perFrame = (s.uploadBytes - bytes) / frames;
  const double colsPerFrame = static_cast<double>(s.read - cols) / static_cast<double>(frames);
  MESSAGE("per frame: " << colsPerFrame << " new columns, " << perFrame << " bytes uploaded (" << s.texRows << " rows)");
  // Each new column and the blank slot after it: (columns + 1) · rows · 4 bytes, nothing else.
  CHECK(static_cast<double>(perFrame) <= (colsPerFrame + 1.5) * s.texRows * 4.0);
  CHECK(g.renderer->liveTextures() == live);  // no texture was made again
  // With the gate on, the floor estimate recolours what changed only.
  REQUIRE(gramTestControl(*rig.face, "gate", 1.0));
  bytes = s.uploadBytes;
  for (int i = 0; i < 60; ++i) s = rig.play(1.0 / 60.0, chord);
  MESSAGE("with the gate: " << (s.uploadBytes - bytes) / 60 << " bytes per frame");
}

TEST_CASE("spectrogram face: the scale lines are the track tuning's degrees (DocTarget::pitchGuides)") {
  FakeTransport port;
  ViewContext ctx{port};
  doc::DocumentStore& store = ctx.store();
  auto run = [&](doc::ops::EditResult e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  };
  doc::TuningDef edo22;
  edo22.system = *tuning::Edo::make(22);
  const doc::TuningId b = run(doc::ops::addMainTuning(*store.current(), store.ids(), "22edo", edo22, doc::Colour{0xE0A43C})).tunings.at(0);
  const doc::TrackId track =
      run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "A", doc::Colour{0x3FA9F5})).tracks.at(0);
  const doc::DeviceId dev = run(doc::ops::addMetaDevice(*store.current(), store.ids(), track, "spectrogram")).devices.at(0);
  DocTarget t(ctx, dev, "", meta::spectrogramType());
  std::vector<PitchGuide> g;
  // The project's first main tuning (12-EDO, C3 = 261.626 Hz): twelve degrees an octave, degree 0 on the Cs.
  t.pitchGuides(100.0f, 1000.0f, g);
  REQUIRE(g.size() == 40);  // 3.32 octaves: G#1 (103.8 Hz) … B4 (987.8 Hz) in C3 = 261.626 Hz naming
  for (size_t i = 1; i < g.size(); ++i) CHECK(g[i].hz / g[i - 1].hz == doctest::Approx(std::pow(2.0, 1.0 / 12.0)).epsilon(1e-5));
  const auto c3 = std::find_if(g.begin(), g.end(), [](const PitchGuide& p) { return p.degree == 0 && p.equave == 0; });
  REQUIRE(c3 != g.end());
  CHECK(c3->hz == doctest::Approx(261.626f).epsilon(1e-5));
  // The track in 22-EDO: the key changes, twenty-two degrees an octave.
  const uint64_t key = t.guidesKey();
  REQUIRE(store.dispatch(doc::SetTrackTuning{track, doc::TuningAssignment::mainTuning(b)}).ok());
  CHECK(t.guidesKey() != key);
  t.pitchGuides(261.0f, 523.0f, g);
  REQUIRE(g.size() == 22);
  CHECK(g[1].hz / g[0].hz == doctest::Approx(std::pow(2.0, 1.0 / 22.0)).epsilon(1e-5));
}

TEST_CASE("render: device-meta-spectrogram matches the golden image") { checkScene("device-meta-spectrogram"); }

TEST_CASE("render: project-meta-spectrogram matches the golden image") { checkScene("project-meta-spectrogram"); }

TEST_CASE("spectrogram displays: the UI frame of 4 spectrograms (poll + colour + upload + paint)" * doctest::test_suite("perf")) {
  Gpu& g = Gpu::get();
  std::vector<std::unique_ptr<FaceRig>> rigs;
  for (int i = 0; i < 4; ++i) {
    rigs.push_back(std::make_unique<FaceRig>(nullptr, g.renderer->images().get()));
    rigs.back()->scale = 2.0f;
    rigs.back()->paint();
    (void)rigs.back()->play(1.0, chord);
  }
  REQUIRE(gramTestControl(*rigs[1]->face, "keys", 1.0));
  REQUIRE(gramTestControl(*rigs[2]->face, "ph", 1.0));
  REQUIRE(gramTestControl(*rigs[3]->face, "src", 1.0));
  DrawList dl;
  auto theme = loadTheme(defaultThemePath());
  REQUIRE(theme.ok());
  auto frame = [&] {
    for (auto& r : rigs) {
      stream(r->tap, r->feed, 1.0 / 60.0, chord, r->at, r->step);  // the analysis thread's side (not timed)
    }
    Clock::now() += 1.0 / 60.0;
    const auto t0 = std::chrono::steady_clock::now();
    dl.reset(2560, 1440);
    for (size_t i = 0; i < rigs.size(); ++i) {
      (void)rigs[i]->face->pollVoices();
      FaceEnv env;
      env.theme = &*theme;
      env.uiScale = 1.0f;
      FacePainter p(dl, {{20.0f + 620.0f * static_cast<float>(i % 2), 20.0f + 400.0f * static_cast<float>(i / 2)}, 2.0f});
      p.setImages(g.renderer->images().get());
      rigs[i]->face->paintLive(p, env);  // a 300 × 170 face at 2x: 600 × 340 device px
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  };
  auto run = [&](const char* what) {
    for (int i = 0; i < 20; ++i) frame();
    std::vector<double> ms;
    for (int i = 0; i < 200; ++i) ms.push_back(frame());
    std::sort(ms.begin(), ms.end());
    MESSAGE("4 spectrograms (keyboard, loop, colour by frequency), " << std::string(what) << ", poll + colour + upload + paint: median "
                                                                     << ms[100] << " ms, p99 " << ms[198] << " ms");
    return ms[100];
  };
  const double gated = run("gate on");
  for (auto& r : rigs) REQUIRE(gramTestControl(*r->face, "gate", 0.0));
  const double plain = run("gate off");
#ifdef PQ_OPTIMIZED
  CHECK(gated < 4.0);
  CHECK(plain < 4.0);
#endif
}
