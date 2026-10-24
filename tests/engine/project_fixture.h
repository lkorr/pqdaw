#pragma once

// T1.x-arch1: a generated project for the per-track compile units (tests/engine/test_project.cpp and
// bench/engine/bench_project.cpp). `numTracks` instrument tracks, each: a 4-operator FM voice with four
// atom-built ADSRs and a vibrato LFO (≈ proto-Operator), then a TPT state-variable filter (per-sample loop) and
// `effects` effect devices cycling through Drive / Echo (feedback delay, block-wise) / Chorus (modulated delay) /
// Shaper / Fir / Comp (envelope follower) / Eq8 (eight TDF-II peaking biquads, per-sample loops) / Verb (4 feedback
// combs + 2 allpasses on delay lines, sub-blocks) with per-track parameters; a volume param and a send export. Tracks 0 … n/2−1 feed
// group A, the rest group B; every track's send feeds a return (Svf + Echo); groups and return feed the master (a
// soft clip), which the device plays. Optional: a sidechain from track `sidechainFrom` into group B's compressor
// key (merges that track with group B: SPEC §15), and a cross-track modulation from track `modFrom`'s LFO export
// into the return's filter cutoff (merges it with the return).
//
// The device library and every track patch are generated as patch-language text and parsed once (patches keep
// their NodeIds, so edits on the Patch IR keep state keys, as the editor's would).

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "pq/graph/infer.h"
#include "pq/graph/parse.h"
#include "pq/graph/project.h"

