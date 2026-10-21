// Structural validation of a Patch (T1.1). Rules:
//
// Nodes
//  - Atom: a registered atom with exactly its signature's port counts (variadic: at least fixed + minRepeat).
//  - Const: kind real/bool, finite literal; a bool literal is 0 or 1 without a unit.
//  - Param: kind real/bool and a default of that kind; finite numbers; a range only on real params, with
//    min ≤ default ≤ max (normalised values); attribute and argument names are identifiers.
//  - in/out: the declared kind is real or bool (the grammar requires a valueKind).
//  - Unknown/Equation: belong to an existing solve block.
//  - Subcircuit: callee name and port names are identifiers, port names unique per direction.
// Names
//  - param/in/out/export/unknown nodes are named; equations are not; other nodes may be.
//  - Names are identifiers, not keywords (except `in`/`out`, see isKeywordNotName), not `voice`/`transport`/`sr`, and unique within the patch (one namespace:
//    a binding target may be an output, PATCH_LANGUAGE §2).
//  - A name may bind one output (`y = svf(x).lp`) only on a multi-output-capable node, never on a declaration.
//  - Layout placement needs a name (layout text refers to nodes by name), finite coordinates and a rotation that
//    is a multiple of 90 in [0, 360).
// Wires ("dangling" = refers to something that does not exist)
//  - Both endpoints exist: source is an existing output, target an existing input.
//  - An input has at most one driver (summing is explicit: an add node).
//  - Declared types must be compatible where both sides are concrete: same kind (no implicit bool↔real,
//    PATCH_LANGUAGE §3), event rate only into event rate, and equal widths except that mono broadcasts into any
//    width (§3 Width). Rate and poly lifts (param → audio modulation, poly → global sum) are inference's job (T1.3).
//  - Routing points are finite.
// Ports
//  - Every non-optional input is driven ("dangling port"): atom inputs without a default, out/export/equation
//    inputs, and subcircuit inputs the callee gives no default.
// Solve blocks
//  - 1..kMaxSolveIterations iterations, at least one unknown, as many equations as unknowns.

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "pq/graph/patch.h"

namespace pq::graph {

namespace {

class Checker {
public:
  explicit Checker(const Patch& p) : p_(p) {}

  std::vector<Error> run() {
    checkPatchName();
    checkNodes();
    checkNames();
    checkWires();
    checkSolves();
    checkComments();
    return std::move(errors_);
  }

private:
  void fail(ErrorCode code, std::string msg) {
    errors_.push_back(Error{code, "patch '" + p_.name() + "': " + std::move(msg), {}});
  }

  std::string inPort(NodeIndex n, uint32_t i) const {
    return "input '" + std::string(p_.inputName(n, i)) + "' of " + p_.describe(n);
  }
  std::string outPort(NodeIndex n, uint32_t i) const {
    return "output '" + std::string(p_.outputName(n, i)) + "' of " + p_.describe(n);
  }

  void checkPatchName() {
    if (!isIdentifier(p_.name()) || isKeywordNotName(p_.name()))
      fail(ErrorCode::InvalidArgument, "patch name '" + p_.name() + "' is not a valid identifier");
  }

  static bool finite(double v) noexcept { return std::isfinite(v); }

  // `what` builds the description only when there is something to report (validation of a valid patch should
  // not allocate a message per node).
  template <class What>
  void checkConstant(const Constant& c, Kind expected, What&& what) {
    if (c.kind != Kind::Real && c.kind != Kind::Bool) {
      fail(ErrorCode::InvalidArgument, what() + " has no kind");
      return;
    }
    if (expected != Kind::Infer && c.kind != expected)
      fail(ErrorCode::Type, what() + " is " + std::string(toText(c.kind)) + ", expected " +
                                std::string(toText(expected)));
    if (!finite(c.literal) || !finite(c.value())) fail(ErrorCode::InvalidArgument, what() + " is not finite");
    if (c.kind == Kind::Bool && (c.unit != Unit::None || (c.literal != 0.0 && c.literal != 1.0)))
      fail(ErrorCode::InvalidArgument, what() + " is not a bool literal");
  }

