#pragma once

// The immutable project model (SPEC §2, §3.2, §9.3–9.6; T3.4).
//
// A Snapshot is a `shared_ptr<const Project>`. Every node that can be shared between snapshots is held by
// `shared_ptr<const T>` (tracks, clips, note chunks, automation lanes, tuning slots, the timeline), so an edit copies
// only the path from the changed node to the root and every untouched subtree stays pointer-equal across commits.
// Nodes are never mutated after they are published in a snapshot; commands build new nodes (see commands.h).
//
// View state that is not part of undo (fold, track/lane height, selection, zoom, scroll; Q15) lives in
// ViewState (store.h), not here: a snapshot only holds what undo restores.
//
// Not RT code. The audio thread never reads the document; T3.16/T3.17 lower it to plain-data images.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pq/core/result.h"
#include "pq/core/time.h"
#include "pq/document/ids.h"
#include "pq/dsp/curve.h"
#include "pq/document/note_seq.h"
#include "pq/document/panel.h"
#include "pq/meta/data.h"
#include "pq/tuning/edo.h"
#include "pq/tuning/mos.h"
#include "pq/tuning/tuning.h"

namespace pq::graph {
struct PatchFile;
}

namespace pq::doc {

// 0xRRGGBB. Clip/track colours come from the theme palette (T3.2) but are stored as plain RGB.
struct Colour {
  uint32_t rgb = 0x808080;
  friend constexpr bool operator==(Colour, Colour) noexcept = default;
};

// ---- Tunings (SPEC §3.2) ---------------------------------------------------------------------------------------

// A Scala scale tuning as data (T2.4, embedded in the project so it is self-contained, T3.5): the .scl scale and,
// if one was imported, the .kbm keyboard mapping. The mapping only decides which degree a MIDI key plays (and is
// kept for export); the pitch of degree 0 is always TuningDef::referenceHz, which an import derives from the
// mapping's reference key and frequency (makeScaleDef), so the reference stays one continuous parameter for every
// tuning kind.
struct ScaleDef {
  tuning::ScalaScale scale;  // description must be valid UTF-8 (convert with descriptionUtf8() on import)
  std::optional<tuning::KeyboardMapping> mapping;
  friend bool operator==(const ScaleDef&, const ScaleDef&) = default;
};

// A tuning definition as data: the system plus its reference pitch (the Hz of degree 0, equave 0). A JI context
// joins the variant when pq::tuning has one (tuning.h).
struct TuningDef {
  std::variant<tuning::Edo, tuning::MosParams, ScaleDef> system = tuning::Edo::standard12();
  double referenceHz = tuning::kDefaultReferenceHz;

  const tuning::Edo* edo() const noexcept { return std::get_if<tuning::Edo>(&system); }
  const tuning::MosParams* mos() const noexcept { return std::get_if<tuning::MosParams>(&system); }
  const ScaleDef* scale() const noexcept { return std::get_if<ScaleDef>(&system); }
};
bool operator==(const TuningDef& a, const TuningDef& b) noexcept;
// Builds the resolvable tuning (validates the definition: Mos::make / Scale::make, a valid .kbm, a UTF-8 scale
// description, reference > 0). Allocates for MOS and Scale.
Result<tuning::Tuning> buildTuning(const TuningDef& def);
// A Scale definition from an imported .scl (+ optional .kbm). Without a mapping the reference is `referenceHz`;
// with one it is derived from the mapping (tuning::makeScalaTuning), so key → Hz matches Scala. The description
// is converted to UTF-8 (descriptionUtf8). Errors: as buildTuning / makeScalaTuning.
Result<TuningDef> makeScaleDef(tuning::ScalaScale scale, std::optional<tuning::KeyboardMapping> mapping,
                               double referenceHz = tuning::kDefaultReferenceHz);

// A named main tuning (SPEC §3.2). Its automation lanes live on tracks (ParamRef with a Tuning* kind).
struct TuningSlot {
  TuningId id;
  std::string name;
  Colour colour;
  TuningDef def;
};
bool operator==(const TuningSlot& a, const TuningSlot& b) noexcept;
using TuningSlotPtr = std::shared_ptr<const TuningSlot>;

// Which tuning a track or clip uses.
//   Inherit: a clip uses its track's tuning; a track uses its parent group's, and a top-level track the project's
//            first main tuning.
//   Main:    a named main tuning (`main`).
//   Private: an unnamed tuning owned by this track/clip (`privateDef`).
enum class TuningMode : uint8_t { Inherit, Main, Private };
struct TuningAssignment {
  TuningMode mode = TuningMode::Inherit;
  TuningId main;
  std::shared_ptr<const TuningDef> privateDef;

  static TuningAssignment inherit() { return {}; }
  static TuningAssignment mainTuning(TuningId id) { return {TuningMode::Main, id, nullptr}; }
  static TuningAssignment privateTuning(TuningDef def) {
    return {TuningMode::Private, TuningId{}, std::make_shared<const TuningDef>(std::move(def))};
  }
};
bool operator==(const TuningAssignment& a, const TuningAssignment& b) noexcept;

// ---- Parameters and automation (SPEC §9.6) -----------------------------------------------------------------------

// Every automatable or engine-visible parameter has a stable identity. T3.17 keys param slots by ParamRef.
enum class ParamKind : uint8_t {
  TrackVolume,      // owner = track, dB in [kMinVolumeDb, kMaxVolumeDb]
  TrackPan,         // owner = track, [−1, 1]
  TrackMute,        // owner = track, 0/1 (Ableton's track activator; Q15: mute is a param, not structural)
  TrackSolo,        // owner = track, 0/1; not automatable
  TrackSend,        // owner = track, target = return track, dB
  TuningReference,  // owner = tuning, Hz
  TuningEquave,     // owner = EDO tuning, equave in cents
  MosGenerator,     // owner = MOS tuning, cents
  MosPeriod,        // owner = MOS tuning, cents
  MosStretch,       // owner = MOS tuning, cents added to the 1200-cent octave
  // T4.1: a param of a device (owner = device id, target = deviceParamHash(name)). Its range, unit and default are
  // the device patch's (DeviceCatalog, devices.h); a rack's macros and selector are its params too (T4.8). T4.18:
  // automatable (its lane carries the name: AutomationLane::paramName).
  DeviceParam,
  // T4.7: the amount of a modulation wire (owner = wire id), in [−1, 1]; a runtime param. T4.18: automatable.
  WireAmount,
  // TMM.3: a param of a NESTED meta-module instance (owner = device id, target = deviceParamHash("<path>.<name>"));
  // a runtime param "dev<id>:<path>.<name>". Not automatable or macro-mappable in MM (docs/plans/MM.md). A chain-level
  // meta device's params are its DeviceParams.
  MetaParam,
};

struct ParamRef {
  ParamKind kind = ParamKind::TrackVolume;
  uint32_t owner = 0;   // raw TrackId / TuningId
  uint32_t target = 0;  // raw TrackId of the return for TrackSend, else 0
  friend constexpr auto operator<=>(const ParamRef&, const ParamRef&) noexcept = default;

