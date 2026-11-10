// T6.7: the recorder (pq/engine/recorder.h) on a timeline driven like the engine drives it: input frames captured
// bit-exact and placed on the song sample they were PLAYED against (a Run segment per song run, a loop wrap starts a new
// run; an Audio segment per captured piece), the punch window [in, out) of what was played for record latencies (device
// in + out + PDC + manual) 0 / 256 / 1000 + 480, at block sizes that do not divide anything; M6 review M6: with a loop
// (and the punch = the loop) every pass gets the frames played at its end that arrive after the wrap; overflow drops
// whole pieces (counted, the segment says so) and never blocks; M6 review M7: the device latency is frozen per session; Record `here`, RecordOff and Stop; the monitor gates
// (In / Off / Auto with the transport); RtScope on the audio side. M6 review M8: a WAV past its RIFF limit becomes RF64.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <memory>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/rt_scope.h"
#include "pq/core/time.h"
#include "pq/engine/recorder.h"
#include "pq/engine/timeline.h"
#include "pq/media/probe.h"
#include "pq/media/wav_file.h"

namespace e = pq::engine;
using pq::kPpq;
using pq::Ticks;

namespace {

constexpr double kSr = 48000.0;

// Input channel c at engine frame n: distinct, exactly representable values (bit-exact checks).
float source(uint32_t c, int64_t n) { return (c == 0 ? 1.0f : -1.0f) * static_cast<float>(n % 65536) * (1.0f / 65536.0f); }

struct Rig {
  e::Timeline tl;
  e::Recorder rec;
  std::vector<std::vector<float>> in;  // per channel, one block
  int64_t time = 0;
  explicit Rig(uint32_t maxBlock = 1024) : tl([&] {
    e::TimelineConfig c;
    c.sampleRate = kSr;
    c.maxBlockFrames = maxBlock;
    return c;
  }()) {}
  // One engine block of `frames` (≤ the timeline's max block) at the next engine time, as Engine::process + its hook.
  void block(uint32_t frames, uint32_t pdc = 0, bool rt = true) {
    in.assign(2, std::vector<float>(frames));
    for (uint32_t c = 0; c < 2; ++c)
      for (uint32_t f = 0; f < frames; ++f) in[c][f] = source(c, time + f);
    const float* ch[2] = {in[0].data(), in[1].data()};
    std::vector<float> o(frames);
    float* out[1] = {o.data()};
    const e::AudioBlock b{ch, 2, out, 1, frames, time};
    if (rt) {
      pq::RtScope scope;
      rec.beginBlock(b);
      tl.process(time, frames);
      rec.capture(tl, time, frames, pdc);
    } else {
      rec.beginBlock(b);
      tl.process(time, frames);
      rec.capture(tl, time, frames, pdc);
    }
    time += frames;
  }
};

struct Captured {
  std::vector<e::CaptureSegment> runs;        // Run segments
  std::vector<e::CaptureSegment> segs;        // Audio segments
  std::vector<std::vector<float>> frames{2};  // the captured frames, per channel, in order
};

Captured drain(Rig& r, e::CaptureSet& set) {
  Captured out;
  e::CaptureSegment s;
  while (r.rec.popSegment(s)) {
    if (s.kind == e::CaptureSegment::Run) {
      out.runs.push_back(s);
      continue;
    }
    out.segs.push_back(s);
    if (s.dropped != 0) continue;
    for (uint32_t c = 0; c < 2; ++c) {
      std::vector<float> x(s.frames);
      REQUIRE(set.ring(c).pop(x.data(), s.frames));
      out.frames[c].insert(out.frames[c].end(), x.begin(), x.end());
    }
  }
  return out;
}

e::TransportCommand cmd(e::TransportCommandKind k, Ticks t = 0) {
  e::TransportCommand c;
  c.kind = k;
  c.tick = t;
  return c;
}

}  // namespace

