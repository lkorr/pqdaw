#pragma once

// Tests only (T1.5): two ways to run a global, mono patch with the real dsp kernels, to check that a Schedule
// computes what the patch means.
//   Naive     — the oracle: one sample at a time, every node once per sample in a topological order of the patch,
//               every z1/delay split (read at its place, write after the sample), fused kernels with frames = 1.
//               No buffer reuse, no regions, no sub-blocks.
//   Executor  — runs a Schedule the way schedule.h says a backend must: Control regions once per block on 8-float
//               patterns, Block regions block-wise, Loop regions in sub-blocks, physical buffers shared as the
//               scheduler assigned them, the state arena laid out by StateLayout.
// Both are deliberately simple (not RT code). Unsupported: voices, solve blocks, multichannel, resources, noise.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <malloc.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pq/dsp/atoms.h"
#include "pq/graph/kernel_map.h"
#include "pq/graph/schedule.h"

namespace sched_exec {

using namespace pq::graph;
namespace dsp = pq::dsp;

inline double inputSignal(uint32_t ordinal, uint64_t t) {
  return 0.7 * std::sin(0.031 * static_cast<double>(t) * (ordinal + 1)) + 0.2 * std::sin(0.0047 * static_cast<double>(t));
}

struct AlignedBlock {
  explicit AlignedBlock(size_t bytes) : p(static_cast<char*>(_aligned_malloc(std::max<size_t>(bytes, 64), 64))) {
    std::fill(p, p + std::max<size_t>(bytes, 64), char{0});
  }
  ~AlignedBlock() { _aligned_free(p); }
  AlignedBlock(const AlignedBlock&) = delete;
  AlignedBlock& operator=(const AlignedBlock&) = delete;
  char* p;
};

// Does the patch only use what both runners support?
inline bool supported(const TypedPatch& tp) {
  const Patch& p = tp.patch;
  for (const PortType& t : tp.types)
    if (t.width > 1 || t.poly == Poly::Voice) return false;
  for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
    const Node& nd = p.node(n);
    if (nd.kind == NodeKind::Unknown || nd.kind == NodeKind::Equation || nd.kind == NodeKind::Subcircuit) return false;
    if (nd.kind != NodeKind::Atom) continue;
    const KernelMapping& m = kernelMapping(nd.atom);
    if (m.lowering == Lowering::Constant) continue;
    if (m.lowering != Lowering::Kernel) return false;
    if (dsp::kernelInfo(m.kernel).resource != dsp::Resource::None || m.kernel == dsp::Atom::Noise) return false;
  }
  return true;
}

inline float constValue(const Patch& p, NodeIndex n, uint32_t i, double sr) {
  const Adjacency adj = p.adjacency();
  const WireIndex w = adj.driver[p.flatInput(n, i)];
  if (w == kNone) {
    const AtomSignature& sig = atomSignature(p.node(n).atom);
    return sig.inputs[std::min<size_t>(i, sig.inputs.size() - 1)].defaultValue;
  }
  const NodeIndex d = p.wire(w).from.node;
  if (p.node(d).kind == NodeKind::Const) return static_cast<float>(p.constant(d).value());
  return static_cast<float>(sr);
}

// ---- the oracle ----------------------------------------------------------------------------------------------
class Naive {
public:
  Naive(const TypedPatch& tp, double sr) : tp_(tp), p_(tp.patch), sr_(sr) {
    const uint32_t n = p_.nodeCount();
    adj_ = p_.adjacency();
    val_.assign(p_.portCount(), 0.0f);
    for (NodeIndex v = 0; v < n; ++v) {
      const Node& nd = p_.node(v);
      if (nd.kind == NodeKind::Input) inputOrdinal_[v] = static_cast<uint32_t>(inputOrdinal_.size());
      if (nd.kind != NodeKind::Atom || kernelMapping(nd.atom).lowering != Lowering::Kernel) continue;
      const dsp::Atom k = kernelMapping(nd.atom).kernel;
      if (dsp::kernelInfo(k).stateSize == 0) continue;
      dsp::StateInit init;
      init.sampleRate = sr_;
      init.lanes = 1;
      if (k == dsp::Atom::DelayLinear || k == dsp::Atom::DelayCubic || k == dsp::Atom::DelayAllpass) {
        const WireIndex w = adj_.driver[p_.flatInput(v, 2)];
        const NodeIndex d = p_.wire(w).from.node;
        const Constant& c = p_.constant(d);
        init.maxDelayFrames = static_cast<uint32_t>(c.unit == Unit::smp ? std::ceil(c.value()) : std::ceil(c.value() * sr_));
      }
      if (k == dsp::Atom::Z1) init.z1Init = constValue(p_, v, 1, sr_);
      state_[v] = std::make_unique<AlignedBlock>(dsp::stateBytes(k, init));
      dsp::initState(k, state_[v]->p, init);
    }
    order();
  }
  bool ok() const { return ok_; }

