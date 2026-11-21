// SM.3 (docs/plans/SM.md "SM.3"; pq/document/modules.h "the Sampler"): the Sampler group preset through the real Engine
// path (ProjectLink: the lowering, the link's pinned sample, the timeline, the voices) —
//   classic    drawn notes into the group play the sample at hz / root; with the Envelope at sustain 1 the group's
//              output is exactly 0.5 (its amount) × the engine's own (SM.2's reference-checked `meta:sampler`, wired
//              straight to Out) on the same notes, stereo kept; after the note-off the Envelope's release sounds (the
//              engine alone ends at the note-off)
//   one-shot   a short note plays the sample to its end, then the voice ends
//   slice      drawn notes at keys 36 / 37 (12-EDO degrees −24 / −23: voice.key) play slices 0 / 1
//   backends   every case renders bit for bit the same on the JIT as on the interpreter
//   rewire     inside the group: the Filter switched on at 200 Hz takes the 880 Hz note down; the engine wired straight
//              to Out plays the engine-alone output
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <array>
#include <numbers>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/modules.h"
#include "pq/document/store.h"
#include "pq/engine/wav_file.h"
#include "pq/meta/sampler.h"

using namespace pq;
using namespace pq::doc;

namespace {

namespace fs = std::filesystem;

constexpr double kRate = 48000.0;
constexpr Ticks kBeat = kPpq;  // 0.5 s at 120 BPM

std::string utf8(const fs::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

void run(DocumentStore& s, const ops::EditResult& e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(s.dispatch(e->command).ok());
}

// One MIDI track playing `notes` (12-EDO degrees, start / length in beats) through the Sampler group (or, `bare`, with
// the engine wired straight to the group's Out), its sample a stereo WAV of `frames` frames whose channels `fill` gives.
struct Song {
  DocumentStore store;
  fs::path dir;
  TrackId track;
  DeviceId group, engine, envelope, filter, filterFrequency, amp;
  SampleId sample;
  Song(const char* name, bool bare, const std::vector<std::vector<float>>& channels, double rootHz,
       std::vector<float> slices = {}) : dir(fs::temp_directory_path() / name) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Midi);
    run(store, t);
    track = t->tracks.at(0);
    {
      std::vector<DeviceId> heads;
      for (const Device* d : mainPath(*store.current(), *findTrackPtr(*store.current(), track)).blocks) heads.push_back(d->id);
      if (!heads.empty()) run(store, ops::dissolveDevices(*store.current(), store.ids(), nullptr, heads));
    }
    const fs::path p = dir / "s.wav";
    REQUIRE(engine::writeWav(p, channels, static_cast<uint32_t>(kRate)).ok());
    SampleRef s;
    s.id = store.ids().sample();
    s.path = utf8(p);
    s.size = fs::file_size(p);
    s.frames = static_cast<int64_t>(channels[0].size());
    s.rate = kRate;
    s.channels = static_cast<uint32_t>(channels.size());
    s.displayName = "s.wav";
    REQUIRE(store.dispatch(Command{AddSample{s}}).ok());
    sample = s.id;
    {
      IdSource local;
      const ops::EditResult e = ops::pasteBlocksAtNotes(*store.current(), store.ids(), track, samplerBlocks(local));
      run(store, e);
      group = e->devices.at(0);
      forEachDevice(*store.current(), [&](const Device& d, const Track&) {
        if (d.patch == "meta:sampler" && d.group == group) engine = d.id;
        if (d.patch == "meta:envelope") envelope = d.id;
        if (d.name == "Filter" && d.group == group) filter = d.id;
        if (d.name == "Amp" && d.group == group) amp = d.id;
      });
      forEachDevice(*store.current(), [&](const Device& d, const Track&) {
        if (d.group == filter && bandRole(d) == BandRole::Frequency) filterFrequency = d.id;
      });
      REQUIRE(envelope.valid());
      REQUIRE(filterFrequency.valid());
      // Sustain 1: after the 5 ms attack the envelope is its amount exactly (no decay to hear).
      run(store, ops::setDeviceParam(*store.current(), envelope, "sustain", 1.0));
      if (bare) engineToOut();
    }
    REQUIRE(engine.valid());
    run(store, ops::setSamplerSample(*store.current(), bare ? engine : group, sample, rootHz, std::move(slices)));  // (either id)
  }
  // The engine wired straight to the group's Out (the Amp's wire to Out deleted): the engine alone per voice (its own
  // `done` ends the voice), envelope- and filter-free.
  void engineToOut() {
    std::vector<WireId> toOut;
    for (const Wire& w : wiresOf(*store.current()))
      if (w.from == amp && w.to == group) toOut.push_back(w.id);
    REQUIRE(toOut.size() == 1);
    run(store, ops::deleteWires(*store.current(), toOut));
    run(store, ops::connectWire(*store.current(), store.ids(), engine, "out", group, std::string(kGroupOutPort)));
  }
  ~Song() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void notes(const std::vector<std::array<double, 3>>& ns) {  // {degree, start beat, length beats}
    IdSource& ids = store.ids();
    std::vector<Note> n;
    for (const auto& x : ns)
      n.push_back(playback_fixture::note(ids, static_cast<Ticks>(x[1] * kBeat), static_cast<Ticks>(x[2] * kBeat),
                                         static_cast<int32_t>(x[0]), 0, 100.0f));
    REQUIRE(store.dispatch(AddClip{track, playback_fixture::clip(ids, 0, 16 * kBeat, n, "Notes")}).ok());
  }
  std::vector<std::vector<float>> render(double seconds, bool jit = false) const {
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = jit;
    c.sampleRate = kRate;
    auto l = app::ProjectLink::create(c, store.current());
    REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
    app::ProjectLink& link = **l;
    REQUIRE(link.waitForSamples(30.0));
    if (jit) REQUIRE(link.waitForJit(180.0));
    link.flush();
    engine::BufferSink sink;
    auto st = link.renderOffline(0, static_cast<uint64_t>(std::llround(seconds * kRate)), sink);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    CHECK(link.lastError().empty());
    REQUIRE(sink.channels.size() == 2);
    return sink.channels;
  }
};

