// TX.scope-sound (bug 2026-11-02_18-21-54: "this patch sounds different with the oscilloscope in it vs if i delete the
// oscilloscope"): a pass-through device — the analysers (Oscilloscope, Spectrum, Spectrogram), whose output is their
// input — never changes the sound, wherever it sits: last in a meta voice run (the voices still end by the Envelope's
// `done`), after a stock effect, inside a group (alone, after the run's Envelope, the whole run grouped), free in a loop.
// Each case renders the patch with and without the analyser through the interpreter (bit for bit equal) and the patch
// with it through the JIT (equal to the interpreter), a three-note chord with its note-offs and releases inside the
// render, under the allocation trap.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/document/lower.h"
#include "pq/document/meta.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/meta/registry.h"
#include "playback_fixture.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
// The chord's notes end at 30000 … 39000 frames; the Envelope's release (0.25 s by default) ends inside the render.
constexpr uint32_t kFrames = 72000;
constexpr const char* kAnalysers[] = {"oscilloscope", "spectrum", "spectrogram"};

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  return **lib;
}

// The lowered project of `p` (one MIDI event slot) rendered for `frames` with a three-note chord.
// IG.1-2: rendered as an older file of the project loads: its meta voice runs in instrument groups (per note), as written.
golden::Channels render(const Project& p0, uint32_t frames, engine::UnitBackend backend = {}) {
  Project p = p0;
  (void)migrateToInstrumentGroups(p);
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [](TrackId) { return 0u; };
  auto low = lower(p, lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  CHECK(low->missingDevices.empty());
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
  for (const LoweredParam& x : low->params) {
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
  auto build = compiler.build(low->graph);
  REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
  engine::NoteListSource notes(std::vector<engine::TimedNote>{
      {0, 30000, 220.0f, 0.8f, 0.5f}, {3000, 30000, 277.18f, 0.6f, 0.5f}, {9000, 30000, 329.63f, 0.9f, 0.5f}});
  engine::ProjectConfig pc;
  pc.sampleRate = kRate;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 2;
  pc.numProjectParams = static_cast<uint32_t>(values.size());
  pc.eventSources.push_back(notes.source());
  auto prog = engine::ProjectProgram::create(pc, build->layout);
  REQUIRE_MESSAGE(prog.ok(), (prog.ok() ? std::string() : prog.error().message));
  for (uint32_t u = 0; u < build->images.size(); ++u) REQUIRE((*prog)->publish(u, std::move(build->images[u])).ok());
  std::copy(values.begin(), values.end(), (*prog)->projectParams().begin());
  golden::Channels out(2);
  std::vector<float> l(kBlock), r(kBlock);
  float* ch[2] = {l.data(), r.data()};
  for (int64_t t = 0; t < frames; t += kBlock) {
    engine::AudioBlock ab{nullptr, 0, ch, 2, kBlock, t};
    CHECK(alloc_trap::countRtViolations([&] {
            DenormalGuard dg;
            (*prog)->process(ab);
          }) == 0);
    out[0].insert(out[0].end(), l.begin(), l.end());
    out[1].insert(out[1].end(), r.begin(), r.end());
  }
  return out;
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

// The last frame whose |sample| exceeds 1e-6 (+1; 0: silent): where the release ends.
size_t tailEnd(const golden::Channels& a) {
  size_t end = 0;
  for (const auto& c : a)
    for (size_t i = 0; i < c.size(); ++i)
      if (std::fabs(c[i]) > 1e-6f) end = std::max(end, i + 1);
  return end;
}

double rms(const golden::Channels& a, size_t from, size_t to) {
  double s = 0.0;
  size_t n = 0;
  for (const auto& c : a)
    for (size_t i = from; i < std::min(to, c.size()); ++i, ++n) s += static_cast<double>(c[i]) * c[i];
  return n == 0 ? 0.0 : std::sqrt(s / static_cast<double>(n));
}

// TX.all-free: the devices made `free` (placed off the main path); wired() leaves them out of the series.
std::vector<DeviceId> gOffPath;

DevicePtr metaDevice(DeviceId id, std::string_view type, DeviceKind kind, bool free = false) {
  const meta::Type* t = meta::findType(type);
  REQUIRE(t != nullptr);
  auto d = std::make_shared<Device>(*makeDevice(id, std::string(kMetaDevicePrefix) + std::string(type), kind));
  d->meta = std::make_shared<const MetaMap>(MetaMap{MetaEntry{"", defaultMetaInstance(*t)}});
  if (free) gOffPath.push_back(id);
  return d;
}

void wired(DocumentStore& s) {
  playback_fixture::wireSeries(s, gOffPath);
  gOffPath.clear();
}

void wire(DocumentStore& s, DeviceId from, std::string port, DeviceId to, std::string param) {
  auto w = ops::connectWire(*s.current(), s.ids(), from, std::move(port), to, std::move(param));
  REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
  REQUIRE(s.dispatch(w->command).ok());
}

void groupOf(DocumentStore& s, std::vector<DeviceId> devices) {
  auto e = ops::groupInCircuit(s.current(), s.ids(), library(), devices);
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(s.dispatch(e->command).ok());
}

// The reported patch (bug 2026-11-02_18-21-54): a MIDI track PA → Wavetable (fold 0.25) → Envelope (attack 0.03)
// [→ the analyser], a free VCA (value 0.3: ×−1) with Envelope.out → VCA.in and VCA.out → PA.freq — a loop through the
// voice run (its 1-sample delay on a wire). `stock`: a plain block (GU.11: an atom block, tanh) after the Envelope
// (before the analyser).
// GU.4: the head a Wavetable at the notes (was the Phase accumulator, a group preset now), the VCA its module (a group:
// In → mul ← Value at 0.3), the loop closing into the first Wavetable's position input.
struct Bug {
  TrackId track;
  DeviceId pa, wt, env, vca, sat, analyser;
};
Bug bugPatch(DocumentStore& store, const char* analyser, bool stock = false) {
  IdSource& ids = store.ids();
  Bug g{};
  g.track = ids.track();
  g.pa = ids.device();
  g.wt = ids.device();
  g.env = ids.device();
  g.vca = ids.device();
  Batch b{"bug", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(g.track, TrackKind::Midi, "1 MIDI"), {}, kAppend});
  b.commands.push_back(AddDevice{g.track, metaDevice(g.pa, "wavetable", DeviceKind::Instrument), kAppend});
  b.commands.push_back(AddDevice{g.track, metaDevice(g.wt, "wavetable", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{g.track, metaDevice(g.env, "envelope", DeviceKind::AudioEffect), kAppend});
  if (stock) {
    g.sat = ids.device();
    // GU.11: a plain (non-meta) block in the chain: an atom block (the stock Saturator was one).
    b.commands.push_back(AddDevice{g.track, makeDevice(g.sat, "atom:tanh", DeviceKind::AudioEffect), kAppend});
  }
  if (analyser != nullptr) {
    g.analyser = ids.device();
    b.commands.push_back(AddDevice{g.track, metaDevice(g.analyser, analyser, DeviceKind::AudioEffect), kAppend});
  }
  b.commands.push_back(SetDeviceParam{g.wt, "fold", 0.2533332407474518});
  b.commands.push_back(SetDeviceParam{g.env, "attack", 0.03081697977654113});
  REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
  wired(store);  // TX.all-free: wired in series
  VcaOptions vo;
  vo.value = 0.3;
  IdSource local;
  auto paste = ops::pasteBlocks(*store.current(), store.ids(), g.track, vcaBlocks(local, vo));
  REQUIRE(paste.ok());
  REQUIRE(store.dispatch(paste->command).ok());
  g.vca = paste->devices.at(0);
  wire(store, g.env, "out", g.vca, "");
  wire(store, g.vca, "", g.pa, "pos");
  return g;
}

// `with` sounds exactly as `without` (interpreter), and `with` through the JIT exactly as through the interpreter.
// `sounds` false: the patch is silent by IG.1-2's rule (an Envelope outside every instrument group reads no note's gate)
// — the analyser must still change nothing.
void same(const Project& without, const Project& with, const std::string& what, bool sounds = true) {
  const golden::Channels a = render(without, kFrames);
  const golden::Channels b = render(with, kFrames);
  const golden::Channels j = render(with, kFrames, compile::jitUnitBackend());
  if (sounds) {
    CHECK(rms(a, 0, 30000) > 1e-3);
    CHECK(tailEnd(a) > 39000);  // the releases are inside the render
  }
  const double d = maxDiff(a, b), dj = maxDiff(b, j);
  MESSAGE(what << ": without vs with, max |diff| = " << d << " (tail end " << tailEnd(a) << " vs " << tailEnd(b)
               << ", release rms " << rms(a, 39000, kFrames) << " vs " << rms(b, 39000, kFrames)
               << "); with, interpreter vs JIT = " << dj);
  CHECK(d == 0.0);
  CHECK(dj == 0.0);
}

}  // namespace

TEST_CASE("scope-sound: the reported patch sounds the same with each analyser last in the voice run as without it") {
  DocumentStore none;
  (void)bugPatch(none, nullptr);
  for (const char* a : kAnalysers) {
    DocumentStore with;
    (void)bugPatch(with, a);
    same(*none.current(), *with.current(), std::string("PA → WT → Envelope → ") + a + " (VCA loop)");
  }
}

TEST_CASE("scope-sound: two analysers in a row, and an analyser between the run's stages, change nothing") {
  DocumentStore none;
  (void)bugPatch(none, nullptr);
  {
    DocumentStore with;
    const Bug g = bugPatch(with, "spectrum");
    const DeviceId more = with.ids().device();
    REQUIRE(with.dispatch(AddDevice{g.track, metaDevice(more, "oscilloscope", DeviceKind::AudioEffect), kAppend}).ok());
    {  // TX.all-free: onto the path's end
      auto e = ops::insertOnPath(*with.current(), with.ids(), more, kAppend);
      REQUIRE(e.ok());
      REQUIRE(with.dispatch(e->command).ok());
    }
    same(*none.current(), *with.current(), "… → Envelope → Spectrum → Oscilloscope");
  }
  {
    DocumentStore with;
    const Bug g = bugPatch(with, nullptr);
    const DeviceId mid = with.ids().device();
    REQUIRE(with.dispatch(AddDevice{g.track, metaDevice(mid, "spectrogram", DeviceKind::AudioEffect), 1}).ok());
    {  // TX.all-free: onto the path after the Phase accumulator
      auto e = ops::insertOnPath(*with.current(), with.ids(), mid, 1);
      REQUIRE(e.ok());
      REQUIRE(with.dispatch(e->command).ok());
    }
    same(*none.current(), *with.current(), "PA → Spectrogram → WT → Envelope");
  }
}

TEST_CASE("scope-sound: an analyser after a stock effect changes nothing") {
  DocumentStore none;
  (void)bugPatch(none, nullptr, /*stock=*/true);
  for (const char* a : kAnalysers) {
    DocumentStore with;
    (void)bugPatch(with, a, /*stock=*/true);
    same(*none.current(), *with.current(), std::string("… → Envelope → Saturator → ") + a);
  }
}

TEST_CASE("scope-sound: an analyser inside a group changes nothing — alone, after the Envelope, the whole run grouped") {
  for (const char* a : kAnalysers) {
    SUBCASE("the analyser alone in a group, last in the run") {
      DocumentStore none, with;
      (void)bugPatch(none, nullptr);
      const Bug g = bugPatch(with, a);
      groupOf(with, {g.analyser});
      same(*none.current(), *with.current(), std::string("… → Envelope → [") + a + "]");
    }
    SUBCASE("Envelope → analyser grouped") {
      DocumentStore none, with;
      const Bug n = bugPatch(none, nullptr);
      groupOf(none, {n.env});
      const Bug g = bugPatch(with, a);
      groupOf(with, {g.env, g.analyser});
      // (IG.1-2: the group holding the run's Envelope is not an instrument group: the Envelope is outside the voices.)
      same(*none.current(), *with.current(), std::string("… → [Envelope] vs … → [Envelope → ") + a + "]", false);
    }
    SUBCASE("the whole run grouped") {
      DocumentStore none, with;
      const Bug n = bugPatch(none, nullptr);
      // (IG.1-2: with the VCA of the run's feedback loop, so the loop stays inside the instrument group — a loop across
      // its edge is IG.3's per-sample region.)
      groupOf(none, {n.pa, n.wt, n.env, n.vca});
      const Bug g = bugPatch(with, a);
      groupOf(with, {g.pa, g.wt, g.env, g.vca, g.analyser});
      same(*none.current(), *with.current(), std::string("[PA → WT → Envelope] vs [… → ") + a + "]");
    }
  }
}

TEST_CASE("scope-sound: a free analyser fed by the Envelope, and one inside the VCA loop, change nothing") {
  for (const char* a : kAnalysers) {
    DocumentStore none;
    (void)bugPatch(none, nullptr);
    {
      DocumentStore with;
      const Bug g = bugPatch(with, nullptr);
      const DeviceId f = with.ids().device();
      REQUIRE(with.dispatch(AddDevice{g.track, metaDevice(f, a, DeviceKind::AudioEffect), kAppend}).ok());  // unwired
      wire(with, g.env, "out", f, "in");
      same(*none.current(), *with.current(), std::string("free ") + a + " reading Envelope.out");
    }
    {
      // Envelope.out → analyser → VCA.in instead of Envelope.out → VCA.in: the loop's 1-sample delay stays one sample.
      DocumentStore with;
      const Bug g = bugPatch(with, nullptr);
      const DeviceId f = with.ids().device();
      REQUIRE(with.dispatch(AddDevice{g.track, metaDevice(f, a, DeviceKind::AudioEffect), kAppend}).ok());  // unwired
      Batch drop{"unwire", {}};
      for (const Wire& w : wiresOf(*with.current()))
        if (w.from == g.env && w.to == g.vca) drop.commands.push_back(RemoveWire{w.id});
      REQUIRE(drop.commands.size() == 1);
      REQUIRE(with.dispatch(drop).ok());
      wire(with, g.env, "out", f, "in");
      wire(with, f, "out", g.vca, "in");
      same(*none.current(), *with.current(), std::string("Envelope → free ") + a + " → VCA (in the loop)");
    }
  }
}
