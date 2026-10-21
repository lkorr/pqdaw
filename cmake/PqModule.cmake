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

function(pq_add_executable name)
  cmake_parse_arguments(A "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${A_SOURCES})
  target_link_libraries(${name} PRIVATE ${A_DEPS})
  pq_target_defaults(${name})
endfunction()

# pq_add_test(pq_tests_core SOURCES test_x.cpp DEPS pq_core)
# Every test executable links doctest with a shared main and registers with CTest.
function(pq_add_test name)
  cmake_parse_arguments(A "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${A_SOURCES} ${PQ_SOURCE_DIR}/tests/doctest_main.cpp)
  target_link_libraries(${name} PRIVATE doctest::doctest ${A_DEPS})
  target_compile_definitions(${name} PRIVATE PQ_TEST_DATA_DIR="${PQ_SOURCE_DIR}/tests")
  pq_target_defaults(${name})
  add_test(NAME ${name} COMMAND ${name} --no-intro=true)
endfunction()

set(PQ_SOURCE_DIR ${CMAKE_SOURCE_DIR} CACHE INTERNAL "")