  void checkAttrs(const std::vector<Attr>& attrs, NodeIndex n) {
    for (const Attr& a : attrs) {
      auto where = [&] { return "param " + p_.describe(n) + ": attribute @" + a.name; };
      if (!isIdentifier(a.name)) fail(ErrorCode::InvalidArgument, where() + ": the name is invalid");
      for (const AttrArg& arg : a.args) {
        if (!arg.name.empty() && !isIdentifier(arg.name))
          fail(ErrorCode::InvalidArgument, where() + " has an invalid argument name '" + arg.name + "'");
        auto argWhat = [&] { return where() + " argument"; };
        switch (arg.value.type) {
          case AttrValue::Type::Number: checkConstant(arg.value.number, Kind::Real, argWhat); break;
          case AttrValue::Type::Bool: checkConstant(arg.value.number, Kind::Bool, argWhat); break;
          case AttrValue::Type::Ident:
            if (!isIdentifier(arg.value.text))
              fail(ErrorCode::InvalidArgument, where() + " has an invalid identifier argument '" + arg.value.text + "'");
            break;
          case AttrValue::Type::String: break;
        }
      }
    }
  }

  void checkParam(NodeIndex n) {
    const ParamDecl& d = p_.paramDecl(n);
    auto what = [&] { return "param " + p_.describe(n); };
    if (d.kind != Kind::Real && d.kind != Kind::Bool) {
      fail(ErrorCode::Type, what() + " must be real or bool");
      return;
    }
    checkConstant(d.defaultValue, d.kind, [&] { return what() + " default"; });
    if (d.hasRange) {
      if (d.kind != Kind::Real) {
        fail(ErrorCode::Type, what() + ": only real params have a range");
        return;
      }
      checkConstant(d.rangeMin, Kind::Real, [&] { return what() + " range min"; });
      checkConstant(d.rangeMax, Kind::Real, [&] { return what() + " range max"; });
      const double lo = d.rangeMin.value(), hi = d.rangeMax.value(), v = d.defaultValue.value();
      if (!(lo <= hi)) fail(ErrorCode::InvalidArgument, what() + ": range min is above range max");
      else if (!(v >= lo && v <= hi)) fail(ErrorCode::InvalidArgument, what() + ": default is outside the range");
    }
    checkAttrs(d.attrs, n);
  }

  void checkNodes() {
    const AtomRegistry& reg = AtomRegistry::builtin();
    for (NodeIndex n = 0; n < p_.nodeCount(); ++n) {
      const Node& nd = p_.node(n);
      switch (nd.kind) {
        case NodeKind::Atom: {
          if (nd.atom >= reg.size()) {
            fail(ErrorCode::NotFound, "node #" + std::to_string(n) + " refers to a nonexistent atom");
            break;
          }
          const AtomSignature& sig = reg.get(nd.atom);
          const bool varIn = sig.has(atom_flags::kVariadicInputs), varOut = sig.has(atom_flags::kVariadicOutputs);
          const bool inOk = varIn ? nd.numInputs >= sig.fixedInputs() + sig.minRepeat : nd.numInputs == sig.inputs.size();
          const bool outOk =
              varOut ? nd.numOutputs >= sig.fixedOutputs() + sig.minRepeat : nd.numOutputs == sig.outputs.size();
          if (!inOk || !outOk)
            fail(ErrorCode::InvalidArgument, p_.describe(n) + " has the wrong number of ports for its atom");
          break;
        }
        case NodeKind::Const:
          checkConstant(p_.constant(n), Kind::Infer, [&] { return "constant " + p_.describe(n); });
          break;
        case NodeKind::Param: checkParam(n); break;
        case NodeKind::Input:
        case NodeKind::Output: {
          const PortType& t = nd.kind == NodeKind::Input ? p_.outputType(n, 0) : p_.inputType(n, 0);
          if (t.kind == Kind::Infer)
            fail(ErrorCode::Type, p_.describe(n) + " needs a declared kind (real, bool or audio)");
          break;
        }
        case NodeKind::Unknown:
        case NodeKind::Equation:
          if (nd.payload >= p_.solveCount())
            fail(ErrorCode::NotFound, p_.describe(n) + " belongs to a nonexistent solve block");
          break;
        case NodeKind::Subcircuit: checkSubcircuit(n); break;
        case NodeKind::Export: break;
      }
      const NodeLayout& l = p_.layout(n);
      if (l.placed) {
        if (p_.nodeName(n).empty())
          fail(ErrorCode::InvalidArgument, p_.describe(n) + " has a layout position but no name to refer to it by");
        if (!std::isfinite(l.x) || !std::isfinite(l.y))
          fail(ErrorCode::InvalidArgument, p_.describe(n) + " has a non-finite layout position");
        if (l.rotation < 0 || l.rotation >= 360 || l.rotation % 90 != 0)
          fail(ErrorCode::InvalidArgument, p_.describe(n) + " has rotation " + std::to_string(l.rotation) +
                                               " (must be 0, 90, 180 or 270)");
      }
    }
  }

