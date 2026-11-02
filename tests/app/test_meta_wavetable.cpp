// TMM.5: the Wavetable meta module through the playback link — keyframe edits and baked knobs reach the engine as new
// tables built off-thread (no compile), the swap is click-free end to end, offline renders wait for pending builds,
// identical specs share one table, a rate change rebuilds, the golden `wavetable-morph`, the alias suite on the
// module's own output, and (the TMM.2 / TMM.4 join) faces read the real voice taps: a played note is a dot.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <filesystem>
#include <numbers>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../golden/golden_audio.h"
#include "../support/alias_meter.h"
#include "playback_fixture.h"
#include "pq/app/face_voices.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/dsp/fft.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/engine/offline_render.h"
#include "pq/engine/project_program.h"
#include "pq/engine/wav_file.h"
#include "pq/meta/phase_acc.h"
#include "pq/meta/table_builder.h"
#include "pq/meta/wavetable.h"
#include "../ui/golden_check.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scene_textures.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/meta/tap_voices.h"
#include "pq/dsp/wave_warp.h"
#include "pq/ui/theme.h"
#include "../../src/views/src/meta/faces.h"  // TMM.16: the Wavetable face's drawn curve (tests)

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kBlock = 64;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s, double rate = 48000.0) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  c.sampleRate = rate;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

struct Driver {
  ProjectLink& link;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  std::vector<float> out;  // left channel
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;  // RT safety of every block, also while tables are built and published
        DenormalGuard dg;
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      out.insert(out.end(), l.begin(), l.end());
    }
  }
};

std::vector<float> cycleOf(dsp::BasicCycle c) {
  const AlignedBuffer<float> b = dsp::basicCycle(c);
  return {b.span().begin(), b.span().end()};
}

meta::DataPtr oneCycle(dsp::BasicCycle c) {
  meta::WavetableData d;
  d.keyframes.push_back({0.0f, cycleOf(c)});
  return meta::wavetableData(d);
}

meta::DataPtr sawToSquare(dsp::GapMode gap = dsp::GapMode::Spectral) {
  meta::WavetableData d;
  d.keyframes.push_back({0.0f, cycleOf(dsp::BasicCycle::Saw)});
  d.keyframes.push_back({1.0f, cycleOf(dsp::BasicCycle::Square)});
  d.gaps = {gap};
  return meta::wavetableData(d);
}

// MIDI tracks whose instrument is a chain Wavetable, each holding notes.
struct WtSong {
  DocumentStore store;
  std::vector<TrackId> tracks;
  std::vector<DeviceId> wts;
  WtSong() { must(SetTempoMap{{{0, 120.0, TempoCurve::Step}}}); }
  void must(Command c) {
    const Status st = store.dispatch(std::move(c));
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  void edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    must(e->command);
  }
  const Project& doc() const { return *store.current(); }
  // A track with a chain Wavetable and one note per (start, length, degree).
  DeviceId track(std::vector<std::array<Ticks, 3>> notes) {
    auto t = ops::createTrack(doc(), store.ids(), TrackKind::Midi);
    REQUIRE(t.ok());
    must(t->command);
    const TrackId id = t->tracks.at(0);
    auto w = ops::addMetaDevice(doc(), store.ids(), id, "wavetable", 0);
    REQUIRE(w.ok());
    must(w->command);
    std::vector<Note> ns;
    Ticks end = 0;
    for (const auto& n : notes) {
      ns.push_back(playback_fixture::note(store.ids(), n[0], n[1], static_cast<int32_t>(n[2])));
      end = std::max(end, n[0] + n[1]);
    }
    if (!ns.empty()) must(AddClip{id, playback_fixture::clip(store.ids(), 0, end, ns, "notes")});
    tracks.push_back(id);
    wts.push_back(w->devices.at(0));
    return w->devices.at(0);
  }
  void data(DeviceId d, const meta::DataPtr& v) { edit(ops::setMetaData(doc(), d, "", "wavetable", v)); }
  void param(DeviceId d, const char* name, double v) { edit(ops::setMetaParam(doc(), d, "", "wavetable", name, v)); }
  void setting(DeviceId d, const char* name, double v) { edit(ops::setMetaSetting(doc(), d, "", "wavetable", name, v)); }
  void lane(size_t track, DeviceId d, const char* name, std::vector<Breakpoint> points) {
    auto l = std::make_shared<AutomationLane>();
    l->id = store.ids().lane();
    l->param = ParamRef::device(d, name);
    l->paramName = name;
    for (Breakpoint& p : points) p.id = store.ids().breakpoint();
    l->points = std::move(points);
    must(AddAutomationLane{tracks.at(track), l});
  }
};

double maxStep(const std::vector<float>& x, size_t from, size_t to) {
  double m = 0.0;
  for (size_t i = std::max<size_t>(from, 1); i < to; ++i) m = std::max(m, std::fabs(double(x[i]) - x[i - 1]));
  return m;
}
double peakOf(const std::vector<float>& x, size_t from, size_t to) {
  double m = 0.0;
  for (size_t i = from; i < to; ++i) m = std::max(m, std::fabs(double(x[i])));
  return m;
}
// Σ(Δx)² / Σx²: (2πf/sr)² for a sine; a saw's harmonics raise it many times.
double brightness(const std::vector<float>& x, size_t from, size_t to) {
  double e = 0, de = 0;
  for (size_t i = from + 1; i < to; ++i) {
    e += double(x[i]) * x[i];
    de += (double(x[i]) - x[i - 1]) * (double(x[i]) - x[i - 1]);
  }
  return de / std::max(e, 1e-30);
}

// The table a direct build of the instance's current state gives (what the link must have published).
std::shared_ptr<const dsp::FrameTable> directTable(const Project& p, DeviceId dev, std::string_view path, double rate) {
  const Device& d = *findDevice(p, dev).device;
  const meta::Type& t = meta::wavetableType();
  const MetaInstance* inst = d.metaAt(path);
  meta::TableSource src;
  src.data = inst != nullptr && inst->data ? inst->data : meta::wavetableDefaultData();
  src.dataHash = meta::dataHash(*src.data);
  std::vector<double> params(t.params.size());
  for (size_t i = 0; i < params.size(); ++i) params[i] = metaParamValue(d, path, t, i);
  const std::vector<double> settings = metaSettings(d, path, t);
  src.params = params;
  src.settings = settings;
  src.sampleRate = rate;
  meta::TableJob job;
  REQUIRE(t.frameTable(src, 0, job).ok());
  dsp::FrameTableBuilder b;
  auto built = b.build(job.spec);
  REQUIRE(built.ok());
  return std::make_shared<const dsp::FrameTable>(std::move(built.value()));
}

bool sameTable(const dsp::FrameTable& a, const dsp::FrameTable& b) {
  if (a.frames() != b.frames() || a.layout().levels != b.layout().levels) return false;
  for (uint32_t f = 0; f < a.frames(); ++f)
    for (uint32_t j = 0; j < a.layout().levels; ++j) {
      const auto ra = a.row(f, j), rb = b.row(f, j);
      if (ra.size() != rb.size() || !std::equal(ra.begin(), ra.end(), rb.begin())) return false;
    }
  return true;
}

}  // namespace

TEST_CASE("meta wavetable: a keyframe edit is audible without a compile; the swap is click-free; RT-safe while playing") {
  WtSong s;
  const DeviceId wt = s.track({{0, 64 * kPpq, 0}});
  auto link = makeLink(s.store.current());
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  REQUIRE(link->waitForMetaTables());
  const auto slot = link->metaFrameTable(wt, "", 0);
  REQUIRE(slot != nullptr);
  CHECK(slot->generation() == 2);  // the default sine, then the instance's own (one keyframe: one frame)
  CHECK(slot->table()->frames() == 1);
  Driver d{*link};
  link->play(0);
  d.run(200);
  engine::ProjectProgram& program = *link->engine().program();
  const engine::ProgramImage* image = nullptr;
  for (uint32_t u = 0; u < program.numUnits(); ++u)
    if (program.layout().units[u].voices) image = program.current(u);
  REQUIRE(image != nullptr);
  const app::LinkStats s0 = link->stats();
  const uint64_t gen = slot->generation();
  // The edit: saw → square, 64 dense frames. Blocks keep running while the table builds (as a device would).
  const size_t mark = d.out.size();
  const auto t0 = std::chrono::steady_clock::now();
  s.data(wt, sawToSquare());
  CHECK(s.store.lastChanges().metaDataOnly());
  uint32_t blocks = 0;
  while (slot->generation() == gen && blocks < 48000) {
    d.run(1);
    ++blocks;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE(slot->generation() == gen + 1);
  MESSAGE("keyframe edit (64 frames) to the table published: " << ms << " ms (" << blocks << " blocks)");
  const size_t swap = d.out.size();
  d.run(400);
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().unitCompiles == s0.unitCompiles);
  CHECK(program.current(0) != nullptr);
  bool same = false;
  for (uint32_t u = 0; u < program.numUnits(); ++u) same = same || program.current(u) == image;
  CHECK(same);  // the unit kept its image
  CHECK(slot->table()->frames() == meta::kWtDefaultFrames);
  CHECK(sameTable(*slot->table(), *directTable(s.doc(), wt, "", 48000.0)));
  // Click-free (TMM.2's bound end to end, QUESTIONS Q170 1): around the swap no step exceeds the larger of the two
  // tables' own steps plus the fade's share of their difference (≤ (peak_a + peak_b) / 64).
  const size_t w = 4096;
  const double stepA = maxStep(d.out, mark - w, mark), stepB = maxStep(d.out, swap + 4 * kBlock, swap + 4 * kBlock + w);
  const double peakA = peakOf(d.out, mark - w, mark), peakB = peakOf(d.out, swap + 4 * kBlock, swap + 4 * kBlock + w);
  const double stepSwap = maxStep(d.out, swap - 2 * kBlock, swap + 3 * kBlock);
  MESSAGE("steps: sine " << stepA << ", saw " << stepB << ", around the swap " << stepSwap);
  CHECK(stepSwap <= std::max(stepA, stepB) + (peakA + peakB) / dsp::kFrameTableFadeFrames + 1e-6);
  // Audible: sine → saw.
  CHECK(brightness(d.out, swap + 8 * kBlock, d.out.size()) > 5.0 * brightness(d.out, mark - w, mark));
}

