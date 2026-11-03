// TX.node-menu (SPEC §15 2026-11-02 "cables and the node menu"): the project patch view's node placements — spawn at the
// pointer (free blocks, wired from a cable), insert on a cable, Z (a VCA at 1), Shift-splice, dissolve — each composed
// from the document's edit builders on a scratch snapshot and dispatched as ONE Batch (one undo step); and the push-apart
// of blocks after a spawn (view state). See pq/views/project_patch/project_patch_view.h "node menu".

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "internal.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/library_files.h"
#include "pq/meta/registry.h"
#include "pq/views/browser/library.h"
#include "pq/views/browser/library_load.h"
#include "pq/views/project_patch/project_patch_view.h"

namespace pq::views::project_patch {

using ui::Vec2;

namespace {

// Builders applied in turn to a scratch snapshot (each reads what the previous made), kept as the steps of one Batch.
class Composer {
public:
  Composer(doc::Snapshot s, doc::IdSource& ids) : snap_(std::move(s)), ids_(ids) {}
  const doc::Project& doc() const noexcept { return *snap_; }
  doc::IdSource& ids() noexcept { return ids_; }
  std::vector<doc::Command> steps;
  std::string why;
  bool add(const doc::ops::EditResult& e) {
    if (!e) {
      why = e.error().message;
      return false;
    }
    if (e->empty()) return true;
    return add(e->command);
  }
  bool add(const doc::Command& c) {
    doc::ChangeSet cs;
    auto next = doc::apply(snap_, c, cs);
    if (!next) {
      why = next.error().message;
      return false;
    }
    snap_ = std::move(*next);
    steps.push_back(c);
    return true;
  }

private:
  doc::Snapshot snap_;
  doc::IdSource& ids_;
};

// The main ports of `d` as the library describes the patch it plays (racks: none of their own; the chain is theirs).
circuit::MainPorts mainPortsOf(const doc::DeviceLibrary* lib, const doc::Device& d) {
  if (lib == nullptr || d.isRack()) return {};
  std::shared_ptr<const doc::ResolvedDevice> hold;
  const graph::Patch* p = lib->patchOf(d, hold);
  if (p == nullptr) return {};
  return circuit::mainPortsOf(graph::subcircuitSignature(*p), p, d.kind == doc::DeviceKind::Instrument);
}

// The edit that adds `c`'s device to `track` at list position `index`. `plain`: no default preset (Z's VCA is exactly
// the type's default: ×1). Its Edit::devices holds the new device first.
doc::ops::EditResult addChoice(ViewContext& ctx, const doc::Project& p, doc::IdSource& ids, const ProjectPatchView::NodeChoice& c,
                               doc::TrackId track, uint32_t index, bool plain) {
  auto paste = [&](doc::DevicePtr d) {
    return doc::ops::pasteDevices(p, ids, track, std::span<const doc::DevicePtr>(&d, 1), index);
  };
  auto meta = [&](std::string_view id) -> doc::ops::EditResult {
    const meta::Type* t = meta::findType(id);
    if (t == nullptr) return Error{ErrorCode::NotFound, "no meta module '" + std::string(id) + "'", {}};
    const std::string patch = std::string(doc::kMetaDevicePrefix) + std::string(id);
    if (!plain)
      if (doc::DevicePtr def = ctx.library().defaultPreset(deviceDisplayName(ctx, patch), patch); def && def->patch == patch) {
        auto k = doc::ops::metaDeviceKindAt(p, *t, track, index);
        if (!k) return std::move(k).error();
        auto d = std::make_shared<doc::Device>(*def);
        d->kind = *k;
        return paste(d);
      }
    return doc::ops::addMetaDevice(p, ids, track, id, index);
  };
  switch (c.item) {
    case circuit::NodeItem::Meta: return meta(c.id);
    case circuit::NodeItem::Device: {
      const doc::DeviceLibrary* lib = ctx.deviceLibrary();
      const doc::DeviceInfo* info = lib != nullptr ? lib->catalog().find(c.id) : nullptr;
      const doc::DeviceKind kind = info != nullptr ? info->kind : doc::DeviceKind::AudioEffect;
      if (!plain)
        if (doc::DevicePtr def = ctx.library().defaultPreset(deviceDisplayName(ctx, c.id), c.id); def && def->kind == kind) return paste(def);
      return doc::ops::addDevice(p, ids, track, c.id, kind, index);
    }
    case circuit::NodeItem::Rack:
      return doc::ops::addDevice(p, ids, track, c.id, c.id == doc::kInstrumentRack ? doc::DeviceKind::Instrument : doc::DeviceKind::AudioEffect, index);
    case circuit::NodeItem::LibraryFile: {
      auto d = libraryDeviceOf(c.id);
      if (!d) return std::move(d).error();
      if (doc::isMetaDevice(**d))
        if (const meta::Type* t = meta::findType(doc::metaDeviceType(**d))) {
          auto k = doc::ops::metaDeviceKindAt(p, *t, track, index);
          if (!k) return std::move(k).error();
          auto m = std::make_shared<doc::Device>(**d);
          m->kind = *k;
          *d = std::move(m);
        }
      return paste(*d);
    }
    case circuit::NodeItem::Atom:
    case circuit::NodeItem::Spec: {
      // A typed `meta_<type>` is that meta module (its face), not a device around a call.
      if (c.item == circuit::NodeItem::Spec && c.id.starts_with(meta::kCallPrefix))
        if (meta::findType(std::string_view(c.id).substr(meta::kCallPrefix.size())) != nullptr)
          return meta(std::string_view(c.id).substr(meta::kCallPrefix.size()));
      auto sd = circuit::specDevice(c.id);
      if (!sd) return std::move(sd).error();
      auto d = std::make_shared<doc::Device>();
      d->id = doc::DeviceId{1};
      d->patch = sd->patch;
      d->kind = doc::DeviceKind::AudioEffect;
      d->name = sd->name;
      d->source = std::make_shared<const doc::UserPatch>(doc::UserPatch{sd->text, nullptr});
      return paste(d);
    }
  }
  return Error{ErrorCode::InvalidArgument, "nothing to place", {}};
}

bool choiceIsInstrument(ViewContext& ctx, const ProjectPatchView::NodeChoice& c) {
  if (c.item == circuit::NodeItem::Rack) return c.id == doc::kInstrumentRack;
  if (c.item == circuit::NodeItem::Device) {
    const doc::DeviceLibrary* lib = ctx.deviceLibrary();
    const doc::DeviceInfo* info = lib != nullptr ? lib->catalog().find(c.id) : nullptr;
    return info != nullptr && info->kind == doc::DeviceKind::Instrument;
  }
  if (c.item == circuit::NodeItem::LibraryFile)
    if (auto d = libraryDeviceOf(c.id); d && !doc::isMetaDevice(**d)) return (*d)->kind == doc::DeviceKind::Instrument;
  return false;
}

// Wires `from`.`port` (or note source `notes`'s output `port`) → `to`.`param` in the composition.
bool wire(Composer& k, doc::DeviceId from, doc::TrackId notes, const std::string& port, doc::DeviceId to, const std::string& param) {
  if (notes.valid()) return k.add(doc::ops::connectNoteWire(k.doc(), k.ids(), notes, port, to, param));
  return k.add(doc::ops::connectWire(k.doc(), k.ids(), from, port, to, param));
}

}  // namespace

ProjectPatchView::NodeChoice ProjectPatchView::choiceOf(const circuit::NodeEntry& e) { return {e.item, e.id, e.name}; }
ProjectPatchView::NodeChoice ProjectPatchView::specChoice(std::string spec) {
  NodeChoice c;
  c.item = circuit::NodeItem::Spec;
  c.name = spec;
  c.id = std::move(spec);
  return c;
}

circuit::MainPorts ProjectPatchView::mainPorts(doc::DeviceId device) const {
  const doc::Device* d = doc::findDevice(*ctx_.doc(), device).device;
  return d != nullptr ? mainPortsOf(ctx_.deviceLibrary(), *d) : circuit::MainPorts{};
}

bool ProjectPatchView::dispatchSteps(const char* label, std::vector<doc::Command> steps) {
  if (steps.empty()) return false;
  return dispatch(doc::Batch{label, std::move(steps)}, label);
}

// ---- spawn ---------------------------------------------------------------------------------------------------------

doc::DeviceId ProjectPatchView::spawnNode(const NodeChoice& c, graph::Point2 at, const SpawnLink& link) {
  sync();
  const doc::Snapshot start = ctx_.doc();
  const doc::Project& p = *start;
  // Where: the free rack's track, the group panel's group, else the band under the point (the nearest one) and an opened
  // capsule of it under the point.
  doc::TrackId track;
  doc::DeviceId group;
  uint32_t band = kNone;
  if (freeRack_) track = rackTrack_;
  if (scope_.valid()) {
    group = scope_;
    if (const doc::Track* t = doc::findDevice(p, scope_).track) track = t->id;
  }
  if (!track.valid()) {
    float best = 0.0f;
    for (uint32_t b = 0; b < patch_.bands.size(); ++b) {
      const Band& bb = patch_.bands[b];
      if (!bb.track.valid()) continue;
      const float d = at.y < bb.y ? bb.y - at.y : at.y > bb.y + bb.h ? at.y - (bb.y + bb.h) : 0.0f;
      if (band == kNone || d < best) {
        band = b;
        best = d;
      }
    }
    if (band == kNone) {
      ctx_.setStatus("Add: there is no track to place it on");
      return {};
    }
    track = patch_.bands[band].track;
  } else {
    band = patch_.bandOf(track);
  }
  if (!group.valid())
    for (const Node& n : patch_.nodes)
      if (n.group && n.groupOpen && (band == kNone || n.band == band) && at.x >= n.x && at.x <= n.x + n.w && at.y >= n.y && at.y <= n.y + n.h)
        group = n.device;
  const bool instrument = choiceIsInstrument(ctx_, c);
  const bool rack = c.item == circuit::NodeItem::Rack;
  // A stock instrument or a rack cannot be free: it goes into the chain (an instrument first; a rack where the point
  // is along the chain).
  uint32_t index = doc::kAppend;
  if (rack && !instrument && band != kNone) {
    index = 0;
    const doc::Track* t = doc::findTrackPtr(p, track);
    for (const uint32_t d : patch_.bands[band].devices) {
      const Node& n = patch_.nodes[d];
      if (n.free || n.capsule != kNone || t == nullptr) continue;
      if (n.x + n.w * 0.5f < at.x) index = static_cast<uint32_t>(std::max(0, t->deviceIndex(n.device)) + 1);
    }
  }
  if ((instrument || rack) && (freeRack_ || group.valid())) {
    ctx_.setStatus("Add: " + c.name + " goes into a chain, not into a free rack or a group");
    return {};
  }
  Composer k(start, ctx_.store().ids());
  doc::ops::EditResult add = addChoice(ctx_, p, k.ids(), c, track, index, false);
  if (!add || add->devices.empty()) {
    ctx_.setStatus("Add " + c.name + ": " + (add ? std::string("nothing was added") : add.error().message));
    return {};
  }
  const doc::DeviceId id = add->devices.front();
  if (!k.add(add)) {
    ctx_.setStatus("Add " + c.name + ": " + k.why);
    return {};
  }
  const doc::Device* made = doc::findDevice(k.doc(), id).device;
  const bool free = made != nullptr && made->kind != doc::DeviceKind::Instrument && !made->isRack();
  if (free) {
    const doc::DeviceId one[1] = {id};
    if (!k.add(doc::ops::setDevicesFree(k.doc(), one, true))) {
      ctx_.setStatus("Add " + c.name + ": " + k.why);
      return {};
    }
    if (group.valid() && !k.add(doc::SetDeviceGroup{id, group})) {
      ctx_.setStatus("Add " + c.name + ": " + k.why);
      return {};
    }
  }
  // Wired from the cable it was dropped from: into its main input, or its main output into the param.
  std::string note;
  if (link.kind != SpawnLink::Kind::None && made != nullptr) {
    const circuit::MainPorts mp = mainPortsOf(ctx_.deviceLibrary(), *doc::findDevice(k.doc(), id).device);
    if (link.kind == SpawnLink::Kind::FromOutput) {
      if (mp.in.empty()) note = c.name + " has no input: placed unwired";
      else if (!wire(k, link.device, link.notes, link.port, id, mp.in)) note = "Wire: " + k.why;
    } else {
      if (mp.out.empty()) note = c.name + " has no output: placed unwired";
      else if (!wire(k, id, {}, mp.out, link.device, link.port)) note = "Wire: " + k.why;
    }
  }
  const std::string label = "Add " + c.name;
  if (!dispatchSteps(label.c_str(), std::move(k.steps))) return {};
  if (c.item == circuit::NodeItem::LibraryFile && libraryItemOf(c.id) == LibraryItem::Preset) {
    std::string stem = c.id.substr(c.id.find_last_of("\\/") + 1);
    if (const size_t dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
    ctx_.library().setPresetName(id, stem);
  }
  if (!note.empty()) ctx_.setStatus(note);
  if (free) {
    placeAt(id, at);
    startPush(id);
  }
  return id;
}

void ProjectPatchView::placeAt(doc::DeviceId device, graph::Point2 at) {
  sync();
  const uint32_t n = patch_.nodeOf(device);
  if (n == kNone) return;
  const Node& nd = patch_.nodes[n];
  float dx = at.x - (nd.x + nd.w * 0.5f), dy = at.y - (nd.y + nd.h * 0.5f);
  // Never above its band's title strip (the band grows down and right to hold it, never up).
  if (nd.band < patch_.bands.size()) {
    const float top = patch_.bands[nd.band].y + metrics::kTitle;
    if (nd.y + dy < top) dy = top - nd.y;
  }
  nudge(device.value, dx, dy);
}

// ---- push-apart ------------------------------------------------------------------------------------------------------

void ProjectPatchView::startPush(doc::DeviceId placed) {
  push_ = {};
  sync();
  const uint32_t pn = patch_.nodeOf(placed);
  if (pn == kNone) return;
  const Node& p = patch_.nodes[pn];
  const float minY = p.band < patch_.bands.size() ? patch_.bands[p.band].y + metrics::kTitle : -1e9f;
  auto& nudges = ctx_.store().view().patchNudges;
  auto body = [&](const Node& n, bool movable) {
    Push::Body b;
    b.device = n.device;
    b.x = n.x, b.y = n.y, b.w = n.w, b.h = n.h;
    b.minY = minY;
    b.movable = movable;
    if (const auto it = nudges.find(n.device.value); it != nudges.end()) b.base = it->second;
    return b;
  };
  push_.bodies.push_back(body(p, true));
  // What it may meet: the blocks of its band at its level (inside the same capsule, or outside any): free blocks move,
  // chain blocks, the source, the mixer and capsules stand.
  for (uint32_t i = 0; i < patch_.nodes.size(); ++i) {
    if (i == pn) continue;
    const Node& n = patch_.nodes[i];
    if (n.band != p.band || n.capsule != p.capsule) continue;
    if (n.kind == NodeKind::Terminal || (n.group && n.groupOpen && i == p.capsule)) continue;
    const bool movable = n.kind == NodeKind::Device && n.free && !(n.group && n.groupOpen) && !n.expanded;
    push_.bodies.push_back(body(n, movable));
  }
  // Nothing in the way: no push.
  bool any = false;
  const Push::Body& a = push_.bodies.front();
  for (size_t j = 1; j < push_.bodies.size() && !any; ++j) {
    const Push::Body& b = push_.bodies[j];
    any = a.x < b.x + b.w + kPushGap && b.x < a.x + a.w + kPushGap && a.y < b.y + b.h + kPushGap && b.y < a.y + a.h + kPushGap;
  }
  if (!any) {
    push_.bodies.clear();
    return;
  }
  // Where they end: overlapping pairs (closer than kPushGap) are separated along their axis of least overlap — split
  // between two free blocks, all of it on the free one against a standing block — until none overlaps (the band's
  // title strip is a wall: a pair it blocks vertically separates sideways).
  for (int iter = 0; iter < 256; ++iter) {
    bool overlap = false;
    for (size_t i = 0; i < push_.bodies.size(); ++i)
      for (size_t j = i + 1; j < push_.bodies.size(); ++j) {
        Push::Body& u = push_.bodies[i];
        Push::Body& v = push_.bodies[j];
        if (!u.movable && !v.movable) continue;
        const float ux = u.x + u.tx, uy = u.y + u.ty, vx = v.x + v.tx, vy = v.y + v.ty;
        const float ox = std::min(ux + u.w + kPushGap - vx, vx + v.w + kPushGap - ux);
        const float oy = std::min(uy + u.h + kPushGap - vy, vy + v.h + kPushGap - uy);
        if (ox <= 0.0f || oy <= 0.0f) continue;
        overlap = true;
        const float su = u.movable && v.movable ? 0.5f : (u.movable ? 1.0f : 0.0f), sv = u.movable && v.movable ? 0.5f : (v.movable ? 1.0f : 0.0f);
        const bool uFirst = (ux + u.w * 0.5f) <= (vx + v.w * 0.5f);  // u left of v (sideways) …
        const bool uAbove = (uy + u.h * 0.5f) <= (vy + v.h * 0.5f);  // … u above v (vertically)
        bool vertical = oy < ox;
        if (vertical) {  // the wall: the one going up must stay below the title strip
          Push::Body& up = uAbove ? u : v;
          const float upShare = uAbove ? su : sv;
          if (up.movable && up.y + up.ty - (oy + 0.5f) * upShare < up.minY) vertical = false;
        }
        if (vertical) {
          const float d = oy + 0.5f;
          u.ty += (uAbove ? -d : d) * su;
          v.ty += (uAbove ? d : -d) * sv;
        } else {
          const float d = ox + 0.5f;
          u.tx += (uFirst ? -d : d) * su;
          v.tx += (uFirst ? d : -d) * sv;
        }
      }
    if (!overlap) break;
  }
  for (Push::Body& b : push_.bodies) b.ty = std::max(b.ty, b.minY - b.y);
  if (!pushAnimated_ || !shown()) {
    finishPushNow();
    return;
  }
  requestFrame();
}

bool ProjectPatchView::stepPush(double seconds) {
  // Each moving block approaches its solved place (startPush) as a critically damped spring (no overshoot, ≈ 0.3 s):
  // a'' = ω²·(target − x) − 2ω·x'. Fixed sub-steps: the same path at any frame rate. Settled: within half a unit and
  // slower than 4 units / s (then exactly at the target).
  constexpr double kStep = 1.0 / 240.0;
  constexpr float kOmega = 24.0f;
  const int steps = std::clamp(static_cast<int>(std::ceil(seconds / kStep)), 1, 240);
  const float dt = static_cast<float>(kStep);
  bool moving = false;
  for (Push::Body& b : push_.bodies) {
    if (!b.movable) continue;
    for (int s = 0; s < steps; ++s) {
      b.vx += (kOmega * kOmega * (b.tx - b.dx) - 2.0f * kOmega * b.vx) * dt;
      b.vy += (kOmega * kOmega * (b.ty - b.dy) - 2.0f * kOmega * b.vy) * dt;
      b.dx += b.vx * dt;
      b.dy += b.vy * dt;
    }
    if (std::fabs(b.tx - b.dx) < 0.5f && std::fabs(b.ty - b.dy) < 0.5f && std::fabs(b.vx) < 4.0f && std::fabs(b.vy) < 4.0f) {
      b.dx = b.tx, b.dy = b.ty, b.vx = b.vy = 0.0f;
    } else {
      moving = true;
    }
  }
  return moving;
}

void ProjectPatchView::applyPush() {
  auto& nudges = ctx_.store().view().patchNudges;
  bool changed = false;
  for (const Push::Body& b : push_.bodies) {
    if (!b.movable) continue;
    doc::PatchNudge n = b.base;
    n.dx = std::round(b.base.dx + b.dx);
    n.dy = std::round(b.base.dy + b.dy);
    auto it = nudges.find(b.device.value);
    const doc::PatchNudge now = it != nudges.end() ? it->second : doc::PatchNudge{};
    if (now == n) continue;
    changed = true;
    if (n.empty()) nudges.erase(b.device.value);
    else nudges[b.device.value] = n;
  }
  if (!changed) return;
  ctx_.viewStateChanged();
  relayout();
}

void ProjectPatchView::finishPushNow() {
  for (Push::Body& b : push_.bodies) b.dx = b.tx, b.dy = b.ty;
  applyPush();
  push_.bodies.clear();
}

// ---- inserting on a cable ------------------------------------------------------------------------------------------

namespace {
// Where in its track's list a chain cable's insert goes: before the block it enters, else after the block it leaves.
uint32_t chainIndexOf(const ProjectPatch& pp, const doc::Project& p, const Edge& e) {
  const Node& a = pp.nodes[e.from];
  const Node& b = pp.nodes[e.to];
  const doc::Track* t = a.band < pp.bands.size() ? doc::findTrackPtr(p, pp.bands[a.band].track) : nullptr;
  if (t == nullptr) return doc::kAppend;
  if (b.kind == NodeKind::Device)
    if (const int32_t i = t->deviceIndex(b.device); i >= 0) return static_cast<uint32_t>(i);
  if (a.kind == NodeKind::Device)
    if (const int32_t i = t->deviceIndex(a.device); i >= 0) return static_cast<uint32_t>(i) + 1;
  return doc::kAppend;
}
}  // namespace

bool ProjectPatchView::insertOnEdge(uint32_t edge, const NodeChoice& c) { return insertImpl(edge, c, false); }

bool ProjectPatchView::insertImpl(uint32_t edge, const NodeChoice& c, bool vca) {
  sync();
  if (edge >= patch_.edges.size()) return false;
  const Edge e = patch_.edges[edge];
  const doc::Snapshot start = ctx_.doc();
  const doc::Project& p = *start;
  const std::string& name = c.name;
  const std::string label = "Insert " + name;
  Composer k(start, ctx_.store().ids());
  doc::DeviceId id;
  graph::Point2 mid{0.0f, 0.0f};
  if (!e.points.empty()) {
    const graph::Point2& a = e.points.front();
    const graph::Point2& b = e.points.back();
    mid = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
  }
  switch (e.kind) {
    case EdgeKind::Notes:
      ctx_.setStatus("Insert: the note wire is the chain's own (insert after the instrument)");
      return false;
    case EdgeKind::Route:
    case EdgeKind::Send:
      ctx_.setStatus("Insert: a route or a send is a bus into a mix (insert on a chain or a wire)");
      return false;
    case EdgeKind::Chain: {
      if (choiceIsInstrument(ctx_, c)) {
        ctx_.setStatus("Insert: an instrument goes first in its chain");
        return false;
      }
      const doc::TrackId track = patch_.bands[patch_.nodes[e.from].band].track;
      doc::ops::EditResult add = addChoice(ctx_, p, k.ids(), c, track, chainIndexOf(patch_, p, e), vca);
      if (!add || add->devices.empty()) {
        ctx_.setStatus(label + ": " + (add ? std::string("nothing was added") : add.error().message));
        return false;
      }
      id = add->devices.front();
      if (!k.add(add)) {
        ctx_.setStatus(label + ": " + k.why);
        return false;
      }
      if (!dispatchSteps(label.c_str(), std::move(k.steps))) return false;
      return true;
    }
    case EdgeKind::Modulation: {
      const doc::Wire* w = doc::findWire(p, e.wire);
      if (w == nullptr) return false;
      const doc::Wire old = *w;
      const doc::DeviceLoc target = doc::findDevice(p, old.to);
      if (target.track == nullptr || target.chain != nullptr) {
        ctx_.setStatus(label + ": the wire's target is inside a rack (insert in the rack's chain)");
        return false;
      }
      if (choiceIsInstrument(ctx_, c) || c.item == circuit::NodeItem::Rack) {
        ctx_.setStatus(label + ": an instrument or a rack goes into a chain, not onto a wire");
        return false;
      }
      // Into the target's group when it is inside one (or is the group's Out).
      doc::DeviceId group = target.device->group;
      if (target.device->patch == doc::kGroupDevice && old.param == doc::kGroupOutPort) group = old.to;
      const doc::WireId gone[1] = {old.id};
      if (!k.add(doc::ops::deleteWires(k.doc(), gone))) break;
      doc::ops::EditResult add = addChoice(ctx_, k.doc(), k.ids(), c, target.track->id, doc::kAppend, vca);
      if (!add || add->devices.empty()) {
        ctx_.setStatus(label + ": " + (add ? std::string("nothing was added") : add.error().message));
        return false;
      }
      id = add->devices.front();
      if (!k.add(add)) break;
      const doc::DeviceId one[1] = {id};
      if (!k.add(doc::ops::setDevicesFree(k.doc(), one, true))) break;
      if (group.valid() && !k.add(doc::SetDeviceGroup{id, group})) break;
      const circuit::MainPorts mp = mainPortsOf(ctx_.deviceLibrary(), *doc::findDevice(k.doc(), id).device);
      if (!mp.in.empty() && !wire(k, old.from, old.noteSource, old.port, id, mp.in)) break;
      if (!mp.out.empty()) {
        if (!wire(k, id, {}, mp.out, old.to, old.param)) break;
        if (old.reducer != doc::WireReducer::Sum) {
          const doc::Wire& nw = doc::wiresOf(k.doc()).back();
          if (!k.add(doc::ops::setWireReducer(k.doc(), nw.id, old.reducer))) break;
        }
      }
      if (!dispatchSteps(label.c_str(), std::move(k.steps))) return false;
      if (mp.in.empty() || mp.out.empty())
        ctx_.setStatus(name + (mp.in.empty() ? " has no input: only its output is wired" : " has no output: only its input is wired"));
      placeAt(id, mid);
      startPush(id);
      return true;
    }
  }
  ctx_.setStatus(label + ": " + k.why);
  return false;
}

bool ProjectPatchView::insertVca(uint32_t edge) {
  NodeChoice c;
  c.item = circuit::NodeItem::Meta;
  c.id = "vca";
  c.name = "VCA";
  return insertImpl(edge, c, true);  // plain: no default preset (exactly x1)
}

// ---- splice, dissolve -------------------------------------------------------------------------------------------------

bool ProjectPatchView::spliceIntoEdge(doc::DeviceId device, uint32_t edge) {
  sync();
  if (edge >= patch_.edges.size()) return false;
  const Edge e = patch_.edges[edge];
  const doc::Snapshot start = ctx_.doc();
  const doc::Project& p = *start;
  const doc::DeviceLoc loc = doc::findDevice(p, device);
  if (loc.device == nullptr) return false;
  const std::string name = deviceDisplayName(ctx_, *loc.device);
  const circuit::MainPorts mp = mainPortsOf(ctx_.deviceLibrary(), *loc.device);
  if (mp.in.empty() || mp.out.empty()) {
    ctx_.setStatus("Splice: " + name + " needs an input and an output");
    return false;
  }
  const uint32_t self = patch_.nodeOf(device);
  if (e.from == self || e.to == self) {
    ctx_.setStatus("Splice: that cable is the block's own");
    return false;
  }
  Composer k(start, ctx_.store().ids());
  const doc::DeviceId one[1] = {device};
  switch (e.kind) {
    case EdgeKind::Chain: {
      const doc::TrackId track = patch_.bands[patch_.nodes[e.from].band].track;
      if (loc.device->kind == doc::DeviceKind::Instrument) {
        ctx_.setStatus("Splice: an instrument goes first in its chain");
        return false;
      }
      if (loc.device->group.valid() && !k.add(doc::SetDeviceGroup{device, {}})) break;
      if (loc.device->free && !k.add(doc::ops::setDevicesFree(k.doc(), one, false))) break;
      uint32_t index = chainIndexOf(patch_, k.doc(), e);
      // Moving within its own list: the index after its removal.
      if (const doc::Track* t = doc::findTrackPtr(k.doc(), track); t != nullptr && loc.track != nullptr && loc.track->id == track)
        if (const int32_t at = t->deviceIndex(device); at >= 0 && index != doc::kAppend && static_cast<uint32_t>(at) < index) --index;
      if (!k.add(doc::ops::moveDevice(k.doc(), device, track, index))) break;
      return dispatchSteps("Splice", std::move(k.steps));
    }
    case EdgeKind::Modulation: {
      const doc::Wire* w = doc::findWire(p, e.wire);
      if (w == nullptr) return false;
      const doc::Wire old = *w;
      if (old.from == device || old.to == device) {
        ctx_.setStatus("Splice: that cable is the block's own");
        return false;
      }
      const doc::WireId gone[1] = {old.id};
      if (!k.add(doc::ops::deleteWires(k.doc(), gone))) break;
      if (!loc.device->free && !k.add(doc::ops::setDevicesFree(k.doc(), one, true))) break;
      if (!wire(k, old.from, old.noteSource, old.port, device, mp.in)) break;
      if (!wire(k, device, {}, mp.out, old.to, old.param)) break;
      return dispatchSteps("Splice", std::move(k.steps));
    }
    default:
      ctx_.setStatus("Splice: notes, routes and sends are not cut (splice into a chain or a wire)");
      return false;
  }
  ctx_.setStatus("Splice: " + k.why);
  return false;
}

bool ProjectPatchView::dissolve(doc::DeviceId device) {
  const doc::Snapshot start = ctx_.doc();
  const doc::Project& p = *start;
  const doc::DeviceLoc loc = doc::findDevice(p, device);
  if (loc.device == nullptr || loc.track == nullptr) return false;
  const std::string name = deviceDisplayName(ctx_, *loc.device);
  const circuit::MainPorts mp = mainPortsOf(ctx_.deviceLibrary(), *loc.device);
  if (mp.in.empty() || mp.out.empty()) {
    ctx_.setStatus("Dissolve: " + name + " needs an input and an output (Delete removes it)");
    return false;
  }
  struct Source {
    doc::DeviceId from;
    doc::TrackId notes;
    std::string port;
  };
  struct Target {
    doc::DeviceId to;
    std::string param;
    doc::WireReducer reducer;
  };
  std::vector<Source> sources;
  std::vector<Target> targets;
  for (const doc::Wire& w : doc::wiresOf(p)) {
    if (w.to == device && w.param == mp.in && w.from != device) sources.push_back({w.from, w.noteSource, w.port});
    if (w.from == device && !w.fromNoteSource() && w.port == mp.out && w.to != device) targets.push_back({w.to, w.param, w.reducer});
  }
  // A device of the series: what fed it is the device before it in the chain (its main output); the chain itself
  // closes up when it goes.
  if (!loc.device->free && loc.chain == nullptr && !targets.empty()) {
    const std::vector<doc::DevicePtr>& list = loc.track->devices;
    for (uint32_t i = loc.index; i-- > 0;) {
      const doc::Device& prev = *list[i];
      if (prev.free || !prev.active) continue;
      const circuit::MainPorts pm = mainPortsOf(ctx_.deviceLibrary(), prev);
      if (!pm.out.empty()) sources.push_back({prev.id, {}, pm.out});
      break;
    }
  }
  Composer k(start, ctx_.store().ids());
  const doc::DeviceId one[1] = {device};
  if (!k.add(doc::ops::deleteDevices(k.doc(), one))) {
    ctx_.setStatus("Dissolve: " + k.why);
    return false;
  }
  for (const Source& s : sources)
    for (const Target& t : targets) {
      if (!wire(k, s.from, s.notes, s.port, t.to, t.param)) {
        ctx_.setStatus("Dissolve: " + k.why);
        return false;
      }
      if (t.reducer != doc::WireReducer::Sum)
        if (!k.add(doc::ops::setWireReducer(k.doc(), doc::wiresOf(k.doc()).back().id, t.reducer))) return false;
    }
  return dispatchSteps("Dissolve", std::move(k.steps));
}

// ---- the menus -------------------------------------------------------------------------------------------------------

circuit::NodeMenu* ProjectPatchView::openSpawnMenu(Vec2 at, const SpawnLink& link) {
  circuit::NodeMenuOptions o;
  o.catalog = circuit::nodeCatalog(circuit::NodeCatalog::Scope::Project, ctx_);
  o.filter = link.kind == SpawnLink::Kind::FromOutput ? circuit::NodeFilter::WithInput
             : link.kind == SpawnLink::Kind::IntoParam ? circuit::NodeFilter::WithOutput
                                                       : circuit::NodeFilter::None;
  o.title = link.kind == SpawnLink::Kind::FromOutput ? "Connect from" : link.kind == SpawnLink::Kind::IntoParam ? "Drive " + link.port : "Add";
  if (link.kind == SpawnLink::Kind::FromOutput) o.title += " " + link.port;
  const graph::Point2 world = canvas_->toWorld(at);
  std::weak_ptr<int> alive = alive_;
  o.onPick = [this, alive, world, link](const circuit::NodeEntry& e) {
    if (!alive.expired()) (void)spawnNode(choiceOf(e), world, link);
  };
  o.onSpec = [this, alive, world, link](const std::string& s) {
    if (!alive.expired()) (void)spawnNode(specChoice(s), world, link);
  };
  // TX.node-menu-fix1: a cable dropped here hangs from its port to the menu until it closes; the spawned node's cable
  // then takes its motion over (the drop runs before the pick, the cable goes at the next frame).
  o.onClosed = [this, alive] {
    if (!alive.expired()) canvas_->dropLooseCable();
  };
  return circuit::openNodeMenu(*canvas_, at, std::move(o));
}

circuit::NodeMenu* ProjectPatchView::openInsertMenu(uint32_t edge, Vec2 at) {
  if (edge >= patch_.edges.size()) return nullptr;
  const Edge e = patch_.edges[edge];
  if (e.kind == EdgeKind::Notes || e.kind == EdgeKind::Route || e.kind == EdgeKind::Send) {
    ctx_.setStatus("Insert: notes, routes and sends are not cut (Tab over a chain cable or a wire)");
    return nullptr;
  }
  // The cable by what it joins (indices change with every rebuild while the menu is open).
  const doc::WireId wireId = e.wire;
  const doc::DeviceId fromDev = patch_.nodes[e.from].device, toDev = patch_.nodes[e.to].device;
  const NodeKind fromKind = patch_.nodes[e.from].kind, toKind = patch_.nodes[e.to].kind;
  const uint32_t band = patch_.nodes[e.from].band;
  const doc::TrackId track = band < patch_.bands.size() ? patch_.bands[band].track : doc::TrackId{};
  const EdgeKind kind = e.kind;
  std::weak_ptr<int> alive = alive_;
  auto resolve = [this, wireId, fromDev, toDev, fromKind, toKind, track, kind]() -> uint32_t {
    sync();
    for (uint32_t i = 0; i < patch_.edges.size(); ++i) {
      const Edge& x = patch_.edges[i];
      if (x.kind != kind) continue;
      if (kind == EdgeKind::Modulation) {
        if (x.wire == wireId) return i;
        continue;
      }
      const Node& a = patch_.nodes[x.from];
      const Node& b = patch_.nodes[x.to];
      if (a.kind == fromKind && b.kind == toKind && a.device == fromDev && b.device == toDev && a.band < patch_.bands.size() &&
          patch_.bands[a.band].track == track)
        return i;
    }
    return kNone;
  };
  circuit::NodeMenuOptions o;
  o.catalog = circuit::nodeCatalog(circuit::NodeCatalog::Scope::Project, ctx_);
  o.filter = circuit::NodeFilter::WithBoth;
  o.title = "Insert";
  o.onPick = [this, alive, resolve](const circuit::NodeEntry& en) {
    if (alive.expired()) return;
    if (const uint32_t i = resolve(); i != kNone) (void)insertOnEdge(i, choiceOf(en));
  };
  o.onSpec = [this, alive, resolve](const std::string& s) {
    if (alive.expired()) return;
    if (const uint32_t i = resolve(); i != kNone) (void)insertOnEdge(i, specChoice(s));
  };
  return circuit::openNodeMenu(*canvas_, at, std::move(o));
}

}  // namespace pq::views::project_patch
