// TX.mod-all (the user, 2026-11-01: "make sure every knob is able to be audio rate modulated"): a wire into a param
// makes that param live at audio rate, for every meta module and stock device — one rule (pq/document/lower.h
// "Modulation wires"; meta::ExpandContext::modulated), no per-type lists.
//
//   repro   the user's case: a Phase accumulator's audio wired into a chain Wavetable's warp (the bottom-middle knob),
//           in Phase mode and in Shape mode (Shape mode refused the wire before: its fold / warp / sync were baked),
//           wired from the start (interpreter and JIT) and added while playing.
//   sweep   (slow) every continuous param (real, ranged, not @steps) of every stock device and meta module, in the
//           configurations that switch its section on: a free Phase accumulator at 750 Hz (a ramp 0 … 1 that is
//           exactly 0 at every 64-frame block start: 48000 / 64 = 750) through a free VCA (Bend curve, min = max = M,
//           M = ±35 % of the param's range towards its interior) wired into the param, vs the same project without the
//           wire. A param that reached its consumer only at param rate (once per block) would read the ramp's 0 at
//           every block start and render identically to the unwired project: a difference means the param responds
//           per sample. Params that cannot are listed with the reason (kNoResponse).
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/meta.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/meta/vca.h"
#include "pq/meta/wavetable.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr double kModHz = kRate / kBlock;  // the modulator's ramp is 0 at every block start

struct Song {
  DocumentStore store;
  TrackId track;
  Song() {
    must(SetTempoMap{{{0, 120.0, TempoCurve::Step}}});
    auto t = ops::createTrack(doc(), store.ids(), TrackKind::Midi);  // BasicSynth as its instrument
    REQUIRE(t.ok());
    must(t->command);
    track = t->tracks.at(0);
  }
  void must(Command c) {
    const Status st = store.dispatch(std::move(c));
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  void edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    must(e->command);
  }
  const Project& doc() const { return *store.current(); }
  DeviceId meta(std::string_view type, uint32_t index = kAppend) {
    auto e = ops::addMetaDevice(doc(), store.ids(), track, type, index);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    must(e->command);
    return e->devices.at(0);
  }
  DeviceId stock(const std::string& patch, DeviceKind kind) {
    auto e = ops::addDevice(doc(), store.ids(), track, patch, kind);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    must(e->command);
    return e->devices.at(0);
  }
  DeviceId freeMeta(std::string_view type) {
    const DeviceId d = meta(type);
    edit(ops::setDevicesFree(doc(), std::vector<DeviceId>{d}, true));
    return d;
  }
  void wire(DeviceId from, const std::string& port, DeviceId to, const std::string& param) {
    edit(ops::connectWire(doc(), store.ids(), from, port, to, param));
  }
  void notes(Ticks length) { notes({{0, length, 0}}); }
  // Notes {start, length, degree} in one clip from tick 0.
  void notes(const std::vector<std::array<Ticks, 3>>& ns) {
    std::vector<Note> v;
    Ticks end = 0;
    for (const auto& n : ns) {
      v.push_back(playback_fixture::note(store.ids(), n[0], n[1], static_cast<int32_t>(n[2])));
      end = std::max(end, n[0] + n[1]);
    }
    must(AddClip{track, playback_fixture::clip(store.ids(), 0, end, std::move(v), "n")});
  }
  // The audio-rate modulator: a free Phase accumulator at kModHz (starting at `phase`) through a free VCA of exactly `m`
  // (Bend, min = max).
  DeviceId modulator(double m, double phase = 0.0) {
    const DeviceId pa = freeMeta("phase_acc");
    edit(ops::setMetaParam(doc(), pa, "", "phase_acc", "freq", kModHz));
    if (phase != 0.0) edit(ops::setMetaParam(doc(), pa, "", "phase_acc", "phase", phase));
    const DeviceId vca = freeMeta("vca");
    edit(ops::setMetaSetting(doc(), vca, "", "vca", "curve", static_cast<double>(meta::VcaCurve::Bend)));
    edit(ops::setMetaParam(doc(), vca, "", "vca", "max", m));
    edit(ops::setMetaParam(doc(), vca, "", "vca", "min", m));
    wire(pa, "out", vca, "in");
    return vca;
  }
};

