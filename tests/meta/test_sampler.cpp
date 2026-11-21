// SM.2: the Sampler engine (pq/meta/sampler.h) — its expansion (registered, every setting combination in both
// contexts, the text form parses back, the reading atom keeps its id), and renders on both backends against a C++
// reference of the documented semantics (pq/dsp/atoms.h sample_read, pq/meta/sampler.h):
//   classic    a forward loop with its crossfade, a ping-pong loop; the gate retriggers
//   one-shot   start → end with fades in / out; Re-Pitch follows the tempo; done at the end
//   slice      the key's slice of the data's slice table (key − 36), keys out of range silent
//   scrub      a wired position reads there directly
//   swap       a new sample mid-note changes nothing of the sounding note (click-free); the next trigger plays it
//   rt         10 000 blocks of note-ons, sample swaps and slice-table publishes: no allocation on the audio side

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/dsp/sample_slot.h"
#include "pq/graph/parse.h"
#include "pq/graph/patch_file.h"
#include "pq/meta/registry.h"
#include "pq/meta/sampler.h"

using namespace pq;
using namespace pq::meta;

namespace {

namespace fs = std::filesystem;

constexpr double kSr = 48000.0;
constexpr float kRoot = 261.63f;

Expansion mustExpand(const ExpandContext& c, const std::string& name = "smp") {
  auto e = expand(samplerType(), c, name);
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  return std::move(e.value());
}

// A compiled root patch R calling the global expansion `smp` with `settings` (its Sample request on slot 1).
struct Rig {
  std::unique_ptr<compile::SourceFile> src;
  compile::Compiled c;
  int gate = -1, hz = -1, key = -1, pos = -1;  // param slots of the root's params
};

Rig compileRig(const std::vector<double>& settings, const std::string& call, const std::string& position = {}) {
  ExpandContext x;
  x.voice = false;
  x.settings = settings;
  const uint32_t slots[] = {1};
  x.slots = slots;
  std::string text = graph::print(mustExpand(x).patch);
  text += "\npatch R {\n  param gate : bool = false\n  param hz = 261.63\n  param key = 0\n  param pos = 0\n"
          "  out l : audio\n  out r : audio\n  out g : audio\n";
  text += "  s = smp(hz: hz, gate: gate, key: key" + (position.empty() ? std::string() : ", position: " + position) +
          call + ")\n";
  text += "  l = s.out[0]\n  r = s.out[1]\n  g = s.gateOut\n}\n";
  const fs::path f = fs::temp_directory_path() / "pq_test_sampler.pqp";
  std::ofstream(f, std::ios::binary) << text;
  auto src = compile::SourceFile::load(f);
  REQUIRE_MESSAGE(src.ok(), (src.ok() ? std::string() : src.error().message));
  auto idx = (*src)->findPatch("R");
  REQUIRE(idx.ok());
  compile::CompileOptions co;
  co.sampleRate = kSr;
  auto c = compile::compilePatch(**src, *idx, co);
  REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message) << "\n" << text);
  Rig r;
  r.src = std::move(src.value());
  r.c = std::move(c.value());
  const auto& params = r.c.schedule.params;
  for (size_t i = 0; i < params.size(); ++i) {
    const std::string n = r.c.scheduled().patch.nodeName(params[i].node);
    if (n == "gate") r.gate = static_cast<int>(i);
    if (n == "hz") r.hz = static_cast<int>(i);
    if (n == "key") r.key = static_cast<int>(i);
    if (n == "pos") r.pos = static_cast<int>(i);
  }
  REQUIRE(r.gate >= 0);
  fs::remove(f);
  return r;
}

// What the root's params are per block.
struct Drive {
  bool gate = false;
  float hz = kRoot, key = 0, pos = 0;
};
using Score = std::function<Drive(uint64_t frame)>;  // the params of the block starting at `frame`

struct Render {
  std::vector<float> l, r, g;
  uint64_t rtViolations = 0;
};

