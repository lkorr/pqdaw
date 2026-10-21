// T1.8: voices — the event list, allocation and stealing, sample-accurate voice.* signals, lane state reset,
// voice end, the steal fade, 64 voices under heavy note traffic without allocation.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "interp_harness.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/note_events.h"
#include "pq/engine/voices.h"

using namespace interp_test;
using eng::NoteEvent;
using eng::VoiceConfig;

namespace {

constexpr double kSr = 48000.0;

// Probes every audio-rate voice.* signal through voice outs (the engine mixes them with the lane gain).
const char* kProbe = R"(
patch Probe {
  out y : audio voice
  out f : audio voice
  out trig : audio voice
  out c : audio voice
  y = gate_to_real(voice.gate) * voice.velocity
  f = voice.hz
  trig = gate_to_real(voice.trigger)
  c = voice.cents
}
)";

// A held sine: never silent while sounding (steal / fade tests).
const char* kSine = R"(
patch Sine {
  out y : audio voice
  y = sine(phasor(voice.hz)) * 0.3
}
)";

// The phase of a per-voice phasor; ends itself one block after note-off through `done`.
const char* kPhase = R"(
patch Phase {
  out ph : audio voice
  out done : bool voice
  ph = phasor(voice.hz)
  done = not(voice.gate)
}
)";

// No voice out at all: the voices are reduced inside the patch (voice end falls back to the release timeout).
const char* kInternal = R"(
patch Internal {
  out g : audio global
  g = voice_sum(gate_to_real(voice.gate) * voice.velocity)
}
)";

// A per-voice feedback echo through a delay line (moving a voice must re-align its ring).
const char* kEcho = R"(
patch Echo {
  out y : audio voice
  x = sine(phasor(voice.hz)) * gate_to_real(voice.gate) * 0.2
  e = delay(x + e * 0.6, 3ms, 10ms)
  y = x + e
}
)";

struct Ev {
  int64_t t;
  NoteEvent e;
};

const Patch& schedPatch(const Compiled& c) { return c.sched.rewritten ? c.sched.rewritten->patch : c.typed.patch; }

struct Rig {
  Compiled c;
  std::unique_ptr<eng::ProgramImage> img;
  eng::VoiceEngine ve;
  eng::EventBuffer buf{4096};
  std::vector<std::vector<float>> globalOut;
  std::vector<float*> globalPtr;
  int64_t time = 0;
  uint64_t rtViolations = 0;

  Rig(Compiled compiled, VoiceConfig cfg, uint32_t groups, uint32_t maxBlock = 128, bool useDone = true)
      : c(std::move(compiled)) {
    eng::InterpretOptions io;
    io.maxBlockFrames = maxBlock;
    io.maxVoiceGroups = groups;
    auto built = eng::buildInterpreted(c.sched, io);
    REQUIRE_MESSAGE(built.ok(), (built.ok() ? "" : built.error().message));
    img = std::move(built.value());
    if (useDone) cfg.doneSlot = eng::findVoiceSlot(img->io(), schedPatch(c), "done");
    cfg.maxVoices = std::min(cfg.maxVoices, groups * 8);
    // The Probe's pitch outputs are not audio: only `y` decides silence and level.
    if (const uint32_t y = eng::findVoiceSlot(img->io(), schedPatch(c), "y"); y != eng::kNoSlot && cfg.monitorMask == 0)
      cfg.monitorMask = 1u << y;
    const auto st = ve.prepare(img.get(), cfg);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? "" : st.error().message));
    globalOut.assign(img->io().globalOut.size(), std::vector<float>(maxBlock));
    for (auto& g : globalOut) globalPtr.push_back(g.data());
  }
  Rig(const char* text, VoiceConfig cfg, uint32_t groups, uint32_t maxBlock = 128, bool useDone = true)
      : Rig(compileText(text, kSr), cfg, groups, maxBlock, useDone) {}

  uint32_t mixIndex(std::string_view name) const {
    for (uint32_t i = 0; i < ve.numMixes(); ++i)
      if (schedPatch(c).nodeName(img->io().voiceOut[ve.mixSlot(i)].node) == name) return i;
    FAIL("no mix named " << name);
    return 0;
  }

  // Renders `frames` frames in blocks of `block`; events at absolute times (≥ time). Returns every mix (and, last,
  // global out 0 if any) per sample. `perBlock` is called after each block with its start time and length.
  template <class F>
  std::vector<std::vector<float>> run(uint32_t frames, uint32_t block, const std::vector<Ev>& evs, F&& perBlock) {
    const uint32_t nm = ve.numMixes() + (globalOut.empty() ? 0u : 1u);
    std::vector<std::vector<float>> out(nm);
    size_t next = 0;
    while (next < evs.size() && evs[next].t < time) ++next;
    pq::DenormalGuard dg;
    for (uint32_t done = 0; done < frames;) {
      const uint32_t n = std::min(block, frames - done);
      buf.clear();
      while (next < evs.size() && evs[next].t < time + n) {
        NoteEvent e = evs[next].e;
        e.frame = static_cast<uint32_t>(evs[next].t - time);
        buf.push(e);
        ++next;
      }
      eng::ProcessContext ctx;
      ctx.frames = n;
      ctx.sampleRate = kSr;
      ctx.sampleTime = time;
      ctx.out = globalPtr.data();
      ctx.numOut = static_cast<uint32_t>(globalPtr.size());
      ctx.events = &buf.list();
      rtViolations += pq::alloc_trap::countRtViolations([&] { ve.process(ctx); });
      for (uint32_t i = 0; i < ve.numMixes(); ++i) out[i].insert(out[i].end(), ve.mix(i), ve.mix(i) + n);
      if (!globalOut.empty()) out.back().insert(out.back().end(), globalOut[0].begin(), globalOut[0].begin() + n);
      perBlock(time, n);
      time += n;
      done += n;
    }
    return out;
  }
  std::vector<std::vector<float>> run(uint32_t frames, uint32_t block, const std::vector<Ev>& evs = {}) {
    return run(frames, block, evs, [](int64_t, uint32_t) {});
  }
};

