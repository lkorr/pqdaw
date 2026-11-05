// Schedule → LLVM IR (codegen.h says what is generated; pq/graph/schedule.h says how a Schedule must run).

#include "codegen.h"

#include <llvm/ADT/FloatingPointMode.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/ModRef.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

#include "ir_ops.h"
#include "pq/core/fast_math.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/atoms.h"
#include "pq/engine/newton.h"
#include "runtime.h"

namespace pq::jit::detail {

namespace {

namespace g = pq::graph;
namespace newton = pq::engine::newton;
using dsp::Atom;
using llvm::Value;

// The kernels' constants (src/dsp/src/kernels_math.cpp, kernels_gen.cpp), as the same float literals.
constexpr float kLn2 = 0.693147180559945309f;
constexpr float kLog2e = 1.44269504088896341f;
constexpr float kTwoPi = 6.28318530717958648f;
constexpr float kBelowOne = 0.99999994f;
constexpr float kTwoPiOver2p32 = 1.46291807926715968e-09f;
constexpr float k2pm24 = 5.9604644775390625e-08f;

// Code-size caps for fused runs (opCost units ≈ instructions per chunk; T1b.1-fix1, measured in bench/RESULTS.md).
constexpr uint32_t kMaxRunCost = 1024;         // a fused run is split before it grows past this
constexpr uint32_t kInterleaveBudget = 2048;   // chunks interleaved per iteration × run cost
constexpr size_t kMaxRunInvariants = 16;       // distinct pattern operands (params, constants) per fused run
// T4.12: a voice stage with a chain above this cost gets no pair function. Twice a big chain's live values and staged
// state overflowed the 4 KiB frame that needs no stack probes (Operator with every section on: chains of 372–740,
// pair frames of 4.4 KiB; the corpus's chains are ≤ 232); such a stage runs group by group.
constexpr uint32_t kMaxPairChainCost = 320;
// TX.smooth: ... and a lower bound for a stage that runs param_ramp calls: a stage with calls holds the call scratch
// and the pointers its calls and ramp dispatch reuse (measured: the Operator's "full sines" pair frame went from 3.7 to
// 4.2 KiB with its 11 ramps at a chain cost inside (256, 320]; the corpus's chains, ≤ 232, keep their pairs).
constexpr uint32_t kMaxPairChainCostWithRamps = 256;
// T1b.x-perf (Q122): a Block region of a GLOBAL stage joins a chain only up to this cost. In a chain its ops run one
// frame at a time (scalar) instead of 8 frames per vector, which is free while they fill the recursions' idle issue
// slots and a loss once they outweigh them (measured: bench_devices, Saturator / EQ Eight).
constexpr uint32_t kMaxGlobalChainBlockCost = 96;
// TX.uniform-branch: a branch (graph::Schedule::branches) is taken in a Block region when the ops one of its sides
// guards cost at least this (opCost units): below it, the stores and loads where the guarded ops leave the fused run
// cost about what skipping them saves.
constexpr uint32_t kMinBranchCost = 8;
// ... and a global chain holds at most this many loop-invariant operands (params, constants, patterns: loaded once
// before the loop) plus state words (carried from frame to frame). Past the register file they spill, and a spilled
// state puts a store→load round trip on its recursion (measured: EQ Eight's 14 sections in one chain ran 2.3× slower
// than one loop per section).
constexpr size_t kMaxGlobalChainLive = 20;

static_assert(offsetof(dsp::KernelArgs, in) == 0 && offsetof(dsp::KernelArgs, out) == 8 &&
              offsetof(dsp::KernelArgs, state) == 16 && offsetof(dsp::KernelArgs, resource) == 24 &&
              offsetof(dsp::KernelArgs, frames) == 32 && offsetof(dsp::KernelArgs, numIn) == 36 &&
              offsetof(dsp::KernelArgs, numOut) == 38 && offsetof(dsp::KernelArgs, splMask) == 40 &&
              sizeof(dsp::KernelArgs) == 48,
              "the IR's KernelArgs struct type mirrors this layout");
static_assert(offsetof(dsp::PhasorState, incScale) == 32 && offsetof(dsp::PhasorState, halfSr) == 40);
static_assert(offsetof(dsp::FlipFlopState, prevClk) == 32 && offsetof(dsp::SampleHoldState, prevTrig) == 32);
static_assert(offsetof(dsp::CounterState, prevUp) == 32 && offsetof(dsp::CounterState, prevDown) == 64);
static_assert(offsetof(dsp::ClockDivState, prevClk) == 32 && offsetof(dsp::TimerState, prevTrig) == 32);

bool isElementwise(Atom a) noexcept {
  switch (a) {
    case Atom::Add: case Atom::Sub: case Atom::Mul: case Atom::Div: case Atom::Min: case Atom::Max: case Atom::Mod:
    case Atom::Pow: case Atom::PowFast: case Atom::CentsToHz: case Atom::CentsToHzFast: case Atom::HzToCents:
    case Atom::HzToCentsFast: case Atom::Neg: case Atom::Abs: case Atom::Sign: case Atom::Floor: case Atom::Frac:
    case Atom::Sqrt: case Atom::SqrtFast: case Atom::Exp: case Atom::ExpFast: case Atom::Log: case Atom::LogFast:
    case Atom::Sin: case Atom::SinFast: case Atom::Tan: case Atom::TanFast: case Atom::Tanh: case Atom::TanhFast:
    case Atom::DbToGain: case Atom::DbToGainFast: case Atom::GainToDb: case Atom::GainToDbFast: case Atom::Const:
    case Atom::Clip: case Atom::Lerp: case Atom::Select: case Atom::MulAdd: case Atom::And: case Atom::Or:
    case Atom::Xor: case Atom::Nand: case Atom::Nor: case Atom::Not: case Atom::Gt: case Atom::Lt: case Atom::Ge:
    case Atom::Le: case Atom::Eq: case Atom::Ne: case Atom::GateToReal: case Atom::Mux: case Atom::Demux:
    case Atom::SinePhase:
      return true;
    default:
      return false;
  }
}

// Stateful atoms generated inline in per-frame contexts (one frame per evaluation; state in registers).
bool isSimpleStateful(Atom a) noexcept { return inlineStateful(a); }  // jit_image.cpp

int exactOf(Atom a) noexcept {
  switch (a) {
    case Atom::Sin: return static_cast<int>(rt::Exact::Sin);
    case Atom::Tan: return static_cast<int>(rt::Exact::Tan);
    case Atom::Tanh: return static_cast<int>(rt::Exact::Tanh);
    case Atom::Exp: return static_cast<int>(rt::Exact::Exp);
    case Atom::Log: return static_cast<int>(rt::Exact::Log);
    case Atom::Pow: return static_cast<int>(rt::Exact::Pow);
    case Atom::CentsToHz: return static_cast<int>(rt::Exact::CentsToHz);
    case Atom::HzToCents: return static_cast<int>(rt::Exact::HzToCents);
    case Atom::DbToGain: return static_cast<int>(rt::Exact::DbToGain);
    case Atom::GainToDb: return static_cast<int>(rt::Exact::GainToDb);
    default: return -1;
  }
}

using Map = std::unordered_map<uint32_t, Value*>;  // buffer index → its value at the current frame (this width)

struct Frame {
  Value* t = nullptr;   // i64 frame index of the first frame this evaluation covers
  unsigned w = 1;       // 1: scalar, 8: <8 x float>, 16: <16 x float> (a voice pair)
  Map* map = nullptr;   // values computed earlier in this evaluation
  bool hoist = false;   // pattern loads may go to the region preheader
  Value* mask = nullptr;  // <8 x i1>: only these frames exist (a 1-lane slice's last, partial 8 frames)
};

class Gen {
public:
  Gen(const g::Schedule& s, const HostLayout& L, PtrTable& tab, llvm::Module& m, const GenOptions& o)
      : s_(s), L_(L), tab_(tab), M_(m), C_(m.getContext()), B_(C_), o_(o) {
    f32_ = llvm::Type::getFloatTy(C_);
    f64_ = llvm::Type::getDoubleTy(C_);
    i16_ = llvm::Type::getInt16Ty(C_);
    i32_ = llvm::Type::getInt32Ty(C_);
    i64_ = llvm::Type::getInt64Ty(C_);
    ptr_ = llvm::PointerType::getUnqual(C_);
    void_ = llvm::Type::getVoidTy(C_);
    kargsTy_ = llvm::StructType::get(C_, {ptr_, ptr_, ptr_, ptr_, i32_, i16_, i16_, i32_});
    if (o.contract) {
      fmf_.setAllowContract(true);
      fmf_.setApproxFunc(true);
    }
  }

  // T4.5: per region, the first region of the unit generated code times it in (a chain is one loop: one unit).
  std::vector<uint32_t> units() {
    analyse();
    std::vector<uint32_t> u(s_.regions.size());
    for (uint32_t r = 0; r < u.size(); ++r) u[r] = r;
    for (uint32_t r = 0; r < u.size(); ++r)
      if (chainEnd_[r] >= 0)
        for (uint32_t q = r + 1; q <= static_cast<uint32_t>(chainEnd_[r]) && q < u.size(); ++q) u[q] = r;
    return u;
  }

  Result<GenResult> run() {
    analyse();
    if (o_.scalarLanes)
      for (const g::Stage& st : s_.stages) {
        if (st.poly != g::Poly::Voice) continue;
        for (uint32_t r = st.firstRegion; r < st.firstRegion + st.numRegions; ++r)
          for (uint32_t i = s_.regions[r].firstOp; i < s_.regions[r].firstOp + s_.regions[r].numOps; ++i)
            if (!inline_[i])
              return Error{ErrorCode::Unsupported,
                           "jit: the scalar-lane baseline needs every voice op inline; op " + std::to_string(i) + " (" +
                               (s_.ops[i].kind == g::OpKind::Kernel ? dsp::kernelInfo(s_.ops[i].kernel).name : "special op") +
                               ") is a call",
                           {}};
      }
    res_.pairFunctions.assign(s_.stages.size(), {});
    res_.rampFunctions.assign(s_.stages.size(), {});
    for (uint32_t k = 0; k < s_.stages.size(); ++k) {
      const g::Stage& st = s_.stages[k];
      const bool voice = st.poly == g::Poly::Voice;
      const std::string name = o_.prefix + "_stage" + std::to_string(k);
      stageFunction(k, name, voice ? (o_.scalarLanes ? 1u : 8u) : 0u, false);
      res_.stageFunctions.push_back(name);
      if (wantsPair(k)) {
        stageFunction(k, name + "_pair", 16, false);
        res_.pairFunctions[k] = name + "_pair";
      }
    }
    // TX.smooth-fix1: the ramp-mode functions after every ramp-free one, so that the code the engine runs at rest is
    // the ramp-free schedule's, function for function and in the same order (its placement included).
    for (uint32_t k = 0; k < s_.stages.size(); ++k) {
      const bool voice = s_.stages[k].poly == g::Poly::Voice;
      const std::string name = o_.prefix + "_stage" + std::to_string(k);
      stageFunction(k, name, voice ? (o_.scalarLanes ? 1u : 8u) : 0u, true);
      if (wantsPair(k)) stageFunction(k, name + "_pair", 16, true);
    }
    for (uint32_t i = 0; i < s_.ops.size(); ++i) (inline_[i] ? res_.inlinedOps : res_.calledOps) += 1;
    std::string err;
    llvm::raw_string_ostream os(err);
    if (llvm::verifyModule(M_, &os)) return Error{ErrorCode::Unknown, "jit: generated IR does not verify: " + err, {}};
    return res_;
  }

  // T1b.x-perf (Q111 C): which stages run() gives a pair function, without generating any code.
  std::vector<uint8_t> pairPlan() {
    analyse();
    std::vector<uint8_t> plan(s_.stages.size(), 0);
    for (uint32_t k = 0; k < s_.stages.size(); ++k) plan[k] = wantsPair(k) ? 1 : 0;
    return plan;
  }

private:
  // A voice stage the host lets pair, without per-frame calls and without a chain too big for a pair's frame (after
  // analyse()).
  bool wantsPair(uint32_t k) const {
    const g::Stage& st = s_.stages[k];
    return st.poly == g::Poly::Voice && !o_.scalarLanes && o_.pairs && k < L_.pairable.size() && L_.pairable[k] &&
           !frameCalls(st) &&
           chainCost_[k] <= (stageChecksRamps(st) && !pairCanBail(st) ? kMaxPairChainCostWithRamps : kMaxPairChainCost);
  }
  // TX.smooth: a pair function of a stage with checked ramps (TX.smooth-fix1: non-static sources) hands a block in
  // which one moves back to the host (which then runs the two groups one by one) when everything before its ramp check
  // may run twice: a Control region of stateless kernels only. Its code at rest is then the pre-ramp pair code plus
  // the checks. A stage whose ramps are all static has no checks: its pair function is the ramp-free pair code.
  bool pairCanBail(const g::Stage& st) const noexcept {
    for (uint32_t r = st.firstRegion; r < st.firstRegion + st.numRegions; ++r) {
      const g::Region& reg = s_.regions[r];
      if (reg.kind != g::RegionKind::Control) break;
      if (reg.numSolves != 0) return false;
      for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i)
        if (s_.ops[i].kind != g::OpKind::Kernel || s_.ops[i].state != g::kNone) return false;
    }
    return true;
  }
  // TX.smooth-fix1: the stage has a ramp of a non-static source (its code checks it per block).
  bool stageChecksRamps(const g::Stage& st) const noexcept {
    if (st.numRegions == 0 || st.firstRegion + st.numRegions > s_.regions.size()) return false;
    const uint32_t a = s_.regions[st.firstRegion].firstOp;
    const g::Region& last = s_.regions[st.firstRegion + st.numRegions - 1];
    const uint32_t b = last.firstOp + last.numOps;
    for (const g::Ramp& rp : s_.ramps)
      if (rp.op >= a && rp.op < b && !rp.staticSource) return true;
    return false;
  }

  // ================================================================================================================
  // Analysis: how each op is emitted, and which op outputs must reach memory.
  // ================================================================================================================
  // Whether a stage calls a function once per frame (a kernel call in a per-sample loop, an exact transcendental
  // there, Newton's n > 1 update; not a wavetable_pos computed inline after its block's first frame, whose call is
  // off the frame path). Such a stage gets no pair function: a call clobbers every vector register, so the
  // loop spills and reloads all its live values around it, and twice the values per pair overflow the 4 KiB frame
  // (stack probes; tests/jit stateful_loops.pqp) for no gain, since the calls dominate.
  bool frameCalls(const g::Stage& st) const {
    for (uint32_t r = st.firstRegion; r < st.firstRegion + st.numRegions; ++r) {
      const g::Region& reg = s_.regions[r];
      if (!perFrame(reg) || reg.kind == g::RegionKind::Control) continue;
      for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i)
        if ((!inline_[i] && !sineInline(i, true, 16)) || exactOf(s_.ops[i].kernel) >= 0) return true;
      for (uint32_t k = reg.firstSolve; k < reg.firstSolve + reg.numSolves; ++k)
        if (s_.solves[k].numUnknowns > 1) return true;
    }
    return false;
  }
  // T4.x-fix-dsp: a global smoothed gain with a param (Spl) target is generated inline: per frame its frame-by-frame
  // definition (kernels_gain.cpp; per-sample loops, chains, a run's moving path), in a vectorised run its settled
  // form (emitFused branches on it once per block), after the call's prologue (dsp::gainPrologue). The device mixes
  // (EQ Eight's sections between their SVF loops, the Saturator's oversampled samples) then stay in one chain / run
  // instead of splitting it around a call. Voice gains and audio-rate targets stay kernel calls.
  bool inlineGain(uint32_t i) const {
    const g::Op& op = s_.ops[i];
    if (op.kind != g::OpKind::Kernel || (op.kernel != Atom::Gain && op.kernel != Atom::GainAdd) || op.lanes != 1) return false;
    if (op.state == g::kNone || (i < body_.size() && body_[i])) return false;
    const uint32_t kA = op.kernel == Atom::GainAdd ? 1u : 0u;
    return ((op.splMask >> (kA + 1)) & 1u) != 0 && ((op.splMask >> (kA + 2)) & 1u) != 0 && op.numIn >= kA + 3;
  }
  // Input k of op i is read only by an inline gain's prologue (its target, time and start: not in the frame loop, so
  // not a register the loop holds).
  bool prologueOnly(uint32_t i, uint32_t k) const {
    if (i >= inline_.size() || !inline_[i] || (s_.ops[i].kernel != Atom::Gain && s_.ops[i].kernel != Atom::GainAdd)) return false;
    return k >= (s_.ops[i].kernel == Atom::GainAdd ? 2u : 1u);
  }
  // While emitting a frame loop's settled copy (emitGainBranch): inline gains take their settled form.
  bool gainsSettled_ = false;
  std::unordered_map<uint32_t, Value*> gainTarget_;  // op → its target, loaded after its prologue (before the loops)
  bool perFrame(const g::Region& r) const noexcept {
    return r.kind == g::RegionKind::Control || (r.kind == g::RegionKind::Loop && (r.subBlock <= 1 || r.numSolves > 0));
  }

