// T1.7: the interpreter backend against the reference evaluator, and the ProgramImage contract.
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "interp_harness.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/block_adapter.h"
#include "pq/engine/image_runner.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/null_device.h"

using namespace interp_test;

namespace {

constexpr double kTol = 1e-5;
constexpr uint32_t kFrames = 2048;
constexpr uint32_t kMaxBlock = 1024;
constexpr uint32_t kBlocks[] = {1, 37, 64, kMaxBlock};

uint32_t voicesFor(const Compiled& c) {
  for (const Stage& s : c.sched.stages)
    if (s.poly == Poly::Voice) return 11;  // two groups, the second one partly active
  return 0;
}

std::vector<fs::path> allPatchFiles() {
  std::vector<fs::path> files = text_util::pqpFiles(text_util::casesDir());
  for (const fs::path& p : text_util::pqpFiles(patchesDir())) files.push_back(p);
  return files;
}

// Runs one compiled patch with `voices` voices and a param set against the reference at every block size.
double checkAgainstReference(const Compiled& c, uint32_t voices, int paramSet, std::string& worst) {
  const Setup st(c, voices, paramSet);
  std::string why;
  const Streams ref = runReference(st, kFrames, why);
  REQUIRE_MESSAGE(why.empty(), c.file << ":" << c.name << ": reference evaluator: " << why);
  double maxErr = 0.0;
  for (uint32_t block : kBlocks) {
    const InterpRun run = runInterp(st, kFrames, block, kMaxBlock);
    INFO(c.file << ":" << c.name << " voices " << voices << " params " << paramSet << " block " << block);
    REQUIRE_MESSAGE(run.error.empty(), run.error);
    CHECK(run.rtViolations == 0);
    eng::InterpretOptions io;
    io.maxBlockFrames = kMaxBlock;
    io.maxVoiceGroups = std::max<uint32_t>(1, (voices + 7) / 8);
    auto img = eng::buildInterpreted(c.sched, io);
    REQUIRE(img.ok());
    const Mismatch m = compare(c, ref, run.streams, (*img)->io());
    CHECK_MESSAGE(m.maxErr <= kTol, "max error " << m.maxErr << ": " << m.where);
    if (m.maxErr > maxErr) {
      maxErr = m.maxErr;
      worst = c.file + ":" + c.name + " " + m.where + " (block " + std::to_string(block) + ")";
    }
  }
  return maxErr;
}

}  // namespace

TEST_CASE("interpreter: every patch in tests/graph/cases and tests/engine/patches matches the reference evaluator" * doctest::test_suite("slow")) {
  uint32_t compared = 0, files = 0, voicePatches = 0;
  double maxErr = 0.0;
  std::string worst;
  std::vector<std::string> skipped;
  for (const fs::path& path : allPatchFiles()) {
    ++files;
    for (const Compiled& c : compileFile(path, 48000.0, skipped)) {
      const uint32_t voices = voicesFor(c);
      voicePatches += voices > 0;
      for (int paramSet = 0; paramSet < 2; ++paramSet) {
        std::string w;
        const double e = checkAgainstReference(c, voices, paramSet, w);
        if (e >= maxErr) {
          maxErr = e;
          worst = w;
        }
      }
      ++compared;
    }
  }
  std::string skippedText;
  for (const std::string& s : skipped) skippedText += "\n  skipped: " + s;
  MESSAGE("compared " << compared << " patches from " << files << " files (" << voicePatches
                      << " with voice stages, 11 voices) × 2 param sets × blocks 1/37/64/1024, " << kFrames
                      << " frames each; max error " << maxErr << (worst.empty() ? "" : " at " + worst) << skippedText);
  CHECK(compared >= 20);
  CHECK(voicePatches >= 6);
}

