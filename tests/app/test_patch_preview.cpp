// T4.4: edits in the circuit editor hot-swap the running patch through the preview (PatchPreview over a ProgramHost):
// click-free (no step larger than the signal's own), state kept (a phasor's phase runs on through text and circuit
// edits), a parse or check error never reaches the audio (the last good program plays on, bit for bit), the JIT image
// replaces the interpreter's with no difference, the audio thread's part allocation-free (RtScope), an instrument's
// voices keep sounding through a swap.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <vector>

#include "pq/app/patch_preview.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/graph/atoms.h"
#include "pq/views/circuit/patch_editor.h"
#include "pq/views/devices/device_view.h"

using namespace pq;
using namespace pq::app;
using namespace pq::views::circuit;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 256;

std::string stdlibPath(const char* rel) { return (fs::path(PQ_TEST_DATA_DIR) / ".." / "stdlib" / rel).string(); }

// Renders `blocks` device blocks of the preview (stereo, zeroed first) and appends channel 0.
void render(PatchPreview& p, uint32_t blocks, std::vector<float>& out, int64_t& t) {
  std::vector<float> l(kBlock), r(kBlock);
  float* outs[2] = {l.data(), r.data()};
  for (uint32_t b = 0; b < blocks; ++b) {
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    engine::AudioBlock blk;
    blk.out = outs;
    blk.numOut = 2;
    blk.frames = kBlock;
    blk.frameTime = t;
    {
      RtScope rt;
      DenormalGuard dg;
      p.process(blk);
    }
    t += kBlock;
    out.insert(out.end(), l.begin(), l.end());
  }
}

float maxStep(const std::vector<float>& x, size_t from = 1) {
  float m = 0.0f;
  for (size_t i = std::max<size_t>(from, 1); i < x.size(); ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
  return m;
}

const char* kTone =
    "patch Tone {\n"
    "  param freq = 220Hz [20Hz..2kHz]\n"
    "  out y : audio\n"
    "  ph = phasor(freq)\n"
    "  y = sine(ph) * 0.5\n"
    "}\n";

}  // namespace

TEST_CASE("patch preview: text and circuit edits hot-swap click-free; errors leave the last good program playing") {
  PreviewConfig pc;
  pc.sampleRate = kRate;
  pc.gainDb = 0.0f;
  PatchPreview preview(pc);
  int activations = 0;
  preview.onActiveChanged = [&](bool on) { activations += on ? 1 : -1; };
  auto made = PatchEditor::fromText(stdlibPath("preview_test.pqp"), kTone);
  REQUIRE(made.ok());
  PatchEditor& ed = **made;
  ed.setPlayer(&preview);
  { auto pr = ed.play(0); REQUIRE_MESSAGE(pr.ok(), (pr.ok() ? std::string() : pr.error().message)); }
  CHECK(activations == 1);
  std::vector<float> y;
  int64_t t = 0;
  render(preview, 40, y, t);
  // The steady sine's largest step: amplitude · 2π f / sr.
  const float steady = 0.5f * static_cast<float>(2.0 * std::numbers::pi * 220.0 / kRate);
  CHECK(maxStep(y) <= steady * 1.01f);
  // 1. A text edit: the amplitude 0.5 → 0.4 (a structural change: the constant is folded). Crossfaded.
  ed.replaceText(static_cast<uint32_t>(ed.text().find("0.5")), 3, "0.4", false);
  REQUIRE(ed.check().ok());
  size_t mark = y.size();
  render(preview, 40, y, t);
  CHECK(maxStep(y, mark) <= steady * 1.01f);
  // 2. A circuit edit: tanh in front of the output (graph IR edit → text → swap). The phasor keeps its phase.
  {
    const graph::Patch& p = ed.drawn(0);
    auto th = ed.addNode(0, "tanh", {200, 100});
    REQUIRE(th.ok());
    const graph::Patch& q = ed.drawn(0);
    graph::NodeIndex mul = graph::kNone;
    for (graph::NodeIndex v = 0; v < q.nodeCount(); ++v)
      if (q.node(v).kind == graph::NodeKind::Atom && graph::atomSignature(q.node(v).atom).name == "mul") mul = v;
    REQUIRE(mul != graph::kNone);
    REQUIRE(ed.connect(0, {mul, 0}, {*q.indexOf(*th), 0}).ok());
    const graph::Patch& r = ed.drawn(0);
    REQUIRE(ed.connect(0, {*r.indexOf(*th), 0}, {*r.findByName("y"), 0}).ok());
    (void)p;
  }
  REQUIRE(ed.check().ok());
  CHECK(ed.text().find("y = tanh(sine(ph) * 0.4)") != std::string::npos);
  // Every state entry carried over (the phasor's), none new or lost.
  CHECK(preview.stats().lastMatched >= 1);
  CHECK(preview.stats().lastFresh == 0);
  CHECK(preview.stats().lastDropped == 0);
  mark = y.size();
  render(preview, 40, y, t);
  CHECK(maxStep(y, mark) <= steady * 1.01f);
  // The phase ran on through both swaps: the output is still the 220 Hz sine from t = 0 (no restart).
  const size_t probe = y.size() - 10;
  const double phase = std::fmod(220.0 * static_cast<double>(probe) / kRate, 1.0);
  CHECK(y[probe] == doctest::Approx(std::tanh(0.4 * std::sin(2.0 * std::numbers::pi * phase))).epsilon(2e-3));
  // 3. A syntax error, then a check error (a loop with no delay): neither reaches the audio.
  const uint64_t swaps = ed.stats().swaps;
  ed.replaceText(static_cast<uint32_t>(ed.text().find("tanh(")), 5, "tanh((", false);
  CHECK_FALSE(ed.textLowered());
  CHECK_FALSE(ed.check().ok());
  REQUIRE(ed.undo());
  ed.replaceText(static_cast<uint32_t>(ed.text().find("phasor(freq)")), 12, "tanh(ph)", false);
  CHECK(ed.textLowered());
  CHECK_FALSE(ed.check().ok());
  CHECK(ed.stats().swaps == swaps);
  REQUIRE(ed.undo());
  REQUIRE(ed.check().ok());
  CHECK(ed.stats().swaps == swaps + 1);
  mark = y.size();
  render(preview, 20, y, t);
  CHECK(maxStep(y, mark) <= steady * 1.01f);
  ed.stop();
  CHECK(activations == 0);
  CHECK_FALSE(preview.playing());
}

