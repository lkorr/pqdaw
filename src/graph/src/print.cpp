// Printer of the patch language (T1.2): Patch IR → canonical .pqp text. See print() in parse.h.
//
// Layout of the output (one member per line, so the port-type line rule of the parser always holds):
//   patch Name {
//     param … / in … / out …                    declarations, in node order per kind
//     name = expr                                named computed nodes, in node order; anonymous single-consumer
//     out = expr                                 nodes are inlined into the expression that consumes them
//     export name = expr
//     solve(iterations: n) { unknown x = … ; lhs == rhs }
//     layout { node … / wire … / comment … }
//   }
// Numbers use std::to_chars (shortest representation that reads back to the same double/float), so literals,
// params and layout coordinates round-trip bit for bit.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <vector>

#include "pq/graph/parse.h"
#include "text_common.h"

namespace pq::graph {

using namespace detail;

namespace {

void appendDouble(std::string& out, double v) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, r.ptr);
}

void appendFloat(std::string& out, float v) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, r.ptr);
}

void appendConstant(std::string& out, const Constant& c) {
  if (c.kind == Kind::Bool) {
    out += c.literal != 0.0 ? "true" : "false";
    return;
  }
  appendDouble(out, c.literal);
  out += unitSuffix(c.unit);
}

void appendString(std::string& out, std::string_view s) {
  out += '"';
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  out += '"';
}

void appendPortType(std::string& out, const PortType& t) {
  Rate rate = t.rate;
  if (t.kind == Kind::Real && rate == Rate::Audio) {
    out += "audio";
    rate = Rate::Infer;
  } else {
    out += t.kind == Kind::Bool ? "bool" : "real";
  }
  switch (rate) {
    case Rate::Const: out += " const"; break;
    case Rate::Param: out += " param"; break;
    case Rate::Audio: out += " audio"; break;
    default: break;  // Infer; Event has no text form
  }
  if (t.poly == Poly::Global) out += " global";
  else if (t.poly == Poly::Voice) out += " voice";
  if (t.width == kWidthMono) out += " mono";
  else if (t.width == kWidthStereo) out += " stereo";
  else if (t.width != kWidthInfer) {
    out += " ch(";
    out += std::to_string(t.width);
    out += ')';
  }
}

void appendAttrs(std::string& out, const std::vector<Attr>& attrs) {
  for (const Attr& a : attrs) {
    out += " @";
    out += a.name;
    if (a.args.empty()) continue;
    out += '(';
    for (size_t i = 0; i < a.args.size(); ++i) {
      if (i) out += ", ";
      const AttrArg& arg = a.args[i];
      if (!arg.name.empty()) {
        out += arg.name;
        out += ": ";
      }
      switch (arg.value.type) {
        case AttrValue::Type::Number:
        case AttrValue::Type::Bool: appendConstant(out, arg.value.number); break;
        case AttrValue::Type::String: appendString(out, arg.value.text); break;
        case AttrValue::Type::Ident: out += arg.value.text; break;
      }
    }
    out += ')';
  }
}

bool isExprNode(NodeKind k) noexcept { return k == NodeKind::Atom || k == NodeKind::Const || k == NodeKind::Subcircuit; }

class PatchPrinter {
public:
  PatchPrinter(const Patch& p, std::string& out) : p_(p), out_(out), adj_(p.adjacency()), ops_(OperatorAtoms::get()) {}

  // Only the layout block (printLayout, spliceLayout).
  void runLayout() {
    plan();
    layout();
  }

