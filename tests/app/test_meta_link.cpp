// TMM.3: meta modules through the playback link. GU.4: the Phase Accumulator and the VCA are modules (group presets,
// pq/document/modules.h): a Phase Accumulator's Frequency drag is param writes only (no compile job, no unit compile,
// the unit keeps its image: the unit hash is unchanged) and audible; a Phase Accumulator in an instrument group plays
// its voices; a MetaData edit compiles nothing.
// TMM.10: Phase Accumulator -> Wavetable -> Auto Filter (an instrument group: In -> PA -> Wavetable -> Out) plays exact
// pitches in 31edo; its voices end with the group's release.
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
#include "pq/document/modules.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/document/serialize.h"
#include "pq/engine/project_program.h"
#include "pq/tuning/edo.h"
#include "pq/app/face_voices.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/view_context.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr uint32_t kBlock = 64;

// IG.1-2: the project as an older file of it loads — its meta voice runs (and instruments) in instrument groups, so they
// play per note as these cases were written (pq/document/instrument_groups.h).
Snapshot grouped(const Snapshot& s) {
  Project p = *s;
  return migrateToInstrumentGroups(p) ? std::make_shared<const Project>(std::move(p)) : s;
}
std::unique_ptr<ProjectLink> makeLink(const Snapshot& s0) {
  const Snapshot s = grouped(s0);
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
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
    playback_fixture::wireSeries(store);  // TX.all-free: wired in series
  }
  void edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
  }
};

// GU.4: track `t`'s path made of `blocks` — "pa" a Phase Accumulator module (its roles in `pa`), else a meta type —
// in an instrument group (In -> blocks -> Out, the way an older file's meta voice run loads), its instrument gone.
// Returns the group.
DeviceId instrumentRun(DocumentStore& s, TrackId t, const std::vector<std::string>& blocks, PhaseAccumulatorRoles* pa = nullptr) {
  const DeviceLibrary& lib = playback_fixture::unitsLibrary();
  auto run = [&](Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(s.dispatch(e->command).ok());
    return *e;
  };
  // The track's instrument grouped (an instrument group at the notes), then taken out of it: the group stays, empty.
  if (mainPath(*s.current(), *findTrackPtr(*s.current(), t)).blocks.empty())
    run(ops::addDevice(*s.current(), s.ids(), t, std::string(kDefaultInstrument), DeviceKind::Instrument, 0));
  const DeviceId inst = mainPath(*s.current(), *findTrackPtr(*s.current(), t)).blocks.at(0)->id;
  const DeviceId g = run(ops::groupInCircuit(s.current(), s.ids(), lib, std::vector<DeviceId>{inst})).devices.at(0);
  run(ops::deleteDevices(*s.current(), std::vector<DeviceId>{inst}));
  DeviceId at = g;
  std::string port(kGroupInPort);
  for (const std::string& b : blocks) {
    DeviceId d;
    if (b == "pa") {
      IdSource local;
      PhaseAccumulatorRoles lr;
      const ops::Edit e = run(ops::pasteBlocks(*s.current(), s.ids(), t, phaseAccumulatorBlocks(local, {}, &lr), g));
      auto id = [&](DeviceId l) { return l.valid() ? e.devices.at(l.value - 1) : DeviceId{}; };
      if (pa != nullptr)
        *pa = PhaseAccumulatorRoles{id(lr.group), id(lr.freq), id(lr.phase), id(lr.amp), id(lr.pitch), id(lr.ref),
                                    id(lr.sum), id(lr.ratio), id(lr.pitched), id(lr.phasor), id(lr.addPhase),
                                    id(lr.pm), id(lr.frac), id(lr.out)};
      d = e.devices.at(0);
    } else {
      d = run(ops::addMetaDevice(*s.current(), s.ids(), t, b, ops::kOffPath)).devices.at(0);
      REQUIRE(s.dispatch(SetDeviceGroup{d, g}).ok());
    }
    run(ops::connectWire(*s.current(), s.ids(), at, port, d, ""));
    at = d;
    port.clear();
  }
  run(ops::connectWire(*s.current(), s.ids(), at, port, g, std::string(kGroupOutPort)));
  return g;
}

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

