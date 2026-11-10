#pragma once

// The leaf-atom registry: name → signature for every native atom (SPEC §7.2) and every operator of the patch
// language (PATCH_LANGUAGE §2: `a + b` is the atom `add`, `c ? a : b` is `select`, `x[i]` is `channel`, a tuple
// `(l, r)` is `pack`). The kernels are in `dsp` (T1.6); this is the data the compiler front end needs: port names
// and declared types, defaults for unconnected inputs, state size, and flags.
//
// The table is static and immutable. AtomId is the index into it; ids are stable within one build but are never
// persisted (files store names).

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "pq/graph/response_rule.h"
#include "pq/graph/types.h"

namespace pq::graph {

enum class AtomCategory : uint8_t {
  Generator,  // phasor, sine, wavetable, noise
  Math,       // arithmetic, transcendental (exact + fast_), conversions, select, lerp
  Shaping,    // shaper
  Memory,     // z1, delay*, samplehold, latch
  Logic,      // bool gates, comparators, edge detectors, flip-flops, counters, mux/demux
  Structure,  // channel select, pack, voice→global reducers (voice_sum …)
  Voice,      // voice.* sources
  Transport,  // transport.* sources, sr
  Sample,     // sample_read_*, sample_pos_*
  Io,         // track.in, sidechain.in, send, clipPlayer
};

std::string_view toText(AtomCategory) noexcept;

namespace atom_flags {
inline constexpr uint32_t kStateful = 1u << 0;        // keeps state across samples (see PortSpec::delayed for cycles)
inline constexpr uint32_t kVariableState = 1u << 1;   // state size depends on const inputs (delay buffers, players)
inline constexpr uint32_t kVariadicInputs = 1u << 2;  // the last input spec repeats (≥ minRepeat times)
inline constexpr uint32_t kVariadicOutputs = 1u << 3; // the last output spec repeats (≥ minRepeat times)
inline constexpr uint32_t kCommutative = 1u << 4;     // inputs 0 and 1 may be swapped (CSE canonicalisation)
inline constexpr uint32_t kNeedsVoice = 1u << 5;      // only valid in a voice context (voice.*)
inline constexpr uint32_t kNeedsTransport = 1u << 6;  // reads the transport snapshot
inline constexpr uint32_t kSideEffect = 1u << 7;      // writes outside the patch (send): never dead-code eliminated
inline constexpr uint32_t kFastApprox = 1u << 8;      // the fast_ approximation of an exact atom (exactVariant)
inline constexpr uint32_t kBroadcast = 1u << 9;       // real inputs broadcast mono over N channels (§3 Width)
inline constexpr uint32_t kInternal = 1u << 10;       // inserted by the compiler only (TMM.6: oversample_*): text and
                                                      // the circuit editor cannot call it
}  // namespace atom_flags

struct PortSpec {
  std::string_view name;
  PortType type;              // declared upper bound; Infer fields follow inference (T1.3)
  bool optional = false;      // may be left unconnected; then reads `defaultValue` as a const
  float defaultValue = 0.0f;  // bool inputs: 0 = false, 1 = true
  // The output at sample n depends on this input only up to sample n−1 (z1.x, delay.x, phasor.hz). A cycle whose
  // every path passes through such an input is ordinary feedback; any other cycle is delay-free (PATCH_LANGUAGE §3,
  // scheduled by T1.5). Stateful atoms with feedthrough (latch, samplehold, counter) do not break cycles.
  bool delayed = false;
};

// T6.2 (PDC, pq/graph/latency.h): what a path through one node of an atom gains, in BASE-rate samples. `value[i]` is
// input i's constant value where `known[i]` (a literal after optimise()); `factor` is the oversample factor of the
// region the node runs in (1 outside one).
struct LatencyArgs {
  uint32_t numInputs = 0;
  const double* value = nullptr;
  const uint8_t* known = nullptr;
  uint32_t factor = 1;
};
using LatencyRule = double (*)(const LatencyArgs& a) noexcept;

struct AtomSignature {
  std::string_view name;
  AtomCategory category;
  std::span<const PortSpec> inputs;
  std::span<const PortSpec> outputs;
  // Bytes of the kernel's state block (pq/dsp/atom_state.h) per channel: one block holds a voice group (8 lanes) or
  // one global instance (0 = stateless or engine-owned). With kVariableState this is the fixed part (header) only;
  // the scheduler (T1.5) adds the ring computed from the const inputs (dsp::stateBytes). kernel_map.h maps every
  // atom to its kernel and is checked against this table by the tests.
  uint32_t stateBytes = 0;
  uint32_t flags = 0;
  uint16_t minRepeat = 0;               // for variadic signatures: minimum count of the repeated port
  std::string_view exactVariant = {};   // for kFastApprox atoms: the exact atom this approximates
  std::string_view summary = {};        // one line, for tooltips and `pqc` listings
  // EQ.2: the small-signal response at one frequency (pq/graph/response_rule.h); null: nonlinear, no rule (a response
  // through this atom is Unavailable).
  ResponseRule response = nullptr;
  // EQ.1b: bytes of scratch the response rule keeps between the calls of one evaluation (ResponseArgs::cache).
  uint32_t responseCacheBytes = 0;
  // T6.2: the latency this atom REPORTS (pq/graph/latency.h; null: 0). Only what the plugin-delay compensation must
  // align: the `latency` atom's frames, an oversample region's half-band converters. z1 and delay lines are effects and
  // report nothing.
  LatencyRule latency = nullptr;

  bool has(uint32_t flag) const noexcept { return (flags & flag) != 0; }
  // Number of fixed (non-repeating) input/output ports.
  uint32_t fixedInputs() const noexcept;
  uint32_t fixedOutputs() const noexcept;
};

class AtomRegistry {
public:
  // The built-in table. Constructed once (thread-safe static), immutable afterwards.
  static const AtomRegistry& builtin();

  uint32_t size() const noexcept { return static_cast<uint32_t>(atoms_.size()); }
  const AtomSignature& get(AtomId id) const noexcept { return atoms_[id]; }
  std::span<const AtomSignature> all() const noexcept { return atoms_; }
  // O(log n) lookup by exact name (e.g. "add", "voice.hz", "fast_tan").
  std::optional<AtomId> find(std::string_view name) const noexcept;

private:
  AtomRegistry();
  std::span<const AtomSignature> atoms_;
  std::span<const AtomId> byName_;  // atom ids sorted by name
};

// Shorthand for the builtin registry's lookup, for code that names atoms literally: aborts if the name is unknown
// (a programming error). User-supplied names go through AtomRegistry::find.
AtomId atomId(std::string_view name) noexcept;
const AtomSignature& atomSignature(AtomId id) noexcept;

}  // namespace pq::graph
