#pragma once

// EQ.2 (docs/plans/EQ.md "Analytic responses"): the frequency response H(e^jω) of a lowered graph between a source and
// a sink, from the graph alone (no audio): any linear circuit — a filter band, a whole EQ with its M/S matrix, a comb —
// draws exactly.
//
// The graph is a TypedPatch as infer() makes it (flattened, typed; BEFORE optimise(), whose fusions would hide atoms
// behind fused kernels): its port widths decide the channels. Durations are read as the language writes them (seconds;
// TypedPatch::durationsInSamples is honoured) and converted with the sample rate, an `smp` literal wired straight into
// a duration as samples (optimise()'s rule).
//
// ---- The model ------------------------------------------------------------------------------------------------------
// A port value is SIGNAL when it depends on the source (forward from the source; the source's own inputs are cut: its
// value is the excitation). The RELEVANT part is what is both signal and reaches the sink. Every relevant node is a
// linear map of its signal inputs given its other inputs (its atom's response rule, pq/graph/response_rule.h), per
// output channel. Its other inputs are evaluated as CONTROL values (graph::ControlEval, pq/graph/control_eval.h, shared
// with StaticTransfer below): constants, params (setParam), `sr`, and the pure
// kernels computed from them (the runtime's own kernels, graph's constant folding), `channel` / `pack` / `mux` /
// `select` of them, and a `curve` atom through the curve the caller gives (setCurve). Anything else that is not signal
// (another audio input, a stateful atom's output, a solve block's unknown) is UNKNOWN: a rule that needs it (mul by an
// LFO) makes the response Unavailable; a rule that does not (add of an unrelated signal) ignores it (superposition).
//
// Evaluation: one complex unknown per (relevant port, channel) and source channel. The unknowns' dependency graph is
// split into strongly connected components at prepare (Tarjan; a component is a feedback loop through z1 / delay
// lines); per frequency they are solved in dependency order — a lone unknown by substitution, a loop by complex
// Gaussian elimination (partial pivoting) over that component only. O(relevant ports × points) for feed-forward
// circuits; a loop of k unknowns adds O(k³) per point. Multichannel: H for every (source channel, sink channel) pair
// at once (one right-hand side per source channel).
//
// Status (ResponseKind): Exact — every relevant atom is linear here; Approximate — a nonlinear atom with a slope at 0 is
// on the path (tanh); Unavailable — an atom without a rule (or one that is not linear at these values) is on the path,
// or the ends are invalid.
// TX.group-oversample: OVERSAMPLE REGIONS (PATCH_LANGUAGE §2.3). A node of a region at ×n is evaluated at ω/n (its z1 /
// delays / filters see z = e^(−jω/n)), with `sr` and its durations at n·sr; the converters on its edge are linear
// (dsp::os::halfBandResponse: the up / down half-band cascades at the base ω), so the response through a region is
// up(ω) · H_inner(ω/n at n·sr) · down(ω). It ignores what the region's images add (folded back by the down: below the
// half-bands' stopbands, ≥ 116 dB, up to 20 kHz). A phase converter, a bool's hold / any: no rule (Unavailable). A sink the source does not
// reach gives H = 0 (Exact). A frequency where a loop is singular (an undamped resonance exactly on it) gives a
// non-finite H at that point.
//
// TX.mod-bands: SET PORTS. prepare() may name flat output ports whose value the CALLER gives (setValue), as it gives a
// param's: the control evaluation reads them without evaluating what drives them, and the signal pass stops at their
// node (none of its outputs is signal). A driven control's modulated value is one (the port where its wires have been
// summed into it, doc::CurveSite::value): whatever modulates it — an LFO, an envelope, the input itself through a
// follower — the response is H with the control held at the value given (the system frozen at that instant), not
// Unavailable. A set port's node's other outputs are unknown. Only the set ports the response reads are kept
// (setPorts()); one never given a value is unknown.
//
// Cost and threads: prepare() allocates (once per structural revision of the graph; any thread). evaluate() never
// allocates, locks or throws (UI thread, on every drag); setParam / setCurve are plain stores. One evaluator is not
// thread-safe; prepare a copy per thread.

#include <complex>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pq/dsp/curve.h"
#include "pq/graph/control_eval.h"
#include "pq/graph/infer.h"
#include "pq/graph/response_rule.h"

