// T4.5: live probes and cost meters on images (pq/engine/inspect.h). Compiled into pq_tests_engine (interpreter) and
// pq_tests_jit (interp_test::rigImageBuilder() = the JIT): every case runs on both backends, and the JIT build also
// compares the two backends' probe streams bit for bit.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "probe_harness.h"
#include "silence_harness.h"

using namespace probe_test;
using interp_test::Setup;

namespace {

constexpr uint32_t kFrames = 3000;

uint32_t probeId(const interp_test::Compiled& c, uint32_t index) {
  REQUIRE(index < c.sched.probes.size());
  return c.sched.probes[index].id;
}

std::vector<double> stream(const interp_test::InterpRun& r, int kind, uint32_t slot, uint32_t voice = 0) {
  const auto it = r.streams.find({kind, slot, 0, voice});
  return it == r.streams.end() ? std::vector<double>{} : it->second;
}

const char* kGlobal = R"(
patch G {
  param k = 0.5 [0..1]
  out y : audio
  out b : bool
  s = sine(phasor(440Hz))
  m = s * k
  y = m * 2
  c = s > 0
  b = c
}
)";

}  // namespace

TEST_CASE("probes: a probed global value is the value the program computes, and the audio does not change (T4.5)") {
  const interp_test::Compiled probed = compileText(kGlobal, {"s", "m", "k", "c"});
  const interp_test::Compiled plain = interp_test::compileText(kGlobal);
  REQUIRE(probed.sched.probes.size() == 4);
  // Nothing but buffers differs: same ops, regions and state.
  CHECK(probed.sched.ops.size() == plain.sched.ops.size());
  CHECK(probed.sched.regions.size() == plain.sched.regions.size());
  CHECK(probed.sched.state.entries.size() == plain.sched.state.entries.size());
  Taps taps(probed.sched);
  const Setup sp(probed, 0, 0), sq(plain, 0, 0);
  for (const uint32_t block : {64u, 37u}) {
    INFO("block " << block);
    Taps t(probed.sched);
    const auto a = interp_test::runInterp(sp, kFrames, block, 1024, binding(interp_test::rigImageBuilder(), &t));
    const auto b = interp_test::runInterp(sq, kFrames, block, 1024, interp_test::rigImageBuilder());
    REQUIRE(a.error.empty());
    REQUIRE(b.error.empty());
    CHECK(a.rtViolations == 0);
    std::string where;
    CHECK_MESSAGE(sameStreams(a.streams, b.streams, where), where);
    const std::vector<double> y = stream(a, 0, 0);
    const std::vector<float> s = t.all(probeId(probed, 0)), m = t.all(probeId(probed, 1)), k = t.all(probeId(probed, 2));
    const std::vector<float> bits = t.all(probeId(probed, 3));
    REQUIRE(s.size() == kFrames);
    REQUIRE(m.size() == kFrames);
    REQUIRE(k.size() == kFrames);
    REQUIRE(bits.size() == kFrames);
    bool ok = true;
    for (uint32_t f = 0; f < kFrames && ok; ++f) {
      ok = static_cast<double>(m[f]) * 2.0 == y[f] && s[f] == m[f] * 2.0f && k[f] == 0.5f &&
           std::signbit(bits[f]) == (s[f] > 0.0f);
      if (!ok) MESSAGE("frame " << f << ": y " << y[f] << " s " << s[f] << " m " << m[f] << " k " << k[f]);
    }
    CHECK(ok);
    // The level of the last block.
    const pq::ProbeLevel* lv = t.level(probeId(probed, 0));
    REQUIRE(lv != nullptr);
    CHECK(lv->blocks.load() == (kFrames + block - 1) / block);
    CHECK(lv->last.load() == s.back());
    const pq::ProbeLevel* lb = t.level(probeId(probed, 3));
    const uint32_t lastN = kFrames % block ? kFrames % block : block;  // the last block's frames
    bool anyHigh = false, allHigh = true;
    for (uint32_t f = kFrames - lastN; f < kFrames; ++f) {
      anyHigh = anyHigh || s[f] > 0.0f;
      allHigh = allHigh && s[f] > 0.0f;
    }
    CHECK(lb->hi.load() == (anyHigh ? 1.0f : 0.0f));
    CHECK(lb->lo.load() == (allHigh ? 1.0f : 0.0f));
    CHECK(lb->last.load() == (s.back() > 0.0f ? 1.0f : 0.0f));
  }
}

