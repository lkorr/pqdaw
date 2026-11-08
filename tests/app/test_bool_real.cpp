// TX.bool-real (user report 2026-11-08_12-29-32: "kind mismatch: bool into real input 'x' of (atom gain) inside 'Midi4'
// … from 'out' (out, #3)" failed the whole unit, so the track did not play): a bool reaching a real input is its 0 / 1,
// wherever the wire comes from (graph::infer inserts the gate_to_real; doc lowering converts a bool cable's source).
// On the user's project (tests/app/fixtures/bool_real: the report's project.pqd, which as filed compiles: the bool block
// had been undone):
//   path      a comparison (atom:gt, x > 0) appended to the track's main path: its bool `out` (an atom block's out
//             node, #3) into the mixer's gain x — the report's error. Compiles; silent before the note, the 0 / 1
//             square while it sounds; interpreter == JIT bit for bit.
//   terminal  a comparison cabled into the EQ group's Out terminal: the EQ's `out` is a bool into the mixer.
//   control   the synth's Amplitude knob at 0 and a cable from a comparison (note gate > 0) into it: silent without a
//             note, the as-filed level while the gate is high (the cable was Invalid before TX.bool-real).
// (The gate into gain's x and the patch-level terminals: tests/engine/test_voices.cpp, both backends; inference:
// tests/graph/test_infer.cpp.)
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/serialize.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"

using namespace pq;
using namespace pq::doc;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kFrames = 19200;                  // 0.4 s
constexpr int64_t kNoteOn = 4800, kNoteOff = 14400;  // 0.1 s … 0.3 s
const TrackId kTrack{4};                             // "1 MIDI"
const DeviceId kEq{51}, kEqGain{104}, kPhaseAcc{6}, kAmplitude{9};

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  return **lib;
}

Snapshot userProject() {
  LoadOptions lo;
  lo.devices = &library();
  auto r = loadProject((fs::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / "bool_real").string(), lo);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return r->project;
}

template <class E>
E run(DocumentStore& s, Result<E> e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  const Status st = s.dispatch(e->command);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  return *e;
}
void apply(DocumentStore& s, Command c) {
  const Status st = s.dispatch(playback_fixture::inUnits(s, std::move(c)));
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
}
DeviceId block(DocumentStore& s, std::string patch, DeviceId group) {
  const DeviceId d =
      run(s, ops::addDevice(*s.current(), s.ids(), kTrack, std::move(patch), DeviceKind::AudioEffect, ops::kOffPath))
          .devices.at(0);
  apply(s, SetDeviceGroup{d, group});
  return d;
}
WireId cable(DocumentStore& s, DeviceId from, std::string port, DeviceId to, std::string param) {
  return run(s, ops::connectWire(*s.current(), s.ids(), from, std::move(port), to, std::move(param))).wires.at(0);
}
WireStatus statusOf(const LoweredProject& l, WireId w) {
  for (const WireState& st : l.wires)
    if (st.id == w) return st.status;
  FAIL("no state for wire " << w.value);
  return WireStatus::Invalid;
}

LowerOptions lowerOptions() {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [](TrackId t) { return t == kTrack ? 0u : 1u; };
  return lo;
}

// `p` rendered for kFrames with one note on kTrack (kNoteOn … kNoteOff), every block under the allocation trap.
golden::Channels render(const Project& p, engine::UnitBackend backend = {}) {
  auto low = lower(p, lowerOptions());
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  CHECK(low->missingDevices.empty());
  CHECK(low->deviceErrors.empty());
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
  for (const LoweredParam& x : low->params) {
    links.push_back({x.track, x.name, static_cast<uint32_t>(values.size())});
    values.push_back(x.value);
  }
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  co.voice.maxVoices = 8;
  co.paramLinks = links;
  co.backend = backend;
  engine::ProjectCompiler compiler(co);
  auto build = compiler.build(low->graph);
  // The report's failure: a kind mismatch here failed the whole unit (the track's every image).
  REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
  engine::NoteListSource notes(std::vector<engine::TimedNote>{{kNoteOn, kNoteOff - kNoteOn, 220.0f, 0.8f, 0.5f}});
  engine::NoteListSource none(std::vector<engine::TimedNote>{});
  engine::ProjectConfig pc;
  pc.sampleRate = kRate;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 2;
  pc.numProjectParams = static_cast<uint32_t>(values.size());
  pc.eventSources.push_back(notes.source());
  pc.eventSources.push_back(none.source());
  auto prog = engine::ProjectProgram::create(pc, build->layout);
  REQUIRE_MESSAGE(prog.ok(), (prog.ok() ? std::string() : prog.error().message));
  for (uint32_t u = 0; u < build->images.size(); ++u) {
    const auto pub = (*prog)->publish(u, std::move(build->images[u]));
    REQUIRE_MESSAGE(pub.ok(), (pub.ok() ? std::string() : pub.error().message));
  }
  std::copy(values.begin(), values.end(), (*prog)->projectParams().begin());
  golden::Channels out(2);
  std::vector<float> l(kBlock), r(kBlock);
  float* ch[2] = {l.data(), r.data()};
  for (int64_t t = 0; t < kFrames; t += kBlock) {
    engine::AudioBlock ab{nullptr, 0, ch, 2, kBlock, t};
    CHECK(alloc_trap::countRtViolations([&] {
            DenormalGuard dg;
            (*prog)->process(ab);
          }) == 0);
    out[0].insert(out[0].end(), l.begin(), l.end());
    out[1].insert(out[1].end(), r.begin(), r.end());
  }
  return out;
}

