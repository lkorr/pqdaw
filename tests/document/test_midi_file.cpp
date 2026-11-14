// MF.1: Standard MIDI File reader / writer and note resolution (pq/document/midi_file.h).
#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

#include "pq/document/midi_file.h"

using namespace pq::doc;
using pq::Result;

namespace {

using Bytes = std::vector<uint8_t>;

void put32(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}
void putLe32(Bytes& b, uint32_t v) {
  for (int s = 0; s <= 24; s += 8) b.push_back(uint8_t(v >> s));
}
void putVlq(Bytes& b, uint32_t v) {
  uint8_t buf[5];
  int n = 0;
  buf[n++] = uint8_t(v & 127);
  while ((v >>= 7) != 0) buf[n++] = uint8_t(0x80 | (v & 127));
  while (n > 0) b.push_back(buf[--n]);
}

// A hand-written track body: every call appends a delta time and the event's bytes, status written out (raw() for
// running status and anything else).
struct T {
  Bytes b;
  T& raw(uint32_t dt, std::initializer_list<uint8_t> bytes) {
    putVlq(b, dt);
    b.insert(b.end(), bytes);
    return *this;
  }
  T& on(uint32_t dt, uint8_t ch, uint8_t key, uint8_t vel = 100) { return raw(dt, {uint8_t(0x90 | ch), key, vel}); }
  T& off(uint32_t dt, uint8_t ch, uint8_t key, uint8_t vel = 64) { return raw(dt, {uint8_t(0x80 | ch), key, vel}); }
  T& cc(uint32_t dt, uint8_t ch, uint8_t c, uint8_t v) { return raw(dt, {uint8_t(0xB0 | ch), c, v}); }
  T& bend(uint32_t dt, uint8_t ch, int v14) { return raw(dt, {uint8_t(0xE0 | ch), uint8_t(v14 & 127), uint8_t(v14 >> 7)}); }
  T& rpn(uint8_t ch, uint8_t msb, uint8_t lsb, uint8_t value, int valueLsb = -1) {
    cc(0, ch, 101, msb).cc(0, ch, 100, lsb).cc(0, ch, 6, value);
    if (valueLsb >= 0) cc(0, ch, 38, uint8_t(valueLsb));
    return cc(0, ch, 101, 127).cc(0, ch, 100, 127);
  }
  T& sysex(uint32_t dt, uint8_t status, const Bytes& payload) {
    putVlq(b, dt);
    b.push_back(status);
    putVlq(b, uint32_t(payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
    return *this;
  }
  T& meta(uint32_t dt, uint8_t type, const Bytes& payload) {
    putVlq(b, dt);
    b.push_back(0xFF);
    b.push_back(type);
    putVlq(b, uint32_t(payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
    return *this;
  }
  T& end(uint32_t dt = 0) { return raw(dt, {0xFF, 0x2F, 0x00}); }
};

Bytes chunk(const char* id, const Bytes& body) {
  Bytes c(id, id + 4);
  put32(c, uint32_t(body.size()));
  c.insert(c.end(), body.begin(), body.end());
  return c;
}

Bytes smf(uint16_t format, std::initializer_list<Bytes> tracks, uint16_t division = 96) {
  Bytes h = {uint8_t(format >> 8), uint8_t(format), uint8_t(tracks.size() >> 8), uint8_t(tracks.size()),
             uint8_t(division >> 8), uint8_t(division)};
  Bytes out = chunk("MThd", h);
  for (const Bytes& t : tracks) {
    const Bytes c = chunk("MTrk", t);
    out.insert(out.end(), c.begin(), c.end());
  }
  return out;
}

MidiFile parsed(const Bytes& b) {
  Result<MidiFile> f = parseMidiFile(b);
  if (!f) FAIL(f.error().message);
  return std::move(f).value();
}
MidiNotes notesOf(const Bytes& b) { return resolveNotes(parsed(b)); }

// MTS frequency data for `cents` above key 0's 12-TET pitch.
void freq(Bytes& b, double cents) {
  const int semis = int(cents / 100.0);
  const int frac = int((cents - semis * 100.0) * 16384.0 / 100.0 + 0.5);
  b.push_back(uint8_t(semis));
  b.push_back(uint8_t(frac >> 7));
  b.push_back(uint8_t(frac & 127));
}

}  // namespace

TEST_CASE("midi file: header, division, running status, end of track") {
  T t;
  t.on(0, 0, 60).raw(96, {60, 0}).raw(0, {64, 90}).raw(96, {64, 0}).end(10).raw(0, {0x90, 1, 1});  // after EOT: ignored
  const MidiFile f = parsed(smf(0, {t.b}, 480));
  CHECK(f.format == 0);
  CHECK(f.division == MidiDivision{480, 0, 0});
  REQUIRE(f.tracks.size() == 1);
  const MidiTrack& tr = f.tracks[0];
  REQUIRE(tr.events.size() == 4);
  CHECK(tr.events[1].tick == 96);
  CHECK(tr.events[1].status == 0x90);
  CHECK(tr.events[2].data1 == 64);
  CHECK(tr.events[2].data2 == 90);
  CHECK(tr.endTick == 202);

  const MidiNotes n = resolveNotes(f);
  REQUIRE(n.notes.size() == 2);
  CHECK(n.notes[0].key == 60);
  CHECK(n.notes[0].length == 96);
  CHECK(n.notes[0].releaseVelocity == 64);  // velocity-0 note-on = off
  CHECK(n.notes[1].tick == 96);
  CHECK(n.notes[1].velocity == 90);
  CHECK(n.endTick == 202);

  // SMPTE: 25 fps × 40 ticks per frame (division E7 28).
  const MidiFile s = parsed(smf(1, {T{}.end().b}, 0xE728));
  CHECK(s.division.isSmpte());
  CHECK(s.division.smpteFps == 25);
  CHECK(s.division.ticksPerFrame == 40);
  CHECK(s.division.ticksPerSecond() == 1000.0);
}

TEST_CASE("midi file: sysex, meta, unknown chunks, RMID") {
  T t;
  t.meta(0, 0x03, {'P', 'i', 'a', 'n', 'o'})
      .meta(0, 0x51, {0x07, 0xA1, 0x20})       // 500000 µs = 120 bpm
      .meta(0, 0x58, {3, 3, 24, 8})            // 3/8
      .sysex(0, 0xF0, {0x7E, 0x7F, 0x09, 0x01, 0xF7})  // GM on
      .sysex(5, 0xF7, {0xF8})                  // escape
      .meta(10, 0x51, {0x0F, 0x42, 0x40})      // 1000000 µs = 60 bpm
      .meta(0, 0x7F, {})                       // sequencer specific, empty
      .end();
  const Bytes plain = smf(1, {t.b});
  const MidiFile f = parsed(plain);
  const MidiTrack& tr = f.tracks[0];
  REQUIRE(tr.events.size() == 7);
  CHECK(tr.events[3].status == 0xF0);
  CHECK(tr.payload(tr.events[3]).size() == 5);
  CHECK(tr.events[4].status == 0xF7);
  CHECK(tr.events[4].tick == 5);
  CHECK(tr.payload(tr.events[4])[0] == 0xF8);
  CHECK(tr.events[6].type == 0x7F);
  CHECK(tr.events[6].size == 0);

  const MidiNotes n = resolveNotes(f);
  CHECK(n.trackNames == std::vector<std::string>{"Piano"});
  REQUIRE(n.tempos.size() == 2);
  CHECK(n.tempos[0].bpm() == 120.0);
  CHECK(n.tempos[1].tick == 15);
  CHECK(n.tempos[1].usPerQuarter == 1000000);
  REQUIRE(n.signatures.size() == 1);
  CHECK(n.signatures[0].numerator == 3);
  CHECK(n.signatures[0].denominator == 8);

  // An unknown chunk between tracks and after the last one is skipped.
  Bytes withJunk = smf(1, {t.b});
  const Bytes junk = chunk("XFIH", {1, 2, 3});
  withJunk.insert(withJunk.begin() + 14, junk.begin(), junk.end());
  withJunk.insert(withJunk.end(), junk.begin(), junk.end());
  CHECK(parsed(withJunk) == f);

  // RIFF RMID: a padded odd-sized chunk before "data".
  Bytes body = {'R', 'M', 'I', 'D', 'L', 'I', 'S', 'T'};
  putLe32(body, 3);
  body.insert(body.end(), {'a', 'b', 'c', 0});
  body.insert(body.end(), {'d', 'a', 't', 'a'});
  putLe32(body, uint32_t(plain.size()));
  body.insert(body.end(), plain.begin(), plain.end());
  Bytes riff = {'R', 'I', 'F', 'F'};
  putLe32(riff, uint32_t(body.size()));
  riff.insert(riff.end(), body.begin(), body.end());
  CHECK(parsed(riff) == f);
}

TEST_CASE("midi file: malformed input is an error") {
  const Bytes good = smf(1, {T{}.on(0, 0, 60).off(10, 0, 60).end().b});
  CHECK(parseMidiFile(good).ok());
  // Every truncation fails (the last MTrk's chunk is cut).
  for (size_t n = 0; n < good.size(); ++n) {
    CAPTURE(n);
    CHECK_FALSE(parseMidiFile(std::span<const uint8_t>(good.data(), n)).ok());
  }
  auto fails = [](const Bytes& b) {
    const Result<MidiFile> r = parseMidiFile(b);
    if (r.ok()) return false;
    CHECK(r.error().code == pq::ErrorCode::Parse);
    CHECK(r.error().message.find("at byte") != std::string::npos);
    return true;
  };
  CHECK(fails({'J', 'U', 'N', 'K', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96}));
  CHECK(fails(smf(3, {T{}.end().b})));                          // format
  CHECK(fails(smf(1, {T{}.end().b}, 0)));                       // zero ppq
  CHECK(fails(smf(1, {T{}.end().b}, 0xE500)));                  // 27 fps
  CHECK(fails(smf(1, {T{}.raw(0, {60, 64}).end().b})));         // data byte, no running status
  CHECK(fails(smf(1, {T{}.raw(0, {0x90, 60, 0x80}).end().b})));  // status inside a message
  CHECK(fails(smf(1, {T{}.raw(0, {0xF1, 0}).end().b})));         // system message
  CHECK(fails(smf(1, {T{}.raw(0x0FFFFFFF, {}).b})));             // event missing after the delta
  CHECK(fails(smf(1, {Bytes{0x81, 0x81, 0x81, 0x81, 0x00, 0xFF, 0x2F, 0x00}})));  // a 5-byte delta
  CHECK(fails(smf(1, {T{}.raw(0, {0xFF, 0x01, 0x05, 'a'}).b})));  // meta longer than its chunk
  Bytes missing = smf(1, {T{}.end().b});
  missing[11] = 2;  // two tracks declared, one present
  CHECK(fails(missing));
  CHECK(fails({'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E'}));
  CHECK(fails({'R', 'I', 'F', 'F', 4, 0, 0, 0, 'R', 'M', 'I', 'D'}));  // no data chunk
}

TEST_CASE("midi notes: FIFO pairing per track, channel and key; unpaired notes; format 1 vs 2 channel state") {
  T a;  // two overlapping C4s, the first off ends the first on; a third never ends
  a.on(0, 0, 60, 10).on(10, 0, 60, 20).off(10, 0, 60, 30).off(10, 0, 60, 40).on(0, 0, 61, 50).on(0, 1, 60, 60).end(100);
  T b;  // the same key on another track does not pair with track 0's
  b.bend(0, 0, 8192 + 4096).on(5, 0, 60, 70).off(1000, 0, 60).end();
  const MidiNotes n = notesOf(smf(1, {a.b, b.b}));
  REQUIRE(n.notes.size() == 5);
  // By (tick, track): track 1's bend at 0 comes after track 0's note-on at 0 and reaches its later notes (one port,
  // shared channels).
  CHECK(n.notes[0].velocity == 10);
  CHECK(n.notes[0].length == 20);
  CHECK(n.notes[0].releaseVelocity == 30);
  CHECK(n.notes[0].cents == 0.0);
  CHECK(n.notes[1].velocity == 70);
  CHECK(n.notes[1].track == 1);
  CHECK(n.notes[1].length == 1000);
  CHECK(n.notes[2].velocity == 20);
  CHECK(n.notes[2].length == 20);
  CHECK(n.notes[2].releaseVelocity == 40);
  CHECK(n.notes[2].cents == doctest::Approx(100.0));
  CHECK(n.notes[3].velocity == 50);
  CHECK(n.notes[3].length == 100);  // unpaired: to its track's end (tick 130)
  CHECK(n.notes[4].channel == 1);
  CHECK(n.notes[4].length == 100);
  CHECK(n.trackNotes == std::vector<uint32_t>{4, 1});
  CHECK(n.midNoteBends == 1);  // the bend at 0 lands under track 0's first note
  CHECK(n.endTick == 1005);

  // Format 2: each track its own sequence; track 1's bend does not reach track 0.
  const MidiNotes f2 = notesOf(smf(2, {a.b, b.b}));
  REQUIRE(f2.notes.size() == 5);
  CHECK(f2.notes[0].cents == 0.0);
  CHECK(f2.notes[1].track == 1);
  CHECK(f2.notes[1].cents == doctest::Approx(100.0));
}

TEST_CASE("midi notes: pitch bend ranges (RPN 0), counts of what is dropped") {
  T t;
  t.bend(0, 0, 16383).on(0, 0, 60).off(10, 0, 60);  // default ±2: +200 · 8191/8192
  t.bend(0, 0, 0).on(0, 0, 60).off(10, 0, 60);      // −200
  t.rpn(0, 0, 0, 12).bend(0, 0, 8192 + 4096).on(0, 0, 60).off(10, 0, 60);  // ±12: +600
  t.rpn(0, 0, 0, 1, 50).bend(0, 0, 8192 - 4096).on(0, 0, 60);              // ±1.5: −75
  t.bend(5, 0, 8192).off(5, 0, 60);  // a bend under a sounding note: counted
  t.cc(0, 0, 99, 0).cc(0, 0, 98, 0).cc(0, 0, 6, 24).on(0, 0, 62).off(1, 0, 62);  // NRPN data entry: no range change
  t.cc(0, 1, 7, 100).cc(0, 1, 10, 64).raw(0, {0xD1, 5}).raw(0, {0xA1, 60, 5}).raw(0, {0xC1, 3});
  t.bend(0, 1, 9000).end();  // no note on channel 1: not counted
  const MidiNotes n = notesOf(smf(1, {t.b}));
  REQUIRE(n.notes.size() == 5);
  CHECK(n.notes[0].cents == doctest::Approx(200.0 * 8191.0 / 8192.0));
  CHECK(n.notes[1].cents == doctest::Approx(-200.0));
  CHECK(n.notes[2].cents == doctest::Approx(600.0));
  CHECK(n.notes[3].cents == doctest::Approx(-75.0));
  CHECK(n.notes[4].cents == doctest::Approx(0.0));
  CHECK(n.midNoteBends == 1);
  CHECK(n.droppedControllers == 2);
  CHECK(n.droppedPressure == 2);
}

TEST_CASE("midi notes: MPE zones (MCM), master bend added, member ranges") {
  T t;
  t.rpn(0, 0, 6, 15);                                          // lower zone, 15 members: ±48
  t.bend(0, 1, 8192 + 4096).on(0, 1, 60).off(10, 1, 60);       // +2400
  t.bend(0, 0, 8192 + 4096).on(0, 2, 60).off(10, 2, 60);       // master +100 (±2) on a member at 0
  t.on(0, 0, 60).off(10, 0, 60);                               // the master's own note: its bend only
  t.rpn(3, 0, 0, 24).bend(0, 2, 8192 + 4096).on(0, 2, 61).off(10, 2, 61);  // RPN 0 on a member: every member ±24
  t.bend(0, 0, 8192).bend(0, 9, 8192 + 4096).on(0, 9, 62).off(10, 9, 62);  // +1200 on another member
  t.on(0, 5, 64).bend(5, 0, 9000).off(5, 5, 64);               // master bend under a member's note: counted
  t.rpn(15, 0, 6, 3);                                          // upper zone 12–14 shrinks the lower to 1–11
  t.bend(0, 13, 8192 + 4096).on(0, 13, 60).off(10, 13, 60);    // +2400 (upper member ±48)
  t.bend(0, 0, 8192).rpn(0, 0, 6, 0);                          // the lower zone ends: 1–11 back to ±2
  t.bend(0, 4, 8192 + 4096).on(0, 4, 60).off(10, 4, 60).end();  // +100
  const MidiNotes n = notesOf(smf(0, {t.b}));
  REQUIRE(n.notes.size() == 8);
  CHECK(n.notes[0].cents == doctest::Approx(2400.0));
  CHECK(n.notes[1].cents == doctest::Approx(100.0));
  CHECK(n.notes[2].cents == doctest::Approx(100.0));
  CHECK(n.notes[3].cents == doctest::Approx(1200.0 + 100.0));
  CHECK(n.notes[4].cents == doctest::Approx(1200.0));
  CHECK(n.notes[5].cents == doctest::Approx(0.0));
  CHECK(n.notes[6].cents == doctest::Approx(2400.0));
  CHECK(n.notes[7].cents == doctest::Approx(100.0));
  CHECK(n.midNoteBends == 1);
}

TEST_CASE("midi notes: MTS single-note changes, bulk dump, scale / octave, tuning programs, split sysex") {
  T t;
  t.on(0, 0, 61).off(10, 0, 61);  // before any tuning: 0
  // 08 02 real-time, program 0: key 61 → 60.50 semitones (−50 ¢)
  t.sysex(0, 0xF0, {0x7F, 0x7F, 0x08, 0x02, 0x00, 0x01, 61, 60, 0x40, 0x00, 0xF7});
  t.on(0, 0, 61).off(10, 0, 61);
  // the same, split into F0 + F7 packets: key 62 → +25
  t.sysex(0, 0xF0, {0x7F, 0x7F, 0x08, 0x02, 0x00, 0x01});
  t.sysex(0, 0xF7, {62, 62, 0x20, 0x00, 0xF7});
  t.on(0, 0, 62).off(10, 0, 62);
  // 08 07 non-real-time, bank 1 program 2: key 60 → +25, only on a channel that selects it (RPN 4 bank, RPN 3 program)
  t.sysex(0, 0xF0, {0x7E, 0x00, 0x08, 0x07, 0x01, 0x02, 0x01, 60, 60, 0x20, 0x00, 0xF7});
  t.rpn(3, 0, 4, 1).rpn(3, 0, 3, 2);
  t.on(0, 3, 60).off(10, 3, 60).on(0, 0, 60).off(10, 0, 60);
  // 08 01 bulk dump of program 0: key 69 → +50, key 61 "no change" (keeps −50), others 12-TET
  Bytes dump = {0x7E, 0x7F, 0x08, 0x01, 0x00};
  for (int i = 0; i < 16; ++i) dump.push_back(' ');
  for (int k = 0; k < 128; ++k) {
    if (k == 61)
      dump.insert(dump.end(), {0x7F, 0x7F, 0x7F});
    else
      freq(dump, k * 100.0 + (k == 69 ? 50.0 : 0.0));
  }
  dump.insert(dump.end(), {0x00, 0xF7});
  t.sysex(0, 0xF0, dump);
  t.on(0, 0, 69).off(10, 0, 69).on(0, 0, 61).off(10, 0, 61).on(0, 0, 62).off(10, 0, 62);
  // 08 08 (1-byte) on channel 5 (hh bit 5): C +10; 08 09 (2-byte) on channel 8 (gg bit 1): D +25
  Bytes so1 = {0x7E, 0x7F, 0x08, 0x08, 0x00, 0x00, 0x20, 0x4A};
  for (int i = 1; i < 12; ++i) so1.push_back(0x40);
  so1.push_back(0xF7);
  Bytes so2 = {0x7F, 0x7F, 0x08, 0x09, 0x00, 0x02, 0x00};
  for (int i = 0; i < 12; ++i) so2.insert(so2.end(), {uint8_t(i == 2 ? 0x50 : 0x40), 0x00});
  so2.push_back(0xF7);
  t.sysex(0, 0xF0, so1).sysex(0, 0xF0, so2);
  t.on(0, 5, 72).off(10, 5, 72).on(0, 5, 62).off(10, 5, 62).on(0, 8, 62).off(10, 8, 62).on(0, 0, 72).off(10, 0, 72);
  t.end();
  const MidiNotes n = notesOf(smf(1, {t.b}));
  REQUIRE(n.notes.size() == 12);
  CHECK(n.notes[0].cents == 0.0);
  CHECK(n.notes[1].cents == doctest::Approx(-50.0));
  CHECK(n.notes[2].cents == doctest::Approx(25.0));
  CHECK(n.notes[3].channel == 3);
  CHECK(n.notes[3].cents == doctest::Approx(25.0));
  CHECK(n.notes[4].cents == 0.0);
  CHECK(n.notes[5].cents == doctest::Approx(50.0));
  CHECK(n.notes[6].cents == doctest::Approx(-50.0));
  CHECK(n.notes[7].cents == doctest::Approx(0.0));  // the dump retuned 62 back to 12-TET
  CHECK(n.notes[8].cents == doctest::Approx(10.0));
  CHECK(n.notes[9].cents == doctest::Approx(0.0));
  CHECK(n.notes[10].cents == doctest::Approx(25.0));
  CHECK(n.notes[11].cents == doctest::Approx(0.0));
}

TEST_CASE("midi notes: sustain holds released notes; all notes off; reset") {
  T t;
  t.on(0, 0, 60).cc(5, 0, 64, 127).off(5, 0, 60, 33).on(0, 0, 62).cc(40, 0, 64, 0);  // 60: 0 → 50; 62 sounds on
  t.cc(0, 0, 64, 100).on(0, 0, 64).off(10, 0, 64).on(10, 0, 64).off(10, 0, 64);       // re-strike ends the held one
  t.cc(10, 0, 64, 0).off(0, 0, 62);
  t.on(0, 1, 70).on(0, 1, 71).cc(10, 1, 123, 0);                                     // all notes off
  t.cc(0, 2, 64, 127).on(0, 2, 72).off(5, 2, 72).cc(5, 2, 121, 0);                   // reset releases the pedal
  t.cc(0, 3, 64, 127).on(0, 3, 73).off(5, 3, 73).end(20);                            // held to the end
  const MidiNotes n = notesOf(smf(1, {t.b}));
  REQUIRE(n.notes.size() == 8);
  CHECK(n.notes[0].length == 50);
  CHECK(n.notes[0].releaseVelocity == 33);
  CHECK(n.notes[1].key == 62);
  CHECK(n.notes[1].length == 80);
  CHECK(n.notes[2].tick == 50);
  CHECK(n.notes[2].length == 20);  // ended by the re-strike at 70
  CHECK(n.notes[3].tick == 70);
  CHECK(n.notes[3].length == 20);  // held from 80 to the pedal at 90
  CHECK(n.notes[4].length == 10);
  CHECK(n.notes[5].length == 10);
  CHECK(n.notes[6].length == 10);
  CHECK(n.notes[7].tick == 110);
  CHECK(n.notes[7].length == 25);
}

TEST_CASE("midi file: write → read round trips") {
  MidiFile f;
  f.format = 1;
  f.division = {960, 0, 0};
  MidiTrack& t0 = f.tracks.emplace_back();
  t0.name("Tempo");
  t0.tempo(0, 500000);
  t0.timeSignature(0, 7, 8);
  t0.tempo(1920, 400000);
  t0.endTick = 4000;
  MidiTrack& t1 = f.tracks.emplace_back();
  t1.noteOff(960, 2, 60, 10);  // built out of order: the writer sorts (stable)
  t1.rpn(0, 2, 0, 12);
  t1.pitchBend(0, 2, 8192 + 1024);
  t1.noteOn(0, 2, 60, 100);
  t1.channel(5, 0xC2, 7);
  t1.sysex(6, 0xF0, std::vector<uint8_t>{0x7E, 0x7F, 0x09, 0x01, 0xF7});
  t1.noteOn(0x20000000ll, 3, 61, 1);  // a delta past the VLQ's range: bridged by empty Text metas
  const std::vector<uint8_t> bytes = writeMidiFile(f);
  const MidiFile back = parsed(bytes);
  CHECK(back.format == 1);
  CHECK(back.division == f.division);
  REQUIRE(back.tracks.size() == 2);
  CHECK(back.tracks[0] == f.tracks[0]);
  const MidiTrack& b1 = back.tracks[1];
  REQUIRE(b1.events.size() == 12 + 1);  // + one bridging Text meta
  CHECK(b1.events[0].status == 0xB2);
  CHECK(b1.events[7].status == 0x92);
  CHECK(b1.events[7].tick == 0);
  CHECK(b1.events[8].status == 0xC2);
  CHECK(b1.events[8].data2 == 0);
  CHECK(b1.events[10].status == 0x82);
  CHECK(b1.events[11].isMeta());
  CHECK(b1.events[11].type == 0x01);
  CHECK(b1.events.back().tick == 0x20000000ll);
  CHECK(b1.endTick == 0x20000000ll);

  const MidiNotes n = resolveNotes(back);
  REQUIRE(n.notes.size() == 2);
  CHECK(n.notes[0].cents == doctest::Approx(1200.0 * 1024.0 / 8192.0));
  CHECK(n.notes[0].length == 960);
  CHECK(n.trackNames == std::vector<std::string>{"Tempo", ""});
  CHECK(n.signatures == std::vector<MidiTimeSignature>{{0, 7, 8}});
  CHECK(n.tempos.size() == 2);
  // resolveNotes reads the unsorted in-memory file the same way.
  CHECK(resolveNotes(f).notes == n.notes);

  // Writing what was read gives the same file again (running status, SMPTE).
  CHECK(parsed(writeMidiFile(back)) == back);
  MidiFile s = back;
  s.division = {0, 30, 80};
  CHECK(parsed(writeMidiFile(s)) == s);
}

TEST_CASE("midi file: random mutations never crash; whatever parses writes back to itself") {
  T t;
  t.meta(0, 0x03, {'x'}).meta(0, 0x51, {0x07, 0xA1, 0x20}).rpn(0, 0, 6, 15).rpn(1, 0, 0, 12);
  t.sysex(0, 0xF0, {0x7F, 0x7F, 0x08, 0x02, 0x00, 0x02, 61, 60, 0x40, 0x00, 62, 63, 0x10, 0x00, 0xF7});
  t.sysex(0, 0xF0, {0x7E, 0x7F, 0x08, 0x08, 0x03, 0x7F, 0x7F, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0xF7});
  for (int i = 0; i < 24; ++i) {
    t.bend(0, uint8_t(i % 16), 8192 + i * 100).on(1, uint8_t(i % 16), uint8_t(40 + i)).raw(2, {uint8_t(41 + i), 70});
    t.cc(1, uint8_t(i % 16), 64, uint8_t(i * 5)).off(3, uint8_t(i % 16), uint8_t(40 + i));
  }
  t.end();
  const Bytes seed = smf(1, {t.b, T{}.on(0, 1, 50).raw(4, {50, 0}).end().b});
  std::mt19937 rng(1234);
  int parsedCount = 0;
  for (int iter = 0; iter < 600; ++iter) {
    Bytes b = seed;
    const int edits = 1 + int(rng() % 6);
    for (int e = 0; e < edits; ++e) {
      const size_t at = rng() % b.size();
      switch (rng() % 4) {
        case 0: b[at] = uint8_t(rng()); break;
        case 1: b[at] ^= uint8_t(1u << (rng() % 8)); break;
        case 2: b.erase(b.begin() + ptrdiff_t(at)); break;
        default: b.insert(b.begin() + ptrdiff_t(at), uint8_t(rng())); break;
      }
      if (b.size() < 2) b = seed;
    }
    if (rng() % 8 == 0) b.resize(rng() % b.size());
    const Result<MidiFile> f = parseMidiFile(b);
    if (!f) continue;
    ++parsedCount;
    const MidiNotes n = resolveNotes(*f);
    for (const MidiNote& note : n.notes) {
      CHECK(note.length >= 0);
      CHECK(note.track < f->tracks.size());
    }
    const Result<MidiFile> again = parseMidiFile(writeMidiFile(*f));
    REQUIRE(again.ok());
    CHECK(*again == *f);
  }
  MESSAGE("mutations that still parse: ", parsedCount, " of 600");
}

namespace {
// ≈ 10.6 MB: 16 tracks × 110k note pairs, running status, a bend and a controller every 16 notes.
const Bytes& bigFile() {
  static const Bytes bytes = [] {
    MidiFile f;
    f.division.ppq = 960;
    for (int tr = 0; tr < 16; ++tr) {
      MidiTrack& t = f.tracks.emplace_back();
      t.events.reserve(240000);
      int64_t tick = 0;
      for (int i = 0; i < 110000; ++i) {
        const uint8_t key = uint8_t(36 + (i * 7 + tr) % 60);
        if (i % 16 == 0) {
          t.pitchBend(tick, uint8_t(tr), uint16_t(8192 + i % 2000));
          t.controller(tick, uint8_t(tr), 74, uint8_t(i & 127));
        }
        t.noteOn(tick, uint8_t(tr), key, uint8_t(1 + i % 127));
        t.channel(tick + 60, uint8_t(0x90 | tr), key, 0);
        tick += 120;
      }
      t.endTick = tick;
    }
    return writeMidiFile(f);
  }();
  return bytes;
}
}  // namespace

TEST_CASE("midi file: a 10 MB file parses and resolves") {
  const Bytes& b = bigFile();
  CHECK(b.size() > 10'000'000);
  const auto t0 = std::chrono::steady_clock::now();
  const Result<MidiFile> f = parseMidiFile(b);
  const auto t1 = std::chrono::steady_clock::now();
  REQUIRE(f.ok());
  const MidiNotes n = resolveNotes(*f);
  const auto t2 = std::chrono::steady_clock::now();
  CHECK(n.notes.size() == 16u * 110000u);
  CHECK(n.notes[1].length == 60);
  MESSAGE("10 MB: ", b.size(), " bytes; parse ", std::chrono::duration<double, std::milli>(t1 - t0).count(),
          " ms; resolveNotes ", std::chrono::duration<double, std::milli>(t2 - t1).count(), " ms");
}

TEST_CASE("midi file: a 10 MB file parses in under 50 ms" * doctest::test_suite("perf")) {
  const Bytes& b = bigFile();
  double best = 1e9;
  for (int i = 0; i < 3; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    const Result<MidiFile> f = parseMidiFile(b);
    best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    REQUIRE(f.ok());
  }
  MESSAGE("10 MB parse, best of 3: ", best, " ms");
  CHECK(best < 50.0);
}

TEST_CASE("midi notes (AU.dsp, RB-DY M7): a hostile file — 300k note-ons on one track, their note-offs on another — "
          "resolves in linear time") {
  // Each note-off of track 1 finds no note of its own track on C4; it used to walk all of track 0's sounding C4s
  // (≈ 4.5e10 link visits here), now one lookup.
  constexpr int kN = 300000;
  MidiFile f;
  f.format = 1;
  f.tracks.resize(2);
  for (int i = 0; i < kN; ++i) f.tracks[0].noteOn(i, 0, 60, 100);
  for (int i = 0; i < kN; ++i) f.tracks[1].noteOff(i, 0, 60);
  f.tracks[0].endTick = f.tracks[1].endTick = kN + 10;
  const auto t0 = std::chrono::steady_clock::now();
  const MidiNotes n = resolveNotes(f);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  MESSAGE("hostile pairing: " << ms << " ms");
  REQUIRE(n.notes.size() == static_cast<size_t>(kN));
  CHECK(n.notes.front().length == kN + 10);  // unpaired: to its track's end
  CHECK(n.notes.back().length == 11);
  CHECK(ms < 2000.0);  // seconds before the fix would be hours
  // Paired on its own track, interleaved with another track's same key: still FIFO per track.
  MidiFile g;
  g.format = 1;
  g.tracks.resize(2);
  for (int i = 0; i < 1000; ++i) {
    g.tracks[0].noteOn(2 * i, 0, 60, 100);
    g.tracks[1].noteOn(2 * i, 0, 60, 90);
  }
  for (int i = 0; i < 1000; ++i) g.tracks[1].noteOff(5000 + i, 0, 60);
  for (int i = 0; i < 1000; ++i) g.tracks[0].noteOff(9000 + i, 0, 60);
  g.tracks[0].endTick = g.tracks[1].endTick = 20000;
  const MidiNotes m = resolveNotes(g);
  REQUIRE(m.notes.size() == 2000u);
  for (const MidiNote& x : m.notes) {
    const int i = static_cast<int>(x.tick / 2);
    CHECK(x.tick + x.length == (x.track == 0 ? 9000 + i : 5000 + i));
  }
}
