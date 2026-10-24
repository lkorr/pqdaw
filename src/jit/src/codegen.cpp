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
#include <llvm/IR/Verifier.h>
#include <llvm/Support/ModRef.h>
#include <llvm/Support/raw_ostream.h>

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
bool isSimpleStateful(Atom a) noexcept {
  switch (a) {
    case Atom::Schmitt: case Atom::Rise: case Atom::Fall: case Atom::Change: case Atom::SrFlipFlop:
    case Atom::DFlipFlop: case Atom::TFlipFlop: case Atom::Counter: case Atom::ClockDiv: case Atom::Pulse:
    case Atom::Timer: case Atom::Z1: case Atom::Z1Read: case Atom::Z1Write: case Atom::SampleHold: case Atom::Latch:
    case Atom::Phasor: case Atom::SineHz:
      return true;
    default:
      return false;
  }
}

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
  unsigned w = 1;       // 1: scalar, 8: <8 x float>
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

  Result<GenResult> run() {
    analyse();
    for (uint32_t k = 0; k < s_.stages.size(); ++k) {
      const std::string name = o_.prefix + "_stage" + std::to_string(k);
      stageFunction(s_.stages[k], name);
      res_.stageFunctions.push_back(name);
    }
    for (uint32_t i = 0; i < s_.ops.size(); ++i) (inline_[i] ? res_.inlinedOps : res_.calledOps) += 1;
    std::string err;
    llvm::raw_string_ostream os(err);
    if (llvm::verifyModule(M_, &os)) return Error{ErrorCode::Unknown, "jit: generated IR does not verify: " + err, {}};
    return res_;
  }

