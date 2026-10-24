#pragma once

// `pqdaw --play` (T1.7): a .pqp file → pq_compile (pq/compile/compile.h, the pipeline pqc runs too: parse with the
// imports relative to the file → infer → optimise → schedule → a ProgramImage on the JIT or the interpreter;
// T1.x-arch2), for one patch with at least one global `out`, or (T1.8) an instrument patch with voice `out`s, played
// through the VoiceEngine with an arpeggio (--notes). Minimal on purpose: T3.16 has the real sequencer.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pq/compile/compile.h"
#include "pq/core/result.h"
#include "pq/engine/image_runner.h"
#include "pq/engine/program_image.h"
#include "pq/engine/voices.h"

namespace pq::app {

struct CompiledPlay {
  std::unique_ptr<engine::ProgramImage> image;
  std::string patchName;   // the patch compiled
  compile::Backend backend = compile::Backend::Jit;
  uint32_t ops = 0;        // schedule size, for the summary line
  uint32_t outputs = 0;    // global `out` slots played
  uint32_t voiceOutputs = 0;  // voice `out` slots (played through the VoiceEngine when there is no global out)
  uint32_t doneSlot = engine::kNoSlot;  // the voice out/export named `done`, if any
};

// The --notes test sequence: note k starts at k · step (k = 0, 1, …) with pitch hz[k mod n] and velocity 0.7, and
// is released half a step later. Note ids are k + 1. RT-safe event source (pure arithmetic).
class Arpeggio {
public:
  Arpeggio(std::vector<float> hz, double sampleRate, double stepSeconds = 0.25);
  engine::EventSource source() noexcept { return {&Arpeggio::produce, this}; }

private:
  static void produce(void* self, int64_t t, uint32_t frames, engine::EventBuffer& out) noexcept;
  std::vector<float> hz_;
  int64_t step_ = 1, gate_ = 1;
};

// Voices --play allows (polyphony; the image gets one more group of fade headroom).
inline constexpr uint32_t kPlayVoices = 16;
// Extra gain for instrument playback: −6 dB of headroom for overlapping release tails, so the default arpeggio of
// FmVoice peaks below --level (measured in tests/app/test_play.cpp).
inline constexpr float kPlayVoiceHeadroom = 0.5f;

// `patch` empty: the first patch of the file. Not RT-safe (file I/O, compiling).
Result<CompiledPlay> compilePatchFile(const std::string& path, std::string_view patch, double sampleRate,
                                      uint32_t maxBlockFrames, compile::Backend backend = compile::Backend::Jit);

}  // namespace pq::app
