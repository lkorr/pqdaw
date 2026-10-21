#pragma once

// Tests only (T1.5-fix1): the schedule-replay buffer-aliasing checker.
//
// The scheduler gives every op output a virtual value and then maps values onto shared physical buffers (interval
// reuse, in-place ops). A mistake there is silent: the overwritten value is just a wrong number, on every backend
// alike (M1 review C1/C2). This checker proves, for one Schedule, that no buffer is overwritten while a value in it
// is still going to be read.
//
// How: the same patch is scheduled twice, once normally and once with ScheduleOptions::shareBuffers = false (every
// value its own buffer, no in-place: buffer index = value identity; everything else is identical). The checker
// replays the documented execution order (schedule.h "How a backend runs a Schedule") on the shared schedule with
// tags instead of numbers: every write stores (value, serial number) into the physical buffer slot it hits, and
// every read asserts that the slot still holds the latest write of the value the read means. The replay covers
//   - `blocks` blocks (a read must see a write of the current block: no stale value from the previous one);
//   - `groups` voice groups per voice stage (perGroup buffers have one copy per group, others are shared by the
//     groups: a value of group 0 read after group 1 ran is caught), reducer accumulators initialised before the
//     group loop and combined in every group;
//   - `frames` abstract frames per block: Block-region ops write every frame, every Loop region runs one iteration
//     per frame (so each loop runs at least twice), patterns (Spl) have one slot shared by all frames;
//   - solve blocks per frame with the solver's buffer traffic: init reads, all unknowns written, then per Newton
//     iteration (at most `newton`) a body evaluation, residual reads (lhs, rhs) and unknown reads, and for each
//     unknown a perturbed write + body + reads twice (central differences) and a restore; then the update write
//     and, at the end, one more body evaluation (whose values later ops read);
//   - the engine's reads: voice outputs at the end of their stage per group, global outputs after the last stage.
// Op-level checks: an output may share a physical buffer with an input only as output 0 of an in-place-capable
// (dsp::KernelInfo::inPlaceOk) kernel; outputs of one op are distinct; a kernel op has the kernel's input and output
// counts; a value always maps to the same physical buffer, of the same class, lanes and perGroup flag.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pq/dsp/atoms.h"
#include "pq/graph/schedule.h"

namespace sched_replay {

using namespace pq::graph;
namespace dsp = pq::dsp;

struct Options {
  uint32_t groups = 2;   // voice groups per voice stage
  uint32_t frames = 2;   // abstract frames per block (= iterations of every Loop region)
  uint32_t blocks = 2;
  uint32_t newton = 2;   // Newton iterations replayed per solve (capped by the block's own count)
};

struct Coverage {
  uint64_t reads = 0, writes = 0, bodyEvaluations = 0, outputReads = 0, solveReads = 0;
};

class Replay {
public:
  Replay(const Schedule& shared, const Schedule& unique, Options o) : s_(shared), u_(unique), o_(o) {}

  // "" when every read sees its value; otherwise the first violation.
  std::string run() {
    if (!structure()) return err_;
    layout();
    for (uint32_t blk = 0; blk < o_.blocks && err_.empty(); ++blk) {
      block_ = blk;
      blockStart_ = serial_ + 1;
      for (const Stage& sg : s_.stages) {
        if (!err_.empty()) break;
        if (sg.poly == Poly::Global) {
          runStage(sg, -1);
          continue;
        }
        // Reducer accumulators: initialised once before the group loop.
        for (uint32_t i = sg.firstOp; i < sg.firstOp + sg.numOps; ++i)
          if (s_.ops[i].kind == OpKind::Reduce)
            for (uint32_t k = 0; k < s_.ops[i].numOut; ++k)
              writeAll(s_.ops[i].firstOperand + s_.ops[i].numIn + k, -1, "reducer init before op #" + std::to_string(i));
        for (uint32_t g = 0; g < o_.groups && err_.empty(); ++g) {
          runStage(sg, static_cast<int>(g));
          for (size_t k = 0; k < s_.outputs.size(); ++k)
            if (s_.outputs[k].poly == Poly::Voice && s_.outputs[k].stage == sg.index)
              readOutput(k, static_cast<int>(g));
        }
      }
      for (size_t k = 0; k < s_.outputs.size() && err_.empty(); ++k)
        if (s_.outputs[k].poly != Poly::Voice) readOutput(k, -1);
    }
    return err_;
  }
  const Coverage& coverage() const { return cov_; }

private:
  struct Tag {
    uint32_t val = UINT32_MAX;
    uint64_t serial = 0;
    std::string* who = nullptr;
  };

