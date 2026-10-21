// RtAudio backend checks that need no running stream (unit tests must not play sound or depend on a device being
// free). The real-device run is `pqdaw --sine`, whose dev build links the allocation trap and aborts if the audio
// callback path allocates.
#include <doctest/doctest.h>

#include <string>

#include "pq/engine/audio_device.h"
#include "pq/engine/rtaudio_device.h"

using namespace pq::engine;

namespace {
void silent(void*, const AudioBlock&) noexcept {}
}  // namespace

TEST_CASE("rtaudio: backends compiled into this build") {
  CHECK(backendCompiled(Backend::Null));
  CHECK(backendCompiled(Backend::Wasapi));
  CHECK(backendCompiled(Backend::RtAudioWasapi));
  CHECK(backendCompiled(Backend::Asio));  // our own host since T0.6-fix2
  CHECK(std::string(backendName(Backend::Wasapi)) == "WASAPI");
  CHECK(std::string(backendName(Backend::RtAudioWasapi)) == "RtAudio WASAPI");
  CHECK(std::string(backendName(Backend::Asio)) == "ASIO");
  CHECK(std::string(backendName(Backend::Null)) == "Null");
}

TEST_CASE("rtaudio: WASAPI device creation and enumeration") {
  auto dev = createAudioDevice(Backend::RtAudioWasapi);
  REQUIRE(dev.ok());
  CHECK((*dev)->backend() == Backend::RtAudioWasapi);
  CHECK_FALSE((*dev)->isOpen());
  CHECK_FALSE((*dev)->isRunning());
  auto list = (*dev)->enumerate();
  REQUIRE(list.ok());  // may be empty on a machine without audio devices
  int defaultOutputs = 0;
  for (const DeviceInfo& d : *list) {
    CHECK(d.id != 0);
    CHECK_FALSE(d.name.empty());
    defaultOutputs += d.isDefaultOutput ? 1 : 0;
  }
  CHECK(defaultOutputs <= 1);
}

TEST_CASE("rtaudio: exclusive mode is reported as unsupported, not faked") {
  auto dev = createAudioDevice(Backend::RtAudioWasapi);
  REQUIRE(dev.ok());
  StreamConfig c;
  c.shareMode = ShareMode::Exclusive;
  auto r = (*dev)->open(c, {&silent, nullptr});
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == pq::ErrorCode::Unsupported);
  CHECK_FALSE((*dev)->isOpen());
}

TEST_CASE("rtaudio: invalid stream requests fail cleanly") {
  auto dev = createAudioDevice(Backend::RtAudioWasapi);
  REQUIRE(dev.ok());
  StreamConfig c;
  c.outputDevice = 0x7FFFFFF0u;  // no such id
  CHECK((*dev)->open(c, {&silent, nullptr}).error().code == pq::ErrorCode::NotFound);
  c = StreamConfig{};
  CHECK((*dev)->open(c, AudioCallback{}).error().code == pq::ErrorCode::InvalidArgument);
  c.outputChannels = 0;
  c.inputChannels = 0;
  CHECK((*dev)->open(c, {&silent, nullptr}).error().code == pq::ErrorCode::InvalidArgument);
  c = StreamConfig{};
  c.bufferFrames = 0;
  CHECK((*dev)->open(c, {&silent, nullptr}).error().code == pq::ErrorCode::InvalidArgument);
  CHECK((*dev)->start().error().code == pq::ErrorCode::InvalidArgument);
  CHECK((*dev)->stop().ok());  // stopping a stopped device is a no-op
  CHECK(createAudioDevice(Backend::Null).ok());
}

// T0.6-fix2: Backend::Asio is our own host (AsioDevice); RtAudio's ASIO backend (Steinberg SDK files) is never
// built, so RtAudioDevice refuses it.
TEST_CASE("rtaudio: ASIO is our own host, not RtAudio's") {
  auto dev = createAudioDevice(Backend::Asio);
  REQUIRE(dev.ok());
  CHECK((*dev)->backend() == Backend::Asio);
  auto rt = RtAudioDevice::create(Backend::Asio);
  REQUIRE_FALSE(rt.ok());
  CHECK(rt.error().code == pq::ErrorCode::InvalidArgument);
}
