// T1.x-text-ids (QUESTIONS Q68): text edits of a .pqp keep hot-swap state. The edited text is parsed with the
// previous parse (ParseOptions::previous: nodes inherit NodeIds, id_match.h) and scheduled with the previous state
// layout; the transfer plan against the running image then matches the old state entries. Corpus-wide per edit kind
// (an export, inserted lines, renamed bindings, reordered lines, a canonical reprint), and a hot swap after a text
// edit that is bit-identical to no swap on the unchanged path (global and voices).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../graph/rename_edits.h"
#include "../graph/text_edits.h"
#include "interp_harness.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/note_events.h"
#include "pq/engine/program_host.h"
#include "pq/engine/state_transfer.h"

using namespace interp_test;
namespace graph = pq::graph;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 64;

// One version of a text: its parse (the next version's ParseOptions::previous) and every patch that compiles,
// scheduled against the previous version's layout of the same patch.
struct Version {
  std::unique_ptr<ParseResult> parsed;
  std::map<std::string, Compiled> units;
};

Version compileVersion(const std::string& text, const fs::path& path, const Version* previous, bool inheritIds = true) {
  Version v;
  text_util::DirLoader loader(path.parent_path());
  ParseOptions po;
  po.imports = &loader;
  po.previous = previous && inheritIds ? &previous->parsed->file : nullptr;
  v.parsed = std::make_unique<ParseResult>(parseDetailed(text, path.string(), po));
  if (!v.parsed->ok()) return v;
  // Imported files (not edited) parse the same every time.
  std::vector<std::unique_ptr<ParseResult>> imported;
  std::vector<std::string> texts;
  std::vector<fs::path> pending;
  for (const std::string& imp : v.parsed->file.imports) pending.push_back(path.parent_path() / imp);
  while (!pending.empty()) {
    const fs::path p = fs::weakly_canonical(pending.back());
    pending.pop_back();
    texts.push_back(text_util::readFile(p));
    ParseOptions io;
    io.imports = &loader;
    imported.push_back(std::make_unique<ParseResult>(parseDetailed(texts.back(), p.string(), io)));
    if (!imported.back()->ok()) continue;
    for (const std::string& imp : imported.back()->file.imports) pending.push_back(p.parent_path() / imp);
  }
  PatchLibrary lib;
  lib.add(v.parsed->file);
  for (const auto& f : imported)
    if (f->ok()) lib.add(f->file);
  for (const Patch& patch : v.parsed->file.patches) {
    InferOptions io;
    io.library = &lib;
    InferResult inf = inferDetailed(patch, io);
    if (!inf.ok()) {  // as compileFile: retry with unconnected root ins reading their defaults
      std::vector<RootInput> roots;
      const Adjacency adj = patch.adjacency();
      for (NodeIndex n = 0; n < patch.nodeCount(); ++n) {
        if (patch.node(n).kind != NodeKind::Input) continue;
        RootInput ri;
        ri.connected = adj.driver[patch.flatInput(n, 0)] == kNone;
        roots.push_back(ri);
      }
      io.inputs = roots;
      inf = inferDetailed(patch, io);
    }
    if (!inf.ok()) continue;
    OptimiseOptions oo;
    oo.sampleRate = kSr;
    auto opt = optimise(std::move(inf.typed), oo);
    if (!opt.ok()) continue;
    ScheduleOptions so;
    so.sampleRate = kSr;
    if (previous) {
      const auto it = previous->units.find(patch.name());
      if (it != previous->units.end()) so.previous = &it->second.sched.state;
    }
    ScheduleResult sr = scheduleDetailed(opt.value(), so);
    if (!sr.ok()) continue;
    Compiled c;
    c.file = path.filename().string();
    c.name = patch.name();
    c.typed = std::move(opt.value());
    c.sched = std::move(sr.schedule);
    v.units.emplace(c.name, std::move(c));
  }
  return v;
}

std::unique_ptr<eng::ProgramImage> buildImage(const Compiled& c, uint32_t groups = 1) {
  eng::InterpretOptions io;
  io.maxBlockFrames = kBlock;
  io.maxVoiceGroups = groups;
  auto built = eng::buildInterpreted(c.sched, io);
  if (!built.ok()) return nullptr;
  return std::move(built.value());
}

struct PlanTally {
  uint64_t oldEntries = 0, matched = 0, patches = 0, edits = 0;
  double worst = 1.0;
  std::string worstWhere;
  double fraction() const { return oldEntries ? static_cast<double>(matched) / static_cast<double>(oldEntries) : 1.0; }
};

