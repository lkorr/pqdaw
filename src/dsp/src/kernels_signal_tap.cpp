// signal_tap (TX.spectrum): the analysis-tap sink. Semantics: pq/dsp/atoms.h; the ring: pq/core/tap_ring.h.
//
// Cost: unwatched, one relaxed load per call (TapRing::wanted); watched, a copy of the block per channel (8 lanes: the
// lanes summed first, in chunks on the stack).

#include <algorithm>

#include "kernels_common.h"
#include "pq/core/tap_ring.h"

namespace pq::dsp::detail {
namespace {

template <uint32_t M>
void signalTap1(const KernelArgs& a) noexcept {  // RT-safe
  if (a.frames == 0 || a.resource == nullptr) return;
  auto* tap = const_cast<TapRing*>(static_cast<const TapRing*>(a.resource));
  if (!tap->wanted()) return;
  tap->write(a.in, kSignalTapChannels, a.frames, M);
}

// A voice group's signal: the sum over its 8 lanes (silent lanes are 0).
template <uint32_t M>
void signalTap8(const KernelArgs& a) noexcept {  // RT-safe
  if (a.frames == 0 || a.resource == nullptr) return;
  auto* tap = const_cast<TapRing*>(static_cast<const TapRing*>(a.resource));
  if (!tap->wanted()) return;
  constexpr uint32_t kChunk = 128;
  alignas(32) float sum[kSignalTapChannels][kChunk];
  const float* ptr[kSignalTapChannels];
  for (uint32_t c = 0; c < kSignalTapChannels; ++c) ptr[c] = sum[c];
  for (uint32_t done = 0; done < a.frames; done += kChunk) {
    const uint32_t n = std::min(kChunk, a.frames - done);
    for (uint32_t c = 0; c < kSignalTapChannels; ++c) {
      const bool spl = (M >> c) & 1u;
      for (uint32_t i = 0; i < n; ++i) {
        const float* x = a.in[c] + (spl ? 0u : static_cast<size_t>(done + i) * kGroupLanes);
        float s = 0.0f;
        for (uint32_t l = 0; l < kGroupLanes; ++l) s += x[l];
        sum[c][i] = s;
      }
    }
    tap->write(ptr, kSignalTapChannels, n);
  }
}

template <uint32_t... M>
constexpr std::array<KernelFn, 2 * sizeof...(M)> signalTapTable(std::integer_sequence<uint32_t, M...>) noexcept {
  return {&signalTap1<M>..., &signalTap8<M>...};
}
constexpr uint32_t kMasks = 1u << kSignalTapChannels;
constexpr auto kSignalTap = signalTapTable(std::make_integer_sequence<uint32_t, kMasks>{});

}  // namespace

KernelFn findSignalTapKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept {
  if (a != Atom::SignalTap || specMask >= kMasks) return nullptr;
  if (lanes == 1) return kSignalTap[specMask];
  if (lanes == kGroupLanes) return kSignalTap[kMasks + specMask];
  return nullptr;
}

}  // namespace pq::dsp::detail
