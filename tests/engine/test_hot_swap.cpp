// T1.9: hot swap — publishing images from another thread, swap at a block boundary, state transfer by state key
// + type tag, crossfade when audible / instant when silent, voices across a swap, epoch reclamation of old images.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "interp_harness.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/note_events.h"
#include "pq/engine/program_host.h"
#include "pq/engine/state_transfer.h"

using namespace interp_test;
namespace graph = pq::graph;
namespace dsp = pq::dsp;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 64;

// ---- compiling with the previous layout ------------------------------------------------------------------------
// Edits are made on the Patch IR, as the editor and the project lowering (T3.17) make them: nodes keep their
// NodeIds, so their state keys survive. (A plain re-parse of edited TEXT renumbers nodes: declarations get ids before
// bindings, so adding an `export` line would shift every later node's id and thus its state key; parsing with
// ParseOptions::previous keeps them, tests/engine/test_text_edits.cpp.)
Patch parsePatch(const std::string& text) {
  ParseResult pr = parseDetailed(text, "swap.pqp");
  REQUIRE_MESSAGE(pr.ok(), (pr.ok() ? "" : formatDiagnostic(text, pr.diagnostics.front())));
  return pr.file.patches[0];
}

// The edit "add an unrelated node": export spare = sine(phasor(hz)) (global), or sine(phasor(voice.hz · ratio)).
Patch withSpare(Patch p, double hzOrRatio, bool voice) {
  const NodeIndex ph = p.addAtom(atomId("phasor"));
  const NodeIndex k = p.addConst(Constant::real(hzOrRatio));
  if (voice) {
    const NodeIndex vh = p.addAtom(atomId("voice.hz"));
    const NodeIndex m = p.addAtom(atomId("mul"));
    p.connect(vh, m, 0);
    p.connect(k, m, 1);
    p.connect(m, ph, 0);
  } else {
    p.connect(k, ph, 0);
  }
  const NodeIndex s = p.addAtom(atomId("sine"));
  p.connect(ph, s, 0);
  const NodeIndex ex = p.addExport("spare");
  p.connect(s, ex, 0);
  REQUIRE(p.validate().ok());
  return p;
}

std::unique_ptr<Compiled> compilePatch(const Patch& patch, const StateLayout* previous) {
  InferOptions io;
  auto inf = infer(patch, io);
  REQUIRE_MESSAGE(inf.ok(), (inf.ok() ? "" : inf.error().message));
  OptimiseOptions oo;
  oo.sampleRate = kSr;
  auto opt = optimise(std::move(inf.value()), oo);
  REQUIRE(opt.ok());
  ScheduleOptions so;
  so.sampleRate = kSr;
  so.previous = previous;
  auto s = schedule(opt.value(), so);
  REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
  auto c = std::make_unique<Compiled>();
  c->name = patch.name();
  c->typed = std::move(opt.value());
  c->sched = std::move(s.value());
  return c;
}

// Counts live images and records the thread that destroyed them: wraps any ProgramImage through the ABI only (the
// host must not depend on the backend).
std::atomic<int> gLiveImages{0};
std::atomic<int> gDestroyedOffMain{0};
std::thread::id gMainThread;

class CountingImage final : public eng::ProgramImage {
public:
  explicit CountingImage(std::unique_ptr<eng::ProgramImage> inner) : inner_(std::move(inner)) { ++gLiveImages; }
  ~CountingImage() override {
    --gLiveImages;
    if (std::this_thread::get_id() != gMainThread) ++gDestroyedOffMain;
  }
  void process(eng::ProcessContext& ctx) noexcept override { inner_->process(ctx); }
  const StateLayout& stateLayout() const noexcept override { return inner_->stateLayout(); }
  const eng::ImageInterface& io() const noexcept override { return inner_->io(); }
  eng::StateArena stateArena() noexcept override { return inner_->stateArena(); }
  uint64_t rejectedBlocks() const noexcept override { return inner_->rejectedBlocks(); }

private:
  std::unique_ptr<eng::ProgramImage> inner_;
};

std::unique_ptr<eng::ProgramImage> buildImage(const Compiled& c, uint32_t groups = 1) {
  eng::InterpretOptions io;
  io.maxBlockFrames = kBlock;
  io.maxVoiceGroups = groups;
  auto built = eng::buildInterpreted(c.sched, io);
  REQUIRE_MESSAGE(built.ok(), (built.ok() ? "" : built.error().message));
  return std::make_unique<CountingImage>(std::move(built.value()));
}

