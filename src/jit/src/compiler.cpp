// Compiler: LLJIT for the host CPU, the IR pipeline, and image assembly (pq/jit/compiler.h).

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
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include <chrono>
#include <cstdlib>
#include <mutex>

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

}  // namespace

struct Compiler::Session {
  std::unique_ptr<llvm::orc::LLJIT> jit;
  std::unique_ptr<llvm::TargetMachine> tm;  // the IR pipeline's target info (same CPU/features as the JIT's)
  std::string cpu, features;
  std::recursive_mutex mutex;  // compiles are serialised; images remove their code under it (also from a failed compile)
  uint64_t nextId = 0;
  // Filled by the object transform while a compile materialises its module (under `mutex`).
  uint64_t objectBytes = 0;
  bool stackProbes = false;
};

namespace {

// Keeps an image's machine code alive; removes it when the image is destroyed.
struct CodeOwner {
  std::shared_ptr<Compiler::Session> session;
  llvm::orc::ResourceTrackerSP tracker;
  CodeOwner(std::shared_ptr<Compiler::Session> s, llvm::orc::ResourceTrackerSP t) : session(std::move(s)), tracker(std::move(t)) {}
  ~CodeOwner() {
    std::lock_guard<std::recursive_mutex> lock(session->mutex);
    llvm::consumeError(tracker->remove());
  }
  CodeOwner(const CodeOwner&) = delete;
  CodeOwner& operator=(const CodeOwner&) = delete;
};

}  // namespace

Compiler::Compiler(std::shared_ptr<Session> s) noexcept : session_(std::move(s)) {}
Compiler::~Compiler() = default;

Result<std::unique_ptr<Compiler>> Compiler::create(const CompilerConfig& config) {
  if (config.codeGenLevel > 3) return Error{ErrorCode::InvalidArgument, "jit: codeGenLevel must be 0..3", {}};
  initialiseLlvmOnce();
  auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
  if (!jtmb) return llvmError("detecting the host", jtmb.takeError());
  // Host CPU and every feature it has (AVX2, FMA, and AVX-512 where present).
  jtmb->setCPU(llvm::sys::getHostCPUName().str());
  llvm::SubtargetFeatures features;
  for (const auto& f : llvm::sys::getHostCPUFeatures()) features.AddFeature(f.first(), f.second);
  jtmb->getFeatures() = features;
  static constexpr llvm::CodeGenOptLevel kLevels[] = {llvm::CodeGenOptLevel::None, llvm::CodeGenOptLevel::Less,
                                                      llvm::CodeGenOptLevel::Default, llvm::CodeGenOptLevel::Aggressive};
  jtmb->setCodeGenOptLevel(kLevels[config.codeGenLevel]);
  auto session = std::make_shared<Session>();
  session->cpu = jtmb->getCPU();
  session->features = jtmb->getFeatures().getString();
  auto tm = jtmb->createTargetMachine();
  if (!tm) return llvmError("creating the target machine", tm.takeError());
  session->tm = std::move(*tm);
  auto jit = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
  if (!jit) return llvmError("creating LLJIT", jit.takeError());
  session->jit = std::move(*jit);
  // Every object on its way to the linker: its size, and whether it calls the stack probe (a frame > 4 KiB).
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
        return std::move(obj);
      });
  return std::unique_ptr<Compiler>(new Compiler(std::move(session)));
}

const std::string& Compiler::hostCpu() const noexcept { return session_->cpu; }
const std::string& Compiler::hostFeatures() const noexcept { return session_->features; }

Result<std::unique_ptr<engine::ProgramImage>> Compiler::compile(const graph::Schedule& schedule, const CompileOptions& options,
                                                                CompileStats* stats) {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  CompileStats st;
  if (options.optLevel > 3) return Error{ErrorCode::InvalidArgument, "jit: optLevel must be 0..3", {}};
  detail::PtrTable tab;
  auto host = detail::buildHost(schedule, options.maxBlockFrames, options.maxVoiceGroups, options.resources, tab);
  if (!host) return std::move(host).error();
  std::unique_ptr<detail::JitImage> image = std::move(host.value());
  st.hostMs = msSince(t0);

  std::lock_guard<std::recursive_mutex> lock(session_->mutex);
  Session& s = *session_;
  const uint64_t id = s.nextId++;
  auto ctx = std::make_unique<llvm::LLVMContext>();
  auto mod = std::make_unique<llvm::Module>("pq_image" + std::to_string(id), *ctx);
  mod->setDataLayout(s.tm->createDataLayout());
  mod->setTargetTriple(s.tm->getTargetTriple());

  auto t1 = clock::now();
  detail::GenOptions go;
  go.contract = options.contract;
  go.cpu = s.cpu;
  go.features = s.features;
  go.prefix = "pq_image" + std::to_string(id);
  auto gen = detail::generate(schedule, image->layout_, tab, *mod, go);
  if (!gen) return std::move(gen).error();
  st.codegenMs = msSince(t1);
  st.inlinedOps = gen->inlinedOps;
  st.calledOps = gen->calledOps;

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
  for (const llvm::Function& f : *mod) st.irInstructions += static_cast<uint32_t>(f.getInstructionCount());

  t1 = clock::now();
  llvm::orc::ResourceTrackerSP tracker = s.jit->getMainJITDylib().createResourceTracker();
  if (auto err = s.jit->addIRModule(tracker, llvm::orc::ThreadSafeModule(std::move(mod), std::move(ctx))))
    return llvmError("adding the module", std::move(err));
  // From here the tracker owns code: release it if anything below fails.
  auto owner = std::make_shared<CodeOwner>(session_, tracker);
  s.objectBytes = 0;
  s.stackProbes = false;
  for (size_t k = 0; k < gen->stageFunctions.size(); ++k) {
    auto sym = s.jit->lookup(gen->stageFunctions[k]);  // materialises (compiles) the module now, not on the audio thread
    if (!sym) return llvmError("looking up a stage function", sym.takeError());
    image->fns_[k] = sym->toPtr<detail::StageFn>();
  }
  st.machineCodeMs = msSince(t1);
  st.objectBytes = s.objectBytes;
  st.stackProbes = s.stackProbes;
  image->tab_ = std::move(tab.ptrs());
  image->codeOwner_ = std::move(owner);
  st.totalMs = msSince(t0);
  if (stats) *stats = st;
  return std::unique_ptr<engine::ProgramImage>(std::move(image));
}

}  // namespace pq::jit
