// NZ.1: the Noise engine (pq/meta/noise.h) — its expansion (forms, fixed ids, the text form parses back, settings keep
// the atom's state key), and renders through pqc on both backends:
//   white     the default (Rate at its top) is white noise in [−1, 1); interpreter == JIT bit for bit
//   settings  every clock / distribution / restart / voices combination: interpreter == JIT bit for bit
//   clock     a wired clock (a 10 Hz sine) overrides the clock setting: a step exactly at each upward zero crossing
//   sync      the Sync clock steps on the song's grid (1/16 at the transport's tempo)
//   voices    Per voice: the voices' values differ; Shared: identical; the Note clock draws a value per note-on
//   range     min / max map [−1, 1] onto [min, max]
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pq/compile/compile.h"
#include "pq/dsp/fft.h"
#include "pq/engine/offline_render.h"
#include "pq/graph/parse.h"
#include "pq/graph/patch_file.h"
#include "pq/meta/noise.h"
#include "pq/meta/registry.h"
#include "pq/pqc/pqc.h"

using namespace pq;
using namespace pq::meta;

namespace {

namespace fs = std::filesystem;

fs::path writeTemp(const std::string& name, const std::string& text) {
  const fs::path p = fs::temp_directory_path() / ("pq_test_noise_" + name);
  std::ofstream(p, std::ios::binary) << text;
  return p;
}

std::vector<float> render(const fs::path& file, double seconds, pqc::Backend backend = pqc::Backend::Interpreter,
                          std::vector<std::pair<std::string, float>> params = {}, const std::string& notes = {}) {
  pqc::RenderOptions o;
  o.patch = "R";  // the root (the file opens with the expansion)
  o.seconds = seconds;
  o.params = std::move(params);
  o.backend = backend;
  o.notes = notes;
  engine::BufferSink sink;
  auto r = pqc::renderFile(file, o, sink);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  REQUIRE(!sink.channels.empty());
  return sink.channels[0];
}

Expansion mustExpand(const ExpandContext& c, const std::string& name = "x") {
  auto e = expand(noiseType(), c, name);
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  return std::move(e.value());
}

// The expansion with `settings` (voice context or not) as a subcircuit `nz`, called by a root patch. (A text call
// `meta_noise()` is the prelude's nested form, expanded in a voice context: its note-ons make it per voice, so a global
// patch here calls an expansion made out of one, as the document lowering does for a module outside an instrument
// group.) Voiced: the root's
// output is the spread of the voices' values, voice_highest − voice_lowest (n · gate is per voice: a Per voice
// instance runs per voice).
std::string patchWith(const std::vector<double>& settings, bool voiced, const std::string& root) {
  ExpandContext c;
  c.voice = voiced;
  c.settings = settings;
  return graph::print(mustExpand(c, "nz").patch) + "\n" + root;
}
const char* kRootGlobal = R"(patch R {
  out out : audio
  n = nz()
  out = n
}
)";
std::string globalCall(const std::string& call) {
  return patchWith({0.0, 0.0, 0.0, 0.0}, false, "patch R {\n  out out : audio\n  n = " + call + "\n  out = n\n}\n");
}
const char* kRootSpread = R"(patch R {
  out out : audio
  m = nz()
  n = m * gate_to_real(voice.gate)
  out = voice_highest(n) - voice_lowest(n)
}
)";

size_t changes(const std::vector<float>& y, size_t from = 1) {
  size_t c = 0;
  for (size_t i = std::max<size_t>(from, 1); i < y.size(); ++i) c += y[i] != y[i - 1];
  return c;
}

}  // namespace

TEST_CASE("noise: expansion — forms, fixed ids, settings keep the atom's id, the text form parses back") {
  const Type& t = noiseType();
  CHECK(findType("noise") == &t);
  CHECK(t.chainInput == t.inputIndex("clock"));
  CHECK(t.inputs[0].def == InputDefault::Unwired);
  ExpandContext c;
  const Expansion a = mustExpand(c), b = mustExpand(c);
  CHECK(a.patch == b.patch);
  REQUIRE(a.resources.size() == 1);
  CHECK(a.resources[0].kind == ResourceKind::VoiceTap);
  auto stepId = [](const Expansion& e) {
    for (graph::NodeIndex n = 0; n < e.patch.nodeCount(); ++n)
      if (e.patch.node(n).kind == graph::NodeKind::Atom && graph::atomSignature(e.patch.node(n).atom).name == "noise_step")
        return e.patch.node(n).id.value;
    return 0u;
  };
  CHECK(stepId(a) == kBodyIdBase + 0);
  const std::string text = graph::print(a.patch);
  CAPTURE(text);
  auto parsed = graph::parse(text, "expansion.pqp");
  REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
  REQUIRE(parsed->patches.size() == 1);
  CHECK(parsed->patches[0] == a.patch);
  // Every setting combination expands, the atom keeps its id (its state survives a setting change), keys differ.
  std::vector<uint64_t> keys;
  for (double clock : {0.0, 1.0, 2.0})
    for (double dist : {0.0, 1.0})
      for (double restart : {0.0, 1.0, 2.0})
        for (double voices : {0.0, 1.0})
          for (bool voice : {false, true}) {
            const std::vector<double> s = {clock, dist, restart, voices};
            ExpandContext x;
            x.settings = s;
            x.voice = voice;
            const Expansion e = mustExpand(x);
            CHECK(stepId(e) == kBodyIdBase + 0);
            keys.push_back(e.key);
          }
  std::sort(keys.begin(), keys.end());
  CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
  // The prelude has every form; the sync grid's values.
  const auto pre = prelude();
  const Prelude::Entry* e = pre->find("noise");
  REQUIRE(e != nullptr);
  CHECK(e->voice.has_value());
  CHECK(e->notes.has_value());
  CHECK(e->audio.has_value());
  CHECK(noiseSyncBeats(kNoiseSyncDefault) == 0.25);  // 1/16
  CHECK(noiseSyncBeats(0) == doctest::Approx(1.0 / 24.0));
  CHECK(noiseSyncBeats(26) == 24.0);  // 4/1 dotted
}

