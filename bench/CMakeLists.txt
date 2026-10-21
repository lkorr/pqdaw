# Benchmark executables. Not registered with CTest; run them from the release build and record results in RESULTS.md.
#   pq_add_bench(pq_bench_core SOURCES core/bench_spsc.cpp DEPS pq_core)

add_library(pq_bench_support INTERFACE)
target_include_directories(pq_bench_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

function(pq_add_bench name)
  cmake_parse_arguments(A "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${A_SOURCES})
  target_link_libraries(${name} PRIVATE pq_bench_support ${A_DEPS})
  pq_target_defaults(${name})
endfunction()

# Benchmarks are added below as modules appear.

pq_add_bench(pq_bench_core SOURCES core/bench_rt_primitives.cpp DEPS pq_core)
pq_add_bench(pq_bench_tuning SOURCES tuning/bench_resolve.cpp DEPS pq_tuning)
pq_add_bench(pq_bench_fast_math SOURCES core/bench_fast_math.cpp DEPS pq_core)
pq_add_bench(pq_bench_time SOURCES core/bench_time.cpp DEPS pq_core)
pq_add_bench(pq_bench_mos SOURCES tuning/bench_mos.cpp DEPS pq_tuning)
pq_add_bench(pq_bench_ui SOURCES ui/bench_ui.cpp DEPS pq_ui)