TEST_CASE("meta link: a Phase Accumulator's Frequency drag compiles nothing and is audible") {
  // (GU.4: was a nested meta_phase_acc call's freq; the module's Frequency is a value block's knob, undriven: a param.)
  Keys k;
  PhaseAccumulatorRoles pa;
  (void)instrumentRun(k.store, k.track, {"pa"}, &pa);
  auto link = makeLink(k.store.current());
  [[maybe_unused]] const ListenerId sub = k.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(100);
  CHECK(link->lastError().empty());
  engine::ProjectProgram& program = *link->engine().program();
  const uint32_t u = voiceUnit(program);
  CHECK(program.voices(u)->liveVoices() == 3);
  const engine::ProgramImage* image = program.current(u);
  REQUIRE(image != nullptr);
  const std::string key = "dev" + std::to_string(pa.freq.value) + ":value";
  REQUIRE(link->paramSlot(key) != ProjectLink::kNoSlot);
  const size_t mark = d.out.size();
  d.run(100);
  const double before = rms(d.out, mark, d.out.size());
  // A drag: 16 values in one gesture. Param writes only.
  app::LinkStats s0 = link->stats();
  REQUIRE(k.store.beginGesture().ok());
  for (int i = 1; i <= 16; ++i) {
    k.edit(ops::setDeviceParamValue(*k.store.current(), link->devices(), pa.freq, std::string(kValueParam), 250.0 * i));
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
  // Back to its default: still a write.
  s0 = link->stats();
  k.edit(ops::setDeviceParam(*k.store.current(), pa.freq, std::string(kValueParam), std::nullopt));
  link->flush();
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().paramWrites == s0.paramWrites + 1);
}

TEST_CASE("meta link: a Phase Accumulator in an instrument group plays its voices; MetaData edits compile nothing") {
  meta::testing::registerTestType();
  Keys k;
  auto link = makeLink(k.store.current());
  [[maybe_unused]] const ListenerId sub = k.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(20);
  // The Phase Accumulator replaces the instrument (In -> its group -> Out of an instrument group).
  PhaseAccumulatorRoles pa;
  (void)instrumentRun(k.store, k.track, {"pa"}, &pa);
  link->flush();
  CHECK(link->lastError().empty());
  REQUIRE(isGroupDevice(*findDevice(*k.store.current(), pa.group).device));
  link->stop();
  d.run(5);
  link->play(0);
  d.run(100);
  engine::ProjectProgram& program = *link->engine().program();
  const uint32_t u = voiceUnit(program);
  CHECK(program.voices(u)->liveVoices() == 3);
  const size_t mark = d.out.size();
  d.run(50);
  CHECK(rms(d.out, mark, d.out.size()) > 1e-3);  // three ramps (DC and all) through the mixer
  // Its Amplitude is a value block's param: a write, no compile.
  app::LinkStats s0 = link->stats();
  k.edit(ops::setDeviceParamValue(*k.store.current(), link->devices(), pa.amp, std::string(kValueParam), 0.5));
  link->flush();
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().paramWrites == s0.paramWrites + 1);
  // A data edit (a nested test module's samples) compiles nothing either. (TMM.13-fix1 L6: the state sits on a stock
  // device of the chain.)
  const DeviceId util = [&] {
    auto e = ops::addDevice(*k.store.current(), k.store.ids(), k.track, "utility", DeviceKind::AudioEffect);
    REQUIRE(e.ok());
    REQUIRE(k.store.dispatch(e->command).ok());
    return e->devices.at(0);
  }();
  k.edit(ops::setMetaSetting(*k.store.current(), util, "x", "test_shape", "stages", 1.0));  // state only: no call
  link->flush();
  s0 = link->stats();
  k.edit(ops::setMetaData(*k.store.current(), util, "x", "test_shape",
                          std::make_shared<const meta::DataValue>(meta::DataValue::record({{"values", meta::DataValue::floatArray({0.25f})}}))));
  CHECK(k.store.lastChanges().metaDataOnly());
  link->flush();
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().paramWrites == s0.paramWrites);
}

