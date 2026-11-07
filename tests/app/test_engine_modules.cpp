// GU.7 (docs/plans/GU.md "Phases" 7): the Wavetable, the Envelope and the analysers are group modules around their
// native engines (pq/document/modules.h engineModuleBlocks), and a placed display gets its tap from the lowering.
//   renders   tests/app/fixtures/instrument_groups/group_notes (notes → an instrument group: Phase Accumulator →
//             meta:wavetable → meta:envelope → Out → Saturator) against the same project with the Wavetable and the
//             Envelope each moved into its module (the engine kept, a group around it: In → engine → Out, the module's
//             panel): bit for bit, interpreter and JIT (a group is identity; the engine is the same leaf)
//   structure engineModuleBlocks: the group, its one engine, In → the engine's chain input, `out` → Out, its panel
//   voices    the Phase Accumulator's phase orbit shows the voices: its Signal binding (frac's output) is tapped by the
//             lowering (a voice tap at "#voice:out"), so with a note playing the app's FaceVoiceHub reader — what the
//             panel's target hands the orbit (ViewContext::faceVoices) — reads a voice at the phase the module reads
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
#include "pq/app/face_voices.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/devices.h"
#include "pq/document/library_files.h"
#include "pq/document/modules.h"
#include "pq/document/serialize.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/view_context.h"

using namespace pq;
using namespace pq::doc;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kBlocks = 600;  // 0.8 s

const DeviceLibrary& library() {
  static const auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  return **lib;
}

Snapshot loadFixture(const std::string& rel) {
  LoadOptions lo;
  lo.devices = &library();
  auto r = loadProject((fs::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / rel).string(), lo);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
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
  return std::move(*l);
}

void process(app::ProjectLink& link, int64_t& t, golden::Channels* out) {
  std::vector<float> L(kBlock), R(kBlock);
  float* ch[2] = {L.data(), R.data()};
  {
    RtScope rt;
    DenormalGuard dg;
    link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
  }
  t += kBlock;
  if (out != nullptr) {
    (*out)[0].insert((*out)[0].end(), L.begin(), L.end());
    (*out)[1].insert((*out)[1].end(), R.begin(), R.end());
  }
}

golden::Channels render(const Snapshot& s, bool jit) {
  auto link = makeLink(s, jit);
  link->play(0);
  golden::Channels out(2);
  int64_t t = 0;
  for (uint32_t b = 0; b < kBlocks; ++b) process(*link, t, &out);
  CHECK(link->lastError().empty());
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

uint32_t maxId(const Project& p) {
  uint32_t m = 0;
  forEachDevice(p, [&](const Device& d, const Track&) { m = std::max(m, d.id.value); });
  for (const Wire& w : wiresOf(p)) m = std::max(m, w.id.value);
  for (const TrackPtr& t : p.tracks) m = std::max(m, t->id.value);
  return m + 100;
}

// `p` with engine `engine` (a free meta device) moved into its module: a group G (in the engine's group) around it,
// the wires into its chain input entering G's main input (G's In feeds the input), the wires from its `out` leaving G's
// main output (its `out` feeds G's Out); its other wires unchanged (across the edge). What engineModuleBlocks builds,
// with the engine's own id, params and data kept.
Snapshot inModule(const Snapshot& s, DeviceId engine, ModuleKind kind, uint32_t& nextId) {
  auto p = std::make_shared<Project>(*s);
  const DeviceId g{nextId++};
  for (TrackPtr& tp : p->tracks) {
    auto it = std::find_if(tp->devices.begin(), tp->devices.end(), [&](const DevicePtr& d) { return d->id == engine; });
    if (it == tp->devices.end()) continue;
    auto t = std::make_shared<Track>(*tp);
    auto& list = t->devices;
    auto at = list.begin() + (it - tp->devices.begin());
    auto moved = std::make_shared<Device>(**at);
    auto group = std::make_shared<Device>(*makeGroup(g));
    group->name = std::string(findModule(moduleEngineType(kind))->name);
    group->group = moved->group;
    group->panel = std::make_shared<const Panel>(enginePanel(kind, engine));
    moved->group = g;
    *at = moved;
    list.insert(at, group);
    tp = t;
    break;
  }
  const meta::Type* type = meta::findType(moduleEngineType(kind));
  REQUIRE(type != nullptr);
  const std::string in(type->inputs[static_cast<size_t>(type->chainInput)].name);
  auto wires = std::make_shared<WireList>();
  for (Wire w : wiresOf(*s)) {
    if (w.to == engine && !w.toMixer() && (w.param == in || w.param == kMainPort)) {
      Wire inner = w;
      inner.id = WireId{nextId++};
      inner.from = g;
      inner.port = std::string(kGroupInPort);
      inner.noteSource = {};
      inner.param = in;
      w.to = g;
      w.param = std::string(kMainPort);
      wires->push_back(w);
      wires->push_back(inner);
      continue;
    }
    if (w.from == engine && (w.port == "out" || w.port == kMainPort)) {
      Wire inner = w;
      inner.id = WireId{nextId++};
      inner.to = g;
      inner.mixer = {};
      inner.param = std::string(kGroupOutPort);
      inner.port = "out";
      w.from = g;
      w.port = std::string(kMainPort);
      wires->push_back(w);
      wires->push_back(inner);
      continue;
    }
    wires->push_back(w);
  }
  std::sort(wires->begin(), wires->end(), [](const Wire& a, const Wire& b) { return a.id < b.id; });
  p->wires = wires;
  return p;
}

const Device* firstOf(const Project& p, std::string_view patch) {
  const Device* out = nullptr;
  forEachDevice(p, [&](const Device& d, const Track&) {
    if (out == nullptr && d.patch == patch) out = &d;
  });
  return out;
}

}  // namespace

