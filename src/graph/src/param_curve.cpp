// GU.1: the curve a param declaration gives (pq/graph/param_curve.h).
#include "pq/graph/param_curve.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace pq::graph {

namespace {
dsp::CurveUnit curveUnitOf(Unit u) noexcept {
  switch (u) {
    case Unit::Hz:
    case Unit::kHz: return dsp::CurveUnit::Hz;
    case Unit::dB: return dsp::CurveUnit::Db;
    case Unit::percent: return dsp::CurveUnit::Percent;
    case Unit::s:
    case Unit::ms: return dsp::CurveUnit::Seconds;
    case Unit::cents: return dsp::CurveUnit::Cents;
    default: return dsp::CurveUnit::None;
  }
}
}  // namespace

dsp::Curve declaredCurve(const ParamDecl& d) noexcept {
  Unit unit = d.defaultValue.unit;
  std::optional<dsp::CurvePreset> preset;
  double arg = 0.0;
  bool steps = false, wrap = false, sgn = false;
  for (const Attr& a : d.attrs) {
    if (a.name == "wrap") wrap = true;
    if (a.name == "signed") sgn = true;
    if (a.args.empty()) continue;
    if (a.name == "unit") {
      Unit u = Unit::None;
      if (unitFromSuffix(a.args[0].value.text, u)) unit = u;
    } else if (a.name == "curve") {
      preset = dsp::curvePresetFromName(a.args[0].value.text);
      const bool num = a.args.size() > 1 && a.args[1].value.type == AttrValue::Type::Number;
      if (preset == dsp::CurvePreset::Exp0)  // TMM.3: `@curve(exp0, k)` (k default 14)
        arg = num ? std::clamp(a.args[1].value.number.value(), 1e-3, 64.0) : 14.0;
      else if (num)
        arg = std::clamp(std::fabs(a.args[1].value.number.value()), 1e-3, 64.0);
    } else if (a.name == "steps" && a.args[0].value.type == AttrValue::Type::Number) {
      steps = std::lround(a.args[0].value.number.value()) > 1;
    }
  }
  const bool isBool = d.kind == Kind::Bool;
  double lo = 0.0, hi = 1.0;
  if (!isBool) {
    const double def = d.defaultValue.value();
    if (d.hasRange) {
      lo = d.rangeMin.value();
      hi = d.rangeMax.value();
      if (unit == Unit::None && d.defaultValue.unit == Unit::None) unit = d.rangeMax.unit;
    } else {
      lo = std::min(0.0, def);
      hi = std::max(1.0, def);
    }
    if (lo > hi) std::swap(lo, hi);
  }
  dsp::Curve c;
  if (!isBool && unit == Unit::dB && lo > 0.0) {  // a dB gain: the curve is in dB (linear, or bent in dB)
    dsp::CurvePreset p = preset.value_or(dsp::CurvePreset::Linear);
    if (p != dsp::CurvePreset::Exponential && p != dsp::CurvePreset::Logarithmic) p = dsp::CurvePreset::Linear;
    c = dsp::curvePreset(p, 20.0 * std::log10(lo), 20.0 * std::log10(hi), arg);
    c.output = dsp::CurveOutput::DbToGain;
  } else {
    dsp::CurvePreset p = preset.value_or(dsp::CurvePreset::Linear);
    if (isBool || steps || !(hi > lo)) p = dsp::CurvePreset::Linear;
    if (p == dsp::CurvePreset::Octave && !(lo > 0.0)) p = dsp::CurvePreset::Linear;
    c = dsp::curvePreset(p, lo, hi, arg);
  }
  c.unit = curveUnitOf(unit);
  if (isBool || steps) c.step = 1.0;
  c.mode = wrap ? dsp::CurveMode::Wrap : sgn ? dsp::CurveMode::Signed : dsp::CurveMode::Clip;
  return c;
}

}  // namespace pq::graph