float peak(const golden::Channels& a, size_t from, size_t to) {
  float p = 0.0f;
  for (const auto& c : a)
    for (size_t i = from; i < std::min(to, c.size()); ++i) p = std::max(p, std::fabs(c[i]));
  return p;
}
double maxDiff(const golden::Channels& a, const golden::Channels& b) {
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c)
    for (size_t i = 0; i < a[c].size(); ++i) {
      const double d = std::fabs(static_cast<double>(a[c][i]) - static_cast<double>(b[c][i]));
      m = std::max(m, std::isnan(d) ? 1e9 : d);
    }
  return m;
}

}  // namespace

// The track's 0 / 1 square (a comparison of the synth's signal with 0, the mixer at +6 dB): interpreter == JIT bit for
// bit, silent before the note, two levels (0 and the mixer's gain) while it sounds.
static void checkSquare(const Project& p, const char* what) {
  const golden::Channels a = render(p);
  const golden::Channels j = render(p, compile::jitUnitBackend());
  const double dj = maxDiff(a, j);
  MESSAGE(std::string(what) << ": interpreter vs JIT max |diff| = " << dj << ", peak " << peak(a, kNoteOn, kNoteOff));
  CHECK(dj == 0.0);
  CHECK(peak(a, 0, kNoteOn) == 0.0f);  // the synth is silent: x > 0 is false, 0
  CHECK(peak(a, kNoteOn, kNoteOff) > 1.0f);  // well above the synth itself (≈ 0.45)
  size_t ones = 0, zeros = 0;
  for (size_t i = kNoteOn + 2400; i < kNoteOff; ++i) {
    const float v = std::fabs(a[0][i]);
    ones += v > 1.0f ? 1u : 0u;
    zeros += v == 0.0f ? 1u : 0u;
  }
  CHECK(ones > 1000);
  CHECK(zeros > 1000);
}

TEST_CASE("bool real: the user's project compiles and plays (as filed; a bool block ending the path, the report's error)") {
  {  // the project as filed
    const golden::Channels a = render(*userProject());
    MESSAGE("as filed: peak while the note sounds " << peak(a, kNoteOn, kNoteOff));
    CHECK(peak(a, 0, kNoteOn) == 0.0f);
    CHECK(peak(a, kNoteOn, kNoteOff) > 0.05f);
  }
  {  // The report's error ("bool into real input 'x' of (atom gain) inside 'Midi4' … from 'out' (out, #3)": an atom
     // block's `out`): a comparison appended to the track's main path (EQ → x > 0 → the mixer). Its bool `out` feeds the
     // mixer's gains (out, pre, post) through the series, no cable. Before TX.bool-real the unit failed to compile.
    DocumentStore s(userProject());
    run(s, ops::addDevice(*s.current(), s.ids(), kTrack, "atom:gt", DeviceKind::AudioEffect, kAppend));
    checkSquare(*s.current(), "a comparison ending the path");
  }
  {  // A group terminal: the EQ group's Out fed by a comparison (In > 0) instead of its output gain, so the EQ's `out`
     // is a bool into the mixer's gains.
    DocumentStore s(userProject());
    const DeviceId gt = block(s, "atom:gt", kEq);
    cable(s, kEq, "input", gt, "a");
    std::vector<WireId> old;
    for (const Wire& w : wiresOf(*s.current()))
      if (w.from == kEqGain && w.to == kEq) old.push_back(w.id);
    REQUIRE(old.size() == 1);
    run(s, ops::deleteWires(*s.current(), old));
    const WireId out = cable(s, gt, "out", kEq, "output");
    auto low = lower(*s.current(), lowerOptions());
    REQUIRE(low.ok());
    CHECK(statusOf(*low, out) == WireStatus::Lowered);  // (Invalid before TX.bool-real: a bool output was no signal)
    checkSquare(*s.current(), "a comparison into the EQ's Out");
  }
}

TEST_CASE("bool real: a cable from a comparison into a control plays while the gate is high") {
  DocumentStore s(userProject());
  apply(s, SetDeviceParam{kAmplitude, "value", 0.0});  // the synth's Amplitude knob at 0: silent (the EQ's filter
  {                                                    // state rings at < 1e-19)
    const golden::Channels a = render(*s.current());
    CHECK(peak(a, 0, kFrames) < 1e-9f);
  }
  // A comparison inside the Phase Accumulator (per note): note gate > 0, cabled into the Amplitude knob (+1 in its
  // units while the gate is high).
  const DeviceId gt = block(s, "atom:gt", kPhaseAcc);
  run(s, ops::connectNoteWire(*s.current(), s.ids(), kTrack, "gate", gt, "a"));
  const WireId w = cable(s, gt, "out", kAmplitude, "value");
  {
    auto low = lower(*s.current(), lowerOptions());
    REQUIRE(low.ok());
    CHECK(statusOf(*low, w) == WireStatus::Lowered);  // (Invalid before TX.bool-real: a bool output was no signal)
  }
  const golden::Channels a = render(*s.current());
  MESSAGE("bool cable into a control: peak while the gate is high " << peak(a, kNoteOn, kNoteOff));
  CHECK(peak(a, 0, kNoteOn) < 1e-9f);
  // Amplitude 0 + the cable's 1 while the gate is high: the level the project as filed plays at (its knob is at 1).
  CHECK(peak(a, kNoteOn, kNoteOff) == doctest::Approx(peak(render(*userProject()), kNoteOn, kNoteOff)).epsilon(0.02));
}
