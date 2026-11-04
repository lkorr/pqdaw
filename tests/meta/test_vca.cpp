// TMM.14: the VCA meta module — its expansion (forms, ids, the text form parses back for both curves, the tap), the
// curve law (endpoints exact, the −1 … +1 arc from 10 to 2 o'clock, continuous value and slope at the joins, monotonic
// for every state, ±20 k ends, log spacing, custom ranges incl. both-positive / both-negative / within the fine zone,
// the Bend curve, the inverse), the DSP against the law on both backends (interpreter == JIT bit for bit), the gain
// smoothing of a value step (no clicks), an audio-rate value per sample, and the cost (perf).
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "pq/engine/offline_render.h"
#include "pq/graph/parse.h"
#include "pq/graph/patch_file.h"
#include "pq/meta/registry.h"
#include "pq/meta/vca.h"
#include "pq/pqc/pqc.h"

using namespace pq;
using namespace pq::meta;

namespace {

namespace fs = std::filesystem;

fs::path writeTemp(const std::string& name, const std::string& text) {
  const fs::path p = fs::temp_directory_path() / ("pq_test_vca_" + name);
  std::ofstream(p, std::ios::binary) << text;
  return p;
}

std::vector<float> render(const fs::path& file, double seconds, std::vector<std::pair<std::string, float>> params = {},
                          pqc::Backend backend = pqc::Backend::Interpreter, const std::string& patch = {}) {
  pqc::RenderOptions o;
  o.seconds = seconds;
  o.params = std::move(params);
  o.backend = backend;
  o.patch = patch;
  engine::BufferSink sink;
  auto r = pqc::renderFile(file, o, sink);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  REQUIRE(sink.channels.size() == 1);
  return sink.channels[0];
}

Expansion mustExpand(const ExpandContext& c, const std::string& name = "x") {
  auto e = expand(vcaType(), c, name);
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  return std::move(e.value());
}

// A file whose patch `Main` calls the VCA expanded with `curve` (a structural setting: a standalone file plays the
// default, so the test prints the expansion itself and calls it), with `in` unwired (1): out = value.
fs::path curveFile(const std::string& name, VcaCurve curve, const std::string& inExpr = "") {
  ExpandContext c;
  const double s = static_cast<double>(curve);
  c.settings = std::span<const double>(&s, 1);
  const Expansion e = mustExpand(c, "VcaX");
  std::string text = graph::print(e.patch);
  text += "\npatch Main {\n  out out : audio mono\n  v = VcaX(" + inExpr + ")\n  out = v\n}\n";
  return writeTemp(name, text);
}

double slopeAt(const VcaLaw& l, double p, double h) { return (vcaValue(l, p + h) - vcaValue(l, p - h)) / (2.0 * h); }

}  // namespace

TEST_CASE("vca: expansion — every form, fixed ids, the text form parses back (both curves), the tap") {
  const Type& t = vcaType();
  CHECK(findType("vca") == &t);
  CHECK(t.chainInput == t.inputIndex("in"));
  CHECK(t.params.size() == kVcaParams);
  CHECK(t.paramIndex("value") == static_cast<int32_t>(kVcaValue));
  CHECK(t.settingIndex("curve") == static_cast<int32_t>(kVcaCurveSetting));
  for (const double curve : {0.0, 1.0}) {
    CAPTURE(curve);
    ExpandContext c;
    c.settings = std::span<const double>(&curve, 1);
    const Expansion a = mustExpand(c), b = mustExpand(c);
    CHECK(a.patch == b.patch);
    CHECK(a.key == b.key);
    REQUIRE(a.resources.size() == 1);
    CHECK(a.resources[0].kind == ResourceKind::VoiceTap);
    REQUIRE(a.patch.findByName("value"));
    CHECK(a.patch.node(*a.patch.findByName("value")).id.value == kParamIdBase + kVcaValue);
    CHECK(a.patch.node(*a.patch.findByName("min")).id.value == kParamIdBase + kVcaMin);
    REQUIRE(a.patch.findByName("tap"));
    CHECK(a.patch.node(*a.patch.findByName("tap")).id.value == kBodyIdBase + 1);  // fixed across curves
    const std::string text = graph::print(a.patch);
    CAPTURE(text);
    auto parsed = graph::parse(text, "expansion.pqp");
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    REQUIRE(parsed->patches.size() == 1);
    CHECK(parsed->patches[0] == a.patch);
  }
  // The curve changes the expansion (its key too); every chain form exists.
  const double bend = 1.0;
  ExpandContext cb;
  cb.settings = std::span<const double>(&bend, 1);
  CHECK(mustExpand(ExpandContext{}).key != mustExpand(cb).key);
  for (const Form f : {Form::ChainNotes, Form::ChainAudio, Form::ChainVoice}) {
    ExpandContext c;
    c.form = f;
    c.voice = f != Form::ChainAudio;
    const Expansion e = mustExpand(c);
    // At the note source `in` has no signal (Reads::Signal): it reads 1, the module is a value per voice.
    CHECK(e.patch.findByName("in").has_value() == (f != Form::ChainNotes));
  }
  const auto pre = prelude();
  const Prelude::Entry* e = pre->find("vca");
  REQUIRE(e != nullptr);
  CHECK(e->voice.has_value());
  CHECK(e->notes.has_value());
  CHECK(e->audio.has_value());
}

