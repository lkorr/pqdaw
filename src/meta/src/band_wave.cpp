// The Band Levels and Waveform meta modules (VZ.6): pq/meta/band_wave.h.
#include "pq/meta/band_wave.h"

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

DataPtr defaultData() { return std::make_shared<const DataValue>(DataValue::record({})); }
std::string dataProblem(const DataValue& v) {
  if (v.kind != DataValue::Kind::Record) return "the display's data is a record of numbers";
  for (const auto& [name, f] : v.fields)
    if (f.kind != DataValue::Kind::Number || !std::isfinite(f.number)) return "the display's '" + name + "' is not a number";
  return {};
}

Type makeType(std::string_view id, std::string_view name) {
  Type t;
  t.id = id;
  t.name = name;
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

const Type& bandLevelsType() noexcept {
  static const Type t = makeType("band_levels", "Band Levels");
  return t;
}
const Type& waveformType() noexcept {
  static const Type t = makeType("waveform", "Waveform");
  return t;
}

}  // namespace pq::meta
