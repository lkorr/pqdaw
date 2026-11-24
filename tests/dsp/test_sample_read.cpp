// SM.2: the sample atoms on live slots (pq/dsp/atoms.h "Tables", pq/dsp/sample_slot.h): sample_pos and sample_read
// against double references (every lane count, Spl combination and block partition: atom_harness.h) — loops forward
// and ping-pong, the loop crossfade, end, fade in / out, normalised units, mip levels, stereo —, the loop seam taps,
// the slot's generations (a swap never changes a sounding note; a reset takes the newest; entries no lane reads are
// retired), sample_slice and sample_scrub.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <thread>
#include <numbers>
#include <random>
#include <vector>

#include "atom_harness.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/sample_slot.h"

using namespace pqtest;
using pq::dsp::SampleData;
using pq::dsp::SampleSlot;

namespace {

using V = std::vector<std::vector<double>>;

AtomSpec spec(Atom a, std::vector<Gen> gens, RefFn ref, Tol tol = {}) {
  AtomSpec s;
  s.atom = a;
  s.gens = std::move(gens);
  s.ref = std::move(ref);
  s.tol = {tol};
  return s;
}

double hermite(double xm1, double x0, double x1, double x2, double t) {
  const double c1 = 0.5 * (x1 - xm1);
  const double c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
  const double c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
  return ((c3 * t + c2) * t + c1) * t + x0;
}

struct Slot {
  pq::EpochReclaimer reclaimer{1};
  SampleSlot slot;
  std::shared_ptr<const SampleData> data;
  explicit Slot(const std::vector<float>& l, const std::vector<float>& r = {}, uint32_t levels = 0,
                double fileRate = 48000.0, std::vector<float> slices = {}, std::vector<float> markers = {}) {
    data = SampleData::build(l, r, levels).value();
    REQUIRE(slot.publish(SampleData::source(data, fileRate, std::move(slices), std::move(markers)), 48000.0, reclaimer).ok());
  }
};

std::vector<float> noise(size_t n, uint32_t seed) {
  std::vector<float> v(n);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> ud(-1, 1);
  for (auto& x : v) x = ud(rng);
  return v;
}

// The documented sample_read for one lane in frame units (pq/dsp/atoms.h), in double: inputs rate, reset, start,
// loopStart, loopEnd, fade, pingpong, end, fadeIn, fadeOut, units → out, playing, right, pos. Positions stay below 2^20
// with 2^-32 steps (exact in double); |rate| < the loop length (a ping-pong reflection then never needs a clamp).
struct ReadRef {
  const std::vector<float>* left;
  const std::vector<float>* right;
  bool cubic;
  double at(const std::vector<float>& d, int64_t i) const {
    return i >= 0 && i < static_cast<int64_t>(d.size()) ? static_cast<double>(d[static_cast<size_t>(i)]) : 0.0;
  }
  void operator()(const V& in, V& out) const {
    const auto len = static_cast<int64_t>(left->size());
    double pos = 0;
    bool active = false, back = false;
    for (size_t t = 0; t < in[0].size(); ++t) {
      const double ls = std::floor(in[3][t]), le = std::floor(in[4][t]);
      const bool loopOn = le > ls;
      const bool pp = loopOn && in[6][t] > 0.5;
      const double endEff = std::min(std::floor(in[7][t]), static_cast<double>(len));
      const double F = loopOn && !pp ? std::min({std::max(std::floor(in[5][t]), 0.0), le - ls, std::max(ls, 0.0)}) : 0.0;
      if (in[1][t] != 0) {
        pos = in[2][t];
        active = std::floor(pos) >= 0 && std::floor(pos) < endEff;
        back = false;
      }
      const int64_t idx = static_cast<int64_t>(std::floor(pos));
      const int64_t ci = std::clamp<int64_t>(idx, 0, len - 1);
      const double fr = pos - std::floor(pos);
      const bool inLoop = loopOn && static_cast<double>(ci) >= ls && static_cast<double>(ci) < le;
      const auto ils = static_cast<int64_t>(ls), ile = static_cast<int64_t>(le);
      auto read = [&](const std::vector<float>& d, int64_t i, bool loopTaps) {
        auto tap = [&](int64_t j) {
          if (loopTaps && inLoop) {
            if (pp) {
              if (j > ile - 1) j = 2 * (ile - 1) - j;
              if (j < ils) j = 2 * ils - j;
            } else {
              while (j >= ile) j -= ile - ils;
              while (j < ils) j += ile - ils;
            }
          }
          return at(d, std::clamp<int64_t>(j, -1, len));
        };
        return cubic ? hermite(tap(i - 1), tap(i), tap(i + 1), tap(i + 2), fr) : tap(i) + fr * (tap(i + 1) - tap(i));
      };
      double yl = read(*left, ci, true), yr = read(*right, ci, true);
      if (F > 0 && inLoop && static_cast<double>(ci) >= le - F) {
        const double u = std::numbers::pi / 2 * ((static_cast<double>(ci) - (le - F)) + fr) / F;
        const int64_t j = std::clamp<int64_t>(idx - (ile - ils), 0, len - 1);
        yl = std::cos(u) * yl + std::sin(u) * read(*left, j, false);
        yr = std::cos(u) * yr + std::sin(u) * read(*right, j, false);
      }
      const double fi = in[8][t], fo = in[9][t];
      if (fi > 0 || fo > 0) {
        const double x = static_cast<double>(idx) + fr;
        const double gi = fi > 0 ? (x - in[2][t]) / fi : 1.0, go = fo > 0 ? (endEff - x) / fo : 1.0;
        const double g = std::clamp(std::min({gi, go, 1.0}), 0.0, 1.0);
        yl *= g;
        yr *= g;
      }
      out[0][t] = active ? yl : 0.0;
      out[1][t] = active ? 1.0 : 0.0;
      out[2][t] = active ? yr : 0.0;
      out[3][t] = static_cast<double>(idx) + fr;
      if (!active) continue;
      double inc = std::floor(in[0][t] * 0x1p32) * 0x1p-32;
      if (back) inc = -inc;
      double next = pos + inc;
      if (loopOn && static_cast<double>(idx) < le && std::floor(next) >= le) {
        if (pp) next = 2 * (le - 1) - next, back = !back;
        else
          while (std::floor(next) >= le) next -= (le - ls);
      } else if (loopOn && static_cast<double>(idx) >= ls && std::floor(next) < ls) {
        if (pp) next = 2 * ls - next, back = !back;
        else
          while (std::floor(next) < ls) next += (le - ls);
      }
      pos = next;
      active = std::floor(pos) >= 0 && std::floor(pos) < endEff;
    }
  }
};

// sample_read's inputs (rate, reset, start, loopStart, loopEnd, fade, pingpong, end, fadeIn, fadeOut, units).
std::vector<Gen> readGens() {
  return {pickFrom({1.0f, 0.5f, -0.75f, 1.25f, 2.0f, 0.3125f, -1.5f}), boolRuns(0.02), uniform(0, 150),
          pickFrom({20.0f, 50.5f}), pickFrom({0.0f, 80.0f, 120.7f}), pickFrom({0.0f}), pickFrom({0.0f}),
          pickFrom({1.0e30f}), pickFrom({0.0f}), pickFrom({0.0f}), pickFrom({0.0f}),
          pickFrom({0.0f}), pickFrom({120.0f})};  // SM.15: tempo 0 (no Re-Pitch), sampleBpm
}

}  // namespace

