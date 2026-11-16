// VZ.9 (docs/reviews/VZ.md C1, "Tests"): every analysis stage reconfigured in sequence. The worker reconfigures a stage
// whenever a face's options change, in any order; C1 was a buffer sized for one configuration and used by a later one
// (Particles → Glow → tracker window 4096 → Particles ran a 4096 window over 2048-sized buffers). Each case walks every
// ordered pair of a set of configurations with audio between them, then checks the stage against a fresh one built with
// the final configuration and fed the same audio: what it shows must have converged to the same thing. (Size invariants
// are also asserted inside the stages: the check preset aborts on a mismatch.)

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "pq/core/denormal_guard.h"
#include "pq/dsp/band_levels.h"
#include "pq/dsp/loudness.h"
#include "pq/dsp/spectrum.h"
#include "pq/dsp/stereo.h"
#include "pq/dsp/stereo_cloud.h"
#include "pq/dsp/waterfall.h"
#include "pq/dsp/waveform.h"

using namespace pq::dsp;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Stateless test audio: a 1 kHz tone (at 48 kHz; it scales with the rate a configuration claims), R quieter and
// delayed, plus hashed noise (so the same frame index is the same sample in every instance).
float noiseAt(uint64_t i, uint32_t salt) {
  uint64_t x = (i + 0x9E3779B97F4A7C15ull * (salt + 1)) * 0xBF58476D1CE4E5B9ull;
  x ^= x >> 31;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 29;
  return static_cast<float>(static_cast<double>(x >> 40) / static_cast<double>(1ull << 24) - 0.5);
}
void sampleAt(uint64_t i, float& l, float& r, bool noisy = true) {
  const double ph = 2.0 * kPi * 1000.0 * static_cast<double>(i) / 48000.0;
  l = static_cast<float>(0.3 * std::sin(ph)) + (noisy ? 0.02f * noiseAt(i, 0) : 0.0f);
  r = static_cast<float>(0.2 * std::sin(ph - 0.3)) + (noisy ? 0.02f * noiseAt(i, 1) : 0.0f);
}

// Frames [t, t + n) in blocks of 512 into push(l, r, k).
void feed(uint64_t& t, uint64_t n, const std::function<void(const float*, const float*, uint32_t)>& push, bool noisy = true) {
  std::vector<float> l(512), r(512);
  for (uint64_t done = 0; done < n;) {
    const auto k = static_cast<uint32_t>(std::min<uint64_t>(512, n - done));
    for (uint32_t i = 0; i < k; ++i) sampleAt(t + i, l[i], r[i], noisy);
    push(l.data(), r.data(), k);
    t += k;
    done += k;
  }
}

// Every ordered pair (i, j): configure i, audio, configure j, audio.
template <class Configure>
void walkPairs(size_t count, Configure&& configure, const std::function<void()>& audio) {
  for (size_t i = 0; i < count; ++i)
    for (size_t j = 0; j < count; ++j) {
      CAPTURE(i);
      CAPTURE(j);
      configure(i);
      audio();
      configure(j);
      audio();
    }
}

}  // namespace

