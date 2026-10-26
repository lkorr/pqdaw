// T3.17-fix2 (M3 review H1): the cost of the lowered mixer through the whole playback path (doc::lower →
// ProjectLink → Engine → ProjectProgram + Timeline), per 64-frame engine block at 48 kHz, with the transport playing.
//   mixer/<T>x<R> silent   T MIDI tracks without notes, R returns (every track has its implicit send to every
//                          return, all at −∞): what the mixer, the sends and the summing cost on their own
//   mixer/<T>x<R> notes    the same, every track holding one note (BasicSynth: the instrument dominates)
// Backends: interp (every unit interpreted: the acceptance row, ≤ 3 % of a core for 64 × 8 silent) and jit (every
// unit on its JIT image, what pqdaw plays once the background compiles are done). "busy" rows switch the idle-unit
// skip off (ProjectConfig::idleUnits): what the mixer costs while it has to run (a track whose voices sound, a send
// that carries signal). The rows are ns per block; the "% of a core" line is ns / 1 333 333 (one block period).
// Release / ship build. Usage: pq_bench_playback [runner options]
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/time.h"
#include "pq/document/commands.h"
#include "pq/document/store.h"
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
  return runner.main(argc, argv);
}
