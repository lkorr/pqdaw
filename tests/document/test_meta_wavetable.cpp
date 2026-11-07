// TMM.5: the Wavetable meta module in the document — keyframe cycles save and load bit-exact (64 keyframes within
// 20 ms), and its data reaches the lowering's resources for the link. TMM.8: fold / warp / sync take lanes, wires and
// macros (the live path; the lowering says which params the table must not bake; TX.mod-all: in Shape mode too —
// it refused them before), an effect Wavetable starts in Shape mode, a wired nested fold pin runs live.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/meta.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/graph/atoms.h"
#include "pq/meta/wavetable.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/meta/data.h"

using namespace pq::doc;
using pq::Result;
namespace meta = pq::meta;

namespace {

std::string stdlibDir() { return std::string(PQ_TEST_DATA_DIR) + "/../stdlib"; }

DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(stdlibDir());
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

std::string replaced(std::string s, const std::string& from, const std::string& to) {
  const size_t at = s.find(from);
  REQUIRE_MESSAGE(at != std::string::npos, from);
  s.replace(at, from.size(), to);
  return s;
}

// A MIDI track whose instrument is a chain Wavetable, and a MIDI track with BasicSynth.
struct Song {
  DocumentStore store;
  TrackId wtTrack, synthTrack;
  DeviceId wt, synth;
  Song() {
    wtTrack = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0);
    synthTrack = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0);
    synth = findTrackPtr(doc(), synthTrack)->devices.at(0)->id;
    // (IG.1-2: the instrument alone on the path first, so the Wavetable replaces it there — a rack can take it)
    edit(ops::ungroupInCircuit(store.current(), store.ids(), library(), findTrackPtr(doc(), wtTrack)->devices.at(1)->id));
    wt = edit(ops::addMetaDevice(doc(), store.ids(), wtTrack, "wavetable", 0)).devices.at(0);
  }
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
};

// 64 keyframes of smoothed random walks (any float bit patterns survive the file).
meta::DataPtr manyKeyframes(uint32_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> step(0.0f, 0.05f);
  meta::WavetableData d;
  for (uint32_t k = 0; k < n; ++k) {
    std::vector<float> c(pq::dsp::kKeyframeCycle);
    float x = 0.0f;
    for (float& v : c) v = (x += step(rng));
    c[7] = 1e-38f;  // a denormal-adjacent value
    d.keyframes.push_back({n == 1 ? 0.0f : static_cast<float>(k) / static_cast<float>(n - 1), std::move(c)});
  }
  d.gaps.assign(n - 1, pq::dsp::GapMode::Spectral);
  if (n > 2) d.gaps[1] = pq::dsp::GapMode::Crossfade;
  return meta::wavetableData(d);
}

}  // namespace

TEST_CASE("wavetable document: keyframe cycles save -> load bit-exact; 64 keyframes save + load within 20 ms") {
  Song s;
  const meta::DataPtr data = manyKeyframes(64, 5);
  s.edit(ops::setMetaData(s.doc(), s.wt, "", "wavetable", data));
  CHECK(s.store.lastChanges().metaDataOnly());
  s.edit(ops::setMetaParamValue(s.doc(), s.wt, "", "wavetable", "fold", 0.25));
  CHECK(s.store.lastChanges().paramOnly());
  s.edit(ops::setMetaSetting(s.doc(), s.wt, "", "wavetable", "warpMode", 2.0));
  const auto t0 = std::chrono::steady_clock::now();
  Result<std::string> text = serializeProject(s.doc(), s.store.view());
  REQUIRE(text.ok());
  Result<LoadedProject> back = parseProject(*text);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  MESSAGE("64 keyframes: file " << text->size() / 1024 << " KiB, save + load " << ms << " ms");
#if PQ_OPTIMIZED
  CHECK(ms <= 20.0);
#endif
  CHECK(*back->project == s.doc());
  const MetaInstance* inst = findDevice(*back->project, s.wt).device->metaAt("");
  REQUIRE(inst != nullptr);
  REQUIRE(inst->data != nullptr);
  CHECK(meta::dataEqual(inst->data, data));  // bit for bit
  auto parsed = meta::parseWavetableData(*inst->data);
  REQUIRE(parsed.ok());
  CHECK(parsed->keyframes.size() == 64);
  CHECK(parsed->gaps[1] == pq::dsp::GapMode::Crossfade);
  // Invalid wavetable data in a file is refused with the type's reason.
  std::string bad = *text;
  const std::string key = "\"frames\"";
  const size_t at = bad.find("\"keyframes\"");
  REQUIRE(at != std::string::npos);
  bad.insert(at, key + ": 0, ");
  Result<LoadedProject> rb = parseProject(bad);
  REQUIRE_FALSE(rb.ok());
  CHECK(rb.error().message.find("frames") != std::string::npos);
}

