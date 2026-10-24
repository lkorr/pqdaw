// T1b.3: the stock subcircuits of stdlib/ (filters, envelopes, LFO, followers) against their hand-written C++
// references (stdlib_reference.h), on the interpreter and the JIT, in global and voice contexts: impulse
// responses, noise and sine sweeps, every output, param-rate and audio-rate (including fast audio-rate) modulation
// of cutoff / Q / gain / time, block sizes that split the sub-blocked loops. Criterion (TASKS T1b.3): every sample
// within 1e-5 (absolute below 1, relative above: the resonant outputs exceed 1). Also: the damped comb's pitch, the
// svf and biquad designs against the analog responses, and
// the adsr semantics (stage times, retrigger, release from attack, sustain changes, done).
#include <doctest/doctest.h>

#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "stdlib_harness.h"
#include "stdlib_reference.h"

using namespace stdlib_test;
using stdref::BiquadType;

namespace {

constexpr float kSr = 48000.0f;
constexpr double kTol = 1e-5;
constexpr Backend kBackends[] = {Backend::Interp, Backend::Jit};
constexpr uint32_t kVoices = 11;  // two voice groups, the second one partly occupied

// ---- stimuli ---------------------------------------------------------------------------------------------------
double noise(uint64_t t, uint32_t seed) {
  uint64_t z = (t + 1) * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(seed) * 0xD1B54A32D192ED03ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return static_cast<double>(z >> 11) / 9007199254740992.0 * 2.0 - 1.0;
}
// Exponential sine sweep 20 Hz → 20 kHz over `len` samples.
double chirp(uint64_t t, uint64_t len) {
  const double f0 = 20.0, f1 = 20000.0, T = static_cast<double>(len) / kSr, k = std::log(f1 / f0);
  const double s = static_cast<double>(t) / kSr;
  return 0.8 * std::sin(2.0 * 3.141592653589793 * f0 * T / k * (std::exp(s / T * k) - 1.0));
}
// The test signal of voice v (voice 0 = the global one): an impulse, noise, then a sweep.
double signalOf(uint32_t v, uint64_t t) {
  if (t < 1024) return t == 0 ? 1.0 : 0.0;
  if (t < 3072) return 0.7 * noise(t, v);
  return chirp(t - 3072, 6144) * (1.0 - 0.05 * v);
}
constexpr uint32_t kSignalFrames = 3072 + 6144;

// A cutoff modulated at audio rate (log-sweep up and down, plus a fast 300 Hz wobble), voice-dependent.
double modCutoff(uint32_t v, uint64_t t) {
  const double s = static_cast<double>(t) / kSr;
  return 900.0 * (1.0 + 0.1 * v) * std::exp2(2.5 * std::sin(2.0 * 3.141592653589793 * 2.0 * s)) *
         (1.0 + 0.4 * std::sin(2.0 * 3.141592653589793 * 300.0 * s));
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
  for (size_t k = 0; (k = s.find(from, k)) != std::string::npos; k += to.size()) s.replace(k, from.size(), to);
  return s;
}

// %P → "voice" or "global" in a test patch.
std::string withPoly(const std::string& text, bool voice) { return replaceAll(text, "%P", voice ? "voice" : "global"); }

// err = |got − ref| / max(1, |ref|).
struct Check {
  double worst = 0.0;
  std::string where;
  void add(const std::string& what, const std::vector<float>& got, const std::vector<double>& ref) {
    REQUIRE_MESSAGE(got.size() == ref.size(), what);
    for (size_t t = 0; t < ref.size(); ++t) {
      const double e = std::isfinite(got[t]) ? std::fabs(got[t] - ref[t]) / std::max(1.0, std::fabs(ref[t])) : INFINITY;
      if (e > worst) {
        worst = e;
        where = what + " t=" + std::to_string(t) + " got " + std::to_string(got[t]) + " ref " + std::to_string(ref[t]);
      }
    }
  }
};

void expectClose(const Check& c, const std::string& label) {
  CHECK_MESSAGE(c.worst <= kTol, label << ": max error " << c.worst << " (" << c.where << ")");
  MESSAGE(label << ": max error " << c.worst);
}

using RefFn = std::function<std::map<std::string, std::vector<double>>(uint32_t voice)>;

// Runs `p` on both backends with `st` and compares every output named in the reference's map, for every voice.
void compareAll(const Program& p, const Stimulus& st, const RefFn& ref, const std::string& label) {
  const uint32_t voices = std::max<uint32_t>(st.voices, 1);
  std::vector<std::map<std::string, std::vector<double>>> refs;
  for (uint32_t v = 0; v < voices; ++v) refs.push_back(ref(st.voices ? v : 0));
  for (Backend b : kBackends) {
    const Outputs o = run(p, b, st);
    REQUIRE_MESSAGE(o.error.empty(), o.error);
    CHECK(o.rtViolations == 0);
    Check c;
    for (uint32_t v = 0; v < voices; ++v)
      for (const auto& [name, r] : refs[v]) c.add(name + " voice " + std::to_string(v), o.at(name)[v], r);
    expectClose(c, label + " [" + toText(b) + ", block " + std::to_string(st.block) + (st.voices ? ", voices]" : ", global]"));
  }
}

Stimulus stimulus(bool voice, uint32_t frames = kSignalFrames, uint32_t block = 64) {
  Stimulus st;
  st.frames = frames;
  st.block = block;
  st.voices = voice ? kVoices : 0;
  return st;
}

}  // namespace