  // ---- structure ------------------------------------------------------------------------------------------
  bool fail(std::string m) {
    if (err_.empty()) err_ = std::move(m);
    return false;
  }
  static std::string str(const Operand& o) {
    const char* k = o.kind == Operand::Kind::Buffer ? "b" : o.kind == Operand::Kind::Param ? "p"
                    : o.kind == Operand::Kind::Const ? "c" : o.kind == Operand::Kind::External ? "e" : "none";
    return std::string(k) + std::to_string(o.index);
  }
  bool sameShape(const Operand& a, const Operand& b, const std::string& where) {
    if (a.kind != b.kind) return fail(where + ": operand kinds differ between the shared and unshared schedules");
    if (a.kind != Operand::Kind::Buffer) {
      if (!(a == b)) return fail(where + ": non-buffer operands differ (" + str(a) + " vs " + str(b) + ")");
      return true;
    }
    if (a.index >= s_.buffers.size() || b.index >= u_.buffers.size()) return fail(where + ": buffer out of range");
    const Buffer& pa = s_.buffers[a.index];
    const Buffer& pb = u_.buffers[b.index];
    if (pa.cls != pb.cls || pa.lanes != pb.lanes || pa.perGroup != pb.perGroup)
      return fail(where + ": value v" + std::to_string(b.index) + " is in physical buffer " + str(a) +
                  " of another class/lanes/perGroup");
    if (phys_[b.index] == UINT32_MAX) phys_[b.index] = a.index;
    if (phys_[b.index] != a.index)
      return fail(where + ": value v" + std::to_string(b.index) + " is in two physical buffers (b" +
                  std::to_string(phys_[b.index]) + ", " + str(a) + ")");
    return true;
  }
  bool structure() {
    if (s_.ops.size() != u_.ops.size() || s_.operands.size() != u_.operands.size() ||
        s_.solves.size() != u_.solves.size() || s_.unknowns.size() != u_.unknowns.size() ||
        s_.equations.size() != u_.equations.size() || s_.outputs.size() != u_.outputs.size() ||
        s_.regions.size() != u_.regions.size() || s_.stages.size() != u_.stages.size())
      return fail("the shared and unshared schedules differ in structure");
    phys_.assign(u_.buffers.size(), UINT32_MAX);
    for (size_t i = 0; i < s_.ops.size(); ++i) {
      const Op& a = s_.ops[i];
      const Op& b = u_.ops[i];
      const std::string where = "op #" + std::to_string(i);
      if (a.kind != b.kind || a.kernel != b.kernel || a.numIn != b.numIn || a.numOut != b.numOut ||
          a.firstOperand != b.firstOperand || a.splMask != b.splMask)
        return fail(where + ": differs between the shared and unshared schedules");
      if (a.kind == OpKind::Kernel) {
        const dsp::KernelInfo& ki = dsp::kernelInfo(a.kernel);
        if (ki.variadicIn ? a.numIn < ki.numIn : a.numIn != ki.numIn)
          return fail(where + " (" + ki.name + "): " + std::to_string(a.numIn) + " inputs, the kernel takes " +
                      std::to_string(ki.numIn));
        if (ki.variadicOut ? a.numOut < ki.numOut : a.numOut != ki.numOut)
          return fail(where + " (" + ki.name + "): " + std::to_string(a.numOut) + " outputs, the kernel has " +
                      std::to_string(ki.numOut));
      }
      for (uint32_t k = 0; k < a.numIn + a.numOut; ++k)
        if (!sameShape(s_.operands[a.firstOperand + k], u_.operands[a.firstOperand + k], where)) return false;
      // Aliasing inside one op.
      const bool inPlace = a.kind == OpKind::Kernel && dsp::kernelInfo(a.kernel).inPlaceOk;
      for (uint32_t k = 0; k < a.numOut; ++k) {
        const Operand out = s_.operands[a.firstOperand + a.numIn + k];
        if (out.kind != Operand::Kind::Buffer) return fail(where + ": an output is not a buffer");
        for (uint32_t j = 0; j < a.numIn; ++j) {
          const Operand in = s_.operands[a.firstOperand + j];
          if (in.kind == Operand::Kind::Buffer && in.index == out.index && !(inPlace && k == 0))
            return fail(where + ": output " + std::to_string(k) + " shares b" + std::to_string(out.index) +
                        " with input " + std::to_string(j) + " (only output 0 of an element-wise kernel may)");
        }
        for (uint32_t j = 0; j < k; ++j)
          if (s_.operands[a.firstOperand + a.numIn + j] == out)
            return fail(where + ": two outputs in b" + std::to_string(out.index));
      }
    }
    for (size_t k = 0; k < s_.unknowns.size(); ++k) {
      const std::string where = "unknown " + std::to_string(k);
      if (!sameShape(s_.unknowns[k].value, u_.unknowns[k].value, where) ||
          !sameShape(s_.unknowns[k].init, u_.unknowns[k].init, where))
        return false;
    }
    for (size_t k = 0; k < s_.equations.size(); ++k) {
      const std::string where = "equation " + std::to_string(k);
      if (!sameShape(s_.equations[k].lhs, u_.equations[k].lhs, where) ||
          !sameShape(s_.equations[k].rhs, u_.equations[k].rhs, where))
        return false;
    }
    for (size_t k = 0; k < s_.outputs.size(); ++k)
      if (!sameShape(s_.outputs[k].value, u_.outputs[k].value, "output " + std::to_string(k))) return false;
    for (const OutputRef& o : s_.outputs) {
      if (o.poly != Poly::Voice) continue;
      bool found = false;
      for (const Stage& sg : s_.stages) found |= sg.index == o.stage && sg.poly == Poly::Voice;
      if (!found) return fail("a voice output is read at stage " + std::to_string(o.stage) + ", which does not exist");
    }
    return true;
  }

