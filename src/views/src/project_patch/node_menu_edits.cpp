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
#include "pq/document/modules.h"
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

// The edit that adds `c`'s device to `track` at list position `index`. `plain`: no default preset. Its Edit::devices
// holds the new device first (a module: its group).
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
    case circuit::NodeItem::Device: {  // the value block
      if (!plain)
        if (doc::DevicePtr def = ctx.library().defaultPreset(deviceDisplayName(ctx, c.id), c.id); def && def->patch == c.id) return paste(def);
      return doc::ops::addDevice(p, ids, track, c.id, doc::DeviceKind::AudioEffect, index);
    }
    case circuit::NodeItem::Rack:
      return doc::ops::addDevice(p, ids, track, c.id, c.id == doc::kInstrumentRack ? doc::DeviceKind::Instrument : doc::DeviceKind::AudioEffect, index);
    case circuit::NodeItem::Module: {  // GU.4: a built-in module: its group preset, pasted (fresh ids, off the path)
      const doc::ModuleInfo* m = doc::findModule(c.id);
      if (m == nullptr) return Error{ErrorCode::NotFound, "no module '" + c.id + "'", {}};
      if (m->instrument) return doc::ops::pasteBlocksAtNotes(p, ids, track, doc::moduleBlocks(m->kind));  // GU.8
      return doc::ops::pasteBlocks(p, ids, track, doc::moduleBlocks(m->kind));
    }
    case circuit::NodeItem::LibraryFile: {
      // GU.2: a group preset spawns as a paste of its blocks (fresh ids), off the path (the view places it).
      if (auto blocks = libraryBlocksOf(c.id); blocks && !blocks->empty()) return doc::ops::pasteBlocks(p, ids, track, *blocks);
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
      // GU.2: an atom is an atom block ("atom:<name>": the atom itself, no generated text) when it can be one.
      if (doc::atomBlockPatch(c.id) != nullptr)
        return doc::ops::addDevice(p, ids, track, std::string(doc::kAtomDevicePrefix) + c.id, doc::DeviceKind::AudioEffect, index);
      [[fallthrough]];
    case circuit::NodeItem::Spec: {
      // A typed `meta_<type>` is that meta module (its face), not a device around a call.
      if (c.item == circuit::NodeItem::Spec && c.id.starts_with(meta::kCallPrefix))
        if (meta::findType(std::string_view(c.id).substr(meta::kCallPrefix.size())) != nullptr)
          return meta(std::string_view(c.id).substr(meta::kCallPrefix.size()));
      auto block = circuit::specBlockPatch(c.id);  // GU.11: one atom block (no patch text of its own)
      if (!block) return std::move(block).error();
      return doc::ops::addDevice(p, ids, track, *block, doc::DeviceKind::AudioEffect, index);
    }
  }
  return Error{ErrorCode::InvalidArgument, "nothing to place", {}};
}

bool choiceIsInstrument(ViewContext& ctx, const ProjectPatchView::NodeChoice& c) {
  (void)ctx;
  if (c.item == circuit::NodeItem::Rack) return c.id == doc::kInstrumentRack;
  if (c.item == circuit::NodeItem::LibraryFile)
    if (auto d = libraryDeviceOf(c.id); d && !doc::isMetaDevice(**d)) return (*d)->kind == doc::DeviceKind::Instrument;
  return false;
}

// Wires `from`.`port` (or track `notes`'s source: kMainPort its own signal, else a note output) → `to`.`param` in the
// composition; TX.all-free: two main ports by name are written as kMainPort (doc::canonicalWire).
bool wire(Composer& k, const doc::DeviceLibrary* lib, doc::DeviceId from, doc::TrackId notes, const std::string& port,
          doc::DeviceId to, const std::string& param, std::optional<float> tap = {}) {
  doc::Wire w;
  w.from = notes.valid() ? doc::DeviceId{} : from;
  w.noteSource = notes;
  if (notes.valid()) w.tap = tap;  // TL.1: where its cable leaves the lane
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

bool ProjectPatchView::placeTarget(graph::Point2 at, doc::TrackId& track, doc::DeviceId& group, uint32_t& band) const {
  const doc::Project& p = *ctx_.doc();
  // Where: the free rack's track, the group panel's group, else the band under the point (the nearest one) and an opened
  // capsule of it under the point (GU.2: the innermost one: nested capsules come after their parent).
  track = {};
  group = {};
  band = kNone;
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
    if (band == kNone) return false;
    track = patch_.bands[band].track;
  } else {
    band = patch_.bandOf(track);
  }
  for (const Node& n : patch_.nodes)
    if (n.group && n.groupOpen && (band == kNone || n.band == band) && at.x >= n.x && at.x <= n.x + n.w && at.y >= n.y &&
        at.y <= n.y + n.h && (!scope_.valid() || n.device == scope_ || doc::groupContains(doc::findTrackPtr(p, track)->devices, scope_, n.device)))
      group = n.device;
  return true;
}

