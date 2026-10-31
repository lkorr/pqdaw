#include "pq/media/decoder.h"

#include <algorithm>
#include <climits>
#include <optional>
#include <vector>

#include "codecs.h"
#include "file_io.h"
#include "mp3_layout.h"
#include "pq/media/pcm.h"
#include "pq/media/probe.h"

namespace pq::media {

namespace {

constexpr uint64_t kChunkFrames = 4096;  // scratch size of the interleaving decoders

Error decodeError(const std::string& what) { return Error{ErrorCode::Parse, what, {}}; }

// The bytes a decoder reads: its own mapping of the file, or a caller's buffer.
struct Source {
  std::optional<detail::MappedFile> map;
  std::span<const uint8_t> bytes;
};

// Planar destination pointers advanced by `offset` frames.
struct Planar {
  float* p[kMaxDecodeChannels];
  Planar(float* const* dst, uint32_t channels, uint64_t offset) noexcept {
    for (uint32_t c = 0; c < channels; ++c) p[c] = dst[c] + offset;
  }
};

class PcmDecoder final : public Decoder {
public:
  PcmDecoder(Source src, const SampleInfo& info) : src_(std::move(src)) { info_ = info; }

protected:
  Result<uint64_t> readImpl(float* const* dst, uint64_t frames) override {
    const uint8_t* at = src_.bytes.data() + info_.dataOffset + position_ * info_.blockAlign;
    pcmToPlanar(info_.pcm, info_.bigEndian, at, info_.channels, static_cast<std::size_t>(frames), dst);
    return frames;
  }
  Status seekImpl(uint64_t) override { return Ok{}; }

private:
  Source src_;
};

class DrWavDecoder final : public Decoder {
public:
  DrWavDecoder(Source src, const SampleInfo& info) : src_(std::move(src)), heap_(kCodecHeapLimit) { info_ = info; }
  ~DrWavDecoder() override {
    if (open_) drwav_uninit(&wav_);
  }
  Status init() {
    auto cb = heap_.callbacks<drwav_allocation_callbacks>();
    if (!drwav_init_memory(&wav_, src_.bytes.data(), src_.bytes.size(), &cb)) return decodeError("WAV: cannot decode");
    open_ = true;
    if (wav_.channels != info_.channels) return decodeError("WAV: channel count mismatch");
    info_.frames = std::min<uint64_t>(info_.frames, wav_.totalPCMFrameCount);
    scratch_.resize(kChunkFrames * info_.channels);
    return Ok{};
  }

protected:
  Result<uint64_t> readImpl(float* const* dst, uint64_t frames) override {
    uint64_t done = 0;
    while (done < frames) {
      const uint64_t want = std::min(kChunkFrames, frames - done);
      const uint64_t got = drwav_read_pcm_frames_f32(&wav_, want, scratch_.data());
      Planar out(dst, info_.channels, done);
      deinterleaveF32(scratch_.data(), info_.channels, static_cast<std::size_t>(got), out.p);
      done += got;
      if (got < want) break;
    }
    return done;
  }
  Status seekImpl(uint64_t frame) override {
    if (!drwav_seek_to_pcm_frame(&wav_, frame)) return decodeError("WAV: seek failed");
    return Ok{};
  }

private:
  Source src_;
  detail::CappedHeap heap_;
  drwav wav_{};
  bool open_ = false;
  std::vector<float> scratch_;
};

class FlacDecoder final : public Decoder {
public:
  FlacDecoder(Source src, const SampleInfo& info) : src_(std::move(src)), heap_(kCodecHeapLimit) { info_ = info; }
  ~FlacDecoder() override {
    if (flac_) drflac_close(flac_);
  }
  Status init() {
    auto cb = heap_.callbacks<drflac_allocation_callbacks>();
    flac_ = drflac_open_memory(src_.bytes.data(), src_.bytes.size(), &cb);
    if (!flac_) return decodeError("FLAC: cannot decode");
    if (flac_->channels != info_.channels) return decodeError("FLAC: channel count mismatch");
    scratch_.resize(kChunkFrames * info_.channels);
    return Ok{};
  }

protected:
  Result<uint64_t> readImpl(float* const* dst, uint64_t frames) override {
    uint64_t done = 0;
    while (done < frames) {
      const uint64_t want = std::min(kChunkFrames, frames - done);
      const uint64_t got = drflac_read_pcm_frames_s32(flac_, want, scratch_.data());
      Planar out(dst, info_.channels, done);
      deinterleaveS32(scratch_.data(), info_.channels, static_cast<std::size_t>(got), out.p);
      done += got;
      if (got < want) break;
    }
    return done;
  }
  Status seekImpl(uint64_t frame) override {
    if (!drflac_seek_to_pcm_frame(flac_, frame)) return decodeError("FLAC: seek failed");
    return Ok{};
  }

private:
  Source src_;
  detail::CappedHeap heap_;
  drflac* flac_ = nullptr;
  std::vector<int32_t> scratch_;
};

// MP3 on dr_mp3's low-level frame decoder (minimp3) with our own frame table: dr_mp3's high-level seek mixes frame
// counts with and without the gapless delay in this version, so seeking is done here, exactly. Output = the frames
// the decoder produces from the first audio frame (after a Xing/Info frame), minus the LAME delay at the start, cut at
// info().frames (which excludes the padding).
class Mp3Decoder final : public Decoder {
public:
  Mp3Decoder(Source src, const SampleInfo& info) : src_(std::move(src)) { info_ = info; }