TEST_CASE("meta wavetable: a baked fold drag compiles nothing, is one undo step, and the audio follows") {
  WtSong s;
  const DeviceId wt = s.track({{0, 64 * kPpq, 0}});
  s.data(wt, oneCycle(dsp::BasicCycle::Saw));
  auto link = makeLink(s.store.current());
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  REQUIRE(link->waitForMetaTables());
  Driver d{*link};
  link->play(0);
  d.run(100);
  const auto slot = link->metaFrameTable(wt, "", 0);
  REQUIRE(slot != nullptr);
  const auto plain = slot->table();
  const app::LinkStats s0 = link->stats();
  const size_t undo = s.store.history().cursor();
  const size_t before = d.out.size();
  REQUIRE(s.store.beginGesture("fold").ok());
  for (int i = 1; i <= 8; ++i) {
    s.param(wt, "fold", 0.1 * i);
    d.run(4);
  }
  REQUIRE(s.store.commitGesture().ok());
  REQUIRE(link->waitForMetaTables());
  d.run(20);
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().unitCompiles == s0.unitCompiles);
  CHECK(s.store.history().cursor() == undo + 1);
  CHECK(metaParamValue(*findDevice(s.doc(), wt).device, "", meta::wavetableType(), meta::kWtFold) ==
        doctest::Approx(0.8));
  // The slot plays the table of fold 0.8 (bit for bit what a direct build gives), and it sounds different.
  REQUIRE(slot->table() != plain);
  CHECK(sameTable(*slot->table(), *directTable(s.doc(), wt, "", 48000.0)));
  const size_t after = d.out.size();
  d.run(100);
  bool differs = false;
  for (size_t i = 0; i < 4096 && !differs; ++i) differs = std::fabs(d.out[before - 4096 + i]) != std::fabs(d.out[after + i]);
  CHECK(differs);
  const meta::TableBuilder::Stats st = link->metaTableStats();
  MESSAGE("fold drag of 8 values: built " << st.built << ", replaced " << st.replaced << ", shared " << st.shared
                                          << "; last build " << st.lastBuildMs << " ms (max " << st.maxBuildMs << ")");
  // Undo restores fold 0: the plain table again (the cache still holds it: no build).
  const uint64_t built = st.built;
  REQUIRE(s.store.undo());
  REQUIRE(link->waitForMetaTables());
  CHECK(slot->table() == plain);
  CHECK(link->metaTableStats().built == built);
}

namespace {

// Amplitude spectrum (dB re full scale) of n samples from `from`, Blackman-Harris windowed.
std::vector<double> spectrumDb(const std::vector<float>& x, size_t from, uint32_t n) {
  std::vector<double> re(n), im(n, 0.0);
  for (uint32_t i = 0; i < n; ++i) {
    const double t = 2.0 * 3.14159265358979 * i / (n - 1);
    const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
    re[i] = x[from + i] * w;
  }
  dsp::fftInPlace(re, im, false);
  std::vector<double> db(n / 2);
  for (uint32_t b = 0; b < n / 2; ++b) db[b] = 20.0 * std::log10(std::max(std::hypot(re[b], im[b]) / (0.35875 * n / 2), 1e-12));
  return db;
}

// The largest difference (dB) between two spectra over bins whose level is above −60 dBFS in either, below 18 kHz.
double spectrumDiff(const std::vector<double>& a, const std::vector<double>& b, double sr) {
  double worst = 0;
  const size_t n = a.size() * 2;
  for (size_t k = 1; k < a.size() && k * sr / n < 18000.0; ++k)
    if (std::max(a[k], b[k]) > -60.0) worst = std::max(worst, std::fabs(a[k] - b[k]));
  return worst;
}

}  // namespace

TEST_CASE("meta wavetable: automating fold switches to the live path and back (one compile each), seamlessly") {
  // TMM.10's Accept (Q175 10) with TMM.8-fix's criteria: the live path's table is built before the recompiled image
  // plays (its own slot: baked and live never mix), and the live-band bake makes the two paths sound the same, so a
  // lane holding the knob's value changes nothing audible in either direction.
  WtSong s;
  const DeviceId wt = s.track({{0, 64 * kPpq, 0}});
  s.data(wt, oneCycle(dsp::BasicCycle::Saw));
  auto link = makeLink(s.store.current());
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  REQUIRE(link->waitForMetaTables());
  // A static fold: baked (no compile).
  s.param(wt, "fold", 0.4);
  link->flush();
  REQUIRE(link->waitForMetaTables());
  const auto bakedSlot = link->metaFrameTable(wt, "", 0);
  REQUIRE(bakedSlot != nullptr);
  const auto baked = bakedSlot->table();
  Driver d{*link};
  link->play(0);
  d.run(200);
  const double stepBaked = maxStep(d.out, d.out.size() - 8192, d.out.size());
  const std::vector<double> before = spectrumDb(d.out, d.out.size() - 8192, 8192);
  // A lane holding 0.4: one structural compile onto the live path, in a new slot whose table bakes no fold.
  const app::LinkStats s0 = link->stats();
  s.lane(0, wt, "fold", {Breakpoint{0, 0.4f, 0.0f, {}}, Breakpoint{64 * kPpq, 0.4f, 0.0f, {}}});
  const size_t at1 = d.out.size();
  link->flush();
  REQUIRE(link->lastError().empty());
  CHECK(link->stats().compiles == s0.compiles + 1);
  const auto liveSlot = link->metaFrameTable(wt, "", 0);
  REQUIRE(liveSlot != nullptr);
  CHECK(liveSlot != bakedSlot);           // its own slot
  CHECK(liveSlot->table() != baked);      // already built when the image was published (no wait here)
  CHECK(bakedSlot->table() == baked);     // the old image's table is untouched
  d.run(200);
  const double stepSwitch1 = maxStep(d.out, at1, at1 + 4096);
  const std::vector<double> live = spectrumDb(d.out, d.out.size() - 8192, 8192);
  // Back: the lane deleted, one compile to the baked path again.
  const app::LinkStats s1 = link->stats();
  const LaneId laneId = findTrackPtr(s.doc(), s.tracks.at(0))->lanes.at(0)->id;
  s.must(RemoveAutomationLane{s.tracks.at(0), laneId});
  const size_t at2 = d.out.size();
  link->flush();
  CHECK(link->stats().compiles == s1.compiles + 1);
  const auto back = link->metaFrameTable(wt, "", 0);
  REQUIRE(back != nullptr);
  CHECK(back->table() != nullptr);
  CHECK(sameTable(*back->table(), *baked));
  d.run(200);
  const double stepSwitch2 = maxStep(d.out, at2, at2 + 4096);
  const std::vector<double> after = spectrumDb(d.out, d.out.size() - 8192, 8192);
  const double dLive = spectrumDiff(before, live, 48000.0), dBack = spectrumDiff(before, after, 48000.0);
  MESSAGE("fold 0.4, baked -> live -> baked: largest step " << stepBaked << " / " << stepSwitch1 << " / " << stepSwitch2
                                                           << "; spectrum difference live " << dLive << " dB, back "
                                                           << dBack << " dB");
  // No click: the switches' largest sample step stays within 5 % of the steady signal's own (the saw's edge); the
  // spectra of the three steady stretches agree within 0.1 dB over every bin above −60 dBFS.
  CHECK(stepSwitch1 <= 1.05 * stepBaked + 1e-3);
  CHECK(stepSwitch2 <= 1.05 * stepBaked + 1e-3);
  CHECK(dLive <= 0.1);
  CHECK(dBack <= 0.1);
}

TEST_CASE("meta wavetable: a Wavetable effect in Shape mode shapes the track's audio (ADAA, oversampled)") {
  WtSong s;
  // A MIDI track with BasicSynth and a Wavetable effect after it: Shape mode by default.
  auto t = ops::createTrack(s.doc(), s.store.ids(), TrackKind::Midi);
  REQUIRE(t.ok());
  s.must(t->command);
  const TrackId id = t->tracks.at(0);
  auto w = ops::addMetaDevice(s.doc(), s.store.ids(), id, "wavetable", 1);
  REQUIRE(w.ok());
  s.must(w->command);
  const DeviceId fx = w->devices.at(0);
  REQUIRE(findDevice(s.doc(), fx).device->kind == DeviceKind::AudioEffect);
  s.must(AddClip{id, playback_fixture::clip(s.store.ids(), 0, 8 * kPpq,
                                            {playback_fixture::note(s.store.ids(), 0, 8 * kPpq, 0)}, "notes")});
  s.data(fx, oneCycle(dsp::BasicCycle::Square));
  auto link = makeLink(s.store.current());
  REQUIRE(link->lastError().empty());
  REQUIRE(link->waitForMetaTables());
  const auto slot = link->metaFrameTable(fx, "", 0);
  REQUIRE(slot != nullptr);
  CHECK(slot->table()->hasAntiderivative());
  Driver d{*link};
  link->play(0);
  d.run(400);
  double peak = 0;
  for (const float v : d.out) peak = std::max(peak, std::fabs(static_cast<double>(v)));
  CHECK(peak > 0.05);
  CHECK(std::all_of(d.out.begin(), d.out.end(), [](float v) { return std::isfinite(v); }));
}