  void run() {
    plan();
    out_ += "patch ";
    out_ += p_.name();
    out_ += " {\n";
    const uint32_t n = p_.nodeCount();
    bool section = false;
    auto gap = [&] {
      if (section) out_ += '\n';
      section = false;
    };

    for (NodeKind k : {NodeKind::Param, NodeKind::Input, NodeKind::Output})
      for (NodeIndex v = 0; v < n; ++v)
        if (p_.node(v).kind == k) {
          declaration(v);
          section = true;
        }
    gap();

    for (NodeIndex v = 0; v < n; ++v) {
      if (!isExprNode(p_.node(v).kind) || inline_[v]) continue;
      line();
      out_ += nameOf(v);
      out_ += " = ";
      const uint16_t no = p_.node(v).nameOutput;
      if (no != kWholeNode && !p_.nodeName(v).empty()) {
        expr(v, kPrecPostfix);
        out_ += '.';
        out_ += portTextName(p_, v, no, false);
      } else {
        expr(v, kPrecTernary);
      }
      out_ += '\n';
      section = true;
    }
    for (NodeIndex v = 0; v < n; ++v) {
      if (p_.node(v).kind != NodeKind::Output || !driven(v, 0)) continue;
      line();
      out_ += nameOf(v);
      out_ += " = ";
      ref(driver(v, 0), kPrecTernary);
      out_ += '\n';
      section = true;
    }
    gap();

    for (NodeIndex v = 0; v < n; ++v) {
      if (p_.node(v).kind != NodeKind::Export) continue;
      line();
      out_ += "export ";
      out_ += nameOf(v);
      if (driven(v, 0)) {  // an undriven export is `export e` (T4.4)
        out_ += " = ";
        ref(driver(v, 0), kPrecTernary);
      }
      out_ += '\n';
      section = true;
    }
    gap();

    for (SolveIndex s = 0; s < p_.solveCount(); ++s) {
      out_ += "  solve(iterations: ";
      out_ += std::to_string(p_.solves()[s].iterations);
      out_ += ") {\n";
      for (NodeIndex v = 0; v < n; ++v) {
        const Node& nd = p_.node(v);
        if (nd.kind != NodeKind::Unknown || nd.payload != s) continue;
        out_ += "    unknown ";
        out_ += nameOf(v);
        if (driven(v, 0)) {
          out_ += " = ";
          ref(driver(v, 0), kPrecTernary);
        }
        out_ += '\n';
      }
      for (NodeIndex v = 0; v < n; ++v) {
        const Node& nd = p_.node(v);
        if (nd.kind != NodeKind::Equation || nd.payload != s) continue;
        out_ += "    ";
        side(v, 0);
        out_ += " == ";
        side(v, 1);
        out_ += '\n';
      }
      out_ += "  }\n";
      section = true;
    }
    gap();

    layout();
    out_ += "}\n";
  }

private:
  void line() { out_ += "  "; }

  bool driven(NodeIndex v, uint32_t input) const { return adj_.driver[p_.flatInput(v, input)] != kNone; }
  PortRef driver(NodeIndex v, uint32_t input) const { return p_.wire(adj_.driver[p_.flatInput(v, input)]).from; }

  std::string_view nameOf(NodeIndex v) const {
    return p_.nodeName(v).empty() ? std::string_view(genName_[v]) : std::string_view(p_.nodeName(v));
  }

  void side(NodeIndex eq, uint32_t input) {
    if (driven(eq, input)) ref(driver(eq, input), kPrecAdd);
    else out_ += '0';  // invalid patch (an equation side is required); keep the text parseable
  }

