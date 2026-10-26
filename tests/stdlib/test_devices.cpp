// T4.9 / T4.10: the stock devices of stdlib/devices/ (Utility, Saturator, EQ Eight; DSP only, panels are T4.6)
// against their hand-written C++ references (device_reference.h) on the interpreter and the JIT, plus analytic
// checks: Utility's matrix identities and its bass-mono crossover, the Saturator's transfer curves and its aliasing
// with HQ off / on, EQ Eight's magnitude responses against the analytic (bilinear) responses and its stability
// under audio-rate modulation. Every device is also held to interpreter == JIT bit for bit (no fast-math contraction
// by default, ARCHITECTURE jit), and every process() call runs in an RtScope.
#include <doctest/doctest.h>

#include <cmath>
#include <array>
#include <complex>
#include <cstring>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "device_reference.h"
#include "stdlib_harness.h"

using namespace stdlib_test;

namespace {

constexpr float kSr = 48000.0f;
constexpr double kPi = 3.141592653589793;
constexpr Backend kBackends[] = {Backend::Interp, Backend::Jit};

double noise(uint64_t t, uint32_t seed) {
  uint64_t z = (t + 1) * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(seed) * 0xD1B54A32D192ED03ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return static_cast<double>(z >> 11) / 9007199254740992.0 * 2.0 - 1.0;
}
double chirp(uint64_t t, uint64_t len, double amp) {
  const double f0 = 20.0, f1 = 20000.0, T = static_cast<double>(len) / kSr, k = std::log(f1 / f0);
  const double s = static_cast<double>(t) / kSr;
  return amp * std::sin(2.0 * kPi * f0 * T / k * (std::exp(s / T * k) - 1.0));
}
// A stereo test signal: impulse, noise, then sweeps; the channels differ (correlated noise plus independent noise,
// and a sweep against a slower sweep) so mid, side and every matrix entry are exercised.
double stereoSignal(uint32_t ch, uint64_t t, double amp = 0.8) {
  if (t < 512) return t == 0 ? (ch == 0 ? 1.0 : -0.5) : 0.0;
  if (t < 3072) return amp * (0.6 * noise(t, 7) + 0.4 * noise(t, 11 + ch));
  return ch == 0 ? chirp(t - 3072, 6144, amp) : chirp(t - 3072, 9000, amp * 0.7);
}
constexpr uint32_t kFrames = 3072 + 6144;

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
  for (size_t k = 0; (k = s.find(from, k)) != std::string::npos; k += to.size()) s.replace(k, from.size(), to);
  return s;
}

// err = |got − ref| / max(1, |ref|).
struct Check {
  double worst = 0.0;
  std::string where;
  void add(const std::string& what, const std::vector<float>& got, const std::vector<double>& ref, size_t from = 0) {
    REQUIRE_MESSAGE(got.size() == ref.size(), what);
    for (size_t t = from; t < ref.size(); ++t) {
      const double e = std::isfinite(got[t]) ? std::fabs(got[t] - ref[t]) / std::max(1.0, std::fabs(ref[t])) : INFINITY;
      if (e > worst) {
        worst = e;
        where = what + " t=" + std::to_string(t) + " got " + std::to_string(got[t]) + " ref " + std::to_string(ref[t]);
      }
    }
  }
};

// Runs `p` on both backends; requires no error, no RT violation and bit-identical outputs; returns the interpreter's.
Outputs runBoth(const Program& p, const Stimulus& st, const std::string& label) {
  const Outputs a = run(p, Backend::Interp, st);
  REQUIRE_MESSAGE(a.error.empty(), a.error);
  const Outputs b = run(p, Backend::Jit, st);
  REQUIRE_MESSAGE(b.error.empty(), b.error);
  CHECK(a.rtViolations == 0);
  CHECK(b.rtViolations == 0);
  for (const auto& [name, va] : a.byName) {
    const auto& vb = b.at(name);
    REQUIRE(va.size() == vb.size());
    for (size_t v = 0; v < va.size(); ++v) {
      size_t diffs = 0, first = 0;
      for (size_t t = 0; t < va[v].size(); ++t)
        if (std::memcmp(&va[v][t], &vb[v][t], sizeof(float)) != 0 && !(va[v][t] != va[v][t] && vb[v][t] != vb[v][t]))
          if (diffs++ == 0) first = t;
      CHECK_MESSAGE(diffs == 0, label << ": interpreter and JIT differ in '" << name << "' at " << diffs
                                      << " samples, first t=" << first << " (" << va[v][first] << " vs " << vb[v][first] << ")");
    }
  }
  return a;
}

