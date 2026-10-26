// T4.6 device panels (+ the Utility / Saturator / EQ Eight panels of T4.9 / T4.10): panel descriptions against the
// stock devices, the generated panel, value mapping, the displays' math against the DSP references, edits reaching the
// target (params as writes, consts as recompiles through the editor's path), a param change damaging only its control,
// the spectrum costing nothing while hidden, idle after settling, and the goldens.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "../stdlib/device_reference.h"
#include "../ui/golden_check.h"
#include "../../src/views/src/devices/device_math.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/circuit_viewer.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/devices/device_panel.h"
#include "pq/views/devices/device_view.h"
#include "pq/views/view_scenes.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::devices;
namespace fs = std::filesystem;

namespace {

std::string stdlibPath(const char* rel) { return (fs::path(PQ_TEST_DATA_DIR) / ".." / "stdlib" / rel).string(); }

double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

const graph::Patch& devicePatch(const std::shared_ptr<const circuit::PatchDocument>& doc, std::string_view name) {
  const graph::Patch* p = doc->parsed.file.find(name);
  REQUIRE(p != nullptr);
  return *p;
}

struct Device {
  std::shared_ptr<const circuit::PatchDocument> doc;
  DeviceSignature sig;
  PanelDesc desc;
  explicit Device(const char* name) {
    const std::string path = stdlibPath((std::string("devices/") + name + ".pqp").c_str());
    auto d = circuit::loadPatchDocument(path);
    REQUIRE_MESSAGE(d.ok(), (d.ok() ? "" : d.error().message));
    doc = *d;
    sig = signatureOf(devicePatch(doc, name));
    auto p = loadPanelDesc(panelPathFor(path), sig);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? "" : p.error().message));
    desc = std::move(*p);
  }
};

// Records what the panel sends.
struct FakeTarget final : DeviceTarget {
  struct Write {
    std::string name;
    double value;
  };
  std::vector<Write> params, consts;
  int begins = 0, ends = 0;
  std::vector<bool> active;
  SpectrumFeed* feed = nullptr;
  void paramGestureBegin(std::string_view) override { ++begins; }
  void setParam(std::string_view p, double v) override { params.push_back({std::string(p), v}); }
  void paramGestureEnd(std::string_view) override { ++ends; }
  void setConst(std::string_view n, double v) override { consts.push_back({std::string(n), v}); }
  void setActive(bool on) override { active.push_back(on); }
  SpectrumFeed* outputSpectrum() override { return feed; }
};

// A panel alone in a tree (with an overlay for choosers).
struct PanelRig {
  TestDriver d{1000, 400};
  DevicePanel* panel = nullptr;
  PanelRig(const Device& dev, DeviceTarget* target, bool gpu = false) {
    if (gpu) REQUIRE(d.enableGpu().ok());
    auto& root = d.setRoot<Widget>("root");
    root.setLayout(LayoutKind::Stack);
    auto& host = root.add<Widget>("host");
    host.setLayout(LayoutKind::Row);
    panel = &host.add<DevicePanel>("device", dev.sig, dev.desc, target);
    panel->setHeight(Length::fixed(panel->logicalHeight()));
    (void)root.add<OverlayLayer>("overlay");
    d.tree().layoutNow();
    d.step();
  }
  // The damage rects of the next frame (after update), without painting.
  std::vector<IRect> damage() {
    d.tree().update(d.now());
    IRect rs[Damage::kMaxRects];
    int64_t area = 0;
    const uint32_t n = d.tree().damageRects(rs, area);
    return std::vector<IRect>(rs, rs + n);
  }
  Vec2 centre(const Widget& w) {
    const IRect r = w.rect();
    return {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)};
  }
};

bool inside(const IRect& a, const IRect& outer) {
  return a.x >= outer.x && a.y >= outer.y && a.right() <= outer.right() && a.bottom() <= outer.bottom();
}