TEST_CASE("GU.7 modules: the Wavetable and the Envelope modules render bit for bit as their bare engines (interp == JIT)") {
  const Snapshot base = loadFixture("instrument_groups/group_notes");
  const Device* wt = firstOf(*base, "meta:wavetable");
  const Device* env = firstOf(*base, "meta:envelope");
  REQUIRE(wt != nullptr);
  REQUIRE(env != nullptr);
  uint32_t next = maxId(*base);
  const Snapshot wrapped = inModule(inModule(base, wt->id, ModuleKind::Wavetable, next), env->id, ModuleKind::Envelope, next);
  // Two more groups, each around one engine, the engines inside them.
  uint32_t groups0 = 0, groups1 = 0;
  forEachDevice(*base, [&](const Device& d, const Track&) { groups0 += isGroupDevice(d) ? 1 : 0; });
  forEachDevice(*wrapped, [&](const Device& d, const Track&) { groups1 += isGroupDevice(d) ? 1 : 0; });
  CHECK(groups1 == groups0 + 2);
  const golden::Channels ref = render(base, false);
  REQUIRE(peakOf(ref) > 1e-3);  // it sounds
  const golden::Channels a = render(wrapped, false), b = render(wrapped, true);
  CHECK(maxDiff(ref, a) == 0.0);
  CHECK(maxDiff(a, b) == 0.0);
  CHECK(maxDiff(ref, render(base, true)) == 0.0);
}

TEST_CASE("GU.7 modules: engineModuleBlocks is a group around one engine — In → chain input, out → Out — with its panel") {
  for (const ModuleKind k : {ModuleKind::Wavetable, ModuleKind::Envelope, ModuleKind::Oscilloscope, ModuleKind::Spectrum,
                             ModuleKind::Spectrogram}) {
    const std::string_view type = moduleEngineType(k);
    CAPTURE(type);
    IdSource ids;
    EngineRoles r;
    const BlockSet set = engineModuleBlocks(ids, k, {}, &r);
    REQUIRE(set.devices.size() == 2);
    CHECK(set.devices[0]->id == r.group);
    CHECK(isGroupDevice(*set.devices[0]));
    REQUIRE(set.devices[0]->hasPanel());
    CHECK(*set.devices[0]->panel == enginePanel(k, r.engine));
    CHECK(set.devices[1]->id == r.engine);
    CHECK(set.devices[1]->patch == std::string(kMetaDevicePrefix) + std::string(type));
    CHECK(set.devices[1]->group == r.group);
    REQUIRE(set.wires.size() == 2);
    const meta::Type* t = meta::findType(type);
    REQUIRE(t != nullptr);
    CHECK(set.wires[0].from == r.group);
    CHECK(set.wires[0].port == kGroupInPort);
    CHECK(set.wires[0].to == r.engine);
    CHECK(set.wires[0].param == t->inputs[static_cast<size_t>(t->chainInput)].name);
    CHECK(set.wires[1].from == r.engine);
    CHECK(set.wires[1].to == r.group);
    CHECK(set.wires[1].param == kGroupOutPort);
    // Every element binds the engine (a data or control binding), nothing else.
    for (const PanelElement& e : set.devices[0]->panel->elements)
      if (const Binding* b = e.binding()) CHECK(b->device == r.engine);
    CHECK(moduleOfEngine(type) == findModule(type));
  }
}