  Status init() {
    start_ = info_.dataOffset;
    end_ = detail::mp3End(src_.bytes);
    if (start_ >= end_) return decodeError("MP3: no audio frames");
    dec_ = std::make_unique<drmp3dec>();
    drmp3dec_init(dec_.get());
    pcm_.resize(DRMP3_MAX_SAMPLES_PER_FRAME);
    cursor_ = start_;
    return Ok{};
  }

protected:
  Result<uint64_t> readImpl(float* const* dst, uint64_t frames) override {
    const uint32_t ch = info_.channels;
    uint64_t done = 0;
    while (done < frames) {
      if (bufPos_ == bufFrames_) {
        if (!decodeNext()) break;
        continue;
      }
      if (internal_ < info_.delayFrames) {  // gapless: drop the encoder + decoder delay
        const uint64_t skip = std::min<uint64_t>(bufFrames_ - bufPos_, info_.delayFrames - internal_);
        bufPos_ += static_cast<uint32_t>(skip);
        internal_ += skip;
        continue;
      }
      const auto n = static_cast<uint32_t>(std::min<uint64_t>(bufFrames_ - bufPos_, frames - done));
      const float* src = pcm_.data() + std::size_t{bufPos_} * frameChannels_;
      if (frameChannels_ == ch) {
        Planar out(dst, ch, done);
        deinterleaveF32(src, ch, n, out.p);
      } else {  // a frame whose channel count differs from the first frame's: map it
        for (uint32_t c = 0; c < ch; ++c)
          for (uint32_t f = 0; f < n; ++f)
            dst[c][done + f] = src[std::size_t{f} * frameChannels_ + std::min(c, frameChannels_ - 1)];
      }
      bufPos_ += n;
      internal_ += n;
      done += n;
    }
    return done;
  }

  Status seekImpl(uint64_t frame) override {
    if (!tableBuilt_) buildTable();
    const uint64_t target = frame + info_.delayFrames;  // decoder frames from the first audio frame
    // The frame holding `target`: the last one with output that starts at or before it.
    auto it = std::upper_bound(table_.begin(), table_.end(), target,
                               [](uint64_t t, const FrameEntry& e) { return t < e.firstSample; });
    std::size_t k = static_cast<std::size_t>(it - table_.begin());
    while (k > 0 && table_[k - 1].samples == 0) --k;
    if (k == 0 || target >= table_[k - 1].firstSample + table_[k - 1].samples) {
      cursor_ = end_;  // at or past the end of the decodable frames: nothing more to read
      bufPos_ = bufFrames_ = 0;
      internal_ = target;
      return Ok{};
    }
    --k;
    // Lead-in: decode from frame j with a fresh decoder so frame k sees the state a sequential decode has: the bit
    // reservoir (≤ 511 bytes of earlier main data) for frames k-2 and k-1, and two correctly decoded frames before k
    // (the IMDCT overlap and the synthesis filterbank both reach back one granule).
    std::size_t j = k;
    uint64_t bytes = 0;
    while (j > 0 && (k - j < 3 || bytes < 2 * DRMP3_MAX_BITRESERVOIR_BYTES)) {
      --j;
      if (k - j > 2) bytes += table_[j + 1].offset - table_[j].offset;
    }
    drmp3dec_init(dec_.get());
    cursor_ = table_[j].offset;
    for (std::size_t f = j; f < k; ++f) (void)decodeOne();
    bufPos_ = bufFrames_ = 0;
    if (!decodeNext()) {
      internal_ = target;
      return Ok{};
    }
    internal_ = target;
    bufPos_ = static_cast<uint32_t>(std::min<uint64_t>(target - table_[k].firstSample, bufFrames_));
    return Ok{};
  }

private:
  struct FrameEntry {
    uint64_t offset;       // byte where decoding this frame starts
    uint64_t firstSample;  // decoder frames output before it
    uint32_t samples;      // frames it outputs (0 while its bit reservoir is incomplete)
  };

