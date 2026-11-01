// TMM.5: the Wavetable meta module in the document — keyframe cycles save and load bit-exact (64 keyframes within
// 20 ms), its baked controls (fold / warp / sync) cannot be automated, wired or macro-mapped (position can), a wired
// baked pin in a circuit is refused with a reason, and its data reaches the lowering's resources for the link.
#include <doctest/doctest.h>

#include <algorithm>
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
#include "pq/meta/wavetable.h"

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
  s.edit(ops::setMetaParam(s.doc(), s.wt, "", "wavetable", "fold", 0.25));
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

TEST_CASE("wavetable document: fold / warp / sync cannot be automated, wired or mapped; position can") {
  Song s;
  auto laneFor = [&](const char* param) {
    auto lane = std::make_shared<AutomationLane>();
    lane->id = s.store.ids().lane();
    lane->param = ParamRef::device(s.wt, param);
    lane->paramName = param;
    return s.store.dispatch(AddAutomationLane{s.wtTrack, lane});
  };
  for (const char* p : {"fold", "warp", "sync"}) {
    const auto r = laneFor(p);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("baked") != std::string::npos);
  }
  CHECK(laneFor("position").ok());
  CHECK(laneFor("phase").ok());
  // A modulation wire from the synth's export to fold: refused; to position: accepted.
  const std::vector<std::string> exports = {"out"};
  Wire w;
  w.id = s.store.ids().wire();
  w.from = s.synth;
  w.port = "out";
  w.to = s.wt;
  w.param = "fold";
  const auto rw = s.store.dispatch(AddWire{w});
  REQUIRE_FALSE(rw.ok());
  CHECK(rw.error().message.find("baked") != std::string::npos);
  w.param = "position";
  CHECK(s.store.dispatch(AddWire{w}).ok());
  // A macro mapping: refused for sync, accepted for position.
  const DeviceId rack = s.edit(ops::groupDevices(s.doc(), s.store.ids(), std::vector<DeviceId>{s.wt})).devices.at(0);
  auto m = ops::mapMacro(s.doc(), s.store.ids(), rack, 0, s.wt, "sync");
  REQUIRE(m.ok());
  const auto rm = s.store.dispatch(m->command);
  REQUIRE_FALSE(rm.ok());
  CHECK(rm.error().message.find("baked") != std::string::npos);
  s.edit(ops::mapMacro(s.doc(), s.store.ids(), rack, 1, s.wt, "position"));
  // So the chain device's baked params are always static: its expansion stays the baked one.
  LowerOptions o;
  o.library = &library().library();
  o.devices = &library();
  o.slotOf = [](TrackId) { return 0u; };
  o.metaSlotOf = [](DeviceId, std::string_view, uint32_t index) { return 40u + index; };
  Result<LoweredProject> lp = lower(s.doc(), o);
  REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
  CHECK(lp->deviceErrors.empty());
  REQUIRE(lp->metaResources.size() == 1);
  CHECK(lp->metaResources[0].type == "wavetable");
  CHECK(lp->metaResources[0].slot == 40u);
  CHECK(lp->metaResources[0].request.kind == meta::ResourceKind::FrameTable);
}

TEST_CASE("wavetable document: a nested Wavetable lowers with its data; a wired baked pin is refused with a reason") {
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
  // A wired fold pin: the device reports why (its expansion is refused), nothing crashes.
  const std::string wired = replaced(text, "meta_wavetable()", "meta_wavetable(fold: env)");
  s.edit(ops::editDevicePatch(s.doc(), s.synth, wired));
  Result<LoweredProject> lw = lower(s.doc(), o);
  REQUIRE(lw.ok());
  REQUIRE(lw->deviceErrors.size() == 1);
  CHECK(lw->deviceErrors[0].device == s.synth);
  CHECK(lw->deviceErrors[0].message.find("fold") != std::string::npos);
  CHECK(lw->deviceErrors[0].message.find("baked") != std::string::npos);
}
