// M5 review (media): standalone probes against the release pq_media / pq_dsp / pq_core libs.
//   probe_media poll     a prefetch held while stopped keeps the disk thread polling at 500 Hz (idle claim)
//   probe_media collide  two compressed files with the same size + hash windows share one decoded .f32 cache
//   probe_media pqwf     a corrupt .pqwf header (huge frames) makes loadOverview allocate before validating
//   probe_media xing     an MP3 whose Xing frame count is huge makes buildOverview allocate from the header
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "pq/media/decoder.h"
#include "pq/media/hash.h"
#include "pq/media/sample_pool.h"
#include "pq/media/waveform.h"

namespace fs = std::filesystem;
using namespace pq::media;

static void le16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
static void le32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }

static std::vector<uint8_t> wavHeader(uint16_t tag, uint16_t ch, uint32_t rate, uint16_t bytesPerSample, uint32_t dataBytes) {
  std::vector<uint8_t> h;
  h.insert(h.end(), {'R', 'I', 'F', 'F'});
  le32(h, 36 + dataBytes);
  h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  le32(h, 16);
  le16(h, tag);
  le16(h, ch);
  le32(h, rate);
  le32(h, rate * ch * bytesPerSample);
  le16(h, uint16_t(ch * bytesPerSample));
  le16(h, uint16_t(bytesPerSample * 8));
  h.insert(h.end(), {'d', 'a', 't', 'a'});
  le32(h, dataBytes);
  return h;
}

static void writeFile(const fs::path& p, const std::vector<uint8_t>& b) {
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

static fs::path tmpDir(const char* name) {
  fs::path d = fs::temp_directory_path() / "pq_review_m5" / name;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d);
  return d;
}

static int poll() {
  const fs::path dir = tmpDir("poll");
  const uint32_t frames = 48000u * 200u;  // 200 s stereo 16-bit: 73 MiB decoded > 32 MiB → streamed
  std::vector<uint8_t> b = wavHeader(1, 2, 48000, 2, frames * 4);
  b.resize(b.size() + size_t(frames) * 4, 0x11);
  writeFile(dir / "long.wav", b);
  PoolConfig cfg;
  cfg.decodedCacheDir = dir / "cache";
  SamplePool pool(cfg);
  pool.startDiskThread();
  auto s = pool.load(dir / "long.wav");
  if (!s) { std::printf("load failed: %s\n", s.error().message.c_str()); return 1; }
  std::printf("streamed=%d\n", int((*s)->view().streamed()));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const uint64_t w0 = pool.stats().diskWakeups;
  std::this_thread::sleep_for(std::chrono::seconds(1));
  const uint64_t w1 = pool.stats().diskWakeups;
  std::printf("idle, nothing claimed:            %llu wakeups in 1 s\n", (unsigned long long)(w1 - w0));
  // ProjectLink::stop(): disengage, then prefetchAt(now) → SamplePool::prefetch for the clip under the playhead.
  const bool ok = pool.prefetch(**s, 48000 * 100, Direction::Forward);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the prefetch fills
  const uint64_t w2 = pool.stats().diskWakeups;
  const uint64_t r2 = pool.stats().diskReads;
  std::this_thread::sleep_for(std::chrono::seconds(2));
  const uint64_t w3 = pool.stats().diskWakeups;
  const uint64_t r3 = pool.stats().diskReads;
  std::printf("stopped, one prefetch held (ok=%d): %llu wakeups in 2 s (%llu reads), not engaged\n", int(ok),
              (unsigned long long)(w3 - w2), (unsigned long long)(r3 - r2));
  pool.clearPrefetches();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const uint64_t w4 = pool.stats().diskWakeups;
  std::this_thread::sleep_for(std::chrono::seconds(1));
  std::printf("after clearPrefetches:             %llu wakeups in 1 s\n",
              (unsigned long long)(pool.stats().diskWakeups - w4));
  pool.stopDiskThread();
  s = pq::Error{};
  return 0;
}

