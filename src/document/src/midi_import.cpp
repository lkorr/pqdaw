// MIDI file import (MF.2): pq/document/midi_import.h.

#include "pq/document/midi_import.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "edit_common.h"
#include "pq/document/input_map.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

// round(t · num / den) for t ≥ 0, exact in int64 (r · num < den · num stays far below 2^63 for MIDI divisions).
int64_t mulDivRound(int64_t t, int64_t num, int64_t den) noexcept {
  const int64_t q = t / den, r = t % den;
  return q * num + (2 * r * num + den) / (2 * den);
}

// File ticks → ticks relative to the file's start.
class TickMap {
public:
  explicit TickMap(const MidiDivision& d) : d_(d) {}
  // SMPTE with an imported tempo map: seconds go through it (relative points from tick 0).
  void setTempo(TempoMap m) { tempo_ = std::move(m); }
  Ticks operator()(int64_t t) const noexcept {
    t = std::max<int64_t>(0, t);
    if (!d_.isSmpte()) return mulDivRound(t, kPpq, d_.ppq);
    if (tempo_) return std::llround(tempo_->secondsToTicks(static_cast<double>(t) / d_.ticksPerSecond()));
    // 120 BPM: 2 quarters per second; 29 = 29.97 drop-frame (30000 / 1001 fps).
    const int64_t fpsNum = d_.smpteFps == 29 ? 30000 : d_.smpteFps;
    const int64_t fpsDen = d_.smpteFps == 29 ? 1001 : 1;
    return mulDivRound(t, 2 * kPpq * fpsDen, fpsNum * d_.ticksPerFrame);
  }

private:
  MidiDivision d_;
  std::optional<TempoMap> tempo_;
};

// The file's tempo list as points relative to its start (a point at 0: the first tempo when it is at file tick 0,
// else the SMF default 120 BPM). SMPTE: positions are seconds, placed through the points before them.
std::vector<TempoPoint> relativeTempo(const MidiNotes& n, const MidiDivision& d) {
  std::vector<TempoPoint> pts{TempoPoint{0, 120.0, TempoCurve::Step}};
  double prevSec = 0.0;
  for (const MidiTempo& t : n.tempos) {
    const double bpm = std::clamp(t.bpm(), kMinBpm, kMaxBpm);
    Ticks at = 0;
    if (!d.isSmpte()) {
      at = mulDivRound(std::max<int64_t>(0, t.tick), kPpq, d.ppq);
    } else {
      const double sec = static_cast<double>(std::max<int64_t>(0, t.tick)) / d.ticksPerSecond();
      at = pts.back().tick + std::llround((sec - prevSec) * pts.back().bpm / 60.0 * static_cast<double>(kPpq));
      prevSec = sec;
    }
    if (at <= pts.back().tick)
      pts.back().bpm = bpm;  // the same tick (or rounded onto it): the later wins
    else
      pts.push_back(TempoPoint{at, bpm, TempoCurve::Step});
  }
  return pts;
}

// The first bar line at or after `t`.
Ticks barUp(const TimeSigMap& m, Ticks t) noexcept {
  const BarBeatTick b = m.ticksToBarBeat(t);
  return b.beat == 0 && b.tick == 0 ? t : m.barToTicks(b.bar + 1);
}

struct Dest {
  std::vector<uint32_t> notes;  // indices into MidiNotes::notes (in tick order)
  std::string name;
  TrackId track;
};

}  // namespace

Ticks midiPpqTicks(int64_t fileTick, uint16_t ppq) noexcept {
  return mulDivRound(std::max<int64_t>(0, fileTick), kPpq, std::max<uint16_t>(1, ppq));
}

