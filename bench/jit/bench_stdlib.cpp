// T1b.3: the stock subcircuits of stdlib/ compiled by the JIT (and the interpreter, for reference) against their
// hand-written C++ implementations (tests/stdlib/stdlib_reference.h: the same float32 arithmetic, FMAs where the
// optimiser fuses, state in locals, SVML for the exact transcendentals; 8 voices = one __m256 per frame). TASKS
// T1b.3: the JIT'd filters within ~10 % of the hand-written cost. Release / ship build.
//
// Every case processes 64-frame blocks: mono (a global patch, ns/sample) and 8 voices (one voice group,
// ns/voice-sample). Params are param-rate (coefficients once per block, as the compiler hoists them) unless the
// name says "audio-rate", where the control is a block buffer and the coefficients run per sample on both sides.
// JIT and C++ are measured alternately, three rounds each; the table prints each one's best median and the ratio.
//   pq_bench_stdlib [--block N] [filter]
#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../tests/stdlib/stdlib_harness.h"
#include "../../tests/stdlib/stdlib_reference.h"
#include "pq/core/denormal_guard.h"
#include "pq_bench.h"

using namespace stdref;
namespace st = stdlib_test;
namespace eng = pq::engine;

namespace {

uint32_t kBlock = 64;  // --block N (≤ 1024)
constexpr float kSr = 48000.0f;
std::string gFilter;

// Inputs every case reads: a noise-like audio signal, a modulated cutoff (Hz), a gate, a delay time (s).
struct Signals {
  std::vector<float> x, cutoff, gate, time;  // kBlock · 8 each (frame-major lanes; mono uses the first kBlock)
  Signals() {
    for (uint32_t i = 0; i < kBlock * 8; ++i) {
      x.push_back(0.5f * std::sin(0.37f * static_cast<float>(i)) + 0.3f * std::sin(1.91f * static_cast<float>(i)));
      cutoff.push_back(800.0f + 600.0f * std::sin(0.05f * static_cast<float>(i)));
      gate.push_back((i / 8) % 50 < 40 ? 1.0f : 0.0f);
      time.push_back(0.004f + 0.001f * std::sin(0.03f * static_cast<float>(i)));
    }
  }
};
const Signals& sig() {
  static const Signals s;
  return s;
}

// A compiled image driven by 64-frame blocks: root `in`s by ordinal from `inputs`, 8 voices when `voices`.
class Rig {
public:
  Rig(const st::Program& p, st::Backend backend, bool voices, std::vector<const std::vector<float>*> inputs, const std::map<std::string, double>& params)
      : voices_(voices) {
    auto built = st::build(p, backend, kBlock, 1);
    if (!built) {
      std::fprintf(stderr, "%s\n", built.error().message.c_str());
      std::exit(2);
    }
    img_ = std::move(built.value());
    const eng::ImageInterface& io = img_->io();
    auto gateBool = [&](uint32_t ord) {
      uint32_t k = 0;
      for (pq::graph::NodeIndex n = 0; n < p.typed.patch.nodeCount(); ++n)
        if (p.typed.patch.node(n).kind == pq::graph::NodeKind::Input && k++ == ord)
          return p.typed.outputType(n, 0).kind == pq::graph::Kind::Bool;
      return false;
    };
    auto fill = [&](std::vector<float>& dst, uint32_t ord, uint32_t lanes) {
      const std::vector<float>& src = *inputs.at(ord);
      dst.resize(static_cast<size_t>(kBlock) * lanes);
      for (size_t i = 0; i < dst.size(); ++i) dst[i] = gateBool(ord) ? pq::dsp::boolMask(src[i] != 0.0f) : src[i];
    };
    gIn_.resize(io.globalIn.size());
    for (size_t s = 0; s < io.globalIn.size(); ++s) fill(gIn_[s], io.globalIn[s].ordinal, 1);
    vIn_.resize(io.voiceIn.size());
    for (size_t s = 0; s < io.voiceIn.size(); ++s) fill(vIn_[s], io.voiceIn[s].ordinal, 8);
    gOut_.assign(io.globalOut.size(), std::vector<float>(kBlock));
    vOut_.assign(io.voiceOut.size(), std::vector<float>(kBlock * 8));
    sigs_.assign(eng::kVoiceSignalCount, std::vector<float>(kBlock * 8, 0.0f));
    for (auto& b : gIn_) gInP_.push_back(b.data());
    for (auto& b : vIn_) vInP_.push_back(b.data());
    for (auto& b : gOut_) gOutP_.push_back(b.data());
    for (auto& b : vOut_) vOutP_.push_back(b.data());
    for (auto& b : sigs_) sigP_.push_back(b.data());
    params_.assign(io.params.size(), NAN);
    for (size_t i = 0; i < io.params.size(); ++i)
      if (auto it = params.find(p.typed.patch.nodeName(io.params[i].node)); it != params.end())
        params_[i] = static_cast<float>(it->second);
    gio_.laneMask = 0xFF;
    gio_.signals = sigP_.data();
    gio_.in = vInP_.data();
    gio_.numIn = static_cast<uint32_t>(vInP_.size());
    gio_.out = vOutP_.data();
    gio_.numOut = static_cast<uint32_t>(vOutP_.size());
    vt_.groups = &gio_;
    vt_.numGroups = voices ? 1 : 0;
    ctx_.frames = kBlock;
    ctx_.in = gInP_.data();
    ctx_.numIn = static_cast<uint32_t>(gInP_.size());
    ctx_.out = gOutP_.data();
    ctx_.numOut = static_cast<uint32_t>(gOutP_.size());
    ctx_.params = params_.data();
    ctx_.numParams = static_cast<uint32_t>(params_.size());
    ctx_.voices = voices ? &vt_ : nullptr;
    ctx_.transport.beatsPerFrame = 2.0 / kSr;
    ctx_.transport.playing = true;
  }
  void process() noexcept { img_->process(ctx_); }
  float last() const { return voices_ ? vOut_[0].back() : gOut_[0].back(); }

private:
  bool voices_;
  std::unique_ptr<eng::ProgramImage> img_;
  std::vector<std::vector<float>> gIn_, vIn_, gOut_, vOut_, sigs_;
  std::vector<const float*> gInP_, vInP_, sigP_;
  std::vector<float*> gOutP_, vOutP_;
  std::vector<float> params_;
  eng::VoiceGroupIO gio_;
  eng::VoiceTable vt_;
  eng::ProcessContext ctx_;
};

struct Case {
  std::string name;
  std::string text;                        // the test patch (%P = voice / global)
  std::vector<const std::vector<float>*> inputs;  // per root in ordinal
  std::map<std::string, double> params;
  std::function<void()> mono, voices;      // the C++ reference for one block (mono, 8 voices)
};

double gWorstRatio = 0.0;
// The JIT image's fixed cost per block (params, pointer table, output copies) minus a plain copy, per (voice-)sample:
// measured by the `passthrough` case (y = x), [mono, x8]. Part of every image, not of any filter in it.
double gOverhead[2] = {0.0, 0.0};

void runCase(const Case& c) {
  if (!gFilter.empty() && c.name.find(gFilter) == std::string::npos) return;
  for (const bool voice : {false, true}) {
    const std::function<void()>& ref = voice ? c.voices : c.mono;
    if (!ref) continue;
    std::string text = c.text;
    for (size_t k = 0; (k = text.find("%P", k)) != std::string::npos;) text.replace(k, 2, voice ? "voice" : "global");
    const st::Program p = st::compile(text);
    Rig jit(p, st::Backend::Jit, voice, c.inputs, c.params);
    Rig interp(p, st::Backend::Interp, voice, c.inputs, c.params);
    const uint64_t ops = voice ? kBlock * 8 : kBlock;
    pq::DenormalGuard dg;
    double bestJ = 1e30, bestR = 1e30, minJ = 1e30, minR = 1e30;
    for (int round = 0; round < 3; ++round) {
      const auto sj = pq::bench::measure([&] { jit.process(); }, ops, 9, 2, 1.5e6);
      const auto sr = pq::bench::measure(ref, ops, 9, 2, 1.5e6);
      bestJ = std::min(bestJ, sj.medianNs);
      bestR = std::min(bestR, sr.medianNs);
      minJ = std::min(minJ, sj.minNs);
      minR = std::min(minR, sr.minNs);
    }
    const auto si = pq::bench::measure([&] { interp.process(); }, ops, 9, 2, 1.5e6);
    pq::bench::doNotOptimize(jit.last());
    const char* unit = voice ? "ns/voice-sample" : "ns/sample";
    const std::string tag = c.name + (voice ? " x8" : " mono");
    pq::bench::report(("stdlib-jit/" + tag).c_str(), {bestJ, minJ}, unit);
    pq::bench::report(("stdlib-c++/" + tag).c_str(), {bestR, minR}, unit);
    pq::bench::report(("stdlib-interp/" + tag).c_str(), {si.medianNs, si.minNs}, unit);
    const double ratio = bestJ / bestR;
    if (c.name == "passthrough") gOverhead[voice] = std::max(0.0, bestJ - bestR);
    else gWorstRatio = std::max(gWorstRatio, ratio);
    std::printf("      %-28s %-5s jit %7.3f  c++ %7.3f  jit/c++ %5.2f  (excl. image overhead %5.2f)   interp %7.3f  (%zu ops)\n",
                c.name.c_str(), voice ? "x8" : "mono", bestJ, bestR, ratio, (bestJ - gOverhead[voice]) / bestR, si.medianNs,
                p.sched.ops.size());
  }
}

// ---- C++ reference blocks -----------------------------------------------------------------------------------
// Output sinks (one block); the references read sig() and write here.
std::vector<float> gOut(1024 * 8);

// Runs `tick(V x, uint32_t frame) -> V` over one block and stores the result.
template <class V, class F>
inline void block(F&& tick) {
  for (uint32_t t = 0; t < kBlock; ++t) store(gOut.data(), t, tick(load(sig().x.data(), t, V{}), t));
}

template <class V>
V ctl(const std::vector<float>& v, uint32_t t) {
  return load(v.data(), t, V{});
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--block" && i + 1 < argc) kBlock = std::min<uint32_t>(1024, static_cast<uint32_t>(std::atoi(argv[++i])));
    else gFilter = a;
  }
  pq::bench::SystemLoad load;
  std::printf("T1b.3 stdlib subcircuits: JIT vs hand-written C++ (stdlib_reference.h), block %u, 48 kHz; JIT target %s\n",
              kBlock, st::jitCompiler().hostCpu().c_str());
  const Signals& S = sig();
  const auto* X = &S.x;
  const auto* CUT = &S.cutoff;
  const auto* GATE = &S.gate;
  const auto* TIME = &S.time;

