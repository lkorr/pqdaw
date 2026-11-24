// WI.3: the declared-type clash of a wire (pq/document/wire_types.h).
#include "pq/document/wire_types.h"

#include <optional>
#include <string>

#include "pq/document/devices.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/document/note_devices.h"
#include "pq/meta/registry.h"

namespace pq::doc {

namespace {

using graph::Kind;
using graph::NodeIndex;
using graph::PortType;
using graph::Rate;

const graph::Patch* patchOfDevice(const Project& p, DeviceId id, const DeviceLibrary& lib, const Device** dev) {
  const DeviceLoc loc = findDevice(p, id);
  if (loc.device == nullptr) return nullptr;
  *dev = loc.device;
  return lib.patchOf(*loc.device);
}

// The declared type of the source end, or nullopt when nothing is declared there.
std::optional<PortType> sourceType(const Project& p, const Wire& w, const DeviceLibrary& lib) {
  if (w.fromNoteSource()) {
    if (w.port.empty()) return std::nullopt;  // the track's own signal / its notes: the lowering's business
    const NoteOutput* n = findNoteOutput(w.port);
    if (n == nullptr) return std::nullopt;
    return graph::portType(n->boolean ? Kind::Bool : Kind::Real);
  }
  const Device* d = nullptr;
  const graph::Patch* callee = patchOfDevice(p, w.from, lib, &d);
  if (callee == nullptr) return std::nullopt;
  const std::string name = w.port.empty() ? mainPortsOf(*d, &lib).out : w.port;
  if (name.empty()) return std::nullopt;
  for (NodeIndex v = 0; v < callee->nodeCount(); ++v) {
    const graph::Node& n = callee->node(v);
    if ((n.kind == graph::NodeKind::Output || n.kind == graph::NodeKind::Export) && n.numInputs > 0 &&
        callee->nodeName(v) == name)
      return callee->inputType(v, 0);
  }
  return std::nullopt;
}

// The declared bound of the target end, or nullopt.
std::optional<PortType> targetType(const Project& p, const Wire& w, const DeviceLibrary& lib) {
  if (w.toMixer()) return graph::portType(Kind::Infer, Rate::Audio);
  const Device* d = nullptr;
  const graph::Patch* callee = patchOfDevice(p, w.to, lib, &d);
  if (callee == nullptr) return std::nullopt;
  const std::string name = w.param.empty() ? mainPortsOf(*d, &lib).in : w.param;
  if (name.empty()) return std::nullopt;  // an instrument's notes
  for (NodeIndex v = 0; v < callee->nodeCount(); ++v) {
    const graph::Node& n = callee->node(v);
    if (callee->nodeName(v) != name) continue;
    if (n.kind == graph::NodeKind::Input && n.numOutputs > 0) return callee->outputType(v, 0);
    if (n.kind == graph::NodeKind::Param) return graph::portType(callee->paramDecl(v).kind, Rate::Param);
  }
  return std::nullopt;
}

}  // namespace

WireClash notesWireClash(const Project& p, const Wire& w) {
  const Device* from = w.fromNoteSource() ? nullptr : findDevice(p, w.from).device;
  const Device* to = w.toMixer() ? nullptr : findDevice(p, w.to).device;
  const MainSignals fs = from != nullptr ? declaredMainSignals(*from) : MainSignals{};
  const MainSignals ts = to != nullptr ? declaredMainSignals(*to) : MainSignals{};
  if (fs.out != ChainSignal::Notes && ts.in != ChainSignal::Notes) return WireClash::None;  // (nearly every wire)
  // An end by its main port: kMainPort, or the port's own name (a wire not yet canonical).
  const bool mainOut = from != nullptr && (w.port.empty() || w.port == mainPortsOf(*from, nullptr).out);
  const bool mainIn = to != nullptr && (w.param.empty() || w.param == mainPortsOf(*to, nullptr).in);
  const bool notesOut = mainOut && fs.out == ChainSignal::Notes;
  const bool notesIn = mainIn && ts.in == ChainSignal::Notes;
  if (notesIn) {
    bool carries = notesOut;
    if (w.fromNoteSource() && w.port.empty())
      if (const Track* t = findTrackPtr(p, w.noteSource)) carries = t->kind == TrackKind::Midi;
    if (!carries) return WireClash::NotesInput;
  }
  if (notesOut && !notesIn) {
    bool takes = false;
    if (mainIn) {
      const meta::Type* mt = isMetaDevice(*to) ? meta::findType(metaDeviceType(*to)) : nullptr;
      takes = isGroupDevice(*to) || to->kind == DeviceKind::Instrument || (mt != nullptr && metaChainInput(*to, *mt) >= 0);
    }
    if (!takes) return WireClash::NotesOutput;
  }
  return WireClash::None;
}

WireClash declaredWireClash(const Project& p, const Wire& w, const DeviceLibrary* lib) {
  if (const WireClash c = notesWireClash(p, w); c != WireClash::None) return c;
  if (lib == nullptr) return WireClash::None;
  const std::optional<PortType> src = sourceType(p, w, *lib);
  if (!src) return WireClash::None;
  const std::optional<PortType> dst = targetType(p, w, *lib);
  if (!dst) return WireClash::None;
  // graph::infer's check(), on what the two ends declare (an Infer field is no declaration).
  if (dst->kind == Kind::Bool && src->kind == Kind::Real) return WireClash::RealIntoBool;
  if (dst->rate != Rate::Infer && src->rate != Rate::Infer && (dst->rate == Rate::Event) != (src->rate == Rate::Event))
    return WireClash::EventMismatch;
  return WireClash::None;
}

std::string_view wireClashText(WireClash c) noexcept {
  switch (c) {
    case WireClash::None: return "";
    case WireClash::RealIntoBool:
      return "Wire: a real signal cannot drive a bool input (on / off): compare it first (a comparison block's output is a bool)";
    case WireClash::EventMismatch: return "Wire: an event output only drives an event input, and an event input takes only events";
    case WireClash::NotesInput:
      return "Wire: a notes input takes only a notes cable (a MIDI track's notes, or a note effect's output)";
    case WireClash::NotesOutput:
      return "Wire: a notes output goes only into a main input that takes notes (an instrument, a rack, a module, a note effect)";
  }
  return "";
}

}  // namespace pq::doc
