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
    // TX.knob-curves: none has a knob curve (the knob travels along the curve box; the range ends and the box are typed /
    // dragged values of that curve).
    {"value", "Value", kVcaDefaultPos, 0.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, false, true, false, true},
    {"min", "Min", -kVcaRange, -kVcaRange, kVcaRange, graph::Unit::None, Curve::Linear, 0.0, false, false, false, false, true},
    {"max", "Max", kVcaRange, -kVcaRange, kVcaRange, graph::Unit::None, Curve::Linear, 0.0, false, false, false, false, true},
    {"zone", "Fine zone", 0.4, kVcaZoneMin, kVcaZoneMax, graph::Unit::None, Curve::Linear, 0.0, false, false, false, false, true},
    {"bend", "Bend", 0.0, -1.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, false, false, false, true},
    // TX.vca-knob: the markers' offsets (kVcaMarkValue order), 0: where the curve puts their values.
    {"mk_n100", "Marker -100", 0.0, -1.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, false, false, false, true},
    {"mk_n10", "Marker -10", 0.0, -1.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, false, false, false, true},
    {"mk_n1", "Marker -1", 0.0, -1.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, false, false, false, true},
    {"mk_p1", "Marker +1", 0.0, -1.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, false, false, false, true},
    {"mk_p10", "Marker +10", 0.0, -1.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, false, false, false, true},
    {"mk_p100", "Marker +100", 0.0, -1.0, 1.0, graph::Unit::None, Curve::Linear, 0.0, false, false, false, false, true},
}};
static_assert(kVcaMarks == 6, "one param per marker value (kParams)");
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
  Builder::PortRef u0, ua, ub;  // unsigned (the positive form): outer = u0 + ua·u + ub·e^(K·u)
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
  const P c0n = b.named(c0, t + "_c0");
  const P alphaN = b.named(alpha, t + "_alpha");
  const P kn = b.named(k, t + "_k");
  if (!negative) return {c0n, alphaN, bb, kn, invLen, c0n, alphaN, bb};
  return {b.op("neg", {c0n}), b.op("neg", {alphaN}), b.op("neg", {bb}), kn, invLen, c0n, alphaN, bb};
}

constexpr int kNewtonSteps = 5;

