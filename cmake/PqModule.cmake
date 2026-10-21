# Helpers for declaring pqdaw modules, executables and tests consistently.
#
#   pq_add_module(pq_core
#     SOURCES src/a.cpp src/b.cpp     # omit for header-only (INTERFACE) modules
#     DEPS pq_core                    # public dependencies
#     PRIVATE_DEPS pq_llvm)           # private dependencies
#
# Public headers live in <module dir>/include/pq/<module>/...

function(pq_add_module name)
  cmake_parse_arguments(A "" "" "SOURCES;DEPS;PRIVATE_DEPS" ${ARGN})
  if(A_SOURCES)
    add_library(${name} STATIC ${A_SOURCES})
    target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                       PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(${name} PUBLIC ${A_DEPS} PRIVATE ${A_PRIVATE_DEPS})
    pq_target_defaults(${name})
  else()
    add_library(${name} INTERFACE)
    target_include_directories(${name} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(${name} INTERFACE ${A_DEPS})
  endif()
endfunction()

# Allocation trap (T0.3, pq/core/alloc_trap.h) in app executables: on in dev builds, compiled out of
# release builds (the CRT's operator new/delete are used untouched). Test executables always have it.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(_pq_trap_default OFF)
else()
  set(_pq_trap_default ON)
endif()
option(PQ_ALLOC_TRAP_IN_APPS "Link the RtScope allocation trap into pq_add_executable targets" ${_pq_trap_default})

# Every app executable gets the startup CPU baseline check (pq/core/cpu_baseline.h): it exits with a
# message instead of crashing on a CPU without AVX2/FMA.
function(pq_add_executable name)
  cmake_parse_arguments(A "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${A_SOURCES})
  target_link_libraries(${name} PRIVATE pq_cpu_check ${A_DEPS})
  if(PQ_ALLOC_TRAP_IN_APPS)
    target_link_libraries(${name} PRIVATE pq_alloc_trap)
    target_compile_definitions(${name} PRIVATE PQ_ALLOC_TRAP=1)
  endif()
  pq_target_defaults(${name})
endfunction()

# Absolute paths of this checkout at run time (cmake/pq_build_paths.h): links pq_build_paths and force-includes
# its header. Never bake a path into a compile definition instead: the compiler cache would hand that object
# to other worktrees (cmake/PqCompilerCache.cmake).
function(pq_use_build_paths target)
  target_link_libraries(${target} PRIVATE pq_build_paths)
  target_compile_options(${target} PRIVATE /FIpq_build_paths.h)
endfunction()

# pq_add_test(pq_tests_core SOURCES test_x.cpp DEPS pq_core)
# Every test executable links doctest with a shared main, the allocation trap (an allocation inside an
# RtScope fails the running test case), and registers with CTest.
# Wall-clock performance checks that depend on machine load live in doctest cases decorated with
# `* doctest::test_suite("perf")` (T0.x-flake). The default CTest entry excludes that suite; with PERF, a second
# entry `<name>_perf` runs only it, labelled `perf`, which the test presets exclude (run: ctest -L perf).
function(pq_add_test name)
  cmake_parse_arguments(A "PERF" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${A_SOURCES} ${PQ_SOURCE_DIR}/tests/doctest_main.cpp)
  target_link_libraries(${name} PRIVATE doctest::doctest pq_alloc_trap ${A_DEPS})
  target_compile_definitions(${name} PRIVATE PQ_ALLOC_TRAP=1)
  pq_use_build_paths(${name})
  target_compile_definitions(${name} PRIVATE PQ_TEST_DATA_DIR=pq_build_test_data_dir)
  pq_target_defaults(${name})
  # Test tiers (T0.x-buildspeed): heavy sweeps are tagged `* doctest::test_suite("slow")` and run as a second
  # CTest entry `<name>_slow` labelled `slow`, which the test presets (and so scripts/build.ps1) skip unless
  # -Full. Registered for every executable (it runs nothing where no case is tagged), so tagging a case can never
  # drop it from the full run.
  add_test(NAME ${name} COMMAND ${name} --no-intro=true --test-suite-exclude=perf,slow)
  add_test(NAME ${name}_slow COMMAND ${name} --no-intro=true --test-suite=slow)
  set_tests_properties(${name}_slow PROPERTIES LABELS slow)
  if(A_PERF)
    add_test(NAME ${name}_perf COMMAND ${name} --no-intro=true --test-suite=perf)
    set_tests_properties(${name}_perf PROPERTIES LABELS perf RUN_SERIAL TRUE)
  endif()
endfunction()

set(PQ_SOURCE_DIR ${CMAKE_SOURCE_DIR} CACHE INTERNAL "")
