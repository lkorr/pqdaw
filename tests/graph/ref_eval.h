#pragma once

// A slow reference evaluator for TypedPatches (tests only): a sample-by-sample walk of the graph in double, one
// voice, mono signals. Used to check that the optimiser (T1.4) preserves semantics: evaluate a patch before and
// after optimise() and compare every output. Atoms it does not know (delay lines, timers, tables, solve blocks,
// multichannel signals, transport) make supported() false; the caller then skips the numerical comparison.

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "pq/graph/infer.h"

namespace ref_eval {

using namespace pq::graph;

class Evaluator {
public:
  explicit Evaluator(const TypedPatch& tp, double sampleRate = 48000.0) : tp_(tp), p_(tp.patch), sr_(sampleRate) {
    const uint32_t n = p_.nodeCount();
    adj_ = p_.adjacency();
    val_.assign(p_.portCount(), 0.0);
    state_.assign(n, 0.0);
    state2_.assign(n, 0.0);
    name_.resize(n);
    for (NodeIndex v = 0; v < n; ++v) {
      if (p_.node(v).kind == NodeKind::Atom) name_[v] = atomSignature(p_.node(v).atom).name;
      for (uint32_t j = 0; j < p_.node(v).numOutputs; ++j)
        if (tp.outputType(v, j).width > 1) ok_ = false;
      const NodeKind k = p_.node(v).kind;
      if (k == NodeKind::Unknown || k == NodeKind::Equation || k == NodeKind::Subcircuit) ok_ = false;
      if (k == NodeKind::Atom && !known(name_[v])) ok_ = false;
      if (k == NodeKind::Atom && name_[v] == "z1") {
        double init = 0.0;
        if (const uint32_t d = driverNode(v, 1); d != kNone && p_.node(d).kind == NodeKind::Const)
          init = static_cast<float>(p_.constant(d).value());
        state_[v] = init;
      }
    }
    order();
  }

  bool supported() const { return ok_; }

  // Value of a root `in` at sample t (by name); default: a slow sine per input, bools a square.
  void setInput(std::string name, double (*f)(uint64_t)) { inputs_[std::move(name)] = f; }
  void setParam(std::string name, double v) { params_[std::move(name)] = v; }

  // Evaluates one sample; outputs() then holds every `out`/`export` value by name.
  void step(uint64_t t) {
    for (NodeIndex v : order_) evalNode(v, t);
    for (NodeIndex v : order_) update(v);
    outs_.clear();
    for (NodeIndex v = 0; v < p_.nodeCount(); ++v) {
      const NodeKind k = p_.node(v).kind;
      if (k == NodeKind::Output || k == NodeKind::Export) outs_[p_.nodeName(v)] = in(v, 0);
    }
  }
  const std::map<std::string, double>& outputs() const { return outs_; }

private:
  static bool known(std::string_view a) {
    static const char* names[] = {
        "add", "sub", "mul", "div", "mod", "neg", "min", "max", "clip", "abs", "sign", "floor", "frac", "tan", "tanh",
        "exp", "log", "pow", "sqrt", "sin", "fast_tan", "fast_tanh", "fast_exp", "fast_log", "fast_pow", "fast_sqrt",
        "fast_sin", "muladd", "cents_to_hz", "hz_to_cents", "db_to_gain", "gain_to_db", "fast_cents_to_hz",
        "fast_hz_to_cents", "fast_db_to_gain", "fast_gain_to_db", "select", "lerp", "gate_to_real", "lt", "le", "gt",
        "ge", "eq", "ne", "and", "or", "xor", "nand", "nor", "not", "mux", "channel", "z1", "phasor", "sine",
        "sine_hz", "rise", "fall", "change", "srff", "samplehold", "latch", "sr", "voice.hz", "voice.gate",
        "voice.trigger", "voice.velocity", "voice_sum", "voice_last", "voice_highest", "voice_lowest", "voice_loudest"};
    for (const char* n : names)
      if (a == n) return true;
    return false;
  }

  uint32_t driverNode(NodeIndex v, uint32_t i) const {
    if (i >= p_.node(v).numInputs) return kNone;
    const WireIndex w = adj_.driver[p_.flatInput(v, i)];
    return w == kNone ? kNone : p_.wire(w).from.node;
  }

  bool delayedInput(NodeIndex v, uint32_t i) const {
    if (p_.node(v).kind != NodeKind::Atom) return false;
    const AtomSignature& sig = atomSignature(p_.node(v).atom);
    return i < sig.inputs.size() && sig.inputs[i].delayed;
  }