// TX.vca-knob: u ∈ [0, 1] where an outer (unsigned) reaches t: the exponential's own inverse, then Newton steps (the
// outer is convex and increasing). vcaLaw computes the same steps.
Builder::PortRef outerInverse(Body& b, const SideCoefs& sc, double t, const std::string& name) {
  using P = Builder::PortRef;
  const P tk = b.named(b.k(t), name + "_t");
  const P r = b.op("div", {b.op("sub", {tk, sc.u0}), b.op("max", {sc.ub, b.k(1e-30)})});
  P u = b.op("clip", {b.op("div", {b.op("log", {b.op("max", {r, b.k(1e-30)})}), b.op("max", {sc.k, b.k(1e-30)})}), b.k(0.0), b.k(1.0)});
  for (int i = 0; i < kNewtonSteps; ++i) {
    u = b.named(u, name + "_u" + std::to_string(i));
    const P ex = b.named(b.op("exp", {b.op("mul", {sc.k, u})}), name + "_e" + std::to_string(i));
    const P f = b.op("sub", {b.op("add", {b.op("add", {sc.u0, b.op("mul", {sc.ua, u})}), b.op("mul", {sc.ub, ex})}), tk});
    const P fp = b.op("add", {sc.ua, b.op("mul", {b.op("mul", {sc.ub, sc.k}), ex})});
    u = b.op("clip", {b.op("sub", {u, b.op("div", {f, b.op("max", {fp, b.k(1e-30)})})}), b.k(0.0), b.k(1.0)});
  }
  return b.named(u, name + "_u");
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
    // ---- TX.vca-knob: the markers (param rate): base positions, ordered positions, the warp's segments ----
    std::array<P, kVcaMarks> base{}, at{};
    std::array<P, kVcaMarks + 1> gap{};
    std::array<P, kVcaMarks> shown{};
    for (uint32_t i = 0; i < kVcaMarks; ++i) {
      const double mv = kVcaMarkValue[i];
      const std::string m = "m" + std::to_string(i);
      shown[i] = b.named(b.op("and", {b.op("lt", {lo, b.k(mv)}), b.op("lt", {b.k(mv), hi})}), m + "_shown");
      P bv;
      if (mv == -kVcaFine) bv = pN;
      else if (mv == kVcaFine) bv = pP;
      else if (std::abs(mv) < kVcaFine) bv = b.op("add", {pN, b.op("div", {b.op("sub", {b.k(mv), fLo}), b.op("max", {slope, b.k(1e-30)})})});
      else if (mv > 0.0) bv = b.op("add", {pP, b.op("mul", {outerInverse(b, sp, mv, m), lenP})});
      else bv = b.op("sub", {pN, b.op("mul", {outerInverse(b, sn, -mv, m), lenN})});
      const P hidden = b.op("select", {b.op("le", {b.k(mv), lo}), b.k(0.0), b.k(1.0)});
      base[i] = b.named(b.op("select", {shown[i], bv, hidden}), m + "_base");
    }
    for (uint32_t i = 0; i <= kVcaMarks; ++i) {
      const P prev = i == 0 ? b.k(0.0) : base[i - 1];
      const P cur = i == kVcaMarks ? b.k(1.0) : base[i];
      gap[i] = b.named(b.op("min", {b.k(kVcaMarkGap), b.op("sub", {cur, prev})}), "g" + std::to_string(i));
    }
    P low = b.k(0.0), prevAt = b.k(0.0), moved = b.k(0.0);
    for (uint32_t i = 0; i < kVcaMarks; ++i) {
      const std::string m = "m" + std::to_string(i);
      low = b.named(b.op("add", {low, gap[i]}), m + "_low");
      P rest = gap[kVcaMarks];
      for (uint32_t j = kVcaMarks - 1; j > i; --j) rest = b.op("add", {rest, gap[j]});
      const P high = b.op("sub", {b.k(1.0), rest});
      const P want = b.op("add", {base[i], b.op("clip", {bld.param(kVcaMark0 + i), b.k(-1.0), b.k(1.0)})});
      const P ordered = b.op("max", {b.op("clip", {want, low, high}), b.op("add", {prevAt, gap[i]})});
      at[i] = b.named(b.op("select", {shown[i], ordered, base[i]}), m + "_at");
      prevAt = at[i];
      moved = b.op("add", {moved, b.op("abs", {b.op("sub", {at[i], base[i]})})});
    }
    const P warped = b.named(b.op("gt", {moved, b.k(0.0)}), "warped");
    // Segment s runs from knot s − 1 to knot s (knot −1 = (0, 0), knot kVcaMarks = (1, 1)): q = r·p + c.
    std::array<P, kVcaMarks + 1> segR{}, segC{};
    for (uint32_t sI = 0; sI <= kVcaMarks; ++sI) {
      const P pb = sI == kVcaMarks ? b.k(1.0) : at[sI], bb = sI == kVcaMarks ? b.k(1.0) : base[sI];
      if (sI == 0) {  // from (0, 0): a constant read once each
        segR[0] = b.named(b.op("div", {bb, b.op("max", {pb, b.k(1e-30)})}), "r0");
        segC[0] = b.k(0.0);
        continue;
      }
      const P pa = at[sI - 1], ba = base[sI - 1];
      segR[sI] = b.named(b.op("div", {b.op("sub", {bb, ba}), b.op("max", {b.op("sub", {pb, pa}), b.k(1e-30)})}), "r" + std::to_string(sI));
      segC[sI] = b.op("sub", {ba, b.op("mul", {pa, segR[sI]})});
    }
    // ---- per position (per sample only when `value` moves at audio rate) ----
    P r = segR[0], c = segC[0];
    for (uint32_t i = 0; i < kVcaMarks; ++i) {
      const P past = b.named(b.op("ge", {pos, at[i]}), "past" + std::to_string(i));
      r = b.op("select", {past, segR[i + 1], r});
      c = b.op("select", {past, segC[i + 1], c});
    }
    const P q = b.named(b.op("select", {warped, b.op("clip", {b.op("add", {b.op("mul", {r, pos}), c}), b.k(0.0), b.k(1.0)}), pos}), "q");
    const P side = b.named(b.op("ge", {q, pC}), "side");
    const P u = b.named(b.op("select", {side, b.op("mul", {b.op("sub", {q, pP}), sp.invLen}),
                                        b.op("mul", {b.op("sub", {pN, q}), sn.invLen})}),
                        "u");
    const P c0 = b.op("select", {side, sp.c0, sn.c0});
    const P ca = b.op("select", {side, sp.ca, sn.ca});
    const P cb = b.op("select", {side, sp.cb, sn.cb});
    const P k = b.op("select", {side, sp.k, sn.k});
    const P outer = b.op("add", {b.op("add", {c0, b.op("mul", {ca, u})}), b.op("mul", {cb, b.op("exp", {b.op("mul", {k, u})})})});
    const P fine = b.op("add", {fLo, b.op("mul", {b.op("sub", {q, pN}), slope})});
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

// TX.knob-curves (Type::unitsLaw): an Add wire into `value` adds to the multiplier on the Bend curve (a line between min
// and max at bend 0: the knob's units), to the knob position on the Log curve (through the curve, as a cutoff's octaves).
bool unitsLaw(std::span<const double> settings, uint32_t param) {
  if (param != kVcaValue) return true;
  const double curve = kVcaCurveSetting < settings.size() ? settings[kVcaCurveSetting] : 0.0;
  return static_cast<VcaCurve>(std::lround(curve)) == VcaCurve::Bend;
}

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
  t.unitsLaw = &unitsLaw;
  return t;
}