TEST_CASE("meta wavetable: offline renders wait for pending builds; identical specs share one table; a rate change rebuilds") {
  WtSong s;
  const DeviceId a = s.track({{0, 4 * kPpq, 0}});
  const DeviceId b = s.track({{0, 4 * kPpq, 7}});
  auto link = makeLink(s.store.current());
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  s.data(a, sawToSquare());
  s.data(b, sawToSquare());
  s.param(a, "position", 0.4);
  engine::BufferSink first;
  REQUIRE(link->renderOffline(0, 9600, first).ok());  // without waiting: the render must
  const auto sa = link->metaFrameTable(a, "", 0), sb = link->metaFrameTable(b, "", 0);
  REQUIRE(sa != nullptr);
  REQUIRE(sb != nullptr);
  CHECK(sa->table() == sb->table());  // one table for both instances
  CHECK(sa->table()->frames() == meta::kWtDefaultFrames);
  auto fresh = makeLink(s.store.current());
  engine::BufferSink second;
  REQUIRE(fresh->renderOffline(0, 9600, second).ok());
  REQUIRE(first.channels.size() == 2);
  CHECK(first.channels == second.channels);  // deterministic: the render did not start on the default tables
  CHECK(peakOf(first.channels[0], 0, 9600) > 0.1);
  // Another rate: the defaults are republished and every instance's table rebuilt at the new rate.
  REQUIRE(link->setSampleRate(96000.0).ok());
  REQUIRE(link->waitForMetaTables());
  const auto sa2 = link->metaFrameTable(a, "", 0);
  REQUIRE(sa2 != nullptr);
  CHECK(sa2->table()->layout().sampleRate == 96000.0);
  CHECK(sa2->table()->frames() == meta::kWtDefaultFrames);
}

TEST_CASE("meta wavetable: File > Open of another project never plays the old project's table (TMM.13-fix1 H2)") {
  // Device ids restart per project, so project B's first Wavetable has the id of A's. Meta slots were keyed by
  // (device, path, index) and kept over a Reset: B's instance played A's table (and its first preview was built over
  // it) until its own build arrived. A Reset now drops every slot: B's first block is what a fresh link plays.
  WtSong a, b;
  const DeviceId wa = a.track({{0, 4 * kPpq, 0}});
  const DeviceId wb = b.track({{0, 4 * kPpq, 0}});
  REQUIRE(wa == wb);
  a.data(wa, sawToSquare());
  b.data(wb, oneCycle(dsp::BasicCycle::Sine));
  auto link = makeLink(a.store.current());
  REQUIRE(link->waitForMetaTables());
  const auto slotA = link->metaFrameTable(wa, "", 0);
  REQUIRE(slotA != nullptr);
  REQUIRE(slotA->table()->frames() == meta::kWtDefaultFrames);
  const meta::TableBuilder::Stats before = link->metaTableStats();
  const doc::ChangeSet none;
  link->onDocumentChange(StoreEvent{StoreEventKind::Reset, a.store.current(), b.store.current(), none});
  link->flush();
  const auto slotB = link->metaFrameTable(wb, "", 0);
  REQUIRE(slotB != nullptr);
  CHECK(slotB != slotA);  // a new slot object: nothing of A's carries over
  CHECK(slotB->table()->frames() == 1);  // B's own table, already there when its image plays (TMM.8-fix's wait)
  CHECK(sameTable(*slotB->table(), *directTable(b.doc(), wb, "", 48000.0)));
  CHECK(link->metaTableStats().previews == before.previews);  // no preview was built over A's table
  // The first blocks after the swap are a fresh link's on B, sample for sample.
  auto fresh = makeLink(b.store.current());
  REQUIRE(fresh->waitForMetaTables());
  Driver da{*link}, db{*fresh};
  da.run(4);  // stopped: the new images become current (an instant swap)
  db.run(4);
  da.out.clear();
  db.out.clear();
  link->play(0);
  fresh->play(0);
  da.run(64);
  db.run(64);
  CHECK(peakOf(db.out, 0, db.out.size()) > 0.05);
  size_t firstDiff = da.out.size();
  for (size_t i = 0; i < da.out.size() && firstDiff == da.out.size(); ++i)
    if (da.out[i] != db.out[i]) firstDiff = i;
  MESSAGE("first differing sample: " << firstDiff << " of " << da.out.size());
  CHECK(da.out == db.out);
}

TEST_CASE("meta wavetable: golden wavetable-morph (saw -> square spectral sweep, crossfade gap, baked warp per mode)") {
  WtSong s;
  constexpr Ticks q = kPpq;  // 0.5 s at 120 BPM
  const DeviceId spectral = s.track({{0, q, 0}});
  const DeviceId cross = s.track({{q, q, 0}});
  s.data(spectral, sawToSquare(dsp::GapMode::Spectral));
  s.data(cross, sawToSquare(dsp::GapMode::Crossfade));
  s.lane(0, spectral, "position", {{0, 0.0f, 0.0f, {}}, {q, 1.0f, 0.0f, {}}});
  s.lane(1, cross, "position", {{q, 0.0f, 0.0f, {}}, {2 * q, 1.0f, 0.0f, {}}});
  for (int mode = 0; mode < 3; ++mode) {
    const DeviceId w = s.track({{2 * q + mode * (q / 2), q / 2, 0}});
    s.data(w, oneCycle(dsp::BasicCycle::Saw));
    s.param(w, "warp", 0.6);
    s.setting(w, "warpMode", mode);
  }
  const DeviceId fs1 = s.track({{2 * q + 3 * (q / 2), q / 2, 0}});
  s.data(fs1, oneCycle(dsp::BasicCycle::Saw));
  s.param(fs1, "fold", 0.4);
  s.param(fs1, "sync", 0.3);
  constexpr double kSeconds = 2.0, kRate = 48000.0;
  auto link = makeLink(s.store.current());
  engine::BufferSink sink;
  REQUIRE(link->renderOffline(0, static_cast<uint64_t>(kSeconds * kRate), sink).ok());
  CHECK(link->lastError().empty());
  const golden::Channels& out = sink.channels;
  REQUIRE(out.size() == 2);
  CHECK(peakOf(out[0], 0, out[0].size()) > 0.2);
  const fs::path wav = golden::goldenDir() / "audio" / "wavetable_morph.wav";
  const char* up = std::getenv("PQ_UPDATE_GOLDENS");
  if (up != nullptr && *up != '\0' && *up != '0') {
    REQUIRE(engine::writeWav(wav, out, static_cast<uint32_t>(kRate)).ok());
    MESSAGE("updated " << wav.string());
  }
  auto gold = engine::readWav(wav);
  REQUIRE_MESSAGE(gold.ok(), (gold.ok() ? std::string() : gold.error().message + " (PQ_UPDATE_GOLDENS=1)"));
  const golden::Comparison cmp = golden::compare(*gold, out, static_cast<uint32_t>(kRate));
  CHECK_MESSAGE(cmp.ok, cmp.message);
  // Each warp mode bakes a different table (the segments differ).
  const size_t seg = static_cast<size_t>(0.25 * kRate), at = static_cast<size_t>(1.0 * kRate) + 2400;
  CHECK(std::fabs(brightness(out[0], at, at + 4800) - brightness(out[0], at + seg, at + seg + 4800)) > 1e-6);
}

namespace {

// The alias suite on the module's output (TMM.1's method, tests/support/alias_meter.h): BasicSynth forked so its voice
// is a nested Wavetable read by a float phase ramp acc = frac(z1(acc) + d), d = k / 2^16 a device param: bin-exact
// pitches k·sr / 2^16 whose phases are exact multiples of 2^-16 at every rate (a phasor(f) would need f within
// 0.5·sr / 2^32 Hz of the bin, which float cannot hold above ~1 kHz at 44.1 kHz). The table: saw → square (spectral), read at position 0.5 (a blend of two dense frames), with each baked
// control at its values. Everything non-harmonic below 20 kHz must be ≤ −100 dBFS.
struct AliasRig {
  DocumentStore store;
  TrackId track;
  DeviceId inst;
  std::unique_ptr<ProjectLink> link;
  ListenerId sub{};
  double rate;
  explicit AliasRig(double sr) : rate(sr) {
    IdSource& ids = store.ids();
    track = ids.track();
    inst = ids.device();
    Batch b{"alias rig", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "WT", inst), {}, kAppend});
    std::vector<Note> notes{playback_fixture::note(ids, 0, 256 * kTicksPerWhole, 0)};
    b.commands.push_back(AddClip{track, playback_fixture::clip(ids, 0, 256 * kTicksPerWhole, notes, "held")});
    REQUIRE(store.dispatch(b).ok());
    link = makeLink(store.current(), sr);
    std::string text = link->devices().stockText("BasicSynth");
    auto swap = [&](const std::string& from, const std::string& to) {
      const size_t at = text.find(from);
      REQUIRE_MESSAGE(at != std::string::npos, from);
      text.replace(at, from.size(), to);
    };
    swap("  out   out  : audio voice mono", "  param d = 0.01 [0..0.5]\n  out   out  : audio voice mono");
    swap("  out = osc * env * vel * level", "  acc = frac(z1(acc) + d)\n  wt1 = meta_wavetable(x: acc)\n  out = wt1");
    sub = store.subscribe([this](const StoreEvent& e) { link->onDocumentChange(e); });
    edit(ops::addMetaCall(*store.current(), inst, text, "wt1", "wavetable"));
    edit(ops::setMetaData(*store.current(), inst, "wt1", "wavetable", sawToSquare()));
    edit(ops::setMetaParam(*store.current(), inst, "wt1", "wavetable", "position", 0.5));
    link->flush();
    REQUIRE(link->lastError().empty());
    REQUIRE(link->waitForMetaTables());
    link->play(0);
  }
  ~AliasRig() { store.unsubscribe(sub); }
  void edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
  }
  void bake(const char* param, double v) {
    edit(ops::setMetaParam(*store.current(), inst, "wt1", "wavetable", param, v));
    REQUIRE(link->waitForMetaTables());
  }
  void warpMode(int m) {
    edit(ops::setMetaSetting(*store.current(), inst, "wt1", "wavetable", "warpMode", m));
    link->flush();  // a setting: one compile (the table follows it)
    REQUIRE(link->waitForMetaTables());
  }
  // N frames of the left channel after the phase increment `inc` (cycles per sample) has played `lead` frames.
  std::vector<float> capture(double inc, uint32_t n, uint32_t lead = 4096) {
    REQUIRE(store.dispatch(SetDeviceParam{inst, "d", inc}).ok());
    std::vector<float> l(kBlock), r(kBlock), out;
    out.reserve(n);
    uint32_t done = 0;
    while (out.size() < n) {
      float* ch[2] = {l.data(), r.data()};
      {
        DenormalGuard dg;
        link->engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      if (done >= lead) out.insert(out.end(), l.begin(), l.end());
      done += kBlock;
    }
    out.resize(n);
    for (float& x : out) x *= static_cast<float>(std::sqrt(2.0));  // the centre pan's −3 dB
    return out;
  }
  int64_t time = 0;
};