// Transfer plans from every unit of `a` to the same-named unit of `b`.
void tallyPlans(const Version& a, const Version& b, const std::string& where, PlanTally& t) {
  ++t.edits;
  for (const auto& [name, ca] : a.units) {
    const auto it = b.units.find(name);
    REQUIRE_MESSAGE(it != b.units.end(), where << ": " << name << " no longer compiles");
    if (ca.sched.state.entries.empty()) continue;
    auto ia = buildImage(ca), ib = buildImage(it->second);
    if (!ia || !ib) continue;  // needs resources the plain build does not have
    auto plan = eng::buildTransferPlan(ia->stateLayout(), ia->stateArena(), ib->stateLayout(), ib->stateArena());
    REQUIRE(plan.ok());
    const uint64_t n = ca.sched.state.entries.size();
    t.oldEntries += n;
    t.matched += plan->matched;
    ++t.patches;
    const double f = static_cast<double>(plan->matched) / static_cast<double>(n);
    if (f < t.worst) {
      t.worst = f;
      t.worstWhere = where + ":" + name + " (" + std::to_string(plan->matched) + "/" + std::to_string(n) + ")";
    }
  }
}

std::vector<fs::path> corpusFiles() {
  const fs::path tests = fs::path(PQ_TEST_DATA_DIR);
  std::vector<fs::path> files;
  for (const fs::path& dir : {text_util::casesDir(), text_util::stdlibDir(), tests / "golden" / "patches",
                              tests / "engine" / "patches", tests / "jit" / "patches",
                              tests / ".." / "docs" / "reviews" / "M1" / "probes"})
    for (const fs::path& p : text_util::pqpFiles(dir)) files.push_back(p);
  return files;
}

}  // namespace

TEST_CASE("text edits: ≥ 99 % of the state entries of every corpus patch match in the transfer plan" * doctest::test_suite("slow")) {
  std::map<std::string, PlanTally> tally;
  PlanTally baseline;  // the Q68 behaviour: the same export edit re-parsed without the previous parse
  uint32_t files = 0;
  for (const fs::path& path : corpusFiles()) {
    const std::string text = text_util::readFile(path);
    const Version a = compileVersion(text, path, nullptr);
    if (!a.parsed->ok() || a.units.empty()) continue;
    ++files;
    const std::string where = path.filename().string();
    CAPTURE(where);
    auto edit = [&](const std::string& kind, const std::string& edited) {
      CAPTURE(kind);
      const Version b = compileVersion(edited, path, &a);
      REQUIRE_MESSAGE(b.parsed->ok(), "edited text does not parse:\n" << edited);
      tallyPlans(a, b, where, tally[kind]);
    };
    const std::string withExport = text_edits::insertAtTop(text, "  export edit_spare = sine(phasor(331))");
    edit("export at the top", withExport);
    tallyPlans(a, compileVersion(withExport, path, &a, false), where, baseline);
    edit("inserted lines", text_edits::insertInMiddle(text_edits::insertAtTop(text_edits::insertAtBottom(text, "  param edit_p = 0.5"),
                                                                              "  edit_top = phasor(0.37)"),
                                                      "  edit_mid = phasor(0.41)"));
    edit("reordered lines", text_edits::shuffleBindings(text, 7));
    edit("canonical reprint", print(a.parsed->file));
    // Renames: up to 4 bindings one at a time, then up to 64 at once (each checked to be exactly that IR rename).
    const auto candidates = rename_edits::bindings(a.parsed->file);
    const size_t step = std::max<size_t>(1, candidates.size() / 4);
    for (size_t c = 0; c < candidates.size(); c += step) {
      const std::string edited = rename_edits::renameOne(text, path, a.parsed->file, candidates[c].first, candidates[c].second);
      if (!edited.empty()) edit("renamed binding", edited);
    }
    size_t renamedAtOnce = 0;
    const std::string all = rename_edits::renameAll(text, path, a.parsed->file, 64, &renamedAtOnce);
    if (renamedAtOnce > 1) edit("all bindings renamed at once", all);
  }
  CHECK(files >= 35);
  MESSAGE("Q68 baseline (export at the top, re-parsed WITHOUT the previous parse): " << baseline.matched << " / "
                                                                                   << baseline.oldEntries << " = "
                                                                                   << 100.0 * baseline.fraction() << " %");
  CHECK(baseline.fraction() < 0.5);  // the edit really renumbers: the test can see a lost match
  for (const auto& [kind, t] : tally) {
    MESSAGE(kind << ": " << t.edits << " edits over " << t.patches << " stateful patches: " << t.matched << " / "
                 << t.oldEntries << " state entries matched = " << 100.0 * t.fraction() << " %; worst patch "
                 << 100.0 * t.worst << " % " << t.worstWhere);
    CHECK(t.fraction() >= 0.99);
    CHECK(t.worst >= 0.99);
  }
  CHECK(tally.size() == 6);
}

