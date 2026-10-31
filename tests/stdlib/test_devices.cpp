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
  // The coefficients start at their targets (L10) and are smoothed (2 ms) only when they change; compare after 100 ms.
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

// M4 review M10: with HQ the dry/wet mix runs on the oversampled samples, so dry/wet 0 is the input through the
// half-bands' up + down chain (an allpass, flat to 20 kHz), exactly as devref computes it; without HQ it is the input
// bit for bit. (Before M10 it was the input bit for bit at every HQ, and the partial settings combed.)
TEST_CASE("Saturator: dry/wet 0 is the input exactly (HQ off) or the half-bands' allpass of it (HQ on); waveshaper drive 0 is the identity") {
  devref::SatParams p;
  p.dryWet = 0.0f;
  p.drive = 15.0f;
  Stimulus st;
  st.frames = 4096;
  st.inputCh = [](uint32_t ord, uint32_t ch, uint32_t, uint64_t t) { return ord == 0 ? stereoSignal(ch, t) : 1.0; };
  st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
  setSatParams(st, p);
  {
    const Outputs o = runBoth(saturatorProgram(6, 1, true, true), st, "dry 1x");
    for (uint32_t t = 0; t < st.frames; ++t) {
      CHECK(o.at("y")[0][t] == static_cast<float>(stereoSignal(0, t)));
      CHECK(o.at("y[1]")[0][t] == static_cast<float>(stereoSignal(1, t)));
    }
  }
  for (const int hq : {2, 8}) {
    const Outputs o = runBoth(saturatorProgram(6, hq, true, true), st, "dry hq");
    devref::OsUp up[2];
    devref::OsDown down[2];
    double worst = 0.0;
    for (uint32_t t = 0; t < st.frames; ++t)
      for (int ch = 0; ch < 2; ++ch) {
        float s8[8];
        up[ch].tick(static_cast<float>(stereoSignal(ch, t)), hq, s8);
        const double want = down[ch].tick(s8, hq);
        worst = std::max(worst, std::fabs(o.at(ch ? "y[1]" : "y")[0][t] - want));
      }
    CHECK_MESSAGE(worst == 0.0, "hq " << hq << ": dry/wet 0 differs from down(up(x)) by " << worst);
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

// ================================================================================================================
// EQ Eight
// ================================================================================================================
namespace {

// Audio-rate modulators: a cutoff sweeping ~60 Hz … 20 kHz (3 Hz) with a 700 Hz wobble, a gain ±15 dB (5 Hz),
// a Q 0.1 … 18 (11 Hz).
double eqModFreq(uint64_t t) {
  const double s = static_cast<double>(t) / kSr;
  return 1000.0 * std::exp2(4.2 * std::sin(2.0 * kPi * 3.0 * s)) * (1.0 + 0.5 * std::sin(2.0 * kPi * 700.0 * s));
}
double eqModGain(uint64_t t) { return std::pow(10.0, 15.0 * std::sin(2.0 * kPi * 5.0 * static_cast<double>(t) / kSr) / 20.0); }
double eqModQ(uint64_t t) { return 0.1 * std::pow(180.0, 0.5 + 0.5 * std::sin(2.0 * kPi * 11.0 * static_cast<double>(t) / kSr)); }

// mods: "" (none); "some": A band 3 freq, band 4 gain, band 5 q; "all": every A band's freq (m1 · (0.25 … 2)),
// gain (m2 on odd bands, 1/m2 on even ones, so the cascade's gain stays within ±15 dB-ish) and q (0.1 + 0.3·m3:
// 0.13 … 5.5); "full": as "all" with q = m3 itself (0.1 … 18; for the stability test).
Program eqProgram(const devref::EqConf& c, const std::string& mods = "") {
  std::string args = "mode: " + std::to_string(c.mode) + ", hq: " + std::to_string(c.hq);
  for (int s = 0; s < 2; ++s)
    for (int i = 0; i < 8; ++i) {
      const std::string n = (s ? "B" : "") + std::to_string(i + 1);
      args += ", type" + n + ": " + std::to_string(c.type[s][i]) + ", on" + n + ": " + (c.on[s][i] ? "1" : "0");
    }
  if (mods == "some") args += ", freq3: m1, gain4: m2, q5: m3";
  if (mods == "all" || mods == "full")
    for (int i = 0; i < 8; ++i) {
      const std::string n = std::to_string(i + 1), f = std::to_string(0.25 + 0.25 * i);
      args += ", freq" + n + ": m1 * " + f + ", gain" + n + (i % 2 ? ": 1 / m2" : ": m2") +
              (mods == "full" ? ", q" + n + ": m3" : ", q" + n + ": 0.1 + m3 * 0.3");
    }
  std::string t = R"(import "devices/eq_eight.pqp"
patch T {
  in  x : audio stereo
  in  m1 : audio
  in  m2 : audio
  in  m3 : audio
  out y : audio stereo
  out a : audio
  out b : audio
  out band1 : audio
  out band2 : audio
  out band3 : audio
  out band4 : audio
  out band5 : audio
  out band6 : audio
  out band7 : audio
  out band8 : audio
  e = eq_eight(x, %ARGS)
  y = e
  a = e.a
  b = e.b
  band1 = e.band1
  band2 = e.band2
  band3 = e.band3
  band4 = e.band4
  band5 = e.band5
  band6 = e.band6
  band7 = e.band7
  band8 = e.band8
})";
  return compile(replaceAll(t, "%ARGS", args));
}

double modFactor(int i) { return 0.25 + 0.25 * i; }

void setEqParams(Stimulus& st, const devref::EqParams& p, const std::string& mods = "") {
  for (int s = 0; s < 2; ++s)
    for (int i = 0; i < 8; ++i) {
      const std::string n = (s ? "B" : "") + std::to_string(i + 1);
      const bool all = s == 0 && (mods == "all" || mods == "full"), some = mods == "some" && s == 0;
      if (!all && !(some && i == 2)) st.params["e__freq" + n] = p.band[s][i].freq;
      if (!all && !(some && i == 3)) st.params["e__gain" + n] = p.band[s][i].gain;
      if (!all && !(some && i == 4)) st.params["e__q" + n] = p.band[s][i].q;
    }
  st.params["e__adaptive"] = p.adaptive;
  st.params["e__scale"] = p.scale;
  st.params["e__outGain"] = p.outGain;
}

// The reference's params at sample t with the modulators applied.
devref::EqParams modulated(devref::EqParams p, const std::string& mods, uint64_t t) {
  if (mods == "some") {
    p.band[0][2].freq = static_cast<float>(eqModFreq(t));
    p.band[0][3].gain = static_cast<float>(eqModGain(t));
    p.band[0][4].q = static_cast<float>(eqModQ(t));
  } else if (mods == "all" || mods == "full") {
    for (int i = 0; i < 8; ++i) {
      // The wrapper's expressions, in float32 as the patch computes them.
      const float m2 = static_cast<float>(eqModGain(t)), m3 = static_cast<float>(eqModQ(t));
      p.band[0][i].freq = static_cast<float>(eqModFreq(t)) * static_cast<float>(modFactor(i));
      p.band[0][i].gain = i % 2 ? 1.0f / m2 : m2;
      p.band[0][i].q = mods == "full" ? m3 : devref::fma_(m3, 0.3f, 0.1f);
    }
  }
  return p;
}

Stimulus eqStimulus(uint32_t frames, uint32_t block, const std::function<double(uint32_t, uint64_t)>& x) {
  Stimulus st;
  st.frames = frames;
  st.block = block;
  st.inputCh = [x](uint32_t ord, uint32_t ch, uint32_t, uint64_t t) {
    switch (ord) {
      case 0: return x(ch, t);
      case 1: return eqModFreq(t);
      case 2: return eqModGain(t);
      default: return eqModQ(t);
    }
  };
  st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
  return st;
}

void checkEq(const devref::EqConf& conf, const devref::EqParams& p, const std::string& mods, uint32_t block, const std::string& label,
             double tol = 1e-5) {
  const Program prog = eqProgram(conf, mods);
  Stimulus st = eqStimulus(kFrames, block, [](uint32_t ch, uint64_t t) { return stereoSignal(ch, t, 0.5); });
  setEqParams(st, p, mods);
  const Outputs o = runBoth(prog, st, label);
  devref::EqEight ref;
  ref.conf = conf;
  std::map<std::string, std::vector<double>> r;
  for (uint64_t t = 0; t < st.frames; ++t) {
    const devref::EqParams q = modulated(p, mods, t);
    ref.tick(static_cast<float>(stereoSignal(0, t, 0.5)), static_cast<float>(stereoSignal(1, t, 0.5)), q);
    r["y"].push_back(ref.out[0]);
    r["y[1]"].push_back(ref.out[1]);
    r["a"].push_back(ref.a[0]);
    if (conf.mode == 0) r["a[1]"].push_back(ref.a[1]);
    r["b"].push_back(ref.b);
    for (int i = 0; i < 8; ++i) {
      r["band" + std::to_string(i + 1)].push_back(ref.band[i][0]);
      if (conf.mode == 0) r["band" + std::to_string(i + 1) + "[1]"].push_back(ref.band[i][1]);
    }
  }
  Check c;
  for (const auto& [name, v] : r) c.add(name, o.at(name)[0], v);
  CHECK_MESSAGE(c.worst <= tol, label << ": max error " << c.worst << " (" << c.where << ")");
}

// Every type once in the A set (and a different arrangement in B), non-default params.
devref::EqConf allTypes(int mode, int hq) {
  devref::EqConf c;
  c.mode = mode;
  c.hq = hq;
  const int a[8] = {0, 2, 3, 4, 3, 5, 6, 7}, b[8] = {1, 3, 3, 2, 5, 4, 7, 6};
  for (int i = 0; i < 8; ++i) {
    c.type[0][i] = a[i];
    c.type[1][i] = b[i];
    c.on[0][i] = c.on[1][i] = true;
  }
  c.on[1][5] = false;
  return c;
}
devref::EqParams someParams() {
  devref::EqParams p;
  const float f[8] = {40, 120, 400, 1500, 3000, 6000, 11000, 16000};
  const float g[8] = {1, 2.5f, 0.3f, 1, 4.0f, 0.6f, 1, 1};
  const float q[8] = {0.7071f, 1.2f, 2.5f, 6.0f, 0.5f, 0.9f, 3.0f, 1.4f};
  for (int s = 0; s < 2; ++s)
    for (int i = 0; i < 8; ++i) {
      p.band[s][i].freq = f[i] * (s ? 0.8f : 1.0f);
      p.band[s][i].gain = s ? 1.0f / g[i] : g[i];
      p.band[s][i].q = q[i];
    }
  p.outGain = 0.7f;
  return p;
}

// Analytic response of one band (the bilinear design: the analog prototype at s = j·tan(πf/sr)/tan(πfc/sr)).
std::complex<double> eqAnalytic(int type, double f, const devref::EqBand& b, bool adaptive, double scale, double srEff) {
  const auto s = sOf(f, b.freq, srEff);
  const double ge = std::pow(static_cast<double>(b.gain), scale), A = std::sqrt(ge);
  const double qe = adaptive ? b.q * std::sqrt(std::max(ge, 1.0 / ge)) : b.q;
  const double k = 1.0 / b.q, kb = 1.0 / qe, sA = std::sqrt(A);
  const std::complex<double> s2 = s * s;
  auto sec = [&](double kk, bool hp) { return (hp ? s2 : std::complex<double>(1.0)) / (s2 + kk * s + 1.0); };
  switch (type) {
    case 0:
    case 7: {
      // An 8th-order Butterworth (section dampings 2·sin((2i − 1)π/16), from its pole angles) whose least damped
      // section alone carries the resonance: its damping × 0.7071/q (M4 review H3). Derived here from the poles, not
      // from the device's constants (T4.10's version of this test shared the device's law, so it could not see that
      // scaling all four sections multiplied their peaks).
      std::complex<double> h = 1.0;
      for (int i = 1; i <= 4; ++i) {
        const double d = 2.0 * std::sin((2 * i - 1) * kPi / 16.0);
        h *= sec(i == 1 ? d * std::sqrt(0.5) * k : d, type == 0);
      }
      return h;
    }
    case 1: return sec(k, true);
    case 2: return A * (s2 + sA * kb * s + A) / (A * s2 + sA * kb * s + 1.0);
    case 3: return (s2 + kb * A * s + 1.0) / (s2 + kb / A * s + 1.0);
    case 4: return (s2 + 1.0) / (s2 + k * s + 1.0);
    case 5: return A * (A * s2 + sA * kb * s + 1.0) / (s2 + sA * kb * s + A);
    default: return sec(k, false);
  }
}

}  // namespace

static void eqVsReference(bool full) {
  for (const int mode : {0, 1, 2})
    for (const int hq : {1, 2}) {
      if (!full && (mode + hq) % 2 == 0) continue;  // default tier: stereo 1×, L/R 2×, M/S 1×
      const devref::EqConf c = allTypes(mode, hq);
      devref::EqParams p = someParams();
      if (mode == 2) {
        p.adaptive = true;
        p.scale = 1.5f;
      }
      checkEq(c, p, "", hq == 2 ? 37 : 64, "eq mode " + std::to_string(mode) + " hq " + std::to_string(hq));
    }
  // The default device (bands 2–7 on, flat).
  checkEq(devref::EqConf{}, devref::EqParams{}, "", 64, "eq defaults");
}

TEST_CASE("EQ Eight: every type, mode and HQ vs the C++ reference, interpreter == JIT") { eqVsReference(false); }
TEST_CASE("EQ Eight: every mode at 1x and 2x vs the C++ reference" * doctest::test_suite("slow")) { eqVsReference(true); }

TEST_CASE("EQ Eight: audio-rate freq / gain / Q modulation vs the C++ reference") {
  for (const int hq : {1, 2}) {
    devref::EqParams p = someParams();
    p.adaptive = hq == 2;
    checkEq(allTypes(0, hq), p, "some", 64, "eq modulated some hq " + std::to_string(hq));
    checkEq(allTypes(1, hq), p, "all", 37, "eq modulated all hq " + std::to_string(hq));
  }
}

static void eqMagnitudes(bool full) {
  struct Cfg {
    int type;
    float freq, gainDb, q;
    bool adaptive;
    float scale;
  };
  const Cfg cfgs[] = {
      {0, 30, 0, 0.7071f, false, 1}, {0, 1000, 0, 3, false, 1},     {0, 12000, 0, 0.5f, false, 1},
      {1, 100, 0, 0.7071f, false, 1}, {1, 5000, 0, 4, false, 1},
      {2, 100, 12, 0.7071f, false, 1}, {2, 2000, -9, 1.5f, true, 1}, {2, 400, 6, 1, false, 1.5f},
      {3, 1000, 15, 1, false, 1},     {3, 60, -12, 4, false, 1},    {3, 12000, 6, 0.3f, false, 1}, {3, 18000, -15, 8, false, 1},
      {3, 3000, 10, 2, true, 1},      {3, 700, 8, 1.5f, true, -0.5f},
      {4, 1000, 0, 2, false, 1},      {4, 300, 0, 10, false, 1},
      {5, 8000, 9, 0.7071f, false, 1}, {5, 500, -6, 2, true, 1.2f},
      {6, 8000, 0, 0.7071f, false, 1}, {6, 200, 0, 6, false, 1},
      {7, 2000, 0, 0.7071f, false, 1}, {7, 15000, 0, 1.5f, false, 1},
  };
  constexpr uint32_t kLen = 32768;
  int lastType[3] = {-1, -1, -1};
  for (const int hq : {1, 2})
    for (const Cfg& cf : cfgs) {
      if (!full && lastType[hq] == cf.type) continue;  // default tier: the first setting of each type
      lastType[hq] = cf.type;
      devref::EqConf c;
      c.hq = hq;
      for (int i = 0; i < 8; ++i) c.on[0][i] = c.on[1][i] = false;
      c.type[0][3] = cf.type;
      c.on[0][3] = true;
      devref::EqParams p;
      p.band[0][3] = {cf.freq, static_cast<float>(std::pow(10.0, cf.gainDb / 20.0)), cf.q};
      p.adaptive = cf.adaptive;
      p.scale = cf.scale;
      const Program prog = eqProgram(c);
      Stimulus st = eqStimulus(kLen, 64, [](uint32_t, uint64_t t) { return t == 0 ? 1.0 : 0.0; });
      setEqParams(st, p);
      const Outputs o = run(prog, hq == 2 ? Backend::Jit : Backend::Interp, st);
      REQUIRE_MESSAGE(o.error.empty(), o.error);
      double worst = 0.0, at = 0.0;
      for (const double f : logFreqs(20.0, 20000.0, 41)) {
        const double want = db(eqAnalytic(cf.type, f, p.band[0][3], cf.adaptive, cf.scale, kSr * hq));
        if (want < -60.0) continue;  // below: float32 noise of the impulse response
        const double e = std::fabs(magDb(o.at("y")[0], 0, f) - want);
        if (e > worst) {
          worst = e;
          at = f;
        }
      }
      CHECK_MESSAGE(worst < 0.1, "type " << cf.type << " freq " << cf.freq << " gain " << cf.gainDb << " q " << cf.q << " hq " << hq
                                         << ": max deviation " << worst << " dB at " << at << " Hz");
    }
}

TEST_CASE("EQ Eight: magnitude responses vs the analytic responses within 0.1 dB") { eqMagnitudes(false); }

// M4 review H3: the 48 dB cuts' resonance. Their peak gain (rendered impulse response, measured on a fine grid around
// the corner, fc/8 … 8·fc) stays within 1 dB above Low Cut 12's at the same q (or the 0 dB passband), over the whole Q range: one resonance, not four
// multiplied (T4.10's law gave +105 dB at q 18 against LC12's +25 dB).
static double eqPeakDb(int type, double q) {
  constexpr uint32_t kLen = 16384;
  constexpr double kFc = 1000.0;
  devref::EqConf c;
  for (int i = 0; i < 8; ++i) c.on[0][i] = c.on[1][i] = false;
  c.type[0][3] = type;
  c.on[0][3] = true;
  devref::EqParams p;
  p.band[0][3] = {static_cast<float>(kFc), 1.0f, static_cast<float>(q)};
  const Program prog = eqProgram(c);
  Stimulus st = eqStimulus(kLen, 64, [](uint32_t, uint64_t t) { return t == 0 ? 1.0 : 0.0; });
  setEqParams(st, p);
  const Outputs o = run(prog, Backend::Jit, st);
  REQUIRE_MESSAGE(o.error.empty(), o.error);
  const auto& y = o.at("y")[0];
  // Coarse: 1.2 % steps over fc/8 … 8·fc; then 1/40 of a step around the coarse maximum (a q-18 peak is 5.5 % wide).
  const std::vector<double> coarse = logFreqs(kFc / 8.0, kFc * 8.0, 361);
  size_t best = 0;
  double top = -1e9;
  for (size_t i = 0; i < coarse.size(); ++i)
    if (const double m = magDb(y, 0, coarse[i]); m > top) top = m, best = i;
  const double lo = coarse[best > 0 ? best - 1 : 0], hi = coarse[std::min(best + 1, coarse.size() - 1)];
  for (const double f : logFreqs(lo, hi, 81)) top = std::max(top, magDb(y, 0, f));
  return top;
}

static void eqCutPeaks(const std::vector<double>& qs) {
  for (const double q : qs) {
    // Every cut's passband is unity (0 dB); at low q LC12's gain is still below it at 8·fc, so the bound is the larger
    // of LC12's peak and the passband.
    const double lc12 = std::max(eqPeakDb(1, q), 0.0), lc48 = eqPeakDb(0, q), hc48 = eqPeakDb(7, q);
    MESSAGE("q " << q << ": peak LC12 " << lc12 << " dB, LC48 " << lc48 << " dB, HC48 " << hc48 << " dB");
    CHECK_MESSAGE(lc48 <= lc12 + 1.0, "q " << q << ": LC48 peak " << lc48 << " dB vs LC12 " << lc12 << " dB");
    CHECK_MESSAGE(hc48 <= lc12 + 1.0, "q " << q << ": HC48 peak " << hc48 << " dB vs LC12 " << lc12 << " dB");
    // and the resonance is still there: within 1 dB below LC12's as well once LC12 resonates
    if (lc12 > 1.0) CHECK_MESSAGE(lc48 >= lc12 - 1.0, "q " << q << ": LC48 peak " << lc48 << " dB vs LC12 " << lc12 << " dB");
  }
}

TEST_CASE("EQ Eight: the 48 dB cuts resonate no more than Low Cut 12 (peak within +1 dB) at q 0.1, 1.5, 18") {
  eqCutPeaks({0.1, 1.5, 18.0});
}
TEST_CASE("EQ Eight: the 48 dB cuts' peak within +1 dB of Low Cut 12 over q 0.1 … 18" * doctest::test_suite("slow")) {
  eqCutPeaks({0.1, 0.2, 0.4, 0.7071, 0.85, 1.0, 1.25, 2.0, 3.0, 4.0, 6.0, 9.0, 13.0, 18.0});
}
TEST_CASE("EQ Eight: magnitude responses of 22 settings x 1x / 2x within 0.1 dB" * doctest::test_suite("slow")) { eqMagnitudes(true); }

TEST_CASE("EQ Eight: HQ keeps high bells closer to the analog shape") {
  // A +12 dB bell at 16 kHz, q 1: at 1× the bilinear transform squeezes it towards Nyquist; at 2× it is much closer
  // to the analog bell (unwarped: s = j·f/fc).
  constexpr uint32_t kLen = 16384;
  double dev[3] = {};
  for (const int hq : {1, 2}) {
    devref::EqConf c;
    c.hq = hq;
    for (int i = 0; i < 8; ++i) c.on[0][i] = false;
    c.on[0][3] = true;
    devref::EqParams p;
    p.band[0][3] = {16000.0f, 3.981071705534973f, 1.0f};
    const Program prog = eqProgram(c);
    Stimulus st = eqStimulus(kLen, 64, [](uint32_t, uint64_t t) { return t == 0 ? 1.0 : 0.0; });
    setEqParams(st, p);
    const Outputs o = run(prog, Backend::Jit, st);
    REQUIRE(o.error.empty());
    for (const double f : logFreqs(2000.0, 20000.0, 31)) {
      const std::complex<double> s(0.0, f / 16000.0);
      const double A = std::sqrt(3.981071705534973);
      const double want = db((s * s + A * s + 1.0) / (s * s + s / A + 1.0));
      dev[hq] = std::max(dev[hq], std::fabs(magDb(o.at("y")[0], 0, f) - want));
    }
  }
  MESSAGE("bell 16 kHz +12 dB: max deviation from the analog bell 2–20 kHz: 1x " << dev[1] << " dB, 2x " << dev[2] << " dB");
  CHECK(dev[2] < 0.5 * dev[1]);
}

namespace {

double lowTones(uint64_t t) {
  const double s = static_cast<double>(t) / kSr;
  return 0.3 * (std::sin(2.0 * kPi * 110.0 * s) + std::sin(2.0 * kPi * 440.0 * s + 1.0) + std::sin(2.0 * kPi * 1300.0 * s + 2.0));
}

// The 10 ms-window RMS maximum of a signal from sample `from` on.
template <class T>
double maxRms(const std::vector<T>& y, size_t from = 0) {
  double acc = 0.0, best = 0.0;
  for (size_t t = from; t < y.size(); ++t) {
    acc += static_cast<double>(y[t]) * y[t];
    if ((t - from) % 480 == 479) {
      best = std::max(best, std::sqrt(acc / 480.0));
      acc = 0.0;
    }
  }
  return best;
}

// The modulated ANALOG filter of one band, integrated in double (RK4, 16 steps per sample; coefficients and input
// interpolated linearly between samples): each SVF section is bp' = ω(x − k·bp − lp), lp' = ω·bp with
// ω = 2·os·sr·g (g the band's prewarped integrator gain, so ω is the analog frequency the bilinear design maps to),
// output m0·x + m1·bp + m2·lp, the eq8_coef coefficients of each sample.
std::vector<double> analogBand(int type, int os, const std::vector<devref::EqBand>& bands, bool adaptive, double scale,
                               const std::vector<double>& x) {
  const size_t n = x.size();
  const int nsec = type == 0 || type == 7 ? 4 : 1;
  struct Sec {
    double g, k, m0, m1, m2;
  };
  std::vector<std::array<Sec, 4>> cs(n);
  for (size_t t = 0; t < n; ++t) {
    const devref::EqCoef c = devref::eqCoef(type, static_cast<float>(os), bands[t], adaptive, static_cast<float>(scale), kSr);
    const bool hp = type == 0;
    cs[t][0] = {c.g, c.k, c.m0, c.m1, c.m2};
    const double ks[3] = {c.k2, c.k3, c.k4};
    for (int i = 0; i < 3; ++i) cs[t][i + 1] = {c.g, ks[i], hp ? 1.0 : 0.0, hp ? -ks[i] : 0.0, hp ? -1.0 : 1.0};
  }
  constexpr int kSteps = 16;
  const double dt = 1.0 / (kSr * kSteps), wScale = 2.0 * os * kSr;
  double st[4][2] = {};  // (bp, lp) per section
  std::vector<double> out(n, 0.0);
  // Derivatives of the cascade at fractional position u ∈ [0, 1] of sample interval t.
  auto deriv = [&](size_t t, double u, const double (&s)[4][2], double (&d)[4][2]) {
    const size_t t1 = std::min(t + 1, n - 1);
    double in = x[t] + u * (x[t1] - x[t]);
    for (int i = 0; i < nsec; ++i) {
      const Sec& a = cs[t][i];
      const Sec& b = cs[t1][i];
      const double g = a.g + u * (b.g - a.g), k = a.k + u * (b.k - a.k);
      const double w = wScale * g;
      d[i][0] = w * (in - k * s[i][0] - s[i][1]);
      d[i][1] = w * s[i][0];
      const double m0 = a.m0 + u * (b.m0 - a.m0), m1 = a.m1 + u * (b.m1 - a.m1), m2 = a.m2 + u * (b.m2 - a.m2);
      in = m0 * in + m1 * s[i][0] + m2 * s[i][1];
    }
  };
  auto output = [&](size_t t) {
    double in = x[t];
    for (int i = 0; i < nsec; ++i) in = cs[t][i].m0 * in + cs[t][i].m1 * st[i][0] + cs[t][i].m2 * st[i][1];
    return in;
  };
  for (size_t t = 0; t < n; ++t) {
    out[t] = output(t);
    for (int step = 0; step < kSteps; ++step) {
      const double u = static_cast<double>(step) / kSteps, h = 1.0 / kSteps;
      double k1[4][2], k2[4][2], k3[4][2], k4[4][2], tmp[4][2];
      deriv(t, u, st, k1);
      for (int i = 0; i < 4; ++i) for (int j = 0; j < 2; ++j) tmp[i][j] = st[i][j] + 0.5 * dt * k1[i][j];
      deriv(t, u + 0.5 * h, tmp, k2);
      for (int i = 0; i < 4; ++i) for (int j = 0; j < 2; ++j) tmp[i][j] = st[i][j] + 0.5 * dt * k2[i][j];
      deriv(t, u + 0.5 * h, tmp, k3);
      for (int i = 0; i < 4; ++i) for (int j = 0; j < 2; ++j) tmp[i][j] = st[i][j] + dt * k3[i][j];
      deriv(t, u + h, tmp, k4);
      for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 2; ++j) st[i][j] += dt / 6.0 * (k1[i][j] + 2.0 * k2[i][j] + 2.0 * k3[i][j] + k4[i][j]);
    }
  }
  return out;
}

}  // namespace

