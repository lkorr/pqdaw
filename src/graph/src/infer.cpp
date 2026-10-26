// Type inference (T1.3). See pq/graph/infer.h for the semantics. Three phases over one flat working graph:
//
//   1. flatten: instantiate the root and, recursively, every subcircuit call into FNodes. A callee's `in`, bound
//      `param`, `out` and `export` nodes become Identity FNodes that carry the declared bound of that boundary, so
//      bound checks and voice→global reduction happen exactly once per boundary (not once per consumer).
//   2. infer: a worklist fixpoint over the FNodes, seeded in topological order (drivers first), joining each output
//      upwards in the rate/poly/width/kind lattices; then the bound checks.
//      Then the voice tunings (T2.6): a `tune_voices(hz, cents)` node rebinds voice.hz (and, with `cents`
//      connected, voice.cents) for every subcircuit instantiated below its patch, so in-ports reading those
//      voice sources are re-driven by the binder's inputs (the nearest binding ancestor wins; the binder's own
//      patch still reads the engine's values, so `tuning_hz(…, voice.hz, …)` there is the fallback, not a cycle).
//   3. emit: build the flattened Patch. Identities are wired through (or become voice_sum nodes where they reduce
//      voices), root outputs with a global bound get a voice_sum in front, bool eq/ne become xor (+ not), and every
//      port gets its inferred type.
//
// Everything is flat arrays indexed by FNode / port number; no recursion except the bounded instantiation
// (kMaxSubcircuitDepth).

#include "pq/graph/infer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace pq::graph {

void PatchLibrary::add(const Patch& p) { byName_.emplace(p.name(), &p); }

void PatchLibrary::add(const PatchFile& f) {
  for (const Patch& p : f.patches) add(p);
}

void PatchLibrary::addAll(const PatchLibrary& other) {
  for (const auto& [name, patch] : other.byName_) byName_.emplace(name, patch);
}

const Patch* PatchLibrary::find(std::string_view name) const noexcept {
  const auto it = byName_.find(name);
  return it == byName_.end() ? nullptr : it->second;
}

namespace {

constexpr uint64_t kRootKey = 0x6A09E667F3BCC908ull;

uint64_t hashMix(uint64_t h, uint64_t v) noexcept {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 31;
  h *= 0x94D049BB133111EBull;
  h ^= h >> 29;
  return h;
}

// ---- lattice helpers ----------------------------------------------------------------------------------------------

Rate maxRate(Rate a, Rate b) noexcept { return a < b ? b : a; }
Poly maxPoly(Poly a, Poly b) noexcept { return a < b ? b : a; }

// Width join: 0 = unknown, 1 = mono (broadcasts), N > 1 must agree. Returns false on a conflict (a is kept).
bool joinWidth(uint16_t& a, uint16_t b) noexcept {
  if (b == kWidthInfer || b == a) return true;
  if (b == kWidthMono) {
    if (a == kWidthInfer) a = kWidthMono;
    return true;
  }
  if (a <= kWidthMono) {
    a = b;
    return true;
  }
  return false;
}

// Kind join: Infer joins anything; Real and Bool conflict. Returns false on a conflict (a is kept).
bool joinKind(Kind& a, Kind b) noexcept {
  if (b == Kind::Infer || b == a) return true;
  if (a == Kind::Infer) {
    a = b;
    return true;
  }
  return false;
}

std::string widthText(uint16_t w) {
  if (w == kWidthMono) return "mono";
  if (w == kWidthStereo) return "stereo";
  return "ch(" + std::to_string(w) + ")";
}

// ---- working graph ------------------------------------------------------------------------------------------------

enum class FKind : uint8_t { Atom, Const, Param, Input, Output, Export, Unknown, Equation, Identity };

struct FNode {
  FKind kind = FKind::Atom;
  NodeKind srcKind = NodeKind::Atom;  // for Identity: the boundary node it stands for (Input/Param/Output/Export)
  AtomId atom = 0;
  uint16_t nIn = 0, nOut = 0;
  uint32_t inst = 0;
  NodeIndex src = kNone;              // node in instances[inst].patch
  uint32_t firstIn = 0, firstOut = 0;
  uint32_t solve = kNone;             // flat solve block (Unknown/Equation)
  bool shared = false;                // Atom: Node::shared (`shared(…)`, T1.3-fix1)
};

struct CalleeInfo {
  Adjacency adj;
  std::vector<NodeIndex> inPorts;   // subcircuitSignature order: ins, then params
  std::vector<NodeIndex> outPorts;  // outs, then exports
  std::vector<uint32_t> portOf;     // node → index in inPorts (kNone if not an in/param)
};

struct Inst {
  const Patch* patch = nullptr;
  const CalleeInfo* info = nullptr;
  uint32_t parent = kNone;
  NodeIndex callNode = kNone;
  uint64_t key = kRootKey;
  uint32_t gbase = 0;       // first global node id of this instance
  uint32_t solveBase = 0;   // first flat solve block
  uint32_t depth = 0;
  std::string prefix;       // name prefix of its nodes ("" for the root)
  NodeIndex binder = kNone; // its tune_voices node (T2.6), if any
};

class Inferrer {
public:
  Inferrer(const Patch& root, const InferOptions& o) : root_(root), opt_(o) {}

  InferResult run() {
    InferResult r;
    flatten();
    if (diags_.empty()) {
      solveFixpoint();
      normalise();
      check();
    }
    if (diags_.empty()) {
      emit(r.typed);
      locateProbes(r);
    }
    r.diagnostics = std::move(diags_);
    return r;
  }

private:
  // ---- diagnostics ----------------------------------------------------------------------------------------------

  std::string describe(uint32_t inst, NodeIndex n) const {
    const Inst& I = insts_[inst];
    std::string s = I.patch->describe(n);
    if (inst != 0)
      s += " inside '" + I.patch->name() + "' called by " + describe(I.parent, I.callNode);
    return s;
  }

  // The root-patch node a problem at (inst, node) is reported at: the outermost call site.
  NodeIndex rootNode(uint32_t inst, NodeIndex n) const {
    while (inst != 0) {
      n = insts_[inst].callNode;
      inst = insts_[inst].parent;
    }
    return n;
  }

  void report(uint32_t inst, NodeIndex n, ErrorCode code, std::string msg) {
    if (diags_.size() >= kMaxDiagnostics) return;
    msg = "patch '" + root_.name() + "': " + msg;
    const NodeIndex rn = rootNode(inst, n);
    const SourceText& src = opt_.source;
    if (!src.text.empty()) {
      SourceSpan span;
      if (rn < src.nodeSpans.size()) span = src.nodeSpans[rn];
      diags_.push_back(makeDiagnostic(src.text, src.fileName, span, std::move(msg), code));
    } else {
      Diagnostic d;
      d.message = std::move(msg);
      d.code = code;
      diags_.push_back(std::move(d));
    }
  }

  // Report a problem with FNode f once (conflicts can be found again on every re-evaluation).
  void reportNode(uint32_t f, ErrorCode code, std::string msg) {
    if (failed_[f]) return;
    failed_[f] = 1;
    const FNode& fn = fnodes_[f];
    report(fn.inst, fn.src, code, std::move(msg));
  }

