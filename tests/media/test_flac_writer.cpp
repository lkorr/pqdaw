// EX.3: media::FlacWriter (libFLAC, level 5) round-trips through the media decoder (dr_flac) bit-exactly at 16 and
// 24 bits: the generator's reference PCM (on the integer grid) written in odd-sized chunks decodes to the same floats;
// off-grid values round and clamp like WavWriter; a bad format is refused.

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <vector>

#include "media_test_util.h"
#include "pq/media/decoder.h"
#include "pq/media/flac_file.h"

using namespace pq;
using namespace pq::media;
using namespace pq::media::test;

namespace {

fs::path tempFile(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "pq_media_flac_writer";
  fs::create_directories(dir);
  return dir / name;
}

Status writeFlac(const fs::path& p, const Planar& in, uint32_t rate, uint32_t bits, uint32_t chunk) {
  FlacWriter w;
  if (auto s = w.open(p, static_cast<uint32_t>(in.size()), rate, bits); !s) return s;
  std::vector<const float*> at(in.size());
  const size_t frames = in[0].size();
  for (size_t off = 0; off < frames; off += chunk) {
    for (size_t c = 0; c < in.size(); ++c) at[c] = in[c].data() + off;
    if (auto s = w.write(at.data(), static_cast<uint32_t>(std::min<size_t>(chunk, frames - off))); !s) return s;
  }
  CHECK(w.framesWritten() == frames);
  return w.close();
}

Planar decodeFile(const fs::path& p, uint32_t& rate, uint32_t& bits) {
  auto d = Decoder::open(p);
  REQUIRE_MESSAGE(d.ok(), (d.ok() ? "" : d.error().message));
  rate = (*d)->info().sampleRate;
  bits = (*d)->info().bitsPerSample;
  return decodeAll(**d, 1000);
}

}  // namespace

TEST_CASE("flac writer: 16 and 24-bit round trips through dr_flac are bit-exact") {
  struct Case {
    const char* file;
    Planar ref;
    uint32_t bits;
  };
  for (const Case& k : {Case{"rt16.flac", referencePcm16(), 16}, Case{"rt24.flac", referencePcm24(), 24}}) {
    CAPTURE(k.bits);
    const fs::path p = tempFile(k.file);
    REQUIRE(k.ref.size() == 2);
    REQUIRE(writeFlac(p, k.ref, 48000, k.bits, 1237).ok());  // odd chunks: cross libFLAC's 4096-frame blocks
    CHECK(fs::file_size(p) < k.ref[0].size() * 2 * (k.bits / 8));  // compressed
    uint32_t rate = 0, bits = 0;
    const Planar got = decodeFile(p, rate, bits);
    CHECK(rate == 48000);
    CHECK(bits == k.bits);
    REQUIRE(got.size() == 2);
    REQUIRE(got[0].size() == k.ref[0].size());
    size_t diffs = 0;
    for (int c = 0; c < 2; ++c)
      for (size_t i = 0; i < k.ref[c].size(); ++i)
        if (std::memcmp(&got[c][i], &k.ref[c][i], 4) != 0) ++diffs;
    CHECK(diffs == 0);
    fs::remove(p);
  }
}

TEST_CASE("flac writer: off-grid values round to the nearest step and clamp to full scale; bad formats refused") {
  const Planar in{{0.25f / 32768.0f, 0.75f / 32768.0f, -0.75f / 32768.0f, 1.5f, -1.5f, 1.0f}};
  const fs::path p = tempFile("round.flac");
  REQUIRE(writeFlac(p, in, 44100, 16, 4).ok());
  uint32_t rate = 0, bits = 0;
  const Planar got = decodeFile(p, rate, bits);
  REQUIRE(got.size() == 1);
  REQUIRE(got[0].size() == in[0].size());
  CHECK(got[0][0] == 0.0f);
  CHECK(got[0][1] == 1.0f / 32768.0f);
  CHECK(got[0][2] == -1.0f / 32768.0f);
  CHECK(got[0][3] == 32767.0f / 32768.0f);
  CHECK(got[0][4] == -1.0f);
  CHECK(got[0][5] == 32767.0f / 32768.0f);
  fs::remove(p);

  FlacWriter w;
  CHECK_FALSE(w.open(p, 2, 48000, 32).ok());  // 32-bit float is WAV only
  CHECK_FALSE(w.open(p, 9, 48000, 16).ok());
  CHECK_FALSE(w.open(p, 2, 0, 16).ok());
  CHECK_FALSE(w.isOpen());
  CHECK_FALSE(fs::exists(p));
}
