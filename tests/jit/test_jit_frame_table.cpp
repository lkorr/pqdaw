// TMM.2: wavetable_pos and voice_tap through whole images — interpreter and JIT bit-identical (global and voice
// patches, through table swaps), both within 1e-6 of TMM.1's double reference, voice taps through a VoiceEngine (lane
// masks from the image host, the phases read equal to the phases sampled), RtScope on every process() with swaps and
// taps, and the reader state + table generation carried across a hot swap. TMM.12-fix1: wavetable_pos in per-sample
// loops, whose frames after a block's first the JIT computes inline on sine tables (codegen.cpp emitWavetableSine).
#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../dsp/frame_table_fixtures.h"
#include "../engine/interp_harness.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/core/voice_tap.h"
#include "pq/dsp/frame_table.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/dsp/frame_table_slot.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/note_events.h"
#include "pq/engine/program_host.h"
#include "pq/engine/voices.h"
#include "pq/jit/compiler.h"

using namespace interp_test;
namespace jit = pq::jit;
namespace dsp = pq::dsp;

namespace {

using TablePtr = std::shared_ptr<const dsp::FrameTable>;

jit::Compiler& compiler() {
  static std::unique_ptr<jit::Compiler> c = [] {
    auto r = jit::Compiler::create();
    REQUIRE(r.ok());
    return std::move(r.value());
  }();
  return *c;
}

TablePtr build(std::vector<ftfix::Cycle> cycles, uint32_t dense = 64) {
  dsp::FrameTableBuilder b;
  ftfix::SpecHolder s(std::move(cycles), 48000, dense);
  auto t = b.build(s.spec);
  REQUIRE(t.ok());
  return std::make_shared<const dsp::FrameTable>(std::move(t.value()));
}
const TablePtr& tableA() {
  static const TablePtr t = build(ftfix::drawnFixture(8));
  return t;
}
const TablePtr& tableB() {
  static const TablePtr t = build({ftfix::basic(dsp::BasicCycle::Saw), ftfix::basic(dsp::BasicCycle::Square)});
  return t;
}

// One backend's resources: frame-table slot 1 (table A first) and voice tap 2, both read by a registered reader.
struct Rig {
  pq::EpochReclaimer rec{1};
  pq::EpochReclaimer::ReaderId reader{};
  std::unique_ptr<dsp::FrameTableSlot> slot;
  std::shared_ptr<pq::VoiceTap> tap;
  std::vector<const dsp::FrameTableSlot*> slots;
  std::vector<pq::VoiceTap*> taps;
  eng::Resources res;
  explicit Rig(uint32_t groups = 1, bool read = true, TablePtr initial = nullptr) {
    reader = *rec.registerReader();
    pq::VoiceTapConfig tc;
    tc.groups = groups;
    tc.stride = 8;
    slot = std::make_unique<dsp::FrameTableSlot>(initial ? initial : tableA(), tc);
    tap = std::make_shared<pq::VoiceTap>(tc);
    slots = {nullptr, slot.get()};
    taps = {nullptr, nullptr, tap.get()};
    res.frameTables = slots;
    res.voiceTaps = taps;
    if (read) {
      slot->tap()->addReader();
      tap->addReader();
    }
  }
  ~Rig() { rec.unregisterReader(reader); }
  void swap(const TablePtr& t) {
    slot->publish(t, rec);
    rec.collect();
  }
};

pq::Result<std::unique_ptr<eng::ProgramImage>> buildImage(int backend, const Schedule& s, const eng::Resources& res,
                                                          uint32_t maxBlock, uint32_t groups,
                                                          jit::VoiceCode vc = jit::VoiceCode::Auto,
                                                          jit::CompileStats* stats = nullptr) {
  if (backend == 0) {
    eng::InterpretOptions io;
    io.maxBlockFrames = maxBlock;
    io.maxVoiceGroups = groups;
    io.resources = &res;
    return eng::buildInterpreted(s, io);
  }
  jit::CompileOptions o;
  o.maxBlockFrames = maxBlock;
  o.maxVoiceGroups = groups;
  o.resources = &res;
  o.voiceCode = vc;
  return compiler().compile(s, o, stats);
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

std::vector<pq::VoiceTapRecord> records(const pq::VoiceTap& t, uint32_t group) {
  std::vector<pq::VoiceTapRecord> out;
  const uint64_t w = t.written(group);
  for (uint64_t i = w > t.depth() - 1 ? w - (t.depth() - 1) : 0; i < w; ++i) {
    pq::VoiceTapRecord r;
    REQUIRE(t.read(group, i, r));
    out.push_back(r);
  }
  return out;
}

const char* kGlobal = R"(
patch G {
  in ph : audio
  in u : audio
  param pos = 0.4 [0..1]
  out y : audio
  out z : audio
  y = wavetable_pos(ph, pos, 1)
  z = wavetable_pos(ph, u, 1)
}
)";

}  // namespace