  // ---- planning: which anonymous nodes are inlined, which get generated names ----
  void plan() {
    const uint32_t n = p_.nodeCount();
    inline_.assign(n, 0);
    // Layout never changes what is inlined (T4.3): placed and routed anonymous nodes are named by their layout key.
    for (NodeIndex v = 0; v < n; ++v)
      if (isExprNode(p_.node(v).kind) && p_.nodeName(v).empty() && adj_.outgoing(v).size() == 1) inline_[v] = 1;
    breakInlineCycles();
    limitDepth();

    // Generated names for everything that must be referred to by name but has none (never for equations, which
    // cannot be named). `_n<index>`, made unique against the patch's own names.
    genName_.assign(n, {});
    NameTable taken;
    taken.reset(n);
    for (NodeIndex v = 0; v < n; ++v)
      if (!p_.nodeName(v).empty()) taken.insert(p_.nodeName(v), v);
    for (NodeIndex v = 0; v < n; ++v) {
      if (inline_[v] || !p_.nodeName(v).empty() || p_.node(v).kind == NodeKind::Equation) continue;
      std::string name = "_n" + std::to_string(v);
      while (taken.find(name)) name += '_';
      genName_[v] = std::move(name);
      taken.insert(genName_[v], v);
    }
    // Layout keys of equations (T4.3): `_e<k>` for the k-th equation in print order (= text order), unique like the
    // `_n` names. The parser reads `_e<k>` followed by any '_' as that equation when no node has the name.
    eqName_.assign(n, {});
    uint32_t eq = 0;
    for (SolveIndex s = 0; s < p_.solveCount(); ++s)
      for (NodeIndex v = 0; v < n; ++v) {
        if (p_.node(v).kind != NodeKind::Equation || p_.node(v).payload != s) continue;
        std::string name = "_e" + std::to_string(eq++);
        while (taken.find(name)) name += '_';
        eqName_[v] = std::move(name);
        taken.insert(eqName_[v], v);
      }

    // The order statements are printed in: declarations by kind, solve members, then named computed nodes. Parsing
    // the printed text creates nodes in this order, so layout lines sorted by it print identically next time.
    rank_.assign(n, kNone);
    uint32_t r = 0;
    for (NodeKind k : {NodeKind::Param, NodeKind::Input, NodeKind::Output, NodeKind::Export})
      for (NodeIndex v = 0; v < n; ++v)
        if (p_.node(v).kind == k) rank_[v] = r++;
    for (SolveIndex s = 0; s < p_.solveCount(); ++s)
      for (NodeKind k : {NodeKind::Unknown, NodeKind::Equation})
        for (NodeIndex v = 0; v < n; ++v)
          if (p_.node(v).kind == k && p_.node(v).payload == s) rank_[v] = r++;
    for (NodeIndex v = 0; v < n; ++v)
      if (rank_[v] == kNone && !inline_[v]) rank_[v] = r++;
    for (NodeIndex v = 0; v < n; ++v)
      if (rank_[v] == kNone) rank_[v] = r++;
  }

  // An inlined node has one consumer; following consumers from inlined node to inlined node must reach a printed
  // statement. A loop of anonymous nodes never does, so one node per loop is named.
  void breakInlineCycles() {
    const uint32_t n = p_.nodeCount();
    std::vector<uint8_t> state(n, 0);  // 0 new, 1 on the current path, 2 done
    std::vector<NodeIndex> path;
    for (NodeIndex v0 = 0; v0 < n; ++v0) {
      if (!inline_[v0] || state[v0]) continue;
      NodeIndex v = v0;
      while (inline_[v] && state[v] == 0) {
        state[v] = 1;
        path.push_back(v);
        v = p_.wire(adj_.outgoing(v)[0]).to.node;
      }
      if (inline_[v] && state[v] == 1) inline_[v] = 0;
      for (NodeIndex u : path) state[u] = 2;
      path.clear();
    }
  }

  // Printed expression depth (the parser's AST depth: a node is 1 + its deepest argument; a reference to a named
  // node is 1; selecting a non-first output of an inlined node adds 1). Children that would push a statement past
  // kMaxExprDepth are named instead of inlined, so printing recursion is bounded and the output always parses.
  void limitDepth() {
    const uint32_t n = p_.nodeCount();
    depth_.assign(n, 0);
    std::vector<std::pair<NodeIndex, uint32_t>> stack;
    for (NodeIndex root = 0; root < n; ++root) {
      if (inline_[root]) continue;
      stack.emplace_back(root, 0);
      while (!stack.empty()) {
        auto& [v, next] = stack.back();
        if (next < p_.node(v).numInputs) {
          const uint32_t k = next++;
          if (!driven(v, k)) continue;
          const NodeIndex c = driver(v, k).node;
          if (inline_[c] && depth_[c] == 0) stack.emplace_back(c, 0);
          continue;
        }
        const Node& nd = p_.node(v);
        uint32_t bound = kMaxExprDepth;
        if (!inline_[v]) {
          if (nd.kind == NodeKind::Equation) bound = kMaxExprDepth;
          else if (isExprNode(nd.kind)) bound = kMaxExprDepth - (nd.nameOutput != kWholeNode ? 1u : 0u);
          else bound = kMaxExprDepth + 1;  // out/export/in default/unknown init: the expression is the reference
        }
        uint32_t d = 0;
        for (uint32_t k = 0; k < nd.numInputs; ++k) {
          if (!driven(v, k)) continue;
          const PortRef from = driver(v, k);
          uint32_t rd = 1;
          if (inline_[from.node]) {
            rd = depth_[from.node] + (from.port != 0 ? 1u : 0u);
            if (1 + rd > bound) {
              inline_[from.node] = 0;
              rd = 1;
            }
          }
          d = std::max(d, rd);
        }
        depth_[v] = 1 + d;
        stack.pop_back();
      }
    }
  }

