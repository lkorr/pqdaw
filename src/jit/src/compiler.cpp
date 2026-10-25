// Compiler: LLJIT for the target CPU, the IR pipeline, image assembly and the compile cache (pq/jit/compiler.h,
// pq/jit/cache.h).

#include "pq/jit/compiler.h"

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
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Support/FileSystem.h>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

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

void initialiseLlvmOnce() {
  static std::once_flag once;
  std::call_once(once, [] {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
  });
}

// optLevel 1 (the default): the few IR passes generated code needs, since codegen.h already emits SSA values,
// explicit vectors and hoisted invariants: SROA (the Newton unknowns' allocas), CSE, and LICM after loop rotation,
// which keeps per-sample state (z1, phasor …) in registers across a loop. Measured against LLVM's O2/O3 pipelines
// (bench/RESULTS.md): the same run time within noise at 3–5× less compile time. InstCombine is left out on purpose:
// with it the interleaved fused loops ran slower (chain32_fast 4.5 vs 3.8 ns/sample).
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
  llvm::ModulePassManager mpm;
  mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
  return mpm;
}

// One image's machine code: its own JITDylib (so two copies of one object, e.g. a disk entry loaded while a fresh
// compile of the same key is alive, never collide on symbol names), the stage functions, and the pointer-table slots
// after the host's. Shared by every live image with the same key; removed (under the session lock) with the last.
struct SharedCode {
  detail::Key key;
  llvm::orc::ExecutionSession* es = nullptr;
  llvm::orc::JITDylib* jd = nullptr;
  bool shared = false;  // registered in Session::codes
  std::vector<detail::StageFn> fns;
  std::vector<detail::StageFn> pairFns;  // per stage: the two-group function or null (T1b.2)
  uint32_t pairedStages = 0;
  std::vector<const void*> fnSlots;  // pointer-table slots hostSlots…: functions (and data) inside the executable
  uint32_t hostSlots = 0;
  detail::Key irHash;                // recorded when compiled with CacheConfig::verify (else zero) …
  detail::Key irKey;                 // … from IR whose functions are named after this key
  bool baselineIr = false;           // … for Target::Baseline
  uint64_t objectBytes = 0;
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

struct Compiler::Session {
  std::unique_ptr<llvm::orc::LLJIT> jit;
  std::unique_ptr<llvm::TargetMachine> tm;  // the IR pipeline's target info (same CPU/features as the JIT's)
  std::string cpu, features;
  uint32_t codeGenLevel = CompilerConfig::kDefaultCodeGenLevel;
  bool baseline = false;
  mutable std::recursive_mutex mutex;  // compiles are serialised; images release their code under it
  uint64_t nextId = 0;
  // Filled by the object transform while a compile materialises its module or a cached object is linked (`mutex`).
  uint64_t objectBytes = 0;
  bool stackProbes = false;
  bool capture = false;                         // keep a copy of the next objects (for the disk cache)
  std::vector<std::vector<uint8_t>> captured;
  CacheConfig cache;
  CacheCounters counters;
  // The read-write directory's size as this process last saw it (eviction bookkeeping).
  std::filesystem::path sizedDir;
  uint64_t dirBytes = 0;
  uint32_t storesSinceScan = 0;
  // Live code by key. Declared after `jit`, so it is destroyed first (its JITDylibs are removed while LLJIT lives).
  std::unordered_map<detail::Key, std::shared_ptr<SharedCode>, detail::KeyHash> codes;
  ~Session() { codes.clear(); }
};

namespace {

// Keeps an image's machine code alive; the last image of a code removes it (and its registration).
struct ImageCode {
  std::shared_ptr<Compiler::Session> session;
  std::shared_ptr<SharedCode> code;
  ImageCode(std::shared_ptr<Compiler::Session> s, std::shared_ptr<SharedCode> c) : session(std::move(s)), code(std::move(c)) {}
  ~ImageCode() {
    std::lock_guard<std::recursive_mutex> lock(session->mutex);
    // Every copy of a SharedCode pointer is made or dropped under the lock, so use_count is exact here.
    if (code->shared && code.use_count() == 2) {
      session->codes.erase(code->key);
      code->shared = false;
    }
    code.reset();
  }
  ImageCode(const ImageCode&) = delete;
  ImageCode& operator=(const ImageCode&) = delete;
};

}  // namespace

Compiler::Compiler(std::shared_ptr<Session> s) noexcept : session_(std::move(s)) {}
Compiler::~Compiler() = default;

Result<std::unique_ptr<Compiler>> Compiler::create(const CompilerConfig& config) {
  if (config.codeGenLevel > 3) return Error{ErrorCode::InvalidArgument, "jit: codeGenLevel must be 0..3", {}};
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
  auto tm = jtmb->createTargetMachine();
  if (!tm) return llvmError("creating the target machine", tm.takeError());
  session->tm = std::move(*tm);
  auto jit = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
  if (!jit) return llvmError("creating LLJIT", jit.takeError());
  session->jit = std::move(*jit);
  // Every object on its way to the linker: its size, whether it calls the stack probe (a frame > 4 KiB), and a copy
  // for the disk cache while a compile asks for one.
  Session* sp = session.get();
  session->jit->getObjTransformLayer().setTransform(
      [sp](std::unique_ptr<llvm::MemoryBuffer> obj) -> llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> {
        sp->objectBytes += obj->getBufferSize();
        auto file = llvm::object::ObjectFile::createObjectFile(obj->getMemBufferRef());
        if (!file) return file.takeError();
        for (const llvm::object::SymbolRef& sym : (*file)->symbols()) {
          auto name = sym.getName();
          if (!name) return name.takeError();
          if (*name == "__chkstk" || *name == "__chkstk_ms" || *name == "___chkstk_ms") sp->stackProbes = true;
        }
        if (sp->capture) {
          const auto* b = reinterpret_cast<const uint8_t*>(obj->getBufferStart());
          sp->captured.emplace_back(b, b + obj->getBufferSize());
        }
        return std::move(obj);
      });
  return std::unique_ptr<Compiler>(new Compiler(std::move(session)));
}

const std::string& Compiler::hostCpu() const noexcept { return session_->cpu; }
const std::string& Compiler::hostFeatures() const noexcept { return session_->features; }

void Compiler::setCache(const CacheConfig& cache) {
  std::lock_guard<std::recursive_mutex> lock(session_->mutex);
  session_->cache = cache;
  session_->sizedDir.clear();  // size the (new) directory again at the next store
}

CacheConfig Compiler::cache() const {
  std::lock_guard<std::recursive_mutex> lock(session_->mutex);
  return session_->cache;
}

CacheCounters Compiler::counters() const {
  std::lock_guard<std::recursive_mutex> lock(session_->mutex);
  return session_->counters;
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

bool insideBinary(const void* p, const detail::BinaryIdentity& bin) noexcept {
  const auto a = reinterpret_cast<uintptr_t>(p);
  return bin.ok && a >= bin.base && a < bin.base + bin.size;
}

}  // namespace

Result<std::unique_ptr<engine::ProgramImage>> Compiler::compile(const graph::Schedule& schedule, const CompileOptions& options,
                                                                CompileStats* stats) {
  using clock = std::chrono::steady_clock;
  using Source = CompileStats::Source;
  const auto t0 = clock::now();
  CompileStats st;
  if (options.optLevel > 3) return Error{ErrorCode::InvalidArgument, "jit: optLevel must be 0..3", {}};
  // Voice code (T1b.2): pairs where possible; 512-bit pairs only where the target has AVX-512F.
  auto resolve = [&](bool avx512) {
    VoiceCode vc = options.voiceCode;
    if (vc == VoiceCode::Auto) vc = avx512 ? VoiceCode::Pairs512 : VoiceCode::Pairs256;
    if (vc == VoiceCode::Pairs512 && !avx512) vc = VoiceCode::Pairs256;
    return vc;
  };
  const VoiceCode vc = resolve(session_->features.find("+avx512f") != std::string::npos);
  const VoiceCode baselineVc = resolve(false);  // what a Target::Baseline compiler makes of the same options
  const bool pairs = vc == VoiceCode::Pairs256 || vc == VoiceCode::Pairs512;
  detail::PtrTable tab;
  auto host = detail::buildHost(schedule, options.maxBlockFrames, options.maxVoiceGroups, options.resources, pairs, tab);
  if (!host) return std::move(host).error();
  std::unique_ptr<detail::JitImage> image = std::move(host.value());
  st.hostMs = msSince(t0);
  const auto hostSlots = static_cast<uint32_t>(tab.ptrs().size());

  std::lock_guard<std::recursive_mutex> lock(session_->mutex);
  Session& s = *session_;
  const CacheConfig& cc = s.cache;
  auto tc = clock::now();
  const detail::BinaryIdentity& bin = detail::binaryIdentity();
  const Key digest = detail::codegenDigest(schedule, image->layout_, hostSlots);
  detail::KeyOptions ko{s.cpu, s.features, s.codeGenLevel, options.optLevel, options.contract, static_cast<uint8_t>(vc)};
  const Key key = detail::cacheKey(digest, ko, bin.hash);
  const uint64_t id = s.nextId++;  // the image's number in this process (PQ_JIT_DUMP)

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
    image->tab_ = std::move(t);
    image->codeOwner_ = std::make_shared<ImageCode>(session_, code);
    st.source = source;
    st.objectBytes = code->objectBytes;
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
    ++s.counters.verified;
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
    auto h = irDigest(schedule, image->layout_, tab.ptrs(), vo, *s.tm, slots);
    if (h.ok() && *h == code.irHash && slots == code.fnSlots) return true;
    ++s.counters.verifyMismatches;
    return false;
  };

