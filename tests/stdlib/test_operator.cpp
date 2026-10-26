// T4.12: Operator (stdlib/devices/operator.pqp) and its multi-stage envelope (stdlib/envelopes/msenv.pqp) against
// the hand-written C++ reference (operator_reference.h) on the interpreter and the JIT, bit-identical to each
// other, every process() call in an RtScope; plus analytic checks: exact pitches and just ratios, the built-in
// waveform tables' partials, the envelope's stage times and loop modes, tuned playback (31edo and a MOS) through
// the tuning signal and the VoiceEngine, and what an operator or section that is off costs (nothing).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "operator_reference.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/tables.h"
#include "pq/dsp/tuning_table.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/note_events.h"
#include "pq/engine/tuning_signal.h"
#include "pq/engine/voices.h"
#include "pq/tuning/mos_tuning.h"
#include "pq/tuning/tuning.h"
#include "stdlib_harness.h"

using namespace stdlib_test;
namespace eng = pq::engine;

namespace {

constexpr double kSr = 48000.0;
constexpr double kPi = 3.141592653589793;
constexpr double kRefHz = 261.626;

std::string num(double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.9g", v);
  return b;
}

// ---- programs --------------------------------------------------------------------------------------------------

// The device as an instrument: stereo out, done, the four operator exports and the modulation exports.
std::string operatorText(const opref::Config& c, bool exports = true) {
  std::string args = "algorithm: " + std::to_string(c.algorithm);
  const char* ops = "ABCD";
  for (int i = 0; i < 4; ++i) {
    const std::string X(1, ops[i]);
    args += ", on" + X + ": " + (c.on[static_cast<size_t>(i)] ? "1" : "0");
    args += ", wave" + X + ": " + std::to_string(c.wave[static_cast<size_t>(i)]);
    args += ", loop" + X + ": " + std::to_string(c.loop[static_cast<size_t>(i)]);
  }
  args += ", pitchEnvOn: " + std::to_string(c.pitchEnvOn ? 1 : 0) + ", pitchLoop: " + std::to_string(c.pitchLoop);
  args += ", lfoOn: " + std::to_string(c.lfoOn ? 1 : 0) + ", lfoWave: " + std::to_string(c.lfoWave) +
          ", lfoRetrig: " + std::to_string(c.lfoRetrig ? 1 : 0);
  args += ", filterOn: " + std::to_string(c.filterOn ? 1 : 0) + ", filterType: " + std::to_string(c.filterType) +
          ", filterSlope: " + std::to_string(c.filterSlope) + ", filterLoop: " + std::to_string(c.filterLoop) +
          ", filterCircuit: " + std::to_string(c.filterCircuit);
  args += ", glideOn: " + std::to_string(c.glideOn ? 1 : 0) + ", spreadOn: " + std::to_string(c.spreadOn ? 1 : 0);
  if (!exports)
    return "import \"devices/operator.pqp\"\npatch T {\n  out y : audio voice stereo\n  out done : bool voice\n"
           "  o = operator(" + args + ")\n  y = o\n  done = o.done\n}\n";
  return "import \"devices/operator.pqp\"\n"
         "patch T {\n"
         "  out y    : audio voice stereo\n"
         "  out done : bool voice\n"
         "  out a    : audio voice\n  out b : audio voice\n  out c : audio voice\n  out d : audio voice\n"
         "  out lfo  : audio voice\n  out pe : audio voice\n  out fe : audio voice\n"
         "  o = operator(" + args + ")\n"
         "  y = o\n  done = o.done\n  a = o.opA\n  b = o.opB\n  c = o.opC\n  d = o.opD\n"
         "  lfo = o.lfo\n  pe = o.pitchEnv\n  fe = o.filterEnv\n"
         "}\n";
}

// The params of `p` that exist in the compiled program (parts that are off are compiled away with their params).
void setParams(Stimulus& st, const Program& prog, const opref::Params& p) {
  std::set<std::string> names;
  for (const auto& ps : prog.sched.params) names.insert(prog.typed.patch.nodeName(ps.node));
  auto set = [&](const std::string& n, double v) {
    if (names.count("o__" + n)) st.params["o__" + n] = v;
  };
  const char* ops = "ABCD";
  for (size_t i = 0; i < 4; ++i) {
    const std::string X(1, ops[i]);
    const opref::OpParams& o = p.op[i];
    set("coarse" + X, o.coarse);
    set("den" + X, o.den);
    set("fine" + X, o.fine);
    set("fixed" + X, o.fixed);
    set("freq" + X, o.freq);
    set("level" + X, o.level);
    set("vel" + X, o.vel);
    set("key" + X, o.key);
    set("attack" + X, o.env.a);
    set("decay" + X, o.env.d);
    set("release" + X, o.env.r);
    set("initial" + X, o.env.init);
    set("peak" + X, o.env.peak);
    set("sustain" + X, o.env.sus);
    set("end" + X, o.env.end);
    set("repeat" + X, o.env.rep);
    set("penvTo" + X, o.penvTo);
    set("lfoTo" + X, o.lfoTo);
    for (int h = 0; h < 16; ++h) set("u" + X + std::to_string(h + 1), o.u[static_cast<size_t>(h)]);
  }
  set("feedback", p.feedback);
  set("penvAmount", p.penvAmount);
  set("penvAttack", p.penv.a);
  set("penvDecay", p.penv.d);
  set("penvRelease", p.penv.r);
  set("penvInitial", p.penv.init);
  set("penvPeak", p.penv.peak);
  set("penvSustain", p.penv.sus);
  set("penvEnd", p.penv.end);
  set("penvRepeat", p.penv.rep);
  set("lfoRate", p.lfoRate);
  set("lfoSync", p.lfoSync);
  set("lfoBeats", p.lfoBeats);
  set("lfoAmount", p.lfoAmount);
  set("lfoPitch", p.lfoPitch);
  set("lfoFilter", p.lfoFilter);
  set("filterFreq", p.filterFreq);
  set("filterRes", p.filterRes);
  set("filterDrive", p.filterDrive);
  set("morph", p.morph);
  set("filterKey", p.filterKey);
  set("filterVel", p.filterVel);
  set("fenvAmount", p.fenvAmount);
  set("fenvAttack", p.fenv.a);
  set("fenvDecay", p.fenv.d);
  set("fenvRelease", p.fenv.r);
  set("fenvInitial", p.fenv.init);
  set("fenvPeak", p.fenv.peak);
  set("fenvSustain", p.fenv.sus);
  set("fenvEnd", p.fenv.end);
  set("fenvRepeat", p.fenv.rep);
  set("volume", p.volume);
  set("pan", p.pan);
  set("transpose", p.transpose);
  set("time", p.time);
  set("spread", p.spread);
  set("glideTime", p.glideTime);
}

// ---- notes -------------------------------------------------------------------------------------------------------

struct Note {
  double hz;
  uint64_t on, off;  // gate in [on, off)
  double velocity;
};

float centsOf(double hz) { return static_cast<float>(1200.0 * std::log2(hz / kRefHz)); }

// voice.* as the VoiceEngine renders them: hz 0 and cents kIdleCents before the onset.
double signalOf(const std::vector<Note>& notes, eng::VoiceSignal sig, uint32_t v, uint64_t t) {
  const Note& n = notes[v];
  const bool started = t >= n.on;
  switch (sig) {
    case eng::VoiceSignal::Hz: return started ? n.hz : 0.0;
    case eng::VoiceSignal::Cents: return started ? static_cast<double>(centsOf(n.hz)) : static_cast<double>(eng::kIdleCents);
    case eng::VoiceSignal::Gate: return t >= n.on && t < n.off ? 1.0 : 0.0;
    case eng::VoiceSignal::Trigger: return t == n.on ? 1.0 : 0.0;
    case eng::VoiceSignal::Velocity: return n.velocity;
    default: return 0.0;
  }
}

Stimulus noteStimulus(const std::vector<Note>& notes, uint32_t frames, uint32_t block) {
  Stimulus st;
  st.frames = frames;
  st.block = block;
  st.voices = static_cast<uint32_t>(notes.size());
  st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
  st.voiceSignal = [notes](eng::VoiceSignal sig, uint32_t v, uint64_t t) { return signalOf(notes, sig, v, t); };
  st.transportBeat0 = 0.37;
  return st;
}

// The transport position at sample t as the image computes it (per block: block start + frame · beats per frame).
float beatAt(const Stimulus& st, uint64_t t) {
  const double bpf = st.bpm / 60.0 / kSr;
  const uint64_t t0 = t / st.block * st.block;
  const double b0 = st.transportBeat0 + static_cast<double>(t0) * bpf;
  return static_cast<float>(b0 + static_cast<double>(t - t0) * bpf);
}

// The reference's outputs over the run, named like the program's.
std::map<std::string, std::vector<std::vector<double>>> runReference(const opref::Config& cfg, const opref::Params& prm,
                                                                    const std::vector<Note>& notes, const Stimulus& st) {
  opref::Operator ref(cfg, prm, notes.size());
  std::map<std::string, std::vector<std::vector<double>>> r;
  const char* names[] = {"y", "y[1]", "done", "a", "b", "c", "d", "lfo", "pe", "fe"};
  for (const char* n : names) r[n].assign(notes.size(), {});
  std::vector<opref::VoiceIn> in(notes.size());
  std::vector<opref::Out> out;
  for (uint64_t t = 0; t < st.frames; ++t) {
    for (uint32_t v = 0; v < notes.size(); ++v) {
      in[v].hz = static_cast<float>(signalOf(notes, eng::VoiceSignal::Hz, v, t));
      in[v].cents = static_cast<float>(signalOf(notes, eng::VoiceSignal::Cents, v, t));
      in[v].gate = signalOf(notes, eng::VoiceSignal::Gate, v, t) != 0.0;
      in[v].trigger = signalOf(notes, eng::VoiceSignal::Trigger, v, t) != 0.0;
      in[v].velocity = static_cast<float>(notes[v].velocity);
    }
    ref.tick(in, beatAt(st, t), static_cast<float>(st.bpm), out);
    for (uint32_t v = 0; v < notes.size(); ++v) {
      r["y"][v].push_back(out[v].l);
      r["y[1]"][v].push_back(out[v].r);
      r["done"][v].push_back(out[v].done ? 1.0 : 0.0);
      r["a"][v].push_back(out[v].op[0]);
      r["b"][v].push_back(out[v].op[1]);
      r["c"][v].push_back(out[v].op[2]);
      r["d"][v].push_back(out[v].op[3]);
      r["lfo"][v].push_back(out[v].lfo);
      r["pe"][v].push_back(out[v].penv);
      r["fe"][v].push_back(out[v].fenv);
    }
  }
  return r;
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
      CHECK_MESSAGE(diffs == 0, label << ": interpreter and JIT differ in '" << name << "' voice " << v << " at " << diffs
                                      << " samples, first t=" << first << " (" << va[v][first] << " vs " << vb[v][first] << ")");
    }
  }
  return a;
}