  void order() {
    const uint32_t n = p_.nodeCount();
    std::vector<uint8_t> state(n, 0);
    for (NodeIndex s = 0; s < n && ok_; ++s) {
      if (state[s]) continue;
      std::vector<std::pair<NodeIndex, uint32_t>> stack{{s, 0}};
      state[s] = 1;
      while (!stack.empty() && ok_) {
        auto& [v, i] = stack.back();
        if (i < p_.node(v).numInputs) {
          const uint32_t k = i++;
          if (delayedInput(v, k)) continue;
          const uint32_t d = driverNode(v, k);
          if (d == kNone) continue;
          if (state[d] == 1) ok_ = false;  // a delay-free cycle
          if (state[d] == 0) {
            state[d] = 1;
            stack.emplace_back(d, 0);
          }
          continue;
        }
        state[v] = 2;
        order_.push_back(v);
        stack.pop_back();
      }
    }
  }

  // Value of input i (the driver's output, or the input's default / 0).
  double in(NodeIndex v, uint32_t i) const {
    if (i >= p_.node(v).numInputs) return 0.0;
    const WireIndex w = adj_.driver[p_.flatInput(v, i)];
    if (w != kNone) return val_[p_.flatOutput(p_.wire(w).from.node, p_.wire(w).from.port)];
    if (p_.node(v).kind == NodeKind::Atom) {
      const AtomSignature& sig = atomSignature(p_.node(v).atom);
      const PortSpec& ps = sig.inputs[i < sig.inputs.size() ? i : sig.inputs.size() - 1];
      return ps.defaultValue;
    }
    return 0.0;
  }

  void set(NodeIndex v, double x, uint32_t j = 0) { val_[p_.flatOutput(v, j)] = x; }

  void evalNode(NodeIndex v, uint64_t t) {
    const Node& nd = p_.node(v);
    switch (nd.kind) {
      // Literals and params reach the program as float32 (1e308 is inf there), so they are rounded once; the
      // arithmetic itself is double.
      case NodeKind::Const: set(v, static_cast<float>(p_.constant(v).value())); return;
      case NodeKind::Param: {
        const auto it = params_.find(p_.nodeName(v));
        set(v, static_cast<float>(it != params_.end() ? it->second : p_.paramDecl(v).defaultValue.value()));
        return;
      }
      case NodeKind::Input: {
        const auto it = inputs_.find(p_.nodeName(v));
        if (it != inputs_.end()) set(v, it->second(t));
        else if (tp_.outputType(v, 0).kind == Kind::Bool) set(v, (t / 7) % 2 ? 1.0 : 0.0);
        else set(v, 0.8 * std::sin(0.05 * static_cast<double>(t) * static_cast<double>(v + 1)));
        return;
      }
      case NodeKind::Atom: evalAtom(v, t); return;
      default: return;
    }
  }

  static double b(bool x) { return x ? 1.0 : 0.0; }

