// T1.11: a 32-op per-voice patch through the VoiceEngine at 1 / 8 / 64 held voices, interpreted (the criterion)
// and JIT-compiled (for comparison); T1.10: offline render speed of the golden patches (renderOffline, both
// backends). Runner benchmarks (pq_bench.h): pq_bench_render [--filter REGEX] [--csv FILE] …
//   patch32/<backend> xN   ns per frame (one 64-frame block per call) and per voice-sample (frame / N); [per block]:
//                          the same, each block timed on its own (median / p99 of single blocks, T1.11-fix1)
//   render/<golden> <backend>   ns per output frame of a whole 1 s offline render (compile excluded), and ×realtime
//   operator/<backend> x64  T1.11-fix1, SPEC §13 calibration: the M1 review's proto-Operator (bench/patches/
//                          proto_operator.pqp: 4 serial FM operators, 4 atom ADSRs, a TPT SVF with an LFO) at 64 held
//                          voices through a ProgramHost (as on the device): ns per frame (rounds and single blocks),
//                          "core" = % of one core at 48 kHz (the §13 target: < 10 %), and the blocks of a 20 ms
//                          crossfade to a fresh image of the same program (both images run: the cost of every edit)
//   operator/stock-<config> <backend> xN   T4.12, SPEC §13 calibration of the stock Operator (stdlib/devices/
//                          operator.pqp through bench/patches/operator_calibration.pqp): default (operator A only),
//                          full (four operators, feedback, pitch envelope, per-voice LFO, 24 dB filter with its
//                          envelope, glide), spread (full + spread) and, T4.12-fix1, ms2 / prd (full with the MS2 /
//                          PRD circuit model at +12 dB drive), spread-ms2 (both) at 1 / 8 / 64 held voices through a
//                          ProgramHost; ns per frame, per voice-sample, "core" and, at 64 voices, the crossfade blocks as above
//   operator/stock-full <backend> x64 moving | no-ramps   TX.smooth: every continuous param of the full stock
//                          Operator jumping to a new value every block (every param ramp moving all the time: the
//                          worst case), and the same static patch scheduled without param ramps (what the ramps cost
//                          while nothing moves: the other rows' patch has them)
//   operator/stock-full <backend> x64 ramps A/B   the two (with / without ramps, params at rest) in interleaved rounds:
//                          the median ratio (robust against the load other processes put on the machine), four hosts
//                          of each (placement); ramps A/A: the same method with the ramped schedule on both sides
//                          (the noise floor)
//   operator/stock-four <backend> xN   the stock Operator with four sines, algorithm 1, feedback (TMM.12's comparison
//                          with the Meta Operator, deleted in GU.4)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "pq/core/denormal_guard.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/program_host.h"
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
  auto block = [&] {
    ve.process(ctx);
    bench::doNotOptimize(ve.mix(0)[kBlock - 1]);
  };
  const bench::Stats st = c.measure(block, kBlock, "ns/sample");
  bench::Stats per = st;
  per.medianNs /= voices;
  per.minNs /= voices;
  per.p99Ns /= voices;
  per.maxNs /= voices;
  c.record(per, "ns/voice-sample", "per voice");
  c.measureBlocks(block, kBlock, "ns/sample");  // the tail of single blocks (Q75 B)
}

// ---- operator/ (T1.11-fix1) -------------------------------------------------------------------------------------

constexpr uint32_t kOperatorVoices = 64;

const g::Schedule& operatorSchedule() {
  static const std::unique_ptr<pqc::SourceFile> src = [] {
    auto s = pqc::SourceFile::load(std::filesystem::path(PQ_SOURCE_DIR) / "bench" / "patches" / "proto_operator.pqp");
    if (!s) die(s.error().message);
    return std::move(s.value());
  }();
  static const g::Schedule s = [] {
    auto idx = src->findPatch("ProtoOperator");
    if (!idx) die(idx.error().message);
    auto compiled = pqc::compilePatch(*src, *idx);
    if (!compiled) die(compiled.error().message);
    std::printf("PATCH  ProtoOperator: %zu ops\n", compiled->schedule.ops.size());
    return std::move(compiled->schedule);
  }();
  return s;
}

