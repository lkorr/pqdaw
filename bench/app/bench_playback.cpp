// T3.17-fix2 (M3 review H1): the cost of the lowered mixer through the whole playback path (doc::lower →
// ProjectLink → Engine → ProjectProgram + Timeline), per 64-frame engine block at 48 kHz, with the transport playing.
//   mixer/<T>x<R> silent   T MIDI tracks without notes, R returns (every track has its implicit send to every
//                          return, all at −∞): what the mixer, the sends and the summing cost on their own
//   mixer/<T>x<R> notes    the same, every track holding one note (BasicSynth: the instrument dominates)
// Backends: interp (every unit interpreted: the acceptance row, ≤ 3 % of a core for 64 × 8 silent) and jit (every
// unit on its JIT image, what pqdaw plays once the background compiles are done). "busy" rows switch the idle-unit
// skip off (ProjectConfig::idleUnits): what the mixer costs while it has to run (a track whose voices sound, a send
// that carries signal). The rows are ns per block; the "% of a core" line is ns / 1 333 333 (one block period).
// T5.4: audio tracks (each with an empty chain: the clip player, the mixer stages), one 60 s clip each of one resident
// stereo 48 kHz file, JIT images:
//   audio/64 rate 1        every clip at the device rate without transpose: the straight-copy path (plan: ≤ 1 % of a core)
//   audio/64 transposed    every clip +3 semitones: the stretched sinc resampler (plan: ≤ 6 % of a core)
//   audio/64 no clip yet   every clip starts 10 min in: nothing under the playhead (plan: the cost of 64 empty tracks)
//   audio/64 empty         64 audio tracks without clips (the reference for the row above)
// The transport loops over the first 30 s (a wrap every 30 s fades the clips over: negligible).
// T4.18: device automation, 16 MIDI tracks (BasicSynth with a held note -> Auto Filter -> Utility), JIT images:
//   device automation/16x0 moving   no lanes (the reference)
//   device automation/16xN moving   N device params per track automated (filter freq, res, Utility gain, width), a
//                                   breakpoint every half bar: every lane's value changes every block
//   device automation/16x4 held     the same four lanes, each constant: rendered, but no param changes
// (moving − reference) / (16 · N) is the cost of one automated device param per block, devices' reaction included.
// GU.4: the Phase Accumulator / VCA projects of tests/app/fixtures/gu4_modules (meta modules before GU.4, their group
// presets after: the load converts them), one MIDI / audio track each:
//   modules/<case> compile interp   ProjectLink::create (lowering, every unit's interpreter image), ms
//   modules/<case> compile jit      the same with the JIT, until every unit is on its final image, ms
//   modules/<case> play jit         one 64-frame block on the JIT images, ns/block
// Release / ship build. Usage: pq_bench_playback [runner options]
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/jit/compiler.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/time.h"
#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/engine/wav_file.h"
#include "pq_bench.h"

using namespace pq;
using namespace pq::doc;

