// T4.13: per-device CPU cost for the project patch view's labels, through the playback link — one CostMeter per unit
// schedule bound to every published image, the region → device attribution of T4.5 per member track. Nothing is
// reported while metering is off; on, the devices that run report ns per sample; the audio is unchanged by metering.
#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/store.h"
#include "pq/views/project_patch/project_patch_view.h"

using namespace pq;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr uint32_t kBlock = 64;

std::unique_ptr<ProjectLink> makeLink(const doc::Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  c.idleUnits = false;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

std::vector<float> run(ProjectLink& link, int64_t& time, uint32_t blocks) {
  std::vector<float> l(kBlock), r(kBlock), out;
  for (uint32_t b = 0; b < blocks; ++b) {
    float* ch[2] = {l.data(), r.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
    }
    time += kBlock;
    out.insert(out.end(), l.begin(), l.end());
  }
  return out;
}

}  // namespace

TEST_CASE("project patch cost: devices report ns per sample only while metered; metering leaves the audio unchanged") {
  doc::DocumentStore store;
  REQUIRE(views::project_patch::buildSketchProject(store).ok());
  const doc::Snapshot s = store.current();
  std::vector<doc::DeviceId> ids;
  doc::DeviceId op, sat;
  doc::forEachTrack(*s, [&](const doc::Track& t, const doc::Track*) {
    for (const doc::DevicePtr& d : t.devices) {
      ids.push_back(d->id);
      if (t.name == "Keys" && d->patch == "operator") op = d->id;
      if (t.name == "Keys" && d->patch == "saturator") sat = d->id;
    }
  });
  REQUIRE(op.valid());
  REQUIRE(sat.valid());

  // The same playback twice: unmetered, then metered.
  std::vector<float> plain, metered;
  for (int pass = 0; pass < 2; ++pass) {
    auto link = makeLink(s);
    link->flush();
    int64_t time = 0;
    std::vector<float> ns(ids.size(), 0.0f);
    link->readDeviceCosts(ids, ns);
    for (const float v : ns) CHECK(v < 0.0f);  // nothing measured while off
    if (pass == 1) link->setCostMetering(true);
    link->play(0);
    std::vector<float> a = run(*link, time, 64);
    link->readDeviceCosts(ids, ns);  // the meters' baselines
    std::vector<float> b = run(*link, time, 256);
    a.insert(a.end(), b.begin(), b.end());
    link->readDeviceCosts(ids, ns);
    (pass == 0 ? plain : metered) = std::move(a);
    if (pass == 0) {
      for (const float v : ns) CHECK(v < 0.0f);
      continue;
    }
    // The keys' Operator (voices playing their clip) and Saturator run: both are charged a positive cost.
    for (size_t i = 0; i < ids.size(); ++i) {
      if (ids[i] == op || ids[i] == sat) {
        CHECK(ns[i] > 0.0f);
        MESSAGE("device " << ids[i].value << ": " << ns[i] << " ns per sample");
      }
    }
    link->setCostMetering(false);
    link->readDeviceCosts(ids, ns);
    for (const float v : ns) CHECK(v < 0.0f);
  }
  REQUIRE(plain.size() == metered.size());
  bool same = true;
  for (size_t i = 0; i < plain.size(); ++i) same = same && plain[i] == metered[i];
  CHECK(same);  // timing regions changes nothing that is computed
}
