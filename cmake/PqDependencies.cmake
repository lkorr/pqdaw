# Third-party dependencies. Add new ones here (not ad hoc in module CMakeLists) so versions stay in one place,
# and list each in docs/DEPENDENCIES.md with its archive hash.
#
# Every dependency is a release archive (the project's own release asset where it publishes one, else GitHub's
# tag archive) pinned by a SHA-256 URL_HASH: a tag can move, a hash cannot, and a configure needs neither git
# nor a clone (T0.1-fix1). To bump one: change the URL, download it, sha256sum it, and update the hash here
# and in docs/DEPENDENCIES.md.

include(FetchContent)
set(FETCHCONTENT_QUIET ON)
# Every third-party library is linked statically. Forced into the cache before any dependency is declared:
# RtAudio's CMakeLists declares option(BUILD_SHARED_LIBS ... ON), which would otherwise put ON in the cache
# for every dependency configured after it.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "pqdaw links every third-party library statically" FORCE)

# Downloaded archives are kept in one directory shared by every checkout and worktree (T0.x-buildspeed), keyed
# by their SHA-256, so a fresh worktree does not download them again. Each archive is downloaded to a unique
# temporary name, hash-checked, then renamed into place: concurrent configures never see a partial file. What
# is shared is only the verified archive; extraction and builds stay in each build tree (_deps). The compiler
# cache shares the compiled objects.
set(PQ_DOWNLOAD_CACHE "" CACHE PATH
  "Dependency archives shared by every checkout (empty: %LOCALAPPDATA%/pqdaw/downloads, else <build>/_downloads)")
if(PQ_DOWNLOAD_CACHE)
  set(_pq_download_cache "${PQ_DOWNLOAD_CACHE}")
elseif(DEFINED ENV{LOCALAPPDATA})
  file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/pqdaw/downloads" _pq_download_cache)
else()
  set(_pq_download_cache "${CMAKE_BINARY_DIR}/_downloads")
endif()

# pq_fetch_declare(<name> URL <url> [<mirror>...] URL_HASH SHA256=<hash> <other FetchContent_Declare args>)
# FetchContent_Declare with the archive fetched through the shared download cache.
function(pq_fetch_declare name)
  set(_urls "")
  set(_hash "")
  set(_rest "")
  set(_mode "")
  foreach(_arg IN LISTS ARGN)
    if(_arg STREQUAL "URL" OR _arg STREQUAL "URL_HASH")
      set(_mode "${_arg}")
    elseif(_mode STREQUAL "URL" AND NOT _arg MATCHES "^[A-Z_]+$")
      list(APPEND _urls "${_arg}")
    elseif(_mode STREQUAL "URL_HASH")
      if(NOT _arg MATCHES "^SHA256=([0-9a-f]+)$")
        message(FATAL_ERROR "pq_fetch_declare(${name}): URL_HASH must be SHA256=<lowercase hex>")
      endif()
      set(_hash "${CMAKE_MATCH_1}")
      set(_mode "")
    else()
      set(_mode "")
      list(APPEND _rest "${_arg}")
    endif()
  endforeach()
  if(NOT _urls OR NOT _hash)
    message(FATAL_ERROR "pq_fetch_declare(${name}): URL and URL_HASH SHA256=... are required")
  endif()
  list(GET _urls 0 _first)
  get_filename_component(_fname "${_first}" NAME)
  set(_file "${_pq_download_cache}/${_hash}/${_fname}")
  if(NOT EXISTS "${_file}")
    file(MAKE_DIRECTORY "${_pq_download_cache}/${_hash}")
    string(RANDOM LENGTH 16 _tag)
    set(_part "${_file}.${_tag}.part")
    foreach(_url IN LISTS _urls)
      message(STATUS "pqdaw: downloading ${_url}")
      file(DOWNLOAD "${_url}" "${_part}" STATUS _status TLS_VERIFY ON)
      list(GET _status 0 _code)
      if(_code EQUAL 0)
        file(SHA256 "${_part}" _got)
        if(_got STREQUAL _hash)
          file(RENAME "${_part}" "${_file}" RESULT _renamed)
          break()  # a failed rename means a concurrent configure put the same bytes there first
        endif()
        message(WARNING "pqdaw: ${_url}: SHA-256 ${_got}, expected ${_hash}")
      else()
        message(WARNING "pqdaw: ${_url}: download failed: ${_status}")
      endif()
    endforeach()
    file(REMOVE "${_part}")
    if(NOT EXISTS "${_file}")
      message(FATAL_ERROR "pq_fetch_declare(${name}): could not download a verified archive from: ${_urls}")
    endif()
  endif()
  # FetchContent checks the hash of the local file again before extracting it.
  FetchContent_Declare(${name} URL "${_file}" URL_HASH SHA256=${_hash} ${_rest})
