// T6.4: live note input in the engine (pq/engine/live_input.h): routing per (port, channel), the stamp → frame map
// (exact at any block size, smoothed under callback jitter), held notes (re-routes, sustain, retrigger, All Notes Off,
// ring overflow), pitch bend, the computer keyboard's table, the record tap, 10k events/s for 60 s without a drop or a
// stuck note, the Engine merge (timeline + auditions + live), and RtScope cleanliness; T6.9: the capture rings.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/rt_scope.h"
#include "pq/engine/block_adapter.h"
#include "pq/engine/engine.h"
#include "pq/engine/live_input.h"

namespace {

namespace e = pq::engine;
using e::LiveInput;
using e::LivePitch;
using e::RawMidi;

constexpr double kSr = 48000.0;

LivePitch pitchOf(int32_t key, float transpose = 0.0f) {
  LivePitch p;
  p.id = pq::tuning::PitchId::fromDegree(((key - 60) % 12 + 12) % 12, (key - 60 >= 0 ? (key - 60) / 12 : (key - 71) / 12));
  p.hz = static_cast<float>(440.0 * std::exp2((key - 69 + transpose) / 12.0));
  p.noteDegree = static_cast<float>(key - 60);
  return p;
}

e::LiveTrackDesc track(uint32_t slot, uint32_t sources, uint8_t channel = 0, float transpose = 0.0f, bool keyboard = false) {
  e::LiveTrackDesc t;
  t.slot = slot;
  t.sources = sources;
  t.channel = channel;
  for (int32_t k = 0; k < 128; ++k) t.keys.push_back(pitchOf(k, transpose));
  if (keyboard)
    for (uint32_t i = 0; i < e::kKeyboardKeys; ++i) {
      const int32_t d = e::keyboardEquave(static_cast<uint8_t>(i)) * 12 + e::keyboardStep(static_cast<uint8_t>(i));
      t.keyboard.push_back(pitchOf(60 + d, transpose));
    }
  return t;
}

std::unique_ptr<const e::LiveRouteImage> image(std::vector<e::LiveTrackDesc> tracks) {
  e::LiveRouteDesc d;
  d.tracks = std::move(tracks);
  auto r = e::LiveRouteImage::build(d);
  REQUIRE(r.ok());
  return std::move(r.value());
}

RawMidi msg(uint8_t port, uint8_t status, uint8_t d1, uint8_t d2, int64_t stamp = 0) {
  RawMidi m;
  m.stamp = stamp;
  m.port = port;
  m.status = status;
  m.d1 = d1;
  m.d2 = d2;
  return m;
}

// A stand-alone input with the host clock in samples (clockHz = the sample rate), so stamps are frames.
struct Rig {
  std::shared_ptr<e::LivePorts> ports;
  std::unique_ptr<LiveInput> in;
  int64_t now = 0;
  Rig(uint32_t ring = 4096, uint32_t maxHeld = 1024) {
    ports = std::make_shared<e::LivePorts>(ring);
    e::LiveInputConfig c;
    c.sampleRate = kSr;
    c.clockHz = kSr;
    c.maxHeld = maxHeld;
    in = std::make_unique<LiveInput>(c, ports);
  }
  void push(const RawMidi& m) { REQUIRE(ports->push(m.port, m)); }
  // One block: process, then the events of each slot in `slots`.
  std::map<uint32_t, std::vector<e::NoteEvent>> block(uint32_t frames, std::initializer_list<uint32_t> slots,
                                                       int64_t hostTime = 0) {
    LiveInput::Block b;
    b.start = now;
    b.frames = frames;
    b.hostTime = hostTime;
    std::map<uint32_t, std::vector<e::NoteEvent>> out;
    e::EventBuffer buf(256);
    {
      pq::RtScope rt;
      in->process(b);
    }
    for (uint32_t s : slots) {
      buf.clear();
      {
        pq::RtScope rt;
        in->emit(s, now, frames, buf);
      }
      for (uint32_t i = 0; i < buf.size(); ++i) out[s].push_back(buf.list().events[i]);
    }
    now += frames;
    return out;
  }
};

}  // namespace

