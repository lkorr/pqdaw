#pragma once

// The circuit editor's audio preview (T4.4): implements views::circuit::PatchPlayer over an engine::ProgramHost, so
// the patch shown in the editor plays — and every clean edit hot-swaps in — through the T1.9 path: images are built
// off the audio thread (the interpreter at once; the JIT's bit-identical image follows from a background compiler and
// replaces it with an Instant swap, as ProjectLink does), published through the host's lock-free ring, state carried by
// state key, a linear 5–20 ms crossfade while audible. A patch with voice outputs plays the `pqdaw --play` arpeggio
// (C4 E4 G4 C5, one note per 0.25 s); one with global outputs plays as is (its `in`s read silence).
//
// The window adds it after the project engine (AudioOutput::setExtraSource), so it shares the device; tests render it
// offline (renderBlock). Until T4.1 puts devices in tracks this is the editor's only way to hear a patch; there, edits
// go to the document instead (PatchEditor::onCommitted) and the project's per-track compile swaps the unit.
//
// Threads: play() / stop() / stateLayout() on the UI thread; process() on the audio thread (RT-safe: no allocation,
// no locks); the background compiler's deliver on its worker (publishes under the host's own serialisation).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pq/app/play.h"
#include "pq/core/aligned_buffer.h"
#include "pq/core/spsc_ring.h"
#include "pq/core/tap_ring.h"
#include "pq/engine/audio_block.h"
#include "pq/engine/background_compile.h"
#include "pq/engine/program_host.h"
#include "pq/views/circuit/patch_player.h"

namespace pq::app {

struct PreviewConfig {
  double sampleRate = 48000.0;
  uint32_t maxBlockFrames = 128;   // the image block (the host splits device blocks into these)
  uint32_t maxDeviceFrames = 8192; // largest device block process() takes in one go (larger ones run in pieces)
  uint32_t numOut = 2;
  float gainDb = -12.0f;           // the preview's level under the project
  engine::UnitBackend jit;         // the background image builder (fn null: interpreter only)
};

class PatchPreview final : public views::circuit::PatchPlayer {
public:
  explicit PatchPreview(const PreviewConfig& config);
  ~PatchPreview() override;  // after the audio thread has stopped calling process()
  PatchPreview(const PatchPreview&) = delete;
  PatchPreview& operator=(const PatchPreview&) = delete;

  // views::circuit::PatchPlayer
  double sampleRate() const override { return cfg_.sampleRate; }
  Status play(const graph::Schedule& schedule, const graph::TypedPatch& scheduled) override;
  void stop() override;
  bool playing() const override { return active_.load(std::memory_order_acquire); }
  const graph::StateLayout* stateLayout() const override;
  // T4.6: param writes by root param name (an SPSC ring to the audio thread, applied before the next block by
  // ParamSlot::key, so a write that crosses a hot swap lands in whichever image is current), and the output tap the
  // device panels' spectrum reads (written after the program, before the preview's level; nothing while no display
  // wants it).
  bool setParam(std::string_view name, float value) override;
  TapRing* outputTap() noexcept override { return &tap_; }

  // Audio thread: adds the preview into b.out (RT-safe). Nothing while stopped.
  void process(const engine::AudioBlock& b) noexcept;
  engine::AudioCallback callback() noexcept { return {&PatchPreview::onBlock, this}; }
  // UI thread: called when playback starts or stops (the window attaches the preview to the audio output only while
  // it plays, so an idle DAW still suspends its stream).
  std::function<void(bool playing)> onActiveChanged;

  // A new device rate (the project engine was rebuilt at it): playback stops and the host is rebuilt at `rate`; the
  // next play() starts there. Call while no audio thread uses the preview (AudioOutput::setExtraSource(nullptr)).
  void setSampleRate(double rate);

  // Waits until the background compiler has delivered the JIT image of the last schedule (tests); true if idle.
  bool waitForJit(double seconds = 30.0);
  struct Stats {
    uint64_t published = 0, jitPublished = 0, staleJit = 0, failures = 0;
    // The last hot swap's state transfer (engine::TransferPlan): entries carried over, new, dropped.
    uint32_t lastMatched = 0, lastFresh = 0, lastDropped = 0;
  };
  Stats stats() const;

private:
  void makeHost(int kind);
  static void onBlock(void* self, const engine::AudioBlock& b) noexcept;
  static void deliver(void* self, const engine::BackgroundJob& job, Result<std::unique_ptr<engine::ProgramImage>> image);

  PreviewConfig cfg_;
  float gain_ = 0.25f;
  Arpeggio arp_;
  // Two hosts: [0] global patches, [1] patches with voice stages (run through a VoiceEngine playing the arpeggio). Made
  // when first needed, kept until the preview goes (or the rate changes), so the audio thread can switch between them
  // with one pointer.
  std::unique_ptr<engine::ProgramHost> hosts_[2];
  std::atomic<engine::ProgramHost*> current_{nullptr};
  int currentKind_ = -1, jitKind_ = 0;  // UI thread (jitKind_ under mutex_)
  std::unique_ptr<engine::BackgroundCompiler> jit_;
  std::atomic<bool> active_{false};
  mutable std::mutex mutex_;  // latest_, the voice slots and stats (UI thread vs the compiler's worker)
  std::shared_ptr<const graph::Schedule> latest_;
  engine::VoiceSlots slots_;
  Stats stats_;
  bool fresh_ = true;  // the next publish starts playback (instant), not a hot swap
  // Audio-thread scratch: the host renders here, then it is added to the device block.
  AlignedBuffer<float> scratch_;
  std::vector<float*> outs_;
  // T4.6: param writes (UI → audio) and the names of the playing program's params (UI thread, under mutex_).
  struct ParamWrite {
    uint64_t key = 0;
    float value = 0.0f;
  };
  SpscRing<ParamWrite> paramWrites_{256};
  std::vector<std::pair<std::string, uint64_t>> paramKeys_;
  TapRing tap_{2, 32768, 2048};
  void applyParamWrites(engine::ProgramHost& host) noexcept;
};

}  // namespace pq::app
