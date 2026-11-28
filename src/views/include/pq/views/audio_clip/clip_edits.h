#pragma once

// SX.2: the audio clip edits, value specs and readouts shared by the clip panel (AudioClipView) and the radial clip menu
// (pq/views/audio_clip/clip_menu.h), so both reach a clip through the same ops and show the same numbers. Each edit
// looks the clip up in the current document (nothing is cached across calls), dispatches one command (inside the
// caller's store gesture when one is open: a drag stays one undo step) and returns false when nothing changed — the
// clip is gone, the value is the clip's already, or the op refused (the status line then says why).
//
// UI thread only.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "pq/document/edit_ops.h"
#include "pq/document/model.h"
#include "pq/tuning/tuning.h"
#include "pq/ui/controls.h"
#include "pq/views/view_context.h"

namespace pq::views::clip_edits {

// --- lookup ---
// The audio clip `ref` names in `p` (nullptr: gone, or not an audio clip).
const doc::AudioClip* findAudioClip(const doc::Project& p, const doc::ClipRef& ref) noexcept;
// The tuning a Steps transpose of the clip resolves in (nullopt: none resolves; steps are then semitones).
std::optional<tuning::Tuning> clipTuning(const doc::Project& p, const doc::ClipRef& ref);
// The sample's detected tempo when confident (>= 0.6), else 0 (what Warp as Loop's bar count uses).
double detectedTempo(ViewContext& ctx, const doc::AudioClip& c);

// --- transpose in the clip's tuning ---
double transposeCents(const std::optional<tuning::Tuning>& tun, const doc::AudioTranspose& t);
// The degree of the tuning nearest `cents` (a Steps transpose; no tuning: semitones).
doc::AudioTranspose nearestStep(const std::optional<tuning::Tuning>& tun, double cents);
// The mean size of one step of the tuning in cents (100 without one).
double meanStepCents(const std::optional<tuning::Tuning>& tun);
// The transpose `count` steps (octave: octaves of 1200 ¢, the unit kept where it can be) above (dir > 0) or below
// `cur`; nullopt when it would leave ±kMaxTransposeCents. The panel knob's Up / Down and wheel.
std::optional<doc::AudioTranspose> steppedTranspose(const std::optional<tuning::Tuning>& tun, const doc::AudioTranspose& cur,
                                                    int dir, bool octave, int count = 1);
// The ratio nearest 2^(cents / 1200) with a denominator <= maxDen (a Ratio transpose; within ±kMaxTransposeCents).
doc::AudioTranspose nearestRatio(double cents, int64_t maxDen);
// `t` re-expressed in `unit`: Cents keeps the pitch exactly, Steps takes the nearest degree, Ratio the nearest ratio
// with a denominator <= 32. `t` itself when it already is in `unit`.
doc::AudioTranspose inUnit(const std::optional<tuning::Tuning>& tun, const doc::AudioTranspose& t, doc::TransposeUnit unit);

// --- the edits (ops::setAudioClip…; one dispatch each) ---
bool setGain(ViewContext& ctx, const doc::ClipRef& ref, float db);
bool setTranspose(ViewContext& ctx, const doc::ClipRef& ref, const doc::AudioTranspose& t);
// Typed text through parseTranspose (audio_clip_view.h); unparsable: the status line says what parses.
bool setTransposeText(ViewContext& ctx, const doc::ClipRef& ref, std::string_view text);
bool setDetune(ViewContext& ctx, const doc::ClipRef& ref, float cents);
bool setReverse(ViewContext& ctx, const doc::ClipRef& ref, bool on);
// ops::setWarpMode: from Off the clip is warped from the shape it has (it sounds the same until it is edited or the
// tempo changes). setWarped: the Warp toggle — on: ViewPrefs::defaultWarpMode, off: Off.
bool setWarp(ViewContext& ctx, const doc::ClipRef& ref, doc::AudioWarp warp);
bool setWarped(ViewContext& ctx, const doc::ClipRef& ref, bool on);
bool setSourceBpm(ViewContext& ctx, const doc::ClipRef& ref, double bpm);  // warped clips only
bool scaleSourceBpm(ViewContext& ctx, const doc::ClipRef& ref, double factor);  // :2 / ×2 (kept within kMinBpm … kMaxBpm)
bool setLoopEnabled(ViewContext& ctx, const doc::ClipRef& ref, bool on);  // warped clips only; a fresh loop spans the clip
// The fades in milliseconds at the clip's tempo (as the panel shows them); `curveIn` / `curveOut` unset: kept.
bool setFadesMs(ViewContext& ctx, const doc::ClipRef& ref, double inMs, double outMs, std::optional<float> curveIn = std::nullopt,
                std::optional<float> curveOut = std::nullopt);
bool setWarpParams(ViewContext& ctx, const doc::ClipRef& ref, const doc::WarpParams& p);

// The clip's fades and length in ms (the panel's boxes: the fades' range is the clip's length).
struct FadesMs {
  double in = 0.0, out = 0.0, clipMs = 1.0;
};
FadesMs fadesMs(const doc::Project& p, const doc::AudioClip& c);

// --- the controls' specs, readouts and choices (the panel's and the menu's) ---
ui::ValueSpec transposeSpec();
ui::ValueSpec detuneSpec();
ui::ValueSpec gainSpec();
ui::ValueSpec fadeSpec(double clipMs);
ui::ValueSpec bpmSpec();
ui::ValueSpec beatsEnvelopeSpec();
ui::ValueSpec grainSpec();
ui::ValueSpec fluxSpec();
ui::ValueSpec formantsSpec();
ui::ValueSpec envelopeOrderSpec();
std::string formatDetune(double cents);  // "0 ¢", "+12 ¢"
std::string formatGain(double db);       // "-inf dB" at the floor, "-3.5 dB"
// Beats' Preserve choices (granulations: Transients, 1/4 … 1/32) and Transient Loop choices.
inline constexpr Ticks kGranulations[] = {0, kPpq, kPpq / 2, kPpq / 4, kPpq / 8};
inline constexpr const char* kGranulationLabels[] = {"Trans", "1/4", "1/8", "1/16", "1/32"};
inline constexpr const char* kBeatsLoopLabels[] = {"Off", "\xE2\x86\x92", "\xE2\x86\x94"};  // Off, →, ↔
// The Preserve choice a granulation shows as (an unknown one reads as 1/16).
int32_t granulationIndex(Ticks g) noexcept;

// --- which controls a mode reads (the panel greys / hides the others) ---
inline bool transposeApplies(doc::AudioWarp w) noexcept { return doc::warpTransposes(w); }  // transpose, detune
inline bool tempoApplies(doc::AudioWarp w) noexcept { return doc::isWarped(w); }  // BPM, :2, ×2, Loop

}  // namespace pq::views::clip_edits
