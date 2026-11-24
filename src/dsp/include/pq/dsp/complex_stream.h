#pragma once

// The Complex / Complex Pro stream (SM.16; docs/plans/SM.md "Rework 2026-11-23"): the one drive of a phase-vocoder
// stretcher (dsp::StretchPool, pq/dsp/stretch_pool.h) for the clip player (pq/engine/clip_player.h, T6.5) and the
// Sampler's Complex voices (pq/dsp/sampler_stretch.h, SM.5).
//
// ---- The model ----------------------------------------------------------------------------------------------------------
// A stretcher is fed ONE continuous stream of source frames and produces vocoder frames on the host's vocoder clock y
// (the host reads its output at y(t): the clip (t − tA)·r through the sinc resampler, the Sampler Φ(t) through the
// cubic; the stretcher itself runs at transpose 1, so the pitch is the read rate exactly).
//   stream frame k   = the UNFOLDED source frame u0 + dir·k (dir −1: a reversed clip), folded into the file through
//                      the loop (WarpLaw::foldFrame: an unfolded frame past the loop's end plays inside the loop);
//                      zeros below zeroBelow (before the host's start), from zeroAbove on (after its end, no loop)
//                      and outside the file. [0, head) are in.
//   vocoder frames   [vLo, vHi) are held in the stretcher's output buffer (kComplexFrames per channel), computed in
//                      chunks of kComplexChunk on a grid fixed at the priming (ys), so what a chunk holds does not
//                      depend on the block size.
//   content          the vocoder frame y holds the source at the host's law U(y) (HOST::unfolded: the unfolded frame
//                      the host's time law puts where its read reaches y). A chunk [vHi, vHi + C) takes the input up
//                      to round(U(vHi + C + output latency)) + the input latency: recomputed from the law every
//                      chunk, so the content never drifts.
//   priming          reset, the formant params, a seek with the history before the first frame (its rate: the law's
//                      slope there), then chunks from ys = an output latency + a block + the host's read reach
//                      before its first read (complexPrimeStart). How fast the chunks may come (the host's schedule:
//                      the clip's pre-roll / cue in song time, the voice's per-sample budget) is the host's.
//   lead             complexLead: each voice keeps a golden-ratio fraction of the analysis interval computed ahead of
//                      its reads, so voices started together do their spectral work in different blocks (M6 M10).
//
// ---- The host (a template parameter; nothing virtual) ---------------------------------------------------------------
//   double unfolded(double y)         U(y), see above
//   const WarpLaw& law()              the loop (whole frames)
//   int64_t dir()                     ±1
//   int64_t startU() / endU()         the unfolded frames the content starts / ends at (endU INT64_MAX: none)
//   int64_t frames()                  the file's length
//   uint32_t channels()               input channels to fill (1 or 2)
//   void read(a, b, forward, dst)     file frames [a, b) ⊂ [0, frames) into dst[c][0 … b − a), in stream order
//                                     (forward false: dst[c][j] = frame b − 1 − j); frames it cannot deliver: zeros
// RT-safe: everything noexcept, no allocation; ComplexStream is POD (a clip voice's member, a Sampler lane's words).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "pq/dsp/fixed.h"
#include "pq/dsp/stretch_pool.h"
#include "pq/dsp/warp.h"

