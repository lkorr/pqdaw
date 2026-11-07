// GU.1 (docs/plans/GU.md "Curves"; SPEC §15 2026-11-06 "built GUIs"): the document side of control curves — the curve a
// declaration gives (graph::declaredCurve; the knob curve menus' presets over it), a device's own curves (model.h
// ControlCurve: SetParamCurve keeps the knob position — the sound follows —, one undo step, Structural only when the
// class changes, the file, the checks), the wire laws a curve decides (an Add wire adds in position on a curved control,
// in units on a linear one: dB on a dB gain), the 15 → 16 migration (TX.patching's added wires keep their law as Units)
// and 19 → 20 (values and TX.knob-curves' named curves become knob positions and curve objects, with the library).
// GU.11: the controls are a native engine's (the Envelope: attack / decay curved, Exp0 in seconds; sustain / amount
// linear); the declared-curve cases read declarations of a small patch of their own (the retired EQ Eight's and
// Utility's params).
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/modules.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/graph/parse.h"
#include "../support/blocks.h"

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

// Declarations of the kinds the declared-curve cases need: an exponential cutoff in Hz, a dB gain, a stepped param, a
// bool (the retired EQ Eight's and Utility's).
const pq::graph::ParamDecl& declOf(const char* param) {
  static const pq::graph::PatchFile file = [] {
    auto f = pq::graph::parse("patch decls {\n"
                              "  param freq1 = 30Hz [10Hz..22kHz] @unit(Hz) @curve(exp) @label(\"1 Freq\")\n"
                              "  param gain1 = 0dB [-15dB..15dB] @unit(dB) @label(\"1 Gain\")\n"
                              "  param chan = 0 [0..3] @steps(4) @label(\"Channel Mode\")\n"
                              "  param mono = false @label(\"Mono\")\n"
                              "}\n",
                              "decls.pqp");
    REQUIRE_MESSAGE(f.ok(), (f.ok() ? std::string() : f.error().message));
    return std::move(*f);
  }();
  const pq::graph::ParamDecl* d = findAnyParamDecl(file.patches.at(0), param);
  REQUIRE(d != nullptr);
  return *d;
}

// The Envelope engine's declaration of `param` (what the library plays for a "meta:envelope" block).
const pq::graph::ParamDecl& envDecl(const char* param) {
  const DevicePtr env = makeDevice(DeviceId{1}, "meta:envelope", DeviceKind::AudioEffect);
  const pq::graph::Patch* p = library().patchOf(*env);
  REQUIRE(p != nullptr);
  const pq::graph::ParamDecl* d = findParamDecl(*p, param);
  REQUIRE(d != nullptr);
  return *d;
}

// A MIDI track: its instrument a Wavetable engine in an instrument group (inst; test_blocks::addEngineInstrument: no
// knob curves, so the project may be relabelled as an older schema), an oscillator off the path as the modulation
// source (osc, a Wavetable engine: its `out`) and an Envelope engine after the instrument (env: the curves under test).
struct Doc {
  DocumentStore store;
  TrackId keys;
  DeviceId inst, osc, env;
  Doc() {
    store.setDeviceLibrary(&library());
    auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Keys");
    REQUIRE(t.ok());
    REQUIRE(store.dispatch(t->command).ok());
    keys = t->tracks.at(0);
    inst = test_blocks::addEngineInstrument(store, keys, "wavetable", library());
    auto o = ops::addMetaDevice(*store.current(), store.ids(), keys, "wavetable", ops::kOffPath);
    REQUIRE(o.ok());
    REQUIRE(store.dispatch(o->command).ok());
    osc = o->devices.at(0);
    env = test_blocks::appendEngineAfter(store, keys, "envelope");
  }
  WireId run(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return e->wires.empty() ? WireId{} : e->wires.at(0);
  }
  // The oscillator's `out` into `param` of the Envelope (an Add wire).
  WireId lfoInto(const char* param) { return run(ops::connectWire(doc(), store.ids(), osc, "out", env, param)); }
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

const ModulationTap* tapOf(const LoweredProject& l, DeviceId device, std::string_view param) {
  for (const ModulationTap& t : l.taps)
    if (t.device == device && t.param == param) return &t;
  return nullptr;
}

using Domain = ModulationTap::Domain;

}  // namespace