  void checkSubcircuit(NodeIndex n) {
    const SubcircuitRef& r = p_.subcircuit(n);
    if (!isIdentifier(r.patch) || isKeywordNotName(r.patch))
      fail(ErrorCode::InvalidArgument, p_.describe(n) + " calls a patch with an invalid name");
    auto checkPorts = [&](const std::vector<SubcircuitPort>& ports, const char* dir) {
      std::vector<std::string_view> names;
      for (const SubcircuitPort& sp : ports) {
        if (!isIdentifier(sp.name))
          fail(ErrorCode::InvalidArgument, p_.describe(n) + " has an invalid " + dir + " name '" + sp.name + "'");
        names.push_back(sp.name);
      }
      std::sort(names.begin(), names.end());
      if (std::adjacent_find(names.begin(), names.end()) != names.end())
        fail(ErrorCode::InvalidArgument, p_.describe(n) + " has duplicate " + dir + " names");
    };
    checkPorts(r.inputs, "input");
    checkPorts(r.outputs, "output");
  }

  void checkNames() {
    std::vector<std::pair<std::string_view, NodeIndex>> named;
    for (NodeIndex n = 0; n < p_.nodeCount(); ++n) {
      const Node& nd = p_.node(n);
      const std::string& name = p_.nodeName(n);
      const bool declaration = nd.kind == NodeKind::Param || nd.kind == NodeKind::Input ||
                               nd.kind == NodeKind::Output || nd.kind == NodeKind::Export ||
                               nd.kind == NodeKind::Unknown;
      if (name.empty()) {
        if (declaration) fail(ErrorCode::InvalidArgument, p_.describe(n) + " must be named");
        continue;
      }
      if (nd.kind == NodeKind::Equation) fail(ErrorCode::InvalidArgument, "equations cannot be named ('" + name + "')");
      if (!isIdentifier(name)) fail(ErrorCode::InvalidArgument, "'" + name + "' is not a valid identifier");
      else if (isKeywordNotName(name)) fail(ErrorCode::InvalidArgument, "'" + name + "' is a keyword");
      else if (isReservedName(name))
        fail(ErrorCode::InvalidArgument, "'" + name + "' would shadow a built-in namespace");
      if (nd.nameOutput != kWholeNode && (declaration || nd.nameOutput >= nd.numOutputs))
        fail(ErrorCode::InvalidArgument, "name '" + name + "' binds a nonexistent output");
      named.emplace_back(name, n);
    }
    std::sort(named.begin(), named.end());
    for (size_t i = 1; i < named.size(); ++i)
      if (named[i].first == named[i - 1].first)
        fail(ErrorCode::InvalidArgument,
             "name '" + std::string(named[i].first) + "' is used by " + p_.describe(named[i - 1].second) + " and " +
                 p_.describe(named[i].second));
  }