namespace {

struct Render {
  std::vector<float> ch[2];
  int64_t time = 0;
  uint64_t rtViolations = 0;
};

void render(eng::ProgramHost& h, uint32_t frames, Render& r) {
  std::vector<float> b0(kBlock), b1(kBlock);
  float* outs[2] = {b0.data(), b1.data()};
  while (frames > 0) {
    const uint32_t n = std::min(kBlock, frames);
    eng::AudioBlock ab;
    ab.out = outs;
    ab.numOut = 2;
    ab.frames = n;
    ab.frameTime = r.time;
    r.rtViolations += pq::alloc_trap::countRtViolations([&] {
      pq::DenormalGuard dg;
      h.process(ab);
    });
    r.ch[0].insert(r.ch[0].end(), b0.begin(), b0.begin() + n);
    r.ch[1].insert(r.ch[1].end(), b1.begin(), b1.begin() + n);
    r.time += n;
    frames -= n;
  }
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
  double m = 0.0;
  for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
  return m;
}

struct Script {
  std::vector<std::pair<int64_t, eng::NoteEvent>> events;
  static void fill(void* user, int64_t time, uint32_t frames, eng::EventBuffer& out) noexcept {
    auto* s = static_cast<Script*>(user);
    for (const auto& [t, e] : s->events)
      if (t >= time && t < time + frames) {
        eng::NoteEvent ev = e;
        ev.frame = static_cast<uint32_t>(t - time);
        out.push(ev);
      }
  }
};

eng::HostConfig hostConfig(Script* voices) {
  eng::HostConfig hc;
  hc.sampleRate = kSr;
  hc.maxBlockFrames = kBlock;
  hc.numOut = 2;
  if (voices) {
    hc.voices = true;
    hc.voice.maxVoices = 16;
    hc.events = {&Script::fill, voices};
  }
  return hc;
}

// Renders `a` alone, and `a` then the edited text's image swapped in (Auto: audible, so crossfaded) at kSwap.
struct SwapResult {
  Render ref, swapped;
  eng::TransferPlan plan;
};
SwapResult swapAfterEdit(const Compiled& a, const Compiled& b, Script* voices, uint32_t groups) {
  constexpr uint32_t kSwap = kBlock * 37, kTotal = kBlock * 200;
  SwapResult out;
  {
    eng::ProgramHost h(hostConfig(voices));
    REQUIRE(h.publish(buildImage(a, groups)).ok());
    render(h, kTotal, out.ref);
  }
  eng::ProgramHost h(hostConfig(voices));
  REQUIRE(h.publish(buildImage(a, groups)).ok());
  render(h, kSwap, out.swapped);
  auto r = h.publish(buildImage(b, groups), eng::SwapPolicy{eng::SwapMode::Auto, 10.0f});
  REQUIRE(r.ok());
  out.plan = r.value().plan;
  render(h, kTotal - kSwap, out.swapped);
  CHECK(h.stats().crossfades == 1);  // audible: the unchanged path must come out of the fade unchanged
  CHECK(out.ref.rtViolations == 0);
  CHECK(out.swapped.rtViolations == 0);
  return out;
}

}  // namespace