  // One sample; returns the outputs in node order.
  std::vector<float> step(uint64_t t) {
    for (NodeIndex v : order_) eval(v, t);
    for (NodeIndex v : order_) write(v);
    std::vector<float> out;
    for (NodeIndex v = 0; v < p_.nodeCount(); ++v)
      if (p_.node(v).kind == NodeKind::Output || p_.node(v).kind == NodeKind::Export) out.push_back(in(v, 0));
    return out;
  }

private:
  bool split(NodeIndex v, uint32_t i) const {
    const Node& nd = p_.node(v);
    if (nd.kind != NodeKind::Atom || i != 0) return false;
    const KernelMapping& m = kernelMapping(nd.atom);
    return m.lowering == Lowering::Kernel &&
           (m.kernel == dsp::Atom::Z1 || m.kernel == dsp::Atom::DelayLinear || m.kernel == dsp::Atom::DelayCubic ||
            m.kernel == dsp::Atom::DelayAllpass);
  }
  void order() {
    const uint32_t n = p_.nodeCount();
    std::vector<uint8_t> st(n, 0);
    for (NodeIndex s = 0; s < n && ok_; ++s) {
      if (st[s]) continue;
      std::vector<std::pair<NodeIndex, uint32_t>> stack{{s, 0}};
      st[s] = 1;
      while (!stack.empty() && ok_) {
        auto& [v, i] = stack.back();
        if (i < p_.node(v).numInputs) {
          const uint32_t k = i++;
          if (split(v, k)) continue;
          const WireIndex w = adj_.driver[p_.flatInput(v, k)];
          if (w == kNone) continue;
          const NodeIndex d = p_.wire(w).from.node;
          if (st[d] == 1) ok_ = false;
          if (st[d] == 0) {
            st[d] = 1;
            stack.emplace_back(d, 0);
          }
          continue;
        }
        st[v] = 2;
        order_.push_back(v);
        stack.pop_back();
      }
    }
  }
  float in(NodeIndex v, uint32_t i) const {
    const WireIndex w = adj_.driver[p_.flatInput(v, i)];
    if (w != kNone) return val_[p_.flatOutput(p_.wire(w).from.node, p_.wire(w).from.port)];
    if (p_.node(v).kind == NodeKind::Atom) {
      const AtomSignature& sig = atomSignature(p_.node(v).atom);
      const float d = sig.inputs[std::min<size_t>(i, sig.inputs.size() - 1)].defaultValue;
      return tp_.inputType(v, i).kind == Kind::Bool ? dsp::boolMask(d != 0.0f) : d;
    }
    return 0.0f;
  }
  void eval(NodeIndex v, uint64_t t) {
    const Node& nd = p_.node(v);
    float& out0 = val_[nd.numOutputs ? p_.flatOutput(v, 0) : 0];
    switch (nd.kind) {
      case NodeKind::Const: {
        const Constant& c = p_.constant(v);
        out0 = c.kind == Kind::Bool ? dsp::boolMask(c.value() != 0) : static_cast<float>(c.value());
        return;
      }
      case NodeKind::Param: {
        const ParamDecl& d = p_.paramDecl(v);
        out0 = d.kind == Kind::Bool ? dsp::boolMask(d.defaultValue.value() != 0)
                                    : static_cast<float>(d.defaultValue.value());
        return;
      }
      case NodeKind::Input:
        if (adj_.driver[p_.flatInput(v, 0)] != kNone) out0 = in(v, 0);
        else out0 = static_cast<float>(inputSignal(inputOrdinal_[v], t));
        return;
      case NodeKind::Atom: break;
      default: return;
    }
    const KernelMapping& m = kernelMapping(nd.atom);
    if (m.lowering == Lowering::Constant) {
      out0 = static_cast<float>(sr_);
      return;
    }
    std::vector<float> ins;
    std::vector<uint32_t> regs;
    for (uint32_t i = 0; i < nd.numInputs; ++i) {
      const ArgMap& a = m.args[std::min<size_t>(i, m.args.size() - 1)];
      if (a.use == ArgUse::Input || a.use == ArgUse::InputSeconds) {
        ins.push_back(in(v, i));
        regs.push_back(i);
      }
    }
    std::vector<const float*> ip;
    std::vector<float*> op;
    dsp::Atom k = m.kernel;
    if (state_.count(v) && split(v, 0)) {
      k = k == dsp::Atom::Z1 ? dsp::Atom::Z1Read
          : k == dsp::Atom::DelayLinear ? dsp::Atom::DelayReadLinear
          : k == dsp::Atom::DelayCubic  ? dsp::Atom::DelayReadCubic
                                        : dsp::Atom::DelayReadAllpass;
      for (size_t j = 0; j < regs.size(); ++j)
        if (regs[j] != 0) ip.push_back(&ins[j]);
    } else {
      for (float& x : ins) ip.push_back(&x);
    }
    for (uint32_t o = 0; o < nd.numOutputs; ++o) op.push_back(&val_[p_.flatOutput(v, o)]);
    run(k, ip, op, v);
  }
  void run(dsp::Atom k, std::vector<const float*>& ip, std::vector<float*>& op, NodeIndex v) {
    dsp::KernelArgs a;
    a.in = ip.data();
    a.out = op.data();
    a.state = state_.count(v) ? state_[v]->p : nullptr;
    a.frames = 1;
    a.numIn = static_cast<uint16_t>(ip.size());
    a.numOut = static_cast<uint16_t>(op.size());
    const dsp::KernelFn fn = dsp::findKernel(k, 1, 0);
    if (!fn) std::abort();
    fn(a);
  }
  void write(NodeIndex v) {
    if (!state_.count(v) || !split(v, 0)) return;
    const dsp::Atom k = kernelMapping(p_.node(v).atom).kernel;
    float x = in(v, 0);
    std::vector<const float*> ip{&x};
    std::vector<float*> op;
    run(k == dsp::Atom::Z1 ? dsp::Atom::Z1Write : dsp::Atom::DelayWrite, ip, op, v);
  }