// Every output vs the reference: |got − ref| / max(1, |ref|) ≤ tol (done exactly). Returns the largest error.
double compare(const Outputs& o, const std::map<std::string, std::vector<std::vector<double>>>& ref, const std::string& label,
             double tol = 1e-5) {
  double worst = 0.0;
  std::string where;
  for (const auto& [name, rv] : ref) {
    const auto it = o.byName.find(name);
    if (it == o.byName.end()) continue;  // an export the configuration compiled away
    for (size_t v = 0; v < rv.size(); ++v) {
      REQUIRE(it->second[v].size() == rv[v].size());
      for (size_t t = 0; t < rv[v].size(); ++t) {
        const float g = it->second[v][t];
        const double e = std::isfinite(g) ? std::fabs(g - rv[v][t]) / std::max(1.0, std::fabs(rv[v][t])) : INFINITY;
        if (e > worst) {
          worst = e;
          where = name + " voice " + std::to_string(v) + " t=" + std::to_string(t) + " got " + num(g) + " ref " + num(rv[v][t]);
        }
      }
    }
  }
  CHECK_MESSAGE(worst <= tol, label << ": max error " << worst << " (" << where << ")");
  return worst;
}

double checkVsReference(const opref::Config& cfg, const opref::Params& prm, const std::vector<Note>& notes, uint32_t frames,
                        uint32_t block, const std::string& label, double tol = 1e-5) {
  const Program prog = compile(operatorText(cfg), kSr);
  Stimulus st = noteStimulus(notes, frames, block);
  setParams(st, prog, prm);
  const Outputs o = runBoth(prog, st, label);
  return compare(o, runReference(cfg, prm, notes, st), label, tol);
}

// Three voices: different pitches, onsets, velocities and note lengths (one released early, one held to the end).
std::vector<Note> threeVoices() {
  return {{220.0, 0, 2600, 1.0}, {329.6275569, 37, 4400, 0.7}, {97.99885858, 300, 1500, 0.45}};
}

opref::Params richParams() {
  opref::Params p;
  // A 1, B 2, C 3/2 (a just fifth: coarse 3, den 2), D 7.013
  p.op[1].coarse = 2;
  p.op[2].coarse = 3;
  p.op[2].den = 2;
  p.op[3].coarse = 7;
  p.op[3].fine = 13;
  p.op[1].level = opref::db(-6);
  p.op[2].level = opref::db(-9);
  p.op[3].level = opref::db(-12);
  p.op[0].env = {0.003f, 0.25f, 0.04f, opref::db(-70), 1.0f, opref::db(-6), opref::db(-70), 1.0f};
  p.op[1].env = {0.0f, 0.08f, 0.06f, opref::db(-70), 1.0f, opref::db(-12), opref::db(-70), 1.0f};
  p.op[2].env = {0.01f, 0.4f, 0.02f, opref::db(-20), opref::db(-3), opref::db(-18), opref::db(-70), 1.0f};
  p.op[3].env = {0.001f, 0.15f, 0.08f, opref::db(-70), 1.0f, opref::db(-24), opref::db(-70), 1.0f};
  p.op[0].vel = 0.6f;
  p.op[3].vel = -0.4f;
  p.op[1].key = 0.3f;
  p.op[2].key = -0.5f;
  p.feedback = 0.35f;
  p.volume = opref::db(-6);
  p.pan = -0.3f;
  return p;
}

}  // namespace

