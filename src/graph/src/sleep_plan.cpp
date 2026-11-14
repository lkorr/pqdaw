// T1.x-silence (SPEC §7.7): the sleep plan of a schedule. pq/graph/schedule.h ("Sleep plan") describes what it is;
// this file builds it from the atoms' silence declarations (pq/dsp/atoms.h: dsp::silenceInfo, dsp::silenceTail).
//
// Values are op outputs (a value id per output of every op), found by replaying the schedule's buffer writes in op
// order, so physical buffer reuse never confuses two values. Reads of a value made in an EARLIER stage are sources of
// the reading stage (the buffer still holds the value when the stage starts).
//
// AU.dsp (RB-DY H4): besides Z, every value gets under H whether it is CONSTANT (C: from params, constants and Z values
// through stateless ops, a follower converged on a C input, a settled smoother of C signals) and, when known, its
// RANGE (dsp::rangeOf from its inputs' ranges: Z [0, 0], a literal its value, a ranged param its declared range, which
// the host clamps to). A value whose range is [0, 0] is Z like one its rule proves: fast_gain_to_db of a silent level is
// [−inf, −inf], so max(0, dB − Ceiling) is Z and the Limiter's windows and followers decay; the Compressor's Gain
// Computer, fed the −120 dB floor and its controls, is a constant its followers converge on. A smoother whose signals are C (not Z) keeps its state once settled, a
// follower on a C input once converged (SleepStateKind::Converged); both wake on the params their C inputs are made of.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string_view>
#include <utility>
#include <vector>

#include "pq/dsp/atoms.h"
#include "pq/graph/atoms.h"
#include "pq/graph/schedule.h"

namespace pq::graph {

namespace {

using dsp::Atom;
using dsp::SilenceRule;

enum class InKind : uint8_t { Other, Value, Source, Param, ConstZero, ConstK };

struct In {
  InKind kind = InKind::Other;
  uint32_t value = kNone;    // Value / Source of an earlier stage: the producer's value id
  uint32_t index = kNone;    // Param: slot; ConstZero / ConstK: constant index; Source: Schedule::operands index
};

constexpr uint32_t kSolveWrite = kNone - 1;  // lastWriter of a solve unknown's buffer

bool isZero(float v) noexcept { return (std::bit_cast<uint32_t>(v) & 0x7FFFFFFFu) == 0; }

bool isDelayAtom(Atom a) noexcept {
  switch (a) {
    case Atom::DelayLinear: case Atom::DelayCubic: case Atom::DelayAllpass: case Atom::DelayReadLinear:
    case Atom::DelayReadCubic: case Atom::DelayReadAllpass: case Atom::DelayWrite: case Atom::DelayFade:
    case Atom::DelayReadFade: return true;
    default: return false;
  }
}

class Planner {
public:
  explicit Planner(Schedule& s) : s_(s), n_(static_cast<uint32_t>(s.ops.size())) {}

  void run() {
    SleepPlan& plan = s_.sleep;
    plan = SleepPlan{};
    index();
    C_.assign(n_, 0);
    known_.assign(n_, 0);
    zRule_.assign(n_, 0);
    range_.assign(n_, dsp::ValueRange{});
    computeK();
    voiceTail();
    plan.regions.assign(s_.regions.size(), RegionSleep{});
    // Candidates by structure: Block / Loop regions of global stages without solve blocks, of kernels, broadcasts
    // and sends only.
    cand_.assign(s_.regions.size(), 0);
    for (uint32_t r = 0; r < s_.regions.size(); ++r) {
      const Region& reg = s_.regions[r];
      // TMM.6: an oversample region never sleeps (its buffers hold n samples per frame; its converters ring on).
      if (reg.kind == RegionKind::Control || reg.numSolves > 0 || !global_[r] || reg.numOps == 0 || reg.factor > 1) continue;
      bool ok = true;
      for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps && ok; ++i) ok = s_.ops[i].kind != OpKind::Reduce;
      cand_[r] = ok ? 1 : 0;
    }
    // Refine until stable: Z under the current candidates, ops whose state H keeps, live-outs Z. Every round that
    // changes something removes a candidate, so this ends after at most one round per region.
    for (;;) {
      computeZ();
      bool changed = false;
      for (uint32_t r = 0; r < s_.regions.size(); ++r) {
        if (!cand_[r]) continue;
        const Region& reg = s_.regions[r];
        for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i)
          if (!opKeepsState(i)) {
            cand_[r] = 0;
            changed = true;
            break;
          }
      }
      if (changed) continue;
      unite();
      for (uint32_t r = 0; r < s_.regions.size(); ++r) {
        if (!cand_[r]) continue;
        const Region& reg = s_.regions[r];
        bool ok = true;
        for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps && ok; ++i) {
          if (Z_[i]) continue;
          for (uint32_t o = 0; o < s_.ops[i].numOut && ok; ++o) {
            const uint32_t v = valBase_[i] + o;
            if (outputRead_[v]) ok = false;
            for (uint32_t q : readers_[v])
              if (!sameSegment(q, r)) ok = false;
          }
        }
        if (!ok) {
          cand_[r] = 0;
          changed = true;
        }
      }
      if (!changed) break;
    }
    build();
  }

