// The Phase accumulator meta module (TMM.3): pq/meta/phase_acc.h.
#include "pq/meta/phase_acc.h"

#include <array>
#include <cmath>

namespace pq::meta {

namespace {

constexpr std::array<InputSpec, 2> kInputs = {{
    {"hz", "Frequency in", InputDefault::Zero, graph::Kind::Real, Reads::Frequency},  // TMM.10-fix1: 0 Hz unwired
    {"pm", "Phase modulation", InputDefault::Zero, graph::Kind::Real, Reads::Phase},
}};
constexpr std::array<ParamSpec, 4> kParams = {{
    {"freq", "Frequency", 0.0, 0.0, kPhaseAccMaxHz, graph::Unit::Hz, Curve::Exp0, kPhaseAccCurveK, false, false},
    {"phase", "Phase", 0.0, 0.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, true, false},
    {"amp", "Amplitude", 1.0, 0.0, 4.0, graph::Unit::None, Curve::Linear, 0.0, false, false},
    // TMM.15: last, so the earlier params keep their positions in the call signature (older texts and files: 0).
    {"pitch", "Pitch", 0.0, -kPhaseAccPitchRange, kPhaseAccPitchRange, graph::Unit::cents, Curve::Linear, 0.0, false, false},
}};
static_assert(kParams[kPhaseAccPitch].name == "pitch");
constexpr std::array<OutputSpec, 1> kOutputs = {{{"out", graph::kWidthMono}}};

// Body node ids (stable: the phasor's state key is the call path plus kPhasor; TMM.15's nodes are appended).
enum : uint32_t { kHzSum, kPhasor, kAddPhase, kAddPm, kFrac, kAmp, kTap, kOne, kRatio, kPitched };

Status body(Builder& b) {
  const auto sum = b.atom("add", {b.input(0), b.param(0)}, kHzSum);
  // TMM.15: the pitch transposes the whole frequency, (hz + freq) · 2^(pitch/1200). The ratio reads only a param, so it
  // is param rate (the scheduler's memoised control region: one exp2 in double per block, only while pitch changes);
  // per sample it costs one multiply. cents_to_hz evaluates in double and rounds once, so whole octaves are exactly
  // 2^n and pitch 0 is exactly 1 (x · 1 = x: the same output bits as before the param existed).
  const auto ratio = b.atom("cents_to_hz", {b.param(kPhaseAccPitch), b.konst(1.0, kOne)}, kRatio);
  const auto hz = b.atom("mul", {sum, ratio}, kPitched);
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
  t.chainInput = 0;  // TMM.10-fix1: the chain feeds the frequency (the note source's Hz; a signal before it: FM)
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
double phaseAccPitchRatio(double cents) noexcept { return std::exp2(cents / 1200.0); }

}  // namespace pq::meta