static void eqTracksAnalog(bool full) {
  // One band of each type with its freq (7 Hz … 6.9 kHz, a 3 Hz sweep plus a ±50 % 700 Hz wobble), gain (±15 dB)
  // and Q (0.1 … 18) all modulated at audio rate, over 0.5 s of three tones (110, 440, 1300 Hz: low enough that the
  // bilinear frequency warping of the input does not matter). Deep, fast cutoff modulation of a resonant filter
  // pumps energy into it in the ANALOG filter too (parametric resonance: the cuts' Q-18 sections gain ~70 dB here),
  // so "stable" means: the digital band's energy follows the analog band's (10 ms RMS maxima within 1 dB; measured
  // ≤ 0.35 dB) and adds no growth of its own.
  constexpr uint32_t kN = 24000;
  for (const int hq : {1, 2})
    for (int type = 0; type < 8; ++type) {
      if (!full && !(hq == 1 && (type == 0 || type == 3)) && !(hq == 2 && type == 5)) continue;
      devref::EqConf c;
      c.hq = hq;
      for (int i = 0; i < 8; ++i) c.on[0][i] = c.on[1][i] = false;
      c.type[0][0] = type;
      c.on[0][0] = true;
      const Program prog = eqProgram(c, "full");
      Stimulus st = eqStimulus(kN, 64, [](uint32_t, uint64_t t) { return lowTones(t); });
      setEqParams(st, someParams(), "full");
      const Outputs o = run(prog, Backend::Jit, st);
      REQUIRE_MESSAGE(o.error.empty(), o.error);
      std::vector<double> x(kN);
      std::vector<devref::EqBand> bands(kN);
      for (uint32_t t = 0; t < kN; ++t) {
        x[t] = static_cast<float>(lowTones(t));
        bands[t] = modulated(someParams(), "full", t).band[0][0];
      }
      std::vector<double> ana = analogBand(type, hq, bands, false, 1.0, x);
      for (double& v : ana) v *= static_cast<double>(someParams().outGain);  // the device's output gain (0.7)

      const double rd = maxRms(o.at("y")[0]), ra = maxRms(ana);
      const double ddb = 20.0 * std::log10(rd / ra);
      MESSAGE("type " << type << " hq " << hq << ": max 10 ms RMS digital " << rd << ", analog " << ra << " (" << ddb << " dB)");
      CHECK(std::isfinite(rd));
      CHECK_MESSAGE(std::fabs(ddb) < 1.0, "type " << type << " hq " << hq << ": digital " << rd << " vs analog " << ra);
    }
}