// ================================================================================================================
TEST_CASE("stdlib onepole: lp / hp / allpass vs the reference (param and audio-rate cutoff, global and voice)") {
  for (const bool voice : {false, true}) {
    // Param cutoff, several values (one compile each: params are block-rate, set per run).
    const Program pp = compile(withPoly(R"(import "filters/onepole.pqp"
patch T {
  in    x : audio %P
  param c = 1kHz [1Hz..20kHz]
  out   lp : audio %P
  out   hp : audio %P
  out   ap : audio %P
  f  = onepole(x, cutoff: c)
  lp = f.lp
  hp = f.hp
  ap = f.allpass
})", voice));
    for (const double cutoff : {20.0, 1000.0, 15000.0}) {
      Stimulus st = stimulus(voice);
      st.input = [](uint32_t, uint32_t v, uint64_t t) { return signalOf(v, t); };
      st.params["c"] = cutoff;
      compareAll(pp, st, [&](uint32_t v) {
        stdref::OnePole<float> f;
        const float G = stdref::OnePole<float>::coef(static_cast<float>(cutoff), kSr);
        std::map<std::string, std::vector<double>> r;
        for (uint64_t t = 0; t < st.frames; ++t) {
          const float x = static_cast<float>(signalOf(v, t));
          const float lp = f.tick(x, G);
          r["lp"].push_back(lp);
          r["hp"].push_back(x - lp);
          r["ap"].push_back(lp - (x - lp));
        }
        return r;
      }, "onepole cutoff " + std::to_string(cutoff));
    }
    // Audio-rate cutoff.
    const Program pa = compile(withPoly(R"(import "filters/onepole.pqp"
patch T {
  in    x : audio %P
  in    c : audio %P
  out   lp : audio %P
  out   ap : audio %P
  f  = onepole(x, cutoff: c)
  lp = f.lp
  ap = f.allpass
})", voice));
    for (const uint32_t block : {64u, 37u}) {
      Stimulus st = stimulus(voice, kSignalFrames, block);
      st.input = [](uint32_t ord, uint32_t v, uint64_t t) { return ord == 0 ? signalOf(v, t) : modCutoff(v, t); };
      compareAll(pa, st, [&](uint32_t v) {
        stdref::OnePole<float> f;
        std::map<std::string, std::vector<double>> r;
        for (uint64_t t = 0; t < st.frames; ++t) {
          const float x = static_cast<float>(signalOf(v, t));
          const float lp = f.tick(x, stdref::OnePole<float>::coef(static_cast<float>(modCutoff(v, t)), kSr));
          r["lp"].push_back(lp);
          r["ap"].push_back(lp - (x - lp));
        }
        return r;
      }, "onepole audio-rate cutoff");
    }
  }
}

TEST_CASE("stdlib allpass: first-order allpass vs the reference") {
  const Program p = compile(R"(import "filters/allpass.pqp"
patch T {
  in    x : audio
  in    c : audio
  out   y : audio
  y = allpass(x, cutoff: c)
})");
  Stimulus st = stimulus(false);
  st.input = [](uint32_t ord, uint32_t v, uint64_t t) { return ord == 0 ? signalOf(v, t) : modCutoff(v, t); };
  compareAll(p, st, [&](uint32_t v) {
    stdref::OnePole<float> f;
    std::map<std::string, std::vector<double>> r;
    for (uint64_t t = 0; t < st.frames; ++t) {
      const float x = static_cast<float>(signalOf(v, t));
      const float lp = f.tick(x, stdref::OnePole<float>::coef(static_cast<float>(modCutoff(v, t)), kSr));
      r["y"].push_back(lp - (x - lp));
    }
    return r;
  }, "allpass");
}

// ================================================================================================================
namespace {

std::map<std::string, std::vector<double>> svfReference(uint32_t v, uint32_t frames, const std::function<float(uint64_t)>& cutoff,
                                                         const std::function<float(uint64_t)>& q,
                                                         const std::function<float(uint64_t)>& gain) {
  stdref::Svf<float> f;
  std::map<std::string, std::vector<double>> r;
  for (uint64_t t = 0; t < frames; ++t) {
    const float x = static_cast<float>(signalOf(v, t));
    const auto c = stdref::Svf<float>::coef(cutoff(t), q(t), gain(t), kSr);
    const auto o = f.tick(x, c);
    r["lp"].push_back(o.lp);
    r["bp"].push_back(o.bp);
    r["hp"].push_back(o.hp);
    r["notch"].push_back(o.notch);
    r["peak"].push_back(o.peak);
    r["ap"].push_back(o.allpass);
    r["bell"].push_back(o.bell);
    r["ls"].push_back(o.lowshelf);
    r["hs"].push_back(o.highshelf);
  }
  return r;
}

const char* kSvfOuts = R"(
  out   lp    : audio %P
  out   bp    : audio %P
  out   hp    : audio %P
  out   notch : audio %P
  out   peak  : audio %P
  out   ap    : audio %P
  out   bell  : audio %P
  out   ls    : audio %P
  out   hs    : audio %P
  lp    = f.lp
  bp    = f.bp
  hp    = f.hp
  notch = f.notch
  peak  = f.peak
  ap    = f.allpass
  bell  = f.bell
  ls    = f.lowshelf
  hs    = f.highshelf
})";

}  // namespace