TEST_CASE("wavetable document: a huge finite cycle sample is refused when read, naming the instance (TMM.13-fix1 M6)") {
  // The review's fuzz case 259: a cycle scaled to 3e38 (finite, so the reader took it) rendered 71,430 non-finite
  // samples at the master; from about 1e10 the build already lost the waveform. Cycles are audio: |x| <= 64.
  auto problemAt = [](float peak) {
    meta::WavetableData d;
    std::vector<float> c(pq::dsp::kKeyframeCycle);
    for (size_t i = 0; i < c.size(); ++i) c[i] = peak * static_cast<float>(std::sin(6.283185307179586 * static_cast<double>(i) / c.size()));
    c[100] = peak;
    d.keyframes.push_back({0.0f, std::move(c)});
    return meta::wavetableDataProblem(*meta::wavetableData(d));
  };
  CHECK(problemAt(64.0f).empty());
  CHECK(problemAt(1.0f).empty());
  CHECK_FALSE(problemAt(65.0f).empty());
  const std::string problem = problemAt(3e38f);
  CHECK(problem.find("beyond") != std::string::npos);
  CHECK(problem.find("keyframe 0") != std::string::npos);
  // In a file (written from a valid project, its samples then replaced: the store refuses invalid data): the reader
  // refuses it with the instance's path.
  const std::vector<float> half(pq::dsp::kKeyframeCycle, 0.5f), huge(pq::dsp::kKeyframeCycle, 3e38f);
  Song s;
  meta::WavetableData ok;
  ok.keyframes.push_back({0.0f, half});
  s.edit(ops::setMetaData(s.doc(), s.wt, "", "wavetable", meta::wavetableData(ok)));
  Result<std::string> saved = serializeProject(s.doc(), s.store.view());
  REQUIRE(saved.ok());
  const std::string text = *saved;
  const std::string from = meta::encodeFloats(half), to = meta::encodeFloats(huge);
  Result<LoadedProject> rb = parseProject(replaced(text, from, to));
  REQUIRE_FALSE(rb.ok());
  CHECK(rb.error().message.find("beyond") != std::string::npos);
  CHECK(rb.error().message.find("$.tracks") != std::string::npos);
  // The builder refuses it too (data that reaches it another way never builds a non-finite table).
  pq::dsp::Keyframe k{0.0f, huge};
  pq::dsp::FrameTableSpec spec;
  spec.keyframes = std::span<const pq::dsp::Keyframe>(&k, 1);
  spec.denseFrames = 1;
  pq::dsp::FrameTableBuilder b;
  auto built = b.build(spec);
  REQUIRE_FALSE(built.ok());
  CHECK(built.error().message.find("64") != std::string::npos);
}

// TMM.8 (was TMM.5's "cannot be automated"): the live path lifts the refusal. In Phase mode fold / warp / sync take
// lanes, wires and macros, and the lowering expands the instance onto its live path (wavetable_live in an oversample
// region) and tells the link which params its table must not bake. TX.mod-all: Shape mode takes them too (its curve
// is then read through the live path).
bool expansionHas(const LoweredProject& lp, std::string_view atom) {
  if (!lp.metaPatches) return false;
  for (const pq::graph::Patch& p : lp.metaPatches->patches)
    for (pq::graph::NodeIndex n = 0; n < p.nodeCount(); ++n)
      if (p.node(n).kind == pq::graph::NodeKind::Atom && p.node(n).atom == pq::graph::atomId(atom)) return true;
  return false;
}

