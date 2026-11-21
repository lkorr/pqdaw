// Note devices (AP.2): pq/document/note_devices.h.
#include "pq/document/note_devices.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <optional>

#include "pq/engine/arp.h"

namespace pq::doc {

namespace {

std::vector<std::string> splitChoices(std::string_view s) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i)
    if (i == s.size() || s[i] == '|') {
      out.emplace_back(s.substr(start, i - start));
      start = i + 1;
    }
  return out;
}

std::vector<NoteDeviceType> makeTypes() {
  std::vector<NoteDeviceType> out;
  NoteDeviceType arp;
  arp.id = "arpeggiator";
  arp.name = "Arpeggiator";
  arp.patchName = "Arpeggiator";
  arp.in = arp.out = ChainSignal::Notes;
  for (const engine::ArpParamInfo& r : engine::kArpParamInfo) {
    NoteParamSpec s;
    s.name = std::string(r.name);
    s.label = std::string(r.label);
    s.min = r.min;
    s.max = r.max;
    s.def = r.def;
    s.step = r.step;
    s.isBool = r.isBool;
    s.choices = splitChoices(r.choices);
    s.runtime = r.runtime;
    arp.params.push_back(std::move(s));
  }
  out.push_back(std::move(arp));
  return out;
}

const std::vector<NoteDeviceType>& types() {
  static const std::vector<NoteDeviceType> t = makeTypes();
  return t;
}

std::string num(double v) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  return std::string(buf, r.ptr);
}

}  // namespace

const NoteParamSpec* NoteDeviceType::param(std::string_view n) const noexcept {
  for (const NoteParamSpec& s : params)
    if (s.name == n) return &s;
  return nullptr;
}

int32_t NoteDeviceType::paramIndex(std::string_view n) const noexcept {
  for (size_t i = 0; i < params.size(); ++i)
    if (params[i].name == n) return static_cast<int32_t>(i);
  return -1;
}

std::span<const NoteDeviceType> noteDeviceTypes() { return types(); }

const NoteDeviceType* findNoteDeviceType(std::string_view id) noexcept {
  for (const NoteDeviceType& t : types())
    if (t.id == id) return &t;
  return nullptr;
}

bool isNoteDevicePatch(std::string_view patch) noexcept {
  return patch.starts_with(kNoteDevicePrefix) && findNoteDeviceType(patch.substr(kNoteDevicePrefix.size())) != nullptr;
}

std::string noteDevicePatch(std::string_view type) { return std::string(kNoteDevicePrefix) + std::string(type); }

bool isNoteDevice(const Device& d) noexcept { return isNoteDevicePatch(d.patch); }

const NoteDeviceType* noteDeviceTypeOf(const Device& d) noexcept {
  if (!d.patch.starts_with(kNoteDevicePrefix)) return nullptr;
  return findNoteDeviceType(std::string_view(d.patch).substr(kNoteDevicePrefix.size()));
}

std::string noteDevicePatchText(const NoteDeviceType& t) {
  std::string s = "patch " + t.patchName + " {\n  in    notes : audio global\n";
  for (const NoteParamSpec& p : t.params)
    if (!p.runtime) s += "  in    " + p.name + " : real const = " + num(p.def) + "\n";
  for (const NoteParamSpec& p : t.params) {
    if (!p.runtime) continue;
    s += "  param " + p.name + " = ";
    if (p.isBool) {
      s += p.def > 0.5 ? "true" : "false";
    } else {
      s += num(p.def) + " [" + num(p.min) + ".." + num(p.max) + "]";
      if (p.step > 0.0) s += " @steps(" + num(std::round((p.max - p.min) / p.step) + 1.0) + ")";
    }
    s += " @label(\"" + p.label + "\")";
    if (!p.choices.empty()) {
      std::string items;
      for (const std::string& c : p.choices) items += (items.empty() ? "" : "|") + c;
      s += " @choices(\"" + items + "\")";
    }
    s += "\n";
  }
  s += "  out   out : audio global\n  out = notes\n}\n";
  return s;
}

uint32_t noteRun(const MainPath& path) noexcept {
  uint32_t n = 0;
  while (n < path.blocks.size() && isNoteDevice(*path.blocks[n])) ++n;
  return n;
}

