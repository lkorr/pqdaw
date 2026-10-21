// T1.11: a 32-op per-voice patch through the VoiceEngine at 1 / 8 / 64 held voices, interpreted (the criterion)
// and JIT-compiled (for comparison); T1.10: offline render speed of the golden patches (renderOffline, both
// backends). Runner benchmarks (pq_bench.h): pq_bench_render [--filter REGEX] [--csv FILE] …
//   patch32/<backend> xN   ns per frame (one 64-frame block per call) and per voice-sample (frame / N)
//   render/<golden> <backend>   ns per output frame of a whole 1 s offline render (compile excluded), and ×realtime
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/denormal_guard.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/voices.h"
#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"
#include "pq/jit/compiler.h"
#include "pq/pqc/pqc.h"
#include "pq_bench.h"

using namespace pq;
namespace g = pq::graph;
namespace e = pq::engine;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

[[noreturn]] void die(const std::string& m) {
  std::fprintf(stderr, "%s\n", m.c_str());
  std::exit(1);
}

// A per-voice chain: an oscillator on voice.hz, then `steps` element-wise ops (the T1.7 chain32 mix: muladd,
// exact tanh/sin, min/max/abs), gated. The text for a given number of steps.
std::string voiceChain(int steps) {
  std::string t = "patch Voice32 {\n  out o : audio voice mono\n  a0 = sine(phasor(voice.hz))\n";
  const char* ops[] = {"$ * 0.99 + 0.01", "tanh($)", "min($, 0.9)", "$ * $ - 0.1", "max($, -0.9)", "abs($) * 0.5", "$ + 0.25", "sin($)"};
  for (int i = 1; i <= steps; ++i) {
    std::string x = ops[(i - 1) % 8];
    const std::string prev = "a" + std::to_string(i - 1);
    for (size_t k; (k = x.find('$')) != std::string::npos;) x.replace(k, 1, prev);
    t += "  a" + std::to_string(i) + " = " + x + "\n";
  }
  t += "  o = a" + std::to_string(steps) + " * gate_to_real(voice.gate)\n}\n";
  return t;
}

g::Schedule scheduleOf(const std::string& text) {
  auto parsed = g::parse(text, "voice32.pqp");
  if (!parsed) die(parsed.error().message);
  g::PatchLibrary lib;
  lib.add(*parsed);
  g::InferOptions io;
  io.library = &lib;
  auto typed = g::infer(parsed->patches[0], io);
  if (!typed) die(typed.error().message);
  g::OptimiseOptions oo;
  oo.sampleRate = kRate;
  auto opt = g::optimise(std::move(typed.value()), oo);
  if (!opt) die(opt.error().message);
  g::ScheduleOptions so;
  so.sampleRate = kRate;
  auto s = g::schedule(opt.value(), so);
  if (!s) die(s.error().message);
  return std::move(s.value());
}

// The schedule with exactly 32 ops (the chain length is searched once).
const g::Schedule& patch32() {
  static const g::Schedule s = [] {
    for (int steps = 16; steps < 64; ++steps) {
      g::Schedule c = scheduleOf(voiceChain(steps));
      if (c.ops.size() == 32) {
        std::printf("PATCH  Voice32: %d chain steps -> %zu ops\n", steps, c.ops.size());
        return c;
      }
      if (c.ops.size() > 32) break;
    }
    die("no chain length gives 32 ops");
  }();
  return s;
}

jit::Compiler& jitCompiler() {
  static std::unique_ptr<jit::Compiler> c = [] {
    auto r = jit::Compiler::create();
    if (!r) die(r.error().message);
    return std::move(r.value());
  }();
  return *c;
}

std::unique_ptr<e::ProgramImage> buildImage(const g::Schedule& s, bool jit, uint32_t groups) {
  if (jit) {
    jit::CompileOptions co;
    co.maxBlockFrames = kBlock;
    co.maxVoiceGroups = groups;
    auto img = jitCompiler().compile(s, co);
    if (!img) die(img.error().message);
    return std::move(img.value());
  }
  e::InterpretOptions eo;
  eo.maxBlockFrames = kBlock;
  eo.maxVoiceGroups = groups;
  auto img = e::buildInterpreted(s, eo);
  if (!img) die(img.error().message);
  return std::move(img.value());
}

