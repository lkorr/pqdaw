// GU.4 (docs/plans/GU.md "Phases" 4): the Phase Accumulator and the VCA are group presets (pq/document/modules.h) —
// renders against the meta modules they replaced. tests/app/fixtures/gu4_modules/<case>/ holds a project with the
// presets and the interpreter render of the same project with the meta modules, recorded on the code before GU.4
// (04c59ffe + the recorder this file replaced, commit 7b3a94f2); GU.f7 re-saved the projects as the load conversion
// (deleted) made them: notes into a PA in an instrument group (pitch, amplitude, phase offset, frequency), a
// free-running PA with its pm and its (wrapping) phase modulated, a wire taking a PA's frequency below zero (the phase
// runs backwards: its curve is signed), the VCA's Log law at three positions, other ranges / zones / the Bend curve /
// an unwired VCA as a value source, and a VCA's knob automated and wired (Log: in position; Bend: in units).
//   renders     interpreter == JIT bit for bit; the Phase Accumulator cases equal their references bit for bit (the
//               preset is the expansion's atoms, operand for operand); the VCA cases within −110 dB of the reference's
//               peak: its law is a curve now (dsp::CurvePreset::Vca, the same function: tests/dsp/test_curve.cpp checks
//               it against the old law to 1e-15 of the range), evaluated in double on the UI thread and rounded once,
//               where the module computed it in float atoms (QUESTIONS Q?-GU.4-a); `mul` where it had the 2 ms gain
//   circuit     an opened VCA is In → mul ← Value → Out
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/devices.h"
#include "pq/document/library_files.h"
#include "pq/document/modules.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/engine/wav_file.h"

using namespace pq;
using namespace pq::doc;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kBlocks = 600;  // 0.8 s
const char* const kPaCases[] = {"pa_voice", "pa_free", "pa_signed"};
const char* const kVcaCases[] = {"vca_law", "vca_ranges", "vca_driven"};

fs::path caseDir(const std::string& name) { return fs::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / "gu4_modules" / name; }

const DeviceLibrary& library() {
  static const auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  return **lib;
}

Snapshot loadCase(const std::string& name) {
  LoadOptions lo;
  lo.devices = &library();
  auto r = loadProject(caseDir(name).string(), lo);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return r->project;
}

golden::Channels render(const Snapshot& s, bool jit) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
  c.sampleRate = kRate;
  auto l = app::ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  app::ProjectLink& link = **l;
  REQUIRE(link.waitForMetaTables());
  if (jit) REQUIRE(link.waitForJit(180.0));
  link.play(0);
  golden::Channels out(2);
  std::vector<float> L(kBlock), R(kBlock);
  int64_t t = 0;
  for (uint32_t b = 0; b < kBlocks; ++b) {
    float* ch[2] = {L.data(), R.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
    }
    t += kBlock;
    out[0].insert(out[0].end(), L.begin(), L.end());
    out[1].insert(out[1].end(), R.begin(), R.end());
  }
  CHECK(link.lastError().empty());
  return out;
}

double maxDiff(const golden::Channels& a, const golden::Channels& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = 0; i < a[c].size(); ++i) m = std::max(m, std::fabs(static_cast<double>(a[c][i]) - b[c][i]));
  }
  return m;
}
double peakOf(const golden::Channels& a) {
  double p = 0.0;
  for (const auto& c : a)
    for (const float v : c) p = std::max(p, static_cast<double>(std::fabs(v)));
  return p;
}
double db(double x) { return x > 0.0 ? 20.0 * std::log10(x) : -400.0; }

std::vector<const Device*> devicesOf(const Project& p, std::string_view patch) {
  std::vector<const Device*> out;
  forEachDevice(p, [&](const Device& d, const Track&) {
    if (d.patch == patch) out.push_back(&d);
  });
  return out;
}
const Device* byId(const Project& p, DeviceId id) { return findDevice(p, id).device; }
// The wires into `to`'s `param`.
std::vector<Wire> into(const Project& p, DeviceId to, std::string_view param) {
  std::vector<Wire> out;
  for (const Wire& w : wiresOf(p))
    if (w.to == to && !w.toMixer() && w.param == param) out.push_back(w);
  return out;
}
const Device* member(const Project& p, DeviceId group, std::string_view name) {
  const Device* out = nullptr;
  forEachDevice(p, [&](const Device& d, const Track&) {
    if (d.group == group && d.name == name && out == nullptr) out = &d;
  });
  return out;
}

}  // namespace

TEST_CASE("GU.4 modules: the presets render as the meta modules (PA diff 0, VCA ≤ −110 dB)") {
  for (const bool pa : {true, false})
    for (const char* n : pa ? std::span<const char* const>(kPaCases) : std::span<const char* const>(kVcaCases)) {
      const std::string name = n;
      CAPTURE(name);
      const Snapshot s = loadCase(name);
      CHECK(devicesOf(*s, "meta:phase_acc").empty());
      CHECK(devicesOf(*s, "meta:vca").empty());
      CHECK_FALSE(devicesOf(*s, std::string(kGroupDevice)).empty());
      auto ref = engine::readWav(caseDir(name) / "reference.wav");
      REQUIRE(ref.ok());
      const golden::Channels a = render(s, false), b = render(s, true);
      CHECK(maxDiff(a, b) == 0.0);
      const double d = maxDiff(ref->channels, a), peak = peakOf(ref->channels);
      MESSAGE(name << ": peak " << peak << ", error vs the meta module's render " << d << " (" << db(d / std::max(1.0, peak))
                   << " dB of the peak)");
      if (pa) CHECK(d == 0.0);
      else CHECK(db(d / std::max(1.0, peak)) <= -110.0);
    }
}

