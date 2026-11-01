// The Phase accumulator meta module (TMM.3): pq/meta/phase_acc.h.
#include "pq/meta/phase_acc.h"

#include <array>

namespace pq::meta {

namespace {

constexpr std::array<InputSpec, 2> kInputs = {{
    {"hz", "Frequency in", InputDefault::VoiceHz},
    {"pm", "Phase modulation", InputDefault::Zero},
}};
constexpr std::array<ParamSpec, 3> kParams = {{
    {"freq", "Frequency", 0.0, 0.0, kPhaseAccMaxHz, graph::Unit::Hz, Curve::Exp0, kPhaseAccCurveK, false, false},
    {"phase", "Phase", 0.0, 0.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, true, false},
    {"amp", "Amplitude", 1.0, 0.0, 4.0, graph::Unit::None, Curve::Linear, 0.0, false, false},
}};
constexpr std::array<OutputSpec, 1> kOutputs = {{{"out", graph::kWidthMono}}};

// Body node ids (stable: the phasor's state key is the call path plus kPhasor).
enum : uint32_t { kHzSum, kPhasor, kAddPhase, kAddPm, kFrac, kAmp, kTap };

Status body(Builder& b) {
  const auto hz = b.atom("add", {b.input(0), b.param(0)}, kHzSum);
  const auto ph = b.atom("phasor", {hz}, kPhasor);
  const auto p1 = b.atom("add", {ph, b.param(1)}, kAddPhase);
  const auto p2 = b.atom("add", {p1, b.input(1)}, kAddPm);
  const auto wrapped = b.atom("frac", {p2}, kFrac);
  b.output(0, b.atom("mul", {b.param(2), wrapped}, kAmp));
  // TMM.5: the face's dots — the phase each voice reads, into the instance's voice tap (one relaxed load per block
  // while no face watches it).
  b.bind(wrapped, "ph");
  b.bind(b.atom("voice_tap", {wrapped, b.slot(0)}, kTap), "tap");
  return Ok{};
}

std::vector<ResourceRequest> resources(std::span<const double>) { return {ResourceRequest{ResourceKind::VoiceTap, 0}}; }

Type makeType() {
  Type t;
  t.id = "phase_acc";
  t.name = "Phase Accumulator";
  t.version = 1;
  t.inputs = kInputs;
  t.params = kParams;
  t.outputs = kOutputs;
  t.chainInstrument = true;
  t.chainVoiceInput = 1;  // TMM.10: after another meta device in a chain, its voice signal modulates the phase (pm)
  t.resources = &resources;
  t.body = &body;
  return t;
}

}  // namespace

// A function-local static: types may be looked up during other translation units' static initialisation.
const Type& phaseAccType() noexcept {
  static const Type t = makeType();
  return t;
}

double phaseAccFreqFromTravel(double t) noexcept { return travelToValue(kParams[0], t); }
double phaseAccTravelFromFreq(double hz) noexcept { return valueToTravel(kParams[0], hz); }

}  // namespace pq::meta
