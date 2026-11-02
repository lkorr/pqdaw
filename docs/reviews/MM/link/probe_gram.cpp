// MM review (analysis feeds): two threads on one SpectrumFeed with a spectrogram consumer.
//   writer thread  = the audio thread: fills a 4-channel TapRing (the link's: 16384 frames) at real time, 64-frame blocks
//   feed worker    = SpectrumFeed's own thread (gramPush holds gramMutex_ around dsp::Spectrogram::push)
//   "UI" thread    = main: 60 Hz, gramState() + gramRead() like SpectrogramFace::ingest; times how long each call
//                    waits (gramMutex_ held by the worker) -> a UI frame stall.
// Also: Spectrogram::push of one 4096-frame chunk (the worker's kChunk) on its own, per config.
// Usage: probe_gram [seconds] [fft] [hop]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "pq/core/tap_ring.h"
#include "pq/dsp/spectrogram.h"
#include "pq/views/devices/analysis.h"

using namespace pq;
using Clock = std::chrono::steady_clock;

namespace {
double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

struct NullSink final : dsp::Spectrogram::Sink {
  uint64_t n = 0;
  void column(const float*, uint64_t, double) override { ++n; }
};

void pushCost(uint32_t fft, uint32_t hop) {
  dsp::SpectrogramConfig c;
  c.fftSize = fft;
  c.hop = hop;
  dsp::Spectrogram g(c);
  std::vector<float> x(4096);
  uint64_t t = 0;
  NullSink sink;
  double worst = 0, sum = 0;
  for (int rep = 0; rep < 12; ++rep) {
    for (float& v : x) v = 0.3f * std::sin(0.031 * static_cast<double>(t++)) + 0.1f * std::sin(0.4 * static_cast<double>(t));
    const auto a = Clock::now();
    g.push(x.data(), 4096, sink);
    const double d = ms(a, Clock::now());
    if (rep >= 2) worst = std::max(worst, d), sum += d;
  }
  std::printf("  push(4096) fft %5u hop req %4u (eff %4u, low %5u): mean %6.2f ms worst %6.2f ms\n", fft, hop, g.hop(),
              g.lowFftSize(), sum / 10, worst);
}
}  // namespace

int main(int argc, char** argv) {
  const double seconds = argc > 1 ? std::atof(argv[1]) : 6.0;
  std::printf("== Spectrogram::push of the worker's 4096-frame chunk (held under gramMutex_)\n");
  for (auto [fft, hop] : {std::pair{2048u, 256u}, {2048u, 32u}, {4096u, 32u}, {16384u, 32u}, {16384u, 256u}}) pushCost(fft, hop);

  std::printf("== UI wait on gramState/gramRead while the worker analyses (%.0f s, 48 kHz real time)\n", seconds);
  auto tap = std::make_shared<TapRing>(4, 16384, 64);
  views::devices::SpectrumFeed feed(tap, views::devices::AnalysisHub::config(48000.0));
  feed.setWatch(true);
  const int key = 0;
  feed.acquire(views::devices::kWantSpectrogram);
  views::devices::SpectrogramRequest q;
  q.config.fftSize = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 16384u;  // 16384: the face's largest window (oFft 14)
  q.config.hop = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 32u;  // a short span on a wide display (clamped)
  q.columns = 1024;
  feed.setSpectrogram(&key, q);
  std::atomic<bool> stop{false};
  std::thread audio([&] {
    std::vector<float> l(64), r(64), b(64, 0.0f), p(64, 0.0f);
    const float* ch[4] = {b.data(), p.data(), l.data(), r.data()};
    uint64_t t = 0;
    auto next = Clock::now();
    while (!stop.load()) {
      for (uint32_t i = 0; i < 64; ++i, ++t) l[i] = r[i] = 0.4f * std::sin(0.05 * static_cast<double>(t)) + 0.2f * std::sin(0.71 * static_cast<double>(t));
      tap->write(ch, 4, 64);
      next += std::chrono::microseconds(1333);  // 64 frames at 48 kHz
      std::this_thread::sleep_until(next);
    }
  });
  std::vector<float> rows(16 * views::devices::SpectrumFeed::kGramRows);
  std::vector<views::devices::GramColumn> info(16);
  std::vector<double> waits;
  uint64_t last = 0;
  const auto t0 = Clock::now();
  while (ms(t0, Clock::now()) < seconds * 1000.0) {
    const auto a = Clock::now();
    const views::devices::GramState st = feed.gramState();
    uint64_t from = std::max(last, st.oldest);
    while (from < st.end) {
      const uint32_t got = feed.gramRead(from, 16, rows.data(), info.data());
      if (got == 0) break;
      from += got;
    }
    last = st.end;
    waits.push_back(ms(a, Clock::now()));
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  stop = true;
  audio.join();
  const views::devices::GramState st = feed.gramState();
  feed.clearSpectrogram(&key);
  feed.release(views::devices::kWantSpectrogram);
  std::sort(waits.begin(), waits.end());
  std::printf("  UI frames %zu: ingest median %.3f ms p99 %.3f ms max %.3f ms; frames over 4 ms: %zu; columns %llu, hop %u fft %u\n",
              waits.size(), waits[waits.size() / 2], waits[waits.size() * 99 / 100], waits.back(),
              static_cast<size_t>(std::count_if(waits.begin(), waits.end(), [](double w) { return w > 4.0; })),
              (unsigned long long)st.end, st.hop, st.fft);
  return 0;
}
