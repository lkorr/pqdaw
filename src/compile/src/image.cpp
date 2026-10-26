// pq_compile: Schedule → ProgramImage on either backend, and the process's JIT compiler (pq/compile/compile.h).
#include <mutex>
#include <optional>
#include <string>

#include "pq/compile/compile.h"

namespace pq::compile {

const char* backendLabel(Backend b) noexcept { return b == Backend::Jit ? "jit" : "interpreter"; }

Result<Backend> parseBackend(std::string_view name) {
  if (name == "interp" || name == "interpreter") return Backend::Interpreter;
  if (name == "jit") return Backend::Jit;
  return Error{ErrorCode::InvalidArgument, "unknown backend '" + std::string(name) + "' (interp or jit)", {}};
}

namespace {

// setJitCache before the compiler exists: kept here and applied when jitCompiler() creates it.
std::mutex& cacheMutex() {
  static std::mutex m;
  return m;
}
std::optional<jit::CacheConfig>& pendingCache() {
  static std::optional<jit::CacheConfig> c;
  return c;
}
bool& compilerCreated() {
  static bool created = false;
  return created;
}

}  // namespace

Result<jit::Compiler*> jitCompiler() {
  // Created once, on first use, by whichever thread gets here first (a function-local static is thread-safe).
  static Result<std::unique_ptr<jit::Compiler>> compiler = [] {
    jit::CompilerConfig config;
    std::lock_guard<std::mutex> lock(cacheMutex());
    if (pendingCache()) config.cache = *pendingCache();
    compilerCreated() = true;
    return jit::Compiler::create(config);
  }();
  if (!compiler) return compiler.error();
  return compiler.value().get();
}

void setJitCache(const jit::CacheConfig& cache) {
  {
    std::lock_guard<std::mutex> lock(cacheMutex());
    pendingCache() = cache;
    if (!compilerCreated()) return;  // applied at creation
  }
  if (auto jc = jitCompiler()) (*jc)->setCache(cache);
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
Result<std::unique_ptr<engine::ProgramImage>> jitUnitWith(void* compiler, const graph::Schedule& schedule,
                                                          uint32_t maxBlockFrames, uint32_t maxVoiceGroups,
                                                          const engine::Resources* resources) {
  jit::CompileOptions co;
  co.maxBlockFrames = maxBlockFrames;
  co.maxVoiceGroups = maxVoiceGroups;
  co.resources = resources;
  return static_cast<jit::Compiler*>(compiler)->compile(schedule, co);
}
}  // namespace

namespace {
// Q111 C: the quick first image (jit::CompileOptions::quickFirst) through `compiler` (null: the process compiler).
Result<std::unique_ptr<engine::ProgramImage>> jitUnitQuick(void* compiler, const graph::Schedule& schedule,
                                                           uint32_t maxBlockFrames, uint32_t maxVoiceGroups,
                                                           const engine::Resources* resources, bool* refine) {
  jit::Compiler* c = static_cast<jit::Compiler*>(compiler);
  if (c == nullptr) {
    auto jc = jitCompiler();
    if (!jc) return Error{jc.error().code, "jit: " + jc.error().message, {}};
    c = *jc;
  }
  jit::CompileOptions co;
  co.maxBlockFrames = maxBlockFrames;
  co.maxVoiceGroups = maxVoiceGroups;
  co.resources = resources;
  co.quickFirst = true;
  jit::CompileStats st;
  auto img = c->compile(schedule, co, &st);
  if (refine != nullptr) *refine = img.ok() && st.refine;
  return img;
}
}  // namespace

engine::UnitBackend jitUnitBackend(bool quickFirst) noexcept {
  engine::UnitBackend b{&jitUnit, nullptr};
  if (quickFirst) b.quick = &jitUnitQuick;
  return b;
}
engine::UnitBackend jitUnitBackend(jit::Compiler* compiler, bool quickFirst) noexcept {
  engine::UnitBackend b{&jitUnitWith, compiler};
  if (quickFirst) b.quick = &jitUnitQuick;
  return b;
}

}  // namespace pq::compile
