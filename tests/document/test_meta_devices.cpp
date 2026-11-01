// TMM.10: meta modules as chain devices — voice continuity (a meta voice run: Phase accumulator → Wavetable → Auto
// Filter lowers to per-voice calls, a release, then the voice sum; golden text), the kinds addMetaDevice picks, a meta
// effect outside a run (bypassed with a reason), the param fast path, copy / paste / presets keep the state.
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

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
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/graph/atoms.h"
#include "pq/graph/infer.h"
#include "pq/graph/parse.h"
#include "pq/meta/envelope.h"
#include "pq/meta/phase_acc.h"
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

namespace {

// The stages of a track's own chain (doc::chainStages from the note source on a MIDI track).
std::vector<ChainStage> stagesOf(const Track& t) { return chainStages(t.devices, t.kind == TrackKind::Midi); }
ChainSignal inOf(const Track& t, DeviceId d) {
  for (const ChainStage& s : stagesOf(t))
    if (s.device->id == d) return s.in;
  FAIL("no stage");
  return ChainSignal::None;
}

}  // namespace

TEST_CASE("meta devices: what reaches each chain input decides its form — kinds are positions (TMM.10-fix1)") {
  MetaChain c;
  CHECK(c.dev(c.acc).kind == DeviceKind::Instrument);  // first: the note source reaches it
  CHECK(c.dev(c.wt).kind == DeviceKind::AudioEffect);
  CHECK(inOf(c.tr(), c.acc) == ChainSignal::Notes);
  CHECK(inOf(c.tr(), c.wt) == ChainSignal::Voices);    // the Phase accumulator's per-voice phase
  CHECK(inOf(c.tr(), c.filter) == ChainSignal::Voices);  // a stock effect: summed at its (global) input
  CHECK(stagesOf(c.tr()).back().out == ChainSignal::Audio);
  CHECK(chainSignalAt(c.tr().devices, true, 2) == ChainSignal::Voices);
  CHECK(chainSignalAt(c.tr().devices, true, 3) == ChainSignal::Audio);
  // Any type, any slot: the kind is only the position.
  CHECK(*ops::metaDeviceKindAt(c.doc(), meta::wavetableType(), c.track, 2) == DeviceKind::AudioEffect);
  CHECK(*ops::metaDeviceKindAt(c.doc(), meta::envelopeType(), c.track, kAppend) == DeviceKind::AudioEffect);
  CHECK(*ops::metaDeviceKindAt(c.doc(), meta::envelopeType(), c.track, 0) == DeviceKind::Instrument);
  // A second Phase accumulator after the first: its chain input (frequency) takes the first one's voice signal.
  const DeviceId acc2 = c.edit(ops::addMetaDevice(c.doc(), c.store.ids(), c.track, "phase_acc", 1)).devices.at(0);
  CHECK(c.dev(acc2).kind == DeviceKind::AudioEffect);
  CHECK(inOf(c.tr(), acc2) == ChainSignal::Voices);
  // An inactive device is skipped: the next one takes what it would have.
  c.edit(ops::setDevicesActive(c.doc(), std::vector<DeviceId>{c.acc}, false));
  CHECK(inOf(c.tr(), acc2) == ChainSignal::Notes);
  c.edit(ops::setDevicesActive(c.doc(), std::vector<DeviceId>{c.acc}, true));
  // A meta device inserted first takes the note source; the one that was first becomes an audio effect (no
  // replacing), and back when it is removed.
  const DeviceId env = c.edit(ops::addMetaDevice(c.doc(), c.store.ids(), c.track, "envelope", 0)).devices.at(0);
  CHECK(c.tr().devices.at(0)->id == env);
  CHECK(c.dev(env).kind == DeviceKind::Instrument);
  CHECK(c.dev(c.acc).kind == DeviceKind::AudioEffect);
  CHECK(inOf(c.tr(), c.acc) == ChainSignal::Voices);
  c.edit(ops::deleteDevices(c.doc(), std::vector<DeviceId>{env}));
  CHECK(c.dev(c.acc).kind == DeviceKind::Instrument);
  // A stock instrument added to the track goes first; the meta device follows it (its voices are summed at the
  // instrument's output: the meta device receives audio).
  const DeviceId synth = c.edit(ops::addDevice(c.doc(), c.store.ids(), c.track, "BasicSynth", DeviceKind::Instrument)).devices.at(0);
  CHECK(c.tr().devices.at(0)->id == synth);
  CHECK(c.dev(c.acc).kind == DeviceKind::AudioEffect);
  CHECK(inOf(c.tr(), c.acc) == ChainSignal::Audio);
  // …and a meta device asked for slot 0 takes the note source there: the stock instrument (which could not follow it:
  // it takes no chain signal) is replaced.
  CHECK(*ops::metaDeviceKindAt(c.doc(), meta::phaseAccType(), c.track, 0) == DeviceKind::Instrument);
  {
    const ops::Edit add = *ops::addMetaDevice(c.doc(), c.store.ids(), c.track, "envelope", 0);
    const Batch& b = std::get<Batch>(add.command.v);
    REQUIRE(b.commands.size() == 2);
    CHECK(std::get<RemoveDevice>(b.commands[0].v).device == synth);
  }
  // Moving the meta device to the front of a list re-kinds both.
  c.edit(ops::deleteDevices(c.doc(), std::vector<DeviceId>{synth}));
  c.edit(ops::moveDevice(c.doc(), c.wt, c.track, 0));
  CHECK(c.dev(c.wt).kind == DeviceKind::Instrument);
  CHECK(c.dev(c.acc).kind == DeviceKind::AudioEffect);
  // An audio track: everything receives audio.
  const TrackId audio = c.edit(ops::createTrack(c.doc(), c.store.ids(), TrackKind::Audio)).tracks.at(0);
  const DeviceId fm = c.edit(ops::addMetaDevice(c.doc(), c.store.ids(), audio, "phase_acc")).devices.at(0);
  CHECK(c.dev(fm).kind == DeviceKind::AudioEffect);
  CHECK(inOf(*findTrackPtr(c.doc(), audio), fm) == ChainSignal::Audio);
  // Unplugged: nothing reaches it, it runs once.
  c.edit(ops::setDeviceChainInput(c.doc(), fm, std::string(kChainUnplugged)));
  CHECK(inOf(*findTrackPtr(c.doc(), audio), fm) == ChainSignal::None);
  CHECK(chainForm(stagesOf(*findTrackPtr(c.doc(), audio)).back()) == meta::Form::ChainAudio);
  CHECK_FALSE(ops::setDeviceChainInput(c.doc(), fm, "nope").ok());
  CHECK_FALSE(ops::setDeviceChainInput(c.doc(), c.filter, "-").ok());  // only chain meta devices
  // Plugging into the type's own input stores "" (the default).
  c.edit(ops::setDeviceChainInput(c.doc(), fm, "hz"));
  CHECK(c.dev(fm).chainInput.empty());
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

TEST_CASE("meta devices: a Wavetable moved out of its run becomes a chain audio effect; moved back, it continues the run") {
  MetaChain c;
  // After the Auto Filter: no run. TMM.8: the Wavetable has an effect form (the chain's audio is its x: in Phase mode
  // the signal is read as a phase; Shape mode shapes it), so it plays as an effect instead of being bypassed.
  c.edit(ops::moveDevice(c.doc(), c.wt, c.track, 2));
  CHECK(inOf(c.tr(), c.wt) == ChainSignal::Audio);  // the filter's audio reaches it (TMM.10-fix1: chainStages)
  Result<LoweredProject> lp = lowerIt(c.doc());
  REQUIRE(lp.ok());
  CHECK(lp->deviceErrors.empty());
  CHECK(trackPatch(*lp, c.track).findByName("d" + std::to_string(c.wt.value)).has_value());
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

TEST_CASE("meta devices: the Meta Operator (TMM.12) lowers each nested instance to its own expansion, with its slots") {
  DocumentStore store;
  auto run = [&](Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  };
  const TrackId t = run(ops::createTrack(*store.current(), store.ids(), TrackKind::Midi)).tracks.at(0);
  const DeviceId op = run(ops::addDevice(*store.current(), store.ids(), t, "meta_operator", DeviceKind::Instrument)).devices.at(0);
  REQUIRE(library().catalog().find("meta_operator") != nullptr);
  CHECK(library().catalog().find("meta_operator")->name == "Meta Operator");
  run(ops::setMetaParam(*store.current(), op, "envB", "envelope", "amount", 0.25));
  run(ops::setDeviceSetting(*store.current(), op, "algorithm", 5.0));
  run(ops::setDeviceSetting(*store.current(), op, "hq", 2.0));
  run(ops::setDeviceSetting(*store.current(), op, "fbOn", 1.0));
  CHECK_FALSE(findDevice(*store.current(), op).device->edited());  // a nested state edit never forks the stock device
  Result<LoweredProject> lp = lowerIt(*store.current());
  REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
  for (const LoweredProject::DeviceError& e : lp->deviceErrors) FAIL_CHECK(e.message);
  // Twelve instances with resources (every one a voice tap; the Wavetables a table too).
  std::vector<std::string> paths;
  for (const LoweredProject::MetaResource& r : lp->metaResources)
    if (r.device == op && std::find(paths.begin(), paths.end(), r.path) == paths.end()) paths.push_back(r.path);
  CHECK(paths.size() == 12);
  for (const char* p : {"accA", "wtA", "envA", "accD", "wtD", "envD"})
    CHECK_MESSAGE(std::find(paths.begin(), paths.end(), std::string(p)) != paths.end(), p);
  // The device's patch is a specialised copy whose meta calls are the instances' expansions.
  const pq::graph::Patch* dp = lp->graph.library->find(calleeOf(*lp, t, op));
  REQUIRE(dp != nullptr);
  const auto acc = dp->findByName("accA");
  REQUIRE(acc.has_value());
  CHECK(dp->subcircuit(*acc).patch.starts_with("meta_phase_acc__d" + std::to_string(op.value) + "_accA_"));
}

// ---- TMM.10-fix1: notes are a signal; a chain is wires (SPEC §15 2026-11-01) ----------------------------------------

namespace {

struct Doc {
  DocumentStore store;
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  TrackId track(TrackKind k) { return edit(ops::createTrack(doc(), store.ids(), k)).tracks.at(0); }
  DeviceId meta(TrackId t, const char* type, uint32_t index = kAppend) {
    return edit(ops::addMetaDevice(doc(), store.ids(), t, type, index)).devices.at(0);
  }
  Result<LoweredProject> lowered() {
    Result<LoweredProject> lp = lowerIt(doc());
    REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
    for (const LoweredProject::DeviceError& e : lp->deviceErrors) FAIL_CHECK(e.message);
    return lp;
  }
};

const pq::graph::Patch& calleePatch(const LoweredProject& lp, TrackId t, DeviceId d) {
  const pq::graph::Patch* p = lp.graph.library->find(calleeOf(lp, t, d));
  REQUIRE(p != nullptr);
  return *p;
}
bool hasAtom(const pq::graph::Patch& p, std::string_view atom) {
  for (pq::graph::NodeIndex n = 0; n < p.nodeCount(); ++n)
    if (p.node(n).kind == pq::graph::NodeKind::Atom && p.node(n).atom == pq::graph::atomId(atom)) return true;
  return false;
}
bool hasInput(const pq::graph::Patch& p) {
  for (pq::graph::NodeIndex n = 0; n < p.nodeCount(); ++n)
    if (p.node(n).kind == pq::graph::NodeKind::Input) return true;
  return false;
}
pq::graph::Poly outPoly(const pq::graph::Patch& p) {
  const auto o = p.findByName("out");
  REQUIRE(o.has_value());
  return p.inputType(*o, 0).poly;
}

}  // namespace

TEST_CASE("meta devices (fix1): an unplugged Phase accumulator first on a MIDI track runs once, freely — no voices") {
  Doc d;
  const TrackId t = d.track(TrackKind::Midi);
  const DeviceId acc = d.meta(t, "phase_acc", 0);  // replaces the default instrument
  d.edit(ops::setDeviceChainInput(d.doc(), acc, std::string(kChainUnplugged)));
  Result<LoweredProject> lp = d.lowered();
  const pq::graph::Patch& pa = calleePatch(*lp, t, acc);
  CHECK_FALSE(hasInput(pa));                      // no note wire, no chain wire
  CHECK_FALSE(hasAtom(pa, "voice.hz"));           // no implicit note
  CHECK(outPoly(pa) == pq::graph::Poly::Global);  // one voice
  const std::string text = patchText(*lp->patches[trackIndex(*lp, t)]);
  CAPTURE(text);
  CHECK(text.find("voice_sum") == std::string::npos);
  // Nothing is wired into the call: its pins are its params (freq first), runtime params of the track.
  const pq::graph::Patch& tp = trackPatch(*lp, t);
  const auto call = tp.findByName("d" + std::to_string(acc.value));
  REQUIRE(call.has_value());
  for (uint32_t i = 0; i < tp.subcircuit(*call).inputs.size(); ++i)
    CHECK(tp.adjacency().driver[tp.flatInput(*call, i)] == pq::graph::kNone);
  CHECK(std::any_of(lp->params.begin(), lp->params.end(),
                    [&](const LoweredParam& p) { return p.key == "dev" + std::to_string(acc.value) + ":freq"; }));
  CHECK_FALSE(trackPatch(*lp, t).findByName("done").has_value());  // no voices to end
}

TEST_CASE("meta devices (fix1): a Phase accumulator on an audio track takes the audio on its frequency (FM)") {
  Doc d;
  const TrackId t = d.track(TrackKind::Audio);
  const DeviceId acc = d.meta(t, "phase_acc");
  CHECK(findDevice(d.doc(), acc).device->kind == DeviceKind::AudioEffect);
  Result<LoweredProject> lp = d.lowered();
  const pq::graph::Patch& pa = calleePatch(*lp, t, acc);
  const auto hz = pa.findByName("hz");
  REQUIRE(hz.has_value());
  CHECK(pa.node(*hz).kind == pq::graph::NodeKind::Input);
  CHECK(pa.outputType(*hz, 0).poly == pq::graph::Poly::Global);
  CHECK(outPoly(pa) == pq::graph::Poly::Global);
  // The call's chain input is wired (the track's signal so far).
  const pq::graph::Patch& tp = trackPatch(*lp, t);
  const auto call = tp.findByName("d" + std::to_string(acc.value));
  REQUIRE(call.has_value());
  CHECK(tp.adjacency().driver[tp.flatInput(*call, 0)] != pq::graph::kNone);
}

TEST_CASE("meta devices (fix1): an Envelope at the note source plays its envelope per note and ends the voices") {
  Doc d;
  const TrackId t = d.track(TrackKind::Midi);
  const DeviceId env = d.meta(t, "envelope", 0);
  Result<LoweredProject> lp = d.lowered();
  const pq::graph::Patch& ep = calleePatch(*lp, t, env);
  CHECK_FALSE(hasInput(ep));  // `in` reads its default 1: the note source carries no signal for it
  CHECK(outPoly(ep) == pq::graph::Poly::Voice);
  const std::string text = patchText(*lp->patches[trackIndex(*lp, t)]);
  CAPTURE(text);
  CHECK(text.find("200 / sr") == std::string::npos);  // no 5 ms ramp: the Envelope's done ends the voice
  CHECK(text.find(".done") != std::string::npos);
}

TEST_CASE("meta devices (fix1): after a stock instrument a meta device receives its (summed) audio") {
  Doc d;
  const TrackId t = d.track(TrackKind::Midi);  // BasicSynth first
  const DeviceId acc = d.meta(t, "phase_acc");
  CHECK(findDevice(d.doc(), acc).device->kind == DeviceKind::AudioEffect);
  Result<LoweredProject> lp = d.lowered();
  const pq::graph::Patch& pa = calleePatch(*lp, t, acc);
  REQUIRE(pa.findByName("hz").has_value());
  CHECK(pa.outputType(*pa.findByName("hz"), 0).poly == pq::graph::Poly::Global);
  CHECK(outPoly(pa) == pq::graph::Poly::Global);
}

TEST_CASE("meta devices (fix1): the note's frequency wired to a filter cutoff in a circuit modulates it per voice") {
  // Inside a circuit the note source is an ordinary signal: voice.hz into any input — here a one-pole's cutoff — per
  // voice; the Phase accumulator hears the note only because voice.hz is wired into its hz.
  Doc d;
  const TrackId t = d.track(TrackKind::Midi);
  const DeviceId synth = d.doc().tracks.front()->devices.at(0)->id;
  const std::string text =
      "patch BasicSynth {\n  out out : audio voice mono\n  out done : bool voice mono\n"
      "  acc1 = meta_phase_acc(voice.hz)\n  x = acc1 - 0.5\n"
      "  g = tan(3.141592653589793 * clip(voice.hz * 4, 0, 0.49 * sr) / sr)\n  G = g / (1 + g)\n"
      "  v = (x - s) * G\n  low = s + v\n  s = z1(low + v)\n  out = low * 0.2\n  done = !voice.gate\n}\n";
  d.edit(ops::editDevicePatch(d.doc(), synth, text));
  Result<LoweredProject> lp = d.lowered();
  pq::graph::InferOptions io;
  io.library = lp->graph.library;
  auto typed = pq::graph::infer(trackPatch(*lp, t), io);
  REQUIRE_MESSAGE(typed.ok(), (typed.ok() ? std::string() : typed.error().message));
  size_t tans = 0;
  for (pq::graph::NodeIndex n = 0; n < typed->patch.nodeCount(); ++n)
    if (typed->patch.node(n).kind == pq::graph::NodeKind::Atom && typed->patch.node(n).atom == pq::graph::atomId("tan")) {
      ++tans;
      CHECK(typed->inputType(n, 0).poly == pq::graph::Poly::Voice);  // the cutoff is the note's, per voice
    }
  CHECK(tans == 1);
}

TEST_CASE("meta devices (fix1): a schema-10 file keeps its meaning (migration 10 → 11)") {
  // Track A as a schema-10 build could leave it: the instrument deleted, a Wavetable (an audio effect after it) now
  // first — it heard silence, no voices. Track B: Phase accumulator → Wavetable, a voice run. Track C: a Wavetable
  // first on its own (an oscillator per note).
  Doc d;
  const TrackId a = d.track(TrackKind::Midi), b = d.track(TrackKind::Midi), c = d.track(TrackKind::Midi);
  const DeviceId wtA = d.meta(a, "wavetable");
  const DeviceId accB = d.meta(b, "phase_acc", 0), wtB = d.meta(b, "wavetable");
  const DeviceId wtC = d.meta(c, "wavetable", 0);
  Result<std::string> text = serializeProject(d.doc(), d.store.view());
  REQUIRE(text.ok());
  nlohmann::json j = nlohmann::json::parse(*text);
  j["schemaVersion"] = 10;
  for (nlohmann::json& t : j["tracks"]) {
    nlohmann::json& devs = t["devices"];
    if (devs.size() == 2 && devs[0]["patch"] == "BasicSynth") devs.erase(devs.begin());  // track A's instrument
  }
  const Result<LoadedProject> old = parseProject(j.dump(2));
  REQUIRE_MESSAGE(old.ok(), (old.ok() ? std::string() : old.error().message));
  CHECK(old->fileSchemaVersion == 10);
  const Project& p = *old->project;
  const Device& A = *findDevice(p, wtA).device;
  CHECK(A.chainInput == kChainUnplugged);        // still hears no note
  CHECK(A.kind == DeviceKind::Instrument);        // its position's kind
  CHECK(findDevice(p, accB).device->chainInput.empty());
  CHECK(findDevice(p, wtB).device->chainInput.empty());
  CHECK(findDevice(p, wtB).device->kind == DeviceKind::AudioEffect);
  CHECK(findDevice(p, wtC).device->chainInput.empty());
  CHECK(findDevice(p, wtC).device->kind == DeviceKind::Instrument);
  // Lowered: A plays once on nothing (no voices), B a voice run, C an oscillator per note — as in schema 10.
  Result<LoweredProject> lp = lowerIt(p);
  REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
  CHECK_FALSE(trackPatch(*lp, a).findByName("done").has_value());
  CHECK(outPoly(calleePatch(*lp, a, wtA)) == pq::graph::Poly::Global);
  CHECK(trackPatch(*lp, b).findByName("done").has_value());
  CHECK(outPoly(calleePatch(*lp, b, wtB)) == pq::graph::Poly::Voice);
  CHECK(hasAtom(calleePatch(*lp, c, wtC), "phasor"));  // the note's phase: phasor(voice.hz)
  // A re-save writes schema 11 with the chain input; it loads back equal.
  Result<std::string> again = serializeProject(p, old->view);
  REQUIRE(again.ok());
  CHECK(again->find("\"chainInput\": \"-\"") != std::string::npos);
  const Result<LoadedProject> back = parseProject(*again);
  REQUIRE(back.ok());
  CHECK(*back->project == p);
}
