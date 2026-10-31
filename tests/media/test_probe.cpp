// T5.1: probing. Every fixture's header fields, the metadata chunks, file-name BPMs, the NTFS identity (kept across
// a rename), and the probe's cost (median ≤ 1 ms per file, warm cache).

#include <doctest/doctest.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "media_test_util.h"
#include "pq/media/decoder.h"
#include "pq/media/probe.h"

using namespace pq;
using namespace pq::media;
using namespace pq::media::test;

namespace {

struct Expect {
  const char* file;
  Container container;
  uint64_t frames;
  uint16_t bits;
  bool isFloat;
  PcmEncoding pcm;
  bool bigEndian;
};

const Expect kFixtures[] = {
    {"wav_pcm16.wav", Container::Wav, 6000, 16, false, PcmEncoding::S16, false},
    {"wav_pcm24.wav", Container::Wav, 6000, 24, false, PcmEncoding::S24, false},
    {"wav_pcm32.wav", Container::Wav, 6000, 32, false, PcmEncoding::S32, false},
    {"wav_f32.wav", Container::Wav, 6000, 32, true, PcmEncoding::F32, false},
    {"wavex_pcm24.wav", Container::Wav, 6000, 24, false, PcmEncoding::S24, false},
    {"rf64_pcm16.rf64", Container::Rf64, 6000, 16, false, PcmEncoding::S16, false},
    {"aiff_pcm16.aiff", Container::Aiff, 6000, 16, false, PcmEncoding::S16, true},
    {"aiff_pcm24.aiff", Container::Aiff, 6000, 24, false, PcmEncoding::S24, true},
    {"flac_16.flac", Container::Flac, 6000, 16, false, PcmEncoding::None, false},
    {"flac_24.flac", Container::Flac, 6000, 24, false, PcmEncoding::None, false},
    {"mp3_cbr320.mp3", Container::Mp3, 48000, 0, false, PcmEncoding::None, false},
    {"mp3_vbr.mp3", Container::Mp3, 48000, 0, false, PcmEncoding::None, false},
    {"ogg_vorbis.ogg", Container::OggVorbis, 48000, 0, false, PcmEncoding::None, false},
};

}  // namespace

TEST_CASE("probe: every fixture's format fields") {
  for (const Expect& e : kFixtures) {
    CAPTURE(std::string(e.file));
    auto info = probe(fixture(e.file));
    REQUIRE_MESSAGE(info.ok(), (info.ok() ? "" : info.error().message));
    CHECK(info->container == e.container);
    CHECK(info->sampleRate == 48000);
    CHECK(info->channels == 2);
    CHECK(info->frames == e.frames);
    CHECK_FALSE(info->framesScanned);
    CHECK(info->bitsPerSample == e.bits);
    CHECK(info->isFloat == e.isFloat);
    CHECK(info->pcm == e.pcm);
    if (e.pcm != PcmEncoding::None) {
      CHECK(info->bigEndian == e.bigEndian);
      CHECK(info->blockAlign == 2 * pcmBytes(e.pcm));
      CHECK(info->dataOffset + e.frames * info->blockAlign <= fs::file_size(fixture(e.file)));
    }
    CHECK(info->identity.hasFileId());
    CHECK(info->identity.size == fs::file_size(fixture(e.file)));
    CHECK(info->identity.mtime > 0);
    // Probing the bytes gives the same fields (identity aside).
    const auto bytes = readBytes(fixture(e.file));
    auto mem = probeBytes(bytes, e.file);
    REQUIRE(mem.ok());
    CHECK(mem->frames == info->frames);
    CHECK(mem->dataOffset == info->dataOffset);
    CHECK_FALSE(mem->identity.hasFileId());
  }
}

TEST_CASE("probe: MP3 gapless information from the LAME tag") {
  for (const char* f : {"mp3_cbr320.mp3", "mp3_vbr.mp3"}) {
    CAPTURE(std::string(f));
    auto info = probe(fixture(f));
    REQUIRE(info.ok());
    CHECK(info->delayFrames >= 529);  // encoder delay + the 529-frame decoder delay
    CHECK(info->paddingFrames > 0);
    // Whole MPEG-1 layer III frames of 1152: delay + frames + padding.
    CHECK((info->delayFrames + info->frames + info->paddingFrames) % 1152 == 0);
  }
}

TEST_CASE("probe: smpl and acid chunks (root key, loop, tempo)") {
  auto info = probe(fixture("meta_smpl_acid.wav"));
  REQUIRE(info.ok());
  CHECK(info->rootKey == 62);
  CHECK(info->rootCents == doctest::Approx(50.0f));
  CHECK(info->hasLoop());
  CHECK(info->loopStart == 1000);
  CHECK(info->loopEnd == 5000);  // smpl's end is inclusive
  CHECK(info->bpm == doctest::Approx(120.5));
  CHECK(info->bpmSource == BpmSource::Acid);
}

