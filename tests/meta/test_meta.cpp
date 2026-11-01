// TMM.3: the meta-module framework — registry, call names and paths, deterministic expansion (fixed ids, a fixed
// signature whatever the state, every form), the prelude, state data (equality, hash, base64) and the control laws.
#include <doctest/doctest.h>

#include <cmath>

#include "meta_test_type.h"
#include "pq/graph/parse.h"
#include "pq/graph/patch_file.h"
#include "pq/meta/phase_acc.h"
#include "pq/meta/registry.h"

using namespace pq;
using namespace pq::meta;

namespace {

Expansion mustExpand(const Type& t, const ExpandContext& c, std::string name = "x") {
  auto e = expand(t, c, std::move(name));
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  return std::move(e.value());
}

std::vector<uint32_t> idsOf(const graph::Patch& p) {
  std::vector<uint32_t> v;
  for (graph::NodeIndex n = 0; n < p.nodeCount(); ++n) v.push_back(p.node(n).id.value);
  return v;
}

}  // namespace

TEST_CASE("meta registry: built-in types, call names, instance names, paths") {
  const Type& acc = phaseAccType();
  CHECK(findType("phase_acc") == &acc);
  CHECK(callName(acc) == "meta_phase_acc");
  CHECK(typeOfCall("meta_phase_acc") == &acc);
  CHECK(typeOfCall("meta_phase_acc__d5_acc1_0123456789abcdef") == &acc);
  CHECK(typeOfCall("meta_nothing") == nullptr);
  CHECK(typeOfCall("phase_acc") == nullptr);
  const std::string a = instanceName(acc, 5, "op.acc", 42), b = instanceName(acc, 5, "op_acc", 42);
  CHECK(a.starts_with("meta_phase_acc__d5_op_acc_"));
  CHECK(a != b);  // the key covers the path
  CHECK(typeOfCall(a) == &acc);
  CHECK(validPath(""));
  CHECK(validPath("acc1"));
  CHECK(validPath("opA.acc"));
  CHECK_FALSE(validPath(".a"));
  CHECK_FALSE(validPath("a..b"));
  CHECK_FALSE(validPath("a."));
  CHECK_FALSE(validPath("1a"));
  // Registration: a new id once; invalid ids refused.
  const Type& t = testing::registerTestType();
  CHECK(findType("test_shape") == &t);
  CHECK_FALSE(registerType(t));
  Type bad = t;
  bad.id = "bad__id";
  CHECK_FALSE(registerType(bad));
}

TEST_CASE("meta expansion: deterministic, fixed ids, prints and parses back") {
  const Type& acc = phaseAccType();
  ExpandContext c;
  const Expansion a = mustExpand(acc, c), b = mustExpand(acc, c);
  CHECK(a.patch == b.patch);
  CHECK(idsOf(a.patch) == idsOf(b.patch));
  CHECK(a.key == b.key);
  // TMM.5: its voice tap (the face's dots).
  REQUIRE(a.resources.size() == 1);
  CHECK(a.resources[0].kind == ResourceKind::VoiceTap);
  // Ports at their fixed ids: inputs, params, outputs.
  CHECK(a.patch.node(*a.patch.findByName("hz")).id.value == kInputIdBase);
  CHECK(a.patch.node(*a.patch.findByName("pm")).id.value == kInputIdBase + 1);
  CHECK(a.patch.node(*a.patch.findByName("freq")).id.value == kParamIdBase);
  CHECK(a.patch.node(*a.patch.findByName("amp")).id.value == kParamIdBase + 2);
  CHECK(a.patch.node(*a.patch.findByName("out")).id.value == kOutputIdBase);
  // The text form of the expansion (what TMM.4's expanded view shows) parses back to the same graph.
  const std::string text = graph::print(a.patch);
  CAPTURE(text);
  auto parsed = graph::parse(text, "expansion.pqp");
  REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
  REQUIRE(parsed->patches.size() == 1);
  CHECK(parsed->patches[0] == a.patch);
  CHECK(text.find("@curve(exp0, 14)") != std::string::npos);
  // Outside a voice context hz defaults to 0; the key differs.
  ExpandContext g;
  g.voice = false;
  const Expansion global = mustExpand(acc, g);
  CHECK(global.key != a.key);
  CHECK_FALSE(global.patch == a.patch);
}