TEST_CASE("vca: the default Log curve — exact ends, the −1 … +1 arc from 10 to 2 o'clock, C1 joins, log spacing") {
  const VcaLaw l = vcaLaw(VcaCurve::Log, -kVcaRange, kVcaRange, 0.4, 0.0);
  CHECK(l.pN == doctest::Approx(0.3).epsilon(1e-12));
  CHECK(l.pP == doctest::Approx(0.7).epsilon(1e-12));
  CHECK(vcaValue(l, 0.0) == doctest::Approx(-20000.0).epsilon(1e-12));
  CHECK(vcaValue(l, 1.0) == doctest::Approx(20000.0).epsilon(1e-12));
  // The arc: linear −1 … +1 over 0.3 … 0.7 (exactly), 0 at 12 o'clock.
  for (int i = 0; i <= 40; ++i) {
    const double p = 0.3 + 0.4 * i / 40.0;
    CHECK(vcaValue(l, p) == doctest::Approx(-1.0 + 2.0 * i / 40.0).epsilon(1e-12).scale(1.0));
  }
  CHECK(vcaValue(l, 0.5) == doctest::Approx(0.0).scale(1.0).epsilon(1e-12));
  CHECK(vcaValue(l, kVcaDefaultPos) == doctest::Approx(1.0).epsilon(1e-12));  // the default: unity
  // Continuous value and slope at both joins (5 per unit of travel inside the arc).
  for (const double j : {0.3, 0.7}) {
    CHECK(vcaValue(l, j - 1e-9) == doctest::Approx(vcaValue(l, j + 1e-9)).epsilon(1e-6));
    CHECK(slopeAt(l, j - 1e-7, 5e-8) == doctest::Approx(5.0).epsilon(1e-3));
    CHECK(slopeAt(l, j + 1e-7, 5e-8) == doctest::Approx(5.0).epsilon(1e-3));
  }
  // Odd symmetry; log spacing toward the end: each quarter of the outer travel multiplies by ≈ 12 once past 10.
  for (const double p : {0.05, 0.12, 0.2, 0.29}) CHECK(vcaValue(l, p) == doctest::Approx(-vcaValue(l, 1.0 - p)).epsilon(1e-9));
  const double q[] = {vcaValue(l, 0.775), vcaValue(l, 0.85), vcaValue(l, 0.925), vcaValue(l, 1.0)};
  CHECK(q[0] == doctest::Approx(9.8).epsilon(0.02));
  for (int i = 1; i < 4; ++i) CHECK(q[i] / q[i - 1] == doctest::Approx(12.0).epsilon(0.15));
}

