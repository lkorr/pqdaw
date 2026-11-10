// M6 review (PDC / T6.2, T6.2b): standalone probes against the release libs (build_probes.ps1). Each section prints
// its numbers; nothing here asserts. Modelled on tests/engine/test_pdc.cpp's rig.
//   A  a delayed tail survives its producer going idle (silence flag vs the ring's held audio)
//   B  a shifted automation lane after a loop wrap vs the delay-line content it is applied to
//   C  a dropped line forgotten mid-fade by the next plan (discontinuity)
//   D  audio-thread cost of the block that starts / grows big delay lines (64 tracks, −1000 ms Track Delay)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/time.h"
#include "pq/engine/automation.h"
#include "pq/engine/latency_plan.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/engine/timeline.h"
#include "pq/graph/parse.h"
#include "pq/graph/project.h"

namespace graph = pq::graph;
namespace eng = pq::engine;
using W = graph::WireKind;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 64;

const char* kPatches = R"(
patch Late480 {
  out out : audio global mono
  out = latency(pulse(), 480)
}
patch Late4800 {
  out out : audio global mono
  out = latency(pulse(), 4800)
}
patch Gate {
  out out : audio global mono
  out = gate_to_real(voice.gate)
}
patch Sine {
  out out : audio global mono
  out = sine(phasor(100Hz))
}
patch Quiet {
  out out : audio global mono
  out = sine(phasor(100Hz)) * 0
}
patch Sum {
  in  in : audio global mono
  out out : audio global mono
  out = in
}
)";

graph::PatchFile& lib() {
  static graph::PatchFile f = [] {
    auto r = graph::parse(kPatches, "probe.pqp");
    if (!r.ok()) {
      std::printf("parse: %s\n", r.error().message.c_str());
      std::exit(1);
    }
    return std::move(r.value());
  }();
  return f;
}
const graph::Patch* get(const std::string& n) {
  for (const graph::Patch& p : lib().patches)
    if (p.name() == n) return &p;
  std::printf("no patch %s\n", n.c_str());
  std::exit(1);
}

struct G {
  graph::ProjectGraph g;
  uint32_t add(const std::string& patch, graph::TrackRole role = graph::TrackRole::Audio, uint32_t slot = graph::kNone) {
    g.tracks.push_back({static_cast<uint32_t>(g.tracks.size() + 1), patch + std::to_string(g.tracks.size()), get(patch), role, slot});
    return static_cast<uint32_t>(g.tracks.size() - 1);
  }
  void wire(uint32_t from, const char* fp, uint32_t to, const char* tp, W kind = W::Audio) {
    g.wires.push_back({{from, fp}, {to, tp}, kind});
  }
};

eng::ProjectCompileOptions options() {
  eng::ProjectCompileOptions o;
  o.sampleRate = kSr;
  o.maxBlockFrames = kBlock;
  o.voice.maxVoices = 4;
  o.voice.silenceDb = -300.0f;
  return o;
}

struct Rig {
  std::unique_ptr<eng::ProjectCompiler> compiler;
  std::unique_ptr<eng::ProjectProgram> program;
  std::vector<std::unique_ptr<eng::NoteListSource>> notes;
  eng::CompensationPlan plan;
  int64_t time = 0;
  uint64_t rtViolations = 0;
  double maxBlockUs = 0.0;
};

std::unique_ptr<Rig> makeRig(const graph::ProjectGraph& g, bool compensate, std::vector<std::vector<eng::TimedNote>> notes = {},
                             std::vector<int32_t> offset = {}) {
  auto r = std::make_unique<Rig>();
  r->compiler = std::make_unique<eng::ProjectCompiler>(options());
  auto b = r->compiler->build(g);
  if (!b.ok()) {
    std::printf("build: %s\n", b.error().message.c_str());
    std::exit(1);
  }
  eng::ProjectConfig pc;
  pc.sampleRate = kSr;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 1;
  for (auto& n : notes) {
    r->notes.push_back(std::make_unique<eng::NoteListSource>(n));
    pc.eventSources.push_back(r->notes.back()->source());
  }
  auto prog = eng::ProjectProgram::create(pc, b.value().layout);
  if (!prog.ok()) std::exit(2);
  r->program = std::move(prog.value());
  for (uint32_t u = 0; u < b.value().images.size(); ++u)
    if (!r->program->publish(u, std::move(b.value().images[u])).ok()) std::exit(3);
  r->plan = eng::planCompensation(b.value().layout, r->compiler->latencies(), offset);
  if (compensate && !r->plan.none())
    if (!r->program->setCompensation(r->plan).ok()) std::exit(4);
  return r;
}

