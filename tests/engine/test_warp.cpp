// T6.3: the warp engine I (docs/plans/M6.md T6.3; pq/engine/audio_sequence.h "Beats, Tones, Texture", clip_player.h):
//   - Re-Pitch with warp markers: every segment's source position is exact (rubato: four segments of different speeds)
//   - Beats: a 120 BPM drum loop played at 90 / 150 BPM keeps every hit within ±1 ms of the warped grid (grid slices and
//     transient slices), its spectral centroid within 2 %; every loop mode renders
//   - Tones: a 220 Hz sine played at 0.5× / 2× the tempo stays at 220 Hz ±0.5 ¢; transposed +3 steps of 31-EDO it is
//     exactly 3·1200/31 ¢ higher (±0.5 ¢); Texture keeps the level of a stationary source
//   - renders bit-identical across engine blocks 64 / 1000 (every mode; loops, reverse, a tempo ramp)
//   - a gain-only edit while playing keeps the voice (seamless: after the 2 ms ramp, bit-identical to a render at the
//     new gain); streamed (offline blocking fills) == resident bit for bit, every mode
//   - RtScope over every mode (streamed and resident, swaps, slice claims)
// T6.5, Complex / Complex Pro (the stretch pool, pq/engine/stretch_pool.h): every test above covers them too (they are
// in everyMode()), and
//   - a 220 Hz sine at 0.5× / 2× stays within ±1 ¢ over two consecutive 1.4 s windows; +3 steps of 31-EDO likewise
//   - sample-aligned (no latency: at the source tempo the output lines up with the source to the frame; silent before
//     the clip; the first 20 ms within -80 dB of the source); a drum loop's onsets stay within ±2.5 ms of the warped
//     grid at 90 / 150 BPM (the vocoder smears attacks; Beats keeps them within ±1 ms)
//   - Complex Pro keeps a vowel's formants at +7 st (shift < 5 %), Complex moves them with the pitch (> 30 %)
//   - an exhausted pool plays Complex as Tones (counted, identical to a Tones clip, no click); deterministic renders
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/alloc_trap.h"
#include "pq/dsp/fft.h"
#include "pq/engine/audio_sequence.h"
#include "pq/engine/clip_player.h"
#include "pq/engine/timeline.h"
#include "pq/media/sample_pool.h"
#include "pq/media/transients.h"
#include "pq/media/wav_file.h"

namespace {

namespace e = pq::engine;
namespace fs = std::filesystem;
using pq::kPpq;
using pq::SampleTime;
using pq::Ticks;

constexpr double kSr = 48000.0;
constexpr int64_t kG = pq::media::kSampleGuardFrames;

struct Frames {
  pq::AlignedBuffer<float> buf;
};

e::ClipSample makeSample(int64_t frames, uint32_t channels, const std::function<float(uint32_t, int64_t)>& fn) {
  auto o = std::make_shared<Frames>();
  const int64_t stride = frames + 2 * kG;
  o->buf = pq::AlignedBuffer<float>(static_cast<size_t>(stride) * channels);
  std::fill(o->buf.data(), o->buf.data() + o->buf.size(), 0.0f);
  e::ClipSample s;
  for (uint32_t c = 0; c < channels; ++c) {
    float* d = o->buf.data() + static_cast<size_t>(c) * stride + kG;
    for (int64_t i = 0; i < frames; ++i) d[i] = fn(c, i);
    s.fixed.data[c] = d;
  }
  s.fixed.frames = s.fixed.residentFrames = frames;
  s.fixed.channels = channels;
  s.fixed.sampleRate = 48000;
  s.owner = o;
  return s;
}

pq::TempoMap tempoMap(std::vector<pq::TempoPoint> pts) {
  auto r = pq::TempoMap::create(pts);
  REQUIRE(r.ok());
  return std::move(r.value());
}

struct Rig {
  e::TimelineConfig cfg;
  std::unique_ptr<e::Timeline> tl;
  pq::TempoMap map;
  int64_t time = 0;

  explicit Rig(uint32_t block = 64, pq::media::StreamPool* streams = nullptr) {
    cfg.sampleRate = kSr;
    cfg.maxBlockFrames = block;
    cfg.maxTracks = 8;
    cfg.clipDeclickSeconds = 0.002;
    cfg.streams = streams;
    tl = std::make_unique<e::Timeline>(cfg);
  }
  void tempo(std::vector<pq::TempoPoint> pts) {
    map = tempoMap(pts);
    auto img = e::TempoImage::create(pts, {});
    REQUIRE(img.ok());
    REQUIRE(tl->publishTempo(std::move(img.value())).ok());
  }
  std::unique_ptr<e::AudioSequenceImage> build(std::vector<e::AudioTrackDesc> slots, std::vector<e::ClipSample> samples) const {
    e::AudioBuildOptions o;
    o.sampleRate = kSr;
    auto r = e::AudioSequenceImage::build(slots, std::move(samples), map, o);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return std::move(r.value());
  }
  void audio(std::vector<e::AudioTrackDesc> slots, std::vector<e::ClipSample> samples) {
    REQUIRE(tl->publishAudio(build(std::move(slots), std::move(samples))).ok());
  }
  void play(Ticks t = 0) {
    e::TransportCommand c;
    c.kind = e::TransportCommandKind::Play;
    c.tick = t;
    tl->apply(c);
  }
  std::vector<float> render(uint64_t frames, uint32_t slot = 0, uint32_t ch = 0, std::vector<float>* right = nullptr) {
    std::vector<float> out;
    out.reserve(frames);
    for (uint64_t done = 0; done < frames;) {
      const auto n = static_cast<uint32_t>(std::min<uint64_t>(cfg.maxBlockFrames, frames - done));
      tl->process(time, n);
      time += n;
      const float* const* chans = tl->clipPlayer().channels();
      out.insert(out.end(), chans[slot * e::kClipChannels + ch], chans[slot * e::kClipChannels + ch] + n);
      if (right != nullptr) right->insert(right->end(), chans[slot * e::kClipChannels + 1], chans[slot * e::kClipChannels + 1] + n);
      done += n;
    }
    return out;
  }
};

double fptAt(double bpm) { return kSr * 60.0 / (bpm * static_cast<double>(kPpq)); }

e::AudioClipDesc warped(e::ClipWarp w, uint32_t id, Ticks start, Ticks length, double sourceBpm) {
  e::AudioClipDesc c;
  c.id = id;
  c.warp = w;
  c.start = start;
  c.length = length;
  c.framesPerTick = fptAt(sourceBpm);
  c.stretch.seed = id;
  return c;
}

// |DTFT| of a Hann-windowed stretch at f Hz.
double dtftMag(const std::vector<float>& x, size_t from, size_t n, double f) {
  const double w = 2.0 * std::numbers::pi * f / kSr;
  double re = 0.0, im = 0.0;
  // Rotating phasor in double, renormalised (a direct sum: no bin bias).
  double c = 1.0, s = 0.0;
  const double cw = std::cos(w), sw = std::sin(w);
  for (size_t i = 0; i < n; ++i) {
    const double h = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n));
    const double y = h * x[from + i];
    re += y * c;
    im -= y * s;
    const double c2 = c * cw - s * sw;
    s = s * cw + c * sw;
    c = c2;
    if ((i & 4095) == 4095) {
      const double m = std::sqrt(c * c + s * s);
      c /= m;
      s /= m;
    }
  }
  return std::sqrt(re * re + im * im);
}