  // "input 'x' of <node>" for in-port `port` of FNode f; for a callee `in`/`param`, the call site's input.
  std::string inPortText(uint32_t f, uint32_t i) const {
    const FNode& fn = fnodes_[f];
    const Inst& I = insts_[fn.inst];
    if (fn.kind == FKind::Identity && (fn.srcKind == NodeKind::Input || fn.srcKind == NodeKind::Param) && fn.inst != 0)
      return "input '" + I.patch->nodeName(fn.src) + "' of " + describe(I.parent, I.callNode);
    if (fn.kind == FKind::Identity || fn.kind == FKind::Output || fn.kind == FKind::Export)
      return describe(fn.inst, fn.src);
    return "input '" + std::string(I.patch->inputName(fn.src, i)) + "' of " + describe(fn.inst, fn.src);
  }

  // Location of a problem on an in-port: a callee's `in`/`param` is wired at the call site.
  void reportPort(uint32_t f, ErrorCode code, std::string msg) {
    if (failed_[f]) return;
    failed_[f] = 1;
    const FNode& fn = fnodes_[f];
    if (fn.kind == FKind::Identity && (fn.srcKind == NodeKind::Input || fn.srcKind == NodeKind::Param) && fn.inst != 0)
      report(insts_[fn.inst].parent, insts_[fn.inst].callNode, code, std::move(msg));
    else
      report(fn.inst, fn.src, code, std::move(msg));
  }

  // ---- phase 1: flatten -----------------------------------------------------------------------------------------

  const CalleeInfo& calleeInfo(const Patch& p) {
    auto it = callees_.find(&p);
    if (it != callees_.end()) return it->second;
    CalleeInfo& ci = callees_[&p];
    ci.adj = p.adjacency();
    ci.portOf.assign(p.nodeCount(), kNone);
    for (NodeKind k : {NodeKind::Input, NodeKind::Param})
      for (NodeIndex n = 0; n < p.nodeCount(); ++n)
        if (p.node(n).kind == k) {
          ci.portOf[n] = static_cast<uint32_t>(ci.inPorts.size());
          ci.inPorts.push_back(n);
        }
    for (NodeKind k : {NodeKind::Output, NodeKind::Export})
      for (NodeIndex n = 0; n < p.nodeCount(); ++n)
        if (p.node(n).kind == k) ci.outPorts.push_back(n);
    return ci;
  }

  uint32_t newNode(FKind kind, uint32_t inst, NodeIndex src, uint32_t nIn, uint32_t nOut) {
    FNode f;
    f.kind = kind;
    f.inst = inst;
    f.src = src;
    f.nIn = static_cast<uint16_t>(nIn);
    f.nOut = static_cast<uint16_t>(nOut);
    f.firstIn = static_cast<uint32_t>(inBound_.size());
    f.firstOut = static_cast<uint32_t>(outDecl_.size());
    inBound_.resize(inBound_.size() + nIn);
    inDrv_.resize(inDrv_.size() + nIn, kNone);
    outDecl_.resize(outDecl_.size() + nOut);
    outOwner_.resize(outOwner_.size() + nOut, static_cast<uint32_t>(fnodes_.size()));
    fnodes_.push_back(f);
    return static_cast<uint32_t>(fnodes_.size() - 1);
  }

  uint32_t newIdentity(uint32_t inst, NodeIndex src, NodeKind srcKind, const PortType& bound) {
    const uint32_t f = newNode(FKind::Identity, inst, src, 1, 1);
    fnodes_[f].srcKind = srcKind;
    inBound_[fnodes_[f].firstIn] = bound;
    return f;
  }

  // Is input k of the call that created `inst` connected at the call site?
  bool callInputConnected(uint32_t inst, uint32_t k) const {
    const Inst& I = insts_[inst];
    const Inst& P = insts_[I.parent];
    return P.info->adj.driver[P.patch->flatInput(I.callNode, k)] != kNone;
  }

  void flatten() {
    Inst root;
    root.patch = &root_;
    root.info = &calleeInfo(root_);
    insts_.push_back(std::move(root));
    active_.push_back(&root_);
    // Capacity for the root alone (the common case of a flat patch); instances grow the arrays from there.
    fnodes_.reserve(root_.nodeCount());
    inBound_.reserve(root_.portCount());
    inDrv_.reserve(root_.portCount());
    outDecl_.reserve(root_.portCount());
    outOwner_.reserve(root_.portCount());
    instantiate(0);
    failed_.assign(fnodes_.size(), 0);
    bindVoiceTunings();
  }

  // T2.6: re-drive voice.hz / voice.cents reads under a tune_voices binder (see the file comment).
  void bindVoiceTunings() {
    if (voiceReads_.empty()) return;
    std::vector<uint32_t> repl(outDecl_.size(), kNone);
    bool any = false;
    for (const auto& [f, cents] : voiceReads_) {
      const FNode& fn = fnodes_[f];
      for (uint32_t a = insts_[fn.inst].parent; a != kNone; a = insts_[a].parent) {
        const Inst& A = insts_[a];
        if (A.binder == kNone) continue;
        const uint32_t bf = gF_[A.gbase + A.binder];
        if (bf != kNone) {
          const uint32_t drv = inDrv_[fnodes_[bf].firstIn + (cents ? 1u : 0u)];
          if (drv != kNone) {
            repl[fn.firstOut] = drv;
            any = true;
          }
        }
        break;  // the nearest binder decides (an unconnected `cents` leaves voice.cents to the engine)
      }
    }
    if (!any) return;
    // A binder's input may itself read a rebound voice source (nested binders): follow the chain. Each step goes
    // to a strictly outer binder, so a chain is at most kMaxSubcircuitDepth long.
    for (uint32_t& d : inDrv_) {
      for (uint32_t steps = 0; d != kNone && repl[d] != kNone && steps <= kMaxSubcircuitDepth; ++steps) d = repl[d];
    }
  }

