// T4.6 benchmarks: device panels (CPU only: text through the CPU atlas, as bench_circuit_editor).
//   - a full paint of each stock panel in a 1280x300 tree (what a first frame or a theme change costs)
//   - a knob drag frame (Utility gain): pointer move → value → target write → damage → partial paint of that knob
//   - an EQ Eight dot drag frame: freq / gain write, the response recomputed, the display repainted
//   - an EQ Eight spectrum frame while audio plays: snapshot → 45 ms ease → the display repainted (the analysis apart)
//   - the analysis per hop (2048-point FFT, every other hop the 8192-point one, 1024 points, ballistics)
//   - the audio side: the tap's write of a 128-frame stereo block, with a reader and without one
//   - TX.scope: the scope tracker per frame (detected / note period), 4 Oscilloscope faces per 60 Hz frame
//   - TX.spectrogram: the analysis per column, 4 Spectrogram faces per 60 Hz frame (with a GPU: the ring texture uploads)
// Results → bench/RESULTS.md (ship).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include <windows.h>
#include <intrin.h>

#include "pq/core/tap_ring.h"
#include "pq/dsp/fft.h"
#include "pq/dsp/spectrum.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/scope.h"
#include "pq/dsp/spectrogram.h"
#include "pq/dsp/transfer.h"
#include "pq/meta/oscilloscope.h"
#include "pq/meta/spectrogram.h"
#include "pq/ui/gpu.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/texture_pool.h"
#include "pq/ui/face_painter.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/devices/spectrum_plot.h"
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

  // ---- TX.spectrum: the analysis tap kernel, the transfer estimate, 8 displays ----
  {
    TapRing tap(dsp::kSignalTapChannels, 16384, 1024);
    std::vector<float> a(128, 0.5f), b(128, -0.5f), c(128, 0.25f), d(128, -0.25f);
    const float* in[4] = {a.data(), b.data(), c.data(), d.data()};
    dsp::KernelArgs args;
    args.in = in;
    args.resource = &tap;
    args.frames = 128;
    const dsp::KernelFn k = dsp::findKernel(dsp::Atom::SignalTap, 1, 0);
    auto stats = pq::bench::measure([&] { k(args); }, 1);
    pq::bench::report("device.signal_tap kernel, 128-frame block, 4 channels, no reader (hidden display)", stats, "ns/block");
    tap.addReader();
    stats = pq::bench::measure([&] { k(args); }, 1);
    pq::bench::report("device.signal_tap kernel, 128-frame block, 4 channels, a reader", stats, "ns/block");
  }
  {
    std::vector<float> axis(1024);
    for (uint32_t i = 0; i < 1024; ++i) axis[i] = 10.0f * std::pow(2200.0f, static_cast<float>(i) / 1023.0f);
    dsp::TransferAnalyzer t(dsp::TransferConfig{}, axis);
    const std::vector<float> sig = testSignal(48000 * 4);
    std::vector<float> y(sig.size());
    for (size_t i = 0; i < sig.size(); ++i) y[i] = 0.7f * sig[i] + (i > 0 ? 0.2f * sig[i - 1] : 0.0f);
    t.push(sig.data(), y.data(), 40000);  // fill the long window
    size_t pos = 40000;
    std::vector<double> ns;
    for (int i = 0; i < 1000; ++i) {
      if (pos + 1024 > sig.size()) pos = 40000;
      const uint64_t c0 = threadCycles();
      t.push(sig.data() + pos, y.data() + pos, 1024);
      ns.push_back(static_cast<double>(threadCycles() - c0) * nsPerCycle);
      pos += 1024;
    }
    pq::bench::reportStats("device.transfer estimate per hop (1024 samples: 2 FFTs 4096 + every 4th hop 2 FFTs 32768, 1024 points)",
                           pq::bench::summarize(ns), "ns CPU/hop");
  }
  {
    const uint32_t P = 1024;
    SpectrumSnapshot s;
    s.out.resize(P);
    s.peaks.resize(P);
    s.response.resize(P);
    s.confidence.assign(P, 1.0f);
    for (uint32_t p = 0; p < P; ++p) {
      s.out[p] = -20.0f - 30.0f * static_cast<float>(p) / P + 6.0f * std::sin(0.05f * static_cast<float>(p));
      s.peaks[p] = s.out[p] + 4.0f;
      s.response[p] = 6.0f * std::sin(0.01f * static_cast<float>(p));
    }
    std::vector<std::unique_ptr<SpectrumPlot>> plots;
    for (int i = 0; i < 8; ++i) {
      auto pl = std::make_unique<SpectrumPlot>();
      SpectrumPlotStyle st;
      st.peaks = true;
      st.response = true;
      pl->setStyle(st);
      pl->setShownForTest(s, static_cast<float>(P));
      plots.push_back(std::move(pl));
    }
    ui::DrawList plotDl;
    PlotText text;
    auto stats = pq::bench::measure(
        [&] {
          plotDl.reset(2560, 1440);
          for (int i = 0; i < 8; ++i) {
            const float x = 20.0f + 620.0f * static_cast<float>(i % 4), y = 20.0f + 360.0f * static_cast<float>(i / 4);
            plots[i]->paintAll(plotDl, {x, y, x + 600.0f, y + 220.0f}, *res.theme, 2.0f, text);
          }
        },
        1);
    pq::bench::report("device.8 spectrum displays, every layer (spectrum, peaks, measured response, grid), 300 x 110 at 2x",
                      stats, "ns/frame");
  }

  // ---- TX.scope: the tracker per frame, 4 Oscilloscope faces (poll + paint) ----
  {
    auto bright = [](double ph) {
      double v = 0.0;
      for (int k = 1; k <= 8; ++k) v += std::sin(2.0 * 3.14159265358979323846 * k * ph + 0.7 * k * k) / k;
      return static_cast<float>(0.45 * v);
    };
    std::vector<float> sig(48000 * 4);
    for (size_t i = 0; i < sig.size(); ++i) sig[i] = bright(220.0 * static_cast<double>(i) / 48000.0);
    for (const bool note : {false, true}) {
      dsp::ScopeTracker t;
      size_t end = 96000;
      std::vector<double> ns;
      for (int i = 0; i < 2000; ++i) {
        if (end + 800 > sig.size()) end = 96000;
        const size_t n = std::min(t.need(48000.0), end);
        const uint64_t c0 = threadCycles();
        (void)t.frame(std::span<const float>(sig.data() + end - n, n), 48000.0, note ? 220.0f : 0.0f, false);
        ns.push_back(static_cast<double>(threadCycles() - c0) * nsPerCycle);
        end += 800;
      }
      pq::bench::reportStats(note ? "device.scope tracker per frame, Pitch (Constant), the note's period (220 Hz bright wave)"
                                  : "device.scope tracker per frame, Pitch (Constant), detected (220 Hz bright wave)",
                             pq::bench::summarize(ns), "ns CPU/frame");
    }
    struct Rig {
      TapRing tap{4, 32768, 4096};
      SpectrumFeed feed{tap, AnalysisHub::config(48000.0), SpectrumFeed::Manual{}};
      std::unique_ptr<views::MetaFace> face = views::makeFace(meta::oscilloscopeType());
      uint64_t at = 0;
      double now = 1.0;
    };
    std::vector<std::unique_ptr<Rig>> rigs;
    auto feedRig = [&](Rig& r, uint32_t n) {
      std::vector<float> b(n), hz(n, 0.0f), l(n), rr(n);
      for (uint32_t i = 0; i < n; ++i) {
        b[i] = static_cast<float>(static_cast<double>(r.at + i) * 2.0 / 48000.0);
        l[i] = sig[(r.at + i) % sig.size()];
        rr[i] = 0.5f * l[i];
      }
      const float* ch[4] = {b.data(), hz.data(), l.data(), rr.data()};
      r.tap.write(ch, 4, n);
      r.at += n;
      r.feed.step(r.now += static_cast<double>(n) / 48000.0);
    };
    ui::DrawList faceDl;
    const views::FaceEnv env{res.theme, 1.0f};
    for (int i = 0; i < 4; ++i) {
      auto r = std::make_unique<Rig>();
      r->face->bind(nullptr, nullptr);
      Rig* raw = r.get();
      r->face->setAnalysis([raw] { return &raw->feed; }, [] {});
      faceDl.reset(2560, 1440);
      ui::FacePainter p(faceDl, {{0.0f, 0.0f}, 2.3f});
      r->face->paintLive(p, env);  // shown: acquires the history
      feedRig(*r, 24000);
      rigs.push_back(std::move(r));
    }
    auto stats = pq::bench::measure(
        [&] {
          for (auto& r : rigs) feedRig(*r, 800);  // a 60 Hz frame's audio (the analysis thread's side, timed too)
          faceDl.reset(2560, 1440);
          for (size_t i = 0; i < rigs.size(); ++i) {
            (void)rigs[i]->face->pollVoices();
            ui::FacePainter p(faceDl, {{20.0f + 620.0f * static_cast<float>(i % 2), 20.0f + 400.0f * static_cast<float>(i / 2)}, 2.3f});
            rigs[i]->face->paintLive(p, env);
          }
        },
        1);
    pq::bench::report("device.4 oscilloscope faces, a 60 Hz frame: 800 new frames each, poll (copy + pitch + align) + paint, 600 x 370 px",
                      stats, "ns/frame");
  }

  // ---- TX.spectrogram: the analysis per column, 4 Spectrogram faces (poll + colour + upload + paint) ----
  {
    const std::vector<float> sig = testSignal(48000 * 4);
    struct Drop final : dsp::Spectrogram::Sink {
      uint64_t n = 0;
      void column(const float*, uint64_t, double) override { ++n; }
    };
    for (const bool reassign : {true, false}) {
      dsp::SpectrogramConfig c;
      c.hop = 500;
      c.reassign = reassign;
      dsp::Spectrogram g(c);
      Drop drop;
      g.push(sig.data(), 48000, drop);  // warm
      std::vector<double> ns;
      size_t at = 48000;
      for (int i = 0; i < 400; ++i) {
        if (at + 500 > sig.size()) at = 48000;
        const uint64_t c0 = threadCycles();
        g.push(sig.data() + at, 500, drop);  // one hop: one frame (main 2048 + low 4096 chains)
        ns.push_back(static_cast<double>(threadCycles() - c0) * nsPerCycle);
        at += 500;
      }
      pq::bench::reportStats(reassign ? "device.spectrogram analysis per column (hop 500: 2048 + 4096 Kaiser, 3 FFTs each, reassigned)"
                                      : "device.spectrogram analysis per column (hop 500: 2048 + 4096 Kaiser, 1 FFT each, plain STFT)",
                             pq::bench::summarize(ns), "ns CPU/column");
    }
    // The faces with a GPU texture pool when a GPU is there (the uploads' staging copies are then timed too).
    std::unique_ptr<GpuContext> gpu;
    std::unique_ptr<Renderer> renderer;
    {
      auto gc = GpuContext::create({});
      if (gc) {
        gpu = std::move(*gc);
        auto r = Renderer::create(*gpu, OffscreenTarget::kFormat, 1);
        if (r) renderer = std::move(*r);
      }
    }
    TexturePool* pool = renderer ? renderer->images().get() : nullptr;
    struct Rig {
      TapRing tap{4, 32768, 4096};
      SpectrumFeed feed{tap, AnalysisHub::config(48000.0), SpectrumFeed::Manual{}};
      std::unique_ptr<views::MetaFace> face = views::makeFace(meta::spectrogramType());
      uint64_t at = 0;
      double now = 1.0;
    };
    std::vector<std::unique_ptr<Rig>> rigs;
    auto feedRig = [&](Rig& r, uint32_t n) {
      std::vector<float> b(n), z(n, 0.0f), l(n);
      for (uint32_t i = 0; i < n; ++i) {
        b[i] = static_cast<float>(static_cast<double>(r.at + i) * 2.0 / 48000.0);
        l[i] = sig[(r.at + i) % sig.size()];
      }
      for (uint32_t i = 0; i < n; i += 4096) {
        const uint32_t k = std::min<uint32_t>(4096, n - i);
        const float* ch[4] = {b.data() + i, z.data() + i, l.data() + i, l.data() + i};
        r.tap.write(ch, 4, k);
        r.feed.step(r.now += static_cast<double>(k) / 48000.0);
      }
      r.at += n;
    };
    ui::DrawList faceDl;
    const views::FaceEnv env{res.theme, 1.0f};
    auto paintAll = [&] {
      faceDl.reset(2560, 1440);
      for (size_t i = 0; i < rigs.size(); ++i) {
        (void)rigs[i]->face->pollVoices();
        ui::FacePainter p(faceDl, {{20.0f + 620.0f * static_cast<float>(i % 2), 20.0f + 400.0f * static_cast<float>(i / 2)}, 2.0f});
        p.setImages(pool);
        rigs[i]->face->paintLive(p, env);  // a 300 x 170 face at 2x: 600 x 340 device px
      }
    };
    for (int i = 0; i < 4; ++i) {
      auto r = std::make_unique<Rig>();
      r->face->bind(nullptr, nullptr);
      Rig* raw = r.get();
      r->face->setAnalysis([raw] { return &raw->feed; }, [] {});
      rigs.push_back(std::move(r));
    }
    paintAll();  // shown: acquire, ask for the columns
    for (auto& r : rigs) feedRig(*r, 48000);
    paintAll();
    const uint64_t bytes0 = pool ? pool->bytesUpdated() : 0;
    uint64_t frames = 0;
    auto stats = pq::bench::measure(
        [&] {
          for (auto& r : rigs) feedRig(*r, 800);  // a 60 Hz frame's audio (the analysis thread's side, timed too)
          paintAll();
          ++frames;
        },
        1);
    char label[200];
    std::snprintf(label, sizeof label,
                  "device.4 spectrogram faces, a 60 Hz frame: 800 new frames each (analysis + poll + colour + upload + paint), 600 x 340 px%s",
                  pool ? "" : " (no GPU: no upload)");
    pq::bench::report(label, stats, "ns/frame");
    if (pool && frames > 0)
      std::printf("  uploads: %.0f bytes per frame for 4 faces (%llu frames)\n",
                  static_cast<double>(pool->bytesUpdated() - bytes0) / static_cast<double>(frames), static_cast<unsigned long long>(frames));
    // The UI side alone (the columns already analysed: poll + colour + upload + paint).
    for (auto& r : rigs) feedRig(*r, 800);
    std::vector<double> ns;
    for (int i = 0; i < 300; ++i) {
      for (auto& r : rigs) feedRig(*r, 800);
      const uint64_t c0 = threadCycles();
      paintAll();
      ns.push_back(static_cast<double>(threadCycles() - c0) * nsPerCycle);
    }
    pq::bench::reportStats("device.4 spectrogram faces, the UI thread's part of a 60 Hz frame (poll + colour + upload + paint)",
                           pq::bench::summarize(ns), "ns CPU/frame");
    rigs.clear();  // the faces' textures go before the renderer
  }
  return 0;
}
