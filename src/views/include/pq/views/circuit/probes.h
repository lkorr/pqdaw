#pragma once

// Live probes and cost meters in the circuit editor (T4.5, SPEC §7.3 / §7.6 / §11.1), the views' half. The engine side
// is pq/engine/inspect.h (taps, rdtsc per region), the compiler side pq/graph/inspect.h (which value a probe reads).
//
//   probes      hovering a wire (≈ 150 ms dwell) compiles a probe on its source into the playing program (a recompile:
//               the new image is bit-identical, so the swap is inaudible; the JIT image follows in the background) and
//               shows a readout at the pointer: the value (bools: HIGH / LOW) and a mini-scope of the last ~21 ms from
//               the probe's TapRing. The probe stays compiled ~1.5 s after the pointer leaves, so moving between
//               wires does not recompile on every pass.
//   LED trace   while a patch plays, every bool net of the shown patch is probed for its level only (ProbeLevel: no
//               signal copy) and its wires are drawn lit (circuit.wireBoolLit) in any block the value was high, polled
//               at 30 Hz.
//   cost        the title bar's Cost toggle turns the player's cost meter on (nothing is measured while it is off) and
//               labels every node of the shown patch with its ns per sample, a subcircuit with its whole instance, and
//               a wire with the cost of what it implies (an inserted voice → global sum, a global → voice copy), polled
//               at 4 Hz and smoothed; the status line shows the program's total. The engine times regions; one region's
//               time is split among its ops by graph::opCostWeight (an estimate where a region mixes subcircuits).
//
// UI thread only.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pq/graph/patch.h"
#include "pq/views/circuit/patch_editor.h"
#include "pq/views/circuit/patch_player.h"

namespace pq::views::circuit {

// The cost of the shown patch's parts, ns per sample frame, from one reading of the playing program.
struct CostMap {
  double totalNs = 0.0;     // the whole program (stages, voices, the host's own work)
  double shownNs = 0.0;     // what is inside the shown instance (the sum of the entries below)
  struct Entry {
    graph::NodeId node;
    double ns = 0.0;
  };
  std::vector<Entry> nodes;       // per node of the shown patch (a subcircuit call: its whole instance)
  std::vector<Entry> sumsInto;    // inserted voice → global sums on the wires into this node
  std::vector<Entry> copiesFrom;  // global → voice copies of this node's value
};

// Splits `reading` (per region, of `played`'s schedule) among the ops of each timing unit by graph::opCostWeight and
// sums them per node of the instance at `path` (call NodeIds from the played patch). False when the reading is not of
// `played` (another generation, another region count) or the path names no instance.
bool attributeCost(const PatchEditor::PlayedProgram& played, const PatchPlayer::CostReading& reading,
                   std::span<const graph::NodeId> path, CostMap& out);

// What the edit canvas draws over the scene for probes and cost (filled by the editor, painted by EditCanvas).
struct ProbeOverlay {
  // The hovered wire's probe.
  bool readout = false;
  uint32_t wire = UINT32_MAX;     // scene wire
  bool isBool = false;
  bool hasData = false;           // the probe is compiled in and data has arrived
  std::string source;             // what it reads: the source's name ("next", "svf_core.low", "tanh")
  std::string value;              // "0.5124", "HIGH", "—"
  std::vector<float> samples;     // the most recent samples, oldest first (bools: lane masks)
  // Cost labels.
  struct Label {
    uint32_t item = UINT32_MAX;   // scene node or wire
    std::string text;
    float heat = 0.0f;            // relative to the costliest labelled part, 0..1 (label emphasis)
  };
  std::vector<Label> nodeLabels, wireLabels;
};

// "0.0123", "-1.250", "1234", "1.2e+06", "nan": four significant digits, for readouts.
std::string formatProbeValue(float v);
// "0.42 ns", "12.3 ns", "1.21 µs".
std::string formatCostNs(double ns);

}  // namespace pq::views::circuit
