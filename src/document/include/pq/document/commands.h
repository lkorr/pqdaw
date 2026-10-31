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
struct SetTrackMute { TrackId track; bool mute = false; };        // Param (TrackMute), Q15
struct SetTrackSolo { TrackId track; bool solo = false; };        // Param (TrackSolo): gains via effectiveAudible
struct SetTrackArm { TrackId track; bool arm = false; };          // Meta until recording/monitoring exist (M5)
struct SetSendLevel { TrackId track; TrackId target; float db = kMinVolumeDb; };  // Param (TrackSend)
struct SetSendPreFader { TrackId track; TrackId target; bool preFader = false; }; // Structural (tap point moves)
struct SetTrackOutput { TrackId track; Output output; };          // Structural (routing)
struct SetTrackActive { TrackId track; bool active = true; };     // Structural (Q15: deactivate = DCE + recompile)
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
struct AddClip { TrackId track; ClipPtr clip; AudioClipPtr audio; };  // Sequence
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
// (with AutomationLane::paramName), rack macros / selector, chain volume / pan (ParamRef::chain(c, kChainVolume /
// kChainPan)) and wire amounts only on the track holding the device (laneHome). After every command a lane of those
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
// Structural. Rejected while a track or clip uses the tuning, or if it is the last one; lanes automating it are
// removed with it.
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

// ---- Devices (T4.1; QUESTIONS Q129) -----------------------------------------------------------------------
// The chain rule: a MIDI track holds at most one instrument, at index 0, then audio effects; groups, returns and the
// master hold audio effects only. Device ids are project-wide unique (pre-allocated from the IdSource).
// Structural. Inserts `device` into `track`'s chain at `index` (kAppend: at the end). T4.8: with `chain` valid, into that
// rack chain instead (`track` is then ignored); a rack device brings its chains, mappings and inner devices (all ids
// new).
struct AddDevice { TrackId track; DevicePtr device; uint32_t index = kAppend; ChainId chain; };
// Structural. Removes a device (its values and its wires go with it; undo restores them). T4.8: a rack goes with
// everything inside it (their wires too); mappings of outer racks onto the removed devices go with them.
struct RemoveDevice { DeviceId device; };
// Structural. Moves a device to `toTrack` (invalid: its own track) at `index` (in the destination chain after removal;
// kAppend: the end). Within one track only that track's unit recompiles (every device keeps its state: its NodeId is
// its id); between tracks both do, and the device starts fresh there (state lives per track). T4.8: with `toChain`
// valid, into that rack chain (`toTrack` ignored; a rack cannot go into itself); mappings of racks the device leaves
// are removed with it (a macro maps only devices inside its rack).
struct MoveDevice { DeviceId device; TrackId toTrack; uint32_t index = kAppend; ChainId toChain; };
struct SetDeviceActive { DeviceId device; bool active = true; };   // Structural (left out of the graph when off)
// Param (ParamRef::device(device, param)): a runtime value, one param write, never a compile. `value` in the
// param's normalised unit; nullopt = back to the patch's default.
struct SetDeviceParam { DeviceId device; std::string param; std::optional<double> value; };
// Structural: a `real const` input (a literal at the call site; the unit recompiles). nullopt = the default.
struct SetDeviceSetting { DeviceId device; std::string setting; std::optional<double> value; };
// T4.4-fix1: Structural. The device's own patch text (a user device: the circuit editor's edit of it) or null (back to
// the library's stock patch). The id, values and wires stay: state keys follow the device id, so only that track's
// unit recompiles and every state the edit kept carries over. An equal text is a no-op.
struct SetDevicePatch { DeviceId device; UserPatchPtr source; };
// T4.17: Meta. The device's own name ("" = the patch's default name, Device::name).
struct RenameDevice { DeviceId device; std::string name; };

// ---- Modulation wires (T4.7; model.h "Modulation wires") ------------------------------------------------------
// Structural. Adds `wire` (its id pre-allocated). Both devices must exist; the amount must be in [−1, 1]; a wire with
// the same source (device, export) and target (device, param) as an existing one is rejected. The ports are not
// checked against the device patches (the document is library-free): a wire naming a port the patch does not have is
// left out by the lowering.
struct AddWire { Wire wire; };
struct RemoveWire { WireId wire; };                                // Structural
// Param (ParamRef::wireAmount): a runtime value, one param write, never a compile.
struct SetWireAmount { WireId wire; double amount = kDefaultWireAmount; };
struct SetWireReducer { WireId wire; WireReducer reducer = WireReducer::Sum; };  // Structural