// ---- rendering --------------------------------------------------------------------------------------------------
struct Render {
  std::vector<float> ch[2];
  int64_t time = 0;
  uint64_t rtViolations = 0;
};

void render(eng::ProgramHost& h, uint32_t frames, Render& r, uint32_t block = kBlock) {
  std::vector<float> b0(block), b1(block);
  float* outs[2] = {b0.data(), b1.data()};
  while (frames > 0) {
    const uint32_t n = std::min(block, frames);
    eng::AudioBlock ab;
    ab.out = outs;
    ab.numOut = 2;
    ab.frames = n;
    ab.frameTime = r.time;
    r.rtViolations += pq::alloc_trap::countRtViolations([&] {
      pq::DenormalGuard dg;
      h.process(ab);
    });
    r.ch[0].insert(r.ch[0].end(), b0.begin(), b0.begin() + n);
    r.ch[1].insert(r.ch[1].end(), b1.begin(), b1.begin() + n);
    r.time += n;
    frames -= n;
  }
}

eng::HostConfig globalConfig() {
  eng::HostConfig hc;
  hc.sampleRate = kSr;
  hc.maxBlockFrames = kBlock;
  hc.numOut = 2;
  return hc;
}

double dbfs(double x) { return x > 0.0 ? 20.0 * std::log10(x) : -INFINITY; }

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b, size_t from = 0, size_t to = SIZE_MAX) {
  double m = 0.0;
  to = std::min({to, a.size(), b.size()});
  for (size_t i = from; i < to; ++i) m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
  return m;
}

// Largest |x[n] − x[n−1]| for n in [from, to).
double maxStep(const std::vector<float>& x, size_t from, size_t to) {
  double m = 0.0;
  for (size_t i = std::max<size_t>(from, 1); i < std::min(to, x.size()); ++i)
    m = std::max(m, std::fabs(static_cast<double>(x[i]) - x[i - 1]));
  return m;
}

// The state block of the entry with this kernel in `img` (first match), or null.
const graph::StateEntry* findEntry(const eng::ProgramImage& img, dsp::Atom atom, uint32_t nth = 0) {
  for (const graph::StateEntry& e : img.stateLayout().entries)
    if (e.tag.kernel == static_cast<uint16_t>(atom) && nth-- == 0) return &e;
  return nullptr;
}

// A global echo: an oscillator into a feedback delay line, with a param on the unchanged path.
const char* kEcho = R"(
patch Echo {
  param level = 0.3 [0..1]
  out y : audio
  osc = sine(phasor(220.5))
  e = delay(osc * 0.4 + e * 0.5, 2.7ms, 10ms)
  y = osc * level + e
}
)";
// The same plus an unrelated node (a second oscillator into an export: computed, not played).
Patch echoPlusSpare() { return withSpare(parsePatch(kEcho), 331.0, false); }

// Two outputs: `a` is the unchanged path, `b` gets a new downstream gain.
const char* kTwoOuts = R"(
patch Two {
  out a : audio
  out b : audio
  osc = sine(phasor(220.5))
  f = delay(osc * 0.4 + f * 0.5, 2.7ms, 10ms)
  a = f * 0.5
  b = f * 0.75
}
)";
const char* kTwoOutsGain = R"(
patch Two {
  out a : audio
  out b : audio
  osc = sine(phasor(220.5))
  f = delay(osc * 0.4 + f * 0.5, 2.7ms, 10ms)
  a = f * 0.5
  b = f * 0.25
}
)";
// The delay ring grows (10 ms → 20 ms max): another tag, so the delay starts fresh.
const char* kTwoOutsLongerRing = R"(
patch Two {
  out a : audio
  out b : audio
  osc = sine(phasor(220.5))
  f = delay(osc * 0.4 + f * 0.5, 2.7ms, 20ms)
  a = f * 0.5
  b = f * 0.75
}
)";

struct HostFixture {
  std::vector<std::unique_ptr<Compiled>> compiled;
  HostFixture() { gMainThread = std::this_thread::get_id(); }
  const Compiled& add(const Patch& patch, const Compiled* previous = nullptr) {
    compiled.push_back(compilePatch(patch, previous ? &previous->sched.state : nullptr));
    return *compiled.back();
  }
  const Compiled& add(const char* text, const Compiled* previous = nullptr) { return add(parsePatch(text), previous); }
};

}  // namespace

