#pragma once

// Tests only (T1.7): compile .pqp patches to Schedules, drive an interpreted ProgramImage block by block with a
// deterministic stimulus, run the reference evaluator (ref_eval.h) on the same stimulus, and compare.
//
// Comparison metric: per sample and stream, err = |interp − ref| / max(1, |ref|) (absolute below 1, relative
// above); both NaN, or the same infinity, count as equal; NaN/inf in only one of them is a mismatch (err = inf).
// Bool outputs are compared as 0/1. Streams: every global out/export slot, every voice out/export slot for each
// active voice, every (send bus, channel).

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
#include "pq/dsp/tables.h"
#include "pq/engine/interpreter.h"
#include "pq/graph/optimise.h"
#include "pq/graph/schedule.h"
#include "ref_eval.h"

namespace interp_test {

using namespace pq::graph;
namespace eng = pq::engine;
namespace fs = std::filesystem;

inline fs::path patchesDir() { return fs::path(PQ_TEST_DATA_DIR) / "engine" / "patches"; }

// ---- compiling ------------------------------------------------------------------------------------------------
struct Compiled {
  std::string file, name;
  TypedPatch typed;
  Schedule sched;
};

// Every patch of `path` that infers, optimises and schedules (imports are loaded recursively, relative to the
// importing file). Patches that do not compile are listed in `skipped`.
inline std::vector<Compiled> compileFile(const fs::path& path, double sr, std::vector<std::string>& skipped) {
  std::vector<Compiled> out;
  text_util::DirLoader loader(path.parent_path());
  std::vector<std::unique_ptr<ParseResult>> files;
  std::vector<std::string> texts;
  std::vector<fs::path> pending{path};
  std::vector<std::string> seen;
  while (!pending.empty()) {
    const fs::path p = fs::weakly_canonical(pending.front());
    pending.erase(pending.begin());
    if (std::find(seen.begin(), seen.end(), p.string()) != seen.end()) continue;
    seen.push_back(p.string());
    texts.push_back(text_util::readFile(p));
    ParseOptions po;
    po.imports = &loader;
    files.push_back(std::make_unique<ParseResult>(parseDetailed(texts.back(), p.string(), po)));
    if (!files.back()->ok()) {
      skipped.push_back(p.string() + ": parse error");
      if (files.size() == 1) return out;
      continue;
    }
    for (const std::string& imp : files.back()->file.imports) pending.push_back(p.parent_path() / imp);
  }
  PatchLibrary lib;
  for (const auto& f : files)
    if (f->ok()) lib.add(f->file);
  const PatchFile& root = files.front()->file;
  for (const Patch& patch : root.patches) {
    InferOptions io;
    io.library = &lib;
    InferResult inf = inferDetailed(patch, io);
    std::vector<RootInput> roots;
    if (!inf.ok()) {
      // The engine may leave a root `in` with a default unconnected (it then reads its default expression, e.g.
      // `in k : bool = transport.playing`); retry that way before giving up.
      const Adjacency adj = patch.adjacency();
      for (NodeIndex n = 0; n < patch.nodeCount(); ++n) {
        if (patch.node(n).kind != NodeKind::Input) continue;
        RootInput ri;
        ri.connected = adj.driver[patch.flatInput(n, 0)] == kNone;
        roots.push_back(ri);
      }
      io.inputs = roots;
      inf = inferDetailed(patch, io);
    }
    if (!inf.ok()) {
      skipped.push_back(path.filename().string() + ":" + patch.name() + ": " + inf.diagnostics.front().message);
      continue;
    }
    OptimiseOptions oo;
    oo.sampleRate = sr;
    auto opt = optimise(std::move(inf.typed), oo);
    if (!opt.ok()) {
      skipped.push_back(path.filename().string() + ":" + patch.name() + ": " + opt.error().message);
      continue;
    }
    ScheduleOptions so;
    so.sampleRate = sr;
    ScheduleResult sr2 = scheduleDetailed(opt.value(), so);
    if (!sr2.ok()) {
      skipped.push_back(path.filename().string() + ":" + patch.name() + ": " + sr2.diagnostics.front().message);
      continue;
    }
    Compiled c;
    c.file = path.filename().string();
    c.name = patch.name();
    c.typed = std::move(opt.value());
    c.sched = std::move(sr2.schedule);
    out.push_back(std::move(c));
  }
  return out;
}

// Compiles source text (all patches; the first one is returned or the test fails).
inline Compiled compileText(const std::string& text, double sr = 48000.0) {
  ParseResult pr = parseDetailed(text, "test.pqp");
  if (!pr.ok()) {
    std::fprintf(stderr, "%s\n", formatDiagnostic(text, pr.diagnostics.front()).c_str());
    std::abort();
  }
  PatchLibrary lib;
  lib.add(pr.file);
  InferOptions io;
  io.library = &lib;
  auto inf = infer(pr.file.patches[0], io);
  if (!inf.ok()) {
    std::fprintf(stderr, "%s\n", inf.error().message.c_str());
    std::abort();
  }
  OptimiseOptions oo;
  oo.sampleRate = sr;
  auto opt = optimise(std::move(inf.value()), oo);
  if (!opt.ok()) std::abort();
  ScheduleOptions so;
  so.sampleRate = sr;
  auto s = schedule(opt.value(), so);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.error().message.c_str());
    std::abort();
  }
  Compiled c;
  c.name = pr.file.patches[0].name();
  c.typed = std::move(opt.value());
  c.sched = std::move(s.value());
  return c;
}

