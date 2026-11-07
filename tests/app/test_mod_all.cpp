// TX.mod-all (the user, 2026-11-01: "make sure every knob is able to be audio rate modulated"): a wire into a param
// makes that param live at audio rate, for every engine and module (GU.11: a module's controls are value blocks) —
// one rule (pq/document/lower.h "Modulation wires"; meta::ExpandContext::modulated), no per-type lists.
//
//   repro   the user's case: a Phase accumulator's audio wired into a chain Wavetable's warp (the bottom-middle knob),
//           in Phase mode and in Shape mode (Shape mode refused the wire before: its fold / warp / sync were baked),
//           wired from the start (interpreter and JIT) and added while playing.
//   sweep   (slow) every continuous param (real, ranged, not @steps) of every module (its value blocks) and engine, in the
//           configurations that switch its section on: a free Phase Accumulator at 750 Hz (a ramp 0 … 1 that is
//           exactly 0 at every 64-frame block start: 48000 / 64 = 750) through a free VCA (its knob a constant M,
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
#include <optional>
#include <tuple>
#include <memory>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/meta.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/document/main_path.h"
#include "pq/document/modules.h"
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
    auto t = ops::createTrack(doc(), store.ids(), TrackKind::Midi);  // the default instrument group heads its path
    REQUIRE(t.ok());
    must(t->command);
    track = t->tracks.at(0);
  }
  // GU.11: the default instrument group deleted: the path empty, so an engine at slot 0 takes the notes.
  void bare() { playback_fixture::clearHead(store, track); }
  // A command built by hand: its device params in units (GU.1: stored as knob positions, tests/support/knob_positions.h).
  void must(Command c) { raw(playback_fixture::inUnits(store, std::move(c))); }
  void raw(Command c) {
    const Status st = store.dispatch(std::move(c));
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  // An edit op's command as it is (ops::setDeviceParam takes a position, setDeviceParamValue converts itself).
  void edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    raw(e->command);
  }
  const Project& doc() const { return *store.current(); }
  DeviceId meta(std::string_view type, uint32_t index = kAppend) {
    auto e = ops::addMetaDevice(doc(), store.ids(), track, type, index);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    must(e->command);
    return e->devices.at(0);
  }
  // GU.11: a native engine / a module ("module:<key>") spliced in at the path's end (after the instrument group, not
  // into its voice run); a module off the path.
  DeviceId metaAfter(std::string_view type) { return test_blocks::appendEngineAfter(store, track, type); }
  DeviceId moduleAfter(std::string_view key) { return test_blocks::appendAfter(store, track, "module:" + std::string(key)); }
  DeviceId moduleOff(std::string_view key) {
    auto e = ops::addDevice(doc(), store.ids(), track, "module:" + std::string(key), DeviceKind::AudioEffect, ops::kOffPath);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    must(e->command);
    return e->devices.at(0);
  }
  // GU.4: a built-in module pasted off the main path; `blocks` its pasted ids (local id k + 1 → index k).
  DeviceId module(const BlockSet& set, std::vector<DeviceId>* blocks = nullptr) {
    auto e = ops::pasteBlocks(doc(), store.ids(), track, set);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    must(e->command);
    if (blocks != nullptr) *blocks = e->devices;
    return e->devices.at(0);
  }
  // GU.4: a free Phase Accumulator module at `hz` (from `phase`); returns its group.
  DeviceId freePa(double hz, double phase = 0.0) {
    IdSource local;
    PhaseAccumulatorOptions o;
    o.freq = dsp::curvePosition(phaseAccumulatorCurve(kPaFrequency), hz, 0.0);
    o.phase = phase;
    return module(phaseAccumulatorBlocks(local, o));
  }
  DeviceId freeMeta(std::string_view type) {  // TX.all-free: a block placed off the main path (unwired)
    auto e = ops::addMetaDevice(doc(), store.ids(), track, type, ops::kOffPath);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    must(e->command);
    return e->devices.at(0);
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
  // The audio-rate modulator: a free Phase Accumulator at kModHz (starting at `phase`) through a free VCA of exactly `m`
  // (GU.4: the modules; the VCA's knob a constant curve, min = max = m).
  DeviceId modulator(double m, double phase = 0.0) {
    const DeviceId pa = freePa(kModHz, phase);
    IdSource local;
    VcaOptions vo;
    vo.curve = dsp::curvePreset(dsp::CurvePreset::Linear, m, m);
    const DeviceId vca = module(vcaBlocks(local, vo));
    wire(pa, "", vca, "");
    return vca;
  }
};

