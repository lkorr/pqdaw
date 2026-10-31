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
// Release / ship build. Usage: pq_bench_playback [runner options]
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/time.h"
#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
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
  return runner.main(argc, argv);
}
