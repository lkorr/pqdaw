#pragma once

// Shared helpers for the media tests (T5.1, T5.2): fixture paths, reference PCM, the known signal, whole-file decode.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "pq/media/decoder.h"

namespace pq::media::test {

namespace fs = std::filesystem;

inline fs::path fixtureDir() { return fs::path(PQ_TEST_DATA_DIR) / "media" / "fixtures"; }
inline fs::path fixture(const char* name) { return fixtureDir() / name; }

inline std::vector<uint8_t> readBytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline void writeBytes(const fs::path& p, const std::vector<uint8_t>& b) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

using Planar = std::vector<std::vector<float>>;

// Reference planar float from a raw interleaved stereo file written by make_audio_fixtures.py.
template <class T>
Planar referenceRaw(const char* name, float scale) {
  const std::vector<uint8_t> b = readBytes(fixture(name));
  const std::size_t n = b.size() / sizeof(T);
  Planar out(2, std::vector<float>(n / 2));
  for (std::size_t i = 0; i < n; ++i) {
    T v;
    std::memcpy(&v, b.data() + i * sizeof(T), sizeof(T));
    out[i % 2][i / 2] = static_cast<float>(v) * scale;
  }
  return out;
}
inline Planar referencePcm16() { return referenceRaw<int16_t>("ref_pcm16.raw", 1.0f / 32768.0f); }
inline Planar referencePcm24() { return referenceRaw<int32_t>("ref_pcm24.raw", 1.0f / 8388608.0f); }
inline Planar referencePcm32() { return referenceRaw<int32_t>("ref_pcm32.raw", 1.0f / 2147483648.0f); }
inline Planar referenceF32() { return referenceRaw<float>("ref_f32.raw", 1.0f); }

// tools/fixtures/make_audio_fixtures.py's known_signal(), in double.
inline Planar knownSignal(std::size_t total, double rate = 48000.0) {
  Planar out(2, std::vector<float>(total));
  std::vector<double> l(total), r(total);
  const double T = static_cast<double>(total) / rate;
  auto phase = [&](double n, double f0, double f1) {
    const double t = n / rate;
    const double k = std::log(f1 / f0);
    return 2.0 * std::numbers::pi * f0 * T / k * (std::exp(t / T * k) - 1.0);
  };
  for (std::size_t n = 0; n < total; ++n) {
    l[n] = 0.5 * std::sin(phase(static_cast<double>(n), 40.0, 12000.0));
    r[n] = 0.5 * std::sin(phase(static_cast<double>(n), 12000.0, 40.0));
  }
  auto pulse = [](int m) {
    const double x = std::numbers::pi * m / 3.0;
    const double s = m == 0 ? 1.0 : std::sin(x) / x;
    return 0.3 * s * 0.5 * (1.0 + std::cos(std::numbers::pi * m / 16.0));
  };
  for (auto [start, ch] : {std::pair<std::size_t, std::vector<double>*>{1000, &l}, {1750, &r}})
    for (std::size_t p = start; p < total; p += 1500)
      for (int m = -15; m <= 15; ++m) {
        const auto i = static_cast<std::ptrdiff_t>(p) + m;
        if (i >= 0 && static_cast<std::size_t>(i) < total) (*ch)[static_cast<std::size_t>(i)] += pulse(m);
      }
  for (std::size_t n = 0; n < total; ++n) {
    out[0][n] = static_cast<float>(l[n]);
    out[1][n] = static_cast<float>(r[n]);
  }
  return out;
}

// Decodes everything a decoder returns (in `chunk`-frame reads).
inline Planar decodeAll(Decoder& d, uint64_t chunk = 4096, uint64_t limit = 1ull << 26) {
  const uint32_t ch = d.info().channels;
  Planar out(ch);
  std::vector<std::vector<float>> buf(ch, std::vector<float>(chunk));
  std::vector<float*> ptrs(ch);
  for (uint32_t c = 0; c < ch; ++c) ptrs[c] = buf[c].data();
  uint64_t total = 0;
  while (total < limit) {
    auto got = d.read(ptrs.data(), chunk);
    if (!got || *got == 0) break;
    for (uint32_t c = 0; c < ch; ++c) out[c].insert(out[c].end(), buf[c].begin(), buf[c].begin() + static_cast<std::ptrdiff_t>(*got));
    total += *got;
  }
  return out;
}

// SNR in dB of `got` against `ref` over the common length (both channels).
inline double snrDb(const Planar& ref, const Planar& got) {
  double sig = 0, err = 0;
  for (std::size_t c = 0; c < ref.size() && c < got.size(); ++c) {
    const std::size_t n = std::min(ref[c].size(), got[c].size());
    for (std::size_t i = 0; i < n; ++i) {
      sig += double(ref[c][i]) * ref[c][i];
      const double e = double(ref[c][i]) - got[c][i];
      err += e * e;
    }
  }
  return err == 0 ? 999.0 : 10.0 * std::log10(sig / err);
}

}  // namespace pq::media::test