TEST_CASE("wavetable_pos (global): interpreter and JIT bit-identical through swaps, both ≤ 1e-6 from the reference; RtScope") {
  const Compiled c = compileText(kGlobal);
  constexpr uint32_t kBlock = 37, kN = 37 * 120;
  std::vector<float> phase(kN), u(kN);
  double acc = 0.1;
  for (uint32_t n = 0; n < kN; ++n) {
    phase[n] = static_cast<float>(acc);
    acc += (110.0 + 2000.0 * n / kN) / 48000.0;  // a sweep: every level on the way
    acc -= std::floor(acc);
    u[n] = static_cast<float>(0.5 + 0.5 * std::sin(n * 0.003));
  }
  std::vector<float> outs[2][2];
  uint64_t violations = 0;
  for (int backend = 0; backend < 2; ++backend) {
    CAPTURE(backend);
    Rig rig;
    auto img = buildImage(backend, c.sched, rig.res, 64, 1);
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    const eng::ImageInterface& face = (*img)->io();
    REQUIRE(face.globalIn.size() == 2);
    REQUIRE(face.globalOut.size() == 2);
    std::vector<float> in[2] = {std::vector<float>(64), std::vector<float>(64)}, out[2] = {std::vector<float>(64), std::vector<float>(64)};
    const float* inP[2] = {in[0].data(), in[1].data()};
    float* outP[2] = {out[0].data(), out[1].data()};
    for (uint32_t t0 = 0, blk = 0; t0 < kN; t0 += kBlock, ++blk) {
      if (blk == 40) rig.swap(tableB());
      if (blk == 41) rig.swap(tableA());  // inside the first fade (37-frame blocks): it waits (AU.fold), then fades on
      if (blk == 80) rig.swap(tableB());
      for (uint32_t s = 0; s < 2; ++s)
        std::copy_n((face.globalIn[s].ordinal == 0 ? phase : u).begin() + t0, kBlock, in[s].begin());
      eng::ProcessContext ctx;
      ctx.frames = kBlock;
      ctx.sampleRate = 48000.0;
      ctx.sampleTime = t0;
      ctx.in = inP;
      ctx.numIn = 2;
      ctx.out = outP;
      ctx.numOut = 2;
      violations += pq::alloc_trap::countRtViolations([&] {
        pq::DenormalGuard dg;
        (*img)->process(ctx);
      });
      rig.rec.quiescent(rig.reader);
      const pq::graph::Patch& P = c.sched.rewritten ? c.sched.rewritten->patch : c.typed.patch;
      for (uint32_t s = 0; s < 2; ++s) {
        const uint32_t k = P.nodeName(face.globalOut[s].node) == "z" ? 1u : 0u;
        outs[backend][k].insert(outs[backend][k].end(), out[s].begin(), out[s].begin() + kBlock);
      }
    }
    CHECK(rig.slot->tap()->written(0) > 0);  // global op: group 0, lane 0
  }
  CHECK(violations == 0);
  CHECK(sameBits(outs[0][0], outs[1][0]));
  CHECK(sameBits(outs[0][1], outs[1][1]));
  // Before the first swap, against the double reference (position 0.4 as a pattern; u per sample).
  for (const bool perSample : {false, true}) {
    dsp::FrameReadState ref{};
    std::vector<double> want(40 * kBlock);
    const std::vector<float> pos(perSample ? u : std::vector<float>(8, 0.4f));
    for (uint32_t b = 0; b < 40; ++b)
      dsp::readFrameTableReferenceBlock(tableA()->view(), ref, phase.data() + b * kBlock,
                                        pos.data() + (perSample ? b * kBlock : 0), perSample, 1, kBlock,
                                        want.data() + b * kBlock);
    double worst = 0;
    for (uint32_t n = 0; n < 40 * kBlock; ++n) worst = std::max(worst, std::fabs(outs[1][perSample ? 1 : 0][n] - want[n]));
    const std::string what = std::string(perSample ? "per-sample" : "param") + " position: JIT vs reference " + std::to_string(worst);
    MESSAGE(what);
    CHECK(worst <= 1e-6);
  }
}

