// AU.fold (the user, 2026-11-08: "wavefolding has a bunch of clicks when folding waves … the wavefold feature in a phase
// accumulator -> wavetable setup"): the default instrument (In → Phase Accumulator → Wavetable → Envelope) with the
// Wavetable's fold static and dragged as a knob (vs the same sweep automated), rendered offline through the link.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/meta.h"
#include "pq/document/modules.h"
#include "pq/dsp/fft.h"
#include "pq/meta/wavetable.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

double degreeHz(int32_t d) { return tuning::kDefaultReferenceHz * std::exp2(d / 12.0); }

const Device* firstOf(const Project& p, std::string_view patch) {
  const Device* out = nullptr;
  forEachDevice(p, [&](const Device& d, const Track&) {
    if (out == nullptr && d.patch == patch) out = &d;
  });
  return out;
}

struct FoldSong {
  DocumentStore store;
  TrackId track;
  DeviceId wt;
  explicit FoldSong(int32_t degree, double seconds) {
    must(SetTempoMap{{{0, 120.0, TempoCurve::Step}}});
    auto t = ops::createTrack(doc(), store.ids(), TrackKind::Midi);  // the default instrument
    REQUIRE(t.ok());
    must(t->command);
    track = t->tracks.at(0);
    const int32_t deg = ((degree % 12) + 12) % 12, eq = (degree - deg) / 12;
    const Ticks len = static_cast<Ticks>(seconds * 2.0 * kPpq) + kPpq;  // 120 bpm: 2 beats per second
    std::vector<Note> ns{playback_fixture::note(store.ids(), 0, len, deg, eq)};
    must(AddClip{track, playback_fixture::clip(store.ids(), 0, len, ns, "n")});
    const Device* w = firstOf(doc(), "meta:wavetable");
    REQUIRE(w != nullptr);
    wt = w->id;
  }
  void must(Command c) { raw(playback_fixture::inUnits(store, std::move(c))); }
  void raw(Command c) {
    const Status st = store.dispatch(std::move(c));
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  void edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    raw(e->command);
  }
  const Project& doc() const { return *store.current(); }
  void fold(double v) { control("fold", v); }
  void control(const char* name, double v) { edit(ops::setMetaParamValue(doc(), wt, "", "wavetable", name, v)); }
  void lane(const char* name, std::vector<Breakpoint> points) {
    auto l = std::make_shared<AutomationLane>();
    l->id = store.ids().lane();
    l->param = ParamRef::device(wt, name);
    l->paramName = name;
    for (Breakpoint& p : points) p.id = store.ids().breakpoint();
    l->points = std::move(points);
    must(AddAutomationLane{track, l});
  }
};

std::unique_ptr<app::ProjectLink> makeLink(const Snapshot& s) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  c.sampleRate = kRate;
  auto l = app::ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

struct Driver {
  app::ProjectLink& link;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  std::vector<float> out;
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      out.insert(out.end(), l.begin(), l.end());
    }
  }
};

// Non-harmonic content of n samples from `from` (Blackman-Harris 4-term): bins farther than ±guard from every harmonic
// k·f0 (and DC). Returns {total non-harmonic energy re the whole (dB), the largest non-harmonic bin below 20 kHz (dBFS)}.
struct Nh {
  double ratioDb = -300, peakDbfs = -300;
};
Nh nonHarmonic(const std::vector<float>& x, size_t from, uint32_t n, double f0, uint32_t guard = 8) {
  std::vector<double> re(n), im(n, 0.0);
  for (uint32_t i = 0; i < n; ++i) {
    const double t = 2.0 * 3.14159265358979 * i / n;
    const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
    re[i] = x[from + i] * w;
  }
  dsp::fftInPlace(re, im, false);
  const double binHz = kRate / n;
  double all = 0, nh = 0, peak = 0;
  for (uint32_t b = 1; b < n / 2; ++b) {
    const double p = re[b] * re[b] + im[b] * im[b];
    all += p;
    const double hz = b * binHz;
    const double k = std::round(hz / f0);
    if (std::fabs(hz - k * f0) / binHz <= guard || b <= guard) continue;
    nh += p;
    if (hz < 20000.0) peak = std::max(peak, std::sqrt(p) / (0.35875 * n / 2));
  }
  return {10.0 * std::log10(std::max(nh, 1e-300) / std::max(all, 1e-300)), 20.0 * std::log10(std::max(peak, 1e-15))};
}

