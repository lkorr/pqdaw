// SM.14: KernelArgs::laneMask (pq/dsp/kernel.h "Occupied lanes") on the stateful per-lane kernels, called as the
// backends call them (8 lanes, one group): a lane whose bit is clear (its voice is over) stops.
//   - work done stops: sampler_warp (Tones loop) renders no frame of a freed lane (WarpVoice::t frozen), Complex runs
//     no vocoder chunk for it (SamplerStretch::chunks = those of the held lane alone), granular spawns no grain
//     (GranVoice::count frozen, every slot free); sample_read's playhead stops
//   - the occupied lane renders bit for bit what it renders with every lane occupied
//   - a freed lane retriggered plays bit for bit what a fresh state's lane plays from the same call on
//   - RtScope: 10 000 calls of triggers and changing masks allocate nothing
// The engine side (the mask delivered per group on both backends, a VoiceEngine's freed lanes): tests/meta
// test_sampler.cpp "SM.14".
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/granular.h"
#include "pq/dsp/sample_slot.h"
#include "pq/dsp/sampler_stretch.h"
#include "pq/dsp/warp_voice.h"

using namespace pq;
using namespace pq::dsp;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 64;

struct Src {
  EpochReclaimer reclaimer{1};
  SampleSlot slot;
  Src() {
    std::vector<float> l(48000), r(48000);
    for (size_t i = 0; i < l.size(); ++i) {
      const double x = static_cast<double>(i) / kSr;
      l[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * x) + 0.1 * std::sin(0.0007 * i));
      r[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 330.0 * x + 1.0));
    }
    auto data = SampleData::build(l, r, 3).value();
    REQUIRE(slot.publish(SampleData::source(data, 44100.0), kSr, reclaimer).ok());
  }
};

enum class K { Tones, Complex, Granular, Read };

Atom atomOf(K k) {
  switch (k) {
    case K::Granular: return Atom::Granular;
    case K::Read: return Atom::SampleReadCubic;
    default: return Atom::SamplerWarp;
  }
}