TEST_CASE("EQ Eight: under audio-rate modulation a band tracks the modulated analog filter (no blow-up)") { eqTracksAnalog(false); }
TEST_CASE("EQ Eight: under audio-rate modulation every band type tracks the analog filter, 1x and 2x" * doctest::test_suite("slow")) {
  eqTracksAnalog(true);
}

TEST_CASE("EQ Eight: all eight bands modulated at audio rate stay finite and ring out") {
  // Every band (every type) with freq, gain (±15 dB) and Q (0.1 … 18) modulated at audio rate: 1 s of noise, then
  // 1 s of silence while the modulation goes on. Interpreter == JIT bit for bit throughout.
  for (const int hq : {1, 2}) {
    const Program prog = eqProgram(allTypes(0, hq), "full");
    Stimulus st = eqStimulus(96000, 64, [](uint32_t ch, uint64_t t) { return t < 48000 ? 0.5 * noise(t, 3 + ch) : 0.0; });
    setEqParams(st, someParams(), "full");
    const Outputs o = runBoth(prog, st, "eq full modulation hq " + std::to_string(hq));
    double peak = 0.0, tail = 0.0;
    bool finite = true;
    for (int ch = 0; ch < 2; ++ch) {
      const auto& y = o.at(ch ? "y[1]" : "y")[0];
      for (size_t t = 0; t < y.size(); ++t) {
        finite &= std::isfinite(y[t]);
        peak = std::max(peak, std::fabs(static_cast<double>(y[t])));
        if (t >= 84000) tail = std::max(tail, std::fabs(static_cast<double>(y[t])));
      }
    }
    MESSAGE("eq full modulation hq " << hq << ": peak " << peak << ", last 250 ms peak " << tail);
    CHECK(finite);
    CHECK(tail < 1e-6);
  }
}

