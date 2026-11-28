#pragma once

// RT.1 (docs/plans/RT.md; model.h "Modulation wires": AUDIO IN, MIXER OUT): recording from a track, Live's "Audio From".
//
// ---- The Audio In ------------------------------------------------------------------------------------------------
// Every AUDIO track has one Audio In: its record and monitor input. Its source is exactly one of
//   None        nothing (Live's "No Input");
//   Ext         device input channels: the track's Mixer::audioIn (Ext. In; SetTrackAudioInput), no cable;
//   Output      ONE CABLE onto it (Wire::recordInput) from any audio output of the project: a device's / group's main
//               or named output (a rack chain and a chain mixer are groups), a track's own signal (its source:
//               Wire::noteSource, port kMainPort; Live's Pre FX), a track's mixer output (Wire::mixerOut; Live's Post
//               Mixer);
//   Resampling  the cable from the MASTER's mixer output (the master minus the recording track's own bus: RT.2).
// The arrangement's choosers and the circuit's cable are views of the same wire; nothing else stores the setting.
//
// ---- Loops -----------------------------------------------------------------------------------------------------------
// A track B FEEDS the track its output goes to, the returns of its stored sends and the track at the other end of every
// cable that leaves it (Audio In cables included), transitively (checks.h AudioFeeds). A source on B itself is refused
// (kAudioInSelf), one on a track B feeds is refused (kAudioInLoop). Resampling is refused only when B reaches the master
// other than through its own output route (a send, a cable, another track's Audio In). The rule is checked by the final
// pass of every structural command (and every send-level change) while the project has an Audio In cable, so a later
// edit (a reroute, a send, a cable, a moved device) cannot close a loop either; the loader checks it too.
//
// Not RT code (UI thread).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/edit_ops.h"
#include "pq/document/model.h"

namespace pq::doc {

class DeviceLibrary;
namespace detail {
struct AudioFeeds;
}

inline constexpr std::string_view kAudioInLoop = "Audio From would create a loop";
inline constexpr std::string_view kAudioInSelf = "a track cannot record from itself";
inline constexpr std::string_view kAudioInNotAudio = "Audio In takes audio";
inline constexpr std::string_view kAudioInOneSource = "an Audio In has one source (Ext. In or one cable)";
inline constexpr std::string_view kAudioInTrack = "only an audio track has an Audio In";

enum class AudioFromKind : uint8_t { None, Ext, Output, Resampling };

// An Audio In's source (above). Built by the factories, which normalise it, so equal settings compare equal.
struct AudioFrom {
  AudioFromKind kind = AudioFromKind::None;
  AudioInput ext;  // Ext: the device channels (non-empty)
  Wire start;      // Output: the cable's start only (from + port | noteSource, port "" | mixerOut); else empty
  static AudioFrom none() noexcept { return {}; }
  static AudioFrom extIn(AudioInput channels) noexcept;
  static AudioFrom output(const Wire& start);  // the start fields of `start` (a master mixer out: Resampling)
  static AudioFrom resampling() noexcept;      // (audioFromOf reads the master's mixer-out cable as this)
  friend bool operator==(const AudioFrom&, const AudioFrom&) = default;
};

// The wire onto `track`'s Audio In (null: none).
const Wire* audioInWireOf(const Project& p, TrackId track) noexcept;
// `track`'s Audio In source, read back from the wire (or Mixer::audioIn). None for a track that is not an audio track.
AudioFrom audioFromOf(const Project& p, TrackId track);

// What audio track `track` may record from — the menus' filter (RT.3: leave refused entries out) and the edits' check.
// Built once per menu (one pass over tracks and wires); each query is a hash lookup plus, for a device start, its
// declared port type. `lib` (optional) also refuses device outputs DECLARED control-rate / bool / event; without it
// only notes, value blocks and atom blocks' declared outputs are known (the compile cuts what is left: lower.h "Cut
// wires").
class AudioInSources {
public:
  AudioInSources(const Project& p, TrackId track, const DeviceLibrary* lib = nullptr);
  // Why `from` is refused ("" = allowed): kAudioInTrack, kAudioInNotAudio, kAudioInSelf, kAudioInLoop, or a missing
  // source. None and Ext are always allowed here (Ext's channels are SetTrackAudioInput's check).
  std::string_view refusal(const AudioFrom& from) const;
  bool allows(const AudioFrom& from) const { return refusal(from).empty(); }
  // Whether the Audio From list offers `source` at all: not the track itself, nor a track it feeds.
  bool offersTrack(TrackId source) const noexcept;
  bool offersResampling() const noexcept { return allows(AudioFrom::resampling()); }

private:
  const Project* p_;
  TrackId track_;
  const DeviceLibrary* lib_;
  bool audio_ = false;
  std::shared_ptr<const detail::AudioFeeds> feeds_;
  std::vector<uint32_t> fed_;  // fedBy(track, true), sorted
  bool resamplingLoops_ = false;
};
std::string audioFromRefusal(const Project& p, TrackId track, const AudioFrom& from, const DeviceLibrary* lib = nullptr);

namespace ops {
// ONE undo step ("Audio From"): `track`'s Audio In source becomes `from`, replacing the old one — its cable removed,
// Ext. In cleared — and (Output / Resampling) the new cable added (Add law, Edit::wires = it). Picking what is already
// the source is an empty edit. Refused with AudioInSources::refusal, or SetTrackAudioInput's channel check for Ext.
// ops::connect onto an Audio In (Wire::recordInput) is this with AudioFrom::output(ends): drawing a second cable
// replaces the first.
EditResult setAudioFrom(const Project& p, IdSource& ids, TrackId track, const AudioFrom& from,
                        const DeviceLibrary* lib = nullptr);
}  // namespace ops

}  // namespace pq::doc
