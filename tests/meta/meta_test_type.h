#pragma once

// A test-only meta-module type (TMM.3): exercises what the Phase accumulator does not — settings that change the
// circuit, state data, a resource slot, the chain forms.
//
//   input    x (default 0; the effect form's audio input)
//   setting  stages 1 … 4 (default 1): tanh applied `stages` times
//   param    gain 0 … 2 (default 0.5, bakeable)
//   data     {"values": Floats}: four floats by default
//   resource one frame-table slot (its id is a constant in the expansion)
//   output   out = tanh^stages(x · gain) + slot · 0   (a modulated gain: clip(gain, 0, 2))
#include <array>
#include <memory>

#include "pq/meta/registry.h"
#include "pq/meta/type.h"

namespace pq::meta::testing {

inline constexpr std::array<InputSpec, 1> kTestInputs = {{{"x", "In", InputDefault::Zero}}};
inline constexpr std::array<ParamSpec, 1> kTestParams = {
    {{"gain", "Gain", 0.5, 0.0, 2.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, true}}};
inline constexpr std::array<SettingSpec, 1> kTestSettings = {{{"stages", "Stages", 1.0, 1.0, 4.0, 4}}};
inline constexpr std::array<OutputSpec, 1> kTestOutputs = {{{"out", graph::kWidthMono}}};

inline Status testBody(Builder& b) {
  // A modulated gain is clamped at audio rate (the "live path"); a static one is used as it is.
  const graph::PortRef g = b.modulated(0) ? b.atom("clip", {b.param(0), b.konst(0.0, 4), b.konst(2.0, 5)}, 6) : b.param(0);
  graph::PortRef y = b.atom("mul", {b.input(0), g}, 0);
  const int stages = static_cast<int>(b.setting(0));
  for (int i = 0; i < stages; ++i) y = b.atom("tanh", {y}, 10 + static_cast<uint32_t>(i));
  const graph::PortRef dead = b.atom("mul", {b.slot(0), b.konst(0.0, 1)}, 2);
  b.output(0, b.atom("add", {y, dead}, 3));
  return Ok{};
}

inline std::vector<ResourceRequest> testResources(std::span<const double>) {
  return {ResourceRequest{ResourceKind::FrameTable, 0}};
}

inline DataPtr testDefaultData() {
  return std::make_shared<const DataValue>(DataValue::record({{"values", DataValue::floatArray({0.0f, 0.0f, 0.0f, 0.0f})}}));
}

inline std::string testDataProblem(const DataValue& d) {
  const DataValue* v = d.field("values");
  if (d.kind != DataValue::Kind::Record || d.fields.size() != 1 || v == nullptr || v->kind != DataValue::Kind::Floats)
    return "test data is {\"values\": floats}";
  return {};
}

inline const Type& testType() {
  static const Type t = [] {
    Type x;
    x.id = "test_shape";
    x.name = "Test Shape";
    x.version = 2;
    x.inputs = kTestInputs;
    x.params = kTestParams;
    x.settings = kTestSettings;
    x.outputs = kTestOutputs;
    x.chainInput = 0;  // TMM.10-fix1: the chain feeds x (every chain form)
    x.resources = &testResources;
    x.body = &testBody;
    x.defaultData = &testDefaultData;
    x.dataProblem = &testDataProblem;
    x.upgradeData = [](const DataPtr& d, uint32_t from) -> Result<DataPtr> {
      if (from != 1) return Error{ErrorCode::Unsupported, "no such version", {}};
      return d;  // version 1 had the same data
    };
    return x;
  }();
  return t;
}

// Registers the test type once (idempotent); every test that uses it calls this first.
inline const Type& registerTestType() {
  const Type& t = testType();
  if (findType(t.id) == nullptr) (void)registerType(t);
  return t;
}

}  // namespace pq::meta::testing
