// T4.4-fix1: user devices (SPEC §15 2026-10-26) in the document — fork on the first edit (one undo step, undo restores
// the stock reference, other instances and the stock patch unchanged), Revert to Stock, the project file (schema 5),
// the device library's resolution (NodeIds carried from the stock file / the editor's parse, renamed patches, the last
// good text on an error) and the lowering (only the edited track's patch changes; a layout or comment edit changes
// nothing), plus the store's coalescing dispatch the circuit editor's typing uses.

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/graph/parse.h"

using namespace pq::doc;
using pq::Result;

namespace {

std::string stdlibDir() { return std::string(PQ_TEST_DATA_DIR) + "/../stdlib"; }
std::string fixturePath(const std::string& rel) { return std::string(PQ_TEST_DATA_DIR) + "/document/fixtures/" + rel; }

std::unique_ptr<DeviceLibrary> freshLibrary() {
  auto r = loadDeviceLibrary(stdlibDir());
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return std::move(*r);
}

std::string replaced(std::string s, const std::string& from, const std::string& to) {
  const size_t at = s.find(from);
  REQUIRE_MESSAGE(at != std::string::npos, from);
  s.replace(at, from.size(), to);
  return s;
}

// Two MIDI tracks with BasicSynth, a Saturator on the first.
struct Two {
  DocumentStore store;
  TrackId a, b;
  DeviceId synA, synB, sat;
  explicit Two() {
    auto run = [&](Result<ops::Edit> e) {
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
      REQUIRE(store.dispatch(e->command).ok());
      return *e;
    };
    a = run(ops::createTrack(*store.current(), store.ids(), TrackKind::Midi)).tracks.at(0);
    b = run(ops::createTrack(*store.current(), store.ids(), TrackKind::Midi)).tracks.at(0);
    synA = findTrackPtr(*store.current(), a)->devices.at(0)->id;
    synB = findTrackPtr(*store.current(), b)->devices.at(0)->id;
    sat = run(ops::addDevice(*store.current(), store.ids(), a, "saturator", DeviceKind::AudioEffect)).devices.at(0);
  }
  const Device& dev(DeviceId d) const { return *findDevice(*store.current(), d).device; }
};

// The BasicSynth text with its level default changed (a structural edit of one constant).
std::string louderSynth(const DeviceLibrary& lib) {
  return replaced(lib.stockText("BasicSynth"), "param level   = 0.2   [0..1]", "param level   = 0.3   [0..1]");
}

const LoweredParam* paramOf(const LoweredProject& lp, const std::string& key) {
  for (const LoweredParam& p : lp.params)
    if (p.key == key) return &p;
  return nullptr;
}

uint32_t graphIndex(const LoweredProject& lp, TrackId t) {
  for (uint32_t i = 0; i < lp.trackIds.size(); ++i)
    if (lp.trackIds[i] == t) return i;
  FAIL("track not lowered");
  return 0;
}

}  // namespace

