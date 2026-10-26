// M1 review L7: the per-lane / shared word map of every stateful atom's state (dsp::laneStateInfo), which the
// VoiceEngine uses to reset a lane at note-on and to move a voice between groups. Every stateful atom must declare
// one, and each PerLane declaration must be true: running the 8-lane kernel on lane-permuted inputs from a
// lane-permuted state gives the lane-permuted state in the declared per-lane bytes and the same shared bytes.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atoms.h"

using pq::dsp::Atom;
using pq::dsp::LaneState;

namespace {

constexpr uint32_t kLanes = 8;
constexpr uint32_t kFrames = 24;
constexpr uint32_t perm(uint32_t l) noexcept { return (3 * l + 1) % kLanes; }  // a bijection of 0..7

// Per-lane words [0, perLaneWords) of `s` moved lane l → perm(l) (word w belongs to lane w mod 8).
std::vector<uint32_t> permuteLanes(const std::vector<uint32_t>& s, uint32_t perLaneWords) {
  std::vector<uint32_t> p = s;
  for (uint32_t w = 0; w < perLaneWords; ++w) p[w - w % kLanes + perm(w % kLanes)] = s[w];
  return p;
}

struct Inputs {
  std::vector<std::vector<float>> a, b;  // b = a with lanes permuted
};

Inputs makeInputs(const pq::dsp::KernelInfo& info, std::mt19937& rng) {
  std::uniform_real_distribution<float> u(-2.0f, 2.0f);
  std::bernoulli_distribution coin(0.3);
  Inputs in;
  for (uint32_t i = 0; i < info.numIn; ++i) {
    const bool spl = (info.splRequired >> i) & 1u;
    const bool isBool = (info.boolIn >> i) & 1u;
    const uint32_t frames = spl ? 1 : kFrames;  // a Spl pattern: one value per lane
    std::vector<float> a(static_cast<size_t>(frames) * kLanes), b(a.size());
    for (uint32_t f = 0; f < frames; ++f)
      for (uint32_t l = 0; l < kLanes; ++l) {
        const float v = isBool ? pq::dsp::boolMask(coin(rng)) : u(rng);
        a[f * kLanes + l] = v;
        b[f * kLanes + perm(l)] = v;
      }
    in.a.push_back(std::move(a));
    in.b.push_back(std::move(b));
  }
  return in;
}

void run(Atom atom, const pq::dsp::KernelInfo& info, std::vector<uint32_t>& state,
         const std::vector<std::vector<float>>& in) {
  pq::AlignedBuffer<uint32_t> s(state.size());
  std::memcpy(s.data(), state.data(), state.size() * 4);
  std::vector<const float*> ins;
  for (const auto& v : in) ins.push_back(v.data());
  std::vector<std::vector<float>> outs(info.numOut, std::vector<float>(kFrames * kLanes));
  std::vector<float*> outPtrs;
  for (auto& o : outs) outPtrs.push_back(o.data());
  pq::dsp::KernelArgs args{ins.data(), outPtrs.data(), s.data(), nullptr, kFrames, static_cast<uint16_t>(ins.size()),
                           static_cast<uint16_t>(outPtrs.size()), info.splRequired};
  const pq::dsp::KernelFn fn = pq::dsp::findKernel(atom, kLanes, info.splRequired);
  REQUIRE(fn != nullptr);
  fn(args);
  std::memcpy(state.data(), s.data(), state.size() * 4);
}

}  // namespace

TEST_CASE("dsp lane state: every stateful atom declares which state bytes are per voice lane (M1 review L7)") {
  uint32_t stateful = 0, checked = 0;
  for (uint32_t k = 0; k < pq::dsp::kAtomCount; ++k) {
    const auto atom = static_cast<Atom>(k);
    const pq::dsp::KernelInfo& info = pq::dsp::kernelInfo(atom);
    const pq::dsp::LaneStateInfo lane = pq::dsp::laneStateInfo(atom);
    const std::string name = info.name;
    INFO(name);
    if (info.stateSize == 0) {
      CHECK(lane.kind == LaneState::None);
      continue;
    }
    ++stateful;
    REQUIRE_MESSAGE(lane.kind != LaneState::None, "a stateful atom without a lane layout: add it to dsp::laneStateInfo");
    if (lane.kind == LaneState::DelayLine) {
      CHECK(info.stateSize == sizeof(pq::dsp::DelayState));  // the VoiceEngine's delay handling knows this struct
      continue;
    }
    CHECK(lane.perLaneBytes % (4 * kLanes) == 0);  // whole [8] arrays
    CHECK(lane.perLaneBytes <= info.stateSize);
    if (info.resource != pq::dsp::Resource::None) continue;  // needs a table; covered by the declaration checks only
    // The kernel itself says which words are per lane: permute the lanes of the inputs and of the state.
    pq::dsp::StateInit init;
    init.lanes = kLanes;
    init.seed = 1234;
    std::vector<uint32_t> s1(info.stateSize / 4);
    {
      pq::AlignedBuffer<uint32_t> tmp(s1.size());
      pq::dsp::initState(atom, tmp.data(), init);
      std::memcpy(s1.data(), tmp.data(), s1.size() * 4);
    }
    std::mt19937 rng(77 + k);
    const uint32_t perLaneWords = lane.perLaneBytes / 4;
    for (int block = 0; block < 4; ++block) {
      const Inputs in = makeInputs(info, rng);
      std::vector<uint32_t> s2 = permuteLanes(s1, perLaneWords);
      run(atom, info, s1, in.a);
      run(atom, info, s2, in.b);
      const std::vector<uint32_t> want = permuteLanes(s1, perLaneWords);
      for (size_t w = 0; w < s1.size(); ++w) {
        const std::string where = std::string(w < perLaneWords ? "per lane" : "shared");
        INFO("block " << block << ", word " << w << " (" << where << ")");
        REQUIRE(s2[w] == want[w]);
      }
    }
    ++checked;
  }
  CHECK(stateful >= 25);
  CHECK(checked >= 19);  // all but the table readers (wavetable, sample_read) and the delay lines
}
