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

// Wires `from`.`port` (or track `notes`'s source: kMainPort its own signal, else a note output) → `to`.`param` in the
// composition; TX.all-free: two main ports by name are written as kMainPort (doc::canonicalWire).
bool wire(Composer& k, const doc::DeviceLibrary* lib, doc::DeviceId from, doc::TrackId notes, const std::string& port,
          doc::DeviceId to, const std::string& param) {
  doc::Wire w;
  w.from = notes.valid() ? doc::DeviceId{} : from;
  w.noteSource = notes;
  w.port = port;
  w.to = to;
  w.param = param;
  return k.add(doc::ops::connect(k.doc(), k.ids(), doc::canonicalWire(k.doc(), std::move(w), lib)));
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
  return d != nullptr ? doc::mainPortsOf(*d, ctx_.deviceLibrary()) : circuit::MainPorts{};
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
  // TX.all-free (SPEC §15 2026-11-03 "everything is free"): every node spawns as a free block at the pointer, unwired
  // (doc::ops::kOffPath) but for the cable it was dropped from; into the capsule under it, in a group panel into the
  // group. (A stock instrument plays once the notes reach it: a cable from the track's notes into its main input.)
  if (choiceIsInstrument(ctx_, c) && group.valid()) {
    ctx_.setStatus("Add: " + c.name + " takes the notes: place it on the band, not inside a group");
    return {};
  }
  Composer k(start, ctx_.store().ids());
  doc::ops::EditResult add = addChoice(ctx_, p, k.ids(), c, track, doc::ops::kOffPath, false);
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
  if (made != nullptr && group.valid() && !made->isRack() && !k.add(doc::SetDeviceGroup{id, group})) {
    ctx_.setStatus("Add " + c.name + ": " + k.why);
    return {};
  }
  // Wired from the cable it was dropped from: into its main input, or its main output into the param (TX.all-free: by
  // kMainPort, so a cable from a main output joins the paths the way it was drawn).
  std::string note, inAt, outAt;
  const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
  if (link.kind != SpawnLink::Kind::None && made != nullptr) {
    const circuit::MainPorts mp = doc::mainPortsOf(*doc::findDevice(k.doc(), id).device, lib);
    const bool takesNotes = made->kind == doc::DeviceKind::Instrument && !doc::isMetaDevice(*made);
    if (link.kind == SpawnLink::Kind::FromOutput) {
      if (mp.in.empty() && !takesNotes && !made->isRack()) note = c.name + " has no input: placed unwired";
      else if (!wire(k, lib, link.device, link.notes, link.port, id, std::string(doc::kMainPort))) note = "Wire: " + k.why;
      else inAt = mp.in.empty() ? std::string("notes") : mp.in;
    } else {
      if (mp.out.empty()) note = c.name + " has no output: placed unwired";
      else if (!wire(k, lib, id, {}, std::string(doc::kMainPort), link.device, link.port)) note = "Wire: " + k.why;
      else outAt = mp.out;
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
  placeAt(id, at, inAt, outAt);
  startPush(id);
  return id;
}

void ProjectPatchView::placeAt(doc::DeviceId device, graph::Point2 at, std::string_view inPort, std::string_view outPort) {
  sync();
  const uint32_t n = patch_.nodeOf(device);
  if (n == kNone) return;
  const Node& nd = patch_.nodes[n];
  float dx = at.x - (nd.x + nd.w * 0.5f), dy = at.y - (nd.y + nd.h * 0.5f);
  // TX.node-menu-fix1: a node spawned from a dropped cable meets it where it was dropped — its input pin there (a cable
  // from an output), or its output pin (a backwards cable into a param); an anchored input (a face's control) or an
  // unknown port: its centre.
  if (const int32_t k = inPort.empty() ? -1 : nd.input(inPort); k >= 0 && !nd.inputs[static_cast<size_t>(k)].anchored) {
    const graph::Point2 pin = inputEntry(nd, static_cast<size_t>(k));
    dx = at.x - pin.x, dy = at.y - pin.y;
  } else if (const int32_t o = outPort.empty() ? -1 : nd.output(outPort); o >= 0) {
    dx = at.x - (nd.x + nd.w), dy = at.y - (nd.y + pinOffset(nd, static_cast<size_t>(o)));
  }
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
  // What it may meet: the blocks of its band at its level (inside the same capsule, or outside any): blocks move (TX.all-
  // free: every block is free), the source, the mixer, capsules and opened blocks stand.
  for (uint32_t i = 0; i < patch_.nodes.size(); ++i) {
    if (i == pn) continue;
    const Node& n = patch_.nodes[i];
    if (n.band != p.band || n.capsule != p.capsule) continue;
    if (n.kind == NodeKind::Terminal || (n.group && n.groupOpen && i == p.capsule)) continue;
    const bool movable = n.kind == NodeKind::Device && !(n.group && n.groupOpen) && !n.expanded;
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

bool ProjectPatchView::insertOnEdge(uint32_t edge, const NodeChoice& c) { return insertImpl(edge, c, false); }

bool ProjectPatchView::insertImpl(uint32_t edge, const NodeChoice& c, bool vca) {
  sync();
  if (edge >= patch_.edges.size()) return false;
  const Edge e = patch_.edges[edge];
  const doc::Snapshot start = ctx_.doc();
  const doc::Project& p = *start;
  const std::string& name = c.name;
  const std::string label = "Insert " + name;
  graph::Point2 mid{0.0f, 0.0f};
  if (!e.points.empty()) {
    const graph::Point2& a = e.points.front();
    const graph::Point2& b = e.points.back();
    mid = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
  }
  // TX.all-free: every cable between blocks is a document wire (the main path's links too); routes and sends are buses.
  const doc::Wire* w = e.wire.valid() ? doc::findWire(p, e.wire) : nullptr;
  if (w == nullptr || e.kind == EdgeKind::Route || e.kind == EdgeKind::Send) {
    ctx_.setStatus("Insert: a route or a send is a bus into a mix (insert on a cable between blocks)");
    return false;
  }
  const doc::Wire old = *w;
  if (choiceIsInstrument(ctx_, c) && !(old.fromNoteSource() && old.port.empty())) {
    ctx_.setStatus(label + ": an instrument takes the notes (insert it on the cable from the track's notes)");
    return false;
  }
  // The block's track and group: the wire's target's (into a mixer: that track; into a group's Out: that group).
  doc::TrackId track;
  doc::DeviceId group;
  if (old.toMixer()) {
    track = old.mixer;
  } else {
    const doc::DeviceLoc target = doc::findDevice(p, old.to);
    if (target.track == nullptr || target.chain != nullptr) {
      ctx_.setStatus(label + ": the wire's target is inside a rack (insert in the rack's chain)");
      return false;
    }
    track = target.track->id;
    group = target.device->group;
    if (target.device->patch == doc::kGroupDevice && old.param == doc::kGroupOutPort) group = old.to;
  }
  Composer k(start, ctx_.store().ids());
  doc::ops::EditResult add = addChoice(ctx_, p, k.ids(), c, track, doc::ops::kOffPath, vca);
  if (!add || add->devices.empty()) {
    ctx_.setStatus(label + ": " + (add ? std::string("nothing was added") : add.error().message));
    return false;
  }
  const doc::DeviceId id = add->devices.front();
  if (!k.add(add)) {
    ctx_.setStatus(label + ": " + k.why);
    return false;
  }
  if (group.valid() && !k.add(doc::SetDeviceGroup{id, group})) {
    ctx_.setStatus(label + ": " + k.why);
    return false;
  }
  // The cable becomes its source → the block's main input, the block's main output → its target (a link of the main
  // path stays a path: the block is on it). A block without an input or an output connects only what it has.
  const doc::Device* made = doc::findDevice(k.doc(), id).device;
  const circuit::MainPorts mp = doc::mainPortsOf(*made, ctx_.deviceLibrary());
  const bool hasIn = !mp.in.empty() || made->kind == doc::DeviceKind::Instrument || made->isRack();
  const bool hasOut = !mp.out.empty();
  if (hasIn && hasOut) {
    if (!k.add(doc::ops::spliceIntoWire(k.doc(), k.ids(), ctx_.deviceLibrary(), id, old.id))) {
      ctx_.setStatus(label + ": " + k.why);
      return false;
    }
  } else {
    const doc::WireId gone[1] = {old.id};
    if (!k.add(doc::ops::deleteWires(k.doc(), gone))) {
      ctx_.setStatus(label + ": " + k.why);
      return false;
    }
    if (hasIn && !wire(k, ctx_.deviceLibrary(), old.from, old.noteSource, old.port, id, std::string(doc::kMainPort))) {
      ctx_.setStatus(label + ": " + k.why);
      return false;
    }
    if (hasOut) {
      doc::Wire nw;
      nw.from = id;
      nw.to = old.to;
      nw.mixer = old.mixer;
      nw.param = old.param;
      nw.reducer = old.reducer;
      if (!k.add(doc::ops::connect(k.doc(), k.ids(), nw))) {
        ctx_.setStatus(label + ": " + k.why);
        return false;
      }
    }
  }
  if (!dispatchSteps(label.c_str(), std::move(k.steps))) return false;
  if (!hasIn || !hasOut) ctx_.setStatus(name + (!hasIn ? " has no input: only its output is wired" : " has no output: only its input is wired"));
  placeAt(id, mid);
  startPush(id);
  return true;
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
  const doc::Project& p = *ctx_.doc();
  const doc::DeviceLoc loc = doc::findDevice(p, device);
  if (loc.device == nullptr) return false;
  const std::string name = deviceDisplayName(ctx_, *loc.device);
  const circuit::MainPorts mp = doc::mainPortsOf(*loc.device, ctx_.deviceLibrary());
  const bool takesNotes = loc.device->kind == doc::DeviceKind::Instrument;
  if ((mp.in.empty() && !takesNotes && !loc.device->isRack()) || mp.out.empty()) {
    ctx_.setStatus("Splice: " + name + " needs an input and an output");
    return false;
  }
  const uint32_t self = patch_.nodeOf(device);
  if (e.from == self || e.to == self) {
    ctx_.setStatus("Splice: that cable is the block's own");
    return false;
  }
  if (!e.wire.valid() || e.kind == EdgeKind::Route || e.kind == EdgeKind::Send) {
    ctx_.setStatus("Splice: routes and sends are buses (splice into a cable between blocks)");
    return false;
  }
  return apply(doc::ops::spliceIntoWire(p, ctx_.store().ids(), ctx_.deviceLibrary(), device, e.wire), "Splice");
}

bool ProjectPatchView::dissolve(doc::DeviceId device) {
  const doc::Project& p = *ctx_.doc();
  const doc::DeviceLoc loc = doc::findDevice(p, device);
  if (loc.device == nullptr || loc.track == nullptr) return false;
  // TX.all-free: doc::ops::dissolveDevices — it goes, what fed its main input is wired to what its main output fed (the
  // main path through it stays connected). One undo step.
  return apply(doc::ops::dissolveDevices(p, ctx_.store().ids(), ctx_.deviceLibrary(), std::span<const doc::DeviceId>(&device, 1)),
               "Dissolve");
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
    if (alive.expired()) return;
    canvas_->dropLooseCable();
    spawnMark_.reset();
    canvas_->markDirty();
  };
  circuit::NodeMenu* m = circuit::openNodeMenu(*canvas_, at, std::move(o));
  if (m != nullptr) {
    spawnMark_ = world;
    canvas_->markDirty();
  }
  return m;
}

circuit::NodeMenu* ProjectPatchView::openInsertMenu(uint32_t edge, Vec2 at) {
  if (edge >= patch_.edges.size()) return nullptr;
  const Edge e = patch_.edges[edge];
  if (!e.wire.valid() || e.kind == EdgeKind::Route || e.kind == EdgeKind::Send) {
    ctx_.setStatus("Insert: routes and sends are buses (Tab over a cable between blocks)");
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
  // TX.node-menu-fix1: the cable it goes into is shown selected while the menu is open (the selection comes back when
  // it closes, before a choice rebuilds the scene); the point on it marked.
  const circuit::CircuitCanvas::Selection keep = canvas_->selection();
  for (uint32_t w = 0; w < scene_.modelEdge.size(); ++w)
    if (scene_.modelEdge[w] == edge) {
      const uint32_t one[1] = {w};
      canvas_->select({}, one);
      break;
    }
  o.onClosed = [this, alive, keep] {
    if (alive.expired()) return;
    spawnMark_.reset();
    const uint32_t nn = canvas_->scene() ? static_cast<uint32_t>(canvas_->scene()->nodes.size()) : 0u;
    const uint32_t nw = canvas_->scene() ? static_cast<uint32_t>(canvas_->scene()->wires.size()) : 0u;
    std::vector<uint32_t> nodes, wires;
    for (const uint32_t n : keep.nodes)
      if (n < nn) nodes.push_back(n);
    for (const uint32_t w : keep.wires)
      if (w < nw) wires.push_back(w);
    canvas_->select(nodes, wires);
    canvas_->markDirty();
  };
  spawnMark_ = canvas_->toWorld(at);
  o.onPick = [this, alive, resolve](const circuit::NodeEntry& en) {
    if (alive.expired()) return;
    if (const uint32_t i = resolve(); i != kNone) (void)insertOnEdge(i, choiceOf(en));
  };
  o.onSpec = [this, alive, resolve](const std::string& s) {
    if (alive.expired()) return;
    if (const uint32_t i = resolve(); i != kNone) (void)insertOnEdge(i, specChoice(s));
  };
  circuit::NodeMenu* m = circuit::openNodeMenu(*canvas_, at, std::move(o));
  if (m == nullptr) {  // no overlay: nothing to show it in
    spawnMark_.reset();
    canvas_->select(keep.nodes, keep.wires);
  }
  canvas_->markDirty();
  return m;
}

}  // namespace pq::views::project_patch
