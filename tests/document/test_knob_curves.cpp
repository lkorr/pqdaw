// GU.1 (docs/plans/GU.md "Curves"; SPEC §15 2026-11-06 "built GUIs"): the document side of control curves — the curve a
// declaration gives (graph::declaredCurve; the knob curve menus' presets over it), a device's own curves (model.h
// ControlCurve: SetParamCurve keeps the knob position — the sound follows —, one undo step, Structural only when the
// class changes, the file, the checks), the wire laws a curve decides (an Add wire adds in position on a curved control,
// in units on a linear one: dB on a dB gain), the 15 → 16 migration (TX.patching's added wires keep their law as Units)
// and 19 → 20 (values and TX.knob-curves' named curves become knob positions and curve objects, with the library).
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "../support/stand_in_synth.h"

using namespace pq::doc;
using pq::Result;
namespace dsp = pq::dsp;

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

const pq::graph::ParamDecl& declOf(const char* patch, const char* param) {
  const pq::graph::Patch* p = library().library().find(patch);
  REQUIRE(p != nullptr);
  const pq::graph::ParamDecl* d = findParamDecl(*p, param);
  REQUIRE(d != nullptr);
  return *d;
}

struct Doc {
  DocumentStore store;
  TrackId keys;
  DeviceId lfo, eq;
  Doc() {
    store.setDeviceLibrary(&library());
    auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Keys");
    REQUIRE(t.ok());
    REQUIRE(store.dispatch(t->command).ok());
    keys = t->tracks.at(0);
    REQUIRE(stand_in::useBasicSynth(store, keys).valid());  // (GU.10: the IG.1-2 layout)
    // IG.1-2: the instrument out of its instrument group, alone on the path (a lone instrument: the same sound).
    auto ug = ops::ungroupInCircuit(store.current(), store.ids(), library(), findTrackPtr(*store.current(), keys)->devices.at(1)->id);
    REQUIRE_MESSAGE(ug.ok(), (ug.ok() ? std::string() : ug.error().message));
    REQUIRE(store.dispatch(ug->command).ok());
    lfo = add("lfo_device");
    eq = add("eq_eight");
  }
  DeviceId add(const char* patch) {
    auto e = ops::addDevice(*store.current(), store.ids(), keys, patch, DeviceKind::AudioEffect);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return e->devices.at(0);
  }
  WireId run(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return e->wires.empty() ? WireId{} : e->wires.at(0);
  }
  const Project& doc() const { return *store.current(); }
};

LoweredProject lowerDoc(const Project& p) {
  LowerOptions o;
  o.library = &library().library();
  o.devices = &library();
  o.slotOf = [](TrackId) { return 0u; };
  auto r = lower(p, o);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return std::move(*r);
}

const ModulationTap* tapOf(const LoweredProject& l, std::string_view param) {
  for (const ModulationTap& t : l.taps)
    if (t.param == param) return &t;
  return nullptr;
}

using Domain = ModulationTap::Domain;

}  // namespace

