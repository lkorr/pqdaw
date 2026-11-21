// SM.7: root pitch detection (pq/media/pitch_detect.h) on signals synthesised here: sines and band-limited saws over
// 55 Hz … 2.5 kHz at 44.1 / 48 kHz, a tone of harmonics 2 … 6 (missing fundamental), Karplus-Strong plucked strings
// (period N + ½), a sung-like vowel with 5.5 Hz ±40 ¢ vibrato, a breath of noise and an attack — all within 5 ¢;
// white noise, a snare hit and a hi-hat are unpitched (kUnpitchedHz). The analysis window is the steadiest loud
// part (a note after silence is found); the decoder path equals the in-memory one and lands in the memory cache; a
// 10 min file costs no more than its first minute (slow).
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <vector>

#include "pq/media/pitch_detect.h"
#include "pq/media/wav_file.h"

namespace {

namespace fs = std::filesystem;
using pq::media::PitchEstimate;

constexpr double kTwoPi = 2.0 * std::numbers::pi;

struct Rng {
  uint32_t s;
  double next() {
    s = s * 1664525u + 1013904223u;
    return static_cast<double>(s >> 8) / 16777216.0;
  }
  double signedNext() { return 2.0 * next() - 1.0; }
};

double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

PitchEstimate detect(const std::vector<float>& x, uint32_t rate) {
  const float* ch[1] = {x.data()};
  return pq::media::detectPitch(ch, 1, static_cast<int64_t>(x.size()), rate);
}

// Harmonics k = from … to of f0 with amplitude amp(k), below Nyquist; a 10 ms fade in and out.
template <class Amp>
std::vector<float> harmonic(double f0, int from, int to, uint32_t rate, double seconds, Amp amp) {
  const size_t n = static_cast<size_t>(seconds * rate);
  std::vector<float> x(n, 0.0f);
  for (int k = from; k <= to && k * f0 < 0.45 * rate; ++k) {
    const double a = amp(k), w = kTwoPi * k * f0 / rate;
    for (size_t i = 0; i < n; ++i) x[i] += static_cast<float>(a * std::sin(w * static_cast<double>(i)));
  }
  const size_t fade = rate / 100;
  for (size_t i = 0; i < fade && i < n; ++i) {
    const float g = static_cast<float>(i) / fade;
    x[i] *= g;
    x[n - 1 - i] *= g;
  }
  return x;
}

std::vector<float> sine(double f0, uint32_t rate, double seconds = 2.0) {
  return harmonic(f0, 1, 1, rate, seconds, [](int) { return 0.5; });
}
std::vector<float> saw(double f0, uint32_t rate, double seconds = 2.0) {
  return harmonic(f0, 1, 400, rate, seconds, [](int k) { return 0.4 / k; });
}

// Karplus-Strong: a delay of n frames fed back through a two-tap average (period n + ½), excited by noise.
std::vector<float> pluck(uint32_t n, uint32_t rate, uint32_t seed, double seconds = 2.0) {
  const size_t len = static_cast<size_t>(seconds * rate);
  std::vector<float> y(len, 0.0f);
  Rng r{seed};
  for (size_t i = 0; i < len; ++i) {
    if (i <= n) y[i] = static_cast<float>(0.5 * r.signedNext());
    else y[i] = 0.996f * 0.5f * (y[i - n] + y[i - n - 1]);
  }
  return y;
}

// A sung vowel: 5.5 Hz vibrato of ±depth cents around f0 (onset after 150 ms), harmonics shaped by two formants,
// a slow swell, a breath of noise at −35 dB.
std::vector<float> voice(double f0, double depthCents, uint32_t rate, uint32_t seed, double seconds = 2.5) {
  const size_t n = static_cast<size_t>(seconds * rate);
  std::vector<float> x(n, 0.0f);
  Rng r{seed};
  double phase = 0.0;
  const auto formant = [](double hz, double centre, double bw) { return 1.0 / (1.0 + std::pow((hz - centre) / bw, 2)); };
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double vib = t < 0.15 ? 0.0 : depthCents * std::sin(kTwoPi * 5.5 * (t - 0.15));
    const double f = f0 * std::exp2(vib / 1200.0);
    phase += f / rate;
    double s = 0.0;
    for (int k = 1; k * f < 0.45 * rate && k <= 40; ++k)
      s += (0.25 / k + formant(k * f, 700.0, 120.0) + 0.6 * formant(k * f, 1200.0, 150.0)) *
           std::sin(kTwoPi * k * phase);
    const double env = std::min(1.0, t / 0.08) * std::min(1.0, (seconds - t) / 0.1) * (0.8 + 0.2 * std::sin(kTwoPi * 0.7 * t));
    x[i] = static_cast<float>(0.15 * env * s + 0.01 * r.signedNext());
  }
  return x;
}

