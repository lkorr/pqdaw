// AP.1: the arpeggiator note stage (pq/engine/arp.h) per 64-frame block at 48 kHz, 120 BPM, through the stage's
// function pointer as the engine calls it:
//   arp/idle             installed, nothing held, no event (the cost of a slot with an arp and no notes)
//   arp/8 notes 1/32     8 held notes, Up, 1/32 synced on a playing transport (a step every 3000 samples: most
//                        blocks have no step, every ~47th has one note-off and one note-on)
//   arp/8 notes 1/32 chord   the same as Chord Trigger (8 offs + 8 ons per step)
//   arp/8 notes 1/32 glides  AP.3: Up with Into (previous step) and Out (next step) glides, Exponential: per step a
//                        note-on + an Into Pitch event, and an Out action (a look-ahead pick) at the gate's end
//   arp/walk 4 notes 1/32    AP.3: Harmonic Walk over 4 JI notes (Range 7, merged by Hz), UpDown
//   arp/pulse 8:9:10:12      AP.4: Pulse mode, 4 JI notes (harmonics 8, 9, 10, 12), Base 1/16, h_ref = 8: four
//                        clocks (6000 · 8 / h samples), a note-off + note-on per onset
//   arp/pulse 8:9:10:12 layers   the same with 3 layers (×3/2 each): four notes per onset
//   arp/pulse 128 notes      AP.4: 128 JI notes (harmonics 8 … 135), Base 1/4, h_ref 8: ≈ 1144 onsets per beat
#include <cstdint>
#include <memory>
#include <string>

#include "pq/engine/arp.h"
#include "pq/engine/note_stage.h"
#include "pq_bench.h"

namespace {

namespace e = pq::engine;
constexpr uint32_t kFrames = 64;

void benchArp(pq::bench::Runner& runner, const std::string& name, e::ArpStyle style, uint32_t notes,
              bool glides = false, int32_t pulseDen = 0, int32_t layers = 0, const int32_t* harmonics = nullptr) {
  runner.add("arp/" + name, [style, notes, glides, pulseDen, layers, harmonics](pq::bench::Context& ctx) {
    e::ArpParams p;
    p.style = style;
    p.rate = {true, 1, 32, 1, 1, 100.0f};
    p.walkRange = 7;
    p.walkOrder = e::ArpWalkOrder::UpDown;
    if (glides) {
      p.into = e::ArpGlide::Step;
      p.out = e::ArpGlide::Step;
      p.glideMs = 10.0f;
      p.glideShape = e::RampShape::Exponential;
    }
    if (pulseDen > 0) {
      p.pulse = true;
      p.rate = {true, 1, pulseDen, 1, 1, 100.0f};
      p.layers = layers;
      p.ratioP = 3;
      p.ratioQ = 2;
    }
    auto stage = std::make_unique<e::ArpStage>(p);
    auto tempo = std::make_unique<pq::TempoMap>();
    e::EventBuffer in(256), out(1024);
    for (uint32_t i = 0; i < notes; ++i) {
      const int32_t hh = harmonics != nullptr ? harmonics[i] : static_cast<int32_t>(8 + i);
      e::NoteEvent n = e::noteOn(0, i + 1, 25.0f * static_cast<float>(hh), 0.8f);
      n.harmonic = static_cast<float>(hh);  // a JI group over 25 Hz
      in.push(n);
    }
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
  benchArp(runner, "8 notes 1/32 glides", e::ArpStyle::Up, 8, true);
  benchArp(runner, "walk 4 notes 1/32", e::ArpStyle::HarmonicWalk, 4);
  static constexpr int32_t kPulse[4] = {8, 9, 10, 12};
  benchArp(runner, "pulse 8:9:10:12", e::ArpStyle::Up, 4, false, 16, 0, kPulse);
  benchArp(runner, "pulse 8:9:10:12 layers", e::ArpStyle::Up, 4, false, 16, 3, kPulse);
  benchArp(runner, "pulse 128 notes", e::ArpStyle::Up, 128, false, 4);
}
