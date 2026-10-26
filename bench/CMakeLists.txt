# Benchmark executables. Not registered with CTest (except the runner smoke test at the end); run them from the
# release build and record results in RESULTS.md (runner benches: --csv + tools/bench/append_results.py).
#   pq_add_bench(pq_bench_core SOURCES core/bench_spsc.cpp DEPS pq_core)

add_library(pq_bench_support STATIC  # thread pinning, TSC, system load; the T1.11 runner (filter, CSV)
  support/pq_bench_support.cpp
  support/pq_bench_runner.cpp)
target_include_directories(pq_bench_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
pq_target_defaults(pq_bench_support)
# T1.11-fix1: the preset the benches were built with, from the compile flags (NDEBUG alone cannot tell ship from
# release), for the CSV and tools/bench/append_results.py: ship = Release + /GL (LTCG), release = Release, dev = else.
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_CXX_FLAGS_RELEASE MATCHES "/GL")
  set(PQ_BENCH_PRESET ship)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(PQ_BENCH_PRESET release)
else()
  set(PQ_BENCH_PRESET dev)
endif()
target_compile_definitions(pq_bench_support PRIVATE PQ_BENCH_PRESET="${PQ_BENCH_PRESET}")

function(pq_add_bench name)
  cmake_parse_arguments(A "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${A_SOURCES})
  target_link_libraries(${name} PRIVATE pq_bench_support ${A_DEPS})
  pq_target_defaults(${name})
  # T0.x-buildsize: in dev, benches are built only on request (`build.ps1 -Target <bench>`), or all of them with
  # -DPQ_DEV_BENCHES=ON. Recorded numbers come from ship anyway; dev bench binaries cost ~0.5 GB each when they link
  # LLVM. pq_bench_dsp stays in `all` (the runner smoke test runs it).
  if(PQ_BENCH_PRESET STREQUAL "dev" AND NOT PQ_DEV_BENCHES AND NOT name STREQUAL "pq_bench_dsp")
    set_target_properties(${name} PROPERTIES EXCLUDE_FROM_ALL TRUE)
  endif()
endfunction()
option(PQ_DEV_BENCHES "dev preset: build every bench executable in `all`" OFF)

# Benchmarks are added below as modules appear.

pq_add_bench(pq_bench_core SOURCES core/bench_rt_primitives.cpp DEPS pq_core)
pq_add_bench(pq_bench_tuning SOURCES tuning/bench_resolve.cpp DEPS pq_tuning)
pq_add_bench(pq_bench_fast_math SOURCES core/bench_fast_math.cpp DEPS pq_core)
pq_add_bench(pq_bench_time SOURCES core/bench_time.cpp DEPS pq_core)
pq_add_bench(pq_bench_grid SOURCES core/bench_grid.cpp DEPS pq_core)
pq_add_bench(pq_bench_mos SOURCES tuning/bench_mos.cpp DEPS pq_tuning)
pq_add_bench(pq_bench_scala_ji SOURCES tuning/bench_scala_ji.cpp DEPS pq_tuning)
pq_add_bench(pq_bench_ui SOURCES ui/bench_ui.cpp DEPS pq_ui pq_ui_scenes)
pq_add_bench(pq_bench_graph SOURCES graph/bench_patch.cpp DEPS pq_graph)
pq_add_bench(pq_bench_audio_io SOURCES engine/bench_audio_io.cpp DEPS pq_engine)
pq_add_bench(pq_bench_dsp SOURCES dsp/bench_atoms.cpp DEPS pq_dsp)
pq_add_bench(pq_bench_text_lines SOURCES ui/bench_text_lines.cpp DEPS pq_ui pq_ui_scenes)
pq_add_bench(pq_bench_parse SOURCES graph/bench_parse.cpp DEPS pq_graph)
pq_add_bench(pq_bench_document SOURCES document/bench_document.cpp DEPS pq_doc)
pq_add_bench(pq_bench_widgets SOURCES ui/bench_widgets.cpp DEPS pq_ui)
pq_add_bench(pq_bench_controls SOURCES ui/bench_controls.cpp DEPS pq_ui pq_ui_scenes)
pq_add_bench(pq_bench_partial_redraw SOURCES ui/bench_partial_redraw.cpp DEPS pq_ui pq_ui_scenes)
pq_add_bench(pq_bench_infer SOURCES graph/bench_infer.cpp DEPS pq_graph)
pq_add_bench(pq_bench_serialize SOURCES document/bench_serialize.cpp DEPS pq_doc)
pq_add_bench(pq_bench_optimise SOURCES graph/bench_optimise.cpp DEPS pq_graph)
pq_add_bench(pq_bench_edit_ops SOURCES document/bench_edit_ops.cpp DEPS pq_doc)
pq_add_bench(pq_bench_schedule SOURCES graph/bench_schedule.cpp DEPS pq_graph)
pq_add_bench(pq_bench_interpreter SOURCES engine/bench_interpreter.cpp DEPS pq_engine)
pq_add_bench(pq_bench_shell SOURCES views/bench_shell.cpp DEPS pq_views)
pq_add_bench(pq_bench_voices SOURCES engine/bench_voices.cpp DEPS pq_engine)
pq_add_bench(pq_bench_jit SOURCES jit/bench_jit.cpp DEPS pq_jit)
# T1b.3: the stdlib subcircuits (JIT, interpreter) vs their hand-written C++ references (tests/stdlib/).
pq_add_bench(pq_bench_stdlib SOURCES jit/bench_stdlib.cpp DEPS pq_jit)
pq_use_build_paths(pq_bench_stdlib)
target_compile_definitions(pq_bench_stdlib PRIVATE PQ_TEST_DATA_DIR=pq_build_test_data_dir)
# T4.9 / T4.10: the stock devices (Utility, Saturator, EQ Eight) vs their hand-written C++ references.
pq_add_bench(pq_bench_devices SOURCES jit/bench_devices.cpp DEPS pq_jit)
pq_use_build_paths(pq_bench_devices)
target_compile_definitions(pq_bench_devices PRIVATE PQ_TEST_DATA_DIR=pq_build_test_data_dir)
# T4.5: what probes and cost meters cost on the stock devices, and the meter against a benchmark of the same image.
pq_add_bench(pq_bench_inspect SOURCES jit/bench_inspect.cpp DEPS pq_jit)
pq_use_build_paths(pq_bench_inspect)
target_compile_definitions(pq_bench_inspect PRIVATE PQ_TEST_DATA_DIR=pq_build_test_data_dir)
pq_add_bench(pq_bench_hot_swap SOURCES engine/bench_hot_swap.cpp DEPS pq_engine)
pq_add_bench(pq_bench_timeline SOURCES engine/bench_timeline.cpp DEPS pq_engine)
# T2.6: per-voice pitch cost of tuning as a signal (the tuning_hz kernel; a tuned track through the VoiceEngine).
pq_add_bench(pq_bench_tuning_signal SOURCES engine/bench_tuning_signal.cpp DEPS pq_engine)
# T1.x-arch1: per-track compile units on the 30-track fixture (compile, per-unit vs whole, one-track edits).
pq_add_bench(pq_bench_project SOURCES engine/bench_project.cpp DEPS pq_jit)
# T1b.5: the interpreter -> JIT live swap (background compiler, Instant swaps) on the 30-track fixture.
pq_add_bench(pq_bench_live_swap SOURCES engine/bench_live_swap.cpp DEPS pq_compile)
# T1b.4: the JIT compile cache on the 30-track fixture: cold open with no cache, an empty cache and a warm cache.
pq_add_bench(pq_bench_jit_cache SOURCES engine/bench_jit_cache.cpp DEPS pq_compile)
# T1.x-silence: the idle 30-track stdlib reverb/delay project after its tails (asleep), and the sleep bookkeeping on
# the busy 30-track fixture (interp / JIT, sleep plans on / off).
pq_add_bench(pq_bench_silence SOURCES engine/bench_silence.cpp DEPS pq_compile)
pq_use_build_paths(pq_bench_silence)
target_compile_definitions(pq_bench_silence PRIVATE PQ_TEST_DATA_DIR=pq_build_test_data_dir)
# T3.17-fix2: the lowered mixer through the whole playback path (64 tracks x R returns, silent / notes, interp / JIT).
pq_add_bench(pq_bench_playback SOURCES app/bench_playback.cpp DEPS pq_app)
pq_add_bench(pq_bench_arrangement SOURCES views/bench_arrangement.cpp DEPS pq_views)
pq_add_bench(pq_bench_mos_chart SOURCES views/bench_mos_chart.cpp DEPS pq_views)
pq_add_bench(pq_bench_mos_wheel SOURCES views/bench_mos_wheel.cpp DEPS pq_views)
pq_add_bench(pq_bench_mixer SOURCES views/bench_mixer.cpp DEPS pq_views)
pq_add_bench(pq_bench_automation SOURCES views/bench_automation.cpp DEPS pq_views)
# T1.11: the 32-op voice patch at 1/8/64 voices (interpreter, JIT) and the golden offline renders (T1.10).
# T1.11-fix1: + the proto-Operator at 64 voices (interpreter, JIT, crossfade; SPEC §13 calibration, bench/patches).
pq_add_bench(pq_bench_render SOURCES engine/bench_render.cpp DEPS pq_pqc)
pq_use_build_paths(pq_bench_render)
target_compile_definitions(pq_bench_render PRIVATE PQ_SOURCE_DIR=pq_build_source_dir)

# The bench tests: the T1.11 runner's options and CSV output (a few ms of measuring, any build), and
# tools/bench/append_results.py's checks (T1.11-fix1; needs a Python 3 interpreter, skipped without one).
add_test(NAME pq_bench_runner_smoke
  COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:pq_bench_dsp> -DCSV=${CMAKE_CURRENT_BINARY_DIR}/runner_smoke.csv
          -DPRESET=${PQ_BENCH_PRESET} -P ${CMAKE_CURRENT_SOURCE_DIR}/runner_smoke.cmake)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME pq_tools_append_results
    COMMAND Python3::Interpreter -B -I ${PROJECT_SOURCE_DIR}/tools/bench/test_append_results.py)
