// T1.8: voice engine costs. Release build only.
//   overhead/*     VoiceEngine::process around an image that does nothing (NullImage, 1 voice out slot): the
//                  engine's own per-block cost (signal buffers, mixing, level metering, voice end checks)
//   event/*        extra cost per note event (allocation, op bucketing, per-lane signal rendering), null image
//   fm_voice/*     FmVoice end to end through the VoiceEngine vs the bare image with a hand-filled voice table
//   split/*        the rejected alternative: running the image in sub-blocks split at event frames
//   packing/*      groups run vs ceil(live / 8) under random traffic, best fit alone vs with compaction, and the
//                  time per block (null image: engine only; FmVoice: engine + image)
//   ring/*         (`pq_bench_voices ring`, T1.8-fix1) 64 voices with a per-voice 2 s delay line under note traffic:
//                  per-block audio-thread cost (thread cycles) against the block period
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/denormal_guard.h"
#include "pq/dsp/kernel.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/voices.h"
#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"
#include "pq_bench.h"

#include <windows.h>

using namespace pq;
namespace g = pq::graph;
namespace e = pq::engine;

namespace {

constexpr uint32_t kBlock = 64;

const char* kFmVoice = R"(
patch FmVoice {
  param ratio = 2.0 [0.25..16]
  param index = 1.0 [0..20]
  param attack  = 5ms   [0ms..10s]
  param release = 300ms [1ms..30s]
  out   out : audio voice mono
  mod = sine(phasor(voice.hz * ratio))
  car = sine(phasor(voice.hz + mod * index * voice.hz))
  env = adsr(voice.gate, a: attack, d: 200ms, s: 0.7, r: release)
  out = car * env
}
patch adsr {
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
)";

// FmVoice's oscillators without the envelope: every op block-wise (what a JIT'd voice looks like to the engine).
const char* kFmOsc = R"(patch FmOsc {
  param ratio = 2.0 [0.25..16]
  param index = 1.0 [0..20]
  out   out : audio voice mono
  mod = sine(phasor(voice.hz * ratio))
  car = sine(phasor(voice.hz + mod * index * voice.hz))
  out = car * gate_to_real(voice.gate)
})";

std::unique_ptr<e::ProgramImage> compile(const std::string& text, uint32_t groups, uint32_t block) {
  auto parsed = g::parse(text, "bench.pqp");
  if (!parsed) std::exit(1);
  g::PatchLibrary lib;
  lib.add(*parsed);
  g::InferOptions io;
  io.library = &lib;
  auto typed = g::infer(parsed->patches[0], io);
  if (!typed) std::exit(2);
  g::OptimiseOptions oo;
  oo.sampleRate = 48000.0;
  auto opt = g::optimise(std::move(typed.value()), oo);
  if (!opt) std::exit(3);
  g::ScheduleOptions so;
  so.sampleRate = 48000.0;
  auto s = g::schedule(opt.value(), so);
  if (!s) std::exit(4);
  e::InterpretOptions eo;
  eo.maxBlockFrames = block;
  eo.maxVoiceGroups = groups;
  auto img = e::buildInterpreted(s.value(), eo);
  if (!img) std::exit(5);
  return std::move(img.value());
}

// An image that does nothing: isolates the VoiceEngine's own cost.
class NullImage final : public e::ProgramImage {
public:
  NullImage(uint32_t groups, uint32_t block) {
    io_.sampleRate = 48000.0;
    io_.maxBlockFrames = block;
    io_.maxVoiceGroups = groups;
    io_.hasVoiceStages = true;
    io_.voiceOut.push_back(e::IoSlot{});
  }
  void process(e::ProcessContext&) noexcept override {}
  const g::StateLayout& stateLayout() const noexcept override { return layout_; }
  const e::ImageInterface& io() const noexcept override { return io_; }
  e::StateArena stateArena() noexcept override { return {}; }
  uint64_t rejectedBlocks() const noexcept override { return 0; }

private:
  g::StateLayout layout_;
  e::ImageInterface io_;
};