struct AliasCase {
  const char* control;  // nullptr: the plain table
  double value;
  int mode;
};

void runAliasSuite(double rate, const std::vector<AliasCase>& cases, uint32_t pitches) {
  constexpr uint32_t kN = 1u << 16;
  AliasRig rig(rate);
  test::AliasMeter meter(kN);
  std::vector<uint32_t> ks;
  for (uint32_t i = 0; i < pitches; ++i) {
    const double f = 20.0 * std::pow(12000.0 / 20.0, pitches > 1 ? double(i) / (pitches - 1) : 0.0);
    const uint32_t k = std::max(1u, static_cast<uint32_t>(std::lround(f * kN / rate)));
    if (ks.empty() || ks.back() != k) ks.push_back(k);
  }
  double worst = -400.0;
  std::string worstWhere;
  int curMode = 0;
  for (const AliasCase& c : cases) {
    if (c.control != nullptr) {
      if (std::string(c.control) == "warp" && c.mode != curMode) {
        rig.warpMode(c.mode);
        curMode = c.mode;
      }
      rig.bake(c.control, c.value);
    }
    for (const uint32_t k : ks) {
      const double f = k * rate / kN;
      const std::vector<float> x = rig.capture(static_cast<double>(k) / kN, kN);
      const test::AliasReport rep = meter.measure(x, k, rate, [](uint32_t) { return -1.0; });
      if (rep.worstAliasDb > worst) {
        worst = rep.worstAliasDb;
        worstWhere = std::string(c.control ? c.control : "plain") + " " + std::to_string(c.value) + " mode " +
                     std::to_string(c.mode) + " at " + std::to_string(f) + " Hz (alias at " +
                     std::to_string(rep.worstAliasHz) + " Hz)";
      }
      CAPTURE(rate);
      CAPTURE(f);
      CAPTURE(c.control ? c.control : "plain");
      CAPTURE(c.value);
      CAPTURE(c.mode);
      CHECK(rep.worstAliasDb <= -100.0);
      // Not silent: the fundamental is there (a baked sync moves the energy up; high notes keep little of it).
      if (c.control == nullptr) CHECK(peakOf(x, 0, x.size()) > 0.5);
    }
    if (c.control != nullptr) rig.bake(c.control, 0.0);
  }
  MESSAGE("alias suite at " << rate << " Hz: " << ks.size() << " pitches x " << cases.size()
                            << " tables, worst alias " << worst << " dBFS (" << worstWhere << ")");
}

std::vector<AliasCase> fullCases() {
  std::vector<AliasCase> c{{nullptr, 0.0, 0}};
  for (const double v : {0.1, 0.3, 0.5, 0.75, 1.0}) c.push_back({"fold", v, 0});
  for (const double v : {0.1, 0.3, 0.5, 0.75, 1.0}) c.push_back({"sync", v, 0});
  for (int m = 0; m < 3; ++m)
    for (const double v : {-1.0, -0.4, 0.2, 0.6, 1.0}) c.push_back({"warp", v, m});
  return c;
}

}  // namespace

TEST_CASE("meta wavetable: the alias suite on the module output (48 kHz, one value per control, 9 pitches)") {
  runAliasSuite(48000.0,
                {{nullptr, 0.0, 0}, {"fold", 0.5, 0}, {"sync", 0.5, 0}, {"warp", 0.6, 0}, {"warp", 0.6, 1}, {"warp", -0.6, 2}},
                9);
}

TEST_CASE("meta wavetable: the alias suite on the module output (44.1 / 48 / 96 kHz, 5 values per control, 25 pitches)" *
          doctest::test_suite("slow")) {
  for (const double rate : {44100.0, 48000.0, 96000.0}) runAliasSuite(rate, fullCases(), 25);
}

TEST_CASE("meta wavetable: a keyframe edit reaches the engine within 30 ms (64 frames), 160 ms with a bake" *
          doctest::test_suite("perf")) {
  WtSong s;
  const DeviceId wt = s.track({{0, 64 * kPpq, 0}});
  auto link = makeLink(s.store.current());
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  REQUIRE(link->waitForMetaTables());
  const auto slot = link->metaFrameTable(wt, "", 0);
  REQUIRE(slot != nullptr);
  auto latency = [&](auto&& edit) {
    const uint64_t gen = slot->generation();
    const auto t0 = std::chrono::steady_clock::now();
    edit();
    while (slot->generation() == gen) std::this_thread::yield();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  };
  double plain = 1e9, baked = 1e9;
  for (int i = 0; i < 3; ++i) {  // best of 3 (fresh data each time: no cache hit)
    plain = std::min(plain, latency([&] { s.data(wt, sawToSquare(i % 2 ? dsp::GapMode::Crossfade : dsp::GapMode::Spectral)); }));
    baked = std::min(baked, latency([&] { s.param(wt, "fold", 0.2 + 0.1 * i); }));
  }
  MESSAGE("keyframe edit -> audible table: 64 frames " << plain << " ms, with a baked fold " << baked << " ms");
#if PQ_OPTIMIZED
  CHECK(plain <= 30.0);
  CHECK(baked <= 160.0);
#endif
}

TEST_CASE("meta wavetable: a baked drag is heard at >= 30 Hz through preview tables; the full table when it stops; "
          "a drawn stroke is audible within 30 ms" * doctest::test_suite("perf")) {  // TMM.9 (Q173 9): wall-clock
  using Clock = std::chrono::steady_clock;
  WtSong s;
  const DeviceId wt = s.track({{0, 64 * kPpq, 0}});
  s.data(wt, sawToSquare());
  auto link = makeLink(s.store.current());
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  REQUIRE(link->waitForMetaTables());
  const auto slot = link->metaFrameTable(wt, "", 0);
  REQUIRE(slot != nullptr);
  // A fold drag: 30 steps at 60 Hz (one gesture). Every table publish is timed.
  std::vector<double> latency;  // per step: its edit → the next publish
  uint32_t publishes = 0;
  const auto t0 = Clock::now();
  REQUIRE(s.store.beginGesture("fold").ok());
  for (int i = 1; i <= 30; ++i) {
    const uint64_t gen = slot->generation();
    const auto at = Clock::now();
    s.param(wt, "fold", 0.02 * i);
    bool heard = false;
    const auto until = t0 + std::chrono::microseconds(16667 * i);
    while (Clock::now() < until || (i == 30 && !heard && Clock::now() < until + std::chrono::milliseconds(200))) {
      if (slot->generation() != gen && !heard) {
        heard = true;
        latency.push_back(std::chrono::duration<double, std::milli>(Clock::now() - at).count());
      }
      std::this_thread::yield();
    }
    publishes += slot->generation() != gen ? 1u : 0u;
  }
  const double dragS = std::chrono::duration<double>(Clock::now() - t0).count();
  const auto previewFrames = slot->table()->frames();
  REQUIRE(s.store.commitGesture().ok());
  REQUIRE(link->waitForMetaTables());
  // The drag's end: the full table (64 frames), bit for bit a direct build.
  CHECK(slot->table()->frames() == meta::kWtDefaultFrames);
  CHECK(sameTable(*slot->table(), *directTable(s.doc(), wt, "", 48000.0)));
  std::sort(latency.begin(), latency.end());
  REQUIRE(!latency.empty());
  const meta::TableBuilder::Stats st = link->metaTableStats();
  MESSAGE("baked fold drag, 30 steps at 60 Hz: " << latency.size() << " steps heard, " << publishes << " publishes in "
                                                 << dragS << " s (" << publishes / dragS << " Hz); latency median "
                                                 << latency[latency.size() / 2] << " ms, max " << latency.back()
                                                 << " ms; preview " << st.lastPreviewMs << " ms (max " << st.maxPreviewMs
                                                 << "), full " << st.lastBuildMs << " ms; tables during the drag: "
                                                 << previewFrames << " frames; submitted " << st.submitted
                                                 << ", previews " << st.previews << " (" << st.focused << " focused), replaced "
                                                 << st.replaced << ", built " << st.built);
  // TMM.16: the previews are focused (the full table's frames, the ones read rebuilt): as many frames as the table.
  CHECK(previewFrames == meta::kWtDefaultFrames);
  CHECK(st.focused > 0u);
  // A drawn stroke on that baked table (a keyframe's cycle changed, as the face writes it): the preview is heard.
  auto stroke = [&](float level) {
    auto d = meta::parseWavetableData(*sawToSquare());
    REQUIRE(d.ok());
    for (uint32_t i = 200; i < 900; ++i) d->keyframes[0].cycle[i] = level;
    const uint64_t gen = slot->generation();
    const auto at = Clock::now();
    s.data(wt, meta::wavetableData(*d));
    while (slot->generation() == gen) std::this_thread::yield();
    return std::chrono::duration<double, std::milli>(Clock::now() - at).count();
  };
  double baked = 1e9;
  for (int i = 0; i < 3; ++i) baked = std::min(baked, stroke(0.2f + 0.1f * static_cast<float>(i)));
  REQUIRE(link->waitForMetaTables());
  s.param(wt, "fold", 0.0);
  REQUIRE(link->waitForMetaTables());
  double plain = 1e9;
  for (int i = 0; i < 3; ++i) plain = std::min(plain, stroke(-0.2f - 0.1f * static_cast<float>(i)));
  MESSAGE("drawn stroke -> audible table: unbaked 64 frames " << plain << " ms, baked (fold 0.6, preview) " << baked << " ms");
#if PQ_OPTIMIZED
  CHECK(latency[latency.size() / 2] <= 33.0);  // ≥ 30 Hz
  CHECK(publishes / dragS >= 30.0);
  CHECK(plain <= 30.0);
  CHECK(baked <= 30.0);
#endif
}