Ev on(int64_t t, uint32_t id, float hz, float vel = 0.8f) { return {t, eng::noteOn(0, id, hz, vel)}; }
Ev off(int64_t t, uint32_t id, float rel = 0.5f) { return {t, eng::noteOff(0, id, rel)}; }
Ev pitch(int64_t t, uint32_t id, float hz, uint32_t ramp = 0) { return {t, eng::notePitch(0, id, hz, ramp)}; }

}  // namespace

TEST_CASE("voices: EventBuffer keeps frame order (stable) and counts overflow") {
  eng::EventBuffer b(4);
  CHECK(b.push(eng::noteOn(10, 1, 100.0f, 1.0f)));
  CHECK(b.push(eng::noteOn(3, 2, 100.0f, 1.0f)));
  CHECK(b.push(eng::noteOff(10, 3)));
  CHECK(b.push(eng::noteOff(0, 4)));
  CHECK_FALSE(b.push(eng::noteOff(0, 5)));
  CHECK(b.dropped() == 1);
  const eng::EventList& l = b.list();
  REQUIRE(l.count == 4);
  CHECK(l.events[0].noteId == 4);
  CHECK(l.events[1].noteId == 2);
  CHECK(l.events[2].noteId == 1);  // same frame: push order
  CHECK(l.events[3].noteId == 3);
  CHECK(pq::alloc_trap::countRtViolations([&] {
          b.clear();
          b.push(eng::noteOn(1, 9, 1.0f, 1.0f));
        }) == 0);
}

TEST_CASE("voices: an event at frame k takes effect at sample k, for every block size") {
  // Note-on / note-off / pitch steps placed on and around block boundaries.
  const std::vector<Ev> evs = {on(0, 1, 110.0f, 0.5f),  on(31, 2, 220.0f, 0.25f), on(32, 3, 330.0f, 0.125f),
                               on(33, 4, 440.0f, 1.0f), off(64, 1),              off(97, 2),
                               pitch(100, 3, 660.0f),   off(127, 4),             off(200, 3)};
  std::vector<float> refY;
  for (uint32_t block : {1u, 7u, 31u, 32u, 37u, 64u, 128u}) {
    INFO("block " << block);
    VoiceConfig cfg;
    Rig r(kProbe, cfg, 2);
    const auto out = r.run(300, block, evs);
    const auto& y = out[r.mixIndex("y")];
    const auto& f = out[r.mixIndex("f")];
    const auto& tr = out[r.mixIndex("trig")];
    for (uint32_t t = 0; t < 300; ++t) {
      INFO("t " << t);
      // y = Σ gate · velocity: exact steps at the event samples.
      const float e1 = t < 64 ? 0.5f : 0.0f;
      const float e2 = t >= 31 && t < 97 ? 0.25f : 0.0f;
      const float e3 = t >= 32 && t < 200 ? 0.125f : 0.0f;
      const float e4 = t >= 33 && t < 127 ? 1.0f : 0.0f;
      CHECK(y[t] == doctest::Approx(e1 + e2 + e3 + e4).epsilon(1e-6));
      // trigger: one sample at each onset.
      const float trig = (t == 0 || t == 31 || t == 32 || t == 33) ? 1.0f : 0.0f;
      CHECK(tr[t] == trig);
      // voice.hz (gain-weighted sum over voices: every started voice still has gain 1 until it ends).
      float hz = 110.0f;
      if (t >= 31) hz += 220.0f;
      if (t >= 32) hz += t >= 100 ? 660.0f : 330.0f;
      if (t >= 33) hz += 440.0f;
      CHECK(f[t] == doctest::Approx(hz).epsilon(1e-6));
    }
    CHECK(r.rtViolations == 0);
    CHECK(r.ve.stats().noteOns == 4);
    CHECK(r.ve.stats().noteOffs == 4);
    if (refY.empty()) refY = y;
    CHECK(y == refY);
  }
}

