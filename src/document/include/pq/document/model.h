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
#include "pq/document/note_seq.h"
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
  // the device patch's (DeviceCatalog, devices.h); not automatable yet (device automation arrives with M4's panels).
  DeviceParam,
  // T4.7: the amount of a modulation wire (owner = wire id), in [−1, 1]; a runtime param, not automatable yet.
  WireAmount,
  // T4.8: a rack chain's runtime values (owner = chain id; target = what: 0 mixer (volume, pan), 1 + ZoneKind a zone).
  // The lowering derives the chain's param values from them (lower.h "Racks"); not automatable yet.
  ChainMixer,
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
  static ParamRef chain(ChainId c, uint32_t what = 0) { return {ParamKind::ChainMixer, c.value, what}; }
};
// FNV-1a 32 of a device param's name: the ParamRef target of a device param (QUESTIONS Q129, item 2).
uint32_t deviceParamHash(std::string_view name) noexcept;
bool isTrackParam(ParamKind k) noexcept;
bool isAutomatable(ParamKind k) noexcept;
struct ParamRange {
  double min = 0.0;
  double max = 1.0;
};
ParamRange paramRange(ParamKind k) noexcept;

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

struct Mixer {
  float volumeDb = 0.0f;
  float pan = 0.0f;
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

// What a device is, fixed when it is created from the catalog (devices.h), so the document can enforce the chain
// rule without the device library: a MIDI track holds at most one instrument, at index 0, then audio effects; groups,
// returns and the master hold audio effects only.
enum class DeviceKind : uint8_t { Instrument, AudioEffect };

// A value the user set on a device: a param (runtime, in the param's normalised unit: Hz, seconds, linear gain for a
// dB param, a fraction for %, 0/1 for a bool) or a setting (a `real const` input of the device patch: structural).
struct DeviceValue {
  std::string name;
  double value = 0.0;
  friend bool operator==(const DeviceValue&, const DeviceValue&) = default;
};

// T4.4-fix1 (SPEC §15 2026-10-26; QUESTIONS Q138): a USER DEVICE is a stock device edited in the circuit
// editor. The first edit forks the instance: it gets its own copy of the device's patch file (`text`: the whole .pqp
// with its imports and helper patches), stored in the project; the stock file and every other instance are unchanged.
// The device's patch is the patch of `text` named Device::patch (the stock name it was forked from: "Revert to Stock"
// drops the copy and plays the library's patch again). Imports resolve relative to the stock file's location (the
// stdlib), so `import "../filters/svf.pqp"` keeps working.
// `ids` (optional, never saved) is the circuit editor's parse of `text` at the edit: the device library takes its
// NodeIds from it (ParseOptions::previous), so the project plays the very ids the editor shows (probes, cost) and keeps
// every state the editor kept. Without it (a loaded project) the library parses `text` with the ids of the device's
// previous text, else of the stock file (devices.h DeviceLibrary::resolve).
struct UserPatch {
  std::string text;
  std::shared_ptr<const graph::PatchFile> ids;
  friend bool operator==(const UserPatch& a, const UserPatch& b) noexcept { return a.text == b.text; }
};
using UserPatchPtr = std::shared_ptr<const UserPatch>;
struct Rack;

// One device instance in a track's chain: a call of `patch` (a patch of the device library) with the values the user
// set. Ports are the patch's signature and are not stored; params / settings not listed play the patch's defaults.
// T4.8: a RACK is a Device with `rack` set (patch kAudioEffectRack / kInstrumentRack, kind AudioEffect / Instrument): it
// holds parallel chains of devices instead of calling a library patch; its params are its macros and its chain
// selector ("macro1" … "macro16", "select"), stored in `params` like any device's.
struct Device {
  DeviceId id;
  std::string patch;
  DeviceKind kind = DeviceKind::AudioEffect;
  bool active = true;                // deactivated: left out of the graph (structural, zero CPU)
  std::vector<DeviceValue> params;   // sorted by name, unique
  std::vector<DeviceValue> settings; // sorted by name, unique (racks: none)
  // T4.4-fix1: the instance's own patch text (a user device), or null (the library's stock patch). Shared, so a param
  // edit copies the Device but never the text. Racks: always null (a rack has no patch text).
  UserPatchPtr source;
  std::shared_ptr<const Rack> rack;  // T4.8: non-null for a rack
  // T4.17: the instance's own name (Live: Ctrl+R renames a device), shown in its title bar; "" = the patch's default
  // name (the catalog's / the panel description's title). Meta.
  std::string name;
  bool edited() const noexcept { return source != nullptr; }
  const DeviceValue* param(std::string_view key) const noexcept;    // nullptr: the patch's default
  const DeviceValue* setting(std::string_view key) const noexcept;  // nullptr: the patch's default
  bool isRack() const noexcept { return rack != nullptr; }
};
bool operator==(const Device& a, const Device& b) noexcept;  // deep (racks by content, user texts by text)
using DevicePtr = std::shared_ptr<const Device>;

// ---- Racks (T4.8, SPEC §8.2; QUESTIONS Q129 item 6, Q?-T4.8-a) -------------------------------------------------------
// An Audio Effect Rack (kind AudioEffect) or an Instrument Rack (kind Instrument) holds parallel CHAINS, each a device
// list (the same chain rule as a track: an Instrument Rack's chain may start with one instrument — a device or a nested
// Instrument Rack —, then audio effects; an Audio Effect Rack's chains hold audio effects only) with its own mixer
// (volume, pan, activator, solo) and ZONES. Racks nest (a chain may hold racks). Up to 16 MACROS: params of the rack
// ("macro1" … "macro16", 0 … 127) that drive any number of MAPPINGS (a target param of a device inside the rack, a
// min / max in the target's knob travel and a curve); the CHAIN SELECTOR is the rack param "select" (0 … 127).
// Everything continuous is a runtime value (a macro turn is one param write; a chain's volume / pan or a zone drag are
// param writes too); chains, mappings, mute / solo and a zone switched on or off are structural.
inline constexpr std::string_view kAudioEffectRack = "AudioEffectRack";
inline constexpr std::string_view kInstrumentRack = "InstrumentRack";
inline constexpr uint32_t kMacroCount = 16;
inline constexpr double kMacroMax = 127.0;   // a macro's value range is 0 … 127 (Live's macro scale)
inline constexpr double kSelectMax = 127.0;  // the chain selector's range (and a select zone's)
inline constexpr double kVelocityMax = 127.0;
// The rack params: "macro1" … "macro16" (index 0 … 15) and "select".
std::string macroParamName(uint32_t index);                           // "macro<index + 1>"
std::optional<uint32_t> macroIndex(std::string_view param) noexcept;  // the inverse; nullopt: not a macro
inline constexpr std::string_view kSelectParam = "select";
bool isRackParam(std::string_view param) noexcept;  // a macro or "select"

// A zone: the values where a chain plays. Gain 0 below `lo` and above `hi`, rising linearly from `lo` to `fadeLo`,
// 1 from `fadeLo` to `fadeHi`, falling linearly to `hi` (lo ≤ fadeLo ≤ fadeHi ≤ hi). A zone covering its whole range
// is OFF: it costs nothing (no node in the graph).
enum class ZoneKind : uint8_t { Key, Velocity, Select };
// Key zones of an Instrument Rack: in Hz (a pitch range; the fades are linear in log frequency) or in degrees of the
// track's tuning (converted to Hz from the track's current tuning; they follow a tuning param edit).
enum class KeyZoneUnit : uint8_t { Hz, Degree };
struct Zone {
  double lo = 0.0, fadeLo = 0.0, fadeHi = 0.0, hi = 0.0;
  friend constexpr bool operator==(const Zone&, const Zone&) noexcept = default;
};
struct ZoneRange {
  double min = 0.0, max = 0.0;
};
ZoneRange zoneRange(ZoneKind kind, KeyZoneUnit unit) noexcept;  // Key: Hz 1 … 100000, Degree −1024 … 1024; else 0 … 127
Zone fullZone(ZoneKind kind, KeyZoneUnit unit) noexcept;        // {min, min, max, max}: off
bool zoneOff(const Zone& z, ZoneKind kind, KeyZoneUnit unit) noexcept;
bool zoneValid(const Zone& z, ZoneKind kind, KeyZoneUnit unit) noexcept;  // ordered, finite, inside the range

struct Chain {
  ChainId id;
  std::string name;
  std::vector<DevicePtr> devices;  // left → right
  float volumeDb = 0.0f;           // [kMinVolumeDb, kMaxVolumeDb]; at kMinVolumeDb: −∞
  float pan = 0.0f;                // [−1, 1], balance
  bool active = true;              // the chain activator: off = muted, left out of the graph (structural, zero CPU)
  bool solo = false;               // any soloed chain in a rack: only soloed chains play (structural)
  Zone key, velocity, select;      // makeChain sets the full (off) zones
  const Zone& zone(ZoneKind k) const noexcept {
    return k == ZoneKind::Key ? key : k == ZoneKind::Velocity ? velocity : select;
  }
  Zone& zone(ZoneKind k) noexcept { return k == ZoneKind::Key ? key : k == ZoneKind::Velocity ? velocity : select; }
};
bool operator==(const Chain& a, const Chain& b) noexcept;
using ChainPtr = std::shared_ptr<const Chain>;

// A macro mapping: macro `macro` (0 … 15) drives `param` of `device` (a device inside the rack, at any depth; a nested
// rack's macro or selector too). The target's knob position = min + (max − min) · shape(macro / 127, curve), in the
// target's knob travel (the panel knob's 0 … 1: linear, in dB for gains, logarithmic for @curve(exp); doc::paramTravel)
// with the automation curve law (pq/core/automation_curve.h; curve 0 = straight). min > max inverts. A mapped param
// plays the macro (its own stored value is kept but unused); wires into it still add (Q135's law, the mapping as the
// centre). A param has at most one mapping in the project.
struct MacroMapping {
  MappingId id;
  uint8_t macro = 0;
  DeviceId device;
  std::string param;
  double min = 0.0, max = 1.0;  // [0, 1] knob travel
  double curve = 0.0;           // [−1, 1]
  friend bool operator==(const MacroMapping&, const MacroMapping&) = default;
};

// A stored snapshot of all macro values (Live's macro variations).
struct MacroVariation {
  std::string name;
  std::array<double, kMacroCount> values{};
  friend bool operator==(const MacroVariation&, const MacroVariation&) = default;
};

struct Rack {
  std::vector<ChainPtr> chains;
  std::vector<MacroMapping> mappings;                 // sorted by id
  std::array<std::string, kMacroCount> macroNames{};  // "" = "Macro <k>" (Meta)
  uint8_t visibleMacros = 8;                          // 1 … 16 knobs shown (Meta)
  std::vector<MacroVariation> variations;             // Meta
  uint16_t randomizeExclude = 0;                      // bit k: Rand leaves macro k alone (Meta)
  KeyZoneUnit keyUnit = KeyZoneUnit::Hz;              // Instrument Rack (structural)
  // Instrument Rack: each chain's audio effects run per voice (SPEC §8.2 voicechain: the voices are summed after the
  // chain instead of after its instrument). Structural.
  bool voiceChains = false;
  bool showChains = true;  // the chain list is shown (Meta)
  bool showMacros = true;  // the macro knobs are shown (Meta)
  const Chain* findChain(ChainId id) const noexcept;

