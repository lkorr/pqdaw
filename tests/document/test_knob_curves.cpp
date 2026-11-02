// TX.knob-curves (SPEC §15 2026-11-01 "answers to Q186" + clarification): the document side — a knob's curve per
// device instance (model.h KnobCurve: SetParamCurve, undo, the file, the checks), the curves' laws (doc::paramTravel
// on a curve: positions and values round-trip, what each param can take), the wire laws they decide (an Add wire into
// a curved knob adds in travel, into a linear one in units, into a dB gain in dB) and the 15 → 16 migration (TX.patching's
// added wires keep their law as Units, so an old project lowers to the same graph).
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

using namespace pq::doc;
using pq::Result;

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
    auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Keys");
    REQUIRE(t.ok());
    REQUIRE(store.dispatch(t->command).ok());
    keys = t->tracks.at(0);
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

}  // namespace

TEST_CASE("knob curves: the laws — what each param can take, positions and values round-trip, Default is the declaration") {
  const ParamTravel cutoff = paramTravel(declOf("eq_eight", "freq1"));  // 10 Hz … 22 kHz @curve(exp)
  const ParamTravel gain = paramTravel(declOf("eq_eight", "gain1"));    // −15 … +15 dB (gains)
  CHECK(declaredCurve(cutoff) == KnobCurve::Octave);
  CHECK(declaredCurve(gain) == KnobCurve::Linear);
  CHECK(gain.db);
  CHECK_FALSE(travelsInUnits(cutoff));  // curved: an Add wire adds in travel
  CHECK(travelsInUnits(gain));          // linear in dB: an Add wire adds dB
  for (const KnobCurve c : {KnobCurve::Linear, KnobCurve::Octave, KnobCurve::Exponential, KnobCurve::Logarithmic})
    CHECK(curveAvailable(cutoff, c));
  CHECK_FALSE(curveAvailable(gain, KnobCurve::Octave));  // equal ratios of a dB value mean nothing
  CHECK(curveAvailable(gain, KnobCurve::Exponential));
  // Default and the declared curve are the declaration itself.
  for (const KnobCurve c : {KnobCurve::Default, KnobCurve::Octave}) {
    const ParamTravel t = paramTravel(declOf("eq_eight", "freq1"), c);
    CHECK(t.log == cutoff.log);
    CHECK(t.exp0 == cutoff.exp0);
  }
  CHECK(travelsInUnits(withCurve(cutoff, KnobCurve::Linear)));
  CHECK_FALSE(travelsInUnits(withCurve(gain, KnobCurve::Logarithmic)));
  // Every curve maps [0, 1] onto the whole range monotonically, and back.
  for (const ParamTravel& d : {cutoff, gain})
    for (const KnobCurve c : {KnobCurve::Linear, KnobCurve::Octave, KnobCurve::Exponential, KnobCurve::Logarithmic}) {
      if (!curveAvailable(d, c)) continue;
      const ParamTravel t = withCurve(d, c);
      CHECK(travelValue(t, 0.0) == doctest::Approx(d.lo));
      CHECK(travelValue(t, 1.0) == doctest::Approx(d.hi));
      double prev = -1e300;
      for (int i = 0; i <= 20; ++i) {
        const double p = i / 20.0, v = travelValue(t, p);
        CHECK(v >= prev);
        prev = v;
        CHECK(travelPosition(t, v) == doctest::Approx(p).epsilon(1e-9));
      }
    }
  // Exponential is fine at the low end (half the travel reaches less than the middle), Logarithmic at the high end.
  const ParamTravel lin = withCurve(cutoff, KnobCurve::Linear);
  const double mid = 0.5 * (cutoff.lo + cutoff.hi);
  CHECK(travelValue(withCurve(cutoff, KnobCurve::Exponential), 0.5) < mid);
  CHECK(travelValue(withCurve(cutoff, KnobCurve::Logarithmic), 0.5) > mid);
  CHECK(travelValue(lin, 0.5) == doctest::Approx(mid));
  // A dB gain bent in dB: the middle of the travel is below 0 dB on Exponential.
  CHECK(travelValue(withCurve(gain, KnobCurve::Exponential), 0.5) < 1.0);
  // A stepped param has no curve to choose.
  ParamTravel stepped = lin;
  stepped.round = true;
  CHECK_FALSE(curveAvailable(stepped, KnobCurve::Exponential));
}

