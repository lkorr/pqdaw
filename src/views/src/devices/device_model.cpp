// Device signatures and value mapping (pq/views/devices/device_model.h, T4.6).

#include "pq/views/devices/device_model.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace pq::views::devices {

namespace {

const graph::Attr* findAttr(const graph::ParamDecl& d, std::string_view name) {
  for (const graph::Attr& a : d.attrs)
    if (a.name == name) return &a;
  return nullptr;
}

DisplayUnit unitOf(graph::Unit u) {
  switch (u) {
    case graph::Unit::Hz:
    case graph::Unit::kHz: return DisplayUnit::Hz;
    case graph::Unit::dB: return DisplayUnit::Db;
    case graph::Unit::percent: return DisplayUnit::Percent;
    case graph::Unit::ms:
    case graph::Unit::s: return DisplayUnit::Seconds;
    default: return DisplayUnit::None;
  }
}

bool gainFloorIsSilence(const ParamInfo& p) { return p.unit == DisplayUnit::Db && p.min <= 0.00031623; }  // ≤ −70 dB

double clampV(const ParamInfo& p, double v) { return std::clamp(v, std::min(p.min, p.max), std::max(p.min, p.max)); }

std::string fmt(const char* f, double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, f, v);
  return buf;
}

}  // namespace

const ParamInfo* DeviceSignature::findParam(std::string_view name) const noexcept {
  for (const ParamInfo& p : params)
    if (p.name == name) return &p;
  return nullptr;
}

const ConstInfo* DeviceSignature::findConst(std::string_view name) const noexcept {
  for (const ConstInfo& c : consts)
    if (c.name == name) return &c;
  return nullptr;
}

DeviceSignature signatureOf(const graph::Patch& patch) {
  DeviceSignature sig;
  sig.patch = patch.name();
  const graph::Adjacency adj = patch.adjacency();
  for (graph::NodeIndex n = 0; n < patch.nodeCount(); ++n) {
    const graph::Node& node = patch.node(n);
    switch (node.kind) {
      case graph::NodeKind::Param: {
        const graph::ParamDecl& d = patch.paramDecl(n);
        ParamInfo p;
        p.name = patch.nodeName(n);
        p.label = p.name;
        p.isBool = d.kind == graph::Kind::Bool;
        p.def = d.defaultValue.value();
        if (d.hasRange) {
          p.min = d.rangeMin.value();
          p.max = d.rangeMax.value();
        } else if (p.isBool) {
          p.min = 0.0;
          p.max = 1.0;
        } else {
          p.min = std::min(0.0, p.def);
          p.max = std::max(1.0, p.def);
        }
        p.unit = unitOf(d.defaultValue.unit);
        if (p.unit == DisplayUnit::None && d.hasRange) p.unit = unitOf(d.rangeMax.unit);
        if (const graph::Attr* a = findAttr(d, "unit"); a && !a->args.empty()) {
          graph::Unit u;
          const std::string& t = a->args[0].value.text;
          if (graph::unitFromSuffix(t, u)) p.unit = unitOf(u);
        }
        if (const graph::Attr* a = findAttr(d, "label"); a && !a->args.empty() && !a->args[0].value.text.empty())
          p.label = a->args[0].value.text;
        if (const graph::Attr* a = findAttr(d, "curve"); a && !a->args.empty()) p.exp = a->args[0].value.text == "exp";
        if (const graph::Attr* a = findAttr(d, "steps"); a && !a->args.empty())
          p.steps = static_cast<int32_t>(std::lround(a->args[0].value.number.value()));
        p.hidden = findAttr(d, "hidden") != nullptr;
        if (p.exp && (p.min <= 0.0 || p.max <= 0.0)) p.exp = false;  // a log scale needs positive ends
        sig.params.push_back(std::move(p));
        break;
      }
      case graph::NodeKind::Input: {
        const graph::PortType& t = patch.outputType(n, 0);
        if (t.rate == graph::Rate::Const) {
          ConstInfo c;
          c.name = patch.nodeName(n);
          const graph::WireIndex w = adj.driver[patch.flatInput(n, 0)];
          if (w != graph::kNone) {
            const graph::NodeIndex src = patch.wire(w).from.node;
            if (patch.node(src).kind == graph::NodeKind::Const) c.def = patch.constant(src).value();
          }
          sig.consts.push_back(std::move(c));
        } else if (sig.audioIn.empty()) {
          sig.audioIn = patch.nodeName(n);
        }
        break;
      }
      case graph::NodeKind::Export: sig.exports.push_back(patch.nodeName(n)); break;
      default: break;
    }
  }
  return sig;
}

