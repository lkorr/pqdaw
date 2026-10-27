// Node tooltips and recognised constant forms of the circuit editor (T4.15): pq/views/circuit/node_info.h.

#include "pq/views/circuit/node_info.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <numeric>
#include <utility>

#include "pq/graph/atoms.h"
#include "pq/graph/edit.h"
#include "pq/views/devices/device_model.h"

namespace pq::views::circuit {

namespace {

constexpr const char* kPi = "\xCF\x80";       // π
constexpr const char* kRoot = "\xE2\x88\x9A";  // √
constexpr const char* kMinus = "\xE2\x88\x92"; // −

// The shortest decimal that reads back as v, and how many significant digits it needs.
std::string shortest(double v, int* digits = nullptr) {
  char buf[40];
  for (int p = 1; p <= 17; ++p) {
    std::snprintf(buf, sizeof buf, "%.*g", p, v);
    if (std::strtod(buf, nullptr) == v) {
      if (digits) *digits = p;
      return buf;
    }
  }
  if (digits) *digits = 17;
  return buf;
}

struct Form {
  double value;
  std::string text;
};

std::vector<Form> buildForms() {
  std::vector<Form> f;
  const double pi = std::numbers::pi;
  for (int q = 2; q <= 32; ++q)
    for (int p = 1; p <= 12; ++p)
      if (std::gcd(p, q) == 1) f.push_back({static_cast<double>(p) / q, std::to_string(p) + "/" + std::to_string(q)});
  for (const int q : {1, 2, 3, 4, 6, 8, 12})
    for (int p = 1; p <= 24; ++p) {
      if (std::gcd(p, q) != 1) continue;
      std::string t = (p == 1 ? std::string() : std::to_string(p)) + kPi;
      if (q != 1) t += "/" + std::to_string(q);
      f.push_back({p * pi / q, t});
    }
  for (int q = 1; q <= 8; ++q)
    for (int p = 1; p <= 4; ++p) {
      if (std::gcd(p, q) != 1) continue;
      std::string t = std::to_string(p) + "/" + (q == 1 ? std::string(kPi) : "(" + std::to_string(q) + kPi + ")");
      f.push_back({p / (q * pi), t});
    }
  const std::string r = kRoot;
  f.push_back({std::numbers::sqrt2, r + "2"});
  f.push_back({1.0 / std::numbers::sqrt2, "1/" + r + "2"});
  f.push_back({std::numbers::sqrt3, r + "3"});
  f.push_back({1.0 / std::numbers::sqrt3, "1/" + r + "3"});
  f.push_back({std::numbers::sqrt3 / 2.0, r + "3/2"});
  f.push_back({std::exp2(1.0 / 12.0), "2^(1/12) (a 12-edo semitone)"});
  f.push_back({std::exp2(-1.0 / 12.0), "2^(" + std::string(kMinus) + "1/12)"});
  f.push_back({std::numbers::e, "e"});
  f.push_back({1.0 / std::numbers::e, "1/e"});
  f.push_back({std::numbers::ln2, "ln 2"});
  f.push_back({1.0 / std::numbers::ln2, "1/ln 2"});
  f.push_back({std::numbers::ln10, "ln 10"});
  f.push_back({1.0 / std::numbers::ln10, "1/ln 10"});
  f.push_back({std::log10(2.0), "log\xE2\x82\x81\xE2\x82\x80 2"});
  f.push_back({20.0 / std::numbers::ln10, "20/ln 10 (dB per neper)"});
  f.push_back({std::numbers::ln10 / 20.0, "ln 10/20 (nepers per dB)"});
  for (int d = -120; d <= 48; ++d) {
    if (d == 0) continue;
    f.push_back({std::pow(10.0, d / 20.0), (d < 0 ? std::string(kMinus) + std::to_string(-d) : "+" + std::to_string(d)) + " dB"});
  }
  return f;
}

const std::vector<Form>& forms() {
  static const std::vector<Form> f = buildForms();
  return f;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// Word-wraps `s` at about `width` bytes (UTF-8 is counted in bytes: lines stay a little shorter with symbols).
std::string wrap(std::string_view s, size_t width) {
  std::string out, line;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(' ', i);
    if (j == std::string_view::npos) j = s.size();
    const std::string_view word = s.substr(i, j - i);
    if (!line.empty() && line.size() + 1 + word.size() > width) {
      out += line + "\n";
      line.clear();
    }
    if (!line.empty()) line += ' ';
    line += word;
    i = j + 1;
  }
  out += line;
  return out;
}

std::string valueText(double v, bool isBool) {
  if (isBool) return v != 0.0 ? "true" : "false";
  std::string s = shortest(v);
  if (const std::string f = recognisedForm(v); !f.empty()) s += " (" + f + ")";
  return s;
}

std::string styleText(const WireStyle& s) {
  std::string t = s.boolean ? "bool" : "real";
  t += s.rate == WireRate::Const ? " \xC2\xB7 const" : s.rate == WireRate::Param ? " \xC2\xB7 param rate" : " \xC2\xB7 audio rate";
  t += s.poly ? " \xC2\xB7 per voice" : " \xC2\xB7 global";
  return t;
}

// The `// …` comment after the declaration of node n in the text (named nodes only).
std::string trailingComment(const TooltipContext& c, graph::NodeIndex n) {
  if (c.source.empty() || n >= c.spans.size() || c.spans[n].offset == graph::kNone) return {};
  const std::string& name = c.patch->nodeName(n);
  if (name.empty() || graph::edit::isGeneratedName(name)) return {};
  const size_t at = c.spans[n].offset;
  if (at >= c.source.size()) return {};
  const size_t eol = std::min(c.source.find('\n', at), c.source.size());
  const size_t bol = c.source.rfind('\n', at) == std::string_view::npos ? 0 : c.source.rfind('\n', at) + 1;
  const std::string_view line = c.source.substr(bol, eol - bol);
  // Only the line that declares or binds the name (an expression spanning lines keeps its comment for its own line).
  const std::string_view head = trim(line);
  if (head.starts_with("//")) return {};
  const size_t k = line.find("//");
  if (k == std::string_view::npos) return {};
  return std::string(trim(line.substr(k + 2)));
}

}  // namespace

std::string recognisedForm(double v) {
  if (!std::isfinite(v) || v == 0.0) return {};
  const double a = std::fabs(v);
  int digits = 0;
  (void)shortest(a, &digits);
  if (digits <= 6) return {};  // reads clearly as written
  const double tol = a * 0x1p-23;
  for (const Form& f : forms())
    if (std::fabs(a - f.value) <= tol) return (v < 0.0 ? std::string(kMinus) : std::string()) + f.text;
  return {};
}

std::string patchDescription(std::string_view source, uint32_t patchBegin) {
  if (patchBegin == graph::kNone || patchBegin > source.size()) return {};
  // The comment lines directly above `patch`, nearest last.
  auto block = [&](size_t end) {  // lines ending before `end` (the start of a line)
    std::vector<std::string_view> lines;
    size_t e = end;
    while (e > 0) {
      const size_t lineEnd = e - 1;  // the '\n' before `e`
      const size_t lb = source.rfind('\n', lineEnd == 0 ? 0 : lineEnd - 1);
      const size_t b = (lb == std::string_view::npos || lineEnd == 0) ? 0 : lb + 1;
      const std::string_view line = trim(source.substr(b, lineEnd - b));
      if (!line.starts_with("//")) break;
      lines.push_back(trim(line.substr(2)));
      if (b == 0) break;
      e = b;
    }
    std::reverse(lines.begin(), lines.end());
    return lines;
  };
  size_t lineStart = source.rfind('\n', patchBegin == 0 ? 0 : patchBegin - 1);
  lineStart = (lineStart == std::string_view::npos || patchBegin == 0) ? 0 : lineStart + 1;
  std::vector<std::string_view> lines = block(lineStart);
  if (lines.empty()) {
    // The first patch of a file (imports between it and the header): the file's leading comment block.
    const std::string_view before = source.substr(0, lineStart);
    if (before.find("patch ") == std::string_view::npos) {
      size_t e = 0;
      while (e < source.size()) {
        const size_t le = std::min(source.find('\n', e), source.size());
        if (!trim(source.substr(e, le - e)).starts_with("//")) break;
        e = le + 1;
      }
      if (e > 0) lines = block(std::min(e, source.size()));
    }
  }
  // The first paragraph, joined; its first sentence.
  std::string para;
  for (const std::string_view l : lines) {
    if (l.empty()) {
      if (!para.empty()) break;
      continue;
    }
    if (!para.empty()) para += ' ';
    para += l;
  }
  if (para.empty()) return {};
  size_t cut = std::string::npos;
  int depth = 0;
  for (size_t i = 0; i + 1 < para.size(); ++i) {
    if (para[i] == '(') ++depth;
    else if (para[i] == ')') depth = std::max(0, depth - 1);
    else if (para[i] == '.' && depth == 0 && para[i + 1] == ' ' && i > 0 && !(para[i - 1] >= '0' && para[i - 1] <= '9')) {
      cut = i + 1;
      break;
    }
  }
  if (cut != std::string::npos) para.resize(cut);
  if (para.size() > 300) para = para.substr(0, 297) + "\xE2\x80\xA6";
  return wrap(para, 96);
}

std::string nodeTooltip(const TooltipContext& c, graph::NodeIndex n) {
  if (!c.patch || n >= c.patch->nodeCount()) return {};
  const graph::Patch& p = *c.patch;
  const graph::Node& nd = p.node(n);
  std::string name = p.nodeName(n);
  if (graph::edit::isGeneratedName(name)) name.clear();
  const std::string bound = name.empty() ? std::string() : name + " = ";
  std::vector<std::string> lines;
  std::string desc;
  switch (nd.kind) {
    case graph::NodeKind::Atom: {
      const graph::AtomSignature& sig = graph::atomSignature(nd.atom);
      lines.push_back(bound + std::string(sig.name) + " \xC2\xB7 atom (" + std::string(graph::toText(sig.category)) + ")");
      desc = std::string(sig.summary);
      break;
    }
    case graph::NodeKind::Subcircuit: {
      const graph::SubcircuitRef& ref = p.subcircuit(n);
      lines.push_back(bound + ref.patch + " \xC2\xB7 subcircuit");
      if (c.describeCallee) desc = c.describeCallee(ref.patch);
      if (desc.empty()) desc = "an instance of patch " + ref.patch + " (double-click to open it)";
      break;
    }
    case graph::NodeKind::Param: {
      const devices::DeviceSignature sig = devices::signatureOf(p);
      const devices::ParamInfo* pi = sig.findParam(name.empty() ? p.nodeName(n) : name);
      if (!pi) {
        lines.push_back(p.nodeName(n) + " \xC2\xB7 param");
        break;
      }
      lines.push_back((pi->label != pi->name ? pi->label + " (" + pi->name + ")" : pi->name) + " \xC2\xB7 param");
      double now = pi->def;
      bool set = false;
      for (const graph::ConstArg& v : c.paramValues)
        if (v.name == pi->name) {
          now = v.value;
          set = true;
        }
      if (pi->isBool) {
        desc = std::string("on / off \xC2\xB7 now ") + (now != 0.0 ? "on" : "off");
      } else {
        desc = devices::formatValue(*pi, pi->min) + " \xE2\x80\xA6 " + devices::formatValue(*pi, pi->max) + " \xC2\xB7 now " +
               devices::formatValue(*pi, now);
      }
      if (set && now != pi->def) desc += " (default " + devices::formatValue(*pi, pi->def) + ")";
      else desc += " (the default)";
      break;
    }
    case graph::NodeKind::Input: {
      const graph::PortType t = p.outputType(n, 0);
      lines.push_back(p.nodeName(n) + " \xC2\xB7 in (" + graph::toText(t) + ")");
      double v = 0.0;
      if (c.structure && c.structure->constantAt(p, n, 0, v)) desc = "= " + valueText(v, t.kind == graph::Kind::Bool) + " (a structural constant)";
      else if (t.rate == graph::Rate::Const) desc = "a structural constant (a change recompiles)";
      else desc = "an input of the patch";
      break;
    }
    case graph::NodeKind::Output:
      lines.push_back(p.nodeName(n) + " \xC2\xB7 out (" + graph::toText(p.inputType(n, 0)) + ")");
      desc = "an output of the patch";
      break;
    case graph::NodeKind::Export:
      lines.push_back(p.nodeName(n) + " \xC2\xB7 export");
      desc = "a jack on top of the device: wire it to any param in the project";
      break;
    case graph::NodeKind::Const: {
      const graph::Constant& k = p.constant(n);
      if (k.kind == graph::Kind::Bool) {
        lines.push_back(std::string(k.literal != 0.0 ? "true" : "false") + " \xC2\xB7 constant");
      } else {
        lines.push_back(shortest(k.literal) + std::string(graph::unitSuffix(k.unit)) + " \xC2\xB7 constant");
        const std::string f = recognisedForm(k.value());
        if (!f.empty()) desc = "= " + f;
        else if (k.unit != graph::Unit::None) desc = "= " + shortest(k.value()) + " (normalised)";
      }
      if (desc.empty()) desc = "a literal (folded at compile time)";
      break;
    }
    case graph::NodeKind::Unknown:
      lines.push_back(p.nodeName(n) + " \xC2\xB7 unknown (solve)");
      desc = "solved every sample from the block's equations";
      break;
    case graph::NodeKind::Equation:
      lines.push_back("equation (solve)");
      desc = "lhs == rhs, solved with the block's unknowns";
      break;
  }
  if (!desc.empty()) lines.push_back(std::move(desc));
  if (std::string tc = trailingComment(c, n); !tc.empty()) lines.push_back(wrap(tc, 96));
  // Inferred kind / rate / poly of the value.
  uint32_t port = graph::kNone;
  if (nd.numOutputs > 0) {
    port = p.flatOutput(n, 0);
  } else if (nd.numInputs > 0) {
    const graph::Adjacency adj = p.adjacency();
    const graph::WireIndex w = adj.driver[p.flatInput(n, 0)];
    if (w != graph::kNone) port = p.flatOutput(p.wire(w).from.node, p.wire(w).from.port);
  }
  if (port != graph::kNone && port < c.outputStyles.size()) lines.push_back(styleText(c.outputStyles[port]));
  if (c.structure && n < c.structure->nodeInactive.size() && c.structure->nodeInactive[n])
    lines.push_back("Inactive: a structural constant switches this branch off (not compiled)");
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) out += '\n';
    out += lines[i];
  }
  return out;
}

}  // namespace pq::views::circuit
