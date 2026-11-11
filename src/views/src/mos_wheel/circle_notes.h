#pragma once

// TU.2: what lights the MOS wheel's circle — every note that sounds in the tuning the wheel edits (the very TuningDef
// its target plays: the clip's, the track's or the main tuning's) becomes MosCircle::noteOn / noteOff of its degree in
// the colour of its Hz. Read on the UI side only (no engine or audio-thread work):
//   - playback: while the transport plays, at each new playhead report, the notes of the clips that play in that
//     tuning under the playhead (the played clips of every MIDI track, doc::forEachClipSegment: the pass the playhead
//     is in, a note sounding from its start for its length, cut at the pass end), their degree and Hz from
//     doc::resolveTrackPitches (cached; rebuilt at most every kRebuildSeconds while the document changes);
//   - the computer keyboard: its held notes (ComputerKeyboard::held, view_change::kLiveNotes) when an armed (or
//     auto-armed) MIDI track that listens to it plays in that tuning (its live clip's, doc::liveClip);
//   - auditions: a note clicked or drawn in the piano roll (ViewContext::audition), when its clip plays in that tuning,
//     for its audition length or until the press ends.
//   - hardware MIDI in (TU.2b): the notes TransportPort::liveMidiNotes() holds (the app's UI-side feed off the MIDI
//     callback, routed as the engine routes them), on a track whose live clip plays in that tuning, at their degree
//     (LiveMidiNote::noteDegree; none with a 12-TET Hz key map) and Hz. The feed runs only while a CircleNotes
//     listens (TransportPort::listenMidiNotes).
// UI thread only.

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "pq/document/model.h"
#include "pq/tuning/tuning.h"
#include "pq/views/view_context.h"

namespace pq::views {

class MosCircle;

class CircleNotes {
public:
  static constexpr double kRebuildSeconds = 0.1;
  // `host` asks for the frames (the popover; it forwards onFrame to frame()).
  CircleNotes(ViewContext& ctx, const TuningTarget& target, MosCircle& circle, ui::Widget& host);
  ~CircleNotes();
  CircleNotes(const CircleNotes&) = delete;
  CircleNotes& operator=(const CircleNotes&) = delete;

  void frame(double now);

  // Ids of the notes lit (tests): one per sounding playback note, held key, audition.
  static uint64_t keyId(uint32_t key) noexcept { return (uint64_t{1} << 56) | key; }
  static uint64_t auditionId(uint64_t serial) noexcept { return (uint64_t{3} << 56) | (serial & ((uint64_t{1} << 56) - 1)); }
  static uint64_t midiId(const LiveMidiNote& n) noexcept {
    return (uint64_t{4} << 56) | (uint64_t{n.trackId} << 24) | (uint64_t{n.port} << 16) | (uint64_t{n.channel} << 8) | n.key;
  }

private:
  struct PlayNote {
    Ticks start = 0, end = 0;
    uint64_t id = 0;
    int64_t degree = 0;
    double hz = 0.0;
  };
  struct PlayClip {
    doc::ClipPtr clip;
    std::vector<PlayNote> notes;  // by start
    Ticks maxLength = 0;          // the longest note: a poll scans only notes starting within it of the playhead
  };
  struct Lit {
    uint64_t id;
    int64_t degree;
    double hz;
  };

  void onView(uint32_t bits);
  void syncKeyboard();
  void syncAudition();
  void syncMidi();
  void rebuildPlayback();
  void pollPlayback(Ticks tick);
  void stopPlayback();
  // Diffs `now` against `was` (both by id): noteOn for the new ones, noteOff for the gone ones; was = now.
  void apply(std::vector<Lit>& now, std::vector<uint64_t>& was);
  const tuning::Tuning* targetTuning();  // built from the target's definition (cached on it)

  ViewContext& ctx_;
  TuningTarget target_;
  MosCircle& circle_;
  ui::Widget& host_;
  ViewContext::Subscription viewSub_, docSub_;

  std::optional<doc::TuningDef> builtDef_;
  std::optional<tuning::Tuning> built_;

  std::vector<uint64_t> keysLit_;
  std::vector<uint64_t> midiLit_;
  uint64_t auditionSeen_ = 0;
  bool auditionReleased_ = true;

  bool polling_ = false;
  bool playDirty_ = true;
  double lastBuild_ = -1.0;
  uint64_t lastSequence_ = ~uint64_t{0};
  std::vector<PlayClip> play_;
  std::vector<uint64_t> playLit_;
  std::vector<Lit> scratch_;
};

}  // namespace pq::views
