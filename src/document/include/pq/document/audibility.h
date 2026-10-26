#pragma once

// Mute and solo as one pure function of the project (T3.10; consumed by T3.17's lowering and by the views).
//
// It lives in `document` (not `views`) so the project → graph lowering (T3.17, also in `document`) and the mixer
// views read the same rule without the lowering depending on `views` (ARCHITECTURE §1: views → document, never the
// other way). It reads only Track::mixer (mute = Ableton's track activator, Q15; solo) and Track::active, so a mute
// or solo change is a Param edit (ChangeSet::paramOnly): the lowering turns each track's gate into a gain param
// write, never a recompile.
//
// What a gate means (the lowering's view of a track: its own output stage and its send taps):
//   output  the track's output stage passes signal to its destination (parent group, master, an explicit output).
//           Silence upstream is not this function's business: a muted group's gain is 0, so its members are
//           silent through it without their own gates closing (a muted bus silences what is routed into it).
//   sends   the track's sends feed the return tracks.
//
// Ableton semantics (Live 12, "Exclusive Solo" and "Solo in Place" on, its defaults):
//   - Mute (activator off) or deactivation closes both gates of the track.
//   - No solo anywhere: every other track is open.
//   - Solo is exclusive in the UI (a click unsolos the others; Ctrl-click adds): that is the views' business
//     (doc::ops::soloTrack); this function takes whatever solo flags are set.
//   - When any main-tree track (MIDI or group) is soloed, a main-tree track is open iff it is soloed, inside a
//     soloed group (group solo solos its members) or contains a soloed track (a member solo keeps its parent groups
//     audible: the members' signal passes through them). Other main-tree tracks close both gates (their sends too,
//     so their effects do not leak into the returns).
//   - Return tracks: when any return is soloed, only soloed returns are open; otherwise every return stays open even
//     while main tracks are soloed (Solo in Place: a soloed track's sends stay audible through the returns).
//   - Soloing only returns closes every main-tree track's output but leaves their sends open (the soloed return is
//     heard with what feeds it).
//   - The master is always open (it has no activator or solo).
//   - A soloed track that is muted stays silent (the activator wins).
//   - Solo follows the routing (T3.17-fix3, M3 review M1): every group on a solo-related track's output chain is
//     solo-related too, also one it is routed to explicitly (OutputKind::Track), so its signal reaches the master
//     (its own other members stay closed); an audible return routed into a group opens that group's chain likewise.
//   - The solo flag of a deactivated track, or of a track inside a deactivated group, is ignored: such a track is
//     not played at all (T3.17-fix3, M3 review L3); a member of a deactivated group is closed like the group.
//
// Cost: computeAudibility is one pass over the track tree (O(tracks), one allocation for the result); the lookup
// is a binary search. Not RT code: the audio thread sees only the gains T3.17 derives from it.

#include <cstdint>
#include <span>
#include <vector>

#include "pq/document/ids.h"
#include "pq/document/model.h"

namespace pq::doc {

struct TrackAudibility {
  TrackId id;
  bool output = true;  // the output stage passes signal
  bool sends = true;   // the sends feed the returns
  friend constexpr bool operator==(const TrackAudibility&, const TrackAudibility&) noexcept = default;
};

struct AudibilityOptions {
  // T3.17-fix3: compute the gates as if no track were muted (the lowering applies the mute as its own param, so a
  // mute lane can play; solo does not depend on other tracks' mutes, so gate × unmuted is the full rule).
  bool ignoreMute = false;
};

// Every track's gates (main tree depth-first, then returns, then the master: forEachTrack order), plus a sorted index
// for lookups by id.
class AudibilityMap {
public:
  std::span<const TrackAudibility> tracks() const noexcept { return tracks_; }
  // nullptr for an unknown id.
  const TrackAudibility* find(TrackId id) const noexcept;
  bool anySolo() const noexcept { return anyMainSolo_ || anyReturnSolo_; }
  bool anyMainSolo() const noexcept { return anyMainSolo_; }
  bool anyReturnSolo() const noexcept { return anyReturnSolo_; }

private:
  friend AudibilityMap computeAudibility(const Project& p, const AudibilityOptions& options);
  std::vector<TrackAudibility> tracks_;
  std::vector<uint32_t> byId_;  // indices into tracks_, sorted by id
  bool anyMainSolo_ = false;
  bool anyReturnSolo_ = false;
};

AudibilityMap computeAudibility(const Project& p, const AudibilityOptions& options = {});

// The output gate of one track (false for an unknown id). One pass over the project: for many tracks, compute the
// map once.
bool effectiveAudible(const Project& p, TrackId track);

}  // namespace pq::doc