TEST_CASE("noise: the default is white noise; interpreter == JIT") {
  const fs::path f = writeTemp("white.pqp", globalCall("nz()"));
  const std::vector<float> y = render(f, 0.5), j = render(f, 0.5, pqc::Backend::Jit);
  REQUIRE(y == j);
  double m = 0, v = 0;
  for (float x : y) {
    REQUIRE(x >= -1.0f);
    REQUIRE(x < 1.0f);
    m += x, v += x * x;
  }
  m /= y.size(), v /= y.size();
  CHECK(std::abs(m) < 0.01);
  CHECK(std::abs(v - 1.0 / 3.0) < 0.01);
  CHECK(changes(y) > y.size() * 99 / 100);
  fs::remove(f);
}

TEST_CASE("noise: every setting combination, interpreter == JIT bit for bit" * doctest::test_suite("slow")) {
  for (double clock : {0.0, 1.0, 2.0})
    for (double dist : {0.0, 1.0})
      for (double restart : {0.0, 1.0, 2.0})
        for (double voices : {0.0, 1.0}) {
          CAPTURE(clock);
          CAPTURE(dist);
          CAPTURE(restart);
          CAPTURE(voices);
          const fs::path f = writeTemp("combo.pqp", patchWith({clock, dist, restart, voices}, true, kRootSpread));
          const std::vector<std::pair<std::string, float>> params = {
              {"m__rate", 37.0f}, {"m__smooth", 0.5f}, {"m__colour", -3.0f}, {"m__steps", 9.0f}, {"m__density", 300.0f}};
          const std::string notes = "60@0:0.3 64@0.05:0.3 67@0.12:0.2";
          const std::vector<float> y = render(f, 0.4, pqc::Backend::Interpreter, params, notes);
          const std::vector<float> j = render(f, 0.4, pqc::Backend::Jit, params, notes);
          REQUIRE(y.size() == j.size());
          size_t diff = 0;
          for (size_t i = 0; i < y.size(); ++i) diff += y[i] != j[i];
          CHECK(diff == 0);
          fs::remove(f);
        }
}

TEST_CASE("noise: a wired clock overrides the clock setting (a step at each upward zero crossing)") {
  // The clock: a 10 Hz sine (its upward crossings at 0, 0.1, 0.2 … s); the noise's own rate (its top) is cut.
  const fs::path f = writeTemp("clock.pqp", globalCall("nz(sine(phasor(10Hz)))"));
  const std::vector<float> y = render(f, 1.0), j = render(f, 1.0, pqc::Backend::Jit);
  REQUIRE(y == j);
  std::vector<size_t> at;
  for (size_t i = 1; i < y.size(); ++i)
    if (y[i] != y[i - 1]) at.push_back(i);
  // sine(phasor(10 Hz)): above 0 on the first half of each cycle, so it rises above 0 one sample into each cycle.
  REQUIRE(at.size() >= 9);
  REQUIRE(at.size() <= 10);
  // (the first sample with sin > 0 after each wrap: 1, 4801, … within a sample of the phasor's rounding)
  for (size_t k = 0; k < at.size(); ++k) CHECK(std::abs(static_cast<double>(at[k]) - 4800.0 * static_cast<double>(k) - 1.0) <= 1.0);
  fs::remove(f);
}