// ---- the shared stimulus ------------------------------------------------------------------------------------
struct Tables {
  std::vector<pq::dsp::Wavetable> wavetables;
  std::vector<const pq::dsp::WavetableView*> wavetableViews;
  std::vector<pq::dsp::ShaperTable> shapers;
  std::vector<pq::dsp::SampleData> samples;
  std::vector<const pq::dsp::ShaperView*> shaperViews;
  std::vector<const pq::dsp::SampleView*> sampleViews;
  std::vector<refeval::ShaperSpec> shaperSpecs;
  std::vector<std::vector<float>> sampleData;
  eng::Resources resources;

  Tables() {
    // Table 0: a band-limited saw (its mip levels differ, so the level choice matters).
    const pq::AlignedBuffer<float> saw = pq::dsp::Wavetable::basicCycle(pq::dsp::BasicShape::Saw, 2048);
    wavetables.push_back(std::move(pq::dsp::Wavetable::build(saw.span()).value()));
    for (const auto& w : wavetables) wavetableViews.push_back(&w.view());
    refeval::ShaperSpec sp;
    sp.lo = -2.0f;
    sp.hi = 2.0f;
    for (int i = 0; i <= 1024; ++i) sp.points.push_back(static_cast<float>(std::tanh(-2.0 + 4.0 * i / 1024.0)));
    shaperSpecs.push_back(sp);
    shapers.push_back(std::move(pq::dsp::ShaperTable::build(sp.points, sp.lo, sp.hi).value()));
    std::vector<float> ramp, wave;
    for (int i = 0; i < 64; ++i) ramp.push_back(static_cast<float>(i) / 64.0f);
    for (int i = 0; i < 300; ++i) wave.push_back(static_cast<float>(0.8 * std::sin(2 * refeval::kPi * i / 37.0) + 0.1));
    sampleData = {ramp, wave};
    for (const auto& d : sampleData) samples.push_back(std::move(pq::dsp::SampleData::build(d).value()));
    for (const auto& s : shapers) shaperViews.push_back(&s.view());
    for (const auto& s : samples) sampleViews.push_back(&s.view());
    resources.wavetables = wavetableViews;
    resources.shapers = shaperViews;
    resources.samples = sampleViews;
  }
};

inline const Tables& tables() {
  static const Tables t;
  return t;
}

