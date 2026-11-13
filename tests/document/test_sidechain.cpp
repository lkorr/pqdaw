// DY.2 (docs/plans/DY.md "DY.2"; SPEC §8 "sidechain", §15): a cable from another track into a block's SIGNAL input is a
// sidechain (graph::WireKind::Sidechain: compensated, every channel); into a param it stays modulation. The cable
// sources a picker offers (Pre FX / Post FX per other track) and its one-step rewire (doc::ops::setCableSource), the
// Input binding through save / load.
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/panel.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "../support/blocks.h"

using namespace pq::doc;
using pq::Result;
using W = pq::graph::WireKind;

namespace {

const DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// Kick (MIDI: instrument → latency 480) and Bass (MIDI: instrument → sub, a value block off the path); Pad (audio).
struct Doc {
  DocumentStore store;
  TrackId kick, bass, pad;
  DeviceId lat, sub, val;
  Doc() {
    store.setDeviceLibrary(&library());
    kick = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi, {}, "Kick")).tracks.at(0);
    bass = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi, {}, "Bass")).tracks.at(0);
    pad = edit(ops::createTrack(doc(), store.ids(), TrackKind::Audio, {}, "Pad")).tracks.at(0);
    lat = test_blocks::appendAfter(store, kick, "atom:latency");
    edit(ops::setDeviceSetting(doc(), lat, "frames", 480.0));
    sub = test_blocks::appendAfter(store, bass, "atom:sub");
    val = edit(ops::addDevice(doc(), store.ids(), bass, std::string(kValueDevice), DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  }
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(const Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const size_t before = store.history().cursor();
    REQUIRE(store.dispatch(e->command).ok());
    CHECK(store.history().cursor() == before + (e->empty() ? 0u : 1u));  // one undo step
    return *e;
  }
  WireId wire(Wire ends) { return edit(ops::connect(doc(), store.ids(), std::move(ends))).wires.at(0); }
  DeviceId instrument(TrackId t) const { return mainPath(doc(), *findTrackPtr(doc(), t)).blocks.front()->id; }
  LoweredProject lowered() const {
    LowerOptions lo;
    lo.library = &library().library();
    lo.devices = &library();
    lo.slotOf = [](TrackId) { return 0u; };
    auto r = lower(doc(), lo);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }
};

Wire from(DeviceId d, std::string port, DeviceId to, std::string param) {
  Wire w;
  w.from = d;
  w.port = std::move(port);
  w.to = to;
  w.param = std::move(param);
  return w;
}
Wire fromSource(TrackId t, DeviceId to, std::string param) {
  Wire w;
  w.noteSource = t;
  w.to = to;
  w.param = std::move(param);
  return w;
}

std::optional<W> kindOf(const LoweredProject& l, WireId w) {
  const std::string m = "m" + std::to_string(w.value);
  for (const pq::graph::ProjectWire& gw : l.graph.wires)
    if (gw.from.port == m && gw.to.port == m) return gw.kind;
  return std::nullopt;
}
WireStatus statusOf(const LoweredProject& l, WireId w) {
  for (const WireState& s : l.wires)
    if (s.id == w) return s.status;
  return WireStatus::Invalid;
}

}  // namespace

TEST_CASE("DY.2 lowering: a cable from another track into a signal input is a Sidechain; into a param Modulation; a "
          "track's own signal reaches another track's block") {
  Doc d;
  const WireId sc = d.wire(from(d.lat, "", d.sub, "b"));                 // into sub's input b: a sidechain
  const WireId mod = d.wire(from(d.lat, "", d.val, std::string(kValueParam)));  // into a value block's param: modulation
  const WireId own = d.wire(fromSource(d.pad, d.sub, "b"));              // Pad's own signal (Pre FX) into the same input
  const LoweredProject l = d.lowered();
  CHECK(statusOf(l, sc) == WireStatus::Lowered);
  CHECK(statusOf(l, mod) == WireStatus::Lowered);
  CHECK(statusOf(l, own) == WireStatus::Lowered);  // (was Inactive before DY.2: a track's own signal stayed on it)
  CHECK(kindOf(l, sc) == W::Sidechain);
  CHECK(kindOf(l, mod) == W::Modulation);
  CHECK(kindOf(l, own) == W::Sidechain);
  // The sidechain carries every channel: the source exports the block's whole output (no `channel` pick for it), the
  // target reads it as an inferred-width input; the param cable keeps its first channel (as before DY.2).
  size_t kickIdx = 0;
  for (size_t i = 0; i < l.trackIds.size(); ++i)
    if (l.trackIds[i] == d.kick) kickIdx = i;
  const std::string kickText = patchText(*l.patches[kickIdx]);
  CAPTURE(kickText);
  CHECK(kickText.find("export m" + std::to_string(sc.value)) != std::string::npos);
  CHECK(kickText.find("export m" + std::to_string(mod.value)) != std::string::npos);
  // A cable into the track's own block stays on the track (no graph wire).
  const WireId same = d.wire(from(d.instrument(d.bass), "", d.sub, "b"));
  CHECK_FALSE(kindOf(d.lowered(), same).has_value());
}