// The frequency of the strongest component near `guess` (±3 Hz, golden-section on the windowed DTFT).
double peakHz(const std::vector<float>& x, size_t from, size_t n, double guess) {
  double lo = guess - 3.0, hi = guess + 3.0;
  double best = lo, bestM = -1.0;
  for (double f = lo; f <= hi; f += 0.05) {  // coarse: avoid a side lobe
    const double m = dtftMag(x, from, n, f);
    if (m > bestM) {
      bestM = m;
      best = f;
    }
  }
  lo = best - 0.05;
  hi = best + 0.05;
  const double g = (std::sqrt(5.0) - 1.0) / 2.0;
  double a = hi - g * (hi - lo), b = lo + g * (hi - lo);
  double fa = dtftMag(x, from, n, a), fb = dtftMag(x, from, n, b);
  for (int i = 0; i < 40; ++i) {
    if (fa > fb) {
      hi = b;
      b = a;
      fb = fa;
      a = hi - g * (hi - lo);
      fa = dtftMag(x, from, n, a);
    } else {
      lo = a;
      a = b;
      fa = fb;
      b = lo + g * (hi - lo);
      fb = dtftMag(x, from, n, b);
    }
  }
  return 0.5 * (lo + hi);
}

double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }

// ---- The drum loop: 120 BPM, 2 bars, a hit on every eighth (kick / hat / snare / hat) --------------------------

constexpr int64_t kHitFrames = 12000;  // an eighth at 120 BPM
constexpr int kHits = 16;

float drum(int64_t i) {
  const int64_t h = i / kHitFrames;
  const double t = static_cast<double>(i - h * kHitFrames) / kSr;
  uint32_t z = static_cast<uint32_t>(i) * 747796405u + 2891336453u;
  z = ((z >> ((z >> 28u) + 4u)) ^ z) * 277803737u;
  const double noise = static_cast<double>((z >> 22u) ^ z) / 4294967296.0 * 2.0 - 1.0;
  switch (h % 4) {
    case 0: {  // kick: a falling sine and a click
      const double ph = 2.0 * std::numbers::pi * (50.0 * t + 70.0 * 0.04 * (1.0 - std::exp(-t / 0.04)));
      return static_cast<float>(0.8 * std::sin(ph + 0.6) * std::exp(-t / 0.08) + 0.3 * noise * std::exp(-t / 0.002));
    }
    case 2:  // snare: noise and a tone
      return static_cast<float>(0.5 * noise * std::exp(-t / 0.05) + 0.3 * std::sin(2.0 * std::numbers::pi * 190.0 * t + 0.4) *
                                                                       std::exp(-t / 0.03));
    default:  // hat
      return static_cast<float>(0.35 * noise * std::exp(-t / 0.012));
  }
}

// Where the hit that starts at source frame `srcStart` sits in the output near `e`: the lag in [−10, 10] ms that best
// correlates its first 40 ms (normalised), so tails of the hit before do not matter.
int64_t hitNear(const std::vector<float>& y, int64_t e, const std::vector<float>& src, int64_t srcStart) {
  constexpr int64_t kLen = 1920, kReach = 480;
  double best = -1e300;
  int64_t at = 0;
  for (int64_t lag = -kReach; lag <= kReach; ++lag) {
    const int64_t o = e + lag;
    if (o < 0 || o + kLen > static_cast<int64_t>(y.size())) continue;
    double num = 0.0, en = 0.0;
    for (int64_t i = 0; i < kLen; ++i) {
      const double a = y[static_cast<size_t>(o + i)], b = src[static_cast<size_t>(srcStart + i)];
      num += a * b;
      en += a * a;
    }
    const double sc = en > 0.0 ? num / std::sqrt(en) : 0.0;
    if (sc > best) {
      best = sc;
      at = lag;
    }
  }
  return at;
}

double centroid(const std::vector<float>& y, size_t from, size_t to) {
  constexpr uint32_t N = 4096;
  pq::dsp::RealFft fft(N);
  std::vector<float> x(N), re(N / 2 + 1), im(N / 2 + 1);
  std::vector<double> mag(N / 2 + 1, 0.0);
  for (size_t at = from; at + N <= to; at += N / 2) {
    for (uint32_t i = 0; i < N; ++i)
      x[i] = y[at + i] * static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / N));
    fft.forward(x.data(), re.data(), im.data());
    for (uint32_t k = 0; k <= N / 2; ++k) mag[k] += std::sqrt(static_cast<double>(re[k]) * re[k] + static_cast<double>(im[k]) * im[k]);
  }
  double num = 0.0, den = 0.0;
  for (uint32_t k = 2; k <= N / 2; ++k) {
    const double f = k * kSr / N;
    num += f * mag[k];
    den += mag[k];
  }
  return num / den;
}

fs::path writeWav(const char* name, int64_t frames, uint32_t channels, const std::function<float(uint32_t, int64_t)>& fn) {
  const fs::path dir = fs::temp_directory_path() / "pq_warp_tests";
  fs::create_directories(dir);
  const fs::path p = dir / name;
  pq::media::WavWriter w;
  REQUIRE(w.open(p, channels, 48000).ok());
  std::vector<std::vector<float>> buf(channels, std::vector<float>(4096));
  for (int64_t at = 0; at < frames; at += 4096) {
    const auto n = static_cast<uint32_t>(std::min<int64_t>(4096, frames - at));
    std::vector<const float*> ptrs;
    for (uint32_t c = 0; c < channels; ++c) {
      for (uint32_t i = 0; i < n; ++i) buf[c][i] = fn(c, at + i);
      ptrs.push_back(buf[c].data());
    }
    REQUIRE(w.write(ptrs.data(), n).ok());
  }
  REQUIRE(w.close().ok());
  return p;
}

void fillWait(void* user, pq::media::StreamId id, int64_t frames) {
  (void)static_cast<pq::media::SamplePool*>(user)->waitFilled(id, frames);
}

// One clip of every mode (loops, reverse, markers) over one sample: what the identity tests render.
std::vector<e::AudioClipDesc> everyMode() {
  std::vector<e::AudioClipDesc> v;
  e::AudioClipDesc rp = warped(e::ClipWarp::RePitch, 1, 0, kPpq * 3, 100.0);
  rp.segments = {{0, 0.0, fptAt(100.0)}, {kPpq, fptAt(100.0) * kPpq, fptAt(140.0)}, {kPpq * 2, fptAt(100.0) * kPpq + fptAt(140.0) * kPpq, fptAt(80.0)}};
  v.push_back(rp);
  e::AudioClipDesc b = warped(e::ClipWarp::Beats, 2, kPpq * 3, kPpq * 4, 120.0);
  b.contentStart = kPpq / 8;
  b.loop = {true, kPpq / 2, kPpq * 5 / 2};
  b.stretch.sliceLoop = e::SliceLoop::Forward;
  b.stretch.pitchRatio = 1.12;
  b.crossfadeIn = kPpq / 8;
  v.push_back(b);
  e::AudioClipDesc b2 = warped(e::ClipWarp::Beats, 3, kPpq * 7, kPpq * 3, 120.0);
  b2.stretch.sliceLoop = e::SliceLoop::BackForward;
  b2.stretch.envelope = 60.0f;
  b2.reverse = true;
  v.push_back(b2);
  e::AudioClipDesc t = warped(e::ClipWarp::Tones, 4, kPpq * 10, kPpq * 3, 110.0);
  t.loop = {true, kPpq / 4, kPpq * 2};
  t.stretch.pitchRatio = 0.94;
  v.push_back(t);
  e::AudioClipDesc x = warped(e::ClipWarp::Texture, 5, kPpq * 13, kPpq * 3, 130.0);
  x.stretch.flux = 40.0f;
  x.reverse = true;
  x.stretch.grainSeconds = 0.05;
  v.push_back(x);
  e::AudioClipDesc t2 = warped(e::ClipWarp::Tones, 6, kPpq * 16, kPpq * 2, 120.0);  // pitch rate 1: straight copies
  v.push_back(t2);
  // T6.5: Complex looped and transposed (the stream unfolds through the wraps), Complex Pro reversed with formants.
  e::AudioClipDesc cx = warped(e::ClipWarp::Complex, 7, kPpq * 18, kPpq * 3, 105.0);
  cx.contentStart = kPpq / 3;
  cx.loop = {true, kPpq / 2, kPpq * 3 / 2};
  cx.stretch.pitchRatio = 1.09;
  v.push_back(cx);
  e::AudioClipDesc cp = warped(e::ClipWarp::ComplexPro, 8, kPpq * 21, kPpq * 2, 135.0);
  cp.reverse = true;
  cp.stretch.pitchRatio = 0.85;
  cp.stretch.formants = 80.0f;
  cp.crossfadeIn = kPpq / 8;
  v.push_back(cp);
  return v;
}