TEST_CASE("control curves: the declared curves, the presets each can take, every preset round-trips") {
  const dsp::Curve cutoff = declaredCurve(declOf("eq_eight", "freq1"));  // 10 Hz … 22 kHz @curve(exp)
  const dsp::Curve gain = declaredCurve(declOf("eq_eight", "gain1"));    // −15 … +15 dB (gains)
  CHECK(cutoff.preset == dsp::CurvePreset::Octave);
  CHECK(cutoff.unit == dsp::CurveUnit::Hz);
  CHECK(gain.preset == dsp::CurvePreset::Linear);
  CHECK(gain.output == dsp::CurveOutput::DbToGain);
  CHECK(gain.min == doctest::Approx(-15.0).epsilon(1e-12));
  CHECK_FALSE(addsInUnits(cutoff));  // curved: an Add wire adds in position
  CHECK(addsInUnits(gain));          // linear in dB: an Add wire adds dB
  const std::vector<dsp::CurvePreset> cp = curvePresets(cutoff), gp = curvePresets(gain);
  CHECK(cp.size() == 4);
  CHECK(std::find(gp.begin(), gp.end(), dsp::CurvePreset::Octave) == gp.end());  // equal ratios of dB mean nothing
  CHECK(std::find(gp.begin(), gp.end(), dsp::CurvePreset::Exponential) != gp.end());
  CHECK(curveWithPreset(cutoff, dsp::CurvePreset::Octave) == cutoff);  // the declared preset is the declared curve
  CHECK(addsInUnits(curveWithPreset(cutoff, dsp::CurvePreset::Linear)));
  CHECK_FALSE(addsInUnits(curveWithPreset(gain, dsp::CurvePreset::Logarithmic)));
  // Every preset maps [0, 1] onto the whole range monotonically, and back (gains in their DSP units).
  for (const dsp::Curve& d : {cutoff, gain})
    for (const dsp::CurvePreset c : curvePresets(d)) {
      const dsp::Curve t = curveWithPreset(d, c);
      CHECK(dsp::evalCurveUnits(t, 0.0) == doctest::Approx(d.min));
      CHECK(dsp::evalCurveUnits(t, 1.0) == doctest::Approx(d.max));
      double prev = -1e300;
      for (int i = 0; i <= 20; ++i) {
        const double p = i / 20.0, v = dsp::evalCurve(t, p);
        CHECK(v >= prev);
        prev = v;
        CHECK(dsp::curvePosition(t, v, 0.5) == doctest::Approx(p).epsilon(1e-9));
      }
    }
  // Exponential is fine at the low end (half the travel reaches less than the middle), Logarithmic at the high end.
  const double mid = 0.5 * (cutoff.min + cutoff.max);
  CHECK(dsp::evalCurve(curveWithPreset(cutoff, dsp::CurvePreset::Exponential), 0.5) < mid);
  CHECK(dsp::evalCurve(curveWithPreset(cutoff, dsp::CurvePreset::Logarithmic), 0.5) > mid);
  CHECK(dsp::evalCurve(curveWithPreset(cutoff, dsp::CurvePreset::Linear), 0.5) == doctest::Approx(mid));
  // A dB gain bent in dB: the middle of the travel is below 0 dB on Exponential.
  CHECK(dsp::evalCurve(curveWithPreset(gain, dsp::CurvePreset::Exponential), 0.5) < 1.0);
  // A stepped param has no curve to choose (and steps by 1).
  const dsp::Curve chan = declaredCurve(declOf("utility", "chan"));
  CHECK(chan.step == 1.0);
  CHECK(curvePresets(chan).empty());
  // A bool is a stepped 0 … 1.
  const dsp::Curve mono = declaredCurve(*findAnyParamDecl(*library().library().find("utility"), "mono"));
  CHECK(dsp::evalCurve(mono, 1.0) == 1.0);
  CHECK(dsp::evalCurve(mono, 0.0) == 0.0);
}

