// MX.2 (docs/plans/MX.md "MX.2"; pq/document/modules.h "the Math module"; edit_ops.h setMathOp):
//   ops table   keys / codes unique, five categories in menu order, gates and arity as the plan lists them
//   group       its roles (A, B child groups with their values, Tolerance), the op from its setting, the panel's one
//               trace tap (A, B, Out), every wire lowered, spawned through the catalogue key `module:math`
//   swap        setMathOp is one undo step that round-trips save / load; it keeps A, B, Tolerance and the wires into
//               them and from the Out; every op lowers; a bad key or a non-Math group is refused
//   trace tap   the lowering gives the trace element one signal_tap requested under traceTapPath (one resource)
#include <doctest/doctest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "pq/document/change_set.h"
#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/model.h"
#include "pq/document/modules.h"
#include "pq/document/panel.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/dsp/curve.h"
#include "pq/graph/atoms.h"

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
  TrackId track;
  Doc() { track = edit(ops::createTrack(doc(), store.ids(), TrackKind::Audio, {}, "A")).tracks.at(0); }
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(const pq::Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  // `e` as one undo step: undo gives back the project before, redo the one after, which saves and loads as itself.
  ops::Edit step(const pq::Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
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
  MathInfo info(DeviceId g) const {
    const std::optional<MathInfo> m = mathInfo(doc(), g);
    REQUIRE(m.has_value());
    return *m;
  }
  const Device& dev(DeviceId d) const { return *findDevice(doc(), d).device; }
  LoweredProject lowered(bool taps = false) const {
    LowerOptions lo;
    lo.library = &library().library();
    lo.devices = &library();
    lo.clipSlotOf = [](TrackId) { return 0u; };
    if (taps) lo.metaSlotOf = [](DeviceId, std::string_view, uint32_t) { return 7u; };
    auto low = lower(doc(), lo);
    REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
    return std::move(*low);
  }
};

}  // namespace

TEST_CASE("MX.2 Math ops: unique keys and codes, five categories in menu order, gates and arity as the plan lists") {
  const auto ops = mathOpInfos();
  CHECK(ops.size() == 28);
  std::set<std::string_view> keys;
  std::set<uint8_t> codes;
  uint8_t category = 0;
  for (const MathOpInfo& o : ops) {
    CHECK(keys.insert(o.key).second);
    CHECK(codes.insert(o.code).second);
    CHECK(o.code != 0);
    CHECK(static_cast<uint8_t>(o.category) >= category);  // grouped by category, in order
    category = static_cast<uint8_t>(o.category);
    CHECK(findMathOp(o.key) == &o);
    CHECK(mathOpOfCode(o.code) == &o);
    CHECK(o.gate == (o.category == MathCategory::Compare || o.category == MathCategory::Logic));
    CHECK(o.tolerance == (o.key == "eq" || o.key == "ne"));
    CHECK(o.arity == (o.key == "not" ? 1 : 2));
    CHECK_FALSE(o.symbol.empty());
    CHECK_FALSE(o.name.empty());
  }
  CHECK(category == static_cast<uint8_t>(MathCategory::Geometry));
  CHECK(mathCategoryName(MathCategory::Range) == "Range");
  CHECK(findMathOp("ring_mod") == nullptr);  // the plan's left-out audio ops
  CHECK(mathOpOfCode(0.0) == nullptr);
  // A few reference values (the module's documented edge rules).
  CHECK(mathOpReference("div", 1.0, 1e-10, 0.0) == 0.0);
  CHECK(mathOpReference("mod", 2.5, 0.0, 0.0) == 2.5);
  CHECK(mathOpReference("mod", -0.5, 2.0, 0.0) == 1.5);
  CHECK(mathOpReference("pow", 0.0, -2.0, 0.0) == 0.0);
  CHECK(mathOpReference("pow", -8.0, 1.0 / 3.0, 0.0) == doctest::Approx(-2.0));
  CHECK(mathOpReference("wrap", 1.5, 1.0, 0.0) == doctest::Approx(-0.5));
  CHECK(mathOpReference("fold", 1.5, 1.0, 0.0) == doctest::Approx(0.5));
  CHECK(mathOpReference("fold", -3.5, 1.0, 0.0) == doctest::Approx(0.5));
  CHECK(mathOpReference("clip", 3.0, -2.0, 0.0) == 2.0);
  CHECK(mathOpReference("wrap", 3.0, 0.0, 0.0) == 0.0);
  CHECK(mathOpReference("eq", 1.0, 1.0005, 1e-3) == 1.0);
  CHECK(mathOpReference("ne", 1.0, 1.0005, 1e-3) == 0.0);
  CHECK(mathOpReference("not", 0.0, 0.0, 0.0) == 1.0);
}

