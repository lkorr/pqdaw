// TMM.10: meta modules as chain devices — voice continuity (a meta voice run: Envelope → Wavetable → Auto
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
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/graph/atoms.h"
#include "pq/graph/infer.h"
#include "pq/graph/parse.h"
#include "pq/meta/envelope.h"
#include "pq/meta/wavetable.h"
#include "../support/stand_in_synth.h"

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

// IG.1-2: a new MIDI track's default instrument out of its instrument group, alone on the path (so a meta module added
// at slot 0 replaces it, as these cases were written).
void ungroupHead(DocumentStore& s, TrackId t) {
  (void)stand_in::useBasicSynth(s, t);  // (GU.10: the IG.1-2 layout first: Basic Synth in the group)
  const Track& tr = *findTrackPtr(*s.current(), t);
  DeviceId g;
  for (const DevicePtr& d : tr.devices)
    if (isGroupDevice(*d)) g = d->id;
  if (!g.valid()) return;
  auto e = ops::ungroupInCircuit(s.current(), s.ids(), library(), g);
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(s.dispatch(e->command).ok());
}

// A MIDI track with Envelope → Wavetable → Auto Filter on its path (outside any instrument group: the run's stages as
// chainStages reads them; groupRun puts the two meta modules into an instrument group). GU.4: `acc` was a Phase
// accumulator (a group preset now): the Envelope takes the note source the same way (any meta module first does).
struct MetaChain {
  DocumentStore store;
  TrackId track;
  DeviceId acc, wt, filter;
  MetaChain() {
    track = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0);
    ungroupHead(store, track);
    acc = edit(ops::addMetaDevice(doc(), store.ids(), track, "envelope", 0)).devices.at(0);
    wt = edit(ops::addMetaDevice(doc(), store.ids(), track, "wavetable")).devices.at(0);
    filter = edit(ops::addDevice(doc(), store.ids(), track, "auto_filter", DeviceKind::AudioEffect)).devices.at(0);
  }
  const Project& doc() const { return *store.current(); }
  const Device& dev(DeviceId d) const { return *findDevice(doc(), d).device; }
  const Track& tr() const { return *findTrackPtr(doc(), track); }
  // IG.1-2: Ctrl+G on the run (Envelope → Wavetable): an instrument group at the note source; they run per
  // note inside it.
  DeviceId groupRun() {
    const DeviceId run[2] = {acc, wt};
    return edit(ops::groupInCircuit(store.current(), store.ids(), library(), run)).devices.at(0);
  }
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

// The stages of a track's blocks along its main path (doc::trackStages, from the note source on a MIDI track).
std::vector<ChainStage> stagesOf(const Project& p, const Track& t) { return trackStages(p, t); }
ChainSignal inOf(const Project& p, const Track& t, DeviceId d) {
  for (const ChainStage& s : stagesOf(p, t))
    if (s.device->id == d) return s.in;
  FAIL("no stage");
  return ChainSignal::None;
}
// TX.all-free: the track's main path as a list (what chainSignalAt reads).
std::vector<DevicePtr> pathList(const Project& p, const Track& t) {
  const MainPath path = mainPath(p, t);
  std::vector<DevicePtr> l = flowOrder(t, path);
  l.resize(path.blocks.size());
  return l;
}

}  // namespace