TEST_CASE("hot swap: transfer plan matches by key/channel/arena + tag, coalesces, keeps offsets") {
  HostFixture f;
  const Compiled& a = f.add(kEcho);
  const Compiled& b = f.add(echoPlusSpare(), &a);
  auto ia = buildImage(a), ib = buildImage(b);
  auto plan = eng::buildTransferPlan(ia->stateLayout(), ia->stateArena(), ib->stateLayout(), ib->stateArena());
  REQUIRE(plan.ok());
  CHECK(plan->matched == a.sched.state.entries.size());  // every old entry survives
  CHECK(plan->fresh >= 1);                               // the spare oscillator
  CHECK(plan->mismatched == 0);
  CHECK(plan->dropped == 0);
  // Stable offsets (T1.5): the surviving entries are one contiguous range copied in place.
  REQUIRE(plan->global.size() == 1);
  CHECK(plan->global[0].src == plan->global[0].dst);

  // Without the previous layout the offsets may move, but the transfer still goes by key.
  const Compiled& b2 = f.add(echoPlusSpare());
  auto ib2 = buildImage(b2);
  auto plan2 = eng::buildTransferPlan(ia->stateLayout(), ia->stateArena(), ib2->stateLayout(), ib2->stateArena());
  REQUIRE(plan2.ok());
  CHECK(plan2->matched == plan->matched);
  CHECK(plan2->globalBytes == plan->globalBytes);

  // A resized delay ring is another tag: fresh state, the old ring is dropped.
  const Compiled& c = f.add(kTwoOuts);
  const Compiled& d = f.add(kTwoOutsLongerRing, &c);
  auto ic = buildImage(c), id = buildImage(d);
  auto plan3 = eng::buildTransferPlan(ic->stateLayout(), ic->stateArena(), id->stateLayout(), id->stateArena());
  REQUIRE(plan3.ok());
  CHECK(plan3->mismatched == 1);
  CHECK(plan3->dropped == 1);
  CHECK(plan3->matched == c.sched.state.entries.size() - 1);
}

TEST_CASE("hot swap: unchanged path + added node → identical to no swap, oscillator phase exact, RtScope clean") {
  HostFixture f;
  const Compiled& a = f.add(kEcho);
  const Compiled& b = f.add(echoPlusSpare(), &a);
  constexpr uint32_t kBefore = 64 * 37 + 64, kAfter = 64 * 200;

  for (const eng::SwapMode mode : {eng::SwapMode::Auto, eng::SwapMode::Instant, eng::SwapMode::Crossfade}) {
    CAPTURE(static_cast<int>(mode));
    // Reference: A only.
    eng::ProgramHost ref(globalConfig());
    auto refImg = buildImage(a);
    eng::ProgramImage* refRaw = refImg.get();
    REQUIRE(ref.publish(std::move(refImg)).ok());
    Render rr;
    render(ref, kBlock, rr);
    ref.params()[0] = 0.7f;  // a knob turn on the audio thread: must survive the swap
    render(ref, kBefore - kBlock + kAfter, rr);

    // Swapped: A, then A + spare published mid-render (from another thread).
    eng::ProgramHost host(globalConfig());
    REQUIRE(host.publish(buildImage(a)).ok());
    Render rs;
    render(host, kBlock, rs);
    host.params()[0] = 0.7f;
    render(host, kBefore - kBlock, rs);
    auto newImg = buildImage(b);
    eng::ProgramImage* newRaw = newImg.get();
    eng::PublishInfo info;
    bool published = false;
    std::thread compiler([&] {  // no doctest assertions on this thread
      auto r = host.publish(std::move(newImg), eng::SwapPolicy{mode, 10.0f});
      published = r.ok();
      if (published) info = std::move(r.value());
    });
    compiler.join();
    REQUIRE(published);
    CHECK(info.sequence == 2);
    CHECK(info.paramsCarried == 1);
    CHECK(info.plan.fresh >= 1);
    render(host, kAfter, rs);

    CHECK(host.currentSequence() == 2);
    CHECK(host.stats().swaps == 2);
    CHECK(host.stats().crossfades == (mode == eng::SwapMode::Instant ? 0u : 1u));  // Auto: the echo is audible
    CHECK(host.params()[0] == 0.7f);
    CHECK(rs.rtViolations == 0);
    CHECK(rr.rtViolations == 0);
    // Bit-exact output on both channels (mono patch on every channel).
    const double diff = std::max(maxAbsDiff(rr.ch[0], rs.ch[0]), maxAbsDiff(rr.ch[1], rs.ch[1]));
    MESSAGE("unchanged path, max |swap − no swap| = " << diff << " (" << dbfs(diff) << " dBFS)");
    CHECK(diff == 0.0);
    // The oscillator's uint32 phase in the new image equals the reference image's phase, word for word.
    const graph::StateEntry* pr = findEntry(*refRaw, dsp::Atom::Phasor);
    REQUIRE(pr != nullptr);
    const graph::StateEntry* pn = nullptr;
    for (const graph::StateEntry& e : newRaw->stateLayout().entries)
      if (e.key == pr->key && e.channel == pr->channel) pn = &e;
    REQUIRE(pn != nullptr);
    CHECK(std::memcmp(refRaw->stateArena().global + pr->offset, newRaw->stateArena().global + pn->offset, pr->size) == 0);
  }
}