constexpr Ticks kEveryModeEnd = kPpq * 23;  // the end of everyMode()

float busy(uint32_t c, int64_t i) {
  const double t = static_cast<double>(i) / kSr;
  return static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * 233.0 * t + c) + 0.3 * drum(i % (kHitFrames * kHits)));
}

}  // namespace

TEST_CASE("warp: Re-Pitch with markers reads every segment at its own speed, sample-exactly (rubato)") {
  // A sine of period 64 frames: the output's phase tells the source position.
  const double P = 64.0;
  Rig rig(64);
  rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
  e::AudioClipDesc c = warped(e::ClipWarp::RePitch, 1, kPpq / 2, kPpq * 8, 120.0);
  // Markers at beats 0, 2, 3, 5 of content with source frames 0, 40000, 52000, 100000: segments of rate 0.833, 0.5, 1,
  // and beyond the last at 1.
  const double f1 = 40000.0, f2 = 52000.0, f3 = 100000.0;
  c.segments = {{0, 0.0, f1 / (2.0 * kPpq)}, {kPpq * 2, f1, (f2 - f1) / kPpq}, {kPpq * 3, f2, (f3 - f2) / (2.0 * kPpq)}};
  auto img = rig.build({{1, {c}}}, {makeSample(200000, 1, [&](uint32_t, int64_t i) {
                          return static_cast<float>(std::sin(2.0 * std::numbers::pi * static_cast<double>(i) / P));
                        })});
  const e::AudioSequenceImage::Clip& ic = img->clips(0)[0];
  // The law itself: the marker frames at the marker times.
  CHECK(*img->sourceFrame(ic, rig.map.ticksToSamples(c.start + kPpq * 2, kSr)) == doctest::Approx(f1).epsilon(1e-9));
  CHECK(*img->sourceFrame(ic, rig.map.ticksToSamples(c.start + kPpq * 3, kSr)) == doctest::Approx(f2).epsilon(1e-9));
  std::vector<double> law;
  const SampleTime s0 = ic.bodyStart;
  for (SampleTime s = s0; s < ic.bodyEnd; ++s) law.push_back(*img->sourceFrame(ic, s));
  REQUIRE(rig.tl->publishAudio(std::move(img)).ok());
  rig.play(0);
  const std::vector<float> out = rig.render(static_cast<uint64_t>(s0 + static_cast<SampleTime>(law.size())));
  // Fit 256-sample windows across every segment (past the fade in): the phase error in frames.
  double worst = 0.0;
  int windows = 0;
  for (SampleTime w = s0 + 512; w + 256 < s0 + static_cast<SampleTime>(law.size()) - 512; w += 2000) {
    const double fA = law[static_cast<size_t>(w - s0)], fB = law[static_cast<size_t>(w - s0 + 255)];
    const double rate = (fB - fA) / 255.0;
    if (std::abs(rate - (law[static_cast<size_t>(w - s0 + 1)] - fA)) > 1e-6) continue;  // a segment boundary inside
    const double omega = 2.0 * std::numbers::pi * rate / P;
    double sc = 0, ss = 0, cc = 0, cs = 0, s2 = 0;
    for (int i = 0; i < 256; ++i) {
      const double y = out[static_cast<size_t>(w + i)], co = std::cos(omega * i), si = std::sin(omega * i);
      sc += y * co, ss += y * si, cc += co * co, s2 += si * si, cs += co * si;
    }
    const double det = cc * s2 - cs * cs;
    const double a = (sc * s2 - ss * cs) / det, b = (ss * cc - sc * cs) / det;
    double err = std::fmod(std::atan2(a, b) / (2.0 * std::numbers::pi) * P - std::fmod(fA, P) + 10.0 * P, P);
    if (err > P / 2) err -= P;
    worst = std::max(worst, std::abs(err));
    ++windows;
  }
  MESSAGE("Re-Pitch with markers: worst source position error ", worst, " frames over ", windows, " windows");
  CHECK(windows > 50);
  CHECK(worst < 0.01);
}

TEST_CASE("warp: Beats keeps a 120 BPM drum loop's hits within ±1 ms of the warped grid at 90 / 150 BPM") {
  const e::ClipSample loop = makeSample(kHitFrames * kHits, 1, [](uint32_t, int64_t i) { return drum(i); });
  const std::vector<float> src = [&] {
    std::vector<float> v(static_cast<size_t>(kHitFrames * kHits));
    for (size_t i = 0; i < v.size(); ++i) v[i] = drum(static_cast<int64_t>(i));
    return v;
  }();
  const double srcCentroid = centroid(src, 0, src.size());
  // The detector on the loop: every hit, within 0.5 ms (they start on exact frames).
  const float* ch = src.data();
  const std::vector<double> onsets = pq::media::detectTransients(&ch, 1, static_cast<int64_t>(src.size()), 48000);
  REQUIRE(onsets.size() == static_cast<size_t>(kHits));
  for (int h = 0; h < kHits; ++h) CHECK(std::abs(onsets[static_cast<size_t>(h)] - static_cast<double>(h * kHitFrames)) <= 24.0);
  struct Case {
    double bpm;
    Ticks granulation;
    e::SliceLoop loopMode;
  };
  for (const Case k : {Case{90.0, kPpq / 4, e::SliceLoop::Forward}, Case{150.0, kPpq / 4, e::SliceLoop::Forward},
                       Case{90.0, 0, e::SliceLoop::BackForward}, Case{150.0, 0, e::SliceLoop::Off},
                       Case{90.0, kPpq / 2, e::SliceLoop::Off}}) {
    CAPTURE(k.bpm);
    CAPTURE(k.granulation);
    CAPTURE(static_cast<int>(k.loopMode));
    Rig rig(256);
    rig.tempo({{0, k.bpm, pq::TempoCurve::Step}});
    e::AudioClipDesc c = warped(e::ClipWarp::Beats, 1, 0, kPpq * 8, 120.0);
    c.stretch.granulation = k.granulation;
    c.stretch.sliceLoop = k.loopMode;
    c.transients = onsets;
    rig.audio({{1, {c}}}, {loop});
    rig.play(0);
    const SampleTime end = rig.map.ticksToSamples(kPpq * 8, kSr);
    const std::vector<float> out = rig.render(static_cast<uint64_t>(end));
    double worst = 0.0;
    for (int h = 0; h < kHits; ++h) {
      const SampleTime expected = rig.map.ticksToSamples(static_cast<Ticks>(h) * kPpq / 2, kSr);
      const double off = std::abs(static_cast<double>(hitNear(out, expected, src, static_cast<int64_t>(h) * kHitFrames)));
      if (off > 48.0) MESSAGE("hit ", h, ": ", off, " samples from ", expected);
      worst = std::max(worst, off);
    }
    MESSAGE("Beats at ", k.bpm, " BPM: worst hit offset ", worst / 48.0, " ms");
    CHECK(worst <= 48.0);
    const double cen = centroid(out, 0, out.size());
    MESSAGE("centroid ", cen, " Hz (source ", srcCentroid, " Hz)");
    CHECK(std::abs(cen / srcCentroid - 1.0) <= 0.02);
    CHECK(rig.tl->clipPlayer().stats().slicePasses > 0);
  }
}