TEST_CASE("meta devices: what reaches each main input along the path decides its form (TMM.10-fix1; TX.all-free)") {
  MetaChain c;
  CHECK(inOf(c.doc(), c.tr(), c.acc) == ChainSignal::Notes);  // first on the path: the note source reaches it
  CHECK(inOf(c.doc(), c.tr(), c.wt) == ChainSignal::Voices);    // the Envelope's per-voice signal
  CHECK(inOf(c.doc(), c.tr(), c.filter) == ChainSignal::Voices);  // a stock effect: summed at its (global) input
  CHECK(stagesOf(c.doc(), c.tr()).back().out == ChainSignal::Audio);
  CHECK(chainSignalAt(pathList(c.doc(), c.tr()), true, 2) == ChainSignal::Voices);
  CHECK(chainSignalAt(pathList(c.doc(), c.tr()), true, 3) == ChainSignal::Audio);
  // TX.all-free: on a track a meta block's kind means nothing (its cables decide): an audio effect, any slot.
  CHECK(*ops::metaDeviceKindAt(c.doc(), meta::wavetableType(), c.track, 2) == DeviceKind::AudioEffect);
  CHECK(*ops::metaDeviceKindAt(c.doc(), meta::envelopeType(), c.track, 0) == DeviceKind::AudioEffect);
  // A second Envelope after the first: its main input takes the first one's voice signal.
  const DeviceId acc2 = c.edit(ops::addMetaDevice(c.doc(), c.store.ids(), c.track, "envelope", 1)).devices.at(0);
  CHECK(inOf(c.doc(), c.tr(), acc2) == ChainSignal::Voices);
  // An inactive device is skipped: the next one takes what it would have.
  c.edit(ops::setDevicesActive(c.doc(), std::vector<DeviceId>{c.acc}, false));
  CHECK(inOf(c.doc(), c.tr(), acc2) == ChainSignal::Notes);
  c.edit(ops::setDevicesActive(c.doc(), std::vector<DeviceId>{c.acc}, true));
  // A meta device spliced in first takes the note source; the one that was first then receives its voices (no
  // replacing), and the notes again when it is dissolved.
  const DeviceId env = c.edit(ops::addMetaDevice(c.doc(), c.store.ids(), c.track, "envelope", 0)).devices.at(0);
  CHECK(mainPath(c.doc(), c.tr()).blocks.at(0)->id == env);
  CHECK(inOf(c.doc(), c.tr(), env) == ChainSignal::Notes);
  CHECK(inOf(c.doc(), c.tr(), c.acc) == ChainSignal::Voices);
  c.edit(ops::dissolveDevices(c.doc(), c.store.ids(), &library(), std::vector<DeviceId>{env}));
  CHECK(inOf(c.doc(), c.tr(), c.acc) == ChainSignal::Notes);
  // A stock instrument added to the track goes first (it takes the notes); the meta device follows it (its voices are
  // summed at the instrument's output: the meta device receives audio).
  const DeviceId synth = c.edit(ops::addDevice(c.doc(), c.store.ids(), c.track, "BasicSynth", DeviceKind::Instrument)).devices.at(0);
  CHECK(mainPath(c.doc(), c.tr()).blocks.at(0)->id == synth);
  CHECK(inOf(c.doc(), c.tr(), c.acc) == ChainSignal::Audio);
  // …and a meta device asked for slot 0 takes the note source there: the stock instrument (which could not follow it:
  // it takes no main-input signal but the notes) is replaced.
  {
    const ops::Edit add = *ops::addMetaDevice(c.doc(), c.store.ids(), c.track, "envelope", 0);
    const Batch& b = std::get<Batch>(add.command.v);
    bool removesSynth = false;
    for (const Command& x : b.commands)
      if (const RemoveDevice* r = std::get_if<RemoveDevice>(&x.v)) removesSynth = removesSynth || r->device == synth;
    CHECK(removesSynth);
  }
  // Moving the Wavetable to the head of the path (the Device view's reorder): it takes the notes now.
  c.edit(ops::dissolveDevices(c.doc(), c.store.ids(), &library(), std::vector<DeviceId>{synth}));
  c.edit(ops::moveOnPath(c.doc(), c.store.ids(), &library(), c.wt, c.track, 0));
  CHECK(mainPath(c.doc(), c.tr()).blocks.at(0)->id == c.wt);
  CHECK(inOf(c.doc(), c.tr(), c.wt) == ChainSignal::Notes);
  CHECK(inOf(c.doc(), c.tr(), c.acc) == ChainSignal::Voices);
  // An audio track: everything receives audio.
  const TrackId audio = c.edit(ops::createTrack(c.doc(), c.store.ids(), TrackKind::Audio)).tracks.at(0);
  const DeviceId fm = c.edit(ops::addMetaDevice(c.doc(), c.store.ids(), audio, "wavetable")).devices.at(0);
  CHECK(inOf(c.doc(), *findTrackPtr(c.doc(), audio), fm) == ChainSignal::Audio);
  // Unplugged: nothing reaches it, it runs once.
  c.edit(ops::setDeviceChainInput(c.doc(), fm, std::string(kChainUnplugged)));
  CHECK(inOf(c.doc(), *findTrackPtr(c.doc(), audio), fm) == ChainSignal::None);
  CHECK(chainForm(stagesOf(c.doc(), *findTrackPtr(c.doc(), audio)).back()) == meta::Form::ChainAudio);
  CHECK_FALSE(ops::setDeviceChainInput(c.doc(), fm, "nope").ok());
  CHECK_FALSE(ops::setDeviceChainInput(c.doc(), c.filter, "-").ok());  // only chain meta devices
  // Plugging into the type's own input stores "" (the default).
  c.edit(ops::setDeviceChainInput(c.doc(), fm, "x"));
  CHECK(c.dev(fm).chainInput.empty());
}