TEST_CASE("wavetable_pos / voice_tap (voices): interpreter == JIT, taps with lane masks, phases read == phases sampled") {
  const Compiled c = compileText(R"(
patch V {
  param pos = 0.3 [0..1]
  out y : audio voice
  ph = phasor(voice.hz)
  y = wavetable_pos(ph, frac(pos + ph * 0.25), 1) * gate_to_real(voice.gate) * 0.2
  t = voice_tap(ph, 2)
}
)");
  constexpr uint32_t kBlock = 64, kBlocks = 300, kGroups = 2;
  std::vector<float> mix[2];
  std::vector<pq::VoiceTapRecord> wpRec[2], vtRec[2];
  uint32_t maskBits[2] = {0, 0};
  uint64_t violations = 0;
  for (int backend = 0; backend < 2; ++backend) {
    CAPTURE(backend);
    Rig rig(kGroups);
    auto img = buildImage(backend, c.sched, rig.res, kBlock, kGroups);
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    eng::VoiceEngine ve;
    eng::VoiceConfig cfg;
    cfg.maxVoices = 16;
    REQUIRE(ve.prepare(img->get(), cfg).ok());
    eng::EventBuffer buf(64);
    for (uint32_t b = 0; b < kBlocks; ++b) {
      if (b == 100) rig.swap(tableB());
      if (b == 200) rig.swap(tableA());
      buf.clear();
      if (b == 0) {
        buf.push(eng::noteOn(0, 1, 110.0f, 0.7f));
        buf.push(eng::noteOn(5, 2, 165.0f, 0.7f));
        buf.push(eng::noteOn(9, 3, 220.0f, 0.7f));
      }
      if (b == 150) buf.push(eng::noteOn(3, 4, 275.0f, 0.7f));
      eng::ProcessContext ctx;
      ctx.frames = kBlock;
      ctx.sampleRate = 48000.0;
      ctx.sampleTime = static_cast<int64_t>(b) * kBlock;
      ctx.events = &buf.list();
      violations += pq::alloc_trap::countRtViolations([&] {
        pq::DenormalGuard dg;
        ve.process(ctx);
      });
      rig.rec.quiescent(rig.reader);
      mix[backend].insert(mix[backend].end(), ve.mix(0), ve.mix(0) + kBlock);
      if (b == 120) {  // three voices: the records' masks say so
        pq::VoiceTapRecord r;
        REQUIRE(rig.tap->read(0, rig.tap->written(0) - 1, r));
        maskBits[0] = static_cast<uint32_t>(std::popcount(r.laneMask));
      }
      if (b == 250) {
        pq::VoiceTapRecord r;
        REQUIRE(rig.slot->tap()->read(0, rig.slot->tap()->written(0) - 1, r));
        maskBits[1] = static_cast<uint32_t>(std::popcount(r.laneMask));
      }
    }
    CHECK(maskBits[0] == 3);
    CHECK(maskBits[1] == 4);
    CHECK(rig.tap->written(0) == kBlocks);  // one record per 64-frame block (stride 8)
    CHECK(rig.tap->written(1) == 0);        // group 1 holds no voice: never run, never written
    wpRec[backend] = records(*rig.slot->tap(), 0);
    vtRec[backend] = records(*rig.tap, 0);
  }
  CHECK(violations == 0);
  double energy = 0;
  for (float x : mix[0]) energy += double(x) * x;
  CHECK(energy > 1.0);
  CHECK(sameBits(mix[0], mix[1]));
  REQUIRE(wpRec[0].size() == wpRec[1].size());
  REQUIRE(vtRec[0].size() == wpRec[0].size());
  bool tapsEqual = true, readEqualsSampled = true;
  for (size_t i = 0; i < wpRec[0].size(); ++i) {
    tapsEqual = tapsEqual && std::memcmp(&wpRec[0][i], &wpRec[1][i], sizeof(pq::VoiceTapRecord)) == 0 &&
                std::memcmp(&vtRec[0][i], &vtRec[1][i], sizeof(pq::VoiceTapRecord)) == 0;
    // wavetable_pos records the phase it read; voice_tap(ph) samples the same phasor at the same frames.
    for (uint32_t l = 0; l < 8; ++l) {
      if (((vtRec[0][i].laneMask >> l) & 1u) == 0) continue;
      for (uint32_t s = 0; s < pq::kVoiceTapSamples; ++s)
        readEqualsSampled = readEqualsSampled && wpRec[0][i].sample[s][l] == vtRec[0][i].sample[s][l];
    }
  }
  CHECK(tapsEqual);
  CHECK(readEqualsSampled);
}

