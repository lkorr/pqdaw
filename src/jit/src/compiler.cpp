// Compiler: LLJIT for the target CPU, the IR pipeline, image assembly and the compile cache (pq/jit/compiler.h,
// pq/jit/cache.h).

#include "pq/jit/compiler.h"

#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/LICM.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>
#include <llvm/Transforms/Scalar/LoopRotation.h>
#include <llvm/Transforms/Scalar/LoopStrengthReduce.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Support/FileSystem.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <new>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <system_error>
#include <thread>
#include <unordered_set>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "cache.h"
#include "codegen.h"
#include "jit_image.h"

namespace pq::jit {

namespace {

double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

Error llvmError(const char* what, llvm::Error e) {
  return Error{ErrorCode::Unknown, std::string("jit: ") + what + ": " + llvm::toString(std::move(e)), {}};
}

// TMM.13-fix1 L10: an allocation LLVM cannot make ("Buffer allocation failed", SmallVector growth …) killed the process
// (exit / abort: the DAW with unsaved work). It throws std::bad_alloc instead, which Compiler::compile catches: that
// compile fails (the previous image keeps playing) and the session is poisoned (LLVM is built without exceptions, so
// its frames were unwound without their destructors: its state is not trusted again — later compiles fail at once,
// its code is never released, the app keeps running on the images it has and the interpreter). The handler allocates
// nothing.
[[noreturn]] void onLlvmBadAlloc(void*, const char*, bool) { throw std::bad_alloc(); }

void initialiseLlvmOnce() {
  static std::once_flag once;
  std::call_once(once, [] {
    llvm::install_bad_alloc_error_handler(&onLlvmBadAlloc);
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    // PF.1: no Loop Strength Reduction in the machine-code pipeline: it was 15 % (+ IV Users 3.4 %) of a compile, spent
    // on the long global stages, where it gained no run time (bench/RESULTS.md "PF.1"). Small functions still get it
    // from the light IR pipeline (SmallFunctionLsr). LLVM's options are process-wide; only the JIT runs LLVM here.
    const char* args[] = {"pqdaw", "-disable-lsr"};
    [[maybe_unused]] const bool parsed = llvm::cl::ParseCommandLineOptions(2, args, "", &llvm::nulls());
    assert(parsed && "LLVM has no -disable-lsr");
  });
}

// optLevel 1 (the default): the few IR passes generated code needs, since codegen.h already emits SSA values,
// explicit vectors and hoisted invariants: SROA (the Newton unknowns' allocas), CSE, and LICM after loop rotation,
// which keeps per-sample state (z1, phasor …) in registers across a loop. Measured against LLVM's O2/O3 pipelines
// (bench/RESULTS.md): the same run time within noise at 3–5× less compile time. InstCombine is left out on purpose:
// with it the interleaved fused loops ran slower (chain32_fast 4.5 vs 3.8 ns/sample).
//
// PF.1: Loop Strength Reduction runs here, on small functions only, instead of in the machine-code pipeline (which runs
// it on every loop; -disable-lsr, initialiseLlvmOnce). On the long global stages it cost ~15 % of a compile and
// gained nothing; on small voice loops it still pays (fm_osc x8: 1.15 vs 1.40 ns/voice-sample without it).
constexpr unsigned kLsrMaxInstructions = 2000;
struct SmallFunctionLsr : llvm::PassInfoMixin<SmallFunctionLsr> {
  llvm::FunctionToLoopPassAdaptor lsr = llvm::createFunctionToLoopPassAdaptor(llvm::LoopStrengthReducePass());
  llvm::PreservedAnalyses run(llvm::Function& f, llvm::FunctionAnalysisManager& fam) {
    if (f.getInstructionCount() > kLsrMaxInstructions) return llvm::PreservedAnalyses::all();
    return lsr.run(f, fam);
  }
};

llvm::ModulePassManager lightPipeline() {
  llvm::FunctionPassManager fpm;
  fpm.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
  fpm.addPass(llvm::EarlyCSEPass(true));
  fpm.addPass(llvm::SimplifyCFGPass());
  llvm::LoopPassManager lpm;
  lpm.addPass(llvm::LoopRotatePass());
  lpm.addPass(llvm::LICMPass(llvm::LICMOptions()));
  fpm.addPass(llvm::createFunctionToLoopPassAdaptor(std::move(lpm), /*UseMemorySSA=*/true));
  fpm.addPass(llvm::SimplifyCFGPass());
  fpm.addPass(SmallFunctionLsr());
  llvm::ModulePassManager mpm;
  mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
  return mpm;
}

// One image's machine code: its own JITDylib (so two copies of one object, e.g. a disk entry loaded while a fresh
// compile of the same key is alive, never collide on symbol names), the stage functions, and the pointer-table slots
// after the host's. Shared by every live image with the same key; released with the last of them (CodeReleaser: while
// no compile runs in the session, never waiting for one — M1 review L6).
struct SharedCode {
  detail::Key key;
  llvm::orc::ExecutionSession* es = nullptr;
  llvm::orc::JITDylib* jd = nullptr;
  std::vector<detail::StageFn> fns;
  std::vector<detail::StageFn> pairFns;  // per stage: the two-group function or null (T1b.2)
  uint32_t pairedStages = 0;
  std::vector<const void*> fnSlots;  // pointer-table slots hostSlots…: functions (and data) inside the executable
  uint32_t hostSlots = 0;
  detail::Key irHash;                // recorded when compiled with CacheConfig::verify (else zero) …
  detail::Key irKey;                 // … from IR whose functions are named after this key
  bool baselineIr = false;           // … for Target::Baseline
  uint64_t objectBytes = 0;
  uint32_t objects = 0;
  bool stackProbes = false;
  uint32_t irInstructions = 0, inlinedOps = 0, calledOps = 0;
  SharedCode() = default;
  SharedCode(const SharedCode&) = delete;
  SharedCode& operator=(const SharedCode&) = delete;
  ~SharedCode() {
    if (es != nullptr && jd != nullptr) llvm::consumeError(es->removeJITDylib(*jd));
  }
};

}  // namespace

// PF.4 (survey #5): compiles run concurrently (BackgroundCompiler's pool). They share the LLJIT (ORC's
// ExecutionSession and object linking layer are thread-safe; each compile links its own JITDylib) and the session
// state below; each compile has its own LLVMContext and module and leases a TargetMachine of its own for the IR
// pipeline and instruction selection (a TargetMachine caches subtargets: one thread at a time). The machine code is
// compiled to an object here (orc::SimpleCompiler on the leased TargetMachine) and linked with addObjectFile, as a
// cached entry is: the same bytes whatever the number of compile threads.
struct Compiler::Session {
  std::unique_ptr<llvm::orc::LLJIT> jit;
  std::optional<llvm::orc::JITTargetMachineBuilder> jtmb;  // what LLJIT targets: makes the leased TargetMachines
  std::string cpu, features;
  uint32_t codeGenLevel = CompilerConfig::kDefaultCodeGenLevel;
  bool baseline = false;
  void (*beforeCompile)(void*) = nullptr;  // CompilerConfig's test hook
  void* beforeCompileUser = nullptr;