std::vector<float> render(Rig& r, uint32_t frames) {
  std::vector<float> out;
  std::vector<float> l(kBlock);
  float* outs[1] = {l.data()};
  r.maxBlockUs = 0.0;
  while (frames > 0) {
    const uint32_t n = std::min(kBlock, frames);
    eng::AudioBlock ab;
    ab.out = outs;
    ab.numOut = 1;
    ab.frames = n;
    ab.frameTime = r.time;
    const auto t0 = std::chrono::steady_clock::now();
    r.rtViolations += pq::alloc_trap::countRtViolations([&] {
      pq::DenormalGuard dg;
      r.program->process(ab);
    });
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    r.maxBlockUs = std::max(r.maxBlockUs, us);
    out.insert(out.end(), l.begin(), l.begin() + n);
    r.time += n;
    frames -= n;
  }
  return out;
}

// ---- A ---------------------------------------------------------------------------------------------------------------
void probeTail(const char* late, int delay) {
  G g;
  const uint32_t a = g.add(late), inst = g.add("Gate", graph::TrackRole::Instrument, 0), m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", m, "in");
  g.wire(inst, "out", m, "in");
  g.g.output = {m, "out"};
  const std::vector<std::vector<eng::TimedNote>> notes = {{{0, 1000, 220.0f, 0.8f, 0.5f}}};
  auto r = makeRig(g.g, true, notes);
  const std::vector<float> y = render(*r, static_cast<uint32_t>(delay) + 4000);
  const uint32_t ui = r->compiler->partition().unitOf[inst];
  size_t ones = 0, first = y.size(), last = 0, stray = 0;
  for (size_t i = 0; i < y.size(); ++i) {
    const float v = i == static_cast<size_t>(delay) ? y[i] - 1.0f : y[i];  // minus the late impulse
    if (v == 1.0f) {
      ++ones;
      first = std::min(first, i);
      last = i;
    } else if (v != 0.0f) {
      ++stray;
    }
  }
  std::printf("A tail  delay %5d: gate frames heard %zu (want 1000) at [%zu, %zu] (want [%d, %d]); stray %zu; "
              "gate unit idle blocks %llu; rt violations %llu\n",
              delay, ones, first, last, delay, delay + 999, stray,
              static_cast<unsigned long long>(r->program->unitStats(ui).idleBlocks),
              static_cast<unsigned long long>(r->rtViolations));
}

// ---- B ---------------------------------------------------------------------------------------------------------------
void probeLoopWrap() {
  // A step 0 → 1 at the loop start (automation "fades in" the loop). Lane 0 unshifted, lane 1 shifted 480 (its unit
  // hears its inputs/clips/notes 480 late through delay lines). A delay line at the reader replays the end of the
  // previous pass for the first 480 frames after a wrap; what the shifted lane should give there is lane 0 480 frames
  // earlier (engine time).
  eng::TimelineConfig tc;
  tc.sampleRate = kSr;
  tc.maxBlockFrames = kBlock;
  eng::Timeline tl(tc);
  const pq::Ticks S = pq::kPpq, E = 2 * pq::kPpq;  // [24000, 48000) samples at 120 BPM
  const uint32_t shift = 480;
  const float* taps[2] = {nullptr, nullptr};
  std::vector<eng::AutomationLaneDesc> lanes(2);
  for (uint32_t l = 0; l < 2; ++l) {
    lanes[l].target = l;
    lanes[l].tap = &taps[l];
    lanes[l].points = {{0, 0.0f, 0.0f}, {S, 0.0f, 0.0f}, {S, 1.0f, 0.0f}};
  }
  lanes[1].shiftFrames = shift;
  auto img = eng::AutomationImage::build(lanes);
  if (!img.ok() || !tl.publishAutomation(std::move(img.value())).ok()) std::exit(5);
  eng::TransportCommand loop;
  loop.kind = eng::TransportCommandKind::SetLoop;
  loop.loopEnabled = true;
  loop.loopStart = S;
  loop.loopEnd = E;
  tl.apply(loop);
  eng::TransportCommand play;
  play.kind = eng::TransportCommandKind::Play;
  play.tick = S;
  tl.apply(play);
  std::vector<float> params(2, -1.0f);
  std::vector<float> v0, v1;
  const int64_t total = 24000 * 3 + 4000;
  for (int64_t t = 0; t < total; t += kBlock) {
    tl.process(t, kBlock);
    tl.writeAutomatedParams(params);
    for (uint32_t i = 0; i < kBlock; ++i) {
      v0.push_back(taps[0] != nullptr ? taps[0][i] : params[0]);
      v1.push_back(taps[1] != nullptr ? taps[1][i] : params[1]);
    }
  }
  // Engine frame 24000·k is the k-th wrap (play started at the loop start at engine frame 0).
  for (int k = 1; k <= 3; ++k) {
    const size_t w = static_cast<size_t>(24000 * k);
    size_t mism = 0, firstMis = 0;
    for (size_t i = w; i < w + shift; ++i)
      if (v1[i] != v0[i - shift]) {
        if (mism == 0) firstMis = i - w;
        ++mism;
      }
    std::printf("B wrap %d: shifted lane over [wrap, wrap+%u): %zu frames differ from the delay-line-consistent value "
                "(first at +%zu; lane1=%g, want %g)\n",
                k, shift, mism, firstMis, v1[w], v0[w - shift]);
  }
  size_t steady = 0;
  for (size_t i = shift; i < 24000; ++i) steady += v1[i] != v0[i - shift];
  std::printf("B first pass (no wrap): %zu frames differ (control)\n", steady);
}