TEST_CASE("knob curves: SetParamCurve keeps the value, is one undo step, Default removes the entry, racks refuse") {
  Doc d;
  REQUIRE(d.store.dispatch(SetDeviceParam{d.eq, "freq1", 440.0}).ok());
  const auto before = d.store.current();
  auto e = ops::setParamCurve(d.doc(), d.eq, "freq1", KnobCurve::Linear);
  REQUIRE(e.ok());
  REQUIRE(d.store.dispatch(e->command).ok());
  const Device* eq = findDevice(d.doc(), d.eq).device;
  REQUIRE(eq != nullptr);
  CHECK(eq->curve("freq1") == KnobCurve::Linear);
  CHECK(eq->curve("freq2") == KnobCurve::Default);
  CHECK(eq->param("freq1")->value == 440.0);  // the value stays: the knob re-maps
  CHECK(d.store.history().undoLabel() == "Knob Curve");
  // The same curve again is an empty edit; another one replaces it; Default removes it.
  auto same = ops::setParamCurve(d.doc(), d.eq, "freq1", KnobCurve::Linear);
  REQUIRE(same.ok());
  CHECK(same->empty());
  REQUIRE(d.store.dispatch(SetParamCurve{d.eq, "freq1", KnobCurve::Logarithmic}).ok());
  REQUIRE(d.store.dispatch(SetParamCurve{d.eq, "gain1", KnobCurve::Exponential}).ok());
  eq = findDevice(d.doc(), d.eq).device;
  REQUIRE(eq->curves.size() == 2);
  CHECK(eq->curves[0].param == "freq1");  // sorted by param
  REQUIRE(d.store.dispatch(SetParamCurve{d.eq, "gain1", KnobCurve::Default}).ok());
  CHECK(findDevice(d.doc(), d.eq).device->curves.size() == 1);
  REQUIRE(d.store.undo());
  REQUIRE(d.store.undo());
  REQUIRE(d.store.undo());
  CHECK(findDevice(d.doc(), d.eq).device->curve("freq1") == KnobCurve::Linear);
  REQUIRE(d.store.undo());
  CHECK(d.store.current() == before);
  REQUIRE(d.store.redo());
  CHECK(findDevice(d.doc(), d.eq).device->curve("freq1") == KnobCurve::Linear);
  // A rack's macros are linear 0 … 127: no curve.
  auto rack = ops::groupDevices(d.doc(), d.store.ids(), std::vector<DeviceId>{d.lfo});
  REQUIRE(rack.ok());
  REQUIRE(d.store.dispatch(rack->command).ok());
  const Track* t = findTrackPtr(d.doc(), d.keys);
  DeviceId rackId;
  for (const DevicePtr& x : t->devices)
    if (x->rack) rackId = x->id;
  REQUIRE(rackId.valid());
  CHECK_FALSE(ops::setParamCurve(d.doc(), rackId, "macro0", KnobCurve::Exponential).ok());
  CHECK_FALSE(d.store.dispatch(SetParamCurve{rackId, "macro0", KnobCurve::Exponential}).ok());
}

TEST_CASE("knob curves: the file — written as \"curves\", read back equal, bad entries refused") {
  Doc d;
  REQUIRE(d.store.dispatch(SetParamCurve{d.eq, "freq1", KnobCurve::Linear}).ok());
  REQUIRE(d.store.dispatch(SetParamCurve{d.eq, "gain2", KnobCurve::Logarithmic}).ok());
  Result<std::string> text = serializeProject(d.doc());
  REQUIRE(text.ok());
  CHECK(text->find("\"curves\": {\"freq1\": \"linear\", \"gain2\": \"logarithmic\"}") != std::string::npos);
  Result<LoadedProject> back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == d.doc());
  for (const char* bad : {"\"default\"", "\"steep\"", "1"}) {
    std::string t = *text;
    const size_t at = t.find("\"linear\"");
    REQUIRE(at != std::string::npos);
    t.replace(at, 8, bad);
    CHECK_FALSE(parseProject(t).ok());
  }
}

TEST_CASE("knob curves: an Add wire adds before the curve — travel on a curved knob, units on a linear one, dB on a gain") {
  Doc d;
  const WireId cut = d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "lfo", d.eq, "freq1"));
  const WireId g = d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "lfo", d.eq, "gain1"));
  (void)cut;
  (void)g;
  {
    const LoweredProject l = lowerDoc(d.doc());
    REQUIRE(tapOf(l, "freq1") != nullptr);
    REQUIRE(tapOf(l, "gain1") != nullptr);
    CHECK_FALSE(tapOf(l, "freq1")->units);  // octave: in travel
    CHECK(tapOf(l, "gain1")->units);        // linear in dB: in nepers
    CHECK(tapOf(l, "gain1")->db);
  }
  REQUIRE(d.store.dispatch(SetParamCurve{d.eq, "freq1", KnobCurve::Linear}).ok());
  REQUIRE(d.store.dispatch(SetParamCurve{d.eq, "gain1", KnobCurve::Exponential}).ok());
  {
    const LoweredProject l = lowerDoc(d.doc());
    CHECK(tapOf(l, "freq1")->units);  // linear: in Hz
    CHECK_FALSE(tapOf(l, "freq1")->db);
    CHECK_FALSE(tapOf(l, "gain1")->units);  // a bent gain: in travel
  }
}

