#pragma once

#include <cstdint>

namespace pq::engine {

// Most channels a stream or adapter handles. Channel pointer arrays are fixed-size members of this length, so
// the audio callback path never allocates them.
inline constexpr uint32_t kMaxChannels = 32;

// One block of planar (non-interleaved) float audio, as handed to an AudioCallback.
// in[c] / out[c] point at `frames` contiguous samples for channel c. `in` may be null when numIn == 0.
struct AudioBlock {
  const float* const* in = nullptr;
  uint32_t numIn = 0;
  float* const* out = nullptr;
  uint32_t numOut = 0;
  uint32_t frames = 0;
  // Frames delivered by this callback's source before the first frame of this block (device frames for a
  // device callback, engine frames for a BlockAdapter engine callback). int64 so it never wraps (SPEC §13).
  int64_t frameTime = 0;
};

// Called on the audio thread once per block. It must write all numOut × frames output samples (nothing is
// zeroed for it) and must be RT-safe: no allocation, locks, syscalls or I/O. A plain function pointer plus a
// user pointer: one indirect call per block, no std::function (CLAUDE.md rule 2).
using AudioCallbackFn = void (*)(void* user, const AudioBlock& block) noexcept;

struct AudioCallback {
  AudioCallbackFn fn = nullptr;
  void* user = nullptr;
  explicit operator bool() const noexcept { return fn != nullptr; }
};

}  // namespace pq::engine
