// Meta modules in the document (TMM.3): pq/document/meta.h.
#include "pq/document/meta.h"

#include <algorithm>

#include "pq/graph/infer.h"
#include "pq/meta/registry.h"

namespace pq::doc {

namespace {

using graph::NodeIndex;

void collect(const graph::Patch& p, const std::string& prefix, const PatchLookup& lookup,
             std::vector<const graph::Patch*>& active, std::vector<MetaSite>& out) {
  if (active.size() >= graph::kMaxSubcircuitDepth || std::find(active.begin(), active.end(), &p) != active.end()) return;
  active.push_back(&p);
  std::optional<graph::Adjacency> adj;
  for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
    if (p.node(n).kind != graph::NodeKind::Subcircuit) continue;
    const std::string& name = p.nodeName(n);
    if (name.empty()) continue;
    const std::string& callee = p.subcircuit(n).patch;
    if (const meta::Type* t = meta::typeOfCall(callee)) {
      if (!adj) adj = p.adjacency();
      MetaSite s;
      s.path = prefix + name;
      s.type = t;
      s.wired.assign(t->params.size(), 0);
      for (size_t i = 0; i < t->params.size(); ++i)
        if (const auto pin = p.findInput(n, t->params[i].name))
          s.wired[i] = adj->driver[p.flatInput(n, *pin)] != graph::kNone ? 1 : 0;
      out.push_back(std::move(s));
    } else if (lookup) {
      if (const graph::Patch* c = lookup(callee)) collect(*c, prefix + name + ".", lookup, active, out);
    }
  }
  active.pop_back();
}

bool anyMeta(const graph::Patch& p, const PatchLookup& lookup, std::vector<const graph::Patch*>& active) {
  if (active.size() >= graph::kMaxSubcircuitDepth || std::find(active.begin(), active.end(), &p) != active.end())
    return false;
  active.push_back(&p);
  bool found = false;
  for (NodeIndex n = 0; n < p.nodeCount() && !found; ++n) {
    if (p.node(n).kind != graph::NodeKind::Subcircuit || p.nodeName(n).empty()) continue;
    const std::string& callee = p.subcircuit(n).patch;
    if (meta::typeOfCall(callee) != nullptr) {
      found = true;
    } else if (lookup) {
      if (const graph::Patch* c = lookup(callee)) found = anyMeta(*c, lookup, active);
    }
  }
  active.pop_back();
  return found;
}

std::string flatPath(std::string_view path) {
  std::string s;
  for (const char c : path) {
    if (c == '.') s += "__";
    else s += c;
  }
  return s;
}

// Every named call reachable from `p` within `file`: (NodeId path, name path).
struct CallPath {
  std::vector<uint32_t> ids;
  std::string names;
};
void callPaths(const graph::PatchFile& file, const graph::Patch& p, const CallPath& prefix,
               std::vector<const graph::Patch*>& active, std::vector<CallPath>& out) {
  if (active.size() >= graph::kMaxSubcircuitDepth || std::find(active.begin(), active.end(), &p) != active.end()) return;
  active.push_back(&p);
  for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
    if (p.node(n).kind != graph::NodeKind::Subcircuit || p.nodeName(n).empty()) continue;
    CallPath c = prefix;
    c.ids.push_back(p.node(n).id.value);
    c.names += (c.names.empty() ? "" : ".") + p.nodeName(n);
    out.push_back(c);
    if (const graph::Patch* callee = file.find(p.subcircuit(n).patch)) callPaths(file, *callee, c, active, out);
  }
  active.pop_back();
}

bool under(std::string_view path, std::string_view root) {
  return path == root || (path.size() > root.size() && path.starts_with(root) && path[root.size()] == '.');
}

}  // namespace

std::vector<MetaSite> metaSites(const graph::Patch& root, const PatchLookup& lookup) {
  std::vector<MetaSite> out;
  std::vector<const graph::Patch*> active;
  collect(root, std::string(), lookup, active, out);
  return out;
}

bool callsMeta(const graph::Patch& p, const PatchLookup& lookup) {
  std::vector<const graph::Patch*> active;
  return anyMeta(p, lookup, active);
}

std::string metaParamKey(DeviceId device, std::string_view path, std::string_view param) {
  return "dev" + std::to_string(device.value) + ":" + std::string(path) + "." + std::string(param);
}

std::string metaParamFlatName(DeviceId device, std::string_view path, std::string_view param) {
  return "d" + std::to_string(device.value) + "__" + flatPath(path) + "__" + std::string(param);
}

double metaParamValue(const Device& d, std::string_view path, const meta::Type& t, size_t i) {
  const meta::ParamSpec& s = t.params[i];
  const DeviceValue* v = nullptr;
  if (path.empty()) {
    v = d.param(s.name);
  } else if (const MetaInstance* inst = d.metaAt(path)) {
    v = inst->param(s.name);
  }
  return v != nullptr ? v->value : s.def;
}