  void evalAtom(NodeIndex v, uint64_t t) {
    const std::string_view a = name_[v];
    const double x = in(v, 0), y = in(v, 1), z = in(v, 2);
    const double pi = 3.14159265358979323846;
    double r = 0.0;
    if (a == "add") r = x + y;
    else if (a == "sub") r = x - y;
    else if (a == "mul") r = x * y;
    else if (a == "div") r = x / y;
    else if (a == "mod") r = x - y * std::floor(x / y);
    else if (a == "neg") r = -x;
    else if (a == "min") r = x < y ? x : y;
    else if (a == "max") r = x > y ? x : y;
    else if (a == "clip") { const double m = x > y ? x : y; r = m < z ? m : z; }
    else if (a == "abs") r = std::fabs(x);
    else if (a == "sign") r = x > 0 ? 1.0 : x < 0 ? -1.0 : 0.0;
    else if (a == "floor") r = std::floor(x);
    else if (a == "frac") r = x - std::floor(x);
    else if (a == "tan" || a == "fast_tan") r = std::tan(x);
    else if (a == "tanh" || a == "fast_tanh") r = std::tanh(x);
    else if (a == "exp" || a == "fast_exp") r = std::exp(x);
    else if (a == "log" || a == "fast_log") r = std::log(x);
    else if (a == "pow" || a == "fast_pow") r = std::pow(x, y);
    else if (a == "sqrt" || a == "fast_sqrt") r = std::sqrt(x);
    else if (a == "sin" || a == "fast_sin") r = std::sin(x);
    else if (a == "muladd") r = std::fma(x, y, z);
    else if (a == "cents_to_hz" || a == "fast_cents_to_hz") r = y * std::exp2(x / 1200.0);
    else if (a == "hz_to_cents" || a == "fast_hz_to_cents") r = 1200.0 * std::log2(x / y);
    else if (a == "db_to_gain" || a == "fast_db_to_gain") r = std::pow(10.0, x / 20.0);
    else if (a == "gain_to_db" || a == "fast_gain_to_db") r = 20.0 * std::log10(x);
    else if (a == "select") r = x != 0.0 ? y : z;
    else if (a == "lerp") r = x + z * (y - x);
    else if (a == "gate_to_real") r = x != 0.0 ? 1.0 : 0.0;
    else if (a == "lt") r = b(x < y);
    else if (a == "le") r = b(x <= y);
    else if (a == "gt") r = b(x > y);
    else if (a == "ge") r = b(x >= y);
    else if (a == "eq") r = b(x == y);
    else if (a == "ne") r = b(x != y);
    else if (a == "and") r = b(x != 0 && y != 0);
    else if (a == "or") r = b(x != 0 || y != 0);
    else if (a == "xor") r = b((x != 0) != (y != 0));
    else if (a == "nand") r = b(!(x != 0 && y != 0));
    else if (a == "nor") r = b(!(x != 0 || y != 0));
    else if (a == "not") r = b(x == 0);
    else if (a == "mux") {
      const uint32_t n = p_.node(v).numInputs - 1u;
      double k = std::isnan(x) ? 0.0 : std::floor(x);
      k = k < 0 ? 0 : k > n - 1 ? n - 1 : k;
      r = in(v, 1 + static_cast<uint32_t>(k));
    } else if (a == "channel") r = x;
    else if (a == "z1") r = state_[v];
    else if (a == "phasor" || a == "sine_hz") {
      if (y != 0.0) state_[v] = 0.0;
      r = a == "phasor" ? state_[v] : std::sin(2 * pi * state_[v]);
    } else if (a == "sine") r = std::sin(2 * pi * x);
    else if (a == "rise") r = b(x != 0 && state_[v] == 0);
    else if (a == "fall") r = b(x == 0 && state_[v] != 0);
    else if (a == "change") r = b((x != 0) != (state_[v] != 0));
    else if (a == "srff") { state2_[v] = y != 0 ? 0.0 : x != 0 ? 1.0 : state2_[v]; r = state2_[v]; }
    else if (a == "samplehold") { if (y != 0 && state_[v] == 0) state2_[v] = x; r = state2_[v]; }
    else if (a == "latch") { if (y != 0) state2_[v] = x; r = state2_[v]; }
    else if (a == "sr") r = sr_;
    else if (a == "voice.hz") r = 220.0;
    else if (a == "voice.gate") r = b(t % 40 < 25);
    else if (a == "voice.trigger") r = b(t % 40 == 0);
    else if (a == "voice.velocity") r = 0.8;
    else if (a.starts_with("voice_")) r = x;  // one voice
    set(v, r);
  }

  // End of the sample: state that the next sample reads.
  void update(NodeIndex v) {
    const std::string_view a = name_[v];
    if (a == "z1") state_[v] = in(v, 0);
    else if (a == "phasor" || a == "sine_hz") {
      double ph = in(v, 1) != 0.0 ? 0.0 : state_[v] + in(v, 0) / sr_;
      ph -= std::floor(ph);
      state_[v] = ph;
    } else if (a == "rise" || a == "fall" || a == "change" || a == "samplehold") {
      state_[v] = a == "samplehold" ? in(v, 1) : in(v, 0);
    }
  }

  const TypedPatch& tp_;
  const Patch& p_;
  double sr_;
  Adjacency adj_;
  bool ok_ = true;
  std::vector<std::string_view> name_;
  std::vector<NodeIndex> order_;
  std::vector<double> val_, state_, state2_;
  std::map<std::string, double (*)(uint64_t)> inputs_;
  std::map<std::string, double> params_;
  std::map<std::string, double> outs_;
};

// Runs both patches for `samples` samples; returns the largest relative difference over every output both have
// (NaN in exactly one of them counts as a mismatch: returns infinity). Outputs are matched by name.
inline double maxRelDiff(const TypedPatch& a, const TypedPatch& b, uint64_t samples, double sampleRate = 48000.0) {
  Evaluator ea(a, sampleRate), eb(b, sampleRate);
  double worst = 0.0;
  for (uint64_t t = 0; t < samples; ++t) {
    ea.step(t);
    eb.step(t);
    for (const auto& [name, va] : ea.outputs()) {
      const auto it = eb.outputs().find(name);
      if (it == eb.outputs().end()) return INFINITY;
      const double vb = it->second;
      if (std::isnan(va) || std::isnan(vb)) {
        if (std::isnan(va) != std::isnan(vb)) return INFINITY;
        continue;
      }
      if (std::isinf(va) || std::isinf(vb)) {
        if (va != vb) return INFINITY;
        continue;
      }
      const double d = std::fabs(va - vb) / std::max(1.0, std::fabs(va));
      if (d > worst) worst = d;
    }
  }
  return worst;
}

}  // namespace ref_eval
