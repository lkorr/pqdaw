// PF.6: lowering the document (doc::lower) per structural edit, the cost the project patch view (UI thread) and the
// playback link (ProjectLink) pay. Two projects, built through the edit operations as a user would:
//   - wide64: 64 MIDI tracks with the default instrument (groups of eight with a Utility each, a Saturator or VCA and
//     every third a Utility per track, two returns fed by sends; the project patch view's bench project)
//   - heavy30: 30 MIDI tracks (Operator / the default instrument) with 8 effects each, 3 groups, a return fed by every
//     track, an LFO track modulating a device on every tenth track (cross-track wires)
// Rows: a full lowering (no previous), and a lowering after one device switched off / on with the previous lowering
// passed (what the view and the link do), each with the device library (LowerOptions::devices, as both callers set).
// Run from release (A/B) or ship (bench/RESULTS.md).

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/store.h"
#include "pq_bench.h"

using namespace pq;

namespace {

struct Runner {
  doc::DocumentStore& store;
  bool ok = true;
  std::optional<doc::ops::Edit> run(const doc::ops::EditResult& e) {
    if (!ok) return std::nullopt;
    if (!e) {
      std::printf("edit: %s\n", e.error().message.c_str());
      ok = false;
      return std::nullopt;
    }
    if (Status s = store.dispatch(e->command); !s) {
      std::printf("dispatch: %s\n", s.error().message.c_str());
      ok = false;
      return std::nullopt;
    }
    return *e;
  }
  doc::TrackId track(doc::TrackKind kind, doc::TrackId after, std::string name) {
    auto e = run(doc::ops::createTrack(*store.current(), store.ids(), kind, after, std::move(name)));
    return e && !e->tracks.empty() ? e->tracks[0] : doc::TrackId{};
  }
  doc::DeviceId device(doc::TrackId t, const char* patch, doc::DeviceKind kind, uint32_t index = doc::kAppend) {
    auto e = run(doc::ops::addDevice(*store.current(), store.ids(), t, patch, kind, index));
    return e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
  }
  doc::TrackId group(std::vector<doc::TrackId> members, std::string name) {
    auto e = run(doc::ops::groupTracks(*store.current(), store.ids(), members, std::move(name)));
    return e && !e->tracks.empty() ? e->tracks[0] : doc::TrackId{};
  }
};

// Returns a device to toggle (on the middle track's path).
doc::DeviceId buildWide64(doc::DocumentStore& store) {
  Runner r{store};
  using K = doc::DeviceKind;
  doc::TrackId after;
  std::vector<doc::TrackId> made;
  doc::DeviceId toggle;
  for (uint32_t i = 0; i < 64; ++i) {
    after = r.track(doc::TrackKind::Midi, after, "Track " + std::to_string(i + 1));
    made.push_back(after);
    const doc::DeviceId d = r.device(after, i % 2 == 0 ? "module:saturator" : "module:vca", K::AudioEffect);
    if (i == 8) toggle = d;
    if (i % 3 == 0) (void)r.device(after, "module:utility", K::AudioEffect);
  }
  for (uint32_t g = 0; g + 8 <= made.size(); g += 8) {
    const doc::TrackId gt = r.group(std::vector<doc::TrackId>(made.begin() + g, made.begin() + g + 8), "Group " + std::to_string(g / 8 + 1));
    (void)r.device(gt, "module:utility", K::AudioEffect);
  }
  const doc::TrackId a = r.track(doc::TrackKind::Return, {}, "A");
  const doc::TrackId b = r.track(doc::TrackKind::Return, a, "B");
  for (size_t i = 0; i < made.size(); i += 2) (void)store.dispatch(doc::SetSendLevel{made[i], i % 4 == 0 ? a : b, -10.0f});
  return r.ok ? toggle : doc::DeviceId{};
}

doc::DeviceId buildHeavy30(doc::DocumentStore& store) {
  Runner r{store};
  using K = doc::DeviceKind;
  static const char* const fx[] = {"module:saturator", "module:vca", "module:utility", "module:saturator",
                                   "module:vca",       "module:utility", "module:saturator", "module:vca"};
  doc::TrackId after;
  std::vector<doc::TrackId> made;
  std::vector<doc::DeviceId> targets;
  doc::DeviceId toggle;
  for (uint32_t i = 0; i < 30; ++i) {
    after = r.track(doc::TrackKind::Midi, after, "Voice " + std::to_string(i + 1));
    made.push_back(after);
    if (i % 2 == 1) (void)r.device(after, "module:operator", K::Instrument);
    for (uint32_t k = 0; k < 8; ++k) {
      const doc::DeviceId d = r.device(after, fx[k], K::AudioEffect);
      if (i == 15 && k == 3) toggle = d;
      if (i % 10 == 0 && k == 0) targets.push_back(d);
    }
  }
  for (uint32_t g = 0; g < 3; ++g) {
    const doc::TrackId gt = r.group(std::vector<doc::TrackId>(made.begin() + g * 10, made.begin() + g * 10 + 10), "Bus " + std::to_string(g + 1));
    (void)r.device(gt, "module:vca", K::AudioEffect);
    (void)r.device(gt, "module:utility", K::AudioEffect);
  }
  const doc::TrackId ret = r.track(doc::TrackKind::Return, {}, "Space");
  (void)r.device(ret, "module:saturator", K::AudioEffect);
  for (const doc::TrackId t : made) (void)store.dispatch(doc::SetSendLevel{t, ret, -12.0f});
  // Cross-track modulation: an LFO on its own track into a Saturator's Drive on every tenth track.
  const doc::TrackId mod = r.track(doc::TrackKind::Midi, {}, "Mod");
  const doc::DeviceId lfo = r.device(mod, "module:lfo", K::AudioEffect, doc::ops::kOffPath);
  for (const doc::DeviceId t : targets) {
    const doc::DeviceLoc l = doc::findDevice(*store.current(), t);
    if (l.track == nullptr) continue;
    for (const doc::Device* d : doc::groupMembersDeep(l.track->devices, t))
      if (d->name == "Drive") {
        (void)r.run(doc::ops::connectWire(*store.current(), store.ids(), lfo, "y", d->id, std::string(doc::kValueParam)));
        break;
      }
  }
  return r.ok ? toggle : doc::DeviceId{};
}

// A digest of everything a lowering hands its users (the patches' printed IR, params, routing, wire states, taps, curve
// sites, meta resources): equal digests before / after a change to the lowering = the same output on this fixture.
uint64_t digest(const doc::LoweredProject& l) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](std::string_view s) {
    for (const char c : s) h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
    h = (h ^ 0xFFu) * 1099511628211ull;
  };
  auto num = [&](double v) { mix(std::to_string(v)); };
  for (size_t i = 0; i < l.patches.size(); ++i) {
    num(l.trackIds[i].value);
    mix(doc::patchText(*l.patches[i]));
    num(static_cast<double>(doc::patchHash(l.patches[i]->patch)));
    const pq::graph::ProjectTrack& t = l.graph.tracks[i];
    num(t.id), num(static_cast<double>(t.role)), num(t.eventSlot), num(t.maxVoices), num(t.steal);
  }
  for (const pq::graph::ProjectWire& w : l.graph.wires) {
    num(w.from.track), mix(w.from.port), num(w.to.track), mix(w.to.port), num(static_cast<double>(w.kind));
  }
  for (const pq::graph::ProjectPort& p : l.graph.taps) num(p.track), mix(p.port);
  for (const doc::LoweredParam& p : l.params) mix(p.key), num(p.track), mix(p.name), num(p.value), num(p.position);
  num(l.master);
  for (const doc::DelayedSend& d : l.delayedSends) num(d.from.value), num(d.to.value);
  for (const doc::TrackId t : l.withoutSlot) num(t.value);
  for (const doc::DeviceId d : l.missingDevices) num(d.value);
  for (const doc::WireState& w : l.wires) num(w.id.value), num(static_cast<double>(w.status)), num(static_cast<double>(w.reason));
  for (const doc::ModulationTap& t : l.taps) num(t.device.value), mix(t.param), num(t.track), mix(t.port), num(static_cast<double>(t.domain));
  for (const doc::CurveSite& c : l.curves) num(c.track.value), num(c.device.value), mix(c.param), num(c.slot), num(c.value.value), num(c.valuePort);
  for (const auto& e : l.deviceErrors) num(e.device.value), mix(e.message);
  for (const auto& m : l.metaResources) num(m.track.value), num(m.device.value), mix(m.path), mix(m.type), num(m.slot);
  return h;
}

