#pragma once

// Tests only (T1b.3): compile a small test patch that calls stdlib subcircuits (imports resolved against stdlib/),
// run it on the interpreter or the JIT with a caller-defined stimulus, and collect its outputs per voice.
//
//   Program p = compile(R"(import "filters/svf.pqp"
//                          patch T { in x : audio  in c : audio  out y : audio  y = svf(x, cutoff: c).lp })");
//   Stimulus st;  st.input = [](uint32_t ord, uint32_t voice, uint64_t t) { … };
//   Outputs o = run(p, Backend::Jit, st);   o.at("y")[voice][t]
//
// Root `in`s are addressed by ordinal (declaration order; `inputCh` also by channel); bool inputs are 0/1 (written as
// lane masks). Channel c > 0 of a multichannel output is named "<name>[c]". Params are
// set by name (normalised units). Global patches use voice 0; patches with voice stages run `voices` voices in
// ceil(voices / 8) groups (lanes past `voices` are left out of the lane mask). The transport plays at 120 BPM from
// beat `transportBeat0`. Every process() call runs inside an RtScope (Outputs::rtViolations counts allocations).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../graph/text_util.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/kernel.h"
#include "pq/engine/interpreter.h"
#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/schedule.h"
#include "pq/jit/compiler.h"

namespace stdlib_test {

namespace fs = std::filesystem;
namespace g = pq::graph;
namespace eng = pq::engine;

inline fs::path stdlibDir() { return fs::weakly_canonical(text_util::stdlibDir()); }

// Every patch file of stdlib/, parsed once (the library the test patches' calls resolve into).
struct Stdlib {
  std::vector<std::string> texts;
  std::vector<g::PatchFile> files;
  g::PatchLibrary library;
  text_util::DirLoader loader{stdlibDir()};
  Stdlib() {
    const auto paths = text_util::pqpFiles(stdlibDir());
    texts.reserve(paths.size());
    files.reserve(paths.size());
    for (const fs::path& p : paths) {
      texts.push_back(text_util::readFile(p));
      g::ParseOptions po;
      po.imports = &loader;
      auto r = g::parse(texts.back(), p.string(), po);
      if (!r) {
        std::fprintf(stderr, "%s\n", r.error().message.c_str());
        std::abort();
      }
      files.push_back(std::move(r.value()));
    }
    for (const g::PatchFile& f : files) library.add(f);
  }
};
inline Stdlib& stdlib() {
  static Stdlib s;
  return s;
}

struct Program {
  std::string text;
  g::PatchFile file;
  g::TypedPatch typed;
  g::Schedule sched;
  std::vector<std::string> inputs;  // root `in` names by ordinal
};

// Compiles the first patch of `text` (a file in stdlib/'s directory, so `import "filters/svf.pqp"` works).
inline Program compile(const std::string& text, double sr = 48000.0) {
  Program p;
  p.text = text;
  g::ParseOptions po;
  po.imports = &stdlib().loader;
  const std::string name = (stdlibDir() / "__t1b3_test.pqp").string();
  auto parsed = g::parse(p.text, name, po);
  if (!parsed) {
    std::fprintf(stderr, "%s\n", parsed.error().message.c_str());
    std::abort();
  }
  p.file = std::move(parsed.value());
  g::PatchLibrary lib = stdlib().library;
  lib.add(p.file);
  g::InferOptions io;
  io.library = &lib;
  auto inf = g::infer(p.file.patches[0], io);
  if (!inf) {
    std::fprintf(stderr, "%s\n", inf.error().message.c_str());
    std::abort();
  }
  g::OptimiseOptions oo;
  oo.sampleRate = sr;
  auto opt = g::optimise(std::move(inf.value()), oo);
  if (!opt) {
    std::fprintf(stderr, "%s\n", opt.error().message.c_str());
    std::abort();
  }
  p.typed = std::move(opt.value());
  g::ScheduleOptions so;
  so.sampleRate = sr;
  auto s = g::schedule(p.typed, so);
  if (!s) {
    std::fprintf(stderr, "%s\n", s.error().message.c_str());
    std::abort();
  }
  p.sched = std::move(s.value());
  const g::Patch& root = p.file.patches[0];
  for (g::NodeIndex n = 0; n < root.nodeCount(); ++n)
    if (root.node(n).kind == g::NodeKind::Input) p.inputs.push_back(root.nodeName(n));
  return p;
}

enum class Backend { Interp, Jit };
inline const char* toText(Backend b) { return b == Backend::Jit ? "jit" : "interp"; }

inline pq::jit::Compiler& jitCompiler() {
  static std::unique_ptr<pq::jit::Compiler> c = [] {
    auto r = pq::jit::Compiler::create();
    if (!r) {
      std::fprintf(stderr, "%s\n", r.error().message.c_str());
      std::abort();
    }
    return std::move(r.value());
  }();
  return *c;
}

inline pq::Result<std::unique_ptr<eng::ProgramImage>> build(const Program& p, Backend b, uint32_t maxBlock,
                                                            uint32_t groups, eng::SleepMode sleep = eng::SleepMode::On) {
  if (b == Backend::Jit) {
    pq::jit::CompileOptions o;
    o.maxBlockFrames = maxBlock;
    o.maxVoiceGroups = groups;
    o.sleep = sleep;
    return jitCompiler().compile(p.sched, o);
  }
  eng::InterpretOptions o;
  o.maxBlockFrames = maxBlock;
  o.maxVoiceGroups = groups;
  o.sleep = sleep;
  return eng::buildInterpreted(p.sched, o);
}

struct Stimulus {
  uint32_t frames = 4096;
  uint32_t block = 64;
  uint32_t voices = 0;  // 0: no voice groups
  // Root input `ordinal` for `voice` (0 for global inputs) at sample t.
  std::function<double(uint32_t ordinal, uint32_t voice, uint64_t t)> input;
  // Multichannel inputs (T4.9): when set, used instead of `input`, with the channel of the input's slot.
  std::function<double(uint32_t ordinal, uint32_t channel, uint32_t voice, uint64_t t)> inputCh;
  // voice.* signals (Hz, Gate, …) per voice; default: 220 Hz, gate held, velocity 1.
  std::function<double(eng::VoiceSignal sig, uint32_t voice, uint64_t t)> voiceSignal;
  std::map<std::string, double> params;  // by name; others keep their defaults
  // Block-rate automation (T4.x-fix-dsp): when set, called at each block's first frame for every param; a finite
  // result replaces the param's value from that block on (NaN: keep it).
  std::function<double(const std::string& name, uint64_t t0)> paramAt;
  double transportBeat0 = 0.0;
  double bpm = 120.0;
  eng::SleepMode sleep = eng::SleepMode::On;  // T1.x-silence: the images sleep as the engine runs them by default
};

struct Outputs {
  std::map<std::string, std::vector<std::vector<float>>> byName;  // [voice][t]
  uint64_t rtViolations = 0;
  eng::SleepStats sleep;  // the image's sleep statistics after the run (T4.x-fix-dsp)
  std::string error;
  const std::vector<std::vector<float>>& at(const std::string& n) const {
    const auto it = byName.find(n);
    if (it == byName.end()) {
      std::fprintf(stderr, "no output '%s'\n", n.c_str());
      std::abort();
    }
    return it->second;
  }
};

inline double defaultVoiceSignal(eng::VoiceSignal sig, uint32_t, uint64_t) {
  switch (sig) {
    case eng::VoiceSignal::Hz: return 220.0;
    case eng::VoiceSignal::Gate: return 1.0;
    case eng::VoiceSignal::Velocity: return 1.0;
    default: return 0.0;
  }
}

inline Outputs run(const Program& p, Backend backend, const Stimulus& st) {
  Outputs res;
  const uint32_t B = st.block;
  const uint32_t groups = st.voices ? (st.voices + 7) / 8 : 0;
  auto built = build(p, backend, B, std::max<uint32_t>(groups, 1), st.sleep);
  if (!built) {
    res.error = built.error().message;
    return res;
  }
  eng::ProgramImage& img = **built;
  const eng::ImageInterface& face = img.io();
  const g::TypedPatch& tp = p.typed;
  auto inBool = [&](uint32_t ord) {
    uint32_t k = 0;
    for (g::NodeIndex n = 0; n < tp.patch.nodeCount(); ++n)
      if (tp.patch.node(n).kind == g::NodeKind::Input && k++ == ord) return tp.outputType(n, 0).kind == g::Kind::Bool;
    return false;
  };
  auto outBool = [&](const eng::IoSlot& s) { return tp.inputType(s.node, 0).kind == g::Kind::Bool; };
  // Output name; channel c > 0 of a multichannel output is "<name>[c]".
  auto slotName = [&](const eng::IoSlot& s) {
    const std::string n = tp.patch.nodeName(s.node);
    return s.channel == 0 ? n : n + "[" + std::to_string(s.channel) + "]";
  };

  std::vector<std::vector<float>> gIn(face.globalIn.size(), std::vector<float>(B)), gOut(face.globalOut.size(), std::vector<float>(B));
  std::vector<const float*> gInP;
  std::vector<float*> gOutP;
  for (auto& b : gIn) gInP.push_back(b.data());
  for (auto& b : gOut) gOutP.push_back(b.data());
  std::vector<std::vector<std::vector<float>>> vSig(groups), vIn(groups), vOut(groups);
  std::vector<std::vector<const float*>> vSigP(groups), vInP(groups);
  std::vector<std::vector<float*>> vOutP(groups);
  std::vector<eng::VoiceGroupIO> gio(groups);
  for (uint32_t gi = 0; gi < groups; ++gi) {
    vSig[gi].assign(eng::kVoiceSignalCount, std::vector<float>(static_cast<size_t>(B) * 8));
    vIn[gi].assign(face.voiceIn.size(), std::vector<float>(static_cast<size_t>(B) * 8));
    vOut[gi].assign(face.voiceOut.size(), std::vector<float>(static_cast<size_t>(B) * 8));
    for (auto& b : vSig[gi]) vSigP[gi].push_back(b.data());
    for (auto& b : vIn[gi]) vInP[gi].push_back(b.data());
    for (auto& b : vOut[gi]) vOutP[gi].push_back(b.data());
    const uint32_t lanes = std::min(8u, st.voices - gi * 8);
    gio[gi].laneMask = lanes == 8 ? 0xFFu : (1u << lanes) - 1u;
    gio[gi].signals = vSigP[gi].data();
    gio[gi].in = vInP[gi].data();
    gio[gi].numIn = static_cast<uint32_t>(vInP[gi].size());
    gio[gi].out = vOutP[gi].data();
    gio[gi].numOut = static_cast<uint32_t>(vOutP[gi].size());
  }
  eng::VoiceTable vt;
  vt.groups = gio.data();
  vt.numGroups = groups;

  std::vector<float> params(face.params.size());
  for (size_t i = 0; i < params.size(); ++i) {
    const auto it = st.params.find(tp.patch.nodeName(face.params[i].node));
    params[i] = it == st.params.end() ? NAN : static_cast<float>(it->second);
  }
  for (const auto& [name, v] : st.params) {
    bool found = false;
    for (const auto& ps : face.params) found |= tp.patch.nodeName(ps.node) == name;
    if (!found) {
      res.error = "no param '" + name + "'";
      return res;
    }
  }
  const auto voiceSig = st.voiceSignal ? st.voiceSignal : defaultVoiceSignal;
  auto toLane = [](bool isBool, double x) { return isBool ? pq::dsp::boolMask(x != 0) : static_cast<float>(x); };

  for (uint32_t t0 = 0; t0 < st.frames; t0 += B) {
    const uint32_t n = std::min(B, st.frames - t0);
    if (st.paramAt)
      for (size_t i = 0; i < params.size(); ++i) {
        const double v = st.paramAt(tp.patch.nodeName(face.params[i].node), t0);
        if (std::isfinite(v)) params[i] = static_cast<float>(v);
      }
    for (uint32_t s = 0; s < face.globalIn.size(); ++s) {
      const uint32_t ord = face.globalIn[s].ordinal, ch = face.globalIn[s].channel;
      const bool b = inBool(ord);
      for (uint32_t f = 0; f < n; ++f) gIn[s][f] = toLane(b, st.inputCh ? st.inputCh(ord, ch, 0, t0 + f) : st.input(ord, 0, t0 + f));
    }
    for (uint32_t gi = 0; gi < groups; ++gi) {
      for (uint32_t sig = 0; sig < eng::kVoiceSignalCount; ++sig) {
        const auto vs = static_cast<eng::VoiceSignal>(sig);
        const bool b = vs == eng::VoiceSignal::Gate || vs == eng::VoiceSignal::Trigger;
        for (uint32_t f = 0; f < n; ++f)
          for (uint32_t l = 0; l < 8; ++l) {
            const uint32_t v = gi * 8 + l;
            vSig[gi][sig][static_cast<size_t>(f) * 8 + l] = v < st.voices ? toLane(b, voiceSig(vs, v, t0 + f)) : 0.0f;
          }
      }
      for (uint32_t s = 0; s < face.voiceIn.size(); ++s) {
        const uint32_t ord = face.voiceIn[s].ordinal, ch = face.voiceIn[s].channel;
        const bool b = inBool(ord);
        for (uint32_t f = 0; f < n; ++f)
          for (uint32_t l = 0; l < 8; ++l) {
            const uint32_t v = gi * 8 + l;
            const double x = v < st.voices ? (st.inputCh ? st.inputCh(ord, ch, v, t0 + f) : st.input(ord, v, t0 + f)) : 0.0;
            vIn[gi][s][static_cast<size_t>(f) * 8 + l] = v < st.voices ? toLane(b, x) : 0.0f;
          }
      }
    }
    eng::ProcessContext ctx;
    ctx.frames = n;
    ctx.sampleRate = p.sched.sampleRate;
    ctx.sampleTime = t0;
    ctx.transport.bpm = st.bpm;
    ctx.transport.beatsPerFrame = st.bpm / 60.0 / p.sched.sampleRate;
    ctx.transport.beat = st.transportBeat0 + static_cast<double>(t0) * ctx.transport.beatsPerFrame;
    ctx.transport.playing = true;
    ctx.in = gInP.data();
    ctx.numIn = static_cast<uint32_t>(gInP.size());
    ctx.out = gOutP.data();
    ctx.numOut = static_cast<uint32_t>(gOutP.size());
    ctx.params = params.data();
    ctx.numParams = static_cast<uint32_t>(params.size());
    ctx.voices = groups ? &vt : nullptr;
    res.rtViolations += pq::alloc_trap::countRtViolations([&] {
      pq::DenormalGuard dg;
      img.process(ctx);
    });
    for (uint32_t s = 0; s < face.globalOut.size(); ++s) {
      auto& dst = res.byName[slotName(face.globalOut[s])];
      dst.resize(1);
      const bool b = outBool(face.globalOut[s]);
      for (uint32_t f = 0; f < n; ++f) dst[0].push_back(b ? (pq::dsp::maskIsTrue(gOut[s][f]) ? 1.0f : 0.0f) : gOut[s][f]);
    }
    for (uint32_t s = 0; s < face.voiceOut.size(); ++s) {
      auto& dst = res.byName[slotName(face.voiceOut[s])];
      dst.resize(st.voices);
      const bool b = outBool(face.voiceOut[s]);
      for (uint32_t v = 0; v < st.voices; ++v)
        for (uint32_t f = 0; f < n; ++f) {
          const float x = vOut[v / 8][s][static_cast<size_t>(f) * 8 + v % 8];
          dst[v].push_back(b ? (pq::dsp::maskIsTrue(x) ? 1.0f : 0.0f) : x);
        }
    }
  }
  if (const eng::SleepStats* ss = img.sleepStats()) res.sleep = *ss;
  return res;
}

// Largest |a − b| over two equally long signals (inf when a NaN / inf appears in only one, or lengths differ).
struct Diff {
  double max = 0.0;
  size_t at = 0;
};
inline Diff maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
  Diff d;
  if (a.size() != b.size()) return {INFINITY, 0};
  for (size_t i = 0; i < a.size(); ++i) {
    const bool fa = std::isfinite(a[i]), fb = std::isfinite(b[i]);
    const double e = fa && fb ? std::fabs(static_cast<double>(a[i]) - b[i]) : (fa == fb && (a[i] == b[i] || (a[i] != a[i] && b[i] != b[i])) ? 0.0 : INFINITY);
    if (e > d.max) d = {e, i};
  }
  return d;
}

inline double peak(const std::vector<float>& a) {
  double m = 0.0;
  for (float x : a) m = std::max(m, std::fabs(static_cast<double>(x)));
  return m;
}

}  // namespace stdlib_test