NoteDissolve dissolveNoteDevices(const Project& p) {
  NoteDissolve out;
  std::vector<DeviceId> notes;
  forEachDevice(p, [&](const Device& d, const Track&) {
    if (isNoteDevice(d)) notes.push_back(d.id);
  });
  if (notes.empty()) return out;
  out.any = true;
  WireList w = wiresOf(p);
  std::vector<std::pair<WireId, WireId>> alias;
  for (const DeviceId n : notes) {
    const Wire* in = nullptr;  // the oldest wire into its main input (the list is by id)
    for (const Wire& x : w)
      if (!x.toMixer() && x.to == n && x.param == kMainPort) {
        in = &x;
        break;
      }
    const std::optional<Wire> from = in != nullptr ? std::optional<Wire>(*in) : std::nullopt;
    WireList next;
    next.reserve(w.size());
    WireId joinedFirst;
    for (const Wire& x : w) {
      const bool into = !x.toMixer() && x.to == n;
      const bool outOf = !x.fromNoteSource() && x.from == n;
      if (!into && !outOf) {
        next.push_back(x);
        continue;
      }
      if (outOf && !into && x.port == kMainPort && from) {  // the joined wire: from where its input's wire starts
        Wire j = x;
        j.from = from->from;
        j.noteSource = from->noteSource;
        j.port = from->port;
        j.tap = from->tap;
        if (!joinedFirst.valid()) joinedFirst = j.id;
        next.push_back(std::move(j));
        continue;
      }
      if (from && x.id == from->id) continue;  // (aliased below)
      out.dropped.push_back(x.id);
    }
    if (from) {
      if (joinedFirst.valid()) alias.emplace_back(from->id, joinedFirst);
      else out.dropped.push_back(from->id);  // nothing goes on from it
    }
    w = std::move(next);
  }
  // A chain of note devices: an alias of an alias plays as the last one.
  for (auto& [a, b] : alias)
    for (size_t guard = 0; guard <= alias.size(); ++guard) {
      const auto it = std::find_if(alias.begin(), alias.end(), [&](const auto& x) { return x.first == b; });
      if (it == alias.end()) break;
      b = it->second;
    }
  // A dropped wire stays dropped even when it was aliased first (then dissolved away by a later device).
  std::sort(out.dropped.begin(), out.dropped.end());
  out.dropped.erase(std::unique(out.dropped.begin(), out.dropped.end()), out.dropped.end());
  for (const auto& [a, b] : alias)
    if (std::binary_search(out.dropped.begin(), out.dropped.end(), b)) out.dropped.push_back(a);
    else out.aliases.emplace_back(a, b);
  std::sort(out.dropped.begin(), out.dropped.end());
  out.dropped.erase(std::unique(out.dropped.begin(), out.dropped.end()), out.dropped.end());
  std::sort(out.aliases.begin(), out.aliases.end());
  if (!w.empty()) out.wires = std::make_shared<const WireList>(std::move(w));
  return out;
}

Project withoutNoteDevices(const Project& p, const NoteDissolve& how) {
  if (!how.any) return p;
  Project q = p;
  std::function<TrackPtr(const TrackPtr&)> walk = [&](const TrackPtr& tp) -> TrackPtr {
    bool has = false;
    for (const DevicePtr& d : tp->devices) has = has || isNoteDevice(*d);
    bool kids = false;
    std::vector<TrackPtr> children = tp->children;
    for (TrackPtr& ch : children) {
      TrackPtr n = walk(ch);
      kids = kids || n != ch;
      ch = std::move(n);
    }
    if (!has && !kids) return tp;
    auto nt = std::make_shared<Track>(*tp);
    if (has) std::erase_if(nt->devices, [](const DevicePtr& d) { return isNoteDevice(*d); });
    nt->children = std::move(children);
    return nt;
  };
  for (TrackPtr& t : q.tracks) t = walk(t);
  for (TrackPtr& t : q.returns) t = walk(t);
  if (q.master) q.master = walk(q.master);
  q.wires = how.wires;
  return q;
}

double noteParamValue(const NoteParamSpec& s, double position) noexcept {
  const double p = std::isfinite(position) ? std::clamp(position, 0.0, 1.0) : 0.0;
  double v = s.min + p * (s.max - s.min);
  if (s.isBool) return p > 0.5 ? 1.0 : 0.0;
  if (s.step > 0.0) v = s.min + std::round((v - s.min) / s.step) * s.step;
  return std::clamp(v, std::min(s.min, s.max), std::max(s.min, s.max));
}

double noteParamPosition(const NoteParamSpec& s, double value) noexcept {
  if (s.isBool) return value > 0.5 ? 1.0 : 0.0;
  const double span = s.max - s.min;
  if (!(std::abs(span) > 0.0) || !std::isfinite(value)) return 0.0;
  return std::clamp((value - s.min) / span, 0.0, 1.0);
}

double noteDeviceValue(const Device& d, const NoteParamSpec& s) noexcept {
  if (!s.runtime) {
    const DeviceValue* v = d.setting(s.name);
    return v != nullptr ? v->value : s.def;
  }
  const DeviceValue* v = d.param(s.name);
  return v != nullptr ? noteParamValue(s, v->value) : s.def;
}

}  // namespace pq::doc
