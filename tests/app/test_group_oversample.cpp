// TX.group-oversample (model.h "Oversampled groups", lower.h "Oversampled groups"): any group runs its contents at
// N× the rate inside one oversample region of its track patch —
//   linear     an EQ band (a bell) at 4× renders the 1× band's magnitude spectrum (the half-bands' up · down is an
//              allpass) within 0.05 dB to 16 kHz (the chord's partials: bins within 50 dB of the peak); interpreter == JIT
//   nonlinear  a tanh drive (a sine outside the group, the drive inside) aliases ≥ 30 dB less at 4× than at 1×
//   nested     an inner oversampled group shares the outer one's region at the larger factor (2× in 4×, 8× around 2×)
//   voices     an instrument group at 2× runs its region per voice (the chord renders, its spectrum within 0.5 dB of
//              1× to 16 kHz); the region path allocates nothing (alloc trap on every block)
//   response   GroupResponse through an oversampled EQ == the DFT of its rendered impulse response (0.01 dB / 0.1°
//              to 20 kHz, after the track's constant gain), and == up · Π bands at ω/n · down; a band inside it draws
//              its own svf at ω/n
//   document   the setting: one undo step, a structural change, saved and loaded, out-of-range values refused
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <map>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/document/edit_ops.h"
#include "pq/document/group_response.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/modules.h"
#include "pq/document/serialize.h"
#include "pq/dsp/fft.h"
#include "pq/dsp/oversample.h"
#include "pq/dsp/svf_design.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "playback_fixture.h"

using namespace pq;
using namespace pq::doc;
using Cx = std::complex<double>;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

const DeviceLibrary& library() { return playback_fixture::unitsLibrary(); }

LoweredProject lowered(const Project& p) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [](TrackId) { return 0u; };
  lo.clipSlotOf = [](TrackId) { return 0u; };
  auto low = lower(p, lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  return std::move(*low);
}

// The lowered project rendered for `frames` (three notes on the event slot), every block under the alloc trap.
golden::Channels render(const Project& p, uint32_t frames, engine::UnitBackend backend = {}) {
  const LoweredProject low = lowered(p);
  CHECK(low.missingDevices.empty());
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
  for (const LoweredParam& x : low.params) {
    links.push_back({x.track, x.name, static_cast<uint32_t>(values.size())});
    values.push_back(x.value);
  }
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  co.voice.maxVoices = 8;
  co.paramLinks = links;
  co.backend = backend;
  engine::ProjectCompiler compiler(co);
  auto build = compiler.build(low.graph);
  REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
  engine::NoteListSource notes(std::vector<engine::TimedNote>{
      {0, 60000, 220.0f, 0.8f, 0.5f}, {0, 60000, 277.18f, 0.6f, 0.5f}, {0, 60000, 329.63f, 0.9f, 0.5f}});
  engine::ProjectConfig pc;
  pc.sampleRate = kRate;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 2;
  pc.numProjectParams = static_cast<uint32_t>(values.size());
  pc.eventSources.push_back(notes.source());
  auto prog = engine::ProjectProgram::create(pc, build->layout);
  REQUIRE_MESSAGE(prog.ok(), (prog.ok() ? std::string() : prog.error().message));
  for (uint32_t u = 0; u < build->images.size(); ++u) {
    const auto pub = (*prog)->publish(u, std::move(build->images[u]));
    REQUIRE_MESSAGE(pub.ok(), (pub.ok() ? std::string() : pub.error().message));
  }
  std::copy(values.begin(), values.end(), (*prog)->projectParams().begin());
  golden::Channels out(2);
  std::vector<float> l(kBlock), r(kBlock);
  float* ch[2] = {l.data(), r.data()};
  decltype(alloc_trap::countRtViolations([] {})) violations = 0;
  for (int64_t t = 0; t < static_cast<int64_t>(frames); t += kBlock) {
    engine::AudioBlock ab{nullptr, 0, ch, 2, kBlock, t};
    violations += alloc_trap::countRtViolations([&] {
      DenormalGuard dg;
      (*prog)->process(ab);
    });
    out[0].insert(out[0].end(), l.begin(), l.end());
    out[1].insert(out[1].end(), r.begin(), r.end());
  }
  CHECK(violations == 0);
  return out;
}