struct Rendered {
  std::vector<float> out;  // the left channel, then the right
  std::string error;
  uint32_t invalidWires = 0, refusedWires = 0;
};

Rendered render(const Snapshot& s, uint32_t blocks, bool jit = false) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
  c.sampleRate = kRate;
  auto l = app::ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  app::ProjectLink& link = **l;
  REQUIRE(link.waitForMetaTables());
  if (jit) REQUIRE(link.waitForJit(180.0));
  link.play(0);
  Rendered r;
  std::vector<float> L(kBlock), R(kBlock), right;
  int64_t t = 0;
  for (uint32_t b = 0; b < blocks; ++b) {
    float* ch[2] = {L.data(), R.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
    }
    t += kBlock;
    r.out.insert(r.out.end(), L.begin(), L.end());
    right.insert(right.end(), R.begin(), R.end());
  }
  r.out.insert(r.out.end(), right.begin(), right.end());
  r.error = link.lastError();
  r.invalidWires = link.stats().invalidWires;
  r.refusedWires = link.stats().refusedWires;
  return r;
}

double rms(const std::vector<float>& a) {
  double e = 0;
  for (float v : a) e += double(v) * v;
  return std::sqrt(e / static_cast<double>(std::max<size_t>(a.size(), 1)));
}
double rmsDiff(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  double e = 0;
  for (size_t i = 0; i < a.size(); ++i) e += (double(a[i]) - b[i]) * (double(a[i]) - b[i]);
  return std::sqrt(e / static_cast<double>(std::max<size_t>(a.size(), 1)));
}

std::vector<float> cycleOf(dsp::BasicCycle c) {
  const AlignedBuffer<float> b = dsp::basicCycle(c);
  return {b.span().begin(), b.span().end()};
}
// Saw → square: the position moves between two different frames.
meta::DataPtr sawToSquare() {
  meta::WavetableData d;
  d.keyframes.push_back({0.0f, cycleOf(dsp::BasicCycle::Saw)});
  d.keyframes.push_back({1.0f, cycleOf(dsp::BasicCycle::Square)});
  d.gaps = {dsp::GapMode::Spectral};
  return meta::wavetableData(d);
}

}  // namespace

TEST_CASE("mod-all repro: audio into a chain Wavetable's warp changes the sound, Phase and Shape mode (interpreter, JIT)") {
  // The user's patch: notes → Phase accumulator → Wavetable, and another Phase accumulator's audio wired into the
  // Wavetable's warp. Before TX.mod-all Phase mode worked and Shape mode refused the wire ("baked into its curve").
  for (const double mode : {0.0, 1.0})
    for (const bool jit : {false, true}) {
      auto build = [&](bool wired) {
        auto s = std::make_unique<Song>();
        s->meta("phase_acc", 0);
        const DeviceId wt = s->meta("wavetable");
        s->edit(ops::setMetaSetting(s->doc(), wt, "", "wavetable", "mode", mode));
        const DeviceId src = s->freeMeta("phase_acc");
        s->edit(ops::setMetaParam(s->doc(), src, "", "phase_acc", "freq", 750.0));
        s->notes(2 * kPpq);
        if (wired) s->wire(src, "out", wt, "warp");
        return s;
      };
      auto a = build(false), b = build(true);
      const Rendered ya = render(a->store.current(), 300, jit), yb = render(b->store.current(), 300, jit);
      CHECK(yb.error.empty());
      MESSAGE("mode " << mode << std::string(jit ? " JIT" : " interpreter") << ": rms " << rms(ya.out) << ", diff with the wire "
                      << rmsDiff(ya.out, yb.out));
      CHECK(rmsDiff(ya.out, yb.out) > 0.05 * rms(ya.out));
    }
}