doc::DeviceId ProjectPatchView::spawnNode(const NodeChoice& c, graph::Point2 at, const SpawnLink& link) {
  sync();
  const doc::Snapshot start = ctx_.doc();
  const doc::Project& p = *start;
  doc::TrackId track;
  doc::DeviceId group;
  uint32_t band = kNone;
  if (!placeTarget(at, track, group, band)) {
    ctx_.setStatus("Add: there is no track to place it on");
    return {};
  }
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
  if (made != nullptr && group.valid() && !k.add(doc::SetDeviceGroup{id, group})) {
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
      if (mp.in.empty() && !takesNotes) note = c.name + " has no input: placed unwired";
      else if (!wire(k, lib, link.device, link.notes, link.port, id, std::string(doc::kMainPort), link.tap)) note = "Wire: " + k.why;
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
  placeAt(id, at, inAt, outAt);  // TX.block-drag: the nudge's re-layout settles any overlap (the new block gives way most)
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
    const graph::Point2 pin = inPortPoint(nd, static_cast<size_t>(k));  // TX.orbit-ports: where its port settled
    dx = at.x - pin.x, dy = at.y - pin.y;
  } else if (const int32_t o = outPort.empty() ? -1 : nd.output(outPort); o >= 0) {
    const graph::Point2 pin = outPortPoint(nd, static_cast<size_t>(o));
    dx = at.x - pin.x, dy = at.y - pin.y;
  }
  // Never above its band's title strip (the band grows down and right to hold it, never up).
  if (nd.band < patch_.bands.size()) {
    const float top = patch_.bands[nd.band].y + metrics::kTitle;
    if (nd.y + dy < top) dy = top - nd.y;
  }
  const bool orbiting = nd.orbitIn.on || nd.orbitOut.on;
  nudge(device.value, dx, dy);
  // TX.orbit-ports: an orbiting port turns toward its cable where the block now is: a step or two more puts it on the
  // drop point (the turn shrinks with each step; whole units, as nudges are).
  for (int step = 0; orbiting && step < 3; ++step) {
    const uint32_t m = patch_.nodeOf(device);
    if (m == kNone) return;
    const Node& b = patch_.nodes[m];
    graph::Point2 pin;
    if (const int32_t k = inPort.empty() ? -1 : b.input(inPort); k >= 0 && !b.inputs[static_cast<size_t>(k)].anchored) pin = inPortPoint(b, static_cast<size_t>(k));
    else if (const int32_t o = outPort.empty() ? -1 : b.output(outPort); o >= 0) pin = outPortPoint(b, static_cast<size_t>(o));
    else return;
    const float rx = at.x - pin.x, ry = at.y - pin.y;
    if (std::fabs(rx) < 0.75f && std::fabs(ry) < 0.75f) return;
    nudge(device.value, rx, ry);
  }
}

// ---- push-apart (TX.block-drag: blocks never overlap) -----------------------------------------------------------------

namespace {
bool closer(const float* a, const float* b, float gap) noexcept {  // rects {x, y, w, h} nearer than `gap` (0: overlapping)
  return a[0] < b[0] + b[2] + gap && b[0] < a[0] + a[2] + gap && a[1] < b[1] + b[3] + gap && b[1] < a[1] + a[3] + gap;
}
}  // namespace

bool ProjectPatchView::blockGestureRunning() const noexcept {
  const ProjectCanvas::Drag d = canvas_->drag();
  return d == ProjectCanvas::Drag::MoveDevice || d == ProjectCanvas::Drag::Nudge || d == ProjectCanvas::Drag::Splice || d == ProjectCanvas::Drag::MoveMixer ||
         d == ProjectCanvas::Drag::Resize;
}