TEST_CASE("vca: every state's curve is continuous and monotonic, ends at min and max; the inverse round-trips") {
  std::mt19937 rng(14);
  std::uniform_real_distribution<double> range(-kVcaRange, kVcaRange), zone(kVcaZoneMin, kVcaZoneMax), bend(-1.0, 1.0);
  std::vector<std::array<double, 4>> states = {{-20000, 20000, 0.4, 0},  {20, 20000, 0.4, 0},   {-20000, -20, 0.4, 0},
                                               {0, 20000, 0.4, 0},       {-0.5, 2, 0.4, 0},      {-0.5, 0.5, 0.4, 0},
                                               {-1, 20000, 0.4, 0},      {-3, 1.5, 0.9, 0},      {1, 1000, 0.05, 0},
                                               {5, 5, 0.4, 0},           {300, -300, 0.4, 0},    {-20000, 20000, 0.05, 0}};
  for (int i = 0; i < 200; ++i) states.push_back({range(rng), range(rng), zone(rng), bend(rng)});
  for (int i = 0; i < 100; ++i) states.push_back({range(rng) * 1e-3, range(rng), zone(rng), bend(rng)});
  // TX.vca-knob: every state also with random marker offsets (any order: the law keeps them ordered).
  std::uniform_real_distribution<double> offset(-0.3, 0.3);
  std::vector<std::array<double, kVcaMarks>> offs(states.size());
  for (auto& o : offs)
    for (double& x : o) x = offset(rng);
  for (size_t si = 0; si < states.size(); ++si)
    for (const bool moved : {false, true})
    for (const VcaCurve curve : {VcaCurve::Log, VcaCurve::Bend}) {
      const auto& s = states[si];
      CAPTURE(moved);
      CAPTURE(s[0]);
      CAPTURE(s[1]);
      CAPTURE(s[2]);
      CAPTURE(s[3]);
      CAPTURE(static_cast<int>(curve));
      const VcaLaw l = moved ? vcaLaw(curve, s[0], s[1], s[2], s[3], offs[si]) : vcaLaw(curve, s[0], s[1], s[2], s[3]);
      const double lo = s[0], hi = std::max(s[0], s[1]);
      // Shown markers: in order with at least the gap (or their bases' own distance), each at its value exactly.
      double prevAt = 0.0, prevBase = 0.0;
      for (const VcaLaw::Mark& m : l.marks) {
        if (!m.shown) continue;
        CHECK(m.at >= prevAt + std::min(kVcaMarkGap, m.base - prevBase) - 1e-12);
        CHECK(m.at < 1.0);
        CHECK(vcaValue(l, m.at) == doctest::Approx(m.value).epsilon(1e-9));
        prevAt = m.at;
        prevBase = m.base;
      }
      CHECK(vcaValue(l, 0.0) == doctest::Approx(lo).epsilon(1e-9).scale(1.0));
      CHECK(vcaValue(l, 1.0) == doctest::Approx(hi).epsilon(1e-9).scale(1.0));
      bool monotonic = true;
      auto worstJump = [&](int n) {
        double prev = vcaValue(l, 0.0), worst = 0.0;
        for (int k = 1; k <= n; ++k) {
          const double v = vcaValue(l, static_cast<double>(k) / n);
          monotonic = monotonic && v >= prev - 1e-9 * std::max(1.0, std::abs(v));
          worst = std::max(worst, std::abs(v - prev));
          prev = v;
        }
        return worst;
      };
      // Continuous: the largest step between neighbouring positions halves with the grid (a jump would not shrink).
      const double j1 = worstJump(20000), j2 = worstJump(40000);
      CHECK(monotonic);
      CHECK(j2 <= 0.55 * j1 + 1e-9);
      for (const double p : {0.0, 0.13, 0.31, 0.5, 0.77, 0.99, 1.0}) {
        const double v = vcaValue(l, p);
        CHECK(vcaValue(l, vcaPosition(l, v)) == doctest::Approx(v).epsilon(1e-9).scale(1.0));
      }
    }
}