// T4.12: the stock Operator's calibration patches (bench/patches/operator_calibration.pqp).
const g::Schedule& stockSchedule(const char* patch) {
  static const std::unique_ptr<pqc::SourceFile> src = [] {
    auto s = pqc::SourceFile::load(std::filesystem::path(PQ_SOURCE_DIR) / "bench" / "patches" / "operator_calibration.pqp");
    if (!s) die(s.error().message);
    return std::move(s.value());
  }();
  static std::map<std::string, g::Schedule> cache;
  auto it = cache.find(patch);
  if (it == cache.end()) {
    auto idx = src->findPatch(patch);
    if (!idx) die(idx.error().message);
    auto compiled = pqc::compilePatch(*src, *idx);
    if (!compiled) die(compiled.error().message);
    std::printf("PATCH  %s: %zu ops\n", patch, compiled->schedule.ops.size());
    it = cache.emplace(patch, std::move(compiled->schedule)).first;
  }
  return it->second;
}

// N notes on at frame 0 (distinct pitches, all held for the whole run); N in the source's user pointer.
void operatorNotes(void* user, int64_t t, uint32_t, e::EventBuffer& out) noexcept {
  if (t != 0) return;
  const uint32_t n = user ? *static_cast<const uint32_t*>(user) : kOperatorVoices;
  for (uint32_t v = 0; v < n; ++v)
    out.push(e::noteOn(0, v + 1, 55.0f * std::exp2(static_cast<float>(v % 48) / 12.0f + 0.0001f * static_cast<float>(v)),
                       0.8f));
}

// TX.smooth: every continuous param (real, with a range) moves to a new value every block, inside its range.
struct ParamMotion {
  const g::Schedule* s = nullptr;
  uint64_t blocks = 0;
};
void moveParams(void* user, int64_t, uint32_t, e::TransportSnapshot&, std::span<float> params) noexcept {
  auto& m = *static_cast<ParamMotion*>(user);
  ++m.blocks;
  for (size_t i = 0; i < params.size() && i < m.s->params.size(); ++i) {
    const g::ParamSlot& p = m.s->params[i];
    if (p.kind != g::Kind::Real || !p.hasRange || !(p.rangeMax > p.rangeMin)) continue;
    const double u = 0.5 + 0.25 * std::sin(0.37 * static_cast<double>(m.blocks) + static_cast<double>(i));
    params[i] = static_cast<float>(p.rangeMin + u * (p.rangeMax - p.rangeMin));
  }
}

