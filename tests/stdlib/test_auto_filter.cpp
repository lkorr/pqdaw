// T4.11: Auto Filter (stdlib/devices/auto_filter.pqp) against its hand-written C++ reference (device_reference.h:
// devref::AutoFilter, every type and circuit model, LFO, follower, sidechain) on the interpreter and the JIT
// (bit-identical to each other, RtScope clean), plus the analytic small-signal responses of every type and model
// (within 0.1 dB), the nonlinear models' measured harmonic profiles, self-oscillation that stays bounded, audio-rate
// cutoff modulation that stays bounded and rings out, LFO shapes, the follower / sidechain and the exact bypasses.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "device_reference.h"
#include "stdlib_harness.h"

using namespace stdlib_test;

namespace {

constexpr float kSr = 48000.0f;
constexpr double kPi = 3.141592653589793;
using Conf = devref::AfRuntime;
using Params = devref::AfParams;
using Ref = devref::AutoFilter<devref::AfRuntime>;

const char* kTypeNames[] = {"LP", "HP", "BP", "Notch", "Morph", "DJ", "Comb", "Resampling", "Notch+LP", "Vowel"};
const char* kCircuitNames[] = {"Clean", "OSR", "MS2", "SMP", "PRD"};

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
// Input: impulse, noise, sweeps (the channels differ). Sidechain: 30 ms tone bursts every 120 ms.
double inputSignal(uint32_t ch, uint64_t t) {
  if (t < 512) return t == 0 ? (ch == 0 ? 0.9 : -0.5) : 0.0;
  if (t < 3072) return 0.5 * (0.6 * noise(t, 7) + 0.4 * noise(t, 11 + ch));
  return ch == 0 ? chirp(t - 3072, 6144, 0.6) : chirp(t - 3072, 9000, 0.45);
}
double scSignal(uint32_t ch, uint64_t t) {
  return (t % 5760) < 1440 ? (ch ? 0.6 : 0.7) * std::sin(2.0 * kPi * 330.0 * static_cast<double>(t) / kSr) : 0.0;
}
// An audio-rate cutoff (the `freq` param driven from an input): ~35 Hz … 0.49·sr, a 3 Hz sweep with a 700 Hz wobble.
double freqMod(uint64_t t) {
  const double s = static_cast<double>(t) / kSr;
  return 1000.0 * std::exp2(3.3 * std::sin(2.0 * kPi * 3.0 * s)) * (1.0 + 0.5 * std::sin(2.0 * kPi * 700.0 * s));
}
constexpr uint32_t kFrames = 3072 + 6144;

std::string afProgramText(const Conf& c, bool freqIn) {
  std::string args = "type: " + std::to_string(c.type) + ", circuit: " + std::to_string(c.circuit) + ", slope: " + std::to_string(c.slope) +
                     ", lfo: " + std::string(c.lfo ? "1" : "0") + ", lfoShape: " + std::to_string(c.lfoShape) +
                     ", env: " + std::string(c.env ? "1" : "0") + ", scOn: " + std::string(c.scOn ? "1" : "0");
  if (freqIn) args += ", freq: m";
  return "import \"devices/auto_filter.pqp\"\npatch T {\n  in x : audio stereo\n  in s : audio stereo\n  in m : audio\n"
         "  out y : audio stereo\n  out cutoff : audio\n  out lfoSignal : audio\n  out envelope : audio\n"
         "  a = auto_filter(x, sc: s, " + args + ")\n  y = a\n  cutoff = a.cutoff\n  lfoSignal = a.lfoSignal\n  envelope = a.envelope\n}\n";
}
Program afProgram(const Conf& c, bool freqIn = false) { return compile(afProgramText(c, freqIn)); }

void setParams(Stimulus& st, const Params& p, bool freqIn = false) {
  if (!freqIn) st.params["a__freq"] = p.freq;
  st.params["a__res"] = p.res;
  st.params["a__drive"] = p.drive;
  st.params["a__morph"] = p.morph;
  st.params["a__dj"] = p.dj;
  st.params["a__lfoAmount"] = p.lfoAmount;
  st.params["a__lfoRate"] = p.lfoRate;
  st.params["a__lfoSync"] = p.lfoSync;
  st.params["a__lfoBeats"] = p.lfoBeats;
  st.params["a__lfoOffset"] = p.lfoOffset;
  st.params["a__lfoPhase"] = p.lfoPhase;
  st.params["a__lfoSpin"] = p.lfoSpin;
  st.params["a__envAmount"] = p.envAmount;
  st.params["a__attack"] = p.attack;
  st.params["a__release"] = p.release;
  st.params["a__scGain"] = p.scGain;
  st.params["a__scMix"] = p.scMix;
  st.params["a__dryWet"] = p.dryWet;
  st.params["a__output"] = p.output;
}

Stimulus afStimulus(uint32_t frames, uint32_t block, std::function<double(uint32_t, uint64_t)> x,
                    std::function<double(uint32_t, uint64_t)> sc = scSignal, std::function<double(uint64_t)> m = freqMod) {
  Stimulus st;
  st.frames = frames;
  st.block = block;
  st.inputCh = [x, sc, m](uint32_t ord, uint32_t ch, uint32_t, uint64_t t) { return ord == 0 ? x(ch, t) : ord == 1 ? sc(ch, t) : m(t); };
  st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
  return st;
}

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
      CHECK_MESSAGE(diffs == 0, label << ": interpreter and JIT differ in '" << name << "' at " << diffs << " samples, first t=" << first);
    }
  }
  return a;
}

std::string label(const Conf& c, const std::string& extra = "") {
  return std::string(kTypeNames[c.type]) + " " + kCircuitNames[c.type > 4 ? 0 : c.circuit] + (c.slope ? " 24" : " 12") +
         (c.lfo ? " lfo" + std::to_string(c.lfoShape) : "") + (c.env ? " env" : "") + (c.scOn ? " sc" : "") + extra;
}

// The device vs the C++ reference over the test signal: max |got − ref| / max(1, |ref|) of every output.
void checkVsReference(const Conf& c, const Params& p, bool freqIn, uint32_t block, double tol = 1e-5) {
  const std::string lb = label(c, (freqIn ? " audio-rate freq" : "") + std::string(" block ") + std::to_string(block));
  const Program prog = afProgram(c, freqIn);
  Stimulus st = afStimulus(kFrames, block, inputSignal);
  setParams(st, p, freqIn);
  const Outputs o = runBoth(prog, st, lb);
  Ref ref;
  ref.conf = c;
  std::map<std::string, std::vector<double>> r;
  const bool stereoCut = c.lfo;
  for (uint64_t t = 0; t < st.frames; ++t) {
    Params q = p;
    if (freqIn) q.freq = static_cast<float>(freqMod(t));
    ref.prepare(q);
    const float x[2] = {static_cast<float>(inputSignal(0, t)), static_cast<float>(inputSignal(1, t))};
    const float s[2] = {static_cast<float>(scSignal(0, t)), static_cast<float>(scSignal(1, t))};
    ref.process(x, s);
    r["y"].push_back(ref.out[0]);
    r["y[1]"].push_back(ref.out[1]);
    r["cutoff"].push_back(ref.cutoff[0]);
    if (stereoCut) r["cutoff[1]"].push_back(ref.cutoff[1]);
    if (c.lfo && c.lfoShape != 5) {
      r["lfoSignal"].push_back(ref.lfo[0]);
      r["lfoSignal[1]"].push_back(ref.lfo[1]);
    }
    if (c.env) r["envelope"].push_back(ref.envelope);
  }
  double worst = 0.0;
  std::string where;
  for (const auto& [name, v] : r) {
    const auto& got = o.at(name)[0];
    REQUIRE(got.size() == v.size());
    for (size_t t = 0; t < v.size(); ++t) {
      const double e = std::isfinite(got[t]) ? std::fabs(got[t] - v[t]) / std::max(1.0, std::fabs(v[t])) : INFINITY;
      if (e > worst) {
        worst = e;
        where = name + " t=" + std::to_string(t) + " got " + std::to_string(got[t]) + " ref " + std::to_string(v[t]);
      }
    }
  }
  CHECK_MESSAGE(worst <= tol, lb << ": max error " << worst << " (" << where << ")");
}

