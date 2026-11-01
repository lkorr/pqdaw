// Meta-module expansion (TMM.3): pq/meta/type.h.
#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

#include "pq/graph/atoms.h"
#include "pq/meta/type.h"

namespace pq::meta {

namespace {

using graph::NodeIndex;
using graph::PortRef;

Error expandError(const Type& t, std::string m) {
  return Error{ErrorCode::InvalidArgument, "meta module '" + std::string(t.id) + "': " + std::move(m), {}};
}

constexpr graph::PortType kAudio = graph::portType(graph::Kind::Real, graph::Rate::Audio);

graph::ParamDecl paramDecl(const ParamSpec& s) {
  graph::ParamDecl d;
  d.kind = graph::Kind::Real;
  d.defaultValue = graph::Constant::real(s.def, s.unit);
  d.hasRange = true;
  d.rangeMin = graph::Constant::real(s.min, s.unit);
  d.rangeMax = graph::Constant::real(s.max, s.unit);
  if (!s.label.empty()) d.attrs.push_back(graph::Attr{"label", {graph::AttrArg{{}, graph::AttrValue::string(std::string(s.label))}}});
  if (s.curve == Curve::Exp) {
    d.attrs.push_back(graph::Attr{"curve", {graph::AttrArg{{}, graph::AttrValue::ident("exp")}}});
  } else if (s.curve == Curve::Exp0) {
    d.attrs.push_back(graph::Attr{"curve", {graph::AttrArg{{}, graph::AttrValue::ident("exp0")},
                                            graph::AttrArg{{}, graph::AttrValue::num(s.curveK)}}});
  }
  return d;
}

}  // namespace

double travelToValue(const ParamSpec& p, double t) noexcept {
  t = std::clamp(t, 0.0, 1.0);
  switch (p.curve) {
    case Curve::Exp:
      if (p.min > 0.0 && p.max > p.min) return std::clamp(p.min * std::pow(p.max / p.min, t), p.min, p.max);
      break;
    case Curve::Exp0:
      if (p.curveK > 0.0)
        return std::clamp(p.min + (p.max - p.min) * std::expm1(t * p.curveK * std::numbers::ln2) /
                                      std::expm1(p.curveK * std::numbers::ln2),
                          p.min, p.max);
      break;
    case Curve::Linear: break;
  }
  return p.min + t * (p.max - p.min);
}

double valueToTravel(const ParamSpec& p, double v) noexcept {
  if (!(p.max > p.min)) return 0.0;
  v = std::clamp(v, p.min, p.max);
  switch (p.curve) {
    case Curve::Exp:
      if (p.min > 0.0) return std::clamp(std::log(v / p.min) / std::log(p.max / p.min), 0.0, 1.0);
      break;
    case Curve::Exp0:
      if (p.curveK > 0.0) {
        const double u = (v - p.min) / (p.max - p.min);  // [0, 1]
        return std::clamp(std::log1p(u * std::expm1(p.curveK * std::numbers::ln2)) / (p.curveK * std::numbers::ln2), 0.0,
                          1.0);
      }
      break;
    case Curve::Linear: break;
  }
  return (v - p.min) / (p.max - p.min);
}

int32_t Type::paramIndex(std::string_view n) const noexcept {
  for (size_t i = 0; i < params.size(); ++i)
    if (params[i].name == n) return static_cast<int32_t>(i);
  return -1;
}
int32_t Type::settingIndex(std::string_view n) const noexcept {
  for (size_t i = 0; i < settings.size(); ++i)
    if (settings[i].name == n) return static_cast<int32_t>(i);
  return -1;
}
int32_t Type::inputIndex(std::string_view n) const noexcept {
  for (size_t i = 0; i < inputs.size(); ++i)
    if (inputs[i].name == n) return static_cast<int32_t>(i);
  return -1;
}

// ---- Builder -------------------------------------------------------------------------------------------------------

NodeIndex Builder::take(NodeIndex n, uint32_t id) {
  ids_.push_back(id);
  return n;
}

bool Builder::modulated(size_t i) const noexcept { return i < ctx_->modulated.size() && ctx_->modulated[i] != 0; }

PortRef Builder::slot(uint32_t index) {
  const uint32_t id = kSlotIdBase + index;
  for (size_t n = 0; n < ids_.size(); ++n)
    if (ids_[n] == id) return PortRef{static_cast<NodeIndex>(n), 0};
  if (index >= kBodyIdBase - kSlotIdBase) {
    failed_ = true;
    error_ = "resource slot index " + std::to_string(index) + " out of range";
    return PortRef{};
  }
  const double v = index < ctx_->slots.size() ? static_cast<double>(ctx_->slots[index]) : 0.0;
  return PortRef{take(p_.addConst(graph::Constant::real(v)), id), 0};
}

PortRef Builder::atom(std::string_view atomName, std::initializer_list<PortRef> ins, uint32_t k, uint16_t repeat) {
  if (k >= kMaxBodyIds) {
    failed_ = true;
    error_ = "body node id " + std::to_string(k) + " out of range";
    return PortRef{};
  }
  const std::optional<graph::AtomId> a = graph::AtomRegistry::builtin().find(atomName);
  if (!a) {
    failed_ = true;
    error_ = "no atom '" + std::string(atomName) + "'";
    return PortRef{};
  }
  const NodeIndex n = take(p_.addAtom(*a, repeat), kBodyIdBase + k);
  uint16_t i = 0;
  for (const PortRef& in : ins) {
    if (in.node != graph::kNone) p_.connect(in, PortRef{n, i});
    ++i;
  }
  return PortRef{n, 0};
}

PortRef Builder::konst(double v, uint32_t k) {
  if (k >= kMaxBodyIds) {
    failed_ = true;
    error_ = "body node id " + std::to_string(k) + " out of range";
    return PortRef{};
  }
  return PortRef{take(p_.addConst(graph::Constant::real(v)), kBodyIdBase + k), 0};
}

void Builder::bind(PortRef value, std::string_view name) {
  if (value.node == graph::kNone || value.node >= p_.nodeCount()) return;
  p_.setNodeName(value.node, std::string(name));
}

uint16_t Builder::region(uint32_t factor, std::initializer_list<std::string_view> phase) {
  graph::OversampleRegion r;
  r.factor = factor;
  for (const std::string_view n : phase) r.phase.emplace_back(n);
  return p_.addRegion(std::move(r));
}

void Builder::inRegion(PortRef node, uint16_t region) {
  if (node.node == graph::kNone || node.node >= p_.nodeCount() || region == 0) return;
  p_.setNodeRegion(node.node, region);
}

void Builder::output(size_t i, PortRef value) {
  if (i >= outputs_.size() || value.node == graph::kNone) {
    failed_ = true;
    error_ = "output " + std::to_string(i) + " is not an output of the type";
    return;
  }
  outputSet_[i] = 1;
  outputs_[i] = value.node;
  outPorts_[i] = value.port;
}

// ---- expand --------------------------------------------------------------------------------------------------------

Result<Expansion> expand(const Type& type, const ExpandContext& ctx, std::string name) {
  if (type.body == nullptr) return expandError(type, "the type has no body");
  if (type.outputs.empty()) return expandError(type, "the type has no output");
  if (ctx.form == Form::ChainInstrument && !type.chainInstrument)
    return expandError(type, "it cannot be an instrument");
  if (ctx.form == Form::ChainEffect &&
      (type.chainEffectInput < 0 || static_cast<size_t>(type.chainEffectInput) >= type.inputs.size()))
    return expandError(type, "it cannot be an audio effect");
  if (ctx.form == Form::ChainVoice &&
      (type.chainVoiceInput < 0 || static_cast<size_t>(type.chainVoiceInput) >= type.inputs.size()))
    return expandError(type, "it cannot continue a voice");
  Builder b(type, ctx, std::move(name));
  graph::Patch& p = b.p_;
  b.settings_.resize(type.settings.size());
  for (size_t i = 0; i < type.settings.size(); ++i) {
    const SettingSpec& s = type.settings[i];
    const double v = i < ctx.settings.size() && std::isfinite(ctx.settings[i]) ? ctx.settings[i] : s.def;
    b.settings_[i] = std::clamp(v, std::min(s.min, s.max), std::max(s.min, s.max));
  }
  // Inputs: an `in` with its default (Nested), or the default alone (chain forms), the effect's audio `in`.
  b.inputs_.resize(type.inputs.size());
  for (size_t i = 0; i < type.inputs.size(); ++i) {
    const InputSpec& in = type.inputs[i];
    const bool audioIn = ctx.form == Form::ChainEffect && static_cast<int32_t>(i) == type.chainEffectInput;
    const bool voiceIn = ctx.form == Form::ChainVoice && static_cast<int32_t>(i) == type.chainVoiceInput;
    if (audioIn || voiceIn) {
      const NodeIndex n = b.take(p.addInput(std::string(in.name), graph::portType(graph::Kind::Real, graph::Rate::Audio,
                                                                                  voiceIn ? graph::Poly::Voice
                                                                                          : graph::Poly::Global)),
                                 kInputIdBase + static_cast<uint32_t>(i));
      b.inputs_[i] = PortRef{n, 0};
      continue;
    }
    NodeIndex def;
    if (in.def == InputDefault::VoiceHz && ctx.voice) {
      def = b.take(p.addAtom(graph::atomId("voice.hz")), kDefaultIdBase + static_cast<uint32_t>(i));
    } else if (in.def == InputDefault::VoicePhase && ctx.voice) {  // TMM.5: phasor(voice.hz)
      const NodeIndex hz = b.take(p.addAtom(graph::atomId("voice.hz")), kFixedIdBase + 32 + static_cast<uint32_t>(i));
      def = b.take(p.addAtom(graph::atomId("phasor")), kDefaultIdBase + static_cast<uint32_t>(i));
      p.connect(hz, def, 0);
    } else
      def = b.take(p.addConst(graph::Constant::real(-0.0)), kDefaultIdBase + static_cast<uint32_t>(i));
    if (ctx.form != Form::Nested) {
      b.inputs_[i] = PortRef{def, 0};
      continue;
    }
    const NodeIndex n = b.take(p.addInput(std::string(in.name), kAudio), kInputIdBase + static_cast<uint32_t>(i));
    p.connect(PortRef{def, 0}, PortRef{n, 0});
    b.inputs_[i] = PortRef{n, 0};
  }
  b.params_.resize(type.params.size());
  for (size_t i = 0; i < type.params.size(); ++i)
    b.params_[i] = PortRef{b.take(p.addParam(std::string(type.params[i].name), paramDecl(type.params[i])),
                                  kParamIdBase + static_cast<uint32_t>(i)),
                           0};
  b.outputs_.assign(type.outputs.size(), graph::kNone);
  b.outPorts_.assign(type.outputs.size(), 0);
  b.outputSet_.assign(type.outputs.size(), 0);
  if (Status s = type.body(b); !s) return expandError(type, s.error().message);
  if (b.failed_) return expandError(type, b.error_);
  const graph::Poly poly = ctx.form == Form::ChainInstrument || ctx.form == Form::ChainVoice ? graph::Poly::Voice
                           : ctx.form == Form::ChainEffect   ? graph::Poly::Global
                                                             : graph::Poly::Infer;
  for (size_t i = 0; i < type.outputs.size(); ++i) {
    if (!b.outputSet_[i]) return expandError(type, "output '" + std::string(type.outputs[i].name) + "' is not set");
    const NodeIndex o = b.take(p.addOutput(std::string(type.outputs[i].name),
                                           graph::portType(graph::Kind::Real, graph::Rate::Audio, poly,
                                                           type.outputs[i].width)),
                               kOutputIdBase + static_cast<uint32_t>(i));
    p.connect(PortRef{b.outputs_[i], b.outPorts_[i]}, PortRef{o, 0});
  }
  if (ctx.form == Form::ChainInstrument) {  // the voice ends at note-off
    const NodeIndex gate = b.take(p.addAtom(graph::atomId("voice.gate")), kFixedIdBase);
    const NodeIndex inv = b.take(p.addAtom(graph::atomId("not")), kFixedIdBase + 1);
    p.connect(gate, inv, 0);
    const NodeIndex done = b.take(p.addOutput("done", graph::portType(graph::Kind::Bool, graph::Rate::Infer,
                                                                      graph::Poly::Voice, graph::kWidthMono)),
                                  kOutputIdBase + static_cast<uint32_t>(type.outputs.size()));
    p.connect(inv, done, 0);
  }
  std::vector<graph::NodeId> ids;
  ids.reserve(b.ids_.size());
  for (const uint32_t v : b.ids_) ids.push_back(graph::NodeId{v});
  if (ids.size() != p.nodeCount() || !p.setNodeIds(ids)) return expandError(type, "two nodes have one id");
  if (Status s = p.validate(); !s) return expandError(type, s.error().message);
  Expansion e;
  if (type.resources != nullptr) e.resources = type.resources(b.settings_);
  e.key = expansionKey(type, ctx);
  e.patch = std::move(p);
  return e;
}

uint64_t expansionKey(const Type& type, const ExpandContext& ctx) noexcept {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) {
    for (int i = 0; i < 8; ++i, v >>= 8) h = (h ^ (v & 0xFF)) * 1099511628211ull;
  };
  for (const char c : type.id) mix(static_cast<unsigned char>(c));
  mix(0xFF);
  mix(type.version);
  mix(static_cast<uint64_t>(ctx.form));
  mix(ctx.voice ? 1 : 0);
  for (size_t i = 0; i < type.settings.size(); ++i) {
    const SettingSpec& s = type.settings[i];
    const double v = i < ctx.settings.size() && std::isfinite(ctx.settings[i]) ? ctx.settings[i] : s.def;
    mix(std::bit_cast<uint64_t>(std::clamp(v, std::min(s.min, s.max), std::max(s.min, s.max)) + 0.0));
  }
  for (size_t i = 0; i < type.params.size(); ++i) mix(i < ctx.modulated.size() && ctx.modulated[i] != 0 ? 1 : 0);
  mix(ctx.slots.size());
  for (const uint32_t s : ctx.slots) mix(s);
  return h;
}

}  // namespace pq::meta
