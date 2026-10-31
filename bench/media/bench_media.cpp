// T5.1 / T5.2: media decoding and streaming. Runner benchmarks (pq_bench.h):
//   decode/<fixture>        decode the whole fixture (seek 0 + read in 4096-frame chunks into planar float; the decoder
//                           is opened once): MB/s of decoded float32 output and MB/s of file input, × realtime
//   probe/<fixture>         probe() of the file (map, parse headers, identity): µs per file (warm OS cache)
//   pcm/<encoding> stereo   the interleaved → planar conversion kernel (pq/media/pcm.h): ns per stereo frame
//   hash/content 1 GiB      contentHashBytes on a 1 GiB buffer (three 64 KiB windows); hash/bytes 64 MiB: GB/s
//   stream/span 64          StreamPool::span of 64 frames from a filled ring (the audio thread's per-block cost)
//   stream/cursor x64       64 SampleCursors (streamed samples, the T5.2 64-stream case) each reading 64 frames: one
//                           audio block, ns per block and per stream
#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "pq/media/decoder.h"
#include "pq/media/hash.h"
#include "pq/media/pcm.h"
#include "pq/media/probe.h"
#include "pq/media/stream.h"
#include "pq_bench.h"

using namespace pq;
using namespace pq::media;
namespace fs = std::filesystem;

namespace {

fs::path fixture(const char* name) { return fs::path(PQ_TEST_DATA_DIR) / "media" / "fixtures" / name; }

const char* kFixtures[] = {"wav_pcm16.wav", "wav_pcm24.wav", "wav_f32.wav",   "aiff_pcm24.aiff", "flac_16.flac",
                           "flac_24.flac",  "mp3_cbr320.mp3", "mp3_vbr.mp3", "ogg_vorbis.ogg"};

struct FakeSample {
  SampleView v;
  const SampleView& view() const noexcept { return v; }
};

}  // namespace