// Renders `frames` frames in blocks of `block` with the slot as resource 1. `between`: called before each block (off
// the audio side: publishes).
Render render(const Rig& rig, compile::Backend backend, dsp::SampleSlot& slot, uint64_t frames, uint32_t block,
              const Score& score, double bpm = 120.0, const std::function<void(uint64_t)>& between = {}) {
  const dsp::SampleSlot* views[2] = {nullptr, &slot};
  engine::Resources res;
  res.samples = views;
  compile::ImageOptions io;
  io.maxBlockFrames = 1024;
  io.maxVoiceGroups = 1;
  io.resources = &res;
  auto img = compile::buildImage(rig.c.schedule, backend, io);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? std::string() : img.error().message));
  engine::ProgramImage& im = **img;
  REQUIRE(im.io().globalOut.size() == 3);
  std::vector<float> params(im.io().params.size());
  for (size_t i = 0; i < params.size(); ++i) params[i] = im.io().params[i].defaultValue;
  Render out;
  out.l.resize(frames), out.r.resize(frames), out.g.resize(frames);
  std::vector<float> bl(1024), br(1024), bg(1024);
  float* outs[3] = {nullptr, nullptr, nullptr};
  for (size_t i = 0; i < 3; ++i) {  // the out slots by name
    const std::string n = rig.c.scheduled().patch.nodeName(im.io().globalOut[i].node);
    outs[i] = n == "l" ? bl.data() : n == "r" ? br.data() : bg.data();
  }
  DenormalGuard dg;
  for (uint64_t t0 = 0; t0 < frames; t0 += block) {
    if (between) between(t0);
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(block, frames - t0));
    const Drive d = score(t0);
    params[rig.gate] = d.gate ? 1.0f : 0.0f;  // (a bool param is written 0 / 1)
    if (rig.hz >= 0) params[rig.hz] = d.hz;
    if (rig.key >= 0) params[rig.key] = d.key;
    if (rig.pos >= 0) params[rig.pos] = d.pos;
    engine::ProcessContext ctx;
    ctx.frames = n;
    ctx.sampleRate = kSr;
    ctx.sampleTime = static_cast<int64_t>(t0);
    ctx.transport.bpm = bpm;
    ctx.transport.playing = true;
    ctx.out = outs;
    ctx.numOut = 3;
    ctx.params = params.data();
    ctx.numParams = static_cast<uint32_t>(params.size());
    out.rtViolations += alloc_trap::countRtViolations([&] { im.process(ctx); });
    std::copy_n(bl.data(), n, out.l.data() + t0);
    std::copy_n(br.data(), n, out.r.data() + t0);
    std::copy_n(bg.data(), n, out.g.data() + t0);
  }
  return out;
}

