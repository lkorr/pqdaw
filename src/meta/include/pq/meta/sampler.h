#pragma once

// The Sampler engine, `sampler` (SM.2; docs/plans/SM.md "Where it sits", "Semantics"): one sample played per voice —
// Classic, One-Shot and Slice, warp Off, Re-Pitch, and (SM.4) Beats / Tones / Texture, (SM.5) Complex / Complex Pro
// (the granulator is SM.6). A native engine (`meta:sampler`, a leaf): the Sampler group preset (SM.3) wraps
// it with an envelope, a filter and an LFO.
//
//   inputs   hz        (Hz; unwired: the root, so the sample plays at its recorded pitch; Reads::Frequency)
//            gate      (Bool; default the voice's gate): a rising edge triggers (the playhead to the start)
//            key       (a MIDI key: Slice plays slice key − 36; unwired 0: no slice)
//            position  (0 … 1; unwired: the engine's own playhead) — wired, the output is a direct read at that
//                      position of the sample (sample_scrub: its slope picks the mip level), gate and rate ignored
//   params   start, end           0 … 1 of the sample (default 0, 1)
//            loopStart, loopLength 0 … 1 (default 0, 1): the loop is [loopStart, min(loopStart + loopLength, 1))
//            loopFade             0 … 1 s (the loop's equal-power crossfade, Classic, warp Off / Re-Pitch)
//            transpose            −48 … 48 st (stepped), detune −50 … 50 cents
//            gain                 −36 … 12 dB
//            root                 8 … 20000 Hz (default 261.63 Hz, middle C): the pitch the sample plays at rate 1
//            sampleBpm            20 … 999 (default 120): the sample's tempo (Re-Pitch's reference; the stretch modes'
//                                 beat law without markers)
//            fadeIn, fadeOut      0 … 2 s: One-Shot / Slice fades at the start and before the end (the sample's time;
//                                 warp Off / Re-Pitch)
//            grain                0.001 … 0.5 s (default 0.03): Tones / Texture grain size (taken at each trigger)
//            flux                 0 … 100 %: Texture's grain position jitter (of the hop)
//            envelope             0 … 100 % (default 100): Beats holds a slice for that % of its gap, then fades it
//            formants             0 … 100 % (default 100): Complex Pro's formant preservation (SM.5)
//            formantOrder         8 … 256 (default 128): Complex Pro's spectral envelope order (SM.5)
//   settings mode     0 Classic (loop while held: the loop setting), 1 One-Shot (start → end, then done),
//                     2 Slice (slice key − 36 of the data's slice table: [slices[n], slices[n + 1]))
//            warp     0 Off: rate = hz / root · 2^(transpose + detune/100)/12 (Slice: the transpose only);
//                     (Slice per voice: × a wired hz / voice.noteHz, so glides and bends move it);
//                     1 Re-Pitch: the same × transport.bpm / sampleBpm (the sample follows the song's tempo by speed);
//                     SM.4: 2 Beats, 3 Tones, 4 Texture (Classic / One-Shot): sampler_warp — the content follows
//                     transport.bpm from the trigger through the data's markers (none: sampleBpm), the pitch is the
//                     rate above per sample (glides continuous); Beats slices at the data's slice table (the
//                     transients); SM.5: 5 Complex, 6 Complex Pro: a phase vocoder per voice
//                     (pq/dsp/sampler_stretch.h; Tones for its first ≈ 70 ms while it primes, then a 2 ms cross);
//                     Pro moves the formants back by formants % (the resource request asks for the stretchers).
//                     Slice mode ignores 2 … 6 (its slices play unwarped).
//            loop     0 Off, 1 Forward, 2 Ping-pong (Classic; the stretch modes loop forward in content beats)
//            trigger  0 Trigger (One-Shot / Slice: plays to the end whatever the gate), 1 Gate (the gate output, and
//                     so the envelope after it, follows the note: the voice releases at note-off)
//            beatsLoop 0 Off, 1 Forward (default), 2 Back-Forward: what a Beats slice does past its natural end
//   data     {sample: a SampleId value (0: none), slices: Floats (start frames), sliceBy, sensitivity: Numbers,
//             markers: Floats (frame, beat pairs: the stretch modes' law), rootText: String (the root as an identity,
//             SM.7 / SM.10)}
//   outputs  out (stereo: a mono sample on both channels); gateOut (1 / 0, for the envelope after it: Classic the input
//            gate; One-Shot / Slice: playing, in Gate mode also the input gate); done (Type::voiceEnd: not gateOut,
//            Classic: not (gate and playing)); SM.8: outside a voice (no notes reach it) gateOut is also high
//            while a wired position scrubs, so the envelope after it opens (an LFO on an audio track plays it)
//   resource Sample request 0 (the link pins and publishes the data's sample, slices and markers into it; a data edit
//            is no compile; SM.5: `stretch` in Complex / Pro, the slot then carries the stretchers); SM.10: VoiceTap request 1 (voice_tap of where each voice reads, 0 … 1: the face's playheads)
// Settings choose the atoms (a mode change recompiles): Off / Re-Pitch is one sample_read_cubic (+ sample_slice in
// Slice, + transport.bpm with Re-Pitch), so they keep SM.2's path and cost; Beats / Tones / Texture one sampler_warp;
// an unwired position folds the scrub path away.

#include "pq/meta/type.h"

namespace pq::meta {

const Type& samplerType() noexcept;

enum SamplerInput : uint32_t { kSmpHz, kSmpGate, kSmpKey, kSmpPosition, kSmpInputs };
enum SamplerParam : uint32_t { kSmpStart, kSmpEnd, kSmpLoopStart, kSmpLoopLength, kSmpLoopFade, kSmpTranspose,
                               kSmpDetune, kSmpGain, kSmpRoot, kSmpSampleBpm, kSmpFadeIn, kSmpFadeOut, kSmpGrain,
                               kSmpFlux, kSmpEnvelope, kSmpFormants, kSmpFormantOrder, kSmpParams };
enum SamplerSetting : uint32_t { kSmpMode, kSmpWarp, kSmpLoop, kSmpTrigger, kSmpBeatsLoop, kSmpSettings };
enum class SamplerMode : uint8_t { Classic = 0, OneShot = 1, Slice = 2 };
// SM.4: the warp setting (SM.5: Complex / Complex Pro are the phase vocoder, pq/dsp/sampler_stretch.h).
enum class SamplerWarp : uint8_t { Off = 0, RePitch = 1, Beats = 2, Tones = 3, Texture = 4, Complex = 5, ComplexPro = 6 };
inline constexpr double kSamplerDefaultRoot = 261.63;  // unpitched → middle C (docs/plans/SM.md "Root pitch")

// The data's sample (SampleId value; 0: none) and slice table.
uint32_t samplerSample(const DataValue* data) noexcept;
std::vector<float> samplerSlices(const DataValue* data);
// A data value with `sample` and `slices` (the rest default).
DataPtr samplerData(uint32_t sample, std::vector<float> slices = {});
// SM.11: the data's warp markers ((frame, beat) pairs, both strictly ascending) and its root identity text ("": the
// root shows as Hz), and a copy of `data` (null: the default) with field `name` set to `v` (the editors' writes).
std::vector<float> samplerMarkers(const DataValue* data);
std::string samplerRootText(const DataValue* data);
DataPtr samplerWith(const DataValue* data, std::string_view name, DataValue v);

}  // namespace pq::meta
