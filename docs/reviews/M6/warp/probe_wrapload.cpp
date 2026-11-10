// M6 review (warp): CPU of a 2 s arrangement loop over N Complex tracks (block 64, the app's engine block): per-block
// thread cycles in steady play vs the cue window before each wrap (cue voices prime at 4x the playing rate), and the
// stopped cue after a Locate. Thread cycles, so other load on the machine does not count.
#include "probe_common.h"

int main() {
  const e::ClipSample s = makeSample(48000 * 12, 2, [](uint32_t c, int64_t i) {
    return static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * 233.0 * static_cast<double>(i) / kSr + c));
  });
  for (uint32_t tracks : {1u, 8u, 16u}) {
    Rig rig(64, 64);
    rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
    std::vector<e::AudioTrackDesc> t;
    for (uint32_t k = 0; k < tracks; ++k) t.push_back({1 + k, {warped(e::ClipWarp::Complex, 1 + k, 0, kPpq * 16, 120.0)}});
    rig.audio(t, {s});
    e::TransportCommand lc;
    lc.kind = e::TransportCommandKind::SetLoop;
    lc.loopEnabled = true;
    lc.loopStart = kPpq * 2;
    lc.loopEnd = kPpq * 6;  // 2 s
    rig.tl->apply(lc);
    rig.command(e::TransportCommandKind::Locate, kPpq * 2);
    double stoppedMax = 0, stoppedSum = 0;
    for (int i = 0; i < 300; ++i) {
      rig.worstMcycles = 0;
      rig.step(64);
      stoppedMax = std::max(stoppedMax, rig.worstMcycles);
      stoppedSum += rig.worstMcycles;
    }
    rig.play(kPpq * 2);
    // song position within the loop: block i covers [i·64, i·64+64) samples after loop start (96000-sample loop)
    double steadySum = 0, steadyMax = 0, cueSum = 0, cueMax = 0;
    int steadyN = 0, cueN = 0, over = 0;
    for (int i = 0; i < 96000 * 3 / 64; ++i) {
      rig.worstMcycles = 0;
      rig.step(64);
      const int64_t pos = (static_cast<int64_t>(i) * 64) % 96000;
      const bool cueWin = pos >= 96000 - 24000;  // the last 0.5 s (look-ahead) before the wrap
      if (i < 96000 / 64) continue;              // skip the first pass
      if (cueWin) {
        cueSum += rig.worstMcycles;
        cueMax = std::max(cueMax, rig.worstMcycles);
        ++cueN;
      } else {
        steadySum += rig.worstMcycles;
        steadyMax = std::max(steadyMax, rig.worstMcycles);
        over += rig.worstMcycles > 4.0;
        ++steadyN;
      }
    }
    const e::ClipPlayerStats& ps = rig.tl->clipPlayer().stats();
    std::printf("%2u Complex tracks, block 64 (1.33 ms): steady mean %.3f max %.3f Mcyc; cue window mean %.3f max %.3f Mcyc; "
                "stopped cue mean %.3f max %.3f; steady blocks > 4 Mcyc: %d of %d; late %llu fallbacks %llu\n",
                tracks, steadySum / steadyN, steadyMax, cueSum / cueN, cueMax, stoppedSum / 300, stoppedMax, over, steadyN,
                static_cast<unsigned long long>(ps.stretchLateStarts), static_cast<unsigned long long>(ps.stretchFallbacks));
  }
  return 0;
}
