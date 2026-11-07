// GU.4 reference recorder (runs on the code BEFORE GU.4, with PQ_GU4_RECORD=1): builds projects that use the Phase
// Accumulator and VCA meta modules in every way their state can (notes, free running, phase modulation, a wrapped phase,
// a negative frequency, the VCA's Log law at several positions and ranges, its Bend curve, an unwired VCA as a value
// source, automation of its knob, wires into its knob), saves each (schema 21, meta:phase_acc / meta:vca) under
// tests/app/fixtures/gu4_modules/<case>/ and records its interpreter render as reference.wav (32-bit float).
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

fs::path caseDir(const std::string& name) { return fs::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / "gu4_modules" / name; }

const DeviceLibrary& library() {
  static const auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  return **lib;
}

struct Song {
  DocumentStore store;
  TrackId track;
  explicit Song(TrackKind kind) {
    must(SetTempoMap{{{0, 120.0, TempoCurve::Step}}});
    auto t = ops::createTrack(doc(), store.ids(), kind);
    REQUIRE(t.ok());
    must(t->command);
    track = t->tracks.at(0);
    if (kind == TrackKind::Midi) {
      playback_fixture::ungroupHead(store, library(), track);
      std::vector<Note> v{playback_fixture::note(store.ids(), 0, kPpq, 0),
                          playback_fixture::note(store.ids(), kPpq / 4, kPpq / 2, 7)};
      must(AddClip{track, playback_fixture::clip(store.ids(), 0, kPpq, std::move(v), "n")});
    }
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
  DeviceId freeMeta(std::string_view type) { return meta(type, ops::kOffPath); }
  DeviceId stock(const std::string& patch) {
    auto e = ops::addDevice(doc(), store.ids(), track, patch, DeviceKind::AudioEffect);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    must(e->command);
    return e->devices.at(0);
  }
  void pa(DeviceId d, const std::string& param, double units) {
    edit(ops::setMetaParamValue(doc(), d, "", "phase_acc", param, units));
  }
  void vca(DeviceId d, const std::string& param, double units) {
    edit(ops::setMetaParamValue(doc(), d, "", "vca", param, units));
  }
  void bend(DeviceId d) { edit(ops::setMetaSetting(doc(), d, "", "vca", "curve", static_cast<double>(meta::VcaCurve::Bend))); }
  // A free Phase accumulator at `hz` through a free VCA on its Bend curve from lo to hi at its default position (0.7):
  // a slow ramp scaled into [lo + 0.7·(hi − lo) …].
  DeviceId modulator(double hz, double lo, double hi) {
    const DeviceId lfo = freeMeta("phase_acc");
    pa(lfo, "freq", hz);
    const DeviceId v = freeMeta("vca");
    bend(v);
    vca(v, "max", hi);
    vca(v, "min", lo);
    edit(ops::connectWire(doc(), store.ids(), lfo, "out", v, "in"));
    return v;
  }
  void wire(DeviceId from, DeviceId to, const std::string& param) {
    edit(ops::connectWire(doc(), store.ids(), from, "out", to, param));
  }
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
  if (name == "pa_voice") {  // notes: In → PA → Envelope → Out (an instrument group once grouped)
    auto s = std::make_unique<Song>(TrackKind::Midi);
    const DeviceId acc = s->meta("phase_acc", 0);
    const DeviceId env = s->meta("envelope");
    s->pa(acc, "pitch", 700.0);
    s->pa(acc, "amp", 0.6);
    s->pa(acc, "phase", 0.25);
    s->pa(acc, "freq", 3.0);
    (void)env;
    return s;
  }
  auto s = std::make_unique<Song>(TrackKind::Audio);
  if (name == "pa_free") {  // free running on an audio track's path; pm and a wrapping phase modulated
    const DeviceId acc = s->meta("phase_acc");
    s->pa(acc, "freq", 330.0);
    s->pa(acc, "amp", 0.8);
    s->pa(acc, "pitch", -1250.0);
    s->wire(s->modulator(5.0, 0.0, 0.4), acc, "pm");
    s->wire(s->modulator(2.0, 0.6, 0.9), acc, "phase");
  } else if (name == "pa_signed") {  // a wire takes the frequency below zero (the phase runs backwards)
    const DeviceId acc = s->meta("phase_acc");
    s->wire(s->modulator(1.5, -0.7, -0.2), acc, "freq");
  } else if (name == "vca_law") {  // the default Log law: fine zone, positive outer, negative outer
    const DeviceId acc = s->meta("phase_acc");
    s->pa(acc, "freq", 220.0);
    const DeviceId a = s->meta("vca"), b = s->meta("vca"), c = s->meta("vca");
    s->vca(a, "value", 0.62);
    s->vca(b, "value", 0.78);
    s->vca(c, "value", 0.21);
  } else if (name == "vca_ranges") {  // other ranges and zones, the Bend curve, an unwired VCA as a source
    const DeviceId acc = s->meta("phase_acc");
    s->pa(acc, "freq", 110.0);
    const DeviceId a = s->meta("vca"), b = s->meta("vca"), c = s->meta("vca");
    s->vca(a, "min", -2.0);
    s->vca(a, "max", 50.0);
    s->vca(a, "zone", 0.6);
    s->vca(a, "value", 0.83);
    s->bend(b);
    s->vca(b, "min", 0.0);
    s->vca(b, "max", 2.0);
    s->vca(b, "bend", 0.5);
    s->vca(b, "value", 0.4);
    s->vca(c, "min", 0.25);
    s->vca(c, "max", 0.8);
    s->vca(c, "value", 0.35);
    // A source: unwired (in = 1), a pure exponential 20 … 20000 into the accumulator's frequency input.
    const DeviceId src = s->freeMeta("vca");
    s->vca(src, "min", 20.0);
    s->vca(src, "max", 20000.0);
    s->vca(src, "value", 0.3);
    s->wire(src, acc, "hz");
  } else if (name == "vca_driven") {  // automation of the knob, wires into it (Log: in position; Bend: in units)
    const DeviceId acc = s->meta("phase_acc");
    s->pa(acc, "freq", 220.0);
    const DeviceId a = s->meta("vca"), b = s->meta("vca");
    s->lane(a, "value", {{0, 0.5f, 0.0f, {}}, {kPpq / 2, 0.9f, 0.3f, {}}, {kPpq, 0.2f, 0.0f, {}}});
    s->vca(b, "value", 0.62);
    s->wire(s->modulator(3.0, 0.0, 0.15), b, "value");
    const DeviceId c = s->meta("vca");
    s->bend(c);
    s->vca(c, "min", 0.0);
    s->vca(c, "max", 1.0);
    s->wire(s->modulator(4.0, -0.2, 0.2), c, "value");
  }
  return s;
}

}  // namespace

