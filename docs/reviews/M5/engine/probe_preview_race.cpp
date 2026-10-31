// M5 review probe: PreviewPlayer::collect() frees an image the audio thread is rendering right now.
// The audio thread applies a Play command (acked_ = seq) and only after the whole process() call publishes using_[].
// A second play() from the UI in that window (browsing quickly) runs collect(), sees the first image with
// lastSeq <= acked, not in the stale using_[], not current_ -> destroys it while the audio thread reads it.
// The window is widened by one long process() call (a resampled 44.1 kHz file); the race is the same at 64 frames.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include "pq/engine/preview_player.h"
#include "pq/media/sample_pool.h"
#include "pq/media/wav_file.h"

namespace fs = std::filesystem;
using namespace pq;

std::atomic<int> gInProc{0};
std::atomic<uint64_t> gCalls{0};
std::atomic<bool> gQuit{false};

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const fs::path dir = fs::temp_directory_path() / "pq_review_m5";
  fs::create_directories(dir);
  const fs::path wav = dir / "race44.wav";
  {
    std::vector<std::vector<float>> ch(2, std::vector<float>(44100 * 40));
    for (size_t i = 0; i < ch[0].size(); ++i) ch[0][i] = ch[1][i] = 0.5f * static_cast<float>(std::sin(0.01 * i));
    if (!media::writeWav(wav, ch, 44100).ok()) return 1;
  }
  media::PoolConfig pc;
  pc.decodedCacheDir = dir / "cache";
  media::SamplePool pool(pc);
  auto s = pool.load(wav);
  if (!s.ok()) return 2;
  engine::PreviewConfig cfg;
  cfg.sampleRate = 48000.0;
  cfg.maxBlockFrames = 1024;
  cfg.streams = &pool.streams();
  engine::PreviewPlayer pp(cfg);
  constexpr uint32_t kBig = 1u << 19;  // one long device callback: ~10 ms of sinc work
  std::vector<float> l(kBig), r(kBig);
  std::thread audio([&] {
    while (!gQuit.load()) {
      float* o[2] = {l.data(), r.data()};
      gInProc.store(1);
      pp.process(o, 2, kBig);
      gCalls.fetch_add(1);
      gInProc.store(0);
      std::this_thread::yield();
    }
  });
  int trials = 0, freedWhileRendering = 0;
  for (int t = 0; t < 20; ++t) {
    pp.stop();
    while (pp.state() == engine::PreviewPlayer::State::Playing) std::this_thread::yield();
    (void)pp.collect();
    const uint64_t cmds0 = pp.stats().commands;
    if (!pp.play(*s, 1.0, {}).ok()) continue;  // image X
    // Wait until the audio thread has applied X's command (stats_.commands is bumped right before apply()).
    while (pp.stats().commands == cmds0) std::this_thread::yield();
    const uint64_t call = gCalls.load();
    std::this_thread::sleep_for(std::chrono::microseconds(200));  // X's first render is under way
    if (gCalls.load() != call || gInProc.load() != 1) continue;  // missed the window
    const size_t heldBefore = pp.collect();  // X (and older images the stale using_[] still protects)
    ++trials;
    if (!pp.play(*s, 2.0, {}).ok()) continue;  // image Y: play() runs collect()
    const size_t held = pp.collect();
    const bool stillSameCall = gCalls.load() == call && gInProc.load() == 1;
    // Y was added: held == heldBefore means X was destroyed.
    if (held == heldBefore && stillSameCall) {
      ++freedWhileRendering;
      // Scribble over the freed heap so the audio thread's dangling Clip* / ClipSample reads show.
      std::vector<std::vector<uint8_t>> junk;
      for (int k = 0; k < 2000; ++k) junk.emplace_back(512, 0xFF);
    }
    std::printf("trial %d: held before %zu, after second play %zu, audio thread still in X's first process() = %d\n", t, heldBefore, held,
                stillSameCall ? 1 : 0);
  }
  gQuit.store(true);
  audio.join();
  std::printf("RESULT: %d of %d trials destroyed image X while the audio thread was rendering it\n", freedWhileRendering,
              trials);
  return 0;
}