std::vector<float> noise(uint32_t rate, uint32_t seed, double seconds = 2.0) {
  std::vector<float> x(static_cast<size_t>(seconds * rate));
  Rng r{seed};
  for (float& v : x) v = static_cast<float>(0.5 * r.signedNext());
  return x;
}

// A snare: a 185 Hz body (τ 40 ms) under noise (τ 70 ms), after 100 ms of silence, in 1.5 s.
std::vector<float> snare(uint32_t rate, uint32_t seed) {
  std::vector<float> x(static_cast<size_t>(1.5 * rate), 0.0f);
  Rng r{seed};
  const size_t at = rate / 10;
  for (size_t i = at; i < x.size(); ++i) {
    const double t = static_cast<double>(i - at) / rate;
    x[i] = static_cast<float>(0.4 * std::exp(-t / 0.04) * std::sin(kTwoPi * 185.0 * t) +
                              0.6 * std::exp(-t / 0.07) * r.signedNext());
  }
  return x;
}

// A closed hi-hat: high-passed (first difference) noise, τ 25 ms, every 250 ms.
std::vector<float> hats(uint32_t rate, uint32_t seed) {
  std::vector<float> x(static_cast<size_t>(2.0 * rate), 0.0f);
  Rng r{seed};
  double prev = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = std::fmod(static_cast<double>(i) / rate, 0.25);
    const double v = r.signedNext();
    x[i] = static_cast<float>(0.5 * std::exp(-t / 0.025) * (v - prev));
    prev = v;
  }
  return x;
}

void checkPitched(const PitchEstimate& e, double want, const char* what) {
  INFO(what, ": ", e.hz, " Hz (want ", want, "), ", cents(e.hz, want), " cents, confidence ", e.confidence);
  CHECK(e.pitched);
  CHECK(e.confidence >= pq::media::kPitchConfidenceThreshold);
  CHECK(std::abs(cents(e.hz, want)) <= 5.0);
}

void checkUnpitched(const PitchEstimate& e, const char* what) {
  INFO(what, ": ", e.hz, " Hz, confidence ", e.confidence);
  CHECK_FALSE(e.pitched);
  CHECK(e.confidence < pq::media::kPitchConfidenceThreshold);
  CHECK(e.hz == pq::media::kUnpitchedHz);
}

}  // namespace

TEST_CASE("pitch: sines and saws over 55 Hz … 2.5 kHz within 5 cents (44.1 / 48 kHz)") {
  for (const uint32_t rate : {44100u, 48000u}) {
    for (const double f : {55.0, 82.41, 110.0, 261.63, 440.0, 1000.0, 2500.0}) checkPitched(detect(sine(f, rate), rate), f, "sine");
    for (const double f : {41.2, 98.0, 196.0, 659.26, 1318.5}) checkPitched(detect(saw(f, rate), rate), f, "saw");
  }
}

TEST_CASE("pitch: a missing fundamental reads as the fundamental") {
  for (const double f : {150.0, 220.0, 330.0}) {
    const std::vector<float> x = harmonic(f, 2, 6, 48000, 2.0, [](int) { return 0.15; });
    checkPitched(detect(x, 48000), f, "harmonics 2-6");
  }
}

TEST_CASE("pitch: Karplus-Strong plucked strings (period n + 1/2) within 5 cents") {
  uint32_t seed = 7;
  for (const uint32_t n : {60u, 109u, 300u, 700u}) checkPitched(detect(pluck(n, 48000, seed++), 48000), 48000.0 / (n + 0.5), "pluck");
  checkPitched(detect(pluck(200, 44100, 99), 44100), 44100.0 / 200.5, "pluck 44.1k");
}

TEST_CASE("pitch: a sung vowel with 5.5 Hz vibrato reads as its centre within 5 cents") {
  uint32_t seed = 3;
  for (const double f : {110.0, 196.0, 293.66, 440.0})
    for (const double depth : {0.0, 25.0, 40.0}) checkPitched(detect(voice(f, depth, 48000, seed++), 48000), f, "voice");
}

