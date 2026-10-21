// T1.9: hot swap costs. Release build only.
//   transfer/*   executeTransfer for a ~1000-node voice patch (125 oscillator chains, 25 short voice delay lines),
//                64 voices (8 of 9 groups occupied), old image → new image with one unrelated node added (stable
//                offsets: few coalesced ranges) and the same without the previous layout (offsets reshuffled)
//   swap_block/* the audio-thread time of the block that performs the swap (ProgramHost::process: transfer, param
//                remap, voice binding switch, then the block) vs an ordinary block, 64 voices held, instant swaps
//   crossfade/*  the blocks of a 10 ms crossfade (two images run on the same voice table, outputs faded) vs ordinary
//                blocks
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/denormal_guard.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/program_host.h"
#include "pq/engine/state_transfer.h"
#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"
#include "pq_bench.h"

using namespace pq;
namespace g = pq::graph;
namespace e = pq::engine;

namespace {

constexpr uint32_t kBlock = 64;
constexpr uint32_t kVoices = 64;

// ~1000 parsed nodes: 125 chains `p_i = sine(phasor(voice.hz · r_i)) · a_i` (every 5th through a 2 ms delay line)
// summed into one voice out.
std::string bigVoicePatch() {
  std::string s = "patch Big {\n  out y : audio voice\n";
  std::string sum;
  for (int i = 0; i < 125; ++i) {
    char line[160];
    std::snprintf(line, sizeof line, "  p%d = sine(phasor(voice.hz * %.4f)) * %.5f\n", i, 1.0 + 0.0137 * i, 0.001 * (i + 1));
    s += line;
    if (i % 5 == 0) {
      std::snprintf(line, sizeof line, "  d%d = delay(p%d, %.3fms, 2ms)\n", i, i, 0.5 + 0.01 * i);
      s += line;
    }
    char term[16];
    std::snprintf(term, sizeof term, i % 5 == 0 ? "d%d" : "p%d", i);
    sum += (i ? " + " : "") + std::string(term);
  }
  s += "  y = " + sum + "\n}\n";
  return s;
}

g::Patch parsePatch(const std::string& text) {
  auto parsed = g::parse(text, "bench.pqp");
  if (!parsed) std::exit(1);
  return parsed->patches[0];
}

g::Patch withSpare(g::Patch p) {
  const g::NodeIndex vh = p.addAtom(g::atomId("voice.hz"));
  const g::NodeIndex k = p.addConst(g::Constant::real(1.5));
  const g::NodeIndex m = p.addAtom(g::atomId("mul"));
  p.connect(vh, m, 0);
  p.connect(k, m, 1);
  const g::NodeIndex ph = p.addAtom(g::atomId("phasor"));
  p.connect(m, ph, 0);
  const g::NodeIndex s = p.addAtom(g::atomId("sine"));
  p.connect(ph, s, 0);
  const g::NodeIndex ex = p.addExport("spare");
  p.connect(s, ex, 0);
  return p;
}

struct Compiled {
  g::Schedule sched;
  uint32_t nodes = 0;
};

Compiled compile(const g::Patch& patch, const g::StateLayout* previous) {
  g::InferOptions io;
  auto typed = g::infer(patch, io);
  if (!typed) std::exit(2);
  g::OptimiseOptions oo;
  oo.sampleRate = 48000.0;
  auto opt = g::optimise(std::move(typed.value()), oo);
  if (!opt) std::exit(3);
  g::ScheduleOptions so;
  so.sampleRate = 48000.0;
  so.previous = previous;
  auto s = g::schedule(opt.value(), so);
  if (!s) std::exit(4);
  return {std::move(s.value()), patch.nodeCount()};
}

std::unique_ptr<e::ProgramImage> build(const g::Schedule& s) {
  e::InterpretOptions eo;
  eo.maxBlockFrames = kBlock;
  eo.maxVoiceGroups = e::voiceGroupsFor(kVoices);
  auto img = e::buildInterpreted(s, eo);
  if (!img) std::exit(5);
  return std::move(img.value());
}

float pitch(uint32_t i) { return 110.0f * std::exp2(static_cast<float>(i % 48) / 12.0f + 0.0001f * static_cast<float>(i)); }

void transferBench(const char* name, e::ProgramImage& from, e::ProgramImage& to, uint32_t occupiedGroups) {
  auto plan = e::buildTransferPlan(from.stateLayout(), from.stateArena(), to.stateLayout(), to.stateArena());
  if (!plan) std::exit(6);
  std::vector<uint32_t> mask(from.stateArena().voiceGroups, 0);
  for (uint32_t gi = 0; gi < occupiedGroups && gi < mask.size(); ++gi) mask[gi] = 0xFF;
  const e::StateArena a = from.stateArena(), b = to.stateArena();
  const bench::Stats st = bench::measure([&] { e::executeTransfer(*plan, a, b, mask.data()); }, 1);
  const double bytes = static_cast<double>(plan->globalBytes) + static_cast<double>(plan->voiceBytes) * occupiedGroups;
  bench::report(name, st, "ns/swap");
  std::printf("      %u global + %u voice ranges (matched %u, fresh %u); %.0f KiB copied (%.1f KiB per group, %u groups)"
              " -> %.1f GB/s\n",
              static_cast<unsigned>(plan->global.size()), static_cast<unsigned>(plan->voice.size()), plan->matched,
              plan->fresh, bytes / 1024.0, static_cast<double>(plan->voiceBytes) / 1024.0, occupiedGroups,
              bytes / st.medianNs);
}

struct HostRun {
  std::unique_ptr<e::ProgramHost> host;
  std::vector<float> out0, out1;
  float* outs[2];
  int64_t time = 0;
  std::vector<std::pair<int64_t, e::NoteEvent>> events;
  static void fill(void* user, int64_t t, uint32_t frames, e::EventBuffer& out) noexcept {
    auto* r = static_cast<HostRun*>(user);
    for (const auto& [at, ev] : r->events)
      if (at >= t && at < t + frames) {
        e::NoteEvent x = ev;
        x.frame = static_cast<uint32_t>(at - t);
        out.push(x);
      }
  }
  HostRun() : out0(kBlock), out1(kBlock) {
    outs[0] = out0.data();
    outs[1] = out1.data();
    e::HostConfig hc;
    hc.sampleRate = 48000.0;
    hc.maxBlockFrames = kBlock;
    hc.numOut = 2;
    hc.maxPending = 64;
    hc.voices = true;
    hc.voice.maxVoices = kVoices;
    hc.voice.compact = false;
    hc.events = {&HostRun::fill, this};
    host = std::make_unique<e::ProgramHost>(hc);
    for (uint32_t i = 0; i < kVoices; ++i) events.push_back({0, e::noteOn(0, i + 1, pitch(i), 0.8f)});
  }
  double block() {  // ns
    e::AudioBlock ab;
    ab.out = outs;
    ab.numOut = 2;
    ab.frames = kBlock;
    ab.frameTime = time;
    const auto t0 = std::chrono::steady_clock::now();
    host->process(ab);
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
    time += kBlock;
    return ns;
  }
};

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

}  // namespace

