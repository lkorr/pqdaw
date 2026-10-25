// T2.6: tuning as a signal end to end — Degree notes (NoteEvent::degree / equave, from the timeline's PitchSource)
// → voice.degree / voice.equave → a track patch's tuning node (tuning_hz + tune_voices) → the instrument's voice.hz.
// A generator sweep through several MOS regions gives every voice the pitch buildMOSScale gives its degree at that
// sample. Runs on the interpreter (pq_tests_engine) and on JIT images (pq_tests_jit sets rigImageBuilder).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "interp_harness.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/time.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/note_events.h"
#include "pq/engine/sequence.h"
#include "pq/engine/timeline.h"
#include "pq/engine/tuning_signal.h"
#include "pq/engine/voices.h"
#include "pq/tuning/mos_tuning.h"
#include "tuning_signal_ref.h"

using namespace interp_test;

namespace {

constexpr double kSr = 48000.0;
constexpr double kRef = 261.626;

// A track patch: the tuning node feeds the instrument (Synth reads plain voice.hz / voice.cents).
const char* kTrack = R"(
patch Track {
  in gen : audio global
  param per = 1200 [100..2400]
  param st = 2.5 [-100..100]
  param ref = 261.626 [20..2000]
  out f : audio voice
  out c : audio voice
  out y : audio voice
  t = tuning_hz(voice.degree, voice.equave, voice.hz, voice.cents, gen, per, st, ref, table: 0)
  tv = tune_voices(t.hz, t.cents)
  s = Synth()
  f = s.f
  c = s.c
  y = s.y
}
patch Synth {
  out f : audio voice
  out c : audio voice
  out y : audio voice
  f = voice.hz
  c = voice.cents
  y = sine(phasor(voice.hz)) * gate_to_real(voice.gate) * 0.2
}
)";

pq::tuning::MosParams sweepMos() {
  pq::tuning::MosParams p;
  p.n = 7;
  p.period = 1200.0;
  p.rotation = 2;
  p.octaveStretch = 2.5;
  p.generator = 700.0;
  return p;
}

// The compiled track, its tuning table and a VoiceEngine on it.
struct TunedRig {
  Compiled c;
  pq::dsp::TuningTable table;
  std::vector<const pq::dsp::TuningTableView*> views;
  eng::Resources res;
  std::unique_ptr<eng::ProgramImage> img;
  eng::VoiceEngine ve;
  eng::EventBuffer buf{256};
  std::vector<float> gen;   // the generator sweep, one value per sample
  uint32_t f = 0, cents = 0;  // mix indices
  uint64_t rtViolations = 0;

  explicit TunedRig(uint32_t frames) : c(compileText(kTrack, kSr)) {
    auto m = pq::tuning::Mos::make(sweepMos());
    REQUIRE(m.ok());
    auto t = pq::tuning::Tuning::make(m.value(), kRef);
    REQUIRE(t.ok());
    auto tab = eng::makeTuningTable(t.value());
    REQUIRE(tab.ok());
    table = std::move(tab.value());
    views = {&table.view()};
    res.tunings = views;
    eng::InterpretOptions io;
    io.maxBlockFrames = 128;
    io.maxVoiceGroups = 2;
    io.resources = &res;
    auto built = buildRigImage(c.sched, io);
    REQUIRE_MESSAGE(built.ok(), (built.ok() ? "" : built.error().message));
    img = std::move(built.value());
    eng::VoiceConfig cfg;
    cfg.maxVoices = 8;
    const Patch& sp = c.sched.rewritten ? c.sched.rewritten->patch : c.typed.patch;
    cfg.monitorMask = 1u << eng::findVoiceSlot(img->io(), sp, "y");
    REQUIRE(ve.prepare(img.get(), cfg).ok());
    for (uint32_t i = 0; i < ve.numMixes(); ++i) {
      const auto name = sp.nodeName(img->io().voiceOut[ve.mixSlot(i)].node);
      if (name == "f") f = i;
      if (name == "c") cents = i;
    }
    gen.resize(frames);
    for (uint32_t s = 0; s < frames; ++s) gen[s] = static_cast<float>(450.0 + 400.0 * s / (frames - 1));
  }

  // One note from frame 0 for `frames` samples; returns the f and c mixes per sample.
  void run(const eng::NoteEvent& on, uint32_t frames, std::vector<float>& outF, std::vector<float>& outC) {
    pq::DenormalGuard dg;
    const float* in[1] = {nullptr};
    for (uint32_t done = 0; done < frames;) {
      const uint32_t n = std::min(128u, frames - done);
      buf.clear();
      if (done == 0) buf.push(on);
      in[0] = gen.data() + done;
      eng::ProcessContext ctx;
      ctx.frames = n;
      ctx.sampleRate = kSr;
      ctx.sampleTime = done;
      ctx.in = in;
      ctx.numIn = 1;
      ctx.events = &buf.list();
      rtViolations += pq::alloc_trap::countRtViolations([&] { ve.process(ctx); });
      outF.insert(outF.end(), ve.mix(f), ve.mix(f) + n);
      outC.insert(outC.end(), ve.mix(cents), ve.mix(cents) + n);
      done += n;
    }
  }
};

}  // namespace