  // ---- declarations ----
  void declaration(NodeIndex v) {
    const Node& nd = p_.node(v);
    line();
    if (nd.kind == NodeKind::Param) {
      const ParamDecl& d = p_.paramDecl(v);
      out_ += "param ";
      out_ += nameOf(v);
      if (d.kind == Kind::Bool) out_ += " : bool";
      out_ += " = ";
      appendConstant(out_, d.defaultValue);
      if (d.hasRange) {
        out_ += " [";
        appendConstant(out_, d.rangeMin);
        out_ += "..";
        appendConstant(out_, d.rangeMax);
        out_ += ']';
      }
      appendAttrs(out_, d.attrs);
    } else if (nd.kind == NodeKind::Input) {
      out_ += "in ";
      out_ += nameOf(v);
      out_ += " : ";
      appendPortType(out_, p_.outputType(v, 0));
      if (driven(v, 0)) {
        out_ += " = ";
        ref(driver(v, 0), kPrecTernary);
      }
    } else {
      out_ += "out ";
      out_ += nameOf(v);
      out_ += " : ";
      appendPortType(out_, p_.inputType(v, 0));
    }
    out_ += '\n';
  }

  // ---- expressions ----
  // A value flowing out of `from`: a name (with `.port` when it is not the port the name stands for), or the
  // inlined expression of an anonymous node.
  void ref(PortRef from, int minPrec) {
    const NodeIndex v = from.node;
    if (inline_[v]) {
      if (from.port != 0) {
        expr(v, kPrecPostfix);
        out_ += '.';
        out_ += portTextName(p_, v, from.port, false);
      } else {
        expr(v, minPrec);
      }
      return;
    }
    out_ += nameOf(v);
    const uint16_t no = p_.nodeName(v).empty() ? kWholeNode : p_.node(v).nameOutput;
    const uint32_t named = no == kWholeNode ? 0u : no;
    if (from.port != named) {
      out_ += '.';
      out_ += portTextName(p_, v, from.port, false);
    }
  }

  void input(NodeIndex v, uint32_t k, int minPrec) { ref(driver(v, k), minPrec); }

  bool allDriven(NodeIndex v) const {
    for (uint32_t k = 0; k < p_.node(v).numInputs; ++k)
      if (!driven(v, k)) return false;
    return true;
  }

  // The defining expression of a computed node; `shared(…)` around it when the node is marked (Node::shared).
  void expr(NodeIndex v, int minPrec) {
    if (p_.node(v).shared && p_.node(v).kind == NodeKind::Atom) {
      out_ += "shared(";
      exprBody(v, kPrecTernary);
      out_ += ')';
      return;
    }
    exprBody(v, minPrec);
  }

