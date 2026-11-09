// EQ.2: graph::LinearResponse (pq/graph/response.h) against the interpreter: the analytic H of small linear circuits
// equals the DFT of their interpreted impulse response; nonlinear paths are flagged; re-evaluation after a param change
// matches a fresh prepare; evaluate() does not allocate; the 32-section chain at 1024 points is timed (perf).
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/svf_design.h"
#include "pq/engine/interpreter.h"
#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/response.h"
#include "pq/graph/schedule.h"

using namespace pq::graph;
namespace eng = pq::engine;
using Cx = std::complex<double>;

namespace {

constexpr double kSr = 48000.0;

struct Circuit {
  TypedPatch inferred;   // what the evaluator reads
  TypedPatch optimised;  // what the interpreter runs
  Schedule sched;
};

Circuit compile(const std::string& text) {
  ParseResult pr = parseDetailed(text, "response.pqp");
  if (!pr.ok()) FAIL(formatDiagnostic(text, pr.diagnostics.front()));
  PatchLibrary lib;
  lib.add(pr.file);
  InferOptions io;
  io.library = &lib;
  io.voiceContext = false;
  auto inf = infer(pr.file.patches[0], io);
  if (!inf.ok()) FAIL(inf.error().message);
  Circuit c;
  c.inferred = *inf;
  OptimiseOptions oo;
  oo.sampleRate = kSr;
  auto opt = optimise(std::move(*inf), oo);
  if (!opt.ok()) FAIL(opt.error().message);
  c.optimised = std::move(*opt);
  ScheduleOptions so;
  so.sampleRate = kSr;
  auto s = schedule(c.optimised, so);
  if (!s.ok()) FAIL(s.error().message);
  c.sched = std::move(*s);
  return c;
}

NodeIndex named(const Patch& p, std::string_view name) {
  const auto n = p.findByName(name);
  REQUIRE_MESSAGE(n.has_value(), std::string(name));
  return *n;
}

// The interpreted response of `c` from root `in` `inName` channel `inCh` to every channel of root `out` `outName`:
// `frames` samples after an impulse (after a 4096-sample warm-up of silence, so smoothed gains and param ramps have
// settled). Params by name.
std::vector<std::vector<double>> impulse(const Circuit& c, std::string_view inName, uint16_t inCh, std::string_view outName,
                                         uint32_t frames, const std::vector<std::pair<std::string, float>>& params = {}) {
  eng::InterpretOptions io;
  io.maxBlockFrames = 256;
  io.maxVoiceGroups = 1;
  auto built = eng::buildInterpreted(c.sched, io);
  REQUIRE_MESSAGE(built.ok(), (built.ok() ? std::string() : built.error().message));
  eng::ProgramImage& img = **built;
  const eng::ImageInterface& face = img.io();
  const Patch& sp = c.sched.rewritten ? c.sched.rewritten->patch : c.optimised.patch;
  constexpr uint32_t B = 256;
  std::vector<std::vector<float>> in(face.globalIn.size(), std::vector<float>(B)), out(face.globalOut.size(), std::vector<float>(B));
  std::vector<const float*> inP;
  std::vector<float*> outP;
  for (auto& b : in) inP.push_back(b.data());
  for (auto& b : out) outP.push_back(b.data());
  std::vector<float> pv(face.params.size());
  for (size_t i = 0; i < pv.size(); ++i) {
    pv[i] = face.params[i].defaultValue;
    for (const auto& [name, v] : params)
      if (sp.nodeName(face.params[i].node) == name) pv[i] = v;
  }
  int inSlot = -1;
  for (size_t s = 0; s < face.globalIn.size(); ++s)
    if (sp.nodeName(face.globalIn[s].node) == inName && face.globalIn[s].channel == inCh) inSlot = static_cast<int>(s);
  REQUIRE(inSlot >= 0);
  std::vector<std::vector<double>> res;
  std::vector<uint32_t> outSlots;  // by channel
  for (size_t s = 0; s < face.globalOut.size(); ++s)
    if (sp.nodeName(face.globalOut[s].node) == outName) {
      const uint16_t ch = face.globalOut[s].channel;
      if (outSlots.size() <= ch) outSlots.resize(ch + 1u, ~0u);
      outSlots[ch] = static_cast<uint32_t>(s);
    }
  REQUIRE_FALSE(outSlots.empty());
  for (uint32_t s : outSlots) REQUIRE(s != ~0u);
  res.resize(outSlots.size());
  constexpr uint32_t kWarm = 4096;
  for (uint32_t t0 = 0; t0 < kWarm + frames; t0 += B) {
    for (size_t s = 0; s < in.size(); ++s) std::fill(in[s].begin(), in[s].end(), 0.0f);
    if (t0 == kWarm) in[inSlot][0] = 1.0f;
    eng::ProcessContext ctx;
    ctx.frames = B;
    ctx.sampleRate = kSr;
    ctx.sampleTime = t0;
    ctx.in = inP.data();
    ctx.numIn = static_cast<uint32_t>(inP.size());
    ctx.out = outP.data();
    ctx.numOut = static_cast<uint32_t>(outP.size());
    ctx.params = pv.data();
    ctx.numParams = static_cast<uint32_t>(pv.size());
    {
      pq::DenormalGuard dg;
      img.process(ctx);
    }
    if (t0 < kWarm) continue;
    for (size_t k = 0; k < outSlots.size(); ++k)
      for (uint32_t f = 0; f < B && res[k].size() < frames; ++f) res[k].push_back(out[outSlots[k]][f]);
  }
  return res;
}

Cx dft(const std::vector<double>& h, double hz) {
  const double w = 2.0 * std::numbers::pi * hz / kSr;
  Cx acc{};
  for (size_t n = 0; n < h.size(); ++n) acc += h[n] * std::polar(1.0, -w * static_cast<double>(n));
  return acc;
}

std::vector<double> testFrequencies() {
  std::vector<double> hz;
  for (int k = 0; k < 24; ++k) hz.push_back(20.0 * std::pow(1000.0, k / 23.0) * 0.999);  // 20 Hz … ~20 kHz
  hz.push_back(kSr * 0.5 * 0.97);
  return hz;
}

// 0.01 dB / 0.1° where the measured |H| is above −60 dB, else |ΔH| ≤ 1e-4.
void checkClose(Cx analytic, Cx measured, const std::string& where) {
  INFO(where << ": analytic " << analytic << " measured " << measured);
  REQUIRE(std::isfinite(analytic.real()));
  REQUIRE(std::isfinite(analytic.imag()));
  if (std::abs(measured) > 1e-3) {
    const double db = 20.0 * std::log10(std::abs(analytic) / std::abs(measured));
    const double deg = std::arg(analytic / measured) * 180.0 / std::numbers::pi;
    CHECK(std::fabs(db) <= 0.01);
    CHECK(std::fabs(deg) <= 0.1);
  } else {
    CHECK(std::abs(analytic - measured) <= 1e-4);
  }
}

ResponseEnd inEnd(const Circuit& c, std::string_view name) { return {named(c.inferred.patch, name), 0, false}; }
ResponseEnd outEnd(const Circuit& c, std::string_view name) { return {named(c.inferred.patch, name), 0, true}; }

// Analytic vs measured for every (in channel, out channel) pair.
void compareAll(const Circuit& c, ResponseKind expect = ResponseKind::Exact,
                const std::vector<std::pair<std::string, float>>& params = {}) {
  LinearResponse r;
  r.prepare(c.inferred, inEnd(c, "x"), outEnd(c, "y"));
  for (uint32_t i = 0; i < r.paramNodes().size(); ++i)
    for (const auto& [name, v] : params)
      if (c.inferred.patch.nodeName(r.paramNodes()[i]) == name) r.setParam(i, v);
  const std::vector<double> hz = testFrequencies();
  std::vector<Cx> h(hz.size() * r.sourceChannels() * r.sinkChannels());
  REQUIRE(r.evaluate(hz, kSr, h) == expect);
  for (uint16_t i = 0; i < r.sourceChannels(); ++i) {
    const auto meas = impulse(c, "x", i, "y", 16384, params);
    REQUIRE(meas.size() == r.sinkChannels());
    for (uint16_t j = 0; j < r.sinkChannels(); ++j)
      for (size_t k = 0; k < hz.size(); ++k)
        checkClose(h[(k * r.sourceChannels() + i) * r.sinkChannels() + j], dft(meas[j], hz[k]),
                   "ch " + std::to_string(i) + "->" + std::to_string(j) + " at " + std::to_string(hz[k]) + " Hz");
  }
}

// A random linear circuit of atom blocks (text): sums, differences, scalings by literals and params, z1, the three
// delay lines (fractional delays, seconds and smp), lerp, a settled gain, a literal mux, and feedback loops through z1
// and through a delay line (gains below 1, so the impulse response decays within the measured window).
std::string randomCircuit(std::mt19937& rng, std::vector<std::pair<std::string, float>>& params) {
  std::uniform_real_distribution<double> u(-0.9, 0.9);
  std::uniform_int_distribution<int> op(0, 11);
  auto lit = [&] {
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", u(rng));
    return std::string(b);
  };
  std::string t = "patch R {\n  in x : audio global mono\n";
  std::vector<std::string> sig{"x"};
  std::string body;
  auto pick = [&] { return sig[std::uniform_int_distribution<size_t>(0, sig.size() - 1)(rng)]; };
  const int steps = std::uniform_int_distribution<int>(4, 9)(rng);
  for (int k = 0; k < steps; ++k) {
    const std::string n = "s" + std::to_string(k);
    const std::string a = pick(), b = pick();
    switch (op(rng)) {
      case 0: body += "  " + n + " = " + a + " + " + b + "\n"; break;
      case 1: body += "  " + n + " = " + a + " - " + lit() + " * " + b + "\n"; break;
      case 2: {
        const std::string p = "p" + std::to_string(k);
        const float v = static_cast<float>(u(rng));
        t += "  param " + p + " = 0.5 [-2..2]\n";
        params.push_back({p, v});
        body += "  " + n + " = " + a + " * " + p + "\n";
        break;
      }
      case 3: body += "  " + n + " = z1(" + a + ") * " + lit() + " + " + b + "\n"; break;
      case 4: {
        char d[32];
        std::snprintf(d, sizeof d, "%.3fsmp", 1.0 + 9.0 * std::fabs(u(rng)));
        body += "  " + n + " = delay(" + a + ", " + d + ", 32smp)\n";
        break;
      }
      case 5: {
        char d[32];
        std::snprintf(d, sizeof d, "%.7fs", (2.0 + 9.0 * std::fabs(u(rng))) / kSr);
        body += "  " + n + " = delay_cubic(" + a + ", " + d + ", 0.001s) + " + b + "\n";
        break;
      }
      case 6: {
        char d[32];
        std::snprintf(d, sizeof d, "%.3fsmp", 1.5 + 9.0 * std::fabs(u(rng)));
        body += "  " + n + " = delay_allpass(" + a + ", " + d + ", 32smp)\n";
        break;
      }
      case 7: body += "  " + n + " = lerp(" + a + ", " + b + ", " + lit() + ")\n"; break;
      case 8: body += "  " + n + " = gain(" + a + ", " + lit() + ", 0.002, 1)\n"; break;
      case 9: body += "  " + n + " = mux(1, " + a + ", " + b + ")\n"; break;
      case 10: {  // a one-pole loop through z1
        const std::string f = "f" + std::to_string(k);
        body += "  " + f + " = " + a + " + " + lit() + " * z1(" + f + ")\n  " + n + " = " + f + "\n";
        break;
      }
      default: {  // a two-unknown loop: a comb through a delay line with an FIR tap in it
        const std::string f = "f" + std::to_string(k), g = "g" + std::to_string(k);
        char c1[32];
        std::snprintf(c1, sizeof c1, "%.4f", 0.6 * u(rng));
        body += "  " + f + " = " + a + " + " + c1 + " * delay(" + g + ", 5.25smp, 32smp)\n  " + g + " = " + f +
                " * 0.5 + z1(" + f + ") * 0.4\n  " + n + " = " + g + "\n";
        break;
      }
    }
    sig.push_back(n);
  }
  t += body + "  out y : audio\n  y = " + sig.back() + " + 0.25 * " + sig[sig.size() - 2] + "\n}\n";
  return t;
}

}  // namespace

