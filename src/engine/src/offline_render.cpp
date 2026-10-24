#include "pq/engine/offline_render.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "render_loop.h"

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

namespace detail {

Result<RenderStats> runRenderLoop(const RenderLoop& L, AudioSink& sink) {
  const uint32_t numCh = std::max(L.numChannels, 1u);
  const uint32_t block = std::max(L.blockFrames, 1u);
  const uint32_t chunk = std::max(block, (std::max(L.chunkFrames, 1u) + block - 1) / block * block);
  const size_t stride = (static_cast<size_t>(chunk) + 15u) & ~size_t{15};
  AlignedBuffer<float> buf(stride * numCh);
  std::vector<float*> chPtr(numCh), outPtr(numCh);
  for (uint32_t c = 0; c < numCh; ++c) chPtr[c] = buf.data() + c * stride;

  RenderStats st;
  st.numChannels = numCh;
  if (auto s = sink.begin(numCh, static_cast<uint32_t>(std::lround(L.sampleRate)), L.frames); !s) return s.error();
  double wall = 0.0;
  uint64_t t = 0;
  while (t < L.frames) {
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(chunk, L.frames - t));
    const auto t0 = std::chrono::steady_clock::now();
    {
      // The per-block loop is the realtime path: no allocation (trapped in tests), FTZ/DAZ as on the device.
      RtScope rt;
      DenormalGuard denormals;
      for (uint32_t off = 0; off < n; off += block) {
        const uint32_t m = std::min(block, n - off);
        for (uint32_t c = 0; c < numCh; ++c) outPtr[c] = chPtr[c] + off;
        const AudioBlock ab{nullptr, 0, outPtr.data(), numCh, m, L.startFrame + static_cast<int64_t>(t + off)};
        L.callback.fn(L.callback.user, ab);
        if (L.gain != 1.0f)
          for (uint32_t c = 0; c < numCh; ++c)
            for (uint32_t f = 0; f < m; ++f) outPtr[c][f] = L.gain * outPtr[c][f];
        ++st.blocks;
      }
    }
    wall += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (auto s = sink.write(chPtr.data(), n); !s) return s.error();
    t += n;
    if (L.between != nullptr) L.between(L.user, L.startFrame + static_cast<int64_t>(t));
  }
  if (auto s = sink.end(); !s) return s.error();
  st.frames = L.frames;
  st.wallSeconds = wall;
  st.realtimeFactor = wall > 0.0 ? (static_cast<double>(L.frames) / L.sampleRate) / wall : 0.0;
  return st;
}

}  // namespace detail

namespace {

// The caller's image, published to a ProgramHost without handing over ownership (one forwarding call per block).
class BorrowedImage final : public ProgramImage {
public:
  explicit BorrowedImage(ProgramImage& image) noexcept : image_(image) {}
  void process(ProcessContext& ctx) noexcept override { image_.process(ctx); }  // RT-safe
  const graph::StateLayout& stateLayout() const noexcept override { return image_.stateLayout(); }
  const ImageInterface& io() const noexcept override { return image_.io(); }
  StateArena stateArena() noexcept override { return image_.stateArena(); }
  uint64_t rejectedBlocks() const noexcept override { return image_.rejectedBlocks(); }

private:
  ProgramImage& image_;
};

}  // namespace

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
  const VoiceBinding* callerBinding = rq.voices != nullptr ? rq.voices->binding() : nullptr;
  if (rq.voices != nullptr && (callerBinding == nullptr || callerBinding->image != rq.image))
    return Error{ErrorCode::InvalidArgument, "renderOffline: `voices` is not prepared for `image`", {}};

  // How many channels: one per played slot unless asked.
  uint32_t played = 0;
  for (const IoSlot& s : io.globalOut) played += s.isExport ? 0u : 1u;
  if (played == 0 && rq.voices != nullptr) played = rq.voices->numMixes();
  const uint32_t numCh = rq.numChannels != 0 ? rq.numChannels : std::max(played, 1u);
  if (numCh > kMaxChannels)
    return Error{ErrorCode::InvalidArgument, "renderOffline: at most " + std::to_string(kMaxChannels) + " channels", {}};

  // The image through the hot-swap host, as on the device.
  HostConfig hc;
  hc.sampleRate = sr;
  hc.maxBlockFrames = io.maxBlockFrames;
  hc.numIn = 0;
  hc.numOut = numCh;
  hc.externalVoices = rq.voices;
  hc.events = rq.events;
  hc.hook = rq.hook;
  hc.eventCapacity = std::max(rq.eventCapacity, 1u);
  ProgramHost host(hc);
  VoiceSlots slots;
  if (rq.voices != nullptr) {
    slots.doneSlot = rq.voices->config().doneSlot;
    slots.monitorMask = rq.voices->config().monitorMask;
  }
  if (auto p = host.publish(std::make_unique<BorrowedImage>(*rq.image), SwapPolicy{SwapMode::Instant}, slots); !p)
    return p.error();
  host.takePending();
  if (!rq.params.empty()) std::copy(rq.params.begin(), rq.params.end(), host.params().begin());
  host.transport() = rq.transport;

  detail::RenderLoop loop;
  loop.callback = host.callback();
  loop.numChannels = numCh;
  loop.sampleRate = sr;
  loop.frames = rq.frames;
  loop.blockFrames = rq.blockFrames;
  loop.chunkFrames = rq.chunkFrames;
  loop.gain = rq.gain;
  const uint64_t rejectedBefore = rq.image->rejectedBlocks();
  auto st = detail::runRenderLoop(loop, sink);
  // The caller's VoiceEngine keeps its own binding (the host's dies with it).
  if (rq.voices != nullptr) rq.voices->attach(const_cast<VoiceBinding*>(callerBinding), nullptr);
  if (!st) return st.error();
  st->rejectedBlocks = rq.image->rejectedBlocks() - rejectedBefore;
  return st;
}

Result<RenderStats> renderOffline(ProgramHost& host, const HostRenderRequest& rq, AudioSink& sink) {
  if (host.lastPublished() == nullptr) return Error{ErrorCode::InvalidArgument, "renderOffline: nothing published", {}};
  if (rq.blockFrames == 0) return Error{ErrorCode::InvalidArgument, "renderOffline: block of 0 frames", {}};
  if (rq.numChannels == 0 || rq.numChannels > kMaxChannels)
    return Error{ErrorCode::InvalidArgument, "renderOffline: 1.." + std::to_string(kMaxChannels) + " channels", {}};
  const ProgramImage* first = host.current();
  const uint64_t rejectedBefore = first != nullptr ? first->rejectedBlocks() : 0;
  detail::RenderLoop loop;
  loop.callback = host.callback();
  loop.numChannels = rq.numChannels;
  loop.sampleRate = host.lastPublished()->io().sampleRate;
  loop.frames = rq.frames;
  loop.blockFrames = rq.blockFrames;
  loop.chunkFrames = rq.chunkFrames;
  loop.startFrame = rq.startFrame;
  loop.gain = rq.gain;
  loop.between = rq.betweenChunks;
  loop.user = rq.user;
  auto st = detail::runRenderLoop(loop, sink);
  if (!st) return st.error();
  if (const ProgramImage* last = host.current()) st->rejectedBlocks = last->rejectedBlocks() - (last == first ? rejectedBefore : 0);
  return st;
}

}  // namespace pq::engine