namespace {

// T4.1: a MIDI track with the default instrument, as ops::createTrack makes it.
TrackPtr midiTrack(TrackId id, IdSource& ids) {
  Track t = makeTrack(id, TrackKind::Midi, "T");
  t.devices.push_back(makeDefaultInstrument(ids.device()));
  return std::make_shared<const Track>(std::move(t));
}

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

Snapshot project(DocumentStore& store, uint32_t tracks, uint32_t returns, bool notes) {
  IdSource& ids = store.ids();
  Batch b{"bench", {}};
  for (uint32_t r = 0; r < returns; ++r)
    b.commands.push_back(AddTrack{std::make_shared<const Track>(makeTrack(ids.track(), TrackKind::Return, "R")), {}, kAppend});
  for (uint32_t t = 0; t < tracks; ++t) {
    const TrackId id = ids.track();
    b.commands.push_back(AddTrack{midiTrack(id, ids), {}, kAppend});
    if (!notes) continue;
    NoteClip c;
    c.id = ids.clip();
    c.start = 0;
    c.length = 1000 * kPpq;
    Note n;
    n.id = ids.note();
    n.start = 0;
    n.length = 1000 * kPpq;
    n.pitch = tuning::PitchId::fromDegree(static_cast<int32_t>(t % 12), 0);
    n.velocity = 90.0f;
    c.notes = NoteSeq::fromNotes({n}).value();
    b.commands.push_back(AddClip{id, std::make_shared<const NoteClip>(std::move(c))});
  }
  if (!store.dispatch(b).ok()) std::exit(2);
  return store.current();
}

struct Rig {
  DocumentStore store;
  std::unique_ptr<app::ProjectLink> link;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  int64_t time = 0;
  Rig(uint32_t tracks, uint32_t returns, bool notes, bool jit, bool idle) {
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = jit;
    c.idleUnits = idle;
    auto made = app::ProjectLink::create(c, project(store, tracks, returns, notes));
    if (!made.ok()) {
      std::fprintf(stderr, "%s\n", made.error().message.c_str());
      std::exit(3);
    }
    link = std::move(made.value());
    link->play(0);
    if (jit) {  // room for the JIT publishes comes from blocks being run: drive the engine while it compiles
      for (int i = 0; i < 200000; ++i) {
        const app::JitStats js = link->jitStats();
        // (T1b.x-perf, Q111 C: and every quick first image refined to its final one)
        if (js.published + js.errors + js.stale >= js.wanted && js.refined + js.refineStale + js.refineErrors >= js.firstImages)
          break;
        block();
      }
      link->collect();
    }
    for (int i = 0; i < 400; ++i) block();  // attacks, smoothers settled
  }
  void block() {
    float* ch[2] = {l.data(), r.data()};
    link->engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
    time += kBlock;
  }
};

// T5.4: 64 audio tracks over one 60 s resident stereo file (written once to the temp directory).
std::string audioFile() {
  static const std::string path = [] {
    const std::filesystem::path p = std::filesystem::temp_directory_path() / "pq_bench_playback_60s.wav";
    if (!std::filesystem::exists(p)) {
      const size_t n = static_cast<size_t>(60 * kRate);
      std::vector<std::vector<float>> ch(2, std::vector<float>(n));
      for (size_t i = 0; i < n; ++i) {
        ch[0][i] = static_cast<float>(0.3 * std::sin(0.031 * static_cast<double>(i)));
        ch[1][i] = static_cast<float>(0.3 * std::sin(0.017 * static_cast<double>(i)));
      }
      if (!engine::writeWav(p, ch, static_cast<uint32_t>(kRate)).ok()) std::exit(4);
    }
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
  }();
  return path;
}

struct AudioRig {
  DocumentStore store;
  std::unique_ptr<app::ProjectLink> link;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  int64_t time = 0;
  // mode: 0 rate 1, 1 transposed, 2 clips 10 min in, 3 no clips
  AudioRig(uint32_t tracks, int mode) {
    IdSource& ids = store.ids();
    SampleRef s;
    s.id = ids.sample();
    s.path = audioFile();
    s.frames = static_cast<int64_t>(60 * kRate);
    s.rate = kRate;
    s.channels = 2;
    s.displayName = "bench";
    std::vector<ops::NewAudioClip> clips;
    for (uint32_t t = 0; t < tracks; ++t) {
      auto tr = ops::createTrack(*store.current(), ids, TrackKind::Audio);
      if (!tr.ok() || !store.dispatch(tr->command).ok()) std::exit(5);
      if (mode != 3) clips.push_back({tr->tracks.at(0), mode == 2 ? 1200 * kPpq : 0, s.id, {}});
    }
    if (!clips.empty()) {
      auto add = ops::addAudioClips(*store.current(), ids, std::span<const SampleRef>(&s, 1), clips);
      if (!add.ok() || !store.dispatch(add->command).ok()) std::exit(6);
      if (mode == 1) {
        auto tp = ops::setAudioClipTranspose(*store.current(), add->clips,
                                             AudioTranspose{TransposeUnit::Cents, 300.0, 0, {1}});
        if (!tp.ok() || !store.dispatch(tp->command).ok()) std::exit(7);
      }
    }
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = true;
    auto made = app::ProjectLink::create(c, store.current());
    if (!made.ok()) {
      std::fprintf(stderr, "%s\n", made.error().message.c_str());
      std::exit(3);
    }
    link = std::move(made.value());
    (void)link->waitForSamples();
    link->setLoop(views::TransportLoop{0, 60 * kPpq, true});  // 30 s at 120 BPM: the clips keep playing while measured
    link->play(0);
    for (int i = 0; i < 200000; ++i) {
      const app::JitStats js = link->jitStats();
      if (js.published + js.errors + js.stale >= js.wanted && js.refined + js.refineStale + js.refineErrors >= js.firstImages)
        break;
      block();
    }
    link->collect();
    for (int i = 0; i < 400; ++i) block();
  }
  void block() {
    float* ch[2] = {l.data(), r.data()};
    link->engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
    time += kBlock;
  }
};

// T4.18: T MIDI tracks, each BasicSynth (one held note) -> Auto Filter -> Utility, JIT images. `lanes` per track (0 … 4)
// of {filter freq, filter res, utility gain, utility width} automated: a triangle with a breakpoint every half bar, so
// every lane moves every block; the unautomated params keep their values. `held`: the same lanes, each one constant
// (the automation renders, but no param changes: what a lane costs while it holds a value).
struct DeviceAutoRig {
  DocumentStore store;
  std::unique_ptr<app::ProjectLink> link;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  int64_t time = 0;
  DeviceAutoRig(uint32_t tracks, uint32_t lanes, bool held) {
    IdSource& ids = store.ids();
    const char* params[4][2] = {{"af", "freq"}, {"af", "res"}, {"ut", "gain"}, {"ut", "width"}};
    const float lo[4] = {200.0f, 0.1f, 0.25f, 0.5f}, hi[4] = {8000.0f, 0.9f, 1.0f, 2.0f};
    for (uint32_t t = 0; t < tracks; ++t) {
      auto tr = ops::createTrack(*store.current(), ids, TrackKind::Midi);
      if (!tr.ok() || !store.dispatch(tr->command).ok()) std::exit(5);
      const TrackId id = tr->tracks.at(0);
      auto af = ops::addDevice(*store.current(), ids, id, "auto_filter", DeviceKind::AudioEffect);
      if (!af.ok() || !store.dispatch(af->command).ok()) std::exit(5);
      auto ut = ops::addDevice(*store.current(), ids, id, "utility", DeviceKind::AudioEffect);
      if (!ut.ok() || !store.dispatch(ut->command).ok()) std::exit(5);
      Batch b{"bench", {}};
      NoteClip c;
      c.id = ids.clip();
      c.start = 0;
      c.length = 1000 * kPpq;
      Note n;
      n.id = ids.note();
      n.start = 0;
      n.length = 1000 * kPpq;
      n.pitch = tuning::PitchId::fromDegree(static_cast<int32_t>(t % 12), 0);
      n.velocity = 90.0f;
      c.notes = NoteSeq::fromNotes({n}).value();
      b.commands.push_back(AddClip{id, std::make_shared<const NoteClip>(std::move(c))});
      for (uint32_t k = 0; k < lanes; ++k) {
        auto lane = std::make_shared<AutomationLane>();
        lane->id = ids.lane();
        const DeviceId dev = params[k][0][0] == 'a' ? af->devices.at(0) : ut->devices.at(0);
        lane->param = ParamRef::device(dev, params[k][1]);
        lane->paramName = params[k][1];
        const Ticks half = kTicksPerWhole / 2;
        for (int64_t i = 0; i <= 2000; ++i)
          lane->points.push_back({i * half, held ? lo[k] : ((i & 1) != 0 ? hi[k] : lo[k]), 0.0f, ids.breakpoint()});
        b.commands.push_back(AddAutomationLane{id, std::move(lane)});
      }
      if (!store.dispatch(b).ok()) std::exit(6);
    }
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = true;
    auto made = app::ProjectLink::create(c, store.current());
    if (!made.ok()) {
      std::fprintf(stderr, "%s\n", made.error().message.c_str());
      std::exit(3);
    }
    link = std::move(made.value());
    link->play(0);
    for (int i = 0; i < 200000; ++i) {
      const app::JitStats js = link->jitStats();
      if (js.published + js.errors + js.stale >= js.wanted && js.refined + js.refineStale + js.refineErrors >= js.firstImages)
        break;
      block();
    }
    link->collect();
    for (int i = 0; i < 400; ++i) block();
  }
  void block() {
    float* ch[2] = {l.data(), r.data()};
    link->engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
    time += kBlock;
  }
};

// TX.cross-track: T MIDI tracks, each BasicSynth (one held note) in a group of its own; `closed`: a cable from each
// group's own signal into its track's mixer closes a loop between the two tracks (its z⁻¹ on the cable): the loop's
// stages (the track's mixer sum and gains, the group's input) run per sample, everything else block-wise.
struct LoopRig {
  DocumentStore store;
  std::unique_ptr<app::ProjectLink> link;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  int64_t time = 0;
  LoopRig(uint32_t tracks, bool closed, bool jit) {
    IdSource& ids = store.ids();
    auto run = [&](const ops::EditResult& e) {
      if (!e.ok() || !store.dispatch(e->command).ok()) std::exit(5);
      return *e;
    };
    for (uint32_t t = 0; t < tracks; ++t) {
      const TrackId id = run(ops::createTrack(*store.current(), ids, TrackKind::Midi)).tracks.at(0);
      NoteClip c;
      c.id = ids.clip();
      c.start = 0;
      c.length = 1000 * kPpq;
      Note n;
      n.id = ids.note();
      n.start = 0;
      n.length = 1000 * kPpq;
      n.pitch = tuning::PitchId::fromDegree(static_cast<int32_t>(t % 12), 0);
      n.velocity = 90.0f;
      c.notes = NoteSeq::fromNotes({n}).value();
      if (!store.dispatch(AddClip{id, std::make_shared<const NoteClip>(std::move(c))}).ok()) std::exit(6);
      const TrackId g = run(ops::groupTracks(*store.current(), ids, std::vector<TrackId>{id})).tracks.at(0);
      if (closed) {
        Wire w;
        w.noteSource = g;
        w.mixer = id;
        (void)run(ops::connect(*store.current(), ids, std::move(w)));
      }
    }
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = jit;
    auto made = app::ProjectLink::create(c, store.current());
    if (!made.ok()) {
      std::fprintf(stderr, "%s\n", made.error().message.c_str());
      std::exit(3);
    }
    link = std::move(made.value());
    link->play(0);
    if (jit) {
      for (int i = 0; i < 200000; ++i) {
        const app::JitStats js = link->jitStats();
        if (js.published + js.errors + js.stale >= js.wanted && js.refined + js.refineStale + js.refineErrors >= js.firstImages)
          break;
        block();
      }
      link->collect();
    }
    for (int i = 0; i < 400; ++i) block();
  }
  void block() {
    float* ch[2] = {l.data(), r.data()};
    link->engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
    time += kBlock;
  }
};

// GU.4: a gu4_modules fixture project (loaded once; the load converts what it must).
Snapshot moduleProject(const std::string& name) {
  static const auto lib = app::loadDeviceLibrary();
  if (!lib.ok()) std::exit(7);
  LoadOptions lo;
  lo.devices = lib->get();
  auto r = loadProject((std::filesystem::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / "gu4_modules" / name).string(), lo);
  if (!r.ok()) {
    std::fprintf(stderr, "%s\n", r.error().message.c_str());
    std::exit(8);
  }
  return r->project;
}
// `compiler`: a fresh one per measured compile (no in-memory cache hits), null: the process compiler.
std::unique_ptr<app::ProjectLink> moduleLink(const Snapshot& s, bool jit, jit::Compiler* compiler = nullptr) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
  if (compiler != nullptr) c.jitBackend = compile::jitUnitBackend(compiler, false);
  auto made = app::ProjectLink::create(c, s);
  if (!made.ok()) {
    std::fprintf(stderr, "%s\n", made.error().message.c_str());
    std::exit(3);
  }
  if (!(*made)->waitForMetaTables()) std::exit(9);
  if (jit && !(*made)->waitForJit(180.0)) std::exit(10);
  return std::move(made.value());
}

}  // namespace

