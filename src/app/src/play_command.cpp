// --play (T1.7): compile a patch (pq_compile: the JIT, or the interpreter) and play it on the audio device. Parsing
// of the flags lives in cli.cpp; the device/adapter setup mirrors --sine (audio_commands.cpp).
#include "pq/app/play.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "pq/app/cli.h"
#include "pq/app/commands.h"
#include "pq/engine/audio_device.h"
#include "pq/engine/block_adapter.h"
#include "pq/engine/image_runner.h"

namespace pq::app {

Arpeggio::Arpeggio(std::vector<float> hz, double sampleRate, double stepSeconds) : hz_(std::move(hz)) {
  if (hz_.empty()) hz_.push_back(261.626f);
  step_ = std::max<int64_t>(2, std::llround(stepSeconds * sampleRate));
  gate_ = step_ / 2;
}

void Arpeggio::produce(void* self, int64_t t, uint32_t frames, engine::EventBuffer& out) noexcept {  // RT-safe
  const Arpeggio& a = *static_cast<const Arpeggio*>(self);
  const int64_t end = t + frames;
  // Note k sounds over [k·step, k·step + gate); a block holds at most a few notes' events, pushed in time order.
  for (int64_t k = std::max<int64_t>(0, (t - a.gate_) / a.step_); k * a.step_ < end; ++k) {
    const int64_t on = k * a.step_, off = on + a.gate_;
    const uint32_t id = static_cast<uint32_t>(k + 1);
    if (on >= t && on < end)
      out.push(engine::noteOn(static_cast<uint32_t>(on - t), id, a.hz_[static_cast<size_t>(k) % a.hz_.size()], 0.7f));
    if (off >= t && off < end) out.push(engine::noteOff(static_cast<uint32_t>(off - t), id));
  }
}

Result<CompiledPlay> compilePatchFile(const std::string& path, std::string_view patch, double sampleRate,
                                      uint32_t maxBlockFrames, compile::Backend backend) {
  auto src = compile::SourceFile::load(compile::pathFromUtf8(path));
  if (!src) return src.error();
  auto k = (*src)->findPatch(patch);
  if (!k) return k.error();
  compile::CompileOptions co;
  co.sampleRate = sampleRate;
  auto compiled = compile::compilePatch(**src, *k, co);
  if (!compiled) return compiled.error();
  compile::ImageOptions io;
  io.maxBlockFrames = maxBlockFrames;
  io.maxVoiceGroups = engine::voiceGroupsFor(kPlayVoices);
  auto image = compile::buildImage(compiled->schedule, backend, io);
  if (!image) return image.error();
  CompiledPlay c;
  c.patchName = compiled->patchName;
  c.backend = backend;
  c.ops = static_cast<uint32_t>(compiled->schedule.ops.size());
  for (const engine::IoSlot& s : (*image)->io().globalOut) c.outputs += s.isExport ? 0u : 1u;
  for (const engine::IoSlot& s : (*image)->io().voiceOut) c.voiceOutputs += s.isExport ? 0u : 1u;
  if (c.outputs == 0 && c.voiceOutputs == 0)
    return Error{ErrorCode::Unsupported, "patch '" + c.patchName + "' has no `out` (global or voice) to play", {}};
  c.doneSlot = engine::findVoiceSlot((*image)->io(), compiled->scheduled().patch, "done");
  c.image = std::move(image.value());
  return c;
}

int runPlay(const CliOptions& options) {
  const engine::Backend backend = options.asio ? engine::Backend::Asio
                                  : options.wasapi == WasapiImpl::Own ? engine::Backend::Wasapi
                                                                      : engine::Backend::RtAudioWasapi;
  auto created = engine::createAudioDevice(backend);
  if (!created) {
    std::fprintf(stderr, "pqdaw: %s\n", created.error().message.c_str());
    return 1;
  }
  engine::AudioDevice& dev = **created;
  engine::StreamConfig config;
  config.outputDevice = options.device;
  config.outputEndpointId = options.deviceEndpoint;
  config.outputChannels = 2;
  config.sampleRate = options.sampleRate;
  config.bufferFrames = options.bufferFrames;
  config.shareMode = options.exclusive ? engine::ShareMode::Exclusive : engine::ShareMode::Shared;
  engine::BlockAdapter adapter;
  engine::ImageRunner runner;
  auto opened = dev.open(config, adapter.deviceCallback());
  if (!opened) {
    std::fprintf(stderr, "pqdaw: %s\n", opened.error().message.c_str());
    return 1;
  }
  const engine::StreamInfo info = *opened;
  // Compiled for the device's rate, with the internal block as the largest block.
  const compile::Backend programBackend =
      options.playBackend == PlayBackend::Interpreter ? compile::Backend::Interpreter : compile::Backend::Jit;
  auto compiled =
      compilePatchFile(options.playPath, options.patchName, info.sampleRate, options.blockFrames, programBackend);
  if (!compiled) {
    std::fprintf(stderr, "pqdaw: %s\n", compiled.error().message.c_str());
    dev.close();
    return 1;
  }
  const bool instrument = compiled->outputs == 0;
  const float gain = static_cast<float>(std::pow(10.0, options.levelDb / 20.0)) * (instrument ? kPlayVoiceHeadroom : 1.0f);
  if (!runner.prepare(compiled->image.get(), info.inputChannels, info.outputChannels, gain, true) ||
      !adapter.prepare(runner.callback(), info.inputChannels, info.outputChannels, options.blockFrames,
                       info.bufferFrames)) {
    std::fprintf(stderr, "pqdaw: invalid stream configuration\n");
    dev.close();
    return 1;
  }
  // An instrument (voice outs only): the voice engine plays the --notes arpeggio.
  engine::VoiceEngine voices;
  auto notes = parsePlayNotes(options.notes.empty() ? std::string_view("60 64 67 72") : std::string_view(options.notes));
  if (!notes) {
    std::fprintf(stderr, "pqdaw: %s\n", notes.error().message.c_str());
    dev.close();
    return 1;
  }
  Arpeggio arp(notes.value(), info.sampleRate);
  if (instrument) {
    engine::VoiceConfig vc;
    vc.maxVoices = kPlayVoices;
    vc.doneSlot = compiled->doneSlot;
    if (auto s = voices.prepare(compiled->image.get(), vc); !s) {
      std::fprintf(stderr, "pqdaw: %s\n", s.error().message.c_str());
      dev.close();
      return 1;
    }
    runner.setVoices(&voices, arp.source());
  }
  std::printf("patch:    '%s' from %s: %u ops (%s), %u %s output(s) played at %.1f dBFS, clipped to [-1, 1]\n",
              compiled->patchName.c_str(), options.playPath.c_str(), compiled->ops,
              compile::backendLabel(compiled->backend),
              instrument ? compiled->voiceOutputs : compiled->outputs, instrument ? "voice" : "global", options.levelDb);
  if (instrument)
    std::printf("notes:    %zu-note arpeggio, one note per 0.25 s, up to %u voices\n", notes->size(), kPlayVoices);
  std::printf("device:   %s [%s], %u Hz, %u ch out, buffer %u frames; engine block %u (%s)\n",
              info.outputDeviceName.c_str(), engine::backendName(info.backend), info.sampleRate, info.outputChannels,
              info.bufferFrames, adapter.blockFrames(), adapter.directMode() ? "direct" : "FIFO");
  if (auto s = dev.start(); !s) {
    std::fprintf(stderr, "pqdaw: %s\n", s.error().message.c_str());
    dev.close();
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::duration<double>(options.seconds));
  const auto stopped = dev.stop();
  const engine::CallbackStats::Snapshot st = dev.stats().snapshot();
  dev.close();
  std::printf("ran:      %llu callbacks, %llu engine blocks; xruns %llu; callback time mean %.2f us, max %.2f us "
              "(period %.1f us); rejected blocks %llu\n",
              static_cast<unsigned long long>(st.callbacks), static_cast<unsigned long long>(runner.blocks()),
              static_cast<unsigned long long>(st.xruns()), st.meanDurationUs, st.maxDurationUs, st.periodUs,
              static_cast<unsigned long long>(compiled->image->rejectedBlocks()));
  if (instrument)
    std::printf("voices:   %llu note-ons, %llu ended, %llu steals\n",
                static_cast<unsigned long long>(voices.stats().noteOns),
                static_cast<unsigned long long>(voices.stats().voicesEnded),
                static_cast<unsigned long long>(voices.stats().steals));
  if (!stopped) {
    std::fprintf(stderr, "pqdaw: %s\n", stopped.error().message.c_str());
    return 1;
  }
  return 0;
}

}  // namespace pq::app