TEST_CASE("meta devices: a voice run lowers per voice — calls in a row, a release, then the sum (golden)") {
  MetaChain c;
  c.groupRun();  // IG.1-2: per voice inside its instrument group (no voice runs on a path)
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
  // IG.1-2: the Wavetable's expansion is its free form in the voice context (per voice where the notes reach it),
  // without a `done`; the group's release ramp ends the voices and its Out is summed.
  CHECK(text.find("voice_sum") != std::string::npos);
  CHECK(text.find("200 / sr") != std::string::npos);
  CHECK_FALSE(wt->findByName("done").has_value());
  // Its params are the device's own (the fast path lists them too).
  const std::vector<LoweredParam> fast = lowerParams(c.doc(), true, library());
  for (const char* name : {"phase", "position", "fold"}) {
    const std::string key = "dev" + std::to_string(c.wt.value) + ":" + name;
    CHECK(std::any_of(lp->params.begin(), lp->params.end(), [&](const LoweredParam& p) { return p.key == key; }));
    CHECK(std::any_of(fast.begin(), fast.end(), [&](const LoweredParam& p) { return p.key == key; }));
  }
  // Both modules' resources are listed (the Wavetable's table, the Envelope's tap).
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
  ungroupHead(store, t);
  auto e = ops::addMetaDevice(*store.current(), store.ids(), t, "wavetable", 0);
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  {  // IG.1-2: the lone meta instrument is a Wavetable alone in an instrument group
    const DeviceId one[1] = {e->devices.at(0)};
    auto g = ops::groupInCircuit(store.current(), store.ids(), library(), one);
    REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
    REQUIRE(store.dispatch(g->command).ok());
  }
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
  c.edit(ops::moveOnPath(c.doc(), c.store.ids(), &library(), c.wt, c.track, 3));  // TX.all-free: after the filter
  CHECK(inOf(c.doc(), c.tr(), c.wt) == ChainSignal::Audio);  // the filter's audio reaches it (TMM.10-fix1: chainStages)
  Result<LoweredProject> lp = lowerIt(c.doc());
  REQUIRE(lp.ok());
  CHECK(lp->deviceErrors.empty());
  CHECK(trackPatch(*lp, c.track).findByName("d" + std::to_string(c.wt.value)).has_value());
  c.edit(ops::moveOnPath(c.doc(), c.store.ids(), &library(), c.wt, c.track, 1));
  Result<LoweredProject> back = lowerIt(c.doc());
  REQUIRE(back.ok());
  CHECK(back->deviceErrors.empty());
  CHECK(trackPatch(*back, c.track).findByName("d" + std::to_string(c.wt.value)).has_value());
}