// ================================================================================================================
// M4 review M10, M13, L10 (T4.x-fix-dsp): HQ dry/wet alignment, smoothed params, Utility's start value
// ================================================================================================================

TEST_CASE("Saturator: HQ dry/wet 50 % is flat within 0.1 dB to 18 kHz (2x / 4x / 8x)") {
  // Small signal (an impulse of 1e-3: every curve is linear there, Analog Clip's cubic term is 1e-7 of it), so the
  // wet path is the half-bands' allpass and dry + wet must not comb.
  constexpr double kAmp = 1e-3;
  constexpr uint32_t kLen = 8192;
  for (const int hq : {2, 4, 8}) {
    const Program prog = saturatorProgram(0, hq, false, false);
    Stimulus st;
    st.frames = kLen;
    st.inputCh = [](uint32_t ord, uint32_t, uint32_t, uint64_t t) { return ord == 0 && t == 0 ? kAmp : 0.0; };
    st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
    st.params["s__dryWet"] = 0.5;
    const Outputs o = runBoth(prog, st, "dry/wet 50 % hq " + std::to_string(hq));
    // What mixing the plain input with the oversampled wet path gave (before M10): x/2 + down(up(x))/2.
    std::vector<float> old(kLen);
    devref::OsUp up;
    devref::OsDown down;
    for (uint32_t t = 0; t < kLen; ++t) {
      const float x = t == 0 ? static_cast<float>(kAmp) : 0.0f;
      float s8[8];
      up.tick(x, hq, s8);
      old[t] = 0.5f * x + 0.5f * down.tick(s8, hq);
    }
    double worst = 0.0, worstAt = 0.0, oldWorst = 0.0, oldAt = 0.0;
    for (const double f : logFreqs(20.0, 18000.0, 160)) {
      const double e = magDb(o.at("y")[0], 0, f) - 20.0 * std::log10(kAmp);
      const double eo = magDb(old, 0, f) - 20.0 * std::log10(kAmp);
      if (std::fabs(e) > std::fabs(worst)) worst = e, worstAt = f;
      if (std::fabs(eo) > std::fabs(oldWorst)) oldWorst = eo, oldAt = f;
    }
    MESSAGE("hq " << hq << " dry/wet 50 %: worst " << worst << " dB at " << worstAt << " Hz (unaligned dry: " << oldWorst
                  << " dB at " << oldAt << " Hz; 5 kHz " << magDb(old, 0, 5000.0) - 20.0 * std::log10(kAmp) << " dB, 10 kHz "
                  << magDb(old, 0, 10000.0) - 20.0 * std::log10(kAmp) << " dB)");
    CHECK(std::fabs(worst) <= 0.1);
  }
}