// The worst short-window non-harmonic ratio over [from, to): 4096-sample windows (85 ms), hop 1024, bins > 5 from every
// harmonic (the Blackman-Harris main lobe is ±4 bins: a narrower guard counts the harmonics' own leakage).
Nh worstWindow(const std::vector<float>& x, size_t from, size_t to, double f0) {
  Nh worst;
  for (size_t i = from; i + 4096 <= to; i += 1024) {
    const Nh w = nonHarmonic(x, i, 4096, f0, 5);
    worst.ratioDb = std::max(worst.ratioDb, w.ratioDb);
    worst.peakDbfs = std::max(worst.peakDbfs, w.peakDbfs);
  }
  return worst;
}

double maxStep(const std::vector<float>& x, size_t from, size_t to) {
  double m = 0.0;
  for (size_t i = std::max<size_t>(from, 1); i < to; ++i) m = std::max(m, std::fabs(double(x[i]) - x[i - 1]));
  return m;
}

void dump(const char* name, const std::vector<float>& x) {
  if (const char* dir = std::getenv("PQ_AU_FOLD_DUMP")) {
    const std::string p = std::string(dir) + "/" + name + ".f32";
    if (FILE* f = std::fopen(p.c_str(), "wb")) {
      std::fwrite(x.data(), sizeof(float), x.size(), f);
      std::fclose(f);
    }
  }
}

}  // namespace

TEST_CASE("AU.fold: a static fold through the Phase Accumulator does not alias (55 Hz … 2 kHz, fold 0.25 … 1)") {
  // Ruled out as the click's cause: the baked fold (the live-band bake) leaves no non-harmonic content above the
  // window's floor — the largest non-harmonic bin below 20 kHz ≤ −100 dBFS (measured −109.7 at worst), so the PA's
  // phase wrap, the fold's own curve and the mip choice are clean at static values.
  for (const int32_t d : {-27, 21, 36}) {
    for (const double fold : {0.25, 0.5, 1.0}) {
      FoldSong s(d, 1.0);
      s.fold(fold);
      auto link = makeLink(s.store.current());
      REQUIRE(link->waitForMetaTables());
      link->play(0);
      Driver dr{*link};
      dr.run(static_cast<uint32_t>(0.6 * kRate / kBlock));
      const Nh nh = nonHarmonic(dr.out, 8192, 16384, degreeHz(d));
      CAPTURE(degreeHz(d));
      CAPTURE(fold);
      CHECK(nh.peakDbfs <= -100.0);
      CHECK(nh.ratioDb <= -85.0);
    }
  }
}