  // ---- tags -------------------------------------------------------------------------------------------------
  void layout() {
    base_.assign(s_.buffers.size() + 1, 0);
    for (size_t b = 0; b < s_.buffers.size(); ++b) {
      const Buffer& B = s_.buffers[b];
      base_[b + 1] = base_[b] + (B.perGroup ? o_.groups : 1u) * (B.cls == Buffer::Class::Pattern ? 1u : o_.frames);
    }
    tags_.assign(base_.back(), Tag{});
    ctxs_ = o_.groups + 1;
    last_.assign(static_cast<size_t>(u_.buffers.size()) * ctxs_ * o_.frames, 0);
  }
  uint32_t slotOf(uint32_t phys, int ctx, uint32_t f, const std::string& where) {
    const Buffer& B = s_.buffers[phys];
    const uint32_t fs = B.cls == Buffer::Class::Pattern ? 0u : f;
    const uint32_t perCopy = B.cls == Buffer::Class::Pattern ? 1u : o_.frames;
    uint32_t copy = 0;
    if (B.perGroup) {
      if (ctx < 0) fail(where + ": per-group buffer b" + std::to_string(phys) + " used outside a voice group");
      copy = ctx < 0 ? 0u : static_cast<uint32_t>(ctx);
    }
    return base_[phys] + copy * perCopy + fs;
  }
  uint64_t& lastWrite(uint32_t val, int ctx, uint32_t f) {
    const uint32_t fs = u_.buffers[val].cls == Buffer::Class::Pattern ? 0u : f;
    return last_[(static_cast<size_t>(val) * ctxs_ + static_cast<uint32_t>(ctx + 1)) * o_.frames + fs];
  }
  std::string* note(std::string s) {
    notes_.push_back(std::make_unique<std::string>(std::move(s)));
    return notes_.back().get();
  }
  std::string ctxText(int ctx, uint32_t f) const {
    return (ctx < 0 ? std::string("global") : "group " + std::to_string(ctx)) + ", frame " + std::to_string(f) +
           ", block " + std::to_string(block_);
  }

