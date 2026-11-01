// TMM.3: meta modules through the playback link — a nested Phase accumulator's freq drag is param writes only (no
// compile job, no unit compile, the unit keeps its image: the unit hash is unchanged) and audible; a chain-level
// `meta:phase_acc` instrument plays its voices; a MetaData edit compiles nothing.
// TMM.10: Phase accumulator -> Wavetable -> Auto Filter as a chain (a meta voice run) plays exact pitches in 31edo;
// its voices end with the run's release.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <complex>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../meta/meta_test_type.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/core/voice_tap.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/dsp/frame_table_slot.h"
#include "pq/meta/envelope.h"
#include "pq/meta/registry.h"
#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/engine/project_program.h"
#include "pq/tuning/edo.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr uint32_t kBlock = 64;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

struct Driver {
  ProjectLink& link;
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

std::string replaced(std::string s, const std::string& from, const std::string& to) {
  const size_t at = s.find(from);
  REQUIRE_MESSAGE(at != std::string::npos, from);
  s.replace(at, from.size(), to);
  return s;
}

// A held chord on a MIDI track with BasicSynth.
struct Keys {
  DocumentStore store;
  TrackId track;
  DeviceId inst;
  Keys() {
    IdSource& ids = store.ids();
    track = ids.track();
    inst = ids.device();
    Batch b{"keys", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Keys", inst), {}, kAppend});
    std::vector<Note> notes;
    for (int32_t d : {0, 4, 7}) notes.push_back(playback_fixture::note(ids, 0, 64 * kPpq, d));
    b.commands.push_back(AddClip{track, playback_fixture::clip(ids, 0, 64 * kPpq, notes, "chord")});
    REQUIRE(store.dispatch(b).ok());
  }
  void edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
  }
};

uint32_t voiceUnit(engine::ProjectProgram& program) {
  for (uint32_t u = 0; u < program.numUnits(); ++u)
    if (program.layout().units[u].voices) return u;
  FAIL("no instrument unit");
  return 0;
}

double rms(const std::vector<float>& v, size_t from, size_t to) {
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += static_cast<double>(v[i]) * v[i];
  return std::sqrt(s / static_cast<double>(to - from));
}

}  // namespace

TEST_CASE("meta link: a nested Phase accumulator's freq drag compiles nothing and is audible") {
  Keys k;
  auto link = makeLink(k.store.current());
  [[maybe_unused]] const ListenerId sub = k.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(50);
  // Fork the synth with a Phase accumulator as its oscillator phase (one structural compile of its unit).
  const std::string text = replaced(link->devices().stockText("BasicSynth"), "ph  = phasor(voice.hz)",
                                    "acc1 = meta_phase_acc(voice.hz)\n  ph  = acc1");  // TMM.10-fix1: the note wired
  app::LinkStats s0 = link->stats();
  k.edit(ops::addMetaCall(*k.store.current(), k.inst, text, "acc1", "phase_acc"));
  link->flush();
  CHECK(link->stats().compiles == s0.compiles + 1);
  CHECK(link->stats().compileErrors == s0.compileErrors);
  CHECK(link->lastError().empty());
  d.run(100);
  engine::ProjectProgram& program = *link->engine().program();
  const uint32_t u = voiceUnit(program);
  CHECK(program.voices(u)->liveVoices() == 3);
  const engine::ProgramImage* image = program.current(u);
  REQUIRE(image != nullptr);
  const std::string key = metaParamKey(k.inst, "acc1", "freq");
  REQUIRE(link->paramSlot(key) != ProjectLink::kNoSlot);
  const size_t mark = d.out.size();
  d.run(100);
  const double before = rms(d.out, mark, d.out.size());
  // A drag: 16 values in one gesture. Param writes only.
  s0 = link->stats();
  REQUIRE(k.store.beginGesture().ok());
  for (int i = 1; i <= 16; ++i) {
    k.edit(ops::setMetaParam(*k.store.current(), k.inst, "acc1", "phase_acc", "freq", 250.0 * i));
    link->flush();
  }
  REQUIRE(k.store.commitGesture().ok());
  link->flush();
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().unitCompiles == s0.unitCompiles);
  CHECK(link->stats().topologyChanges == s0.topologyChanges);
  CHECK(link->stats().paramWrites >= s0.paramWrites + 16);
  d.run(50);
  CHECK(program.current(u) == image);  // the same image: the unit (and its hash) did not change
  CHECK(program.voices(u)->liveVoices() == 3);
  // Audible: 4 kHz added to every voice's frequency changes the sound.
  const size_t mark2 = d.out.size();
  d.run(100);
  const double after = rms(d.out, mark2, d.out.size());
  CHECK(before > 1e-4);
  CHECK(after > 1e-4);
  bool differs = false;
  for (size_t i = 0; i < 6400 && !differs; ++i) differs = d.out[mark + i] != d.out[mark2 + i];
  CHECK(differs);
  // A setting-free nested param back to its default: still a write.
  s0 = link->stats();
  k.edit(ops::setMetaParam(*k.store.current(), k.inst, "acc1", "phase_acc", "freq", std::nullopt));
  link->flush();
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().paramWrites == s0.paramWrites + 1);
}

