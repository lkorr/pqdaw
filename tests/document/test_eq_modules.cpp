// EQ.3 (docs/plans/EQ.md "Structure", "Ops"; pq/document/modules.h "the band and the EQ"; edit_ops.h "EQ.3"):
//   band        its roles, the Frequency curve on the display axis, Type and Stages structural (their wires into the
//               svf's const inputs lowered), a wire into a const input from anything else left out
//   ops         each one undo step that undoes exactly and round-trips save / load; the 33rd band refused; solo / unsolo
//               restores the bypass states; a Type change is a structural change
//   response    the EQ group's In → Out (GroupResponse) in Stereo / L/R / M/S is the hand-computed matrix around the
//               chains' svfResponse products (the default four bands with gains set; chain B a copy with other gains)
#include <doctest/doctest.h>

#include <cmath>
#include <complex>
#include <memory>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "pq/document/change_set.h"
#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/group_response.h"
#include "pq/document/lower.h"
#include "pq/document/model.h"
#include "pq/document/modules.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/dsp/svf_design.h"

using namespace pq::doc;
using Cx = std::complex<double>;
namespace dsp = pq::dsp;

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
  // `e` dispatched as one undo step: the history grows by one, undo gives back exactly the project before, redo the
  // one after, and the project after saves and loads as itself.
  ops::Edit step(const pq::Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE_FALSE(e->empty());
    const Snapshot before = store.current();
    const size_t cursor = store.history().cursor();
    REQUIRE(store.dispatch(e->command).ok());
    CHECK(store.history().cursor() == cursor + 1);
    const Snapshot after = store.current();
    REQUIRE(store.undo());
    CHECK(*store.current() == *before);
    REQUIRE(store.redo());
    CHECK(*store.current() == *after);
    pq::Result<std::string> text = serializeProject(doc());
    REQUIRE_MESSAGE(text.ok(), (text.ok() ? std::string() : text.error().message));
    pq::Result<LoadedProject> back = parseProject(*text);
    REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
    CHECK(*back->project == doc());
    return *e;
  }
  DeviceId eq() { return test_blocks::addModule(store, track, "eq"); }
  EqInfo info(DeviceId eq) const {
    std::optional<EqInfo> e = eqInfo(doc(), eq);
    REQUIRE(e.has_value());
    return *e;
  }
  const Device& dev(DeviceId d) const { return *findDevice(doc(), d).device; }
  double units(DeviceId v) const { return valueBlockUnits(dev(v)); }
  // A value block set to `value` in its units.
  void set(DeviceId v, double value) {
    const dsp::Curve& c = *dev(v).curve(kValueParam);
    REQUIRE(store.dispatch(SetDeviceParam{v, std::string(kValueParam), dsp::curvePosition(c, dsp::curveOutput(c, value), 0.0)}).ok());
  }
  // The block's runtime value (its param: the curve at the knob, as float).
  float runtime(DeviceId v) const {
    const pq::graph::ParamDecl* decl = findAnyParamDecl(valuePatch(), kValueParam);
    REQUIRE(decl != nullptr);
    return static_cast<float>(paramValue(dev(v), kValueParam, *decl));
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
  std::vector<double> freqs(const std::vector<BandInfo>& bands) const {
    std::vector<double> f;
    for (const BandInfo& b : bands) f.push_back(units(b.frequency));
    return f;
  }
};

bool near(const std::vector<double>& got, const std::vector<double>& want) {
  if (got.size() != want.size()) return false;
  for (size_t i = 0; i < got.size(); ++i)
    if (std::fabs(got[i] - want[i]) > 1e-6 * std::max(1.0, std::fabs(want[i]))) return false;
  return true;
}

}  // namespace