TEST_CASE("wavetable_pos: no reader, no tap writes, the same audio") {
  const Compiled c = compileText(kGlobal);
  std::vector<float> outs[2];
  for (int read = 0; read < 2; ++read) {
    Rig rig(1, read != 0);
    auto img = buildImage(1, c.sched, rig.res, 64, 1);
    REQUIRE(img.ok());
    std::vector<float> in(64), out0(64), out1(64);
    const float* inP[2] = {in.data(), in.data()};
    float* outP[2] = {out0.data(), out1.data()};
    for (uint32_t b = 0; b < 50; ++b) {
      for (uint32_t f = 0; f < 64; ++f) in[f] = std::fmod(0.0091f * static_cast<float>(b * 64 + f), 1.0f);
      eng::ProcessContext ctx;
      ctx.frames = 64;
      ctx.sampleRate = 48000.0;
      ctx.in = inP;
      ctx.numIn = 2;
      ctx.out = outP;
      ctx.numOut = 2;
      (*img)->process(ctx);
      outs[read].insert(outs[read].end(), out0.begin(), out0.end());
    }
    CHECK(rig.slot->tap()->written(0) == (read ? 50u * 2u : 0u));  // two ops share the slot's tap (y and z)
  }
  CHECK(sameBits(outs[0], outs[1]));
}

TEST_CASE("wavetable_pos: the reader state and table generation carry across a hot swap (bit-exact vs no swap)") {
  const char* text = R"(
patch H {
  param level = 0.5 [0..1]
  out y : audio
  y = wavetable_pos(phasor(220.5), 0.6, 1) * level
}
)";
  auto compile = [&](const pq::graph::StateLayout* previous) {
    ParseResult pr = parseDetailed(text, "h.pqp");
    REQUIRE(pr.ok());
    InferOptions io;
    auto inf = infer(pr.file.patches[0], io);
    REQUIRE(inf.ok());
    OptimiseOptions oo;
    oo.sampleRate = 48000.0;
    auto opt = optimise(std::move(inf.value()), oo);
    REQUIRE(opt.ok());
    ScheduleOptions so;
    so.sampleRate = 48000.0;
    so.previous = previous;
    auto s = schedule(opt.value(), so);
    REQUIRE(s.ok());
    return std::move(s.value());
  };
  const Schedule sa = compile(nullptr);
  const Schedule sb = compile(&sa.state);
  for (int backend = 0; backend < 2; ++backend) {
    CAPTURE(backend);
    std::vector<float> outs[2];
    for (int swapped = 0; swapped < 2; ++swapped) {
      Rig rig;
      eng::HostConfig hc;
      hc.sampleRate = 48000.0;
      hc.maxBlockFrames = 64;
      hc.numOut = 1;
      eng::ProgramHost host(hc);
      auto first = buildImage(backend, sa, rig.res, 64, 1);
      REQUIRE(first.ok());
      REQUIRE(host.publish(std::move(first.value())).ok());
      std::vector<float> b0(64);
      float* o[1] = {b0.data()};
      for (uint32_t blk = 0; blk < 200; ++blk) {
        if (blk == 30) rig.swap(tableB());  // generation 2 before the image swap
        if (blk == 60 && swapped) {
          auto next = buildImage(backend, sb, rig.res, 64, 1);
          REQUIRE(next.ok());
          REQUIRE(host.publish(std::move(next.value()), eng::SwapPolicy{eng::SwapMode::Instant, 10.0f}).ok());
        }
        if (blk == 61 && swapped) {  // the new image's state: generation 2 (not fresh)
          const eng::ProgramImage* img = host.current();
          REQUIRE(img != nullptr);
          bool found = false;
          for (const auto& e : img->stateLayout().entries)
            if (e.tag.kernel == static_cast<uint16_t>(dsp::Atom::WavetablePos)) {
              dsp::WavetablePosState st;
              std::memcpy(&st, const_cast<eng::ProgramImage*>(img)->stateArena().global + e.offset, sizeof(st));
              CHECK(st.generation == 2);
              found = true;
            }
          CHECK(found);
        }
        eng::AudioBlock ab;
        ab.out = o;
        ab.numOut = 1;
        ab.frames = 64;
        ab.frameTime = static_cast<int64_t>(blk) * 64;
        host.process(ab);
        rig.rec.quiescent(rig.reader);
        outs[swapped].insert(outs[swapped].end(), b0.begin(), b0.end());
      }
      CHECK(host.stats().swaps == (swapped ? 2u : 1u));
    }
    CHECK(sameBits(outs[0], outs[1]));
  }
}