TEST_CASE("meta expansion: one signature for every state; settings, static-ness and slots change the body") {
  const Type& t = testing::registerTestType();
  const double s1[] = {1.0}, s3[] = {3.0};
  const uint8_t mod[] = {1};
  const uint32_t slotA[] = {7}, slotB[] = {9};
  ExpandContext c1, c3, cm, csa, csb;
  c1.settings = s1;
  c3.settings = s3;
  cm.modulated = mod;
  csa.slots = slotA;
  csb.slots = slotB;
  const Expansion e1 = mustExpand(t, c1), e3 = mustExpand(t, c3), em = mustExpand(t, cm), ea = mustExpand(t, csa),
                  eb = mustExpand(t, csb);
  const graph::SubcircuitRef sig = graph::subcircuitSignature(e1.patch);
  for (const Expansion* e : {&e3, &em, &ea, &eb}) CHECK(graph::subcircuitSignature(e->patch) == sig);
  CHECK(e3.patch.nodeCount() == e1.patch.nodeCount() + 2);  // two more tanh stages
  CHECK(em.patch.nodeCount() == e1.patch.nodeCount() + 3);  // clip + its two bounds
  CHECK_FALSE(ea.patch == eb.patch);                        // the slot id is a constant of the expansion
  CHECK(e1.key != e3.key);
  CHECK(e1.key != em.key);
  CHECK(ea.key != eb.key);
  REQUIRE(e1.resources.size() == 1);
  CHECK(e1.resources[0].kind == ResourceKind::FrameTable);
  // The nodes the settings share keep their ids (state keys survive a settings change).
  for (const char* name : {"x", "gain", "out"}) CHECK(e1.patch.node(*e1.patch.findByName(name)).id ==
                                                     e3.patch.node(*e3.patch.findByName(name)).id);
  // Out of range settings are clamped: 9 stages = 4.
  const double s9[] = {9.0}, s4[] = {4.0};
  ExpandContext c9, c4;
  c9.settings = s9;
  c4.settings = s4;
  CHECK(mustExpand(t, c9).patch == mustExpand(t, c4).patch);
  CHECK(expansionKey(t, c9) == expansionKey(t, c4));
}

TEST_CASE("meta expansion: chain forms are devices") {
  const Type& t = testing::registerTestType();
  ExpandContext inst;
  inst.form = Form::ChainInstrument;
  const Expansion i = mustExpand(t, inst);
  // An instrument: no audio input, a per-voice first output and `done`.
  for (graph::NodeIndex n = 0; n < i.patch.nodeCount(); ++n) CHECK(i.patch.node(n).kind != graph::NodeKind::Input);
  const auto out = i.patch.findByName("out");
  REQUIRE(out);
  CHECK(i.patch.inputType(*out, 0).poly == graph::Poly::Voice);
  CHECK(i.patch.findByName("done").has_value());
  ExpandContext fx;
  fx.form = Form::ChainEffect;
  fx.voice = false;
  const Expansion e = mustExpand(t, fx);
  const auto in = e.patch.findByName("x");
  REQUIRE(in);
  CHECK(e.patch.node(*in).kind == graph::NodeKind::Input);
  CHECK(e.patch.outputType(*in, 0).poly == graph::Poly::Global);
  // The Phase accumulator is an instrument only.
  CHECK_FALSE(expand(phaseAccType(), fx, "y").ok());
  ExpandContext ai;
  ai.form = Form::ChainInstrument;
  CHECK(expand(phaseAccType(), ai, "y").ok());
}

TEST_CASE("meta prelude: one default expansion and signature per type") {
  testing::registerTestType();
  const std::shared_ptr<const Prelude> p = prelude();
  const Prelude::Entry* acc = p->find("phase_acc");
  REQUIRE(acc != nullptr);
  CHECK(acc->nested.name() == "meta_phase_acc");
  CHECK(acc->instrument.has_value());
  CHECK_FALSE(acc->effect.has_value());
  const Prelude::Entry* t = p->find("test_shape");
  REQUIRE(t != nullptr);
  CHECK(t->effect.has_value());
  REQUIRE(p->signatures.size() == p->entries.size());
  // The params of every form are the same (the document's param lookups read any of them).
  auto params = [](const graph::Patch& x) {
    std::vector<std::string> v;
    for (graph::NodeIndex n = 0; n < x.nodeCount(); ++n)
      if (x.node(n).kind == graph::NodeKind::Param) v.push_back(x.nodeName(n));
    return v;
  };
  CHECK(params(acc->nested) == params(*acc->instrument));
  // A call parses against the prelude's signatures.
  graph::ParseOptions po;
  po.externals = p->signatures;
  auto f = graph::parse("patch P {\n  out out : audio\n  acc1 = meta_phase_acc(freq: 100Hz)\n  out = acc1\n}\n", "p.pqp", po);
  REQUIRE_MESSAGE(f.ok(), (f.ok() ? std::string() : f.error().message));
}