TEST_CASE("live input: routes by port and channel; note-off reaches the slot of its note-on after a re-route") {
  Rig r;
  // Slot 3: every port, every channel. Slot 5: port 1, channel 2 only, a semitone up.
  r.in->publish(image({track(3, (1u << e::kLivePorts) - 1), track(5, 1u << 1, 2, 1.0f)}));
  r.push(msg(0, 0x90, 60, 100));        // port 0 ch 1 → slot 3 only
  r.push(msg(1, 0x91, 64, 80));         // port 1 ch 2 → slots 3 and 5
  auto ev = r.block(64, {3, 5});
  REQUIRE(ev[3].size() == 2);
  REQUIRE(ev[5].size() == 1);
  CHECK(ev[3][0].kind == e::NoteEventKind::NoteOn);
  CHECK(ev[3][0].hz == doctest::Approx(261.6256f));
  CHECK(ev[3][0].velocity == doctest::Approx(100.0f / 127.0f));
  CHECK(ev[3][0].noteId >= 0xE0000000u);
  CHECK(ev[3][0].noteId < 0xF0000000u);
  CHECK(ev[3][1].hz == doctest::Approx(329.6276f));
  CHECK(ev[5][0].hz == doctest::Approx(349.2282f));  // the transposed table
  CHECK(r.in->heldNotes() == 3);
  // Re-routed: nothing listens any more. The note-offs still go where the notes are.
  r.in->publish(image({}));
  r.push(msg(0, 0x80, 60, 0));
  r.push(msg(1, 0x91, 64, 0));  // velocity 0 = note-off
  ev = r.block(64, {3, 5});
  REQUIRE(ev[3].size() == 2);
  REQUIRE(ev[5].size() == 1);
  CHECK(ev[3][0].kind == e::NoteEventKind::NoteOff);
  CHECK(ev[5][0].kind == e::NoteEventKind::NoteOff);
  CHECK(r.in->heldNotes() == 0);
  CHECK(r.in->idle() == false);  // emitted events are dropped at the next block
  r.block(64, {});
  CHECK(r.in->idle());
  r.in->collect(false);
}

TEST_CASE("live input: a stamped event plays at stamp + latency, to the frame, at any block size and block offset") {
  for (const uint32_t bs : {1u, 7u, 32u, 64u, 100u, 256u, 1024u}) {
    for (const uint32_t perCallback : {1u, 4u}) {  // engine blocks per device callback (a BlockAdapter's direct mode)
      CAPTURE(bs);
      CAPTURE(perCallback);
      Rig r;
      r.in->publish(image({track(0, 1u)}));
      const uint32_t latency = bs * perCallback;  // one device period
      r.in->setLatency(latency);
      constexpr int64_t kHost0 = 1'000'000;  // host clock = engine time + kHost0 (in frames)
      std::mt19937 rng(bs * 31 + perCallback);
      std::vector<int64_t> want, got;
      int64_t nextStamp = 3;
      bool on = true;
      for (uint32_t cb = 0; cb < 4000 / bs + 40; ++cb) {
        const int64_t devStart = r.now;
        // Everything stamped before this callback started reaches it.
        while (nextStamp < devStart) {
          r.push(msg(0, on ? 0x90 : 0x80, 60, 100, kHost0 + nextStamp));
          want.push_back(nextStamp + latency);
          on = !on;
          nextStamp += 1 + static_cast<int64_t>(rng() % (2 * bs + 5));
        }
        for (uint32_t k = 0; k < perCallback; ++k) {
          LiveInput::Block b;
          b.start = r.now;
          b.frames = bs;
          b.hostTime = kHost0 + devStart;
          b.hostOffset = static_cast<int32_t>(k * bs);
          r.in->process(b);
          e::EventBuffer buf(256);
          r.in->emit(0, r.now, bs, buf);
          for (uint32_t i = 0; i < buf.size(); ++i) got.push_back(r.now + buf.list().events[i].frame);
          r.now += bs;
        }
      }
      REQUIRE(got.size() <= want.size());
      want.resize(got.size());
      CHECK(got.size() > 30);
      CHECK(got == want);
      CHECK(r.in->stats().lateEvents == 0);
    }
  }
}