TEST_CASE("meta devices: copy / paste, duplicate and a preset keep the state (params, settings, data)") {
  MetaChain c;
  // Some state: a param, a setting, data (two keyframes).
  c.edit(ops::setMetaParamValue(c.doc(), c.wt, "", "wavetable", "position", 0.4));
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
  // (IG.1-2: a MIDI track's instrument out of its group: a meta module added at slot 0 replaces it, as written)
  TrackId track(TrackKind k) {
    const TrackId t = edit(ops::createTrack(doc(), store.ids(), k)).tracks.at(0);
    if (k == TrackKind::Midi) ungroupHead(store, t);
    return t;
  }
  // IG.1-2: devices (on the path from its head) Ctrl+G'd into an instrument group.
  DeviceId group(std::vector<DeviceId> ds) { return edit(ops::groupInCircuit(store.current(), store.ids(), library(), ds)).devices.at(0); }
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

TEST_CASE("meta devices (fix1): an unplugged Wavetable first on a MIDI track runs once, freely — no voices") {
  // (GU.4: was the Phase accumulator, a group preset now; the same holds for any meta module.)
  Doc d;
  const TrackId t = d.track(TrackKind::Midi);
  const DeviceId acc = d.meta(t, "wavetable", 0);  // replaces the default instrument
  d.edit(ops::setDeviceChainInput(d.doc(), acc, std::string(kChainUnplugged)));
  Result<LoweredProject> lp = d.lowered();
  const pq::graph::Patch& pa = calleePatch(*lp, t, acc);
  CHECK_FALSE(hasInput(pa));                      // no note wire, no chain wire
  CHECK_FALSE(hasAtom(pa, "voice.hz"));           // no implicit note
  CHECK(outPoly(pa) == pq::graph::Poly::Global);  // one voice
  const std::string text = patchText(*lp->patches[trackIndex(*lp, t)]);
  CAPTURE(text);
  CHECK(text.find("voice_sum") == std::string::npos);
  // Nothing is wired into the call: its pins are its params (phase first), runtime params of the track.
  const pq::graph::Patch& tp = trackPatch(*lp, t);
  const auto call = tp.findByName("d" + std::to_string(acc.value));
  REQUIRE(call.has_value());
  for (uint32_t i = 0; i < tp.subcircuit(*call).inputs.size(); ++i)
    CHECK(tp.adjacency().driver[tp.flatInput(*call, i)] == pq::graph::kNone);
  CHECK(std::any_of(lp->params.begin(), lp->params.end(),
                    [&](const LoweredParam& p) { return p.key == "dev" + std::to_string(acc.value) + ":phase"; }));
  CHECK_FALSE(trackPatch(*lp, t).findByName("done").has_value());  // no voices to end
}

TEST_CASE("meta devices (fix1): a Wavetable on an audio track takes the audio on its phase input (a shaper)") {
  // (GU.4: was the Phase accumulator's FM; tests/app/test_module_presets.cpp renders its preset on an audio track.)
  Doc d;
  const TrackId t = d.track(TrackKind::Audio);
  const DeviceId acc = d.meta(t, "wavetable");
  CHECK(findDevice(d.doc(), acc).device->kind == DeviceKind::AudioEffect);
  Result<LoweredProject> lp = d.lowered();
  const pq::graph::Patch& pa = calleePatch(*lp, t, acc);
  const auto hz = pa.findByName("x");
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
  d.group({env});  // IG.1-2: per note inside an instrument group (its In gives the Envelope's `in` nothing: 1)
  Result<LoweredProject> lp = d.lowered();
  const std::string text = patchText(*lp->patches[trackIndex(*lp, t)]);
  CAPTURE(text);
  CHECK(text.find("200 / sr") == std::string::npos);  // no 5 ms ramp: the Envelope's done ends the voice
  CHECK(text.find(".done") != std::string::npos);
}

TEST_CASE("meta devices (fix1): after a stock instrument a meta device receives its (summed) audio") {
  Doc d;
  const TrackId t = d.track(TrackKind::Midi);  // BasicSynth first
  const DeviceId acc = d.meta(t, "wavetable");
  CHECK(findDevice(d.doc(), acc).device->kind == DeviceKind::AudioEffect);
  Result<LoweredProject> lp = d.lowered();
  const pq::graph::Patch& pa = calleePatch(*lp, t, acc);
  REQUIRE(pa.findByName("x").has_value());
  CHECK(pa.outputType(*pa.findByName("x"), 0).poly == pq::graph::Poly::Global);
  CHECK(outPoly(pa) == pq::graph::Poly::Global);
}

TEST_CASE("meta devices (fix1): the note's frequency wired to a filter cutoff in a circuit modulates it per voice") {
  // Inside a circuit the note source is an ordinary signal: voice.hz into any input — here a one-pole's cutoff — per
  // voice; a phasor hears the note only because voice.hz is wired into it.
  Doc d;
  const TrackId t = d.track(TrackKind::Midi);
  const DeviceId synth = d.doc().tracks.front()->devices.at(0)->id;
  const std::string text =
      "patch BasicSynth {\n  out out : audio voice mono\n  out done : bool voice mono\n"
      "  acc1 = phasor(voice.hz)\n  x = acc1 - 0.5\n"
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
  // first — it heard silence, no voices. Track B: Envelope → Wavetable, a voice run. Track C: a Wavetable
  // first on its own (an oscillator per note).
  Doc d;
  const TrackId a = d.track(TrackKind::Midi), b = d.track(TrackKind::Midi), c = d.track(TrackKind::Midi);
  const DeviceId wtA = d.meta(a, "wavetable");
  const DeviceId accB = d.meta(b, "envelope", 0), wtB = d.meta(b, "wavetable");
  const DeviceId wtC = d.meta(c, "wavetable", 0);
  Result<std::string> text = serializeProject(d.doc(), d.store.view());
  REQUIRE(text.ok());
  nlohmann::json j = nlohmann::json::parse(*text);
  j["schemaVersion"] = 10;
  j.erase("wires");  // TX.all-free: a schema-10 file has no main links (the 16 -> 17 step wires its chains)
  for (nlohmann::json& t : j["tracks"]) {
    nlohmann::json& devs = t["devices"];
    if (devs.size() == 2 && devs[0]["patch"] == "BasicSynth") devs.erase(devs.begin());  // track A's instrument
    // Schema 10's kinds were positions: a meta device first in a MIDI track's list (B's accumulator, C's Wavetable)
    // was an instrument (TX.all-free writes a track's meta blocks as audio effects).
    else if (!devs.empty() && devs[0]["patch"].get<std::string>().starts_with("meta:")) devs[0]["kind"] = "instrument";
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
  // (IG.1-2: the 18 -> 19 step wraps B's run and C's Wavetable into instrument groups: per note, their release ends
  // the voices; C's Wavetable reads the note's phase through its group's In.)
  CHECK(trackPatch(*lp, b).findByName("done").has_value());
  CHECK(findDevice(p, wtB).device->group.valid());
  CHECK(findDevice(p, wtC).device->group.valid());
  CHECK(trackPatch(*lp, c).findByName("done").has_value());
  // A re-save writes schema 11 with the chain input; it loads back equal.
  Result<std::string> again = serializeProject(p, old->view);
  REQUIRE(again.ok());
  CHECK(again->find("\"chainInput\": \"-\"") != std::string::npos);
  const Result<LoadedProject> back = parseProject(*again);
  REQUIRE(back.ok());
  CHECK(*back->project == p);
}

// ---- TMM.10-fix2: wires from the note source (SPEC §15 2026-11-01 "the note source is a Hz piano roll") -------------

namespace {

// The inferred type of what drives call `d<device>`'s input `pin` in track `t`'s patch (the modulated value: found in
// the typed patch by its NodeId, the calls being flattened there).
pq::graph::PortType pinType(const LoweredProject& lp, TrackId t, DeviceId device, std::string_view pin) {
  const pq::graph::Patch& tp = trackPatch(lp, t);
  const auto call = tp.findByName("d" + std::to_string(device.value));
  REQUIRE(call.has_value());
  const auto in = tp.findInput(*call, pin);
  REQUIRE(in.has_value());
  const pq::graph::WireIndex w = tp.adjacency().driver[tp.flatInput(*call, *in)];
  REQUIRE(w != pq::graph::kNone);
  const pq::graph::PortRef from = tp.wire(w).from;
  pq::graph::InferOptions io;
  io.library = lp.graph.library;
  auto typed = pq::graph::infer(tp, io);
  REQUIRE_MESSAGE(typed.ok(), (typed.ok() ? std::string() : typed.error().message));
  for (pq::graph::NodeIndex n = 0; n < typed->patch.nodeCount(); ++n) {
    const pq::graph::NodeOrigin& o = typed->origin[n];
    if (o.instance == 0 && o.what == pq::graph::NodeOrigin::What::Node && o.node == from.node) return typed->outputType(n, from.port);
  }
  FAIL("the driver is not in the typed patch");
  return {};
}

WireStatus statusOf(const LoweredProject& lp, WireId w) {
  for (const WireState& s : lp.wires)
    if (s.id == w) return s.status;
  FAIL("no wire state");
  return WireStatus::Invalid;
}

}  // namespace

TEST_CASE("meta devices (fix2): the note source's Hz wired to a meta param of its voice run modulates it per voice") {
  MetaChain c;  // Envelope (at the note source) → Wavetable (its voice run) → Auto Filter
  c.groupRun();  // IG.1-2: the run in its instrument group
  const WireId w = c.edit(ops::connectTravelNoteWire(c.doc(), c.store.ids(), c.track, "hz", c.wt, "position")).wires.at(0);
  Result<LoweredProject> lp = lowerIt(c.doc());
  REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
  CHECK(lp->deviceErrors.empty());
  CHECK(statusOf(*lp, w) == WireStatus::Lowered);
  const std::string text = patchText(*lp->patches[trackIndex(*lp, c.track)]);
  CAPTURE(text);
  // No reducer anywhere: the Wavetable runs per voice, so the param the note drives is per voice.
  for (const char* r : {"voice_last(", "voice_highest(", "voice_lowest(", "voice_loudest("}) CHECK(text.find(r) == std::string::npos);
  CHECK(pinType(*lp, c.track, c.wt, "position").poly == pq::graph::Poly::Voice);
  // The wire's amount is a param (an amount drag never recompiles); the modulated param has its tap (the live ring).
  const std::string amount = "wire" + std::to_string(w.value) + ":amount";
  CHECK(std::any_of(lp->params.begin(), lp->params.end(), [&](const LoweredParam& p) { return p.key == amount; }));
  CHECK(std::any_of(lp->taps.begin(), lp->taps.end(), [&](const ModulationTap& t) { return t.device == c.wt && t.param == "position"; }));
}

TEST_CASE("meta devices (fix2): the note source into a stock (global) device param goes through the wire's reducer") {
  MetaChain c;
  const WireId hz = c.edit(ops::connectTravelNoteWire(c.doc(), c.store.ids(), c.track, "hz", c.filter, "freq")).wires.at(0);
  const WireId vel = c.edit(ops::connectTravelNoteWire(c.doc(), c.store.ids(), c.track, "velocity", c.filter, "res")).wires.at(0);
  const WireId gate = c.edit(ops::connectTravelNoteWire(c.doc(), c.store.ids(), c.track, "gate", c.filter, "drive")).wires.at(0);
  c.edit(ops::setWireReducer(c.doc(), vel, WireReducer::Highest));
  Result<LoweredProject> lp = lowerIt(c.doc());
  REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
  for (const WireId w : {hz, vel, gate}) CHECK(statusOf(*lp, w) == WireStatus::Lowered);
  const std::string text = patchText(*lp->patches[trackIndex(*lp, c.track)]);
  CAPTURE(text);
  CHECK(text.find("voice_highest(voice.velocity") != std::string::npos);       // the wire's own reducer
  CHECK(text.find("voice_sum(gate_to_real(voice.gate") != std::string::npos);  // a gate reads 0 / 1, summed
  for (const char* pin : {"freq", "res", "drive"}) CHECK(pinType(*lp, c.track, c.filter, pin).poly == pq::graph::Poly::Global);
  CHECK(text.find("voice_sum(voice.hz") != std::string::npos);  // no tuning node: voice.hz is what the devices hear
  // With tuning nodes (the app's lowering) "hz" is the last node's hz, as the devices hear it (tune_voices), not the
  // root's own voice.hz (the engine's).
  LowerOptions o;
  o.library = &library().library();
  o.devices = &library();
  o.slotOf = [](TrackId) { return 0u; };
  o.metaSlotOf = [](DeviceId d, std::string_view, uint32_t i) { return d.value * 4 + i + 1; };
  o.tuningTableOf = [](TuningId, const TuningDef&) { return 0u; };
  Result<LoweredProject> tuned = lower(c.doc(), o);
  REQUIRE_MESSAGE(tuned.ok(), (tuned.ok() ? std::string() : tuned.error().message));
  const std::string tt = patchText(*tuned->patches[trackIndex(*tuned, c.track)]);
  CAPTURE(tt);
  REQUIRE(tt.find("tune_voices(") != std::string::npos);
  CHECK(tt.find("voice_sum(voice.hz") == std::string::npos);
  CHECK(pinType(*tuned, c.track, c.filter, "freq").poly == pq::graph::Poly::Global);
}

TEST_CASE("meta devices (fix2): a note-source wire plays only while its track has voices; across tracks it leaves reduced") {
  Doc d;
  const TrackId a = d.track(TrackKind::Midi);  // BasicSynth: voices
  const TrackId b = d.track(TrackKind::Audio);
  const DeviceId fx = d.edit(ops::addDevice(d.doc(), d.store.ids(), b, "auto_filter", DeviceKind::AudioEffect)).devices.at(0);
  const WireId w = d.edit(ops::connectTravelNoteWire(d.doc(), d.store.ids(), a, "velocity", fx, "freq")).wires.at(0);
  {
    Result<LoweredProject> lp = d.lowered();
    CHECK(statusOf(*lp, w) == WireStatus::Lowered);
    const std::string m = "m" + std::to_string(w.value);
    bool modWire = false;
    for (const pq::graph::ProjectWire& gw : lp->graph.wires)
      modWire = modWire || (gw.kind == pq::graph::WireKind::Modulation && gw.from.port == m && gw.to.port == m);
    CHECK(modWire);
    const std::string src = patchText(*lp->patches[trackIndex(*lp, a)]);
    CAPTURE(src);
    CHECK(src.find("voice_sum(voice.velocity") != std::string::npos);  // reduced in the source track's patch
    CHECK(trackPatch(*lp, a).findByName(m).has_value());                 // and leaves it as export m<wire>
  }
  // IG.1-2: without an instrument the MIDI track's notes are still read (a note output is a signal of the notes,
  // whatever plays them): the wire stays Lowered, reduced by its reducer.
  const DeviceId synth[1] = {findTrackPtr(d.doc(), a)->devices.at(0)->id};
  d.edit(ops::deleteDevices(d.doc(), synth));
  {
    Result<LoweredProject> lp = d.lowered();
    CHECK(statusOf(*lp, w) == WireStatus::Lowered);
  }
  // A note-source wire comes only from a MIDI track, from one of its note outputs.
  CHECK_FALSE(ops::connectTravelNoteWire(d.doc(), d.store.ids(), b, "hz", fx, "freq").ok());
  CHECK_FALSE(ops::connectTravelNoteWire(d.doc(), d.store.ids(), a, "pitch", fx, "freq").ok());
}

// Regression (bug report 2026-11-01_12-15-15, after TMM.10-fix1): meta → meta → … → stock chains lower to patches the
// compiler accepts — every track patch infers (no width mismatch inside an expansion), two Wavetables in a row included.
TEST_CASE("meta devices: meta → meta → stock chains (two Wavetables in a row) lower and infer") {
  const std::vector<std::vector<std::string>> chains = {
      // (GU.4: the report's Phase accumulator is an Envelope here: a meta module first, its signal into the next.)
      {"envelope", "wavetable", "wavetable", "utility"},      // the report's chain
      {"envelope", "wavetable", "wavetable"},
      {"envelope", "wavetable", "utility"},
      {"envelope", "wavetable", "envelope", "wavetable", "utility"},
      {"wavetable", "wavetable", "utility"},
      {"envelope", "envelope", "wavetable", "auto_filter"},
      // A meta device after a stereo stock effect (ChainAudio): the stereo signal reaches a module whose outputs are
      // mono — the report's state (a Wavetable after a stock device, deleted before the shot). Lowered as the mid.
      {"envelope", "wavetable", "utility", "wavetable"},
      {"envelope", "wavetable", "auto_filter", "wavetable", "utility"},
      {"envelope", "wavetable", "utility", "envelope", "utility"},
      {"envelope", "wavetable", "utility", "envelope"},
  };
  for (const std::vector<std::string>& chain : chains) {
    std::string name;
    for (const std::string& s : chain) name += s + " ";
    CAPTURE(name);
    Doc d;
    const TrackId t = d.track(TrackKind::Midi);
    bool first = true;
    for (const std::string& s : chain) {
      if (s == "utility" || s == "auto_filter") d.edit(ops::addDevice(d.doc(), d.store.ids(), t, s, DeviceKind::AudioEffect));
      else (void)d.meta(t, s.c_str(), first ? 0u : kAppend);
      first = false;
    }
    Result<LoweredProject> lp = d.lowered();
    CHECK(lp->missingDevices.empty());
    pq::graph::InferOptions io;
    io.library = lp->graph.library;
    auto typed = pq::graph::infer(trackPatch(*lp, t), io);
    CHECK_MESSAGE(typed.ok(), (typed.ok() ? std::string() : typed.error().message));
  }
}

// TMM.13-fix1 L6 / L8: the reader kept state nothing used — nested paths on a chain meta device (its expansion calls
// no nested modules) — and saved it back forever; and the device's own state (path "") printed as `meta[0]` in errors.
TEST_CASE("meta devices: a chain meta device holds only its own state; errors there name meta[\"\"] (TMM.13-fix1 L6, L8)") {
  MetaChain c;
  Result<std::string> text = serializeProject(c.doc(), c.store.view());
  REQUIRE(text.ok());
  nlohmann::json j = nlohmann::json::parse(*text);
  nlohmann::json* wt = nullptr;
  for (nlohmann::json& d : j["tracks"][0]["devices"])
    if (d["id"] == c.wt.value) wt = &d;
  REQUIRE(wt != nullptr);
  REQUIRE(wt->contains("meta"));
  REQUIRE((*wt)["meta"].contains(""));
  {  // a nested path beside its own state: refused
    nlohmann::json k = j;
    for (nlohmann::json& d : k["tracks"][0]["devices"])
      if (d["id"] == c.wt.value) d["meta"]["osc"] = d["meta"][""];
    Result<LoadedProject> r = parseProject(k.dump(2));
    REQUIRE_FALSE(r.ok());
    MESSAGE(r.error().message);
    CHECK(r.error().message.find("only its own state") != std::string::npos);
  }
  {  // its own state with params of its own (they are the device's): refused at meta[""]
    nlohmann::json k = j;
    for (nlohmann::json& d : k["tracks"][0]["devices"])
      if (d["id"] == c.wt.value) d["meta"][""]["params"] = nlohmann::json::object({{"position", 0.5}});
    Result<LoadedProject> r = parseProject(k.dump(2));
    REQUIRE_FALSE(r.ok());
    MESSAGE(r.error().message);
    CHECK(r.error().message.find("meta[\"\"]") != std::string::npos);
    CHECK(r.error().message.find("meta[0]") == std::string::npos);
  }
  // The store refuses the same (a command building such a device).
  auto d = std::make_shared<Device>(c.dev(c.wt));
  MetaMap m = *d->meta;
  m.push_back(MetaEntry{"osc", m.front().instance});
  d->meta = std::make_shared<const MetaMap>(std::move(m));
  d->id = c.store.ids().device();
  CHECK_FALSE(c.store.dispatch(AddDevice{c.track, d, kAppend}).ok());
}
