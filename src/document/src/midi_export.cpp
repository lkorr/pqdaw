// MF.3: MIDI file export (pq/document/midi_export.h describes it).

#include "pq/document/midi_export.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>

#include "edit_common.h"
#include "pq/document/clip_time.h"
#include "pq/document/input_map.h"
#include "pq/document/midi_file.h"
#include "pq/document/pitch_resolve.h"
#include "pq/tuning/scala.h"

namespace pq::doc {

namespace {

constexpr int64_t kNoKey = -1;

// One note as written: file ticks, its MTrk (1-based: 0 is the tempo track), channel, key, bend.
struct OutNote {
  int64_t on = 0, off = 0;
  uint32_t track = 0;
  uint8_t key = 0, channel = 0, velocity = 100, release = 64;
  uint16_t bend = 8192;
  bool dropped = false;  // MPE: stolen at its own note-on tick (zero length)
};

// Arrangement tick → file tick from `origin`: round((t − origin) · division / kPpq), exact in int64 (t ≥ origin).
struct TickMap {
  Ticks origin = 0;
  int64_t division = 960;
  int64_t operator()(Ticks t) const noexcept {
    const Ticks d = std::max<Ticks>(0, t - origin);
    const int64_t q = d / kPpq, r = d % kPpq;
    return q * division + (2 * r * division + kPpq) / (2 * kPpq);
  }
};

int64_t nearestKey(double hz) noexcept { return std::llround(69.0 + 12.0 * std::log2(hz / 440.0)); }

// Degrees mode: absolute degree → MIDI key (the lowest key the .kbm maps to it; without a .kbm, 60 + degree).
class DegreeKeys {
public:
  explicit DegreeKeys(const TuningDef* def) {
    const ScaleDef* s = def ? def->scale() : nullptr;
    if (!s || !s->mapping) return;
    kbm_ = true;
    for (int32_t k = 127; k >= 0; --k)
      if (const std::optional<int64_t> d = tuning::mapKeyToDegree(*s->mapping, k)) keyOf_[*d] = k;
  }
  int64_t key(int64_t degree) const {
    if (!kbm_) return 60 + degree;
    const auto it = keyOf_.find(degree);
    return it == keyOf_.end() ? kNoKey : it->second;
  }

private:
  bool kbm_ = false;
  std::unordered_map<int64_t, int64_t> keyOf_;
};

uint8_t clamp7(float v, int lo) noexcept {
  return static_cast<uint8_t>(std::clamp<long>(std::lround(v), lo, 127));
}

// MPE: member channels for notes in note-on order (shared by every clip: one port). A free channel (its note ended at or
// before the note-on) is taken round-robin from after the last one used; none free: the channel whose note started
// first, that note ending at the new note-on (dropped when it started on the same tick).
void allocateMpe(std::vector<OutNote>& notes, ExportMidiReport& rep) {
  std::array<int64_t, kMpeMembers> holder;  // index into notes of the channel's current note, −1 none
  holder.fill(-1);
  uint32_t next = 0;
  for (size_t i = 0; i < notes.size(); ++i) {
    OutNote& n = notes[i];
    int chosen = -1;
    for (uint32_t k = 0; k < kMpeMembers; ++k) {
      const uint32_t c = (next + k) % kMpeMembers;
      if (holder[c] < 0 || notes[static_cast<size_t>(holder[c])].off <= n.on) {
        chosen = static_cast<int>(c);
        break;
      }
    }
    if (chosen < 0) {
      for (uint32_t c = 0; c < kMpeMembers; ++c)
        if (chosen < 0 || notes[static_cast<size_t>(holder[c])].on < notes[static_cast<size_t>(holder[chosen])].on)
          chosen = static_cast<int>(c);
      OutNote& old = notes[static_cast<size_t>(holder[chosen])];
      old.off = n.on;
      old.dropped = old.on >= n.on;
      ++rep.stolen;
    }
    holder[chosen] = static_cast<int64_t>(i);
    n.channel = static_cast<uint8_t>(chosen + 1);
    next = static_cast<uint32_t>(chosen + 1) % kMpeMembers;
  }
}

uint32_t maxSounding(const std::vector<OutNote>& notes) {
  std::vector<std::pair<int64_t, int>> edges;
  edges.reserve(notes.size() * 2);
  for (const OutNote& n : notes) {
    edges.emplace_back(n.on, +1);
    edges.emplace_back(n.off, -1);
  }
  std::sort(edges.begin(), edges.end());  // at one tick ends (−1) before starts
  int cur = 0, best = 0;
  for (const auto& e : edges) best = std::max(best, cur += e.second);
  return static_cast<uint32_t>(best);
}

Error invalidArg(std::string m) { return Error{ErrorCode::InvalidArgument, std::move(m), {}}; }

}  // namespace

Result<std::vector<uint8_t>> exportMidi(const Project& p, std::span<const ClipRef> clips, const ExportMidiOptions& opt,
                                        ExportMidiReport* report) {
  if (clips.empty()) return invalidArg("no clips to export");
  if (opt.division == 0 || opt.division > 32767) return invalidArg("MIDI export: the division must be 1 … 32767");
  ExportMidiReport rep;
  struct Found {
    ops::detail::ClipFound f;
    ClipPitches pitches;
  };
  std::vector<Found> found;
  found.reserve(clips.size());
  Ticks origin = std::numeric_limits<Ticks>::max(), end = std::numeric_limits<Ticks>::min();
  for (const ClipRef& r : clips) {
    Result<ops::detail::ClipFound> f = ops::detail::findClip(p, r);
    if (!f) return std::move(f).error();
    Result<ClipPitches> cp = resolveClipPitches(p, r.track, r.clip);
    if (!cp) return std::move(cp).error();
    origin = std::min(origin, f->clip->start);
    end = std::max(end, clipEnd(*f->clip));
    found.push_back({std::move(*f), std::move(*cp)});
  }
  const TickMap fileTick{origin, opt.division};

  // 1. The notes as they play, keyed by mode.
  std::vector<OutNote> notes;
  for (uint32_t ci = 0; ci < found.size(); ++ci) {
    const NoteClip& c = *found[ci].f.clip;
    const ClipPitches& cp = found[ci].pitches;
    const DegreeKeys degreeKeys(opt.mode == MidiExportMode::Degrees ? cp.tuning.def : nullptr);
    std::vector<const Note*> order;  // the clip's note order (start, id) = cp.notes' order
    order.reserve(c.notes.size());
    for (const Note& n : c.notes) order.push_back(&n);
    forEachClipSegment(c, c.start, clipEnd(c), [&](const ClipSegment& seg) {
      auto it = std::lower_bound(order.begin(), order.end(), seg.contentFrom,
                                 [](const Note* n, Ticks t) { return n->start < t; });
      for (; it != order.end() && (*it)->start < seg.contentTo; ++it) {
        const Note& n = **it;
        if (!n.enabled()) continue;
        const NotePitch& np = cp.notes[static_cast<size_t>(it - order.begin())];
        if (!np.hz || !(*np.hz > 0.0)) {
          ++rep.unresolved;
          continue;
        }
        OutNote o;
        int64_t key = kNoKey;
        switch (opt.mode) {
          case MidiExportMode::TwelveEdo:
            key = nearestKey(*np.hz);
            break;
          case MidiExportMode::Mpe: {
            key = nearestKey(*np.hz);
            const double semis = 12.0 * std::log2(*np.hz / twelveTetHz(static_cast<double>(key)));
            const long v = std::lround(8192.0 + semis * 8192.0 / kMpeBendSemitones);
            o.bend = static_cast<uint16_t>(std::clamp<long>(v, 0, 16383));
            break;
          }
          case MidiExportMode::Degrees: {
            if (!np.keyDegree) {
              ++rep.unresolved;
              continue;
            }
            const double d = std::round(*np.keyDegree);
            if (std::fabs(*np.keyDegree - d) > 1e-9) ++rep.snapped;
            key = std::fabs(d) < 1e9 ? degreeKeys.key(static_cast<int64_t>(d)) : kNoKey;
            break;
          }
        }
        if (key < 0 || key > 127) {
          ++rep.outOfRange;
          continue;
        }
        const Ticks at = seg.at + (n.start - seg.contentFrom);
        o.on = fileTick(at);
        o.off = std::max(o.on + 1, fileTick(at + std::min(n.length, seg.contentTo - n.start)));
        o.track = ci + 1;
        o.key = static_cast<uint8_t>(key);
        o.velocity = clamp7(n.velocity, 1);
        o.release = clamp7(n.releaseVelocity, 0);
        notes.push_back(o);
      }
    });
  }
  std::stable_sort(notes.begin(), notes.end(), [](const OutNote& a, const OutNote& b) { return a.on < b.on; });
  if (opt.mode == MidiExportMode::Mpe) {
    rep.maxVoices = maxSounding(notes);
    allocateMpe(notes, rep);
  }

  // 2. The tracks.
  MidiFile file;
  file.format = 1;
  file.division = MidiDivision{opt.division, 0, 0};
  file.tracks.resize(found.size() + 1);
  const Timeline& tl = *p.timeline;
  {
    MidiTrack& t0 = file.tracks[0];
    uint32_t last = 0;
    auto put = [&](Ticks t, double bpm) {
      const auto us = static_cast<uint32_t>(std::clamp<long long>(std::llround(60'000'000.0 / bpm), 1, 0xFFFFFF));
      if (us == last) return;
      t0.tempo(fileTick(t), us);
      last = us;
      ++rep.tempoEvents;
    };
    const std::vector<TempoPoint>& pts = tl.tempo;  // the first at tick 0; before it its tempo continues
    for (size_t i = 0; i < pts.size(); ++i) {
      const Ticks a = i == 0 ? std::numeric_limits<Ticks>::min() : pts[i].tick;
      const Ticks b = i + 1 < pts.size() ? pts[i + 1].tick : std::numeric_limits<Ticks>::max();
      Ticks lo = std::max(a, origin);
      const Ticks hi = std::min(b, end);
      if (lo >= hi) continue;
      if (pts[i].curve != TempoCurve::Linear || i + 1 == pts.size() || lo < pts[i].tick) put(lo, pts[i].bpm);
      if (pts[i].curve != TempoCurve::Linear || i + 1 == pts.size()) continue;
      constexpr Ticks kStep = kPpq / 4;  // a sixteenth note
      for (Ticks t = std::max(lo, pts[i].tick); t < hi;) {
        const Ticks t2 = std::min(hi, pts[i].tick + ((t - pts[i].tick) / kStep + 1) * kStep);
        const double sec = tl.tempoMap.ticksToSeconds(t2) - tl.tempoMap.ticksToSeconds(t);
        put(t, sec > 0.0 ? 60.0 * (static_cast<double>(t2 - t) / static_cast<double>(kPpq)) / sec : pts[i].bpm);
        t = t2;
      }
    }
    const TimeSigMap& sm = tl.sigMap;
    for (size_t i = sm.changeIndexAt(origin); i < sm.changeCount(); ++i) {
      const Ticks at = std::max(sm.changeStartTick(i), origin);
      if (at >= end && i != sm.changeIndexAt(origin)) break;
      const TimeSignature s = sm.changeSignature(i);
      const bool pow2 = s.denominator > 0 && (s.denominator & (s.denominator - 1)) == 0 && s.denominator <= 128;
      if (!pow2 || s.numerator < 1 || s.numerator > 255) {
        ++rep.skippedSignatures;
        continue;
      }
      t0.timeSignature(fileTick(at), static_cast<uint8_t>(s.numerator), static_cast<uint8_t>(s.denominator),
                       static_cast<uint8_t>(std::max(1, 96 / s.denominator)), 8);
      ++rep.signatures;
    }
    t0.endTick = fileTick(end);
  }
  // Per clip: its name, the MPE setup, then its notes (at one tick: note-offs, then bends, then note-ons).
  struct Ev {
    int64_t tick;
    uint8_t order;  // 0 off, 1 bend, 2 on
    uint32_t note;
  };
  std::vector<std::vector<Ev>> evs(file.tracks.size());
  for (uint32_t i = 0; i < notes.size(); ++i) {
    const OutNote& n = notes[i];
    if (n.dropped) continue;
    std::vector<Ev>& e = evs[n.track];
    e.push_back({n.off, 0, i});
    if (opt.mode == MidiExportMode::Mpe) e.push_back({n.on, 1, i});
    e.push_back({n.on, 2, i});
    ++rep.notes;
  }
  for (uint32_t ti = 1; ti < file.tracks.size(); ++ti) {
    const NoteClip& c = *found[ti - 1].f.clip;
    MidiTrack& t = file.tracks[ti];
    if (!c.name.empty()) t.name(c.name);
    if (opt.mode == MidiExportMode::Mpe) {
      t.rpn(0, 0, 6, kMpeMembers);  // MCM: lower zone, 15 members
      for (uint8_t ch = 1; ch <= kMpeMembers; ++ch) t.rpn(0, ch, 0, kMpeBendSemitones, 0);
    }
    std::vector<Ev>& e = evs[ti];
    std::stable_sort(e.begin(), e.end(), [](const Ev& a, const Ev& b) {
      return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });
    for (const Ev& v : e) {
      const OutNote& n = notes[v.note];
      switch (v.order) {
        case 0: t.noteOff(v.tick, n.channel, n.key, n.release); break;
        case 1: t.pitchBend(v.tick, n.channel, n.bend); break;
        default: t.noteOn(v.tick, n.channel, n.key, n.velocity); break;
      }
    }
    t.endTick = fileTick(clipEnd(c));
  }
  rep.clips = static_cast<uint32_t>(found.size());
  if (report) *report = rep;
  return writeMidiFile(file);
}

std::string describeMidiExport(const ExportMidiReport& r, MidiExportMode mode) {
  std::string s = "Exported " + std::to_string(r.notes) + (r.notes == 1 ? " note" : " notes") + " (" +
                  std::string(kMidiExportModeNames[static_cast<size_t>(mode)]) + ")";
  auto add = [&](uint32_t n, const char* what) {
    if (n) s += ", " + std::to_string(n) + " " + what;
  };
  add(r.stolen, "voices over 15 ended early");
  add(r.snapped, "snapped to the nearest degree");
  add(r.outOfRange, "out of the key range");
  add(r.unresolved, "without a pitch");
  add(r.skippedSignatures, "signatures MIDI cannot hold");
  return s;
}

}  // namespace pq::doc