TEST_CASE("tuning voices: a generator sweep through several regions gives every Degree voice buildMOSScale's pitch") {
  constexpr uint32_t kFrames = 6000;
  struct NoteSpec {
    int32_t degree, equave;
  };
  const NoteSpec notes[] = {{0, 0}, {1, 0}, {3, 0}, {6, -1}, {4, 1}, {2, 2}};
  double maxErr = 0.0;
  uint64_t checked = 0, skipped = 0, badHz = 0, badCents = 0;
  std::vector<std::optional<pq::tuning::Mos>> ref(kFrames);  // buildMOSScale at every sample's generator
  {
    TunedRig r(kFrames);
    for (uint32_t t = 0; t < kFrames; ++t) ref[t] = tuning_ref::mosAt(sweepMos(), r.gen[t]);
  }
  for (const NoteSpec& ns : notes) {
    CAPTURE(ns.degree);
    CAPTURE(ns.equave);
    TunedRig r(kFrames);  // a fresh engine per note: the mix is that one voice
    // The note-on carries the note's static Hz in the definition, as the timeline gives it (T2.6-fix3: the node
    // reads it, a Degree note's hz over its static degree pitch being its ratio / glide).
    const auto staticHz = pq::tuning::resolveHzDouble(pq::tuning::PitchId::fromDegree(ns.degree, ns.equave),
                                                      pq::tuning::Tuning::make(pq::tuning::Mos::make(sweepMos()).value(), kRef).value(), {});
    REQUIRE(staticHz);
    eng::NoteEvent on = eng::noteOn(0, 1, static_cast<float>(*staticHz), 0.8f);
    on.degree = static_cast<float>(ns.degree);
    on.equave = static_cast<float>(ns.equave);
    std::vector<float> f, c;
    r.run(on, kFrames, f, c);
    CHECK(r.rtViolations == 0);
    REQUIRE(f.size() == kFrames);
    for (uint32_t t = 0; t < kFrames; ++t) {
      if (!ref[t]) {
        ++skipped;
        continue;
      }
      const double cents = ref[t]->centsOf(ns.degree, ns.equave);
      const double refHz = kRef * std::exp2(cents / 1200.0);
      badHz += !tuning_ref::withinTolerance(f[t], refHz);
      // voice.cents follows too: the engine's cents of the note's float32 Hz moved by the tuning (T2.6-fix3), so it
      // carries that Hz's rounding (≤ 1.03e-4 cents).
      badCents += std::fabs(c[t] - cents) > std::fabs(cents) * 1.2e-7 + 2e-4;
      maxErr = std::max(maxErr, tuning_ref::centsError(f[t], refHz));
      ++checked;
    }
  }
  MESSAGE("voice.hz from the tuning node vs buildMOSScale: " << checked << " samples, max error " << maxErr
                                                             << " cents (incl. float32 rounding), " << skipped
                                                             << " boundary samples skipped");
  CHECK(badHz == 0);
  CHECK(badCents == 0);
  CHECK(skipped * 1000 < checked);
}

TEST_CASE("tuning voices: notes without a Degree identity keep their static voice.hz") {
  constexpr uint32_t kFrames = 512;
  TunedRig r(kFrames);
  std::vector<float> f, c;
  r.run(eng::noteOn(0, 1, 333.0f, 0.8f), kFrames, f, c);  // degree / equave NaN (the default)
  CHECK(r.rtViolations == 0);
  for (float x : f) CHECK(x == 333.0f);
}

TEST_CASE("tuning voices: the timeline's PitchSource reaches the note-on as degree / equave") {
  eng::TimelineConfig cfg;
  cfg.sampleRate = kSr;
  cfg.maxBlockFrames = 128;
  cfg.maxTracks = 1;
  eng::Timeline tl(cfg);
  eng::NoteDesc a, b;
  a.id = 1;
  a.start = 0;
  a.length = pq::kPpq;
  a.hz = 300.0f;
  a.pitch = {7, 3, -1};  // tuning 7: a Degree note (degree 3, equave −1)
  a.pitch.node = 2;      // T2.6-fix3: the track patch's second tuning node
  b = a;
  b.id = 2;
  b.start = pq::kPpq / 2;
  b.pitch = {};  // static
  eng::ClipDesc clip;
  clip.id = 1;
  clip.notes = {a, b};
  const std::vector<eng::TrackSequenceDesc> tracks{{1, {clip}}};
  auto img = eng::SequenceImage::build(tracks);
  REQUIRE(img.ok());
  REQUIRE(tl.publishSequence(std::move(img.value())));
  eng::TransportCommand play;
  play.kind = eng::TransportCommandKind::Play;
  tl.apply(play);
  std::vector<eng::NoteEvent> ons;
  for (int64_t t = 0; t < 48000; t += 128) {
    tl.process(t, 128);
    const eng::EventList l = tl.events(0);
    for (uint32_t i = 0; i < l.count; ++i)
      if (l.events[i].kind == eng::NoteEventKind::NoteOn) ons.push_back(l.events[i]);
  }
  REQUIRE(ons.size() == 2);
  CHECK(ons[0].degree == 3.0f);
  CHECK(ons[0].equave == -1.0f);
  CHECK(ons[0].tuning == 2.0f);
  CHECK(ons[1].tuning == 0.0f);
  CHECK(std::isnan(ons[1].degree));
  CHECK(std::isnan(ons[1].equave));
}
