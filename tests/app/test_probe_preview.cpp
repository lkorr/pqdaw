// T4.5: live probes and cost meters through the circuit editor's document and the app's preview (PatchPreview): the
// editor's probes reach the preview's taps (interpreter and JIT image alike, bit for bit), the played audio is the same
// with and without probes, taps are reused only after they rest, and the cost meter reports the program the editor
// played (its generation, its regions), RtScope-clean on the audio side.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <vector>

#include "pq/app/patch_preview.h"
#include "pq/compile/compile.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/views/circuit/patch_editor.h"
#include "pq/views/circuit/probes.h"

using namespace pq;
using namespace pq::app;
using namespace pq::views::circuit;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 256;

std::string stdlibPath(const char* rel) { return (fs::path(PQ_TEST_DATA_DIR) / ".." / "stdlib" / rel).string(); }

void render(PatchPreview& p, uint32_t blocks, std::vector<float>& out) {
  std::vector<float> l(kBlock), r(kBlock);
  float* outs[2] = {l.data(), r.data()};
  for (uint32_t b = 0; b < blocks; ++b) {
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    engine::AudioBlock blk;
    blk.out = outs;
    blk.numOut = 2;
    blk.frames = kBlock;
    {
      RtScope rt;
      DenormalGuard dg;
      p.process(blk);
    }
    out.insert(out.end(), l.begin(), l.end());
  }
}

const char* kTone =
    "patch Tone {\n"
    "  param freq = 220Hz [20Hz..2kHz]\n"
    "  out y : audio\n"
    "  ph = phasor(freq)\n"
    "  hi = ph > 0.5\n"
    "  s = sine(ph)\n"
    "  y = s * select(hi, 0.5, 0.25)\n"
    "}\n";

graph::ProbeTarget target(const PatchEditor& ed, std::string_view name) {
  const graph::Patch& p = ed.parsed().file.patches[0];
  graph::ProbeTarget t;
  t.node = p.node(*p.findByName(name)).id;
  return t;
}

std::vector<float> tail(TapRing* ring, uint32_t n) {
  std::vector<float> v;
  if (!ring) return v;
  const uint64_t w = ring->written();
  const uint64_t from = std::max<uint64_t>(ring->oldestReadable(), w > n ? w - n : 0);
  v.resize(static_cast<size_t>(w - from));
  if (!v.empty()) v.resize(ring->read(0, from, static_cast<uint32_t>(v.size()), v.data()));
  return v;
}

}  // namespace

TEST_CASE("probe preview: the editor's probes reach the preview's taps, the audio does not change (T4.5)") {
  PreviewConfig pc;
  pc.sampleRate = kRate;
  pc.gainDb = 0.0f;
  PatchPreview probed(pc), plain(pc);
  auto ea = PatchEditor::fromText(stdlibPath("probe_test.pqp"), kTone);
  auto eb = PatchEditor::fromText(stdlibPath("probe_test.pqp"), kTone);
  REQUIRE(ea.ok());
  REQUIRE(eb.ok());
  PatchEditor& ed = **ea;
  ed.setPlayer(&probed);
  (*eb)->setPlayer(&plain);
  ed.setProbes({target(ed, "ph"), target(ed, "hi")}, {7, 9});
  const uint32_t signals[] = {7};
  probed.setProbeSignals(signals);
  REQUIRE(ed.play(0).ok());
  REQUIRE((*eb)->play(0).ok());
  REQUIRE(ed.played() != nullptr);
  CHECK(ed.played()->probeIds == std::vector<uint32_t>{7, 9});
  CHECK(ed.played()->generation == probed.generation());
  std::vector<float> ya, yb;
  render(probed, 20, ya);
  render(plain, 20, yb);
  CHECK(ya == yb);  // bit-identical with and without probes
  // The phasor's phase: a ramp at 220 Hz (the last 256 frames).
  const std::vector<float> ph = tail(probed.probeSignal(7), 256);
  REQUIRE(ph.size() == 256);
  bool ramp = true;
  for (size_t i = 1; i < ph.size(); ++i) {
    float d = ph[i] - ph[i - 1];
    if (d < -0.5f) d += 1.0f;
    ramp = ramp && std::fabs(d - static_cast<float>(220.0 / kRate)) < 1e-4f;
  }
  CHECK(ramp);
  CHECK(probed.probeSignal(9) == nullptr);  // a level only
  const ProbeLevel* lv = probed.probeLevel(9);
  REQUIRE(lv != nullptr);
  CHECK(lv->blocks.load() > 0);
  CHECK(lv->hi.load() == 1.0f);  // 256 frames of a 220 Hz phasor: above 0.5 somewhere
  CHECK(lv->lo.load() == 0.0f);
  // Another probe set: the old ids' taps are freed (not reused for a second), the new one gets a fresh tap.
  ed.setProbes({target(ed, "s")}, {11});
  const uint32_t signals2[] = {11};
  probed.setProbeSignals(signals2);
  REQUIRE(ed.check().ok());
  render(probed, 4, ya);
  CHECK(probed.probeSignal(7) == nullptr);
  CHECK(probed.probeLevel(9) == nullptr);
  REQUIRE(probed.probeSignal(11) != nullptr);
  const std::vector<float> s = tail(probed.probeSignal(11), 128);
  REQUIRE(s.size() == 128);
  for (float v : s) CHECK(std::fabs(v) <= 1.0f);
  ed.stop();
  (*eb)->stop();
}