TEST_CASE("sample atoms: sample_pos against a double reference (the newest entry's left channel)") {
  constexpr int32_t len = 200;
  const std::vector<float> smp = noise(len, 7);
  Slot s(smp, noise(len, 8));
  auto at = [&](int64_t i) { return i >= 0 && i < len ? static_cast<double>(smp[static_cast<size_t>(i)]) : 0.0; };
  for (bool cubic : {false, true}) {
    AtomSpec a = spec(cubic ? Atom::SamplePosCubic : Atom::SamplePosLinear, {uniform(-5, 210)}, [&, cubic](const V& in, V& out) {
      for (size_t t = 0; t < in[0].size(); ++t) {
        const double p = in[0][t];
        if (!(p >= 0 && p < len)) {
          out[0][t] = 0;
          continue;
        }
        const auto i = static_cast<int64_t>(std::floor(p));
        const double fr = p - std::floor(p);
        out[0][t] = cubic ? hermite(at(i - 1), at(i), at(i + 1), at(i + 2), fr) : at(i) + fr * (at(i + 1) - at(i));
      }
    }, Tol{2e-6, 0});
    a.resource = &s.slot;
    checkAtom(a, 60);
  }
}

TEST_CASE("sample atoms: sample_read against a double playhead — loops, ping-pong, the loop fade, end, fades, stereo") {
  constexpr int32_t len = 200;
  const std::vector<float> l = noise(len, 7), r = noise(len, 9);
  Slot mono(l), stereo(l, r);
  for (bool cubic : {false, true}) {
    for (int ch = 0; ch < 2; ++ch) {
      INFO("cubic " << cubic << " stereo " << ch);
      const ReadRef ref{&l, ch == 1 ? &r : &l, cubic};
      AtomSpec a = spec(cubic ? Atom::SampleReadCubic : Atom::SampleReadLinear, readGens(), ref, Tol{2e-6, 0});
      a.tol = {Tol{2e-6, 0}, Tol{}, Tol{2e-6, 0}, Tol{1e-4, 0}};
      a.resource = ch == 1 ? &stereo.slot : &mono.slot;
      a.T = 900;
      checkAtom(a, 70);
      // Audio-rate rate input.
      a.gens[0] = uniform(-2, 2);
      checkAtom(a, 71);
      // Tiny rates of either sign (M1 review M11a), no loop.
      a.gens[0] = pickFrom({-1e-9f, -2.9e-8f, -3e-8f, -1e-6f, -1e-12f, 1e-9f, -0.4999f, -0.5f, -0.0f});
      a.gens[3] = pickFrom({0.0f});
      a.gens[4] = pickFrom({0.0f});
      checkAtom(a, 72);
      // Rates beyond the loop length, both directions (M1 review M11b). Loops of 1 to 5 samples.
      a.gens[0] = pickFrom({2.0f, 3.5f, 7.25f, -5.5f, 64.0f, -64.0f, 1.0f, -1.0f, 1000.75f});
      a.gens[2] = pickFrom({0.0f, 3.0f, 3.5f, 10.0f});
      a.gens[3] = pickFrom({3.0f});
      a.gens[4] = pickFrom({4.0f, 5.0f, 8.0f});
      checkAtom(a, 73);
      // Ping-pong: turns at loopEnd − 1 and loopStart, mirrored taps (rates below the loop length).
      a.gens = readGens();
      a.gens[0] = pickFrom({1.0f, 0.5f, -0.75f, 1.25f, 2.0f, 3.3f, -1.5f});
      a.gens[3] = pickFrom({20.0f, 40.5f});
      a.gens[4] = pickFrom({60.0f, 90.0f});
      a.gens[6] = pickFrom({1.0f});
      checkAtom(a, 74);
      // The loop crossfade (forward loops; fade up to the loop's length and the frames before loopStart).
      a.gens = readGens();
      a.gens[0] = pickFrom({1.0f, 0.5f, 1.25f, 0.3125f});
      a.gens[3] = pickFrom({20.0f, 60.0f});
      a.gens[4] = pickFrom({90.0f, 120.0f});
      a.gens[5] = pickFrom({0.0f, 8.0f, 25.5f, 70.0f});
      a.tol[0] = a.tol[2] = Tol{3e-6, 0};  // the crossfade's fastSin gains
      checkAtom(a, 75);
      // End (one-shot regions) and fades in / out.
      a.gens = readGens();
      a.gens[3] = pickFrom({0.0f});
      a.gens[4] = pickFrom({0.0f});
      a.gens[7] = pickFrom({50.0f, 120.0f, 400.0f});
      a.gens[8] = pickFrom({0.0f, 5.0f, 30.0f});
      a.gens[9] = pickFrom({0.0f, 7.0f});
      checkAtom(a, 76);
    }
  }
}