std::vector<float> sine(double hz, double amp, size_t frames) {
  std::vector<float> x(frames);
  for (size_t i = 0; i < frames; ++i) x[i] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / kRate));
  return x;
}

size_t at(double seconds) { return static_cast<size_t>(seconds * kRate); }

double rms(const std::vector<float>& x, double from, double to) {
  double s = 0.0;
  for (size_t i = at(from); i < at(to); ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / static_cast<double>(at(to) - at(from)));
}

// Upward zero crossings, linearly interpolated: the frequency over [from, to).
double hzBetween(const std::vector<float>& x, double from, double to) {
  std::vector<double> c;
  for (size_t i = std::max<size_t>(at(from), 1); i < at(to); ++i)
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) c.push_back(static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]));
  REQUIRE(c.size() > 4);
  return (static_cast<double>(c.size()) - 1.0) / ((c.back() - c.front()) / kRate);
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b, double scale, double from, double to) {
  double m = 0.0;
  for (size_t i = at(from); i < at(to); ++i) m = std::max(m, std::fabs(static_cast<double>(a[i]) - scale * b[i]));
  return m;
}

}  // namespace

TEST_CASE("SM.3 Sampler group: drawn notes play the sample through the group = 0.5 x the engine alone; stereo; the "
          "Envelope's release; rewiring inside the group") {
  const size_t frames = at(2.0);
  const std::vector<std::vector<float>> ch = {sine(440.0, 0.5, frames), sine(440.0, 0.25, frames)};
  // Degree 9 = 440 Hz (the sample's pitch: rate 1), then degree 21 = 880 Hz (rate 2).
  const std::vector<std::array<double, 3>> ns = {{9, 0, 1}, {21, 3, 1}};
  Song g("pq_sm3_group", false, ch, 440.0), e("pq_sm3_bare", true, ch, 440.0);
  g.notes(ns);
  e.notes(ns);
  const auto go = g.render(2.5), eo = e.render(2.5);
  {  // the JIT backend plays it bit for bit as the interpreter
    const auto gj = g.render(2.5, true);
    for (int c = 0; c < 2; ++c) CHECK(maxDiff(gj[c], go[c], 1.0, 0.0, 2.5) == 0.0);
  }
  CHECK(rms(eo[0], 0.05, 0.45) > 0.1);
  for (int c = 0; c < 2; ++c) {
    CAPTURE(c);
    // The sustain of both notes: the group is the engine × 0.5, sample for sample.
    CHECK(maxDiff(go[c], eo[c], 0.5, 0.02, 0.49) < 1e-5);
    CHECK(maxDiff(go[c], eo[c], 0.5, 1.52, 1.99) < 1e-5);
  }
  CHECK(rms(go[1], 0.05, 0.45) == doctest::Approx(0.5 * rms(go[0], 0.05, 0.45)).epsilon(1e-3));  // stereo kept
  CHECK(hzBetween(go[0], 0.05, 0.45) == doctest::Approx(440.0).epsilon(1e-3));
  CHECK(hzBetween(go[0], 1.55, 1.95) == doctest::Approx(880.0).epsilon(1e-3));
  // After the note-off (0.5 s): the Envelope's 250 ms release sounds; the engine alone ends at the note-off.
  CHECK(rms(go[0], 0.52, 0.56) > 0.02);
  CHECK(rms(eo[0], 0.52, 0.56) < 1e-6);
  CHECK(rms(go[0], 1.2, 1.45) < 1e-6);  // the voice ended
  // Rewiring inside the opened group: the Filter on (Low Pass at 200 Hz) takes the 880 Hz note down …
  {
    const DeviceId one[] = {g.filter};
    run(g.store, ops::setDevicesActive(*g.store.current(), one, true));
    run(g.store, ops::setDeviceParam(*g.store.current(), g.filterFrequency, std::string(kValueParam), pq::dsp::curvePosition(bandFrequencyCurve(), 200.0, 0.0)));
  }
  {
    const auto f = g.render(2.0);
    CHECK(rms(f[0], 1.6, 1.95) < 0.1 * rms(go[0], 1.6, 1.95));
  }
  // … and the engine wired straight to Out: the reference song's circuit, its output.
  g.engineToOut();
  const auto r = g.render(1.0);
  for (int c = 0; c < 2; ++c) CHECK(maxDiff(r[c], eo[c], 1.0, 0.0, 0.49) < 1e-5);
}