  std::vector<Case> cases;
  cases.push_back({"passthrough", R"(patch T { in x : audio %P
  out y : audio %P
  y = x })", {X}, {},
                   [] { std::copy(sig().x.begin(), sig().x.begin() + kBlock, gOut.begin()); },
                   [] { std::copy(sig().x.begin(), sig().x.begin() + kBlock * 8, gOut.begin()); }});

  // ---- onepole -----------------------------------------------------------------------------------------------
  {
    static OnePole<float> m;
    static OnePole<F8> v;
    cases.push_back({"onepole lp", R"(import "filters/onepole.pqp"
patch T { in x : audio %P
  param c = 1kHz [1Hz..20kHz]
  out y : audio %P
  y = onepole(x, cutoff: c).lp })", {X}, {{"c", 1000.0}},
                     [] { auto f = m; const float G = OnePole<float>::coef(1000.0f, kSr); block<float>([&](float x, uint32_t) { return f.tick(x, G); }); m = f; },
                     [] { auto f = v; const F8 G = OnePole<F8>::coef(F8(1000.0f), kSr); block<F8>([&](F8 x, uint32_t) { return f.tick(x, G); }); v = f; }});
    static OnePole<float> ma;
    static OnePole<F8> va;
    cases.push_back({"onepole lp audio-rate", R"(import "filters/onepole.pqp"
patch T { in x : audio %P
  in c : audio %P
  out y : audio %P
  y = onepole(x, cutoff: c).lp })", {X, CUT}, {},
                     [] { auto f = ma; block<float>([&](float x, uint32_t t) { return f.tick(x, OnePole<float>::coef(ctl<float>(sig().cutoff, t), kSr)); }); ma = f; },
                     [] { auto f = va; block<F8>([&](F8 x, uint32_t t) { return f.tick(x, OnePole<F8>::coef(ctl<F8>(sig().cutoff, t), kSr)); }); va = f; }});
  }
  // ---- svf ---------------------------------------------------------------------------------------------------
  {
    static SvfCore<float> m;
    static SvfCore<F8> v;
    cases.push_back({"svf lp", R"(import "filters/svf.pqp"
patch T { in x : audio %P
  param c = 1kHz [20Hz..20kHz]
  param q = 2 [0.1..40]
  out y : audio %P
  y = svf(x, cutoff: c, q: q).lp })", {X}, {{"c", 1000.0}, {"q", 2.0}},
                     [] {
                       auto f = m;
                       const auto c = SvfCore<float>::coef(prewarp(1000.0f, kSr), 1.0f / std::max(2.0f, 0.01f));
                       block<float>([&](float x, uint32_t) { float b, l; f.tick(x, c, b, l); return l; });
                       m = f;
                     },
                     [] {
                       auto f = v;
                       const auto c = SvfCore<F8>::coef(prewarp(F8(1000.0f), kSr), F8(1.0f) / vmax(F8(2.0f), F8(0.01f)));
                       block<F8>([&](F8 x, uint32_t) { F8 b, l; f.tick(x, c, b, l); return l; });
                       v = f;
                     }});
    static SvfCore<float> ma;
    static SvfCore<F8> va;
    cases.push_back({"svf lp audio-rate", R"(import "filters/svf.pqp"
patch T { in x : audio %P
  in c : audio %P
  param q = 2 [0.1..40]
  out y : audio %P
  y = svf(x, cutoff: c, q: q).lp })", {X, CUT}, {{"q", 2.0}},
                     [] {
                       auto f = ma;
                       const float k = 1.0f / std::max(2.0f, 0.01f);
                       block<float>([&](float x, uint32_t t) {
                         float b, l;
                         f.tick(x, SvfCore<float>::coef(prewarp(ctl<float>(sig().cutoff, t), kSr), k), b, l);
                         return l;
                       });
                       ma = f;
                     },
                     [] {
                       auto f = va;
                       const F8 k = F8(1.0f) / vmax(F8(2.0f), F8(0.01f));
                       block<F8>([&](F8 x, uint32_t t) {
                         F8 b, l;
                         f.tick(x, SvfCore<F8>::coef(prewarp(ctl<F8>(sig().cutoff, t), kSr), k), b, l);
                         return l;
                       });
                       va = f;
                     }});
  }
  // ---- biquad ------------------------------------------------------------------------------------------------
  {
    static Biquad<float> m, mp;
    static Biquad<F8> v, vp;
    cases.push_back({"biquad_lowpass", R"(import "filters/biquad.pqp"
patch T { in x : audio %P
  param c = 1kHz [20Hz..20kHz]
  param q = 0.7071 [0.1..40]
  out y : audio %P
  y = biquad_lowpass(x, cutoff: c, q: q) })", {X}, {{"c", 1000.0}, {"q", 0.7071}},
                     [] { auto f = m; const auto k = design<float>(BiquadType::Lowpass, 1000.0f, 0.7071f, 1.0f, kSr); block<float>([&](float x, uint32_t) { return f.tick(x, k); }); m = f; },
                     [] { auto f = v; const auto k = design<F8>(BiquadType::Lowpass, F8(1000.0f), F8(0.7071f), F8(1.0f), kSr); block<F8>([&](F8 x, uint32_t) { return f.tick(x, k); }); v = f; }});
    cases.push_back({"biquad_peak", R"(import "filters/biquad.pqp"
patch T { in x : audio %P
  param c = 1kHz [20Hz..20kHz]
  param q = 0.7071 [0.1..40]
  param gain = 0dB [-24dB..24dB]
  out y : audio %P
  y = biquad_peak(x, cutoff: c, q: q, gain: gain) })", {X}, {{"c", 1000.0}, {"q", 2.0}, {"gain", 2.0}},
                     [] { auto f = mp; const auto k = design<float>(BiquadType::Peak, 1000.0f, 2.0f, 2.0f, kSr); block<float>([&](float x, uint32_t) { return f.tick(x, k); }); mp = f; },
                     [] { auto f = vp; const auto k = design<F8>(BiquadType::Peak, F8(1000.0f), F8(2.0f), F8(2.0f), kSr); block<F8>([&](F8 x, uint32_t) { return f.tick(x, k); }); vp = f; }});
  }
  // ---- dcblock, smooth, slew, envfollow, adsr ------------------------------------------------------------------
  {
    static DcBlock<float> m;
    static DcBlock<F8> v;
    cases.push_back({"dcblock", R"(import "filters/dcblock.pqp"
patch T { in x : audio %P
  param c = 10Hz [1Hz..200Hz]
  out y : audio %P
  y = dcblock(x, cutoff: c) })", {X}, {{"c", 10.0}},
                     [] { auto f = m; const float R = DcBlock<float>::coef(10.0f, kSr); block<float>([&](float x, uint32_t) { return f.tick(x, R); }); m = f; },
                     [] { auto f = v; const F8 R = DcBlock<F8>::coef(F8(10.0f), kSr); block<F8>([&](F8 x, uint32_t) { return f.tick(x, R); }); v = f; }});
    static Smooth<float> sm;
    static Smooth<F8> sv;
    cases.push_back({"smooth", R"(import "control/smooth.pqp"
patch T { in x : audio %P
  param t = 10ms [0ms..10s]
  out y : audio %P
  y = smooth(x, time: t) })", {X}, {{"t", 0.01}},
                     [] { auto f = sm; const float c = Smooth<float>::coef(0.01f, kSr); block<float>([&](float x, uint32_t) { return f.tick(x, c); }); sm = f; },
                     [] { auto f = sv; const F8 c = Smooth<F8>::coef(F8(0.01f), kSr); block<F8>([&](F8 x, uint32_t) { return f.tick(x, c); }); sv = f; }});
    static Slew<float> lm;
    static Slew<F8> lv;
    cases.push_back({"slew", R"(import "control/slew.pqp"
patch T { in x : audio %P
  param r = 5ms [0ms..10s]
  param f = 20ms [0ms..10s]
  out y : audio %P
  y = slew(x, rise: r, fall: f) })", {X}, {{"r", 0.005}, {"f", 0.02}},
                     [] { auto f = lm; const float u = Slew<float>::up(0.005f, kSr), d = Slew<float>::down(0.02f, kSr); block<float>([&](float x, uint32_t) { return f.tick(x, u, d); }); lm = f; },
                     [] { auto f = lv; const F8 u = Slew<F8>::up(F8(0.005f), kSr), d = Slew<F8>::down(F8(0.02f), kSr); block<F8>([&](F8 x, uint32_t) { return f.tick(x, u, d); }); lv = f; }});
    static EnvFollow<float> em;
    static EnvFollow<F8> ev;
    cases.push_back({"envfollow peak", R"(import "dynamics/envfollow.pqp"
patch T { in x : audio %P
  param a = 1ms [0ms..1s]
  param r = 100ms [1ms..10s]
  out y : audio %P
  y = envfollow(x, attack: a, release: r) })", {X}, {{"a", 0.001}, {"r", 0.1}},
                     [] { auto f = em; const float a = EnvFollow<float>::coef(0.001f, kSr), r = EnvFollow<float>::coef(0.1f, kSr); block<float>([&](float x, uint32_t) { return f.tick(x, a, r, false); }); em = f; },
                     [] { auto f = ev; const F8 a = EnvFollow<F8>::coef(F8(0.001f), kSr), r = EnvFollow<F8>::coef(F8(0.1f), kSr); block<F8>([&](F8 x, uint32_t) { return f.tick(x, a, r, false); }); ev = f; }});
    static Adsr<float> am;
    static Adsr<F8> av;
    cases.push_back({"adsr", R"(import "envelopes/adsr.pqp"
patch T { in g : bool %P
  param a = 5ms [0ms..10s]
  param d = 200ms [1ms..10s]
  param s = 0.7 [0..1]
  param r = 300ms [1ms..30s]
  out y : audio %P
  y = adsr(g, a: a, d: d, s: s, r: r) })", {GATE}, {{"a", 0.005}, {"d", 0.2}, {"s", 0.7}, {"r", 0.3}},
                     [] {
                       auto f = am;
                       const auto c = Adsr<float>::coef(0.005f, 0.2f, 0.7f, 0.3f, kSr);
                       for (uint32_t t = 0; t < kBlock; ++t) {
                         bool done;
                         gOut[t] = f.tick(sig().gate[t] != 0.0f, c, done);
                       }
                       am = f;
                     },
                     [] {
                       auto f = av;
                       const auto c = Adsr<F8>::coef(F8(0.005f), F8(0.2f), F8(0.7f), F8(0.3f), kSr);
                       for (uint32_t t = 0; t < kBlock; ++t) {
                         M8 done;
                         store(gOut.data(), t, f.tick(loadMask(sig().gate.data(), t, M8{}), c, done));
                       }
                       av = f;
                     }});
  }
  // ---- delay-line filters --------------------------------------------------------------------------------------
  {
    static Comb<float> m(0.05f, kSr, 1);
    static Comb<F8> v(0.05f, kSr, 8);
    cases.push_back({"comb", R"(import "filters/comb.pqp"
patch T { in x : audio %P
  param t = 5ms [1ms..40ms]
  param fb = 0.8 [-0.999..0.999]
  out y : audio %P
  y = comb(x, 50ms, time: t, fb: fb) })", {X}, {{"t", 0.005}, {"fb", 0.8}},
                     [] { const float d = Comb<float>::delaySamples(0.005f, kSr); block<float>([&](float x, uint32_t) { return m.tick(x, d, 0.8f); }); },
                     [] { const F8 d = Comb<F8>::delaySamples(F8(0.005f), kSr); block<F8>([&](F8 x, uint32_t) { return v.tick(x, d, F8(0.8f)); }); }});
    static Comb<float> ma(0.05f, kSr, 1);
    static Comb<F8> va(0.05f, kSr, 8);
    cases.push_back({"comb audio-rate time", R"(import "filters/comb.pqp"
patch T { in x : audio %P
  in tm : audio %P
  param fb = 0.8 [-0.999..0.999]
  out y : audio %P
  y = comb(x, 50ms, time: tm, fb: fb) })", {X, TIME}, {{"fb", 0.8}},
                     [] { block<float>([&](float x, uint32_t t) { return ma.tick(x, Comb<float>::delaySamples(ctl<float>(sig().time, t), kSr), 0.8f); }); },
                     [] { block<F8>([&](F8 x, uint32_t t) { return va.tick(x, Comb<F8>::delaySamples(ctl<F8>(sig().time, t), kSr), F8(0.8f)); }); }});
    static CombLp<float> lm(0.05f, kSr, 1);
    static CombLp<F8> lv(0.05f, kSr, 8);
    cases.push_back({"comb_lp", R"(import "filters/comb.pqp"
patch T { in x : audio %P
  param t = 5ms [1ms..40ms]
  param fb = 0.9 [-0.999..0.999]
  param damp = 0.3 [0..1]
  out y : audio %P
  y = comb_lp(x, 50ms, time: t, fb: fb, damp: damp) })", {X}, {{"t", 0.005}, {"fb", 0.9}, {"damp", 0.3}},
                     [] { const float d = CombLp<float>::delaySamples(0.005f, kSr); block<float>([&](float x, uint32_t) { return lm.tick(x, d, 0.9f, 0.3f); }); },
                     [] { const F8 d = CombLp<F8>::delaySamples(F8(0.005f), kSr); block<F8>([&](F8 x, uint32_t) { return lv.tick(x, d, F8(0.9f), F8(0.3f)); }); }});
    static AllpassDelay<float> am(0.05f, kSr, 1);
    static AllpassDelay<F8> av(0.05f, kSr, 8);
    cases.push_back({"allpass_delay", R"(import "filters/allpass.pqp"
patch T { in x : audio %P
  param t = 5ms [1ms..40ms]
  param g = 0.6 [-0.99..0.99]
  out y : audio %P
  y = allpass_delay(x, 50ms, time: t, g: g) })", {X}, {{"t", 0.005}, {"g", 0.6}},
                     [] { const float d = AllpassDelay<float>::delaySamples(0.005f, kSr); block<float>([&](float x, uint32_t) { return am.tick(x, d, 0.6f); }); },
                     [] { const F8 d = AllpassDelay<F8>::delaySamples(F8(0.005f), kSr); block<F8>([&](F8 x, uint32_t) { return av.tick(x, d, F8(0.6f)); }); }});
  }
  // ---- lfo (mono: a global LFO is the common case) ---------------------------------------------------------------
  {
    static Lfo l;
    cases.push_back({"lfo sine", R"(import "control/lfo.pqp"
patch T {
  param rate = 3Hz [0.01Hz..40Hz]
  out y : audio %P
  y = lfo(rate: rate).sine })", {}, {{"rate", 3.0}},
                     [] { auto f = l; for (uint32_t t = 0; t < kBlock; ++t) gOut[t] = f.tick(3.0f, false, false, 1.0f, 0.0f, 0.0f, kSr).sine; l = f; },
                     {}});
  }

  for (const Case& c : cases) runCase(c);
  std::printf("worst jit/c++ ratio %.2f; system load during the run %.0f %%\n", gWorstRatio, load.busyPercent());
  return 0;
}
