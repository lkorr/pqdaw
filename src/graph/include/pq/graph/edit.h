#pragma once

// Editing operations on the patch IR for the circuit editor (T4.4, SPEC §7.3): connect / disconnect with the editor's
// feedback rule, add a node from a short spec, delete, rename (callers in the same file follow), const → param pin,
// make subcircuit. Pure functions over Patch / PatchFile; the views turn each into new text (print) and re-parse it
// with the previous parse, so NodeIds and hot-swap state keys come from the T1.x-text-ids diff, exactly as for a
// text edit. Not RT-safe (UI thread).
//
// Rules shared by the operations:
// - Generated names: the printer names an unnamed node it cannot inline `_n<digits>` (several consumers, no consumer).
//   Re-parsed, that is an ordinary binding name; isGeneratedName() lets the editor treat it as "no name" again
//   (anonymize), so a node that gets a single consumer goes back inline (`out = tanh(x)`, not `_n4 = tanh(x)`).
// - Feedback (PATCH_LANGUAGE §3): a cycle without a delay is an error in text. connect() therefore inserts an
//   explicit `z1` on a new wire that would close a delay-free cycle and reports it (the editor shows a warning), so
//   the text stays valid; the patch language's own rule is unchanged. A cycle through any delayed input (z1.x,
//   delay.x, phasor.hz: PortSpec::delayed) is ordinary feedback. Subcircuit inputs count as delay-free (conservative:
//   a loop through a subcircuit that delays internally gets one sample more than it needs; the z1 is visible and can
//   be deleted after a re-route).
// - Signatures: an edit that adds, removes or renames a declaration (in / out / param / export) of a patch changes the
//   signature its callers in the same file copied (SubcircuitRef); refreshCallers() rebuilds those call nodes with
//   the new signature, carrying wires by port name (renamed ports through the rename map). Imported callees are not
//   edited here.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pq/core/result.h"
#include "pq/graph/patch.h"
#include "pq/graph/patch_file.h"

namespace pq::graph::edit {

// `_n` followed by digits (the printer's generated names).
bool isGeneratedName(std::string_view name) noexcept;
// Clears every generated name (see above). Returns how many it cleared.
uint32_t anonymize(Patch& p);
// `base` if no node of p has that name, else base1, base2, … (base must be an identifier).
std::string uniqueName(const Patch& p, std::string_view base);
// Is `name` usable for a node of p (identifier, not a keyword or built-in name, unused except by `self`)? The error
// says why not.
Status checkName(const Patch& p, std::string_view name, NodeIndex self = kNone);

// ---- wires --------------------------------------------------------------------------------------------------------
// Does input `input` of node n delay its value by at least one sample (PortSpec::delayed)?
bool inputDelayed(const Patch& p, NodeIndex n, uint32_t input) noexcept;
// Would the wire from → to close a cycle with no delay in it? (O(nodes + wires))
bool closesDelayFreeCycle(const Patch& p, PortRef from, PortRef to);

struct ConnectResult {
  WireIndex wire = kNone;        // the new wire into `to` (from the z1 when one was inserted)
  NodeIndex delay = kNone;       // the inserted z1, or kNone
  bool replaced = false;         // `to` had a driver, which the new wire replaced (an input has one driver)
};
// Connects output `from` to input `to`. Refused: ports out of range, a declared bool output into a declared real input
// or the other way round (convert explicitly: gate_to_real, x > 0). A wire that closes a delay-free cycle runs through
// a new z1 (placed halfway between the two nodes when both are placed). Node indices stay valid (nodes are only added).
Result<ConnectResult> connect(Patch& p, PortRef from, PortRef to);
// Removes the wire into input `to` (false when it had none).
bool disconnectInput(Patch& p, PortRef to);

// ---- nodes --------------------------------------------------------------------------------------------------------
// A node from a short spec, as typed into the editor's "add" field:
//   a literal (`0.5`, `440Hz`, `-3dB`, `true`)          → a const
//   an atom name (`tanh`, `z1`, `voice.hz`, `mux`)       → that atom (variadic atoms with their minimum ports)
//   an operator (`+ - * / % < <= > >= == != && || ^^ !`) → add, sub, mul, … (`-x`: `neg`, `!`: `not`)
//   a callee's name (this file's patches, `callees`)      → a subcircuit
//   a declaration (`param cutoff = 1kHz [20Hz..20kHz]`, `in x : audio`, `out y : audio voice`, `export e`)
//                                                         → param / in / out / export, as in the language
// The node is unplaced and unconnected (a new declaration name must be free: checkName).
Result<NodeIndex> addNode(Patch& p, std::string_view spec, std::span<const SubcircuitRef> callees);

// A param declaration in place of const node c (its value is the default; same unit), driving everything c drove.
// `name` empty: a free name from the first input c drives (`cutoff`), else `p`. Returns the param's index (indices
// after c shift down by one).
Result<NodeIndex> constToParam(Patch& p, NodeIndex c, std::string name = {});

// Renames node n of file.patches[patch] (empty `name`: unnamed, allowed for computed nodes only). A declaration's new
// name is carried into the signature copies of its callers in the file (refreshCallers). Returns the patches changed.
Result<std::vector<size_t>> rename(PatchFile& file, size_t patch, NodeIndex n, std::string name);

// Erases nodes of file.patches[patch]; when a declaration goes, callers follow (refreshCallers). Returns the patches
// changed.
std::vector<size_t> eraseNodes(PatchFile& file, size_t patch, std::span<const NodeIndex> nodes);

// Brings every call to file.patches[callee] in the other patches of the file up to its current signature (no-op for
// calls that already match): a new call node with the new ports, the old one's name, layout and wires (by port name,
// `renamed` = {old, new} port names), then the old one erased. Wires into ports that no longer exist are dropped.
// Returns the patches changed.
std::vector<size_t> refreshCallers(PatchFile& file, size_t callee,
                                   std::span<const std::pair<std::string, std::string>> renamed = {});

struct MakeSubcircuitResult {
  size_t patch = 0;            // index of the new patch in file.patches (right after the edited one)
  NodeIndex instance = kNone;  // the call node in the edited patch
};
// Moves `nodes` of file.patches[patch] into a new patch `name` (empty: Sub1, Sub2, …) inserted after it, and puts a
// call of it in their place. Every value entering the selection becomes an `in` (a param of the parent becomes a
// `param` pin with its declaration, wired from the parent's param); every value leaving it becomes an `out`. Inside,
// the nodes keep their names and relative layout; the call sits where the selection was. Declarations, solve members
// and an empty selection are refused.
Result<MakeSubcircuitResult> makeSubcircuit(PatchFile& file, size_t patch, std::span<const NodeIndex> nodes,
                                            std::string name = {});

}  // namespace pq::graph::edit
