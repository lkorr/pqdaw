// T1.5: the scheduler. Schedule structure (expected dumps), SCCs and feedback regions with split kernels and
// sub-blocks, delay-free cycles (text error at a source location / visual-mode z1 + warning), solve blocks,
// voice/global stages (broadcasts, reducers, per-group buffers), buffer liveness (a 100-node chain needs ≤ 3
// scratch buffers), the state layout (identical across reschedules, stable against edits given the previous
// layout), structural invariants over the test corpus and random patches, and numerical equivalence: every
// schedule of a supported patch, run block-wise with its shared buffers, matches a naive per-sample run of the
// patch with the same kernels (sched_exec.h).

#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <algorithm>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"
#include "sched_exec.h"
#include "sched_replay.h"
#include "text_util.h"

using namespace pq::graph;
namespace dsp = pq::dsp;

namespace {

std::string diagText(const std::vector<Diagnostic>& ds, std::string_view src) {
  std::string s;
  for (const Diagnostic& d : ds) s += formatDiagnostic(src, d) + "\n";
  return s;
}

struct Compiled {
  std::string text;
  ParseResult parsed;
  PatchLibrary lib;
  TypedPatch typed;
  ScheduleResult sched;
};

// Parse → infer (first patch) → optimise (48 kHz) → schedule. `edit` may change the parsed root patch first (NodeIds
// survive API edits, as they survive document edits).
template <class Edit>
void compileWith(Compiled& c, std::string_view src, ScheduleOptions so, Edit&& edit) {
  c.text = std::string(src);
  c.parsed = parseDetailed(c.text, "test.pqp");
  REQUIRE_MESSAGE(c.parsed.ok(), diagText(c.parsed.diagnostics, c.text));
  edit(c.parsed.file.patches[0]);
  c.lib.add(c.parsed.file);
  InferOptions io;
  io.library = &c.lib;
  io.source = {c.text, "test.pqp", c.parsed.nodeSpans[0]};
  InferResult inf = inferDetailed(c.parsed.file.patches[0], io);
  REQUIRE_MESSAGE(inf.ok(), diagText(inf.diagnostics, c.text));
  OptimiseOptions oo;
  oo.sampleRate = 48000.0;
  auto opt = optimise(std::move(inf.typed), oo);
  REQUIRE_MESSAGE(opt.ok(), (opt.ok() ? std::string() : opt.error().message));
  c.typed = std::move(opt.value());
  so.source = {c.text, "test.pqp", c.parsed.nodeSpans[0]};
  c.sched = scheduleDetailed(c.typed, so);
}

void compile(Compiled& c, std::string_view src, ScheduleOptions so = {}) {
  compileWith(c, src, so, [](Patch&) {});
}

const Schedule& ok(const Compiled& c) {
  REQUIRE_MESSAGE(c.sched.ok(), diagText(c.sched.diagnostics, c.text));
  return c.sched.schedule;
}

std::string dumpOf(std::string_view src, ScheduleOptions so = {}) {
  Compiled c;
  compile(c, src, so);
  return dump(ok(c), c.typed);
}

std::string errorsOf(std::string_view src, ScheduleOptions so = {}) {
  Compiled c;
  compile(c, src, so);
  REQUIRE_FALSE(c.sched.ok());
  return diagText(c.sched.diagnostics, c.text);
}

// Structural invariants every schedule must satisfy (independent of the patch).
void checkStructure(const Schedule& s, const TypedPatch& tp) {
  const auto numOps = static_cast<uint32_t>(s.ops.size());
  // Regions tile the op list; stages tile the regions; parity = poly; stage numbers increase.
  uint32_t nextOp = 0, nextRegion = 0;
  int64_t lastStage = -1;
  for (const Stage& sg : s.stages) {
    CHECK(static_cast<int64_t>(sg.index) > lastStage);
    lastStage = sg.index;
    CHECK(sg.poly == ((sg.index & 1) ? Poly::Voice : Poly::Global));
    CHECK(sg.firstRegion == nextRegion);
    CHECK(sg.firstOp == nextOp);
    for (uint32_t r = sg.firstRegion; r < sg.firstRegion + sg.numRegions; ++r) {
      const Region& rg = s.regions[r];
      CHECK(rg.stage == sg.index);
      CHECK(rg.poly == sg.poly);
      CHECK(rg.firstOp == nextOp);
      CHECK((rg.kind == RegionKind::Loop) == (rg.subBlock >= 1));
      nextOp += rg.numOps;
      for (uint32_t k = rg.firstSolve; k < rg.firstSolve + rg.numSolves; ++k) {
        const SolveBlock& b = s.solves[k];
        CHECK(b.region == r);
        CHECK(b.firstOp >= rg.firstOp);
        CHECK(b.firstOp + b.numOps <= rg.firstOp + rg.numOps);
        CHECK(b.numUnknowns == b.numEquations);
        CHECK(b.numUnknowns > 0);
      }
    }
    nextRegion += sg.numRegions;
    CHECK(sg.firstOp + sg.numOps == nextOp);
  }
  CHECK(nextOp == numOps);
  CHECK(nextRegion == s.regions.size());
  // Operands are in range; Control ops are 8-lane pattern ops; kernels exist.
  auto valid = [&](const Operand& o) {
    switch (o.kind) {
      case Operand::Kind::None: return true;
      case Operand::Kind::Buffer: return o.index < s.buffers.size();
      case Operand::Kind::Param: return o.index < s.params.size();
      case Operand::Kind::Const: return o.index < s.constants.size();
      case Operand::Kind::External: return o.index < s.externals.size();
    }
    return false;
  };
  for (const Region& rg : s.regions) {
    for (uint32_t i = rg.firstOp; i < rg.firstOp + rg.numOps; ++i) {
      const Op& op = s.ops[i];
      for (const Operand& o : s.inputs(op)) CHECK(valid(o));
      for (const Operand& o : s.outputsOf(op)) {
        CHECK(o.kind == Operand::Kind::Buffer);
        CHECK(valid(o));
        if (o.kind == Operand::Kind::Buffer && rg.kind == RegionKind::Control)
          CHECK(s.buffers[o.index].cls == Buffer::Class::Pattern);
      }
      if (rg.kind == RegionKind::Control) {
        CHECK(op.lanes == 8);
        CHECK(op.splMask == 0);
      } else if (op.kind != OpKind::Broadcast && op.kind != OpKind::Reduce) {
        CHECK(op.lanes == (rg.poly == Poly::Voice ? 8 : 1));
      }
      if (op.kind == OpKind::Kernel) {
        CHECK(dsp::findKernel(op.kernel, op.lanes, op.splMask) != nullptr);
        // Operand counts are the kernel's (M1 review H2: a stereo delay:read in a loop got (time, time)).
        const dsp::KernelInfo& ki = dsp::kernelInfo(op.kernel);
        INFO("op #" << i << " " << std::string(ki.name));
        if (ki.variadicIn) CHECK(op.numIn >= ki.numIn);
        else CHECK(op.numIn == ki.numIn);
        if (ki.variadicOut) CHECK(op.numOut >= ki.numOut);
        else CHECK(op.numOut == ki.numOut);
        CHECK((op.state != kNone) == (dsp::kernelInfo(op.kernel).stateSize > 0));
      }
      if (op.state != kNone) {
        REQUIRE(op.state < s.state.entries.size());
        CHECK(s.state.entries[op.state].arena == (op.lanes == 8 ? Poly::Voice : Poly::Global));
      }
    }
  }
  for (const SolveUnknown& u : s.unknowns) {
    CHECK(u.value.kind == Operand::Kind::Buffer);
    CHECK((u.init.kind == Operand::Kind::None) == (u.state != kNone));
  }
  for (const OutputRef& o : s.outputs) CHECK(valid(o.value));
  // State: entries inside their arena, 32-byte aligned, no overlap, unique (key, channel, arena).
  for (int a = 0; a < 2; ++a) {
    const Poly arena = a ? Poly::Voice : Poly::Global;
    const uint32_t bytes = a ? s.state.voiceBytes : s.state.globalBytes;
    CHECK(bytes % 64 == 0);
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (const StateEntry& e : s.state.entries) {
      if (e.arena != arena) continue;
      CHECK(e.offset % dsp::kStateAlign == 0);
      CHECK(e.size % dsp::kStateAlign == 0);
      CHECK(e.offset + e.size <= bytes);
      if (e.tag.kernel != StateTag::kSolveGuess)
        CHECK(e.size >= dsp::stateBytes(static_cast<dsp::Atom>(e.tag.kernel), e.init));
      ranges.emplace_back(e.offset, e.offset + e.size);
    }
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i) CHECK(ranges[i].first >= ranges[i - 1].second);
  }
  std::set<std::tuple<uint64_t, uint16_t, int>> keys;
  for (const StateEntry& e : s.state.entries) CHECK(keys.insert({e.key, e.channel, e.arena == Poly::Voice}).second);
  // Every param node has a slot.
  uint32_t params = 0;
  for (NodeIndex n = 0; n < tp.patch.nodeCount(); ++n) params += tp.patch.node(n).kind == NodeKind::Param;
  CHECK(params == s.params.size());
}

const char* const kChainA = R"(patch A {
  in x : audio
  param g = 0.5 [0..1]
  out y : audio
  y = tanh(x * g * 2) + z1(x)
}
)";

const char* const kFeedback = R"(patch Fb {
  in x : audio
  param fb = 0.5 [0..0.99]
  param time = 250ms [1ms..2s]
  out y : audio
  echo = delay(x + echo * fb, time, 2s)
  lp = z1(lp + (echo - lp) * 0.2)
  y = lp
}
)";

