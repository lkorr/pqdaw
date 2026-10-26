// Editing operations for the circuit editor (T4.4): pq/graph/edit.h.

#include "pq/graph/edit.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "pq/graph/atoms.h"
#include "pq/graph/parse.h"

namespace pq::graph::edit {

namespace {

Error refuse(std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message), {}}; }

const PortSpec& inputSpec(const AtomSignature& sig, uint32_t k) noexcept {
  return sig.inputs[std::min<size_t>(k, sig.inputs.size() - 1)];
}

bool isDeclaration(NodeKind k) noexcept {
  return k == NodeKind::Param || k == NodeKind::Input || k == NodeKind::Output || k == NodeKind::Export ||
         k == NodeKind::Unknown;
}

// The name of node v as the editor sees it (generated names are no names).
std::string_view ownName(const Patch& p, NodeIndex v) {
  const std::string& n = p.nodeName(v);
  return isGeneratedName(n) ? std::string_view() : std::string_view(n);
}

// The variadic port count addAtom() needs to rebuild node v.
uint16_t repeatOf(const Patch& p, NodeIndex v) {
  const AtomSignature& sig = atomSignature(p.node(v).atom);
  if (sig.has(atom_flags::kVariadicInputs)) return static_cast<uint16_t>(p.node(v).numInputs - sig.fixedInputs());
  if (sig.has(atom_flags::kVariadicOutputs)) return static_cast<uint16_t>(p.node(v).numOutputs - sig.fixedOutputs());
  return 0;
}

// A copy of node v of `from` in `to` (kind, payload, ports, shared flag); no name, layout or wires.
NodeIndex copyNode(const Patch& from, NodeIndex v, Patch& to) {
  const Node& nd = from.node(v);
  switch (nd.kind) {
    case NodeKind::Atom: {
      const NodeIndex n = to.addAtom(nd.atom, repeatOf(from, v));
      to.setShared(n, nd.shared);
      return n;
    }
    case NodeKind::Const: return to.addConst(from.constant(v));
    case NodeKind::Subcircuit: return to.addSubcircuit(from.subcircuit(v));
    default: return kNone;
  }
}

}  // namespace

bool isGeneratedName(std::string_view name) noexcept {
  if (name.size() < 3 || name[0] != '_' || name[1] != 'n') return false;
  for (size_t i = 2; i < name.size(); ++i)
    if (name[i] < '0' || name[i] > '9') return false;
  return true;
}

uint32_t anonymize(Patch& p) {
  uint32_t n = 0;
  for (NodeIndex v = 0; v < p.nodeCount(); ++v)
    if (!isDeclaration(p.node(v).kind) && isGeneratedName(p.nodeName(v))) {
      p.setNodeName(v, {}, kWholeNode);
      ++n;
    }
  return n;
}

std::string uniqueName(const Patch& p, std::string_view base) {
  auto used = [&](std::string_view s) {
    for (NodeIndex v = 0; v < p.nodeCount(); ++v)
      if (p.nodeName(v) == s) return true;
    return false;
  };
  if (!used(base) && checkName(p, base).ok()) return std::string(base);
  for (uint32_t k = 1;; ++k) {
    std::string s = std::string(base) + std::to_string(k);
    if (!used(s)) return s;
  }
}

Status checkName(const Patch& p, std::string_view name, NodeIndex self) {
  if (!isIdentifier(name)) return refuse("'" + std::string(name) + "' is not a name (letters, digits and _, not first a digit)");
  if (isKeywordNotName(name)) return refuse("'" + std::string(name) + "' is a keyword");
  if (isReservedName(name) || name == "track" || name == "sidechain" || name == "shared")
    return refuse("'" + std::string(name) + "' is a built-in name");
  if (isGeneratedName(name)) return refuse("names like '" + std::string(name) + "' are reserved for generated names");
  for (NodeIndex v = 0; v < p.nodeCount(); ++v)
    if (v != self && p.nodeName(v) == name) return refuse("'" + std::string(name) + "' is already used in this patch");
  return Ok{};
}

// ---- wires ------------------------------------------------------------------------------------------------------------

