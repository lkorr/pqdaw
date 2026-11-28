// RT.1: an audio track's Audio In — its source as a value, the menus' filter, the one-step edit (pq/document/audio_in.h).
#include "pq/document/audio_in.h"

#include <algorithm>
#include <utility>

#include "checks.h"
#include "edit_common.h"
#include "pq/document/wire_types.h"

namespace pq::doc {

namespace {

// The start fields of `w` only, as a cable onto an Audio In starts (everything else at its default).
Wire startOf(const Wire& w) {
  Wire s;
  if (w.fromMixerOut()) {
    s.mixerOut = w.mixerOut;
  } else if (w.fromNoteSource()) {
    s.noteSource = w.noteSource;
    s.port = w.port;
  } else {
    s.from = w.from;
    s.port = w.port;
  }
  return s;
}

bool oneStart(const Wire& s) noexcept {
  return int{s.from.valid()} + int{s.noteSource.valid()} + int{s.mixerOut.valid()} == 1;
}

}  // namespace

AudioFrom AudioFrom::extIn(AudioInput channels) noexcept {
  AudioFrom f;
  f.kind = channels.none() ? AudioFromKind::None : AudioFromKind::Ext;
  if (!channels.none()) f.ext = channels;
  return f;
}

AudioFrom AudioFrom::output(const Wire& start) {
  AudioFrom f;
  f.kind = AudioFromKind::Output;
  f.start = startOf(start);
  return f;
}

AudioFrom AudioFrom::resampling() noexcept {
  AudioFrom f;
  f.kind = AudioFromKind::Resampling;
  return f;
}

const Wire* audioInWireOf(const Project& p, TrackId track) noexcept {
  for (const Wire& w : wiresOf(p))
    if (w.recordInput == track && track.valid()) return &w;
  return nullptr;
}

AudioFrom audioFromOf(const Project& p, TrackId track) {
  const Track* t = findTrackPtr(p, track);
  if (t == nullptr || t->kind != TrackKind::Audio) return AudioFrom::none();
  if (const Wire* w = audioInWireOf(p, track)) {
    if (w->fromMixerOut() && p.master && w->mixerOut == p.master->id) return AudioFrom::resampling();
    return AudioFrom::output(*w);
  }
  return AudioFrom::extIn(t->mixer.audioIn);
}

// ---- AudioInSources ------------------------------------------------------------------------------------------------

AudioInSources::AudioInSources(const Project& p, TrackId track, const DeviceLibrary* lib)
    : p_(&p), track_(track), lib_(lib) {
  const Track* t = findTrackPtr(p, track);
  audio_ = t != nullptr && t->kind == TrackKind::Audio;
  if (!audio_) return;
  auto f = std::make_shared<detail::AudioFeeds>(p);
  fed_ = f->fedBy(track.value, true);
  if (p.master) {
    const std::vector<uint32_t> own = f->fedBy(track.value, false);
    resamplingLoops_ = std::binary_search(own.begin(), own.end(), p.master->id.value);
  }
  feeds_ = std::move(f);
}

bool AudioInSources::offersTrack(TrackId source) const noexcept {
  return audio_ && source.valid() && !std::binary_search(fed_.begin(), fed_.end(), source.value);
}

std::string_view AudioInSources::refusal(const AudioFrom& from) const {
  if (!audio_) return kAudioInTrack;
  switch (from.kind) {
    case AudioFromKind::None:
    case AudioFromKind::Ext: return {};
    case AudioFromKind::Resampling:
      if (!p_->master) return "the project has no master";
      return resamplingLoops_ ? kAudioInLoop : std::string_view{};
    case AudioFromKind::Output: break;
  }
  const Wire& s = from.start;
  if (!oneStart(s)) return "an Audio In's cable starts at one output";
  if (s.fromMixerOut() && p_->master && s.mixerOut == p_->master->id) return refusal(AudioFrom::resampling());
  const uint32_t src = feeds_->sourceTrack(s);
  if (src == 0 || findTrackPtr(*p_, TrackId{src}) == nullptr) return "the source was not found";
  if (!carriesAudio(*p_, s, lib_)) return kAudioInNotAudio;
  if (src == track_.value) return kAudioInSelf;
  return std::binary_search(fed_.begin(), fed_.end(), src) ? kAudioInLoop : std::string_view{};
}

std::string audioFromRefusal(const Project& p, TrackId track, const AudioFrom& from, const DeviceLibrary* lib) {
  return std::string(AudioInSources(p, track, lib).refusal(from));
}

// ---- The edit ------------------------------------------------------------------------------------------------------

namespace ops {

using namespace ops::detail;  // makeEdit, batchOf, invalid, notFound

EditResult setAudioFrom(const Project& p, IdSource& ids, TrackId track, const AudioFrom& from, const DeviceLibrary* lib) {
  const Track* t = findTrackPtr(p, track);
  if (t == nullptr) return notFound("track not found");
  if (t->kind != TrackKind::Audio) return invalid(std::string(kAudioInTrack));
  // (Normalised: an Output from the master's mixer is Resampling; an Ext of no channels is None.)
  AudioFrom want = from.kind == AudioFromKind::Output   ? AudioFrom::output(from.start)
                   : from.kind == AudioFromKind::Ext ? AudioFrom::extIn(from.ext)
                                                     : from;
  if (want.kind == AudioFromKind::Output && want.start.fromMixerOut() && p.master && want.start.mixerOut == p.master->id)
    want = AudioFrom::resampling();
  if (want.kind == AudioFromKind::Ext && !want.ext.none() &&
      uint32_t{want.ext.firstChannel} + want.ext.channels > kMaxInputChannels)
    return invalid("audio input channel out of range");
  if (want.kind == AudioFromKind::Ext && want.ext.channels > 2) return invalid("an audio input is mono or a stereo pair");
  if (const std::string_view why = AudioInSources(p, track, lib).refusal(want); !why.empty())
    return invalid(std::string(why));
  Edit e = makeEdit("Audio From");
  e.tracks.push_back(track);
  if (audioFromOf(p, track) == want) return e;  // already the source: nothing changes
  Batch& b = batchOf(e);
  if (const Wire* old = audioInWireOf(p, track)) b.commands.push_back(RemoveWire{old->id});
  const AudioInput ext = want.kind == AudioFromKind::Ext ? want.ext : AudioInput{};
  if (!(t->mixer.audioIn == ext)) b.commands.push_back(SetTrackAudioInput{track, ext});
  if (want.kind == AudioFromKind::Output || want.kind == AudioFromKind::Resampling) {
    Wire w = want.kind == AudioFromKind::Output ? want.start : Wire{};
    if (want.kind == AudioFromKind::Resampling) w.mixerOut = p.master->id;
    w.recordInput = track;
    w.id = ids.wire();
    e.wires.push_back(w.id);
    b.commands.push_back(AddWire{std::move(w)});
  }
  return e;
}

}  // namespace ops

}  // namespace pq::doc