TEST_CASE("reconfigure in sequence: CrossSpectrum (Stereo Image / Phase Scope) — every pair of options, then as fresh") {
  pq::DenormalGuard dg;
  std::vector<CrossSpectrumConfig> cs(7);
  cs[0].parts = kStereoParticles, cs[0].peakFft = 2048;
  cs[1].parts = kStereoGlow, cs[1].peakFft = 4096;
  cs[2].parts = kStereoParticles, cs[2].peakFft = 4096;
  cs[3].fftSize = 8192, cs[3].parts = kStereoGlow | kStereoAfterglow;
  cs[4].fftSize = 2048, cs[4].points = 128, cs[4].parts = kStereoParticles | kStereoGlow, cs[4].peakFft = 1024;
  cs[5].window = SpecWindow::Hann, cs[5].scale = FreqScale::Linear, cs[5].parts = 0;
  cs[6].parts = kStereoParticles, cs[6].peakFft = 8192, cs[6].hop = 512;
  CrossSpectrum walked;
  uint64_t t = 0;
  walkPairs(cs.size(), [&](size_t k) { walked.configure(cs[k]); },
            [&] { feed(t, 4800, [&](const float* l, const float* r, uint32_t n) { walked.push(l, r, n); }); });
  for (const size_t last : {size_t{2}, size_t{4}, size_t{6}}) {
    CAPTURE(last);
    walked.configure(cs[last]);
    CrossSpectrum fresh(cs[last]);
    feed(t, 3 * 48000, [&](const float* l, const float* r, uint32_t n) {
      walked.push(l, r, n);
      fresh.push(l, r, n);
    });
    REQUIRE(walked.points().size() == fresh.points().size());
    for (size_t p = 0; p < fresh.points().size(); ++p) {
      const StereoPoint &a = walked.points()[p], &b = fresh.points()[p];
      if (b.db < cs[last].floorDb) continue;
      CAPTURE(b.hz);
      CHECK(std::fabs(a.db - b.db) < 0.5f);
      CHECK(std::fabs(a.pos - b.pos) < 0.05f);
      CHECK(std::fabs(a.coherence - b.coherence) < 0.05f);
    }
    auto toneParticle = [](const CrossSpectrum& s) {
      for (const StereoParticle& p : s.particles())
        if (p.alive && std::fabs(p.logF - std::log2(1000.0f)) < 0.1f) return true;
      return false;
    };
    CHECK(toneParticle(fresh));
    CHECK(toneParticle(walked));
  }
}

TEST_CASE("reconfigure in sequence: StereoFieldState (Stereo Field) — bands on / off, rates, crossovers") {
  pq::DenormalGuard dg;
  struct C {
    double sr;
    float ms;
    bool bands;
    float lo, hi;
  };
  const std::vector<C> cs = {{48000, 300, false, 200, 2000}, {48000, 100, true, 200, 2000}, {96000, 300, true, 300, 3000},
                             {48000, 50, true, 2000, 200},   {44100, 300, true, 120, 8000}};
  StereoFieldState walked;
  uint64_t t = 0;
  walkPairs(cs.size(), [&](size_t k) { walked.configure(cs[k].sr, cs[k].ms, cs[k].bands, cs[k].lo, cs[k].hi); },
            [&] { feed(t, 4800, [&](const float* l, const float* r, uint32_t n) { walked.push(l, r, n); }); });
  for (const size_t last : {size_t{1}, size_t{4}}) {
    const C& c = cs[last];
    walked.configure(c.sr, c.ms, c.bands, c.lo, c.hi);
    StereoFieldState fresh;
    fresh.configure(c.sr, c.ms, c.bands, c.lo, c.hi);
    feed(t, 2 * 48000, [&](const float* l, const float* r, uint32_t n) {
      walked.push(l, r, n);
      fresh.push(l, r, n);
    });
    CHECK(walked.correlation() == doctest::Approx(fresh.correlation()).epsilon(0.01));
    for (uint32_t b = 0; b < StereoFieldState::kBands; ++b) {
      CAPTURE(b);
      CHECK(std::fabs(walked.bandCorrelation(b) - fresh.bandCorrelation(b)) < 0.02f);
      CHECK(std::isfinite(walked.bandRadius(b)));
    }
  }
}

TEST_CASE("reconfigure in sequence: BandLevels — band counts, modes, channels, ballistics, rates") {
  pq::DenormalGuard dg;
  std::vector<BandLevelsConfig> cs(6);
  cs[1].bands = 5, cs[1].independent3 = false, cs[1].crossovers[0] = 60, cs[1].crossovers[1] = 250,
  cs[1].crossovers[2] = 2000, cs[1].crossovers[3] = 8000;
  cs[2].bands = 4, cs[2].independent3 = false, cs[2].crossovers[0] = 120, cs[2].crossovers[1] = 800,
  cs[2].crossovers[2] = 5000, cs[2].channel = BandLevelsConfig::Channel::Side;
  cs[3].ballistics = BandLevelsConfig::Ballistics::Peak, cs[3].sampleRate = 96000.0, cs[3].columnRate = 30.0f;
  cs[4].ballistics = BandLevelsConfig::Ballistics::Custom, cs[4].attackMs = 5.0f, cs[4].releaseMs = 50.0f,
  cs[4].channel = BandLevelsConfig::Channel::Left;
  cs[5].independent3 = false, cs[5].columnRate = 120.0f;
  BandLevels walked;
  uint64_t t = 0;
  walkPairs(cs.size(), [&](size_t k) { walked.configure(cs[k]); },
            [&] { feed(t, 4800, [&](const float* l, const float* r, uint32_t n) { walked.push(l, r, n); }); });
  for (const size_t last : {size_t{1}, size_t{2}, size_t{4}}) {
    CAPTURE(last);
    walked.configure(cs[last]);
    BandLevels fresh(cs[last]);
    feed(t, 2 * 48000, [&](const float* l, const float* r, uint32_t n) {
      walked.push(l, r, n);
      fresh.push(l, r, n);
    });
    for (int b = 0; b < cs[last].bands; ++b) {
      CAPTURE(b);
      CHECK(std::fabs(walked.level(b) - fresh.level(b)) < 0.3f);
    }
  }
}