TEST_CASE("probe preview: the JIT image writes the same probe values as the interpreter's (T4.5)") {
  PreviewConfig pc;
  pc.sampleRate = kRate;
  pc.jit = compile::jitUnitBackend();
  PatchPreview jit(pc);
  PreviewConfig pi;
  pi.sampleRate = kRate;
  PatchPreview interp(pi);
  auto ej = PatchEditor::fromText(stdlibPath("probe_test.pqp"), kTone);
  auto ei = PatchEditor::fromText(stdlibPath("probe_test.pqp"), kTone);
  REQUIRE(ej.ok());
  REQUIRE(ei.ok());
  for (auto* e : {&**ej, &**ei}) e->setProbes({target(*e, "s"), target(*e, "hi")}, {1, 2});
  const uint32_t signals[] = {1, 2};
  jit.setProbeSignals(signals);
  interp.setProbeSignals(signals);
  (*ej)->setPlayer(&jit);
  (*ei)->setPlayer(&interp);
  REQUIRE((*ej)->play(0).ok());
  REQUIRE((*ei)->play(0).ok());
  REQUIRE(jit.waitForJit());
  CHECK(jit.stats().jitPublished == 1);
  std::vector<float> yj, yi;
  render(jit, 30, yj);
  render(interp, 30, yi);
  CHECK(yj == yi);
  for (uint32_t id : {1u, 2u}) {
    const std::vector<float> a = tail(jit.probeSignal(id), 4096), b = tail(interp.probeSignal(id), 4096);
    CHECK(a.size() == 4096);
    size_t diff = 0;
    while (diff < a.size() && diff < b.size() && std::bit_cast<uint32_t>(a[diff]) == std::bit_cast<uint32_t>(b[diff])) ++diff;
    CHECK_MESSAGE(diff == a.size(), "probe " << id << " differs at " << diff << ": jit " << (diff < a.size() ? std::bit_cast<uint32_t>(a[diff]) : 0u)
                                            << " interp " << (diff < b.size() ? std::bit_cast<uint32_t>(b[diff]) : 0u));
  }
  (*ej)->stop();
  (*ei)->stop();
}

TEST_CASE("probe preview: the cost meter reports the played program, only while it is on (T4.5)") {
  PreviewConfig pc;
  pc.sampleRate = kRate;
  PatchPreview preview(pc);
  auto made = PatchEditor::fromText(stdlibPath("probe_test.pqp"), kTone);
  REQUIRE(made.ok());
  PatchEditor& ed = **made;
  ed.setPlayer(&preview);
  REQUIRE(ed.play(0).ok());
  std::vector<float> y;
  PatchPlayer::CostReading rd;
  render(preview, 8, y);
  CHECK_FALSE(preview.readCost(rd));  // off: nothing measured (the first read is the baseline)
  render(preview, 8, y);
  CHECK_FALSE(preview.readCost(rd));
  preview.setMetering(true);
  render(preview, 8, y);
  CHECK(preview.readCost(rd));  // the baseline was taken while off: the blocks since are measured
  render(preview, 40, y);
  REQUIRE(preview.readCost(rd));
  CHECK(rd.generation == ed.played()->generation);
  CHECK(rd.totalNsPerFrame > 0.0);
  REQUIRE(rd.regionNsPerFrame.size() == ed.played()->schedule.regions.size());
  CostMap cm;
  REQUIRE(attributeCost(*ed.played(), rd, {}, cm));
  CHECK(cm.shownNs > 0.0);
  CHECK(cm.shownNs <= cm.totalNs * 1.05 + 0.5);
  // The sine and the phasor are nodes of the shown patch with a cost.
  const graph::Patch& p = ed.parsed().file.patches[0];
  bool sine = false;
  for (const CostMap::Entry& e : cm.nodes) sine = sine || e.node == p.node(*p.findByName("s")).id;
  CHECK(sine);
  // A swap starts a new generation: the next reading is its own.
  ed.replaceText(static_cast<uint32_t>(ed.text().find("0.25")), 4, "0.3", false);
  REQUIRE(ed.check().ok());
  render(preview, 8, y);
  CHECK_FALSE(preview.readCost(rd));  // the new image's baseline
  render(preview, 40, y);
  REQUIRE(preview.readCost(rd));
  CHECK(rd.generation == ed.played()->generation);
  preview.setMetering(false);
  render(preview, 8, y);
  CHECK_FALSE(preview.readCost(rd));
  ed.stop();
}