TEST_CASE("control curves: the declared curves, the presets each can take, every preset round-trips") {
  const dsp::Curve cutoff = declaredCurve(declOf("freq1"));  // 10 Hz … 22 kHz @curve(exp)
  const dsp::Curve gain = declaredCurve(declOf("gain1"));    // −15 … +15 dB (gains)
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
  const dsp::Curve chan = declaredCurve(declOf("chan"));
  CHECK(chan.step == 1.0);
  CHECK(curvePresets(chan).empty());
  // A bool is a stepped 0 … 1.
  const dsp::Curve mono = declaredCurve(declOf("mono"));
  CHECK(dsp::evalCurve(mono, 1.0) == 1.0);
  CHECK(dsp::evalCurve(mono, 0.0) == 0.0);
  // A native engine's controls: the Envelope's attack is curved (Exp0, in seconds), its sustain linear.
  CHECK(declaredCurve(envDecl("attack")).preset == dsp::CurvePreset::Exp0);
  CHECK_FALSE(addsInUnits(declaredCurve(envDecl("attack"))));
  CHECK(addsInUnits(declaredCurve(envDecl("sustain"))));
}

TEST_CASE("control curves: SetParamCurve keeps the knob position (the sound follows), one undo step, groups refuse") {
  Doc d;
  const dsp::Curve attack = declaredCurve(envDecl("attack"));
  REQUIRE(d.run(ops::setDeviceParamValue(d.doc(), library(), d.env, "attack", 0.1)) == WireId{});
  const double pos = findDevice(d.doc(), d.env).device->param("attack")->value;
  CHECK(paramValue(*findDevice(d.doc(), d.env).device, "attack", envDecl("attack")) == doctest::Approx(0.1).epsilon(1e-12));
  const auto before = d.store.current();
  auto e = ops::setParamCurvePreset(d.doc(), library(), d.env, "attack", dsp::CurvePreset::Linear);
  REQUIRE(e.ok());
  REQUIRE(d.store.dispatch(e->command).ok());
  const Device* env = findDevice(d.doc(), d.env).device;
  REQUIRE(env != nullptr);
  REQUIRE(env->curve("attack") != nullptr);
  CHECK(env->curve("attack")->preset == dsp::CurvePreset::Linear);
  CHECK(env->curve("decay") == nullptr);
  CHECK(env->param("attack")->value == pos);  // the knob stays …
  const double now = paramValue(*env, "attack", envDecl("attack"));
  CHECK(now == doctest::Approx(attack.min + pos * (attack.max - attack.min)).epsilon(1e-12));  // … and the value follows
  CHECK(d.store.history().undoLabel() == "Knob Curve");
  // Exp0 → Linear changes the class (an Exp segment → a straight one): Structural.
  REQUIRE(std::holds_alternative<Batch>(e->command.v));
  const Batch& b = std::get<Batch>(e->command.v);
  REQUIRE(b.commands.size() == 1);
  CHECK(std::get<SetParamCurve>(b.commands[0].v).structural);
  auto same = ops::setParamCurvePreset(d.doc(), library(), d.env, "attack", dsp::CurvePreset::Linear);
  REQUIRE(same.ok());
  CHECK(same->empty());
  // Linear → Linear on a linear control with another range keeps the class: a Param change, no compile.
  Doc d2;
  dsp::Curve wider = declaredCurve(envDecl("sustain"));
  wider.max = 0.5;
  REQUIRE(dsp::validCurve(wider));
  auto ex = ops::setParamCurve(d2.doc(), library(), d2.env, "sustain", wider);
  REQUIRE(ex.ok());
  CHECK_FALSE(std::get<SetParamCurve>(std::get<Batch>(ex->command.v).commands.at(0).v).structural);
  bool paramOnly = false;
  const pq::doc::ListenerId sub = d2.store.subscribe([&](const StoreEvent& ev) { paramOnly = ev.changes.paramOnly(); });
  REQUIRE(d2.store.dispatch(ex->command).ok());
  d2.store.unsubscribe(sub);
  CHECK(paramOnly);
  // Another curve replaces it; the declared one removes the entry (sorted by param).
  REQUIRE(d.run(ops::setParamCurvePreset(d.doc(), library(), d.env, "attack", dsp::CurvePreset::Logarithmic)) == WireId{});
  REQUIRE(d.run(ops::setParamCurvePreset(d.doc(), library(), d.env, "sustain", dsp::CurvePreset::Exponential)) == WireId{});
  env = findDevice(d.doc(), d.env).device;
  REQUIRE(env->curves.size() == 2);
  CHECK(env->curves[0].param == "attack");
  REQUIRE(d.run(ops::setParamCurvePreset(d.doc(), library(), d.env, "sustain", dsp::CurvePreset::Linear)) == WireId{});
  CHECK(findDevice(d.doc(), d.env).device->curves.size() == 1);
  REQUIRE(d.store.undo());
  REQUIRE(d.store.undo());
  REQUIRE(d.store.undo());
  CHECK(findDevice(d.doc(), d.env).device->curve("attack")->preset == dsp::CurvePreset::Linear);
  REQUIRE(d.store.undo());
  CHECK(d.store.current() == before);
  REQUIRE(d.store.redo());
  CHECK(findDevice(d.doc(), d.env).device->curve("attack")->preset == dsp::CurvePreset::Linear);
  // An invalid curve and an unknown param are refused; a group has no knobs.
  dsp::Curve bad = curveWithPreset(attack, dsp::CurvePreset::Linear);
  bad.points[1].t = 0.5;
  CHECK_FALSE(ops::setParamCurve(d.doc(), library(), d.env, "attack", bad).ok());
  CHECK_FALSE(ops::setParamCurve(d.doc(), library(), d.env, "nope", std::nullopt).ok());
  CHECK_FALSE(ops::setParamCurve(d.doc(), library(), d.inst, "voices", std::nullopt).ok());
  // A knob position outside [0, 1] is refused by the store (a value in units: ops::setDeviceParamValue).
  CHECK_FALSE(d.store.dispatch(SetDeviceParam{d.env, "attack", 4.0}).ok());
}