  void exprBody(NodeIndex v, int minPrec) {
    const Node& nd = p_.node(v);
    if (nd.kind == NodeKind::Const) {
      const Constant& c = p_.constant(v);
      const bool paren = c.kind == Kind::Real && std::signbit(c.literal) && minPrec > kPrecUnary;
      if (paren) out_ += '(';
      appendConstant(out_, c);
      if (paren) out_ += ')';
      return;
    }
    if (nd.kind == NodeKind::Subcircuit) {
      call(v, p_.subcircuit(v).patch, kNone);
      return;
    }
    const AtomSignature& sig = atomSignature(nd.atom);
    if (sig.inputs.empty()) {
      out_ += sig.name;
      // Namespaced atoms (voice.hz) and sr are values; others (pulse) are written as calls so a local of the same
      // name cannot capture them.
      if (sig.name.find('.') == std::string_view::npos && sig.name != "sr") out_ += "()";
      return;
    }
    if (allDriven(v)) {
      const int bi = nd.numInputs == 2 ? ops_.binaryIndex(nd.atom) : -1;
      if (bi >= 0) {
        const BinaryOp& op = kBinaryOps[bi];
        const bool paren = op.prec < minPrec;
        if (paren) out_ += '(';
        input(v, 0, op.prec == kPrecCompare ? op.prec + 1 : op.prec);
        out_ += ' ';
        out_ += op.text;
        out_ += ' ';
        input(v, 1, op.prec + 1);
        if (paren) out_ += ')';
        return;
      }
      if (nd.numInputs == 1 && (nd.atom == ops_.neg || nd.atom == ops_.notOp)) {
        const bool paren = kPrecUnary < minPrec;
        if (paren) out_ += '(';
        out_ += nd.atom == ops_.neg ? '-' : '!';
        const PortRef x = driver(v, 0);
        if (inline_[x.node] && x.port == 0 && p_.node(x.node).kind == NodeKind::Const) {
          // `-(3)`: the parser reads `-3` as the literal −3, not as neg(3).
          out_ += '(';
          ref(x, kPrecTernary);
          out_ += ')';
        } else {
          ref(x, kPrecPostfix);
        }
        if (paren) out_ += ')';
        return;
      }
      if (nd.numInputs == 3 && nd.atom == ops_.select) {
        const bool paren = kPrecTernary < minPrec;
        if (paren) out_ += '(';
        input(v, 0, kPrecOr);
        out_ += " ? ";
        input(v, 1, kPrecTernary);
        out_ += " : ";
        input(v, 2, kPrecTernary);
        if (paren) out_ += ')';
        return;
      }
      if (nd.numInputs == 2 && nd.atom == ops_.channel) {
        input(v, 0, kPrecPostfix);
        out_ += '[';
        input(v, 1, kPrecTernary);
        out_ += ']';
        return;
      }
      if (nd.atom == ops_.pack && nd.numInputs >= 2) {
        out_ += '(';
        for (uint32_t k = 0; k < nd.numInputs; ++k) {
          if (k) out_ += ", ";
          input(v, k, kPrecTernary);
        }
        out_ += ')';
        return;
      }
    }
    const uint32_t repeatedFrom = (sig.has(atom_flags::kVariadicInputs)) ? sig.fixedInputs() : kNone;
    call(v, sig.name, repeatedFrom);
  }

  // `name(args)`: connected inputs positionally up to the first unconnected one, the rest by name.
  void call(NodeIndex v, std::string_view name, uint32_t repeatedFrom) {
    out_ += name;
    out_ += '(';
    const Node& nd = p_.node(v);
    bool positional = true, first = true;
    for (uint32_t k = 0; k < nd.numInputs; ++k) {
      if (!driven(v, k)) {
        positional = false;
        continue;
      }
      if (!first) out_ += ", ";
      first = false;
      if (!positional) {
        out_ += k >= repeatedFrom ? portTextName(p_, v, k, true) : std::string(p_.inputName(v, k));
        out_ += ": ";
      }
      input(v, k, kPrecTernary);
    }
    out_ += ')';
  }

  // ---- layout ----
  // Every node's layout key (T4.3) and the canonical order layout lines are printed in: each printed statement in
  // print order (rank), followed depth-first by the anonymous nodes inlined into it, through its inputs in port
  // order. A statement's key is its name (`_e<k>` for an equation); an inlined node's key is its consumer's key, '.',
  // and the consumer's input it drives. The order depends only on the printed text, so it survives a re-parse.
  void layoutKeys() {
    const uint32_t n = p_.nodeCount();
    key_.assign(n, {});
    keyPos_.assign(n, kNone);
    keyOrder_.clear();
    keyOrder_.reserve(n);
    std::vector<NodeIndex> roots;
    roots.reserve(n);
    for (NodeIndex v = 0; v < n; ++v)
      if (!inline_[v]) roots.push_back(v);
    std::sort(roots.begin(), roots.end(), [&](NodeIndex a, NodeIndex b) { return rank_[a] < rank_[b]; });
    std::vector<std::pair<NodeIndex, uint32_t>> stack;  // node, next input
    for (const NodeIndex root : roots) {
      key_[root] = p_.node(root).kind == NodeKind::Equation ? eqName_[root] : std::string(nameOf(root));
      keyPos_[root] = static_cast<uint32_t>(keyOrder_.size());
      keyOrder_.push_back(root);
      stack.emplace_back(root, 0);
      while (!stack.empty()) {
        auto& [v, next] = stack.back();
        if (next >= p_.node(v).numInputs) {
          stack.pop_back();
          continue;
        }
        const uint32_t k = next++;
        if (!driven(v, k)) continue;
        const NodeIndex c = driver(v, k).node;
        if (!inline_[c] || keyPos_[c] != kNone) continue;
        key_[c] = key_[v] + '.' + portTextName(p_, v, k, true);
        keyPos_[c] = static_cast<uint32_t>(keyOrder_.size());
        keyOrder_.push_back(c);
        stack.emplace_back(c, 0);  // invalidates v/next: the loop re-reads the top
      }
    }
    // Inlined nodes not reached from a statement (none in a well-formed patch) keep an empty key: never printed.
    for (NodeIndex v = 0; v < n; ++v)
      if (keyPos_[v] == kNone) keyPos_[v] = static_cast<uint32_t>(n + v);
  }