TEST_CASE("vca: custom ranges — one-sided log, the fine zone inside the range, the Bend curve's shapes") {
  // Both positive: pure log, the geometric middle at 12 o'clock.
  const VcaLaw pos = vcaLaw(VcaCurve::Log, 20.0, 20000.0, 0.4, 0.0);
  CHECK(vcaValue(pos, 0.5) == doctest::Approx(std::sqrt(20.0 * 20000.0)).epsilon(1e-9));
  CHECK(vcaValue(pos, 0.25) == doctest::Approx(20.0 * std::pow(1000.0, 0.25)).epsilon(1e-9));
  // Both negative: the mirror.
  const VcaLaw neg = vcaLaw(VcaCurve::Log, -20000.0, -20.0, 0.4, 0.0);
  CHECK(vcaValue(neg, 0.5) == doctest::Approx(-std::sqrt(20.0 * 20000.0)).epsilon(1e-9));
  CHECK(vcaValue(neg, 0.0) == doctest::Approx(-20000.0));
  // 0 … 20000: the fine zone 0 … 1 takes 0.2 of the travel (half of it lies in the range), linear there, C1 at 1.
  const VcaLaw half = vcaLaw(VcaCurve::Log, 0.0, 20000.0, 0.4, 0.0);
  CHECK(half.pP == doctest::Approx(0.2));
  CHECK(vcaValue(half, 0.1) == doctest::Approx(0.5));
  CHECK(slopeAt(half, 0.2 + 1e-5, 1e-6) == doctest::Approx(5.0).epsilon(1e-3));
  // A range inside ±1: the whole travel is linear.
  const VcaLaw small = vcaLaw(VcaCurve::Log, -0.5, 0.8, 0.4, 0.0);
  for (const double p : {0.0, 0.25, 0.6, 1.0}) CHECK(vcaValue(small, p) == doctest::Approx(-0.5 + 1.3 * p));
  // −2 … 2: the outers are short, still C1 (or the straight line when the join's slope would overshoot the end).
  const VcaLaw two = vcaLaw(VcaCurve::Log, -2.0, 2.0, 0.4, 0.0);
  CHECK(vcaValue(two, 0.3) == doctest::Approx(-1.0));
  CHECK(vcaValue(two, 0.7) == doctest::Approx(1.0));
  CHECK(vcaValue(two, 1.0) == doctest::Approx(2.0));
  // The zone param widens the arc.
  const VcaLaw wide = vcaLaw(VcaCurve::Log, -20000.0, 20000.0, 0.8, 0.0);
  CHECK(wide.pN == doctest::Approx(0.1));
  CHECK(vcaValue(wide, 0.9) == doctest::Approx(1.0));
  // Max below min: a constant min.
  const VcaLaw flat = vcaLaw(VcaCurve::Log, 300.0, -300.0, 0.4, 0.0);
  CHECK(vcaValue(flat, 0.0) == 300.0);
  CHECK(vcaValue(flat, 1.0) == 300.0);
  // Bend: 0 is linear, > 0 exponential (below the line), < 0 logarithmic (above it), ends exact.
  const VcaLaw lin = vcaLaw(VcaCurve::Bend, -2.0, 6.0, 0.4, 0.0);
  CHECK(vcaValue(lin, 0.25) == doctest::Approx(0.0));
  const VcaLaw ex = vcaLaw(VcaCurve::Bend, 0.0, 1.0, 0.4, 0.6), lg = vcaLaw(VcaCurve::Bend, 0.0, 1.0, 0.4, -0.6);
  CHECK(vcaValue(ex, 0.5) < 0.5);
  CHECK(vcaValue(lg, 0.5) > 0.5);
  CHECK(vcaValue(ex, 0.5) == doctest::Approx(std::expm1(4.8 * 0.5) / std::expm1(4.8)));
  CHECK(vcaValue(ex, 0.5) + vcaValue(lg, 0.5) == doctest::Approx(1.0));  // mirror images
}