// ---- The TMM.2 / TMM.4 join: faces read the real voice taps ------------------------------------------------------

TEST_CASE("meta faces: a played note is a dot — the face reads the engine's voice tap through the app's hub") {
  WtSong s;
  // A chain Phase accumulator playing a three-note chord, and a chain Wavetable playing one note.
  auto t = ops::createTrack(s.doc(), s.store.ids(), TrackKind::Midi);
  REQUIRE(t.ok());
  s.must(t->command);
  const TrackId accTrack = t->tracks.at(0);
  auto a = ops::addMetaDevice(s.doc(), s.store.ids(), accTrack, "phase_acc", 0);
  REQUIRE(a.ok());
  s.must(a->command);
  const DeviceId acc = a->devices.at(0);
  std::vector<Note> chord;
  for (int32_t deg : {0, 4, 7}) chord.push_back(playback_fixture::note(s.store.ids(), 0, 64 * kPpq, deg));
  s.must(AddClip{accTrack, playback_fixture::clip(s.store.ids(), 0, 64 * kPpq, chord, "chord")});
  const DeviceId wt = s.track({{0, 64 * kPpq, 0}});
  auto link = makeLink(s.store.current());
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  REQUIRE(link->waitForMetaTables());
  app::FaceVoiceHub hub(*link);
  views::VoiceTapReader* accVoices = hub.voices(acc, "");
  views::VoiceTapReader* wtVoices = hub.voices(wt, "");
  CHECK(hub.voices(acc, "") == accVoices);  // stable
  // A face bound to the reader, as the project patch view binds it.
  std::unique_ptr<views::MetaFace> face = views::makeFace(meta::phaseAccType());
  REQUIRE(face != nullptr);
  face->bind(nullptr, accVoices);
  CHECK_FALSE(accVoices->live());  // silent: the host's timer does not run
  CHECK_FALSE(hub.anyFresh());
  CHECK(face->sounding().empty());
  Driver d{*link};
  link->play(0);
  d.run(20);
  CHECK(hub.anyFresh());  // records arrived: the host wakes its faces
  CHECK(accVoices->live());
  CHECK(face->pollVoices());
  REQUIRE(face->sounding().size() == 3);  // three dots
  const auto tap = link->metaVoiceTap(acc, "", 0);
  REQUIRE(tap != nullptr);
  // The dots are the phases the voices read: each in [0, 1), each voice's samples rising (a ramp, wrapping).
  for (const views::FaceVoice& v : face->sounding()) {
    CHECK(v.phase >= 0.0f);
    CHECK(v.phase < 1.0f);
    CHECK(v.sampleCount > 0);
    CHECK(v.sampleCount <= views::FaceVoice::kMaxSamples);
  }
  // The same voice slots as the engine's reader, at the same newest phase.
  VoiceTapReader check(tap);
  d.run(2);
  (void)face->pollVoices();
  (void)check.poll(0.0);
  for (const views::FaceVoice& v : face->sounding()) {
    CHECK(check.voice(v.slot).active);
    CHECK(check.voice(v.slot).value == v.phase);
  }
  CHECK_FALSE(static_cast<views::TapVoices*>(accVoices)->fresh());  // read: nothing new until the next block
  // The Wavetable's dots come from its table slot's tap (wavetable_pos writes the phase it read).
  std::array<views::FaceVoice, 8> buf{};
  d.run(4);
  CHECK(wtVoices->read(buf) == 1);
  CHECK(buf[0].phase >= 0.0f);
  CHECK(buf[0].phase < 1.0f);
  // Stopped: the voices end, the readers fall silent (the face timer stops).
  link->stop();
  d.run(static_cast<uint32_t>(0.3 * 48000 / kBlock));
  // The first read after the stop takes the last records; once they are older than the staleness window (0.1 s) the
  // reader's serial moves and the next read finds no voice.
  (void)face->pollVoices();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  (void)face->pollVoices();
  CHECK(face->sounding().empty());
  CHECK_FALSE(accVoices->live());
}

TEST_CASE("meta faces: a face bound before its instance compiled follows the tap that appears later") {
  WtSong s;
  auto t = ops::createTrack(s.doc(), s.store.ids(), TrackKind::Midi);
  REQUIRE(t.ok());
  s.must(t->command);
  const TrackId track = t->tracks.at(0);
  std::vector<Note> one{playback_fixture::note(s.store.ids(), 0, 64 * kPpq, 0)};
  s.must(AddClip{track, playback_fixture::clip(s.store.ids(), 0, 64 * kPpq, one, "one")});
  auto link = makeLink(s.store.current());
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  app::FaceVoiceHub hub(*link);
  auto a = ops::addMetaDevice(s.doc(), s.store.ids(), track, "phase_acc", 0);
  REQUIRE(a.ok());
  const DeviceId acc = a->devices.at(0);
  views::VoiceTapReader* r = hub.voices(acc, "");  // no such instance yet: no tap
  CHECK_FALSE(r->live());
  s.must(a->command);
  link->flush();
  (void)r->serial();  // the host's next poll finds the new tap and registers with it
  Driver d{*link};
  link->play(0);
  d.run(20);
  CHECK(r->live());
  std::array<views::FaceVoice, 8> buf{};
  CHECK(r->read(buf) == 1);
}

// ---- TMM.7: the Wavetable face over the engine's taps --------------------------------------------------------------

namespace {

// A chain Wavetable's state read from the document (the face's values and data), read-only.
class SongTarget final : public views::MetaTarget {
public:
  SongTarget(const DocumentStore& s, DeviceId d) : store_(s), dev_(d) {}
  double value(size_t p) const override {
    return metaParamValue(*findDevice(*store_.current(), dev_).device, "", meta::wavetableType(), p);
  }
  bool editable() const noexcept override { return false; }
  void begin(const char*) override {}
  void set(size_t, double) override {}
  void end() override {}
  void cancel() override {}
  void reset(size_t) override {}
  double setting(size_t i) const override {
    return metaSettings(*findDevice(*store_.current(), dev_).device, "", meta::wavetableType())[i];
  }
  meta::DataPtr data() const override { return findDevice(*store_.current(), dev_).device->metaAt("")->data; }
  const meta::Type* targetType() const noexcept override { return &meta::wavetableType(); }

private:
  const DocumentStore& store_;
  DeviceId dev_;
};

}  // namespace

TEST_CASE("meta faces: the hub's reader knows the Wavetable's table — a publish is news (the face repaints), heardTable is the slot's") {
  // TMM.16: the face draws the published table; a table published while nothing sounds must still reach it.
  WtSong s;
  const DeviceId wt = s.track({{0, 64 * kPpq, 0}});
  s.data(wt, sawToSquare());
  auto link = makeLink(s.store.current());
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  REQUIRE(link->waitForMetaTables());
  app::FaceVoiceHub hub(*link);
  auto* r = static_cast<views::TapVoices*>(hub.voices(wt, ""));
  const auto slot = link->metaFrameTable(wt, "", 0);
  REQUIRE(slot != nullptr);
  std::unique_ptr<views::MetaFace> face = views::makeFace(meta::wavetableType());
  face->bind(std::make_unique<SongTarget>(s.store, wt), r);
  (void)face->pollVoices();
  CHECK(r->heardTable().table == slot->table());
  CHECK(r->heardTable().modulated.size() == meta::wavetableType().params.size());
  CHECK_FALSE(r->fresh());
  const uint64_t serial = r->serial();
  CHECK_FALSE(face->pollVoices());  // nothing new
  s.data(wt, oneCycle(dsp::BasicCycle::Square));  // a new table, no voice sounding
  REQUIRE(link->waitForMetaTables());
  CHECK(r->fresh());           // the shell wakes its faces
  CHECK(r->live());            // the host keeps polling (kTableLinger)
  CHECK(r->serial() != serial);
  CHECK(face->pollVoices());   // ... and the face repaints with the new table
  CHECK(r->heardTable().table == slot->table());
  CHECK_FALSE(r->fresh());
  CHECK(r->live());            // still within the linger: a full table may follow a preview
  // A Phase accumulator's reader has no table.
  auto t = ops::createTrack(s.doc(), s.store.ids(), TrackKind::Midi);
  REQUIRE(t.ok());
  s.must(t->command);
  auto a = ops::addMetaDevice(s.doc(), s.store.ids(), t->tracks.at(0), "phase_acc", 0);
  REQUIRE(a.ok());
  s.must(a->command);
  link->flush();
  CHECK(hub.voices(a->devices.at(0), "")->heardTable().table == nullptr);
}

