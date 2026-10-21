#include <doctest/doctest.h>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>

#include <memory>
#include <string>

// Proves the prebuilt LLVM links into an MSVC /MT binary and that ORC can JIT and run native code.
TEST_CASE("llvm: ORC LLJIT compiles and runs float a*a+b") {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  MESSAGE("LLVM host CPU: " << llvm::sys::getHostCPUName().str());

  auto jitOrErr = llvm::orc::LLJITBuilder().create();
  if (!jitOrErr) {
    FAIL(llvm::toString(jitOrErr.takeError()));
  }
  auto jit = std::move(*jitOrErr);

  auto ctx = std::make_unique<llvm::LLVMContext>();
  auto mod = std::make_unique<llvm::Module>("pq_smoke", *ctx);
  auto* f32 = llvm::Type::getFloatTy(*ctx);
  auto* fnTy = llvm::FunctionType::get(f32, {f32, f32}, false);
  auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, "pq_smoke_mad", mod.get());
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(*ctx, "entry", fn));
  auto arg = fn->arg_begin();
  llvm::Value* a = &*arg++;
  llvm::Value* c = &*arg;
  b.CreateRet(b.CreateFAdd(b.CreateFMul(a, a), c));
  REQUIRE_FALSE(llvm::verifyFunction(*fn, &llvm::errs()));

  if (auto err = jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(mod), std::move(ctx)))) {
    FAIL(llvm::toString(std::move(err)));
  }
  auto symOrErr = jit->lookup("pq_smoke_mad");
  if (!symOrErr) {
    FAIL(llvm::toString(symOrErr.takeError()));
  }
  auto* fp = symOrErr->toPtr<float (*)(float, float)>();
  CHECK(fp(3.0f, 1.0f) == 10.0f);
  CHECK(fp(-2.0f, 0.5f) == 4.5f);
}
