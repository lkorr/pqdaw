// The interpreter backend (T1.7): pq/engine/interpreter.h says what it does; pq/graph/schedule.h says how a
// Schedule must be run. This file is the builder (Schedule → flat records with resolved pointers) and the image's
// process() (the only RT code here). Everything around the records — validation, memory, params, engine sources,
// reducers / sends / broadcast, the stage loop and output copies — is the shared image host (pq/engine/image_host.h,
// T1.x-arch2), the same code the JIT's images run.
//
// Layout of what the builder produces:
//   recs_     every op of every region as {KernelFn, KernelArgs}, in schedule order; voice stages materialised once
//             per voice group (pointers to that group's per-group buffers, state and engine data)
//   pool_     the KernelArgs::in/out pointer arrays of all records
//   relocs_   per record in a Loop region (or a solve body): the Blk operands, re-pointed at base + t·lanes before
//             each sub-block / frame
//   sites_    the pointer cells that read engine data (root inputs, voice.*, track/sidechain input), rewritten once
//             per block from the host's engine-source table
//   regions_  per (stage, group) region: the record range (and, with solve blocks, an item list)
//   swaps_    TX.smooth: per record with ramped inputs (pq/graph/schedule.h "Param ramps"), the pool cells that read
//             the ramp's buffer instead of the param-rate pattern in a block where one of the record's ramps moves
//   branches_ TX.uniform-branch: per (region, group) branch of the schedule (pq/graph/schedule.h "Uniform branches"),
//             its select record and the select kernels for every Spl mask; records carry their guard (an index into
//             guardOn_, 2 per branch), evaluated once per block at the region's start

#include "pq/engine/interpreter.h"

#include <intrin.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <cmath>
#include <string>
#include <unordered_map>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atoms.h"
#include "pq/engine/image_host.h"
#include "pq/engine/newton.h"

namespace pq::engine {

namespace {

namespace g = pq::graph;

constexpr uint32_t kSolveItem = 0x80000000u;

// ---- image ------------------------------------------------------------------------------------------------------
struct Rec {
  dsp::KernelFn fn = nullptr;
  dsp::KernelArgs args;
  uint32_t inBase = 0, outBase = 0;      // into pool_
  uint32_t firstReloc = 0, numReloc = 0;
  // TX.smooth: the ramped inputs (swaps_), and the kernel / Spl mask as scheduled and with them as blocks.
  uint32_t firstSwap = 0, numSwaps = 0;
  dsp::KernelFn fnBase = nullptr, fnRamp = nullptr;
  uint32_t maskBase = 0, maskRamp = 0;
  bool rampMode = false;                 // this block reads the ramp buffers
  // TX.uniform-branch: the guard this record runs under (guardOn_ index) and, for a branch's select, its branch.
  uint32_t guard = g::kNone;
  uint32_t branch = g::kNone;
};

// TX.uniform-branch: one branch of one region (and voice group).
struct BranchRun {
  uint32_t rec = 0;                      // its select record
  uint32_t parent = g::kNone;            // the guardOn_ index its select runs under
  uint8_t state = 0;                     // this block: 0 mixed (select), 1 all true (input 1), 2 none true (input 2)
  dsp::KernelFn select[8] = {};          // the select kernel per Spl mask (its lanes)
};

// TX.smooth: one ramped input of a record (the param_ramp state says whether it moves this block).
struct Swap {
  uint32_t cell = 0;                     // pool_ index of the input
  float* ramp = nullptr;                 // the ramp's buffer (this group's)
  float* saved = nullptr;                // the cell's pattern pointer while it reads the ramp
  const dsp::ParamRampState* state = nullptr;
  uint32_t stride = 1;                   // floats per frame (the op's lanes)
  bool checked = false;                  // TX.smooth-fix1: its source is not static (it may move in ramp-free blocks)
};

struct Reloc {
  uint32_t cell;   // pool_ index
  uint32_t stride; // floats per frame of the operand (its lanes)
  float* base;
};

struct RegionRun {
  g::RegionKind kind = g::RegionKind::Block;
  uint32_t schedRegion = 0;  // its Schedule::regions index (the sleep plan's, T1.x-silence)
  // T3.17-fix2: a global Control region that computes only from params and constants (no engine sources, no solve
  // blocks) into buffers nothing else writes: its results stay valid while the params do, so it is skipped in a
  // block whose params equal the previous block's (a settled mixer's gain math costs nothing).
  bool memo = false;
  uint32_t subBlock = 0;
  uint32_t factor = 1;  // TMM.6: an oversample region runs its ops on frames · factor samples
  uint32_t firstRec = 0, numRec = 0;
  uint32_t firstItem = 0, numItems = 0;  // with solve blocks: rec index, or kSolveItem | solve index
  uint32_t firstRampRec = 0, numRampRec = 0;  // TX.smooth: its records with ramped inputs (into rampRecs_)
  uint32_t firstBranch = 0, numBranches = 0;  // TX.uniform-branch: its branches (into branches_; Block regions)
  // TX.smooth-fix1: a region of param_ramp ops (its records whose ramp is not static: rampChecked_ [firstChecked,
  // + numChecked)), whether some ramped input of the region is not static, and whether some record of it read its
  // ramps in the last block it ran (ramp-free blocks then put the patterns back once).
  bool rampOps = false;
  bool anyChecked = false;
  mutable bool rampDirty = false;
  uint32_t firstChecked = 0, numChecked = 0;
};

struct OperandRef {  // element (frame t, lane l) = base[t·stride + l]; stride 0 = a pattern
  float* base = nullptr;
  uint32_t stride = 0;
  float& at(uint32_t t, uint32_t l) const noexcept { return base[t * stride + l]; }
};

struct UnkRun {
  OperandRef value;
  OperandRef init;        // base null: start from `guess`
  float* guess = nullptr; // previous solution (state), 8 floats
};

struct EqRun {
  OperandRef lhs, rhs;
};

// Forward-mode tangents of a solve body (pq/engine/newton.h): per body op, where its inputs' and outputs' tangents
// live (a slot of the tangent scratch, one per distinct buffer of the block) and which directions are non-zero.
struct AdOp {
  newton::TangentStepFn step = nullptr;
  uint32_t numIn = 0, numOut = 0;
  uint32_t firstIn = 0, firstOut = 0;  // into adPorts_
  uint32_t outAny = 0;                 // OR of the outputs' direction bits
};
using AdPort = newton::TanPort;  // a tangent slot and its direction bits (0: no tangent, slot unused)

struct SolveRun {
  uint32_t iterations = 1;
  uint32_t lanes = 1;
  uint32_t n = 0;
  uint32_t firstBodyRec = 0, numBodyRec = 0;
  uint32_t firstUnk = 0, firstEq = 0;
  bool loop = false;  // body records are relocated to frame t
  bool ad = false;    // Jacobian by forward-mode tangents (else finite differences)
  bool finalEval = true;  // run the body once more after the last iteration (newton::bodyReadAfter)
  uint32_t firstAdOp = 0;   // numBodyRec AdOps
  uint32_t firstAdEq = 0;   // per equation: lhs, rhs AdPorts (into adPorts_)
  uint32_t firstAdUnk = 0;  // per unknown: its slot (into adPorts_)
};

struct StageRegions {  // per schedule stage; voice: group g's regions at firstRegion + g · numRegions
  uint32_t firstRegion = 0, numRegions = 0;
};

struct Site {
  float** target = nullptr;
  uint32_t ext = 0;
  uint32_t group = 0;
};

class InterpretedImage final : public ProgramImage, public RtHeapObject {  // PF.4b: on the RT heap
public:
  void process(ProcessContext& ctx) noexcept override;  // RT-safe
  const g::StateLayout& stateLayout() const noexcept override { return host_.stateLayout(); }
  const ImageInterface& io() const noexcept override { return host_.io(); }
  StateArena stateArena() noexcept override { return host_.stateArena(); }
  uint64_t rejectedBlocks() const noexcept override { return host_.rejectedBlocks(); }
  const SleepStats* sleepStats() const noexcept override { return &host_.sleepStats(); }
  bool bindInspect(const InspectBinding& b) override { return host_.bindInspect(b); }
  const CostMeter* boundMeter() const noexcept override { return host_.boundMeter(); }