TEST_CASE("voices: voice.cents follows voice.hz; pitch steps and glides (linear in cents)") {
  VoiceConfig cfg;
  Rig r(kProbe, cfg, 1);
  const auto out = r.run(400, 64, {on(10, 1, 200.0f), pitch(50, 1, 400.0f, 100), pitch(300, 1, 300.0f)});
  const auto& f = out[r.mixIndex("f")];
  const auto& c = out[r.mixIndex("c")];
  CHECK(f[9] == 0.0f);
  CHECK(f[10] == 200.0f);
  CHECK(f[50] == 200.0f);
  CHECK(f[100] == doctest::Approx(200.0 * std::sqrt(2.0)).epsilon(1e-5));
  CHECK(f[150] == 400.0f);  // reaches the target exactly
  CHECK(f[299] == 400.0f);
  CHECK(f[300] == 300.0f);
  for (uint32_t t = 51; t <= 150; ++t) CHECK(f[t] > f[t - 1]);
  const double ref = cfg.referenceHz;
  CHECK(c[10] == doctest::Approx(1200.0 * std::log2(200.0 / ref)).epsilon(1e-5));
  CHECK(c[100] == doctest::Approx(1200.0 * std::log2(200.0 * std::sqrt(2.0) / ref)).epsilon(1e-5));
  CHECK(c[150] == doctest::Approx(1200.0 * std::log2(400.0 / ref)).epsilon(1e-6));
  for (uint32_t t = 52; t < 150; ++t)  // linear in cents: constant step
    CHECK((c[t] - c[t - 1]) == doctest::Approx(c[51] - c[50]).epsilon(1e-3));
  CHECK(r.ve.stats().pitchEvents == 2);
}

TEST_CASE("voices: idle and pre-onset lanes read hz 0, gate false; param-rate signals are 8-float patterns") {
  VoiceConfig cfg;
  Rig r(kProbe, cfg, 2);
  NoteEvent e = eng::noteOn(0, 7, 330.0f, 0.6f);
  e.ratio = 1.5f;
  e.harmonic = 3.0f;
  e.unisonCount = 2;
  e.unisonIndex = 1;
  r.run(64, 64, {on(0, 1, 110.0f), {20, e}});
  REQUIRE(r.ve.laneStatus(1) == eng::LaneStatus::Held);
  const float* hz = r.ve.signal(0, eng::VoiceSignal::Hz);
  const float* gate = r.ve.signal(0, eng::VoiceSignal::Gate);
  for (uint32_t t = 0; t < 64; ++t) {
    CHECK(hz[t * 8 + 0] == 110.0f);
    CHECK(hz[t * 8 + 1] == (t < 20 ? 0.0f : 330.0f));
    CHECK(std::bit_cast<uint32_t>(gate[t * 8 + 1]) == (t < 20 ? 0u : 0xFFFFFFFFu));
    CHECK(hz[t * 8 + 2] == 0.0f);  // free lane
    CHECK(r.ve.signal(0, eng::VoiceSignal::Cents)[t * 8 + 2] == eng::kIdleCents);
    CHECK(r.ve.gain(0)[t * 8 + 1] == (t < 20 ? 0.0f : 1.0f));
  }
  CHECK(r.ve.signal(0, eng::VoiceSignal::Velocity)[1] == 0.6f);
  CHECK(r.ve.signal(0, eng::VoiceSignal::Ratio)[1] == 1.5f);
  CHECK(r.ve.signal(0, eng::VoiceSignal::Harmonic)[1] == 3.0f);
  CHECK(r.ve.signal(0, eng::VoiceSignal::UnisonCount)[1] == 2.0f);
  CHECK(r.ve.signal(0, eng::VoiceSignal::Index)[1] == 1.0f);
  CHECK(r.ve.signal(0, eng::VoiceSignal::Ratio)[0] == 1.0f);
  r.run(64, 64, {off(70, 1, 0.3f)});
  CHECK(r.ve.signal(0, eng::VoiceSignal::Release)[0] == 0.3f);
  // Steady blocks re-use the buffers (nothing re-rendered) and stay correct.
  const auto out = r.run(256, 64);
  CHECK(out[r.mixIndex("f")][255] == 440.0f);  // the released 110 Hz voice still sounds (y = 0 for < 20 ms)
}

