// T3.16: the engine timeline. Release build only. Runner benchmarks (pq_bench.h):
//   idle/<tracks>x200 clips      Timeline::process for a 64-frame block while playing, no event in the block: the
//                                per-track fast path (one compare) plus the per-block transport work, reported per
//                                track (criterion: < 50 ns per track) and per block
//   busy/16ths x64               64 tracks of 1/16 notes (one note-on + one note-off every 1/16 per track), an 8-bar
//                                loop: per block and per event (block cost / events per block)
//   build/100k notes             SequenceImage::build from plain descriptions (criterion: < 20 ms)
//   automation/<kind>            AutomationRenderer per lane-sample for 32 lanes over 64-frame blocks: constant,
//                                straight, curved, and a ramping tempo (exact per-sample ticks)
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "pq/engine/automation.h"
#include "pq/engine/sequence.h"
#include "pq/engine/timeline.h"
#include "pq_bench.h"

using namespace pq;
namespace e = pq::engine;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr Ticks kBar = kTicksPerWhole;

[[noreturn]] void die(const std::string& m) {
  std::fprintf(stderr, "%s\n", m.c_str());
  std::exit(1);
}

// `tracks` tracks × 200 one-bar clips (with `loops`, every third looped on half a bar), `notesPerClip` notes per
// clip from bar `firstNoteBar` on (earlier clips are empty), offsets spread over the beat.
std::vector<e::TrackSequenceDesc> arrangement(uint32_t tracks, uint32_t notesPerClip, uint32_t firstNoteBar,
                                              bool loops = true) {
  std::vector<e::TrackSequenceDesc> out(tracks);
  uint32_t id = 1;
  for (uint32_t k = 0; k < tracks; ++k) {
    out[k].id = k + 1;
    for (uint32_t c = 0; c < 200; ++c) {
      e::ClipDesc cd;
      cd.id = c + 1;
      cd.start = static_cast<Ticks>(c) * kBar;
      cd.length = kBar;
      if (loops && (c + k) % 3 == 0) {
        cd.loopEnabled = true;
        cd.loopStart = 0;
        cd.loopEnd = 2 * kPpq;
      }
      if (c >= firstNoteBar)
        for (uint32_t i = 0; i < notesPerClip; ++i) {
          e::NoteDesc n;
          n.id = id++;
          n.start = static_cast<Ticks>(i) * kBar / notesPerClip + (k * 131) % 997;
          n.length = kBar / notesPerClip / 2;
          n.hz = 110.0f + static_cast<float>(k);
          n.chance = i % 5 == 4 ? 0.75f : 1.0f;
          n.velocityRange = 10.0f;
          cd.notes.push_back(n);
        }
      out[k].clips.push_back(std::move(cd));
    }
  }
  return out;
}

std::unique_ptr<e::SequenceImage> build(const std::vector<e::TrackSequenceDesc>& d) {
  auto r = e::SequenceImage::build(d);
  if (!r) die(r.error().message);
  return std::move(r.value());
}

e::TimelineConfig config(uint32_t tracks) {
  e::TimelineConfig c;
  c.sampleRate = kRate;
  c.maxBlockFrames = kBlock;
  c.maxTracks = tracks;
  c.maxAutomationLanes = 64;
  return c;
}