// A player that records what the preview asks of it (no audio).
struct FakePlayer final : circuit::PatchPlayer {
  int plays = 0, stops = 0;
  bool on = false;
  std::vector<std::pair<std::string, float>> writes;
  std::vector<std::string> paramNames;  // of the last schedule
  double sampleRate() const override { return 48000.0; }
  Status play(const graph::Schedule& s, const graph::TypedPatch& tp) override {
    ++plays;
    on = true;
    paramNames.clear();
    for (const graph::ParamSlot& p : s.params) paramNames.push_back(tp.patch.nodeName(p.node));
    return Ok{};
  }
  void stop() override {
    ++stops;
    on = false;
  }
  bool playing() const override { return on; }
  const graph::StateLayout* stateLayout() const override { return nullptr; }
  bool setParam(std::string_view name, float v) override {
    writes.emplace_back(std::string(name), v);
    return true;
  }
};

void checkGolden(const char* scene, const std::string& file) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, file, w, h, UiScale(1.0f), o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  auto ref = readPng(std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png");
  if (!ref) (void)writePng(std::string(scene) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &d));
  MESSAGE(std::string(scene) << ": " << pq::ui::test::describe(d));
}

}  // namespace

// ---- descriptions -----------------------------------------------------------------------------------------------------

TEST_CASE("device panels: the stock panel descriptions bind to their devices") {
  Device u("utility"), s("saturator"), e("eq_eight");
  CHECK(u.desc.title == "Utility");
  CHECK(s.desc.title == "Saturator");
  CHECK(e.desc.title == "EQ Eight");
  // Every visible param of Utility and Saturator has a control.
  for (const Device* d : {&u, &s})
    for (const ParamInfo& p : d->sig.params) {
      bool found = false;
      for (const ControlDesc& c : d->desc.controls) found = found || c.param == p.name || c.bind.count(p.name) || c.bind.count("x");
      for (const ControlDesc& c : d->desc.controls)
        for (const auto& [role, name] : c.bind) found = found || name == p.name;
      CHECK_MESSAGE(found, d->sig.patch << ": no control for " << p.name);
    }
  // Structural settings are bound as consts (QUESTIONS Q121).
  CHECK(s.sig.findConst("type") != nullptr);
  CHECK(s.sig.findConst("hq") != nullptr);
  CHECK(u.sig.findConst("bassMono") != nullptr);
  CHECK(e.sig.findConst("typeB8") != nullptr);
  // EQ Eight: the repeat expanded to eight band toggles and choosers; the knobs bind through the band / A-B selectors.
  int toggles = 0, choosers = 0;
  for (const ControlDesc& c : e.desc.controls) {
    toggles += c.kind == ControlKind::Toggle && c.constName.rfind("on{ab}", 0) == 0;
    choosers += c.kind == ControlKind::Chooser && c.constName.rfind("type{ab}", 0) == 0;
  }
  CHECK(toggles == 8);
  CHECK(choosers == 8);
  // Exports become jacks.
  CHECK(u.sig.exports == std::vector<std::string>{"mid", "side", "bass"});
}

TEST_CASE("device panels: a description that names a missing param, const or selector fails with the control") {
  Device u("utility");
  auto bad = parsePanelDesc(R"({"format": "pqdaw-panel 1", "patch": "utility", "title": "U", "size": [100, 100],
    "controls": [{"type": "knob", "param": "gian", "rect": [0, 0, 50, 50]}]})", u.sig);
  REQUIRE_FALSE(bad.ok());
  CHECK(bad.error().message.find("gian") != std::string::npos);
  auto badConst = parsePanelDesc(R"({"format": "pqdaw-panel 1", "patch": "utility", "title": "U", "size": [100, 100],
    "controls": [{"type": "toggle", "const": "gain", "rect": [0, 0, 50, 20]}]})", u.sig);
  REQUIRE_FALSE(badConst.ok());
  CHECK(badConst.error().message.find("structural const") != std::string::npos);
  auto badSel = parsePanelDesc(R"({"format": "pqdaw-panel 1", "patch": "utility", "title": "U", "size": [100, 100],
    "controls": [{"type": "knob", "param": "{nope}", "rect": [0, 0, 50, 50]}]})", u.sig);
  REQUIRE_FALSE(badSel.ok());
  CHECK(badSel.error().message.find("not a selector") != std::string::npos);
  auto unknownKey = parsePanelDesc(R"({"format": "pqdaw-panel 1", "patch": "utility", "title": "U", "size": [100, 100],
    "controls": [{"type": "knob", "param": "gain", "rect": [0, 0, 50, 50], "colour": 1}]})", u.sig);
  REQUIRE_FALSE(unknownKey.ok());
  CHECK(unknownKey.error().message.find("colour") != std::string::npos);
  auto wrongPatch = parsePanelDesc(R"({"format": "pqdaw-panel 1", "patch": "saturator", "title": "U", "size": [100, 100],
    "controls": []})", u.sig);
  CHECK_FALSE(wrongPatch.ok());
}