TEST_CASE("mod-all repro: the warp wire added while playing takes effect (Shape mode: the live path's table)") {
  auto run = [&](bool wire) {
    Song s;
    const DeviceId wt = s.meta("wavetable");  // after BasicSynth: an effect in Shape mode
    CHECK(metaSettings(*findDevice(s.doc(), wt).device, "", meta::wavetableType())[meta::kWtMode] == 1.0);
    const DeviceId src = s.freeMeta("phase_acc");
    s.edit(ops::setMetaParam(s.doc(), src, "", "phase_acc", "freq", 750.0));
    s.notes(8 * kPpq);
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = false;
    c.sampleRate = kRate;
    auto l = app::ProjectLink::create(c, s.store.current());
    REQUIRE(l.ok());
    app::ProjectLink& link = **l;
    [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link.onDocumentChange(e); });
    REQUIRE(link.waitForMetaTables());
    link.play(0);
    std::vector<float> L(kBlock), R(kBlock), out;
    int64_t t = 0;
    auto blocks = [&](uint32_t n) {
      for (uint32_t b = 0; b < n; ++b) {
        float* ch[2] = {L.data(), R.data()};
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
        t += kBlock;
        out.insert(out.end(), L.begin(), L.end());
      }
    };
    blocks(100);
    if (wire) s.wire(src, "out", wt, "warp");
    link.flush();
    REQUIRE(link.waitForMetaTables());
    blocks(300);
    CHECK(link.lastError().empty());
    return std::vector<float>(out.end() - 200 * kBlock, out.end());
  };
  const std::vector<float> a = run(false), b = run(true);
  MESSAGE("Shape mode, wire added while playing: rms " << rms(a) << ", diff " << rmsDiff(a, b));
  CHECK(rmsDiff(a, b) > 0.05 * rms(a));
}

// Shape mode's live path reads the same curve as its baked ADAA path: a sine (a Phase accumulator into a Phase-mode
// Wavetable) shaped by a second Wavetable in Shape mode with warp 0.5 and fold 0.3, baked (no wire) vs live (a wire
// carrying exactly 0 into each: the live path at the same static values). They differ only by the two band-limiting
// methods (first-order ADAA at ×2 vs the live read's band at ×16 with its corrections, each with its own fraction of a
// sample of delay).
TEST_CASE("mod-all: Shape mode's live path at static values matches its baked curve") {
  auto build = [&](bool live) {
    auto s = std::make_unique<Song>();
    s->meta("phase_acc", 0);
    s->meta("wavetable");  // the sine
    const DeviceId shaper = s->meta("wavetable");
    s->edit(ops::setMetaSetting(s->doc(), shaper, "", "wavetable", "mode", 1.0));
    s->edit(ops::setMetaParam(s->doc(), shaper, "", "wavetable", "warp", 0.5));
    s->edit(ops::setMetaParam(s->doc(), shaper, "", "wavetable", "fold", 0.3));
    s->notes(2 * kPpq);
    if (live) {
      const DeviceId zero = s->modulator(0.0);
      s->wire(zero, "out", shaper, "warp");
      s->wire(zero, "out", shaper, "fold");
    }
    return s;
  };
  auto a = build(false), b = build(true);
  const Rendered ya = render(a->store.current(), 600), yb = render(b->store.current(), 600);
  // From 0.1 s (past the note's start): the steady shaped tone.
  const size_t from = 4800, to = ya.out.size() / 2;
  const std::vector<float> sa(ya.out.begin() + from, ya.out.begin() + to), sb(yb.out.begin() + from, yb.out.begin() + to);
  // The live path runs at ×16 (its converters' and corrections' delay) where the ADAA curve runs at ×2: the live output
  // is a little later. Aligned by the delay that fits best (0 … 4 samples in steps of 1/32, linear interpolation), the
  // two are the same curve.
  double best = 1e9, bestDelay = 0.0;
  for (int k = 0; k <= 128; ++k) {
    const double d = k / 32.0;
    double e = 0.0, s = 0.0;
    for (size_t i = 8; i + 8 < sa.size(); ++i) {
      const double t = static_cast<double>(i) + d;
      const auto j = static_cast<size_t>(t);
      const double f = t - static_cast<double>(j);
      const double v = sb[j] * (1.0 - f) + sb[j + 1] * f;
      e += (v - sa[i]) * (v - sa[i]);
      s += static_cast<double>(sa[i]) * sa[i];
    }
    const double db = 10.0 * std::log10(std::max(e, 1e-24) / s);
    if (db < best) {
      best = db;
      bestDelay = d;
    }
  }
  const double raw = 20.0 * std::log10(std::max(rmsDiff(sa, sb), 1e-12) / rms(sa));
  MESSAGE("Shape mode, warp 0.5 + fold 0.3 on a 261.6 Hz sine: live vs baked " << raw << " dB re the signal; aligned by "
                                                                               << bestDelay << " samples " << best << " dB");
  CHECK(rms(sa) > 0.05);
  CHECK(bestDelay > 0.5);
  CHECK(bestDelay < 4.0);
  CHECK(best < -30.0);
}

