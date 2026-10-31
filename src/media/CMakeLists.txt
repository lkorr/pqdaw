# media (T5.1, T5.2): audio file probing and decoding, the sample pool, RT-safe disk streaming (ARCHITECTURE §1).
# The third-party decoders (dr_wav, dr_flac, dr_mp3, stb_vorbis) are compiled in their own C files with warnings
# off for those files only (pq_media_codecs); pq_media's own code keeps /W4 /WX and sees their headers as SYSTEM.
add_library(pq_media_codecs OBJECT
  third_party/dr_wav.c
  third_party/dr_flac.c
  third_party/dr_mp3.c
  third_party/stb_vorbis_impl.c)
target_include_directories(pq_media_codecs PRIVATE third_party ${dr_libs_SOURCE_DIR} ${stb_SOURCE_DIR})
target_compile_options(pq_media_codecs PRIVATE /W0 /utf-8 /arch:AVX2)
target_compile_definitions(pq_media_codecs PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

pq_add_module(pq_media
  SOURCES
    src/wav_file.cpp
    src/pcm.cpp
    src/hash.cpp
    src/file_io.cpp
    src/probe.cpp
    src/decoder.cpp
  DEPS pq_core pq_dsp
  PRIVATE_DEPS pq_media_codecs
)
target_include_directories(pq_media SYSTEM PRIVATE third_party ${dr_libs_SOURCE_DIR} ${stb_SOURCE_DIR})