TEST_CASE("control curves: the file — curve objects under \"curves\", read back equal, bad entries refused") {
  Doc d;
  d.run(ops::setParamCurvePreset(d.doc(), library(), d.env, "attack", dsp::CurvePreset::Linear));
  d.run(ops::setParamCurvePreset(d.doc(), library(), d.env, "sustain", dsp::CurvePreset::Logarithmic));
  Result<std::string> text = serializeProject(d.doc());
  REQUIRE(text.ok());
  CHECK(text->find("\"attack\": {\"min\": 0.0, \"max\": ") != std::string::npos);
  CHECK(text->find("\"preset\": \"linear\"") != std::string::npos);
  CHECK(text->find("\"sustain\": {\"min\": 0.0, \"max\": 1.0, \"preset\": \"logarithmic\"") != std::string::npos);
  Result<LoadedProject> back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == d.doc());
  // Broken curves: a point out of order, a shape that is not one, a missing range.
  const std::string at0 = "\"sustain\": {\"min\": 0.0, ";
  for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
           {"[1.0, 1.0]]", "[0.0, 1.0]]"}, {"[\"linear\", 0.0]", "[\"steep\", 0.0]"}, {at0, "\"sustain\": {"}}) {
    std::string t = *text;
    const size_t at = t.find(from);
    REQUIRE(at != std::string::npos);
    t.replace(at, from.size(), to);
    CHECK_FALSE(parseProject(t).ok());
  }
}

TEST_CASE("control curves: an Add wire adds in position on a curved control, in units on a linear one") {
  Doc d;
  d.lfoInto("attack");
  d.lfoInto("sustain");
  {
    const LoweredProject l = lowerDoc(d.doc());
    REQUIRE(tapOf(l, d.env, "attack") != nullptr);
    REQUIRE(tapOf(l, d.env, "sustain") != nullptr);
    CHECK(tapOf(l, d.env, "attack")->domain == Domain::Position);  // Exp0: in position
    CHECK(tapOf(l, d.env, "sustain")->domain == Domain::Curve);    // linear: in units
  }
  d.run(ops::setParamCurvePreset(d.doc(), library(), d.env, "attack", dsp::CurvePreset::Linear));
  d.run(ops::setParamCurvePreset(d.doc(), library(), d.env, "sustain", dsp::CurvePreset::Exponential));
  {
    const LoweredProject l = lowerDoc(d.doc());
    CHECK(tapOf(l, d.env, "attack")->domain == Domain::Curve);      // linear: in seconds
    CHECK(tapOf(l, d.env, "sustain")->domain == Domain::Position);  // bent: in position
  }
}

