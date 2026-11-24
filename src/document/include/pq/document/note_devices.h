#pragma once

// NOTE DEVICES (AP.2; docs/plans/AP.md "Where it sits"): a block whose main input and main output are NOTES — a note
// stage on a MIDI track's notes link, after the tuning block and before the first block that takes notes (an
// instrument group, an Instrument Rack, a meta module fed the note source). The arpeggiator is the first one.
//
//   the device   Device::patch "note:<type>" (kNoteDevicePrefix) of a registered NoteDeviceType, kind AudioEffect (on a
//                track the kind of a block means nothing: its signals decide). Its params are the type's runtime params
//                (knob POSITIONS, GU.1: automation and drags write them) and its settings the type's structural ones.
//                Only on a MIDI track (checks.cpp: no notes reach any other), never in a group.
//   the cables   AR.1 (wire_types.h notesWireClash): its main input takes only a cable that carries notes (a MIDI
//                track's notes, a Notes-out device's main output) and its main output goes only into a main input
//                that takes notes; the cable edits refuse anything else. A cable that completes a way from the notes
//                through note devices into an input REPLACES the direct cable between those two ends (ops::connect:
//                a track has one note stream — it runs through the stages or around them, not both). Half-wired (or
//                beside a direct cable that is still there) it is off the main path: no stage, and nothing plays
//                through it.
//   the signals  declared by its type (NoteDeviceType::in / out: Notes → Notes), never by its patch name:
//                doc::chainStages gives a device with declared signals `in` → `out` when `in` reaches it, else passes
//                on what reaches it (it changes nothing it does not take). So the instrument after an arpeggiator still
//                takes the notes (the voice contexts, the views' note wires and the lowering all follow chainStages).
//   lowering     a note device is not in the graph: the lowering DISSOLVES every note device of the project first
//                (lower.h "Note devices": its main input's wires joined to its main output's, so the instrument after
//                it reads the note source as before; its other wires are Inactive), and lowerNoteChains gives each
//                MIDI track's chain of active note devices on its notes link, which the playback link publishes per
//                event slot (pq/engine/note_stage.h, Engine::setNoteStages).
//
// The type's params are declared ONCE, in the engine's table (pq/engine/arp.h kArpParamInfo): a new param there is a
// new device param here with no document change. Library-free; not RT code.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/document/model.h"
#include "pq/document/panel.h"

