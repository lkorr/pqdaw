// TMM.3: the Phase accumulator through the compiler — a standalone .pqp with `meta_phase_acc(…)` checks and renders in
// pqc (the prelude, default state); at freq 0 it is bit-identical to frac(phasor(voice.hz)); at 100 Hz its cycles run
// at the note + 100 Hz (≤ 1e-4 cents over 10 s); phase and amp do what they say; interpreter == JIT. TMM.15: the pitch
// param — whole octaves bit-exact (×2^n), 700 cents at 2^(7/12), per voice, interpreter == JIT.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <bit>
#include <string>
#include <utility>
#include <vector>

#include "pq/engine/offline_render.h"
#include "pq/meta/phase_acc.h"
#include "pq/pqc/pqc.h"

using namespace pq;

namespace {

namespace fs = std::filesystem;

fs::path writeTemp(const std::string& name, const std::string& text) {
  const fs::path p = fs::temp_directory_path() / ("pq_test_meta_" + name);
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

// The cycle rate of a 0 → 1 ramp: wraps located to a fraction of a sample (the ramp is linear between samples), the
// rate fitted from the first to the last wrap.
double rampHz(const std::vector<float>& y, double sr, size_t skip) {
  double first = -1.0, last = -1.0;
  size_t wraps = 0;
  for (size_t i = skip + 1; i < y.size(); ++i) {
    if (!(y[i] < y[i - 1] - 0.5f)) continue;
    const double inc = static_cast<double>(y[i]) + 1.0 - static_cast<double>(y[i - 1]);
    const double t = static_cast<double>(i) - static_cast<double>(y[i]) / inc;
    if (first < 0.0) first = t; else ++wraps;
    last = t;
  }
  REQUIRE(wraps > 100);
  return static_cast<double>(wraps) * sr / (last - first);
}

// TMM.10-fix1 (SPEC §15 2026-11-01 "notes are a signal"): the note plays because its frequency is wired into hz.
const char* kAcc = R"(patch Acc {
  out out : audio voice mono
  acc1 = meta_phase_acc(voice.hz)
  out = acc1
}
)";
// Nothing wired: it runs once, freely, at its frequency bar.
const char* kFree = R"(patch Free {
  out out : audio mono
  acc1 = meta_phase_acc(freq: 100Hz)
  out = acc1
}
)";
const char* kFreeRef = R"(patch FreeRef {
  out out : audio mono
  out = frac(phasor(100Hz))
}
)";
const char* kDc = R"(patch Dc {
  out out : audio voice mono
  acc1 = meta_phase_acc(phase: 0.25)
  out = acc1
}
)";
const char* kRef = R"(patch Ref {
  out out : audio voice mono
  out = frac(phasor(voice.hz))
}
)";

}  // namespace

TEST_CASE("phase_acc: a standalone .pqp checks and renders in pqc (the prelude, default state)") {
  const fs::path f = writeTemp("acc.pqp", kAcc);
  std::ostringstream out, err;
  const std::vector<std::string> args{"check", f.string()};
  CHECK(pqc::run(args, out, err) == pqc::kExitOk);
  CHECK(out.str().find("ok") != std::string::npos);
  CAPTURE(err.str());
  // A wrong argument name is a compile error reported against the call.
  const fs::path bad = writeTemp("bad.pqp", "patch B {\n  out out : audio voice mono\n  out = meta_phase_acc(nope: 1)\n}\n");
  std::ostringstream o2, e2;
  const std::vector<std::string> a2{"check", bad.string()};
  CHECK(pqc::run(a2, o2, e2) == pqc::kExitCompileError);
  fs::remove(f);
  fs::remove(bad);
}

TEST_CASE("phase_acc: freq 0 is bit-identical to frac(phasor(voice.hz)) on both backends") {
  const fs::path acc = writeTemp("acc0.pqp", kAcc), ref = writeTemp("ref0.pqp", kRef);
  for (const pqc::Backend b : {pqc::Backend::Interpreter, pqc::Backend::Jit}) {
    CAPTURE(compile::backendLabel(b));
    // Three notes, one with a fractional pitch; overlapping, released inside the render.
    const std::string notes = "57@0:0.6 60.37@0.05:0.4 69@0.2:0.5";
    const std::vector<float> a = render(acc, notes, 0.8, {}, b), r = render(ref, notes, 0.8, {}, b);
    REQUIRE(a.size() == r.size());
    size_t diff = 0, nonzero = 0;
    for (size_t i = 0; i < a.size(); ++i) {
      diff += std::bit_cast<uint32_t>(a[i]) != std::bit_cast<uint32_t>(r[i]) ? 1u : 0u;
      nonzero += a[i] != 0.0f ? 1u : 0u;
    }
    CHECK(diff == 0);
    CHECK(nonzero > a.size() / 2);
  }
  fs::remove(acc);
  fs::remove(ref);
}

