#pragma once

// Probing (T5.1): a file's SampleInfo from its headers alone. Not RT-safe (file I/O, allocation); safe to call from
// any non-RT thread concurrently. Cost: one 64 KiB read at the start of the file plus a few small reads (chunk
// headers past a large data chunk; the last 64 KiB of an OGG file for its final granule position). Two exceptions get
// the exact length the decoder outputs by reading the whole file: an MP3 without a usable Xing/Info frame count scans
// every frame header once (SampleInfo::framesScanned), and a FLAC stream whose STREAMINFO has no length is decoded.
// ProbeOptions::headersOnly (the browser's indexer) skips both and never reads past the headers and the file's tail:
// such an MP3's length is estimated from its VBRI frame count or its first frame's bitrate × the file size, the FLAC
// stream's is left 0 (unknown); SampleInfo::framesEstimated says so. The decoder and the sample pool always probe
// exactly.
//
// Every size a header declares is checked against the file size: a data chunk that claims more bytes than the file
// holds is cut to what is there, and nothing is allocated from a declared size.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include "pq/core/result.h"
#include "pq/media/sample_info.h"

namespace pq::media {

struct ProbeOptions {
  bool headersOnly = false;  // see above: never scan or decode the whole file; lengths may be estimates
};

// Errors: Io (cannot open/read), Unsupported (not an audio file we decode, or an encoding we don't), Parse
// (malformed headers).
Result<SampleInfo> probe(const std::filesystem::path& path, ProbeOptions options = {});

// The same on a whole file held in memory (tests, fuzzing, drops of in-memory data). `fileName` supplies the
// extension (for a bare MP3) and the file-name BPM; identity stays zero.
Result<SampleInfo> probeBytes(std::span<const uint8_t> file, std::string_view fileName, ProbeOptions options = {});

// A tempo written in a file name: a number of 20..999 (optionally with one decimal part) directly before or after
// "bpm" (any case), separated by nothing, a space, '_' or '-'. "Loop 120bpm.wav", "drums_98_BPM", "bpm-140 x" →
// 120, 98, 140. The number must not be part of a longer word ("a120bpm" is accepted: letters before the digits
// are a separate token; "1120bpm" is out of range). 0 = none.
double bpmFromFileName(std::string_view name) noexcept;

}  // namespace pq::media