// Magnitude (dB) of an impulse response at frequency f (direct DFT).
double magDb(const std::vector<float>& ir, size_t from, double f) {
  std::complex<double> acc = 0.0, rot = 1.0;
  const std::complex<double> step = std::polar(1.0, -2.0 * kPi * f / kSr);
  for (size_t t = from; t < ir.size(); ++t) {
    acc += static_cast<double>(ir[t]) * rot;
    rot *= step;
  }
  return 20.0 * std::log10(std::max(std::abs(acc), 1e-30));
}

// Bilinear-transform frequency variable for a design prewarped at fc: s = j·tan(πf/sr)/tan(πfc/sr).
std::complex<double> sOf(double f, double fc, double sr = kSr) {
  return {0.0, std::tan(kPi * f / sr) / std::tan(kPi * std::min(fc, 0.49 * sr) / sr)};
}

double db(std::complex<double> h) { return 20.0 * std::log10(std::max(std::abs(h), 1e-30)); }

std::vector<double> logFreqs(double lo, double hi, int n) {
  std::vector<double> f;
  for (int i = 0; i < n; ++i) f.push_back(lo * std::pow(hi / lo, static_cast<double>(i) / (n - 1)));
  return f;
}

// ================================================================================================================
// Utility
// ================================================================================================================
const char* kUtilityWrapper = R"(import "devices/utility.pqp"
patch T {
  in  x : audio stereo
  in  c : audio
  out y : audio stereo
  out mid : audio
  out side : audio
  out bass : audio
  u = utility(x, bassMono: %BM, audition: %AU, dc: %DC, bassFreq: c)
  y = u
  mid = u.mid
  side = u.side
  bass = u.bass
})";

Program utilityProgram(bool bassMono, bool audition, bool dc) {
  std::string t = kUtilityWrapper;
  t = replaceAll(t, "%BM", bassMono ? "1" : "0");
  t = replaceAll(t, "%AU", audition ? "1" : "0");
  t = replaceAll(t, "%DC", dc ? "1" : "0");
  return compile(t);
}

void setUtilityParams(Stimulus& st, const devref::UtilityParams& p) {
  st.params["u__chan"] = p.chan;
  st.params["u__invL"] = p.invL;
  st.params["u__invR"] = p.invR;
  st.params["u__width"] = p.width;
  st.params["u__msMode"] = p.msMode;
  st.params["u__ms"] = p.ms;
  st.params["u__mono"] = p.mono;
  st.params["u__balance"] = p.balance;
  st.params["u__gain"] = p.gain;
  st.params["u__mute"] = p.mute;
}

double bassMod(uint64_t t) { return 120.0 * std::exp2(1.6 * std::sin(2.0 * kPi * 3.0 * static_cast<double>(t) / kSr)) * (1.0 + 0.3 * std::sin(2.0 * kPi * 170.0 * static_cast<double>(t) / kSr)); }

}  // namespace

