#pragma once
// This checkout's absolute paths. Defined in the generated pq_build_paths.cpp (cmake/PqCompilerCache.cmake), a
// translation unit compiled without the compiler cache, because a cached object may come from another worktree.
// Force-included (/FI) into targets that call pq_use_build_paths(); those map their path macros
// (PQ_SOURCE_DIR, PQ_TEST_DATA_DIR, PQ_ASSETS_DIR, PQ_SOURCE_ASSETS_DIR, PQ_ALLOC_TRAP_PROBE, PQC_EXE) onto these arrays.
#ifdef __cplusplus
extern "C" {
#endif
extern const char pq_build_source_dir[];         // <source>
extern const char pq_build_test_data_dir[];      // <source>/tests
extern const char pq_build_source_assets_dir[];  // <source>/assets
extern const char pq_build_alloc_trap_probe[];   // the pq_alloc_trap_abort_probe executable
extern const char pq_build_pqc_exe[];            // the pqc executable
#ifdef __cplusplus
}
#endif
