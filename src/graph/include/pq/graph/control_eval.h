#pragma once

// DY.3: the CONTROL EVALUATOR shared by graph::LinearResponse (EQ.2) and graph::StaticTransfer (DY.3)
// (pq/graph/response.h): the value of a port computed from the graph alone, no audio — constants, params (setParam),
// `sr`, the pure kernels computed from them (the runtime's own 8-lane kernels, graph's constant folding:
// detail::foldPureAtom), `channel` / `pack` of them, a `curve` atom through the curve the caller gives (setCurve), and
// SET PORTS whose value the caller gives (setValue; TX.mod-bands: a driven control's modulated value). Anything else (an
// audio input, a stateful atom's output, a solve block's unknown) is UNKNOWN, and so is everything computed from it.
//
// The ops are kept in dependency order; run(sampleRate, first) evaluates ops[first …] (a caller that varies one value
// puts the ops that do not depend on it first and re-runs only the rest: StaticTransfer). prepare-side helpers
// allocate (once per structural revision); run() and the setters never allocate, lock or throw.

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "pq/dsp/curve.h"
#include "pq/graph/infer.h"

namespace pq::graph {

// One end of a response / transfer: an output port (`input` false: output `port` of `node`) or the value arriving at an
// input (`input` true: input `port` of `node` — its driver; e.g. a root `out`'s input 0).
struct ResponseEnd {
  NodeIndex node = kNone;
  uint16_t port = 0;
  bool input = false;
};

class ControlEval {
public:
  // The Param nodes the evaluation depends on, in node order; setParam(i, …) sets paramNodes()[i]. Values in the
  // param's units, as the engine writes them (bools: 0 / 1). They start at their declared defaults.
  std::span<const NodeIndex> paramNodes() const noexcept { return paramNode_; }
  void setParam(uint32_t i, float value) noexcept { paramValue_[i] = value; }
  float param(uint32_t i) const noexcept { return paramValue_[i]; }
  // The `curve` atoms among the control nodes (the GU.1 knob curve of a driven control), in node order; setCurve(i, c)
  // makes curveNodes()[i] evaluate dsp::evalCurve(c, position) (evalCurveUnits when its `convert` input is 0). One
  // without a curve is unknown.
  std::span<const NodeIndex> curveNodes() const noexcept { return curveNode_; }
  void setCurve(uint32_t i, const dsp::Curve& c) noexcept {
    curve_[i] = c;
    curveSet_[i] = 1;
  }
  // TX.mod-bands: the set ports the evaluation reads (of prepare's `settable`); setValue(i, …) gives setPorts()[i] its
  // value (every channel). One never given a value is unknown.
  std::span<const uint32_t> setPorts() const noexcept { return setPort_; }
  void setValue(uint32_t i, float v) noexcept {
    setValue_[i] = v;
    setKnown_[i] = 1;
  }
  float value(uint32_t i) const noexcept { return setValue_[i]; }

protected:
  // A value source of an input: a port's control value / unknown, or a default constant. (`signal` / `toSamples`:
  // LinearResponse's cells.)
  struct InSrc {
    uint32_t port = kNone;  // flat output port of the driver; kNone: unconnected (reads `def`)
    float def = 0.0f;
    uint16_t width = 1;     // the driver's width (broadcast when 1)
    uint8_t signal = 0;     // the driver is signal
    uint8_t toSamples = 0;  // a duration input fed in seconds: · sampleRate
  };
  // TX.group-oversample: rates — r = log2 of the oversample region's factor a node runs at (0: the base rate; ×16 is 4).
  static constexpr uint32_t kRates = 5;