TEST_CASE("patch preview: a parse error during playback leaves the program untouched, bit for bit") {
  PreviewConfig pc;
  pc.sampleRate = kRate;
  PatchPreview a(pc), b(pc);
  auto ea = PatchEditor::fromText(stdlibPath("preview_test.pqp"), kTone);
  auto eb = PatchEditor::fromText(stdlibPath("preview_test.pqp"), kTone);
  REQUIRE(ea.ok());
  REQUIRE(eb.ok());
  (*ea)->setPlayer(&a);
  (*eb)->setPlayer(&b);
  REQUIRE((*ea)->play(0).ok());
  REQUIRE((*eb)->play(0).ok());
  std::vector<float> ya, yb;
  int64_t ta = 0, tb = 0;
  render(a, 10, ya, ta);
  render(b, 10, yb, tb);
  // Editor A breaks its text and checks (as the view does 50 ms after a key): nothing is swapped.
  (*ea)->replaceText(static_cast<uint32_t>((*ea)->text().find("sine(")), 0, "((", false);
  CHECK_FALSE((*ea)->check().ok());
  render(a, 20, ya, ta);
  render(b, 20, yb, tb);
  CHECK(ya == yb);
}

TEST_CASE("patch preview: the JIT image replaces the interpreter's without a difference") {
  PreviewConfig pc;
  pc.sampleRate = kRate;
  pc.jit = compile::jitUnitBackend();
  PatchPreview jit(pc);
  PreviewConfig pi;
  pi.sampleRate = kRate;
  PatchPreview interp(pi);
  auto ej = PatchEditor::fromText(stdlibPath("preview_test.pqp"), kTone);
  auto ei = PatchEditor::fromText(stdlibPath("preview_test.pqp"), kTone);
  REQUIRE(ej.ok());
  REQUIRE(ei.ok());
  (*ej)->setPlayer(&jit);
  (*ei)->setPlayer(&interp);
  REQUIRE((*ej)->play(0).ok());
  REQUIRE((*ei)->play(0).ok());
  REQUIRE(jit.waitForJit());
  std::vector<float> yj, yi;
  int64_t tj = 0, ti = 0;
  render(jit, 30, yj, tj);
  render(interp, 30, yi, ti);
  CHECK(jit.stats().jitPublished == 1);
  CHECK(yj == yi);
}