// ---- Racks (T4.8; model.h "Racks") ------------------------------------------------------------------------------
// Structural. Inserts `chain` (with its devices; ids new) into rack `rack` at `index` (kAppend: the end). The chain
// rule of the rack's kind applies to its devices.
struct AddChain { DeviceId rack; ChainPtr chain; uint32_t index = kAppend; };
// Structural. Removes a chain with its devices (their wires and the mappings onto them go too).
struct RemoveChain { ChainId chain; };
// Structural. Moves a chain within its rack (index after removal; kAppend: the end).
struct MoveChain { ChainId chain; uint32_t index = kAppend; };
struct RenameChain { ChainId chain; std::string name; };          // Meta
// Param (ParamRef::chain(chain, 0)): a chain's volume (dB, kMinVolumeDb = −∞) and pan; one param write per value.
struct SetChainMixer { ChainId chain; float volumeDb = 0.0f; float pan = 0.0f; };
struct SetChainActive { ChainId chain; bool active = true; };     // Structural (a muted chain is left out)
struct SetChainSolo { ChainId chain; bool solo = false; };        // Structural
// Param (ParamRef::chain(chain, 1 + kind)) while the zone stays on (or off); Structural when it switches on or off
// (an off zone has no node). The zone must be valid for the rack's key unit.
struct SetChainZone { ChainId chain; ZoneKind kind = ZoneKind::Select; Zone zone; };
// Structural. Adds a mapping (id pre-allocated) to rack `rack`: its target must be a device inside the rack (or one of
// the rack's nested racks' params), the param a name (not checked against the patch: the document is library-free; a
// param the patch lacks is left out by the lowering), not already mapped anywhere; min / max in [0, 1], curve in
// [−1, 1], macro < 16.
struct AddMacroMapping { DeviceId rack; MacroMapping mapping; };
struct RemoveMacroMapping { MappingId mapping; };                  // Structural
// Structural. Replaces a mapping's macro / min / max / curve (matched by id; the target cannot change).
struct SetMacroMapping { MacroMapping mapping; };
// Everything about a rack that is not a chain, a mapping or a param value. Meta, except `keyUnit` / `voiceChains`
// (Structural; a key unit change resets every chain's key zone to off).
struct RackOptions {
  std::array<std::string, kMacroCount> macroNames{};
  uint8_t visibleMacros = 8;
  std::vector<MacroVariation> variations;
  uint16_t randomizeExclude = 0;
  KeyZoneUnit keyUnit = KeyZoneUnit::Hz;
  bool voiceChains = false;
  bool showChains = true;
  bool showMacros = true;
  friend bool operator==(const RackOptions&, const RackOptions&) = default;
};
RackOptions rackOptions(const Rack& r);
struct SetRackOptions { DeviceId rack; RackOptions options; };

// ---- Compound --------------------------------------------------------------------------------------------------
struct Command;
// Several commands as one undo step, applied in order; fails as a whole if any fails. ChangeSet = union.
struct Batch {
  std::string label;
  std::vector<Command> commands;
};

using CommandVariant = std::variant<
    AddTrack, RemoveTrack, MoveTrack, RenameTrack, SetTrackColour, SetTrackVolume, SetTrackPan, SetTrackMute,
    SetTrackSolo, SetTrackArm, SetSendLevel, SetSendPreFader, SetTrackOutput, SetTrackActive, SetTrackTuning,
    GroupTracks, UngroupTracks,
    AddClip, RemoveClip, MoveClip, SetClipBounds, SetClipLoop, SetClipTuning, RenameClip, SetClipColour,
    SetClipSignature, SetClipActive, SetClipFundamental, SetAudioClip, AddSample, RemoveSample, SetSample,
    AddNotes, RemoveNotes, UpdateNotes, MoveNote, ResizeNote, SetNoteProperty,
    AddAutomationLane, RemoveAutomationLane, AddBreakpoints, RemoveBreakpoints, UpdateBreakpoints,
    SetTempoMap, SetTimeSignatures, SetLoop, AddLocator, RemoveLocator, UpdateLocator,
    AddTuning, RemoveTuning, RenameTuning, SetTuningColour, SetTuningDef, SetTuningParam,
    AddDevice, RemoveDevice, MoveDevice, SetDeviceActive, SetDeviceParam, SetDeviceSetting, SetDevicePatch, RenameDevice,
    AddWire, RemoveWire, SetWireAmount, SetWireReducer,
    AddChain, RemoveChain, MoveChain, RenameChain, SetChainMixer, SetChainActive, SetChainSolo, SetChainZone,
    AddMacroMapping, RemoveMacroMapping, SetMacroMapping, SetRackOptions,
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
// (merged into whatever it already holds; normalized).
Result<Snapshot> apply(const Snapshot& s, const Command& c, ChangeSet& changes);

}  // namespace pq::doc