  void write(const Operand& phys, const Operand& val, int ctx, uint32_t f, std::string* who) {
    if (phys.kind != Operand::Kind::Buffer) return;
    const uint32_t slot = slotOf(phys.index, ctx, f, *who);
    ++serial_;
    ++cov_.writes;
    tags_[slot] = Tag{val.index, serial_, who};
    lastWrite(val.index, ctx, f) = serial_;
  }
  void read(const Operand& phys, const Operand& val, int ctx, uint32_t f, const std::string& where) {
    if (phys.kind != Operand::Kind::Buffer || !err_.empty()) return;
    ++cov_.reads;
    uint64_t want = lastWrite(val.index, ctx, f);
    if (ctx >= 0) want = std::max(want, lastWrite(val.index, -1, f));  // a global value read inside a voice group
    if (want < blockStart_) {
      fail(where + " (" + ctxText(ctx, f) + "): reads value v" + std::to_string(val.index) +
           " before anything wrote it in this block");
      return;
    }
    const Tag& t = tags_[slotOf(phys.index, ctx, f, where)];
    if (t.val != val.index || t.serial != want)
      fail(where + " (" + ctxText(ctx, f) + "): reads value v" + std::to_string(val.index) + " from b" +
           std::to_string(phys.index) + ", but b" + std::to_string(phys.index) + " holds " +
           (t.val == UINT32_MAX ? std::string("nothing") : "v" + std::to_string(t.val)) +
           (t.who ? " written by " + *t.who : std::string()) +
           (t.val == val.index ? " (an older write of the same value)" : std::string()));
  }
  void writeAll(uint32_t operandIdx, int ctx, const std::string& who) {
    std::string* w = note(who);
    for (uint32_t f = 0; f < o_.frames; ++f) write(s_.operands[operandIdx], u_.operands[operandIdx], ctx, f, w);
  }

  // ---- execution ------------------------------------------------------------------------------------------
  void runStage(const Stage& sg, int ctx) {
    for (uint32_t r = sg.firstRegion; r < sg.firstRegion + sg.numRegions && err_.empty(); ++r) {
      const Region& rg = s_.regions[r];
      if (rg.kind == RegionKind::Loop) {
        for (uint32_t f = 0; f < o_.frames; ++f) runItems(rg, ctx, f, f + 1);
      } else if (rg.kind == RegionKind::Block) {
        runItems(rg, ctx, 0, o_.frames);
      } else {
        runItems(rg, ctx, 0, 1);  // Control: once per block, on patterns
      }
    }
  }
  void runItems(const Region& rg, int ctx, uint32_t f0, uint32_t f1) {
    const uint32_t end = rg.firstOp + rg.numOps;
    uint32_t k = rg.firstSolve;
    const uint32_t kEnd = rg.firstSolve + rg.numSolves;
    uint32_t i = rg.firstOp;
    while (err_.empty()) {
      if (k < kEnd && s_.solves[k].firstOp == i) {
        for (uint32_t f = f0; f < f1; ++f) runSolve(k, ctx, f);
        i += s_.solves[k].numOps;
        ++k;
        continue;
      }
      if (i >= end) break;
      runOp(i, ctx, f0, f1);
      ++i;
    }
    if (k != kEnd) fail("solve block " + std::to_string(k) + " is not at an op boundary of its region");
  }
  void runOp(uint32_t i, int ctx, uint32_t f0, uint32_t f1) {
    const Op& op = s_.ops[i];
    const std::string where = "op #" + std::to_string(i);
    const uint32_t ins = op.firstOperand, outs = op.firstOperand + op.numIn;
    const int wctx = op.kind == OpKind::Reduce ? -1 : ctx;  // the accumulator is global
    for (uint32_t f = f0; f < f1; ++f)
      for (uint32_t j = 0; j < op.numIn; ++j)
        read(s_.operands[ins + j], u_.operands[ins + j], ctx, f, where + " input " + std::to_string(j));
    if (op.kind == OpKind::Reduce)
      for (uint32_t f = f0; f < f1; ++f)
        for (uint32_t j = 0; j < op.numOut; ++j)
          read(s_.operands[outs + j], u_.operands[outs + j], -1, f, where + " accumulator");
    std::string* who = note(where);
    for (uint32_t f = f0; f < f1; ++f)
      for (uint32_t j = 0; j < op.numOut; ++j) write(s_.operands[outs + j], u_.operands[outs + j], wctx, f, who);
  }
  void body(const SolveBlock& b, int ctx, uint32_t f) {
    ++cov_.bodyEvaluations;
    for (uint32_t i = b.firstOp; i < b.firstOp + b.numOps && err_.empty(); ++i) runOp(i, ctx, f, f + 1);
  }
  void solverReads(uint32_t k, const SolveBlock& b, int ctx, uint32_t f, const char* when) {
    const std::string where = std::string("solve ") + std::to_string(k) + " " + when;
    for (uint32_t e = b.firstEquation; e < b.firstEquation + b.numEquations; ++e) {
      read(s_.equations[e].lhs, u_.equations[e].lhs, ctx, f, where + ": lhs of equation " + std::to_string(e));
      read(s_.equations[e].rhs, u_.equations[e].rhs, ctx, f, where + ": rhs of equation " + std::to_string(e));
      cov_.solveReads += 2;
    }
    for (uint32_t q = b.firstUnknown; q < b.firstUnknown + b.numUnknowns; ++q) {
      read(s_.unknowns[q].value, u_.unknowns[q].value, ctx, f, where + ": unknown " + std::to_string(q));
      ++cov_.solveReads;
    }
  }
  void writeUnknown(uint32_t q, int ctx, uint32_t f, std::string* who) {
    write(s_.unknowns[q].value, u_.unknowns[q].value, ctx, f, who);
  }
  void runSolve(uint32_t k, int ctx, uint32_t f) {
    const SolveBlock& b = s_.solves[k];
    std::string* who = note("the solver (solve " + std::to_string(k) + ")");
    for (uint32_t q = b.firstUnknown; q < b.firstUnknown + b.numUnknowns; ++q)
      read(s_.unknowns[q].init, u_.unknowns[q].init, ctx, f, "solve " + std::to_string(k) + " initial guess");
    for (uint32_t q = b.firstUnknown; q < b.firstUnknown + b.numUnknowns; ++q) writeUnknown(q, ctx, f, who);
    const uint32_t iters = std::min(b.iterations, o_.newton);
    for (uint32_t it = 0; it < iters && err_.empty(); ++it) {
      body(b, ctx, f);
      solverReads(k, b, ctx, f, "after a body evaluation");
      for (uint32_t q = b.firstUnknown; q < b.firstUnknown + b.numUnknowns && err_.empty(); ++q) {
        for (int side = 0; side < 2; ++side) {
          writeUnknown(q, ctx, f, who);  // u_q ± h
          body(b, ctx, f);
          solverReads(k, b, ctx, f, "after a Jacobian evaluation");
        }
        writeUnknown(q, ctx, f, who);  // restore
      }
      for (uint32_t q = b.firstUnknown; q < b.firstUnknown + b.numUnknowns; ++q) writeUnknown(q, ctx, f, who);
    }
    body(b, ctx, f);  // the final evaluation: later ops read these values
  }
  void readOutput(size_t k, int ctx) {
    const OutputRef& o = s_.outputs[k];
    for (uint32_t f = 0; f < o_.frames; ++f) {
      ++cov_.outputReads;
      read(o.value, u_.outputs[k].value, ctx, f,
           std::string("the engine reading output ") + std::to_string(k) +
               (o.poly == Poly::Voice ? " at the end of stage " + std::to_string(o.stage) : std::string(" after the last stage")));
    }
  }