EditResult importMidi(const Project& p, IdSource& ids, const MidiNotes& notes, const ImportMidiOptions& opt,
                      MidiImportReport* report) {
  MidiImportReport rep;
  rep.droppedControllers = notes.droppedControllers;
  rep.droppedPressure = notes.droppedPressure;
  rep.midNoteBends = notes.midNoteBends;
  if (notes.notes.empty()) return invalid("the MIDI file has no notes");
  if (opt.at < 0 || opt.at > kMaxTicks) return invalid("import position out of range");
  if (!opt.division.isSmpte() && opt.division.ppq == 0) return invalid("bad MIDI division");
  if (opt.division.isSmpte() && opt.division.ticksPerFrame == 0) return invalid("bad MIDI division");

  // 1. Where the notes go: format 0 = one track; else one per MTrk with notes.
  std::vector<Dest> dests;
  if (opt.format == 0) {
    Dest d;
    d.name = !notes.trackNames.empty() && !notes.trackNames[0].empty() ? notes.trackNames[0] : opt.name;
    d.notes.reserve(notes.notes.size());
    for (uint32_t i = 0; i < notes.notes.size(); ++i) d.notes.push_back(i);
    dests.push_back(std::move(d));
  } else {
    std::vector<int32_t> destOf(std::max<size_t>(notes.trackNotes.size(), 1), -1);
    for (const MidiNote& n : notes.notes)
      if (n.track >= destOf.size()) destOf.resize(n.track + 1u, -1);
    for (uint32_t i = 0; i < notes.notes.size(); ++i) {
      const uint16_t t = notes.notes[i].track;
      if (destOf[t] < 0) {
        destOf[t] = static_cast<int32_t>(dests.size());
        Dest d;
        d.name = t < notes.trackNames.size() && !notes.trackNames[t].empty() ? notes.trackNames[t] : opt.name;
        if (t < notes.trackNotes.size()) d.notes.reserve(notes.trackNotes[t]);
        dests.push_back(std::move(d));
      }
      dests[static_cast<size_t>(destOf[t])].notes.push_back(i);
    }
    // Tracks in file order (notes are in tick order: the first note of track 2 may come before track 1's).
    std::vector<Dest> sorted;
    sorted.reserve(dests.size());
    for (const int32_t i : destOf)
      if (i >= 0) sorted.push_back(std::move(dests[static_cast<size_t>(i)]));
    dests = std::move(sorted);
  }

  Edit e = makeEdit("Import MIDI");
  Snapshot snap = std::make_shared<const Project>(p);
  ChangeSet changes;
  auto take = [&](Command c) -> Status {
    Result<Snapshot> next = apply(snap, c, changes);
    if (!next) return std::move(next).error();
    snap = std::move(*next);
    batchOf(e).commands.push_back(std::move(c));
    return ok();
  };

  // 2. Tempo and signatures: only into a project without clips.
  TickMap toTicks(opt.division);
  const bool fileHasTiming = !notes.tempos.empty() || !notes.signatures.empty();
  if (fileHasTiming && projectHasClips(p)) rep.tempoKept = true;
  if (fileHasTiming && !rep.tempoKept) {
    const Timeline& tl = *p.timeline;
    if (!notes.tempos.empty()) {
      const std::vector<TempoPoint> rel = relativeTempo(notes, opt.division);
      if (opt.division.isSmpte()) {
        Result<TempoMap> m = TempoMap::create(rel);
        if (!m) return std::move(m).error();
        toTicks.setTempo(std::move(*m));
      }
      std::vector<TempoPoint> pts;
      for (const TempoPoint& q : tl.tempo)
        if (q.tick < opt.at) pts.push_back(q);
      for (TempoPoint q : rel) {
        q.tick += opt.at;
        pts.push_back(q);
      }
      PQ_DOC_TRY(take(SetTempoMap{std::move(pts)}));
      rep.tempoImported = true;
    }
    if (!notes.signatures.empty()) {
      // The project's changes before the drop point, then the file's (4/4 at its start when it has none there), each
      // on the bar line at or after it.
      std::vector<TimeSigChange> ch;
      std::vector<Ticks> chStart;
      for (size_t i = 0; i < tl.sigMap.changeCount(); ++i)
        if (tl.sigMap.changeStartTick(i) < opt.at || i == 0) {
          ch.push_back({tl.sigMap.changeBar(i), tl.sigMap.changeSignature(i)});
          chStart.push_back(tl.sigMap.changeStartTick(i));
        }
      auto place = [&](Ticks t, TimeSignature sig, bool fromFile) {
        const Ticks s0 = chStart.back();
        const Ticks bar = ticksPerBar(ch.back().sig);
        const Ticks k = (std::max<Ticks>(0, t - s0) + bar - 1) / bar;
        if (s0 + k * bar != t && fromFile) ++rep.movedSignatures;
        if (k == 0) {
          ch.back().sig = sig;
        } else {
          ch.push_back({ch.back().bar + k, sig});
          chStart.push_back(s0 + k * bar);
        }
      };
      bool first = true;
      for (const MidiTimeSignature& s : notes.signatures) {
        const TimeSignature sig{s.numerator, s.denominator};
        if (s.numerator < 1 || !noteValueIsExact(s.denominator)) {
          ++rep.skippedSignatures;
          continue;
        }
        if (first && s.tick > 0) place(opt.at, TimeSignature{}, false);  // the SMF default before the file's first
        first = false;
        place(opt.at + toTicks(s.tick), sig, true);
      }
      PQ_DOC_TRY(take(SetTimeSignatures{std::move(ch)}));
      rep.signaturesImported = true;
    }
  }

  // 3. The tracks: the drop's MIDI track takes the first; new MIDI tracks after it (or after `newTracksAfter`).
  const Track* target = opt.target.valid() ? findTrackPtr(*snap, opt.target) : nullptr;
  size_t firstNew = 0;
  if (target != nullptr && target->kind == TrackKind::Midi) {
    dests[0].track = opt.target;
    firstNew = 1;
  }
  TrackId after = opt.newTracksAfter.valid() ? opt.newTracksAfter : (target != nullptr ? opt.target : TrackId{});
  for (size_t i = firstNew; i < dests.size(); ++i) {
    EditResult t = createTrack(*snap, ids, TrackKind::Midi, after, dests[i].name);
    if (!t) return std::move(t).error();
    dests[i].track = t->tracks.at(0);
    after = dests[i].track;
    for (Command& c : batchOf(*t).commands) PQ_DOC_TRY(take(std::move(c)));
    ++rep.newTracks;
  }

  // 4. Notes → clips (pitch through each track's input map), one Capture-MIDI step.
  const TimeSigMap& sigs = snap->timeline->sigMap;
  std::vector<CapturedClip> clips;
  clips.reserve(dests.size());
  for (const Dest& d : dests) {
    Result<InputPitchMapPtr> map = buildInputPitchMap(*snap, d.track);
    if (!map) return std::move(map).error();
    const InputPitchMap& m = **map;
    CapturedClip c;
    c.track = d.track;
    c.start = opt.at;
    c.name = d.name;
    c.notes.reserve(d.notes.size());
    Ticks end = 1;
    for (const uint32_t i : d.notes) {
      const MidiNote& mn = notes.notes[i];
      Note n;
      n.start = toTicks(mn.tick);
      n.length = std::max<Ticks>(1, toTicks(mn.tick + mn.length) - n.start);
      n.velocity = static_cast<float>(std::max<uint8_t>(1, mn.velocity));
      n.releaseVelocity = static_cast<float>(mn.releaseVelocity);
      // AU.dsp (RB-DY M8, Q263 user 2026-11-13): one rule per FILE, no per-note test — a file with any bend, MTS or RPN
      // tuning is absolute pitch for every note; any other plays its keys through the track's key map.
      bool keyed = !notes.absolutePitch;
      if (keyed) {
        const InputPitch& k = m.at(mn.key);
        if (k.mapped) {
          n.pitch = k.id;
        } else {
          ++rep.unmappedKeys;
          keyed = false;
        }
      } else if (mn.cents != 0.0) {
        ++rep.bentNotes;
      }
      if (!keyed) {
        const double hz = twelveTetHz(static_cast<double>(mn.key)) * std::exp2(mn.cents / 1200.0);
        n.pitch = m.atHz(hz, kMidiDegreeSnapCents).id;
      }
      end = std::max(end, n.start + n.length);
      c.notes.push_back(n);
    }
    c.length = barUp(sigs, opt.at + end) - opt.at;
    rep.notes += static_cast<uint32_t>(c.notes.size());
    clips.push_back(std::move(c));
  }
  EditResult cap = captureClip(*snap, ids, clips);
  if (!cap) return std::move(cap).error();
  for (Command& c : batchOf(*cap).commands) batchOf(e).commands.push_back(std::move(c));
  e.clips = std::move(cap->clips);
  for (const Dest& d : dests) e.tracks.push_back(d.track);
  rep.tracks = static_cast<uint32_t>(dests.size());
  if (report != nullptr) *report = rep;
  return e;
}

}  // namespace pq::doc::ops
