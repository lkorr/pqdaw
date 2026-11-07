// Instrument groups (IG.1-2): pq/document/instrument_groups.h.
#include "pq/document/instrument_groups.h"

#include <algorithm>
#include <string>
#include <unordered_set>

#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/meta/registry.h"

namespace pq::doc {

namespace {

// A stock instrument or an Instrument Rack: a block whose own code is per voice (its main input is notes).
bool playsNotes(const Device& d) noexcept { return !isMetaDevice(d) && !isGroupDevice(d) && d.kind == DeviceKind::Instrument; }

bool midiTrack(const Project& p, TrackId t) {
  const Track* x = findTrackPtr(p, t);
  return x != nullptr && x->kind == TrackKind::Midi;
}

// A notes cable (a MIDI track's own signal, port "") into block `d`'s main input.
bool feedsNotes(const Project& p, const Wire& w, const Device& d) {
  if (!w.fromNoteSource() || !w.port.empty() || w.toMixer() || w.to != d.id) return false;
  if (!(w.param.empty() || (isGroupDevice(d) && w.param == "x"))) return false;
  return midiTrack(p, w.noteSource);
}

}  // namespace

const VoiceContext* VoiceContexts::blockOf(DeviceId d) const noexcept {
  const auto it = byBlock.find(d.value);
  return it != byBlock.end() ? &list[it->second] : nullptr;
}
const VoiceContext* VoiceContexts::memberOf(DeviceId d) const noexcept {
  const auto it = byMember.find(d.value);
  return it != byMember.end() ? &list[it->second] : nullptr;
}
const VoiceContext* VoiceContexts::of(DeviceId d) const noexcept {
  if (const VoiceContext* c = memberOf(d)) return c;
  return blockOf(d);
}

VoiceContexts voiceContexts(const Project& p) {
  VoiceContexts out;
  bool any = false;
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const DevicePtr& d : t.devices) any = any || isGroupDevice(*d) || playsNotes(*d);
  });
  if (!any) return out;
  std::unordered_map<uint32_t, std::vector<const Wire*>> feeds;  // block id → notes cables into it (id order)
  for (const Wire& w : wiresOf(p))
    if (w.fromNoteSource() && w.port.empty() && !w.toMixer() && w.to.valid()) feeds[w.to.value].push_back(&w);
  const MainLinks links(p);
  // The context of block `d` on track `t` whose main path head is `head`, or false.
  auto contextFor = [&](const Track& t, const Device& d, const Device* head, bool onPath, VoiceContext& c) {
    const Wire* cross = nullptr;
    const Wire* own = nullptr;
    if (const auto it = feeds.find(d.id.value); it != feeds.end())
      for (const Wire* w : it->second) {
        if (!feedsNotes(p, *w, d)) continue;
        if (w->noteSource != t.id) {
          if (cross == nullptr) cross = w;
        } else if (own == nullptr) {
          own = w;
        }
      }
    c.block = &d;
    c.home = &t;
    c.group = isGroupDevice(d);
    c.onPath = onPath;
    if (cross != nullptr) {
      c.source = cross->noteSource;
      c.feed = cross->id;
      return true;
    }
    if (t.kind == TrackKind::Midi && head == &d) {
      c.source = t.id;
      return true;
    }
    if (own != nullptr) {
      c.source = t.id;
      c.feed = own->id;
      return true;
    }
    return false;
  };
  forEachTrack(p, [&](const Track& t, const Track*) {
    const MainPath path = mainPath(t, links);
    const Device* head = nullptr;
    if (t.kind == TrackKind::Midi)
      for (const Device* b : path.blocks)
        if (b->active) {
          head = b;
          break;
        }
    // Instrument groups first (their members are not lone instruments).
    std::unordered_set<uint32_t> groups;
    for (const DevicePtr& d : t.devices) {
      if (!isGroupDevice(*d) || d->group.valid()) continue;
      VoiceContext c;
      if (!contextFor(t, *d, head, path.contains(d->id), c)) continue;
      const auto i = static_cast<uint32_t>(out.list.size());
      out.list.push_back(c);
      out.byBlock.emplace(d->id.value, i);
      groups.insert(d->id.value);
    }
    for (const DevicePtr& d : t.devices) {
      if (d->group.valid() && groups.count(d->group.value) != 0) {
        out.byMember.emplace(d->id.value, out.byBlock.at(d->group.value));
        continue;
      }
      if (!playsNotes(*d)) continue;
      VoiceContext c;
      if (!contextFor(t, *d, head, path.contains(d->id), c)) continue;
      out.byBlock.emplace(d->id.value, static_cast<uint32_t>(out.list.size()));
      out.list.push_back(c);
    }
  });
  return out;
}