// A-law WAV (format tag 6, PcmEncoding::None → dr_wav, cached as .f32 like FLAC / MP3 / OGG): fixed 1 byte per
// frame, so an edit inside the file keeps its size (like a CBR MP3 re-export).
static int collide() {
  const fs::path dir = tmpDir("collide");
  const uint32_t frames = 9000000;  // mono: 34.3 MiB decoded > 32 MiB → streamed, decoded into the .f32 cache
  std::vector<uint8_t> a = wavHeader(6, 1, 48000, 1, frames);
  const size_t hdr = a.size();
  std::mt19937 rng(1);
  for (uint32_t i = 0; i < frames; ++i) a.push_back(uint8_t(rng()));
  std::vector<uint8_t> b = a;
  for (size_t i = 100000; i < 120000; ++i) b[i] ^= 0x55;  // outside [0, 64 KiB), the middle and the last window
  writeFile(dir / "take1.wav", a);
  writeFile(dir / "take2.wav", b);
  std::printf("contentHash take1=%016llx take2=%016llx\n", (unsigned long long)*contentHash(dir / "take1.wav"),
              (unsigned long long)*contentHash(dir / "take2.wav"));
  PoolConfig cfg;
  cfg.decodedCacheDir = dir / "cache";
  SamplePool pool(cfg);
  auto sa = pool.load(dir / "take1.wav");
  auto sb = pool.load(dir / "take2.wav");
  if (!sa || !sb) { std::printf("load failed\n"); return 1; }
  std::printf("distinct entries=%d, take2 streamed=%d, cache files:", int(sa->get() != sb->get()), int((*sb)->view().streamed()));
  for (auto& e : fs::directory_iterator(dir / "cache")) std::printf(" %s", e.path().filename().string().c_str());
  std::printf("\n");
  // take2's resident head against a direct decode of take2.
  auto d = Decoder::open(dir / "take2.wav");
  std::vector<float> ref(131072);
  float* p[1] = {ref.data()};
  (void)(*d)->read(p, ref.size());
  const float* head = (*sb)->view().data[0];
  size_t wrong = 0;
  for (size_t f = 0; f < ref.size(); ++f) wrong += head[f] != ref[f];
  std::printf("take2 head frames that differ from take2's own decode: %zu of %zu (edited frames: %zu)\n", wrong,
              ref.size(), size_t(20000));
  (void)hdr;
  return 0;
}

static int pqwf() {
  const fs::path dir = tmpDir("pqwf");
  std::vector<uint8_t> h;
  h.insert(h.end(), {'P', 'Q', 'W', 'F'});
  le32(h, 1);  // version
  le32(h, 2);  // channels
  le32(h, 48000);
  le32(h, 0); le32(h, 0x10000000u);  // frames = 2^60
  le32(h, 0x9abc); le32(h, 0);       // contentHash
  le32(h, 0); le32(h, 0);            // dataBytes = 0 → the size check passes (file = 64 bytes)
  le32(h, 256);
  le32(h, 4);
  h.resize(64, 0);
  writeFile(dir / "x.pqwf", h);
  try {
    auto o = loadOverview(dir / "x.pqwf", 0x9abc);
    std::printf("loadOverview returned %s\n", o ? "ok" : o.error().message.c_str());
  } catch (const std::exception& e) {
    std::printf("loadOverview THREW %s (uncaught on the analysis worker / browser thread = std::terminate)\n", e.what());
  }
  return 0;
}

static int xing() {
  const fs::path dir = tmpDir("xing");
  // MPEG-1 layer III, 128 kb/s, 44.1 kHz, stereo, no CRC: 417-byte frames; side info 32 → Xing at byte 36.
  std::vector<uint8_t> f;
  for (int k = 0; k < 4; ++k) {
    std::vector<uint8_t> fr(417, 0);
    fr[0] = 0xFF; fr[1] = 0xFB; fr[2] = 0x90; fr[3] = 0x00;
    if (k == 0) {
      std::memcpy(fr.data() + 36, "Xing", 4);
      fr[43] = 0x01;                                   // flags: frames
      fr[44] = fr[45] = fr[46] = fr[47] = 0xFF;        // frames = 2^32 − 1
    }
    f.insert(f.end(), fr.begin(), fr.end());
  }
  writeFile(dir / "tiny.mp3", f);
  auto d = Decoder::open(dir / "tiny.mp3");
  if (!d) { std::printf("open failed: %s\n", d.error().message.c_str()); return 1; }
  std::printf("file %zu bytes, probe frames=%llu (%.1f days at 44.1 kHz)\n", f.size(),
              (unsigned long long)(*d)->info().frames, double((*d)->info().frames) / 44100 / 86400);
  SamplePool pool;
  auto s = pool.load(dir / "tiny.mp3");
  std::printf("SamplePool::load: %s\n", s ? "ok" : s.error().message.c_str());
  OverviewRequest r;
  r.path = dir / "tiny.mp3";
  try {
    auto o = loadOrBuildOverview(r);
    std::printf("loadOrBuildOverview: %s\n", o ? "ok" : o.error().message.c_str());
  } catch (const std::exception& e) {
    std::printf("loadOrBuildOverview THREW %s (BrowserPanel::requestOverview runs it on a detached std::thread)\n", e.what());
  }
  return 0;
}

int main(int argc, char** argv) {
  const std::string m = argc > 1 ? argv[1] : "";
  if (m == "poll") return poll();
  if (m == "collide") return collide();
  if (m == "pqwf") return pqwf();
  if (m == "xing") return xing();
  std::printf("usage: probe_media poll|collide|pqwf|xing\n");
  return 2;
}
