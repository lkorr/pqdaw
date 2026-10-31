// T1.x-silence (SPEC §7.7): every atom's silence declaration (dsp::silenceInfo / silenceTail) is true of its kernel.
// The scheduler's sleep plan skips whole regions on these declarations, so a wrong one is wrong audio after a wake:
//   Any / All / Clip / MulAdd   the kernel's outputs are ±0 when the named inputs are ±0 (every lane count, every
//                               Blk/Spl combination, random finite coefficients);
//   Memory                      after silenceTail() frames of silent input, from any history, the outputs are ±0
//                               and the history it keeps is too (the split read halves: from the write's history);
//   Smoother                    with silent signal inputs the output is ±0 whatever the residual; a settled state
//                               (residual exactly 0) stays bit for bit unchanged while target and time do.
#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atoms.h"

using pq::dsp::Atom;
using pq::dsp::SilenceRule;

namespace {

bool silent(float v) noexcept { return (std::bit_cast<uint32_t>(v) & 0x7FFFFFFFu) == 0; }

struct Kernel {
  Atom atom;
  uint32_t lanes;
  uint32_t spl;    // Spl mask
  uint32_t numIn, numOut;
};

// A coefficient for input i (not named by the rule): finite, non-zero, of either sign (div's denominator too).
float coefficient(Atom atom, uint32_t i, const pq::dsp::KernelInfo& info, std::mt19937& rng) {
  std::uniform_real_distribution<float> mag(0.1f, 2.0f);
  std::bernoulli_distribution coin(0.5);
  if ((info.boolIn >> i) & 1u) return pq::dsp::boolMask(coin(rng));
  switch (atom) {
    case Atom::Clip: return i == 1 ? -mag(rng) : mag(rng);  // lo ≤ 0 ≤ hi
    case Atom::Mux:
    case Atom::Demux: return static_cast<float>(std::uniform_int_distribution<int>(0, 3)(rng));  // the index
    case Atom::DelayLinear:
    case Atom::DelayCubic:
    case Atom::DelayAllpass:
    case Atom::DelayReadLinear:
    case Atom::DelayReadCubic:
    case Atom::DelayReadAllpass: return std::uniform_real_distribution<float>(1.0f, 40.0f)(rng);  // delay time
    case Atom::Gain:
    case Atom::GainAdd:
      if (i == (atom == Atom::Gain ? 2u : 3u)) return std::uniform_real_distribution<float>(0.0f, 200.0f)(rng);  // time
      if (i == (atom == Atom::Gain ? 3u : 4u)) return coin(rng) ? 1.0f : 0.0f;  // start (T4.x-fix-dsp)
      break;
    default: break;
  }
  return coin(rng) ? mag(rng) : -mag(rng);
}

float silentValue(std::mt19937& rng) { return std::bernoulli_distribution(0.5)(rng) ? -0.0f : 0.0f; }

struct Run {
  std::vector<std::vector<float>> in, out;
  std::vector<const float*> inPtr;
  std::vector<float*> outPtr;
};

// Runs `frames` frames with input i's value from gen(i, f, lane).
template <class Gen>
void runKernel(const Kernel& k, void* state, uint32_t frames, Run& r, Gen&& gen) {
  const pq::dsp::KernelFn fn = pq::dsp::findKernel(k.atom, k.lanes, k.spl);
  REQUIRE(fn != nullptr);
  r.in.assign(k.numIn, {});
  r.out.assign(k.numOut, std::vector<float>(static_cast<size_t>(frames) * k.lanes + 8));
  r.inPtr.clear();
  r.outPtr.clear();
  for (uint32_t i = 0; i < k.numIn; ++i) {
    const bool spl = (k.spl >> i) & 1u;
    const uint32_t n = spl ? 1u : frames;
    std::vector<float>& v = r.in[i];
    v.assign(static_cast<size_t>(spl ? 8u : n * k.lanes) + 8, 0.0f);
    for (uint32_t f = 0; f < n; ++f)
      for (uint32_t l = 0; l < (spl ? 8u : k.lanes); ++l) v[f * (spl ? 8u : k.lanes) + l] = gen(i, f, l);
    r.inPtr.push_back(v.data());
  }
  for (auto& o : r.out) r.outPtr.push_back(o.data());
  pq::dsp::KernelArgs args{r.inPtr.data(), r.outPtr.data(), state, nullptr, frames, static_cast<uint16_t>(k.numIn),
                           static_cast<uint16_t>(k.numOut), k.spl};
  fn(args);
}

// The input counts a kernel is called with (mux: index + 3 inputs; demux: 3 outputs).
void arity(Atom a, const pq::dsp::KernelInfo& info, uint32_t& numIn, uint32_t& numOut) {
  numIn = a == Atom::Mux ? 4u : info.numIn;
  numOut = a == Atom::Demux ? 3u : info.numOut;
}

// Every Spl mask the kernel table has for this atom (variadic mux: all Blk and all Spl data inputs).
std::vector<uint32_t> splMasks(Atom a, const pq::dsp::KernelInfo& info, uint32_t numIn) {
  std::vector<uint32_t> masks;
  if (info.variadicIn) {
    masks = {0u, (1u << numIn) - 1u, 1u};
    return masks;
  }
  const uint32_t spec = info.splSpecMask;
  for (uint32_t m = 0; m < (1u << numIn); ++m) {
    if ((m & ~spec) != 0) continue;
    masks.push_back(m | info.splRequired);
  }
  (void)a;
  return masks;
}

// The inputs that must be silent for one check (Any: just one of them).
std::vector<uint32_t> silentSets(const pq::dsp::SilenceInfo& s, uint32_t numIn) {
  const uint32_t named = s.inputs & ((numIn >= 32 ? 0u : (1u << numIn)) - 1u);
  switch (s.rule) {
    case SilenceRule::Any: {
      std::vector<uint32_t> sets;
      for (uint32_t i = 0; i < numIn; ++i)
        if ((named >> i) & 1u) sets.push_back(1u << i);
      return sets;
    }
    case SilenceRule::MulAdd: return {0b101u, 0b110u};
    default: return {named};
  }
}

}  // namespace

