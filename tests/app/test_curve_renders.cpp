// GU.1 reference recorder (runs on the code BEFORE GU.1's document change, with PQ_GU1_RECORD=1): builds the five
// curve-render projects with the pre-GU.1 API, saves each (schema 19) under tests/app/fixtures/gu1_curves/<case>/ and
// records its interpreter render as reference.wav (32-bit float). The GU.1 test that replaces this file loads those
// projects (the schema 19 → 20 migration turns their values into knob positions) and renders them against the
// references.
#include <doctest/doctest.h>

#include <cstdlib>
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
#include "pq/document/edit_ops.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/serialize.h"
#include "pq/engine/wav_file.h"
#include "pq/meta/vca.h"

using namespace pq;
using namespace pq::doc;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kBlocks = 600;  // 0.8 s

fs::path caseDir(const std::string& name) { return fs::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / "gu1_curves" / name; }

const DeviceLibrary& library() {
  static const auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  return **lib;
}

struct Song {
  DocumentStore store;
  TrackId track;
  DeviceId acc, env, filter, util, lfo, vca;
  Song() {
    must(SetTempoMap{{{0, 120.0, TempoCurve::Step}}});
    auto t = ops::createTrack(doc(), store.ids(), TrackKind::Midi);
    REQUIRE(t.ok());
    must(t->command);
    track = t->tracks.at(0);
    playback_fixture::ungroupHead(store, library(), track);
    acc = meta("phase_acc", 0);
    env = meta("envelope");
    filter = stock("auto_filter");
    util = stock("utility");
    // Undriven values off their defaults (units).
    edit(ops::setMetaParam(doc(), acc, "", "phase_acc", "pitch", -300.0));
    edit(ops::setMetaParam(doc(), acc, "", "phase_acc", "amp", 0.6));
    edit(ops::setMetaParam(doc(), env, "", "envelope", "attack", 0.02));
    edit(ops::setMetaParam(doc(), env, "", "envelope", "decay", 0.15));
    edit(ops::setMetaParam(doc(), env, "", "envelope", "sustain", 0.5));
    edit(ops::setMetaParam(doc(), env, "", "envelope", "release", 0.2));
    edit(ops::setDeviceParam(doc(), filter, "freq", 1800.0));
    edit(ops::setDeviceParam(doc(), filter, "res", 0.4));
    edit(ops::setDeviceParam(doc(), util, "gain", 0.5));
    edit(ops::setDeviceParam(doc(), util, "width", 0.8));
    std::vector<Note> v{playback_fixture::note(store.ids(), 0, kPpq, 0)};  // a quarter note (0.5 s)
    must(AddClip{track, playback_fixture::clip(store.ids(), 0, kPpq, std::move(v), "n")});
  }
  void must(Command c) {
    const Status st = store.dispatch(std::move(c));
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  void edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    must(e->command);
  }
  const Project& doc() const { return *store.current(); }
  DeviceId meta(std::string_view type, uint32_t index = kAppend) {
    auto e = ops::addMetaDevice(doc(), store.ids(), track, type, index);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    must(e->command);
    return e->devices.at(0);
  }
  DeviceId freeMeta(std::string_view type) {
    auto e = ops::addMetaDevice(doc(), store.ids(), track, type, ops::kOffPath);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    must(e->command);
    return e->devices.at(0);
  }
  DeviceId stock(const std::string& patch) {
    auto e = ops::addDevice(doc(), store.ids(), track, patch, DeviceKind::AudioEffect);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    must(e->command);
    return e->devices.at(0);
  }
  // A 3 Hz ramp × 0.35 (a free Phase accumulator through a free VCA on its Bend curve, min = max = 0.35).
  void modulator() {
    lfo = freeMeta("phase_acc");
    edit(ops::setMetaParam(doc(), lfo, "", "phase_acc", "freq", 3.0));
    vca = freeMeta("vca");
    edit(ops::setMetaSetting(doc(), vca, "", "vca", "curve", static_cast<double>(meta::VcaCurve::Bend)));
    edit(ops::setMetaParam(doc(), vca, "", "vca", "max", 0.35));
    edit(ops::setMetaParam(doc(), vca, "", "vca", "min", 0.35));
    edit(ops::connectWire(doc(), store.ids(), lfo, "out", vca, "in"));
  }
  void wire(DeviceId to, const std::string& param) { edit(ops::connectWire(doc(), store.ids(), vca, "out", to, param)); }
  void lane(DeviceId d, const std::string& param, std::vector<Breakpoint> points) {
    auto l = std::make_shared<AutomationLane>();
    l->id = store.ids().lane();
    l->param = ParamRef::device(d, param);
    l->paramName = param;
    for (Breakpoint& p : points) p.id = store.ids().breakpoint();
    l->points = std::move(points);
    must(AddAutomationLane{track, l});
  }
};

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