inline float f32(double x) { return static_cast<float>(x); }

inline double voiceSignal(uint32_t sig, uint32_t v, uint64_t t) {
  const auto gate = [&](uint64_t tt) { return (tt + 97u * v) % 700u < 450u; };
  const double hz = f32(110.0 * (1.0 + 0.37 * v) * (1.0 + 0.002 * std::sin(0.001 * static_cast<double>(t))));
  switch (static_cast<eng::VoiceSignal>(sig)) {
    case eng::VoiceSignal::Hz: return hz;
    case eng::VoiceSignal::Cents: return f32(1200.0 * std::log2(hz / 261.626));
    case eng::VoiceSignal::Gate: return gate(t) ? 1.0 : 0.0;
    case eng::VoiceSignal::Trigger: return gate(t) && (t == 0 || !gate(t - 1)) ? 1.0 : 0.0;
    case eng::VoiceSignal::Velocity: return f32(0.5 + 0.04 * v);
    case eng::VoiceSignal::Release: return f32(0.25 + 0.01 * v);
    case eng::VoiceSignal::Ratio: return 1.5;
    case eng::VoiceSignal::Harmonic: return 3.0 + v % 2;
    case eng::VoiceSignal::UnisonCount: return 1.0;
    case eng::VoiceSignal::Index: return static_cast<double>(v % 3);
    default: return 0.0;
  }
}
inline bool voiceSignalIsBool(uint32_t sig) {
  return sig == static_cast<uint32_t>(eng::VoiceSignal::Gate) || sig == static_cast<uint32_t>(eng::VoiceSignal::Trigger);
}

struct Setup {
  const Compiled* c = nullptr;
  uint32_t voices = 0;
  int paramSet = 0;  // 0: defaults; 1: near the range minimum
  // Per root input ordinal: bool kind, param rate.
  std::map<uint32_t, bool> inBool, inParam;

  explicit Setup(const Compiled& comp, uint32_t v, int ps) : c(&comp), voices(v), paramSet(ps) {
    for (const External& e : comp.sched.externals)
      if (e.source == External::Source::RootInput) inParam[e.index] = e.rate != Rate::Audio;
    uint32_t ord = 0;
    const Patch& p = comp.typed.patch;
    for (NodeIndex n = 0; n < p.nodeCount(); ++n)
      if (p.node(n).kind == NodeKind::Input) inBool[ord++] = comp.typed.outputType(n, 0).kind == Kind::Bool;
  }

  double root(uint32_t ord, uint32_t ch, uint32_t v, uint64_t t) const {
    const bool b = inBool.count(ord) && inBool.at(ord);
    const bool param = inParam.count(ord) && inParam.at(ord);
    if (b) {
      if (param) return (ord + v) % 2 == 0 ? 1.0 : 0.0;
      return ((t + 7u * ord + 13u * v + 3u * ch) / (23u + 4u * ord)) % 3u == 0 ? 1.0 : 0.0;
    }
    if (param) return f32(0.3 + 0.1 * ord + 0.05 * ch + 0.01 * v);
    return f32(0.5 + 0.45 * std::sin(0.013 * (ord + 1) * static_cast<double>(t) + 0.7 * ch + 0.3 * v));
  }
  double param(NodeIndex n) const {
    if (paramSet == 0) return NAN;
    const ParamDecl& d = c->typed.patch.paramDecl(n);
    if (d.kind == Kind::Bool) return d.defaultValue.value() != 0 ? 0.0 : 1.0;
    if (d.hasRange) return d.rangeMin.value() + 0.013 * (d.rangeMax.value() - d.rangeMin.value());
    return 0.5 * d.defaultValue.value() + 0.1;
  }
  static double track(uint32_t ch, uint64_t t) { return f32(0.3 * std::sin(0.021 * static_cast<double>(t) + ch)); }
  static double side(uint32_t ch, uint64_t t) { return f32(0.2 * std::sin(0.017 * static_cast<double>(t) + 2.0 * ch)); }
  static eng::TransportSnapshot transport() {
    eng::TransportSnapshot tr;
    tr.bpm = 120.0;
    tr.beat = 1.25;
    tr.beatsPerFrame = 2.0 / 48000.0;
    tr.beatsPerBar = 4.0;
    tr.barStartBeat = 0.0;
    tr.playing = true;
    return tr;
  }
  uint32_t lastVoice() const { return voices > 2 ? 2u : eng::kNoVoice; }