  // Idle TargetMachines (one is leased per compile in flight; made on demand, kept for the next compile).
  std::mutex tmMutex;
  std::vector<std::unique_ptr<llvm::TargetMachine>> tms;

  // `llvmGate`: every compile holds it SHARED while it runs (lookups, loads, LLVM); releasing machine code takes it
  // EXCLUSIVE, so code is removed only while no compile runs (TMM.13-fix1 L10: after an LLVM out-of-memory unwind
  // LLVM's state is not trusted; nothing is removed then). A release never WAITS for it (M1 review L6: an image freed
  // off the audio thread must not stall behind a compile of seconds): a release that finds a compile running parks
  // the code in `released`, and the last compile to leave frees it (drainIfIdle).
  std::shared_mutex llvmGate;
  std::mutex releaseMutex;                 // guards `released` only (held for a push or a swap)
  std::vector<SharedCode*> released;       // codes whose last image is gone, waiting for the gate
  std::atomic<bool> pendingRelease{false};
  std::atomic<uint64_t> releasesDeferred{0};
  std::atomic<bool> poisoned{false};  // TMM.13-fix1 L10: LLVM ran out of memory inside a compile (see onLlvmBadAlloc)
  std::atomic<uint64_t> nextId{0};    // image numbers (PQ_JIT_DUMP) and JITDylib names
  std::atomic<uint32_t> active{0};    // compiles running (Compiling)

  // `mutex`: short holds only (never across LLVM work or file I/O, never while a SharedCode reference is dropped):
  // the cache configuration, the counters, the live codes and the keys being compiled.
  mutable std::mutex mutex;
  CacheConfig cache;
  CacheCounters counters;
  // Live code by key (weak: an image's code lives as long as its images). Every code is released while LLJIT
  // lives (~Session).
  std::unordered_map<detail::Key, std::weak_ptr<SharedCode>, detail::KeyHash> codes;
  // Keys an LLVM compile is running for: another compile of the same key waits for it and shares its code (64 tracks
  // of one instrument compile once, not once per compile thread).
  std::unordered_set<detail::Key, detail::KeyHash> inflight;
  std::condition_variable inflightDone;

  // The read-write directory's size as this process last saw it (eviction bookkeeping; `storeMutex`).
  std::mutex storeMutex;
  std::filesystem::path sizedDir;
  uint64_t dirBytes = 0;
  uint32_t storesSinceScan = 0;

  // The gate held exclusive: frees one code (its JITDylib) and forgets its key unless a newer code took it.
  void freeCode(SharedCode* c) noexcept {
    {
      std::lock_guard lock(mutex);
      if (auto it = codes.find(c->key); it != codes.end() && it->second.expired()) codes.erase(it);
      ++counters.codesReleased;
    }
    if (poisoned.load()) c->es = nullptr;  // L10: LLVM's locks may be held: its code is leaked, never removed
    delete c;
  }
  // The gate held exclusive: frees what releases parked meanwhile.
  void drainReleased() noexcept {
    std::vector<SharedCode*> list;
    {
      std::lock_guard lock(releaseMutex);
      list.swap(released);
      pendingRelease.store(false);
    }
    for (SharedCode* c : list) freeCode(c);
  }
  // After a compile let go of the gate: frees parked codes if no compile runs now. A release that parks after our
  // drain either finds the gate free and drains itself, or we see its flag here and drain again; a compile still
  // running drains when it leaves.
  void drainIfIdle() noexcept {
    while (pendingRelease.load() && llvmGate.try_lock()) {
      drainReleased();
      llvmGate.unlock();
    }
  }
  // RAII: the gate, shared, for one compile.
  struct Compiling {
    Session& s;
    explicit Compiling(Session& session) : s(session) {
      s.llvmGate.lock_shared();
      s.active.fetch_add(1);
    }
    ~Compiling() {
      s.active.fetch_sub(1);
      s.llvmGate.unlock_shared();
      s.drainIfIdle();
    }
    Compiling(const Compiling&) = delete;
    Compiling& operator=(const Compiling&) = delete;
  };

  // A TargetMachine for one compile, back to the pool when the lease ends.
  struct TmLease {
    Session* s = nullptr;
    std::unique_ptr<llvm::TargetMachine> tm;
    TmLease() = default;
    TmLease(TmLease&& o) noexcept : s(o.s), tm(std::move(o.tm)) {}
    TmLease& operator=(TmLease&&) = delete;
    ~TmLease() {
      if (s == nullptr || tm == nullptr) return;
      std::lock_guard lock(s->tmMutex);
      s->tms.push_back(std::move(tm));
    }
    llvm::TargetMachine* operator->() const noexcept { return tm.get(); }
    llvm::TargetMachine& operator*() const noexcept { return *tm; }
  };
  Result<TmLease> leaseTm() {
    TmLease l;
    l.s = this;
    std::lock_guard lock(tmMutex);
    if (!tms.empty()) {
      l.tm = std::move(tms.back());
      tms.pop_back();
      return l;
    }
    auto tm = jtmb->createTargetMachine();
    if (!tm) return llvmError("creating a target machine", tm.takeError());
    l.tm = std::move(*tm);
    return l;
  }