TEST_CASE("SM.3 Sampler group: One-Shot plays a short note's sample to its end, then the voice ends") {
  const size_t frames = at(1.0);
  Song g("pq_sm3_oneshot", false, {sine(440.0, 0.5, frames)}, 440.0);
  run(g.store, ops::setDeviceSetting(*g.store.current(), g.engine, "mode", static_cast<double>(meta::SamplerMode::OneShot)));
  g.notes({{9, 0, 0.2}});  // 0.1 s
  const auto o = g.render(2.0);
  {
    const auto oj = g.render(2.0, true);  // JIT == interpreter
    CHECK(maxDiff(oj[0], o[0], 1.0, 0.0, 2.0) == 0.0);
  }
  CHECK(rms(o[0], 0.5, 0.9) > 0.1);       // long after the note-off: still the sample
  CHECK(hzBetween(o[0], 0.5, 0.9) == doctest::Approx(440.0).epsilon(1e-3));
  CHECK(rms(o[0], 1.0, 1.1) < 0.05);      // the sample ended at 1 s: the release of nothing
  CHECK(rms(o[0], 1.4, 2.0) < 1e-6);      // the voice ended
}

TEST_CASE("SM.3 Sampler group: Slice picks the slice by voice.key (drawn notes at keys 36 / 37)") {
  const size_t half = at(0.5);
  std::vector<float> x = sine(300.0, 0.5, half), hi = sine(600.0, 0.5, half);
  x.insert(x.end(), hi.begin(), hi.end());
  Song g("pq_sm3_slice", false, {x}, 261.63, {0.0f, static_cast<float>(half)});
  run(g.store, ops::setDeviceSetting(*g.store.current(), g.engine, "mode", static_cast<double>(meta::SamplerMode::Slice)));
  g.notes({{-24, 0, 1}, {-23, 2, 1}});  // 12-EDO from key 60: keys 36, 37
  const auto o = g.render(1.6);
  {
    const auto oj = g.render(1.6, true);  // JIT == interpreter
    CHECK(maxDiff(oj[0], o[0], 1.0, 0.0, 1.6) == 0.0);
  }
  CHECK(hzBetween(o[0], 0.05, 0.45) == doctest::Approx(300.0).epsilon(1e-3));
  CHECK(hzBetween(o[0], 1.05, 1.45) == doctest::Approx(600.0).epsilon(1e-3));
}