// ---- TMM.12-fix1: wavetable_pos in per-sample loops (the frames after a block's first, inline on sine tables) ------

namespace {

const TablePtr& sineOne() {
  static const TablePtr t = build({ftfix::basic(dsp::BasicCycle::Sine)}, 1);
  return t;
}
const TablePtr& sineMorph() {  // 9 frames, every one a sine: the coefficients per lane (frame 0 at position 0)
  static const TablePtr t = build({ftfix::basic(dsp::BasicCycle::Sine), ftfix::fromHarmonics({{1, {0.6, 1.3}}})}, 9);
  return t;
}
// sineOne without its sine coefficients: every read is the rows' (the general path).
const TablePtr& sineRows() {
  static const TablePtr t = [] {
    dsp::FrameTableBuilder b;
    ftfix::SpecHolder s({ftfix::basic(dsp::BasicCycle::Sine)}, 48000, 1);
    auto r = b.build(s.spec);
    REQUIRE(r.ok());
    dsp::FrameTable table = std::move(r.value());
    table.setSine(false);
    return std::make_shared<const dsp::FrameTable>(std::move(table));
  }();
  return t;
}

// The Meta Operator's operator D: the read inside its own feedback loop (z1 of its output), so a per-sample loop.
const char* kFeedbackVoice = R"(
patch F {
  param fb = 0.6 [0..1]
  param pos = 0 [0..1]
  out y : audio voice
  ph = phasor(voice.hz)
  d = wavetable_pos(frac(ph + z1(d) * fb), pos, 1)
  y = d * gate_to_real(voice.gate) * 0.2
}
)";

std::vector<float> renderFeedback(int backend, jit::VoiceCode vc, const Schedule& s, uint64_t& violations,
                                  jit::CompileStats* stats) {
  constexpr uint32_t kBlock = 64, kBlocks = 240, kGroups = 2;
  Rig rig(kGroups, false, sineOne());
  auto img = buildImage(backend, s, rig.res, kBlock, kGroups, vc, stats);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  eng::VoiceEngine ve;
  eng::VoiceConfig cfg;
  cfg.maxVoices = 16;
  REQUIRE(ve.prepare(img->get(), cfg).ok());
  eng::EventBuffer buf(64);
  std::vector<float> mix;
  for (uint32_t b = 0; b < kBlocks; ++b) {
    if (b == 40) rig.swap(sineMorph());               // many sine frames: still inline
    if (b == 80) rig.swap(tableB());                  // the rows (saw → square): the kernel every frame
    if (b == 81) rig.swap(sineOne());                 // inside that fade: it waits, then fades on; inline again after
    if (b == 120) rig.slot->tap()->addReader();       // the kernel writes the tap: every frame calls it
    if (b == 160) rig.slot->tap()->removeReader();
    if (b == 200) rig.swap(sineMorph());
    buf.clear();
    if (b == 0)
      for (uint32_t v = 0; v < 12; ++v)  // two groups
        buf.push(eng::noteOn(v % 7, v + 1, 110.0f * std::exp2(static_cast<float>(v) / 5.0f), 0.7f));
    if (b == 100) buf.push(eng::noteOn(17, 13, 1234.5f, 0.7f));
    eng::ProcessContext ctx;
    ctx.frames = kBlock;
    ctx.sampleRate = 48000.0;
    ctx.sampleTime = static_cast<int64_t>(b) * kBlock;
    ctx.events = &buf.list();
    violations += pq::alloc_trap::countRtViolations([&] {
      pq::DenormalGuard dg;
      ve.process(ctx);
    });
    rig.rec.quiescent(rig.reader);
    mix.insert(mix.end(), ve.mix(0), ve.mix(0) + kBlock);
  }
  return mix;
}

}  // namespace