TEST_CASE("user devices: the first edit forks the instance (one undo step; undo restores the stock reference)") {
  auto lib = freshLibrary();
  Two t;
  const std::string stock = lib->stockText("BasicSynth");
  REQUIRE_FALSE(stock.empty());
  const Snapshot before = t.store.current();
  const size_t steps = t.store.history().cursor();

  auto e = ops::editDevicePatch(*before, t.synA, louderSynth(*lib));
  REQUIRE(e.ok());
  REQUIRE(t.store.dispatch(e->command).ok());
  CHECK(t.store.history().cursor() == steps + 1);
  CHECK(t.store.history().undoLabel() == "Edit Device Patch");
  CHECK(t.store.lastChanges().has(kChangeStructural));
  CHECK(t.store.lastChanges().devices == std::vector<DeviceId>{t.synA});
  // The instance owns its text; the other instance and the stock file are untouched.
  REQUIRE(t.dev(t.synA).edited());
  CHECK(t.dev(t.synA).source->text == louderSynth(*lib));
  CHECK(t.dev(t.synA).patch == "BasicSynth");  // still names what it was forked from
  CHECK(t.dev(t.synA).id == t.synA);
  CHECK_FALSE(t.dev(t.synB).edited());
  CHECK((findTrackPtr(*t.store.current(), t.b) == findTrackPtr(*before, t.b)));  // pointer-equal: untouched
  CHECK(lib->stockText("BasicSynth") == stock);
  CHECK(freshLibrary()->stockText("BasicSynth") == stock);  // nothing was written to the stdlib

  // A second edit is another step; an unchanged text is an empty edit.
  auto same = ops::editDevicePatch(*t.store.current(), t.synA, louderSynth(*lib));
  REQUIRE(same.ok());
  CHECK(same->empty());

  // Undo: the stock reference again (the very snapshot from before the edit).
  REQUIRE(t.store.undo());
  CHECK((t.store.current() == before));
  CHECK_FALSE(t.dev(t.synA).edited());
  REQUIRE(t.store.redo());
  CHECK(t.dev(t.synA).edited());

  // Revert to Stock: one step; stock again; undo brings the text back.
  auto rev = ops::revertDevicesToStock(*t.store.current(), std::vector<DeviceId>{t.synA, t.synB});
  REQUIRE(rev.ok());
  CHECK(rev->batch().commands.size() == 1);  // synB is not edited
  REQUIRE(t.store.dispatch(rev->command).ok());
  CHECK_FALSE(t.dev(t.synA).edited());
  REQUIRE(t.store.undo());
  CHECK(t.dev(t.synA).source->text == louderSynth(*lib));
  // A duplicate of an edited device is edited too (it shares the text).
  REQUIRE(t.store.dispatch(ops::editDevicePatch(*t.store.current(), t.sat, "// x\n" + lib->stockText("saturator"))->command).ok());
  auto dup = ops::duplicateDevices(*t.store.current(), t.store.ids(), std::vector<DeviceId>{t.sat});
  REQUIRE(dup.ok());
  REQUIRE(t.store.dispatch(dup->command).ok());
  REQUIRE(dup->devices.size() == 1);
  CHECK((t.dev(dup->devices[0]).source == t.dev(t.sat).source));
  CHECK(ops::editDevicePatch(*t.store.current(), t.synA, "").error().code == pq::ErrorCode::InvalidArgument);
}

TEST_CASE("user devices: the project file keeps a device's own text (schema 5), line by line") {
  auto lib = freshLibrary();
  Two t;
  // CRLF lines, a non-ASCII comment, quotes and backslashes in a comment, no trailing newline.
  std::string text = replaced(lib->stockText("saturator"), "patch saturator {",
                              "// Ä edited \"copy\" with a \\ backslash\r\npatch saturator {");
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  REQUIRE(t.store.dispatch(ops::editDevicePatch(*t.store.current(), t.sat, text)->command).ok());
  REQUIRE(t.store.dispatch(ops::editDevicePatch(*t.store.current(), t.synB, louderSynth(*lib) + "\n")->command).ok());
  Result<std::string> saved = serializeProject(*t.store.current(), t.store.view());
  REQUIRE(saved.ok());
  CHECK(saved->rfind("{\n  \"schemaVersion\": 5,", 0) == 0);
  CHECK(saved->find("\"source\": [") != std::string::npos);
  CHECK(saved->find("\"patch saturator {\"") != std::string::npos);  // one JSON string per line
  Result<LoadedProject> back = parseProject(*saved);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == *t.store.current());
  const Device& sat = *findDevice(*back->project, t.sat).device;
  REQUIRE(sat.edited());
  CHECK(sat.source->text == text);
  CHECK(findDevice(*back->project, t.synB).device->source->text == louderSynth(*lib) + "\n");
  CHECK_FALSE(findDevice(*back->project, t.synA).device->edited());
  CHECK((sat.source->ids == nullptr));  // the editor's ids are not saved: the library resolves the text again
  // Saved again: byte-identical.
  Result<std::string> again = serializeProject(*back->project, back->view);
  REQUIRE(again.ok());
  CHECK(*again == *saved);
  // An empty source is refused.
  std::string broken = *saved;
  const size_t at = broken.find("\"source\": [");
  const size_t end = broken.find(']', at);
  broken.replace(at, end + 1 - at, "\"source\": []");
  CHECK_FALSE(parseProject(broken).ok());
}