void benchPatch32(bench::Context& c, bool jit, uint32_t voices) {
  const g::Schedule& s = patch32();
  auto image = buildImage(s, jit, e::voiceGroupsFor(voices));
  e::VoiceEngine ve;
  e::VoiceConfig cfg;
  cfg.maxVoices = voices;
  if (!ve.prepare(image.get(), cfg)) die("VoiceEngine::prepare failed");
  e::EventBuffer events(256);
  e::ProcessContext ctx;
  ctx.frames = kBlock;
  ctx.sampleRate = kRate;
  ctx.events = &events.list();
  // N held notes (distinct pitches), started in the first block; then blocks without events.
  for (uint32_t v = 0; v < voices; ++v)
    events.push(e::noteOn(0, v + 1, 110.0f * std::exp2(static_cast<float>(v) / 12.0f), 0.8f));
  DenormalGuard dg;
  ve.process(ctx);
  events.clear();
  if (ve.liveVoices() != voices) die("voices did not start");
  const bench::Stats st = c.measure([&] {
    ve.process(ctx);
    bench::doNotOptimize(ve.mix(0)[kBlock - 1]);
  }, kBlock, "ns/sample");
  bench::Stats per = st;
  per.medianNs /= voices;
  per.minNs /= voices;
  per.p99Ns /= voices;
  per.maxNs /= voices;
  c.record(per, "ns/voice-sample", "per voice");
}

void benchRender(bench::Context& c, const char* file, const char* notes, bool jit) {
  const std::filesystem::path path = std::filesystem::path(PQ_SOURCE_DIR) / "tests" / "golden" / "patches" / file;
  pqc::RenderOptions o;
  o.backend = jit ? pqc::Backend::Jit : pqc::Backend::Interpreter;
  o.seconds = 1.0;
  o.notes = notes;
  // Compile once, then time renderOffline only, through the same image and VoiceEngine every round (state and
  // releasing voices carry over from round to round; a fresh image per round would time the build too).
  auto src = pqc::SourceFile::load(path);
  if (!src) die(src.error().message);
  auto compiled = pqc::compilePatch(**src, 0);
  if (!compiled) die(compiled.error().message);
  auto image = buildImage(compiled->schedule, jit, e::voiceGroupsFor(o.maxVoices));
  e::VoiceEngine ve;
  e::NoteListSource list;
  e::RenderRequest rq;
  rq.image = image.get();
  rq.frames = static_cast<uint64_t>(kRate);
  rq.blockFrames = kBlock;
  if (image->io().hasVoiceStages) {
    auto spec = pqc::parseNotes(notes);
    if (!spec) die(spec.error().message);
    list = e::NoteListSource(pqc::timedNotes(*spec, kRate, rq.frames));
    e::VoiceConfig vc;
    vc.maxVoices = o.maxVoices;
    if (!ve.prepare(image.get(), vc)) die("VoiceEngine::prepare failed");
    rq.voices = &ve;
    rq.events = list.source();
  }
  struct NullSink final : e::AudioSink {
    Status begin(uint32_t, uint32_t, uint64_t) override { return Ok{}; }
    Status write(const float* const* ch, uint32_t n) override {
      bench::doNotOptimize(ch[0][n - 1]);
      return Ok{};
    }
    Status end() override { return Ok{}; }
  } sink;
  const bench::Stats st = c.measure([&] {
    if (!e::renderOffline(rq, sink)) die("render failed");
  }, rq.frames, "ns/frame");
  const double secPerFrame = 1.0 / kRate;
  // Audio seconds per wall second (larger = faster): min ← the slowest round, max ← the fastest, and "p99" ← the
  // p99 round time, i.e. the speed that 99 % of rounds reached.
  bench::Stats rt;
  rt.medianNs = secPerFrame / (st.medianNs * 1e-9);
  rt.minNs = secPerFrame / (st.maxNs * 1e-9);
  rt.p99Ns = secPerFrame / (st.p99Ns * 1e-9);
  rt.maxNs = secPerFrame / (st.minNs * 1e-9);
  rt.samples = st.samples;
  c.record(rt, "x realtime", "speed");
}

}  // namespace

int main(int argc, char** argv) {
  bench::Runner r("pq_bench_render");
  for (bool jit : {false, true})
    for (uint32_t v : {1u, 8u, 64u})
      r.add(std::string("patch32/") + (jit ? "jit" : "interp") + " x" + std::to_string(v),
            [jit, v](bench::Context& c) { benchPatch32(c, jit, v); });
  struct G {
    const char* name;
    const char* file;
    const char* notes;
  };
  // tests/golden/test_golden_render.cpp's cases (1 s each here).
  static const G goldens[] = {
      {"fm_voice", "fm_voice.pqp", "60@0:0.4 64@0.1:0.4 67@0.2:0.5*0.9 72@0.45:0.35*0.6 55@0.6:0.3"},
      {"pluck", "pluck.pqp", "48@0:0.5 55@0.15:0.5*0.8 64@0.3:0.4*0.9 67@0.5:0.4"},
      {"drive_zdf", "drive_zdf.pqp", ""},
      {"stereo_echo", "stereo_echo.pqp", ""},
      {"noise_tables", "noise_tables.pqp", ""},
  };
  for (const G& gc : goldens)
    for (bool jit : {false, true})
      r.add(std::string("render/") + gc.name + (jit ? " jit" : " interp"),
            [&gc, jit](bench::Context& c) { benchRender(c, gc.file, gc.notes, jit); });
  return r.main(argc, argv);
}