int runOne(const char* name, const doc::DeviceLibrary& lib, doc::DeviceId (*build)(doc::DocumentStore&)) {
  doc::DocumentStore store;
  store.setDeviceLibrary(&lib);
  const doc::DeviceId toggle = build(store);
  if (!toggle.valid()) {
    std::printf("%s: build failed\n", name);
    return 1;
  }
  doc::LowerOptions lo;
  lo.library = &lib.library();
  lo.devices = &lib;
  std::shared_ptr<const doc::LoweredProject> prev;
  std::vector<double> full, edit;
  size_t units = 0, ops = 0;
  for (int i = 0; i < 12; ++i) {
    const doc::Snapshot s = store.current();
    const auto t0 = std::chrono::steady_clock::now();
    auto r = doc::lower(*s, lo);
    full.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count());
    if (!r) {
      std::printf("%s: lower: %s\n", name, r.error().message.c_str());
      return 1;
    }
    units = r->graph.tracks.size();
    ops = r->params.size();
    prev = std::make_shared<doc::LoweredProject>(std::move(*r));
  }
  for (int i = 0; i < 24; ++i) {
    auto e = doc::ops::setDevicesActive(*store.current(), std::vector<doc::DeviceId>{toggle}, i % 2 != 0);
    if (!e || !store.dispatch(e->command)) return 1;
    const doc::Snapshot s = store.current();
    const auto t0 = std::chrono::steady_clock::now();
    auto r = doc::lower(*s, lo, prev.get());
    edit.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count());
    if (!r) return 1;
    prev = std::make_shared<doc::LoweredProject>(std::move(*r));
  }
  size_t devices = 0;
  doc::forEachDevice(*store.current(), [&](const doc::Device&, const doc::Track&) { ++devices; });
  std::printf("%s: %zu tracks, %zu devices, %zu wires, %zu params; lowering digest %016llx\n", name, units, devices,
              doc::wiresOf(*store.current()).size(), ops, static_cast<unsigned long long>(digest(*prev)));
  const std::string a = std::string("lower.") + name + " full";
  const std::string b = std::string("lower.") + name + " after a device toggle (previous passed)";
  pq::bench::reportStats(a.c_str(), pq::bench::summarize(full), "ns/lower");
  pq::bench::reportStats(b.c_str(), pq::bench::summarize(edit), "ns/lower");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  auto lib = doc::loadDeviceLibrary(std::string(PQ_SOURCE_DIR) + "/stdlib");
  if (!lib) {
    std::printf("device library: %s\n", lib.error().message.c_str());
    return 1;
  }
  const doc::DeviceLibrary& l = **lib;
  pq::bench::Runner r("pq_bench_lower");
  r.add("lower/wide64", [&](pq::bench::Context&) { (void)runOne("wide64", l, buildWide64); });
  r.add("lower/heavy30", [&](pq::bench::Context&) { (void)runOne("heavy30", l, buildHeavy30); });
  return r.main(argc, argv);
}