TEST_CASE("probe: a BPM in the file name") {
  CHECK(bpmFromFileName("Loop 120bpm.wav") == 120);
  CHECK(bpmFromFileName("drums_98_BPM.flac") == 98);
  CHECK(bpmFromFileName("bpm-140 groove.wav") == 140);
  CHECK(bpmFromFileName("BPM 87.5 pad.wav") == 87.5);
  CHECK(bpmFromFileName("pad 120 BPM") == 120);
  CHECK(bpmFromFileName("120bpm") == 120);
  CHECK(bpmFromFileName("kick.wav") == 0);
  CHECK(bpmFromFileName("1120bpm.wav") == 0);   // out of range
  CHECK(bpmFromFileName("9bpm.wav") == 0);      // out of range
  CHECK(bpmFromFileName("bpmx120.wav") == 0);   // "bpm" not followed by a number
  CHECK(bpmFromFileName("120bpmish.wav") == 0); // "bpm" inside a longer word
  CHECK(bpmFromFileName("track2_174bpm_v3.wav") == 174);

  // Through a probe: a copy of a fixture named with a tempo.
  const fs::path dir = fs::temp_directory_path() / "pq_media_probe_bpm";
  fs::create_directories(dir);
  const fs::path p = dir / "Amen 136 BPM.wav";
  fs::copy_file(fixture("wav_pcm16.wav"), p, fs::copy_options::overwrite_existing);
  auto info = probe(p);
  REQUIRE(info.ok());
  CHECK(info->fileNameBpm == 136);
  CHECK(info->bpm == 136);
  CHECK(info->bpmSource == BpmSource::FileName);
  fs::remove_all(dir);
}

TEST_CASE("probe: the file identity survives a rename and changes with the content") {
  const fs::path dir = fs::temp_directory_path() / "pq_media_probe_identity";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path a = dir / "a.wav";
  const fs::path b = dir / "b.wav";
  fs::copy_file(fixture("wav_pcm24.wav"), a);
  auto before = probe(a);
  REQUIRE(before.ok());
  fs::rename(a, b);
  auto after = probe(b);
  REQUIRE(after.ok());
  CHECK(before->identity == after->identity);
  // Another file has another ID.
  const fs::path c = dir / "c.wav";
  fs::copy_file(fixture("wav_pcm24.wav"), c);
  auto other = probe(c);
  REQUIRE(other.ok());
  CHECK(other->identity.fileId != after->identity.fileId);
  fs::remove_all(dir);
}

TEST_CASE("probe: median cost per file ≤ 1 ms (warm cache)") {
  std::vector<fs::path> files;
  for (const Expect& e : kFixtures) files.push_back(fixture(e.file));
  for (const fs::path& f : files) REQUIRE(probe(f).ok());  // warm the OS cache
  std::vector<double> us;
  for (int round = 0; round < 10; ++round)
    for (const fs::path& f : files) {
      const auto t0 = std::chrono::steady_clock::now();
      auto info = probe(f);
      us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
      REQUIRE(info.ok());
    }
  std::sort(us.begin(), us.end());
  const double median = us[us.size() / 2];
  MESSAGE("probe median " << median << " us, max " << us.back() << " us over " << us.size() << " probes");
  CHECK(median <= 1000.0);
}

TEST_CASE("probe: unsupported and missing files are errors") {
  auto missing = probe(fixtureDir() / "does_not_exist.wav");
  REQUIRE_FALSE(missing.ok());
  CHECK(missing.error().code == ErrorCode::Io);
  auto raw = probe(fixture("ref_pcm16.raw"));  // headerless PCM
  REQUIRE_FALSE(raw.ok());
  CHECK(raw.error().code == ErrorCode::Unsupported);
}

TEST_CASE("probe: an MP3 without a Xing/Info frame is scanned once, flagged, and its length matches the decode") {
  const std::vector<uint8_t> orig = readBytes(fixture("mp3_vbr.mp3"));
  auto tagged = probeBytes(orig, "x.mp3");
  REQUIRE(tagged.ok());
  REQUIRE(tagged->dataOffset > 0);  // the first audio frame follows the Info frame
  const std::vector<uint8_t> bare(orig.begin() + static_cast<std::ptrdiff_t>(tagged->dataOffset), orig.end());
  auto info = probeBytes(bare, "x.mp3");
  REQUIRE(info.ok());
  CHECK(info->framesScanned);
  CHECK(info->delayFrames == 0);
  CHECK(info->paddingFrames == 0);
  CHECK(info->frames == tagged->frames + tagged->delayFrames + tagged->paddingFrames);  // every frame's samples
  auto d = Decoder::openMemory(bare, "x.mp3");
  REQUIRE(d.ok());
  const Planar all = decodeAll(**d);
  CHECK(all[0].size() == info->frames);
  CHECK_FALSE((*d)->truncated());
}


// T5.x-index-progressive: the browser's indexer probes with headersOnly, which never reads the middle of a file.
namespace {

// MPEG-1 layer III, 128 kb/s, 44.1 kHz, stereo, no padding: 417-byte frames of 1152 samples.
constexpr uint8_t kMp3Header[4] = {0xFF, 0xFB, 0x90, 0x00};
constexpr std::size_t kMp3FrameBytes = 417;

void putFrame(uint8_t* p) {
  std::memset(p, 0, kMp3FrameBytes);
  std::memcpy(p, kMp3Header, 4);
}

}  // namespace

