// Live probes through the compiler (T4.5): pq/graph/inspect.h.

#include "pq/graph/inspect.h"

#include "pq/dsp/kernel.h"
#include "pq/graph/schedule.h"

namespace pq::graph {

std::vector<ProbePoint> probePoints(const Patch& optimised, std::span<const uint32_t> sources,
                                    std::span<const uint32_t> valueMap, std::span<const uint32_t> ids) {
  std::vector<ProbePoint> out(sources.size());
  for (size_t k = 0; k < sources.size(); ++k) {
    out[k].id = k < ids.size() ? ids[k] : static_cast<uint32_t>(k);
    const uint32_t src = sources[k];
    if (src == kNone || src >= valueMap.size()) continue;
    const uint32_t port = valueMap[src];
    if (port == kNone || port >= optimised.portCount() || optimised.nodeCount() == 0) continue;
    // The node whose port range holds `port`: ports are contiguous per node, in node order (binary search).
    NodeIndex lo = 0, hi = optimised.nodeCount();
    while (hi - lo > 1) {
      const NodeIndex mid = lo + (hi - lo) / 2;
      if (optimised.node(mid).firstPort <= port) lo = mid;
      else hi = mid;
    }
    const Node& nd = optimised.node(lo);
    const uint32_t first = nd.firstPort + nd.numInputs;
    if (port < first || port >= first + nd.numOutputs) continue;
    out[k].node = lo;
    out[k].output = static_cast<uint16_t>(port - first);
  }
  return out;
}

float opCostWeight(const Schedule& s, uint32_t op) noexcept {
  if (op >= s.ops.size()) return 1.0f;
  const Op& o = s.ops[op];
  switch (o.kind) {
    case OpKind::Broadcast: return 1.0f;
    case OpKind::Reduce: return 2.0f;
    case OpKind::Send: return 1.0f;
    case OpKind::Kernel: break;
  }
  using dsp::Atom;
  switch (o.kernel) {
    case Atom::Div: case Atom::Sqrt: return 4.0f;
    case Atom::Mod: case Atom::Floor: case Atom::Frac: case Atom::Sign: case Atom::SqrtFast: case Atom::Gain:
    case Atom::GainAdd: return 2.0f;
    case Atom::ExpFast: case Atom::LogFast: case Atom::SinFast: case Atom::TanFast: case Atom::TanhFast:
    case Atom::DbToGainFast: case Atom::GainToDbFast: case Atom::CentsToHzFast: case Atom::HzToCentsFast:
    case Atom::SinePhase: case Atom::Shaper: return 8.0f;
    case Atom::PowFast: return 15.0f;
    case Atom::Exp: case Atom::Log: case Atom::Sin: case Atom::Tan: case Atom::Tanh: case Atom::DbToGain:
    case Atom::GainToDb: case Atom::CentsToHz: case Atom::HzToCents: case Atom::Pow: case Atom::TuningHz: return 20.0f;
    case Atom::Mux: case Atom::Demux: return static_cast<float>(o.numIn + o.numOut);
    case Atom::Schmitt: case Atom::Rise: case Atom::Fall: case Atom::Change: case Atom::SrFlipFlop:
    case Atom::DFlipFlop: case Atom::TFlipFlop: case Atom::Counter: case Atom::ClockDiv: case Atom::Pulse:
    case Atom::Timer: case Atom::Z1: case Atom::Z1Read: case Atom::Z1Write: case Atom::SampleHold: case Atom::Latch:
    case Atom::Phasor: return 3.0f;
    case Atom::Noise: return 4.0f;
    case Atom::DelayLinear: case Atom::DelayCubic: case Atom::DelayAllpass: case Atom::DelayReadLinear:
    case Atom::DelayReadCubic: case Atom::DelayReadAllpass: case Atom::DelayWrite: case Atom::SineHz:
    case Atom::Wavetable: case Atom::SampleReadLinear: case Atom::SampleReadCubic: case Atom::SamplePosLinear:
    case Atom::SamplePosCubic: return 10.0f;
    default: return 1.0f;
  }
}

}  // namespace pq::graph
