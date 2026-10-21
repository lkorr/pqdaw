# Toolchain smoke tests: prove the compiler, CRT, and LLVM ORC JIT link and run.
pq_add_test(pq_tests_smoke
  SOURCES smoke/test_toolchain.cpp smoke/test_llvm_jit.cpp smoke/test_cpu_baseline.cpp
  DEPS pq_core pq_llvm)

# Module test suites are added below as modules appear, e.g.
#   pq_add_test(pq_tests_core SOURCES core/test_spsc_ring.cpp DEPS pq_core)

pq_add_test(pq_tests_core
  SOURCES
    core/test_result.cpp
    core/test_alloc_trap.cpp
    core/test_aligned_buffer.cpp
    core/test_spsc_ring.cpp
    core/test_mpsc_queue.cpp
    core/test_fixed_pool.cpp
    core/test_arena.cpp
    core/test_epoch_reclaimer.cpp
    core/test_denormal_guard.cpp
    core/test_fast_math.cpp
    core/test_time.cpp
    core/test_grid.cpp
    core/test_math_time_rt.cpp
    core/test_stable_id.cpp
    core/test_rt_log.cpp
  DEPS pq_core)

pq_add_test(pq_tests_tuning
  SOURCES
    tuning/test_rational.cpp
    tuning/test_monzo.cpp
    tuning/test_edo.cpp
    tuning/test_resolve.cpp
    tuning/test_mos.cpp
    tuning/test_mos_fixtures.cpp
    tuning/test_tuning_rt.cpp
    tuning/test_scala.cpp
    tuning/test_scale_tuning.cpp
    tuning/test_ji.cpp
    tuning/test_ji_fixtures.cpp
  DEPS pq_tuning nlohmann_json::nlohmann_json)

# Child process for the "default handler aborts with a stack trace" test: links the trap, no handler.
add_executable(pq_alloc_trap_abort_probe core/alloc_trap_abort_probe.cpp)
target_link_libraries(pq_alloc_trap_abort_probe PRIVATE pq_alloc_trap)
pq_target_defaults(pq_alloc_trap_abort_probe)
add_dependencies(pq_tests_core pq_alloc_trap_abort_probe)
target_compile_definitions(pq_tests_core PRIVATE
  PQ_ALLOC_TRAP_PROBE="$<TARGET_FILE:pq_alloc_trap_abort_probe>")
include(${CMAKE_CURRENT_SOURCE_DIR}/core/queue_mutation.cmake)
# ui: draw list, PNG, damage, text, line primitives, colormaps (CPU) and the offscreen Vulkan path incl. the
# test-pattern / text / lines goldens (GPU). PQ_ASSETS_DIR: the bundled fonts.
pq_add_test(pq_tests_ui PERF
  SOURCES
    ui/test_draw_list.cpp
    ui/test_primitives.cpp
    ui/test_image.cpp
    ui/test_render.cpp
    ui/test_text.cpp
    ui/test_lines.cpp
    ui/test_colormap.cpp
    ui/test_layout.cpp
    ui/test_widgets.cpp
    ui/test_hidpi.cpp
    ui/test_controls.cpp
    ui/test_partial_redraw.cpp
    ui/thread_cpu.cpp
  DEPS pq_ui pq_ui_scenes)
target_compile_definitions(pq_tests_ui PRIVATE PQ_ASSETS_DIR="${PQ_SOURCE_DIR}/assets")

# app: CLI parsing (window, --screenshot, --list-devices, --sine, --play), the --screenshot command, the --play compile path.
pq_add_test(pq_tests_app
  SOURCES
    app/test_cli.cpp
    app/test_play.cpp
    app/test_shell_app.cpp
  DEPS pq_app pq_ui)

# views (T3.7): ViewContext (project I/O, undo, transport routing), the shell (layout, hotkeys, control bar), goldens.
pq_add_test(pq_tests_views
  SOURCES
    views/test_view_context.cpp
    views/test_shell.cpp
    views/test_arrangement.cpp
  DEPS pq_views)
# T3.7: the shell window idling over the empty fixture: 0 frames in 10 s at < 0.1 % CPU (exit 77 = no display).
add_test(NAME pqdaw_idle_shell
  COMMAND pqdaw --project ${PQ_SOURCE_DIR}/tests/views/fixtures/empty --idle-check 10)