TEST_CASE("meta data: deep equality, hash, base64 floats, problems") {
  const DataValue a = DataValue::record({{"b", DataValue::num(2.0)},
                                         {"a", DataValue::list({DataValue::str("x"), DataValue::floatArray({1.5f, -0.25f})})}});
  const DataValue b = DataValue::record({{"a", DataValue::list({DataValue::str("x"), DataValue::floatArray({1.5f, -0.25f})})},
                                         {"b", DataValue::num(2.0)}});
  CHECK(a == b);
  CHECK(dataHash(a) == dataHash(b));
  CHECK(a.fields.front().first == "a");
  DataValue c = b;
  c.fields[0].second.items[1].floats[1] = -0.5f;
  CHECK_FALSE(a == c);
  CHECK(dataHash(a) != dataHash(c));
  // ±0 differ (bit for bit), NaN equals itself.
  CHECK_FALSE(DataValue::num(0.0) == DataValue::num(-0.0));
  CHECK(DataValue::num(std::nan("")) == DataValue::num(std::nan("")));
  CHECK(dataProblem(a).empty());
  CHECK_FALSE(dataProblem(DataValue::num(INFINITY)).empty());
  DataValue badName;
  badName.kind = DataValue::Kind::Record;
  badName.fields.emplace_back("$f32", DataValue::num(1.0));
  CHECK_FALSE(dataProblem(badName).empty());
  // Base64 float32 LE round trips bit for bit, every length mod 3.
  for (size_t n : {0u, 1u, 2u, 3u, 7u, 2048u}) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = std::sin(static_cast<float>(i) * 0.37f) * 3.0f;
    const std::string s = encodeFloats(v);
    CHECK(s.size() % 4 == 0);
    std::vector<float> back;
    REQUIRE(decodeFloats(s, back));
    CHECK(back == v);
  }
  CHECK(encodeFloats({1.0f}) == "AACAPw==");  // 0x3F800000 little-endian
  std::vector<float> out;
  CHECK_FALSE(decodeFloats("AACAPw=", out));
  CHECK_FALSE(decodeFloats("AA=APw==", out));
  CHECK_FALSE(decodeFloats("AACA", out));  // 3 bytes: not a whole float
}

TEST_CASE("meta laws: the Phase accumulator's frequency travel") {
  CHECK(phaseAccFreqFromTravel(0.0) == 0.0);
  CHECK(phaseAccFreqFromTravel(1.0) == doctest::Approx(20000.0).epsilon(1e-12));
  CHECK(phaseAccFreqFromTravel(-1.0) == 0.0);
  // f = 20 kHz · (2^(14t) − 1)/(2^14 − 1).
  for (double t : {0.1, 0.25, 0.5, 0.75, 0.9}) {
    const double f = 20000.0 * (std::pow(2.0, 14.0 * t) - 1.0) / (std::pow(2.0, 14.0) - 1.0);
    CHECK(phaseAccFreqFromTravel(t) == doctest::Approx(f).epsilon(1e-12));
    CHECK(phaseAccTravelFromFreq(f) == doctest::Approx(t).epsilon(1e-12));
  }
  CHECK(phaseAccTravelFromFreq(0.0) == 0.0);
  CHECK(phaseAccTravelFromFreq(1e9) == 1.0);
  const ParamSpec lin{"g", "", 0.0, -1.0, 3.0};
  CHECK(travelToValue(lin, 0.5) == 1.0);
  CHECK(valueToTravel(lin, 1.0) == 0.5);
  const ParamSpec ex{"c", "", 100.0, 10.0, 1000.0, graph::Unit::Hz, Curve::Exp};
  CHECK(travelToValue(ex, 0.5) == doctest::Approx(100.0));
  CHECK(valueToTravel(ex, 100.0) == doctest::Approx(0.5));
}