  // 1. A live image with this key: share its code.
  if (auto it = s.codes.find(key); it != s.codes.end() && verified(*it->second)) {
    ++s.counters.memoryHits;
    return attach(it->second, Source::Memory);
  }

  // 2. Disk: the read-write directory, then the read-only ones (also under the baseline target's key).
  if (bin.ok && s.codes.find(key) == s.codes.end()) {
    std::error_code fsError;  // a failed delete of a bad entry is not an error of the compile
    auto tryDir = [&](const std::filesystem::path& dir, const Key& k, bool writable) -> std::shared_ptr<SharedCode> {
      detail::Entry e;
      const detail::ReadResult r = detail::readEntry(dir, k, writable, e);
      if (r == detail::ReadResult::Miss) return nullptr;
      bool ok = r == detail::ReadResult::Hit && e.hostSlots == hostSlots && e.stageFunctions.size() == schedule.stages.size() &&
                e.pairFunctions.size() == schedule.stages.size();
      for (uint64_t off : e.fnOffsets) ok = ok && off < bin.size;
      if (!ok) {  // did not validate, or validated but does not describe this schedule (never expected)
        ++s.counters.rejected;
        if (writable && r == detail::ReadResult::Hit) std::filesystem::remove(detail::entryPath(dir, k), fsError);
        return nullptr;
      }
      auto code = std::make_shared<SharedCode>();
      code->key = key;
      code->hostSlots = hostSlots;
      for (uint64_t off : e.fnOffsets) code->fnSlots.push_back(reinterpret_cast<const void*>(bin.base + off));
      code->irHash = e.irHash;
      code->irKey = k;
      code->baselineIr = k != key || s.baseline;
      if (!verified(*code)) return nullptr;
      bool linked = true;
      if (!e.object.empty()) {  // (a schedule without stages has no code)
        auto jd = s.jit->createJITDylib("pq_code" + std::to_string(s.nextId++));
        if (!jd) {
          llvm::consumeError(jd.takeError());
          return nullptr;
        }
        code->es = &s.jit->getExecutionSession();
        code->jd = &*jd;
        llvm::Error err = s.jit->addObjectFile(
            *code->jd, llvm::MemoryBuffer::getMemBufferCopy(
                           llvm::StringRef(reinterpret_cast<const char*>(e.object.data()), e.object.size()), "pq_cached"));
        linked = !err;
        llvm::consumeError(std::move(err));
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
        ++s.counters.rejected;
        if (writable) std::filesystem::remove(detail::entryPath(dir, k), fsError);
        return nullptr;  // ~SharedCode removes the JITDylib
      }
      code->objectBytes = e.objectBytes;
      code->stackProbes = e.stackProbes;
      code->irInstructions = e.irInstructions;
      code->inlinedOps = e.inlinedOps;
      code->calledOps = e.calledOps;
      code->shared = true;
      s.codes[key] = code;
      return code;
    };
    if (!cc.dir.empty())
      if (auto code = tryDir(cc.dir, key, true)) {
        ++s.counters.diskHits;
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
            ++s.counters.aotHits;
            return attach(code, Source::Aot);
          }
          if (k == baselineKey) break;
        }
    }
  }
  st.cacheMs = msSince(tc);

  // 3. Compile.
  ++s.counters.compiles;
  auto ctx = std::make_unique<llvm::LLVMContext>();
  auto mod = std::make_unique<llvm::Module>(go.prefix, *ctx);
  mod->setDataLayout(s.tm->createDataLayout());
  mod->setTargetTriple(s.tm->getTargetTriple());

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

  t1 = clock::now();
  {
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassBuilder pb(s.tm.get());
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
    mpm.run(*mod, mam);
  }
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
      if (!s.tm->addPassesToEmitFile(pm, as, nullptr, llvm::CodeGenFileType::AssemblyFile)) pm.run(*clone);
    }
  }
  for (const llvm::Function& f : *mod) st.irInstructions += static_cast<uint32_t>(f.getInstructionCount());

  // The code may be shared and stored only if every pointer-table slot it added is inside the executable (functions,
  // static data): those are the same in every image of this key and at the same offset in every run of this binary.
  auto code = std::make_shared<SharedCode>();
  code->key = key;
  code->hostSlots = hostSlots;
  code->fnSlots.assign(tab.ptrs().begin() + hostSlots, tab.ptrs().end());
  code->irHash = irHash;
  code->irKey = key;
  code->baselineIr = s.baseline;
  bool cacheable = bin.ok;
  for (const void* p : code->fnSlots) cacheable = cacheable && insideBinary(p, bin);
  if (!cacheable) ++s.counters.uncacheable;
  const bool store = cacheable && !cc.dir.empty();

  t1 = clock::now();
  auto jd = s.jit->createJITDylib("pq_code" + std::to_string(s.nextId++));
  if (!jd) return llvmError("creating a JITDylib", jd.takeError());
  code->es = &s.jit->getExecutionSession();
  code->jd = &*jd;  // from here `code` owns the JITDylib: released if anything below fails
  if (auto err = s.jit->addIRModule(*code->jd, llvm::orc::ThreadSafeModule(std::move(mod), std::move(ctx))))
    return llvmError("adding the module", std::move(err));
  s.objectBytes = 0;
  s.stackProbes = false;
  s.capture = store;
  s.captured.clear();
  code->fns.assign(gen->stageFunctions.size(), nullptr);
  code->pairFns.assign(gen->stageFunctions.size(), nullptr);
  for (size_t k = 0; k < gen->stageFunctions.size(); ++k) {
    auto sym = s.jit->lookup(*code->jd, gen->stageFunctions[k]);  // materialises (compiles) the module now, not on the audio thread
    if (!sym) {
      s.capture = false;
      return llvmError("looking up a stage function", sym.takeError());
    }
    code->fns[k] = sym->toPtr<detail::StageFn>();
    if (gen->pairFunctions[k].empty()) continue;
    auto pair = s.jit->lookup(*code->jd, gen->pairFunctions[k]);
    if (!pair) {
      s.capture = false;
      return llvmError("looking up a pair function", pair.takeError());
    }
    code->pairFns[k] = pair->toPtr<detail::StageFn>();
    ++code->pairedStages;
  }
  s.capture = false;
  st.machineCodeMs = msSince(t1);
  st.objectBytes = s.objectBytes;
  st.stackProbes = s.stackProbes;
  code->objectBytes = s.objectBytes;
  code->stackProbes = s.stackProbes;
  code->irInstructions = st.irInstructions;
  code->inlinedOps = st.inlinedOps;
  code->calledOps = st.calledOps;

  tc = clock::now();
  if (store && s.captured.size() == (gen->stageFunctions.empty() ? 0u : 1u)) {
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
    if (!s.captured.empty()) e.object = std::move(s.captured.front());
    if (s.sizedDir != cc.dir) {
      s.sizedDir = cc.dir;
      s.dirBytes = detail::directoryBytes(cc.dir);
      s.storesSinceScan = 0;
    }
    if (const uint64_t written = detail::writeEntry(cc.dir, key, e); written > 0) {
      ++s.counters.stores;
      s.dirBytes += written;
      // Other processes write here too: look at the real size now and then, and whenever ours says it is over.
      if (s.dirBytes > cc.maxBytes || ++s.storesSinceScan >= 64) {
        const detail::EvictResult r = detail::evict(cc.dir, cc.maxBytes);
        s.dirBytes = r.bytes;
        s.storesSinceScan = 0;
        s.counters.evictedFiles += r.evictedFiles;
        s.counters.evictedBytes += r.evictedBytes;
      }
    } else {
      ++s.counters.storeErrors;
    }
  } else if (store) {
    ++s.counters.storeErrors;  // not one object (never expected)
  }
  s.captured.clear();
  if (cacheable && s.codes.find(key) == s.codes.end()) {  // (a verify mismatch leaves the live one registered)
    code->shared = true;
    s.codes[key] = code;
  }
  st.cacheMs += msSince(tc);
  return attach(code, Source::Compiled);
}

}  // namespace pq::jit