TEST_CASE("live input: the clock filter smooths callback jitter (constant latency within a fraction of the jitter)") {
  Rig r;
  r.in->publish(image({track(0, 1u)}));
  constexpr uint32_t bs = 64;
  r.in->setLatency(256);
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> jitter(-48, 48);  // ±1 ms of callback jitter
  constexpr int64_t kHost0 = 5'000'000;
  double worst = 0.0;
  int64_t stamp = 100;
  bool on = true;
  for (int cb = 0; cb < 20000; ++cb) {
    const int64_t devStart = r.now;
    std::vector<int64_t> due;
    while (stamp < devStart - 48) {
      r.push(msg(0, on ? 0x90 : 0x80, 60, 100, kHost0 + stamp));
      due.push_back(stamp + 256);
      on = !on;
      stamp += 97;
    }
    LiveInput::Block b;
    b.start = r.now;
    b.frames = bs;
    b.hostTime = kHost0 + devStart + jitter(rng);  // the callback ran a little early or late
    r.in->process(b);
    if (cb > 2000)  // after the filter settled
      for (int64_t s : due) worst = std::max(worst, std::fabs(static_cast<double>(r.in->engineTimeOf(kHost0 + s) - s)));
    e::EventBuffer buf(256);
    r.in->emit(0, r.now, bs, buf);
    r.now += bs;
  }
  MESSAGE("worst schedule error after settling: " << worst << " frames (callback jitter ±48)");
  CHECK(worst <= 16.0);
}

TEST_CASE("live input: sustain pedal, retrigger, All Notes Off") {
  Rig r;
  r.in->publish(image({track(2, 1u)}));
  r.push(msg(0, 0xB0, 64, 127));  // pedal down
  r.push(msg(0, 0x90, 60, 100));
  r.push(msg(0, 0x80, 60, 0));    // held by the pedal
  auto ev = r.block(64, {2});
  REQUIRE(ev[2].size() == 1);
  CHECK(r.in->heldNotes() == 1);
  r.push(msg(0, 0x90, 60, 90));  // struck again: the sustained note ends first
  ev = r.block(64, {2});
  REQUIRE(ev[2].size() == 2);
  CHECK(ev[2][0].kind == e::NoteEventKind::NoteOff);
  CHECK(ev[2][1].kind == e::NoteEventKind::NoteOn);
  CHECK(ev[2][0].noteId != ev[2][1].noteId);
  r.push(msg(0, 0x80, 60, 0));
  r.push(msg(0, 0x90, 62, 90));
  r.push(msg(0, 0xB0, 64, 0));  // pedal up: the sustained 60 ends, 62 is still held
  ev = r.block(64, {2});
  CHECK(r.in->heldNotes() == 1);
  r.push(msg(0, 0xB0, 123, 0));  // All Notes Off
  ev = r.block(64, {2});
  REQUIRE(ev[2].size() == 1);
  CHECK(ev[2][0].kind == e::NoteEventKind::NoteOff);
  CHECK(r.in->heldNotes() == 0);
}

TEST_CASE("live input: pitch bend glides held notes from their key's Hz; a note-on under a bend starts bent") {
  Rig r;
  e::LiveRouteDesc d;
  d.tracks = {track(1, 1u)};
  d.bendSemitones[0] = 12.0f;
  auto img = e::LiveRouteImage::build(d);
  REQUIRE(img.ok());
  r.in->publish(std::move(img.value()));
  r.push(msg(0, 0x90, 69, 100));
  r.push(msg(0, 0xE0, 0, 0x60));  // (0x60 << 7) = 12288: +0.5 of the range = +6 semitones
  auto ev = r.block(64, {1});
  REQUIRE(ev[1].size() == 2);
  CHECK(ev[1][0].hz == doctest::Approx(440.0f));
  CHECK(ev[1][1].kind == e::NoteEventKind::Pitch);
  CHECK(ev[1][1].hz == doctest::Approx(440.0f * std::sqrt(2.0f)));
  CHECK(ev[1][1].rampFrames == 96);  // 2 ms
  r.push(msg(0, 0x90, 57, 100));     // under the bend
  ev = r.block(64, {1});
  REQUIRE(ev[1].size() == 1);
  CHECK(ev[1][0].hz == doctest::Approx(220.0f * std::sqrt(2.0f)));
  r.push(msg(0, 0xE0, 0, 0x40));  // centre: both back to their keys
  ev = r.block(64, {1});
  REQUIRE(ev[1].size() == 2);
  CHECK(ev[1][0].hz == doctest::Approx(440.0f));
  CHECK(ev[1][1].hz == doctest::Approx(220.0f));
}