else()
  message(STATUS "pqdaw: no Python 3 interpreter; the append_results.py test (pq_tools_append_results) is not registered")
endif()
pq_add_bench(pq_bench_piano_roll SOURCES views/bench_piano_roll.cpp DEPS pq_views)
# T4.2 / T4.3: the circuit canvas over a 10k-atom patch (pan / zoom frames, tiles) and the auto-layout of 500-node patches.
pq_add_bench(pq_bench_circuit SOURCES views/bench_circuit.cpp DEPS pq_views)
# T4.4: the circuit editor on the largest stdlib patch: a keystroke (re-parse + id diff + layout, + scene), the debounced
# check, and the typing latency of the editor window (key -> document -> scene -> frame).
pq_add_bench(pq_bench_circuit_editor SOURCES views/bench_circuit_editor.cpp DEPS pq_views)
pq_use_build_paths(pq_bench_circuit_editor)
target_compile_definitions(pq_bench_circuit_editor PRIVATE PQ_SOURCE_DIR=pq_build_source_dir)
# T4.6: device panels (full paints, knob / EQ-dot drag frames, the spectrum frame and analysis, the tap's audio-side cost).
pq_add_bench(pq_bench_device_panels SOURCES views/bench_device_panels.cpp DEPS pq_views)
pq_use_build_paths(pq_bench_device_panels)
target_compile_definitions(pq_bench_device_panels PRIVATE PQ_SOURCE_DIR=pq_build_source_dir)
# T4.13: the project patch view over 64 tracks (model / scene builds, whole-project and band paints and pans, the morph).
pq_add_bench(pq_bench_project_patch SOURCES views/bench_project_patch.cpp DEPS pq_views)
pq_use_build_paths(pq_bench_project_patch)
target_compile_definitions(pq_bench_project_patch PRIVATE PQ_SOURCE_DIR=pq_build_source_dir)