Conf conf(int type, int circuit, int slope, bool lfo = false, int shape = 0, bool env = false, bool scOn = false) {
  Conf c;
  c.type = type;
  c.circuit = circuit;
  c.slope = slope;
  c.lfo = lfo;
  c.lfoShape = shape;
  c.env = env;
  c.scOn = scOn;
  return c;
}

// A param set with the modulators active (they only act where the configuration turns them on).
Params modParams(float res, float drive = 1.0f) {
  Params p;
  p.freq = 900.0f;
  p.res = res;
  p.drive = drive;
  p.morph = 0.35f;
  p.dj = -0.45f;
  p.lfoAmount = 0.55f;
  p.lfoRate = 7.0f;
  p.lfoOffset = 0.2f;
  p.lfoPhase = 0.25f;
  p.lfoSpin = 0.1f;
  p.lfoBeats = 0.5f;
  p.envAmount = 0.6f;
  p.attack = 0.002f;
  p.release = 0.08f;
  p.scGain = 1.6f;
  p.scMix = 0.7f;
  p.dryWet = 0.85f;
  p.output = 0.8f;
  return p;
}

}  // namespace

TEST_CASE("Auto Filter: types, circuits, LFO, follower and sidechain vs the C++ reference, interpreter == JIT") {
  struct Case {
    Conf c;
    Params p;
    bool freqIn;
    uint32_t block;
  };
  Params djRight = modParams(0.7f);
  djRight.dj = 0.4f;
  Params sync = modParams(0.6f);
  sync.lfoSync = true;
  const Case cases[] = {
      {conf(0, 0, 0), modParams(0.5f), false, 64},
      {conf(1, 0, 1, true, 0), modParams(0.9f), false, 37},
      {conf(2, 0, 0, false, 0, true, true), modParams(0.7f), false, 64},
      {conf(3, 0, 1, true, 1, true), modParams(0.4f), false, 64},
      {conf(4, 0, 0, true, 3), sync, false, 64},
      {conf(5, 0, 1, true, 4), modParams(0.6f), false, 64},
      {conf(5, 0, 0), djRight, false, 37},
      {conf(0, 1, 0, true, 2), modParams(1.1f, 2.0f), false, 64},
      {conf(2, 1, 1), modParams(0.7f, 4.0f), false, 64},
      {conf(0, 2, 1, true, 0), modParams(0.95f, 3.0f), false, 64},
      {conf(1, 3, 0), modParams(0.8f, 5.0f), false, 37},
      {conf(0, 4, 1, true, 0), modParams(0.9f, 2.0f), false, 64},
      {conf(4, 4, 0, false, 0, true), modParams(0.6f, 8.0f), false, 64},
      {conf(6, 0, 0, true, 0), modParams(1.0f), false, 64},
      {conf(7, 0, 0, true, 1), modParams(0.5f), false, 64},
      {conf(8, 0, 0), modParams(0.8f), false, 64},
      {conf(9, 0, 0, true, 0), modParams(0.6f), false, 64},
      {conf(0, 0, 1), modParams(1.0f), true, 64},
      {conf(0, 4, 1), modParams(0.9f, 3.0f), true, 64},
      {conf(2, 2, 0), modParams(0.8f, 2.0f), true, 37},
  };
  for (const Case& k : cases) checkVsReference(k.c, k.p, k.freqIn, k.block);
}

TEST_CASE("Auto Filter: every type x circuit x slope vs the C++ reference" * doctest::test_suite("slow")) {
  for (int type = 0; type < 10; ++type)
    for (int circuit = 0; circuit < 5; ++circuit)
      for (int slope = 0; slope < 2; ++slope) {
        if (type > 4 && circuit > 0) continue;
        if (type > 5 && slope > 0) continue;
        checkVsReference(conf(type, circuit, slope, true, (type + circuit) % 5, true, slope == 1), modParams(0.85f, 3.0f), false, 64);
        checkVsReference(conf(type, circuit, slope), modParams(0.3f + 0.2f * static_cast<float>(circuit), 1.5f), true, 37);
      }
}