TEST_CASE("meta faces: golden meta-wavetable-voices — eight engine voices, each dot at the phase its voice read") {
  WtSong s;
  // An eight-note chord on a chain Wavetable (saw → square → vocal, position 0.3); NullDevice-style blocks.
  std::vector<std::array<Ticks, 3>> notes;
  for (const int deg : {0, 3, 7, 10, 12, 15, 19, 22}) notes.push_back({0, 64 * kPpq, static_cast<Ticks>(deg)});
  const DeviceId wt = s.track(notes);
  meta::WavetableData d;
  d.keyframes.push_back({0.0f, cycleOf(dsp::BasicCycle::Saw)});
  d.keyframes.push_back({0.5f, cycleOf(dsp::BasicCycle::Square)});
  d.keyframes.push_back({1.0f, cycleOf(dsp::BasicCycle::Triangle)});
  s.data(wt, meta::wavetableData(d));
  s.param(wt, "position", 0.3);
  auto link = makeLink(s.store.current());
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  REQUIRE(link->waitForMetaTables());
  app::FaceVoiceHub hub(*link);
  views::VoiceTapReader* voices = hub.voices(wt, "");
  (void)voices->serial();
  const auto tap = link->metaVoiceTap(wt, "", 0);
  REQUIRE(tap != nullptr);
  VoiceTapReader check(tap);  // the engine-side reader the dots must agree with
  Driver drv{*link};
  link->play(0);
  drv.run(40);
  // The face appears now (voices already sounding: shown at once) and reads the tap.
  std::unique_ptr<views::MetaFace> face = views::makeFace(meta::wavetableType());
  REQUIRE(face != nullptr);
  face->bind(std::make_unique<SongTarget>(s.store, wt), voices);
  REQUIRE(face->sounding().size() == 8u);
  // Each dot is the newest phase its voice read, at the position played.
  (void)check.poll(0.0);
  for (const views::FaceVoice& v : face->sounding()) {
    CHECK(check.voice(v.slot).active);
    CHECK(v.phase == check.voice(v.slot).value - std::floor(check.voice(v.slot).value));
    CHECK(v.position == doctest::Approx(0.3));
    CHECK(v.sampleCount > 0u);
  }
  // One displayed frame later (1/60 s of blocks): each voice brings the phases it read in that frame (the glow).
  drv.run(13);
  CHECK(face->pollVoices());
  REQUIRE(face->sounding().size() == 8u);
  for (const views::FaceVoice& v : face->sounding()) CHECK(v.sampleCount >= 13u * kVoiceTapSamples - 8u);
  // Rendered offscreen at 2x, zoom 1.5, as the canvas paints a face (static layers directly, jacks, live layer).
  ui::GpuOptions o;
  o.validation = true;
  auto g = ui::GpuContext::create(o);
  REQUIRE_MESSAGE(g.ok(), (g.ok() ? "" : g.error().message));
  auto r = ui::Renderer::create(**g, ui::OffscreenTarget::kFormat, 1);
  REQUIRE(r.ok());
  auto tex = ui::createSceneTextures(**r, ui::UiScale(2.0f));
  REQUIRE(tex.ok());
  const ui::PaintResources res = ui::paintResources(*tex);
  constexpr float kScale = 3.0f;  // zoom 1.5 at 2x
  const auto w = static_cast<uint32_t>(std::ceil(204.0f * kScale + 32.0f)), h = static_cast<uint32_t>(std::ceil(248.0f * kScale + 32.0f));
  ui::DrawList dl;
  dl.reset(w, h);
  dl.addRectFilled({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)}, res.theme->circuit.background);
  views::FaceJack jack{{204.0f, 125.0f}, false, true, true, "out"};
  views::FaceEnv env;
  env.theme = res.theme;
  env.uiScale = 2.0f;
  env.jacks = std::span<const views::FaceJack>(&jack, 1);
  ui::FacePainter p(dl, {{16.0f, 16.0f}, kScale});
  p.setText(res.text);  // TMM.13-fix2 (MM review M9): with the text system, as a canvas paints (the "os" cell was a bar)
  views::paintFace(*face, p, env);
  auto target = ui::OffscreenTarget::create(**g, w, h, 1);
  REQUIRE(target.ok());
  REQUIRE((*target)->draw(**r, dl, ui::kSceneClearColor).ok());
  auto img = (*target)->read();
  REQUIRE(img.ok());
  auto ref = ui::readPng(std::string(PQ_TEST_DATA_DIR) + "/views/golden/meta-wavetable-voices.png");
  if (!ref) {
    (void)ui::writePng("meta-wavetable-voices.actual.png", *img);
    FAIL_CHECK("no golden meta-wavetable-voices: wrote meta-wavetable-voices.actual.png");
    return;
  }
  ui::ImageDiff diff;
  CHECK(pq::ui::test::matchesGolden("meta-wavetable-voices", *img, *ref, true, &diff));
  MESSAGE("meta-wavetable-voices: " << pq::ui::test::describe(diff));
}

// ---- TMM.16: the picture always matches the sound -------------------------------------------------------------------

namespace {

// The face edits the song's document as DocTarget edits the app's (one undo step per gesture; data inside a gesture
// joins it), so the link builds tables exactly as it does under the real face.
class StoreTarget final : public views::MetaTarget {
public:
  StoreTarget(WtSong& s, DeviceId d) : s_(s), dev_(d) {}
  ~StoreTarget() override {
    if (gesture_) (void)s_.store.commitGesture();
  }
  const meta::Type* targetType() const noexcept override { return &meta::wavetableType(); }
  double value(size_t p) const override { return metaParamValue(dev(), "", meta::wavetableType(), p); }
  void begin(const char* label) override {
    if (!gesture_) gesture_ = s_.store.beginGesture(label).ok();
  }
  void set(size_t p, double v) override { s_.param(dev_, std::string(meta::wavetableType().params[p].name).c_str(), v); }
  void end() override {
    if (gesture_) (void)s_.store.commitGesture();
    gesture_ = false;
  }
  void cancel() override {
    if (gesture_) (void)s_.store.cancelGesture();
    gesture_ = false;
  }
  void reset(size_t p) override {
    s_.edit(ops::setMetaParam(s_.doc(), dev_, "", "wavetable", std::string(meta::wavetableType().params[p].name),
                              std::nullopt));
  }
  double setting(size_t i) const override { return metaSettings(dev(), "", meta::wavetableType())[i]; }
  void setSetting(size_t i, double v) override {
    s_.setting(dev_, std::string(meta::wavetableType().settings[i].name).c_str(), v);
  }
  meta::DataPtr data() const override {
    const MetaInstance* inst = dev().metaAt("");
    return inst != nullptr && inst->data ? inst->data : meta::wavetableDefaultData();
  }
  void setData(meta::DataPtr d) override { s_.data(dev_, d); }

private:
  const Device& dev() const { return *findDevice(s_.doc(), dev_).device; }
  WtSong& s_;
  DeviceId dev_;
  bool gesture_ = false;
};

// Face-local geometry (wavetable_face.cpp's layout, world units).
ui::Vec2 toolAt(int i) { return {57.5f + 14.5f * static_cast<float>(i), 24.0f}; }
ui::Vec2 modeAt(int i) { return {21.0f + 15.5f * static_cast<float>(i), 24.0f}; }
ui::Vec2 shapeAt(int i) {
  return {188.0f - 13.0f * static_cast<float>(6 - i) - 1.5f * static_cast<float>(5 - i) + 6.5f, 24.0f};
}
ui::Vec2 warpModeAt(int i) { return {74.5f + 14.5f * static_cast<float>(i), 233.0f}; }
ui::Vec2 optAt(int i) { return {25.0f + 13.5f * static_cast<float>(i), 85.5f}; }
ui::Vec2 drawAt(float x, float v) { return {20.0f + 138.0f * x, 129.0f - 35.0f * v}; }
ui::Vec2 barAt(uint32_t h, float amp) { return {16.0f + 5.375f * (static_cast<float>(h) - 0.5f), 57.5f - 21.0f * amp}; }
ui::Vec2 phaseBarAt(uint32_t h, float ph) {
  return {16.0f + 5.375f * (static_cast<float>(h) - 0.5f), 66.0f - ph / std::numbers::pi_v<float> * 5.0f};
}
float wtPosY(float u) { return 172.0f - 94.0f * u; }
constexpr float kWtKnobX[3] = {38.0f, 89.0f, 140.0f};
constexpr float kPx = 4.0f;  // device px per world unit (zoom 2 at 2x)

}  // namespace