TEST_CASE("noise: Sync steps on the song's grid; Note draws per note-on; Per voice differ, Shared identical") {
  {  // Sync with the transport stopped (pqc's offline render): the beat stands, the value holds (the grid while
     // playing: tests/app/test_noise_module.cpp)
    const fs::path f = writeTemp("sync.pqp", patchWith({1.0, 0.0, 1.0, 0.0}, false, kRootGlobal));
    const std::vector<float> y = render(f, 0.5), j = render(f, 0.5, pqc::Backend::Jit);
    REQUIRE(y == j);
    CHECK(changes(y) == 0);
    fs::remove(f);
  }
  const std::string chord = "60@0:0.3 64@0:0.3 67@0:0.3";
  for (const double voices : {0.0, 1.0}) {
    CAPTURE(voices);
    const fs::path f = writeTemp("voices.pqp", patchWith({0.0, 0.0, 1.0, voices}, true, kRootSpread));
    const std::vector<float> y = render(f, 0.25, pqc::Backend::Interpreter, {{"m__rate", 50.0f}}, chord);
    float spread = 0.0f;
    for (size_t i = 2400; i < y.size(); ++i) spread = std::max(spread, std::abs(y[i]));
    if (voices == 0.0) CHECK(spread > 0.05f);  // Per voice: lanes seeded apart
    else CHECK(spread == 0.0f);               // Shared: one stream
    fs::remove(f);
  }
  {  // Note clock, per voice: a value per note-on, held (the voices' spread changes only when a note starts)
    const fs::path f = writeTemp("note.pqp", patchWith({2.0, 0.0, 1.0, 0.0}, true, kRootSpread));
    const std::vector<float> y = render(f, 0.5, pqc::Backend::Interpreter, {}, "60@0:0.45 64@0.1:0.35 67@0.2:0.25");
    std::vector<size_t> at;
    for (size_t i = 1; i < y.size(); ++i)
      if (y[i] != y[i - 1]) at.push_back(i);
    // the spread moves when the second and third notes start (and when voices end); nothing in between
    for (size_t k : at) {
      const double s = static_cast<double>(k) / 48000.0;
      CHECK((std::abs(s - 0.1) < 0.01 || std::abs(s - 0.2) < 0.01 || s > 0.44));
    }
    fs::remove(f);
  }
}

TEST_CASE("noise: blue and violet through the module keep their slope and stay in range (Q?-NZ.1-a: scaled, not folded)") {
  // The module's whole path (the expansion, its Range at −1 … 1) at Rate's top, 2^20 samples; a Welch power spectrum
  // (4096-point Hann, 50 % overlap) summed in third-octave bands 100 Hz … 16 kHz: each band's level minus the target
  // slope within ±1 dB of their mean (the design's ±0.42 dB plus the estimate's scatter).
  constexpr uint32_t kN = 4096;
  pq::dsp::RealFft fft(kN);
  std::vector<float> win(kN), seg(kN), pw(kN / 2 + 1);
  for (uint32_t i = 0; i < kN; ++i) win[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * i / kN));
  for (const double colour : {3.0, 6.0}) {
    CAPTURE(colour);
    const fs::path f = writeTemp("blue.pqp", globalCall("nz(colour: " + std::to_string(colour) + ")"));
    const std::vector<float> y = render(f, (1 << 20) / 48000.0);
    fs::remove(f);
    float peak = 0.0f;
    for (float v : y) peak = std::max(peak, std::fabs(v));
    CHECK(peak <= 1.0f);
    CHECK(peak > 0.3f);
    std::vector<double> psd(kN / 2 + 1, 0.0);
    uint32_t segs = 0;
    for (size_t at = 0; at + kN <= y.size(); at += kN / 2, ++segs) {
      for (uint32_t i = 0; i < kN; ++i) seg[i] = y[at + i] * win[i];
      fft.power(seg.data(), pw.data());
      for (uint32_t k = 0; k <= kN / 2; ++k) psd[k] += pw[k];
    }
    REQUIRE(segs > 100);
    std::vector<double> dev;
    for (double lo = 100.0; lo * std::cbrt(2.0) <= 16000.0; lo *= std::cbrt(2.0)) {
      const double hi = lo * std::cbrt(2.0), binHz = 48000.0 / kN;
      double sum = 0.0;
      uint32_t bins = 0;
      for (auto k = static_cast<uint32_t>(std::ceil(lo / binHz)); k * binHz < hi; ++k) sum += psd[k], ++bins;
      REQUIRE(bins > 0);
      dev.push_back(10.0 * std::log10(sum / bins) - colour * std::log2(std::sqrt(lo * hi)));
    }
    const auto [mn, mx] = std::minmax_element(dev.begin(), dev.end());
    MESSAGE("colour " << colour << ": slope deviation ±" << (*mx - *mn) / 2.0 << " dB over " << dev.size() << " bands");
    CHECK((*mx - *mn) / 2.0 < 1.0);
  }
}

TEST_CASE("noise: min / max map [−1, 1] onto the range") {
  const fs::path f = writeTemp("range.pqp", globalCall("nz(min: 0.25, max: 0.75)"));
  const std::vector<float> y = render(f, 0.2);
  float lo = 1, hi = 0;
  for (float x : y) lo = std::min(lo, x), hi = std::max(hi, x);
  CHECK(lo >= 0.25f);
  CHECK(hi <= 0.75f);
  CHECK(lo < 0.26f);
  CHECK(hi > 0.74f);
  fs::remove(f);
}