// ================================================================================================================
// Small-signal responses
// ================================================================================================================
namespace {

double magDb(const std::vector<float>& ir, double f, double scale) {
  std::complex<double> acc = 0.0, rot = 1.0;
  const std::complex<double> step = std::polar(1.0, -2.0 * kPi * f / kSr);
  for (float v : ir) {
    acc += static_cast<double>(v) * rot;
    rot *= step;
  }
  return 20.0 * std::log10(std::max(std::abs(acc) / scale, 1e-30));
}
double db(std::complex<double> h) { return 20.0 * std::log10(std::max(std::abs(h), 1e-30)); }
std::complex<double> sAt(double f, double fc) {
  const double fcc = std::clamp(fc, 10.0, 0.49 * kSr);
  return {0.0, std::tan(kPi * f / kSr) / std::tan(kPi * fcc / kSr)};
}

// Clean's damping (af_kclean, M4 review M12): √2·(1 − res) up to 90 %, then exponentially down to 0.02 at 125 %.
double kCleanD(double res) { return res < 0.9 ? std::sqrt(2.0) * (1.0 - res) : 0.1414213562373095 * std::exp(-5.545177444479562 * (res - 0.9)); }

// The analytic response of the device at small signal levels (linear: tanh(v) ≈ v, below the OSR diode's knee).
std::complex<double> analytic(const Conf& c, const Params& p, double f) {
  using C = std::complex<double>;
  const int type = c.type;
  const double pm = 4.0 * p.morph;
  const double hLP = std::max(0.0, 1.0 - std::fabs(pm)) + std::max(0.0, pm - 3.0), hBP = std::max(0.0, 1.0 - std::fabs(pm - 1.0));
  const double hHP = std::max(0.0, 1.0 - std::fabs(pm - 2.0)), hN = std::max(0.0, 1.0 - std::fabs(pm - 3.0));
  const double djHP = p.dj > 0 ? 1.0 : 0.0;
  const double wv[6][4] = {{1, 0, 0, 0}, {0, 0, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}, {hLP, hBP, hHP, hN}, {1 - djHP, 0, djHP, 0}};
  double base = p.freq;
  if (type == 5) base = p.dj < 0 ? 20000.0 * std::exp(6.684611727667927 * p.dj) : 15.0 * std::exp(6.907755278982137 * p.dj);
  const C s = sAt(f, base);
  if (type <= 5) {
    const double wLP = wv[type][0], wBP = wv[type][1], wHP = wv[type][2], wN = wv[type][3], hn = wHP + wN;
    const int model = type > 4 ? 0 : c.circuit;
    C h;
    if (model == 4) {
      const double k = 4.0 * p.res;
      const C G = 1.0 / (1.0 + s);
      double cc[5];
      if (c.slope) {
        cc[0] = hn, cc[1] = -4 * hn, cc[2] = 4 * wBP + 6 * wHP + 8 * wN, cc[3] = -8 * wBP - 4 * wHP - 8 * wN, cc[4] = wLP + 4 * wBP + wHP + 4 * wN;
      } else {
        cc[0] = hn, cc[1] = 2 * wBP - 2 * hn, cc[2] = wLP - 2 * wBP + wHP + 2 * wN, cc[3] = 0, cc[4] = 0;
      }
      C num = 0.0, Gn = 1.0;
      for (double ci : cc) {
        num += ci * Gn;
        Gn *= G;
      }
      num += k * (wLP + wN) * G * G * G * G;  // the passband compensation (M4 review M12)
      h = num / (1.0 + k * G * G * G * G);
    } else {
      const bool clean = model == 0;
      const double k12 = clean ? kCleanD(p.res) : std::sqrt(2.0) * (1.0 - p.res);
      const double kB = c.slope ? 0.541196100146197 * k12 : k12;
      const double kbB = clean ? kB : std::max(kB, 0.25);
      auto sec = [&](double k, double kb) {
        const C D = s * s + k * s + 1.0;
        return (hn * D + (kb * wBP - k * hn) * s + (wLP - wHP)) / D;
      };
      h = sec(kB, kbB);
      if (c.slope) h *= sec(1.8477590650225735, 1.8477590650225735);
    }
    if (type == 5) h = 1.0 + std::min(1.0, 20.0 * std::fabs(p.dj)) * (h - 1.0);
    return h;
  }
  if (type == 6) {
    const double fb = 0.792 * p.res, T = kSr / std::clamp(base, 20.0, 10000.0);
    return std::sqrt(1.0 - fb * fb) / (1.0 - fb * std::polar(1.0, -2.0 * kPi * f / kSr * T));
  }
  if (type == 8) {
    const double k = kCleanD(p.res);
    const C notch = (s * s + 1.0) / (s * s + k * s + 1.0);
    const C s2 = sAt(f, std::min(2.0 * base, 0.49 * kSr));
    return notch / (s2 * s2 + std::sqrt(2.0) * s2 + 1.0);
  }
  // Vowel
  const double q4 = 4.0 * p.morph;
  const int i = static_cast<int>(std::min(std::floor(q4), 3.0));
  const double fr = q4 - i;
  const double F[3][5] = {{730, 530, 270, 570, 300}, {1090, 1840, 2290, 840, 870}, {2440, 2480, 3010, 2410, 2240}};
  const double k = 1.0 / (3.0 + 9.0 * p.res), sh = std::sqrt(base * 0.001), gains[3] = {1.0, 0.5, 0.25};
  C h = 0.0;
  for (int j = 0; j < 3; ++j) {
    const double fj = std::clamp((F[j][i] + fr * (F[j][i + 1] - F[j][i])) * sh, 20.0, 0.45 * kSr);
    const C sj = sAt(f, fj);
    h += gains[j] * k * sj / (sj * sj + k * sj + 1.0);
  }
  return h;
}

std::vector<double> logFreqs(double lo, double hi, int n) {
  std::vector<double> f;
  for (int i = 0; i < n; ++i) f.push_back(lo * std::pow(hi / lo, static_cast<double>(i) / (n - 1)));
  return f;
}

// The impulse response (amplitude `amp`, so the nonlinear models stay in their linear range) vs analytic().
double responseError(const Conf& c, const Params& p, double amp, double* atHz = nullptr) {
  constexpr uint32_t kLen = 32768;
  const Program prog = afProgram(c);
  Stimulus st = afStimulus(kLen, 64, [amp](uint32_t, uint64_t t) { return t == 0 ? amp : 0.0; });
  setParams(st, p);
  // Sleep off for this measurement only (T1.x-silence, Q130): a silent-input segment's state is flushed once it is
  // ≤ 2^-30 (≈ −180 dBFS absolute). The nonlinear models are measured at −100 dBFS (1e-5, to stay linear), where
  // that cut is only 80 dB below the impulse, and a 24 dB model's high-Q section after the Clean pre-section (which
  // sleeps on its own) lifts the truncation into the 0.1 dB budget at deep notch / stopband points. That is the
  // engine's documented −180 dBFS approximation, not the filter's response; the test below holds the sleeping
  // device to the non-sleeping one at full scale.
  st.sleep = pq::engine::SleepMode::Off;
  const Outputs o = run(prog, Backend::Jit, st);
  REQUIRE_MESSAGE(o.error.empty(), o.error);
  double worst = 0.0;
  for (const double f : logFreqs(20.0, 20000.0, 47)) {
    const double want = db(analytic(c, p, f));
    if (want < -60.0) continue;  // below: float32 noise of the impulse response
    const double e = std::fabs(magDb(o.at("y")[0], f, amp) - want);
    if (e > worst) {
      worst = e;
      if (atHz) *atHz = f;
    }
  }
  return worst;
}

}  // namespace

static void responses(bool full) {
  struct Setting {
    float freq, res;
  };
  const Setting settings[] = {{1000.0f, 0.5f}, {150.0f, 0.9f}, {6000.0f, 0.2f}};
  for (int circuit = 0; circuit < 5; ++circuit)
    for (int type = 0; type < 5; ++type)
      for (int slope = 0; slope < 2; ++slope)
        for (int si = 0; si < (full ? 3 : 1); ++si) {
          if (!full && slope != (type + circuit) % 2) continue;  // default tier: one slope per model and type
          Params p;
          p.freq = settings[(si + circuit) % 3].freq;
          p.res = settings[(si + circuit) % 3].res;
          p.morph = 0.15f + 0.2f * static_cast<float>(si + slope);
          const Conf c = conf(type, circuit, slope);
          double at = 0.0;
          const double e = responseError(c, p, circuit == 0 ? 1.0 : 1e-5, &at);
          CHECK_MESSAGE(e < 0.1, label(c) << " freq " << p.freq << " res " << p.res << " morph " << p.morph << ": max deviation " << e
                                          << " dB at " << at << " Hz");
        }
  // The Clean-only types.
  struct Other {
    int type, slope;
    float freq, res, morph, dj;
  };
  const Other others[] = {{5, 0, 0, 0.5f, 0, -0.3f}, {5, 1, 0, 0.8f, 0, 0.6f},   {5, 0, 0, 0.3f, 0, 0.02f}, {6, 0, 480.0f, 0.9f, 0, 0},
                          {6, 0, 2400.0f, 1.2f, 0, 0}, {8, 0, 800.0f, 0.7f, 0, 0}, {9, 0, 1000.0f, 0.5f, 0.0f, 0}, {9, 0, 1300.0f, 1.0f, 0.62f, 0}};
  for (const Other& k : others) {
    Params p;
    p.freq = k.freq > 0 ? k.freq : 1000.0f;
    p.res = k.res;
    p.morph = k.morph;
    p.dj = k.dj;
    const Conf c = conf(k.type, 0, k.slope);
    double at = 0.0;
    const double e = responseError(c, p, 1.0, &at);
    CHECK_MESSAGE(e < 0.1, label(c) << " freq " << p.freq << " res " << p.res << " dj " << p.dj << ": max deviation " << e << " dB at " << at << " Hz");
  }
}

TEST_CASE("Auto Filter: small-signal responses of every type and circuit model vs the analytic responses within 0.1 dB") { responses(false); }
TEST_CASE("Auto Filter: small-signal responses, every type x model x slope x 3 settings" * doctest::test_suite("slow")) { responses(true); }

TEST_CASE("Auto Filter: sleeping (T1.x-silence) only cuts tails below -160 dBFS; a ringing filter is never cut") {
  // A full-scale click into each model at 12 and 24 dB, resonance 90 % at 150 Hz (a long tail), 0.7 s: the image
  // that sleeps against one that never does. Measured: Clean segments sleep once their state is ≤ 2^-30 (the last
  // samples they would have played are ≈ 1e-9); a 24 dB nonlinear model's Clean pre-section sleeps under its still
  // ringing second section, which then misses ≤ 2.8e-9 of input; the solve / OSR sections themselves never sleep.
  for (int circuit = 0; circuit < 5; ++circuit)
    for (int slope = 0; slope < 2; ++slope)
      for (const int type : {0, 1, 3}) {
        Params p;
        p.freq = 150.0f;
        p.res = 0.9f;
        const Conf c = conf(type, circuit, slope);
        const Program prog = afProgram(c);
        Stimulus st = afStimulus(32768, 64, [](uint32_t, uint64_t t) { return t == 0 ? 1.0 : 0.0; });
        setParams(st, p);
        const Outputs on = run(prog, Backend::Jit, st);
        st.sleep = pq::engine::SleepMode::Off;
        const Outputs off = run(prog, Backend::Jit, st);
        REQUIRE(on.error.empty());
        REQUIRE(off.error.empty());
        double d = 0.0;
        for (const char* n : {"y", "y[1]"})
          for (size_t t = 0; t < on.at(n)[0].size(); ++t) d = std::max(d, std::fabs(static_cast<double>(on.at(n)[0][t]) - off.at(n)[0][t]));
        CHECK_MESSAGE(d < 1e-8, label(c) << ": sleeping vs not, max difference " << d);
      }
}