namespace {

// sweep: every gesture (slow tier); else the reported ones — the line and the curve in draw mode, a harmonic drag and
// a baked knob drag — in the default tier.
void pictureMatchesSound(bool sweep) {
  // TMM.16 (bug reports 2026-11-01_12-12-09, 12-13-10). At every step of every gesture, with the face bound as the app
  // binds it (the hub's reader, which knows the instance's slot):
  //   1. the face draws from the table the slot plays now (draw mode's curve, the stack's highlighted frame, the
  //      thumbnails behind), at the level it says (level 0 while nothing sounds), with the live path's controls
  //      applied on top: its curve equals that table read through the readers' interpolator (≤ 1e-5);
  //   2. that table — the focused preview while the mouse is held, the full table after — equals a direct build of the
  //      document's state at that step where the face says it is heard (the position, or the edited keyframe's own
  //      position), at level 0 and at the shown level (≤ 1e-5): the audio follows every step, not only the release.
  WtSong s;
  const DeviceId wt = s.track({{0, 64 * kPpq, 0}});
  meta::WavetableData d;  // the reports' setup: several keyframes, a drawn one, a baked fold and a Mirror warp
  const dsp::BasicCycle shapes[5] = {dsp::BasicCycle::Saw, dsp::BasicCycle::Square, dsp::BasicCycle::Triangle,
                                     dsp::BasicCycle::Pulse25, dsp::BasicCycle::Sine};
  for (int i = 0; i < 5; ++i) d.keyframes.push_back({0.25f * static_cast<float>(i), cycleOf(shapes[i])});
  for (uint32_t i = 0; i < 2048; ++i)
    d.keyframes[2].cycle[i] = (i % 512 < 180 ? 0.7f : -0.3f) + 0.2f * std::sin(0.01f * static_cast<float>(i));
  d.frames = 16;  // (a bake of 16 dense frames builds in tens of ms in the check preset; previews still apply)
  s.data(wt, meta::wavetableData(d));
  s.param(wt, "position", 0.6);
  s.param(wt, "fold", 0.55);
  s.param(wt, "warp", -0.45);
  s.setting(wt, "warpMode", 1.0);
  auto link = makeLink(s.store.current());
  [[maybe_unused]] const ListenerId sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  REQUIRE(link->lastError().empty());
  REQUIRE(link->waitForMetaTables());

  app::FaceVoiceHub hub(*link);
  Driver audio{*link};  // the audio thread takes each recompiled image (a compile waits for it otherwise)
  std::unique_ptr<views::MetaFace> face = views::makeFace(meta::wavetableType());
  REQUIRE(face != nullptr);
  face->bind(std::make_unique<StoreTarget>(s, wt), hub.voices(wt, ""));
  auto theme = ui::loadTheme(ui::defaultThemePath());
  REQUIRE(theme.ok());
  views::FaceEnv env;
  env.theme = &theme.value();
  env.uiScale = 2.0f;
  auto paint = [&] {
    ui::DrawList dl;
    dl.reset(1000, 1100);
    ui::FacePainter p(dl, {{0.0f, 0.0f}, kPx});
    (void)face->pollVoices();  // as the host does before painting
    face->paintLive(p, env);
  };
  using K = views::FacePointer::Kind;
  auto pointer = [&](K kind, ui::Vec2 l) {
    views::FacePointer e;
    e.kind = kind;
    e.local = l;
    e.device = {l.x * kPx, l.y * kPx};
    e.pxPerUnit = kPx;
    e.uiScale = 2.0f;
    (void)face->onPointer(e);
  };
  // The job the link builds for the document's state now (with the params its live path applies).
  auto stateJob = [&] {
    const Device& dv = *findDevice(s.doc(), wt).device;
    const meta::Type& t = meta::wavetableType();
    const MetaInstance* inst = dv.metaAt("");
    meta::TableSource src;
    src.data = inst != nullptr && inst->data ? inst->data : meta::wavetableDefaultData();
    src.dataHash = meta::dataHash(*src.data);
    std::vector<double> params(t.params.size());
    for (size_t i = 0; i < params.size(); ++i) params[i] = metaParamValue(dv, "", t, i);
    const std::vector<double> settings = metaSettings(dv, "", t);
    std::vector<uint8_t> modulated;
    (void)link->metaFrameTable(wt, "", 0, modulated);
    src.params = params;
    src.settings = settings;
    src.sampleRate = 48000.0;
    src.modulated = modulated;
    meta::TableJob job;
    REQUIRE(t.frameTable(src, 0, job).ok());
    return job;
  };
  struct Worst {
    double face = 0.0, state = 0.0, thumb = 0.0;
    std::string where;
  } worst;
  double liveFold = -1.0;  // ≥ 0: fold runs on the live path at this value (the table does not bake it)
  uint32_t checks = 0, previewChecks = 0;
  uint64_t lastKey = 0;
  float lastU = -1.0f;
  dsp::FrameTableBuilder direct;
  std::vector<double> paintNew, paintSame;  // ms: a paint that takes a new table (reads its frames), one that does not
  const dsp::FrameTable* lastPainted = nullptr;
  auto median = [](std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  auto check = [&](const std::string& what, bool full) {
    CAPTURE(what);
    const auto slot = link->metaFrameTable(wt, "", 0);
    REQUIRE(slot != nullptr);
    views::WavetableShown sh;
    std::shared_ptr<const dsp::FrameTable> table;
    for (int attempt = 0; attempt < 5; ++attempt) {  // a newer table published while checking: look again
      table = slot->table();
      const bool fresh = table.get() != lastPainted;
      const auto t0 = std::chrono::steady_clock::now();
      paint();
      (fresh ? paintNew : paintSame).push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      lastPainted = table.get();
      REQUIRE(views::wavetableShown(*face, sh));
      if (sh.table == table.get()) break;
    }
    CAPTURE(sh.position);
    CAPTURE(sh.drawMode);
    REQUIRE(sh.heard);
    CHECK(sh.table == table.get());  // 1. the face holds the table the slot plays
    const auto n = static_cast<uint32_t>(sh.values.size() - 1);
    REQUIRE(n == 1024u);
    auto heard = [&](const dsp::FrameTable& t, double phase, double u, uint32_t level) {
      const double v = dsp::readFrameTableReference(t.view(), phase, u, level);
      return liveFold >= 0.0 ? dsp::foldValue(v, liveFold) : v;
    };
    // The face shows the heard harmonics with Lanczos σ weights (no Gibbs ripples in the picture): its curve's
    // harmonics 0 … 32 are the table's (from 2048 reads at its level) times σ_h, K = min(H_level, 511).
    auto harmonic = [](const auto& x, uint32_t count, uint32_t h) {
      std::complex<double> c{};
      for (uint32_t i = 0; i < count; ++i) c += static_cast<double>(x[i]) * std::polar(1.0, -2.0 * std::numbers::pi * h * i / count);
      return c / static_cast<double>(count);
    };
    auto sigma = [](uint32_t h, uint32_t k) {
      if (h == 0) return 1.0;
      if (h > k) return 0.0;
      const double a = std::numbers::pi * h / (k + 1);
      return std::sin(a) / a;
    };
    auto tableHarmonics = [&](double u, uint32_t level) {
      std::vector<double> x(2048);
      for (uint32_t i = 0; i < 2048; ++i) x[i] = heard(*table, i / 2048.0, u, level);
      return x;
    };
    const auto& tl0 = table->layout();
    double eFace = 0.0;
    {
      const std::vector<double> x = tableHarmonics(sh.position, sh.level);
      const uint32_t k = std::min<uint32_t>(tl0.top[sh.level], 511);
      for (uint32_t h = 0; h <= 32; ++h)
        eFace = std::max(eFace, std::abs(harmonic(sh.values, n, h) - harmonic(x, 2048, h) * sigma(h, k)));
    }
    // ... and its thumbnails are that table's dense frames (harmonics 0 … 8 at the shown level, σ up to 127; read at
    // 512 phases, so a level's harmonics near 512 fold back a little)
    double eThumb = 0.0;
    if (sh.stack.size() == table->frames()) {
      const uint32_t k = std::min<uint32_t>(tl0.top[sh.level], 127);
      for (size_t f = 0; f < sh.stack.size(); f += 3) {
        const double u = sh.stack.size() > 1 ? static_cast<double>(f) / static_cast<double>(sh.stack.size() - 1) : 0.0;
        const std::vector<double> x = tableHarmonics(u, sh.level);
        const auto m = static_cast<uint32_t>(sh.stack[f].size() - 1);
        for (uint32_t h = 0; h <= 8; ++h)
          eThumb = std::max(eThumb, std::abs(harmonic(sh.stack[f], m, h) - harmonic(x, 2048, h) * sigma(h, k)));
      }
    } else {
      CHECK(sh.stack.size() == table->frames());
    }
    CHECK(eThumb <= 2e-2);  // ≤ 2 % of full scale (a baked sync / pulse width puts strong harmonics near 512)
    worst.thumb = std::max(worst.thumb, eThumb);
    CHECK(eFace <= 1e-5);
    // 2. the table is the document's state where the face says it is heard: a direct build of the frames read there.
    meta::TableJob job = stateJob();
    double eState = 0.0;
    if (job.key != lastKey || sh.position != lastU || full) {
      lastKey = job.key;
      lastU = sh.position;
      const uint32_t frames = job.spec.denseFrames;
      CHECK(table->frames() == frames);
      auto layout = dsp::frameTableLayout(48000.0);
      REQUIRE(layout.ok());
      auto t = dsp::FrameTable::allocate(layout.value(), frames, job.spec.antiderivative);
      REQUIRE(t.ok());
      const float x = std::clamp(sh.position, 0.0f, 1.0f) * static_cast<float>(frames - 1);
      const uint32_t k = frames > 1 ? std::min(static_cast<uint32_t>(x), frames - 2) : 0u;
      std::vector<uint32_t> pair{k};
      if (frames > 1) pair.push_back(k + 1);
      REQUIRE(direct.buildFramesInto(job.spec, t.value(), pair).ok());
      for (const uint32_t level : {0u, sh.level})
        for (uint32_t i = 0; i < n; i += 3) {
          const double ph = static_cast<double>(i) / n;
          eState = std::max(eState, std::fabs(heard(*table, ph, sh.position, level) - heard(t.value(), ph, sh.position, level)));
        }
      CHECK(eState <= 1e-5);
    }
    ++checks;
    if (!full) ++previewChecks;
    if (std::max(eFace, eState) > std::max(worst.face, worst.state)) worst.where = what;
    worst.face = std::max(worst.face, eFace);
    worst.state = std::max(worst.state, eState);
  };
  // Whether the slot's table now is the document's state where the face shows it (the frames read there, built
  // directly).
  auto isState = [&] {
    const auto slot = link->metaFrameTable(wt, "", 0);
    paint();
    views::WavetableShown sh;
    if (!views::wavetableShown(*face, sh)) return false;
    const auto table = slot->table();
    meta::TableJob job = stateJob();
    const uint32_t frames = job.spec.denseFrames;
    if (table->frames() != frames) return false;
    auto layout = dsp::frameTableLayout(48000.0);
    auto t = dsp::FrameTable::allocate(layout.value(), frames, job.spec.antiderivative);
    const float x = std::clamp(sh.position, 0.0f, 1.0f) * static_cast<float>(frames - 1);
    const uint32_t k = frames > 1 ? std::min(static_cast<uint32_t>(x), frames - 2) : 0u;
    std::vector<uint32_t> pair{k};
    if (frames > 1) pair.push_back(k + 1);
    if (!direct.buildFramesInto(job.spec, t.value(), pair).ok()) return false;
    for (uint32_t i = 0; i < 1024; i += 5) {
      const double ph = i / 1024.0;
      const double a = dsp::readFrameTableReference(table->view(), ph, sh.position, 0);
      const double b = dsp::readFrameTableReference(t.value().view(), ph, sh.position, 0);
      if (std::fabs(a - b) > 1e-5) return false;
    }
    return true;
  };
  // One gesture step: the event, then (when it changed what the table is built from) the tables the link publishes
  // — previews while a drag runs; one for the previous step may still arrive first — until one is this step's state,
  // then the check. A step whose state never reaches the audio side times out.
  auto step = [&](const std::string& what, auto&& event) {
    const auto slot = link->metaFrameTable(wt, "", 0);
    REQUIRE(slot != nullptr);
    uint64_t gen = slot->generation();
    const uint64_t key = stateJob().key;
    event();
    if (stateJob().key != key) {
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      bool reached = false;
      while (!reached && std::chrono::steady_clock::now() < until) {
        if (slot->generation() != gen) {
          gen = slot->generation();
          reached = isState();
        } else {
          std::this_thread::yield();
        }
      }
      CAPTURE(what);
      REQUIRE(reached);
    }
    check(what, false);
  };
  auto settled = [&](const std::string& what) {
    REQUIRE(link->waitForMetaTables());
    check(what + " (full table)", true);
  };
  auto drag = [&](const std::string& what, const std::vector<ui::Vec2>& path) {
    step(what + " down", [&] { pointer(K::Down, path.front()); });
    for (size_t i = 1; i < path.size(); ++i) step(what + " move " + std::to_string(i), [&] { pointer(K::Move, path[i]); });
    step(what + " up", [&] { pointer(K::Up, path.back()); });
    settled(what);
  };
  auto click = [&](const std::string& what, ui::Vec2 at) {
    step(what, [&] {
      pointer(K::Down, at);
      pointer(K::Up, at);
    });
    settled(what);
  };
  auto settingClick = [&](const std::string& what, ui::Vec2 at) {
    pointer(K::Down, at);
    pointer(K::Up, at);
    link->flush();
    REQUIRE(link->lastError().empty());
    audio.run(4);
    settled(what);
  };

  check("stack at rest", true);
  if (sweep) {
    // Draw mode on the selected keyframe (0, at position 0: away from the position): the pencil.
    click("pencil tool", toolAt(0));
    drag("pencil", {drawAt(0.1f, 0.2f), drawAt(0.2f, 0.9f), drawAt(0.3f, -0.4f), drawAt(0.45f, 0.6f), drawAt(0.6f, -0.9f)});
    click("leave draw mode", toolAt(0));
  }
  // A position drag: the stack's highlighted frame follows, and from now on edits follow the position (0.6 → ≈ 0.4:
  // between keyframes, so the first stroke makes a keyframe there).
  drag("position", {{178.0f, wtPosY(0.6f)}, {178.0f, wtPosY(0.55f)}, {178.0f, wtPosY(0.47f)}, {178.0f, wtPosY(0.4f)}});
  // The line (the reported gesture) and the smooth curve, in draw mode at the position.
  click("line tool", toolAt(1));
  drag("line", {drawAt(0.15f, -0.8f), drawAt(0.3f, -0.2f), drawAt(0.5f, 0.5f), drawAt(0.7f, 0.9f), drawAt(0.85f, 0.1f)});
  click("curve tool", toolAt(2));
  drag("curve", {drawAt(0.05f, 0.0f), drawAt(0.2f, 0.7f), drawAt(0.35f, 0.1f), drawAt(0.5f, -0.6f), drawAt(0.7f, 0.4f),
                 drawAt(0.9f, -0.2f)});
  if (sweep) {
    // Normalise and remove DC (the table's flags), from draw mode's corner.
    click("normalise", optAt(1));
    click("remove DC", optAt(2));
  }
  click("leave draw mode", toolAt(2));
  // The harmonic rows: amplitudes painted across bars, then phases.
  drag("harmonic amplitudes", {barAt(2, 0.6f), barAt(3, 0.3f), barAt(5, 0.8f), barAt(7, 0.1f)});
  if (!sweep) {
    drag("fold knob", {{kWtKnobX[0], 212.0f}, {kWtKnobX[0], 208.0f}, {kWtKnobX[0], 203.0f}});
    const meta::TableBuilder::Stats st = link->metaTableStats();
    MESSAGE(checks << " checks (" << previewChecks << " during gestures); face vs its table " << worst.face
                   << " (thumbnails " << worst.thumb << "), table vs the state " << worst.state << "; previews " << st.previews << " (" << st.focused
                   << " focused), last " << st.lastPreviewMs << " ms (max " << st.maxPreviewMs << "), full "
                   << st.lastBuildMs << " ms; face paint (check, 2x zoom, 16 frames) with a new table median "
                   << median(paintNew) << " ms, otherwise " << median(paintSame) << " ms");
    CHECK(st.focused > 0u);
    return;
  }
  drag("harmonic phases", {phaseBarAt(1, 1.0f), phaseBarAt(2, -2.0f), phaseBarAt(4, 0.5f)});
  // A basic shape replaces the edited keyframe's cycle.
  click("basic shape (triangle)", shapeAt(1));
  // A keyframe marker moved (keyframe 3 at 0.75 → ≈ 0.68).
  drag("keyframe move", {{171.5f, wtPosY(0.75f)}, {171.5f, wtPosY(0.72f)}, {171.5f, wtPosY(0.68f)}});
  // The knobs (baked: a new table every step).
  for (int k = 0; k < 3; ++k) {
    const std::string name = k == 0 ? "fold knob" : (k == 1 ? "warp knob" : "sync knob");
    drag(name, {{kWtKnobX[k], 212.0f}, {kWtKnobX[k], 208.0f}, {kWtKnobX[k], 203.0f}, {kWtKnobX[k], 199.0f}});
  }
  // Mode switches: settings (one compile each), then the new image's table.
  settingClick("warp mode Bend", warpModeAt(0));
  settingClick("warp mode Pulse width", warpModeAt(2));
  settingClick("os x2", {164.0f, 233.0f});
  settingClick("Shape mode", modeAt(1));
  drag("fold knob (Shape mode)", {{kWtKnobX[0], 212.0f}, {kWtKnobX[0], 216.0f}, {kWtKnobX[0], 220.0f}});
  click("pencil tool (Shape mode)", toolAt(0));
  drag("pencil (Shape mode)", {drawAt(0.3f, 0.5f), drawAt(0.4f, -0.5f), drawAt(0.5f, 0.2f)});
  click("leave draw mode (Shape mode)", toolAt(0));
  settingClick("Phase mode", modeAt(0));
  // The live path: fold automated (a lane holding the knob's value): the table bakes no fold, the kernel applies it.
  const double fold = metaParamValue(*findDevice(s.doc(), wt).device, "", meta::wavetableType(), meta::kWtFold);
  s.lane(0, wt, "fold",
         {Breakpoint{0, static_cast<float>(fold), 0.0f, {}}, Breakpoint{64 * kPpq, static_cast<float>(fold), 0.0f, {}}});
  link->flush();
  REQUIRE(link->lastError().empty());
  audio.run(4);
  liveFold = static_cast<double>(static_cast<float>(fold));
  settled("live fold");
  click("line tool (live path)", toolAt(1));
  drag("line (live path)", {drawAt(0.2f, 0.9f), drawAt(0.5f, -0.7f), drawAt(0.8f, 0.3f)});
  const meta::TableBuilder::Stats st = link->metaTableStats();
  MESSAGE(checks << " checks (" << previewChecks << " during gestures); face vs its table " << worst.face
                 << " (thumbnails " << worst.thumb << "), table vs the state " << worst.state << " (worst: " << worst.where << "); previews " << st.previews
                 << " (" << st.focused << " focused)");
  CHECK(st.focused > 0u);
}

}  // namespace

TEST_CASE("meta wavetable: the picture matches the sound — the reported gestures, step by step: the face draws the "
          "published table, which is the state") {
  pictureMatchesSound(false);
}

TEST_CASE("meta wavetable: the picture matches the sound — every face gesture and mode, step by step" *
          doctest::test_suite("slow")) {
  pictureMatchesSound(true);
}
