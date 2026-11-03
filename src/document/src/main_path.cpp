// TX.all-free: a track's main path (pq/document/main_path.h).
#include "pq/document/main_path.h"

#include <algorithm>

#include "pq/document/devices.h"
#include "pq/document/meta.h"
#include "pq/meta/registry.h"

namespace pq::doc {

MainPorts mainPortsOf(const graph::SubcircuitRef& sig, const graph::Patch* callee, bool instrument) {
  MainPorts m;
  if (!sig.outputs.empty()) m.out = sig.outputs.front().name;
  if (instrument) return m;
  for (const graph::SubcircuitPort& sp : sig.inputs) {
    if (sp.type.rate == graph::Rate::Const) continue;
    if (callee != nullptr) {
      const std::optional<graph::NodeIndex> n = callee->findByName(sp.name);
      if (!n || callee->node(*n).kind != graph::NodeKind::Input) continue;  // a param
      if (sp.name.size() > 4 && sp.name.ends_with("_add"))  // a param's added wires (TX.patching), not a signal input
        if (const auto q = callee->findByName(std::string_view(sp.name).substr(0, sp.name.size() - 4));
            q && callee->node(*q).kind == graph::NodeKind::Param)
          continue;
      if (callee->outputType(*n, 0).rate == graph::Rate::Const) continue;
      bool drives = false;
      for (const graph::Wire& w : callee->wires())
        if (w.from.node == *n) {
          drives = true;
          break;
        }
      if (!drives) continue;  // an input wired to nothing carries nothing into the node
    } else if (sp.optional) {
      continue;
    }
    m.in = sp.name;
    break;
  }
  return m;
}

MainPorts mainPortsOf(const Device& d, const DeviceLibrary* lib) {
  if (d.isRack()) return d.kind == DeviceKind::Instrument ? MainPorts{"", "out"} : MainPorts{"in", "out"};
  if (isGroupDevice(d)) return {"x", "y"};
  if (isMetaDevice(d)) {
    MainPorts m;
    if (const meta::Type* t = meta::findType(metaDeviceType(d))) {
      if (const int32_t i = metaChainInput(d, *t); i >= 0) m.in = std::string(t->inputs[static_cast<size_t>(i)].name);
      if (!t->outputs.empty()) m.out = std::string(t->outputs.front().name);
    }
    return m;
  }
  if (lib == nullptr) return {};
  std::shared_ptr<const ResolvedDevice> hold;
  const graph::Patch* p = lib->patchOf(d, hold);
  if (p == nullptr) return {};
  return mainPortsOf(graph::subcircuitSignature(*p), p, d.kind == DeviceKind::Instrument);
}

int32_t MainPath::indexOf(DeviceId d) const noexcept {
  for (size_t i = 0; i < blocks.size(); ++i)
    if (blocks[i]->id == d) return static_cast<int32_t>(i);
  return -1;
}

bool MainPath::isLink(WireId w) const noexcept { return std::find(links.begin(), links.end(), w) != links.end(); }

namespace {

// The path of `t` over `wires` (any range of Wire / Wire pointers holding at least its main links, in id order).
// A main link of a track: from the source (null) or a block, into a block or the mixer (null).
struct PathLink {
  const Wire* w;
  const Device* from;
  const Device* to;
};
// Scratch kept per thread (UI and compile threads call this; never the audio thread): no allocation per call once warm.
struct PathScratch {
  std::vector<const Device*> blocks, reaches, todo;
  std::vector<PathLink> links;
};

template <class Range, class Get>
MainPath pathOver(const Track& t, const Range& wires, Get get) {
  // A track holds a handful of blocks: small vectors searched linearly beat hashing (this runs on every Device view
  // and cable refresh, once per track per project patch rebuild and lowering).
  MainPath out;
  if (wires.empty()) return out;
  thread_local PathScratch scratch;
  // The blocks a link may enter: the track's own, group members apart (they are inside their group).
  std::vector<const Device*>& blocks = scratch.blocks;
  blocks.clear();
  for (const DevicePtr& d : t.devices)
    if (!d->group.valid() || t.deviceIndex(d->group) < 0) blocks.push_back(d.get());
  auto blockOf = [&](DeviceId id) -> const Device* {
    for (const Device* d : blocks)
      if (d->id == id) return d;
    return nullptr;
  };
  // The main links of the track, in id order.
  using Link = PathLink;
  std::vector<Link>& links = scratch.links;
  links.clear();
  for (const auto& item : wires) {
    const Wire& w = get(item);
    if (!w.mainLink()) continue;
    const Device* from = nullptr;
    const Device* to = nullptr;
    if (w.fromNoteSource()) {
      if (w.noteSource != t.id) continue;
    } else if ((from = blockOf(w.from)) == nullptr) {
      continue;
    }
    if (w.toMixer()) {
      if (w.mixer != t.id) continue;
    } else if ((to = blockOf(w.to)) == nullptr) {
      continue;
    }
    links.push_back({&w, from, to});
  }
  if (links.empty()) return out;
  auto has = [](const std::vector<const Device*>& v, const Device* d) { return std::find(v.begin(), v.end(), d) != v.end(); };
  // Which blocks reach the mixer along main links (backwards from it).
  std::vector<const Device*>& reaches = scratch.reaches;
  reaches.clear();
  {
    std::vector<const Device*>& todo = scratch.todo;
    todo.clear();
    for (const Link& l : links)
      if (l.w->toMixer() && l.from != nullptr && !has(reaches, l.from)) reaches.push_back(l.from), todo.push_back(l.from);
    while (!todo.empty()) {
      const Device* b = todo.back();
      todo.pop_back();
      for (const Link& l : links)
        if (!l.w->toMixer() && l.to == b && l.from != nullptr && !has(reaches, l.from))
          reaches.push_back(l.from), todo.push_back(l.from);
    }
  }
  const Device* at = nullptr;  // the source
  for (size_t guard = 0; guard <= blocks.size(); ++guard) {
    const Link* best = nullptr;
    bool bestReaches = false;
    for (const Link& l : links) {
      if (at == nullptr ? !l.w->fromNoteSource() : l.from != at) continue;
      if (!l.w->toMixer() && has(out.blocks, l.to)) continue;  // never a block twice
      const bool r = l.w->toMixer() || has(reaches, l.to);
      if (best == nullptr || (r && !bestReaches)) {  // links are in id order: the first of each kind is the oldest
        best = &l;
        bestReaches = r;
      }
    }
    if (best == nullptr) break;
    out.links.push_back(best->w->id);
    if (best->w->toMixer()) {
      out.complete = true;
      break;
    }
    out.blocks.push_back(best->to);
    at = best->to;
  }
  return out;
}

}  // namespace

MainPath mainPath(const Project& p, const Track& t) {
  return pathOver(t, wiresOf(p), [](const Wire& w) -> const Wire& { return w; });
}

MainLinks::MainLinks(const Project& p) {
  const WireList& wires = wiresOf(p);
  size_t n = 0;
  for (const Wire& w : wires) n += w.mainLink() ? 1 : 0;
  if (n == 0) return;
  std::vector<std::pair<uint32_t, uint32_t>> trackOf;  // top-level block → its track, sorted by block id
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const DevicePtr& d : t.devices) trackOf.emplace_back(d->id.value, t.id.value);
  });
  std::sort(trackOf.begin(), trackOf.end());
  by_.reserve(n);
  for (const Wire& w : wires) {
    if (!w.mainLink()) continue;
    if (w.fromNoteSource()) {
      by_.emplace_back(w.noteSource.value, &w);
      continue;
    }
    const auto it = std::lower_bound(trackOf.begin(), trackOf.end(), std::pair<uint32_t, uint32_t>{w.from.value, 0});
    if (it != trackOf.end() && it->first == w.from.value) by_.emplace_back(it->second, &w);
  }
  // By track, each track's links still in id order (the wires are).
  std::stable_sort(by_.begin(), by_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
}

