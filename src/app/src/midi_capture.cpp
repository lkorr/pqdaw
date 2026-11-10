// T6.9: Capture MIDI's clips (pq/app/midi_capture.h).
#include "pq/app/midi_capture.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "pq/media/tempo_detect.h"

namespace pq::app {

namespace {

using engine::CapturedNote;
using Notes = std::vector<const CapturedNote*>;

int64_t endOf(const CapturedNote& n, int64_t now) noexcept {
  return n.endTime == engine::kCaptureOpen ? std::max(now, n.engineTime + 1) : n.endTime;
}

// The notes after the last silence of at least `gap` frames (a note's end to the next note-on), by note-on time.
Notes lastPhrase(Notes v, int64_t gap, int64_t now) {
  std::stable_sort(v.begin(), v.end(), [](const CapturedNote* a, const CapturedNote* b) { return a->engineTime < b->engineTime; });
  size_t begin = 0;
  int64_t maxEnd = std::numeric_limits<int64_t>::min();
  for (size_t i = 0; i < v.size(); ++i) {
    if (i > 0 && v[i]->engineTime - maxEnd >= gap) begin = i;
    maxEnd = std::max(maxEnd, endOf(*v[i], now));
  }
  return Notes(v.begin() + static_cast<std::ptrdiff_t>(begin), v.end());
}

Ticks barFloor(const TimeSigMap& sm, Ticks t) noexcept { return sm.barToTicks(sm.ticksToBarBeat(t).bar); }
Ticks barCeil(const TimeSigMap& sm, Ticks t) noexcept {
  const Ticks f = barFloor(sm, t);
  return f == t ? t : sm.barToTicks(sm.ticksToBarBeat(t).bar + 1);
}

tuning::PitchId identityOf(const CapturedNote& n, const CaptureTrack& ct) {
  if (ct.hzMap && n.bendCents != 0.0f && n.raw.port != engine::kLiveKeyboard) {
    const doc::InputPitch ip = ct.hzMap->atNoteOn(n.raw.d1, n.bendCents);
    if (ip.mapped) return ip.id;
  }
  return n.id;
}

doc::Note noteOf(const CapturedNote& n, const CaptureTrack& ct, Ticks start, Ticks length) {
  doc::Note out;
  out.start = start;
  out.length = std::max<Ticks>(1, length);
  out.pitch = identityOf(n, ct);
  out.velocity = std::clamp(n.velocity, 1.0f, 127.0f);
  out.releaseVelocity = n.endTime == engine::kCaptureOpen ? 64.0f : std::clamp(n.release, 0.0f, 127.0f);
  return out;
}

}  // namespace

CapturePlan planCapture(const doc::Project& p, std::span<const CaptureTrack> tracks, const CaptureTiming& t) {
  CapturePlan plan;
  const TempoMap& tm = p.timeline->tempoMap;
  const TimeSigMap& sm = p.timeline->sigMap;
  const double sr = t.sampleRate;
  const int64_t oldest = t.now - static_cast<int64_t>(engine::kCaptureSeconds * sr);
  // The silence that ends a phrase: 2 bars at the song tempo where the capture looks (the playhead's song position is
  // not known here: the insert marker's; the gap only separates phrases, it need not be exact).
  const Ticks at = std::max<Ticks>(0, t.insertMarker);
  const Ticks twoBars = 2 * ticksPerBar(sm.signatureAt(at));
  const auto gap = static_cast<int64_t>(std::llround((tm.ticksToSeconds(at + twoBars) - tm.ticksToSeconds(at)) * sr));

  struct Stopped {
    const CaptureTrack* track;
    Notes notes;
  };
  std::vector<Stopped> stopped;
  const int64_t loopS0 = tm.ticksToSamples(t.loopStart, sr), loopS1 = tm.ticksToSamples(t.loopEnd, sr);
  for (const CaptureTrack& ct : tracks) {
    Notes cand;
    for (const CapturedNote& n : ct.notes)
      if (n.engineTime >= oldest && (!t.playing || n.songSample >= 0)) cand.push_back(&n);
    if (cand.empty()) continue;
    Notes phrase = lastPhrase(std::move(cand), gap, t.now);
    const bool songTimed = std::all_of(phrase.begin(), phrase.end(), [](const CapturedNote* n) { return n->songSample >= 0; });
    if (!songTimed) {
      stopped.push_back({&ct, std::move(phrase)});
      continue;
    }
    // On the song ticks it was played against (Record's law; see the file comment).
    std::vector<doc::Note> notes;
    Ticks lo = std::numeric_limits<Ticks>::max(), hi = 0;
    for (const CapturedNote* n : phrase) {
      int64_t song = n->songSample - t.noteBack;
      if (t.loop && loopS1 > loopS0 && n->songSample >= loopS0 && n->songSample < loopS1 && song < loopS0)
        song += loopS1 - loopS0;  // heard before the wrap the note played after
      song -= t.pdc;
      const int64_t endSong = song + (endOf(*n, t.now) - n->engineTime);
      const auto s = std::max<Ticks>(0, static_cast<Ticks>(std::llround(tm.samplesToTicks(song, sr))));
      const auto e = std::max<Ticks>(s + 1, static_cast<Ticks>(std::llround(tm.samplesToTicks(endSong, sr))));
      notes.push_back(noteOf(*n, ct, s, e - s));
      lo = std::min(lo, s);
      hi = std::max(hi, e);
    }
    doc::ops::CapturedClip c;
    c.track = ct.track;
    c.start = barFloor(sm, lo);
    c.length = std::max(barCeil(sm, hi), c.start + ticksPerBar(sm.signatureAt(c.start))) - c.start;
    for (doc::Note& n : notes) n.start -= c.start;
    c.notes = std::move(notes);
    plan.clips.push_back(std::move(c));
  }
  if (stopped.empty()) return plan;

  // Stopped: the tempo from every stopped phrase's onsets; the first onset is the clip's downbeat.
  int64_t t0 = std::numeric_limits<int64_t>::max(), tEnd = std::numeric_limits<int64_t>::min();
  for (const Stopped& s : stopped)
    for (const CapturedNote* n : s.notes) {
      t0 = std::min(t0, n->engineTime);
      tEnd = std::max(tEnd, endOf(*n, t.now));
    }
  std::vector<std::pair<double, float>> on;
  for (const Stopped& s : stopped)
    for (const CapturedNote* n : s.notes) on.emplace_back(static_cast<double>(n->engineTime - t0), n->velocity / 127.0f);
  std::sort(on.begin(), on.end());
  std::vector<double> onsets;
  std::vector<float> weights;
  for (const auto& [o, w] : on) {
    onsets.push_back(o);
    weights.push_back(w);
  }
  const media::TempoEstimate est = media::estimateTempo(onsets, weights, sr, tEnd - t0);
  plan.inferredBpm = est.bpm;
  const Ticks clipStart = barFloor(sm, at);
  const bool setTempo = est.bpm > 0.0 && !doc::ops::projectHasClips(p);
  if (setTempo) plan.bpm = std::round(est.bpm * 100.0) / 100.0;  // the tempo box's precision
  // A note dt frames after the first: at the new constant tempo, else at the song's tempo from the clip's start.
  const double startSec = tm.ticksToSeconds(clipStart);
  auto ticksAfter = [&](int64_t dt) -> Ticks {
    if (setTempo) return static_cast<Ticks>(std::llround(static_cast<double>(dt) / sr * *plan.bpm * kPpq / 60.0));
    return static_cast<Ticks>(std::llround(tm.secondsToTicks(startSec + static_cast<double>(dt) / sr))) - clipStart;
  };
  const TimeSignature sig = sm.signatureAt(clipStart);
  const Ticks bar = ticksPerBar(sig), beat = ticksPerBeat(sig);
  Ticks lastOnset = 0;
  std::vector<std::vector<doc::Note>> notes(stopped.size());
  for (size_t i = 0; i < stopped.size(); ++i)
    for (const CapturedNote* n : stopped[i].notes) {
      const Ticks s = ticksAfter(n->engineTime - t0);
      const Ticks e = ticksAfter(endOf(*n, t.now) - t0);
      notes[i].push_back(noteOf(*n, *stopped[i].track, s, e - s));
      lastOnset = std::max(lastOnset, s);
    }
  const int64_t bars = std::max<int64_t>(1, std::llround(static_cast<double>(lastOnset + beat) / static_cast<double>(bar)));
  for (size_t i = 0; i < stopped.size(); ++i) {
    doc::ops::CapturedClip c;
    c.track = stopped[i].track->track;
    c.start = clipStart;
    c.length = bars * bar;
    c.loop = true;
    c.notes = std::move(notes[i]);
    plan.clips.push_back(std::move(c));
  }
  return plan;
}

}  // namespace pq::app