TEST_CASE("EQ.3 band: roles, the Frequency curve is the display axis, Type and Stages set the svf's const inputs") {
  // The curve: f = 10 Hz · (2001^t − 1), i.e. t = log(1 + f / 10) / log(2001).
  const dsp::Curve fc = bandFrequencyCurve();
  for (const double t : {0.0, 0.1, 0.37, 0.5, 0.9, 1.0}) {
    const double want = 10.0 * (std::pow(2001.0, t) - 1.0);
    CHECK(dsp::evalCurve(fc, t) == doctest::Approx(want).epsilon(1e-12));
  }
  CHECK(dsp::curvePosition(fc, 1000.0) == doctest::Approx(std::log(101.0) / std::log(2001.0)).epsilon(1e-12));
  Doc d;
  const DeviceId band = test_blocks::addModule(d.store, d.track, "eq_band");
  const std::optional<BandInfo> b = bandInfo(d.doc(), band);
  REQUIRE(b.has_value());
  REQUIRE(b->q.valid());
  REQUIRE(b->gain.valid());
  REQUIRE(b->stages.valid());
  REQUIRE(b->type.valid());
  CHECK(d.units(b->frequency) == doctest::Approx(1000.0));
  CHECK(d.units(b->q) == doctest::Approx(0.70710678118654752));
  CHECK(d.units(b->gain) == doctest::Approx(0.0).epsilon(1e-9));
  CHECK(d.units(b->stages) == 1.0);
  CHECK(d.units(b->type) == 4.0);  // Bell
  // Type and Stages are structural (their knob recompiles); Frequency, Q and Gain are params.
  CHECK(lowersAsLiteral(d.doc(), d.dev(b->type)));
  CHECK(lowersAsLiteral(d.doc(), d.dev(b->stages)));
  CHECK_FALSE(lowersAsLiteral(d.doc(), d.dev(b->frequency)));
  CHECK_FALSE(lowersAsLiteral(d.doc(), d.dev(b->gain)));
  // Every wire of the band lowers (the const inputs' too).
  for (const WireState& s : wireStates(d.doc(), library())) CHECK(s.status == WireStatus::Lowered);
  // A Type change is one structural step.
  d.edit(ops::setBandType(d.doc(), band, 0));
  CHECK(d.store.lastChanges().structural());
  CHECK(d.units(b->type) == 0.0);
  // A wire INTO Type: it stays structural (the const input takes the knob; the wire is ignored there).
  d.edit(ops::connectWire(d.doc(), d.store.ids(), b->q, std::string(kValueOutput), b->type, std::string(kValueParam)));
  CHECK(lowersAsLiteral(d.doc(), d.dev(b->type)));
  // A wire into a const input from anything but a stepped value block is left out (Invalid).
  DeviceId svf;
  for (const DevicePtr& x : findDevice(d.doc(), band).track->devices)
    if (x->group == band && isAtomDevice(*x)) svf = x->id;
  REQUIRE(svf.valid());
  const WireId bad = d.edit(ops::connectWire(d.doc(), d.store.ids(), b->q, std::string(kValueOutput), svf, "prewarp")).wires.at(0);
  for (const WireState& s : wireStates(d.doc(), library()))
    if (s.id == bad) CHECK(s.status == WireStatus::Invalid);
  d.lowered();  // (still lowers)
}