TEST_CASE("linear response: random linear circuits of atom blocks (feedback through z1 and delay lines included) match "
          "the DFT of their interpreted impulse response within 0.01 dB / 0.1 degrees") {
  std::mt19937 rng(20261107u);
  for (int i = 0; i < 40; ++i) {
    std::vector<std::pair<std::string, float>> params;
    const std::string text = randomCircuit(rng, params);
    INFO(text);
    const Circuit c = compile(text);
    compareAll(c, ResponseKind::Exact, params);
  }
}

TEST_CASE("linear response: the svf atom (EQ.1) — every mode, both prewarps, stages 1 / 2 / 8 / 17, a param cutoff and "
          "gain — matches the DFT of its interpreted impulse response within 0.01 dB / 0.1 degrees") {
  for (int mode = 0; mode < 8; ++mode)
    for (int prewarp = 0; prewarp < 2; ++prewarp)
      for (int stages : {1, 2, 8, 17}) {
        if ((mode == 3 || mode == 4) && stages > 1) continue;  // notch, bell: one section
        char text[512];
        std::snprintf(text, sizeof text, R"(patch F {
  in x : audio global mono
  param c = 1000 [20..20000]
  param gn = 1 [0..4]
  out y : audio
  y = svf(x, cutoff: c, q: 1.2, gain: gn, mode: %d, stages: %d, prewarp: %d) + 0.5 * svf(x, cutoff: 7000, q: 0.8, gain: 0.5, mode: %d, stages: 2, prewarp: %d)
})", mode, stages, prewarp, mode, prewarp);
        INFO(text);
        const Circuit c = compile(text);
        compareAll(c, ResponseKind::Exact, {{"c", 1500.0f}, {"gn", 2.5f}});
      }
}

