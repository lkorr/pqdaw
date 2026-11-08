// EQ.2: doc::GroupResponse (pq/document/group_response.h): a group's In → Out response from the lowered project; a knob
// change re-evaluated without relowering equals a relowered evaluation.
#include <doctest/doctest.h>

#include <cmath>
#include <complex>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/group_response.h"
#include "pq/document/lower.h"
#include "pq/document/model.h"
#include "pq/document/store.h"

using namespace pq::doc;
using Cx = std::complex<double>;

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
  double value(DeviceId v) const {
    const pq::graph::ParamDecl* decl = findAnyParamDecl(valuePatch(), kValueParam);
    REQUIRE(decl != nullptr);
    return paramValue(*findDevice(doc(), v).device, kValueParam, *decl);
  }
};

const std::vector<double> kHz{50.0, 440.0, 3000.0, 15000.0};
constexpr double kSr = 48000.0;

std::vector<Cx> eval(GroupResponse& g, pq::graph::ResponseKind expect = pq::graph::ResponseKind::Exact) {
  std::vector<Cx> h(kHz.size() * g.inChannels() * g.outChannels());
  CHECK(g.evaluate(kHz, kSr, h) == expect);
  return h;
}

}  // namespace

TEST_CASE("group response: the VCA module (In → mul ← Value → Out) is its value through the curve; a knob change "
          "re-read by update() equals a relowered evaluation") {
  Doc d;
  const DeviceId g = test_blocks::addModule(d.store, d.track, "vca");
  const DeviceId v = test_blocks::member(d.doc(), g, kVcaValueName);
  REQUIRE(v.valid());
  GroupResponse r;
  {
    const pq::Status st = r.prepare(d.doc(), d.lowered(), library(), g);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  REQUIRE(r.inChannels() >= 1);
  auto expectGain = [&](const std::vector<Cx>& h, double gain) {
    const uint32_t I = r.inChannels(), O = r.outChannels();
    for (size_t k = 0; k < kHz.size(); ++k)
      for (uint32_t i = 0; i < I; ++i)
        for (uint32_t j = 0; j < O; ++j)
          CHECK(std::abs(h[(k * I + i) * O + j] - Cx{(I == 1 || O == 1 || i == j) ? gain : 0.0, 0.0}) < 1e-6 * std::max(1.0, std::fabs(gain)));
  };
  expectGain(eval(r), d.value(v));
  // A knob drag: the document changes, the response re-reads it (no lowering).
  d.cmd(SetDeviceParam{v, std::string(kValueParam), 0.9});
  const double after = d.value(v);
  CHECK(std::fabs(after - 1.0) > 1e-3);  // the drag moved the gain away from 1 (the module's default)
  r.update(d.doc());
  const std::vector<Cx> live = eval(r);
  expectGain(live, after);
  GroupResponse fresh;
  REQUIRE(fresh.prepare(d.doc(), d.lowered(), library(), g).ok());
  const std::vector<Cx> relowered = eval(fresh);
  REQUIRE(relowered.size() == live.size());
  for (size_t k = 0; k < live.size(); ++k) CHECK(live[k] == relowered[k]);
}

TEST_CASE("group response: a group of atom blocks with feedback through z1 (In → add → Out, add → z1 → mul ← Value → "
          "add) is 1 / (1 − v·z⁻¹); a value change matches a relowering") {
  Doc d;
  const DeviceId g = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.track, std::string(kGroupDevice), DeviceKind::AudioEffect)).devices.at(0);
  const DeviceId add = d.block("atom:add", g), z1 = d.block("atom:z1", g), mul = d.block("atom:mul", g);
  const DeviceId v = d.block(std::string(kValueDevice), g);
  d.cmd(SetDeviceParam{v, std::string(kValueParam), 0.6});
  d.wire(g, std::string(kGroupInPort), add, "a");
  d.wire(add, "out", z1, "x");
  d.wire(z1, "out", mul, "a");
  d.wire(v, "y", mul, "b");
  d.wire(mul, "out", add, "b");
  d.wire(add, "out", g, std::string(kGroupOutPort));
  GroupResponse r;
  {
    const pq::Status st = r.prepare(d.doc(), d.lowered(), library(), g);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  auto check = [&](const std::vector<Cx>& h, double gain) {
    const uint32_t I = r.inChannels(), O = r.outChannels();
    for (size_t k = 0; k < kHz.size(); ++k) {
      const Cx want = 1.0 / (1.0 - gain * std::polar(1.0, -2.0 * std::numbers::pi * kHz[k] / kSr));
      for (uint32_t i = 0; i < I; ++i)
        for (uint32_t j = 0; j < O; ++j)
          CHECK(std::abs(h[(k * I + i) * O + j] - ((I == 1 || O == 1 || i == j) ? want : Cx{})) < 1e-5 * std::abs(want));
    }
  };
  const double v0 = d.value(v);
  REQUIRE(std::fabs(v0) < 1.0);
  check(eval(r), v0);
  d.cmd(SetDeviceParam{v, std::string(kValueParam), 0.3});
  r.update(d.doc());
  const std::vector<Cx> live = eval(r);
  check(live, d.value(v));
  GroupResponse fresh;
  REQUIRE(fresh.prepare(d.doc(), d.lowered(), library(), g).ok());
  const std::vector<Cx> relowered = eval(fresh);
  for (size_t k = 0; k < live.size(); ++k) CHECK(live[k] == relowered[k]);
}
