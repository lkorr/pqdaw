// DY.3: doc::GroupTransfer (pq/document/group_response.h): a group's static In → Out curve from the lowered project — the
// Saturator's "Hard Curve" group exactly against its scalar formula, a group holding z1 has none, a hand-built
// gain-reduction group's knobs re-read by update() (its stamp moves only when a value it reads does) equal a relowering.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/group_response.h"
#include "pq/document/lower.h"
#include "pq/document/modules.h"
#include "pq/document/model.h"
#include "pq/document/store.h"

using namespace pq::doc;
using pq::graph::ResponseKind;

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
  TrackId track;
  Doc() { track = edit(ops::createTrack(doc(), store.ids(), TrackKind::Audio, {}, "A")).tracks.at(0); }
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(const pq::Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  void cmd(Command c) {
    const pq::Status st = store.dispatch(std::move(c));
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  DeviceId block(std::string patch, DeviceId group) {
    const DeviceId d = edit(ops::addDevice(doc(), store.ids(), track, std::move(patch), DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
    cmd(SetDeviceGroup{d, group});
    return d;
  }
  void wire(DeviceId from, std::string port, DeviceId to, std::string param) {
    edit(ops::connectWire(doc(), store.ids(), from, std::move(port), to, std::move(param)));
  }
  LoweredProject lowered() const {
    LowerOptions lo;
    lo.library = &library().library();
    lo.devices = &library();
    lo.clipSlotOf = [](TrackId) { return 0u; };
    auto low = lower(doc(), lo);
    REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
    return std::move(*low);
  }
  // Value block `v` set to `units` through its curve (`curve`: replaced first, a structural edit).
  void setValue(DeviceId v, double units, std::optional<pq::dsp::Curve> curve = std::nullopt) {
    if (curve) cmd(SetParamCurve{v, std::string(kValueParam), *curve});
    const pq::dsp::Curve* c = findDevice(doc(), v).device->curve(kValueParam);
    REQUIRE(c != nullptr);
    cmd(SetDeviceParam{v, std::string(kValueParam), pq::dsp::curvePosition(*c, pq::dsp::curveOutput(*c, units), 0.5)});
  }
  float value(DeviceId v) const { return static_cast<float>(valueBlockUnits(*findDevice(doc(), v).device)); }
};

constexpr double kSr = 48000.0;

std::vector<float> grid() {
  std::vector<float> xs;
  for (int i = -30; i <= 30; ++i) xs.push_back(0.1f * static_cast<float>(i));
  return xs;
}

std::vector<float> eval(GroupTransfer& t, const std::vector<float>& xs, ResponseKind expect = ResponseKind::Exact) {
  std::vector<float> ys(xs.size() * t.outChannels());
  CHECK(t.evaluate(xs, kSr, ys) == expect);
  return ys;
}

// A gain computer: In (level dB) → sub ← Threshold → max ← Zero → mul ← Slope → Out (reduction dB = slope · max(0, x − T)).
struct Reduction {
  DeviceId group, threshold, zero, slope;
};
Reduction reductionGroup(Doc& d) {
  Reduction r;
  r.group = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.track, std::string(kGroupDevice), DeviceKind::AudioEffect)).devices.at(0);
  const DeviceId sub = d.block("atom:sub", r.group), mx = d.block("atom:max", r.group), mul = d.block("atom:mul", r.group);
  r.threshold = d.block(std::string(kValueDevice), r.group);
  r.zero = d.block(std::string(kValueDevice), r.group);
  r.slope = d.block(std::string(kValueDevice), r.group);
  d.setValue(r.threshold, -24.0, pq::dsp::curvePreset(pq::dsp::CurvePreset::Linear, -60.0, 0.0));
  d.setValue(r.zero, 0.0, pq::dsp::curvePreset(pq::dsp::CurvePreset::Linear, -1.0, 1.0));
  d.setValue(r.slope, 0.75, pq::dsp::curvePreset(pq::dsp::CurvePreset::Linear, 0.0, 1.0));
  d.wire(r.group, std::string(kGroupInPort), sub, "a");
  d.wire(r.threshold, "y", sub, "b");
  d.wire(sub, "out", mx, "a");
  d.wire(r.zero, "y", mx, "b");
  d.wire(mx, "out", mul, "a");
  d.wire(r.slope, "y", mul, "b");
  d.wire(mul, "out", r.group, std::string(kGroupOutPort));
  return r;
}

}  // namespace