  ~Session() {
    codes.clear();
    for (SharedCode* c : released) {  // every image is gone (each holds the session): nothing races this
      if (poisoned.load()) c->es = nullptr;
      delete c;
    }
    released.clear();
    if (poisoned.load()) {  // L10: LLJIT's state is not trusted (its locks may be held): leaked, not destroyed
      (void)jit.release();
      for (auto& tm : tms) (void)tm.release();
    }
  }
};

namespace {

// The deleter of a SharedCode (its last owner is gone): frees it if no compile runs right now, else parks it for the
// last compile to leave (Session::drainIfIdle). Never blocks on a compile (M1 review L6).
struct CodeReleaser {
  Compiler::Session* s = nullptr;
  void operator()(SharedCode* c) const noexcept {
    if (s->llvmGate.try_lock()) {
      s->freeCode(c);
      s->drainReleased();
      s->llvmGate.unlock();
      return;
    }
    {
      std::lock_guard lock(s->releaseMutex);
      s->released.push_back(c);
      s->pendingRelease.store(true);
    }
    s->releasesDeferred.fetch_add(1);
    s->drainIfIdle();  // the compiles may have left between our two looks
  }
};

std::shared_ptr<SharedCode> newCode(Compiler::Session& s) {
  return std::shared_ptr<SharedCode>(new SharedCode, CodeReleaser{&s});
}

// Keeps an image's machine code (and the session that releases it) alive. Members are destroyed in reverse order:
// the code first, while the session lives.
struct ImageCode {
  std::shared_ptr<Compiler::Session> session;
  std::shared_ptr<SharedCode> code;
  ImageCode(std::shared_ptr<Compiler::Session> s, std::shared_ptr<SharedCode> c) : session(std::move(s)), code(std::move(c)) {}
  ImageCode(const ImageCode&) = delete;
  ImageCode& operator=(const ImageCode&) = delete;
};

}  // namespace

Compiler::Compiler(std::shared_ptr<Session> s) noexcept : session_(std::move(s)) {}
Compiler::~Compiler() = default;

Result<std::unique_ptr<Compiler>> Compiler::create(const CompilerConfig& config) {
  if (config.codeGenLevel > 3) return Error{ErrorCode::InvalidArgument, "jit: codeGenLevel must be 0..3", {}};
  startBinaryIdentity();  // PF.1: hashed while LLJIT is created and the first unit is lowered
  initialiseLlvmOnce();
  auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
  if (!jtmb) return llvmError("detecting the host", jtmb.takeError());
  if (config.target == Target::Baseline) {
    // x86-64-v3 and what it implies, nothing of the build machine's.
    jtmb->setCPU(kBaselineCpu);
    jtmb->getFeatures() = llvm::SubtargetFeatures();
  } else {
    // Host CPU and every feature it has (AVX2, FMA, and AVX-512 where present).
    jtmb->setCPU(llvm::sys::getHostCPUName().str());
    llvm::SubtargetFeatures features;
    for (const auto& f : llvm::sys::getHostCPUFeatures()) features.AddFeature(f.first(), f.second);
    jtmb->getFeatures() = features;
  }
  static constexpr llvm::CodeGenOptLevel kLevels[] = {llvm::CodeGenOptLevel::None, llvm::CodeGenOptLevel::Less,
                                                      llvm::CodeGenOptLevel::Default, llvm::CodeGenOptLevel::Aggressive};
  jtmb->setCodeGenOptLevel(kLevels[config.codeGenLevel]);
  auto session = std::make_shared<Session>();
  session->cpu = jtmb->getCPU();
  session->features = jtmb->getFeatures().getString();
  session->codeGenLevel = config.codeGenLevel;
  session->baseline = config.target == Target::Baseline;
  session->cache = config.cache;
  session->beforeCompile = config.beforeCompile;
  session->beforeCompileUser = config.beforeCompileUser;
  llvm::orc::LLJITBuilder builder;
  builder.setJITTargetMachineBuilder(std::move(*jtmb)).setSupportConcurrentCompilation(true);
  if (auto err = builder.prepareForConstruction()) return llvmError("configuring LLJIT", std::move(err));
  session->jtmb = *builder.JTMB;  // as LLJIT has it (prepareForConstruction may adjust it)
  auto jit = builder.create();
  if (!jit) return llvmError("creating LLJIT", jit.takeError());
  session->jit = std::move(*jit);
  if (auto tm = session->leaseTm(); !tm) return std::move(tm).error();  // the first TargetMachine: fails here, not later
  return std::unique_ptr<Compiler>(new Compiler(std::move(session)));
}

const std::string& Compiler::hostCpu() const noexcept { return session_->cpu; }
const std::string& Compiler::hostFeatures() const noexcept { return session_->features; }

void Compiler::setCache(const CacheConfig& cache) {
  {
    std::lock_guard lock(session_->mutex);
    session_->cache = cache;
  }
  std::lock_guard lock(session_->storeMutex);
  session_->sizedDir.clear();  // size the (new) directory again at the next store
}

CacheConfig Compiler::cache() const {
  std::lock_guard lock(session_->mutex);
  return session_->cache;
}

CacheCounters Compiler::counters() const {
  std::lock_guard lock(session_->mutex);
  CacheCounters c = session_->counters;
  c.releasesDeferred = session_->releasesDeferred.load();
  return c;
}

namespace {

using detail::Key;

// The generated IR's hash (CacheConfig::verify): the module printed right after code generation, with the function
// names the key gives it. `fnSlots` receives the pointer-table slots code generation added after the host's.
Result<Key> irDigest(const graph::Schedule& schedule, const detail::HostLayout& layout, std::vector<const void*> hostTab,
                     const detail::GenOptions& go, const llvm::TargetMachine& tm, std::vector<const void*>& fnSlots) {
  detail::PtrTable tab;
  for (const void* p : hostTab) tab.slot(p);
  llvm::LLVMContext ctx;
  llvm::Module mod("pq_verify", ctx);
  mod.setDataLayout(tm.createDataLayout());
  mod.setTargetTriple(tm.getTargetTriple());
  auto gen = detail::generate(schedule, layout, tab, mod, go);
  if (!gen) return std::move(gen).error();
  std::string text;
  llvm::raw_string_ostream os(text);
  for (const llvm::Function& f : mod) f.print(os);  // the functions only: the module header names the module
  os.flush();
  fnSlots.assign(tab.ptrs().begin() + static_cast<std::ptrdiff_t>(hostTab.size()), tab.ptrs().end());
  return detail::hashBytes(text.data(), text.size());
}

// PF.4 (survey #7): a module of at least kSplitInstructions generated IR instructions and two functions or more is
// compiled as kSplitParts modules (its functions balanced by IR instructions), each optimised and compiled to its own
// object, all linked into one JITDylib. The split depends on the module only (never on the threads or the load), so
// the code is the same bytes every time. Measured on the 30-track fixture's largest unit (bench/RESULTS.md, PF.4):
// 161 → 113 ms with the parts in parallel; 3 or more parts gained nothing more (its largest function is then the
// critical path) and only added code.
constexpr uint32_t kSplitParts = 2;
constexpr uint32_t kSplitInstructions = 3000;
uint32_t splitParts(const llvm::Module& m) {
  uint32_t defs = 0, instructions = 0;
  for (const llvm::Function& f : m)
    if (!f.isDeclaration()) {
      ++defs;
      instructions += static_cast<uint32_t>(f.getInstructionCount());
    }
  return defs >= 2 && instructions >= kSplitInstructions ? std::min(kSplitParts, defs) : 1;
}

bool insideBinary(const void* p, const detail::BinaryIdentity& bin) noexcept {
  const auto a = reinterpret_cast<uintptr_t>(p);
  return bin.ok && a >= bin.base && a < bin.base + bin.size;
}

}  // namespace

namespace {
VoiceCode resolveVoiceCode(VoiceCode vc, bool avx512) noexcept {
  if (vc == VoiceCode::Auto) vc = avx512 ? VoiceCode::Pairs512 : VoiceCode::Pairs256;
  if (vc == VoiceCode::Pairs512 && !avx512) vc = VoiceCode::Pairs256;
  return vc;
}
}  // namespace

Result<std::unique_ptr<engine::ProgramImage>> Compiler::compile(const graph::Schedule& schedule, const CompileOptions& options,
                                                                CompileStats* stats) {
  if (session_->poisoned.load())
    return Error{ErrorCode::Unsupported,
                 "jit: LLVM ran out of memory in an earlier compile; this session compiles no more (the images that play "
                 "keep playing; save and restart pqdaw to compile again)", {}};
  try {
    return compileGuarded(schedule, options, stats);
  } catch (const std::bad_alloc&) {  // TMM.13-fix1 L10 (onLlvmBadAlloc)
    session_->poisoned.store(true);
    return Error{ErrorCode::Overflow,
                 "jit: out of memory while compiling (LLVM); the previous image keeps playing; save your work and restart "
                 "pqdaw to compile again", {}};
  }
}

void simulateLlvmOutOfMemory() { llvm::report_bad_alloc_error("simulated (tests)"); }

Result<std::unique_ptr<engine::ProgramImage>> Compiler::compileGuarded(const graph::Schedule& schedule,
                                                                       const CompileOptions& options, CompileStats* stats) {
  if (!options.quickFirst) return compileImpl(schedule, options, stats, false, true);
  CompileOptions o = options;
  o.quickFirst = false;
  const VoiceCode vc = resolveVoiceCode(o.voiceCode, session_->features.find("+avx512f") != std::string::npos);
  if (vc != VoiceCode::Pairs256 && vc != VoiceCode::Pairs512) return compileImpl(schedule, o, stats, false, true);
  // Q111 C: the final image if the cache has it (memory, disk, AOT seed: about a millisecond) …
  auto cached = compileImpl(schedule, o, stats, true, true);
  if (cached.ok() || cached.error().code != ErrorCode::NotFound) return cached;
  // … else voice groups only, when pairs would add anything (else the final image is no slower to compile).
  if (!pairsPlanned(schedule, o)) return compileImpl(schedule, o, stats, false, true);
  CompileOptions q = o;
  q.voiceCode = VoiceCode::Groups;
  auto first = compileImpl(schedule, q, stats, false, false);
  if (first.ok() && stats != nullptr) stats->refine = true;
  return first;
}

bool Compiler::pairsPlanned(const graph::Schedule& schedule, const CompileOptions& options) const {
  detail::PtrTable tab;
  auto host = detail::buildHost(schedule, options.maxBlockFrames, options.maxVoiceGroups, options.resources, true, tab,
                                options.sleep);
  if (!host) return false;
  detail::GenOptions go;
  go.cpu = session_->cpu;
  go.features = session_->features;
  go.pairs = true;
  for (uint8_t p : detail::pairPlan(schedule, (*host)->layout_, go))
    if (p) return true;
  return false;
}

Result<std::unique_ptr<engine::ProgramImage>> Compiler::compileImpl(const graph::Schedule& schedule, const CompileOptions& options,
                                                                    CompileStats* stats, bool cacheOnly, bool storeEntry) {
  using clock = std::chrono::steady_clock;
  using Source = CompileStats::Source;
  const auto t0 = clock::now();
  CompileStats st;
  if (options.optLevel > 3) return Error{ErrorCode::InvalidArgument, "jit: optLevel must be 0..3", {}};
  // Voice code (T1b.2): pairs where possible; 512-bit pairs only where the target has AVX-512F.
  auto resolve = [&](bool avx512) { return resolveVoiceCode(options.voiceCode, avx512); };
  const VoiceCode vc = resolve(session_->features.find("+avx512f") != std::string::npos);
  const VoiceCode baselineVc = resolve(false);  // what a Target::Baseline compiler makes of the same options
  const bool pairs = vc == VoiceCode::Pairs256 || vc == VoiceCode::Pairs512;
  detail::PtrTable tab;
  auto host = detail::buildHost(schedule, options.maxBlockFrames, options.maxVoiceGroups, options.resources, pairs, tab,
                                options.sleep);
  if (!host) return std::move(host).error();
  std::unique_ptr<detail::JitImage> image = std::move(host.value());
  st.hostMs = msSince(t0);
  const auto hostSlots = static_cast<uint32_t>(tab.ptrs().size());

  Session& s = *session_;
  Session::Compiling compiling(s);  // PF.4: shared with the other compiles; code is released only between them
  CacheConfig cc;
  {
    std::lock_guard lock(s.mutex);
    cc = s.cache;
  }
  auto tc = clock::now();
  const detail::BinaryIdentity& bin = detail::binaryIdentity();
  const Key digest = detail::codegenDigest(schedule, image->layout_, hostSlots);
  detail::KeyOptions ko{s.cpu, s.features, s.codeGenLevel, options.optLevel, options.contract, static_cast<uint8_t>(vc)};
  const Key key = detail::cacheKey(digest, ko, bin.hash);
  const uint64_t id = s.nextId.fetch_add(1);  // the image's number in this process (PQ_JIT_DUMP)
  auto count = [&](uint64_t CacheCounters::*field) {
    std::lock_guard lock(s.mutex);
    ++(s.counters.*field);
  };

  detail::GenOptions go;
  go.contract = options.contract;
  go.cpu = s.cpu;
  go.features = s.features;
  go.prefix = "pq_" + key.hex();
  go.scalarLanes = vc == VoiceCode::ScalarLanes;
  go.pairs = pairs;
  go.pairsZmm = vc == VoiceCode::Pairs512;

  // Hands `code` to the image (its stage and pair functions, its pointer table: the host's slots + the code's).
  auto attach = [&](const std::shared_ptr<SharedCode>& code, Source source) -> Result<std::unique_ptr<engine::ProgramImage>> {
    std::vector<const void*>& t = tab.ptrs();
    t.insert(t.end(), code->fnSlots.begin(), code->fnSlots.end());
    image->fns_ = code->fns;
    image->pairFns_ = code->pairFns;
    image->scalarLanes_ = go.scalarLanes;
    detail::finishPairs(*image, schedule);
    image->host_.setMeterUnits(detail::meterUnits(schedule, image->layout_, go));  // T4.5
    image->tab_ = std::move(t);
    image->codeOwner_ = std::make_shared<ImageCode>(session_, code);
    st.source = source;
    st.objectBytes = code->objectBytes;
    st.objects = code->objects;
    st.stackProbes = code->stackProbes;
    st.irInstructions = code->irInstructions;
    st.inlinedOps = code->inlinedOps;
    st.calledOps = code->calledOps;
    st.pairedStages = code->pairedStages;
    if (source != Source::Compiled) st.cacheMs = msSince(tc);
    st.totalMs = msSince(t0);
    if (stats) *stats = st;
    return std::unique_ptr<engine::ProgramImage>(std::move(image));
  };
  // CacheConfig::verify: the IR this schedule generates now must be the IR the code was compiled from (for the target
  // and with the function names of the key it was stored under: a baseline entry is baseline IR).
  auto verified = [&](const SharedCode& code) {
    if (!cc.verify || code.irHash == Key{}) return true;
    count(&CacheCounters::verified);
    detail::GenOptions vo = go;
    vo.prefix = "pq_" + code.irKey.hex();
    if (code.baselineIr) {
      vo.cpu = kBaselineCpu;
      vo.features.clear();
      vo.scalarLanes = baselineVc == VoiceCode::ScalarLanes;
      vo.pairs = baselineVc == VoiceCode::Pairs256 || baselineVc == VoiceCode::Pairs512;
      vo.pairsZmm = false;
    }
    std::vector<const void*> slots;
    auto tm = s.leaseTm();
    if (!tm) return false;
    auto h = irDigest(schedule, image->layout_, tab.ptrs(), vo, **tm, slots);
    if (h.ok() && *h == code.irHash && slots == code.fnSlots) return true;
    count(&CacheCounters::verifyMismatches);
    return false;
  };

  // 1. A live image with this key: share its code. A compile of this key in flight: wait for it, then share (PF.4).
  // `owner`: this compile is the key's one LLVM compile (cleared, and the waiters woken, however it ends).
  bool owner = false;
  std::shared_ptr<SharedCode> liveCode;
  {
    std::unique_lock lock(s.mutex);
    for (;;) {
      if (auto it = s.codes.find(key); it != s.codes.end()) liveCode = it->second.lock();
      if (liveCode || cacheOnly || !s.inflight.contains(key)) break;
      s.inflightDone.wait(lock);
    }
    if (!liveCode && !cacheOnly) owner = s.inflight.insert(key).second;
  }
  struct Inflight {
    Session& s;
    const Key& key;
    bool owner;
    ~Inflight() {
      if (!owner) return;
      {
        std::lock_guard lock(s.mutex);
        s.inflight.erase(key);
      }
      s.inflightDone.notify_all();
    }
  } inflightGuard{s, key, owner};
  if (liveCode && verified(*liveCode)) {
    count(&CacheCounters::memoryHits);
    return attach(liveCode, Source::Memory);
  }
  const bool liveUnverified = liveCode != nullptr;  // (verify, tests) a live code that failed: compile, keep it registered
  liveCode.reset();  // (outside s.mutex: dropping the last reference releases the code)

  // 2. Disk: the read-write directory, then the read-only ones (also under the baseline target's key).
  if (bin.ok && !liveUnverified) {
    std::error_code fsError;  // a failed delete of a bad entry is not an error of the compile
    auto tryDir = [&](const std::filesystem::path& dir, const Key& k, bool writable) -> std::shared_ptr<SharedCode> {
      detail::Entry e;
      const detail::ReadResult r = detail::readEntry(dir, k, writable, e);
      if (r == detail::ReadResult::Miss) return nullptr;
      bool ok = r == detail::ReadResult::Hit && e.hostSlots == hostSlots && e.stageFunctions.size() == schedule.stages.size() &&
                e.pairFunctions.size() == schedule.stages.size();
      for (uint64_t off : e.fnOffsets) ok = ok && off < bin.size;
      if (!ok) {  // did not validate, or validated but does not describe this schedule (never expected)
        count(&CacheCounters::rejected);
        if (writable && r == detail::ReadResult::Hit) std::filesystem::remove(detail::entryPath(dir, k), fsError);
        return nullptr;
      }
      auto code = newCode(s);
      code->key = key;
      code->hostSlots = hostSlots;
      for (uint64_t off : e.fnOffsets) code->fnSlots.push_back(reinterpret_cast<const void*>(bin.base + off));
      code->irHash = e.irHash;
      code->irKey = k;
      code->baselineIr = k != key || s.baseline;
      if (!verified(*code)) return nullptr;
      bool linked = true;
      if (!e.objects.empty()) {  // (a schedule without stages has no code)
        auto jd = s.jit->createJITDylib("pq_code" + std::to_string(s.nextId.fetch_add(1)));
        if (!jd) {
          llvm::consumeError(jd.takeError());
          return nullptr;
        }
        code->es = &s.jit->getExecutionSession();
        code->jd = &*jd;
        for (const std::vector<uint8_t>& o : e.objects) {
          llvm::Error err = s.jit->addObjectFile(
              *code->jd, llvm::MemoryBuffer::getMemBufferCopy(
                             llvm::StringRef(reinterpret_cast<const char*>(o.data()), o.size()), "pq_cached"));
          linked = linked && !err;
          llvm::consumeError(std::move(err));
        }
      }
      auto lookUp = [&](const std::string& name, detail::StageFn& fn) {
        auto sym = s.jit->lookup(*code->jd, name);  // links the object now: no codegen
        if (!sym) {
          llvm::consumeError(sym.takeError());
          return false;
        }
        fn = sym->toPtr<detail::StageFn>();
        return true;
      };
      code->fns.assign(e.stageFunctions.size(), nullptr);
      code->pairFns.assign(e.stageFunctions.size(), nullptr);
      for (size_t k2 = 0; linked && k2 < e.stageFunctions.size(); ++k2) {
        linked = lookUp(e.stageFunctions[k2], code->fns[k2]);
        if (linked && !e.pairFunctions[k2].empty()) {
          linked = lookUp(e.pairFunctions[k2], code->pairFns[k2]);
          ++code->pairedStages;
        }
      }
      if (!linked) {
        count(&CacheCounters::rejected);
        if (writable) std::filesystem::remove(detail::entryPath(dir, k), fsError);
        return nullptr;  // ~SharedCode removes the JITDylib
      }
      code->objectBytes = e.objectBytes;
      code->objects = static_cast<uint32_t>(e.objects.size());
      code->stackProbes = e.stackProbes;
      code->irInstructions = e.irInstructions;
      code->inlinedOps = e.inlinedOps;
      code->calledOps = e.calledOps;
      std::lock_guard lock(s.mutex);
      s.codes[key] = code;
      return code;
    };
    if (!cc.dir.empty())
      if (auto code = tryDir(cc.dir, key, true)) {
        count(&CacheCounters::diskHits);
        return attach(code, Source::Disk);
      }
    if (!cc.readOnly.empty()) {
      Key baselineKey = key;
      // A baseline entry fits only where the voice code it was built with gives the same host layout and image.
      if (!s.baseline && (baselineVc == VoiceCode::Pairs256 || baselineVc == VoiceCode::Pairs512) == pairs) {
        detail::KeyOptions kb = ko;
        kb.cpu = kBaselineCpu;
        kb.features = {};
        kb.voiceCode = static_cast<uint8_t>(baselineVc);
        baselineKey = detail::cacheKey(digest, kb, bin.hash);
      }
      for (const std::filesystem::path& dir : cc.readOnly)
        for (const Key& k : {key, baselineKey}) {
          if (auto code = tryDir(dir, k, false)) {
            count(&CacheCounters::aotHits);
            return attach(code, Source::Aot);
          }
          if (k == baselineKey) break;
        }
    }
  }
  st.cacheMs = msSince(tc);
  if (cacheOnly) return Error{ErrorCode::NotFound, "jit: not in the cache", {}};

  // 3. Compile, on this thread, with a TargetMachine of its own.
  if (s.beforeCompile != nullptr) s.beforeCompile(s.beforeCompileUser);
  count(&CacheCounters::compiles);
  auto tm = s.leaseTm();
  if (!tm) return std::move(tm).error();
  auto ctx = std::make_unique<llvm::LLVMContext>();
  auto mod = std::make_unique<llvm::Module>(go.prefix, *ctx);
  mod->setDataLayout((*tm)->createDataLayout());
  mod->setTargetTriple((*tm)->getTargetTriple());

  auto t1 = clock::now();
  auto gen = detail::generate(schedule, image->layout_, tab, *mod, go);
  if (!gen) return std::move(gen).error();
  st.codegenMs = msSince(t1);
  st.inlinedOps = gen->inlinedOps;
  st.calledOps = gen->calledOps;
  Key irHash;
  if (cc.verify) {
    std::string text;
    llvm::raw_string_ostream os(text);
    for (const llvm::Function& f : *mod) f.print(os);
    os.flush();
    irHash = detail::hashBytes(text.data(), text.size());
  }

  // The light pipeline (or O0/O2/O3) on one module, with that TargetMachine's target information.
  auto optimise = [&](llvm::Module& m, llvm::TargetMachine& machine) {
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassBuilder pb(&machine);
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    llvm::ModulePassManager mpm =
        options.optLevel == 0   ? pb.buildO0DefaultPipeline(llvm::OptimizationLevel::O0)
        : options.optLevel == 1 ? lightPipeline()
                                : pb.buildPerModuleDefaultPipeline(options.optLevel == 2 ? llvm::OptimizationLevel::O2
                                                                                          : llvm::OptimizationLevel::O3);
    mpm.run(m, mam);
  };
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> objects;
  const uint32_t parts = splitParts(*mod);
  if (parts <= 1) {
    t1 = clock::now();
    optimise(*mod, **tm);
    st.optimiseMs = msSince(t1);
    // Developer aid (compiler.h): PQ_JIT_DUMP=<path prefix> writes each image's optimised IR and its assembly.
    if (const char* dump = std::getenv("PQ_JIT_DUMP"); dump != nullptr && *dump != '\0') {
      std::error_code ec;
      llvm::raw_fd_ostream os(std::string(dump) + std::to_string(id) + ".ll", ec);
      if (!ec) mod->print(os, nullptr);
      llvm::raw_fd_ostream as(std::string(dump) + std::to_string(id) + ".s", ec);
      if (!ec) {
        llvm::legacy::PassManager pm;
        auto clone = llvm::CloneModule(*mod);
        if (!(*tm)->addPassesToEmitFile(pm, as, nullptr, llvm::CodeGenFileType::AssemblyFile)) pm.run(*clone);
      }
    }
    for (const llvm::Function& f : *mod) st.irInstructions += static_cast<uint32_t>(f.getInstructionCount());
    t1 = clock::now();
    if (!gen->stageFunctions.empty()) {
      auto obj = llvm::orc::SimpleCompiler(**tm)(*mod);
      if (!obj) return llvmError("compiling the module", obj.takeError());
      objects.push_back(std::move(*obj));
    }
    st.machineCodeMs = msSince(t1);
  } else {
    // PF.4 (splitParts): the functions in `parts` modules (largest first, each to the lightest part), handed to fresh
    // contexts as bitcode. The parts run on helper threads only while this is the session's only compile (an edit's:
    // its latency is what the split is for); with other compiles running every core is busy already, so they run
    // here, one after the other (the same objects either way).
    t1 = clock::now();
    std::vector<llvm::Function*> defs;
    for (llvm::Function& f : *mod)
      if (!f.isDeclaration()) defs.push_back(&f);
    std::vector<size_t> order(defs.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = k;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return defs[a]->getInstructionCount() > defs[b]->getInstructionCount(); });
    std::vector<uint64_t> load(parts, 0);
    std::unordered_map<const llvm::GlobalValue*, uint32_t> partOf;
    for (size_t k : order) {
      const auto p = static_cast<uint32_t>(std::min_element(load.begin(), load.end()) - load.begin());
      load[p] += defs[k]->getInstructionCount() + 1;
      partOf[defs[k]] = p;
      if (defs[k]->hasLocalLinkage()) defs[k]->setLinkage(llvm::GlobalValue::ExternalLinkage);  // called across parts
    }
    std::vector<llvm::SmallVector<char, 0>> bitcode(parts);
    for (uint32_t p = 0; p < parts; ++p) {
      llvm::ValueToValueMapTy vmap;
      auto part = llvm::CloneModule(*mod, vmap, [&](const llvm::GlobalValue* gv) {
        const auto it = partOf.find(gv);
        return it != partOf.end() && it->second == p;
      });
      llvm::raw_svector_ostream os(bitcode[p]);
      llvm::WriteBitcodeToFile(*part, os);
    }
    mod.reset();
    ctx.reset();
    const double splitMs = msSince(t1);
    struct Part {
      std::unique_ptr<llvm::MemoryBuffer> object;
      uint32_t irInstructions = 0;
      double optimiseMs = 0, machineCodeMs = 0;
      std::string error;
      std::exception_ptr thrown;
    };
    std::vector<Part> out(parts);
    const int priority = GetThreadPriority(GetCurrentThread());
    auto work = [&](uint32_t p) {
      Part& r = out[p];
      try {
        auto machine = s.leaseTm();
        if (!machine) {
          r.error = machine.error().message;
          return;
        }
        llvm::LLVMContext c;
        auto m = llvm::parseBitcodeFile(llvm::MemoryBufferRef(llvm::StringRef(bitcode[p].data(), bitcode[p].size()), "pq_part"), c);
        if (!m) {
          r.error = llvm::toString(m.takeError());
          return;
        }
        auto t = clock::now();
        optimise(**m, **machine);
        r.optimiseMs = msSince(t);
        if (const char* dump = std::getenv("PQ_JIT_DUMP"); dump != nullptr && *dump != '\0') {  // per part
          const std::string base = std::string(dump) + std::to_string(id) + "_part" + std::to_string(p);
          std::error_code ec;
          llvm::raw_fd_ostream os(base + ".ll", ec);
          if (!ec) (*m)->print(os, nullptr);
          llvm::raw_fd_ostream as(base + ".s", ec);
          if (!ec) {
            llvm::legacy::PassManager pm;
            auto clone = llvm::CloneModule(**m);
            if (!(*machine)->addPassesToEmitFile(pm, as, nullptr, llvm::CodeGenFileType::AssemblyFile)) pm.run(*clone);
          }
        }
        for (const llvm::Function& f : **m) r.irInstructions += static_cast<uint32_t>(f.getInstructionCount());
        t = clock::now();
        auto obj = llvm::orc::SimpleCompiler(**machine)(**m);
        if (!obj) {
          r.error = llvm::toString(obj.takeError());
          return;
        }
        r.object = std::move(*obj);
        r.machineCodeMs = msSince(t);
      } catch (...) {
        r.thrown = std::current_exception();
      }
    };
    const bool parallel = s.active.load() == 1;
    std::vector<std::thread> helpers;
    std::vector<uint32_t> here = {0};  // the parts this thread compiles
    for (uint32_t p = 1; p < parts; ++p) {
      bool started = false;
      if (parallel) try {
          helpers.emplace_back([&, p] {
            SetThreadPriority(GetCurrentThread(), priority);
            work(p);
          });
          started = true;
        } catch (const std::system_error&) {  // no thread: compiled here
        }
      if (!started) here.push_back(p);
    }
    for (uint32_t p : here) work(p);
    for (std::thread& h : helpers) h.join();
    for (Part& r : out) {
      if (r.thrown) std::rethrow_exception(r.thrown);
      if (!r.error.empty()) return Error{ErrorCode::Unknown, "jit: compiling a part: " + r.error, {}};
      st.irInstructions += r.irInstructions;
      st.optimiseMs = parallel ? std::max(st.optimiseMs, r.optimiseMs) : st.optimiseMs + r.optimiseMs;
      st.machineCodeMs = parallel ? std::max(st.machineCodeMs, r.machineCodeMs) : st.machineCodeMs + r.machineCodeMs;
      objects.push_back(std::move(r.object));
    }
    st.codegenMs += splitMs;
  }