const char* const kDiode = R"(patch D {
  in x : audio
  param drive = 4 [1..20]
  out y : audio
  solve(iterations: 8) {
    unknown v = x
    v == x - drive * tanh(v) * 0.1
  }
  y = v
}
)";

const char* const kVoiceGlobal = R"(patch V {
  param cutoff = 1000 [20..20000]
  in lfo : audio global
  out y : audio global
  v = sine(phasor(voice.hz)) * (lfo + voice.velocity * cutoff)
  y = v
}
)";

const char* const kInstrument = R"(patch I {
  param a = 2 [1..4]
  out o : audio voice
  solve(iterations: 4) {
    unknown k
    k * k == a
  }
  solve(iterations: 2) {
    unknown v
    v == tanh(sine(phasor(voice.hz)) * k - v)
  }
  o = v
}
)";

const char* const kStereo = R"(patch St {
  in x : audio stereo
  param g = 0.5 [0..1]
  out y : audio stereo
  out m : audio
  w = z1(x) * g
  y = (w[1], w[0] + 1)
  m = w[0] * w[1]
}
)";

const char* const kPerGroup = R"(patch G {
  out y : audio voice
  v = sine(phasor(voice.hz))
  s = voice_sum(v)
  y = v * s
}
)";

const char* const kCombs = R"(patch C {
  in x : audio
  out y : audio
  a = delay_cubic(x + a * 0.5, 100smp, 200smp)
  b = delay_allpass(a + b * 0.3, 7.2smp, 20smp)
  y = b
}
)";

const char* const kLoops = R"(patch L {
  in x : audio
  out y : audio
  a = z1(a * 0.5 + x)
  b = z1(b * 0.25 + x)
  c = delay(c * 0.5 + x, 10smp, 20smp)
  d = delay_cubic(d * 0.5 + a, 5smp, 20smp)
  y = a + b + c + d
}
)";

// A chain of `n` nodes: kind 0 = element-wise (tanh), 1 = stateful (z1), 2 = the bench chain (t = fast_tanh(prev ·
// p); a = t + z1(t): three ops per step).
std::string chainText(int n, int kind, bool voice) {
  const std::string poly = voice ? "voice" : "global";
  std::string s = "patch Chain {\n  in x : audio " + poly + "\n  param p = 0.5 [0..1]\n  out y : audio " + poly + "\n";
  std::string prev = "x";
  for (int i = 0; i < n; ++i) {
    const std::string name = "n" + std::to_string(i);
    if (kind == 0) s += "  " + name + " = tanh(" + prev + ")\n";
    else if (kind == 1) s += "  " + name + " = z1(" + prev + ")\n";
    else s += "  t" + name + " = fast_tanh(" + prev + " * p)\n  " + name + " = t" + name + " + z1(t" + name + ")\n";
    prev = name;
  }
  return s + "  y = " + prev + "\n}\n";
}

uint32_t opNodes(const Schedule& s) {
  std::set<NodeIndex> nodes;
  for (const Op& op : s.ops) nodes.insert(op.node);
  return static_cast<uint32_t>(nodes.size());
}

}  // namespace

// ---- structure: expected dumps --------------------------------------------------------------------------------

TEST_CASE("schedule: a block-wise chain with in-place ops and a z1 outside any loop") {
  CHECK(dumpOf(kChainA) == R"(schedule 'A': 1 stages, 1 regions, 5 ops; buffers: 2 block, 0 pattern; state: 64 B global, 0 B per voice group
params:
  p0 'g' real = 0.5 [0..1]
externals:
  e0 in#0 'x' ch0 audio global
buffers: b0=blk1 b1=blk1
stage 0 global
  region 0 block
    #0 mul/1 (e0, ~p0) -> b0  mul#6
    #1 mul/1 (b0, ~2) -> b0  mul#5
    #2 tanh/1 (b0) -> b0  tanh#4
    #3 z1/1 (e0) -> b1 [s0]  z1#8
    #4 add/1 (b0, b1) -> b0  add#3
outputs:
  out 'y' ch0 <- b0 (global)
state global (64 B):
  s0 @0 +32 z1/1 z1#8 ch0
)");
}

TEST_CASE("schedule: feedback SCCs become loops with split kernels; delay loops run in sub-blocks") {
  // The echo loop's only cut is a delay of at least 1 ms (the param's range minimum) = 48 samples: sub-blocks of 48.
  // The one-pole loop has a z1: per sample. The control region computes time · sr once per block.
  CHECK(dumpOf(kFeedback) == R"(schedule 'Fb': 1 stages, 3 regions, 8 ops; buffers: 3 block, 1 pattern; state: 524416 B global, 0 B per voice group
params:
  p0 'fb' real = 0.5 [0..0.99]
  p1 'time' real = 0.25 [0.001..2]
externals:
  e0 in#0 'x' ch0 audio global
buffers: b0=pat b1=blk1 b2=blk1 b3=blk1
stage 0 global
  region 0 control
    #0 mul/8 (p1, 48000) -> b0  mul#9
  region 1 loop, sub-block 48
    #1 delay:read/1 (~b0) -> b1 [s0]  'echo'
    #2 muladd/1 (b1, ~p0, e0) -> b2  muladd#12
    #3 delay:write/1 (b2) [s0]  'echo'
  region 2 loop, sub-block 1
    #4 z1:read/1 () -> b2 [s1]  'lp'
    #5 sub/1 (b1, b2) -> b3  sub#6
    #6 muladd/1 (b3, ~0.2, b2) -> b3  muladd#11
    #7 z1:write/1 (b3) [s1]  'lp'
outputs:
  out 'y' ch0 <- b2 (global)
state global (524416 B):
  s0 @0 +524384 delay/1 frames 96000 'echo' ch0
  s1 @524384 +32 z1/1 'lp' ch0
)");
  // Without trusting param ranges the echo loop runs per sample.
  ScheduleOptions so;
  so.trustParamRanges = false;
  Compiled c;
  compile(c, kFeedback, so);
  CHECK(ok(c).regions[1].subBlock == 1);
  // Delay interpolators: cubic ⌊D⌋ − 1, allpass ⌊D − 0.5⌋.
  CHECK(dumpOf(kCombs) == R"(schedule 'C': 1 stages, 2 regions, 6 ops; buffers: 3 block, 0 pattern; state: 1344 B global, 0 B per voice group
externals:
  e0 in#0 'x' ch0 audio global
buffers: b0=blk1 b1=blk1 b2=blk1
stage 0 global
  region 0 loop, sub-block 99
    #0 delay_cubic:read/1 (~100) -> b0 [s0]  'a'
    #1 muladd/1 (b0, ~0.5, e0) -> b1  muladd#11
    #2 delay:write/1 (b1) [s0]  'a'
  region 1 loop, sub-block 6
    #3 delay_allpass:read/1 (~7.2) -> b1 [s1]  'b'
    #4 muladd/1 (b1, ~0.3, b0) -> b2  muladd#10
    #5 delay:write/1 (b2) [s1]  'b'
outputs:
  out 'y' ch0 <- b1 (global)
state global (1344 B):
  s0 @0 +1120 delay_cubic/1 frames 200 'a' ch0
  s1 @1120 +224 delay_allpass/1 frames 20 'b' ch0
)");
}

TEST_CASE("schedule: loops with the same sub-block share a region; each different sub-block gets its own") {
  Compiled c;
  compile(c, kLoops);
  const Schedule& s = ok(c);
  checkStructure(s, c.typed);
  std::vector<uint32_t> subs;
  for (const Region& r : s.regions)
    if (r.kind == RegionKind::Loop) subs.push_back(r.subBlock);
  std::sort(subs.begin(), subs.end());
  // a and b (both z1: per sample) share one region; c (linear, 10) and d (cubic, 5 − 1) have their own.
  CHECK(subs == std::vector<uint32_t>{1, 4, 10});
}

TEST_CASE("schedule: solve blocks (audio rate in a loop, param rate in the control region, previous-solution state)") {
  CHECK(dumpOf(kDiode) == R"(schedule 'D': 1 stages, 1 regions, 4 ops; buffers: 2 block, 0 pattern; state: 0 B global, 0 B per voice group
params:
  p0 'drive' real = 4 [1..20]
externals:
  e0 in#0 'x' ch0 audio global
buffers: b0=blk1 b1=blk1
stage 0 global
  region 0 loop, sub-block 1
    solve #0 (8 iterations): ops #0..#3; unknowns 'v'=b0 (init e0); equations b0 == b1
    #0 tanh/1 (b0) -> b1  tanh#8
    #1 mul/1 (~p0, b1) -> b1  mul#7
    #2 mul/1 (b1, ~0.1) -> b1  mul#6
    #3 sub/1 (e0, b1) -> b1  sub#5
outputs:
  out 'y' ch0 <- b0 (global)
)");
  CHECK(dumpOf(kInstrument) == R"(schedule 'I': 2 stages, 3 regions, 6 ops; buffers: 3 block, 2 pattern; state: 64 B global, 128 B per voice group
params:
  p0 'a' real = 2 [1..4]
externals:
  e0 voice.hz ch0 audio voice
buffers: b0=pat b1=pat b2=blk8 b3=blk8 b4=blk8
stage 0 global
  region 0 control
    solve #0 (4 iterations): ops #0..#0; unknowns 'k'=b0 (prev s0); equations b1 == p0
    #0 mul/8 (b0, b0) -> b1  mul#12
stage 1 voice
  region 1 block
    #1 phasor/8 (e0, ~false) -> b2 [s1]  phasor#10
    #2 sine/8 (b2) -> b2  sine#9
    #3 mul/8 (b2, ~b0) -> b2  mul#8
  region 2 loop, sub-block 1
    solve #1 (2 iterations): ops #4..#5; unknowns 'v'=b3 (prev s2); equations b3 == b4
    #4 sub/8 (b2, b3) -> b4  sub#7
    #5 tanh/8 (b4) -> b4  tanh#6
outputs:
  out 'o' ch0 <- b3 (voice, end of stage 1)
state global (64 B):
  s0 @0 +32 guess/1 'k' ch0
state voice (128 B per group):
  s1 @0 +64 phasor/8 phasor#10 ch0
  s2 @64 +32 guess/8 'v' ch0
)");
  // The corpus file's TwoUnknowns: two unknowns in one block, a block read by another, an initial guess from a
  // third block.
  std::string two = text_util::readFile(text_util::casesDir() / "solve.pqp");
  two = two.substr(two.find("patch TwoUnknowns"));
  Compiled c2;
  compile(c2, two);
  const Schedule& s = ok(c2);
  checkStructure(s, c2.typed);
  REQUIRE(s.solves.size() == 3);
  CHECK(s.solves[0].numUnknowns == 2);
  CHECK(s.solves[0].iterations == 4);
  CHECK(s.solves[1].iterations == 1);
  CHECK(s.solves[2].iterations == 64);
  CHECK(s.unknowns[s.solves[2].firstUnknown].init == s.unknowns[s.solves[1].firstUnknown].value);
}