e::TransportCommand play(Ticks t) {
  e::TransportCommand c;
  c.kind = e::TransportCommandKind::Play;
  c.tick = t;
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  pq::bench::Runner r("pq_bench_timeline");

  // No events: every note is beyond bar 190; the playhead stays in bars 0..~2 (clips exist, cursors are parked on
  // the far notes). A loop keeps the measurement there however many blocks the runner calls.
  for (uint32_t tracks : {1u, 8u, 64u}) {
    r.add("idle/" + std::to_string(tracks) + "x200 clips", [tracks](pq::bench::Context& c) {
      e::Timeline tl(config(tracks));
      if (!tl.publishSequence(build(arrangement(tracks, 8, 190)))) die("publish");
      e::TransportCommand loop;
      loop.kind = e::TransportCommandKind::SetLoop;
      loop.loopEnabled = true;
      loop.loopStart = 0;
      loop.loopEnd = 2 * kBar;
      tl.apply(loop);
      tl.apply(play(0));
      int64_t clock = 0;
      auto block = [&] {
        tl.process(clock, kBlock);
        clock += kBlock;
      };
      for (int i = 0; i < 100; ++i) block();
      c.measure(block, tracks, "ns/track-block", "per track");
      c.measure(block, 1, "ns/block", "per block");
      if (tl.stats().noteOns != 0) die("idle benchmark played notes");
    });
  }

  // Events: 64 tracks of 16ths (64 notes per bar per track, 2 events each).
  r.add("busy/16ths x64", [](pq::bench::Context& c) {
    e::Timeline tl(config(64));
    if (!tl.publishSequence(build(arrangement(64, 16, 0)))) die("publish");
    // An 8-bar loop keeps the playhead among the notes however many blocks the runner calls (a wrap every 12000
    // blocks: its release + re-seek is amortised away).
    e::TransportCommand loop;
    loop.kind = e::TransportCommandKind::SetLoop;
    loop.loopEnabled = true;
    loop.loopStart = 0;
    loop.loopEnd = 8 * kBar;
    tl.apply(loop);
    tl.apply(play(0));
    int64_t clock = 0;
    // Events per block on average: measure them over a stretch first.
    uint64_t events = 0;
    const int warm = 4000;
    for (int i = 0; i < warm; ++i) {
      tl.process(clock, kBlock);
      for (uint32_t k = 0; k < 64; ++k) events += tl.events(k).count;
      clock += kBlock;
    }
    const double perBlock = static_cast<double>(events) / warm;
    std::printf("INFO   busy: %.2f events per 64-frame block\n", perBlock);
    const pq::bench::Stats s = c.measure([&] {
      tl.process(clock, kBlock);
      clock += kBlock;
    }, 1, "ns/block", "per block");
    pq::bench::Stats perEvent = s;
    perEvent.medianNs /= perBlock;
    perEvent.minNs /= perBlock;
    perEvent.p99Ns /= perBlock;
    perEvent.maxNs /= perBlock;
    c.record(perEvent, "ns/event", "per event");
  });

  r.add("build/100k notes", [](pq::bench::Context& c) {
    // 64 tracks × 200 clips × 8 notes = 102,400 notes.
    const auto d = arrangement(64, 8, 0, false);
    size_t n = 0;
    for (const auto& t : d)
      for (const auto& cl : t.clips) n += cl.notes.size();
    std::printf("INFO   build: %zu notes\n", n);
    c.measure([&] {
      auto img = build(d);
      pq::bench::doNotOptimize(img);
    }, 1, "ns/build");
  });

  // Automation: 32 lanes, one long segment each (or flat), 64-frame blocks.
  struct AutoCase {
    const char* name;
    float v0, v1, curve;
    bool ramp;
  };
  for (const AutoCase& ac : {AutoCase{"constant", 0.5f, 0.5f, 0.0f, false}, AutoCase{"straight", 0.0f, 1.0f, 0.0f, false},
                             AutoCase{"curved", 0.0f, 1.0f, 0.6f, false}, AutoCase{"curved, tempo ramp", 0.0f, 1.0f, 0.6f, true}}) {
    r.add(std::string("automation/") + ac.name, [ac](pq::bench::Context& c) {
      e::Timeline tl(config(1));
      std::vector<e::AutomationLaneDesc> lanes(32);
      for (uint32_t l = 0; l < 32; ++l) {
        lanes[l].target = l;
        lanes[l].points = {{0, ac.v0, ac.curve}, {100000 * kBar, ac.v1, 0.0f}};
      }
      auto ai = e::AutomationImage::build(lanes);
      if (!ai) die(ai.error().message);
      if (!tl.publishAutomation(std::move(ai.value()))) die("publish");
      if (ac.ramp) {
        const std::vector<TempoPoint> tp{{0, 100.0, TempoCurve::Linear}, {100000 * kBar, 200.0, TempoCurve::Step}};
        auto ti = e::TempoImage::create(tp, {});
        if (!ti) die(ti.error().message);
        if (!tl.publishTempo(std::move(ti.value()))) die("publish");
      }
      tl.apply(play(0));
      int64_t clock = 0;
      c.measure([&] {
        tl.process(clock, kBlock);
        clock += kBlock;
      }, 32ull * kBlock, "ns/lane-sample");
    });
  }

  return r.main(argc, argv);
}