bool isInstrumentGroup(const Project& p, DeviceId d) {
  const VoiceContext* c = voiceContexts(p).blockOf(d);
  return c != nullptr && c->group;
}

// ---- Migration 17 → 18 ----------------------------------------------------------------------------------------------

namespace {

// What the migration does to one track: new groups (each at a place in the list) and members joining groups.
struct TrackPlan {
  struct NewGroup {
    DevicePtr group;
    uint32_t index = kAppend;
  };
  std::vector<NewGroup> groups;
  std::vector<std::pair<DeviceId, DeviceId>> join;  // (device, its group)
};

std::string firstOutput(const Device& d) {
  const meta::Type* t = isMetaDevice(d) ? meta::findType(metaDeviceType(d)) : nullptr;
  return t != nullptr && !t->outputs.empty() ? std::string(t->outputs.front().name) : std::string();
}

Wire makeWire(WireId id) {
  Wire w;
  w.id = id;
  w.law = WireLaw::Add;
  return w;
}

void rebuild(std::vector<TrackPtr>& list, const std::unordered_map<uint32_t, TrackPlan>& plans) {
  for (TrackPtr& tp : list) {
    std::vector<TrackPtr> children = tp->children;
    rebuild(children, plans);
    const auto it = plans.find(tp->id.value);
    if (it == plans.end() && children == tp->children) continue;
    Track t = *tp;
    t.children = std::move(children);
    if (it != plans.end()) {
      const TrackPlan& pl = it->second;
      for (DevicePtr& d : t.devices)
        for (const auto& [dev, group] : pl.join)
          if (d->id == dev) {
            auto c = std::make_shared<Device>(*d);
            c->group = group;
            d = std::move(c);
          }
      // Later places first, so earlier indices stay put (a group at a block's index goes before that block).
      std::vector<TrackPlan::NewGroup> gs = pl.groups;
      std::stable_sort(gs.begin(), gs.end(), [](const auto& a, const auto& b) { return a.index > b.index; });
      for (const auto& g : gs) {
        const size_t at = std::min<size_t>(g.index, t.devices.size());
        t.devices.insert(t.devices.begin() + static_cast<std::ptrdiff_t>(at), g.group);
      }
    }
    tp = std::make_shared<const Track>(std::move(t));
  }
}

}  // namespace