  // ---- built by Builder ----
  host::ImageHost host_;
  AlignedBuffer<float> solveMem_, tanMem_;
  std::vector<float*> pool_;
  std::vector<Rec> recs_;
  std::vector<Reloc> relocs_;
  std::vector<Swap> swaps_;          // TX.smooth
  std::vector<uint32_t> rampRecs_;   // TX.smooth: record indices, per region
  std::vector<uint32_t> rampChecked_;  // TX.smooth-fix1: the param_ramp records of non-static ramps, per region
  std::vector<BranchRun> branches_;  // TX.uniform-branch
  std::vector<uint8_t> guardOn_;     // TX.uniform-branch: per guard (2 per branch), whether it runs this block
  bool rampMode_ = false;          // TX.smooth-fix1: the stage being run is in its ramp mode (ImageHost::run)
  std::vector<uint32_t> items_;
  std::vector<RegionRun> regions_;
  std::vector<StageRegions> stages_;
  std::vector<SolveRun> solves_;
  std::vector<UnkRun> unks_;
  std::vector<EqRun> eqs_;
  std::vector<AdOp> adOps_;
  std::vector<AdPort> adPorts_;
  std::vector<newton::Tan<float>> adIn_, adOut_;  // scratch: one op's input / output tangents (largest arity)
  std::vector<Site> sites_;          // global sites first, then voice sites sorted by group
  uint32_t numGlobalSites_ = 0;
  std::vector<uint32_t> voiceSiteBegin_;  // per group: first site (size maxGroups + 1)
  uint32_t maxSolveN_ = 0;
  bool shadow_ = false;            // SleepMode::Shadow (T1.x-silence)
  bool paramsSame_ = false;        // this block's params equal the previous block's

private:
  void runRegion(const RegionRun& r, uint32_t frames) noexcept;
  void prepareRamps(const RegionRun& r, uint32_t frames) noexcept;
  // In a Loop region, a record in ramp mode reads its ramps from frame t (after relocate()).
  void relocateRamps(const Rec& rec, uint32_t t) noexcept {  // RT-safe
    if (!rec.rampMode) return;
    for (uint32_t k = rec.firstSwap; k < rec.firstSwap + rec.numSwaps; ++k)
      pool_[swaps_[k].cell] = swaps_[k].ramp + static_cast<size_t>(t) * swaps_[k].stride;
  }
  void runMetered(ProcessContext& ctx) noexcept;
  void runBranched(const RegionRun& r, uint32_t frames) noexcept;
  void runBranchSelect(Rec& rec, const BranchRun& b) noexcept;
  void relocate(const Rec& rec, uint32_t t) noexcept {  // RT-safe
    for (uint32_t k = rec.firstReloc; k < rec.firstReloc + rec.numReloc; ++k) {
      const Reloc& rl = relocs_[k];
      pool_[rl.cell] = rl.base + static_cast<size_t>(t) * rl.stride;
    }
  }
  void runSolve(const SolveRun& s, uint32_t t) noexcept;
  void evalBody(const SolveRun& s, uint32_t t) noexcept {  // RT-safe
    for (uint32_t i = s.firstBodyRec; i < s.firstBodyRec + s.numBodyRec; ++i) {
      Rec& rec = recs_[i];
      if (s.loop) {
        relocate(rec, t);
        rec.args.frames = 1;
      }
      rec.fn(rec.args);
    }
  }
  void evalBodyTangents(const SolveRun& s, uint32_t t) noexcept;
  void finiteDifferenceJacobian(const SolveRun& s, uint32_t t, const float* r0, float* J) noexcept;
};

// TX.smooth: once per block before a region's records run. A record whose ramps all rest runs as scheduled (Spl
// inputs); one with a moving ramp reads every ramp of it as a block (a resting one's buffer filled with its value).
// TX.smooth-fix1: in a ramp-free block only the non-static ramps ran (their `active` is this block's); a static one
// rests by construction (its last `active` is stale and ignored).
void InterpretedImage::prepareRamps(const RegionRun& r, uint32_t frames) noexcept {  // RT-safe
  bool dirty = false;
  for (uint32_t q = r.firstRampRec; q < r.firstRampRec + r.numRampRec; ++q) {
    Rec& rec = recs_[rampRecs_[q]];
    bool any = false;
    for (uint32_t k = rec.firstSwap; k < rec.firstSwap + rec.numSwaps; ++k)
      any = any || ((rampMode_ || swaps_[k].checked) && swaps_[k].state->active != 0);
    dirty = dirty || any;
    if (any) {
      for (uint32_t k = rec.firstSwap; k < rec.firstSwap + rec.numSwaps; ++k) {
        Swap& sw = swaps_[k];
        if (!((rampMode_ || sw.checked) && sw.state->active != 0)) dsp::paramRampHold(sw.state, sw.ramp, frames, sw.stride);
        if (pool_[sw.cell] != sw.ramp) {
          sw.saved = pool_[sw.cell];  // the pattern (an engine source's is rewritten every block)
          pool_[sw.cell] = sw.ramp;
        }
      }
      rec.fn = rec.fnRamp;
      rec.args.splMask = rec.maskRamp;
      rec.rampMode = true;
    } else if (rec.rampMode) {
      for (uint32_t k = rec.firstSwap; k < rec.firstSwap + rec.numSwaps; ++k) {
        Swap& sw = swaps_[k];
        if (pool_[sw.cell] == sw.ramp) pool_[sw.cell] = sw.saved;
      }
      rec.fn = rec.fnBase;
      rec.args.splMask = rec.maskBase;
      rec.rampMode = false;
    }
  }
  r.rampDirty = dirty;
}

void InterpretedImage::runRegion(const RegionRun& r, uint32_t frames) noexcept {  // RT-safe
  frames *= r.factor;  // TMM.6 (Control regions have factor 1)
  // TX.smooth-fix1: in a ramp-free block a region whose ramps are all static runs exactly as scheduled (its records
  // were put back to their patterns by the first such block), and a param_ramp region runs only its non-static ramps.
  if (r.numRampRec != 0 && (rampMode_ || r.anyChecked || r.rampDirty)) prepareRamps(r, frames);
  if (r.rampOps && !rampMode_) {
    for (uint32_t q = r.firstChecked; q < r.firstChecked + r.numChecked; ++q) {
      Rec& rec = recs_[rampChecked_[q]];
      rec.args.frames = frames;
      rec.fn(rec.args);
    }
    return;
  }
  switch (r.kind) {
    case g::RegionKind::Control:
      if (r.memo && paramsSame_) return;  // its buffers still hold this block's values
      if (r.numItems == 0) {
        for (uint32_t i = r.firstRec; i < r.firstRec + r.numRec; ++i) recs_[i].fn(recs_[i].args);
      } else {
        for (uint32_t k = r.firstItem; k < r.firstItem + r.numItems; ++k) {
          const uint32_t it = items_[k];
          if (it & kSolveItem) runSolve(solves_[it & ~kSolveItem], 0);
          else recs_[it].fn(recs_[it].args);
        }
      }
      return;
    case g::RegionKind::Block:
      if (r.numBranches != 0) {
        runBranched(r, frames);
        return;
      }
      for (uint32_t i = r.firstRec; i < r.firstRec + r.numRec; ++i) {
        Rec& rec = recs_[i];
        rec.args.frames = frames;
        rec.fn(rec.args);
      }
      return;
    case g::RegionKind::Loop: {
      const uint32_t sub = r.subBlock ? r.subBlock : 1u;
      for (uint32_t t = 0; t < frames; t += sub) {
        const uint32_t n = std::min(sub, frames - t);
        if (r.numItems == 0) {
          for (uint32_t i = r.firstRec; i < r.firstRec + r.numRec; ++i) {
            Rec& rec = recs_[i];
            relocate(rec, t);
            relocateRamps(rec, t);
            rec.args.frames = n;
            rec.fn(rec.args);
          }
        } else {
          for (uint32_t k = r.firstItem; k < r.firstItem + r.numItems; ++k) {
            const uint32_t it = items_[k];
            if (it & kSolveItem) {
              runSolve(solves_[it & ~kSolveItem], t);  // regions with solves run per sample (n = 1)
              continue;
            }
            Rec& rec = recs_[it];
            relocate(rec, t);
            relocateRamps(rec, t);
            rec.args.frames = n;
            rec.fn(rec.args);
          }
        }
      }
      // Ramp-mode cells back at frame 0 (prepareRamps compares them with the ramp's buffer).
      for (uint32_t q = r.firstRampRec; q < r.firstRampRec + r.numRampRec; ++q) relocateRamps(recs_[rampRecs_[q]], 0);
      return;
    }
  }
}

// TX.uniform-branch (pq/graph/schedule.h "Uniform branches"): a Block region with branches. Each branch's state from
// the sign bits of its condition's 8 lanes (as the select kernel reads them), outer branches first; then every record
// whose guard runs, a branch's select as the side it chose.
void InterpretedImage::runBranched(const RegionRun& r, uint32_t frames) noexcept {  // RT-safe
  for (uint32_t k = r.firstBranch; k < r.firstBranch + r.numBranches; ++k) {
    BranchRun& b = branches_[k];
    const int mask = _mm256_movemask_ps(_mm256_loadu_ps(pool_[recs_[b.rec].inBase]));
    const bool on = b.parent == g::kNone || guardOn_[b.parent] != 0;
    b.state = mask == 0xFF ? uint8_t{1} : mask == 0 ? uint8_t{2} : uint8_t{0};
    guardOn_[2 * k] = on && mask != 0 ? 1 : 0;
    guardOn_[2 * k + 1] = on && mask != 0xFF ? 1 : 0;
  }
  for (uint32_t i = r.firstRec; i < r.firstRec + r.numRec; ++i) {
    Rec& rec = recs_[i];
    if (rec.guard != g::kNone && guardOn_[rec.guard] == 0) continue;
    rec.args.frames = frames;
    if (rec.branch != g::kNone && branches_[rec.branch].state != 0) runBranchSelect(rec, branches_[rec.branch]);
    else rec.fn(rec.args);
  }
}

// The select of a branch that took one side: select(cond, x, x) with x the chosen input (bit for bit its value; the
// other input, not computed this block, is not read). The other input's cell and the Spl mask are restored after.
void InterpretedImage::runBranchSelect(Rec& rec, const BranchRun& b) noexcept {  // RT-safe
  const uint32_t keep = b.state == 1 ? 1u : 2u, drop = 3u - keep;
  float*& cell = pool_[rec.inBase + drop];
  float* const saved = cell;
  const uint32_t mask = rec.args.splMask;
  const uint32_t m = (mask & ~(1u << drop)) | (((mask >> keep) & 1u) << drop);
  cell = pool_[rec.inBase + keep];
  rec.args.splMask = m;
  b.select[m & 7u](rec.args);
  rec.args.splMask = mask;
  cell = saved;
}

// Newton per lane (pq/engine/newton.h has the scheme; the JIT generates the same arithmetic). Scratch: solveMem_
// holds u, r0, rp, rm (n·8 each), up, um (8 each), J and Jfd (n·n·8 each) and the elimination matrix (n·(n+1)
// doubles); tanMem_ the tangents (slots · n · 8).
void InterpretedImage::evalBodyTangents(const SolveRun& s, uint32_t t) noexcept {  // RT-safe
  const uint32_t n = s.n, L = s.lanes;
  float* T = tanMem_.data();
  for (uint32_t j = 0; j < n; ++j) {
    const AdPort& u = adPorts_[s.firstAdUnk + j];
    for (uint32_t l = 0; l < L; ++l) T[(u.slot * n + j) * 8 + l] = 1.0f;
  }
  alignas(32) float snap[3 * 8] = {};
  for (uint32_t k = 0; k < s.numBodyRec; ++k) {
    Rec& rec = recs_[s.firstBodyRec + k];
    if (s.loop) {
      relocate(rec, t);
      rec.args.frames = 1;
    }
    const AdOp& op = adOps_[s.firstAdOp + k];
    if (op.outAny == 0) {
      rec.fn(rec.args);
      continue;
    }
    // The rules read input values; an in-place op overwrites one, so take them before the call.
    const uint32_t na = std::min<uint32_t>(op.numIn, 3);
    for (uint32_t i = 0; i < na; ++i)
      for (uint32_t l = 0; l < L; ++l) snap[i * 8 + l] = rec.args.in[i][l];
    rec.fn(rec.args);
    newton::TangentStep g;
    g.numIn = op.numIn;
    g.numOut = op.numOut;
    g.n = n;
    g.lanes = L;
    g.outAny = op.outAny;
    g.a = snap;
    g.y = rec.args.out[0];
    g.in = adPorts_.data() + op.firstIn;
    g.out = adPorts_.data() + op.firstOut;
    g.T = T;
    g.dScratch = adIn_.data();
    g.dyScratch = adOut_.data();
    op.step(g);
  }
}

// J by finite differences around the current u (the body's values are those at u and r0 its residuals).
void InterpretedImage::finiteDifferenceJacobian(const SolveRun& s, uint32_t t, const float* r0, float* J) noexcept {  // RT-safe
  const uint32_t n = s.n, L = s.lanes, N8 = maxSolveN_ * 8;
  const float* u = solveMem_.data();
  float* rp = solveMem_.data() + 2 * N8;
  float* rm = rp + N8;
  float* up = rm + N8;
  float* um = up + 8;
  const UnkRun* unk = unks_.data() + s.firstUnk;
  const EqRun* eq = eqs_.data() + s.firstEq;
  auto residuals = [&](float* r) noexcept {
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t l = 0; l < L; ++l) r[i * 8 + l] = eq[i].lhs.at(t, l) - eq[i].rhs.at(t, l);
  };
  for (uint32_t j = 0; j < n; ++j) {
    for (uint32_t l = 0; l < L; ++l) {
      const float saved = u[j * 8 + l];
      const float step = newton::kStep * std::max(std::fabs(saved), newton::kStepFloor);
      const float h = std::isfinite(step) ? step : 0.0f;
      up[l] = saved + h;
      um[l] = saved - h;
    }
    for (uint32_t l = 0; l < L; ++l) unk[j].value.at(t, l) = up[l];
    evalBody(s, t);
    residuals(rp);
    for (uint32_t l = 0; l < L; ++l) unk[j].value.at(t, l) = um[l];
    evalBody(s, t);
    residuals(rm);
    for (uint32_t l = 0; l < L; ++l) unk[j].value.at(t, l) = u[j * 8 + l];
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t l = 0; l < L; ++l) {
        const float p = rp[i * 8 + l], m = rm[i * 8 + l], saved = u[j * 8 + l];
        // Central difference over the step actually taken in float; one-sided where a side is not finite.
        J[(i * n + j) * 8 + l] = std::isfinite(p) && std::isfinite(m) ? (p - m) / (up[l] - um[l])
                                 : std::isfinite(p)                    ? (p - r0[i * 8 + l]) / (up[l] - saved)
                                                                       : (r0[i * 8 + l] - m) / (saved - um[l]);
      }
  }
}

