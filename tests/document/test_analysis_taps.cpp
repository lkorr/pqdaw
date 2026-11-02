// TX.spectrum: the lowering's analysis taps (pq/document/lower.h "Analysis taps") — an analysed stock device (EQ Eight,
// Auto Filter) gets a signal_tap of its input and output with a slot from metaSlotOf, listed in metaResources; none
// without metaSlotOf (tools, tests), for an inactive device, or for a device that is not analysed; a chain Spectrum
// meta device lists its own SignalTap request; the lowered graph compiles.
#include <doctest/doctest.h>

#include <memory>
#include <ostream>
#include <string>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/store.h"
#include "pq/graph/atoms.h"

using namespace pq::doc;
using pq::Result;

namespace {

DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

struct Rig {
  DocumentStore store;
  TrackId track;
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  Rig() { track = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0); }
  DeviceId add(const char* patch) {
    return edit(ops::addDevice(doc(), store.ids(), track, patch, DeviceKind::AudioEffect)).devices.at(0);
  }
};

Result<LoweredProject> lowerIt(const Project& p, bool slots) {
  LowerOptions o;
  o.library = &library().library();
  o.devices = &library();
  o.slotOf = [](TrackId) { return 0u; };
  if (slots) o.metaSlotOf = [](DeviceId d, std::string_view, uint32_t i) { return d.value * 4 + i + 1; };
  return lower(p, o);
}

// The signal_tap nodes of every lowered track patch.
uint32_t tapsIn(const LoweredProject& lp) {
  uint32_t n = 0;
  for (const auto& patch : lp.patches)
    for (pq::graph::NodeIndex v = 0; v < patch->patch.nodeCount(); ++v) {
      const pq::graph::Node& nd = patch->patch.node(v);
      if (nd.kind == pq::graph::NodeKind::Atom && pq::graph::atomSignature(nd.atom).name == "signal_tap") ++n;
    }
  return n;
}

}  // namespace

TEST_CASE("analysis taps: EQ Eight and Auto Filter get one each with their slot; Utility none; none without slots") {
  CHECK(analysedDevice("eq_eight"));
  CHECK(analysedDevice("auto_filter"));
  CHECK_FALSE(analysedDevice("utility"));
  CHECK_FALSE(analysedDevice("saturator"));
  Rig r;
  const DeviceId eq = r.add("eq_eight"), af = r.add("auto_filter");
  (void)r.add("utility");
  auto lp = lowerIt(r.doc(), true);
  REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
  CHECK(tapsIn(*lp) == 2);
  uint32_t listed = 0;
  for (const LoweredProject::MetaResource& m : lp->metaResources) {
    if (m.request.kind != pq::meta::ResourceKind::SignalTap) continue;
    ++listed;
    CHECK(m.path == kAnalysisTapPath);
    CHECK(m.type.empty());
    CHECK((m.device == eq || m.device == af));
    CHECK(m.slot == m.device.value * 4 + 1);
    CHECK(m.track == r.track);
  }
  CHECK(listed == 2);
  // Without metaSlotOf (tools, tests): no tap, the patch as before.
  auto plain = lowerIt(r.doc(), false);
  REQUIRE(plain.ok());
  CHECK(tapsIn(*plain) == 0);
  // An inactive device is not lowered: no tap.
  const DeviceId off[1] = {eq};
  r.edit(ops::setDevicesActive(r.doc(), off, false));
  auto lp2 = lowerIt(r.doc(), true);
  REQUIRE(lp2.ok());
  CHECK(tapsIn(*lp2) == 1);
}

TEST_CASE("analysis taps: a chain Spectrum meta device lists its own SignalTap request") {
  Rig r;
  const DeviceId spec = r.edit(ops::addMetaDevice(r.doc(), r.store.ids(), r.track, "spectrum")).devices.at(0);
  auto lp = lowerIt(r.doc(), true);
  REQUIRE_MESSAGE(lp.ok(), (lp.ok() ? std::string() : lp.error().message));
  bool found = false;
  for (const LoweredProject::MetaResource& m : lp->metaResources)
    if (m.device == spec && m.request.kind == pq::meta::ResourceKind::SignalTap) {
      found = true;
      CHECK(m.path.empty());
      CHECK(m.type == "spectrum");
      CHECK(m.slot == spec.value * 4 + 1);
    }
  CHECK(found);
}