TEST_CASE("schedule: voice and global stages, broadcasts, reducers and per-group buffers") {
  // A global audio input read by voice ops is broadcast once per block in the global stage; the reducer runs in
  // the voice stage and its accumulator is a global 1-lane buffer.
  CHECK(dumpOf(kVoiceGlobal) == R"(schedule 'V': 2 stages, 3 regions, 7 ops; buffers: 4 block, 1 pattern; state: 0 B global, 64 B per voice group
params:
  p0 'cutoff' real = 1000 [20..20000]
externals:
  e0 in#0 'lfo' ch0 audio global
  e1 voice.hz ch0 audio voice
  e2 voice.velocity ch0 param voice
buffers: b0=blk8 b1=pat b2=blk1 b3=blk8 b4=blk8
stage 0 global
  region 0 block
    #0 broadcast/8 (e0) -> b0  'lfo'
stage 1 voice
  region 1 control
    #1 mul/8 (e2, p0) -> b1  mul#9
  region 2 block
    #2 add/8 (b0, ~b1) -> b3  add#8
    #3 phasor/8 (e1, ~false) -> b4 [s0]  phasor#6
    #4 sine/8 (b4) -> b4  sine#5
    #5 mul/8 (b4, b3) -> b4  'v'
    #6 voice_sum/8 (b4) -> b2  voice_sum#3
outputs:
  out 'y' ch0 <- b2 (global)
state voice (64 B per group):
  s0 @0 +64 phasor/8 phasor#6 ch0
)");
  // v is read again after the reducer (stage 3): it needs one buffer per voice group; the reduced sum is
  // broadcast in the global stage between. The broadcast copy b0 is read by every voice group of stage 3, so the
  // mul cannot write over it in place (T1.7: it did, and voice group 1 read group 0's product).
  CHECK(dumpOf(kPerGroup) == R"(schedule 'G': 3 stages, 3 regions, 5 ops; buffers: 4 block, 0 pattern; state: 0 B global, 64 B per voice group
externals:
  e0 voice.hz ch0 audio voice
buffers: b0=blk8 b1=blk1 b2=blk8/group b3=blk8
stage 1 voice
  region 0 block
    #0 phasor/8 (e0, ~false) -> b0 [s0]  phasor#3
    #1 sine/8 (b0) -> b2  'v'
    #2 voice_sum/8 (b2) -> b1  's'
stage 2 global
  region 1 block
    #3 broadcast/8 (b1) -> b0  's'
stage 3 voice
  region 2 block
    #4 mul/8 (b2, b0) -> b3  mul#5
outputs:
  out 'y' ch0 <- b3 (voice, end of stage 3)
state voice (64 B per group):
  s0 @0 +64 phasor/8 phasor#3 ch0
)");
}

TEST_CASE("schedule: multichannel signals: one op and one state block per channel; pack/channel are views") {
  CHECK(dumpOf(kStereo) == R"(schedule 'St': 1 stages, 1 regions, 6 ops; buffers: 3 block, 0 pattern; state: 64 B global, 0 B per voice group
params:
  p0 'g' real = 0.5 [0..1]
externals:
  e0 in#0 'x' ch0 audio global
  e1 in#0 'x' ch1 audio global
buffers: b0=blk1 b1=blk1 b2=blk1
stage 0 global
  region 0 block
    #0 z1/1 (e0) -> b0 [s0]  z1#5
    #1 z1/1 (e1) -> b1 [s1]  z1#5 ch1
    #2 mul/1 (b0, ~p0) -> b0  'w'
    #3 mul/1 (b1, ~p0) -> b1  'w' ch1
    #4 add/1 (b0, ~1) -> b2  add#12
    #5 mul/1 (b0, b1) -> b0  mul#6
outputs:
  out 'y' ch0 <- b1 (global)
  out 'y' ch1 <- b2 (global)
  out 'm' ch0 <- b0 (global)
state global (64 B):
  s0 @0 +32 z1/1 z1#5 ch0
  s1 @32 +32 z1/1 z1#5 ch1
)");
}

TEST_CASE("schedule: every corpus patch schedules with valid structure (FmVoice: all audio per voice)") {
  text_util::DirLoader loader(text_util::casesDir());
  uint32_t scheduled = 0;
  bool sawFm = false;
  for (const auto& path : text_util::pqpFiles(text_util::casesDir())) {
    const std::string text = text_util::readFile(path);
    ParseOptions po;
    po.imports = &loader;
    const ParseResult parsed = parseDetailed(text, path.string(), po);
    if (!parsed.ok()) continue;
    std::vector<ParseResult> imported;
    for (const std::string& imp : parsed.file.imports) {
      const auto ipath = path.parent_path() / imp;
      ParseOptions ipo;
      ipo.imports = &loader;
      imported.push_back(parseDetailed(text_util::readFile(ipath), ipath.string(), ipo));
    }
    PatchLibrary lib;
    lib.add(parsed.file);
    for (const ParseResult& ip : imported) lib.add(ip.file);
    for (size_t k = 0; k < parsed.file.patches.size(); ++k) {
      InferOptions io;
      io.library = &lib;
      InferResult inf = inferDetailed(parsed.file.patches[k], io);
      if (!inf.ok()) continue;
      OptimiseOptions oo;
      oo.sampleRate = 44100.0;
      auto opt = optimise(std::move(inf.typed), oo);
      REQUIRE(opt.ok());
      ScheduleOptions so;
      so.sampleRate = 44100.0;
      const ScheduleResult r = scheduleDetailed(opt.value(), so);
      INFO(path.string() << " patch " << parsed.file.patches[k].name());
      CHECK_MESSAGE(r.ok(), diagText(r.diagnostics, text));
      if (!r.ok()) continue;
      checkStructure(r.schedule, opt.value());
      CHECK(!dump(r.schedule, opt.value()).empty());
      ++scheduled;
      if (parsed.file.patches[k].name() == "FmVoice") {
        sawFm = true;
        const Schedule& s = r.schedule;
        REQUIRE(!s.outputs.empty());
        CHECK(s.outputs[0].poly == Poly::Voice);
        // Every audio-rate op is per voice. Since select sinking (T1.4-fix1) the adsr's coefficients of the global
        // attack/release params are computed once per block in a global stage before the voices: Control only.
        uint32_t voiceStages = 0;
        for (const Stage& sg : s.stages) {
          if (sg.poly == Poly::Voice) {
            ++voiceStages;
            continue;
          }
          for (uint32_t ri = sg.firstRegion; ri < sg.firstRegion + sg.numRegions; ++ri)
            CHECK(s.regions[ri].kind == RegionKind::Control);
        }
        CHECK(voiceStages >= 1);
      }
    }
  }
  CHECK(sawFm);
  CHECK(scheduled >= 10);
}

// ---- delay-free cycles ------------------------------------------------------------------------------------------

const char* const kZeroDelay = R"(patch Z {
  in x : audio
  out y : audio
  a = x + b * 0.5
  b = tanh(a)
  y = b
}
)";

TEST_CASE("schedule: a delay-free cycle is an error in text, located at a node of the cycle") {
  const std::string e = errorsOf(kZeroDelay);
  // Reported at a node of the cycle (a on line 4 or b on line 5), naming the whole cycle.
  CHECK((e.find("test.pqp:4:") != std::string::npos || e.find("test.pqp:5:") != std::string::npos));
  CHECK((e.find("delay-free feedback loop 'a' → 'b' → 'a'") != std::string::npos ||
         e.find("delay-free feedback loop 'b' → 'a' → 'b'") != std::string::npos));
  CHECK(e.find("^") != std::string::npos);
  // The schedule() wrapper reports the same as an Error.
  Compiled c;
  compile(c, kZeroDelay);
  auto r = schedule(c.typed);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == pq::ErrorCode::Type);
  CHECK(r.error().message.find("delay-free feedback loop") != std::string::npos);
}