TEST_CASE("stdlib svf: all nine outputs vs the reference (param cutoff / q / gain, global and voice)") {
  for (const bool voice : {false, true}) {
    const Program p = compile(withPoly(std::string(R"(import "filters/svf.pqp"
patch T {
  in    x : audio %P
  param c = 1kHz [20Hz..20kHz]
  param q = 0.7071 [0.1..40]
  param gain = 0dB [-24dB..24dB]
  f = svf(x, cutoff: c, q: q, gain: gain))") + kSvfOuts, voice));
    struct Case {
      double c, q, gainDb;
    };
    for (const Case k : {Case{1000, 0.7071, 0}, Case{40, 0.5, 6}, Case{5000, 8, -12}, Case{15000, 20, 18}, Case{200, 2, -24}}) {
      Stimulus st = stimulus(voice);
      st.input = [](uint32_t, uint32_t v, uint64_t t) { return signalOf(v, t); };
      st.params = {{"c", k.c}, {"q", k.q}, {"gain", std::pow(10.0, k.gainDb / 20.0)}};
      const float c = static_cast<float>(k.c), q = static_cast<float>(k.q), gain = static_cast<float>(std::pow(10.0, k.gainDb / 20.0));
      compareAll(p, st, [&](uint32_t v) {
        return svfReference(v, st.frames, [&](uint64_t) { return c; }, [&](uint64_t) { return q; }, [&](uint64_t) { return gain; });
      }, "svf c " + std::to_string(k.c) + " q " + std::to_string(k.q) + " gain " + std::to_string(k.gainDb) + " dB");
    }
  }
}

TEST_CASE("stdlib svf: audio-rate cutoff, q and gain modulation (global and voice, odd blocks)") {
  auto qOf = [](uint32_t v, uint64_t t) { return 0.7 + 4.0 * (0.5 + 0.5 * std::sin(0.0007 * static_cast<double>(t) + v)); };
  auto gainOf = [](uint32_t v, uint64_t t) { return std::pow(10.0, 12.0 * std::sin(0.0011 * static_cast<double>(t) + 0.5 * v) / 20.0); };
  for (const bool voice : {false, true}) {
    const Program p = compile(withPoly(std::string(R"(import "filters/svf.pqp"
patch T {
  in    x : audio %P
  in    c : audio %P
  in    q : audio %P
  in    gain : audio %P
  f = svf(x, cutoff: c, q: q, gain: gain))") + kSvfOuts, voice));
    for (const uint32_t block : {64u, 37u}) {
      Stimulus st = stimulus(voice, kSignalFrames, block);
      st.input = [&](uint32_t ord, uint32_t v, uint64_t t) {
        return ord == 0 ? signalOf(v, t) : ord == 1 ? modCutoff(v, t) : ord == 2 ? qOf(v, t) : gainOf(v, t);
      };
      compareAll(p, st, [&](uint32_t v) {
        return svfReference(v, st.frames, [&](uint64_t t) { return static_cast<float>(modCutoff(v, t)); },
                            [&](uint64_t t) { return static_cast<float>(qOf(v, t)); },
                            [&](uint64_t t) { return static_cast<float>(gainOf(v, t)); });
      }, "svf audio-rate");
    }
  }
}

TEST_CASE("stdlib svf: the designs match the analog prototypes (steady-state gain of a sine)") {
  // Each response at a few frequencies, measured from the reference (the patch matches the reference above), against
  // the bilinear-transformed analog prototype H(s) evaluated at s = j·tan(πf/sr) / tan(πfc/sr).
  using C = std::complex<double>;
  const double fc = 1000, q = 2, gainDb = 9;
  const double A = std::pow(10.0, gainDb / 40.0);
  for (const double f : {100.0, 700.0, 1000.0, 1500.0, 8000.0}) {
    const double w = std::tan(3.141592653589793 * f / kSr) / std::tan(3.141592653589793 * fc / kSr);
    const C s(0.0, w);
    const C den = s * s + s / q + 1.0;
    const std::map<std::string, C> expect = {
        {"lp", 1.0 / den},
        {"bp", (s / q) / den},
        {"hp", s * s / den},
        {"notch", (s * s + 1.0) / den},
        {"ap", (s * s - s / q + 1.0) / den},
        {"bell", (s * s + s * (A / q) + 1.0) / (s * s + s / (A * q) + 1.0)},
        {"ls", A * (s * s + std::sqrt(A) / q * s + A) / (A * s * s + std::sqrt(A) / q * s + 1.0)},
        {"hs", A * (A * s * s + std::sqrt(A) / q * s + 1.0) / (s * s + std::sqrt(A) / q * s + A)},
    };
    stdref::Svf<float> filt;
    const auto c = stdref::Svf<float>::coef(static_cast<float>(fc), static_cast<float>(q),
                                            static_cast<float>(std::pow(10.0, gainDb / 20.0)), kSr);
    const uint32_t n = 48000, settle = 24000;
    std::map<std::string, C> acc;
    for (uint32_t t = 0; t < n; ++t) {
      const double ph = 2.0 * 3.141592653589793 * f * t / kSr;
      const auto o = filt.tick(static_cast<float>(std::sin(ph)), c);
      if (t < settle) continue;
      const C e = std::polar(1.0, -ph);
      acc["lp"] += static_cast<double>(o.lp) * e;
      acc["bp"] += static_cast<double>(o.bp) * e;
      acc["hp"] += static_cast<double>(o.hp) * e;
      acc["notch"] += static_cast<double>(o.notch) * e;
      acc["ap"] += static_cast<double>(o.allpass) * e;
      acc["bell"] += static_cast<double>(o.bell) * e;
      acc["ls"] += static_cast<double>(o.lowshelf) * e;
      acc["hs"] += static_cast<double>(o.highshelf) * e;
    }
    for (const auto& [name, h] : expect) {
      // Response to sin = Im(e^{jωt}): correlate with e^{−jωt}, 2/N, then the sine's phase reference (−j).
      const C measured = acc[name] * (2.0 / (n - settle)) * C(0.0, 1.0);
      INFO(name << " at " << f << " Hz: |H| " << std::abs(measured) << " expected " << std::abs(h));
      CHECK(std::abs(measured - h) < 2e-3 * std::max(1.0, std::abs(h)));
    }
  }
}

// ================================================================================================================
TEST_CASE("stdlib biquad: raw coefficients and every designer vs the reference (global and voice)") {
  for (const bool voice : {false, true}) {
    const Program raw = compile(withPoly(R"(import "filters/biquad.pqp"
patch T {
  in    x : audio %P
  param b0 = 1
  param b1 = 0
  param b2 = 0
  param a1 = 0
  param a2 = 0
  out   y : audio %P
  y = biquad(x, b0: b0, b1: b1, b2: b2, a1: a1, a2: a2)
})", voice));
    {
      Stimulus st = stimulus(voice);
      st.input = [](uint32_t, uint32_t v, uint64_t t) { return signalOf(v, t); };
      const stdref::BiquadCoef<float> k = stdref::design<float>(BiquadType::Peak, 3000.0f, 3.0f, 2.0f, kSr);
      st.params = {{"b0", k.b0}, {"b1", k.b1}, {"b2", k.b2}, {"a1", k.a1}, {"a2", k.a2}};
      compareAll(raw, st, [&](uint32_t v) {
        stdref::Biquad<float> f;
        std::map<std::string, std::vector<double>> r;
        for (uint64_t t = 0; t < st.frames; ++t) r["y"].push_back(f.tick(static_cast<float>(signalOf(v, t)), k));
        return r;
      }, "biquad raw");
    }
    struct D {
      const char* name;
      BiquadType type;
      bool hasGain;
    };
    for (const D d : {D{"biquad_lowpass", BiquadType::Lowpass, false}, D{"biquad_highpass", BiquadType::Highpass, false},
                      D{"biquad_bandpass", BiquadType::Bandpass, false}, D{"biquad_notch", BiquadType::Notch, false},
                      D{"biquad_allpass", BiquadType::Allpass, false}, D{"biquad_peak", BiquadType::Peak, true},
                      D{"biquad_lowshelf", BiquadType::Lowshelf, true}, D{"biquad_highshelf", BiquadType::Highshelf, true}}) {
      const std::string call = std::string(d.name) + "(x, cutoff: c, q: q" + (d.hasGain ? ", gain: gain" : "") + ")";
      const Program p = compile(withPoly(R"(import "filters/biquad.pqp"
patch T {
  in    x : audio %P
  param c = 1kHz [20Hz..20kHz]
  param q = 0.7071 [0.1..40]
  param gain = 0dB [-24dB..24dB]
  out   y : audio %P
  y = )" + call + "\n}", voice));
      struct Case {
        double c, q, gainDb;
      };
      for (const Case k : {Case{1000, 0.7071, 6}, Case{60, 0.5, -9}, Case{9000, 6, 15}}) {
        Stimulus st = stimulus(voice);
        st.input = [](uint32_t, uint32_t v, uint64_t t) { return signalOf(v, t); };
        const double gain = std::pow(10.0, k.gainDb / 20.0);
        st.params = {{"c", k.c}, {"q", k.q}, {"gain", gain}};
        const auto coef = stdref::design<float>(d.type, static_cast<float>(k.c), static_cast<float>(k.q), static_cast<float>(gain), kSr);
        compareAll(p, st, [&](uint32_t v) {
          stdref::Biquad<float> f;
          std::map<std::string, std::vector<double>> r;
          for (uint64_t t = 0; t < st.frames; ++t) r["y"].push_back(f.tick(static_cast<float>(signalOf(v, t)), coef));
          return r;
        }, std::string(d.name) + " c " + std::to_string(k.c));
      }
    }
  }
}

TEST_CASE("stdlib biquad: audio-rate cutoff (coefficients per sample) vs the reference") {
  for (const bool voice : {false, true}) {
    const Program p = compile(withPoly(R"(import "filters/biquad.pqp"
patch T {
  in    x : audio %P
  in    c : audio %P
  out   lp : audio %P
  out   pk : audio %P
  lp = biquad_lowpass(x, cutoff: c, q: 1.5)
  pk = biquad_peak(x, cutoff: c, q: 1.5, gain: 6dB)
})", voice));
    Stimulus st = stimulus(voice, kSignalFrames, 37);
    st.input = [](uint32_t ord, uint32_t v, uint64_t t) { return ord == 0 ? signalOf(v, t) : modCutoff(v, t); };
    compareAll(p, st, [&](uint32_t v) {
      stdref::Biquad<float> lp, pk;
      const float g6 = static_cast<float>(std::pow(10.0, 6.0 / 20.0));
      std::map<std::string, std::vector<double>> r;
      for (uint64_t t = 0; t < st.frames; ++t) {
        const float x = static_cast<float>(signalOf(v, t)), c = static_cast<float>(modCutoff(v, t));
        r["lp"].push_back(lp.tick(x, stdref::design<float>(BiquadType::Lowpass, c, 1.5f, 1.0f, kSr)));
        r["pk"].push_back(pk.tick(x, stdref::design<float>(BiquadType::Peak, c, 1.5f, g6, kSr)));
      }
      return r;
    }, "biquad audio-rate cutoff");
  }
}

TEST_CASE("stdlib biquad: the designers match the analog prototypes") {
  using C = std::complex<double>;
  const double fc = 2000, q = 1.3, gainDb = -7.5;
  const double A = std::pow(10.0, gainDb / 40.0);
  struct D {
    BiquadType t;
    std::function<C(C)> h;
  };
  const std::vector<D> designs = {
      {BiquadType::Lowpass, [&](C s) { return 1.0 / (s * s + s / q + 1.0); }},
      {BiquadType::Highpass, [&](C s) { return s * s / (s * s + s / q + 1.0); }},
      {BiquadType::Bandpass, [&](C s) { return (s / q) / (s * s + s / q + 1.0); }},
      {BiquadType::Notch, [&](C s) { return (s * s + 1.0) / (s * s + s / q + 1.0); }},
      {BiquadType::Allpass, [&](C s) { return (s * s - s / q + 1.0) / (s * s + s / q + 1.0); }},
      {BiquadType::Peak, [&](C s) { return (s * s + s * (A / q) + 1.0) / (s * s + s / (A * q) + 1.0); }},
      {BiquadType::Lowshelf, [&](C s) { return A * (s * s + std::sqrt(A) / q * s + A) / (A * s * s + std::sqrt(A) / q * s + 1.0); }},
      {BiquadType::Highshelf, [&](C s) { return A * (A * s * s + std::sqrt(A) / q * s + 1.0) / (s * s + std::sqrt(A) / q * s + A); }},
  };
  for (const D& d : designs) {
    const auto c = stdref::design<double>(d.t, fc, q, std::pow(10.0, gainDb / 20.0), kSr);
    for (const double f : {50.0, 1000.0, 2000.0, 3000.0, 15000.0}) {
      const C z = std::polar(1.0, 2.0 * 3.141592653589793 * f / kSr), zi = 1.0 / z;
      const C hz = (c.b0 + c.b1 * zi + c.b2 * zi * zi) / (1.0 + c.a1 * zi + c.a2 * zi * zi);
      const C s(0.0, std::tan(3.141592653589793 * f / kSr) / std::tan(3.141592653589793 * fc / kSr));
      INFO("type " << static_cast<int>(d.t) << " at " << f << " Hz: |H(z)| " << std::abs(hz) << " analog " << std::abs(d.h(s)));
      CHECK(std::abs(hz - d.h(s)) < 1e-6);  // the designs round π to float32: ~1e-7 frequency error
    }
  }
}

// ================================================================================================================
TEST_CASE("stdlib dcblock: vs the reference, and DC is removed") {
  for (const bool voice : {false, true}) {
    const Program p = compile(withPoly(R"(import "filters/dcblock.pqp"
patch T {
  in    x : audio %P
  param c = 10Hz [1Hz..200Hz]
  out   y : audio %P
  y = dcblock(x, cutoff: c)
})", voice));
    for (const double cutoff : {5.0, 30.0}) {
      Stimulus st = stimulus(voice, 48000);
      auto in = [](uint32_t v, uint64_t t) { return 0.5 + 0.1 * v + (t < kSignalFrames ? signalOf(v, t) : 0.0); };
      st.input = [&](uint32_t, uint32_t v, uint64_t t) { return in(v, t); };
      st.params["c"] = cutoff;
      compareAll(p, st, [&](uint32_t v) {
        stdref::DcBlock<float> f;
        const float R = stdref::DcBlock<float>::coef(static_cast<float>(cutoff), kSr);
        std::map<std::string, std::vector<double>> r;
        for (uint64_t t = 0; t < st.frames; ++t) r["y"].push_back(f.tick(static_cast<float>(in(v, t)), R));
        return r;
      }, "dcblock " + std::to_string(cutoff) + " Hz");
      const Outputs o = run(p, Backend::Jit, st);
      const auto& y = o.at("y")[0];
      CHECK(std::fabs(y.back()) < 0.01);  // the 0.5 offset decayed (≥ 1 s at ≥ 5 Hz)
    }
  }
}

// ================================================================================================================
TEST_CASE("stdlib comb / allpass_delay: vs the reference (param and audio-rate time, global and voice, blocks 64/37/1)") {
  auto timeOf = [](uint32_t v, uint64_t t) { return 0.004 + 0.0005 * v + 0.0015 * std::sin(2.0 * 3.141592653589793 * 3.0 * t / kSr); };
  for (const bool voice : {false, true}) {
    const Program p = compile(withPoly(R"(import "filters/comb.pqp"
import "filters/allpass.pqp"
patch T {
  in    x : audio %P
  in    tm : audio %P
  param t = 7ms [2ms..50ms]
  out   c1 : audio %P
  out   c2 : audio %P
  out   a1 : audio %P
  out   a2 : audio %P
  out   l1 : audio %P
  c1 = comb(x, 60ms, time: t, fb: 0.8)
  c2 = comb(x, 60ms, time: tm, fb: -0.7)
  a1 = allpass_delay(x, 60ms, time: t, g: 0.6)
  a2 = allpass_delay(x, 60ms, time: tm, g: -0.5)
  l1 = comb_lp(x, 60ms, time: tm, fb: 0.9, damp: 0.4)
})", voice));
    for (const uint32_t block : {64u, 37u, 1u}) {
      Stimulus st = stimulus(voice, block == 1 ? 4096 : kSignalFrames, block);
      st.input = [&](uint32_t ord, uint32_t v, uint64_t t) { return ord == 0 ? signalOf(v, t) : timeOf(v, t); };
      compareAll(p, st, [&](uint32_t v) {
        stdref::Comb<float> c1(0.06f, kSr, 1), c2(0.06f, kSr, 1);
        stdref::AllpassDelay<float> a1(0.06f, kSr, 1), a2(0.06f, kSr, 1);
        stdref::CombLp<float> l1(0.06f, kSr, 1);
        std::map<std::string, std::vector<double>> r;
        for (uint64_t t = 0; t < st.frames; ++t) {
          const float x = static_cast<float>(signalOf(v, t)), tm = static_cast<float>(timeOf(v, t));
          r["c1"].push_back(c1.tick(x, stdref::Comb<float>::delaySamples(0.007f, kSr), 0.8f));
          r["c2"].push_back(c2.tick(x, stdref::Comb<float>::delaySamples(tm, kSr), -0.7f));
          r["a1"].push_back(a1.tick(x, stdref::AllpassDelay<float>::delaySamples(0.007f, kSr), 0.6f));
          r["a2"].push_back(a2.tick(x, stdref::AllpassDelay<float>::delaySamples(tm, kSr), -0.5f));
          r["l1"].push_back(l1.tick(x, stdref::CombLp<float>::delaySamples(tm, kSr), 0.9f, 0.4f));
        }
        return r;
      }, "comb/allpass_delay");
    }
  }
}

TEST_CASE("stdlib comb_lp: the fundamental is 1/time (tuning-compensated damping)") {
  // Impulse response of a long-ringing low-pass comb; the strongest DFT bin near 1/time, refined to 0.05 cents.
  const Program p = compile(R"(import "filters/comb.pqp"
patch T {
  in    x : audio
  param time = 10ms [0.1ms..50ms]
  param damp = 0.5 [0..1]
  out   y : audio
  y = comb_lp(x, 50ms, time: time, fb: 0.995, damp: damp)
})");
  for (const double f0 : {110.0, 440.0, 1000.0, 2900.0}) {
    for (const double damp : {0.2, 0.5, 1.0}) {
      Stimulus st = stimulus(false, 48000);
      st.input = [](uint32_t, uint32_t, uint64_t t) { return t == 0 ? 1.0 : 0.0; };
      st.params = {{"time", 1.0 / f0}, {"damp", damp}};
      const Outputs o = run(p, Backend::Jit, st);
      REQUIRE(o.error.empty());
      const auto& y = o.at("y")[0];
      auto power = [&](double f) {
        std::complex<double> acc;
        for (size_t t = 0; t < y.size(); ++t) acc += static_cast<double>(y[t]) * std::polar(1.0, -2.0 * 3.141592653589793 * f * t / kSr);
        return std::norm(acc);
      };
      double best = 0, bestCents = 0;
      for (double cents = -20; cents <= 20.0001; cents += 0.05) {
        const double pw = power(f0 * std::exp2(cents / 1200.0));
        if (pw > best) best = pw, bestCents = cents;
      }
      // The damping FIR is linear-phase (exactly one sample), so the loop is exactly `time` at every frequency; what
      // is left is the delay line's linear interpolation of a fractional period, which matters only at high f0. (A
      // one-pole damper without compensation would be flat by its whole delay, e.g. 15 cents at 1 kHz with
      // damp 0.8; 9 cents in the M1 review's pluck.)
      MESSAGE("comb_lp f0 " << f0 << " damp " << damp << ": peak at " << bestCents << " cents");
      CHECK(std::fabs(bestCents) <= (f0 <= 1000.0 ? 0.1 : 0.5));
    }
  }
}

// ================================================================================================================
namespace {

// Gate pattern of voice v: notes of different lengths, a short one released during its attack, and a retrigger
// two samples after a release (abutting notes).
bool gateOf(uint32_t v, uint64_t t) {
  const uint64_t s = (t + 331u * v) % 24000u;
  return (s >= 100 && s < 9000) || (s >= 12000 && s < 12100) || (s >= 15000 && s < 19000) || (s >= 19002 && s < 22000);
}

}  // namespace

TEST_CASE("stdlib adsr: vs the reference (gate patterns, param and audio-rate sustain, global and voice)") {
  for (const bool voice : {false, true}) {
    const Program p = compile(withPoly(R"(import "envelopes/adsr.pqp"
patch T {
  in    g : bool %P
  in    sus : audio %P
  param a = 5ms [0ms..10s]
  param d = 200ms [1ms..10s]
  param s = 0.7 [0..1]
  param r = 300ms [1ms..30s]
  out   e1 : audio %P
  out   e2 : audio %P
  out   done : bool %P
  env = adsr(g, a: a, d: d, s: s, r: r)
  e1 = env
  done = env.done
  e2 = adsr(g, a: 0ms, d: 30ms, s: sus, r: 50ms)
})", voice));
    auto susOf = [](uint32_t v, uint64_t t) { return 0.3 + 0.2 * std::sin(0.0003 * static_cast<double>(t) + v); };
    struct Case {
      double a, d, s, r;
    };
    for (const Case k : {Case{0.005, 0.2, 0.7, 0.3}, Case{0.05, 0.02, 0.0, 0.01}, Case{0.0, 0.5, 1.0, 1.0}}) {
      Stimulus st = stimulus(voice, 48000, 37);
      st.input = [&](uint32_t ord, uint32_t v, uint64_t t) { return ord == 0 ? (gateOf(v, t) ? 1.0 : 0.0) : susOf(v, t); };
      st.params = {{"a", k.a}, {"d", k.d}, {"s", k.s}, {"r", k.r}};
      compareAll(p, st, [&](uint32_t v) {
        stdref::Adsr<float> e1, e2;
        const auto c1 = stdref::Adsr<float>::coef(static_cast<float>(k.a), static_cast<float>(k.d), static_cast<float>(k.s),
                                                  static_cast<float>(k.r), kSr);
        std::map<std::string, std::vector<double>> r;
        for (uint64_t t = 0; t < st.frames; ++t) {
          const bool gt = gateOf(v, t);
          bool done = false, unused = false;
          r["e1"].push_back(e1.tick(gt, c1, done));
          r["done"].push_back(done ? 1.0 : 0.0);
          const auto c2 = stdref::Adsr<float>::coef(0.0f, 0.03f, static_cast<float>(susOf(v, t)), 0.05f, kSr);
          r["e2"].push_back(e2.tick(gt, c2, unused));
        }
        return r;
      }, "adsr a " + std::to_string(k.a));
    }
  }
}

TEST_CASE("stdlib adsr: stage semantics (attack time, −80 dB decay/release times, done, retrigger without a click)") {
  // The reference is checked against the stage definitions of stdlib/envelopes/adsr.pqp (the patch is checked
  // against the reference above).
  stdref::Adsr<float> e;
  const auto c = stdref::Adsr<float>::coef(0.01f, 0.1f, 0.5f, 0.2f, kSr);
  std::vector<float> lv;
  std::vector<bool> done;
  for (uint32_t t = 0; t < 48000; ++t) {
    bool d = false;
    lv.push_back(e.tick(t < 24000, c, d));
    done.push_back(d);
  }
  // Attack: 0 → 1 in a = 480 samples, linear.
  CHECK(lv[0] == doctest::Approx(1.0 / 480));
  CHECK(lv[239] == doctest::Approx(0.5).epsilon(1e-4));
  CHECK(lv[479] == doctest::Approx(1.0).epsilon(1e-5));
  // Decay: d = 4800 samples later the distance to s is 1e-4 of (1 − s).
  CHECK(lv[480 + 4799] - 0.5f == doctest::Approx(0.5e-4).epsilon(0.02));
  // (float32 stalls the approach once a step is below half an ulp of s: within ulp(s) / (2·(1 − dcoef)) of s.)
  CHECK(std::fabs(lv[23999] - 0.5) <= 6e-8 / (2.0 * (1.0 - c.dcoef)) + 1e-9);
  // Release: r = 9600 samples after the gate falls the level is 1e-4 of where it started.
  CHECK(lv[24000 + 9599] == doctest::Approx(0.5e-4).epsilon(0.02));
  // done: once released below 1e-4 (0.5 → 1e-4 takes r · ln(5000)/ln(1e4) ≈ 8892 samples).
  CHECK(!done[24000 + 8800]);
  CHECK(done[24000 + 9000]);
  CHECK(done.back());
  CHECK(!done[1000]);
  // A retrigger during the release attacks from the current level: no step larger than one attack increment.
  stdref::Adsr<float> r;
  float prev = 0.0f, worstStep = 0.0f;
  for (uint32_t t = 0; t < 10000; ++t) {
    bool d = false;
    const float x = r.tick(t < 3000 || t >= 3500, c, d);
    worstStep = std::max(worstStep, std::fabs(x - prev));
    prev = x;
  }
  CHECK(worstStep <= 1.0f / 480 + 1e-6f);
}

// ================================================================================================================
TEST_CASE("stdlib envfollow / slew / smooth: vs the reference (global and voice)") {
  for (const bool voice : {false, true}) {
    const Program p = compile(withPoly(R"(import "dynamics/envfollow.pqp"
import "control/slew.pqp"
import "control/smooth.pqp"
patch T {
  in    x : audio %P
  in    rt : audio %P
  param att = 1ms [0ms..1s]
  param rel = 100ms [1ms..10s]
  param rms = false
  out   pk : audio %P
  out   ef : audio %P
  out   sl : audio %P
  out   sm : audio %P
  out   sa : audio %P
  pk = envfollow(x, attack: 0ms, release: 50ms)
  ef = envfollow(x, attack: att, release: rel, rms: rms)
  sl = slew(x * 3, rise: 5ms, fall: 20ms)
  sm = smooth(x, time: 3ms)
  sa = smooth(x, time: rt)
})", voice));
    auto rtOf = [](uint32_t v, uint64_t t) { return 0.002 * (1.5 + std::sin(0.002 * static_cast<double>(t) + v)); };
    for (const bool rms : {false, true}) {
      Stimulus st = stimulus(voice, kSignalFrames, 37);
      st.input = [&](uint32_t ord, uint32_t v, uint64_t t) { return ord == 0 ? signalOf(v, t) : rtOf(v, t); };
      st.params = {{"att", 0.002}, {"rel", 0.08}, {"rms", rms ? 1.0 : 0.0}};
      compareAll(p, st, [&](uint32_t v) {
        stdref::EnvFollow<float> pk, ef;
        stdref::Slew<float> sl;
        stdref::Smooth<float> sm, sa;
        const float pa = stdref::EnvFollow<float>::coef(0.0f, kSr), pr = stdref::EnvFollow<float>::coef(0.05f, kSr);
        const float ea = stdref::EnvFollow<float>::coef(0.002f, kSr), er = stdref::EnvFollow<float>::coef(0.08f, kSr);
        const float up = stdref::Slew<float>::up(0.005f, kSr), down = stdref::Slew<float>::down(0.02f, kSr);
        const float c3 = stdref::Smooth<float>::coef(0.003f, kSr);
        std::map<std::string, std::vector<double>> r;
        for (uint64_t t = 0; t < st.frames; ++t) {
          const float x = static_cast<float>(signalOf(v, t));
          r["pk"].push_back(pk.tick(x, pa, pr, false));
          r["ef"].push_back(ef.tick(x, ea, er, rms));
          r["sl"].push_back(sl.tick(x * 3.0f, up, down));
          r["sm"].push_back(sm.tick(x, c3));
          r["sa"].push_back(sa.tick(x, stdref::Smooth<float>::coef(static_cast<float>(rtOf(v, t)), kSr)));
        }
        return r;
      }, std::string("envfollow/slew/smooth") + (rms ? " rms" : " peak"));
    }
  }
}

// ================================================================================================================
TEST_CASE("stdlib lfo: every shape vs the reference (free with reset, audio-rate rate, tempo sync)") {
  for (const bool voice : {false, true}) {
    const Program p = compile(withPoly(R"(import "control/lfo.pqp"
patch T {
  in    rst : bool %P
  in    fm : audio %P
  param rate = 3Hz [0.01Hz..40Hz]
  param sync = false
  param beats = 1 [0.0625..64]
  param ph = 0 [0..1]
  out   sine : audio %P
  out   tri : audio %P
  out   saw : audio %P
  out   sq : audio %P
  out   fmsaw : audio %P
  l = lfo(rst, rate: rate, sync: sync, beats: beats, phase: ph)
  sine = l.sine
  tri = l.tri
  saw = l.saw
  sq = l.square
  fmsaw = lfo(rate: fm).saw
})", voice));
    auto rstOf = [](uint32_t v, uint64_t t) { return (t + 977u * v) % 5000u == 0; };
    auto fmOf = [](uint32_t v, uint64_t t) { return 200.0 + 150.0 * std::sin(2.0 * 3.141592653589793 * 7.0 * t / kSr + v); };
    for (const bool sync : {false, true}) {
      Stimulus st = stimulus(voice, 30000, 37);
      st.input = [&](uint32_t ord, uint32_t v, uint64_t t) { return ord == 0 ? (rstOf(v, t) ? 1.0 : 0.0) : fmOf(v, t); };
      st.params = {{"rate", 17.0}, {"sync", sync ? 1.0 : 0.0}, {"beats", 0.25}, {"ph", 0.1}};
      st.transportBeat0 = 13.37;
      compareAll(p, st, [&](uint32_t v) {
        stdref::Lfo l, f;
        std::map<std::string, std::vector<double>> r;
        const double bpf = st.bpm / 60.0 / kSr;
        for (uint64_t t = 0; t < st.frames; ++t) {
          // transport.beat as the engine computes it: per block, beat0 + t0·bpf, then + f·bpf, in double.
          const uint64_t t0 = t / st.block * st.block;
          const float beat = static_cast<float>(st.transportBeat0 + static_cast<double>(t0) * bpf + static_cast<double>(t - t0) * bpf);
          const auto o = l.tick(17.0f, rstOf(v, t), sync, 0.25f, 0.1f, beat, kSr);
          r["sine"].push_back(o.sine);
          r["tri"].push_back(o.tri);
          r["saw"].push_back(o.saw);
          r["sq"].push_back(o.square);
          r["fmsaw"].push_back(f.tick(static_cast<float>(fmOf(v, t)), false, false, 1.0f, 0.0f, beat, kSr).saw);
        }
        return r;
      }, std::string("lfo") + (sync ? " synced" : " free"));
    }
  }
}
