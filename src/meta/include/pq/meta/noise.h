#pragma once

// The Noise engine, `noise` (NZ.1; docs/plans/NZ.md, SPEC §15 2026-11-12 "a noise module"): every fundamental kind of
// randomness in one source. Audio noise and random modulation are the same signal, differing only in how often a new
// value is drawn: one `noise_step` atom (pq/dsp/atoms.h "Generators") and the Range around it.
//
//   input    clock  (Real; chain input: the module's In) — wired, a rising crossing above 0 draws a value and the
//                   clock setting is overridden (its rate / sync / note sources are cut); unwired it reads
//                   kUnwiredInput, which the body's `wired` compare folds away
//   params   rate     0.01 Hz … 48 kHz (octave curve); the top is a new value every frame (plain noise) at any sr
//            sync     0 … 26 (stepped): the Sync grid, kNoiseSyncItems (1/64T … 4/1D: triplet, straight, dotted per value)
//            smooth   0 … 1: glide over smooth · the step period (0: stepped)
//            colour   −6 … +6 dB/oct (0 white)
//            density  0.1 Hz … 100 kHz (octave); the top is continuous
//            steps    0 … 64 (stepped; 0 off, N levels across the range)
//            min, max −1 … 1, default −1 / 1: out = lerp(min, max, (v + 1) / 2) (wires add in units)
//            seed     0 … 999 (stepped): the stream (no recompile)
//   settings clock    0 Rate, 1 Sync (floor(transport.beat / grid) changes), 2 Note (voice.trigger)
//            dist     0 Uniform, 1 Gaussian (σ = 1/3)
//            restart  0 On play (rising transport.playing: every playback and bounce draws the same), 1 Free (never),
//                     2 On note (voice.trigger: every note the same pattern — every lane one stream)
//            voices   0 Per voice (lanes seeded apart; in a voice context each voice's note-on is a trig: its value
//                     and its rate start there), 1 Shared (the atom is `shared(…)`: one stream for all voices; Note
//                     clock and On note restart on any voice's note-on, voice_sum(voice.trigger))
//   output   out
//   tap      voice_tap(v) into VoiceTap request 0 (v ∈ [−1, 1], before the Range): the face's trace and dots
// Out of a voice context (a main path, an audio track) Note clock and On note have no notes: the value holds.
// Cost: the atom's (bench/RESULTS.md NZ.1) plus the Range's multiply-add; everything else is param rate.

#include <array>
#include <string_view>

#include "pq/meta/type.h"

namespace pq::meta {

const Type& noiseType() noexcept;

enum NoiseParam : uint32_t { kNoiseRate, kNoiseSync, kNoiseSmooth, kNoiseColour, kNoiseDensity, kNoiseSteps, kNoiseMin,
                             kNoiseMax, kNoiseSeed, kNoiseParams };
enum NoiseSetting : uint32_t { kNoiseClock, kNoiseDist, kNoiseRestart, kNoiseVoices, kNoiseSettings };
enum class NoiseClock : uint8_t { Rate = 0, Sync = 1, Note = 2 };
enum class NoiseRestart : uint8_t { OnPlay = 0, Free = 1, OnNote = 2 };

inline constexpr double kNoiseRateTop = 48000.0;  // the Rate knob's top: every frame
inline constexpr uint32_t kNoiseSyncCount = 27;
inline constexpr uint32_t kNoiseSyncDefault = 7;  // 1/16
// The Sync values in beats (a quarter = 1) and their names, index order (each value: triplet, straight, dotted).
double noiseSyncBeats(uint32_t index) noexcept;
inline constexpr const char* kNoiseSyncItems =
    "1/64T|1/64|1/64D|1/32T|1/32|1/32D|1/16T|1/16|1/16D|1/8T|1/8|1/8D|1/4T|1/4|1/4D|1/2T|1/2|1/2D|1/1T|1/1|1/1D|"
    "2/1T|2/1|2/1D|4/1T|4/1|4/1D";

}  // namespace pq::meta