private:
  // ---- indexing ---------------------------------------------------------------------------------------------------
  void index() {
    regionOf_.assign(n_, kNone);
    stageOf_.assign(n_, kNone);
    global_.assign(s_.regions.size(), 0);
    for (uint32_t k = 0; k < s_.stages.size(); ++k) {
      const Stage& st = s_.stages[k];
      for (uint32_t r = st.firstRegion; r < st.firstRegion + st.numRegions && r < s_.regions.size(); ++r) {
        global_[r] = st.poly == Poly::Global ? 1 : 0;
        const Region& reg = s_.regions[r];
        for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps && i < n_; ++i) {
          regionOf_[i] = r;
          stageOf_[i] = k;
        }
      }
    }
    valBase_.assign(n_ + 1, 0);
    for (uint32_t i = 0; i < n_; ++i) valBase_[i + 1] = valBase_[i] + s_.ops[i].numOut;
    const uint32_t nv = valBase_[n_];
    valOp_.assign(nv, 0);
    for (uint32_t i = 0; i < n_; ++i)
      for (uint32_t o = 0; o < s_.ops[i].numOut; ++o) valOp_[valBase_[i] + o] = i;
    readers_.assign(nv, {});
    outputRead_.assign(nv, 0);
    inBase_.assign(n_ + 1, 0);
    ins_.clear();
    std::vector<uint32_t> lastWriter(s_.buffers.size(), kNone);
    std::vector<uint32_t> solveAt(n_ + 1, kNone);  // first solve block starting at op i (blocks are in op order)
    for (uint32_t b = 0; b < s_.solves.size(); ++b)
      if (s_.solves[b].firstOp < n_ && solveAt[s_.solves[b].firstOp] == kNone) solveAt[s_.solves[b].firstOp] = b;
    // The solver reads values too (outside any op): an unknown's initial guess at the block's start, the equations'
    // sides after its body. Such a value is read outside every segment.
    auto solverRead = [&](const Operand& o) {
      if (o.kind != Operand::Kind::Buffer || o.index >= lastWriter.size()) return;
      const uint32_t w = lastWriter[o.index];
      if (w != kNone && w != kSolveWrite) outputRead_[w] = 1;
    };
    auto equationsRead = [&](const SolveBlock& sb) {
      for (uint32_t q = sb.firstEquation; q < sb.firstEquation + sb.numEquations && q < s_.equations.size(); ++q) {
        solverRead(s_.equations[q].lhs);
        solverRead(s_.equations[q].rhs);
      }
    };
    std::vector<std::vector<uint32_t>> solveEndsAt(n_);  // solve blocks whose body ends with op i
    for (uint32_t b = 0; b < s_.solves.size(); ++b)
      if (s_.solves[b].numOps > 0 && s_.solves[b].firstOp + s_.solves[b].numOps - 1 < n_)
        solveEndsAt[s_.solves[b].firstOp + s_.solves[b].numOps - 1].push_back(b);
    for (uint32_t i = 0; i < n_; ++i) {
      for (uint32_t b = solveAt[i]; b != kNone && b < s_.solves.size() && s_.solves[b].firstOp == i; ++b) {
        const SolveBlock& sb = s_.solves[b];
        for (uint32_t u = sb.firstUnknown; u < sb.firstUnknown + sb.numUnknowns && u < s_.unknowns.size(); ++u)
          solverRead(s_.unknowns[u].init);
        for (uint32_t u = sb.firstUnknown; u < sb.firstUnknown + sb.numUnknowns && u < s_.unknowns.size(); ++u)
          if (s_.unknowns[u].value.kind == Operand::Kind::Buffer && s_.unknowns[u].value.index < lastWriter.size())
            lastWriter[s_.unknowns[u].value.index] = kSolveWrite;
        if (sb.numOps == 0) equationsRead(sb);
      }
      const Op& op = s_.ops[i];
      inBase_[i] = static_cast<uint32_t>(ins_.size());
      for (uint32_t k = 0; k < op.numIn; ++k) {
        const uint32_t oi = op.firstOperand + k;
        const Operand& o = s_.operands[oi];
        In in;
        switch (o.kind) {
          case Operand::Kind::Const:
            in.kind = isZero(s_.constants[o.index].value) ? InKind::ConstZero : InKind::ConstK;
            in.index = o.index;
            break;
          case Operand::Kind::Param:
            in.kind = InKind::Param;
            in.index = o.index;
            break;
          case Operand::Kind::External: {
            const External& x = s_.externals[o.index];
            const std::string_view name = x.source == External::Source::Engine ? atomSignature(x.atom).name : "";
            const bool src = x.poly == Poly::Global && (x.source == External::Source::RootInput || name == "track.in" ||
                                                        name == "sidechain.in" || name == "clipPlayer");
            if (src) {
              in.kind = InKind::Source;
              in.index = oi;
            }
            break;
          }
          case Operand::Kind::Buffer: {
            const uint32_t w = o.index < lastWriter.size() ? lastWriter[o.index] : kNone;
            if (w == kNone || w == kSolveWrite) break;
            in.value = w;
            readers_[w].push_back(i);
            in.kind = stageOf_[valOp_[w]] == stageOf_[i] ? InKind::Value : InKind::Source;
            if (in.kind == InKind::Source) in.index = oi;
            break;
          }
          default: break;
        }
        ins_.push_back(in);
      }
      for (uint32_t k = 0; k < op.numOut; ++k) {
        const Operand& o = s_.operands[op.firstOperand + op.numIn + k];
        if (o.kind == Operand::Kind::Buffer && o.index < lastWriter.size()) lastWriter[o.index] = valBase_[i] + k;
      }
      for (uint32_t b : solveEndsAt[i]) equationsRead(s_.solves[b]);
    }
    inBase_[n_] = static_cast<uint32_t>(ins_.size());
    for (const OutputRef& o : s_.outputs) {
      if (o.poly != Poly::Global || o.value.kind != Operand::Kind::Buffer || o.value.index >= lastWriter.size()) continue;
      const uint32_t w = lastWriter[o.value.index];
      if (w != kNone && w != kSolveWrite) outputRead_[w] = 1;
    }
    // The memory write half of each state entry (split z1 / delay pairs share their state).
    writerOf_.assign(s_.state.entries.size(), kNone);
    for (uint32_t i = 0; i < n_; ++i) {
      const Op& op = s_.ops[i];
      if (op.kind != OpKind::Kernel || op.state == kNone || op.state >= writerOf_.size()) continue;
      if (op.kernel == Atom::Z1Write || op.kernel == Atom::DelayWrite) writerOf_[op.state] = i;
    }
  }

  const In& in(uint32_t op, uint32_t k) const noexcept { return ins_[inBase_[op] + k]; }
  bool global(uint32_t op) const noexcept { return regionOf_[op] != kNone && global_[regionOf_[op]]; }

  // ---- K: computed from params and constants only (constant while they are) --------------------------------------
  bool inK(const In& x) const noexcept {
    switch (x.kind) {
      case InKind::Param: case InKind::ConstZero: case InKind::ConstK: return true;
      case InKind::Value: case InKind::Source: return x.value != kNone && K_[x.value];
      default: return false;
    }
  }
  // Identity of a K value: equal codes are equal values (a settled z1 of x is x).
  uint64_t code(const In& x) const noexcept {
    switch (x.kind) {
      case InKind::Param: return (uint64_t{1} << 32) | x.index;
      case InKind::ConstZero: case InKind::ConstK: return (uint64_t{2} << 32) | x.index;
      default: return x.value != kNone ? canon_[x.value] : ~uint64_t{0};
    }
  }
  void computeK() {
    const uint32_t nv = valBase_[n_];
    K_.assign(nv, 0);
    canon_.assign(nv, 0);
    for (uint32_t v = 0; v < nv; ++v) canon_[v] = (uint64_t{3} << 32) | v;
    for (uint32_t i = 0; i < n_; ++i) {
      const Op& op = s_.ops[i];
      if (op.kind != OpKind::Kernel || !global(i) || op.numOut == 0) continue;
      const dsp::KernelInfo& info = dsp::kernelInfo(op.kernel);
      if (op.kernel == Atom::Z1 && op.numIn >= 1 && inK(in(i, 0))) {  // settled: equal to its input
        K_[valBase_[i]] = 1;
        canon_[valBase_[i]] = code(in(i, 0));
        continue;
      }
      if (info.stateSize != 0 || info.resource != dsp::Resource::None) continue;
      bool k = true;
      for (uint32_t j = 0; j < op.numIn && k; ++j) k = inK(in(i, j));
      if (k)
        for (uint32_t o = 0; o < op.numOut; ++o) K_[valBase_[i] + o] = 1;
    }
  }
  // x − x of one K value (a de-zippered coefficient's change term once its z1 has settled).
  bool equalK(uint32_t i) const noexcept {
    const Op& op = s_.ops[i];
    return op.kind == OpKind::Kernel && op.kernel == Atom::Sub && op.numIn == 2 && inK(in(i, 0)) && inK(in(i, 1)) &&
           code(in(i, 0)) == code(in(i, 1));
  }

  // ---- Z: exactly ±0 under H (greatest fixpoint) ----------------------------------------------------------------
  bool inZ(const In& x) const noexcept {
    switch (x.kind) {
      // A source is assumed silent — unless it is computed from params only (a coefficient an earlier stage
      // computed): that is constant, not silent.
      case InKind::Source: return x.value == kNone || !K_[x.value];
      case InKind::ConstZero: return true;
      case InKind::Value: return Z_[valOp_[x.value]] != 0;
      default: return false;
    }
  }
  bool eligible(uint32_t i) const noexcept {
    if (!global(i)) return false;
    const Region& reg = s_.regions[regionOf_[i]];
    if (reg.kind == RegionKind::Control || reg.numSolves > 0 || reg.factor > 1) return false;
    const Op& op = s_.ops[i];
    if (op.kind == OpKind::Reduce) return false;
    if (op.kind == OpKind::Kernel && dsp::kernelInfo(op.kernel).stateSize != 0) return cand_[regionOf_[i]] != 0;
    return true;
  }
  bool rule(uint32_t i) const noexcept {
    const Op& op = s_.ops[i];
    if (op.kind == OpKind::Broadcast || op.kind == OpKind::Send) return op.numIn >= 1 && inZ(in(i, 0));
    if (op.kind != OpKind::Kernel) return false;
    if (equalK(i)) return true;
    const dsp::SilenceInfo si = dsp::silenceInfo(op.kernel);
    const uint32_t mask = op.numIn >= 32 ? si.inputs : si.inputs & ((1u << op.numIn) - 1u);
    switch (si.rule) {
      case SilenceRule::None: return false;
      case SilenceRule::Any:
        for (uint32_t k = 0; k < op.numIn && k < 32; ++k)
          if (((mask >> k) & 1u) && inZ(in(i, k))) return true;
        return false;
      case SilenceRule::All:
      case SilenceRule::Smoother: {
        if (mask == 0) return false;
        for (uint32_t k = 0; k < op.numIn && k < 32; ++k)
          if (((mask >> k) & 1u) && !inZ(in(i, k))) return false;
        if (si.rule == SilenceRule::Smoother)
          for (uint32_t k = 0; k < op.numIn && k < 32; ++k)
            if (((si.settle >> k) & 1u) && !inK(in(i, k))) return false;
        return true;
      }
      case SilenceRule::Clip: {
        if (op.numIn < 3 || !inZ(in(i, 0))) return false;
        const In &lo = in(i, 1), &hi = in(i, 2);
        auto constOf = [&](const In& x, float& v) {
          if (x.kind != InKind::ConstZero && x.kind != InKind::ConstK) return false;
          v = s_.constants[x.index].value;
          return true;
        };
        float l = 0.0f, h = 0.0f;
        return constOf(lo, l) && constOf(hi, h) && l <= 0.0f && h >= 0.0f;
      }
      case SilenceRule::MulAdd:
        return op.numIn >= 3 && (inZ(in(i, 0)) || inZ(in(i, 1))) && inZ(in(i, 2));
      case SilenceRule::Memory: {
        if (mask == 0) {  // a split read: the history its write half keeps
          const uint32_t w = op.state < writerOf_.size() ? writerOf_[op.state] : kNone;
          return w != kNone && cand_[regionOf_[w]] && s_.ops[w].numIn >= 1 && inZ(in(w, 0));
        }
        return inZ(in(i, 0));
      }
    }
    return false;
  }
  // AU.dsp: one forward pass per round (a stateless op reads only values written before it in op order; the memory
  // halves that read later writes are Z by their rule or nothing): each op's Z by its rule or by its range, then its
  // C-ness and range from its inputs'. Rounds only remove Z, so ranges only widen: the greatest fixpoint as before.
  void computeZ() {
    Z_.assign(n_, 0);
    for (uint32_t i = 0; i < n_; ++i) Z_[i] = eligible(i) ? 1 : 0;
    for (bool changed = true; changed;) {
      changed = false;
      for (uint32_t i = 0; i < n_; ++i) {
        zRule_[i] = Z_[i] && rule(i) ? 1 : 0;
        classify(i);
        if (Z_[i] && !zRule_[i] && !(known_[i] && isZero(range_[i].lo) && isZero(range_[i].hi))) {
          Z_[i] = 0;
          changed = true;
          classify(i);
        }
        if (Z_[i]) {
          C_[i] = known_[i] = 1;
          range_[i] = {0.0f, 0.0f};
        }
      }
    }
  }

  // ---- C and ranges under H (AU.dsp) -------------------------------------------------------------------------------
  bool inC(const In& x) const noexcept {
    switch (x.kind) {
      case InKind::Param: case InKind::ConstZero: case InKind::ConstK: return true;
      case InKind::Source: return true;  // silent (Z) or computed from params (K)
      case InKind::Value: return x.value != kNone && (K_[x.value] || C_[valOp_[x.value]] || Z_[valOp_[x.value]]);
      default: return false;
    }
  }
  bool inRange(const In& x, dsp::ValueRange& r) const noexcept {
    switch (x.kind) {
      case InKind::ConstZero: case InKind::ConstK: {
        const ConstValue& c = s_.constants[x.index];
        if (c.kind != Kind::Real || c.value != c.value) return false;
        r = {c.value, c.value};
        return true;
      }
      case InKind::Param: {
        const ParamSlot& p = s_.params[x.index];
        if (p.kind != Kind::Real) return false;
        // The host clamps a ranged param and maps a non-finite one to its default: finite either way.
        constexpr float big = std::numeric_limits<float>::max();
        if (p.hasRange && p.rangeMin <= p.rangeMax && std::fabs(p.rangeMin) <= big && std::fabs(p.rangeMax) <= big) {
          r = {p.rangeMin, p.rangeMax};
          return true;
        }
        if (!(std::fabs(p.defaultValue) <= big)) return false;
        r = {-big, big};
        return true;
      }
      case InKind::Source:  // silent, or a value of an earlier stage computed from params (its range, when known)
        if (inZ(x)) {
          r = {0.0f, 0.0f};
          return true;
        }
        if (x.value == kNone || !known_[valOp_[x.value]]) return false;
        r = range_[valOp_[x.value]];
        return true;
      case InKind::Value: {
        const uint32_t j = valOp_[x.value];
        if (Z_[j]) {
          r = {0.0f, 0.0f};
          return true;
        }
        if (!known_[j]) return false;
        r = range_[j];
        return true;
      }
      default: return false;
    }
  }
  // C-ness and range of op i's outputs from its inputs' (range: one-output stateless kernels only).
  void classify(uint32_t i) noexcept {
    C_[i] = known_[i] = 0;
    const Op& op = s_.ops[i];
    if (!global(i)) return;
    if (op.kind == OpKind::Broadcast) {
      if (op.numIn < 1) return;
      C_[i] = inC(in(i, 0)) ? 1 : 0;
      dsp::ValueRange r;
      if (C_[i] && inRange(in(i, 0), r)) {
        known_[i] = 1;
        range_[i] = r;
      }
      return;
    }
    if (op.kind != OpKind::Kernel) return;
    const dsp::KernelInfo& info = dsp::kernelInfo(op.kernel);
    if (info.stateSize != 0) {
      SleepStateKind k{};
      C_[i] = cStateful(i, k) ? 1 : 0;
      return;
    }
    if (info.resource != dsp::Resource::None) return;
    for (uint32_t k = 0; k < op.numIn; ++k)
      if (!inC(in(i, k))) return;
    C_[i] = 1;
    if (op.numOut != 1 || op.numIn > kMaxRangeIn) return;
    constexpr float inf = std::numeric_limits<float>::infinity();
    dsp::ValueRange r[kMaxRangeIn];
    for (uint32_t k = 0; k < op.numIn; ++k)
      if (!inRange(in(i, k), r[k])) {
        // A choice's index (select's condition, mux's index) may be any value: only the data inputs bound it.
        if (k != 0 || (op.kernel != Atom::Select && op.kernel != Atom::Mux)) return;
        r[k] = {-inf, inf};
      }
    dsp::ValueRange o;
    if (dsp::rangeOf(op.kernel, r, op.numIn, o)) {
      known_[i] = 1;
      range_[i] = o;
    }
  }
  // A stateful op whose output is C under H without being Z (eligible: in a candidate region, its state checked):
  // a follower converged on a C input (with C coefficients: then it reaches a fixed point), a settled smoother of C
  // signals and K target and time.
  bool cStateful(uint32_t i, SleepStateKind& kind) const noexcept {
    const Op& op = s_.ops[i];
    if (Z_[i] || !eligible(i)) return false;
    if (dsp::hasConvergeState(op.kernel) && op.numOut == 1) {
      for (uint32_t k = 0; k < op.numIn; ++k)
        if (!inC(in(i, k))) return false;
      kind = SleepStateKind::Converged;
      return true;
    }
    const dsp::SilenceInfo si = dsp::silenceInfo(op.kernel);
    if (si.rule != SilenceRule::Smoother) return false;
    for (uint32_t k = 0; k < op.numIn && k < 32; ++k) {
      if (((si.inputs >> k) & 1u) && !inC(in(i, k))) return false;
      if (((si.settle >> k) & 1u) && !inK(in(i, k))) return false;
    }
    kind = SleepStateKind::Smoother;
    return true;
  }
  // The inputs op i's Z-ness rests on (its rule's named inputs that are Z; a smoother's signal; a memory's history).
  // AU.dsp: a value Z by its range, or C, rests on every input (a silent source among them is a source).
  bool relied(uint32_t i, uint32_t k) const noexcept {
    const Op& op = s_.ops[i];
    if (!Z_[i] && !C_[i]) return false;
    if (op.kind == OpKind::Broadcast || op.kind == OpKind::Send) return k == 0;
    if (op.kind != OpKind::Kernel) return false;
    if (!zRule_[i]) {
      const In& x = in(i, k);
      return x.kind == InKind::Value || (x.kind == InKind::Source && inZ(x));
    }
    if (equalK(i)) return false;
    const dsp::SilenceInfo si = dsp::silenceInfo(op.kernel);
    if (si.rule == SilenceRule::Any || si.rule == SilenceRule::MulAdd) {
      // One silent factor is enough: rest on one (a value of this stage before a source), not on every one.
      const uint32_t c = anyChoice(i, si.rule == SilenceRule::MulAdd ? 3u : si.inputs);
      if (k == c) return true;
      return si.rule == SilenceRule::MulAdd && k == 2 && inZ(in(i, 2));
    }
    return k < 32 && ((si.inputs >> k) & 1u) && inZ(in(i, k));
  }
  // The factor an Any (or MulAdd's product) rule rests on: the first Z input among `mask`, values of this stage first.
  uint32_t anyChoice(uint32_t i, uint32_t mask) const noexcept {
    const Op& op = s_.ops[i];
    for (int pass = 0; pass < 2; ++pass)
      for (uint32_t k = 0; k < op.numIn && k < 32; ++k) {
        if (!((mask >> k) & 1u)) continue;
        const In& x = in(i, k);
        if (!inZ(x)) continue;
        if (pass == 0 && x.kind != InKind::Value && x.kind != InKind::ConstZero) continue;
        return k;
      }
    return kNone;
  }

  // ---- which ops keep their state under H -------------------------------------------------------------------------
  // How op i's state behaves under H, or false when it does not keep it (the region cannot sleep).
  bool stateKind(uint32_t i, SleepStateKind& kind) const noexcept {
    const Op& op = s_.ops[i];
    const dsp::SilenceInfo si = dsp::silenceInfo(op.kernel);
    if (si.rule == SilenceRule::Memory && Z_[i]) {
      kind = isDelayAtom(op.kernel)         ? SleepStateKind::Delay
             : dsp::hasDecayState(op.kernel) ? SleepStateKind::Decay
                                             : SleepStateKind::Memory;
      return true;
    }
    if (op.kernel == Atom::Z1 && op.numOut == 1 && K_[valBase_[i]]) {
      kind = SleepStateKind::Settled;
      return true;
    }
    if (si.rule == SilenceRule::Smoother && Z_[i]) {
      kind = SleepStateKind::Smoother;
      return true;
    }
    return cStateful(i, kind);  // AU.dsp: converged on / settled with C inputs
  }
  bool opKeepsState(uint32_t i) const noexcept {
    const Op& op = s_.ops[i];
    switch (op.kind) {
      case OpKind::Broadcast: return true;
      case OpKind::Send: return Z_[i] != 0;
      case OpKind::Reduce: return false;
      case OpKind::Kernel: {
        if (dsp::kernelInfo(op.kernel).stateSize == 0) return true;
        SleepStateKind k{};
        return stateKind(i, k);
      }
    }
    return false;
  }

  // ---- segments -----------------------------------------------------------------------------------------------
  uint32_t find(uint32_t r) noexcept {
    while (uf_[r] != r) r = uf_[r] = uf_[uf_[r]];
    return r;
  }
  void join(uint32_t a, uint32_t b) noexcept {
    a = find(a);
    b = find(b);
    if (a != b) uf_[std::max(a, b)] = std::min(a, b);
  }
  bool sameSegment(uint32_t op, uint32_t r) noexcept {
    const uint32_t q = regionOf_[op];
    return q != kNone && cand_[q] && find(q) == find(r);
  }
  // Values of the same stage a candidate relies on through ops that always run: back to candidates (joined) and
  // sources (collected).
  // Ops already visited in the current walk generation (newWalk() starts one).
  void newWalk() {
    if (stamp_.size() != n_) stamp_.assign(n_, 0);
    if (++gen_ == 0) {
      std::fill(stamp_.begin(), stamp_.end(), 0);
      gen_ = 1;
    }
  }
  bool visit(uint32_t op) {
    if (stamp_[op] == gen_) return false;
    stamp_[op] = gen_;
    return true;
  }
  // M4 review M9: iterative (an explicit stack of (op, next input)), in the recursive walk's order: a chain of values
  // as long as a deep rack nest's signal path does not deepen the call stack.
  template <class OnSource>
  void walk(uint32_t region, uint32_t value, OnSource&& onSource) {
    std::vector<std::pair<uint32_t, uint32_t>>& stack = walkStack_;
    stack.clear();
    auto enter = [&](uint32_t v) {
      const uint32_t j = valOp_[v];
      if (!visit(j)) return;
      const uint32_t rj = regionOf_[j];
      if (rj != kNone && cand_[rj]) {
        join(region, rj);
        return;
      }
      stack.emplace_back(j, 0u);
    };
    enter(value);
    while (!stack.empty()) {
      const uint32_t j = stack.back().first;
      const uint32_t k = stack.back().second;
      if (k == s_.ops[j].numIn) {
        stack.pop_back();
        continue;
      }
      ++stack.back().second;
      if (!relied(j, k)) continue;
      const In& x = in(j, k);
      if (x.kind == InKind::Source) onSource(x);
      else if (x.kind == InKind::Value) enter(x.value);
    }
  }
  void unite() {
    uf_.resize(s_.regions.size());
    std::iota(uf_.begin(), uf_.end(), 0u);
    for (uint32_t r = 0; r < s_.regions.size(); ++r) {
      if (!cand_[r]) continue;
      newWalk();  // one walk generation per region: what it reaches through always-running ops joins it
      const Region& reg = s_.regions[r];
      for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i)
        for (uint32_t k = 0; k < s_.ops[i].numIn; ++k) {
          const In& x = in(i, k);
          if (x.kind != InKind::Value) continue;
          const uint32_t rj = regionOf_[valOp_[x.value]];
          if (rj != kNone && cand_[rj]) {
            join(r, rj);
          } else if (relied(i, k)) {
            walk(r, x.value, [](const In&) {});
          }
        }
    }
  }

  // Params whose values the K value x is computed from (pre-order; iterative, M4 review M9).
  void kParams(const In& x0, std::vector<uint32_t>& out, std::vector<uint32_t>& seen, uint32_t gen) const {
    std::vector<std::pair<uint32_t, uint32_t>> stack;  // (op, next input)
    auto enter = [&](const In& x) {
      if (x.kind == InKind::Param) {
        out.push_back(x.index);
        return;
      }
      if ((x.kind != InKind::Value && x.kind != InKind::Source) || x.value == kNone) return;
      const uint32_t j = valOp_[x.value];
      if (seen[j] == gen) return;
      seen[j] = gen;
      stack.emplace_back(j, 0u);
    };
    enter(x0);
    while (!stack.empty()) {
      const uint32_t j = stack.back().first;
      const uint32_t k = stack.back().second;
      if (k == s_.ops[j].numIn) {
        stack.pop_back();
        continue;
      }
      ++stack.back().second;
      enter(in(j, k));
    }
  }

  // AU.dsp: params a C value is made of: kParams' walk, stopped at Z values and values whose range is a point (the same
  // for every param value in range), and through the stateful C ops (a converged follower, a settled smoother) to
  // their inputs.
  void cParams(const In& x0, std::vector<uint32_t>& out, std::vector<uint32_t>& seen, uint32_t gen) const {
    std::vector<std::pair<uint32_t, uint32_t>> stack;  // (op, next input)
    auto enter = [&](const In& x) {
      if (x.kind == InKind::Param) {
        out.push_back(x.index);
        return;
      }
      if ((x.kind != InKind::Value && x.kind != InKind::Source) || x.value == kNone) return;
      if (x.kind == InKind::Source && !K_[x.value]) return;  // silent
      const uint32_t j = valOp_[x.value];
      if (Z_[j] || (known_[j] && range_[j].lo == range_[j].hi)) return;
      if (seen[j] == gen) return;
      seen[j] = gen;
      stack.emplace_back(j, 0u);
    };
    enter(x0);
    while (!stack.empty()) {
      const uint32_t j = stack.back().first;
      const uint32_t k = stack.back().second;
      if (k == s_.ops[j].numIn) {
        stack.pop_back();
        continue;
      }
      ++stack.back().second;
      enter(in(j, k));
    }
  }

  void build() {
    SleepPlan& plan = s_.sleep;
    unite();
    std::vector<uint32_t> segOfRoot(s_.regions.size(), kNone);
    std::vector<std::vector<uint32_t>> members;
    for (uint32_t r = 0; r < s_.regions.size(); ++r) {
      if (!cand_[r]) continue;
      const uint32_t root = find(r);
      if (segOfRoot[root] == kNone) {
        segOfRoot[root] = static_cast<uint32_t>(members.size());
        members.emplace_back();
      }
      members[segOfRoot[root]].push_back(r);
    }
    std::vector<uint32_t> pseen(n_, 0);  // kParams' visited ops, per segment (generation sg + 1)
    for (uint32_t sg = 0; sg < members.size(); ++sg) {
      const std::vector<uint32_t>& regs = members[sg];
      SleepSegment seg;
      seg.stage = stageOf_[s_.regions[regs.front()].firstOp];
      std::vector<uint32_t> sources;  // operand indices
      std::vector<uint32_t> params;
      std::vector<SleepState> states;
      const uint32_t pgen = sg + 1;
      newWalk();  // one walk generation per segment: its sources reached through always-running ops
      auto addSource = [&](const In& x) { sources.push_back(x.index); };
      for (uint32_t r : regs) {
        const Region& reg = s_.regions[r];
        seg.feedback = seg.feedback || reg.kind == RegionKind::Loop;
        for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i) {
          const Op& op = s_.ops[i];
          for (uint32_t k = 0; k < op.numIn; ++k) {
            if (!relied(i, k)) continue;
            const In& x = in(i, k);
            if (x.kind == InKind::Source) {
              addSource(x);
            } else if (x.kind == InKind::Value && !cand_[regionOf_[valOp_[x.value]]]) {
              walk(r, x.value, addSource);
            }
          }
          if (op.kind != OpKind::Kernel) continue;
          if (equalK(i)) {
            kParams(in(i, 0), params, pseen, pgen);
            kParams(in(i, 1), params, pseen, pgen);
          }
          if (dsp::kernelInfo(op.kernel).stateSize == 0 || op.state == kNone) continue;
          SleepStateKind kind{};
          stateKind(i, kind);
          const dsp::SilenceInfo si = dsp::silenceInfo(op.kernel);
          if (kind == SleepStateKind::Settled) kParams(in(i, 0), params, pseen, pgen);
          if (kind == SleepStateKind::Smoother)
            for (uint32_t k = 0; k < op.numIn && k < 32; ++k) {
              if ((si.settle >> k) & 1u) kParams(in(i, k), params, pseen, pgen);
              if (((si.inputs >> k) & 1u) && !Z_[i]) cParams(in(i, k), params, pseen, pgen);  // AU.dsp: C signals
            }
          if (kind == SleepStateKind::Converged)  // its input and its coefficients: their change moves its fixed point
            for (uint32_t k = 0; k < op.numIn; ++k) cParams(in(i, k), params, pseen, pgen);
          const uint32_t tail = dsp::silenceTail(op.kernel, s_.state.entries[op.state].init);
          auto it = std::find_if(states.begin(), states.end(), [&](const SleepState& x) { return x.entry == op.state; });
          if (it == states.end()) {
            states.push_back({op.state, kind, tail});
          } else {
            it->tail = std::max(it->tail, tail);
          }
          seg.tailFrames = std::max(seg.tailFrames, tail);
        }
      }
      // One source per operand value (the same input read by several ops is one source).
      auto key = [&](uint32_t oi) {
        return (static_cast<uint64_t>(s_.operands[oi].kind) << 32) | s_.operands[oi].index;
      };
      std::sort(sources.begin(), sources.end(), [&](uint32_t a, uint32_t b) { return key(a) < key(b); });
      sources.erase(std::unique(sources.begin(), sources.end(), [&](uint32_t a, uint32_t b) { return key(a) == key(b); }),
                    sources.end());
      std::sort(params.begin(), params.end());
      params.erase(std::unique(params.begin(), params.end()), params.end());
      seg.firstSource = static_cast<uint32_t>(plan.sources.size());
      for (uint32_t oi : sources) plan.sources.push_back(s_.operands[oi]);
      seg.numSources = static_cast<uint32_t>(sources.size());
      seg.firstState = static_cast<uint32_t>(plan.states.size());
      plan.states.insert(plan.states.end(), states.begin(), states.end());
      seg.numStates = static_cast<uint32_t>(states.size());
      seg.firstParam = static_cast<uint32_t>(plan.params.size());
      plan.params.insert(plan.params.end(), params.begin(), params.end());
      seg.numParams = static_cast<uint32_t>(params.size());
      // Fills: per region, the buffers of its values read outside the segment.
      for (uint32_t r : regs) {
        RegionSleep& rs = plan.regions[r];
        rs.segment = sg;
        rs.firstFill = static_cast<uint32_t>(plan.fills.size());
        const Region& reg = s_.regions[r];
        std::vector<uint32_t> fills;
        for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i)
          for (uint32_t o = 0; o < s_.ops[i].numOut; ++o) {
            const uint32_t v = valBase_[i] + o;
            bool outside = outputRead_[v] != 0;
            for (uint32_t q : readers_[v]) outside = outside || !sameSegment(q, r);
            const Operand& dst = s_.operands[s_.ops[i].firstOperand + s_.ops[i].numIn + o];
            if (outside && dst.kind == Operand::Kind::Buffer) fills.push_back(dst.index);
          }
        std::sort(fills.begin(), fills.end());
        fills.erase(std::unique(fills.begin(), fills.end()), fills.end());
        plan.fills.insert(plan.fills.end(), fills.begin(), fills.end());
        rs.numFills = static_cast<uint32_t>(fills.size());
      }
      plan.segments.push_back(seg);
    }
    // Dormant: nothing outside the segments keeps state or sends.
    bool dormant = true;
    for (uint32_t i = 0; i < n_ && dormant; ++i) {
      if (!global(i)) continue;
      const Op& op = s_.ops[i];
      if (cand_[regionOf_[i]]) continue;
      if (op.kind == OpKind::Send) dormant = false;
      // TX.smooth: a param ramp's state only settles (the host checks it is settled before it reports dormant).
      if (op.kind == OpKind::Kernel && dsp::kernelInfo(op.kernel).stateSize != 0 && op.kernel != dsp::Atom::ParamRamp)
        dormant = false;
    }
    for (const SolveBlock& sb : s_.solves)
      if (sb.region < s_.regions.size() && global_[sb.region]) dormant = false;
    plan.dormantCapable = dormant;
    if (plan.segments.empty()) {
      const uint32_t tail = plan.voiceTailFrames;
      const bool d = plan.dormantCapable;
      plan = SleepPlan{};
      plan.voiceTailFrames = tail;
      plan.dormantCapable = d;
    }
  }

  void voiceTail() {
    uint32_t tail = 0;
    for (uint32_t i = 0; i < n_; ++i) {
      const Op& op = s_.ops[i];
      if (op.kind != OpKind::Kernel || global(i) || regionOf_[i] == kNone || !isDelayAtom(op.kernel) ||
          op.state >= s_.state.entries.size())
        continue;
      tail = std::max(tail, dsp::silenceTail(op.kernel, s_.state.entries[op.state].init));
    }
    s_.sleep.voiceTailFrames = tail;
  }

  Schedule& s_;
  const uint32_t n_;
  std::vector<uint32_t> regionOf_, stageOf_;
  std::vector<uint8_t> global_;     // per region: in a global stage
  std::vector<uint32_t> valBase_, valOp_;
  std::vector<std::vector<uint32_t>> readers_;
  std::vector<uint8_t> outputRead_;
  std::vector<uint32_t> inBase_;
  std::vector<In> ins_;
  std::vector<uint32_t> writerOf_;  // per state entry: its write-half op
  std::vector<uint8_t> K_;
  std::vector<uint64_t> canon_;
  std::vector<uint8_t> Z_;          // per op
  std::vector<uint8_t> zRule_;      // per op: Z by its silence rule (else by its range)
  std::vector<uint8_t> C_;          // per op: constant under H (AU.dsp)
  std::vector<uint8_t> known_;      // per op: range_ holds its (one) output's bounds under H
  std::vector<dsp::ValueRange> range_;
  static constexpr uint32_t kMaxRangeIn = 16;
  std::vector<uint8_t> cand_;       // per region
  std::vector<uint32_t> uf_;
  std::vector<uint32_t> stamp_;     // walk(): per op, the generation that visited it
  std::vector<std::pair<uint32_t, uint32_t>> walkStack_;  // walk()'s stack (reused)
  uint32_t gen_ = 0;
};

}  // namespace

void planSleep(Schedule& s) { Planner(s).run(); }

}  // namespace pq::graph
