// TMM.10: meta modules as chain devices — voice continuity (a meta voice run: Phase accumulator → Wavetable → Auto
// Filter lowers to per-voice calls, a release, then the voice sum; golden text), the kinds addMetaDevice picks, a meta
// effect outside a run (bypassed with a reason), the param fast path, copy / paste / presets keep the state.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/library_files.h"
#include "pq/document/lower.h"
#include "pq/document/meta.h"
#include "pq/document/store.h"
#include "pq/graph/parse.h"
#include "pq/meta/wavetable.h"

using namespace pq::doc;
using pq::Result;
namespace meta = pq::meta;

namespace {

std::string stdlibDir() { return std::string(PQ_TEST_DATA_DIR) + "/../stdlib"; }
std::string fixturePath(const std::string& rel) { return std::string(PQ_TEST_DATA_DIR) + "/document/fixtures/" + rel; }
std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
bool updateGoldens() {
  const char* v = std::getenv("PQ_UPDATE_GOLDENS");
  return v != nullptr && *v != '\0' && *v != '0';
}
void checkGolden(const std::string& name, const std::string& text) {
  const std::string path = fixturePath("meta/" + name);
  if (updateGoldens()) std::ofstream(path, std::ios::binary) << text;
  const std::string gold = readFile(path);
  CAPTURE(text);
  CHECK(text == gold);
}

DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(stdlibDir());
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// A MIDI track with Phase accumulator → Wavetable → Auto Filter.
struct MetaChain {
  DocumentStore store;
  TrackId track;
  DeviceId acc, wt, filter;
  MetaChain() {
    track = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0);
    acc = edit(ops::addMetaDevice(doc(), store.ids(), track, "phase_acc", 0)).devices.at(0);
    wt = edit(ops::addMetaDevice(doc(), store.ids(), track, "wavetable")).devices.at(0);
    filter = edit(ops::addDevice(doc(), store.ids(), track, "auto_filter", DeviceKind::AudioEffect)).devices.at(0);
  }
  const Project& doc() const { return *store.current(); }
  const Device& dev(DeviceId d) const { return *findDevice(doc(), d).device; }
  const Track& tr() const { return *findTrackPtr(doc(), track); }
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
};

Result<LoweredProject> lowerIt(const Project& p) {
  LowerOptions o;
  o.library = &library().library();
  o.devices = &library();
  o.slotOf = [](TrackId) { return 0u; };
  o.metaSlotOf = [](DeviceId d, std::string_view, uint32_t i) { return d.value * 4 + i + 1; };
  return lower(p, o);
}

size_t trackIndex(const LoweredProject& lp, TrackId t) {
  for (size_t i = 0; i < lp.trackIds.size(); ++i)
    if (lp.trackIds[i] == t) return i;
  FAIL("track not lowered");
  return 0;
}

const pq::graph::Patch& trackPatch(const LoweredProject& lp, TrackId t) { return lp.patches[trackIndex(lp, t)]->patch; }

std::string calleeOf(const LoweredProject& lp, TrackId t, DeviceId d) {
  const pq::graph::Patch& p = trackPatch(lp, t);
  const auto n = p.findByName("d" + std::to_string(d.value));
  REQUIRE(n.has_value());
  return p.subcircuit(*n).patch;
}

}  // namespace