TEST_CASE("sample atoms: sample_read interpolates across the loop seam from the loop's other end (M1 review L3)") {
  // Sample k = k (a ramp), loop [10, 14), start 12, rate 0.5: frames read 12, 12.5, 13, 13.5, 10, 10.5 …; at 13.5
  // the linear taps are 13 and 10 (the sample after loopEnd − 1 inside the loop is loopStart), not 13 and 14.
  std::vector<float> ramp(32);
  for (size_t k = 0; k < ramp.size(); ++k) ramp[k] = static_cast<float>(k);
  Slot s(ramp);
  for (const bool cubic : {false, true}) {
    for (const uint32_t lanes : {1u, 8u}) {
      INFO("cubic " << cubic << ", lanes " << lanes);
      const Atom atom = cubic ? Atom::SampleReadCubic : Atom::SampleReadLinear;
      constexpr uint32_t T = 8;
      std::vector<float> rate(8, 0.5f), start(8, 12.0f), ls(8, 10.0f), le(8, 14.0f), zero(8, 0.0f), end(8, 1e30f);
      std::vector<float> reset(T * lanes, 0.0f);
      for (uint32_t l = 0; l < lanes; ++l) reset[l] = pq::dsp::boolMask(true);  // reset at frame 0
      std::vector<float> out(T * lanes), playing(T * lanes), right(T * lanes), pos(T * lanes);
      pq::dsp::SampleReadState st{};
      pq::dsp::StateInit init;
      init.lanes = lanes;
      pq::dsp::initState(atom, &st, init);
      const float* ins[] = {rate.data(), reset.data(), start.data(), ls.data(), le.data(), zero.data(),
                            zero.data(), end.data(), zero.data(), zero.data(), zero.data(), zero.data(), zero.data()};
      float* outs[] = {out.data(), playing.data(), right.data(), pos.data()};
      const uint32_t spl = 0x1FFDu;  // everything at param rate but reset (per sample)
      pq::dsp::KernelArgs args{ins, outs, &st, &s.slot, T, 13, 4, spl};
      pq::dsp::findKernel(atom, lanes, spl)(args);
      const auto w = [](int j) { return static_cast<double>(10 + ((j - 10) % 4 + 4) % 4); };
      for (uint32_t t = 0; t < T; ++t) {
        const double p = 12.0 + 0.5 * t >= 14.0 ? 12.0 + 0.5 * t - 4.0 : 12.0 + 0.5 * t;
        const int i = static_cast<int>(std::floor(p));
        const double fr = p - i;
        const double want = cubic ? hermite(w(i - 1), w(i), w(i + 1), w(i + 2), fr) : w(i) + fr * (w(i + 1) - w(i));
        for (uint32_t l = 0; l < lanes; ++l) {
          CHECK(out[t * lanes + l] == doctest::Approx(want).epsilon(1e-6));
          CHECK(right[t * lanes + l] == out[t * lanes + l]);  // mono: the left read, copied
          CHECK(pos[t * lanes + l] == static_cast<float>(p));
        }
      }
      if (!cubic) CHECK(out[3 * lanes] == 11.5f);
    }
  }
}