TEST_CASE("EQ.3 EQ: the default (4 bands in A in frequency order, B empty) and every op is one undo step that round-trips") {
  Doc d;
  const DeviceId eq = d.eq();
  EqInfo e = d.info(eq);
  REQUIRE(e.mode.valid());
  REQUIRE(e.output.valid());
  REQUIRE(e.editChain.valid());
  REQUIRE(e.bands[0].size() == 4);
  CHECK(e.bands[1].empty());
  CHECK(near(d.freqs(e.bands[0]), {100.0, 400.0, 2500.0, 8000.0}));
  CHECK(d.units(e.bands[0][0].type) == 5.0);  // low shelf
  CHECK(d.units(e.bands[0][1].type) == 4.0);  // bell
  CHECK(d.units(e.bands[0][3].type) == 6.0);  // high shelf
  CHECK(lowersAsLiteral(d.doc(), d.dev(e.mode)));
  for (const WireState& s : wireStates(d.doc(), library())) CHECK(s.status == WireStatus::Lowered);
  const size_t elements = d.dev(eq).panel->elements.size();

  // addBand: a bell at 1.5 kHz lands between 400 Hz and 2.5 kHz; the strip grows by one column.
  const DeviceId added = d.step(ops::addBand(d.doc(), d.store.ids(), eq, 0, 4, 1500.0, 3.0)).devices.at(0);
  e = d.info(eq);
  REQUIRE(e.bands[0].size() == 5);
  CHECK(e.bands[0][2].group == added);
  CHECK(d.units(e.bands[0][2].gain) == doctest::Approx(3.0));
  CHECK(d.dev(eq).panel->elements.size() == elements + 5);  // type, Hz, Q, gain, stages
  CHECK(bindingsProblem(findDevice(d.doc(), eq).track->devices, eq, *d.dev(eq).panel) == "");
  // setBandType / setBandStages.
  d.step(ops::setBandType(d.doc(), added, 1));
  CHECK(d.units(e.bands[0][2].type) == 1.0);
  CHECK(d.store.lastChanges().structural());
  d.step(ops::setBandStages(d.doc(), added, 4));
  CHECK(d.units(e.bands[0][2].stages) == 4.0);
  CHECK(d.store.lastChanges().structural());
  CHECK_FALSE(ops::setBandStages(d.doc(), added, 65).ok());
  // setBandBypass.
  d.step(ops::setBandBypass(d.doc(), added, true));
  CHECK_FALSE(d.dev(added).active);
  // setEqMode: the first split copies A (five bands, fresh ids) into B.
  d.step(ops::setEqMode(d.doc(), d.store.ids(), eq, EqMode::MidSide));
  e = d.info(eq);
  CHECK(d.units(e.mode) == 2.0);
  REQUIRE(e.bands[1].size() == 5);
  CHECK(near(d.freqs(e.bands[1]), d.freqs(e.bands[0])));
  for (size_t i = 0; i < 5; ++i) CHECK(e.bands[1][i].group != e.bands[0][i].group);
  CHECK_FALSE(d.dev(e.bands[1][2].group).active);  // (the copy keeps the bypass)
  CHECK(d.dev(eq).panel->elements.size() == elements + 5 + 5 * 5);
  // A second switch copies nothing more.
  d.step(ops::setEqMode(d.doc(), d.store.ids(), eq, EqMode::LeftRight));
  CHECK(d.info(eq).bands[1].size() == 5);
  // removeBand: its chain closes over it, its strip column goes.
  d.step(ops::removeBand(d.doc(), d.store.ids(), added));
  e = d.info(eq);
  CHECK(e.bands[0].size() == 4);
  CHECK(near(d.freqs(e.bands[0]), {100.0, 400.0, 2500.0, 8000.0}));
  CHECK(d.dev(eq).panel->elements.size() == elements + 5 * 5);
  CHECK(bindingsProblem(findDevice(d.doc(), eq).track->devices, eq, *d.dev(eq).panel) == "");
  // (B's copy of the bypassed band is off: its inner wires wait, Inactive)
  for (const WireState& s : wireStates(d.doc(), library()))
    CHECK((s.status == WireStatus::Lowered || s.status == WireStatus::Inactive));
  d.lowered();
}

TEST_CASE("EQ.3 EQ: a chain holds at most 32 bands (the 33rd is refused)") {
  Doc d;
  const DeviceId eq = d.eq();
  for (uint32_t i = 4; i < kMaxBandsPerChain; ++i) d.edit(ops::addBand(d.doc(), d.store.ids(), eq, 0, 4, 50.0 + 400.0 * i, 0.0));
  CHECK(d.info(eq).bands[0].size() == kMaxBandsPerChain);
  const auto refused = ops::addBand(d.doc(), d.store.ids(), eq, 0, 4, 1000.0, 0.0);
  REQUIRE_FALSE(refused.ok());
  CHECK(refused.error().message.find("32") != std::string::npos);
  // (frequency order held throughout)
  const std::vector<double> f = d.freqs(d.info(eq).bands[0]);
  for (size_t i = 1; i < f.size(); ++i) CHECK(f[i - 1] <= f[i]);
  d.lowered();
}