TEST_CASE("reconfigure in sequence: WaveCapture (Waveform) — loop on / off, spans, rates") {
  pq::DenormalGuard dg;
  std::vector<WaveConfig> cs(5);
  cs[1].loop = true, cs[1].loopSeconds = 1.0f;
  cs[2].loop = true, cs[2].loopSeconds = 4.0f, cs[2].sampleRate = 96000.0;
  cs[3].loop = true, cs[3].loopBeats = 8.0;
  cs[4].sampleRate = 44100.0;
  WaveCapture walked;
  uint64_t t = 0;
  auto push = [](WaveCapture& w) {
    return [&w](const float* l, const float* r, uint32_t n) { w.push(nullptr, l, r, n); };
  };
  walkPairs(cs.size(), [&](size_t k) { walked.configure(cs[k]); }, [&] { feed(t, 4800, push(walked)); });
  for (const size_t last : {size_t{1}, size_t{2}}) {
    CAPTURE(last);
    walked.configure(cs[last]);
    WaveCapture fresh(cs[last]);
    feed(t, 2 * 48000, [&](const float* l, const float* r, uint32_t n) {
      walked.push(nullptr, l, r, n);
      fresh.push(nullptr, l, r, n);
    }, /*noisy=*/false);
    std::vector<WaveBin> a(64), b(64);
    REQUIRE(walked.readBins(walked.bins() - 64, 64, a.data()) == 64);
    REQUIRE(fresh.readBins(fresh.bins() - 64, 64, b.data()) == 64);
    for (int c = 0; c < WaveBin::kChannels; ++c) {
      float ma = 0.0f, mb = 0.0f;
      for (int k = 0; k < 64; ++k) ma = std::max(ma, a[k].mx[c]), mb = std::max(mb, b[k].mx[c]);
      CAPTURE(c);
      CHECK(ma == doctest::Approx(mb).epsilon(2e-3));
    }
    std::vector<WaveBin> slots(WaveCapture::kLoopSlots);
    std::vector<uint8_t> filled(WaveCapture::kLoopSlots);
    WaveLoopState sa, sb;
    (void)walked.readLoop(0, slots.data(), filled.data(), sa);
    (void)fresh.readLoop(0, slots.data(), filled.data(), sb);
    CHECK(sa.spanSamples == doctest::Approx(sb.spanSamples));
    CHECK(sa.clocked == sb.clocked);
  }
}