TEST_CASE("meta link (TMM.10-fix1): a free Phase Accumulator plays its bar's phase once, whatever notes play") {
  // (GU.4: the module off the path, nothing into its In, its output into the mixer; was an unplugged chain module.)
  Keys k;  // a held three-note chord
  const DeviceLibrary& lib = playback_fixture::unitsLibrary();
  k.edit(ops::deleteDevices(*k.store.current(), std::vector<DeviceId>{k.inst}));
  IdSource local;
  PhaseAccumulatorRoles lr;
  auto e = ops::pasteBlocks(*k.store.current(), k.store.ids(), k.track, phaseAccumulatorBlocks(local, {}, &lr));
  REQUIRE(e.ok());
  const DeviceId acc = e->devices.at(0), freq = e->devices.at(lr.freq.value - 1);
  k.edit(std::move(e));
  k.edit(ops::setDeviceParamValue(*k.store.current(), lib, freq, std::string(kValueParam), 100.0));
  Wire m;
  m.from = acc;
  m.mixer = k.track;
  k.edit(ops::connect(*k.store.current(), k.store.ids(), m));
  auto link = makeLink(k.store.current());
  CHECK(link->lastError().empty());
  Driver d{*link};
  link->play(0);
  d.run(10);
  const size_t mark = d.out.size();
  d.run(750);  // 1 s
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

TEST_CASE("meta link: Phase Accumulator -> Wavetable -> Auto Filter plays an 8-voice 31edo chord, every pitch exact") {
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
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
    playback_fixture::wireSeries(store);  // TX.all-free: wired in series
  }
  auto edit = [&](Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  };
  // GU.4: In -> Phase Accumulator -> Wavetable -> Out (an instrument group), then the Auto Filter.
  const DeviceId g = instrumentRun(store, track, {"pa", "wavetable"});
  edit(ops::addDevice(*store.current(), ids, track, "auto_filter", DeviceKind::AudioEffect));
  CHECK(mainPath(*store.current(), *findTrackPtr(*store.current(), track)).blocks.at(0)->id == g);
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

TEST_CASE("meta link: an instrument group's voices end 5 ms after note-off (no click, no stuck voice)") {
  Keys k;
  (void)instrumentRun(k.store, k.track, {"pa", "wavetable"});
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

TEST_CASE("meta link: Phase Accumulator -> Wavetable -> Envelope — the envelope shapes each voice and ends it") {
  Keys k;
  const DeviceId g = instrumentRun(k.store, k.track, {"pa", "wavetable", "envelope"});
  DeviceId env;
  for (const DevicePtr& d : findTrackPtr(*k.store.current(), k.track)->devices)
    if (d->group == g && d->patch == "meta:envelope") env = d->id;
  REQUIRE(env.valid());
  CHECK(findDevice(*k.store.current(), env).device->kind == DeviceKind::AudioEffect);
  // A slow attack (100 ms), sustain 0.5, a 200 ms release (−80 dB at 200 ms).
  for (auto [name, v] : {std::pair{"attack", 0.1}, std::pair{"decay", 0.05}, std::pair{"sustain", 0.5},
                         std::pair{"release", 0.2}})
    k.edit(ops::setMetaParamValue(*k.store.current(), env, "", "envelope", name, v));
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
  (void)instrumentRun(plainKeys.store, plainKeys.track, {"pa", "wavetable"});
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

// ---- TMM.14: the VCA through the link ------------------------------------------------------------------------------

TEST_CASE("meta link (TMM.14): a VCA knob's changes glide over 20 ms (no click; TX.smooth) and compile nothing; its curve too") {
  // GU.4: an unwired VCA (a constant source of its value) is a Value block with the VCA's curve (what such a VCA converts
  // to); its knob and its curve's range edits are param writes that glide.
  Keys k;
  const DeviceLibrary& lib = playback_fixture::unitsLibrary();
  k.edit(ops::deleteDevices(*k.store.current(), std::vector<DeviceId>{k.inst}));
  const DeviceId vca = [&] {
    auto e = ops::addDevice(*k.store.current(), k.store.ids(), k.track, std::string(kValueDevice), DeviceKind::AudioEffect, ops::kOffPath);
    REQUIRE(e.ok());
    REQUIRE(k.store.dispatch(e->command).ok());
    return e->devices.at(0);
  }();
  const std::string vp(kValueParam);
  const dsp::Curve law = vcaCurve();
  k.edit(ops::setParamCurve(*k.store.current(), lib, vca, vp, law));
  k.edit(ops::setDeviceParamValue(*k.store.current(), lib, vca, vp, 0.5));
  Wire m;
  m.from = vca;
  m.mixer = k.track;
  k.edit(ops::connect(*k.store.current(), k.store.ids(), m));
  auto link = makeLink(k.store.current());
  [[maybe_unused]] const ListenerId sub = k.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  CHECK(link->lastError().empty());
  Driver d{*link};
  link->play(0);
  d.run(20);
  const float g = d.out.back() / 0.5f;  // the mixer's gain
  CHECK(g > 0.6f);
  CHECK(g < 0.8f);
  float spread = 0.0f;  // settled: a DC
  for (size_t i = d.out.size() - 64; i < d.out.size(); ++i) spread = std::max(spread, std::fabs(d.out[i] - d.out.back()));
  CHECK(spread <= 1e-5f);  // (the mixer's own float rounding)
  // Steps: up to 20 (a knob jump across the fine zone into the log part), down to −3, then min moved under a held knob
  // (a curve edit: the same knob position plays the new curve's value).
  const app::LinkStats s0 = link->stats();
  const dsp::Curve narrow = vcaCurve(-100.0, 20000.0);
  const double p3 = dsp::curvePosition(law, -3.0, 0.5);
  float from = 0.5f;
  for (int step = 0; step < 3; ++step) {
    CAPTURE(step);
    double want = 0.0;
    if (step == 0) {
      k.edit(ops::setDeviceParamValue(*k.store.current(), lib, vca, vp, 20.0));
      want = 20.0;
    } else if (step == 1) {
      k.edit(ops::setDeviceParam(*k.store.current(), vca, vp, p3));
      want = -3.0;
    } else {
      k.edit(ops::setParamCurve(*k.store.current(), lib, vca, vp, narrow));
      want = dsp::evalCurve(narrow, p3);
    }
    link->flush();
    const size_t mark = d.out.size();
    d.run(16);  // TX.smooth: every param glides over 20 ms (960 frames: 15 blocks), then one more block at rest
    const float to = static_cast<float>(want);
    CHECK(d.out.back() / g == doctest::Approx(to).epsilon(1e-3));
    // TX.smooth (pq/graph/schedule.h "Param ramps"): a linear glide from the old value to the new over 20 ms: monotonic,
    // no sample-to-sample step above the glide's (1/960 of the distance), never past the target.
    const float dist = std::fabs(to - from) * g, maxStep = dist / 960.0f * 1.02f + 1e-6f;
    float worst = 0.0f;
    bool monotonic = true, beyond = false;
    for (size_t i = mark; i < d.out.size(); ++i) {
      const float prev = i == mark ? from * g : d.out[i - 1];
      worst = std::max(worst, std::fabs(d.out[i] - prev));
      monotonic = monotonic && (to > from ? d.out[i] >= prev - 1e-6f : d.out[i] <= prev + 1e-6f);
      const float tol = std::fabs(to * g) * 1e-4f + 1e-6f;
      beyond = beyond || (to > from ? d.out[i] > to * g + tol : d.out[i] < to * g - tol);
    }
    CHECK(worst <= maxStep);
    CHECK(worst > 0.5f * maxStep);  // it moved at once (smoothed, not delayed)
    CHECK(monotonic);
    CHECK_FALSE(beyond);
    from = to;
  }
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().unitCompiles == s0.unitCompiles);
  CHECK(link->stats().paramWrites >= s0.paramWrites + 3);
}

// ---- TMM.17 ---------------------------------------------------------------------------------------------------------

// The engine error "voices: the patch has no voice stages" (bug_reports 2026-11-01_13-50-52 / 13-54-55): a MIDI track
// whose instrument came or went kept its unit's voice context. ProjectCompiler::update compared only the units' tracks,
// not their instrument member, so a track that lost its instrument (deleted, an Undo Add Device back to an empty track)
// was recompiled as a voice unit with a patch that has no voice stages (the VoiceEngine refused the image, and the old
// image played on: the deleted instrument kept sounding), and one that gained an instrument was compiled without a voice
// context ("voice.hz is only available in a voice context": the track stayed silent, every later edit of it failed,
// until something rebuilt the whole program).
TEST_CASE("meta link (TMM.17): a MIDI track's instrument coming and going rebuilds its unit (no voice-context errors)") {
  Keys k;  // BasicSynth and a held chord
  auto link = makeLink(k.store.current());
  [[maybe_unused]] const ListenerId sub = k.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(20);
  CHECK(rms(d.out, d.out.size() - 640, d.out.size()) > 1e-3);
  auto expectClean = [&](const char* what) {
    INFO(std::string(what));
    link->flush();
    CHECK(link->stats().compileErrors == 0);
    CHECK_MESSAGE(link->lastError().empty(), link->lastError());
  };
  auto replay = [&]() {  // the held chord again from its start (notes held before an instrument existed stay unheard)
    link->stop();
    d.run(10);
    link->play(0);
    d.run(30);
    return rms(d.out, d.out.size() - 640, d.out.size());
  };
  // The instrument deleted: the track plays nothing, and its unit stops being a voice unit.
  const uint64_t topo0 = link->stats().topologyChanges;
  k.edit(ops::deleteDevices(*k.store.current(), std::vector<DeviceId>{k.inst}));
  expectClean("instrument deleted");
  CHECK(link->stats().topologyChanges == topo0 + 1);
  CHECK(replay() == 0.0);
  engine::ProjectProgram& program = *link->engine().program();
  for (uint32_t u = 0; u < program.numUnits(); ++u) CHECK_FALSE(program.layout().units[u].voices);
  // A Wavetable arrives as the instrument: a voice unit again, playing the chord. (GU.4: was a Phase accumulator.)
  k.edit(ops::addMetaDevice(*k.store.current(), k.store.ids(), k.track, "wavetable", 0));
  expectClean("wavetable added");
  CHECK(replay() > 1e-3);
  CHECK(program.voices(voiceUnit(program))->liveVoices() == 3);
  // Undo Add Device: an empty track again; Redo: the Wavetable plays again.
  REQUIRE(k.store.undo());
  expectClean("undo add");
  CHECK(replay() == 0.0);
  REQUIRE(k.store.redo());
  expectClean("redo add");
  CHECK(replay() > 1e-3);
  // A stock instrument onto an empty track too.
  REQUIRE(k.store.undo());
  k.edit(ops::addDevice(*k.store.current(), k.store.ids(), k.track, "BasicSynth", DeviceKind::Instrument));
  expectClean("BasicSynth added");
  CHECK(replay() > 1e-3);
}

// The status line said "Device 'meta:<type>' is not available: it is bypassed" for every chain meta device (the same
// reports): the check looked the device up in the library by its patch name, which a meta device is not in.
TEST_CASE("meta link (TMM.17): chain meta devices are not reported as missing; a missing stock device still is") {
  Keys k;
  auto link = makeLink(k.store.current());
  views::ViewContext ctx(*link);
  ctx.store().reset(k.store.current());
  ctx.store().ids().reserveThrough(k.store.ids().peek());
  link->attach(ctx);
  link->flush();
  auto edit = [&](Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(ctx.store().dispatch(e->command).ok());
    link->flush();
  };
  edit(ops::addMetaDevice(*ctx.doc(), ctx.store().ids(), k.track, "envelope", 0));
  edit(ops::addMetaDevice(*ctx.doc(), ctx.store().ids(), k.track, "wavetable"));
  edit(ops::addMetaDevice(*ctx.doc(), ctx.store().ids(), k.track, "spectrum"));
  CHECK(ctx.status().find("not available") == std::string::npos);
  edit(ops::addDevice(*ctx.doc(), ctx.store().ids(), k.track, "NoSuchDevice", DeviceKind::AudioEffect));
  CHECK(ctx.status().find("Device 'NoSuchDevice' is not available") != std::string::npos);
  CHECK(ctx.status().find("more") == std::string::npos);  // only that one
  link->detach();
}