double peakOf(const golden::Channels& a) {
  double p = 0.0;
  for (const auto& c : a)
    for (const float x : c) p = std::max(p, static_cast<double>(std::fabs(x)));
  return p;
}
double maxDiff(const golden::Channels& a, const golden::Channels& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = 0; i < a[c].size(); ++i) {
      const double d = std::fabs(static_cast<double>(a[c][i]) - static_cast<double>(b[c][i]));
      m = std::max(m, std::isnan(d) ? 1e9 : d);
    }
  }
  return m;
}

// The Hann-windowed power spectrum (dB) of `x` from `start`, kFft samples.
constexpr uint32_t kFft = 16384;
std::vector<double> spectrumDb(const std::vector<float>& x, size_t start) {
  REQUIRE(x.size() >= start + kFft);
  std::vector<float> w(kFft), pw(kFft / 2 + 1);
  for (uint32_t i = 0; i < kFft; ++i)
    w[i] = x[start + i] * static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kFft));
  dsp::RealFft fft(kFft);
  fft.power(w.data(), pw.data());
  std::vector<double> db(pw.size());
  for (size_t k = 0; k < pw.size(); ++k) db[k] = 10.0 * std::log10(std::max(1e-30, static_cast<double>(pw[k])));
  return db;
}
double binHz(size_t k) { return static_cast<double>(k) * kRate / kFft; }

// The largest |ΔdB| between two spectra over the bins below `maxHz` within `range` dB of a's peak.
double spectrumDiff(const std::vector<double>& a, const std::vector<double>& b, double maxHz, double range) {
  const double peak = *std::max_element(a.begin(), a.end());
  double m = 0.0;
  for (size_t k = 1; k < a.size() && binHz(k) < maxHz; ++k)
    if (a[k] > peak - range) m = std::max(m, std::fabs(a[k] - b[k]));
  return m;
}

