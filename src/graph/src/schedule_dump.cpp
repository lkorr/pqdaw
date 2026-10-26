// Text form of a Schedule (pq/graph/schedule.h): `pqc dump-schedule` (T1.10) and the expected dumps of the tests.
//
//   schedule 'Name': 2 stages, 3 regions, 7 ops; buffers: 2 block, 1 pattern; state: 64 B global, 96 B per voice group
//   params:    p0 'cutoff' real = 1000 [20..20000]
//   externals: e0 in#0 'x' ch0 audio global
//   stage 1 voice
//     region 2 loop, sub-block 1
//       #4 z1:read/8 () -> b3 [s0]  'lp'
//       #5 add/8 (b1, ~p0) -> b4  'y'          (~ marks a Spl input: an 8-float pattern)
//       solve #0 (8 iterations): ops #6..#7; unknowns v=b5 (init b2); equations b6 == b7
//   outputs:   out 'y' ch0 <- b4 (global)
//   state global (64 B): s0 @0 +32 z1/8 'lp' ch0

#include <bit>
#include <charconv>
#include <string>

#include "pq/graph/schedule.h"

namespace pq::graph {

namespace {

std::string num(double v) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof(buf), static_cast<float>(v));
  return std::string(buf, r.ptr);
}

class Dumper {
public:
  Dumper(const Schedule& s, const TypedPatch& tp, bool keys) : s_(s), p_(tp.patch), keys_(keys) {}

  std::string run() {
    line("schedule '" + p_.name() + "': " + std::to_string(s_.stages.size()) + " stages, " +
         std::to_string(s_.regions.size()) + " regions, " + std::to_string(s_.ops.size()) + " ops; buffers: " +
         std::to_string(s_.blockBuffers()) + " block, " + std::to_string(s_.patternBuffers()) + " pattern; state: " +
         std::to_string(s_.state.globalBytes) + " B global, " + std::to_string(s_.state.voiceBytes) +
         " B per voice group");
    for (const InsertedDelay& d : s_.insertedDelays)
      line("inserted z1 #" + std::to_string(d.z1) + " before input '" +
           std::string(p_.inputName(d.consumer, d.input)) + "' of " + label(d.consumer));
    if (!s_.params.empty()) {
      line("params:");
      for (size_t i = 0; i < s_.params.size(); ++i) {
        const ParamSlot& ps = s_.params[i];
        std::string t = "  p" + std::to_string(i) + " " + label(ps.node) + " " + std::string(toText(ps.kind)) + " = " +
                        (ps.kind == Kind::Bool ? std::string(ps.defaultValue != 0 ? "true" : "false")
                                               : num(ps.defaultValue));
        if (ps.hasRange) t += " [" + num(ps.rangeMin) + ".." + num(ps.rangeMax) + "]";
        if (keys_) t += " key " + hex(ps.key);
        line(t);
      }
    }
    if (!s_.externals.empty()) {
      line("externals:");
      for (size_t i = 0; i < s_.externals.size(); ++i) {
        const External& e = s_.externals[i];
        std::string t = "  e" + std::to_string(i) + " ";
        if (e.source == External::Source::RootInput) t += "in#" + std::to_string(e.index) + " " + label(e.node);
        else t += std::string(atomSignature(e.atom).name);
        t += " ch" + std::to_string(e.channel) + " " + std::string(toText(e.rate)) + " " + std::string(toText(e.poly));
        line(t);
      }
    }
    if (!s_.buffers.empty()) {
      std::string t = "buffers:";
      for (size_t i = 0; i < s_.buffers.size(); ++i) {
        const Buffer& b = s_.buffers[i];
        t += " b" + std::to_string(i) + "=" + (b.cls == Buffer::Class::Pattern ? "pat" : "blk" + std::to_string(b.lanes));
        if (b.perGroup) t += "/group";
      }
      line(t);
    }
    for (const Stage& sg : s_.stages) {
      line("stage " + std::to_string(sg.index) + " " + std::string(toText(sg.poly)));
      for (uint32_t r = sg.firstRegion; r < sg.firstRegion + sg.numRegions; ++r) region(r);
    }
    if (!s_.outputs.empty()) {
      line("outputs:");
      for (const OutputRef& o : s_.outputs) {
        std::string t = std::string("  ") + (o.isExport ? "export " : "out ") + label(o.node) + " ch" +
                        std::to_string(o.channel) + " <- " + operand(o.value) + " (" + std::string(toText(o.poly));
        if (o.poly == Poly::Voice) t += ", end of stage " + std::to_string(o.stage);
        line(t + ")");
      }
    }
    if (!s_.probes.empty()) {  // T4.5
      line("probes:");
      for (const ProbeRef& p : s_.probes) {
        std::string t = "  probe " + std::to_string(p.id) + " <- " + operand(p.value) + " (" + std::string(toText(p.poly));
        if (p.poly == Poly::Voice) t += std::string(", after each group of stage ") + std::to_string(s_.stages[p.stage].index);
        if (p.region != kNone) t += ", region " + std::to_string(p.region);
        if (p.kind == Kind::Bool) t += ", bool";
        if (p.voiceSum) t += ", sum";
        line(t + ")");
      }
    }
    state(Poly::Global, s_.state.globalBytes);
    state(Poly::Voice, s_.state.voiceBytes);
    for (const Diagnostic& w : s_.warnings) line("warning: " + w.message);
    return out_;
  }

private:
  void line(const std::string& t) { out_ += t + "\n"; }

