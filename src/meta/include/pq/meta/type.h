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
#include "pq/dsp/curve.h"
#include "pq/graph/patch.h"
#include "pq/graph/types.h"
#include "pq/meta/data.h"

namespace pq::meta {


struct ParamSpec {
  std::string_view name;
  std::string_view label;
  double def = 0.0, min = 0.0, max = 1.0;
  graph::Unit unit = graph::Unit::None;  // the literals' unit in the expansion (Hz: values are Hz)
  // GU.1: the control's declared curve (pq/dsp/curve.h), as the expansion's `@curve(<preset>[, curveK])` (Octave is
  // written `exp`, its old name; Exp0: exponential from exactly min, the Phase accumulator's frequency: 0 Hz at 0).
  dsp::CurvePreset curve = dsp::CurvePreset::Linear;
  double curveK = 0.0;                   // Exp0's k (Exponential / Logarithmic: the bend, 0: the default)
  bool wraps = false;                    // the face drags it around its range (a phase)
  bool bakeable = false;                 // static, it may be baked into table data (TMM.5); else always runtime
  // TMM.18: the param's value is SIGNED beyond its knob range: the knob covers min … max, but what wires add may take
  // it down to −max(|min|, |max|) (an `@signed` attribute on the expansion's param; its curve is mirrored below 0:
  // dsp::CurveMode::Signed). (GU.4: the Phase Accumulator preset's Frequency value block carries such a curve.)
  bool signedValue = false;
};
// GU.1: the param's declared curve — graph::declaredCurve of the expansion's declaration of it (the same curve the
// document lowering reads), and the value at a knob position / the position of a value through it.
dsp::Curve paramCurve(const ParamSpec& p) noexcept;
double travelToValue(const ParamSpec& p, double t) noexcept;  // t clamped to [0, 1]
double valueToTravel(const ParamSpec& p, double v) noexcept;  // the nearest position (dsp::curvePosition)

struct SettingSpec {
  std::string_view name;
  std::string_view label;
  double def = 0.0, min = 0.0, max = 1.0;
  uint32_t steps = 0;  // n discrete values min … max (0: any value; settings are usually choices)
};

// What an unwired input reads. TMM.10-fix1 (SPEC §15 2026-11-01 "notes are a signal"): no default reads the note's
// pitch (the old VoiceHz / VoicePhase): a module hears a note only through a wire — in a circuit `voice.hz`, in a
// chain the wire from the note source (Form::ChainNotes).
enum class InputDefault : uint8_t {
  Zero,       // −0.0: an added zero the optimiser removes (x + (−0) → x), so an unwired modulation input costs nothing
  One,        // TMM.11: 1.0 (a multiplier: the Envelope's `in` unwired, out = the envelope; the optimiser removes x·1)
  VoiceGate,  // TMM.11: voice.gate when the module runs per voice (ExpandContext::voice), else false (a Bool input):
              // the gate of the voice the module belongs to (QUESTIONS Q?-TMM.10-fix1-a item 3)
};
// TMM.10-fix1: what an input means — how the note source feeds it when it is a chain device's chain input
// (Form::ChainNotes).
enum class Reads : uint8_t {
  Signal,     // audio or a control value: the note source has none (the input reads its default)
  Frequency,  // Hz: the note's frequency (voice.hz)
  Phase,      // a phase in [0, 1): the note's phase (phasor(voice.hz), from 0 at note-on: its frequency integrated)
  Gate,       // a Bool gate: the note's gate (voice.gate)
};
struct InputSpec {
  std::string_view name;
  std::string_view label;
  InputDefault def = InputDefault::Zero;
  graph::Kind kind = graph::Kind::Real;  // TMM.11: Bool for a gate (kinds never convert implicitly: infer.h)
  Reads reads = Reads::Signal;           // TMM.10-fix1
};
struct OutputSpec {
  std::string_view name;
  uint16_t width = graph::kWidthMono;
};

// An off-thread resource an expansion reads through a slot id (a frame table, a voice tap; TMM.2 builds them). The
// document lowering allocates the slots (LowerOptions::metaSlotOf) and lists each instance's requests with its data
// for the link (LoweredProject::metaResources).
enum class ResourceKind : uint8_t { FrameTable, VoiceTap, SignalTap };  // SignalTap: TX.spectrum (a pq::TapRing)
struct ResourceRequest {
  ResourceKind kind = ResourceKind::FrameTable;
  uint32_t index = 0;  // the type's own numbering (ExpandContext::slot(index))
  friend bool operator==(const ResourceRequest&, const ResourceRequest&) = default;
};

// Where an expansion is called from. TMM.10-fix1 ("a chain is wires"): a chain device's form follows from what reaches
// its chain input (Type::chainInput, or the device's own choice: ExpandContext::chainInput), never from its type: every
// type has every form (pq/document/meta.h chainStages decides which one a device plays).
enum class Form : uint8_t {
  Nested,      // a call `name = meta_<type>(…)` in a circuit: every input is an `in` (with its default)
  ChainNotes,  // the chain input is wired to the NOTE SOURCE (the first stage of a MIDI track or Instrument Rack chain):
               // it reads the note's signal of its meaning (Reads: Frequency voice.hz, Phase phasor(voice.hz), Gate
               // voice.gate; Signal: its default), the other inputs their defaults; outputs per voice, plus `done`
               // (the type's voice end, else !voice.gate)
  ChainAudio,  // the chain input is an `in` of global audio (the previous stage's signal), outputs global; with no chain
               // input (ExpandContext::chainInput −1: unplugged) every input reads its default: the module runs once,
               // freely (a Phase accumulator: the phase of its frequency bar)
  ChainVoice,  // TMM.10: the chain input is a per-voice `in` (the previous stage's voice signal: docs/plans/MM.md "Voice
               // continuity"), the other inputs read their defaults (voice context), outputs per voice, no `done` (the
               // lowering ends the voices: pq/document/lower.h "Meta modules")
};

// ExpandContext::chainInput: the type's own chain input (Type::chainInput).
inline constexpr int32_t kTypeChainInput = -2;
struct ExpandContext {
  Form form = Form::Nested;
  bool voice = true;                      // voice.* exists (the module runs per voice): VoiceGate reads voice.gate
  int32_t chainInput = kTypeChainInput;   // chain forms: the input the chain feeds (−1: none; kTypeChainInput: the type's)
  std::span<const double> settings;    // per SettingSpec; empty: every default; a value is clamped to its range
  std::span<const uint8_t> modulated;  // per ParamSpec: 1 = automated, wired or macro-mapped (not static)
  // TX.patching (chain forms): per InputSpec, 1 = wires reach it (pq/document/lower.h "Modulation wires"): an `in` of
  // the expansion whatever the form, which reads its default while the call leaves it unconnected; the chain input
  // under ChainNotes reads the note's signal PLUS that `in` (a wire adds to what the chain gives it). Empty: none.
  std::span<const uint8_t> wired;
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
  // TMM.10-fix1: the input a chain wires the previous stage's signal into (−1: none), like a device's audio `in`. Any
  // type may sit anywhere in any chain; what arrives there decides the form (Form). A device may plug the chain into
  // another of its inputs, or none (doc::Device::chainInput).
  int32_t chainInput = -1;
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
  // path for it. false: such params are baked only, and the document refuses to modulate them
  // (doc::metaParamBakedOnly). TMM.8: the Wavetable's are live (TX.mod-all: in either mode).
  bool liveBakeable = true;
  // TMM.8 (optional): with these (resolved) settings, why bakeable param `param` cannot be modulated ("" = it can):
  // a type whose live path depends on a setting (TX.mod-all: no type has one now: a wire into a param makes it live
  // at audio rate, one rule).
  std::string (*bakedOnly)(std::span<const double> settings, uint32_t param) = nullptr;
  // TMM.8-fix (optional): a number for what of the instance's structural state its TABLE depends on (which params the
  // live path applies, the Wavetable's Shape mode). The lowering asks the link for the slot of (device, path,
  // index | variant << 16), so a change gives the recompiled image a slot of its own, built before the image plays,
  // while the old image keeps reading the table it was made for: baked and live never mix (null: 0).
  uint32_t (*tableVariant)(std::span<const double> settings, std::span<const uint8_t> modulated) = nullptr;
  // TMM.11: the type decides when a voice ends (the Envelope: released and below −80 dB). Its body calls
  // Builder::done(x) with a Bool; the framework then
  //   ChainNotes       uses it for the `done` output (instead of !voice.gate)
  //   ChainVoice       adds an output `done` (Bool, voice) after the outputs: the lowering ends the run's voices by the
  //                    run's last device's `done` instead of its fixed release (pq/document/lower.h "Meta modules")
  //   Nested           adds an output `done` (Bool) after the outputs (an instrument circuit wires it to its `done`)
  //   ChainAudio       ignores it
  bool voiceEnd = false;