// fixtures/schema4 is fixtures/full as the schema-4 build wrote it (main at bd62cfd). Schema 5 only adds the optional
// per-device "source", so the migrated project is the rich project itself and re-saves as fixtures/full.
TEST_CASE("user devices: a schema-4 project loads unchanged into schema 5 (every device stock)") {
  Result<LoadedProject> r = loadProject(fixturePath("schema4"));
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  CHECK(r->fileSchemaVersion == 4);
  forEachTrack(*r->project, [](const Track& t, const Track*) {
    for (const DevicePtr& d : t.devices) CHECK_FALSE(d->edited());
  });
  Result<LoadedProject> full = loadProject(fixturePath("full"));
  REQUIRE(full.ok());
  CHECK(*r->project == *full->project);
  Result<std::string> again = serializeProject(*r->project, r->view);
  REQUIRE(again.ok());
  Result<std::string> fullText = serializeProject(*full->project, full->view);
  REQUIRE(fullText.ok());
  CHECK(*again == *fullText);
}

TEST_CASE("user devices: resolution carries NodeIds, renames apart, keeps the last good text") {
  auto lib = freshLibrary();
  Two t;
  REQUIRE(t.store.dispatch(ops::editDevicePatch(*t.store.current(), t.synA, louderSynth(*lib))->command).ok());
  const Device& a = t.dev(t.synA);
  CHECK((lib->resolve(t.dev(t.synB)) == nullptr));  // stock
  auto r = lib->resolve(a);
  REQUIRE((r != nullptr));
  CHECK(r->error.empty());
  REQUIRE((r->graph != nullptr));
  CHECK_FALSE(r->stale);
  CHECK(r->graph->root.rfind("BasicSynth__u" + std::to_string(t.synA.value) + "_", 0) == 0);
  REQUIRE(r->root() != nullptr);
  CHECK(r->root()->name() == r->graph->root);
  CHECK(r->info.name == "Basic Synth");
  CHECK(r->info.kind == DeviceKind::Instrument);
  CHECK(r->info.param("level")->defaultValue == doctest::Approx(0.3));
  CHECK((lib->resolve(a) == r));  // cached
  // Every node of the stock patch kept its NodeId (the edit changed a declaration's value, not the graph's shape).
  const pq::graph::Patch* stock = lib->library().find("BasicSynth");
  REQUIRE(stock != nullptr);
  const pq::graph::Patch& mine = *r->parsed.find("BasicSynth");
  REQUIRE(mine.nodeCount() == stock->nodeCount());
  for (pq::graph::NodeIndex v = 0; v < stock->nodeCount(); ++v) {
    CAPTURE(stock->describe(v));
    CHECK(mine.indexOf(stock->node(v).id).has_value());
  }
  // A comment / layout-only edit is a new text with the same graph: the same renamed patches (no recompile).
  REQUIRE(t.store.dispatch(ops::editDevicePatch(*t.store.current(), t.synA, "// mine\n" + louderSynth(*lib))->command).ok());
  auto commented = lib->resolve(t.dev(t.synA));
  REQUIRE((commented != nullptr));
  CHECK((commented != r));
  CHECK((commented->graph == r->graph));
  // A broken text keeps the last good version playing (stale) and says why.
  const std::string broken = replaced(louderSynth(*lib), "out = osc * env * vel * level", "out = osc * env * vel * levl");
  REQUIRE(t.store.dispatch(ops::editDevicePatch(*t.store.current(), t.synA, broken)->command).ok());
  auto bad = lib->resolve(t.dev(t.synA));
  REQUIRE((bad != nullptr));
  CHECK_FALSE(bad->error.empty());
  CHECK(bad->stale);
  CHECK((bad->graph == r->graph));
  CHECK(bad->error.find("levl") != std::string::npos);
  // A text without the device's patch, or that is not an instrument any more: errors too.
  auto renamed = std::make_shared<UserPatch>();
  renamed->text = replaced(louderSynth(*lib), "patch BasicSynth {", "patch Other {");
  Device other = t.dev(t.synA);
  other.source = renamed;
  auto none = freshLibrary()->resolve(other);  // a library that never saw a good version: nothing plays
  REQUIRE((none != nullptr));
  CHECK((none->graph == nullptr));
  CHECK(none->error.find("no patch 'BasicSynth'") != std::string::npos);
  // The editor's parse (UserPatch::ids) decides the ids: a fresh library resolving the same text agrees with it.
  auto lib2 = freshLibrary();
  pq::graph::ParseOptions po;
  po.lenientLayout = true;
  po.deferMissingInputs = true;
  pq::graph::ParseResult pr = pq::graph::parseDetailed(louderSynth(*lib), "x.pqp", po);
  REQUIRE(pr.ok());
  auto ids = std::make_shared<pq::graph::PatchFile>(pr.file);
  Device withIds = t.dev(t.synA);
  auto src = std::make_shared<UserPatch>();
  src->text = louderSynth(*lib);
  src->ids = ids;
  withIds.source = src;
  auto r2 = lib2->resolve(withIds);
  REQUIRE((r2 != nullptr));
  REQUIRE(r2->error.empty());
  const pq::graph::Patch& p2 = *r2->parsed.find("BasicSynth");
  for (pq::graph::NodeIndex v = 0; v < p2.nodeCount(); ++v) CHECK(p2.node(v).id == ids->patches[0].node(v).id);
}