// `voices` held voices of schedule `s` through a ProgramHost; crossfade rows when `fades`; every param moving every
// block when `moving`.
void benchVoicePatch(bench::Context& c, const g::Schedule& s, bool jit, uint32_t voices, bool fades, bool moving = false) {
  static uint32_t noteCount = 0;
  noteCount = voices;
  const uint32_t groups = e::voiceGroupsFor(std::max(voices, kOperatorVoices));
  e::HostConfig hc;
  hc.sampleRate = kRate;
  hc.maxBlockFrames = kBlock;
  hc.numOut = 2;
  hc.maxPending = 64;
  hc.voices = true;
  hc.voice.maxVoices = kOperatorVoices;
  hc.voice.compact = false;
  hc.events = {&operatorNotes, &noteCount};
  static ParamMotion motion;
  motion = ParamMotion{&s, 0};
  if (moving) hc.hook = e::BlockHook{&moveParams, &motion, nullptr};
  e::ProgramHost host(hc);
  if (!host.publish(buildImage(s, jit, groups)).ok()) die("operator: publish failed");
  std::vector<float> o0(kBlock), o1(kBlock);
  float* outs[2] = {o0.data(), o1.data()};
  int64_t time = 0;
  DenormalGuard dg;
  auto block = [&] {
    e::AudioBlock ab;
    ab.out = outs;
    ab.numOut = 2;
    ab.frames = kBlock;
    ab.frameTime = time;
    host.process(ab);
    time += kBlock;
    bench::doNotOptimize(o0[kBlock - 1]);
  };
  for (int i = 0; i < 16; ++i) block();  // notes on, attacks under way
  if (host.voices()->liveVoices() != voices) die("operator: voices did not start");
  const bench::Stats st = c.measure(block, kBlock, "ns/sample");
  bench::Stats per = st;
  per.medianNs /= voices;
  per.minNs /= voices;
  per.p99Ns /= voices;
  per.maxNs /= voices;
  c.record(per, "ns/voice-sample", "per voice");
  const bench::Stats blocks = c.measureBlocks(block, kBlock, "ns/sample");
  // SPEC §13: share of one core at 48 kHz (1 / 48000 s of CPU per sample = 100 %).
  const double nsPerSample = 1e9 / kRate;
  bench::Stats core = st;
  core.medianNs = 100.0 * st.medianNs / nsPerSample;
  core.minNs = 100.0 * st.minNs / nsPerSample;
  core.p99Ns = 100.0 * st.p99Ns / nsPerSample;
  core.maxNs = 100.0 * st.maxNs / nsPerSample;
  c.record(core, "% of one core at 48 kHz", "core");
  if (!fades) return;
  // Crossfades: a fresh image of the same program, 20 ms (15 blocks of 64), every block of the fade timed (from the
  // swap block on). Images are built before timing (a JIT compile takes ~0.1 s).
  constexpr int kRounds = 12;
  std::vector<std::unique_ptr<e::ProgramImage>> images;
  for (int i = 0; i < kRounds; ++i) images.push_back(buildImage(s, jit, groups));
  std::vector<double> fade;
  const double nsPerTick = 1.0 / bench::tscTicksPerNs();
  for (int r = 0; r < kRounds; ++r) {
    if (!host.publish(std::move(images[static_cast<size_t>(r)]), e::SwapPolicy{e::SwapMode::Crossfade, 20.0f}).ok())
      die("operator: crossfade publish failed");
    do {
      const uint64_t t0 = bench::rdtsc();
      block();
      fade.push_back(static_cast<double>(bench::rdtsc() - t0) * nsPerTick / kBlock);
    } while (host.crossfading());
    host.collect();
    for (int i = 0; i < 8; ++i) block();
  }
  if (host.stats().crossfades != static_cast<uint64_t>(kRounds)) die("operator: not every swap crossfaded");
  const bench::Stats f = bench::summarize(fade);
  c.record(f, "ns/sample", "crossfade [per block]");
  bench::Stats ratio;  // fade blocks over the median single steady block
  ratio.medianNs = f.medianNs / blocks.medianNs;
  ratio.minNs = f.minNs / blocks.medianNs;
  ratio.p99Ns = f.p99Ns / blocks.medianNs;
  ratio.maxNs = f.maxNs / blocks.medianNs;
  ratio.samples = f.samples;
  c.record(ratio, "x steady block", "crossfade ratio");
}