TEST_CASE("interpreter: voice stages with 1, 8 and 16 voices (reducers, broadcasts, per-group buffers, sends)") {
  std::vector<std::string> skipped;
  const auto compiled = compileFile(patchesDir() / "interp.pqp", 48000.0, skipped);
  REQUIRE(skipped.empty());
  for (const Compiled& c : compiled) {
    if (voicesFor(c) == 0) continue;
    for (uint32_t voices : {1u, 8u, 16u}) {
      std::string w;
      CHECK(checkAgainstReference(c, voices, 0, w) <= kTol);
    }
  }
}

TEST_CASE("interpreter: no voices → voice stages do not run and reducers read 0") {
  std::vector<std::string> skipped;
  const Compiled c = compileFile(patchesDir() / "interp.pqp", 48000.0, skipped).at(3);
  REQUIRE(c.name == "VoiceMix");
  const Setup st(c, 0, 0);
  const InterpRun run = runInterp(st, 256, 64);
  REQUIRE(run.error.empty());
  CHECK(run.rtViolations == 0);
  for (const auto& [key, stream] : run.streams)
    for (double x : stream) CHECK(x == 0.0);
}

TEST_CASE("interpreter: params are clamped to their declared range, NaN reads the default, bools are masks") {
  const Compiled c = compileText(R"(patch P {
  param p = 0.25 [0..1]
  param q = 3
  param on : bool = false
  out y : real
  out z : real
  out b : bool
  y = p
  z = q
  b = on
})");
  auto img = eng::buildInterpreted(c.sched);
  REQUIRE(img.ok());
  eng::ProgramImage& im = **img;
  REQUIRE(im.io().params.size() == 3);
  REQUIRE(im.io().globalOut.size() == 3);
  std::vector<float> o0(8), o1(8), o2(8);
  float* outs[3] = {o0.data(), o1.data(), o2.data()};
  eng::ProcessContext ctx;
  ctx.frames = 8;
  ctx.out = outs;
  ctx.numOut = 3;
  auto run = [&](float p, float q, float on) {
    const float params[3] = {p, q, on};
    ctx.params = params;
    ctx.numParams = 3;
    CHECK(pq::alloc_trap::countRtViolations([&] { im.process(ctx); }) == 0);
  };
  run(5.0f, -7.0f, 1.0f);
  CHECK(o0[7] == 1.0f);
  CHECK(o1[7] == -7.0f);  // no range: unclamped
  CHECK(pq::dsp::maskIsTrue(o2[0]));
  run(-2.0f, NAN, 0.0f);
  CHECK(o0[0] == 0.0f);
  CHECK(o1[0] == 3.0f);
  CHECK(!pq::dsp::maskIsTrue(o2[0]));
  run(NAN, 1.0f, 0.0f);
  CHECK(o0[3] == 0.25f);
  ctx.params = nullptr;  // no param block: defaults
  im.process(ctx);
  CHECK(o0[0] == 0.25f);
  CHECK(o1[0] == 3.0f);
}

TEST_CASE("interpreter: a block above maxBlockFrames is rejected with silent outputs; frames = 0 does nothing") {
  const Compiled c = compileText("patch P { out y : audio\n y = sine(phasor(440Hz)) }");
  eng::InterpretOptions o;
  o.maxBlockFrames = 32;
  auto img = eng::buildInterpreted(c.sched, o);
  REQUIRE(img.ok());
  std::vector<float> out(64, 7.0f);
  float* outs[1] = {out.data()};
  eng::ProcessContext ctx;
  ctx.out = outs;
  ctx.numOut = 1;
  ctx.frames = 64;
  (*img)->process(ctx);
  CHECK((*img)->rejectedBlocks() == 1);
  for (float x : out) CHECK(x == 0.0f);
  ctx.frames = 0;
  out.assign(64, 7.0f);
  (*img)->process(ctx);
  CHECK(out[0] == 7.0f);
  ctx.frames = 32;
  (*img)->process(ctx);
  CHECK(out[0] == 0.0f);  // phase 0
  CHECK(out[1] != 0.0f);
  CHECK(out[40] == 7.0f);
}