struct Rendered {
  std::vector<float> out;  // the left channel, then the right
  std::string error;
  uint32_t invalidWires = 0, refusedWires = 0;
};

// IG.1-2: rendered as an older file of the project loads: its meta voice runs in instrument groups (per note), as written.
Rendered render(const Snapshot& s0, uint32_t blocks, bool jit = false) {
  Project gp = *s0;
  const Snapshot s = migrateToInstrumentGroups(gp) ? std::make_shared<const Project>(std::move(gp)) : s0;
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
  // (GU.4: the Wavetable first reads the note's phase itself — what the default Phase accumulator gave it —, the other
  // accumulator is the module.)
  for (const double mode : {0.0, 1.0})
    for (const bool jit : {false, true}) {
      auto build = [&](bool wired) {
        auto s = std::make_unique<Song>();
        s->bare();
        const DeviceId wt = s->meta("wavetable", 0);
        s->edit(ops::setMetaSetting(s->doc(), wt, "", "wavetable", "mode", mode));
        const DeviceId src = s->freePa(750.0);
        s->notes(2 * kPpq);
        if (wired) s->wire(src, "", wt, "warp");
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
    const DeviceId wt = s.metaAfter("wavetable");  // after the instrument group: an effect (in Shape mode)
    s.edit(ops::setMetaSetting(s.doc(), wt, "", "wavetable", "mode", 1.0));
    CHECK(metaSettings(*findDevice(s.doc(), wt).device, "", meta::wavetableType())[meta::kWtMode] == 1.0);
    const DeviceId src = s.freePa(750.0);
    s.notes(8 * kPpq);
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = false;
    c.sampleRate = kRate;
    Project gp = *s.store.current();
    auto l = app::ProjectLink::create(c, migrateToInstrumentGroups(gp) ? std::make_shared<const Project>(std::move(gp)) : s.store.current());  // IG.1-2
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
    if (wire) s.wire(src, "", wt, "warp");
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
    s->bare();
    s->meta("wavetable", 0);  // the sine (reading the note's phase: GU.4, it followed a Phase accumulator)
    const DeviceId shaper = s->meta("wavetable");
    s->edit(ops::setMetaSetting(s->doc(), shaper, "", "wavetable", "mode", 1.0));
    s->edit(ops::setMetaParamValue(s->doc(), shaper, "", "wavetable", "warp", 0.5));
    s->edit(ops::setMetaParamValue(s->doc(), shaper, "", "wavetable", "fold", 0.3));
    s->notes(2 * kPpq);
    if (live) {
      const DeviceId zero = s->modulator(0.0);
      s->wire(zero, "", shaper, "warp");
      s->wire(zero, "", shaper, "fold");
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
  dsp::Curve curve;  // GU.1: its declared curve (an Add wire adds in its input domain: position, or units if linear)
  std::string block;  // GU.11: a module's control: its value block's name inside the device (the wire's target)
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

// GU.11: a module's controls — its value blocks with a continuous curve (not stepped, a range), the first of each name
// (Operator's rows repeat them: row A's, the one heard).
std::vector<Param> moduleParams(const BlockSet& set) {
  std::vector<Param> out;
  for (const DevicePtr& d : set.devices) {
    if (d->patch != kValueDevice) continue;
    const dsp::Curve* c = d->curve(kValueParam);
    if (c == nullptr || c->step > 0.0 || !(c->max > c->min)) continue;
    if (std::any_of(out.begin(), out.end(), [&](const Param& p) { return p.block == d->name; })) continue;
    const DeviceValue* v = d->param(kValueParam);
    out.push_back({std::string(kValueParam), dsp::evalCurve(*c, v != nullptr ? v->value : 0.0), c->min, c->max, *c, d->name});
  }
  return out;
}
BlockSet moduleSet(std::string_view key) {
  const ModuleInfo* m = findModule(key);
  REQUIRE(m != nullptr);
  return moduleBlocks(m->kind);
}

std::vector<Param> metaParams(std::string_view type) {
  const meta::Type* t = meta::findType(type);
  REQUIRE(t != nullptr);
  std::vector<Param> out;
  for (const meta::ParamSpec& p : t->params) {
    out.push_back({std::string(p.name), p.def, p.min, p.max, meta::paramCurve(p)});  // the expansion's declaration
  }
  return out;
}

// GU.11: a module's configuration: its controls (value blocks by name) set in units.
void configure(Song& s, DeviceId d, const Config& c) {
  for (const auto& [k, v] : c.params) {
    const DeviceId block = test_blocks::member(s.doc(), d, k);
    REQUIRE_MESSAGE(block.valid(), k);
    s.edit(ops::setDeviceParamValue(s.doc(), library(), block, std::string(kValueParam), v));  // GU.1: values in units
  }
}

DeviceId moduleEffect(Song& s, std::string_view key, const Config& c) {
  const DeviceId d = s.moduleAfter(key);  // after the default instrument group
  configure(s, d, c);
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
  // GU.11: the stock devices are modules (groups): their controls are value blocks (moduleParams). The default
  // instrument (a new track's group) and Operator (an instrument module: it replaces the group at the notes) as they
  // come; Utility after another Utility that makes the signal stereo (its Balance), so Width and Mid/Side have a side;
  // the LFO's output (its Out) wired into a Utility's Gain after it, so its controls reach the sound. (The stock
  // devices' configurations of sections that the modules do not have went with them: EQ Eight and Auto Filter are
  // retired.)
  {
    IdSource local;
    d.push_back({"Basic Synth", [](Song& s, const Config&) { return s.doc().tracks.front()->devices.front()->id; },
                 moduleParams(defaultInstrumentBlocks(local)), one("default")});
  }
  d.push_back({"Operator", [](Song& s, const Config&) {
                 s.edit(ops::addDevice(s.doc(), s.store.ids(), s.track, "module:operator", DeviceKind::Instrument, 0));
                 return mainPath(s.doc(), *findTrackPtr(s.doc(), s.track)).blocks.at(0)->id;
               },
               moduleParams(moduleSet("operator")), one("default")});
  d.push_back({"Utility", [](Song& s, const Config& c) {
                 configure(s, s.moduleAfter("utility"), Config{"pre", {}, {{"Balance", -0.6}}});
                 return moduleEffect(s, "utility", c);
               },
               moduleParams(moduleSet("utility")),
               {Config{"default", {}, {}}, Config{"mid/side", {}, {{"Mid/Side Mode", 1}}}}});
  d.push_back({"Saturator", [](Song& s, const Config& c) { return moduleEffect(s, "saturator", c); },
               moduleParams(moduleSet("saturator")),
               {Config{"default", {}, {}}, Config{"Medium Curve, soft clip", {}, {{"Type", 3}, {"Soft Clip", 1}}}}});
  d.push_back({"LFO", [](Song& s, const Config& c) {
                 const DeviceId lfo = s.moduleOff("lfo");
                 configure(s, lfo, c);
                 const DeviceId util = s.moduleAfter("utility");
                 s.wire(lfo, "y", test_blocks::member(s.doc(), util, "Gain"), std::string(kValueParam));
                 return lfo;
               },
               moduleParams(moduleSet("lfo")), {Config{"free", {}, {}}, Config{"synced", {}, {{"Sync", 1}}}}});
  // GU.4: the Phase Accumulator module at the notes, the instrument gone (the module itself is then the instrument group:
  // the outermost group fed notes, its voices ending at its Out): each of its
  // value blocks' knobs (one control each: `value`, its curve the module's).
  for (const std::string_view control : {kPaFrequency, kPaPhase, kPaAmplitude, kPaPitch}) {
    const std::string name(control);
    const dsp::Curve curve = phaseAccumulatorCurve(control);
    const double def = control == kPaAmplitude ? 1.0 : 0.0;
    d.push_back({"Phase Accumulator " + name, [control](Song& s, const Config&) {
                   const DeviceLibrary& lib = library();
                   std::vector<DeviceId> old;
                   for (const DevicePtr& x : findTrackPtr(s.doc(), s.track)->devices) old.push_back(x->id);
                   s.edit(ops::dissolveDevices(s.doc(), s.store.ids(), &lib, old));
                   IdSource local;
                   PhaseAccumulatorRoles r;
                   std::vector<DeviceId> ids;
                   const DeviceId pa = s.module(phaseAccumulatorBlocks(local, {}, &r), &ids);
                   s.edit(ops::insertOnPath(s.doc(), s.store.ids(), pa, 0));
                   const DeviceId block = control == kPaFrequency ? r.freq : control == kPaPhase ? r.phase
                                          : control == kPaAmplitude ? r.amp : r.pitch;
                   return ids.at(block.value - 1);
                 },
                 {Param{std::string(kValueParam), def, curve.min, curve.max, curve}}, one("the instrument")});
  }
  auto wavetable = [](double mode) {
    return [mode](Song& s, const Config&) {
      // (GU.4: in Phase mode first on the path, reading the note's phase — what a Phase accumulator before it gave.)
      if (mode == 0.0) s.bare();
      const DeviceId wt = mode == 0.0 ? s.meta("wavetable", 0) : s.metaAfter("wavetable");
      s.edit(ops::setMetaSetting(s.doc(), wt, "", "wavetable", "mode", mode));
      s.edit(ops::setMetaData(s.doc(), wt, "", "wavetable", sawToSquare()));
      return wt;
    };
  };
  d.push_back({"Wavetable (Phase mode)", wavetable(0.0), metaParams("wavetable"), one("at the notes (their phase)")});
  d.push_back({"Wavetable (Shape mode)", wavetable(1.0), metaParams("wavetable"), one("after Basic Synth")});
  d.push_back({"Envelope", [](Song& s, const Config&) {
                 s.bare();
                 s.meta("wavetable", 0);
                 return s.meta("envelope");
               },
               metaParams("envelope"), one("after a Wavetable")});
  // GU.4: the VCA module after a Wavetable: its knob (the value block, on the VCA's law, or a line) at the configured
  // positions.
  auto vca = [](std::optional<dsp::Curve> curve, double knob) {
    return [curve, knob](Song& s, const Config&) {
      s.bare();
      s.meta("wavetable", 0);
      IdSource local;
      VcaOptions vo;
      vo.curve = curve;
      vo.value = knob;
      VcaRoles r;
      std::vector<DeviceId> ids;
      const DeviceId g = s.module(vcaBlocks(local, vo, &r), &ids);
      s.edit(ops::insertOnPath(s.doc(), s.store.ids(), g, 1));
      return ids.at(r.value.value - 1);
    };
  };
  const dsp::Curve law = vcaCurve();
  for (const auto& [label, curve, knob] :
       {std::tuple{"VCA (its law)", std::optional<dsp::Curve>{}, 0.7},
        std::tuple{"VCA (a line 0 … 2)", std::optional<dsp::Curve>{dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 2.0)}, 0.7},
        std::tuple{"VCA (its law, knob between -100 and -10)", std::optional<dsp::Curve>{}, 0.19},
        std::tuple{"VCA (its law, knob near 0)", std::optional<dsp::Curve>{}, 0.53},  // (at 0 the VCA is silent)
        std::tuple{"VCA (its law, knob between +10 and +100)", std::optional<dsp::Curve>{}, 0.81}}) {
    const dsp::Curve c = curve.value_or(law);
    d.push_back({label, vca(curve, knob), {Param{std::string(kValueParam), dsp::evalCurve(c, knob), c.min, c.max, c}},
                 one("after a Wavetable")});
  }
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
        // GU.1: 35 % of the knob's input domain towards its interior — position on a curved control, the curve's units
        // on a linear one (dB on a dB gain: an Add wire adds there).
        const bool curved = !addsInUnits(p.curve);
        const double span = curved ? 1.0 : p.curve.max - p.curve.min;
        const double frac = curved ? dsp::curvePosition(p.curve, p.def, 0.0) : (dsp::curveInput(p.curve, p.def) - p.curve.min) / span;
        const double m = std::clamp((frac < 0.5 ? 0.35 : -0.35) * span, -20000.0, 20000.0);
        Song s;
        const DeviceId d = dut.build(s, c);
        s.notes(c.notes);
        const DeviceId mod = s.modulator(m, phase);
        s.wire(mod, "", p.block.empty() ? d : test_blocks::member(s.doc(), d, p.block), p.name);
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

TEST_CASE("mod-all sweep: every continuous control of every module and engine responds to an audio-rate wire" *
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
  CHECK(tested >= 40);  // (GU.11: the modules' controls; the stock devices' 250-odd params went with them)
}