TEST_CASE("live input: a ring overflow releases the port's held notes (never a stuck note)") {
  Rig r(16);
  r.in->publish(image({track(4, 3u)}));
  r.push(msg(0, 0x90, 60, 100));
  r.push(msg(1, 0x90, 61, 100));
  r.block(64, {4});
  CHECK(r.in->heldNotes() == 2);
  for (int i = 0; i < 40; ++i) (void)r.ports->push(0, msg(0, 0xB0, 1, static_cast<uint8_t>(i)));  // a flood: 24 lost
  CHECK(r.ports->overflows(0) == 24);
  auto ev = r.block(64, {4});
  REQUIRE(ev[4].size() == 1);
  CHECK(ev[4][0].kind == e::NoteEventKind::NoteOff);
  CHECK(r.in->heldNotes() == 1);  // port 1's note is untouched
  CHECK(r.in->stats().overflowReleases == 1);
}

TEST_CASE("live input: stale events (rings filled while the device was stopped) release but never start notes") {
  Rig r;
  r.in->publish(image({track(0, 1u)}));
  LiveInput::Block b;
  b.frames = 64;
  b.hostTime = 1000;
  r.in->process(b);  // anchored: host 1000 = engine 0
  r.push(msg(0, 0x90, 60, 100, 1000 + 10));
  r.now = 64;
  r.block(64, {0}, 1064);
  CHECK(r.in->heldNotes() == 1);
  // Two seconds later (the device was stopped): a note-on and the note-off of the held note, both stamped long ago.
  r.push(msg(0, 0x90, 64, 100, 1000 + 200));
  r.push(msg(0, 0x80, 60, 0, 1000 + 300));
  r.now = 2 * 48000;
  auto ev = r.block(64, {0}, 1000 + 2 * 48000);
  REQUIRE(ev[0].size() == 1);
  CHECK(ev[0][0].kind == e::NoteEventKind::NoteOff);
  CHECK(ev[0][0].frame == 0);
  CHECK(r.in->heldNotes() == 0);
  CHECK(r.in->stats().droppedOns == 1);
}

TEST_CASE("live input: the computer keyboard source plays its own table; the record tap carries raw data") {
  Rig r;
  r.in->publish(image({track(7, 1u << e::kLiveKeyboard, 0, 0.0f, true)}));
  r.in->setRecording(true);
  r.push(msg(e::kLiveKeyboard, 0x90, e::keyboardKey(1, 4), 90));  // equave +1, step 4: E4 in this 12-TET table
  auto ev = r.block(64, {7});
  REQUIRE(ev[7].size() == 1);
  CHECK(ev[7][0].hz == doctest::Approx(659.2551f));
  e::LiveNoteRecord rec;
  REQUIRE(r.in->popRecord(rec));
  CHECK(rec.kind == e::LiveNoteRecord::Kind::On);
  CHECK(rec.slot == 7);
  CHECK(rec.velocity == 90.0f);
  CHECK(rec.raw.d1 == e::keyboardKey(1, 4));
  CHECK(rec.pitch.hz == ev[7][0].hz);
  CHECK(rec.songSample == -1);
  r.in->setRecording(false);
  r.push(msg(e::kLiveKeyboard, 0x80, e::keyboardKey(1, 4), 64));
  r.block(64, {7});
  CHECK_FALSE(r.in->popRecord(rec));  // off: nothing written
}

