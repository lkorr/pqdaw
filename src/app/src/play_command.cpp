// --play (T1.7): compile a patch with the interpreter and play it on the audio device. Parsing of the flag lives
// in cli.cpp; the device/adapter setup mirrors --sine (audio_commands.cpp).
#include "pq/app/play.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include "pq/app/cli.h"
#include "pq/app/commands.h"
#include "pq/engine/audio_device.h"
#include "pq/engine/block_adapter.h"
#include "pq/engine/image_runner.h"
#include "pq/engine/interpreter.h"
#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"

namespace pq::app {

namespace {

namespace fs = std::filesystem;

Result<std::string> readText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Error{ErrorCode::NotFound, "cannot read " + path.string(), {}};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Loads imports relative to the importing file, recursively; keeps every parsed file alive for the PatchLibrary.
class FileLoader final : public graph::ImportLoader {
public:
  Result<std::vector<graph::SubcircuitRef>> load(std::string_view path, std::string_view importer) override {
    const fs::path full = fs::weakly_canonical(fs::path(importer).parent_path() / fs::path(path));
    const std::string key = full.string();
    if (auto it = byPath_.find(key); it != byPath_.end()) return graph::signatures(it->second->file);
    if (active_.count(key)) return Error{ErrorCode::InvalidArgument, "import cycle at " + key, {}};
    auto text = readText(full);
    if (!text) return text.error();
    active_[key] = 1;
    texts_.push_back(std::move(*text));
    graph::ParseOptions po;
    po.imports = this;
    auto parsed = std::make_unique<graph::ParseResult>(graph::parseDetailed(texts_.back(), key, po));
    active_.erase(key);
    if (!parsed->ok())
      return Error{ErrorCode::Parse, formatDiagnostic(texts_.back(), parsed->diagnostics.front()), {}};
    auto sigs = graph::signatures(parsed->file);
    byPath_[key] = parsed.get();
    files_.push_back(std::move(parsed));
    return sigs;
  }
  void addTo(graph::PatchLibrary& lib) const {
    for (const auto& f : files_) lib.add(f->file);
  }

private:
  std::deque<std::string> texts_;
  std::vector<std::unique_ptr<graph::ParseResult>> files_;
  std::map<std::string, graph::ParseResult*> byPath_;
  std::map<std::string, int> active_;
};

}  // namespace

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
                                      uint32_t maxBlockFrames) {
  const fs::path full = fs::weakly_canonical(fs::path(path));
  auto text = readText(full);
  if (!text) return text.error();
  FileLoader loader;
  graph::ParseOptions po;
  po.imports = &loader;
  const graph::ParseResult parsed = graph::parseDetailed(*text, full.string(), po);
  if (!parsed.ok()) return Error{ErrorCode::Parse, formatDiagnostic(*text, parsed.diagnostics.front()), {}};
  if (parsed.file.patches.empty()) return Error{ErrorCode::InvalidArgument, path + " has no patch", {}};
  size_t k = 0;
  if (!patch.empty()) {
    while (k < parsed.file.patches.size() && parsed.file.patches[k].name() != patch) ++k;
    if (k == parsed.file.patches.size())
      return Error{ErrorCode::NotFound, "no patch '" + std::string(patch) + "' in " + path, {}};
  }
  graph::PatchLibrary lib;
  lib.add(parsed.file);
  loader.addTo(lib);
  graph::InferOptions io;
  io.library = &lib;
  io.source = {*text, full.string(), parsed.nodeSpans[k]};
  auto typed = graph::infer(parsed.file.patches[k], io);
  if (!typed) return typed.error();
  graph::OptimiseOptions oo;
  oo.sampleRate = sampleRate;
  auto opt = graph::optimise(std::move(typed.value()), oo);
  if (!opt) return opt.error();
  graph::ScheduleOptions so;
  so.sampleRate = sampleRate;
  so.source = {*text, full.string(), parsed.nodeSpans[k]};
  auto sched = graph::schedule(opt.value(), so);
  if (!sched) return sched.error();
  engine::InterpretOptions eo;
  eo.maxBlockFrames = maxBlockFrames;
  eo.maxVoiceGroups = engine::voiceGroupsFor(kPlayVoices);
  auto image = engine::buildInterpreted(sched.value(), eo);
  if (!image) return image.error();
  CompiledPlay c;
  c.patchName = parsed.file.patches[k].name();
  c.ops = static_cast<uint32_t>(sched->ops.size());
  for (const engine::IoSlot& s : (*image)->io().globalOut) c.outputs += s.isExport ? 0u : 1u;
  for (const engine::IoSlot& s : (*image)->io().voiceOut) c.voiceOutputs += s.isExport ? 0u : 1u;
  if (c.outputs == 0 && c.voiceOutputs == 0)
    return Error{ErrorCode::Unsupported, "patch '" + c.patchName + "' has no `out` (global or voice) to play", {}};
  const graph::Patch& scheduled = sched->rewritten ? sched->rewritten->patch : opt->patch;
  c.doneSlot = engine::findVoiceSlot((*image)->io(), scheduled, "done");
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
  auto compiled = compilePatchFile(options.playPath, options.patchName, info.sampleRate, options.blockFrames);
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
  std::printf("patch:    '%s' from %s: %u ops (interpreter), %u %s output(s) played at %.1f dBFS, clipped to [-1, 1]\n",
              compiled->patchName.c_str(), options.playPath.c_str(), compiled->ops,
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