namespace {

// ---- the sweep ------------------------------------------------------------------------------------------------------

// The notes {start, length, degree}: by default one note from 25 samples in (kPpq / 960 at 120 BPM), where the
// modulator's ramp is 25/64, so what a module samples at note-on (an envelope's times, a key scaling) reads a value a
// param-rate read (the block start: 0) would not.
constexpr Ticks kNoteStart = kPpq / 960;
static_assert(kNoteStart * 960 == kPpq);
using Notes = std::vector<std::array<Ticks, 3>>;

// One way to set a device up: settings (structural) and params set before the sweep, the notes it plays.
struct Config {
  std::string name;
  std::vector<std::pair<std::string, double>> settings, params;
  Notes notes = {{kNoteStart, kPpq, 0}};
};
struct Param {
  std::string name;
  double def = 0.0, min = 0.0, max = 1.0;
  ParamTravel travel;  // TX.knob-curves: its declared knob curve (an Add wire adds in its input domain)
};
// A device under test: how its song is built (returns the device), its params, its configurations.
struct Dut {
  std::string label;
  std::function<DeviceId(Song&, const Config&)> build;
  std::vector<Param> params;
  std::vector<Config> configs;
};

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  return **lib;
}

std::vector<Param> stockParams(const std::string& patch) {
  const DeviceInfo* info = library().catalog().find(patch);
  REQUIRE_MESSAGE(info != nullptr, patch);
  std::vector<Param> out;
  for (const DeviceParamInfo& p : info->params)
    if (p.kind == graph::Kind::Real && p.hasRange && p.steps == 0 && p.max > p.min)
      out.push_back({p.name, p.defaultValue, p.min, p.max,
                     paramTravel(*findParamDecl(*library().library().find(info->patch.empty() ? patch : info->patch), p.name))});
  return out;
}

std::vector<Param> metaParams(std::string_view type) {
  const meta::Type* t = meta::findType(type);
  REQUIRE(t != nullptr);
  std::vector<Param> out;
  for (const meta::ParamSpec& p : t->params) {
    ParamTravel tr;  // the expansion's declaration (pq/meta/type.h Curve → @curve)
    tr.lo = p.min;
    tr.hi = p.max;
    tr.log = p.curve == meta::Curve::Exp && p.min > 0.0;
    tr.exp0 = p.curve == meta::Curve::Exp0 ? p.curveK : 0.0;
    out.push_back({std::string(p.name), p.def, p.min, p.max, tr});
  }
  return out;
}