TEST_CASE("Auto Filter: DJ centre and dry/wet 0 pass the input exactly") {
  Params p = modParams(1.0f, 4.0f);
  p.dj = 0.0f;
  const Program dj = afProgram(conf(5, 0, 1, true, 0, true));
  Stimulus st = afStimulus(4096, 64, inputSignal);
  setParams(st, p);
  const Outputs o = runBoth(dj, st, "dj centre");
  p.output = 1.0f;
  setParams(st, p);
  const Outputs o1 = runBoth(dj, st, "dj centre");
  for (uint32_t t = 0; t < st.frames; ++t) {
    CHECK(o1.at("y")[0][t] == static_cast<float>(static_cast<float>(inputSignal(0, t))));
    CHECK(o1.at("y[1]")[0][t] == static_cast<float>(inputSignal(1, t)));
  }
  Params q = modParams(1.2f, 10.0f);
  q.dryWet = 0.0f;
  q.output = 1.0f;
  const Program prd = afProgram(conf(0, 4, 1, true, 0));
  setParams(st, q);
  const Outputs w = runBoth(prd, st, "dry");
  for (uint32_t t = 0; t < st.frames; ++t) CHECK(w.at("y")[0][t] == static_cast<float>(inputSignal(0, t)));
}

// ================================================================================================================
// Nonlinear behaviour
// ================================================================================================================
namespace {

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

// Harmonics 2 … 7 of a bin-exact sine through `c` (dB relative to the fundamental).
std::vector<double> harmonics(const Conf& c, const Params& p, double amp, uint32_t k0 = 75) {
  constexpr size_t kN = 16384, kSkip = 16384;
  const Program prog = afProgram(c);
  Stimulus st = afStimulus(static_cast<uint32_t>(kSkip + kN), 64, [amp, k0](uint32_t, uint64_t t) {
    return amp * std::sin(2.0 * kPi * static_cast<double>(k0) * static_cast<double>(t) / kN);
  });
  setParams(st, p);
  const Outputs o = run(prog, Backend::Jit, st);
  REQUIRE_MESSAGE(o.error.empty(), o.error);
  std::vector<std::complex<double>> a(kN);
  for (size_t t = 0; t < kN; ++t) a[t] = o.at("y")[0][kSkip + t];
  fft(a);
  std::vector<double> h;
  const double f1 = std::abs(a[k0]);
  for (uint32_t n = 2; n <= 7; ++n) h.push_back(20.0 * std::log10(std::max(std::abs(a[n * k0]) / f1, 1e-12)));
  return h;
}

std::string fmt(const std::vector<double>& h) {
  std::string s;
  for (size_t i = 0; i < h.size(); ++i) {
    char b[32];
    std::snprintf(b, sizeof b, " H%zu %.1f", i + 2, h[i]);
    s += b;
  }
  return s;
}

}  // namespace

TEST_CASE("Auto Filter: harmonic profiles of the circuit models (lowpass 12 dB, 220 Hz at -6 dBFS)") {
  // A 219.7 Hz sine at amplitude 0.5 through the 12 dB lowpass at 2 kHz, resonance 50 %, drive 0 / +12 / +24 dB.
  // Clean is linear; OSR, MS2 and PRD are symmetric (odd harmonics only), SMP's biased feedback adds even ones.
  double h3[5][3] = {};
  for (int circuit = 0; circuit < 5; ++circuit)
    for (int di = 0; di < 3; ++di) {
      Params p;
      p.freq = 2000.0f;
      p.res = 0.5f;
      p.drive = static_cast<float>(std::pow(10.0, 12.0 * di / 20.0));
      const std::vector<double> h = harmonics(conf(0, circuit, 0), p, 0.5);
      MESSAGE(std::string(kCircuitNames[circuit]) << " drive +" << 12 * di << " dB:" << fmt(h));
      h3[circuit][di] = h[1];
      if (circuit == 0) {
        for (double v : h) CHECK(v < -100.0);
        continue;
      }
      if (circuit != 3)
        for (size_t i = 0; i < h.size(); i += 2)
          CHECK_MESSAGE(h[i] < -90.0, std::string(kCircuitNames[circuit]) << ": even harmonic H" << i + 2 << " " << h[i]);
      if (circuit == 3 && di > 0) CHECK_MESSAGE(h[0] > -60.0, "SMP H2 " << h[0]);
      if (di == 2) CHECK_MESSAGE(h[1] > -50.0, std::string(kCircuitNames[circuit]) << ": H3 at +24 dB " << h[1]);
    }
  // More drive, more distortion.
  for (int circuit = 1; circuit < 5; ++circuit) CHECK_MESSAGE(h3[circuit][2] > h3[circuit][0] + 10.0, std::string(kCircuitNames[circuit]));
}

TEST_CASE("Auto Filter: the nonlinear models' Newton iterations are close to the converged solution") {
  // Driven hard (+24 dB, resonance 90 %, 24 dB, the test signal): the device (MS2 / SMP 2 iterations, PRD 1) against
  // the reference iterated to convergence (30 iterations). Measured (reference, 1.5 × the test signal, drive +12 /
  // +24 dB, resonance 50 / 90 %): MS2 / SMP 2 iterations −120 … −132 dB re peak (1 iteration: −46 … −66 dB),
  // PRD 1 iteration −127 … −131 dB. Self-oscillating settings (resonance > 100 %) are a limit cycle whose phase
  // drifts with any rounding difference, so no iteration count compares there.
  for (const int circuit : {2, 3, 4}) {
    Params p = modParams(0.9f, 15.85f);
    const Conf c = conf(0, circuit, 1, true, 0);
    const Program prog = afProgram(c);
    Stimulus st = afStimulus(kFrames, 64, inputSignal);
    setParams(st, p);
    const Outputs o = run(prog, Backend::Jit, st);
    REQUIRE(o.error.empty());
    Conf c8 = c;
    c8.iterations = 30;
    c8.ladderIterations = 30;
    Ref ref;
    ref.conf = c8;
    ref.prepare(p);
    double worst = 0.0, pk = 0.0;
    for (uint64_t t = 0; t < kFrames; ++t) {
      const float x[2] = {static_cast<float>(inputSignal(0, t)), static_cast<float>(inputSignal(1, t))};
      const float s[2] = {0, 0};
      ref.process(x, s);
      worst = std::max(worst, std::fabs(static_cast<double>(o.at("y")[0][t]) - ref.out[0]));
      pk = std::max(pk, std::fabs(static_cast<double>(ref.out[0])));
    }
    const double rel = 20.0 * std::log10(std::max(worst, 1e-12) / pk);
    MESSAGE(std::string(kCircuitNames[circuit]) << ": the device vs 30 Newton iterations, max difference " << worst << " (" << rel
                                                << " dB re peak " << pk << ")");
    CHECK(rel < -80.0);
  }
}

// Resonance 125 %, a click, then 1 s of silence (default tier: 12 dB on the JIT; slow: both slopes, both backends).
static void selfOscillation(bool full) {
  for (int circuit = 0; circuit < 5; ++circuit)
    for (int slope = 0; slope < (full ? 2 : 1); ++slope) {
      Params p;
      p.freq = 700.0f;
      p.res = 1.25f;
      const Conf c = conf(0, circuit, slope);
      const Program prog = afProgram(c);
      Stimulus st = afStimulus(48000, 64, [](uint32_t, uint64_t t) { return t < 4 ? 0.1 : 0.0; });
      setParams(st, p);
      const Outputs o = full ? runBoth(prog, st, label(c, " self-oscillation")) : run(prog, Backend::Jit, st);
      REQUIRE(o.error.empty());
      const auto& y = o.at("y")[0];
      double pk = 0.0, tail = 0.0;
      for (size_t t = 0; t < y.size(); ++t) {
        pk = std::max(pk, std::fabs(static_cast<double>(y[t])));
        if (t >= 36000) tail = std::max(tail, std::fabs(static_cast<double>(y[t])));
      }
      MESSAGE(label(c) << " at resonance 125 %: peak " << pk << ", last 250 ms peak " << tail);
      CHECK(std::isfinite(pk));
      if (circuit == 0) {
        CHECK(tail < 1e-4);
      } else {
        CHECK(tail > 0.05);  // oscillates on its own
        CHECK(pk < 4.0);     // bounded by the model's nonlinearity
      }
    }
}

