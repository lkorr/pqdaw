// TMM.6: the oversample(n) region on both backends. A region holding tanh(x · 8) at 4× / 8× matches a double reference
// (up → shape → down with the same filters) within 1e-5, and its output below 20 kHz holds nothing that its
// oversampled signal does not (the half-bands' rejection of everything that folds, measured on a full-scale 5 kHz
// sine); a phasor inside keeps its exact pitch; a phase input stays a clean ramp; interpreter == JIT bit for bit at
// every block size, per voice too; RtScope around every block; state transfer across a hot swap (the region's
// filters and phasors keep their state; a new factor starts them fresh).
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../engine/interp_harness.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/fft.h"
#include "pq/dsp/oversample.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/state_transfer.h"
#include "pq/jit/compiler.h"

using namespace interp_test;
namespace jit = pq::jit;
namespace os = pq::dsp::os;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kMaxBlock = 1024;

jit::Compiler& osCompiler() {
  static std::unique_ptr<jit::Compiler> c = [] {
    auto r = jit::Compiler::create();
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return std::move(r.value());
  }();
  return *c;
}

enum class Backend { Interp, Jit };

std::unique_ptr<eng::ProgramImage> build(const Schedule& s, Backend b, uint32_t groups = 1) {
  if (b == Backend::Interp) {
    eng::InterpretOptions io;
    io.maxBlockFrames = kMaxBlock;
    io.maxVoiceGroups = groups;
    auto r = eng::buildInterpreted(s, io);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return std::move(r.value());
  }
  jit::CompileOptions o;
  o.maxBlockFrames = kMaxBlock;
  o.maxVoiceGroups = groups;
  auto r = osCompiler().compile(s, o);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
  return std::move(r.value());
}

// Runs a global image: one root input (or none), output slot 0; returns `frames` output samples. Counts allocations
// and other RT violations inside process().
struct GlobalRun {
  std::unique_ptr<eng::ProgramImage> img;
  uint64_t t = 0;
  uint64_t rtViolations = 0;

  std::vector<float> run(const std::vector<float>& in, uint32_t frames, uint32_t block) {
    const eng::ImageInterface& face = img->io();
    std::vector<float> out(frames), inBuf(kMaxBlock);
    std::vector<std::vector<float>> outBufs(std::max<size_t>(face.globalOut.size(), 1), std::vector<float>(kMaxBlock));
    std::vector<float> params(face.params.size(), NAN);
    const float* ins[1] = {inBuf.data()};
    std::vector<float*> outs;
    for (auto& b : outBufs) outs.push_back(b.data());  // slot 0: the patch's output (exports follow)
    const std::vector<float>& outBuf = outBufs[0];
    for (uint32_t t0 = 0; t0 < frames; t0 += block) {
      const uint32_t n = std::min(block, frames - t0);
      if (!in.empty()) std::memcpy(inBuf.data(), in.data() + t0, n * sizeof(float));
      eng::ProcessContext ctx;
      ctx.frames = n;
      ctx.sampleRate = kSr;
      ctx.sampleTime = t;
      ctx.in = ins;
      ctx.numIn = static_cast<uint32_t>(face.globalIn.size());
      ctx.out = outs.data();
      ctx.numOut = static_cast<uint32_t>(face.globalOut.size());
      ctx.params = params.data();
      ctx.numParams = static_cast<uint32_t>(params.size());
      rtViolations += pq::alloc_trap::countRtViolations([&] {
        pq::DenormalGuard dg;
        img->process(ctx);
      });
      std::memcpy(out.data() + t0, outBuf.data(), n * sizeof(float));
      t += n;
    }
    return out;
  }
};