bool inputDelayed(const Patch& p, NodeIndex n, uint32_t input) noexcept {
  const Node& nd = p.node(n);
  if (nd.kind != NodeKind::Atom || input >= nd.numInputs) return false;
  return inputSpec(atomSignature(nd.atom), input).delayed;
}

bool closesDelayFreeCycle(const Patch& p, PortRef from, PortRef to) {
  if (inputDelayed(p, to.node, to.port)) return false;
  if (from.node == to.node) return true;
  // Is there a path to.node ⇝ from.node through inputs that do not delay?
  const Adjacency adj = p.adjacency();
  std::vector<uint8_t> seen(p.nodeCount(), 0);
  std::vector<NodeIndex> stack{to.node};
  seen[to.node] = 1;
  while (!stack.empty()) {
    const NodeIndex v = stack.back();
    stack.pop_back();
    for (const WireIndex w : adj.outgoing(v)) {
      const Wire& wr = p.wire(w);
      const NodeIndex u = wr.to.node;
      if (seen[u] || inputDelayed(p, u, wr.to.port)) continue;
      if (u == from.node) return true;
      seen[u] = 1;
      stack.push_back(u);
    }
  }
  return false;
}

bool disconnectInput(Patch& p, PortRef to) {
  for (WireIndex w = 0; w < p.wireCount(); ++w)
    if (p.wire(w).to == to) {
      p.disconnect(w);
      return true;
    }
  return false;
}

Result<ConnectResult> connect(Patch& p, PortRef from, PortRef to) {
  if (from.node >= p.nodeCount() || to.node >= p.nodeCount()) return refuse("no such node");
  if (from.port >= p.node(from.node).numOutputs) return refuse("no such output");
  if (to.port >= p.node(to.node).numInputs) return refuse("no such input");
  const Kind ko = p.outputType(from.node, from.port).kind, ki = p.inputType(to.node, to.port).kind;
  if (ko != Kind::Infer && ki != Kind::Infer && ko != ki)
    return refuse(ko == Kind::Bool ? "a bool output cannot drive a real input (convert it: gate_to_real(x))"
                                   : "a real output cannot drive a bool input (compare it: x > 0)");
  ConnectResult r;
  const bool cycle = closesDelayFreeCycle(p, from, to);
  r.replaced = disconnectInput(p, to);
  if (!cycle) {
    r.wire = p.connect(from, to);
    return r;
  }
  r.delay = p.addAtom(atomId("z1"));
  const NodeLayout& a = p.layout(from.node);
  const NodeLayout& b = p.layout(to.node);
  if (a.placed && b.placed) {
    NodeLayout l;
    l.placed = true;
    l.x = std::round((a.x + b.x) * 0.125f) * 4.0f;
    l.y = std::round((std::max(a.y, b.y) + 48.0f) * 0.25f) * 4.0f;
    p.setLayout(r.delay, l);
  }
  p.connect(from, PortRef{r.delay, 0});
  r.wire = p.connect(PortRef{r.delay, 0}, to);
  return r;
}

// ---- nodes ------------------------------------------------------------------------------------------------------------

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

struct OpName {
  std::string_view text, atom;
};
constexpr OpName kOps[] = {{"+", "add"}, {"-", "sub"},  {"*", "mul"}, {"/", "div"}, {"%", "mod"}, {"<", "lt"},
                           {"<=", "le"}, {">", "gt"},   {">=", "ge"}, {"==", "eq"}, {"!=", "ne"}, {"&&", "and"},
                           {"||", "or"}, {"^^", "xor"}, {"!", "not"}, {"-x", "neg"}};

// Parses `patch __add { <body> }` (no validation, missing inputs allowed) and returns the patch.
Result<Patch> parseSnippet(std::string_view body) {
  std::string text = "patch __add {\n";
  text += body;
  text += "\n}\n";
  ParseOptions o;
  o.validate = false;
  o.deferMissingInputs = true;
  ParseResult r = parseDetailed(text, "<add>", o);
  if (!r.ok() || r.file.patches.size() != 1) {
    std::string m = r.diagnostics.empty() ? std::string("cannot read it") : r.diagnostics.front().message;
    return refuse(m);
  }
  return std::move(r.file.patches.front());
}

}  // namespace

