// M6 review (warp): shared helpers for the probes (modelled on tests/engine/test_warp.cpp's Rig).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <numbers>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/engine/audio_sequence.h"
#include "pq/engine/clip_player.h"
#include "pq/engine/timeline.h"

namespace e = pq::engine;
using pq::kPpq;
using pq::SampleTime;
using pq::Ticks;

constexpr double kSr = 48000.0;
constexpr int64_t kG = pq::media::kSampleGuardFrames;

struct Frames {
  pq::AlignedBuffer<float> buf;
};

inline e::ClipSample makeSample(int64_t frames, uint32_t channels, const std::function<float(uint32_t, int64_t)>& fn) {
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

struct Rig {
  e::TimelineConfig cfg;
  std::unique_ptr<e::Timeline> tl;
  pq::TempoMap map;
  int64_t time = 0;
  double worstBlockUs = 0.0;
  double worstMcycles = 0.0;     // thread cycles (excludes preemption by other processes)
  int64_t worstBlockIndex = -1, blocks = 0;
  explicit Rig(uint32_t block = 64, uint32_t tracks = 8) {
    cfg.sampleRate = kSr;
    cfg.maxBlockFrames = block;
    cfg.maxTracks = tracks;
    cfg.clipDeclickSeconds = 0.002;
    tl = std::make_unique<e::Timeline>(cfg);
  }
  bool tempo(std::vector<pq::TempoPoint> pts) {
    auto m = pq::TempoMap::create(pts);
    if (!m.ok()) return false;
    map = std::move(m.value());
    auto img = e::TempoImage::create(pts, {});
    return img.ok() && tl->publishTempo(std::move(img.value())).ok();
  }
  bool audio(std::vector<e::AudioTrackDesc> slots, std::vector<e::ClipSample> samples) {
    e::AudioBuildOptions o;
    o.sampleRate = kSr;
    auto r = e::AudioSequenceImage::build(slots, std::move(samples), map, o);
    if (!r.ok()) {
      std::printf("build failed: %s\n", r.error().message.c_str());
      return false;
    }
    return tl->publishAudio(std::move(r.value())).ok();
  }
  void command(e::TransportCommandKind k, Ticks t) {
    e::TransportCommand c;
    c.kind = k;
    c.tick = t;
    tl->apply(c);
  }
  void play(Ticks t = 0) { command(e::TransportCommandKind::Play, t); }
  void step(uint32_t n) {
    ULONG64 c0 = 0, c1 = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c0);
    const auto t0 = std::chrono::steady_clock::now();
    tl->process(time, n);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    QueryThreadCycleTime(GetCurrentThread(), &c1);
    worstBlockUs = std::max(worstBlockUs, us);
    const double mc = static_cast<double>(c1 - c0) / 1e6;
    if (mc > worstMcycles) {
      worstMcycles = mc;
      worstBlockIndex = blocks;
    }
    ++blocks;
    time += n;
  }
  std::vector<float> render(uint64_t frames, uint32_t slot = 0, uint32_t ch = 0) {
    std::vector<float> out;
    out.reserve(frames);
    for (uint64_t done = 0; done < frames;) {
      const auto n = static_cast<uint32_t>(std::min<uint64_t>(cfg.maxBlockFrames, frames - done));
      step(n);
      const float* p = tl->clipPlayer().channels()[slot * e::kClipChannels + ch];
      out.insert(out.end(), p, p + n);
      done += n;
    }
    return out;
  }
};

inline double fptAt(double bpm) { return kSr * 60.0 / (bpm * static_cast<double>(kPpq)); }

inline e::AudioClipDesc warped(e::ClipWarp w, uint32_t id, Ticks start, Ticks length, double sourceBpm) {
  e::AudioClipDesc c;
  c.id = id;
  c.warp = w;
  c.start = start;
  c.length = length;
  c.framesPerTick = fptAt(sourceBpm);
  c.stretch.seed = id;
  return c;
}

inline const char* modeName(e::ClipWarp w) {
  switch (w) {
    case e::ClipWarp::Off: return "Off";
    case e::ClipWarp::RePitch: return "RePitch";
    case e::ClipWarp::Beats: return "Beats";
    case e::ClipWarp::Tones: return "Tones";
    case e::ClipWarp::Texture: return "Texture";
    case e::ClipWarp::Complex: return "Complex";
    case e::ClipWarp::ComplexPro: return "ComplexPro";
  }
  return "?";
}

struct Stat {
  int nonfinite = 0;
  double peak = 0.0, maxStep = 0.0;
};
inline Stat stat(const std::vector<float>& x) {
  Stat s;
  for (size_t i = 0; i < x.size(); ++i) {
    if (!std::isfinite(x[i])) {
      ++s.nonfinite;
      continue;
    }
    s.peak = std::max(s.peak, static_cast<double>(std::abs(x[i])));
    if (i > 0 && std::isfinite(x[i - 1])) s.maxStep = std::max(s.maxStep, static_cast<double>(std::abs(x[i] - x[i - 1])));
  }
  return s;
}

// |DTFT| of a Hann-windowed stretch at f Hz (as tests/engine/test_warp.cpp).
inline double dtftMag(const std::vector<float>& x, size_t from, size_t n, double f) {
  const double w = 2.0 * std::numbers::pi * f / kSr;
  double re = 0.0, im = 0.0, c = 1.0, s = 0.0;
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
inline double peakHz(const std::vector<float>& x, size_t from, size_t n, double guess) {
  double best = guess - 3.0, bestM = -1.0;
  for (double f = guess - 3.0; f <= guess + 3.0; f += 0.05) {
    const double m = dtftMag(x, from, n, f);
    if (m > bestM) {
      bestM = m;
      best = f;
    }
  }
  double lo = best - 0.05, hi = best + 0.05;
  const double g = (std::sqrt(5.0) - 1.0) / 2.0;
  double a = hi - g * (hi - lo), b = lo + g * (hi - lo);
  double fa = dtftMag(x, from, n, a), fb = dtftMag(x, from, n, b);
  for (int i = 0; i < 40; ++i) {
    if (fa > fb) {
      hi = b; b = a; fb = fa; a = hi - g * (hi - lo); fa = dtftMag(x, from, n, a);
    } else {
      lo = a; a = b; fa = fb; b = lo + g * (hi - lo); fb = dtftMag(x, from, n, b);
    }
  }
  return 0.5 * (lo + hi);
}
inline double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }
