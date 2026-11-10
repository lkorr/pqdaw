// M6 review (warp): renderComplex re-primes a playing voice when its read advances less per call than the resampler's
// reach margin (complexReach = taps + 2 vs sincReach's first = y − center): after complexEnsure compacts the buffer to
// keepFrom = this run's first frame, the next call's `y − reach < vLo` test fires and primeComplex() resets the
// stretcher. Probe: the app's engine block (64) with a pitched-down Complex clip; vocoder chunks (a re-prime recomputes
// ≈ 80), the most in one block, differences against block 1024, and the largest step in the output (a reset's seam).
#include "probe_common.h"

int main() {
  const e::ClipSample s = makeSample(48000 * 12, 2, [](uint32_t c, int64_t i) {
    return static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / kSr + c));
  });
  for (double ratio : {1.0, 0.9, 0.75, 0.7, 0.6, 0.5}) {
    std::vector<std::vector<float>> res;
    std::vector<e::ClipPlayerStats> st;
    std::vector<double> mc;
    for (uint32_t b : {64u, 1024u}) {
      Rig rig(b);
      rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
      e::AudioClipDesc c = warped(e::ClipWarp::Complex, 1, kPpq, kPpq * 12, 120.0);
      c.stretch.pitchRatio = ratio;
      rig.audio({{1, {c}}}, {s});
      rig.play(0);
      res.push_back(rig.render(static_cast<uint64_t>(kSr * 6.0)));
      st.push_back(rig.tl->clipPlayer().stats());
      mc.push_back(rig.worstMcycles);
    }
    size_t diff = 0;
    for (size_t i = 0; i < res[0].size(); ++i) diff += res[0][i] != res[1][i];
    // steps: a pure sine at 220·ratio Hz has max step 0.5·2π·f/sr; report the max step over the body (past 30 ms)
    const double expectStep = 0.5 * 2.0 * std::numbers::pi * 220.0 * ratio / kSr;
    double step64 = 0, step1024 = 0;
    for (size_t i = 24000 + 1440; i + 1 < res[0].size(); ++i) {
      step64 = std::max(step64, static_cast<double>(std::abs(res[0][i + 1] - res[0][i])));
      step1024 = std::max(step1024, static_cast<double>(std::abs(res[1][i + 1] - res[1][i])));
    }
    std::printf("ratio %.2f: block 64: cells %llu (block max %llu, worst %.2f Mcyc); block 1024: cells %llu (block max %llu); "
                "%zu samples differ; max step 64: %.4f 1024: %.4f (sine's own %.4f)\n",
                ratio, static_cast<unsigned long long>(st[0].stretchCells), static_cast<unsigned long long>(st[0].stretchCellsBlockMax),
                mc[0], static_cast<unsigned long long>(st[1].stretchCells), static_cast<unsigned long long>(st[1].stretchCellsBlockMax),
                diff, step64, step1024, expectStep);
  }
  // A 48 kHz file in a 96 kHz engine, no transposition (pitch rate 0.5 from the file rate alone), block 64 vs 1024.
  {
    std::vector<std::vector<float>> res;
    std::vector<e::ClipPlayerStats> st;
    for (uint32_t b : {64u, 1024u}) {
      e::TimelineConfig cfg;
      cfg.sampleRate = 96000.0;
      cfg.maxBlockFrames = b;
      cfg.maxTracks = 8;
      e::Timeline tl(cfg);
      const std::vector<pq::TempoPoint> pts = {{0, 120.0, pq::TempoCurve::Step}};
      auto map = pq::TempoMap::create(pts);
      (void)tl.publishTempo(std::move(e::TempoImage::create(pts, {}).value()));
      e::AudioClipDesc c = warped(e::ClipWarp::Complex, 1, kPpq, kPpq * 12, 120.0);  // fpt for a 48 kHz file
      e::AudioBuildOptions o;
      o.sampleRate = 96000.0;
      auto img = e::AudioSequenceImage::build(std::vector<e::AudioTrackDesc>{{1, {c}}}, {s}, map.value(), o);
      (void)tl.publishAudio(std::move(img.value()));
      e::TransportCommand pc;
      pc.kind = e::TransportCommandKind::Play;
      tl.apply(pc);
      std::vector<float> out;
      int64_t t = 0;
      for (int i = 0; i < 96000 * 6 / static_cast<int>(b); ++i) {
        tl.process(t, b);
        t += b;
        const float* p = tl.clipPlayer().channels()[0];
        out.insert(out.end(), p, p + b);
      }
      res.push_back(std::move(out));
      st.push_back(tl.clipPlayer().stats());
    }
    size_t diff = 0;
    double step64 = 0, step1024 = 0;
    for (size_t i = 0; i < std::min(res[0].size(), res[1].size()); ++i) diff += res[0][i] != res[1][i];
    for (size_t i = 48000 + 2880; i + 1 < std::min(res[0].size(), res[1].size()); ++i) {
      step64 = std::max(step64, static_cast<double>(std::abs(res[0][i + 1] - res[0][i])));
      step1024 = std::max(step1024, static_cast<double>(std::abs(res[1][i + 1] - res[1][i])));
    }
    std::printf("48 kHz file in a 96 kHz engine, ratio 1: block 64: cells %llu (block max %llu); block 1024: cells %llu (block max %llu); "
                "%zu differ; max step 64: %.4f 1024: %.4f (sine's own %.4f)\n",
                static_cast<unsigned long long>(st[0].stretchCells), static_cast<unsigned long long>(st[0].stretchCellsBlockMax),
                static_cast<unsigned long long>(st[1].stretchCells), static_cast<unsigned long long>(st[1].stretchCellsBlockMax), diff,
                step64, step1024, 0.5 * 2.0 * std::numbers::pi * 220.0 / 96000.0);
  }
  return 0;
}