TEST_CASE("meta devices: addMetaDevice picks the kind — a run continues per voice, an instrument-only type replaces") {
  MetaChain c;
  CHECK(c.dev(c.acc).kind == DeviceKind::Instrument);
  CHECK(c.dev(c.wt).kind == DeviceKind::AudioEffect);  // continues the Phase accumulator's voices
  REQUIRE(metaVoiceRun(c.tr()).size() == 1);
  CHECK(metaVoiceRun(c.tr())[0] == &c.dev(c.wt));
  // Before the Auto Filter: the run goes on; after it: nothing continues, a Wavetable (no effect form until Shape mode)
  // takes the instrument position instead.
  CHECK(continuesMetaVoiceRun(c.tr(), 2));
  CHECK_FALSE(continuesMetaVoiceRun(c.tr(), 3));
  CHECK(*ops::metaDeviceKindAt(c.doc(), meta::wavetableType(), c.track, 2) == DeviceKind::AudioEffect);
  CHECK(*ops::metaDeviceKindAt(c.doc(), meta::wavetableType(), c.track, kAppend) == DeviceKind::Instrument);
  CHECK(*ops::metaDeviceKindAt(c.doc(), meta::wavetableType(), c.track, 0) == DeviceKind::Instrument);
  // A second Phase accumulator after the first continues too (its voice input is pm).
  const DeviceId acc2 = c.edit(ops::addMetaDevice(c.doc(), c.store.ids(), c.track, "phase_acc", 1)).devices.at(0);
  CHECK(c.dev(acc2).kind == DeviceKind::AudioEffect);
  CHECK(metaVoiceRun(c.tr()).size() == 2);
  // An inactive device does not break the run.
  c.edit(ops::setDevicesActive(c.doc(), std::vector<DeviceId>{acc2}, false));
  CHECK(metaVoiceRun(c.tr()).size() == 1);
  CHECK(continuesMetaVoiceRun(c.tr(), 3));
  // An audio track has no run.
  const TrackId audio = c.edit(ops::createTrack(c.doc(), c.store.ids(), TrackKind::Audio)).tracks.at(0);
  CHECK_FALSE(continuesMetaVoiceRun(*findTrackPtr(c.doc(), audio), 0));
}

TEST_CASE("meta devices: a voice run lowers per voice — calls in a row, a release, then the sum (golden)") {
  MetaChain c;
  Result<LoweredProject> lp = lowerIt(c.doc());
  REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
  for (const LoweredProject::DeviceError& e : lp->deviceErrors) FAIL_CHECK(e.message);
  CHECK(lp->missingDevices.empty());
  std::string text = patchText(*lp->patches[trackIndex(*lp, c.track)]);
  const std::string wtCallee = calleeOf(*lp, c.track, c.wt);
  CHECK(wtCallee.starts_with("meta_wavetable__d" + std::to_string(c.wt.value) + "__"));
  const pq::graph::Patch* wt = lp->graph.library->find(wtCallee);
  REQUIRE(wt != nullptr);
  text += "\n" + pq::graph::print(*wt);
  checkGolden("lower_meta_voice_run.txt", text);
  // The Wavetable's expansion is the voice form: its `x` is a per-voice `in`, no phasor of its own, no `done`.
  CHECK(text.find("in x : audio voice") != std::string::npos);
  CHECK_FALSE(wt->findByName("done").has_value());
  // Its params are the device's own (the fast path lists them too).
  const std::vector<LoweredParam> fast = lowerParams(c.doc(), true, library());
  for (const char* name : {"phase", "position", "fold"}) {
    const std::string key = "dev" + std::to_string(c.wt.value) + ":" + name;
    CHECK(std::any_of(lp->params.begin(), lp->params.end(), [&](const LoweredParam& p) { return p.key == key; }));
    CHECK(std::any_of(fast.begin(), fast.end(), [&](const LoweredParam& p) { return p.key == key; }));
  }
  // Both modules' resources are listed (the Wavetable's table, the Phase accumulator's tap).
  size_t tables = 0, taps = 0;
  for (const LoweredProject::MetaResource& r : lp->metaResources) {
    tables += r.device == c.wt && r.request.kind == meta::ResourceKind::FrameTable ? 1 : 0;
    taps += r.device == c.acc && r.request.kind == meta::ResourceKind::VoiceTap ? 1 : 0;
  }
  CHECK(tables == 1);
  CHECK(taps == 1);
}

