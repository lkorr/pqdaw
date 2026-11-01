// TMM.11: the Envelope meta module — its expansion (forms, the voice's end as `done`, fixed ids, the text form parses
// back), bit-identity with stdlib msenv (mode None) when every curve is 0 on both backends, the curves' shapes against
// the continuous-time law the face draws (attack / decay / release reach their goals at their times; concave and convex
// sides of a straight line), clicks-free early releases with curves, `in` as a VCA, amount, and the tap's encoding.
#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "pq/compile/compile.h"
#include "pq/engine/offline_render.h"
#include "pq/graph/parse.h"
#include "pq/graph/patch_file.h"
#include "pq/meta/envelope.h"
#include "pq/meta/registry.h"
#include "pq/pqc/pqc.h"

using namespace pq;
using namespace pq::meta;

namespace {

namespace fs = std::filesystem;

fs::path writeTemp(const std::string& name, const std::string& text) {
  const fs::path p = fs::temp_directory_path() / ("pq_test_env_" + name);
  std::ofstream(p, std::ios::binary) << text;
  return p;
}

std::vector<float> render(const fs::path& file, const std::string& notes, double seconds,
                          std::vector<std::pair<std::string, float>> params = {},
                          pqc::Backend backend = pqc::Backend::Interpreter) {
  pqc::RenderOptions o;
  o.notes = notes;
  o.seconds = seconds;
  o.params = std::move(params);
  o.backend = backend;
  engine::BufferSink sink;
  auto r = pqc::renderFile(file, o, sink);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  REQUIRE(sink.channels.size() == 1);
  return sink.channels[0];
}

Expansion mustExpand(const ExpandContext& c) {
  auto e = expand(envelopeType(), c, "x");
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  return std::move(e.value());
}

const char* kEnv = R"(patch Env {
  out out : audio voice mono
  env1 = meta_envelope()
  out = env1
}
)";

std::string refText() {
  const std::string msenv = (fs::path(PQ_TEST_DATA_DIR) / ".." / "stdlib" / "envelopes" / "msenv.pqp").generic_string();
  return "import \"" + msenv + R"("
patch Ref {
  param a   = 5ms   [0ms..60s] @unit(s)
  param d   = 400ms [1ms..60s] @unit(s)
  param r   = 250ms [1ms..60s] @unit(s)
  param sus = 0.6   [0..1]
  out out : audio voice mono
  out = msenv(voice.gate, a: a, d: d, r: r, sus: sus)
}
)";
}

constexpr double kSr = 48000.0;

// A sink that keeps nothing (timings).
class Discard final : public engine::AudioSink {
public:
  Status begin(uint32_t, uint32_t, uint64_t) override { return Ok{}; }
  Status write(const float* const*, uint32_t) override { return Ok{}; }
  Status end() override { return Ok{}; }
};
size_t at(double seconds) { return static_cast<size_t>(std::llround(seconds * kSr)); }

}  // namespace

TEST_CASE("envelope: expansion — forms, done, ids, the text form parses back, the tap") {
  const Type& t = envelopeType();
  CHECK(findType("envelope") == &t);
  CHECK(t.voiceEnd);
  CHECK_FALSE(t.chainInstrument);
  CHECK(t.chainVoiceInput == t.inputIndex("in"));
  ExpandContext c;
  const Expansion a = mustExpand(c), b = mustExpand(c);
  CHECK(a.patch == b.patch);
  CHECK(a.key == b.key);
  REQUIRE(a.resources.size() == 1);
  CHECK(a.resources[0].kind == ResourceKind::VoiceTap);
  // Nested: inputs in (1) and gate (voice.gate), outputs out and done.
  REQUIRE(a.patch.findByName("in"));
  REQUIRE(a.patch.findByName("gate"));
  REQUIRE(a.patch.findByName("done"));
  CHECK(a.patch.node(*a.patch.findByName("done")).id.value == kOutputIdBase + 1);
  CHECK(a.patch.node(*a.patch.findByName("attack")).id.value == kParamIdBase);
  const std::string text = graph::print(a.patch);
  CAPTURE(text);
  auto parsed = graph::parse(text, "expansion.pqp");
  REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
  REQUIRE(parsed->patches.size() == 1);
  CHECK(parsed->patches[0] == a.patch);
  // The chain voice form: `in` is the run's voice signal, done an output; no instrument form.
  ExpandContext v;
  v.form = Form::ChainVoice;
  const Expansion cv = mustExpand(v);
  CHECK(cv.patch.findByName("done").has_value());
  CHECK_FALSE(cv.patch.findByName("gate").has_value());  // reads its default (voice.gate)
  ExpandContext ins;
  ins.form = Form::ChainInstrument;
  CHECK_FALSE(expand(t, ins, "x").ok());
  // The prelude has the nested and the voice forms.
  const auto pre = prelude();
  const Prelude::Entry* e = pre->find("envelope");
  REQUIRE(e != nullptr);
  CHECK(e->voice.has_value());
  CHECK_FALSE(e->instrument.has_value());
  // The tap's encoding.
  for (const double lvl : {0.0, 0.25, 0.6, 1.0})
    for (const bool rel : {false, true}) {
      const EnvelopeTap d = decodeEnvelopeTap(static_cast<float>(encodeEnvelopeTap(lvl, rel)));
      CHECK(d.released == rel);
      CHECK(d.level == doctest::Approx(lvl).epsilon(1e-6));
    }
}

