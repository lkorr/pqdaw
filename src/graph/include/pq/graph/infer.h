#pragma once

// Type inference (T1.3, SPEC §7.1/§7.4, PATCH_LANGUAGE §3): flattens a patch and infers the kind, rate, poly and
// width of every port.
//
// Flattening. Every subcircuit node is instantiated (recursively) per call site, so each instance is typed on its
// own: a filter whose cutoff is unmodulated at one call site and audio-rate at another gets a param-rate cutoff
// path at the first and an audio-rate one at the second ("pay only for what is used", SPEC §0). The result has no
// Subcircuit nodes; a callee's `in`s, bound `param`s, `out`s and `export`s become plain wires. A callee `param`
// that the call site leaves unconnected stays a Param node (a knob of that instance, never folded).
//
// Lattices (each inferred field joins upwards, starting from "unknown"):
//   rate   const < param < audio   (event: only into event; nothing produces event in v0)
//   poly   global < voice
//   width  mono < N (mono broadcasts; two different N > 1 are an error)
//   kind   real | bool, never converted implicitly (gate_to_real / comparisons are explicit atoms)
// Rules:
//   - A declared port type is an UPPER BOUND for what flows into it: atom inputs (e.g. delay `max` is const), the
//     `in`s of a callee, the root's `out`s. Exceeding a rate or width bound, or a kind mismatch, is an error,
//     reported at the node's source position. Callee `param`s are modulation targets and accept any rate and poly
//     (a modulation wire raises the parameter's path to audio rate); their kind and width bounds still apply.
//   - A node's output rate is the max of its input rates; stateful atoms (z1, phasor, delay …) output audio, and
//     sources output their declared rate (voice.velocity param, sr const …).
//   - A node is per-voice if any input is (or it is a voice.* source). A voice signal into a GLOBAL bound (the
//     root's `out … global`, a callee's `in … global`) is reduced by an inserted voice_sum node (SPEC §7.4: poly →
//     global defaults to sum; on bools the sum is "any voice"). The per-wire override is an explicit reducer call
//     in the text: voice_last / voice_highest / voice_lowest / voice_loudest (QUESTIONS Q28).
//   - Free-running sources (T1.3-fix1, QUESTIONS Q29 B): a stateful atom whose inputs are all global and at most
//     param rate (noise(), pulse(), phasor(0.5Hz), phasor(rateParam)) is per-voice when InferOptions::voiceContext
//     and freeRunningPoly = Voice (the default) and its output feeds voice processing (some path of consumers
//     reaches a per-voice node, not through a global-bound port): one instance per voice, phase from note-on, noise
//     seeded per voice. A source that reaches voice processing only through a node marked `shared(…)`
//     (Node::shared) keeps one global instance, as does every source with freeRunningPoly = Global (the T1.3
//     behaviour). Delay lines are never promoted. Decided after the fixpoint (infer.cpp freeRunning).
//   - Element-wise atoms broadcast mono over N channels; pack concatenates channels; channel(x, i) is mono (i must
//     be a const below x's width, any i on a mono x). Engine I/O sources have InferOptions::ioChannels channels.
//   - eq / ne on bools are rewritten to not(xor) / xor (the dsp comparators are real-only, kernel_map.h).
//
// Cost: linear in the flattened graph (each node is re-evaluated at most a lattice-height number of times by a
// worklist seeded in topological order). Not RT-safe: compiler thread.

#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pq/core/result.h"
#include "pq/graph/parse.h"
#include "pq/graph/patch.h"
#include "pq/graph/inspect.h"