TEST_CASE("control curves: 15 -> 16 keeps TX.patching's added wires as Units (same graph), chain links stay added") {
  Doc d;
  d.lfoInto("attack");  // curved: the laws differ
  d.lfoInto("decay");   // curved
  d.lfoInto("amount");  // linear: the same
  d.run(ops::connectTravelWire(d.doc(), d.store.ids(), d.osc, "out", d.env, "sustain", 0.4));
  const DeviceId series[1] = {d.env};
  d.run(ops::groupInCircuit(d.store.current(), d.store.ids(), library(), series));
  size_t chainLinks = 0;
  for (const Wire& w : wiresOf(d.doc())) chainLinks += w.chain ? 1u : 0u;
  REQUIRE(chainLinks >= 2);  // (the new group's In → the engine → Out, and the instrument group's inside)
  Result<std::string> text = serializeProject(d.doc());
  REQUIRE(text.ok());
  nlohmann::json j = nlohmann::json::parse(*text);
  j["schemaVersion"] = 15;
  LoadOptions lo;
  lo.devices = &library();
  Result<LoadedProject> old = parseProject(j.dump(2), lo);
  REQUIRE_MESSAGE(old.ok(), (old.ok() ? std::string() : old.error().message));
  CHECK(old->fileSchemaVersion == 15);
  size_t units = 0, links = 0, travel = 0;
  for (const Wire& w : wiresOf(*old->project)) {
    if (w.mainLink()) continue;  // TX.all-free: the main path's links (this text is schema 20 relabelled 15)
    if (w.chain) {
      CHECK(w.law == WireLaw::Add);
      ++links;
    } else if (w.to == d.env && w.param == "sustain") {
      CHECK(w.law == WireLaw::Travel);
      ++travel;
    } else if (w.from == d.osc) {
      CHECK(w.law == WireLaw::Units);
      ++units;
    }
  }
  CHECK(units == 3);
  CHECK(travel == 1);
  CHECK(links == chainLinks);
  Result<std::string> again = serializeProject(*old->project);
  REQUIRE(again.ok());
  CHECK(again->find("\"law\": \"units\"") != std::string::npos);
  Result<LoadedProject> back = parseProject(*again);
  REQUIRE(back.ok());
  CHECK(*back->project == *old->project);
  // Into a curved control a Units wire and an Add wire differ by where the conversion sits: in position vs in seconds
  // after the curve.
  Doc c, e;
  c.lfoInto("attack");
  Wire x = *findWire(e.doc(), e.lfoInto("attack"));
  REQUIRE(e.store.dispatch(RemoveWire{x.id}).ok());
  x.law = WireLaw::Units;
  REQUIRE(e.store.dispatch(AddWire{x}).ok());
  const LoweredProject lc = lowerDoc(c.doc()), le = lowerDoc(e.doc());
  bool differ = false;
  for (size_t i = 0; i < lc.patches.size(); ++i) differ = differ || lc.patches[i]->hash != le.patches[i]->hash;
  CHECK(differ);
  CHECK(tapOf(le, e.env, "attack")->domain == Domain::Dsp);
}