TEST_CASE("patch preview: an instrument's voices keep sounding through an edit") {
  PreviewConfig pc;
  pc.sampleRate = kRate;
  PatchPreview preview(pc);
  auto made = PatchEditor::open(stdlibPath("instruments/basic_synth.pqp"));
  REQUIRE(made.ok());
  PatchEditor& ed = **made;
  ed.setPlayer(&preview);
  { auto pr = ed.play(0); REQUIRE_MESSAGE(pr.ok(), (pr.ok() ? std::string() : pr.error().message)); }
  std::vector<float> y;
  int64_t t = 0;
  render(preview, 60, y, t);  // 0.32 s: two arpeggio notes
  float peak = 0.0f;
  for (float v : y) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.01f);
  // A text edit (brightness default) swaps the program; the sounding note continues (no silence after the swap).
  ed.replaceText(static_cast<uint32_t>(ed.text().find("0.35")), 4, "0.5", false);
  REQUIRE(ed.check().ok());
  const size_t mark = y.size();
  render(preview, 8, y, t);
  float after = 0.0f;
  for (size_t i = mark; i < y.size(); ++i) after = std::max(after, std::fabs(y[i]));
  CHECK(after > 0.01f);
  CHECK(ed.stats().swaps == 2);
}

// ---- T4.6: device panels through the preview --------------------------------------------------------------------------

TEST_CASE("patch preview: param writes by name reach the playing program (RT-safe), and the output tap fills only when read") {
  PreviewConfig pc;
  pc.sampleRate = kRate;
  pc.gainDb = 0.0f;
  PatchPreview preview(pc);
  auto made = PatchEditor::fromText(stdlibPath("preview_param_test.pqp"),
                                    "patch Tone {\n  param level = 0.5 [0..1]\n  out y : audio\n  y = sine(phasor(220Hz)) * level\n}\n");
  REQUIRE(made.ok());
  PatchEditor& ed = **made;
  ed.setPlayer(&preview);
  REQUIRE(ed.play(0).ok());
  std::vector<float> y;
  int64_t t = 0;
  render(preview, 20, y, t);
  auto peakFrom = [&](size_t from) {
    float m = 0.0f;
    for (size_t i = from; i < y.size(); ++i) m = std::max(m, std::fabs(y[i]));
    return m;
  };
  CHECK(peakFrom(y.size() / 2) == doctest::Approx(0.5f).epsilon(0.01));
  // Nobody reads the tap: the audio thread copies nothing.
  TapRing& tap = *preview.outputTap();
  CHECK(tap.written() == 0);
  CHECK(preview.setParam("level", 0.25f));
  CHECK_FALSE(preview.setParam("nope", 1.0f));
  tap.addReader();
  const size_t mark = y.size();
  render(preview, 20, y, t);  // under RtScope (render): the param write and the tap copy allocate nothing
  CHECK(peakFrom(mark) == doctest::Approx(0.25f).epsilon(0.01));
  CHECK(tap.written() == 20u * kBlock);
  std::vector<float> got(kBlock);
  REQUIRE(tap.read(0, tap.written() - kBlock, kBlock, got.data()) == kBlock);
  for (uint32_t i = 0; i < kBlock; ++i) CHECK(got[i] == doctest::Approx(y[y.size() - kBlock + i]));
  tap.removeReader();
  // Stopped: writes are refused.
  ed.stop();
  CHECK_FALSE(preview.setParam("level", 0.1f));
}

TEST_CASE("patch preview: a device panel's preview plays the Saturator; a curve change hot-swaps click-free") {
  PreviewConfig pc;
  pc.sampleRate = kRate;
  pc.gainDb = 0.0f;
  PatchPreview preview(pc);
  auto pv = views::devices::DevicePreview::open(stdlibPath("devices/saturator.pqp"), &preview);
  REQUIRE_MESSAGE(pv.ok(), (pv.ok() ? std::string() : pv.error().message));
  views::devices::DevicePreview& dev = **pv;
  REQUIRE(dev.play().ok());
  std::vector<float> y;
  int64_t t = 0;
  render(preview, 60, y, t);  // the arpeggio through the Saturator (Analog Clip)
  float peak = 0.0f;
  for (float v : y) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.05f);
  // Drive up: a param write (no recompile), heard at once.
  const uint64_t published = preview.stats().published;
  dev.setParam("drive", std::pow(10.0, 18.0 / 20.0));
  render(preview, 20, y, t);
  CHECK(preview.stats().published == published);
  // Type → Hard Curve: a recompile through the host patch's editor, crossfaded while it plays.
  dev.setConst("type", 4.0);
  CHECK(preview.stats().published == published + 1);
  CHECK(preview.stats().lastMatched > 0);  // state carried (the source's phasors and envelopes)
  const size_t mark = y.size();
  render(preview, 40, y, t);
  // No step at the swap larger than the signal's own largest step before it (a saturated arpeggio: steep but bounded).
  const float before = maxStep(std::vector<float>(y.begin(), y.begin() + static_cast<std::ptrdiff_t>(mark)));
  const float during = maxStep(std::vector<float>(y.begin() + static_cast<std::ptrdiff_t>(mark) - 1, y.end()));
  CHECK(during <= 1.5f * before + 1e-3f);
  dev.stop();
}
