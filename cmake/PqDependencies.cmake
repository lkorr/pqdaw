# Third-party dependencies. Every entry is pinned to an exact tag/commit and listed in docs/DEPENDENCIES.md.
# Add new dependencies here (not ad hoc in module CMakeLists) so versions stay in one place.

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# --- doctest (MIT): unit tests --------------------------------------------------------------
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
  GIT_SHALLOW    TRUE)
set(DOCTEST_NO_INSTALL ON)
FetchContent_MakeAvailable(doctest)

# --- zlib (zlib) + zstd (BSD): required by the prebuilt LLVM's Support library ----------------
# LLVM's exported targets reference ZLIB::ZLIB and zstd::libzstd_static; we provide them.
FetchContent_Declare(zlib
  GIT_REPOSITORY https://github.com/madler/zlib.git
  GIT_TAG        v1.3.1
  GIT_SHALLOW    TRUE
  EXCLUDE_FROM_ALL)
set(ZLIB_BUILD_EXAMPLES OFF)
FetchContent_MakeAvailable(zlib)
if(NOT TARGET ZLIB::ZLIB)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

FetchContent_Declare(zstd
  GIT_REPOSITORY https://github.com/facebook/zstd.git
  GIT_TAG        v1.5.7
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  build/cmake
  EXCLUDE_FROM_ALL)
set(ZSTD_BUILD_PROGRAMS OFF)
set(ZSTD_BUILD_TESTS OFF)
set(ZSTD_BUILD_SHARED OFF)
set(ZSTD_BUILD_STATIC ON)
set(ZSTD_LEGACY_SUPPORT OFF)
FetchContent_MakeAvailable(zstd)
if(NOT TARGET zstd::libzstd_static)
  add_library(zstd::libzstd_static ALIAS libzstd_static)
endif()

# --- nlohmann/json (MIT): JSON reading in tests (fixtures); later project files --------------------------------
# The release's source tarball (single include tree, ~110 KB) instead of a git clone of the full repository.
FetchContent_Declare(nlohmann_json
  URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)