TEST_CASE("schedule: a phasor / sine_hz reset fed by voice.gate is warned about (level-sensitive, Q14)") {
  Compiled c;
  compile(c, R"(patch R {
  out y : audio voice
  a = sine(phasor(voice.hz, reset: voice.gate))
  b = sine_hz(voice.hz * 2, reset: voice.gate)
  e = sine(phasor(voice.hz * 3, reset: rise(voice.gate)))
  y = a + b + e
}
)");
  const Schedule& s = ok(c);
  REQUIRE(s.warnings.size() == 2);  // a and b; e restarts on the gate's rising edge, as intended
  for (const auto& w : s.warnings) {
    CHECK(w.message.find("level-sensitive") != std::string::npos);
    CHECK(w.message.find("rise(voice.gate)") != std::string::npos);
    CHECK(w.location.line >= 3);
  }
  // Still an ordinary schedule: a warning, not an error.
  checkStructure(s, c.typed);
}

TEST_CASE("schedule: visual mode inserts a z1 into a delay-free cycle and warns") {
  ScheduleOptions so;
  so.visualMode = true;
  Compiled c;
  compile(c, kZeroDelay, so);
  const Schedule& s = ok(c);
  REQUIRE(s.warnings.size() == 1);
  CHECK(s.warnings[0].message.find("inserted a one-sample delay (z1)") != std::string::npos);
  CHECK(s.warnings[0].location.line >= 4);
  REQUIRE(s.insertedDelays.size() == 1);
  REQUIRE(s.rewritten);
  CHECK(s.rewritten->patch.nodeCount() == c.typed.patch.nodeCount() + 1);
  const NodeIndex z = s.insertedDelays[0].z1;
  CHECK(atomSignature(s.rewritten->patch.node(z).atom).name == "z1");
  checkStructure(s, *s.rewritten);
  // The loop is now ordinary feedback: one per-sample loop region with a split z1.
  uint32_t loops = 0, splitReads = 0;
  for (const Region& r : s.regions) loops += r.kind == RegionKind::Loop;
  for (const Op& op : s.ops) splitReads += op.kernel == dsp::Atom::Z1Read;
  CHECK(loops == 1);
  CHECK(splitReads == 1);
  CHECK(dump(s, c.typed).find("inserted z1 #") != std::string::npos);
  // It computes what the rewritten patch means.
  REQUIRE(sched_exec::supported(*s.rewritten));
  CHECK(sched_exec::compare(*s.rewritten, s, 64, 4, 48000.0) < 1e-5);
  // The inserted z1's state key is stable: rescheduling gives the same layout.
  Compiled c2;
  compile(c2, kZeroDelay, so);
  REQUIRE(ok(c2).state.entries.size() == 1);
  CHECK(ok(c2).state.entries[0].key == s.state.entries[0].key);
  CHECK(ok(c2).state.entries[0].offset == s.state.entries[0].offset);
}

TEST_CASE("schedule: visual mode on a param-rate cycle raises the loop to audio rate") {
  ScheduleOptions so;
  so.visualMode = true;
  Compiled c;
  compile(c, R"(patch Z {
  param p = 0.5 [0..1]
  out y : audio
  a = p + b * 0.5
  b = tanh(a)
  y = b
}
)",
          so);
  const Schedule& s = ok(c);
  REQUIRE(s.rewritten);
  checkStructure(s, *s.rewritten);
  bool loop = false;
  for (const Region& r : s.regions) loop |= r.kind == RegionKind::Loop;
  CHECK(loop);
  const TypedPatch& rw = *s.rewritten;
  for (NodeIndex n = 0; n < rw.patch.nodeCount(); ++n)
    if (rw.patch.nodeName(n) == "b") CHECK(rw.outputType(n, 0).rate == Rate::Audio);
  REQUIRE(sched_exec::supported(rw));
  CHECK(sched_exec::compare(rw, s, 16, 4, 48000.0) < 1e-5);
}

TEST_CASE("schedule: a loop whose only delay is phasor.hz is reported (no split kernel)") {
  const std::string e = errorsOf(R"(patch F {
  out y : audio
  car = sine(phasor(220Hz + car * 100))
  y = car
}
)");
  CHECK(e.find("whose only delay is input 'hz' of phasor") != std::string::npos);
  CHECK(e.find("put a z1") != std::string::npos);
  // With a z1 in the loop it is ordinary feedback: phasor stays fused, per sample.
  Compiled c;
  compile(c, R"(patch F {
  out y : audio
  car = sine(phasor(220Hz + z1(car) * 100))
  y = car
}
)");
  const Schedule& s = ok(c);
  checkStructure(s, c.typed);
  bool fusedPhasor = false;
  for (const Op& op : s.ops) fusedPhasor |= op.kernel == dsp::Atom::Phasor;
  CHECK(fusedPhasor);
  CHECK(sched_exec::compare(c.typed, s, 64, 4, 48000.0) < 1e-5);
}

TEST_CASE("schedule: feedback across the voice/global boundary is an error") {
  const std::string e = errorsOf(R"(patch X {
  out y : audio global
  g = z1(voice_sum(sine(phasor(voice.hz + g))))
  y = g
}
)");
  CHECK(e.find("crosses the voice/global boundary") != std::string::npos);
}

TEST_CASE("schedule: malformed solve blocks are reported") {
  CHECK(errorsOf(R"(patch S {
  in x : audio
  out y : audio
  solve(iterations: 2) {
    unknown a
    unknown b
    a == x
    a * 2 == x + 1
  }
  y = a + b
}
)")
            .find("unknown 'b' is not read by any equation") != std::string::npos);
  CHECK(errorsOf(R"(patch S {
  in x : audio
  out y : audio
  solve(iterations: 2) {
    unknown a
    a == z1(a) + x * 0.5 + gate_to_real(rise(a > 0))
  }
  y = a
}
)")
            .find("stateful rise") != std::string::npos);
  // phasor(a) inside the equations: its hz input is delayed, but phasor has no split kernel.
  CHECK(errorsOf(R"(patch S {
  in x : audio
  out y : audio
  solve(iterations: 2) {
    unknown a
    a == x * 0.5 + phasor(a)
  }
  y = a
}
)")
            .find("whose only delay is input 'hz' of phasor") != std::string::npos);
  // Two blocks that need each other's solution within one sample.
  CHECK(errorsOf(R"(patch S {
  in x : audio
  out y : audio
  solve(iterations: 2) {
    unknown a
    a == x - tanh(b)
  }
  solve(iterations: 2) {
    unknown b
    b == x + tanh(a)
  }
  y = a + b
}
)")
            .find("depend on each other within one sample") != std::string::npos);
}

TEST_CASE("schedule: a solve inside a feedback loop: z1 split around the Newton body") {
  Compiled c;
  compile(c, R"(patch Ladder {
  in x : audio
  out y : audio
  solve(iterations: 3) {
    unknown v = s
    v == tanh(x - v) * 0.5 + s
  }
  s = z1(v)
  y = v
}
)");
  const Schedule& s = ok(c);
  checkStructure(s, c.typed);
  REQUIRE(s.solves.size() == 1);
  const Region& r = s.regions[s.solves[0].region];
  CHECK(r.kind == RegionKind::Loop);
  CHECK(r.subBlock == 1);
  // z1:read before the solve, z1:write after it, in the same loop.
  CHECK(s.ops[r.firstOp].kernel == dsp::Atom::Z1Read);
  CHECK(s.ops[r.firstOp + r.numOps - 1].kernel == dsp::Atom::Z1Write);
  CHECK(s.solves[0].firstOp > r.firstOp);
  CHECK(s.solves[0].firstOp + s.solves[0].numOps < r.firstOp + r.numOps);
}

// ---- buffers ------------------------------------------------------------------------------------------------

TEST_CASE("schedule: a 100-node chain needs at most 3 scratch buffers") {
  for (int voice = 0; voice < 2; ++voice) {
    for (int kind = 0; kind < 3; ++kind) {
      Compiled c;
      compile(c, chainText(kind == 2 ? 34 : 100, kind, voice != 0));
      const Schedule& s = ok(c);
      checkStructure(s, c.typed);
      INFO("kind " << kind << " voice " << voice << " ops " << s.ops.size());
      CHECK(opNodes(s) >= 100);
      CHECK(s.blockBuffers() <= 3);
      if (kind == 0) CHECK(s.blockBuffers() == 1);  // element-wise ops run in place
      if (kind == 1) CHECK(s.blockBuffers() == 2);  // stateful ops ping-pong
      if (!voice) {
        REQUIRE(sched_exec::supported(c.typed));
        CHECK(sched_exec::compare(c.typed, s, 64, 2, 48000.0) < 1e-5);
      }
    }
  }
}

TEST_CASE("schedule: a value read inside a loop stays live to the loop's end") {
  // tanh(x) · 0.5 is computed before the loop and read by every sample of it: its buffer is not reused inside.
  Compiled c;
  compile(c, R"(patch L {
  in x : audio
  out y : audio
  h = tanh(x) * 0.5
  a = z1(a * 0.5 + h)
  y = a + 1
}
)");
  const Schedule& s = ok(c);
  REQUIRE(sched_exec::supported(c.typed));
  CHECK(sched_exec::compare(c.typed, s, 64, 4, 48000.0) < 1e-6);
  const Region* loop = nullptr;
  for (const Region& r : s.regions)
    if (r.kind == RegionKind::Loop) loop = &r;
  REQUIRE(loop);
  std::set<uint32_t> readFromOutside, written;
  for (uint32_t i = loop->firstOp; i < loop->firstOp + loop->numOps; ++i) {
    for (const Operand& o : s.inputs(s.ops[i]))
      if (o.kind == Operand::Kind::Buffer && !written.count(o.index)) readFromOutside.insert(o.index);
    for (const Operand& o : s.outputsOf(s.ops[i])) written.insert(o.index);
  }
  CHECK(!readFromOutside.empty());
  for (uint32_t b : readFromOutside) CHECK_FALSE(written.count(b));
}

