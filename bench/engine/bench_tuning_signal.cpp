// T2.6: per-voice pitch cost of tuning as a signal. Record the numbers from the `ship` preset.
//   kernel/*   the tuning_hz kernel alone, 64-frame blocks, called through its KernelFn like the interpreter does
//              (ns per frame = 8 voices for L8, ns per sample for L1):
//                unmodulated       every tuning input a param at the table's definition: a copy (T2.6-fix3)
//                static            every tuning input a param (Spl), modulated: one pitch per lane per block, per sample
//                                  the hz / hz0 factor (T2.6-fix3) and blends
//                gen sweep         audio-rate generator moving inside one interval (no region crossing)
//                gen crossing      audio-rate generator crossing an interval boundary every ~32 samples
//                all audio         generator, period, stretch and reference all audio rate
//   voices/*   a 7-note MOS track patch (tuning node + `y = sine(phasor(voice.hz))` instrument) through the
//              VoiceEngine on the interpreter, 8 held voices, per block: with a static and an audio-rate (swept)
//              generator, against the same instrument reading the engine's static voice.hz (no tuning node).
// pq_bench_tuning_signal [--filter REGEX] [--csv FILE] (pq_bench.h runner).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/tuning_table.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/note_events.h"
#include "pq/engine/tuning_signal.h"
#include "pq/engine/voices.h"
#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"
#include "pq/tuning/mos_tuning.h"
#include "pq_bench.h"

namespace dsp = pq::dsp;
namespace e = pq::engine;
namespace g = pq::graph;

namespace {

constexpr uint32_t kFrames = 64;

pq::tuning::Tuning mosTuning() {
  pq::tuning::MosParams p;
  p.n = 7;
  p.rotation = 1;
  p.generator = 696.578428;
  auto m = pq::tuning::Mos::make(p);
  if (!m) std::exit(1);
  auto t = pq::tuning::Tuning::make(m.value(), 261.626);
  if (!t) std::exit(1);
  return t.value();
}

struct KernelFixture {
  dsp::TuningTable table;
  pq::AlignedBuffer<float> buf{14 * kFrames * 8};
  float* in[10]{};
  float* out[2]{};
  const float* inPtr[10]{};
  float* outPtr[2]{};
  KernelFixture() {
    auto t = e::makeTuningTable(mosTuning());
    if (!t) std::exit(2);
    table = std::move(t.value());
    for (uint32_t i = 0; i < 10; ++i) in[i] = buf.data() + i * kFrames * 8 + i * 16;
    for (uint32_t o = 0; o < 2; ++o) out[o] = buf.data() + (10 + o) * kFrames * 8 + (10 + o) * 16;
    for (uint32_t i = 0; i < 10; ++i) inPtr[i] = in[i];
    for (uint32_t o = 0; o < 2; ++o) outPtr[o] = out[o];
  }
};

enum class Mode { Unmodulated, Static, Sweep, Crossing, AllAudio };

void benchKernel(pq::bench::Context& ctx, KernelFixture& fx, Mode mode, uint32_t lanes) {
  // Inputs: degree, equave, hz, cents, generator, period, stretch, reference, select, node (the last two Spl 0).
  const uint32_t mask = 0b1100000000u | (mode == Mode::Static || mode == Mode::Unmodulated ? 0b11110011u
                                         : mode == Mode::AllAudio                          ? 0b00000011u
                                                                                           : 0b11100011u);
  for (uint32_t t = 0; t < kFrames * 8; ++t) fx.in[8][t] = fx.in[9][t] = 0.0f;
  for (uint32_t i = 0; i < 8; ++i)
    for (uint32_t t = 0; t < kFrames; ++t)
      for (uint32_t l = 0; l < 8; ++l) {
        const bool spl = (mask >> i) & 1u;
        const uint32_t tt = spl ? 0 : t;
        float v = 0.0f;
        switch (i) {
          case 0: v = static_cast<float>((l * 3) % 7); break;
          case 1: v = static_cast<float>(static_cast<int>(l % 3) - 1); break;
          case 2: v = 300.0f; break;
          case 3: v = 0.0f; break;
          case 4:
            v = mode == Mode::Unmodulated ? fx.table.view().generatorF
                : mode == Mode::Crossing  ? 590.0f + 20.0f * static_cast<float>((tt / 32) % 2) + 0.01f * tt
                                          : 696.0f + 0.01f * static_cast<float>(tt);
            break;
          case 5: v = 1200.0f + (mode == Mode::AllAudio ? 0.001f * tt : 0.0f); break;
          case 6: v = mode == Mode::AllAudio ? 0.001f * tt : 0.0f; break;
          case 7: v = 261.626f + (mode == Mode::AllAudio ? 0.001f * tt : 0.0f); break;
        }
        fx.in[i][(spl ? 0 : t * lanes) + (lanes == 8 || spl ? l : 0)] = v;
      }
  const dsp::KernelFn fn = dsp::findKernel(dsp::Atom::TuningHz, lanes, mask);
  dsp::KernelArgs args{fx.inPtr, fx.outPtr, nullptr, &fx.table.view(), kFrames, 10, 2, mask};
  ctx.measure([&] {
    fn(args);
    pq::bench::doNotOptimize(fx.out[0][0]);
  }, kFrames, lanes == 1 ? "ns/sample" : "ns/frame(8 voices)", lanes == 1 ? "L1" : "L8");
}

// ---- through the VoiceEngine ----------------------------------------------------------------------------------

const char* kTuned = R"(
patch Track {
  in gen : audio global
  param ref = 261.626 [20..2000]
  out y : audio voice
  t = tuning_hz(voice.degree, voice.equave, voice.hz, voice.cents, gen, 1200, 0, ref, table: 0)
  tv = tune_voices(t.hz, t.cents)
  s = Synth()
  y = s.y
}
patch Synth {
  out y : audio voice
  y = sine(phasor(voice.hz)) * gate_to_real(voice.gate) * 0.2
}
)";
const char* kTunedStatic = R"(
patch Track {
  param gen = 696.578428 [0..1200]
  param ref = 261.626 [20..2000]
  out y : audio voice
  t = tuning_hz(voice.degree, voice.equave, voice.hz, voice.cents, gen, 1200, 0, ref, table: 0)
  tv = tune_voices(t.hz, t.cents)
  s = Synth()
  y = s.y
}
patch Synth {
  out y : audio voice
  y = sine(phasor(voice.hz)) * gate_to_real(voice.gate) * 0.2
}
)";
const char* kPlain = R"(
patch Track {
  in gen : audio global
  out y : audio voice
  s = Synth()
  y = s.y
}
patch Synth {
  out y : audio voice
  y = sine(phasor(voice.hz)) * gate_to_real(voice.gate) * 0.2
}
)";

