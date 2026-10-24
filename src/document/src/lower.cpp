// Project → graph lowering (T3.17): pq/document/lower.h describes the signal flow.
#include "pq/document/lower.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "pq/document/audibility.h"

namespace pq::doc {

namespace {

enum class Kind : uint8_t { Midi, Group, Return, Master };

struct SendInfo {
  TrackId ret;
  bool preFader = false;
};

struct Node {
  const Track* track = nullptr;
  Kind kind = Kind::Midi;
  const Track* parent = nullptr;  // active parent group (main tree)
  std::vector<SendInfo> sends;    // per active return
};

std::string num(double v) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  return std::string(buf, r.ptr);
}

// The gain stage shared by every role: `pl`/`pr` (pan law), `vg` (volume gain), `g` (× gate), the send gains, one
// smoothed multichannel gain `sm` (2 + 2 · sends channels).
void gainStage(std::string& s, Kind kind, const std::vector<SendInfo>& sends, float smoothingMs) {
  s += "  vg = volume <= -70 ? 0 : db_to_gain(volume)\n";
  s += kind == Kind::Master ? "  g = vg\n" : "  g = vg * gate\n";
  if (kind == Kind::Midi) {  // a mono source: constant power, −3 dB per side at the centre
    s += "  th = (pan + 1) * 0.7853981633974483\n";
    s += "  pl = sin(th + 1.5707963267948966)\n";
    s += "  pr = sin(th)\n";
  } else {  // a stereo bus: balance, 0 dB at the centre
    s += "  pl = min(1, 1 - pan)\n";
    s += "  pr = min(1, 1 + pan)\n";
  }
  std::string target = "(pl * g, pr * g";
  for (const SendInfo& si : sends) {
    const std::string r = std::to_string(si.ret.value);
    s += "  sg" + r + " = sendGate * (send" + r + " <= -70 ? 0 : db_to_gain(send" + r + "))" +
         (si.preFader ? "" : " * vg") + "\n";
    target += ", pl * sg" + r + ", pr * sg" + r;
  }
  target += ")";
  s += "  target = " + target + "\n";
  s += "  k = 1 - exp(-1 / (" + num(static_cast<double>(smoothingMs) * 1e-3) + " * sr))\n";
  s += "  sm = z1(sm + (target - sm) * k)\n";
}

std::string patchText(const Node& n, const std::string& instrument, float smoothingMs) {
  const std::string id = std::to_string(n.track->id.value);
  std::string s;
  switch (n.kind) {
    case Kind::Midi: s = "patch Midi" + id + " {\n"; break;
    case Kind::Group: s = "patch Group" + id + " {\n"; break;
    case Kind::Return: s = "patch Return" + id + " {\n"; break;
    case Kind::Master: s = "patch Master" + id + " {\n"; break;
  }
  s += "  param volume = 0 [-70..6]\n  param pan = 0 [-1..1]\n";
  if (n.kind != Kind::Master) s += "  param gate = 1 [0..1]\n";
  if (!n.sends.empty()) s += "  param sendGate = 1 [0..1]\n";
  for (const SendInfo& si : n.sends) s += "  param send" + std::to_string(si.ret.value) + " = -70 [-70..6]\n";
  if (n.kind != Kind::Midi) s += "  in in : audio global stereo = 0\n";
  s += "  out out : audio global stereo\n";
  if (n.kind == Kind::Midi) {
    s += "  out done : bool voice mono\n";
    s += "  syn = " + instrument + "()\n";
    s += "  x = voice_sum(syn)\n";
    s += "  done = syn.done\n";
  } else {
    s += "  x = in\n";
  }
  gainStage(s, n.kind, n.sends, smoothingMs);
  s += "  out = x * (sm[0], sm[1])\n";
  for (size_t i = 0; i < n.sends.size(); ++i)
    s += "  export tap" + std::to_string(n.sends[i].ret.value) + " = x * (sm[" + std::to_string(2 + 2 * i) + "], sm[" +
         std::to_string(3 + 2 * i) + "])\n";
  s += "}\n";
  return s;
}

graph::TrackRole roleOf(Kind k) {
  switch (k) {
    case Kind::Midi: return graph::TrackRole::Instrument;
    case Kind::Group: return graph::TrackRole::Group;
    case Kind::Return: return graph::TrackRole::Return;
    case Kind::Master: return graph::TrackRole::Master;
  }
  return graph::TrackRole::Audio;
}

float sendLevel(const Track& t, TrackId ret, bool* pre) {
  for (const Send& s : t.mixer.sends)
    if (s.target == ret) {
      if (pre) *pre = s.preFader;
      return std::clamp(s.levelDb, kMinVolumeDb, kMaxVolumeDb);
    }
  if (pre) *pre = false;
  return kMinVolumeDb;
}

// The lowered nodes of a project (active tracks), in forEachTrack order: main tree depth-first, returns, master.
std::vector<Node> collect(const Project& p) {
  std::vector<TrackId> returns;
  for (const TrackPtr& r : p.returns)
    if (r->active) returns.push_back(r->id);
  std::vector<Node> nodes;
  struct Walk {
    std::vector<Node>& nodes;
    const std::vector<TrackId>& returns;
    void operator()(const std::vector<TrackPtr>& list, const Track* parent) {
      for (const TrackPtr& t : list) {
        if (!t->active) continue;  // deactivated: left out with its members (Q15)
        if (t->kind != TrackKind::Midi && t->kind != TrackKind::Group) continue;
        Node n;
        n.track = t.get();
        n.kind = t->kind == TrackKind::Midi ? Kind::Midi : Kind::Group;
        n.parent = parent;
        for (TrackId r : returns) {
          SendInfo si{r, false};
          sendLevel(*t, r, &si.preFader);
          n.sends.push_back(si);
        }
        nodes.push_back(std::move(n));
        if (t->kind == TrackKind::Group) (*this)(t->children, t.get());
      }
    }
  } walk{nodes, returns};
  walk(p.tracks, nullptr);
  for (const TrackPtr& r : p.returns)
    if (r->active) nodes.push_back(Node{r.get(), Kind::Return, nullptr, {}});
  if (p.master) nodes.push_back(Node{p.master.get(), Kind::Master, nullptr, {}});
  return nodes;
}

void addParams(const Node& n, uint32_t index, const AudibilityMap& aud, std::vector<LoweredParam>& out) {
  const Track& t = *n.track;
  const std::string id = std::to_string(t.id.value);
  auto add = [&](std::string name, float v, std::optional<ParamRef> ref) {
    out.push_back(LoweredParam{id + ":" + (name.starts_with("send") && name != "sendGate" ? "send:" + name.substr(4) : name),
                               index, std::move(name), v, ref});
  };
  add("volume", std::clamp(t.mixer.volumeDb, kMinVolumeDb, kMaxVolumeDb), ParamRef::track(ParamKind::TrackVolume, t.id));
  add("pan", std::clamp(t.mixer.pan, -1.0f, 1.0f), ParamRef::track(ParamKind::TrackPan, t.id));
  const TrackAudibility* a = aud.find(t.id);
  if (n.kind != Kind::Master) add("gate", a == nullptr || a->output ? 1.0f : 0.0f, std::nullopt);
  if (!n.sends.empty()) add("sendGate", a == nullptr || a->sends ? 1.0f : 0.0f, std::nullopt);
  for (const SendInfo& si : n.sends)
    add("send" + std::to_string(si.ret.value), sendLevel(t, si.ret, nullptr), ParamRef::track(ParamKind::TrackSend, t.id, si.ret));
}

}  // namespace