TEST_CASE("voices: allocation packs voices into the fullest occupied group; voices end on silence") {
  VoiceConfig cfg;
  cfg.silenceMs = 5.0f;  // 240 frames
  Rig r(kProbe, cfg, eng::voiceGroupsFor(64), 64);
  std::vector<Ev> evs;
  for (uint32_t i = 0; i < 20; ++i) evs.push_back(on(i, 100 + i, 100.0f + 10.0f * static_cast<float>(i)));
  r.run(64, 64, evs);
  for (uint32_t i = 0; i < 20; ++i) CHECK(r.ve.laneOf(100 + i) == i);
  CHECK(r.ve.liveVoices() == 20);
  CHECK(r.ve.activeGroups() == 3);
  CHECK(r.ve.groupsRunLastBlock() == 3);
  // Release the voices in lanes 2 and 3 (group 0): y drops to 0, so they end after silenceMs.
  r.run(64, 64, {off(70, 102), off(70, 103)});
  CHECK(r.ve.laneStatus(2) == eng::LaneStatus::Released);
  CHECK(r.ve.liveVoices() == 20);
  r.run(64 * 3, 64);
  CHECK(r.ve.laneStatus(2) == eng::LaneStatus::Released);  // 186 silent frames < 240
  r.run(64, 64);
  CHECK(r.ve.laneStatus(2) == eng::LaneStatus::Free);
  CHECK(r.ve.laneStatus(3) == eng::LaneStatus::Free);
  CHECK(r.ve.liveVoices() == 18);
  CHECK(r.ve.stats().voicesEnded == 2);
  // Group 0 holds 6, group 2 holds 4: the new note goes to group 0 (lane 2), not to an empty or emptier group.
  r.run(64, 64, {on(r.time + 5, 500, 900.0f)});
  CHECK(r.ve.laneOf(500) == 2);
  CHECK(r.ve.activeGroups() == 3);
  // All notes off: everything ends and the voice stages stop running.
  r.run(64, 64, {{r.time, NoteEvent{0, eng::NoteEventKind::AllNotesOff}}});
  r.run(64 * 6, 64);
  CHECK(r.ve.liveVoices() == 0);
  CHECK(r.ve.activeGroups() == 0);
  CHECK(r.ve.groupsRunLastBlock() == 0);
  CHECK(r.rtViolations == 0);
}

TEST_CASE("voices: stealing — oldest, released first, quietest, none") {
  SUBCASE("oldest; released voices go first") {
    VoiceConfig cfg;
    cfg.maxVoices = 4;
    Rig r(kProbe, cfg, eng::voiceGroupsFor(4), 64);
    r.run(64, 64, {on(0, 1, 100.0f), on(1, 2, 200.0f), on(2, 3, 300.0f), on(3, 4, 400.0f)});
    r.run(64, 64, {on(70, 5, 500.0f)});
    CHECK(r.ve.stats().steals == 1);
    CHECK(r.ve.laneStatus(r.ve.laneOf(1)) == eng::LaneStatus::Fading);
    CHECK(r.ve.laneStatus(r.ve.laneOf(5)) == eng::LaneStatus::Held);
    CHECK(r.ve.liveVoices() == 4);
    r.run(256, 64);  // the 3 ms fade (144 frames) ends
    CHECK(r.ve.laneOf(1) == eng::kNoSlot);
    r.run(64, 64, {off(r.time + 1, 3), on(r.time + 2, 6, 600.0f)});
    CHECK(r.ve.laneOf(2) != eng::kNoSlot);  // held 2 is older than released 3, but 3 goes first
    CHECK(r.ve.laneStatus(r.ve.laneOf(3)) == eng::LaneStatus::Fading);
    CHECK(r.ve.stats().steals == 2);
    CHECK(r.ve.stats().deferredStarts == 0);
  }
  SUBCASE("quietest (by the monitored outputs' level)") {
    VoiceConfig cfg;
    cfg.maxVoices = 4;
    cfg.steal = eng::StealPolicy::Quietest;
    Rig r(kProbe, cfg, eng::voiceGroupsFor(4), 64);
    r.run(64, 64, {on(0, 1, 100.0f, 0.9f), on(0, 2, 200.0f, 0.2f), on(0, 3, 300.0f, 0.7f), on(0, 4, 400.0f, 0.5f)});
    r.run(64, 64);
    CHECK(r.ve.laneLevel(r.ve.laneOf(2)) < r.ve.laneLevel(r.ve.laneOf(4)));
    r.run(64, 64, {on(r.time + 3, 5, 500.0f)});
    CHECK(r.ve.laneStatus(r.ve.laneOf(2)) == eng::LaneStatus::Fading);
    CHECK(r.ve.laneStatus(r.ve.laneOf(1)) == eng::LaneStatus::Held);
  }
  SUBCASE("none: notes beyond maxVoices are dropped") {
    VoiceConfig cfg;
    cfg.maxVoices = 2;
    cfg.steal = eng::StealPolicy::None;
    Rig r(kProbe, cfg, 1, 64);
    r.run(64, 64, {on(0, 1, 100.0f), on(1, 2, 200.0f), on(2, 3, 300.0f)});
    CHECK(r.ve.laneOf(3) == eng::kNoSlot);
    CHECK(r.ve.stats().droppedNotes == 1);
    CHECK(r.ve.liveVoices() == 2);
  }
}