TEST_CASE("probe: headersOnly estimates an MP3 without a frame count from its bitrate, no scan") {
  constexpr std::size_t kFrames = 300;
  std::vector<uint8_t> bytes(kFrames * kMp3FrameBytes);
  for (std::size_t f = 0; f < kFrames; ++f) putFrame(bytes.data() + f * kMp3FrameBytes);
  auto exact = probeBytes(bytes, "cbr.mp3");
  REQUIRE(exact.ok());
  CHECK(exact->framesScanned);
  CHECK_FALSE(exact->framesEstimated);
  CHECK(exact->frames == kFrames * 1152);
  auto quick = probeBytes(bytes, "cbr.mp3", ProbeOptions{.headersOnly = true});
  REQUIRE(quick.ok());
  CHECK_FALSE(quick->framesScanned);
  CHECK(quick->framesEstimated);
  CHECK(quick->sampleRate == 44100);
  CHECK(quick->channels == 2);
  // 417 bytes per frame at 128 kb/s is 1149.4 samples per frame by bitrate (the real 417.96 is rounded down): ≤ 0.3 %.
  CHECK(static_cast<double>(quick->frames) == doctest::Approx(static_cast<double>(exact->frames)).epsilon(0.003));
  // A file with a Xing count is exact either way.
  const std::vector<uint8_t> vbr = readBytes(fixture("mp3_vbr.mp3"));
  auto a = probeBytes(vbr, "x.mp3");
  auto b = probeBytes(vbr, "x.mp3", ProbeOptions{.headersOnly = true});
  REQUIRE(a.ok());
  REQUIRE(b.ok());
  CHECK(a->frames == b->frames);
  CHECK_FALSE(b->framesEstimated);
}

// An MP3 whose Xing count is implausible (M5 review H1) used to make the probe scan every frame header: the whole
// file. Here the file's middle is reserved but not committed (PAGE_NOACCESS): any read of it is an access violation,
// so the probe passing proves it read only the first and the last 64 KiB.
TEST_CASE("probe: headersOnly reads only the head and tail of an MP3 with a bogus Xing count") {
  constexpr std::size_t kSize = std::size_t{64} << 20;  // 64 MiB
  constexpr std::size_t kEdge = std::size_t{64} << 10;
  auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE, PAGE_NOACCESS));
  REQUIRE(base != nullptr);
  REQUIRE(VirtualAlloc(base, kEdge, MEM_COMMIT, PAGE_READWRITE) != nullptr);
  REQUIRE(VirtualAlloc(base + kSize - kEdge, kEdge, MEM_COMMIT, PAGE_READWRITE) != nullptr);
  for (std::size_t f = 0; (f + 1) * kMp3FrameBytes <= kEdge; ++f) putFrame(base + f * kMp3FrameBytes);
  // The first frame carries a Xing tag (after the header and 32 bytes of side info) whose count is 2^32 − 1.
  std::memcpy(base + 36, "Xing", 4);
  base[43] = 1;  // flags: frame count present
  std::memset(base + 44, 0xFF, 4);
  {
    DWORD old = 0;  // the head and tail are read-only from here on, as a mapped file would be
    VirtualProtect(base, kEdge, PAGE_READONLY, &old);
    VirtualProtect(base + kSize - kEdge, kEdge, PAGE_READONLY, &old);
  }
  auto info = probeBytes(std::span<const uint8_t>(base, kSize), "bogus.mp3", ProbeOptions{.headersOnly = true});
  REQUIRE(info.ok());
  CHECK(info->framesEstimated);
  CHECK_FALSE(info->framesScanned);
  // The estimate: the audio after the Xing frame at 128 kb/s, capped by the file size like a Xing count.
  const double expected = static_cast<double>(kSize - kMp3FrameBytes) * 8.0 * 44100.0 / 128000.0;
  CHECK(static_cast<double>(info->frames) == doctest::Approx(expected).epsilon(1e-6));
  VirtualFree(base, 0, MEM_RELEASE);
}

TEST_CASE("probe: a file name outside the ANSI code page probes (path::string() threw)") {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("pq_probe_unicode_" + std::to_string(GetCurrentProcessId()));
  std::filesystem::create_directories(dir);
  const std::filesystem::path file = dir / L"Κουφονήσι 八.wav";
  std::filesystem::copy_file(fixture("wav_pcm16.wav"), file, std::filesystem::copy_options::overwrite_existing);
  auto info = probe(file);
  CHECK(info.ok());
  if (info) CHECK(info->frames == 6000);
  auto missing = probe(dir / L"八 missing.wav");
  REQUIRE_FALSE(missing.ok());
  CHECK(missing.error().code == ErrorCode::Io);
  {
    auto d = Decoder::open(file);  // the decoder took the file name the same way
    CHECK(d.ok());
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}
