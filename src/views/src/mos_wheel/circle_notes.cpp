// TU.2: the MOS circle's lit notes. See circle_notes.h.

#include "circle_notes.h"

#include <algorithm>
#include <cmath>

#include "pq/document/clip_time.h"
#include "pq/document/pitch_resolve.h"
#include "pq/views/mos_wheel/mos_circle.h"
#include "pq/views/mos_wheel/mos_wheel.h"

namespace pq::views {

namespace {

constexpr uint64_t kPlayTag = uint64_t{2} << 56;
constexpr uint64_t kTagMask = (uint64_t{1} << 56) - 1;

uint64_t playId(doc::ClipId clip, doc::NoteId note) noexcept {
  return kPlayTag | ((uint64_t{clip.value} * 0x9E3779B97F4A7C15ull ^ uint64_t{note.value}) & kTagMask);
}

}  // namespace

CircleNotes::CircleNotes(ViewContext& ctx, const TuningTarget& target, MosCircle& circle, ui::Widget& host, doc::TrackId only)
    : ctx_(ctx), target_(target), only_(only), circle_(circle), host_(host) {
  viewSub_ = ctx_.observe([this](uint32_t bits) { onView(bits); });
  docSub_ = ctx_.subscribe([this](const doc::StoreEvent&) { playDirty_ = true; });
  auditionSeen_ = ctx_.lastAudition().serial;  // an audition from before the wheel opened is not shown
  ctx_.transport().listenMidiNotes(true);
  syncKeyboard();
  syncMidi();
  onView(view_change::kTransport);
}

CircleNotes::~CircleNotes() { ctx_.transport().listenMidiNotes(false); }

const tuning::Tuning* CircleNotes::targetTuning() {
  const doc::TuningDef* def = views::targetTuning(*ctx_.doc(), target_);
  if (!def) return nullptr;
  if (!builtDef_ || !(*builtDef_ == *def)) {
    builtDef_ = *def;
    Result<tuning::Tuning> t = doc::buildTuning(*def);
    built_.reset();
    if (t.ok()) built_ = std::move(*t);
  }
  return built_ ? &*built_ : nullptr;
}

void CircleNotes::apply(std::vector<Lit>& now, std::vector<uint64_t>& was) {
  for (const Lit& l : now)
    if (std::find(was.begin(), was.end(), l.id) == was.end()) circle_.noteOn(l.id, l.degree, l.hz);
  for (uint64_t id : was)
    if (std::none_of(now.begin(), now.end(), [id](const Lit& l) { return l.id == id; })) circle_.noteOff(id);
  was.clear();
  for (const Lit& l : now) was.push_back(l.id);
}

void CircleNotes::onView(uint32_t bits) {
  if (bits & view_change::kLiveNotes) {
    syncKeyboard();
    syncAudition();
    syncMidi();
  }
  if ((bits & view_change::kTransport) && ctx_.playing() && !polling_) {
    polling_ = true;
    host_.requestFrame();
  }
}

// --- the computer keyboard ---------------------------------------------------------------------------------------

void CircleNotes::syncKeyboard() {
  scratch_.clear();
  const ComputerKeyboard& kb = ctx_.computerKeyboard();
  if (!kb.held().empty()) {
    // Does a track that listens to the keyboard play in the target's tuning (its live clip's)?
    const doc::Project& p = *ctx_.doc();
    const doc::TuningDef* def = views::targetTuning(p, target_);
    const doc::TrackId autoArm = ctx_.autoArmTrack();
    bool listens = false;
    if (def)
      doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
        if (listens || t.kind != doc::TrackKind::Midi || !(t.mixer.arm || t.id == autoArm) || skips(t.id)) return;
        // A 12-TET key map plays semitones, not the tuning's degrees: nothing to light.
        if (t.noteInput.keyMap == doc::InputKeyMap::TwelveTetNearest || t.noteInput.keyMap == doc::InputKeyMap::TwelveTetHz) return;
        const std::optional<doc::TrackPath> path = doc::findTrack(p, t.id);
        listens = path && doc::effectiveTuning(p, *path, ctx_.liveClipOf(t).clip) == def;
      });
    const tuning::Tuning* tun = listens ? targetTuning() : nullptr;
    if (tun)
      for (const ComputerKeyboard::Held& h : kb.held()) {
        const std::optional<double> hz = tuning::resolveHzDouble(tuning::PitchId::fromDegree(h.step, h.equave), *tun, {});
        scratch_.push_back({keyId(h.key), h.step, hz.value_or(0.0)});
      }
  }
  apply(scratch_, keysLit_);
}

// --- hardware MIDI in (TU.2b) ----------------------------------------------------------------------------------

void CircleNotes::syncMidi() {
  scratch_.clear();
  const std::span<const LiveMidiNote> notes = ctx_.transport().liveMidiNotes();
  if (!notes.empty()) {
    const doc::Project& p = *ctx_.doc();
    const doc::TuningDef* def = views::targetTuning(p, target_);
    // Per note its track's live tuning (a handful of held notes: the lookups are cheap; one track per run of notes).
    uint32_t lastTrack = 0;
    bool lastPlays = false;
    for (const LiveMidiNote& n : notes) {
      if (!def || !std::isfinite(n.noteDegree) || !(n.hz > 0.0f) || skips(doc::TrackId{n.trackId})) continue;
      if (n.trackId != lastTrack) {
        lastTrack = n.trackId;
        lastPlays = false;
        const std::optional<doc::TrackPath> path = doc::findTrack(p, doc::TrackId{n.trackId});
        if (path) {
          const doc::Track& t = doc::trackAt(p, *path);
          lastPlays = doc::effectiveTuning(p, *path, ctx_.liveClipOf(t).clip) == def;
        }
      }
      if (lastPlays) scratch_.push_back({midiId(n), std::llround(n.noteDegree), static_cast<double>(n.hz)});
    }
  }
  apply(scratch_, midiLit_);
}

