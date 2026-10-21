#include "pq/engine/audio_device.h"

#include "pq/engine/null_device.h"
#include "pq/engine/rtaudio_device.h"

namespace pq::engine {

const char* backendName(Backend b) noexcept {
  switch (b) {
    case Backend::Null: return "Null";
    case Backend::Wasapi: return "WASAPI";
    case Backend::Asio: return "ASIO";
  }
  return "?";
}

Result<std::unique_ptr<AudioDevice>> createAudioDevice(Backend backend) {
  if (backend == Backend::Null) return std::unique_ptr<AudioDevice>(std::make_unique<NullDevice>());
  auto dev = RtAudioDevice::create(backend);
  if (!dev) return std::move(dev).error();
  return std::unique_ptr<AudioDevice>(std::move(dev).value());
}

}  // namespace pq::engine