// ---- the reference ------------------------------------------------------------------------------------------------
// One sample_read lane in double, frame positions (the kernel's float conversions of the module's values done the
// same way: the inputs are float32 products), plus the module's gate / gain.
struct RefRead {
  const std::vector<float>* left = nullptr;
  const std::vector<float>* right = nullptr;
  double pos = 0;
  bool active = false, back = false;
  static double hermite(double xm1, double x0, double x1, double x2, double t) {
    const double c1 = 0.5 * (x1 - xm1);
    const double c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
    const double c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
  }
  // Frame values of the block (the module's normalised positions × length, in float as the kernel does).
  struct In {
    float rate = 1;
    bool reset = false;
    double start = 0, ls = 0, le = 0, fade = 0, end = 1e30, fi = 0, fo = 0;
    bool pp = false;
  };
  double at(const std::vector<float>& d, int64_t i) const {
    return i >= 0 && i < static_cast<int64_t>(d.size()) ? static_cast<double>(d[static_cast<size_t>(i)]) : 0.0;
  }
  void step(const In& in, double& yl, double& yr, bool& playing) {
    const auto len = static_cast<int64_t>(left->size());
    const double ls = std::floor(in.ls), le = std::floor(in.le);
    const bool loopOn = le > ls, pp = loopOn && in.pp;
    const double endEff = std::min(std::floor(in.end), static_cast<double>(len));
    const double F = loopOn && !pp ? std::min({std::max(std::floor(in.fade), 0.0), le - ls, std::max(ls, 0.0)}) : 0.0;
    if (in.reset) {
      pos = in.start;
      active = std::floor(pos) >= 0 && std::floor(pos) < endEff;
      back = false;
    }
    const int64_t idx = static_cast<int64_t>(std::floor(pos));
    const int64_t ci = std::clamp<int64_t>(idx, 0, len - 1);
    const double fr = pos - std::floor(pos);
    const bool inLoop = loopOn && ci >= ls && ci < le;
    const auto ils = static_cast<int64_t>(ls), ile = static_cast<int64_t>(le);
    auto read = [&](const std::vector<float>& d, int64_t i, bool loopTaps) {
      auto tap = [&](int64_t j) {
        if (loopTaps && inLoop) {
          if (pp) {
            if (j > ile - 1) j = 2 * (ile - 1) - j;
            if (j < ils) j = 2 * ils - j;
          } else {
            while (j >= ile) j -= ile - ils;
            while (j < ils) j += ile - ils;
          }
        }
        return at(d, std::clamp<int64_t>(j, -1, len));
      };
      return hermite(tap(i - 1), tap(i), tap(i + 1), tap(i + 2), fr);
    };
    yl = read(*left, ci, true), yr = read(*right, ci, true);
    if (F > 0 && inLoop && ci >= le - F) {
      const double u = std::numbers::pi / 2 * ((static_cast<double>(ci) - (le - F)) + fr) / F;
      const int64_t j = std::clamp<int64_t>(idx - (ile - ils), 0, len - 1);
      yl = std::cos(u) * yl + std::sin(u) * read(*left, j, false);
      yr = std::cos(u) * yr + std::sin(u) * read(*right, j, false);
    }
    if (in.fi > 0 || in.fo > 0) {
      const double x = static_cast<double>(idx) + fr;
      const double gi = in.fi > 0 ? (x - in.start) / in.fi : 1.0, go = in.fo > 0 ? (endEff - x) / in.fo : 1.0;
      const double g = std::clamp(std::min({gi, go, 1.0}), 0.0, 1.0);
      yl *= g, yr *= g;
    }
    playing = active;
    if (!active) yl = yr = 0;
    if (!active) return;
    double inc = std::floor(static_cast<double>(in.rate) * 0x1p32) * 0x1p-32;
    if (back) inc = -inc;
    double next = pos + inc;
    if (loopOn && idx < le && std::floor(next) >= le) {
      if (pp) next = 2 * (le - 1) - next, back = !back;
      else
        while (std::floor(next) >= le) next -= (le - ls);
    } else if (loopOn && idx >= ls && std::floor(next) < ls) {
      if (pp) next = 2 * ls - next, back = !back;
      else
        while (std::floor(next) < ls) next += (le - ls);
    }
    pos = next;
    active = std::floor(pos) >= 0 && std::floor(pos) < endEff;
  }
};

std::vector<float> tone(size_t n, double cycles, double phase) {
  std::vector<float> v(n);
  for (size_t k = 0; k < n; ++k)
    v[k] = static_cast<float>(0.6 * std::sin(2 * std::numbers::pi * cycles * k / n + phase) + 0.2 * std::sin(0.0031 * k));
  return v;
}

struct Sample {
  pq::EpochReclaimer reclaimer{1};
  dsp::SampleSlot slot;
  std::vector<float> l, r;
  std::shared_ptr<const dsp::SampleData> data;
  Sample(size_t n, double fileRate, std::vector<float> slices = {}) : l(tone(n, 37.0, 0.0)), r(tone(n, 23.0, 1.0)) {
    data = dsp::SampleData::build(l, r).value();
    REQUIRE(slot.publish(dsp::SampleData::source(data, fileRate, std::move(slices)), kSr, reclaimer).ok());
  }
};

float f32(double x) { return static_cast<float>(x); }

// Compares a render with the reference on both channels (and the gate output).
void checkClose(const Render& a, const std::vector<double>& rl, const std::vector<double>& rr, const std::vector<double>& rg,
                double tol) {
  double worst = 0;
  size_t at = 0;
  for (size_t t = 0; t < rl.size(); ++t) {
    const double e = std::max(std::fabs(a.l[t] - rl[t]), std::fabs(a.r[t] - rr[t]));
    if (e > worst) worst = e, at = t;
    REQUIRE(a.g[t] == static_cast<float>(rg[t]));
  }
  INFO("worst at frame " << at);
  CHECK(worst < tol);
}

}  // namespace