struct Driver {
  e::VoiceEngine ve;
  e::EventBuffer buf{4096};
  e::ProcessContext ctx;
  explicit Driver(e::ProgramImage* img, e::VoiceConfig cfg) {
    if (!ve.prepare(img, cfg).ok()) std::exit(6);
    ctx.frames = kBlock;
    ctx.sampleRate = 48000.0;
  }
  void block() noexcept {
    ctx.events = &buf.list();
    ve.process(ctx);
    buf.clear();
  }
};

// Bounded, mostly distinct pitches (no same-note retriggers by accident).
float pitch(uint32_t i) { return 110.0f * std::exp2(static_cast<float>(i % 48) / 12.0f + 0.0001f * static_cast<float>(i % 997)); }

void overhead(const char* name, uint32_t voices, uint32_t groups) {
  NullImage img(groups, kBlock);
  e::VoiceConfig cfg;
  cfg.maxVoices = std::min(64u, groups * 8);
  Driver d(&img, cfg);
  for (uint32_t i = 0; i < voices; ++i) d.buf.push(e::noteOn(i % kBlock, i + 1, pitch(i), 0.8f));
  d.block();
  d.block();
  DenormalGuard dg;
  const bench::Stats s = bench::measure([&] { d.block(); }, 1);
  bench::report(name, s, "ns/block");
}

// Steady traffic, 32 live voices: per block `perBlock` note-ons (random frames) and as many note-offs of notes
// started 32 / perBlock blocks earlier; released voices end at the end of their block (silence is immediate: outputs are 0, silenceMs tiny).
void eventCost(const char* name, e::ProgramImage* img, uint32_t perBlock, bool withEvents) {
  e::VoiceConfig cfg;
  cfg.maxVoices = 64;
  cfg.silenceMs = 0.01f;
  cfg.retriggerSameNote = false;
  Driver d(img, cfg);
  uint32_t next = 1, rng = 7;
  auto rnd = [&] {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
  };
  auto fill = [&] {
    for (uint32_t k = 0; k < perBlock; ++k) {
      const uint32_t id = next++;
      d.buf.push(e::noteOn(rnd() % kBlock, id, pitch(id), 0.8f));
      if (id > 32) d.buf.push(e::noteOff(rnd() % kBlock, id - 32));
    }
  };
  for (int i = 0; i < 64; ++i) {
    fill();
    d.block();
  }
  DenormalGuard dg;
  const bench::Stats s = bench::measure(
      [&] {
        if (withEvents) fill();
        d.block();
      },
      1);
  bench::report(name, s, "ns/block");
  if (!withEvents) return;
  const bench::Stats p = bench::measure(  // the producer's share: generating and pushing the events
      [&] {
        fill();
        d.buf.clear();
      },
      1);
  bench::report("  of which: producing + pushing the events", p, "ns/block");
}

void fmEndToEnd(uint32_t voices) {
  const uint32_t groups = e::voiceGroupsFor(voices);
  auto img = compile(kFmVoice, groups, kBlock);
  e::VoiceConfig cfg;
  cfg.maxVoices = voices;
  Driver d(img.get(), cfg);
  for (uint32_t i = 0; i < voices; ++i) d.buf.push(e::noteOn(0, i + 1, pitch(i), 0.8f));
  d.block();
  DenormalGuard dg;
  const bench::Stats s = bench::measure([&] { d.block(); }, static_cast<uint64_t>(kBlock) * voices);
  char label[96];
  std::snprintf(label, sizeof label, "fm_voice/engine x%u (held)", voices);
  bench::report(label, s, "ns/voice-sample");
}