  // One frame at cursor_ into pcm_; returns its frame count (0 = no output) or −1 at the end.
  int decodeOne() {
    if (cursor_ >= end_) return -1;
    drmp3dec_frame_info fi{};
    const int avail = static_cast<int>(std::min<uint64_t>(end_ - cursor_, INT_MAX));
    const int n = drmp3dec_decode_frame(dec_.get(), src_.bytes.data() + cursor_, avail, pcm_.data(), &fi);
    if (fi.frame_bytes <= 0) return -1;
    cursor_ += static_cast<uint64_t>(fi.frame_bytes);
    if (n > 0) frameChannels_ = static_cast<uint32_t>(std::clamp(fi.channels, 1, 2));
    return n;
  }
  bool decodeNext() {
    for (;;) {
      const int n = decodeOne();
      if (n < 0) return false;
      if (n > 0) {
        bufFrames_ = static_cast<uint32_t>(n);
        bufPos_ = 0;
        return true;
      }
    }
  }

  // The frame table: one pass with a second decoder that parses without synthesising (built on the first seek and
  // kept). Its sample counts match a sequential decode: both start fresh at the first audio frame.
  void buildTable() {
    tableBuilt_ = true;
    auto scan = std::make_unique<drmp3dec>();
    drmp3dec_init(scan.get());
    uint64_t at = start_, total = 0;
    while (at < end_) {
      drmp3dec_frame_info fi{};
      const int avail = static_cast<int>(std::min<uint64_t>(end_ - at, INT_MAX));
      const int n = drmp3dec_decode_frame(scan.get(), src_.bytes.data() + at, avail, nullptr, &fi);
      if (fi.frame_bytes <= 0) break;
      const auto out = static_cast<uint32_t>(std::max(n, 0));
      table_.push_back({at, total, out});
      total += out;
      at += static_cast<uint64_t>(fi.frame_bytes);
    }
  }