namespace project_fixture {

namespace graph = pq::graph;

struct Options {
  uint32_t numTracks = 30;
  uint32_t effects = 28;          // effect devices per track after the filter (28: ≈ 680 schedule ops per track)
  int sidechainFrom = -1;         // track index, or −1
  int modFrom = -1;               // track index, or −1
};

struct Project {
  std::vector<std::string> texts;  // sources (kept alive: diagnostics)
  graph::PatchFile devices;
  std::vector<graph::SubcircuitRef> deviceSigs;
  std::vector<std::unique_ptr<graph::Patch>> trackPatches;  // stable addresses
  graph::PatchLibrary library;
  graph::ProjectGraph graph;
  uint32_t groupA = 0, groupB = 0, ret = 0, master = 0;  // track indices
};

inline const char* deviceText() {
  return R"(
patch Adsr {
  in    gate : bool audio voice mono
  param a = 5ms   [0ms..10s]
  param d = 200ms [1ms..10s]
  param s = 0.7   [0..1]
  param r = 300ms [1ms..30s]
  out   out : audio voice mono
  attacking = srff(rise(gate), level >= 0.999 || !gate)
  target    = attacking ? 1.0 : gate ? s : 0.0
  time      = attacking ? a : gate ? d : r
  coef      = 1 - exp(-1 / max(time * sr, 1))
  level     = z1(level + (target - level) * coef)
  out = level
}

patch FmSynth {
  param ratio1  = 2.0 [0.25..16]
  param ratio2  = 3.0 [0.25..16]
  param ratio3  = 0.5 [0.25..16]
  param index1  = 1.0 [0..20]
  param index2  = 0.5 [0..20]
  param index3  = 0.3 [0..20]
  param attack  = 5ms   [0ms..10s]
  param release = 150ms [1ms..30s]
  param level   = 0.2 [0..1]
  param vib     = 0.002 [0..0.05]
  out   out : audio voice mono
  lfo = sine(phasor(5.5Hz)) * vib
  hz  = voice.hz * (1 + lfo)
  e1  = Adsr(voice.gate, a: attack, d: 300ms, s: 0.5, r: release)
  e2  = Adsr(voice.gate, a: 2ms, d: 150ms, s: 0.3, r: release)
  e3  = Adsr(voice.gate, a: 1ms, d: 80ms, s: 0.0, r: release)
  m3  = sine(phasor(hz * ratio3)) * index3 * e3
  m2  = sine(phasor(hz * ratio2 + m3 * hz)) * index2 * e2
  m1  = sine(phasor(hz * ratio1 + m2 * hz)) * index1 * e1
  car = sine(phasor(hz + m1 * hz))
  env = Adsr(voice.gate, a: attack, d: 200ms, s: 0.7, r: release)
  out = car * env * voice.velocity * level
}

patch Svf {
  in    x : audio global mono
  param cutoff = 2000Hz [20Hz..20kHz]
  param res = 0.3 [0..0.98]
  out   lp : audio global mono
  g   = tan(3.141592653589793 * cutoff / sr)
  k   = 2 - 2 * res
  h   = 1 / (1 + g * (g + k))
  s1  = z1(n1)
  s2  = z1(n2)
  hp  = (x - s1 * (g + k) - s2) * h
  v1  = g * hp
  bp  = v1 + s1
  n1  = bp + v1
  v2  = g * bp
  lpv = v2 + s2
  n2  = lpv + v2
  lp  = lpv
}

patch Drive {
  in    x : audio global mono
  param drive = 2.0 [0.1..10]
  param mix = 0.5 [0..1]
  out   y : audio global mono
  y = x + (tanh(x * drive) * 0.5 - x) * mix
}

patch Echo {
  in    x : audio global mono
  param time = 120ms [20ms..1s]
  param fb = 0.35 [0..0.9]
  param mix = 0.25 [0..1]
  out   y : audio global mono
  d = delay(x + d * fb, time, 1s)
  y = x + d * mix
}

patch Chorus {
  in    x : audio global mono
  param rate = 0.7Hz [0.05Hz..5Hz]
  param depth = 3ms [0ms..8ms]
  param mix = 0.3 [0..1]
  out   y : audio global mono
  lfo = sine(phasor(rate)) * 0.5 + 0.5
  w   = delay(x, 10ms + depth * lfo, 25ms)
  y   = x + (w - x) * mix
}

patch Shaper {
  in    x : audio global mono
  param amount = 0.3 [0..1]
  out   y : audio global mono
  c = clip(x, -1, 1)
  y = x + (c - c * c * c * 0.3333 - x) * amount
}

patch Fir {
  in    x : audio global mono
  param tilt = 0.2 [-1..1]
  out   y : audio global mono
  x1 = z1(x)
  x2 = z1(x1)
  y  = x * (0.5 + 0.5 * tilt) + x1 * 0.35 + x2 * (0.15 - 0.5 * tilt)
}

patch Comp {
  in    x : audio global mono
  in    key : audio global mono = 0
  param threshold = 0.3 [0.01..1]
  param ratio = 3.0 [1..20]
  param keyed = 0.0 [0..1]
  out   y : audio global mono
  det = abs(x) + abs(key) * keyed
  env = z1(env + (det - env) * (det > env ? 0.01 : 0.0005))
  over = max(env / threshold, 1)
  g = pow(over, 1 / ratio - 1)
  y = x * g
}

patch Peak {
  in    x : audio global mono
  param freq = 1000Hz [20Hz..20kHz]
  param gain = 1.0 [0.25..4]
  param q = 0.7 [0.1..10]
  out   y : audio global mono
  a  = sqrt(gain)
  w  = 6.283185307179586 * freq / sr
  sw = sin(w)
  cw = sin(w + 1.5707963267948966)
  al = sw / (2 * q)
  ia = 1 / (1 + al / a)
  n0 = (1 + al * a) * ia
  n1 = -2 * cw * ia
  n2 = (1 - al * a) * ia
  m2 = (1 - al / a) * ia
  v  = n0 * x + s1
  s1 = z1(n1 * x - n1 * v + s2)
  s2 = z1(n2 * x - m2 * v)
  y  = v
}

patch Eq8 {
  in    x : audio global mono
  param low = 1.2 [0.25..4]
  param mid = 0.8 [0.25..4]
  param high = 1.1 [0.25..4]
  out   y : audio global mono
  b1 = Peak(x, freq: 60Hz, gain: low, q: 0.7)
  b2 = Peak(b1, freq: 150Hz, gain: low, q: 0.9)
  b3 = Peak(b2, freq: 400Hz, gain: mid, q: 1.2)
  b4 = Peak(b3, freq: 900Hz, gain: mid, q: 1.0)
  b5 = Peak(b4, freq: 2000Hz, gain: mid, q: 1.0)
  b6 = Peak(b5, freq: 4000Hz, gain: high, q: 0.8)
  b7 = Peak(b6, freq: 8000Hz, gain: high, q: 0.7)
  y  = Peak(b7, freq: 14000Hz, gain: high, q: 0.7)
}

patch Verb {
  in    x : audio global mono
  param size = 1.0 [0.5..2]
  param fb = 0.6 [0..0.9]
  param mix = 0.2 [0..1]
  out   y : audio global mono
  c1 = delay(x + c1 * fb, 29.7ms * size, 80ms)
  c2 = delay(x + c2 * fb, 37.1ms * size, 80ms)
  c3 = delay(x + c3 * fb, 41.1ms * size, 90ms)
  c4 = delay(x + c4 * fb, 43.7ms * size, 90ms)
  m  = (c1 + c2 + c3 + c4) * 0.25
  z1a = delay(w1, 5ms, 10ms)
  w1 = m + z1a * 0.5
  a1 = z1a - w1 * 0.5
  z1b = delay(w2, 1.7ms, 10ms)
  w2 = a1 + z1b * 0.5
  a2 = z1b - w2 * 0.5
  y  = x + (a2 - x) * mix
}

patch MasterClip {
  in    in : audio global mono
  param drive = 0.8 [0.1..4]
  out   out : audio global mono
  out = tanh(in * drive)
}
)";
}

