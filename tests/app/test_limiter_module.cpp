// DY.6: the Limiter module (pq/document/modules.h "the Limiter"; docs/plans/DY.md "DY.6") through the real lowering and
// the link, every block under RtScope (dynamics_fixture.h). Its input is a source group on the same audio track.
//   ceiling    the acceptance set — impulses, square bursts, full-scale noise, a 20 Hz … 20 kHz sweep, a beat each, up to
//              +12 dB over a −1 dB ceiling — at every lookahead (1.5 / 3 / 6 ms), Link Stereo: with the safety clip taken
//              out of the circuit no sample passes the ceiling (the gain alone holds it: the proof in modules.h), and the
//              link reports round(la · sr) + 6 frames; L-R and True Peak the same at 3 ms (M-S removed: Q266)
//   true peak  sines at 12 kHz (samples ±45° off the peaks: +3 dB between them) and 15.5 kHz, +6 dB over a −1 dB
//              ceiling: with True Peak the output's true peak (a 16× windowed-sinc reconstruction) ≤ ceiling + 0.2 dB;
//              without it the sample peaks hold and the true peak goes ≥ 2 dB over
//   release    a DC step 0 dB → −40 dB under a −12 dB ceiling (Release 100 ms, Auto off): the reduction the output shows
//              equals the law (max hold over L, instant attack, one-pole release, mean over L) frame by frame (0.01 dB)
//   L-R        a hot left (0 dB) and a quiet right (−20 dB) under −6 dB: L-R leaves the right channel untouched (== the
//              render with nothing over the ceiling), Stereo pulls it down with the left
//   JIT        interpreter == JIT on the acceptance set (True Peak, Auto)
//   CPU        the switches lower as literals: the default schedules no truepeak, no M/S matrix and no mux
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "dynamics_fixture.h"
#include "pq/document/edit_ops.h"
#include "pq/document/store.h"

using namespace pq;
using namespace pq::doc;
using namespace dynamics_fixture;