TEST_CASE("vca: the markers (TX.vca-knob) — defaults are the old law exactly, each marker moves alone, ordering, hidden") {
  // No offsets: the base law, bit for bit (an older state has no marker params: they read 0, its sound is unchanged).
  const std::array<double, kVcaMarks> zero{};
  for (const auto& r : {std::array<double, 3>{-20000, 20000, 0.4}, {-50, 20000, 0.7}, {0, 20000, 0.4}, {-3, 1.5, 0.9},
                        {20, 20000, 0.4}, {-20000, 20000, 0.05}}) {
    CAPTURE(r[0]);
    CAPTURE(r[1]);
    const VcaLaw a = vcaLaw(VcaCurve::Log, r[0], r[1], r[2], 0.0), b = vcaLaw(VcaCurve::Log, r[0], r[1], r[2], 0.0, zero);
    CHECK_FALSE(a.warped);
    for (int i = 0; i <= 1000; ++i) {
      CHECK(vcaWarp(a, i / 1000.0) == i / 1000.0);
      CHECK(vcaValue(a, i / 1000.0) == vcaValue(b, i / 1000.0));
    }
    // The shown markers sit where the curve reaches their values (the Newton inverse to double precision).
    for (const VcaLaw::Mark& m : a.marks) {
      CHECK(m.shown == (r[0] < m.value && m.value < r[1]));
      if (!m.shown) continue;
      CHECK(m.at == m.base);
      CHECK(vcaValue(a, m.base) == doctest::Approx(m.value).epsilon(1e-12));
      CHECK(vcaPosition(a, m.value) == doctest::Approx(m.base).epsilon(1e-12));
    }
  }
  // The default law's markers: -1 / +1 at 0.3 / 0.7, -10 / +10 and -100 / +100 mirrored around 12 o'clock.
  const VcaLaw d = vcaLaw(VcaCurve::Log, -kVcaRange, kVcaRange, 0.4, 0.0);
  CHECK(d.marks[2].base == doctest::Approx(0.3));
  CHECK(d.marks[3].base == doctest::Approx(0.7));
  CHECK(d.marks[4].base == doctest::Approx(1.0 - d.marks[1].base).epsilon(1e-12));
  CHECK(d.marks[5].base == doctest::Approx(1.0 - d.marks[0].base).epsilon(1e-12));
  CHECK(d.marks[4].base > 0.77);
  CHECK(d.marks[5].base < 0.86);
  // Each marker moved alone: it lands where it was put with its value exactly; every other marker stays; outside the
  // two segments next to it nothing changes.
  for (uint32_t i = 0; i < kVcaMarks; ++i)
    for (const double off : {-0.03, 0.02}) {
      CAPTURE(i);
      CAPTURE(off);
      std::array<double, kVcaMarks> o{};
      o[i] = off;
      const VcaLaw m = vcaLaw(VcaCurve::Log, -kVcaRange, kVcaRange, 0.4, 0.0, o);
      CHECK(m.warped);
      CHECK(m.marks[i].at == doctest::Approx(d.marks[i].base + off).epsilon(1e-12));
      CHECK(vcaValue(m, m.marks[i].at) == doctest::Approx(kVcaMarkValue[i]).epsilon(1e-9));
      for (uint32_t j = 0; j < kVcaMarks; ++j) {
        if (j == i) continue;
        CHECK(m.marks[j].at == d.marks[j].at);
        CHECK(vcaValue(m, m.marks[j].at) == doctest::Approx(kVcaMarkValue[j]).epsilon(1e-9));
      }
      const double a0 = i == 0 ? 0.0 : d.marks[i - 1].at, a1 = i + 1 == kVcaMarks ? 1.0 : d.marks[i + 1].at;
      for (int k = 0; k <= 200; ++k) {
        const double p = k / 200.0;
        if (p > a0 && p < a1) continue;
        CHECK(vcaValue(m, p) == doctest::Approx(vcaValue(d, p)).epsilon(1e-9).scale(1.0));
      }
    }
  // +1 moved right by 0.1: the fine zone runs 0.3 ... 0.8, linear (0 at 0.55); +1 ... +10 is squeezed.
  std::array<double, kVcaMarks> o{};
  o[3] = 0.1;
  const VcaLaw a = vcaLaw(VcaCurve::Log, -kVcaRange, kVcaRange, 0.4, 0.0, o);
  CHECK(vcaValue(a, 0.55) == doctest::Approx(0.0).scale(1.0).epsilon(1e-12));
  CHECK(vcaValue(a, 0.675) == doctest::Approx(0.5).epsilon(1e-12));
  CHECK(vcaValue(a, 0.8) == doctest::Approx(1.0).epsilon(1e-12));
  CHECK(vcaValue(a, 0.3) == doctest::Approx(-1.0).epsilon(1e-12));
  // Ordering, for any state (the face's drag never writes one out of order: it clamps, below): the markers stay a gap
  // apart and from the ends; an offset past a neighbour pushes that neighbour up (a lower marker wins).
  o = {};
  o[3] = 0.5;  // +1 past +10 (at about 0.776) and +100
  const VcaLaw c = vcaLaw(VcaCurve::Log, -kVcaRange, kVcaRange, 0.4, 0.0, o);
  CHECK(c.marks[3].at == doctest::Approx(1.0 - 3.0 * kVcaMarkGap).epsilon(1e-12));
  CHECK(c.marks[4].at == doctest::Approx(1.0 - 2.0 * kVcaMarkGap).epsilon(1e-12));
  CHECK(c.marks[5].at == doctest::Approx(1.0 - kVcaMarkGap).epsilon(1e-12));
  for (const VcaLaw::Mark& m : c.marks) CHECK(vcaValue(c, m.at) == doctest::Approx(m.value).epsilon(1e-9));
  o = {};
  o[5] = 0.9;  // +100 to the end
  const VcaLaw e = vcaLaw(VcaCurve::Log, -kVcaRange, kVcaRange, 0.4, 0.0, o);
  CHECK(e.marks[5].at == doctest::Approx(1.0 - kVcaMarkGap).epsilon(1e-12));
  o = {};
  o[0] = -0.9;
  const VcaLaw f = vcaLaw(VcaCurve::Log, -kVcaRange, kVcaRange, 0.4, 0.0, o);
  CHECK(f.marks[0].at == doctest::Approx(kVcaMarkGap).epsilon(1e-12));
  // The offset a drag writes (vcaMarkOffset): clamped between the shown neighbours, the other markers unmoved.
  CHECK(vcaMarkOffset(d, 3, 0.9, 0.0) == doctest::Approx(d.marks[4].at - kVcaMarkGap - d.marks[3].base).epsilon(1e-12));
  CHECK(vcaMarkOffset(d, 3, 0.4, 0.0) == doctest::Approx(0.4 - d.marks[3].base).epsilon(1e-12));
  CHECK(vcaMarkOffset(d, 2, 0.75, 0.0) == doctest::Approx(d.marks[3].at - kVcaMarkGap - d.marks[2].base).epsilon(1e-12));
  CHECK(vcaMarkOffset(d, 0, -1.0, 0.0) == doctest::Approx(kVcaMarkGap - d.marks[0].base).epsilon(1e-12));
  // Hidden markers (their values outside (min, max)) have no effect: -50 ... 20000 hides -100.
  o = {};
  o[0] = 0.2;
  const VcaLaw h = vcaLaw(VcaCurve::Log, -50.0, 20000.0, 0.4, 0.0, o), h0 = vcaLaw(VcaCurve::Log, -50.0, 20000.0, 0.4, 0.0);
  CHECK_FALSE(h.marks[0].shown);
  CHECK(h.marks[1].shown);
  CHECK_FALSE(h.warped);
  for (int k = 0; k <= 100; ++k) CHECK(vcaValue(h, k / 100.0) == vcaValue(h0, k / 100.0));
  CHECK(vcaMarkOffset(h, 0, 0.5, 0.2) == 0.2);  // a hidden marker keeps its offset
  // 0 ... 20000: the negative markers are hidden, -1 too (min 0 > -1).
  const VcaLaw half = vcaLaw(VcaCurve::Log, 0.0, 20000.0, 0.4, 0.0);
  CHECK_FALSE(half.marks[2].shown);
  CHECK(half.marks[3].shown);
  // The Bend curve has no markers.
  for (const VcaLaw::Mark& m : vcaLaw(VcaCurve::Bend, -kVcaRange, kVcaRange, 0.4, 0.0, o).marks) CHECK_FALSE(m.shown);
  // The zone (the box drag) places every marker's base: wider, +1 moves out and the outer's markers with it (the outer's
  // share of the travel shrinks from 0.3 to 0.2; +10 keeps about its share of it).
  const VcaLaw wide = vcaLaw(VcaCurve::Log, -kVcaRange, kVcaRange, 0.6, 0.0);
  CHECK(wide.marks[3].base == doctest::Approx(0.8));
  CHECK(wide.marks[4].base > d.marks[4].base);
  CHECK((1.0 - wide.marks[4].base) / (1.0 - d.marks[4].base) == doctest::Approx(0.2 / 0.3).epsilon(0.1));
}