TEST_CASE("warp: Tones keeps a 220 Hz sine at 220 Hz ±0.5 ¢ at 0.5× / 2× tempo; +3 steps of 31-EDO is 3·1200/31 ¢") {
  const e::ClipSample sine = makeSample(static_cast<int64_t>(kSr * 20.0), 1, [](uint32_t, int64_t i) {
    return static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / kSr));
  });
  struct Case {
    double bpm;
    double ratio;
    double expectCents;
  };
  const double steps = 3.0 * 1200.0 / 31.0;
  for (const Case k : {Case{60.0, 1.0, 0.0}, Case{240.0, 1.0, 0.0}, Case{60.0, std::exp2(3.0 / 31.0), steps},
                       Case{240.0, std::exp2(3.0 / 31.0), steps}, Case{120.0, std::exp2(3.0 / 31.0), steps}}) {
    CAPTURE(k.bpm);
    CAPTURE(k.ratio);
    Rig rig(128);
    rig.tempo({{0, k.bpm, pq::TempoCurve::Step}});
    e::AudioClipDesc c = warped(e::ClipWarp::Tones, 1, 0, kPpq * 16, 120.0);
    c.stretch.pitchRatio = k.ratio;
    rig.audio({{1, {c}}}, {sine});
    rig.play(0);
    const std::vector<float> out = rig.render(static_cast<uint64_t>(kSr * 3.2));
    const double f = peakHz(out, 4800, 131072, 220.0 * k.ratio);
    const double err = cents(f, 220.0) - k.expectCents;
    MESSAGE("Tones at ", k.bpm, " BPM, ratio ", k.ratio, ": ", f, " Hz, error ", err, " cents");
    CHECK(std::abs(err) <= 0.5);
    double peak = 0.0;
    for (size_t i = 4800; i < out.size(); ++i) peak = std::max(peak, static_cast<double>(std::abs(out[i])));
    CHECK(peak > 0.35);
    CHECK(peak < 0.65);
    CHECK(rig.tl->clipPlayer().stats().grains > 100);
  }
}

TEST_CASE("warp: Texture keeps a stationary source's level; Flux is deterministic per clip") {
  const e::ClipSample sine = makeSample(static_cast<int64_t>(kSr * 10.0), 1, [](uint32_t, int64_t i) {
    return static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kSr));
  });
  std::vector<std::vector<float>> runs;
  for (int run = 0; run < 2; ++run) {
    Rig rig(run == 0 ? 64 : 1000);
    rig.tempo({{0, 70.0, pq::TempoCurve::Step}});
    e::AudioClipDesc c = warped(e::ClipWarp::Texture, 9, 0, kPpq * 8, 120.0);
    c.stretch.flux = 60.0f;
    rig.audio({{1, {c}}}, {sine});
    rig.play(0);
    runs.push_back(rig.render(static_cast<uint64_t>(kSr * 2.0)));
  }
  CHECK(runs[0] == runs[1]);
  double rms = 0.0;
  for (size_t i = 4800; i < runs[0].size(); ++i) rms += static_cast<double>(runs[0][i]) * runs[0][i];
  rms = std::sqrt(rms / static_cast<double>(runs[0].size() - 4800));
  MESSAGE("Texture RMS ", rms, " (source 0.354)");
  CHECK(rms > 0.2);
  CHECK(rms < 0.45);
}

TEST_CASE("warp: every mode renders bit-identically across engine blocks 64 / 1000 (loops, reverse, a ramp)") {
  const e::ClipSample s = makeSample(400000, 2, busy);
  const std::vector<e::AudioClipDesc> all = everyMode();
  for (size_t only = 0; only <= all.size(); ++only)
  for (const bool ramp : {false, true}) {
    CAPTURE(ramp);
    CAPTURE(only);  // all.size(): every clip at once (crossfaded)
    std::vector<std::vector<float>> results;
    for (uint32_t block : {64u, 1000u}) {
      Rig rig(block);
      if (ramp) rig.tempo({{0, 100.0, pq::TempoCurve::Linear}, {kEveryModeEnd, 150.0, pq::TempoCurve::Step}});
      else rig.tempo({{0, 128.0, pq::TempoCurve::Step}});
      std::vector<e::AudioClipDesc> clips = all;
      if (only < all.size()) {
        clips = {all[only]};
        clips[0].crossfadeIn = 0;
      }
      rig.audio({{1, clips}}, {s});
      rig.play(0);
      std::vector<float> r;
      std::vector<float> l = rig.render(static_cast<uint64_t>(rig.map.ticksToSamples(kEveryModeEnd, kSr)), 0, 0, &r);
      l.insert(l.end(), r.begin(), r.end());
      results.push_back(std::move(l));
    }
    size_t diff = 0;
    for (size_t k = 0; k < results[0].size(); ++k) diff += results[1][k] != results[0][k] ? 1u : 0u;
    CHECK(diff == 0);
    double peak = 0.0;
    for (float v : results[0]) peak = std::max(peak, static_cast<double>(std::abs(v)));
    CHECK(peak > 0.2);
  }
}

TEST_CASE("warp: a gain-only edit while playing keeps every stretch voice (seamless)") {
  const e::ClipSample s = makeSample(400000, 2, busy);
  for (const e::ClipWarp w : {e::ClipWarp::Beats, e::ClipWarp::Tones, e::ClipWarp::Texture, e::ClipWarp::Complex,
                             e::ClipWarp::ComplexPro}) {
    CAPTURE(static_cast<int>(w));
    auto clipWith = [&](float gain) {
      e::AudioClipDesc c = warped(w, 1, 0, kPpq * 8, 120.0);
      c.stretch.flux = 30.0f;
      c.stretch.pitchRatio = 1.07;
      c.gain = gain;
      return c;
    };
    // Reference: gain 0.5 from the start.
    Rig ref(64);
    ref.tempo({{0, 97.0, pq::TempoCurve::Step}});
    ref.audio({{1, {clipWith(0.5f)}}}, {s});
    ref.play(0);
    const std::vector<float> want = ref.render(48000);
    // Edited: gain 0.9, then 0.5 at 0.5 s.
    Rig rig(64);
    rig.tempo({{0, 97.0, pq::TempoCurve::Step}});
    rig.audio({{1, {clipWith(0.9f)}}}, {s});
    rig.play(0);
    std::vector<float> got = rig.render(24000);
    rig.audio({{1, {clipWith(0.5f)}}}, {s});
    const std::vector<float> rest = rig.render(24000);
    got.insert(got.end(), rest.begin(), rest.end());
    CHECK(rig.tl->clipPlayer().stats().kept == 1);
    CHECK(rig.tl->clipPlayer().stats().faded == 0);
    // After the 2 ms gain ramp: the same frames as a render at the new gain.
    size_t diff = 0;
    for (size_t i = 24000 + 200; i < 48000; ++i) diff += got[i] != want[i] ? 1u : 0u;
    CHECK(diff == 0);
  }
}