TEST_CASE("group transfer: the Saturator's Hard Curve group draws v / (1 + v^4)^(1/4) exactly (every channel)") {
  Doc d;
  const DeviceId sat = test_blocks::addModule(d.store, d.track, "saturator");
  const DeviceId hard = test_blocks::member(d.doc(), sat, "Hard Curve");
  REQUIRE(hard.valid());
  GroupTransfer t;
  {
    const pq::Status st = t.prepare(d.doc(), d.lowered(), library(), hard);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  REQUIRE(t.status() == ResponseKind::Exact);
  REQUIRE(t.outChannels() >= 1);
  const std::vector<float> xs = grid();
  const std::vector<float> ys = eval(t, xs);
  for (size_t k = 0; k < xs.size(); ++k) {
    const float v = xs[k], v2 = v * v, v4 = v2 * v2;
    const float want = v / std::sqrt(std::sqrt(1.0f + v4));
    for (uint32_t j = 0; j < t.outChannels(); ++j) CHECK_MESSAGE(ys[k * t.outChannels() + j] == want, "x " << v << " ch " << j);
  }
}

TEST_CASE("group transfer: a group holding z1 between In and Out has no static curve") {
  Doc d;
  const DeviceId g = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.track, std::string(kGroupDevice), DeviceKind::AudioEffect)).devices.at(0);
  const DeviceId add = d.block("atom:add", g), z1 = d.block("atom:z1", g);
  d.wire(g, std::string(kGroupInPort), add, "a");
  d.wire(g, std::string(kGroupInPort), z1, "x");
  d.wire(z1, "out", add, "b");
  d.wire(add, "out", g, std::string(kGroupOutPort));
  GroupTransfer t;
  REQUIRE(t.prepare(d.doc(), d.lowered(), library(), g).ok());
  CHECK(t.status() == ResponseKind::Unavailable);
  const std::vector<float> ys = eval(t, grid(), ResponseKind::Unavailable);
  CHECK(std::isnan(ys.front()));
}

TEST_CASE("group transfer: a gain-reduction group's knobs are re-read by update() (the stamp moves only when a value it "
          "reads does) and equal a relowered evaluation") {
  Doc d;
  const Reduction r = reductionGroup(d);
  GroupTransfer t;
  {
    const pq::Status st = t.prepare(d.doc(), d.lowered(), library(), r.group);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  std::vector<float> xs;
  for (int db = -60; db <= 6; db += 3) xs.push_back(static_cast<float>(db));
  auto check = [&](const std::vector<float>& ys) {
    const float th = d.value(r.threshold), z = d.value(r.zero), s = d.value(r.slope);
    for (size_t k = 0; k < xs.size(); ++k)
      for (uint32_t j = 0; j < t.outChannels(); ++j) CHECK(ys[k * t.outChannels() + j] == std::max(xs[k] - th, z) * s);
  };
  CHECK(d.value(r.threshold) == doctest::Approx(-24.0f));
  check(eval(t, xs));
  // A document change the transfer does not read: the stamp stays (a display does not re-evaluate).
  const uint64_t s0 = t.stamp();
  d.cmd(RenameDevice{r.group, "Gain Computer"});
  t.update(d.doc());
  CHECK(t.stamp() == s0);
  // A knob drag: the stamp moves, the curve follows without a lowering.
  d.setValue(r.slope, 0.5);
  t.update(d.doc());
  CHECK(t.stamp() != s0);
  const std::vector<float> live = eval(t, xs);
  check(live);
  GroupTransfer fresh;
  REQUIRE(fresh.prepare(d.doc(), d.lowered(), library(), r.group).ok());
  CHECK(eval(fresh, xs) == live);
}
