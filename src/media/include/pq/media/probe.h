#pragma once

// Probing (T5.1): a file's SampleInfo from its headers alone. Not RT-safe (file I/O, allocation); safe to call from
// any non-RT thread concurrently. Cost: one 64 KiB read at the start of the file plus a few small reads (chunk
// headers past a large data chunk; the last 64 KiB of an OGG file for its final granule position). An MP3 without a
// Xing/Info header is the one exception: its frame headers are scanned once (SampleInfo::framesScanned).
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

// Errors: Io (cannot open/read), Unsupported (not an audio file we decode, or an encoding we don't), Parse
// (malformed headers).
Result<SampleInfo> probe(const std::filesystem::path& path);

// The same on a whole file held in memory (tests, fuzzing, drops of in-memory data). `fileName` supplies the
// extension (for a bare MP3) and the file-name BPM; identity stays zero.
Result<SampleInfo> probeBytes(std::span<const uint8_t> file, std::string_view fileName);

// A tempo written in a file name: a number of 20..999 (optionally with one decimal part) directly before or after
// "bpm" (any case), separated by nothing, a space, '_' or '-'. "Loop 120bpm.wav", "drums_98_BPM", "bpm-140 x" →
// 120, 98, 140. The number must not be part of a longer word ("a120bpm" is accepted: letters before the digits
// are a separate token; "1120bpm" is out of range). 0 = none.
double bpmFromFileName(std::string_view name) noexcept;

}  // namespace pq::media