namespace {

// One sample_read call over `frames` frames for `lanes` lanes, inputs constant over the call (reset per frame).
struct Reader {
  Atom atom = Atom::SampleReadCubic;
  uint32_t lanes = 8;
  pq::dsp::SampleReadState st{};
  std::vector<float> rate = std::vector<float>(8, 1.0f), start = std::vector<float>(8, 0.0f),
                     ls = std::vector<float>(8, 0.0f), le = std::vector<float>(8, 0.0f), fade = std::vector<float>(8, 0.0f),
                     pp = std::vector<float>(8, 0.0f), end = std::vector<float>(8, 1e30f), fi = std::vector<float>(8, 0.0f),
                     fo = std::vector<float>(8, 0.0f), units = std::vector<float>(8, 0.0f),
                     tempo = std::vector<float>(8, 0.0f), sbpm = std::vector<float>(8, 120.0f);
  std::vector<float> reset, out, playing, right, pos;
  explicit Reader(uint32_t l = 8, Atom a = Atom::SampleReadCubic) : atom(a), lanes(l) {
    pq::dsp::StateInit init;
    init.lanes = lanes;
    pq::dsp::initState(atom, &st, init);
  }
  // trig: bit l = lane l resets at the call's first frame.
  void run(const SampleSlot& slot, uint32_t frames, uint32_t trig = 0) {
    reset.assign(static_cast<size_t>(frames) * lanes, 0.0f);
    for (uint32_t l = 0; l < lanes; ++l)
      if (((trig >> l) & 1u) != 0 && frames > 0) reset[l] = pq::dsp::boolMask(true);
    out.assign(reset.size(), 0.0f);
    playing = right = pos = out;
    const float* ins[] = {rate.data(), reset.data(), start.data(), ls.data(), le.data(), fade.data(),
                          pp.data(), end.data(), fi.data(), fo.data(), units.data(), tempo.data(), sbpm.data()};
    float* outs[] = {out.data(), playing.data(), right.data(), pos.data()};
    const uint32_t spl = 0x1FFDu;
    pq::dsp::KernelArgs args{ins, outs, &st, &slot, frames, 13, 4, spl};
    pq::dsp::findKernel(atom, lanes, spl)(args);
  }
  float at(uint32_t t, uint32_t l) const { return out[static_cast<size_t>(t) * lanes + l]; }
};

}  // namespace