  int32_t paramIndex(std::string_view n) const noexcept;    // −1: none
  int32_t settingIndex(std::string_view n) const noexcept;  // −1: none
  int32_t inputIndex(std::string_view n) const noexcept;    // −1: none
};
// TMM.10-fix1: the input a chain feeds in `context` (ExpandContext::chainInput resolved against the type; −1: none).
int32_t chainInputOf(const Type& type, const ExpandContext& context) noexcept;

// Ids of an expansion's nodes (graph::NodeId values): fixed per port, so a re-expansion keeps every state key.
inline constexpr uint32_t kInputIdBase = 1;      // input i's `in`
inline constexpr uint32_t kDefaultIdBase = 64;   // input i's default value
inline constexpr uint32_t kParamIdBase = 128;    // param i
inline constexpr uint32_t kOutputIdBase = 256;   // output i (kOutputIdBase + outputs: `done`)
inline constexpr uint32_t kFixedIdBase = 384;    // the framework's own nodes (voice.gate, not; a Phase chain input
                                                 // i's voice.hz under ChainNotes: kFixedIdBase + 32 + i; TX.patching:
                                                 // a wired chain input's −0 / sum + 64 / + 96 + i)
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
  // TMM.8: a new `oversample(factor, phase: …)` region (its id) — `phase` names outer values bound with bind() — and
  // a body node computed inside one (PATCH_LANGUAGE §2.3).
  uint16_t region(uint32_t factor, std::initializer_list<std::string_view> phase);
  void inRegion(PortRef node, uint16_t region);
  void output(size_t i, PortRef value);
  // TMM.11: the voice's end (a Bool; Type::voiceEnd types only, required for them).
  void done(PortRef value);
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
  PortRef done_{graph::kNone, 0};
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
// A hash of everything an expansion depends on (type id and version, form, voice, a chain input other than the
// type's, resolved settings, modulated params, slots): equal keys give equal patches.
uint64_t expansionKey(const Type& type, const ExpandContext& context) noexcept;

}  // namespace pq::meta