std::unique_ptr<e::ProgramImage> compile(const char* text, const e::Resources& res) {
  auto parsed = g::parse(text, "bench.pqp");
  if (!parsed) std::exit(3);
  g::PatchLibrary lib;
  lib.add(*parsed);
  g::InferOptions io;
  io.library = &lib;
  auto typed = g::infer(parsed->patches[0], io);
  if (!typed) {
    std::fprintf(stderr, "%s\n", typed.error().message.c_str());
    std::exit(4);
  }
  g::OptimiseOptions oo;
  oo.sampleRate = 48000.0;
  auto opt = g::optimise(std::move(typed.value()), oo);
  if (!opt) std::exit(5);
  g::ScheduleOptions so;
  so.sampleRate = 48000.0;
  auto s = g::schedule(opt.value(), so);
  if (!s) std::exit(6);
  e::InterpretOptions eo;
  eo.maxBlockFrames = kFrames;
  eo.maxVoiceGroups = 2;
  eo.resources = &res;
  auto img = e::buildInterpreted(s.value(), eo);
  if (!img) std::exit(7);
  return std::move(img.value());
}

void benchVoices(pq::bench::Context& ctx, const char* text, bool sweep) {
  auto tab = e::makeTuningTable(mosTuning());
  if (!tab) std::exit(8);
  const dsp::TuningTableView* views[1] = {&tab.value().view()};
  e::Resources res;
  res.tunings = views;
  auto img = compile(text, res);
  e::VoiceEngine ve;
  e::VoiceConfig cfg;
  cfg.maxVoices = 8;
  if (!ve.prepare(img.get(), cfg).ok()) std::exit(9);
  e::EventBuffer buf(64);
  std::vector<float> gen(kFrames);
  const float* in[1] = {gen.data()};
  e::ProcessContext pc;
  pc.frames = kFrames;
  pc.sampleRate = 48000.0;
  pc.in = in;
  pc.numIn = img->io().globalIn.empty() ? 0u : 1u;
  for (uint32_t v = 0; v < 8; ++v) {
    e::NoteEvent on = e::noteOn(0, v + 1, 200.0f + 30.0f * v, 0.8f);
    on.degree = static_cast<float>(v % 7);
    on.equave = static_cast<float>(v / 7);
    buf.push(on);
  }
  uint64_t block = 0;
  auto run = [&] {
    for (uint32_t t = 0; t < kFrames; ++t)  // a slow LFO on the generator inside the 5L 2s region
      gen[t] = sweep ? 696.0f + 8.0f * std::sin(1e-4f * static_cast<float>(block * kFrames + t)) : 696.578428f;
    pc.events = &buf.list();
    ve.process(pc);
    buf.clear();
    ++block;
  };
  run();
  ctx.measure(run, 8, "ns/voice-block(64 frames)");
}

}  // namespace

int main(int argc, char** argv) {
  pq::DenormalGuard dg;
  KernelFixture fx;
  pq::bench::Runner runner("pq_bench_tuning_signal");
  const struct {
    const char* name;
    Mode mode;
  } kModes[] = {{"kernel/unmodulated", Mode::Unmodulated},
                {"kernel/static", Mode::Static},
                {"kernel/gen sweep", Mode::Sweep},
                {"kernel/gen crossing every 32 samples", Mode::Crossing},
                {"kernel/all audio", Mode::AllAudio}};
  for (const auto& m : kModes)
    runner.add(m.name, [&fx, m](pq::bench::Context& c) {
      benchKernel(c, fx, m.mode, 8);
      benchKernel(c, fx, m.mode, 1);
    });
  runner.add("voices/static voice.hz (no tuning node)", [](pq::bench::Context& c) { benchVoices(c, kPlain, false); });
  runner.add("voices/tuning node, param generator", [](pq::bench::Context& c) { benchVoices(c, kTunedStatic, false); });
  runner.add("voices/tuning node, audio-rate generator", [](pq::bench::Context& c) { benchVoices(c, kTuned, true); });
  return runner.main(argc, argv);
}