TEST_CASE("Auto Filter: self-oscillation of the nonlinear models is bounded; Clean rings out") { selfOscillation(false); }
TEST_CASE("Auto Filter: self-oscillation, both slopes, interpreter == JIT" * doctest::test_suite("slow")) { selfOscillation(true); }

// The cutoff driven at audio rate (35 Hz … 0.49·sr with a 700 Hz wobble) over 0.5 s of noise, then 0.5 s of
// silence; resonance 70 %, every model, 12 and 24 dB, lowpass and bandpass; interpreter == JIT throughout (default
// tier: one type and slope per model).
static void audioRateCutoff(bool full) {
  for (int circuit = 0; circuit < 5; ++circuit)
    for (const int type : {0, 2})
      for (int slope = 0; slope < 2; ++slope) {
        if (!full && (type == 2) != (circuit % 2 == 1)) continue;
        if (!full && slope != (circuit / 2) % 2) continue;
        Params p;
        p.res = 0.7f;
        p.drive = 2.0f;
        const Conf c = conf(type, circuit, slope);
        const Program prog = afProgram(c, true);
        Stimulus st = afStimulus(48000, 64, [](uint32_t ch, uint64_t t) { return t < 24000 ? 0.5 * noise(t, 3 + ch) : 0.0; });
        setParams(st, p, true);
        const Outputs o = runBoth(prog, st, label(c, " audio-rate freq"));
        double pk = 0.0, tail = 0.0;
        bool finite = true;
        for (const char* n : {"y", "y[1]"}) {
          const auto& y = o.at(n)[0];
          for (size_t t = 0; t < y.size(); ++t) {
            finite &= std::isfinite(y[t]);
            pk = std::max(pk, std::fabs(static_cast<double>(y[t])));
            if (t >= 43200) tail = std::max(tail, std::fabs(static_cast<double>(y[t])));
          }
        }
        MESSAGE(label(c) << " audio-rate cutoff: peak " << pk << ", last 100 ms " << tail);
        CHECK(finite);
        CHECK(pk < 6.0);
        CHECK(tail < 1e-4);
      }
}

TEST_CASE("Auto Filter: audio-rate cutoff modulation stays finite and bounded, and rings out") { audioRateCutoff(false); }
TEST_CASE("Auto Filter: audio-rate cutoff modulation, every model x type x slope" * doctest::test_suite("slow")) { audioRateCutoff(true); }

namespace {

// The ANALOG 24 dB Clean lowpass (two SVF sections, bp' = ω(in − k·bp − lp), lp' = ω·bp) along a cutoff trajectory,
// integrated in double (RK4, 16 steps per sample, ω and the input interpolated linearly between samples), with
// ω = 2·sr·tan(π·fc/sr): the analog filter the bilinear (TPT) design maps to at each cutoff.
std::vector<double> analogLp24(const std::vector<float>& fc, double kB, const std::function<double(double)>& x) {
  const size_t n = fc.size();
  const double kA = 1.8477590650225735;
  std::vector<double> out(n);
  double st[2][2] = {};
  constexpr int kSteps = 16;
  const double dt = 1.0 / kSteps;
  auto omega = [&](double tt) {
    const size_t i = std::min(static_cast<size_t>(tt), n - 1), j = std::min(i + 1, n - 1);
    const double u = tt - static_cast<double>(i);
    const double f = fc[i] + u * (static_cast<double>(fc[j]) - fc[i]);
    return 2.0 * std::tan(kPi * f / kSr);  // per sample
  };
  auto deriv = [&](double tt, const double (&s)[2][2], double (&d)[2][2]) {
    const double w = omega(tt);
    double in = x(tt);
    const double k[2] = {kA, kB};
    for (int i = 0; i < 2; ++i) {
      d[i][0] = w * (in - k[i] * s[i][0] - s[i][1]);
      d[i][1] = w * s[i][0];
      in = s[i][1];
    }
  };
  for (size_t t = 0; t < n; ++t) {
    out[t] = st[1][1];
    for (int step = 0; step < kSteps; ++step) {
      const double tt = static_cast<double>(t) + step * dt;
      double k1[2][2], k2[2][2], k3[2][2], k4[2][2], tmp[2][2];
      deriv(tt, st, k1);
      for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) tmp[i][j] = st[i][j] + 0.5 * dt * k1[i][j];
      deriv(tt + 0.5 * dt, tmp, k2);
      for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) tmp[i][j] = st[i][j] + 0.5 * dt * k2[i][j];
      deriv(tt + 0.5 * dt, tmp, k3);
      for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) tmp[i][j] = st[i][j] + dt * k3[i][j];
      deriv(tt + dt, tmp, k4);
      for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) st[i][j] += dt / 6.0 * (k1[i][j] + 2.0 * k2[i][j] + 2.0 * k3[i][j] + k4[i][j]);
    }
  }
  return out;
}

}  // namespace

TEST_CASE("Auto Filter: square-LFO cutoff jumps click no more than the analog filter does") {
  // A 110 Hz sine through the 24 dB lowpass at 800 Hz, resonance 50 %, the cutoff jumping ±2 octaves 4 times a
  // second (square LFO, de-clicked by a 0.5 ms one-pole). The same cutoff trajectory (the `cutoff` export) drives the
  // analog filter: the Clean filter's largest sample-to-sample step stays within 10 % of the analog filter's, and its
  // output within −40 dB of the analog output (the bilinear transform's warping at 110 Hz is far smaller). OSR and
  // PRD (nonlinear, other topologies) stay within 1.5 × the Clean filter's largest step.
  Params p;
  p.freq = 800.0f;
  p.res = 0.5f;
  p.lfoAmount = 0.5f;
  p.lfoRate = 2.0f;
  auto sine = [](double tt) { return 0.5 * std::sin(2.0 * kPi * 110.0 * tt / kSr); };
  double cleanStep = 0.0;
  for (const int circuit : {0, 1, 4}) {
    const Conf c = conf(0, circuit, 1, true, 4);
    const Program prog = afProgram(c);
    Stimulus st = afStimulus(48000, 64, [&](uint32_t, uint64_t t) { return sine(static_cast<double>(t)); });
    setParams(st, p);
    const Outputs o = run(prog, Backend::Jit, st);
    REQUIRE(o.error.empty());
    const auto& y = o.at("y")[0];
    double maxStep = 0.0;
    for (size_t t = 4800; t < y.size(); ++t) maxStep = std::max(maxStep, std::fabs(static_cast<double>(y[t]) - y[t - 1]));
    if (circuit == 0) {
      const double kB = 0.541196100146197 * kCleanD(0.5);
      const std::vector<double> a = analogLp24(o.at("cutoff")[0], kB, sine);
      double aStep = 0.0, err = 0.0, pk = 0.0;
      for (size_t t = 4800; t < y.size(); ++t) {
        aStep = std::max(aStep, std::fabs(a[t] - a[t - 1]));
        err = std::max(err, std::fabs(y[t] - a[t]));
        pk = std::max(pk, std::fabs(a[t]));
      }
      MESSAGE("Clean: largest step " << maxStep << ", analog " << aStep << "; max difference " << 20.0 * std::log10(err / pk) << " dB re peak");
      CHECK(maxStep < 1.1 * aStep);
      CHECK(20.0 * std::log10(err / pk) < -40.0);
      cleanStep = maxStep;
    } else {
      MESSAGE(std::string(kCircuitNames[circuit]) << ": largest step " << maxStep << " (Clean " << cleanStep << ")");
      CHECK(maxStep < 1.5 * cleanStep);
    }
  }
}

