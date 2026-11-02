// The VCA meta module (TMM.14): pq/meta/vca.h.
#include "pq/meta/vca.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace pq::meta {

namespace {

constexpr std::array<InputSpec, 1> kInputs = {{
    {"in", "Signal in", InputDefault::One, graph::Kind::Real, Reads::Signal},
}};
constexpr std::array<ParamSpec, kVcaParams> kParams = {{
    // TX.patching: a position whose value (the multiplier) the body computes: added wires add to the multiplier.
    {"value", "Value", kVcaDefaultPos, 0.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, false, true},
    {"min", "Min", -kVcaRange, -kVcaRange, kVcaRange, graph::Unit::None, Curve::Linear, 0.0, false, false},
    {"max", "Max", kVcaRange, -kVcaRange, kVcaRange, graph::Unit::None, Curve::Linear, 0.0, false, false},
    {"zone", "Fine zone", 0.4, kVcaZoneMin, kVcaZoneMax, graph::Unit::None, Curve::Linear, 0.0, false, false},
    {"bend", "Bend", 0.0, -1.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, false},
}};
constexpr std::array<SettingSpec, 1> kSettings = {{{"curve", "Curve", 0.0, 0.0, 1.0, 2}}};
constexpr std::array<OutputSpec, 1> kOutputs = {{{"out", graph::kWidthMono}}};

// The Bend curve is linear below this |K| (float's e^x − 1 near 0 keeps few bits).
constexpr double kLinearK = 0.01;

// Body node ids: the stateful ones fixed (the gain's smoothing state and the tap survive a curve change), the rest in
// build order from kSeq (each curve's order is fixed, so every expansion of one state has the same ids).
enum : uint32_t { kGain, kTap, kSeq = 16 };

class Body {
public:
  using P = Builder::PortRef;
  explicit Body(Builder& b) : b_(b) {}
  P op(std::string_view atom, std::initializer_list<P> ins) { return b_.atom(atom, ins, next_++); }
  P k(double v) { return b_.konst(v, next_++); }
  // A value read more than once needs a name (the printed expansion parses back to the same graph).
  P named(P v, std::string_view name) {
    b_.bind(v, name);
    return v;
  }

private:
  Builder& b_;
  uint32_t next_ = kSeq;
};

// One outer side's coefficients (param rate), signed: outer = c0 + ca·u + cb·e^(K·u).
struct SideCoefs {
  Builder::PortRef c0, ca, cb, k, invLen;
};
SideCoefs sideCoefs(Body& b, const std::string& t, Builder::PortRef s0, Builder::PortRef end, Builder::PortRef extent,
                    Builder::PortRef len, Builder::PortRef slope, Builder::PortRef join, bool negative) {
  using P = Builder::PortRef;
  // extent = ln(end / s0) ≥ 0 (computed once for the travel split)
  const P kj = b.named(b.op("max", {extent, b.k(1.0)}), t + "_kj");
  const P span = b.named(b.op("sub", {end, s0}), t + "_span");
  const P a = b.named(b.op("min", {b.op("mul", {slope, len}), span}), t + "_a");
  const P den = b.op("sub", {b.op("sub", {b.op("exp", {kj}), b.k(1.0)}), kj});
  const P bj = b.named(b.op("div", {b.op("sub", {span, a}), den}), t + "_bj");
  const P alphaJ = b.op("sub", {a, b.op("mul", {bj, kj})});
  const P k = b.op("select", {join, kj, extent});
  const P bb = b.named(b.op("select", {join, bj, s0}), t + "_b");
  const P alpha = b.op("select", {join, alphaJ, b.k(0.0)});
  const P c0 = b.op("sub", {s0, bb});
  const P invLen = b.op("div", {b.k(1.0), b.op("max", {len, b.k(1e-9)})});
  if (!negative) return {c0, alpha, bb, k, invLen};
  return {b.op("neg", {c0}), b.op("neg", {alpha}), b.op("neg", {bb}), k, invLen};
}

Status body(Builder& bld) {
  using P = Builder::PortRef;
  Body b(bld);
  const P in = bld.input(0);
  const P lo = bld.param(kVcaMin);  // a param port is named already
  const P hi = b.named(b.op("max", {bld.param(kVcaMax), lo}), "hi");
  const P pos = b.named(b.op("clip", {bld.param(kVcaValue), b.k(0.0), b.k(1.0)}), "pos");
  P v;
  if (static_cast<VcaCurve>(std::lround(bld.setting(kVcaCurveSetting))) == VcaCurve::Bend) {
    const P k = b.named(b.op("mul", {b.op("clip", {bld.param(kVcaBend), b.k(-1.0), b.k(1.0)}), b.k(kVcaBendK)}), "bk");
    const P lin = b.named(b.op("lt", {b.op("abs", {k}), b.k(kLinearK)}), "blin");
    const P range = b.named(b.op("sub", {hi, lo}), "brange");
    const P den = b.op("select", {lin, b.k(1.0), b.op("sub", {b.op("exp", {k}), b.k(1.0)})});
    const P scale = b.op("div", {range, den});
    const P curved = b.op("mul", {b.op("sub", {b.op("exp", {b.op("mul", {k, pos})}), b.k(1.0)}), scale});
    v = b.op("add", {lo, b.op("select", {lin, b.op("mul", {pos, range}), curved})});
  } else {
    // ---- the Log law (param rate; pq/meta/vca.h) ----
    const P fLo = b.named(b.op("clip", {b.k(-kVcaFine), lo, hi}), "flo");
    const P fw = b.named(b.op("sub", {b.op("clip", {b.k(kVcaFine), lo, hi}), fLo}), "fw");
    const P sP = b.named(b.op("max", {b.k(kVcaFine), lo}), "sp");
    const P sN = b.named(b.op("max", {b.k(kVcaFine), b.op("neg", {hi})}), "sn");
    const P endP = b.named(b.op("max", {hi, sP}), "endp");
    const P endN = b.named(b.op("max", {b.op("neg", {lo}), sN}), "endn");
    const P eP = b.named(b.op("log", {b.op("div", {endP, sP})}), "ep");
    const P eN = b.named(b.op("log", {b.op("div", {endN, sN})}), "en");
    const P e = b.named(b.op("add", {eN, eP}), "e");
    const P zone = b.op("clip", {bld.param(kVcaZone), b.k(kVcaZoneMin), b.k(kVcaZoneMax)});
    const P zf = b.named(b.op("select", {b.op("gt", {e, b.k(0.0)}), b.op("mul", {b.op("mul", {zone, fw}), b.k(0.5 / kVcaFine)}),
                                         b.k(1.0)}),
                         "zf");
    const P share = b.named(b.op("div", {b.op("sub", {b.k(1.0), zf}), b.op("max", {e, b.k(1e-30)})}), "share");
    const P lenN = b.named(b.op("mul", {share, eN}), "pn");  // the negative outer's travel, which ends at pN
    const P lenP = b.named(b.op("mul", {share, eP}), "lenp");
    const P pN = lenN;
    const P pP = b.named(b.op("add", {pN, zf}), "pp");
    const P pC = b.op("add", {pN, b.op("mul", {zf, b.k(0.5)})});
    const P slope = b.named(b.op("div", {fw, b.op("max", {zf, b.k(1e-30)})}), "slope");
    // Whether the fine zone joins each outer (then C1 there), else the outer is a pure exponential.
    const P joinP = b.named(b.op("lt", {lo, b.k(kVcaFine)}), "p_join");
    const P joinN = b.named(b.op("gt", {hi, b.k(-kVcaFine)}), "n_join");
    const SideCoefs sp = sideCoefs(b, "p", sP, endP, eP, lenP, slope, joinP, false);
    const SideCoefs sn = sideCoefs(b, "n", sN, endN, eN, lenN, slope, joinN, true);
    // ---- per position (per sample only when `value` moves at audio rate) ----
    const P side = b.named(b.op("ge", {pos, pC}), "side");
    const P u = b.named(b.op("select", {side, b.op("mul", {b.op("sub", {pos, pP}), sp.invLen}),
                                        b.op("mul", {b.op("sub", {pN, pos}), sn.invLen})}),
                        "u");
    const P c0 = b.op("select", {side, sp.c0, sn.c0});
    const P ca = b.op("select", {side, sp.ca, sn.ca});
    const P cb = b.op("select", {side, sp.cb, sn.cb});
    const P k = b.op("select", {side, sp.k, sn.k});
    const P outer = b.op("add", {b.op("add", {c0, b.op("mul", {ca, u})}), b.op("mul", {cb, b.op("exp", {b.op("mul", {k, u})})})});
    const P fine = b.op("add", {fLo, b.op("mul", {b.op("sub", {pos, pN}), slope})});
    v = b.op("select", {b.op("gt", {u, b.k(0.0)}), outer, fine});
  }
  // TX.patching (SPEC §15 2026-11-01 "a wire adds its value in the target param's own units"; reverses Q183's knob-space
  // wires): a wire into `value` adds to the MULTIPLIER, clamped to [min, max] (the knob, automation and macros still
  // move the position through the curve).
  if (bld.unitsWired(kVcaValue)) v = b.op("clip", {bld.units(kVcaValue, v), lo, hi});
  // out = in · value: smoothed (2 ms) while a param-rate value moves, one multiply once settled; start 1: the first
  // block is at the value (no fade-in).
  bld.output(0, bld.atom("gain", {in, v, b.k(0.002), b.k(1.0)}, kGain));
  b.named(bld.atom("voice_tap", {pos, bld.slot(0)}, kTap), "tap");
  return Ok{};
}

std::vector<ResourceRequest> resources(std::span<const double>) { return {ResourceRequest{ResourceKind::VoiceTap, 0}}; }

Type makeType() {
  Type t;
  t.id = "vca";
  t.name = "VCA";
  t.version = 1;
  t.inputs = kInputs;
  t.params = kParams;
  t.settings = kSettings;
  t.outputs = kOutputs;
  t.chainInput = 0;  // the chain feeds `in` (TMM.10-fix1); at a note source `in` has no signal: a value per voice
  t.resources = &resources;
  t.body = &body;
  return t;
}

double finiteOr(double v, double d) noexcept { return std::isfinite(v) ? v : d; }

}  // namespace