TEST_CASE("hot swap: downstream gain change crossfades; the unchanged path has no discontinuity") {
  HostFixture f;
  const Compiled& a = f.add(kTwoOuts);
  const Compiled& b = f.add(kTwoOutsGain, &a);
  constexpr uint32_t kSwap = 64 * 41, kTotal = 64 * 120;
  const uint32_t fadeFrames = static_cast<uint32_t>(std::lround(10.0 * 0.001 * kSr));  // 480

  auto renderOnly = [&](const Compiled& c) {
    eng::ProgramHost h(globalConfig());
    REQUIRE(h.publish(buildImage(c)).ok());
    Render r;
    render(h, kTotal, r);
    CHECK(r.rtViolations == 0);
    return r;
  };
  const Render refA = renderOnly(a), refB = renderOnly(b);

  auto swapRender = [&](eng::SwapMode mode) {
    eng::ProgramHost h(globalConfig());
    REQUIRE(h.publish(buildImage(a)).ok());
    Render r;
    render(h, kSwap, r);
    REQUIRE(h.publish(buildImage(b), eng::SwapPolicy{mode, 10.0f}).ok());
    render(h, kTotal - kSwap, r);
    CHECK(r.rtViolations == 0);
    return r;
  };
  const Render rs = swapRender(eng::SwapMode::Auto);

  // Unchanged path (channel 0): exactly the no-swap render.
  const double unchanged = maxAbsDiff(refA.ch[0], rs.ch[0]);
  MESSAGE("unchanged path: max |swap − no swap| = " << unchanged << " (" << dbfs(unchanged) << " dBFS)");
  CHECK(unchanged == 0.0);
  CHECK(dbfs(unchanged) < -90.0);

  // Changed path (channel 1): old before the swap, the linear crossfade over 480 frames, exactly new after it.
  CHECK(maxAbsDiff(refA.ch[1], rs.ch[1], 0, kSwap) == 0.0);
  CHECK(maxAbsDiff(refB.ch[1], rs.ch[1], kSwap + fadeFrames, kTotal) == 0.0);
  double fadeErr = 0.0;
  for (uint32_t k = 0; k < fadeFrames; ++k) {
    const double g = static_cast<double>(k + 1) / fadeFrames;
    const double ideal = refA.ch[1][kSwap + k] + g * (refB.ch[1][kSwap + k] - refA.ch[1][kSwap + k]);
    fadeErr = std::max(fadeErr, std::fabs(ideal - rs.ch[1][kSwap + k]));
  }
  CHECK(fadeErr < 1e-6);
  // Steps: the crossfaded signal moves no faster than the two programs themselves (plus the fade slope).
  const double ownStep = std::max(maxStep(refA.ch[1], kSwap - 256, kSwap + fadeFrames + 256),
                                  maxStep(refB.ch[1], kSwap - 256, kSwap + fadeFrames + 256));
  const double fadeStep = maxStep(rs.ch[1], kSwap - 256, kSwap + fadeFrames + 256);
  const double atSwap = std::fabs(static_cast<double>(rs.ch[1][kSwap]) - rs.ch[1][kSwap - 1]);
  const double refAtSwap = std::fabs(static_cast<double>(refA.ch[1][kSwap]) - refA.ch[1][kSwap - 1]);
  MESSAGE("changed path: step at the swap " << atSwap << " (the old program's own step there: " << refAtSwap
                                            << "), max step in the fade " << fadeStep << " vs the programs' " << ownStep);
  // The extra step the swap adds at the boundary is 1/480 of the change there: well below a click; inside the
  // fade the steps stay within the programs' own steps plus the fade slope (max |new − old| / 480).
  const double changeAtSwap = std::fabs(static_cast<double>(refB.ch[1][kSwap]) - refA.ch[1][kSwap]);
  const double maxChange = maxAbsDiff(refA.ch[1], refB.ch[1], kSwap, kSwap + fadeFrames);
  CHECK(std::fabs(atSwap - refAtSwap) <= changeAtSwap / fadeFrames + 1e-6);
  CHECK(fadeStep <= ownStep + maxChange / fadeFrames + 1e-6);

  // Discontinuity: the second difference x[n] − 2x[n−1] + x[n−2] at the swap, beyond what either program has there
  // itself (the smaller excess over the old and the new program). A crossfade adds (new − old) / 480 per sample.
  auto d2 = [](const std::vector<float>& x, size_t n) {
    return static_cast<double>(x[n]) - 2.0 * x[n - 1] + static_cast<double>(x[n - 2]);
  };
  auto excess = [&](const Render& r) {
    double vsA = 0.0, vsB = 0.0;
    for (size_t n = kSwap; n < kSwap + 3; ++n) {
      vsA = std::max(vsA, std::fabs(d2(r.ch[1], n) - d2(refA.ch[1], n)));
      vsB = std::max(vsB, std::fabs(d2(r.ch[1], n) - d2(refB.ch[1], n)));
    }
    return std::min(vsA, vsB);
  };
  const double fadeExcess = excess(rs);
  // Negative control: an instant swap of an audible change does click (the metric sees it).
  const Render ri = swapRender(eng::SwapMode::Instant);
  CHECK(maxAbsDiff(refA.ch[0], ri.ch[0]) == 0.0);  // the unchanged path is exact either way
  CHECK(maxAbsDiff(refB.ch[1], ri.ch[1], kSwap, kTotal) == 0.0);
  const double instantExcess = excess(ri);
  MESSAGE("changed path, discontinuity at the swap (2nd-difference excess): crossfade " << fadeExcess << " ("
          << dbfs(fadeExcess) << " dBFS), instant " << instantExcess << " (" << dbfs(instantExcess) << " dBFS)");
  CHECK(fadeExcess <= 2.0 * changeAtSwap / fadeFrames + 1e-6);
  CHECK(instantExcess > 100.0 * fadeExcess);
}