// Every structural configuration × param set `sets` (all eight in the slow tier) vs devref::Utility.
static void utilityVsReference(bool full) {
  std::vector<devref::UtilityParams> sets;
  {
    devref::UtilityParams p;
    sets.push_back(p);                                                  // defaults
    p.chan = 1; p.invR = true; p.width = 2.5f; p.balance = 0.3f; p.gain = 1.7f; sets.push_back(p);
    p = {}; p.chan = 2; p.invL = true; p.msMode = true; p.ms = -0.4f; p.balance = -0.8f; p.gain = 0.25f; sets.push_back(p);
    p = {}; p.chan = 3; p.msMode = true; p.ms = 0.7f; sets.push_back(p);
    p = {}; p.mono = true; p.width = 3.0f; p.gain = 56.234f; sets.push_back(p);
    p = {}; p.width = 0.0f; p.invL = true; p.invR = true; sets.push_back(p);
    p = {}; p.gain = 0.00031622776f; sets.push_back(p);                  // −70 dB = −inf
    p = {}; p.mute = true; p.gain = 2.0f; sets.push_back(p);
  }
  for (const bool bassMono : {false, true})
    for (const bool audition : {false, true}) {
      if (audition && !bassMono) continue;
      for (const bool dc : {false, true}) {
        const Program prog = utilityProgram(bassMono, audition, dc);
        for (size_t si = 0; si < sets.size(); ++si) {
          if (!full && si % 2 != (dc ? 1u : 0u)) continue;  // default tier: half the sets per configuration
          for (const uint32_t block : {64u, 37u}) {
            if (block == 37 && si % 3 != 0) continue;
            Stimulus st;
            st.frames = kFrames;
            st.block = block;
            st.inputCh = [](uint32_t ord, uint32_t ch, uint32_t, uint64_t t) { return ord == 0 ? stereoSignal(ch, t) : bassMod(t); };
            st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
            setUtilityParams(st, sets[si]);
            const std::string label = "utility bassMono " + std::to_string(bassMono) + " audition " + std::to_string(audition) +
                                      " dc " + std::to_string(dc) + " set " + std::to_string(si) + " block " + std::to_string(block);
            const Outputs o = runBoth(prog, st, label);
            devref::Utility ref;
            ref.conf = {bassMono, audition, dc};
            std::map<std::string, std::vector<double>> r;
            for (uint64_t t = 0; t < st.frames; ++t) {
              devref::UtilityParams p = sets[si];
              p.bassFreq = static_cast<float>(bassMod(t));
              ref.tick(static_cast<float>(stereoSignal(0, t)), static_cast<float>(stereoSignal(1, t)), p);
              r["y"].push_back(ref.out[0]);
              r["y[1]"].push_back(ref.out[1]);
              r["mid"].push_back(ref.mid);
              r["side"].push_back(ref.side);
              r["bass"].push_back(ref.bass);
            }
            Check c;
            for (const auto& [name, v] : r) c.add(name, o.at(name)[0], v);
            CHECK_MESSAGE(c.worst <= 1e-5, label << ": max error " << c.worst << " (" << c.where << ")");
          }
        }
      }
    }
}

TEST_CASE("Utility: every mode vs the C++ reference, interpreter == JIT") { utilityVsReference(false); }
TEST_CASE("Utility: every mode x every param set vs the C++ reference" * doctest::test_suite("slow")) { utilityVsReference(true); }