const Type& vcaType() noexcept {
  static const Type t = makeType();
  return t;
}

VcaLaw vcaLaw(VcaCurve curve, double min, double max, double zone, double bend) noexcept {
  VcaLaw l;
  l.curve = curve;
  l.lo = std::clamp(finiteOr(min, -kVcaRange), -kVcaRange, kVcaRange);
  l.hi = std::max(std::clamp(finiteOr(max, kVcaRange), -kVcaRange, kVcaRange), l.lo);
  if (curve == VcaCurve::Bend) {
    l.k = kVcaBendK * std::clamp(finiteOr(bend, 0.0), -1.0, 1.0);
    return l;
  }
  constexpr double w = kVcaFine;
  l.fLo = std::clamp(-w, l.lo, l.hi);
  const double fw = std::clamp(w, l.lo, l.hi) - l.fLo;
  const double sP = std::max(w, l.lo), sN = std::max(w, -l.hi);
  const double endP = std::max(l.hi, sP), endN = std::max(-l.lo, sN);
  const double eP = std::log(endP / sP), eN = std::log(endN / sN);
  const double e = eN + eP;
  const double z = std::clamp(finiteOr(zone, 0.4), kVcaZoneMin, kVcaZoneMax);
  const double zf = e > 0.0 ? z * fw * (0.5 / w) : 1.0;
  const double share = (1.0 - zf) / std::max(e, 1e-30);
  const double lenN = share * eN, lenP = share * eP;
  l.pN = lenN;
  l.pP = lenN + zf;
  l.slope = fw / std::max(zf, 1e-30);
  auto side = [&](VcaLaw::Side& s, double s0, double end, double extent, double len, bool join) {
    s.on = extent > 0.0;
    s.s0 = s0;
    s.len = len;
    if (join) {
      s.k = std::max(extent, 1.0);
      const double a = std::min(l.slope * len, end - s0);
      s.b = (end - s0 - a) / (std::expm1(s.k) - s.k);
      s.alpha = a - s.b * s.k;
    } else {
      s.k = extent;
      s.b = s0;
      s.alpha = 0.0;
    }
  };
  side(l.pos, sP, endP, eP, lenP, l.lo < w);
  side(l.neg, sN, endN, eN, lenN, l.hi > -w);
  return l;
}