  refeval::Stimulus stimulus() const {
    refeval::Stimulus s;
    s.sampleRate = c->sched.sampleRate;
    s.voices = voices;
    s.root = [this](uint32_t o, uint32_t ch, uint32_t v, uint64_t t) { return root(o, ch, v, t); };
    s.voiceSignal = &voiceSignal;
    s.trackIn = &track;
    s.sidechainIn = &side;
    s.param = [this](NodeIndex n) { return param(n); };
    s.transport = transport();
    s.lastVoice = lastVoice();
    s.wavetables = tables().wavetableViews;
    s.shapers = tables().shaperSpecs;
    s.samples = tables().sampleData;
    const Schedule* sc = &c->sched;
    s.noiseSeed = [sc](NodeIndex n, uint16_t ch) -> uint32_t {
      for (const StateEntry& e : sc->state.entries)
        if (e.node == n && e.channel == ch && e.tag.kernel == static_cast<uint16_t>(pq::dsp::Atom::Noise)) return e.init.seed;
      return 0u;
    };
    return s;
  }
};

// ---- streams --------------------------------------------------------------------------------------------------
struct StreamKey {
  int kind;  // 0 global out slot, 1 voice out slot, 2 send
  uint32_t a, b, v;
  bool operator<(const StreamKey& o) const {
    return std::tie(kind, a, b, v) < std::tie(o.kind, o.a, o.b, o.v);
  }
};
using Streams = std::map<StreamKey, std::vector<double>>;

inline std::vector<std::pair<uint32_t, uint32_t>> sendBuses(const Schedule& s) {
  std::vector<std::pair<uint32_t, uint32_t>> b;
  for (const Op& op : s.ops)
    if (op.kind == OpKind::Send) b.emplace_back(op.resource, op.channel);
  std::sort(b.begin(), b.end());
  b.erase(std::unique(b.begin(), b.end()), b.end());
  return b;
}

inline std::string streamName(const Compiled& c, const StreamKey& k, const eng::ImageInterface& io) {
  char buf[160];
  if (k.kind == 2) {
    std::snprintf(buf, sizeof buf, "send bus %u ch %u", k.a, k.b);
    return buf;
  }
  const eng::IoSlot& sl = (k.kind == 0 ? io.globalOut : io.voiceOut)[k.a];
  std::snprintf(buf, sizeof buf, "%s '%s' ch %u%s", sl.isExport ? "export" : "out",
                c.typed.patch.nodeName(sl.node).c_str(), sl.channel,
                k.kind == 1 ? (" voice " + std::to_string(k.v)).c_str() : "");
  return buf;
}

// Reference streams for `frames` samples.
inline Streams runReference(const Setup& st, uint32_t frames, std::string& why) {
  const Compiled& c = *st.c;
  refeval::Evaluator ref(c.typed, st.stimulus());
  Streams out;
  if (!ref.ok()) {
    why = ref.why();
    return out;
  }
  // Slots exactly as the image numbers them (schedule output order, split by poly).
  std::vector<const OutputRef*> gOut, vOut;
  for (const OutputRef& o : c.sched.outputs) (o.poly == Poly::Voice ? vOut : gOut).push_back(&o);
  const auto buses = sendBuses(c.sched);
  for (uint64_t t = 0; t < frames; ++t) {
    ref.step(t);
    for (uint32_t s = 0; s < gOut.size(); ++s) out[{0, s, 0, 0}].push_back(ref.sink(gOut[s]->node, gOut[s]->channel, 0));
    for (uint32_t s = 0; s < vOut.size(); ++s)
      for (uint32_t v = 0; v < st.voices; ++v) out[{1, s, 0, v}].push_back(ref.sink(vOut[s]->node, vOut[s]->channel, v));
    for (const auto& [bus, ch] : buses) {
      const auto it = ref.sends().find({bus, ch});
      out[{2, bus, ch, 0}].push_back(it == ref.sends().end() ? 0.0 : it->second);
    }
    if (!ref.ok()) {
      why = ref.why();
      return {};
    }
  }
  return out;
}