namespace {

// The click of a param step: the largest |first difference| (sample-to-sample step) of the output in the 10 ms after
// the step (at a block boundary, 1.3 rad into the sine's cycle, so the step does not hide in a zero crossing), over
// the largest one in the steady stretches before the step and after it has settled (a sine in: the steady first
// difference is 2π·f/sr · amplitude). An output that jumps by Δ steps by Δ in one sample: many times the steady
// value; a 2 ms one-pole glide spreads Δ over ~100 samples, and a filter whose coefficients step at a block boundary
// keeps a continuous output (its states are the analog filter's), so both stay near 1.
double stepClick(const Program& prog, const std::string& param, double from, double to, double hz, double amp = 0.5,
                 const std::map<std::string, double>& params = {}, const std::string& out = "y") {
  constexpr uint64_t kStep = 9600, kLen = 19200;
  Stimulus st;
  st.frames = kLen;
  st.block = 64;
  st.inputCh = [&](uint32_t ord, uint32_t, uint32_t, uint64_t t) {
    return ord == 0 ? amp * std::sin(2.0 * kPi * hz * (static_cast<double>(t) - static_cast<double>(kStep)) / kSr + 1.3) : 0.0;
  };
  st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
  st.params = params;
  st.params[param] = from;
  st.paramAt = [&](const std::string& name, uint64_t t0) { return name == param && t0 >= kStep ? to : NAN; };
  const Outputs o = runBoth(prog, st, "step " + param);
  const auto& y = o.at(out)[0];
  auto d1 = [&](uint64_t t) { return std::fabs(static_cast<double>(y[t]) - y[t - 1]); };
  double steady = 0.0, click = 0.0;
  for (uint64_t t = kStep - 4800; t < kStep; ++t) steady = std::max(steady, d1(t));
  for (uint64_t t = kStep + 4800; t < kLen; ++t) steady = std::max(steady, d1(t));
  for (uint64_t t = kStep; t < kStep + 480; ++t) click = std::max(click, d1(t));
  return click / steady;
}

}  // namespace