  const Schedule& s_;
  const Schedule& u_;
  Options o_;
  std::string err_;
  std::vector<uint32_t> phys_;  // value → physical buffer
  std::vector<uint32_t> base_;
  std::vector<Tag> tags_;
  std::vector<uint64_t> last_;
  uint32_t ctxs_ = 0;
  uint64_t serial_ = 0, blockStart_ = 0;
  uint32_t block_ = 0;
  std::vector<std::unique_ptr<std::string>> notes_;
  Coverage cov_;
};

// Schedules `tp` with and without buffer sharing (else identical options) and replays. "" = no aliasing.
inline std::string check(const TypedPatch& tp, ScheduleOptions so, Options o = {}, Coverage* cov = nullptr) {
  so.shareBuffers = true;
  const ScheduleResult shared = scheduleDetailed(tp, so);
  so.shareBuffers = false;
  const ScheduleResult unique = scheduleDetailed(tp, so);
  if (!shared.ok() || !unique.ok()) return "the patch does not schedule";
  Replay r(shared.schedule, unique.schedule, o);
  std::string e = r.run();
  if (cov) {
    cov->reads += r.coverage().reads;
    cov->writes += r.coverage().writes;
    cov->bodyEvaluations += r.coverage().bodyEvaluations;
    cov->outputReads += r.coverage().outputReads;
    cov->solveReads += r.coverage().solveReads;
  }
  return e;
}

}  // namespace sched_replay