// ================================================================================================================
// LFO, follower, sidechain
// ================================================================================================================
TEST_CASE("Auto Filter: LFO shapes, offset, stereo phase, spin and sync") {
  auto runLfo = [](int shape, const Params& p) {
    const Program prog = afProgram(conf(0, 0, 0, true, shape));
    Stimulus st = afStimulus(48000, 64, [](uint32_t, uint64_t) { return 0.0; });
    setParams(st, p);
    Outputs o = run(prog, Backend::Jit, st);
    REQUIRE(o.error.empty());
    return o;
  };
  auto frac = [](double x) { return x - std::floor(x); };
  auto shapeOf = [&](int shape, double ph) {
    switch (shape) {
      case 0: return std::sin(2.0 * kPi * ph);
      case 1: return 4.0 * std::fabs(frac(ph + 0.75) - 0.5) - 1.0;
      case 2: return 2.0 * frac(ph + 0.5) - 1.0;
      case 3: return 1.0 - 2.0 * frac(ph + 0.5);
      default: return ph < 0.5 ? 1.0 : -1.0;
    }
  };
  Params p;
  p.lfoRate = 3.0f;
  p.lfoOffset = 0.1f;
  p.lfoPhase = 0.25f;
  p.lfoSpin = 0.2f;
  // The expected signal: the formula, through the device's 0.5 ms one-pole for the shapes with jumps (saws, square).
  const double c = 1.0 - std::exp(-1.0 / (0.0005 * kSr));
  auto expected = [&](int shape, const std::function<double(uint64_t)>& phase) {
    std::vector<double> e(48000);
    double y = 0.0;
    for (uint64_t t = 0; t < e.size(); ++t) {
      const double v = shapeOf(shape, phase(t));
      y += (v - y) * c;
      e[t] = shape >= 2 ? y : v;
    }
    return e;
  };
  // Compared everywhere except for 200 samples after each jump (a one-sample difference in where a jump falls, from
  // the phasor's fixed-point phase, decays with the smoother's time constant).
  auto compare = [&](const std::vector<float>& got, const std::vector<double>& want, const std::function<double(uint64_t)>& phase) {
    size_t bad = 0;
    uint64_t lastJump = 0;
    bool jumped = false;
    for (uint64_t t = 1; t < want.size(); ++t) {
      const double a = phase(t - 1), b = phase(t);
      if (b < a || (a < 0.5 && b >= 0.5)) {  // the saws jump at phase 0.5, the square at 0 and 0.5
        lastJump = t;
        jumped = true;
      }
      if (jumped && t - lastJump < 200) continue;
      bad += std::fabs(got[t] - want[t]) > 2e-4;
    }
    return bad;
  };
  for (int shape = 0; shape < 5; ++shape) {
    const Outputs o = runLfo(shape, p);
    const auto pl = [&](uint64_t t) { return frac(3.0 * static_cast<double>(t) / kSr + 0.1); };
    const auto pr = [&](uint64_t t) { return frac(3.6 * static_cast<double>(t) / kSr + 0.35); };
    const size_t bad = compare(o.at("lfoSignal")[0], expected(shape, pl), pl) + compare(o.at("lfoSignal[1]")[0], expected(shape, pr), pr);
    CHECK_MESSAGE(bad == 0, "shape " << shape << ": " << bad << " samples off the formula");
  }
  // Sync: one cycle per lfoBeats beats of the song (120 BPM: a beat is 24000 samples), whatever the rate.
  Params q;
  q.lfoSync = true;
  q.lfoBeats = 0.5f;
  q.lfoRate = 13.0f;
  const Outputs o = runLfo(2, q);
  const auto ps = [&](uint64_t t) { return frac(static_cast<double>(t) / 24000.0 / 0.5); };
  CHECK(compare(o.at("lfoSignal")[0], expected(2, ps), ps) == 0);
  // Random: a new value from the first sample and at each cycle start (smoothed over ~5 ms), held for the rest of the
  // cycle, within ±1, independent per channel.
  Params r;
  r.lfoRate = 10.0f;
  const Outputs n = runLfo(5, r);
  std::set<float> held;
  size_t sameLR = 0, moving = 0;
  for (uint64_t t = 1; t < 48000; ++t) {
    const float a = n.at("lfoSignal")[0][t], b = n.at("lfoSignal")[0][t - 1];
    CHECK(std::fabs(a) <= 1.0f);
    const double ph = frac(10.0 * static_cast<double>(t) / kSr);
    if (ph > 0.1) {  // 10 ms after the cycle start: settled
      moving += std::fabs(a - b) > 1e-6f;
      held.insert(a);
      sameLR += a == n.at("lfoSignal[1]")[0][t];
    }
  }
  CHECK(moving == 0);
  CHECK(held.size() >= 10);
  CHECK(held.size() <= 10);
  CHECK(sameLR == 0);
}

TEST_CASE("Auto Filter: the reference's block path (the benchmark's C++ side) computes what its per-sample path does") {
  for (const Conf& c : {conf(0, 0, 0, true, 0), conf(0, 0, 1, true, 1, true), conf(4, 0, 0, true, 2, true), conf(2, 3, 0, true, 4),
                        conf(0, 4, 1, true, 3, true), conf(6, 0, 0, true, 0), conf(9, 0, 0, true, 0), conf(0, 2, 0)}) {
    Params p = modParams(0.7f, 2.0f);
    Ref a, b;
    a.conf = b.conf = c;
    double worst = 0.0;
    std::vector<float> xl(64), xr(64), outB(128);
    for (uint64_t t0 = 0; t0 < 4096; t0 += 64) {
      a.prepare(p);
      b.prepare(p);
      for (uint32_t i = 0; i < 64; ++i) {
        xl[i] = static_cast<float>(inputSignal(0, t0 + i));
        xr[i] = static_cast<float>(inputSignal(1, t0 + i));
      }
      b.processBlock(xl.data(), xr.data(), 64, outB.data());
      for (uint32_t i = 0; i < 64; ++i) {
        const float x[2] = {xl[i], xr[i]}, s[2] = {0.0f, 0.0f};
        a.process(x, s);
        for (int ch = 0; ch < 2; ++ch) worst = std::max(worst, std::fabs(static_cast<double>(a.out[ch]) - outB[2 * i + ch]));
      }
    }
    CHECK_MESSAGE(worst < 1e-5, label(c) << ": block vs per-sample " << worst);
  }
}

TEST_CASE("Auto Filter: the envelope follower moves the cutoff; the sidechain replaces or mixes into its input") {
  // The main input is silent; the sidechain plays tone bursts. Follower on, amount +50 % (= +4 octaves per unit level).
  Params p;
  p.freq = 300.0f;
  p.envAmount = 0.5f;
  p.attack = 0.001f;
  p.release = 0.05f;
  auto cutoffPeak = [&](bool scOn, float mix) {
    Params q = p;
    q.scMix = mix;
    const Program prog = afProgram(conf(0, 0, 0, false, 0, true, scOn));
    Stimulus st = afStimulus(12000, 64, [](uint32_t, uint64_t) { return 0.0; });
    setParams(st, q);
    const Outputs o = run(prog, Backend::Jit, st);
    REQUIRE(o.error.empty());
    return peak(o.at("cutoff")[0]);
  };
  CHECK(cutoffPeak(false, 1.0f) == doctest::Approx(300.0).epsilon(1e-5));  // own input: silent
  CHECK(cutoffPeak(true, 0.0f) == doctest::Approx(300.0).epsilon(1e-5));   // sidechain mixed out
  const double full = cutoffPeak(true, 1.0f), half = cutoffPeak(true, 0.5f);
  // Burst peak 0.65 (mono mean of 0.7 and 0.6): 300 Hz · 2^(4·0.65) ≈ 1.8 kHz; half the mix, half the octaves.
  MESSAGE("sidechain follower: cutoff peak " << full << " Hz (mix 100 %), " << half << " Hz (mix 50 %)");
  CHECK(full > 1500.0);
  CHECK(full < 1900.0);
  CHECK(std::log2(half / 300.0) == doctest::Approx(0.5 * std::log2(full / 300.0)).epsilon(0.02));
}