  // What prepare() works from (allocating): the typed patch's adjacency, each flat output port's node, the set ports'
  // nodes (setOut[n]: 1 + the set output; 0: none) and the emission state of the control DFS.
  struct Graph {
    explicit Graph(const TypedPatch& typed);
    // Marks the nodes of `settable` (flat output ports) as set ports' nodes, except `keep` (a response's source).
    void cut(std::span<const uint32_t> settable, NodeIndex keep);
    const TypedPatch& typed;
    const Patch& p;
    Adjacency adj;
    std::vector<NodeIndex> owner;   // per flat output port
    std::vector<uint16_t> setOut;   // per node
    std::vector<uint8_t> state;     // per node: 0 new, 1 open, 2 emitted (needControl)
    std::vector<std::pair<NodeIndex, uint32_t>> stack;
    // The flat output port driving input i of n (kNone: unconnected).
    uint32_t driverPort(NodeIndex n, uint32_t i) const noexcept;
    uint16_t width(uint32_t flatPort) const noexcept;
    // The default an unconnected input reads, in the kernels' domain (bools as masks).
    float inputDefault(NodeIndex n, uint32_t i) const noexcept;
    // Its rate (log2 of its oversample region's factor; the converters run at the base rate).
    uint8_t rateOf(NodeIndex n) const noexcept;
    // Whether n is computed from its inputs here (`curve`, `channel`, `pack`, a pure kernel; not a set port's node).
    bool computes(NodeIndex n) const noexcept;
    // An end as a flat output port (kNone: an unconnected input); `valid` false: out of range. `width`: its channels.
    uint32_t resolve(ResponseEnd e, bool& valid, uint16_t& width) const noexcept;
    // SIGNAL: the ports that depend on flat output port `src` (forward from it; its node's inputs cut; a set port's node
    // stops it: its value is given). sig: per flat output port.
    void signalFrom(uint32_t src, std::vector<uint8_t>& sig) const;
    // RELEVANT: the signal ports `sink` reads through signal ports (back from it, not past `src`'s node). rel: per flat
    // output port; relNode: per node.
    void relevantTo(uint32_t sink, uint32_t src, const std::vector<uint8_t>& sig, std::vector<uint8_t>& rel,
                    std::vector<uint8_t>& relNode) const;
  };

  // Clears the controls and sizes the per-port tables for g.
  void beginControls(const Graph& g);
  // Emits `root` and, if it computes, what it is computed from (depth first: dependencies first). `source` (a flat
  // output port, or kNone): the varied value of a StaticTransfer — its node is emitted as OpKind::Source.
  void needControl(Graph& g, NodeIndex root, uint32_t source = kNone);
  // After the last needControl: sizes the curve / set value tables.
  void endControls();
  bool readControl(const InSrc& s, uint32_t channel, float& v) const noexcept;
  // Evaluates ops_[first …] in order; Source writes `sourceValue` on its port (every channel).
  void runControl(double sampleRate, uint32_t first = 0, float sourceValue = 0.0f) noexcept;
  // Whether flat output port f has a control value (it is a control op's output) and it is known after the last run.
  bool controlKnown(uint32_t f) const noexcept { return ctrlOff_[f] != kNone && ctrlKnown_[f] != 0; }
  float controlValue(uint32_t f, uint32_t channel) const noexcept { return ctrlVal_[ctrlOff_[f] + channel]; }

  enum class OpKind : uint8_t { Const, Param, Sr, Curve, Channel, Pack, Fold, Set, Source, Unknown };
  struct CtrlOp {
    OpKind kind = OpKind::Unknown;
    AtomId atom = 0;
    NodeIndex node = kNone;
    uint32_t inBegin = 0, inCount = 0;    // into ctrlIns_
    uint32_t outBegin = 0, outCount = 0;  // into ctrlOuts_ (flat output ports)
    uint16_t width = 1;                   // channels of every output
    uint32_t index = 0;                   // Param: param index; Curve: curve index; Set: set port index
    uint16_t setOutput = 0;               // Set / Source: which output carries the value
    float constant = 0.0f;
    uint8_t rate = 0;                     // TX.group-oversample: its rate (Sr: 2^rate · sr)
  };

  // Control values: per flat output port, an offset into ctrlVal_ (kNone: none) and a known flag.
  std::vector<uint32_t> ctrlOff_;
  std::vector<float> ctrlVal_;
  std::vector<uint8_t> ctrlKnown_;  // per flat output port
  std::vector<CtrlOp> ops_;         // in dependency order
  std::vector<uint32_t> ctrlOuts_;
  std::vector<InSrc> ctrlIns_;
  std::vector<NodeIndex> paramNode_;
  std::vector<float> paramValue_;
  std::vector<NodeIndex> curveNode_;
  std::vector<dsp::Curve> curve_;
  std::vector<uint8_t> curveSet_;
  std::vector<uint32_t> setPort_;  // TX.mod-bands
  std::vector<float> setValue_;
  std::vector<uint8_t> setKnown_;
};

}  // namespace pq::graph
