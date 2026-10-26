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
// Developer aid: with the environment variable PQ_JIT_DUMP=<path prefix>, compile() writes every image's optimised
// IR (<prefix><n>.ll) and assembly (<prefix><n>.s), n = the image's number in this process (T1b.2).
//
// Compile cache (T1b.4, pq/jit/cache.h): compile() first looks the machine code up in memory (a live image with the
// same key shares it), then on disk (CacheConfig), and runs LLVM only on a miss. A cached image is bit-identical in
// output to a freshly compiled one. counters() says what happened (the "no JIT compile" test hook).
//
// Threads: Compiler::compile is NOT RT-safe (the compiler worker thread; it allocates, takes a lock, runs LLVM, reads
// and writes the cache directory). The returned image's process() is RT-safe. An image keeps its machine code alive
// on its own (it may outlive the Compiler); destroy it off the audio thread. Destroying it never waits for a compile in
// progress (M1 review L6): its code is released at once when the compiler is idle, else by that compile when it ends.

#include <cstdint>
#include <memory>
#include <string>

#include "pq/core/result.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/program_image.h"
#include "pq/graph/schedule.h"
#include "pq/jit/cache.h"

namespace pq::jit {

// How voice stages are generated (T1b.2, src/jit/src/codegen.h). Every choice computes the same bits.
enum class VoiceCode : uint8_t {
  Auto,         // the default: Pairs512 on hosts with AVX-512, Pairs256 elsewhere (QUESTIONS Q111)
  Groups,       // one voice group (8 lanes, <8 x float>) per call: T1b.1's code
  Pairs256,     // two occupied groups per call as <16 x float> on two 256-bit halves; a lone group as Groups
  Pairs512,     // the same on 512-bit registers (AVX-512 hosts; Pairs256 on hosts without AVX-512F)
  ScalarLanes,  // the benchmark baseline "scalar JIT": one voice lane per call, scalar code. An error for a schedule
                // whose voice stages need kernel or special-op calls (only fully inline voices can be run this way)
};

struct CompileOptions {
  uint32_t maxBlockFrames = 1024;  // largest ProcessContext::frames
  uint32_t maxVoiceGroups = 8;     // voice groups with state (8 → 64 voices)
  const engine::Resources* resources = nullptr;  // as InterpretOptions::resources (must outlive the image)
  uint32_t optLevel = 1;           // 0: no IR passes; 1: the light pipeline (SROA, CSE, LICM; compiler.cpp); 2/3: LLVM O2/O3
  bool contract = false;           // true: allow fmul + fadd → fma contraction (and `afn`); NOT bit-identical to the
                                   // interpreter (and unboundedly different in chaotic feedback loops)
  VoiceCode voiceCode = VoiceCode::Auto;
  engine::SleepMode sleep = engine::SleepMode::On;  // T1.x-silence (pq/engine/program_image.h); not part of the code
  // T1b.x-perf (QUESTIONS Q111 option C): a quick FIRST image. When the voice code would give some voice stage a pair
  // function and the cache has no image of these options, the image is compiled with VoiceCode::Groups instead
  // (roughly half the compile of a big voice stage) and not stored on disk, and CompileStats::refine says so: compiling
  // again without quickFirst gives the final image, bit-identical in output (so an Instant swap replaces the first).
  // A cache hit, or options without pairs to add, give the final image at once (refine = false). Not in the key.
  bool quickFirst = false;
};

struct CompileStats {
  enum class Source : uint8_t { Compiled, Memory, Disk, Aot };  // where the machine code came from (pq/jit/cache.h)
  Source source = Source::Compiled;
  double cacheMs = 0;         // key + lookups + (hits) loading and linking the object, (compiles) writing the entry
  double hostMs = 0;          // schedule checks, memory layout, state init
  double codegenMs = 0;       // Schedule → LLVM IR
  double optimiseMs = 0;      // LLVM IR pipeline
  double machineCodeMs = 0;   // instruction selection, register allocation, linking (ORC)
  double totalMs = 0;
  uint32_t irInstructions = 0;  // after optimisation
  uint32_t inlinedOps = 0;      // schedule ops generated inline
  uint32_t calledOps = 0;       // schedule ops that call a dsp kernel or a runtime helper
  uint32_t pairedStages = 0;    // voice stages that got a two-group function (VoiceCode::Pairs*, T1b.2)
  uint64_t objectBytes = 0;     // size of the machine-code object(s) linked
  bool stackProbes = false;     // the object calls __chkstk: some stage function has a frame > 4 KiB, whose guard-page
                                // probes may fault on the audio thread the first time (must stay false; tests/jit)
  bool refine = false;          // CompileOptions::quickFirst gave a first image: compile without it for the final one
};

// What the machine code is compiled for.
enum class Target : uint8_t {
  Host,      // this CPU and every feature it has (AVX-512 where present)
  Baseline,  // kBaselineCpu (x86-64-v3: AVX2, FMA, BMI2), pqdaw's own baseline, so it runs wherever pqdaw runs. The
             // AOT seed is compiled for it (QUESTIONS Q116); a Host compiler finds such entries in
             // CacheConfig::readOnly directories.
};
inline constexpr const char* kBaselineCpu = "x86-64-v3";

// Per-Compiler settings (one LLJIT instance and target machine each).
struct CompilerConfig {
  // LLVM CodeGenOptLevel for instruction selection and register allocation: 0 None (FastISel, fast register
  // allocator), 1 Less, 2 Default, 3 Aggressive. Measured in bench/RESULTS.md (T1b.1-fix1).
  uint32_t codeGenLevel = kDefaultCodeGenLevel;
  static constexpr uint32_t kDefaultCodeGenLevel = 3;  // Aggressive, as in T1b.1: levels 1–3 measured equal (Q99)
  Target target = Target::Host;
  CacheConfig cache;  // default: code shared in memory, no disk cache
  // Test hook: called by compile() on its thread, with the compiler's lock held, right before an LLVM compile (cache
  // misses only); may block (a simulated slow compile).
  void (*beforeCompile)(void* user) = nullptr;
  void* beforeCompileUser = nullptr;
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

  // The CPU the machine code is for: the host's name and features (e.g. "rocketlake", "+avx2,+fma,…"), or
  // kBaselineCpu and no extra features with Target::Baseline.
  const std::string& hostCpu() const noexcept;
  const std::string& hostFeatures() const noexcept;

  // Thread-safe; applies from the next compile(). Images already built keep their code.
  void setCache(const CacheConfig& cache);
  CacheConfig cache() const;
  CacheCounters counters() const;  // thread-safe snapshot

  struct Session;  // LLJIT + target machine, shared with the images it compiled

private:
  explicit Compiler(std::shared_ptr<Session> s) noexcept;
  // compile() without quickFirst. cacheOnly: ErrorCode::NotFound instead of an LLVM compile on a cache miss; store:
  // false keeps a compiled image off the disk cache.
  Result<std::unique_ptr<engine::ProgramImage>> compileImpl(const graph::Schedule& schedule, const CompileOptions& options,
                                                            CompileStats* stats, bool cacheOnly, bool store);
  bool pairsPlanned(const graph::Schedule& schedule, const CompileOptions& options) const;
  std::shared_ptr<Session> session_;
};

}  // namespace pq::jit