VcaLaw vcaLaw(std::span<const double> v, double curveSetting) noexcept {
  auto at = [&](size_t i) { return i < v.size() ? v[i] : kParams[i].def; };
  return vcaLaw(std::lround(curveSetting) == 1 ? VcaCurve::Bend : VcaCurve::Log, at(kVcaMin), at(kVcaMax), at(kVcaZone),
                at(kVcaBend));
}

double vcaValue(const VcaLaw& l, double p) noexcept {
  p = std::clamp(finiteOr(p, 0.0), 0.0, 1.0);
  if (l.curve == VcaCurve::Bend) {
    const double g = std::abs(l.k) < kLinearK ? p : std::expm1(l.k * p) / std::expm1(l.k);
    return l.lo + (l.hi - l.lo) * g;
  }
  auto outer = [](const VcaLaw::Side& s, double u) { return s.s0 + s.alpha * u + s.b * std::expm1(s.k * u); };
  if (l.neg.on && p < l.pN) return -outer(l.neg, (l.pN - p) / l.neg.len);
  if (l.pos.on && p > l.pP) return outer(l.pos, (p - l.pP) / l.pos.len);
  return l.fLo + (p - l.pN) * l.slope;
}

double vcaPosition(const VcaLaw& l, double v) noexcept {
  if (!(v > vcaValue(l, 0.0))) return 0.0;
  if (v >= vcaValue(l, 1.0)) return 1.0;
  double a = 0.0, b = 1.0;  // value(a) < v ≤ value(b)
  for (int i = 0; i < 60; ++i) {
    const double m = 0.5 * (a + b);
    (vcaValue(l, m) < v ? a : b) = m;
  }
  return b;
}

}  // namespace pq::meta