TEST_CASE("user devices: lowering plays the device's own patch; only its track changes") {
  auto lib = freshLibrary();
  Two t;
  LowerOptions lo;
  lo.library = &lib->library();
  lo.devices = lib.get();
  uint32_t slot = 0;
  lo.slotOf = [&](TrackId) { return slot++; };
  auto l0 = lower(*t.store.current(), lo);
  REQUIRE(l0.ok());
  CHECK((l0->userLibrary == nullptr));
  CHECK((l0->graph.library == &lib->library()));
  const uint32_t ia = graphIndex(*l0, t.a), ib = graphIndex(*l0, t.b);
  const std::string levelA = "dev" + std::to_string(t.synA.value) + ":level";
  CHECK(paramOf(*l0, levelA)->value == doctest::Approx(0.2f));

  REQUIRE(t.store.dispatch(ops::editDevicePatch(*t.store.current(), t.synA, louderSynth(*lib))->command).ok());
  auto l1 = lower(*t.store.current(), lo, &*l0);
  REQUIRE_MESSAGE(l1.ok(), (l1.ok() ? std::string() : l1.error().message));
  CHECK(l1->missingDevices.empty());
  CHECK(l1->deviceErrors.empty());
  REQUIRE((l1->userLibrary != nullptr));
  CHECK((l1->graph.library == l1->userLibrary.get()));
  REQUIRE(l1->userDevices.size() == 1);
  // Track a's patch changed (its call names the user patch), track b's is shared.
  CHECK((l1->patches[ia] != l0->patches[ia]));
  CHECK((l1->patches[ib] == l0->patches[ib]));
  const pq::graph::Patch& pa = l1->patches[ia]->patch;
  const auto call = pa.indexOf(pq::graph::NodeId{nodeIdFor(NodeRole::Device, t.synA.value, 0)});
  REQUIRE(call.has_value());  // the same call node id: the device keeps its state keys
  CHECK(pa.subcircuit(*call).patch == l1->userDevices[0]->graph->root);
  CHECK(l1->graph.library->find(pa.subcircuit(*call).patch) != nullptr);
  CHECK(l1->graph.library->find("BasicSynth") != nullptr);  // track b's stock patch
  // The user patch's param default (an unset param plays the patch's default).
  CHECK(paramOf(*l1, levelA)->value == doctest::Approx(0.3f));
  CHECK(lowerParams(*t.store.current(), false, *lib).size() == l1->params.size());
  // Without a device library to resolve it, the device is bypassed and named.
  LowerOptions plain = lo;
  plain.devices = nullptr;
  auto l2 = lower(*t.store.current(), plain);
  REQUIRE(l2.ok());
  CHECK(l2->missingDevices == std::vector<DeviceId>{t.synA});
  // A comment edit: the same IR, the patch is shared (no recompile).
  REQUIRE(t.store.dispatch(ops::editDevicePatch(*t.store.current(), t.synA, "// c\n" + louderSynth(*lib))->command).ok());
  auto l3 = lower(*t.store.current(), lo, &*l1);
  REQUIRE(l3.ok());
  CHECK((l3->patches[ia] == l1->patches[ia]));
  // A broken text: the last good version keeps playing (same patch), and the error is reported.
  REQUIRE(t.store.dispatch(
                 ops::editDevicePatch(*t.store.current(), t.synA, replaced(louderSynth(*lib), "vel * level", "vel * levl"))->command)
              .ok());
  auto l4 = lower(*t.store.current(), lo, &*l3);
  REQUIRE(l4.ok());
  CHECK((l4->patches[ia] == l3->patches[ia]));
  REQUIRE(l4->deviceErrors.size() == 1);
  CHECK(l4->deviceErrors[0].device == t.synA);
  CHECK(l4->deviceErrors[0].stale);
  // Revert to Stock: the stock call again.
  REQUIRE(t.store.dispatch(ops::revertDevicesToStock(*t.store.current(), std::vector<DeviceId>{t.synA})->command).ok());
  auto l5 = lower(*t.store.current(), lo, &*l4);
  REQUIRE(l5.ok());
  CHECK((l5->userLibrary == nullptr));
  CHECK(l5->patches[ia]->patch == l0->patches[ia]->patch);
}