int main(int argc, char** argv) {
  DenormalGuard dg;
  bench::Runner runner("pq_bench_playback");
  struct Case {
    uint32_t tracks, returns;
    bool notes, jit, idle;
  };
  const Case cases[] = {{64, 0, false, false, true},  {64, 1, false, false, true},  {64, 8, false, false, true},
                        {64, 12, false, false, true}, {64, 8, false, true, true},   {64, 0, false, false, false},
                        {64, 8, false, false, false}, {64, 8, false, true, false},  {64, 8, true, false, true},
                        {64, 8, true, true, true}};
  for (const Case& k : cases) {
    const std::string name = "mixer/" + std::to_string(k.tracks) + "x" + std::to_string(k.returns) +
                             (k.notes ? " notes" : k.idle ? " silent" : " silent busy") + (k.jit ? " jit" : " interp");
    runner.add(name, [k, name](bench::Context& c) {
      Rig rig(k.tracks, k.returns, k.notes, k.jit, k.idle);
      const bench::Stats s = c.measure([&] { rig.block(); }, 1, "ns/block");
      std::printf("  %s: %.2f %% of a core (median)\n", name.c_str(), 100.0 * s.medianNs / (1e9 * kBlock / kRate));
    });
  }
  const char* audioNames[] = {"audio/64 rate 1", "audio/64 transposed", "audio/64 no clip yet", "audio/64 empty"};
  for (int mode = 0; mode < 4; ++mode) {
    const std::string name = audioNames[mode];
    runner.add(name, [mode, name](bench::Context& c) {
      AudioRig rig(64, mode);
      const bench::Stats s = c.measure([&] { rig.block(); }, 1, "ns/block");
      std::printf("  %s: %.2f %% of a core (median); %llu clip voices started\n", name.c_str(),
                  100.0 * s.medianNs / (1e9 * kBlock / kRate),
                  static_cast<unsigned long long>(rig.link->engine().timeline().clipPlayer().stats().voicesStarted));
    });
  }
  // T4.18: 16 tracks x {0 … 4} automated device params (moving), and 4 held (constant lanes).
  struct DevCase {
    uint32_t lanes;
    bool held;
  };
  for (const DevCase k : {DevCase{0, false}, DevCase{1, false}, DevCase{2, false}, DevCase{3, false}, DevCase{4, false},
                          DevCase{4, true}}) {
    const std::string name = "device automation/16x" + std::to_string(k.lanes) + (k.held ? " held" : " moving") + " jit";
    runner.add(name, [k, name](bench::Context& c) {
      DeviceAutoRig rig(16, k.lanes, k.held);
      const bench::Stats s = c.measure([&] { rig.block(); }, 1, "ns/block");
      std::printf("  %s: %.2f %% of a core (median)\n", name.c_str(), 100.0 * s.medianNs / (1e9 * kBlock / kRate));
    });
  }
  // TX.cross-track: 16 tracks in groups, each loop closed (per sample) or open (the same project without the cables).
  for (const bool jit : {false, true})
    for (const bool closed : {false, true}) {
      const std::string name = std::string("cross-track loop/16 ") + (closed ? "closed" : "open") + (jit ? " jit" : " interp");
      runner.add(name, [jit, closed, name](bench::Context& c) {
        LoopRig rig(16, closed, jit);
        const bench::Stats s = c.measure([&] { rig.block(); }, 1, "ns/block");
        std::printf("  %s: %.2f %% of a core (median)\n", name.c_str(), 100.0 * s.medianNs / (1e9 * kBlock / kRate));
      });
    }
  // GU.4: compile and play the Phase Accumulator / VCA projects.
  for (const char* m : {"pa_voice", "pa_free", "vca_law", "vca_driven"}) {
    const std::string mod = m;
    runner.add("modules/" + mod + " compile interp", [mod](bench::Context& c) {
      const Snapshot s = moduleProject(mod);
      c.measure([&] { (void)moduleLink(s, false); }, 1, "ns");
    });
    runner.add("modules/" + mod + " compile jit", [mod](bench::Context& c) {
      const Snapshot s = moduleProject(mod);
      c.measure([&] {
        auto compiler = jit::Compiler::create();
        if (!compiler.ok()) std::exit(11);
        (void)moduleLink(s, true, compiler->get());
      }, 1, "ns");
    });
    runner.add("modules/" + mod + " play jit", [mod](bench::Context& c) {
      const Snapshot s = moduleProject(mod);
      auto link = moduleLink(s, true);
      link->play(0);
      std::vector<float> l(kBlock), r(kBlock);
      int64_t t = 0;
      c.measure([&] {
        float* ch[2] = {l.data(), r.data()};
        link->engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
        t += kBlock;
        if (t > static_cast<int64_t>(kRate) * 2) {  // the clip's notes: loop the first 2 s
          link->play(0);
          t = 0;
        }
      }, 1, "ns/block");
    });
  }
  return runner.main(argc, argv);
}