TEST_CASE("Saturator and EQ Eight: param jumps glide (smoothed gains) instead of clicking") {
  struct Case {
    std::string what;
    const Program* prog;
    std::string param;
    double from, to, hz, limit;
    std::map<std::string, double> params;
  };
  const Program sat1 = saturatorProgram(0, 1, false, false), sat2 = saturatorProgram(0, 2, false, false);
  devref::EqConf ec;
  const Program eq = eqProgram(ec);
  ec.type[0][3] = 6;  // band 4 a High Cut 12
  const Program eqHc = eqProgram(ec);
  const std::vector<Case> cases = {
      {"saturator drive 0 -> +12 dB", &sat1, "s__drive", 1.0, 3.981, 220.0, 2.0, {}},
      {"saturator output 0 -> -12 dB", &sat1, "s__output", 1.0, 0.251, 220.0, 2.0, {}},
      {"saturator dry/wet 100 -> 30 % (HQ 2x)", &sat2, "s__dryWet", 1.0, 0.3, 220.0, 2.0, {{"s__drive", 3.981}}},
      {"eq eight band 4 gain 0 -> +12 dB (1 kHz bell)", &eq, "e__gain4", 1.0, 3.981, 1000.0, 2.0, {}},
      {"eq eight band 4 q 0.71 -> 4 at +12 dB", &eq, "e__q4", 0.7071, 4.0, 1000.0, 2.0, {{"e__gain4", 3.981}}},
      {"eq eight band 4 freq 1 -> 2 kHz at +12 dB", &eq, "e__freq4", 1000.0, 2000.0, 1500.0, 2.0, {{"e__gain4", 3.981}}},
      {"eq eight output 0 -> -12 dB", &eq, "e__outGain", 1.0, 0.251, 220.0, 2.0, {}},
      {"eq eight band 4 high cut 12 freq 4 -> 1 kHz", &eqHc, "e__freq4", 4000.0, 1000.0, 700.0, 2.0, {{"e__q4", 2.0}}},
  };
  for (const Case& c : cases) {
    const double r = stepClick(*c.prog, c.param, c.from, c.to, c.hz, 0.5, c.params);
    MESSAGE(c.what << ": click " << r << " x the steady sample-to-sample step");
    CHECK_MESSAGE(r < c.limit, c.what << ": " << r);
  }
}