TEST_CASE("dsp silence: every declaration holds for its kernel (T1.x-silence)") {
  uint32_t declared = 0;
  std::mt19937 rng(20261025);
  for (uint32_t a = 0; a < pq::dsp::kAtomCount; ++a) {
    const auto atom = static_cast<Atom>(a);
    const pq::dsp::KernelInfo& info = pq::dsp::kernelInfo(atom);
    const pq::dsp::SilenceInfo si = pq::dsp::silenceInfo(atom);
    const std::string name = info.name;
    INFO(name);
    if (si.rule == SilenceRule::None) continue;
    ++declared;
    CHECK(info.resource == pq::dsp::Resource::None);  // no declaration may depend on a table
    uint32_t numIn = 0, numOut = 0;
    arity(atom, info, numIn, numOut);
    pq::dsp::StateInit init;
    init.maxDelayFrames = 48;
    for (uint32_t lanes : {1u, 8u}) {
      init.lanes = lanes;
      const size_t bytes = pq::dsp::stateBytes(atom, init);
      pq::AlignedBuffer<uint32_t> state(bytes / 4 + 8);
      for (uint32_t spl : splMasks(atom, info, numIn)) {
        const Kernel k{atom, lanes, spl, numIn, numOut};
        if (pq::dsp::findKernel(atom, lanes, spl) == nullptr) continue;
        INFO("lanes " << lanes << ", spl " << spl);
        for (int trial = 0; trial < 6; ++trial) {
          Run r;
          if (si.rule == SilenceRule::Memory) {
            // Any history first (random inputs), then silence: the outputs from the tail on are ±0.
            pq::dsp::initState(atom, state.data(), init);
            const bool readHalf = si.inputs == 0;  // split read: drive the history through the fused delay / z1
            const Atom writer = !readHalf ? atom
                                : atom == Atom::Z1Read ? Atom::Z1
                                : atom == Atom::DelayReadLinear ? Atom::DelayLinear
                                : atom == Atom::DelayReadCubic ? Atom::DelayCubic
                                                               : Atom::DelayAllpass;
            const pq::dsp::KernelInfo& winfo = pq::dsp::kernelInfo(writer);
            const Kernel w{writer, lanes, winfo.splRequired, winfo.numIn, winfo.numOut};
            std::uniform_real_distribution<float> u(-1.0f, 1.0f);
            runKernel(w, state.data(), 37, r, [&](uint32_t i, uint32_t, uint32_t) {
              return i == 0 ? ((winfo.boolIn & 1u) ? pq::dsp::boolMask(u(rng) > 0.0f) : u(rng)) : coefficient(writer, i, winfo, rng);
            });
            const uint32_t tail = pq::dsp::silenceTail(atom, init);
            CHECK(tail >= 1);
            runKernel(w, state.data(), tail, r, [&](uint32_t i, uint32_t, uint32_t) {
              return i == 0 ? silentValue(rng) : coefficient(writer, i, winfo, rng);
            });
            // The allpass interpolator's own one-sample recursion (DelayState::apY1, |coefficient| ≤ 1/3) decays
            // rather than ends: the sleep plan checks and flushes it like a z1 (SleepStateKind::Delay).
            if (atom == Atom::DelayAllpass || atom == Atom::DelayReadAllpass) {
              auto* ds = reinterpret_cast<pq::dsp::DelayState*>(state.data());
              for (uint32_t l = 0; l < lanes; ++l) CHECK(std::fabs(ds->apY1[l]) < 1.0f);
              std::memset(ds->apY1, 0, sizeof ds->apY1);
            }
            // From here every output is ±0 (the kernel under test, on more silence).
            // (A split read runs in slices no longer than its delay: one frame here.)
            const uint32_t frames = readHalf ? 1u : 29u;
            runKernel(k, state.data(), frames, r, [&](uint32_t i, uint32_t, uint32_t) {
              return ((si.inputs >> i) & 1u) ? silentValue(rng) : coefficient(atom, i, info, rng);
            });
            for (uint32_t o = 0; o < numOut; ++o)
              for (uint32_t x = 0; x < frames * lanes; ++x) {
                INFO("output " << o << ", element " << x << " = " << r.out[o][x]);
                REQUIRE(silent(r.out[o][x]));
              }
            continue;
          }
          if (si.rule == SilenceRule::Smoother) {
            // Random residual: the output is still ±0 for a silent signal.
            pq::dsp::initState(atom, state.data(), init);
            // Inputs: …, g, time, start (T4.x-fix-dsp).
            const uint32_t gi = numIn - 3, ti = numIn - 2, si0 = numIn - 1;
            const float g0 = coefficient(atom, gi, info, rng), t0 = coefficient(atom, ti, info, rng);
            const float s0 = coefficient(atom, si0, info, rng);
            auto fixedIn = [&](uint32_t i, float other) { return i == gi ? g0 : i == ti ? t0 : i == si0 ? s0 : other; };
            runKernel(k, state.data(), 16, r, [&](uint32_t i, uint32_t, uint32_t) {
              return fixedIn(i, ((si.inputs >> i) & 1u) ? silentValue(rng) : 0.5f);
            });
            for (uint32_t x = 0; x < 16 * lanes; ++x) REQUIRE(silent(r.out[0][x]));
            // Settle (a fixed target and time), then: unchanged state bit for bit, ±0 out.
            for (int b = 0; b < 400; ++b)
              runKernel(k, state.data(), 64, r, [&](uint32_t i, uint32_t, uint32_t) { return fixedIn(i, silentValue(rng)); });
            const auto* gs = reinterpret_cast<const pq::dsp::GainState*>(state.data());
            for (uint32_t l = 0; l < lanes; ++l) REQUIRE(silent(gs->d[l]));
            std::vector<uint32_t> before(state.data(), state.data() + bytes / 4);
            runKernel(k, state.data(), 33, r, [&](uint32_t i, uint32_t, uint32_t) { return fixedIn(i, silentValue(rng)); });
            for (uint32_t x = 0; x < 33 * lanes; ++x) REQUIRE(silent(r.out[0][x]));
            CHECK(std::memcmp(before.data(), state.data(), bytes) == 0);
            continue;
          }
          // Stateless rules: one call per silent set.
          for (uint32_t set : silentSets(si, numIn)) {
            const uint32_t frames = 19;
            runKernel(k, state.data(), frames, r, [&](uint32_t i, uint32_t, uint32_t) {
              return ((set >> i) & 1u) ? silentValue(rng) : coefficient(atom, i, info, rng);
            });
            for (uint32_t o = 0; o < numOut; ++o)
              for (uint32_t x = 0; x < frames * lanes; ++x) {
                INFO("silent inputs " << set << ", output " << o << ", element " << x << " = " << r.out[o][x]);
                REQUIRE(silent(r.out[o][x]));
              }
          }
        }
      }
    }
  }
  CHECK(declared >= 45);
}

TEST_CASE("dsp silence: tails (T1.x-silence)") {
  pq::dsp::StateInit init;
  init.maxDelayFrames = 4800;
  CHECK(pq::dsp::silenceTail(Atom::Z1, init) == 1);
  CHECK(pq::dsp::silenceTail(Atom::Rise, init) == 1);
  CHECK(pq::dsp::silenceTail(Atom::DelayCubic, init) == 4803);
  CHECK(pq::dsp::silenceTail(Atom::DelayWrite, init) == 4803);
  CHECK(pq::dsp::silenceTail(Atom::Add, init) == 0);
  CHECK(pq::dsp::silenceTail(Atom::Gain, init) == 0);
  // Generators and held values make no promise.
  for (Atom a : {Atom::Phasor, Atom::Noise, Atom::SampleHold, Atom::Latch, Atom::Exp, Atom::DbToGain, Atom::Ge,
                 Atom::Not, Atom::Counter, Atom::Shaper, Atom::Wavetable})
    CHECK(pq::dsp::silenceInfo(a).rule == SilenceRule::None);
}