struct Doc {
  DocumentStore store;
  TrackId track;
  explicit Doc(TrackKind kind = TrackKind::Midi) {
    store.setDeviceLibrary(&library());
    track = edit(ops::createTrack(doc(), store.ids(), kind, {}, "T")).tracks.at(0);
  }
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(const Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  void cmd(Command c) {
    const Status st = store.dispatch(std::move(c));
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  DeviceId insert(const BlockSet& set, uint32_t slot) {
    const DeviceId g = edit(ops::pasteBlocks(doc(), store.ids(), track, set)).devices.at(0);
    edit(ops::insertOnPath(doc(), store.ids(), g, slot));
    return g;
  }
  DeviceId block(std::string patch, DeviceId group) {
    const DeviceId d = edit(ops::addDevice(doc(), store.ids(), track, std::move(patch), DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
    if (group.valid()) cmd(SetDeviceGroup{d, group});
    return d;
  }
  DeviceId atom(std::string name, DeviceId group) { return block(std::string(kAtomDevicePrefix) + name, group); }
  // A value block whose output is `v` (a constant: a linear curve from v to v).
  DeviceId constant(double v, DeviceId group) {
    const DeviceId d = block(std::string(kValueDevice), group);
    cmd(SetParamCurve{d, std::string(kValueParam), dsp::curvePreset(dsp::CurvePreset::Linear, v, v), true});
    return d;
  }
  void wire(DeviceId from, std::string port, DeviceId to, std::string param) {
    edit(ops::connectWire(doc(), store.ids(), from, std::move(port), to, std::move(param)));
  }
  void oversample(DeviceId g, uint32_t exponent) {
    edit(ops::setDeviceSetting(doc(), g, std::string(kGroupOversample), exponent == 0 ? std::nullopt : std::optional<double>(exponent)));
  }
  const Device& dev(DeviceId d) const { return *findDevice(doc(), d).device; }
  void set(DeviceId v, double value) {
    const dsp::Curve& c = *dev(v).curve(kValueParam);
    cmd(SetDeviceParam{v, std::string(kValueParam), dsp::curvePosition(c, dsp::curveOutput(c, value), 0.0)});
  }
};

// Every unit's optimised graph: atom counts and the oversample regions' factors.
struct Compiled {
  std::map<std::string, int> atoms;
  std::vector<uint32_t> factors;
};
Compiled compiled(const Project& p) {
  const LoweredProject low = lowered(p);
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  engine::ProjectCompiler compiler(co);
  auto build = compiler.build(low.graph);
  REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
  Compiled c;
  for (uint32_t u = 0; u < compiler.numUnits(); ++u) {
    const graph::TypedPatch& tp = *compiler.unit(u).typed;
    for (graph::NodeIndex n = 0; n < tp.patch.nodeCount(); ++n)
      if (tp.patch.node(n).kind == graph::NodeKind::Atom) ++c.atoms[std::string(graph::atomSignature(tp.patch.node(n).atom).name)];
    for (const graph::OversampleInfo& o : tp.oversample) c.factors.push_back(o.factor);
  }
  return c;
}
int count(const Compiled& c, const char* atom) {
  const auto it = c.atoms.find(atom);
  return it == c.atoms.end() ? 0 : it->second;
}

}  // namespace

TEST_CASE("TX.group-oversample: an EQ band (bell) at 4x plays the 1x band's magnitude spectrum (up · down is an "
          "allpass) within 0.05 dB to 16 kHz; one region with its converters; interpreter == JIT") {
  Doc d;
  EqBandOptions o;
  o.freqHz = 3000.0;
  o.gainDb = 9.0;
  o.q = 1.5;
  const DeviceId band = d.insert(eqBandBlocks(d.store.ids(), o), 1);
  const golden::Channels one = render(d.doc(), 40000);
  CHECK(compiled(d.doc()).factors.empty());
  d.oversample(band, 2);
  CHECK(d.store.lastChanges().structural());
  CHECK(groupOversampleFactor(d.dev(band)) == 4);
  const Compiled c = compiled(d.doc());
  CHECK(c.factors == std::vector<uint32_t>{4});
  CHECK(count(c, "oversample_up") >= 1);
  CHECK(count(c, "oversample_down") >= 1);
  CHECK(count(c, "svf") == 1);
  const golden::Channels four = render(d.doc(), 40000);
  CHECK(peakOf(four) > 0.01);
  // (bins within 50 dB of the peak: the chord's partials; the window's leakage valleys between them, lower, move with
  // the half-bands' few samples of group delay)
  const double diff = spectrumDiff(spectrumDb(one[0], 16384), spectrumDb(four[0], 16384), 16000.0, 50.0);
  MESSAGE("1x vs 4x band: max |dB| difference to 16 kHz (bins within 50 dB of the peak): " << diff);
  CHECK(diff <= 0.05);
  const golden::Channels jit = render(d.doc(), 40000, compile::jitUnitBackend());
  CHECK(maxDiff(four, jit) <= 1e-6 * peakOf(four));
}

TEST_CASE("TX.group-oversample: a tanh drive in a group (the sine outside it) aliases at least 30 dB less at 4x than "
          "at 1x") {
  // An audio track: sine 5 kHz (outside) → group [x · 6 → tanh] → the track's output.
  Doc d(TrackKind::Audio);
  const DeviceId g = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.track, std::string(kGroupDevice), DeviceKind::AudioEffect, 0)).devices.at(0);
  const double f0 = binHz(1707);  // ≈ 5001 Hz, on a bin
  const DeviceId sine = d.atom("sine_hz", DeviceId{});
  d.wire(d.constant(f0, DeviceId{}), std::string(kValueOutput), sine, "hz");
  const DeviceId mul = d.atom("mul", g), th = d.atom("tanh", g);
  d.wire(sine, "out", g, std::string(kMainPort));
  d.wire(g, std::string(kGroupInPort), mul, "a");
  d.wire(d.constant(6.0, g), std::string(kValueOutput), mul, "b");
  d.wire(mul, "out", th, "x");
  d.wire(th, "out", g, std::string(kGroupOutPort));
  // Power away from the true harmonics (odd multiples of f0 below Nyquist, ±6 bins) relative to the fundamental.
  auto aliasDb = [&](const golden::Channels& r) {
    const std::vector<double> s = spectrumDb(r[0], 8192);
    double fund = 0.0, rest = 0.0;
    for (size_t k = 8; k < s.size() && binHz(k) < 20000.0; ++k) {
      const double h = binHz(k) / f0;
      const double nearest = std::round(h);
      const bool harmonic = std::fabs(h - nearest) * f0 < 6.0 * kRate / kFft && static_cast<int>(nearest) % 2 == 1;
      const double p = std::pow(10.0, s[k] / 10.0);
      if (harmonic && nearest == 1.0) fund += p;
      else if (!harmonic) rest += p;
    }
    return 10.0 * std::log10(rest / fund);
  };
  const golden::Channels one = render(d.doc(), 32768);
  CHECK(peakOf(one) > 0.3);
  const double a1 = aliasDb(one);
  d.oversample(g, 2);
  const golden::Channels four = render(d.doc(), 32768);
  const double a4 = aliasDb(four);
  d.oversample(g, 1);
  const double a2 = aliasDb(render(d.doc(), 32768));
  d.oversample(g, 3);
  const double a8 = aliasDb(render(d.doc(), 32768));
  MESSAGE("tanh(6 · sine 5 kHz) aliasing (power off the harmonics / fundamental): 1x " << a1 << " dB, 2x " << a2
                                                                                       << " dB, 4x " << a4 << " dB, 8x " << a8 << " dB");
  CHECK(a4 <= a1 - 30.0);
  CHECK(a2 < a1);
  CHECK(a8 <= a4 + 1.0);
}

TEST_CASE("TX.group-oversample: nested oversampled groups share the outermost one's region at the largest factor") {
  Doc d;
  StockModuleOptions so;
  const DeviceId eq = d.insert(eqBlocks(d.store.ids(), so), 1);
  const EqInfo e = *eqInfo(d.doc(), eq);
  REQUIRE_FALSE(e.bands[0].empty());
  const DeviceId band = e.bands[0][0].group;
  d.oversample(eq, 2);    // 4×
  d.oversample(band, 1);  // 2× inside: covered
  CHECK(compiled(d.doc()).factors == std::vector<uint32_t>{4});
  d.oversample(eq, 1);    // 2×
  d.oversample(band, 3);  // 8× inside: the whole EQ's region at 8×
  CHECK(compiled(d.doc()).factors == std::vector<uint32_t>{8});
  d.oversample(eq, 0);  // only the band: its own region
  CHECK(compiled(d.doc()).factors == std::vector<uint32_t>{8});
  CHECK(peakOf(render(d.doc(), 8192)) > 1e-3);
  d.oversample(band, 0);
  CHECK(compiled(d.doc()).factors.empty());
}

TEST_CASE("TX.group-oversample: an instrument group at 2x runs its region per voice (the chord's spectrum within "
          "0.5 dB of 1x to 16 kHz); interpreter == JIT") {
  Doc d;
  const MainPath path = mainPath(d.doc(), *findTrackPtr(d.doc(), d.track));
  REQUIRE_FALSE(path.blocks.empty());
  const DeviceId inst = path.blocks.front()->id;
  REQUIRE(isInstrumentGroup(d.doc(), inst));
  const golden::Channels one = render(d.doc(), 40000);
  d.oversample(inst, 1);
  const Compiled c = compiled(d.doc());
  CHECK(c.factors == std::vector<uint32_t>{2});
  const golden::Channels two = render(d.doc(), 40000);
  CHECK(peakOf(two) > 0.01);
  CHECK(std::fabs(peakOf(two) / peakOf(one) - 1.0) < 0.1);
  const double diff = spectrumDiff(spectrumDb(one[0], 16384), spectrumDb(two[0], 16384), 16000.0, 40.0);
  MESSAGE("instrument group 1x vs 2x: max |dB| difference to 16 kHz (bins within 40 dB of the peak): " << diff);
  CHECK(diff <= 0.5);
  CHECK(maxDiff(two, render(d.doc(), 40000, compile::jitUnitBackend())) <= 1e-6 * peakOf(two));
}

TEST_CASE("TX.group-oversample: the response through an oversampled EQ equals its rendered impulse response "
          "(0.01 dB / 0.1 degrees to 20 kHz) and up · bands at omega/n · down; a band inside draws its svf at omega/n") {
  for (const uint32_t exponent : {0u, 2u}) {
    CAPTURE(exponent);
    const uint32_t n = 1u << exponent;
    // An audio track: a unit impulse at t = 0.1 s (1 − z1 of a step made by a delay of 1), into an EQ on its path.
    Doc d(TrackKind::Audio);
    StockModuleOptions so;
    const DeviceId eq = d.insert(eqBlocks(d.store.ids(), so), 0);
    const EqInfo e = *eqInfo(d.doc(), eq);
    const double gains[] = {6.0, -4.0, 5.0, -8.0};
    for (size_t i = 0; i < e.bands[0].size() && i < 4; ++i) d.set(e.bands[0][i].gain, gains[i]);
    const DeviceId del = d.atom("delay", DeviceId{}), z = d.atom("z1", DeviceId{}), sub = d.atom("sub", DeviceId{});
    d.cmd(SetDeviceSetting{del, "max", 0.2});
    d.wire(d.constant(1.0, DeviceId{}), std::string(kValueOutput), del, "x");
    d.wire(d.constant(0.1, DeviceId{}), std::string(kValueOutput), del, "time");
    d.wire(del, "out", z, "x");
    d.wire(del, "out", sub, "a");
    d.wire(z, "out", sub, "b");
    d.wire(sub, "out", eq, std::string(kMainPort));
    d.oversample(eq, exponent);
    const golden::Channels r = render(d.doc(), 32768);
    // The impulse arrives at 4800 (+ the delay's rounding): the first sample that is not 0.
    size_t t0 = 0;
    while (t0 < r[0].size() && r[0][t0] == 0.0f) ++t0;
    REQUIRE(t0 >= 4790);
    REQUIRE(t0 <= 4801);
    if (exponent == 0) CHECK(t0 == 4800);
    const size_t start = 4800;
    std::vector<double> hz;
    for (int k = 0; k < 24; ++k) hz.push_back(20.0 * std::pow(1000.0, k / 23.0) * 0.999);  // 20 Hz … ~20 kHz
    auto dft = [&](double f) {
      const double w = 2.0 * std::numbers::pi * f / kRate;
      Cx acc{};
      for (size_t i = start; i < r[0].size(); ++i) acc += static_cast<double>(r[0][i]) * std::polar(1.0, -w * static_cast<double>(i - start));
      return acc;
    };
    GroupResponse g;
    {
      const Status st = g.prepare(d.doc(), lowered(d.doc()), library(), eq);
      REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    }
    std::vector<Cx> h(hz.size() * g.inChannels() * g.outChannels());
    REQUIRE(g.evaluate(hz, kRate, h) == graph::ResponseKind::Exact);
    auto H = [&](size_t k) { return h[k * g.inChannels() * g.outChannels()]; };  // In ch 0 → Out ch 0
    // The track's constant gain (its mixer, the master's) from 1 kHz; the rest must follow the analytic response.
    size_t ref = 0;
    while (hz[ref] < 1000.0) ++ref;
    const Cx k0 = dft(hz[ref]) / H(ref);
    for (size_t k = 0; k < hz.size(); ++k) {
      const Cx ratio = dft(hz[k]) / (H(k) * k0);
      INFO(n << "x at " << hz[k] << " Hz: measured / analytic " << ratio);
      CHECK(std::fabs(20.0 * std::log10(std::abs(ratio))) <= 0.01);
      CHECK(std::fabs(std::arg(ratio) * 180.0 / std::numbers::pi) <= 0.1);
    }
    // The EQ's response is the product of its bands' (each its svf at ω/n, n·sr: both a band's terminals are inside
    // the region, no converters between them) between the converters; a band inside the n× EQ is the same band at 1×
    // evaluated at n·sr.
    const LoweredProject now = lowered(d.doc());
    std::vector<std::vector<Cx>> bandH;
    for (const BandInfo& b : e.bands[0]) {
      GroupResponse gb;
      REQUIRE(gb.prepare(d.doc(), now, library(), b.group).ok());
      std::vector<Cx> hb(hz.size() * gb.inChannels() * gb.outChannels());
      REQUIRE(gb.evaluate(hz, kRate, hb) == graph::ResponseKind::Exact);
      std::vector<Cx> first;
      for (size_t k = 0; k < hz.size(); ++k) first.push_back(hb[k * gb.inChannels() * gb.outChannels()]);
      bandH.push_back(std::move(first));
    }
    for (size_t k = 0; k < hz.size(); ++k) {
      const double w = 2.0 * std::numbers::pi * hz[k] / kRate;
      Cx want = dsp::os::halfBandResponse(n, w, true) * dsp::os::halfBandResponse(n, w, false);
      for (const auto& b : bandH) want *= b[k];
      CHECK(std::abs(H(k) - want) <= 1e-9 * std::max(1.0, std::abs(want)));
    }
    if (n > 1) {
      d.oversample(eq, 0);
      GroupResponse g1;
      REQUIRE(g1.prepare(d.doc(), lowered(d.doc()), library(), e.bands[0][0].group).ok());
      std::vector<Cx> h1(hz.size() * g1.inChannels() * g1.outChannels());
      REQUIRE(g1.evaluate(hz, kRate * n, h1) == graph::ResponseKind::Exact);
      for (size_t k = 0; k < hz.size(); ++k)
        CHECK(std::abs(h1[k * g1.inChannels() * g1.outChannels()] - bandH[0][k]) <= 1e-9 * std::max(1.0, std::abs(bandH[0][k])));
    }
  }
}

TEST_CASE("TX.group-oversample: the setting is one undo step and a structural change, saved and loaded; values "
          "outside 0 … 3 are refused") {
  Doc d;
  const DeviceId band = d.insert(eqBandBlocks(d.store.ids()), 1);
  d.oversample(band, 3);
  CHECK(d.store.lastChanges().structural());
  CHECK(groupOversampleFactor(d.dev(band)) == 8);
  CHECK(groupOversampleLatency(8) == doctest::Approx(5.17));
  const Result<std::string> text = serializeProject(d.doc());
  REQUIRE(text.ok());
  auto back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(groupOversampleFactor(*findDevice(*back->project, band).device) == 8);
  REQUIRE(d.store.undo());
  CHECK(groupOversampleFactor(d.dev(band)) == 1);
  CHECK(d.dev(band).setting(kGroupOversample) == nullptr);
  CHECK_FALSE(d.store.dispatch(SetDeviceSetting{band, std::string(kGroupOversample), 4.0}).ok());
  CHECK_FALSE(d.store.dispatch(SetDeviceSetting{band, std::string(kGroupOversample), 1.5}).ok());
}

TEST_CASE("TX.group-oversample: a member with an oversample region of its own (the Wavetable's Shape mode) stays "
          "outside its group's region and runs at its own factor") {
  Doc d;
  playback_fixture::clearHead(d.store, d.track);
  auto w = ops::addMetaDevice(d.doc(), d.store.ids(), d.track, "wavetable", 0);
  REQUIRE(w.ok());
  d.cmd(w->command);
  const DeviceId wt = w->devices.at(0);
  playback_fixture::asInstrumentGroups(d.store);
  d.edit(ops::setMetaSetting(d.doc(), wt, "", "wavetable", "mode", 1.0));  // Shape: oversample(2) { … } (auto ×2)
  const DeviceId g = d.dev(wt).group;
  REQUIRE(g.valid());
  const std::vector<uint32_t> own = compiled(d.doc()).factors;
  REQUIRE_FALSE(own.empty());
  d.oversample(g, 2);
  std::vector<uint32_t> both = own;
  both.push_back(4);
  std::vector<uint32_t> got = compiled(d.doc()).factors;
  std::sort(both.begin(), both.end());
  std::sort(got.begin(), got.end());
  // (the group's region holds the rest of its members — or nothing, when the Wavetable is all it has: no region)
  CHECK((got == both || got == own));
  const golden::Channels r = render(d.doc(), 8192);
  CHECK(std::isfinite(peakOf(r)));
}