TEST_CASE("Utility starts at its target: an impulse at sample 0 comes out at once (M4 review L10)") {
  for (const bool bassMono : {false, true}) {
    const Program prog = utilityProgram(bassMono, false, false);
    Stimulus st;
    st.frames = 256;
    st.inputCh = [](uint32_t ord, uint32_t ch, uint32_t, uint64_t t) { return ord == 0 ? (t == 0 ? (ch ? -0.5 : 1.0) : 0.0) : 120.0; };
    st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
    devref::UtilityParams p;
    p.gain = 0.5f;
    p.width = 2.0f;
    setUtilityParams(st, p);
    const Outputs o = runBoth(prog, st, "utility start");
    if (!bassMono) {
      // M = 0.25, S = 0.75; out = gain · (M ± width·S).
      CHECK(o.at("y")[0][0] == doctest::Approx(0.5 * (0.25 + 1.5)).epsilon(1e-6));
      CHECK(o.at("y[1]")[0][0] == doctest::Approx(0.5 * (0.25 - 1.5)).epsilon(1e-6));
    } else {
      devref::Utility ref;
      ref.conf.bassMono = true;
      p.bassFreq = 120.0f;
      ref.tick(1.0f, -0.5f, p);
      CHECK(o.at("y")[0][0] == doctest::Approx(ref.out[0]).epsilon(1e-6));
      CHECK(std::fabs(ref.out[0]) > 0.1);
    }
  }
}