TEST_CASE("device panels: a patch without a description gets one control per param (auto panel)") {
  auto doc = circuit::loadPatchDocument(stdlibPath("filters/svf.pqp"));
  REQUIRE(doc.ok());
  const DeviceSignature sig = signatureOf(devicePatch(*doc, "svf"));
  REQUIRE(!sig.params.empty());
  const PanelDesc p = autoPanel(sig, "SVF");
  CHECK(p.generated);
  size_t visible = 0;
  for (const ParamInfo& prm : sig.params) visible += prm.hidden ? 0 : 1;
  CHECK(p.controls.size() == visible + sig.consts.size());
  for (const ControlDesc& c : p.controls) {
    CHECK(c.x + c.w <= p.width + 1.0f);
    CHECK(c.y + c.h <= p.height + 1.0f);
  }
  FakeTarget t;
  Device holder("utility");  // only for the rig's signature type; build the auto panel directly
  TestDriver d(800, 300);
  auto& root = d.setRoot<Widget>("root");
  root.setLayout(LayoutKind::Row);
  auto& panel = root.add<DevicePanel>("device", sig, p, &t);
  d.tree().layoutNow();
  for (const ParamInfo& prm : sig.params)
    if (!prm.hidden) CHECK_MESSAGE(panel.controlFor(prm.name) != nullptr, prm.name);
}

// ---- value mapping ------------------------------------------------------------------------------------------------------

TEST_CASE("device panels: knob positions, display text and typed values") {
  Device s("saturator"), e("eq_eight"), u("utility");
  const ParamInfo& drive = *s.sig.findParam("drive");
  CHECK(drive.unit == DisplayUnit::Db);
  CHECK(toPosition(drive, 1.0) == doctest::Approx(0.5));  // 0 dB in −36 … 36 dB
  CHECK(fromPosition(drive, 0.75) == doctest::Approx(dbToLin(18.0)));
  CHECK(formatValue(drive, dbToLin(-6.0)) == "-6.0 dB");
  CHECK(*parseValue(drive, "12") == doctest::Approx(dbToLin(12.0)));
  CHECK(*parseValue(drive, "-3 dB") == doctest::Approx(dbToLin(-3.0)));
  const ParamInfo& f = *e.sig.findParam("freq4");
  CHECK(f.exp);
  CHECK(fromPosition(f, toPosition(f, 1000.0)) == doctest::Approx(1000.0));
  CHECK(toPosition(f, std::sqrt(10.0 * 22000.0)) == doctest::Approx(0.5));  // log midpoint
  CHECK(formatValue(f, 1000.0) == "1.00 kHz");
  CHECK(formatValue(f, 120.0) == "120 Hz");
  CHECK(formatValue(f, 15000.0) == "15.0 kHz");
  CHECK(*parseValue(f, "2.5k") == doctest::Approx(2500.0));
  CHECK(*parseValue(f, "440 Hz") == doctest::Approx(440.0));
  CHECK_FALSE(parseValue(f, "3 dB").has_value());
  const ParamInfo& gain = *u.sig.findParam("gain");
  CHECK(formatValue(gain, gain.min) == "-inf dB");
  CHECK(*parseValue(gain, "-inf") == doctest::Approx(gain.min));
  const ParamInfo& width = *u.sig.findParam("width");
  CHECK(width.unit == DisplayUnit::Percent);
  CHECK(formatValue(width, 1.2) == "120 %");
  CHECK(*parseValue(width, "50%") == doctest::Approx(0.5));
  const ParamInfo& chan = *u.sig.findParam("chan");
  CHECK(chan.steps == 4);
  CHECK(fromPosition(chan, 0.4) == 1.0);  // stepped: whole values
}

