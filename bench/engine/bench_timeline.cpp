// T3.16: the engine timeline. Release build only. Runner benchmarks (pq_bench.h):
//   idle/<tracks>x200 clips      Timeline::process for a 64-frame block while playing, no event in the block: the
//                                per-track fast path (one compare) plus the per-block transport work, reported per
//                                track (criterion: < 50 ns per track) and per block
//   busy/16ths x64               64 tracks of 1/16 notes (one note-on + one note-off every 1/16 per track), an 8-bar
//                                loop: per block and per event (block cost / events per block)
//   build/100k notes             SequenceImage::build from plain descriptions (criterion: < 20 ms)
//   automation/<kind>            AutomationRenderer per lane-sample for 32 lanes over 64-frame blocks: constant,
//                                straight, curved, and a ramping tempo (exact per-sample ticks); "…, block rate":
//                                the same lanes with AutomationLaneDesc::perSample false (T2.6-fix2: lanes whose
//                                consumers read params render only their frame-0 value)
//   clips/64 <kind>              T5.4: the clip player alone (Timeline::process with 64 clip slots, each playing one clip
//                                of a shared resident stereo 48 kHz sample; the mixer is pq_bench_playback's): rate 1
//                                (straight copies), +3 st and +7 st (stretched sinc), -5 st (sinc), 44.1 kHz file
//                                (sinc), no clip under the playhead (the clips start 10 min in): per block and per
//                                track-frame. T5.x-perf-audio (Q151 item 1): +24 st and +36 st read directly (the
//                                stretched kernel) and "… mips" from the sample's decimated copies (a pooled resident
//                                sample with media::SamplePool::buildMips; the loop is shortened so the reads stay
//                                inside the file)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/engine/audio_sequence.h"
#include "pq/engine/automation.h"
#include "pq/engine/sequence.h"
#include "pq/engine/timeline.h"
#include "pq/media/sample_pool.h"
#include "pq/media/wav_file.h"
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

  // T5.4: the clip player. One 60 s stereo sample shared by 64 slots; the transport loops over the first 30 s.
  struct ClipCase {
    const char* name;
    double rate;
    bool later;
    bool mips = false;
    Ticks loop = 60 * kPpq;  // 30 s at 120 BPM
  };
  for (const ClipCase& cc : {ClipCase{"rate 1", 1.0, false}, ClipCase{"+3 st", std::pow(2.0, 3.0 / 12.0), false},
                             ClipCase{"+7 st", std::pow(2.0, 7.0 / 12.0), false},
                             ClipCase{"-5 st", std::pow(2.0, -5.0 / 12.0), false},
                             ClipCase{"44.1 kHz file", 44100.0 / 48000.0, false}, ClipCase{"no clip under the playhead", 1.0, true},
                             ClipCase{"+24 st", 4.0, false, false, 24 * kPpq}, ClipCase{"+24 st mips", 4.0, false, true, 24 * kPpq},
                             ClipCase{"+36 st", 8.0, false, false, 12 * kPpq}, ClipCase{"+36 st mips", 8.0, false, true, 12 * kPpq}}) {
    r.add(std::string("clips/64 ") + cc.name, [cc](pq::bench::Context& c) {
      static pq::AlignedBuffer<float> data;
      const int64_t frames = static_cast<int64_t>(60 * kRate), g = pq::media::kSampleGuardFrames;
      if (data.size() == 0) {
        data = pq::AlignedBuffer<float>(static_cast<size_t>(2 * (frames + 2 * g)));
        for (int64_t i = 0; i < frames; ++i) {
          data[static_cast<size_t>(g + i)] = static_cast<float>(0.3 * std::sin(0.031 * static_cast<double>(i)));
          data[static_cast<size_t>(3 * g + frames + i)] = static_cast<float>(0.3 * std::sin(0.017 * static_cast<double>(i)));
        }
      }
      e::ClipSample smp;
      smp.fixed.data[0] = data.data() + g;
      smp.fixed.data[1] = data.data() + 3 * g + frames;
      smp.fixed.frames = smp.fixed.residentFrames = frames;
      smp.fixed.channels = 2;
      smp.fixed.sampleRate = 48000;
      // The mips rows: the same frames as a pooled resident sample with its decimated copies.
      static std::unique_ptr<pq::media::SamplePool> pool;
      static pq::media::SampleRef pooled;
      if (cc.mips) {
        if (!pooled) {
          const std::filesystem::path p = std::filesystem::temp_directory_path() / "pq_bench_timeline_60s.wav";
          std::vector<std::vector<float>> ch(2, std::vector<float>(static_cast<size_t>(frames)));
          for (int64_t i = 0; i < frames; ++i) {
            ch[0][static_cast<size_t>(i)] = smp.fixed.data[0][i];
            ch[1][static_cast<size_t>(i)] = smp.fixed.data[1][i];
          }
          if (!pq::media::writeWav(p, ch, 48000).ok()) die("write wav");
          pq::media::PoolConfig pc;
          pc.residentLimitBytes = 1ull << 30;
          pool = std::make_unique<pq::media::SamplePool>(pc);
          auto s = pool->load(p);
          if (!s) die(s.error().message);
          pooled = std::move(s.value());
          if (!pool->buildMips(*pooled, pq::dsp::kMaxMipLevels)) die("mips");
        }
        smp = {};
        smp.pooled = pooled;
      }
      std::vector<e::AudioTrackDesc> slots(64);
      for (uint32_t k = 0; k < 64; ++k) {
        e::AudioClipDesc d;
        d.id = k + 1;
        d.start = cc.later ? 1200 * kPpq : 0;
        d.length = 110 * kPpq;
        d.speed = kRate * cc.rate;
        d.contentStartFrame = 0.0;
        slots[k].id = k + 1;
        slots[k].clips.push_back(d);
      }
      e::Timeline tl(config(64));
      e::AudioBuildOptions o;
      o.sampleRate = kRate;
      auto img = e::AudioSequenceImage::build(slots, {smp}, TempoMap{}, o);
      if (!img) die(img.error().message);
      if (!tl.publishAudio(std::move(img.value()))) die("publish");
      e::TransportCommand loop;
      loop.kind = e::TransportCommandKind::SetLoop;
      loop.loopEnabled = true;
      loop.loopStart = 0;
      loop.loopEnd = cc.loop;
      tl.apply(loop);
      tl.apply(play(0));
      int64_t clock = 0;
      auto block = [&] {
        tl.process(clock, kBlock);
        clock += kBlock;
      };
      for (int i = 0; i < 100; ++i) block();
      const pq::bench::Stats s = c.measure(block, 1, "ns/block", "per block");
      pq::bench::Stats per = s;
      const double n = 64.0 * kBlock;
      per.medianNs /= n;
      per.minNs /= n;
      per.p99Ns /= n;
      per.maxNs /= n;
      c.record(per, "ns/track-frame", "per track-frame");
      std::printf("INFO   clips/64 %s: %.2f %% of a core; %llu voices started; %llu frames read from mips\n", cc.name,
                  100.0 * s.medianNs / (1e9 * kBlock / kRate),
                  static_cast<unsigned long long>(tl.clipPlayer().stats().voicesStarted),
                  static_cast<unsigned long long>(tl.clipPlayer().stats().mipFrames));
    });
  }

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
                             AutoCase{"curved", 0.0f, 1.0f, 0.6f, false}, AutoCase{"curved, tempo ramp", 0.0f, 1.0f, 0.6f, true}})
  for (const bool perSample : {true, false}) {
    r.add(std::string("automation/") + ac.name + (perSample ? "" : ", block rate"), [ac, perSample](pq::bench::Context& c) {
      e::Timeline tl(config(1));
      std::vector<e::AutomationLaneDesc> lanes(32);
      for (uint32_t l = 0; l < 32; ++l) {
        lanes[l].target = l;
        lanes[l].points = {{0, ac.v0, ac.curve}, {100000 * kBar, ac.v1, 0.0f}};
        lanes[l].perSample = perSample;
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