std::string metaParamBakedOnly(const Device& d, std::string_view param) {
  if (!isMetaDevice(d)) return {};
  const meta::Type* t = meta::findType(metaDeviceType(d));
  if (t == nullptr) return {};
  const int32_t i = t->paramIndex(param);
  if (i < 0 || !t->params[static_cast<size_t>(i)].bakeable) return {};
  if (t->liveBakeable) {  // TMM.8: live, unless the type's settings say otherwise (the Wavetable's Shape mode)
    if (t->bakedOnly == nullptr) return {};
    return t->bakedOnly(metaSettings(d, "", *t), static_cast<uint32_t>(i));
  }
  return "the " + std::string(t->name) + "'s " + std::string(t->params[static_cast<size_t>(i)].label) +
         " is baked into its table: it cannot be automated, wired or mapped to a macro yet";
}

std::vector<double> metaSettings(const Device& d, std::string_view path, const meta::Type& t) {
  std::vector<double> out(t.settings.size());
  const MetaInstance* inst = path.empty() ? nullptr : d.metaAt(path);
  for (size_t i = 0; i < t.settings.size(); ++i) {
    const DeviceValue* v = path.empty() ? d.setting(t.settings[i].name)
                           : inst != nullptr ? inst->setting(t.settings[i].name)
                                             : nullptr;
    out[i] = v != nullptr ? v->value : t.settings[i].def;
  }
  return out;
}

MetaInstance defaultMetaInstance(const meta::Type& t) {
  MetaInstance m;
  m.type = std::string(t.id);
  m.version = t.version;
  if (t.defaultData != nullptr) m.data = t.defaultData();
  return m;
}

int32_t metaChainInput(const Device& d, const meta::Type& t) {
  if (d.chainInput.empty())
    return t.chainInput >= 0 && static_cast<size_t>(t.chainInput) < t.inputs.size() ? t.chainInput : -1;
  if (d.chainInput == kChainUnplugged) return -1;
  return t.inputIndex(d.chainInput);
}

std::vector<ChainStage> chainStages(const std::vector<DevicePtr>& list, bool notes, bool voices) {
  std::vector<ChainStage> out;
  out.reserve(list.size());
  ChainSignal s = notes ? ChainSignal::Notes : ChainSignal::Audio;
  for (const DevicePtr& dp : list) {
    const Device& d = *dp;
    if (!d.active) continue;
    ChainStage st;
    st.device = &d;
    const meta::Type* t = isMetaDevice(d) ? meta::findType(metaDeviceType(d)) : nullptr;
    if (t != nullptr) {
      st.input = metaChainInput(d, *t);
      st.in = st.input < 0 ? ChainSignal::None : s;
      st.out = st.in == ChainSignal::Notes || st.in == ChainSignal::Voices ? ChainSignal::Voices : ChainSignal::Audio;
      if (!voices && st.out == ChainSignal::Voices && st.in == ChainSignal::Voices) st.in = st.out = ChainSignal::Audio;
    } else {
      st.in = s;
      st.out = ChainSignal::Audio;  // a stock instrument's voices are summed at its output; an effect's input is global
    }
    s = st.out;
    out.push_back(st);
  }
  return out;
}

meta::Form chainForm(const ChainStage& s) {
  if (s.device == nullptr || !isMetaDevice(*s.device)) return meta::Form::Nested;
  switch (s.in) {
    case ChainSignal::Notes: return meta::Form::ChainNotes;
    case ChainSignal::Voices: return meta::Form::ChainVoice;
    case ChainSignal::None:
    case ChainSignal::Audio: break;
  }
  return meta::Form::ChainAudio;
}

ChainSignal chainSignalAt(const std::vector<DevicePtr>& list, bool notes, uint32_t index) {
  const size_t at = std::min<size_t>(index, list.size());
  const std::vector<DevicePtr> before(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(at));
  const std::vector<ChainStage> st = chainStages(before, notes);
  return st.empty() ? (notes ? ChainSignal::Notes : ChainSignal::Audio) : st.back().out;
}

std::vector<MetaRename> metaRenames(const graph::PatchFile& before, const graph::PatchFile& after, std::string_view root) {
  std::vector<MetaRename> out;
  const graph::Patch* rb = before.find(root);
  const graph::Patch* ra = after.find(root);
  if (rb == nullptr || ra == nullptr) return out;
  std::vector<CallPath> pb, pa;
  std::vector<const graph::Patch*> active;
  callPaths(before, *rb, {}, active, pb);
  callPaths(after, *ra, {}, active, pa);
  std::stable_sort(pa.begin(), pa.end(), [](const CallPath& a, const CallPath& b) { return a.ids.size() < b.ids.size(); });
  for (const CallPath& a : pa) {
    const auto it = std::find_if(pb.begin(), pb.end(), [&](const CallPath& b) { return b.ids == a.ids; });
    if (it == pb.end() || it->names == a.names) continue;
    // Where the outer renames accepted so far have moved the old path.
    std::string from = it->names;
    for (const MetaRename& r : out)
      if (under(from, r.from)) from = r.to + from.substr(r.from.size());
    if (from != a.names) out.push_back(MetaRename{from, a.names});
  }
  return out;
}

}  // namespace pq::doc
