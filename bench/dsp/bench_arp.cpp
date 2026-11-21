// AP.1: the arpeggiator note stage (pq/engine/arp.h) per 64-frame block at 48 kHz, 120 BPM, through the stage's
// function pointer as the engine calls it:
//   arp/idle             installed, nothing held, no event (the cost of a slot with an arp and no notes)
//   arp/8 notes 1/32     8 held notes, Up, 1/32 synced on a playing transport (a step every 3000 samples: most
//                        blocks have no step, every ~47th has one note-off and one note-on)
//   arp/8 notes 1/32 chord   the same as Chord Trigger (8 offs + 8 ons per step)
#include <cstdint>
#include <memory>
#include <string>

#include "pq/engine/arp.h"
#include "pq/engine/note_stage.h"
#include "pq_bench.h"

namespace {

namespace e = pq::engine;
constexpr uint32_t kFrames = 64;

void benchArp(pq::bench::Runner& runner, const std::string& name, e::ArpStyle style, uint32_t notes) {
  runner.add("arp/" + name, [style, notes](pq::bench::Context& ctx) {
    e::ArpParams p;
    p.style = style;
    p.rate = {true, 1, 32, 1, 1, 100.0f};
    auto stage = std::make_unique<e::ArpStage>(p);
    auto tempo = std::make_unique<pq::TempoMap>();
    e::EventBuffer in(64), out(1024);
    for (uint32_t i = 0; i < notes; ++i) in.push(e::noteOn(0, i + 1, 200.0f + 25.0f * static_cast<float>(i), 0.8f));
    e::ClockSegment seg{0, kFrames, true, 0};
    int64_t t = 0;
    bool first = true;
    auto block = [&] {
      seg.pos = t;
      e::NoteStageBlock b;
      b.time = t;
      b.frames = kFrames;
      b.clock.tempo = tempo.get();
      b.clock.sampleRate = 48000.0;
      b.clock.segments = &seg;
      b.clock.numSegments = 1;
      out.clear();
      const e::EventList none{};
      stage->processFn(stage.get(), b, first ? in.list() : none, out);
      first = false;
      t += kFrames;
      pq::bench::doNotOptimize(out.size());
    };
    ctx.measure(block, 1, "ns/block");
  });
}

}  // namespace

void addArpBenches(pq::bench::Runner& runner) {
  benchArp(runner, "idle", e::ArpStyle::Up, 0);
  benchArp(runner, "8 notes 1/32", e::ArpStyle::Up, 8);
  benchArp(runner, "8 notes 1/32 chord", e::ArpStyle::ChordTrigger, 8);
}