  void instantiate(uint32_t ii) {
    const Patch& P = *insts_[ii].patch;
    const CalleeInfo& ci = *insts_[ii].info;
    const bool isRoot = ii == 0;
    const uint32_t base = static_cast<uint32_t>(gF_.size());
    insts_[ii].gbase = base;
    insts_[ii].solveBase = static_cast<uint32_t>(solves_.size());
    for (const Solve& s : P.solves()) solves_.push_back(s.iterations);
    gF_.resize(base + P.nodeCount(), kNone);
    gChild_.resize(base + P.nodeCount(), kNone);

    uint32_t rootInputIndex = 0;
    for (NodeIndex n = 0; n < P.nodeCount(); ++n) {
      const Node& nd = P.node(n);
      uint32_t f = kNone;
      switch (nd.kind) {
        case NodeKind::Atom:
        case NodeKind::Const:
        case NodeKind::Unknown:
        case NodeKind::Equation: {
          const FKind k = nd.kind == NodeKind::Atom      ? FKind::Atom
                          : nd.kind == NodeKind::Const   ? FKind::Const
                          : nd.kind == NodeKind::Unknown ? FKind::Unknown
                                                         : FKind::Equation;
          f = newNode(k, ii, n, nd.numInputs, nd.numOutputs);
          FNode& fn = fnodes_[f];
          fn.atom = nd.atom;
          fn.shared = k == FKind::Atom && nd.shared;
          if (k == FKind::Atom && !isRoot && (nd.atom == atomVoiceHz_ || nd.atom == atomVoiceCents_))
            voiceReads_.emplace_back(f, nd.atom == atomVoiceCents_);
          if (k == FKind::Atom && nd.atom == atomTuneVoices_) {
            if (insts_[ii].binder != kNone)
              report(ii, n, ErrorCode::InvalidArgument,
                     "a patch can bind the voice tuning (tune_voices) only once: " + describe(ii, n));
            else
              insts_[ii].binder = n;
          }
          if (k == FKind::Unknown || k == FKind::Equation) fn.solve = insts_[ii].solveBase + nd.payload;
          for (uint32_t i = 0; i < nd.numInputs; ++i) inBound_[fn.firstIn + i] = P.inputType(n, i);
          for (uint32_t i = 0; i < nd.numOutputs; ++i) outDecl_[fn.firstOut + i] = P.outputType(n, i);
          break;
        }
        case NodeKind::Param: {
          if (isRoot || !callInputConnected(ii, ci.portOf[n])) {
            f = newNode(FKind::Param, ii, n, 0, 1);
            outDecl_[fnodes_[f].firstOut] = P.outputType(n, 0);
          } else {
            // Bound at the call site: a modulation target. Any rate and poly; kind and width are bounded.
            f = newIdentity(ii, n, NodeKind::Param, portType(P.paramDecl(n).kind, Rate::Infer, Poly::Infer, kWidthMono));
          }
          break;
        }
        case NodeKind::Input: {
          const PortType decl = P.outputType(n, 0);
          if (isRoot) {
            const RootInput ri = rootInputIndex < opt_.inputs.size() ? opt_.inputs[rootInputIndex] : RootInput{};
            ++rootInputIndex;
            if (ri.connected) {
              f = newNode(FKind::Input, ii, n, 0, 1);
              outDecl_[fnodes_[f].firstOut] = rootInputType(n, decl, ri.type);
            } else {
              f = newIdentity(ii, n, NodeKind::Input, decl);
            }
          } else {
            f = newIdentity(ii, n, NodeKind::Input, decl);
          }
          break;
        }
        case NodeKind::Output:
          if (isRoot) {
            f = newNode(FKind::Output, ii, n, 1, 0);
            inBound_[fnodes_[f].firstIn] = P.inputType(n, 0);
          } else {
            f = newIdentity(ii, n, NodeKind::Output, P.inputType(n, 0));
          }
          break;
        case NodeKind::Export:
          if (isRoot) f = newNode(FKind::Export, ii, n, 1, 0);
          else f = newIdentity(ii, n, NodeKind::Export, PortType{});
          break;
        case NodeKind::Subcircuit:
          gChild_[base + n] = instantiateCall(ii, n);
          break;
      }
      gF_[base + n] = f;
    }

    // Wires. Children are complete, so a wire into a call reaches the callee's boundary node directly, and a wire
    // out of a call starts at the callee's out/export boundary node.
    for (const Wire& w : P.wires()) {
      if (w.from.node >= P.nodeCount() || w.to.node >= P.nodeCount()) continue;  // validation reports these
      uint32_t inPort = kNone;
      const Node& dst = P.node(w.to.node);
      if (dst.kind == NodeKind::Subcircuit) {
        const uint32_t child = gChild_[base + w.to.node];
        if (child == kNone) continue;
        const Inst& C = insts_[child];
        if (w.to.port >= C.info->inPorts.size()) continue;
        const uint32_t f = gF_[C.gbase + C.info->inPorts[w.to.port]];
        inPort = fnodes_[f].firstIn;
      } else {
        const uint32_t f = gF_[base + w.to.node];
        if (f == kNone || w.to.port >= fnodes_[f].nIn) continue;  // e.g. a connected root input's default
        if (fnodes_[f].kind == FKind::Identity && dst.kind == NodeKind::Input && !isRoot &&
            callInputConnected(ii, ci.portOf[w.to.node]))
          continue;  // the call site's value replaces the default
        inPort = fnodes_[f].firstIn + w.to.port;
      }
      const uint32_t src = sourceOf(base + w.from.node, w.from.port);
      if (src != kNone && inDrv_[inPort] == kNone) inDrv_[inPort] = src;
    }
  }

  // The out-port that output `port` of global node g provides.
  uint32_t sourceOf(uint32_t g, uint32_t port) const {
    const uint32_t child = gChild_[g];
    if (child != kNone) {
      const Inst& C = insts_[child];
      if (port >= C.info->outPorts.size()) return kNone;
      return fnodes_[gF_[C.gbase + C.info->outPorts[port]]].firstOut;
    }
    const uint32_t f = gF_[g];
    if (f == kNone || port >= fnodes_[f].nOut) return kNone;
    return fnodes_[f].firstOut + port;
  }

  PortType rootInputType(NodeIndex n, const PortType& decl, const PortType& actual) {
    PortType t;
    t.kind = decl.kind;
    t.rate = actual.rate != Rate::Infer ? actual.rate : decl.rate != Rate::Infer ? decl.rate : Rate::Audio;
    t.poly = actual.poly != Poly::Infer ? actual.poly : decl.poly != Poly::Infer ? decl.poly : Poly::Global;
    t.width = actual.width != kWidthInfer ? actual.width : decl.width != kWidthInfer ? decl.width : kWidthMono;
    const std::string what = "the engine's connection to " + root_.describe(n);
    if (actual.kind != Kind::Infer && actual.kind != decl.kind)
      report(0, n, ErrorCode::Type, "kind mismatch: " + std::string(toText(actual.kind)) + " into " +
                                        std::string(toText(decl.kind)) + ": " + what);
    if (decl.rate != Rate::Infer && t.rate > decl.rate)
      report(0, n, ErrorCode::Type, "rate " + std::string(toText(t.rate)) + " exceeds the declared bound " +
                                        std::string(toText(decl.rate)) + ": " + what);
    if (decl.poly == Poly::Global && t.poly == Poly::Voice)
      report(0, n, ErrorCode::Type, "a per-voice signal into a global input: " + what);
    if (decl.width != kWidthInfer && t.width != kWidthMono && t.width != decl.width)
      report(0, n, ErrorCode::Type, "width mismatch: " + widthText(t.width) + " into " + widthText(decl.width) + ": " +
                                        what);
    return t;
  }