TEST_CASE("MX.2 Math group: roles, the stored op, the panel's one trace tap, spawned by `module:math`, every wire lowered") {
  Doc d;
  const ModuleInfo* m = findModule("math");
  REQUIRE(m != nullptr);
  CHECK(m->kind == ModuleKind::Math);
  CHECK(m->name == "Math");
  CHECK(moduleEngineType(ModuleKind::Math).empty());
  const DeviceId g = test_blocks::addModule(d.store, d.track, "math", 0);
  const MathInfo mi = d.info(g);
  REQUIRE(mi.op != nullptr);
  CHECK(mi.op->key == kMathDefaultOp);
  CHECK(d.dev(g).name == "Math");
  CHECK(d.dev(mi.roles.inA).group == g);
  CHECK(d.dev(mi.roles.inB).group == g);
  CHECK(d.dev(mi.roles.a).group == mi.roles.inA);
  CHECK(d.dev(mi.roles.b).group == mi.roles.inB);
  CHECK(d.dev(mi.roles.tolerance).group == g);
  CHECK(valueBlockUnits(d.dev(mi.roles.a)) == doctest::Approx(0.0).epsilon(1e-12));
  CHECK(valueBlockUnits(d.dev(mi.roles.b)) == doctest::Approx(0.0).epsilon(1e-12));
  CHECK(valueBlockUnits(d.dev(mi.roles.tolerance)) == doctest::Approx(kMathDefaultTolerance).epsilon(1e-9));
  CHECK(mi.roles.sumA.valid());
  CHECK(mi.roles.sumB.valid());
  REQUIRE(mi.roles.op.size() == 1);  // >: one gt atom
  CHECK(atomDeviceName(d.dev(mi.roles.op[0])) == "gt");
  // The panel: its bindings inside the group; one tap, the trace of A, B and the group's Out.
  REQUIRE(d.dev(g).hasPanel());
  CHECK(bindingsProblem(findDevice(d.doc(), g).track->devices, g, *d.dev(g).panel) == "");
  const std::vector<PanelTap> taps = panelTaps(*d.dev(g).panel);
  REQUIRE(taps.size() == 1);
  CHECK(taps[0].device == mi.roles.inA);
  REQUIRE(taps[0].traces.size() == 2);
  CHECK(taps[0].traces[0].first == mi.roles.inB);
  CHECK(taps[0].traces[1].first == g);
  CHECK(traceTapPath(taps[0]).rfind("#trace:y|", 0) == 0);
  for (const WireState& s : wireStates(d.doc(), library())) CHECK(s.status == WireStatus::Lowered);
  d.lowered();
}