// One track's patch text: FmSynth → Svf → effects… → vol; `export send`, `export lfo` (a slow LFO, for modulation).
inline std::string trackText(uint32_t k, uint32_t effects) {
  char buf[512];
  std::string s = "patch Track" + std::to_string(k) + " {\n";
  std::snprintf(buf, sizeof buf, "  param vol = %.3f [0..1]\n  param sendAmt = %.3f [0..1]\n  out out : audio global mono\n",
                0.4 + 0.01 * (k % 10), 0.1 + 0.01 * (k % 7));
  s += buf;
  std::snprintf(buf, sizeof buf,
                "  syn = FmSynth(ratio1: %.3f, ratio2: %.3f, index1: %.3f, index2: %.3f, release: %.0fms, level: 0.15)\n",
                1.0 + 0.25 * (k % 8), 2.0 + 0.5 * (k % 5), 0.5 + 0.1 * (k % 9), 0.2 + 0.05 * (k % 6), 80.0 + 10.0 * (k % 5));
  s += buf;
  std::snprintf(buf, sizeof buf, "  f = Svf(syn, cutoff: %.0fHz, res: %.2f)\n", 800.0 + 150.0 * (k % 12), 0.1 + 0.05 * (k % 10));
  s += buf;
  std::string prev = "f";
  for (uint32_t e = 0; e < effects; ++e) {
    const uint32_t v = k * 7 + e * 3;
    const std::string name = "e" + std::to_string(e);
    switch ((k + e) % 8) {
      case 0: std::snprintf(buf, sizeof buf, "  %s = Drive(%s, drive: %.2f, mix: %.2f)\n", name.c_str(), prev.c_str(), 1.0 + 0.3 * (v % 7), 0.2 + 0.05 * (v % 9)); break;
      case 1: std::snprintf(buf, sizeof buf, "  %s = Echo(%s, time: %.0fms, fb: %.2f, mix: %.2f)\n", name.c_str(), prev.c_str(), 60.0 + 17.0 * (v % 11), 0.2 + 0.05 * (v % 8), 0.15 + 0.02 * (v % 5)); break;
      case 2: std::snprintf(buf, sizeof buf, "  %s = Chorus(%s, rate: %.2fHz, depth: %.1fms, mix: %.2f)\n", name.c_str(), prev.c_str(), 0.2 + 0.13 * (v % 9), 1.0 + 0.5 * (v % 9), 0.2 + 0.03 * (v % 7)); break;
      case 3: std::snprintf(buf, sizeof buf, "  %s = Shaper(%s, amount: %.2f)\n", name.c_str(), prev.c_str(), 0.1 + 0.07 * (v % 10)); break;
      case 4: std::snprintf(buf, sizeof buf, "  %s = Fir(%s, tilt: %.2f)\n", name.c_str(), prev.c_str(), -0.5 + 0.1 * (v % 11)); break;
      case 5: std::snprintf(buf, sizeof buf, "  %s = Comp(%s, threshold: %.2f, ratio: %.1f)\n", name.c_str(), prev.c_str(), 0.1 + 0.05 * (v % 9), 2.0 + 0.5 * (v % 7)); break;
      case 6: std::snprintf(buf, sizeof buf, "  %s = Eq8(%s, low: %.2f, mid: %.2f, high: %.2f)\n", name.c_str(), prev.c_str(), 0.6 + 0.1 * (v % 9), 0.7 + 0.08 * (v % 7), 0.8 + 0.06 * (v % 8)); break;
      default: std::snprintf(buf, sizeof buf, "  %s = Verb(%s, size: %.2f, fb: %.2f, mix: %.2f)\n", name.c_str(), prev.c_str(), 0.7 + 0.1 * (v % 9), 0.4 + 0.04 * (v % 8), 0.1 + 0.02 * (v % 6)); break;
    }
    s += buf;
    prev = name;
  }
  s += "  y = " + prev + " * vol\n  out = y\n  export send = y * sendAmt\n";
  std::snprintf(buf, sizeof buf, "  export lfo = sine(phasor(%.2fHz)) * 0.5 + 0.5\n}\n", 0.3 + 0.1 * (k % 5));
  s += buf;
  return s;
}

