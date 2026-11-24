// sampler_warp (SM.4; semantics in pq/dsp/atoms.h "Tables", the voice in pq/dsp/warp_voice.h): Beats / Tones /
// Texture per lane over a live SampleSlot; SM.5: Complex / Complex Pro (pq/dsp/sampler_stretch.h) with the slot's
// stretchers.

#include <cstddef>
#include <cstring>

#include "kernels_common.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/sample_slot.h"
#include "pq/dsp/sampler_stretch.h"
#include "pq/dsp/warp_voice.h"

namespace pq::dsp::detail {
namespace {

using Entry = SampleSlot::Entry;

// A lane's words: the WarpVoice, the entry's generation, the ComplexVoice (SM.5).
constexpr uint32_t kVoiceWords = (static_cast<uint32_t>(sizeof(WarpVoice)) + 3u) / 4u;
constexpr uint32_t kComplexWords = (static_cast<uint32_t>(sizeof(ComplexVoice)) + 3u) / 4u;
constexpr uint32_t kLaneWords = kVoiceWords + 1 + kComplexWords;
static_assert(kLaneWords <= kSamplerWarpWords, "WarpVoice + ComplexVoice outgrew SamplerWarpState");
// SM.14: the words a free lane's stop writes (WarpVoice::stop, ComplexVoice idle: the next trigger primes afresh).
constexpr uint32_t kActiveWord = static_cast<uint32_t>(offsetof(WarpVoice, active)) / 4u;
constexpr uint32_t kPhaseWord = kVoiceWords + 1 + static_cast<uint32_t>(offsetof(ComplexVoice, phase)) / 4u;
static_assert(offsetof(WarpVoice, active) % 4 == 0 && offsetof(ComplexVoice, phase) % 4 == 0);

void loadLane(const SamplerWarpState& st, uint32_t l, WarpVoice& v, uint32_t& gen, ComplexVoice& cv) noexcept {  // RT-safe
  uint32_t buf[kLaneWords];
  for (uint32_t w = 0; w < kLaneWords; ++w) buf[w] = st.w[w][l];
  std::memcpy(&v, buf, sizeof(WarpVoice));
  gen = buf[kVoiceWords];
  std::memcpy(&cv, buf + kVoiceWords + 1, sizeof(ComplexVoice));
}

void storeLane(SamplerWarpState& st, uint32_t l, const WarpVoice& v, uint32_t gen, const ComplexVoice& cv) noexcept {  // RT-safe
  uint32_t buf[kLaneWords] = {};
  std::memcpy(buf, &v, sizeof(WarpVoice));
  buf[kVoiceWords] = gen;
  std::memcpy(buf + kVoiceWords + 1, &cv, sizeof(ComplexVoice));
  for (uint32_t w = 0; w < kLaneWords; ++w) st.w[w][l] = buf[w];
}

// The per-call scratch (warp_voice.h WarpScratch), in the state after the lanes' words (atom_state.h).
constexpr uint32_t kGainF = kWarpVoiceChunk + 16, kCorrAF = kWarpMaxAlignLen + 64,
                   kCorrBF = kWarpMaxAlignLen + 2 * kWarpMaxAlignLags + 64, kGatherF = 2 * kWarpGatherFrames;
static_assert(kGainF + kCorrAF + kCorrBF + kGatherF == kSamplerWarpScratchFloats);
static_assert(kGainF % 8 == 0 && kCorrAF % 8 == 0 && kCorrBF % 8 == 0);  // each buffer 32-byte aligned

constexpr uint32_t kSeg = 256;  // frames per lane segment (the lane's contiguous in / out)

struct SamplerWarpK {
  static constexpr uint32_t kSpec = 2;  // rate, reset
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    constexpr uint32_t W = S::kW;
    auto& st = *static_cast<SamplerWarpState*>(a.state);
    const auto& slot = *static_cast<const SampleSlot*>(a.resource);
    const Entry* newest = slot.acquire();
    SamplerStretch* bank = slot.stretch();  // SM.5: the instance's stretchers (null: none; Complex plays Tones)
    WarpScratch ws;
    ws.gain = st.scratch;
    ws.corrA = ws.gain + kGainF;
    ws.corrB = ws.corrA + kCorrAF;
    ws.gather = ws.corrB + kCorrBF;
    alignas(32) float rate[kSeg], l[kSeg], r[kSeg], pos[kSeg], play[kSeg];
    const uint32_t frames = a.frames;
    auto resetAt = [&](uint32_t i, uint32_t lane) noexcept {
      return maskIsTrue(splBit(SM, 1) ? a.in[1][lane] : a.in[1][i * W + lane]);
    };
    // The first frame in [from, limit) with a high reset (limit: none). A param-rate reset is one value: no scan.
    auto nextReset = [&](uint32_t from, uint32_t limit, uint32_t lane) noexcept {
      if constexpr (splBit(SM, 1)) {
        return resetAt(0, lane) ? from : limit;
      } else {
        while (from < limit && !resetAt(from, lane)) ++from;
        return from;
      }
    };
    const uint32_t freeLanes = W == 8 ? ~a.laneMask & 0xFFu : 0u;  // SM.14 (kernel.h "Occupied lanes")
    for (uint32_t lane = 0; lane < W; ++lane) {
      // SM.14: a free lane (its voice is over) stops: no render, no vocoder, until the note-on that occupies it.
      // Its stretcher claim stays (a moved copy of the voice may be the one that holds it; the lane's next trigger
      // frees what it held).
      if (((freeLanes >> lane) & 1u) != 0) {
        st.w[kActiveWord][lane] = 0;
        st.w[kPhaseWord][lane] = ComplexVoice::kWait;
        for (uint32_t i = 0; i < frames; ++i)
          a.out[0][i * W + lane] = a.out[1][i * W + lane] = a.out[2][i * W + lane] = a.out[3][i * W + lane] = 0.0f;
        continue;
      }
      WarpVoice v;
      uint32_t gen = 0;
      ComplexVoice cv;
      loadLane(st, lane, v, gen, cv);
      const uint32_t global = st.group * 8u + lane;
      const Entry* e = newest;
      if (v.active != 0 && gen != static_cast<uint32_t>(newest->generation)) {
        e = SampleSlot::find(newest, gen);
        if (e == nullptr) {  // its entry was retired: silent until its next trigger
          v.stop();
          e = newest;
        } else {
          e->touched.store(1, std::memory_order_relaxed);  // still read: not retired yet
        }
      }
      WarpSource src = warpSourceOf(*e);
      // The param-rate inputs (positions in [0, 1] of the lane's sample).
      WarpVoiceRun run;
      WarpRunLaw law;
      bool complex = false;
      auto frameOf = [&](uint32_t k) noexcept {
        const double x = a.in[k][lane];
        return std::isfinite(x) ? x * static_cast<double>(src.length()) : 0.0;
      };
      auto setRun = [&] {
        run.start = frameOf(2);
        run.end = frameOf(3);
        run.loopStart = frameOf(4);
        run.loopEnd = frameOf(5);
        run.bpm = std::isfinite(a.in[6][lane]) ? a.in[6][lane] : 120.0;
        run.sampleBpm = std::isfinite(a.in[7][lane]) ? a.in[7][lane] : 120.0;
        // The mode is the AudioWarp's value (warp.h; the stretch modes only: anything else plays Tones). SM.5: Complex /
        // Complex Pro: a Tones voice (the onset, the fallback) under the vocoder.
        run.mode = audioWarpOf(a.in[8][lane]);
        if (!warpStretches(run.mode)) run.mode = AudioWarp::Tones;
        complex = warpComplex(run.mode);
        run.set.formants = std::isfinite(a.in[14][lane]) ? a.in[14][lane] : 100.0f;
        run.set.formantEnvelope = std::isfinite(a.in[15][lane]) ? a.in[15][lane] : 128.0f;
        run.set.grainSeconds = std::isfinite(a.in[9][lane]) ? a.in[9][lane] : 0.03;
        run.set.flux = std::isfinite(a.in[10][lane]) ? a.in[10][lane] : 0.0f;
        run.set.sliceLoop = warpSliceLoopOf(a.in[11][lane]);
        run.set.envelope = std::isfinite(a.in[12][lane]) ? std::clamp(a.in[12][lane], 0.0f, 100.0f) : 100.0f;
        const double sr = a.in[13][lane];
        run.sampleRate = std::isfinite(sr) ? std::clamp(sr, 1000.0, 768000.0) : 48000.0;
        law = warpRunLaw(src, run);  // once per run record (never per render)
      };
      setRun();
      uint32_t f = 0;
      while (f < frames) {
        // A high reset restarts the voice at that sample on the newest entry.
        uint32_t f1 = f;
        if (resetAt(f, lane)) {
          e = newest;
          gen = static_cast<uint32_t>(newest->generation);
          src = warpSourceOf(*e);
          setRun();
          v.trigger(src, run, law);
          if (complex) cv.trigger(bank, global);
          else cv.phase = ComplexVoice::kWait;
          f1 = f + 1;  // a held reset restarts every sample
        } else {
          if (v.active == 0) {  // idle: skip to the next reset
            f1 = nextReset(f, frames, lane);
            for (uint32_t i = f; i < f1; ++i) {
              a.out[0][i * W + lane] = 0.0f;
              a.out[1][i * W + lane] = 0.0f;
              a.out[2][i * W + lane] = 0.0f;
              a.out[3][i * W + lane] = 0.0f;
            }
            f = f1;
            continue;
          }
          f1 = nextReset(f, std::min(frames, f + kSeg), lane);
        }
        const uint32_t n = f1 - f;
        if constexpr (splBit(SM, 0)) std::fill_n(rate, n, a.in[0][lane]);  // (param rate: one value)
        else
          for (uint32_t i = 0; i < n; ++i) rate[i] = a.in[0][(f + i) * W + lane];
        if (complex) cv.render(bank, global, v, src, run, law, rate, n, l, r, pos, play, ws);
        else v.render(src, run, law, rate, n, l, r, pos, play, ws);
        for (uint32_t i = 0; i < n; ++i) {
          const uint32_t o = (f + i) * W + lane;
          a.out[0][o] = l[i];
          a.out[1][o] = boolMask(play[i] != 0.0f);
          a.out[2][o] = r[i];
          a.out[3][o] = pos[i];
        }
        f = f1;
      }
      storeLane(st, lane, v, gen, cv);
    }
  }
};

}  // namespace

KernelFn findSamplerWarpKernel(uint32_t lanes, uint32_t m) noexcept { return pick<SamplerWarpK>(lanes, m); }

}  // namespace pq::dsp::detail
