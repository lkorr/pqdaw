// TX.smooth: every continuous param of a user patch and the meta modules is click-free when it jumps (pq/graph/
// schedule.h "Param ramps"). Per param: the device plays (an effect: a 110 Hz sine; an instrument: a held note), and
// mid-note the param jumps from its default by 35 % of its range at one block boundary (what a knob drag, an automation
// step or a macro delivers). Its high-frequency energy (4th-order Butterworth high-pass at 12 kHz, above what the test signals and a fast glide contain: a click is broadband) over 10 ms before to
// 50 ms after the jump is compared with the same move made slowly, a 200 ms sweep of per-block steps (the ramped
// reference): the jump may add at most kMaxExcessDb. The same jump scheduled without param ramps (the T1.5 schedule,
// as before TX.smooth) is measured too, to show the metric sees the clicks the ramps remove. JIT, block 64, 48 kHz;
// every block inside an RtScope (the param_ramp path allocates nothing).
// GU.10: the stock devices are retired (retired/); GU.12: their test copies (tests/retired_devices/) stay in the sweep as
// real-world param sets for the ramp mechanism.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/engine/note_list.h"
#include "pq/engine/offline_render.h"
#include "pq/engine/voices.h"

namespace fs = std::filesystem;
namespace eng = pq::engine;
namespace g = pq::graph;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 64;
constexpr uint64_t kFrames = 48000;           // 1 s
constexpr uint64_t kJump = 375 * kBlock;      // 0.5 s, a block boundary
constexpr uint64_t kSweep = 150 * kBlock;     // the reference's 200 ms
constexpr double kMaxExcessDb = 6.0;

std::string retiredPath(const char* file) {
  return (fs::path(PQ_TEST_DATA_DIR) / "retired_devices" / file).lexically_normal().generic_string();
}

struct Device {
  const char* name;
  std::string text;  // the wrapper patch
  bool voices;
};

std::vector<Device> devices() {
  // At the jump (0.5 s) the sine is at its peak, so a gain step there is a step of the output (the worst case).
  const std::string sine = "  s = sine(phasor(110Hz) + 0.25) * 0.5\n";
  auto effect = [&](const char* name, const char* file, const std::string& call) {
    return Device{name, "import \"" + retiredPath(file) + "\"\npatch T {\n  out y : audio stereo\n" + sine + "  d = " + call + "\n  y = d\n}\n", false};
  };
  std::vector<Device> d;
  // A user patch that multiplies by its params directly (no smoothed gain): what any new device written that way gets.
  d.push_back(Device{"user patch (x * level + offset, a crossfade)",
                     "patch T {\n  out y : audio\n  param level = 0.5 [0..1]\n  param offset = 0 [-0.5..0.5]\n"
                     "  param mix = 0.2 [0..1]\n" + sine + "  t = sine(phasor(330Hz)) * 0.5\n"
                     "  y = (s * level + offset) * (1 - mix) + t * mix\n}\n",
                     false});
  // GU.12: the retired stock devices' test copies (Utility, Saturator, EQ Eight, Auto Filter, the LFO, Operator).
  d.push_back(effect("utility", "utility.pqp", "utility((s, s))"));
  d.push_back(effect("saturator", "saturator.pqp", "saturator((s, s))"));
  d.push_back(effect("eq_eight", "eq_eight.pqp", "eq_eight((s, s))"));
  d.push_back(effect("auto_filter", "auto_filter.pqp", "auto_filter((s, s))"));
  d.push_back(Device{"lfo_device (its export)",
                     "import \"" + retiredPath("lfo_device.pqp") + "\"\npatch T {\n  out y : audio\n" + sine +
                         "  d = lfo_device((s, s))\n  y = s * (1 + 0.5 * d.lfo)\n}\n",
                     false});
  d.push_back(Device{"operator (additive, filter on)",
                     "import \"" + retiredPath("operator.pqp") + "\"\npatch T {\n  out y : audio voice stereo\n"
                     "  out done : bool voice\n  o = operator(algorithm: 11, onB: 1, onC: 1, onD: 1, filterOn: 1)\n"
                     "  y = o\n  done = o.done\n}\n",
                     true});
  // GU.4: the Phase accumulator and the VCA are group presets (their params value blocks: a phasor's frequency, a
  // multiplier, as here); the Meta Operator is deleted.
  d.push_back(Device{"meta modules (phasor → wavetable → envelope → × level)",
                     "patch T {\n  param level = 0.7 [0..1]\n  out y : audio voice mono\n  a = phasor(voice.hz)\n"
                     "  w = meta_wavetable(a)\n  e = meta_envelope(w)\n  y = e * level\n}\n",
                     true});
  return d;
}

