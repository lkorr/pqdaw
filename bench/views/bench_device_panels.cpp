// T4.6 benchmarks: device panels (CPU only: text through the CPU atlas, as bench_circuit_editor).
//   - a full paint of each stock panel in a 1280x300 tree (what a first frame or a theme change costs)
//   - a knob drag frame (Utility gain): pointer move → value → target write → damage → partial paint of that knob
//   - an EQ Eight dot drag frame: freq / gain write, the response recomputed, the display repainted
//   - an EQ Eight spectrum frame while audio plays: snapshot → 45 ms ease → the display repainted (the analysis apart)
//   - the analysis per hop (2048-point FFT, every other hop the 8192-point one, 1024 points, ballistics)
//   - the audio side: the tap's write of a 128-frame stereo block, with a reader and without one
// Results → bench/RESULTS.md (ship).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

#include <windows.h>
#include <intrin.h>

#include "pq/core/tap_ring.h"
#include "pq/dsp/fft.h"
#include "pq/dsp/spectrum.h"
#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/overlay.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/text.h"
#include "pq/ui/widget.h"
#include "pq/views/circuit/circuit_viewer.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/devices/device_panel.h"
#include "pq_bench.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views::devices;
namespace fs = std::filesystem;

namespace {

uint64_t threadCycles() {
  ULONG64 c = 0;
  QueryThreadCycleTime(GetCurrentThread(), &c);
  return c;
}

double tscNsPerCycle() {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  const uint64_t c0 = __rdtsc();
  while (clock::now() - t0 < std::chrono::milliseconds(200)) {
  }
  const uint64_t c1 = __rdtsc();
  return std::chrono::duration<double, std::nano>(clock::now() - t0).count() / static_cast<double>(c1 - c0);
}

struct Target final : DeviceTarget {
  SpectrumFeed* feed = nullptr;
  uint64_t writes = 0;
  void setParam(std::string_view, double) override { ++writes; }
  void setConst(std::string_view, double) override {}
  void setActive(bool) override {}
  SpectrumFeed* outputSpectrum() override { return feed; }
};

struct Loaded {
  DeviceSignature sig;
  PanelDesc desc;
};

Loaded load(const char* name) {
  const std::string path = (fs::path(PQ_SOURCE_DIR) / "stdlib" / "devices" / (std::string(name) + ".pqp")).string();
  auto doc = views::circuit::loadPatchDocument(path);
  if (!doc) {
    std::printf("cannot load %s: %s\n", name, doc.error().message.c_str());
    std::exit(1);
  }
  Loaded l;
  l.sig = signatureOf(*(*doc)->parsed.file.find(name));
  auto d = loadPanelDesc(panelPathFor(path), l.sig);
  if (!d) {
    std::printf("panel %s: %s\n", name, d.error().message.c_str());
    std::exit(1);
  }
  l.desc = std::move(*d);
  return l;
}

std::vector<float> testSignal(uint32_t n) {
  std::vector<float> x(n);
  uint32_t s = 7;
  for (uint32_t i = 0; i < n; ++i) {
    s = s * 1664525u + 1013904223u;
    const float w = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
    double v = 0.05 * w;
    for (int h = 1; h <= 12; ++h) v += 0.2 / h * std::sin(2.0 * 3.141592653589793 * 110.0 * h * i / 48000.0);
    x[i] = static_cast<float>(v);
  }
  return x;
}

}  // namespace