// ---- state layout ---------------------------------------------------------------------------------------------

namespace {

const char* const kStateful = R"(patch S {
  in x : audio
  out y : audio
  a = z1(x)
  b = delay(a, 10smp, 100smp)
  c = phasor(220Hz)
  d = z1(b) * c
  y = b + c + d
}
)";

const StateEntry* entryNamed(const Schedule& s, const TypedPatch& tp, std::string_view name) {
  for (const StateEntry& e : s.state.entries)
    if (tp.patch.nodeName(e.node) == name) return &e;
  return nullptr;
}

NodeIndex named(const Patch& p, std::string_view name) {
  const auto n = p.findByName(name);
  REQUIRE(n.has_value());
  return *n;
}

// Edit: a new z1 `pre` between x and `a`.
void addPre(Patch& p) {
  const NodeIndex a = named(p, "a");
  const WireIndex w = p.adjacency().driver[p.flatInput(a, 0)];
  const PortRef from = p.wire(w).from;
  p.disconnect(w);
  const NodeIndex z = p.addAtom(atomId("z1"));
  p.setNodeName(z, "pre");
  p.connect(from, PortRef{z, 0});
  p.connect(PortRef{z, 0}, PortRef{a, 0});
}

// Edit: every reader of the delay line `b` reads `a` instead (b becomes dead and is removed by optimise()).
void dropDelay(Patch& p) {
  const NodeIndex a = named(p, "a"), b = named(p, "b");
  for (;;) {
    WireIndex w = kNone;
    for (WireIndex i = 0; i < p.wireCount(); ++i)
      if (p.wire(i).from.node == b) w = i;
    if (w == kNone) break;
    const PortRef to = p.wire(w).to;
    p.disconnect(w);
    p.connect(PortRef{a, 0}, to);
  }
}

}  // namespace

TEST_CASE("schedule: the state layout is identical across reschedules of the same patch") {
  Compiled a, b;
  compile(a, kStateful);
  compile(b, kStateful);
  const auto& ea = ok(a).state.entries;
  const auto& eb = ok(b).state.entries;
  REQUIRE(ea.size() == 4);
  REQUIRE(ea.size() == eb.size());
  for (size_t i = 0; i < ea.size(); ++i) {
    CHECK(ea[i].key == eb[i].key);
    CHECK(ea[i].offset == eb[i].offset);
    CHECK(ea[i].size == eb[i].size);
    CHECK(ea[i].tag == eb[i].tag);
  }
  // Rescheduling with the previous layout keeps it bit for bit.
  ScheduleOptions so;
  so.previous = &ok(a).state;
  Compiled c;
  compile(c, kStateful, so);
  CHECK(ok(c).state.globalBytes == ok(a).state.globalBytes);
  for (const StateEntry& e : ok(c).state.entries) {
    const StateEntry* pe = ok(a).state.find(e.key, e.channel, e.arena);
    REQUIRE(pe);
    CHECK(pe->offset == e.offset);
  }
}

TEST_CASE("schedule: unchanged nodes keep their state slots when other nodes are added or removed") {
  Compiled base;
  compile(base, kStateful);
  const Schedule& s0 = ok(base);
  ScheduleOptions so;
  so.previous = &s0.state;

  // A new z1 between x and `a`: every old entry keeps its offset; the new one goes after them.
  Compiled added;
  compileWith(added, kStateful, so, addPre);
  const Schedule& s1 = ok(added);
  checkStructure(s1, added.typed);
  for (const char* n : {"a", "b", "c"}) {
    INFO(n);
    const StateEntry* before = entryNamed(s0, base.typed, n);
    const StateEntry* after = entryNamed(s1, added.typed, n);
    REQUIRE(before);
    REQUIRE(after);
    CHECK(before->key == after->key);
    CHECK(before->offset == after->offset);
    CHECK(before->size == after->size);
    CHECK(before->tag == after->tag);
  }
  const StateEntry* pre = entryNamed(s1, added.typed, "pre");
  REQUIRE(pre);
  for (const StateEntry& e : s0.state.entries) CHECK(pre->offset >= e.offset + e.size);

  // Remove `a` (its input feeds b directly) and add a new z1 `fresh`: b and c keep their offsets; `fresh` (same
  // size as a) fills the hole `a` left, so the arena does not grow.
  ScheduleOptions so2;
  so2.previous = &s1.state;
  Compiled removed;
  compileWith(removed, kStateful, so2, [](Patch& p) {
    addPre(p);  // the same edit as above first, so the NodeIds match the previous compile's
    const NodeIndex a = named(p, "a");
    const NodeIndex b = named(p, "b");
    const NodeIndex y = named(p, "y");
    const PortRef from = p.wire(p.adjacency().driver[p.flatInput(a, 0)]).from;
    p.disconnect(p.adjacency().driver[p.flatInput(b, 0)]);
    p.connect(from, PortRef{b, 0});
    const NodeIndex z = p.addAtom(atomId("z1"));
    p.setNodeName(z, "fresh");
    p.connect(from, PortRef{z, 0});
    const WireIndex yw = p.adjacency().driver[p.flatInput(y, 0)];
    const PortRef yIn = p.wire(yw).from;
    p.disconnect(yw);
    const NodeIndex add = p.addAtom(atomId("add"));
    p.connect(yIn, PortRef{add, 0});
    p.connect(PortRef{z, 0}, PortRef{add, 1});
    p.connect(PortRef{add, 0}, PortRef{y, 0});
    const NodeIndex gone[] = {a};
    p.eraseNodes(gone);
  });
  const Schedule& s2 = ok(removed);
  checkStructure(s2, removed.typed);
  for (const char* n : {"b", "c"}) {
    INFO(n);
    const StateEntry* before = entryNamed(s1, added.typed, n);
    const StateEntry* after = entryNamed(s2, removed.typed, n);
    REQUIRE(before);
    REQUIRE(after);
    CHECK(before->offset == after->offset);
  }
  const StateEntry* fresh = entryNamed(s2, removed.typed, "fresh");
  REQUIRE(fresh);
  CHECK(fresh->offset == entryNamed(s1, added.typed, "a")->offset);
  CHECK(s2.state.globalBytes == s1.state.globalBytes);
  CHECK_FALSE(s2.state.compacted);
}

TEST_CASE("schedule: a changed type tag starts fresh state; holes that dominate compact the arena") {
  Compiled base;
  compile(base, kStateful);
  ScheduleOptions so;
  so.previous = &ok(base).state;
  // A longer delay line: a different ring size → a different tag and size → a new slot.
  std::string longer = kStateful;
  longer.replace(longer.find("100smp"), 6, "900smp");
  Compiled c;
  compile(c, longer, so);
  const StateEntry* b0 = entryNamed(ok(base), base.typed, "b");
  const StateEntry* b1 = entryNamed(ok(c), c.typed, "b");
  REQUIRE(b0);
  REQUIRE(b1);
  CHECK_FALSE(b0->tag == b1->tag);
  CHECK(b1->offset != b0->offset);
  // `a` (unchanged) keeps its slot.
  CHECK(entryNamed(ok(base), base.typed, "a")->offset == entryNamed(ok(c), c.typed, "a")->offset);

  // The previous layout had a huge delay that is now gone: its hole would dominate, so the layout is compacted.
  std::string huge = kStateful;
  huge.replace(huge.find("100smp"), 6, "100000smp");
  Compiled h;
  compile(h, huge);
  ScheduleOptions so2;
  so2.previous = &ok(h).state;
  Compiled small;
  compileWith(small, huge, so2, dropDelay);
  REQUIRE(ok(small).state.entries.size() == 3);
  CHECK(ok(small).state.compacted);
  CHECK(ok(small).state.globalBytes <= 192);
  // A small hole is kept (no compaction): a, c and d's z1 keep their offsets.
  ScheduleOptions so3;
  so3.previous = &ok(base).state;
  Compiled dropped;
  compileWith(dropped, kStateful, so3, dropDelay);
  CHECK_FALSE(ok(dropped).state.compacted);
  for (const char* n : {"a", "c"})
    CHECK(entryNamed(ok(base), base.typed, n)->offset == entryNamed(ok(dropped), dropped.typed, n)->offset);
}

// ---- numerical equivalence ------------------------------------------------------------------------------------

TEST_CASE("schedule: block-wise execution with shared buffers matches a naive per-sample run") {
  const char* cases[] = {kChainA, kFeedback, kCombs, kLoops,
                         R"(patch Svf {
  in x : audio
  param cutoff = 1000 [20..20000]
  param res = 0.5 [0..1]
  out lp : audio
  g = tan(3.14159 * cutoff / sr)
  k = 2 - 2 * res
  a1 = 1 / (1 + g * (g + k))
  a2 = g * a1
  a3 = g * a2
  v3 = x - ic2
  v1 = a1 * ic1 + a2 * v3
  v2 = ic2 + a2 * ic1 + a3 * v3
  ic1 = z1(2 * v1 - ic1)
  ic2 = z1(2 * v2 - ic2)
  lp = v2
}
)",
                         R"(patch Logic {
  in x : audio
  out y : audio
  g = x > 0.1
  r = rise(g)
  c = schmitt(x, -0.2, 0.3)
  h = samplehold(x, r)
  t = timer(r, 3smp)
  y = h + gate_to_real(c && t) + mux(1, x, h * 2, x * 3) + phasor(100Hz, r)
}
)"};
  for (const char* src : cases) {
    Compiled c;
    compile(c, src);
    const Schedule& s = ok(c);
    REQUIRE(sched_exec::supported(c.typed));
    for (uint32_t frames : {64u, 37u, 1u}) {
      INFO(src << " frames " << frames);
      CHECK(sched_exec::compare(c.typed, s, frames, frames == 1 ? 200 : 8, 48000.0) < 1e-5);
    }
  }
}