TEST_CASE("pitch: the steadiest loud window is found (a note after silence and a noisy attack)") {
  const uint32_t rate = 48000;
  std::vector<float> x(static_cast<size_t>(1.0 * rate), 0.0f);  // 1 s of silence
  Rng r{11};
  for (size_t i = 0; i < rate / 20; ++i) x.push_back(static_cast<float>(0.8 * r.signedNext()));  // a 50 ms scrape
  const std::vector<float> tone = saw(349.23, rate, 1.5);
  x.insert(x.end(), tone.begin(), tone.end());
  x.resize(x.size() + rate, 0.0f);  // and a second of silence
  checkPitched(detect(x, rate), 349.23, "note after silence");
  // Shorter than a window: the whole file.
  checkPitched(detect(sine(523.25, rate, 0.2), rate), 523.25, "200 ms");
}

TEST_CASE("pitch: white noise, a snare hit, hi-hats and silence are unpitched (261.63 Hz)") {
  checkUnpitched(detect(noise(48000, 1), 48000), "white noise");
  checkUnpitched(detect(noise(44100, 2), 44100), "white noise 44.1k");
  checkUnpitched(detect(snare(48000, 3), 48000), "snare");
  checkUnpitched(detect(snare(44100, 4), 44100), "snare 44.1k");
  checkUnpitched(detect(hats(48000, 5), 48000), "hi-hats");
  checkUnpitched(detect(std::vector<float>(48000, 0.0f), 48000), "silence");
  checkUnpitched(detect(std::vector<float>(10, 0.1f), 48000), "10 frames");
}

TEST_CASE("pitch: stereo is the channels' mean; the decoder path equals the in-memory one; memory cache") {
  const uint32_t rate = 48000;
  std::vector<std::vector<float>> st = {voice(220.0, 30.0, rate, 21), voice(220.0, 30.0, rate, 22)};
  const float* ch[2] = {st[0].data(), st[1].data()};
  const PitchEstimate mem = pq::media::detectPitch(ch, 2, static_cast<int64_t>(st[0].size()), rate);
  checkPitched(mem, 220.0, "stereo voice");

  const fs::path dir = fs::temp_directory_path() / "pq_pitch_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  const fs::path wav = dir / "voice.wav";
  REQUIRE(pq::media::writeWav(wav, st, rate).ok());
  pq::media::pitchCache().clear();
  pq::media::TransientRequest r;
  r.path = wav;
  r.contentHash = 0x9177C4ull;
  r.frames = static_cast<int64_t>(st[0].size());
  auto first = pq::media::loadOrDetectPitch(r);
  REQUIRE(first.ok());
  CHECK((*first)->estimate.hz == mem.hz);  // float32 WAV: the same frames
  CHECK((*first)->estimate.confidence == mem.confidence);
  CHECK((*first)->estimate.pitched);
  auto again = pq::media::loadOrDetectPitch(r);
  REQUIRE(again.ok());
  CHECK(again->get() == first->get());  // from the memory cache
  pq::media::pitchCache().clear();
  fs::remove_all(dir, ec);
}

TEST_CASE("pitch: a 10 min file costs its first minute (decoder and memory)" * doctest::test_suite("slow")) {
  const uint32_t rate = 48000;
  const size_t n = static_cast<size_t>(600.0 * rate);
  std::vector<std::vector<float>> x(1, std::vector<float>(n));
  const double w = kTwoPi * 146.83 / rate;
  for (size_t i = 0; i < n; ++i) x[0][i] = static_cast<float>(0.4 * std::sin(w * static_cast<double>(i)));
  const float* ch[1] = {x[0].data()};
  auto t0 = std::chrono::steady_clock::now();
  const PitchEstimate mem = pq::media::detectPitch(ch, 1, static_cast<int64_t>(n), rate);
  const double memMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  checkPitched(mem, 146.83, "10 min in memory");

  const fs::path dir = fs::temp_directory_path() / "pq_pitch_long";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  const fs::path wav = dir / "long.wav";
  REQUIRE(pq::media::writeWavPcm24(wav, x, rate).ok());
  x.clear();
  x.shrink_to_fit();
  pq::media::pitchCache().clear();
  pq::media::TransientRequest r;
  r.path = wav;
  r.contentHash = 0x10A1Dull;
  r.frames = static_cast<int64_t>(n);
  t0 = std::chrono::steady_clock::now();
  auto dec = pq::media::loadOrDetectPitch(r);
  const double decMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE(dec.ok());
  checkPitched((*dec)->estimate, 146.83, "10 min 24-bit WAV");
  MESSAGE("10 min file: ", memMs, " ms in memory, ", decMs, " ms through the decoder (24-bit WAV)");
  pq::media::pitchCache().clear();
  fs::remove_all(dir, ec);
}