Result<NodeIndex> addNode(Patch& p, std::string_view specIn, std::span<const SubcircuitRef> callees) {
  const std::string_view spec = trim(specIn);
  if (spec.empty()) return refuse("type an atom, a value, a patch or a declaration");
  // Operators.
  for (const OpName& op : kOps)
    if (spec == op.text) return p.addAtom(atomId(op.atom));
  const std::string_view head = spec.substr(0, spec.find_first_of(" \t:="));
  // Declarations, read by the parser itself.
  if (head == "param" || head == "in" || head == "out" || head == "export") {
    std::string body(spec);
    auto snip = parseSnippet(body);
    if (!snip) return std::move(snip).error();
    const Patch& s = *snip;
    for (NodeIndex v = 0; v < s.nodeCount(); ++v) {
      const Node& nd = s.node(v);
      if (!isDeclaration(nd.kind)) continue;
      const std::string& name = s.nodeName(v);
      if (Status ok = checkName(p, name); !ok) return std::move(ok).error();
      switch (nd.kind) {
        case NodeKind::Param: return p.addParam(name, s.paramDecl(v));
        case NodeKind::Input: {
          // `in x : t = <literal>`: the default comes along (a computed default is written in the text).
          const Adjacency adj = s.adjacency();
          const WireIndex d = adj.driver[s.flatInput(v, 0)];
          if (d != kNone && s.node(s.wire(d).from.node).kind != NodeKind::Const)
            return refuse("an input's default here must be a literal; write a computed default in the text");
          const NodeIndex in = p.addInput(name, s.outputType(v, 0));
          if (d != kNone) p.connect(p.addConst(s.constant(s.wire(d).from.node)), in, 0);
          return in;
        }
        case NodeKind::Output: return p.addOutput(name, s.inputType(v, 0));
        case NodeKind::Export: return p.addExport(name);
        default: break;
      }
    }
    return refuse("expected a declaration");
  }
  // Literals.
  const char c0 = spec.front();
  if ((c0 >= '0' && c0 <= '9') || c0 == '.' || (c0 == '-' && spec.size() > 1) || spec == "true" || spec == "false") {
    auto snip = parseSnippet("__v = " + std::string(spec));
    if (!snip) return std::move(snip).error();
    const Patch& s = *snip;
    if (s.nodeCount() != 1 || s.node(0).kind != NodeKind::Const) return refuse("'" + std::string(spec) + "' is not a literal");
    return p.addConst(s.constant(0));
  }
  // Atoms, then callees.
  if (auto id = AtomRegistry::builtin().find(spec)) return p.addAtom(*id);
  for (const SubcircuitRef& r : callees)
    if (r.patch == spec) return p.addSubcircuit(r);
  return refuse("no atom or patch named '" + std::string(spec) + "'");
}

Result<std::vector<size_t>> rename(PatchFile& file, size_t patch, NodeIndex n, std::string name) {
  if (patch >= file.patches.size()) return refuse("no such patch");
  Patch& p = file.patches[patch];
  if (n >= p.nodeCount()) return refuse("no such node");
  const NodeKind kind = p.node(n).kind;
  if (kind == NodeKind::Equation) return refuse("an equation has no name");
  const std::string old = p.nodeName(n);
  if (name == old) return std::vector<size_t>{};
  if (name.empty()) {
    if (isDeclaration(kind)) return refuse("a declaration needs a name");
    p.setNodeName(n, {}, kWholeNode);
    return std::vector<size_t>{patch};
  }
  if (Status ok = checkName(p, name, n); !ok) return std::move(ok).error();
  p.setNodeName(n, name, p.node(n).nameOutput);
  std::vector<size_t> changed{patch};
  if (kind == NodeKind::Param || kind == NodeKind::Input || kind == NodeKind::Output || kind == NodeKind::Export) {
    const std::pair<std::string, std::string> map[1] = {{old, name}};
    for (size_t q : refreshCallers(file, patch, map)) changed.push_back(q);
  }
  return changed;
}