struct Move {
  std::vector<float> base;
  uint32_t slot = 0;
  float to = 0.0f;
  uint64_t frames = 0;  // 0: a jump at kJump; else a sweep of per-block steps from kJump over `frames`
};

void hook(void* user, int64_t t, uint32_t, eng::TransportSnapshot&, std::span<float> params) noexcept {
  const Move& m = *static_cast<const Move*>(user);
  for (size_t i = 0; i < params.size() && i < m.base.size(); ++i) params[i] = m.base[i];
  if (t < static_cast<int64_t>(kJump)) return;
  const float from = m.base[m.slot];
  double u = 1.0;
  if (m.frames > 0) u = std::min(1.0, static_cast<double>(t - static_cast<int64_t>(kJump) + kBlock) / static_cast<double>(m.frames));
  params[m.slot] = static_cast<float>(from + (m.to - from) * u);
}

struct Rendered {
  std::vector<std::vector<float>> ch;
  uint64_t rtViolations = 0;
};

Rendered render(const g::Schedule& s, const g::Patch& scheduled, bool voices, const Move& m,
                pq::compile::Backend backend = pq::compile::Backend::Jit) {
  pq::compile::ImageOptions io;
  io.maxBlockFrames = kBlock;
  io.maxVoiceGroups = 1;
  auto img = pq::compile::buildImage(s, backend, io);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? std::string() : img.error().message));
  eng::RenderRequest rq;
  rq.image = img->get();
  rq.frames = kFrames;
  rq.blockFrames = kBlock;
  rq.params = m.base;
  rq.hook.fn = &hook;
  rq.hook.user = const_cast<Move*>(&m);
  eng::VoiceEngine ve;
  eng::NoteListSource notes(std::vector<eng::TimedNote>{{2400, static_cast<int64_t>(kFrames), 233.0f, 0.8f, 0.5f}});  // not at a zero crossing at the jump
  if (voices) {
    eng::VoiceConfig vc;
    vc.maxVoices = 4;
    vc.doneSlot = eng::findVoiceSlot((*img)->io(), scheduled, "done");
    REQUIRE(ve.prepare(img->get(), vc).ok());
    rq.voices = &ve;
    rq.events = notes.source();
  }
  eng::BufferSink sink;
  Rendered r;
  pq::Result<eng::RenderStats> st = pq::Error{};
  {
    // renderOffline runs each chunk's blocks inside an RtScope (its setup outside): what is counted here is what the
    // blocks allocated.
    pq::alloc_trap::ExpectViolations expect;
    st = eng::renderOffline(rq, sink);
    r.rtViolations = expect.count();
  }
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  CHECK(r.rtViolations == 0);
  r.ch = std::move(sink.channels);
  return r;
}

// The signal above 12 kHz (4th-order Butterworth high-pass, two biquads, double), all channels.
std::vector<std::vector<double>> highPassed(const Rendered& r) {
  std::vector<std::vector<double>> out;
  const double w0 = std::tan(3.141592653589793 * 12000.0 / kSr);
  for (const std::vector<float>& x : r.ch) {
    std::vector<double> y(x.begin(), x.end());
    for (const double qf : {0.5411961001461970, 1.3065629648763766}) {  // Butterworth Qs for order 4
      const double k = w0, norm = 1.0 / (1.0 + k / qf + k * k);
      const double b0 = norm, b1 = -2.0 * norm, b2 = norm, a1 = 2.0 * (k * k - 1.0) * norm, a2 = (1.0 - k / qf + k * k) * norm;
      double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
      for (double& v : y) {
        const double o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1, x1 = v, y2 = y1, y1 = o;
        v = o;
      }
    }
    out.push_back(std::move(y));
  }
  return out;
}

// Mean power over [t0, t1) of all channels.
double power(const std::vector<std::vector<double>>& y, uint64_t t0, uint64_t t1) {
  double e = 0.0;
  for (const std::vector<double>& c : y)
    for (uint64_t t = t0; t < t1 && t < c.size(); ++t) e += c[t] * c[t];
  return e / static_cast<double>(t1 - t0);
}