TEST_CASE("EQ.3 EQ: solo holds the chain's other bands off and unsolo restores each one's state (undo and save keep it)") {
  Doc d;
  const DeviceId eq = d.eq();
  const EqInfo e = d.info(eq);
  const DeviceId b0 = e.bands[0][0].group, b1 = e.bands[0][1].group, b2 = e.bands[0][2].group, b3 = e.bands[0][3].group;
  d.edit(ops::setBandBypass(d.doc(), b1, true));
  d.step(ops::setBandSolo(d.doc(), d.info(eq).bands[0][2].group, true));
  CHECK_FALSE(d.dev(b0).active);
  CHECK_FALSE(d.dev(b1).active);
  CHECK(d.dev(b2).active);
  CHECK_FALSE(d.dev(b3).active);
  // Bypass while held off changes what unsolo restores; soloing another band moves the solo.
  d.step(ops::setBandBypass(d.doc(), b3, true));
  CHECK_FALSE(d.dev(b3).active);
  d.step(ops::setBandSolo(d.doc(), b0, true));
  CHECK(d.dev(b0).active);
  CHECK_FALSE(d.dev(b2).active);
  d.step(ops::setBandSolo(d.doc(), b0, false));
  CHECK(d.dev(b0).active);
  CHECK_FALSE(d.dev(b1).active);  // bypassed before the solo
  CHECK(d.dev(b2).active);
  CHECK_FALSE(d.dev(b3).active);  // bypassed during it
  for (const DeviceId b : {b0, b1, b2, b3}) {
    CHECK(d.dev(b).setting(kSoloRestoreSetting) == nullptr);
    CHECK(d.dev(b).setting(kBandSoloSetting) == nullptr);
  }
  // Unsoloing a band that is not soloed changes nothing.
  CHECK(ops::setBandSolo(d.doc(), b0, false)->empty());
}

TEST_CASE("EQ.3 EQ response: Stereo / L/R / M/S are the hand-computed matrices around the chains' svf products") {
  Doc d;
  const DeviceId eq = d.eq();
  // Gains on A's bands, then the split (B a copy) with other gains on B.
  const double gainsA[] = {4.0, -6.0, 3.5, -2.0}, gainsB[] = {-3.0, 5.0, 0.0, 6.0};
  for (size_t i = 0; i < 4; ++i) d.set(d.info(eq).bands[0][i].gain, gainsA[i]);
  d.edit(ops::setEqMode(d.doc(), d.store.ids(), eq, EqMode::LeftRight));
  for (size_t i = 0; i < 4; ++i) d.set(d.info(eq).bands[1][i].gain, gainsB[i]);
  constexpr double kSr = 48000.0;
  const std::vector<double> hz{20.0, 90.0, 400.0, 1000.0, 2500.0, 7000.0, 15000.0};
  // A chain's response: the product of its bands' svfResponse at their runtime input values.
  auto chain = [&](uint32_t c, double f) {
    Cx h{1.0, 0.0};
    for (const BandInfo& b : d.info(eq).bands[c]) {
      const dsp::SvfParams sp = dsp::svfParamsFromInputs(d.runtime(b.frequency), d.runtime(b.q), d.runtime(b.gain),
                                                         d.runtime(b.type), d.runtime(b.stages), 0.0f, kSr);
      h *= dsp::svfResponse(sp, 2.0 * 3.14159265358979323846 * f / kSr);
    }
    return h;
  };
  for (const EqMode mode : {EqMode::Stereo, EqMode::LeftRight, EqMode::MidSide}) {
    CAPTURE(static_cast<int>(mode));
    d.set(d.info(eq).mode, static_cast<double>(mode));
    GroupResponse r;
    const pq::Status st = r.prepare(d.doc(), d.lowered(), library(), eq);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    REQUIRE(r.inChannels() == 2);
    REQUIRE(r.outChannels() == 2);
    std::vector<Cx> h(hz.size() * 4);
    CHECK(r.evaluate(hz, kSr, h) != pq::graph::ResponseKind::Unavailable);
    for (size_t k = 0; k < hz.size(); ++k) {
      const Cx a = chain(0, hz[k]), b = chain(1, hz[k]);
      Cx want[2][2];
      if (mode == EqMode::Stereo) want[0][0] = want[1][1] = a, want[0][1] = want[1][0] = 0.0;
      if (mode == EqMode::LeftRight) want[0][0] = a, want[1][1] = b, want[0][1] = want[1][0] = 0.0;
      if (mode == EqMode::MidSide) want[0][0] = want[1][1] = 0.5 * (a + b), want[0][1] = want[1][0] = 0.5 * (a - b);
      for (uint32_t i = 0; i < 2; ++i)
        for (uint32_t j = 0; j < 2; ++j) {
          CAPTURE(hz[k]);
          CAPTURE(i);
          CAPTURE(j);
          CHECK(std::abs(h[(k * 2 + i) * 2 + j] - want[i][j]) < 1e-9 * std::max(1.0, std::abs(want[i][j])));
        }
    }
  }
}