TEST_CASE("voices: without a free lane the new note waits for the victim's fade (deferred start)") {
  VoiceConfig cfg;
  cfg.maxVoices = 8;  // = lanes: no headroom
  Rig r(kProbe, cfg, 1, 64);
  std::vector<Ev> evs;
  for (uint32_t i = 0; i < 8; ++i) evs.push_back(on(i, i + 1, 100.0f * static_cast<float>(i + 1)));
  r.run(64, 64, evs);
  r.run(64, 64, {on(64 + 10, 9, 950.0f)});  // steals note 1 (lane 0), fade ends at 74 + 144 = 218
  CHECK(r.ve.stats().steals == 1);
  CHECK(r.ve.stats().deferredStarts == 1);
  CHECK(r.ve.laneStatus(0) == eng::LaneStatus::Fading);
  int64_t startBlock = -1;
  uint32_t trig0 = 0;
  r.run(64 * 4, 64, {}, [&](int64_t t0, uint32_t) {
    if (startBlock < 0 && r.ve.laneNote(0) == 9 && r.ve.laneStatus(0) == eng::LaneStatus::Held) {
      startBlock = t0;
      trig0 = std::bit_cast<uint32_t>(r.ve.signal(0, eng::VoiceSignal::Trigger)[0]);
    }
  });
  CHECK(startBlock == 256);  // first block after the fade ended (block 192..255 contains 218)
  CHECK(trig0 == 0xFFFFFFFFu);  // the trigger at frame 0 of that block
}

TEST_CASE("voices: same-note retrigger keeps the voice and its state; otherwise a fresh lane restarts") {
  for (bool retrigger : {true, false}) {
    INFO("retrigger " << retrigger);
    VoiceConfig cfg;
    cfg.retriggerSameNote = retrigger;
    Rig r(kSine, cfg, 2, 64);
    r.run(64, 64, {on(0, 1, 220.0f)});
    const uint32_t lane1 = r.ve.laneOf(1);
    r.run(64, 64, {off(64, 1), on(100, 2, 220.0f, 0.4f)});
    if (retrigger) {
      CHECK(r.ve.laneOf(2) == lane1);
      CHECK(r.ve.stats().retriggers == 1);
      CHECK(r.ve.laneStatus(lane1) == eng::LaneStatus::Held);
      CHECK(r.ve.liveVoices() == 1);
    } else {
      CHECK(r.ve.laneOf(2) != lane1);
      CHECK(r.ve.liveVoices() == 2);
    }
  }
}