namespace {

constexpr double kPi = 3.141592653589793;

// An audio track: a source group, then the Limiter (module:limiter) on its path.
struct Rig {
  DocumentStore store;
  TrackId track;
  DeviceId lim;
  ops::Edit edit(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  template <class F>
  explicit Rig(F&& source) {
    store.setDeviceLibrary(&library());
    track = edit(ops::createTrack(*store.current(), store.ids(), TrackKind::Audio, {}, "A")).tracks.at(0);
    const DeviceId src = edit(ops::pasteBlocks(*store.current(), store.ids(), track, source(store.ids()))).devices.at(0);
    edit(ops::insertOnPath(*store.current(), store.ids(), src, 0));
    lim = test_blocks::addModule(store, track, "limiter");
  }
  DeviceId named(std::string_view name) const {
    const DeviceId d = test_blocks::member(*store.current(), lim, name);
    REQUIRE_MESSAGE(d.valid(), std::string(name));
    return d;
  }
  // Value block `name` set to `value` in its curve's units.
  void set(std::string_view name, double value) {
    const DeviceId v = named(name);
    const dsp::Curve& c = *findDevice(*store.current(), v).device->curve(kValueParam);
    REQUIRE(store.dispatch(SetDeviceParam{v, std::string(kValueParam), dsp::curvePosition(c, dsp::curveOutput(c, value), 0.0)}).ok());
    CHECK(valueBlockUnits(*findDevice(*store.current(), v).device) == doctest::Approx(value).epsilon(1e-6));
  }
  // The safety clip taken out: the limiter's Out reads the gain stage directly (what the proof holds without it).
  void bypassClip() {
    const DeviceId clip = named("ceiling clip");
    const Wire* in = nullptr;
    const Wire* out = nullptr;
    for (const Wire& w : *store.current()->wires) {
      if (w.to == clip && w.param == "x") in = &w;
      if (w.from == clip && w.to == lim && w.param == kGroupOutPort) out = &w;
    }
    REQUIRE(in != nullptr);
    REQUIRE(out != nullptr);
    Wire w = *out;
    w.id = store.ids().wire();
    w.from = in->from;
    w.port = in->port;
    Batch b{"bypass", {}};
    b.commands.push_back(RemoveWire{out->id});
    b.commands.push_back(AddWire{std::move(w)});
    REQUIRE(store.dispatch(b).ok());
  }
};

// The acceptance set, a beat each (0.5 s): impulses (37 Hz; L +9.5 dB, R −1.5 × L), square bursts (441 Hz in 6 Hz bursts;
// L +8 dB, R +1.6 dB), full-scale noise × 4 (+12 dB, independent channels), a 20 Hz … 20 kHz exponential sweep (L +6 dB,
// R +3.5 dB a quarter cycle on).
BlockSet acceptanceSet(IdSource& ids) {
  SourceKit k(ids);
  using R = SourceKit::Ref;
  const R beat = k.op("transport.beat", {});
  auto seg = [&](int i) { return k.sub(k.from(beat, i), k.from(beat, i + 1.0)); };
  auto sine = [&](R phase) { return k.op("sine", {{"phase", phase}}); };
  const R imp = k.op("clip", {{"x", k.mul(k.sub(k.V(37.0 / kRate), k.op("phasor", {{"hz", k.V(37.0)}})), k.V(1e9))},
                              {"lo", k.V(0.0)},
                              {"hi", k.V(1.0)}});
  const R burst = k.op("clip", {{"x", k.mul(sine(k.op("phasor", {{"hz", k.V(6.0)}})), k.V(1e6))}, {"lo", k.V(0.0)}, {"hi", k.V(1.0)}});
  const R square = k.mul(k.op("sign", {{"x", sine(k.op("phasor", {{"hz", k.V(441.0)}}))}}), burst);
  const R noiseL = k.op("noise", {}), noiseR = k.op("noise", {});
  const R hz = k.mul(k.V(20.0), k.op("exp", {{"x", k.mul(k.sub(beat, k.V(3.0)), k.V(std::log(1000.0)))}}));
  const R ph = k.op("phasor", {{"hz", hz}});
  const R sweepL = sine(ph), sweepR = sine(k.op("frac", {{"x", k.add(ph, k.V(0.25))}}));
  auto mix = [&](std::array<std::pair<R, double>, 4> parts) {
    R acc = k.V(0.0);
    for (int i = 0; i < 4; ++i) acc = k.op("muladd", {{"a", k.mul(seg(i), parts[i].first)}, {"b", k.V(parts[i].second)}, {"c", acc}});
    return acc;
  };
  const R L = mix({{{imp, 3.0}, {square, 2.5}, {noiseL, 4.0}, {sweepL, 2.0}}});
  const R Rr = mix({{{imp, -4.5}, {square, 1.2}, {noiseR, 4.0}, {sweepR, 1.5}}});
  return k.finish(L, Rr);
}

constexpr uint32_t kSetFrames = static_cast<uint32_t>(4 * kBeat);

double peak(const golden::Channels& c) { return std::max(peakOf(c[0], 0, c[0].size()), peakOf(c[1], 0, c[1].size())); }

// The true peak of `x[from, to)`: a 16× reconstruction by a Blackman-windowed sinc over ±64 samples (flat to ~0.45·sr).
double truePeak(const std::vector<float>& x, size_t from, size_t to) {
  constexpr int kHalf = 64, kOver = 16;
  static const std::vector<double> table = [] {
    std::vector<double> t(static_cast<size_t>(kOver) * (2 * kHalf + 1));
    for (int j = 0; j < kOver; ++j)
      for (int k = -kHalf; k <= kHalf; ++k) {
        const double u = static_cast<double>(j) / kOver - k;  // distance from tap k
        const double s = u == 0.0 ? 1.0 : std::sin(kPi * u) / (kPi * u);
        const double w = 0.42 + 0.5 * std::cos(kPi * u / (kHalf + 1)) + 0.08 * std::cos(2.0 * kPi * u / (kHalf + 1));
        t[static_cast<size_t>(j) * (2 * kHalf + 1) + static_cast<size_t>(k + kHalf)] = s * w;
      }
    return t;
  }();
  double p = 0.0;
  for (size_t n = std::max<size_t>(from, kHalf); n < std::min(to, x.size() - kHalf); ++n)
    for (int j = 0; j < kOver; ++j) {
      const double* t = &table[static_cast<size_t>(j) * (2 * kHalf + 1)];
      double v = 0.0;
      for (int k = -kHalf; k <= kHalf; ++k) v += t[k + kHalf] * x[n + static_cast<size_t>(k)];
      p = std::max(p, std::fabs(v));
    }
  return p;
}

}  // namespace

TEST_CASE("DY.6 ceiling: no sample over it on the acceptance set at every lookahead (the gain alone, no clip); latency L + 6") {
  const double ceiling = -1.0, c = std::pow(10.0, ceiling / 20.0);
  struct Case {
    double la;
    double link;
    double truePeak;
    bool clip;
  };
  const Case cases[] = {{0.0015, 0, 0, false}, {0.003, 0, 0, false}, {0.006, 0, 0, false},
                        {0.003, 1, 0, false},  {0.003, 0, 1, false}};
  for (const Case& k : cases) {
    CAPTURE(k.la);
    CAPTURE(k.link);
    CAPTURE(k.truePeak);
    Rig r(acceptanceSet);
    r.set("Ceiling", ceiling);
    r.set("Lookahead", k.la);
    r.set("Link", k.link);
    r.set("True Peak", k.truePeak);
    if (!k.clip) r.bypassClip();
    uint32_t latency = 0;
    const golden::Channels out = render(r.store.current(), kSetFrames, false, &latency);
    CHECK(latency == static_cast<uint32_t>(std::lround(k.la * kRate)) + 6u);
    const double p = peak(out);
    MESSAGE("peak " << db(p) << " dB (ceiling " << ceiling << ")");
    if (k.clip) {
      CHECK(p <= c);
    } else {
      CHECK(p <= c * (1.0 + 1e-5));  // float rounding of dB / gain / the mean: ~1e-6
    }
    CHECK(p >= c * 0.99);  // it limits to the ceiling, not below it
    // Every segment was over the ceiling and is pulled down near it: within 3 dB (Auto release still holds some of the
    // noise's 13 dB into the sweep), with True Peak within 6 dB (the noise's and the squares' peaks between samples sit
    // over their sample peaks).
    for (int s = 0; s < 4; ++s) {
      const size_t a = static_cast<size_t>(s * kBeat) + latency + 600, b = static_cast<size_t>((s + 1) * kBeat);
      CAPTURE(s);
      CHECK(std::max(peakOf(out[0], a, b), peakOf(out[1], a, b)) >= c * (k.truePeak == 1.0 ? 0.5 : 0.7));
    }
  }
}

TEST_CASE("DY.6 true peak: the reconstructed output stays within 0.2 dB of the ceiling with True Peak, not without") {
  const double ceiling = -1.0;
  auto source = [](IdSource& ids) {
    SourceKit k(ids);
    using R = SourceKit::Ref;
    const R beat = k.op("transport.beat", {});
    const R first = k.sub(k.V(1.0), k.from(beat, 1.0));
    // 12 kHz an eighth of a cycle on (every sample 3 dB under the peaks between them), then 15.5 kHz; amplitude 2 (+6 dB)
    const R a = k.op("sine", {{"phase", k.op("frac", {{"x", k.add(k.op("phasor", {{"hz", k.V(12000.0)}}), k.V(0.125))}})}});
    const R b = k.op("sine", {{"phase", k.op("frac", {{"x", k.add(k.op("phasor", {{"hz", k.V(15500.0)}}), k.V(0.3))}})}});
    const R x = k.mul(k.V(2.0), k.op("lerp", {{"a", b}, {"b", a}, {"t", first}}));
    return k.finish(x, x);
  };
  for (const double tp : {1.0, 0.0}) {
    CAPTURE(tp);
    Rig r(source);
    r.set("Ceiling", ceiling);
    r.set("True Peak", tp);
    const golden::Channels out = render(r.store.current(), static_cast<uint32_t>(2 * kBeat));
    const double sample = db(peak(out));
    const double over = db(std::max(truePeak(out[0], 2400, out[0].size()), truePeak(out[1], 2400, out[1].size())));
    MESSAGE("sample peak " << sample << " dB, true peak " << over << " dB (ceiling " << ceiling << ")");
    CHECK(sample <= ceiling + 1e-6);
    if (tp == 1.0) {
      CHECK(over <= ceiling + 0.2);
    } else {
      CHECK(over >= ceiling + 2.0);
    }
  }
}

TEST_CASE("DY.6 release: after a step down the reduction follows the law frame by frame (hold L, release, mean L)") {
  const double hot = 1.0, quiet = 0.01, ceiling = -12.0, release = 0.100, la = 0.003;
  Rig r([&](IdSource& ids) { return sourceBlocks(ids, hot, {{1.0, quiet}}, false); });
  r.set("Ceiling", ceiling);
  r.set("Release", release);
  r.set("Auto", 0.0);
  r.set("Lookahead", la);
  const golden::Channels out = render(r.store.current(), static_cast<uint32_t>(2 * kBeat));
  const std::vector<float>& o = out[0];
  const int L = static_cast<int>(std::lround(la * kRate));
  // Held at the ceiling while hot.
  CHECK(std::fabs(o[static_cast<size_t>(kBeat / 2)]) == doctest::Approx(std::pow(10.0, ceiling / 20.0)).epsilon(1e-5));
  // The first quiet sample in the output: the input stepped at frame s = n0 − 6 − L.
  size_t n0 = static_cast<size_t>(kBeat / 2);
  while (n0 < o.size() && std::fabs(o[n0]) > 0.05f) ++n0;
  REQUIRE(n0 < o.size());
  const long s = static_cast<long>(n0) - 6 - L;
  CHECK(std::labs(s - static_cast<long>(kBeat)) <= 1);
  // The law: r[m] = 12 dB while det (7 frames behind) is hot; h = max over L; g = instant attack, one-pole release;
  // b = mean over L.
  const double rho = std::exp(-1.0 / (release * kRate));
  const size_t end = n0 + static_cast<size_t>(4 * release * kRate);
  std::vector<double> g(end + 1, 0.0), b(end + 1, 0.0);
  double y = 0.0, sum = 0.0;
  for (size_t m = 0; m <= end; ++m) {
    // h[m] = max r[m − L + 1 … m]: 12 while some index there is ≤ s + 6
    const double h = static_cast<long>(m) - L + 1 <= s + 6 ? -ceiling : 0.0;
    y = h > y ? h : y * rho + h * (1.0 - rho);
    g[m] = y;
    sum += y;
    if (m >= static_cast<size_t>(L)) sum -= g[m - static_cast<size_t>(L)];
    b[m] = sum / L;
  }
  double worst = 0.0;
  size_t n37 = 0;
  for (size_t n = n0; n <= end; ++n) {
    const double gr = db(quiet) - db(std::fabs(o[n]));
    worst = std::max(worst, std::fabs(gr - b[n]));
    if (n37 == 0 && gr < 0.368 * -ceiling) n37 = n;
  }
  MESSAGE("reduction vs the law: worst " << worst << " dB; 37 % after " << n37 - n0 << " frames (release " << release * kRate
                                         << ", L " << L << ")");
  CHECK(worst <= 0.01);
  CHECK(n37 > n0 + static_cast<size_t>(release * kRate));
  CHECK(n37 < n0 + static_cast<size_t>(release * kRate) + static_cast<size_t>(L));
}

TEST_CASE("DY.6 Link L-R limits only the hot channel; Stereo pulls the quiet one down with it") {
  auto source = [](IdSource& ids) {
    SourceKit k(ids);
    const SourceKit::Ref s = k.op("sine", {{"phase", k.op("phasor", {{"hz", k.V(1000.0)}})}});
    return k.finish(s, k.mul(s, k.V(0.1)));
  };
  const uint32_t frames = 192 * kBlock;
  const size_t from = frames / 4;
  Rig ref(source);
  ref.set("Ceiling", 0.0);  // nothing over it (per channel): the input, delayed
  ref.set("Link", 1.0);
  const golden::Channels clean = render(ref.store.current(), frames);
  for (const double link : {1.0, 0.0}) {
    CAPTURE(link);
    Rig r(source);
    r.set("Ceiling", -6.0);
    r.set("Link", link);
    const golden::Channels out = render(r.store.current(), frames);
    CHECK(peakOf(out[0], from, frames) <= std::pow(10.0, -6.0 / 20.0) + 1e-6);
    CHECK(peakOf(out[0], from, frames) >= std::pow(10.0, -6.0 / 20.0) * 0.99);
    const double right = db(peakOf(out[1], from, frames)) - db(peakOf(clean[1], from, frames));
    MESSAGE("right channel " << right << " dB");
    if (link == 1.0) {
      double diff = 0.0;
      for (size_t i = from; i < out[1].size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(out[1][i] - clean[1][i])));
      CHECK(diff == 0.0);
    } else {
      CHECK(right == doctest::Approx(-6.0).epsilon(0.02));
    }
  }
}