  uint32_t instantiateCall(uint32_t parent, NodeIndex n) {
    const Patch& P = *insts_[parent].patch;
    const SubcircuitRef& ref = P.subcircuit(n);
    const Patch* callee = opt_.library ? opt_.library->find(ref.patch) : nullptr;
    if (!callee) {
      report(parent, n, ErrorCode::NotFound,
             "unknown subcircuit '" + ref.patch + "' (not in the patch library): " + describe(parent, n));
      return kNone;
    }
    if (std::find(active_.begin(), active_.end(), callee) != active_.end()) {
      report(parent, n, ErrorCode::InvalidArgument,
             "'" + ref.patch + "' instantiates itself (recursion): " + describe(parent, n));
      return kNone;
    }
    if (insts_[parent].depth + 1 > kMaxSubcircuitDepth) {
      report(parent, n, ErrorCode::Overflow,
             "subcircuits nest deeper than " + std::to_string(kMaxSubcircuitDepth) + " levels: " + describe(parent, n));
      return kNone;
    }
    const CalleeInfo& ci = calleeInfo(*callee);
    // The call node carries a copy of the callee's signature (patch_file.h); it must still match by name.
    bool match = ref.inputs.size() == ci.inPorts.size() && ref.outputs.size() == ci.outPorts.size();
    for (size_t k = 0; match && k < ref.inputs.size(); ++k) match = ref.inputs[k].name == callee->nodeName(ci.inPorts[k]);
    for (size_t k = 0; match && k < ref.outputs.size(); ++k)
      match = ref.outputs[k].name == callee->nodeName(ci.outPorts[k]);
    if (!match || P.node(n).numInputs != ref.inputs.size() || P.node(n).numOutputs != ref.outputs.size()) {
      report(parent, n, ErrorCode::Type,
             "the call's ports no longer match the signature of '" + ref.patch + "': " + describe(parent, n));
      return kNone;
    }
    Inst c;
    c.patch = callee;
    c.info = &ci;
    c.parent = parent;
    c.callNode = n;
    c.key = hashMix(insts_[parent].key, P.node(n).id.value);
    c.depth = insts_[parent].depth + 1;
    const std::string& callName = P.nodeName(n);
    std::string local = callName.empty() ? callee->name() + "_" + std::to_string(n) : callName;
    c.prefix = insts_[parent].prefix.empty() ? std::move(local) : insts_[parent].prefix + "__" + local;
    const auto ci_ = static_cast<uint32_t>(insts_.size());
    insts_.push_back(std::move(c));
    active_.push_back(callee);
    instantiate(ci_);
    active_.pop_back();
    return ci_;
  }

  // ---- phase 2: fixpoint ----------------------------------------------------------------------------------------

  // The type arriving at in-port p (a voice signal into a global bound is reduced).
  PortType inType(uint32_t p) const {
    const PortType& b = inBound_[p];
    const uint32_t d = inDrv_[p];
    if (d == kNone) return portType(b.kind, Rate::Const, Poly::Global, kWidthMono);
    PortType t = outT_[d];
    if (b.poly == Poly::Global && t.poly == Poly::Voice) t.poly = Poly::Global;
    return t;
  }

  // Joins `t` into out-port o; true if it changed.
  bool setOut(uint32_t f, uint32_t o, PortType t, bool joinWidthWithOld = true) {
    PortType& cur = outT_[o];
    PortType next = cur;
    next.rate = maxRate(cur.rate, t.rate);
    next.poly = maxPoly(cur.poly, t.poly);
    if (!joinKind(next.kind, t.kind))
      reportNode(f, ErrorCode::Type, "kind conflict: " + describe(fnodes_[f].inst, fnodes_[f].src) +
                                         " would be both real and bool");
    if (joinWidthWithOld) {
      if (!joinWidth(next.width, t.width))
        reportNode(f, ErrorCode::Type, "width conflict: " + describe(fnodes_[f].inst, fnodes_[f].src) +
                                           " would be both " + widthText(next.width) + " and " + widthText(t.width));
    } else {
      next.width = std::max(next.width, t.width);
    }
    if (next == cur) return false;
    cur = next;
    return true;
  }

  // Re-evaluates FNode f; true if any of its outputs changed.
  bool eval(uint32_t f) {
    const FNode& fn = fnodes_[f];
    switch (fn.kind) {
      case FKind::Const: {
        const Constant& c = insts_[fn.inst].patch->constant(fn.src);
        return setOut(f, fn.firstOut, portType(c.kind, Rate::Const, Poly::Global, kWidthMono));
      }
      case FKind::Param:
        return setOut(f, fn.firstOut,
                      portType(insts_[fn.inst].patch->paramDecl(fn.src).kind, Rate::Param, Poly::Global, kWidthMono));
      case FKind::Input: return setOut(f, fn.firstOut, outDecl_[fn.firstOut]);
      case FKind::Output:
      case FKind::Export: return false;
      case FKind::Identity: {
        if (inDrv_[fn.firstIn] == kNone) return false;
        PortType t = inType(fn.firstIn);
        if (t.kind == Kind::Infer) t.kind = inBound_[fn.firstIn].kind;
        return setOut(f, fn.firstOut, t);
      }
      case FKind::Equation: {
        PortType j{};
        for (uint32_t i = 0; i < fn.nIn; ++i) {
          const PortType t = inType(fn.firstIn + i);
          j.rate = maxRate(j.rate, t.rate);
          j.poly = maxPoly(j.poly, t.poly);
          if (!joinWidth(j.width, t.width))
            reportNode(f, ErrorCode::Type, "width conflict between the sides of " + describe(fn.inst, fn.src));
        }
        PortType& b = solveType_[fn.solve];
        PortType nb = b;
        nb.rate = maxRate(b.rate, j.rate);
        nb.poly = maxPoly(b.poly, j.poly);
        if (!joinWidth(nb.width, j.width))
          reportNode(f, ErrorCode::Type, "width conflict inside the solve block of " + describe(fn.inst, fn.src));
        if (nb == b) return false;
        b = nb;
        for (uint32_t u : solveUnknowns_[fn.solve]) push(u);
        return false;  // no outputs of its own
      }
      case FKind::Unknown: {
        PortType t = solveType_[fn.solve];
        const PortType init = inType(fn.firstIn);
        if (inDrv_[fn.firstIn] != kNone) {
          t.rate = maxRate(t.rate, init.rate);
          t.poly = maxPoly(t.poly, init.poly);
          if (!joinWidth(t.width, init.width))
            reportNode(f, ErrorCode::Type, "width conflict between the initial guess and the equations of " +
                                               describe(fn.inst, fn.src));
        }
        t.kind = Kind::Real;
        return setOut(f, fn.firstOut, t);
      }
      case FKind::Atom: return evalAtom(f);
    }
    return false;
  }