TEST_CASE("T6.7 recorder: inputs captured bit-exact with their song samples, loop wraps split passes, RtScope") {
  Rig r;
  const uint16_t chans[2] = {0, 1};
  auto set = std::make_shared<e::CaptureSet>(chans, 1u << 18, 7);
  r.rec.setCapture(set);
  r.block(64, 0, false);  // the recorder has taken the set
  // Loop [0, 1 bar) = 96 000 samples at 120 BPM; record from tick 0 for 2.3 loops with a block of 1000 (wraps inside
  // blocks).
  e::TransportCommand loop = cmd(e::TransportCommandKind::SetLoop);
  loop.loopEnabled = true;
  loop.loopStart = 0;
  loop.loopEnd = 4 * kPpq;
  r.tl.apply(loop);
  r.tl.apply(cmd(e::TransportCommandKind::Record, 0));
  const int64_t start = r.time;
  for (int i = 0; i < 221; ++i) r.block(1000);
  r.tl.apply(cmd(e::TransportCommandKind::Stop));
  r.block(1000);
  const Captured cap = drain(r, *set);
  REQUIRE_FALSE(cap.segs.empty());
  // The runs: the song-time map, contiguous in engine time, wrapping with the loop.
  int64_t expectEngine = start, runFrames = 0;
  uint32_t wraps = 0;
  for (const e::CaptureSegment& s : cap.runs) {
    CHECK(s.engineTime == expectEngine);
    CHECK(s.songSample == (s.engineTime - start) % 96000);
    if (s.songSample == 0 && s.engineTime != start) ++wraps;
    expectEngine = s.engineTime + s.frames;
    runFrames += s.frames;
  }
  CHECK(runFrames == 221000);
  CHECK(wraps == 2);
  // Every captured frame (no latency: played = captured): piece k's frame i was input at engine time engineTime + i
  // and played song sample songSample + i; the frames are the input's bit for bit, and song time follows the loop.
  size_t at = 0;
  int64_t frames = 0;
  expectEngine = start;
  for (const e::CaptureSegment& s : cap.segs) {
    CHECK(s.engineTime == expectEngine);
    CHECK(s.dropped == 0);
    CHECK(s.captureSet == 7);
    CHECK(s.songSample == (s.engineTime - start) % 96000);
    for (uint32_t i = 0; i < s.frames; ++i, ++at) {
      if (cap.frames[0][at] != source(0, s.engineTime + i) || cap.frames[1][at] != source(1, s.engineTime + i)) {
        FAIL("frame " << at << " differs");
      }
    }
    expectEngine = s.engineTime + s.frames;
    frames += s.frames;
  }
  CHECK(frames == 221000);
  CHECK(r.rec.stats().droppedFrames == 0);
  CHECK(r.rec.droppedFrames() == 0);
  // Stopped: nothing more.
  r.block(1000);
  e::CaptureSegment s;
  CHECK_FALSE(r.rec.popSegment(s));
}

TEST_CASE("T6.7 recorder: the punch keeps what was played in [in, out), captured the record latency later (0 / 256 / 1000, + PDC 480, + manual)") {
  const uint32_t latencies[3] = {0, 256, 1000};
  for (uint32_t lat : latencies) {
    for (uint32_t pdc : {0u, 480u}) {
      Rig r;
      const uint16_t chans[2] = {0, 1};
      auto set = std::make_shared<e::CaptureSet>(chans, 1u << 18, 1);
      r.rec.setCapture(set);
      r.rec.setDeviceLatency(lat / 2, lat - lat / 2);
      r.rec.setManualOffset(lat == 1000 ? -3 : 0);
      const int64_t off = r.rec.latencyOffset(pdc);
      CHECK(off == int64_t{lat} + pdc + (lat == 1000 ? -3 : 0));
      e::TransportCommand p = cmd(e::TransportCommandKind::SetPunch);
      p.punchEnabled = true;
      p.punchIn = kPpq;       // 24 000
      p.punchOut = 3 * kPpq;  // 72 000
      r.tl.apply(p);
      r.tl.apply(cmd(e::TransportCommandKind::Record, 0));
      const int64_t start = r.time;
      for (int i = 0; i < 150; ++i) r.block(777, pdc);
      const Captured cap = drain(r, *set);
      int64_t first = -1, last = -1, n = 0;
      size_t at = 0;
      for (const e::CaptureSegment& s : cap.segs) {
        for (uint32_t i = 0; i < s.frames; ++i, ++at) {
          const int64_t song = s.songSample + i;  // what it was played against
          if (first < 0) first = song;
          last = song;
          ++n;
          REQUIRE(cap.frames[0][at] == source(0, s.engineTime + i + off));  // captured `off` after it was played
          REQUIRE(song == s.engineTime + i - start);  // no loop: song = played engine time − start
        }
      }
      CHECK(first == 24000);
      CHECK(last == 72000 - 1);
      CHECK(n == 48000);
    }
  }
}