// Every level's bodies: a band's blocks outside capsules (its source, mixer and Out standing, its title strip a wall)
// and each opened capsule's members (its terminals standing). With `onlyOverlapping`, only the levels where two bodies
// (one of them movable) are closer than kPushDetect, or a movable one is above its wall.
void ProjectPatchView::collectBodies(std::vector<Push::Body>& out, bool onlyOverlapping) const {
  out.clear();
  const auto& nudges = ctx_.store().view().patchNudges;
  auto listed = [](const std::vector<uint32_t>& v, uint32_t id) { return std::find(v.begin(), v.end(), id) != v.end(); };
  for (uint32_t i = 0; i < patch_.nodes.size(); ++i) {
    const Node& n = patch_.nodes[i];
    if (n.band >= patch_.bands.size()) continue;
    const bool device = n.kind == NodeKind::Device;
    const bool standing = n.kind == NodeKind::Sum || n.kind == NodeKind::Mixer || n.kind == NodeKind::Output || n.kind == NodeKind::Terminal;
    if (!device && !standing) continue;
    if (n.folded) continue;  // TL.1: hidden on a folded band (a lane is no body)
    if (n.w <= 0.0f || n.h <= 0.0f) continue;
    Push::Body b;
    b.device = device ? n.device : doc::DeviceId{};
    b.x = n.x, b.y = n.y, b.w = n.w, b.h = n.h;
    b.level = n.band * 65536u + (n.capsule == kNone ? 65535u : std::min<uint32_t>(n.capsule, 65534u));
    // The band's title strip is a wall (a group's panel and the free rack have none: their band is only the room
    // around their blocks).
    b.minY = n.capsule == kNone && !patch_.scoped ? patch_.bands[n.band].y + metrics::kTitle : -1e9f;
    b.movable = device;
    if (device) {
      b.mass = std::max(1.0f, n.w * n.h);
      if (listed(moved_, n.device.value)) b.mass *= 0.25f;   // the one just placed gives way most
      if (listed(resized_, n.device.value)) b.mass *= 4.0f;  // a resized one holds its place (its neighbours make room)
      if (const auto it = nudges.find(n.device.value); it != nudges.end()) b.base = it->second;
    } else {
      b.mass = 0.0f;
    }
    out.push_back(b);
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const Push::Body& a, const Push::Body& b) { return a.level != b.level ? a.level < b.level : a.x < b.x; });
  if (!onlyOverlapping) return;
  // Per level (sorted by x): a sweep finds the pairs nearer than kPushDetect.
  std::vector<Push::Body> keep;
  for (size_t l0 = 0; l0 < out.size();) {
    size_t l1 = l0;
    while (l1 < out.size() && out[l1].level == out[l0].level) ++l1;
    bool hit = false;
    for (size_t i = l0; i < l1 && !hit; ++i) {
      const Push::Body& a = out[i];
      if (a.movable && a.y < a.minY - 0.5f) hit = true;
      const float ra[4] = {a.x, a.y, a.w, a.h};
      for (size_t j = i + 1; j < l1 && !hit && out[j].x < a.x + a.w + kPushDetect; ++j) {
        const Push::Body& b = out[j];
        if (!a.movable && !b.movable) continue;
        const float rb[4] = {b.x, b.y, b.w, b.h};
        hit = closer(ra, rb, kPushDetect);
      }
    }
    if (hit) keep.insert(keep.end(), out.begin() + static_cast<std::ptrdiff_t>(l0), out.begin() + static_cast<std::ptrdiff_t>(l1));
    l0 = l1;
  }
  out.swap(keep);
}

std::vector<std::pair<doc::DeviceId, doc::DeviceId>> ProjectPatchView::overlappingBlocks() const {
  std::vector<std::pair<doc::DeviceId, doc::DeviceId>> pairs;
  std::vector<Push::Body> all;
  collectBodies(all, false);
  for (size_t i = 0; i < all.size(); ++i)
    for (size_t j = i + 1; j < all.size() && all[j].level == all[i].level; ++j) {
      const Push::Body &a = all[i], &b = all[j];
      if (!a.movable && !b.movable) continue;
      const float ra[4] = {a.x, a.y, a.w, a.h}, rb[4] = {b.x, b.y, b.w, b.h};
      if (closer(ra, rb, kPushDetect)) pairs.emplace_back(a.device, b.device);
    }
  return pairs;
}

bool ProjectPatchView::settleOverlaps() {
  if (pushing() || blockGestureRunning()) return false;
  sync();
  if (pushing()) return true;  // the sync's re-layout found one
  const bool pushed = startPush();
  moved_.clear();
  resized_.clear();
  return pushed;
}

void ProjectPatchView::landPush() {
  for (uint32_t i = 0; pushing() && i <= kPushCascade; ++i) finishPushNow();
}