namespace pq::graph {

class LinearResponse : public ControlEval {
public:
  // Prepares `typed` (copied: the evaluator keeps no reference) from `source` to `sink`. The parameters start at their
  // declared defaults, curve atoms without a curve are unknown. Never fails: invalid ends give status() Unavailable.
  // `settable`: candidate set ports (flat output ports of typed.patch; see "SET PORTS").
  void prepare(const TypedPatch& typed, ResponseEnd source, ResponseEnd sink, std::span<const uint32_t> settable = {});

  // Params, curves and set ports: ControlEval's API (paramNodes / setParam, curveNodes / setCurve, setPorts / setValue):
  // the ones the response depends on (control inputs of relevant nodes, at any depth).

  uint16_t sourceChannels() const noexcept { return srcWidth_; }
  uint16_t sinkChannels() const noexcept { return sinkWidth_; }
  // What prepare() could already decide (Unavailable: no response whatever the values; else the best evaluate() can
  // give).
  ResponseKind status() const noexcept { return prepared_; }

  // H at each frequency `hz` (Hz; at `sampleRate`): out[(k · sourceChannels() + i) · sinkChannels() + j] = the
  // response from source channel i to sink channel j at hz[k]. out.size() ≥ hz.size() · sourceChannels() ·
  // sinkChannels(). Returns the status at these values; when Unavailable, out is filled with NaN. RT-safe (no
  // allocation, locks or exceptions).
  ResponseKind evaluate(std::span<const double> hz, double sampleRate, std::span<std::complex<double>> out) noexcept;

private:
  // One point at every rate: ω / 2^r, its z = e^(−jω/2^r) and e^(jω/2^r) − 1.
  struct Point {
    double omega[kRates] = {};
    std::complex<double> z[kRates] = {};
    std::complex<double> ejw1[kRates] = {};
  };
  struct Cell {  // one relevant node at one output channel
    ResponseRule rule = nullptr;
    uint32_t inBegin = 0, inCount = 0;  // into ins_
    uint32_t numOutputs = 0;
    uint16_t channel = 0;
    uint32_t gainBase = 0;              // into gain_: numOutputs × inCount
    uint32_t argBase = 0;               // into argValue_ / argKnown_ / argSignal_: inCount (this channel's values)
    uint32_t cacheBase = 0;             // into cache_: the rule's scratch (AtomSignature::responseCacheBytes)
    bool zOnly = false;                 // a z1: its term reads the shared z slot (gain_[rate]) instead of calling the rule
    uint8_t rate = 0;                   // TX.group-oversample: evaluated at ω / 2^rate, 2^rate · sr (Point)
  };
  struct Term {
    uint32_t from = 0;  // unknown read
    uint32_t gain = 0;  // into gain_
  };

  // Gathers the cell's argument values (once per evaluate) and calls its rule.
  ResponseKind firstCall(uint32_t c, const Point& pt, double sampleRate, bool& dynamic) noexcept;
  // Calls a dynamic cell's rule again at another frequency (its arguments as gathered).
  ResponseKind recall(uint32_t c, const Point& pt, double sampleRate, bool& dynamic, bool fresh = false) noexcept;
  // Points are solved kBatch at a time, as lanes: every value and gain holds kBatch doubles (re and im apart).
  static constexpr uint32_t kBatch = 8;
  void storeGains(uint32_t cell, uint32_t lane) noexcept;
  void solveBatch(uint32_t first, uint32_t count, std::span<std::complex<double>> out) noexcept;

  ResponseKind prepared_ = ResponseKind::Unavailable;
  bool durationsInSamples_ = false;
  uint16_t srcWidth_ = 1, sinkWidth_ = 1;
  bool identity_ = false;  // source == sink
  bool zero_ = false;      // the sink is not reached

  std::vector<InSrc> ins_;  // the cells' inputs