void configure(Song& s, DeviceId d, const Config& c, std::string_view metaType) {
  for (const auto& [k, v] : c.settings) {
    if (metaType.empty()) s.must(SetDeviceSetting{d, k, v});
    else s.edit(ops::setMetaSetting(s.doc(), d, "", metaType, k, v));
  }
  for (const auto& [k, v] : c.params) {
    if (metaType.empty()) s.must(SetDeviceParam{d, k, v});
    else s.edit(ops::setMetaParam(s.doc(), d, "", metaType, k, v));
  }
}

DeviceId stockInstrument(Song& s, const std::string& patch, const Config& c) {
  const DeviceId d = patch == "BasicSynth" ? s.doc().tracks.front()->devices.front()->id : s.stock(patch, DeviceKind::Instrument);
  configure(s, d, c, "");
  return d;
}
DeviceId stockEffect(Song& s, const std::string& patch, const Config& c) {
  const DeviceId d = s.stock(patch, DeviceKind::AudioEffect);  // after BasicSynth
  configure(s, d, c, "");
  return d;
}

Config with(Config c, std::string name, std::vector<std::pair<std::string, double>> settings,
            std::vector<std::pair<std::string, double>> params = {}) {
  c.name = std::move(name);
  c.settings.insert(c.settings.end(), settings.begin(), settings.end());
  c.params.insert(c.params.end(), params.begin(), params.end());
  return c;
}