TEST_CASE("AU.fold: a knob drag of a baked control (fold, warp, sync) is as smooth as the same steps on a live control; "
          "a live control's steps do not click" * doctest::test_suite("slow")) {
  // The user's clicks: an unwired fold is baked into the table, so a drag publishes a table per step (≈ 30 Hz, TMM.9's
  // previews), and each swap was a 64-sample (1.3 ms) crossfade: the waveform stepped at every publish. The reference
  // is the same 36 steps at the same times as an automation lane's staircase on the live path (a wired / automated
  // control), which TX.smooth ramps over 20 ms per step: how any live knob sounds when dragged. Measured as the worst
  // 85 ms window's non-harmonic energy over the sweep (worstWindow), before this fix the drag was −29.4 dB at 220 Hz
  // (880 Hz −28.6; warp −42.7; sync −19.8); with the 25 ms queued fades (frame_table_slot.h "Publication") −45.9
  // (−45.8; −59.3; −36.8).
  //   The live staircase itself clicked too: wavetable_live ramps a param-rate control from the previous block's value,
  // and after the compiler's 20 ms ramp (audio-rate blocks) that value was the one from BEFORE the ramp, so each step
  // ended in a jump back and a one-block re-ramp: −32.2 dB (−31.4; −36.7; −22.3). Fixed (kernels_wave_live.cpp): −48.1
  // (−47.8; −55.1; −38.3). Each case's `live` bound sits between the two. (A continuous per-sample ramp, MESSAGE
  // "lane", is cleaner still, −73 … −90 dB: no stepped drag is that, baked or live.)
  struct Case {
    int32_t degree;
    const char* control;
    double from, to;
    double live;  // the live staircase's bound (dB)
  };
  constexpr int kSteps = 36;
  constexpr uint32_t kStepBlocks = 25, kLead = 100;
  auto tickAt = [](double samples) { return static_cast<Ticks>(samples / kRate * 2.0 * kPpq); };  // 120 bpm
  for (const Case c : {Case{-3, "fold", 0.2, 0.8, -44.0}, Case{21, "fold", 0.2, 0.8, -44.0},
                       Case{-3, "warp", -0.5, 0.5, -50.0}, Case{-3, "sync", 0.1, 0.5, -34.0}}) {
    const int32_t d = c.degree;
    const double f0 = degreeHz(d);
    auto value = [&](int i) { return c.from + (c.to - c.from) * i / kSteps; };
    const uint32_t blocks = kLead + kSteps * kStepBlocks + kLead;
    // A knob drag: from → to in 36 steps 33 ms apart (30 Hz, as the previews arrive), each table published.
    std::vector<float> drag, stairs, ramp;
    {
      FoldSong s(d, 2.0);
      s.control(c.control, c.from);
      auto link = makeLink(s.store.current());
      [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
      REQUIRE(link->waitForMetaTables());
      link->play(0);
      Driver dr{*link};
      dr.run(kLead);
      REQUIRE(s.store.beginGesture(c.control).ok());
      for (int i = 1; i <= kSteps; ++i) {
        s.control(c.control, value(i));
        REQUIRE(link->waitForMetaTables());
        dr.run(kStepBlocks);
      }
      REQUIRE(s.store.commitGesture().ok());
      dr.run(kLead);
      drag = dr.out;
    }
    // Automation lanes (values in units: must() converts them to knob positions): the staircase (each step one tick
    // long) and the continuous ramp.
    auto laneRender = [&](std::vector<Breakpoint> points) {
      FoldSong s(d, 2.0);
      s.lane(c.control, std::move(points));
      auto link = makeLink(s.store.current());
      link->flush();
      REQUIRE(link->waitForMetaTables());
      link->play(0);
      Driver dr{*link};
      dr.run(blocks);
      return dr.out;
    };
    {
      std::vector<Breakpoint> pts{Breakpoint{0, static_cast<float>(c.from), 0.0f, {}}};
      for (int i = 1; i <= kSteps; ++i) {
        const Ticks t = tickAt(static_cast<double>(kLead + (i - 1) * kStepBlocks) * kBlock);
        pts.push_back(Breakpoint{t - 1, static_cast<float>(value(i - 1)), 0.0f, {}});
        pts.push_back(Breakpoint{t, static_cast<float>(value(i)), 0.0f, {}});
      }
      stairs = laneRender(std::move(pts));
    }
    ramp = laneRender({Breakpoint{0, static_cast<float>(c.from), 0.0f, {}},
                       Breakpoint{tickAt(kLead * kBlock), static_cast<float>(c.from), 0.0f, {}},
                       Breakpoint{tickAt(static_cast<double>(kLead + kSteps * kStepBlocks) * kBlock), static_cast<float>(c.to), 0.0f, {}}});
    const size_t a = kLead * kBlock, b = (kLead + kSteps * kStepBlocks) * kBlock;
    const Nh wd = worstWindow(drag, a, b, f0), ws = worstWindow(stairs, a, b, f0), wr = worstWindow(ramp, a, b, f0);
    MESSAGE("f0 " << f0 << " Hz, " << std::string(c.control) << " " << c.from << " -> " << c.to << ": knob drag worst window "
                  << wd.ratioDb << " dB (peak " << wd.peakDbfs << " dBFS); live staircase " << ws.ratioDb << " dB (peak "
                  << ws.peakDbfs << " dBFS); lane " << wr.ratioDb << " dB");
    CAPTURE(f0);
    CAPTURE(std::string(c.control));
    CHECK(ws.ratioDb <= c.live);
    CHECK(wd.ratioDb <= ws.ratioDb + 3.0);
    CHECK(wd.peakDbfs <= ws.peakDbfs + 3.0);
    CHECK(maxStep(drag, a, b) <= 1.05 * maxStep(ramp, a, b) + 1e-3);
    dump(("drag_" + std::string(c.control) + std::to_string(d)).c_str(), drag);
    dump(("stairs_" + std::string(c.control) + std::to_string(d)).c_str(), stairs);
    dump(("lane_" + std::string(c.control) + std::to_string(d)).c_str(), ramp);
  }
}
