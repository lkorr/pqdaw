// GU.10 (docs/plans/GU.md "One hierarchy"): the stock devices are group presets (pq/document/modules.h), the default
// instrument too. Light render checks, each interpreter == JIT:
//   default    a new MIDI track (createTrack: the "Basic Synth" instrument group, PA → Wavetable → Envelope) plays a
//              chord: not silent, below 0 dBFS, and the voices end after the release
//   Utility    after the default instrument, at gain −6 dB, width 50 %, balance 0.3: the same as the retired patch
//              (its stand-in copy) at the same settings — the preset is the old matrix term for term
//   Saturator  drive +12 dB, Analog Clip and Medium Curve: the same as the retired patch (colour off, HQ off)
//   LFO        its output into an audio track's mixer is the 1 Hz sine it should be
//   Operator   A alone is the default instrument's chain at level 1 (4 × the default's 0.25); B modulating A changes
//              the sound and stays finite
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kBlocks = 1200;  // 1.6 s: the chord (1 s at 120 BPM: two beats) and its release

const DeviceLibrary& library() { return playback_fixture::unitsLibrary(); }

golden::Channels render(const Snapshot& s, bool jit, uint32_t blocks = kBlocks) {
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
  golden::Channels out(2);
  std::vector<float> L(kBlock), R(kBlock);
  int64_t t = 0;
  for (uint32_t b = 0; b < blocks; ++b) {
    float* ch[2] = {L.data(), R.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
    }
    t += kBlock;
    out[0].insert(out[0].end(), L.begin(), L.end());
    out[1].insert(out[1].end(), R.begin(), R.end());
  }
  CHECK(link.lastError().empty());
  return out;
}

double maxDiff(const golden::Channels& a, const golden::Channels& b, double scaleB = 1.0) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = 0; i < a[c].size(); ++i) m = std::max(m, std::fabs(static_cast<double>(a[c][i]) - scaleB * b[c][i]));
  }
  return m;
}
double peakOf(const golden::Channels& a, size_t from = 0, size_t to = SIZE_MAX) {
  double p = 0.0;
  for (const auto& c : a)
    for (size_t i = from; i < std::min(to, c.size()); ++i) p = std::max(p, static_cast<double>(std::fabs(c[i])));
  return p;
}

// Renders `s` with the interpreter and the JIT; they agree (float rounding of the same program).
golden::Channels both(const Snapshot& s) {
  const golden::Channels interp = render(s, false), jit = render(s, true);
  const double peak = peakOf(interp);
  CHECK(maxDiff(interp, jit) <= 1e-5 * std::max(peak, 1.0));
  return interp;
}

struct Song {
  DocumentStore store;
  TrackId keys;
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  // A MIDI track (its default instrument) playing a three-note chord for two beats.
  Song() {
    store.setDeviceLibrary(&library());
    keys = edit(ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Keys")).tracks.at(0);
    std::vector<Note> notes;
    for (const int32_t d : {0, 4, 7}) notes.push_back(playback_fixture::note(store.ids(), 0, 2 * kPpq, d));
    Batch b{"clip", {}};
    b.commands.push_back(AddClip{keys, playback_fixture::clip(store.ids(), 0, 4 * kPpq, notes, "chord")});
    REQUIRE(store.dispatch(b).ok());
  }
  // Pastes `set` (a preset) onto Keys and puts its group on the path after the instrument; returns the group.
  DeviceId insert(const BlockSet& set, uint32_t slot = 1) {
    const DeviceId g = edit(ops::pasteBlocks(*store.current(), store.ids(), keys, set)).devices.at(0);
    edit(ops::insertOnPath(*store.current(), store.ids(), g, slot));
    return g;
  }
  // The block named `name` inside group `g` (any depth).
  DeviceId named(DeviceId g, std::string_view name) const {
    const Track& t = *findTrackPtr(*store.current(), keys);
    for (const Device* d : groupMembersDeep(t.devices, g))
      if (d->name == name) return d->id;
    FAIL("no block " << std::string(name));
    return {};
  }
  // Sets value block `v` to `value` in its curve's units (dB for a gain).
  void set(DeviceId v, double value) {
    const Device& d = *findDevice(*store.current(), v).device;
    REQUIRE(d.curve(kValueParam) != nullptr);
    const dsp::Curve& c = *d.curve(kValueParam);
    REQUIRE(store.dispatch(SetDeviceParam{v, std::string(kValueParam), dsp::curvePosition(c, dsp::curveOutput(c, value), 0.0)}).ok());
  }
};

}  // namespace