TEST_CASE("M6 review M7: a device latency that changes during a session (WASAPI round trip) moves nothing until the next") {
  Rig r;
  const uint16_t chans[2] = {0, 1};
  auto set = std::make_shared<e::CaptureSet>(chans, 1u << 18, 1);
  r.rec.setCapture(set);
  r.rec.setDeviceLatency(128, 128);
  r.tl.apply(cmd(e::TransportCommandKind::Record, 0));
  const int64_t start = r.time;
  for (int i = 0; i < 40; ++i) {
    if (i == 20) r.rec.setDeviceLatency(128 + 1440, 128);  // a re-primed round trip: +30 ms
    r.block(500);
  }
  Captured cap = drain(r, *set);
  int64_t n = 0, nextSong = 0;
  size_t at = 0;
  for (const e::CaptureSegment& s : cap.segs)
    for (uint32_t i = 0; i < s.frames; ++i, ++at, ++n) {
      REQUIRE(s.songSample + i == nextSong++);  // contiguous: no gap, no repeat
      REQUIRE(s.songSample + i == s.engineTime + i - start);
      REQUIRE(cap.frames[0][at] == source(0, s.engineTime + i + 256));  // one offset, the session's
    }
  CHECK(n == 40 * 500 - 256);
  // The next session takes the new latency.
  r.tl.apply(cmd(e::TransportCommandKind::Stop));
  r.block(500);
  (void)drain(r, *set);
  r.tl.apply(cmd(e::TransportCommandKind::Record, 0));
  const int64_t start2 = r.time;
  for (int i = 0; i < 10; ++i) r.block(500);
  cap = drain(r, *set);
  REQUIRE_FALSE(cap.segs.empty());
  CHECK(cap.segs.front().engineTime == start2);
  CHECK(cap.frames[0].front() == source(0, start2 + 1696));
}

TEST_CASE("T6.7 recorder: overflow drops whole runs (counted), Record here / RecordOff / Stop, sessions") {
  Rig r;
  const uint16_t chans[2] = {0, 1};
  auto set = std::make_shared<e::CaptureSet>(chans, 2048, 3);  // 2048 frames per channel: two 1000-frame runs
  r.rec.setCapture(set);
  r.tl.apply(cmd(e::TransportCommandKind::Play, 0));
  r.block(1000);
  e::CaptureSegment s;
  CHECK_FALSE(r.rec.popSegment(s));  // playing, not recording: nothing
  CHECK_FALSE(r.tl.recording());
  e::TransportCommand here = cmd(e::TransportCommandKind::Record, 0);
  here.here = true;
  r.tl.apply(here);  // records from where the playhead is (no locate)
  CHECK(r.tl.recording());
  const uint32_t session = r.tl.recordSession();
  for (int i = 0; i < 4; ++i) r.block(1000);
  r.tl.apply(cmd(e::TransportCommandKind::RecordOff));
  r.block(1000);
  CHECK(r.tl.playing());
  CHECK_FALSE(r.tl.recording());
  std::vector<e::CaptureSegment> segs;
  while (r.rec.popSegment(s))
    if (s.kind == e::CaptureSegment::Audio) segs.push_back(s);
  REQUIRE(segs.size() == 4);
  CHECK(segs[0].songSample == 1000);  // it went on from the playhead
  CHECK(segs[0].dropped == 0);
  CHECK(segs[1].dropped == 0);
  CHECK(segs[2].dropped == 1000);  // the rings were full: the piece is dropped whole, for every channel
  CHECK(segs[3].dropped == 1000);
  for (const auto& g : segs) CHECK(g.session == session);
  CHECK(r.rec.droppedFrames() == 2000);
  // The consumer read what the rings kept: exactly the first two runs.
  std::vector<float> x(2000);
  CHECK(set->ring(0).pop(x.data(), 2000));
  CHECK(x[0] == source(0, segs[0].engineTime));
  CHECK(x[1999] == source(0, segs[1].engineTime + 999));
  CHECK_FALSE(set->ring(0).pop(x.data(), 1));
  // A new Record is a new session; Stop ends it.
  r.tl.apply(cmd(e::TransportCommandKind::Record, 0));
  CHECK(r.tl.recordSession() == session + 1);
  r.tl.apply(cmd(e::TransportCommandKind::Stop));
  CHECK_FALSE(r.tl.recording());
  r.tl.apply(cmd(e::TransportCommandKind::Record, 0));
  r.tl.apply(cmd(e::TransportCommandKind::Play, 0));  // Play ends recording
  CHECK_FALSE(r.tl.recording());
}

