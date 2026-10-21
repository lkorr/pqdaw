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
pq_add_bench(pq_bench_grid SOURCES core/bench_grid.cpp DEPS pq_core)
pq_add_bench(pq_bench_mos SOURCES tuning/bench_mos.cpp DEPS pq_tuning)
pq_add_bench(pq_bench_scala_ji SOURCES tuning/bench_scala_ji.cpp DEPS pq_tuning)
pq_add_bench(pq_bench_ui SOURCES ui/bench_ui.cpp DEPS pq_ui)
pq_add_bench(pq_bench_graph SOURCES graph/bench_patch.cpp DEPS pq_graph)
pq_add_bench(pq_bench_audio_io SOURCES engine/bench_audio_io.cpp DEPS pq_engine)
pq_add_bench(pq_bench_dsp SOURCES dsp/bench_atoms.cpp DEPS pq_dsp)
pq_add_bench(pq_bench_text_lines SOURCES ui/bench_text_lines.cpp DEPS pq_ui)
pq_add_bench(pq_bench_parse SOURCES graph/bench_parse.cpp DEPS pq_graph)
pq_add_bench(pq_bench_document SOURCES document/bench_document.cpp DEPS pq_doc)
pq_add_bench(pq_bench_widgets SOURCES ui/bench_widgets.cpp DEPS pq_ui)
pq_add_bench(pq_bench_infer SOURCES graph/bench_infer.cpp DEPS pq_graph)
pq_add_bench(pq_bench_serialize SOURCES document/bench_serialize.cpp DEPS pq_doc)
pq_add_bench(pq_bench_optimise SOURCES graph/bench_optimise.cpp DEPS pq_graph)