// ================================================================================================================
// Algorithms
// ================================================================================================================

TEST_CASE("Operator: every algorithm vs the C++ reference (4 sine operators, feedback, levels, envelopes), interpreter == JIT") {
  const opref::Params prm = richParams();
  for (int alg = 1; alg <= 11; ++alg) {
    opref::Config cfg;
    cfg.algorithm = alg;
    cfg.on = {true, true, true, true};
    checkVsReference(cfg, prm, threeVoices(), 5000, alg % 2 ? 64u : 37u, "algorithm " + std::to_string(alg));
  }
}

TEST_CASE("Operator: algorithms with operators switched off vs the reference (off = absent from the routing)") {
  const opref::Params prm = richParams();
  const std::array<std::array<bool, 4>, 4> sets{{{true, false, true, true}, {true, true, false, true}, {false, true, true, true}, {true, true, true, false}}};
  for (int alg = 1; alg <= 11; ++alg)
    for (size_t s = 0; s < sets.size(); ++s) {
      if ((alg + static_cast<int>(s)) % 3 != 0) continue;  // a third of the combinations in the default tier
      opref::Config cfg;
      cfg.algorithm = alg;
      cfg.on = sets[s];
      checkVsReference(cfg, prm, threeVoices(), 2400, 64, "algorithm " + std::to_string(alg) + " on-set " + std::to_string(s));
    }
}

TEST_CASE("Operator: algorithms x on-sets, the rest" * doctest::test_suite("slow")) {
  const opref::Params prm = richParams();
  const std::array<std::array<bool, 4>, 4> sets{{{true, false, true, true}, {true, true, false, true}, {false, true, true, true}, {true, true, true, false}}};
  for (int alg = 1; alg <= 11; ++alg)
    for (size_t s = 0; s < sets.size(); ++s) {
      if ((alg + static_cast<int>(s)) % 3 == 0) continue;
      opref::Config cfg;
      cfg.algorithm = alg;
      cfg.on = sets[s];
      checkVsReference(cfg, prm, threeVoices(), 2400, 64, "algorithm " + std::to_string(alg) + " on-set " + std::to_string(s));
    }
}

TEST_CASE("Operator: routing semantics (additive = the sum of four sines; a serial stack with silent modulators = a sine)") {
  // Algorithm 11 with no envelope shaping: the output is the sum of the four operators' sines at their ratios.
  opref::Params prm;
  for (auto& o : prm.op) o.level = 1.0f;
  prm.op[1].coarse = 2;
  prm.op[2].coarse = 3;
  prm.op[3].coarse = 4;
  prm.volume = 1.0f;
  opref::Config cfg;
  cfg.algorithm = 11;
  cfg.on = {true, true, true, true};
  const std::vector<Note> notes{{200.0, 0, 100000, 1.0}};
  {
    const Program prog = compile(operatorText(cfg), kSr);
    Stimulus st = noteStimulus(notes, 2048, 64);
    setParams(st, prog, prm);
    const Outputs o = runBoth(prog, st, "additive");
    double worst = 0.0;
    for (uint64_t t = 1; t < 2048; ++t) {
      double s = 0.0;
      for (int h = 1; h <= 4; ++h) s += std::sin(2.0 * kPi * 200.0 * h * static_cast<double>(t) / kSr);
      worst = std::max(worst, std::fabs(o.at("y")[0][t] - s));
    }
    CHECK(worst < 2e-5);  // fast sine (1.5e-7) on 24-bit phases, four of them
  }
  // Algorithm 1 with B, C, D on but at −inf: A alone.
  prm.op[1].level = prm.op[2].level = prm.op[3].level = opref::db(-70);
  cfg.algorithm = 1;
  {
    const Program prog = compile(operatorText(cfg), kSr);
    Stimulus st = noteStimulus(notes, 2048, 64);
    setParams(st, prog, prm);
    const Outputs o = runBoth(prog, st, "serial, silent modulators");
    double worst = 0.0;
    for (uint64_t t = 1; t < 2048; ++t) worst = std::max(worst, std::fabs(o.at("y")[0][t] - std::sin(2.0 * kPi * 200.0 * static_cast<double>(t) / kSr)));
    CHECK(worst < 5e-6);
  }
}

// ================================================================================================================
// Waveforms
// ================================================================================================================

TEST_CASE("Operator: every waveform vs the reference (alone, and phase-modulated by B), interpreter == JIT") {
  opref::Params prm;
  prm.op[1].coarse = 3;
  prm.op[1].level = opref::db(-10);
  for (int h = 0; h < 16; ++h) prm.op[0].u[static_cast<size_t>(h)] = 0.8f / static_cast<float>(h + 1) * (h % 3 == 1 ? 0.3f : 1.0f);
  prm.volume = 1.0f;
  // A high note too, so the user partials' band limit (h · f < 0.45 · sr) and the table mips are exercised.
  const std::vector<Note> notes{{110.0, 0, 3000, 1.0}, {1760.0, 5, 3000, 1.0}, {7040.0, 9, 3000, 1.0}};
  for (int w = 0; w <= 22; ++w) {
    if (w == 21) continue;  // Noise White: below
    for (const bool pm : {false, true}) {
      opref::Config cfg;
      cfg.algorithm = 1;
      cfg.on = {true, pm, false, false};
      cfg.wave[0] = w;
      checkVsReference(cfg, prm, notes, 1600, 64, "wave " + std::to_string(w) + (pm ? " modulated" : ""));
    }
  }
}

TEST_CASE("Operator: Noise White is per-voice noise, bounded, interpreter == JIT") {
  opref::Config cfg;
  cfg.wave[0] = 21;
  opref::Params prm;
  prm.volume = 1.0f;
  const std::vector<Note> notes{{220.0, 0, 4000, 1.0}, {220.0, 0, 4000, 1.0}};
  const Program prog = compile(operatorText(cfg), kSr);
  Stimulus st = noteStimulus(notes, 4000, 64);
  setParams(st, prog, prm);
  const Outputs o = runBoth(prog, st, "noise white");
  double c = 0.0, e0 = 0.0, e1 = 0.0;
  for (size_t t = 0; t < 4000; ++t) {
    const double a = o.at("y")[0][t], b = o.at("y")[1][t];
    CHECK(std::fabs(a) <= 1.0);
    c += a * b;
    e0 += a * a;
    e1 += b * b;
  }
  CHECK(e0 > 100.0);                                   // it sounds (uniform noise: ~1/3 per sample)
  CHECK(std::fabs(c) / std::sqrt(e0 * e1) < 0.1);      // two voices, two noises
}

