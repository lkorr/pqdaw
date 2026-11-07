// GU.1 (docs/plans/GU.md "Curves", Phases 1): controls own curves and store knob positions — renders against the
// lowering before GU.1. tests/app/fixtures/gu1_curves/<case>/ holds a schema-19 project (values in units) and its
// interpreter render recorded on the pre-GU.1 code (commit bdef3da9, the recorder this file replaced; automation_steps
// and switched re-recorded on that code with its recorder extended): the cases — undriven values off their defaults
// (a Phase accumulator, an Envelope inside the instrument group, Auto Filter, Utility), wires (an LFO through a VCA
// into an envelope time and a cutoff: curved, added in position; into a dB gain and a sustain: linear, added in
// units), automation lanes of holds and jumps (a cutoff and a gain), a macro mapping (Auto Filter's cutoff in an Audio
// Effect Rack) and switched curves (Exponential cutoff, Logarithmic dB gain, both wired).
//   references  each project loads through migration 19 → 20 (its values become knob positions through the library's
//               curves) and renders on the interpreter and the JIT: the two bit for bit, and within −120 dB of the
//               reference — dBFS, or of its peak where it is above full scale (switched: +13 dBFS; float rounding is
//               relative) — the curve atom's float arithmetic replaces the old per-curve atom chains (plan tolerance)
//   ramps       automation lanes hold knob POSITIONS now (plan: "automation and macros record knob positions"), so a
//               ramp between two breakpoints moves linearly in position, not in units: the `automation` case's lanes
//               (ramps) convert exactly at every breakpoint and play differently between them, by design
//   no op       an undriven control has no `curve` op in its unit's schedule (its value is the curve evaluated when it
//               changes, written as its param), inside the instrument group too; each driven control has one
//   live edits  under RtScope: a knob drag on an undriven control (one param write), a curve edit of the same class on
//               a driven one (a curve-slot publication, no compile) and on an undriven one (one write) all reach the
//               audio; a class change compiles
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/meta.h"
#include "pq/document/serialize.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/wav_file.h"

using namespace pq;
using namespace pq::doc;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kBlocks = 600;  // 0.8 s
const char* const kCases[] = {"undriven", "wires", "automation_steps", "switched"};  // (GU.9: "macro" went with the racks)

fs::path caseDir(const std::string& name) { return fs::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / "gu1_curves" / name; }

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
  CHECK(r->fileSchemaVersion == 19);
  return r->project;
}

std::unique_ptr<app::ProjectLink> makeLink(const Snapshot& s, bool jit) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
  c.sampleRate = kRate;
  auto l = app::ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  REQUIRE((*l)->waitForMetaTables());
  if (jit) REQUIRE((*l)->waitForJit(180.0));
  return std::move(l.value());
}

struct Driver {
  app::ProjectLink& link;
  int64_t t = 0;
  uint64_t violations = 0;
  golden::Channels out = golden::Channels(2);
  void run(uint32_t blocks) {
    std::vector<float> L(kBlock), R(kBlock);
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {L.data(), R.data()};
      violations += alloc_trap::countRtViolations([&] {
        DenormalGuard dg;
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
      });
      t += kBlock;
      out[0].insert(out[0].end(), L.begin(), L.end());
      out[1].insert(out[1].end(), R.begin(), R.end());
    }
  }
};

golden::Channels render(const Snapshot& s, bool jit) {
  auto link = makeLink(s, jit);
  link->play(0);
  Driver d{*link};
  d.run(kBlocks);
  CHECK(d.violations == 0);
  CHECK(link->lastError().empty());
  return d.out;
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

double dbfs(double x) { return x > 0.0 ? 20.0 * std::log10(x) : -400.0; }

// The `curve` kernel ops in the schedules of `p` lowered with the library (no link: slot 0, the IR is what matters).
uint32_t curveOps(const Project& p) {
  LowerOptions o;
  o.library = &library().library();
  o.devices = &library();
  o.slotOf = [](TrackId) { return 0u; };
  auto low = lower(p, o);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  co.voice.maxVoices = 8;
  engine::ProjectCompiler compiler(co);
  auto build = compiler.build(low->graph);
  REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
  uint32_t n = 0;
  for (uint32_t u = 0; u < compiler.numUnits(); ++u)
    for (const graph::Op& op : compiler.unit(u).schedule->ops)
      if (op.kind == graph::OpKind::Kernel && op.kernel == dsp::Atom::Curve) ++n;
  return n;
}

const Device* deviceOf(const Project& p, std::string_view patch) {
  const Device* out = nullptr;
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const DevicePtr& d : t.devices) {
      if (d->patch == patch && out == nullptr) out = d.get();
    }
  });
  return out;
}

}  // namespace