  bool evalAtom(uint32_t f) {
    const FNode& fn = fnodes_[f];
    const AtomSignature& sig = atomSignature(fn.atom);
    const bool pack = fn.atom == atomPack_;
    Rate rate = Rate::Infer;
    Poly poly = Poly::Infer;
    Kind kind = Kind::Infer;
    uint16_t width = kWidthInfer;
    uint32_t packWidth = 0;
    for (uint32_t i = 0; i < fn.nIn; ++i) {
      const uint32_t p = fn.firstIn + i;
      const PortType t = inType(p);
      rate = maxRate(rate, t.rate);
      poly = maxPoly(poly, t.poly);
      if (pack) {
        packWidth += std::max<uint32_t>(t.width, 1);
      } else if (!joinWidth(width, t.width)) {
        reportNode(f, ErrorCode::Type, "width mismatch: " + widthText(width) + " and " + widthText(t.width) +
                                           " meet at " + describe(fn.inst, fn.src) + " (only mono broadcasts)");
      }
      if (inBound_[p].kind == Kind::Infer && !joinKind(kind, t.kind))
        reportNode(f, ErrorCode::Type, "kind mismatch: real and bool meet at " + describe(fn.inst, fn.src) +
                                           " (convert explicitly with gate_to_real or a comparison)");
    }
    const bool stateful = sig.has(atom_flags::kStateful);
    if (stateful) rate = maxRate(rate, Rate::Audio);
    if (pack) {
      if (packWidth > kMaxWidth) {
        reportNode(f, ErrorCode::Overflow, "more than " + std::to_string(kMaxWidth) + " channels at " +
                                               describe(fn.inst, fn.src) + " (a pack in a feedback loop?)");
        packWidth = kMaxWidth;
      }
      width = static_cast<uint16_t>(packWidth);
    }
    if (!perVoice_.empty() && perVoice_[f]) poly = Poly::Voice;  // a free-running source made per-voice (freeRunning)
    bool changed = false;
    for (uint32_t j = 0; j < fn.nOut; ++j) {
      const uint32_t o = fn.firstOut + j;
      const PortType& d = outDecl_[o];
      PortType t;
      t.rate = maxRate(rate, d.rate);
      t.poly = d.poly != Poly::Infer ? d.poly : poly;
      t.kind = d.kind != Kind::Infer ? d.kind : kind;
      t.width = d.width != kWidthInfer ? d.width : sig.category == AtomCategory::Io ? opt_.ioChannels : width;
      changed |= setOut(f, o, t, !pack);
    }
    return changed;
  }

  void push(uint32_t f) {
    if (queued_[f]) return;
    queued_[f] = 1;
    queue_.push_back(f);
  }

  void solveFixpoint() {
    const auto nf = static_cast<uint32_t>(fnodes_.size());
    const auto nOut = static_cast<uint32_t>(outDecl_.size());
    outT_.assign(nOut, PortType{});
    solveType_.assign(solves_.size(), PortType{});
    solveUnknowns_.assign(solves_.size(), {});
    if (const auto p = AtomRegistry::builtin().find("pack")) atomPack_ = *p;
    for (uint32_t f = 0; f < nf; ++f)
      if (fnodes_[f].kind == FKind::Unknown) solveUnknowns_[fnodes_[f].solve].push_back(f);

    // Consumers of each out-port (CSR).
    consBegin_.assign(nOut + 1, 0);
    for (uint32_t d : inDrv_)
      if (d != kNone) ++consBegin_[d + 1];
    for (uint32_t o = 0; o < nOut; ++o) consBegin_[o + 1] += consBegin_[o];
    cons_.resize(consBegin_[nOut]);
    {
      std::vector<uint32_t> fill(consBegin_.begin(), consBegin_.end() - 1);
      for (uint32_t f = 0; f < nf; ++f)
        for (uint32_t i = 0; i < fnodes_[f].nIn; ++i)
          if (const uint32_t d = inDrv_[fnodes_[f].firstIn + i]; d != kNone) cons_[fill[d]++] = f;
    }

    // Seed in topological order (drivers before consumers; back edges of cycles are ignored), so an acyclic graph
    // converges in one pass.
    queued_.assign(nf, 0);
    queue_.clear();
    queue_.reserve(nf);
    {
      std::vector<uint8_t> state(nf, 0);  // 0 new, 1 on stack, 2 done
      std::vector<std::pair<uint32_t, uint32_t>> stack;
      for (uint32_t s = 0; s < nf; ++s) {
        if (state[s]) continue;
        stack.emplace_back(s, 0);
        state[s] = 1;
        while (!stack.empty()) {
          auto& [f, i] = stack.back();
          const FNode& fn = fnodes_[f];
          if (i < fn.nIn) {
            const uint32_t d = inDrv_[fn.firstIn + i++];
            if (d != kNone) {
              const uint32_t g = outOwner_[d];
              if (state[g] == 0) {
                state[g] = 1;
                stack.emplace_back(g, 0);
              }
            }
            continue;
          }
          state[f] = 2;
          push(f);
          stack.pop_back();
        }
      }
    }
    drain();
    freeRunning();
  }

  // Processes the worklist until nothing changes.
  void drain() {
    for (size_t head = 0; head < queue_.size(); ++head) {
      const uint32_t f = queue_[head];
      queued_[f] = 0;
      if (eval(f)) {
        const FNode& fn = fnodes_[f];
        for (uint32_t o = fn.firstOut; o < fn.firstOut + fn.nOut; ++o)
          for (uint32_t c = consBegin_[o]; c < consBegin_[o + 1]; ++c) push(cons_[c]);
      }
      // The queue only grows; compact it now and then so long cascades do not keep the whole history.
      if (head > 4096 && head * 2 > queue_.size()) {
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(head) + 1);
        head = static_cast<size_t>(-1);
      }
    }
    queue_.clear();
  }