TEST_CASE("sampler: registered; inputs, params, settings; every setting combination expands in both contexts") {
  const Type& t = samplerType();
  CHECK(findType("sampler") == &t);
  CHECK(t.voiceEnd);
  CHECK(t.chainInput == t.inputIndex("hz"));
  CHECK(t.inputs[kSmpHz].def == InputDefault::Unwired);
  CHECK(t.inputs[kSmpGate].def == InputDefault::VoiceGate);
  CHECK(t.params[kSmpRoot].def == kSamplerDefaultRoot);
  ExpandContext c;
  const Expansion a = mustExpand(c), b = mustExpand(c);
  CHECK(a.patch == b.patch);
  REQUIRE(a.resources.size() == 2);  // SM.10: the Sample, then the face's VoiceTap (the playheads)
  CHECK(a.resources[0].kind == ResourceKind::Sample);
  CHECK(a.resources[1].kind == ResourceKind::VoiceTap);
  CHECK(a.resources[1].index == 1);
  const std::string text = graph::print(a.patch);
  CAPTURE(text);
  auto parsed = graph::parse(text, "expansion.pqp");
  REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
  REQUIRE(parsed->patches.size() == 1);
  CHECK(parsed->patches[0] == a.patch);
  auto readId = [](const Expansion& e) {
    for (graph::NodeIndex n = 0; n < e.patch.nodeCount(); ++n)
      if (e.patch.node(n).kind == graph::NodeKind::Atom &&
          graph::atomSignature(e.patch.node(n).atom).name == "sample_read_cubic")
        return e.patch.node(n).id.value;
    return 0u;
  };
  std::vector<uint64_t> keys;
  for (double mode : {0.0, 1.0, 2.0})
    for (double warp : {0.0, 1.0})
      for (double loop : {0.0, 1.0, 2.0})
        for (double trig : {0.0, 1.0})
          for (bool voice : {false, true}) {
            const std::vector<double> s = {mode, warp, loop, trig};
            ExpandContext x;
            x.settings = s;
            x.voice = voice;
            const Expansion e = mustExpand(x);
            CHECK(readId(e) == kBodyIdBase + 0);
            keys.push_back(e.key);
            // The text form parses back to the same graph.
            auto p = graph::parse(graph::print(e.patch), "x.pqp");
            REQUIRE(p.ok());
            CHECK(p->patches[0] == e.patch);
          }
  std::sort(keys.begin(), keys.end());
  CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
  const auto pre = prelude();
  const Prelude::Entry* e = pre->find("sampler");
  REQUIRE(e != nullptr);
  CHECK(e->notes.has_value());
  CHECK(e->audio.has_value());
}

TEST_CASE("sampler: data — the sample id and slices, validation, sampleRefs / sampleBinding") {
  const Type& t = samplerType();
  const DataPtr d = t.defaultData();
  CHECK(t.dataProblem(*d).empty());
  CHECK(samplerSample(d.get()) == 0);
  CHECK(t.sampleRefs(*d).empty());
  const DataPtr s = samplerData(7, {100.0f, 50.0f});
  CHECK(t.dataProblem(*s).empty());
  CHECK(t.sampleRefs(*s) == std::vector<uint32_t>{7});
  const SampleBinding b = t.sampleBinding(s.get(), 0);
  CHECK(b.sample == 7);
  CHECK(b.slices == std::vector<float>{100.0f, 50.0f});
  CHECK(t.sampleBinding(s.get(), 1).sample == 0);
  CHECK(!t.dataProblem(DataValue::record({{"sample", DataValue::num(-1)}})).empty());
  CHECK(!t.dataProblem(DataValue::record({{"sample", DataValue::num(1.5)}})).empty());
  CHECK(!t.dataProblem(DataValue::record({{"sample", DataValue::num(2)}, {"slices", DataValue::num(3)}})).empty());
  CHECK(!t.dataProblem(DataValue::num(2)).empty());
}