// ---- display math against the DSP references (stdlib/devices *.pqp ↔ tests/stdlib/device_reference.h) ----------------

TEST_CASE("device panels: the Saturator curve display follows the device's curves") {
  for (int type = 0; type <= 8; ++type) {
    math::SaturatorState s;
    s.type = type;
    s.wsDrive = 0.7;
    s.wsCurve = 0.4;
    s.wsDepth = 0.3;
    s.wsPeriod = 0.2;
    s.wsDamp = 0.5;
    devref::SatParams p;
    p.wsDrive = 0.7f;
    p.wsCurve = 0.4f;
    p.wsDepth = 0.3f;
    p.wsPeriod = 0.2f;
    p.wsDamp = 0.5f;
    for (double v = -3.0; v <= 3.0; v += 0.01) {
      const double a = math::saturatorCurve(s, v), b = devref::satCurve(type, static_cast<float>(v), p);
      CHECK_MESSAGE(std::fabs(a - b) < 2e-5, "type " << type << " at " << v << ": " << a << " vs " << b);
    }
  }
  math::SaturatorState s;
  s.drive = dbToLin(12.0);
  s.output = 0.5;
  s.softClip = true;
  s.dryWet = 0.25;
  const double x = 0.3, u = math::saturatorCurve(s, x * s.drive) * s.output;
  CHECK(math::saturatorTransfer(s, x) == doctest::Approx(x + 0.25 * (std::tanh(u) - x)));
}

TEST_CASE("device panels: the EQ display's response is the bands' measured response (every type, 1x and 2x)") {
  const double sr = 48000.0;
  for (int os = 1; os <= 2; ++os)
    for (int type = 0; type < 8; ++type) {
      math::EqBand b;
      b.on = true;
      b.type = type;
      b.freq = 900.0;
      b.gain = dbToLin(type == math::kBell ? 7.0 : -5.0);
      b.q = 1.4;
      const math::EqCoef c = math::eqCoef(b, true, 0.8, sr, os);
      devref::EqBand rb{static_cast<float>(b.freq), static_cast<float>(b.gain), static_cast<float>(b.q)};
      devref::EqBandState st;
      st.prepare(type, devref::eqCoef(type, static_cast<float>(os), rb, true, 0.8f, static_cast<float>(sr)));
      for (double f : {120.0, 600.0, 900.0, 1400.0, 5000.0}) {
        // The band at os·sr on a sine (os samples per base sample, as the 2× bands run).
        const double fs = os * sr;
        double peak = 0.0;
        for (int i = 0; i < static_cast<int>(fs); ++i) {
          const float y = st.tick(type, static_cast<float>(std::sin(2.0 * math::kPi * f * i / fs)));
          if (i > static_cast<int>(fs) / 2) peak = std::max(peak, static_cast<double>(std::fabs(y)));
        }
        const double measured = 20.0 * std::log10(std::max(peak, 1e-9));
        const double shown = 20.0 * std::log10(std::abs(math::eqBandResponse(b, c, f, sr, os)));
        if (shown < -50.0) continue;  // deep in a cut: below what the display needs
        CHECK_MESSAGE(std::fabs(measured - shown) < 0.05, "type " << type << " os " << os << " f " << f << ": " << measured << " vs " << shown);
      }
    }
}

// ---- the panel: edits, damage, bindings ---------------------------------------------------------------------------------

