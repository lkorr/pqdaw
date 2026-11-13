#pragma once

// Commands: every edit of the document is a value of `Command` (a variant of plain structs) applied to a snapshot
// to produce the next snapshot (SPEC §2, T3.4). Commands are the primitives; T3.6 builds higher-level edits
// (split, consolidate, insert time, quantize…) as Batches of these.
//
// Contract of apply():
// - Pure: the result depends only on (snapshot, command). New entities carry ids pre-allocated from the
//   DocumentStore's IdSource, so commands never allocate ids themselves.
// - A command that changes nothing returns the *same* snapshot pointer and an empty ChangeSet (the store then
//   records no history entry).
// - On error the input snapshot is untouched (nothing is shared mutably), and the ChangeSet is unspecified.
// - Untouched subtrees of the result are pointer-equal to the input's.
// - Notes are addressed by NoteKey {start, id}: an exact key is a binary search; a stale key still works (falls
//   back to a search by id), so keys are a lookup hint, never a correctness hazard.
// - Clips are addressed by ClipRef {track, clip}: the track narrows the search to one track's clips.
//
// The ChangeSet classification of each kind is documented at the struct (see change_set.h for the classes).

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/change_set.h"
#include "pq/document/model.h"

namespace pq::doc {

inline constexpr uint32_t kAppend = 0xFFFFFFFFu;  // insertion index meaning "at the end"

struct ClipRef {
  TrackId track;
  ClipId clip;
};

// ---- Tracks ----------------------------------------------------------------------------------------------------
// Structural. Inserts `track` (with its whole subtree, e.g. a pasted group) under `parent` (a Group; invalid =
// top level) at `index`. Return tracks go to the returns list (parent must be invalid). Master is rejected. T5.3:
// audio tracks hold audio clips (their Off lengths are re-derived from the project's tempo map on insertion).
struct AddTrack { TrackPtr track; TrackId parent; uint32_t index = kAppend; };
// Structural. Removes a track and its subtree. Sends to a removed return, lanes automating them and outputs routed
// to a removed group are cleaned up (those tracks are listed as touched).
struct RemoveTrack { TrackId track; };
// Structural. Moves a track (with its subtree) under `parent` (invalid = top level) at `index` (in the destination
// list after removal). Returns can only be reordered among returns.
struct MoveTrack { TrackId track; TrackId parent; uint32_t index = kAppend; };
struct RenameTrack { TrackId track; std::string name; };          // Meta
struct SetTrackColour { TrackId track; Colour colour; };          // Meta
struct SetTrackVolume { TrackId track; float db = 0.0f; };        // Param (TrackVolume)
struct SetTrackPan { TrackId track; float pan = 0.0f; };          // Param (TrackPan)
// Structural (TX.pan-laws: the law is the track patch's pan math; a recompile of that track only).
struct SetTrackPanMode { TrackId track; PanMode mode = PanMode::Balance; };
struct SetTrackMute { TrackId track; bool mute = false; };        // Param (TrackMute), Q15
struct SetTrackSolo { TrackId track; bool solo = false; };        // Param (TrackSolo): gains via effectiveAudible
// T6.1: Param (TrackArm; the monitor gate follows it). MIDI and audio tracks.
struct SetTrackArm { TrackId track; bool arm = false; };
struct SetSendLevel { TrackId track; TrackId target; float db = kMinVolumeDb; };  // Param (TrackSend)
struct SetSendPreFader { TrackId track; TrackId target; bool preFader = false; }; // Structural (tap point moves)
struct SetTrackOutput { TrackId track; Output output; };          // Structural (routing)
struct SetTrackActive { TrackId track; bool active = true; };     // Structural (Q15: deactivate = DCE + recompile)
// FZ.1 (model.h "Frozen tracks"): Structural. A MIDI or audio track; on a group every MIDI and audio track inside it (at
// any depth; a group itself is never frozen). Refused on a return or the master.
struct SetTrackFrozen { TrackId track; bool frozen = true; };
// ---- Recording inputs (T6.1; model.h "Recording inputs", "Note input") -----------------------------------------
// Param (TrackMonitor): the monitor gate's value. MIDI and audio tracks.
struct SetTrackMonitor { TrackId track; MonitorMode mode = MonitorMode::Auto; };
// An audio track's input. Structural (the track's patch reads `track.in` only while it has one; the channels are what
// the engine binds, LoweredProject::audioInputs).
struct SetTrackAudioInput { TrackId track; AudioInput input; };
// A MIDI track's note input (port / channel). Meta (the app's MIDI router reads it; no engine work).
struct SetTrackMidiInput { TrackId track; MidiInput input; };
// Track Delay in ms (Mixer::trackDelayMs). Meta: the link plans compensation again without a compile (T6.6). Not on
// the master.
struct SetTrackDelay { TrackId track; float ms = 0.0f; };
// How incoming keys become notes (Track::noteInput). Meta (the input map is rebuilt off-thread). MIDI tracks.
struct SetNoteInput { TrackId track; NoteInput input; };
// Meta. The punch region (Project::punch) and the record settings (Project::record; T6.6: the link sets the engine's
// metronome from them, Record reads the count-in).
struct SetPunch { PunchRegion punch; };
struct SetRecordSettings { RecordSettings settings; };

// ---- Take lanes (T6.1; model.h "Take lanes") -----------------------------------------------------------------------
// Adds take lane `lane` (its id and its clips' ids pre-allocated and unused; clips of the track's kind, valid, sorted
// by (start, id); Off audio clips are synced to the tempo map) at `index` of the track's takes (kAppend: last). Meta:
// a take that is not auditioned is never lowered.
struct AddTakeLane { TrackId track; TakeLanePtr lane; uint32_t index = kAppend; };
// Removes a take lane (with its clips). Meta; Sequence too when it was the audition take (the main lane plays again).
struct RemoveTakeLane { TrackId track; LaneId lane; };
struct RenameTakeLane { TrackId track; LaneId lane; std::string name; };  // Meta
// Sequence: which lane the track plays (invalid: the main lane). A take's clips replace the main lane's in the
// sequences (playedClips); nothing compiles.
struct SetAuditionTake { TrackId track; LaneId lane; };
// Sequence: take `lane`'s clips become the main lane and the main lane's clips take its place in the take lane (the
// lane keeps its id, name and position; the audition take is reset to the main lane).
struct PromoteTake { TrackId track; LaneId lane; };

// Structural: changes which tuning node feeds the track's voices.
struct SetTrackTuning { TrackId track; TuningAssignment tuning; };
// Structural. Wraps `tracks` (main tree, any parents, none an ancestor of another) in the empty Group `group`,
// inserted where the first of them (in tree order) was; the tracks keep their tree order inside it.
struct GroupTracks { std::vector<TrackId> tracks; TrackPtr group; };
// Structural. Replaces a group by its children (in place) and removes it.
struct UngroupTracks { TrackId group; };

// ---- Clips -----------------------------------------------------------------------------------------------------
// T5.3: exactly one of `clip` (a note clip: the track must be Midi) and `audio` (an audio clip: the track must be Audio,
// its sample in the table; an Off clip's length is re-derived from the tempo map) is set. Audio commands also drop the
// crossfades their edit leaves without an adjacent left neighbour (model.h AudioClip::crossfadeIn).
// T6.1: `take` = the slot's provenance (ClipSlot::take; display only).
struct AddClip { TrackId track; ClipPtr clip; AudioClipPtr audio; LaneId take; };  // Sequence
struct RemoveClip { ClipRef clip; };                              // Sequence (either kind)
// Sequence. Moves to `start` on `toTrack` (may be the same track; another track of the same kind). Either kind.
struct MoveClip { ClipRef clip; TrackId toTrack; Ticks start = 0; };
// Sequence. Sets arrangement start/length and the content start marker (T3.6 resize keeps notes in place). Note clips.
struct SetClipBounds { ClipRef clip; Ticks start = 0; Ticks length = 1; Ticks contentStart = 0; };
struct SetClipLoop { ClipRef clip; ClipLoop loop; };              // Sequence (note clips)
// Structural for a note clip (see SetTrackTuning); T5.3: Sequence for an audio clip (its Steps transpose's tuning).
struct SetClipTuning { ClipRef clip; TuningAssignment tuning; };
struct RenameClip { ClipRef clip; std::string name; };            // Meta (either kind)
struct SetClipColour { ClipRef clip; Colour colour; };            // Meta (either kind)
struct SetClipSignature { ClipRef clip; TimeSignature signature; };  // Meta (display only; note clips)
struct SetClipActive { ClipRef clip; bool active = true; };       // Sequence (either kind)
// T5.3: Sequence. Replaces an audio clip with `audio` (same id; any other field may change: bounds, gain, transpose,
// detune, reverse, warp, source BPM, fades, crossfade, sample). The audio-specific edits (edit_ops.h) are built from it;
// never a compile (runtime data of the clip player).
struct SetAudioClip { ClipRef clip; AudioClipPtr audio; };

// ---- Samples (T5.3, T5.8) ----------------------------------------------------------------------------------------
// Meta. Adds an entry to the sample table (id pre-allocated, unique; samples.h). Dropping a file adds its sample and
// its clip in one Batch.
struct AddSample { SampleRef sample; };
// Meta. Removes an entry no clip uses (refused while one does).
struct RemoveSample { SampleId sample; };
// Replaces an entry (matched by id): Meta; Sequence too when its rate changed (the Off clips playing it are re-derived).
// Path updates after Collect All / relink are not undo steps: DocumentStore::relocateSamples.
struct SetSample { SampleRef sample; };
// Meta (not audible). The steps / lock the clip's virtual fundamental is derived from (T3.6-fix1); written by the
// Up/Down and drag builders (ops::transposeNotes / moveNotes) and the lock builders (ops::lockFundamental).
struct SetClipFundamental { ClipRef clip; ClipFundamental fundamental; };

// ---- Notes (all Sequence) --------------------------------------------------------------------------------------
struct AddNotes { ClipRef clip; std::vector<Note> notes; };
struct RemoveNotes { ClipRef clip; std::vector<NoteKey> notes; };
// Replaces whole notes (matched by id; the new value may change any field but the id). Drags of many notes.
struct NoteUpdate { NoteKey key; Note note; };
struct UpdateNotes { ClipRef clip; std::vector<NoteUpdate> updates; };
struct MoveNote { ClipRef clip; NoteKey note; Ticks start = 0; tuning::PitchId pitch; };
struct ResizeNote { ClipRef clip; NoteKey note; Ticks length = 1; };
enum class NoteProperty : uint8_t { Velocity, ReleaseVelocity, Chance, VelocityRange, Enabled /* value ≠ 0 */ };
struct SetNoteProperty { ClipRef clip; std::vector<NoteKey> notes; NoteProperty property = NoteProperty::Velocity; float value = 0.0f; };

// ---- Automation ------------------------------------------------------------------------------------------------
// Structural (the parameter becomes an audio-rate automation signal, T3.16). At most one lane per ParamRef in the
// project. Track params may only be automated on their own track; tuning params on any track. T4.18: device params
// (with AutomationLane::paramName) and wire amounts only on the track holding the device (laneHome). After every command a lane of those
// kinds follows its device: removed with it, moved with it to another track (apply's final pass, one undo step).
struct AddAutomationLane { TrackId track; LanePtr lane; };
struct RemoveAutomationLane { TrackId track; LaneId lane; };      // Structural
struct AddBreakpoints { TrackId track; LaneId lane; std::vector<Breakpoint> points; };     // Automation
struct RemoveBreakpoints { TrackId track; LaneId lane; std::vector<BreakpointId> points; };  // Automation
struct UpdateBreakpoints { TrackId track; LaneId lane; std::vector<Breakpoint> points; };  // Automation (by id)

// ---- Timeline --------------------------------------------------------------------------------------------------
struct SetTempoMap { std::vector<TempoPoint> points; };            // Timeline
struct SetTimeSignatures { std::vector<TimeSigChange> changes; };  // Timeline
struct SetLoop { LoopRegion loop; };                               // Timeline (transport)
struct AddLocator { Locator locator; };                            // Meta
struct RemoveLocator { LocatorId locator; };                       // Meta
struct UpdateLocator { Locator locator; };                         // Meta (move / rename, matched by id)

// ---- Main tunings ----------------------------------------------------------------------------------------------
struct AddTuning { TuningSlot slot; };                             // Structural (a new tuning signal node)
// Structural. Rejected while a track or clip uses the tuning, while it is the project's default, or if it is the last
// one; lanes automating it are removed with it.
struct RemoveTuning { TuningId tuning; };
struct RenameTuning { TuningId tuning; std::string name; };        // Meta
struct SetTuningColour { TuningId tuning; Colour colour; };        // Meta
// Classified by what changed: only continuous parameters (reference, EDO equave, MOS generator/period/stretch,
// MOS region hint) → Param with their ParamRefs (T3.17 must not recompile while a generator is dragged, SPEC §3.3);
// anything discrete (kind, EDO size, MOS n/periods/rotation/MODMOS/generator sequence, Scale pitches or key
// mapping) → Structural. A Scale's description alone is Meta. Lanes that no longer apply to the new kind are
// removed.
struct SetTuningDef { TuningId tuning; TuningDef def; };
// Param. Sets one continuous parameter (the ParamKind must be a tuning param that applies to the tuning's kind).
struct SetTuningParam { TuningId tuning; ParamKind param = ParamKind::TuningReference; double value = 0.0; };
// TU.3. Structural: the project's default tuning (Project::defaultTuning), what a top-level Inherit resolves to.
struct SetDefaultTuning { TuningId tuning; };

// ---- Devices (T4.1; QUESTIONS Q129) -----------------------------------------------------------------------
// TX.all-free: a track's devices are BLOCKS (a set: Track::devices) wired by cables; these commands add, remove and move
// blocks and never wire anything (ops::insertOnPath / ops::dissolveDevices rewire the main path; main_path.h).
// Instruments go on MIDI tracks only. Device ids are project-wide unique (pre-allocated from the IdSource).
// Structural. Adds `device` to `track`'s blocks at list position `index` (kAppend: at the end; the position only orders
// the set: model.h Track::devices).
struct AddDevice { TrackId track; DevicePtr device; uint32_t index = kAppend; };
// Structural. Removes a device (its values and its wires go with it; undo restores them).
struct RemoveDevice { DeviceId device; };
// Structural. Moves a device to `toTrack` (invalid: its own track) at `index` (in the destination list after removal;
// kAppend: the end; its wires stay: they name the device). Within one track only that track's unit recompiles (every device keeps its state: its NodeId is
// its id); between tracks both do, and the device starts fresh there (state lives per track).
struct MoveDevice { DeviceId device; TrackId toTrack; uint32_t index = kAppend; };
struct SetDeviceActive { DeviceId device; bool active = true; };   // Structural (left out of the graph when off)
// Param (ParamRef::device(device, param)): a runtime value, one param write, never a compile. `value` in the
// param's normalised unit; nullopt = back to the patch's default.
struct SetDeviceParam { DeviceId device; std::string param; std::optional<double> value; };
// Structural: a `real const` input (a literal at the call site; the unit recompiles). nullopt = the default.
struct SetDeviceSetting { DeviceId device; std::string setting; std::optional<double> value; };
// T4.17: Meta. The device's own name ("" = the patch's default name, Device::name).
struct RenameDevice { DeviceId device; std::string name; };
// TX.groups: puts a block of a track into group `group` (a group device of the same track), or takes it out (invalid); a
// member is never on the main path (main_path.h). GU.2: Structural (membership decides whether a block runs per note:
// an instrument group's members at any depth); groups nest (a group may go into another group; no cycles).
struct SetDeviceGroup { DeviceId device; DeviceId group; };
// GU.1: a control's curve (model.h ControlCurve; nullopt: the declared one, the entry removed). The knob position stays:
// the sound follows the curve. Structural when `structural` (the curve's class changed — segment count, shapes, flags —
// which the specialised `curve` atom and the Add wire law follow; ops::setParamCurve decides it against the declared
// curve, which the library-free document does not know), else Param (the value of an undriven control, the slot data
// of a driven one: no compile). The param is not checked against the patch (ops::setParamCurve checks it). Never on a
// rack or a group.
struct SetParamCurve { DeviceId device; std::string param; std::optional<dsp::Curve> curve; bool structural = true; };
// GU.3 (pq/document/panel.h): group `group`'s panel, replaced whole (null or empty: none — the group is drawn as its
// mini circuit). Meta: a panel is never lowered. Refused on a block that is not a group, for a panel that fails
// checkPanel, or one binding anything outside the group (bindingsProblem). An equal panel is a no-op.
struct SetGroupPanel { DeviceId group; PanelPtr panel; };

// ---- Modulation wires (T4.7; model.h "Modulation wires") ------------------------------------------------------
// Structural. Adds `wire` (its id pre-allocated). Both ends must exist (TX.all-free: a track's source with kMainPort on
// any track; a mixer only from that track's own blocks or source); a Travel wire's amount must be in [−1, 1], an
// Add wire's is 1 (TX.patching); a wire with the same source (device, output) and target (device, param or input) as an
// existing one is rejected. The ports are not
// checked against the device patches (the document is library-free): a wire naming a port the patch does not have is
// left out by the lowering.
struct AddWire { Wire wire; };
struct RemoveWire { WireId wire; };                                // Structural
// Param (ParamRef::wireAmount): a runtime value, one param write, never a compile. A Travel wire only (an Add wire has no
// amount: refused).
struct SetWireAmount { WireId wire; double amount = kDefaultWireAmount; };
struct SetWireReducer { WireId wire; WireReducer reducer = WireReducer::Sum; };  // Structural
// IG.1-2 (model.h Wire::single): a cable per note (`single` false) or single-signal, reduced by `reducer` — one command,
// one undo step. Structural.
struct SetWireSignal { WireId wire; bool single = false; WireReducer reducer = WireReducer::Sum; };
// TL.1 (model.h Wire::tap): where a source wire's cable leaves its track's lane (band-local world x; none: the view's
// default). A source wire only; Meta (views only, never a compile).
struct SetWireTap { WireId wire; std::optional<float> x; };

// ---- Meta modules (TMM.3; model.h "Meta modules") -----------------------------------------------------------------
// An instance is addressed by (device, path); "" is a chain-level meta device itself (Device::patch "meta:<type>"). The
// type must be registered (pq/meta/registry.h), the instance at the type's version, names the type's, values finite,
// data valid for the type. Classes follow what the lowering reads: params are runtime values, settings change the
// expansion, data never reaches it.
// Adds the state of a nested instance at `path` (none there yet; a chain device's "" entry only on a meta device of that
// type, without params / settings). Structural when it sets a setting; else Param (the params it sets, as MetaParams)
// and MetaData (when it has data): a call's default state plays already, so adding it alone compiles nothing.
struct AddMetaInstance { DeviceId device; std::string path; MetaInstance instance; };
// The inverse (the instance plays its default state again): classified as AddMetaInstance of what it removes.
struct RemoveMetaInstance { DeviceId device; std::string path; };
// Structural. Moves the state at `from` and every entry below it ("from.…") to `to` (the call was renamed: the state
// key follows the name). `to` and the entries below it must be free; neither may be "".
struct RenameMetaInstance { DeviceId device; std::string from; std::string to; };
// Param. A param of the instance (nullopt: back to the type's default). Path "": the chain device's own param (a
// SetDeviceParam: ParamRef::device); else ParamRef::metaParam. The nested instance must exist (ops::setMetaParam adds it).
struct SetMetaParam { DeviceId device; std::string path; std::string param; std::optional<double> value; };
// Structural. A setting of the instance (path "": the chain device's own setting).
struct SetMetaSetting { DeviceId device; std::string path; std::string setting; std::optional<double> value; };
// MetaData (never a compile; ChangeSet::metaData lists the instance). The instance must exist; equal data is a no-op.
struct SetMetaData { DeviceId device; std::string path; meta::DataPtr data; };

// ---- Compound --------------------------------------------------------------------------------------------------
struct Command;
// Several commands as one undo step, applied in order; fails as a whole if any fails. ChangeSet = union.
struct Batch {
  std::string label;
  std::vector<Command> commands;
};

using CommandVariant = std::variant<
    AddTrack, RemoveTrack, MoveTrack, RenameTrack, SetTrackColour, SetTrackVolume, SetTrackPan, SetTrackPanMode,
    SetTrackMute, SetTrackSolo, SetTrackArm, SetSendLevel, SetSendPreFader, SetTrackOutput, SetTrackActive, SetTrackTuning,
    GroupTracks, UngroupTracks, SetTrackFrozen,
    SetTrackMonitor, SetTrackAudioInput, SetTrackMidiInput, SetTrackDelay, SetNoteInput, SetPunch, SetRecordSettings,
    AddTakeLane, RemoveTakeLane, RenameTakeLane, SetAuditionTake, PromoteTake,
    AddClip, RemoveClip, MoveClip, SetClipBounds, SetClipLoop, SetClipTuning, RenameClip, SetClipColour,
    SetClipSignature, SetClipActive, SetClipFundamental, SetAudioClip, AddSample, RemoveSample, SetSample,
    AddNotes, RemoveNotes, UpdateNotes, MoveNote, ResizeNote, SetNoteProperty,
    AddAutomationLane, RemoveAutomationLane, AddBreakpoints, RemoveBreakpoints, UpdateBreakpoints,
    SetTempoMap, SetTimeSignatures, SetLoop, AddLocator, RemoveLocator, UpdateLocator,
    AddTuning, RemoveTuning, RenameTuning, SetTuningColour, SetTuningDef, SetTuningParam, SetDefaultTuning,
    AddDevice, RemoveDevice, MoveDevice, SetDeviceActive, SetDeviceParam, SetDeviceSetting, RenameDevice,
    SetDeviceGroup, SetParamCurve, SetGroupPanel, AddWire, RemoveWire, SetWireAmount, SetWireReducer, SetWireSignal, SetWireTap,
    AddMetaInstance, RemoveMetaInstance, RenameMetaInstance, SetMetaParam, SetMetaSetting, SetMetaData,
    Batch>;

struct Command {
  CommandVariant v;

