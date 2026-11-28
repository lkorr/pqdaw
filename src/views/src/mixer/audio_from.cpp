// RT.3: Audio From — the shared chooser / menu builder (pq/views/mixer/audio_from.h).
#include "pq/views/mixer/audio_from.h"

#include <algorithm>
#include <utility>

#include "pq/document/devices.h"
#include "pq/document/main_path.h"
#include "pq/document/racks.h"
#include "pq/ui/color.h"
#include "pq/views/browser/library_load.h"
#include "pq/views/transport_port.h"
#include "pq/views/view_context.h"

namespace pq::views::audio_from {

namespace {

using doc::AudioFrom;
using doc::AudioFromKind;

std::string blockName(const ViewContext& ctx, const doc::Device& d) {
  if (!d.name.empty()) return d.name;
  if (d.patch == doc::kGroupDevice) return "Group";
  if (const doc::DeviceLibrary* lib = ctx.deviceLibrary())
    if (const doc::DeviceInfo* info = lib->infoOf(d)) return info->name;
  return deviceDisplayName(ctx, d);
}

std::string rackChainName(const doc::RackInfo& r, const doc::RackChain& c) {
  return std::string(r.instrument ? "Instrument Rack" : "Audio Effect Rack") + " | " + (c.name.empty() ? "Chain" : c.name);
}

std::string sourceName(const doc::Track& t) {
  return t.kind == doc::TrackKind::Audio ? "Clips" : t.kind == doc::TrackKind::Midi ? "Notes" : "Input";
}

doc::Wire deviceStart(doc::DeviceId d) {
  doc::Wire w;
  w.from = d;
  return w;
}

// The default output of `source` when it is picked in Audio From: its last block before the mixer (Live's Post FX),
// else its source, else its mixer (outputsOf's order: the mixer is last).
const Output* defaultOf(const std::vector<Output>& outs) {
  if (outs.empty()) return nullptr;
  return outs.size() >= 2 && outs.back().start.fromMixerOut() ? &outs[outs.size() - 2] : &outs.back();
}

doc::AudioInput defaultExt(const doc::AudioFrom& cur, uint32_t have) {
  if (cur.kind == AudioFromKind::Ext) return cur.ext;
  return have == 1 ? doc::AudioInput{0, 1} : doc::AudioInput{0, 2};
}

}  // namespace

doc::TrackId trackOf(const doc::Project& p, const doc::Wire& start) noexcept {
  if (start.fromMixerOut()) return start.mixerOut;
  if (start.fromNoteSource()) return start.noteSource;
  const doc::DeviceLoc l = doc::findDevice(p, start.from);
  return l.track != nullptr ? l.track->id : doc::TrackId{};
}

std::string outputName(const ViewContext& ctx, const doc::Wire& start) {
  const doc::Project& p = *ctx.doc();
  if (start.fromMixerOut()) return "Mixer";
  if (start.fromNoteSource()) {
    const doc::Track* t = doc::findTrackPtr(p, start.noteSource);
    std::string s = t != nullptr ? sourceName(*t) : std::string("?");
    return start.port.empty() ? s : s + " " + start.port;
  }
  const doc::DeviceLoc l = doc::findDevice(p, start.from);
  if (l.device == nullptr) return "?";
  std::string s;
  if (const std::optional<doc::RackInfo> r = doc::rackOfChain(p, *l.track, l.device->id))
    if (const doc::RackChain* c = r->findChain(l.device->id)) s = rackChainName(*r, *c);
  if (s.empty()) s = blockName(ctx, *l.device);
  return start.port.empty() ? s : s + " (" + start.port + ")";
}

std::vector<Output> outputsOf(const ViewContext& ctx, const doc::AudioInSources& allowed, const doc::Track& source) {
  const doc::Project& p = *ctx.doc();
  std::vector<Output> out;
  auto add = [&](std::string label, doc::Wire start) {
    if (!allowed.allows(AudioFrom::output(start))) return;  // notes, a loop: left out (RT.1's filter)
    out.push_back({std::move(label), std::move(start)});
  };
  doc::Wire src;
  src.noteSource = source.id;
  add(sourceName(source), src);
  if (source.kind != doc::TrackKind::Master) {
    const doc::MainPath path = doc::mainPath(p, source);
    std::vector<doc::DeviceId> racksDone;
    for (const doc::Device* d : path.blocks) {
      if (const std::optional<doc::RackInfo> r = doc::rackOfChain(p, source, d->id)) {
        if (std::find(racksDone.begin(), racksDone.end(), r->id) != racksDone.end()) continue;
        racksDone.push_back(r->id);
        for (const doc::RackChain& c : r->chains) add(rackChainName(*r, c), deviceStart(c.id));
        continue;
      }
      add(blockName(ctx, *d), deviceStart(d->id));
    }
  }
  doc::Wire mx;
  mx.mixerOut = source.id;
  add("Mixer", mx);
  return out;
}

std::string extChannels(const doc::AudioInput& in) {
  if (in.none()) return {};
  const std::string a = std::to_string(in.firstChannel + 1);
  return in.channels == 1 ? a : a + "/" + std::to_string(in.firstChannel + 2);
}

std::string fromLabel(const ViewContext& ctx, const AudioFrom& from) {
  switch (from.kind) {
    case AudioFromKind::None: return "No Input";
    case AudioFromKind::Ext: return "Ext. In";
    case AudioFromKind::Resampling: return "Resampling";
    case AudioFromKind::Output: break;
  }
  const doc::Track* t = doc::findTrackPtr(*ctx.doc(), trackOf(*ctx.doc(), from.start));
  return t != nullptr ? t->name : std::string("?");
}

std::string channelLabel(const ViewContext& ctx, const AudioFrom& from) {
  if (from.kind == AudioFromKind::Ext) return extChannels(from.ext);
  if (from.kind == AudioFromKind::Output) return outputName(ctx, from.start);
  return {};
}

bool channelShown(const AudioFrom& from) noexcept { return from.kind == AudioFromKind::Ext || from.kind == AudioFromKind::Output; }

std::string sourceLabel(const ViewContext& ctx, const AudioFrom& from) {
  switch (from.kind) {
    case AudioFromKind::None: return "No Input";
    case AudioFromKind::Ext: return "Ext. In " + extChannels(from.ext);
    case AudioFromKind::Resampling: return "Resampling";
    case AudioFromKind::Output: break;
  }
  return fromLabel(ctx, from) + " | " + outputName(ctx, from.start);
}

bool set(ViewContext& ctx, doc::TrackId track, const AudioFrom& from) {
  const doc::ops::EditResult e = doc::ops::setAudioFrom(*ctx.doc(), ctx.store().ids(), track, from, ctx.deviceLibrary());
  if (!e) {
    ctx.setStatus("Audio From: " + e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (Status s = ctx.store().dispatch(e->command); !s) {
    ctx.setStatus("Audio From: " + s.error().message);
    return false;
  }
  return true;
}

namespace {

ui::MenuItem choice(ViewContext& ctx, doc::TrackId track, std::string label, AudioFrom from, bool checked) {
  ViewContext* c = &ctx;
  ui::MenuItem m = ui::MenuItem::item(std::move(label), [c, track, from = std::move(from)] { (void)set(*c, track, from); });
  m.checked = checked;
  return m;
}

// Ext. In's channels: mono inputs, then stereo pairs (at least 2 offered; ones the open device lacks marked).
std::vector<ui::MenuItem> extItems(ViewContext& ctx, doc::TrackId track, const AudioFrom& cur) {
  const uint32_t have = ctx.transport().audioInputChannels();
  const doc::AudioInput sel = cur.kind == AudioFromKind::Ext ? cur.ext : doc::AudioInput{};
  const uint32_t n = std::max<uint32_t>({have, 2u, static_cast<uint32_t>(sel.firstChannel + sel.channels)});
  std::vector<ui::MenuItem> items;
  auto item = [&](doc::AudioInput in) {
    std::string label = extChannels(in);
    if (static_cast<uint32_t>(in.firstChannel + in.channels) > have) label += " (not available)";
    items.push_back(choice(ctx, track, std::move(label), AudioFrom::extIn(in), cur.kind == AudioFromKind::Ext && sel == in));
  };
  for (uint32_t ch = 0; ch < n; ++ch) item(doc::AudioInput{static_cast<uint16_t>(ch), 1});
  items.push_back(ui::MenuItem::sep());
  for (uint32_t ch = 0; ch + 1 < n; ch += 2) item(doc::AudioInput{static_cast<uint16_t>(ch), 2});
  return items;
}

// A track's outputs as choices (the current cable's start kept in the list even when it is not one of them: a cable
// drawn from a block off the path reads back too).
std::vector<ui::MenuItem> outputItems(ViewContext& ctx, doc::TrackId track, const AudioFrom& cur, const doc::Track& source,
                                      const doc::AudioInSources& allowed) {
  std::vector<Output> outs = outputsOf(ctx, allowed, source);
  const bool here = cur.kind == AudioFromKind::Output && trackOf(*ctx.doc(), cur.start) == source.id;
  if (here && std::none_of(outs.begin(), outs.end(), [&](const Output& o) { return AudioFrom::output(o.start) == cur; })) {
    const auto at = outs.empty() || !outs.back().start.fromMixerOut() ? outs.end() : outs.end() - 1;
    outs.insert(at, Output{outputName(ctx, cur.start), cur.start});
  }
  std::vector<ui::MenuItem> items;
  for (Output& o : outs) {
    const AudioFrom f = AudioFrom::output(o.start);
    items.push_back(choice(ctx, track, std::move(o.label), f, here && f == cur));
  }
  return items;
}

// Every other track the recording track may take (not itself, not one it feeds), in lane order (no master: Resampling).
template <class F>
void forEachOffered(const doc::Project& p, const doc::AudioInSources& allowed, F&& f) {
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    if (t.kind == doc::TrackKind::Master || !allowed.offersTrack(t.id)) return;
    f(t);
  });
}

}  // namespace

std::vector<ui::MenuItem> fromItems(ViewContext& ctx, doc::TrackId track) {
  const doc::Snapshot snap = ctx.doc();
  const doc::Project& p = *snap;
  const AudioFrom cur = doc::audioFromOf(p, track);
  const doc::AudioInSources allowed(p, track, ctx.deviceLibrary());
  std::vector<ui::MenuItem> items;
  items.push_back(choice(ctx, track, "Ext. In", AudioFrom::extIn(defaultExt(cur, ctx.transport().audioInputChannels())),
                         cur.kind == AudioFromKind::Ext));
  if (allowed.offersResampling())
    items.push_back(choice(ctx, track, "Resampling", AudioFrom::resampling(), cur.kind == AudioFromKind::Resampling));
  items.push_back(choice(ctx, track, "No Input", AudioFrom::none(), cur.kind == AudioFromKind::None));
  bool sep = false;
  const doc::TrackId curTrack = cur.kind == AudioFromKind::Output ? trackOf(p, cur.start) : doc::TrackId{};
  forEachOffered(p, allowed, [&](const doc::Track& t) {
    const std::vector<Output> outs = outputsOf(ctx, allowed, t);
    const Output* def = defaultOf(outs);
    if (def == nullptr) return;  // nothing of it carries audio here
    if (!sep) items.push_back(ui::MenuItem::sep()), sep = true;
    // Picking the track it already records from keeps the output; another starts at its default (Post FX).
    const AudioFrom f = curTrack == t.id ? cur : AudioFrom::output(def->start);
    ui::MenuItem m = choice(ctx, track, t.name, f, curTrack == t.id);
    m.swatch = ui::hex(t.colour.rgb);
    items.push_back(std::move(m));
  });
  return items;
}

std::vector<ui::MenuItem> channelItems(ViewContext& ctx, doc::TrackId track) {
  const doc::Snapshot snap = ctx.doc();
  const doc::Project& p = *snap;
  const AudioFrom cur = doc::audioFromOf(p, track);
  if (cur.kind == AudioFromKind::Ext) return extItems(ctx, track, cur);
  if (cur.kind != AudioFromKind::Output) return {};
  const doc::Track* source = doc::findTrackPtr(p, trackOf(p, cur.start));
  if (source == nullptr) return {};
  return outputItems(ctx, track, cur, *source, doc::AudioInSources(p, track, ctx.deviceLibrary()));
}

std::vector<ui::MenuItem> menu(ViewContext& ctx, doc::TrackId track) {
  const doc::Snapshot snap = ctx.doc();
  const doc::Project& p = *snap;
  const AudioFrom cur = doc::audioFromOf(p, track);
  const doc::AudioInSources allowed(p, track, ctx.deviceLibrary());
  std::vector<ui::MenuItem> items;
  ui::MenuItem ext = ui::MenuItem::sub("Ext. In", extItems(ctx, track, cur));
  ext.checked = cur.kind == AudioFromKind::Ext;
  items.push_back(std::move(ext));
  if (allowed.offersResampling())
    items.push_back(choice(ctx, track, "Resampling", AudioFrom::resampling(), cur.kind == AudioFromKind::Resampling));
  items.push_back(choice(ctx, track, "No Input", AudioFrom::none(), cur.kind == AudioFromKind::None));
  bool sep = false;
  const doc::TrackId curTrack = cur.kind == AudioFromKind::Output ? trackOf(p, cur.start) : doc::TrackId{};
  forEachOffered(p, allowed, [&](const doc::Track& t) {
    std::vector<ui::MenuItem> sub = outputItems(ctx, track, cur, t, allowed);
    if (sub.empty()) return;
    if (!sep) items.push_back(ui::MenuItem::sep()), sep = true;
    ui::MenuItem m = ui::MenuItem::sub(t.name, std::move(sub));
    m.checked = curTrack == t.id;
    m.swatch = ui::hex(t.colour.rgb);
    items.push_back(std::move(m));
  });
  return items;
}

}  // namespace pq::views::audio_from
