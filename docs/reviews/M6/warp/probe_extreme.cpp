// M6 review (warp) Q2/Q3: extreme warp markers in every mode — a 1-tick segment spanning 10 s of source (rate → huge),
// a 4-beat segment spanning 1 frame (rate → 0), two markers 1 tick / 0.5 frame apart. Non-finite output, peak, the
// largest sample step (clicks), the worst block's wall time against the same clip without the extreme segments.
#include "probe_common.h"

struct M {
  Ticks c;
  double f;
};
static std::vector<e::WarpSegment> segs(const std::vector<M>& m) {
  std::vector<e::WarpSegment> s;
  for (size_t i = 0; i + 1 < m.size(); ++i)
    s.push_back({m[i].c, m[i].f, (m[i + 1].f - m[i].f) / static_cast<double>(m[i + 1].c - m[i].c)});
  // after the last marker: the last segment's slope continues (the segment above already covers it)
  return s;
}

static float src(uint32_t c, int64_t i) {
  const double t = static_cast<double>(i) / kSr;
  return static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * 220.0 * t + c) + 0.2 * std::sin(2.0 * std::numbers::pi * 1234.5 * t));
}

int main() {
  const e::ClipSample s = makeSample(48000 * 16, 2, src);
  const std::vector<M> normal = {{0, 0.0}, {kPpq * 10, 240000.0}};
  const std::vector<M> extreme = {{0, 0.0}, {kPpq * 2, 48000.0}, {kPpq * 2 + 1, 528000.0}, {kPpq * 6, 528001.0}, {kPpq * 8, 576001.0}};
  const std::vector<M> close = {{0, 0.0}, {kPpq * 2, 48000.0}, {kPpq * 2 + 1, 48000.5}, {kPpq * 4, 96000.5}};
  const std::vector<M> longFast = {{0, 0.0}, {kPpq * 2, 48000.0}, {kPpq * 3, 720000.0}};  // 1 beat over 14 s: rate 28
  struct Case {
    const char* name;
    const std::vector<M>* m;
  } cases[] = {{"normal", &normal}, {"extreme", &extreme}, {"close", &close}, {"1 beat = 14 s", &longFast}};
  for (e::ClipWarp w : {e::ClipWarp::RePitch, e::ClipWarp::Beats, e::ClipWarp::Tones, e::ClipWarp::Texture, e::ClipWarp::Complex,
                        e::ClipWarp::ComplexPro}) {
    for (const Case& k : cases) {
      Rig rig(256);
      rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
      e::AudioClipDesc c = warped(w, 1, kPpq, kPpq * 10, 120.0);
      c.segments = segs(*k.m);
      c.stretch.flux = 30.0f;
      c.stretch.pitchRatio = 1.07;
      if (!rig.audio({{1, {c}}}, {s})) continue;
      rig.play(0);
      const auto t0 = std::chrono::steady_clock::now();
      const std::vector<float> o = rig.render(static_cast<uint64_t>(kSr * 6.0));
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      const Stat st = stat(o);
      const e::ClipPlayerStats& ps = rig.tl->clipPlayer().stats();
      std::printf("%-10s %-14s nonfinite %d peak %.3f maxStep %.3f worstBlock %.0f us %.2f Mcyc (block %lld) total %.0f ms cells %llu cellsBlockMax %llu restarts %llu\n",
                  modeName(w), k.name, st.nonfinite, st.peak, st.maxStep, rig.worstBlockUs, rig.worstMcycles, static_cast<long long>(rig.worstBlockIndex), ms,
                  static_cast<unsigned long long>(ps.stretchCells), static_cast<unsigned long long>(ps.stretchCellsBlockMax),
                  static_cast<unsigned long long>(ps.streamRestarts));
    }
  }
  // Tempo extremes for Complex: a 1 BPM and a 999 BPM song tempo on a 120 BPM clip (rate 1/120 and 8.3).
  for (double bpm : {1.0, 20.0, 999.0}) {
    Rig rig(256);
    rig.tempo({{0, bpm, pq::TempoCurve::Step}});
    e::AudioClipDesc c = warped(e::ClipWarp::Complex, 1, 0, kPpq * 32, 120.0);
    if (!rig.audio({{1, {c}}}, {s})) continue;
    rig.play(0);
    const std::vector<float> o = rig.render(static_cast<uint64_t>(kSr * 4.0));
    const Stat st = stat(o);
    std::printf("Complex at %5.0f BPM: nonfinite %d peak %.3f maxStep %.3f worstBlock %.0f us %.2f Mcyc (block %lld)\n", bpm, st.nonfinite, st.peak,
                st.maxStep, rig.worstBlockUs, rig.worstMcycles, static_cast<long long>(rig.worstBlockIndex));
  }
  return 0;
}