set_tests_properties(pqdaw_idle_shell PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
add_test(NAME pqdaw_screenshot_shell
  COMMAND pqdaw --screenshot shell ${CMAKE_CURRENT_BINARY_DIR}/pqdaw_screenshot_shell.png --project ${PQ_SOURCE_DIR}/tests/views/fixtures/empty)
# T3.8: the arrangement scene through the real executable.
add_test(NAME pqdaw_screenshot_arrangement
  COMMAND pqdaw --screenshot arrangement-basic ${CMAKE_CURRENT_BINARY_DIR}/pqdaw_screenshot_arrangement.png --project ${PQ_SOURCE_DIR}/tests/views/fixtures/arrangement)

# The real executable: headless screenshot, and the scripted window resize/minimise/restore run
# (exit 77 = no display available → reported as skipped, not passed).
add_test(NAME pqdaw_screenshot
  COMMAND pqdaw --screenshot test-pattern ${CMAKE_CURRENT_BINARY_DIR}/pqdaw_screenshot.png --size 400x240)
add_test(NAME pqdaw_screenshot_text   # the executable finds the bundled fonts (pq::ui::assetsDir)
  COMMAND pqdaw --screenshot text ${CMAKE_CURRENT_BINARY_DIR}/pqdaw_screenshot_text.png)
# T0.1-fix1: the startup CPU baseline check on an emulated CPU without AVX2 (and without FMA): message, exit 1.
add_test(NAME pqdaw_cpu_check_no_avx2 COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:pqdaw> -DDISABLE=avx2
  -DEXPECT=AVX2 -P ${CMAKE_CURRENT_SOURCE_DIR}/app/cpu_baseline_check.cmake)
add_test(NAME pqdaw_cpu_check_no_avx2_fma COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:pqdaw> -DDISABLE=fma,avx2
  "-DEXPECT=AVX2, FMA" -P ${CMAKE_CURRENT_SOURCE_DIR}/app/cpu_baseline_check.cmake)
add_test(NAME pqdaw_window_selftest COMMAND pqdaw --window-selftest)
set_tests_properties(pqdaw_window_selftest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
# Same run with the live widget tree (T3.1): SDL window loop → UiTree layout/paint at every size.
add_test(NAME pqdaw_window_selftest_widgets COMMAND pqdaw --window-selftest --scene widgets-core)
set_tests_properties(pqdaw_window_selftest_widgets PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
# T3.2: the control gallery live (themed controls, overlays, SDL text input and clipboard) through the same run.
add_test(NAME pqdaw_window_selftest_gallery COMMAND pqdaw --window-selftest --scene widgets)
set_tests_properties(pqdaw_window_selftest_gallery PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
add_test(NAME pqdaw_screenshot_widgets COMMAND pqdaw --screenshot widgets ${CMAKE_CURRENT_BINARY_DIR}/pqdaw_screenshot_widgets.png)
# T0.7-fix1: partial redraw through the swapchain (2 px playhead over `lines`: pixels shaded, CPU vs full).
add_test(NAME pqdaw_window_selftest_partial COMMAND pqdaw --window-selftest --scene lines-playhead)
set_tests_properties(pqdaw_window_selftest_partial PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)

pq_add_test(pq_tests_graph
  SOURCES
    graph/test_types.cpp
    graph/test_atoms.cpp
    graph/test_patch_build.cpp
    graph/test_validate.cpp
    graph/test_patch_equal.cpp
    graph/test_patch_file.cpp
    graph/test_parse_roundtrip.cpp
    graph/test_parse_errors.cpp
    graph/test_parse_fuzz.cpp
    graph/test_kernel_map.cpp
    graph/test_infer.cpp
    graph/test_optimise.cpp
    graph/test_schedule.cpp
  DEPS pq_graph)

# engine: block adapter, NullDevice (cadence, buffer sizes, RtScope on the callback path), RtAudio backend checks,
# WASAPI backend (conversion, underrun tracker, opt-in device runs with PQ_TEST_AUDIO_DEVICE=1), the ASIO host
# (fake driver; opt-in real drivers), the interpreter
# backend vs the reference evaluator (T1.7).
pq_add_test(pq_tests_engine PERF
  SOURCES
    engine/test_block_adapter.cpp
    engine/test_null_device.cpp
    engine/test_rtaudio_device.cpp
    engine/test_wasapi.cpp
    engine/test_asio.cpp
    engine/test_rt_log_device.cpp
    engine/test_interpreter.cpp
    engine/test_voices.cpp
    engine/test_hot_swap.cpp
    engine/test_offline_render.cpp
  DEPS pq_engine)

# T1.10: golden offline renders (tests/golden: patches, committed WAVs, the absolute + spectral comparison helper) on
# both backends, and the pqc CLI (in-process, plus the real pqc.exe once).
pq_add_test(pq_tests_render
  SOURCES
    golden/golden_audio.cpp
    golden/test_golden_audio.cpp
    golden/test_golden_render.cpp
    tools/test_pqc.cpp
  DEPS pq_pqc)
add_dependencies(pq_tests_render pqc)
target_compile_definitions(pq_tests_render PRIVATE PQC_EXE="$<TARGET_FILE:pqc>")

# jit (T1b.1): the LLVM backend against the interpreter and the reference evaluator over the T1.7 corpus, plus the
# ProgramImage contract cases and compile times.
pq_add_test(pq_tests_jit
  SOURCES
    jit/test_jit.cpp
    engine/test_voices.cpp  # the T1.8 voice suite again, on JIT images (rigImageBuilder, set in test_jit.cpp)
  DEPS pq_jit)

pq_add_test(pq_tests_dsp
  SOURCES
    dsp/test_atoms_math_logic.cpp
    dsp/test_atoms_memory_gen.cpp
    dsp/test_phasor_drift.cpp
    dsp/test_tables.cpp
    dsp/test_dsp_rt.cpp
  DEPS pq_dsp)

# document: note sequence, every command kind's ChangeSet, history/gestures, structural sharing.
pq_add_test(pq_tests_document
  SOURCES
    document/test_note_seq.cpp
    document/test_commands.cpp
    document/test_store.cpp
    document/test_serialize.cpp
    document/test_serialize_errors.cpp
    document/test_project_io.cpp
    document/test_edit_ops.cpp
    document/test_edit_pitch.cpp
    document/test_edit_time.cpp
  DEPS pq_doc)

# T0.9-fix1: pq::ui's line primitives vs PQ Meter's own code, compiled from the PQ Meter source tree when it
# exists on this machine (skipped otherwise). See the file for how and why.
include(ui/pqmeter_diff.cmake)

# T0.x-flake: wall-clock checks that depend on machine load are opt-in under the `perf` label (the test presets
# exclude it; run with ctest -L perf or scripts/build.ps1 -Perf). See tests/README.md.
add_test(NAME pqdaw_window_selftest_perf COMMAND pqdaw --window-selftest --scene lines-playhead)
set_tests_properties(pqdaw_window_selftest_perf PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 LABELS perf RUN_SERIAL TRUE
  ENVIRONMENT PQ_PERF_CHECKS=1)
