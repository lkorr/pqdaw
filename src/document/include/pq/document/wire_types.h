#pragma once

// WI.3 (docs/plans/RB.md "### WI.3"): what a wire's DECLARED port types already rule out, so a wiring gesture can refuse
// the drop instead of making a cable the compiler then cuts (WI.2, lower.h "Cut wires"). One general check for every
// gesture (the Device view's jacks, the circuit's pins and controls); everything it cannot see from declarations alone
// (a group's or an export's inferred kind, widths, voices, a rate bound) is left to WI.2's compile-time cut.
//
// The two clashes are graph::infer's own rules (infer.cpp check()) applied to the declarations:
//   RealIntoBool    the source is declared real and the target declares bool: an atom block's bool input (`and`,
//                   a flip-flop's), a bool param. (A bool into a real input converts: allowed.)
//   EventMismatch   one end is declared event-rate and the other a concrete non-event rate.
// An end the library cannot resolve (no device, no patch, no such port, a group terminal, a track's own signal or
// notes, an undeclared kind or rate) is not a clash.
//
// The SOURCE: a device's output by name (kMainPort: its main output, main_path.h), its declared `out` type (an export:
// its node's declared input type); a MIDI track's note output (model.h kNoteOutputs: gate bool, hz / velocity real). The
// TARGET: a device's input (kMainPort: its main input) — an `in`'s declared type — or param (`param` kind, param rate);
// a track's mixer input (audio rate).
//
// Not RT code (UI thread).

#include <cstdint>
#include <string_view>

#include "pq/document/model.h"

namespace pq::doc {

class DeviceLibrary;

enum class WireClash : uint8_t { None, RealIntoBool, EventMismatch };

// `lib` null: None (nothing is declared without it).
WireClash declaredWireClash(const Project& p, const Wire& w, const DeviceLibrary* lib);
// The status line's explanation ("" for None).
std::string_view wireClashText(WireClash c) noexcept;

}  // namespace pq::doc