TEST_CASE("sample atoms: units 1 — positions in [0, 1] of the lane's sample, fades in seconds of the file") {
  std::vector<float> ramp(1000);
  for (size_t k = 0; k < ramp.size(); ++k) ramp[k] = static_cast<float>(k);
  Slot s(ramp, {}, 0, 1000.0);  // a 1000 Hz file at 48 kHz: rate 1 moves 1000/48000 frames per sample
  Reader r(1, Atom::SampleReadLinear);
  r.units.assign(8, 1.0f);
  r.start.assign(8, 0.25f);
  r.end.assign(8, 0.5f);
  r.rate.assign(8, 48.0f);  // one frame per sample
  r.run(s.slot, 300, 1);
  CHECK(r.at(0, 0) == 250.0f);
  CHECK(r.pos[0] == doctest::Approx(0.25f));
  CHECK(r.at(249, 0) == 499.0f);
  CHECK(!pq::dsp::maskIsTrue(r.playing[250]));  // end = 0.5 · 1000
  // A fade in of 0.01 s = 10 frames of the file.
  Reader f(1, Atom::SampleReadLinear);
  f.units.assign(8, 1.0f);
  f.start.assign(8, 0.25f);
  f.rate.assign(8, 48.0f);
  f.fi.assign(8, 0.01f);
  f.run(s.slot, 20, 1);
  CHECK(f.at(0, 0) == 0.0f);
  CHECK(f.at(5, 0) == doctest::Approx(255.0f * 0.5f));
  CHECK(f.at(12, 0) == 262.0f);
}