  // TX.uniform-branch (graph::Schedule::branches): which branches the slices take (taken_), and per op the guard it runs
  // under among those (guardEff_: its nearest taken guard, kNone: unconditional). A branch is taken when its select is
  // in a Block region (Loop regions hold none) and one of its sides guards at least kMinBranchCost; chained regions
  // (emitChain) run every op, as without branches.
  void analyseBranches() {
    const auto n = static_cast<uint32_t>(s_.ops.size());
    guardEff_.assign(n, g::kNone);
    branchOf_.assign(n, g::kNone);
    taken_.assign(s_.branches.size(), 0);
    if (s_.branches.empty() || s_.guardOf.size() != n) return;
    std::vector<uint32_t> cost(2 * s_.branches.size(), 0);
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t gd = s_.guardOf[i]; gd != g::kNone && (gd >> 1) < s_.branches.size(); gd = s_.branches[gd >> 1].parent)
        cost[gd] += opCost(i);
    for (uint32_t b = 0; b < s_.branches.size(); ++b) {
      const g::Branch& br = s_.branches[b];
      if (br.op >= n || br.region >= s_.regions.size() || s_.regions[br.region].kind != g::RegionKind::Block) continue;
      const g::Op& op = s_.ops[br.op];
      if (op.kind != g::OpKind::Kernel || op.kernel != Atom::Select || op.numIn != 3 || (op.splMask & 1u) == 0) continue;
      if (br.parent != g::kNone && (br.parent >> 1) >= b) continue;  // malformed: parents come first
      taken_[b] = std::max(cost[2 * b], cost[2 * b + 1]) >= kMinBranchCost ? 1 : 0;
      if (taken_[b]) branchOf_[br.op] = b;
    }
    for (uint32_t i = 0; i < n; ++i)
      guardEff_[i] = takenGuard(branchOf_[i] != g::kNone ? s_.branches[branchOf_[i]].parent : s_.guardOf[i]);
  }
  uint32_t takenGuard(uint32_t gd) const {
    while (gd != g::kNone && (gd >> 1) < taken_.size() && !taken_[gd >> 1]) gd = s_.branches[gd >> 1].parent;
    return gd != g::kNone && (gd >> 1) < taken_.size() ? gd : g::kNone;
  }
  bool branchSelect(uint32_t i) const noexcept { return i < branchOf_.size() && branchOf_[i] != g::kNone; }

  void analyse() {
    const auto n = static_cast<uint32_t>(s_.ops.size());
    inline_.assign(n, false);
    grp_.assign(n, -1);
    body_.assign(n, false);
    opRegion_.assign(n, 0);
    for (uint32_t r = 0; r < s_.regions.size(); ++r)
      for (uint32_t i = s_.regions[r].firstOp; i < s_.regions[r].firstOp + s_.regions[r].numOps && i < n; ++i) opRegion_[i] = r;
    for (const g::SolveBlock& sb : s_.solves)
      for (uint32_t i = sb.firstOp; i < sb.firstOp + sb.numOps && i < n; ++i) body_[i] = true;
    analyseBranches();
    int32_t next = 0;
    for (const g::Region& r : s_.regions) {
      const bool pf = perFrame(r);
      const int32_t regionGroup = next++;
      int32_t run = -1;
      uint32_t runLanes = 0, runSize = 0;
      uint32_t runGuard = g::kNone;  // TX.uniform-branch: a run holds ops of one guard; a branch's select is alone
      std::set<std::pair<int, uint32_t>> runInv;  // the run's distinct pattern operands (hoisted loads)
      for (uint32_t i = r.firstOp; i < r.firstOp + r.numOps; ++i) {
        const g::Op& op = s_.ops[i];
        const bool kernel = op.kind == g::OpKind::Kernel;
        // In slices, exact transcendentals stay block-wise kernel calls: the CRT/SVML call is the cost, and a call
        // inside a fused loop would spill every live vector register around it.
        // A mux/demux wider than kStackCallOperands is a kernel call: inline, its select chain is one basic block
        // of thousands of instructions per chunk (the M1 review's 1200-input mux), the kernel runs it as a loop.
        // T1b.2: in voice slices the simple stateful atoms are inline too (one frame of the group's voices per
        // chunk: the state is one vector carried from frame to frame, as in a per-sample loop), and in the
        // scalar-lane baseline so are the exact transcendentals (one scalar call per lane and frame).
        const bool voiceSlice = !pf && r.poly == g::Poly::Voice && op.lanes == 8;
        inline_[i] = kernel && !wideCall(op) &&
                     ((isElementwise(op.kernel) && (pf || exactOf(op.kernel) < 0 || (voiceSlice && o_.scalarLanes))) ||
                      ((pf || voiceSlice) && isSimpleStateful(op.kernel)) || (r.poly == g::Poly::Global && inlineGain(i)));
        if (pf) {
          grp_[i] = inline_[i] && !body_[i] ? regionGroup : -1;
          continue;
        }
        if (!inline_[i]) {
          run = -1;
          continue;
        }
        // A run ends at kMaxRunCost: past that, values kept in registers across the run (store elimination) only
        // turn into spills, and instruction selection / register allocation of one huge block is superlinear (the
        // M1 review's 1200-input mux after 1200 products took 1.3–3 s to compile as one run).
        // It also ends before it reads more than kMaxRunInvariants distinct params / constants / patterns: they are
        // loaded once before the loop, so each is a register for the whole loop (or, past the register file, a
        // spill: with one constant per op, the review's mux probe spilled 15 KB and needed stack probes).
        const uint32_t c = opCost(i);
        uint32_t newInv = 0;
        const auto opIns = s_.inputs(op);
        for (uint32_t k = 0; k < opIns.size(); ++k) {
          const g::Operand& o = opIns[k];
          if (isPattern(o) && o.kind != g::Operand::Kind::None && !prologueOnly(i, k) &&
              !runInv.count({static_cast<int>(o.kind), o.index}))
            ++newInv;
        }
        const bool sel = branchSelect(i);
        if (run < 0 || runLanes != op.lanes || (runSize > 0 && runSize + c > kMaxRunCost) ||
            (runSize > 0 && runInv.size() + newInv > kMaxRunInvariants) || guardEff_[i] != runGuard || sel ||
            (i > r.firstOp && branchSelect(i - 1))) {
          run = next++;
          runLanes = op.lanes;
          runSize = 0;
          runInv.clear();
          runGuard = guardEff_[i];
        }
        runSize += c;
        for (uint32_t k = 0; k < opIns.size(); ++k)
          if (isPattern(opIns[k]) && opIns[k].kind != g::Operand::Kind::None && !prologueOnly(i, k))
            runInv.insert({static_cast<int>(opIns[k].kind), opIns[k].index});
        grp_[i] = run;
      }
    }
    // Chains (T1b.2, codegen.h): consecutive regions of a voice stage that run frame by frame with every op inline
    // (Block regions of inline ops, per-sample Loop regions) become one loop over frames, when one of them is a
    // per-sample loop (whose dependency chain the other regions' independent work overlaps) and the whole body
    // stays below kMaxRunCost (a bigger body would not fit the out-of-order window either). One group: values
    // cross the regions in registers.
    // T1b.x-perf (Q122): global stages too, where each feedback SCC is its own per-sample loop (an EQ's bands, a
    // crossover, half-band allpasses: one latency-bound loop after another). A global chain runs from its first to
    // its last per-sample loop (at least two; the Block regions before and after them stay vectorised over time),
    // its Block regions are cheap (kMaxGlobalChainBlockCost: they turn scalar), and its regions share one sleep
    // segment (or none), so one flag decides the whole chain. Independent recursions then overlap (L/R, M/S) and a
    // cascade pipelines across frames: per frame the cost tends to the longest recursion, not the sum.
    memo_.assign(s_.regions.size(), 0);
    for (uint32_t r = 0; r < s_.regions.size(); ++r) memo_[r] = memoizable(s_.regions[r]) ? 1 : 0;
    // TX.vca-knob2 (graph::Schedule::memoMask): a memo region whose ops read different params runs, in a block whose
    // params changed, only the runs of ops (consecutive, one mask) that read a changed one. Each run is its own group:
    // a value it hands to another run is stored (the scheduler gave it a buffer of its own).
    masked_.assign(s_.regions.size(), 0);
    for (uint32_t r = 0; r < s_.regions.size(); ++r) {
      const g::Region& reg = s_.regions[r];
      if (!memo_[r] || s_.memoMask.size() != n || reg.numOps < 2) continue;
      bool mixed = false;
      for (uint32_t i = reg.firstOp + 1; i < reg.firstOp + reg.numOps && !mixed; ++i)
        mixed = s_.memoMask[i] != s_.memoMask[reg.firstOp];
      if (!mixed) continue;
      masked_[r] = 1;
      int32_t run = -1;
      for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i) {
        if (i == reg.firstOp || s_.memoMask[i] != s_.memoMask[i - 1]) run = next++;
        if (grp_[i] >= 0) grp_[i] = run;
      }
    }
    chainEnd_.assign(s_.regions.size(), -1);
    chainCost_.assign(s_.stages.size(), 0);
    for (uint32_t sk = 0; sk < s_.stages.size(); ++sk) {
      const g::Stage& st = s_.stages[sk];
      const bool voice = st.poly == g::Poly::Voice;
      auto segmentOf = [&](uint32_t r) {
        return !voice && r < s_.sleep.regions.size() ? s_.sleep.regions[r].segment : g::kNone;
      };
      auto chainable = [&](uint32_t r) {
        const g::Region& reg = s_.regions[r];
        if (reg.kind == g::RegionKind::Control) return false;
        if (perFrame(reg)) return true;
        if (reg.kind != g::RegionKind::Block) return false;
        for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i)
          if (!inline_[i]) return false;
        return voice || runCost(reg.firstOp, reg.firstOp + reg.numOps) <= kMaxGlobalChainBlockCost;
      };
      auto isLoop = [&](uint32_t q) { return s_.regions[q].kind == g::RegionKind::Loop; };
      uint32_t r = st.firstRegion;
      const uint32_t end = st.firstRegion + st.numRegions;
      while (r < end) {
        if (!chainable(r) || (!voice && !isLoop(r))) {
          ++r;
          continue;
        }
        uint32_t last = r, cost = 0, loops = 0, lastLoop = r;
        std::set<std::pair<int, uint32_t>> live;  // global chains: values held across the loop (kMaxGlobalChainLive)
        // TMM.6: a chain runs one frame count (an oversample region's regions chain only with each other).
        for (uint32_t q = r; q < end && chainable(q) && segmentOf(q) == segmentOf(r) &&
                             s_.regions[q].factor == s_.regions[r].factor;
             ++q) {
          const g::Region& reg = s_.regions[q];
          const uint32_t c = runCost(reg.firstOp, reg.firstOp + reg.numOps);
          if (q > r && cost + c > kMaxRunCost) break;
          if (!voice) {
            std::set<std::pair<int, uint32_t>> more = live;
            for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i) {
              const auto opIns = s_.inputs(s_.ops[i]);
              for (uint32_t k = 0; k < opIns.size(); ++k)
                if (opIns[k].kind != g::Operand::Kind::None && isPattern(opIns[k]) && !prologueOnly(i, k))
                  more.insert({static_cast<int>(opIns[k].kind), opIns[k].index});
              if (s_.ops[i].state != g::kNone) more.insert({-1, s_.ops[i].state});
            }
            if (q > r && more.size() > kMaxGlobalChainLive) break;
            live = std::move(more);
          }
          cost += c;
          if (isLoop(q)) {
            ++loops;
            lastLoop = q;
          }
          last = q;
        }
        if (!voice) last = lastLoop;  // a global chain ends with its last loop (trailing Block regions stay vectorised)
        if (last > r && loops >= (voice ? 1u : 2u)) {
          if (voice) chainCost_[sk] = std::max(chainCost_[sk], cost);
          chainEnd_[r] = static_cast<int32_t>(last);
          const int32_t id = next++;
          for (uint32_t q = r; q <= last; ++q)
            for (uint32_t i = s_.regions[q].firstOp; i < s_.regions[q].firstOp + s_.regions[q].numOps; ++i)
              if (grp_[i] >= 0) grp_[i] = id;
        }
        r = last + 1;
      }
    }
    // Store elimination. Accesses per buffer in op order (an op's reads before its writes).
    std::vector<bool> always(s_.buffers.size(), false);
    auto mark = [&](const g::Operand& o) {
      if (o.kind == g::Operand::Kind::Buffer && o.index < always.size()) always[o.index] = true;
    };
    for (const g::OutputRef& o : s_.outputs) mark(o.value);
    for (const g::ProbeRef& p : s_.probes) mark(p.value);  // T4.5: the host copies it after the stage
    for (const g::SolveUnknown& u : s_.unknowns) {
      mark(u.value);
      mark(u.init);
    }
    for (const g::SolveEquation& e : s_.equations) {
      mark(e.lhs);
      mark(e.rhs);
    }
    struct Access {
      uint32_t op;
      uint32_t operand;  // index into Schedule::operands
      bool write;
    };
    std::vector<std::vector<Access>> acc(s_.buffers.size());
    for (uint32_t i = 0; i < n; ++i) {
      const g::Op& op = s_.ops[i];
      for (uint32_t k = 0; k < op.numIn; ++k) {
        const g::Operand& o = s_.operands[op.firstOperand + k];
        if (o.kind == g::Operand::Kind::Buffer) acc[o.index].push_back({i, op.firstOperand + k, false});
      }
      for (uint32_t k = 0; k < op.numOut; ++k) {
        const g::Operand& o = s_.operands[op.firstOperand + op.numIn + k];
        if (o.kind == g::Operand::Kind::Buffer) acc[o.index].push_back({i, op.firstOperand + op.numIn + k, true});
      }
    }
    store_.assign(s_.operands.size(), true);
    for (uint32_t b = 0; b < acc.size(); ++b) {
      if (always[b]) continue;
      const auto& list = acc[b];
      for (size_t p = 0; p < list.size(); ++p) {
        if (!list[p].write) continue;
        const int32_t grp = grp_[list[p].op];
        if (grp < 0) continue;
        bool needed = false;
        for (size_t q = p + 1; q < list.size(); ++q) {
          if (list[q].write) break;
          if (grp_[list[q].op] != grp) {
            needed = true;
            break;
          }
        }
        store_[list[p].operand] = needed;
      }
    }
  }

  // ================================================================================================================
  // Function scaffolding and cached base values (all emitted in the entry block).
  // ================================================================================================================
  // One function per stage, all regions in it. (Splitting a stage into one function per region was measured: it
  // makes compiles slower, since every function repeats its base-pointer setup; bench/RESULTS.md.)
  // vw: the voice width of a voice stage's function (1 scalar lanes, 8 a group, 16 a pair); 0 for a global stage.
  void stageFunction(uint32_t k, const std::string& name, unsigned vw, bool rampPart) {
    const g::Stage& st = s_.stages[k];
    // The kernel-call pointer arrays are sized for this stage's calls only (an inline op never uses them): the
    // M1 review's 1200-input mux sized them at 2 × 9.6 KB for every stage, a frame that needs __chkstk probes.
    uint32_t maxOperands = 1;
    for (uint32_t r = st.firstRegion; r < st.firstRegion + st.numRegions; ++r)
      for (uint32_t i = s_.regions[r].firstOp; i < s_.regions[r].firstOp + s_.regions[r].numOps; ++i)
        if (!inline_[i] && !wideCall(s_.ops[i]))
          maxOperands = std::max<uint32_t>(maxOperands, std::max<uint32_t>(s_.ops[i].numIn, s_.ops[i].numOut));
    voice_ = vw != 0;
    vw_ = voice_ ? vw : 8u;
    gainTarget_.clear();
    local_ = vw == 16 ? stageLocalBuffers(s_, k) : std::vector<uint8_t>(s_.buffers.size(), 0);
    staged_.assign(s_.state.entries.size(), 0);
    if (vw == 16) {
      auto stage = [&](uint32_t e) {
        if (e != g::kNone && voiceArena(e)) staged_[e] = 1;
      };
      for (uint32_t r = st.firstRegion; r < st.firstRegion + st.numRegions; ++r) {
        const g::Region& reg = s_.regions[r];
        for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i)
          if (inline_[i]) stage(s_.ops[i].state);
        for (uint32_t q = reg.firstSolve; q < reg.firstSolve + reg.numSolves; ++q)
          for (uint32_t u = s_.solves[q].firstUnknown; u < s_.solves[q].firstUnknown + s_.solves[q].numUnknowns; ++u)
            stage(s_.unknowns[u].state);
      }
    }
    const uint32_t end = st.firstRegion + st.numRegions;
    // TX.smooth-fix1 (pq/graph/schedule.h "Param ramps"): the stage function is the stage's ramp-free code — the
    // schedule without its param_ramp regions, every ramped input its pattern, no ramp state touched — and the image
    // host calls `<name>_rampmode` instead in a block in which a static ramp of the stage may move (one group, or
    // global: a pair function is ramp-free code only). Ramps whose source is not static (Ramp::staticSource: it can
    // move with no param changing) are the exception: after the Control region the stage function checks those
    // (at rest: `active` cleared, as their kernel would) and, if one moves, ends in a tail call of `<name>_ramps`
    // (a pair function hands the block back instead when its Control region may run twice, emitRampBail).
    // `<name>_ramps` runs the param_ramp regions and the rest of the stage in ramp mode; `<name>_rampmode` is the
    // Control region and a tail call of it.
    uint32_t ctrlEnd = st.firstRegion;
    while (ctrlEnd < end && s_.regions[ctrlEnd].kind == g::RegionKind::Control) ++ctrlEnd;
    uint32_t split = ctrlEnd;
    while (split < end && isRampRegion(split)) ++split;
    std::vector<uint32_t> stageRamps, checked;  // every ramp whose op is in this stage; those of non-static sources
    if (split > ctrlEnd) {
      const uint32_t a = s_.regions[ctrlEnd].firstOp, b = s_.regions[split - 1].firstOp + s_.regions[split - 1].numOps;
      for (uint32_t q = 0; q < s_.ramps.size(); ++q)
        if (s_.ramps[q].op >= a && s_.ramps[q].op < b) {
          stageRamps.push_back(q);
          if (!s_.ramps[q].staticSource) checked.push_back(q);
        }
    }
    const std::vector<uint32_t> ramps = split < end ? unitRamps(split, end - 1) : std::vector<uint32_t>{};
    // A pair function stages its voice state after the dispatch when the Control region touches none of it.
    bool lateStage = vw == 16 && !checked.empty();
    for (uint32_t r = st.firstRegion; r < split && lateStage; ++r) {
      const g::Region& reg = s_.regions[r];
      for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i)
        if (inline_[i] && s_.ops[i].state != g::kNone && staged_[s_.ops[i].state]) lateStage = false;
      for (uint32_t q = reg.firstSolve; q < reg.firstSolve + reg.numSolves; ++q)
        for (uint32_t u = s_.solves[q].firstUnknown; u < s_.solves[q].firstUnknown + s_.solves[q].numUnknowns; ++u)
          if (s_.unknowns[u].state != g::kNone && staged_[s_.unknowns[u].state]) lateStage = false;
    }
    // The ramp-mode body: needed for the checked ramps' dispatch, and (one group / global) for the host's ramp mode.
    // run() emits it in a second pass (rampPart), after every ramp-free function.
    const bool hostRampMode = vw != 16 && !stageRamps.empty();
    const bool bail = vw == 16 && !checked.empty() && pairCanBail(st);
    const bool dispatch = !bail && !checked.empty();
    if (!rampPart) {
      beginFunction(name, llvm::Function::ExternalLinkage, maxOperands);
      if (vw == 16 && !lateStage) stagePairState(true);
      emitRegions(st.firstRegion, ctrlEnd);
      if (bail) emitRampBail(checked);
      else if (dispatch) emitRampDispatch(checked, name + "_ramps", lateStage);
      if (lateStage) stagePairState(true);
      steady_ = !stageRamps.empty();  // no ramp moves in the code below: no ramp handling
      emitRegions(split, end);
      steady_ = false;
      if (vw == 16) stagePairState(false);
      B_.CreateRetVoid();
    } else if (dispatch || hostRampMode) {  // the same regions in ramp mode
      llvm::Function* decl = dispatch ? M_.getFunction(name + "_ramps") : nullptr;  // the dispatch's declaration
      if (decl) decl->setName(name + "_ramps_decl");
      beginFunction(name + "_ramps", llvm::Function::InternalLinkage, maxOperands);
      llvm::Function* tail = F_;
      F_->addFnAttr(llvm::Attribute::NoInline);
      if (decl) {
        decl->replaceAllUsesWith(F_);
        decl->eraseFromParent();
      }
      gainTarget_.clear();
      if (vw == 16) stagePairState(true);
      emitRegions(ctrlEnd, split);  // the param_ramp ops
      const unsigned halves = vw == 16 ? 2u : 1u;
      unitHeld_.assign(s_.ramps.size(), 0);
      for (const uint32_t r : ramps) {
        for (unsigned h = 0; h < halves; ++h) emitRampHold(r, h);
        unitHeld_[r] = 1;
      }
      rampMode_ = true;
      emitRegions(split, end);
      rampMode_ = false;
      unitHeld_.assign(s_.ramps.size(), 0);
      if (vw == 16) stagePairState(false);
      B_.CreateRetVoid();
      if (hostRampMode) {  // the host's entry: the Control region, then the ramp-mode body
        beginFunction(name + "_rampmode", llvm::Function::ExternalLinkage, maxOperands);
        gainTarget_.clear();
        emitRegions(st.firstRegion, ctrlEnd);
        std::vector<Value*> args;
        for (llvm::Argument& a : F_->args()) args.push_back(&a);
        llvm::CallInst* call = B_.CreateCall(tail, args);
        call->setDoesNotThrow();
        call->setTailCall();
        B_.CreateRetVoid();
        res_.rampFunctions[k] = name + "_rampmode";
      }
    }
    voice_ = false;
    vw_ = 8;
  }

  // Regions [from, to) of the current stage: memo regions, chains, sleeping segments, the cost meter.
  void emitRegions(uint32_t from, uint32_t to) {
    for (uint32_t r = from; r < to; ++r) {
      Value* t0 = meterStart();  // T4.5: the region's (or chain's) time while a cost meter is shown
      const uint32_t first = r;
      if (!voice_ && memo_[r]) {
        emitMemo(r);
        meterStop(first, t0);
        continue;
      }
      const uint32_t last = chainEnd_[r] >= 0 ? static_cast<uint32_t>(chainEnd_[r]) : r;
      auto body = [&] {
        if (last > r) emitChain(r, last);
        else emitRegion(r);
      };
      if (!voice_ && r < s_.sleep.regions.size() && s_.sleep.regions[r].segment != g::kNone) emitSleeping(r, last, body);
      else body();
      r = last;
      meterStop(first, t0);
    }
  }
  bool isRampRegion(uint32_t r) const noexcept {
    const g::Region& reg = s_.regions[r];
    return reg.numOps > 0 && s_.ops[reg.firstOp].kernel == Atom::ParamRamp;
  }

  llvm::Function* beginFunction(const std::string& name, llvm::Function::LinkageTypes linkage, uint32_t maxOperands) {
    llvm::FunctionType* ty = llvm::FunctionType::get(
        void_, {ptr_, ptr_, ptr_, ptr_, ptr_, ptr_, ptr_, i32_, i32_, i32_, i32_}, false);
    F_ = llvm::Function::Create(ty, linkage, name, M_);
    for (unsigned a = 0; a < 7; ++a) F_->addParamAttr(a, llvm::Attribute::NoAlias);
    for (unsigned a : {1u, 5u, 6u}) F_->addParamAttr(a, llvm::Attribute::ReadOnly);
    F_->addFnAttr(llvm::Attribute::NoUnwind);
    F_->addFnAttr("no-builtins");  // no memset/memcpy calls the JIT would have to resolve
    if (!o_.cpu.empty()) F_->addFnAttr("target-cpu", o_.cpu);
    if (!o_.features.empty()) F_->addFnAttr("target-features", o_.features);
    Vx::kMasks() = o_.features.find("+avx512vl") != std::string::npos;  // comparisons as k-register predicates
    if (vw_ == 16 && o_.pairsZmm) {  // a pair's <16 x float> on one zmm register (LLVM prefers 256-bit vectors)
      F_->addFnAttr("prefer-vector-width", "512");
      F_->addFnAttr("min-legal-vector-width", "512");
    } else {
      // Every other function stays on 256-bit registers (AVX-512VL instructions, 32 registers and k-masks remain):
      // without this attribute LLVM legalises the phasor's <8 x double> onto zmm, and one 512-bit instruction in a
      // loop turns off the core's port-1 vector ALU for the whole loop (measured T1b.2: voice groups 12–15 % faster
      // with it; the same as with AVX-512 switched off in these functions, Q111).
      F_->addFnAttr("prefer-vector-width", "256");
      F_->addFnAttr("min-legal-vector-width", "256");
    }
    {
      llvm::AttrBuilder ab(C_);
      ab.addDenormalFPEnvAttr(llvm::DenormalFPEnv(llvm::DenormalMode::getPreserveSign()));  // FTZ/DAZ on audio threads
      F_->addFnAttrs(ab);
    }
    auto arg = F_->arg_begin();
    buf_ = &*arg++;
    aux_ = &*arg++;
    gstate_ = &*arg++;
    vstate_ = &*arg++;
    vstate1_ = &*arg++;
    ext_ = &*arg++;
    tabArg_ = &*arg++;
    Value* frames32 = &*arg++;
    group32_ = &*arg++;
    Value* group1_32 = &*arg++;
    Value* lane32 = &*arg;
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(C_, "entry", F_);
    llvm::BasicBlock* start = llvm::BasicBlock::Create(C_, "start", F_);
    B_.SetInsertPoint(entry);
    entryTerm_ = B_.CreateBr(start);
    B_.SetInsertPoint(start);
    B_.setFastMathFlags(fmf_);
    bufBase_.clear();
    framesF_.clear();
    extPtr_.clear();
    tabLoad_.clear();
    statePtr_.clear();
    sideBase_[0] = sideBase_[1] = sideBase_[2] = nullptr;
    stageSlot_.clear();
    inv_.clear();
    hoistIP_ = nullptr;
    {
      llvm::IRBuilder<> eb(entryTerm_);
      frames64_ = eb.CreateZExt(frames32, i64_);
      group64_ = eb.CreateZExt(group32_, i64_);
      group1_64_ = eb.CreateZExt(group1_32, i64_);
      lane64_ = eb.CreateZExt(lane32, i64_);
      meterOn_ = eb.CreateICmpNE(eb.CreateAlignedLoad(i32_, eb.CreateConstInBoundsGEP1_64(f32_, aux_, L_.meterFlagOff),
                                                      llvm::Align(4)),
                                 c32(0));
      kargs_ = eb.CreateAlloca(kargsTy_);
      inArr_ = eb.CreateAlloca(llvm::ArrayType::get(ptr_, maxOperands));
      outArr_ = eb.CreateAlloca(llvm::ArrayType::get(ptr_, maxOperands));
      slot_ = eb.CreateAlloca(llvm::ArrayType::get(f32_, 16));
      slot_->setAlignment(llvm::Align(32));
    }
    return F_;
  }

  // T4.5 cost meter (image_host.h): behind the flag the host raises for a metered block (loaded once per call), the TSC
  // before a region and its cycles added to the region's uint64 counter in buf after it. Unmetered: one branch each.
  Value* meterStart() {
    llvm::BasicBlock* from = B_.GetInsertBlock();
    llvm::BasicBlock* on = llvm::BasicBlock::Create(C_, "meter.start", F_);
    llvm::BasicBlock* cont = llvm::BasicBlock::Create(C_, "meter.started", F_);
    B_.CreateCondBr(meterOn_, on, cont);
    B_.SetInsertPoint(on);
    Value* t = B_.CreateIntrinsic(i64_, llvm::Intrinsic::readcyclecounter, llvm::ArrayRef<Value*>{});
    B_.CreateBr(cont);
    B_.SetInsertPoint(cont);
    llvm::PHINode* phi = B_.CreatePHI(i64_, 2);
    phi->addIncoming(c64(0), from);
    phi->addIncoming(t, on);
    return phi;
  }
  void meterStop(uint32_t region, Value* t0) {
    llvm::BasicBlock* on = llvm::BasicBlock::Create(C_, "meter.stop", F_);
    llvm::BasicBlock* cont = llvm::BasicBlock::Create(C_, "meter.stopped", F_);
    B_.CreateCondBr(meterOn_, on, cont);
    B_.SetInsertPoint(on);
    Value* t1 = B_.CreateIntrinsic(i64_, llvm::Intrinsic::readcyclecounter, llvm::ArrayRef<Value*>{});
    Value* p = B_.CreateConstInBoundsGEP1_64(i64_, B_.CreateConstInBoundsGEP1_64(f32_, buf_, L_.meterOff), 2 * uint64_t{region});
    Value* old = B_.CreateAlignedLoad(i64_, p, llvm::Align(8));
    B_.CreateAlignedStore(B_.CreateAdd(old, B_.CreateSub(t1, t0)), p, llvm::Align(8));
    Value* q = B_.CreateConstInBoundsGEP1_64(i64_, p, 1);  // how often it was timed (the meter subtracts the TSC reads)
    B_.CreateAlignedStore(B_.CreateAdd(B_.CreateAlignedLoad(i64_, q, llvm::Align(8)), c64(1)), q, llvm::Align(8));
    B_.CreateBr(cont);
    B_.SetInsertPoint(cont);
  }

  llvm::Constant* c64(uint64_t v) const { return llvm::ConstantInt::get(i64_, v); }
  llvm::Constant* c32(uint32_t v) const { return llvm::ConstantInt::get(i32_, v); }

  // A buffer's base: buf + a constant, emitted where it is used (`at`, default the current insert point), so it folds
  // into the instruction's addressing mode. (Computed once in the entry block, every buffer's base was a value live
  // across the whole function: with ~1200 buffers they were spilled, the frame needed stack probes and register
  // allocation took seconds.) A per-group buffer's base needs the group index: computed once, in the entry block.
  // h: the half of a pair (0: the function's group, 1: its second group; codegen.h). A stage-local voice value's
  // second half lives in its HostLayout::altOff copy.
  Value* bufBase(uint32_t b, llvm::IRBuilderBase* at = nullptr, unsigned h = 0) {
    if (!s_.buffers[b].perGroup)
      return (at ? *at : B_).CreateConstInBoundsGEP1_64(f32_, buf_, bufOffset(b, h));
    auto it = bufBase_.find(b * 2 + h);
    if (it != bufBase_.end()) return it->second;
    llvm::IRBuilder<> eb(entryTerm_);
    Value* p = eb.CreateConstInBoundsGEP1_64(f32_, buf_, L_.bufOff[b]);
    p = eb.CreateInBoundsGEP(f32_, p, eb.CreateMul(h ? group1_64_ : group64_, c64(L_.bufSize[b])));
    bufBase_.emplace(b * 2 + h, p);
    return p;
  }
  // Float offset of a buffer that is not per-group, for half h.
  uint64_t bufOffset(uint32_t b, unsigned h) const noexcept {
    return h == 1 && local_[b] && L_.altOff[b] != g::kNone ? L_.altOff[b] : L_.bufOff[b];
  }
  Value* extBase(uint32_t e, unsigned h = 0) {
    const bool perGroup = s_.externals[e].poly == g::Poly::Voice;
    if (!perGroup) h = 0;
    auto it = extPtr_.find(e * 2 + h);
    if (it != extPtr_.end()) return it->second;
    llvm::IRBuilder<> eb(entryTerm_);
    Value* idx = c64(static_cast<uint64_t>(e) * L_.groups);
    if (perGroup) idx = eb.CreateAdd(idx, h ? group1_64_ : group64_);
    Value* p = eb.CreateLoad(ptr_, eb.CreateInBoundsGEP(ptr_, ext_, idx));
    extPtr_.emplace(e * 2 + h, p);
    return p;
  }
  Value* tabLoad(const void* p) {
    const uint32_t slot = tab_.slot(p);
    return tabSlot(slot);
  }
  Value* tabSlot(uint32_t slot) {
    auto it = tabLoad_.find(slot);
    if (it != tabLoad_.end()) return it->second;
    llvm::IRBuilder<> eb(entryTerm_);
    Value* v = eb.CreateLoad(ptr_, eb.CreateConstInBoundsGEP1_64(ptr_, tabArg_, slot));
    tabLoad_.emplace(slot, v);
    return v;
  }
  Value* statePtr(uint32_t entry, unsigned h = 0) {
    const g::StateEntry& e = s_.state.entries[entry];
    if (e.arena != g::Poly::Voice) h = 0;
    auto it = statePtr_.find(entry * 2 + h);
    if (it != statePtr_.end()) return it->second;
    llvm::IRBuilder<> eb(entryTerm_);
    Value* p = eb.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), arenaBase(e, h), e.offset);
    statePtr_.emplace(entry * 2 + h, p);
    return p;
  }
  // The base an entry's offset is from: its arena argument, or (TX.smooth-fix1, StateEntry::side) the ramp side
  // table's part (global, or the group's block: the table slot plus group · stride, computed once in the entry block).
  Value* arenaBase(const g::StateEntry& e, unsigned h) {
    const bool voice = e.arena == g::Poly::Voice;
    if (!e.side) return voice ? (h ? vstate1_ : vstate_) : gstate_;
    const unsigned key = voice ? 1u + h : 0u;
    if (sideBase_[key]) return sideBase_[key];
    llvm::IRBuilder<> eb(entryTerm_);
    Value* p = eb.CreateLoad(ptr_, eb.CreateConstInBoundsGEP1_64(ptr_, tabArg_, voice ? L_.sideSlotVoice : L_.sideSlotGlobal));
    if (voice) p = eb.CreateInBoundsGEP(llvm::Type::getInt8Ty(C_), p, eb.CreateMul(h ? group1_64_ : group64_, c64(L_.sideVoiceStride)));
    sideBase_[key] = p;
    return p;
  }
  bool voiceArena(uint32_t entry) const noexcept { return s_.state.entries[entry].arena == g::Poly::Voice; }
  // TX.smooth: a state entry's address computed where it is used (a constant offset from the arena argument, folded
  // into the addressing mode), not cached in the entry block: a stage reads ~80 param ramps' states (the Operator), and
  // as entry-block values their pointers were live across the stage, spilled, and pushed the pair frame past 4 KiB.
  Value* statePtrHere(uint32_t entry, unsigned h) {
    const g::StateEntry& e = s_.state.entries[entry];
    if (e.arena != g::Poly::Voice) h = 0;
    return B_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), arenaBase(e, h), e.offset);
  }

  // ---- voice-stage operands (T1b.2): the 8 floats of operand o at frame t for half h; plus the lane in the
  // scalar baseline. Patterns are not offset by t.
  Value* voicePtr(const g::Operand& o, Value* t, unsigned h, llvm::IRBuilderBase& b) {
    Value* idx = nullptr;
    auto* ct = llvm::dyn_cast<llvm::ConstantInt>(t);
    if (!isPattern(o) && !(ct && ct->isZero())) idx = b.CreateMul(t, c64(lanesOf(o)), "", true, true);
    if (vw_ == 1) idx = idx ? b.CreateAdd(idx, lane64_, "", true, true) : lane64_;
    if (o.kind == g::Operand::Kind::Buffer && !s_.buffers[o.index].perGroup) {
      const uint64_t off = bufOffset(o.index, h);
      if (!idx) return b.CreateConstInBoundsGEP1_64(f32_, buf_, off);
      return b.CreateInBoundsGEP(f32_, buf_, b.CreateAdd(idx, c64(off), "", true, true));  // see framePtr
    }
    Value* base = o.kind == g::Operand::Kind::Buffer     ? bufBase(o.index, &b, h)
                  : o.kind == g::Operand::Kind::External ? extBase(o.index, h)
                                                         : basePtr(o, &b);
    return idx ? b.CreateInBoundsGEP(f32_, base, idx) : base;
  }
  // Whether both halves of a pair read the same memory for o (params, constants, global values).
  bool sharedByHalves(const g::Operand& o) const noexcept {
    switch (o.kind) {
      case g::Operand::Kind::Buffer: return !s_.buffers[o.index].perGroup && !local_[o.index];
      case g::Operand::Kind::External: return s_.externals[o.index].poly != g::Poly::Voice;
      default: return true;
    }
  }
  llvm::Align voiceAlign(const g::Operand& o) const {
    if (vw_ == 1 || o.kind == g::Operand::Kind::External) return llvm::Align(4);
    return llvm::Align(32);
  }
  Value* concat(Value* lo, Value* hi) {
    llvm::SmallVector<int, 16> idx;
    for (int k = 0; k < 16; ++k) idx.push_back(k);
    return B_.CreateShuffleVector(lo, hi, idx);
  }
  Value* half(Value* v, unsigned h) {
    llvm::SmallVector<int, 8> idx;
    for (int k = 0; k < 8; ++k) idx.push_back(static_cast<int>(h) * 8 + k);
    return B_.CreateShuffleVector(v, idx);
  }
  Value* voiceLoad(const g::Operand& o, Value* t, llvm::IRBuilderBase& b, llvm::Type* elem = nullptr) {
    if (!elem) elem = f32_;
    if (vw_ == 1) return b.CreateAlignedLoad(elem, voicePtr(o, t, 0, b), llvm::Align(4));
    llvm::Type* v8 = llvm::FixedVectorType::get(elem, 8);
    Value* lo = b.CreateAlignedLoad(v8, voicePtr(o, t, 0, b), voiceAlign(o));
    if (vw_ == 8) return lo;
    Value* hi = sharedByHalves(o) ? lo : b.CreateAlignedLoad(v8, voicePtr(o, t, 1, b), voiceAlign(o));
    llvm::SmallVector<int, 16> idx;
    for (int k = 0; k < 16; ++k) idx.push_back(k);
    return b.CreateShuffleVector(lo, hi, idx);
  }
  void voiceStore(Value* val, const g::Operand& o, Value* t) {
    if (vw_ != 16) {
      B_.CreateAlignedStore(val, voicePtr(o, t, 0, B_), voiceAlign(o));
      return;
    }
    B_.CreateAlignedStore(half(val, 0), voicePtr(o, t, 0, B_), voiceAlign(o));
    B_.CreateAlignedStore(half(val, 1), voicePtr(o, t, 1, B_), voiceAlign(o));
  }
  // Pair functions stage the voice state of the stage's inline stateful ops (and solve guesses): each 32-byte lane
  // array of the two groups is one 16-lane value in a stack slot, filled at entry from both groups' blocks (in) and
  // written back at exit. The slots cannot alias the buffers (the groups' blocks could, for LLVM, not be told apart
  // from each other's halves), so SROA turns them into registers and a loop carries each state value in one
  // register: without staging, LICM kept every state value as two 256-bit halves, joined and split on every frame
  // (on the feedback path), measured 1.2× slower on osc_filter ×64.
  Value* stagedPtr(uint32_t entry, uint32_t off) {
    auto it = stageSlot_.find(static_cast<uint64_t>(entry) << 32 | off);
    if (it != stageSlot_.end()) return it->second;
    llvm::IRBuilder<> eb(entryTerm_);
    llvm::AllocaInst* a = eb.CreateAlloca(llvm::FixedVectorType::get(i32_, 16));
    a->setAlignment(llvm::Align(64));
    stageSlot_.emplace(static_cast<uint64_t>(entry) << 32 | off, a);
    return a;
  }
  void stagePairState(bool in) {
    llvm::Type* i8 = llvm::Type::getInt8Ty(C_);
    llvm::Type* v8 = llvm::FixedVectorType::get(i32_, 8);
    llvm::Type* v16 = llvm::FixedVectorType::get(i32_, 16);
    for (uint32_t e = 0; e < staged_.size(); ++e) {
      if (!staged_[e]) continue;
      const uint32_t size = (s_.state.entries[e].size + 31) / 32 * 32;
      for (uint32_t off = 0; off < size; off += 32) {
        Value* g0 = B_.CreateConstInBoundsGEP1_64(i8, statePtr(e, 0), off);
        Value* g1 = B_.CreateConstInBoundsGEP1_64(i8, statePtr(e, 1), off);
        if (in) {
          Value* lo = B_.CreateAlignedLoad(v8, g0, llvm::Align(32));
          Value* hi = B_.CreateAlignedLoad(v8, g1, llvm::Align(32));
          B_.CreateAlignedStore(concat(lo, hi), stagedPtr(e, off), llvm::Align(64));
        } else {
          Value* x = B_.CreateAlignedLoad(v16, stagedPtr(e, off), llvm::Align(64));
          B_.CreateAlignedStore(half(x, 0), g0, llvm::Align(32));
          B_.CreateAlignedStore(half(x, 1), g1, llvm::Align(32));
        }
      }
    }
  }

  // A state array (8 lanes × 4 bytes at byte offset off of a state entry), as a value of this width: the
  // global arena's (Control regions of global stages: 8 lanes; per-sample loops: 1), or a voice entry's per half
  // (and at the lane, scalar baseline).
  Value* ldState(uint32_t entry, uint32_t off, Vx& v, bool asInt) {
    llvm::Type* i8 = llvm::Type::getInt8Ty(C_);
    llvm::Type* elem = asInt ? i32_ : f32_;
    if (!voice_ || !voiceArena(entry))
      return B_.CreateAlignedLoad(asInt ? v.iT() : v.fT(), B_.CreateConstInBoundsGEP1_64(i8, statePtr(entry), off),
                                  llvm::Align(v.width() == 8 ? 32 : 4));
    if (vw_ == 1) {
      Value* p = B_.CreateConstInBoundsGEP1_64(i8, statePtr(entry), off);
      return B_.CreateAlignedLoad(elem, B_.CreateInBoundsGEP(elem, p, lane64_), llvm::Align(4));
    }
    if (vw_ == 16 && staged_[entry]) return B_.CreateAlignedLoad(asInt ? v.iT() : v.fT(), stagedPtr(entry, off), llvm::Align(64));
    llvm::Type* v8 = llvm::FixedVectorType::get(elem, 8);
    Value* lo = B_.CreateAlignedLoad(v8, B_.CreateConstInBoundsGEP1_64(i8, statePtr(entry, 0), off), llvm::Align(32));
    if (vw_ == 8) return lo;
    Value* hi = B_.CreateAlignedLoad(v8, B_.CreateConstInBoundsGEP1_64(i8, statePtr(entry, 1), off), llvm::Align(32));
    return concat(lo, hi);
  }
  void stState(uint32_t entry, uint32_t off, Value* x, Vx& v) {
    llvm::Type* i8 = llvm::Type::getInt8Ty(C_);
    if (!voice_ || !voiceArena(entry)) {
      B_.CreateAlignedStore(x, B_.CreateConstInBoundsGEP1_64(i8, statePtr(entry), off), llvm::Align(v.width() == 8 ? 32 : 4));
      return;
    }
    if (vw_ == 1) {
      Value* p = B_.CreateConstInBoundsGEP1_64(i8, statePtr(entry), off);
      B_.CreateAlignedStore(x, B_.CreateInBoundsGEP(x->getType(), p, lane64_), llvm::Align(4));
      return;
    }
    if (vw_ == 8) {
      B_.CreateAlignedStore(x, B_.CreateConstInBoundsGEP1_64(i8, statePtr(entry, 0), off), llvm::Align(32));
      return;
    }
    if (staged_[entry]) {
      B_.CreateAlignedStore(x, stagedPtr(entry, off), llvm::Align(64));
      return;
    }
    B_.CreateAlignedStore(half(x, 0), B_.CreateConstInBoundsGEP1_64(i8, statePtr(entry, 0), off), llvm::Align(32));
    B_.CreateAlignedStore(half(x, 1), B_.CreateConstInBoundsGEP1_64(i8, statePtr(entry, 1), off), llvm::Align(32));
  }

  bool isPattern(const g::Operand& o) const noexcept { return isPatternOperand(s_, o); }
  uint32_t lanesOf(const g::Operand& o) const noexcept { return lanesOfOperand(s_, o); }

  // Start of the operand's data: a block buffer's frame 0 or a pattern.
  Value* basePtr(const g::Operand& o, llvm::IRBuilderBase* at = nullptr) {
    llvm::IRBuilderBase& b = at ? *at : B_;
    switch (o.kind) {
      case g::Operand::Kind::Buffer: return bufBase(o.index, at);
      case g::Operand::Kind::Param: return b.CreateConstInBoundsGEP1_64(f32_, aux_, L_.paramOff + static_cast<uint64_t>(o.index) * 8);
      case g::Operand::Kind::Const: return b.CreateConstInBoundsGEP1_64(f32_, aux_, L_.constOff + static_cast<uint64_t>(o.index) * 8);
      case g::Operand::Kind::External: return extBase(o.index);
      default: return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr_));
    }
  }
  // The operand's data at frame t (patterns: the pattern).
  Value* framePtr(const g::Operand& o, Value* t) {
    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(t); isPattern(o) || (c && c->isZero())) return basePtr(o);
    Value* idx = B_.CreateMul(t, c64(lanesOf(o)), "", true, true);
    // A buffer's frame t as buf + (t·lanes + offset): one loop-variant address, so LICM cannot hoist a base pointer
    // per buffer out of the loop (hundreds of them live across a long fused run spill; codegen.h).
    if (o.kind == g::Operand::Kind::Buffer && !s_.buffers[o.index].perGroup)
      return B_.CreateInBoundsGEP(f32_, buf_, B_.CreateAdd(idx, c64(L_.bufOff[o.index]), "", true, true));
    return B_.CreateInBoundsGEP(f32_, basePtr(o), idx);
  }
  llvm::Align alignOf(const g::Operand& o, unsigned w) const {
    if (w == 1) return llvm::Align(4);
    if (o.kind == g::Operand::Kind::External) return llvm::Align(4);  // engine buffers: no promise
    if (isPattern(o)) return llvm::Align(32);
    return lanesOf(o) == 8 ? llvm::Align(32) : llvm::Align(4);  // 1-lane: 8 frames from any frame
  }
  llvm::Type* vecTy(unsigned w) const {
    return w == 1 ? f32_ : static_cast<llvm::Type*>(llvm::FixedVectorType::get(f32_, w));
  }

  // ================================================================================================================
  // Operand access.
  // ================================================================================================================
  // TX.smooth (pq/graph/schedule.h "Param ramps"): the ramp of op input operand `o` (a reference into
  // s_.operands), or nullptr.
  uint32_t rampIndexOf(const g::Operand& o) const noexcept {
    if (s_.rampOf.empty() || &o < s_.operands.data() || &o >= s_.operands.data() + s_.operands.size()) return g::kNone;
    return s_.rampOf[static_cast<size_t>(&o - s_.operands.data())];
  }
  // In a ramp-mode copy, the block a ramped input reads instead of its pattern.
  const g::Operand* rampedOperand(const g::Operand& o) const noexcept {
    if (!rampMode_) return nullptr;
    const uint32_t r = rampIndexOf(o);
    return r == g::kNone ? nullptr : &s_.ramps[r].out;
  }
  // i1: ramp r wrote its block this block (ParamRampState::active), for half h.
  Value* rampActive(uint32_t r, unsigned h) {
    Value* q = B_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), statePtrHere(s_.ops[s_.ramps[r].op].state, h),
                                             offsetof(dsp::ParamRampState, active));
    return B_.CreateICmpNE(B_.CreateAlignedLoad(i32_, q, llvm::Align(4)), c32(0));
  }
  // Unless ramp r moved this block (half h): fill its block with its value (dsp::paramRampHold).
  void emitRampHold(uint32_t r, unsigned h) {
    const g::Ramp& rp = s_.ramps[r];
    const g::Buffer& buf = s_.buffers[rp.out.index];
    llvm::BasicBlock* hold = llvm::BasicBlock::Create(C_, "ramp_hold", F_);
    llvm::BasicBlock* next = llvm::BasicBlock::Create(C_, "ramp_held", F_);
    B_.CreateCondBr(rampActive(r, h), next, hold);
    B_.SetInsertPoint(hold);
    Value* n = B_.CreateTrunc(B_.CreateMul(frames64_, c64(buf.factor), "", true, true), i32_);
    Value* out = voice_ ? voicePtr(rp.out, c64(0), h, B_) : framePtr(rp.out, c64(0));
    llvm::FunctionType* holdTy = llvm::FunctionType::get(void_, {ptr_, ptr_, i32_, i32_}, false);
    callPtr(holdTy, tabLoad(reinterpret_cast<const void*>(&dsp::paramRampHold)),
            {statePtrHere(s_.ops[rp.op].state, h), out, n, c32(buf.lanes)});
    B_.CreateBr(next);
    B_.SetInsertPoint(next);
  }
  // The ramps that inline code or calls in loops of regions r0..r1 read (block-wise calls choose for themselves,
  // emitCallHalf).
  std::vector<uint32_t> unitRamps(uint32_t r0, uint32_t r1) const {
    std::vector<uint32_t> out;
    if (s_.rampOf.empty()) return out;
    for (uint32_t r = r0; r <= r1; ++r)
      for (uint32_t i = s_.regions[r].firstOp; i < s_.regions[r].firstOp + s_.regions[r].numOps; ++i) {
        // Inline code reads ramps in place; calls in a loop run per frame or sub-block, so their holds go to the unit.
        if (!inline_[i] && s_.regions[r].kind != g::RegionKind::Loop) continue;
        const g::Op& op = s_.ops[i];
        for (uint32_t j = 0; j < op.numIn; ++j) {
          const uint32_t k = s_.rampOf[op.firstOperand + j];
          if (k != g::kNone && std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
        }
      }
    return out;
  }
  // A unit (a region or chain) that reads ramps, behind one branch per block: no ramp moves → `body` as scheduled
  // (every Spl input a hoisted pattern); one moves → `body` again in ramp mode, where every ramped input is read from
  // its ramp's block (a resting ramp's block filled with its value first, dsp::paramRampHold) and each kernel call
  // with ramped inputs picks, by its own ramps, the kernel for its Spl mask without them (what the interpreter does).
  // TX.smooth: one branch per block on whether any of `ramps` (TX.smooth-fix1: the stage's checked ramps) moved
  // (either half of a pair); if so, the rest of the stage runs in the ramp-mode function (declared here, defined by
  // stageFunction) and the stage function returns (a pair function writes its staged voice state back first: the
  // callee stages its own).
  llvm::Function* emitRampDispatch(const std::vector<uint32_t>& ramps, const std::string& name, bool unstaged) {
    Value* any = emitRampsMoving(ramps);
    llvm::BasicBlock* moving = llvm::BasicBlock::Create(C_, "ramps_moving", F_);
    llvm::BasicBlock* steady = llvm::BasicBlock::Create(C_, "ramps_steady", F_);
    llvm::MDBuilder md(C_);
    B_.CreateCondBr(any, moving, steady, md.createBranchWeights(1, 63));
    B_.SetInsertPoint(moving);
    llvm::Function* fn = llvm::Function::Create(F_->getFunctionType(), llvm::Function::ExternalLinkage, name, M_);
    std::vector<Value*> args;
    for (llvm::Argument& a : F_->args()) args.push_back(&a);
    if (voice_ && vw_ == 16 && !unstaged) stagePairState(false);
    llvm::CallInst* call = B_.CreateCall(fn, args);
    call->setDoesNotThrow();
    B_.CreateRetVoid();
    B_.SetInsertPoint(steady);
    return fn;
  }
  // i1: some ramp of `ramps` (either half of a pair) is not at rest (each check clears its `active`).
  Value* emitRampsMoving(const std::vector<uint32_t>& ramps) {
    const unsigned halves = voice_ && vw_ == 16 ? 2u : 1u;
    Value* any = nullptr;
    for (const uint32_t r : ramps)
      for (unsigned h = 0; h < halves; ++h) {
        Value* a = B_.CreateNot(rampAtRest(r, h));
        any = any ? B_.CreateOr(any, a) : a;
      }
    return any ? any : static_cast<Value*>(llvm::ConstantInt::getFalse(C_));
  }
  // A pair function's ramp check (pairCanBail): a checked ramp of either group not at rest → the bail word
  // (jit_image.h, HostLayout::memoOff + 1) set and return; the host runs the two groups one by one.
  void emitRampBail(const std::vector<uint32_t>& ramps) {
    Value* any = emitRampsMoving(ramps);
    llvm::BasicBlock* moving = llvm::BasicBlock::Create(C_, "ramps_bail", F_);
    llvm::BasicBlock* steady = llvm::BasicBlock::Create(C_, "ramps_steady", F_);
    llvm::MDBuilder md(C_);
    B_.CreateCondBr(any, moving, steady, md.createBranchWeights(1, 63));
    B_.SetInsertPoint(moving);
    B_.CreateAlignedStore(c32(1), B_.CreateConstInBoundsGEP1_64(f32_, buf_, L_.memoOff + 1), llvm::Align(4));
    B_.CreateRetVoid();
    B_.SetInsertPoint(steady);
  }
  Value* loadPattern(const g::Operand& o, unsigned w, bool hoist) {
    const auto key = std::make_tuple(static_cast<int>(o.kind), o.index, w);
    if (hoist && hoistIP_) {
      auto it = inv_.find(key);
      if (it != inv_.end()) return it->second;
      llvm::IRBuilder<> hb(hoistIP_);
      Value* v = voice_ ? voiceLoad(o, c64(0), hb) : hb.CreateAlignedLoad(vecTy(w), basePtr(o, &hb), alignOf(o, w));
      inv_.emplace(key, v);
      return v;
    }
    if (voice_) return voiceLoad(o, c64(0), B_);
    return B_.CreateAlignedLoad(vecTy(w), basePtr(o), alignOf(o, w));
  }

  Value* read(const g::Operand& o, const Frame& fr) {
    if (const g::Operand* ro = rampedOperand(o)) return read(*ro, fr);  // TX.smooth: the ramp's block
    Vx v(B_, fr.w);
    switch (o.kind) {
      case g::Operand::Kind::Const: return v.cbits(std::bit_cast<uint32_t>(s_.constants[o.index].value));
      case g::Operand::Kind::Buffer: {
        if (fr.map) {
          auto it = fr.map->find(o.index);
          if (it != fr.map->end()) return it->second;
        }
        if (isPattern(o)) return loadPattern(o, fr.w, fr.hoist);
        Value* x = loadBlk(o, fr);
        // Loaded once per evaluation: every write to a buffer in this evaluation goes through write() (which
        // updates the map) or a call / solve (which erase what they write).
        if (fr.map) (*fr.map)[o.index] = x;
        return x;
      }
      case g::Operand::Kind::Param: return loadPattern(o, fr.w, fr.hoist);
      case g::Operand::Kind::External:
        if (isPattern(o)) return loadPattern(o, fr.w, fr.hoist);
        return loadBlk(o, fr);
      default: return v.zero();
    }
  }

  // A block operand's frames (masked: frames past the slice are neither read nor written; their lanes compute
  // harmless garbage that is never stored).
  Value* loadBlk(const g::Operand& o, const Frame& fr) {
    if (voice_) return voiceLoad(o, fr.t, B_);
    if (fr.mask)
      return B_.CreateMaskedLoad(vecTy(fr.w), framePtr(o, fr.t), alignOf(o, fr.w), fr.mask, Vx(B_, fr.w).zero());
    return B_.CreateAlignedLoad(vecTy(fr.w), framePtr(o, fr.t), alignOf(o, fr.w));
  }

  void write(uint32_t opIndex, uint32_t k, Value* val, const Frame& fr) {
    const g::Op& op = s_.ops[opIndex];
    const uint32_t operand = op.firstOperand + op.numIn + k;
    const g::Operand& o = s_.operands[operand];
    if (fr.map) (*fr.map)[o.index] = val;
    if (!store_[operand]) return;
    if (voice_) {
      voiceStore(val, o, fr.t);
      return;
    }
    if (fr.mask && !isPattern(o)) B_.CreateMaskedStore(val, framePtr(o, fr.t), alignOf(o, fr.w), fr.mask);
    else B_.CreateAlignedStore(val, framePtr(o, fr.t), alignOf(o, fr.w));
  }

  // ================================================================================================================
  // Calls: dsp kernels and runtime helpers, through the pointer table.
  // ================================================================================================================
  llvm::CallInst* callPtr(llvm::FunctionType* ty, Value* fn, llvm::ArrayRef<Value*> args) {
    llvm::CallInst* c = B_.CreateCall(ty, fn, args);
    c->setDoesNotThrow();
    return c;
  }

  // Op i as a kernel/helper call on frames [t0, t0 + n): once, or in a pair function once per half (each group
  // with its own buffers, state and reducer record).
  void emitCall(uint32_t i, Value* t0, Value* n32) {
    for (unsigned h = 0; h < (vw_ == 16 ? 2u : 1u); ++h) emitCallHalf(i, t0, n32, h);
  }
  void emitCallHalf(uint32_t i, Value* t0, Value* n32, unsigned h) {
    const g::Op& op = s_.ops[i];
    if (op.kind == g::OpKind::Kernel && op.kernel == Atom::ParamRamp && op.state != g::kNone) {
      emitRampOp(i, t0, n32, h);
      return;
    }
    emitCallHalfBody(i, t0, n32, h);
  }
  // TX.smooth: a param_ramp at rest (every lane started, its input bitwise its target, no frames left) only clears its
  // `active` word; checked inline, so a block in which nothing moves costs no call (kernels_ramp.cpp: the call would
  // do exactly that).
  // i1: ramp r (half h) at rest; clears its `active` word either way (a ramp that is not at rest gets its kernel).
  Value* rampAtRest(uint32_t r, unsigned h) {
    const g::Op& op = s_.ops[s_.ramps[r].op];
    const unsigned L = op.lanes == 8 ? 8u : 1u;
    llvm::Type* i8 = llvm::Type::getInt8Ty(C_);
    Value* st = statePtrHere(op.state, h);
    llvm::Type* fv = L == 1 ? static_cast<llvm::Type*>(f32_) : llvm::FixedVectorType::get(f32_, L);
    llvm::Type* iv = L == 1 ? static_cast<llvm::Type*>(i32_) : llvm::FixedVectorType::get(i32_, L);
    const g::Operand& src = s_.inputs(op)[0];
    Value* xp = voice_ ? voicePtr(src, c64(0), h, B_) : basePtr(src);
    Value* x = B_.CreateAlignedLoad(iv, xp, llvm::Align(4));
    auto field = [&](size_t off, llvm::Type* t) {
      return B_.CreateAlignedLoad(t, B_.CreateConstInBoundsGEP1_64(i8, st, off), llvm::Align(4));
    };
    Value* same = B_.CreateICmpEQ(x, field(offsetof(dsp::ParamRampState, target), iv));
    Value* rest = B_.CreateFCmpOEQ(field(offsetof(dsp::ParamRampState, rem), fv), llvm::Constant::getNullValue(fv));
    Value* started = B_.CreateFCmpUNE(field(offsetof(dsp::ParamRampState, started), fv), llvm::Constant::getNullValue(fv));
    Value* ok = B_.CreateAnd(B_.CreateAnd(same, rest), started);
    if (L > 1) ok = B_.CreateAndReduce(ok);
    B_.CreateAlignedStore(c32(0), B_.CreateConstInBoundsGEP1_64(i8, st, offsetof(dsp::ParamRampState, active)), llvm::Align(4));
    return ok;
  }
  void emitRampOp(uint32_t i, Value* t0, Value* n32, unsigned h) {
    const g::Op& op = s_.ops[i];
    const unsigned L = op.lanes == 8 ? 8u : 1u;
    llvm::Type* i8 = llvm::Type::getInt8Ty(C_);
    Value* st = statePtrHere(op.state, h);
    llvm::Type* fv = L == 1 ? static_cast<llvm::Type*>(f32_) : llvm::FixedVectorType::get(f32_, L);
    llvm::Type* iv = L == 1 ? static_cast<llvm::Type*>(i32_) : llvm::FixedVectorType::get(i32_, L);
    const g::Operand& src = s_.inputs(op)[0];
    Value* xp = voice_ ? voicePtr(src, c64(0), h, B_) : basePtr(src);
    Value* x = B_.CreateAlignedLoad(iv, xp, llvm::Align(4));
    auto field = [&](size_t off, llvm::Type* t) {
      return B_.CreateAlignedLoad(t, B_.CreateConstInBoundsGEP1_64(i8, st, off), llvm::Align(4));
    };
    Value* same = B_.CreateICmpEQ(x, field(offsetof(dsp::ParamRampState, target), iv));
    Value* rest = B_.CreateFCmpOEQ(field(offsetof(dsp::ParamRampState, rem), fv), llvm::Constant::getNullValue(fv));
    Value* started = B_.CreateFCmpUNE(field(offsetof(dsp::ParamRampState, started), fv), llvm::Constant::getNullValue(fv));
    Value* ok = B_.CreateAnd(B_.CreateAnd(same, rest), started);
    if (L > 1) ok = B_.CreateAndReduce(ok);
    llvm::BasicBlock* call = llvm::BasicBlock::Create(C_, "ramp_call", F_);
    llvm::BasicBlock* idle = llvm::BasicBlock::Create(C_, "ramp_rest", F_);
    llvm::BasicBlock* after = llvm::BasicBlock::Create(C_, "ramp_after", F_);
    llvm::MDBuilder md(C_);
    B_.CreateCondBr(ok, idle, call, md.createBranchWeights(63, 1));
    B_.SetInsertPoint(idle);
    B_.CreateAlignedStore(c32(0), B_.CreateConstInBoundsGEP1_64(i8, st, offsetof(dsp::ParamRampState, active)), llvm::Align(4));
    B_.CreateBr(after);
    B_.SetInsertPoint(call);
    emitCallHalfBody(i, t0, n32, h);
    B_.CreateBr(after);
    B_.SetInsertPoint(after);
  }
  void emitCallHalfBody(uint32_t i, Value* t0, Value* n32, unsigned h) {
    const g::Op& op = s_.ops[i];
    const auto ins = s_.inputs(op);
    const auto outs = s_.outputsOf(op);
    const void* fn = nullptr;
    Value* state = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr_));
    Value* resource = state;
    uint32_t spl = op.splMask;
    switch (op.kind) {
      case g::OpKind::Kernel: {
        const bool control = s_.regions[regionOf(i)].kind == g::RegionKind::Control;
        fn = reinterpret_cast<const void*>(dsp::findKernel(op.kernel, control ? 8u : op.lanes, op.splMask));
        if (op.state != g::kNone) state = op.kernel == Atom::ParamRamp ? statePtrHere(op.state, h) : statePtr(op.state, h);
        if (L_.opInfo[i] != g::kNone) resource = tabSlot(L_.opInfo[i]);
        break;
      }
      case g::OpKind::Broadcast:
        fn = reinterpret_cast<const void*>(&engine::host::broadcastKernel);
        spl = isPattern(ins[0]) ? 1u : 0u;
        break;
      case g::OpKind::Reduce:
        fn = reinterpret_cast<const void*>(&engine::host::reduceKernel);
        state = tabSlot(h ? L_.opInfoB[i] : L_.opInfo[i]);
        break;
      case g::OpKind::Send:
        fn = reinterpret_cast<const void*>(&engine::host::sendKernel);
        state = tabSlot(L_.opInfo[i]);
        break;
    }
    auto ptrOf = [&](const g::Operand& o) { return voice_ ? voicePtr(o, t0, h, B_) : framePtr(o, t0); };
    // TX.smooth: a call whose own ramps move reads every one of them as a block (a resting one's block filled with its
    // value first; in a unit's ramp-mode copy that was done for the unit), else its patterns: chosen per call, as the
    // interpreter chooses per record, so a unit whose ramps only calls read needs no ramp-mode copy.
    Value* opActive = nullptr;
    uint32_t ramped = 0;
    if (op.kind == g::OpKind::Kernel && !steady_)
      for (uint32_t j = 0; j < ins.size() && j < 32; ++j) {
        const uint32_t r = rampIndexOf(ins[j]);
        if (r == g::kNone) continue;
        ramped |= 1u << j;
        Value* a = rampActive(r, h);
        opActive = opActive ? B_.CreateOr(opActive, a) : a;
      }
    uint32_t toHold = 0;  // ramps of this call the unit has not filled
    for (uint32_t j = 0; j < ins.size() && j < 32; ++j)
      if (((ramped >> j) & 1u) != 0 && !(rampMode_ && unitHeld_[rampIndexOf(ins[j])])) toHold |= 1u << j;
    if (toHold != 0) {
      llvm::BasicBlock* holds = llvm::BasicBlock::Create(C_, "call_ramp_holds", F_);
      llvm::BasicBlock* after = llvm::BasicBlock::Create(C_, "call_ramp_ready", F_);
      llvm::MDBuilder md(C_);
      B_.CreateCondBr(opActive, holds, after, md.createBranchWeights(1, 63));
      B_.SetInsertPoint(holds);
      for (uint32_t j = 0; j < ins.size() && j < 32; ++j)
        if (((toHold >> j) & 1u) != 0) emitRampHold(rampIndexOf(ins[j]), h);
      B_.CreateBr(after);
      B_.SetInsertPoint(after);
    }
    // Pointer arrays: on the stack, or for a wide call in buf (jit_image.h kStackCallOperands).
    Value* inArr = inArr_;
    Value* outArr = outArr_;
    if (wideCall(op)) {
      inArr = B_.CreateConstInBoundsGEP1_64(f32_, buf_, L_.callOff);
      outArr = B_.CreateConstInBoundsGEP1_64(ptr_, inArr, ins.size());
    }
    for (uint32_t j = 0; j < ins.size(); ++j) {
      Value* p = ptrOf(ins[j]);
      if (j < 32 && ((ramped >> j) & 1u) != 0) p = B_.CreateSelect(opActive, ptrOf(s_.ramps[rampIndexOf(ins[j])].out), p);
      B_.CreateStore(p, B_.CreateConstInBoundsGEP1_64(ptr_, inArr, j));
    }
    for (uint32_t k = 0; k < outs.size(); ++k) B_.CreateStore(ptrOf(outs[k]), B_.CreateConstInBoundsGEP1_64(ptr_, outArr, k));
    B_.CreateStore(inArr, B_.CreateStructGEP(kargsTy_, kargs_, 0));
    B_.CreateStore(outArr, B_.CreateStructGEP(kargsTy_, kargs_, 1));
    B_.CreateStore(state, B_.CreateStructGEP(kargsTy_, kargs_, 2));
    B_.CreateStore(resource, B_.CreateStructGEP(kargsTy_, kargs_, 3));
    B_.CreateStore(n32, B_.CreateStructGEP(kargsTy_, kargs_, 4));
    B_.CreateStore(llvm::ConstantInt::get(i16_, op.numIn), B_.CreateStructGEP(kargsTy_, kargs_, 5));
    B_.CreateStore(llvm::ConstantInt::get(i16_, op.numOut), B_.CreateStructGEP(kargsTy_, kargs_, 6));
    Value* splV = c32(spl);
    Value* fnV = tabLoad(fn);
    if (ramped != 0) {
      const void* fnRamp = reinterpret_cast<const void*>(dsp::findKernel(op.kernel, op.lanes, op.splMask & ~ramped));
      splV = B_.CreateSelect(opActive, c32(op.splMask & ~ramped), splV);
      fnV = B_.CreateSelect(opActive, tabLoad(fnRamp), fnV);
    }
    B_.CreateStore(splV, B_.CreateStructGEP(kargsTy_, kargs_, 7));
    llvm::FunctionType* ty = llvm::FunctionType::get(void_, {ptr_}, false);
    callPtr(ty, fnV, {kargs_});
  }

  uint32_t regionOf(uint32_t op) const { return opRegion_[op]; }
  static bool wideCall(const g::Op& op) noexcept { return op.numIn > kStackCallOperands || op.numOut > kStackCallOperands; }

  // Exact transcendental through the CRT (scalar) or SVML (8-wide) wrapper.
  Value* exact(rt::Exact e, Value* a, Value* b, unsigned w) {
    const bool binary = rt::exactIsBinary(e);
    if (w == 16) {  // a pair: the 8-wide call per half (SVML's 512-bit functions need not round like the 256-bit ones)
      Value* lo = exact(e, half(a, 0), binary ? half(b, 0) : nullptr, 8);
      Value* hi = exact(e, half(a, 1), binary ? half(b, 1) : nullptr, 8);
      return concat(lo, hi);
    }
    if (w == 1) {
      llvm::FunctionType* ty =
          binary ? llvm::FunctionType::get(f32_, {f32_, f32_}, false) : llvm::FunctionType::get(f32_, {f32_}, false);
      llvm::CallInst* c = binary ? callPtr(ty, tabLoad(rt::exactScalar(e)), {a, b}) : callPtr(ty, tabLoad(rt::exactScalar(e)), {a});
      c->setMemoryEffects(llvm::MemoryEffects::inaccessibleMemOnly());  // errno at most
      c->addFnAttr(llvm::Attribute::WillReturn);
      return c;
    }
    B_.CreateAlignedStore(a, slot_, llvm::Align(32));
    if (binary) B_.CreateAlignedStore(b, B_.CreateConstInBoundsGEP2_64(slot_->getAllocatedType(), slot_, 0, 8), llvm::Align(32));
    llvm::FunctionType* ty = llvm::FunctionType::get(void_, {ptr_}, false);
    llvm::CallInst* c = callPtr(ty, tabLoad(rt::exactVector(e)), {slot_});
    c->setMemoryEffects(llvm::MemoryEffects::argMemOnly());
    c->addFnAttr(llvm::Attribute::WillReturn);
    return B_.CreateAlignedLoad(vecTy(w), slot_, llvm::Align(32));
  }

  // ================================================================================================================
  // Inline ops.
  // ================================================================================================================
  // A state entry's lane array at byte offset `off` (ldState / stState).
  Value* ldF(uint32_t entry, uint32_t off, Vx& v) { return ldState(entry, off, v, false); }
  Value* ldI(uint32_t entry, uint32_t off, Vx& v) { return ldState(entry, off, v, true); }
  void st(uint32_t entry, uint32_t off, Value* x, Vx& v) { stState(entry, off, x, v); }
  Value* splatScalar(Value* x, unsigned w) { return w == 1 ? x : B_.CreateVectorSplat(w, x); }

  // round(x) when 1 ≤ x < 2^31, else `fallback` (kernels_logic.cpp countFrom).
  Value* countFrom(Vx& v, Value* x, int32_t fallback) {
    Value* valid = v.band(v.ge(x, v.cf(1.0f)), v.lt(x, v.cf(2147483648.0f)));
    return v.selecti(v.asI(valid), v.roundToInt(x), v.ci(fallback));
  }

  // `argsOut`: the input values (Newton's tangent rules read them).
  void emitInline(uint32_t i, const Frame& fr, std::vector<Value*>* argsOut = nullptr) {
    const g::Op& op = s_.ops[i];
    const auto ins = s_.inputs(op);
    if (i == pickOp_ && pick_ > 0 && !argsOut) {  // TX.uniform-branch: a branch's select, one side chosen
      write(i, 0, read(ins[static_cast<uint32_t>(pick_)], fr), fr);
      return;
    }
    Vx v(B_, fr.w);
    IrScope scope(v);
    std::vector<Value*> a;
    a.reserve(ins.size());
    for (const g::Operand& o : ins) a.push_back(read(o, fr));
    if (argsOut) *argsOut = a;
    auto out = [&](Value* x) { write(i, 0, x, fr); };
    const int ex = exactOf(op.kernel);
    if (ex >= 0) {
      const auto e = static_cast<rt::Exact>(ex);
      out(exact(e, a[0], rt::exactIsBinary(e) ? a[1] : nullptr, fr.w));
      return;
    }
    using IrFm = IrS;
    switch (op.kernel) {
      // ---- math ----
      case Atom::Add: out(v.add(a[0], a[1])); return;
      case Atom::Sub: out(v.sub(a[0], a[1])); return;
      case Atom::Mul: out(v.mul(a[0], a[1])); return;
      case Atom::Div: out(v.div(a[0], a[1])); return;
      case Atom::Min: out(v.min(a[0], a[1])); return;
      case Atom::Max: out(v.max(a[0], a[1])); return;
      case Atom::Mod: {  // dsp modFloored: a result that rounds to b is 0
        Value* r = v.fnma(a[1], v.floor(v.div(a[0], a[1])), a[0]);
        out(v.andnot(v.eq(r, a[1]), r));
        return;
      }
      case Atom::PowFast:
        out(pq::detail::fm::exp2<IrFm>(v.mul(a[1], pq::detail::fm::log2<IrFm>(a[0]))));
        return;
      case Atom::CentsToHzFast:
        out(v.mul(a[1], pq::detail::fm::exp2<IrFm>(v.mul(a[0], v.cf(1.0f / 1200.0f)))));
        return;
      case Atom::HzToCentsFast:
        out(v.mul(v.cf(1200.0f), pq::detail::fm::log2<IrFm>(v.div(a[0], a[1]))));
        return;
      case Atom::Neg: out(v.neg(a[0])); return;
      case Atom::Abs: out(v.abs(a[0])); return;
      case Atom::Sign:
        out(v.sub(v.band(v.gt(a[0], v.zero()), v.cf(1.0f)), v.band(v.lt(a[0], v.zero()), v.cf(1.0f))));
        return;
      case Atom::Floor: out(v.floor(a[0])); return;
      case Atom::Frac: out(v.min(v.sub(a[0], v.floor(a[0])), v.cf(kBelowOne))); return;
      case Atom::Sqrt:
      case Atom::SqrtFast: out(v.sqrt(a[0])); return;
      case Atom::ExpFast: out(pq::detail::fm::exp2<IrFm>(v.mul(a[0], v.cf(kLog2e)))); return;
      case Atom::LogFast: out(v.mul(pq::detail::fm::log2<IrFm>(a[0]), v.cf(kLn2))); return;
      case Atom::SinFast:  // dsp SinFastOp: clamped to [−1, 1], NaN propagates
        out(v.max(v.cf(-1.0f), v.min(v.cf(1.0f), pq::detail::fm::sin<IrFm>(a[0]))));
        return;
      case Atom::TanFast: out(pq::detail::fm::tan<IrFm>(a[0])); return;
      case Atom::TanhFast: out(pq::detail::fm::tanh<IrFm>(a[0])); return;
      case Atom::DbToGainFast: out(pq::detail::fm::exp2<IrFm>(v.mul(a[0], v.cf(0.166096404744368118f)))); return;
      case Atom::GainToDbFast: out(v.mul(pq::detail::fm::log2<IrFm>(a[0]), v.cf(6.02059991327962390f))); return;
      case Atom::Const: out(a[0]); return;
      case Atom::SinePhase:
        out(pq::detail::fm::sin<IrFm>(v.mul(v.sub(a[0], v.round(a[0])), v.cf(kTwoPi))));
        return;
      case Atom::Clip: out(v.min(v.max(a[0], a[1]), a[2])); return;
      case Atom::Lerp: out(v.fma(a[2], v.sub(a[1], a[0]), a[0])); return;
      case Atom::Select: out(v.select(a[0], a[1], a[2])); return;
      case Atom::MulAdd: out(v.fma(a[0], a[1], a[2])); return;
      // ---- logic ----
      case Atom::And: out(v.band(a[0], a[1])); return;
      case Atom::Or: out(v.bor(a[0], a[1])); return;
      case Atom::Xor: out(v.bxor(a[0], a[1])); return;
      case Atom::Nand: out(v.bnot(v.band(a[0], a[1]))); return;
      case Atom::Nor: out(v.bnot(v.bor(a[0], a[1]))); return;
      case Atom::Not: out(v.bnot(a[0])); return;
      case Atom::Gt: out(v.gt(a[0], a[1])); return;
      case Atom::Lt: out(v.lt(a[0], a[1])); return;
      case Atom::Ge: out(v.ge(a[0], a[1])); return;
      case Atom::Le: out(v.le(a[0], a[1])); return;
      case Atom::Eq: out(v.eq(a[0], a[1])); return;
      case Atom::Ne: out(v.ne(a[0], a[1])); return;
      case Atom::GateToReal: out(v.band(a[0], v.cf(1.0f))); return;
      case Atom::Mux: {
        Value* r = a[1];
        for (uint32_t k = 1; k + 1 < a.size(); ++k) r = v.select(v.ge(a[0], v.cf(static_cast<float>(k))), a[k + 1], r);
        out(r);
        return;
      }
      case Atom::Demux: {
        // max(floor, 0) returns 0 for NaN (second operand); then clamp to n − 1.
        Value* c = v.min(v.max(v.floor(a[0]), v.zero()), v.cf(static_cast<float>(op.numOut - 1)));
        for (uint32_t k = 0; k < op.numOut; ++k) write(i, k, v.band(v.eq(c, v.cf(static_cast<float>(k))), a[1]), fr);
        return;
      }
      default: break;
    }
    // ---- stateful (one frame per evaluation) ----
    const uint32_t sp = op.state;
    switch (op.kernel) {
      case Atom::Gain:
      case Atom::GainAdd: {  // inlineGain(): per frame the kernel's definition, vectorised the settled form
        const bool add = op.kernel == Atom::GainAdd;
        Value* x = a[add ? 1 : 0];
        Value* acc = add ? a[0] : v.zero();
        Vx s1(B_, 1);
        if (rampedGain(i)) {  // TX.smooth: the kernel's audio-rate path with g the ramp's frames (kernels_gain.cpp audio1)
          Value* gv = a[add ? 2 : 1];
          gv = v.select(v.lt(v.abs(gv), v.cf(std::numeric_limits<float>::infinity())), gv, v.zero());
          if (fr.w == 1 && !gainsSettled_) {
            Value* d = ldF(sp, static_cast<uint32_t>(offsetof(dsp::GainState, d)), s1);
            Value* r = ldF(sp, static_cast<uint32_t>(offsetof(dsp::GainState, r)), s1);
            Value* y = v.fma(x, v.add(gv, d), acc);
            out(v.select(v.band(v.eq(d, v.zero()), v.eq(gv, v.zero())), acc, y));
            Value* dn = v.mul(d, r);
            st(sp, static_cast<uint32_t>(offsetof(dsp::GainState, d)),
               v.select(v.lt(v.abs(dn), v.cf(dsp::kGainSettled)), v.zero(), dn), s1);
            return;
          }
          out(v.select(v.eq(gv, v.zero()), acc, v.fma(x, gv, acc)));
          return;
        }
        const auto pre = gainTarget_.find(i);  // loaded after the prologue, before the loop (the settled form's)
        Value* tgt = pre != gainTarget_.end() && (fr.w != 1 || gainsSettled_)
                         ? pre->second
                         : ldF(sp, static_cast<uint32_t>(offsetof(dsp::GainState, target)), s1);
        if (fr.w == 1 && !gainsSettled_) {
          Value* d = ldF(sp, static_cast<uint32_t>(offsetof(dsp::GainState, d)), s1);
          Value* r = ldF(sp, static_cast<uint32_t>(offsetof(dsp::GainState, r)), s1);
          Value* y = v.fma(x, v.add(tgt, d), acc);
          out(v.select(v.band(v.eq(d, v.zero()), v.eq(tgt, v.zero())), acc, y));
          Value* dn = v.mul(d, r);
          st(sp, static_cast<uint32_t>(offsetof(dsp::GainState, d)),
             v.select(v.lt(v.abs(dn), v.cf(dsp::kGainSettled)), v.zero(), dn), s1);
          return;
        }
        Value* t = splatScalar(tgt, fr.w);
        out(v.select(splatScalar(s1.eq(tgt, s1.zero()), fr.w), acc, v.fma(x, t, acc)));
        return;
      }
      case Atom::Z1: {
        out(ldF(sp, 0, v));
        st(sp, 0, a[0], v);
        return;
      }
      case Atom::Z1Read: out(ldF(sp, 0, v)); return;
      case Atom::Z1Write: st(sp, 0, a[0], v); return;
      case Atom::Rise:
      case Atom::Fall:
      case Atom::Change: {
        Value* prev = ldF(sp, 0, v);
        Value* x = a[0];
        out(op.kernel == Atom::Rise ? v.andnot(prev, x) : op.kernel == Atom::Fall ? v.andnot(x, prev) : v.bxor(x, prev));
        st(sp, 0, x, v);
        return;
      }
      case Atom::Schmitt: {
        Value* q = ldF(sp, 0, v);
        q = v.select(v.gt(a[0], a[2]), v.ones(), v.andnot(v.lt(a[0], a[1]), q));
        out(q);
        st(sp, 0, q, v);
        return;
      }
      case Atom::SrFlipFlop: {
        Value* q = v.andnot(a[1], v.bor(a[0], ldF(sp, 0, v)));
        out(q);
        st(sp, 0, q, v);
        return;
      }
      case Atom::DFlipFlop:
      case Atom::TFlipFlop: {
        Value* q = ldF(sp, 0, v);
        Value* prev = ldF(sp, 32, v);
        Value* c = a[1];
        Value* edge = v.andnot(prev, c);
        q = op.kernel == Atom::TFlipFlop ? v.bxor(q, v.band(edge, a[0])) : v.select(edge, a[0], q);
        out(q);
        st(sp, 0, q, v);
        st(sp, 32, c, v);
        return;
      }
      case Atom::Counter: {
        Value* count = ldI(sp, 0, v);
        Value* u = a[0];
        Value* d = a[1];
        Value* eu = v.asI(v.andnot(ldF(sp, 32, v), u));  // −1 on an up edge
        Value* ed = v.asI(v.andnot(ldF(sp, 64, v), d));
        count = v.addi(v.subi(count, eu), ed);
        Value* n = countFrom(v, a[3], 0);
        Value* zero = v.ci(0);
        Value* wrap = v.gti(n, zero);
        Value* nm1 = v.subi(n, v.ci(1));
        count = v.selecti(v.andi(wrap, v.gti(count, nm1)), zero, count);
        count = v.selecti(v.andi(wrap, v.gti(zero, count)), nm1, count);
        count = v.selecti(v.asI(a[2]), zero, count);
        out(v.toFloat(count));
        st(sp, 0, count, v);
        st(sp, 32, u, v);
        st(sp, 64, d, v);
        return;
      }
      case Atom::ClockDiv: {
        Value* k = ldI(sp, 0, v);
        Value* c = a[0];
        Value* edge = v.asI(v.andnot(ldF(sp, 32, v), c));
        Value* n = countFrom(v, a[1], 1);
        Value* nextK = v.addi(k, v.ci(1));
        nextK = v.selecti(v.gti(n, nextK), nextK, v.ci(0));  // k + 1 ≥ n → 0
        k = v.selecti(edge, nextK, k);
        out(v.band(c, v.asF(v.eqi(k, v.ci(0)))));
        st(sp, 0, k, v);
        st(sp, 32, c, v);
        return;
      }
      case Atom::Pulse: {
        Value* r = ldI(sp, 0, v);
        Value* zero = v.ci(0);
        Value* fire = v.eqi(r, zero);
        Value* p = countFrom(v, a[0], 0);  // invalid → 0 → next value −1 (one-shot)
        Value* dec = v.addi(r, v.gti(r, zero));
        r = v.selecti(fire, v.subi(p, v.ci(1)), dec);
        out(v.asF(fire));
        st(sp, 0, r, v);
        return;
      }
      case Atom::Timer: {
        Value* r = ldI(sp, 0, v);
        Value* t = a[0];
        Value* edge = v.asI(v.andnot(ldF(sp, 32, v), t));
        Value* d = v.min(v.max(a[1], v.zero()), v.cf(2147483520.0f));  // NaN → 0 (dsp Timer)
        r = v.selecti(edge, v.roundToInt(d), r);
        Value* on = v.gti(r, v.ci(0));
        out(v.asF(on));
        r = v.addi(r, on);  // −1 while positive
        st(sp, 0, r, v);
        st(sp, 32, t, v);
        return;
      }
      case Atom::SampleHold: {
        Value* held = ldF(sp, 0, v);
        Value* t = a[1];
        held = v.select(v.andnot(ldF(sp, 32, v), t), a[0], held);
        out(held);
        st(sp, 0, held, v);
        st(sp, 32, t, v);
        return;
      }
      case Atom::Latch: {
        Value* held = v.select(a[1], a[0], ldF(sp, 0, v));
        out(held);
        st(sp, 0, held, v);
        return;
      }
      case Atom::Phasor:
      case Atom::SineHz: {
        Value* ph = ldI(sp, 0, v);
        // incScale and halfSr are per op (the same in every group's block): read from the first half's.
        Value* scale = B_.CreateAlignedLoad(f64_, B_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), statePtr(sp), 32), llvm::Align(8));
        Value* halfSr = splatScalar(
            B_.CreateAlignedLoad(f32_, B_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), statePtr(sp), 40), llvm::Align(8)), v.width());
        ph = v.selecti(v.asI(a[1]), v.ci(0), ph);
        if (op.kernel == Atom::Phasor) out(v.mul(v.toFloat(v.srli(ph, 8)), v.cf(k2pm24)));
        else out(pq::detail::fm::sin<IrFm>(v.mul(v.toFloat(ph), v.cf(kTwoPiOver2p32))));
        // dsp PhasorK::increment: NaN hz → 0 Hz, then clamped to ±sr/2.
        Value* h = v.min(v.max(v.band(v.eq(a[0], a[0]), a[0]), v.neg(halfSr)), halfSr);
        ph = v.addi(ph, v.roundMulToInt(h, scale));
        st(sp, 0, ph, v);
        return;
      }
      default: break;
    }
  }

  // An op in a per-frame context: inline, or a kernel call for this one frame.
  void emitFrameOp(uint32_t i, const Frame& fr) {
    if (inline_[i]) {
      emitInline(i, fr);
      return;
    }
    if (sineInline(i, voice_, vw_) && !(rampMode_ && opReadsRamps(i))) {
      emitWavetableSine(i, fr);
      return;
    }
    emitCall(i, fr.t, c32(1));
    const g::Op& op = s_.ops[i];
    if (fr.map)
      for (const g::Operand& o : s_.outputsOf(op)) fr.map->erase(o.index);
  }

  // TMM.12-fix1: wavetable_pos in a per-sample loop (a feedback loop through a Wavetable: the Meta Operator's
  // operator D). A kernel call per frame costs the call and the spills of every live vector around it, so frames
  // whose read the readers would do as a sine (frame_table.h "Sine tables") are computed inline instead: the kernel
  // runs at the block's first frame (swaps, crossfades, taps, the frame choice: everything the slot decides) and
  // leaves WavetablePosState::sineOn with the lanes' coefficients; the following frames run inline while it is set,
  // else call the kernel. The inline step is the readers' sine path with the same operations (the same bits as the
  // interpreter) and updates the reader state as they do. Not in solve bodies, Control regions or the scalar-lane
  // voice baseline (calls there).
  bool opReadsRamps(uint32_t i) const noexcept {
    if (s_.rampOf.empty()) return false;
    const g::Op& op = s_.ops[i];
    for (uint32_t j = 0; j < op.numIn; ++j)
      if (s_.rampOf[op.firstOperand + j] != g::kNone) return true;
    return false;
  }
  bool sineInline(uint32_t i, bool voice, unsigned vw) const {
    const g::Op& op = s_.ops[i];
    if (op.kind != g::OpKind::Kernel || op.kernel != Atom::WavetablePos) return false;
    if (op.state == g::kNone || body_[i]) return false;
    if (s_.regions[regionOf(i)].kind != g::RegionKind::Loop) return false;
    return voice ? (vw == 8 || vw == 16) && op.lanes == 8 : op.lanes == 1;
  }
  void emitWavetableSine(uint32_t i, const Frame& fr) {
    const g::Op& op = s_.ops[i];
    const uint32_t sp = op.state;
    llvm::Type* i8 = llvm::Type::getInt8Ty(C_);
    Vx v(B_, fr.w);
    IrScope scope(v);
    Value* p = read(s_.inputs(op)[0], fr);  // before the branch: both paths' frames see it
    auto flag = [&](unsigned h) {
      Value* q = B_.CreateConstInBoundsGEP1_64(i8, statePtr(sp, h), offsetof(dsp::WavetablePosState, sineOn));
      return B_.CreateICmpNE(B_.CreateAlignedLoad(i32_, q, llvm::Align(4)), c32(0));
    };
    Value* on = B_.CreateAnd(B_.CreateICmpNE(fr.t, c64(0)), flag(0));
    if (voice_ && vw_ == 16) on = B_.CreateAnd(on, flag(1));
    llvm::BasicBlock* fast = llvm::BasicBlock::Create(C_, "wt_sine", F_);
    llvm::BasicBlock* slow = llvm::BasicBlock::Create(C_, "wt_call", F_);
    llvm::BasicBlock* join = llvm::BasicBlock::Create(C_, "wt_join", F_);
    llvm::MDBuilder md(C_);
    B_.CreateCondBr(on, fast, slow, md.createBranchWeights(63, 1));
    // Inline: dc + amp·sin(2π(r + θ)), r = p − round(p), NaN → 0 (frame_table_read.cpp sineStep).
    B_.SetInsertPoint(fast);
    using S = dsp::WavetablePosState;
    auto off = [](std::size_t o) { return static_cast<uint32_t>(o); };
    Value* r = v.sub(p, v.round(p));
    r = v.band(r, v.eq(r, r));
    Value* x = v.mul(v.add(r, ldF(sp, off(offsetof(S, sineTh)), v)), v.cf(kTwoPi));
    Value* yFast =
        v.fma(ldF(sp, off(offsetof(S, sineAmp)), v), pq::detail::fm::sin<IrS>(x), ldF(sp, off(offsetof(S, sineDc)), v));
    // The reader state (frame_table_ops.h phaseDelta): |Δphase| wrapped to [0, ½], NaN → 0; then the phase.
    Value* d = v.sub(p, ldF(sp, off(offsetof(S, prevPhase)), v));
    d = v.max(v.abs(v.sub(d, v.round(d))), v.zero());
    st(sp, off(offsetof(S, prevDelta)), d, v);
    st(sp, off(offsetof(S, prevPhase)), p, v);
    llvm::BasicBlock* fastEnd = B_.GetInsertBlock();
    B_.CreateBr(join);
    // The kernel for this frame (it writes the output buffer and decides sineOn for the next frames).
    B_.SetInsertPoint(slow);
    emitCall(i, fr.t, c32(1));
    const Frame plain{fr.t, fr.w, nullptr, fr.hoist, fr.mask};
    Value* ySlow = loadBlk(s_.outputsOf(op)[0], plain);
    llvm::BasicBlock* slowEnd = B_.GetInsertBlock();
    B_.CreateBr(join);
    B_.SetInsertPoint(join);
    llvm::PHINode* y = B_.CreatePHI(v.fT(), 2);
    y->addIncoming(yFast, fastEnd);
    y->addIncoming(ySlow, slowEnd);
    write(i, 0, y, fr);
  }

  // ================================================================================================================
  // Loops and regions.
  // ================================================================================================================
  template <class Body>
  void forLoop(Value* start, Value* end, uint64_t step, Body&& body) {
    llvm::BasicBlock* pre = B_.GetInsertBlock();
    llvm::BasicBlock* head = llvm::BasicBlock::Create(C_, "loop", F_);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(C_, "body", F_);
    llvm::BasicBlock* exit = llvm::BasicBlock::Create(C_, "exit", F_);
    B_.CreateBr(head);
    B_.SetInsertPoint(head);
    llvm::PHINode* i = B_.CreatePHI(i64_, 2);
    i->addIncoming(start, pre);
    B_.CreateCondBr(B_.CreateICmpSLT(i, end), bodyBB, exit);
    B_.SetInsertPoint(bodyBB);
    body(static_cast<Value*>(i));
    Value* next = B_.CreateAdd(i, c64(step), "", true, true);
    i->addIncoming(next, B_.GetInsertBlock());
    B_.CreateBr(head);
    B_.SetInsertPoint(exit);
  }

  // TMM.6: the frames a region's ops run on: the block's frames times its oversample factor (computed once per
  // function, in the entry block).
  Value* regionFrames(uint32_t ri) {
    const uint32_t f = s_.regions[ri].factor;
    if (f <= 1) return frames64_;
    auto it = framesF_.find(f);
    if (it != framesF_.end()) return it->second;
    llvm::IRBuilder<> eb(entryTerm_);
    Value* v = eb.CreateMul(frames64_, c64(f), "", true, true);
    framesF_.emplace(f, v);
    return v;
  }

  void emitRegion(uint32_t ri) {
    const g::Region& reg = s_.regions[ri];
    if (reg.kind == g::RegionKind::Control) {
      Map m;
      emitPerFrame(reg, c64(0), voice_ ? vw_ : 8u, m, false);
      return;
    }
    // Preheader for the region's invariant pattern loads.
    llvm::BasicBlock* body = llvm::BasicBlock::Create(C_, "region", F_);
    hoistIP_ = B_.CreateBr(body);
    inv_.clear();
    B_.SetInsertPoint(body);
    const unsigned lanes = voice_ ? vw_ : 1u;
    if (reg.kind == g::RegionKind::Block) {
      emitBranchStates(ri);
      emitSlice(reg.firstOp, reg.firstOp + reg.numOps, c64(0), regionFrames(ri));
      guardOn_.clear();
      branchAll_.clear();
      branchNone_.clear();
    } else if (perFrame(reg)) {
      emitGainBranch(voice_ ? nullptr : emitRegionGainPrologues(ri, ri), [&] {
        forLoop(c64(0), regionFrames(ri), 1, [&](Value* t) {
          Map m;
          emitPerFrame(reg, t, lanes, m, true);
        });
      });
    } else {
      const uint32_t sub = reg.subBlock;
      forLoop(c64(0), regionFrames(ri), sub, [&](Value* t) {
        Value* n = B_.CreateBinaryIntrinsic(llvm::Intrinsic::smin, c64(sub), B_.CreateSub(regionFrames(ri), t));
        emitSlice(reg.firstOp, reg.firstOp + reg.numOps, t, n);
      });
    }
    hoistIP_ = nullptr;
    inv_.clear();
  }

  // T1.x-silence: sleepable regions r0..r1 (one region, or a chain of one segment) behind their segment's flag (the
  // host decides it at the stage's start, image_host.h): asleep, their fill buffers are zeroed (the constant-output
  // kernel) instead of running them. One branch per block.
  template <class Body>
  void emitSleeping(uint32_t r0, uint32_t r1, Body&& body) {
    const g::RegionSleep& rs = s_.sleep.regions[r0];
    Value* flag = B_.CreateAlignedLoad(i32_, B_.CreateConstInBoundsGEP1_64(f32_, aux_, L_.sleepOff + rs.segment),
                                       llvm::Align(4));
    llvm::BasicBlock* asleep = llvm::BasicBlock::Create(C_, "asleep", F_);
    llvm::BasicBlock* awake = llvm::BasicBlock::Create(C_, "awake", F_);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(C_, "after", F_);
    B_.CreateCondBr(B_.CreateICmpNE(flag, c32(0)), asleep, awake);
    B_.SetInsertPoint(asleep);
    llvm::Constant* zero8 = llvm::Constant::getNullValue(vecTy(8));
    for (const uint32_t b : fillsOf(r0, r1)) {
      const g::Buffer& buf = s_.buffers[b];
      // Whole 8-float chunks: a buffer is sized in 16-float units (image_host.cpp layoutMemory), so the rounded-up
      // tail stays inside it.
      Value* n = buf.cls == g::Buffer::Class::Pattern ? static_cast<Value*>(c64(8))
                                                      : B_.CreateMul(frames64_, c64(uint64_t{buf.lanes} * buf.factor), "", true, true);
      n = B_.CreateAnd(B_.CreateAdd(n, c64(7)), c64(~uint64_t{7}));
      forLoop(c64(0), n, 8, [&](Value* i) {
        Value* p = B_.CreateInBoundsGEP(f32_, buf_, B_.CreateAdd(i, c64(L_.bufOff[b]), "", true, true));
        B_.CreateAlignedStore(zero8, p, llvm::Align(32));
      });
    }
    B_.CreateBr(done);
    B_.SetInsertPoint(awake);
    body();
    B_.CreateBr(done);
    B_.SetInsertPoint(done);
  }

  // T1b.x-perf (Q126): a memo Control region (analyse()) runs only in a block whose params differ from the previous
  // block's (the flag word JitImage::process writes); otherwise its buffers still hold this block's values. One
  // branch per block: a settled mixer's dB / pan math (exact transcendentals) costs nothing, as in the interpreter.
  void emitMemo(uint32_t r) {
    Value* flag = B_.CreateAlignedLoad(i32_, B_.CreateConstInBoundsGEP1_64(f32_, buf_, L_.memoOff), llvm::Align(4));
    llvm::BasicBlock* run = llvm::BasicBlock::Create(C_, "memo_run", F_);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(C_, "memo_after", F_);
    B_.CreateCondBr(B_.CreateICmpEQ(flag, c32(0)), run, done);
    B_.SetInsertPoint(run);
    if (masked_[r]) emitMaskedRegion(r);
    else emitRegion(r);
    B_.CreateBr(done);
    B_.SetInsertPoint(done);
  }

  // TX.vca-knob2: a memo region in runs of one param mask (graph::Schedule::memoMask), each behind a test of the
  // block's changed-param mask (HostLayout::memoOff + 2, a u64: JitImage::process from ImageHost::paramsChangedMask;
  // all bits in the first block). A run of constants only (mask 0) runs in the first block. The values a run hands on
  // are in memory (store elimination: each run is a group), so each run starts with an empty value map.
  void emitMaskedRegion(uint32_t r) {
    const g::Region& reg = s_.regions[r];
    llvm::Type* i64 = llvm::Type::getInt64Ty(C_);
    Value* changed = B_.CreateAlignedLoad(i64, B_.CreateConstInBoundsGEP1_64(f32_, buf_, L_.memoOff + 2), llvm::Align(8));
    const uint32_t end = reg.firstOp + reg.numOps;
    for (uint32_t i = reg.firstOp; i < end;) {
      const uint64_t m = s_.memoMask[i];
      uint32_t j = i + 1;
      while (j < end && s_.memoMask[j] == m) ++j;
      Value* cond = m != 0 ? B_.CreateICmpNE(B_.CreateAnd(changed, llvm::ConstantInt::get(i64, m)), llvm::ConstantInt::get(i64, 0))
                           : B_.CreateICmpEQ(changed, llvm::ConstantInt::get(i64, ~uint64_t{0}));
      llvm::BasicBlock* body = llvm::BasicBlock::Create(C_, "memo_part", F_);
      llvm::BasicBlock* after = llvm::BasicBlock::Create(C_, "memo_part_after", F_);
      B_.CreateCondBr(cond, body, after);
      B_.SetInsertPoint(body);
      Map map;
      const Frame fr{c64(0), 8u, &map, false};
      for (uint32_t k = i; k < j; ++k) emitFrameOp(k, fr);
      B_.CreateBr(after);
      B_.SetInsertPoint(after);
      i = j;
    }
  }

  // A global Control region whose results stay valid while the params do (the interpreter's RegionRun::memo rule,
  // src/engine/src/interpreter.cpp): no solve, every op a stateless kernel without a resource reading params,
  // constants and values of the region itself, into buffers that no other op (and no solve unknown) writes.
  bool memoizable(const g::Region& reg) const {
    if (reg.kind != g::RegionKind::Control || reg.poly != g::Poly::Global || reg.numSolves != 0) return false;
    std::vector<uint32_t> written;
    for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i) {
      const g::Op& op = s_.ops[i];
      if (op.kind != g::OpKind::Kernel || op.state != g::kNone || op.resource != g::kNone) return false;
      for (const g::Operand& o : s_.inputs(op)) {
        if (o.kind == g::Operand::Kind::Param || o.kind == g::Operand::Kind::Const) continue;
        if (o.kind != g::Operand::Kind::Buffer) return false;
        if (std::find(written.begin(), written.end(), o.index) == written.end()) return false;
      }
      for (const g::Operand& o : s_.outputsOf(op)) {
        if (o.kind != g::Operand::Kind::Buffer) return false;
        written.push_back(o.index);
      }
    }
    for (uint32_t i = 0; i < s_.ops.size(); ++i) {
      if (i >= reg.firstOp && i < reg.firstOp + reg.numOps) continue;
      for (const g::Operand& o : s_.outputsOf(s_.ops[i]))
        if (o.kind == g::Operand::Kind::Buffer && std::find(written.begin(), written.end(), o.index) != written.end())
          return false;
    }
    for (const g::SolveUnknown& u : s_.unknowns)
      if (u.value.kind == g::Operand::Kind::Buffer && std::find(written.begin(), written.end(), u.value.index) != written.end())
        return false;
    return true;
  }

  // The buffers sleeping regions r0..r1 fill.
  std::vector<uint32_t> fillsOf(uint32_t r0, uint32_t r1) const {
    std::vector<uint32_t> f;
    for (uint32_t r = r0; r <= r1; ++r) {
      const g::RegionSleep& rs = s_.sleep.regions[r];
      f.insert(f.end(), s_.sleep.fills.begin() + rs.firstFill, s_.sleep.fills.begin() + rs.firstFill + rs.numFills);
    }
    return f;
  }

  // Regions [r0, r1] of a stage as one loop over frames (a chain, analyse()): per frame, every region's ops in order
  // (Block regions' inline ops, per-sample regions with their solves), values in one map. A voice chain runs one
  // frame of the group's (pair's, lane's) voices per iteration, a global chain one frame (scalar).
  void emitChain(uint32_t r0, uint32_t r1) {
    const unsigned w = voice_ ? vw_ : 1u;
    llvm::BasicBlock* body = llvm::BasicBlock::Create(C_, "chain", F_);
    hoistIP_ = B_.CreateBr(body);
    inv_.clear();
    B_.SetInsertPoint(body);
    emitGainBranch(voice_ ? nullptr : emitRegionGainPrologues(r0, r1), [&] {
      forLoop(c64(0), regionFrames(r0), 1, [&](Value* t) {
        Map m;
        for (uint32_t r = r0; r <= r1; ++r) {
          const g::Region& reg = s_.regions[r];
          if (reg.kind == g::RegionKind::Block) {
            const Frame fr{t, w, &m, true};
            for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i) emitInline(i, fr);
          } else {
            emitPerFrame(reg, t, w, m, true);
          }
        }
      });
    });
    hoistIP_ = nullptr;
    inv_.clear();
  }

  // TX.uniform-branch: at a Block region's start, per taken branch of it (outer first): whether its condition is true
  // in ALL lanes of the pattern its select reads (8; a pair's 16; the scalar baseline's one) or in NONE (the sign bits,
  // as the select reads them), and so whether each of its guards runs this block (guardOn_; under its parent's).
  void emitBranchStates(uint32_t ri) {
    guardOn_.clear();
    branchAll_.clear();
    branchNone_.clear();
    for (uint32_t b = 0; b < s_.branches.size(); ++b) {
      if (!taken_[b] || s_.branches[b].region != ri) continue;
      const g::Op& op = s_.ops[s_.branches[b].op];
      const unsigned w = voice_ ? vw_ : 8u;
      Value* c = read(s_.inputs(op)[0], Frame{c64(0), w, nullptr, true});
      llvm::Type* ity = c->getType()->isVectorTy()
                            ? static_cast<llvm::Type*>(llvm::FixedVectorType::get(
                                  i32_, llvm::cast<llvm::FixedVectorType>(c->getType())->getNumElements()))
                            : i32_;
      Value* neg = B_.CreateICmpSLT(B_.CreateBitCast(c, ity), llvm::Constant::getNullValue(ity));
      Value* all = ity->isVectorTy() ? B_.CreateAndReduce(neg) : neg;
      Value* any = ity->isVectorTy() ? B_.CreateOrReduce(neg) : neg;
      Value* on = nullptr;
      const uint32_t parent = takenGuard(s_.branches[b].parent);
      if (parent != g::kNone) {
        auto it = guardOn_.find(parent);
        on = it != guardOn_.end() ? it->second : nullptr;
      }
      Value* t = any;
      Value* f = B_.CreateNot(all);
      if (on) t = B_.CreateAnd(on, t), f = B_.CreateAnd(on, f);
      guardOn_[2 * b] = t;
      guardOn_[2 * b + 1] = f;
      branchAll_[b] = all;
      branchNone_[b] = B_.CreateNot(any);
    }
  }

  // Ops [a, b) of a Block region or sub-block on frames [t0, t0 + n): fused runs and calls. TX.uniform-branch: ops of a
  // taken guard behind one branch on it (guardOn_), a taken branch's select as the side its condition chose.
  void emitSlice(uint32_t a, uint32_t b, Value* t0, Value* n) {
    if (guardOn_.empty()) {
      emitSliceOps(a, b, t0, n);
      return;
    }
    uint32_t i = a;
    while (i < b) {
      const uint32_t gd = guardEff_[i];
      uint32_t j = i + 1;
      if (!branchSelect(i))
        while (j < b && guardEff_[j] == gd && !branchSelect(j)) ++j;
      llvm::BasicBlock* after = nullptr;
      if (gd != g::kNone) {
        auto it = guardOn_.find(gd);
        if (it != guardOn_.end()) {
          llvm::BasicBlock* run = llvm::BasicBlock::Create(C_, "guarded", F_);
          after = llvm::BasicBlock::Create(C_, "guarded_after", F_);
          B_.CreateCondBr(it->second, run, after);
          B_.SetInsertPoint(run);
        }
      }
      if (branchSelect(i)) emitBranchSelect(i, t0, n);
      else emitSliceOps(i, j, t0, n);
      if (after) {
        B_.CreateBr(after);
        B_.SetInsertPoint(after);
      }
      i = j;
    }
  }

  // A taken branch's select (alone in its run): cond ? a : b while the condition is mixed, else its output is the
  // chosen input (pick_: what the select gives when every lane chose it; the other input is not read).
  void emitBranchSelect(uint32_t i, Value* t0, Value* n) {
    const uint32_t b = branchOf_[i];
    auto it = branchAll_.find(b);
    if (it == branchAll_.end()) {
      emitSliceOps(i, i + 1, t0, n);
      return;
    }
    llvm::BasicBlock* pickA = llvm::BasicBlock::Create(C_, "branch_true", F_);
    llvm::BasicBlock* notA = llvm::BasicBlock::Create(C_, "branch_not_true", F_);
    llvm::BasicBlock* pickB = llvm::BasicBlock::Create(C_, "branch_false", F_);
    llvm::BasicBlock* mixed = llvm::BasicBlock::Create(C_, "branch_mixed", F_);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(C_, "branch_after", F_);
    B_.CreateCondBr(it->second, pickA, notA);
    B_.SetInsertPoint(notA);
    B_.CreateCondBr(branchNone_[b], pickB, mixed);
    const std::pair<llvm::BasicBlock*, int> cases[] = {{pickA, 1}, {pickB, 2}, {mixed, -1}};
    for (const auto& [bb, pick] : cases) {
      B_.SetInsertPoint(bb);
      pickOp_ = i;
      pick_ = pick;
      emitSliceOps(i, i + 1, t0, n);
      pickOp_ = g::kNone;
      pick_ = -1;
      B_.CreateBr(done);
    }
    B_.SetInsertPoint(done);
  }

  void emitSliceOps(uint32_t a, uint32_t b, Value* t0, Value* n) {
    uint32_t i = a;
    while (i < b) {
      if (!inline_[i]) {
        emitCall(i, t0, B_.CreateTrunc(n, i32_));
        ++i;
        continue;
      }
      uint32_t j = i;
      while (j < b && inline_[j] && grp_[j] == grp_[i]) ++j;
      emitFused(i, j, t0, n);
      i = j;
    }
  }

  // Rough instruction count of one op in a fused run's body (fast_ functions expand to ~20 instructions).
  uint32_t opCost(uint32_t i) const {
    switch (s_.ops[i].kernel) {
      case Atom::PowFast: return 45;
      case Atom::CentsToHzFast: case Atom::HzToCentsFast: case Atom::ExpFast: case Atom::LogFast: case Atom::SinFast:
      case Atom::TanFast: case Atom::TanhFast: case Atom::DbToGainFast: case Atom::GainToDbFast: case Atom::SinePhase:
        return 22;
      case Atom::Mux: case Atom::Demux: return 2 * s_.ops[i].numIn + 2 * s_.ops[i].numOut;
      case Atom::Gain: case Atom::GainAdd: return 4;
      default: return 2;
    }
  }
  uint32_t runCost(uint32_t a, uint32_t b) const {
    uint32_t c = 0;
    for (uint32_t i = a; i < b; ++i) c += opCost(i);
    return c;
  }
  // Critical path of a fused run's body (opCost units): the longest chain of ops that feed each other.
  uint32_t runDepth(uint32_t a, uint32_t b) const {
    std::unordered_map<uint32_t, uint32_t> depthOf;  // buffer → depth of the value the run wrote there
    uint32_t depth = 0;
    for (uint32_t i = a; i < b; ++i) {
      uint32_t d = 0;
      for (const g::Operand& o : s_.inputs(s_.ops[i]))
        if (o.kind == g::Operand::Kind::Buffer)
          if (auto it = depthOf.find(o.index); it != depthOf.end()) d = std::max(d, it->second);
      d += opCost(i);
      for (const g::Operand& o : s_.outputsOf(s_.ops[i]))
        if (o.kind == g::Operand::Kind::Buffer) depthOf[o.index] = d;
      depth = std::max(depth, d);
    }
    return depth;
  }
  // How many chunks one iteration of a fused run interleaves. Interleaving exists for latency-bound runs (a long
  // dependency chain: one chunk leaves the core idle); a run whose chunk already holds many independent chains (a
  // row of products feeding a mux) gains nothing from it. So: enough chunks for ~8 chains in flight
  // (8 · depth / cost, rounded up to a power of two, at most 8), none for short runs, and never more copies than
  // kInterleaveBudget allows (code size and compile time grow with u · cost; the u-chunk body plus the one-chunk
  // remainder loop are u + 1 copies of the run).
  static unsigned interleaveFor(uint32_t cost, uint32_t depth) noexcept {
    if (cost < 40) return 1;
    unsigned u = 1;
    while (u < 8 && static_cast<uint64_t>(u) * cost < 8ull * depth) u *= 2;
    while (u > 1 && u * cost > kInterleaveBudget) u /= 2;
    return u;
  }

  // A fused run over frames [t0, t0 + n). Each loop iteration evaluates `u` independent chunks (8 frames of a
  // 1-lane run, or one frame of 8 voices), emitted op by op across the chunks so the chunks' dependency chains
  // interleave: a long run is otherwise latency-bound (one serial chain per iteration, and an iteration of a long
  // run does not fit the out-of-order window), where the interpreter's op-at-a-time loops are throughput-bound.
  // What the u-chunk loop leaves is done one chunk at a time; a 1-lane run's chunks are masked to the frames that
  // exist (vmaskmov), so one body serves full and partial chunks. Every op is element-wise and none of them is an
  // exact transcendental (those are calls in slices), so the masked lanes compute exactly what the kernels' scalar
  // tail does: results stay bit-identical.
  // The prologues of the inline gains among ops [a, b) (dsp::gainPrologue: prepare, retarget, start), at the insert
  // point; returns whether all of them are settled (their residual is 0), or nullptr if there are none.
  Value* emitGainPrologues(uint32_t a, uint32_t b, Value* t0 = nullptr, Value* tLast = nullptr) {
    Value* settled = nullptr;
    for (uint32_t i = a; i < b; ++i) {
      if (!inline_[i] || (s_.ops[i].kernel != Atom::Gain && s_.ops[i].kernel != Atom::GainAdd)) continue;
      const g::Op& op = s_.ops[i];
      const auto ins = s_.inputs(op);
      const uint32_t kA = op.kernel == Atom::GainAdd ? 1u : 0u;
      const Frame f0{c64(0), 1, nullptr, false};
      Value* tau = read(ins[kA + 2], f0);
      Value* start = ins.size() > kA + 3 ? read(ins[kA + 3], f0) : static_cast<Value*>(llvm::ConstantFP::get(f32_, 0.0));
      if (rampedGain(i)) {
        // TX.smooth, a ramp-mode copy: g is the ramp's block, the kernel's audio-rate start (gainAudioPrologue: prepare,
        // a first call's `start`, no retarget) with g at the slice's first frame. The target the kernel leaves at the
        // call's end, its last g (finite or 0), is stored here, before the frames run: the audio-rate path never reads
        // it, and later ops of the run may take the ramp's buffer for their own values once its last frame is read.
        Value* g0 = read(ins[kA + 1], Frame{t0 ? t0 : c64(0), 1, nullptr, false});
        llvm::FunctionType* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(C_), {ptr_, f32_, f32_, f32_}, false);
        callPtr(ty, tabLoad(reinterpret_cast<const void*>(&dsp::gainAudioPrologue)), {statePtr(op.state), g0, tau, start});
        {
          Vx v(B_, 1);
          Value* gl = read(ins[kA + 1], Frame{tLast ? tLast : B_.CreateSub(frames64_, c64(1)), 1, nullptr, false});
          gl = v.select(v.lt(v.abs(gl), v.cf(std::numeric_limits<float>::infinity())), gl, v.zero());
          B_.CreateAlignedStore(gl, B_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), statePtr(op.state),
                                                                  offsetof(dsp::GainState, target)),
                                llvm::Align(4));
        }
        Value* d = B_.CreateAlignedLoad(
            f32_, B_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), statePtr(op.state), offsetof(dsp::GainState, d)),
            llvm::Align(4));
        Value* z = B_.CreateFCmpOEQ(d, llvm::ConstantFP::get(f32_, 0.0));
        settled = settled ? B_.CreateAnd(settled, z) : z;
        gainTarget_.erase(i);
        continue;
      }
      Value* g = read(ins[kA + 1], f0);
      // The steady case needs no call: started, the same target and the same time constant leave the state as it is
      // (retarget and prepare change nothing then).
      auto field = [&](size_t off) {
        return B_.CreateAlignedLoad(f32_, B_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), statePtr(op.state), off),
                                    llvm::Align(4));
      };
      Value* steady = B_.CreateAnd(B_.CreateFCmpUNE(field(offsetof(dsp::GainState, started)), llvm::ConstantFP::get(f32_, 0.0)),
                                   B_.CreateAnd(B_.CreateFCmpOEQ(field(offsetof(dsp::GainState, target)), g),
                                                B_.CreateFCmpOEQ(field(offsetof(dsp::GainState, tau)), tau)));
      llvm::BasicBlock* call = llvm::BasicBlock::Create(C_, "gain_prologue", F_);
      llvm::BasicBlock* after = llvm::BasicBlock::Create(C_, "gain_ready", F_);
      B_.CreateCondBr(steady, after, call);
      B_.SetInsertPoint(call);
      llvm::FunctionType* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(C_), {ptr_, f32_, f32_, f32_}, false);
      callPtr(ty, tabLoad(reinterpret_cast<const void*>(&dsp::gainPrologue)), {statePtr(op.state), g, tau, start});
      B_.CreateBr(after);
      B_.SetInsertPoint(after);
      Value* d = B_.CreateAlignedLoad(
          f32_, B_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), statePtr(op.state), offsetof(dsp::GainState, d)),
          llvm::Align(4));
      Value* z = B_.CreateFCmpOEQ(d, llvm::ConstantFP::get(f32_, 0.0));
      settled = settled ? B_.CreateAnd(settled, z) : z;
      gainTarget_[i] = B_.CreateAlignedLoad(
          f32_, B_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), statePtr(op.state), offsetof(dsp::GainState, target)),
          llvm::Align(4));
    }
    return settled;
  }
  // TX.smooth: an inline gain whose g reads a ramp, in a ramp-mode copy (the kernel's audio-rate path, frame by frame).
  bool rampedGain(uint32_t i) const {
    if (!rampMode_ || i >= inline_.size() || !inline_[i]) return false;
    const g::Op& op = s_.ops[i];
    if (op.kernel != Atom::Gain && op.kernel != Atom::GainAdd) return false;
    return rampIndexOf(s_.inputs(op)[op.kernel == Atom::GainAdd ? 2u : 1u]) != g::kNone;
  }
  // ... of the inline gains of regions [r0, r1] (a chain or a per-sample loop: before its frame loop).
  Value* emitRegionGainPrologues(uint32_t r0, uint32_t r1) {
    Value* settled = nullptr;
    Value* tLast = nullptr;
    for (uint32_t r = r0; r <= r1; ++r)
      if (Value* s = emitGainPrologues(s_.regions[r].firstOp, s_.regions[r].firstOp + s_.regions[r].numOps, nullptr,
                                       tLast ? tLast : (tLast = B_.CreateSub(regionFrames(r0), c64(1)))))
        settled = settled ? B_.CreateAnd(settled, s) : s;
    return settled;
  }
  // A frame loop whose body has inline gains: once per block, settled (every residual 0: the gains are a multiply-add
  // each) or moving (their per-frame definition). `loop` emits the loop at the insert point.
  template <class Loop>
  void emitGainBranch(Value* settled, Loop&& loop) {
    if (!settled) {
      loop();
      return;
    }
    llvm::BasicBlock* fast = llvm::BasicBlock::Create(C_, "gains_settled", F_);
    llvm::BasicBlock* slow = llvm::BasicBlock::Create(C_, "gains_moving", F_);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(C_, "gains_after", F_);
    B_.CreateCondBr(settled, fast, slow);
    B_.SetInsertPoint(slow);
    loop();
    B_.CreateBr(done);
    B_.SetInsertPoint(fast);
    gainsSettled_ = true;
    loop();
    gainsSettled_ = false;
    B_.CreateBr(done);
    B_.SetInsertPoint(done);
  }

  void emitFused(uint32_t a, uint32_t b, Value* t0, Value* n) {
    if (Value* settled = emitGainPrologues(a, b, t0, B_.CreateSub(B_.CreateAdd(t0, n, "", true, true), c64(1)))) {
      // A gain is moving (rare: ~20 time constants after a change): the run frame by frame, every gain per its
      // definition; else the vectorised run with the gains' settled form.
      llvm::BasicBlock* fast = llvm::BasicBlock::Create(C_, "gains_settled", F_);
      llvm::BasicBlock* slow = llvm::BasicBlock::Create(C_, "gains_moving", F_);
      llvm::BasicBlock* done = llvm::BasicBlock::Create(C_, "gains_after", F_);
      B_.CreateCondBr(settled, fast, slow);
      B_.SetInsertPoint(slow);
      forLoop(c64(0), n, 1, [&](Value* f) {
        Map m;
        const Frame fr{B_.CreateAdd(t0, f, "", true, true), 1, &m, true};
        for (uint32_t i = a; i < b; ++i) emitInline(i, fr);
      });
      B_.CreateBr(done);
      B_.SetInsertPoint(fast);
      emitFusedRun(a, b, t0, n);
      B_.CreateBr(done);
      B_.SetInsertPoint(done);
      return;
    }
    emitFusedRun(a, b, t0, n);
  }

  void emitFusedRun(uint32_t a, uint32_t b, Value* t0, Value* n) {
    const unsigned lanes = s_.ops[a].lanes;
    const uint64_t chunk = lanes == 8 ? 1 : 8;  // frames per <8 x float>
    // 8 chunks: a 64-frame block of a 1-lane run is one iteration with eight independent chains (measured: 4 chunks
    // reach 1.4×, 8 chunks 2× the interpreter on a 36-op fast_ chain; bench/RESULTS.md). Short runs are not
    // latency-bound (the out-of-order window overlaps their iterations) and stay at one chunk: less code. Wide
    // runs get fewer chunks (interleaveFor): one chunk of them already holds enough independent work.
    // TX.smooth: a unit's ramp-mode copy (blocks in which a param moves) is not interleaved: less code for the rare case.
    const unsigned u = rampMode_ ? 1u : interleaveFor(runCost(a, b), runDepth(a, b));
    const unsigned w = lanes == 8 ? vw_ : 8u;  // a voice run: one frame of the group's (pair's, lane's) voices
    auto bodyAt = [&](Value* t, unsigned chunks, Value* mask) {
      std::vector<Map> maps(chunks);
      std::vector<Frame> frs;
      for (unsigned c = 0; c < chunks; ++c)
        frs.push_back({c == 0 ? t : B_.CreateAdd(t, c64(c * chunk), "", true, true), w, &maps[c], true, mask});
      for (uint32_t i = a; i < b; ++i)
        for (unsigned c = 0; c < chunks; ++c) emitInline(i, frs[c]);
    };
    Value* done = c64(0);
    if (u > 1) {
      Value* nu = B_.CreateAnd(n, c64(~(u * chunk - 1)));
      forLoop(c64(0), nu, u * chunk, [&](Value* k) { bodyAt(B_.CreateAdd(t0, k, "", true, true), u, nullptr); });
      done = nu;
    }
    if (lanes == 8) {
      forLoop(done, n, 1, [&](Value* f) { bodyAt(B_.CreateAdd(t0, f, "", true, true), 1, nullptr); });
      return;
    }
    forLoop(done, n, 8, [&](Value* k) {
      // lane l exists when l < n − k (n − k ≤ 2^20: i32 is exact)
      Value* lanesIdx = llvm::ConstantVector::get({c32(0), c32(1), c32(2), c32(3), c32(4), c32(5), c32(6), c32(7)});
      Value* mask = B_.CreateICmpSLT(lanesIdx, B_.CreateVectorSplat(8, B_.CreateTrunc(B_.CreateSub(n, k), i32_)));
      bodyAt(B_.CreateAdd(t0, k, "", true, true), 1, mask);
    });
  }

  // A Control region (t = 0, 8 lanes) or one frame of a per-sample Loop region: ops and solve blocks in order.
  void emitPerFrame(const g::Region& reg, Value* t, unsigned lanes, Map& map, bool hoist) {
    const Frame fr{t, lanes, &map, hoist};
    uint32_t sIdx = reg.firstSolve;
    const uint32_t sEnd = reg.firstSolve + reg.numSolves;
    uint32_t i = reg.firstOp;
    const uint32_t end = reg.firstOp + reg.numOps;
    while (i < end || sIdx < sEnd) {
      if (sIdx < sEnd && s_.solves[sIdx].firstOp <= i) {
        const g::SolveBlock& sb = s_.solves[sIdx];
        emitSolve(sb, t, lanes, map, hoist);
        i = std::max(i, sb.firstOp + sb.numOps);
        ++sIdx;
        continue;
      }
      if (i >= end) break;
      emitFrameOp(i, fr);
      ++i;
    }
  }

  // ================================================================================================================
  // Solve blocks: Newton exactly as the interpreter (pq/engine/newton.h): forward-mode tangents through the inlined
  // body (one evaluation per iteration), finite differences for bodies without tangent rules and for lanes whose
  // tangent Jacobian is not finite.
  // ================================================================================================================
  // The arithmetic newton::tangentOp needs, as IR (fast-math flags are cleared in solve blocks).
  struct IrOps {
    using F = llvm::Value*;
    Gen& g;
    Vx& v;
    F c(float x) { return v.cf(x); }
    F add(F a, F b) { return v.add(a, b); }
    F sub(F a, F b) { return v.sub(a, b); }
    F mul(F a, F b) { return v.mul(a, b); }
    F div(F a, F b) { return v.div(a, b); }
    F neg(F a) { return v.neg(a); }
    F floor(F a) { return v.floor(a); }
    F round(F a) { return v.round(a); }
    F lt(F a, F b) { return v.lt(a, b); }
    F gt(F a, F b) { return v.gt(a, b); }
    F ge(F a, F b) { return v.ge(a, b); }
    F eq(F a, F b) { return v.eq(a, b); }
    F sel(F m, F a, F b) { return v.select(m, a, b); }
    F cosE(F x) { return g.perLaneCall(reinterpret_cast<const void*>(&newton::cosExact), x, v.width()); }
    F logE(F x) { return g.perLaneCall(reinterpret_cast<const void*>(&newton::logExact), x, v.width()); }
  };

  // float f(float) per lane through the pointer table (the interpreter calls the same function per lane).
  Value* perLaneCall(const void* fn, Value* x, unsigned w) {
    llvm::FunctionType* ty = llvm::FunctionType::get(f32_, {f32_}, false);
    auto call = [&](Value* s) {
      llvm::CallInst* c = callPtr(ty, tabLoad(fn), {s});
      c->setMemoryEffects(llvm::MemoryEffects::inaccessibleMemOnly());  // errno at most
      c->addFnAttr(llvm::Attribute::WillReturn);
      return static_cast<Value*>(c);
    };
    if (w == 1) return call(x);
    Value* r = llvm::PoisonValue::get(x->getType());
    for (unsigned l = 0; l < w; ++l) r = B_.CreateInsertElement(r, call(B_.CreateExtractElement(x, uint64_t{l})), uint64_t{l});
    return r;
  }

  void emitSolve(const g::SolveBlock& sb, Value* t, unsigned lanes, Map& outer, bool hoist) {
    const uint32_t n = sb.numUnknowns;
    // Values the solver or the body write are in memory from here on.
    for (uint32_t k = 0; k < n; ++k) outer.erase(s_.unknowns[sb.firstUnknown + k].value.index);
    for (uint32_t i = sb.firstOp; i < sb.firstOp + sb.numOps; ++i)
      for (const g::Operand& o : s_.outputsOf(s_.ops[i])) outer.erase(o.index);
    llvm::IRBuilderBase::FastMathFlagGuard guard(B_);
    B_.clearFastMathFlags();  // the interpreter's arithmetic, exactly
    Vx v(B_, lanes);
    llvm::Type* vt = v.fT();
    const newton::TangentPlan plan = newton::planTangents(s_, sb);
    std::vector<llvm::AllocaInst*> u(n);
    {
      llvm::IRBuilder<> eb(entryTerm_);
      for (uint32_t j = 0; j < n; ++j) u[j] = eb.CreateAlloca(vt);
    }
    auto unk = [&](uint32_t j) -> const g::SolveUnknown& { return s_.unknowns[sb.firstUnknown + j]; };
    auto storeUnknown = [&](uint32_t j, Value* x) {
      const g::Operand& o = unk(j).value;
      if (voice_) voiceStore(x, o, t);
      else B_.CreateAlignedStore(x, framePtr(o, t), alignOf(o, lanes));
    };
    const Frame memFrame{t, lanes, nullptr, hoist};
    auto residuals = [&]() {
      std::vector<Value*> r(n);
      for (uint32_t q = 0; q < n; ++q) {
        const g::SolveEquation& e = s_.equations[sb.firstEquation + q];
        r[q] = v.sub(read(e.lhs, memFrame), read(e.rhs, memFrame));
      }
      return r;
    };
    auto evalBody = [&]() {
      Map m;
      const Frame fr{t, lanes, &m, hoist};
      for (uint32_t i = sb.firstOp; i < sb.firstOp + sb.numOps; ++i) emitFrameOp(i, fr);
    };
    auto isFinite = [&](Value* x) {  // std::isfinite: |x| < inf, false for NaN
      return B_.CreateFCmpOLT(B_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, x), llvm::ConstantFP::getInfinity(x->getType()));
    };
    // Initial guess: the init operand, or the previous frame's solution (state).
    for (uint32_t j = 0; j < n; ++j) {
      Value* x0 = unk(j).init.kind != g::Operand::Kind::None ? read(unk(j).init, memFrame) : ldF(unk(j).state, 0, v);
      B_.CreateStore(x0, u[j]);
      storeUnknown(j, x0);
    }
    // J by finite differences around u (newton.h): relative step, one-sided where a side is not finite.
    auto fdJacobian = [&](const std::vector<Value*>& r0) {
      std::vector<Value*> J(static_cast<size_t>(n) * n);
      for (uint32_t j = 0; j < n; ++j) {
        Value* saved = B_.CreateLoad(vt, u[j]);
        Value* absS = v.abs(saved);
        Value* m = B_.CreateSelect(B_.CreateFCmpOLT(absS, v.cf(newton::kStepFloor)), v.cf(newton::kStepFloor), absS);  // std::max
        Value* step = v.mul(v.cf(newton::kStep), m);
        Value* h = B_.CreateSelect(isFinite(step), step, v.zero());
        Value* up = v.add(saved, h);
        Value* um = v.sub(saved, h);
        storeUnknown(j, up);
        evalBody();
        const std::vector<Value*> rp = residuals();
        storeUnknown(j, um);
        evalBody();
        const std::vector<Value*> rm = residuals();
        storeUnknown(j, saved);
        Value* hh = v.sub(up, um);  // the steps actually taken in float
        Value* hp = v.sub(up, saved);
        Value* hm = v.sub(saved, um);
        for (uint32_t q = 0; q < n; ++q) {
          Value* fp = isFinite(rp[q]);
          Value* fm = isFinite(rm[q]);
          Value* central = v.div(v.sub(rp[q], rm[q]), hh);
          Value* fwd = v.div(v.sub(rp[q], r0[q]), hp);
          Value* bwd = v.div(v.sub(r0[q], rm[q]), hm);
          J[q * n + j] = B_.CreateSelect(B_.CreateAnd(fp, fm), central, B_.CreateSelect(fp, fwd, bwd));
        }
      }
      return J;
    };
    // One body evaluation carrying tangents; J from the equations' tangents.
    auto adJacobian = [&]() {
      using TanV = newton::Tan<Value*>;
      Map m;
      const Frame fr{t, lanes, &m, hoist};
      std::unordered_map<uint32_t, std::vector<Value*>> tm;  // buffer → its value's tangent per direction
      for (uint32_t j = 0; j < n; ++j) {
        std::vector<Value*>& seed = tm[unk(j).value.index];
        seed.assign(n, nullptr);
        seed[j] = v.cf(1.0f);
      }
      auto tangentOf = [&](const g::Operand& o, uint32_t nzBits, uint32_t j) -> TanV {
        if (((nzBits >> j) & 1u) == 0) return {};
        return {tm.at(o.index).at(j), true};
      };
      IrOps ops{*this, v};
      for (uint32_t k = 0; k < sb.numOps; ++k) {
        const uint32_t i = sb.firstOp + k;
        const g::Op& op = s_.ops[i];
        std::vector<Value*> a;
        const auto ins = s_.inputs(op);
        const auto outs = s_.outputsOf(op);
        Value* y = nullptr;
        if (inline_[i]) {
          emitInline(i, fr, &a);
          y = m.at(outs[0].index);
        } else {  // a wide mux/demux: the rules read a[0..2] and y; take the inputs before an in-place call
          for (uint32_t q = 0; q < std::min<uint32_t>(op.numIn, 3); ++q) a.push_back(read(ins[q], fr));
          emitFrameOp(i, fr);
          y = read(outs[0], fr);
        }
        a.resize(std::max<size_t>(a.size(), 3), nullptr);
        std::vector<std::vector<Value*>> res(op.numOut, std::vector<Value*>(n, nullptr));
        std::vector<TanV> d(op.numIn), dy(op.numOut);
        uint32_t any = 0;
        for (uint32_t o = 0; o < op.numOut; ++o) any |= plan.outNz[plan.firstOut[k] + o];
        for (uint32_t j = 0; j < n; ++j) {
          if (((any >> j) & 1u) == 0) continue;
          for (uint32_t q = 0; q < op.numIn; ++q) d[q] = tangentOf(ins[q], plan.inNz[plan.firstIn[k] + q], j);
          newton::tangentOp(ops, static_cast<Atom>(op.kernel), op.numIn, op.numOut, a.data(), &y, d.data(), dy.data());
          for (uint32_t o = 0; o < op.numOut; ++o)
            if ((plan.outNz[plan.firstOut[k] + o] >> j) & 1u) res[o][j] = dy[o].v;
        }
        for (uint32_t o = 0; o < op.numOut; ++o) tm[outs[o].index] = std::move(res[o]);
      }
      std::vector<Value*> J(static_cast<size_t>(n) * n);
      for (uint32_t q = 0; q < n; ++q) {
        const g::SolveEquation& e = s_.equations[sb.firstEquation + q];
        for (uint32_t j = 0; j < n; ++j) {
          const TanV l = tangentOf(e.lhs, plan.lhsNz[q], j), r = tangentOf(e.rhs, plan.rhsNz[q], j);
          J[q * n + j] = l.on && r.on ? v.sub(l.v, r.v) : l.on ? l.v : r.on ? v.neg(r.v) : v.zero();
        }
      }
      return J;
    };
    auto iteration = [&]() {
      std::vector<Value*> J, r0;
      if (plan.ad) {
        J = adJacobian();
        r0 = residuals();
        // Lanes whose tangent Jacobian is not finite take finite differences (a rarely taken branch).
        Value* bad = nullptr;
        for (Value* x : J) {
          Value* nf = B_.CreateNot(isFinite(x));
          bad = bad ? B_.CreateOr(bad, nf) : nf;
        }
        Value* anyBad = lanes == 1 ? bad : B_.CreateOrReduce(bad);
        llvm::BasicBlock* pre = B_.GetInsertBlock();
        llvm::BasicBlock* fdBB = llvm::BasicBlock::Create(C_, "newton_fd", F_);
        llvm::BasicBlock* join = llvm::BasicBlock::Create(C_, "newton_j", F_);
        B_.CreateCondBr(anyBad, fdBB, join);
        B_.SetInsertPoint(fdBB);
        const std::vector<Value*> Jfd = fdJacobian(r0);
        std::vector<Value*> Jsel(J.size());
        for (size_t e = 0; e < J.size(); ++e) Jsel[e] = B_.CreateSelect(bad, Jfd[e], J[e]);
        llvm::BasicBlock* fdEnd = B_.GetInsertBlock();
        B_.CreateBr(join);
        B_.SetInsertPoint(join);
        for (size_t e = 0; e < J.size(); ++e) {
          llvm::PHINode* p = B_.CreatePHI(vt, 2);
          p->addIncoming(J[e], pre);
          p->addIncoming(Jsel[e], fdEnd);
          J[e] = p;
        }
      } else {
        evalBody();
        r0 = residuals();
        J = fdJacobian(r0);
      }
      if (n == 1) {
        llvm::Type* dt = lanes == 1 ? f64_ : static_cast<llvm::Type*>(llvm::FixedVectorType::get(f64_, lanes));  // 1, 8, 16
        Value* uu = B_.CreateLoad(vt, u[0]);
        Value* d = B_.CreateFDiv(B_.CreateFPExt(r0[0], dt), B_.CreateFPExt(J[0], dt));
        Value* nu = B_.CreateFPTrunc(B_.CreateFSub(B_.CreateFPExt(uu, dt), d), vt);
        Value* ok = B_.CreateAnd(isFinite(d), isFinite(nu));
        B_.CreateStore(B_.CreateSelect(ok, nu, uu), u[0]);
      } else {
        // Per lane: Gaussian elimination in double with partial pivoting (newton::linearUpdate, shared with the interpreter).
        const uint32_t N8 = L_.maxSolveN * 8;
        Value* uS = B_.CreateConstInBoundsGEP1_64(f32_, buf_, L_.solveOff);
        Value* rS = B_.CreateConstInBoundsGEP1_64(f32_, buf_, L_.solveOff + N8);
        Value* jS = B_.CreateConstInBoundsGEP1_64(f32_, buf_, L_.solveOff + 2 * N8);
        Value* mS = B_.CreateConstInBoundsGEP1_64(f32_, buf_, L_.solveOff + 2 * N8 + L_.maxSolveN * N8);
        // A pair solves its halves one after the other in the same scratch (8 lanes each).
        const unsigned halves = lanes == 16 ? 2u : 1u;
        const unsigned hl = lanes == 16 ? 8u : lanes;
        llvm::Type* ht = hl == 1 ? f32_ : static_cast<llvm::Type*>(llvm::FixedVectorType::get(f32_, hl));
        const llvm::Align al(hl == 8 ? 32 : 4);
        auto part = [&](Value* x, unsigned h) { return halves == 2 ? half(x, h) : x; };
        std::vector<Value*> uh(n);
        for (unsigned h = 0; h < halves; ++h) {
          for (uint32_t j = 0; j < n; ++j)
            B_.CreateAlignedStore(part(B_.CreateLoad(vt, u[j]), h), B_.CreateConstInBoundsGEP1_64(f32_, uS, j * 8), al);
          for (uint32_t q = 0; q < n; ++q) B_.CreateAlignedStore(part(r0[q], h), B_.CreateConstInBoundsGEP1_64(f32_, rS, q * 8), al);
          for (uint32_t q = 0; q < n * n; ++q) B_.CreateAlignedStore(part(J[q], h), B_.CreateConstInBoundsGEP1_64(f32_, jS, q * 8), al);
          llvm::FunctionType* ty = llvm::FunctionType::get(void_, {ptr_, ptr_, ptr_, ptr_, i32_, i32_}, false);
          llvm::CallInst* c = callPtr(ty, tabLoad(reinterpret_cast<const void*>(&engine::newton::linearUpdate)), {uS, jS, rS, mS, c32(n), c32(hl)});
          c->setMemoryEffects(llvm::MemoryEffects::argMemOnly());
          c->addFnAttr(llvm::Attribute::WillReturn);
          for (uint32_t j = 0; j < n; ++j) {
            Value* x = B_.CreateAlignedLoad(ht, B_.CreateConstInBoundsGEP1_64(f32_, uS, j * 8), al);
            uh[j] = h == 0 ? x : concat(uh[j], x);
          }
        }
        for (uint32_t j = 0; j < n; ++j) B_.CreateStore(uh[j], u[j]);
      }
      for (uint32_t j = 0; j < n; ++j) storeUnknown(j, B_.CreateLoad(vt, u[j]));
    };
    const uint32_t iterations = std::max<uint32_t>(sb.iterations, 1);
    if (iterations == 1) iteration();
    else forLoop(c64(0), c64(iterations), 1, [&](Value*) { iteration(); });
    if (newton::bodyReadAfter(s_, sb)) evalBody();  // body values read after the block match the solution
    for (uint32_t j = 0; j < n; ++j)
      if (unk(j).state != g::kNone) st(unk(j).state, 0, B_.CreateLoad(vt, u[j]), v);
  }

  const g::Schedule& s_;
  const HostLayout& L_;
  PtrTable& tab_;
  llvm::Module& M_;
  llvm::LLVMContext& C_;
  llvm::IRBuilder<> B_;
  const GenOptions& o_;
  llvm::FastMathFlags fmf_;
  llvm::Type *f32_ = nullptr, *f64_ = nullptr, *i16_ = nullptr, *i32_ = nullptr, *i64_ = nullptr, *ptr_ = nullptr,
             *void_ = nullptr;
  llvm::StructType* kargsTy_ = nullptr;
  GenResult res_;
  // analysis
  std::vector<bool> inline_, body_, store_;
  std::vector<int32_t> grp_;
  std::vector<uint32_t> opRegion_;
  std::vector<int32_t> chainEnd_;  // per region: the last region of the chain it starts, or −1
  std::vector<uint32_t> chainCost_;  // per stage: the cost of its largest chain (T4.12: pair functions)
  std::vector<uint8_t> memo_;        // per region: a memo Control region (Q126)
  std::vector<uint8_t> masked_;      // per region: a memo region run in param-mask runs (TX.vca-knob2)
  // TX.uniform-branch (analyseBranches): per branch whether the slices take it, per op its taken guard and (for a taken
  // branch's select) its branch; while emitting a Block region, its guards' and branches' i1 flags.
  std::vector<uint8_t> taken_;
  std::vector<uint32_t> guardEff_, branchOf_;
  std::unordered_map<uint32_t, Value*> guardOn_, branchAll_, branchNone_;
  uint32_t pickOp_ = g::kNone;
  int pick_ = -1;
  // current function
  bool rampMode_ = false;          // TX.smooth: emitting a unit's copy for a block in which one of its ramps moves
  bool steady_ = false;            // TX.smooth: emitting a stage's code for a block in which every ramp rests
  std::vector<uint8_t> unitHeld_;  // TX.smooth: per ramp, filled by the ramp-mode function's prologue (stageFunction)
  bool voice_ = false;             // a voice stage's function
  unsigned vw_ = 8;                // its voice width: 1 (scalar lanes), 8 (a group), 16 (a pair)
  std::vector<uint8_t> local_;     // pair functions: per buffer, a value of this stage kept per group (altOff)
  std::vector<uint8_t> staged_;    // pair functions: per state entry, staged in 16-lane stack slots
  std::unordered_map<uint64_t, llvm::Value*> stageSlot_;  // (entry, lane-array offset) → its slot
  llvm::Function* F_ = nullptr;
  llvm::Instruction* entryTerm_ = nullptr;
  Value *buf_ = nullptr, *aux_ = nullptr, *gstate_ = nullptr, *vstate_ = nullptr, *vstate1_ = nullptr, *ext_ = nullptr,
        *tabArg_ = nullptr;
  Value *group32_ = nullptr, *frames64_ = nullptr, *group64_ = nullptr, *group1_64_ = nullptr, *lane64_ = nullptr;
  std::map<uint32_t, Value*> framesF_;  // TMM.6: regionFrames(), per oversample factor
  Value* meterOn_ = nullptr;  // i1: this block is metered (T4.5)
  llvm::AllocaInst *kargs_ = nullptr, *inArr_ = nullptr, *outArr_ = nullptr, *slot_ = nullptr;
  std::unordered_map<uint32_t, Value*> bufBase_, extPtr_, tabLoad_, statePtr_;
  Value* sideBase_[3] = {nullptr, nullptr, nullptr};  // TX.smooth-fix1: the ramp side table (global, half 0, half 1)
  llvm::Instruction* hoistIP_ = nullptr;
  std::map<std::tuple<int, uint32_t, unsigned>, Value*> inv_;
};

}  // namespace

Result<GenResult> generate(const g::Schedule& s, const HostLayout& layout, PtrTable& tab, llvm::Module& m,
                           const GenOptions& options) {
  return Gen(s, layout, tab, m, options).run();
}

std::vector<uint32_t> meterUnits(const g::Schedule& s, const HostLayout& layout, const GenOptions& options) {
  llvm::LLVMContext ctx;
  llvm::Module mod("pq_units", ctx);
  PtrTable tab;
  return Gen(s, layout, tab, mod, options).units();
}

std::vector<uint8_t> pairPlan(const g::Schedule& s, const HostLayout& layout, const GenOptions& options) {
  llvm::LLVMContext ctx;
  llvm::Module m("pq_plan", ctx);
  PtrTable tab;
  return Gen(s, layout, tab, m, options).pairPlan();
}

}  // namespace pq::jit::detail