// One 8-lane group of the kernel: inputs rate / hz (param rate, per lane), reset (per sample), the rest param rate.
struct Group {
  K k;
  Atom atom;
  uint32_t nIn;
  std::vector<AlignedBuffer<float>> in, out;
  AlignedBuffer<float> state;
  KernelFn fn = nullptr;
  const SampleSlot* slot;
  Group(K kind, const SampleSlot& s) : k(kind), atom(atomOf(kind)), slot(&s) {
    nIn = kernelInfo(atom).numIn;
    for (uint32_t i = 0; i < nIn; ++i) in.emplace_back(static_cast<size_t>(kBlock) * 8);
    for (uint32_t o = 0; o < 4; ++o) out.emplace_back(static_cast<size_t>(kBlock) * 8);
    std::vector<float> v(nIn, 0.0f);
    if (atom == Atom::SamplerWarp) {
      const auto mode = static_cast<float>(k == K::Complex ? AudioWarp::Complex : AudioWarp::Tones);  // (warp.h)
      const float vals[] = {1.0f, 0.0f, 0.05f, 1.0f, 0.3f, 0.5f, 120.0f, 120.0f, mode, 0.04f, 0.0f, 1.0f, 100.0f,
                            static_cast<float>(kSr), 100.0f, 128.0f};
      v.assign(vals, vals + 16);
    } else if (atom == Atom::Granular) {
      const float vals[] = {392.0f, 0.0f, 261.63f, 1.0f, 0.05f, 0.95f, 0.2f, 1.0f, 0.04f, 120.0f, 0.01f, 1.0f, 0.5f,
                            0.0f, 0.0f, 1.0f, 16.0f, 0.0f, static_cast<float>(kSr)};
      v.assign(vals, vals + 19);
    } else {  // sample_read: rate, reset, start, loopStart, loopEnd, fade, pingpong, end, fadeIn, fadeOut, units,
              // tempo (0: no Re-Pitch), sampleBpm
      const float vals[] = {1.0f, 0.0f, 0.05f, 0.3f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 120.0f};
      v.assign(vals, vals + 13);
    }
    for (uint32_t i = 0; i < nIn; ++i)
      for (uint32_t f = 0; f < kBlock * 8; ++f) in[i][f] = i == 1 ? 0.0f : v[i];
    for (uint32_t l = 0; l < 8; ++l) in[0][l] = v[0] * (1.0f + 0.02f * static_cast<float>(l));  // the lanes differ
    fn = findKernel(atom, 8, kernelInfo(atom).splRequired | 1u);
    REQUIRE(fn != nullptr);
    state = AlignedBuffer<float>(stateBytes(atom, init()) / sizeof(float));
    initState(atom, state.data(), init());
  }
  static StateInit init() {
    StateInit i;
    i.lanes = 8;
    i.voiceGroup = 0;
    i.seed = 7;
    return i;
  }
  // One call of `frames` (≤ kBlock), a reset at its frame 0 on `resetLanes`, the occupied lanes `mask`.
  void call(uint32_t frames, uint32_t resetLanes, uint32_t mask) {
    for (uint32_t l = 0; l < 8; ++l) in[1][l] = boolMask(((resetLanes >> l) & 1u) != 0);
    const float* ip[32];
    for (uint32_t i = 0; i < nIn; ++i) ip[i] = in[i].data();
    float* op[4] = {out[0].data(), out[1].data(), out[2].data(), out[3].data()};
    KernelArgs a{ip, op, state.data(), slot, frames, static_cast<uint16_t>(nIn), 4, kernelInfo(atom).splRequired | 1u};
    a.laneMask = mask;
    fn(a);
    for (uint32_t l = 0; l < 8; ++l) in[1][l] = 0.0f;
  }
  float lane(uint32_t o, uint32_t f, uint32_t l) const { return out[o][f * 8 + l]; }
  // Work counters of lane l.
  int64_t warpT(uint32_t l) const {
    const auto& st = *reinterpret_cast<const SamplerWarpState*>(state.data());
    uint32_t buf[(sizeof(WarpVoice) + 3) / 4];
    for (uint32_t w = 0; w < std::size(buf); ++w) buf[w] = st.w[w][l];
    WarpVoice v;
    std::memcpy(&v, buf, sizeof v);
    return v.active != 0 ? v.t : -1;
  }
  uint32_t grains(uint32_t l, uint32_t& live) const {
    const auto& st = *reinterpret_cast<const GranularState*>(state.data());
    uint32_t buf[(sizeof(GranVoice) + 3) / 4];
    for (uint32_t w = 0; w < std::size(buf); ++w) buf[w] = st.v[w][l];
    GranVoice v;
    std::memcpy(&v, buf, sizeof v);
    live = 0;
    for (uint32_t q = 0; q < 32; ++q) live += st.len[q][l] > 0 && st.age[q][l] < st.len[q][l] ? 1u : 0u;
    return v.count;
  }
  bool readActive(uint32_t l) const {
    return maskIsTrue(reinterpret_cast<const SampleReadState*>(state.data())->active[l]);
  }
};

// The scenario: every lane triggered at call 0, all occupied for 40 calls, then lanes 1 … 7 freed (lane 0 held) for
// 40 calls, then lane 3 retriggered (occupied again) for 40 calls. `fresh`: lane 0 triggered at call 0, lane 3 at the
// same retrigger call, both always occupied (the reference for both).
struct Trace {
  std::vector<float> lane0, lane3;  // out 0 per frame, from call 0 / from the retrigger
  std::vector<int64_t> work;        // per call: the freed lanes' work counter sum (lanes 1 … 7 but 3)
  std::vector<uint64_t> chunks;     // per call: the bank's chunks (Complex)
};