  // Free-running sources (QUESTIONS Q29 B, T1.3-fix1): a stateful atom whose inputs are all global and at most
  // param rate (noise(), pulse(), phasor(0.5Hz), phasor(rateParam), sine_hz(lfoRate)…) gets one instance per
  // voice when the patch is in a voice context and its output feeds voice processing: phase from note-on, noise
  // seeded per voice. A source that reaches voice processing only through a `shared(…)` node, or only through a
  // global-bound port (a callee's `in … global`: that is a reduction), keeps one global instance. Delay lines stay
  // global (a ring per voice is a memory decision the patch makes by feeding a voice signal in).
  //   Decided after the fixpoint, from the inferred types (rates do not depend on poly, so promoting a source never
  // changes which atoms are free-running): the previous rule, evaluated per node during the fixpoint, read an
  // unevaluated driver as const, so the result depended on visit order (M1 review M21). Iterated, because a
  // promoted source makes its consumers per-voice, which other sources may feed.
  void freeRunning() {
    if (opt_.freeRunningPoly != Poly::Voice || !opt_.voiceContext) return;
    const auto nf = static_cast<uint32_t>(fnodes_.size());
    perVoice_.assign(nf, 0);
    auto isVoice = [&](const PortType& t) { return t.poly == Poly::Voice; };
    for (uint32_t round = 0; round < nf + 1; ++round) {
      // Nodes whose value reaches voice processing: backwards from every per-voice node.
      std::vector<uint8_t> feeds(nf, 0);
      std::vector<uint32_t> stack;
      for (uint32_t f = 0; f < nf; ++f) {
        const FNode& fn = fnodes_[f];
        bool v = false;
        for (uint32_t o = fn.firstOut; o < fn.firstOut + fn.nOut && !v; ++o) v = isVoice(outT_[o]);
        // A per-voice input, or a port declared voice (a root `out … voice`: the voices' own output).
        for (uint32_t p = fn.firstIn; p < fn.firstIn + fn.nIn && !v; ++p)
          v = inBound_[p].poly == Poly::Voice || (inDrv_[p] != kNone && isVoice(outT_[inDrv_[p]]));
        if (fn.kind == FKind::Equation || fn.kind == FKind::Unknown) v = v || isVoice(solveType_[fn.solve]);
        if (v) {
          feeds[f] = 1;
          stack.push_back(f);
        }
      }
      while (!stack.empty()) {
        const uint32_t v = stack.back();
        stack.pop_back();
        const FNode& fn = fnodes_[v];
        for (uint32_t p = fn.firstIn; p < fn.firstIn + fn.nIn; ++p) {
          if (inBound_[p].poly == Poly::Global || inDrv_[p] == kNone) continue;  // a reduction, or unconnected
          const uint32_t u = outOwner_[inDrv_[p]];
          if (feeds[u]) continue;
          feeds[u] = 1;
          if (!fnodes_[u].shared) stack.push_back(u);
        }
      }
      bool any = false;
      for (uint32_t f = 0; f < nf; ++f) {
        const FNode& fn = fnodes_[f];
        if (fn.kind != FKind::Atom || fn.shared || perVoice_[f] || !feeds[f]) continue;
        const AtomSignature& sig = atomSignature(fn.atom);
        if (!sig.has(atom_flags::kStateful) || sig.name.starts_with("delay")) continue;
        bool freeRun = true;
        for (uint32_t p = fn.firstIn; p < fn.firstIn + fn.nIn && freeRun; ++p) {
          if (inDrv_[p] == kNone) continue;
          const PortType& t = outT_[inDrv_[p]];
          freeRun = !isVoice(t) && (t.rate == Rate::Infer || t.rate <= Rate::Param);
        }
        if (!freeRun) continue;
        perVoice_[f] = 1;
        push(f);
        any = true;
      }
      if (!any) break;
      drain();
    }
  }

  void normalise() {
    for (PortType& t : outT_) {
      if (t.kind == Kind::Infer) t.kind = Kind::Real;
      if (t.rate == Rate::Infer) t.rate = Rate::Const;
      if (t.poly == Poly::Infer) t.poly = Poly::Global;
      if (t.width == kWidthInfer) t.width = kWidthMono;
    }
  }

  // ---- bound checks ---------------------------------------------------------------------------------------------

  void check() {
    const auto nf = static_cast<uint32_t>(fnodes_.size());
    const AtomRegistry& reg = AtomRegistry::builtin();
    const auto channelAtom = reg.find("channel");
    for (uint32_t f = 0; f < nf; ++f) {
      const FNode& fn = fnodes_[f];
      if (fn.kind == FKind::Atom) {
        const AtomSignature& sig = atomSignature(fn.atom);
        if (sig.has(atom_flags::kNeedsVoice) && !opt_.voiceContext)
          reportNode(f, ErrorCode::Type, std::string(sig.name) + " is only available in a voice context: " +
                                             describe(fn.inst, fn.src));
        if (channelAtom && fn.atom == *channelAtom) checkChannel(f);
      }
      for (uint32_t i = 0; i < fn.nIn; ++i) {
        const uint32_t p = fn.firstIn + i;
        const uint32_t d = inDrv_[p];
        if (d == kNone) continue;
        const PortType& b = inBound_[p];
        const PortType& t = outT_[d];
        auto from = [&] {
          const FNode& s = fnodes_[outOwner_[d]];
          return std::string(insts_[s.inst].patch->describe(s.src));
        };
        if (b.kind != Kind::Infer && t.kind != b.kind) {
          const char* hint = t.kind == Kind::Bool ? " (convert with gate_to_real)" : " (convert with a comparison)";
          reportPort(f, ErrorCode::Type, "kind mismatch: " + std::string(toText(t.kind)) + " into " +
                                             std::string(toText(b.kind)) + " " + inPortText(f, i) + ", from " +
                                             from() + hint);
          continue;
        }
        if (b.rate != Rate::Infer && ((b.rate == Rate::Event) != (t.rate == Rate::Event))) {
          reportPort(f, ErrorCode::Type, "rate mismatch: " + std::string(toText(t.rate)) + " into " +
                                             std::string(toText(b.rate)) + " " + inPortText(f, i));
          continue;
        }
        if (b.rate != Rate::Infer && t.rate > b.rate) {
          reportPort(f, ErrorCode::Type, "rate " + std::string(toText(t.rate)) + " exceeds the declared bound " +
                                             std::string(toText(b.rate)) + " of " + inPortText(f, i) + ", from " +
                                             from());
          continue;
        }
        if (b.width != kWidthInfer && t.width != kWidthMono && t.width != b.width) {
          reportPort(f, ErrorCode::Type, "width mismatch: " + widthText(t.width) + " into " + widthText(b.width) +
                                             " " + inPortText(f, i) + ", from " + from());
          continue;
        }
        if (b.poly == Poly::Global && t.poly == Poly::Voice && !opt_.implicitVoiceSum)
          reportPort(f, ErrorCode::Type, "a per-voice signal into global " + inPortText(f, i) + ", from " + from() +
                                             " needs an explicit reducer (voice_sum, voice_last, voice_highest, "
                                             "voice_lowest, voice_loudest)");
      }
    }
  }