endfunction()

# --- doctest (MIT): unit tests --------------------------------------------------------------
pq_fetch_declare(doctest
  URL      https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
  URL_HASH SHA256=632ed2c05a7f53fa961381497bf8069093f0d6628c5f26286161fbd32a560186
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(DOCTEST_NO_INSTALL ON)
FetchContent_MakeAvailable(doctest)

# --- zlib (zlib) + zstd (BSD): required by the prebuilt LLVM's Support library ----------------
# LLVM's exported targets reference ZLIB::ZLIB and zstd::libzstd_static; we provide them.
pq_fetch_declare(zlib
  URL      https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.xz
  URL_HASH SHA256=38ef96b8dfe510d42707d9c781877914792541133e1870841463bfa73f883e32
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  EXCLUDE_FROM_ALL)
set(ZLIB_BUILD_EXAMPLES OFF)
FetchContent_MakeAvailable(zlib)
if(NOT TARGET ZLIB::ZLIB)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

pq_fetch_declare(zstd
  URL      https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
  URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
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

# --- nlohmann/json (MIT): pq_doc project-file schema migrations; JSON reading in tests (fixtures) --------------
# The release's source tarball (single include tree, ~110 KB) instead of a git clone of the full repository.
pq_fetch_declare(nlohmann_json
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
pq_fetch_declare(SDL3
  URL      https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
  URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
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
pq_fetch_declare(VulkanMemoryAllocator
  URL      https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.tar.gz
  URL_HASH SHA256=822aa850c6ce77346ae96a8a1d351d52e77e85929f35363849a0a4e638e0a2a1
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(VulkanMemoryAllocator)
add_library(pq_vma INTERFACE)
target_include_directories(pq_vma SYSTEM INTERFACE ${vulkanmemoryallocator_SOURCE_DIR}/include)
target_link_libraries(pq_vma INTERFACE Vulkan::Vulkan)


# --- RtAudio (MIT-style, see docs/DEPENDENCIES.md): audio device I/O for pq_engine --------------------------
# Release tarball of tag 6.0.1 (2023-08-01), SHA-256 pinned. Static library, /MT via CMAKE_MSVC_RUNTIME_LIBRARY.
# WASAPI only. RtAudio's ASIO backend is never built: it compiles the Steinberg ASIO SDK files RtAudio bundles,
# which are not permissively licensed; pqdaw has its own clean-room ASIO host (pq_engine AsioDevice, T0.6-fix2).
# SYSTEM: RtAudio's headers are third-party and must not see our /W4 /WX.
pq_fetch_declare(rtaudio
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
  set(RTAUDIO_API_ASIO OFF)
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
# The official release tarball: savannah, with the SourceForge mirror as a fallback (identical bytes).
pq_fetch_declare(freetype
  URL      https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz
           https://downloads.sourceforge.net/project/freetype/freetype2/2.14.3/freetype-2.14.3.tar.xz
  URL_HASH SHA256=36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
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
pq_fetch_declare(harfbuzz
  URL      https://github.com/harfbuzz/harfbuzz/releases/download/14.5.1/harfbuzz-14.5.1.tar.xz
  URL_HASH SHA256=7e2fa4e8c7c98e8d8140671f5772542afaaa6acccfbd746506886b6d85f7f8d6
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR  pq-no-cmake   # populate only; the target is defined below
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(harfbuzz)
add_library(pq_harfbuzz STATIC ${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc)
target_include_directories(pq_harfbuzz SYSTEM PUBLIC ${harfbuzz_SOURCE_DIR}/src)
target_compile_definitions(pq_harfbuzz PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(pq_harfbuzz PRIVATE /bigobj /W0 /utf-8)
