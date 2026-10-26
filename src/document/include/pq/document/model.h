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

// A track's clips, sorted by (start, id). The id is kept next to the pointer so finding a clip by id scans a
// contiguous array instead of dereferencing every clip.
struct ClipSlot {
  ClipId id;
  ClipPtr clip;
};

// ---- Tracks and mixer (SPEC §9.3) ------------------------------------------------------------------------------

enum class TrackKind : uint8_t { Midi, Group, Return, Master, Audio /* reserved for M6: rejected by commands */ };

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

// One device instance in a track's chain: a call of `patch` (a patch of the device library) with the values the user
// set. Ports are the patch's signature and are not stored; params / settings not listed play the patch's defaults.
struct Device {
  DeviceId id;
  std::string patch;
  DeviceKind kind = DeviceKind::AudioEffect;
  bool active = true;                // deactivated: left out of the graph (structural, zero CPU)
  std::vector<DeviceValue> params;   // sorted by name, unique
  std::vector<DeviceValue> settings; // sorted by name, unique
  const DeviceValue* param(std::string_view name) const noexcept;    // nullptr: the patch's default
  const DeviceValue* setting(std::string_view name) const noexcept;  // nullptr: the patch's default
  friend bool operator==(const Device&, const Device&) = default;
};
using DevicePtr = std::shared_ptr<const Device>;

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
  TuningAssignment tuning;  // used by Midi tracks (and inherited through groups)
  Mixer mixer;
  bool active = true;  // deactivate (`0`): structural (Q15)
  std::vector<TrackPtr> children;  // Group only
  std::vector<ClipSlot> clips;     // Midi only, sorted by (start, id)
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
  std::vector<TrackPtr> tracks;   // the track tree (Midi + Group, nested)
  std::vector<TrackPtr> returns;  // Return tracks
  TrackPtr master;
  std::shared_ptr<const WireList> wires;  // T4.7: modulation wires, sorted by id (null: none)
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
// The track holding device `id` (nullptr: none) and the device.
struct DeviceLoc {
  const Track* track = nullptr;
  const Device* device = nullptr;
  uint32_t index = 0;  // in track->devices
};
DeviceLoc findDevice(const Project& p, DeviceId id) noexcept;
// T4.7: the project's wires (an empty list when it has none) and one by id (nullptr: none).
const WireList& wiresOf(const Project& p) noexcept;
const Wire* findWire(const Project& p, WireId id) noexcept;

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

}  // namespace pq::doc