TEST_CASE("control curves: SetParamCurve keeps the knob position (the sound follows), one undo step, racks refuse") {
  Doc d;
  REQUIRE(d.run(ops::setDeviceParamValue(d.doc(), library(), d.eq, "freq1", 440.0)) == WireId{});
  const double pos = findDevice(d.doc(), d.eq).device->param("freq1")->value;
  CHECK(paramValue(*findDevice(d.doc(), d.eq).device, "freq1", declOf("eq_eight", "freq1")) == doctest::Approx(440.0).epsilon(1e-12));
  const auto before = d.store.current();
  auto e = ops::setParamCurvePreset(d.doc(), library(), d.eq, "freq1", dsp::CurvePreset::Linear);
  REQUIRE(e.ok());
  REQUIRE(d.store.dispatch(e->command).ok());
  const Device* eq = findDevice(d.doc(), d.eq).device;
  REQUIRE(eq != nullptr);
  REQUIRE(eq->curve("freq1") != nullptr);
  CHECK(eq->curve("freq1")->preset == dsp::CurvePreset::Linear);
  CHECK(eq->curve("freq2") == nullptr);
  CHECK(eq->param("freq1")->value == pos);  // the knob stays …
  const double now = paramValue(*eq, "freq1", declOf("eq_eight", "freq1"));
  CHECK(now == doctest::Approx(10.0 + pos * (22000.0 - 10.0)).epsilon(1e-12));  // … and the value follows the curve
  CHECK(d.store.history().undoLabel() == "Knob Curve");
  // Octave → Linear changes the class (an Exp segment → a straight one): Structural; Linear → Exponential back to Exp.
  REQUIRE(std::holds_alternative<Batch>(e->command.v));
  const Batch& b = std::get<Batch>(e->command.v);
  REQUIRE(b.commands.size() == 1);
  CHECK(std::get<SetParamCurve>(b.commands[0].v).structural);
  auto same = ops::setParamCurvePreset(d.doc(), library(), d.eq, "freq1", dsp::CurvePreset::Linear);
  REQUIRE(same.ok());
  CHECK(same->empty());
  // Octave → Exponential keeps the class (one Exp segment): a Param change, no compile.
  Doc d2;
  auto ex = ops::setParamCurvePreset(d2.doc(), library(), d2.eq, "freq1", dsp::CurvePreset::Exponential);
  REQUIRE(ex.ok());
  CHECK_FALSE(std::get<SetParamCurve>(std::get<Batch>(ex->command.v).commands.at(0).v).structural);
  bool paramOnly = false;
  const pq::doc::ListenerId sub = d2.store.subscribe([&](const StoreEvent& ev) { paramOnly = ev.changes.paramOnly(); });
  REQUIRE(d2.store.dispatch(ex->command).ok());
  d2.store.unsubscribe(sub);
  CHECK(paramOnly);
  // Another curve replaces it; the declared one removes the entry (sorted by param).
  REQUIRE(d.run(ops::setParamCurvePreset(d.doc(), library(), d.eq, "freq1", dsp::CurvePreset::Logarithmic)) == WireId{});
  REQUIRE(d.run(ops::setParamCurvePreset(d.doc(), library(), d.eq, "gain1", dsp::CurvePreset::Exponential)) == WireId{});
  eq = findDevice(d.doc(), d.eq).device;
  REQUIRE(eq->curves.size() == 2);
  CHECK(eq->curves[0].param == "freq1");
  REQUIRE(d.run(ops::setParamCurvePreset(d.doc(), library(), d.eq, "gain1", dsp::CurvePreset::Linear)) == WireId{});
  CHECK(findDevice(d.doc(), d.eq).device->curves.size() == 1);
  REQUIRE(d.store.undo());
  REQUIRE(d.store.undo());
  REQUIRE(d.store.undo());
  CHECK(findDevice(d.doc(), d.eq).device->curve("freq1")->preset == dsp::CurvePreset::Linear);
  REQUIRE(d.store.undo());
  CHECK(d.store.current() == before);
  REQUIRE(d.store.redo());
  CHECK(findDevice(d.doc(), d.eq).device->curve("freq1")->preset == dsp::CurvePreset::Linear);
  // An invalid curve and an unknown param are refused.
  dsp::Curve bad = curveWithPreset(declaredCurve(declOf("eq_eight", "freq1")), dsp::CurvePreset::Linear);
  bad.points[1].t = 0.5;
  CHECK_FALSE(ops::setParamCurve(d.doc(), library(), d.eq, "freq1", bad).ok());
  CHECK_FALSE(ops::setParamCurve(d.doc(), library(), d.eq, "nope", std::nullopt).ok());
  // A knob position outside [0, 1] is refused by the store (a value in units: ops::setDeviceParamValue).
  CHECK_FALSE(d.store.dispatch(SetDeviceParam{d.eq, "freq1", 440.0}).ok());
}