TEST_CASE("Operator: the built-in waveform tables hold the documented partial series") {
  using pq::dsp::BuiltinWave;
  struct Case {
    BuiltinWave w;
    int count;
    bool odd;
  };
  const Case cases[] = {{BuiltinWave::Saw3, 3, false},   {BuiltinWave::Saw8, 8, false},     {BuiltinWave::Saw64, 64, false},
                        {BuiltinWave::Square4, 4, true}, {BuiltinWave::Square16, 16, true}, {BuiltinWave::Square64, 64, true}};
  for (const Case& c : cases) {
    const auto cyc = pq::dsp::builtinWaveCycle(c.w, 2048);
    // Harmonic amplitudes by DFT: 1/h on the series (up to the peak normalisation), 0 elsewhere.
    std::vector<double> amp(200, 0.0);
    for (int h = 1; h < 200; ++h) {
      std::complex<double> acc = 0.0;
      for (int i = 0; i < 2048; ++i) acc += static_cast<double>(cyc[static_cast<size_t>(i)]) * std::polar(1.0, -2.0 * kPi * h * i / 2048.0);
      amp[static_cast<size_t>(h)] = 2.0 * std::abs(acc) / 2048.0;
    }
    const double scale = amp[1];
    double worst = 0.0;
    for (int h = 1; h < 200; ++h) {
      const bool on = c.odd ? (h % 2 == 1 && (h + 1) / 2 <= c.count) : h <= c.count;
      const double want = on ? scale / h : 0.0;
      worst = std::max(worst, std::fabs(amp[static_cast<size_t>(h)] - want));
    }
    CHECK_MESSAGE(worst < 1e-5, "wave " << static_cast<int>(c.w) << ": partial error " << worst);
    float peak = 0.0f;
    for (float x : cyc.span()) peak = std::max(peak, std::fabs(x));
    CHECK(peak == doctest::Approx(1.0).epsilon(1e-6));
  }
  for (uint32_t k = 0; k < pq::dsp::kBuiltinWaveCount; ++k) {
    const auto* v = pq::dsp::builtinWavetable(k);
    REQUIRE(v != nullptr);
    CHECK(v->size == 2048);
    CHECK(pq::dsp::builtinWavetable(k) == v);  // one shared table per wave
  }
  CHECK(pq::dsp::builtinWavetable(pq::dsp::kBuiltinWaveCount) == nullptr);
}

// ================================================================================================================
// Envelopes
// ================================================================================================================

namespace {

std::string msenvText(int mode, bool soft) {
  return "import \"envelopes/msenv.pqp\"\n"
         "patch T {\n"
         "  out y : audio voice\n  out d : bool voice\n"
         "  e = msenv(voice.gate, mode: " + std::to_string(mode) + ", soft: " + (soft ? "1" : "0") + ")\n"
         "  y = e\n  d = e.done\n"
         "}\n";
}

void setEnvParams(Stimulus& st, const Program& prog, const opref::EnvParams& e) {
  std::set<std::string> names;
  for (const auto& ps : prog.sched.params) names.insert(prog.typed.patch.nodeName(ps.node));
  auto set = [&](const char* n, double v) {
    if (names.count(std::string("e__") + n)) st.params[std::string("e__") + n] = v;
  };
  set("a", e.a);
  set("d", e.d);
  set("r", e.r);
  set("init", e.init);
  set("peak", e.peak);
  set("sus", e.sus);
  set("end", e.end);
  set("rep", e.rep);
}

// Gate patterns: held, released, re-triggered while sounding; per voice.
struct GateNote {
  uint64_t on, off;
};
double gateOf(const std::vector<std::vector<GateNote>>& g, uint32_t v, uint64_t t, bool trigger) {
  for (const GateNote& n : g[v]) {
    if (trigger && t == n.on) return 1.0;
    if (!trigger && t >= n.on && t < n.off) return 1.0;
  }
  return 0.0;
}

}  // namespace

TEST_CASE("msenv: every loop mode (soft and hard starts) vs the reference, interpreter == JIT") {
  const std::vector<std::vector<GateNote>> gates{{{0, 3000}}, {{10, 900}, {1400, 6000}}, {{50, 7000}}, {{200, 260}, {300, 1300}, {1310, 1400}}};
  const opref::EnvParams params[] = {
      {0.004f, 0.02f, 0.01f, 0.0f, 1.0f, 0.4f, 0.0f, 0.005f},      // fast: loops several times
      {0.0f, 0.005f, 0.003f, 0.3f, 0.8f, 0.5f, 0.1f, 0.25f},       // instant attack, End above 0, Repeat in beats
      {0.002f, 0.01f, 0.004f, -0.5f, 0.9f, -0.2f, 0.0f, 0.125f},   // bipolar (a pitch / filter envelope)
  };
  for (int mode = 0; mode <= 4; ++mode)
    for (const bool soft : {true, false})
      for (size_t pi = 0; pi < std::size(params); ++pi) {
        const Program prog = compile(msenvText(mode, soft), kSr);
        Stimulus st;
        st.frames = 8000;
        st.block = pi == 1 ? 37 : 64;
        st.voices = static_cast<uint32_t>(gates.size());
        st.input = [](uint32_t, uint32_t, uint64_t) { return 0.0; };
        st.voiceSignal = [&gates](eng::VoiceSignal sig, uint32_t v, uint64_t t) {
          if (sig == eng::VoiceSignal::Gate) return gateOf(gates, v, t, false);
          if (sig == eng::VoiceSignal::Trigger) return gateOf(gates, v, t, true);
          return 0.0;
        };
        st.transportBeat0 = 0.93;
        st.bpm = 150.0;
        setEnvParams(st, prog, params[pi]);
        const std::string label = "msenv mode " + std::to_string(mode) + (soft ? " soft" : " hard") + " set " + std::to_string(pi);
        const Outputs o = runBoth(prog, st, label);
        double worst = 0.0;
        std::string where;
        for (uint32_t v = 0; v < gates.size(); ++v) {
          opref::MsEnv ref;
          ref.mode = mode;
          ref.soft = soft;
          const auto c = opref::MsEnv::coef(params[pi], 1.0f, static_cast<float>(st.bpm));
          for (uint64_t t = 0; t < st.frames; ++t) {
            bool done = false;
            const float y = ref.tick(gateOf(gates, v, t, false) != 0.0, c, beatAt(st, t), done);
            const double e = std::fabs(o.at("y")[v][t] - y);
            if (e > worst) {
              worst = e;
              where = "voice " + std::to_string(v) + " t=" + std::to_string(t) + " got " + num(o.at("y")[v][t]) + " ref " + num(y);
            }
            if ((o.at("d")[v][t] != 0.0f) != done) {
              worst = INFINITY;
              where = "done at voice " + std::to_string(v) + " t=" + std::to_string(t);
            }
          }
        }
        CHECK_MESSAGE(worst <= 1e-6, label << ": max error " << worst << " (" << where << ")");
      }
}

