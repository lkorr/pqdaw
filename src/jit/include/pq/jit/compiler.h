#pragma once

// The LLVM JIT backend (T1b.1, SPEC §7.4 step 6): a graph::Schedule → an engine::ProgramImage with the same ABI as
// the interpreter (pq/engine/interpreter.h), compiled to native code for the host CPU with LLVM ORC LLJIT.
//
// What the image computes is exactly what the interpreter computes (same slots, state layout, engine data, special
// ops, Newton scheme), bit for bit with the default options; tests/jit compares the two on the whole T1.7 corpus plus
// the M1 review probes (T1b.1-fix1). Opting into contraction (CompileOptions::contract) gives up bit-identity. How: one generated function per stage; element-wise ops are fused into loops (vectorised over
// time for global signals, over the 8 voices of a group for voice signals), per-sample feedback loops and Control
// regions are generated inline including the simple stateful atoms, and the remaining atoms call the dsp kernels
// (src/jit/src/codegen.h has the details and the inlined-vs-called list).
//
// Target: the host CPU (llvm::sys::getHostCPUName + host features, e.g. AVX-512 when present). IR pipeline: a light
// one by default (CompileOptions::optLevel 1), measured as fast as LLVM's O2/O3 on the generated code at a fraction
// of the compile time (bench/RESULTS.md); machine code at CompilerConfig::codeGenLevel. Fast-math policy
// (T1b.1-fix1, QUESTIONS Q64 reversed): no fast-math flags by default, so every float operation rounds exactly as
// the interpreter's kernels do. A feedback loop amplifies any one-ulp difference without bound (the M1 review's
// 5-op logistic map differed by 0.988 with contraction), and T1b.5's interpreter → JIT swap must be seamless.
// `contract = true` opts into `contract` + `afn` (never nnan/ninf/nsz/reassoc) for patches that are not
// chaotic; the a·b + c fusions that matter are already explicit muladd atoms from the optimiser, so it buys
// little (bench/RESULTS.md, T1b.1-fix1). The functions declare FTZ/DAZ denormal handling, which the audio
// thread's DenormalGuard provides (generated code uses SSE/AVX only, so MXCSR applies; it never changes MXCSR
// itself).
//
// Threads: Compiler::compile is NOT RT-safe (the compiler worker thread; it allocates, takes a lock, runs LLVM).
// The returned image's process() is RT-safe. An image keeps its machine code alive on its own (it may outlive the
// Compiler); destroy it off the audio thread.

#include <cstdint>
#include <memory>
#include <string>

#include "pq/core/result.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/program_image.h"
#include "pq/graph/schedule.h"

namespace pq::jit {

struct CompileOptions {
  uint32_t maxBlockFrames = 1024;  // largest ProcessContext::frames
  uint32_t maxVoiceGroups = 8;     // voice groups with state (8 → 64 voices)
  const engine::Resources* resources = nullptr;  // as InterpretOptions::resources (must outlive the image)
  uint32_t optLevel = 1;           // 0: no IR passes; 1: the light pipeline (SROA, CSE, LICM; compiler.cpp); 2/3: LLVM O2/O3
  bool contract = false;           // true: allow fmul + fadd → fma contraction (and `afn`); NOT bit-identical to the
                                   // interpreter (and unboundedly different in chaotic feedback loops)
};

struct CompileStats {
  double hostMs = 0;          // schedule checks, memory layout, state init
  double codegenMs = 0;       // Schedule → LLVM IR
  double optimiseMs = 0;      // LLVM IR pipeline
  double machineCodeMs = 0;   // instruction selection, register allocation, linking (ORC)
  double totalMs = 0;
  uint32_t irInstructions = 0;  // after optimisation
  uint32_t inlinedOps = 0;      // schedule ops generated inline
  uint32_t calledOps = 0;       // schedule ops that call a dsp kernel or a runtime helper
  uint64_t objectBytes = 0;     // size of the machine-code object(s) linked
  bool stackProbes = false;     // the object calls __chkstk: some stage function has a frame > 4 KiB, whose guard-page
                                // probes may fault on the audio thread the first time (must stay false; tests/jit)
};

// Per-Compiler settings (one LLJIT instance and target machine each).
struct CompilerConfig {
  // LLVM CodeGenOptLevel for instruction selection and register allocation: 0 None (FastISel, fast register
  // allocator), 1 Less, 2 Default, 3 Aggressive. Measured in bench/RESULTS.md (T1b.1-fix1).
  uint32_t codeGenLevel = kDefaultCodeGenLevel;
  static constexpr uint32_t kDefaultCodeGenLevel = 3;  // Aggressive, as in T1b.1: levels 1–3 measured equal (Q?-T1b.1-fix1-b)
};

class Compiler {
public:
  // Creates the LLJIT instance for the host CPU. Not RT-safe. Errors: codeGenLevel > 3, LLVM failures.
  static Result<std::unique_ptr<Compiler>> create(const CompilerConfig& config = {});
  ~Compiler();
  Compiler(const Compiler&) = delete;
  Compiler& operator=(const Compiler&) = delete;

  // Not RT-safe. Errors: whatever buildInterpreted() rejects (invalid options, malformed schedules, unknown engine
  // sources), and LLVM failures (reported, never thrown).
  Result<std::unique_ptr<engine::ProgramImage>> compile(const graph::Schedule& schedule, const CompileOptions& options = {},
                                                        CompileStats* stats = nullptr);

  const std::string& hostCpu() const noexcept;       // e.g. "rocketlake"
  const std::string& hostFeatures() const noexcept;  // "+avx2,+fma,…"

  struct Session;  // LLJIT + target machine, shared with the images it compiled

private:
  explicit Compiler(std::shared_ptr<Session> s) noexcept;
  std::shared_ptr<Session> session_;
};

}  // namespace pq::jit