void InterpretedImage::runSolve(const SolveRun& s, uint32_t t) noexcept {  // RT-safe
  const uint32_t n = s.n, L = s.lanes, N8 = maxSolveN_ * 8;
  float* u = solveMem_.data();
  float* r0 = u + N8;
  float* J = u + 4 * N8 + 16;
  float* Jfd = J + static_cast<size_t>(maxSolveN_) * N8;
  double* M = reinterpret_cast<double*>(Jfd + static_cast<size_t>(maxSolveN_) * N8);
  const float* T = tanMem_.data();
  const UnkRun* unk = unks_.data() + s.firstUnk;
  const EqRun* eq = eqs_.data() + s.firstEq;
  auto writeU = [&]() noexcept {
    for (uint32_t j = 0; j < n; ++j)
      for (uint32_t l = 0; l < L; ++l) unk[j].value.at(t, l) = u[j * 8 + l];
  };
  auto residuals = [&](float* r) noexcept {
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t l = 0; l < L; ++l) r[i * 8 + l] = eq[i].lhs.at(t, l) - eq[i].rhs.at(t, l);
  };
  for (uint32_t j = 0; j < n; ++j)
    for (uint32_t l = 0; l < L; ++l) u[j * 8 + l] = unk[j].init.base ? unk[j].init.at(t, l) : unk[j].guess[l];
  writeU();
  for (uint32_t it = 0; it < s.iterations; ++it) {
    if (s.ad) {
      evalBodyTangents(s, t);
      residuals(r0);
      uint32_t bad = 0;  // lanes whose tangent Jacobian is not finite
      for (uint32_t i = 0; i < n; ++i) {
        const AdPort& lp = adPorts_[s.firstAdEq + 2 * i];
        const AdPort& rp = adPorts_[s.firstAdEq + 2 * i + 1];
        for (uint32_t j = 0; j < n; ++j) {
          const bool lo = ((lp.nz >> j) & 1u) != 0, ro = ((rp.nz >> j) & 1u) != 0;
          for (uint32_t l = 0; l < L; ++l) {
            const float tl = lo ? T[(lp.slot * n + j) * 8 + l] : 0.0f;
            const float tr = ro ? T[(rp.slot * n + j) * 8 + l] : 0.0f;
            const float x = lo && ro ? tl - tr : lo ? tl : ro ? -tr : 0.0f;
            J[(i * n + j) * 8 + l] = x;
            if (!std::isfinite(x)) bad |= 1u << l;
          }
        }
      }
      if (bad != 0) {
        finiteDifferenceJacobian(s, t, r0, Jfd);
        for (uint32_t q = 0; q < n * n; ++q)
          for (uint32_t l = 0; l < L; ++l)
            if ((bad >> l) & 1u) J[q * 8 + l] = Jfd[q * 8 + l];
      }
    } else {
      evalBody(s, t);
      residuals(r0);
      finiteDifferenceJacobian(s, t, r0, J);
    }
    // J·Δ = r0 per lane, u −= Δ when finite: n = 1 a double division, n > 1 newton::linearUpdate (shared with the JIT).
    if (n == 1) {
      for (uint32_t l = 0; l < L; ++l) {
        const double d = static_cast<double>(r0[l]) / static_cast<double>(J[l]);
        const float nu = static_cast<float>(static_cast<double>(u[l]) - d);
        if (std::isfinite(d) && std::isfinite(nu)) u[l] = nu;
      }
    } else {
      newton::linearUpdate(u, J, r0, M, n, L);
    }
    writeU();
  }
  if (s.finalEval) evalBody(s, t);
  for (uint32_t j = 0; j < n; ++j)
    if (unk[j].guess)
      for (uint32_t l = 0; l < L; ++l) unk[j].guess[l] = u[j * 8 + l];
}