// SM.15: Re-Pitch through the markers. tempo > 0 multiplies the rate by the warp law's local speed at the playhead:
// constant per marker segment, changing on the sample the playhead enters the next one; no markers: tempo / sampleBpm.
TEST_CASE("SM.15 sample_read: tempo follows the warp markers (the law's local slope), sampleBpm without them") {
  std::vector<float> ramp(20000);
  for (size_t k = 0; k < ramp.size(); ++k) ramp[k] = static_cast<float>(k);
  // 2880 BPM at 48 kHz is 0.001 beats per sample: a segment of F frames per beat reads F / 1000 frames per sample.
  {
    Slot none(ramp);
    for (const uint32_t lanes : {1u, 8u}) {
      Reader r(lanes, Atom::SampleReadLinear);
      r.tempo.assign(8, 240.0f);
      r.sbpm.assign(8, 120.0f);  // twice the sample's tempo: two frames per sample
      r.run(none.slot, 100, 0xFF);
      for (uint32_t t = 0; t < 100; ++t) REQUIRE(r.at(t, lanes - 1) == static_cast<float>(2 * t));
    }
  }
  // Markers: beat 0 at frame 0, beat 1 at 1000 (1000 frames per beat), beat 2 at 4000 (3000), beat 3 at 4500 (500).
  Slot m(ramp, {}, 0, 48000.0, {}, {0.0f, 0.0f, 1000.0f, 1.0f, 4000.0f, 2.0f, 4500.0f, 3.0f});
  auto expected = [](uint32_t t) -> double {  // the playhead after t samples from frame 0
    double x = 0.0;
    for (uint32_t i = 0; i < t; ++i) x += x < 1000.0 ? 1.0 : x < 4000.0 ? 3.0 : 0.5;
    return x;
  };
  for (const uint32_t lanes : {1u, 8u})
    for (const uint32_t block : {4000u, 64u, 1u, 333u}) {  // the crossings fall inside calls and on their edges
      CAPTURE(lanes);
      CAPTURE(block);
      Reader r(lanes, Atom::SampleReadLinear);
      r.tempo.assign(8, 2880.0f);
      r.sbpm.assign(8, 77.0f);  // (unused with markers)
      r.tempo[3] = 0.0f;        // lane 3: no Re-Pitch (its rate alone)
      r.rate[3] = 0.25f;
      uint32_t t = 0;
      bool ok = true;
      while (t < 4000) {
        const uint32_t n = std::min(block, 4000 - t);
        r.run(m.slot, n, t == 0 ? 0xFFu : 0u);
        for (uint32_t i = 0; i < n && ok; ++i) {
          ok = r.at(i, 0) == static_cast<float>(expected(t + i));
          if (lanes == 8) ok = ok && r.at(i, 7) == static_cast<float>(expected(t + i)) && r.at(i, 3) == 0.25f * static_cast<float>(t + i);
          if (!ok) MESSAGE("first mismatch at sample ", t + i, ": ", r.at(i, 0), " vs ", expected(t + i));
        }
        t += n;
      }
      CHECK(ok);
    }
  // A loop back over a marker (the speed follows the playhead back), and a retrigger into the first segment.
  Reader r(1, Atom::SampleReadLinear);
  r.tempo.assign(8, 2880.0f);
  r.start.assign(8, 990.0f);
  r.ls.assign(8, 990.0f);
  r.le.assign(8, 1030.0f);
  r.run(m.slot, 60, 1);
  // 990 … 999 at 1 frame per sample, 1000, 1003 … 1027 at 3, the wrap (1030 → 990), then 1 again.
  CHECK(r.at(9, 0) == 999.0f);
  CHECK(r.at(10, 0) == 1000.0f);
  CHECK(r.at(11, 0) == 1003.0f);
  CHECK(r.at(19, 0) == 1027.0f);
  CHECK(r.at(20, 0) == 990.0f);
  CHECK(r.at(21, 0) == 991.0f);
  r.run(m.slot, 5, 1);
  CHECK(r.at(1, 0) == 991.0f);
}

TEST_CASE("sample atoms: a read faster than rate 2 reads a decimated copy (the level per call from |rate|)") {
  // A 1 kHz sine at 48 kHz with three mip levels: at rate 4 (level 2, read at rate 1) the output is the sine at 4 kHz,
  // read from the level's samples (the decimator passes 1 kHz unchanged).
  std::vector<float> sine(48000);
  for (size_t k = 0; k < sine.size(); ++k) sine[k] = static_cast<float>(std::sin(2 * std::numbers::pi * 1000.0 * k / 48000.0));
  Slot s(sine, {}, 3);
  const pq::dsp::SampleLevelView& l2 = s.data->level(2);
  for (const uint32_t lanes : {1u, 8u}) {
    Reader r(lanes);
    r.rate.assign(8, 4.0f);
    r.start.assign(8, 1000.0f);
    r.run(s.slot, 256, 0xFF);
    double worst = 0;
    for (uint32_t t = 0; t < 256; ++t) worst = std::max(worst, static_cast<double>(std::fabs(r.at(t, 0) - l2.ch[0][250 + t])));
    CHECK(worst < 1e-6);  // frame 1000 + 4t of the sample is frame 250 + t of level 2
    double sinErr = 0;
    for (uint32_t t = 0; t < 256; ++t)
      sinErr = std::max(sinErr, std::fabs(r.at(t, 0) - std::sin(2 * std::numbers::pi * 1000.0 * (1000.0 + 4.0 * t) / 48000.0)));
    CHECK(sinErr < 1e-3);
  }
}