TEST_CASE("warp: streamed samples render bit-identically to resident ones (offline blocking fills), every mode") {
  const fs::path wav = writeWav("warp_streamed.wav", 400000, 2, busy);
  auto renderWith = [&](bool streamed) {
    pq::media::PoolConfig pc;
    pc.residentLimitBytes = streamed ? 0 : (1ull << 30);
    pc.headFrames = 4000;
    pc.streamSlots = 32;
    pc.ringFloats = 2 * 16384;
    pq::media::SamplePool pool(pc);
    pool.startDiskThread();
    auto smp = pool.load(wav);
    REQUIRE(smp.ok());
    REQUIRE((*smp)->view().streamed() == streamed);
    std::vector<float> out;
    uint64_t claims = 0;
    {
      Rig rig(128, &pool.streams());
      rig.tl->clipPlayer().setStreamFill(&fillWait, &pool);
      rig.tempo({{0, 111.0, pq::TempoCurve::Step}});
      e::ClipSample cs;
      cs.pooled = *smp;
      rig.audio({{1, everyMode()}}, {cs});
      pool.engage();
      rig.play(0);
      std::vector<float> r;
      out = rig.render(static_cast<uint64_t>(rig.map.ticksToSamples(kEveryModeEnd, kSr)), 0, 0, &r);
      out.insert(out.end(), r.begin(), r.end());
      pool.disengage();
      CHECK(rig.tl->clipPlayer().stats().missingFrames == 0);
      claims = rig.tl->clipPlayer().stats().sliceClaims;
    }
    pool.stopDiskThread();
    if (streamed) CHECK(claims > 0);
    return out;
  };
  const std::vector<float> res = renderWith(false);
  const std::vector<float> str = renderWith(true);
  REQUIRE(res.size() == str.size());
  size_t diff = 0;
  for (size_t i = 0; i < res.size(); ++i) diff += res[i] != str[i] ? 1u : 0u;
  CHECK(diff == 0);
  std::error_code ec;
  fs::remove_all(wav.parent_path(), ec);
}

TEST_CASE("rt: the warp modes (Re-Pitch markers, Beats, Tones, Texture, Complex, Complex Pro; streamed, swaps) allocate nothing") {
  REQUIRE(pq::alloc_trap::installed());
  const fs::path wav = writeWav("warp_rt.wav", 400000, 2, busy);
  pq::media::PoolConfig pc;
  pc.residentLimitBytes = 0;
  pc.headFrames = 2000;
  pc.streamSlots = 32;
  pc.ringFloats = 2 * 8192;
  pq::media::SamplePool pool(pc);
  pool.startDiskThread();
  auto streamed = pool.load(wav);
  REQUIRE(streamed.ok());
  {
    Rig rig(64, &pool.streams());
    rig.tempo({{0, 120.0, pq::TempoCurve::Linear}, {kPpq * 10, 90.0, pq::TempoCurve::Step}});
    e::ClipSample cs;
    cs.pooled = *streamed;
    const e::ClipSample res = makeSample(400000, 2, busy);
    std::vector<e::AudioClipDesc> a = everyMode(), b = everyMode();
    for (auto& c : b) c.sample = 1;  // the same clips on the resident copy, on slot 1
    for (auto& c : b) c.id += 100;
    auto img1 = rig.build({{1, a}, {2, b}}, {cs, res});
    for (auto& c : a) c.gain = 0.6f;
    auto img2 = rig.build({{1, a}, {2, b}}, {cs, res});
    rig.play(0);
    REQUIRE(rig.tl->publishAudio(std::move(img1)).ok());
    pool.engage();
    (void)rig.render(64);
    uint64_t violations = 0;
    for (int round = 0; round < 3; ++round) {
      violations += pq::alloc_trap::countRtViolations([&] {
        for (int k = 0; k < 4000; ++k) {
          rig.tl->process(rig.time, 64);
          rig.time += 64;
        }
      });
      if (round == 0) REQUIRE(rig.tl->publishAudio(std::move(img2)).ok());
    }
    CHECK(violations == 0);
    const e::ClipPlayerStats& st = rig.tl->clipPlayer().stats();
    CHECK(st.grains > 0);
    CHECK(st.slicePasses > 0);
    CHECK(st.stretchCells > 0);  // T6.5: Complex / Complex Pro rendered (streamed and resident)
    CHECK(st.stretchFallbacks == 0);
    pool.disengage();
  }
  pool.stopDiskThread();
  std::error_code ec;
  fs::remove_all(wav.parent_path(), ec);
}

// ---- T6.5: Complex / Complex Pro ---------------------------------------------------------------------------------------

namespace {

// A vowel-like source: a 150 Hz harmonic series (to 6 kHz) shaped by formants at 700 / 1200 / 2600 Hz.
constexpr double kVowelF0 = 150.0;
double vowelEnvelope(double f) {
  auto res = [&](double fc, double bw, double a) { return a / std::sqrt(1.0 + ((f - fc) / bw) * ((f - fc) / bw)); };
  return res(700.0, 110.0, 1.0) + res(1200.0, 140.0, 0.6) + res(2600.0, 220.0, 0.3) + 0.02;
}
float vowel(int64_t i) {
  double y = 0.0;
  const double t = static_cast<double>(i) / kSr;
  for (int h = 1; h * kVowelF0 < 6000.0; ++h)
    y += vowelEnvelope(h * kVowelF0) * std::sin(2.0 * std::numbers::pi * h * kVowelF0 * t + 0.7 * h * h);
  return static_cast<float>(0.12 * y);
}

// The formant shift of a render whose fundamental is f0: the factor α that best explains its harmonic amplitudes (to
// 4 kHz) as the source's envelope moved by α (least squares in log amplitude, a free gain).
double formantShift(const std::vector<float>& y, size_t from, size_t n, double f0) {
  std::vector<double> fh, la;
  for (int h = 1; h * f0 < 4000.0; ++h) {
    fh.push_back(h * f0);
    la.push_back(std::log(dtftMag(y, from, n, h * f0) + 1e-9));
  }
  double bestA = 1.0, bestE = 1e300;
  for (double a = 0.7; a <= 2.0; a += 0.0025) {
    std::vector<double> d(fh.size());
    double mean = 0.0;
    for (size_t k = 0; k < fh.size(); ++k) {
      d[k] = la[k] - std::log(vowelEnvelope(fh[k] / a));
      mean += d[k];
    }
    mean /= static_cast<double>(fh.size());
    double e = 0.0;
    for (const double x : d) e += (x - mean) * (x - mean);
    if (e < bestE) {
      bestE = e;
      bestA = a;
    }
  }
  return bestA;
}

// A ClipPlayer driven directly (its stretch pool size is the ClipPlayer's own config).
std::vector<std::vector<float>> playDirect(e::ClipPlayer& player, const e::AudioSequenceImage& img, uint32_t slots,
                                           SampleTime frames, uint32_t block) {
  std::vector<std::vector<float>> out(slots * 2);
  for (SampleTime s = 0; s < frames; s += block) {
    const auto n = static_cast<uint32_t>(std::min<SampleTime>(block, frames - s));
    player.beginBlock(n);
    if (s == 0) {
      player.setImage(&img, 0, true);
      player.seek(0);
    }
    player.play(0, s, n);
    player.endBlock();
    for (uint32_t k = 0; k < slots * 2; ++k) {
      const float* c = player.channels()[k];
      out[k].insert(out[k].end(), c, c + n);
    }
  }
  return out;
}

}  // namespace

TEST_CASE("warp: Complex / Complex Pro keep a 220 Hz sine within ±1 ¢ at 0.5× / 2× tempo; +3 steps of 31-EDO") {
  const e::ClipSample sine = makeSample(static_cast<int64_t>(kSr * 20.0), 1, [](uint32_t, int64_t i) {
    return static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / kSr));
  });
  struct Case {
    double bpm;
    double ratio;
    double expectCents;
  };
  const double steps = 3.0 * 1200.0 / 31.0;
  for (const e::ClipWarp w : {e::ClipWarp::Complex, e::ClipWarp::ComplexPro})
  for (const Case k : {Case{60.0, 1.0, 0.0}, Case{240.0, 1.0, 0.0}, Case{60.0, std::exp2(3.0 / 31.0), steps},
                       Case{240.0, std::exp2(3.0 / 31.0), steps}, Case{120.0, std::exp2(3.0 / 31.0), steps}}) {
    CAPTURE(static_cast<int>(w));
    CAPTURE(k.bpm);
    CAPTURE(k.ratio);
    Rig rig(128);
    rig.tempo({{0, k.bpm, pq::TempoCurve::Step}});
    e::AudioClipDesc c = warped(w, 1, 0, kPpq * 16, 120.0);
    c.stretch.pitchRatio = k.ratio;
    rig.audio({{1, {c}}}, {sine});
    rig.play(0);
    const std::vector<float> out = rig.render(static_cast<uint64_t>(kSr * 3.2));
    // Sustained: the pitch over the first and the second half of the window.
    double worst = 0.0;
    for (const size_t from : {size_t{4800}, size_t{4800 + 65536}}) {
      const double f = peakHz(out, from, 65536, 220.0 * k.ratio);
      const double err = cents(f, 220.0) - k.expectCents;
      const std::string name = w == e::ClipWarp::Complex ? "Complex" : "Complex Pro";
      MESSAGE(name, " at ", k.bpm, " BPM, ratio ", k.ratio, ": ", f, " Hz, error ", err, " cents");
      worst = std::max(worst, std::abs(err));
    }
    CHECK(worst <= 1.0);
    double peak = 0.0;
    for (size_t i = 4800; i < out.size(); ++i) peak = std::max(peak, static_cast<double>(std::abs(out[i])));
    CHECK(peak > 0.35);
    CHECK(peak < 0.65);
    const e::ClipPlayerStats& st = rig.tl->clipPlayer().stats();
    CHECK(st.stretchClaims == 1);
    CHECK(st.stretchFallbacks == 0);
    CHECK(st.stretchCells > 1000);
  }
}

TEST_CASE("warp: Complex is sample-aligned (no latency) and keeps a drum loop's onsets on the warped grid at 90 / 150 BPM") {
  const e::ClipSample loop = makeSample(kHitFrames * kHits, 1, [](uint32_t, int64_t i) { return drum(i); });
  const std::vector<float> src = [&] {
    std::vector<float> v(static_cast<size_t>(kHitFrames * kHits));
    for (size_t i = 0; i < v.size(); ++i) v[i] = drum(static_cast<int64_t>(i));
    return v;
  }();
  const double srcCentroid = centroid(src, 0, src.size());
  const float* sch = src.data();
  const std::vector<double> srcOnsets = pq::media::detectTransients(&sch, 1, static_cast<int64_t>(src.size()), 48000);
  REQUIRE(srcOnsets.size() == static_cast<size_t>(kHits));
  for (const double bpm : {120.0, 90.0, 150.0}) {
    CAPTURE(bpm);
    Rig rig(256);
    rig.tempo({{0, bpm, pq::TempoCurve::Step}});
    // The clip starts at beat 1 (its voice primes before it starts, in step with the song).
    e::AudioClipDesc c = warped(e::ClipWarp::Complex, 1, kPpq, kPpq * 8, 120.0);
    rig.audio({{1, {c}}}, {loop});
    rig.play(0);
    const SampleTime end = rig.map.ticksToSamples(kPpq * 9, kSr);
    const std::vector<float> out = rig.render(static_cast<uint64_t>(end));
    // Silent before the clip starts.
    const SampleTime s0 = rig.map.ticksToSamples(kPpq, kSr);
    double before = 0.0;
    for (SampleTime i = 0; i < s0; ++i) before = std::max(before, static_cast<double>(std::abs(out[static_cast<size_t>(i)])));
    CHECK(before == 0.0);
    // Every hit's onset (the transient detector on the output) where the warp puts the source's onset (the same
    // detector on the source: its own bias cancels). A stretched waveform is not the source's, so its onsets are
    // compared, not its samples.
    const float* och = out.data() + s0;
    const std::vector<double> got = pq::media::detectTransients(&och, 1, static_cast<int64_t>(out.size()) - s0, 48000);
    double worst = 0.0;
    int found = 1;
    for (int h = 1; h < kHits; ++h) {  // the clip's first hit: its start, checked below (the edge declick fades it in)
      const double srcOn = srcOnsets[static_cast<size_t>(h)];
      const double expected = static_cast<double>(rig.map.ticksToSamples(kPpq + static_cast<Ticks>(std::llround(srcOn / fptAt(120.0))), kSr) - s0);
      double best = 1e30;
      for (const double g : got) best = std::abs(g - expected) < std::abs(best) ? g - expected : best;
      if (std::abs(best) > 48.0) MESSAGE("hit ", h, ": onset ", best, " samples from ", expected);
      if (std::abs(best) < 480.0) ++found;
      worst = std::max(worst, std::abs(best));
    }
    MESSAGE("Complex at ", bpm, " BPM: worst onset offset ", worst / 48.0, " ms (", got.size(), " onsets)");
    CHECK(found == kHits);
    // ±2.5 ms: the vocoder's 120 ms windows smear attacks by a couple of ms when it stretches (measured 2.2 ms at 90
    // BPM, 1.7 at 150; Beats is the mode that keeps hits within ±1 ms); 0 at the source tempo.
    CHECK(worst <= 120.0);
    const double cen = centroid(out, static_cast<size_t>(s0), out.size());
    MESSAGE("centroid ", cen, " Hz (source ", srcCentroid, " Hz)");
    CHECK(std::abs(cen / srcCentroid - 1.0) <= 0.05);
    if (bpm == 120.0) {  // the source's own tempo: the output lines up with the source to the frame
      double bestC = -1e300;
      int bestLag = 0;
      for (int lag = -64; lag <= 64; ++lag) {
        double num = 0.0;
        for (int64_t i = 2000; i < 2 * kHitFrames * 4; ++i) num += out[static_cast<size_t>(s0 + i + lag)] * src[static_cast<size_t>(i)];
        if (num > bestC) {
          bestC = num;
          bestLag = lag;
        }
      }
      MESSAGE("Complex at the source tempo: lag ", bestLag, " frames");
      CHECK(std::abs(bestLag) <= 1);
      // The clip's first 20 ms (its first hit) against the source's, past the 2 ms declick.
      double err = 0.0, ref = 0.0;
      for (int64_t i = 96; i < 960; ++i) {
        const double d = out[static_cast<size_t>(s0 + i)] - src[static_cast<size_t>(i)];
        err += d * d;
        ref += static_cast<double>(src[static_cast<size_t>(i)]) * src[static_cast<size_t>(i)];
      }
      MESSAGE("Complex at the source tempo: the first 20 ms within ", 10.0 * std::log10(err / ref), " dB of the source");
      CHECK(err < 1e-8 * ref);
    }
  }
}

TEST_CASE("warp: Complex Pro holds a vowel's formants at +7 st (shift < 5 %); Complex moves them with the pitch (> 30 %)") {
  const e::ClipSample v = makeSample(static_cast<int64_t>(kSr * 6.0), 1, [](uint32_t, int64_t i) { return vowel(i); });
  const double ratio = std::exp2(7.0 / 12.0);
  double shift[2] = {0.0, 0.0};
  for (const e::ClipWarp w : {e::ClipWarp::Complex, e::ClipWarp::ComplexPro}) {
    Rig rig(256);
    rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
    e::AudioClipDesc c = warped(w, 1, 0, kPpq * 8, 120.0);
    c.stretch.pitchRatio = ratio;
    rig.audio({{1, {c}}}, {v});
    rig.play(0);
    const std::vector<float> out = rig.render(static_cast<uint64_t>(kSr * 3.0));
    const double f0 = peakHz(out, 24000, 65536, kVowelF0 * ratio);
    CHECK(std::abs(cents(f0, kVowelF0 * ratio)) <= 1.0);
    const double a = formantShift(out, 24000, 65536, f0);
    shift[w == e::ClipWarp::ComplexPro ? 1 : 0] = a;
    const std::string name = w == e::ClipWarp::ComplexPro ? "Complex Pro" : "Complex";
    MESSAGE(name, ": formant shift ", (a - 1.0) * 100.0, " %");
  }
  // The estimator on the source itself.
  std::vector<float> src(static_cast<size_t>(kSr * 3.0));
  for (size_t i = 0; i < src.size(); ++i) src[i] = vowel(static_cast<int64_t>(i));
  CHECK(std::abs(formantShift(src, 24000, 65536, kVowelF0) - 1.0) < 0.01);
  CHECK(std::abs(shift[1] - 1.0) < 0.05);
  CHECK(shift[0] - 1.0 > 0.30);
}

