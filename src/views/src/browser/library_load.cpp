// T5.11: library items into the project and back (pq/views/browser/library_load.h).

#include "pq/views/browser/library_load.h"

#include <algorithm>
#include <filesystem>
#include <functional>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/document/modules.h"
#include "pq/meta/registry.h"
#include "pq/library/index.h"
#include "pq/tuning/scala.h"
#include "pq/views/browser/library.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/mos_wheel/mos_wheel.h"

namespace pq::views {

namespace fs = std::filesystem;

namespace {

fs::path pathOf(std::string_view utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8Of(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}
std::string stemOf(std::string_view path) { return utf8Of(pathOf(path).stem()); }

bool endsWithFolded(std::string_view s, std::string_view ext) {
  if (s.size() < ext.size()) return false;
  for (size_t i = 0; i < ext.size(); ++i)
    if (library::foldChar(s[s.size() - ext.size() + i]) != library::foldChar(ext[i])) return false;
  return true;
}

Status dispatchEdit(ViewContext& ctx, const doc::ops::EditResult& e) {
  if (!e) return e.error();
  if (e->empty()) return Ok{};
  return ctx.store().dispatch(e->command);
}

// The new device's id in an edit that added one device (pasteDevices / addDevice / replaceDevice).
Result<doc::DeviceId> firstDevice(ViewContext& ctx, const doc::ops::EditResult& e) {
  if (!e) return e.error();
  if (Status s = ctx.store().dispatch(e->command); !s) return std::move(s).error();
  if (e->devices.empty()) return Error{ErrorCode::Unknown, "no device was added", {}};
  return e->devices.front();
}

// The device a preset file holds (its own ids: cloned on load).
Result<doc::DevicePtr> deviceOfFile(std::string_view path) {
  auto text = doc::readTextFile(path);
  if (!text) return std::move(text).error();
  auto f = doc::parsePreset(*text, path);
  if (!f) return std::move(f).error();
  return std::move(f->device);
}

}  // namespace

Result<doc::DevicePtr> libraryDeviceOf(std::string_view path) { return deviceOfFile(path); }

Result<doc::BlockSet> libraryBlocksOf(std::string_view path) {
  if (libraryItemOf(path) != LibraryItem::Preset) return doc::BlockSet{};
  auto text = doc::readTextFile(path);
  if (!text) return std::move(text).error();
  auto f = doc::parsePreset(*text, path);
  if (!f) return std::move(f).error();
  if (!f->groupPreset()) return doc::BlockSet{};
  return std::move(f->blocks);
}

std::string catalogItemPath(std::string_view patch) { return std::string(kCatalogScheme) + std::string(patch); }

std::string_view catalogPatchOf(std::string_view path) noexcept {
  return path.rfind(kCatalogScheme, 0) == 0 ? path.substr(kCatalogScheme.size()) : std::string_view();
}

LibraryItem libraryItemOf(std::string_view path) noexcept {
  if (!catalogPatchOf(path).empty()) return LibraryItem::CatalogDevice;
  if (endsWithFolded(path, doc::kPresetExtension)) return LibraryItem::Preset;
  if (endsWithFolded(path, doc::kTuningExtension) || endsWithFolded(path, ".scl")) return LibraryItem::Tuning;
  if (endsWithFolded(path, doc::kClipExtension)) return LibraryItem::Clip;
  return LibraryItem::None;
}

std::vector<const meta::Type*> metaModuleTypes(bool project) {
  std::vector<const meta::Type*> out;
  for (const meta::Type* t : meta::types())
    if (hasFace(*t) && !(project && doc::moduleOfEngine(t->id) != nullptr)) out.push_back(t);  // TMM.10-fix1: every type is a chain device anywhere
  return out;
}

std::string deviceDisplayName(const ViewContext& ctx, std::string_view patch) {
  if (patch == doc::kAudioEffectRack) return "Audio Effect Rack";  // GU.9: a rack (a group preset made in code)
  if (patch.starts_with(doc::kModuleItemPrefix))  // GU.4: a built-in module
    if (const doc::ModuleInfo* m = doc::findModule(patch.substr(doc::kModuleItemPrefix.size()))) return std::string(m->name);
  if (patch.starts_with(doc::kMetaDevicePrefix))  // TMM.10: a chain meta device: its type's name
    if (const meta::Type* t = meta::findType(patch.substr(doc::kMetaDevicePrefix.size()))) return std::string(t->name);
  if (patch == doc::kInstrumentRack) return "Instrument Rack";
  (void)ctx;
  return std::string(patch);
}

std::string deviceDisplayName(const ViewContext& ctx, const doc::Device& d) { return deviceDisplayName(ctx, d.patch); }

std::vector<std::string> deviceParamNames(const ViewContext& ctx, const doc::Device& d) {
  std::vector<std::string> out;
  const doc::DeviceLibrary* lib = ctx.deviceLibrary();
  if (lib == nullptr) return out;
  if (const doc::DeviceInfo* info = lib->infoOf(d))
    for (const doc::DeviceParamInfo& p : info->params) out.push_back(p.name);
  return out;
}

namespace {
// GU.9: `add` on the track off its path, then into rack chain `at.chain`: one undo step.
Result<doc::DeviceId> intoChain(ViewContext& ctx, DeviceSlot at, const std::function<Result<doc::DeviceId>(DeviceSlot)>& add) {
  doc::DocumentStore& store = ctx.store();
  const bool own = !store.inGesture() && store.beginGesture("Add Device").ok();
  Result<doc::DeviceId> id = add(DeviceSlot{at.track, doc::ops::kOffPath, {}});
  if (id) {
    const doc::ops::EditResult mv = doc::ops::moveIntoChain(*ctx.doc(), store.ids(), *id, at.index, at.chain);
    Status st = mv ? store.dispatch(mv->command) : Status(mv.error());
    if (!st) id = std::move(st).error();
  }
  if (own) {
    if (id) (void)store.commitGesture();
    else (void)store.cancelGesture();
  }
  return id;
}
}  // namespace

Result<doc::DeviceId> addCatalogDevice(ViewContext& ctx, std::string_view patch, doc::DeviceKind kind, DeviceSlot at) {
  if (at.chain.valid())
    return intoChain(ctx, at, [&](DeviceSlot off) { return addCatalogDevice(ctx, patch, kind, off); });
  const doc::Project& p = *ctx.doc();
  if (patch.starts_with(doc::kModuleItemPrefix)) {  // GU.4: a built-in module: its group preset
    const doc::ModuleInfo* m = doc::findModule(patch.substr(doc::kModuleItemPrefix.size()));
    if (m == nullptr) return Error{ErrorCode::NotFound, "unknown module '" + std::string(patch) + "'", {}};
    if (m->instrument) {  // GU.8: an instrument module takes the notes of a MIDI track with an empty path
      // (GU.11: as an instrument did — only on a MIDI track: no notes reach an audio track's path)
      if (const doc::Track* t = doc::findTrackPtr(p, at.track); t == nullptr || t->kind != doc::TrackKind::Midi)
        return Error{ErrorCode::InvalidArgument, "instruments go on MIDI tracks", {}};
      return firstDevice(ctx, doc::ops::pasteBlocksAtNotes(p, ctx.store().ids(), at.track, doc::moduleBlocks(m->kind)));
    }
    // GU.11: an effect module goes onto the path at the slot (its group: one block of the chain).
    return firstDevice(ctx, doc::ops::addDevice(p, ctx.store().ids(), at.track, std::string(patch), doc::DeviceKind::AudioEffect, at.index));
  }
  if (patch.starts_with(doc::kMetaDevicePrefix)) {  // TMM.10: a chain meta device, its kind from the slot
    const meta::Type* t = meta::findType(patch.substr(doc::kMetaDevicePrefix.size()));
    if (t == nullptr) return Error{ErrorCode::NotFound, "unknown meta module '" + std::string(patch) + "'", {}};
    auto k = doc::ops::metaDeviceKindAt(p, *t, at.track, at.index);
    if (!k) return std::move(k).error();
    if (doc::DevicePtr def = ctx.library().defaultPreset(deviceDisplayName(ctx, patch), patch); def && def->patch == patch) {
      auto d = std::make_shared<doc::Device>(*def);
      d->kind = *k;
      const doc::DevicePtr one = d;
      return firstDevice(ctx, doc::ops::pasteDevices(p, ctx.store().ids(), at.track, std::span<const doc::DevicePtr>(&one, 1),
                                                     at.index));
    }
    return firstDevice(ctx, doc::ops::addMetaDevice(p, ctx.store().ids(), at.track, std::string(patch.substr(doc::kMetaDevicePrefix.size())),
                                                    at.index));
  }
  if (doc::DevicePtr def = ctx.library().defaultPreset(deviceDisplayName(ctx, patch), patch); def && def->kind == kind)
    return firstDevice(ctx, doc::ops::pasteDevices(p, ctx.store().ids(), at.track, std::span<const doc::DevicePtr>(&def, 1),
                                                   at.index));
  return firstDevice(ctx, doc::ops::addDevice(p, ctx.store().ids(), at.track, std::string(patch), kind, at.index));
}

Result<doc::DeviceId> loadDeviceItem(ViewContext& ctx, std::string_view path, DeviceSlot at) {
  if (at.chain.valid()) return intoChain(ctx, at, [&](DeviceSlot off) { return loadDeviceItem(ctx, path, off); });
  if (const std::string_view patch = catalogPatchOf(path); !patch.empty()) {
    doc::DeviceKind kind = doc::DeviceKind::AudioEffect;
    if (patch == doc::kInstrumentRack) kind = doc::DeviceKind::Instrument;
    return addCatalogDevice(ctx, patch, kind, at);
  }
  // GU.2: a group preset is pasted whole (fresh ids) onto the track, off its path (outside a rack chain only).
  if (auto blocks = libraryBlocksOf(path); blocks && !blocks->empty()) {
    return firstDevice(ctx, doc::ops::pasteBlocks(*ctx.doc(), ctx.store().ids(), at.track, *blocks));
  }
  auto d = deviceOfFile(path);
  if (!d) return std::move(d).error();
  if (doc::isMetaDevice(**d))  // TMM.10: a meta device's preset takes the kind its slot gives (an instrument, a voice)
    if (const meta::Type* t = meta::findType(doc::metaDeviceType(**d))) {
      auto k = doc::ops::metaDeviceKindAt(*ctx.doc(), *t, at.track, at.index);
      if (!k) return std::move(k).error();
      auto c = std::make_shared<doc::Device>(**d);
      c->kind = *k;
      *d = std::move(c);
    }
  auto id = firstDevice(ctx, doc::ops::pasteDevices(*ctx.doc(), ctx.store().ids(), at.track,
                                                    std::span<const doc::DevicePtr>(&*d, 1), at.index));
  if (id && libraryItemOf(path) == LibraryItem::Preset) ctx.library().setPresetName(*id, stemOf(path));
  return id;
}

Status hotSwapDevice(ViewContext& ctx, doc::DeviceId target, std::string_view path) {
  doc::DevicePtr with;
  if (const std::string_view patch = catalogPatchOf(path); !patch.empty()) {
    if (patch.starts_with(doc::kModuleItemPrefix))  // GU.4: a module is a group of blocks, not one device
      return Error{ErrorCode::InvalidArgument, "a module cannot replace a device (drop it into the track instead)", {}};
    const doc::Device* cur = doc::findDevice(*ctx.doc(), target).device;
    if (cur == nullptr) return Error{ErrorCode::NotFound, "the device is gone", {}};
    with = ctx.library().defaultPreset(deviceDisplayName(ctx, patch), patch);
    if (!with && patch.starts_with(doc::kMetaDevicePrefix)) {  // TMM.10: a fresh meta device: the type's default state
      const meta::Type* t = meta::findType(patch.substr(doc::kMetaDevicePrefix.size()));
      if (t == nullptr) return Error{ErrorCode::NotFound, "unknown meta module '" + std::string(patch) + "'", {}};
      auto m = std::make_shared<doc::Device>(*doc::makeDevice(doc::DeviceId{1}, std::string(patch), cur->kind));
      m->meta = std::make_shared<const doc::MetaMap>(doc::MetaMap{doc::MetaEntry{"", doc::defaultMetaInstance(*t)}});
      with = std::move(m);
    }
    if (!with) with = doc::makeDevice(doc::DeviceId{1}, std::string(patch), cur->kind);
  } else {
    auto d = deviceOfFile(path);
    if (!d) return std::move(d).error();
    with = std::move(*d);
  }
  const std::vector<std::string> params = deviceParamNames(ctx, *with);
  Status s = dispatchEdit(ctx, doc::ops::replaceDevice(*ctx.doc(), ctx.store().ids(), target, *with, params));
  if (s) ctx.library().setPresetName(target, catalogPatchOf(path).empty() ? stemOf(path) : std::string("Default"));
  return s;
}

Result<doc::TuningFile> readTuningItem(std::string_view path, double referenceHz) {
  auto text = doc::readTextFile(path);
  if (!text) return std::move(text).error();
  if (endsWithFolded(path, ".scl")) {
    auto scl = tuning::parseScl(*text, path);
    if (!scl) return std::move(scl).error();
    auto def = doc::makeScaleDef(std::move(*scl), std::nullopt, referenceHz);
    if (!def) return std::move(def).error();
    return doc::TuningFile{stemOf(path), doc::Colour{}, std::move(*def)};
  }
  return doc::parseTuning(*text, path);
}

Result<std::string> loadTuningItem(ViewContext& ctx, std::string_view path, const TuningTarget* target) {
  const doc::Project& p = *ctx.doc();
  double ref = tuning::kDefaultReferenceHz;
  if (target != nullptr)
    if (const doc::TuningDef* cur = targetTuning(p, *target)) ref = cur->referenceHz;
  auto t = readTuningItem(path, ref);
  if (!t) return std::move(t).error();
  if (target != nullptr) {
    if (targetTuning(p, *target) == nullptr) return Error{ErrorCode::NotFound, "the tuning's owner is gone", {}};
    if (Status s = ctx.store().dispatch(tuningCommand(*target, t->def)); !s) return std::move(s).error();
    return tuningTargetLabel(*ctx.doc(), *target) + ": " + t->name;
  }
  const doc::Colour colour = t->colour;  // a .scl: the default grey (the panel's swatch recolours it)
  if (Status s = dispatchEdit(ctx, doc::ops::addMainTuning(p, ctx.store().ids(), t->name, t->def, colour)); !s)
    return std::move(s).error();
  return "Main tuning \xE2\x80\x9C" + t->name + "\xE2\x80\x9D added";
}

Result<doc::ClipRef> loadClipItem(ViewContext& ctx, std::string_view path, doc::TrackId track, Ticks start) {
  auto f = ctx.library().readClip(path);
  if (!f) return std::move(f).error();
  doc::IdSource& ids = ctx.store().ids();
  doc::Snapshot snap = ctx.doc();
  const doc::TrackKind want = f->clip.audio ? doc::TrackKind::Audio : doc::TrackKind::Midi;
  doc::Batch batch;
  batch.label = "Add Clip";
  const doc::Track* t = doc::findTrackPtr(*snap, track);
  if (t == nullptr || t->kind != want) {  // a new track of the clip's kind after the selected one
    auto nt = doc::ops::createTrack(*snap, ids, want, t != nullptr && t->kind != doc::TrackKind::Return &&
                                                          t->kind != doc::TrackKind::Master ? track : doc::TrackId{});
    if (!nt) return std::move(nt).error();
    doc::ChangeSet cs;
    auto next = doc::apply(snap, nt->command, cs);
    if (!next) return std::move(next).error();
    snap = std::move(*next);
    batch.commands.push_back(nt->command);
    track = nt->tracks.at(0);
  }
  std::optional<doc::SampleRef> newSample;
  doc::ClipSlot clip = f->clip;
  if (f->clip.audio) {
    doc::SampleRef s = *f->sample;
    // Reuse the project's entry of the same file; else a new entry (absolute path: projects reference samples, T5.8).
    const std::string key = library::foldedPathKey(s.path);
    doc::SampleId reuse;
    for (const doc::SampleResolution& r : ctx.sampleResolutions())
      if (!r.path.empty() && library::foldedPathKey(r.path) == key) reuse = r.id;
    doc::AudioClip a = *clip.audio;
    if (reuse.valid() && doc::findSample(*snap, reuse)) {
      a.sample = reuse;
    } else {
      std::error_code ec;
      if (!fs::exists(pathOf(s.path), ec)) return Error{ErrorCode::NotFound, "the clip's sample is missing: " + s.path, {}};
      s.id = ids.sample();
      newSample = s;
    }
    clip.audio = std::make_shared<const doc::AudioClip>(std::move(a));
  }
  auto placed = doc::ops::placeClip(*snap, ids, track, clip, std::max<Ticks>(0, start), newSample);
  if (!placed) return std::move(placed).error();
  batch.commands.push_back(placed->command);
  if (Status s = ctx.store().dispatch(doc::Command{std::move(batch)}); !s) return std::move(s).error();
  return placed->clips.at(0);
}

Result<std::string> saveDevicePreset(ViewContext& ctx, doc::DeviceId device, bool asDefault) {
  const doc::Device* d = doc::findDevice(*ctx.doc(), device).device;
  if (d == nullptr) return Error{ErrorCode::NotFound, "the device is gone", {}};
  const std::string name = deviceDisplayName(ctx, *d);
  Library& lib = ctx.library();
  if (asDefault) {
    return lib.saveDefaultPreset(*d, name);
  }
  auto path = lib.savePreset(*d, name);
  if (path) lib.pendingRename = *path;
  return path;
}

Result<std::string> saveTuningToLibrary(ViewContext& ctx, const TuningTarget& target) {
  const doc::Project& p = *ctx.doc();
  const doc::TuningDef* def = targetTuning(p, target);
  if (def == nullptr) return Error{ErrorCode::NotFound, "the tuning is gone", {}};
  doc::TuningFile f;
  f.def = *def;
  f.name = tuningSummary(*def);
  if (target.kind == TuningTarget::Kind::Main)
    if (const doc::TuningSlot* s = doc::findTuning(p, target.main)) {
      f.name = s->name;
      f.colour = s->colour;
    }
  auto path = ctx.library().saveTuning(f);
  if (path) ctx.library().pendingRename = *path;
  return path;
}

}  // namespace pq::views