int main() {
  DenormalGuard dg;
  const g::Patch base = parsePatch(bigVoicePatch());
  const Compiled a = compile(base, nullptr);
  const Compiled b = compile(withSpare(base), &a.sched.state);
  const Compiled bFresh = compile(withSpare(base), nullptr);
  std::printf("T1.9 hot swap, block %u frames, 48 kHz, %u voices: patch %u nodes -> %zu ops, %zu state entries, "
              "%u B global + %u B per voice group\n",
              kBlock, kVoices, a.nodes, a.sched.ops.size(), a.sched.state.entries.size(), a.sched.state.globalBytes,
              a.sched.state.voiceBytes);
  auto ia = build(a.sched), ib = build(b.sched), ibf = build(bFresh.sched);
  transferBench("transfer/+1 node, previous layout, 8 groups", *ia, *ib, 8);
  transferBench("transfer/+1 node, no previous layout, 8 groups", *ia, *ibf, 8);
  transferBench("transfer/+1 node, previous layout, all 9 groups", *ia, *ib, 9);
  transferBench("transfer/+1 node, previous layout, 1 group", *ia, *ib, 1);

  // Swap blocks and crossfades through the host (64 held voices: audible, so Auto would crossfade).
  for (const e::SwapMode mode : {e::SwapMode::Instant, e::SwapMode::Crossfade}) {
    HostRun r;
    const int rounds = 12;
    std::vector<std::unique_ptr<e::ProgramImage>> imgs;
    for (int i = 0; i <= rounds; ++i) imgs.push_back(build(i % 2 == 0 ? a.sched : b.sched));
    if (!r.host->publish(std::move(imgs[0])).ok()) std::exit(7);
    for (int i = 0; i < 200; ++i) r.block();  // warm up, voices sounding
    std::vector<double> normal, swap, fade;
    for (int k = 1; k <= rounds; ++k) {
      for (int i = 0; i < 40; ++i) normal.push_back(r.block());
      if (!r.host->publish(std::move(imgs[k]), e::SwapPolicy{mode, 10.0f}).ok()) std::exit(8);
      swap.push_back(r.block());
      while (r.host->crossfading()) fade.push_back(r.block());
      r.host->collect();
    }
    std::printf("BENCH swap_block/%-9s 64 voices: ordinary block %8.0f ns, swap block %8.0f ns (+%.0f ns), %llu B "
                "transferred\n",
                mode == e::SwapMode::Instant ? "instant" : "crossfade", median(normal), median(swap),
                median(swap) - median(normal), static_cast<unsigned long long>(r.host->stats().transferBytes));
    if (!fade.empty())
      std::printf("BENCH crossfade/10 ms 64 voices: %zu fade blocks after the swap block, median %8.0f ns vs ordinary "
                  "%8.0f ns (x%.2f)\n",
                  fade.size() / rounds, median(fade), median(normal), median(fade) / median(normal));
  }
  return 0;
}