TEST_CASE("wavetable_pos in a per-sample loop: inline sine frames — interpreter == JIT (groups, pairs) through swaps and a tap reader; RtScope") {
  const Compiled c = compileText(kFeedbackVoice);
  uint64_t violations = 0;
  jit::CompileStats groups, pairs;
  const std::vector<float> yi = renderFeedback(0, jit::VoiceCode::Auto, c.sched, violations, nullptr);
  const std::vector<float> yg = renderFeedback(1, jit::VoiceCode::Groups, c.sched, violations, &groups);
  const std::vector<float> yp = renderFeedback(1, jit::VoiceCode::Auto, c.sched, violations, &pairs);
  CHECK(violations == 0);
  double energy = 0;
  for (float x : yi) energy += double(x) * x;
  CHECK(energy > 1.0);
  CHECK(sameBits(yi, yg));
  CHECK(sameBits(yi, yp));
  // The loop's stage gets a pair function now (its kernel call is off the frame path), without stack probes.
  CHECK(groups.pairedStages == 0);
  CHECK(pairs.pairedStages == 1);
  CHECK_FALSE(groups.stackProbes);
  CHECK_FALSE(pairs.stackProbes);
}

TEST_CASE("wavetable_pos in a global per-sample loop: interpreter == JIT through swaps; the sine path ≈ the rows (≤ −120 dBFS RMS)") {
  const Compiled c = compileText(R"(
patch G {
  param fb = 0 [0..1]
  param pos = 0 [0..1]
  out y : audio
  d = wavetable_pos(frac(phasor(220.5) + z1(d) * fb), pos, 1)
  y = d
}
)");
  constexpr uint32_t kBlock = 64, kBlocks = 160;
  auto render = [&](int backend, const TablePtr& initial, bool swaps) {
    Rig rig(1, false, initial);
    auto img = buildImage(backend, c.sched, rig.res, kBlock, 1);
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    std::vector<float> out(kBlock), all;
    float* o[1] = {out.data()};
    for (uint32_t b = 0; b < kBlocks; ++b) {
      if (swaps && b == 40) rig.swap(tableB());
      if (swaps && b == 80) rig.swap(sineMorph());
      if (swaps && b == 120) rig.swap(sineOne());
      eng::ProcessContext ctx;
      ctx.frames = kBlock;
      ctx.sampleRate = 48000.0;
      ctx.sampleTime = static_cast<int64_t>(b) * kBlock;
      ctx.out = o;
      ctx.numOut = 1;
      (*img)->process(ctx);
      rig.rec.quiescent(rig.reader);
      all.insert(all.end(), out.begin(), out.end());
    }
    return all;
  };
  CHECK(sameBits(render(0, sineOne(), true), render(1, sineOne(), true)));
  // fb = 0: the loop runs, its read is a plain sine of the phasor; inline sine frames vs the rows' read.
  const std::vector<float> fast = render(1, sineOne(), false), rows = render(0, sineRows(), false);
  REQUIRE(fast.size() == rows.size());
  double worst = 0, sq = 0;
  for (std::size_t n = 0; n < fast.size(); ++n) {
    const double d = double(fast[n]) - rows[n];
    worst = std::max(worst, std::fabs(d));
    sq += d * d;
  }
  const double rms = std::sqrt(sq / static_cast<double>(fast.size()));
  MESSAGE("inline sine frames vs the rows' read: peak " << worst << ", RMS " << rms);
  CHECK(rms <= 1e-6);
  CHECK(worst <= 2e-6);  // the rows' own deviation from the exact sine (tests/dsp/test_frame_table_sine.cpp)
}