TEST_CASE("sampler: Classic — a forward loop with its crossfade and a ping-pong loop match the reference on both backends") {
  // 9 600 frames at 32 kHz (rate scale 2/3), loop [0.3, 0.75) of it; hz = 1.5 · root (rate 1.5 · 2/3 = 1 frame per
  // sample, an exact float); held 0.5 s, released (the loop keeps playing: the envelope after it releases), retriggered.
  Sample smp(9600, 32000.0);
  for (const double loop : {1.0, 2.0}) {
    CAPTURE(loop);
    const Rig rig = compileRig({0.0, 0.0, loop, 0.0}, ", start: 0.1, loopStart: 0.3, loopLength: 0.45, loopFade: 0.02");
    const Score score = [](uint64_t t) {
      Drive d;
      d.gate = (t >= 64 && t < 24000) || t >= 30016;
      d.hz = f32(1.5 * 261.63);
      return d;
    };
    const uint64_t N = 40000;
    // Reference: rate = (hz / root) · 1 · scale, positions × 9600.
    RefRead ref{&smp.l, &smp.r};
    std::vector<double> rl(N), rr(N), rg(N);
    bool prevGate = false;
    const float scale = static_cast<float>(32000.0 / kSr);
    for (uint64_t t = 0; t < N; ++t) {
      const Drive d = score(t - t % 64);
      RefRead::In in;
      in.rate = (f32(d.hz) / kRoot) * 1.0f * scale;
      in.reset = d.gate && !prevGate;
      prevGate = d.gate;
      in.start = 0.1f * 9600.0f;
      in.ls = 0.3f * 9600.0f;
      in.le = std::min(0.3f + 0.45f, 1.0f) * 9600.0f;
      in.fade = loop == 1.0 ? 0.02f * 32000.0f : 0.0;
      in.pp = loop == 2.0;
      bool playing = false;
      ref.step(in, rl[t], rr[t], playing);
      rg[t] = d.gate ? 1.0 : 0.0;
    }
    const Render ri = render(rig, compile::Backend::Interpreter, smp.slot, N, 64, score);
    const Render rj = render(rig, compile::Backend::Jit, smp.slot, N, 64, score);
    checkClose(ri, rl, rr, rg, 2e-5);
    CHECK(ri.l == rj.l);  // the backends are bit-identical
    CHECK(ri.r == rj.r);
    CHECK(ri.rtViolations == 0);
    CHECK(rj.rtViolations == 0);
    // Block sizes do not matter (the score's events fall on 32-frame boundaries too).
    const Render r32 = render(rig, compile::Backend::Interpreter, smp.slot, N, 32, score);
    CHECK(r32.l == ri.l);
  }
}

TEST_CASE("sampler: One-Shot — start → end with fades, Re-Pitch follows the tempo, the gate output ends with the sample") {
  Sample smp(48000, 48000.0);
  for (const double trigger : {0.0, 1.0}) {
    CAPTURE(trigger);
    const Rig rig = compileRig({1.0, 1.0, 0.0, trigger}, ", start: 0.2, end: 0.5, fadeIn: 0.01, fadeOut: 0.05, sampleBpm: 100");
    const Score score = [](uint64_t t) {
      Drive d;
      d.gate = t >= 128 && t < 4096;  // a short note: Trigger plays on, Gate ends with the note
      d.hz = kRoot;
      return d;
    };
    const uint64_t N = 20000;
    RefRead ref{&smp.l, &smp.r};
    std::vector<double> rl(N), rr(N), rg(N);
    bool prevGate = false;
    for (uint64_t t = 0; t < N; ++t) {
      const Drive d = score(t - t % 64);
      RefRead::In in;
      in.rate = ((f32(d.hz) / kRoot) * 1.0f) * (f32(150.0) / 100.0f);  // Re-Pitch: bpm 150 over the sample's 100
      in.reset = d.gate && !prevGate;
      prevGate = d.gate;
      in.start = 0.2f * 48000.0f;
      in.end = 0.5f * 48000.0f;
      in.fi = 0.01f * 48000.0f;
      in.fo = 0.05f * 48000.0f;
      bool playing = false;
      ref.step(in, rl[t], rr[t], playing);
      rg[t] = (trigger == 1.0 ? (d.gate && playing) : playing) ? 1.0 : 0.0;
    }
    const Render ri = render(rig, compile::Backend::Interpreter, smp.slot, N, 64, score, 150.0);
    const Render rj = render(rig, compile::Backend::Jit, smp.slot, N, 64, score, 150.0);
    checkClose(ri, rl, rr, rg, 2e-5);
    CHECK(ri.l == rj.l);
    // The note ends: silent after the end (Trigger) or the release (Gate).
    CHECK(ri.l[N - 1] == 0.0f);
    CHECK(ri.g[N - 1] == 0.0f);
  }
}