TEST_CASE("probes: compiled but not read (or not bound) cost a load and change nothing (T4.5)") {
  const interp_test::Compiled probed = compileText(kGlobal, {"s", "m"});
  const interp_test::Compiled plain = interp_test::compileText(kGlobal);
  const Setup sp(probed, 0, 0), sq(plain, 0, 0);
  // Bound, but the rings have no reader and no level is bound: nothing is written.
  std::vector<std::unique_ptr<pq::TapRing>> rings;
  std::vector<eng::ProbeTap> taps;
  for (const ProbeRef& p : probed.sched.probes) {
    rings.push_back(std::make_unique<pq::TapRing>(1, 8192, 1024));
    taps.push_back({p.id, rings.back().get(), nullptr});
  }
  const interp_test::ImageBuilder inner = interp_test::rigImageBuilder();
  const interp_test::ImageBuilder bindUnread = [&](const Schedule& s, const eng::InterpretOptions& io) {
    auto img = inner ? inner(s, io) : eng::buildInterpreted(s, io);
    if (img.ok()) {
      eng::InspectBinding b;
      b.probes = taps;
      (*img)->bindInspect(b);
    }
    return img;
  };
  const auto a = interp_test::runInterp(sp, kFrames, 64, 1024, bindUnread);
  const auto b = interp_test::runInterp(sp, kFrames, 64, 1024, inner);  // not bound at all
  const auto c = interp_test::runInterp(sq, kFrames, 64, 1024, inner);
  REQUIRE(a.error.empty());
  std::string where;
  CHECK_MESSAGE(sameStreams(a.streams, c.streams, where), where);
  CHECK_MESSAGE(sameStreams(b.streams, c.streams, where), where);
  for (const auto& r : rings) CHECK(r->written() == 0);
  CHECK(a.rtViolations == 0);
}

TEST_CASE("probes: a voice value reads the newest voice, or the sum over the sounding voices (T4.5)") {
  const char* text = R"(
patch V {
  out y : audio voice
  g = voice.hz + 1
  h = voice.hz
  on = voice.gate
  y = sine(phasor(g)) * gate_to_real(on)
}
)";
  for (const bool sum : {false, true}) {
    INFO((sum ? "sum" : "newest voice"));
    const interp_test::Compiled c = compileText(text, {"g", "h", "on"}, sum);
    REQUIRE(c.sched.probes.size() == 3);
    CHECK(c.sched.probes[0].poly == Poly::Voice);
    CHECK(c.sched.probes[1].value.kind == Operand::Kind::External);  // voice.hz itself
    // 11 voices: two groups (the JIT pairs them), the newest voice is voice 2 (Setup::lastVoice).
    const Setup st(c, 11, 0);
    Taps t(c.sched);
    const auto r = interp_test::runInterp(st, kFrames, 64, 1024, binding(interp_test::rigImageBuilder(), &t));
    REQUIRE(r.error.empty());
    CHECK(r.rtViolations == 0);
    const std::vector<float> g = t.all(probeId(c, 0)), h = t.all(probeId(c, 1)), on = t.all(probeId(c, 2));
    REQUIRE(g.size() == kFrames);
    REQUIRE(h.size() == kFrames);
    REQUIRE(on.size() == kFrames);
    double worst = 0.0;
    bool gates = true;
    for (uint32_t f = 0; f < kFrames; ++f) {
      double eg = 0.0, eh = 0.0;
      bool anyGate = false;
      for (uint32_t v = 0; v < 11; ++v) {
        if (!sum && v != st.lastVoice()) continue;
        const float hz = static_cast<float>(interp_test::voiceSignal(0, v, f));
        eh += hz;
        eg += static_cast<double>(hz + 1.0f);
        anyGate = anyGate || interp_test::voiceSignal(static_cast<uint32_t>(eng::VoiceSignal::Gate), v, f) != 0.0;
      }
      if (!sum) {
        CHECK(g[f] == static_cast<float>(eg));  // exact: the lane itself
        CHECK(h[f] == static_cast<float>(eh));
      } else {
        worst = std::max({worst, std::fabs(g[f] - eg) / eg, std::fabs(h[f] - eh) / eh});
      }
      gates = gates && std::signbit(on[f]) == anyGate;
    }
    CHECK(worst < 1e-6);
    CHECK(gates);
  }
}