TEST_CASE("linear response: an M/S matrix (channel / pack) with a different filter on mid and side matches per "
          "channel pair") {
  const Circuit c = compile(R"(patch MS {
  in x : audio global stereo
  param k = 0.4 [-1..1]
  m = (x[0] + x[1]) * 0.5
  s = (x[0] - x[1]) * 0.5
  ma = m + k * z1(ma)
  sa = s - 0.3 * delay(s, 3.5smp, 16smp)
  out y : audio
  y = (ma + sa, ma - sa)
})");
  compareAll(c, ResponseKind::Exact, {{"k", 0.7f}});
  // A gain per channel (a stereo gain value) is element-wise.
  const Circuit g = compile(R"(patch G {
  in x : audio global stereo
  out y : audio
  y = gain(x, (0.5, -0.25), 0.002, 1) + z1(x)
})");
  compareAll(g);
}

TEST_CASE("linear response: tanh on the path is Approximate (its slope at 0), mul of two signals is Unavailable, an "
          "unrelated signal added is ignored, a sink the source does not reach is 0") {
  const std::vector<double> hz{100.0, 1000.0, 10000.0};
  std::vector<Cx> h;
  {
    const Circuit c = compile("patch T {\n  in x : audio global mono\n  out y : audio\n  y = tanh(x * 0.5) + z1(x)\n}\n");
    REQUIRE(linearResponse(c.inferred, inEnd(c, "x"), outEnd(c, "y"), hz, kSr, h) == ResponseKind::Approximate);
    for (size_t k = 0; k < hz.size(); ++k) {
      const Cx want = 0.5 + std::polar(1.0, -2.0 * std::numbers::pi * hz[k] / kSr);
      CHECK(std::abs(h[k] - want) < 1e-9);
    }
  }
  {
    const Circuit c = compile("patch M {\n  in x : audio global mono\n  out y : audio\n  y = x * z1(x) + x\n}\n");
    CHECK(linearResponse(c.inferred, inEnd(c, "x"), outEnd(c, "y"), hz, kSr, h) == ResponseKind::Unavailable);
    CHECK(std::isnan(h[0].real()));
  }
  {  // mul by an audio signal that does not depend on the source (an LFO): time-varying, Unavailable
    const Circuit c = compile("patch L {\n  in x : audio global mono\n  out y : audio\n  y = x * sine(phasor(2Hz))\n}\n");
    CHECK(linearResponse(c.inferred, inEnd(c, "x"), outEnd(c, "y"), hz, kSr, h) == ResponseKind::Unavailable);
  }
  {  // noise added (not the source's): superposition, exact
    const Circuit c = compile("patch N {\n  in x : audio global mono\n  out y : audio\n  y = x * 2 + noise()\n  out z : audio\n  z = noise()\n}\n");
    REQUIRE(linearResponse(c.inferred, inEnd(c, "x"), outEnd(c, "y"), hz, kSr, h) == ResponseKind::Exact);
    CHECK(std::abs(h[1] - Cx{2.0, 0.0}) < 1e-12);
    REQUIRE(linearResponse(c.inferred, inEnd(c, "x"), outEnd(c, "z"), hz, kSr, h) == ResponseKind::Exact);
    CHECK(std::abs(h[1]) == 0.0);
  }
}

