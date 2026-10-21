pq_add_module(pq_core
  SOURCES
    src/cpu.cpp
    src/alloc_trap.cpp
    src/epoch_reclaimer.cpp
    src/time.cpp
    src/grid.cpp
    src/rt_log.cpp
)
# DbgHelp: symbolised stack traces when the allocation trap aborts (system library).
target_link_libraries(pq_core PRIVATE Dbghelp)

# CPU baseline (pq/core/cpu_baseline.h, T0.1-fix1): C, compiled without /arch:AVX2 and without /GL, so it is
# safe to run on any x86-64 CPU. The query is archived into pq_core; the startup check is an object linked
# into every pq_add_executable target so its CRT initialiser always reaches the image.
add_library(pq_cpu_baseline OBJECT baseline/cpu_baseline.c)
target_include_directories(pq_cpu_baseline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
pq_target_baseline_defaults(pq_cpu_baseline)
target_link_libraries(pq_core PRIVATE pq_cpu_baseline)
add_library(pq_cpu_check OBJECT baseline/cpu_startup_check.c)
target_link_libraries(pq_cpu_check PUBLIC pq_core user32)
pq_target_baseline_defaults(pq_cpu_check)

# Allocation trap (T0.3): replacement global operator new/delete. An OBJECT library so the definitions
# always reach the linker ahead of the CRT's. Linked by pq_add_test (always) and pq_add_executable
# (when PQ_ALLOC_TRAP_IN_APPS is ON); see pq/core/alloc_trap.h.
add_library(pq_alloc_trap OBJECT trap/alloc_trap_operators.cpp)
target_link_libraries(pq_alloc_trap PUBLIC pq_core)
pq_target_defaults(pq_alloc_trap)