TEST_CASE("live input: 10k events/s for 60 s through 4 ports into 3 slots: no drop, no stuck note, RtScope-clean") {
  Rig r;
  r.in->publish(image({track(0, 0xFu), track(1, 0x3u), track(2, 0xCu, 1)}));
  std::mt19937 rng(42);
  constexpr uint32_t bs = 64;
  constexpr int kBlocks = static_cast<int>(60 * kSr / bs);
  const uint64_t violations0 = pq::alloc_trap::violationCount();
  std::map<uint32_t, std::set<uint32_t>> open;  // slot → sounding note ids
  uint64_t ons = 0, offs = 0, sent = 0;
  std::vector<std::vector<bool>> down(4, std::vector<bool>(128, false));
  double carry = 0.0;
  for (int blk = 0; blk < kBlocks; ++blk) {
    carry += 10000.0 * bs / kSr;
    for (; carry >= 1.0; carry -= 1.0) {
      const auto port = static_cast<uint8_t>(rng() % 4);
      const auto key = static_cast<uint8_t>(36 + rng() % 48);
      const uint32_t kind = rng() % 10;
      if (kind == 0) r.push(msg(port, 0xE0, 0, static_cast<uint8_t>(rng() % 128)));
      else if (down[port][key]) {
        r.push(msg(port, 0x80, key, 0));
        down[port][key] = false;
      } else {
        r.push(msg(port, 0x90, key, static_cast<uint8_t>(1 + rng() % 127)));
        down[port][key] = true;
      }
      ++sent;
    }
    auto ev = r.block(bs, {0, 1, 2});
    for (auto& [slot, list] : ev)
      for (const e::NoteEvent& n : list) {
        if (n.kind == e::NoteEventKind::NoteOn) {
          CHECK(open[slot].insert(n.noteId).second);
          ++ons;
        } else if (n.kind == e::NoteEventKind::NoteOff) {
          CHECK(open[slot].erase(n.noteId) == 1);
          ++offs;
        } else if (n.kind == e::NoteEventKind::Pitch) {
          CHECK(open[slot].count(n.noteId) == 1);
        }
      }
  }
  for (uint8_t p = 0; p < 4; ++p)  // release what is still down
    for (uint8_t k = 0; k < 128; ++k)
      if (down[p][k]) r.push(msg(p, 0x80, k, 0));
  for (auto& [slot, list] : r.block(bs, {0, 1, 2}))
    for (const e::NoteEvent& n : list)
      if (n.kind == e::NoteEventKind::NoteOff) {
        CHECK(open[slot].erase(n.noteId) == 1);
        ++offs;
      }
  r.block(bs, {});
  MESSAGE(sent << " messages, " << ons << " note-ons, " << offs << " note-offs");
  CHECK(sent >= 600000);
  CHECK(ons == offs);
  for (auto& [slot, ids] : open) CHECK(ids.empty());
  CHECK(r.in->heldNotes() == 0);
  CHECK(r.in->idle());
  CHECK(r.in->stats().droppedOns == 0);
  for (uint32_t p = 0; p < 4; ++p) CHECK(r.ports->overflows(p) == 0);
  CHECK(pq::alloc_trap::violationCount() == violations0);
}