void InterpretedImage::process(ProcessContext& ctx) noexcept {  // RT-safe
  if (!host_.begin(ctx)) return;
  // The memo regions' inputs are the params: unchanged (clamped, bit for bit) since the previous block, as the host's
  // param pass found (T1b.x-perf: no second pass; the JIT reads the same flag).
  paramsSame_ = !host_.paramsChanged();
  // Engine data → the records' pointer cells.
  for (uint32_t i = 0; i < numGlobalSites_; ++i) *sites_[i].target = host_.extPtr(sites_[i].ext, 0);
  for (uint32_t i = voiceSiteBegin_[0]; i < voiceSiteBegin_[host_.groups()]; ++i)
    *sites_[i].target = host_.extPtr(sites_[i].ext, sites_[i].group);
  const uint32_t frames = ctx.frames;
  if (host_.metering()) {  // T4.5: a cost meter is shown: the same stages, each region timed
    runMetered(ctx);
    return;
  }
  host_.run(
      ctx,
      [&](uint32_t k, bool ramp) noexcept {
        rampMode_ = ramp;
        const StageRegions& st = stages_[k];
        for (uint32_t r = st.firstRegion; r < st.firstRegion + st.numRegions; ++r) {
          const RegionRun& rr = regions_[r];
          if (host_.regionAsleep(rr.schedRegion)) {  // T1.x-silence: the constant-output kernel instead
            host_.fillRegion(rr.schedRegion, frames);
            continue;
          }
          runRegion(rr, frames);
          if (shadow_ && host_.regionSegmentAsleep(rr.schedRegion) && !host_.regionFillsSilent(rr.schedRegion, frames))
            host_.countViolation();
        }
      },
      [&](uint32_t k, uint32_t gi, bool ramp) noexcept {
        rampMode_ = ramp;
        const StageRegions& st = stages_[k];
        const uint32_t base = st.firstRegion + gi * st.numRegions;
        for (uint32_t r = base; r < base + st.numRegions; ++r) runRegion(regions_[r], frames);
      });
}