double finiteOr(double v, double d) noexcept { return std::isfinite(v) ? v : d; }

}  // namespace

const Type& vcaType() noexcept {
  static const Type t = makeType();
  return t;
}

namespace {

// TX.vca-knob: the expansion's outerInverse, in doubles (the same steps).
double outerInverse(const VcaLaw::Side& sd, double t) noexcept {
  const double c0 = sd.s0 - sd.b;
  double u = std::clamp(std::log(std::max((t - c0) / std::max(sd.b, 1e-30), 1e-30)) / std::max(sd.k, 1e-30), 0.0, 1.0);
  for (int i = 0; i < kNewtonSteps; ++i) {
    const double ex = std::exp(sd.k * u);
    const double f = c0 + sd.alpha * u + sd.b * ex - t;
    const double fp = sd.alpha + sd.b * sd.k * ex;
    u = std::clamp(u - f / std::max(fp, 1e-30), 0.0, 1.0);
  }
  return u;
}

// The markers: base positions, ordered positions, the warp's segments (pq/meta/vca.h "THE MARKERS").
void placeMarks(VcaLaw& l, std::span<const double> offsets) noexcept {
  const bool log = l.curve == VcaCurve::Log;
  for (uint32_t i = 0; i < kVcaMarks; ++i) {
    VcaLaw::Mark& m = l.marks[i];
    const double v = kVcaMarkValue[i];
    m.value = v;
    m.shown = log && l.lo < v && v < l.hi;
    double b = v <= l.lo ? 0.0 : 1.0;
    if (m.shown) {
      if (v == -kVcaFine) b = l.pN;
      else if (v == kVcaFine) b = l.pP;
      else if (std::abs(v) < kVcaFine) b = l.pN + (v - l.fLo) / std::max(l.slope, 1e-30);
      else if (v > 0.0) b = l.pP + outerInverse(l.pos, v) * l.pos.len;
      else b = l.pN - outerInverse(l.neg, -v) * l.neg.len;
    }
    m.base = b;
  }
  std::array<double, kVcaMarks + 1> gap{};
  for (uint32_t i = 0; i <= kVcaMarks; ++i) {
    const double prev = i == 0 ? 0.0 : l.marks[i - 1].base, cur = i == kVcaMarks ? 1.0 : l.marks[i].base;
    gap[i] = std::min(kVcaMarkGap, cur - prev);
  }
  double low = 0.0, prevAt = 0.0, moved = 0.0;
  for (uint32_t i = 0; i < kVcaMarks; ++i) {
    VcaLaw::Mark& m = l.marks[i];
    low = low + gap[i];
    double rest = gap[kVcaMarks];
    for (uint32_t j = kVcaMarks - 1; j > i; --j) rest = rest + gap[j];
    const double high = 1.0 - rest;
    const double d = i < offsets.size() ? std::clamp(finiteOr(offsets[i], 0.0), -1.0, 1.0) : 0.0;
    const double ordered = std::max(std::clamp(m.base + d, low, high), prevAt + gap[i]);
    m.at = m.shown ? ordered : m.base;
    prevAt = m.at;
    moved = moved + std::abs(m.at - m.base);
  }
  l.warped = moved > 0.0;
  for (uint32_t sI = 0; sI <= kVcaMarks; ++sI) {
    const double pb = sI == kVcaMarks ? 1.0 : l.marks[sI].at, bb = sI == kVcaMarks ? 1.0 : l.marks[sI].base;
    if (sI == 0) {
      l.segR[0] = bb / std::max(pb, 1e-30);
      l.segC[0] = 0.0;
      continue;
    }
    const double pa = l.marks[sI - 1].at, ba = l.marks[sI - 1].base;
    l.segR[sI] = (bb - ba) / std::max(pb - pa, 1e-30);
    l.segC[sI] = ba - pa * l.segR[sI];
  }
}

}  // namespace