// The bare image with a hand-filled table (as bench_interpreter), optionally split into `split` sub-blocks.
void fmBare(const char* name, const char* text, uint32_t voices, uint32_t split) {
  const uint32_t groups = voices / 8;
  auto img = compile(text, groups, kBlock);
  std::vector<std::vector<std::vector<float>>> sig(groups, std::vector<std::vector<float>>(e::kVoiceSignalCount, std::vector<float>(kBlock * 8)));
  std::vector<std::vector<float>> vout(groups, std::vector<float>(kBlock * 8));
  std::vector<std::vector<const float*>> sigP(groups);
  std::vector<float*> voutP(groups);
  std::vector<e::VoiceGroupIO> gio(groups);
  for (uint32_t gi = 0; gi < groups; ++gi) {
    for (uint32_t f = 0; f < kBlock; ++f)
      for (uint32_t l = 0; l < 8; ++l) {
        sig[gi][static_cast<size_t>(e::VoiceSignal::Hz)][f * 8 + l] = pitch(gi * 8 + l);
        sig[gi][static_cast<size_t>(e::VoiceSignal::Gate)][f * 8 + l] = dsp::boolMask(true);
      }
    for (auto& b : sig[gi]) sigP[gi].push_back(b.data());
    voutP[gi] = vout[gi].data();
    gio[gi].laneMask = 0xFF;
    gio[gi].signals = sigP[gi].data();
    gio[gi].out = &voutP[gi];
    gio[gi].numOut = 1;
  }
  e::VoiceTable vt;
  vt.groups = gio.data();
  vt.numGroups = groups;
  e::ProcessContext ctx;
  ctx.voices = &vt;
  const uint32_t sub = kBlock / split;
  ctx.frames = sub;
  DenormalGuard dg;
  const bench::Stats s = bench::measure(
      [&] {
        for (uint32_t k = 0; k < split; ++k) img->process(ctx);  // same buffers: the cost is what matters
      },
      static_cast<uint64_t>(kBlock) * voices);
  char label[96];
  if (split == 1)
    std::snprintf(label, sizeof label, "%s/bare image x%u", name, voices);
  else
    std::snprintf(label, sizeof label, "split/%s x%u in %u sub-blocks", name, voices, split);
  bench::report(label, s, "ns/voice-sample");
}

// Random traffic (bursts of 1–4 notes, `burstsPerSecond`, exponential lengths with mean 0.5 s): groups run vs the
// packing lower bound ceil(live / 8), with and without compaction; the time per block (engine + image).
void packing(const char* name, e::ProgramImage* img, double burstsPerSecond, bool compact, int blocks) {
  e::VoiceConfig cfg;
  cfg.maxVoices = 64;
  cfg.silenceMs = 1.0f;
  cfg.retriggerSameNote = false;
  cfg.compact = compact;
  Driver d(img, cfg);
  DenormalGuard dg;
  double ns = 0;
  uint32_t rng = 99;
  auto rnd01 = [&] {
    rng = rng * 1664525u + 1013904223u;
    return static_cast<double>(rng >> 8) / 16777216.0;
  };
  std::vector<std::pair<int64_t, uint32_t>> offs;  // (time, id)
  int64_t t = 0;
  uint32_t id = 1;
  double run = 0, ideal = 0, live = 0;
  for (int b = 0; b < blocks; ++b, t += kBlock) {
    if (rnd01() < burstsPerSecond * kBlock / 48000.0) {
      const uint32_t n = 1 + static_cast<uint32_t>(rnd01() * 4.0);
      for (uint32_t k = 0; k < n; ++k) {
        d.buf.push(e::noteOn(0, id, pitch(id), 0.8f));
        offs.push_back({t + static_cast<int64_t>(-std::log(1.0 - rnd01()) * 0.5 * 48000.0), id});
        ++id;
      }
    }
    for (size_t i = 0; i < offs.size();) {
      if (offs[i].first < t + kBlock) {
        d.buf.push(e::noteOff(static_cast<uint32_t>(std::max<int64_t>(0, offs[i].first - t)), offs[i].second));
        offs[i] = offs.back();
        offs.pop_back();
      } else {
        ++i;
      }
    }
    const auto t0 = std::chrono::steady_clock::now();
    d.block();
    if (b >= 1000) ns += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
    if (b < 1000) continue;
    run += d.ve.groupsRunLastBlock();
    ideal += (d.ve.liveVoices() + 7) / 8;
    live += d.ve.liveVoices();
  }
  const double n = blocks - 1000;
  std::printf("BENCH packing/%s %3.0f bursts/s %-10s groups run %.3f, ceil(live/8) %.3f (live %.1f): %+5.1f %%; "
              "%.0f ns/block; moves %llu, steals %llu\n",
              name, burstsPerSecond, compact ? "compact" : "best fit", run / n, ideal / n, live / n,
              100.0 * (run / ideal - 1.0), ns / n, static_cast<unsigned long long>(d.ve.stats().voiceMoves),
              static_cast<unsigned long long>(d.ve.stats().steals));
}