TEST_CASE("device panels: a knob drag is one param gesture to the target and damages only that knob") {
  Device u("utility");
  FakeTarget t;
  PanelRig r(u, &t);
  Widget* gain = r.panel->controlFor("gain");
  REQUIRE(gain != nullptr);
  const Vec2 c = r.centre(*gain);
  r.d.move(c);
  r.d.step();
  r.d.down(c);
  r.d.step();
  r.d.move({c.x, c.y - 20.0f});
  const std::vector<IRect> dmg = r.damage();
  REQUIRE(!dmg.empty());
  for (const IRect& x : dmg) CHECK_MESSAGE(inside(x, gain->rect()), x.x << "," << x.y << " " << x.w << "x" << x.h);
  r.d.step();
  r.d.up({c.x, c.y - 20.0f});
  REQUIRE(!t.params.empty());
  CHECK(t.params.back().name == "gain");
  CHECK(t.params.back().value > 1.0);  // up = more
  CHECK(t.begins == 1);
  CHECK(t.ends == 1);
  CHECK(t.consts.empty());
  CHECK(r.panel->paramValue("gain") == doctest::Approx(t.params.back().value));
}

TEST_CASE("device panels: an external value change damages only the controls and displays that show it") {
  Device s("saturator"), u("utility");
  FakeTarget t;
  {
    PanelRig r(u, &t);
    r.panel->setParamValue("balance", 0.5);
    const std::vector<IRect> dmg = r.damage();
    REQUIRE(dmg.size() == 1);
    CHECK(dmg[0] == r.panel->controlFor("balance")->rect());
    r.d.step();
    CHECK(r.damage().empty());
  }
  PanelRig r(s, &t);
  Widget* curve = r.panel->control("transfer");
  REQUIRE(curve != nullptr);
  r.panel->setParamValue("drive", dbToLin(6.0));
  for (const IRect& x : r.damage())
    CHECK((inside(x, r.panel->controlFor("drive")->rect()) || inside(x, curve->rect())));
  r.d.step();
  r.panel->setParamValue("base", dbToLin(3.0));  // the colour EQ: not part of the static curve
  const std::vector<IRect> dmg = r.damage();
  REQUIRE(dmg.size() == 1);
  CHECK(dmg[0] == r.panel->controlFor("base")->rect());
  CHECK(t.params.empty());  // external changes are not echoed to the target
}

TEST_CASE("device panels: structural toggles go to the target as consts and change which controls show") {
  Device u("utility");
  FakeTarget t;
  PanelRig r(u, &t);
  CHECK(r.panel->controlFor("bassFreq") == nullptr);  // hidden while Bass Mono is off
  Widget* bass = r.panel->control("bassMono");
  REQUIRE(bass != nullptr);
  r.d.click(r.centre(*bass));
  r.d.step();
  REQUIRE(t.consts.size() == 1);
  CHECK(t.consts[0].name == "bassMono");
  CHECK(t.consts[0].value == 1.0);
  CHECK(r.panel->controlFor("bassFreq") != nullptr);
  // The Mid/Side param swaps the width knob for the mid/side knob (a param: written, not recompiled).
  Widget* ms = r.panel->control("msMode");
  REQUIRE(ms != nullptr);
  CHECK(r.panel->controlFor("width") != nullptr);
  r.d.click(r.centre(*ms));
  r.d.step();
  CHECK(r.panel->controlFor("width") == nullptr);
  CHECK(r.panel->controlFor("ms") != nullptr);
  CHECK(t.params.back().name == "msMode");
  CHECK(t.consts.size() == 1);
  // The activator.
  r.panel->setActive(true);
  Widget* title = r.panel->find("title");
  REQUIRE(title != nullptr);
  const IRect tr = title->rect();
  r.d.click({static_cast<float>(tr.x + 10), static_cast<float>(tr.y + tr.h / 2)});
  REQUIRE(t.active.size() == 1);
  CHECK_FALSE(t.active[0]);
  CHECK_FALSE(r.panel->active());
}