TEST_CASE("hot swap: silent output and stopped transport swap instantly; playing transport crossfades") {
  HostFixture f;
  const char* kGate = R"(
patch Gate {
  param on = false
  out y : audio
  y = on ? sine(phasor(440)) : 0.0
}
)";
  const Compiled& a = f.add(kGate);
  const Compiled& b = f.add(kGate, &a);
  {
    eng::ProgramHost h(globalConfig());
    REQUIRE(h.publish(buildImage(a)).ok());
    Render r;
    render(h, 640, r);
    REQUIRE(h.publish(buildImage(b)).ok());
    render(h, 64, r);
    CHECK(h.stats().instantSwaps == 2);
    CHECK(h.stats().crossfades == 0);
    CHECK_FALSE(h.crossfading());
  }
  {
    eng::ProgramHost h(globalConfig());
    REQUIRE(h.publish(buildImage(a)).ok());
    Render r;
    render(h, 640, r);
    h.transport().playing = true;
    REQUIRE(h.publish(buildImage(b)).ok());
    render(h, 64, r);
    CHECK(h.stats().crossfades == 1);
    CHECK(h.crossfading());
    render(h, 640, r);
    CHECK_FALSE(h.crossfading());
  }
}

namespace {

struct Script {
  std::vector<std::pair<int64_t, eng::NoteEvent>> events;  // absolute times, sorted
  static void fill(void* user, int64_t time, uint32_t frames, eng::EventBuffer& out) noexcept {
    auto* s = static_cast<Script*>(user);
    for (const auto& [t, e] : s->events)
      if (t >= time && t < time + frames) {
        eng::NoteEvent ev = e;
        ev.frame = static_cast<uint32_t>(t - time);
        out.push(ev);
      }
  }
};

const char* kVoice = R"(
patch V {
  out y : audio voice
  y = sine(phasor(voice.hz)) * gate_to_real(voice.gate) * 0.3
}
)";
const char* kVoiceGain = R"(
patch V {
  out y : audio voice
  y = sine(phasor(voice.hz)) * gate_to_real(voice.gate) * 0.2
}
)";