  Rack() = default;
  Rack(const Rack&) = default;
  Rack(Rack&&) noexcept = default;
  Rack& operator=(const Rack&) = default;
  Rack& operator=(Rack&&) noexcept = default;
  // M4 review M9: frees nested racks iteratively (a rack inside one being destroyed hands its chains to the outermost
  // destructor instead of destroying them itself), so a nest of any depth is freed without deep recursion.
  ~Rack();
};
bool operator==(const Rack& a, const Rack& b) noexcept;

// A new rack device (no chains: an empty rack passes its input through) / a new chain (full zones).
DevicePtr makeRack(DeviceId id, DeviceKind kind, std::vector<ChainPtr> chains = {});
ChainPtr makeChain(ChainId id, std::string name, std::vector<DevicePtr> devices = {});
std::string macroDisplayName(const Rack& r, uint32_t index);  // the name, else "Macro <index + 1>"
// A rack param's value: the stored one, else 0.
double rackParamValue(const Device& rack, std::string_view param) noexcept;

// The M3 default instrument (QUESTIONS Q15 A): every MIDI track ops::createTrack makes starts with it.
inline constexpr std::string_view kDefaultInstrument = "BasicSynth";
DevicePtr makeDevice(DeviceId id, std::string patch, DeviceKind kind);
DevicePtr makeDefaultInstrument(DeviceId id);  // makeDevice(id, kDefaultInstrument, Instrument)

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
enum class WireReducer : uint8_t { Sum, Last, Highest, Lowest, Loudest };
inline constexpr double kDefaultWireAmount = 0.5;

struct Wire {
  WireId id;
  DeviceId from;      // the source device
  std::string port;   // its export
  DeviceId to;        // the target device (may be `from` itself)
  std::string param;  // its param
  double amount = kDefaultWireAmount;  // [−1, 1]: knob travel per unit of source
  WireReducer reducer = WireReducer::Sum;
  friend bool operator==(const Wire&, const Wire&) = default;
};
using WireList = std::vector<Wire>;  // sorted by id

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
  std::vector<DevicePtr> devices;  // T4.1: the device chain, signal left → right (MIDI: instrument first)
  // Index of the device in `devices`, or −1.
  int32_t deviceIndex(DeviceId device) const noexcept;
};
bool operator==(const Track& a, const Track& b) noexcept;