// ---- C ---------------------------------------------------------------------------------------------------------------
void probeDropMidFade() {
  G g;
  const uint32_t a = g.add("Sine"), q = g.add("Quiet"), grp = g.add("Sum", graph::TrackRole::Group),
                 m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", grp, "in");
  g.wire(q, "out", grp, "in");
  g.wire(grp, "out", m, "in");
  g.g.output = {m, "out"};
  auto run = [&](bool second) {
    auto r = makeRig(g.g, false);
    const uint32_t uq = r->compiler->partition().unitOf[q];
    auto planWith = [&](double late) {
      std::vector<eng::UnitLatency> lat = r->compiler->latencies();
      for (auto& [bus, frames] : lat[uq].out) frames = late;
      return eng::planCompensation(r->program->layout(), lat);
    };
    (void)r->program->setCompensation(planWith(64.0));
    render(*r, 4800);
    (void)r->program->setCompensation(planWith(0.0));  // a's line dropped: fades 64 → 0 over 480 frames
    std::vector<float> y = render(*r, 128);             // two blocks into the fade
    if (second) (void)r->program->setCompensation(planWith(0.0));  // the next replan (e.g. a Track Delay drag step)
    const std::vector<float> y2 = render(*r, 1024);
    y.insert(y.end(), y2.begin(), y2.end());
    return y;
  };
  const std::vector<float> ref = run(false), y = run(true);
  double jumpRef = 0.0, jump = 0.0, diff = 0.0;
  for (size_t i = 1; i < y.size(); ++i) {
    jumpRef = std::max(jumpRef, static_cast<double>(std::fabs(ref[i] - ref[i - 1])));
    jump = std::max(jump, static_cast<double>(std::fabs(y[i] - y[i - 1])));
    diff = std::max(diff, static_cast<double>(std::fabs(y[i] - ref[i])));
  }
  std::printf("C drop mid-fade: max |y[n]-y[n-1]| %.4f with a second plan 128 frames into the fade vs %.4f for the "
              "full fade; max |y - ref| %.4f (100 Hz sine, amplitude 1)\n",
              jump, jumpRef, diff);
}

// ---- D ---------------------------------------------------------------------------------------------------------------
void probeBigDelayCost() {
  G g;
  std::vector<uint32_t> tr;
  for (int i = 0; i < 64; ++i) tr.push_back(g.add("Sine"));
  const uint32_t m = g.add("Sum", graph::TrackRole::Master);
  for (uint32_t t : tr) g.wire(t, "out", m, "in");
  g.g.output = {m, "out"};
  auto r = makeRig(g.g, false);
  const graph::ProjectPartition& part = r->compiler->partition();
  auto planWithOffset = [&](int32_t frames) {
    std::vector<int32_t> off(part.units.size(), 0);
    off[part.unitOf[tr[0]]] = frames;  // track 0 with a negative Track Delay: every other input waits `frames`
    return eng::planCompensation(r->program->layout(), r->compiler->latencies(), off);
  };
  render(*r, 48000);
  double steady = 0.0;
  {
    std::vector<double> v;
    for (int k = 0; k < 200; ++k) {
      render(*r, kBlock);
      v.push_back(r->maxBlockUs);
    }
    std::sort(v.begin(), v.end());
    steady = v[v.size() / 2];
  }
  auto switchBlock = [&](int32_t frames, const char* what) {
    if (!r->program->setCompensation(planWithOffset(frames)).ok()) std::printf("setCompensation failed\n");
    render(*r, kBlock);
    const double us = r->maxBlockUs;
    std::vector<double> v;
    for (int k = 0; k < 50; ++k) {
      render(*r, kBlock);
      v.push_back(r->maxBlockUs);
    }
    std::sort(v.begin(), v.end());
    std::printf("D %-38s switch block %8.1f us; next blocks median %6.1f us (steady, no PDC: %.1f us; 64-frame block = "
                "1333 us)\n",
                what, us, v[v.size() / 2], steady);
  };
  switchBlock(24000, "63 new mono lines of 24000 (-500 ms)");
  render(*r, 48000);
  switchBlock(48000, "63 lines grow 24000 -> 48000 (-1000 ms)");
  render(*r, 48000);
  switchBlock(47000, "63 lines tap 48000 -> 47000 (fit)");
  std::printf("D rt violations %llu\n", static_cast<unsigned long long>(r->rtViolations));
}

}  // namespace

int main() {
  pq::DenormalGuard dg;
  probeTail("Late480", 480);
  probeTail("Late4800", 4800);
  probeLoopWrap();
  probeDropMidFade();
  probeBigDelayCost();
  return 0;
}