TEST_CASE("meta link: a chain meta:phase_acc instrument plays its voices; MetaData edits compile nothing") {
  meta::testing::registerTestType();
  Keys k;
  auto link = makeLink(k.store.current());
  [[maybe_unused]] const ListenerId sub = k.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(20);
  // The Phase accumulator replaces the instrument (the chain rule: one instrument, first).
  k.edit(ops::addMetaDevice(*k.store.current(), k.store.ids(), k.track, "phase_acc", 0));
  link->flush();
  CHECK(link->lastError().empty());
  const DeviceId acc = findTrackPtr(*k.store.current(), k.track)->devices.at(0)->id;
  REQUIRE(isMetaDevice(*findDevice(*k.store.current(), acc).device));
  d.run(100);
  engine::ProjectProgram& program = *link->engine().program();
  const uint32_t u = voiceUnit(program);
  CHECK(program.voices(u)->liveVoices() == 3);
  const size_t mark = d.out.size();
  d.run(50);
  CHECK(rms(d.out, mark, d.out.size()) > 1e-3);  // three ramps (DC and all) through the mixer
  // Its own param is a device param: a write, no compile.
  app::LinkStats s0 = link->stats();
  k.edit(ops::setMetaParam(*k.store.current(), acc, "", "phase_acc", "amp", 0.5));
  link->flush();
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().paramWrites == s0.paramWrites + 1);
  // A data edit (a nested test module's samples) compiles nothing either.
  k.edit(ops::setMetaSetting(*k.store.current(), acc, "x", "test_shape", "stages", 1.0));  // state only: no call
  link->flush();
  s0 = link->stats();
  k.edit(ops::setMetaData(*k.store.current(), acc, "x", "test_shape",
                          std::make_shared<const meta::DataValue>(meta::DataValue::record({{"values", meta::DataValue::floatArray({0.25f})}}))));
  CHECK(k.store.lastChanges().metaDataOnly());
  link->flush();
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().paramWrites == s0.paramWrites);
}

TEST_CASE("meta link (TMM.10-fix1): an unplugged Phase accumulator plays its bar's phase once, whatever notes play") {
  Keys k;  // a held three-note chord
  k.edit(ops::addMetaDevice(*k.store.current(), k.store.ids(), k.track, "phase_acc", 0));
  const DeviceId acc = findTrackPtr(*k.store.current(), k.track)->devices.at(0)->id;
  k.edit(ops::setMetaParam(*k.store.current(), acc, "", "phase_acc", "freq", 100.0));
  k.edit(ops::setDeviceChainInput(*k.store.current(), acc, std::string(kChainUnplugged)));
  auto link = makeLink(k.store.current());
  CHECK(link->lastError().empty());
  Driver d{*link};
  link->play(0);
  d.run(10);
  const auto tap = link->metaVoiceTap(acc, "", 0);
  REQUIRE(tap != nullptr);
  VoiceTapReader reader(tap);
  const size_t mark = d.out.size();
  for (int i = 0; i < 10; ++i) {
    d.run(75);  // 750 blocks of 64 = 1 s
    reader.poll(1.0);
  }
  // No voices anywhere: the notes are not wired into it.
  engine::ProjectProgram& program = *link->engine().program();
  for (uint32_t u = 0; u < program.numUnits(); ++u) CHECK_FALSE(program.layout().units[u].voices);
  // One ramp at the bar's 100 Hz (each wrap a drop), through the mixer's constant gain.
  uint32_t wraps = 0;
  float peak = 0.0f;
  for (size_t i = mark + 1; i < d.out.size(); ++i) {
    wraps += d.out[i] < d.out[i - 1] - 0.3f ? 1u : 0u;
    peak = std::max(peak, d.out[i]);
  }
  CHECK(wraps >= 99);
  CHECK(wraps <= 101);
  CHECK(peak > 0.6f);   // a single ramp 0 … 1 at −3 dB (three voices would sum past 1)
  CHECK(peak < 0.75f);
  // The face shows one dot (a global module taps one voice).
  uint32_t active = 0;
  for (uint32_t s = 0; s < reader.voiceSlots(); ++s) active += reader.voice(s).active ? 1u : 0u;
  CHECK(active == 1);
}