// M6 review M6 (probe_record_link): with the loop on and the punch = the loop region, the frames played in the last
// `offset` frames of a pass arrive after the wrap; they belong to that pass (were lost: 95 744 of 96 000 per take).
TEST_CASE("T6.7 recorder: loop + punch = loop, latency 256: every pass keeps all it played, also the frames after the wrap") {
  for (const bool punch : {false, true}) {
    CAPTURE(punch);
    Rig r;
    const uint16_t chans[2] = {0, 1};
    auto set = std::make_shared<e::CaptureSet>(chans, 1u << 20, 2);
    r.rec.setCapture(set);
    r.rec.setDeviceLatency(128, 128);
    r.block(64, 0, false);
    e::TransportCommand loop = cmd(e::TransportCommandKind::SetLoop);
    loop.loopEnabled = true;
    loop.loopStart = 0;
    loop.loopEnd = 4 * kPpq;  // 96 000 samples
    r.tl.apply(loop);
    if (punch) {
      e::TransportCommand p = cmd(e::TransportCommandKind::SetPunch);
      p.punchEnabled = true;
      p.punchIn = 0;
      p.punchOut = 4 * kPpq;
      r.tl.apply(p);
    }
    r.tl.apply(cmd(e::TransportCommandKind::Record, 0));
    const int64_t start = r.time;
    for (int i = 0; i < 384; ++i) r.block(1000);  // 4 passes exactly
    r.tl.apply(cmd(e::TransportCommandKind::Stop));
    r.block(1000);
    const Captured cap = drain(r, *set);
    // Per pass (by the engine time it was played at): the song samples it got, and every frame's value.
    std::vector<int64_t> got(4, 0), next(4, 0);
    size_t at = 0;
    for (const e::CaptureSegment& s : cap.segs) {
      const int64_t pass = (s.engineTime - start) / 96000;
      REQUIRE(pass < 4);
      CHECK(s.songSample == next[static_cast<size_t>(pass)]);  // contiguous within its pass, from the loop start
      for (uint32_t i = 0; i < s.frames; ++i, ++at)
        if (cap.frames[0][at] != source(0, s.engineTime + i + 256)) FAIL("frame " << at << " differs");
      got[static_cast<size_t>(pass)] += s.frames;
      next[static_cast<size_t>(pass)] = s.songSample + s.frames;
    }
    CHECK(got[0] == 96000);
    CHECK(got[1] == 96000);
    CHECK(got[2] == 96000);
    CHECK(got[3] == 96000 - 256);  // Stop: the last 256 frames played were not captured yet
  }
}

// M6 review M8: a take past RIFF's 4 GiB becomes RF64 instead of stopping (a small limit stands in for 4 GiB here).
TEST_CASE("T6.7 WavWriter: past its RIFF limit a recording WAV becomes RF64, readable whole, crash-safe patches included") {
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "pq_m8_rf64_take.wav";
  std::filesystem::remove(path);
  {
    pq::media::WavWriter w;
    w.enableRf64(100000);  // the RIFF size past which it becomes RF64
    REQUIRE(w.open(path, 2, 48000, 4800).ok());
    std::vector<float> a(1000), b(1000);
    int64_t n = 0;
    for (int blk = 0; blk < 30; ++blk) {  // 240 kB of samples
      for (int f = 0; f < 1000; ++f, ++n) {
        a[f] = source(0, n);
        b[f] = source(1, n);
      }
      const float* ch[2] = {a.data(), b.data()};
      REQUIRE(w.write(ch, 1000).ok());
    }
    CHECK(w.isRf64());
    // A crash-safe patch past the limit: a reader sees an RF64 file of the patched frames.
    auto mid = pq::media::readWav(path);
    REQUIRE_MESSAGE(mid.ok(), (mid.ok() ? std::string() : mid.error().message));
    CHECK(mid.value().frames() == w.framesPatched());
    REQUIRE(w.close().ok());
  }
  auto info = pq::media::probe(path);
  REQUIRE_MESSAGE(info.ok(), (info.ok() ? std::string() : info.error().message));
  CHECK(info.value().container == pq::media::Container::Rf64);
  CHECK(info.value().frames == 30000);
  auto whole = pq::media::readWav(path);
  REQUIRE(whole.ok());
  REQUIRE(whole.value().frames() == 30000);
  for (int64_t i = 0; i < 30000; ++i)
    if (whole.value().channels[0][i] != source(0, i) || whole.value().channels[1][i] != source(1, i)) FAIL("frame " << i);
  // Below the limit it stays a plain RIFF WAV (with a JUNK chunk where ds64 would go).
  {
    pq::media::WavWriter w;
    w.enableRf64();
    REQUIRE(w.open(path, 1, 48000).ok());
    std::vector<float> x(100, 0.25f);
    const float* ch[1] = {x.data()};
    REQUIRE(w.write(ch, 100).ok());
    REQUIRE(w.close().ok());
    CHECK_FALSE(w.isRf64());
  }
  auto small = pq::media::probe(path);
  REQUIRE(small.ok());
  CHECK(small.value().container == pq::media::Container::Wav);
  CHECK(small.value().frames == 100);
  std::filesystem::remove(path);
}