TEST_CASE("probes: a value in a sleeping region reads 0, which is what the region outputs (T4.5)") {
  const char* text = R"(
patch D {
  in x : audio
  out y : audio
  e = delay(x, 3ms, 10ms) * 0.5
  y = e
}
)";
  const interp_test::Compiled c = compileText(text, {"e"});
  REQUIRE(!c.sched.sleep.segments.empty());
  Taps t(c.sched, 1u << 17);
  const silence_test::Run r = silence_test::run(c, eng::SleepMode::On, silence_test::burstInput(20000 + 17), 40000, {64},
                                                binding(interp_test::rigImageBuilder(), &t));
  REQUIRE(r.error.empty());
  CHECK(r.rtViolations == 0);
  CHECK(r.stats.sleeps > 0);
  const std::vector<float> e = t.all(probeId(c, 0));
  REQUIRE(e.size() == r.out[0].size());
  CHECK(sameFloats(e, r.out[0]));
}

TEST_CASE("cost meter: regions are timed only while the meter is enabled, and add up to the block (T4.5)") {
  // A stdlib device instead of a hand-made filter: its regions are the real mix of Control / Block / Loop.
  std::vector<std::string> skipped;
  const fs::path sat = fs::path(PQ_TEST_DATA_DIR).parent_path() / "stdlib" / "devices" / "saturator.pqp";
  std::vector<Pair> pairs = compileFilePairs(sat, 48000.0, skipped);
  REQUIRE(!pairs.empty());
  const interp_test::Compiled& comp = pairs.front().plain;
  REQUIRE(comp.sched.regions.size() > 1);
  eng::CostMeter meter;
  const Setup st(comp, 0, 0);
  eng::CostMeter::Reading rd;
  // Disabled: images bound to it measure nothing.
  {
    const auto r = interp_test::runInterp(st, 2048, 64, 1024, binding(interp_test::rigImageBuilder(), nullptr, &meter));
    REQUIRE(r.error.empty());
    CHECK(r.rtViolations == 0);
    CHECK_FALSE(meter.read(rd));  // baseline
    CHECK_FALSE(meter.read(rd));  // no frame since
  }
  meter.setEnabled(true);
  {
    // A fresh image (a new key): the first read takes the baseline.
    const auto r = interp_test::runInterp(st, 4096, 64, 1024, binding(interp_test::rigImageBuilder(), nullptr, &meter));
    REQUIRE(r.error.empty());
    CHECK(r.rtViolations == 0);  // RtScope-clean with the meter on
  }
  CHECK_FALSE(meter.read(rd));  // baseline of this image's key (taken after its blocks: nothing new yet)
  {
    // Same meter, another image: a new key again; run, read the baseline, run, read.
    auto inner = interp_test::rigImageBuilder();
    eng::InterpretOptions io;
    io.maxBlockFrames = 1024;
    io.maxVoiceGroups = 1;
    io.resources = &interp_test::tables().resources;
    auto img = inner ? inner(comp.sched, io) : eng::buildInterpreted(comp.sched, io);
    REQUIRE(img.ok());
    eng::InspectBinding b;
    b.meter = &meter;
    REQUIRE((*img)->bindInspect(b));
    std::vector<std::vector<float>> in((*img)->io().globalIn.size(), std::vector<float>(64, 0.25f));
    std::vector<std::vector<float>> out((*img)->io().globalOut.size(), std::vector<float>(64));
    std::vector<const float*> inP;
    std::vector<float*> outP;
    for (auto& v : in) inP.push_back(v.data());
    for (auto& v : out) outP.push_back(v.data());
    auto block = [&] {
      eng::ProcessContext ctx;
      ctx.frames = 64;
      ctx.sampleRate = 48000.0;
      ctx.in = inP.data();
      ctx.numIn = static_cast<uint32_t>(inP.size());
      ctx.out = outP.data();
      ctx.numOut = static_cast<uint32_t>(outP.size());
      pq::DenormalGuard dg;
      CHECK(pq::alloc_trap::countRtViolations([&] { (*img)->process(ctx); }) == 0);
    };
    block();
    CHECK_FALSE(meter.read(rd));  // baseline
    for (int i = 0; i < 100; ++i) block();
    REQUIRE(meter.read(rd));
    CHECK(rd.frames == 6400);
    CHECK(rd.totalNsPerFrame > 0.0);
    REQUIRE(rd.regionNsPerFrame.size() == comp.sched.regions.size());
    REQUIRE(rd.units.size() == comp.sched.regions.size());
    double sum = 0.0;
    for (double v : rd.regionNsPerFrame) sum += v;
    CHECK(sum > 0.0);
    CHECK(sum <= rd.totalNsPerFrame * 1.0001);  // the regions are inside the block
    for (uint32_t r = 0; r < rd.units.size(); ++r) {
      CHECK(rd.units[r] <= r);
      if (rd.units[r] != r) CHECK(rd.regionNsPerFrame[r] == 0.0);  // counted in its unit's first region
    }
    meter.setEnabled(false);
    for (int i = 0; i < 10; ++i) block();
    CHECK_FALSE(meter.read(rd));  // nothing measured while disabled
  }
}