// ---- TMM.2: the link's meta-module resource slots (frame tables, voice taps) -----------------------------------------

namespace {

// A test-only meta type that reads both resource kinds: out = wavetable_pos(phasor(hz), 0, slot 0), and a voice_tap of
// the phase into slot 1.
// TMM.10-fix1: no default reads the note; the call wires voice.hz in.
constexpr std::array<meta::InputSpec, 1> kWtInputs = {
    {{"hz", "Hz", meta::InputDefault::Zero, graph::Kind::Real, meta::Reads::Frequency}}};
constexpr std::array<meta::OutputSpec, 1> kWtOutputs = {{{"out", graph::kWidthMono}}};

const meta::Type& registerWtType() {
  static const meta::Type t = [] {
    meta::Type x;
    x.id = "tmm2_wt";
    x.name = "TMM.2 test wavetable";
    x.inputs = kWtInputs;
    x.outputs = kWtOutputs;
    x.resources = [](std::span<const double>) {
      return std::vector<meta::ResourceRequest>{{meta::ResourceKind::FrameTable, 0}, {meta::ResourceKind::VoiceTap, 1}};
    };
    x.body = [](meta::Builder& b) -> Status {
      const graph::PortRef ph = b.atom("phasor", {b.input(0)}, 0);
      const graph::PortRef y = b.atom("wavetable_pos", {ph, b.konst(0.0, 1), b.slot(0)}, 2);
      b.atom("voice_tap", {ph, b.slot(1)}, 3);
      b.output(0, y);
      return Ok{};
    };
    return x;
  }();
  if (meta::findType(t.id) == nullptr) (void)meta::registerType(t);
  return t;
}

std::shared_ptr<const dsp::FrameTable> sawTable(double sr) {
  const AlignedBuffer<float> saw = dsp::basicCycle(dsp::BasicCycle::Saw);
  const dsp::Keyframe key{0.0f, saw.span()};
  dsp::FrameTableSpec spec;
  spec.keyframes = std::span<const dsp::Keyframe>(&key, 1);
  spec.denseFrames = 1;
  spec.sampleRate = sr;
  dsp::FrameTableBuilder b;
  auto t = b.build(spec);
  REQUIRE(t.ok());
  return std::make_shared<const dsp::FrameTable>(std::move(t.value()));
}

}  // namespace