// TX.smooth: two schedules of one patch (a: with param ramps, b: without) at 64 held voices. TX.smooth-fix1: four
// hosts of each, built in alternating order (a b b a a b b a), so that where the allocator happens to put one image's
// buffers and state (measured: up to ±3 % between two hosts of the same schedule) averages out instead of passing
// for a difference; then interleaved rounds in which every host runs 50 blocks (the order rotates per round). A
// round's ratio is the a hosts' summed time over the b hosts'; the median over the rounds is the row. `selfCheck`:
// the b hosts run schedule a too (A/A: the noise floor of the method).
void benchRampsAB(bench::Context& c, const g::Schedule& sa, const g::Schedule& sb, bool jit, bool selfCheck = false) {
  static uint32_t noteCount = 0;
  noteCount = kOperatorVoices;
  const uint32_t groups = e::voiceGroupsFor(kOperatorVoices);
  auto makeHost = [&](const g::Schedule& s) {
    e::HostConfig hc;
    hc.sampleRate = kRate;
    hc.maxBlockFrames = kBlock;
    hc.numOut = 2;
    hc.maxPending = 64;
    hc.voices = true;
    hc.voice.maxVoices = kOperatorVoices;
    hc.voice.compact = false;
    hc.events = {&operatorNotes, &noteCount};
    auto host = std::make_unique<e::ProgramHost>(hc);
    if (!host->publish(buildImage(s, jit, groups)).ok()) die("ramps A/B: publish failed");
    return host;
  };
  constexpr int kHosts = 4, kRounds = 120, kBlocks = 50;
  struct H {
    std::unique_ptr<e::ProgramHost> host;
    bool a = false;
    int64_t time = 0;
  };
  const g::Schedule& b = selfCheck ? sa : sb;
  std::vector<H> hs;
  for (int i = 0; i < kHosts; ++i) {
    const bool aFirst = i % 2 == 0;
    hs.push_back({makeHost(aFirst ? sa : b), aFirst, 0});
    hs.push_back({makeHost(aFirst ? b : sa), !aFirst, 0});
  }
  std::vector<float> o0(kBlock), o1(kBlock);
  float* outs[2] = {o0.data(), o1.data()};
  DenormalGuard dg;
  auto blocks = [&](H& h, int n) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
      e::AudioBlock ab;
      ab.out = outs;
      ab.numOut = 2;
      ab.frames = kBlock;
      ab.frameTime = h.time;
      h.host->process(ab);
      h.time += kBlock;
      bench::doNotOptimize(o0[kBlock - 1]);
    }
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (n * kBlock);
  };
  for (H& h : hs) blocks(h, 16);
  std::vector<double> ra, rb, ratio;
  const auto n = hs.size();
  for (int round = 0; round < kRounds; ++round) {
    double ta = 0.0, tb = 0.0;
    for (size_t j = 0; j < n; ++j) {
      H& h = hs[(j + static_cast<size_t>(round)) % n];
      (h.a ? ta : tb) += blocks(h, kBlocks);
    }
    ra.push_back(ta / kHosts);
    rb.push_back(tb / kHosts);
    ratio.push_back(ta / tb);
  }
  const char* an = selfCheck ? "ramps (set 1)" : "ramps";
  const char* bn = selfCheck ? "ramps (set 2)" : "no ramps";
  c.record(bench::summarize(ra), "ns/sample", an);
  c.record(bench::summarize(rb), "ns/sample", bn);
  const bench::Stats r = bench::summarize(ratio);
  c.record(r, "ratio", selfCheck ? "A/A" : "ramps/no ramps");
  std::vector<double> q = ratio;
  std::sort(q.begin(), q.end());
  std::printf("  %s / %s = %.4f (median of %d rounds over %d + %d hosts; quartiles %.4f … %.4f)\n", an, bn, r.medianNs,
              kRounds, kHosts, kHosts, q[q.size() / 4], q[q.size() - 1 - q.size() / 4]);
}

const g::Schedule& plainFullSchedule() {
  static const g::Schedule plain = [] {
    auto src = pqc::SourceFile::load(std::filesystem::path(PQ_SOURCE_DIR) / "bench" / "patches" / "operator_calibration.pqp");
    if (!src) die(src.error().message);
    auto idx = (*src)->findPatch("OpFull");
    if (!idx) die(idx.error().message);
    auto compiled = pqc::compilePatch(**src, *idx);
    if (!compiled) die(compiled.error().message);
    g::ScheduleOptions so;
    so.sampleRate = kRate;
    so.paramRamps = false;
    auto sc = g::schedule(compiled->optimised, so);
    if (!sc) die(sc.error().message);
    return std::move(sc.value());
  }();
  return plain;
}

