// M6 review (warp) Q3: block-size identity and Complex priming.
//  A. every mode (markers, loops, reverse, a tempo ramp) at engine blocks 32 / 37 / 333 / 1024 against 64 (the test only
//     compares 64 and 1000); Re-Pitch with markers + loop + ramp is the M5 review's M3 case.
//  B. uncued Play inside a Complex clip (real-time priming, no stream fill) at blocks 32 / 256 / 1024: does the late
//     start (and the audio) depend on the block?
//  C. a Locate while playing into Complex clips at pitch ratio 0.5 / 1 / 2: how late it starts.
//  D. an arrangement loop over Complex clips: is every wrap on time (no late starts), and with 17 / 20 tracks (pool of
//     32 stereo: each wrap needs a cue stretcher besides the playing one)?
#include "probe_common.h"

static float busy(uint32_t c, int64_t i) {
  const double t = static_cast<double>(i) / kSr;
  uint32_t z = static_cast<uint32_t>(i) * 747796405u + 2891336453u;
  z = ((z >> ((z >> 28u) + 4u)) ^ z) * 277803737u;
  const double noise = static_cast<double>((z >> 22u) ^ z) / 4294967296.0 * 2.0 - 1.0;
  const double env = std::exp(-static_cast<double>(i % 12000) / 2400.0);
  return static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * 233.0 * t + c) + 0.3 * noise * env);
}

static std::vector<e::AudioClipDesc> modes() {
  std::vector<e::AudioClipDesc> v;
  const double f100 = fptAt(100.0), f140 = fptAt(140.0), f80 = fptAt(80.0);
  const std::vector<e::WarpSegment> mk = {{0, 0.0, f100}, {kPpq, f100 * kPpq, f140}, {kPpq * 2, f100 * kPpq + f140 * kPpq, f80}};
  e::AudioClipDesc rp = warped(e::ClipWarp::RePitch, 1, kPpq / 3, kPpq * 4, 100.0);
  rp.segments = mk;
  rp.loop = {true, kPpq / 8, kPpq * 5 / 2};
  v.push_back(rp);
  auto add = [&](e::ClipWarp w, uint32_t id, Ticks start, bool rev, double ratio) {
    e::AudioClipDesc c = warped(w, id, start, kPpq * 3, 100.0);
    c.segments = mk;
    c.reverse = rev;
    c.stretch.pitchRatio = ratio;
    c.stretch.flux = 25.0f;
    c.stretch.granulation = kPpq / 4;
    c.loop = {true, kPpq / 4, kPpq * 9 / 4};
    v.push_back(c);
  };
  add(e::ClipWarp::Beats, 2, kPpq * 5, false, 1.12);
  add(e::ClipWarp::Tones, 3, kPpq * 8, true, 0.94);
  add(e::ClipWarp::Texture, 4, kPpq * 11, false, 1.0);
  add(e::ClipWarp::Complex, 5, kPpq * 14, false, 1.09);
  add(e::ClipWarp::ComplexPro, 6, kPpq * 17, true, 0.85);
  return v;
}

