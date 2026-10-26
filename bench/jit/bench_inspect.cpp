// T4.5: what live probes and cost meters cost (pq/engine/inspect.h), and how well the meter agrees with a benchmark.
// Stock devices of bench_devices.cpp (same patches, params and stereo input, 64-frame blocks), JIT and interpreter:
//
//   plain        the schedule without probes, nothing bound: what every image costs while no probe or meter is shown
//   meter off    the same image bound to a CostMeter that is disabled (the circuit editor with the Cost toggle off)
//   probes idle  a schedule with a probe on every output of the test patch's nodes, taps bound, nobody reading
//   probes read  the same, every probe's ring read (the copies run)
//   metered      plain, the meter enabled (rdtsc around every region)
//   meter says   the meter's own total for the metered runs (ns per stereo frame), against `plain` measured here
//
// Each variant is measured alternately with the others, three rounds, best median, ns per stereo sample frame.
//   pq_bench_inspect [--block N] [filter]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../tests/stdlib/stdlib_harness.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/probe_level.h"
#include "pq/core/tap_ring.h"
#include "pq/engine/inspect.h"
#include "pq/graph/inspect.h"
#include "pq_bench.h"

namespace st = stdlib_test;
namespace g = pq::graph;
namespace eng = pq::engine;

namespace {

uint32_t kBlock = 64;
constexpr float kSr = 48000.0f;
std::string gFilter;

struct Signals {
  std::vector<float> ch[2];
  Signals() {
    for (uint32_t i = 0; i < 1024; ++i) {
      const float t = static_cast<float>(i);
      ch[0].push_back(0.4f * std::sin(0.37f * t) + 0.25f * std::sin(1.91f * t) + 0.1f * std::sin(2.71f * t));
      ch[1].push_back(0.35f * std::sin(0.23f * t) + 0.3f * std::sin(1.37f * t) + 0.1f * std::sin(2.93f * t));
    }
  }
};
const Signals& sig() {
  static const Signals s;
  return s;
}

// st::compile with a probe on every output of every node of the test patch.
st::Program compileProbed(const std::string& text) {
  st::Program p;
  p.text = text;
  g::ParseOptions po;
  po.imports = &st::stdlib().loader;
  auto parsed = g::parse(p.text, (st::stdlibDir() / "__t45_bench.pqp").string(), po);
  if (!parsed) std::abort();
  p.file = std::move(parsed.value());
  g::PatchLibrary lib = st::stdlib().library;
  lib.add(p.file);
  const g::Patch& root = p.file.patches[0];
  std::vector<g::ProbeTarget> targets;
  for (g::NodeIndex n = 0; n < root.nodeCount(); ++n)
    for (uint16_t o = 0; o < root.node(n).numOutputs; ++o) {
      g::ProbeTarget t;
      t.node = root.node(n).id;
      t.output = o;
      targets.push_back(t);
    }
  g::InferOptions io;
  io.library = &lib;
  io.probes = targets;
  g::InferResult inf = g::inferDetailed(root, io);
  if (!inf.ok()) std::abort();
  std::vector<uint32_t> valueMap;
  g::OptimiseOptions oo;
  oo.sampleRate = kSr;
  oo.valueMap = &valueMap;
  const std::vector<uint32_t> sources = inf.probeSources;
  auto opt = g::optimise(std::move(inf.typed), oo);
  if (!opt) std::abort();
  p.typed = std::move(opt.value());
  const std::vector<g::ProbePoint> points = g::probePoints(p.typed.patch, sources, valueMap);
  g::ScheduleOptions so;
  so.sampleRate = kSr;
  so.probes = points;
  auto s = g::schedule(p.typed, so);
  if (!s) std::abort();
  p.sched = std::move(s.value());
  return p;
}

class Rig {
public:
  Rig(const st::Program& p, st::Backend backend, const std::map<std::string, double>& params) {
    auto built = st::build(p, backend, kBlock, 1);
    if (!built) {
      std::fprintf(stderr, "%s\n", built.error().message.c_str());
      std::exit(2);
    }
    img_ = std::move(built.value());
    const eng::ImageInterface& io = img_->io();
    gIn_.resize(io.globalIn.size());
    for (size_t s = 0; s < io.globalIn.size(); ++s) {
      const std::vector<float>& src = sig().ch[io.globalIn[s].channel & 1];
      gIn_[s].assign(src.begin(), src.begin() + kBlock);
    }
    gOut_.assign(io.globalOut.size(), std::vector<float>(kBlock));
    for (auto& b : gIn_) gInP_.push_back(b.data());
    for (auto& b : gOut_) gOutP_.push_back(b.data());
    params_.assign(io.params.size(), NAN);
    for (size_t i = 0; i < io.params.size(); ++i)
      if (auto it = params.find(p.typed.patch.nodeName(io.params[i].node)); it != params.end())
        params_[i] = static_cast<float>(it->second);
    ctx_.frames = kBlock;
    ctx_.sampleRate = kSr;
    ctx_.in = gInP_.data();
    ctx_.numIn = static_cast<uint32_t>(gInP_.size());
    ctx_.out = gOutP_.data();
    ctx_.numOut = static_cast<uint32_t>(gOutP_.size());
    ctx_.params = params_.data();
    ctx_.numParams = static_cast<uint32_t>(params_.size());
    // Taps for every probe (rings without a reader until read() is wanted).
    for (const g::ProbeRef& pr : p.sched.probes) {
      rings_.push_back(std::make_unique<pq::TapRing>(1, 1u << 14, 1024));
      taps_.push_back({pr.id, rings_.back().get(), nullptr});
    }
  }
  void bind(eng::CostMeter* meter) {
    eng::InspectBinding b;
    b.probes = taps_;
    b.meter = meter;
    if (!img_->bindInspect(b)) std::abort();
  }
  void readers(bool on) {
    for (auto& r : rings_) on ? r->addReader() : r->removeReader();
  }
  void process() noexcept { img_->process(ctx_); }
  float last() const { return gOut_[0].back(); }
  size_t probes() const { return taps_.size(); }

private:
  std::unique_ptr<eng::ProgramImage> img_;
  std::vector<std::vector<float>> gIn_, gOut_;
  std::vector<const float*> gInP_;
  std::vector<float*> gOutP_;
  std::vector<float> params_;
  eng::ProcessContext ctx_;
  std::vector<std::unique_ptr<pq::TapRing>> rings_;
  std::vector<eng::ProbeTap> taps_;
};

struct Case {
  std::string name, text;
  std::map<std::string, double> params;
};

double gWorstMeterErr = 0.0;

void runCase(const Case& c, st::Backend backend) {
  const std::string name = std::string(st::toText(backend)) + "/" + c.name;
  if (!gFilter.empty() && name.find(gFilter) == std::string::npos) return;
  const st::Program plainP = st::compile(c.text);
  const st::Program probedP = compileProbed(c.text);
  Rig plain(plainP, backend, c.params), meterOff(plainP, backend, c.params), metered(plainP, backend, c.params);
  Rig idle(probedP, backend, c.params), read(probedP, backend, c.params);
  eng::CostMeter offMeter, onMeter;
  meterOff.bind(&offMeter);
  metered.bind(&onMeter);
  onMeter.setEnabled(true);
  idle.bind(nullptr);
  read.bind(nullptr);
  read.readers(true);
  pq::DenormalGuard dg;
  const int rounds = 3;
  const int repeats = backend == st::Backend::Jit ? 9 : 5;
  double best[5] = {1e30, 1e30, 1e30, 1e30, 1e30}, mins[5] = {1e30, 1e30, 1e30, 1e30, 1e30};
  Rig* rigs[5] = {&plain, &meterOff, &idle, &read, &metered};
  eng::CostMeter::Reading rd;
  double meterSays = 1e30;
  for (int round = 0; round < rounds; ++round)
    for (int v = 0; v < 5; ++v) {
      if (v == 4) (void)onMeter.read(rd);  // baseline: the meter's reading covers exactly this measurement
      const auto s = pq::bench::measure([&] { rigs[v]->process(); }, kBlock, repeats, 2, 1.5e6);
      best[v] = std::min(best[v], s.medianNs);
      mins[v] = std::min(mins[v], s.minNs);
      if (v == 4 && onMeter.read(rd)) meterSays = std::min(meterSays, rd.totalNsPerFrame);
    }
  for (Rig* r : rigs) pq::bench::doNotOptimize(r->last());
  const char* unit = "ns/stereo-sample";
  pq::bench::report(("inspect-" + name + " plain").c_str(), {best[0], mins[0]}, unit);
  pq::bench::report(("inspect-" + name + " meter off").c_str(), {best[1], mins[1]}, unit);
  pq::bench::report(("inspect-" + name + " probes idle").c_str(), {best[2], mins[2]}, unit);
  pq::bench::report(("inspect-" + name + " probes read").c_str(), {best[3], mins[3]}, unit);
  pq::bench::report(("inspect-" + name + " metered").c_str(), {best[4], mins[4]}, unit);
  pq::bench::report(("inspect-" + name + " meter says").c_str(), {meterSays, meterSays}, unit);
  const double err = meterSays / best[0] - 1.0;
  gWorstMeterErr = std::max(gWorstMeterErr, std::fabs(err));
  std::printf("      %-38s plain %8.3f  meter-off %+6.1f%%  probes-idle %+6.1f%% (%zu)  probes-read %+6.1f%%  metered %+6.1f%%  "
              "meter says %8.3f (%+5.1f%% vs plain)\n",
              name.c_str(), best[0], 100.0 * (best[1] / best[0] - 1.0), 100.0 * (best[2] / best[0] - 1.0), idle.probes(),
              100.0 * (best[3] / best[0] - 1.0), 100.0 * (best[4] / best[0] - 1.0), meterSays, 100.0 * err);
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--block" && i + 1 < argc) kBlock = std::min<uint32_t>(1024, static_cast<uint32_t>(std::atoi(argv[++i])));
    else gFilter = a;
  }
  pq::bench::SystemLoad load;
  const eng::CostMeter::TimerCost& tc = eng::CostMeter::timerCost();
  std::printf("T4.5 probes and cost meters on the stock devices: stereo, block %u, 48 kHz; JIT target %s; timer cost: "
              "inner %.0f, outer %.0f cycles\n",
              kBlock, st::jitCompiler().hostCpu().c_str(), tc.innerCycles, tc.outerCycles);
  std::vector<Case> cases;
  cases.push_back({"utility", R"(import "devices/utility.pqp"
patch T { in x : audio stereo
  out y : audio stereo
  u = utility(x)
  y = u })", {{"u__width", 1.4}, {"u__balance", 0.2}, {"u__gain", 0.8}}});
  for (const auto& [type, hq] : std::vector<std::pair<int, int>>{{3, 2}, {7, 2}, {3, 8}})
    cases.push_back({"saturator type " + std::to_string(type) + " hq " + std::to_string(hq),
                     "import \"devices/saturator.pqp\"\npatch T { in x : audio stereo\n  out y : audio stereo\n  s = saturator(x, type: " +
                         std::to_string(type) + ", hq: " + std::to_string(hq) + ")\n  y = s }",
                     {{"s__drive", 3.981}, {"s__output", 0.7}}});
  cases.push_back({"eq_eight defaults (6 bands)",
                   "import \"devices/eq_eight.pqp\"\npatch T { in x : audio stereo\n  out y : audio stereo\n  e = eq_eight(x)\n  y = e }",
                   {}});
  for (const Case& c : cases) runCase(c, st::Backend::Jit);
  for (const Case& c : cases) runCase(c, st::Backend::Interp);
  std::printf("worst |meter / plain - 1| %.1f %%; system load during the run %.0f %%\n", 100.0 * gWorstMeterErr, load.busyPercent());
  return 0;
}