TEST_CASE("control curves: the file — curve objects under \"curves\", read back equal, bad entries refused") {
  Doc d;
  d.run(ops::setParamCurvePreset(d.doc(), library(), d.eq, "freq1", dsp::CurvePreset::Linear));
  d.run(ops::setParamCurvePreset(d.doc(), library(), d.eq, "gain2", dsp::CurvePreset::Logarithmic));
  Result<std::string> text = serializeProject(d.doc());
  REQUIRE(text.ok());
  CHECK(text->find("\"freq1\": {\"min\": 10.0, \"max\": 22000.0, \"unit\": \"hz\", \"preset\": \"linear\"") != std::string::npos);
  CHECK(text->find("\"output\": \"dbToGain\", \"preset\": \"logarithmic\"") != std::string::npos);
  Result<LoadedProject> back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == d.doc());
  // Broken curves: a point out of order, a shape that is not one, a missing range.
  for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
           {"[1.0, 1.0]]", "[0.0, 1.0]]"}, {"[\"linear\", 0.0]", "[\"steep\", 0.0]"}, {"\"min\": 10.0, ", ""}}) {
    std::string t = *text;
    const size_t at = t.find(from);
    REQUIRE(at != std::string::npos);
    t.replace(at, from.size(), to);
    CHECK_FALSE(parseProject(t).ok());
  }
}

TEST_CASE("control curves: an Add wire adds in position on a curved control, in units on a linear one, in dB on a gain") {
  Doc d;
  d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "lfo", d.eq, "freq1"));
  d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "lfo", d.eq, "gain1"));
  {
    const LoweredProject l = lowerDoc(d.doc());
    REQUIRE(tapOf(l, "freq1") != nullptr);
    REQUIRE(tapOf(l, "gain1") != nullptr);
    CHECK(tapOf(l, "freq1")->domain == Domain::Position);  // octave: in position
    CHECK(tapOf(l, "gain1")->domain == Domain::Curve);     // linear in dB: in dB
    CHECK(l.curves.size() == 2);                            // one curve atom each
  }
  d.run(ops::setParamCurvePreset(d.doc(), library(), d.eq, "freq1", dsp::CurvePreset::Linear));
  d.run(ops::setParamCurvePreset(d.doc(), library(), d.eq, "gain1", dsp::CurvePreset::Exponential));
  {
    const LoweredProject l = lowerDoc(d.doc());
    CHECK(tapOf(l, "freq1")->domain == Domain::Curve);     // linear: in Hz
    CHECK(tapOf(l, "gain1")->domain == Domain::Position);  // a bent gain: in position
  }
}

TEST_CASE("control curves: 15 -> 16 keeps TX.patching's added wires as Units (same graph), chain links stay added") {
  Doc d;
  const DeviceId util = d.add("utility");
  d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "lfo", d.eq, "freq1"));  // curved: the laws differ
  d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "lfo", d.eq, "gain3"));  // a dB gain: the laws differ
  d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "lfo", util, "balance"));  // linear: the same
  d.run(ops::connectTravelWire(d.doc(), d.store.ids(), d.lfo, "lfo", d.eq, "gain2", 0.4));
  const DeviceId series[1] = {util};
  d.run(ops::groupInCircuit(d.store.current(), d.store.ids(), library(), series));
  Result<std::string> text = serializeProject(d.doc());
  REQUIRE(text.ok());
  nlohmann::json j = nlohmann::json::parse(*text);
  j["schemaVersion"] = 15;
  LoadOptions lo;
  lo.devices = &library();
  Result<LoadedProject> old = parseProject(j.dump(2), lo);
  REQUIRE_MESSAGE(old.ok(), (old.ok() ? std::string() : old.error().message));
  CHECK(old->fileSchemaVersion == 15);
  int units = 0, links = 0;
  for (const Wire& w : wiresOf(*old->project)) {
    if (w.mainLink()) continue;  // TX.all-free: the main path's links (this text is schema 20 relabelled 15)
    if (w.chain) {
      CHECK(w.law == WireLaw::Add);
      ++links;
    } else if (w.param == "gain2") {
      CHECK(w.law == WireLaw::Travel);
    } else {
      CHECK(w.law == WireLaw::Units);
      ++units;
    }
  }
  CHECK(units == 3);
  CHECK(links == 4);  // (IG.1-2: and the 18 -> 19 step's two, In → the synth → Out of its instrument group)
  Result<std::string> again = serializeProject(*old->project);
  REQUIRE(again.ok());
  CHECK(again->find("\"law\": \"units\"") != std::string::npos);
  Result<LoadedProject> back = parseProject(*again);
  REQUIRE(back.ok());
  CHECK(*back->project == *old->project);
  // Into a linear param a Units wire and an Add wire differ only by where the conversion sits (none here): the same IR
  // apart from the tap's domain.
  Doc c, e;
  c.run(ops::connectWire(c.doc(), c.store.ids(), c.lfo, "lfo", c.eq, "freq1"));
  Wire x = *findWire(e.doc(), e.run(ops::connectWire(e.doc(), e.store.ids(), e.lfo, "lfo", e.eq, "freq1")));
  REQUIRE(e.store.dispatch(RemoveWire{x.id}).ok());
  x.law = WireLaw::Units;
  REQUIRE(e.store.dispatch(AddWire{x}).ok());
  const LoweredProject lc = lowerDoc(c.doc()), le = lowerDoc(e.doc());
  bool differ = false;
  for (size_t i = 0; i < lc.patches.size(); ++i) differ = differ || lc.patches[i]->hash != le.patches[i]->hash;
  CHECK(differ);  // into a curved cutoff: in position vs in Hz after the curve
  CHECK(tapOf(le, "freq1")->domain == Domain::Dsp);
}