std::vector<Dut> duts() {
  std::vector<Dut> d;
  auto one = [](std::string name) { return std::vector<Config>{Config{std::move(name), {}, {}}}; };
  d.push_back({"Basic Synth", [](Song& s, const Config& c) { return stockInstrument(s, "BasicSynth", c); },
               stockParams("BasicSynth"), one("default")});
  {
    // Operator: every oscillator on and heard (algorithm 11: all carriers; levels −6 dB), the filter, pitch envelope,
    // LFO and spread on with their amounts up; an octave above the key-scaling reference. Then: User partials with
    // looping envelopes, fixed frequencies, the LFO synced, the Morph filter through a driven circuit, and two
    // overlapping notes (glide).
    // The envelopes get a 20 ms attack (so Initial is heard) and a sustain 12 dB below the peak (so the decay moves);
    // looping, their decay settles within 10 ms and they repeat every 50 ms.
    Config all{"all sections",
               {{"algorithm", 11}, {"onB", 1}, {"onC", 1}, {"onD", 1}, {"filterOn", 1}, {"pitchEnvOn", 1}, {"lfoOn", 1},
                {"glideOn", 1}, {"spreadOn", 1}},
               {{"levelB", 0.5}, {"levelC", 0.5}, {"levelD", 0.5}, {"penvAmount", 1200.0}, {"fenvAmount", 2400.0},
                {"penvAttack", 0.02}, {"fenvAttack", 0.02}},
               {{kNoteStart, kPpq, 12}}};
    std::vector<std::pair<std::string, double>> user, loopParams, fixed;
    for (const char* o : {"A", "B", "C", "D"}) {
      all.params.push_back({std::string("attack") + o, 0.02});
      all.params.push_back({std::string("sustain") + o, 0.25});
      user.push_back({std::string("wave") + o, 22});
      user.push_back({std::string("loop") + o, 1});
      loopParams.push_back({std::string("decay") + o, 0.01});  // settles at the sustain within the note
      loopParams.push_back({std::string("repeat") + o, 0.05});
      fixed.push_back({std::string("fixed") + o, 1});
    }
    user.push_back({"pitchLoop", 1});
    user.push_back({"filterLoop", 1});
    for (const char* e : {"penv", "fenv"}) {
      loopParams.push_back({std::string(e) + "Repeat", 0.05});
      loopParams.push_back({std::string(e) + "Decay", 0.01});  // settles within the note, so the loop's wait starts
    }
    Config glide = with(all, "two overlapping notes (glide)", {});
    glide.notes = {{kNoteStart, kPpq, 12}, {kPpq / 2 + kNoteStart, kPpq / 2, 19}};
    d.push_back({"Operator", [](Song& s, const Config& c) { return stockInstrument(s, "operator", c); },
                 stockParams("operator"),
                 {all, with(all, "user partials, looping envelopes", user, loopParams), with(all, "fixed frequencies", {}, fixed),
                  with(all, "LFO synced", {}, {{"lfoSync", 1}}),
                  with(all, "Morph filter, OSR circuit", {{"filterType", 4}, {"filterCircuit", 1}}), glide}});
  }
  d.push_back({"Meta Operator", [](Song& s, const Config& c) { return stockInstrument(s, "meta_operator", c); },
               stockParams("meta_operator"),
               {Config{"algorithm 1, hq, feedback", {{"algorithm", 1}, {"hq", 1}, {"fbOn", 1}}, {}}}});
  // Utility: after another Utility that makes the signal stereo (its balance), so width and mid/side have a side.
  d.push_back({"Utility", [](Song& s, const Config& c) {
                 const DeviceId pre = s.stock("utility", DeviceKind::AudioEffect);
                 s.must(SetDeviceParam{pre, "balance", -0.6});
                 return stockEffect(s, "utility", c);
               },
               stockParams("utility"),
               {Config{"default", {}, {}}, Config{"bass mono", {{"bassMono", 1}}, {}}, Config{"mid/side", {}, {{"msMode", 1}}}}});
  d.push_back({"Saturator", [](Song& s, const Config& c) { return stockEffect(s, "saturator", c); }, stockParams("saturator"),
               {Config{"default", {}, {}}, Config{"colour (depth +6 dB)", {{"color", 1}}, {{"depth", 2.0}}},
                Config{"waveshaper", {{"type", 7}}, {{"wsDepth", 0.5}, {"wsDamp", 0.5}}}}});
  {
    // EQ Eight: every band on as a bell (Stereo), then L/R so the B bands play on the right channel; gains +4.
    Config bells{"all bells", {{"on1", 1}, {"on8", 1}}, {}};
    for (int b = 1; b <= 8; ++b) bells.settings.push_back({"type" + std::to_string(b), 3});
    Config lr = bells;
    lr.name = "L/R, all bells";
    lr.settings.push_back({"mode", 1});
    lr.settings.push_back({"onB1", 1});
    lr.settings.push_back({"onB8", 1});
    for (int b = 1; b <= 8; ++b) lr.settings.push_back({"typeB" + std::to_string(b), 3});
    for (Config* c : {&bells, &lr})
      for (int b = 1; b <= 8; ++b) {
        c->params.push_back({"gain" + std::to_string(b), 1.585});
        c->params.push_back({"gainB" + std::to_string(b), 1.585});
      }
    d.push_back({"EQ Eight", [](Song& s, const Config& c) { return stockEffect(s, "eq_eight", c); }, stockParams("eq_eight"),
                 {bells, lr}});
  }
  // Auto Filter: its sections on in turn; the sidechain's follower hears a free Phase accumulator at 110 Hz.
  d.push_back({"Auto Filter", [](Song& s, const Config& c) {
                 const DeviceId af = stockEffect(s, "auto_filter", c);
                 if (std::find_if(c.settings.begin(), c.settings.end(), [](const auto& kv) { return kv.first == "scOn"; }) !=
                     c.settings.end()) {
                   const DeviceId sc = s.freeMeta("phase_acc");
                   s.edit(ops::setMetaParam(s.doc(), sc, "", "phase_acc", "freq", 110.0));
                   s.wire(sc, "out", af, "sc");
                 }
                 return af;
               },
               stockParams("auto_filter"),
               {Config{"default", {}, {}}, Config{"OSR circuit", {{"circuit", 1}}, {}},
                Config{"LFO", {{"lfo", 1}}, {{"lfoAmount", 0.5}}},
                Config{"LFO synced", {{"lfo", 1}}, {{"lfoAmount", 0.5}, {"lfoSync", 1}}},
                Config{"envelope", {{"env", 1}}, {{"envAmount", 0.5}}}, Config{"morph", {{"type", 4}}, {}},
                Config{"DJ", {{"type", 5}}, {}}, Config{"sidechain", {{"env", 1}, {"scOn", 1}}, {{"envAmount", 0.5}}}}});
  // The LFO's own output is its export: wired into a Utility's gain after it, so its params reach the sound.
  d.push_back({"LFO", [](Song& s, const Config& c) {
                 const DeviceId lfo = stockEffect(s, "lfo_device", c);
                 const DeviceId util = s.stock("utility", DeviceKind::AudioEffect);
                 s.wire(lfo, "lfo", util, "gain");
                 return lfo;
               },
               stockParams("lfo_device"), {Config{"free", {}, {}}, Config{"synced", {}, {{"sync", 1}}}}});
  d.push_back({"Phase Accumulator", [](Song& s, const Config&) { return s.meta("phase_acc", 0); }, metaParams("phase_acc"),
               one("the instrument")});
  auto wavetable = [](double mode) {
    return [mode](Song& s, const Config&) {
      if (mode == 0.0) s.meta("phase_acc", 0);  // the note's phase; else BasicSynth's audio (Shape mode)
      const DeviceId wt = s.meta("wavetable");
      s.edit(ops::setMetaSetting(s.doc(), wt, "", "wavetable", "mode", mode));
      s.edit(ops::setMetaData(s.doc(), wt, "", "wavetable", sawToSquare()));
      return wt;
    };
  };
  d.push_back({"Wavetable (Phase mode)", wavetable(0.0), metaParams("wavetable"), one("after a Phase accumulator")});
  d.push_back({"Wavetable (Shape mode)", wavetable(1.0), metaParams("wavetable"), one("after Basic Synth")});
  d.push_back({"Envelope", [](Song& s, const Config&) {
                 s.meta("phase_acc", 0);
                 s.meta("wavetable");
                 return s.meta("envelope");
               },
               metaParams("envelope"), one("after a Wavetable")});
  d.push_back({"VCA", [](Song& s, const Config& c) {
                 s.meta("phase_acc", 0);
                 s.meta("wavetable");
                 const DeviceId v = s.meta("vca");
                 configure(s, v, c, "vca");
                 return v;
               },
               metaParams("vca"),
               {Config{"Log curve", {}, {}}, Config{"Bend curve", {{"curve", static_cast<double>(meta::VcaCurve::Bend)}}, {}}}});
  return d;
}

