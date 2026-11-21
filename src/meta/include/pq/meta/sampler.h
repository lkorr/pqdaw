#pragma once

// The Sampler engine, `sampler` (SM.2; docs/plans/SM.md "Where it sits", "Semantics"): one sample played per voice —
// Classic, One-Shot and Slice, warp Off or Re-Pitch (Beats / Tones / Complex and the granulator are SM.4 – SM.6). A
// native engine (`meta:sampler`, a leaf): the Sampler group preset (SM.3) wraps it with an envelope, a filter and an
// LFO.
//
//   inputs   hz        (Hz; unwired: the root, so the sample plays at its recorded pitch; Reads::Frequency)
//            gate      (Bool; default the voice's gate): a rising edge triggers (the playhead to the start)
//            key       (a MIDI key: Slice plays slice key − 36; unwired 0: no slice)
//            position  (0 … 1; unwired: the engine's own playhead) — wired, the output is a direct read at that
//                      position of the sample (sample_scrub: its slope picks the mip level), gate and rate ignored
//   params   start, end           0 … 1 of the sample (default 0, 1)
//            loopStart, loopLength 0 … 1 (default 0, 1): the loop is [loopStart, min(loopStart + loopLength, 1))
//            loopFade             0 … 1 s (the loop's equal-power crossfade, Classic)
//            transpose            −48 … 48 st (stepped), detune −50 … 50 cents
//            gain                 −36 … 12 dB
//            root                 8 … 20000 Hz (default 261.63 Hz, middle C): the pitch the sample plays at rate 1
//            sampleBpm            20 … 999 (default 120): Re-Pitch's tempo reference
//            fadeIn, fadeOut      0 … 2 s: One-Shot / Slice fades at the start and before the end (the sample's time)
//   settings mode     0 Classic (loop while held: the loop setting), 1 One-Shot (start → end, then done),
//                     2 Slice (slice key − 36 of the data's slice table: [slices[n], slices[n + 1]))
//            warp     0 Off: rate = hz / root · 2^(transpose + detune/100)/12 (Slice: the transpose only);
//                     (Slice per voice: × a wired hz / voice.noteHz, so glides and bends move it);
//                     1 Re-Pitch: the same × transport.bpm / sampleBpm (the sample follows the song's tempo by speed)
//            loop     0 Off, 1 Forward, 2 Ping-pong (Classic)
//            trigger  0 Trigger (One-Shot / Slice: plays to the end whatever the gate), 1 Gate (the gate output, and
//                     so the envelope after it, follows the note: the voice releases at note-off)
//   data     {sample: a SampleId value (0: none), slices: Floats (start frames), sliceBy, sensitivity: Numbers,
//             markers: Floats (frame, beat pairs, SM.4), rootText: String (the root as an identity, SM.7 / SM.10)}
//   outputs  out (stereo: a mono sample on both channels); gateOut (1 / 0, for the envelope after it: Classic the input
//            gate; One-Shot / Slice: playing, in Gate mode also the input gate); done (Type::voiceEnd: not gateOut,
//            Classic: not (gate and playing)); SM.8: outside a voice (no notes reach it) gateOut is also high
//            while a wired position scrubs, so the envelope after it opens (an LFO on an audio track plays it)
//   resource Sample request 0 (the link pins and publishes the data's sample into it; a data edit is no compile);
//            SM.10: VoiceTap request 1 (voice_tap of where each voice reads, 0 … 1: the face's playheads)
// Settings choose the atoms (a mode change recompiles): every mode is one sample_read_cubic (+ sample_slice in Slice,
// + transport.bpm with Re-Pitch); an unwired position folds the scrub path away.

#include "pq/meta/type.h"

namespace pq::meta {

const Type& samplerType() noexcept;

enum SamplerInput : uint32_t { kSmpHz, kSmpGate, kSmpKey, kSmpPosition, kSmpInputs };
enum SamplerParam : uint32_t { kSmpStart, kSmpEnd, kSmpLoopStart, kSmpLoopLength, kSmpLoopFade, kSmpTranspose,
                               kSmpDetune, kSmpGain, kSmpRoot, kSmpSampleBpm, kSmpFadeIn, kSmpFadeOut, kSmpParams };
enum SamplerSetting : uint32_t { kSmpMode, kSmpWarp, kSmpLoop, kSmpTrigger, kSmpSettings };
enum class SamplerMode : uint8_t { Classic = 0, OneShot = 1, Slice = 2 };
inline constexpr double kSamplerDefaultRoot = 261.63;  // unpitched → middle C (docs/plans/SM.md "Root pitch")

// The data's sample (SampleId value; 0: none) and slice table.
uint32_t samplerSample(const DataValue* data) noexcept;
std::vector<float> samplerSlices(const DataValue* data);
// A data value with `sample` and `slices` (the rest default).
DataPtr samplerData(uint32_t sample, std::vector<float> slices = {});

}  // namespace pq::meta