double toDisplay(const ParamInfo& p, double v) noexcept {
  switch (p.unit) {
    case DisplayUnit::Db: return v > 0.0 ? 20.0 * std::log10(v) : -1e9;
    case DisplayUnit::Percent: return 100.0 * v;
    case DisplayUnit::Seconds: return 1000.0 * v;  // shown in ms
    default: return v;
  }
}

double fromDisplay(const ParamInfo& p, double d) noexcept {
  switch (p.unit) {
    case DisplayUnit::Db: return std::pow(10.0, d / 20.0);
    case DisplayUnit::Percent: return 0.01 * d;
    case DisplayUnit::Seconds: return 0.001 * d;
    default: return d;
  }
}

double toPosition(const ParamInfo& p, double v) noexcept {
  v = clampV(p, v);
  if (p.isBool) return v >= 0.5 ? 1.0 : 0.0;
  if (p.exp) return std::log(v / p.min) / std::log(p.max / p.min);
  if (p.unit == DisplayUnit::Db && p.min > 0.0) {
    const double a = toDisplay(p, p.min), b = toDisplay(p, p.max);
    return b > a ? (toDisplay(p, v) - a) / (b - a) : 0.0;
  }
  return p.max > p.min ? (v - p.min) / (p.max - p.min) : 0.0;
}

double fromPosition(const ParamInfo& p, double t) noexcept {
  t = std::clamp(t, 0.0, 1.0);
  double v;
  if (p.isBool) return t >= 0.5 ? 1.0 : 0.0;
  if (p.exp) v = p.min * std::pow(p.max / p.min, t);
  else if (p.unit == DisplayUnit::Db && p.min > 0.0) {
    const double a = toDisplay(p, p.min), b = toDisplay(p, p.max);
    v = fromDisplay(p, a + t * (b - a));
  } else v = p.min + t * (p.max - p.min);
  if (p.steps > 1) v = std::round(v);
  return clampV(p, v);
}

std::string formatValue(const ParamInfo& p, double v) {
  if (p.isBool) return v >= 0.5 ? "On" : "Off";
  const double d = toDisplay(p, v);
  switch (p.unit) {
    case DisplayUnit::Hz:
      if (d >= 10000.0) return fmt("%.1f kHz", d / 1000.0);
      if (d >= 1000.0) return fmt("%.2f kHz", d / 1000.0);
      if (d >= 100.0) return fmt("%.0f Hz", d);
      return fmt("%.1f Hz", d);
    case DisplayUnit::Db:
      if (gainFloorIsSilence(p) && v <= p.min * 1.0000001) return "-inf dB";
      return fmt(std::fabs(d) < 0.05 ? "0.0 dB" : "%.1f dB", d);
    case DisplayUnit::Percent: return fmt(std::fabs(d) >= 10.0 ? "%.0f %%" : "%.1f %%", d);
    case DisplayUnit::Seconds: return d >= 1000.0 ? fmt("%.2f s", d / 1000.0) : fmt("%.1f ms", d);
    default:
      if (p.steps > 1) return fmt("%.0f", v);
      return fmt(std::fabs(v) >= 10.0 ? "%.1f" : "%.2f", v);
  }
}

std::optional<double> parseValue(const ParamInfo& p, std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  if (s.empty()) return std::nullopt;
  if (p.isBool) {
    if (s == "On" || s == "on" || s == "1" || s == "true") return 1.0;
    if (s == "Off" || s == "off" || s == "0" || s == "false") return 0.0;
    return std::nullopt;
  }
  if (s == "-inf" || s == "-inf dB" || s == "-infdB") {
    if (p.unit == DisplayUnit::Db) return p.min;
    return std::nullopt;
  }
  double num = 0.0;
  const char* b = s.data();
  const char* e = s.data() + s.size();
  const auto r = std::from_chars(b, e, num);
  if (r.ec != std::errc{}) return std::nullopt;
  std::string_view rest(r.ptr, static_cast<size_t>(e - r.ptr));
  while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
  double d = num;
  if (rest.empty()) {
  } else if (rest == "k" || rest == "kHz" || rest == "khz" || rest == "K") {
    if (p.unit != DisplayUnit::Hz) return std::nullopt;
    d = num * 1000.0;
  } else if (rest == "Hz" || rest == "hz") {
    if (p.unit != DisplayUnit::Hz) return std::nullopt;
  } else if (rest == "dB" || rest == "db") {
    if (p.unit != DisplayUnit::Db) return std::nullopt;
  } else if (rest == "%") {
    if (p.unit != DisplayUnit::Percent) return std::nullopt;
  } else if (rest == "ms") {
    if (p.unit != DisplayUnit::Seconds) return std::nullopt;
  } else if (rest == "s") {
    if (p.unit != DisplayUnit::Seconds) return std::nullopt;
    d = num * 1000.0;
  } else return std::nullopt;
  return clampV(p, fromDisplay(p, d));
}

}  // namespace pq::views::devices