TEST_CASE("warp: an exhausted stretch pool plays Complex as Tones (counted, no click); renders are deterministic") {
  const e::ClipSample s = makeSample(400000, 2, busy);
  auto image = [&](e::ClipWarp second) {
    e::AudioClipDesc a = warped(e::ClipWarp::Complex, 1, 0, kPpq * 6, 120.0);
    a.stretch.pitchRatio = 1.05;
    e::AudioClipDesc b = warped(second, 2, kPpq, kPpq * 4, 97.0);  // starts while the first holds the only stretcher
    b.stretch.pitchRatio = 0.9;
    e::AudioBuildOptions o;
    o.sampleRate = kSr;
    auto r = e::AudioSequenceImage::build(std::vector<e::AudioTrackDesc>{{1, {a}}, {2, {b}}}, {s},
                                          tempoMap({{0, 100.0, pq::TempoCurve::Step}}), o);
    REQUIRE(r.ok());
    return std::move(r.value());
  };
  e::ClipPlayerConfig pc;
  pc.slots = 2;
  pc.maxBlockFrames = 1024;
  pc.sampleRate = kSr;
  pc.stretchVoices = 1;
  const SampleTime frames = static_cast<SampleTime>(kSr * 4.0);
  const auto img = image(e::ClipWarp::Complex);
  e::ClipPlayer p1(pc);
  REQUIRE(p1.stretchPool() != nullptr);
  const auto got = playDirect(p1, *img, 2, frames, 256);
  CHECK(p1.stats().stretchClaims == 1);
  CHECK(p1.stats().stretchFallbacks == 1);
  // The fallback voice is exactly a Tones clip.
  const auto tonesImg = image(e::ClipWarp::Tones);
  e::ClipPlayer p2(pc);
  const auto tones = playDirect(p2, *tonesImg, 2, frames, 256);
  CHECK(got[2] == tones[2]);
  CHECK(got[3] == tones[3]);
  // No click where it starts: the largest step there is no larger than the material's own.
  const SampleTime b0 = tempoMap({{0, 100.0, pq::TempoCurve::Step}}).ticksToSamples(kPpq, kSr);
  double stepAtStart = 0.0, stepLater = 0.0;
  for (SampleTime i = b0 - 64; i < b0 + 480; ++i)
    stepAtStart = std::max(stepAtStart, static_cast<double>(std::abs(got[2][static_cast<size_t>(i + 1)] - got[2][static_cast<size_t>(i)])));
  for (SampleTime i = b0 + 480; i < b0 + 48000; ++i)
    stepLater = std::max(stepLater, static_cast<double>(std::abs(got[2][static_cast<size_t>(i + 1)] - got[2][static_cast<size_t>(i)])));
  MESSAGE("fallback start: largest step ", stepAtStart, " (later ", stepLater, ")");
  CHECK(stepAtStart <= stepLater);
  // The first slot (with the stretcher) renders the same again, in another player and with another block.
  e::ClipPlayer p3(pc);
  const auto again = playDirect(p3, *img, 2, frames, 1000);
  CHECK(again[0] == got[0]);
  CHECK(again[1] == got[1]);
  // A 3× stretch (the library randomises phases beyond 2×): deterministic too.
  e::ClipPlayerConfig pc2 = pc;
  pc2.stretchVoices = 2;
  auto slow = [&] {
    e::AudioClipDesc a = warped(e::ClipWarp::Complex, 5, 0, kPpq * 4, 120.0);
    e::AudioBuildOptions o;
    o.sampleRate = kSr;
    auto r = e::AudioSequenceImage::build(std::vector<e::AudioTrackDesc>{{1, {a}}}, {s}, tempoMap({{0, 40.0, pq::TempoCurve::Step}}), o);
    REQUIRE(r.ok());
    e::ClipPlayer p(pc2);
    return playDirect(p, *r.value(), 1, frames, 256);
  };
  CHECK(slow()[0] == slow()[0]);
  const auto t0 = std::chrono::steady_clock::now();
  const e::StretchPool pool32(32, kSr);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  MESSAGE("stretch pool of 32 (the default) at 48 kHz: ", static_cast<double>(pool32.memoryBytes()) / 1048576.0,
          " MiB (estimated), made in ", ms, " ms");
}

// ---- T6.5b: no priming burst; mono stretchers ------------------------------------------------------------------------

namespace {

// N Complex clips (one per slot) over [0, 16 beats) of a stereo source at 120 BPM (its own tempo).
std::vector<e::AudioTrackDesc> complexTracks(uint32_t n) {
  std::vector<e::AudioTrackDesc> t;
  for (uint32_t k = 0; k < n; ++k) {
    e::AudioClipDesc c = warped(e::ClipWarp::Complex, 1 + k, 0, kPpq * 16, 120.0);
    c.stretch.pitchRatio = k % 2 == 0 ? 1.0 : 1.09;  // copies and sinc reads
    t.push_back({1 + k, {c}});
  }
  return t;
}

void command(Rig& rig, e::TransportCommandKind kind, Ticks tick) {
  e::TransportCommand c;
  c.kind = kind;
  c.tick = tick;
  rig.tl->apply(c);
}

double rmsOf(const std::vector<float>& x, size_t from, size_t to) {
  double e = 0.0;
  for (size_t i = from; i < to; ++i) e += static_cast<double>(x[i]) * x[i];
  return std::sqrt(e / static_cast<double>(to - from));
}

// The most vocoder chunks one 256-frame block may run per voice: priming at kStretchPrimeRate × the playing rate (pitch
// rate ≤ 1.09: ≤ 9 chunks) or the block a voice starts in (a playing block, the priming schedule's step and the
// resampler's reach: ≤ 13), rounded up.
constexpr uint64_t kCellsPerVoiceBlock = 16;

}  // namespace

