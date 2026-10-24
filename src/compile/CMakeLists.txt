# pq_compile (T1.x-arch2): file → Schedule → ProgramImage on either backend, the process's JIT compiler. Used by pqc
# (tools/pqc) and the app (pqdaw --play, the DAW's background JIT images).
pq_add_module(pq_compile
  SOURCES
    src/compile.cpp
    src/image.cpp
  DEPS pq_core pq_graph pq_engine pq_jit)