TEST_CASE("knob curves: 15 -> 16 keeps TX.patching's added wires as Units (same graph), chain links stay added") {
  Doc d;
  const DeviceId util = d.add("utility");
  d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "lfo", d.eq, "freq1"));  // curved: the laws differ
  d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "lfo", d.eq, "gain3"));  // a dB gain: the laws differ
  d.run(ops::connectWire(d.doc(), d.store.ids(), d.lfo, "lfo", util, "balance"));  // linear: the same
  d.run(ops::connectTravelWire(d.doc(), d.store.ids(), d.lfo, "lfo", d.eq, "gain2", 0.4));
  Result<std::string> text = serializeProject(d.doc());
  REQUIRE(text.ok());
  nlohmann::json j = nlohmann::json::parse(*text);
  j["schemaVersion"] = 15;
  // A chain link as TX.groups-fix1 wrote it (schema 15): into an input.
  nlohmann::json link = j["wires"][0];
  link["id"] = 9000;
  link["param"] = "x";
  link["to"] = util.value;
  link["chain"] = true;
  j["wires"].push_back(link);
  Result<LoadedProject> old = parseProject(j.dump(2));
  REQUIRE_MESSAGE(old.ok(), (old.ok() ? std::string() : old.error().message));
  CHECK(old->fileSchemaVersion == 15);
  int units = 0;
  for (const Wire& w : wiresOf(*old->project)) {
    if (w.chain) CHECK(w.law == WireLaw::Add);
    else if (w.param == "gain2") CHECK(w.law == WireLaw::Travel);
    else {
      CHECK(w.law == WireLaw::Units);
      ++units;
    }
  }
  CHECK(units == 3);
  // Re-saved, a Units wire says so; it reads back the same.
  Result<std::string> again = serializeProject(*old->project);
  REQUIRE(again.ok());
  CHECK(again->find("\"law\": \"units\"") != std::string::npos);
  Result<LoadedProject> back = parseProject(*again);
  REQUIRE(back.ok());
  CHECK(*back->project == *old->project);
  // Into a linear param a Units wire and an Add wire are the same math: the same graph.
  Doc a, b;
  const DeviceId ua = a.add("utility"), ub = b.add("utility");
  a.run(ops::connectWire(a.doc(), a.store.ids(), a.lfo, "lfo", ua, "balance"));
  b.run(ops::connectWire(b.doc(), b.store.ids(), b.lfo, "lfo", ub, "balance"));
  Wire w = wiresOf(b.doc()).front();
  REQUIRE(b.store.dispatch(RemoveWire{w.id}).ok());
  w.law = WireLaw::Units;
  REQUIRE(b.store.dispatch(AddWire{w}).ok());
  const LoweredProject la = lowerDoc(a.doc()), lb = lowerDoc(b.doc());
  REQUIRE(la.patches.size() == lb.patches.size());
  for (size_t i = 0; i < la.patches.size(); ++i) CHECK(la.patches[i]->hash == lb.patches[i]->hash);
  // Into a curved one they differ (the Units wire keeps TX.patching's Hz law, the Add wire adds in travel).
  Doc c, e;
  c.run(ops::connectWire(c.doc(), c.store.ids(), c.lfo, "lfo", c.eq, "freq1"));
  e.run(ops::connectWire(e.doc(), e.store.ids(), e.lfo, "lfo", e.eq, "freq1"));
  Wire x = wiresOf(e.doc()).front();
  REQUIRE(e.store.dispatch(RemoveWire{x.id}).ok());
  x.law = WireLaw::Units;
  REQUIRE(e.store.dispatch(AddWire{x}).ok());
  const LoweredProject lc = lowerDoc(c.doc()), le = lowerDoc(e.doc());
  bool differ = false;
  for (size_t i = 0; i < lc.patches.size(); ++i) differ = differ || lc.patches[i]->hash != le.patches[i]->hash;
  CHECK(differ);
  CHECK(tapOf(le, "freq1")->units);
}