TEST_CASE("phase_acc: with no note wired it runs freely — exactly its bar's phase; at 0 Hz a DC offset (fix1)") {
  const fs::path free = writeTemp("accfree.pqp", kFree), ref = writeTemp("accfreeref.pqp", kFreeRef);
  const fs::path dc = writeTemp("accdc.pqp", kDc);
  for (const pqc::Backend b : {pqc::Backend::Interpreter, pqc::Backend::Jit}) {
    CAPTURE(compile::backendLabel(b));
    const std::vector<float> a = render(free, "", 2.0, {}, b), r = render(ref, "", 2.0, {}, b);
    REQUIRE(a.size() == r.size());
    size_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff += std::bit_cast<uint32_t>(a[i]) != std::bit_cast<uint32_t>(r[i]) ? 1u : 0u;
    CHECK(diff == 0);
    CHECK(rampHz(a, 48000.0, 0) == doctest::Approx(100.0).epsilon(1e-6));
    // Inside a voice with nothing wired into hz: 0 Hz — its phase, a DC offset, whatever note plays.
    const std::vector<float> d = render(dc, "69@0:0.3 57@0.1:0.2", 0.3, {}, b);
    REQUIRE(d.size() > 4800);
    for (size_t i = 0; i < 4800; ++i) CHECK(d[i] == 0.25f);
  }
  fs::remove(free);
  fs::remove(ref);
  fs::remove(dc);
}

TEST_CASE("phase_acc: at freq 100 Hz the cycles run at note + 100 Hz (<= 1e-4 cents over 10 s)") {
  const fs::path acc = writeTemp("acc100.pqp", kAcc);
  for (const char* note : {"69", "60"}) {
    CAPTURE(note);
    auto spec = pqc::parseNotes(note);  // the note's float32 Hz as the engine plays it
    REQUIRE(spec.ok());
    const float hz = spec->hz.at(0);
    const std::vector<float> y = render(acc, std::string(note) + "@0:10", 10.0, {{"acc1__freq", 100.0f}});
    REQUIRE(y.size() == 480000);
    // The note's float Hz plus 100 Hz (the expansion adds them in float32).
    const double expected = static_cast<double>(hz + 100.0f);
    const double got = rampHz(y, 48000.0, 64);
    const double cents = 1200.0 * std::log2(got / expected);
    CAPTURE(got);
    CAPTURE(expected);
    CHECK(std::fabs(cents) <= 1e-4);
  }
  fs::remove(acc);
}

TEST_CASE("phase_acc: phase is the start phase, amp multiplies the output, pm adds to the phase") {
  const fs::path acc = writeTemp("accp.pqp", kAcc);
  const std::vector<float> base = render(acc, "69@0:0.2", 0.2);
  const std::vector<float> shifted = render(acc, "69@0:0.2", 0.2, {{"acc1__phase", 0.25f}});
  const std::vector<float> doubled = render(acc, "69@0:0.2", 0.2, {{"acc1__amp", 2.0f}});
  // The note starts at sample 0 at phase 0 (the per-voice phasor resets at note-on): the offset is the start phase.
  CHECK(base[0] == 0.0f);
  CHECK(shifted[0] == 0.25f);
  for (size_t i = 0; i < 2000; ++i) {
    const float want = base[i] + 0.25f;
    CHECK(shifted[i] == doctest::Approx(want - std::floor(want)).epsilon(1e-6));
    CHECK(doubled[i] == 2.0f * base[i]);
  }
  // pm: a wired phase modulation adds to the phase (a constant pm of 0.5 = phase 0.5).
  const fs::path pm = writeTemp("accpm.pqp", R"(patch Pm {
  out out : audio voice mono
  acc1 = meta_phase_acc(voice.hz, pm: 0.5)
  out = acc1
}
)");
  const std::vector<float> withPm = render(pm, "69@0:0.2", 0.2);
  const std::vector<float> half = render(acc, "69@0:0.2", 0.2, {{"acc1__phase", 0.5f}});
  for (size_t i = 0; i < 2000; ++i) CHECK(withPm[i] == half[i]);
  fs::remove(acc);
  fs::remove(pm);
}

// ---- TMM.15: the pitch param ---------------------------------------------------------------------------------------

namespace {

// A by-hand free-running ramp at `hz` (the reference for a transposed 100 Hz accumulator).
std::string freeRefText(const std::string& hz) {
  return "patch FreeRefP {\n  out out : audio mono\n  out = frac(phasor(" + hz + "))\n}\n";
}
size_t bitDiffs(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  size_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff += std::bit_cast<uint32_t>(a[i]) != std::bit_cast<uint32_t>(b[i]) ? 1u : 0u;
  return diff;
}

}  // namespace

