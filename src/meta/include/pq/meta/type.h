#pragma once

// Meta-module types (TMM.3; docs/plans/MM.md "Architecture", SPEC §15 2026-11-01 "meta modules").
//
// A META MODULE is a subcircuit with its own interactive face whose state lives in the document. Every kind of module
// (the Phase accumulator, the Wavetable, the Envelope, …) is one `Type`: its controls (params, settings), inputs and
// outputs, its state data, and its EXPANSION — a generator that turns the state's structural part into an ordinary
// graph::Patch built from atoms (and stock subcircuits). The compiler never sees a meta module, only its expansion.
//
//   params     runtime values (a face drag is a param write, never a compile). The expansion declares each as a
//              `param` with the TYPE's default: the instance's value reaches the engine as a runtime param
//              (document lowering: `dev<id>:<path>.<name>`, or the device's own param for a chain device), so a
//              value never changes the generated patch.
//   settings   structural values (`body` reads them: a change regenerates the patch and recompiles its unit).
//   data       DataValue (data.h): never seen by `body`; edits are the document's MetaData class (no recompile).
//   inputs     wires into the module; unwired, an input reads its default (InputDefault).
//
// The expansion is deterministic and hash-stable: the same Type, version and ExpandContext always give the same patch
// with the same NodeIds (ports have fixed ids, body nodes the ids the type gives them), so a module's state keys
// survive every re-expansion, and expansionKey() names the result. A type's SIGNATURE (inputs then params, outputs) is
// fixed for all states, so a call carries one signature whatever the instance's state (graph::SubcircuitRef).
//
// Not RT code.

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/graph/patch.h"
#include "pq/graph/types.h"
#include "pq/meta/data.h"

namespace pq::meta {

// How a face's control travel t ∈ [0, 1] maps to the value (and the param's `@curve` attribute in the expansion).
enum class Curve : uint8_t {
  Linear,  // min + t·(max − min)
  Exp,     // min·(max / min)^t (min > 0): `@curve(exp)`
  Exp0,    // min + (max − min)·(2^(k·t) − 1)/(2^k − 1), k = curveK: exponential from exactly min (the Phase
           // accumulator's frequency: 0 Hz at t = 0); `@curve(exp0, k)`
};

struct ParamSpec {
  std::string_view name;
  std::string_view label;
  double def = 0.0, min = 0.0, max = 1.0;
  graph::Unit unit = graph::Unit::None;  // the literals' unit in the expansion (Hz: values are Hz)
  Curve curve = Curve::Linear;
  double curveK = 0.0;                   // Exp0's k
  bool wraps = false;                    // the face drags it around its range (a phase)
  bool bakeable = false;                 // static, it may be baked into table data (TMM.5); else always runtime
};
double travelToValue(const ParamSpec& p, double t) noexcept;  // t clamped to [0, 1]
double valueToTravel(const ParamSpec& p, double v) noexcept;  // v clamped to [min, max]

struct SettingSpec {
  std::string_view name;
  std::string_view label;
  double def = 0.0, min = 0.0, max = 1.0;
  uint32_t steps = 0;  // n discrete values min … max (0: any value; settings are usually choices)
};

// What an unwired input reads.
enum class InputDefault : uint8_t {
  Zero,     // −0.0: an added zero the optimiser removes (x + (−0) → x), so an unwired modulation input costs nothing
  VoiceHz,  // voice.hz in a voice context, else −0.0
  VoicePhase,  // TMM.5: phasor(voice.hz) in a voice context (a per-voice phase that restarts at note-on: a lone
               // Wavetable is an oscillator), else −0.0. A wired input leaves the phasor unused (removed).
};
struct InputSpec {
  std::string_view name;
  std::string_view label;
  InputDefault def = InputDefault::Zero;
};
struct OutputSpec {
  std::string_view name;
  uint16_t width = graph::kWidthMono;
};

// An off-thread resource an expansion reads through a slot id (a frame table, a voice tap; TMM.2 builds them). The
// document lowering allocates the slots (LowerOptions::metaSlotOf) and lists each instance's requests with its data
// for the link (LoweredProject::metaResources).
enum class ResourceKind : uint8_t { FrameTable, VoiceTap };
struct ResourceRequest {
  ResourceKind kind = ResourceKind::FrameTable;
  uint32_t index = 0;  // the type's own numbering (ExpandContext::slot(index))
  friend bool operator==(const ResourceRequest&, const ResourceRequest&) = default;
};

// Where an expansion is called from.
enum class Form : uint8_t {
  Nested,           // a call `name = meta_<type>(…)` in a circuit: every input is an `in` (with its default)
  ChainInstrument,  // a chain device at a MIDI track's instrument position: no `in`s (each input reads its default),
                    // its outputs per voice, plus `done` = !voice.gate (the voice ends at note-off)
  ChainEffect,      // a chain audio effect: Type::chainEffectInput is the device's audio `in` (global), the other
                    // inputs read their defaults, outputs global
};

struct ExpandContext {
  Form form = Form::Nested;
  bool voice = true;                   // voice.* exists (an instrument's circuit): VoiceHz defaults to voice.hz
  std::span<const double> settings;    // per SettingSpec; empty: every default; a value is clamped to its range
  std::span<const uint8_t> modulated;  // per ParamSpec: 1 = automated, wired or macro-mapped (not static)
  std::span<const uint32_t> slots;     // per resource request index; missing: 0
};

class Builder;
struct Expansion;
struct TableSource;  // pq/meta/table_builder.h (TMM.5)
struct TableJob;
struct Type {
  std::string_view id;    // "phase_acc": an identifier without "__" (the call is `meta_<id>`)
  std::string_view name;  // "Phase Accumulator"
  uint32_t version = 1;   // of the state (params, settings, data): a file with a newer one is refused
  std::span<const InputSpec> inputs;
  std::span<const ParamSpec> params;
  std::span<const SettingSpec> settings;
  std::span<const OutputSpec> outputs;  // at least one; the first is the call's value
  bool chainInstrument = false;  // may be a chain device at the instrument position (Form::ChainInstrument)
  int32_t chainEffectInput = -1; // ≥ 0: may be a chain audio effect, the chain's signal into this input
  // The resources an expansion with these (resolved) settings reads; null: none.
  std::vector<ResourceRequest> (*resources)(std::span<const double> settings) = nullptr;
  // The expansion's nodes between the ports (Builder: inputs, params, settings, slots → outputs). Required.
  Status (*body)(Builder& b) = nullptr;
  // The data of a new instance (null: none); its problems for this type ("" = valid; null hook: only a null data is
  // valid); an older version's data upgraded to `version` (null hook: data passes unchanged).
  DataPtr (*defaultData)() = nullptr;
  std::string (*dataProblem)(const DataValue& data) = nullptr;
  Result<DataPtr> (*upgradeData)(const DataPtr& data, uint32_t fromVersion) = nullptr;
  // TMM.5: the table a FrameTable request reads, built off the audio thread from the instance's data, params and
  // settings (pq/meta/table_builder.h). Null: the type builds no table (its slot keeps the link's default). An error
  // leaves the slot's table as it is.
  Status (*frameTable)(const TableSource& source, uint32_t index, TableJob& out) = nullptr;
  // Whether a `bakeable` param may be modulated (an automation lane, a wire, a macro): the expansion then has a live
  // path for it. false (the Wavetable until TMM.8): such params are baked only, and the document refuses to modulate
  // them (doc::metaParamBakedOnly).
  bool liveBakeable = true;