TEST_CASE("GU.10 default instrument: a new MIDI track plays its group preset (not silent, below 0 dBFS, voices end)") {
  Song s;
  const golden::Channels out = both(s.store.current());
  const size_t beat = static_cast<size_t>(kRate / 2.0);  // 120 BPM
  const double during = peakOf(out, 0, 2 * beat);
  CHECK(during > 0.05);
  CHECK(during < 1.0);
  // The release (0.25 s) is over well before the end: the last 0.2 s are silent.
  CHECK(peakOf(out, out[0].size() - static_cast<size_t>(0.2 * kRate)) < 1e-4);
}

TEST_CASE("GU.10 Utility: the preset is the retired patch's matrix (gain, width, balance), interpreter == JIT") {
  Song a, b;
  const DeviceId u = a.insert(utilityBlocks(a.store.ids()));
  a.set(a.named(u, "Gain"), -6.0);
  a.set(a.named(u, "Width"), 0.5);
  a.set(a.named(u, "Balance"), 0.3);
  const DeviceId old = b.edit(ops::addDevice(*b.store.current(), b.store.ids(), b.keys, "utility", DeviceKind::AudioEffect)).devices.at(0);
  b.edit(ops::setDeviceParamValue(*b.store.current(), library(), old, "gain", std::pow(10.0, -6.0 / 20.0)));
  b.edit(ops::setDeviceParamValue(*b.store.current(), library(), old, "width", 0.5));
  b.edit(ops::setDeviceParamValue(*b.store.current(), library(), old, "balance", 0.3));
  const golden::Channels x = both(a.store.current()), y = render(b.store.current(), false);
  const double peak = peakOf(y);
  CHECK(peak > 0.02);
  CHECK(maxDiff(x, y) <= 1e-5 * peak);
  // Balance 0.3: the left side is quieter than the right.
  CHECK(peakOf(golden::Channels{x[0]}) < peakOf(golden::Channels{x[1]}));
}

TEST_CASE("GU.10 Saturator: drive into Analog Clip and Medium Curve as the retired patch, interpreter == JIT") {
  for (const double type : {0.0, 3.0}) {
    CAPTURE(type);
    Song a, b;
    const DeviceId sat = a.insert(saturatorBlocks(a.store.ids()));
    a.set(a.named(sat, "Drive"), 12.0);
    a.set(a.named(sat, "Type"), type);
    const DeviceId old = b.edit(ops::addDevice(*b.store.current(), b.store.ids(), b.keys, "saturator", DeviceKind::AudioEffect)).devices.at(0);
    b.edit(ops::setDeviceParamValue(*b.store.current(), library(), old, "drive", std::pow(10.0, 12.0 / 20.0)));
    b.edit(ops::setDeviceSetting(*b.store.current(), old, "type", type));
    const golden::Channels x = both(a.store.current()), y = render(b.store.current(), false);
    const double peak = peakOf(y);
    CHECK(peak > 0.1);
    CHECK(peak <= 1.0 + 1e-6);  // both curves saturate at 1
    CHECK(maxDiff(x, y) <= 1e-5);
  }
}