TEST_CASE("MX.2 Math swap: one undo step, save / load round trip; A, B, Tolerance and the outside wires stay; every op "
          "lowers; refusals") {
  Doc d;
  const DeviceId g = test_blocks::addModule(d.store, d.track, "math", 0);
  const MathInfo before = d.info(g);
  // Outside wires: a value block into B's input, A's value block driven by another, the Out into a free VCA.
  const DeviceId src = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.track, std::string(kValueDevice), DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  d.edit(ops::connectWire(d.doc(), d.store.ids(), src, std::string(kValueOutput), before.roles.inB, std::string(kMainPort)));
  d.edit(ops::connectWire(d.doc(), d.store.ids(), src, std::string(kValueOutput), before.roles.a, std::string(kValueParam)));
  const DeviceId vca = test_blocks::addModule(d.store, d.track, "vca", ops::kOffPath);
  d.edit(ops::connectWire(d.doc(), d.store.ids(), g, std::string(kMainPort), vca, std::string(kMainPort)));
  const auto outside = [&] {
    std::vector<WireId> w;
    for (const Wire& x : wiresOf(d.doc()))
      if (x.from == src || (x.from == g && x.to == vca)) w.push_back(x.id);
    std::sort(w.begin(), w.end());
    return w;
  };
  const std::vector<WireId> kept = outside();
  REQUIRE(kept.size() == 3);

  const ops::Edit e = d.step(ops::setMathOp(d.doc(), d.store.ids(), g, "hypot"));
  CHECK(d.store.lastChanges().structural());
  MathInfo after = d.info(g);
  REQUIRE(after.op != nullptr);
  CHECK(after.op->key == "hypot");
  CHECK(after.roles.inA == before.roles.inA);
  CHECK(after.roles.inB == before.roles.inB);
  CHECK(after.roles.a == before.roles.a);
  CHECK(after.roles.b == before.roles.b);
  CHECK(after.roles.tolerance == before.roles.tolerance);
  CHECK(findDevice(d.doc(), before.roles.op[0]).device == nullptr);  // the old gt atom is gone
  std::vector<DeviceId> fresh = after.roles.op, made = e.devices;
  std::sort(fresh.begin(), fresh.end());
  std::sort(made.begin(), made.end());
  CHECK(fresh == made);
  CHECK(outside() == kept);
  // hypot = sqrt(muladd(A, A, B · B)): three atoms.
  std::multiset<std::string> atoms;
  for (const DeviceId x : after.roles.op) atoms.insert(std::string(atomDeviceName(d.dev(x))));
  CHECK(atoms == std::multiset<std::string>{"mul", "muladd", "sqrt"});
  // Undo restores the old op exactly (step() checked the whole project).
  REQUIRE(d.store.undo());
  CHECK(d.info(g).op->key == "gt");
  REQUIRE(d.store.redo());

  // Every op: swapped in one step, a minimal subgraph, lowered with every wire.
  for (const MathOpInfo& op : mathOpInfos()) {
    CAPTURE(op.key);
    d.edit(ops::setMathOp(d.doc(), d.store.ids(), g, op.key));
    const MathInfo mi = d.info(g);
    CHECK(mi.op == &op);
    CHECK(mi.roles.op.size() <= 16);
    CHECK(outside() == kept);
    for (const WireState& s : wireStates(d.doc(), library())) CHECK(s.status == WireStatus::Lowered);
    d.lowered();
  }
  CHECK_FALSE(ops::setMathOp(d.doc(), d.store.ids(), g, "ring_mod").ok());
  CHECK_FALSE(ops::setMathOp(d.doc(), d.store.ids(), vca, "add").ok());
  // A hand-edited op (an extra block inside) is rebuilt by picking an op again: the stored op is what the face names.
  const DeviceId extra = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.track, std::string(kAtomDevicePrefix) + "neg", DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  REQUIRE(d.store.dispatch(SetDeviceGroup{extra, g}).ok());
  const std::vector<DeviceId> edited = d.info(g).roles.op;
  CHECK(std::count(edited.begin(), edited.end(), extra) == 1);
  d.step(ops::setMathOp(d.doc(), d.store.ids(), g, "add"));
  CHECK(findDevice(d.doc(), extra).device == nullptr);
  CHECK(d.info(g).roles.op.size() == 1);
}

TEST_CASE("MX.2 trace tap: the lowering gives the trace element one signal_tap under traceTapPath") {
  Doc d;
  const DeviceId g = test_blocks::addModule(d.store, d.track, "math", 0);
  const MathInfo mi = d.info(g);
  const LoweredProject low = d.lowered(true);
  std::vector<std::string> paths;
  for (const LoweredProject::MetaResource& r : low.metaResources)
    if (r.device == mi.roles.inA) paths.push_back(r.path);
  REQUIRE(paths.size() == 1);
  const std::vector<PanelTap> taps = panelTaps(*d.dev(g).panel);
  REQUIRE(taps.size() == 1);
  CHECK(paths[0] == traceTapPath(taps[0]));
  CHECK(paths[0] == "#trace:y|" + std::to_string(mi.roles.inB.value) + ":y|" + std::to_string(g.value) + ":y");
  size_t sinks = 0;
  for (const auto& lp : low.patches)
    for (pq::graph::NodeIndex n = 0; n < lp->patch.nodeCount(); ++n)
      if (lp->patch.node(n).kind == pq::graph::NodeKind::Atom && pq::graph::atomSignature(lp->patch.node(n).atom).name == "signal_tap") ++sinks;
  CHECK(sinks == 1);
}