  void checkWires() {
    const uint32_t n = p_.nodeCount();
    std::vector<uint32_t> drivers(p_.portCount(), 0);
    for (WireIndex w = 0; w < p_.wireCount(); ++w) {
      const Wire& wr = p_.wire(w);
      auto wname = [w] { return "wire #" + std::to_string(w); };
      bool ok = true;
      if (wr.from.node >= n) {
        fail(ErrorCode::NotFound, "dangling " + wname() + ": source node #" + std::to_string(wr.from.node) +
                                      " does not exist");
        ok = false;
      } else if (wr.from.port >= p_.node(wr.from.node).numOutputs) {
        fail(ErrorCode::NotFound, "dangling " + wname() + ": " + p_.describe(wr.from.node) + " has no output #" +
                                      std::to_string(wr.from.port));
        ok = false;
      }
      if (wr.to.node >= n) {
        fail(ErrorCode::NotFound, "dangling " + wname() + ": target node #" + std::to_string(wr.to.node) +
                                      " does not exist");
        ok = false;
      } else if (wr.to.port >= p_.node(wr.to.node).numInputs) {
        fail(ErrorCode::NotFound, "dangling " + wname() + ": " + p_.describe(wr.to.node) + " has no input #" +
                                      std::to_string(wr.to.port));
        ok = false;
      }
      for (const Point2& pt : p_.wireVia(w))
        if (!std::isfinite(pt.x) || !std::isfinite(pt.y)) {
          fail(ErrorCode::InvalidArgument, wname() + " has a non-finite routing point");
          break;
        }
      if (!ok) continue;
      if (++drivers[p_.flatInput(wr.to.node, wr.to.port)] == 2)
        fail(ErrorCode::InvalidArgument, inPort(wr.to.node, wr.to.port) + " is driven by more than one wire");
      checkWireTypes(wr);
    }
    for (NodeIndex i = 0; i < n; ++i)
      for (uint32_t k = 0; k < p_.node(i).numInputs; ++k)
        if (drivers[p_.flatInput(i, k)] == 0 && !p_.inputOptional(i, k))
          fail(ErrorCode::InvalidArgument, "dangling port: " + inPort(i, k) + " is not connected");
  }

  void checkWireTypes(const Wire& w) {
    const PortType& a = p_.outputType(w.from.node, w.from.port);
    const PortType& b = p_.inputType(w.to.node, w.to.port);
    auto what = [&] { return outPort(w.from.node, w.from.port) + " → " + inPort(w.to.node, w.to.port); };
    if (a.kind != Kind::Infer && b.kind != Kind::Infer && a.kind != b.kind) {
      const char* hint = a.kind == Kind::Bool ? " (convert with gate_to_real)" : " (convert with a comparison)";
      fail(ErrorCode::Type, "kind mismatch: " + std::string(toText(a.kind)) + " into " +
                                std::string(toText(b.kind)) + ": " + what() + hint);
    }
    if (a.rate != Rate::Infer && b.rate != Rate::Infer && ((a.rate == Rate::Event) != (b.rate == Rate::Event)))
      fail(ErrorCode::Type, "rate mismatch: " + std::string(toText(a.rate)) + " into " +
                                std::string(toText(b.rate)) + ": " + what());
    if (a.width != kWidthInfer && b.width != kWidthInfer && a.width != b.width && a.width != kWidthMono)
      fail(ErrorCode::Type, "width mismatch: " + std::to_string(a.width) + " channels into " +
                                std::to_string(b.width) + ": " + what());
  }

  void checkSolves() {
    std::vector<uint32_t> unknowns(p_.solveCount(), 0), equations(p_.solveCount(), 0);
    for (const Node& nd : p_.nodes()) {
      if (nd.payload >= p_.solveCount()) continue;
      if (nd.kind == NodeKind::Unknown) ++unknowns[nd.payload];
      if (nd.kind == NodeKind::Equation) ++equations[nd.payload];
    }
    for (SolveIndex s = 0; s < p_.solveCount(); ++s) {
      const std::string what = "solve block #" + std::to_string(s);
      const uint32_t it = p_.solves()[s].iterations;
      if (it < 1 || it > kMaxSolveIterations)
        fail(ErrorCode::InvalidArgument, what + " has " + std::to_string(it) + " iterations (must be 1.." +
                                             std::to_string(kMaxSolveIterations) + ")");
      if (unknowns[s] == 0) fail(ErrorCode::InvalidArgument, what + " has no unknowns");
      if (unknowns[s] != equations[s])
        fail(ErrorCode::InvalidArgument, what + " has " + std::to_string(unknowns[s]) + " unknowns but " +
                                             std::to_string(equations[s]) + " equations");
    }
  }

  void checkComments() {
    for (const LayoutComment& c : p_.comments())
      if (!std::isfinite(c.at.x) || !std::isfinite(c.at.y))
        fail(ErrorCode::InvalidArgument, "layout comment '" + c.text + "' has a non-finite position");
  }

  const Patch& p_;
  std::vector<Error> errors_;
};

}  // namespace

std::vector<Error> Patch::validateAll() const { return Checker(*this).run(); }

}  // namespace pq::graph
