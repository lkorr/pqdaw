// T5.1: decoding. Lossless fixtures bit-exact against the generator's PCM, lossy fixtures ≥ 40 dB SNR at offset 0
// (gapless MP3: exactly the encoded length), 1000 random seeks per format equal to a sequential decode, the moved
// WAV reader, and the PCM conversion kernels against a scalar reference.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

#include "media_test_util.h"
#include "pq/media/decoder.h"
#include "pq/media/pcm.h"
#include "pq/media/wav_file.h"

using namespace pq;
using namespace pq::media;
using namespace pq::media::test;

namespace {

std::unique_ptr<Decoder> openOrFail(const char* name) {
  auto d = Decoder::open(fixture(name));
  REQUIRE_MESSAGE(d.ok(), (d.ok() ? "" : d.error().message));
  return std::move(*d);
}

void checkBitExact(const char* name, const Planar& ref) {
  CAPTURE(std::string(name));
  auto d = openOrFail(name);
  const Planar got = decodeAll(*d, 1000);  // odd chunk size: crosses the decoders' internal blocks
  REQUIRE(got.size() == 2);
  REQUIRE(got[0].size() == ref[0].size());
  REQUIRE(got[1].size() == ref[1].size());
  std::size_t diffs = 0;
  for (int c = 0; c < 2; ++c)
    for (std::size_t i = 0; i < ref[c].size(); ++i)
      if (std::memcmp(&got[c][i], &ref[c][i], 4) != 0) ++diffs;
  CHECK(diffs == 0);
  CHECK_FALSE(d->truncated());
}

}  // namespace

TEST_CASE("decode: lossless formats are bit-exact against the generator's PCM") {
  const Planar p16 = referencePcm16(), p24 = referencePcm24(), p32 = referencePcm32(), f32 = referenceF32();
  checkBitExact("wav_pcm16.wav", p16);
  checkBitExact("wav_pcm24.wav", p24);
  checkBitExact("wav_pcm32.wav", p32);
  checkBitExact("wav_f32.wav", f32);
  checkBitExact("wavex_pcm24.wav", p24);
  checkBitExact("rf64_pcm16.rf64", p16);
  checkBitExact("aiff_pcm16.aiff", p16);
  checkBitExact("aiff_pcm24.aiff", p24);
  checkBitExact("flac_16.flac", p16);
  checkBitExact("flac_24.flac", p24);
  checkBitExact("meta_smpl_acid.wav", p16);
}

TEST_CASE("decode: lossy formats at offset 0, gapless: MP3 ≥ 40 dB SNR; Vorbis = libvorbis's own decode") {
  const Planar ref = knownSignal(48000);
  for (const char* name : {"mp3_cbr320.mp3", "mp3_vbr.mp3", "ogg_vorbis.ogg"}) {
    CAPTURE(std::string(name));
    const bool vorbis = std::string(name) == "ogg_vorbis.ogg";
    auto d = openOrFail(name);
    const Planar got = decodeAll(*d);
    REQUIRE(got.size() == 2);
    CHECK(got[0].size() == 48000);  // MP3: delay and padding removed (LAME tag); Vorbis: the final granule
    const double snr = snrDb(ref, got);
    MESSAGE(std::string(name) << ": SNR " << snr << " dB against the known signal");
    // libvorbis (the encoder) stays near 39.5 dB at its highest quality on any signal (QUESTIONS Q?-T5.1-a); the
    // decoder itself is checked against libvorbis's decode of the same stream below.
    CHECK(snr >= (vorbis ? 39.0 : 40.0));
    // The alignment is offset 0: one frame early or late is far worse.
    Planar shifted = got;
    for (auto& ch : shifted) ch.insert(ch.begin(), 0.0f);
    CHECK(snrDb(ref, shifted) < snr - 6.0);
    if (vorbis) {
      const Planar lib = referenceRaw<float>("ogg_vorbis_libvorbis_f32.raw", 1.0f);
      REQUIRE(lib[0].size() == 6000);
      Planar head = got;
      for (auto& ch : head) ch.resize(6000);
      const double agree = snrDb(lib, head);
      MESSAGE("stb_vorbis vs libvorbis: " << agree << " dB");
      CHECK(agree >= 100.0);
    }
  }
}

TEST_CASE("decode: 1000 random seeks per format equal the sequential decode") {
  const char* files[] = {"wav_pcm16.wav", "wav_pcm24.wav", "wav_f32.wav", "rf64_pcm16.rf64", "aiff_pcm24.aiff",
                         "flac_16.flac", "flac_24.flac", "mp3_cbr320.mp3", "mp3_vbr.mp3", "ogg_vorbis.ogg"};
  std::mt19937_64 rng(51);
  for (const char* name : files) {
    CAPTURE(std::string(name));
    auto seq = openOrFail(name);
    const Planar all = decodeAll(*seq);
    const uint64_t frames = all[0].size();
    REQUIRE(frames == seq->info().frames);
    auto d = openOrFail(name);
    std::vector<float> l(4096), r(4096);
    float* ptrs[2] = {l.data(), r.data()};
    std::size_t mismatched = 0;
    for (int i = 0; i < 1000; ++i) {
      const uint64_t at = rng() % frames;
      const uint64_t n = 1 + rng() % 4096;
      REQUIRE(d->seek(at).ok());
      auto got = d->read(ptrs, n);
      REQUIRE(got.ok());
      REQUIRE(*got == std::min(n, frames - at));
      if (std::memcmp(l.data(), all[0].data() + at, *got * 4) != 0 ||
          std::memcmp(r.data(), all[1].data() + at, *got * 4) != 0)
        ++mismatched;
    }
    CHECK(mismatched == 0);
    // Seeking to the end reads nothing; past it is an error.
    REQUIRE(d->seek(frames).ok());
    CHECK(*d->read(ptrs, 16) == 0);
    CHECK_FALSE(d->seek(frames + 1).ok());
  }
}