// --- auditions -------------------------------------------------------------------------------------------------

void CircleNotes::syncAudition() {
  const ViewContext::Audition& a = ctx_.lastAudition();
  if (a.serial != auditionSeen_) {
    auditionSeen_ = a.serial;
    auditionReleased_ = a.released;
    if (!a.degree || !a.clip.clip.valid() || skips(a.clip.track)) return;
    const doc::Project& p = *ctx_.doc();
    const std::optional<doc::TrackPath> path = doc::findTrack(p, a.clip.track);
    if (!path) return;
    const doc::Track& t = doc::trackAt(p, *path);
    const int32_t i = doc::findClipIndex(t, a.clip.clip);
    const doc::NoteClip* c = i >= 0 ? t.clips[static_cast<size_t>(i)].clip.get() : nullptr;
    if (!c || doc::effectiveTuning(p, *path, c) != views::targetTuning(p, target_)) return;
    circle_.noteOn(auditionId(a.serial), *a.degree, a.hz, std::max(0.05, static_cast<double>(a.durationMs) / 1000.0));
    if (a.released) circle_.noteOff(auditionId(a.serial));
  } else if (a.released && !auditionReleased_) {
    auditionReleased_ = true;
    circle_.noteOff(auditionId(a.serial));
  }
}

// --- playback --------------------------------------------------------------------------------------------------

void CircleNotes::rebuildPlayback() {
  play_.clear();
  const doc::Project& p = *ctx_.doc();
  const doc::TuningDef* def = views::targetTuning(p, target_);
  if (!def) return;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    if (t.kind != doc::TrackKind::Midi || skips(t.id)) return;
    const std::optional<doc::TrackPath> path = doc::findTrack(p, t.id);
    if (!path) return;
    const std::vector<doc::ClipSlot>& clips = doc::playedClips(t);
    // Only a track with a clip in the target's tuning is resolved (resolveTrackPitches builds tunings).
    const bool any = std::any_of(clips.begin(), clips.end(), [&](const doc::ClipSlot& c) {
      return c.clip && c.clip->active && doc::effectiveTuning(p, *path, c.clip.get()) == def;
    });
    if (!any) return;
    Result<doc::TrackPitches> tp = doc::resolveTrackPitches(p, t.id);
    if (!tp.ok()) return;
    for (const doc::ClipPitches& cp : tp->clips) {
      if (cp.tuning.def != def) continue;
      const auto slot = std::find_if(clips.begin(), clips.end(), [&](const doc::ClipSlot& c) { return c.clip && c.id == cp.clip; });
      if (slot == clips.end() || !slot->clip->active) continue;
      PlayClip pc;
      pc.clip = slot->clip;
      const doc::NoteSeq& seq = pc.clip->notes;
      size_t k = 0;
      for (auto it = seq.begin(); it != seq.end() && k < cp.notes.size(); ++it, ++k) {
        const doc::Note& n = *it;
        const doc::NotePitch& np = cp.notes[k];
        if (!n.enabled() || np.note != n.id || !np.hz || !np.keyDegree) continue;
        pc.notes.push_back({n.start, n.start + n.length, playId(pc.clip->id, n.id), std::llround(*np.keyDegree), *np.hz});
        pc.maxLength = std::max(pc.maxLength, n.length);
      }
      if (!pc.notes.empty()) play_.push_back(std::move(pc));
    }
  });
}

void CircleNotes::pollPlayback(Ticks tick) {
  scratch_.clear();
  for (const PlayClip& pc : play_) {
    const doc::NoteClip& c = *pc.clip;
    if (tick < c.start || tick >= doc::clipEnd(c)) continue;
    // The pass the playhead is in: the last segment of [clip start, tick + 1) starts where that pass starts.
    ClipSegment pass{};
    bool found = false;
    doc::forEachClipSegment(c, c.start, tick + 1, [&](const ClipSegment& s) {
      pass = s;
      found = true;
    });
    if (!found || tick < pass.at) continue;
    const Ticks ct = pass.contentFrom + (tick - pass.at);
    const Ticks from = std::max(pass.contentFrom, ct - pc.maxLength + 1);  // earlier notes have ended
    auto first = std::lower_bound(pc.notes.begin(), pc.notes.end(), from, [](const PlayNote& n, Ticks t) { return n.start < t; });
    for (auto it = first; it != pc.notes.end() && it->start <= ct; ++it)
      if (it->end > ct) scratch_.push_back({it->id, it->degree, it->hz});
  }
  apply(scratch_, playLit_);
}

void CircleNotes::stopPlayback() {
  scratch_.clear();
  apply(scratch_, playLit_);
  lastSequence_ = ~uint64_t{0};
}

void CircleNotes::frame(double now) {
  if (!polling_) return;
  const PlayheadSnapshot ph = ctx_.transport().playhead();
  if (!ph.playing) {
    polling_ = false;
    stopPlayback();
    return;
  }
  host_.requestFrame();
  if (ph.countingIn) return;
  if (playDirty_ && (lastBuild_ < 0.0 || now - lastBuild_ >= kRebuildSeconds)) {
    playDirty_ = false;
    lastBuild_ = now;
    rebuildPlayback();
    lastSequence_ = ~uint64_t{0};
  }
  if (ph.sequence == lastSequence_) return;
  lastSequence_ = ph.sequence;
  pollPlayback(ph.tick);
}

}  // namespace pq::views
