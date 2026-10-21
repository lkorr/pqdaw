pq_add_module(pq_core
  SOURCES
    src/cpu.cpp
    src/alloc_trap.cpp
    src/epoch_reclaimer.cpp
    src/time.cpp
)
# DbgHelp: symbolised stack traces when the allocation trap aborts (system library).
target_link_libraries(pq_core PRIVATE Dbghelp)

# Allocation trap (T0.3): replacement global operator new/delete. An OBJECT library so the definitions
# always reach the linker ahead of the CRT's. Linked by pq_add_test (always) and pq_add_executable
# (when PQ_ALLOC_TRAP_IN_APPS is ON); see pq/core/alloc_trap.h.
add_library(pq_alloc_trap OBJECT trap/alloc_trap_operators.cpp)
target_link_libraries(pq_alloc_trap PUBLIC pq_core)
pq_target_defaults(pq_alloc_trap)