// T4.5: process() while a cost meter is shown — the TSC read around every region (pq/engine/inspect.h). A separate
// path, so the unmetered one stays the code it was.
void InterpretedImage::runMetered(ProcessContext& ctx) noexcept {  // RT-safe
  const uint32_t frames = ctx.frames;
  host_.run(
      ctx,
      [&](uint32_t k, bool ramp) noexcept {
        rampMode_ = ramp;
        const StageRegions& st = stages_[k];
        for (uint32_t r = st.firstRegion; r < st.firstRegion + st.numRegions; ++r) {
          const RegionRun& rr = regions_[r];
          const uint64_t t0 = __rdtsc();
          if (host_.regionAsleep(rr.schedRegion)) {
            host_.fillRegion(rr.schedRegion, frames);
          } else {
            runRegion(rr, frames);
            if (shadow_ && host_.regionSegmentAsleep(rr.schedRegion) && !host_.regionFillsSilent(rr.schedRegion, frames))
              host_.countViolation();
          }
          host_.addRegionCycles(rr.schedRegion, __rdtsc() - t0);
        }
      },
      [&](uint32_t k, uint32_t gi, bool ramp) noexcept {
        rampMode_ = ramp;
        const StageRegions& st = stages_[k];
        const uint32_t base = st.firstRegion + gi * st.numRegions;
        for (uint32_t r = base; r < base + st.numRegions; ++r) {
          const uint64_t t0 = __rdtsc();
          runRegion(regions_[r], frames);
          host_.addRegionCycles(regions_[r].schedRegion, __rdtsc() - t0);
        }
      });
}

// ---- builder ----------------------------------------------------------------------------------------------------
class Builder {
public:
  Builder(const g::Schedule& s, const InterpretOptions& o) : s_(s), o_(o) {}

  Result<std::unique_ptr<ProgramImage>> build() {
    img_ = std::make_unique<InterpretedImage>();
    host::BuildOptions bo;
    bo.who = "interpreter";
    bo.backend = ImageBackend::Interpreter;
    bo.maxBlockFrames = o_.maxBlockFrames;
    bo.maxVoiceGroups = o_.maxVoiceGroups;
    bo.resources = o_.resources;
    bo.sleep = o_.sleep;
    img_->shadow_ = o_.sleep == SleepMode::Shadow;
    if (Status st = img_->host_.build(s_, bo); !st.ok()) return st.error();
    G_ = o_.maxVoiceGroups;
    if (!stages()) return err_;
    finish();
    return std::unique_ptr<ProgramImage>(std::move(img_));
  }

private:
  bool fail(std::string msg) {
    err_ = img_->host_.error(msg);
    return false;
  }
  host::ImageHost& host() noexcept { return img_->host_; }
  bool isPattern(const g::Operand& o) const { return host::isPatternOperand(s_, o); }
  uint32_t lanesOf(const g::Operand& o) const { return host::lanesOfOperand(s_, o); }

  enum class SiteKind : uint8_t { Pool, Reloc, UnkInit, EqLhs, EqRhs };
  struct PendingSite {
    SiteKind kind;
    uint32_t index;
    uint32_t ext;
    uint32_t group;
  };
  void site(SiteKind k, uint32_t index, const g::Operand& o, uint32_t gi) {
    if (o.kind != g::Operand::Kind::External) return;
    const bool voice = s_.externals[o.index].poly == g::Poly::Voice;
    pending_.push_back({k, index, o.index, voice ? gi : 0u});
  }