TEST_CASE("meta link: frame-table and voice-tap slots — stable ids, taps per voice, a table swap without a compile") {
  registerWtType();
  Keys k;
  auto link = makeLink(k.store.current());
  [[maybe_unused]] const ListenerId sub = k.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(20);
  const std::string base = link->devices().stockText("BasicSynth");
  const std::string text =
      replaced(base, "  out = osc * env * vel * level", "  wt1 = meta_tmm2_wt(voice.hz)\n  out = wt1 * env * vel * level");
  k.edit(ops::addMetaCall(*k.store.current(), k.inst, text, "wt1", "tmm2_wt"));
  link->flush();
  CHECK(link->lastError().empty());
  const uint32_t tableId = link->metaSlot(k.inst, "wt1", 0), tapId = link->metaSlot(k.inst, "wt1", 1);
  CHECK(tableId != 0);
  CHECK(tapId != 0);
  CHECK(tableId != tapId);
  CHECK(link->metaSlotsInUse() == 2);
  const auto slot = link->metaFrameTable(k.inst, "wt1", 0);
  REQUIRE(slot != nullptr);
  CHECK(link->metaFrameTable(k.inst, "wt1", 1) == nullptr);  // a tap, not a table
  const auto tap = link->metaVoiceTap(k.inst, "wt1", 1);
  REQUIRE(tap != nullptr);
  CHECK(link->metaVoiceTap(k.inst, "wt1", 0) == slot->sharedTap());
  CHECK(tap->stride() == 8);  // 64-frame blocks: one record per block
  // Readers: three voices (the held chord) in both taps, a record per block.
  VoiceTapReader phases(tap), reads(slot->sharedTap());
  for (int i = 0; i < 10; ++i) {  // a reader polls more often than the ring's 64 blocks
    d.run(10);
    phases.poll(1.0);
    reads.poll(1.0);
  }
  CHECK(phases.received() == 100);
  CHECK(reads.received() == 100);
  CHECK(phases.lost() == 0);
  uint32_t active = 0;
  for (uint32_t s = 0; s < phases.voiceSlots(); ++s) {
    active += phases.voice(s).active ? 1u : 0u;
    if (phases.voice(s).active) CHECK(reads.voice(s).active);
    if (phases.voice(s).active) CHECK(phases.voice(s).value == reads.voice(s).value);  // the phase read, sampled
  }
  CHECK(active == 3);
  // A table swap: no compile, the unit keeps its image, the sound changes (sine → saw).
  engine::ProjectProgram& program = *link->engine().program();
  const uint32_t u = voiceUnit(program);
  const engine::ProgramImage* image = program.current(u);
  const size_t mark = d.out.size();
  d.run(50);
  app::LinkStats s0 = link->stats();
  CHECK_FALSE(link->publishFrameTable(k.inst, "wt1", 0, sawTable(96000.0)).ok());  // another rate: refused
  REQUIRE(link->publishFrameTable(k.inst, "wt1", 0, sawTable(48000.0)).ok());
  CHECK(slot->generation() == 2);
  d.run(50);
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(program.current(u) == image);
  // Brightness: Σ(Δx)² / Σx² is (2πf/sr)² for sines; the saw's harmonics raise it many times.
  auto brightness = [&](size_t from, size_t to) {
    double e = 0, de = 0;
    for (size_t i = from + 1; i < to; ++i) {
      e += double(d.out[i]) * d.out[i];
      de += (double(d.out[i]) - d.out[i - 1]) * (double(d.out[i]) - d.out[i - 1]);
    }
    return de / std::max(e, 1e-30);
  };
  const double sine = brightness(mark, mark + 64 * 50), saw = brightness(mark + 64 * 55, mark + 64 * 100);
  MESSAGE("brightness: sine table " << sine << ", saw table " << saw);
  CHECK(saw > 5.0 * sine);
  CHECK(link->metaSlot(k.inst, "wt1", 0) == tableId);  // stable
  // Removing the call frees both ids; the slot objects live on in the images that held them.
  k.edit(ops::editDevicePatch(*k.store.current(), k.inst, base));
  link->flush();
  CHECK(link->lastError().empty());
  CHECK(link->metaSlotsInUse() == 0);
  CHECK(link->metaSlot(k.inst, "wt1", 0) == 0);
  d.run(20);
  CHECK(slot->generation() == 2);  // our reference still works
}

// ---- TMM.10: meta devices in a chain — voice continuity ---------------------------------------------------------------

namespace {

// The frequency a component near `f` really has, from the phase advance of the signal demodulated at `f` between two
// Blackman-Harris windows `gap` samples apart (|error| far below 1e-6 Hz for a clean tone whose neighbours are at
// least 4 bins away).
double measuredHz(const std::vector<float>& x, size_t from, size_t window, size_t gap, double f, double sr) {
  constexpr long double kTwoPi = 6.283185307179586476925L;
  auto demod = [&](size_t at) {
    double re = 0.0, im = 0.0;
    for (size_t n = 0; n < window; ++n) {
      const double u = 6.283185307179586 * static_cast<double>(n) / static_cast<double>(window - 1);
      const double w = 0.35875 - 0.48829 * std::cos(u) + 0.14128 * std::cos(2 * u) - 0.01168 * std::cos(3 * u);
      // The absolute sample index in the phase, reduced exactly enough over seconds.
      const long double cyc = static_cast<long double>(f) * static_cast<long double>(at + n) / static_cast<long double>(sr);
      const double ph = static_cast<double>(-kTwoPi * (cyc - std::floor(cyc)));
      re += w * x[at + n] * std::cos(ph);
      im += w * x[at + n] * std::sin(ph);
    }
    return std::complex<double>(re, im);
  };
  const std::complex<double> a = demod(from), b = demod(from + gap);
  return f + std::arg(b / a) * sr / (6.283185307179586 * static_cast<double>(gap));
}

}  // namespace