TEST_CASE("GU.10 LFO: its output is a 1 Hz sine (depth 100 %, offset 0), interpreter == JIT") {
  DocumentStore store;
  auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Audio, {}, "Mod");
  REQUIRE(t.ok());
  REQUIRE(store.dispatch(t->command).ok());
  const TrackId mod = t->tracks.at(0);
  auto e = ops::pasteBlocks(*store.current(), store.ids(), mod, lfoBlocks(store.ids()));
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  const DeviceId lfo = e->devices.at(0);
  Wire w;
  w.from = lfo;
  w.port = std::string(kMainPort);
  w.mixer = mod;
  auto c = ops::connect(*store.current(), store.ids(), w);
  REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
  REQUIRE(store.dispatch(c->command).ok());
  const golden::Channels out = both(store.current());
  // One cycle per second: rising through 0 near the start, falling through 0 near 0.5 s, peaks ±1 at 0.25 / 0.75 s.
  const auto at = [&](double sec) { return out[0][static_cast<size_t>(sec * kRate)]; };
  CHECK(at(0.25) > 0.99);
  CHECK(at(0.75) < -0.99);
  size_t fall = 0;
  for (size_t i = static_cast<size_t>(0.3 * kRate); i < static_cast<size_t>(0.7 * kRate) && fall == 0; ++i)
    if (out[0][i - 1] > 0.0f && out[0][i] <= 0.0f) fall = i;
  CHECK(std::fabs(static_cast<double>(fall) / kRate - 0.5) < 0.003);  // (within a few blocks: the mixer's ramp in)
  CHECK(maxDiff(golden::Channels{out[0]}, golden::Channels{out[1]}) == 0.0);  // mono into both sides
}

TEST_CASE("GU.10 Operator: A alone is the default chain at level 1; B modulating A changes it; interpreter == JIT") {
  Song plain, op;
  // Operator in place of the default instrument (its group and blocks dissolved off the path first).
  {
    const Track& t = *findTrackPtr(*op.store.current(), op.keys);
    DeviceId g;
    std::vector<DeviceId> inside;
    for (const DevicePtr& d : t.devices)
      if (isGroupDevice(*d) && !d->group.valid()) g = d->id;
    for (const Device* m : groupMembersDeep(t.devices, g)) inside.push_back(m->id);
    op.edit(ops::deleteDevices(*op.store.current(), inside));
    const DeviceId gs[1] = {g};
    op.edit(ops::dissolveDevices(*op.store.current(), op.store.ids(), &library(), gs));
  }
  const DeviceId opGroup = op.insert(operatorBlocks(op.store.ids()), 0);
  const golden::Channels a = both(op.store.current()), base = render(plain.store.current(), false);
  CHECK(peakOf(base) > 0.02);
  // A alone is 4 × the default chain (its `+ pm` adds B's 0), compared in loudness window by window (release included).
  const size_t w = 1024;
  double worst = 0.0;
  for (size_t at = 0; at + w <= a[0].size(); at += w) {
    double ea = 0.0, eb = 0.0;
    for (size_t i = at; i < at + w; ++i) {
      ea += static_cast<double>(a[0][i]) * a[0][i];
      eb += 16.0 * static_cast<double>(base[0][i]) * base[0][i];
    }
    const double ra = std::sqrt(ea / w), rb = std::sqrt(eb / w);
    if (rb > 0.01 * peakOf(a)) worst = std::max(worst, std::fabs(ra - rb) / rb);
  }
  CHECK(worst < 0.03);
  // B (coarse 2) at level 0.5 into A's phase: FM — a different sound, still finite and bounded by A's level.
  const DeviceId bEnv = [&] {
    const Track& t = *findTrackPtr(*op.store.current(), op.keys);
    for (const Device* d : groupMembersDeep(t.devices, op.named(opGroup, "Envelope B")))
      if (d->patch == "meta:envelope") return d->id;
    return DeviceId{};
  }();
  REQUIRE(bEnv.valid());
  op.edit(ops::setDeviceParamValue(*op.store.current(), library(), bEnv, "amount", 0.5));
  op.set(op.named(opGroup, "Coarse B"), 2.0);
  const golden::Channels fm = both(op.store.current());
  CHECK(maxDiff(fm, a) > 0.01);
  CHECK(peakOf(fm) <= peakOf(a) * 1.001 + 1e-6);
}