TEST_CASE("Utility: analytic identities (width, mid/side, mono, channel modes, phase, balance, gain, mute)") {
  const Program prog = utilityProgram(false, false, false);
  // The coefficients are smoothed from 0 (2 ms time constant): compare after 100 ms (e^−50: settled exactly).
  constexpr size_t kSettle = 4800, kLen = 8192;
  auto runWith = [&](const devref::UtilityParams& p) {
    Stimulus st;
    st.frames = kLen;
    st.inputCh = [](uint32_t ord, uint32_t ch, uint32_t, uint64_t t) { return ord == 0 ? stereoSignal(ch, t + 600) : 120.0; };
    st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
    setUtilityParams(st, p);
    return run(prog, Backend::Jit, st);
  };
  auto expectEach = [&](const devref::UtilityParams& p, const std::function<void(double L, double R, double oL, double oR)>& f) {
    const Outputs o = runWith(p);
    REQUIRE(o.error.empty());
    for (size_t t = kSettle; t < kLen; ++t) f(stereoSignal(0, t + 600), stereoSignal(1, t + 600), o.at("y")[0][t], o.at("y[1]")[0][t]);
  };
  constexpr double kEps = 2e-6;
  devref::UtilityParams p;
  SUBCASE("defaults pass through, bit for bit once the smoothing has settled") {
    expectEach(p, [](double L, double R, double oL, double oR) { CHECK(oL == static_cast<float>(L)); CHECK(oR == static_cast<float>(R)); });
  }
  SUBCASE("width 0 = mono = (L + R)/2 on both sides") {
    p.width = 0.0f;
    expectEach(p, [](double L, double R, double oL, double oR) { CHECK(std::fabs(oL - 0.5 * (L + R)) < kEps); CHECK(oL == oR); });
    p = {};
    p.mono = true;
    p.width = 3.0f;
    expectEach(p, [](double L, double R, double oL, double oR) { CHECK(std::fabs(oL - 0.5 * (L + R)) < kEps); CHECK(oL == oR); });
  }
  SUBCASE("width w scales the side only: M and S identities") {
    p.width = 2.5f;
    expectEach(p, [](double L, double R, double oL, double oR) {
      CHECK(std::fabs(0.5 * (oL + oR) - 0.5 * (L + R)) < 3 * kEps);
      CHECK(std::fabs(0.5 * (oL - oR) - 2.5 * 0.5 * (L - R)) < 3 * kEps);
    });
  }
  SUBCASE("mid/side mode: −1 = mid only, +1 = side only, 0 = unchanged") {
    p.msMode = true;
    p.ms = -1.0f;
    expectEach(p, [](double L, double R, double oL, double oR) { CHECK(std::fabs(oL - 0.5 * (L + R)) < kEps); CHECK(oL == oR); });
    p.ms = 1.0f;
    expectEach(p, [](double L, double R, double oL, double oR) { CHECK(std::fabs(oL - 0.5 * (L - R)) < kEps); CHECK(std::fabs(oR + oL) < kEps); });
    p.ms = 0.0f;
    expectEach(p, [](double L, double R, double oL, double oR) { CHECK(std::fabs(oL - L) < kEps); CHECK(std::fabs(oR - R) < kEps); });
  }
  SUBCASE("channel modes and phase") {
    p.chan = 1;
    expectEach(p, [](double L, double, double oL, double oR) { CHECK(std::fabs(oL - L) < kEps); CHECK(std::fabs(oR - L) < kEps); });
    p.chan = 2;
    expectEach(p, [](double, double R, double oL, double oR) { CHECK(std::fabs(oL - R) < kEps); CHECK(std::fabs(oR - R) < kEps); });
    p.chan = 3;
    expectEach(p, [](double L, double R, double oL, double oR) { CHECK(std::fabs(oL - R) < kEps); CHECK(std::fabs(oR - L) < kEps); });
    p.chan = 0;
    p.invL = true;
    expectEach(p, [](double L, double R, double oL, double oR) { CHECK(std::fabs(oL + L) < kEps); CHECK(std::fabs(oR - R) < kEps); });
    p.invL = false;
    p.invR = true;
    expectEach(p, [](double L, double R, double oL, double oR) { CHECK(std::fabs(oL - L) < kEps); CHECK(std::fabs(oR + R) < kEps); });
  }
  SUBCASE("balance, gain, −inf, mute") {
    p.balance = 0.5f;
    expectEach(p, [](double L, double R, double oL, double oR) { CHECK(std::fabs(oL - 0.5 * L) < kEps); CHECK(std::fabs(oR - R) < kEps); });
    p.balance = -1.0f;
    expectEach(p, [](double L, double, double oL, double oR) { CHECK(std::fabs(oL - L) < kEps); CHECK(oR == 0.0); });
    p.balance = 0.0f;
    p.gain = 3.981071705534973f;  // +12 dB
    expectEach(p, [](double L, double R, double oL, double oR) { CHECK(std::fabs(oL - 3.981071705534973 * L) < 4 * kEps); CHECK(std::fabs(oR - 3.981071705534973 * R) < 4 * kEps); });
    p.gain = 0.00031622776f;
    expectEach(p, [](double, double, double oL, double oR) { CHECK(std::fabs(oL) < 1e-9); CHECK(std::fabs(oR) < 1e-9); });
    p.gain = 1.0f;
    p.mute = true;
    expectEach(p, [](double, double, double oL, double oR) { CHECK(std::fabs(oL) < 1e-9); CHECK(std::fabs(oR) < 1e-9); });
  }
}