TEST_CASE("device panels: EQ Eight's band selection rebinds the knobs; a dot drag edits freq and gain") {
  Device e("eq_eight");
  FakeTarget t;
  PanelRig r(e, &t, true);  // painted with the theme (dots are placed in paint)
  auto* freq = static_cast<ValueControl*>(r.panel->control("freq"));
  REQUIRE(freq != nullptr);
  CHECK(r.panel->controlFor("freq4") == freq);  // default selection: band 4
  r.panel->select("band", 1);
  CHECK(r.panel->controlFor("freq2") == freq);
  CHECK(r.panel->controlFor("freq4") == nullptr);
  // A/B edit only in L/R or M/S.
  CHECK(r.panel->control("edit") != nullptr);
  CHECK_FALSE(r.panel->control("edit")->visible());
  r.panel->setConstValue("mode", 1.0);
  r.d.step();
  CHECK(r.panel->control("edit")->visible());
  r.panel->select("ab", 1);
  CHECK(r.panel->controlFor("freqB2") == freq);
  r.panel->select("ab", 0);
  // Drag band 4's dot (a bell): right = higher, up = more gain.
  r.panel->select("band", 3);
  r.d.step();
  Widget* eq = r.panel->control("eq");
  REQUIRE(eq != nullptr);
  // Band 4 sits at 1 kHz, 0 dB: find it on the display's log axis (10 Hz … 22 kHz across the inner rect).
  const IRect er = eq->rect();
  const float x0 = static_cast<float>(er.x + 1), x1 = static_cast<float>(er.right() - 1);
  const float x = x0 + (x1 - x0) * static_cast<float>(std::log(1000.0 / 10.0) / std::log(2200.0));
  const float y = static_cast<float>(er.y) + 0.5f * static_cast<float>(er.h);
  r.d.move({x, y});
  r.d.down({x, y});
  r.d.move({x + 30.0f, y - 15.0f});
  r.d.up({x + 30.0f, y - 15.0f});
  bool sawFreq = false, sawGain = false;
  for (const auto& w : t.params) {
    sawFreq = sawFreq || (w.name == "freq4" && w.value > 1000.0);
    sawGain = sawGain || (w.name == "gain4" && w.value > 1.0);
  }
  CHECK(sawFreq);
  CHECK(sawGain);
  CHECK(r.panel->paramValue("freq4") > 1000.0);
  CHECK(freq->value() > toPosition(*e.sig.findParam("freq4"), 1000.0));  // the knob follows the dot
}

// ---- spectrum: nothing while hidden; idle ------------------------------------------------------------------------------