TEST_CASE("control curves: 19 -> 20 turns values and named curves into knob positions and curve objects (library)") {
  Doc d;
  d.run(ops::setDeviceParamValue(d.doc(), library(), d.env, "attack", 0.1));
  d.run(ops::setDeviceParamValue(d.doc(), library(), d.env, "sustain", 0.3));
  Result<std::string> text = serializeProject(d.doc());
  REQUIRE(text.ok());
  // The same project as schema 19 wrote it: values in units, a knob on a named curve, a lane in units.
  nlohmann::json j = nlohmann::json::parse(*text);
  j["schemaVersion"] = 19;
  nlohmann::json* env = nullptr;
  for (nlohmann::json& x : j["tracks"][0]["devices"])
    if (x["id"] == d.env.value) env = &x;
  REQUIRE(env != nullptr);
  (*env)["params"]["attack"] = 0.1;
  (*env)["params"]["sustain"] = 0.3;
  (*env)["params"]["decay"] = 2.0;
  (*env)["curves"] = nlohmann::json::object({{"decay", "linear"}});
  j["tracks"][0]["lanes"] = nlohmann::json::array({nlohmann::json{
      {"id", 9000},
      {"param", {{"kind", "deviceParam"}, {"owner", d.env.value}, {"name", "attack"}}},
      {"points", nlohmann::json::array({nlohmann::json{{"id", 9001}, {"tick", 0}, {"value", 0.01}, {"curve", 0.0}},
                                        nlohmann::json{{"id", 9002}, {"tick", 1000}, {"value", 1.0}, {"curve", 0.0}}})}}});
  const std::string old = j.dump(2);
  LoadOptions lo;
  lo.devices = &library();
  Result<LoadedProject> r = parseProject(old, lo);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  const Device* e = findDevice(*r->project, d.env).device;
  REQUIRE(e != nullptr);
  const dsp::Curve att = declaredCurve(envDecl("attack"));
  CHECK(e->param("attack")->value == doctest::Approx(dsp::curvePosition(att, 0.1, 0.0)).epsilon(1e-12));
  CHECK(paramValue(*e, "attack", envDecl("attack")) == doctest::Approx(0.1).epsilon(1e-12));
  CHECK(paramValue(*e, "sustain", envDecl("sustain")) == doctest::Approx(0.3).epsilon(1e-12));
  REQUIRE(e->curve("decay") != nullptr);  // the named curve became a curve object; its knob kept the value
  CHECK(e->curve("decay")->preset == dsp::CurvePreset::Linear);
  CHECK(paramValue(*e, "decay", envDecl("decay")) == doctest::Approx(2.0).epsilon(1e-12));
  const Track* t = findTrackPtr(*r->project, d.keys);
  REQUIRE(t != nullptr);
  REQUIRE(t->lanes.size() == 1);
  CHECK(t->lanes[0]->points[0].value == doctest::Approx(static_cast<float>(dsp::curvePosition(att, 0.01, 0.0))));
  CHECK(t->lanes[0]->points[1].value == doctest::Approx(static_cast<float>(dsp::curvePosition(att, 1.0, 0.0))));
  // Without the library such a file is refused, saying why; one with nothing to convert loads.
  Result<LoadedProject> none = parseProject(old);
  REQUIRE_FALSE(none.ok());
  CHECK(none.error().message.find("device library") != std::string::npos);
  DocumentStore fresh;
  Result<std::string> plain = serializeProject(*fresh.current());
  REQUIRE(plain.ok());
  nlohmann::json k = nlohmann::json::parse(*plain);
  k["schemaVersion"] = 19;
  CHECK(parseProject(k.dump(2)).ok());
}

// TMM.13-fix1 L7: group invariants the reader and the store did not enforce (the review's fuzz cases 328, 357, 366,
// 385, 442 – 444): knob curves on a group, chain links between devices of no one group or across tracks.
TEST_CASE("groups: a group has no knob curves; chain links join one in-series group's terminals and members") {
  Doc d;
  const DeviceId util = test_blocks::appendAfter(d.store, d.keys, std::string(kValueDevice));
  const DeviceId series[1] = {util};
  auto g = ops::groupInCircuit(d.store.current(), d.store.ids(), library(), series);
  REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
  REQUIRE(d.store.dispatch(g->command).ok());
  const DeviceId group = findDevice(d.doc(), util).device->group;
  REQUIRE(group.valid());
  // Curves: refused by the store (there is no knob) and by the reader.
  CHECK_FALSE(d.store.dispatch(SetParamCurve{group, "gain", dsp::curvePreset(dsp::CurvePreset::Linear, 0, 1), true}).ok());
  // A chain link the user never draws, between devices of no group: refused.
  Wire stray;
  stray.id = d.store.ids().wire();
  stray.from = d.osc;
  stray.port = "out";
  stray.to = util;
  stray.param = std::string(kValueParam);
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