TEST_CASE("voices: lane state is reset to StateInit at note-on (Q29), unless disabled") {
  for (bool reset : {true, false}) {
    INFO("reset " << reset);
    VoiceConfig cfg;
    cfg.resetOnNoteOn = reset;
    Rig r(kPhase, cfg, 1, 64);
    REQUIRE(r.ve.config().doneSlot != eng::kNoSlot);
    // Note 1 plays 300 frames in lane 0, ends through `done` the block after its note-off; note 2 reuses lane 0.
    const auto a = r.run(1024, 64, {on(5, 1, 123.0f), off(305, 1), on(600, 2, 123.0f)});
    CHECK(r.ve.laneOf(2) == 0);
    CHECK(r.ve.stats().voicesEnded == 1);
    const auto& ph = a[r.mixIndex("ph")];
    int same = 0;
    for (uint32_t k = 0; k < 300; ++k) same += ph[5 + k] == ph[600 + k] ? 1 : 0;
    if (reset) {
      CHECK(same == 300);  // the same phase trajectory from each onset
      CHECK(ph[600] == 0.0f);
    } else {
      CHECK(same < 300);
    }
  }
}

TEST_CASE("voices: voice end — done slot, silence after release, release timeout without outputs") {
  SUBCASE("done") {
    VoiceConfig cfg;
    Rig r(kPhase, cfg, 1, 64);
    r.run(64, 64, {on(0, 1, 100.0f)});
    r.run(64, 64, {off(64 + 63, 1)});  // done is true at the last frame of this block
    CHECK(r.ve.laneStatus(0) == eng::LaneStatus::Free);
  }
  SUBCASE("held voices are not ended by silence") {
    VoiceConfig cfg;
    cfg.silenceMs = 1.0f;
    Rig r(kProbe, cfg, 1, 64);
    r.run(64 * 20, 64, {on(0, 1, 100.0f, 0.0f)});  // velocity 0: y is silent, but the note is held
    CHECK(r.ve.laneStatus(0) == eng::LaneStatus::Held);
  }
  SUBCASE("timeout") {
    VoiceConfig cfg;
    cfg.releaseTimeoutMs = 10.0f;  // 480 frames
    Rig r(kInternal, cfg, 1, 64);
    CHECK(r.ve.numMixes() == 0);
    const auto out = r.run(64 * 4, 64, {on(0, 1, 100.0f, 0.5f), off(100, 1)});
    CHECK(out.back()[50] == 0.5f);  // global out (the patch's own voice_sum)
    CHECK(out.back()[100] == 0.0f);
    r.run(64 * 6, 64);  // released at 100: 476 frames at the block ending 576, free after the one ending 640
    CHECK(r.ve.laneStatus(0) == eng::LaneStatus::Free);
    CHECK(r.ve.groupsRunLastBlock() == 1);  // the block in which it ended still ran
    r.run(64, 64);
    CHECK(r.ve.groupsRunLastBlock() == 0);
  }
}

TEST_CASE("voices: a stolen voice fades out without a discontinuity; a hard cut would click") {
  // Two held sines; a third note steals the oldest. Bound: the largest step two sines can make on their own.
  auto maxStep = [](float fadeMs, double& natural) {
    VoiceConfig cfg;
    cfg.maxVoices = 2;
    cfg.stealFadeMs = fadeMs;
    Rig r(kSine, cfg, eng::voiceGroupsFor(2), 64);
    const auto out = r.run(4800, 64, {on(0, 1, 1000.0f), on(0, 2, 1500.0f), on(1000, 3, 700.0f), on(2017, 4, 900.0f)});
    const auto& y = out[0];
    // At most three voices sound at once (the fading victim, the other held voice, the new one): 1000 + 1500 + 700.
    natural = 0.3 * 2.0 * 3.141592653589793 * (1000.0 + 1500.0 + 700.0) / kSr;
    double m = 0.0;
    for (size_t t = 1; t < y.size(); ++t) m = std::max(m, static_cast<double>(std::fabs(y[t] - y[t - 1])));
    CHECK(r.ve.stats().steals == 2);
    CHECK(r.rtViolations == 0);
    return m;
  };
  double natural = 0.0;
  const double faded = maxStep(3.0f, natural);
  CHECK(faded <= natural + 0.3 / 144.0 + 1e-3);
  const double cut = maxStep(0.001f, natural);  // a 1-frame "fade"
  CHECK(cut > natural + 0.05);
  MESSAGE("max step: fade 3 ms " << faded << ", 1 frame " << cut << ", natural bound " << natural);
}