  const TypedPatch& tp_;
  const Patch& p_;
  double sr_;
  Adjacency adj_;
  bool ok_ = true;
  std::vector<NodeIndex> order_;
  std::vector<float> val_;
  std::map<NodeIndex, std::unique_ptr<AlignedBlock>> state_;
  std::map<NodeIndex, uint32_t> inputOrdinal_;
};

// ---- the schedule runner ------------------------------------------------------------------------------------
class Executor {
public:
  Executor(const Schedule& s, uint32_t frames) : s_(s), frames_(frames) {
    for (const Buffer& b : s.buffers)
      bufs_.push_back(std::vector<float>(b.cls == Buffer::Class::Pattern ? 8 : static_cast<size_t>(frames) * b.lanes));
    for (const ParamSlot& p : s.params)
      params_.push_back(std::vector<float>(8, p.kind == Kind::Bool ? dsp::boolMask(p.defaultValue != 0) : p.defaultValue));
    for (const ConstValue& c : s.constants) consts_.push_back(std::vector<float>(8, c.value));
    for (const External& e : s.externals)
      ext_.push_back(std::vector<float>(e.rate == Rate::Audio ? frames : 8, 0.0f));
    arena_ = std::make_unique<AlignedBlock>(s.state.globalBytes);
    for (const StateEntry& e : s.state.entries)
      if (e.tag.kernel != StateTag::kSolveGuess)
        dsp::initState(static_cast<dsp::Atom>(e.tag.kernel), arena_->p + e.offset, e.init);
  }

