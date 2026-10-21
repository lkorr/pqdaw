# Toolchain smoke tests: prove the compiler, CRT, and LLVM ORC JIT link and run.
pq_add_test(pq_tests_smoke
  SOURCES smoke/test_toolchain.cpp smoke/test_llvm_jit.cpp
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
    core/test_math_time_rt.cpp
    core/test_stable_id.cpp
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
  DEPS pq_tuning nlohmann_json::nlohmann_json)

# Child process for the "default handler aborts with a stack trace" test: links the trap, no handler.
add_executable(pq_alloc_trap_abort_probe core/alloc_trap_abort_probe.cpp)
target_link_libraries(pq_alloc_trap_abort_probe PRIVATE pq_alloc_trap)
pq_target_defaults(pq_alloc_trap_abort_probe)
add_dependencies(pq_tests_core pq_alloc_trap_abort_probe)
target_compile_definitions(pq_tests_core PRIVATE
  PQ_ALLOC_TRAP_PROBE="$<TARGET_FILE:pq_alloc_trap_abort_probe>")
# ui: draw list, PNG, damage (CPU) and the offscreen Vulkan path incl. the test-pattern golden (GPU).
pq_add_test(pq_tests_ui
  SOURCES
    ui/test_draw_list.cpp
    ui/test_image.cpp
    ui/test_render.cpp
  DEPS pq_ui)

# app: CLI parsing and the --screenshot command.
pq_add_test(pq_tests_app
  SOURCES
    app/test_cli.cpp
  DEPS pq_app pq_ui)

# The real executable: headless screenshot, and the scripted window resize/minimise/restore run
# (exit 77 = no display available → reported as skipped, not passed).
add_test(NAME pqdaw_screenshot
  COMMAND pqdaw --screenshot test-pattern ${CMAKE_CURRENT_BINARY_DIR}/pqdaw_screenshot.png --size 400x240)
add_test(NAME pqdaw_window_selftest COMMAND pqdaw --window-selftest)
set_tests_properties(pqdaw_window_selftest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

pq_add_test(pq_tests_graph
  SOURCES
    graph/test_types.cpp
    graph/test_atoms.cpp
    graph/test_patch_build.cpp
    graph/test_validate.cpp
    graph/test_patch_equal.cpp
    graph/test_patch_file.cpp
  DEPS pq_graph)