eng::HostConfig voiceConfig(Script& s) {
  eng::HostConfig hc = globalConfig();
  hc.voices = true;
  hc.voice.maxVoices = 16;
  hc.events = {&Script::fill, &s};
  return hc;
}

}  // namespace

TEST_CASE("hot swap: voices keep sounding through a swap (instant and crossfaded)") {
  HostFixture f;
  const Compiled& a = f.add(kVoice);
  const Compiled& spare = f.add(withSpare(parsePatch(kVoice), 1.5, true), &a);
  const Compiled& gain = f.add(kVoiceGain, &a);
  const uint32_t groups = eng::voiceGroupsFor(16);
  Script s;
  s.events = {{100, eng::noteOn(0, 1, 220.0f, 0.8f)},  {130, eng::noteOn(0, 2, 277.2f, 0.8f)},
              {170, eng::noteOn(0, 3, 330.0f, 0.8f)},  {64 * 30 + 200, eng::noteOn(0, 4, 415.3f, 0.8f)},  // in the fade
              {64 * 60, eng::noteOff(0, 2)},           {64 * 70, eng::noteOn(0, 5, 523.3f, 0.8f)}};
  constexpr uint32_t kSwap = 64 * 30, kTotal = 64 * 160;

  auto run = [&](const Compiled* first, const Compiled* second, eng::SwapMode mode, uint32_t& livesAtSwap) {
    eng::ProgramHost h(voiceConfig(s));
    REQUIRE(h.publish(buildImage(*first, groups)).ok());
    Render r;
    render(h, kSwap, r);
    livesAtSwap = h.voices()->liveVoices();
    std::vector<uint32_t> lanes;
    for (uint32_t id = 1; id <= 3; ++id) lanes.push_back(h.voices()->laneOf(id));
    if (second != nullptr) {
      REQUIRE(h.publish(buildImage(*second, groups), eng::SwapPolicy{mode, 10.0f}).ok());
      render(h, kBlock, r);
      // The same notes in the same lanes, still sounding.
      CHECK(h.voices()->liveVoices() == livesAtSwap);
      for (uint32_t id = 1; id <= 3; ++id) CHECK(h.voices()->laneOf(id) == lanes[id - 1]);
      CHECK(h.stats().crossfades == (mode == eng::SwapMode::Instant ? 0u : 1u));  // voices active: audible
    }
    render(h, kTotal - static_cast<uint32_t>(r.time), r);
    CHECK(r.rtViolations == 0);
    CHECK(h.voices()->stats().noteOns == 5);
    return r;
  };
  uint32_t live = 0;
  const Render refA = run(&a, nullptr, eng::SwapMode::Auto, live);
  CHECK(live == 3);
  const Render refGain = run(&gain, nullptr, eng::SwapMode::Auto, live);

  SUBCASE("unrelated node added: output identical to no swap") {
    for (const eng::SwapMode mode : {eng::SwapMode::Auto, eng::SwapMode::Instant}) {
      const Render r = run(&a, &spare, mode, live);
      const double d = maxAbsDiff(refA.ch[0], r.ch[0]);
      MESSAGE("voices, unchanged path: max |swap − no swap| = " << d);
      CHECK(d == 0.0);
    }
  }
  SUBCASE("voice gain changed: crossfade, then exactly the new program with the voices' phases carried") {
    const Render r = run(&a, &gain, eng::SwapMode::Auto, live);
    const uint32_t fadeFrames = 480;
    CHECK(maxAbsDiff(refA.ch[0], r.ch[0], 0, kSwap) == 0.0);
    // After the fade: exactly what the new program renders from the start (same notes, same phases), including
    // the note that started during the fade (its lane was reset in both images).
    CHECK(maxAbsDiff(refGain.ch[0], r.ch[0], kSwap + fadeFrames, kTotal) == 0.0);
    double fadeErr = 0.0;
    for (uint32_t k = 0; k < fadeFrames; ++k) {
      const double g = static_cast<double>(k + 1) / fadeFrames;
      const double ideal = refA.ch[0][kSwap + k] + g * (refGain.ch[0][kSwap + k] - refA.ch[0][kSwap + k]);
      fadeErr = std::max(fadeErr, std::fabs(ideal - r.ch[0][kSwap + k]));
    }
    CHECK(fadeErr < 1e-6);
  }
}

