// TMM.12: the Meta Operator (stdlib/devices/meta_operator.pqp) against its C++ reference (meta_operator_reference.h)
// on the interpreter and the JIT: every algorithm with four sine operators at exact ratios, modulator depths, feedback,
// envelopes through the release and the voice's end; its pitch exact in 31edo and a MOS (voice.hz · n / d, n / d
// exact just ratios, and n = 0 a fixed frequency); hq = 2 compiles the operators oversampled ×2 and stays close to the
// plain rendering below the band edge. Rendered standalone through pqc (the meta modules expand with their default
// state: a sine table), nested params set as runtime params (`envB__amount`).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "meta_operator_reference.h"
#include "pq/engine/offline_render.h"
#include "pq/pqc/pqc.h"
#include "pq/tuning/mos_tuning.h"
#include "pq/tuning/tuning.h"

namespace fs = std::filesystem;

namespace {

constexpr double kSr = 48000.0;

std::string deviceText() {
  std::ifstream in(fs::path(PQ_TEST_DATA_DIR) / ".." / "stdlib" / "devices" / "meta_operator.pqp", std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A wrapper calling the device with its structural settings (D's feedback loop on) (as a track's lowering does: literals into its `in`s), so
// its params and nested instances are `o__nA`, `o__envB__amount`.
fs::path variant(int algorithm, int hq) {
  const std::string dev = (fs::path(PQ_TEST_DATA_DIR) / ".." / "stdlib" / "devices" / "meta_operator.pqp").generic_string();
  const std::string t = "import \"" + dev + "\"\npatch T {\n  out y : audio voice mono\n  out done : bool voice mono\n  o = meta_operator(algorithm: " +
                        std::to_string(algorithm) + ", hq: " + std::to_string(hq) + ", fbOn: 1)\n  y = o\n  done = o.done\n}\n";
  const fs::path p = fs::temp_directory_path() / ("pq_test_mo_" + std::to_string(algorithm) + "_" + std::to_string(hq) + ".pqp");
  std::ofstream(p, std::ios::binary) << t;
  return p;
}

using Params = std::vector<std::pair<std::string, float>>;

std::vector<float> render(const fs::path& file, const std::string& notes, double seconds, const Params& params,
                          pq::pqc::Backend backend) {
  pq::pqc::RenderOptions o;
  o.notes = notes;
  o.seconds = seconds;
  o.params = params;
  o.backend = backend;
  pq::engine::BufferSink sink;
  auto r = pq::pqc::renderFile(file, o, sink);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  REQUIRE(sink.channels.size() == 1);
  return sink.channels[0];
}

// The settings every reference case shares: ratios A 1, B 2, C 3/2, D 7/4; modulator depths and short envelopes;
// D detuned +1.5 Hz on its accumulator; feedback.
struct Case {
  moref::Voice ref;
  Params params;
};

Case standardCase(int algorithm, float feedback) {
  Case c;
  c.ref.algorithm = algorithm;
  c.ref.feedback = feedback;
  const float n[4] = {1, 2, 3, 7}, d[4] = {1, 1, 2, 4};
  const float amount[4] = {0.8f, 0.4f, 0.3f, 0.2f};
  const float decay[4] = {0.5f, 0.25f, 0.3f, 0.15f}, sustain[4] = {0.6f, 0.3f, 0.5f, 0.2f};
  const float release[4] = {0.1f, 0.08f, 0.12f, 0.06f};
  const char* ops = "ABCD";
  for (int i = 0; i < 4; ++i) {
    moref::OpState& o = c.ref.op[static_cast<size_t>(i)];
    o.n = n[i];
    o.d = d[i];
    o.amount = amount[i];
    o.decay = decay[i];
    o.sustain = sustain[i];
    o.release = release[i];
    o.attack = 0.002f * static_cast<float>(i + 1);
    const std::string X(1, ops[i]);
    c.params.push_back({"o__n" + X, n[i]});
    c.params.push_back({"o__d" + X, d[i]});
    c.params.push_back({"o__env" + X + "__amount", amount[i]});
    c.params.push_back({"o__env" + X + "__decay", decay[i]});
    c.params.push_back({"o__env" + X + "__sustain", sustain[i]});
    c.params.push_back({"o__env" + X + "__release", release[i]});
    c.params.push_back({"o__env" + X + "__attack", o.attack});
  }
  c.ref.op[3].freq = 1.5f;
  c.params.push_back({"o__accD__freq", 1.5f});
  c.params.push_back({"o__feedback", feedback});
  return c;
}

// One note (220 Hz, velocity 0.8, 0.3 s) rendered by the reference for `frames` frames.
std::vector<float> reference(moref::Voice v, size_t frames, size_t noteFrames, size_t* doneAt = nullptr) {
  std::vector<float> y(frames, 0.0f);
  // The engine ends a voice at the end of the 64-frame block in which its `done` rose.
  bool ended = false;
  for (size_t t = 0; t < frames; ++t) {
    bool done = false;
    y[t] = v.tick(220.0f, 0.8f, t < noteFrames, done);
    if (done && !ended) {
      ended = true;
      if (doneAt) *doneAt = t;
    }
    if (ended && (t + 1) % 64 == 0) break;
  }
  return y;
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
  double m = 0.0;
  for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
  return m;
}

// Frequency of a sine by zero crossings with linear interpolation, over [from, end).
double measureHz(const std::vector<float>& y, size_t from) {
  double first = -1.0, last = -1.0;
  int n = 0;
  for (size_t t = from + 1; t < y.size(); ++t)
    if (y[t - 1] < 0.0f && y[t] >= 0.0f) {
      const double x = static_cast<double>(t - 1) + y[t - 1] / (static_cast<double>(y[t - 1]) - y[t]);
      if (first < 0.0) first = x;
      last = x;
      ++n;
    }
  return n > 1 ? (n - 1) * kSr / (last - first) : 0.0;
}

}  // namespace

TEST_CASE("Meta Operator: every algorithm vs the C++ reference (sines, exact ratios, depths, feedback, envelopes), both backends") {
  constexpr double kSeconds = 0.6;
  const size_t frames = static_cast<size_t>(kSeconds * kSr), noteFrames = static_cast<size_t>(0.3 * kSr);
  double worst = 0.0;
  for (int alg = 1; alg <= 11; ++alg) {
    CAPTURE(alg);
    const Case c = standardCase(alg, alg % 2 == 0 ? 0.35f : 0.0f);
    const fs::path file = variant(alg, 1);
    const std::vector<float> want = reference(c.ref, frames, noteFrames);
    const std::vector<float> interp = render(file, "220hz@0:0.3*0.8", kSeconds, c.params, pq::pqc::Backend::Interpreter);
    const std::vector<float> jit = render(file, "220hz@0:0.3*0.8", kSeconds, c.params, pq::pqc::Backend::Jit);
    REQUIRE(interp.size() >= frames);
    REQUIRE(jit.size() >= frames);
    double peak = 0.0;
    for (const float v : want) peak = std::max(peak, std::fabs(static_cast<double>(v)));
    CHECK(peak > 0.1);  // it plays
    const double di = maxDiff(interp, want), dj = maxDiff(jit, want);
    CHECK(di <= 1e-5);
    CHECK(dj <= 1e-5);
    worst = std::max({worst, di, dj});
    // The voice ends with its carriers' release: silence after it.
    double tail = 0.0;
    for (size_t t = frames - 2400; t < frames; ++t) tail = std::max(tail, std::fabs(static_cast<double>(interp[t])));
    CHECK(tail == 0.0);
  }
  MESSAGE("Meta Operator vs reference: worst |diff| " << worst << " over 11 algorithms, both backends");
}

TEST_CASE("Meta Operator: pitch is voice.hz · n / d exactly — 31edo and a MOS, just ratios, n = 0 a fixed frequency") {
  namespace tun = pq::tuning;
  const fs::path file = variant(1, 1);
  // Only A sounding (the modulators at depth 0): a sine at voice.hz · nA / dA.
  auto play = [&](double hz, float n, float d, float freq = 0.0f, pq::pqc::Backend b = pq::pqc::Backend::Interpreter) {
    Params p{{"o__nA", n}, {"o__dA", d}, {"o__accA__freq", freq}, {"o__envA__sustain", 1.0f}};
    for (const char* m : {"o__envB__amount", "o__envC__amount", "o__envD__amount"}) p.push_back({m, 0.0f});
    char tok[64];
    std::snprintf(tok, sizeof tok, "%.9ghz@0:0.5", hz);
    const std::vector<float> y = render(file, tok, 0.5, p, b);
    return measureHz(y, 2400);
  };
  const double ref = 261.626;
  const tun::Tuning edo = tun::Tuning::make(tun::Edo::make(31).value(), ref).value();
  double worst = 0.0;
  for (const auto b : {pq::pqc::Backend::Interpreter, pq::pqc::Backend::Jit})
    for (int32_t deg = -31; deg <= 31; deg += 10) {
      const auto hz = tun::resolveHzDouble(tun::PitchId::fromDegree(deg, 0), edo, {});
      REQUIRE(hz);
      const double voiceHz = static_cast<float>(*hz);  // voice.hz is float32 (SPEC §3.1)
      for (const auto& [n, d] : {std::pair{1.0f, 1.0f}, {3.0f, 2.0f}, {7.0f, 4.0f}, {5.0f, 3.0f}}) {
        const double want = voiceHz * n / d;
        const double got = play(voiceHz, n, d, 0.0f, b);
        worst = std::max(worst, std::fabs(1200.0 * std::log2(got / want)));
      }
    }
  tun::MosParams mp;
  mp.n = 7;
  mp.period = 1200.0;
  mp.rotation = 1;
  mp.generator = 696.0;
  const tun::Tuning mos = tun::Tuning::make(tun::Mos::make(mp).value(), ref).value();
  for (int32_t deg = 0; deg < 7; ++deg) {
    const auto hz = tun::resolveHzDouble(tun::PitchId::fromDegree(deg, deg == 3 ? 1 : 0), mos, {});
    REQUIRE(hz);
    const double voiceHz = static_cast<float>(*hz);
    const double got = play(voiceHz, 3.0f, 2.0f);
    worst = std::max(worst, std::fabs(1200.0 * std::log2(got / (voiceHz * 1.5))));
  }
  MESSAGE("31edo and MOS (5L2s at 696 c) × just ratios: max pitch error " << worst << " cents");
  CHECK(worst < 0.002);  // the phasor's uint32 increment: exact to 2^-32 cycles per sample (Operator's bound)
  // n = 0: the note is ignored; the accumulator's frequency is the pitch.
  CHECK(std::fabs(play(330.0, 0.0f, 1.0f, 440.0f) - 440.0) < 440.0 * 1e-6);
  CHECK(std::fabs(play(110.0, 0.0f, 1.0f, 440.0f) - 440.0) < 440.0 * 1e-6);
}

TEST_CASE("Meta Operator: hq = 2 runs the operators oversampled and matches the plain rendering at low index") {
  // A gentle patch (B → A at depth 0.05, low notes): its sidebands stay far below the band edge, so ×2 and ×1 agree
  // up to the half-bands' phase (compared by level, not sample by sample) and both play.
  Case c = standardCase(1, 0.0f);
  for (const char* m : {"o__envB__amount", "o__envC__amount", "o__envD__amount"})
    for (auto& [name, v] : c.params)
      if (name == m) v = name == std::string("o__envB__amount") ? 0.05f : 0.0f;
  const std::vector<float> plain = render(variant(1, 1), "110hz@0:0.3*0.8", 0.4, c.params, pq::pqc::Backend::Jit);
  const std::vector<float> hq = render(variant(1, 2), "110hz@0:0.3*0.8", 0.4, c.params, pq::pqc::Backend::Jit);
  auto rms = [](const std::vector<float>& y, size_t a, size_t b) {
    double s = 0.0;
    for (size_t t = a; t < b; ++t) s += static_cast<double>(y[t]) * y[t];
    return std::sqrt(s / static_cast<double>(b - a));
  };
  const double rp = rms(plain, 4800, 12000), rh = rms(hq, 4800, 12000);
  MESSAGE("rms plain " << rp << ", hq " << rh);
  CHECK(rp > 0.1);
  CHECK(std::fabs(rh / rp - 1.0) < 1e-3);
}