TEST_CASE("sampler: Slice — key 36 + n plays slice n of the data's table; keys out of range are silent") {
  Sample smp(24000, 48000.0, {3000.0f, 0.0f, 9000.0f, 15000.0f});  // sorted: 0, 3000, 9000, 15000
  const Rig rig = compileRig({2.0, 0.0, 0.0, 0.0}, ", fadeOut: 0.001");
  // Notes of 4000 frames: keys 36, 38, 39, 41 (out of range), 37; transpose 0: the slice at its recorded pitch.
  const float keys[] = {36, 38, 39, 41, 37};
  const Score score = [&](uint64_t t) {
    Drive d;
    const uint64_t n = t / 5000;
    d.gate = n < 5 && t % 5000 < 4000;
    d.key = keys[std::min<uint64_t>(n, 4)];
    return d;
  };
  const uint64_t N = 25000;
  const double bounds[] = {0, 3000, 9000, 15000, 24000};
  RefRead ref{&smp.l, &smp.r};
  std::vector<double> rl(N), rr(N), rg(N);
  bool prevGate = false;
  for (uint64_t t = 0; t < N; ++t) {
    const Drive d = score(t - t % 64);
    const int n = static_cast<int>(std::lround(d.key)) - 36;
    RefRead::In in;
    in.rate = 1.0f;
    in.reset = d.gate && !prevGate;
    prevGate = d.gate;
    // sample_slice in units 1: the bounds / length (float), read back × length.
    const float inv = 1.0f / 24000.0f;
    in.start = n >= 0 && n < 4 ? f32(bounds[n]) * inv * 24000.0f : 0.0;
    in.end = n >= 0 && n < 4 ? f32(bounds[n + 1]) * inv * 24000.0f : 0.0;
    in.fo = 0.001f * 48000.0f;
    bool playing = false;
    ref.step(in, rl[t], rr[t], playing);
    rg[t] = playing ? 1.0 : 0.0;
  }
  const Render ri = render(rig, compile::Backend::Interpreter, smp.slot, N, 64, score);
  const Render rj = render(rig, compile::Backend::Jit, smp.slot, N, 64, score);
  checkClose(ri, rl, rr, rg, 2e-5);
  CHECK(ri.l == rj.l);
  // Key 41 (slice 5 of 4): silent through its note.
  for (uint64_t t = 15040; t < 19000; ++t) REQUIRE(ri.l[t] == 0.0f);  // (from its note-on: the block at 15040)
}

TEST_CASE("sampler: a wired position reads the sample there (a scrub), on both backends") {
  // The position: phasor(2 Hz), a 0 → 1 ramp twice a second (its value at frame t: (t · round(2 · 2^32 / sr) mod 2^32)
  // >> 8, × 2^-24, exact in float).
  Sample smp(4800, 48000.0);
  const Rig rig = compileRig({0.0, 0.0, 0.0, 0.0}, "", "phasor(2Hz)");
  const Score score = [](uint64_t) { return Drive{}; };
  const uint64_t N = 30000;
  const Render ri = render(rig, compile::Backend::Interpreter, smp.slot, N, 32, score);
  const Render rj = render(rig, compile::Backend::Jit, smp.slot, N, 32, score);
  CHECK(ri.l == rj.l);
  const uint32_t inc = static_cast<uint32_t>(std::llround(2.0 * 4294967296.0 / kSr));
  double worst = 0;
  for (uint64_t t = 0; t < N; ++t) {
    const uint32_t ph = static_cast<uint32_t>(t * inc);
    const float p = static_cast<float>(ph >> 8) * 0x1p-24f;
    const float x = std::min(p * 4800.0f, 4799.0f);
    const int64_t i = static_cast<int64_t>(std::floor(x));
    auto at = [&](const std::vector<float>& d, int64_t k) {
      return k >= 0 && k < 4800 ? static_cast<double>(d[static_cast<size_t>(k)]) : 0.0;
    };
    const double fr = x - std::floor(x);
    const double wl = RefRead::hermite(at(smp.l, i - 1), at(smp.l, i), at(smp.l, i + 1), at(smp.l, i + 2), fr);
    const double wr = RefRead::hermite(at(smp.r, i - 1), at(smp.r, i), at(smp.r, i + 1), at(smp.r, i + 2), fr);
    worst = std::max({worst, std::fabs(ri.l[t] - wl), std::fabs(ri.r[t] - wr)});
  }
  CHECK(worst < 2e-6);
}

