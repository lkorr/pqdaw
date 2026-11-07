// The Envelope meta module (TMM.11): pq/meta/envelope.h.
#include "pq/meta/envelope.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace pq::meta {

namespace {

constexpr std::array<InputSpec, 2> kInputs = {{
    {"in", "Signal in (a VCA)", InputDefault::One, graph::Kind::Real},
    {"gate", "Gate", InputDefault::VoiceGate, graph::Kind::Bool, Reads::Gate},
}};
constexpr std::array<ParamSpec, kEnvParams> kParams = {{
    {"attack", "Attack", 0.005, 0.0, kEnvMaxSeconds, graph::Unit::s, dsp::CurvePreset::Exp0, kEnvCurveK, false, false},
    {"decay", "Decay", 0.4, 0.001, kEnvMaxSeconds, graph::Unit::s, dsp::CurvePreset::Exp0, kEnvCurveK, false, false},
    {"sustain", "Sustain", 0.6, 0.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"release", "Release", 0.25, 0.001, kEnvMaxSeconds, graph::Unit::s, dsp::CurvePreset::Exp0, kEnvCurveK, false, false},
    {"acurve", "Attack curve", 0.0, -1.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"dcurve", "Decay curve", 0.0, -1.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"rcurve", "Release curve", 0.0, -1.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
    {"amount", "Amount", 1.0, 0.0, 1.0, graph::Unit::None, dsp::CurvePreset::Linear, 0.0, false, false},
}};
constexpr std::array<OutputSpec, 1> kOutputs = {{{"out", graph::kWidthMono}}};

// Body node ids. The stateful nodes (whose state keys must survive every re-expansion) have fixed ids; every other
// node takes the next id from kSeq in build order (the body has no settings, so the order is the same in every
// expansion of one version).
enum : uint32_t { kStart0, kLvl, kZAtt, kTap, kSeq = 16 };

// Build helpers: each node a fresh id; values read more than once get a name, so the printed expansion parses back to
// the same graph (pq/meta/type.h Builder::bind).
class Body {
public:
  using P = Builder::PortRef;
  explicit Body(Builder& b) : b_(b) {}
  P op(std::string_view atom, std::initializer_list<P> ins) { return b_.atom(atom, ins, next_++); }
  P k(double v) { return b_.konst(v, next_++); }
  P named(P v, const std::string& name) {
    b_.bind(v, name);
    return v;
  }

private:
  Builder& b_;
  uint32_t next_ = kSeq;
};

struct FallLaw {
  Builder::PortRef coef, w;  // w: the pull, negated
};

// One falling segment's param-rate law (pq/meta/envelope.h): coef = exp(−δ / max(t·sr, 1)) — with curve 0 msenv's own
// expression, δ = ln 10⁴ exactly — and the overshoot's pull w = ov·span·(1 − k).
FallLaw fallLaw(Body& b, const std::string& t, Builder::PortRef time, Builder::PortRef curve, Builder::PortRef span,
                Builder::PortRef sr) {
  using P = Builder::PortRef;
  // δ = ln 10⁴ · max(1 − c·(c < 0 ? 2 : 1), 0.01)
  const P two = b.op("select", {b.op("lt", {curve, b.k(0.0)}), b.k(2.0), b.k(1.0)});
  const P f = b.op("max", {b.op("sub", {b.k(1.0), b.op("mul", {curve, two})}), b.k(0.01)});
  const P delta = b.named(b.op("mul", {b.k(kEnvDepth), f}), t + "_depth");
  const P negDelta = b.named(b.op("neg", {delta}), t + "_negdepth");
  const P n = b.named(b.op("max", {b.op("mul", {time, sr}), b.k(1.0)}), t + "_n");
  const P coef = b.named(b.op("exp", {b.op("div", {negDelta, n})}), t + "_coef");
  // ov = c > 0 ? q / (1 − q) : 0, q = e^−δ
  const P q = b.named(b.op("exp", {negDelta}), t + "_q");
  const P ov = b.op("select", {b.op("gt", {curve, b.k(0.0)}), b.op("div", {q, b.op("sub", {b.k(1.0), q})}), b.k(0.0)});
  // 1 − k, accurate for small x = δ/N (float exp near 1 keeps few bits): x(1 − x/2) below 1e-3, else 1 − e^−x
  const P x = b.named(b.op("div", {delta, n}), t + "_x");
  const P series = b.op("mul", {x, b.op("sub", {b.k(1.0), b.op("mul", {x, b.k(0.5)})})});
  const P exact = b.op("sub", {b.k(1.0), b.op("exp", {b.op("neg", {x})})});
  const P omk = b.op("select", {b.op("lt", {x, b.k(1e-3)}), series, exact});
  const P w = b.named(b.op("neg", {b.op("mul", {b.op("mul", {ov, span}), omk})}), t + "_pull");  // −w (see body)
  return {coef, w};
}

Status body(Builder& bld) {
  using P = Builder::PortRef;
  Body b(bld);
  const P in = bld.input(0), gate = bld.input(1);
  const P a = bld.param(kEnvAttack), d = bld.param(kEnvDecay), sus = bld.param(kEnvSustain), r = bld.param(kEnvRelease);
  const P ac = bld.param(kEnvACurve), dc = bld.param(kEnvDCurve), rc = bld.param(kEnvRCurve), amt = bld.param(kEnvAmount);
  const P sr = b.named(b.op("sr", {}), "rate");

  // ---- the attack's law (param rate) ----
  const P aN = b.named(b.op("max", {b.op("mul", {a, sr}), b.k(1.0)}), "a_n");
  const P rateLin = b.op("div", {b.k(1.0), aN});  // msenv: max(|peak − init|, 0.001) / max(a·sr, 1) = 1 / max(a·sr, 1)
  const P acAbs = b.named(b.op("abs", {ac}), "a_bend");
  const P lin = b.named(b.op("lt", {acAbs, b.k(1e-3)}), "a_linear");
  const P aDelta = b.named(b.op("mul", {b.op("max", {acAbs, b.k(1e-3)}), b.k(kEnvDepth)}), "a_depth");
  const P aQ = b.named(b.op("exp", {b.op("neg", {aDelta})}), "a_q");
  const P aOv = b.named(b.op("div", {aQ, b.op("sub", {b.k(1.0), aQ})}), "a_ov");
  const P ax = b.named(b.op("div", {aDelta, aN}), "a_x");
  // m = 1 − e^−x (concave) or 1 − e^x (convex), by their series below x = 0.01 (float exp near 1 keeps few bits)
  const P xh = b.named(b.op("mul", {ax, b.k(0.5)}), "a_xh");
  const P x3 = b.named(b.op("mul", {ax, b.k(1.0 / 3.0)}), "a_x3");
  const P small = b.named(b.op("lt", {ax, b.k(0.01)}), "a_small");
  const P concS = b.op("mul", {ax, b.op("sub", {b.k(1.0), b.op("mul", {xh, b.op("sub", {b.k(1.0), x3})})})});
  const P concE = b.op("sub", {b.k(1.0), b.op("exp", {b.op("neg", {ax})})});
  const P convS = b.op("neg", {b.op("mul", {ax, b.op("add", {b.k(1.0), b.op("mul", {xh, b.op("add", {b.k(1.0), x3})})})})});
  const P convE = b.op("sub", {b.k(1.0), b.op("exp", {ax})});
  const P up = b.named(b.op("gt", {ac, b.k(0.0)}), "a_up");
  const P mA = b.named(b.op("select", {lin, b.k(1.0), b.op("select", {up, b.op("select", {small, concS, concE}),
                                                                         b.op("select", {small, convS, convE})})}),
                       "a_m");
  const P tA = b.named(
      b.op("select", {lin, b.k(1.0), b.op("select", {up, b.op("add", {b.k(1.0), aOv}), b.op("neg", {aOv})})}), "a_target");
  const P rateA = b.named(b.op("select", {lin, rateLin, b.k(1e9)}), "a_rate");

  // ---- the decay's and the release's laws (param rate) ----
  const FallLaw dl = fallLaw(b, "d", d, dc, b.op("sub", {b.k(1.0), sus}), sr);
  const FallLaw rl = fallLaw(b, "r", r, rc, b.k(1.0), sr);

  // ---- per sample: msenv (mode None, soft start, Initial 0, Peak 1, End 0) with the curves ----
  const P start0 = b.named(bld.atom("rise", {gate}, kStart0), "start0");
  const P target = b.named(b.op("select", {gate, sus, b.k(0.0)}), "target");  // held ? sus : end
  const P coef = b.op("select", {gate, dl.coef, rl.coef});                   // held ? dcoef : rcoef
  const P startH = b.op("and", {gate, start0});                              // held && start
  const P lvl = b.named(bld.atom("z1", {P{graph::kNone, 0}}, kLvl), "lvl");  // z1(next)
  const P bb = b.named(b.op("max", {lvl, b.op("select", {start0, b.k(0.0), b.k(-2.0)})}), "b");
  const P zAtt = bld.atom("z1", {P{graph::kNone, 0}}, kZAtt);                // z1(att)
  const P inAtt = b.named(b.op("or", {startH, b.op("and", {gate, zAtt})}), "inAtt");
  const P dist = b.op("sub", {b.k(1.0), bb});  // peak − b
  // The attack only rises (levels stay in [0, 1]: dist ≥ 0, and the step is ≥ 0 on every curve), so msenv's
  // clip(dist, −rate, rate) is min(dist, rate) and its |dist| ≤ rate is dist ≤ step: the same values, fewer ops.
  const P step = b.named(b.op("min", {b.op("mul", {b.op("sub", {tA, bb}), mA}), rateA}), "step");
  const P reach = b.named(b.op("le", {dist, step}), "reach");
  const P aNext = b.op("select", {reach, b.k(1.0), b.op("add", {bb, step})});
  const P e = b.named(b.op("sub", {bb, target}), "e");
  // (target − sign(e)·w) + (b − target)·coef: two fused multiply-adds; with w = 0 the first is target exactly and the
  // second msenv's target + (b − target)·coef (the same single rounding).
  const P negW = b.op("select", {gate, dl.w, rl.w});
  const P pulled = b.op("add", {target, b.op("mul", {b.op("sign", {e}), negW})});
  const P n1 = b.op("add", {pulled, b.op("mul", {e, coef})});
  const P dr = b.op("clip", {n1, b.op("min", {bb, target}), b.op("max", {bb, target})});
  const P next = b.named(b.op("select", {inAtt, aNext, dr}), "next");
  const P att = b.named(b.op("and", {inAtt, b.op("not", {reach})}), "att");
  bld.patch().connect(next, graph::PortRef{lvl.node, 0});
  bld.patch().connect(att, graph::PortRef{zAtt.node, 0});
  bld.done(b.op("and", {b.op("not", {gate}), b.op("lt", {next, b.k(1e-4)})}));
  bld.output(0, b.op("mul", {b.op("mul", {next, amt}), in}));
  // The face's dots: 0.4 · level, + 0.5 once released (EnvelopeTap), into the instance's voice tap.
  const P tapX = b.op("add", {b.op("mul", {next, b.k(0.4)}), b.op("select", {gate, b.k(0.0), b.k(0.5)})});
  b.named(bld.atom("voice_tap", {tapX, bld.slot(0)}, kTap), "tap");
  return Ok{};
}

std::vector<ResourceRequest> resources(std::span<const double>) { return {ResourceRequest{ResourceKind::VoiceTap, 0}}; }

Type makeType() {
  Type t;
  t.id = "envelope";
  t.name = "Envelope";
  t.version = 1;
  t.inputs = kInputs;
  t.params = kParams;
  t.outputs = kOutputs;
  // TMM.10-fix1: the chain feeds `in` (a VCA on the previous stage's signal). At the note source it has no signal for
  // `in` (Reads::Signal): it plays its envelope per note (its level, a DC). No exclusion: what it does follows the wire.
  t.chainInput = 0;
  t.voiceEnd = true;
  t.resources = &resources;
  t.body = &body;
  return t;
}

}  // namespace

const Type& envelopeType() noexcept {
  static const Type t = makeType();
  return t;
}

// ---- the shape ------------------------------------------------------------------------------------------------------

EnvSegmentLaw envAttackLaw(double c) noexcept {
  EnvSegmentLaw l;
  const double a = std::abs(c);
  if (!(a >= 1e-3)) {
    l.linear = true;
    return l;
  }
  l.delta = kEnvDepth * std::min(a, 1.0);
  const double q = std::exp(-l.delta);
  l.ov = q / (1.0 - q);
  l.convex = c < 0.0;
  return l;
}

EnvSegmentLaw envFallLaw(double c) noexcept {
  EnvSegmentLaw l;
  c = std::clamp(std::isfinite(c) ? c : 0.0, -1.0, 1.0);
  l.delta = kEnvDepth * std::max(1.0 - c * (c < 0.0 ? 2.0 : 1.0), 0.01);
  const double q = std::exp(-l.delta);
  l.ov = c > 0.0 ? q / (1.0 - q) : 0.0;
  return l;
}

double envAttackLevel(const EnvSegmentLaw& l, double u) noexcept {
  u = std::clamp(u, 0.0, 1.0);
  if (l.linear) return u;
  const double v = l.convex ? -l.ov + l.ov * std::exp(l.delta * u) : (1.0 + l.ov) * (1.0 - std::exp(-l.delta * u));
  return std::clamp(v, 0.0, 1.0);
}

double envAttackTime(const EnvSegmentLaw& l, double level) noexcept {
  level = std::clamp(level, 0.0, 1.0);
  if (l.linear) return level;
  const double u = l.convex ? std::log((level + l.ov) / l.ov) / l.delta : -std::log(1.0 - level / (1.0 + l.ov)) / l.delta;
  return std::clamp(u, 0.0, 1.0);
}

namespace {
// The fall's overshoot target and direction (+1: falling toward goal from above).
double fallTarget(const EnvSegmentLaw& l, double from, double goal, double span, double& sgn) noexcept {
  sgn = from >= goal ? 1.0 : -1.0;
  return goal - sgn * l.ov * span;
}
}  // namespace

double envFallLevel(const EnvSegmentLaw& l, double from, double goal, double span, double u) noexcept {
  double sgn = 1.0;
  const double t = fallTarget(l, from, goal, span, sgn);
  const double v = t + (from - t) * std::exp(-l.delta * std::max(u, 0.0));
  return sgn > 0.0 ? std::max(v, goal) : std::min(v, goal);
}

double envFallTime(const EnvSegmentLaw& l, double from, double goal, double span, double level) noexcept {
  double sgn = 1.0;
  const double t = fallTarget(l, from, goal, span, sgn);
  const double whole = std::abs(from - t);
  if (!(whole > 0.0) || !(l.delta > 0.0)) return 0.0;
  // When it arrives: exactly with an overshoot, else at −80 dB of the distance (msenv's "time").
  const double arrive = l.ov * span > 0.0 ? std::log(whole / (l.ov * span)) / l.delta : kEnvDepth / l.delta;
  if ((level - from) * sgn >= 0.0) return 0.0;
  if ((level - goal) * sgn <= 0.0) return arrive;
  const double rest = std::abs(level - t);
  return std::clamp(std::log(whole / rest) / l.delta, 0.0, arrive);
}

EnvelopeTap decodeEnvelopeTap(double v) noexcept {
  EnvelopeTap t;
  if (!std::isfinite(v)) return t;
  t.released = v >= 0.45;
  t.level = std::clamp((t.released ? v - 0.5 : v) / 0.4, 0.0, 1.0);
  return t;
}

double encodeEnvelopeTap(double level, bool released) noexcept {
  return 0.4 * std::clamp(level, 0.0, 1.0) + (released ? 0.5 : 0.0);
}

}  // namespace pq::meta