TEST_CASE("sample atoms: a swap never changes a sounding note; a reset takes the newest; unread entries are retired") {
  std::vector<float> a(4000, 0.5f), b(4000, -0.25f);
  pq::EpochReclaimer reclaimer(1);
  const auto reader = reclaimer.registerReader();
  REQUIRE(reader.has_value());
  SampleSlot slot;
  auto da = SampleData::build(a).value(), db = SampleData::build(b).value();
  REQUIRE(slot.publish(SampleData::source(da, 48000.0), 48000.0, reclaimer).ok());
  Reader r(8);
  r.run(slot, 64, 0x0F);  // lanes 0 … 3 play sample A
  CHECK(r.at(10, 0) == 0.5f);
  CHECK(r.at(10, 4) == 0.0f);  // never triggered: silent
  REQUIRE(slot.publish(SampleData::source(db, 48000.0), 48000.0, reclaimer).ok());
  r.run(slot, 64, 0x30);  // lanes 4, 5 trigger now: sample B; 0 … 3 keep A
  for (uint32_t t = 0; t < 64; ++t) {
    CHECK(r.at(t, 0) == 0.5f);
    CHECK(r.at(t, 3) == 0.5f);
    CHECK(r.at(t, 4) == -0.25f);
  }
  r.run(slot, 64, 0x01);  // lane 0 retriggers: B
  CHECK(r.at(0, 0) == -0.25f);
  CHECK(r.at(0, 1) == 0.5f);
  // A swap inside a call's frames: lane 1 resets at frame 10 of it and switches there (lanes keep their own entry).
  {
    r.reset.assign(64 * 8, 0.0f);
    r.reset[10 * 8 + 1] = pq::dsp::boolMask(true);
    r.out.assign(64 * 8, 0.0f);
    r.playing = r.right = r.pos = r.out;
    const float* ins[] = {r.rate.data(), r.reset.data(), r.start.data(), r.ls.data(), r.le.data(), r.fade.data(),
                          r.pp.data(), r.end.data(), r.fi.data(), r.fo.data(), r.units.data(), r.tempo.data(), r.sbpm.data()};
    float* outs[] = {r.out.data(), r.playing.data(), r.right.data(), r.pos.data()};
    pq::dsp::KernelArgs args{ins, outs, &r.st, &slot, 64, 13, 4, 0x1FFDu};
    pq::dsp::findKernel(r.atom, 8, 0x1FFDu)(args);
    CHECK(r.at(9, 1) == 0.5f);
    CHECK(r.at(10, 1) == -0.25f);
    CHECK(r.at(10, 2) == 0.5f);
  }
  // Entries stay while read (touched) or superseded less than the keep time ago, then go: lanes 2, 3 still read A.
  slot.setKeepSeconds(0.05);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  slot.sweep(reclaimer);
  CHECK(slot.entries() == 2);  // B, A (the initial silence: superseded long ago, never read)
  CHECK(SampleSlot::find(slot.acquire(), 2) != nullptr);
  // Every lane retriggered on B: A is no longer read; past the keep time it is retired (freed after a quiescent state).
  r.run(slot, 64, 0xFF);
  for (uint32_t l = 0; l < 8; ++l) CHECK(r.at(0, l) == -0.25f);
  r.run(slot, 64);
  slot.sweep(reclaimer);  // (the call that retriggered them still read A at its start: seen now)
  CHECK(slot.entries() == 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  slot.sweep(reclaimer);
  CHECK(slot.entries() == 1);
  CHECK(SampleSlot::find(slot.acquire(), 2) == nullptr);
  CHECK(reclaimer.pending() >= 1);
  reclaimer.quiescent(*reader);
  CHECK(reclaimer.collect() >= 1);
  // A lane whose entry is gone (more swaps than kept entries while it played) stops: silent until its next trigger.
  Reader q(1);
  q.run(slot, 8, 1);
  for (uint32_t k = 0; k < pq::dsp::kSampleSlotMaxEntries + 2; ++k)
    REQUIRE(slot.publish(SampleData::source(k % 2 ? da : db, 48000.0), 48000.0, reclaimer).ok());
  q.run(slot, 8);
  CHECK(!pq::dsp::maskIsTrue(q.playing[0]));
  q.run(slot, 8, 1);
  CHECK(pq::dsp::maskIsTrue(q.playing[0]));
  reclaimer.unregisterReader(*reader);
}

TEST_CASE("sample atoms: sample_slice — slice key − 36 of the slice table; none: slice 0 is the whole sample") {
  pq::EpochReclaimer reclaimer(1);
  SampleSlot slot;
  auto d = SampleData::build(std::vector<float>(1000, 0.1f)).value();
  REQUIRE(slot.publish(SampleData::source(d, 48000.0, {600.0f, 100.0f, 300.0f, 2000.0f, -5.0f, 300.0f}), 48000.0, reclaimer).ok());
  for (const uint32_t lanes : {1u, 8u}) {
    for (const bool units : {false, true}) {
      std::vector<float> key(8);
      const float keys[8] = {36, 37, 38, 39, 35, NAN, 36.4f, 60};
      for (uint32_t l = 0; l < 8; ++l) key[l] = keys[l];
      std::vector<float> u(8, units ? 1.0f : 0.0f), s(4 * lanes), e(4 * lanes);
      const float* ins[] = {key.data(), u.data()};
      float* outs[] = {s.data(), e.data()};
      pq::dsp::KernelArgs args{ins, outs, nullptr, &slot, 4, 2, 2, 0b11};
      pq::dsp::findKernel(Atom::SampleSlice, lanes, 0b11)(args);
      const float k = units ? 1.0f / 1000.0f : 1.0f;
      // The table sorted, outside [0, length) dropped, duplicates merged: 100, 300, 600.
      const float wantS[8] = {100, 300, 600, 0, 0, 0, 100, 0}, wantE[8] = {300, 600, 1000, 0, 0, 0, 300, 0};
      for (uint32_t l = 0; l < lanes; ++l) {
        CHECK(s[3 * lanes + l] == doctest::Approx(wantS[l] * k));
        CHECK(e[3 * lanes + l] == doctest::Approx(wantE[l] * k));
      }
    }
  }
  REQUIRE(slot.publish(SampleData::source(d, 48000.0), 48000.0, reclaimer).ok());
  std::vector<float> key = {36, 37}, u = {0, 0}, s(1), e(1);
  const float* ins[] = {key.data(), u.data()};
  float* outs[] = {s.data(), e.data()};
  pq::dsp::KernelArgs args{ins, outs, nullptr, &slot, 1, 2, 2, 0b11};
  pq::dsp::findKernel(Atom::SampleSlice, 1, 0b11)(args);
  CHECK(s[0] == 0.0f);
  CHECK(e[0] == 1000.0f);
}

TEST_CASE("sample atoms: sample_scrub reads both channels at a position 0 … 1 (0 outside)") {
  std::vector<float> l(100), r(100);
  for (size_t k = 0; k < l.size(); ++k) l[k] = static_cast<float>(k), r[k] = -static_cast<float>(k);
  Slot s(l, r);
  std::vector<float> pos = {0.0f, 0.5f, 0.255f, 1.5f, -0.1f, NAN, 0.99f, 0.1f};
  for (const uint32_t lanes : {1u, 8u}) {
    std::vector<float> outL(lanes), outR(lanes);
    pq::dsp::SampleScrubState st{};
    const float* ins[] = {pos.data()};
    float* outs[] = {outL.data(), outR.data()};
    pq::dsp::KernelArgs args{ins, outs, &st, &s.slot, 1, 1, 2, 0b1};
    pq::dsp::findKernel(Atom::SampleScrub, lanes, 0b1)(args);
    const float want[8] = {0, 50, 25.5f, 0, 0, 0, 99, 10};
    for (uint32_t k = 0; k < lanes; ++k) {
      CHECK(outL[k] == doctest::Approx(want[k]).epsilon(1e-5));
      CHECK(outR[k] == doctest::Approx(-want[k]).epsilon(1e-5));
    }
  }
}

TEST_CASE("sample slot: publish refuses more than 10 minutes; a silent slot never plays") {
  pq::EpochReclaimer reclaimer(1);
  SampleSlot slot;
  Reader r(8);
  r.run(slot, 16, 0xFF);
  for (const float x : r.playing) CHECK(!pq::dsp::maskIsTrue(x));
  auto d = SampleData::build(std::vector<float>(1000, 0.1f)).value();
  pq::dsp::SampleSource src = SampleData::source(d, 1.0);  // 1000 frames at 1 Hz: 16 minutes
  CHECK(!slot.publish(src, 48000.0, reclaimer).ok());
  CHECK(slot.generation() == 1);
}
