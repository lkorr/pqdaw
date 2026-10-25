pq_add_module(pq_jit
  SOURCES
    src/cache.cpp
    src/codegen.cpp
    src/compiler.cpp
    src/jit_image.cpp
    src/runtime.cpp
  DEPS pq_core pq_dsp pq_graph pq_engine
  PRIVATE_DEPS pq_llvm
)
# LLVM's inline header code trips C4702 (unreachable code) when instantiated in our /WX translation units; the
# warning is reported at the header lines, which /external:W0 does not cover for code-generation warnings.
target_compile_options(pq_jit PRIVATE /wd4702)