int main() {
  const e::ClipSample s = makeSample(48000 * 12, 2, busy);
  // ---- A ----
  {
    const std::vector<e::AudioClipDesc> all = modes();
    for (size_t only = 0; only < all.size(); ++only) {
      std::vector<std::vector<float>> res;
      const uint32_t blocks[] = {64, 32, 37, 333, 1024};
      for (uint32_t b : blocks) {
        Rig rig(b);
        rig.tempo({{0, 97.0, pq::TempoCurve::Linear}, {kPpq * 22, 151.0, pq::TempoCurve::Step}});
        rig.audio({{1, {all[only]}}}, {s});
        rig.play(0);
        res.push_back(rig.render(static_cast<uint64_t>(rig.map.ticksToSamples(kPpq * 21, kSr))));
      }
      std::printf("A %-10s:", modeName(all[only].warp));
      for (size_t k = 1; k < res.size(); ++k) {
        size_t diff = 0;
        double mx = 0.0;
        for (size_t i = 0; i < res[0].size(); ++i) {
          const double d = std::abs(static_cast<double>(res[k][i]) - res[0][i]);
          diff += d != 0.0;
          mx = std::max(mx, d);
        }
        std::printf("  b%u: %zu differ (max %.2g)", blocks[k], diff, mx);
      }
      std::printf("  peak %.3f\n", stat(res[0]).peak);
    }
  }
  // ---- B ----
  {
    std::vector<std::vector<float>> res;
    std::vector<size_t> firsts;
    for (uint32_t b : {32u, 256u, 1024u}) {
      Rig rig(b);
      rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
      e::AudioClipDesc c = warped(e::ClipWarp::Complex, 1, 0, kPpq * 16, 120.0);
      rig.audio({{1, {c}}}, {s});
      rig.play(kPpq * 2);
      res.push_back(rig.render(48000));
      size_t f = 0;
      while (f < res.back().size() && res.back()[f] == 0.0f) ++f;
      firsts.push_back(f);
    }
    size_t d1 = 0, d2 = 0;
    for (size_t i = 0; i < res[0].size(); ++i) {
      d1 += res[1][i] != res[0][i];
      d2 += res[2][i] != res[0][i];
    }
    std::printf("B uncued Play inside Complex: first sound at %.2f / %.2f / %.2f ms (blocks 32/256/1024); 256 vs 32: %zu differ, 1024 vs 32: %zu\n",
                firsts[0] / 48.0, firsts[1] / 48.0, firsts[2] / 48.0, d1, d2);
  }
  // ---- C ----
  for (double ratio : {0.5, 1.0, 1.5, 2.0}) {
    Rig rig(256);
    rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
    e::AudioClipDesc c = warped(e::ClipWarp::Complex, 1, 0, kPpq * 20, 120.0);
    c.stretch.pitchRatio = ratio;
    rig.audio({{1, {c}}}, {s});
    rig.play(0);
    (void)rig.render(48000);
    rig.command(e::TransportCommandKind::Locate, kPpq * 10);
    const std::vector<float> o = rig.render(48000);
    // the first block may hold the 2 ms fade-out of the old voice: find the first sample after a run of zeros
    size_t z = 0;
    while (z < o.size() && o[z] != 0.0f) ++z;
    size_t f = z;
    while (f < o.size() && o[f] == 0.0f) ++f;
    std::printf("C locate while playing, Complex ratio %.1f: silent from %.2f ms, sound again at %.2f ms; late starts %llu\n", ratio,
                z / 48.0, f / 48.0, static_cast<unsigned long long>(rig.tl->clipPlayer().stats().stretchLateStarts));
  }
  // ---- D ----
  for (uint32_t tracks : {1u, 16u, 17u, 24u, 40u}) {
    Rig rig(256, 64);
    rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
    std::vector<e::AudioTrackDesc> t;
    for (uint32_t k = 0; k < tracks; ++k) {
      e::AudioClipDesc c = warped(e::ClipWarp::Complex, 1 + k, 0, kPpq * 16, 120.0);
      t.push_back({1 + k, {c}});
    }
    rig.audio(t, {s});
    e::TransportCommand lc;
    lc.kind = e::TransportCommandKind::SetLoop;
    lc.loopEnabled = true;
    lc.loopStart = kPpq * 2;
    lc.loopEnd = kPpq * 6;
    rig.tl->apply(lc);
    rig.command(e::TransportCommandKind::Locate, kPpq * 2);
    for (int i = 0; i < 40; ++i) rig.step(256);  // stopped: cue
    rig.play(kPpq * 2);
    std::vector<std::vector<float>> outs(tracks);
    for (int i = 0; i < 48000 * 7 / 256; ++i) {
      rig.step(256);
      for (uint32_t k = 0; k < tracks; ++k) {
        const float* p = rig.tl->clipPlayer().channels()[k * e::kClipChannels];
        outs[k].insert(outs[k].end(), p, p + 256);
      }
    }
    // zero runs of > 2 ms after the start (a late wrap leaves a gap)
    uint32_t gappy = 0;
    for (uint32_t k = 0; k < tracks; ++k) {
      size_t run = 0, worst = 0;
      for (size_t i = 200; i < outs[k].size(); ++i) {
        run = outs[k][i] == 0.0f ? run + 1 : 0;
        worst = std::max(worst, run);
      }
      gappy += worst > 96;
    }
    const e::ClipPlayerStats& ps = rig.tl->clipPlayer().stats();
    std::printf("D loop of 2 s over %2u Complex tracks, 3 wraps: claims %llu fallbacks %llu lateStarts %llu cues %llu adopted %llu; tracks with a gap > 2 ms: %u; worst block %.2f Mcyc\n",
                tracks, static_cast<unsigned long long>(ps.stretchClaims), static_cast<unsigned long long>(ps.stretchFallbacks),
                static_cast<unsigned long long>(ps.stretchLateStarts), static_cast<unsigned long long>(ps.stretchCues),
                static_cast<unsigned long long>(ps.stretchCuesAdopted), gappy, rig.worstMcycles);
  }
  // ---- E: 20 Complex tracks whose clips all change on the same bar (no loop): stretchers at the boundary ----
  for (uint32_t tracks : {16u, 17u, 20u}) {
    Rig rig(256, 64);
    rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
    std::vector<e::AudioTrackDesc> t;
    for (uint32_t k = 0; k < tracks; ++k) {
      e::AudioClipDesc a = warped(e::ClipWarp::Complex, 1 + 2 * k, 0, kPpq * 4, 120.0);
      e::AudioClipDesc b = warped(e::ClipWarp::Complex, 2 + 2 * k, kPpq * 4, kPpq * 4, 120.0);
      t.push_back({1 + k, {a, b}});
    }
    rig.audio(t, {s});
    rig.command(e::TransportCommandKind::Locate, 0);
    for (int i = 0; i < 40; ++i) rig.step(256);
    rig.play(0);
    for (int i = 0; i < 48000 * 4 / 256; ++i) rig.step(256);
    const e::ClipPlayerStats& ps = rig.tl->clipPlayer().stats();
    std::printf("E %2u tracks, back-to-back Complex clips at bar 2: claims %llu, fallbacks to Tones %llu\n", tracks,
                static_cast<unsigned long long>(ps.stretchClaims), static_cast<unsigned long long>(ps.stretchFallbacks));
  }
  return 0;
}