std::vector<float> render(const Compiled& c, Backend b, const std::vector<float>& in, uint32_t frames, uint32_t block,
                          uint64_t* rt = nullptr) {
  GlobalRun r{build(c.sched, b)};
  std::vector<float> y = r.run(in, frames, block);
  if (rt) *rt += r.rtViolations;
  return y;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

std::string tanhPatch(uint32_t n) {
  return "patch T {\n  in x : audio\n  out y : audio\n  oversample(" + std::to_string(n) +
         ") {\n    y = tanh(x * 8)\n  }\n}\n";
}

// The double reference of a cascade: the kernels' structure and float coefficients, in double.
struct RefCascade {
  double y1[os::kAllpasses] = {};
  double xin[2 * os::kMaxStages] = {};
  double path(uint32_t s, uint32_t p, double x) {
    double prevIn = xin[2 * s + p];
    xin[2 * s + p] = x;
    for (uint32_t k = p; k < os::kStageSize[s]; k += 2) {
      const uint32_t i = os::kStageFirst[s] + k;
      const double c = static_cast<float>(os::kCoefs[i]);
      const double y = c * (x - y1[i]) + prevIn;
      prevIn = y1[i];
      y1[i] = y;
      x = y;
    }
    return x;
  }
  void up(double x, uint32_t n, std::vector<double>& out) {
    out.assign(1, x);
    for (uint32_t s = 0; (1u << s) < n; ++s) {
      std::vector<double> next;
      for (double v : out) {
        next.push_back(path(s, 0, v));
        next.push_back(path(s, 1, v));
      }
      out = std::move(next);
    }
  }
  double down(std::vector<double> buf) {
    for (uint32_t s = os::stagesOf(static_cast<uint32_t>(buf.size())); s-- > 0;) {
      std::vector<double> next;
      for (size_t j = 0; j + 1 < buf.size(); j += 2) next.push_back(0.5 * (path(s, 0, buf[j + 1]) + path(s, 1, buf[j])));
      buf = std::move(next);
    }
    return buf[0];
  }
};

// Amplitude spectrum (a full-scale sine = 1) of n samples, bins 0 … n/2.
std::vector<double> spectrum(const float* x, uint32_t n) {
  pq::dsp::RealFft fft(n);
  std::vector<float> re(n / 2 + 1), im(n / 2 + 1);
  fft.forward(x, re.data(), im.data());
  std::vector<double> a(n / 2 + 1);
  for (uint32_t b = 0; b <= n / 2; ++b) a[b] = 2.0 * std::hypot(static_cast<double>(re[b]), static_cast<double>(im[b])) / n;
  return a;
}

double db(double a) { return 20.0 * std::log10(std::max(a, 1e-30)); }

}  // namespace

TEST_CASE("oversample: tanh(x·8) at 4× / 8× matches the double reference; both backends, every block size, bit for bit") {
  constexpr uint32_t kFrames = 6000;
  std::vector<float> x(kFrames);
  for (uint32_t t = 0; t < kFrames; ++t)
    x[t] = static_cast<float>(std::sin(2.0 * std::numbers::pi * 5000.0 * t / kSr) * (t < 3000 ? 1.0 : 0.3) +
                              0.1 * std::sin(0.0007 * t * t / 100.0));
  for (uint32_t n : {4u, 8u}) {
    CAPTURE(n);
    const Compiled c = compileText(tanhPatch(n), kSr);
    uint64_t rt = 0;
    const std::vector<float> ref = render(c, Backend::Interp, x, kFrames, kMaxBlock, &rt);
    for (uint32_t block : {1u, 37u, 64u, kMaxBlock}) {
      CAPTURE(block);
      CHECK(sameBits(render(c, Backend::Interp, x, kFrames, block, &rt), ref));
      CHECK(sameBits(render(c, Backend::Jit, x, kFrames, block, &rt), ref));
    }
    CHECK(rt == 0);
    RefCascade up, down;
    std::vector<double> u;
    double worst = 0;
    for (uint32_t t = 0; t < kFrames; ++t) {
      up.up(x[t], n, u);
      for (double& v : u) v = std::tanh(v * 8.0);
      worst = std::max(worst, std::abs(down.down(u) - ref[t]));
    }
    MESSAGE("tanh(x·8) at ×" << n << ": max |error| vs the double reference " << worst);
    CHECK(worst <= 1e-5);
  }
}

