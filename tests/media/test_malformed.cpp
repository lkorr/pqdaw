// T5.1: malformed input. A corpus (truncations, zero length, absurd chunk sizes and counts, random bytes) returns
// errors or short decodes and never crashes or over-allocates; the `slow` tier runs a 10k-case mutation fuzz per
// format. Allocation is checked through the process's peak commit (PROCESS_MEMORY_COUNTERS::PeakPagefileUsage).

#include <doctest/doctest.h>

#include <windows.h>
#include <psapi.h>

#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "media_test_util.h"
#include "pq/document/samples.h"
#include "pq/media/decoder.h"
#include "pq/media/hash.h"
#include "pq/media/probe.h"

using namespace pq;
using namespace pq::media;
using namespace pq::media::test;

namespace {

std::size_t peakCommit() {
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof pmc;
  GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
  return pmc.PeakPagefileUsage;
}

// Probes and decodes a buffer to the end (bounded), as every consumer would. Returns false if anything failed
// (which is allowed); the point is that it returns at all, without crashing or allocating wildly.
bool exercise(const std::vector<uint8_t>& bytes, const char* name, uint64_t frameBudget = 1u << 22) {
  auto info = probeBytes(bytes, name);
  (void)contentHashBytes(bytes);
  auto d = Decoder::openMemory(bytes, name);
  if (!d) return false;
  const uint32_t ch = (*d)->info().channels;
  REQUIRE(ch >= 1);
  REQUIRE(ch <= kMaxDecodeChannels);
  std::vector<std::vector<float>> buf(ch, std::vector<float>(4096));
  std::vector<float*> ptrs(ch);
  for (uint32_t c = 0; c < ch; ++c) ptrs[c] = buf[c].data();
  uint64_t total = 0;
  while (total < frameBudget) {
    auto got = (*d)->read(ptrs.data(), 4096);
    if (!got) return false;
    if (*got == 0) break;
    total += *got;
  }
  // A seek into the middle and one more read.
  if ((*d)->info().frames > 0) {
    if (!(*d)->seek((*d)->info().frames / 2).ok()) return false;
    (void)(*d)->read(ptrs.data(), 256);
  }
  return true;
}

const char* kFiles[] = {"wav_pcm16.wav", "wav_pcm24.wav",   "wav_f32.wav",   "wavex_pcm24.wav",
                        "rf64_pcm16.rf64", "aiff_pcm16.aiff", "aiff_pcm24.aiff", "flac_16.flac",
                        "flac_24.flac",  "mp3_cbr320.mp3",  "mp3_vbr.mp3",   "ogg_vorbis.ogg",
                        "meta_smpl_acid.wav"};

void put32le(std::vector<uint8_t>& b, std::size_t at, uint32_t v) {
  if (at + 4 <= b.size()) std::memcpy(b.data() + at, &v, 4);
}
void put32be(std::vector<uint8_t>& b, std::size_t at, uint32_t v) {
  if (at + 4 > b.size()) return;
  for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
std::size_t find(const std::vector<uint8_t>& b, const char* tag, std::size_t from = 0) {
  const std::size_t n = std::strlen(tag);
  for (std::size_t i = from; i + n <= b.size(); ++i)
    if (std::memcmp(b.data() + i, tag, n) == 0) return i;
  return std::string::npos;
}

}  // namespace

TEST_CASE("malformed: truncations, zero length, absurd sizes and random bytes never crash or over-allocate") {
  const std::size_t peakBefore = peakCommit();
  std::size_t cases = 0;
  // Zero length and tiny files.
  for (std::size_t n : {0, 1, 3, 4, 11, 12, 44}) {
    std::vector<uint8_t> b(n, 0);
    CHECK_FALSE(exercise(b, "x.wav"));
    CHECK_FALSE(exercise(b, "x.mp3"));
    ++cases;
  }
  for (const char* name : kFiles) {
    CAPTURE(std::string(name));
    const std::vector<uint8_t> orig = readBytes(fixture(name));
    // Truncated at many points (every header byte, then coarser).
    for (std::size_t cut = 0; cut < orig.size(); cut += cut < 160 ? 1 : 4093) {
      CAPTURE(cut);
      std::vector<uint8_t> b(orig.begin(), orig.begin() + static_cast<std::ptrdiff_t>(cut));
      (void)exercise(b, name, 8192);
      ++cases;
    }
    // Every 32-bit word in the first 256 bytes set to absurd values (sizes, counts, rates).
    for (std::size_t at = 0; at + 4 <= std::min<std::size_t>(orig.size(), 256); at += 4)
      for (uint32_t v : {0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0u, 1u}) {
        CAPTURE(at);
        CAPTURE(v);
        std::vector<uint8_t> b = orig;
        put32le(b, at, v);
        (void)exercise(b, name, 8192);
        ++cases;
      }
  }
  // Format-specific absurdities.
  {
    std::vector<uint8_t> b = readBytes(fixture("wav_pcm16.wav"));
    put32le(b, find(b, "data") + 4, 0xFFFFFFF0u);  // data chunk claims 4 GiB
    auto info = probeBytes(b, "x.wav");
    REQUIRE(info.ok());
    CHECK(info->frames == 6000);  // capped by the file size
    CHECK(exercise(b, "x.wav"));
  }
  {
    std::vector<uint8_t> b = readBytes(fixture("aiff_pcm16.aiff"));
    put32be(b, find(b, "COMM") + 10, 0xFFFFFFFFu);  // numSampleFrames
    auto info = probeBytes(b, "x.aiff");
    REQUIRE(info.ok());
    CHECK(info->frames == 6000);
  }
  {
    std::vector<uint8_t> b = readBytes(fixture("flac_16.flac"));
    // STREAMINFO total samples (36 bits) → maximum: the decoder stops where the data ends.
    const std::size_t si = find(b, "fLaC") + 4 + 4;
    b[si + 13] |= 0x0F;
    for (int i = 14; i < 18; ++i) b[si + i] = 0xFF;
    auto d = Decoder::openMemory(b, "x.flac");
    REQUIRE(d.ok());
    const Planar all = decodeAll(**d);
    CHECK(all[0].size() == 6000);
    CHECK((*d)->truncated());
  }
  {
    std::vector<uint8_t> b = readBytes(fixture("mp3_vbr.mp3"));
    put32be(b, find(b, "Xing") + 8, 0x7FFFFFFFu);  // Xing frame count: the decoder ends with the frames
    auto d = Decoder::openMemory(b, "x.mp3");
    REQUIRE(d.ok());
    CHECK((*d)->info().frames > (1ull << 40));
    const Planar all = decodeAll(**d);
    CHECK(all[0].size() > 48000);  // the padding is not cut when the count is wrong
    CHECK(all[0].size() < 48000 + 2304);
    CHECK((*d)->truncated());
    CHECK((*d)->seek(1u << 20).ok());  // past the frames: reads nothing
    float l[16], r[16];
    float* p[2] = {l, r};
    CHECK(*(*d)->read(p, 16) == 0);
  }
  {
    std::vector<uint8_t> b = readBytes(fixture("ogg_vorbis.ogg"));
    // The last page's granule position → 2^40: the decoder ends with the packets.
    std::size_t last = 0;
    for (std::size_t p = find(b, "OggS"); p != std::string::npos; p = find(b, "OggS", p + 1)) last = p;
    const uint64_t g = 1ull << 40;
    std::memcpy(b.data() + last + 6, &g, 8);
    (void)exercise(b, "x.ogg");
  }
  // Random bytes, with and without plausible magic.
  std::mt19937 rng(99);
  for (int i = 0; i < 200; ++i) {
    CAPTURE(i);
    std::vector<uint8_t> b(1 + rng() % 70000);
    for (auto& x : b) x = static_cast<uint8_t>(rng());
    static const char* magics[] = {"", "RIFF", "FORM", "fLaC", "OggS", "ID3", "\xFF\xFB"};
    const char* m = magics[i % 7];
    std::memcpy(b.data(), m, std::min(std::strlen(m), b.size()));
    if (i % 7 == 1 && b.size() >= 12) std::memcpy(b.data() + 8, "WAVE", 4);
    if (i % 7 == 2 && b.size() >= 12) std::memcpy(b.data() + 8, "AIFF", 4);
    (void)exercise(b, i % 2 ? "x.mp3" : "x.bin", 8192);
    ++cases;
  }
  const std::size_t grew = peakCommit() - peakBefore;
  MESSAGE(cases << " malformed cases; peak commit grew by " << grew / (1 << 20) << " MiB");
  CHECK(grew < (256u << 20));
}

TEST_CASE("malformed: mutation fuzz, 10k cases per format" * doctest::test_suite("slow")) {
  const std::size_t peakBefore = peakCommit();
  const char* formats[] = {"wav_pcm24.wav", "rf64_pcm16.rf64", "aiff_pcm16.aiff", "flac_16.flac",
                           "mp3_vbr.mp3",   "ogg_vorbis.ogg",  "meta_smpl_acid.wav"};
  std::mt19937_64 rng(5101);
  for (const char* name : formats) {
    CAPTURE(std::string(name));
    const std::vector<uint8_t> orig = readBytes(fixture(name));
    for (int i = 0; i < 10000; ++i) {
      std::vector<uint8_t> b = orig;
      const int mutations = 1 + static_cast<int>(rng() % 8);
      for (int m = 0; m < mutations && !b.empty(); ++m) {
        // Headers are where parsers break: bias half the edits into the first 1 KiB.
        const std::size_t span = (rng() & 1) ? std::min<std::size_t>(b.size(), 1024) : b.size();
        const std::size_t at = rng() % span;
        switch (rng() % 6) {
          case 0: b[at] = static_cast<uint8_t>(rng()); break;
          case 1: b[at] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
          case 2: put32le(b, at, static_cast<uint32_t>(rng())); break;
          case 3: put32be(b, at, 0xFFFFFFFFu >> (rng() % 32)); break;
          case 4: b.resize(at); break;
          case 5: b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), static_cast<uint8_t>(rng())); break;
        }
      }
      (void)exercise(b, name, 1u << 20);
    }
  }
  const std::size_t grew = peakCommit() - peakBefore;
  MESSAGE("peak commit grew by " << grew / (1 << 20) << " MiB");
  CHECK(grew < (256u << 20));
}