TEST_CASE("text edits: a hot swap after a text edit is bit-identical to no swap on the unchanged path") {
  const fs::path path = fs::path(PQ_TEST_DATA_DIR) / "engine" / "patches" / "edit.pqp";  // imports resolve here

  SUBCASE("global: echo through a feedback delay; export added on top, oscillator renamed, lines inserted") {
    const std::string text = R"(patch Echo {
  param level = 0.3 [0..1]
  out y : audio
  osc = sine(phasor(220.5))
  e = delay(osc * 0.4 + e * 0.5, 2.7ms, 10ms)
  y = osc * level + e
}
)";
    std::string edited = text_edits::insertAtTop(text, "  export spare = sine(phasor(331))");
    edited = text_edits::renameIdent(edited, "osc", "carrier", 0, 100);
    edited = text_edits::insertInMiddle(edited, "  lfo = phasor(0.5)");
    const Version a = compileVersion(text, path, nullptr);
    const Version b = compileVersion(edited, path, &a);
    const Version cold = compileVersion(edited, path, &a, false);  // Q68: ids renumbered
    REQUIRE(b.parsed->ok());
    const Compiled& ca = a.units.at("Echo");
    const SwapResult s = swapAfterEdit(ca, b.units.at("Echo"), nullptr, 1);
    CHECK(s.plan.matched == ca.sched.state.entries.size());
    CHECK(s.plan.dropped == 0);
    const double d = std::max(maxAbsDiff(s.ref.ch[0], s.swapped.ch[0]), maxAbsDiff(s.ref.ch[1], s.swapped.ch[1]));
    MESSAGE("text edit, unchanged path: max |swap − no swap| = " << d);
    CHECK(d == 0.0);
    // Negative control: the same edit re-parsed without the previous parse loses the delay line and the phase.
    const SwapResult c = swapAfterEdit(ca, cold.units.at("Echo"), nullptr, 1);
    const double dc = maxAbsDiff(c.ref.ch[0], c.swapped.ch[0]);
    MESSAGE("without the previous parse: " << c.plan.matched << " of " << ca.sched.state.entries.size()
                                           << " entries matched, max |swap − no swap| = " << dc);
    CHECK(c.plan.matched < ca.sched.state.entries.size());
    CHECK(dc > 1e-2);
  }

  SUBCASE("voices: notes sounding through the swap; export added, binding renamed, lines reordered") {
    const std::string text = R"(patch V {
  out y : audio voice
  env = z1(max(env * 0.9995, gate_to_real(voice.gate) * 0.3))
  osc = sine(phasor(voice.hz))
  y = osc * env
}
)";
    std::string edited = text_edits::insertAtTop(text, "  export bright = sine(phasor(voice.hz * 2))");
    edited = text_edits::renameIdent(edited, "osc", "tone", 0, 100);
    edited = text_edits::shuffleBindings(edited, 3);
    const Version a = compileVersion(text, path, nullptr);
    const Version b = compileVersion(edited, path, &a);
    const Version cold = compileVersion(edited, path, &a, false);
    REQUIRE(b.parsed->ok());
    Script s;
    s.events = {{100, eng::noteOn(0, 1, 220.0f, 0.8f)},
                {130, eng::noteOn(0, 2, 277.2f, 0.8f)},
                {kBlock * 37 + 10, eng::noteOn(0, 3, 330.0f, 0.8f)},  // in the fade
                {kBlock * 90, eng::noteOff(0, 2)}};
    const uint32_t groups = eng::voiceGroupsFor(16);
    const Compiled& ca = a.units.at("V");
    const SwapResult r = swapAfterEdit(ca, b.units.at("V"), &s, groups);
    CHECK(r.plan.matched == ca.sched.state.entries.size());
    const double d = maxAbsDiff(r.ref.ch[0], r.swapped.ch[0]);
    MESSAGE("voices, text edit, unchanged path: max |swap − no swap| = " << d);
    CHECK(d == 0.0);
    const SwapResult c = swapAfterEdit(ca, cold.units.at("V"), &s, groups);
    CHECK(c.plan.matched < ca.sched.state.entries.size());
    CHECK(maxAbsDiff(c.ref.ch[0], c.swapped.ch[0]) > 1e-3);
  }

  SUBCASE("the largest voice probe (proto_operator.pqp): export on top + reprint") {
    const fs::path big = fs::path(PQ_TEST_DATA_DIR) / ".." / "docs" / "reviews" / "M1" / "probes" / "proto_operator.pqp";
    const std::string text = text_util::readFile(big);
    const Version a = compileVersion(text, big, nullptr);
    REQUIRE(!a.units.empty());
    text_util::DirLoader loader(big.parent_path());
    ParseOptions po;
    po.imports = &loader;
    const ParseResult plain = parseDetailed(text_edits::insertAtTop(text, "  export edit_spare = phasor(3)"), big.string(), po);
    REQUIRE(plain.ok());
    const Version b = compileVersion(print(plain.file), big, &a);  // the edit, then a canonical reformat
    Script s;
    s.events = {{100, eng::noteOn(0, 1, 220.0f, 0.8f)}, {700, eng::noteOn(0, 2, 330.0f, 0.6f)}};
    for (const auto& [name, ca] : a.units) {
      CAPTURE(name);
      const bool voice = std::any_of(ca.sched.stages.begin(), ca.sched.stages.end(), [](const Stage& st) { return st.poly == Poly::Voice; });
      const SwapResult r = swapAfterEdit(ca, b.units.at(name), voice ? &s : nullptr, voice ? eng::voiceGroupsFor(16) : 1);
      CHECK(r.plan.matched == ca.sched.state.entries.size());
      CHECK(maxAbsDiff(r.ref.ch[0], r.swapped.ch[0]) == 0.0);
    }
  }
}