TEST_CASE("sampler: a sample swap mid-note changes nothing of the sounding note; the next trigger plays the new one") {
  Sample a(48000, 48000.0);
  const std::vector<float> bl(48000, 0.125f), br(48000, -0.125f);
  auto bdata = dsp::SampleData::build(bl, br).value();
  const Rig rig = compileRig({0.0, 0.0, 1.0, 0.0}, "");
  const Score score = [](uint64_t t) {
    Drive d;
    d.gate = t < 20032 || t >= 20096;  // held, a retrigger at 20096
    return d;
  };
  for (const auto backend : {compile::Backend::Interpreter, compile::Backend::Jit}) {
    // Without a swap (the reference) and with one at frame 8000.
    Sample plain(48000, 48000.0);
    const Render ref = render(rig, backend, plain.slot, 30000, 64, score);
    const Render swapped = render(rig, backend, a.slot, 30000, 64, score, 120.0, [&](uint64_t t0) {
      if (t0 == 8000) REQUIRE(a.slot.publish(dsp::SampleData::source(bdata, 48000.0), kSr, a.reclaimer).ok());
    });
    for (uint64_t t = 0; t < 20032; ++t) REQUIRE(swapped.l[t] == ref.l[t]);  // the held note: identical, no click
    CHECK(swapped.l[20096 + 10] == doctest::Approx(0.125f));  // retriggered: the new sample
    CHECK(swapped.r[20096 + 10] == doctest::Approx(-0.125f));
    REQUIRE(a.slot.publish(dsp::SampleData::source(a.data, 48000.0), kSr, a.reclaimer).ok());  // back for the next pass
  }
}

TEST_CASE("sampler: 10 000 blocks of note-ons, sample swaps and slice publishes allocate nothing on the audio side") {
  Sample smp(12000, 44100.0, {0.0f, 4000.0f, 8000.0f});
  auto other = dsp::SampleData::build(tone(7000, 11.0, 0.3)).value();  // mono
  for (const double mode : {0.0, 2.0}) {
    const Rig rig = compileRig({mode, 1.0, 2.0, 1.0}, ", loopStart: 0.1, loopLength: 0.3");
    std::mt19937 rng(5);
    for (const auto backend : {compile::Backend::Interpreter, compile::Backend::Jit}) {
      uint32_t publishes = 0;
      const Render r = render(rig, backend, smp.slot, 10000ull * 37, 37,
          [&](uint64_t t) {
            Drive d;
            d.gate = (t / 37) % 7 < 5;
            d.key = static_cast<float>(36 + (t / 37 / 7) % 4);
            d.hz = static_cast<float>(150.0 + (t / 37) % 300);
            return d;
          },
          140.0,
          [&](uint64_t t0) {
            const uint64_t b = t0 / 37;
            if (b % 50 == 17) {  // a swap (new data) or a new slice table (same data), off the audio side
              std::vector<float> slices = {0.0f, static_cast<float>(rng() % 6000), static_cast<float>(6000 + rng() % 5000)};
              const bool swap = b % 100 == 17;
              REQUIRE(smp.slot.publish(swap ? dsp::SampleData::source(other, 22050.0, slices)
                                            : dsp::SampleData::source(smp.data, 44100.0, slices),
                                       kSr, smp.reclaimer).ok());
              ++publishes;
            }
            if (b % 100 == 60) smp.slot.sweep(smp.reclaimer);
          });
      CHECK(r.rtViolations == 0);
      CHECK(publishes >= 199);
      bool sounded = false;
      for (const float x : r.l) sounded = sounded || x != 0.0f;
      CHECK(sounded);
    }
  }
}