TEST_CASE("GU.7 taps: the Phase Accumulator's orbit is tapped by the lowering; a playing note's voice reaches the panel") {
  // pa_voice: notes → an instrument group (Phase Accumulator module → Envelope → Out); the module's orbit binds frac's out.
  LoadOptions lo;
  lo.devices = &library();
  auto loaded = loadProject((fs::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / "gu4_modules" / "pa_voice").string(), lo);
  REQUIRE(loaded.ok());
  const Snapshot s = loaded->project;
  const Device* pa = nullptr;
  forEachDevice(*s, [&](const Device& d, const Track&) {
    if (pa == nullptr && isGroupDevice(d) && d.name == "Phase Accumulator") pa = &d;
  });
  REQUIRE(pa != nullptr);
  REQUIRE(pa->hasPanel());
  const std::vector<PanelTap> taps = panelTaps(*pa->panel);
  REQUIRE(taps.size() == 1);
  CHECK(taps[0].kind == ElementTap::Voice);
  const DeviceId frac = taps[0].device;
  CHECK(findDevice(*s, frac).device->patch == "atom:frac");
  auto link = makeLink(s, false);
  CHECK(link->metaSlot(frac, tapPath(ElementTap::Voice, "out"), 0) != 0);  // the lowering asked for it
  app::FaceVoiceHub hub(*link);
  // What the panel's target hands the orbit: the context's face voices, by the binding's tap path.
  views::FakeTransport port;
  views::ViewContext ctx{port};
  ctx.setFaceVoices([&hub](DeviceId d, std::string_view path) { return hub.voices(d, path); });
  ctx.store().reset(s);
  auto target = views::panel::makeDocPanelTarget(ctx, pa->id, {});
  views::VoiceTapReader* reader = target->voices(signalBinding(frac, "out"));
  REQUIRE(reader != nullptr);
  CHECK(reader == hub.voices(frac, tapPath(ElementTap::Voice, "out")));
  std::array<views::FaceVoice, 16> v{};
  (void)reader->read(v);  // registers the tap's reader
  link->play(0);
  int64_t t = 0;
  uint32_t seen = 0;
  for (uint32_t b = 0; b < 200 && seen == 0; ++b) {
    process(*link, t, nullptr);
    if (b % 8 == 7) seen = reader->read(v);
  }
  REQUIRE(seen > 0);
  CHECK(v[0].phase >= 0.0f);
  CHECK(v[0].phase < 1.0f);
  CHECK(v[0].sampleCount > 0);
  CHECK(reader->live());
  // Without the orbit on the panel there is no tap (a display not placed costs nothing).
  auto bare = std::make_shared<Project>(*s);
  for (TrackPtr& tp : bare->tracks) {
    auto tr = std::make_shared<Track>(*tp);
    for (DevicePtr& d : tr->devices)
      if (d->id == pa->id) {
        auto x = std::make_shared<Device>(*d);
        auto panel = std::make_shared<Panel>(*x->panel);
        std::erase_if(panel->elements, [](const PanelElement& e) { return e.kind == "orbit"; });
        x->panel = panel;
        d = x;
      }
    tp = tr;
  }
  auto link2 = makeLink(bare, false);
  CHECK(link2->metaSlot(frac, tapPath(ElementTap::Voice, "out"), 0) == 0);
}