// ================================================================================================================
// M4 review M12, M13 and the stability / tuning renders (T4.x-fix-dsp)
// ================================================================================================================

namespace {

// |H(f)| in dB of `prog`'s impulse response (amplitude `amp`: every model is linear there) at the listed frequencies.
std::vector<double> smallSignalDb(const Program& prog, const Params& p, const std::vector<double>& freqs, double sr = kSr) {
  constexpr double kAmp = 1e-4;
  const auto len = static_cast<uint32_t>(sr);  // 1 s: the resonances have rung out (−∞ dB well before)
  Stimulus st = afStimulus(len, 64, [](uint32_t, uint64_t t) { return t == 0 ? kAmp : 0.0; });
  setParams(st, p);
  const Outputs o = run(prog, Backend::Jit, st);
  REQUIRE_MESSAGE(o.error.empty(), o.error);
  const auto& y = o.at("y")[0];
  std::vector<double> r;
  for (const double f : freqs) {
    std::complex<double> acc = 0.0, rot = 1.0;
    const std::complex<double> step = std::polar(1.0, -2.0 * kPi * f / sr);
    for (size_t t = 0; t < y.size(); ++t) {
      acc += static_cast<double>(y[t]) * rot;
      rot *= step;
    }
    r.push_back(20.0 * std::log10(std::max(std::abs(acc) / kAmp, 1e-30)));
  }
  return r;
}

// The amplitude of the `hz` component of y over [from, from + n) (a whole number of cycles), by a DFT.
double toneAmp(const std::vector<float>& y, size_t from, size_t n, double hz, double sr = kSr) {
  std::complex<double> acc = 0.0;
  for (size_t t = 0; t < n; ++t) acc += static_cast<double>(y[from + t]) * std::polar(1.0, -2.0 * kPi * hz * static_cast<double>(t) / sr);
  return 2.0 * std::abs(acc) / static_cast<double>(n);
}

// As test_devices.cpp's stepClick: the largest sample-to-sample step of the output in the 10 ms after a param step
// over the steady one before / after (a sine 1.3 rad into its cycle at the step).
double stepClick(const Program& prog, const std::string& param, double from, double to, double hz, const Params& p) {
  constexpr uint64_t kStep = 9600, kLen = 19200;
  Stimulus st = afStimulus(static_cast<uint32_t>(kLen), 64, [hz](uint32_t, uint64_t t) {
    return 0.5 * std::sin(2.0 * kPi * hz * (static_cast<double>(t) - static_cast<double>(kStep)) / kSr + 1.3);
  });
  setParams(st, p);
  st.params[param] = from;
  st.paramAt = [&](const std::string& name, uint64_t t0) { return name == param && t0 >= kStep ? to : NAN; };
  const Outputs o = runBoth(prog, st, "step " + param);
  const auto& y = o.at("y")[0];
  auto d1 = [&](uint64_t t) { return std::fabs(static_cast<double>(y[t]) - y[t - 1]); };
  double steady = 0.0, click = 0.0;
  for (uint64_t t = kStep - 4800; t < kStep; ++t) steady = std::max(steady, d1(t));
  for (uint64_t t = kStep + 4800; t < kLen; ++t) steady = std::max(steady, d1(t));
  for (uint64_t t = kStep; t < kStep + 480; ++t) click = std::max(click, d1(t));
  return click / steady;
}

}  // namespace

TEST_CASE("Auto Filter: the circuit models' levels match at the default resonance (M4 review M12)") {
  // Small signal, resonance 30 % (the default), cutoff 1 kHz: the passbands (LP and notch at 40 Hz, HP at 16 kHz)
  // and, for LP and notch, the level of the whole response (the mean power gain over 20 Hz … 20 kHz, log-spaced:
  // what a broadband signal hears) of every model within 1 dB of Clean's, at 12 and 24 dB.
  const std::vector<double> freqs = logFreqs(20.0, 20000.0, 121);
  for (int slope = 0; slope < 2; ++slope)
    for (const int type : {0, 1, 3}) {
      double lo[5], mean[5];
      std::string line;
      for (int circuit = 0; circuit < 5; ++circuit) {
        Params p;
        p.freq = 1000.0f;
        const std::vector<double> db = smallSignalDb(afProgram(conf(type, circuit, slope)), p, freqs);
        double pw = 0.0;
        for (const double d : db) pw += std::pow(10.0, d / 10.0);
        mean[circuit] = 10.0 * std::log10(pw / static_cast<double>(db.size()));
        lo[circuit] = type == 1 ? db[freqs.size() - 9] : db[6];  // 16.2 kHz / 40 Hz
        line += std::string(" ") + kCircuitNames[circuit] + " " + std::to_string(lo[circuit]) + " / " + std::to_string(mean[circuit]);
      }
      MESSAGE(std::string(kTypeNames[type]) << (slope ? " 24" : " 12") << " passband / mean power gain (dB):" << line);
      for (int circuit = 1; circuit < 5; ++circuit) {
        CHECK_MESSAGE(std::fabs(lo[circuit] - lo[0]) <= 1.0, std::string(kCircuitNames[circuit]) << " passband " << lo[circuit] << " vs Clean " << lo[0]);
        // The ladder's high-pass is its own shape below the cutoff (pole mixing; its passband matches): the mean power
        // gain is compared for the lowpass and the notch, which the passband compensation affects.
        if (type != 1)
          CHECK_MESSAGE(std::fabs(mean[circuit] - mean[0]) <= 1.0, std::string(kCircuitNames[circuit]) << " level " << mean[circuit] << " vs Clean " << mean[0]);
      }
    }
}

TEST_CASE("Auto Filter: drive keeps small signals at their level; the level of a 0 dBFS tone (documented)") {
  // 220 Hz through LP 12 dB at 2 kHz, resonance 30 %: the fundamental at −40 dBFS stays within 0.5 dB of Clean at
  // every drive (the nonlinear terms see drive·v and are divided by drive); at 0 dBFS a saturating input stage keeps
  // the level near its knee (1/drive), so OSR and SMP lose level with drive (measured here and documented).
  for (const double amp : {0.01, 1.0}) {
    std::string line;
    for (int circuit = 0; circuit < 5; ++circuit) {
      line += std::string(" ") + kCircuitNames[circuit] + ":";
      for (const double driveDb : {0.0, 12.0, 24.0}) {
        if (circuit == 0 && driveDb > 0.0) continue;
        Params p;
        p.freq = 2000.0f;
        p.drive = static_cast<float>(std::pow(10.0, driveDb / 20.0));
        Stimulus st = afStimulus(24000, 64, [amp](uint32_t, uint64_t t) { return amp * std::sin(2.0 * kPi * 220.0 * static_cast<double>(t) / kSr); });
        setParams(st, p);
        const Outputs o = run(afProgram(conf(0, circuit, 0)), Backend::Jit, st);
        REQUIRE(o.error.empty());
        const double lvl = 20.0 * std::log10(toneAmp(o.at("y")[0], 24000 - 2400 * 4, 2400 * 4, 220.0) / amp);
        line += " " + std::to_string(lvl);
        if (amp < 0.1) CHECK_MESSAGE(std::fabs(lvl) < 0.5, kCircuitNames[circuit] << " drive " << driveDb << " dB: " << lvl << " dB");
      }
    }
    MESSAGE("fundamental gain (dB) at " << 20.0 * std::log10(amp) << " dBFS, drive 0 / 12 / 24 dB:" << line);
  }
}