TEST_CASE("Utility: bass mono is an LR4 crossover whose bands sum flat; DC filter") {
  constexpr size_t kPre = 9600, kLen = 32768;  // 200 ms of settling (smoothing, filter states at rest), then the IR
  for (const double fc : {50.0, 120.0, 500.0}) {
    const Program prog = utilityProgram(true, false, false);
    const Program aud = utilityProgram(true, true, false);
    // Mono input (L = R = impulse): out = AP(M), flat in magnitude. Side input (L = −R): out L = HP4(S).
    for (const bool side : {false, true}) {
      Stimulus st;
      st.frames = static_cast<uint32_t>(kPre + kLen);
      st.inputCh = [&](uint32_t ord, uint32_t ch, uint32_t, uint64_t t) {
        if (ord == 1) return fc;
        const double v = t == kPre ? 1.0 : 0.0;
        return side && ch == 1 ? -v : v;
      };
      st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
      const Outputs o = runBoth(prog, st, "bass mono IR");
      double worst = 0.0;
      for (const double f : logFreqs(10.0, 20000.0, 61)) {
        const double got = magDb(o.at("y")[0], kPre, f);
        if (!side) {
          worst = std::max(worst, std::fabs(got));  // the bands sum flat: 0 dB everywhere
        } else {
          const auto s = sOf(f, fc);
          const auto hp = s * s / (1.0 + std::sqrt(2.0) * s + s * s);
          const double want = db(hp * hp);
          if (want > -60.0) worst = std::max(worst, std::fabs(got - want));  // below: float32 noise of the IR
        }
      }
      CHECK_MESSAGE(worst < 0.01, "fc " << fc << std::string(side ? " side: HP4" : " mono: flat") << ", worst " << worst << " dB");
      if (side) CHECK(std::fabs(magDb(o.at("y")[0], kPre, fc) + 6.0206) < 0.01);  // LR4: −6 dB at the crossover
    }
    // Audition: LP4 of the mid; LP4 + HP4 of the same signal is the allpass (flat).
    Stimulus st;
    st.frames = static_cast<uint32_t>(kPre + kLen);
    st.inputCh = [&](uint32_t ord, uint32_t, uint32_t, uint64_t t) { return ord == 1 ? fc : t == kPre ? 1.0 : 0.0; };
    st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
    const Outputs a = runBoth(aud, st, "bass mono audition IR");
    double worst = 0.0;
    for (const double f : logFreqs(10.0, 20000.0, 61)) {
      const auto s = sOf(f, fc);
      const auto lp = 1.0 / (1.0 + std::sqrt(2.0) * s + s * s);
      const double want = db(lp * lp);
      if (want > -60.0) worst = std::max(worst, std::fabs(magDb(a.at("y")[0], kPre, f) - want));
    }
    CHECK_MESSAGE(worst < 0.01, "fc " << fc << " audition LP4, worst " << worst << " dB");
  }
  // DC filter: a 0.5 offset under a 1 kHz sine is gone after a second; the sine passes.
  const Program dc = utilityProgram(false, false, true);
  Stimulus st;
  st.frames = 96000;
  st.inputCh = [](uint32_t ord, uint32_t, uint32_t, uint64_t t) { return ord == 1 ? 120.0 : 0.5 + 0.25 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(t) / kSr); };
  st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
  const Outputs o = runBoth(dc, st, "dc");
  double mean = 0.0, peak = 0.0;
  for (size_t t = 48000; t < 96000; ++t) {
    mean += o.at("y")[0][t];
    peak = std::max(peak, std::fabs(static_cast<double>(o.at("y")[0][t])));
  }
  mean /= 48000.0;
  CHECK(std::fabs(mean) < 1e-4);
  CHECK(std::fabs(peak - 0.25) < 0.002);
}

// ================================================================================================================
// Saturator
// ================================================================================================================
namespace {

Program saturatorProgram(int type, int hq, bool color, bool softClip, bool driveIn = false) {
  std::string t = R"(import "devices/saturator.pqp"
patch T {
  in  x : audio stereo
  in  d : audio
  out y : audio stereo
  out driven : audio stereo
  out wet : audio stereo
  s = saturator(x, type: %T, hq: %H, color: %C, softClip: %S%D)
  y = s
  driven = s.driven
  wet = s.wet
})";
  t = replaceAll(t, "%T", std::to_string(type));
  t = replaceAll(t, "%H", std::to_string(hq));
  t = replaceAll(t, "%C", color ? "1" : "0");
  t = replaceAll(t, "%S", softClip ? "1" : "0");
  t = replaceAll(t, "%D", driveIn ? ", drive: d" : "");
  return compile(t);
}

void setSatParams(Stimulus& st, const devref::SatParams& p, bool driveIn = false) {
  if (!driveIn) st.params["s__drive"] = p.drive;
  st.params["s__output"] = p.output;
  st.params["s__dryWet"] = p.dryWet;
  st.params["s__base"] = p.base;
  st.params["s__colorFreq"] = p.colorFreq;
  st.params["s__colorWidth"] = p.colorWidth;
  st.params["s__depth"] = p.depth;
  st.params["s__wsDrive"] = p.wsDrive;
  st.params["s__wsLin"] = p.wsLin;
  st.params["s__wsCurve"] = p.wsCurve;
  st.params["s__wsDamp"] = p.wsDamp;
  st.params["s__wsDepth"] = p.wsDepth;
  st.params["s__wsPeriod"] = p.wsPeriod;
}

