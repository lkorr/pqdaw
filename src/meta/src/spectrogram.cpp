// The Spectrogram meta module (TX.spectrogram): pq/meta/spectrogram.h.
#include "pq/meta/spectrogram.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>

namespace pq::meta {

namespace {

constexpr std::array<InputSpec, 1> kInputs = {{
    {"in", "Signal in", InputDefault::Zero, graph::Kind::Real, Reads::Signal},
}};
constexpr std::array<OutputSpec, 1> kOutputs = {{{"out", graph::kWidthInfer}}};

enum : uint32_t { kTap, kLeft, kRight, kIndex0, kIndex1, kBeat, kNone };

Status body(Builder& bld) {
  using P = Builder::PortRef;
  const P in = bld.input(0);
  bld.output(0, in);  // pass-through: the input port drives the output port, nothing in between
  const P l = bld.atom("channel", {in, bld.konst(0.0, kIndex0)}, kLeft);  // a mono in fills both channels
  const P r = bld.atom("channel", {in, bld.konst(1.0, kIndex1)}, kRight);
  bld.bind(l, "l");
  bld.bind(r, "r");
  const P beat = bld.atom("transport.beat", {}, kBeat);
  bld.bind(beat, "beat");
  bld.bind(bld.atom("signal_tap", {beat, bld.konst(0.0, kNone), l, r, bld.slot(0)}, kTap), "tap");
  return Ok{};
}

std::vector<ResourceRequest> resources(std::span<const double>) { return {ResourceRequest{ResourceKind::SignalTap, 0}}; }

// The display state: a record of numbers (an empty one by default: every field at its default).
DataPtr defaultData() { return std::make_shared<const DataValue>(DataValue::record({})); }
std::string dataProblem(const DataValue& v) {
  if (v.kind != DataValue::Kind::Record) return "the spectrogram's data is a record of numbers";
  for (const auto& [name, f] : v.fields)
    if (f.kind != DataValue::Kind::Number || !std::isfinite(f.number)) return "the spectrogram's '" + name + "' is not a number";
  return {};
}

Type makeType() {
  Type t;
  t.id = "spectrogram";
  t.name = "Spectrogram";
  t.version = 1;
  t.inputs = kInputs;
  t.outputs = kOutputs;
  t.chainInput = 0;
  t.resources = &resources;
  t.body = &body;
  t.defaultData = &defaultData;
  t.dataProblem = &dataProblem;
  return t;
}

}  // namespace

const Type& spectrogramType() noexcept {
  static const Type t = makeType();
  return t;
}

}  // namespace pq::meta