TEST_CASE("Stock devices with smoothed params still sleep on silence (SPEC 7.7), and sleeping changes nothing") {
  // A 0.2 s burst, a param change in the silence (it must wake the smoother, which settles and sleeps again), then
  // silence: every device's segments sleep (stats.sleeps > 0) and the output equals a run that never sleeps.
  struct Dev {
    const char* what;
    const Program* prog;
    const char* param;
    double value;
  };
  devref::EqConf ec;
  const Program sat2 = saturatorProgram(0, 2, false, false), sat1 = saturatorProgram(3, 1, true, false), eq = eqProgram(ec),
                util = utilityProgram(false, false, false);
  const Dev devs[] = {{"saturator hq 2", &sat2, "s__output", 0.5},
                      {"saturator hq 1 colour", &sat1, "s__drive", 2.0},
                      {"eq eight", &eq, "e__gain4", 2.0},
                      {"utility", &util, "u__gain", 0.5}};
  for (const Dev& d : devs) {
    Stimulus st;
    st.frames = 48000;
    st.inputCh = [](uint32_t ord, uint32_t ch, uint32_t, uint64_t t) {
      return ord == 0 && t < 9600 ? 0.5 * std::sin(2.0 * kPi * 330.0 * static_cast<double>(t) / kSr + ch) : ord == 0 ? 0.0 : 120.0;
    };
    st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
    const std::string param = d.param;
    const double value = d.value;
    st.paramAt = [param, value](const std::string& name, uint64_t t0) { return name == param && t0 >= 24000 ? value : NAN; };
    st.sleep = pq::engine::SleepMode::On;
    const Outputs on = runBoth(*d.prog, st, std::string(d.what) + " sleep on");
    st.sleep = pq::engine::SleepMode::Off;
    const Outputs off = run(*d.prog, Backend::Jit, st);
    REQUIRE(off.error.empty());
    MESSAGE(std::string(d.what) << ": sleeps " << on.sleep.sleeps << ", wakes " << on.sleep.wakes << ", asleep blocks " << on.sleep.asleepBlocks);
    CHECK_MESSAGE(on.sleep.sleeps >= 2, d.what);  // after the burst, and again after the param change in the silence
    double worst = 0.0;
    for (const char* n : {"y", "y[1]"})
      for (size_t t = 0; t < st.frames; ++t) worst = std::max(worst, std::fabs(static_cast<double>(on.at(n)[0][t]) - off.at(n)[0][t]));
    CHECK_MESSAGE(worst <= 1e-6, d.what << ": sleeping vs not " << worst);
  }
}