TEST_CASE("oversample: feedback inside a region (a comb in sub-blocks, a one-pole per sample) runs at the region's rate") {
  const char* src = R"(
patch F {
  in x : audio
  out y : audio
  oversample(4) {
    c = x + 0.5 * delay(c, 1ms, 5ms)
    lp = z1(lp) + 0.05 * (c - z1(lp))
    y = lp
  }
}
)";
  const Compiled c = compileText(src, kSr);
  bool subBlocked = false, perSample = false;
  for (const Region& r : c.sched.regions) {
    if (r.factor != 4 || r.kind != RegionKind::Loop) continue;
    subBlocked |= r.subBlock == 192;  // 1 ms at 192 kHz
    perSample |= r.subBlock == 1;
  }
  CHECK(subBlocked);
  CHECK(perSample);
  constexpr uint32_t kFrames = 3000;
  std::vector<float> x(kFrames);
  for (uint32_t t = 0; t < kFrames; ++t) x[t] = static_cast<float>(0.6 * std::sin(0.07 * t) + (t % 700 == 0 ? 0.4 : 0.0));
  uint64_t rt = 0;
  const std::vector<float> ref = render(c, Backend::Interp, x, kFrames, kMaxBlock, &rt);
  for (uint32_t block : {1u, 37u, 64u, kMaxBlock}) {
    CAPTURE(block);
    CHECK(sameBits(render(c, Backend::Interp, x, kFrames, block, &rt), ref));
    CHECK(sameBits(render(c, Backend::Jit, x, kFrames, block, &rt), ref));
  }
  CHECK(rt == 0);
  // The double reference at 192 kHz: the same delay (the kernel's float delay time, linear taps) and one-pole.
  const float d = 0.001f * 192000.0f;
  const auto whole = static_cast<uint32_t>(std::floor(d));
  const double fr = d - std::floor(d);
  RefCascade up, down;
  std::vector<double> u, ring(1 << 16, 0.0);
  uint64_t w = 0;
  double lp = 0, worst = 0;
  for (uint32_t t = 0; t < kFrames; ++t) {
    up.up(x[t], 4, u);
    for (double& v : u) {
      const double x0 = w >= whole ? ring[(w - whole) & 0xFFFF] : 0.0;
      const double x1 = w >= whole + 1 ? ring[(w - whole - 1) & 0xFFFF] : 0.0;
      const double cv = v + 0.5 * (x0 + fr * (x1 - x0));
      ring[w++ & 0xFFFF] = cv;
      lp = lp + 0.05 * (cv - lp);
      v = lp;
    }
    worst = std::max(worst, std::abs(down.down(u) - ref[t]));
  }
  MESSAGE("feedback inside ×4: max |error| vs the double reference " << worst);
  CHECK(worst <= 1e-5);
}

TEST_CASE("oversample: a full-scale 5 kHz sine through tanh(x·8): nothing folds below 20 kHz beyond the half-band spec") {
  // Coherent analysis: N = 8192 at 48 kHz, f0 = bin 853 (4998.6 Hz, odd: no harmonic coincides with another's fold).
  constexpr uint32_t N = 8192, k0 = 853;
  const double f0 = k0 * kSr / N;
  std::vector<float> x(2 * N);
  for (uint32_t t = 0; t < 2 * N; ++t) x[t] = static_cast<float>(std::sin(2.0 * std::numbers::pi * f0 * t / kSr));
  for (uint32_t n : {4u, 8u}) {
    CAPTURE(n);
    const Compiled c = compileText(tanhPatch(n), kSr);
    const std::vector<float> y = render(c, Backend::Jit, x, 2 * N, 512);
    CHECK(sameBits(y, render(c, Backend::Interp, x, 2 * N, 512)));
    // What the oversampled signal holds below 20 kHz (in double: up → tanh, steady state = the second period).
    RefCascade up;
    std::vector<double> u;
    std::vector<float> z(static_cast<size_t>(n) * N);
    for (uint32_t t = 0; t < 2 * N; ++t) {
      up.up(x[t], n, u);
      if (t >= N)
        for (uint32_t j = 0; j < n; ++j) z[(t - N) * n + j] = static_cast<float>(std::tanh(8.0 * u[j]));
    }
    const std::vector<double> Z = spectrum(z.data(), n * N);  // bins of the same width (sr / N)
    const std::vector<double> Y = spectrum(y.data() + N, N);
    const uint32_t b20 = static_cast<uint32_t>(20000.0 / (kSr / N));
    double worst = -400, legit = 0;
    uint32_t worstBin = 0;
    for (uint32_t b = 1; b < b20; ++b) {
      if (db(Z[b]) > -150.0) {  // a component the oversampled signal has here (a harmonic, or a fold inside it)
        legit = std::max(legit, std::abs(db(Y[b]) - db(Z[b])) * (Z[b] > 1e-3 ? 1.0 : 0.0));
        continue;
      }
      if (db(Y[b]) > worst) {
        worst = db(Y[b]);
        worstBin = b;
      }
    }
    MESSAGE("×" << n << ": worst folded component below 20 kHz " << worst << " dBFS at " << worstBin * kSr / N
                << " Hz; passband level error " << legit << " dB");
    CHECK(worst <= -110.0);
    CHECK(legit < 0.001);
  }
}