TEST_CASE("voices: FmVoice output from each onset does not depend on block boundaries") {
  std::vector<std::string> skipped;
  auto files = compileFile(fs::path(PQ_TEST_DATA_DIR) / "graph" / "cases" / "fm_voice.pqp", kSr, skipped);
  REQUIRE(files.size() == 1);
  const std::vector<Ev> evs = {on(3, 1, 220.0f, 0.5f), on(40, 2, 330.0f, 0.7f), on(129, 3, 277.0f), off(700, 1),
                               on(701, 4, 440.0f),      off(1500, 2),            off(1501, 3),       off(2000, 4)};
  std::vector<float> ref;
  for (uint32_t block : {1u, 13u, 64u, 128u}) {
    INFO("block " << block);
    Rig r(files[0], VoiceConfig{}, 2, 128);
    const auto out = r.run(2400, block, evs);
    CHECK(r.rtViolations == 0);
    if (ref.empty()) {
      ref = out[0];
      double e = 0;
      for (float v : ref) e += v * v;
      CHECK(e > 1.0);
      continue;
    }
    double maxDiff = 0;
    for (size_t t = 0; t < ref.size(); ++t) maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(out[0][t] - ref[t])));
    CHECK(maxDiff <= 1e-6);
  }
}

TEST_CASE("voices: the image skips voice groups without voices (their state holds, outputs untouched)") {
  Compiled c = compileText(kPhase, kSr);
  eng::InterpretOptions io;
  io.maxBlockFrames = 16;
  io.maxVoiceGroups = 2;
  auto img = eng::buildInterpreted(c.sched, io);
  REQUIRE(img.ok());
  std::vector<float> hz(16 * 8, 1000.0f), outs[2];
  const float* sig[eng::kVoiceSignalCount] = {};
  sig[0] = hz.data();
  eng::VoiceGroupIO g[2];
  float* op[2][2];
  for (int k = 0; k < 2; ++k) {
    outs[k].assign(16 * 8, -7.0f);
    op[k][0] = outs[k].data();
    op[k][1] = nullptr;
    g[k].signals = sig;
    g[k].out = op[k];
    g[k].numOut = 2;
  }
  g[0].laneMask = 0;
  g[1].laneMask = 1;
  eng::VoiceTable vt;
  vt.groups = g;
  vt.numGroups = 2;
  eng::ProcessContext ctx;
  ctx.frames = 16;
  ctx.voices = &vt;
  (*img)->process(ctx);
  CHECK(outs[0][5] == -7.0f);  // group 0 did not run
  CHECK(outs[1][15 * 8] > 0.0f);  // group 1 lane 0 advanced
}

TEST_CASE("voices: 64 voices under heavy note traffic — no allocation, consistent bookkeeping") {
  std::vector<std::string> skipped;
  auto files = compileFile(fs::path(PQ_TEST_DATA_DIR) / "graph" / "cases" / "fm_voice.pqp", kSr, skipped);
  REQUIRE(files.size() == 1);
  VoiceConfig cfg;
  cfg.maxVoices = 64;
  Rig r(files[0], cfg, eng::voiceGroupsFor(64), 64);
  uint32_t rng = 12345;
  auto rnd = [&](uint32_t n) {
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % n;
  };
  std::vector<uint32_t> held;
  uint32_t nextId = 1;
  uint32_t maxLive = 0;
  for (int b = 0; b < 1500; ++b) {
    std::vector<Ev> evs;
    for (int k = 0; k < 12; ++k) {
      const int64_t t = r.time + rnd(64);
      const uint32_t what = rnd(10);
      if (what < 5 || held.empty()) {
        const uint32_t id = nextId++;
        evs.push_back(on(t, id, 55.0f * std::exp2(static_cast<float>(rnd(6000)) / 1200.0f), 0.2f + 0.01f * static_cast<float>(rnd(80))));
        held.push_back(id);
      } else if (what < 9) {
        const uint32_t i = rnd(static_cast<uint32_t>(held.size()));
        evs.push_back(off(t, held[i]));
        held.erase(held.begin() + i);
      } else {
        evs.push_back(pitch(t, held[rnd(static_cast<uint32_t>(held.size()))], 300.0f, rnd(200)));
      }
    }
    if (b % 300 == 299) evs.push_back({r.time + 63, NoteEvent{0, eng::NoteEventKind::AllSoundOff}});
    std::sort(evs.begin(), evs.end(), [](const Ev& x, const Ev& y) { return x.t < y.t; });
    const auto out = r.run(64, 64, evs);
    for (float v : out[0]) REQUIRE(std::isfinite(v));
    maxLive = std::max(maxLive, r.ve.liveVoices());
    uint32_t live = 0;
    for (uint32_t l = 0; l < r.ve.lanes(); ++l)
      live += (r.ve.laneStatus(l) == eng::LaneStatus::Held || r.ve.laneStatus(l) == eng::LaneStatus::Released ? 1u : 0u) +
              (r.ve.hasDeferredNote(l) ? 1u : 0u);
    REQUIRE(live == r.ve.liveVoices());
    REQUIRE(live <= 64);
  }
  CHECK(r.rtViolations == 0);
  CHECK(maxLive == 64);
  const eng::VoiceStats& s = r.ve.stats();
  CHECK(s.steals > 0);
  CHECK(s.voicesEnded > 0);
  CHECK(s.droppedEvents == 0);
  MESSAGE("noteOns " << s.noteOns << " steals " << s.steals << " deferred " << s.deferredStarts << " ended "
                     << s.voicesEnded << " retriggers " << s.retriggers << " unmatched " << s.unmatchedEvents);
}

