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
  pq_graph_record(module ${name} "${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()

# Target graph for scripts/affected.ps1 (T0.x-buildspeed2): every module, app executable, test and bench records
# its kind and source directories here; at the end of the configure step pq_graph_write() resolves their link
# dependencies (whatever target_link_libraries added, wherever) and writes <build>/pq_graph.json. Generated, so
# it cannot go stale.
function(pq_graph_record kind name)
  set(_dirs "")
  foreach(_d IN LISTS ARGN)
    cmake_path(RELATIVE_PATH _d BASE_DIRECTORY "${PQ_SOURCE_DIR}")
    list(APPEND _dirs "${_d}")
  endforeach()
  list(REMOVE_DUPLICATES _dirs)
  set_property(GLOBAL APPEND PROPERTY PQ_GRAPH_TARGETS ${name})
  set_property(GLOBAL PROPERTY PQ_GRAPH_KIND_${name} ${kind})
  set_property(GLOBAL PROPERTY PQ_GRAPH_DIRS_${name} "${_dirs}")
endfunction()

function(pq_graph_write)
  get_property(_targets GLOBAL PROPERTY PQ_GRAPH_TARGETS)
  set(_entries "")
  foreach(_t IN LISTS _targets)
    get_property(_kind GLOBAL PROPERTY PQ_GRAPH_KIND_${_t})
    get_property(_dirs GLOBAL PROPERTY PQ_GRAPH_DIRS_${_t})
    set(_libs "")
    foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_target_property(_l ${_t} ${_prop})
      if(_l)
        list(APPEND _libs ${_l})
      endif()
    endforeach()
    set(_deps "")
    foreach(_l IN LISTS _libs)
      string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _l "${_l}")
      if(_l IN_LIST _targets AND NOT _l STREQUAL _t)
        list(APPEND _deps "\"${_l}\"")
      endif()
    endforeach()
    list(REMOVE_DUPLICATES _deps)
    list(JOIN _deps ", " _deps)
    set(_d "")
    foreach(_x IN LISTS _dirs)
      list(APPEND _d "\"${_x}\"")
    endforeach()
    list(JOIN _d ", " _d)
    get_property(_src GLOBAL PROPERTY PQ_GRAPH_SOURCES_${_t})
    list(APPEND _entries "  \"${_t}\": {\"kind\": \"${_kind}\", \"dirs\": [${_d}], \"deps\": [${_deps}], \"sources\": [${_src}]}")
  endforeach()
  list(JOIN _entries ",\n" _entries)
  file(WRITE "${CMAKE_BINARY_DIR}/pq_graph.json" "{\n${_entries}\n}\n")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL pq_graph_write)

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
  pq_graph_record(executable ${name} "${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()

# Absolute paths of this checkout at run time (cmake/pq_build_paths.h): links pq_build_paths and force-includes
# its header. Never bake a path into a compile definition instead: the compiler cache would hand that object
# to other worktrees (cmake/PqCompilerCache.cmake).
function(pq_use_build_paths target)
  target_link_libraries(${target} PRIVATE pq_build_paths)
  target_compile_options(${target} PRIVATE /FIpq_build_paths.h)
endfunction()

# pq_add_test(pq_tests_core SOURCES test_x.cpp DEPS pq_core [SHARDS n] [SLOW_SHARDS n])
# Every test executable links doctest with a shared main, the allocation trap (an allocation inside an
# RtScope fails the running test case), and registers with CTest.
# Wall-clock performance checks that depend on machine load live in doctest cases decorated with
# `* doctest::test_suite("perf")` (T0.x-flake). The default CTest entry excludes that suite; with PERF, a second
# entry `<name>_perf` runs only it, labelled `perf`, which the test presets exclude (run: ctest -L perf).
# Shards (T0.x-buildspeed2): SHARDS n registers the default entry as n CTest entries `<name>_s<k>` (k = 0..n-1),
# SLOW_SHARDS n the slow entry as `<name>_slow_s<k>`; each runs its share of the cases (--pq-shard, the shared
# test main), so CTest runs them in parallel. Use it where one entry would run longer than ~30 s in `check`.
function(pq_add_test name)
  cmake_parse_arguments(A "PERF" "SHARDS;SLOW_SHARDS" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${A_SOURCES} ${PQ_SOURCE_DIR}/tests/doctest_main.cpp)
  target_link_libraries(${name} PRIVATE doctest::doctest pq_alloc_trap ${A_DEPS})
  target_compile_definitions(${name} PRIVATE PQ_ALLOC_TRAP=1)
  pq_use_build_paths(${name})
  target_compile_definitions(${name} PRIVATE PQ_TEST_DATA_DIR=pq_build_test_data_dir)
  pq_target_defaults(${name})
  set(_dirs "")
  set(_files "")
  foreach(_s IN LISTS A_SOURCES)
    cmake_path(ABSOLUTE_PATH _s BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    cmake_path(GET _s PARENT_PATH _p)
    list(APPEND _dirs "${_p}")
    cmake_path(RELATIVE_PATH _s BASE_DIRECTORY "${PQ_SOURCE_DIR}")
    list(APPEND _files "\"${_s}\"")
  endforeach()
  pq_graph_record(test ${name} ${_dirs})
  list(JOIN _files ", " _files)
  set_property(GLOBAL PROPERTY PQ_GRAPH_SOURCES_${name} "${_files}")
  # Test tiers (T0.x-buildspeed): heavy sweeps are tagged `* doctest::test_suite("slow")` and run as a second
  # CTest entry `<name>_slow` labelled `slow`, which the test presets (and so scripts/build.ps1) skip unless
  # -Full. Registered for every executable (it runs nothing where no case is tagged), so tagging a case can never
  # drop it from the full run.
  _pq_add_test_shards(${name} ${name} "${A_SHARDS}" "" --no-intro=true --test-suite-exclude=perf,slow)
  _pq_add_test_shards(${name} ${name}_slow "${A_SLOW_SHARDS}" slow --no-intro=true --test-suite=slow)
  if(A_PERF)
    add_test(NAME ${name}_perf COMMAND ${name} --no-intro=true --test-suite=perf)
    set_tests_properties(${name}_perf PROPERTIES LABELS perf RUN_SERIAL TRUE)
  endif()
endfunction()

option(PQ_TEST_SHARDS "Register the SHARDS / SLOW_SHARDS of pq_add_test as separate CTest entries (OFF: one entry each)" ON)
function(_pq_add_test_shards exe entry shards label)
  if(NOT PQ_TEST_SHARDS OR NOT shards OR shards LESS 2)
    add_test(NAME ${entry} COMMAND ${exe} ${ARGN})
    if(label)
      set_tests_properties(${entry} PROPERTIES LABELS ${label})
    endif()
    return()
  endif()
  math(EXPR _last "${shards} - 1")
  foreach(_k RANGE ${_last})
    add_test(NAME ${entry}_s${_k} COMMAND ${exe} ${ARGN} --pq-shard=${_k}/${shards})
    if(label)
      set_tests_properties(${entry}_s${_k} PROPERTIES LABELS ${label})
    endif()
  endforeach()
endfunction()

set(PQ_SOURCE_DIR ${CMAKE_SOURCE_DIR} CACHE INTERNAL "")
