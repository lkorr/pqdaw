// M6 review (warp): Complex renders differ with the engine block (probe_blocks A: blocks 32 / 37 differ from 64 by up to
// 1.2). Which feature triggers it (markers, loop, reverse, ramp, pitch), at which blocks, and where the first
// difference is relative to the clip start.
#include "probe_common.h"

static float busy(uint32_t c, int64_t i) {
  const double t = static_cast<double>(i) / kSr;
  uint32_t z = static_cast<uint32_t>(i) * 747796405u + 2891336453u;
  z = ((z >> ((z >> 28u) + 4u)) ^ z) * 277803737u;
  const double noise = static_cast<double>((z >> 22u) ^ z) / 4294967296.0 * 2.0 - 1.0;
  const double env = std::exp(-static_cast<double>(i % 12000) / 2400.0);
  return static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * 233.0 * t + c) + 0.3 * noise * env);
}

int main() {
  const e::ClipSample s = makeSample(48000 * 12, 2, busy);
  const double f100 = fptAt(100.0), f140 = fptAt(140.0), f80 = fptAt(80.0);
  const std::vector<e::WarpSegment> mk = {{0, 0.0, f100}, {kPpq, f100 * kPpq, f140}, {kPpq * 2, f100 * kPpq + f140 * kPpq, f80}};
  struct V {
    const char* name;
    bool seg, loop, rev, ramp;
    double ratio;
    Ticks start;
  } vars[] = {
      {"plain 120/100", false, false, false, false, 1.0, kPpq * 14},
      {"plain start beat 2", false, false, false, false, 1.0, kPpq * 2},
      {"ratio 1.09", false, false, false, false, 1.09, kPpq * 14},
      {"markers", true, false, false, false, 1.0, kPpq * 14},
      {"loop", false, true, false, false, 1.0, kPpq * 14},
      {"reverse", false, false, true, false, 1.0, kPpq * 14},
      {"ramp", false, false, false, true, 1.0, kPpq * 14},
      {"all", true, true, false, true, 1.09, kPpq * 14},
  };
  const uint32_t blocks[] = {64, 16, 32, 37, 48, 128, 200, 333, 512, 1024};
  for (e::ClipWarp w : {e::ClipWarp::Complex, e::ClipWarp::ComplexPro})
    for (const V& v : vars) {
      std::vector<std::vector<float>> res;
      SampleTime s0 = 0;
      for (uint32_t b : blocks) {
        Rig rig(b);
        if (v.ramp) rig.tempo({{0, 97.0, pq::TempoCurve::Linear}, {kPpq * 22, 151.0, pq::TempoCurve::Step}});
        else rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
        e::AudioClipDesc c = warped(w, 5, v.start, kPpq * 3, 100.0);
        if (v.seg) c.segments = mk;
        if (v.loop) c.loop = {true, kPpq / 4, kPpq * 9 / 4};
        c.reverse = v.rev;
        c.stretch.pitchRatio = v.ratio;
        rig.audio({{1, {c}}}, {s});
        rig.play(0);
        s0 = rig.map.ticksToSamples(v.start, kSr);
        res.push_back(rig.render(static_cast<uint64_t>(rig.map.ticksToSamples(v.start + kPpq * 4, kSr))));
      }
      std::printf("%-10s %-18s:", modeName(w), v.name);
      for (size_t k = 1; k < res.size(); ++k) {
        size_t diff = 0, first = SIZE_MAX;
        for (size_t i = 0; i < res[0].size(); ++i)
          if (res[k][i] != res[0][i]) {
            ++diff;
            first = std::min(first, i);
          }
        if (diff == 0) std::printf(" b%u ok", blocks[k]);
        else std::printf(" b%u %zu@%+.1fms", blocks[k], diff, (static_cast<double>(first) - static_cast<double>(s0)) / 48.0);
      }
      std::printf("\n");
    }
  return 0;
}