TEST_CASE("user devices: a wire into a param an edit removed is Invalid (kept, not lowered) until the param is back") {
  auto lib = freshLibrary();
  Two t;
  auto add = ops::addDevice(*t.store.current(), t.store.ids(), t.a, "lfo_device", DeviceKind::AudioEffect);
  REQUIRE(add.ok());
  REQUIRE(t.store.dispatch(add->command).ok());
  const DeviceId lfoDev = add->devices.at(0);
  auto w = ops::connectWire(*t.store.current(), t.store.ids(), lfoDev, "lfo", t.sat, "drive");
  REQUIRE(w.ok());
  REQUIRE(t.store.dispatch(w->command).ok());
  const WireId wire = w->wires.at(0);
  auto states = wireStates(*t.store.current(), *lib);
  REQUIRE(states.size() == 1);
  CHECK(states[0].status == WireStatus::Lowered);
  // The edit renames `drive`: the wire stays in the document but is not lowered.
  const std::string noDrive = replaced(replaced(lib->stockText("saturator"), "param drive      = 0dB", "param drv        = 0dB"),
                                       "u   = pre * drive", "u   = pre * drv");
  auto ed = ops::editDevicePatch(*t.store.current(), t.sat, noDrive);
  REQUIRE(ed.ok());
  REQUIRE(t.store.dispatch(ed->command).ok());
  auto r = lib->resolve(t.dev(t.sat));
  REQUIRE((r != nullptr));
  CHECK_MESSAGE(r->error.empty(), r->error);
  CHECK(r->info.param("drive") == nullptr);
  CHECK(r->info.param("drv") != nullptr);
  REQUIRE(findWire(*t.store.current(), wire) != nullptr);
  states = wireStates(*t.store.current(), *lib);
  REQUIRE(states.size() == 1);
  CHECK(states[0].status == WireStatus::Invalid);
  // Undo: the param is back and the wire plays again.
  REQUIRE(t.store.undo());
  states = wireStates(*t.store.current(), *lib);
  CHECK(states[0].status == WireStatus::Lowered);
}

TEST_CASE("store: a coalescing dispatch joins its run's history entry") {
  auto lib = freshLibrary();
  Two t;
  const Snapshot before = t.store.current();
  const size_t steps = t.store.history().cursor();
  std::string text = lib->stockText("BasicSynth");
  int notified = 0;
  t.store.subscribe([&](const StoreEvent&) { ++notified; });
  for (int i = 0; i < 3; ++i) {
    text = "// " + std::to_string(i) + "\n" + text;
    REQUIRE(t.store.dispatchCoalescing(ops::editDevicePatch(*t.store.current(), t.synA, text)->command, 42).ok());
  }
  CHECK(notified == 3);  // every change is heard (and played) at once
  CHECK(t.store.history().cursor() == steps + 1);
  CHECK(t.dev(t.synA).source->text == text);
  // Another key, or any other dispatch, starts a new step.
  REQUIRE(t.store.dispatchCoalescing(ops::editDevicePatch(*t.store.current(), t.synA, "//x\n" + text)->command, 43).ok());
  CHECK(t.store.history().cursor() == steps + 2);
  REQUIRE(t.store.dispatch(ops::setDeviceParam(*t.store.current(), t.sat, "drive", 3.0)->command).ok());
  REQUIRE(t.store.dispatchCoalescing(ops::editDevicePatch(*t.store.current(), t.synA, "//y\n" + text)->command, 43).ok());
  CHECK(t.store.history().cursor() == steps + 4);
  // Undo takes each step back; the first one restores the stock device.
  for (int i = 0; i < 4; ++i) REQUIRE(t.store.undo());
  CHECK((t.store.current() == before));
  // After an undo, the same key does not join the entry it undid.
  REQUIRE(t.store.redo());
  REQUIRE(t.store.dispatchCoalescing(ops::editDevicePatch(*t.store.current(), t.synA, "//z\n" + text)->command, 42).ok());
  CHECK(t.store.history().cursor() == steps + 2);
  CHECK(t.store.history().entries().size() == steps + 2);
}
