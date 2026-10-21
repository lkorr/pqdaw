# Toolchain policy for pqdaw. See docs/ARCHITECTURE.md §5.

if(NOT MSVC)
  message(FATAL_ERROR "pqdaw currently builds with MSVC (cl.exe) only. Run scripts/build.ps1, which enters the VS dev shell.")
endif()

# MSYS2/MinGW is on this machine's PATH (PQ Meter uses it). Its libraries are a different ABI: never find them.
set(CMAKE_IGNORE_PREFIX_PATH "C:/msys64/ucrt64;C:/msys64/mingw64;C:/msys64/clang64;C:/msys64/usr")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The prebuilt LLVM libraries are /MT (static release CRT, _ITERATOR_DEBUG_LEVEL=0).
# Everything we link must match, so every configuration uses the static release CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(FATAL_ERROR "No Debug configuration: LLVM is a release build (/MT). Use the 'dev' preset (RelWithDebInfo, /Od, asserts on).")
endif()

# Flags applied to pqdaw's own targets only (third-party code keeps its own warning level).
set(PQ_CXX_FLAGS
  /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /arch:AVX2 /fp:precise /EHsc
  /wd4324  # structure padded due to alignment specifier: intentional (cache-line padding)
)
set(PQ_CXX_DEFS NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

function(pq_target_defaults target)
  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()
  target_compile_options(${target} PRIVATE ${PQ_CXX_FLAGS})
  target_compile_definitions(${target} PRIVATE ${PQ_CXX_DEFS})
endfunction()
