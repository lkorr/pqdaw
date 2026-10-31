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
  uint32_t firstRec = 0, numRec = 0;
  uint32_t firstItem = 0, numItems = 0;  // with solve blocks: rec index, or kSolveItem | solve index
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

class InterpretedImage final : public ProgramImage {
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
  void runMetered(ProcessContext& ctx) noexcept;
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

void InterpretedImage::runRegion(const RegionRun& r, uint32_t frames) noexcept {  // RT-safe
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
            rec.args.frames = n;
            rec.fn(rec.args);
          }
        }
      }
      return;
    }
  }
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
      [&](uint32_t k) noexcept {
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
      [&](uint32_t k, uint32_t gi) noexcept {
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
      [&](uint32_t k) noexcept {
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
      [&](uint32_t k, uint32_t gi) noexcept {
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
          rr.firstRec = static_cast<uint32_t>(im.recs_.size());
          for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i)
            if (!makeRec(i, gi, loop, control)) return false;
          rr.numRec = static_cast<uint32_t>(im.recs_.size()) - rr.firstRec;
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