TEST_CASE("DY.2 cable sources: Pre / Post FX per other track (not the master, not the input's track); one undo step "
          "per pick; the current source read from the wires; save / load") {
  Doc d;
  // A value block "On" the picker sets (0 … 1).
  const DeviceId on = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.bass, std::string(kValueDevice), DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  const std::vector<ops::CableSource> src = ops::cableSources(d.doc(), d.bass);
  // Kick: Pre (its instrument's output — a MIDI track's own signal is notes) and Post (the latency block); Pad: Pre only
  // (no blocks: nothing feeds its mixer but its own signal). Not Bass, not the master.
  REQUIRE(src.size() == 3);
  CHECK(src[0].track == d.kick);
  CHECK_FALSE(src[0].post);
  CHECK(src[0].ends.from == d.instrument(d.kick));
  CHECK(src[1].track == d.kick);
  CHECK(src[1].post);
  CHECK(src[1].ends.from == d.lat);
  CHECK(src[2].track == d.pad);
  CHECK(src[2].ends.noteSource == d.pad);
  CHECK(src[2].ends.port.empty());

  const DeviceLibrary* lib = &library();
  auto pick = [&](std::optional<Wire> s) { return d.edit(ops::setCableSource(d.doc(), d.store.ids(), lib, d.sub, "b", s, on, std::string(kValueParam))); };
  auto onValue = [&] {
    const DeviceValue* v = findDevice(d.doc(), on).device->param(kValueParam);
    return v != nullptr ? v->value : -1.0;
  };
  // Kick Post FX: one step, one wire, "On" set.
  pick(src[1].ends);
  std::vector<WireId> now = ops::crossTrackWiresInto(d.doc(), lib, d.sub, "b");
  REQUIRE(now.size() == 1);
  CHECK(ops::sameSourceEnd(*findWire(d.doc(), now[0]), src[1].ends));
  CHECK(onValue() == doctest::Approx(1.0));
  // The same again: an empty edit (nothing recorded).
  CHECK(ops::setCableSource(d.doc(), d.store.ids(), lib, d.sub, "b", src[1].ends, on, std::string(kValueParam))->empty());
  // Pad Pre FX: the old wire goes in the same step.
  pick(src[2].ends);
  now = ops::crossTrackWiresInto(d.doc(), lib, d.sub, "b");
  REQUIRE(now.size() == 1);
  CHECK(findWire(d.doc(), now[0])->noteSource == d.pad);
  // A cable drawn by hand from another track is a cross-track wire into the input too; None removes every one.
  d.wire(from(d.lat, "", d.sub, "b"));
  CHECK(ops::crossTrackWiresInto(d.doc(), lib, d.sub, "b").size() == 2);
  const WireId keep = d.wire(from(d.instrument(d.bass), "", d.sub, "b"));  // (its own track: not a source)
  pick(std::nullopt);
  CHECK(ops::crossTrackWiresInto(d.doc(), lib, d.sub, "b").empty());
  CHECK(findWire(d.doc(), keep) != nullptr);
  CHECK(onValue() == doctest::Approx(0.0));
  REQUIRE(d.store.undo());
  CHECK(ops::crossTrackWiresInto(d.doc(), lib, d.sub, "b").size() == 2);
  // Refused: a source on the input's own track.
  Wire mine;
  mine.from = d.instrument(d.bass);
  CHECK_FALSE(ops::setCableSource(d.doc(), d.store.ids(), lib, d.sub, "b", mine).ok());

  // Save / load: the wires and a panel's Input binding round-trip.
  const DeviceId g = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.bass, std::string(kGroupDevice), DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  d.edit(ops::setDeviceGroup(d.doc(), d.store.ids(), lib, on, g));
  pick(src[0].ends);
  PanelElement e;
  e.kind = "cablesource";
  e.w = 120, e.h = 30;
  e.bindings = {inputBinding(g), controlBinding(on, std::string(kValueParam))};
  d.edit(ops::addPanelElements(d.doc(), g, {e}));
  d.edit(ops::setCableSource(d.doc(), d.store.ids(), lib, g, "", src[1].ends, on, std::string(kValueParam)));
  REQUIRE(ops::crossTrackWiresInto(d.doc(), lib, g, "").size() == 1);
  auto text = serializeProject(d.doc());
  REQUIRE(text.ok());
  CHECK(text->find("\"kind\": \"input\"") != std::string::npos);
  auto back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == d.doc());
  const Panel* p = findDevice(*back->project, g).device->panel.get();
  REQUIRE(p != nullptr);
  REQUIRE(p->elements.size() == 1);
  CHECK(p->elements[0].bindings.at(0) == inputBinding(g));
}