TEST_CASE("vca: the DSP computes the law (both curves, many states); interpreter == JIT bit for bit") {
  struct S {
    float pos, min, max, zone, bend;
    std::array<float, kVcaMarks> mk{};  // TX.vca-knob: marker offsets
  };
  const S states[] = {{0.7f, -20000, 20000, 0.4f, 0},   {0.5f, -20000, 20000, 0.4f, 0},  {0.0f, -20000, 20000, 0.4f, 0},
                      {1.0f, -20000, 20000, 0.4f, 0},   {0.93f, -20000, 20000, 0.4f, 0}, {0.12f, -20000, 20000, 0.4f, 0},
                      {0.41f, -20000, 20000, 0.4f, 0},  {0.6f, 20, 20000, 0.4f, 0},     {0.3f, -20000, -20, 0.4f, 0},
                      {0.15f, 0, 20000, 0.4f, 0},       {0.75f, -0.5f, 0.8f, 0.4f, 0},  {0.85f, -2, 2, 0.4f, 0},
                      {0.95f, -20000, 20000, 0.8f, 0},  {0.5f, 300, -300, 0.4f, 0},     {0.33f, -1, 20000, 0.05f, 0},
                      {0.5f, -2, 6, 0.4f, 0.6f},        {0.5f, -2, 6, 0.4f, -0.6f},     {0.8f, 0, 20000, 0.4f, 1.0f},
                      {0.25f, -2, 6, 0.4f, 0.0f},       {0.25f, -2, 6, 0.4f, 0.0005f},
                      // TX.vca-knob: markers moved (each alone, several, crossing their neighbours, hidden ones)
                      {0.5f, -20000, 20000, 0.4f, 0, {0, 0, 0, 0.1f, 0, 0}},     {0.75f, -20000, 20000, 0.4f, 0, {0, 0, 0, 0.1f, 0, 0}},
                      {0.2f, -20000, 20000, 0.4f, 0, {0, 0, -0.15f, 0, 0, 0}},   {0.82f, -20000, 20000, 0.4f, 0, {0, 0, 0, 0, 0.05f, 0}},
                      {0.95f, -20000, 20000, 0.4f, 0, {0, 0, 0, 0, 0, -0.1f}},   {0.05f, -20000, 20000, 0.4f, 0, {0.1f, -0.1f, 0, 0, 0, 0}},
                      {0.6f, -20000, 20000, 0.4f, 0, {0, 0, 0, 0.5f, -0.5f, 0}}, {0.3f, -50, 20000, 0.4f, 0, {0.5f, 0.05f, 0, 0, 0, 0}},
                      {0.4f, 0, 20000, 0.4f, 0, {0, 0, 0, 0.1f, 0.1f, 0.1f}},    {0.6f, -3, 1.5f, 0.9f, 0, {0, 0.3f, -0.2f, 0.1f, 0, 0}}};
  for (const VcaCurve curve : {VcaCurve::Log, VcaCurve::Bend}) {
    const fs::path f = curveFile(curve == VcaCurve::Log ? "log.pqp" : "bend.pqp", curve);
    for (const S& s : states) {
      CAPTURE(static_cast<int>(curve));
      CAPTURE(s.pos);
      CAPTURE(s.min);
      CAPTURE(s.max);
      CAPTURE(s.bend);
      const std::vector<std::pair<std::string, float>> params = {
          {"v__value", s.pos},     {"v__min", s.min},       {"v__max", s.max},      {"v__zone", s.zone},
          {"v__bend", s.bend},     {"v__mk_n100", s.mk[0]}, {"v__mk_n10", s.mk[1]}, {"v__mk_n1", s.mk[2]},
          {"v__mk_p1", s.mk[3]},   {"v__mk_p10", s.mk[4]},  {"v__mk_p100", s.mk[5]}};
      const std::vector<float> yi = render(f, 0.01, params, pqc::Backend::Interpreter, "Main");
      const std::vector<float> yj = render(f, 0.01, params, pqc::Backend::Jit, "Main");
      REQUIRE(yi.size() == yj.size());
      CHECK(std::memcmp(yi.data(), yj.data(), yi.size() * sizeof(float)) == 0);
      const std::array<double, kVcaMarks> off = {s.mk[0], s.mk[1], s.mk[2], s.mk[3], s.mk[4], s.mk[5]};
      const double want = vcaValue(vcaLaw(curve, s.min, s.max, s.zone, s.bend, off), s.pos);
      // start: 1 — the first sample is already the value; it stays there.
      CHECK(std::abs(yi.front() - want) <= 1e-6 + 2e-5 * std::abs(want));  // float32 arithmetic
      CHECK(yi.back() == yi.front());
    }
    fs::remove(f);
  }
}