TEST_CASE("probes: the interpreter's and the JIT's probe streams are bit-identical, voice pairs included (T4.5)") {
  if (!interp_test::rigImageBuilder()) return;  // pq_tests_jit only
  const char* text = R"(
patch W {
  out y : audio voice
  out g : audio
  o = sine(phasor(voice.hz * 1.5))
  env = z1(gate_to_real(voice.gate)) * 0.4
  v = o * env
  y = v
  sum = voice_sum(v) + sine(phasor(3Hz)) * 0.01
  g = sum
}
)";
  for (const bool sum : {false, true}) {
    const interp_test::Compiled c = compileText(text, {"o", "env", "v", "sum"}, sum);
    REQUIRE(c.sched.probes.size() == 4);
    const Setup st(c, 19, 0);  // a pair and a lone group
    Taps tj(c.sched), ti(c.sched);
    const auto a = interp_test::runInterp(st, kFrames, 64, 1024, binding(interp_test::rigImageBuilder(), &tj));
    const auto b = interp_test::runInterp(st, kFrames, 64, 1024, binding({}, &ti));
    REQUIRE(a.error.empty());
    REQUIRE(b.error.empty());
    for (const ProbeRef& p : c.sched.probes) {
      INFO("probe " << p.id << (sum ? " (sum)" : ""));
      const std::vector<float> x = tj.all(p.id), y = ti.all(p.id);
      CHECK(x.size() == kFrames);
      CHECK(sameFloats(x, y));
    }
  }
}

TEST_CASE("probes: every node output of the corpus probed, audio bit-identical to the unprobed program (T4.5)" *
          doctest::test_suite("slow")) {
  std::vector<fs::path> files = text_util::pqpFiles(text_util::casesDir());
  for (const fs::path& p : text_util::pqpFiles(interp_test::patchesDir())) files.push_back(p);
  const fs::path stdlib = fs::path(PQ_TEST_DATA_DIR).parent_path() / "stdlib";
  for (const auto& e : fs::recursive_directory_iterator(stdlib))
    if (e.path().extension() == ".pqp") files.push_back(e.path());
  std::vector<std::string> skipped;
  uint32_t compared = 0, probes = 0, jitCompared = 0;
  const bool jit = static_cast<bool>(interp_test::rigImageBuilder());
  for (const fs::path& path : files)
    for (const Pair& pr : compileFilePairs(path, 48000.0, skipped)) {
      INFO(pr.file << ":" << pr.name);
      const uint32_t voices = pr.plain.sched.stages.size() > 1 ? 11u : 0u;
      const Setup sp(pr.probed, voices, 1), sq(pr.plain, voices, 1);
      Taps t(pr.probed.sched);
      const auto a = interp_test::runInterp(sp, 1500, 37, 1024, binding(interp_test::rigImageBuilder(), &t));
      const auto b = interp_test::runInterp(sq, 1500, 37, 1024, interp_test::rigImageBuilder());
      if (!a.error.empty() || !b.error.empty()) {
        skipped.push_back(pr.file + ":" + pr.name + ": " + a.error + b.error);
        continue;
      }
      CHECK(a.rtViolations == 0);
      std::string where;
      CHECK_MESSAGE(sameStreams(a.streams, b.streams, where), where);
      for (const ProbeRef& p : pr.probed.sched.probes) CHECK(t.all(p.id).size() == 1500);
      ++compared;
      probes += static_cast<uint32_t>(pr.probed.sched.probes.size());
      if (jit) {  // and the interpreter's probe streams, bit for bit
        Taps ti(pr.probed.sched);
        const auto c = interp_test::runInterp(sp, 1500, 37, 1024, binding({}, &ti));
        REQUIRE(c.error.empty());
        for (const ProbeRef& p : pr.probed.sched.probes) {
          INFO("probe " << pr.probeNames[p.id]);
          CHECK(sameFloats(t.all(p.id), ti.all(p.id)));
        }
        ++jitCompared;
      }
    }
  MESSAGE("probed corpus: " << compared << " patches, " << probes << " probes, bit-identical audio"
                            << std::string(jit ? "; interpreter == JIT probe streams" : ""));
  CHECK(compared >= 80);
  if (jit) CHECK(jitCompared == compared);
}