// Params that cannot respond to an audio-rate wire, and why (the sweep checks that each of them really does not, so a
// fix shows up here).
const std::map<std::string, std::string>& noResponse() {
  static const std::map<std::string, std::string> m = {};
  return m;
}

constexpr uint32_t kBlocks = 563;      // 0.75 s: the note (0.5 s) and its release
constexpr double kMinResponse = 1e-4;  // rms of the difference over the rms of the unwired render

// How a param answered: the largest relative response over the configurations with the modulator starting at phase
// 0 (zero at every block start: a per-sample response) and, where that showed none, at phase ½ (a modulator in a
// voice runs from the note's start, so what the note's first samples decide — an attack that a zero time makes
// instant, an initial level — reads the modulation only when it is not 0 there).
struct Answer {
  double perSample = 0.0, offset = 0.0;
  std::string where;
};

void sweep(const Dut& dut, uint32_t& tested, std::vector<std::string>& silent) {
  std::map<std::string, Answer> best;
  for (const double phase : {0.0, 0.5})
    for (const Config& c : dut.configs) {
      Song ref;
      (void)dut.build(ref, c);
      ref.notes(c.notes);
      const Rendered r0 = render(ref.store.current(), kBlocks);
      REQUIRE_MESSAGE(r0.error.empty(), dut.label << " / " << c.name << ": " << r0.error);
      const double level = rms(r0.out);
      REQUIRE_MESSAGE(level > 1e-3, dut.label << " / " << c.name << " is silent");
      for (const Param& p : dut.params) {
        Answer& a = best[p.name];
        if (phase != 0.0 && a.perSample > kMinResponse) continue;  // answered per sample already
        // TX.knob-curves: 35 % of the knob's input domain towards its interior — travel on a curved knob, dB on a dB
        // gain, else the units (an Add wire adds before the curve).
        const bool curved = !travelsInUnits(p.travel);
        const double span = curved ? 1.0 : p.travel.db ? 20.0 * std::log10(p.max / p.min) : p.max - p.min;
        const double frac = curved ? travelPosition(p.travel, p.def) : p.travel.db ? std::log10(p.def / p.min) * 20.0 / span
                                                                                  : (p.def - p.min) / span;
        const double m = std::clamp((frac < 0.5 ? 0.35 : -0.35) * span, -meta::kVcaRange, meta::kVcaRange);
        Song s;
        const DeviceId d = dut.build(s, c);
        s.notes(c.notes);
        const DeviceId mod = s.modulator(m, phase);
        s.wire(mod, "out", d, p.name);
        const Rendered r = render(s.store.current(), kBlocks);
        CHECK_MESSAGE(r.error.empty(), dut.label << " / " << c.name << " / " << p.name << ": " << r.error);
        CHECK(r.invalidWires == 0);
        CHECK(r.refusedWires == 0);
        const double resp = rmsDiff(r0.out, r.out) / level;
        double& slot = phase == 0.0 ? a.perSample : a.offset;
        if (resp > slot) {
          slot = resp;
          a.where = c.name;
        }
      }
    }
  for (const Param& p : dut.params) {
    ++tested;
    const Answer& a = best[p.name];
    const std::string key = dut.label + " " + p.name;
    const auto why = noResponse().find(key);
    char buf[96];
    if (a.perSample > kMinResponse) {
      std::snprintf(buf, sizeof buf, "%.3g", a.perSample);
      MESSAGE(key << ": responds per sample (" << buf << ", " << a.where << ")");
      CHECK_MESSAGE(why == noResponse().end(), key << " is listed as unable to respond, but it does (" << buf << ")");
    } else if (a.offset > kMinResponse) {
      std::snprintf(buf, sizeof buf, "%.3g", a.offset);
      MESSAGE(key << ": responds (" << buf << ", " << a.where << "; with the modulator at 0 at the note's start it "
                  << "had no effect: the note's first samples decide it)");
      CHECK_MESSAGE(why == noResponse().end(), key << " is listed as unable to respond, but it does (" << buf << ")");
    } else {
      std::snprintf(buf, sizeof buf, "%.3g / %.3g", a.perSample, a.offset);
      silent.push_back(key + " (" + buf + ")");
      CHECK_MESSAGE(why != noResponse().end(), key << " does not respond to an audio-rate wire (" << buf << ")");
      if (why != noResponse().end()) MESSAGE(key << ": cannot (" << why->second << ")");
    }
  }
}

}  // namespace

TEST_CASE("mod-all sweep: every continuous param of every stock device and meta module responds to an audio-rate wire" *
          doctest::test_suite("slow")) {
  uint32_t tested = 0;
  std::vector<std::string> silent;
  for (const Dut& dut : duts()) {
    INFO(dut.label);
    sweep(dut, tested, silent);
  }
  std::string list;
  for (const std::string& s : silent) list += "\n  " + s;
  MESSAGE("mod-all sweep: " << tested << " params; " << silent.size() << " do not respond:" << list);
  CHECK(tested >= 250);
}