TEST_CASE("vca: a value modulated at audio rate is mapped per sample (no smoothing); in multiplies") {
  // The knob position swept by a 50 Hz triangle through the whole travel: each sample is the law at its position.
  const fs::path f = curveFile("sweep.pqp", VcaCurve::Log, "0.5, value: abs(frac(phasor(50Hz)) * 2 - 1)");
  const fs::path ref = writeTemp("sweepref.pqp", "patch R {\n  out out : audio mono\n  out = abs(frac(phasor(50Hz)) * 2 - 1)\n}\n");
  const std::vector<float> y = render(f, 0.05, {}, pqc::Backend::Jit, "Main");
  const std::vector<float> p = render(ref, 0.05, {}, pqc::Backend::Jit);
  REQUIRE(y.size() == p.size());
  const VcaLaw l = vcaLaw(VcaCurve::Log, -kVcaRange, kVcaRange, 0.4, 0.0);
  for (size_t i = 0; i < y.size(); i += 7) {
    CAPTURE(i);
    const double want = 0.5 * vcaValue(l, p[i]);
    CHECK(std::abs(y[i] - want) <= 1e-6 + 3e-5 * std::abs(want));
  }
  const std::vector<float> yi = render(f, 0.05, {}, pqc::Backend::Interpreter, "Main");
  CHECK(std::memcmp(yi.data(), y.data(), y.size() * sizeof(float)) == 0);
  fs::remove(f);
  fs::remove(ref);
}