bool migrateToInstrumentGroups(Project& p) {
  IdSource ids;
  ids.reserveThrough(maxEntityId(p));
  WireList wires = wiresOf(p);
  std::vector<WireId> removed;
  std::unordered_map<uint32_t, TrackPlan> plans;  // by track id
  std::unordered_set<uint32_t> placed;            // devices given a context here
  const MainLinks links(p);
  auto wireAt = [&](WireId id) -> Wire* {
    for (Wire& w : wires)
      if (w.id == id) return &w;
    return nullptr;
  };
  auto isMetaOff = [&](const Device& d, const MainPath& path) {
    return isMetaDevice(d) && d.active && !d.group.valid() && !path.contains(d.id) && meta::findType(metaDeviceType(d)) != nullptr;
  };
  auto newGroup = [&](TrackPlan& pl, uint32_t index) {
    const DeviceId g = ids.device();
    pl.groups.push_back({makeGroup(g), index});
    return g;
  };
  // Pass 1: each MIDI track's own voices.
  forEachTrack(p, [&](const Track& t, const Track*) {
    if (t.kind != TrackKind::Midi) return;
    const MainPath path = mainPath(t, links);
    const std::vector<ChainStage> stages = trackStages(p, t, path);
    auto stageOf = [&](const Device* d) -> const ChainStage* {
      for (const ChainStage& s : stages)
        if (s.device == d) return &s;
      return nullptr;
    };
    size_t h = path.blocks.size();
    for (size_t i = 0; i < path.blocks.size(); ++i)
      if (path.blocks[i]->active) {
        h = i;
        break;
      }
    const Device* head = h < path.blocks.size() ? path.blocks[h] : nullptr;
    std::vector<const Device*> series;
    DeviceId existing;
    if (head != nullptr) {
      if (isGroupDevice(*head)) {
        existing = head->id;
      } else if (playsNotes(*head)) {
        series.push_back(head);
      } else if (isMetaDevice(*head)) {
        const ChainStage* s = stageOf(head);
        if (s != nullptr && s->in == ChainSignal::Notes) {
          series.push_back(head);
          std::vector<const Device*> pending;  // switched-off blocks between run stages (they pass the voices on)
          for (size_t i = h + 1; i < path.blocks.size(); ++i) {
            const Device* b = path.blocks[i];
            if (!b->active) {
              pending.push_back(b);
              continue;
            }
            const ChainStage* bs = stageOf(b);
            if (!isMetaDevice(*b) || bs == nullptr || bs->in != ChainSignal::Voices) break;
            series.insert(series.end(), pending.begin(), pending.end());
            pending.clear();
            series.push_back(b);
          }
        }
      }
    }
    // Did the track play voices before (its head took the notes, or a notes cable from its source played one of its
    // blocks)?
    bool voiced = !series.empty() || existing.valid();
    for (const Wire& w : wiresOf(p))
      if (w.fromNoteSource() && w.noteSource == t.id && w.port.empty() && !w.toMixer() && t.deviceIndex(w.to) >= 0)
        voiced = true;
    if (!voiced) return;
    // The per-voice set V: the head's series (or the existing group's members) and, by closure, the off-path meta
    // modules that ran per voice — a per-voice signal reached them (a notes cable or note output of this track, a wire
    // from V) or they fed only V (a free-running source the compiler made per voice).
    std::unordered_set<uint32_t> v;
    for (const Device* d : series) v.insert(d->id.value);
    if (existing.valid())
      for (const DevicePtr& d : t.devices)
        if (d->group == existing) v.insert(d->id.value);
    std::vector<DeviceId> join;
    for (bool changed = true; changed;) {
      changed = false;
      for (const DevicePtr& dp : t.devices) {
        const Device& d = *dp;
        if (v.count(d.id.value) != 0 || !isMetaOff(d, path)) continue;
        bool in = false, outAny = false, outAll = true;
        for (const Wire& w : wiresOf(p)) {
          if (w.to == d.id && !w.toMixer() && (w.fromNoteSource() ? w.noteSource == t.id : v.count(w.from.value) != 0)) in = true;
          if (!w.fromNoteSource() && w.from == d.id) {
            outAny = true;
            if (w.toMixer() || v.count(w.to.value) == 0) outAll = false;
          }
        }
        if (in || (outAny && outAll)) {
          v.insert(d.id.value);
          join.push_back(d.id);
          changed = true;
        }
      }
    }
    if (series.empty() && !existing.valid() && join.empty()) return;
    TrackPlan& plan = plans[t.id.value];
    const DeviceId g =
        existing.valid() ? existing
                         : newGroup(plan, series.empty() ? kAppend
                                                         : static_cast<uint32_t>(std::max<int32_t>(t.deviceIndex(series.front()->id), 0)));
    for (const Device* d : series) plan.join.emplace_back(d->id, g);
    for (const DeviceId d : join) plan.join.emplace_back(d, g);
    if (!series.empty()) {
      // The group takes the series' place on the path: the link into its first block enters the group, the link out of
      // its last leaves the group (their ids kept); the links between them go.
      const size_t k1 = h + series.size() - 1;
      if (Wire* in = wireAt(path.links[h])) {
        in->to = g;
        in->param = std::string(kMainPort);
      }
      for (size_t k = h + 1; k <= k1 && k < path.links.size(); ++k) removed.push_back(path.links[k]);
      if (k1 + 1 < path.links.size())
        if (Wire* out = wireAt(path.links[k1 + 1])) {
          out->from = g;
          out->port = std::string(kMainPort);
        }
      // The series as chain links (groupInCircuit's rule): In → the first's chain input … → the last's output → Out.
      DeviceId at = g;
      std::string port(kGroupInPort);
      for (const Device* d : series) {
        std::string in, out;
        bool plugged = true;
        if (playsNotes(*d)) {
          in = out = std::string(kMainPort);  // an instrument's main input is its notes, its main output its sound
        } else {
          const meta::Type* ty = meta::findType(metaDeviceType(*d));
          const int32_t ci = ty != nullptr ? metaChainInput(*d, *ty) : -1;
          plugged = ci >= 0;
          if (plugged) in = std::string(ty->inputs[static_cast<size_t>(ci)].name);
          out = firstOutput(*d);
        }
        if (plugged) {
          Wire w = makeWire(ids.wire());
          w.from = at;
          w.port = port;
          w.to = d->id;
          w.param = in;
          w.chain = true;
          wires.push_back(w);
        }
        at = d->id;
        port = out;
      }
      Wire w = makeWire(ids.wire());
      w.from = at;
      w.port = port;
      w.to = g;
      w.param = std::string(kGroupOutPort);
      w.chain = true;
      wires.push_back(w);
    } else if (!existing.valid()) {
      Wire w = makeWire(ids.wire());  // a group off the path: the track's notes into it
      w.noteSource = t.id;
      w.to = g;
      wires.push_back(w);
    }
    for (const Device* d : series) placed.insert(d->id.value);
    for (const DeviceId d : join) placed.insert(d.value);
  });
  // Pass 2: off-path meta modules another MIDI track's notes played (TX.cross-track's guests): a group each, fed by
  // that source.
  forEachTrack(p, [&](const Track& t, const Track*) {
    const MainPath path = mainPath(t, links);
    for (const DevicePtr& dp : t.devices) {
      const Device& d = *dp;
      if (placed.count(d.id.value) != 0 || !isMetaOff(d, path)) continue;
      const meta::Type* ty = meta::findType(metaDeviceType(d));
      TrackId src;
      for (const Wire& w : wiresOf(p))
        if (w.fromNoteSource() && w.port.empty() && w.to == d.id && w.noteSource != t.id && midiTrack(p, w.noteSource) &&
            ty->inputIndex(w.param) >= 0) {
          src = w.noteSource;
          break;
        }
      if (!src.valid()) continue;
      TrackPlan& plan = plans[t.id.value];
      const DeviceId g = newGroup(plan, kAppend);
      plan.join.emplace_back(d.id, g);
      Wire w = makeWire(ids.wire());
      w.noteSource = src;
      w.to = g;
      wires.push_back(w);
      placed.insert(d.id.value);
    }
  });
  if (plans.empty()) return false;
  std::sort(removed.begin(), removed.end());
  std::erase_if(wires, [&](const Wire& w) { return std::binary_search(removed.begin(), removed.end(), w.id); });
  std::sort(wires.begin(), wires.end(), [](const Wire& a, const Wire& b) { return a.id < b.id; });
  rebuild(p.tracks, plans);
  rebuild(p.returns, plans);
  if (p.master) {
    std::vector<TrackPtr> m{p.master};
    rebuild(m, plans);
    p.master = m.front();
  }
  p.wires = wires.empty() ? nullptr : std::make_shared<const WireList>(std::move(wires));
  return true;
}