double driveMod(uint64_t t) { return 2.5 + 2.0 * std::sin(2.0 * kPi * 230.0 * static_cast<double>(t) / kSr); }

// Runs one saturator configuration on both backends against devref::Saturator over the stereo test signal.
void checkSaturator(const devref::SatConf& conf, const devref::SatParams& p, uint32_t block, bool driveIn, const std::string& label) {
  const Program prog = saturatorProgram(conf.type, conf.hq, conf.color, conf.softClip, driveIn);
  Stimulus st;
  st.frames = kFrames;
  st.block = block;
  st.inputCh = [](uint32_t ord, uint32_t ch, uint32_t, uint64_t t) { return ord == 0 ? stereoSignal(ch, t) : driveMod(t); };
  st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
  setSatParams(st, p, driveIn);
  const Outputs o = runBoth(prog, st, label);
  devref::Saturator ref;
  ref.conf = conf;
  std::map<std::string, std::vector<double>> r;
  for (uint64_t t = 0; t < st.frames; ++t) {
    devref::SatParams q = p;
    if (driveIn) q.drive = static_cast<float>(driveMod(t));
    const float x[2] = {static_cast<float>(stereoSignal(0, t)), static_cast<float>(stereoSignal(1, t))};
    ref.tick(x, q);
    for (int ch = 0; ch < 2; ++ch) {
      const std::string sfx = ch ? "[1]" : "";
      r["y" + sfx].push_back(ref.out[ch]);
      r["driven" + sfx].push_back(ref.driven[ch]);
      r["wet" + sfx].push_back(ref.wet[ch]);
    }
  }
  Check c;
  for (const auto& [name, v] : r) c.add(name, o.at(name)[0], v);
  CHECK_MESSAGE(c.worst <= 1e-5, label << ": max error " << c.worst << " (" << c.where << ")");
}

devref::SatParams satParams(int variant) {
  devref::SatParams p;
  switch (variant % 3) {
    case 0: p.drive = 3.981f; break;  // +12 dB
    case 1: p.drive = 1.0f; p.output = 0.5f; p.dryWet = 0.6f; p.wsDrive = 1.0f; p.wsLin = 0.3f; p.wsCurve = 0.9f; p.wsDamp = 0.4f; p.wsDepth = 0.3f; p.wsPeriod = 0.5f; break;
    default: p.drive = 15.85f; p.output = 0.2f; p.wsDrive = 0.7f; p.wsDepth = 0.8f; p.wsPeriod = 1.0f; p.wsDamp = 1.0f; break;  // +24 dB
  }
  p.base = 2.0f;
  p.colorFreq = 3000.0f;
  p.colorWidth = 1.5f;
  p.depth = 0.5f;
  return p;
}

// ---- FFT (radix 2, double) for the aliasing measurement --------------------------------------------------------
void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const std::complex<double> wl = std::polar(1.0, -2.0 * kPi / static_cast<double>(len));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w = 1.0;
      for (size_t k = 0; k < len / 2; ++k) {
        const auto u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// Aliasing of a steady sine through `prog`: the sine sits exactly on bin k0 of an N-point FFT (no window needed),
// so every harmonic below Nyquist is one bin; everything else (the folded harmonics) is aliasing. Returns the
// power outside the harmonic bins (and DC) up to 20 kHz relative to the total harmonic power, in dB. (Harmonics
// between 20 and 28 kHz fall in the half-band filters' transition band and fold into 20 … 24 kHz by design:
// stdlib/util/oversample.pqp keeps 0 … 20 kHz clean.)
double aliasingDb(const Program& prog, const std::map<std::string, double>& params, uint32_t k0) {
  constexpr size_t kN = 16384, kSkip = 8192;
  Stimulus st;
  st.frames = static_cast<uint32_t>(kSkip + kN);
  st.inputCh = [&](uint32_t ord, uint32_t, uint32_t, uint64_t t) {
    return ord == 0 ? 0.5 * std::sin(2.0 * kPi * static_cast<double>(k0) * static_cast<double>(t) / kN) : 1.0;
  };
  st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
  st.params = params;
  const Outputs o = run(prog, Backend::Jit, st);
  REQUIRE_MESSAGE(o.error.empty(), o.error);
  std::vector<std::complex<double>> a(kN);
  for (size_t t = 0; t < kN; ++t) a[t] = o.at("y")[0][kSkip + t];
  fft(a);
  std::vector<bool> harmonic(kN / 2 + 1, false);
  for (size_t h = 1; h * k0 <= kN / 2; ++h) harmonic[h * k0] = true;
  double ph = 0.0, pa = 0.0;
  const size_t top = static_cast<size_t>(20000.0 / kSr * kN);
  for (size_t b = 1; b <= kN / 2; ++b)
    if (harmonic[b]) ph += std::norm(a[b]);
    else if (b <= top) pa += std::norm(a[b]);
  return 10.0 * std::log10(std::max(pa, 1e-300) / ph);
}

}  // namespace