TEST_CASE("vca: cost — static value vs in · g by hand vs a value modulated at audio rate (JIT)" * doctest::test_suite("perf")) {
  class Discard final : public engine::AudioSink {
  public:
    Status begin(uint32_t, uint32_t, uint64_t) override { return Ok{}; }
    Status write(const float* const*, uint32_t) override { return Ok{}; }
    Status end() override { return Ok{}; }
  };
  const fs::path vca = writeTemp("costv.pqp", "patch V {\n  out out : audio mono\n  v = meta_vca(noise(), value: 0.8)\n  out = v\n}\n");
  const fs::path hand = writeTemp("costh.pqp", "patch H {\n  param g = 3.0\n  out out : audio mono\n  out = noise() * g\n}\n");
  const fs::path src = writeTemp("costs.pqp", "patch S {\n  out out : audio mono\n  out = noise()\n}\n");
  const fs::path mod =
      writeTemp("costm.pqp", "patch M {\n  out out : audio mono\n  v = meta_vca(noise(), value: 0.5 + 0.4 * sine(phasor(3Hz)))\n  out = v\n}\n");
  auto best = [&](const fs::path& f) {
    double t = 1e30;
    for (int i = 0; i < 3; ++i) {
      pqc::RenderOptions o;
      o.seconds = 60.0;
      o.backend = pqc::Backend::Jit;
      Discard sink;
      auto r = pqc::renderFile(f, o, sink);
      REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
      t = std::min(t, r->stats.wallSeconds);
    }
    return t * 1e9 / (60.0 * 48000.0);
  };
  const double v = best(vca), h = best(hand), s = best(src), m = best(mod);
  MESSAGE("ns/sample: meta_vca static " << v << ", noise * g by hand " << h << ", noise alone " << s
                                        << ", value modulated at audio rate " << m);
  CHECK(v > 0.0);
  for (const fs::path& f : {vca, hand, src, mod}) fs::remove(f);
}