TEST_CASE("hot swap: old images are retired to the epoch reclaimer and freed off the audio thread") {
  HostFixture f;
  const Compiled& a = f.add(kTwoOuts);
  const Compiled& b = f.add(kTwoOutsGain, &a);
  const int live0 = gLiveImages.load();
  {
    eng::ProgramHost h(globalConfig());
    Render r;
    REQUIRE(h.publish(buildImage(a)).ok());
    render(h, 64, r);
    REQUIRE(h.publish(buildImage(b), eng::SwapPolicy{eng::SwapMode::Instant, 10.0f}).ok());
    CHECK(h.imagesAlive() == 2);
    render(h, 64, r);  // swap: A retired by the audio thread (it frees nothing)
    CHECK(gLiveImages.load() == live0 + 2);
    CHECK(h.collect() == 0);  // handed to the reclaimer; the audio thread has not passed a quiescent state since
    render(h, 64, r);
    CHECK(h.collect() == 1);
    CHECK(h.imagesFreed() == 1);
    CHECK(h.imagesAlive() == 1);
    CHECK(gLiveImages.load() == live0 + 1);

    // A crossfade keeps the old image until the fade is over.
    REQUIRE(h.publish(buildImage(a), eng::SwapPolicy{eng::SwapMode::Crossfade, 5.0f}).ok());
    render(h, 64, r);
    CHECK(h.crossfading());
    render(h, 64, r);
    CHECK(h.collect() == 0);
    CHECK(h.imagesAlive() == 2);
    render(h, 64 * 4, r);  // 5 ms = 240 frames
    CHECK_FALSE(h.crossfading());
    h.collect();
    render(h, 64, r);
    h.collect();
    CHECK(h.imagesFreed() == 2);
    CHECK(h.imagesAlive() == 1);

    // Several publishes before the audio thread runs: applied one per block, in order (each plan is against the
    // image before it).
    REQUIRE(h.publish(buildImage(b), eng::SwapPolicy{eng::SwapMode::Instant, 10.0f}).ok());
    REQUIRE(h.publish(buildImage(a), eng::SwapPolicy{eng::SwapMode::Instant, 10.0f}).ok());
    const uint64_t seq = h.currentSequence();
    render(h, 64, r);
    CHECK(h.currentSequence() == seq + 1);
    render(h, 64, r);
    CHECK(h.currentSequence() == seq + 2);
    h.collect();       // retires both
    render(h, 64, r);  // quiescent
    h.collect();       // frees both
    CHECK(h.imagesFreed() == 4);
    CHECK(r.rtViolations == 0);  // no allocation or free on the audio thread through all of it

    // Offline audio thread: collect() can free without further blocks.
    REQUIRE(h.publish(buildImage(b), eng::SwapPolicy{eng::SwapMode::Instant, 10.0f}).ok());
    render(h, 64, r);
    h.audioOffline();
    h.collect();
    CHECK(h.imagesFreed() == 5);
    h.audioOnline();
  }
  CHECK(gLiveImages.load() == live0);   // the host frees the rest when destroyed
  CHECK(gDestroyedOffMain.load() == 0);  // every image was freed on this (the non-RT) thread
}

TEST_CASE("hot swap: publish refuses mismatched images and a full queue") {
  HostFixture f;
  const Compiled& a = f.add(kTwoOuts);
  eng::HostConfig hc = globalConfig();
  hc.maxPending = 2;
  eng::ProgramHost h(hc);
  CHECK_FALSE(h.publish(nullptr).ok());
  {
    eng::InterpretOptions io;
    io.maxBlockFrames = 128;  // not the host's
    auto built = eng::buildInterpreted(a.sched, io);
    REQUIRE(built.ok());
    CHECK_FALSE(h.publish(std::move(built.value())).ok());
  }
  REQUIRE(h.publish(buildImage(a)).ok());
  REQUIRE(h.publish(buildImage(a)).ok());
  auto third = h.publish(buildImage(a));
  REQUIRE_FALSE(third.ok());
  CHECK(third.error().code == pq::ErrorCode::Overflow);
  Render r;
  render(h, 64, r);
  CHECK(h.publish(buildImage(a)).ok());  // room again
  CHECK(h.lastPublished() != nullptr);
}
