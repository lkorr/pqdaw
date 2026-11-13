// WI.2 (docs/plans/RB.md D1; lower.h "Cut wires"): a wire LowerOptions::cutWires names is lowered as if it were not in
// the document — the IR equals the project without it, a cut main-path link is a path break — and listed Refused /
// Type; LoweredProject::wireEnds names the track-patch edge each wire and main link makes; no predicate: nothing changes.
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/store.h"
#include "../support/blocks.h"

using namespace pq::doc;

namespace {

const DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

struct Doc {
  DocumentStore store;
  TrackId keys;
  Doc() {
    auto t = ops::createTrack(doc(), store.ids(), TrackKind::Midi, {}, "Keys");
    REQUIRE(t.ok());
    REQUIRE(store.dispatch(t->command).ok());
    keys = t->tracks.at(0);
  }
  const Project& doc() const { return *store.current(); }
  ops::EditResult must(ops::EditResult e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return e;
  }
  DeviceId block(const std::string& patch, uint32_t at) {
    return must(ops::addDevice(doc(), store.ids(), keys, patch, DeviceKind::AudioEffect, at))->devices.at(0);
  }
  WireId wire(DeviceId from, std::string port, DeviceId to, std::string param) {
    return must(ops::connectWire(doc(), store.ids(), from, std::move(port), to, std::move(param)))->wires.at(0);
  }
};

LowerOptions options(std::vector<WireId> cut = {}, bool predicate = false) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [](TrackId) { return 0u; };
  if (predicate || !cut.empty())
    lo.cutWires = [cut](WireId w) { return std::find(cut.begin(), cut.end(), w) != cut.end(); };
  return lo;
}

LoweredProject lowered(const Project& p, const LowerOptions& lo) {
  auto r = lower(p, lo);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return std::move(*r);
}

std::vector<std::string> texts(const LoweredProject& l) {
  std::vector<std::string> v;
  for (const auto& p : l.patches) v.push_back(patchText(*p));
  return v;
}

const WireState* stateOf(const LoweredProject& l, WireId w) {
  for (const WireState& s : l.wires)
    if (s.id == w) return &s;
  return nullptr;
}

}  // namespace

TEST_CASE("cut wires: a cut wire lowers as the project without it, Refused / Type; a cut main link breaks the path") {
  Doc d;
  const DeviceId util = test_blocks::appendAfter(d.store, d.keys, "module:utility");
  const DeviceId fade = d.block("atom:delay_fade", ops::kOffPath);
  const WireId bad = d.wire(util, "", fade, "fade");
  const Snapshot with = d.store.current();
  const MainPath path = mainPath(*with, *findTrackPtr(*with, d.keys));
  REQUIRE(path.complete);
  REQUIRE(path.links.size() >= 3);  // notes → instrument → Utility → mixer

  // No predicate, or one that names nothing: the plain lowering (same IR, same states, nothing Refused / Type).
  const LoweredProject plain = lowered(*with, options());
  const LoweredProject none = lowered(*with, options({}, true));
  CHECK(texts(plain) == texts(none));
  CHECK(plain.wires == none.wires);
  REQUIRE(stateOf(plain, bad) != nullptr);
  CHECK(stateOf(plain, bad)->status == WireStatus::Lowered);

  // The wire into delay_fade's `fade` has its end: the Utility's output → the block's call, on the track's patch.
  bool found = false;
  for (const WireEnd& e : plain.wireEnds)
    if (e.wire == bad) {
      found = true;
      uint32_t entity = 0, slot = 0;
      REQUIRE(nodeEntity(e.to, entity, slot));
      CHECK(entity == fade.value);
      CHECK(slot == 32u);  // NodeRole::Device k 0: the block's call
      REQUIRE(nodeEntity(e.from, entity, slot));
      CHECK(entity == util.value);
      CHECK(e.entity == bad.value);
      CHECK(plain.trackIds.at(e.track) == d.keys);
    }
  CHECK(found);
  // … and every main link into a block has one (the link into the Utility: from the instrument's output to its call).
  const WireId intoUtil = path.links.at(static_cast<size_t>(path.indexOf(util)));
  CHECK(std::any_of(plain.wireEnds.begin(), plain.wireEnds.end(), [&](const WireEnd& e) {
    uint32_t entity = 0, slot = 0;
    return e.wire == intoUtil && nodeEntity(e.to, entity, slot) && entity == util.value;
  }));

  // Cut: the IR of the project without the wire, the wire Refused / Type (every other state as without it).
  const LoweredProject cut = lowered(*with, options({bad}));
  const Snapshot without = [&] {
    DocumentStore s(with);
    REQUIRE(s.dispatch(ops::deleteWires(*with, std::vector<WireId>{bad})->command).ok());
    return s.current();
  }();
  const LoweredProject ref = lowered(*without, options());
  CHECK(texts(cut) == texts(ref));
  REQUIRE(stateOf(cut, bad) != nullptr);
  CHECK(stateOf(cut, bad)->status == WireStatus::Refused);
  CHECK(stateOf(cut, bad)->reason == WireRefusal::Type);
  CHECK(cut.wires.size() == plain.wires.size());
  CHECK(std::is_sorted(cut.wires.begin(), cut.wires.end(), [](const WireState& a, const WireState& b) { return a.id < b.id; }));
  CHECK(std::none_of(cut.wireEnds.begin(), cut.wireEnds.end(), [&](const WireEnd& e) { return e.wire == bad; }));

  // A cut main link: the path breaks there (as without the link: the track's mixer hears nothing of the path).
  const LoweredProject broken = lowered(*with, options({intoUtil}));
  const LoweredProject unlinked = [&] {
    DocumentStore s(with);
    REQUIRE(s.dispatch(ops::deleteWires(*with, std::vector<WireId>{intoUtil})->command).ok());
    return lowered(*s.current(), options());
  }();
  CHECK(texts(broken) == texts(unlinked));
  CHECK(texts(broken) != texts(plain));
  REQUIRE(stateOf(broken, intoUtil) != nullptr);
  CHECK(stateOf(broken, intoUtil)->status == WireStatus::Refused);
  CHECK(stateOf(broken, intoUtil)->reason == WireRefusal::Type);
}