TEST_CASE("decode: the moved WAV reader and the decoder agree") {
  for (const char* name : {"wav_pcm16.wav", "wav_pcm24.wav", "wav_pcm32.wav", "wav_f32.wav", "wavex_pcm24.wav"}) {
    CAPTURE(std::string(name));
    auto w = readWav(fixture(name));
    REQUIRE(w.ok());
    auto d = openOrFail(name);
    const Planar got = decodeAll(*d);
    REQUIRE(w->channels.size() == got.size());
    for (std::size_t c = 0; c < got.size(); ++c) CHECK(w->channels[c] == got[c]);
  }
}

namespace {

// Scalar reference for one sample of each encoding.
float refSample(PcmEncoding e, bool be, const uint8_t* p) {
  uint8_t b[8];
  const uint32_t n = pcmBytes(e);
  for (uint32_t i = 0; i < n; ++i) b[i] = be ? p[n - 1 - i] : p[i];
  switch (e) {
    case PcmEncoding::U8: return (static_cast<int>(b[0]) - 128) / 128.0f;
    case PcmEncoding::S8: return static_cast<int8_t>(b[0]) / 128.0f;
    case PcmEncoding::S16: { int16_t v; std::memcpy(&v, b, 2); return v / 32768.0f; }
    case PcmEncoding::S24: {
      const int32_t v = static_cast<int32_t>(uint32_t{b[0]} << 8 | uint32_t{b[1]} << 16 | uint32_t{b[2]} << 24) >> 8;
      return static_cast<float>(v) / 8388608.0f;
    }
    case PcmEncoding::S32: { int32_t v; std::memcpy(&v, b, 4); return static_cast<float>(v) * (1.0f / 2147483648.0f); }
    case PcmEncoding::F32: { float v; std::memcpy(&v, b, 4); return v; }
    case PcmEncoding::F64: { double v; std::memcpy(&v, b, 8); return static_cast<float>(v); }
    case PcmEncoding::None: break;
  }
  return 0;
}

}  // namespace

TEST_CASE("pcm: conversion kernels equal a scalar reference (every encoding, 1..3 channels, both byte orders)") {
  std::mt19937 rng(7);
  for (PcmEncoding e : {PcmEncoding::U8, PcmEncoding::S8, PcmEncoding::S16, PcmEncoding::S24, PcmEncoding::S32,
                        PcmEncoding::F32, PcmEncoding::F64})
    for (bool be : {false, true})
      for (uint32_t ch : {1u, 2u, 3u})
        for (std::size_t frames : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{8}, std::size_t{9},
                                   std::size_t{64}, std::size_t{1001}}) {
          CAPTURE(static_cast<int>(e));
          CAPTURE(be);
          CAPTURE(ch);
          CAPTURE(frames);
          const uint32_t bytes = pcmBytes(e);
          std::vector<uint8_t> src(frames * ch * bytes);
          for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(rng());
          if (e == PcmEncoding::F32 || e == PcmEncoding::F64)  // finite values only (NaN payloads compare unequal)
            for (std::size_t i = 0; i < frames * ch; ++i) {
              uint8_t* p = src.data() + i * bytes;
              if (e == PcmEncoding::F32) {
                const float v = std::uniform_real_distribution<float>(-1, 1)(rng);
                std::memcpy(p, &v, 4);
                if (be) std::reverse(p, p + 4);
              } else {
                const double v = std::uniform_real_distribution<double>(-1, 1)(rng);
                std::memcpy(p, &v, 8);
                if (be) std::reverse(p, p + 8);
              }
            }
          std::vector<std::vector<float>> out(ch, std::vector<float>(frames + 1, -99.0f));
          std::vector<float*> ptrs(ch);
          for (uint32_t c = 0; c < ch; ++c) ptrs[c] = out[c].data();
          pcmToPlanar(e, be, src.data(), ch, frames, ptrs.data());
          std::size_t bad = 0;
          for (std::size_t f = 0; f < frames; ++f)
            for (uint32_t c = 0; c < ch; ++c)
              if (out[c][f] != refSample(e, be, src.data() + (f * ch + c) * bytes)) ++bad;
          for (uint32_t c = 0; c < ch; ++c) CHECK(out[c][frames] == -99.0f);  // nothing written past the end
          CHECK(bad == 0);
        }
}
