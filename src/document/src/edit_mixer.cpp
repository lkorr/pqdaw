// Mixer edit operations (T3.10): activator (mute), exclusive / additive solo and arm, a return's Pre/Post switch.

#include <algorithm>

#include "edit_common.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

bool canSend(TrackKind k) noexcept {
  return k == TrackKind::Midi || k == TrackKind::Audio || k == TrackKind::Group || k == TrackKind::Return;
}

const Send* sendTo(const Mixer& m, TrackId target) noexcept {
  for (const Send& s : m.sends)
    if (s.target == target) return &s;
  return nullptr;
}

}  // namespace

EditResult setTracksMute(const Project& p, std::span<const TrackId> tracks, bool mute) {
  Edit e = makeEdit(mute ? "Track Activator Off" : "Track Activator On");
  for (TrackId id : tracks) {
    const Track* t = findTrackPtr(p, id);
    if (!t) return notFound("track not found");
    if (t->kind == TrackKind::Master) return invalid("the master track has no activator");
    if (std::find(e.tracks.begin(), e.tracks.end(), id) != e.tracks.end()) continue;
    if (t->mixer.mute != mute) batchOf(e).commands.push_back(SetTrackMute{id, mute});
    e.tracks.push_back(id);
  }
  return e;
}

EditResult toggleTracksMute(const Project& p, std::span<const TrackId> tracks) {
  bool anyOn = false;
  for (TrackId id : tracks) {
    const Track* t = findTrackPtr(p, id);
    if (!t) return notFound("track not found");
    if (t->kind == TrackKind::Master) continue;  // a selected master is left alone
    anyOn = anyOn || !t->mixer.mute;
  }
  std::vector<TrackId> sel;
  sel.reserve(tracks.size());
  for (TrackId id : tracks)
    if (const Track* t = findTrackPtr(p, id); t->kind != TrackKind::Master) sel.push_back(id);
  return setTracksMute(p, sel, anyOn);
}

EditResult soloTrack(const Project& p, TrackId track, bool additive) {
  const Track* t = findTrackPtr(p, track);
  if (!t) return notFound("track not found");
  if (t->kind == TrackKind::Master) return invalid("the master track has no solo");
  const bool on = !t->mixer.solo;
  Edit e = makeEdit(on ? "Solo" : "Unsolo");
  e.tracks.push_back(track);
  batchOf(e).commands.push_back(SetTrackSolo{track, on});
  if (on && !additive)
    forEachTrack(p, [&](const Track& o, const Track*) {
      if (o.id != track && o.mixer.solo) batchOf(e).commands.push_back(SetTrackSolo{o.id, false});
    });
  return e;
}

EditResult armTrack(const Project& p, TrackId track, bool additive) {
  const Track* t = findTrackPtr(p, track);
  if (!t) return notFound("track not found");
  if (t->kind != TrackKind::Midi) return invalid("only MIDI tracks can be armed");
  const bool on = !t->mixer.arm;
  Edit e = makeEdit(on ? "Arm" : "Disarm");
  e.tracks.push_back(track);
  batchOf(e).commands.push_back(SetTrackArm{track, on});
  if (on && !additive)
    forEachTrack(p, [&](const Track& o, const Track*) {
      if (o.id != track && o.mixer.arm) batchOf(e).commands.push_back(SetTrackArm{o.id, false});
    });
  return e;
}

EditResult setReturnSendsPreFader(const Project& p, TrackId ret, bool preFader) {
  const Track* r = findTrackPtr(p, ret);
  if (!r) return notFound("track not found");
  if (r->kind != TrackKind::Return) return invalid("not a return track");
  Edit e = makeEdit(preFader ? "Sends Pre Fader" : "Sends Post Fader");
  e.tracks.push_back(ret);
  forEachTrack(p, [&](const Track& t, const Track*) {
    if (t.id == ret || !canSend(t.kind)) return;
    const Send* s = sendTo(t.mixer, ret);
    if ((s ? s->preFader : false) != preFader) batchOf(e).commands.push_back(SetSendPreFader{t.id, ret, preFader});
  });
  return e;
}

bool returnSendsPreFader(const Project& p, TrackId ret) noexcept {
  bool any = false, all = true;
  forEachTrack(p, [&](const Track& t, const Track*) {
    if (t.id == ret || !canSend(t.kind)) return;
    any = true;
    const Send* s = sendTo(t.mixer, ret);
    all = all && s && s->preFader;
  });
  return any && all;
}

}  // namespace pq::doc::ops