  // One block starting at sample t0; returns per output the block of values.
  std::vector<std::vector<float>> block(uint64_t t0) {
    for (size_t i = 0; i < s_.externals.size(); ++i)
      for (uint32_t f = 0; f < frames_; ++f) ext_[i][f] = static_cast<float>(inputSignal(s_.externals[i].index, t0 + f));
    for (const Region& r : s_.regions) {
      if (r.kind == RegionKind::Control) {
        for (uint32_t i = r.firstOp; i < r.firstOp + r.numOps; ++i) runOp(s_.ops[i], 0, 1);
      } else if (r.kind == RegionKind::Block) {
        for (uint32_t i = r.firstOp; i < r.firstOp + r.numOps; ++i) runOp(s_.ops[i], 0, frames_);
      } else {
        for (uint32_t t = 0; t < frames_; t += r.subBlock) {
          const uint32_t n = std::min(r.subBlock, frames_ - t);
          for (uint32_t i = r.firstOp; i < r.firstOp + r.numOps; ++i) runOp(s_.ops[i], t, n);
        }
      }
    }
    std::vector<std::vector<float>> out;
    for (const OutputRef& o : s_.outputs) {
      std::vector<float> v(frames_);
      const float* src = ptr(o.value, 0, 1);
      const bool pattern = isPattern(o.value);
      for (uint32_t f = 0; f < frames_; ++f) v[f] = pattern ? src[0] : src[f];
      out.push_back(std::move(v));
    }
    return out;
  }

private:
  bool isPattern(const Operand& o) const {
    switch (o.kind) {
      case Operand::Kind::Buffer: return s_.buffers[o.index].cls == Buffer::Class::Pattern;
      case Operand::Kind::External: return s_.externals[o.index].rate != Rate::Audio;
      default: return true;
    }
  }
  float* ptr(const Operand& o, uint32_t t, uint32_t lanes) {
    float* base = nullptr;
    switch (o.kind) {
      case Operand::Kind::Buffer: base = bufs_[o.index].data(); break;
      case Operand::Kind::Param: base = params_[o.index].data(); break;
      case Operand::Kind::Const: base = consts_[o.index].data(); break;
      case Operand::Kind::External: base = ext_[o.index].data(); break;
      default: std::abort();
    }
    return isPattern(o) ? base : base + static_cast<size_t>(t) * lanes;
  }
  void runOp(const Op& op, uint32_t t, uint32_t frames) {
    if (op.kind != OpKind::Kernel) std::abort();
    std::vector<const float*> ip;
    std::vector<float*> outp;
    const auto ins = s_.inputs(op);
    for (const Operand& o : ins) ip.push_back(ptr(o, t, op.lanes));
    for (const Operand& o : s_.outputsOf(op)) outp.push_back(ptr(o, t, op.lanes));
    dsp::KernelArgs a;
    a.in = ip.data();
    a.out = outp.data();
    a.state = op.state != kNone ? arena_->p + s_.state.entries[op.state].offset : nullptr;
    a.frames = frames;
    a.numIn = op.numIn;
    a.numOut = op.numOut;
    a.splMask = op.splMask;
    const dsp::KernelFn fn = dsp::findKernel(op.kernel, op.lanes, op.splMask);
    if (!fn) std::abort();
    fn(a);
  }

  const Schedule& s_;
  uint32_t frames_;
  std::vector<std::vector<float>> bufs_, params_, consts_, ext_;
  std::unique_ptr<AlignedBlock> arena_;
};

// Runs both for `blocks` blocks of `frames` samples; returns the largest |difference| / max(1, |naive|), or
// infinity when the outputs disagree on NaN-ness or count.
inline double compare(const TypedPatch& tp, const Schedule& s, uint32_t frames, uint32_t blocks, double sr) {
  Naive nv(tp, sr);
  if (!nv.ok()) return INFINITY;
  Executor ex(s, frames);
  double worst = 0.0;
  for (uint32_t b = 0; b < blocks; ++b) {
    const auto got = ex.block(static_cast<uint64_t>(b) * frames);
    for (uint32_t f = 0; f < frames; ++f) {
      const std::vector<float> want = nv.step(static_cast<uint64_t>(b) * frames + f);
      if (want.size() != got.size()) return INFINITY;
      for (size_t o = 0; o < want.size(); ++o) {
        const float a = want[o], g = got[o][f];
        if (std::isnan(a) || std::isnan(g)) {
          if (std::isnan(a) != std::isnan(g)) return INFINITY;
          continue;
        }
        worst = std::max(worst, std::fabs(static_cast<double>(a) - g) / std::max(1.0, std::fabs(static_cast<double>(a))));
      }
    }
  }
  return worst;
}

}  // namespace sched_exec
