#include "pq/media/flac_file.h"

#include <cstdio>
#include <string>

#include <FLAC/stream_encoder.h>

#include "pq/media/pcm.h"
#include "file_io.h"  // pathUtf8 (AU.app L3: path::string() throws for names outside the ANSI code page)

namespace pq::media {

namespace {

FLAC__StreamEncoder* encoder(void* p) noexcept { return static_cast<FLAC__StreamEncoder*>(p); }

}  // namespace

FlacWriter::~FlacWriter() {
  if (enc_ != nullptr) (void)close();
}

Status FlacWriter::fail(const char* what) {
  FLAC__StreamEncoder* e = encoder(enc_);
  std::string why = what;
  if (e != nullptr) {
    why += std::string(": ") + FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(e)];
    FLAC__stream_encoder_finish(e);  // closes the file if init_FILE took it
    FLAC__stream_encoder_delete(e);
    enc_ = nullptr;
  }
  return Error{ErrorCode::Io, "FLAC: " + why + " " + detail::pathUtf8(path_), {}};
}

Status FlacWriter::open(const std::filesystem::path& path, uint32_t numChannels, uint32_t sampleRate,
                        uint32_t bitsPerSample) {
  if (enc_ != nullptr) return Error{ErrorCode::InvalidArgument, "FlacWriter: already open", {}};
  if (numChannels == 0 || numChannels > kMaxFlacChannels || sampleRate == 0 ||
      !FLAC__format_sample_rate_is_valid(sampleRate) || (bitsPerSample != 16 && bitsPerSample != 24))
    return Error{ErrorCode::InvalidArgument, "FlacWriter: bad format", {}};
  path_ = path;
  channels_ = numChannels;
  scale_ = bitsPerSample == 16 ? 32768.0 : 8388608.0;
  frames_ = 0;
  FLAC__StreamEncoder* e = FLAC__stream_encoder_new();
  if (e == nullptr) return Error{ErrorCode::Unknown, "FlacWriter: no encoder", {}};
  enc_ = e;
  if (!FLAC__stream_encoder_set_channels(e, numChannels) || !FLAC__stream_encoder_set_bits_per_sample(e, bitsPerSample) ||
      !FLAC__stream_encoder_set_sample_rate(e, sampleRate) ||
      !FLAC__stream_encoder_set_compression_level(e, kFlacCompressionLevel))
    return fail("cannot configure");
  // _wfopen: the path may hold any character (libFLAC's own init_file takes UTF-8 char*). "w+b": the encoder seeks back
  // to rewrite STREAMINFO when it finishes. init_FILE owns the FILE from here (finish closes it).
  FILE* f = _wfopen(path.c_str(), L"w+b");
  if (f == nullptr) {
    FLAC__stream_encoder_delete(e);
    enc_ = nullptr;
    return Error{ErrorCode::Io, "cannot write " + detail::pathUtf8(path), {}};
  }
  if (const FLAC__StreamEncoderInitStatus st = FLAC__stream_encoder_init_FILE(e, f, nullptr, nullptr);
      st != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
    const std::string why = std::string("cannot start: ") + FLAC__StreamEncoderInitStatusString[st];
    FLAC__stream_encoder_delete(e);  // init_FILE took the FILE before it failed: delete (finish) closes it
    enc_ = nullptr;
    return Error{ErrorCode::Io, "FLAC: " + why + " " + detail::pathUtf8(path), {}};
  }
  return Ok{};
}

Status FlacWriter::write(const float* const* channels, uint32_t frames) {
  if (enc_ == nullptr) return Error{ErrorCode::InvalidArgument, "FlacWriter: not open", {}};
  if (frames == 0) return Ok{};
  interleaved_.resize(static_cast<size_t>(frames) * channels_);
  int32_t* o = interleaved_.data();
  for (uint32_t f = 0; f < frames; ++f)
    for (uint32_t c = 0; c < channels_; ++c) *o++ = quantizePcm(channels[c][f], scale_);
  if (!FLAC__stream_encoder_process_interleaved(encoder(enc_), interleaved_.data(), frames))
    return fail("cannot encode");
  frames_ += frames;
  return Ok{};
}

Status FlacWriter::close() {
  if (enc_ == nullptr) return Ok{};
  FLAC__StreamEncoder* e = encoder(enc_);
  const bool ok = FLAC__stream_encoder_finish(e) != 0;  // flushes, rewrites STREAMINFO, closes the file
  const FLAC__StreamEncoderState st = FLAC__stream_encoder_get_state(e);
  FLAC__stream_encoder_delete(e);
  enc_ = nullptr;
  if (!ok)
    return Error{ErrorCode::Io,
                 std::string("FLAC: cannot finish: ") + FLAC__StreamEncoderStateString[st] + " " + detail::pathUtf8(path_), {}};
  return Ok{};
}

}  // namespace pq::media