TEST_CASE("DY.6 interpreter == JIT on the acceptance set (True Peak, Auto release)") {
  Rig r(acceptanceSet);
  r.set("Ceiling", -1.0);
  r.set("True Peak", 1.0);
  const golden::Channels interp = render(r.store.current(), kSetFrames);
  const golden::Channels jit = render(r.store.current(), kSetFrames, true);
  double diff = 0.0;
  for (size_t c = 0; c < 2; ++c)
    for (size_t i = 0; i < interp[c].size(); ++i) diff = std::max(diff, std::fabs(static_cast<double>(interp[c][i]) - jit[c][i]));
  MESSAGE("max |interpreter - JIT| = " << diff);
  CHECK(diff <= 1e-5);
}

TEST_CASE("DY.6 CPU: the switches lower as literals — only the chosen detector is compiled") {
  Rig r([](IdSource& ids) { return sourceBlocks(ids, 0.5, {}, false); });
  auto count = [&](const char* atom) {
    const std::map<std::string, int> m = atomsOf(r.store.current());
    const auto it = m.find(atom);
    return it == m.end() ? 0 : it->second;
  };
  // Default (Stereo, True Peak off, Auto on): |src| linked, two followers, the two windows, two delays, no mux.
  CHECK(count("truepeak") == 0);
  CHECK(count("follow") == 2);
  CHECK(count("window") == 2);
  CHECK(count("latency") == 2);
  CHECK(count("abs") == 1);
  CHECK(count("mux") == 0);
  const int packs = count("pack");  // the source's and the track's: no M/S matrix (Link M-S removed: Q266)
  r.set("Auto", 0.0);
  CHECK(count("follow") == 1);
  r.set("True Peak", 1.0);
  CHECK(count("truepeak") == 1);
  CHECK(count("abs") == 0);
  CHECK(count("latency") == 2);
  CHECK(count("mux") == 0);
  r.set("Link", 1.0);  // L-R: a gain per channel, no linking max
  CHECK(count("mux") == 0);
  CHECK(count("pack") == packs);
}