TEST_CASE("control curves: 19 -> 20 turns values and named curves into knob positions and curve objects (library)") {
  Doc d;
  d.run(ops::setDeviceParamValue(d.doc(), library(), d.eq, "freq1", 1000.0));
  d.run(ops::setDeviceParamValue(d.doc(), library(), d.eq, "gain2", std::pow(10.0, 6.0 / 20.0)));
  Result<std::string> text = serializeProject(d.doc());
  REQUIRE(text.ok());
  // The same project as schema 19 wrote it: values in units, a knob on a named curve, a lane in units.
  nlohmann::json j = nlohmann::json::parse(*text);
  j["schemaVersion"] = 19;
  nlohmann::json* eq = nullptr;
  for (nlohmann::json& x : j["tracks"][0]["devices"])
    if (x["id"] == d.eq.value) eq = &x;
  REQUIRE(eq != nullptr);
  (*eq)["params"]["freq1"] = 1000.0;
  (*eq)["params"]["gain2"] = std::pow(10.0, 6.0 / 20.0);
  (*eq)["params"]["freq2"] = 2000.0;
  (*eq)["curves"] = nlohmann::json::object({{"freq2", "linear"}});
  j["tracks"][0]["lanes"] = nlohmann::json::array({nlohmann::json{
      {"id", 9000},
      {"param", {{"kind", "deviceParam"}, {"owner", d.eq.value}, {"name", "freq1"}}},
      {"points", nlohmann::json::array({nlohmann::json{{"id", 9001}, {"tick", 0}, {"value", 100.0}, {"curve", 0.0}},
                                        nlohmann::json{{"id", 9002}, {"tick", 1000}, {"value", 10000.0}, {"curve", 0.0}}})}}});
  const std::string old = j.dump(2);
  LoadOptions lo;
  lo.devices = &library();
  Result<LoadedProject> r = parseProject(old, lo);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  const Device* e = findDevice(*r->project, d.eq).device;
  REQUIRE(e != nullptr);
  const dsp::Curve oct = declaredCurve(declOf("eq_eight", "freq1"));
  CHECK(e->param("freq1")->value == doctest::Approx(dsp::curvePosition(oct, 1000.0, 0.0)).epsilon(1e-12));
  CHECK(paramValue(*e, "freq1", declOf("eq_eight", "freq1")) == doctest::Approx(1000.0).epsilon(1e-12));
  CHECK(paramValue(*e, "gain2", declOf("eq_eight", "gain2")) == doctest::Approx(std::pow(10.0, 6.0 / 20.0)).epsilon(1e-12));
  REQUIRE(e->curve("freq2") != nullptr);  // the named curve became a curve object; its knob kept the value
  CHECK(e->curve("freq2")->preset == dsp::CurvePreset::Linear);
  CHECK(paramValue(*e, "freq2", declOf("eq_eight", "freq2")) == doctest::Approx(2000.0).epsilon(1e-12));
  const Track* t = findTrackPtr(*r->project, d.keys);
  REQUIRE(t != nullptr);
  REQUIRE(t->lanes.size() == 1);
  CHECK(t->lanes[0]->points[0].value == doctest::Approx(static_cast<float>(dsp::curvePosition(oct, 100.0, 0.0))));
  CHECK(t->lanes[0]->points[1].value == doctest::Approx(static_cast<float>(dsp::curvePosition(oct, 10000.0, 0.0))));
  // Without the library such a file is refused, saying why; one with nothing to convert loads.
  Result<LoadedProject> none = parseProject(old);
  REQUIRE_FALSE(none.ok());
  CHECK(none.error().message.find("device library") != std::string::npos);
  Doc fresh;
  Result<std::string> plain = serializeProject(fresh.doc());
  REQUIRE(plain.ok());
  nlohmann::json k = nlohmann::json::parse(*plain);
  k["schemaVersion"] = 19;
  CHECK(parseProject(k.dump(2)).ok());
}