TEST_CASE("Saturator: every curve type vs the C++ reference (HQ off and 2x), interpreter == JIT") {
  for (int type = 0; type <= 8; ++type)
    for (const int hq : {1, 2}) {
      devref::SatConf conf;
      conf.type = type;
      conf.hq = hq;
      checkSaturator(conf, satParams(type), 64, false, "saturator type " + std::to_string(type) + " hq " + std::to_string(hq));
    }
}

TEST_CASE("Saturator: 4x / 8x, colour, soft clip, dry/wet, audio-rate drive vs the C++ reference") {
  struct Cfg {
    int type, hq;
    bool color, softClip, driveIn;
    uint32_t block;
  };
  for (const Cfg& c : {Cfg{3, 4, false, false, false, 64}, Cfg{6, 8, false, false, false, 64}, Cfg{7, 4, true, true, false, 37},
                       Cfg{0, 8, true, false, true, 64}, Cfg{2, 1, true, true, true, 37}, Cfg{5, 2, false, true, true, 64}}) {
    devref::SatConf conf;
    conf.type = c.type;
    conf.hq = c.hq;
    conf.color = c.color;
    conf.softClip = c.softClip;
    checkSaturator(conf, satParams(c.type + c.hq), c.block, c.driveIn,
                   "saturator type " + std::to_string(c.type) + " hq " + std::to_string(c.hq) + (c.color ? " colour" : "") +
                       (c.softClip ? " softclip" : "") + (c.driveIn ? " audio-rate drive" : ""));
  }
}

TEST_CASE("Saturator: every curve type at 4x and 8x vs the C++ reference" * doctest::test_suite("slow")) {
  for (int type = 0; type <= 8; ++type)
    for (const int hq : {4, 8}) {
      devref::SatConf conf;
      conf.type = type;
      conf.hq = hq;
      conf.softClip = type % 2 == 1;
      checkSaturator(conf, satParams(type + 1), 64, false, "saturator type " + std::to_string(type) + " hq " + std::to_string(hq));
    }
}

TEST_CASE("Saturator: transfer curves are the documented formulas") {
  // A slow ramp −4 … 4 through hq 1, drive 0 dB: out = curve(x), compared with the formulas in double.
  auto formula = [](int type, double u, const devref::SatParams& p) -> double {
    switch (type) {
      case 0: { const double c = std::clamp(u, -1.5, 1.5); return c - 4.0 / 27.0 * c * c * c; }
      case 1: return std::sin(std::clamp(u, -kPi / 2, kPi / 2));
      case 2: return u * (std::fabs(u) + 2.0) / (u * u + std::fabs(u) + 1.0);
      case 3: return std::tanh(u);
      case 4: return u / std::pow(1.0 + u * u * u * u, 0.25);
      case 5: return std::sin(u);
      case 6: return std::clamp(u, -1.0, 1.0);
      case 7: {
        const double t = std::clamp(u, -1.0, 1.0);
        const double shape = p.wsLin * u + p.wsCurve * (1.5 * t - 0.5 * t * t * t) + p.wsDepth * std::sin(kPi * (1.0 + 15.0 * p.wsPeriod) * t);
        const double gate = (u * u + 1e-30) / (u * u + 0.01 * p.wsDamp * p.wsDamp + 1e-30);
        return u + p.wsDrive * (shape * gate - u);
      }
      default: return std::clamp(u, -1.0, 1.0);
    }
  };
  constexpr uint32_t kN = 8192;
  auto ramp = [](uint64_t t) { return -4.0 + 8.0 * static_cast<double>(t) / (kN - 1); };
  for (int type = 0; type <= 8; ++type) {
    const Program prog = saturatorProgram(type, 1, false, false);
    devref::SatParams p;
    p.wsDrive = 0.8f;
    p.wsLin = 0.6f;
    p.wsCurve = 0.7f;
    p.wsDamp = 0.5f;
    p.wsDepth = 0.25f;
    p.wsPeriod = 0.3f;
    Stimulus st;
    st.frames = kN;
    st.inputCh = [&](uint32_t ord, uint32_t, uint32_t, uint64_t t) { return ord == 0 ? ramp(t) : 1.0; };
    st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
    setSatParams(st, p);
    const Outputs o = runBoth(prog, st, "transfer type " + std::to_string(type));
    double worst = 0.0, at = 0.0;
    for (uint32_t t = 0; t < kN; ++t) {
      const double e = std::fabs(o.at("y")[0][t] - formula(type, static_cast<float>(ramp(t)), p));
      if (e > worst) {
        worst = e;
        at = ramp(t);
      }
    }
    CHECK_MESSAGE(worst < 2e-6, "type " << type << ": max deviation from the formula " << worst << " at x = " << at);
    // Odd symmetry: curve(−x) = −curve(x) (the ramp is symmetric: sample t and N−1−t).
    double asym = 0.0;
    for (uint32_t t = 0; t < kN; ++t) asym = std::max(asym, std::fabs(static_cast<double>(o.at("y")[0][t]) + o.at("y")[0][kN - 1 - t]));
    CHECK_MESSAGE(asym < 2e-6, "type " << type << " not odd-symmetric: " << asym);
  }
}