TEST_CASE("meta link: Phase accumulator -> Wavetable -> Auto Filter plays an 8-voice 31edo chord, every pitch exact") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId track = ids.track();
  const TuningId t31 = ids.tuning();
  const std::vector<int32_t> degrees{-13, -5, 0, 8, 13, 18, 26, 31};  // neighbours at least 4 Hz apart
  {
    Batch b{"meta chain", {}};
    TuningSlot s{t31, "31edo", Colour{0x2266AA}, {}};
    s.def.system = tuning::Edo::make(31).value();
    b.commands.push_back(AddTuning{s});
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Meta"), {}, kAppend});
    b.commands.push_back(SetTrackTuning{track, TuningAssignment::mainTuning(t31)});
    std::vector<Note> notes;
    for (const int32_t deg : degrees) {
      const int32_t e = deg >= 0 ? deg / 31 : -((-deg + 30) / 31);
      notes.push_back(playback_fixture::note(ids, 0, 14 * kPpq, deg - e * 31, e, 80.0f));
    }
    b.commands.push_back(AddClip{track, playback_fixture::clip(ids, 0, 16 * kPpq, notes, "chord")});
    REQUIRE(store.dispatch(b).ok());
  }
  auto edit = [&](Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  };
  edit(ops::addMetaDevice(*store.current(), ids, track, "phase_acc", 0));
  const DeviceId wt = edit(ops::addMetaDevice(*store.current(), ids, track, "wavetable")).devices.at(0);
  edit(ops::addDevice(*store.current(), ids, track, "auto_filter", DeviceKind::AudioEffect));
  CHECK(findDevice(*store.current(), wt).device->kind == DeviceKind::AudioEffect);
  REQUIRE(chainStages(findTrackPtr(*store.current(), track)->devices, true).at(1).in == ChainSignal::Voices);
  auto link = makeLink(store.current());
  CHECK(link->lastError().empty());
  Driver d{*link};
  link->play(0);
  link->flush();
  d.run(static_cast<uint32_t>(4.5 * 48000.0 / kBlock));
  engine::ProjectProgram& program = *link->engine().program();
  CHECK(program.voices(voiceUnit(program))->liveVoices() == 8);
  const double sr = link->engine().config().sampleRate;
  REQUIRE(sr == 48000.0);
  double worst = 0.0;
  for (const int32_t deg : degrees) {
    const double want = tuning::kDefaultReferenceHz * std::exp2(static_cast<double>(deg) / 31.0);
    const double got =
        measuredHz(d.out, static_cast<size_t>(0.5 * sr), static_cast<size_t>(sr), static_cast<size_t>(2.5 * sr), want, sr);
    worst = std::max(worst, std::fabs(1200.0 * std::log2(got / want)));
  }
  MESSAGE("meta chain in 31edo, 8 voices: max pitch error " << worst << " cents");
  CHECK(worst <= 1e-4);
}

TEST_CASE("meta link: a meta voice run's voices end 5 ms after note-off (no click, no stuck voice)") {
  Keys k;
  k.edit(ops::addMetaDevice(*k.store.current(), k.store.ids(), k.track, "phase_acc", 0));
  k.edit(ops::addMetaDevice(*k.store.current(), k.store.ids(), k.track, "wavetable"));
  auto link = makeLink(k.store.current());
  Driver d{*link};
  link->play(0);
  d.run(200);
  engine::ProjectProgram& program = *link->engine().program();
  const uint32_t u = voiceUnit(program);
  CHECK(program.voices(u)->liveVoices() == 3);
  // Stop: note-offs. The voices ramp down over 5 ms (240 samples), then end.
  link->stop();
  const size_t mark = d.out.size();
  d.run(12);
  CHECK(program.voices(u)->liveVoices() == 0);
  float worstStep = 0.0f, steady = 0.0f;
  for (size_t i = mark; i + 1 < d.out.size(); ++i) worstStep = std::max(worstStep, std::fabs(d.out[i + 1] - d.out[i]));
  for (size_t i = mark - 2000; i + 1 < mark; ++i) steady = std::max(steady, std::fabs(d.out[i + 1] - d.out[i]));
  CHECK(worstStep <= steady * 1.05f + 1e-6f);  // the fade adds no step beyond the tones' own
  CHECK(std::fabs(d.out.back()) < 1e-6f);
}