TEST_CASE("linear response: re-evaluation after a param change equals a fresh prepare with that value; control math "
          "on params (sr, exp, a coefficient) follows; evaluate() allocates nothing") {
  const Circuit c = compile(R"(patch P {
  in x : audio global mono
  param cutoff = 1000 [20..20000]
  param fb = 0.2 [0..0.9]
  a = exp(-6.283185307179586 * cutoff / sr)
  lp = x * (1 - a) + a * z1(lp)
  y1 = lp + fb * delay(y1, 0.0005s, 0.01s)
  out y : audio
  y = y1
})");
  const std::vector<double> hz = testFrequencies();
  LinearResponse live;
  live.prepare(c.inferred, inEnd(c, "x"), outEnd(c, "y"));
  REQUIRE(live.paramNodes().size() == 2);
  std::vector<Cx> a(hz.size()), b(hz.size());
  REQUIRE(live.evaluate(hz, kSr, a) == ResponseKind::Exact);
  uint32_t cut = 0;
  for (uint32_t i = 0; i < live.paramNodes().size(); ++i)
    if (c.inferred.patch.nodeName(live.paramNodes()[i]) == "cutoff") cut = i;
  live.setParam(cut, 5000.0f);
  CHECK(pq::alloc_trap::countRtViolations([&] { (void)live.evaluate(hz, kSr, a); }) == 0);
  std::vector<Cx> fresh;
  const std::pair<NodeIndex, float> pv[] = {{live.paramNodes()[cut], 5000.0f}};
  REQUIRE(linearResponse(c.inferred, inEnd(c, "x"), outEnd(c, "y"), hz, kSr, fresh, pv) == ResponseKind::Exact);
  for (size_t k = 0; k < hz.size(); ++k) CHECK(a[k] == fresh[k]);
  // And both are the circuit's (measured with cutoff 5 kHz).
  const auto meas = impulse(c, "x", 0, "y", 16384, {{"cutoff", 5000.0f}});
  for (size_t k = 0; k < hz.size(); ++k) checkClose(a[k], dft(meas[0], hz[k]), std::to_string(hz[k]) + " Hz");
}

