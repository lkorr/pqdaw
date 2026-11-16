// The pass-through analysers that share one body (an L / R signal tap): the loudness modules (VZ.4, pq/meta/loudness.h)
// and the stereo modules (VZ.5, pq/meta/stereo.h).
#include "pq/meta/loudness.h"
#include "pq/meta/stereo.h"
#include "pq/meta/view3d.h"

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

enum : uint32_t { kTap, kLeft, kRight, kIndex0, kIndex1 };

Status body(Builder& bld) {
  using P = Builder::PortRef;
  const P in = bld.input(0);
  bld.output(0, in);  // pass-through: the input port drives the output port, nothing in between
  const P l = bld.atom("channel", {in, bld.konst(0.0, kIndex0)}, kLeft);  // a mono in fills both channels
  const P r = bld.atom("channel", {in, bld.konst(1.0, kIndex1)}, kRight);
  bld.bind(l, "l");
  bld.bind(r, "r");
  bld.bind(bld.atom("signal_tap", {l, r, l, r, bld.slot(0)}, kTap), "tap");
  return Ok{};
}

std::vector<ResourceRequest> resources(std::span<const double>) { return {ResourceRequest{ResourceKind::SignalTap, 0}}; }

// The display state: a record of numbers (an empty one by default: every option at its default).
DataPtr defaultData() { return std::make_shared<const DataValue>(DataValue::record({})); }
std::string dataProblem(const DataValue& v) {
  if (v.kind != DataValue::Kind::Record) return "an analyser's data is a record of numbers";
  for (const auto& [name, f] : v.fields)
    if (f.kind != DataValue::Kind::Number || !std::isfinite(f.number)) return "an analyser's '" + name + "' is not a number";
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

const Type& levelMeterType() noexcept {
  static const Type t = makeType("level_meter", "Level Meter");
  return t;
}
const Type& lufsMeterType() noexcept {
  static const Type t = makeType("lufs_meter", "LUFS Meter");
  return t;
}
const Type& loudnessHistoryType() noexcept {
  static const Type t = makeType("loudness_history", "Loudness History");
  return t;
}

const Type& stereoFieldType() noexcept {
  static const Type t = makeType("stereo_field", "Stereo Field");
  return t;
}
const Type& stereoImageType() noexcept {
  static const Type t = makeType("stereo_image", "Stereo Image");
  return t;
}
const Type& phaseScopeType() noexcept {
  static const Type t = makeType("phase_scope", "Phase Scope");
  return t;
}

const Type& waterfallType() noexcept {
  static const Type t = makeType("waterfall", "Waterfall");
  return t;
}
const Type& stereoSpectrogramType() noexcept {
  static const Type t = makeType("stereo_spectrogram", "Stereo Spectrogram");
  return t;
}

}  // namespace pq::meta