  void checkChannel(uint32_t f) {
    const FNode& fn = fnodes_[f];
    const uint32_t xd = inDrv_[fn.firstIn], id = inDrv_[fn.firstIn + 1];
    if (xd == kNone || id == kNone) return;
    const uint16_t w = outT_[xd].width;
    const FNode& idx = fnodes_[outOwner_[id]];
    if (idx.kind != FKind::Const || w <= kWidthMono) return;  // non-literal consts are checked once folded (T1.4)
    const double v = insts_[idx.inst].patch->constant(idx.src).value();
    if (!(v >= 0.0 && v < w && v == std::floor(v))) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%g", v);
      reportNode(f, ErrorCode::Type, "channel index " + std::string(buf) + " is not a channel of a " + widthText(w) +
                                         " signal: " + describe(fn.inst, fn.src));
    }
  }

  // ---- phase 3: emit --------------------------------------------------------------------------------------------

  std::string flatName(uint32_t inst, NodeIndex n) const {
    const std::string& name = insts_[inst].patch->nodeName(n);
    if (name.empty() || inst == 0) return name;
    return insts_[inst].prefix + "__" + name;
  }

  uint64_t nodeKey(uint32_t inst, NodeIndex n) const {
    return hashMix(insts_[inst].key, insts_[inst].patch->node(n).id.value);
  }

  bool reduces(uint32_t p) const {
    const uint32_t d = inDrv_[p];
    return d != kNone && inBound_[p].poly == Poly::Global && outT_[d].poly == Poly::Voice;
  }

  NodeIndex addFlat(TypedPatch& tp, NodeIndex n, uint64_t key, NodeOrigin origin) {
    tp.stateKey.push_back(key);
    tp.origin.push_back(origin);
    return n;
  }

  // The flat output that out-port o stands for, following wired-through identities. kNone: unconnected.
  PortRef flatSource(uint32_t o) {
    uint32_t steps = 0;
    while (true) {
      if (flatOut_[o].node != kNone) return flatOut_[o];
      const uint32_t f = outOwner_[o];
      const FNode& fn = fnodes_[f];
      if (fn.kind != FKind::Identity) return {};
      const uint32_t d = inDrv_[fn.firstIn];
      if (d == kNone) return {};
      if (++steps > fnodes_.size()) {  // identities wired into each other in a ring: a cycle with no node in it
        reportNode(f, ErrorCode::InvalidArgument,
                   "a cycle through subcircuit ports with no node in it: " + describe(fn.inst, fn.src));
        return {};
      }
      o = d;
    }
  }

  void emit(TypedPatch& tp) {
    const AtomRegistry& reg = AtomRegistry::builtin();
    const AtomId voiceSum = *reg.find("voice_sum");
    const AtomId atomXor = *reg.find("xor"), atomNot = *reg.find("not");
    const AtomId atomEq = *reg.find("eq"), atomNe = *reg.find("ne");
    const auto nf = static_cast<uint32_t>(fnodes_.size());
    Patch& out = tp.patch;
    out.setName(root_.name());
    tp.instances.reserve(insts_.size());
    for (const Inst& I : insts_) tp.instances.push_back(InstanceInfo{I.patch, I.parent, I.callNode, I.key});
    for (uint32_t s : solves_) out.addSolve(s);
    tp.stateKey.reserve(nf);
    tp.origin.reserve(nf);

    // Pass 1: nodes. flatOut_[o] = the flat output standing for out-port o; inTarget_[p] = the flat input that
    // in-port p feeds (node kNone: wired through).
    flatOut_.assign(outDecl_.size(), PortRef{});
    inTarget_.assign(inBound_.size(), PortRef{});
    struct Pending {
      PortRef reducerIn;  // the reducer input to wire from in-port p's source
      uint32_t p;
    };
    std::vector<Pending> reducersBefore;  // reducers in front of a non-identity node's input
    for (uint32_t f = 0; f < nf; ++f) {
      const FNode& fn = fnodes_[f];
      const Patch& P = *insts_[fn.inst].patch;
      const NodeOrigin org{fn.inst, fn.src, NodeOrigin::What::Node};
      const uint64_t key = nodeKey(fn.inst, fn.src);
      NodeIndex n = kNone;
      switch (fn.kind) {
        case FKind::Identity:
          if (reduces(fn.firstIn)) {
            n = addFlat(tp, out.addAtom(voiceSum), hashMix(key, 0x5E5Eu), {fn.inst, fn.src, NodeOrigin::What::Reducer});
            inTarget_[fn.firstIn] = {n, 0};
            flatOut_[fn.firstOut] = {n, 0};
          }
          continue;
        case FKind::Atom: {
          const AtomSignature& sig = atomSignature(fn.atom);
          if (fn.atom == atomTuneVoices_) continue;  // consumed by bindVoiceTunings (no node of its own)
          if ((fn.atom == atomEq || fn.atom == atomNe) && inDrv_[fn.firstIn] != kNone &&
              outT_[inDrv_[fn.firstIn]].kind == Kind::Bool) {
            // Bool equality: ne = xor, eq = not(xor). The dsp comparators are real-only (kernel_map.h).
            const NodeIndex x = addFlat(tp, out.addAtom(atomXor), fn.atom == atomEq ? hashMix(key, 1) : key,
                                        fn.atom == atomEq ? NodeOrigin{fn.inst, fn.src, NodeOrigin::What::Rewrite} : org);
            inTarget_[fn.firstIn] = {x, 0};
            inTarget_[fn.firstIn + 1] = {x, 1};
            NodeIndex last = x;
            if (fn.atom == atomEq) {
              last = addFlat(tp, out.addAtom(atomNot), key, {fn.inst, fn.src, NodeOrigin::What::Rewrite});
              out.connect(x, last, 0);
              extraOut_.emplace_back(x, outT_[fn.firstOut]);
            }
            out.setNodeName(last, flatName(fn.inst, fn.src));
            flatOut_[fn.firstOut] = {last, 0};
            continue;
          }
          uint16_t repeat = 0;
          if (sig.has(atom_flags::kVariadicInputs)) repeat = static_cast<uint16_t>(fn.nIn - sig.fixedInputs());
          if (sig.has(atom_flags::kVariadicOutputs)) repeat = static_cast<uint16_t>(fn.nOut - sig.fixedOutputs());
          n = out.addAtom(fn.atom, repeat);
          out.setNodeName(n, flatName(fn.inst, fn.src), P.node(fn.src).nameOutput);
          break;
        }
        case FKind::Const: n = out.addConst(P.constant(fn.src)); out.setNodeName(n, flatName(fn.inst, fn.src)); break;
        case FKind::Param: n = out.addParam(flatName(fn.inst, fn.src), P.paramDecl(fn.src)); break;
        case FKind::Input: n = out.addInput(flatName(fn.inst, fn.src), P.outputType(fn.src, 0)); break;
        case FKind::Output: n = out.addOutput(flatName(fn.inst, fn.src), P.inputType(fn.src, 0)); break;
        case FKind::Export: n = out.addExport(flatName(fn.inst, fn.src)); break;
        case FKind::Unknown: n = out.addUnknown(flatName(fn.inst, fn.src), fn.solve); break;
        case FKind::Equation: n = out.addEquation(fn.solve); break;
      }
      addFlat(tp, n, key, org);
      for (uint32_t j = 0; j < fn.nOut; ++j) flatOut_[fn.firstOut + j] = {n, static_cast<uint16_t>(j)};
      for (uint32_t i = 0; i < fn.nIn; ++i) {
        const uint32_t p = fn.firstIn + i;
        if (reduces(p)) {
          const NodeIndex r = addFlat(tp, out.addAtom(voiceSum), hashMix(key, 0x5E5E0000u + i),
                                      {fn.inst, fn.src, NodeOrigin::What::Reducer});
          out.connect(PortRef{r, 0}, PortRef{n, static_cast<uint16_t>(i)});
          reducersBefore.push_back({PortRef{r, 0}, p});
          PortType rt = outT_[inDrv_[p]];
          rt.poly = Poly::Global;
          extraOut_.emplace_back(r, rt);
        } else {
          inTarget_[p] = {n, static_cast<uint16_t>(i)};
        }
      }
      // A per-voice oscillator made from a free-running one (freeRunning) restarts at its voice's onset sample:
      // reset = voice.trigger (or'ed with its own reset). The lane's state is reset at the start of the onset's
      // block, and a const/param-rate rate (unlike voice.hz, which is 0 before the onset) would otherwise advance
      // it for the frames before the onset.
      if (fn.kind == FKind::Atom && !perVoice_.empty() && perVoice_[f] && (fn.atom == atomPhasor_ || fn.atom == atomSineHz_)) {
        const PortType trigType = portType(Kind::Bool, Rate::Audio, Poly::Voice, kWidthMono);
        const NodeIndex trig = addFlat(tp, out.addAtom(atomVoiceTrigger_), hashMix(key, 0x7121660u),
                                       {fn.inst, fn.src, NodeOrigin::What::Rewrite});
        extraOut_.emplace_back(trig, trigType);
        const uint32_t p = fn.firstIn + 1;  // phasor / sine_hz: (hz, reset)
        if (inDrv_[p] == kNone) {
          out.connect(PortRef{trig, 0}, PortRef{n, 1});
        } else {
          const NodeIndex any = addFlat(tp, out.addAtom(atomOr_), hashMix(key, 0x7121661u),
                                        {fn.inst, fn.src, NodeOrigin::What::Rewrite});
          extraOut_.emplace_back(any, trigType);
          inTarget_[p] = {any, 0};
          out.connect(PortRef{trig, 0}, PortRef{any, 1});
          out.connect(PortRef{any, 0}, PortRef{n, 1});
        }
      }
    }

    // Pass 2: wires from each in-port's (wired-through) source.
    for (uint32_t p = 0; p < inBound_.size(); ++p) {
      if (inTarget_[p].node == kNone || inDrv_[p] == kNone) continue;
      const PortRef src = flatSource(inDrv_[p]);
      if (src.node != kNone) out.connect(src, inTarget_[p]);
    }
    for (const Pending& r : reducersBefore) {
      const PortRef src = flatSource(inDrv_[r.p]);
      if (src.node != kNone) out.connect(src, r.reducerIn);
    }

    // Types: every output from the fixpoint (or, for inserted nodes, from what they compute), then every input
    // from its driver.
    tp.types.assign(out.portCount(), PortType{});
    for (uint32_t o = 0; o < outDecl_.size(); ++o)
      if (const PortRef r = flatOut_[o]; r.node != kNone) tp.types[out.flatOutput(r.node, r.port)] = outT_[o];
    for (const auto& [n, t] : extraOut_) tp.types[out.flatOutput(n, 0)] = t;
    for (const Wire& w : out.wires()) tp.types[out.flatInput(w.to.node, w.to.port)] = tp.types[out.flatOutput(w.from.node, w.from.port)];
    for (NodeIndex n = 0; n < out.nodeCount(); ++n)
      for (uint32_t i = 0; i < out.node(n).numInputs; ++i) {
        PortType& t = tp.types[out.flatInput(n, i)];
        if (t.kind == Kind::Infer)  // unconnected optional input: its default, a const
          t = portType(out.inputType(n, i).kind == Kind::Bool ? Kind::Bool : Kind::Real, Rate::Const, Poly::Global,
                       kWidthMono);
      }
  }

  // T4.5: each probe target's value in the flattened patch: the instance by its call path, then the out-port of the
  // node's output (a call's output is the callee's boundary node), followed through wired-through identities.
  void locateProbes(InferResult& r) {
    r.probeSources.assign(opt_.probes.size(), kNone);
    for (size_t k = 0; k < opt_.probes.size(); ++k) {
      const ProbeTarget& t = opt_.probes[k];
      uint32_t inst = 0;
      bool ok = true;
      for (const NodeId id : t.path) {
        const Inst& I = insts_[inst];
        const std::optional<NodeIndex> n = I.patch->indexOf(id);
        if (!n || gChild_[I.gbase + *n] == kNone) {
          ok = false;
          break;
        }
        inst = gChild_[I.gbase + *n];
      }
      if (!ok) continue;
      const Inst& I = insts_[inst];
      const std::optional<NodeIndex> n = I.patch->indexOf(t.node);
      if (!n) continue;
      const uint32_t o = sourceOf(I.gbase + *n, t.output);
      if (o == kNone) continue;
      const PortRef src = flatSource(o);
      if (src.node == kNone) continue;
      r.probeSources[k] = r.typed.patch.flatOutput(src.node, src.port);
    }
  }

  // ---- state ----------------------------------------------------------------------------------------------------

  const Patch& root_;
  const InferOptions& opt_;
  std::vector<Diagnostic> diags_;
  std::unordered_map<const Patch*, CalleeInfo> callees_;
  std::vector<Inst> insts_;
  std::vector<const Patch*> active_;  // instantiation stack (recursion check)
  std::vector<uint32_t> solves_;      // iterations per flat solve block

  // Global node ids (instance gbase + node index) → FNode / child instance.
  std::vector<uint32_t> gF_, gChild_;

  std::vector<FNode> fnodes_;
  std::vector<PortType> inBound_;   // per in-port: declared upper bound
  std::vector<uint32_t> inDrv_;     // per in-port: driving out-port or kNone
  std::vector<PortType> outDecl_;   // per out-port: declared type (sources, floors)
  std::vector<uint32_t> outOwner_;  // per out-port: FNode
  std::vector<PortType> outT_;      // per out-port: inferred
  std::vector<uint8_t> failed_;     // per FNode: already reported
  std::vector<uint8_t> perVoice_;   // per FNode: a free-running source made per-voice (freeRunning)

  std::vector<uint32_t> consBegin_, cons_;
  std::vector<uint32_t> queue_;
  std::vector<uint8_t> queued_;
  std::vector<PortType> solveType_;
  std::vector<std::vector<uint32_t>> solveUnknowns_;
  AtomId atomPack_ = 0xFFFF;
  // T2.6 voice tunings.
  const AtomId atomVoiceHz_ = atomId("voice.hz");
  const AtomId atomVoiceCents_ = atomId("voice.cents");
  const AtomId atomTuneVoices_ = atomId("tune_voices");
  // T1.3-fix1: per-voice oscillators restart at the onset.
  const AtomId atomPhasor_ = atomId("phasor");
  const AtomId atomSineHz_ = atomId("sine_hz");
  const AtomId atomVoiceTrigger_ = atomId("voice.trigger");
  const AtomId atomOr_ = atomId("or");
  std::vector<std::pair<uint32_t, bool>> voiceReads_;  // (FNode, is voice.cents) of non-root voice.hz/cents reads

  std::vector<PortRef> flatOut_;    // emit: per out-port
  std::vector<PortRef> inTarget_;   // emit: per in-port
  std::vector<std::pair<NodeIndex, PortType>> extraOut_;  // emit: output 0 types of inserted nodes
};

}  // namespace

InferResult inferDetailed(const Patch& root, const InferOptions& options) { return Inferrer(root, options).run(); }

Result<TypedPatch> infer(const Patch& root, const InferOptions& options) {
  InferResult r = inferDetailed(root, options);
  if (r.ok()) return std::move(r.typed);
  Error e;
  e.code = r.diagnostics.front().code;
  e.location = r.diagnostics.front().location;
  for (size_t i = 0; i < r.diagnostics.size(); ++i) {
    if (i) e.message += '\n';
    e.message += options.source.text.empty() ? r.diagnostics[i].message
                                             : formatDiagnostic(options.source.text, r.diagnostics[i]);
  }
  return e;
}

}  // namespace pq::graph