// A track with an empty device chain (a MIDI track plays nothing until it has an instrument: ops::createTrack adds
// the default instrument; builders add makeDefaultInstrument(ids.device()) themselves).
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

// 120 BPM, 4/4, one main tuning "12-EDO", a master track, no other tracks. Ids are drawn from `ids`.
Snapshot makeDefaultProject(IdSource& ids);

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
// The track holding device `id` (nullptr: none) and the device. T4.8: a device inside a rack is found too (any depth):
// `chain` is then the chain holding it (index into chain->devices), `rack` that chain's rack device; the track is the one
// holding the outermost rack.
struct DeviceLoc {
  const Track* track = nullptr;
  const Device* device = nullptr;
  uint32_t index = 0;            // in the list holding it: track->devices, or chain->devices
  const Chain* chain = nullptr;  // T4.8: the chain holding it (nullptr: the track's chain)
  const Device* rack = nullptr;  // T4.8: the rack holding `chain`
  // The list holding the device.
  const std::vector<DevicePtr>& list() const noexcept;
};
DeviceLoc findDevice(const Project& p, DeviceId id) noexcept;
// T4.8: a chain of any rack in the project: the chain, its rack and the track holding the rack.
struct ChainLoc {
  const Track* track = nullptr;
  const Device* rack = nullptr;
  const Chain* chain = nullptr;
  uint32_t index = 0;  // in rack->rack->chains
};
ChainLoc findChain(const Project& p, ChainId id) noexcept;
// T4.8: the mapping with `id` and its rack (nullptr: none).
struct MappingLoc {
  const Track* track = nullptr;
  const Device* rack = nullptr;
  const MacroMapping* mapping = nullptr;
};
MappingLoc findMapping(const Project& p, MappingId id) noexcept;
// T4.8: the mapping driving `device`.`param` (nullptr: unmapped) and its rack.
MappingLoc mappingOf(const Project& p, DeviceId device, std::string_view param) noexcept;
// T4.8: visits `d` and every device inside it (racks: chains in order, depth-first): f(const Device&). M4 review M9:
// iterative (an explicit stack on the heap), so racks nest as deep as memory allows.
template <class F>
void forEachDeviceIn(const Device& d, F&& f);
// Visits every device of a list (and inside racks), depth-first in chain order (iterative, as above).
template <class F>
void forEachDeviceIn(const std::vector<DevicePtr>& list, F&& f);
// Whether `inner` is `outer` or inside it (a rack).
bool deviceContains(const Device& outer, DeviceId inner) noexcept;
// T4.7: the project's wires (an empty list when it has none) and one by id (nullptr: none).
const WireList& wiresOf(const Project& p) noexcept;
const Wire* findWire(const Project& p, WireId id) noexcept;
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