  static std::string hex(uint64_t v) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v, 16);
    return std::string(buf, r.ptr);
  }

  std::string label(NodeIndex n) const {
    if (n == kNone || n >= p_.nodeCount()) return "-";
    const std::string& name = p_.nodeName(n);
    if (!name.empty()) return "'" + name + "'";
    const Node& nd = p_.node(n);
    if (nd.kind == NodeKind::Atom) return std::string(atomSignature(nd.atom).name) + "#" + std::to_string(n);
    return std::string(toText(nd.kind)) + "#" + std::to_string(n);
  }

  std::string operand(const Operand& o) const {
    switch (o.kind) {
      case Operand::Kind::None: return "-";
      case Operand::Kind::Buffer: return "b" + std::to_string(o.index);
      case Operand::Kind::Param: return "p" + std::to_string(o.index);
      case Operand::Kind::External: return "e" + std::to_string(o.index);
      case Operand::Kind::Const: {
        const ConstValue& c = s_.constants[o.index];
        if (c.kind == Kind::Bool) return std::bit_cast<uint32_t>(c.value) != 0u ? "true" : "false";
        return num(c.value);
      }
    }
    return "?";
  }

  void region(uint32_t r) {
    const Region& rg = s_.regions[r];
    std::string t = "  region " + std::to_string(r) + " ";
    t += rg.kind == RegionKind::Control ? "control" : rg.kind == RegionKind::Block ? "block" : "loop";
    if (rg.kind == RegionKind::Loop) t += ", sub-block " + std::to_string(rg.subBlock);
    line(t);
    uint32_t nextSolve = rg.firstSolve;
    for (uint32_t i = rg.firstOp; i < rg.firstOp + rg.numOps; ++i) {
      while (nextSolve < rg.firstSolve + rg.numSolves && s_.solves[nextSolve].firstOp == i) solve(nextSolve++);
      op(i);
    }
    while (nextSolve < rg.firstSolve + rg.numSolves) solve(nextSolve++);
  }

  void op(uint32_t i) {
    const Op& o = s_.ops[i];
    std::string name;
    switch (o.kind) {
      case OpKind::Kernel: name = dsp::kernelInfo(o.kernel).name; break;
      case OpKind::Broadcast: name = "broadcast"; break;
      case OpKind::Reduce:
      case OpKind::Send: name = std::string(atomSignature(o.atom).name); break;
    }
    std::string t = "    #" + std::to_string(i) + " " + name + "/" + std::to_string(o.lanes) + " (";
    const auto ins = s_.inputs(o);
    for (size_t j = 0; j < ins.size(); ++j) {
      if (j) t += ", ";
      if ((o.splMask >> j) & 1u) t += "~";
      t += operand(ins[j]);
    }
    t += ")";
    const auto outs = s_.outputsOf(o);
    if (!outs.empty()) {
      t += " -> ";
      for (size_t j = 0; j < outs.size(); ++j) t += (j ? ", " : "") + operand(outs[j]);
    }
    if (o.state != kNone) t += " [s" + std::to_string(o.state) + "]";
    if (o.resource != kNone) t += " res " + std::to_string(o.resource);
    t += "  " + label(o.node);
    if (o.channel) t += " ch" + std::to_string(o.channel);
    line(t);
  }

  void solve(uint32_t k) {
    const SolveBlock& b = s_.solves[k];
    std::string t = "    solve #" + std::to_string(b.solve) + " (" + std::to_string(b.iterations) + " iterations): ";
    t += b.numOps ? "ops #" + std::to_string(b.firstOp) + "..#" + std::to_string(b.firstOp + b.numOps - 1) : "no ops";
    t += "; unknowns";
    for (uint32_t u = b.firstUnknown; u < b.firstUnknown + b.numUnknowns; ++u) {
      const SolveUnknown& su = s_.unknowns[u];
      t += " " + label(su.node) + (su.channel ? "[" + std::to_string(su.channel) + "]" : "") + "=" + operand(su.value);
      t += su.init.kind != Operand::Kind::None ? " (init " + operand(su.init) + ")" : " (prev s" + std::to_string(su.state) + ")";
    }
    t += "; equations";
    for (uint32_t e = b.firstEquation; e < b.firstEquation + b.numEquations; ++e) {
      const SolveEquation& se = s_.equations[e];
      t += " " + operand(se.lhs) + " == " + operand(se.rhs) + (e + 1 < b.firstEquation + b.numEquations ? "," : "");
    }
    line(t);
  }

  void state(Poly arena, uint32_t bytes) {
    std::string head = std::string("state ") + (arena == Poly::Voice ? "voice" : "global") + " (" +
                       std::to_string(bytes) + " B" + (arena == Poly::Voice ? " per group" : "") + "):";
    bool any = false;
    for (size_t i = 0; i < s_.state.entries.size(); ++i) {
      const StateEntry& e = s_.state.entries[i];
      if (e.arena != arena) continue;
      if (!any) line(head);
      any = true;
      std::string kind = e.tag.kernel == StateTag::kSolveGuess
                             ? std::string("guess")
                             : std::string(dsp::kernelInfo(static_cast<dsp::Atom>(e.tag.kernel)).name);
      std::string t = "  s" + std::to_string(i) + " @" + std::to_string(e.offset) + " +" + std::to_string(e.size) + " " +
                      kind + "/" + std::to_string(e.tag.lanes);
      if (e.tag.extra) t += " frames " + std::to_string(e.tag.extra);
      t += " " + label(e.node) + " ch" + std::to_string(e.channel);
      if (keys_) t += " key " + hex(e.key);
      line(t);
    }
  }

  const Schedule& s_;
  const Patch& p_;
  bool keys_;
  std::string out_;
};

}  // namespace

std::string dump(const Schedule& s, const TypedPatch& patch, bool withKeys) {
  return Dumper(s, s.rewritten ? *s.rewritten : patch, withKeys).run();
}

}  // namespace pq::graph
