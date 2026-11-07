// The retired meta-module types' state descriptions (GU.4): pq/meta/retired.h.
#include "pq/meta/retired.h"

#include <array>

#include "pq/meta/registry.h"

namespace pq::meta {

namespace {

// ---- phase_acc (as TMM.3 … TMM.18 declared it) ----
constexpr std::array<InputSpec, 2> kPaInputs = {{
    {"hz", "Frequency in", InputDefault::Zero, graph::Kind::Real, Reads::Frequency},
    {"pm", "Phase modulation", InputDefault::Zero, graph::Kind::Real, Reads::Phase},
}};
constexpr std::array<ParamSpec, 4> kPaParams = {{
    {"freq", "Frequency", 0.0, 0.0, 20000.0, graph::Unit::Hz, dsp::CurvePreset::Exp0, 14.0, false, false, true},
    {"phase", "Phase", 0.0, 0.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, true, false},
    {"amp", "Amplitude", 1.0, 0.0, 4.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"pitch", "Pitch", 0.0, -4800.0, 4800.0, graph::Unit::cents, dsp::CurvePreset::Linear, 0.0, false, false},
}};
constexpr std::array<OutputSpec, 1> kPaOutputs = {{{"out", graph::kWidthMono}}};

// ---- vca (as TMM.14 … TX.vca-knob2 declared it) ----
constexpr std::array<InputSpec, 1> kVcaInputs = {{
    {"in", "Signal in", InputDefault::One, graph::Kind::Real, Reads::Signal},
}};
constexpr std::array<ParamSpec, 5 + kRetiredVcaMarks> kVcaParams = {{
    {"value", "Value", kRetiredVcaDefaultPos, 0.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"min", "Min", -kRetiredVcaRange, -kRetiredVcaRange, kRetiredVcaRange, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"max", "Max", kRetiredVcaRange, -kRetiredVcaRange, kRetiredVcaRange, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"zone", "Fine zone", 0.4, 0.05, 0.9, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"bend", "Bend", 0.0, -1.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"mk_n100", "Marker -100", 0.0, -1.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"mk_n10", "Marker -10", 0.0, -1.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"mk_n1", "Marker -1", 0.0, -1.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"mk_p1", "Marker +1", 0.0, -1.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"mk_p10", "Marker +10", 0.0, -1.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"mk_p100", "Marker +100", 0.0, -1.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
}};
constexpr std::array<SettingSpec, 1> kVcaSettings = {{{"curve", "Curve", 0.0, 0.0, 1.0, 2}}};
constexpr std::array<OutputSpec, 1> kVcaOutputs = {{{"out", graph::kWidthMono}}};

Type makePa() {
  Type t;
  t.id = kRetiredPhaseAcc;
  t.name = "Phase Accumulator";
  t.version = 1;
  t.inputs = kPaInputs;
  t.params = kPaParams;
  t.outputs = kPaOutputs;
  t.chainInput = 0;
  return t;
}

Type makeVca() {
  Type t;
  t.id = kRetiredVca;
  t.name = "VCA";
  t.version = 1;
  t.inputs = kVcaInputs;
  t.params = kVcaParams;
  t.settings = kVcaSettings;
  t.outputs = kVcaOutputs;
  t.chainInput = 0;
  return t;
}

}  // namespace

const Type* findRetiredType(std::string_view id) noexcept {
  static const Type pa = makePa();
  static const Type vca = makeVca();
  if (id == kRetiredPhaseAcc) return &pa;
  if (id == kRetiredVca) return &vca;
  return nullptr;
}

const Type* findTypeOrRetired(std::string_view id) noexcept {
  if (const Type* t = findType(id)) return t;
  return findRetiredType(id);
}

}  // namespace pq::meta