TEST_CASE("GU.1 curve renders: every case loads (19 → 20), interpreter == JIT, within −120 dBFS of the pre-GU.1 render") {
  for (const std::string name : kCases) {
    CAPTURE(name);
    const Snapshot s = loadCase(name);
    auto ref = engine::readWav(caseDir(name) / "reference.wav");
    REQUIRE(ref.ok());
    const golden::Channels a = render(s, false), b = render(s, true);
    CHECK(maxDiff(a, b) == 0.0);
    const double d = maxDiff(ref->channels, a);
    double peak = 0.0;
    for (const auto& c : ref->channels)
      for (const float v : c) peak = std::max(peak, static_cast<double>(std::fabs(v)));
    MESSAGE(name << ": peak error vs the pre-GU.1 render " << d << " (" << dbfs(d) << " dBFS; reference peak " << peak << ")");
    CHECK(dbfs(d / std::max(1.0, peak)) <= -120.0);
  }
}

TEST_CASE("GU.1 curve renders: a ramp's breakpoints convert exactly; between them the lane moves in position") {
  const Snapshot s = loadCase("automation");
  const Device* filter = deviceOf(*s, "auto_filter");
  REQUIRE(filter != nullptr);
  const graph::Patch* fp = library().patchOf(*filter);
  REQUIRE(fp != nullptr);
  const dsp::Curve c = controlCurve(*filter, "freq", *findParamDecl(*fp, "freq"));
  const AutomationLane* lane = nullptr;
  forEachTrack(*s, [&](const Track& t, const Track*) {
    for (const LanePtr& l : t.lanes)
      if (l->paramName == "freq") lane = l.get();
  });
  REQUIRE(lane != nullptr);
  REQUIRE(lane->points.size() == 2);
  CHECK(dsp::evalCurve(c, lane->points[0].value) == doctest::Approx(400.0).epsilon(1e-6));   // float breakpoints
  CHECK(dsp::evalCurve(c, lane->points[1].value) == doctest::Approx(6000.0).epsilon(1e-6));
  // Halfway along the ramp: the middle of the two positions — the geometric mean on the octave cutoff (√(400 · 6000)),
  // where the old lane passed the arithmetic one (3200 Hz).
  const double mid = 0.5 * (lane->points[0].value + lane->points[1].value);
  CHECK(dsp::evalCurve(c, mid) == doctest::Approx(std::sqrt(400.0 * 6000.0)).epsilon(1e-5));
  // It renders on both backends bit for bit.
  CHECK(maxDiff(render(s, false), render(s, true)) == 0.0);
}

TEST_CASE("GU.1 curve renders: the migration stores knob positions; the values they give are the old ones") {
  const Snapshot s = loadCase("undriven");
  const Device* filter = deviceOf(*s, "auto_filter");
  const Device* env = deviceOf(*s, "meta:envelope");
  REQUIRE(filter != nullptr);
  REQUIRE(env != nullptr);
  // Auto Filter's cutoff (20 Hz … 20 kHz, octave): 1800 Hz is the position log(90) / log(1000).
  REQUIRE(filter->param("freq") != nullptr);
  CHECK(filter->param("freq")->value == doctest::Approx(std::log(1800.0 / 20.0) / std::log(1000.0)).epsilon(1e-12));
  const graph::Patch* fp = library().patchOf(*filter);
  REQUIRE(fp != nullptr);
  CHECK(paramValue(*filter, "freq", *findParamDecl(*fp, "freq")) == doctest::Approx(1800.0).epsilon(1e-12));
  CHECK(paramValue(*filter, "res", *findParamDecl(*fp, "res")) == doctest::Approx(0.4).epsilon(1e-12));
  // The Envelope's attack (exp0, k = 15.87) inside the instrument group: 20 ms.
  const meta::Type* et = meta::findType("envelope");
  REQUIRE(et != nullptr);
  const size_t ai = static_cast<size_t>(et->paramIndex("attack"));
  CHECK(metaParamValue(*env, "", *et, ai) == doctest::Approx(0.02).epsilon(1e-12));
  CHECK(metaParamPosition(*env, "", *et, ai) > 0.0);
  CHECK(metaParamPosition(*env, "", *et, ai) < 1.0);
  // The switched case: the TX.knob-curves names became curve objects (Exponential cutoff, Logarithmic dB gain).
  const Snapshot sw = loadCase("switched");
  const Device* sf = deviceOf(*sw, "auto_filter");
  const Device* su = deviceOf(*sw, "utility");
  REQUIRE(sf != nullptr);
  REQUIRE(su != nullptr);
  REQUIRE(sf->curve("freq") != nullptr);
  CHECK(sf->curve("freq")->preset == dsp::CurvePreset::Exponential);
  REQUIRE(su->curve("gain") != nullptr);
  CHECK(su->curve("gain")->preset == dsp::CurvePreset::Logarithmic);
  CHECK(su->curve("gain")->output == dsp::CurveOutput::DbToGain);
  // Saved again it is a schema-20 file that loads to the same project, without the library.
  auto text = serializeProject(*sw);
  REQUIRE(text.ok());
  auto back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == *sw);
  // The schema-19 file without the library: refused (it holds values to convert), saying why.
  auto raw = loadProject(caseDir("undriven").string());
  REQUIRE_FALSE(raw.ok());
  CHECK(raw.error().message.find("device library") != std::string::npos);
}