// A short linear attack/release envelope over two detuned oscillators (z1 loop, phasors, sines).
const char* kPluck = R"(
patch Pluck {
  out y : audio voice
  rate = voice.gate ? 0.01 : -0.002
  level = z1(clip(level + rate, 0, 1))
  y = (sine(phasor(voice.hz)) + sine(phasor(voice.hz * 1.003))) * level * 0.2
}
)";

TEST_CASE("voices: compaction moves voices between groups without changing the sound") {
  // Random traffic: bursts of notes with random lengths, 4 s.
  std::vector<Ev> evs;
  uint32_t rng = 2024, id = 1;
  auto rnd = [&](uint32_t n) {
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % n;
  };
  std::vector<std::pair<int64_t, uint32_t>> offs;
  for (int64_t t = 0; t < 4 * 48000; t += 997 + rnd(3000)) {
    const uint32_t n = 1 + rnd(4);
    for (uint32_t k = 0; k < n; ++k, ++id) {
      evs.push_back(on(t + rnd(64), id, 80.0f * std::exp2(static_cast<float>(rnd(4800)) / 1200.0f), 0.5f));
      offs.push_back({t + 200 + rnd(30000), id});
    }
  }
  for (const auto& o : offs) evs.push_back(off(o.first, o.second));
  std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
  for (int which = 0; which < 2; ++which) {
    const std::string name = which == 0 ? "Pluck" : "Echo";
    INFO(name);
    std::vector<float> out[2];
    uint64_t moves[2] = {}, groupsRun[2] = {};
    for (int compact = 0; compact < 2; ++compact) {
      VoiceConfig cfg;
      cfg.compact = compact == 1;
      cfg.silenceMs = 5.0f;
      auto r = std::make_unique<Rig>(which == 0 ? kPluck : kEcho, cfg, eng::voiceGroupsFor(64), 64);
      out[compact] = r->run(4 * 48000 + 24000, 64, evs, [&](int64_t, uint32_t) { groupsRun[compact] += r->ve.groupsRunLastBlock(); })[0];
      moves[compact] = r->ve.stats().voiceMoves;
      CHECK(r->rtViolations == 0);
      CHECK(r->ve.stats().steals == 0);
    }
    CHECK(moves[0] == 0);
    CHECK(moves[1] > 20);
    CHECK(groupsRun[1] < groupsRun[0]);
    double maxDiff = 0, energy = 0;
    for (size_t t = 0; t < out[0].size(); ++t) {
      maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(out[0][t] - out[1][t])));
      energy += out[0][t] * out[0][t];
    }
    CHECK(energy > 10.0);
    CHECK(maxDiff <= 1e-5);  // only the summation order of the mix differs
    MESSAGE(name << ": moves " << moves[1] << ", groups run " << groupsRun[0] << " -> "
                                               << groupsRun[1] << ", max diff " << maxDiff);
  }
}

TEST_CASE("voices: prepare rejects bad configurations") {
  Compiled c = compileText(kProbe, kSr);
  eng::InterpretOptions io;
  io.maxBlockFrames = 64;
  io.maxVoiceGroups = 1;
  auto img = eng::buildInterpreted(c.sched, io);
  REQUIRE(img.ok());
  eng::VoiceEngine ve;
  VoiceConfig cfg;
  cfg.maxVoices = 9;  // > 8 lanes
  CHECK_FALSE(ve.prepare(img->get(), cfg).ok());
  cfg.maxVoices = 0;
  CHECK_FALSE(ve.prepare(img->get(), cfg).ok());
  cfg.maxVoices = 8;
  CHECK(ve.prepare(img->get(), cfg).ok());
  CHECK_FALSE(ve.prepare(nullptr, cfg).ok());
  Compiled global = compileText("patch G {\n  out y : audio\n  y = 1\n}\n", kSr);
  auto gimg = eng::buildInterpreted(global.sched, io);
  REQUIRE(gimg.ok());
  CHECK_FALSE(ve.prepare(gimg->get(), cfg).ok());
}
