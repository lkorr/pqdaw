// wavetable_pos and voice_tap (TMM.2): the frame-table read of a live table slot with its swap crossfade, and the
// generic voice-tap sink. Semantics: pq/dsp/atoms.h; slots: pq/dsp/frame_table_slot.h; taps: pq/core/voice_tap.h.
//
// CPU: the steady path is one acquire load (the slot entry), a 64-byte state copy, the TMM.1 reader, and one relaxed
// load (the tap's reader count). The crossfade is its own loop over at most 64 frames per swap, so the steady loop
// carries no fade branch or second read. Tap writes cost frames / stride sample copies, only while a face watches.

#include <immintrin.h>

#include <cmath>
#include <cstddef>
#include <cstring>

#include "kernels_common.h"
#include "pq/core/voice_tap.h"
#include "pq/dsp/frame_table.h"
#include "pq/dsp/frame_table_slot.h"

namespace pq::dsp::detail {
namespace {

constexpr uint32_t kFade = kFrameTableFadeFrames;
constexpr uint32_t kChunk = 64;  // frames per expansion of a Spl phase
constexpr float kInvFade = 1.0f / static_cast<float>(kFade);

static_assert(sizeof(FrameReadState) == offsetof(WavetablePosState, generation));

template <uint32_t L, bool kPosSpl>
inline void readSpan(const FrameTableView& t, FrameReadState& st, const float* phase, const float* pos, uint32_t n,
                     float* out) noexcept {  // RT-safe
  if constexpr (L == 1) readFrameTableL1(t, st, phase, pos, !kPosSpl, n, out);
  else readFrameTableL8(t, st, phase, pos, !kPosSpl, n, out);
}

// n frames from the entry's table, the first ones crossfaded from entry->prev while the state's fade runs.
template <uint32_t L, bool kPosSpl>
void readFading(const FrameTableSlot::Entry& e, WavetablePosState& s, FrameReadState& rs, const float* phase,
                const float* pos, uint32_t n, float* out) noexcept {  // RT-safe
  uint32_t f0 = 0;
  if (s.fadeLeft > 0) {
    const uint32_t m = s.fadeLeft < n ? s.fadeLeft : n;
    alignas(32) float old[kFade * L];
    FrameReadState ro = rs;  // the reader state depends only on the phases: both reads see the same history
    readSpan<L, kPosSpl>(e.prev->view, ro, phase, pos, m, old);
    readSpan<L, kPosSpl>(e.view, rs, phase, pos, m, out);
    const uint32_t k0 = kFade - s.fadeLeft;
    for (uint32_t f = 0; f < m; ++f) {
      const uint32_t k = k0 + f + 1;
      if (k >= kFade) continue;  // g = 1: the new table exactly
      const float g = static_cast<float>(k) * kInvFade;
      float* o = out + static_cast<size_t>(f) * L;
      const float* q = old + static_cast<size_t>(f) * L;
      if constexpr (L == 8) {
        const __m256 a = _mm256_load_ps(q);
        _mm256_storeu_ps(o, _mm256_fmadd_ps(_mm256_set1_ps(g), _mm256_sub_ps(_mm256_loadu_ps(o), a), a));
      } else {
        o[0] = std::fma(g, o[0] - q[0], q[0]);
      }
    }
    s.fadeLeft -= m;
    f0 = m;
  }
  if (f0 < n)
    readSpan<L, kPosSpl>(e.view, rs, phase + static_cast<size_t>(f0) * L, kPosSpl ? pos : pos + static_cast<size_t>(f0) * L,
                         n - f0, out + static_cast<size_t>(f0) * L);
}

template <uint32_t L, bool kPhaseSpl, bool kPosSpl>
void wavetablePosK(const KernelArgs& a) noexcept {  // RT-safe
  if (a.frames == 0) return;
  auto& s = *static_cast<WavetablePosState*>(a.state);
  const auto& slot = *static_cast<const FrameTableSlot*>(a.resource);
  const FrameTableSlot::Entry& e = *slot.acquire();
  if (e.generation != s.generation) {
    s.fadeLeft = s.generation != 0 && e.prev != nullptr ? kFade : 0u;
    s.generation = e.generation;
  }
  FrameReadState rs;
  std::memcpy(&rs, &s, sizeof(rs));
  const float* phase = a.in[0];
  const float* pos = a.in[1];
  float* out = a.out[0];
  if constexpr (kPhaseSpl) {
    // A constant phase pattern: expanded chunk by chunk (the readers take a phase per sample).
    alignas(32) float buf[kChunk * L];
    for (uint32_t f = 0; f < kChunk; ++f)
      for (uint32_t l = 0; l < L; ++l) buf[f * L + l] = phase[l];
    for (uint32_t done = 0; done < a.frames;) {
      const uint32_t n = a.frames - done < kChunk ? a.frames - done : kChunk;
      readFading<L, kPosSpl>(e, s, rs, buf, kPosSpl ? pos : pos + static_cast<size_t>(done) * L, n,
                             out + static_cast<size_t>(done) * L);
      done += n;
    }
  } else {
    readFading<L, kPosSpl>(e, s, rs, phase, pos, a.frames, out);
  }
  std::memcpy(&s, &rs, sizeof(rs));
  VoiceTap* tap = slot.tap();
  const bool watched = tap->wanted();
  // TMM.12-fix1: whether the JIT may compute this block's next frames inline (atom_state.h WavetablePosState).
  s.sineOn = !watched && s.fadeLeft == 0 &&
             frameTableSineCoefs(e.view, kPosSpl ? pos : nullptr, L, s.sineDc, s.sineAmp, s.sineTh);
  if (!watched) return;
  float position[8];
  const float* lastPos = kPosSpl ? pos : pos + static_cast<size_t>(a.frames - 1) * L;
  for (uint32_t l = 0; l < L; ++l) position[l] = lastPos[l];
  tap->write(s.group, L, phase, kPhaseSpl ? 0u : L, a.frames, true, position, rs.prevDelta);
}

template <uint32_t L, bool kSpl>
void voiceTapK(const KernelArgs& a) noexcept {  // RT-safe
  if (a.frames == 0 || a.resource == nullptr) return;
  auto* tap = const_cast<VoiceTap*>(static_cast<const VoiceTap*>(a.resource));
  if (!tap->wanted()) return;
  auto& s = *static_cast<VoiceTapState*>(a.state);
  const float* x = a.in[0];
  const float* last = kSpl ? x : x + static_cast<size_t>(a.frames - 1) * L;
  const float* before = !kSpl && a.frames >= 2 ? x + static_cast<size_t>(a.frames - 2) * L : s.prev;
  float inc[8];
  for (uint32_t l = 0; l < L; ++l) inc[l] = last[l] - before[l];
  for (uint32_t l = 0; l < L; ++l) s.prev[l] = last[l];
  tap->write(s.group, L, x, kSpl ? 0u : L, a.frames, false, nullptr, inc);
}

template <uint32_t L>
KernelFn wavetablePosFor(uint32_t m) noexcept {
  switch (m & 3u) {
    case 0: return &wavetablePosK<L, false, false>;
    case 1: return &wavetablePosK<L, true, false>;
    case 2: return &wavetablePosK<L, false, true>;
    default: return &wavetablePosK<L, true, true>;
  }
}

}  // namespace

KernelFn findFrameTableKernel(Atom a, uint32_t lanes, uint32_t m) noexcept {
  if (lanes != 1 && lanes != 8) return nullptr;
  switch (a) {
    case Atom::WavetablePos: return lanes == 1 ? wavetablePosFor<1>(m) : wavetablePosFor<8>(m);
    case Atom::VoiceTap:
      if (lanes == 1) return (m & 1u) ? &voiceTapK<1, true> : &voiceTapK<1, false>;
      return (m & 1u) ? &voiceTapK<8, true> : &voiceTapK<8, false>;
    default: return nullptr;
  }
}

}  // namespace pq::dsp::detail