// ---- TMM.11: the Envelope in a voice run --------------------------------------------------------------------------

TEST_CASE("meta link: Phase accumulator -> Wavetable -> Envelope — the envelope shapes each voice and ends it") {
  Keys k;
  k.edit(ops::addMetaDevice(*k.store.current(), k.store.ids(), k.track, "phase_acc", 0));
  k.edit(ops::addMetaDevice(*k.store.current(), k.store.ids(), k.track, "wavetable"));
  auto e = ops::addMetaDevice(*k.store.current(), k.store.ids(), k.track, "envelope");
  REQUIRE(e.ok());
  const DeviceId env = e->devices.at(0);
  k.edit(std::move(e));
  CHECK(findDevice(*k.store.current(), env).device->kind == DeviceKind::AudioEffect);
  {
    const std::vector<ChainStage> st = chainStages(findTrackPtr(*k.store.current(), k.track)->devices, true);
    REQUIRE(st.size() == 3);
    CHECK(st[1].in == ChainSignal::Voices);
    CHECK(st[2].in == ChainSignal::Voices);
  }
  // A slow attack (100 ms), sustain 0.5, a 200 ms release (−80 dB at 200 ms).
  for (auto [name, v] : {std::pair{"attack", 0.1}, std::pair{"decay", 0.05}, std::pair{"sustain", 0.5},
                         std::pair{"release", 0.2}})
    k.edit(ops::setMetaParam(*k.store.current(), env, "", "envelope", name, v));
  auto link = makeLink(k.store.current());
  CHECK(link->lastError().empty());
  const auto tap = link->metaVoiceTap(env, "", 0);
  REQUIRE(tap != nullptr);
  VoiceTapReader levels(tap);
  Driver d{*link};
  link->play(0);
  d.run(static_cast<uint32_t>(0.5 * 48000.0 / kBlock));
  engine::ProjectProgram& program = *link->engine().program();
  const uint32_t u = voiceUnit(program);
  CHECK(program.voices(u)->liveVoices() == 3);
  // A VCA on the run's signal: the same run without the Envelope, times the envelope — the linear attack i / 4800, then
  // (the decay settled) the sustain 0.5.
  Keys plainKeys;
  plainKeys.edit(ops::addMetaDevice(*plainKeys.store.current(), plainKeys.store.ids(), plainKeys.track, "phase_acc", 0));
  plainKeys.edit(ops::addMetaDevice(*plainKeys.store.current(), plainKeys.store.ids(), plainKeys.track, "wavetable"));
  auto plainLink = makeLink(plainKeys.store.current());
  Driver plain{*plainLink};
  plainLink->play(0);
  plain.run(static_cast<uint32_t>(0.5 * 48000.0 / kBlock));
  REQUIRE(plain.out.size() == d.out.size());
  float attackErr = 0.0f, heldErr = 0.0f;
  for (size_t i = 0; i < 4800; ++i)
    attackErr = std::max(attackErr, std::fabs(d.out[i] - plain.out[i] * static_cast<float>(i + 1) / 4800.0f));
  for (size_t i = 14400; i < d.out.size(); ++i) heldErr = std::max(heldErr, std::fabs(d.out[i] - 0.5f * plain.out[i]));
  MESSAGE("VCA: attack error " << attackErr << ", sustain error " << heldErr);
  CHECK(attackErr <= 1e-4f);
  CHECK(heldErr <= 1e-4f);
  // The tap carries each voice's level (held: 0.4 · 0.5).
  levels.poll(1.0);
  uint32_t active = 0;
  for (uint32_t s = 0; s < levels.voiceSlots(); ++s) {
    if (!levels.voice(s).active) continue;
    ++active;
    const meta::EnvelopeTap t = meta::decodeEnvelopeTap(levels.voice(s).value);
    CHECK_FALSE(t.released);
    CHECK(t.level == doctest::Approx(0.5).epsilon(1e-4));
  }
  CHECK(active == 3);
  // Note-off: the voices sound on through the release (not the run's 5 ms ramp), then end by the envelope's done.
  link->stop();
  d.run(static_cast<uint32_t>(0.1 * 48000.0 / kBlock));
  CHECK(program.voices(u)->liveVoices() == 3);
  d.run(static_cast<uint32_t>(0.15 * 48000.0 / kBlock));
  CHECK(program.voices(u)->liveVoices() == 0);
  CHECK(std::fabs(d.out.back()) < 1e-6f);
}