Trace scenario(K k, Src& src, bool fresh) {
  const bool complex = k == K::Complex;
  if (complex) src.slot.setStretch(SamplerStretch::build(8, 2, kSr), src.reclaimer);
  Group g(k, src.slot);
  Trace t;
  auto work = [&] {
    int64_t s = 0;
    for (const uint32_t l : {1u, 2u, 4u, 5u, 6u, 7u}) {
      if (k == K::Granular) {
        uint32_t live = 0;
        s += g.grains(l, live);
        s += 1000000 * live;
      } else if (k == K::Read) {
        s += g.readActive(l) ? 1 : 0;
      } else {
        s += g.warpT(l);
      }
    }
    return s;
  };
  auto record = [&](uint32_t frames, bool after) {
    for (uint32_t f = 0; f < frames; ++f) {
      t.lane0.push_back(g.lane(0, f, 0));
      if (after) t.lane3.push_back(g.lane(0, f, 3));
    }
    t.work.push_back(work());
    t.chunks.push_back(complex ? src.slot.stretchView()->chunks : 0);
  };
  g.call(1, fresh ? 0x01u : 0xFFu, fresh ? 0x01u : 0xFFu);
  record(1, false);
  for (uint32_t c = 0; c < 80; ++c) {
    const uint32_t mask = fresh || c >= 40 ? 0x01u : 0xFFu;
    g.call(kBlock, 0, mask);
    record(kBlock, false);
  }
  g.call(1, 0x08u, 0x09u);
  record(1, true);
  for (uint32_t c = 0; c < 40; ++c) {
    g.call(kBlock, 0, 0x09u);
    record(kBlock, true);
  }
  if (complex) src.slot.setStretch(nullptr, src.reclaimer);
  return t;
}

}  // namespace

TEST_CASE("SM.14 lane mask: freed lanes stop (no frames, chunks or grains), the held lane is unchanged, a retrigger is fresh") {
  DenormalGuard dg;
  Src src;
  for (const K k : {K::Tones, K::Complex, K::Granular, K::Read}) {
    CAPTURE(static_cast<int>(k));
    const Trace a = scenario(k, src, false), f = scenario(k, src, true);
    // The work counters of the freed lanes: moving while they are occupied, frozen from the call they are freed.
    if (k == K::Read) CHECK(a.work[40] == 6);  // looping
    else CHECK(a.work[40] != a.work[1]);
    for (size_t c = 41; c <= 80; ++c) CHECK(a.work[c] == (k == K::Read ? 0 : a.work[41]));
    if (k == K::Tones || k == K::Complex) CHECK(a.work[41] == -6);  // every one stopped (WarpVoice::active 0)
    if (k == K::Granular) CHECK(a.work[41] < 1000000);              // no live grain left
    if (k == K::Complex) {
      // The bank's chunks once lanes 1 … 7 are freed: exactly those of lane 0 alone (then with lane 3 again).
      CHECK(a.chunks[40] > 4 * f.chunks[40]);
      for (size_t c = 41; c < a.chunks.size(); ++c) CHECK(a.chunks[c] - a.chunks[41] == f.chunks[c] - f.chunks[41]);
    }
    // Lane 0 (always occupied) renders what it renders beside fresh lanes; the retriggered lane 3 is a fresh voice.
    CHECK(a.lane0 == f.lane0);
    CHECK(a.lane3 == f.lane3);
    double peak = 0.0;
    for (const float x : a.lane3) peak = std::max(peak, static_cast<double>(std::abs(x)));
    CHECK(peak > 0.05);
  }
}

TEST_CASE("SM.14 lane mask: 10 000 calls of triggers and changing masks allocate nothing (RtScope)") {
  REQUIRE(alloc_trap::installed());
  DenormalGuard dg;
  Src src;
  for (const K k : {K::Tones, K::Complex, K::Granular, K::Read}) {
    CAPTURE(static_cast<int>(k));
    if (k == K::Complex) src.slot.setStretch(SamplerStretch::build(8, 2, kSr), src.reclaimer);
    Group g(k, src.slot);
    uint64_t violations = 0;
    uint32_t x = 12345;
    for (uint32_t c = 0; c < 10000; ++c) {
      x = x * 1664525u + 1013904223u;
      const uint32_t mask = (x >> 8) & 0xFFu, reset = c % 7 == 0 ? (x >> 16) & mask : 0u;
      violations += alloc_trap::countRtViolations([&] { g.call(1 + (x >> 24) % kBlock, reset, mask); });
    }
    CHECK(violations == 0);
    if (k == K::Complex) src.slot.setStretch(nullptr, src.reclaimer);
  }
}