TEST_CASE("T6.5b: Play inside 8 Complex clips never primes in a burst; uncued it starts late, cued on time, same audio") {
  const e::ClipSample s = makeSample(48000 * 10, 2, busy);
  constexpr uint32_t kN = 8;
  const Ticks at = kPpq * 2;  // 1 s into every clip
  const uint64_t frames = 48000;
  // Uncued: Play straight into the clips.
  Rig a(256);
  a.tempo({{0, 120.0, pq::TempoCurve::Step}});
  a.audio(complexTracks(kN), {s});
  a.play(at);
  std::vector<std::vector<float>> late(kN);
  for (uint64_t done = 0; done < frames; done += 256) {
    a.tl->process(a.time, 256);
    a.time += 256;
    for (uint32_t k = 0; k < kN; ++k) {
      const float* c = a.tl->clipPlayer().channels()[k * e::kClipChannels];
      late[k].insert(late[k].end(), c, c + 256);
    }
  }
  const e::ClipPlayerStats& sa = a.tl->clipPlayer().stats();
  MESSAGE("uncued Play inside ", kN, " Complex clips: at most ", sa.stretchCellsBlockMax, " vocoder chunks in a block (",
          sa.stretchLateStarts, " late starts; before T6.5b about 80 per voice in the first block)");
  CHECK(sa.stretchClaims == kN);
  CHECK(sa.stretchLateStarts == kN);
  CHECK(sa.stretchCellsBlockMax <= kN * kCellsPerVoiceBlock);
  // Cued: stopped at the position for 30 blocks (160 ms), then Play there.
  Rig b(256);
  b.tempo({{0, 120.0, pq::TempoCurve::Step}});
  b.audio(complexTracks(kN), {s});
  command(b, e::TransportCommandKind::Locate, at);
  for (int k = 0; k < 30; ++k) {
    b.tl->process(b.time, 256);
    b.time += 256;
  }
  CHECK(b.tl->clipPlayer().stats().stretchCues == kN);
  CHECK(b.tl->clipPlayer().anyActive() == false);  // cues are silent
  b.play(at);
  std::vector<std::vector<float>> cued(kN);
  for (uint64_t done = 0; done < frames; done += 256) {
    b.tl->process(b.time, 256);
    b.time += 256;
    for (uint32_t k = 0; k < kN; ++k) {
      const float* c = b.tl->clipPlayer().channels()[k * e::kClipChannels];
      cued[k].insert(cued[k].end(), c, c + 256);
    }
  }
  const e::ClipPlayerStats& sb = b.tl->clipPlayer().stats();
  MESSAGE("cued Play: at most ", sb.stretchCellsBlockMax, " vocoder chunks in a block");
  CHECK(sb.stretchCuesAdopted == kN);
  CHECK(sb.stretchLateStarts == 0);
  CHECK(sb.stretchClaims == kN);  // the cues' claims are the voices'
  CHECK(sb.stretchCellsBlockMax <= kN * kCellsPerVoiceBlock);
  for (uint32_t k = 0; k < kN; ++k) {
    CAPTURE(k);
    // Cued: sounding from the Play position (past the 2 ms fade in).
    CHECK(rmsOf(cued[k], 96, 960) > 0.1);
    // Uncued: silent until its schedule caught up (< 100 ms), then the very same audio as the cued voice (same song
    // time, same vocoder frames: no wrong-time audio), faded in.
    size_t first = 0;
    while (first < late[k].size() && late[k][first] == 0.0f) ++first;
    MESSAGE("voice ", k, " uncued: starts ", static_cast<double>(first) / 48.0, " ms late");
    CHECK(first > 0);
    CHECK(first < 4800);
    size_t diff = 0;
    for (size_t i = first + 96; i < late[k].size(); ++i) diff += late[k][i] != cued[k][i] ? 1u : 0u;
    CHECK(diff == 0);
    // No click where it starts: no step larger than the material's own.
    double stepStart = 0.0, stepLater = 0.0;
    for (size_t i = first; i < first + 96; ++i)
      stepStart = std::max(stepStart, static_cast<double>(std::abs(late[k][i + 1] - late[k][i])));
    for (size_t i = first + 960; i < first + 24000; ++i)
      stepLater = std::max(stepLater, static_cast<double>(std::abs(late[k][i + 1] - late[k][i])));
    CHECK(stepStart <= stepLater);
  }
}

TEST_CASE("T6.5b: a loop wrap into Complex clips starts primed (cued before the wrap), no burst; RtScope") {
  REQUIRE(pq::alloc_trap::installed());
  const e::ClipSample s = makeSample(48000 * 10, 2, busy);
  constexpr uint32_t kN = 4;
  Rig rig(256);
  rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
  rig.audio(complexTracks(kN), {s});
  e::TransportCommand loop;
  loop.kind = e::TransportCommandKind::SetLoop;
  loop.loopEnabled = true;
  loop.loopStart = kPpq * 2;
  loop.loopEnd = kPpq * 6;  // 2 s
  rig.tl->apply(loop);
  command(rig, e::TransportCommandKind::Locate, kPpq * 4);
  std::vector<float> out;
  out.reserve(48000 * 6);
  uint64_t violations = pq::alloc_trap::countRtViolations([&] {
    for (int k = 0; k < 20; ++k) {  // stopped: cues
      rig.tl->process(rig.time, 256);
      rig.time += 256;
    }
  });
  command(rig, e::TransportCommandKind::Play, kPpq * 4);
  const SampleTime wrapAt = 48000;  // 1 s to the loop end, then every 2 s
  for (int k = 0; k < 48000 * 5 / 256; ++k) {
    violations += pq::alloc_trap::countRtViolations([&] {
      rig.tl->process(rig.time, 256);
      rig.time += 256;
    });
    const float* c = rig.tl->clipPlayer().channels()[0];
    out.insert(out.end(), c, c + 256);
  }
  CHECK(violations == 0);
  const e::ClipPlayerStats& st = rig.tl->clipPlayer().stats();
  MESSAGE("loop: ", st.stretchCuesAdopted, " cues adopted, ", st.stretchLateStarts, " late starts, at most ",
          st.stretchCellsBlockMax, " chunks in a block");
  CHECK(rig.tl->stats().loopWraps == 2);
  CHECK(st.stretchCuesAdopted == kN * 3);  // the Play and two wraps
  CHECK(st.stretchLateStarts == 0);
  CHECK(st.stretchCellsBlockMax <= kN * kCellsPerVoiceBlock);
  CHECK(rmsOf(out, 96, 960) > 0.1);
  for (const SampleTime w : {wrapAt, wrapAt + 96000})  // sounding right after each wrap (past the fade in)
    CHECK(rmsOf(out, static_cast<size_t>(w + 96), static_cast<size_t>(w + 960)) > 0.1);
}

TEST_CASE("T6.5b: a mono source runs a mono stretcher (half the work), the same audio as through a stereo one") {
  const e::ClipSample v = makeSample(static_cast<int64_t>(kSr * 6.0), 1, [](uint32_t, int64_t i) { return vowel(i); });
  auto img = [&] {
    e::AudioClipDesc c = warped(e::ClipWarp::ComplexPro, 1, kPpq, kPpq * 6, 120.0);
    c.stretch.pitchRatio = std::exp2(5.0 / 12.0);
    e::AudioClipDesc d = warped(e::ClipWarp::Complex, 2, kPpq, kPpq * 6, 97.0);
    e::AudioBuildOptions o;
    o.sampleRate = kSr;
    auto r = e::AudioSequenceImage::build(std::vector<e::AudioTrackDesc>{{1, {c}}, {2, {d}}}, {v},
                                          tempoMap({{0, 120.0, pq::TempoCurve::Step}}), o);
    REQUIRE(r.ok());
    return std::move(r.value());
  }();
  e::ClipPlayerConfig pc;
  pc.slots = 2;
  pc.maxBlockFrames = 256;
  pc.sampleRate = kSr;
  pc.stretchVoices = 2;
  pc.stretchMonoVoices = 2;
  e::ClipPlayer mono(pc);
  const auto a = playDirect(mono, *img, 2, static_cast<SampleTime>(kSr * 3.0), 256);
  CHECK(mono.stats().stretchMonoClaims == 2);
  pc.stretchMonoVoices = 0;  // no mono stretchers: a stereo one, fed the channel twice (as in T6.5)
  e::ClipPlayer stereo(pc);
  const auto b = playDirect(stereo, *img, 2, static_cast<SampleTime>(kSr * 3.0), 256);
  CHECK(stereo.stats().stretchMonoClaims == 0);
  CHECK(stereo.stats().stretchClaims == 2);
  CHECK(mono.stats().stretchCells == stereo.stats().stretchCells);
  const e::StretchPool monoPool(0, kSr, 32);
  CHECK(monoPool.size() == 32);
  CHECK(monoPool.availableMono() == 32);
  MESSAGE("32 mono stretchers (the default): ", static_cast<double>(monoPool.memoryBytes()) / 1048576.0, " MiB (estimated)");
  for (const uint32_t k : {0u, 2u}) {
    CAPTURE(k);
    double peak = 0.0, worst = 0.0;
    for (size_t i = 0; i < a[k].size(); ++i) {
      peak = std::max(peak, static_cast<double>(std::abs(b[k][i])));
      worst = std::max(worst, static_cast<double>(std::abs(a[k][i] - b[k][i])));
    }
    MESSAGE("mono vs stereo stretcher: peak ", peak, ", largest difference ", worst);
    CHECK(peak > 0.1);
    CHECK(worst == 0.0);
  }
}