TEST_CASE("hash: the content hash is doc::fileIdentity's hash (one identity per sample), files and bytes agree") {
  const fs::path dir = fs::temp_directory_path() / "pq_media_hash";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  std::mt19937 rng(1);
  // Empty, one partial window, exactly one window, overlapping windows, three separate windows.
  for (std::size_t size : {std::size_t{0}, std::size_t{1000}, std::size_t{65536}, std::size_t{65537},
                           std::size_t{150000}, std::size_t{1} << 20}) {
    CAPTURE(size);
    std::vector<uint8_t> b(size);
    for (auto& x : b) x = static_cast<uint8_t>(rng());
    const fs::path p = dir / ("f" + std::to_string(size) + ".bin");
    writeBytes(p, b);
    auto h = contentHash(p);
    REQUIRE(h.ok());
    CHECK(*h == contentHashBytes(b));
    auto id = doc::fileIdentity(p.string());
    REQUIRE(id.ok());
    CHECK(id->size == size);
    CHECK(id->hash == *h);
  }
  for (const char* name : {"wav_pcm16.wav", "ogg_vorbis.ogg"}) {
    auto id = doc::fileIdentity(fixture(name).string());
    REQUIRE(id.ok());
    CHECK(id->hash == *contentHash(fixture(name)));
  }
  // Only the three windows count: a byte outside them doesn't change the hash, inside does; so does the size.
  std::vector<uint8_t> big(1u << 20);
  for (auto& x : big) x = static_cast<uint8_t>(rng());
  const uint64_t h0 = contentHashBytes(big);
  std::vector<uint8_t> outside = big;
  outside[200000] ^= 1;
  CHECK(contentHashBytes(outside) == h0);
  for (std::size_t at : {std::size_t{10}, big.size() / 2, big.size() - 1}) {
    std::vector<uint8_t> inside = big;
    inside[at] ^= 1;
    CHECK(contentHashBytes(inside) != h0);
  }
  std::vector<uint8_t> longer = big;
  longer.push_back(0);
  CHECK(contentHashBytes(longer) != h0);
  fs::remove_all(dir, ec);
}