std::unique_ptr<Song> build(const std::string& name) {
  auto s = std::make_unique<Song>();
  if (name == "wires" || name == "switched") s->modulator();
  if (name == "wires") {
    s->wire(s->env, "decay");     // exp0 (curved): adds in travel
    s->wire(s->filter, "freq");   // octave (curved): adds in travel
    s->wire(s->util, "gain");     // a dB gain (linear in dB): adds in dB
    s->wire(s->env, "sustain");   // linear: adds in units
  } else if (name == "automation") {
    s->lane(s->filter, "freq", {{0, 400.0f, 0.3f, {}}, {kPpq, 6000.0f, 0.0f, {}}});
    s->lane(s->util, "gain", {{0, 0.2f, 0.0f, {}}, {kPpq, 1.0f, 0.0f, {}}});
  } else if (name == "macro") {
    auto g = ops::groupDevices(s->doc(), s->store.ids(), std::vector{s->filter});
    s->edit(std::move(g));
    const DeviceId rack = findDevice(s->doc(), s->filter).rack->id;
    s->edit(ops::mapMacro(s->doc(), s->store.ids(), rack, 0, s->filter, "freq"));
    const MacroMapping& m = findDevice(s->doc(), rack).device->rack->mappings.at(0);
    s->edit(ops::setMacroMapping(s->doc(), m.id, 0.2, 0.9, 0.3));
    s->edit(ops::setMacroValue(s->doc(), rack, 0, 80.0));
  } else if (name == "switched") {
    s->edit(ops::setParamCurve(s->doc(), s->filter, "freq", KnobCurve::Exponential));
    s->edit(ops::setParamCurve(s->doc(), s->util, "gain", KnobCurve::Logarithmic));
    s->wire(s->filter, "freq");  // curved: adds in travel
    s->wire(s->util, "gain");    // a dB gain bent in dB (curved): adds in travel
  }
  return s;
}

}  // namespace

TEST_CASE("GU.1 references: record the pre-GU.1 renders (PQ_GU1_RECORD=1)") {
  if (std::getenv("PQ_GU1_RECORD") == nullptr) return;
  for (const std::string name : {"undriven", "wires", "automation", "macro", "switched"}) {
    CAPTURE(name);
    auto s = build(name);
    Project p = *s->store.current();
    (void)migrateToInstrumentGroups(p);  // as a schema-19 file plays
    const Snapshot snap = std::make_shared<const Project>(std::move(p));
    fs::create_directories(caseDir(name));
    REQUIRE(saveProject(caseDir(name).string(), *snap).ok());
    const golden::Channels ref = render(snap, false);
    REQUIRE(engine::writeWav(caseDir(name) / "reference.wav", ref, static_cast<uint32_t>(kRate)).ok());
    const golden::Channels jit = render(snap, true);
    double d = 0.0;
    for (size_t c = 0; c < 2; ++c)
      for (size_t i = 0; i < ref[c].size(); ++i) d = std::max(d, std::fabs(double(ref[c][i]) - jit[c][i]));
    MESSAGE(name << ": interpreter vs JIT max diff " << d);
  }
}