TEST_CASE("schedule: random feedback patches: structure and numerical equivalence" * doctest::test_suite("slow")) {
  std::mt19937 rng(0x7155u);
  auto pick = [&](uint32_t n) { return static_cast<uint32_t>(rng() % n); };
  uint32_t compared = 0;
  for (int trial = 0; trial < 120; ++trial) {
    const uint32_t count = 6 + pick(30);
    std::vector<std::string> names = {"x", "x2", "p"};
    std::string s = "patch R {\n  in x : audio\n  in x2 : audio\n  param p = 0.3 [0..1]\n  out y : audio\n";
    for (uint32_t i = 0; i < count; ++i) {
      const std::string n = "n" + std::to_string(i);
      auto any = [&] { return names[pick(static_cast<uint32_t>(names.size()))]; };
      auto later = [&] { return "n" + std::to_string(pick(count)); };  // may be a later name: feedback
      std::string e;
      switch (pick(10)) {
        case 0: e = "tanh(" + any() + " * 0.7)"; break;
        case 1: e = any() + " + " + any(); break;
        case 2: e = any() + " * p"; break;
        case 3: e = "min(" + any() + ", " + any() + ")"; break;
        case 4: e = "max(" + any() + ", " + any() + ") - 0.1"; break;
        case 5: e = "z1(tanh(" + later() + "))"; break;
        case 6:
          e = "delay(" + any() + " + tanh(" + later() + ") * 0.4, " + std::to_string(1 + pick(40)) + "smp, 64smp)";
          break;
        case 7:
          e = "delay_cubic(" + any() + " - tanh(" + later() + ") * 0.3, " + std::to_string(2 + pick(30)) +
              ".5smp, 64smp)";
          break;
        case 8: e = "clip(" + any() + " * " + any() + ", -1, 1)"; break;
        default: e = "z1(" + any() + ") * 0.5 + " + any(); break;
      }
      s += "  " + n + " = " + e + "\n";
      names.push_back(n);
    }
    s += "  y = tanh(n" + std::to_string(count - 1) + " + n" + std::to_string(pick(count)) + ")\n}\n";
    Compiled c;
    compile(c, s);
    INFO(s);
    const Schedule& sch = ok(c);
    checkStructure(sch, c.typed);
    if (!sched_exec::supported(c.typed)) continue;
    const uint32_t frames = trial % 3 == 0 ? 37u : 64u;
    CHECK(sched_exec::compare(c.typed, sch, frames, 6, 48000.0) < 1e-5);
    ++compared;
  }
  CHECK(compared >= 100);
}

// ---- buffer liveness: the schedule-replay checker (T1.5-fix1, M1 review C1/C2/H2) ----------------------------

namespace {

// Every patch of every .pqp file under the test corpora and stdlib that infers and optimises at `sr` (imports
// relative to the file). Root `in`s with a default are retried unconnected, as the engine may leave them.
struct CorpusPatch {
  std::string where;
  TypedPatch typed;
};

std::vector<CorpusPatch> corpus(double sr) {
  namespace fs = std::filesystem;
  const fs::path data(PQ_TEST_DATA_DIR);
  std::vector<fs::path> files;
  for (const fs::path& dir : {data / "graph" / "cases", data / "engine" / "patches", data / "jit" / "patches",
                              data / "golden" / "patches", text_util::stdlibDir()})
    for (const fs::path& p : text_util::pqpFiles(dir)) files.push_back(p);
  std::vector<CorpusPatch> out;
  for (const fs::path& path : files) {
    text_util::DirLoader loader(path.parent_path());
    const std::string text = text_util::readFile(path);
    ParseOptions po;
    po.imports = &loader;
    const ParseResult parsed = parseDetailed(text, path.string(), po);
    if (!parsed.ok()) continue;
    std::vector<std::string> itexts;
    std::vector<ParseResult> imported;
    std::vector<fs::path> pending;
    for (const std::string& imp : parsed.file.imports) pending.push_back(path.parent_path() / imp);
    while (!pending.empty() && imported.size() < 64) {
      const fs::path ip = pending.back();
      pending.pop_back();
      itexts.push_back(text_util::readFile(ip));
      ParseOptions ipo;
      ipo.imports = &loader;
      imported.push_back(parseDetailed(itexts.back(), ip.string(), ipo));
      for (const std::string& imp : imported.back().file.imports) pending.push_back(ip.parent_path() / imp);
    }
    PatchLibrary lib;
    lib.add(parsed.file);
    for (const ParseResult& ip : imported)
      if (ip.ok()) lib.add(ip.file);
    for (const Patch& patch : parsed.file.patches) {
      InferOptions io;
      io.library = &lib;
      InferResult inf = inferDetailed(patch, io);
      if (!inf.ok()) {
        std::vector<RootInput> roots;
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
      if (!inf.ok()) continue;
      OptimiseOptions oo;
      oo.sampleRate = sr;
      auto opt = optimise(std::move(inf.typed), oo);
      if (!opt.ok()) continue;
      out.push_back({path.filename().string() + ":" + patch.name(), std::move(opt.value())});
    }
  }
  return out;
}

TypedPatch typedOf(std::string_view src) {
  Compiled c;
  compile(c, src);
  return std::move(c.typed);
}

std::string replay(const TypedPatch& tp, sched_replay::Coverage* cov = nullptr) {
  ScheduleOptions so;
  so.sampleRate = 48000.0;
  return sched_replay::check(tp, so, {}, cov);
}

// A random patch with global and voice stages, reducers and broadcasts, param-rate values, stereo signals, feedback
// through z1 and (multichannel) delay lines, solve blocks with one or two unknowns (global, per voice and param rate)
// and several outputs of each kind, two of them reading the same value (the C2 shape).
std::string randomPatch(std::mt19937& rng) {
  auto pick = [&](uint32_t n) { return static_cast<uint32_t>(rng() % n); };
  enum Cat { G, V, GS, VS, C, kCats };
  std::vector<std::string> pool[kCats] = {{"x", "g0"}, {"v0"}, {"xs", "gs0"}, {"vs0"}, {"p", "c0"}};
  std::string s =
      "patch R {\n  in x : audio\n  in xs : audio stereo\n  param p = 0.3 [0..1]\n"
      "  out g1 : audio\n  out g2 : audio\n  out s1 : audio stereo\n  out o1 : audio voice\n  out o2 : audio voice\n"
      "  out vs1 : audio voice stereo\n  out c1 : real\n"
      "  g0 = tanh(x * 0.5)\n  v0 = sine(phasor(voice.hz))\n  gs0 = xs * 0.5\n  vs0 = (v0, v0 * 0.5 + 0.1)\n"
      "  c0 = p * 2 + 0.1\n";
  const uint32_t count = 6 + pick(26);
  std::vector<Cat> cat(count);
  for (auto& c : cat) c = static_cast<Cat>(pick(kCats));
  auto any = [&](Cat c) { return pool[c][pick(static_cast<uint32_t>(pool[c].size()))]; };
  auto later = [&](Cat c, uint32_t self) {  // a node of the same category, maybe not yet defined: feedback
    std::vector<uint32_t> same;
    for (uint32_t j = 0; j < count; ++j)
      if (cat[j] == c && j >= self) same.push_back(j);
    return "n" + std::to_string(same[pick(static_cast<uint32_t>(same.size()))]);
  };
  auto smp = [&](uint32_t lo, uint32_t n) { return std::to_string(lo + pick(n)) + "smp"; };
  uint32_t solves = 0;
  auto solve = [&](Cat c, const std::string& a, const std::string& b) {
    const std::string u = "u" + std::to_string(solves), w = "w" + std::to_string(solves);
    ++solves;
    std::string t = "  solve(iterations: " + std::to_string(1 + pick(6)) + ") {\n";
    const bool init = pick(2) == 0;
    if (pick(2) == 0) {
      t += "    unknown " + u + (init ? " = " + a : std::string()) + "\n";
      switch (pick(3)) {
        case 0: t += "    " + u + " * " + u + " * 0.25 + " + u + " == " + a + " - " + u + " * 0.5\n"; break;
        case 1: t += "    " + u + " == " + a + " - tanh(" + u + ") * 0.3\n"; break;
        default: t += "    " + u + " * " + u + " == " + a + " * " + a + " + 1 - " + u + " * " + u + "\n"; break;
      }
      pool[c].push_back(u);
    } else {
      t += "    unknown " + u + (init ? " = 1" : std::string()) + "\n    unknown " + w + "\n";
      if (pick(2) == 0) t += "    " + u + " * 2 == " + w + " + " + a + "\n    " + u + " + " + w + " == " + b + "\n";
      else t += "    " + u + " * " + u + " * 0.1 + " + w + " == " + a + "\n    " + w + " * 2 == " + u + " - " + b + "\n";
      pool[c].push_back(u);
      pool[c].push_back(w);
    }
    return t + "  }\n";
  };
  for (uint32_t i = 0; i < count; ++i) {
    const std::string n = "n" + std::to_string(i);
    const Cat c = cat[i];
    std::string e;
    const uint32_t k = pick(8);
    switch (c) {
      case G:
        if (k == 0) e = "tanh(" + any(G) + " * 0.7)";
        else if (k == 1) e = any(G) + " + " + any(G) + " * " + any(C);
        else if (k == 2) e = "voice_sum(" + any(V) + ")";
        else if (k == 3) e = "z1(tanh(" + later(G, i) + ")) * 0.5 + " + any(G);
        else if (k == 4) e = "delay(" + any(G) + " + tanh(" + later(G, i) + ") * 0.4, " + smp(1, 40) + ", 64smp)";
        else if (k == 5) e = "min(" + any(G) + ", " + any(G) + ") - 0.1";
        else if (k == 6) e = any(GS) + "[" + std::to_string(pick(2)) + "] * 0.5";
        else s += solve(G, any(G), any(G)), e = any(G) + " * 0.5";
        break;
      case V:
        if (k == 0) e = "sine(phasor(voice.hz * " + std::to_string(1 + pick(3)) + "))";
        else if (k == 1) e = any(V) + " * " + any(G);
        else if (k == 2) e = any(V) + " + " + any(V);
        else if (k == 3) e = "tanh(" + any(V) + " * " + any(C) + ")";
        else if (k == 4) e = "z1(tanh(" + later(V, i) + ")) * 0.5 + " + any(V);
        else if (k == 5) e = "delay(" + any(V) + " + tanh(" + later(V, i) + ") * 0.4, " + smp(1, 20) + ", 32smp)";
        else if (k == 6) e = any(VS) + "[" + std::to_string(pick(2)) + "]";
        else s += solve(V, any(V), any(V)), e = any(V) + " * 0.5";
        break;
      case GS:
        if (k < 2) e = "(" + any(G) + ", " + any(G) + ")";
        else if (k < 4) e = any(GS) + " * 0.5 + " + any(G);
        else if (k < 6) e = any(GS) + " + delay(" + n + " * 0.5, " + smp(1, 30) + ", 64smp)";
        else e = "tanh(" + any(GS) + " * " + any(GS) + ")";
        break;
      case VS:
        if (k < 2) e = "(" + any(V) + ", " + any(V) + ")";
        else if (k < 4) e = any(VS) + " * " + any(G);
        else if (k < 6) e = any(VS) + " + delay(" + n + " * 0.25, " + smp(1, 16) + ", 32smp)";
        else e = any(VS) + " * " + any(VS);
        break;
      default:
        if (k < 3) e = any(C) + " * 0.5 + 0.1";
        else if (k < 6) e = "tanh(" + any(C) + ") + " + any(C);
        else s += solve(C, any(C), any(C)), e = any(C) + " + 0.25";
        break;
    }
    s += "  " + n + " = " + e + "\n";
    pool[c].push_back(n);
  }
  const std::string o1 = any(V);
  s += "  g1 = " + any(G) + "\n  g2 = " + any(G) + " * 0.5\n  s1 = " + any(GS) + "\n  o1 = " + o1 + "\n  o2 = " +
       (pick(2) ? o1 : any(V)) + " * 0.5\n  vs1 = " + any(VS) + "\n  c1 = " + any(C) + "\n}\n";
  return s;
}

}  // namespace

TEST_CASE("schedule: the replay checker catches a hand-made aliasing bug and accepts the unshared schedule") {
  const TypedPatch tp = typedOf(R"(patch VoiceOut {
  out o1 : audio voice
  out o2 : audio voice
  a = sine(phasor(voice.hz))
  o1 = a
  o2 = a * 0.5
}
)");
  ScheduleOptions so;
  so.shareBuffers = false;
  const ScheduleResult unique = scheduleDetailed(tp, so);
  REQUIRE(unique.ok());
  CHECK(sched_replay::Replay(unique.schedule, unique.schedule, {}).run().empty());
  so.shareBuffers = true;
  const ScheduleResult shared = scheduleDetailed(tp, so);
  REQUIRE(shared.ok());
  CHECK(sched_replay::Replay(shared.schedule, unique.schedule, {}).run().empty());
  // The pre-fix schedule: the mul writes o2 over o1's buffer in place.
  Schedule bad = shared.schedule;
  const Op& mul = bad.ops.back();
  REQUIRE(mul.kernel == dsp::Atom::Mul);
  const Operand from = bad.operands[mul.firstOperand + mul.numIn], to = bad.operands[mul.firstOperand];
  REQUIRE(from.kind == Operand::Kind::Buffer);
  REQUIRE(to.kind == Operand::Kind::Buffer);
  for (Operand& o : bad.operands)
    if (o == from) o = to;
  for (OutputRef& o : bad.outputs)
    if (o.value == from) o.value = to;
  const std::string e = sched_replay::Replay(bad, unique.schedule, {}).run();
  INFO(e);
  CHECK(e.find("the engine reading output 0 at the end of stage 1") != std::string::npos);
}