  // One record for schedule op i in group gi (the host has validated the op).
  bool makeRec(uint32_t i, uint32_t gi, bool loop, bool control) {
    const g::Op& op = s_.ops[i];
    InterpretedImage& im = *img_;
    Rec rec;
    rec.args.numIn = op.numIn;
    rec.args.numOut = op.numOut;
    rec.args.splMask = op.splMask;
    rec.args.frames = control ? 1u : 0u;
    rec.inBase = static_cast<uint32_t>(im.pool_.size());
    rec.firstReloc = static_cast<uint32_t>(im.relocs_.size());
    auto addOperand = [&](const g::Operand& o) {
      const uint32_t cell = static_cast<uint32_t>(im.pool_.size());
      float* p = host().resolve(o, gi);
      im.pool_.push_back(p);
      if (loop && !isPattern(o)) {
        im.relocs_.push_back({cell, lanesOf(o), p});
        site(SiteKind::Reloc, static_cast<uint32_t>(im.relocs_.size() - 1), o, gi);
      } else {
        site(SiteKind::Pool, cell, o, gi);
      }
    };
    const auto ins = s_.inputs(op);
    for (const g::Operand& o : ins) addOperand(o);
    rec.outBase = static_cast<uint32_t>(im.pool_.size());
    for (const g::Operand& o : s_.outputsOf(op)) addOperand(o);
    rec.numReloc = static_cast<uint32_t>(im.relocs_.size()) - rec.firstReloc;
    switch (op.kind) {
      case g::OpKind::Kernel: {
        rec.fn = dsp::findKernel(op.kernel, op.lanes, op.splMask);
        if (!rec.fn)
          return fail(std::string("no ") + dsp::kernelInfo(op.kernel).name + " kernel for " + std::to_string(op.lanes) +
                      " lanes with Spl mask " + std::to_string(op.splMask));
        rec.fnBase = rec.fn;
        rec.maskBase = op.splMask;
        if (!control && !s_.rampOf.empty()) {  // TX.smooth (validated by the host)
          uint32_t ramped = 0;
          rec.firstSwap = static_cast<uint32_t>(im.swaps_.size());
          for (uint32_t j = 0; j < op.numIn; ++j) {
            const uint32_t r = s_.rampOf[op.firstOperand + j];
            if (r == g::kNone) continue;
            const g::Ramp& rp = s_.ramps[r];
            Swap sw;
            sw.cell = rec.inBase + j;
            sw.ramp = host().resolve(rp.out, gi);
            sw.state = reinterpret_cast<const dsp::ParamRampState*>(host().statePtr(s_.state.entries[s_.ops[rp.op].state], gi));
            sw.stride = op.lanes;
            sw.checked = !rp.staticSource;
            im.swaps_.push_back(sw);
            ramped |= 1u << j;
          }
          rec.numSwaps = static_cast<uint32_t>(im.swaps_.size()) - rec.firstSwap;
          if (ramped != 0) {
            rec.maskRamp = op.splMask & ~ramped;
            rec.fnRamp = dsp::findKernel(op.kernel, op.lanes, rec.maskRamp);
            if (!rec.fnRamp) return fail(std::string("no ") + dsp::kernelInfo(op.kernel).name + " kernel for its ramps");
          }
        }
        if (op.state != g::kNone) rec.args.state = host().statePtr(s_.state.entries[op.state], gi);
        const dsp::Resource res = dsp::kernelInfo(op.kernel).resource;
        if (res != dsp::Resource::None) rec.args.resource = host().resource(res, op.resource);
        break;
      }
      case g::OpKind::Broadcast:
        rec.fn = &host::broadcastKernel;
        rec.args.splMask = isPattern(ins[0]) ? 1u : 0u;
        break;
      case g::OpKind::Reduce:
        rec.fn = &host::reduceKernel;
        rec.args.state = const_cast<host::ReduceInfo*>(host().reduceInfo(i));
        break;
      case g::OpKind::Send:
        rec.fn = &host::sendKernel;
        rec.args.state = const_cast<host::SendInfo*>(host().sendInfo(i));
        break;
    }
    im.recs_.push_back(rec);
    return true;
  }

  OperandRef operandRef(const g::Operand& o, uint32_t gi, bool loop) {
    OperandRef r;
    r.base = host().resolve(o, gi);
    r.stride = loop && !isPattern(o) ? lanesOf(o) : 0u;
    return r;
  }

  bool solveRun(const g::SolveBlock& sb, const g::Region& reg, uint32_t gi, uint32_t regionRecBase) {
    InterpretedImage& im = *img_;
    const bool loop = reg.kind == g::RegionKind::Loop;
    SolveRun r;
    r.iterations = std::max<uint32_t>(sb.iterations, 1);
    r.lanes = reg.kind == g::RegionKind::Control || reg.poly == g::Poly::Voice ? 8u : 1u;
    r.n = sb.numUnknowns;
    r.firstBodyRec = regionRecBase + (sb.firstOp - reg.firstOp);
    r.numBodyRec = sb.numOps;
    r.firstUnk = static_cast<uint32_t>(im.unks_.size());
    r.firstEq = static_cast<uint32_t>(im.eqs_.size());
    r.loop = loop;
    for (uint32_t k = 0; k < sb.numUnknowns; ++k) {
      const g::SolveUnknown& su = s_.unknowns[sb.firstUnknown + k];
      UnkRun u;
      u.value = operandRef(su.value, gi, loop);
      if (su.init.kind != g::Operand::Kind::None) {
        u.init = operandRef(su.init, gi, loop);
        site(SiteKind::UnkInit, static_cast<uint32_t>(im.unks_.size()), su.init, gi);
      }
      if (su.state != g::kNone) u.guess = reinterpret_cast<float*>(host().statePtr(s_.state.entries[su.state], gi));
      im.unks_.push_back(u);
    }
    for (uint32_t k = 0; k < sb.numEquations; ++k) {
      const g::SolveEquation& se = s_.equations[sb.firstEquation + k];
      EqRun e;
      e.lhs = operandRef(se.lhs, gi, loop);
      e.rhs = operandRef(se.rhs, gi, loop);
      site(SiteKind::EqLhs, static_cast<uint32_t>(im.eqs_.size()), se.lhs, gi);
      site(SiteKind::EqRhs, static_cast<uint32_t>(im.eqs_.size()), se.rhs, gi);
      im.eqs_.push_back(e);
    }
    adPlan(sb, r);
    r.finalEval = newton::bodyReadAfter(s_, sb);
    maxN_ = std::max(maxN_, r.n);
    im.solves_.push_back(r);
    return true;
  }