  static ParamRef track(ParamKind k, TrackId t, TrackId target = {}) { return {k, t.value, target.value}; }
  static ParamRef tuning(ParamKind k, TuningId t) { return {k, t.value, 0}; }
  static ParamRef device(DeviceId d, std::string_view param);  // {DeviceParam, d, deviceParamHash(param)}
  static ParamRef wireAmount(WireId w) { return {ParamKind::WireAmount, w.value, 0}; }
  // TMM.3: {MetaParam, d, deviceParamHash(path + "." + param)}.
  static ParamRef metaParam(DeviceId d, std::string_view path, std::string_view param);
};
// FNV-1a 32 of a device param's name: the ParamRef target of a device param (QUESTIONS Q129, item 2).
uint32_t deviceParamHash(std::string_view name) noexcept;
bool isTrackParam(ParamKind k) noexcept;
// Whether a param of kind `k` can have an automation lane (every kind but solo; T4.18: device params, wire amounts and
// chain volumes / pans too). The ParamRef form is exact: of a chain's values only kChainVolume / kChainPan.
bool isAutomatable(ParamKind k) noexcept;
bool isAutomatable(const ParamRef& r) noexcept;
// T4.18: a device, chain or wire param (its lane lives on the track holding that device: laneHome, model.h).
bool isDeviceParamKind(ParamKind k) noexcept;
struct ParamRange {
  double min = 0.0;
  double max = 1.0;
};
ParamRange paramRange(ParamKind k) noexcept;
// A device param's range is its patch's (the document does not know it: any finite value).
ParamRange paramRange(const ParamRef& r) noexcept;

// Volume/send floor: the fader bottom, treated as −∞ (silence) by the lowering. A finite floor keeps every stored
// value finite (JSON, T3.5) and matches Ableton's fader (−∞ below −70 dB).
inline constexpr float kMinVolumeDb = -70.0f;
inline constexpr float kMaxVolumeDb = 6.0f;

struct Breakpoint {
  Ticks tick = 0;
  float value = 0.0f;  // in the parameter's own unit (ParamRange)
  float curve = 0.0f;  // [−1, 1] curvature of the segment to the next point; 0 = linear (pq/core/automation_curve.h)
  BreakpointId id;
  friend bool operator==(const Breakpoint&, const Breakpoint&) noexcept = default;
};

struct AutomationLane {
  LaneId id;
  ParamRef param;
  std::vector<Breakpoint> points;  // sorted by (tick, id)
  // T4.18: a DeviceParam lane's param name (deviceParamHash(paramName) == param.target), else "". The project file
  // stores it, and the link's param key ("dev<id>:<name>") is built from it.
  std::string paramName;
};
bool operator==(const AutomationLane& a, const AutomationLane& b) noexcept;
using LanePtr = std::shared_ptr<const AutomationLane>;

// ---- Clips (SPEC §9.4) -----------------------------------------------------------------------------------------

// Loop brace in clip-content ticks (Ableton semantics: playback starts at contentStart and wraps end → start).
struct ClipLoop {
  bool enabled = false;
  Ticks start = 0;
  Ticks end = kTicksPerWhole;  // one bar of 4/4
  friend constexpr bool operator==(const ClipLoop&, const ClipLoop&) noexcept = default;
};

// A note clip. `start`/`length` place it on the arrangement timeline; notes are in content ticks, and content tick
// `contentStart` plays at arrangement tick `start` (Ableton's start marker), so resizing the clip's left edge while
// keeping notes at their absolute time changes start, length and contentStart together.
// ---- Virtual fundamental (T3.6-fix1; SPEC §4.3, QUESTIONS Q40, Q106) -----------------------------------
// The reference-anchored Ratio notes of a clip share a virtual fundamental F, DERIVED on read from what is entered
// (virtualFundamental below): F = 1/D with D = lcm over those notes of each note's effective denominator — its own
// reduced denominator, or, while the note still has the pitch an Up/Down step (or a vertical drag) gave it, the
// denominator of the fundamental it was stepped on (a HarmonicStep). Ratios stay stored reduced and exact: 5/4 Up is
// stored 3/2 with the step {note, 4, 3/2}, so the next Up still lands on 7/4. Adding 4/3 gives lcm(4, 3) = 12;
// deleting it gives 4 again; undo / redo restore both, because the steps live in the clip snapshot. An explicit
// lock (b + index, SPEC §6.2: "this note is the N-th harmonic") overrides the derivation with F = note / N.
inline constexpr int64_t kMaxFundamentalDenominator = int64_t{1} << 20;  // finer: no fundamental (own denominators)
// lcm(a, b) for a, b ≥ 1, or −1 when it exceeds `cap` (computed without overflow: std::lcm is undefined when the
// result does not fit; T2.6-fix4, M3 review L8). RT-safe.
int64_t lcmCapped(int64_t a, int64_t b, int64_t cap) noexcept;

struct HarmonicStep {
  NoteId note;
  int64_t denominator = 1;   // the fundamental 1/denominator the step used (≤ kMaxFundamentalDenominator)
  tuning::Rational ratio;    // the reference-anchored ratio it produced; the step applies while the note has it
  friend bool operator==(const HarmonicStep&, const HarmonicStep&) noexcept = default;
};

struct ClipFundamental {
  std::vector<HarmonicStep> steps;        // sorted by note id, unique; only steps finer than the ratio's own denominator
  std::optional<tuning::Rational> lock;   // explicit fundamental (relative to the clip reference), > 0
  bool empty() const noexcept { return steps.empty() && !lock; }
  friend bool operator==(const ClipFundamental&, const ClipFundamental&) noexcept = default;
};

struct NoteClip {
  ClipId id;
  std::string name;
  Colour colour;
  Ticks start = 0;
  Ticks length = kTicksPerWhole;
  Ticks contentStart = 0;
  ClipLoop loop;
  TimeSignature signature;  // display only (SPEC §9.4)
  TuningAssignment tuning;  // Inherit = the track's tuning
  bool active = true;       // deactivated clips are kept but not played
  ClipFundamental fundamental;  // T3.6-fix1: what the virtual fundamental is derived from (see above)
  NoteSeq notes;
};
bool operator==(const NoteClip& a, const NoteClip& b) noexcept;
using ClipPtr = std::shared_ptr<const NoteClip>;
// A copy of every field except the notes, which are replaced (avoids copying the old chunk directory).
NoteClip withNotes(const NoteClip& c, NoteSeq notes);

// The clip's virtual fundamental relative to its reference (see ClipFundamental): the lock if set, else 1/D, else
// nullopt (no reference-anchored Ratio note, or D > kMaxFundamentalDenominator). `ownDenominators`: the lcm of the
// reference-anchored Ratio notes' own denominators if the caller has it cached (0 none, −1 too large), or
// kComputeOwnDenominators to scan the clip. O(1) without steps (given the lcm); with steps, one pass over the notes.
inline constexpr int64_t kComputeOwnDenominators = -2;
std::optional<tuning::Rational> virtualFundamental(const NoteClip& clip,
                                                   int64_t ownDenominators = kComputeOwnDenominators);

// ---- Samples and audio clips (T5.3, T5.8; SPEC §9.4, §12; docs/plans/M5.md) -------------------------------------

// One entry of the project's sample table: a file many audio clips may play. The document never decodes it: `path` is
// what the project file stores (UTF-8, '/' separators: project-relative "samples/…" — anything not absolute is
// relative to the project directory — or absolute), the rest is what a probe of the file said when it entered the
// project (samples.h SampleProbe: media's probe in the app, the test WAV reader in tests). Which entries are missing on
// this machine is not document state (samples.h resolveSamples; the app keeps it).
struct SampleRef {
  SampleId id;
  std::string path;
  uint64_t contentHash = 0;  // samples.h sampleContentHash (0 = unknown: resolution then trusts name + size)
  uint64_t size = 0;         // bytes
  int64_t frames = 1;        // >= 1
  double rate = 48000.0;     // Hz, > 0
  uint32_t channels = 1;     // >= 1
  std::string displayName;   // the file name without its extension, unless renamed
  friend bool operator==(const SampleRef&, const SampleRef&) = default;
};
using SampleList = std::vector<SampleRef>;  // sorted by id

// How an audio clip's source follows the arrangement (M5: Off and Re-Pitch; M6 adds Beats / Tones / Texture / Complex).
//   Off      the clip keeps its duration in SECONDS (user answer 2026-10-30, plan Q1): its arrangement length follows
//            the tempo map from `start` (a tempo edit moves its end; AudioClip::length is derived, see below). The source
//            plays at its own rate, sped up by the transpose: the source frame at song time tau (seconds) is
//            contentStartFrame + (tau - tau(start)) * rate * k, k = the transpose and detune ratio (audioRateRatio). The
//            transpose changes how fast the source is read, never the clip's duration.
//   RePitch  the clip follows the tempo like a note clip: `length`, `contentStart` and `loop` are in ticks of CONTENT,
//            content tick 0 = source frame 0, one content quarter = 60 / sourceBpm seconds of source (the frame of
//            content tick c is c * rate * 60 / (sourceBpm * kPpq)); playback speed (and pitch) follows tempo / sourceBpm.
//            Transpose and detune do not apply (as in Live, which greys them out in Re-Pitch).
enum class AudioWarp : uint8_t { Off, RePitch };
// The transpose of an audio clip (plan: cents, steps of the clip's tuning, or an exact ratio).
enum class TransposeUnit : uint8_t { Cents, Steps, Ratio };
inline constexpr double kMaxTransposeCents = 4800.0;  // +-48 semitones (a resolved Steps transpose is clamped to it)
inline constexpr int32_t kMaxTransposeSteps = 1024;   // degrees of the clip's tuning
inline constexpr double kMaxDetuneCents = 50.0;
inline constexpr float kMinClipGainDb = -70.0f;       // the floor is -inf (silence), as on the faders
inline constexpr float kMaxClipGainDb = 24.0f;
struct AudioTranspose {
  TransposeUnit unit = TransposeUnit::Cents;
  double cents = 0.0;          // Cents: [-kMaxTransposeCents, kMaxTransposeCents]
  int32_t steps = 0;           // Steps: degrees of the clip's effective tuning, resolved when lowered (follows retuning)
  tuning::Rational ratio{1};   // Ratio: positive, within +-kMaxTransposeCents
  friend bool operator==(const AudioTranspose&, const AudioTranspose&) = default;
};
// A fade at a clip edge: `length` ticks (0 = none), `curve` in [-1, 1] (0 = linear in gain; the automation curve law).
struct AudioFade {
  Ticks length = 0;
  float curve = 0.0f;
  friend constexpr bool operator==(const AudioFade&, const AudioFade&) noexcept = default;
};

// An audio clip (audio tracks only). Reverse mirrors the clip in time: offset o plays what the forward clip plays at
// offset length - o, read backwards (non-destructive; plan "Decisions").
struct AudioClip {
  ClipId id;
  std::string name;
  Colour colour;
  Ticks start = 0;
  // Arrangement length. RePitch: stored. Off: DERIVED from `seconds` and the tempo map (the nearest tick to
  // tau(start) + seconds, minus start, >= 1): commands keep it in sync (audio_clip.h syncedLength) whenever the start,
  // the duration or the tempo map changes, so every generic clip edit can use start + length.
  Ticks length = kTicksPerWhole;
  SampleId sample;
  AudioWarp warp = AudioWarp::Off;
  // Off: the source frame playing at the clip start (fractional, so splits and trims keep every frame where it plays)
  // and the duration in seconds (> 0).
  double contentStartFrame = 0.0;
  double seconds = 1.0;
  // RePitch: the start marker and the loop brace in content ticks (as NoteClip), and the tempo of the source.
  Ticks contentStart = 0;
  ClipLoop loop;
  double sourceBpm = 120.0;  // [kMinBpm, kMaxBpm]
  float gainDb = 0.0f;       // [kMinClipGainDb, kMaxClipGainDb]; at the floor: silence
  AudioTranspose transpose;  // Off only
  float detuneCents = 0.0f;  // Off only; [-kMaxDetuneCents, kMaxDetuneCents]
  bool reverse = false;
  AudioFade fadeIn, fadeOut;  // the lowering clamps them to the clip (fadeIn first)
  // A crossfade with the clip that ends exactly at `start` on the same track: its length in ticks, centred on the
  // boundary (0 = none). A command that leaves the two clips no longer adjacent drops it in the same step. A tempo
  // increase that makes a warp-Off left neighbour (which keeps its seconds) run past `start` keeps it: the lowering
  // ends that clip at `start` (audio_sequence.h AudioClipDesc::cutEnd; M5 review H7), so the two stay crossfaded and
  // the tempo edit is reversible.
  Ticks crossfadeIn = 0;
  TuningAssignment tuning;  // the tuning of a Steps transpose (Inherit = the track's)
  bool active = true;
  friend bool operator==(const AudioClip& a, const AudioClip& b) noexcept = default;
};
using AudioClipPtr = std::shared_ptr<const AudioClip>;

// A track's clips, sorted by (start, id). The id is kept next to the pointer so finding a clip by id scans a
// contiguous array instead of dereferencing every clip. T5.3: exactly one of `clip` (a note clip, MIDI tracks) and
// `audio` (an audio clip, audio tracks) is set; the accessors serve the edits that treat both alike (select, move,
// copy, split, duplicate, delete, rename, colour, activate, time edits).
struct ClipSlot {
  ClipId id;
  ClipPtr clip;
  AudioClipPtr audio;