  // The linear system.
  std::vector<Cell> cells_;
  std::vector<std::complex<double>> gain_;  // the rules' output (per cell, at one point); slot 0 unused (z)
  std::vector<uint32_t> dynamicCells_;  // capacity cells_.size(); filled per evaluate
  uint32_t dynamicCount_ = 0;
  std::vector<uint32_t> termBegin_;     // per unknown + 1
  std::vector<Term> terms_;
  std::vector<uint32_t> sccBegin_;      // per component + 1, into sccVars_ (dependency order)
  std::vector<uint32_t> sccVars_;
  std::vector<uint8_t> sccLoop_;        // per component: a loop (more than one unknown, or a self-loop)
  std::vector<uint32_t> localOf_;       // per unknown: its index in its component (loops only)
  std::vector<uint32_t> sccOf_;         // per unknown
  std::vector<uint32_t> srcVar_;        // per source channel
  std::vector<uint32_t> sinkVar_;       // per sink channel
  std::vector<double> valRe_, valIm_;    // (unknown × source channels) × kBatch
  std::vector<double> gainRe_, gainIm_;  // gain_ slots × kBatch (slot 0: z)
  std::vector<std::complex<double>> mat_;  // largest loop: k × k, then k × source channels
  // Each cell's rule arguments (Cell::argBase), gathered once per evaluate.
  std::vector<float> argValue_;
  std::vector<uint8_t> argKnown_, argSignal_;
  std::vector<double> cache_;  // EQ.1b: every cell's rule scratch, kept across one evaluate's calls
  uint8_t usedRates_ = 0;      // TX.group-oversample: bit r set — a cell runs at rate r
};

// DY.3 (docs/plans/DY.md "DY.3"): the STATIC TRANSFER y = f(x) of a memoryless circuit between a source and a sink —
// a waveshaper's curve, a compressor's gain computer (level dB in, reduction dB out) — from the graph alone, by the
// control evaluator LinearResponse uses (ControlEval: constants, params, set ports, `curve`, `channel` / `pack` and the
// pure kernels — the runtime's own 8-lane kernels, so the curve is the one the audio thread computes, bit for bit).
// The source's value is x on every channel (its node's inputs are cut, its other outputs unknown); the sink's value at
// each x is y. A sink the source does not reach is a constant curve.
//
// Status: Exact, or Unavailable — a node between the source and the sink that is not computed from its inputs here (a
// stateful atom: z1, a filter, a delay, `follow`; an audio input; a solve block) is on the path (prepare decides), or
// the sink depends on an unknown value (an LFO's output on a control: evaluate decides; a driven control given as a
// set port is held, as in LinearResponse), or the ends are invalid.
//
// Cost and threads: prepare() allocates (once per structural revision). evaluate() never allocates, locks or throws:
// the ops that do not depend on the source run once, the rest once per point (one pure kernel call per op and
// channel). One evaluator is not thread-safe.
class StaticTransfer : public ControlEval {
public:
  void prepare(const TypedPatch& typed, ResponseEnd source, ResponseEnd sink, std::span<const uint32_t> settable = {});
  ResponseKind status() const noexcept { return prepared_; }
  uint16_t sinkChannels() const noexcept { return sinkWidth_; }
  // y at each x: ys[k · sinkChannels() + j] = sink channel j at xs[k] (in the source's units: dB for a gain computer).
  // ys.size() ≥ xs.size() · sinkChannels(). Unavailable: ys filled with NaN. RT-safe.
  ResponseKind evaluate(std::span<const float> xs, double sampleRate, std::span<float> ys) noexcept;
  // Tests: evaluate() calls so far, and the ops one point re-runs (those that depend on the source).
  uint64_t evaluations() const noexcept { return evaluations_; }
  uint32_t perPointOps() const noexcept { return static_cast<uint32_t>(ops_.size()) - firstDependent_; }

private:
  ResponseKind prepared_ = ResponseKind::Unavailable;
  uint16_t sinkWidth_ = 1;
  uint32_t sinkPort_ = kNone;
  bool identity_ = false;         // source == sink
  uint32_t firstDependent_ = 0;   // ops_[firstDependent_ …] depend on the source
  uint64_t evaluations_ = 0;
};

// One-shot convenience (allocates: prepare + evaluate): H of `typed` from `source` to `sink` at `hz`, params at their
// defaults except `params` (pairs of Param node index and value). `out` is resized to hz.size() · source channels ·
// sink channels.
ResponseKind linearResponse(const TypedPatch& typed, ResponseEnd source, ResponseEnd sink, std::span<const double> hz,
                            double sampleRate, std::vector<std::complex<double>>& out,
                            std::span<const std::pair<NodeIndex, float>> params = {});

}  // namespace pq::graph