TEST_CASE("interpreter: missing engine inputs read silence; an image's interface lists its slots") {
  const Compiled c = compileText(R"(patch P {
  in a : audio
  in k : real param
  out y : audio
  export e = a * 2
  y = a + k + track.in[0]
})");
  auto img = eng::buildInterpreted(c.sched);
  REQUIRE(img.ok());
  const eng::ImageInterface& io = (*img)->io();
  REQUIRE(io.globalIn.size() == 2);
  CHECK(io.globalIn[0].rate == Rate::Audio);
  CHECK(io.globalIn[1].rate == Rate::Param);
  REQUIRE(io.globalOut.size() == 2);
  CHECK(io.sampleRate == 48000.0);
  std::vector<float> y(16, 5.0f), e(16, 5.0f);
  float* outs[2] = {y.data(), e.data()};
  bool exportFirst = io.globalOut[0].isExport;
  eng::ProcessContext ctx;
  ctx.frames = 16;
  ctx.out = outs;
  ctx.numOut = 2;
  CHECK(pq::alloc_trap::countRtViolations([&] { (*img)->process(ctx); }) == 0);
  for (int i = 0; i < 16; ++i) {
    CHECK(y[i] == 0.0f);
    CHECK(e[i] == 0.0f);
  }
  std::vector<float> a(16, 0.5f), k(16, 0.25f);
  k[3] = 9.0f;  // param rate: only frame 0 is read
  const float* ins[2] = {a.data(), k.data()};
  ctx.in = ins;
  ctx.numIn = 2;
  (*img)->process(ctx);
  float* yy = exportFirst ? e.data() : y.data();
  float* ee = exportFirst ? y.data() : e.data();
  for (int i = 0; i < 16; ++i) {
    CHECK(yy[i] == 0.75f);
    CHECK(ee[i] == 1.0f);
  }
}

TEST_CASE("interpreter: build errors are reported, not crashed on") {
  Compiled c = compileText("patch P { in x : audio\n out y : audio\n y = x * 2 }");
  eng::InterpretOptions bad;
  bad.maxBlockFrames = 0;
  CHECK_FALSE(eng::buildInterpreted(c.sched, bad).ok());
  Schedule broken = c.sched;
  REQUIRE(!broken.ops.empty());
  broken.ops[0].splMask ^= 1u;  // the Spl mask no longer matches the operand
  CHECK_FALSE(eng::buildInterpreted(broken).ok());
  Schedule broken2 = c.sched;
  broken2.operands[broken2.ops[0].firstOperand].index = 9999;
  CHECK_FALSE(eng::buildInterpreted(broken2).ok());
}

TEST_CASE("interpreter: state arena and layout are exposed for state transfer") {
  const Compiled c = compileText("patch P { out y : audio\n lp = z1(lp * 0.5 + 1)\n y = lp }");
  auto img = eng::buildInterpreted(c.sched);
  REQUIRE(img.ok());
  const eng::StateArena a = (*img)->stateArena();
  REQUIRE(a.global != nullptr);
  REQUIRE((*img)->stateLayout().entries.size() == 1);
  const StateEntry& e = (*img)->stateLayout().entries[0];
  std::vector<float> y(4);
  float* outs[1] = {y.data()};
  eng::ProcessContext ctx;
  ctx.frames = 4;
  ctx.out = outs;
  ctx.numOut = 1;
  (*img)->process(ctx);
  CHECK(y[0] == 0.0f);
  CHECK(y[1] == 1.0f);
  CHECK(y[2] == 1.5f);
  CHECK(y[3] == 1.75f);
  float last = 0;
  std::memcpy(&last, a.global + e.offset, sizeof last);
  CHECK(last == 1.875f);  // z1's stored input
}