TEST_CASE("Saturator: oversampling removes the aliasing (HQ off / 2x / 4x / 8x)") {
  // A 0.5 sine at bin 1531 of 16384 (4485 Hz), driven +12 dB into the hard curves: harmonics reach far above
  // Nyquist, so without oversampling they fold back between the harmonics.
  const std::map<std::string, double> params = {{"s__drive", 3.981}};
  for (const int type : {0, 4, 6}) {
    double prev = 0.0;
    std::string line;
    for (const int hq : {1, 2, 4, 8}) {
      const double a = aliasingDb(saturatorProgram(type, hq, false, false), params, 1531);
      line += " " + std::to_string(hq) + "x: " + std::to_string(a) + " dB";
      // Each doubling must lower the aliasing by at least 6 dB until it reaches the float32 floor (≈ −120 dB).
      if (hq > 1) CHECK_MESSAGE((a < prev - 6.0 || a < -115.0), "type " << type << " hq " << hq << " did not reduce aliasing: " << a << " vs " << prev);
      prev = a;
    }
    MESSAGE("saturator type " << type << " aliasing (non-harmonic / harmonic power):" << line);
    // Measured (T4.9): Analog Clip −43 / −67 / −87 / −106 dB, Hard Curve −44 / −80 / −126 / −125 dB, Digital Clip
    // −30 / −44 / −56 / −70 dB at 1× / 2× / 4× / 8×. 8× must clear −60 dB even for the digital clip's corners.
    CHECK_MESSAGE(prev < -60.0, "type " << type << " 8x aliasing " << prev << " dB");
  }
}

TEST_CASE("Saturator: dry/wet 0 is the input exactly; waveshaper drive 0 is the identity") {
  devref::SatParams p;
  p.dryWet = 0.0f;
  p.drive = 15.0f;
  const Program prog = saturatorProgram(6, 2, true, true);
  Stimulus st;
  st.frames = 4096;
  st.inputCh = [](uint32_t ord, uint32_t ch, uint32_t, uint64_t t) { return ord == 0 ? stereoSignal(ch, t) : 1.0; };
  st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
  setSatParams(st, p);
  const Outputs o = runBoth(prog, st, "dry");
  for (uint32_t t = 0; t < st.frames; ++t) {
    CHECK(o.at("y")[0][t] == static_cast<float>(stereoSignal(0, t)));
    CHECK(o.at("y[1]")[0][t] == static_cast<float>(stereoSignal(1, t)));
  }
  devref::SatParams w;
  w.wsDrive = 0.0f;
  w.wsDepth = 1.0f;
  w.wsDamp = 1.0f;
  const Program ws = saturatorProgram(7, 1, false, false);
  setSatParams(st, w);
  const Outputs q = runBoth(ws, st, "ws bypass");
  for (uint32_t t = 0; t < st.frames; ++t) CHECK(q.at("y")[0][t] == static_cast<float>(stereoSignal(0, t)));
}