// T1.8-fix1 (M1 review M9): per-voice delay lines with 2 s of ring (131072 frames, 4 MiB per voice group) under note
// traffic with steals and compaction. Note-on lane resets and compaction moves touch a lane's ring; the audio thread
// must stay well inside the block period (acceptance: < 25 %). Per-block wall time of VoiceEngine::process
// (engine + interpreted image), distribution over `blocks` blocks after a warm-up.
const char* kRingVoice = R"(patch RingVoice {
  out y : audio voice mono
  x = sine(phasor(voice.hz)) * gate_to_real(voice.gate) * 0.1
  e = delay(x + e * 0.5, 300ms, 2s)
  y = x + e
})";

// VoiceStats::skippedMoves exists from T1.8-fix1 on (this bench also builds against the T1.8 engine for the
// before/after rows).
template <class St>
uint64_t skippedMoves(const St& st) {
  if constexpr (requires { st.skippedMoves; }) return st.skippedMoves;
  else return 0;
}

void ringTraffic(double burstsPerSecond, int blocks) {
  auto img = compile(kRingVoice, e::voiceGroupsFor(64), kBlock);
  e::VoiceConfig cfg;
  cfg.maxVoices = 64;
  cfg.silenceDb = -60.0f;
  cfg.retriggerSameNote = false;
  Driver d(img.get(), cfg);
  DenormalGuard dg;
  uint32_t rng = 4242;
  auto rnd01 = [&] {
    rng = rng * 1664525u + 1013904223u;
    return static_cast<double>(rng >> 8) / 16777216.0;
  };
  std::vector<std::pair<int64_t, uint32_t>> offs;
  struct BlockCost {
    double us;
    uint32_t moves, ons;
  };
  std::vector<BlockCost> blocksOut;
  blocksOut.reserve(static_cast<size_t>(blocks));
  const double tscPerUs = bench::tscTicksPerNs() * 1000.0;
  int64_t t = 0;
  uint32_t id = 1;
  uint64_t ons = 0;
  const int warm = 2000;
  for (int b = 0; b < warm + blocks; ++b, t += kBlock) {
    if (rnd01() < burstsPerSecond * kBlock / 48000.0) {
      const uint32_t n = 1 + static_cast<uint32_t>(rnd01() * 4.0);
      for (uint32_t k = 0; k < n; ++k) {
        d.buf.push(e::noteOn(static_cast<uint32_t>(rnd01() * kBlock), id, pitch(id), 0.8f));
        offs.push_back({t + static_cast<int64_t>(-std::log(1.0 - rnd01()) * 0.5 * 48000.0), id});
        ++id;
        if (b >= warm) ++ons;
      }
    }
    for (size_t i = 0; i < offs.size();) {
      if (offs[i].first < t + kBlock) {
        d.buf.push(e::noteOff(static_cast<uint32_t>(std::max<int64_t>(0, offs[i].first - t)), offs[i].second));
        offs[i] = offs.back();
        offs.pop_back();
      } else {
        ++i;
      }
    }
    // Thread CPU cycles (QueryThreadCycleTime): the audio thread's own work, not time lost to preemption on a
    // shared machine; converted with the TSC rate.
    const uint64_t moves0 = d.ve.stats().voiceMoves, ons0 = d.ve.stats().noteOns;
    ULONG64 c0 = 0, c1 = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c0);
    d.block();
    QueryThreadCycleTime(GetCurrentThread(), &c1);
    if (b >= warm)
      blocksOut.push_back({static_cast<double>(c1 - c0) / tscPerUs, static_cast<uint32_t>(d.ve.stats().voiceMoves - moves0),
                           static_cast<uint32_t>(d.ve.stats().noteOns - ons0)});
  }
  std::vector<double> s;
  double maxOn = 0, maxMove = 0, maxQuiet = 0;
  for (const BlockCost& c : blocksOut) {
    s.push_back(c.us);
    if (c.moves > 0) maxMove = std::max(maxMove, c.us);
    else if (c.ons > 0) maxOn = std::max(maxOn, c.us);
    else maxQuiet = std::max(maxQuiet, c.us);
  }
  std::sort(s.begin(), s.end());
  const double period = 1e6 * kBlock / 48000.0;
  const double med = s[s.size() / 2], p99 = s[s.size() * 99 / 100], p999 = s[s.size() * 999 / 1000], mx = s.back();
  std::printf("BENCH ring/2 s voice delay x64, %3.0f bursts/s (thread cycles): per block median %.1f us, p99 %.1f us, "
              "p99.9 %.1f us, max %.1f us = %.1f %% of the %.0f us period (max with a move %.1f, with note-ons %.1f, "
              "neither %.1f); %llu note-ons, %llu moves, %llu skipped, %llu steals, %llu ended\n",
              burstsPerSecond, med, p99, p999, mx, 100.0 * mx / period, period, maxMove, maxOn, maxQuiet,
              static_cast<unsigned long long>(ons), static_cast<unsigned long long>(d.ve.stats().voiceMoves),
              static_cast<unsigned long long>(skippedMoves(d.ve.stats())), static_cast<unsigned long long>(d.ve.stats().steals),
              static_cast<unsigned long long>(d.ve.stats().voicesEnded));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "ring") {  // only the T1.8-fix1 ring rows
    for (double rate : {4.0, 16.0}) ringTraffic(rate, 20000);
    return 0;
  }
  std::printf("T1.8 voices, block %u frames, 48 kHz\n", kBlock);
  overhead("overhead/no voices (9 groups)", 0, e::voiceGroupsFor(64));
  overhead("overhead/8 voices held (2 groups)", 8, e::voiceGroupsFor(8));
  overhead("overhead/64 voices held (9 groups)", 64, e::voiceGroupsFor(64));
  NullImage nul(e::voiceGroupsFor(64), kBlock);
  eventCost("event/null image, 32 voices, no events", &nul, 8, false);
  eventCost("event/null image, 32 voices, 8 on + 8 off", &nul, 8, true);
  eventCost("event/null image, 32 voices, 16 on + 16 off", &nul, 16, true);
  auto fm = compile(kFmVoice, e::voiceGroupsFor(64), kBlock);
  fmEndToEnd(8);
  fmEndToEnd(64);
  fmBare("fm_voice", kFmVoice, 8, 1);
  fmBare("fm_voice", kFmVoice, 64, 1);
  fmBare("fm_voice", kFmVoice, 64, 8);
  fmBare("fm_voice", kFmVoice, 64, 64);
  fmBare("fm_osc", kFmOsc, 64, 1);
  fmBare("fm_osc", kFmOsc, 64, 8);
  fmBare("fm_osc", kFmOsc, 64, 64);
  for (double rate : {8.0, 16.0, 64.0})
    for (bool compact : {false, true}) packing("null", &nul, rate, compact, 200000);
  for (bool compact : {false, true}) packing("fm_voice", fm.get(), 4.0, compact, 30000);
  return 0;
}