int main(int argc, char** argv) {
  bench::Runner r("pq_bench_media");

  for (const char* name : kFixtures) {
    r.add(std::string("decode/") + name, [name](bench::Context& c) {
      auto d = Decoder::open(fixture(name));
      if (!d) {
        std::fprintf(stderr, "%s\n", d.error().message.c_str());
        return;
      }
      Decoder& dec = **d;
      const uint32_t ch = dec.info().channels;
      const uint64_t frames = dec.info().frames;
      std::vector<std::vector<float>> buf(ch, std::vector<float>(frames + 4096));
      std::vector<float*> ptrs(ch);
      auto decodeAll = [&] {
        (void)dec.seek(0);
        uint64_t done = 0;
        for (;;) {
          for (uint32_t i = 0; i < ch; ++i) ptrs[i] = buf[i].data() + done;
          auto got = dec.read(ptrs.data(), 4096);
          if (!got || *got == 0) break;
          done += *got;
        }
        bench::doNotOptimize(buf[0][done / 2]);
      };
      // Per frame first, then derived throughputs from the same timing.
      const bench::Stats s = c.measure(decodeAll, frames, "ns/frame");
      const double fileBytes = static_cast<double>(fs::file_size(fixture(name)));
      auto rate = [&](double ns, double bytesPerFrame) { return bytesPerFrame / ns * 1e3; };  // MB/s
      const double outBpf = 4.0 * ch, inBpf = fileBytes / static_cast<double>(frames);
      bench::Stats out{rate(s.medianNs, outBpf), rate(s.maxNs, outBpf), rate(s.p99Ns, outBpf), rate(s.minNs, outBpf), s.samples};
      bench::Stats in{rate(s.medianNs, inBpf), rate(s.maxNs, inBpf), rate(s.p99Ns, inBpf), rate(s.minNs, inBpf), s.samples};
      const double rtPerNs = 1e9 / static_cast<double>(dec.info().sampleRate);
      bench::Stats rt{rtPerNs / s.medianNs, rtPerNs / s.maxNs, rtPerNs / s.p99Ns, rtPerNs / s.minNs, s.samples};
      c.record(out, "MB/s", "output float32");
      c.record(in, "MB/s", "input file");
      c.record(rt, "x realtime");
    });
  }

  for (const char* name : kFixtures) {
    r.add(std::string("probe/") + name, [name](bench::Context& c) {
      const fs::path p = fixture(name);
      c.measure([&] { bench::doNotOptimize(probe(p)->frames); }, 1, "us/file");
    });
  }

  for (auto [enc, label] : {std::pair{PcmEncoding::S16, "S16"}, {PcmEncoding::S24, "S24"}, {PcmEncoding::S32, "S32"},
                            {PcmEncoding::F32, "F32"}}) {
    r.add(std::string("pcm/") + label + " stereo", [enc = enc](bench::Context& c) {
      constexpr std::size_t kFrames = 4096;  // L1-resident staging, like a decoder chunk
      std::vector<uint8_t> src(kFrames * 2 * pcmBytes(enc) + 64);
      std::mt19937 rng(1);
      for (auto& b : src) b = static_cast<uint8_t>(rng() & 0x3F);
      std::vector<float> l(kFrames), rr(kFrames);
      float* dst[2] = {l.data(), rr.data()};
      c.measure([&] { pcmToPlanar(enc, false, src.data(), 2, kFrames, dst); bench::doNotOptimize(l[17]); }, kFrames,
                "ns/frame");
      c.measure([&] { pcmToPlanar(enc, true, src.data(), 2, kFrames, dst); bench::doNotOptimize(l[17]); }, kFrames,
                "ns/frame", "big endian");
    });
  }

  r.add("hash/content 1 GiB", [](bench::Context& c) {
    std::vector<uint8_t> big(std::size_t{1} << 30, 1);
    c.measure([&] { bench::doNotOptimize(contentHashBytes(big)); }, 1, "us/file");
  });
  r.add("hash/bytes 64 MiB", [](bench::Context& c) {
    std::vector<uint8_t> buf(std::size_t{64} << 20, 3);
    const bench::Stats s = c.measure([&] { bench::doNotOptimize(hashBytes(buf.data(), buf.size(), 1)); }, buf.size(), "ns/byte");
    c.record({1.0 / s.medianNs, 1.0 / s.maxNs, 1.0 / s.p99Ns, 1.0 / s.minNs, s.samples}, "GB/s");
  });

  // The audio thread's side: spans from filled rings (a fake disk side fills them once, then keeps them "full").
  r.add("stream/span 64", [](bench::Context& c) {
    StreamPool pool(1, 2 * 196608);
    VirtualAlloc(pool.ring(0), pool.ringFloats() * sizeof(float), MEM_COMMIT, PAGE_READWRITE);
    const StreamId id = pool.claim(1, 0, Direction::Forward, 2, int64_t{1} << 39);
    int64_t pos = 0;
    c.measure(
        [&] {
          pool.publishFill(0, id.gen, pos + 64);  // keep exactly one block ahead (one store, as the disk thread does)
          const FrameSpan s = pool.span(id, 64);
          bench::doNotOptimize(s.piece[0].ch[0]);
          pos += 64;
        },
        1, "ns/span");
  });
  r.add("stream/cursor x64", [](bench::Context& c) {
    constexpr uint32_t kStreams = 64;
    StreamPool pool(kStreams, 2 * 196608);
    std::vector<float> head(2 * (4096 + 2 * kSampleGuardFrames), 0.25f);
    std::vector<FakeSample> samples(kStreams);
    std::vector<SampleCursor<FakeSample>> cur(kStreams);
    for (uint32_t i = 0; i < kStreams; ++i) {
      VirtualAlloc(pool.ring(i), pool.ringFloats() * sizeof(float), MEM_COMMIT, PAGE_READWRITE);
      FakeSample& s = samples[i];
      s.v.channels = 2;
      s.v.frames = int64_t{1} << 38;
      s.v.residentFrames = 4096;
      s.v.sampleRate = 48000;
      s.v.streamSource = i + 1;
      s.v.data[0] = head.data() + kSampleGuardFrames;
      s.v.data[1] = head.data() + 4096 + 3 * kSampleGuardFrames;
      cur[i].start(pool, s, 4096, Direction::Forward);
    }
    int64_t pos = 4096;
    float acc = 0;
    const bench::Stats s = c.measureBlocks(
        [&] {
          for (uint32_t i = 0; i < kStreams; ++i) {
            pool.publishFill(cur[i].stream().slot, cur[i].stream().gen, pos + 64);
            const FrameSpan sp = cur[i].next(64);
            acc += sp.piece[0].ch[1][63];  // the player reads in place: touch the data once
          }
          pos += 64;
        },
        1, "ns/block");
    bench::doNotOptimize(acc);
    c.record({s.medianNs / kStreams, s.minNs / kStreams, s.p99Ns / kStreams, s.maxNs / kStreams, s.samples}, "ns/stream",
             "per stream");
  });

  return r.main(argc, argv);
}