  // The code may be shared and stored only if every pointer-table slot it added is inside the executable (functions,
  // static data): those are the same in every image of this key and at the same offset in every run of this binary.
  auto code = newCode(s);
  code->key = key;
  code->hostSlots = hostSlots;
  code->fnSlots.assign(tab.ptrs().begin() + hostSlots, tab.ptrs().end());
  code->irHash = irHash;
  code->irKey = key;
  code->baselineIr = s.baseline;
  bool cacheable = bin.ok;
  for (const void* p : code->fnSlots) cacheable = cacheable && insideBinary(p, bin);
  if (!cacheable) count(&CacheCounters::uncacheable);
  const bool store = cacheable && !cc.dir.empty() && storeEntry;

  // Link the object(s) into the code's JITDylib.
  t1 = clock::now();
  std::vector<std::vector<uint8_t>> stored;  // a copy for the disk cache
  mod.reset();  // the IR and its context go before the link
  ctx.reset();
  if (!objects.empty()) {
    auto jd = s.jit->createJITDylib("pq_code" + std::to_string(s.nextId.fetch_add(1)));
    if (!jd) return llvmError("creating a JITDylib", jd.takeError());
    code->es = &s.jit->getExecutionSession();
    code->jd = &*jd;  // from here `code` owns the JITDylib: released if anything below fails
  }
  code->objects = static_cast<uint32_t>(objects.size());
  for (std::unique_ptr<llvm::MemoryBuffer>& obj : objects) {
    code->objectBytes += obj->getBufferSize();
    auto file = llvm::object::ObjectFile::createObjectFile(obj->getMemBufferRef());
    if (!file) return llvmError("reading the object", file.takeError());
    for (const llvm::object::SymbolRef& sym : (*file)->symbols()) {
      auto name = sym.getName();
      if (!name) return llvmError("reading the object", name.takeError());
      if (*name == "__chkstk" || *name == "__chkstk_ms" || *name == "___chkstk_ms") code->stackProbes = true;
    }
    if (store) {
      const auto* b = reinterpret_cast<const uint8_t*>(obj->getBufferStart());
      stored.emplace_back(b, b + obj->getBufferSize());
    }
    if (auto err = s.jit->addObjectFile(*code->jd, std::move(obj))) return llvmError("adding the object", std::move(err));
  }
  code->fns.assign(gen->stageFunctions.size(), nullptr);
  code->pairFns.assign(gen->stageFunctions.size(), nullptr);
  for (size_t k = 0; k < gen->stageFunctions.size(); ++k) {
    auto sym = s.jit->lookup(*code->jd, gen->stageFunctions[k]);  // links the object now, not on the audio thread
    if (!sym) return llvmError("looking up a stage function", sym.takeError());
    code->fns[k] = sym->toPtr<detail::StageFn>();
    if (gen->pairFunctions[k].empty()) continue;
    auto pair = s.jit->lookup(*code->jd, gen->pairFunctions[k]);
    if (!pair) return llvmError("looking up a pair function", pair.takeError());
    code->pairFns[k] = pair->toPtr<detail::StageFn>();
    ++code->pairedStages;
  }
  st.machineCodeMs += msSince(t1);
  st.objectBytes = code->objectBytes;
  st.stackProbes = code->stackProbes;
  code->irInstructions = st.irInstructions;
  code->inlinedOps = st.inlinedOps;
  code->calledOps = st.calledOps;

