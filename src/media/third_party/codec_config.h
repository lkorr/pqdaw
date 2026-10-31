/* Configuration of the third-party decoders (T5.1). Included by their implementation files (dr_*.c and
   stb_vorbis_impl.c here) and by the C++ code that calls them (src/media/src/decoder.cpp), so declarations and
   definitions agree. Every decoder reads from memory (a mapped file view or a caller's buffer): no stdio paths. */
#ifndef PQ_MEDIA_CODEC_CONFIG_H
#define PQ_MEDIA_CODEC_CONFIG_H

#define DR_WAV_NO_STDIO
#define DR_WAV_NO_WCHAR
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_WCHAR
#define DR_FLAC_NO_OGG          /* Ogg FLAC is rare: less third-party code to trust */
#define DR_MP3_NO_STDIO
#define DR_MP3_FLOAT_OUTPUT     /* decode to float: the default s16 path would cut MP3 output to 16 bits */

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION

#endif