  int32_t paramIndex(std::string_view n) const noexcept;    // −1: none
  int32_t settingIndex(std::string_view n) const noexcept;  // −1: none
  int32_t inputIndex(std::string_view n) const noexcept;    // −1: none
};

// Ids of an expansion's nodes (graph::NodeId values): fixed per port, so a re-expansion keeps every state key.
inline constexpr uint32_t kInputIdBase = 1;      // input i's `in`
inline constexpr uint32_t kDefaultIdBase = 64;   // input i's default value
inline constexpr uint32_t kParamIdBase = 128;    // param i
inline constexpr uint32_t kOutputIdBase = 256;   // output i (kOutputIdBase + outputs: `done`)
inline constexpr uint32_t kFixedIdBase = 384;    // the framework's own nodes (voice.gate, not; input i's VoicePhase
                                                 // voice.hz: kFixedIdBase + 32 + i)
inline constexpr uint32_t kSlotIdBase = 512;     // resource slot constant i
inline constexpr uint32_t kBodyIdBase = 1024;    // body node k: kBodyIdBase + k
inline constexpr uint32_t kMaxBodyIds = 1u << 20;

// What a type's body builds with. Every node it adds gets an explicit id k (< kMaxBodyIds, once each), so its state
// key is the same in every expansion.
class Builder {
public:
  using PortRef = graph::PortRef;
  const Type& type() const noexcept { return *type_; }
  const ExpandContext& context() const noexcept { return *ctx_; }
  PortRef input(size_t i) const noexcept { return inputs_[i]; }
  PortRef param(size_t i) const noexcept { return params_[i]; }
  double setting(size_t i) const noexcept { return settings_[i]; }      // resolved (default when not given)
  bool modulated(size_t i) const noexcept;                               // ExpandContext::modulated
  PortRef slot(uint32_t index);                                          // the slot id as a constant
  PortRef atom(std::string_view atomName, std::initializer_list<PortRef> ins, uint32_t k, uint16_t repeat = 0);
  PortRef konst(double v, uint32_t k);
  // Names a body node (a binding in the text form). A value read twice, and a sink, need one for the printed
  // expansion to parse back to the same graph; the name must not be a port's. Names do not change node ids.
  void bind(PortRef value, std::string_view name);
  void output(size_t i, PortRef value);
  graph::Patch& patch() noexcept { return p_; }

private:
  friend Result<Expansion> expand(const Type&, const ExpandContext&, std::string);
  Builder(const Type& t, const ExpandContext& c, std::string name) : type_(&t), ctx_(&c), p_(std::move(name)) {}
  graph::NodeIndex take(graph::NodeIndex n, uint32_t id);
  const Type* type_;
  const ExpandContext* ctx_;
  graph::Patch p_;
  std::vector<PortRef> inputs_, params_;
  std::vector<double> settings_;
  std::vector<graph::NodeIndex> outputs_;
  std::vector<uint16_t> outPorts_;
  std::vector<uint8_t> outputSet_;
  std::vector<uint32_t> ids_;
  bool failed_ = false;
  std::string error_;
};

struct Expansion {
  graph::Patch patch;
  std::vector<ResourceRequest> resources;  // in request order (slot(index) reads ExpandContext::slots[index])
  uint64_t key = 0;                        // expansionKey(type, context)
};

// The type's expansion in `context`, named `name`. Errors: an invalid body (a node id twice or out of range, an
// output left unset, a patch that does not validate) or a body's own error.
Result<Expansion> expand(const Type& type, const ExpandContext& context, std::string name);
// A hash of everything an expansion depends on (type id and version, form, voice, resolved settings, modulated
// params, slots): equal keys give equal patches.
uint64_t expansionKey(const Type& type, const ExpandContext& context) noexcept;

}  // namespace pq::meta