TEST_CASE("linear response (TX.mod-bands): a control an LFO (or the input itself) modulates is Unavailable unless it is a "
          "set port; held at a value, H equals the circuit with the modulation frozen there (measured)") {
  // A band whose cutoff an LFO sweeps, and one an envelope of the input sweeps (signal-dependent: cut too).
  const Circuit c = compile(R"(patch D {
  in x : audio global mono
  param base = 1000 [20..20000]
  fc = clip(base + 600 * sine(phasor(2Hz)), 20, 20000)
  fe = clip(2000 + 3000 * abs(x), 20, 20000)
  out y : audio
  y = svf(x, cutoff: fc, q: 2, gain: 2, mode: 4) + 0.5 * svf(x, cutoff: fe, q: 0.7, mode: 0, stages: 2)
})");
  // The same circuit with both controls as params: what a fixed modulator value plays.
  const Circuit ref = compile(R"(patch R {
  in x : audio global mono
  param fc = 1000 [20..20000]
  param fe = 2000 [20..20000]
  out y : audio
  y = svf(x, cutoff: fc, q: 2, gain: 2, mode: 4) + 0.5 * svf(x, cutoff: fe, q: 0.7, mode: 0, stages: 2)
})");
  const std::vector<double> hz = testFrequencies();
  std::vector<Cx> h(hz.size());
  {
    LinearResponse plain;
    plain.prepare(c.inferred, inEnd(c, "x"), outEnd(c, "y"));
    CHECK(plain.evaluate(hz, kSr, h) == ResponseKind::Unavailable);
  }
  const Patch& p = c.inferred.patch;
  const uint32_t ports[] = {p.flatOutput(named(p, "fc"), 0), p.flatOutput(named(p, "fe"), 0)};
  LinearResponse r;
  r.prepare(c.inferred, inEnd(c, "x"), outEnd(c, "y"), ports);
  REQUIRE(r.setPorts().size() == 2);
  CHECK(r.paramNodes().empty());  // (`base` is behind the cut)
  CHECK(r.evaluate(hz, kSr, h) == ResponseKind::Unavailable);  // no value given yet: unknown
  for (const auto& [vc, ve] : {std::pair{1450.0f, 4200.0f}, std::pair{520.0f, 2600.0f}}) {
    for (uint32_t i = 0; i < 2; ++i) r.setValue(i, r.setPorts()[i] == ports[0] ? vc : ve);
    CHECK(pq::alloc_trap::countRtViolations([&] { (void)r.evaluate(hz, kSr, h); }) == 0);
    REQUIRE(r.evaluate(hz, kSr, h) == ResponseKind::Exact);
    const auto meas = impulse(ref, "x", 0, "y", 16384, {{"fc", vc}, {"fe", ve}});
    for (size_t k = 0; k < hz.size(); ++k) checkClose(h[k], dft(meas[0], hz[k]), std::to_string(hz[k]) + " Hz, fc " + std::to_string(vc));
  }
}

