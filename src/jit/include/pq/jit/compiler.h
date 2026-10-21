#pragma once

// The LLVM JIT backend (T1b.1, SPEC §7.4 step 6): a graph::Schedule → an engine::ProgramImage with the same ABI as
// the interpreter (pq/engine/interpreter.h), compiled to native code for the host CPU with LLVM ORC LLJIT.
//
// What the image computes is exactly what the interpreter computes (same slots, state layout, engine data, special
// ops, Newton scheme), bit for bit except where fast-math contraction is allowed; tests/jit compares the two on the
// whole T1.7 corpus. How: one generated function per stage; element-wise ops are fused into loops (vectorised over
// time for global signals, over the 8 voices of a group for voice signals), per-sample feedback loops and Control
// regions are generated inline including the simple stateful atoms, and the remaining atoms call the dsp kernels
// (src/jit/src/codegen.h has the details and the inlined-vs-called list).
//
// Target: the host CPU (llvm::sys::getHostCPUName + host features, e.g. AVX-512 when present). IR pipeline: a light
// one by default (CompileOptions::optLevel 1), measured as fast as LLVM's O2/O3 on the generated code at a fraction
// of the compile time (bench/RESULTS.md); machine code at CodeGenOptLevel::Aggressive. Fast-math policy
// (TASKS T1b.1): `contract` and `afn` only, never nnan/ninf/nsz/reassoc; the functions declare FTZ/DAZ denormal
// handling, which the audio thread's DenormalGuard provides (generated code uses SSE/AVX only, so MXCSR applies;
// it never changes MXCSR itself).
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
  bool contract = true;            // allow fmul + fadd → fma contraction (and `afn`); false: no fast-math flags
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
};

class Compiler {
public:
  // Creates the LLJIT instance for the host CPU. Not RT-safe.
  static Result<std::unique_ptr<Compiler>> create();
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
