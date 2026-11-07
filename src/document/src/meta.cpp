// Meta modules in the document (TMM.3): pq/document/meta.h.
#include "pq/document/meta.h"

#include <algorithm>

#include "pq/graph/infer.h"
#include "pq/meta/registry.h"
#include "pq/meta/retired.h"

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

double metaParamPosition(const Device& d, std::string_view path, const meta::Type& t, size_t i) {  // GU.1
  const meta::ParamSpec& s = t.params[i];
  const DeviceValue* v = nullptr;
  if (path.empty()) {
    v = d.param(s.name);
  } else if (const MetaInstance* inst = d.metaAt(path)) {
    v = inst->param(s.name);
  }
  return v != nullptr ? v->value : dsp::curvePosition(meta::paramCurve(s), s.def, 0.0);
}

dsp::Curve metaParamCurve(const Device& d, std::string_view path, const meta::Type& t, size_t i) {
  if (path.empty())
    if (const dsp::Curve* c = d.curve(t.params[i].name)) return *c;
  return meta::paramCurve(t.params[i]);
}

double metaParamValue(const Device& d, std::string_view path, const meta::Type& t, size_t i) {
  return dsp::evalCurve(metaParamCurve(d, path, t, i), metaParamPosition(d, path, t, i));
}

std::string metaParamBakedOnly(const Device& d, std::string_view param) {
  if (!isMetaDevice(d)) return {};
  const meta::Type* t = meta::findTypeOrRetired(metaDeviceType(d));
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
  (void)d;
  return t.chainInput >= 0 && static_cast<size_t>(t.chainInput) < t.inputs.size() ? t.chainInput : -1;
}

std::vector<ChainStage> chainStages(const std::vector<DevicePtr>& list, bool notes, bool voices, const MainPath* path) {
  std::vector<ChainStage> out;
  out.reserve(list.size());
  ChainSignal s = notes ? ChainSignal::Notes : ChainSignal::Audio;
  for (const DevicePtr& dp : list) {
    const Device& d = *dp;
    if (!d.active) continue;
    ChainStage st;
    st.device = &d;
    const meta::Type* t = isMetaDevice(d) ? meta::findTypeOrRetired(metaDeviceType(d)) : nullptr;
    if (path != nullptr && !path->contains(d.id)) {  // TX.all-free: off the main path: nothing reaches it from the flow
      st.offPath = true;
      st.in = ChainSignal::None;
      st.out = ChainSignal::Audio;
      out.push_back(st);
      continue;
    }
    if (t != nullptr) {
      st.input = metaChainInput(d, *t);
      st.in = st.input < 0 ? ChainSignal::None : s;
      st.out = st.in == ChainSignal::Notes || st.in == ChainSignal::Voices ? ChainSignal::Voices : ChainSignal::Audio;
      if (!voices && st.out == ChainSignal::Voices && st.in == ChainSignal::Voices) st.in = st.out = ChainSignal::Audio;
    } else if (isGroupDevice(d)) {
      // TX.groups-fix1: a group passes on what reaches it — the notes or voices continue as voices through its terminals
      // (lower.h "Groups"); audio stays audio.
      st.in = s;
      // IG.1-2: an instrument group sums its voices at Out: what follows it hears audio.
      st.out = ChainSignal::Audio;
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

std::vector<ChainStage> trackStages(const Project& p, const Track& t) { return trackStages(p, t, mainPath(p, t)); }

namespace {
// IG.1-2: the members of group `g` along its series — from its In terminal through each member's main output into the
// next member's chain input (a meta module's chain input by name, else kMainPort), as Ctrl+G and new tracks wire them.
std::vector<DevicePtr> groupSeries(const Project& p, const Track& t, DeviceId g) {
  std::vector<DevicePtr> members;
  for (const DevicePtr& d : t.devices)
    if (d->group == g) members.push_back(d);
  std::vector<DevicePtr> out;
  DeviceId cur = g;
  std::string port(kGroupInPort);
  for (size_t guard = 0; guard < members.size(); ++guard) {
    const DevicePtr* next = nullptr;
    for (const Wire& w : wiresOf(p)) {
      // (GU.4: a nested group's main output by its name too — `y`, as Ctrl+G's chain links name it.)
      const bool fromCur = w.port == port || (cur != g && w.port == kMainPort && port == "y");
      if (w.fromNoteSource() || w.toMixer() || w.from != cur || !fromCur) continue;
      for (const DevicePtr& m : members) {
        if (m->id != w.to || std::find(out.begin(), out.end(), m) != out.end()) continue;
        const meta::Type* ty = isMetaDevice(*m) ? meta::findTypeOrRetired(metaDeviceType(*m)) : nullptr;
        const int32_t ci = ty != nullptr ? metaChainInput(*m, *ty) : -1;
        if (w.param == kMainPort || (ci >= 0 && w.param == ty->inputs[static_cast<size_t>(ci)].name) ||
            (isGroupDevice(*m) && w.param == "x"))
          next = &m;
      }
      if (next != nullptr) break;
    }
    if (next == nullptr) break;
    out.push_back(*next);
    cur = (*next)->id;
    const meta::Type* ty = isMetaDevice(**next) ? meta::findTypeOrRetired(metaDeviceType(**next)) : nullptr;
    port = ty != nullptr && !ty->outputs.empty() ? std::string(ty->outputs.front().name)
           : isGroupDevice(**next)                  ? std::string("y")
                                                    : std::string(kMainPort);
  }
  return out;
}
}  // namespace

std::vector<ChainStage> trackStages(const Project& p, const Track& t, const MainPath& path) {
  std::vector<ChainStage> out = chainStages(flowOrder(t, path), t.kind == TrackKind::Midi, true, &path);
  // IG.1-2: a group on the path passes what reaches it to its series (an instrument group: the notes, per note); its
  // members follow, by its series (the others: off it).
  for (size_t i = 0, n = out.size(); i < n; ++i) {
    const ChainStage st = out[i];
    if (st.offPath || !isGroupDevice(*st.device)) continue;
    const std::vector<DevicePtr> series = groupSeries(p, t, st.device->id);
    for (ChainStage m : chainStages(series, st.in == ChainSignal::Notes || st.in == ChainSignal::Voices)) out.push_back(m);
    for (const DevicePtr& d : t.devices)
      if (d->group == st.device->id && d->active && std::find(series.begin(), series.end(), d) == series.end()) {
        ChainStage m;
        m.device = d.get();
        m.offPath = true;
        m.in = ChainSignal::None;
        out.push_back(m);
      }
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
  ChainSignal s = notes ? ChainSignal::Notes : ChainSignal::Audio;
  for (const ChainStage& st : chainStages(before, notes)) s = st.out;
  return s;
}

}  // namespace pq::doc
