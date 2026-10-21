cmake_minimum_required(VERSION 3.28)

# Third-party projects often declare old minimum versions; force the policies we rely on.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)  # CMAKE_MSVC_RUNTIME_LIBRARY honoured by subprojects
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)  # option() respects normal variables (FetchContent config)

project(pqdaw VERSION 0.0.1 LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PqToolchain)
include(PqModule)
include(PqDependencies)
include(PqLLVM)

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
add_subdirectory(bench)
