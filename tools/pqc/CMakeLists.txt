# pqc (T1.10): the patch compiler CLI. pq_pqc holds everything but main() so tests run the same code.
pq_add_module(pq_pqc
  SOURCES
    src/cli.cpp
    src/notes.cpp
    src/render.cpp
  DEPS pq_core pq_graph pq_engine pq_compile)

pq_add_executable(pqc
  SOURCES src/main.cpp
  DEPS pq_pqc)