namespace pq::dsp {

// ---- The constants (one set) ------------------------------------------------------------------------------------------
inline constexpr uint32_t kComplexChunk = 128;       // vocoder frames per process() call
inline constexpr uint32_t kComplexFrames = 8192;     // vocoder output kept per stretcher and channel
inline constexpr uint32_t kComplexInFrames = 32768;  // the input gather per channel at least (beyond: read as zeros)
inline constexpr uint32_t kComplexPrimeRate = 4;     // vocoder frames per output sample while priming, × max(1, r)

// The input gather a host allocates per channel: kComplexInFrames, and at least the seek's history. RT-safe.
inline uint32_t complexInFrames(const StretchPool::Latency& lat) noexcept {
  return std::max<uint32_t>(kComplexInFrames, static_cast<uint32_t>(lat.seek) + kComplexChunk);
}
// Vocoder frames a voice may prime per output sample at pitch rate r (not slower below 1: M6 L14). RT-safe.
inline double complexPrimeRate(double r) noexcept { return static_cast<double>(kComplexPrimeRate) * std::max(1.0, r); }
// The lead of the voice with `key` (the clip's seed, the Sampler's stretcher index): a golden-ratio fraction of the
// analysis interval (consecutive keys land far apart). The vocoder's frames do not depend on when they are computed,
// so it moves only the blocks the work lands in. RT-safe.
inline int64_t complexLead(uint32_t key, const StretchPool::Latency& lat) noexcept {
  const double g = static_cast<double>(key) * 0.6180339887498949;
  return std::clamp<int64_t>(static_cast<int64_t>((g - std::floor(g)) * static_cast<double>(lat.interval)), 0,
                             std::max(lat.interval - 1, 0));
}
// Where a priming starts for a first read at vocoder frame y0 through a reader that reaches `reach` frames back: an
// output latency and a whole block before it (every synthesis frame the first read overlaps is computed), on the
// chunk grid. RT-safe.
inline int64_t complexPrimeStart(int64_t y0, int64_t reach, const StretchPool::Latency& lat) noexcept {
  const auto C = static_cast<int64_t>(kComplexChunk);
  return floorDiv(y0 - lat.output - lat.block - reach, C) * C;
}
// The formant rule (Complex Pro): the vocoder moves the formants back by pitchRatio^(−formants %) (the host's
// resampling moves them by pitchRatio), its envelope analysis at 1 / order cycles per sample. Complex, or Formants 0:
// no formant processing. Not for per-sample use (std::pow): hosts call it when the ratio or the settings change.
inline StretchParams complexFormants(AudioWarp warp, double pitchRatio, double formants, double formantEnvelope) noexcept {
  StretchParams p;
  if (warp != AudioWarp::ComplexPro) return p;
  const double keep = std::clamp(formants, 0.0, 100.0) / 100.0;
  if (keep <= 0.0) return p;
  p.formant = std::pow(pitchRatio, -keep);
  p.formantBase = 1.0 / std::clamp(formantEnvelope, 8.0, 256.0);
  return p;
}

// The host's input gather: one buffer per channel of `cap` frames (ch[1] unused by a mono source).
struct ComplexIn {
  float* ch[2] = {nullptr, nullptr};
  uint32_t cap = 0;
};

struct ComplexStream;
// Tests: called (when set, on the calling thread) after every chunk with the stream, the stream frames [k0, k1) it
// consumed and the gathered ones (in.ch[c][0 … k1 − k0 − lead), `lead` leading frames skipped as zeros).
struct ComplexProbe {
  void (*fn)(void* user, const ComplexStream& s, int64_t k0, int64_t k1, int64_t lead, const ComplexIn& in, uint32_t channels) = nullptr;
  void* user = nullptr;
};
inline thread_local const ComplexProbe* tComplexProbe = nullptr;

struct ComplexStream {
  static constexpr int64_t kUnprimed = INT64_MIN;
  int64_t vLo = 0, vHi = kUnprimed;  // vocoder frames held: [vLo, vHi) (vHi kUnprimed: not primed)
  int64_t ys = 0;                    // the first vocoder frame (the chunk grid's base)
  int64_t head = 0;                  // stream frames fed
  int64_t u0 = 0;                    // unfolded source frame of stream frame 0
  int64_t zeroBelow = 0;             // stream frames below: zeros
  int64_t zeroAbove = INT64_MAX;     // stream frames from here on: zeros

  bool primed() const noexcept { return vHi != kUnprimed; }

  // Where a stream primed at ys starts: u0 and the zero bounds (no stretcher touched: the clip player also asks this
  // for a voice's stream claim before it has one).
  template <class H>
  void place(const H& h, int64_t ys0, const StretchPool::Latency& lat) noexcept {  // RT-safe
    ys = ys0;
    u0 = std::llround(h.unfolded(static_cast<double>(ys + lat.output)));
    zeroBelow = h.dir() * (h.startU() - u0);
    zeroAbove = h.endU() == INT64_MAX ? INT64_MAX : h.dir() * (h.endU() - u0);
  }

  // The stream frames [k0, k1) into `in` (frame k0 at index 0).
  template <class H>
  void gather(H& h, const ComplexIn& in, int64_t k0, int64_t k1) const noexcept {  // RT-safe
    const uint32_t ch = h.channels();
    const WarpLaw& law = h.law();
    const bool loops = law.loopsFrames();
    const int64_t span = law.loopEndF - law.loopStartF;
    const int64_t dir = h.dir(), len = h.frames();
    auto zero = [&](int64_t at, int64_t n) noexcept {
      for (uint32_t c = 0; c < ch; ++c) std::memset(in.ch[c] + at, 0, sizeof(float) * static_cast<size_t>(n));
    };
    for (int64_t k = k0; k < k1;) {
      const int64_t at = k - k0;
      if (k < zeroBelow || k >= zeroAbove) {
        const int64_t n = k < zeroBelow ? std::min(k1, zeroBelow) - k : k1 - k;
        zero(at, n);
        k += n;
        continue;
      }
      int64_t run = std::min(k1, zeroAbove) - k;  // stream frames until the file position jumps (a loop wrap)
      const int64_t u = u0 + dir * k;
      const int64_t src = law.foldFrame(u);
      if (loops) {
        if (dir > 0) run = std::min(run, u < law.loopEndF ? law.loopEndF - u : span - (src - law.loopStartF));
        else if (u >= law.loopEndF) run = std::min(run, src - law.loopStartF + 1);
      }
      const int64_t a = dir > 0 ? src : src - run + 1, b = a + run;  // the file frames [a, b) of this run
      const int64_t fa = std::clamp<int64_t>(a, 0, len), fb = std::clamp<int64_t>(b, 0, len);
      if (fa >= fb) {
        zero(at, run);
      } else {
        // (stream order: forward, file frame i lands at at + (i − a); reverse, at at + (b − 1 − i))
        const int64_t before = dir > 0 ? fa - a : b - fb, after = run - before - (fb - fa);
        if (before > 0) zero(at, before);
        if (after > 0) zero(at + before + (fb - fa), after);
        float* dst[2] = {in.ch[0] + at + before, ch > 1 ? in.ch[1] + at + before : nullptr};
        h.read(fa, fb, dir > 0, dst);
      }
      k += run;
    }
  }