TEST_CASE("GU.4 references: record the pre-GU.4 renders (PQ_GU4_RECORD=1)") {
  if (std::getenv("PQ_GU4_RECORD") == nullptr) return;
  for (const std::string name : {"pa_voice", "pa_free", "pa_signed", "vca_law", "vca_ranges", "vca_driven"}) {
    CAPTURE(name);
    auto s = build(name);
    Project p = *s->store.current();
    (void)migrateToInstrumentGroups(p);
    const Snapshot snap = std::make_shared<const Project>(std::move(p));
    fs::create_directories(caseDir(name));
    REQUIRE(saveProject(caseDir(name).string(), *snap).ok());
    const golden::Channels ref = render(snap, false);
    REQUIRE(engine::writeWav(caseDir(name) / "reference.wav", ref, static_cast<uint32_t>(kRate)).ok());
    const golden::Channels jit = render(snap, true);
    double d = 0.0, peak = 0.0;
    for (size_t c = 0; c < 2; ++c)
      for (size_t i = 0; i < ref[c].size(); ++i) {
        d = std::max(d, std::fabs(double(ref[c][i]) - jit[c][i]));
        peak = std::max(peak, std::fabs(double(ref[c][i])));
      }
    MESSAGE(name << ": peak " << peak << ", interpreter vs JIT max diff " << d);
  }
}
