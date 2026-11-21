// sampler_warp (SM.4; semantics in pq/dsp/atoms.h "Tables", the voice in pq/dsp/warp_voice.h): Beats / Tones /
// Texture per lane over a live SampleSlot.

#include <cstring>

#include "kernels_common.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/sample_slot.h"
#include "pq/dsp/warp_voice.h"

namespace pq::dsp::detail {
namespace {

using Entry = SampleSlot::Entry;

constexpr uint32_t kVoiceWords = (static_cast<uint32_t>(sizeof(WarpVoice)) + 3u) / 4u;
static_assert(kVoiceWords + 1 <= kSamplerWarpWords, "WarpVoice outgrew SamplerWarpState");

void loadLane(const SamplerWarpState& st, uint32_t l, WarpVoice& v, uint32_t& gen) noexcept {  // RT-safe
  uint32_t buf[kVoiceWords];
  for (uint32_t w = 0; w < kVoiceWords; ++w) buf[w] = st.w[w][l];
  std::memcpy(&v, buf, sizeof(WarpVoice));
  gen = st.w[kVoiceWords][l];
}

void storeLane(SamplerWarpState& st, uint32_t l, const WarpVoice& v, uint32_t gen) noexcept {  // RT-safe
  uint32_t buf[kVoiceWords] = {};
  std::memcpy(buf, &v, sizeof(WarpVoice));
  for (uint32_t w = 0; w < kVoiceWords; ++w) st.w[w][l] = buf[w];
  st.w[kVoiceWords][l] = gen;
}

WarpSource sourceOf(const Entry& e) noexcept {  // RT-safe
  WarpSource s;
  s.level = e.level;
  s.levels = e.levels;
  s.channels = e.channels;
  s.fileRate = e.fileRate;
  s.rateScale = e.rateScale;
  s.slices = e.slices;
  s.sliceCount = e.sliceCount;
  s.markers = e.markers;
  s.markerCount = e.markerCount;
  return s;
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
    WarpScratch ws;
    ws.gain = st.scratch;
    ws.corrA = ws.gain + kGainF;
    ws.corrB = ws.corrA + kCorrAF;
    ws.gather = ws.corrB + kCorrBF;
    alignas(32) float rate[kSeg], l[kSeg], r[kSeg], pos[kSeg], play[kSeg];
    const uint32_t frames = a.frames;
    auto rateAt = [&](uint32_t i, uint32_t lane) noexcept {
      return splBit(SM, 0) ? a.in[0][lane] : a.in[0][i * W + lane];
    };
    auto resetAt = [&](uint32_t i, uint32_t lane) noexcept {
      return maskIsTrue(splBit(SM, 1) ? a.in[1][lane] : a.in[1][i * W + lane]);
    };
    for (uint32_t lane = 0; lane < W; ++lane) {
      WarpVoice v;
      uint32_t gen = 0;
      loadLane(st, lane, v, gen);
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
      WarpSource src = sourceOf(*e);
      // The param-rate inputs (positions in [0, 1] of the lane's sample).
      WarpVoiceRun run;
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
        const float m = a.in[8][lane];
        run.mode = m >= 1.5f ? WarpVoiceMode::Texture : m >= 0.5f ? WarpVoiceMode::Tones : WarpVoiceMode::Beats;
        run.grainSeconds = std::isfinite(a.in[9][lane]) ? a.in[9][lane] : 0.03;
        run.flux = std::isfinite(a.in[10][lane]) ? a.in[10][lane] : 0.0;
        const float sl = a.in[11][lane];
        run.sliceLoop = sl >= 1.5f ? WarpSliceLoop::BackForward : sl >= 0.5f ? WarpSliceLoop::Forward : WarpSliceLoop::Off;
        run.envelope = std::isfinite(a.in[12][lane]) ? std::clamp(a.in[12][lane], 0.0f, 100.0f) : 100.0f;
        const double sr = a.in[13][lane];
        run.sampleRate = std::isfinite(sr) ? std::clamp(sr, 1000.0, 768000.0) : 48000.0;
      };
      setRun();
      uint32_t f = 0;
      while (f < frames) {
        // A high reset restarts the voice at that sample on the newest entry.
        uint32_t f1 = f;
        if (resetAt(f, lane)) {
          e = newest;
          gen = static_cast<uint32_t>(newest->generation);
          src = sourceOf(*e);
          setRun();
          v.trigger(src, run);
          f1 = f + 1;  // a held reset restarts every sample
        } else {
          if (v.active == 0) {  // idle: skip to the next reset
            while (f1 < frames && !resetAt(f1, lane)) ++f1;
            for (uint32_t i = f; i < f1; ++i) {
              a.out[0][i * W + lane] = 0.0f;
              a.out[1][i * W + lane] = 0.0f;
              a.out[2][i * W + lane] = 0.0f;
              a.out[3][i * W + lane] = 0.0f;
            }
            f = f1;
            continue;
          }
          while (f1 < frames && f1 - f < kSeg && !resetAt(f1, lane)) ++f1;
        }
        const uint32_t n = f1 - f;
        for (uint32_t i = 0; i < n; ++i) rate[i] = rateAt(f + i, lane);
        v.render(src, run, rate, n, l, r, pos, play, ws);
        for (uint32_t i = 0; i < n; ++i) {
          const uint32_t o = (f + i) * W + lane;
          a.out[0][o] = l[i];
          a.out[1][o] = boolMask(play[i] != 0.0f);
          a.out[2][o] = r[i];
          a.out[3][o] = pos[i];
        }
        f = f1;
      }
      storeLane(st, lane, v, gen);
    }
  }
};

}  // namespace

KernelFn findSamplerWarpKernel(uint32_t lanes, uint32_t m) noexcept { return pick<SamplerWarpK>(lanes, m); }

}  // namespace pq::dsp::detail