TEST_CASE("schedule: M1 review probes: solve operands, unknowns and voice outputs outlive the last op") {
  // C1: u·u is both equation sides' operand; the solver reads lhs and rhs after the body.
  {
    const TypedPatch tp = typedOf(R"(patch S {
  out y : audio
  x = noise() * 0.0001 + 4
  solve(iterations: 8) {
    unknown u = 1
    u * u == x - u * u
  }
  y = u
}
)");
    CHECK(replay(tp) == "");
    const Schedule s = schedule(tp).value();
    REQUIRE(s.equations.size() == 1);
    CHECK_FALSE(s.equations[0].lhs == s.equations[0].rhs);
  }
  // C1, two unknowns: no body op writes into an unknown's buffer.
  {
    const TypedPatch tp = typedOf(R"(patch S {
  out z : audio
  x = noise() * 0.0001 + 4
  solve(iterations: 8) {
    unknown a = 1
    unknown b = 1
    a * 2 == b
    a + b == x
  }
  z = b
}
)");
    CHECK(replay(tp) == "");
    const Schedule s = schedule(tp).value();
    for (const SolveUnknown& u : s.unknowns)
      for (uint32_t i = s.solves[0].firstOp; i < s.solves[0].firstOp + s.solves[0].numOps; ++i)
        for (const Operand& o : s.outputsOf(s.ops[i])) CHECK_FALSE(o == u.value);
  }
  // C2: two voice outputs from one value.
  {
    const TypedPatch tp = typedOf(R"(patch V {
  out o1 : audio voice
  out o2 : audio voice
  a = sine(phasor(voice.hz))
  o1 = a
  o2 = a * 0.5
}
)");
    CHECK(replay(tp) == "");
    const Schedule s = schedule(tp).value();
    REQUIRE(s.outputs.size() == 2);
    CHECK_FALSE(s.outputs[0].value == s.outputs[1].value);
  }
}

TEST_CASE("schedule: a multichannel delay line in a feedback loop reads the same inputs on every channel (H2)") {
  Compiled c;
  compile(c, R"(patch St {
  out y : audio stereo
  src = (noise() * 0.1, noise(seed: 3) * 0.1)
  fb = src + delay(fb * 0.5, 2ms, 1s)
  y = fb
}
)");
  const Schedule& s = ok(c);
  checkStructure(s, c.typed);
  CHECK(replay(c.typed) == "");
  std::vector<std::vector<Operand>> reads;
  uint32_t fills = 0;
  for (const Op& op : s.ops) {
    if (op.kernel == dsp::Atom::DelayReadLinear) reads.emplace_back(s.inputs(op).begin(), s.inputs(op).end());
    fills += op.kernel == dsp::Atom::Const;
  }
  REQUIRE(reads.size() == 2);
  CHECK(reads[0].size() == 1);
  CHECK(reads[1] == reads[0]);
  CHECK(fills == 0);
}

TEST_CASE("schedule: loop sub-blocks are bounded with the runtime's float32 delay (M10)") {
  // 20.75 ms · 48 kHz = 996 in double but float(0.02075) · 48000 = 995.99994: the newest tap is 995 samples old.
  Compiled c;
  compile(c, R"(patch D {
  param t = 20.75ms [20.75ms..1s]
  out y : audio
  src = noise() * 0.1
  fb = src + delay(fb * 0.5, t, 1s)
  y = fb
}
)");
  const Schedule& s = ok(c);
  uint32_t sub = 0;
  for (const Region& r : s.regions)
    if (r.kind == RegionKind::Loop) sub = r.subBlock;
  CHECK(static_cast<float>(0.02075) * 48000.0f < 996.0f);
  CHECK(sub == 995);
  // Exact float values keep their full sub-block: 10smp → 10, 1 ms → 48 (float(0.001) · 48000 = 48.0000023).
  CHECK(dumpOf(kFeedback).find("loop, sub-block 48") != std::string::npos);
  Compiled l;
  compile(l, kLoops);
  std::vector<uint32_t> subs;
  for (const Region& r : ok(l).regions)
    if (r.kind == RegionKind::Loop) subs.push_back(r.subBlock);
  std::sort(subs.begin(), subs.end());
  CHECK(subs == std::vector<uint32_t>{1, 4, 10});
}