TEST_CASE("T6.7 recorder: monitor gates (In 1, Off 0, Auto = armed and (stopped or recording or no clip)), RtScope") {
  Rig r;
  auto img = std::make_shared<e::MonitorImage>();
  img->entries.push_back({0, e::MonitorEntry::In, false, 0});
  img->entries.push_back({1, e::MonitorEntry::Off, true, 1});
  img->entries.push_back({2, e::MonitorEntry::Auto, true, 2});
  img->entries.push_back({3, e::MonitorEntry::Auto, false, 3});
  r.rec.setMonitors(img);
  std::vector<float> params(4, 0.5f);
  r.block(64);
  {
    pq::RtScope scope;
    CHECK(r.rec.monitor(r.tl, params));
  }
  CHECK(params == std::vector<float>{1.0f, 0.0f, 1.0f, 0.0f});
  CHECK(r.rec.monitoring());
  CHECK_FALSE(r.rec.monitor(r.tl, params));  // unchanged: no write
  r.tl.apply(cmd(e::TransportCommandKind::Play, 0));
  r.block(64);
  CHECK_FALSE(r.rec.monitor(r.tl, params));  // playing, no clip sounding on slot 2: the input stays monitored
  CHECK(params[2] == 1.0f);
  // Without an image nothing is written, and nothing is monitored.
  r.rec.setMonitors(nullptr);
  r.block(64);
  params.assign(4, 0.5f);
  CHECK_FALSE(r.rec.monitor(r.tl, params));
  CHECK(params[0] == 0.5f);
  CHECK_FALSE(r.rec.monitoring());
  r.rec.collect(false);
}

TEST_CASE("T6.7 WavWriter: a take killed mid-way is a readable WAV up to the last patch (every second)") {
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "pq_t67_killed_take.wav";
  std::filesystem::remove(path);
  {
    pq::media::WavWriter w;
    REQUIRE(w.open(path, 2, 48000, 48000).ok());
    std::vector<float> a(1000), b(1000);
    int64_t n = 0;
    for (int blk = 0; blk < 125; ++blk) {  // 2.6 s
      for (int f = 0; f < 1000; ++f, ++n) {
        a[f] = source(0, n);
        b[f] = source(1, n);
      }
      const float* ch[2] = {a.data(), b.data()};
      REQUIRE(w.write(ch, 1000).ok());
    }
    CHECK(w.framesPatched() == 96000);  // patched at 1 s and 2 s
    // "Killed" now: what is on disk is what a reader sees while the writer still holds the file (nothing more is
    // flushed or patched by a dead process). It reads as a WAV of the frames up to the last patch, bit for bit.
    auto r = pq::media::readWav(path);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    REQUIRE(r.value().numChannels() == 2);
    REQUIRE(r.value().frames() == 96000);
    for (int64_t i = 0; i < 96000; ++i)
      if (r.value().channels[0][i] != source(0, i) || r.value().channels[1][i] != source(1, i)) FAIL("frame " << i);
    // The writer is never told: its destructor closes the file whole (2.6 s) — what a clean stop gives.
  }
  auto whole = pq::media::readWav(path);
  REQUIRE(whole.ok());
  CHECK(whole.value().frames() == 125000);
  std::filesystem::remove(path);
}