namespace pq::doc {

inline constexpr std::string_view kNoteDevicePrefix = "note:";

// One param of a note device type, in the order of its engine table (index = the engine's param id).
struct NoteParamSpec {
  std::string name;      // the device param / setting name
  std::string label;
  double min = 0.0, max = 1.0, def = 0.0;
  double step = 0.0;     // 0: continuous; 1: integers
  bool isBool = false;
  std::vector<std::string> choices;  // an enum's labels (index = value − min); empty: none
  bool runtime = true;   // a runtime param (else a structural setting)
  std::string unit;      // AP.11: "%" (the value a fraction, shown × 100; the patch text's `%` literals), else ""
  double taper = 0.0;    // AP.11: the knob's exp0 taper k (engine arpTaper; 0: linear)
};

enum class NoteStageKind : uint8_t;  // lower.h: the engine stage a type plays as

struct NoteDeviceType {
  std::string id;         // "arpeggiator" (Device::patch "note:arpeggiator")
  std::string name;       // "Arpeggiator"
  std::string patchName;  // its fixed patch's name ("Arpeggiator": DeviceLibrary::patchOf)
  std::string description;  // one sentence (the node menu's preview)
  ChainSignal in = ChainSignal::Notes, out = ChainSignal::Notes;  // its declared main signals
  std::vector<NoteParamSpec> params;
  // AP.8b (review L7): what the type IS, so nothing else keys on its id: the engine stage it plays as (the playback
  // link and the offline render build that stage) and its device panel (noteDevicePanel; null: the generic one).
  NoteStageKind stage{};
  Panel (*panel)(DeviceId device) = nullptr;
  const NoteParamSpec* param(std::string_view name) const noexcept;
  int32_t paramIndex(std::string_view name) const noexcept;  // −1: none
};

// Every registered type (registration order: the menus' order) and one by id (null: none).
std::span<const NoteDeviceType> noteDeviceTypes();
const NoteDeviceType* findNoteDeviceType(std::string_view id) noexcept;
// The patch of a catalog item "note:<type>" of a registered type.
bool isNoteDevicePatch(std::string_view patch) noexcept;
std::string noteDevicePatch(std::string_view type);  // "note:<type>"
// Whether `d` is a note device (its patch names a registered type), and its type (null: not one).
bool isNoteDevice(const Device& d) noexcept;
const NoteDeviceType* noteDeviceTypeOf(const Device& d) noexcept;
// AR.1: the main signals `d` DECLARES (its type's NoteDeviceType::in / out; None: it declares none — a group, an atom
// block, a meta module: what reaches it decides, chainStages). What a cable may join follows these (wire_types.h
// notesWireClash), never a patch name.
struct MainSignals {
  ChainSignal in = ChainSignal::None, out = ChainSignal::None;
};
MainSignals declaredMainSignals(const Device& d) noexcept;

// The fixed patch text a type's devices play for the views and the edit builders (`in notes`, its params with their
// ranges, defaults, @steps, @label, @choices, its settings as `real const` inputs, `out out = notes`). Never lowered.
std::string noteDevicePatchText(const NoteDeviceType& t);

// AP.6: the DEVICE PANEL of note device `d` — its type's own (the Arpeggiator: docs/plans/AP.md "AP.6"; the views'
// leafPanel draws it 1:1 in the Device view), built from the group-panel elements (pq/document/panel.h) bound to the
// device's own params (Control) and settings (Setting), so every control edits its param as any panel control does:
// automation, cables, undo. Empty: the type has none (the generic panel: one control per param).
// The Arpeggiator's (fit: the shown height is the open sections'):
//   top        Arp (Style, Hold, Seed, Rate: Sync, num / den, tuplet n : m with Straight / Dotted / Triplet shortcuts —
//              free: ms —, Gate, Groove, Offset, Repeats, Retrigger + its rate, Velocity: source, fixed, decay, target,
//              retrigger), Pitch (Unit, Distance, Steps, Shift / Rotate; Ratio p / q only in Ratio, Base in
//              Harmonics or Harmonic Walk (a condition of two sources), Range / Under / walk order only with style
//              Harmonic Walk), the pattern view (AP.10: the whole resolved cycle, pq/document/note_pattern.h).
//   Glide      fold, OPEN by default (AP.10): Into and Out each with its source / target, Time Sync, Offset (with
//              Offset), Time (ms or a note fraction), Bend, Shape; "Offsets in cents"; the bend view (one step large,
//              its handles drag the times / offsets / bends).
//   Pulse      fold: on; with it on its source, ref, cycle (+ bars), Layers + levels, the proportion (with source
//              Proportion).
//   Step Lanes fold: per lane its switch and a step-lane editor (its 32 steps, its length).
// Visibility conditions (Visibility) hide what does not apply; folds are view state (folded by default but Glide).
Panel noteDevicePanel(const Device& d);

// The NOTE RUN of a main path: how many of its first blocks are note devices (the notes link's stages, in order).
uint32_t noteRun(const MainPath& path) noexcept;

// THE DISSOLVE (lower.h "Note devices"; the voice contexts read it too): the project's wires with every note device
// taken out of them — each wire out of a note device's main output starts where the OLDEST wire into its main input
// starts (that wire's source, port, tap; the out-wire's id, target and the rest), repeated through a chain of note
// devices, so the instrument after an arpeggiator reads the note source exactly as without it. A wire into a note
// device's main input is an ALIAS of the joined wire it became (it plays as that one: its status); every other wire
// on a note device (its params, a second wire into its input, its output with nothing into it) is DROPPED (Inactive).
struct NoteDissolve {
  bool any = false;                      // the project has a note device
  std::shared_ptr<const WireList> wires; // the wires as dissolved (null: none left); valid when `any`
  std::vector<std::pair<WireId, WireId>> aliases;  // (wire into a note device, the joined wire it plays as), by first
  std::vector<WireId> dropped;                     // sorted
  // AP.2b: the wires into a note device's runtime (non-bool) param, kept in `wires` (their target is gone): the
  // lowering exports each one's source as a bus the note stage reads one block late (lower.h "Note devices"). Sorted.
  std::vector<WireId> params;
};
NoteDissolve dissolveNoteDevices(const Project& p);
// `p` with its note devices removed from their tracks and its wires `how.wires` (shares every track without one);
// `p` itself when it has none.
Project withoutNoteDevices(const Project& p, const NoteDissolve& how);

// A note device's param value in its units from a knob position (its declared linear curve, rounded to its step) and
// back; the lowering's mapping (scale = max − min, offset = min) is the same line.
double noteParamValue(const NoteParamSpec& s, double position) noexcept;
double noteParamPosition(const NoteParamSpec& s, double value) noexcept;
// What `d` plays for its param / setting `s`: the stored position (or setting value), else the declared default.
double noteDeviceValue(const Device& d, const NoteParamSpec& s) noexcept;

}  // namespace pq::doc