namespace pq::graph {

// Callees by name, for subcircuit nodes. Holds pointers: the patches must outlive the library and the infer() call.
// The first patch added under a name wins (like call resolution: the file's own patches first, then imports).
class PatchLibrary {
public:
  void add(const Patch& p);
  void add(const PatchFile& f);
  // Every patch of `other` whose name is not here yet (T1.x-arch1: a project unit's library = its track patches,
  // then the project's callees).
  void addAll(const PatchLibrary& other);
  const Patch* find(std::string_view name) const noexcept;

private:
  std::unordered_map<std::string_view, const Patch*> byName_;
};

// What the engine connects to one `in` of the root patch.
struct RootInput {
  bool connected = true;  // false: the input reads its default expression (an error if it has none)
  PortType type{};        // the type of what is connected; Infer fields fall back to the declaration, then to
                          // audio rate, global, mono
};

// The root patch's source text, so errors can be reported as file:line:col (ParseResult::nodeSpans[patch]).
struct SourceText {
  std::string_view text;
  std::string_view fileName;
  std::span<const SourceSpan> nodeSpans;
};

struct InferOptions {
  const PatchLibrary* library = nullptr;  // callees; without one, any subcircuit node is an error
  bool voiceContext = true;               // voice.* sources allowed (false for an audio effect's patch)
  bool implicitVoiceSum = true;           // false: a voice signal into a global bound is an error (needs an
                                          // explicit reducer call) instead of getting a voice_sum
  Poly freeRunningPoly = Poly::Voice;     // free-running sources feeding voice processing: Voice (Q29 B) or Global
  uint16_t ioChannels = 2;                // width of track.in, sidechain.in and clipPlayer
  std::span<const RootInput> inputs;      // per root `in`, in node order; missing entries = connected, declared
  SourceText source{};                    // optional
  // T4.5 (pq/graph/inspect.h): values to locate in the flattened patch (InferResult::probeSources). Changes nothing else.
  std::span<const ProbeTarget> probes;
};

inline constexpr uint32_t kMaxSubcircuitDepth = 64;  // nesting of subcircuit instances
inline constexpr uint16_t kMaxWidth = 64;            // channels of one signal

// One instantiated patch. instances[0] is the root.
struct InstanceInfo {
  const Patch* patch = nullptr;
  uint32_t parent = kNone;    // instance index of the caller (kNone for the root)
  NodeIndex callNode = kNone; // the subcircuit node in the parent's patch
  uint64_t key = 0;           // hash of the call path (NodeIds of the call nodes from the root)
};

// Where a flattened node comes from.
struct NodeOrigin {
  enum class What : uint8_t {
    Node,      // the node `node` of instances[instance].patch
    Reducer,   // an inserted voice_sum: `node` is the node whose input (or, for a callee `in`, the call) it feeds
    Rewrite,   // a node of a kind-directed rewrite (bool eq/ne → xor, not) of the node `node`
  };
  uint32_t instance = 0;
  NodeIndex node = kNone;
  What what = What::Node;
};

// The flattened, typed patch that the optimiser (T1.4) and scheduler (T1.5) work on.
struct TypedPatch {
  // No Subcircuit nodes. Port types in here are still the declared bounds; the inferred types are in `types`.
  // Nodes of the root keep their names; nodes of callees are named `<call>__<name>` (the call node's name, or
  // `<patch>_<index>` for an unnamed call) when they had a name. Inner `in`/`out`/`export` nodes are gone (wired
  // through); root `in`s connected by the engine have no default wire.
  Patch patch;
  // Parallel to patch.portTypes(): every field concrete (kind real|bool, rate const|param|audio, poly, width ≥ 1).
  // An input's type is the type of the value arriving (after any reducer); an unconnected optional input is a
  // const, global, mono value of its declared kind.
  std::vector<PortType> types;
  // Per node: the hot-swap state key (SPEC §7.5). Stable across recompiles as long as the source NodeIds along the
  // call path are; use it, not patch.node(n).id, to key state (flattened NodeIds are renumbered every time).
  std::vector<uint64_t> stateKey;
  std::vector<NodeOrigin> origin;     // per node
  std::vector<InstanceInfo> instances;
  // false (as inferred): duration inputs (kernel_map.h ArgUse::InputSeconds) carry seconds. optimise() inserts the
  // `· sr` conversions and sets this: those inputs then carry samples, exactly what the kernels take.
  bool durationsInSamples = false;

  const PortType& inputType(NodeIndex n, uint32_t i) const noexcept { return types[patch.flatInput(n, i)]; }
  const PortType& outputType(NodeIndex n, uint32_t i) const noexcept { return types[patch.flatOutput(n, i)]; }
};

struct InferResult {
  TypedPatch typed;                     // complete only when diagnostics is empty
  std::vector<Diagnostic> diagnostics;  // located at the root patch node (the call site for errors inside callees)
  // Per InferOptions::probes entry: the flat output port of typed.patch carrying that value (kNone: no such node or
  // output, or unconnected). T4.5.
  std::vector<uint32_t> probeSources;
  bool ok() const noexcept { return diagnostics.empty(); }
};

// Every problem (up to kMaxDiagnostics). The patch should have passed validate(); infer() re-checks only what it
// relies on (structure it cannot type is reported, not assumed).
InferResult inferDetailed(const Patch& root, const InferOptions& options = {});
// The typed patch, or an Error whose message holds every diagnostic (formatted with the source line and a caret
// when options.source is set), with the first one's code and location.
Result<TypedPatch> infer(const Patch& root, const InferOptions& options = {});

}  // namespace pq::graph