std::vector<size_t> eraseNodes(PatchFile& file, size_t patch, std::span<const NodeIndex> nodes) {
  if (patch >= file.patches.size()) return {};
  file.patches[patch].eraseNodes(nodes);
  std::vector<size_t> changed{patch};
  for (size_t q : refreshCallers(file, patch)) changed.push_back(q);
  return changed;
}

std::vector<size_t> refreshCallers(PatchFile& file, size_t callee,
                                   std::span<const std::pair<std::string, std::string>> renamed) {
  std::vector<size_t> changed;
  if (callee >= file.patches.size()) return changed;
  const SubcircuitRef sig = subcircuitSignature(file.patches[callee]);
  auto mapped = [&](const std::string& name) -> const std::string& {
    for (const auto& [from, to] : renamed)
      if (from == name) return to;
    return name;
  };
  auto find = [](const std::vector<SubcircuitPort>& ports, const std::string& name) -> uint32_t {
    for (uint32_t k = 0; k < ports.size(); ++k)
      if (ports[k].name == name) return k;
    return kNone;
  };
  for (size_t q = 0; q < file.patches.size(); ++q) {
    if (q == callee) continue;
    Patch& p = file.patches[q];
    std::vector<NodeIndex> old;
    for (NodeIndex v = 0; v < p.nodeCount(); ++v)
      if (p.node(v).kind == NodeKind::Subcircuit && p.subcircuit(v).patch == sig.patch && !(p.subcircuit(v) == sig))
        old.push_back(v);
    if (old.empty()) continue;
    for (const NodeIndex v : old) {
      const SubcircuitRef was = p.subcircuit(v);
      const NodeIndex m = p.addSubcircuit(sig);
      p.setLayout(m, p.layout(v));
      if (!p.nodeName(v).empty()) {
        uint16_t no = p.node(v).nameOutput;
        if (no != kWholeNode) {
          const uint32_t k = find(sig.outputs, mapped(was.outputs[no].name));
          no = k == kNone ? kWholeNode : static_cast<uint16_t>(k);
        }
        p.setNodeName(m, p.nodeName(v), no);
      }
      const uint32_t wires = p.wireCount();
      for (WireIndex w = 0; w < wires; ++w) {
        const Wire wr = p.wire(w);
        PortRef from = wr.from, to = wr.to;
        if (to.node == v) {
          const uint32_t k = find(sig.inputs, mapped(was.inputs[to.port].name));
          if (k == kNone) continue;
          to = {m, static_cast<uint16_t>(k)};
        } else if (from.node == v) {
          const uint32_t k = find(sig.outputs, mapped(was.outputs[from.port].name));
          if (k == kNone) continue;
          from = {m, static_cast<uint16_t>(k)};
        } else {
          continue;
        }
        const std::vector<Point2> via(p.wireVia(w).begin(), p.wireVia(w).end());
        p.setWireVia(p.connect(from, to), via);
      }
    }
    p.eraseNodes(old);
    changed.push_back(q);
  }
  return changed;
}

Result<NodeIndex> constToParam(Patch& p, NodeIndex c, std::string name) {
  if (c >= p.nodeCount() || p.node(c).kind != NodeKind::Const) return refuse("only a constant can become a param pin");
  const Adjacency adj = p.adjacency();
  if (name.empty()) {
    std::string base = "p";
    const auto out = adj.outgoing(c);
    if (!out.empty()) {
      const Wire& w = p.wire(out.front());
      const std::string_view in = p.inputName(w.to.node, w.to.port);
      if (isIdentifier(in) && checkName(p, in).ok()) base = std::string(in);
      else if (isIdentifier(in)) base = std::string(in);
    }
    name = uniqueName(p, base);
  } else if (Status ok = checkName(p, name, c); !ok) {
    return std::move(ok).error();
  }
  ParamDecl decl;
  decl.kind = p.constant(c).kind == Kind::Bool ? Kind::Bool : Kind::Real;
  decl.defaultValue = p.constant(c);
  const NodeIndex k = p.addParam(name, decl);
  p.setLayout(k, p.layout(c));
  std::vector<WireIndex> out(adj.outgoing(c).begin(), adj.outgoing(c).end());
  for (const WireIndex w : out) {
    const std::vector<Point2> via(p.wireVia(w).begin(), p.wireVia(w).end());
    p.setWireVia(p.connect(PortRef{k, 0}, p.wire(w).to), via);
  }
  const NodeIndex erase[1] = {c};
  p.eraseNodes(erase);
  return k - 1;
}