struct InterpRun {
  Streams streams;
  uint64_t rtViolations = 0;
  std::string error;
};

// Builds the image a run drives: the interpreter by default; T1b's JIT tests pass their compiler here.
using ImageBuilder = std::function<pq::Result<std::unique_ptr<eng::ProgramImage>>(const Schedule&, const eng::InterpretOptions&)>;

// Interpreter (or `build`'s) streams for `frames` samples in blocks of `block`.
inline InterpRun runInterp(const Setup& st, uint32_t frames, uint32_t block, uint32_t maxBlock = 1024,
                           const ImageBuilder& build = {}) {
  const Compiled& c = *st.c;
  InterpRun res;
  eng::InterpretOptions io;
  io.maxBlockFrames = maxBlock;
  io.maxVoiceGroups = std::max<uint32_t>(1, (st.voices + 7) / 8);
  io.resources = &tables().resources;
  auto built = build ? build(c.sched, io) : eng::buildInterpreted(c.sched, io);
  if (!built.ok()) {
    res.error = built.error().message;
    return res;
  }
  eng::ProgramImage& img = **built;
  const eng::ImageInterface& face = img.io();
  const uint32_t groups = st.voices ? (st.voices + 7) / 8 : 0;
  const uint32_t B = maxBlock;
  // Buffers.
  std::vector<std::vector<float>> gIn(face.globalIn.size(), std::vector<float>(B)), gOut(face.globalOut.size(), std::vector<float>(B));
  std::vector<const float*> gInP;
  std::vector<float*> gOutP;
  for (auto& b : gIn) gInP.push_back(b.data());
  for (auto& b : gOut) gOutP.push_back(b.data());
  std::vector<std::vector<std::vector<float>>> vSig(groups), vIn(groups), vOut(groups);
  std::vector<std::vector<const float*>> vSigP(groups), vInP(groups);
  std::vector<std::vector<float*>> vOutP(groups);
  std::vector<eng::VoiceGroupIO> gio(groups);
  for (uint32_t g = 0; g < groups; ++g) {
    vSig[g].assign(eng::kVoiceSignalCount, std::vector<float>(static_cast<size_t>(B) * 8));
    vIn[g].assign(face.voiceIn.size(), std::vector<float>(static_cast<size_t>(B) * 8));
    vOut[g].assign(face.voiceOut.size(), std::vector<float>(static_cast<size_t>(B) * 8));
    for (auto& b : vSig[g]) vSigP[g].push_back(b.data());
    for (auto& b : vIn[g]) vInP[g].push_back(b.data());
    for (auto& b : vOut[g]) vOutP[g].push_back(b.data());
    gio[g].laneMask = st.voices >= (g + 1) * 8 ? 0xFFu : (1u << (st.voices - g * 8)) - 1u;
    gio[g].signals = vSigP[g].data();
    gio[g].in = vInP[g].data();
    gio[g].numIn = static_cast<uint32_t>(vInP[g].size());
    gio[g].out = vOutP[g].data();
    gio[g].numOut = static_cast<uint32_t>(vOutP[g].size());
  }
  eng::VoiceTable vt;
  vt.groups = gio.data();
  vt.numGroups = groups;
  if (st.lastVoice() != eng::kNoVoice) {
    vt.lastGroup = st.lastVoice() / 8;
    vt.lastLane = st.lastVoice() % 8;
  }
  const uint32_t ioCh = 2;
  std::vector<std::vector<float>> track(ioCh, std::vector<float>(B)), side(ioCh, std::vector<float>(B));
  std::vector<const float*> trackP, sideP;
  for (auto& b : track) trackP.push_back(b.data());
  for (auto& b : side) sideP.push_back(b.data());
  const auto buses = sendBuses(c.sched);
  uint32_t numBus = 0;
  for (const auto& [bus, ch] : buses) numBus = std::max(numBus, bus + 1);
  std::vector<std::vector<std::vector<float>>> busMem(numBus, std::vector<std::vector<float>>(4, std::vector<float>(B)));
  std::vector<std::vector<float*>> busP(numBus);
  std::vector<eng::SendBus> sendBus(numBus);
  for (uint32_t b = 0; b < numBus; ++b) {
    for (auto& ch : busMem[b]) busP[b].push_back(ch.data());
    sendBus[b].channels = busP[b].data();
    sendBus[b].numChannels = 4;
  }
  std::vector<float> params(face.params.size());
  for (size_t i = 0; i < params.size(); ++i) params[i] = static_cast<float>(st.param(face.params[i].node));
  const eng::TransportSnapshot tr0 = Setup::transport();

  for (uint32_t t0 = 0; t0 < frames; t0 += block) {
    const uint32_t n = std::min(block, frames - t0);
    for (uint32_t s = 0; s < face.globalIn.size(); ++s) {
      const eng::IoSlot& sl = face.globalIn[s];
      const bool b = st.inBool.count(sl.ordinal) && st.inBool.at(sl.ordinal);
      for (uint32_t f = 0; f < n; ++f) {
        const double x = st.root(sl.ordinal, sl.channel, 0, t0 + f);
        gIn[s][f] = b ? pq::dsp::boolMask(x != 0) : static_cast<float>(x);
      }
    }
    for (uint32_t ch = 0; ch < ioCh; ++ch)
      for (uint32_t f = 0; f < n; ++f) {
        track[ch][f] = static_cast<float>(Setup::track(ch, t0 + f));
        side[ch][f] = static_cast<float>(Setup::side(ch, t0 + f));
      }
    for (uint32_t g = 0; g < groups; ++g) {
      for (uint32_t sig = 0; sig < eng::kVoiceSignalCount; ++sig)
        for (uint32_t f = 0; f < n; ++f)
          for (uint32_t l = 0; l < 8; ++l) {
            const uint32_t v = g * 8 + l;
            // Lanes without a voice get loud, valid values (gate held, high pitch): a reducer or send that does
            // not mask them out shows up as a mismatch.
            float x = voiceSignalIsBool(sig) ? pq::dsp::boolMask(true) : 900.0f + 37.0f * static_cast<float>(l);
            if (v < st.voices) {
              const double d = voiceSignal(sig, v, t0 + f);
              x = voiceSignalIsBool(sig) ? pq::dsp::boolMask(d != 0) : static_cast<float>(d);
            }
            vSig[g][sig][static_cast<size_t>(f) * 8 + l] = x;
          }
      for (uint32_t s = 0; s < face.voiceIn.size(); ++s) {
        const eng::IoSlot& sl = face.voiceIn[s];
        const bool b = st.inBool.count(sl.ordinal) && st.inBool.at(sl.ordinal);
        for (uint32_t f = 0; f < n; ++f)
          for (uint32_t l = 0; l < 8; ++l) {
            const uint32_t v = g * 8 + l;
            float x = b ? pq::dsp::boolMask(true) : 7.5f;  // no voice: see above
            if (v < st.voices) {
              const double d = st.root(sl.ordinal, sl.channel, v, t0 + f);
              x = b ? pq::dsp::boolMask(d != 0) : static_cast<float>(d);
            }
            vIn[g][s][static_cast<size_t>(f) * 8 + l] = x;
          }
      }
    }
    for (auto& bus : busMem)
      for (auto& ch : bus) std::fill(ch.begin(), ch.end(), 0.0f);
    eng::ProcessContext ctx;
    ctx.frames = n;
    ctx.sampleRate = c.sched.sampleRate;
    ctx.sampleTime = t0;
    ctx.transport = tr0;
    ctx.transport.beat = tr0.beat + static_cast<double>(t0) * tr0.beatsPerFrame;
    ctx.in = gInP.data();
    ctx.numIn = static_cast<uint32_t>(gInP.size());
    ctx.out = gOutP.data();
    ctx.numOut = static_cast<uint32_t>(gOutP.size());
    ctx.trackIn = trackP.data();
    ctx.numTrackIn = ioCh;
    ctx.sidechainIn = sideP.data();
    ctx.numSidechainIn = ioCh;
    ctx.params = params.data();
    ctx.numParams = static_cast<uint32_t>(params.size());
    ctx.voices = groups ? &vt : nullptr;
    ctx.sends = sendBus.data();
    ctx.numSends = numBus;
    res.rtViolations += pq::alloc_trap::countRtViolations([&] {
      pq::DenormalGuard dg;
      img.process(ctx);
    });
    // Record.
    for (uint32_t s = 0; s < face.globalOut.size(); ++s) {
      const bool isBool = c.typed.inputType(face.globalOut[s].node, 0).kind == Kind::Bool;
      auto& dst = res.streams[{0, s, 0, 0}];
      for (uint32_t f = 0; f < n; ++f)
        dst.push_back(isBool ? (pq::dsp::maskIsTrue(gOut[s][f]) ? 1.0 : 0.0) : gOut[s][f]);
    }
    for (uint32_t s = 0; s < face.voiceOut.size(); ++s) {
      const bool isBool = c.typed.inputType(face.voiceOut[s].node, 0).kind == Kind::Bool;
      for (uint32_t v = 0; v < st.voices; ++v) {
        auto& dst = res.streams[{1, s, 0, v}];
        const uint32_t g = v / 8, l = v % 8;
        for (uint32_t f = 0; f < n; ++f) {
          const float x = vOut[g][s][static_cast<size_t>(f) * 8 + l];
          dst.push_back(isBool ? (pq::dsp::maskIsTrue(x) ? 1.0 : 0.0) : x);
        }
      }
    }
    for (const auto& [bus, ch] : buses) {
      auto& dst = res.streams[{2, bus, ch, 0}];
      for (uint32_t f = 0; f < n; ++f) dst.push_back(busMem[bus][ch][f]);
    }
  }
  return res;
}