  tc = clock::now();
  if (store) {
    detail::Entry e;
    e.stageFunctions = gen->stageFunctions;
    e.pairFunctions = gen->pairFunctions;
    e.hostSlots = hostSlots;
    for (const void* p : code->fnSlots) e.fnOffsets.push_back(reinterpret_cast<uintptr_t>(p) - bin.base);
    e.irHash = irHash;
    e.objectBytes = code->objectBytes;
    e.irInstructions = code->irInstructions;
    e.inlinedOps = code->inlinedOps;
    e.calledOps = code->calledOps;
    e.stackProbes = code->stackProbes;
    e.objects = std::move(stored);
    const uint64_t written = detail::writeEntry(cc.dir, key, e);
    std::lock_guard storeLock(s.storeMutex);
    const bool sized = s.sizedDir == cc.dir;
    if (!sized) {  // (a first store, or another directory: its size now, this entry included)
      s.sizedDir = cc.dir;
      s.dirBytes = detail::directoryBytes(cc.dir);
      s.storesSinceScan = 0;
    }
    if (written > 0) {
      count(&CacheCounters::stores);
      if (sized) s.dirBytes += written;
      // Other processes write here too: look at the real size now and then, and whenever ours says it is over.
      if (s.dirBytes > cc.maxBytes || ++s.storesSinceScan >= 64) {
        const detail::EvictResult r = detail::evict(cc.dir, cc.maxBytes);
        s.dirBytes = r.bytes;
        s.storesSinceScan = 0;
        std::lock_guard lock(s.mutex);
        s.counters.evictedFiles += r.evictedFiles;
        s.counters.evictedBytes += r.evictedBytes;
      }
    } else {
      count(&CacheCounters::storeErrors);
    }
  }
  if (cacheable && !liveUnverified) {  // (a verify mismatch leaves the live one registered)
    std::lock_guard lock(s.mutex);
    s.codes[key] = code;
  }
  st.cacheMs += msSince(tc);
  return attach(code, Source::Compiled);
}

}  // namespace pq::jit