TEST_CASE("oversample: a phasor inside a 4× region keeps its exact pitch") {
  // 750 Hz: at 192 kHz the increment is exactly 2^24 (were the region at sr, 2^26 would sound at 3 kHz).
  const Compiled c = compileText("patch P {\n  out y : audio\n  oversample(4) {\n    y = sine(phasor(750))\n  }\n}\n", kSr);
  constexpr uint32_t N = 48000;
  const std::vector<float> y = render(c, Backend::Jit, {}, 2 * N, 256);
  CHECK(sameBits(y, render(c, Backend::Interp, {}, 2 * N, 256)));
  // The period: the in-region phasor advances 750 / 192000 per sub-sample, so after 64 base samples (one period) the
  // output repeats exactly (steady state, after the filters' transients).
  double rep = 0;
  for (uint32_t t = N; t < N + 6400; ++t) rep = std::max(rep, std::abs(static_cast<double>(y[t]) - y[t + 64]));
  CHECK(rep < 1e-6);
  // And it is 750 Hz: the spectrum's peak.
  std::vector<float> win(y.begin() + N, y.begin() + 2 * N);
  pq::dsp::RealFft fft(32768);
  win.resize(32768);
  std::vector<float> re(16385), im(16385);
  fft.forward(win.data(), re.data(), im.data());
  uint32_t peak = 0;
  double best = 0;
  for (uint32_t b = 1; b <= 16384; ++b) {
    const double m = std::hypot(static_cast<double>(re[b]), static_cast<double>(im[b]));
    if (m > best) {
      best = m;
      peak = b;
    }
  }
  CHECK(std::abs(peak * kSr / 32768.0 - 750.0) < kSr / 32768.0);
}

TEST_CASE("oversample: a phase input is interpolated as a phase (clean), not filtered as audio") {
  constexpr uint32_t N = 16384, k0 = 343;  // 1004.9 Hz on a bin, its phase increment exactly 343 · 2^18
  const double hz = k0 * kSr / N;
  char buf[512];
  auto patch = [&](bool phase) {
    std::snprintf(buf, sizeof buf,
                  "patch P {\n  out y : audio\n  ph = phasor(%.17g)\n  oversample(4%s) {\n    y = sine(ph)\n  }\n}\n",
                  hz, phase ? ", phase: ph" : "");
    return compileText(buf, kSr);
  };
  const Compiled withPhase = patch(true), asAudio = patch(false);
  const std::vector<float> yp = render(withPhase, Backend::Jit, {}, 2 * N, 512);
  CHECK(sameBits(yp, render(withPhase, Backend::Interp, {}, 2 * N, 512)));
  const std::vector<float> ya = render(asAudio, Backend::Jit, {}, 2 * N, 512);
  auto spurs = [&](const std::vector<float>& y) {
    const std::vector<double> Y = spectrum(y.data() + N, N);
    double w = -400;
    for (uint32_t b = 1; b < N / 2; ++b)
      if (b != k0) w = std::max(w, db(Y[b]));
    return w;
  };
  const double sp = spurs(yp), sa = spurs(ya);
  MESSAGE("sine(phase) in a 4× region: largest spur " << sp << " dBFS with phase:, " << sa << " dBFS filtered as audio");
  CHECK(sp < -100.0);
  CHECK(sa > sp + 30.0);
}

TEST_CASE("oversample: per-voice regions, interpreter == JIT bit for bit, RtScope") {
  const Compiled c = compileText(R"(
patch V {
  in  gate : bool voice
  out y : audio voice
  out g : bool voice
  oversample(2) {
    y = tanh(sine(phasor(voice.hz)) * 4) * voice.velocity
    g = rise(gate)
  }
}
)",
                                 kSr);
  const Setup st(c, 11, 0);  // two voice groups, the second partly occupied
  for (uint32_t block : {1u, 64u, kMaxBlock}) {
    CAPTURE(block);
    const InterpRun a = runInterp(st, 3000, block);
    REQUIRE(a.error.empty());
    jit::Compiler* comp = &osCompiler();
    const InterpRun b = runInterp(st, 3000, block, kMaxBlock, [comp](const Schedule& s, const eng::InterpretOptions& io) {
      jit::CompileOptions o;
      o.maxBlockFrames = io.maxBlockFrames;
      o.maxVoiceGroups = io.maxVoiceGroups;
      return comp->compile(s, o);
    });
    REQUIRE(b.error.empty());
    CHECK(a.rtViolations == 0);
    CHECK(b.rtViolations == 0);
    REQUIRE(a.streams.size() == b.streams.size());
    for (const auto& [k, v] : a.streams) {
      CAPTURE(k.v);
      const std::vector<double>& w = b.streams.at(k);
      bool same = v.size() == w.size();
      for (size_t i = 0; same && i < v.size(); ++i) same = std::memcmp(&v[i], &w[i], sizeof(double)) == 0;
      CHECK(same);
    }
  }
}