TEST_CASE("meta devices: a lone meta instrument ends its voices with the release, not at note-off") {
  DocumentStore store;
  const TrackId t = [&] {
    auto e = ops::createTrack(*store.current(), store.ids(), TrackKind::Midi);
    REQUIRE(e.ok());
    REQUIRE(store.dispatch(e->command).ok());
    return e->tracks.at(0);
  }();
  auto e = ops::addMetaDevice(*store.current(), store.ids(), t, "wavetable", 0);
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  Result<LoweredProject> lp = lowerIt(*store.current());
  REQUIRE(lp.ok());
  const std::string text = patchText(*lp->patches[trackIndex(*lp, t)]);
  CAPTURE(text);
  CHECK(text.find("z1(") != std::string::npos);
  CHECK(text.find("200 / sr") != std::string::npos);  // 5 ms
  CHECK(text.find(".done") == std::string::npos);     // the instrument form's own done (= !gate) is not used
}

TEST_CASE("meta devices: a Wavetable moved out of its run is bypassed with a reason; moved back, it plays") {
  MetaChain c;
  // After the Auto Filter: no run (and no effect form until Shape mode).
  c.edit(ops::moveDevice(c.doc(), c.wt, c.track, 2));
  CHECK(metaVoiceRun(c.tr()).empty());
  Result<LoweredProject> lp = lowerIt(c.doc());
  REQUIRE(lp.ok());
  bool reported = false;
  for (const LoweredProject::DeviceError& e : lp->deviceErrors)
    reported = reported || (e.device == c.wt && e.message.find("only after a meta instrument") != std::string::npos);
  CHECK(reported);
  CHECK_FALSE(trackPatch(*lp, c.track).findByName("d" + std::to_string(c.wt.value)).has_value());
  c.edit(ops::moveDevice(c.doc(), c.wt, c.track, 1));
  Result<LoweredProject> back = lowerIt(c.doc());
  REQUIRE(back.ok());
  CHECK(back->deviceErrors.empty());
  CHECK(trackPatch(*back, c.track).findByName("d" + std::to_string(c.wt.value)).has_value());
}

TEST_CASE("meta devices: copy / paste, duplicate and a preset keep the state (params, settings, data)") {
  MetaChain c;
  // Some state: a param, a setting, data (two keyframes).
  c.edit(ops::setMetaParam(c.doc(), c.wt, "", "wavetable", "position", 0.4));
  c.edit(ops::setMetaSetting(c.doc(), c.wt, "", "wavetable", "warpMode", 2.0));
  auto parsed = meta::parseWavetableData(*meta::wavetableDefaultData());
  REQUIRE(parsed.ok());
  meta::WavetableData data = *parsed;
  data.keyframes.push_back(data.keyframes.front());
  data.keyframes.back().position = 1.0f;
  data.gaps.push_back(pq::dsp::GapMode::Crossfade);
  for (float& v : data.keyframes.back().cycle) v *= 0.5f;
  c.edit(ops::setMetaData(c.doc(), c.wt, "", "wavetable", meta::wavetableData(data)));
  const Device& src = c.dev(c.wt);
  REQUIRE(src.meta);
  // Paste (the Device view's clipboard holds DevicePtrs): a new id, the same state.
  const DevicePtr copy = std::make_shared<Device>(src);
  const DeviceId pasted = c.edit(ops::pasteDevices(c.doc(), c.store.ids(), c.track, std::span<const DevicePtr>(&copy, 1), 2)).devices.at(0);
  const Device& p = c.dev(pasted);
  CHECK(p.id != src.id);
  CHECK(p.patch == src.patch);
  CHECK(p.kind == DeviceKind::AudioEffect);
  CHECK(p.params == src.params);
  CHECK(p.settings == src.settings);
  REQUIRE(p.meta);
  CHECK(*p.meta == *src.meta);
  // A preset file round trip (the library's format: the project's device writer and reader).
  auto written = serializePreset(src, "My Table");
  REQUIRE(written.ok());
  const std::string text = *written;
  auto back = parsePreset(text, "My Table.pqpreset");
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  REQUIRE(back->device);
  CHECK(back->device->patch == src.patch);
  CHECK(back->device->kind == src.kind);
  CHECK(back->device->params == src.params);
  CHECK(back->device->settings == src.settings);
  REQUIRE(back->device->meta);
  CHECK(*back->device->meta == *src.meta);
}