TEST_CASE("phase_acc pitch: the ratio is 2^(cents/1200) — whole octaves exact, 700 cents 2^(7/12), 0 exactly 1") {
  const meta::Type& t = meta::phaseAccType();
  REQUIRE(t.paramIndex("pitch") == static_cast<int32_t>(meta::kPhaseAccPitch));
  const meta::ParamSpec& s = t.params[meta::kPhaseAccPitch];
  CHECK(s.def == 0.0);
  CHECK(s.min == -4800.0);
  CHECK(s.max == 4800.0);
  CHECK(s.unit == graph::Unit::cents);
  CHECK(meta::phaseAccPitchRatio(0.0) == 1.0);
  for (int k = 1; k <= 4; ++k) {
    CHECK(meta::phaseAccPitchRatio(1200.0 * k) == std::ldexp(1.0, k));
    CHECK(meta::phaseAccPitchRatio(-1200.0 * k) == std::ldexp(1.0, -k));
  }
  CHECK(meta::phaseAccPitchRatio(700.0) == doctest::Approx(std::pow(2.0, 7.0 / 12.0)).epsilon(1e-15));
}

TEST_CASE("phase_acc pitch: whole octaves are bit-exact transpositions of the frequency, on both backends") {
  const fs::path free = writeTemp("accpitch.pqp", kFree);
  const std::vector<std::pair<float, std::string>> octaves = {{1200.0f, "200Hz"},  {2400.0f, "400Hz"},
                                                              {3600.0f, "800Hz"},  {4800.0f, "1600Hz"},
                                                              {-1200.0f, "50Hz"},  {-2400.0f, "25Hz"},
                                                              {-4800.0f, "6.25Hz"}};
  for (const pqc::Backend b : {pqc::Backend::Interpreter, pqc::Backend::Jit}) {
    CAPTURE(compile::backendLabel(b));
    for (const auto& [cents, hz] : octaves) {
      CAPTURE(cents);
      const fs::path ref = writeTemp("accpitchref.pqp", freeRefText(hz));
      const std::vector<float> a = render(free, "", 1.0, {{"acc1__pitch", cents}}, b);
      const std::vector<float> r = render(ref, "", 1.0, {}, b);
      CHECK(bitDiffs(a, r) == 0);
      fs::remove(ref);
    }
  }
  fs::remove(free);
}

TEST_CASE("phase_acc pitch: 700 cents runs at (note + freq) * 2^(7/12); per voice; interpreter == JIT") {
  const fs::path free = writeTemp("accpitch7.pqp", kFree);
  const std::vector<float> a = render(free, "", 10.0, {{"acc1__pitch", 700.0f}});
  // The expansion's arithmetic in float32: the ratio rounded once, then 100 Hz · ratio.
  const float ratio = static_cast<float>(std::exp2(700.0 / 1200.0));
  const double expected = static_cast<double>(100.0f * ratio);
  const double got = rampHz(a, 48000.0, 0);
  CAPTURE(got);
  CHECK(std::fabs(1200.0 * std::log2(got / expected)) <= 1e-4);
  CHECK(std::fabs(1200.0 * std::log2(got / 100.0) - 700.0) <= 1e-3);
  // Per voice: the note's frequency plus the bar, transposed (an octave down: bit-exact against (hz + bar) · 0.5).
  const fs::path acc = writeTemp("accpitchv.pqp", kAcc);
  const fs::path half = writeTemp("accpitchhalf.pqp", R"(patch Half {
  out out : audio voice mono
  out = frac(phasor((voice.hz + 30Hz) * 0.5))
}
)");
  const std::string notes = "57@0:0.6 60.37@0.05:0.4 69@0.2:0.5";
  for (const pqc::Backend b : {pqc::Backend::Interpreter, pqc::Backend::Jit}) {
    CAPTURE(compile::backendLabel(b));
    const std::vector<float> v = render(acc, notes, 0.8, {{"acc1__pitch", -1200.0f}, {"acc1__freq", 30.0f}}, b);
    CHECK(bitDiffs(v, render(half, notes, 0.8, {}, b)) == 0);
  }
  // Interpreter == JIT at a non-octave pitch, notes and the bar together.
  const std::vector<std::pair<std::string, float>> params = {{"acc1__pitch", 386.3137f}, {"acc1__freq", 7.0f}};
  CHECK(bitDiffs(render(acc, notes, 0.8, params, pqc::Backend::Interpreter),
                 render(acc, notes, 0.8, params, pqc::Backend::Jit)) == 0);
  fs::remove(free);
  fs::remove(acc);
  fs::remove(half);
}