  void layout() {
    const uint32_t n = p_.nodeCount();
    bool any = !p_.comments().empty();
    for (NodeIndex v = 0; v < n && !any; ++v) any = p_.layout(v).placed;
    for (WireIndex w = 0; w < p_.wireCount() && !any; ++w) any = !p_.wireVia(w).empty();
    if (!any) return;
    out_ += "  layout {\n";
    layoutKeys();
    for (const NodeIndex v : keyOrder_) {
      const NodeLayout& l = p_.layout(v);
      if (!l.placed || key_[v].empty()) continue;
      out_ += "    node ";
      out_ += key_[v];
      out_ += " at (";
      appendFloat(out_, l.x);
      out_ += ", ";
      appendFloat(out_, l.y);
      out_ += ')';
      if (l.rotation != 0) {
        out_ += " rot ";
        out_ += std::to_string(l.rotation);
      }
      if (l.flip) out_ += " flip";
      out_ += '\n';
    }
    // Routed wires, by target (an input has one driver, so (target's key order, input) is a unique, stable key).
    std::vector<std::pair<uint64_t, WireIndex>> routed;
    for (WireIndex w = 0; w < p_.wireCount(); ++w) {
      const Wire& wr = p_.wire(w);
      if (p_.wireVia(w).empty() || wr.from.node >= n || wr.to.node >= n) continue;
      routed.emplace_back((static_cast<uint64_t>(keyPos_[wr.to.node]) << 16) | wr.to.port, w);
    }
    std::sort(routed.begin(), routed.end());
    for (const auto& [key, w] : routed) {
      const auto via = p_.wireVia(w);
      const Wire& wr = p_.wire(w);
      if (key_[wr.from.node].empty() || key_[wr.to.node].empty()) continue;
      out_ += "    wire ";
      out_ += key_[wr.from.node];
      out_ += '.';
      out_ += portTextName(p_, wr.from.node, wr.from.port, false);
      out_ += " -> ";
      out_ += key_[wr.to.node];
      out_ += '.';
      out_ += portTextName(p_, wr.to.node, wr.to.port, true);
      out_ += " via (";
      for (size_t i = 0; i < via.size(); ++i) {
        if (i) out_ += ", ";
        out_ += '(';
        appendFloat(out_, via[i].x);
        out_ += ", ";
        appendFloat(out_, via[i].y);
        out_ += ')';
      }
      out_ += ")\n";
    }
    for (const LayoutComment& c : p_.comments()) {
      out_ += "    comment ";
      appendString(out_, c.text);
      out_ += " at (";
      appendFloat(out_, c.at.x);
      out_ += ", ";
      appendFloat(out_, c.at.y);
      out_ += ")\n";
    }
    out_ += "  }\n";
  }

  const Patch& p_;
  std::string& out_;
  Adjacency adj_;
  const OperatorAtoms& ops_;
  std::vector<uint8_t> inline_;
  std::vector<uint32_t> depth_;
  std::vector<std::string> genName_;
  std::vector<uint32_t> rank_;  // print order (see plan())
  std::vector<std::string> eqName_;  // equations: their layout key `_e<k>`
  std::vector<std::string> key_;     // layoutKeys(): every node's layout key
  std::vector<uint32_t> keyPos_;     // … its position in keyOrder_
  std::vector<NodeIndex> keyOrder_;  // … the canonical order of layout lines
};

}  // namespace