VcaLaw vcaLaw(VcaCurve curve, double min, double max, double zone, double bend, std::span<const double> offsets) noexcept {
  VcaLaw l;
  l.curve = curve;
  l.lo = std::clamp(finiteOr(min, -kVcaRange), -kVcaRange, kVcaRange);
  l.hi = std::max(std::clamp(finiteOr(max, kVcaRange), -kVcaRange, kVcaRange), l.lo);
  if (curve == VcaCurve::Bend) {
    l.k = kVcaBendK * std::clamp(finiteOr(bend, 0.0), -1.0, 1.0);
    placeMarks(l, offsets);  // every marker hidden: no warp
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
  placeMarks(l, offsets);
  return l;
}

VcaLaw vcaLaw(std::span<const double> v, double curveSetting) noexcept {
  auto at = [&](size_t i) { return i < v.size() ? v[i] : kParams[i].def; };
  std::array<double, kVcaMarks> off{};
  for (uint32_t i = 0; i < kVcaMarks; ++i) off[i] = at(kVcaMark0 + i);
  return vcaLaw(std::lround(curveSetting) == 1 ? VcaCurve::Bend : VcaCurve::Log, at(kVcaMin), at(kVcaMax), at(kVcaZone),
                at(kVcaBend), off);
}

double vcaWarp(const VcaLaw& l, double p) noexcept {
  p = std::clamp(finiteOr(p, 0.0), 0.0, 1.0);
  if (!l.warped) return p;
  double r = l.segR[0], c = l.segC[0];
  for (uint32_t i = 0; i < kVcaMarks; ++i)
    if (p >= l.marks[i].at) {
      r = l.segR[i + 1];
      c = l.segC[i + 1];
    }
  return std::clamp(r * p + c, 0.0, 1.0);
}

double vcaMarkOffset(const VcaLaw& l, uint32_t i, double p, double current) noexcept {
  if (i >= kVcaMarks || !l.marks[i].shown) return current;
  double lo = 0.0, hi = 1.0;
  for (uint32_t j = i; j-- > 0;)
    if (l.marks[j].shown) {
      lo = l.marks[j].at;
      break;
    }
  for (uint32_t j = i + 1; j < kVcaMarks; ++j)
    if (l.marks[j].shown) {
      hi = l.marks[j].at;
      break;
    }
  const double g = std::min(kVcaMarkGap, 0.5 * (hi - lo));
  const double at = std::clamp(finiteOr(p, l.marks[i].at), lo + g, hi - g);
  return std::clamp(at - l.marks[i].base, -1.0, 1.0);
}

uint32_t vcaMarkPartner(uint32_t i) noexcept {
  if (i >= kVcaMarks) return kVcaMarks;
  for (uint32_t j = 0; j < kVcaMarks; ++j)
    if (j != i && kVcaMarkValue[j] == -kVcaMarkValue[i]) return j;
  return kVcaMarks;
}

std::array<double, kVcaMarks> vcaMoveMark(std::span<const double> params, double curveSetting, uint32_t i, double p,
                                          bool paired) noexcept {
  auto at = [&](size_t k) { return k < params.size() ? params[k] : kParams[k].def; };
  std::array<double, kVcaMarks> off{};
  for (uint32_t k = 0; k < kVcaMarks; ++k) off[k] = std::clamp(finiteOr(at(kVcaMark0 + k), 0.0), -1.0, 1.0);
  const VcaCurve curve = std::lround(curveSetting) == 1 ? VcaCurve::Bend : VcaCurve::Log;
  const VcaLaw l0 = vcaLaw(curve, at(kVcaMin), at(kVcaMax), at(kVcaZone), at(kVcaBend), off);
  if (i >= kVcaMarks || !l0.marks[i].shown) return off;
  p = std::clamp(finiteOr(p, l0.marks[i].at), 0.0, 1.0);
  const uint32_t j = vcaMarkPartner(i);
  // The pair moves together only from an ordered state (every shown marker where its offset puts it).
  auto clean = [&](const std::array<double, kVcaMarks>& o) {
    const VcaLaw l = vcaLaw(curve, at(kVcaMin), at(kVcaMax), at(kVcaZone), at(kVcaBend), o);
    for (uint32_t k = 0; k < kVcaMarks; ++k)
      if (l.marks[k].shown && l.marks[k].at != l.marks[k].base + o[k]) return false;
    return true;
  };
  if (!paired || j >= kVcaMarks || !l0.marks[j].shown || !clean(off)) {
    off[i] = vcaMarkOffset(l0, i, p, off[i]);
    return off;
  }
  // The gap the drag keeps (vcaMarkOffset's): at least kVcaMarkGap from a neighbour or an end, the law's own order too.
  auto moved = [&](double d) {
    std::array<double, kVcaMarks> o = off;
    o[i] = off[i] + d;
    o[j] = off[j] - d;
    return o;
  };
  auto ok = [&](double d) {
    if (d == 0.0) return true;
    const std::array<double, kVcaMarks> o = moved(d);
    if (std::abs(o[i]) > 1.0 || std::abs(o[j]) > 1.0 || !clean(o)) return false;
    const VcaLaw l = vcaLaw(curve, at(kVcaMin), at(kVcaMax), at(kVcaZone), at(kVcaBend), o);
    for (const uint32_t m : {i, j}) {
      double prev = 0.0, prev0 = 0.0, next = 1.0, next0 = 1.0;
      for (uint32_t q = m; q-- > 0;)
        if (l.marks[q].shown) {
          prev = l.marks[q].at;
          prev0 = l0.marks[q].at;
          break;
        }
      for (uint32_t q = m + 1; q < kVcaMarks; ++q)
        if (l.marks[q].shown) {
          next = l.marks[q].at;
          next0 = l0.marks[q].at;
          break;
        }
      // The gap from each neighbour (or end), or what the start already had where that was less.
      if (l.marks[m].at - prev < std::min(kVcaMarkGap, l0.marks[m].at - prev0) - 1e-12) return false;
      if (next - l.marks[m].at < std::min(kVcaMarkGap, next0 - l0.marks[m].at) - 1e-12) return false;
    }
    return true;
  };
  const double want = p - l0.marks[i].at;
  double d = want;
  if (!ok(want)) {
    double lo = 0.0, hi = 1.0;  // the share of `want` that fits (the feasible moves are an interval around 0)
    for (int it = 0; it < 48; ++it) {
      const double m = 0.5 * (lo + hi);
      (ok(want * m) ? lo : hi) = m;
    }
    d = want * lo;
  }
  return moved(d);
}

double vcaValue(const VcaLaw& l, double p) noexcept {
  p = vcaWarp(l, p);
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