  // The tangent records of a solve block (newton::planTangents): one tangent slot per distinct buffer.
  void adPlan(const g::SolveBlock& sb, SolveRun& r) {
    InterpretedImage& im = *img_;
    const newton::TangentPlan plan = newton::planTangents(s_, sb);
    r.ad = plan.ad;
    if (!r.ad) return;
    std::unordered_map<uint32_t, uint32_t> slotOf;
    auto slot = [&](const g::Operand& o) -> uint32_t {
      if (o.kind != g::Operand::Kind::Buffer) return 0;
      return slotOf.emplace(o.index, static_cast<uint32_t>(slotOf.size())).first->second;
    };
    r.firstAdUnk = static_cast<uint32_t>(im.adPorts_.size());
    for (uint32_t j = 0; j < sb.numUnknowns; ++j) im.adPorts_.push_back({slot(s_.unknowns[sb.firstUnknown + j].value), 1u << j});
    r.firstAdOp = static_cast<uint32_t>(im.adOps_.size());
    for (uint32_t k = 0; k < sb.numOps; ++k) {
      const g::Op& op = s_.ops[sb.firstOp + k];
      AdOp a;
      a.step = newton::scalarTangentStep(static_cast<dsp::Atom>(op.kernel));
      a.numIn = op.numIn;
      a.numOut = op.numOut;
      a.firstIn = static_cast<uint32_t>(im.adPorts_.size());
      const auto ins = s_.inputs(op);
      for (uint32_t i = 0; i < op.numIn; ++i) {
        const uint32_t nz = plan.inNz[plan.firstIn[k] + i];
        im.adPorts_.push_back({nz != 0 ? slot(ins[i]) : 0u, nz});
      }
      a.firstOut = static_cast<uint32_t>(im.adPorts_.size());
      const auto outs = s_.outputsOf(op);
      for (uint32_t o = 0; o < op.numOut; ++o) {
        const uint32_t nz = plan.outNz[plan.firstOut[k] + o];
        im.adPorts_.push_back({nz != 0 ? slot(outs[o]) : 0u, nz});
        a.outAny |= nz;
      }
      maxAdIn_ = std::max(maxAdIn_, a.numIn);
      maxAdOut_ = std::max(maxAdOut_, a.numOut);
      im.adOps_.push_back(a);
    }
    r.firstAdEq = static_cast<uint32_t>(im.adPorts_.size());
    for (uint32_t q = 0; q < sb.numEquations; ++q) {
      const g::SolveEquation& e = s_.equations[sb.firstEquation + q];
      im.adPorts_.push_back({plan.lhsNz[q] != 0 ? slot(e.lhs) : 0u, plan.lhsNz[q]});
      im.adPorts_.push_back({plan.rhsNz[q] != 0 ? slot(e.rhs) : 0u, plan.rhsNz[q]});
    }
    maxTanFloats_ = std::max<size_t>(maxTanFloats_, static_cast<size_t>(slotOf.size()) * sb.numUnknowns * 8);
  }