TEST_CASE("TX.detent: the Phase Accumulator's Amplitude curve — x1 at the centre (a detent, exactly 1.0), the PA cases "
          "render as before with it at its default") {
  const dsp::Curve amp = phaseAccumulatorCurve(kPaAmplitude);
  REQUIRE(dsp::validCurve(amp));
  CHECK(dsp::evalCurve(amp, 0.5) == 1.0);
  CHECK(dsp::curvePosition(amp, 1.0, 0.0) == 0.5);
  CHECK(dsp::compileCurve(amp).a[1] == 1.0f);  // what the curve atom reads there (a driven knob)
  CHECK(dsp::evalCurve(amp, 0.0) == 0.0);
  CHECK(dsp::evalCurve(amp, 1.0) == 4.0);
  std::array<double, dsp::kMaxCurveSegments + 1> d{};
  REQUIRE(dsp::curveDetents(amp, d) == 1);
  CHECK(d[0] == 0.5);
  CHECK(PhaseAccumulatorOptions{}.amp == 0.5);
  // The fixtures' Amplitude knobs at their old default (0.25 on 0 … 4) switched to the module's curve at 0.5: the
  // renders stay bit-identical to the meta module's.
  for (const char* n : kPaCases) {
    const std::string name = n;
    CAPTURE(name);
    DocumentStore store(loadCase(name));
    std::vector<DeviceId> moved;
    forEachDevice(*store.current(), [&](const Device& dv, const Track&) {
      const DeviceValue* v = dv.param(kValueParam);
      if (dv.name == kPaAmplitude && v != nullptr && v->value == 0.25) moved.push_back(dv.id);
    });
    REQUIRE_FALSE(moved.empty());
    for (const DeviceId id : moved) {
      REQUIRE(store.dispatch(SetParamCurve{id, std::string(kValueParam), amp, true}).ok());
      REQUIRE(store.dispatch(SetDeviceParam{id, std::string(kValueParam), 0.5}).ok());
    }
    auto ref = engine::readWav(caseDir(name) / "reference.wav");
    REQUIRE(ref.ok());
    CHECK(maxDiff(ref->channels, render(store.current(), false)) == 0.0);
  }
}

TEST_CASE("GU.4 modules: an opened VCA is In → mul ← Value → Out; the knob keeps its position, the curve is the law") {
  const Snapshot s = loadCase("vca_law");
  const std::vector<const Device*> groups = devicesOf(*s, std::string(kGroupDevice));
  REQUIRE(groups.size() == 4);  // the Phase Accumulator, three VCAs
  const Device* vca = byId(*s, DeviceId{9});  // the first VCA's id
  REQUIRE(vca != nullptr);
  CHECK(isGroupDevice(*vca));
  CHECK(vca->name == "VCA");
  CHECK(vca->hasPanel());
  const Device* mul = member(*s, vca->id, "×");
  const Device* value = member(*s, vca->id, kVcaValueName);
  REQUIRE(mul != nullptr);
  REQUIRE(value != nullptr);
  CHECK(mul->patch == "atom:mul");
  CHECK(isValueDevice(*value));
  // In → mul.a, Value.y → mul.b, mul.out → Out: nothing else inside.
  const std::vector<Wire> a = into(*s, mul->id, "a"), b = into(*s, mul->id, "b"), out = into(*s, vca->id, std::string(kGroupOutPort));
  REQUIRE(a.size() == 1);
  CHECK(a[0].from == vca->id);
  CHECK(a[0].port == kGroupInPort);
  REQUIRE(b.size() == 1);
  CHECK(b[0].from == value->id);
  CHECK(b[0].port == kValueOutput);
  REQUIRE(out.size() == 1);
  CHECK(out[0].from == mul->id);
  uint32_t members = 0;
  forEachDevice(*s, [&](const Device& d, const Track&) { members += d.group == vca->id ? 1 : 0; });
  CHECK(members == 2);
  // The knob: 0.62 as before; its curve the VCA's law (±20000, the default zone): × 0.6.
  REQUIRE(value->param(kValueParam) != nullptr);
  CHECK(value->param(kValueParam)->value == doctest::Approx(0.62).epsilon(1e-12));
  REQUIRE(value->curve(kValueParam) != nullptr);
  CHECK(value->curve(kValueParam)->preset == dsp::CurvePreset::Vca);
  CHECK(dsp::evalCurve(*value->curve(kValueParam), 0.62) == doctest::Approx(0.6).epsilon(1e-9));
  // The path: … → PA → VCA → VCA → VCA → mixer, the groups by their main ports, as the meta modules were.
  CHECK(into(*s, vca->id, "").size() == 1);
  // Saved again it is the same project, without the library.
  auto text = serializeProject(*s);
  REQUIRE(text.ok());
  auto back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == *s);
}

TEST_CASE("GU.4 modules: the built-in presets are valid") {
  // The built-in presets paste into a track and validate (the commands' checks), each with its panel.
  DocumentStore store;
  auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Audio);
  REQUIRE(t.ok());
  REQUIRE(store.dispatch(t->command).ok());
  for (const ModuleInfo& m : moduleInfos()) {
    CAPTURE(m.name);
    const BlockSet set = moduleBlocks(m.kind);
    REQUIRE_FALSE(set.empty());
    CHECK(isGroupDevice(*set.devices.front()));
    CHECK(set.devices.front()->hasPanel());
    auto e = ops::pasteBlocks(*store.current(), store.ids(), t->tracks.at(0), set);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE_MESSAGE(store.dispatch(e->command).ok(), m.name);
    CHECK(findModule(m.key) == &m);
    CHECK(findModule(m.name) == &m);
  }
}