TEST_CASE("wavetable document: fold / warp / sync are automated, wired and mapped in Phase mode (the live path); "
          "Shape mode takes them too (TX.mod-all)") {
  Song s;
  auto laneFor = [&](DeviceId dev, TrackId track, const char* param) {
    auto lane = std::make_shared<AutomationLane>();
    lane->id = s.store.ids().lane();
    lane->param = ParamRef::device(dev, param);
    lane->paramName = param;
    return s.store.dispatch(AddAutomationLane{track, lane});
  };
  LowerOptions o;
  o.library = &library().library();
  o.devices = &library();
  o.slotOf = [](TrackId) { return 0u; };
  o.metaSlotOf = [](DeviceId, std::string_view, uint32_t index) { return 40u + (index & 0xFFFFu); };
  // Static: the baked expansion.
  Result<LoweredProject> lp0 = lower(s.doc(), o);
  REQUIRE(lp0.ok());
  CHECK(expansionHas(*lp0, "wavetable_pos"));
  CHECK_FALSE(expansionHas(*lp0, "wavetable_live"));
  // A lane on fold: the live path, fold not baked.
  REQUIRE(laneFor(s.wt, s.wtTrack, "fold").ok());
  Result<LoweredProject> lp = lower(s.doc(), o);
  REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
  CHECK(lp->deviceErrors.empty());
  CHECK(expansionHas(*lp, "wavetable_live"));
  REQUIRE(lp->metaResources.size() == 1);
  CHECK(lp->metaResources[0].type == "wavetable");
  CHECK(lp->metaResources[0].slot == 40u);
  REQUIRE(lp->metaResources[0].modulated.size() == meta::wavetableType().params.size());
  CHECK(lp->metaResources[0].modulated[meta::kWtFold] == 1);
  CHECK(lp->metaResources[0].modulated[meta::kWtWarp] == 0);
  // Lanes on warp and sync, a wire to warp, a macro on sync: all accepted.
  CHECK(laneFor(s.wt, s.wtTrack, "warp").ok());
  CHECK(laneFor(s.wt, s.wtTrack, "sync").ok());
  Wire w;
  w.id = s.store.ids().wire();
  w.from = s.synth;
  w.port = "out";
  w.to = s.wt;
  w.param = "warp";
  CHECK(s.store.dispatch(AddWire{w}).ok());
  const DeviceId macro =  // GU.9: a macro is a value block, its mapping a Travel wire
      s.edit(ops::addDevice(s.doc(), s.store.ids(), s.wtTrack, std::string(kValueDevice), DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  s.edit(ops::mapMacro(s.doc(), s.store.ids(), macro, s.wt, "sync"));
  Result<LoweredProject> lm = lower(s.doc(), o);
  REQUIRE_MESSAGE(lm.ok(), (lm.ok() ? std::string() : lm.error().message));
  CHECK(lm->deviceErrors.empty());
  CHECK(expansionHas(*lm, "wavetable_live"));
  // Shape mode: a Wavetable as an audio effect starts in Shape mode; TX.mod-all: its fold takes a lane (before: refused,
  // "baked into its curve in Shape mode"), and the lowering reads its curve through the live path.
  const DeviceId fx = s.edit(ops::addMetaDevice(s.doc(), s.store.ids(), s.synthTrack, "wavetable", 1)).devices.at(0);
  const Device& fd = *findDevice(s.doc(), fx).device;
  CHECK(fd.kind == DeviceKind::AudioEffect);
  REQUIRE(fd.setting("mode") != nullptr);
  CHECK(fd.setting("mode")->value == 1.0);
  CHECK(laneFor(fx, s.synthTrack, "position").ok());
  Result<LoweredProject> ls = lower(s.doc(), o);
  REQUIRE(ls.ok());
  CHECK(ls->deviceErrors.empty());
  CHECK(expansionHas(*ls, "wavetable_shape"));  // position is a runtime param: still the ADAA curve
  const auto r = laneFor(fx, s.synthTrack, "fold");
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  CHECK(metaParamBakedOnly(*findDevice(s.doc(), fx).device, "fold").empty());
  Result<LoweredProject> lf = lower(s.doc(), o);
  REQUIRE(lf.ok());
  CHECK(lf->deviceErrors.empty());
  bool shapeLive = false;
  for (const LoweredProject::MetaResource& m : lf->metaResources)
    if (m.device == fx) shapeLive = m.modulated.size() > meta::kWtFold && m.modulated[meta::kWtFold] == 1;
  CHECK(shapeLive);
}

TEST_CASE("wavetable document: a nested Wavetable lowers with its data; a wired fold pin runs on the live path") {
  Song s;
  const std::string base = library().stockText("BasicSynth");
  const std::string text = replaced(base, "  out = osc * env * vel * level", "  wt1 = meta_wavetable()\n  out = wt1 * env * vel * level");
  s.edit(ops::addMetaCall(s.doc(), s.synth, text, "wt1", "wavetable"));
  const meta::DataPtr data = manyKeyframes(3, 9);
  s.edit(ops::setMetaData(s.doc(), s.synth, "wt1", "wavetable", data));
  LowerOptions o;
  o.library = &library().library();
  o.devices = &library();
  o.slotOf = [](TrackId) { return 0u; };
  o.metaSlotOf = [](DeviceId, std::string_view, uint32_t index) { return 7u + index; };
  Result<LoweredProject> lp = lower(s.doc(), o);
  REQUIRE(lp.ok());
  CHECK(lp->deviceErrors.empty());
  REQUIRE(lp->metaResources.size() == 2);  // the chain Wavetable's, and the nested one's
  const auto nested = std::find_if(lp->metaResources.begin(), lp->metaResources.end(),
                                   [&](const LoweredProject::MetaResource& r) { return r.device == s.synth; });
  REQUIRE(nested != lp->metaResources.end());
  CHECK(nested->path == "wt1");
  CHECK(meta::dataEqual(nested->data, data));
  // A wired fold pin (TMM.8): the nested instance expands onto its live path; fold is not baked.
  const std::string wired = replaced(text, "meta_wavetable()", "meta_wavetable(fold: env)");
  s.edit(ops::editDevicePatch(s.doc(), s.synth, wired));
  Result<LoweredProject> lw = lower(s.doc(), o);
  REQUIRE(lw.ok());
  CHECK(lw->deviceErrors.empty());
  CHECK(expansionHas(*lw, "wavetable_live"));
  const auto wn = std::find_if(lw->metaResources.begin(), lw->metaResources.end(),
                               [&](const LoweredProject::MetaResource& r) { return r.device == s.synth; });
  REQUIRE(wn != lw->metaResources.end());
  CHECK(wn->modulated.at(meta::kWtFold) == 1);
}