  bool isAudio() const noexcept { return audio != nullptr; }
  Ticks start() const noexcept { return audio ? audio->start : clip->start; }
  Ticks length() const noexcept { return audio ? audio->length : clip->length; }
  Ticks end() const noexcept { return start() + length(); }
  bool active() const noexcept { return audio ? audio->active : clip->active; }
  const std::string& name() const noexcept { return audio ? audio->name : clip->name; }
  Colour colour() const noexcept { return audio ? audio->colour : clip->colour; }
};

// ---- Tracks and mixer (SPEC §9.3) ------------------------------------------------------------------------------

enum class TrackKind : uint8_t { Midi, Group, Return, Master, Audio /* T5.3: audio clips, audio effects only */ };

struct Send {
  TrackId target;  // a Return track
  float levelDb = kMinVolumeDb;
  bool preFader = false;
  friend bool operator==(const Send&, const Send&) noexcept = default;
};

// Where a track's output goes. Default = its parent group, or the master for top-level tracks and returns.
enum class OutputKind : uint8_t { Default, Master, Track /* a Group track */, None /* sends only */ };
struct Output {
  OutputKind kind = OutputKind::Default;
  TrackId track;
  friend bool operator==(const Output&, const Output&) noexcept = default;
};

// TX.pan-laws (bug report 2026-11-05, QUESTIONS Q?-TX.pan-laws-a): how a track's pan p ∈ [−1, 1] sets its output's
// gains (lower.h "Signal flow"; gL / gR per channel, x the track's signal):
//   Balance        gL = min(1, 1 − p), gR = min(1, 1 + p): 0 dB on both at the centre, the far side fades out, the near
//                  side never rises above 0 dB (a mono source loses 3 dB of power at the edges). The default.
//   ConstantPower  θ = (p + 1)·π/4, gL = cos θ, gR = sin θ: −3 dB per side at the centre, 0 dB on the near side at the
//                  edge (that side +3 dB over the centre); the power of a mono source is the same everywhere. A MIDI
//                  track's law before TX.pan-laws (schema ≤ 17 MIDI tracks load with it).
//   Linear         gL = (1 − p)/2, gR = (1 + p)/2: −6 dB per side at the centre, 0 dB at the edge; the mono sum L + R
//                  of a mono source is the same everywhere (its power is +3 dB at the edges over the centre, by design).
//   Stereo         a true stereo pan, each input channel panned on its own (constant power): p > 0 moves the left input
//                  towards the right (L → (L·cos φ, L·sin φ), φ = p·π/2) while the right one stays; p < 0 mirrors it.
//                  0 dB at the centre (the image unchanged); at p = 1 both inputs come out of the right. Uncorrelated
//                  (wide) material keeps its power; identical L and R add coherently at the edge (+3 dB of power).
enum class PanMode : uint8_t { Balance, ConstantPower, Linear, Stereo };
inline constexpr PanMode kPanModes[] = {PanMode::Balance, PanMode::ConstantPower, PanMode::Linear, PanMode::Stereo};
std::string_view panModeName(PanMode m) noexcept;   // the menu's: "Balance", "Constant Power (-3 dB)", …
std::string_view panModeShort(PanMode m) noexcept;  // the mixer face's tag: "BAL", "-3 dB", "-6 dB", "STEREO"
std::string_view panModeKey(PanMode m) noexcept;    // the project file's: "balance", "constantPower", "linear", "stereo"
std::optional<PanMode> panModeFromKey(std::string_view key) noexcept;
// The 2×2 gain matrix law `m` applies at pan `p` (clamped to [−1, 1]; diagonal for every law but Stereo): the
// reference the lowering's graph computes and the tests measure against. Not RT code.
struct PanMatrix {
  double ll = 1.0, rl = 0.0;  // out L = ll·L + rl·R
  double lr = 0.0, rr = 1.0;  // out R = lr·L + rr·R
};
PanMatrix panMatrix(PanMode m, double p) noexcept;

struct Mixer {
  float volumeDb = 0.0f;
  float pan = 0.0f;
  PanMode panMode = PanMode::Balance;  // TX.pan-laws: a structural choice (a recompile of the track)
  bool mute = false;
  bool solo = false;
  bool arm = false;
  // Every track implicitly has a send to every return (Ableton); only sends that differ from the default
  // (kMinVolumeDb, post-fader) are stored, sorted by target id, so equal mixers compare equal.
  std::vector<Send> sends;
  Output output;
  friend bool operator==(const Mixer&, const Mixer&) noexcept = default;
};

// ---- Devices (T4.1, SPEC §7.3, §8; QUESTIONS Q129) ----------------------------------------------------------

// What a device is, fixed when it is created from the catalog (devices.h). TX.all-free: a track's devices are blocks
// wired by cables (Track::devices); an instrument takes the notes through its main input, so instruments go on MIDI
// tracks only. A rack chain keeps its list rule (an Instrument Rack chain may start with one instrument, then audio
// effects; an Audio Effect Rack's chains hold audio effects only).
enum class DeviceKind : uint8_t { Instrument, AudioEffect };

// A value the user set on a device: a param or a setting (a `real const` input of the device patch: structural, in its
// own units). GU.1 (docs/plans/GU.md "Curves"): a param stores its KNOB POSITION in [0, 1] — the value is its curve at
// that position (ControlCurve below: the device's own curve for the param, else the one its declaration gives,
// doc::declaredCurve in lower.h), so editing a curve changes what the position means and the sound follows; a rack's
// params (its macros, its chain selector) keep their own values 0 … 127. A nested meta module's params
// (MetaInstance::params) are positions too.
struct DeviceValue {
  std::string name;
  double value = 0.0;
  friend bool operator==(const DeviceValue&, const DeviceValue&) = default;
};

// GU.1 (SPEC §15 2026-11-06 "built GUIs"; docs/plans/GU.md "Curves"; pq/dsp/curve.h): a control's CURVE, per device
// instance and param — knob position → value (range, unit, step, up to 16 segments of shapes and bends, wrap / signed /
// output conversion). A device stores one only where it differs from the declared curve (doc::declaredCurve: the
// param's range, @curve preset, unit, @steps, @wrap, @signed); the knob's position stays when it changes (the sound
// follows). Its class (dsp::curveClass: segment count, shapes, flags) is structural for a driven control (the `curve`
// atom's specialisation and the wire law: an Add wire adds in units on a linear curve); points, bends and the range are
// data (SetParamCurve's `structural` flag says which). Stepped and bool params have curves too (their step 1).
struct ControlCurve {
  std::string param;
  dsp::Curve curve;
  friend bool operator==(const ControlCurve&, const ControlCurve&) = default;
};

// ---- Meta modules (TMM.3; docs/plans/MM.md "Architecture"; pq/meta/type.h) -------------------------------------------
// A meta module's state lives in its device, keyed by its NAME PATH (pq/meta/registry.h validPath): the call binding
// names from the device's root patch to the instance ("acc1", "opA.acc"; the editor names calls acc1, wt1, …), or ""
// for a chain-level meta device itself (Device::patch = "meta:<type>": the device IS the module).
//   type, version  the meta::Type (registered by id) and the state version it was written with (== the type's in a
//                  snapshot; older files are upgraded on load)
//   params         a nested instance's param values (runtime params "dev<id>:<path>.<name>": a face drag never
//                  recompiles); a chain device keeps its params / settings in Device::params / settings instead (so
//                  automation, macros and wires work as for any device), and its "" instance holds none
//   settings       a nested instance's structural values (a change recompiles its track's unit)
//   data           meta::DataValue (null: none): never part of the expansion; an edit is the MetaData change class
// A nested call without an entry plays the type's default state. Editing the state of a module inside a STOCK device
// does not fork the device (only its patch text does).
// TMM.10-fix1: a chain meta device's KIND says where it sits in a RACK CHAIN, not what it is: Instrument exactly when it
// is the first device of an Instrument Rack chain (its chain input takes the note source), else AudioEffect; every
// command that changes a rack chain keeps this (normaliseMetaKinds). TX.all-free: in a track's own blocks the kind of a
// meta device means nothing (what reaches its main input decides: the cables); what the device plays always follows
// from the signal reaching it (pq/document/meta.h chainStages), never from the kind.
inline constexpr std::string_view kMetaDevicePrefix = "meta:";
struct MetaInstance {
  std::string type;
  uint32_t version = 0;
  std::vector<DeviceValue> params;    // sorted by name, unique
  std::vector<DeviceValue> settings;  // sorted by name, unique
  meta::DataPtr data;
  const DeviceValue* param(std::string_view name) const noexcept;
  const DeviceValue* setting(std::string_view name) const noexcept;
};
bool operator==(const MetaInstance& a, const MetaInstance& b) noexcept;  // data deep
struct MetaEntry {
  std::string path;
  MetaInstance instance;
  friend bool operator==(const MetaEntry&, const MetaEntry&) = default;
};
using MetaMap = std::vector<MetaEntry>;  // sorted by path, unique
using MetaMapPtr = std::shared_ptr<const MetaMap>;

// One BLOCK of a track's circuit (GU.11, docs/plans/GU.md "One hierarchy"): `patch` says which of exactly four it is —
//   a GROUP          kGroupDevice: the one container (its members name it in Device::group; "Groups" below)
//   an ATOM BLOCK    "atom:<name>": one atom of the graph's registry ("Atom blocks and value blocks" below)
//   a VALUE BLOCK    kValueDevice: one control
//   a NATIVE ENGINE  "meta:<type>": a meta type whose expansion is real DSP (the wavetable oscillator, the envelope, the
//                    analysers' taps): a leaf with a face and no inside ("Meta modules" above)
// Nothing else is a block (checks.cpp refuses any other patch: no stock patch, no patch text of the instance's own).
// Ports are the block's fixed patch's signature and are not stored; params / settings not listed play the defaults.
struct Device {
  DeviceId id;
  std::string patch;
  DeviceKind kind = DeviceKind::AudioEffect;
  bool active = true;                // deactivated: left out of the graph (structural, zero CPU)
  std::vector<DeviceValue> params;   // sorted by name, unique
  std::vector<DeviceValue> settings; // sorted by name, unique (racks: none)
  // T4.17: the instance's own name (Live: Ctrl+R renames a device), shown in its title bar; "" = the patch's default
  // name (the catalog's / the panel description's title). Meta.
  std::string name;
  // TMM.3: meta-module state by name path (see "Meta modules" above); null: none. Shared, so a param edit copies the
  // Device but never the map (nor the data in it).
  MetaMapPtr meta;
  // TX.groups (SPEC §15 2026-11-01 "resize, groups, and the free-device rack"; "Groups" below): the group this device
  // belongs to (a group device of the same track's own blocks), or invalid. A member is never on its track's main path
  // (main_path.h: a group has no order inside): it plays through its cables only; membership changes no sound (Meta:
  // SetDeviceGroup), it says where the circuit draws the device (inside its group's capsule) and that the Device view
  // shows only the group.
  DeviceId group;
  // GU.1: the params whose curve is not their declared one (ControlCurve above), sorted by param, unique. Racks and
  // groups have none.
  std::vector<ControlCurve> curves;
  // GU.3 (pq/document/panel.h): a group's panel — its face built from elements bound to what is inside it; null (or
  // empty): the group is drawn as its mini circuit. Groups only. Shared, so a param edit copies the Device, never the
  // panel; SetGroupPanel replaces it whole (Meta: a panel is never lowered).
  PanelPtr panel;
  bool hasPanel() const noexcept { return panel != nullptr && !panel->empty(); }
  const dsp::Curve* curve(std::string_view param) const noexcept;  // null: the declared curve
  const MetaInstance* metaAt(std::string_view path) const noexcept;  // nullptr: no entry
  const DeviceValue* param(std::string_view key) const noexcept;    // nullptr: the patch's default
  const DeviceValue* setting(std::string_view key) const noexcept;  // nullptr: the patch's default
};
bool operator==(const Device& a, const Device& b) noexcept;  // deep (racks by content, user texts by text)
using DevicePtr = std::shared_ptr<const Device>;
// TMM.3: a chain-level meta device (patch "meta:<type>") and its type id ("" for any other device).
bool isMetaDevice(const Device& d) noexcept;
std::string_view metaDeviceType(const Device& d) noexcept;
// TMM.10-fix1: gives the chain meta devices of `list` (this level only) the kind their position says ("Meta modules"
// above; `instrumentsGo`: a MIDI track's own list, an Instrument Rack chain), copying the ones that change; their ids
// are appended to `changed` when given. False: nothing changed.
bool normaliseMetaKinds(std::vector<DevicePtr>& list, bool instrumentsGo, std::vector<DeviceId>* changed = nullptr);

// ---- Groups (TX.groups; SPEC §15 2026-11-01 "resize, groups, and the free-device rack"; QUESTIONS Q?-TX.groups-a) ----
// A GROUP is a capsule of blocks in a track's circuit (the first nested layer: a track's own blocks, never inside a
// rack or a device's circuit) that is one audio-effect block: "basic synth → group" instead of "basic synth → phase
// accumulator → wavetable → phase accumulator". It is built only from what the circuit already has — no second
// mechanism:
//   the group     a Device whose patch is kGroupDevice (kind AudioEffect, no params, no settings, no rack), a block like
//                 any (on its track's main path or off it). It plays the fixed patch groupPatchText(): its main input `x`
//                 and main output `y` are where its cables meet it, and its two TERMINALS are ports of the same device:
//                 the In terminal is its output kGroupInPort (= x: what reaches the group), the Out terminal its input
//                 kGroupOutPort (y = the sum of the wires into it). Nothing else: zero cost of its own.
//   the members   the devices whose Device::group names it, in the same track; never on the main path (no order
//                 inside: everything is placed freely): they play through their cables only. Their positions are the
//                 circuit's view state (ViewState::patchNudges by device id), as every block's.
//   the wires     ordinary Project::wires: In → a member's input is a wire from (group, kGroupInPort); a member's
//                 output → Out a wire into (group, kGroupOutPort); between members, and across the capsule's boundary
//                 (an LFO outside into a knob inside, a member's output into another track), wires work unchanged
//                 because they name devices, not containers.
// Audio enters and leaves through the terminals the user wires (ops::groupInCircuit wires a grouped series In → … → Out,
// so it sounds the same). Polyphony follows the wires, as for every free device.
// GU.2 (docs/plans/GU.md "Nesting and instrument groups"): GROUPS NEST — a group may be a member of another group of the
// same list (any depth up to kMaxGroupDepth, no cycles: checkList). A member at any depth plays through its cables as
// every member does (wires name devices, not containers, so a cable across a nested capsule's edge needs nothing).
// "Inside g" means at any depth (groupContains / groupMembersDeep); Device::group names the innermost group.
// TX.groups-fix1 (SPEC §15 2026-11-01 "notes are a signal"; QUESTIONS Q?-TX.groups-fix1-a): the In terminal carries
// WHATEVER reaches the group's place on the main path (meta.h chainStages): audio, a per-voice signal (the group
// continues the voices: they pass In and Out per voice, and the run goes on after it), or at a MIDI track's note source
// the notes (the group is the track's instrument: In gives each input it reaches the note's signal of that input's
// meaning — a frequency the note's Hz, a phase the note's phase, as a main input at the note source reads it — and the
// voices end as a meta voice run's do, by what reaches Out). The inner wires Ctrl+G makes are chain links (Wire::chain).
inline constexpr std::string_view kGroupDevice = "Group";
inline constexpr std::string_view kGroupInPort = "input";    // the In terminal: an output of the group device
inline constexpr std::string_view kGroupOutPort = "output";  // the Out terminal: an input of the group device
bool isGroupDevice(const Device& d) noexcept;
// The patch every group plays (lower.h "Groups"): `patch Group { in x; in output; out y = output; out input = x }`.
std::string_view groupPatchText() noexcept;
// TX.groups-fix1: what a group plays where voices reach it (devices.h groupVoicePatch / groupNotesPatch).
std::string_view groupVoicePatchText() noexcept;
std::string_view groupNotesPatchText() noexcept;
// The members of group `group` in `list` (list order): its own (Device::group == group), not theirs.
std::vector<const Device*> groupMembers(const std::vector<DevicePtr>& list, DeviceId group);
// GU.2: nesting. The deepest a group may sit (a member of a group of a group … : depth 1 is a top-level group's member).
inline constexpr uint32_t kMaxGroupDepth = 16;
// Every device inside group `group` at any depth (its members, their members …), list order.
std::vector<const Device*> groupMembersDeep(const std::vector<DevicePtr>& list, DeviceId group);
// Whether device `d` sits inside group `group` at any depth (false for the group itself, or when `d` is in no group).
bool groupContains(const std::vector<DevicePtr>& list, DeviceId group, DeviceId d) noexcept;
// The outermost group `d` sits in (`d` itself is not counted; invalid: `d` is in no group). Follows Device::group up to
// kMaxGroupDepth steps (a cycle, refused by checkList, stops there).
DeviceId outermostGroup(const std::vector<DevicePtr>& list, DeviceId d) noexcept;
// TX.insert-group: where a wire is drawn — its target's track (into a mixer: that track) and the innermost group
// holding both its ends. An end sits in the group of the block it touches (Device::group), but a group's own terminal
// reached from inside — its In terminal (the output kGroupInPort), its Out terminal (the input kGroupOutPort) — is
// inside that group (its other ports, its main input and output, are reached from outside). A note source, a mixer or
// an end on another track is the track's top level (an invalid group). An invalid track: the target is gone. A block
// inserted on the cable goes there.
struct Project;
struct Wire;
struct WireContainer {
  TrackId track;
  DeviceId group;
};
WireContainer wireContainer(const Project& p, const Wire& w) noexcept;

// ---- Atom blocks and value blocks (GU.2; docs/plans/GU.md "Concepts") ---------------------------------------------
// The native ingredients a module is built from, as document blocks (free devices of a track, inside groups or not):
//   an ATOM BLOCK   a Device whose patch is "atom:<name>" (kAtomDevicePrefix + an atom of graph::AtomRegistry that may
//                   stand alone: devices.h atomBlockAllowed): the atom itself — its inputs are the block's inputs (an
//                   optional one keeps the atom's default while unwired; a const one is a setting), its outputs the
//                   block's outputs. It lowers inline (devices.h atomBlockPatch: one call the compiler flattens to the
//                   atom; no generated text, no user patch). No params of its own (controls are value blocks).
//   a VALUE BLOCK   a Device whose patch is kValueDevice ("Value"): one control, `value`, whose value (through its
//                   curve, GU.1: Device::curves owns the range and law) is the block's output `y`. Undriven it costs
//                   nothing per sample (a runtime param); driven, the curve atom after the sum of its wires.
// Neither has a rack, a source text or meta modules. Kind AudioEffect.
inline constexpr std::string_view kAtomDevicePrefix = "atom:";
inline constexpr std::string_view kValueDevice = "Value";
inline constexpr std::string_view kValueParam = "value";
inline constexpr std::string_view kValueOutput = "y";
bool isAtomDevice(const Device& d) noexcept;
std::string_view atomDeviceName(const Device& d) noexcept;  // "" when not an atom block
bool isValueDevice(const Device& d) noexcept;
std::string_view valuePatchText() noexcept;  // the patch every value block plays (devices.h valuePatch)

// ---- Instrument groups (IG.1-2; SPEC §15 2026-11-04 "instrument groups", 2026-11-06 "single-signal cables";
// docs/plans/IG.md; pq/document/instrument_groups.h finds them) ------------------------------------------------------
// A group whose main input is fed NOTES (it heads a MIDI track's main path, or a notes cable from a MIDI track's source
// enters its main input) is an INSTRUMENT GROUP: everything inside runs per note (one copy per sounding voice of that
// source), its Out sums the voices; outside it everything is single-signal. Its POLYPHONY is stored as settings of the
// group device (Device::settings; SetDeviceSetting, undoable; absent = the default below):
//   voices   the voice count (1 … kMaxPolyphony); absent: the link's default (a migrated group plays as its track did)
//   mono     0 / 1: one voice (a new note takes the voice)
//   legato   0 / 1 (with mono: a note that overlaps the held one does not retrigger) — stored; IG.4 plays it
//   glide    seconds of portamento (0: none) — stored; IG.4 plays it
//   steal    0 oldest, 1 quietest, 2 none (drop notes beyond the voice count)
inline constexpr std::string_view kGroupVoices = "voices";
inline constexpr std::string_view kGroupMono = "mono";
inline constexpr std::string_view kGroupLegato = "legato";
inline constexpr std::string_view kGroupGlide = "glide";
inline constexpr std::string_view kGroupSteal = "steal";
inline constexpr uint32_t kDefaultGroupVoices = 16;  // a new instrument group's voice count (docs/plans/IG.md "Open")
enum class GroupSteal : uint8_t { Oldest = 0, Quietest = 1, None = 2 };
struct GroupPolyphony {
  uint32_t voices = 0;  // 0: not set (the link's default)
  bool mono = false, legato = false;
  double glide = 0.0;
  GroupSteal steal = GroupSteal::Oldest;
};
GroupPolyphony groupPolyphony(const Device& group) noexcept;
// A new instrument group (kGroupDevice) with the default polyphony written out (16 voices, steal oldest, no glide).
DevicePtr makeInstrumentGroup(DeviceId id);

// ---- Racks (GU.9 v2; pq/document/racks.h) -----------------------------------------------------------------------
// A rack is not stored: it is a set of PARALLEL GROUPS derived from the wiring (racks.h). These are the spawn ids the
// menus and the browser offer (ops::addDevice / ops::addRack build the groups).
inline constexpr std::string_view kAudioEffectRack = "AudioEffectRack";
inline constexpr std::string_view kInstrumentRack = "InstrumentRack";

DevicePtr makeDevice(DeviceId id, std::string patch, DeviceKind kind);
DevicePtr makeGroup(DeviceId id);              // TX.groups: an empty group device (kGroupDevice, AudioEffect)

// ---- Modulation wires (T4.7, SPEC §7.1, §7.4, §9, §15 2026-10-24; QUESTIONS Q129 item 5) -------------------------
// A wire from a device's `export` (an output jack on top of the device) to a `param` of any device in the project
// (the same device, another device of its chain, a device on another track). Wires are project entities, not track
// entities: a wire may cross tracks, and moving a device to another track keeps its wires (they name device ids).
// Removing a device (or its track) removes its wires in the same command.
//
// The modulation law (QUESTIONS Q135): additive in the target's KNOB TRAVEL (the 0..1 position its knob shows:
// linear in the value, in dB for gains, logarithmic for @curve(exp)), with the knob as the centre:
//     position = clamp(knob position + Σ amount_i · source_i, 0, 1),  value = the param's value at that position.
// A source of ±1 at amount 1 sweeps the whole travel; a negative amount inverts. The knob keeps the user's value (the
// modulation never writes the document). A voice source into a global target is reduced per wire (`reducer`, SPEC
// §7.4: sum by default). Wires add no delay; a wire that would close a delay-free cycle gets an automatic 1-sample
// delay (SPEC §15 2026-10-24; decided by the lowering, lower.h).
//
// TMM.10-fix2 (SPEC §15 2026-11-01 "notes are a signal", "the note source is a Hz piano roll"): a wire's source may
// instead be a MIDI track's NOTE SOURCE (its piano roll, in Hz): `noteSource` names the track, `from` stays invalid and
// `port` is one of its note outputs (kNoteOutputs: "hz", "gate", "velocity"). The lowering reads that track's voice
// terminals: per voice into a device that runs per voice on the same track, through the wire's reducer into any other
// (a global device of the track, or a device on another track, as an export's wire). Removing the track removes them.
//
// TX.patching (SPEC §15 2026-11-01 "a wire adds its value in the target param's own units", "free patching in the
// circuit"; QUESTIONS Q?-TX.patching-a): a wire's source is ANY output of its device (an `out` as well as an export: a
// meta module's output, an effect's audio), and its target any real param or any non-const INPUT of its device
// (`param` names either: a device's audio in, a meta module's frequency or phase-modulation input). The wire's LAW:
//   Add      every new wire. GU.1 (docs/plans/GU.md "Curves"): it ADDS IN POSITION — to the target's knob position,
//            before its curve (Device::curves / the declared curve; +1 = the whole range, through the curve: even
//            octave sweeps on an octave knob), wrapped by a wrap curve, mirrored by a signed one — unless the curve is
//            LINEAR (every segment straight), where it ADDS IN UNITS, after the curve and before its output conversion
//            (Hz into a linear cutoff = +Hz exactly; dB into a dB gain, +6 on 0 dB = +6 dB; a multiplier into a VCA on
//            its Bend curve), clamped to the curve's range (wrap: not clamped; signed: ±max(|min|, |max|)); into an
//            input, added to what the input reads (the chain's signal; an unpatched input's default is what it reads
//            only while no wire reaches it). Scaling is the patch's job (a VCA, a log, a divide): no amount (always 1).
//   Travel   in position with an `amount` (Q135's law), kept only for wires migrated from schema ≤ 12 so old projects
//            sound the same; never offered for new wires.
//   Units    in the param's DSP units, after the whole curve (output conversion included: a dB param's linear gain, the
//            VCA's multiplier), clamped as Add; kept for wires of schema 13 … 15 (migrated). Never offered for new
//            wires. No amount.
// TX.groups-fix1: a wire may be a CHAIN LINK (`chain`): a link of a series that Ctrl+G turned into wires (In → first,
// device → next, last → Out). The lowering plans chain links before every other wire, as it plans the main path's own
// links (lower.h "Modulation wires"): a feedback loop through a grouped series keeps its automatic 1-sample delay on the
// wire that had it; into one input, chain links are summed first (as the path's signal is), then the other wires in id
// order. Ctrl+Shift+G removes exactly the chain links of its group. Nothing else differs (the views draw them as any
// wire; a wire the user draws is never one).
//
// TX.all-free (SPEC §15 2026-11-03 "everything is free; the chain is just cables"; main_path.h): EVERY connection of a
// track's circuit is a wire, the series included. Two more ends:
//   MAIN PORTS   `port` kMainPort ("") is its device's MAIN OUTPUT (its first `out`; a rack's output) and `param`
//                kMainPort its target's MAIN INPUT (a stock effect's first audio `in`, a meta module's chain input
//                Device::chainInput, a group's `x`, a rack's input; an instrument's main input is its notes). A wire
//                between two main ports is what the series was: a link of the main path when it lies on it.
//   SOURCE       `noteSource` + `port` kMainPort: the track's own signal — a MIDI track's notes, an audio track's clips,
//                a group's or the master's summed input, a return's sends (any track kind; the note outputs "hz", "gate",
//                "velocity" only on a MIDI track).
//   MIXER        `mixer` valid (`to` invalid, `param` kMainPort): the track's mixer input (its volume, pan, sends,
//                routing). Only from a device of that track or its own source.
// The audio path is the wiring; the lowering compiles it (lower.h "The main path"); a track with no path to its mixer is
// silent.
inline constexpr std::string_view kMainPort = "";
enum class WireReducer : uint8_t { Sum, Last, Highest, Lowest, Loudest };
enum class WireLaw : uint8_t { Add, Travel, Units };
inline constexpr double kDefaultWireAmount = 0.5;  // a Travel wire's default depth (ops::connectTravelWire)

struct Wire {
  WireId id;
  DeviceId from;      // the source device (invalid for a source wire)
  std::string port;   // its output: an `out` or an export (a source wire: its note output), kMainPort: its main output
  DeviceId to;        // the target device (may be `from` itself; invalid into a mixer)
  std::string param;  // its param, or (TX.patching) one of its inputs; kMainPort: its main input (and into a mixer)
  // Travel: [−1, 1], knob travel per unit of source. Add: always 1 (an added wire has no depth; a VCA scales it).
  double amount = 1.0;
  WireReducer reducer = WireReducer::Sum;
  // TMM.10-fix2: valid: the source is this track's source (TX.all-free: a MIDI track's note source, or with port
  // kMainPort any track's own signal; see above).
  TrackId noteSource;
  WireLaw law = WireLaw::Add;  // TX.patching
  bool chain = false;          // TX.groups-fix1: a chain link of a grouped series (planned first; see above)
  TrackId mixer;               // TX.all-free: valid: the target is this track's mixer input (see above)
  // IG.1-2 (SPEC §15 2026-11-06 "single-signal cables"; "Instrument groups" below): a SINGLE-SIGNAL cable — the voices
  // of a per-note source are reduced at it (by `reducer`: sum, max = Highest, newest = Last), so what it feeds runs once
  // instead of per note. A cable from inside an instrument group to anything outside it reduces whatever this says (the
  // group edge is the same rule by default); inside a group a cable is per note unless this is set. Structural
  // (SetWireSignal).
  bool single = false;
  bool travels() const noexcept { return law == WireLaw::Travel; }
  bool adds() const noexcept { return law != WireLaw::Travel; }  // Add or Units: no amount
  bool fromNoteSource() const noexcept { return noteSource.valid(); }
  bool toMixer() const noexcept { return mixer.valid(); }
  // TX.all-free: a wire between two main ports (a device's or the source's main output, a device's main input or the
  // mixer): a link of the main path when it lies on it (main_path.h).
  bool mainLink() const noexcept { return port.empty() && param.empty(); }
  // Whether two wires join the same two ports (a second such wire is refused).
  bool sameEnds(const Wire& o) const noexcept {
    return from == o.from && noteSource == o.noteSource && port == o.port && to == o.to && param == o.param &&
           mixer == o.mixer;
  }
  friend bool operator==(const Wire&, const Wire&) = default;
};
using WireList = std::vector<Wire>;  // sorted by id

// ---- Block sets (GU.2; pq/document/edit_ops.h "block sets") ------------------------------------------------------
// A BLOCK SET is a piece of a track's circuit taken out of its project: blocks (a group with its members at any depth,
// racks with everything inside) and the wires among them (both ends inside the set: a member's cable into a rack's inner
// device too; never a note source's or a mixer's), with the ids they had. It is what Ctrl+C copies (ops::copyBlocks),
// Ctrl+V pastes (ops::pasteBlocks) and a group preset file holds (library_files.h). A device's group tag names a group of the set, or none (a top-level block
// of the set). GU.3: a group's panel lives on its Device (bindings by DeviceId), so a set carries it with no change of
// its own; ops::remapBlocks is the one place ids are rewritten (panel bindings join it there).
struct BlockSet {
  std::vector<DevicePtr> devices;  // its top-level blocks first, then what is inside them (each in list order)
  WireList wires;                  // by id
  bool empty() const noexcept { return devices.empty(); }
};
bool operator==(const BlockSet& a, const BlockSet& b) noexcept;


// TMM.10-fix2: the outputs of a track's note source (its Hz piano roll), in the order the views draw them: the
// voice terminal each reads (pq/graph atoms `voice.*`; "hz" is the tuned pitch, as devices hear it), its label, and
// whether the terminal is a bool (gate: read as 0 / 1). Per voice; a wire into a global target reduces it.
struct NoteOutput {
  std::string_view port;   // the Wire::port
  std::string_view label;  // its name in readouts and menus
  std::string_view pin;    // drawn next to its pin (short)
  std::string_view atom;   // the voice terminal
  bool boolean = false;
};
inline constexpr NoteOutput kNoteOutputs[] = {
    {"hz", "Hz", "Hz", "voice.hz", false},
    {"gate", "gate", "gate", "voice.gate", true},
    {"velocity", "velocity", "vel", "voice.velocity", false},
};
const NoteOutput* findNoteOutput(std::string_view port) noexcept;

struct Track;
using TrackPtr = std::shared_ptr<const Track>;

struct Track {
  TrackId id;
  TrackKind kind = TrackKind::Midi;
  std::string name;
  Colour colour;
  TuningAssignment tuning;  // used by Midi tracks and audio clips' Steps transposes (inherited through groups)
  Mixer mixer;
  bool active = true;  // deactivate (`0`): structural (Q15)
  std::vector<TrackPtr> children;  // Group only
  std::vector<ClipSlot> clips;     // Midi (note clips) and Audio (audio clips) only, sorted by (start, id)
  std::vector<LanePtr> lanes;      // automation lanes shown under this track
  // T4.1; TX.all-free: the track's blocks (devices, meta modules, groups and their members, racks). A SET: the signal
  // path is the wires (model.h "Modulation wires", main_path.h), never this order; the order only says where a block
  // nobody has placed is drawn and which block a tie goes to. Rack chains (inside racks) keep their list order.
  std::vector<DevicePtr> devices;
  // Index of the device in `devices`, or −1.
  int32_t deviceIndex(DeviceId device) const noexcept;
};
bool operator==(const Track& a, const Track& b) noexcept;

// A track with no blocks and no wires (it plays nothing: ops::createTrack wires its source to its mixer, a MIDI track
// through the default instrument; builders wire their tracks with ops::wireSeries).
Track makeTrack(TrackId id, TrackKind kind, std::string name);

// ---- Timeline (SPEC §9.2) --------------------------------------------------------------------------------------

struct Timeline {
  std::vector<TempoPoint> tempo{TempoPoint{}};
  std::vector<TimeSigChange> signatures{TimeSigChange{}};
  TempoMap tempoMap;   // built from `tempo`
  TimeSigMap sigMap;   // built from `signatures`
};
bool operator==(const Timeline& a, const Timeline& b) noexcept;

struct Locator {
  LocatorId id;
  Ticks tick = 0;
  std::string name;
  friend bool operator==(const Locator&, const Locator&) = default;
};

struct LoopRegion {
  Ticks start = 0;
  Ticks end = 4 * kTicksPerWhole;
  bool enabled = false;
  friend constexpr bool operator==(const LoopRegion&, const LoopRegion&) noexcept = default;
};

// ---- Project ---------------------------------------------------------------------------------------------------

using TuningList = std::vector<TuningSlotPtr>;
using LocatorList = std::vector<Locator>;

struct Project {
  std::shared_ptr<const Timeline> timeline;
  std::shared_ptr<const TuningList> tunings;    // never empty (the first is the default for Inherit)
  std::shared_ptr<const LocatorList> locators;  // sorted by (tick, id)
  LoopRegion loop;
  std::vector<TrackPtr> tracks;   // the track tree (Midi + Audio + Group, nested)
  std::vector<TrackPtr> returns;  // Return tracks
  TrackPtr master;
  std::shared_ptr<const WireList> wires;  // T4.7: modulation wires, sorted by id (null: none)
  std::shared_ptr<const SampleList> samples;  // T5.3: the sample table, sorted by id (null: none)
};
// Deep equality (pointer-equal subtrees are not descended into).
bool operator==(const Project& a, const Project& b) noexcept;

using Snapshot = std::shared_ptr<const Project>;

// 120 BPM, 4/4, one main tuning "12-EDO", a master track wired from its input to its mixer (TX.all-free; `wireMaster`
// false: unwired, for builders that reproduce an older file's ids and wire every track at the end with
// ops::wireSeries, as the schema 16 → 17 migration does), no other tracks. Ids are drawn from `ids`.
Snapshot makeDefaultProject(IdSource& ids, bool wireMaster = true);

// Largest raw id used by any entity (for IdSource::reserveThrough after loading). Visits every note.
uint32_t maxEntityId(const Project& p);

// ---- Lookups ---------------------------------------------------------------------------------------------------

// Groups may nest up to this depth (the top level counts as 1). Bounds TrackPath to a fixed array.
inline constexpr uint32_t kMaxTrackDepth = 32;

enum class TrackList : uint8_t { Main, Returns, Master };
// Position of a track: which list, then the child index at each level.
struct TrackPath {
  TrackList list = TrackList::Main;
  uint32_t depth = 0;
  std::array<uint32_t, kMaxTrackDepth> index{};
  TrackPath parent() const noexcept {
    TrackPath p = *this;
    p.depth = depth > 0 ? depth - 1 : 0;
    return p;
  }
};

std::optional<TrackPath> findTrack(const Project& p, TrackId id) noexcept;
const Track& trackAt(const Project& p, const TrackPath& path) noexcept;
const Track* findTrackPtr(const Project& p, TrackId id) noexcept;
// The group containing the track (nullptr at the top level or for returns/master).
const Track* parentOf(const Project& p, const TrackPath& path) noexcept;

const TuningSlot* findTuning(const Project& p, TuningId id) noexcept;
// Index of the clip in track.clips, or −1.
int32_t findClipIndex(const Track& t, ClipId id) noexcept;
// The track holding device `id` (nullptr: none) and the device.
struct DeviceLoc {
  const Track* track = nullptr;
  const Device* device = nullptr;
  uint32_t index = 0;  // in track->devices
  // The list holding the device.
  const std::vector<DevicePtr>& list() const noexcept;
};
DeviceLoc findDevice(const Project& p, DeviceId id) noexcept;
// Visits every device of a list: f(const Device&). (GU.9: no device holds others; a group's members are in its list.)
template <class F>
void forEachDeviceIn(const std::vector<DevicePtr>& list, F&& f) {
  for (const DevicePtr& d : list) f(*d);
}
// T4.7: the project's wires (an empty list when it has none) and one by id (nullptr: none).
const WireList& wiresOf(const Project& p) noexcept;
const Wire* findWire(const Project& p, WireId id) noexcept;
// T4.18: the track an automation lane of a device-kind param lives on (isDeviceParamKind): the track holding the
// device (DeviceParam) or the wire's target device (WireAmount: its amount is a param of
// the target's track patch); nullptr when that entity is gone. Lanes follow it: a command that moves or removes a
// device or wire moves the lanes to the new home track or removes them in the same step (apply.cpp).
const Track* laneHome(const Project& p, const ParamRef& r) noexcept;
// T5.3: the project's sample table (an empty list when it has none) and one entry by id (nullptr: none).
const SampleList& samplesOf(const Project& p) noexcept;
const SampleRef* findSample(const Project& p, SampleId id) noexcept;

// The tuning a track (and optionally one of its clips) plays in, following Inherit up the group chain. nullptr if a
// Main reference is dangling (commands never produce that).
const TuningDef* effectiveTuning(const Project& p, const TrackPath& path, const NoteClip* clip = nullptr) noexcept;

// Visits every track (main tree depth-first, then returns, then master): f(const Track&, const Track* parent).
template <class F>
void forEachTrack(const Project& p, F&& f) {
  struct Walk {
    F& f;
    void operator()(const std::vector<TrackPtr>& list, const Track* parent) {
      for (const TrackPtr& t : list) {
        f(*t, parent);
        (*this)(t->children, t.get());
      }
    }
  } walk{f};
  walk(p.tracks, nullptr);
  walk(p.returns, nullptr);
  if (p.master) f(*p.master, static_cast<const Track*>(nullptr));
}

// Visits every device of the project: f(const Device&, const Track&).
template <class F>
void forEachDevice(const Project& p, F&& f) {
  forEachTrack(p, [&](const Track& t, const Track*) {
    forEachDeviceIn(t.devices, [&](const Device& d) { f(d, t); });
  });
}

}  // namespace pq::doc