TEST_CASE("live input: a producer thread at 10k events/s while the audio side runs (SPSC, nothing lost)") {
  Rig r;
  r.in->publish(image({track(0, 1u)}));
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> pushed{0};
  std::thread producer([&] {
    bool on = true;
    while (!stop.load()) {
      for (int i = 0; i < 10; ++i) {
        while (!r.ports->push(0, msg(0, on ? 0x90 : 0x80, 60, 100)))
          if (stop.load()) return;
        on = !on;
        pushed.fetch_add(1);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  uint64_t ons = 0, offs = 0;
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500)) {
    for (auto& [slot, list] : r.block(64, {0}))
      for (const e::NoteEvent& n : list) (n.kind == e::NoteEventKind::NoteOn ? ons : offs) += 1;
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  stop = true;
  producer.join();
  for (int i = 0; i < 4; ++i)
    for (auto& [slot, list] : r.block(64, {0}))
      for (const e::NoteEvent& n : list) (n.kind == e::NoteEventKind::NoteOn ? ons : offs) += 1;
  CHECK(ons + offs == pushed.load());
  CHECK(ons - offs <= 1);
  CHECK(r.in->stats().messages == pushed.load());
}

TEST_CASE("live input: Engine merges live notes into the slot after the timeline and auditions") {
  e::EngineConfig ec;
  ec.sampleRate = kSr;
  ec.blockFrames = 64;
  ec.maxSlots = 8;
  ec.sleepAfterMs = 10.0f;
  ec.live.clockHz = kSr;
  auto eng = e::Engine::create(ec);
  REQUIRE(eng.ok());
  e::Engine& en = **eng;
  en.liveInput().publish(image({track(2, 1u)}));
  e::ProjectConfig pc = en.projectConfig();
  REQUIRE(pc.eventSources.size() == 8);
  float l[64], rr[64];
  float* outs[2] = {l, rr};
  auto run = [&](int64_t hostTime) {
    pq::RtScope rt;
    e::AudioBlock b{nullptr, 0, outs, 2, 64, 0, hostTime, 0};
    en.process(b);
  };
  run(0);
  REQUIRE(en.audition(2, 330.0f, 100.0f, 48000));
  REQUIRE(en.liveInput().ports().push(0, msg(0, 0x90, 69, 127)));
  run(0);
  e::EventBuffer buf(64);
  {
    pq::RtScope rt;
    pc.eventSources[2].fn(pc.eventSources[2].user, en.time() - 64, 64, buf);
  }
  REQUIRE(buf.size() == 2);
  CHECK(buf.list().events[0].hz == 330.0f);  // the audition first
  CHECK(buf.list().events[1].hz == doctest::Approx(440.0f));
  CHECK(buf.list().events[1].noteId >= 0xE0000000u);
  // A held live note keeps the input busy (the idle gate's quiet needs live input idle).
  for (int i = 0; i < 4; ++i) run(0);
  CHECK(en.liveInput().heldNotes() == 1);
  CHECK_FALSE(en.liveInput().idle());
}

TEST_CASE("live input: BlockAdapter hands each engine block its callback's host time and output offset") {
  struct Seen {
    std::vector<std::pair<int64_t, int32_t>> v;
  } seen;
  e::BlockAdapter direct, fifo;
  auto cb = e::AudioCallback{[](void* u, const e::AudioBlock& b) noexcept {
                               static_cast<Seen*>(u)->v.emplace_back(b.hostTime, b.hostOffset);
                               for (uint32_t c = 0; c < b.numOut; ++c) std::fill(b.out[c], b.out[c] + b.frames, 0.0f);
                             },
                             &seen};
  REQUIRE(direct.prepare(cb, 0, 1, 32, 128));
  std::vector<float> o(128);
  float* op[1] = {o.data()};
  direct.process(e::AudioBlock{nullptr, 0, op, 1, 128, 0, 777, 0});
  REQUIRE(seen.v.size() == 4);
  for (int i = 0; i < 4; ++i) CHECK(seen.v[i] == std::pair<int64_t, int32_t>{777, 32 * i});
  seen.v.clear();
  REQUIRE(fifo.prepare(cb, 0, 1, 32, 48));
  fifo.process(e::AudioBlock{nullptr, 0, op, 1, 48, 0, 900, 0});
  REQUIRE(seen.v.size() == 1);
  CHECK(seen.v[0] == std::pair<int64_t, int32_t>{900, 32});  // its output plays from device frame 32 on
}

TEST_CASE("live input: an idle block costs a load, not a ring scan (CPU)") {
  Rig r;
  r.in->publish(image({track(0, 0xFFFFu), track(1, 0x1FFFFu)}));
  LiveInput::Block b;
  b.frames = 64;
  constexpr int kBlocks = 1'000'000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kBlocks; ++i) {
    b.start = static_cast<int64_t>(i) * 64;
    b.hostTime = 1000 + b.start;
    r.in->process(b);
  }
  const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kBlocks;
  MESSAGE("idle LiveInput::process: " << ns << " ns per block (17 sources, 2 routed tracks)");
  CHECK(ns < 500.0);  // loose (a shared, loaded machine); the target is <= 20 ns optimised
  CHECK(r.in->stats().messages == 0);
}

// T6.9: Capture MIDI's ring, written by the audio thread (RtScope: no allocation on the capture-write path).
TEST_CASE("live input: capture rings keep every note a listening track plays, always (RtScope on the write path)") {
  Rig r;
  auto ring = std::make_shared<e::CaptureRing>(8);
  auto t = track(3, 1u);
  t.capture = ring;
  r.in->publish(image({t, track(4, 1u)}));  // slot 4 listens too, without a ring
  r.push(msg(0, 0x90, 64, 100));
  r.block(64, {3, 4});  // RtScope inside
  {
    // While the transport runs: the note-on's song sample (block-accurate, as the record tap's).
    e::LiveInput::Block b;
    b.start = r.now;
    b.frames = 64;
    b.playing = true;
    b.songSample = 1000;
    r.push(msg(0, 0x91, 67, 80));
    r.push(msg(0, 0x80, 64, 30));  // ends the first note
    pq::RtScope rt;
    r.in->process(b);
  }
  r.now += 64;
  CHECK(r.in->engineNow() == 128);
  std::vector<e::CapturedNote> got;
  ring->read(0, got);
  REQUIRE(got.size() == 2);
  CHECK(got[0].seq == 0);
  CHECK(got[0].engineTime == 0);
  CHECK(got[0].songSample == -1);  // stopped
  CHECK(got[0].endTime == 64);
  CHECK(got[0].release == doctest::Approx(30.0f));
  CHECK(got[0].velocity == 100.0f);
  CHECK(got[0].id == pitchOf(64).id);
  CHECK(got[0].raw.d1 == 64);
  CHECK(got[1].engineTime == 64);
  CHECK(got[1].songSample == 1000);
  CHECK(got[1].endTime == e::kCaptureOpen);  // still held
  CHECK(got[1].raw.status == 0x91);
  CHECK(ring->written() == 2);
  // Read from a mark: only what came after.
  got.clear();
  ring->read(1, got);
  REQUIRE(got.size() == 1);
  CHECK(got[0].seq == 1);
  // The track stops listening while its note is held: the note-off no longer reaches the ring (the image that kept it
  // alive may be freed), and nothing breaks.
  r.in->publish(image({track(4, 1u)}));
  r.block(64, {3, 4});
  r.push(msg(0, 0x81, 67, 0));
  r.block(64, {3, 4});
  r.in->collect(false);
  got.clear();
  ring->read(1, got);
  REQUIRE(got.size() == 1);
  CHECK(got[0].endTime == e::kCaptureOpen);
  // Overwrite: a ring of 8 keeps the last 8 notes.
  r.in->publish(image({t}));
  for (int i = 0; i < 12; ++i) {
    r.push(msg(0, 0x90, static_cast<uint8_t>(40 + i), 90));
    r.push(msg(0, 0x80, static_cast<uint8_t>(40 + i), 90));
    r.block(64, {3});
  }
  got.clear();
  ring->read(0, got);
  REQUIRE(got.size() == 8);
  CHECK(got.front().seq == ring->written() - 8);
  CHECK(got.back().raw.d1 == 51);
  for (const e::CapturedNote& n : got) CHECK(n.endTime == n.engineTime + 1);  // on and off in one block: 1 frame long
}

TEST_CASE("live input: a capture ring read while the audio side writes keeps only whole notes") {
  e::CaptureRing ring(64);
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    RawMidi m;
    for (int64_t i = 0; !stop.load(std::memory_order_relaxed); ++i) {
      m.d1 = static_cast<uint8_t>(i & 127);
      m.stamp = i;
      const uint64_t q = ring.begin(i, i * 2, 1.0f, 0.0f, pq::tuning::PitchId::fromCents(static_cast<double>(i)), m);
      ring.end(q, i + 5, 0.0f);
    }
  });
  for (int k = 0; k < 2000; ++k) {
    std::vector<e::CapturedNote> got;
    ring.read(0, got);
    for (const e::CapturedNote& n : got) {
      REQUIRE(n.engineTime == static_cast<int64_t>(n.seq));
      REQUIRE(n.songSample == 2 * n.engineTime);
      REQUIRE(n.raw.stamp == n.engineTime);
      REQUIRE(n.id.cents() == static_cast<double>(n.engineTime));
      REQUIRE((n.endTime == e::kCaptureOpen || n.endTime == n.engineTime + 5));
    }
  }
  stop = true;
  writer.join();
}
