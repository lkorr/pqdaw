# Prebuilt LLVM (clang+llvm-23.1.3-x86_64-pc-windows-msvc). Provides the pq_llvm interface target.

set(PQ_LLVM_DIR "$ENV{PQ_LLVM_DIR}" CACHE PATH "Root of the extracted prebuilt LLVM")
if(NOT PQ_LLVM_DIR)
  set(PQ_LLVM_DIR "C:/dev/llvm-23.1.3" CACHE PATH "" FORCE)
endif()

# ZLIB::ZLIB and zstd::libzstd_static already exist (PqDependencies.cmake). Stop LLVMConfig from
# searching for system copies or for libxml2 (only needed by LLVMWindowsManifest, which we don't link).
set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_zstd TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_LibXml2 TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_CURL TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_httplib TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_DIASDK TRUE)

# LLVMDebugInfoPDB's link interface names DIASDK::Diaguids. We never link that component, but the
# exported target must resolve at configure time, so provide an empty placeholder.
if(NOT TARGET DIASDK::Diaguids)
  add_library(DIASDK::Diaguids INTERFACE IMPORTED)
endif()

find_package(LLVM 23.1 REQUIRED CONFIG PATHS "${PQ_LLVM_DIR}/lib/cmake/llvm" NO_DEFAULT_PATH)
message(STATUS "pqdaw: LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

llvm_map_components_to_libnames(PQ_LLVM_LIBS
  core support orcjit native passes irreader analysis target transformutils)

add_library(pq_llvm INTERFACE)
target_include_directories(pq_llvm SYSTEM INTERFACE ${LLVM_INCLUDE_DIRS})
separate_arguments(_llvm_defs NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_definitions(pq_llvm INTERFACE ${_llvm_defs})
target_link_libraries(pq_llvm INTERFACE ${PQ_LLVM_LIBS})
# LLVM headers trigger warnings we don't control even as external includes in some TUs.
target_compile_options(pq_llvm INTERFACE /external:W0 /wd4141 /wd4146 /wd4244 /wd4267 /wd4291 /wd4624)