// MX.4 (QUESTIONS Q?-BR.B-b, bug report 2026-11-19_19-50-41: clip with B = 0 clipped a feedback loop to silence): each op
// has a B default where 0 is degenerate (MathOpInfo::bDefault); picking an op moves B to it only while B is still the old
// op's default — in the op swap's one undo step — and a B the user set is kept.
TEST_CASE("MX.4 Math B defaults: the op's own where 0 is degenerate; a swap moves B only while it is the old op's default, "
          "in the same undo step; a set B is kept") {
  // The table: 1 where B = 0 makes the op a constant or near-constant, 0 elsewhere.
  for (const MathOpInfo& op : mathOpInfos()) {
    CAPTURE(op.key);
    const bool one = op.key == "mul" || op.key == "div" || op.key == "pow" || op.key == "and" || op.key == "nand" ||
                     op.key == "clip" || op.key == "wrap" || op.key == "fold" || op.key == "atan2";
    CHECK(op.bDefault == (one ? 1.0 : 0.0));
    // With B at its default the op is not a constant over a typical signal (−½ … ½).
    std::set<double> outs;
    for (const double a : {-0.5, -0.25, 0.0, 0.25, 0.5}) outs.insert(mathOpReference(op.key, a, op.bDefault, kMathDefaultTolerance));
    CHECK(outs.size() >= 2);
  }
  // clip passes a ±1 signal at its default (the report's feedback loop keeps feeding back).
  CHECK(mathOpReference("clip", 0.7, findMathOp("clip")->bDefault, 0.0) == doctest::Approx(0.7));

  Doc d;
  const DeviceId g = test_blocks::addModule(d.store, d.track, "math", 0);
  const DeviceId b = d.info(g).roles.b;
  auto bUnits = [&] { return valueBlockUnits(d.dev(b)); };
  REQUIRE(bUnits() == doctest::Approx(0.0).epsilon(1e-12));  // gt: 0
  d.step(ops::setMathOp(d.doc(), d.store.ids(), g, "clip"));  // (step: one undo step, undo gives B 0 back exactly)
  CHECK(bUnits() == doctest::Approx(1.0).epsilon(1e-9));
  REQUIRE(d.store.undo());
  CHECK(bUnits() == doctest::Approx(0.0).epsilon(1e-12));
  CHECK(d.info(g).op->key == "gt");
  REQUIRE(d.store.redo());
  d.step(ops::setMathOp(d.doc(), d.store.ids(), g, "mul"));  // 1 → 1: nothing to move
  CHECK(bUnits() == doctest::Approx(1.0).epsilon(1e-9));
  d.step(ops::setMathOp(d.doc(), d.store.ids(), g, "add"));  // still mul's default: to add's 0
  CHECK(bUnits() == doctest::Approx(0.0).epsilon(1e-9));
  // A B the user set stays through every swap.
  const pq::dsp::Curve& c = *d.dev(b).curve(kValueParam);
  d.edit(ops::setDeviceParam(d.doc(), b, std::string(kValueParam), pq::dsp::curvePosition(c, pq::dsp::curveOutput(c, 0.25), 0.5)));
  REQUIRE(bUnits() == doctest::Approx(0.25).epsilon(1e-6));
  for (const char* key : {"clip", "add", "atan2", "gt"}) {
    d.edit(ops::setMathOp(d.doc(), d.store.ids(), g, key));
    CHECK(bUnits() == doctest::Approx(0.25).epsilon(1e-6));
  }
  // A Math spawned with an op starts at that op's B.
  IdSource ids;
  MathRoles r;
  MathOptions o;
  o.op = "clip";
  const BlockSet set = mathBlocks(ids, o, &r);
  for (const DevicePtr& x : set.devices)
    if (x->id == r.b) CHECK(valueBlockUnits(*x) == doctest::Approx(1.0).epsilon(1e-9));
}