TEST_CASE("ImageRunner: a mono patch through BlockAdapter plays on every channel, scaled and limited") {
  const Compiled c = compileText("patch P { param g = 4 [0..8]\n out y : audio\n y = sine(phasor(1kHz)) * g }");
  eng::InterpretOptions o;
  o.maxBlockFrames = 32;
  auto img = eng::buildInterpreted(c.sched, o);
  REQUIRE(img.ok());
  eng::ImageRunner runner;
  REQUIRE(runner.prepare(img->get(), 0, 2, 0.5f, true));
  CHECK(runner.slotForChannel(0) == 0);
  CHECK(runner.slotForChannel(1) == 0);
  eng::BlockAdapter adapter;
  REQUIRE(adapter.prepare(runner.callback(), 0, 2, 32, 100));  // 100-frame device buffers: FIFO mode
  std::vector<float> l(100), r(100);
  float* outs[2] = {l.data(), r.data()};
  float peak = 0;
  for (int b = 0; b < 10; ++b) {
    const eng::AudioBlock blk{nullptr, 0, outs, 2, 100, b * 100};
    CHECK(pq::alloc_trap::countRtViolations([&] {
            pq::DenormalGuard dg;
            adapter.deviceCallback().fn(adapter.deviceCallback().user, blk);
          }) == 0);
    for (int i = 0; i < 100; ++i) {
      CHECK(l[i] == r[i]);
      peak = std::max(peak, std::fabs(l[i]));
    }
  }
  CHECK(peak == 1.0f);  // 4 · 0.5 = 2 → limited to 1
  CHECK(runner.blocks() > 0);
}

TEST_CASE("interpreter: RtScope — process() over every compiled patch allocates nothing") {
  std::vector<std::string> skipped;
  uint32_t patches = 0;
  for (const fs::path& path : allPatchFiles())
    for (const Compiled& c : compileFile(path, 48000.0, skipped)) {
      const Setup st(c, voicesFor(c), 0);
      const InterpRun run = runInterp(st, 300, 100, 128);
      INFO(c.file << ":" << c.name);
      REQUIRE(run.error.empty());
      CHECK(run.rtViolations == 0);
      ++patches;
    }
  CHECK(patches >= 20);
}

TEST_CASE("ImageRunner on NullDevice: device → CallbackRunner → BlockAdapter → interpreted comb filter") {
  const Compiled c = compileText(R"(patch Comb {
  in x : audio
  param fb = 0.7 [0..0.95]
  out y : audio
  c = delay(x + c * fb, 2.5ms, 10ms)
  y = c
})");
  eng::InterpretOptions o;
  o.maxBlockFrames = 32;
  auto img = eng::buildInterpreted(c.sched, o);
  REQUIRE(img.ok());
  eng::NullDevice dev;
  eng::BlockAdapter adapter;
  eng::ImageRunner runner;
  eng::StreamConfig cfg;
  cfg.inputChannels = 1;
  cfg.outputChannels = 2;
  cfg.bufferFrames = 96;
  auto info = dev.open(cfg, adapter.deviceCallback());
  REQUIRE(info.ok());
  REQUIRE(runner.prepare(img->get(), 1, 2));
  REQUIRE(adapter.prepare(runner.callback(), 1, 2, 32, 96));
  CHECK(adapter.directMode());
  std::vector<float> y;
  for (int b = 0; b < 5; ++b) {
    for (uint32_t i = 0; i < 96; ++i) dev.input(0)[i] = (b == 0 && i == 0) ? 1.0f : 0.0f;
    dev.processBlock();  // RtScope + DenormalGuard inside CallbackRunner: an allocation would fail this test
    for (uint32_t i = 0; i < 96; ++i) {
      CHECK(dev.output(0)[i] == dev.output(1)[i]);
      y.push_back(dev.output(0)[i]);
    }
  }
  CHECK(y[119] == 0.0f);
  CHECK(y[120] == 1.0f);
  CHECK(y[121] == 0.0f);
  CHECK(y[240] == doctest::Approx(0.7f));
  CHECK(y[360] == doctest::Approx(0.49f));
  dev.close();
}

