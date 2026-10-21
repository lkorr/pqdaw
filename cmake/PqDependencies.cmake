# Third-party dependencies. Every entry is pinned to an exact tag/commit and listed in docs/DEPENDENCIES.md.
# Add new dependencies here (not ad hoc in module CMakeLists) so versions stay in one place.

include(FetchContent)
# Git dependencies that need no submodules set GIT_SUBMODULES "": with the Git-for-Windows mingw64 git.exe that
# FindGit can pick, `git submodule update` fails ("unknown subcommand: update-clone").
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
# --- zlib include dirs for direct use (ui PNG encode/decode) ----------------------------------
# zlib 1.3.1's CMake sets directory-scoped include dirs only; zconf.h is generated in its binary dir.
target_include_directories(zlibstatic SYSTEM INTERFACE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})

# --- Vulkan (system SDK, Apache-2.0/MIT headers + loader): ui renderer ------------------------
# Needs $env:VULKAN_SDK (LunarG SDK 1.4.x). glslc compiles the UI shaders at build time.
find_package(Vulkan 1.3 REQUIRED COMPONENTS glslc)

# --- SDL3 (zlib): window, events, Vulkan surface ---------------------------------------------
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG        release-3.4.18
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES ""   # none needed; also avoids a mismatched-git `submodule--helper` failure on this machine
  EXCLUDE_FROM_ALL)
set(SDL_SHARED OFF)
set(SDL_STATIC ON)
set(SDL_TEST_LIBRARY OFF)
set(SDL_TESTS OFF)
set(SDL_EXAMPLES OFF)
set(SDL_INSTALL OFF)
# Subsystems pqdaw does not use (audio is RtAudio, rendering is our own Vulkan code).
set(SDL_AUDIO OFF)
set(SDL_CAMERA OFF)
set(SDL_GPU OFF)
set(SDL_RENDER OFF)
set(SDL_JOYSTICK OFF)
set(SDL_HAPTIC OFF)
set(SDL_HIDAPI OFF)
set(SDL_SENSOR OFF)
FetchContent_MakeAvailable(SDL3)

# --- VulkanMemoryAllocator (MIT): GPU memory for the ui renderer ------------------------------
FetchContent_Declare(VulkanMemoryAllocator
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG        v3.4.0
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES ""
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(VulkanMemoryAllocator)
add_library(pq_vma INTERFACE)
target_include_directories(pq_vma SYSTEM INTERFACE ${vulkanmemoryallocator_SOURCE_DIR}/include)
target_link_libraries(pq_vma INTERFACE Vulkan::Vulkan)


# --- RtAudio (MIT-style, see docs/DEPENDENCIES.md): audio device I/O for pq_engine --------------------------
# Release tarball of tag 6.0.1 (2023-08-01), SHA-256 pinned. Static library, /MT via CMAKE_MSVC_RUNTIME_LIBRARY.
# WASAPI only by default. RtAudio's ASIO backend compiles the Steinberg ASIO SDK files that RtAudio bundles,
# which are not under a permissive licence (docs/QUESTIONS.md, T0.6), so it is opt-in: -DPQ_WITH_ASIO=ON.
# SYSTEM: RtAudio's headers are third-party and must not see our /W4 /WX.
option(PQ_WITH_ASIO "Build RtAudio's ASIO backend (Steinberg ASIO SDK files; not permissively licensed, see docs/QUESTIONS.md)" OFF)
FetchContent_Declare(rtaudio
  URL      https://github.com/thestk/rtaudio/archive/refs/tags/6.0.1.tar.gz
  URL_HASH SHA256=7206c8b6cee43b474f43d64988fefaadfdcfc4264ed38d8de5f5d0e6ddb0a123
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM
  EXCLUDE_FROM_ALL)
block()
  set(RTAUDIO_BUILD_SHARED_LIBS OFF)
  set(RTAUDIO_BUILD_TESTING OFF)
  set(RTAUDIO_BUILD_PYTHON OFF)
  set(RTAUDIO_API_WASAPI ON)
  set(RTAUDIO_API_ASIO ${PQ_WITH_ASIO})
  set(RTAUDIO_API_DS OFF)
  set(RTAUDIO_API_JACK OFF)
  set(RTAUDIO_API_PULSE OFF)
  set(RTAUDIO_API_ALSA OFF)
  set(RTAUDIO_API_OSS OFF)
  set(RTAUDIO_API_CORE OFF)
  # RtAudio's "uninstall" target would collide with SDL3's.
  set(RTAUDIO_TARGETNAME_UNINSTALL rtaudio_uninstall CACHE STRING "Name of RtAudio's uninstall target" FORCE)
  set(CMAKE_POLICY_DEFAULT_CMP0090 NEW)  # export(PACKAGE) must not write to the user's package registry
  FetchContent_MakeAvailable(rtaudio)
endblock()
# Third-party code: silence its warnings instead of patching it.
target_compile_options(rtaudio PRIVATE /w)

# --- FreeType (FTL, chosen over GPLv2) : hinted glyph rasterisation for ui text ----------------------------
# Built static with no optional dependencies: TrueType/CFF hinting and grayscale rendering need none of
# zlib/bzip2/png/brotli/harfbuzz (those cover compressed fonts, colour emoji and FreeType's own autohinter
# shaping, none of which pqdaw uses).
FetchContent_Declare(freetype
  GIT_REPOSITORY https://github.com/freetype/freetype.git
  GIT_TAG        VER-2-14-3
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES ""
  EXCLUDE_FROM_ALL)
set(FT_DISABLE_ZLIB ON)
set(FT_DISABLE_BZIP2 ON)
set(FT_DISABLE_PNG ON)
set(FT_DISABLE_HARFBUZZ ON)
set(FT_DISABLE_BROTLI ON)
set(FT_ENABLE_ERROR_STRINGS OFF)
set(SKIP_INSTALL_ALL ON)
FetchContent_MakeAvailable(freetype)
set_target_properties(freetype PROPERTIES SYSTEM ON)  # its headers are consumed as external (no /W4 /WX)
# FreeType adds a Windows version resource (ftver.rc) even to the static library, where it is useless, and
# rc.exe rejects the command line Ninja generates for it here (RC1107). Drop it.
get_target_property(_pq_ft_srcs freetype SOURCES)
list(FILTER _pq_ft_srcs EXCLUDE REGEX "[.]rc$")
set_target_properties(freetype PROPERTIES SOURCES "${_pq_ft_srcs}")

# --- HarfBuzz (MIT): text shaping (kerning, ligatures, contextual alternates) for ui text ---------------------
# Compiled from the single-file amalgamation (src/harfbuzz.cc) rather than through HarfBuzz's own build: no
# ICU, glib, graphite, subsetter or FreeType integration (shaping uses HarfBuzz's own OpenType functions and
# linear advances; FreeType only rasterises, see src/ui/src/text.cpp).
FetchContent_Declare(harfbuzz
  GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
  GIT_TAG        14.5.1
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES ""
  SOURCE_SUBDIR  pq-no-cmake   # populate only; the target is defined below
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(harfbuzz)
add_library(pq_harfbuzz STATIC ${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc)
target_include_directories(pq_harfbuzz SYSTEM PUBLIC ${harfbuzz_SOURCE_DIR}/src)
target_compile_definitions(pq_harfbuzz PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(pq_harfbuzz PRIVATE /bigobj /W0 /utf-8)