TEST_CASE("envelope: every curve 0 renders bit-identical to msenv (mode None) on both backends") {
  const fs::path env = writeTemp("env.pqp", kEnv), ref = writeTemp("ref.pqp", refText());
  struct Set {
    float a, d, sus, r;
  };
  // Released inside the attack, inside the decay, after sustaining; overlapping voices; an instant attack.
  const std::string notes = "57@0:0.02 60@0.05:0.15 64@0.1:0.6 69@0.3:0.25 72@0.45:0.05";
  for (const Set s : {Set{0.05f, 0.2f, 0.5f, 0.1f}, Set{0.0f, 0.03f, 0.0f, 0.4f}, Set{0.3f, 1.5f, 1.0f, 0.002f}}) {
    for (const pqc::Backend b : {pqc::Backend::Interpreter, pqc::Backend::Jit}) {
      CAPTURE(compile::backendLabel(b));
      CAPTURE(s.a);
      const std::vector<float> y = render(env, notes, 1.2,
                                          {{"env1__attack", s.a}, {"env1__decay", s.d}, {"env1__sustain", s.sus},
                                           {"env1__release", s.r}},
                                          b);
      const std::vector<float> x = render(ref, notes, 1.2, {{"a", s.a}, {"d", s.d}, {"sus", s.sus}, {"r", s.r}}, b);
      REQUIRE(y.size() == x.size());
      size_t diff = 0, nonzero = 0, first = y.size();
      for (size_t i = 0; i < y.size(); ++i) {
        if (y[i] != x[i] && first == y.size()) first = i;
        diff += y[i] != x[i] ? 1u : 0u;
        nonzero += y[i] != 0.0f ? 1u : 0u;
      }
      CAPTURE(first);
      CHECK(diff == 0);
      CHECK(nonzero > y.size() / 4);
    }
  }
  fs::remove(env);
  fs::remove(ref);
}

TEST_CASE("envelope: curves follow the law the face draws; goals reached at their times") {
  const fs::path env = writeTemp("envc.pqp", kEnv);
  const double a = 0.1, d = 0.2, sus = 0.4, r = 0.15;
  for (const double c : {-0.8, -0.3, 0.3, 0.8}) {
    CAPTURE(c);
    const std::vector<float> y =
        render(env, "69@0:0.5", 0.8,
               {{"env1__attack", float(a)}, {"env1__decay", float(d)}, {"env1__sustain", float(sus)},
                {"env1__release", float(r)}, {"env1__acurve", float(c)}, {"env1__dcurve", float(c)},
                {"env1__rcurve", float(c)}});
    const EnvSegmentLaw al = envAttackLaw(c), fl = envFallLaw(c);
    // Attack: on the law (within float rounding of the recursion), concave above the line for c > 0, convex below.
    for (const double u : {0.1, 0.25, 0.5, 0.75, 0.9}) {
      const double want = envAttackLevel(al, u), got = y[at(u * a)];
      CHECK(got == doctest::Approx(want).epsilon(2e-3).scale(1.0));
      if (c > 0) CHECK(got > u);
      else CHECK(got < u);
    }
    // The peak is reached at time a (within a sample; the decay starts on the next one).
    float peak = 0.0f;
    for (size_t i = at(a) - 2; i <= at(a) + 1; ++i) peak = std::max(peak, y[i]);
    CHECK(peak == 1.0f);
    CHECK(y[at(a) - 3] < 1.0f);
    // Decay toward the sustain, from 1 at time a.
    for (const double u : {0.2, 0.5, 0.8}) {
      const double want = envFallLevel(fl, 1.0, sus, 1.0 - sus, u), got = y[at(a + u * d)];
      CHECK(got == doctest::Approx(want).epsilon(3e-3).scale(1.0));
    }
    // With c > 0 the sustain is reached at time d (an overshoot; within float32's grid near the level, whose rounding
    // of the last per-sample steps delays the very end by a few dozen samples), then held exactly.
    if (c > 0) {
      CHECK(std::abs(y[at(a + d)] - sus) <= 1e-4);
      CHECK(y[at(a + d + 0.01)] == static_cast<float>(sus));
    }
    // Release from the sustain at 0.5 s (span 1: the full scale).
    for (const double u : {0.1, 0.3}) {
      const double want = envFallLevel(fl, sus, 0.0, 1.0, u), got = y[at(0.5 + u * r)];
      CHECK(got == doctest::Approx(want).epsilon(3e-3).scale(1.0));
    }
    CHECK(y[at(0.5 + r) + 2] <= 1.5e-4);
    CHECK(y.back() == 0.0f);
  }
  fs::remove(env);
}