TEST_CASE("schedule: z1s on cycles that all pass through a delay line stay unsplit, so the loop keeps its sub-blocks (T1b.3)") {
  // stdlib comb_lp's shape: a linear-phase FIR of z1 taps in a comb's feedback. Every cycle passes through the
  // delay line, whose lower bound is 5 ms − 1/sr (x − literal bounds) = 239 samples.
  auto loops = [](const Schedule& s) {
    std::vector<uint32_t> subs;
    for (const Region& r : s.regions)
      if (r.kind == RegionKind::Loop) subs.push_back(r.subBlock);
    return subs;
  };
  auto kernels = [](const Schedule& s, pq::dsp::Atom k) {
    uint32_t n = 0;
    for (const Op& op : s.ops) n += op.kind == OpKind::Kernel && op.kernel == k;
    return n;
  };
  Compiled fir;
  compile(fir, R"(patch K {
  in    x : audio
  param t = 5ms [5ms..40ms]
  out   o : audio
  y1 = z1(y)
  w  = 0.25 * (y + z1(y1)) + 0.5 * y1
  y  = x + 0.9 * delay(w, t - 1 / sr, 50ms)
  o  = y
}
)");
  CHECK(loops(ok(fir)) == std::vector<uint32_t>{239});
  CHECK(kernels(ok(fir), pq::dsp::Atom::Z1) == 2);  // fused z1s inside the loop
  CHECK(kernels(ok(fir), pq::dsp::Atom::Z1Read) == 0);
  CHECK(kernels(ok(fir), pq::dsp::Atom::DelayReadLinear) == 1);
  // A z1 on a cycle without the delay line (a one-pole in the feedback) still needs the per-sample loop, and then
  // every z1 of the SCC is split as before.
  Compiled iir;
  compile(iir, R"(patch K {
  in    x : audio
  param t = 5ms [5ms..40ms]
  out   o : audio
  d  = delay(y + z1(y), t, 50ms)
  lp = z1(lp + (d - lp) * 0.3)
  y  = x + 0.9 * lp
  o  = y
}
)");
  CHECK(loops(ok(iir)) == std::vector<uint32_t>{1});
  CHECK(kernels(ok(iir), pq::dsp::Atom::Z1) == 0);
  CHECK(kernels(ok(iir), pq::dsp::Atom::Z1Read) == 2);
}

TEST_CASE("schedule: replay checker over the corpus: no buffer is overwritten while live") {
  sched_replay::Coverage cov;
  uint32_t checked = 0, withVoices = 0, withSolves = 0, withLoops = 0, multiOut = 0;
  for (const CorpusPatch& cp : corpus(48000.0)) {
    ScheduleOptions so;
    so.sampleRate = 48000.0;
    const ScheduleResult r = scheduleDetailed(cp.typed, so);
    if (!r.ok()) continue;
    INFO(cp.where);
    CHECK(replay(cp.typed, &cov) == "");
    ++checked;
    bool voice = false, loop = false;
    for (const Stage& sg : r.schedule.stages) voice |= sg.poly == Poly::Voice && sg.numOps > 0;
    for (const Region& rg : r.schedule.regions) loop |= rg.kind == RegionKind::Loop;
    withVoices += voice;
    withLoops += loop;
    withSolves += !r.schedule.solves.empty();
    multiOut += r.schedule.outputs.size() > 1;
  }
  MESSAGE("replayed " << checked << " corpus patches (" << withVoices << " with voice stages, " << withSolves
                      << " with solves, " << withLoops << " with loops, " << multiOut << " with several outputs): "
                      << cov.reads << " reads, " << cov.bodyEvaluations << " Newton body evaluations, "
                      << cov.solveReads << " solver reads, " << cov.outputReads << " engine output reads");
  CHECK(checked >= 40);
  CHECK(withVoices >= 8);
  CHECK(withSolves >= 6);
}

TEST_CASE("schedule: replay checker over 500 random patches with voices, outputs, stereo delays and solves") {
  std::mt19937 rng(0x5EEDu);
  sched_replay::Coverage cov;
  uint32_t checked = 0, attempts = 0, withSolves = 0, withVoiceSolve = 0, stereoLoops = 0, voiceOutsShared = 0;
  while (checked < 500 && attempts < 2000) {
    ++attempts;
    const std::string src = randomPatch(rng);
    const ParseResult parsed = parseDetailed(src, "r.pqp");
    REQUIRE_MESSAGE(parsed.ok(), src);
    InferResult inf = inferDetailed(parsed.file.patches[0]);
    if (!inf.ok()) continue;  // e.g. a voice value fed back into a global one
    OptimiseOptions oo;
    oo.sampleRate = 48000.0;
    auto opt = optimise(std::move(inf.typed), oo);
    REQUIRE(opt.ok());
    ScheduleOptions so;
    so.sampleRate = 48000.0;
    const ScheduleResult r = scheduleDetailed(opt.value(), so);
    if (!r.ok()) continue;  // feedback across the voice/global boundary, solve blocks depending on each other …
    INFO(src);
    checkStructure(r.schedule, opt.value());
    CHECK(replay(opt.value(), &cov) == "");
    ++checked;
    const Schedule& s = r.schedule;
    withSolves += !s.solves.empty();
    for (const SolveBlock& b : s.solves) withVoiceSolve += s.regions[b.region].poly == Poly::Voice;
    for (const Op& op : s.ops) stereoLoops += op.kernel == dsp::Atom::DelayReadLinear && op.channel == 1;
    for (size_t i = 0; i < s.outputs.size(); ++i)
      for (size_t j = 0; j < i; ++j)
        voiceOutsShared += s.outputs[i].poly == Poly::Voice && s.outputs[j].poly == Poly::Voice &&
                           s.outputs[i].node != s.outputs[j].node;
  }
  MESSAGE("replayed " << checked << " random patches (" << attempts << " generated; " << withSolves
                      << " with solves, " << withVoiceSolve << " voice solve blocks, " << stereoLoops
                      << " channel-1 delay reads in loops): " << cov.reads << " reads, " << cov.bodyEvaluations
                      << " Newton body evaluations, " << cov.solveReads << " solver reads, " << cov.outputReads
                      << " engine output reads");
  CHECK(checked == 500);
  CHECK(withSolves >= 100);
  CHECK(withVoiceSolve >= 50);
  CHECK(stereoLoops >= 50);
  CHECK(voiceOutsShared >= 100);
}

TEST_CASE("schedule: a delay line whose ring would pass 1 GiB is a located compile error (M1 review H3)") {
  // DelayBig from docs/reviews/M1/probes/dsp_probes.pqp: max 12000 s = 5.76e8 frames. Its ring size used to wrap
  // to 0 bytes in uint32 and the kernel wrote out of bounds; it must be rejected, naming the node and its line.
  const std::string big = errorsOf(R"(patch DelayBig {
  out y : audio
  y = delay(sine_hz(100Hz), 1smp, 12000)
}
)");
  CHECK(big.find("test.pqp:3:") != std::string::npos);
  CHECK(big.find("maximum delay") != std::string::npos);
  CHECK(big.find("mono delay line") != std::string::npos);
  // Near the limit the patch schedules and its state entry covers the whole ring (2^28 frames · 4 B); one frame more
  // is an error. The same for a voice delay (8 lanes per frame: 2^25 frames).
  const auto at = [](uint32_t frames, bool voice) {
    const std::string f = std::to_string(frames);
    return voice ? "patch V {\n  out y : audio voice\n  y = delay(sine(phasor(voice.hz)), 1smp, " + f + "smp)\n}\n"
                 : "patch M {\n  out y : audio\n  y = delay(sine_hz(100Hz), 1smp, " + f + "smp)\n}\n";
  };
  for (bool voice : {false, true}) {
    const uint32_t limit = dsp::maxDelayFramesLimit(voice ? 8 : 1);
    CHECK(limit == (voice ? (1u << 25) : (1u << 28)) - 10u);
    Compiled c;
    compile(c, at(limit - 8, voice));  // just below: the language's float literal and seconds round trip may add one
    const Schedule& s = ok(c);
    uint64_t ring = 0;
    for (const StateEntry& e : s.state.entries)
      if (e.tag.kernel == static_cast<uint16_t>(dsp::Atom::DelayLinear)) ring = e.size;
    CHECK(ring >= uint64_t{1} << 30);
    CHECK(ring <= (uint64_t{1} << 30) + sizeof(dsp::DelayState));
    CHECK((voice ? s.state.voiceBytes : s.state.globalBytes) >= ring);
    const std::string e = errorsOf(at(limit + 1, voice));
    CHECK(e.find("test.pqp:3:") != std::string::npos);
    CHECK(e.find(voice ? "voice delay line" : "mono delay line") != std::string::npos);
  }
  // Two maximal rings exceed the 2 GiB arena: an error at the second one, not wrapped offsets.
  const std::string lim = std::to_string(dsp::maxDelayFramesLimit(1) - 8);
  const std::string two = errorsOf("patch Two {\n  out y : audio\n  a = delay(sine_hz(100Hz), 1smp, " + lim +
                                   "smp)\n  b = delay(a, 1smp, " + lim + "smp)\n  y = b\n}\n");
  CHECK(two.find("state exceeds 2 GiB") != std::string::npos);
  CHECK((two.find("test.pqp:3:") != std::string::npos || two.find("test.pqp:4:") != std::string::npos));
}

TEST_CASE("schedule: invalid options and inputs") {
  Compiled c;
  compile(c, kChainA);
  ScheduleOptions so;
  so.sampleRate = 0.0;
  CHECK_FALSE(scheduleDetailed(c.typed, so).ok());
  // Durations still in seconds (optimise() not run).
  const std::string src = "patch T {\n  in x : audio\n  out y : audio\n  y = delay(x, 1ms, 10ms)\n}\n";
  const ParseResult parsed = parseDetailed(src, "t.pqp");
  REQUIRE(parsed.ok());
  InferResult inf = inferDetailed(parsed.file.patches[0]);
  REQUIRE(inf.ok());
  const ScheduleResult r = scheduleDetailed(inf.typed);
  REQUIRE_FALSE(r.ok());
  CHECK(r.diagnostics[0].message.find("run optimise() before schedule()") != std::string::npos);
}