TEST_CASE("Auto Filter: param jumps glide or keep the output continuous (M4 review M13)") {
  struct Case {
    const char* what;
    Conf c;
    const char* param;
    double from, to, hz;
    Params p;
  };
  Params base;
  Params morph;
  Params prd;
  prd.res = 0.6f;
  const Case cases[] = {
      {"LP 12 Clean freq 1 -> 4 kHz", conf(0, 0, 0), "a__freq", 1000.0, 4000.0, 300.0, base},
      {"HP 12 Clean res 30 -> 90 %", conf(1, 0, 0), "a__res", 0.3, 0.9, 2000.0, base},
      {"BP 24 MS2 res 30 -> 90 %", conf(2, 2, 1), "a__res", 0.3, 0.9, 1000.0, base},
      {"LP 24 PRD res 60 -> 10 %", conf(0, 4, 1), "a__res", 0.6, 0.1, 300.0, prd},
      {"Morph 12 Clean morph 0 -> 60 %", conf(4, 0, 0), "a__morph", 0.0, 0.6, 700.0, morph},
      {"LP 12 Clean output 0 -> -12 dB", conf(0, 0, 0), "a__output", 1.0, 0.251, 220.0, base},
      {"LP 12 OSR dry/wet 100 -> 20 %", conf(0, 1, 0), "a__dryWet", 1.0, 0.2, 2500.0, base},
  };
  for (const Case& k : cases) {
    const double r = stepClick(afProgram(k.c), k.param, k.from, k.to, k.hz, k.p);
    MESSAGE(std::string(k.what) << ": click " << r << " x the steady sample-to-sample step");
    CHECK_MESSAGE(r < 2.0, k.what << ": " << r);
  }
}

// Stability and self-oscillation at 22.05, 48 and 192 kHz: every model, 12 / 24 dB, resonance 100 % and 125 %,
// drive 0 / +24 dB, cutoff 1 kHz and 0.45·sr; a click, 0.5 s of 0 dBFS noise, then 0.5 s of silence. Everything stays
// finite and bounded; a self-oscillating model keeps oscillating near the cutoff (its pitch is measured); Clean rings
// out (default tier: a subset at each rate).
static void stabilityRenders(bool full) {
  for (const double sr : {22050.0, 48000.0, 192000.0}) {
    const auto len = static_cast<uint32_t>(sr);
    for (int circuit = 0; circuit < 5; ++circuit)
      for (int slope = 0; slope < 2; ++slope)
        for (const float res : {1.0f, 1.25f})
          for (const double driveDb : {0.0, 24.0})
            for (const double fcRel : {0.0, 0.45}) {
              if (!full && (slope != circuit % 2 || res != 1.25f || (driveDb > 0.0) != (circuit >= 3) || fcRel > 0.0)) continue;
              Params p;
              p.res = res;
              p.drive = static_cast<float>(std::pow(10.0, driveDb / 20.0));
              p.freq = static_cast<float>(fcRel > 0.0 ? std::min(fcRel * sr, 20000.0) : 1000.0);
              const Conf c = conf(0, circuit, slope);
              const Program prog = compile(afProgramText(c, false), sr);
              // Impulse, 0.25 s of free ringing (where a self-oscillation's pitch is measured), 0.25 s of 0 dBFS
              // noise, 0.5 s of silence.
              const uint32_t ringEnd = len / 4, noiseEnd = len / 2;
              Stimulus st = afStimulus(len, 64, [ringEnd, noiseEnd](uint32_t ch, uint64_t t) {
                if (t < 4) return 0.5;
                return t >= ringEnd && t < noiseEnd ? noise(t, 5 + ch) : 0.0;
              });
              setParams(st, p);
              const Outputs o = run(prog, Backend::Jit, st);
              REQUIRE_MESSAGE(o.error.empty(), o.error);
              const auto& y = o.at("y")[0];
              double pk = 0.0, tail = 0.0;
              bool finite = true;
              for (size_t t = 0; t < y.size(); ++t) {
                finite &= std::isfinite(y[t]);
                pk = std::max(pk, std::fabs(static_cast<double>(y[t])));
                if (t >= len - len / 4) tail = std::max(tail, std::fabs(static_cast<double>(y[t])));
              }
              // Self-oscillation pitch: zero crossings over the last half of the free ringing.
              double first = -1.0, last = -1.0;
              int n = 0;
              for (size_t t = ringEnd / 2 + 1; t < ringEnd; ++t)
                if (y[t - 1] < 0.0f && y[t] >= 0.0f) {
                  const double x = static_cast<double>(t - 1) + y[t - 1] / (static_cast<double>(y[t - 1]) - y[t]);
                  if (first < 0.0) first = x;
                  last = x;
                  ++n;
                }
              const double hz = n > 1 ? (n - 1) * sr / (last - first) : 0.0;
              const std::string lb = label(c) + " sr " + std::to_string(static_cast<int>(sr)) + " res " + std::to_string(res) + " drive " +
                                     std::to_string(static_cast<int>(driveDb)) + " dB fc " + std::to_string(static_cast<int>(p.freq));
              MESSAGE(lb << ": peak " << pk << ", last 250 ms " << tail << ", ringing at " << hz << " Hz ("
                         << (hz > 0.0 ? 1200.0 * std::log2(hz / p.freq) : 0.0) << " c from the cutoff)");
              CHECK_MESSAGE(finite, lb);
              CHECK_MESSAGE(pk < 16.0, lb << ": peak " << pk);  // the passband-compensated ladder's oscillation is the loudest
              if (circuit == 0) CHECK_MESSAGE(tail < 1e-3, lb << ": Clean must ring out, " << tail);
            }
  }
}

TEST_CASE("Auto Filter: stability at 22.05 / 48 / 192 kHz with drive (self-oscillation bounded, Clean rings out)") { stabilityRenders(false); }
TEST_CASE("Auto Filter: stability at 22.05 / 48 / 192 kHz, every model x slope x res x drive x cutoff" * doctest::test_suite("slow")) {
  stabilityRenders(true);
}

TEST_CASE("Auto Filter: the comb's impulse response repeats every 1/fc (tuning)") {
  // Comb type: the echoes of an impulse must come every sr/fc samples: the first echo's peak (with the delay line's
  // linear interpolation, the energy centre of the two taps around it) at sr/fc, and the spectrum's peaks at
  // multiples of fc. Measured as the pitch of the impulse response's autocorrelation peak.
  for (const double fc : {100.0, 440.0, 1000.0, 2500.0, 7000.0}) {
    Params p;
    p.freq = static_cast<float>(fc);
    p.res = 1.0f;  // fb 0.79
    const Program prog = afProgram(conf(6, 0, 0));
    Stimulus st = afStimulus(16384, 64, [](uint32_t, uint64_t t) { return t == 0 ? 1.0 : 0.0; });
    setParams(st, p);
    const Outputs o = run(prog, Backend::Jit, st);
    REQUIRE(o.error.empty());
    const auto& y = o.at("y")[0];
    // The first echo: the energy centre of the samples around sr/fc (y[0] is the direct impulse).
    const double T = kSr / fc;
    const auto from = static_cast<size_t>(std::max(1.0, std::floor(T) - 2.0)), to = static_cast<size_t>(std::ceil(T) + 3.0);
    double m0 = 0.0, m1 = 0.0;
    for (size_t t = from; t <= to; ++t) {
      m0 += std::fabs(static_cast<double>(y[t]));
      m1 += std::fabs(static_cast<double>(y[t])) * static_cast<double>(t);
    }
    const double echo = m1 / m0;
    // The spectral peak nearest fc (parabolic interpolation over a fine DFT grid).
    double best = 0.0, bestF = fc;
    for (double f = fc * 0.9; f <= fc * 1.1; f += fc * 0.0005) {
      std::complex<double> acc = 0.0;
      for (size_t t = 0; t < y.size(); ++t) acc += static_cast<double>(y[t]) * std::polar(1.0, -2.0 * kPi * f * static_cast<double>(t) / kSr);
      if (std::abs(acc) > best) best = std::abs(acc), bestF = f;
    }
    const double cents = 1200.0 * std::log2(bestF / fc);
    MESSAGE("comb fc " << fc << " Hz: first echo at " << echo << " samples (sr/fc " << T << "), spectral peak " << bestF << " Hz ("
                       << cents << " c)");
    CHECK_MESSAGE(std::fabs(cents) < 5.0, "comb fc " << fc << ": " << cents << " c");
  }
}