TEST_CASE("envelope: cost per voice-sample against msenv (JIT, 16 held voices)" * doctest::test_suite("perf")) {
  // The JIT's code does not depend on the host build's preset (LLVM generates it), so this measures the kernel the
  // engine runs; wall-clock, so load-sensitive (perf label).
  const fs::path env = writeTemp("envb.pqp", kEnv), ref = writeTemp("refb.pqp", refText());
  const fs::path gate = writeTemp("gateb.pqp", "patch G {\n  out out : audio voice mono\n  out = gate_to_real(voice.gate)\n}\n");
  std::string notes;
  for (int k = 0; k < 16; ++k) notes += std::to_string(40 + 3 * k) + "@0:30 ";
  auto best = [&](const fs::path& f, std::vector<std::pair<std::string, float>> params, uint32_t& ops) {
    double t = 1e30;
    for (int i = 0; i < 3; ++i) {
      pqc::RenderOptions o;
      o.notes = notes;
      o.seconds = 20.0;
      o.params = params;
      o.backend = pqc::Backend::Jit;
      Discard sink;
      auto r = pqc::renderFile(f, o, sink);
      REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
      ops = r->ops;
      t = std::min(t, r->stats.wallSeconds);
    }
    return t * 1e9 / (20.0 * kSr * 16.0);  // ns per voice-sample
  };
  uint32_t opsE = 0, opsC = 0, opsR = 0, opsG = 0;
  const double e = best(env, {}, opsE);
  const double c = best(env, {{"env1__acurve", 0.5f}, {"env1__dcurve", -0.5f}, {"env1__rcurve", 0.5f}}, opsC);
  const double r = best(ref, {}, opsR);
  const double g = best(gate, {}, opsG);
  MESSAGE("ns/voice-sample: meta_envelope " << e << " (" << opsE << " ops), with curves " << c << ", msenv " << r << " ("
                                            << opsR << " ops), gate alone " << g << " (" << opsG << " ops)");
  CHECK(e > 0.0);
  fs::remove(env);
  fs::remove(ref);
  fs::remove(gate);
}

TEST_CASE("envelope: early releases, retriggers and curve drags never click; in is a VCA; amount scales") {
  const fs::path env = writeTemp("envr.pqp", kEnv);
  // Released halfway up a curved attack and halfway down a curved decay: the largest step stays a few per mille.
  for (const double c : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
    CAPTURE(c);
    const std::vector<float> y = render(env, "69@0:0.05 72@0.3:0.17", 0.8,
                                        {{"env1__attack", 0.1f}, {"env1__decay", 0.2f}, {"env1__sustain", 0.3f},
                                         {"env1__release", 0.1f}, {"env1__acurve", float(c)}, {"env1__dcurve", float(c)},
                                         {"env1__rcurve", float(c)}});
    float worst = 0.0f;
    for (size_t i = 1; i < y.size(); ++i) worst = std::max(worst, std::abs(y[i] - y[i - 1]));
    CHECK(worst < 0.01f);
  }
  // amount scales; a wired `in` multiplies (a VCA).
  const fs::path vca = writeTemp("vca.pqp", R"(patch Vca {
  out out : audio voice mono
  env1 = meta_envelope(0.5, amount: 0.5)
  out = env1
}
)");
  const std::vector<float> base = render(env, "69@0:0.3", 0.5), half = render(vca, "69@0:0.3", 0.5);
  REQUIRE(base.size() == half.size());
  for (size_t i = 0; i < base.size(); i += 97) CHECK(half[i] == doctest::Approx(base[i] * 0.25f).epsilon(1e-6));
  fs::remove(env);
  fs::remove(vca);
}