std::span<const std::pair<uint32_t, const Wire*>> MainLinks::of(TrackId t) const noexcept {
  const auto lo = std::lower_bound(by_.begin(), by_.end(), t.value, [](const auto& e, uint32_t v) { return e.first < v; });
  auto hi = lo;
  while (hi != by_.end() && hi->first == t.value) ++hi;
  return {lo, hi};
}

MainPath mainPath(const Track& t, const MainLinks& links) {
  return pathOver(t, links.of(t.id), [](const std::pair<uint32_t, const Wire*>& e) -> const Wire& { return *e.second; });
}

std::vector<DevicePtr> flowOrder(const Track& t, const MainPath& path) {
  std::vector<DevicePtr> out;
  out.reserve(t.devices.size());
  for (const Device* d : path.blocks)
    for (const DevicePtr& x : t.devices)
      if (x.get() == d) out.push_back(x);
  for (const DevicePtr& x : t.devices)
    if (!path.contains(x->id)) out.push_back(x);
  return out;
}

Wire canonicalWire(const Project& p, Wire w, const DeviceLibrary* lib) {
  if (!w.port.empty() && !w.fromNoteSource())
    if (const Device* d = findDevice(p, w.from).device; d != nullptr && findDevice(p, w.from).chain == nullptr)
      if (const MainPorts m = mainPortsOf(*d, lib); !m.out.empty() && m.out == w.port) w.port.clear();
  if (!w.param.empty() && !w.toMixer())
    if (const Device* d = findDevice(p, w.to).device; d != nullptr && findDevice(p, w.to).chain == nullptr)
      if (const MainPorts m = mainPortsOf(*d, lib); !m.in.empty() && m.in == w.param) w.param.clear();
  return w;
}

}  // namespace pq::doc