TEST_CASE("reconfigure in sequence: LoudnessMeter (Level / LUFS / History) — parts on and off, rates, windows") {
  pq::DenormalGuard dg;
  std::vector<LoudnessConfig> cs(6);
  cs[0].parts = kLoudLevels;
  cs[1].parts = kLoudLoudness | kLoudHistory;
  cs[2].parts = kLoudTruePeak, cs[2].sampleRate = 96000.0;
  cs[3].parts = kLoudLevels | kLoudLoudness | kLoudTruePeak | kLoudHistory, cs[3].rmsWindowMs = 50.0f;
  cs[4].parts = kLoudLoudness, cs[4].sampleRate = 44100.0, cs[4].targetLufs = -14.0f;
  cs[5].parts = kLoudLevels | kLoudLoudness | kLoudTruePeak | kLoudHistory;
  for (LoudnessConfig& c : cs) c.autoReset = false;
  LoudnessMeter walked;
  uint64_t t = 0;
  walkPairs(cs.size(), [&](size_t k) { walked.configure(cs[k]); },
            [&] { feed(t, 4800, [&](const float* l, const float* r, uint32_t n) { walked.push(l, r, n); }); });
  walked.configure(cs[5]);
  LoudnessMeter fresh(cs[5]);
  feed(t, 4 * 48000, [&](const float* l, const float* r, uint32_t n) {
    walked.push(l, r, n);
    fresh.push(l, r, n);
  });
  const LoudnessReadings a = walked.readings(), b = fresh.readings();
  CHECK(std::fabs(a.momentary - b.momentary) < 0.05f);
  CHECK(std::fabs(a.shortTerm - b.shortTerm) < 0.05f);
  CHECK(std::fabs(a.truePeakNowDb - b.truePeakNowDb) < 0.1f);
  CHECK(std::fabs(a.correlation - b.correlation) < 0.01f);
  CHECK(std::fabs(a.rmsDbMono - b.rmsDbMono) < 0.05f);
  for (int c = 0; c < 2; ++c) {
    CHECK(std::fabs(a.rmsDb[c] - b.rmsDb[c]) < 0.05f);
    CHECK(std::fabs(a.peakDb[c] - b.peakDb[c]) < 0.1f);
  }
  std::vector<LoudnessPoint> p(8);
  CHECK(walked.history(walked.points() - 8, 8, p.data()) == 8);
  CHECK(std::fabs(p[7].momentary - b.momentary) < 0.05f);
}

TEST_CASE("reconfigure in sequence: WaterfallAnalyzer — sizes, multi-res, channels, scales, spans, slopes") {
  pq::DenormalGuard dg;
  std::vector<WaterfallConfig> cs(6);
  cs[1].fftSize = 8192, cs[1].multiRes = false;
  cs[2].channel = WaterfallChannel::Side, cs[2].spanSec = 12.0f;
  cs[3].scale = FreqScale::Linear, cs[3].slopeDbPerOct = 4.5f;
  cs[4].fftSize = 1024, cs[4].sampleRate = 96000.0, cs[4].spanSec = 2.0f;
  cs[5].channel = WaterfallChannel::Left, cs[5].attackMs = 5.0f, cs[5].releaseMs = 100.0f;
  WaterfallAnalyzer walked;
  uint64_t t = 0;
  walkPairs(cs.size(), [&](size_t k) { walked.configure(cs[k]); },
            [&] { feed(t, 4800, [&](const float* l, const float* r, uint32_t n) { walked.push(l, r, n); }); });
  for (const size_t last : {size_t{1}, size_t{3}, size_t{5}}) {
    CAPTURE(last);
    walked.configure(cs[last]);
    WaterfallAnalyzer fresh(cs[last]);
    feed(t, 2 * 48000, [&](const float* l, const float* r, uint32_t n) {
      walked.push(l, r, n);
      fresh.push(l, r, n);
    });
    CHECK(walked.slicePitch() == fresh.slicePitch());
    const auto a = walked.live(), b = fresh.live();
    REQUIRE(a.size() == b.size());
    for (size_t k = 0; k < b.size(); ++k)
      if (b[k] > -60.0f) {
        CAPTURE(k);
        CHECK(std::fabs(a[k] - b[k]) < 0.5f);
      }
    REQUIRE(walked.slice(walked.slices() - 1) != nullptr);
  }
}

