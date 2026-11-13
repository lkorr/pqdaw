/* libFLAC 1.5.0's config.h for pqdaw (EX.3): what upstream's config.cmake.h.in yields for MSVC x64 with WITH_OGG,
 * ENABLE_MULTITHREADING and every program off. pqdaw compiles libFLAC's sources itself (pq_flac in
 * cmake/PqDependencies.cmake) instead of running its CMake project, so this file replaces the configure step. */
#pragma once

#define CPU_IS_BIG_ENDIAN 0
#define WORDS_BIGENDIAN CPU_IS_BIG_ENDIAN
#define ENABLE_64_BIT_WORDS 1
#define FLAC__ALIGN_MALLOC_DATA 1
#define OGG_FOUND 0
#define FLAC__HAS_OGG 0
#define FLAC__HAS_X86INTRIN 1 /* <intrin.h> */
#define FLAC__HAS_NEONINTRIN 0
#define FLAC__HAS_A64NEONINTRIN 0
#define FLAC__USE_AVX 1 /* runtime-detected; pqdaw's baseline is AVX2 anyway */
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_LROUND 1
#define PACKAGE_VERSION "1.5.0"