std::string paramKey(const ParamRef& ref) {
  switch (ref.kind) {
    case ParamKind::TrackVolume: return std::to_string(ref.owner) + ":volume";
    case ParamKind::TrackPan: return std::to_string(ref.owner) + ":pan";
    case ParamKind::TrackSend: return std::to_string(ref.owner) + ":send:" + std::to_string(ref.target);
    default: return {};  // mute/solo are derived into gates; tuning params become signals with T2.6
  }
}

Result<LoweredProject> lower(const Project& p, const LowerOptions& o, const LoweredProject* previous) {
  if (!p.master) return Error{ErrorCode::InvalidArgument, "lower: the project has no master", {}};
  if (o.library == nullptr || o.library->find(o.instrument) == nullptr)
    return Error{ErrorCode::NotFound, "lower: the instrument '" + o.instrument + "' is not in the library", {}};
  const std::vector<Node> nodes = collect(p);
  const AudibilityMap aud = computeAudibility(p);
  LoweredProject out;
  out.graph.library = o.library;
  std::unordered_map<std::string_view, std::shared_ptr<const LoweredPatch>> prev;
  if (previous != nullptr)
    for (const auto& lp : previous->patches) prev.emplace(lp->text, lp);
  std::unordered_map<uint32_t, uint32_t> indexOf;  // track id → graph index
  for (uint32_t i = 0; i < nodes.size(); ++i) {
    const Node& n = nodes[i];
    std::string text = patchText(n, o.instrument, o.smoothingMs);
    std::shared_ptr<const LoweredPatch> lp;
    if (auto it = prev.find(text); it != prev.end()) {
      lp = it->second;
    } else {
      graph::ParseOptions po;
      po.externals = o.externals;
      auto parsed = graph::parse(text, "track" + std::to_string(n.track->id.value) + ".pqp", po);
      if (!parsed.ok()) return Error{parsed.error().code, "lower: " + parsed.error().message, {}};
      auto made = std::make_shared<LoweredPatch>();
      made->text = std::move(text);
      made->patch = std::move(parsed.value().patches.front());
      lp = std::move(made);
    }
    out.patches.push_back(lp);
    out.trackIds.push_back(n.track->id);
    graph::ProjectTrack pt;
    pt.id = n.track->id.value;
    pt.name = n.track->name;
    pt.patch = &out.patches.back()->patch;
    pt.role = roleOf(n.kind);
    pt.eventSlot = n.kind == Kind::Midi && o.slotOf ? o.slotOf(n.track->id) : graph::kNone;
    out.graph.tracks.push_back(std::move(pt));
    indexOf[n.track->id.value] = i;
    addParams(n, i, aud, out.params);
    if (n.kind == Kind::Master) out.master = i;
  }
  // Wires: outputs, then sends (wire order = summing order).
  using W = graph::WireKind;
  auto isActiveGroup = [&](TrackId id) {
    auto it = indexOf.find(id.value);
    return it != indexOf.end() && nodes[it->second].kind == Kind::Group;
  };
  auto inside = [&](const Track& group, TrackId id) {  // id is group itself or one of its members
    if (group.id == id) return true;
    struct Find {
      TrackId id;
      bool operator()(const std::vector<TrackPtr>& list) const {
        for (const TrackPtr& t : list)
          if (t->id == id || (*this)(t->children)) return true;
        return false;
      }
    };
    return Find{id}(group.children);
  };
  for (uint32_t i = 0; i < nodes.size(); ++i) {
    const Node& n = nodes[i];
    if (n.kind == Kind::Master) continue;
    const Output& op = n.track->mixer.output;
    uint32_t dest = out.master;
    switch (op.kind) {
      case OutputKind::Default:
        if (n.parent != nullptr) dest = indexOf.at(n.parent->id.value);
        break;
      case OutputKind::Master: break;
      case OutputKind::Track:
        if (n.kind != Kind::Return && isActiveGroup(op.track) && !inside(*n.track, op.track)) dest = indexOf.at(op.track.value);
        break;
      case OutputKind::None: dest = graph::kNone; break;
    }
    if (dest != graph::kNone) out.graph.wires.push_back({{i, "out"}, {dest, "in"}, W::Audio});
  }
  for (uint32_t i = 0; i < nodes.size(); ++i)
    for (const SendInfo& si : nodes[i].sends)
      out.graph.wires.push_back({{i, "tap" + std::to_string(si.ret.value)}, {indexOf.at(si.ret.value), "in"}, W::Audio});
  out.graph.output = {out.master, "out"};
  return out;
}

std::vector<LoweredParam> lowerParams(const Project& p) {
  const std::vector<Node> nodes = collect(p);
  const AudibilityMap aud = computeAudibility(p);
  std::vector<LoweredParam> out;
  for (uint32_t i = 0; i < nodes.size(); ++i) addParams(nodes[i], i, aud, out);
  return out;
}

}  // namespace pq::doc