Result<MakeSubcircuitResult> makeSubcircuit(PatchFile& file, size_t patch, std::span<const NodeIndex> nodesIn,
                                            std::string name) {
  if (patch >= file.patches.size()) return refuse("no such patch");
  Patch& p = file.patches[patch];
  std::vector<NodeIndex> nodes(nodesIn.begin(), nodesIn.end());
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  if (nodes.empty()) return refuse("select the nodes to put into the subcircuit");
  std::vector<uint8_t> in(p.nodeCount(), 0);
  for (const NodeIndex v : nodes) {
    if (v >= p.nodeCount()) return refuse("no such node");
    const NodeKind k = p.node(v).kind;
    if (k != NodeKind::Atom && k != NodeKind::Const && k != NodeKind::Subcircuit)
      return refuse("declarations and solve blocks stay in the patch: select only computed nodes and constants");
    in[v] = 1;
  }
  // The new patch's name.
  auto patchNameFree = [&](const std::string& s) {
    return file.find(s) == nullptr && !AtomRegistry::builtin().find(s) && isIdentifier(s) && !isKeyword(s) &&
           !isReservedName(s);
  };
  if (name.empty()) {
    for (uint32_t k = 1;; ++k)
      if (patchNameFree(name = "Sub" + std::to_string(k))) break;
  } else if (!patchNameFree(name)) {
    return refuse("'" + name + "' cannot name a new patch (taken, an atom, or not a name)");
  }

  Patch sub(name);
  // Bounding box of the placed selection: the inner layout is translated to start near the origin.
  float x0 = std::numeric_limits<float>::max(), y0 = x0, x1 = -x0;
  for (const NodeIndex v : nodes)
    if (p.layout(v).placed) {
      x0 = std::min(x0, p.layout(v).x);
      y0 = std::min(y0, p.layout(v).y);
      x1 = std::max(x1, p.layout(v).x);
    }
  const bool placed = x0 != std::numeric_limits<float>::max();
  const float dx = placed ? 160.0f - x0 : 0.0f, dy = placed ? 40.0f - y0 : 0.0f;

  std::vector<NodeIndex> map(p.nodeCount(), kNone);
  for (const NodeIndex v : nodes) {
    const NodeIndex m = copyNode(p, v, sub);
    map[v] = m;
    if (const std::string_view nm = ownName(p, v); !nm.empty()) sub.setNodeName(m, std::string(nm), p.node(v).nameOutput);
    if (p.layout(v).placed) {
      NodeLayout l = p.layout(v);
      l.x += dx;
      l.y += dy;
      sub.setLayout(m, l);
    }
  }
  struct Port {
    PortRef source;     // in the parent
    NodeIndex decl;     // in the subcircuit
    std::string name;
  };
  std::vector<Port> ins, outs;
  auto findPort = [](std::vector<Port>& v, PortRef s) -> Port* {
    for (Port& x : v)
      if (x.source == s) return &x;
    return nullptr;
  };
  auto valueName = [&](PortRef s) -> std::string {
    const std::string_view nm = ownName(p, s.node);
    const uint16_t no = p.node(s.node).nameOutput;
    if (!nm.empty() && (no == kWholeNode ? s.port == 0 : no == s.port)) return std::string(nm);
    return {};
  };
  auto kindOf = [&](PortRef s) {
    return p.outputType(s.node, s.port).kind == Kind::Bool ? Kind::Bool : Kind::Real;
  };
  float inY = 40.0f, outY = 40.0f;
  std::vector<std::pair<PortRef, PortRef>> inside;  // wires to add to the subcircuit (after its ports exist)
  const std::vector<Wire> wires(p.wires().begin(), p.wires().end());
  for (WireIndex w = 0; w < wires.size(); ++w) {
    const Wire& wr = wires[w];
    const bool a = in[wr.from.node] != 0, b = in[wr.to.node] != 0;
    if (a && b) {
      const WireIndex nw = sub.connect(PortRef{map[wr.from.node], wr.from.port}, PortRef{map[wr.to.node], wr.to.port});
      if (placed) {
        std::vector<Point2> via(p.wireVia(w).begin(), p.wireVia(w).end());
        for (Point2& q : via) q = {q.x + dx, q.y + dy};
        sub.setWireVia(nw, std::move(via));
      }
    } else if (b) {  // entering
      Port* port = findPort(ins, wr.from);
      if (!port) {
        Port np;
        np.source = wr.from;
        const bool param = p.node(wr.from.node).kind == NodeKind::Param;
        std::string base = valueName(wr.from);
        if (base.empty()) base = ins.empty() ? "in" : "in" + std::to_string(ins.size());
        np.name = uniqueName(sub, base);
        np.decl = param ? sub.addParam(np.name, p.paramDecl(wr.from.node)) : sub.addInput(np.name, portType(kindOf(wr.from)));
        if (placed) {
          NodeLayout l;
          l.placed = true;
          l.x = 40.0f;
          l.y = inY;
          inY += 48.0f;
          sub.setLayout(np.decl, l);
        }
        ins.push_back(std::move(np));
        port = &ins.back();
      }
      inside.push_back({PortRef{port->decl, 0}, PortRef{map[wr.to.node], wr.to.port}});
    } else if (a) {  // leaving
      if (!findPort(outs, wr.from)) {
        Port np;
        np.source = wr.from;
        std::string base = valueName(wr.from);
        if (!base.empty()) {
          // The output takes the value's name (`env = adsr(…)` drives `out env`) unless the value is also read
          // inside, where it keeps its name and the output gets another.
          bool readInside = false;
          for (const Wire& x : wires)
            if (x.from == wr.from && in[x.to.node]) readInside = true;
          if (!readInside) sub.setNodeName(map[wr.from.node], {}, kWholeNode);
        } else {
          base = outs.empty() ? "out" : "out" + std::to_string(outs.size());
        }
        np.name = uniqueName(sub, base);
        np.decl = sub.addOutput(np.name, portType(kindOf(wr.from)));
        if (placed) {
          NodeLayout l;
          l.placed = true;
          l.x = x1 + dx + 160.0f;
          l.y = outY;
          outY += 48.0f;
          sub.setLayout(np.decl, l);
        }
        sub.connect(PortRef{map[wr.from.node], wr.from.port}, PortRef{np.decl, 0});
        outs.push_back(std::move(np));
      }
    }
  }
  for (const auto& [from, to] : inside) sub.connect(from, to);

  // The call in the parent.
  const SubcircuitRef sig = subcircuitSignature(sub);
  const NodeIndex call = p.addSubcircuit(sig);
  if (placed) {
    NodeLayout l;
    l.placed = true;
    l.x = x0;
    l.y = y0;
    p.setLayout(call, l);
  }
  auto sigIndex = [](const std::vector<SubcircuitPort>& ports, const std::string& n) -> uint16_t {
    for (uint16_t k = 0; k < ports.size(); ++k)
      if (ports[k].name == n) return k;
    return 0;
  };
  for (const Port& x : ins) p.connect(x.source, PortRef{call, sigIndex(sig.inputs, x.name)});
  for (WireIndex w = 0; w < wires.size(); ++w) {
    const Wire& wr = wires[w];
    if (in[wr.from.node] && !in[wr.to.node]) {
      const Port* x = findPort(outs, wr.from);
      p.connect(PortRef{call, sigIndex(sig.outputs, x->name)}, wr.to);
    }
  }
  // A name of a value that left the selection now names the call's output, so the text reads the same outside.
  if (outs.size() == 1) {
    const std::string nm = valueName(outs.front().source);
    if (!nm.empty()) {
      p.setNodeName(outs.front().source.node, {}, kWholeNode);
      p.setNodeName(call, nm, kWholeNode);
    }
  }
  p.eraseNodes(nodes);
  MakeSubcircuitResult r;
  r.instance = p.nodeCount() - 1;  // added last; every erased node was below it
  r.patch = patch + 1;
  file.patches.insert(file.patches.begin() + static_cast<std::ptrdiff_t>(patch + 1), std::move(sub));
  return r;
}

}  // namespace pq::graph::edit