TEST_CASE("reconfigure in sequence: StereoCloud (Stereo Spectrogram) — sizes, low-band factors, axes, reassignment") {
  pq::DenormalGuard dg;
  std::vector<StereoCloudConfig> cs(6);
  cs[1].fftSize = 8192, cs[1].loFactor = 2;
  cs[2].fftSize = 512, cs[2].loFactor = 8;
  cs[3].scale = FreqScale::Linear, cs[3].fMin = 100.0f, cs[3].fMax = 12000.0f;
  cs[4].reassign = false, cs[4].consensus = 0.0f, cs[4].sampleRate = 96000.0;
  cs[5].fftSize = 4096, cs[5].loFactor = 4;
  StereoCloud walked;
  uint64_t t = 0;
  auto none = [](const CloudPoint*, uint32_t) {};
  auto noSlice = [](const StereoCloud::RibSlice&) {};
  walkPairs(cs.size(), [&](size_t k) { walked.configure(cs[k]); },
            [&] { feed(t, 4800, [&](const float* l, const float* r, uint32_t n) { walked.push(l, r, n, none, noSlice); }); });
  for (const size_t last : {size_t{1}, size_t{2}, size_t{5}}) {
    CAPTURE(last);
    walked.configure(cs[last]);
    StereoCloud fresh(cs[last]);
    StereoCloud::RibSlice ra, rb;
    std::vector<CloudPoint> pa, pb;
    feed(t, 2 * 48000, [&](const float* l, const float* r, uint32_t n) {
      walked.push(l, r, n, [&](const CloudPoint* p, uint32_t k) { pa.assign(p, p + k); },
                  [&](const StereoCloud::RibSlice& s) { ra = s; });
      fresh.push(l, r, n, [&](const CloudPoint* p, uint32_t k) { pb.assign(p, p + k); },
                 [&](const StereoCloud::RibSlice& s) { rb = s; });
    });
    for (uint32_t k = 0; k < StereoCloud::kRibBands; ++k)
      if (rb.db[k] > -50.0f) {
        CAPTURE(k);
        CHECK(std::fabs(ra.db[k] - rb.db[k]) < 0.5f);
        CHECK(std::fabs(ra.pos[k] - rb.pos[k]) < 0.03f);
      }
    auto tonePan = [](const std::vector<CloudPoint>& v) {
      double w = 0.0, s = 0.0;
      for (const CloudPoint& p : v)
        if (std::fabs(p.freqHz - 1000.0f) < 30.0f) w += p.power, s += p.power * p.pan;
      return w > 0.0 ? s / w : 99.0;
    };
    CHECK(tonePan(pa) == doctest::Approx(tonePan(pb)).epsilon(0.02));
  }
}

TEST_CASE("reconfigure in sequence: SpectrumAnalyzer (Spectrum / Spectrogram) — live ballistics, hold and average") {
  pq::DenormalGuard dg;
  std::vector<SpectrumLive> ls(5);
  ls[1].attackMs = 5.0f, ls[1].releaseMs = 1000.0f;
  ls[2].peakHold = true, ls[2].holdSeconds = 0.5f, ls[2].fallDbPerSecond = 30.0f;
  ls[3].average = true, ls[3].averageSeconds = 0.5f;
  ls[4].peakHold = true, ls[4].average = true, ls[4].averageSeconds = 0.0f, ls[4].releaseMs = 50.0f;
  SpectrumConfig base;
  base.points = 256;
  SpectrumAnalyzer walked(base);
  uint64_t t = 0;
  auto mono = [](SpectrumAnalyzer& a) {
    return [&a](const float* l, const float* r, uint32_t n) {
      float m[512];
      for (uint32_t i = 0; i < n; ++i) m[i] = 0.5f * (l[i] + r[i]);
      a.push(m, n);
    };
  };
  walkPairs(ls.size(), [&](size_t k) { walked.setLive(ls[k]); }, [&] { feed(t, 4800, mono(walked)); });
  for (const size_t last : {size_t{2}, size_t{3}, size_t{4}}) {
    CAPTURE(last);
    walked.setLive(ls[last]);
    SpectrumConfig c = base;
    c.attackMs = ls[last].attackMs, c.releaseMs = ls[last].releaseMs, c.peakHold = ls[last].peakHold;
    c.holdSeconds = ls[last].holdSeconds, c.fallDbPerSecond = ls[last].fallDbPerSecond;
    c.average = ls[last].average, c.averageSeconds = ls[last].averageSeconds;
    SpectrumAnalyzer fresh(c);
    // The tone alone: the two analysers' hops sit at different stream offsets, so noise bins differ frame by frame.
    feed(t, 3 * 48000, [&](const float* l, const float* r, uint32_t n) {
      mono(walked)(l, r, n);
      mono(fresh)(l, r, n);
    }, /*noisy=*/false);
    std::vector<float> aa(base.points), ab(base.points);
    walked.averageDb(aa.data());
    fresh.averageDb(ab.data());
    for (uint32_t p = 0; p < base.points; ++p)
      if (fresh.levelsDb()[p] > -60.0f) {
        CAPTURE(p);
        CHECK(std::fabs(walked.levelsDb()[p] - fresh.levelsDb()[p]) < 0.5f);
        if (c.peakHold) CHECK(std::fabs(walked.peaksDb()[p] - fresh.peaksDb()[p]) < 1.0f);
        if (c.average) CHECK(std::fabs(aa[p] - ab[p]) < 0.5f);
      }
  }
}