  Source src_;
  std::unique_ptr<drmp3dec> dec_;
  std::vector<float> pcm_;
  uint64_t start_ = 0, end_ = 0, cursor_ = 0;
  uint32_t bufFrames_ = 0, bufPos_ = 0, frameChannels_ = 1;
  uint64_t internal_ = 0;  // decoder frame index of pcm_[bufPos_]
  bool tableBuilt_ = false;
  std::vector<FrameEntry> table_;
};

class VorbisDecoder final : public Decoder {
public:
  VorbisDecoder(Source src, const SampleInfo& info) : src_(std::move(src)) { info_ = info; }
  ~VorbisDecoder() override {
    if (vorbis_) stb_vorbis_close(vorbis_);
  }
  Status init() {
    if (src_.bytes.size() > static_cast<std::size_t>(INT_MAX)) return Error{ErrorCode::Unsupported, "OGG: file over 2 GiB", {}};
    // stb_vorbis allocates from one caller-owned arena: start small, grow up to the codec heap limit.
    for (std::size_t arena = 256u << 10; arena <= kCodecHeapLimit; arena *= 2) {
      arena_.assign(arena, 0);
      stb_vorbis_alloc alloc{arena_.data(), static_cast<int>(arena_.size())};
      int err = 0;
      vorbis_ = stb_vorbis_open_memory(src_.bytes.data(), static_cast<int>(src_.bytes.size()), &err, &alloc);
      if (vorbis_) break;
      if (err != VORBIS_outofmem) return decodeError("OGG: cannot decode (stb_vorbis error " + std::to_string(err) + ")");
    }
    if (!vorbis_) return decodeError("OGG: setup needs more than the codec heap limit");
    const stb_vorbis_info vi = stb_vorbis_get_info(vorbis_);
    if (static_cast<uint32_t>(vi.channels) != info_.channels || vi.sample_rate != info_.sampleRate)
      return decodeError("OGG: format differs from the identification header");
    return Ok{};
  }

protected:
  Result<uint64_t> readImpl(float* const* dst, uint64_t frames) override {
    uint64_t done = 0;
    while (done < frames) {
      const int want = static_cast<int>(std::min<uint64_t>(1u << 20, frames - done));
      Planar out(dst, info_.channels, done);
      const int got = stb_vorbis_get_samples_float(vorbis_, static_cast<int>(info_.channels), out.p, want);
      if (got <= 0) break;
      done += static_cast<uint64_t>(got);
    }
    return done;
  }
  Status seekImpl(uint64_t frame) override {
    if (frame > UINT_MAX) return Error{ErrorCode::Unsupported, "OGG: seek past 2^32 frames", {}};
    if (!stb_vorbis_seek(vorbis_, static_cast<unsigned>(frame))) return decodeError("OGG: seek failed");
    return Ok{};
  }

private:
  Source src_;
  std::vector<char> arena_;
  stb_vorbis* vorbis_ = nullptr;
};

template <class D>
Result<std::unique_ptr<Decoder>> make(Source src, const SampleInfo& info) {
  auto d = std::make_unique<D>(std::move(src), info);
  if (auto s = d->init(); !s) return std::move(s).error();
  return std::unique_ptr<Decoder>(std::move(d));
}

Result<std::unique_ptr<Decoder>> openSource(Source src, const SampleInfo& info) {
  switch (info.container) {
    case Container::Wav:
    case Container::Rf64:
    case Container::W64:
    case Container::Aiff:
    case Container::Aifc:
      if (info.pcm != PcmEncoding::None) return std::unique_ptr<Decoder>(std::make_unique<PcmDecoder>(std::move(src), info));
      return make<DrWavDecoder>(std::move(src), info);
    case Container::Flac: return make<FlacDecoder>(std::move(src), info);
    case Container::Mp3: return make<Mp3Decoder>(std::move(src), info);
    case Container::OggVorbis: return make<VorbisDecoder>(std::move(src), info);
    case Container::Unknown: break;
  }
  return Error{ErrorCode::Unsupported, "no decoder for this file", {}};
}

}  // namespace

Decoder::~Decoder() = default;

Result<std::unique_ptr<Decoder>> Decoder::open(const std::filesystem::path& path) {
  auto m = detail::MappedFile::open(path);
  if (!m) return std::move(m).error();
  auto info = probeBytes(m->bytes(), path.filename().string());
  if (!info) return Error{info.error().code, path.string() + ": " + info.error().message, {}};
  info->identity = m->identity();
  Source src;
  src.bytes = m->bytes();
  src.map = std::move(*m);
  auto d = openSource(std::move(src), *info);
  if (!d) return Error{d.error().code, path.string() + ": " + d.error().message, {}};
  return d;
}

Result<std::unique_ptr<Decoder>> Decoder::openMemory(std::span<const uint8_t> file, std::string_view fileName) {
  auto info = probeBytes(file, fileName);
  if (!info) return std::move(info).error();
  Source src;
  src.bytes = file;
  return openSource(std::move(src), *info);
}

Result<uint64_t> Decoder::read(float* const* dst, uint64_t frames) {
  const uint64_t left = info_.frames - std::min(position_, info_.frames);
  frames = std::min(frames, left);
  if (frames == 0) return uint64_t{0};
  auto got = readImpl(dst, frames);
  if (!got) return got;
  position_ += *got;
  if (*got < frames) truncated_ = true;
  return got;
}

Status Decoder::seek(uint64_t frame) {
  if (frame > info_.frames) return Error{ErrorCode::InvalidArgument, "seek past the end", {}};
  if (auto s = seekImpl(frame); !s) return s;
  position_ = frame;
  truncated_ = false;
  return Ok{};
}

}  // namespace pq::media