private:
  // ================================================================================================================
  // Analysis: how each op is emitted, and which op outputs must reach memory.
  // ================================================================================================================
  bool perFrame(const g::Region& r) const noexcept {
    return r.kind == g::RegionKind::Control || (r.kind == g::RegionKind::Loop && (r.subBlock <= 1 || r.numSolves > 0));
  }

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
    int32_t next = 0;
    for (const g::Region& r : s_.regions) {
      const bool pf = perFrame(r);
      const int32_t regionGroup = next++;
      int32_t run = -1;
      uint32_t runLanes = 0, runSize = 0;
      std::set<std::pair<int, uint32_t>> runInv;  // the run's distinct pattern operands (hoisted loads)
      for (uint32_t i = r.firstOp; i < r.firstOp + r.numOps; ++i) {
        const g::Op& op = s_.ops[i];
        const bool kernel = op.kind == g::OpKind::Kernel;
        // In slices, exact transcendentals stay block-wise kernel calls: the CRT/SVML call is the cost, and a call
        // inside a fused loop would spill every live vector register around it.
        // A mux/demux wider than kStackCallOperands is a kernel call: inline, its select chain is one basic block
        // of thousands of instructions per chunk (the M1 review's 1200-input mux), the kernel runs it as a loop.
        inline_[i] = kernel && !wideCall(op) &&
                     ((isElementwise(op.kernel) && (pf || exactOf(op.kernel) < 0)) || (pf && isSimpleStateful(op.kernel)));
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
        for (const g::Operand& o : s_.inputs(op))
          if (isPattern(o) && o.kind != g::Operand::Kind::None && !runInv.count({static_cast<int>(o.kind), o.index})) ++newInv;
        if (run < 0 || runLanes != op.lanes || (runSize > 0 && runSize + c > kMaxRunCost) ||
            (runSize > 0 && runInv.size() + newInv > kMaxRunInvariants)) {
          run = next++;
          runLanes = op.lanes;
          runSize = 0;
          runInv.clear();
        }
        runSize += c;
        for (const g::Operand& o : s_.inputs(op))
          if (isPattern(o) && o.kind != g::Operand::Kind::None) runInv.insert({static_cast<int>(o.kind), o.index});
        grp_[i] = run;
      }
    }
    // Store elimination. Accesses per buffer in op order (an op's reads before its writes).
    std::vector<bool> always(s_.buffers.size(), false);
    auto mark = [&](const g::Operand& o) {
      if (o.kind == g::Operand::Kind::Buffer && o.index < always.size()) always[o.index] = true;
    };
    for (const g::OutputRef& o : s_.outputs) mark(o.value);
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
  void stageFunction(const g::Stage& st, const std::string& name) {
    // The kernel-call pointer arrays are sized for this stage's calls only (an inline op never uses them): the
    // M1 review's 1200-input mux sized them at 2 × 9.6 KB for every stage, a frame that needs __chkstk probes.
    uint32_t maxOperands = 1;
    for (uint32_t r = st.firstRegion; r < st.firstRegion + st.numRegions; ++r)
      for (uint32_t i = s_.regions[r].firstOp; i < s_.regions[r].firstOp + s_.regions[r].numOps; ++i)
        if (!inline_[i] && !wideCall(s_.ops[i]))
          maxOperands = std::max<uint32_t>(maxOperands, std::max<uint32_t>(s_.ops[i].numIn, s_.ops[i].numOut));
    beginFunction(name, llvm::Function::ExternalLinkage, maxOperands);
    for (uint32_t r = st.firstRegion; r < st.firstRegion + st.numRegions; ++r) emitRegion(r);
    B_.CreateRetVoid();
  }

  llvm::Function* beginFunction(const std::string& name, llvm::Function::LinkageTypes linkage, uint32_t maxOperands) {
    llvm::FunctionType* ty = llvm::FunctionType::get(void_, {ptr_, ptr_, ptr_, ptr_, ptr_, ptr_, i32_, i32_}, false);
    F_ = llvm::Function::Create(ty, linkage, name, M_);
    for (unsigned a = 0; a < 6; ++a) F_->addParamAttr(a, llvm::Attribute::NoAlias);
    for (unsigned a : {1u, 4u, 5u}) F_->addParamAttr(a, llvm::Attribute::ReadOnly);
    F_->addFnAttr(llvm::Attribute::NoUnwind);
    F_->addFnAttr("no-builtins");  // no memset/memcpy calls the JIT would have to resolve
    if (!o_.cpu.empty()) F_->addFnAttr("target-cpu", o_.cpu);
    if (!o_.features.empty()) F_->addFnAttr("target-features", o_.features);
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
    ext_ = &*arg++;
    tabArg_ = &*arg++;
    Value* frames32 = &*arg++;
    group32_ = &*arg;
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(C_, "entry", F_);
    llvm::BasicBlock* start = llvm::BasicBlock::Create(C_, "start", F_);
    B_.SetInsertPoint(entry);
    entryTerm_ = B_.CreateBr(start);
    B_.SetInsertPoint(start);
    B_.setFastMathFlags(fmf_);
    bufBase_.clear();
    extPtr_.clear();
    tabLoad_.clear();
    statePtr_.clear();
    inv_.clear();
    hoistIP_ = nullptr;
    {
      llvm::IRBuilder<> eb(entryTerm_);
      frames64_ = eb.CreateZExt(frames32, i64_);
      group64_ = eb.CreateZExt(group32_, i64_);
      kargs_ = eb.CreateAlloca(kargsTy_);
      inArr_ = eb.CreateAlloca(llvm::ArrayType::get(ptr_, maxOperands));
      outArr_ = eb.CreateAlloca(llvm::ArrayType::get(ptr_, maxOperands));
      slot_ = eb.CreateAlloca(llvm::ArrayType::get(f32_, 16));
      slot_->setAlignment(llvm::Align(32));
    }
    return F_;
  }

  llvm::Constant* c64(uint64_t v) const { return llvm::ConstantInt::get(i64_, v); }
  llvm::Constant* c32(uint32_t v) const { return llvm::ConstantInt::get(i32_, v); }

  // A buffer's base: buf + a constant, emitted where it is used (`at`, default the current insert point), so it folds
  // into the instruction's addressing mode. (Computed once in the entry block, every buffer's base was a value live
  // across the whole function: with ~1200 buffers they were spilled, the frame needed stack probes and register
  // allocation took seconds.) A per-group buffer's base needs the group index: computed once, in the entry block.
  Value* bufBase(uint32_t b, llvm::IRBuilderBase* at = nullptr) {
    if (!s_.buffers[b].perGroup) return (at ? *at : B_).CreateConstInBoundsGEP1_64(f32_, buf_, L_.bufOff[b]);
    auto it = bufBase_.find(b);
    if (it != bufBase_.end()) return it->second;
    llvm::IRBuilder<> eb(entryTerm_);
    Value* p = eb.CreateConstInBoundsGEP1_64(f32_, buf_, L_.bufOff[b]);
    p = eb.CreateInBoundsGEP(f32_, p, eb.CreateMul(group64_, c64(L_.bufSize[b])));
    bufBase_.emplace(b, p);
    return p;
  }
  Value* extBase(uint32_t e) {
    auto it = extPtr_.find(e);
    if (it != extPtr_.end()) return it->second;
    llvm::IRBuilder<> eb(entryTerm_);
    Value* idx = c64(static_cast<uint64_t>(e) * L_.groups);
    if (s_.externals[e].poly == g::Poly::Voice) idx = eb.CreateAdd(idx, group64_);
    Value* p = eb.CreateLoad(ptr_, eb.CreateInBoundsGEP(ptr_, ext_, idx));
    extPtr_.emplace(e, p);
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
  Value* statePtr(uint32_t entry) {
    auto it = statePtr_.find(entry);
    if (it != statePtr_.end()) return it->second;
    const g::StateEntry& e = s_.state.entries[entry];
    llvm::IRBuilder<> eb(entryTerm_);
    Value* p = eb.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), e.arena == g::Poly::Voice ? vstate_ : gstate_, e.offset);
    statePtr_.emplace(entry, p);
    return p;
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
  Value* loadPattern(const g::Operand& o, unsigned w, bool hoist) {
    const auto key = std::make_tuple(static_cast<int>(o.kind), o.index, w);
    if (hoist && hoistIP_) {
      auto it = inv_.find(key);
      if (it != inv_.end()) return it->second;
      llvm::IRBuilder<> hb(hoistIP_);
      Value* v = hb.CreateAlignedLoad(vecTy(w), basePtr(o, &hb), alignOf(o, w));
      inv_.emplace(key, v);
      return v;
    }
    return B_.CreateAlignedLoad(vecTy(w), basePtr(o), alignOf(o, w));
  }

  Value* read(const g::Operand& o, const Frame& fr) {
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

  // Op i as a kernel/helper call on frames [t0, t0 + n).
  void emitCall(uint32_t i, Value* t0, Value* n32) {
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
        if (op.state != g::kNone) state = statePtr(op.state);
        if (L_.opInfo[i] != g::kNone) resource = tabSlot(L_.opInfo[i]);
        break;
      }
      case g::OpKind::Broadcast:
        fn = reinterpret_cast<const void*>(&rt::broadcastKernel);
        spl = isPattern(ins[0]) ? 1u : 0u;
        break;
      case g::OpKind::Reduce:
        fn = reinterpret_cast<const void*>(&rt::reduceKernel);
        state = tabSlot(L_.opInfo[i]);
        break;
      case g::OpKind::Send:
        fn = reinterpret_cast<const void*>(&rt::sendKernel);
        state = tabSlot(L_.opInfo[i]);
        break;
    }
    // Pointer arrays: on the stack, or for a wide call in buf (jit_image.h kStackCallOperands).
    Value* inArr = inArr_;
    Value* outArr = outArr_;
    if (wideCall(op)) {
      inArr = B_.CreateConstInBoundsGEP1_64(f32_, buf_, L_.callOff);
      outArr = B_.CreateConstInBoundsGEP1_64(ptr_, inArr, ins.size());
    }
    for (uint32_t j = 0; j < ins.size(); ++j) B_.CreateStore(framePtr(ins[j], t0), B_.CreateConstInBoundsGEP1_64(ptr_, inArr, j));
    for (uint32_t k = 0; k < outs.size(); ++k) B_.CreateStore(framePtr(outs[k], t0), B_.CreateConstInBoundsGEP1_64(ptr_, outArr, k));
    B_.CreateStore(inArr, B_.CreateStructGEP(kargsTy_, kargs_, 0));
    B_.CreateStore(outArr, B_.CreateStructGEP(kargsTy_, kargs_, 1));
    B_.CreateStore(state, B_.CreateStructGEP(kargsTy_, kargs_, 2));
    B_.CreateStore(resource, B_.CreateStructGEP(kargsTy_, kargs_, 3));
    B_.CreateStore(n32, B_.CreateStructGEP(kargsTy_, kargs_, 4));
    B_.CreateStore(llvm::ConstantInt::get(i16_, op.numIn), B_.CreateStructGEP(kargsTy_, kargs_, 5));
    B_.CreateStore(llvm::ConstantInt::get(i16_, op.numOut), B_.CreateStructGEP(kargsTy_, kargs_, 6));
    B_.CreateStore(c32(spl), B_.CreateStructGEP(kargsTy_, kargs_, 7));
    llvm::FunctionType* ty = llvm::FunctionType::get(void_, {ptr_}, false);
    callPtr(ty, tabLoad(fn), {kargs_});
  }

  uint32_t regionOf(uint32_t op) const { return opRegion_[op]; }
  static bool wideCall(const g::Op& op) noexcept { return op.numIn > kStackCallOperands || op.numOut > kStackCallOperands; }

  // Exact transcendental through the CRT (scalar) or SVML (8-wide) wrapper.
  Value* exact(rt::Exact e, Value* a, Value* b, unsigned w) {
    const bool binary = rt::exactIsBinary(e);
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
  Value* ldF(Value* sp, uint32_t off, Vx& v) {
    return B_.CreateAlignedLoad(v.fT(), B_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), sp, off),
                                llvm::Align(v.width() == 8 ? 32 : 4));
  }
  Value* ldI(Value* sp, uint32_t off, Vx& v) {
    return B_.CreateAlignedLoad(v.iT(), B_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), sp, off),
                                llvm::Align(v.width() == 8 ? 32 : 4));
  }
  void st(Value* sp, uint32_t off, Value* x, Vx& v) {
    B_.CreateAlignedStore(x, B_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), sp, off),
                          llvm::Align(v.width() == 8 ? 32 : 4));
  }
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
    // ---- stateful (per frame only) ----
    Value* sp = statePtr(op.state);
    switch (op.kernel) {
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
        Value* scale = B_.CreateAlignedLoad(f64_, B_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), sp, 32), llvm::Align(8));
        Value* halfSr = splatScalar(
            B_.CreateAlignedLoad(f32_, B_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(C_), sp, 40), llvm::Align(8)), v.width());
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
    emitCall(i, fr.t, c32(1));
    const g::Op& op = s_.ops[i];
    if (fr.map)
      for (const g::Operand& o : s_.outputsOf(op)) fr.map->erase(o.index);
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

  void emitRegion(uint32_t ri) {
    const g::Region& reg = s_.regions[ri];
    if (reg.kind == g::RegionKind::Control) {
      Map m;
      emitPerFrame(reg, c64(0), 8, m, false);
      return;
    }
    // Preheader for the region's invariant pattern loads.
    llvm::BasicBlock* body = llvm::BasicBlock::Create(C_, "region", F_);
    hoistIP_ = B_.CreateBr(body);
    inv_.clear();
    B_.SetInsertPoint(body);
    const unsigned lanes = reg.poly == g::Poly::Voice ? 8u : 1u;
    if (reg.kind == g::RegionKind::Block) {
      emitSlice(reg.firstOp, reg.firstOp + reg.numOps, c64(0), frames64_);
    } else if (perFrame(reg)) {
      forLoop(c64(0), frames64_, 1, [&](Value* t) {
        Map m;
        emitPerFrame(reg, t, lanes, m, true);
      });
    } else {
      const uint32_t sub = reg.subBlock;
      forLoop(c64(0), frames64_, sub, [&](Value* t) {
        Value* n = B_.CreateBinaryIntrinsic(llvm::Intrinsic::smin, c64(sub), B_.CreateSub(frames64_, t));
        emitSlice(reg.firstOp, reg.firstOp + reg.numOps, t, n);
      });
    }
    hoistIP_ = nullptr;
    inv_.clear();
  }

  // Ops [a, b) of a Block region or sub-block on frames [t0, t0 + n): fused runs and calls.
  void emitSlice(uint32_t a, uint32_t b, Value* t0, Value* n) {
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
  void emitFused(uint32_t a, uint32_t b, Value* t0, Value* n) {
    const unsigned lanes = s_.ops[a].lanes;
    const uint64_t chunk = lanes == 8 ? 1 : 8;  // frames per <8 x float>
    // 8 chunks: a 64-frame block of a 1-lane run is one iteration with eight independent chains (measured: 4 chunks
    // reach 1.4×, 8 chunks 2× the interpreter on a 36-op fast_ chain; bench/RESULTS.md). Short runs are not
    // latency-bound (the out-of-order window overlaps their iterations) and stay at one chunk: less code. Wide
    // runs get fewer chunks (interleaveFor): one chunk of them already holds enough independent work.
    const unsigned u = interleaveFor(runCost(a, b), runDepth(a, b));
    auto bodyAt = [&](Value* t, unsigned chunks, Value* mask) {
      std::vector<Map> maps(chunks);
      std::vector<Frame> frs;
      for (unsigned c = 0; c < chunks; ++c)
        frs.push_back({c == 0 ? t : B_.CreateAdd(t, c64(c * chunk), "", true, true), 8, &maps[c], true, mask});
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
      B_.CreateAlignedStore(x, framePtr(o, t), alignOf(o, lanes));
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
      Value* x0 = unk(j).init.kind != g::Operand::Kind::None ? read(unk(j).init, memFrame) : ldF(statePtr(unk(j).state), 0, v);
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
        llvm::Type* dt = lanes == 1 ? f64_ : static_cast<llvm::Type*>(llvm::FixedVectorType::get(f64_, lanes));
        Value* uu = B_.CreateLoad(vt, u[0]);
        Value* d = B_.CreateFDiv(B_.CreateFPExt(r0[0], dt), B_.CreateFPExt(J[0], dt));
        Value* nu = B_.CreateFPTrunc(B_.CreateFSub(B_.CreateFPExt(uu, dt), d), vt);
        Value* ok = B_.CreateAnd(isFinite(d), isFinite(nu));
        B_.CreateStore(B_.CreateSelect(ok, nu, uu), u[0]);
      } else {
        // Per lane: Gaussian elimination in double with partial pivoting (rt::newtonLinear).
        const uint32_t N8 = L_.maxSolveN * 8;
        Value* uS = B_.CreateConstInBoundsGEP1_64(f32_, buf_, L_.solveOff);
        Value* rS = B_.CreateConstInBoundsGEP1_64(f32_, buf_, L_.solveOff + N8);
        Value* jS = B_.CreateConstInBoundsGEP1_64(f32_, buf_, L_.solveOff + 2 * N8);
        Value* mS = B_.CreateConstInBoundsGEP1_64(f32_, buf_, L_.solveOff + 2 * N8 + L_.maxSolveN * N8);
        const llvm::Align al(lanes == 8 ? 32 : 4);
        for (uint32_t j = 0; j < n; ++j)
          B_.CreateAlignedStore(B_.CreateLoad(vt, u[j]), B_.CreateConstInBoundsGEP1_64(f32_, uS, j * 8), al);
        for (uint32_t q = 0; q < n; ++q) B_.CreateAlignedStore(r0[q], B_.CreateConstInBoundsGEP1_64(f32_, rS, q * 8), al);
        for (uint32_t q = 0; q < n * n; ++q) B_.CreateAlignedStore(J[q], B_.CreateConstInBoundsGEP1_64(f32_, jS, q * 8), al);
        llvm::FunctionType* ty = llvm::FunctionType::get(void_, {ptr_, ptr_, ptr_, ptr_, i32_, i32_}, false);
        llvm::CallInst* c = callPtr(ty, tabLoad(reinterpret_cast<const void*>(&rt::newtonLinear)), {uS, jS, rS, mS, c32(n), c32(lanes)});
        c->setMemoryEffects(llvm::MemoryEffects::argMemOnly());
        c->addFnAttr(llvm::Attribute::WillReturn);
        for (uint32_t j = 0; j < n; ++j)
          B_.CreateStore(B_.CreateAlignedLoad(vt, B_.CreateConstInBoundsGEP1_64(f32_, uS, j * 8), al), u[j]);
      }
      for (uint32_t j = 0; j < n; ++j) storeUnknown(j, B_.CreateLoad(vt, u[j]));
    };
    const uint32_t iterations = std::max<uint32_t>(sb.iterations, 1);
    if (iterations == 1) iteration();
    else forLoop(c64(0), c64(iterations), 1, [&](Value*) { iteration(); });
    evalBody();  // body values read after the block match the solution
    for (uint32_t j = 0; j < n; ++j)
      if (unk(j).state != g::kNone) st(statePtr(unk(j).state), 0, B_.CreateLoad(vt, u[j]), v);
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
  // current function
  llvm::Function* F_ = nullptr;
  llvm::Instruction* entryTerm_ = nullptr;
  Value *buf_ = nullptr, *aux_ = nullptr, *gstate_ = nullptr, *vstate_ = nullptr, *ext_ = nullptr, *tabArg_ = nullptr;
  Value *group32_ = nullptr, *frames64_ = nullptr, *group64_ = nullptr;
  llvm::AllocaInst *kargs_ = nullptr, *inArr_ = nullptr, *outArr_ = nullptr, *slot_ = nullptr;
  std::unordered_map<uint32_t, Value*> bufBase_, extPtr_, tabLoad_, statePtr_;
  llvm::Instruction* hoistIP_ = nullptr;
  std::map<std::tuple<int, uint32_t, unsigned>, Value*> inv_;
};

}  // namespace

Result<GenResult> generate(const g::Schedule& s, const HostLayout& layout, PtrTable& tab, llvm::Module& m,
                           const GenOptions& options) {
  return Gen(s, layout, tab, m, options).run();
}

}  // namespace pq::jit::detail