  Command() = default;
  template <class T>
    requires std::is_constructible_v<CommandVariant, T&&> && (!std::is_same_v<std::remove_cvref_t<T>, Command>)
  Command(T&& c) : v(std::forward<T>(c)) {}  // NOLINT: implicit by design (dispatch(MoveNote{…}))
};

// Human-readable label for the undo panel ("Move Note", "Set Volume", a Batch's own label).
std::string_view label(const Command& c) noexcept;

// Applies `c` to `s`. On success returns the new snapshot (or `s` itself for a no-op) and fills `changes`
// (merged into whatever it already holds; normalized). FZ.1: a command that edits a frozen track's frozen content
// (model.h "Frozen tracks") fails with kFrozenRefusal (ErrorCode::InvalidArgument), the whole command.
Result<Snapshot> apply(const Snapshot& s, const Command& c, ChangeSet& changes);

// FZ.1 (docs/plans/RB.md D4): the one rule apply() runs after every command that changed something — the first track
// (forEachTrack order) frozen in both `before` and `after` whose frozen content differs (sameFrozenContent, after
// re-syncing its warp-Off clips to `after`'s tempo map, and its own wires: ownedByFrozen); invalid: none. O(tracks)
// when no track is frozen.
inline constexpr std::string_view kFrozenRefusal = "Frozen: unfreeze to edit";
TrackId frozenConflict(const Project& before, const Project& after);

}  // namespace pq::doc