std::string print(const Patch& patch) {
  std::string out;
  out.reserve(64 + static_cast<size_t>(patch.nodeCount()) * 24);
  PatchPrinter(patch, out).run();
  return out;
}

std::string printLayout(const Patch& patch) {
  std::string out;
  PatchPrinter(patch, out).runLayout();
  return out;
}

Result<std::string> spliceLayout(std::string_view source, const ParseResult& parsed, const PatchFile& laidOut) {
  if (!(parsed.ok() || parsed.lowered) || parsed.patchSpans.size() != laidOut.patches.size())
    return Error{ErrorCode::InvalidArgument, "spliceLayout: the source did not parse, or the patch counts differ", {}};
  std::string out;
  out.reserve(source.size() + 256);
  size_t at = 0;  // source bytes copied so far
  const bool crlf = source.find("\r\n") != std::string_view::npos;  // the block follows the file's line endings
  for (size_t i = 0; i < laidOut.patches.size(); ++i) {
    const PatchTextSpans& ps = parsed.patchSpans[i];
    if (ps.close == kNone || ps.close > source.size() || ps.close < at)
      return Error{ErrorCode::InvalidArgument, "spliceLayout: bad patch spans", {}};
    std::string block = printLayout(laidOut.patches[i]);  // "  layout {\n … \n  }\n" or ""
    if (crlf && !block.empty()) {
      std::string b;
      b.reserve(block.size() + 64);
      for (const char c : block) {
        if (c == '\n') b += '\r';
        b += c;
      }
      block = std::move(b);
    }
    if (ps.layout.offset != kNone) {
      // Replace from the `layout` keyword to its '}': the indentation before the keyword and whatever follows the
      // '}' stay. Without a new block the whole line goes (when the block is alone on its lines).
      size_t b = ps.layout.offset, e = static_cast<size_t>(ps.layout.offset) + ps.layout.length;
      if (b < at || e > source.size()) return Error{ErrorCode::InvalidArgument, "spliceLayout: bad layout span", {}};
      if (block.empty()) {
        size_t lb = b;
        while (lb > at && (source[lb - 1] == ' ' || source[lb - 1] == '\t')) --lb;
        size_t le = e;
        while (le < source.size() && (source[le] == ' ' || source[le] == '\t' || source[le] == '\r')) ++le;
        if ((lb == 0 || source[lb - 1] == '\n') && le < source.size() && source[le] == '\n') {
          b = lb;
          e = le + 1;
        }
        out.append(source.substr(at, b - at));
      } else {
        out.append(source.substr(at, b - at));
        // without the leading indent and the final line ending
        out.append(std::string_view(block).substr(2, block.size() - 2 - (crlf ? 2 : 1)));
      }
      at = e;
    } else if (!block.empty()) {
      // Insert before the closing '}': at the start of its line when only blanks precede it, else on a new line.
      size_t lb = ps.close;
      while (lb > at && (source[lb - 1] == ' ' || source[lb - 1] == '\t')) --lb;
      if (lb == 0 || source[lb - 1] == '\n') {
        out.append(source.substr(at, lb - at));
        out.append(block);  // no blank line added: removing the block again gives the original text back
        at = lb;
      } else {
        out.append(source.substr(at, ps.close - at));
        out += crlf ? "\r\n" : "\n";
        out.append(block);
        at = ps.close;
      }
    }
  }
  out.append(source.substr(at));
  return out;
}

std::string print(const PatchFile& file) {
  std::string out;
  size_t estimate = 64;
  for (const Patch& p : file.patches) estimate += static_cast<size_t>(p.nodeCount()) * 24 + 32;
  out.reserve(estimate);
  for (const std::string& imp : file.imports) {
    out += "import ";
    appendString(out, imp);
    out += '\n';
  }
  for (size_t i = 0; i < file.patches.size(); ++i) {
    if (i || !file.imports.empty()) out += '\n';
    PatchPrinter(file.patches[i], out).run();
  }
  return out;
}

}  // namespace pq::graph