bool ProjectPatchView::startPush() {
  std::vector<Push::Body> bs;
  collectBodies(bs, true);
  if (bs.empty()) return false;
  // Where they end: overlapping pairs (closer than kPushGap) are separated along their axis of least overlap - shared by
  // mass between two blocks, all of it on the block against a standing body - until none overlaps (the band's title
  // strip is a wall: a pair it blocks vertically separates sideways). Each pass walks each level's pairs.
  for (Push::Body& b : bs) b.ty = b.movable ? std::max(0.0f, b.minY - b.y) : 0.0f;
  for (int iter = 0; iter < 256; ++iter) {
    bool overlap = false;
    for (size_t i = 0; i < bs.size(); ++i)
      for (size_t j = i + 1; j < bs.size() && bs[j].level == bs[i].level; ++j) {
        Push::Body& u = bs[i];
        Push::Body& v = bs[j];
        if (!u.movable && !v.movable) continue;
        const float ux = u.x + u.tx, uy = u.y + u.ty, vx = v.x + v.tx, vy = v.y + v.ty;
        const float ox = std::min(ux + u.w + kPushGap - vx, vx + v.w + kPushGap - ux);
        const float oy = std::min(uy + u.h + kPushGap - vy, vy + v.h + kPushGap - uy);
        if (ox <= 0.0f || oy <= 0.0f) continue;
        overlap = true;
        float su = 0.0f, sv = 0.0f;  // each one's share of the separation
        if (u.movable && v.movable) {
          su = v.mass / (u.mass + v.mass);
          sv = 1.0f - su;
        } else {
          su = u.movable ? 1.0f : 0.0f;
          sv = v.movable ? 1.0f : 0.0f;
        }
        const bool uFirst = (ux + u.w * 0.5f) <= (vx + v.w * 0.5f);  // u left of v (sideways) ...
        const bool uAbove = (uy + u.h * 0.5f) <= (vy + v.h * 0.5f);  // ... u above v (vertically)
        Push::Body& up = uAbove ? u : v;
        Push::Body& low = uAbove ? v : u;
        float upShare = uAbove ? su : sv, lowShare = uAbove ? sv : su;
        if (iter >= 64) {
          // Not settled sideways (blocks squeezed between standing ones): from here every overlap is resolved downwards
          // (bands grow down: always room there), which ends.
          if (low.movable) low.ty += oy + 0.5f;
          else up.ty += (low.y + low.ty + low.h + kPushGap) - (up.y + up.ty) + 0.5f;
        } else {
          bool vertical = oy < ox;
          if (vertical && up.movable && upShare > 0.0f && up.y + up.ty - (oy + 0.5f) * upShare < up.minY) {
            // The wall (the band's title strip) stops the upper one: the lower one takes it all, else sideways.
            if (low.movable) upShare = 0.0f, lowShare = 1.0f;
            else vertical = false;
          }
          if (vertical) {
            const float d = oy + 0.5f;
            up.ty -= d * upShare;
            low.ty += d * lowShare;
          } else {
            const float d = ox + 0.5f;
            u.tx += (uFirst ? -d : d) * su;
            v.tx += (uFirst ? d : -d) * sv;
          }
        }
        if (u.movable) u.ty = std::max(u.ty, u.minY - u.y);
        if (v.movable) v.ty = std::max(v.ty, v.minY - v.y);
      }
    if (!overlap) break;
  }
  // Only the blocks that move glide (the standing bodies and the blocks left where they are drop out).
  std::erase_if(bs, [](const Push::Body& b) { return !b.movable || (std::fabs(b.tx) < 0.5f && std::fabs(b.ty) < 0.5f); });
  if (bs.empty()) return false;
  push_.bodies = std::move(bs);
  push_.last = -1.0;
  push_.frames = 0;
  ++push_.count;
  push_.user = !moved_.empty() || !resized_.empty();
  if (!pushAnimated_ || !shown()) {
    finishPushNow();
    return true;
  }
  requestFrame();
  return true;
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
  if (push_.user) ctx_.viewStateChanged();
  settleHeld_ = true;
  relayout();
  settleHeld_ = false;
}

void ProjectPatchView::finishPushNow() {
  for (Push::Body& b : push_.bodies) b.dx = b.tx, b.dy = b.ty;
  applyPush();
  push_.bodies.clear();
  // A capsule or band the push grew may meet others now: the next round (bounded; only on a model laid out again — a
  // hidden view re-lays out when it is shown, and its rebuild checks then).
  if (!stale_ && cascade_ < kPushCascade) {
    ++cascade_;
    (void)startPush();
  }
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
  // TX.insert-group: the block goes where the cable is drawn — the innermost group holding both its ends (a cable from
  // outside into a module's control or its main input stays outside the module; one between two blocks of a group, or
  // into its Out terminal, is inside it).
  const doc::WireContainer place = doc::wireContainer(p, old);
  if (!place.track.valid()) {
    ctx_.setStatus(label + ": the wire's target is gone");
    return false;
  }
  const doc::TrackId track = place.track;
  const doc::DeviceId group = place.group;
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
  const bool hasIn = !mp.in.empty() || made->kind == doc::DeviceKind::Instrument;
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
    if (hasIn && !wire(k, ctx_.deviceLibrary(), old.from, old.noteSource, old.port, id, std::string(doc::kMainPort), old.tap)) {
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
  return true;
}

bool ProjectPatchView::insertVca(uint32_t edge) {
  NodeChoice c;
  c.item = circuit::NodeItem::Module;  // GU.4: the VCA module (its knob at 0.7: × 1)
  c.id = "vca";
  c.name = "VCA";
  return insertImpl(edge, c, true);
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
  if ((mp.in.empty() && !takesNotes) || mp.out.empty()) {
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