namespace detail {
// The device lists still to visit (a range each); a rack's chains are pushed in reverse, so chain 0 comes next.
struct DeviceRange {
  const DevicePtr* next;
  const DevicePtr* end;
};
inline void pushChains(std::vector<DeviceRange>& stack, const Device& d) {
  if (!d.rack) return;
  const std::vector<ChainPtr>& chains = d.rack->chains;
  for (size_t i = chains.size(); i-- > 0;) {
    const std::vector<DevicePtr>& list = chains[i]->devices;
    if (!list.empty()) stack.push_back({list.data(), list.data() + list.size()});
  }
}
template <class F>
void walkDevices(std::vector<DeviceRange>& stack, F& f) {
  while (!stack.empty()) {
    DeviceRange& top = stack.back();
    if (top.next == top.end) {
      stack.pop_back();
      continue;
    }
    const Device& d = **top.next++;  // before a push moves `top`
    f(d);
    pushChains(stack, d);
  }
}
}  // namespace detail

template <class F>
void forEachDeviceIn(const std::vector<DevicePtr>& list, F&& f) {
  if (list.empty()) return;
  std::vector<detail::DeviceRange> stack{{list.data(), list.data() + list.size()}};
  detail::walkDevices(stack, f);
}

template <class F>
void forEachDeviceIn(const Device& d, F&& f) {
  f(d);
  if (!d.rack) return;
  std::vector<detail::DeviceRange> stack;
  detail::pushChains(stack, d);
  detail::walkDevices(stack, f);
}

// T4.8: visits every device of the project, nested ones too: f(const Device&, const Track&).
template <class F>
void forEachDevice(const Project& p, F&& f) {
  forEachTrack(p, [&](const Track& t, const Track*) {
    forEachDeviceIn(t.devices, [&](const Device& d) { f(d, t); });
  });
}

}  // namespace pq::doc
