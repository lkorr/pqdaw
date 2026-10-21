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
  DEPS pq_core)

pq_add_test(pq_tests_tuning
  SOURCES
    tuning/test_rational.cpp
    tuning/test_monzo.cpp
    tuning/test_edo.cpp
    tuning/test_resolve.cpp
  DEPS pq_tuning)

# Child process for the "default handler aborts with a stack trace" test: links the trap, no handler.
add_executable(pq_alloc_trap_abort_probe core/alloc_trap_abort_probe.cpp)
target_link_libraries(pq_alloc_trap_abort_probe PRIVATE pq_alloc_trap)
pq_target_defaults(pq_alloc_trap_abort_probe)
add_dependencies(pq_tests_core pq_alloc_trap_abort_probe)
target_compile_definitions(pq_tests_core PRIVATE
  PQ_ALLOC_TRAP_PROBE="$<TARGET_FILE:pq_alloc_trap_abort_probe>")