struct Mismatch {
  double maxErr = 0.0;
  std::string where;  // the worst sample
};

inline double sampleErr(double ref, double got) {
  if (std::isnan(ref) || std::isnan(got)) return std::isnan(ref) && std::isnan(got) ? 0.0 : INFINITY;
  if (std::isinf(ref) || std::isinf(got)) return ref == got ? 0.0 : INFINITY;
  return std::fabs(ref - got) / std::max(1.0, std::fabs(ref));
}

inline Mismatch compare(const Compiled& c, const Streams& ref, const Streams& got, const eng::ImageInterface& io) {
  Mismatch m;
  for (const auto& [key, r] : ref) {
    const auto it = got.find(key);
    if (it == got.end() || it->second.size() != r.size()) {
      m.maxErr = INFINITY;
      m.where = "missing stream " + streamName(c, key, io);
      return m;
    }
    for (size_t t = 0; t < r.size(); ++t) {
      const double e = sampleErr(r[t], it->second[t]);
      if (e > m.maxErr) {
        m.maxErr = e;
        char buf[200];
        std::snprintf(buf, sizeof buf, " at t=%zu: ref %.9g interp %.9g", t, r[t], it->second[t]);
        m.where = streamName(c, key, io) + buf;
      }
    }
  }
  return m;
}

}  // namespace interp_test
