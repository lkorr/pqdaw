#pragma once

// NOTE DEVICES (AP.2; docs/plans/AP.md "Where it sits"): a block whose main input and main output are NOTES — a note
// stage on a MIDI track's notes link, after the tuning block and before the first block that takes notes (an
// instrument group, an Instrument Rack, a meta module fed the note source). The arpeggiator is the first one.
//
//   the device   Device::patch "note:<type>" (kNoteDevicePrefix) of a registered NoteDeviceType, kind AudioEffect (on a
//                track the kind of a block means nothing: its signals decide). Its params are the type's runtime params
//                (knob POSITIONS, GU.1: automation and drags write them) and its settings the type's structural ones.
//                Only on a MIDI track (checks.cpp: no notes reach any other), never in a group.
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
};

struct NoteDeviceType {
  std::string id;         // "arpeggiator" (Device::patch "note:arpeggiator")
  std::string name;       // "Arpeggiator"
  std::string patchName;  // its fixed patch's name ("Arpeggiator": DeviceLibrary::patchOf)
  ChainSignal in = ChainSignal::Notes, out = ChainSignal::Notes;  // its declared main signals
  std::vector<NoteParamSpec> params;
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
// The fixed patch text a type's devices play for the views and the edit builders (`in notes`, its params with their
// ranges, defaults, @steps, @label, @choices, its settings as `real const` inputs, `out out = notes`). Never lowered.
std::string noteDevicePatchText(const NoteDeviceType& t);

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