  bool stages() {
    InterpretedImage& im = *img_;
    for (const g::Stage& st : s_.stages) {
      const bool voice = st.poly == g::Poly::Voice;
      const uint32_t groups = voice ? G_ : 1u;
      StageRegions sr;
      sr.firstRegion = static_cast<uint32_t>(im.regions_.size());
      sr.numRegions = st.numRegions;
      for (uint32_t gi = 0; gi < groups; ++gi) {
        for (uint32_t ri = st.firstRegion; ri < st.firstRegion + st.numRegions; ++ri) {
          const g::Region& reg = s_.regions[ri];
          const bool loop = reg.kind == g::RegionKind::Loop;
          const bool control = reg.kind == g::RegionKind::Control;
          RegionRun rr;
          rr.kind = reg.kind;
          rr.schedRegion = ri;
          rr.subBlock = reg.subBlock;
          rr.factor = reg.factor;
          rr.firstRec = static_cast<uint32_t>(im.recs_.size());
          for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i)
            if (!makeRec(i, gi, loop, control)) return false;
          rr.numRec = static_cast<uint32_t>(im.recs_.size()) - rr.firstRec;
          if (!branches(ri, rr)) return false;
          rr.firstRampRec = static_cast<uint32_t>(im.rampRecs_.size());
          for (uint32_t q = rr.firstRec; q < rr.firstRec + rr.numRec; ++q)
            if (im.recs_[q].numSwaps != 0) im.rampRecs_.push_back(q);
          rr.numRampRec = static_cast<uint32_t>(im.rampRecs_.size()) - rr.firstRampRec;
          // TX.smooth-fix1: what a ramp-free block runs of the region's ramps (runRegion).
          for (uint32_t q = rr.firstRampRec; q < rr.firstRampRec + rr.numRampRec; ++q) {
            const Rec& rec = im.recs_[im.rampRecs_[q]];
            for (uint32_t k = rec.firstSwap; k < rec.firstSwap + rec.numSwaps; ++k)
              rr.anyChecked = rr.anyChecked || im.swaps_[k].checked;
          }
          rr.rampOps = reg.numOps > 0 && s_.ops[reg.firstOp].kind == g::OpKind::Kernel &&
                       s_.ops[reg.firstOp].kernel == dsp::Atom::ParamRamp;
          rr.firstChecked = static_cast<uint32_t>(im.rampChecked_.size());
          if (rr.rampOps)
            for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i)
              for (const g::Ramp& rp : s_.ramps)
                if (rp.op == i && !rp.staticSource) im.rampChecked_.push_back(rr.firstRec + (i - reg.firstOp));
          rr.numChecked = static_cast<uint32_t>(im.rampChecked_.size()) - rr.firstChecked;
          rr.memo = control && !voice && reg.numSolves == 0 && memoizable(reg);
          if (reg.numSolves > 0) {
            rr.firstItem = static_cast<uint32_t>(im.items_.size());
            uint32_t sIdx = reg.firstSolve;
            const uint32_t sEnd = reg.firstSolve + reg.numSolves;
            uint32_t i = reg.firstOp;
            const uint32_t end = reg.firstOp + reg.numOps;
            while (i < end || sIdx < sEnd) {
              if (sIdx < sEnd && s_.solves[sIdx].firstOp <= i) {
                const g::SolveBlock& sb = s_.solves[sIdx];
                im.items_.push_back(kSolveItem | static_cast<uint32_t>(im.solves_.size()));
                if (!solveRun(sb, reg, gi, rr.firstRec)) return false;
                i = std::max(i, sb.firstOp + sb.numOps);
                ++sIdx;
                continue;
              }
              if (i >= end) return fail("solve block after the end of its region");
              im.items_.push_back(rr.firstRec + (i - reg.firstOp));
              ++i;
            }
            rr.numItems = static_cast<uint32_t>(im.items_.size()) - rr.firstItem;
          }
          im.regions_.push_back(rr);
        }
      }
      im.stages_.push_back(sr);
    }
    return true;
  }

  // TX.uniform-branch: the branches of schedule region ri (one group's records rr) and the guards of its records.
  bool branches(uint32_t ri, RegionRun& rr) {
    InterpretedImage& im = *img_;
    rr.firstBranch = static_cast<uint32_t>(im.branches_.size());
    if (s_.branches.empty()) return true;
    if (s_.guardOf.size() != s_.ops.size()) return fail("the branch guards do not match the ops");
    const g::Region& reg = s_.regions[ri];
    std::vector<uint32_t> local(s_.branches.size(), g::kNone);  // schedule branch -> image branch (this region, group)
    for (uint32_t b = 0; b < s_.branches.size(); ++b) {
      const g::Branch& sb = s_.branches[b];
      if (sb.region != ri) continue;
      if (sb.op < reg.firstOp || sb.op >= reg.firstOp + reg.numOps || reg.kind != g::RegionKind::Block)
        return fail("a branch outside its Block region");
      const g::Op& op = s_.ops[sb.op];
      if (op.kind != g::OpKind::Kernel || op.kernel != dsp::Atom::Select || op.numIn != 3 || op.numOut != 1 ||
          (op.splMask & 1u) == 0)
        return fail("a branch whose op is not a select of a pattern condition");
      if (sb.parent != g::kNone && ((sb.parent >> 1) >= b || local[sb.parent >> 1] == g::kNone))
        return fail("a branch whose parent is not an earlier branch of its region");
      BranchRun br;
      br.rec = rr.firstRec + (sb.op - reg.firstOp);
      br.parent = sb.parent == g::kNone ? g::kNone : 2 * local[sb.parent >> 1] + (sb.parent & 1u);
      for (uint32_t m = 0; m < 8; ++m) {
        br.select[m] = dsp::findKernel(dsp::Atom::Select, op.lanes, m);
        if (!br.select[m]) return fail("no select kernel for a branch");
      }
      local[b] = static_cast<uint32_t>(im.branches_.size());
      im.recs_[br.rec].branch = local[b];
      im.branches_.push_back(br);
    }
    rr.numBranches = static_cast<uint32_t>(im.branches_.size()) - rr.firstBranch;
    for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i) {
      const uint32_t gd = s_.guardOf[i];
      if (gd == g::kNone) continue;
      if ((gd >> 1) >= local.size() || local[gd >> 1] == g::kNone) return fail("an op guarded by a branch of another region");
      im.recs_[rr.firstRec + (i - reg.firstOp)].guard = 2 * local[gd >> 1] + (gd & 1u);
    }
    im.guardOn_.assign(2 * im.branches_.size(), 0);
    return true;
  }

  // See RegionRun::memo. The schedule's Buffer operands are physical buffers, so "nothing else writes them" also
  // excludes a buffer shared (by interval reuse) with a value of another region.
  bool memoizable(const g::Region& reg) const {
    std::vector<uint32_t> written;
    for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i) {
      const g::Op& op = s_.ops[i];
      if (op.kind != g::OpKind::Kernel || op.state != g::kNone || op.resource != g::kNone) return false;
      for (const g::Operand& o : s_.inputs(op)) {
        if (o.kind == g::Operand::Kind::Param || o.kind == g::Operand::Kind::Const) continue;
        if (o.kind != g::Operand::Kind::Buffer) return false;  // an engine source (transport …) may change
        if (std::find(written.begin(), written.end(), o.index) == written.end()) return false;  // from elsewhere
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

  // Pointers into the final vectors.
  void finish() {
    InterpretedImage& im = *img_;
    for (Rec& r : im.recs_) {
      r.args.in = const_cast<const float* const*>(im.pool_.data() + r.inBase);
      r.args.out = im.pool_.data() + r.outBase;
    }
    std::stable_sort(pending_.begin(), pending_.end(), [&](const PendingSite& a, const PendingSite& b) {
      const bool va = s_.externals[a.ext].poly == g::Poly::Voice, vb = s_.externals[b.ext].poly == g::Poly::Voice;
      if (va != vb) return !va;
      return a.group < b.group;
    });
    for (const PendingSite& p : pending_) {
      Site st;
      st.ext = p.ext;
      st.group = p.group;
      switch (p.kind) {
        case SiteKind::Pool: st.target = &im.pool_[p.index]; break;
        case SiteKind::Reloc: st.target = &im.relocs_[p.index].base; break;
        case SiteKind::UnkInit: st.target = &im.unks_[p.index].init.base; break;
        case SiteKind::EqLhs: st.target = &im.eqs_[p.index].lhs.base; break;
        case SiteKind::EqRhs: st.target = &im.eqs_[p.index].rhs.base; break;
      }
      im.sites_.push_back(st);
    }
    im.numGlobalSites_ = 0;
    while (im.numGlobalSites_ < im.sites_.size() && s_.externals[im.sites_[im.numGlobalSites_].ext].poly != g::Poly::Voice)
      ++im.numGlobalSites_;
    im.voiceSiteBegin_.assign(G_ + 1, static_cast<uint32_t>(im.sites_.size()));
    for (uint32_t gi = 0; gi <= G_; ++gi) {
      uint32_t k = im.numGlobalSites_;
      while (k < im.sites_.size() && im.sites_[k].group < gi) ++k;
      im.voiceSiteBegin_[gi] = k;
    }
    // Every cell starts at silence until the first block resolves it.
    for (const Site& st : im.sites_) *st.target = host().zeros();
    im.maxSolveN_ = maxN_;
    if (maxN_ > 0) {  // runSolve's layout: u, r0, rp, rm, up + um, J, Jfd, M (doubles)
      const size_t floats = static_cast<size_t>(maxN_) * 8 * 4 + 16 + 2 * static_cast<size_t>(maxN_) * maxN_ * 8 +
                            2 * static_cast<size_t>(maxN_) * (maxN_ + 1) + 16;
      im.solveMem_ = AlignedBuffer<float>(floats);
    }
    if (maxTanFloats_ > 0) im.tanMem_ = AlignedBuffer<float>(maxTanFloats_);
    im.adIn_.assign(std::max<uint32_t>(maxAdIn_, 1), {});
    im.adOut_.assign(std::max<uint32_t>(maxAdOut_, 1), {});
  }

  const g::Schedule& s_;
  const InterpretOptions& o_;
  std::unique_ptr<InterpretedImage> img_;
  Error err_;
  uint32_t G_ = 1;
  std::vector<PendingSite> pending_;
  uint32_t maxN_ = 0;
  uint32_t maxAdIn_ = 0, maxAdOut_ = 0;
  size_t maxTanFloats_ = 0;
};

}  // namespace

Result<std::unique_ptr<ProgramImage>> buildInterpreted(const graph::Schedule& schedule, const InterpretOptions& options) {
  return Builder(schedule, options).build();
}

double estimateInterpretedNsPerFrame(const graph::Schedule& s, uint32_t voiceGroups) noexcept {
  constexpr double kOpNs = 1.1, kSolveOpNs = 15.0;
  double ns = 0.0;
  for (const graph::Stage& st : s.stages) {
    const double mult = st.poly == graph::Poly::Voice ? static_cast<double>(std::max<uint32_t>(voiceGroups, 1)) : 1.0;
    for (uint32_t r = st.firstRegion; r < st.firstRegion + st.numRegions && r < s.regions.size(); ++r) {
      const graph::Region& reg = s.regions[r];
      if (reg.kind == graph::RegionKind::Control) continue;  // once per block
      double regionNs = kOpNs * reg.numOps;
      for (uint32_t k = reg.firstSolve; k < reg.firstSolve + reg.numSolves && k < s.solves.size(); ++k)
        regionNs += kSolveOpNs * s.solves[k].numOps * std::max<uint32_t>(s.solves[k].iterations, 1);
      ns += mult * regionNs;
    }
  }
  return ns;
}

}  // namespace pq::engine