inline const char* busText() {
  return R"(
patch GroupA {
  in    in : audio global mono
  param vol = 0.8 [0..1]
  out   out : audio global mono
  c = Comp(in, threshold: 0.4, ratio: 2.5)
  out = Fir(c, tilt: 0.1) * vol
}

patch GroupB {
  in    in : audio global mono
  in    key : audio global mono = 0
  param vol = 0.8 [0..1]
  param keyed = 0.5 [0..1]
  out   out : audio global mono
  c = Comp(in, key, threshold: 0.35, ratio: 4, keyed: keyed)
  out = c * vol
}

patch ReturnFx {
  in    in : audio global mono
  in    cutoffMod : audio global mono = 0
  param cutoff = 3000Hz [20Hz..20kHz]
  out   out : audio global mono
  f = Svf(in, cutoff: cutoff * (1 + cutoffMod), res: 0.2)
  out = Echo(f, time: 210ms, fb: 0.5, mix: 0.6)
}

patch Master {
  in    in : audio global mono
  out   out : audio global mono
  out = MasterClip(in * 0.5, drive: 0.9)
}
)";
}

inline const graph::Patch* parseInto(Project& p, std::string text, const std::string& name, graph::PatchFile* fileOut) {
  p.texts.push_back(std::move(text));
  graph::ParseOptions po;
  po.externals = p.deviceSigs;
  auto r = graph::parseDetailed(p.texts.back(), name, po);
  if (!r.ok()) {
    std::fprintf(stderr, "%s\n", graph::formatDiagnostic(p.texts.back(), r.diagnostics.front()).c_str());
    return nullptr;
  }
  if (fileOut != nullptr) {
    *fileOut = std::move(r.file);
    return nullptr;
  }
  p.trackPatches.push_back(std::make_unique<graph::Patch>(std::move(r.file.patches[0])));
  return p.trackPatches.back().get();
}

inline std::unique_ptr<Project> makeProject(const Options& o) {
  auto p = std::make_unique<Project>();
  {
    p->texts.push_back(deviceText());
    auto r = graph::parseDetailed(p->texts.back(), "devices.pqp");
    if (!r.ok()) {
      std::fprintf(stderr, "%s\n", graph::formatDiagnostic(p->texts.back(), r.diagnostics.front()).c_str());
      return nullptr;
    }
    p->devices = std::move(r.file);
    p->library.add(p->devices);
    p->deviceSigs = graph::signatures(p->devices);
  }
  graph::ProjectGraph& g = p->graph;
  g.library = &p->library;
  for (uint32_t k = 0; k < o.numTracks; ++k) {
    const graph::Patch* tp = parseInto(*p, trackText(k, o.effects), "track" + std::to_string(k) + ".pqp", nullptr);
    if (tp == nullptr) return nullptr;
    g.tracks.push_back({k + 1, "Track" + std::to_string(k), tp, graph::TrackRole::Instrument, k});
  }
  // The bus tracks share one file.
  p->texts.push_back(busText());
  graph::ParseOptions po;
  po.externals = p->deviceSigs;
  auto r = graph::parseDetailed(p->texts.back(), "buses.pqp", po);
  if (!r.ok()) {
    std::fprintf(stderr, "%s\n", graph::formatDiagnostic(p->texts.back(), r.diagnostics.front()).c_str());
    return nullptr;
  }
  for (graph::Patch& bp : r.file.patches) p->trackPatches.push_back(std::make_unique<graph::Patch>(std::move(bp)));
  const auto nb = static_cast<uint32_t>(p->trackPatches.size());
  auto addBus = [&](const char* name, uint32_t patchIdx, graph::TrackRole role) {
    g.tracks.push_back({1000 + static_cast<uint32_t>(g.tracks.size()), name, p->trackPatches[patchIdx].get(), role, graph::kNone});
    return static_cast<uint32_t>(g.tracks.size() - 1);
  };
  p->groupA = addBus("GroupA", nb - 4, graph::TrackRole::Group);
  p->groupB = addBus("GroupB", nb - 3, graph::TrackRole::Group);
  p->ret = addBus("Return", nb - 2, graph::TrackRole::Return);
  p->master = addBus("Master", nb - 1, graph::TrackRole::Master);
  using W = graph::WireKind;
  for (uint32_t k = 0; k < o.numTracks; ++k) {
    g.wires.push_back({{k, "out"}, {k < o.numTracks / 2 ? p->groupA : p->groupB, "in"}, W::Audio});
    g.wires.push_back({{k, "send"}, {p->ret, "in"}, W::Audio});
  }
  g.wires.push_back({{p->groupA, "out"}, {p->master, "in"}, W::Audio});
  g.wires.push_back({{p->groupB, "out"}, {p->master, "in"}, W::Audio});
  g.wires.push_back({{p->ret, "out"}, {p->master, "in"}, W::Audio});
  if (o.sidechainFrom >= 0) {
    g.wires.push_back({{static_cast<uint32_t>(o.sidechainFrom), "out"}, {p->groupB, "key"}, W::Sidechain});
  }
  if (o.modFrom >= 0) g.wires.push_back({{static_cast<uint32_t>(o.modFrom), "lfo"}, {p->ret, "cutoffMod"}, W::Modulation});
  g.output = {p->master, "out"};
  return p;
}

}  // namespace project_fixture