// TMM.13-fix1 L7: group invariants the reader and the store did not enforce (the review's fuzz cases 328, 357, 366,
// 385, 442 – 444): knob curves on a group, chain links between devices of no one group or across tracks.
TEST_CASE("groups: a group has no knob curves; chain links join one in-series group's terminals and members") {
  Doc d;
  const DeviceId util = d.add("utility");
  const DeviceId series[1] = {util};
  auto g = ops::groupInCircuit(d.store.current(), d.store.ids(), library(), series);
  REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
  REQUIRE(d.store.dispatch(g->command).ok());
  DeviceId group;
  for (const DevicePtr& x : findTrackPtr(d.doc(), d.keys)->devices)
    if (isGroupDevice(*x)) group = x->id;
  REQUIRE(group.valid());
  // Curves: refused by the store (there is no knob) and by the reader.
  CHECK_FALSE(d.store.dispatch(SetParamCurve{group, "gain", dsp::curvePreset(dsp::CurvePreset::Linear, 0, 1), true}).ok());
  // A chain link the user never draws, between devices of no group: refused.
  Wire stray;
  stray.id = d.store.ids().wire();
  stray.from = d.lfo;
  stray.port = "lfo";
  stray.to = util;
  stray.param = "width";
  stray.chain = true;
  CHECK_FALSE(d.store.dispatch(AddWire{stray}).ok());
  stray.chain = false;  // as an ordinary wire it is fine
  CHECK(d.store.dispatch(AddWire{stray}).ok());
  // In the file.
  Result<std::string> text = serializeProject(d.doc());
  REQUIRE(text.ok());
  REQUIRE(parseProject(*text).ok());
  nlohmann::json j = nlohmann::json::parse(*text);
  {
    nlohmann::json k = j;
    for (nlohmann::json& w : k["wires"])
      if (w["id"] == stray.id.value) w["chain"] = true;
    const Result<LoadedProject> r = parseProject(k.dump(2));
    REQUIRE_FALSE(r.ok());
    MESSAGE(r.error().message);
    CHECK(r.error().message.find(".chain") != std::string::npos);
  }
  {  // knob curves on the group
    nlohmann::json k = j;
    for (nlohmann::json& x : k["tracks"][0]["devices"])
      if (x["id"] == group.value)
        x["curves"] = nlohmann::json::parse(
            R"({"gain": {"min": 0.0, "max": 1.0, "preset": "linear", "points": [[0.0, 0.0], [1.0, 1.0]], "shapes": [["linear", 0.0]]}})");
    const Result<LoadedProject> r = parseProject(k.dump(2));
    REQUIRE_FALSE(r.ok());
    MESSAGE(r.error().message);
  }
}