void benchOperator(bench::Context& c, bool jit) { benchVoicePatch(c, operatorSchedule(), jit, kOperatorVoices, true); }

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
  // Audio seconds per wall second (larger = faster, a higher-is-better unit: pq_bench.h): min ← the slowest round,
  // max ← the fastest, and "p99" ← the p99 round time, i.e. the low tail: the speed that 99 % of rounds reached.
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
  for (bool jit : {false, true})
    r.add(std::string("operator/") + (jit ? "jit" : "interp") + " x64",
          [jit](bench::Context& c) { benchOperator(c, jit); });
  struct Stock {
    const char* name;
    const char* patch;
  };
  static const Stock stock[] = {{"default", "OpDefault"}, {"full", "OpFull"},  {"spread", "OpSpread"},
                                {"ms2", "OpMs2"},         {"prd", "OpPrd"},
                                {"spread-ms2", "OpSpreadMs2"}};
  for (const Stock& sc : stock)
    for (bool jit : {false, true})
      for (uint32_t v : {1u, 8u, 64u})
        r.add(std::string("operator/stock-") + sc.name + " " + (jit ? "jit" : "interp") + " x" + std::to_string(v),
              [&sc, jit, v](bench::Context& c) { benchVoicePatch(c, stockSchedule(sc.patch), jit, v, v == 64); });
  // TX.smooth: the worst case (every param moving every block) and the static cost of the ramps (the same patch without).
  for (bool jit : {false, true}) {
    r.add(std::string("operator/stock-full ") + (jit ? "jit" : "interp") + " x64 moving",
          [jit](bench::Context& c) { benchVoicePatch(c, stockSchedule("OpFull"), jit, 64, false, true); });
    r.add(std::string("operator/stock-full ") + (jit ? "jit" : "interp") + " x64 no-ramps",
          [jit](bench::Context& c) { benchVoicePatch(c, plainFullSchedule(), jit, 64, false); });
    r.add(std::string("operator/stock-full ") + (jit ? "jit" : "interp") + " x64 ramps A/B",
          [jit](bench::Context& c) { benchRampsAB(c, stockSchedule("OpFull"), plainFullSchedule(), jit); });
    r.add(std::string("operator/stock-full ") + (jit ? "jit" : "interp") + " x64 ramps A/A",
          [jit](bench::Context& c) { benchRampsAB(c, stockSchedule("OpFull"), stockSchedule("OpFull"), jit, true); });
  }
  // TMM.12: the stock Operator with four sines (the Meta Operator's comparison; GU.4 deleted the Meta Operator).
  for (bool jit : {false, true})
    for (uint32_t v : {8u, 64u})
      r.add(std::string("operator/stock-four ") + (jit ? "jit" : "interp") + " x" + std::to_string(v),
            [jit, v](bench::Context& c) { benchVoicePatch(c, stockSchedule("OpFour"), jit, v, false); });
  // T1b.x-perf (Q111 C): the cold JIT compile of each stock configuration (64-voice geometry, no cache: every image
  // is dropped before the next compile, so nothing is shared in memory) with voice groups only (the quick first image)
  // and with the default voice code (the pair functions as well: the final image).
  for (const Stock& sc : stock)
    for (jit::VoiceCode vc : {jit::VoiceCode::Groups, jit::VoiceCode::Auto})
      r.add(std::string("operator/stock-") + sc.name + " compile " + (vc == jit::VoiceCode::Groups ? "groups" : "auto"),
            [&sc, vc](bench::Context& c) {
              const g::Schedule& s = stockSchedule(sc.patch);
              std::vector<double> ms;
              uint32_t paired = 0;
              for (int rep = 0; rep < 7; ++rep) {
                jit::CompileOptions co;
                co.maxBlockFrames = kBlock;
                co.maxVoiceGroups = e::voiceGroupsFor(64);
                co.voiceCode = vc;
                jit::CompileStats st;
                auto img = jitCompiler().compile(s, co, &st);
                if (!img) die(img.error().message);
                ms.push_back(st.totalMs);
                paired = st.pairedStages;
              }
              std::printf("  %u paired stages\n", paired);
              c.record(bench::summarize(ms), "ms/compile");
            });
  return r.main(argc, argv);
}