// The move's transient: high-frequency power from 10 ms before to 50 ms after the move's start, over the larger of the
// steady states before it (the 60 ms up to 2.5 ms before) and after it (300 – 360 ms after, both moves done), in dB.
// A click is broadband energy on top of both steady states; a move that changes the sound's own spectrum (a pitch, a
// drive) lands between them. `floor`: power that far below the sound itself is not heard.
double transientDb(const Rendered& r) {
  const auto hp = highPassed(r);
  double sig = 0.0;
  for (const std::vector<float>& c : r.ch)
    for (uint64_t t = kJump - 480; t < kJump + 2400; ++t) sig += static_cast<double>(c[t]) * c[t];
  sig /= 2880.0;
  const double floor = std::max(1e-6 * sig, 1e-12);  // −60 dB re the sound in the window, −120 dB re full scale
  const double win = power(hp, kJump - 480, kJump + 2400);
  const double steady = std::max(power(hp, kJump - 2880 - 120, kJump - 120), power(hp, kJump + 14400, kJump + 17280));
  return 10.0 * std::log10((win + floor) / (steady + floor));
}

}  // namespace

TEST_CASE("smooth params: every continuous param of a user patch, the retired devices' copies and the meta modules jumps without a click" *
          doctest::test_suite("slow")) {
  uint32_t tested = 0, clickedBefore = 0;
  double worst = -1e9;
  std::string worstWhere;
  for (const Device& dev : devices()) {
    const fs::path file = fs::temp_directory_path() / ("pq_test_smooth_" + std::to_string(tested) + "_" +
                                                       std::to_string(std::hash<std::string>{}(dev.name) % 100000) + ".pqp");
    std::ofstream(file, std::ios::binary) << dev.text;
    auto src = pq::compile::SourceFile::load(file);
    REQUIRE_MESSAGE(src.ok(), dev.name << ": " << (src.ok() ? std::string() : src.error().message));
    auto compiled = pq::compile::compilePatch(**src, 0);
    REQUIRE_MESSAGE(compiled.ok(), dev.name << ": " << (compiled.ok() ? std::string() : compiled.error().message));
    const g::Schedule& ramped = compiled->schedule;
    g::ScheduleOptions so;
    so.sampleRate = kSr;
    so.paramRamps = false;
    auto plainR = g::schedule(compiled->optimised, so);
    REQUIRE(plainR.ok());
    const g::Schedule& plain = plainR.value();
    const g::Patch& scheduled = compiled->scheduled().patch;
    // Utility's params all reach smoothed gains' targets (gain / gain_add smooth `g` themselves): no ramps there.
    if (std::string(dev.name) != "utility") CHECK(!ramped.ramps.empty());
    std::vector<float> base;
    for (const g::ParamSlot& p : ramped.params) base.push_back(p.defaultValue);
    uint32_t devTested = 0;
    for (uint32_t k = 0; k < ramped.params.size(); ++k) {
      const g::ParamSlot& p = ramped.params[k];
      if (p.kind != g::Kind::Real || !p.hasRange || !(p.rangeMax > p.rangeMin)) continue;
      const double span = p.rangeMax - p.rangeMin;
      const double frac = (p.defaultValue - p.rangeMin) / span;
      const float to = static_cast<float>(p.rangeMin + span * (frac < 0.5 ? frac + 0.35 : frac - 0.35));
      const std::string pname = p.node < scheduled.nodeCount() ? std::string(scheduled.nodeName(p.node)) : std::to_string(k);
      INFO(std::string(dev.name) << " param " << pname << ": " << p.defaultValue << " -> " << to);
      Move jump{base, k, to, 0}, sweep{base, k, to, kSweep};
      const double tJump = transientDb(render(ramped, scheduled, dev.voices, jump));
      const double tRef = transientDb(render(ramped, scheduled, dev.voices, sweep));
      const double tOld = transientDb(render(plain, scheduled, dev.voices, jump));
      const double excess = tJump - std::max(tRef, 0.0);
      const double before = tOld - std::max(tRef, 0.0);
      CHECK_MESSAGE(excess <= kMaxExcessDb, "a click: +" << excess << " dB over the slow move (jump " << tJump << " dB, slow "
                                                         << tRef << " dB; without ramps +" << before << " dB)");
      if (excess > worst) {
        worst = excess;
        worstWhere = std::string(dev.name) + " " + pname;
      }
      if (before > 10.0) {
        ++clickedBefore;
        MESSAGE("without ramps a click: " << std::string(dev.name) << " " << pname << " +" << before << " dB (with ramps +"
                                          << excess << " dB)");
      }
      ++tested;
      ++devTested;
    }
    MESSAGE(std::string(dev.name) << ": " << devTested << " params, " << ramped.ramps.size() << " ramps");
    std::error_code ec;
    fs::remove(file, ec);
  }
  MESSAGE("smooth params: " << tested << " params jumped; worst excess over the slow move " << worst << " dB ("
                            << worstWhere << "); " << clickedBefore << " of them clicked (> +10 dB) without ramps");
  CHECK(tested >= 150);
  CHECK(clickedBefore >= 6);  // the metric sees the clicks the ramps remove (the user patch, Operator volume / pan, the meta modules)
}
