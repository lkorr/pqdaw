// pq_compile: Schedule → ProgramImage on either backend, and the process's JIT compiler (pq/compile/compile.h).
#include <string>

#include "pq/compile/compile.h"

namespace pq::compile {

const char* backendLabel(Backend b) noexcept { return b == Backend::Jit ? "jit" : "interpreter"; }

Result<Backend> parseBackend(std::string_view name) {
  if (name == "interp" || name == "interpreter") return Backend::Interpreter;
  if (name == "jit") return Backend::Jit;
  return Error{ErrorCode::InvalidArgument, "unknown backend '" + std::string(name) + "' (interp or jit)", {}};
}

Result<jit::Compiler*> jitCompiler() {
  // Created once, on first use, by whichever thread gets here first (a function-local static is thread-safe).
  static Result<std::unique_ptr<jit::Compiler>> compiler = jit::Compiler::create();
  if (!compiler) return compiler.error();
  return compiler.value().get();
}

Result<std::unique_ptr<engine::ProgramImage>> buildImage(const graph::Schedule& schedule, Backend backend,
                                                         const ImageOptions& options) {
  if (backend == Backend::Jit) {
    auto jc = jitCompiler();
    if (!jc) return Error{jc.error().code, "jit: " + jc.error().message, {}};
    jit::CompileOptions co = options.jit;
    co.maxBlockFrames = options.maxBlockFrames;
    co.maxVoiceGroups = options.maxVoiceGroups;
    co.resources = options.resources;
    return (*jc)->compile(schedule, co);  // its errors already say "jit: …"
  }
  engine::InterpretOptions io;
  io.maxBlockFrames = options.maxBlockFrames;
  io.maxVoiceGroups = options.maxVoiceGroups;
  io.resources = options.resources;
  return engine::buildInterpreted(schedule, io);  // "interpreter: …"
}

namespace {
Result<std::unique_ptr<engine::ProgramImage>> jitUnit(void*, const graph::Schedule& schedule, uint32_t maxBlockFrames,
                                                      uint32_t maxVoiceGroups, const engine::Resources* resources) {
  ImageOptions o;
  o.maxBlockFrames = maxBlockFrames;
  o.maxVoiceGroups = maxVoiceGroups;
  o.resources = resources;
  return buildImage(schedule, Backend::Jit, o);
}
}  // namespace

engine::UnitBackend jitUnitBackend() noexcept { return {&jitUnit, nullptr}; }

}  // namespace pq::compile