TEST_CASE("linear response (perf): a 32-section feed-forward chain at 1024 points evaluates well under 1 ms" *
          doctest::test_suite("perf")) {
  std::string t = "patch Chain {\n  in x : audio global mono\n";
  std::string prev = "x";
  for (int k = 0; k < 32; ++k) {
    const std::string s = std::to_string(k);
    t += "  param a" + s + " = 0.5 [-2..2]\n  param b" + s + " = 0.3 [-2..2]\n  param c" + s + " = 0.1 [-2..2]\n";
    t += "  s" + s + " = " + prev + " * a" + s + " + z1(" + prev + ") * b" + s + " + z1(z1(" + prev + ")) * c" + s + "\n";
    prev = "s" + s;
  }
  t += "  out y : audio\n  y = " + prev + "\n}\n";
  const Circuit c = compile(t);
  std::vector<double> hz(1024);
  for (size_t k = 0; k < hz.size(); ++k) hz[k] = 20.0 * std::pow(1000.0, static_cast<double>(k) / 1023.0);
  LinearResponse r;
  r.prepare(c.inferred, inEnd(c, "x"), outEnd(c, "y"));
  std::vector<Cx> h(hz.size());
  REQUIRE(r.evaluate(hz, kSr, h) == ResponseKind::Exact);
  double best = 1e9;
  for (int rep = 0; rep < 20; ++rep) {
    r.setParam(0, 0.5f + 0.01f * static_cast<float>(rep));  // a drag: one param moves
    const auto t0 = std::chrono::steady_clock::now();
    (void)r.evaluate(hz, kSr, h);
    best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  MESSAGE("32-section chain (" << r.paramNodes().size() << " params), 1024 points: " << best << " ms per evaluate");
#ifdef PQ_OPTIMIZED
  CHECK(best < 1.0);
#endif
}

// ---- EQ.1b: the svf rule designs once per evaluation ------------------------------------------------------------------
namespace {

// 32 bands in series (an EQ's chain), band k a param cutoff c<k> and gain g<k>, mode and stages consts.
struct Band {
  float cutoff, q, gain;
  int mode, stages;
};
std::vector<Band> eqBands() {
  std::vector<Band> b;
  // Values the patch text spells exactly (whole Hz, binary fractions), so its params are these floats.
  constexpr float kGains[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f};
  for (int k = 0; k < 32; ++k)
    b.push_back({std::round(40.0f * std::pow(1.2f, static_cast<float>(k))), 0.5f + 0.125f * static_cast<float>(k % 7),
                 kGains[k % 9], k % 8, k % 8 == 3 || k % 8 == 4 ? 1 : 1 + k % 4});
  return b;
}
std::string eqText(const std::vector<Band>& bands) {
  std::string t = "patch Eq {\n  in x : audio global mono\n";
  std::string prev = "x";
  for (size_t k = 0; k < bands.size(); ++k) {
    const Band& b = bands[k];
    const std::string s = std::to_string(k);
    t += "  param c" + s + " = " + std::to_string(b.cutoff) + " [20..20000]\n";
    t += "  param g" + s + " = " + std::to_string(b.gain) + " [0..4]\n";
    t += "  b" + s + " = svf(" + prev + ", cutoff: c" + s + ", q: " + std::to_string(b.q) + ", gain: g" + s +
         ", mode: " + std::to_string(b.mode) + ", stages: " + std::to_string(b.stages) + ", prewarp: 0)\n";
    prev = "b" + s;
  }
  return t + "  out y : audio\n  y = " + prev + "\n}\n";
}
// The product of every band's svfResponse (what the rule computed per frequency before EQ.1b, a design each time).
Cx bandsProduct(const std::vector<Band>& bands, const std::vector<float>& cutoffs, double hz) {
  Cx h(1.0, 0.0);
  for (size_t k = 0; k < bands.size(); ++k) {
    const Band& b = bands[k];
    const pq::dsp::SvfParams p = pq::dsp::svfParamsFromInputs(cutoffs[k], b.q, b.gain, static_cast<float>(b.mode),
                                                              static_cast<float>(b.stages), 0.0f, kSr);
    h *= pq::dsp::svfResponse(p, 2.0 * std::numbers::pi * hz / kSr);
  }
  return h;
}
std::vector<double> logHz(size_t n) {
  std::vector<double> hz(n);
  for (size_t k = 0; k < n; ++k) hz[k] = 20.0 * std::pow(1000.0, static_cast<double>(k) / static_cast<double>(n - 1));
  return hz;
}

}  // namespace

TEST_CASE("linear response (EQ.1b): a 32-band svf chain is the product of its bands' svfResponse, before and after a "
          "param moves; evaluate() allocation-free") {
  const std::vector<Band> bands = eqBands();
  const Circuit c = compile(eqText(bands));
  LinearResponse r;
  r.prepare(c.inferred, inEnd(c, "x"), outEnd(c, "y"));
  const std::vector<double> hz = logHz(200);
  std::vector<Cx> h(hz.size());
  std::vector<float> cutoffs;
  for (const Band& b : bands) cutoffs.push_back(b.cutoff);
  auto check = [&](const char* what) {
    INFO(what);
    for (size_t k = 0; k < hz.size(); ++k) {
      const Cx want = bandsProduct(bands, cutoffs, hz[k]);
      CHECK(std::abs(h[k] - want) <= 1e-12 * std::abs(want) + 1e-300);
    }
  };
  REQUIRE(r.evaluate(hz, kSr, h) == ResponseKind::Exact);
  check("params at their defaults");
  // A drag on band 7's cutoff: the cache is refreshed by the next evaluation (and only that band's design changes).
  uint32_t c7 = 0;
  for (uint32_t i = 0; i < r.paramNodes().size(); ++i)
    if (c.inferred.patch.nodeName(r.paramNodes()[i]) == "c7") c7 = i;
  for (float v : {300.0f, 1234.5f, 8000.0f}) {
    r.setParam(c7, v);
    cutoffs[7] = v;
    uint64_t violations = pq::alloc_trap::countRtViolations([&] { (void)r.evaluate(hz, kSr, h); });
    CHECK(violations == 0);
    check("band 7 moved");
  }
}

TEST_CASE("linear response (perf, EQ.1b): a 32-band svf chain at 1024 points: one design per band per evaluation" *
          doctest::test_suite("perf")) {
  const std::vector<Band> bands = eqBands();
  const Circuit c = compile(eqText(bands));
  LinearResponse r;
  r.prepare(c.inferred, inEnd(c, "x"), outEnd(c, "y"));
  const std::vector<double> hz = logHz(1024);
  std::vector<Cx> h(hz.size());
  REQUIRE(r.evaluate(hz, kSr, h) == ResponseKind::Exact);
  std::vector<float> cutoffs;
  for (const Band& b : bands) cutoffs.push_back(b.cutoff);
  double best = 1e9, bestPerPoint = 1e9;
  Cx sink{};
  for (int rep = 0; rep < 10; ++rep) {
    r.setParam(0, 40.0f + static_cast<float>(rep));  // a drag: one param moves
    auto t0 = std::chrono::steady_clock::now();
    (void)r.evaluate(hz, kSr, h);
    best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    // What the rule cost before EQ.1b: every band designed again at every point (its rule work only).
    t0 = std::chrono::steady_clock::now();
    for (double f : hz) sink += bandsProduct(bands, cutoffs, f);
    bestPerPoint = std::min(bestPerPoint, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  MESSAGE("32-band svf chain, 1024 points: evaluate " << best << " ms (a design per band); a design per band and point "
          "(the pre-EQ.1b rule's work alone) " << bestPerPoint << " ms" << (sink == Cx{} ? "" : ""));
  // Before EQ.1b (a design per band and point inside evaluate): 12.0 ms in the check preset; after: 1.2 ms.
#ifdef PQ_OPTIMIZED
  CHECK(best < 3.0);
  CHECK(best < 0.25 * bestPerPoint);
#endif
}

// TX.group-oversample: the response through oversample regions (pq/graph/response.h "Oversample regions"): the inner
// nodes at omega / n, sr and durations at n·sr, the half-band converters' cascades on the edges. Up to 20 kHz (the
// half-bands' passband; above it the transition band's images make the region time-varying).
TEST_CASE("linear response (TX.group-oversample): through oversample regions at x2 / x4 / x8 (an sr-dependent one-pole "
          "loop, an svf bell, a fractional delay inside; a second region in series) matches the DFT of the interpreted "
          "impulse response within 0.01 dB / 0.1 degrees to 20 kHz") {
  std::vector<double> hz;
  for (int k = 0; k < 24; ++k) hz.push_back(20.0 * std::pow(1000.0, k / 23.0) * 0.999);  // 20 Hz … ~20 kHz
  for (const int n : {2, 4, 8}) {
    char text[1024];
    std::snprintf(text, sizeof text, R"(patch O {
  in x : audio global mono
  param c = 1000 [20..20000]
  out y : audio
  oversample(%d) {
    a = exp(-6.283185307179586 * 3000 / sr)
    f = x * (1 - a) + a * z1(f)
    b = svf(f, cutoff: c, q: 1.2, gain: 2, mode: 4, stages: 1, prewarp: 0) + 0.3 * delay(x, 0.00021s, 0.001s)
  }
  oversample(2) {
    e = b * 0.5 + z1(b) * 0.25
  }
  y = e + 0.1 * x
})", n);
    INFO(text);
    const Circuit c = compile(text);
    REQUIRE(c.inferred.oversample.size() == 2);
    LinearResponse r;
    r.prepare(c.inferred, inEnd(c, "x"), outEnd(c, "y"));
    for (uint32_t i = 0; i < r.paramNodes().size(); ++i) r.setParam(i, 9000.0f);
    std::vector<Cx> h(hz.size());
    REQUIRE(r.evaluate(hz, kSr, h) == ResponseKind::Exact);
    const auto meas = impulse(c, "x", 0, "y", 16384, {{"c", 9000.0f}});
    for (size_t k = 0; k < hz.size(); ++k) checkClose(h[k], dft(meas[0], hz[k]), "x" + std::to_string(n) + " at " + std::to_string(hz[k]) + " Hz");
    CHECK(pq::alloc_trap::countRtViolations([&] { (void)r.evaluate(hz, kSr, h); }) == 0);
  }
}