TEST_CASE("device panels: the spectrum costs nothing while hidden and the panel idles once settled") {
  Device e("eq_eight");
  TapRing tap(2, 32768, 2048);
  dsp::SpectrumConfig sc;
  SpectrumFeed feed(tap, sc);  // the real worker thread
  FakeTarget t;
  t.feed = &feed;
  PanelRig r(e, &t, true);
  // Shown, source live: the display holds the feed, the tap has a reader, the worker runs.
  feed.setSourceLive(true);
  r.panel->wakeDisplays();
  r.d.advance(0.1);
  CHECK(feed.consumers() == 1);
  CHECK(tap.wanted());
  std::vector<float> blk(512, 0.25f);
  const float* ch[2] = {blk.data(), blk.data()};
  for (int i = 0; i < 40; ++i) tap.write(ch, 2, 512);
  for (int i = 0; i < 100 && feed.hopsAnalysed() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(feed.hopsAnalysed() > 0);
  // Hidden (the Device tab switched away): at the next frame the display lets go: no reader, the worker blocks.
  r.panel->setVisible(false);
  r.d.advance(0.05);
  CHECK(feed.consumers() == 0);
  CHECK_FALSE(tap.wanted());
  const uint64_t w0 = tap.written();
  tap.write(ch, 2, 512);  // the audio side: one load, no copy
  CHECK(tap.written() == w0);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const uint64_t wake0 = feed.wakeups();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(feed.wakeups() == wake0);
  // Shown again with the source stopped: it decays, then the tree idles (no frames, no timers).
  feed.setSourceLive(false);
  r.panel->setVisible(true);
  r.d.step();
  for (int i = 0; i < 400 && (r.d.tree().needsFrame() || feed.consumers() > 0); ++i) {
    r.d.advance(0.02);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK_FALSE(r.d.tree().needsFrame());
  CHECK(r.d.tree().nextWakeTime() == std::numeric_limits<double>::infinity());
  CHECK(feed.consumers() == 0);
}

// ---- the preview: params are writes, consts recompile through the editor --------------------------------------------------

TEST_CASE("device preview: param edits are param writes; structural edits re-check the host patch and hot swap") {
  FakePlayer player;
  auto pv = DevicePreview::open(stdlibPath("devices/saturator.pqp"), &player);
  REQUIRE_MESSAGE(pv.ok(), (pv.ok() ? "" : pv.error().message));
  DevicePreview& p = **pv;
  CHECK(p.panelDesc().title == "Saturator");
  CHECK(p.hostText().find("saturator(x: (pq_mono, pq_mono), type: 3, hq: 1") != std::string::npos);
  REQUIRE(p.play().ok());
  CHECK(player.plays == 1);
  CHECK(std::find(player.paramNames.begin(), player.paramNames.end(), "drive") != player.paramNames.end());
  p.setParam("drive", dbToLin(6.0));
  REQUIRE(player.writes.size() == 1);
  CHECK(player.writes[0].first == "drive");
  CHECK(player.plays == 1);  // no recompile
  p.setConst("type", 5.0);
  CHECK(player.plays == 2);  // the editor's check → the player's hot swap
  CHECK(p.hostText().find("type: 5") != std::string::npos);
  // The new host text carries the current param values as defaults (a fresh compile starts where the panel is).
  CHECK(p.editor().text().find("param drive = 6dB") != std::string::npos);
  p.setActive(false);  // bypass: structural
  CHECK(player.plays == 3);
  CHECK(p.hostText().find("saturator(") == std::string::npos);
  CHECK(p.stats().recompiles == 2);
  p.stop();
  CHECK_FALSE(p.playing());
  // Stopped: a structural edit only updates the text (no player call).
  p.setConst("hq", 4.0);
  CHECK(player.plays == 3);
}

TEST_CASE("device preview: every stock device's host patch compiles; a device without a description gets the auto panel") {
  for (const char* dev : {"utility", "saturator", "eq_eight"}) {
    FakePlayer player;
    auto pv = DevicePreview::open(stdlibPath((std::string("devices/") + dev + ".pqp").c_str()), &player);
    REQUIRE_MESSAGE(pv.ok(), (pv.ok() ? "" : pv.error().message));
    Status s = (*pv)->play();
    CHECK_MESSAGE(s.ok(), dev << ": " << (s.ok() ? "" : s.error().message));
    CHECK(player.plays == 1);
    CHECK_FALSE((*pv)->panelDesc().generated);
  }
  const std::vector<DeviceEntry> list = listDevices(stdlibPath("devices"));
  REQUIRE(list.size() >= 3);
  std::vector<std::string> titles;
  for (const DeviceEntry& d : list) titles.push_back(d.title);
  CHECK(std::find(titles.begin(), titles.end(), "EQ Eight") != titles.end());
  CHECK(std::find(titles.begin(), titles.end(), "Saturator") != titles.end());
  CHECK(std::find(titles.begin(), titles.end(), "Utility") != titles.end());
}

// ---- goldens ------------------------------------------------------------------------------------------------------------

TEST_CASE("device panels: golden of Utility") { checkGolden("device-utility", stdlibPath("devices/utility.pqp")); }
TEST_CASE("device panels: golden of Saturator") { checkGolden("device-saturator", stdlibPath("devices/saturator.pqp")); }
TEST_CASE("device panels: golden of EQ Eight") { checkGolden("device-eq-eight", stdlibPath("devices/eq_eight.pqp")); }
