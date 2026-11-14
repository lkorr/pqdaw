// Standard MIDI Files (MF.1): pq/document/midi_file.h.
#include "pq/document/midi_file.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

namespace pq::doc {

namespace {

constexpr uint32_t kMaxVlq = 0x0FFFFFFFu;  // four 7-bit groups

Error parseError(size_t at, std::string_view what) {
  std::string m = "MIDI file: ";
  m += what;
  m += " at byte ";
  m += std::to_string(at);
  return Error{ErrorCode::Parse, std::move(m), {}};
}

uint32_t be32(const uint8_t* p) noexcept {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint32_t le32(const uint8_t* p) noexcept {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t be16(const uint8_t* p) noexcept { return uint16_t((p[0] << 8) | p[1]); }

// A variable-length quantity at p (< end). False: truncated or longer than 4 bytes.
bool readVlq(const uint8_t*& p, const uint8_t* end, uint32_t& out) noexcept {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    if (p >= end) return false;
    const uint8_t b = *p++;
    v = (v << 7) | (b & 0x7Fu);
    if (!(b & 0x80u)) {
      out = v;
      return true;
    }
  }
  return false;
}

// Bytes of data after a channel status (C0 program change and D0 channel pressure take one).
int dataBytes(uint8_t status) noexcept {
  const uint8_t k = status & 0xF0u;
  return (k == 0xC0 || k == 0xD0) ? 1 : 2;
}

// One MTrk's body [p, end); `base` = its offset in the input (error messages).
Result<MidiTrack> parseTrack(const uint8_t* const begin, const uint8_t* const end, size_t base) {
  MidiTrack t;
  t.events.reserve(size_t(end - begin) / 3 + 1);
  const uint8_t* p = begin;
  int64_t tick = 0;
  uint8_t running = 0;
  auto at = [&](const uint8_t* q) { return base + size_t(q - begin); };
  while (p < end) {
    uint32_t delta;
    if (!readVlq(p, end, delta)) return parseError(at(p), "bad or truncated delta time");
    tick += delta;
    if (p >= end) return parseError(at(p), "event missing after a delta time");
    uint8_t status = *p;
    if (status < 0x80) {
      // Running status. Kept across meta and sysex events (the spec cancels it there; lenient on read, the writer
      // always restates it).
      if (running == 0) return parseError(at(p), "data byte without a running status");
      status = running;
    } else {
      ++p;
    }
    if (status < 0xF0) {
      running = status;
      const int n = dataBytes(status);
      if (end - p < n) return parseError(at(p), "truncated channel message");
      const uint8_t d1 = p[0], d2 = n == 2 ? p[1] : uint8_t(0);
      if ((d1 | d2) & 0x80u) return parseError(at(p), "status byte inside a channel message");
      p += n;
      MidiEvent& e = t.events.emplace_back();
      e.tick = tick;
      e.status = status;
      e.data1 = d1;
      e.data2 = d2;
    } else if (status == 0xF0 || status == 0xF7 || status == 0xFF) {
      uint8_t type = 0;
      if (status == 0xFF) {
        if (p >= end) return parseError(at(p), "truncated meta event");
        type = *p++;
        if (type & 0x80u) return parseError(at(p - 1), "bad meta type");
      }
      uint32_t len;
      if (!readVlq(p, end, len)) return parseError(at(p), "bad or truncated event length");
      if (size_t(end - p) < len) return parseError(at(p), "truncated sysex / meta event");
      if (status == 0xFF && type == kMetaEndOfTrack) {
        t.endTick = tick;
        return t;  // anything after End of Track in the chunk is ignored
      }
      MidiEvent& e = t.events.emplace_back();
      e.tick = tick;
      e.status = status;
      e.type = type;
      e.offset = uint32_t(t.bytes.size());
      e.size = len;
      t.bytes.insert(t.bytes.end(), p, p + len);
      p += len;
    } else {
      return parseError(at(p - 1), "system message in a track");
    }
  }
  t.endTick = tick;  // no End of Track: the chunk's end
  return t;
}

void putVlq(std::vector<uint8_t>& out, uint32_t v) {
  uint8_t buf[4];
  int n = 0;
  buf[n++] = uint8_t(v & 0x7Fu);
  while ((v >>= 7) != 0 && n < 4) buf[n++] = uint8_t(0x80u | (v & 0x7Fu));
  while (n > 0) out.push_back(buf[--n]);
}
void put32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}
void put16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

// A delta time; a gap over the VLQ's range is bridged by empty Text meta events (FF 01 00).
void putDelta(std::vector<uint8_t>& out, int64_t delta) {
  while (delta > int64_t(kMaxVlq)) {
    putVlq(out, kMaxVlq);
    out.insert(out.end(), {uint8_t(0xFF), uint8_t(0x01), uint8_t(0x00)});
    delta -= kMaxVlq;
  }
  putVlq(out, uint32_t(delta));
}

}  // namespace

// ── MidiTrack builders ───────────────────────────────────────────────────────────────────────────────────────────────

void MidiTrack::channel(int64_t tick, uint8_t status, uint8_t data1, uint8_t data2) {
  MidiEvent& e = events.emplace_back();
  e.tick = tick;
  e.status = status;
  e.data1 = data1 & 0x7Fu;
  e.data2 = dataBytes(status) == 2 ? uint8_t(data2 & 0x7Fu) : uint8_t(0);
}

void MidiTrack::rpn(int64_t tick, uint8_t ch, uint16_t number, uint8_t msb, uint8_t lsb) {
  controller(tick, ch, 101, uint8_t(number >> 7));
  controller(tick, ch, 100, uint8_t(number & 127));
  controller(tick, ch, 6, msb);
  controller(tick, ch, 38, lsb);
  controller(tick, ch, 101, 127);
  controller(tick, ch, 100, 127);
}

void MidiTrack::meta(int64_t tick, uint8_t type, std::span<const uint8_t> data) {
  MidiEvent& e = events.emplace_back();
  e.tick = tick;
  e.status = 0xFF;
  e.type = type & 0x7Fu;
  e.offset = uint32_t(bytes.size());
  e.size = uint32_t(data.size());
  bytes.insert(bytes.end(), data.begin(), data.end());
}

void MidiTrack::sysex(int64_t tick, uint8_t status, std::span<const uint8_t> data) {
  MidiEvent& e = events.emplace_back();
  e.tick = tick;
  e.status = status == 0xF7 ? uint8_t(0xF7) : uint8_t(0xF0);
  e.offset = uint32_t(bytes.size());
  e.size = uint32_t(data.size());
  bytes.insert(bytes.end(), data.begin(), data.end());
}

void MidiTrack::tempo(int64_t tick, uint32_t us) {
  const uint8_t d[3] = {uint8_t(us >> 16), uint8_t(us >> 8), uint8_t(us)};
  meta(tick, kMetaTempo, d);
}

void MidiTrack::timeSignature(int64_t tick, uint8_t numerator, uint8_t denominator, uint8_t clocks, uint8_t n32) {
  uint8_t dd = 0;
  while (dd < 15 && (1u << (dd + 1)) <= denominator) ++dd;
  const uint8_t d[4] = {numerator, dd, clocks, n32};
  meta(tick, kMetaTimeSignature, d);
}

void MidiTrack::name(std::string_view text, int64_t tick) {
  meta(tick, kMetaTrackName,
       std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

// ── Reader ───────────────────────────────────────────────────────────────────────────────────────────────────────────

Result<MidiFile> parseMidiFile(std::span<const uint8_t> in) {
  const uint8_t* data = in.data();
  size_t size = in.size();
  size_t base = 0;  // offset of `data` in the input

  // RIFF RMID: "RIFF" <LE32 size> "RMID", sub-chunks <id><LE32 size> (padded to even); the SMF is the "data" one.
  if (size >= 4 && std::memcmp(data, "RIFF", 4) == 0) {
    if (size < 12) return parseError(size, "truncated RIFF header");
    if (std::memcmp(data + 8, "RMID", 4) != 0) return parseError(8, "RIFF file is not RMID");
    const uint64_t riffEnd = std::min<uint64_t>(8ull + le32(data + 4), size);
    uint64_t pos = 12;
    bool found = false;
    while (pos + 8 <= riffEnd) {
      const uint32_t len = le32(data + pos + 4);
      if (pos + 8 + len > size) return parseError(size_t(pos), "truncated RIFF chunk");
      if (std::memcmp(data + pos, "data", 4) == 0) {
        base = size_t(pos + 8);
        data += base;
        size = len;
        found = true;
        break;
      }
      pos += 8ull + len + (len & 1u);
    }
    if (!found) return parseError(12, "RMID without a data chunk");
  }

  if (size < 14 || std::memcmp(data, "MThd", 4) != 0)
    return parseError(base, size >= 4 && std::memcmp(data, "MThd", 4) == 0 ? "truncated header" : "not a MIDI file");
  const uint32_t hlen = be32(data + 4);
  if (hlen < 6) return parseError(base + 4, "header too short");
  if (uint64_t(8) + hlen > size) return parseError(base + 8, "truncated header");
  MidiFile f;
  f.format = be16(data + 8);
  const uint16_t declared = be16(data + 10);
  const uint16_t div = be16(data + 12);
  if (f.format > 2) return parseError(base + 8, "unknown format");
  if (div & 0x8000u) {
    const int fps = -int(int8_t(uint8_t(div >> 8)));
    if (fps != 24 && fps != 25 && fps != 29 && fps != 30) return parseError(base + 12, "bad SMPTE frame rate");
    if ((div & 0xFFu) == 0) return parseError(base + 12, "zero ticks per frame");
    f.division.ppq = 0;
    f.division.smpteFps = uint8_t(fps);
    f.division.ticksPerFrame = uint8_t(div & 0xFFu);
  } else {
    if (div == 0) return parseError(base + 12, "zero ticks per quarter");
    f.division.ppq = div;
  }
  f.tracks.reserve(declared);

  size_t pos = 8 + size_t(hlen);
  while (size - pos >= 8) {
    const bool isTrack = std::memcmp(data + pos, "MTrk", 4) == 0;
    const uint32_t len = be32(data + pos + 4);
    if (len > size - pos - 8) {
      // Junk after every declared track (an unknown chunk cut short) ends the file; anything else is truncation.
      if (!isTrack && f.tracks.size() >= declared) break;
      return parseError(base + pos, isTrack ? "truncated track chunk" : "truncated chunk");
    }
    if (isTrack) {
      if (f.tracks.size() >= 0xFFFF) break;  // MidiNote::track is 16-bit
      Result<MidiTrack> t = parseTrack(data + pos + 8, data + pos + 8 + len, base + pos + 8);
      if (!t) return std::move(t).error();
      f.tracks.push_back(std::move(t).value());
    }
    pos += 8 + size_t(len);
  }
  if (f.tracks.size() < declared)
    return parseError(base + pos, "missing track chunks (" + std::to_string(f.tracks.size()) + " of " +
                                      std::to_string(declared) + ")");
  return f;
}

// ── Writer ───────────────────────────────────────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> writeMidiFile(const MidiFile& file) {
  std::vector<uint8_t> out;
  size_t estimate = 14;
  for (const MidiTrack& t : file.tracks) estimate += 12 + t.events.size() * 4 + t.bytes.size() * 1 + t.events.size();
  out.reserve(estimate);

  out.insert(out.end(), {'M', 'T', 'h', 'd'});
  put32(out, 6);
  put16(out, file.format);
  put16(out, uint16_t(std::min<size_t>(file.tracks.size(), 0xFFFF)));
  if (file.division.isSmpte())
    put16(out, uint16_t((uint16_t(uint8_t(-int8_t(file.division.smpteFps))) << 8) | file.division.ticksPerFrame));
  else
    put16(out, uint16_t(file.division.ppq & 0x7FFFu));

  std::vector<uint32_t> order;
  const size_t nTracks = std::min<size_t>(file.tracks.size(), 0xFFFF);
  for (size_t ti = 0; ti < nTracks; ++ti) {
    const MidiTrack& t = file.tracks[ti];
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    const size_t lenAt = out.size();
    put32(out, 0);

    const bool sorted = std::is_sorted(t.events.begin(), t.events.end(),
                                       [](const MidiEvent& a, const MidiEvent& b) { return a.tick < b.tick; });
    if (!sorted) {
      order.resize(t.events.size());
      for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
      std::stable_sort(order.begin(), order.end(),
                       [&](uint32_t a, uint32_t b) { return t.events[a].tick < t.events[b].tick; });
    }
    int64_t tick = 0;
    uint8_t running = 0;
    for (size_t i = 0; i < t.events.size(); ++i) {
      const MidiEvent& e = t.events[sorted ? i : order[i]];
      if (e.isMeta() && e.type == kMetaEndOfTrack) continue;
      const int64_t et = std::max<int64_t>(e.tick, tick);  // negative ticks clamp to 0
      if (e.isChannel()) {
        putDelta(out, et - tick);
        if (e.status != running) out.push_back(e.status);
        running = e.status;
        out.push_back(e.data1 & 0x7Fu);
        if (dataBytes(e.status) == 2) out.push_back(e.data2 & 0x7Fu);
      } else if (e.isSysex() || e.isMeta()) {
        putDelta(out, et - tick);
        out.push_back(e.status);
        if (e.isMeta()) out.push_back(e.type & 0x7Fu);
        // A payload outside the pool or over the VLQ's range (2^28 − 1 bytes) is cut to what fits.
        const size_t avail = e.offset <= t.bytes.size() ? t.bytes.size() - e.offset : 0;
        const uint32_t n = uint32_t(std::min<size_t>({e.size, avail, kMaxVlq}));
        putVlq(out, n);
        out.insert(out.end(), t.bytes.begin() + ptrdiff_t(std::min<size_t>(e.offset, t.bytes.size())),
                   t.bytes.begin() + ptrdiff_t(std::min<size_t>(e.offset, t.bytes.size()) + n));
        running = 0;  // restate the status after a sysex / meta (the spec cancels running status there)
      } else {
        continue;  // not a storable event (status < 0x80 or a system message)
      }
      tick = et;
    }
    putDelta(out, std::max<int64_t>(t.endTick - tick, 0));
    out.insert(out.end(), {uint8_t(0xFF), uint8_t(kMetaEndOfTrack), uint8_t(0x00)});
    const uint32_t len = uint32_t(out.size() - lenAt - 4);
    out[lenAt] = uint8_t(len >> 24);
    out[lenAt + 1] = uint8_t(len >> 16);
    out[lenAt + 2] = uint8_t(len >> 8);
    out[lenAt + 3] = uint8_t(len);
  }
  return out;
}

// ── Notes ────────────────────────────────────────────────────────────────────────────────────────────────────────────

namespace {

constexpr int32_t kNone = -1;
constexpr uint16_t kRpnNull = 0x3FFF;
constexpr int kDefaultRange = 200;  // cents: ±2 semitones
constexpr int kMpeMemberRange = 4800;

struct ChannelState {
  int bend = 0;  // value − 8192
  uint8_t rangeSemis = 2, rangeCentsPart = 0;
  bool sustain = false;
  bool nrpn = false;
  uint16_t rpn = kRpnNull;
  uint8_t tuningProgram = 0, tuningBank = 0;
  uint32_t sounding = 0;  // notes started and not ended (held by the pedal included)
  std::array<double, 12> scaleOctave{};

  int range() const noexcept { return int(rangeSemis) * 100 + rangeCentsPart; }
  void setRange(int cents) noexcept {
    rangeSemis = uint8_t(std::clamp(cents / 100, 0, 127));
    rangeCentsPart = uint8_t(std::clamp(cents % 100, 0, 127));
  }
  double bendCents() const noexcept { return double(bend) / 8192.0 * double(range()); }
};

struct TuningProgram {
  uint8_t bank = 0, program = 0;
  std::array<double, 128> cents{};  // offset from each key's 12-TET pitch
};

class Resolver {
public:
  explicit Resolver(const MidiFile& f) : file_(f) {}

  MidiNotes run() {
    const size_t nTracks = file_.tracks.size();
    out_.trackNames.assign(nTracks, std::string());
    out_.trackNotes.assign(nTracks, 0);
    pending_.assign(nTracks, std::vector<uint8_t>());
    pendingOpen_.assign(nTracks, false);
    named_.assign(nTracks, false);
    orders_.resize(nTracks);
    ends_.resize(nTracks);
    size_t noteOns = 0;
    for (size_t t = 0; t < nTracks; ++t) {
      const MidiTrack& tr = file_.tracks[t];
      int64_t end = tr.endTick;
      for (const MidiEvent& e : tr.events) {
        end = std::max(end, e.tick);  // a built track's endTick may lag
        noteOns += (e.status & 0xF0u) == 0x90 && e.data2 != 0;
      }
      ends_[t] = end;
      out_.endTick = std::max(out_.endTick, end);
      // Tracks built in memory may be out of order: read them stably sorted (the parser's output always is).
      if (!std::is_sorted(tr.events.begin(), tr.events.end(),
                          [](const MidiEvent& a, const MidiEvent& b) { return a.tick < b.tick; })) {
        std::vector<uint32_t>& o = orders_[t];
        o.resize(tr.events.size());
        for (uint32_t i = 0; i < o.size(); ++i) o[i] = i;
        std::stable_sort(o.begin(), o.end(),
                         [&](uint32_t a, uint32_t b) { return tr.events[a].tick < tr.events[b].tick; });
      }
    }

    out_.notes.reserve(noteOns);
    links_.reserve(noteOns);
    if (file_.format == 2) {
      for (size_t t = 0; t < nTracks; ++t) {
        reset();
        const size_t n = file_.tracks[t].events.size();
        for (size_t i = 0; i < n; ++i) event(t, i);
        finish();
      }
      std::stable_sort(out_.notes.begin(), out_.notes.end(), [](const MidiNote& a, const MidiNote& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.track < b.track;
      });
    } else {
      reset();
      merged();
      finish();
    }

    auto byTick = [](const auto& a, const auto& b) { return a.tick < b.tick; };
    std::stable_sort(out_.tempos.begin(), out_.tempos.end(), byTick);
    std::stable_sort(out_.signatures.begin(), out_.signatures.end(), byTick);
    lastPerTick(out_.tempos);
    lastPerTick(out_.signatures);
    return std::move(out_);
  }

private:
  template <class T>
  static void lastPerTick(std::vector<T>& v) {
    size_t w = 0;
    for (size_t i = 0; i < v.size(); ++i) {
      if (w > 0 && v[w - 1].tick == v[i].tick)
        v[w - 1] = v[i];
      else
        v[w++] = v[i];
    }
    v.resize(w);
  }

  const MidiEvent& ev(size_t t, size_t i) const noexcept {
    const std::vector<uint32_t>& o = orders_[t];
    return file_.tracks[t].events[o.empty() ? i : o[i]];
  }

  // Formats 0 / 1: every track's events in time order, ties by track index (one port playing them all). A heap of the
  // tracks' next ticks; each pop plays the track's run up to the next track's turn.
  void merged() {
    const size_t nTracks = file_.tracks.size();
    using Head = std::pair<int64_t, size_t>;  // (tick, track): a binary min-heap, the root replaced in place
    std::vector<Head> h;
    std::vector<size_t> cursor(nTracks, 0);
    for (size_t t = 0; t < nTracks; ++t)
      if (!file_.tracks[t].events.empty()) h.emplace_back(ev(t, 0).tick, t);
    std::make_heap(h.begin(), h.end(), std::greater<Head>());
    auto siftDown = [&h] {
      const size_t n = h.size();
      size_t at = 0;
      const Head v = h[0];
      for (;;) {
        size_t c = 2 * at + 1;
        if (c >= n) break;
        if (c + 1 < n && h[c + 1] < h[c]) ++c;
        if (!(h[c] < v)) break;
        h[at] = h[c];
        at = c;
      }
      h[at] = v;
    };
    while (!h.empty()) {
      const size_t t = h[0].second;
      const size_t n = file_.tracks[t].events.size();
      size_t& i = cursor[t];
      if (h.size() == 1) {
        for (; i < n; ++i) event(t, i);
        break;
      }
      const Head next = h.size() == 2 ? h[1] : std::min(h[1], h[2]);  // the next track's turn
      while (i < n) {
        const int64_t tick = ev(t, i).tick;
        if (tick > next.first || (tick == next.first && t > next.second)) break;
        event(t, i++);
      }
      if (i < n) {
        h[0].first = ev(t, i).tick;
      } else {
        h[0] = h.back();
        h.pop_back();
      }
      siftDown();
    }
  }

  void reset() {
    ch_.fill(ChannelState{});
    lowerMembers_ = upperMembers_ = 0;
    programs_.clear();
    programIndex_.assign(128 * 128, -1);
    sounding_.fill(Slot{});
    trackSlots_.clear();
    held_.fill(kNone);
  }

  // ── notes ──

  // A (channel, key) slot: its sounding notes as a FIFO list (Link::next / prev, every track's), and the ones released
  // under the pedal (Link::nextHeld). AU.dsp (RB-DY M7): each track's sounding notes of the slot are a FIFO of their own
  // too (Link::nextInTrack, headed in trackSlots_), so a note-off finds its track's oldest note in O(1): a format-1 file
  // with 500k note-ons on one track and their note-offs on another walked the shared list per note-off (quadratic).
  struct Slot {
    int32_t head = kNone, tail = kNone;
  };
  static size_t slotOf(uint8_t channel, uint8_t key) noexcept { return size_t(channel) * 128 + key; }
  static uint64_t trackSlot(size_t track, size_t slot) noexcept { return (uint64_t(track) << 11) | slot; }

  double keyOffset(uint8_t channel, uint8_t key) const noexcept {
    const ChannelState& c = ch_[channel];
    double cents = c.scaleOctave[key % 12];
    // AU.dsp (RB-DY M7): the channel's tuning program by index, not a scan of the programs per note-on.
    if (const int32_t i = programIndex_[size_t(c.tuningBank & 0x7F) * 128 + (c.tuningProgram & 0x7F)]; i >= 0) cents += programs_[size_t(i)].cents[key];
    return cents;
  }

  // The MPE master of a member channel, or −1.
  int masterOf(uint8_t channel) const noexcept {
    if (lowerMembers_ > 0 && channel >= 1 && channel <= lowerMembers_) return 0;
    if (upperMembers_ > 0 && channel <= 14 && channel >= 15 - upperMembers_) return 15;
    return -1;
  }

  void endNote(int32_t n, int64_t tick) {
    MidiNote& note = out_.notes[size_t(n)];
    note.length = std::max<int64_t>(tick - note.tick, 0);
    ChannelState& c = ch_[note.channel];
    if (c.sounding > 0) --c.sounding;
  }

  void endHeld(size_t slot, int64_t tick) {
    for (int32_t n = held_[slot]; n != kNone; n = links_[size_t(n)].nextHeld) endNote(n, tick);
    held_[slot] = kNone;
  }

  void noteOn(size_t track, int64_t tick, uint8_t channel, uint8_t key, uint8_t velocity) {
    const size_t s = slotOf(channel, key);
    endHeld(s, tick);  // a re-struck key ends its pedal-held notes
    const int32_t n = int32_t(out_.notes.size());
    MidiNote& note = out_.notes.emplace_back();
    note.tick = tick;
    note.key = key;
    note.channel = channel;
    note.velocity = velocity;
    note.track = uint16_t(track);
    double cents = keyOffset(channel, key) + ch_[channel].bendCents();
    const int m = masterOf(channel);
    if (m >= 0) cents += ch_[size_t(m)].bendCents();
    note.cents = cents;
    links_.push_back(Link{});
    links_.back().track = uint32_t(track);
    Slot& sl = sounding_[s];
    links_[size_t(n)].prev = sl.tail;
    if (sl.tail == kNone)
      sl.head = n;
    else
      links_[size_t(sl.tail)].next = n;
    sl.tail = n;
    Slot& ts = trackSlots_[trackSlot(track, s)];
    if (ts.tail == kNone)
      ts.head = n;
    else
      links_[size_t(ts.tail)].nextInTrack = n;
    ts.tail = n;
    ++ch_[channel].sounding;
    ++out_.trackNotes[track];
  }

  // Releases the oldest sounding note of `track` on (channel, key): ends it, or holds it while the pedal is down.
  void noteOff(size_t track, int64_t tick, uint8_t channel, uint8_t key, uint8_t release) {
    const size_t s = slotOf(channel, key);
    const auto it = trackSlots_.find(trackSlot(track, s));
    if (it == trackSlots_.end() || it->second.head == kNone) return;  // nothing to release
    const int32_t n = it->second.head;
    it->second.head = links_[size_t(n)].nextInTrack;
    if (it->second.head == kNone) trackSlots_.erase(it);
    links_[size_t(n)].nextInTrack = kNone;
    // Out of the slot's list (doubly linked: O(1)).
    Slot& sl = sounding_[s];
    const int32_t before = links_[size_t(n)].prev, after = links_[size_t(n)].next;
    if (before == kNone)
      sl.head = after;
    else
      links_[size_t(before)].next = after;
    if (after == kNone)
      sl.tail = before;
    else
      links_[size_t(after)].prev = before;
    links_[size_t(n)].next = links_[size_t(n)].prev = kNone;
    out_.notes[size_t(n)].releaseVelocity = release;
    if (ch_[channel].sustain) {
      links_[size_t(n)].nextHeld = held_[s];
      held_[s] = n;
    } else {
      endNote(n, tick);
    }
  }

  void allNotesOff(int64_t tick, uint8_t channel, bool immediate) {
    for (uint8_t key = 0; key < 128; ++key) {
      const size_t s = slotOf(channel, key);
      Slot& sl = sounding_[s];
      for (int32_t n = sl.head; n != kNone;) {
        const int32_t after = links_[size_t(n)].next;
        trackSlots_.erase(trackSlot(links_[size_t(n)].track, s));  // every track's notes of the slot end here
        links_[size_t(n)].next = links_[size_t(n)].prev = links_[size_t(n)].nextInTrack = kNone;
        if (ch_[channel].sustain && !immediate) {
          links_[size_t(n)].nextHeld = held_[s];
          held_[s] = n;
        } else {
          endNote(n, tick);
        }
        n = after;
      }
      sl = Slot{};
      if (immediate) endHeld(s, tick);
    }
  }

  void pedalUp(int64_t tick, uint8_t channel) {
    for (uint8_t key = 0; key < 128; ++key) endHeld(slotOf(channel, key), tick);
  }

  // Notes still sounding or held end at their track's end.
  void finish() {
    for (size_t s = 0; s < sounding_.size(); ++s) {
      for (int32_t n = sounding_[s].head; n != kNone; n = links_[size_t(n)].next)
        endNote(n, ends_[links_[size_t(n)].track]);
      for (int32_t n = held_[s]; n != kNone; n = links_[size_t(n)].nextHeld)
        endNote(n, ends_[links_[size_t(n)].track]);
    }
  }

  // ── controllers ──

  void bend(int64_t, uint8_t channel, int value) {
    out_.absolutePitch = true;  // AU.dsp (M8): any pitch bend
    ChannelState& c = ch_[channel];
    if (value == c.bend) return;
    bool affects = c.sounding > 0;
    if (!affects && (channel == 0 || channel == 15)) {
      const bool lower = channel == 0 && lowerMembers_ > 0, upper = channel == 15 && upperMembers_ > 0;
      for (uint8_t m = 1; m <= 14 && !affects; ++m)
        affects = ((lower && m <= lowerMembers_) || (upper && m >= 15 - upperMembers_)) && ch_[m].sounding > 0;
    }
    if (affects) ++out_.midNoteBends;
    c.bend = value;
  }

  void setMemberRange(uint8_t channel, int cents) {
    const int m = masterOf(channel);
    if (m == 0) {
      for (uint8_t c = 1; c <= lowerMembers_; ++c) ch_[c].setRange(cents);
    } else if (m == 15) {
      for (uint8_t c = uint8_t(15 - upperMembers_); c <= 14; ++c) ch_[c].setRange(cents);
    } else {
      ch_[channel].setRange(cents);
    }
  }

  // An MPE Configuration Message on master `channel` (0: lower zone, 15: upper) with `members` member channels.
  void mcm(uint8_t channel, uint8_t members) {
    if (channel != 0 && channel != 15) return;
    std::array<int, 16> before{};
    for (uint8_t c = 0; c < 16; ++c) before[c] = masterOf(c);
    members = std::min<uint8_t>(members, 15);
    if (channel == 0) {
      lowerMembers_ = members;
      if (members > 0) upperMembers_ = uint8_t(std::clamp(int(upperMembers_), 0, std::max(14 - int(members), 0)));
    } else {
      upperMembers_ = members;
      if (members > 0) lowerMembers_ = uint8_t(std::clamp(int(lowerMembers_), 0, std::max(14 - int(members), 0)));
    }
    // Members of the configured zone (and channels joining the other one) play ±48; channels leaving a zone ±2.
    for (uint8_t c = 0; c < 16; ++c) {
      const int now = masterOf(c);
      if (now >= 0 && (now == channel || now != before[c]))
        ch_[c].setRange(kMpeMemberRange);
      else if (now < 0 && before[c] >= 0)
        ch_[c].setRange(kDefaultRange);
    }
    if (members > 0) ch_[channel].setRange(kDefaultRange);
  }

  void dataEntry(uint8_t channel, bool msb, uint8_t value) {
    ChannelState& c = ch_[channel];
    if (c.nrpn || c.rpn == kRpnNull) return;
    if (c.rpn == 0 || c.rpn == 3 || c.rpn == 4) out_.absolutePitch = true;  // AU.dsp (M8): an RPN tuning
    switch (c.rpn) {
      case 0:  // pitch bend sensitivity
        setMemberRange(channel, msb ? int(value) * 100 : int(c.rangeSemis) * 100 + value);
        break;
      case 3:  // tuning program select
        if (msb) c.tuningProgram = value;
        break;
      case 4:  // tuning bank select
        if (msb) c.tuningBank = value;
        break;
      case 6:  // MPE Configuration Message
        if (msb) mcm(channel, value);
        break;
      default:
        break;
    }
  }

  void controller(int64_t tick, uint8_t channel, uint8_t cc, uint8_t value) {
    ChannelState& c = ch_[channel];
    switch (cc) {
      case 64: {
        const bool down = value >= 64;
        if (c.sustain && !down) {
          c.sustain = false;
          pedalUp(tick, channel);
        }
        c.sustain = down;
        break;
      }
      case 101: c.rpn = uint16_t((value << 7) | (c.rpn & 127)); c.nrpn = false; break;
      case 100: c.rpn = uint16_t((c.rpn & ~uint16_t(127)) | value); c.nrpn = false; break;
      case 99:
      case 98: c.nrpn = true; break;
      case 6: dataEntry(channel, true, value); break;
      case 38: dataEntry(channel, false, value); break;
      case 120: allNotesOff(tick, channel, true); break;   // all sound off
      case 123: allNotesOff(tick, channel, false); break;  // all notes off (the pedal still holds)
      case 121:  // reset all controllers (RP-015: bend, sustain, RPN; not the ranges)
        if (c.bend != 0) bend(tick, channel, 0);
        if (c.sustain) {
          c.sustain = false;
          pedalUp(tick, channel);
        }
        c.rpn = kRpnNull;
        c.nrpn = false;
        break;
      default: ++out_.droppedControllers; break;
    }
  }

  // ── MTS ──

  TuningProgram& program(uint8_t bank, uint8_t prog) {
    int32_t& i = programIndex_[size_t(bank & 0x7F) * 128 + (prog & 0x7F)];
    if (i >= 0) return programs_[size_t(i)];
    i = int32_t(programs_.size());
    TuningProgram& p = programs_.emplace_back();
    p.bank = bank;
    p.program = prog;
    return p;
  }

  // Frequency data xx yy zz → the key's offset; false: 7F 7F 7F (no change) or a bad byte.
  static bool frequency(const uint8_t* d, uint8_t key, double& offset) noexcept {
    if (d[0] == 0x7F && d[1] == 0x7F && d[2] == 0x7F) return false;
    if ((d[0] | d[1] | d[2]) & 0x80u) return false;
    const double cents = double(d[0]) * 100.0 + double((d[1] << 7) | d[2]) * (100.0 / 16384.0);
    offset = cents - double(key) * 100.0;
    return true;
  }

  void noteChanges(TuningProgram& p, const uint8_t* d, size_t avail, size_t count) {
    count = std::min(count, avail / 4);
    for (size_t i = 0; i < count; ++i, d += 4) {
      if (d[0] >= 128) continue;
      double off;
      if (frequency(d + 1, d[0], off)) p.cents[d[0]] = off;
    }
  }

  void bulk(TuningProgram& p, const uint8_t* d, size_t avail) {
    if (avail < 128 * 3) return;
    for (uint8_t k = 0; k < 128; ++k) {
      double off;
      if (frequency(d + 3 * k, k, off)) p.cents[k] = off;
    }
  }

  void scaleOctave(const uint8_t* d, size_t avail, bool twoByte) {
    if (avail < 3 + (twoByte ? 24u : 12u)) return;
    const uint32_t mask = (uint32_t(d[0] & 3u) << 14) | (uint32_t(d[1] & 127u) << 7) | uint32_t(d[2] & 127u);
    std::array<double, 12> offs{};
    for (int i = 0; i < 12; ++i) {
      offs[size_t(i)] = twoByte ? (double((d[3 + 2 * i] << 7) | d[4 + 2 * i]) - 8192.0) * (100.0 / 8192.0)
                                : double(d[3 + i]) - 64.0;
    }
    for (uint8_t c = 0; c < 16; ++c)
      if (mask & (1u << c)) ch_[c].scaleOctave = offs;
  }

  // A complete sysex message (after F0).
  void sysexMessage(std::span<const uint8_t> m) {
    if (!m.empty() && m.back() == 0xF7) m = m.first(m.size() - 1);
    if (m.size() < 4 || (m[0] != 0x7E && m[0] != 0x7F) || m[2] != 0x08) return;
    out_.absolutePitch = true;  // AU.dsp (M8): an MTS message
    const uint8_t* d = m.data() + 4;
    const size_t n = m.size() - 4;
    switch (m[3]) {
      case 0x01:  // bulk dump: tt, name[16], 128 × xx yy zz, checksum
        if (n >= 17) bulk(program(0, d[0] & 127u), d + 17, n - 17);
        break;
      case 0x04:  // key-based tuning dump: bb tt name[16] …
        if (n >= 18) bulk(program(d[0] & 127u, d[1] & 127u), d + 18, n - 18);
        break;
      case 0x02:  // single note tuning change: tt ll [kk xx yy zz]…
        if (n >= 2) noteChanges(program(0, d[0] & 127u), d + 2, n - 2, d[1]);
        break;
      case 0x07:  // single note tuning change with bank: bb tt ll […]
        if (n >= 3) noteChanges(program(d[0] & 127u, d[1] & 127u), d + 3, n - 3, d[2]);
        break;
      case 0x08: scaleOctave(d, n, false); break;
      case 0x09: scaleOctave(d, n, true); break;
      default: break;
    }
  }

  void sysex(size_t t, const MidiEvent& e) {
    const std::span<const uint8_t> p = file_.tracks[t].payload(e);
    std::vector<uint8_t>& buf = pending_[t];
    if (e.status == 0xF0) {
      if (!p.empty() && p.back() == 0xF7) {
        pendingOpen_[t] = false;
        sysexMessage(p);
      } else {
        buf.assign(p.begin(), p.end());  // continued in F7 packets
        pendingOpen_[t] = true;
      }
    } else if (pendingOpen_[t]) {  // F7 continuation
      buf.insert(buf.end(), p.begin(), p.end());
      if (!p.empty() && p.back() == 0xF7) {
        pendingOpen_[t] = false;
        sysexMessage(buf);
      }
    }  // else an escape (raw bytes): nothing for notes
  }

  void meta(size_t t, const MidiEvent& e) {
    const std::span<const uint8_t> p = file_.tracks[t].payload(e);
    switch (e.type) {
      case kMetaTempo:
        if (p.size() >= 3) {
          const uint32_t us = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
          if (us > 0) out_.tempos.push_back(MidiTempo{e.tick, us});
        }
        break;
      case kMetaTimeSignature:
        if (p.size() >= 2 && p[0] > 0 && p[1] <= 15)
          out_.signatures.push_back(MidiTimeSignature{e.tick, p[0], uint16_t(1u << p[1])});
        break;
      case kMetaTrackName:
        if (!named_[t]) {
          out_.trackNames[t].assign(reinterpret_cast<const char*>(p.data()), p.size());
          named_[t] = true;
        }
        break;
      default: break;
    }
  }

  void event(size_t t, size_t i) {
    const MidiEvent& e = ev(t, i);
    if (e.isChannel()) {
      const uint8_t c = e.channel();
      switch (e.kind()) {
        case 0x90:
          if (e.data2 > 0) {
            noteOn(t, e.tick, c, e.data1 & 127u, e.data2);
            break;
          }
          noteOff(t, e.tick, c, e.data1 & 127u, 64);
          break;
        case 0x80: noteOff(t, e.tick, c, e.data1 & 127u, e.data2); break;
        case 0xB0: controller(e.tick, c, e.data1 & 127u, e.data2 & 127u); break;
        case 0xE0: bend(e.tick, c, int(((e.data2 & 127) << 7) | (e.data1 & 127)) - 8192); break;
        case 0xA0:
        case 0xD0: ++out_.droppedPressure; break;
        default: break;  // program change: the target track's instrument plays
      }
    } else if (e.isSysex()) {
      sysex(t, e);
    } else if (e.isMeta()) {
      meta(t, e);
    }
  }

  const MidiFile& file_;
  MidiNotes out_;
  std::array<ChannelState, 16> ch_{};
  uint8_t lowerMembers_ = 0, upperMembers_ = 0;
  std::vector<TuningProgram> programs_;
  std::vector<int32_t> programIndex_ = std::vector<int32_t>(128 * 128, -1);  // (bank, program) → programs_, −1: none
  std::array<Slot, 16 * 128> sounding_{};
  std::unordered_map<uint64_t, Slot> trackSlots_;  // (track, slot) → that track's sounding notes there (non-empty)
  std::array<int32_t, 16 * 128> held_{};
  struct Link {
    int32_t next = kNone;         // the next sounding note of its slot
    int32_t prev = kNone;         // the previous one
    int32_t nextInTrack = kNone;  // the next sounding note of its slot and track
    int32_t nextHeld = kNone;     // the next pedal-held note of its slot
    uint32_t track = 0;
  };
  std::vector<Link> links_;  // per note
  std::vector<std::vector<uint8_t>> pending_;  // per track: an unfinished F0 … F7 sysex
  std::vector<bool> pendingOpen_;
  std::vector<std::vector<uint32_t>> orders_;  // per track: a sorted read order (empty: already sorted)
  std::vector<bool> named_;      // per track: its name is read
  std::vector<int64_t> ends_;    // per track: its end
};

}  // namespace

MidiNotes resolveNotes(const MidiFile& file) { return Resolver(file).run(); }

}  // namespace pq::doc