TEST_CASE("GU.1 curve renders: an undriven control has no op (inside the instrument group too); a driven one has one") {
  const Snapshot undriven = loadCase("undriven");
  CHECK(curveOps(*undriven) == 0);
  // wires: four driven controls (the envelope's decay and sustain inside the instrument group, the cutoff, the gain);
  // automation: two; switched: two.
  const Snapshot wires = loadCase("wires");
  const uint32_t w = curveOps(*wires);
  MESSAGE("curve ops: wires " << w);
  CHECK(w >= 4);
  CHECK(curveOps(*loadCase("automation")) >= 2);
  CHECK(curveOps(*loadCase("automation_steps")) >= 2);
  CHECK(curveOps(*loadCase("switched")) >= 2);
  // The undriven project's runtime params are values in units (no "@pos" key); the wired one's driven knobs positions.
  for (const LoweredParam& p : lowerParams(*undriven, true, library())) CHECK_FALSE(p.position);
  uint32_t positions = 0;
  for (const LoweredParam& p : lowerParams(*wires, true, library())) positions += p.position ? 1u : 0u;
  CHECK(positions == 4);
}

TEST_CASE("GU.1 curve renders: live edits reach the audio without a compile where the class stays (RtScope)") {
  DocumentStore store(loadCase("wires"));
  store.setDeviceLibrary(&library());
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.sampleRate = kRate;
  auto l = app::ProjectLink::create(c, store.current());
  REQUIRE(l.ok());
  app::ProjectLink& link = **l;
  [[maybe_unused]] const ListenerId sub = store.subscribe([&](const StoreEvent& e) { link.onDocumentChange(e); });
  REQUIRE(link.waitForMetaTables());
  link.play(0);
  Driver d{link};
  d.run(40);
  const Device* filter = deviceOf(*store.current(), "auto_filter");  // driven (a wire)
  const Device* util = deviceOf(*store.current(), "utility");       // gain driven, width undriven
  REQUIRE(filter != nullptr);
  REQUIRE(util != nullptr);
  const DeviceId fid = filter->id, uid = util->id;
  const uint64_t compiles0 = link.stats().compiles;
  auto edit = [&](const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    link.flush();
  };
  // A knob drag on an undriven control: one param write, the value its curve gives.
  edit(ops::setDeviceParamValue(*store.current(), library(), uid, "width", 1.5));
  d.run(20);
  // A curve edit of the same class on the driven cutoff (Octave → Exponential: one Exp segment either way): its slot.
  const uint32_t slot = link.curveSlot(fid, "freq");
  REQUIRE(slot != 0);
  const dsp::CurveData before = link.curveSlotObject(slot)->data();
  edit(ops::setParamCurvePreset(*store.current(), library(), fid, "freq", dsp::CurvePreset::Exponential));
  CHECK_FALSE(dsp::sameData(before, link.curveSlotObject(slot)->data()));
  d.run(20);
  CHECK(link.stats().compiles == compiles0);  // neither edit compiled
  // A class change (to Linear: the wire law follows it) compiles once.
  edit(ops::setParamCurvePreset(*store.current(), library(), fid, "freq", dsp::CurvePreset::Linear));
  d.run(20);
  CHECK(link.stats().compiles > compiles0);
  CHECK(d.violations == 0);
  CHECK(link.lastError().empty());
}