TEST_CASE("oversample: state transfer across a hot swap keeps the region's state; a new factor starts it fresh") {
  auto compileP = [](const Patch& p, const StateLayout* prev) {
    InferOptions io;
    auto inf = infer(p, io);
    REQUIRE_MESSAGE(inf.ok(), (inf.ok() ? "" : inf.error().message));
    OptimiseOptions oo;
    oo.sampleRate = kSr;
    auto opt = optimise(std::move(inf.value()), oo);
    REQUIRE(opt.ok());
    ScheduleOptions so;
    so.sampleRate = kSr;
    so.previous = prev;
    auto s = schedule(opt.value(), so);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    Compiled c;
    c.typed = std::move(opt.value());
    c.sched = std::move(s.value());
    return c;
  };
  const char* src = R"(
patch S {
  in x : audio
  out y : audio
  oversample(4) {
    y = tanh(x * 8 + sine(phasor(333)))
  }
}
)";
  ParseResult pr = parseDetailed(src, "s.pqp");
  REQUIRE(pr.ok());
  const Patch a = pr.file.patches[0];
  // The edit: an unrelated export (NodeIds of everything else survive).
  Patch b = a;
  {
    const NodeIndex k = b.addConst(Constant::real(0.25));
    const NodeIndex ex = b.addExport("spare");
    b.connect(k, ex, 0);
  }
  constexpr uint32_t kFrames = 4096, kSplit = 1536;
  std::vector<float> x(kFrames);
  for (uint32_t t = 0; t < kFrames; ++t) x[t] = static_cast<float>(0.8 * std::sin(0.031 * t));
  const Compiled ca = compileP(a, nullptr);
  const Compiled cb = compileP(b, &ca.sched.state);
  for (Backend be : {Backend::Interp, Backend::Jit}) {
    // Uninterrupted A.
    GlobalRun whole{build(ca.sched, be)};
    const std::vector<float> want = whole.run(x, kFrames, 256);
    // A, then swap to B with state transfer, then B.
    GlobalRun first{build(ca.sched, be)};
    const std::vector<float> head(x.begin(), x.begin() + kSplit), tail(x.begin() + kSplit, x.end());
    std::vector<float> got = first.run(head, kSplit, 256);
    GlobalRun second{build(cb.sched, be)};
    auto plan = eng::buildTransferPlan(first.img->stateLayout(), first.img->stateArena(), second.img->stateLayout(),
                                       second.img->stateArena());
    REQUIRE(plan.ok());
    CHECK(plan.value().fresh == 0);       // the export adds no state
    CHECK(plan.value().mismatched == 0);
    CHECK(plan.value().matched == second.img->stateLayout().entries.size());
    eng::executeTransfer(plan.value(), first.img->stateArena(), second.img->stateArena());
    second.t = first.t;
    const std::vector<float> rest = second.run(tail, kFrames - kSplit, 256);
    got.insert(got.end(), rest.begin(), rest.end());
    CHECK(sameBits(got, want));
  }
  // Another factor: the converters and the phasor keep their keys but not their tags: fresh state.
  Patch c8 = a;
  c8.region(1).factor = 8;
  const Compiled cc = compileP(c8, &ca.sched.state);
  auto imgA = build(ca.sched, Backend::Interp), imgC = build(cc.sched, Backend::Interp);
  auto plan = eng::buildTransferPlan(imgA->stateLayout(), imgA->stateArena(), imgC->stateLayout(), imgC->stateArena());
  REQUIRE(plan.ok());
  CHECK(plan.value().matched == 0);
  CHECK(plan.value().mismatched == cc.sched.state.entries.size());
}