int main() {
  const double nsPerCycle = tscNsPerCycle();
  auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(1.0f));
  if (!tex) {
    std::printf("scene text: %s\n", tex.error().message.c_str());
    return 1;
  }
  PaintResources res = paintResources(*tex);
  const uint32_t W = 1280, H = 300;
  DrawList dl;
  double now = 1.0;

  // ---- full paints ----
  for (const char* name : {"utility", "saturator", "eq_eight"}) {
    Loaded l = load(name);
    Target t;
    UiTree tree;
    tree.setViewport(W, H, UiScale(1.0f));
    tree.setResources(&res);
    auto& root = tree.emplaceRoot<Widget>("root");
    root.setLayout(LayoutKind::Row);
    auto& panel = root.add<DevicePanel>("device", l.sig, l.desc, &t);
    panel.setHeight(Length::fixed(panel.logicalHeight()));
    tree.layoutNow();
    std::vector<double> ns;
    uint32_t verts = 0;
    for (int i = 0; i < 300; ++i) {
      tex->text->beginFrame();
      dl.reset(W, H);
      const uint64_t c0 = threadCycles();
      tree.paint(dl, res, now);
      ns.push_back(static_cast<double>(threadCycles() - c0) * nsPerCycle);
      verts = static_cast<uint32_t>(dl.vertices().size());
    }
    char label[160];
    std::snprintf(label, sizeof label, "device.panel full paint: %s (%u vertices)", l.desc.title.c_str(), verts);
    pq::bench::reportStats(label, pq::bench::summarize(ns), "ns CPU/frame");
  }

  // ---- interaction frames ----
  {
    Loaded l = load("utility");
    Target t;
    UiTree tree;
    tree.setViewport(W, H, UiScale(1.0f));
    tree.setResources(&res);
    auto& root = tree.emplaceRoot<Widget>("root");
    root.setLayout(LayoutKind::Row);
    auto& panel = root.add<DevicePanel>("device", l.sig, l.desc, &t);
    panel.setHeight(Length::fixed(panel.logicalHeight()));
    tree.layoutNow();
    tex->text->beginFrame();
    dl.reset(W, H);
    tree.paint(dl, res, now);
    Widget* knob = panel.controlFor("gain");
    const IRect kr = knob->rect();
    Vec2 p{static_cast<float>(kr.x + kr.w / 2), static_cast<float>(kr.y + kr.h / 2)};
    PointerEvent e;
    e.type = PointerType::Move;
    e.pos = p;
    tree.dispatchPointer(e);
    e.type = PointerType::Down;
    e.button = PointerButton::Left;
    e.clickCount = 1;
    tree.dispatchPointer(e);
    std::vector<double> ns;
    float dir = -1.0f;
    for (int i = 0; i < 2000; ++i) {
      if (i % 100 == 0) dir = -dir;
      p.y += dir;
      const uint64_t c0 = threadCycles();
      PointerEvent m;
      m.type = PointerType::Move;
      m.pos = p;
      tree.dispatchPointer(m);
      now += 1.0 / 144.0;
      tree.update(now);
      IRect rs[Damage::kMaxRects];
      int64_t area = 0;
      const uint32_t n = tree.damageRects(rs, area);
      tex->text->beginFrame();
      dl.reset(W, H);
      tree.paint(dl, res, now, std::span<const IRect>(rs, n));
      ns.push_back(static_cast<double>(threadCycles() - c0) * nsPerCycle);
    }
    pq::bench::reportStats("device.knob drag frame (Utility gain): move -> write -> damage -> partial paint", pq::bench::summarize(ns),
                           "ns CPU/frame");
    std::printf("  writes %llu\n", static_cast<unsigned long long>(t.writes));
  }
  {
    Loaded l = load("eq_eight");
    TapRing tap(2, 32768, 2048);
    dsp::SpectrumConfig sc;
    SpectrumFeed feed(tap, sc, SpectrumFeed::Manual{});
    Target t;
    t.feed = &feed;
    UiTree tree;
    tree.setViewport(W, H, UiScale(1.0f));
    tree.setResources(&res);
    auto& root = tree.emplaceRoot<Widget>("root");
    root.setLayout(LayoutKind::Row);
    auto& panel = root.add<DevicePanel>("device", l.sig, l.desc, &t);
    panel.setHeight(Length::fixed(panel.logicalHeight()));
    tree.layoutNow();
    tex->text->beginFrame();
    dl.reset(W, H);
    tree.paint(dl, res, now);
    auto frame = [&] {
      now += 1.0 / 144.0;
      tree.update(now);
      IRect rs[Damage::kMaxRects];
      int64_t area = 0;
      const uint32_t n = tree.damageRects(rs, area);
      tex->text->beginFrame();
      dl.reset(W, H);
      tree.paint(dl, res, now, std::span<const IRect>(rs, n));
    };
    // Dot drag: band 4's dot at 1 kHz, 0 dB.
    Widget* eq = panel.control("eq");
    const IRect er = eq->rect();
    const float x0 = static_cast<float>(er.x + 1), x1 = static_cast<float>(er.right() - 1);
    Vec2 p{x0 + (x1 - x0) * static_cast<float>(std::log(100.0) / std::log(2200.0)), static_cast<float>(er.y) + 0.5f * er.h};
    PointerEvent e;
    e.type = PointerType::Move;
    e.pos = p;
    tree.dispatchPointer(e);
    e.type = PointerType::Down;
    e.button = PointerButton::Left;
    e.clickCount = 1;
    tree.dispatchPointer(e);
    std::vector<double> ns;
    float dir = 1.0f;
    for (int i = 0; i < 1000; ++i) {
      if (i % 50 == 0) dir = -dir;
      p.x += dir;
      p.y += 0.5f * dir;
      const uint64_t c0 = threadCycles();
      PointerEvent m;
      m.type = PointerType::Move;
      m.pos = p;
      tree.dispatchPointer(m);
      frame();
      ns.push_back(static_cast<double>(threadCycles() - c0) * nsPerCycle);
    }
    pq::bench::reportStats("device.EQ Eight dot drag frame: freq + gain writes, response, display + knobs repainted",
                           pq::bench::summarize(ns), "ns CPU/frame");
    e.type = PointerType::Up;
    tree.dispatchPointer(e);
    frame();
    // The spectrum animating: new analysis every frame (as while audio plays), eased, the display repainted.
    feed.setSourceLive(true);
    panel.wakeDisplays();
    const std::vector<float> sig = testSignal(48000 * 4);
    size_t pos = 0;
    std::vector<double> spec, analysis;
    for (int i = 0; i < 400; ++i) {
      const uint32_t n = 48000 / 144;  // one frame of audio
      const float* ch[2] = {sig.data() + pos, sig.data() + pos};
      tap.write(ch, 2, n);
      pos = (pos + n) % (sig.size() - n);
      const uint64_t a0 = threadCycles();
      feed.step(now);
      analysis.push_back(static_cast<double>(threadCycles() - a0) * nsPerCycle);
      const uint64_t c0 = threadCycles();
      frame();
      spec.push_back(static_cast<double>(threadCycles() - c0) * nsPerCycle);
    }
    pq::bench::reportStats("device.EQ Eight spectrum frame at 144 Hz: snapshot + ease + display repaint (UI thread)",
                           pq::bench::summarize(spec), "ns CPU/frame");
    pq::bench::reportStats("device.spectrum analysis per 144 Hz frame (333 samples: tap read, mono, hops; analysis thread)",
                           pq::bench::summarize(analysis), "ns CPU/frame");
    feed.setSourceLive(false);
  }

  // ---- the analyser per hop ----
  for (uint32_t n : {2048u, 8192u}) {
    dsp::RealFft fft(n);
    const std::vector<float> sig = testSignal(n);
    std::vector<float> pw(n / 2 + 1);
    auto st = pq::bench::measure([&] { fft.power(sig.data(), pw.data()); }, 1);
    char label[96];
    std::snprintf(label, sizeof label, "device.spectrum real FFT %u (power spectrum)", n);
    pq::bench::report(label, st, "ns/FFT");
  }
  {
    dsp::SpectrumAnalyzer a;
    const std::vector<float> sig = testSignal(48000 * 2);
    size_t pos = 0;
    // Warm: fill the long window.
    a.push(sig.data(), 16384);
    pos = 16384;
    std::vector<double> ns;
    for (int i = 0; i < 1000; ++i) {
      if (pos + 512 > sig.size()) pos = 0;
      const uint64_t c0 = threadCycles();
      a.push(sig.data() + pos, 512);
      ns.push_back(static_cast<double>(threadCycles() - c0) * nsPerCycle);
      pos += 512;
    }
    pq::bench::reportStats("device.spectrum analyser per hop (512 samples: FFT 2048 + every other hop FFT 8192, 1024 points)",
                           pq::bench::summarize(ns), "ns CPU/hop");
  }

  // ---- the audio side: the tap ----
  {
    TapRing tap(2, 32768, 2048);
    std::vector<float> l(128, 0.5f), r(128, -0.5f);
    const float* ch[2] = {l.data(), r.data()};
    auto stats = pq::bench::measure([&] { tap.write(ch, 2, 128); }, 1);
    pq::bench::report("device.tap write, 128-frame stereo block, no reader (hidden display)", stats, "ns/block");
    tap.addReader();
    stats = pq::bench::measure([&] { tap.write(ch, 2, 128); }, 1);
    pq::bench::report("device.tap write, 128-frame stereo block, a reader", stats, "ns/block");
  }
  return 0;
}