TEST_CASE("AU.dsp H4: a Limiter on a track that falls silent sleeps after its tail (Auto, True Peak, L-R) and wakes") {
  struct Case {
    const char* name;
    double truePeak, link, autoRelease;
  };
  for (const Case& c : {Case{"Auto", 0, 0, 1}, Case{"plain release", 0, 0, 0}, Case{"True Peak", 1, 0, 1}, Case{"L-R", 0, 1, 1}}) {
    CAPTURE(std::string(c.name));
    NoteRig r("limiter", {{0, kPpq / 4}, {8 * kPpq, kPpq / 4}});
    r.set("Gain", 24.0);  // over the ceiling: a tail of reduction to release
    // The reduction (dB) releases to ε = 2^-30 before the segment sleeps: τ·ln(GR / ε) ≈ 24 τ, the slowest τ Auto's
    // 4 · Release.
    r.set("Release", 0.02);
    r.set("True Peak", c.truePeak);
    r.set("Link", c.link);
    r.set("Auto", c.autoRelease);
    checkSleeps(r.store.current(), 8.0, 3.0);
  }
}

TEST_CASE("AU.dsp M4: one inf sample does not hold the Limiter's reduction; a 1e30 one releases by the release law") {
  // A −12 dB sine (no reduction) with one bad sample at beat 1: after the lookahead the output is the clean render's.
  auto renderWith = [](bool bad, double scale) {
    Rig r([&](IdSource& ids) { return spikeBlocks(ids, 0.25, bad, scale); });
    r.set("Auto", 0.0);
    r.set("Release", 0.1);
    return render(r.store.current(), static_cast<uint32_t>(6 * kBeat));
  };
  const golden::Channels clean = renderWith(false, 0.0), inf = renderWith(true, 1e38), huge = renderWith(true, 1e-8);
  const size_t from = static_cast<size_t>(kBeat + 0.01 * kRate);  // past the sample and the 3 ms lookahead
  double dInf = 0.0;
  for (size_t i = from; i < clean[0].size(); ++i) dInf = std::max(dInf, std::fabs(static_cast<double>(inf[0][i]) - clean[0][i]));
  CHECK(dInf == 0.0);
  for (float v : inf[0]) REQUIRE(std::isfinite(v));
  // 1e30 is a signal: ~600 dB of reduction, released at 100 ms (≈ 1.3 s to 0.001 dB): reduced at first, clean by the end.
  const size_t end = clean[0].size();
  CHECK(peakOf(huge[0], from, from + 2400) < 0.5 * peakOf(clean[0], from, from + 2400));
  CHECK(std::fabs(peakOf(huge[0], end - 2400, end) - peakOf(clean[0], end - 2400, end)) < 1e-3);
}