TEST_CASE("msenv: stage times and loop semantics") {
  auto render = [](int mode, const opref::EnvParams& e, uint64_t off, uint32_t frames, double bpm = 120.0, double beat0 = 0.0) {
    opref::MsEnv ref;
    ref.mode = mode;
    const auto c = opref::MsEnv::coef(e, 1.0f, static_cast<float>(bpm));
    std::vector<float> y;
    for (uint64_t t = 0; t < frames; ++t) {
      bool done = false;
      y.push_back(ref.tick(t < off, c, static_cast<float>(beat0 + static_cast<double>(t) * bpm / 60.0 / kSr), done));
    }
    return y;
  };
  // Attack: Initial → Peak in a · sr samples (linear; float32 steps: within one sample); decay −80 dB of the
  // distance after d.
  {
    const opref::EnvParams e{0.01f, 0.1f, 0.05f, 0.2f, 1.0f, 0.5f, 0.0f, 1.0f};
    const auto y = render(0, e, 1u << 30, 12000);
    CHECK(y[0] == doctest::Approx(0.2f + 0.8f / 480.0f).epsilon(1e-5));
    CHECK(y[477] < 1.0f);
    CHECK(y[480] == 1.0f);  // 480 samples (± 1 for the float steps) reach Peak
    size_t top = 0;
    while (y[top] < 1.0f) ++top;
    const double dist = std::fabs(y[top + 4800] - 0.5);
    CHECK(dist == doctest::Approx(0.5e-4).epsilon(0.01));
  }
  // Release: −80 dB after r towards End, done below 1e-4.
  {
    const opref::EnvParams e{0.0f, 0.1f, 0.05f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    const auto y = render(0, e, 1000, 5000);
    CHECK(y[999] == 1.0f);
    CHECK(y[999 + 2400] == doctest::Approx(1e-4).epsilon(0.01));
  }
  // Loop: after the decay settles, Repeat seconds, then the attack again (several cycles while held).
  {
    const opref::EnvParams e{0.002f, 0.005f, 0.01f, 0.0f, 1.0f, 0.3f, 0.0f, 0.01f};
    const auto y = render(1, e, 1u << 30, 4800);
    std::vector<size_t> peaks;
    for (size_t t = 1; t < y.size(); ++t)
      if (y[t] == 1.0f && y[t - 1] < 1.0f) peaks.push_back(t);
    REQUIRE(peaks.size() >= 3);
    const size_t p1 = peaks[1] - peaks[0], p2 = peaks[2] - peaks[1];
    CHECK(p1 == p2);  // periodic
    CHECK(p1 > 480);  // ≥ Repeat (10 ms) + the decay
  }
  // Beat: restarts every Repeat beats from the note-on (120 BPM: 0.25 beat = 6000 samples).
  {
    const opref::EnvParams e{0.001f, 0.01f, 0.01f, 0.0f, 1.0f, 0.0f, 0.0f, 0.25f};
    const auto y = render(2, e, 1u << 30, 20000);
    CHECK(y[48] == 1.0f);
    CHECK(y[6000 + 48] == 1.0f);
    CHECK(y[12000 + 48] == 1.0f);
    CHECK(y[5990] < 0.01f);
  }
  // Sync: restarts on the song's Repeat-beat grid (beat 0.9 at the start: the next 0.5-beat line, beat 1.0, is
  // 0.1 beat away).
  {
    const opref::EnvParams e{0.001f, 0.01f, 0.01f, 0.0f, 1.0f, 0.0f, 0.0f, 0.5f};
    const auto y = render(3, e, 1u << 30, 20000, 120.0, 0.9);
    const size_t line = 2400;  // 0.1 beat at 120 BPM
    CHECK(y[line - 10] < 0.01f);
    CHECK(y[line + 48] == 1.0f);
    CHECK(y[line + 12000 + 48] == 1.0f);
  }
  // Trigger: the note-off is ignored; the release starts once the decay has settled.
  {
    const opref::EnvParams e{0.0f, 0.02f, 0.01f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const auto y = render(4, e, 10, 4000);  // a 10-sample note
    CHECK(y[100] > 0.1f);                  // still decaying long after the note-off
    CHECK(y[3999] < 1e-4f);
  }
}

// ================================================================================================================
// Pitch, LFO, filter, glide, spread, velocity / key, output
// ================================================================================================================

TEST_CASE("Operator: pitch envelope and LFO (every reference shape, retrigger, sync) vs the reference") {
  opref::Params prm = richParams();
  prm.penvAmount = 700.0f;
  prm.penv = {0.002f, 0.05f, 0.02f, -0.5f, 1.0f, 0.2f, 0.0f, 0.01f};
  prm.lfoRate = 9.0f;
  prm.lfoPitch = 150.0f;
  prm.lfoAmount = 0.8f;
  prm.op[1].penvTo = false;
  prm.op[2].lfoTo = false;
  for (int lw = 0; lw <= 4; ++lw)
    for (int variant = 0; variant < 3; ++variant) {
      opref::Config cfg;
      cfg.algorithm = 1 + lw * 2;
      cfg.on = {true, true, true, true};
      cfg.pitchEnvOn = variant != 2;
      cfg.pitchLoop = variant;
      cfg.lfoOn = true;
      cfg.lfoWave = lw;
      cfg.lfoRetrig = variant != 1;
      opref::Params p = prm;
      p.lfoSync = variant == 2;
      p.lfoBeats = 0.125f;
      checkVsReference(cfg, p, threeVoices(), 3000, 64, "lfo wave " + std::to_string(lw) + " variant " + std::to_string(variant));
    }
}

TEST_CASE("Operator: filter types, slopes, envelope, key, velocity and LFO vs the reference") {
  opref::Params prm = richParams();
  prm.filterFreq = 900.0f;
  prm.filterRes = 0.7f;
  prm.fenvAmount = 3600.0f;
  prm.fenv = {0.003f, 0.08f, 0.03f, 0.0f, 1.0f, 0.25f, 0.0f, 1.0f};
  prm.filterKey = 0.5f;
  prm.filterVel = -1200.0f;
  prm.lfoFilter = 600.0f;
  prm.lfoRate = 4.0f;
  for (int type = 0; type <= 4; ++type)
    for (int slope = 0; slope <= 1; ++slope) {
      opref::Config cfg;
      cfg.algorithm = 7;
      cfg.on = {true, true, true, true};
      cfg.wave = {8, 0, 17, 0};  // Saw 32 and Square 64 carriers: something to filter
      cfg.filterOn = true;
      cfg.filterType = type;
      cfg.filterSlope = slope;
      cfg.lfoOn = type % 2 == 0;
      opref::Params p = prm;
      p.morph = type == 4 ? 0.13f + 0.21f * static_cast<float>(slope) : 0.0f;
      checkVsReference(cfg, p, threeVoices(), 3000, slope ? 37u : 64u, "filter type " + std::to_string(type) + " slope " + std::to_string(slope));
    }
}

// T4.12-fix1: the circuit models (Auto Filter's sections with Operator's resonance law) in every type and slope,
// driven hard enough that the nonlinearity matters (+12 dB, loud carriers, high resonance), modulated per sample by
// the filter envelope, key, velocity and (every other configuration) the LFO. The default tier runs two types per
// circuit and slope; the slow tier all of them.
void checkCircuit(int circuit, int type, int slope, bool spread) {
  opref::Params prm = richParams();
  prm.op[1].level = 1.0f;  // B a full-level carrier (algorithm 7: D → C, B → A; carriers C and A)
  prm.volume = 1.0f;
  prm.filterFreq = 700.0f;
  prm.filterRes = 0.85f;
  prm.filterDrive = opref::db(12.0);
  prm.fenvAmount = 3600.0f;
  prm.fenv = {0.003f, 0.08f, 0.03f, 0.0f, 1.0f, 0.25f, 0.0f, 1.0f};
  prm.filterKey = 0.5f;
  prm.filterVel = -1200.0f;
  prm.lfoFilter = 600.0f;
  prm.lfoRate = 4.0f;
  prm.spread = 0.5f;
  opref::Config cfg;
  cfg.algorithm = 7;
  cfg.on = {true, true, true, true};
  cfg.wave = {8, 0, 17, 0};  // Saw 32 and Square 64 carriers
  cfg.filterOn = true;
  cfg.filterType = type;
  cfg.filterSlope = slope;
  cfg.filterCircuit = circuit;
  cfg.lfoOn = (type + slope) % 2 == 0;
  cfg.spreadOn = spread;
  opref::Params p = prm;
  p.morph = type == 4 ? 0.13f + 0.21f * static_cast<float>(slope) + 0.1f * static_cast<float>(circuit) : 0.0f;
  const std::string label = "circuit " + std::to_string(circuit) + " type " + std::to_string(type) + " slope " +
                            std::to_string(slope) + (spread ? " spread" : "");
  const double worst = checkVsReference(cfg, p, threeVoices(), 3000, slope ? 37u : 64u, label);
  MESSAGE(label << ": max error vs the reference " << worst);
}

TEST_CASE("Operator: filter circuit models (OSR, MS2, SMP, PRD) vs the reference, interpreter == JIT") {
  for (int circuit = 1; circuit <= 4; ++circuit)
    for (int slope = 0; slope <= 1; ++slope) {
      checkCircuit(circuit, (circuit + 2 * slope) % 5, slope, false);
      checkCircuit(circuit, 4, slope, circuit == 2 + slope);  // Morph (and spread: MS2 12 dB, SMP 24 dB)
    }
}

TEST_CASE("Operator: filter circuit models x every type and slope" * doctest::test_suite("slow")) {
  for (int circuit = 1; circuit <= 4; ++circuit)
    for (int type = 0; type <= 4; ++type)
      for (int slope = 0; slope <= 1; ++slope) checkCircuit(circuit, type, slope, false);
}

TEST_CASE("Operator: the circuit models are the linear filter for small signals and saturate when driven") {
  // One sine carrier at 220 Hz into a resonant 12 dB low-pass at 2 kHz: at −60 dBFS every model's output equals
  // Clean's within 1e-3 of its level (the models are the linear filter there; Clean and the SVF models share the
  // damping k, PRD's ladder only the resonance r, so it is compared with its own linear ladder: its drive at 0 dB);
  // at 0 dBFS with +24 dB drive and resonance 90 % MS2 / SMP / PRD saturate (the output differs from the quiet one
  // scaled up by more than 5 % of its peak) and stay bounded.
  auto render = [](int circuit, float level, float drive, float res) {
    opref::Config cfg;
    cfg.filterOn = true;
    cfg.filterCircuit = circuit;
    opref::Params p;
    p.op[0].env = {0.0f, 0.6f, 0.05f, opref::db(-70), 1.0f, 1.0f, opref::db(-70), 1.0f};
    p.op[0].level = level;
    p.volume = 1.0f;
    p.filterFreq = 2000.0f;
    p.filterRes = res;
    p.filterDrive = drive;
    const Program prog = compile(operatorText(cfg, false), kSr);
    Stimulus st = noteStimulus({{220.0, 0, 6000, 1.0}}, 6000, 64);
    setParams(st, prog, p);
    const Outputs o = runBoth(prog, st, "circuit " + std::to_string(circuit));
    return o.at("y")[0];
  };
  const std::vector<float> clean = render(0, opref::db(-60), 1.0f, 0.5f);
  for (int circuit = 1; circuit <= 3; ++circuit) {
    CAPTURE(circuit);
    const std::vector<float> quiet = render(circuit, opref::db(-60), 1.0f, 0.5f);
    double worst = 0.0;
    for (size_t t = 0; t < clean.size(); ++t) worst = std::max(worst, std::fabs(static_cast<double>(quiet[t]) - clean[t]));
    CHECK(worst <= 1e-3 * opref::db(-60));
  }
  for (int circuit = 2; circuit <= 4; ++circuit) {
    CAPTURE(circuit);
    const std::vector<float> quiet = render(circuit, opref::db(-60), opref::db(24), 0.9f);
    const std::vector<float> loud = render(circuit, 1.0f, opref::db(24), 0.9f);
    double diff = 0.0, ref = 0.0;
    for (size_t t = 3000; t < loud.size(); ++t) {
      diff = std::max(diff, std::fabs(static_cast<double>(loud[t]) - 1000.0 * quiet[t]));
      ref = std::max(ref, std::fabs(static_cast<double>(loud[t])));
    }
    CHECK(ref > 0.05);
    CHECK(diff > 0.05 * ref);  // saturation: not the quiet response scaled
    MESSAGE("circuit " << circuit << ": loud peak " << ref << ", deviation from the scaled quiet response " << diff);
    CHECK(ref < 4.0);          // bounded
  }
}

TEST_CASE("Operator: glide, spread, transpose, fixed frequency, time, volume and pan vs the reference") {
  // Glide: notes in different voices one after another; each slides from the previous note's pitch.
  const std::vector<Note> legato{{220.0, 0, 1500, 1.0}, {330.0, 1200, 2700, 0.9}, {165.0, 2400, 4000, 0.8}, {440.0, 2400, 4000, 0.8}};
  opref::Params prm = richParams();
  prm.glideTime = 0.02f;
  prm.spread = 0.6f;
  prm.transpose = -350.0f;
  prm.time = 0.7f;
  prm.op[3].fixed = true;
  prm.op[3].freq = 1234.5f;
  prm.pan = 0.45f;
  for (int variant = 0; variant < 4; ++variant) {
    opref::Config cfg;
    cfg.algorithm = 1 + variant * 3;
    cfg.on = {true, true, true, true};
    cfg.glideOn = variant != 1;
    cfg.spreadOn = variant >= 1;
    cfg.pitchEnvOn = variant == 3;
    opref::Params p = prm;
    p.penvAmount = 300.0f;
    checkVsReference(cfg, p, legato, 4200, 64, "glide/spread variant " + std::to_string(variant));
  }
}

TEST_CASE("Operator: glide slides from the previous note's pitch, linearly in cents, over the glide time") {
  opref::Config cfg;
  cfg.glideOn = true;
  opref::Params prm;
  prm.volume = 1.0f;
  prm.glideTime = 0.05f;  // 2400 samples
  const std::vector<Note> notes{{200.0, 0, 3000, 1.0}, {400.0, 3000, 9000, 1.0}};
  const Program prog = compile(operatorText(cfg), kSr);
  Stimulus st = noteStimulus(notes, 9000, 64);
  setParams(st, prog, prm);
  const Outputs o = runBoth(prog, st, "glide");
  // The second voice's instantaneous frequency from its phase increments (zero crossings are too coarse): use the
  // reference, whose pitch path is the patch's, to read the frequency, and check the curve.
  opref::Operator ref(cfg, prm, 2);
  std::vector<opref::VoiceIn> in(2);
  std::vector<opref::Out> out;
  std::vector<float> y1;
  for (uint64_t t = 0; t < 9000; ++t) {
    for (uint32_t v = 0; v < 2; ++v) {
      in[v].hz = static_cast<float>(signalOf(notes, eng::VoiceSignal::Hz, v, t));
      in[v].cents = static_cast<float>(signalOf(notes, eng::VoiceSignal::Cents, v, t));
      in[v].gate = signalOf(notes, eng::VoiceSignal::Gate, v, t) != 0.0;
      in[v].trigger = signalOf(notes, eng::VoiceSignal::Trigger, v, t) != 0.0;
    }
    ref.tick(in, 0.0f, 120.0f, out);
    y1.push_back(out[1].l);
  }
  double worst = 0.0;
  for (size_t t = 0; t < 9000; ++t) worst = std::max(worst, static_cast<double>(std::fabs(o.at("y")[1][t] - y1[t])));
  CHECK(worst < 1e-5);
  // Count zero crossings in windows: at the onset ~200 Hz, after the glide 400 Hz.
  auto crossings = [&](size_t from, size_t len) {
    int n = 0;
    for (size_t t = from + 1; t < from + len; ++t) n += (o.at("y")[1][t - 1] < 0.0f) != (o.at("y")[1][t] < 0.0f);
    return n;
  };
  CHECK(std::abs(crossings(3000, 480) - 4) <= 1);    // 10 ms near 200 Hz: ~4 crossings
  CHECK(std::abs(crossings(6000, 2400) - 40) <= 1);  // 50 ms at 400 Hz: 40 crossings
}

TEST_CASE("Operator: velocity and key sensitivity scale an operator's level as documented") {
  opref::Config cfg;
  opref::Params prm;
  prm.volume = 1.0f;
  prm.op[0].vel = 1.0f;
  prm.op[0].key = 0.5f;  // +3 dB per octave
  const std::vector<Note> notes{{261.626 * 2.0, 0, 4000, 0.5}, {261.626, 0, 4000, 1.0}};
  const Program prog = compile(operatorText(cfg), kSr);
  Stimulus st = noteStimulus(notes, 4000, 64);
  setParams(st, prog, prm);
  const Outputs o = runBoth(prog, st, "vel/key");
  auto peakOf = [&](uint32_t v) {
    double m = 0.0;
    for (size_t t = 2000; t < 4000; ++t) m = std::max(m, static_cast<double>(std::fabs(o.at("y")[v][t])));
    return m;
  };
  CHECK(peakOf(1) == doctest::Approx(1.0).epsilon(1e-3));                  // velocity 1 at the reference: 0 dB
  CHECK(peakOf(0) == doctest::Approx(0.5 * std::sqrt(2.0)).epsilon(1e-3));  // velocity ½, one octave up: ½ · 2^½
}

// ================================================================================================================
// Pitch: exact frequencies, just ratios, tuned playback
// ================================================================================================================

namespace {

// Frequency of a sine by zero crossings with linear interpolation, over [from, end).
double measureHz(const std::vector<float>& y, size_t from) {
  double first = -1.0, last = -1.0;
  int n = 0;
  for (size_t t = from + 1; t < y.size(); ++t)
    if (y[t - 1] < 0.0f && y[t] >= 0.0f) {
      const double x = static_cast<double>(t - 1) + y[t - 1] / (static_cast<double>(y[t - 1]) - y[t]);
      if (first < 0.0) first = x;
      last = x;
      ++n;
    }
  return n > 1 ? (n - 1) * kSr / (last - first) : 0.0;
}

}  // namespace

TEST_CASE("Operator: an operator plays voice.hz · coarse/den exactly (just ratios), and fixed Hz") {
  struct Case {
    float coarse, den, fine;
    bool fixed;
    double want;  // × note Hz, or Hz when fixed
  };
  const Case cases[] = {{1, 1, 0, false, 1.0}, {3, 2, 0, false, 1.5}, {5, 4, 0, false, 1.25}, {7, 4, 0, false, 1.75},
                        {0, 1, 0, false, 0.5}, {2, 3, 0, false, 2.0 / 3.0}, {1, 1, 500, false, 1.5}, {1, 1, 0, true, 777.0}};
  const double noteHz = 293.6647679;
  for (const Case& c : cases) {
    opref::Config cfg;
    opref::Params prm;
    prm.volume = 1.0f;
    prm.op[0].coarse = c.coarse;
    prm.op[0].den = c.den;
    prm.op[0].fine = c.fine;
    prm.op[0].fixed = c.fixed;
    prm.op[0].freq = 777.0f;
    const Program prog = compile(operatorText(cfg), kSr);
    const std::vector<Note> notes{{noteHz, 0, 1u << 30, 1.0}};
    Stimulus st = noteStimulus(notes, 48000, 64);
    setParams(st, prog, prm);
    const Outputs o = run(prog, Backend::Jit, st);
    REQUIRE(o.error.empty());
    const double want = c.fixed ? c.want : noteHz * c.want;
    const double got = measureHz(o.at("y")[0], 100);
    // The phasor's uint32 increment is round(hz · 2^32 / sr): exact to 2^-32 cycles per sample.
    CHECK_MESSAGE(std::fabs(1200.0 * std::log2(got / want)) < 0.001, "coarse " << c.coarse << "/" << c.den << " fine " << c.fine
                                                                                << ": " << got << " Hz, want " << want);
  }
}

namespace {

// A track patch with a tuning node in front of Operator (the playback lowering's form, T2.6-fix1): Degree notes
// follow the tuning signal; the node's params are the tuning's generator / period / reference.
const char* kTunedTrack = R"(import "devices/operator.pqp"
patch Track {
  param gen = 700  [0..1200]
  param per = 1200 [100..2400]
  param ref = 261.626 [20..2000]
  out y    : audio voice stereo
  out done : bool voice
  t  = tuning_hz(voice.degree, voice.equave, voice.hz, voice.cents, gen, per, 0, ref, table: 0)
  tv = tune_voices(t.hz, t.cents)
  o  = operator(volume: 0dB)
  y  = o
  done = o.done
}
)";

struct TunedRun {
  std::vector<float> y;
  uint64_t rtViolations = 0;
};

// One Degree note through the VoiceEngine on a JIT (or interpreted) image with the tuning's table.
TunedRun playTuned(const pq::tuning::Tuning& tuning, int32_t degree, int32_t equave, float generator, bool jit) {
  const Program prog = compile(kTunedTrack, kSr);
  auto tab = eng::makeTuningTable(tuning);
  REQUIRE_MESSAGE(tab.ok(), (tab.ok() ? "" : tab.error().message));
  const pq::dsp::TuningTable table = std::move(tab.value());
  const pq::dsp::TuningTableView* views[] = {&table.view()};
  eng::Resources res;
  res.tunings = views;
  std::unique_ptr<eng::ProgramImage> img;
  if (jit) {
    pq::jit::CompileOptions o;
    o.maxBlockFrames = 128;
    o.maxVoiceGroups = 2;
    o.resources = &res;
    auto r = jitCompiler().compile(prog.sched, o);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    img = std::move(r.value());
  } else {
    eng::InterpretOptions o;
    o.maxBlockFrames = 128;
    o.maxVoiceGroups = 2;
    o.resources = &res;
    auto r = eng::buildInterpreted(prog.sched, o);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    img = std::move(r.value());
  }
  eng::VoiceEngine ve;
  eng::VoiceConfig cfg;
  cfg.maxVoices = 8;
  const pq::graph::Patch& sp = prog.sched.rewritten ? prog.sched.rewritten->patch : prog.typed.patch;
  cfg.doneSlot = eng::findVoiceSlot(img->io(), sp, "done");
  REQUIRE(ve.prepare(img.get(), cfg).ok());
  uint32_t mixY = 0;  // the left channel of `y`
  for (uint32_t i = ve.numMixes(); i-- > 0;) {
    const auto& slot = img->io().voiceOut[ve.mixSlot(i)];
    if (sp.nodeName(slot.node) == "y" && slot.channel == 0) mixY = i;
  }
  // params: the node's generator (the others at their defaults)
  std::vector<float> params(img->io().params.size(), NAN);
  for (size_t i = 0; i < params.size(); ++i)
    if (sp.nodeName(img->io().params[i].node) == "gen") params[i] = generator;
  const auto staticHz = pq::tuning::resolveHzDouble(pq::tuning::PitchId::fromDegree(degree, equave), tuning, {});
  REQUIRE(staticHz);
  eng::NoteEvent on = eng::noteOn(0, 1, static_cast<float>(*staticHz), 0.8f);
  on.degree = static_cast<float>(degree);
  on.equave = static_cast<float>(equave);
  eng::EventBuffer buf(16);
  TunedRun out;
  pq::DenormalGuard dg;
  constexpr uint32_t kFrames = 24000;
  for (uint32_t done = 0; done < kFrames;) {
    const uint32_t n = std::min(128u, kFrames - done);
    buf.clear();
    if (done == 0) buf.push(on);
    eng::ProcessContext ctx;
    ctx.frames = n;
    ctx.sampleRate = kSr;
    ctx.sampleTime = done;
    ctx.params = params.data();
    ctx.numParams = static_cast<uint32_t>(params.size());
    ctx.events = &buf.list();
    out.rtViolations += pq::alloc_trap::countRtViolations([&] { ve.process(ctx); });
    out.y.insert(out.y.end(), ve.mix(mixY), ve.mix(mixY) + n);
    done += n;
  }
  return out;
}

}  // namespace

TEST_CASE("Operator: tuned playback in 31edo and a MOS follows the tuning signal (interpreter and JIT)") {
  namespace tun = pq::tuning;
  // 31edo: every 5th degree over two equaves, from the static degree pitches (the node passes them through).
  const tun::Tuning edo = tun::Tuning::make(tun::Edo::make(31).value(), kRefHz).value();
  double worst = 0.0;
  for (const bool jit : {false, true})
    for (int32_t d = -31; d <= 31; d += 5) {
      const TunedRun r = playTuned(edo, d, 0, 0.0f, jit);
      CHECK(r.rtViolations == 0);
      const double want = kRefHz * std::exp2(static_cast<double>(d) / 31.0);
      const double got = measureHz(r.y, 200);
      worst = std::max(worst, std::fabs(1200.0 * std::log2(got / want)));
    }
  MESSAGE("31edo: max pitch error " << worst << " cents");
  CHECK(worst < 0.002);
  // A MOS (5L2s, generator defined at 700 ¢) played with the node's generator moved to 696 ¢ (quarter-comma-ish
  // meantone): Operator's pitch is the tuning signal's, not the note's static Hz.
  tun::MosParams mp;
  mp.n = 7;
  mp.period = 1200.0;
  mp.rotation = 1;
  mp.generator = 700.0;
  const tun::Tuning mos = tun::Tuning::make(tun::Mos::make(mp).value(), kRefHz).value();
  mp.generator = 696.0;
  const tun::Mos moved = tun::Mos::make(mp).value();
  worst = 0.0;
  for (const bool jit : {false, true})
    for (int32_t d : {0, 1, 2, 3, 4, 5, 6}) {
      const TunedRun r = playTuned(mos, d, d == 3 ? 1 : 0, 696.0f, jit);
      CHECK(r.rtViolations == 0);
      const double want = kRefHz * std::exp2(moved.centsOf(d, d == 3 ? 1 : 0) / 1200.0);
      const double got = measureHz(r.y, 200);
      worst = std::max(worst, std::fabs(1200.0 * std::log2(got / want)));
    }
  MESSAGE("MOS at 696 c: max pitch error " << worst << " cents");
  CHECK(worst < 0.002);
}

// ================================================================================================================
// Cost structure
// ================================================================================================================

TEST_CASE("Operator: what is off is not compiled (operators, sections, waveforms)") {
  // The device with only its audio and `done` read (exports unread: nothing keeps a section that is off alive).
  auto opsOf = [](const opref::Config& c) {
    const std::string text = operatorText(c, false);
    const Program p = compile(text, kSr);
    std::map<std::string, int> count;
    for (const auto& op : p.sched.ops)
      if (op.kind == pq::graph::OpKind::Kernel) count[pq::dsp::kernelInfo(op.kernel).name]++;
    return std::make_pair(p.sched.ops.size(), count);
  };
  opref::Config def;
  const auto [nDef, cDef] = opsOf(def);
  CHECK(cDef.at("phasor") == 1);  // operator A only
  CHECK(cDef.count("wavetable") == 0);
  CHECK(cDef.count("fast_tan") == 0);  // no filter
  opref::Config all;
  all.on = {true, true, true, true};
  all.filterOn = all.lfoOn = all.pitchEnvOn = all.glideOn = true;
  all.wave = {3, 0, 0, 0};
  const auto [nAll, cAll] = opsOf(all);
  CHECK(cAll.at("phasor") == 5);  // four operators + the LFO
  CHECK(cAll.at("wavetable") == 1);
  CHECK(nAll > 3 * nDef);
  MESSAGE("ops: default " << nDef << ", everything on " << nAll);
}

TEST_CASE("Operator: JIT images of the full configurations need no stack probes (audio-thread safe frames)") {
  struct V {
    const char* name;
    std::array<int, 4> wave;
    bool spread;
    int circuit = 0;
  };
  const V variants[] = {{"full sines", {0, 0, 0, 0}, false},     {"full tables", {3, 8, 17, 19}, false},
                        {"full user", {22, 0, 22, 0}, false},     {"spread sines", {0, 0, 0, 0}, true},
                        {"spread mixed", {22, 3, 0, 20}, true},   {"full MS2", {0, 0, 0, 0}, false, 2},
                        {"full PRD", {0, 0, 0, 0}, false, 4},     {"spread SMP", {22, 3, 0, 20}, true, 3}};
  for (const V& v : variants) {
    opref::Config c;
    c.on = {true, true, true, true};
    c.wave = v.wave;
    c.filterOn = c.lfoOn = c.pitchEnvOn = c.glideOn = true;
    c.filterSlope = 1;
    c.filterCircuit = v.circuit;
    c.spreadOn = v.spread;
    const Program p = compile(operatorText(c, false), kSr);
    pq::jit::CompileOptions o;
    o.maxBlockFrames = 128;
    o.maxVoiceGroups = 9;
    pq::jit::CompileStats stats;
    auto img = jitCompiler().compile(p.sched, o, &stats);
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    CHECK_MESSAGE(!stats.stackProbes, std::string(v.name) << " needs stack probes");
    MESSAGE(std::string(v.name) << ": " << p.sched.ops.size() << " ops, JIT " << stats.totalMs << " ms, "
                                << stats.pairedStages << " paired stages, " << stats.objectBytes << " B");
  }
}