// SM.8 (docs/plans/SM.md "Decisions", Drop): a Sampler dropped on an AUDIO track lands unwired (no notes reach it: a
// plain group, not an instrument group). Wired into the track's path, an LFO into its engine's Position input scrubs the
// sample and makes sound; with Position unwired it is silent (no gate).
TEST_CASE("SM.8 Sampler on an audio track: an LFO into Position scrubs the sample (sound); unwired: silence") {
  DocumentStore store;
  const fs::path dir = fs::temp_directory_path() / "pq_sm8_audio";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Audio);
  run(store, t);
  const TrackId track = t->tracks.at(0);
  const fs::path p = dir / "s.wav";
  const std::vector<std::vector<float>> ch = {sine(440.0, 0.5, at(1.0))};
  REQUIRE(engine::writeWav(p, ch, static_cast<uint32_t>(kRate)).ok());
  SampleRef s;
  s.id = store.ids().sample();
  s.path = utf8(p);
  s.size = fs::file_size(p);
  s.frames = static_cast<int64_t>(ch[0].size());
  s.rate = kRate;
  s.channels = 1;
  s.displayName = "s.wav";
  REQUIRE(store.dispatch(Command{AddSample{s}}).ok());
  IdSource local;
  SamplerOptions so;
  so.sample = s.id.value;
  const ops::EditResult e = ops::pasteBlocksAtNotes(*store.current(), store.ids(), track, samplerBlocks(local, so));
  run(store, e);
  const DeviceId group = e->devices.at(0);
  CHECK(mainPath(*store.current(), *findTrackPtr(*store.current(), track)).blocks.empty());  // landed unwired
  DeviceId engineId;
  forEachDevice(*store.current(), [&](const Device& d, const Track&) {
    if (d.patch == "meta:sampler" && d.group == group) engineId = d.id;
  });
  REQUIRE(engineId.valid());
  // Its Out into the track's path (the user's wiring), then render: no gate, no Position → silence.
  {
    const MainPath path = mainPath(*store.current(), *findTrackPtr(*store.current(), track));
    REQUIRE(!path.links.empty());
    run(store, ops::spliceIntoWire(*store.current(), store.ids(), nullptr, group, path.links.front()));
  }
  auto render = [&](double seconds) {
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.sampleRate = kRate;
    auto l = app::ProjectLink::create(c, store.current());
    REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
    REQUIRE((*l)->waitForSamples(30.0));
    (*l)->flush();
    engine::BufferSink sink;
    auto st = (*l)->renderOffline(0, static_cast<uint64_t>(std::llround(seconds * kRate)), sink);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    CHECK((*l)->lastError().empty());
    return sink.channels;
  };
  CHECK(rms(render(1.0)[0], 0.0, 1.0) < 1e-6);
  // A stock LFO (free, its default rate) wired into the engine's Position: it scrubs the sample.
  IdSource local2;
  const ops::EditResult lfo = ops::pasteBlocks(*store.current(), store.ids(), track, lfoBlocks(local2));
  run(store, lfo);
  run(store, ops::connectWire(*store.current(), store.ids(), lfo->devices.at(0), std::string(kMainPort), engineId, "position"));
  const auto o = render(2.0);
  CHECK(rms(o[0], 0.0, 2.0) > 0.02);
  fs::remove_all(dir, ec);
}

// SM.12 (docs/plans/SM.md "SM.12"): Slice to Notes writes a clip whose notes, played through the Sampler in Slice mode,
// reproduce the sample's rhythm: clicks at irregular frames, sliced exactly there; each output click lands on the frame
// the sample has it (from the first slice), within one sample — warp Off (seconds through the tempo map, at 97 BPM).
TEST_CASE("SM.12 Slice to Notes: the written clip plays the sample's rhythm (onsets within 1 sample)") {
  const std::vector<size_t> at = {2000, 9000, 15333, 26000, 33501, 42017};
  std::vector<float> x(48000, 0.0f);
  for (const size_t f : at)
    for (size_t k = 0; k < 400 && f + k < x.size(); ++k) x[f + k] = static_cast<float>(0.9 * std::exp(-static_cast<double>(k) / 60.0));
  std::vector<float> slices;
  for (const size_t f : at) slices.push_back(static_cast<float>(f));
  Song g("pq_sm12_notes", true, {x}, 261.63, slices);
  run(g.store, ops::setDeviceSetting(*g.store.current(), g.engine, "mode", static_cast<double>(meta::SamplerMode::Slice)));
  REQUIRE(g.store.dispatch(Command{SetTempoMap{{TempoPoint{0, 97.0}}}}).ok());
  run(g.store, ops::sliceToNotes(*g.store.current(), g.store.ids(), g.group, 0));
  const auto o = g.render(1.1);
  const double t0 = 0.0;  // the clip at tick 0: the first slice's click at frame 0 of the output
  for (size_t i = 0; i < at.size(); ++i) {
    CAPTURE(i);
    const auto expect = static_cast<int64_t>(t0 + static_cast<double>(at[i] - at[0]));
    int64_t found = -1;
    for (int64_t s = std::max<int64_t>(0, expect - 200); s < expect + 200 && s < static_cast<int64_t>(o[0].size()); ++s)
      if (std::fabs(o[0][static_cast<size_t>(s)]) > 0.45f) {
        found = s;
        break;
      }
    CHECK(std::llabs(found - expect) <= 1);
  }
}
