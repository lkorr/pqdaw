#include "pq/engine/offline_render.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "pq/core/aligned_buffer.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"

namespace pq::engine {

Status BufferSink::begin(uint32_t numChannels, uint32_t rate, uint64_t totalFrames) {
  sampleRate = rate;
  channels.assign(numChannels, {});
  for (auto& c : channels) c.reserve(static_cast<size_t>(totalFrames));
  return Ok{};
}

Status BufferSink::write(const float* const* ch, uint32_t frames) {
  for (size_t c = 0; c < channels.size(); ++c) channels[c].insert(channels[c].end(), ch[c], ch[c] + frames);
  return Ok{};
}

Result<RenderStats> renderOffline(const RenderRequest& rq, AudioSink& sink) {
  if (rq.image == nullptr) return Error{ErrorCode::InvalidArgument, "renderOffline: no image", {}};
  const ImageInterface& io = rq.image->io();
  if (rq.blockFrames == 0 || rq.blockFrames > io.maxBlockFrames)
    return Error{ErrorCode::InvalidArgument,
                 "renderOffline: block of " + std::to_string(rq.blockFrames) + " frames (image maximum " +
                     std::to_string(io.maxBlockFrames) + ")",
                 {}};
  if (!rq.params.empty() && rq.params.size() != io.params.size())
    return Error{ErrorCode::InvalidArgument, "renderOffline: params must have one value per param slot", {}};
  const double sr = io.sampleRate;
  if (!(sr >= 1.0) || sr > 4.0e6) return Error{ErrorCode::InvalidArgument, "renderOffline: bad sample rate", {}};

  // How many channels: one per played slot unless asked.
  uint32_t played = 0;
  for (const IoSlot& s : io.globalOut) played += s.isExport ? 0u : 1u;
  if (played == 0 && rq.voices != nullptr) played = rq.voices->numMixes();
  const uint32_t numCh = rq.numChannels != 0 ? rq.numChannels : std::max(played, 1u);
  if (numCh > kMaxChannels)
    return Error{ErrorCode::InvalidArgument, "renderOffline: at most " + std::to_string(kMaxChannels) + " channels", {}};

  ImageRunner runner;
  if (!runner.prepare(rq.image, 0, numCh, rq.gain, false))
    return Error{ErrorCode::InvalidArgument, "renderOffline: cannot prepare the image runner", {}};
  if (!rq.params.empty()) std::copy(rq.params.begin(), rq.params.end(), runner.params().begin());
  runner.transport() = rq.transport;
  if (rq.voices != nullptr) runner.setVoices(rq.voices, rq.events, std::max(rq.eventCapacity, 1u));
  runner.setBlockHook(rq.hook);

  // Chunk buffer: whole blocks, planar.
  const uint32_t block = rq.blockFrames;
  const uint32_t chunk = std::max(block, (std::max(rq.chunkFrames, 1u) + block - 1) / block * block);
  const size_t stride = (static_cast<size_t>(chunk) + 15u) & ~size_t{15};
  AlignedBuffer<float> buf(stride * numCh);
  std::vector<float*> chPtr(numCh);
  for (uint32_t c = 0; c < numCh; ++c) chPtr[c] = buf.data() + c * stride;
  std::vector<float*> outPtr(numCh);

  RenderStats st;
  st.numChannels = numCh;
  const uint64_t rejectedBefore = rq.image->rejectedBlocks();
  if (auto s = sink.begin(numCh, static_cast<uint32_t>(std::lround(sr)), rq.frames); !s) return s.error();
  double wall = 0.0;
  int64_t t = 0;
  while (static_cast<uint64_t>(t) < rq.frames) {
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(chunk, rq.frames - static_cast<uint64_t>(t)));
    const auto t0 = std::chrono::steady_clock::now();
    {
      // The per-block loop is the realtime path: no allocation (trapped in tests), FTZ/DAZ as on the device.
      RtScope rt;
      DenormalGuard denormals;
      for (uint32_t off = 0; off < n; off += block) {
        const uint32_t m = std::min(block, n - off);
        for (uint32_t c = 0; c < numCh; ++c) outPtr[c] = chPtr[c] + off;
        const AudioBlock ab{nullptr, 0, outPtr.data(), numCh, m, t + off};
        runner.process(ab);
        ++st.blocks;
      }
    }
    wall += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (auto s = sink.write(chPtr.data(), n); !s) return s.error();
    t += n;
  }
  if (auto s = sink.end(); !s) return s.error();
  st.frames = rq.frames;
  st.rejectedBlocks = rq.image->rejectedBlocks() - rejectedBefore;
  st.wallSeconds = wall;
  st.realtimeFactor = wall > 0.0 ? (static_cast<double>(rq.frames) / sr) / wall : 0.0;
  return st;
}

}  // namespace pq::engine
