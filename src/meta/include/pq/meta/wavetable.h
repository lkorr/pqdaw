#pragma once

// The Wavetable meta module, `wavetable` (TMM.5; docs/plans/MM.md "Wavetable", SPEC §15 2026-11-01): a band-limited
// multi-frame table, read by a phase, with keyframes interpolated through a position, and fold / warp / sync baked
// into the table off the audio thread.
//
//   inputs   x    the phase read (Phase mode; Reads::Phase; the chain input). TMM.10-fix1: unwired it is 0 (no
//                 implicit note); at a chain's note source it reads the note's phase phasor(voice.hz) (restarting at
//                 note-on), so a Wavetable first on a MIDI track is an oscillator because the note is wired into it.
//            pos  added to the position (modulation; default 0)
//   params   phase     0 … 1, wraps: the phase slider, added to x (the face's horizontal slider)
//            position  0 … 1: where between the keyframes the table is read (the vertical slider)
//            fold      0 … 1   ┐ bakeable: a static value is baked into the table (dsp/wave_warp.h formulas); a drag
//            warp     −1 … 1   │ rebuilds the table off-thread and crossfades it in, never a compile. TMM.8: a
//            sync      0 … 1   ┘ modulated one (a lane, a wire, a macro) runs on the live path (Phase mode only)
//   settings mode      0 Phase, 1 Shape (TMM.8)
//            warpMode  0 Bend, 1 Mirror, 2 Pulse width (dsp::WarpMode)
//            os        TMM.8: the live path's / Shape mode's oversampling: 0 auto, 1 … 4 = ×2 / ×4 / ×8 / ×16
//   output   Phase mode, static controls: out = wavetable_pos(frac(x + phase), clip(position + pos, 0, 1), table:
//            <its slot>).
//            TMM.8, the LIVE PATH (any of fold / warp / sync modulated): ph = frac(x + phase);
//              oversample(n, phase: ph) { out = wavetable_live(ph, pos, warp?, fold?, sync?, table, mode, corr) }
//            with the modulated controls wired (sync also whenever warp is: it comes first) and 0 for the others,
//            which stay baked (fold always bakes; warp unless live; sync unless live). corr = warp or sync live.
//            TMM.8, SHAPE MODE: oversample(n) { out = wavetable_shape(x + phase, pos, table) } over a table with its
//            antiderivative arrays; fold / warp / sync are baked into the curve and cannot be modulated
//            (Type::bakedOnly says why). The atoms write the slot's voice tap (the face's dots)
//   data     WavetableData (below): 1 … 256 keyframes (position + a 2048-sample cycle), a gap mode per gap, the dense
//            frame count, removeDc, normalise. Default: one sine keyframe.
//   chain    any position: the chain's signal is x (the note's phase, a voice signal, audio); ops::addMetaDevice
//            starts one that receives audio in Shape mode (TMM.8).
//
// Cost (static knobs): phasor + add + frac + the wavetable_pos read (position is param rate: clip(position) once per
// block). One keyframe builds one dense frame (the readers' single-frame path; 64 identical frames would cost 64×
// the memory for nothing).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/meta/data.h"
#include "pq/meta/type.h"

namespace pq::meta {

const Type& wavetableType() noexcept;

// Indices into the type's specs.
enum WavetableInput : uint32_t { kWtInX = 0, kWtInPos = 1 };
enum WavetableParam : uint32_t { kWtPhase = 0, kWtPosition = 1, kWtFold = 2, kWtWarp = 3, kWtSync = 4 };
enum WavetableSetting : uint32_t { kWtMode = 0, kWtWarpMode = 1, kWtOversampling = 2 };
// TMM.8: the factor of the live path's / Shape mode's oversample region for these settings and modulated params:
// the `os` setting (1 … 4 = ×2 … ×16), or auto (0): Shape mode ×2; Phase mode ×16 with a modulated warp or sync, ×8
// for fold alone (measured: tests/dsp/test_wave_live.cpp, QUESTIONS TMM.8).
uint32_t wavetableOversampling(std::span<const double> settings, std::span<const uint8_t> modulated) noexcept;
inline constexpr uint32_t kWtDefaultFrames = 64;

// The data in C++ form (the face's editing tools build it; the file keeps it as a DataValue record):
//   {"keyframes": [{"pos": 0.0, "cycle": {"$f32": …}}, …], "gaps": ["spectral" | "crossfade", …], "frames": 64,
//    "removeDc": 0, "normalise": 0}
struct WavetableKeyframe {
  float position = 0.0f;     // [0, 1], non-decreasing along the list
  std::vector<float> cycle;  // dsp::kKeyframeCycle samples
};
struct WavetableData {
  std::vector<WavetableKeyframe> keyframes;  // 1 … 256
  std::vector<dsp::GapMode> gaps;            // keyframes − 1 (Spectral by default)
  uint32_t frames = kWtDefaultFrames;        // dense frames, 1 … 256
  bool removeDc = false;
  bool normalise = false;
};
DataPtr wavetableData(const WavetableData& d);
Result<WavetableData> parseWavetableData(const DataValue& v);
std::string wavetableDataProblem(const DataValue& v);  // "" = valid
// The default data: one sine keyframe at position 0.
DataPtr wavetableDefaultData();

}  // namespace pq::meta