  // Primes stretcher s for a first read near ys (complexPrimeStart): reset (generator `seed`), the formant params,
  // the seek. No vocoder work.
  template <class H>
  void prime(H& h, StretchPool& P, uint32_t s, const ComplexIn& in, int64_t ys0, uint32_t seed, const StretchParams& fp) noexcept {  // RT-safe
    const StretchPool::Latency& lat = P.latency();
    const auto C = static_cast<double>(kComplexChunk);
    place(h, ys0, lat);
    P.reset(s, seed);
    P.setParams(s, fp);
    gather(h, in, lat.input - lat.seek, lat.input);
    const auto y0 = static_cast<double>(ys + lat.output);
    const double rate = std::abs(h.unfolded(y0 + C) - h.unfolded(y0)) / C;
    const float* ip[2] = {in.ch[0], h.channels() > 1 ? in.ch[1] : in.ch[0]};
    P.seek(s, ip, lat.seek, rate);  // a mono stretcher reads ip[0] only
    head = lat.input;
    vLo = vHi = ys;
  }

  // The vocoder frames [vHi, vHi + kComplexChunk) (room for them: ensure()).
  template <class H>
  void chunk(H& h, StretchPool& P, uint32_t s, const ComplexIn& in) noexcept {  // RT-safe
    const StretchPool::Latency& lat = P.latency();
    const auto C = static_cast<int64_t>(kComplexChunk);
    const int64_t target = h.dir() * (std::llround(h.unfolded(static_cast<double>(vHi + C + lat.output))) - u0) + lat.input;
    const int64_t inN = std::clamp<int64_t>(target - head, 0, int64_t{1} << 24);
    const int64_t lead = std::max<int64_t>(0, inN - static_cast<int64_t>(in.cap));  // a run too fast for the gather
    gather(h, in, head + lead, head + inN);
    const float* ip[2] = {in.ch[0], h.channels() > 1 ? in.ch[1] : in.ch[0]};
    float* b0 = P.output(s) + (vHi - vLo);
    float* out[2] = {b0, b0 + kComplexFrames};
    P.process(s, ip, static_cast<int>(inN), static_cast<int>(lead), out, static_cast<int>(C));
    if (tComplexProbe != nullptr && tComplexProbe->fn != nullptr)
      tComplexProbe->fn(tComplexProbe->user, *this, head, head + inN, lead, in, h.channels());
    head += inN;
    vHi += C;
  }

  // Makes room for one more chunk: drops the frames below keepFrom (what the host's reads no longer reach). A read
  // wider than the buffer (hosts cut their runs to fit) keeps nothing.
  void makeRoom(StretchPool& P, uint32_t s, int64_t keepFrom) noexcept {  // RT-safe
    const auto C = static_cast<int64_t>(kComplexChunk), V = static_cast<int64_t>(kComplexFrames);
    if (vHi + C - vLo <= V) return;
    const int64_t from = std::clamp(keepFrom, vLo, vHi);
    const auto keep = static_cast<size_t>(vHi - from);
    float* b0 = P.output(s);
    std::memmove(b0, b0 + (from - vLo), sizeof(float) * keep);
    if (P.channels(s) == 2) std::memmove(b0 + V, b0 + V + (from - vLo), sizeof(float) * keep);
    vLo = from;
    if (vHi + C - vLo > V) vLo = vHi;
  }

  // Chunks until [.., last) is held, at most maxChunks of them; returns how many it computed.
  template <class H>
  uint32_t ensure(H& h, StretchPool& P, uint32_t s, const ComplexIn& in, int64_t keepFrom, int64_t last,
                  uint32_t maxChunks = 0xFFFFFFFFu) noexcept {  // RT-safe
    uint32_t n = 0;
    while (vHi < last && n < maxChunks) {
      makeRoom(P, s, keepFrom);
      chunk(h, P, s, in);
      ++n;
    }
    return n;
  }
};
static_assert(std::is_trivially_copyable_v<ComplexStream>);

}  // namespace pq::dsp
