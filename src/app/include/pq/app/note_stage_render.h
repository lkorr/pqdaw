#pragma once

// AP.7 (docs/plans/AP.md "Roll preview + Flatten"; pq/document/note_flatten.h): the ONE offline path from a MIDI
// track's clips through its note devices (the Arpeggiator) to the notes they generate — what the roll preview draws
// and Flatten to clip writes. It plays what playback plays, by running playback's own code off the audio thread:
//   the notes     the track's played clips lowered by lowerClipDesc (ProjectLink::lowerTrack's lowering: identities,
//                 velocity, chance and velocity range under the link's chance seed, the M7 curves) into a SequenceImage
//   the stages    the track's note chain (doc::lowerNoteChains) built by makeArpStage (what the playback link
//                 publishes), its runtime params read from the devices' knob positions
//   the run       engine::renderNoteStages: a private Timeline from song tick 0 (as an offline render from the start),
//                 the same clock segments, block by block
// The generated note-ons become document notes: their start / end ticks (the sample's tick, on the grid where the
// grid tick is that sample), their EXACT identity (the stage's ArpExact, composed through a chain, applied to the
// source note's identity: note_flatten.h), their velocity, and their Pitch events (glides, carried bends) as a curve.
//   AP.8b        the devices' params follow their AUTOMATION as in playback (the lanes on their keys, written into the
//                param block at every block start); a CABLE into one is named (TrackNoteRender::cable) and left out —
//                its value comes from the audio graph while it plays (a free LFO's phase, an envelope, a signal), which
//                no offline render reproduces, so Flatten refuses while one exists.
// Not modelled: live input, an arrangement loop. A generated note belongs to the clip of the note that generated it,
// starting in that clip's first pass (a clip loop's later passes are not listed) and cut at the clip's end.
// Non-RT; any thread (the views' preview worker runs it on a document snapshot).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/note_stage_status.h"
#include "pq/core/result.h"
#include "pq/core/time.h"
#include "pq/document/lower.h"
#include "pq/document/note_devices.h"
#include "pq/document/note_flatten.h"
#include "pq/engine/arp.h"
#include "pq/engine/note_stage_offline.h"
#include "pq/views/note_preview.h"

namespace pq::app {

// The engine stage of lowered note device `st` (its fixed params and lane steps from st.values; each runtime slot
// mapped to the param slot `slotOf` gives for its key — kNoSlot: not mapped, the fixed value plays — and each cable
// into it to the bus value `busOf` gives — engine::kNoBusValue: none). What ProjectLink::publishNoteStages publishes.
// Null: not a type the engine plays.
inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
std::unique_ptr<engine::ArpStage> makeArpStage(const doc::LoweredNoteStage& st, const engine::ArpTables& tables,
                                               const std::function<uint32_t(const std::string& key)>& slotOf,
                                               const std::function<uint32_t(doc::WireId)>& busOf,
                                               std::shared_ptr<engine::ArpTableMailbox> box = {},
                                               std::shared_ptr<NoteStageStatus> status = {});
// AP.8a (review H4): the engine's degree tables of a lowered chain (its track table, one per tuning node).
engine::ArpTables arpTablesOf(const doc::LoweredNoteChain& c);

struct NoteStageRenderOptions {
  double sampleRate = 48000.0;
  uint64_t chanceSeed = 0;      // LinkConfig::chanceSeed
  uint32_t blockFrames = 512;
  // AP.8b: the engine's block (LinkConfig::blockFrames): automation is read once per block, so a render that follows
  // automation runs at the engine's block to give what it plays (without automation the block size changes nothing).
  uint32_t engineBlockFrames = 64;
  Ticks until = doc::kMaxTicks;      // render the arrangement up to this tick (and at most to the end of the track's clips)
  bool keepEvents = false;      // keep the engine events (TrackNoteRender::events: tests; the first pass's)
  // AP.8b: which clips. Played (the preview): the played lane's clips as they play. All (Flatten, review M6): the main
  // lane's clips as they play, then — in a second pass of their own — its deactivated clips as if they played (what a
  // muted clip would sound like unmuted: Flatten writes that, the clip stays muted).
  enum class Clips : uint8_t { Played, All };
  Clips clips = Clips::Played;
  const std::atomic<bool>* cancel = nullptr;  // AP.8b: set by another thread: the render stops (an error)
};

struct TrackNoteRender {
  bool any = false;                       // the track has an active note-device chain (else nothing below)
  std::vector<doc::ClipGenerated> clips;  // per played note clip of the track (its order): what it generates
  std::vector<doc::DeviceId> devices;     // the chain's devices, in signal order
  engine::NoteStageRender events;         // keepEvents: the run's input / output events (song samples)
  uint64_t inexact = 0;                   // generated notes without an exact identity (Cents)
  bool automated = false;                 // AP.8b: a device param is automated (the render follows it)
  std::string cable;                      // AP.8b: "<device> <param>" of a cable into a device param (left out); "": none
};

// Errors: no such MIDI track; the sequence or tempo map did not build.
Result<TrackNoteRender> renderTrackNoteStages(const doc::Project& p, doc::TrackId track,
                                              const NoteStageRenderOptions& options = {});

// AP.8b: renderTrackNoteStages as the views' note-device source (NotePreviews::setSource, and the view scenes':
// views::setSceneNotePreviewSource): a request's config (rate, chance seed), until, flatten (Clips::All) and cancel
// become the options; the cable into a param is reported (NotePreview::cable).
views::NotePreviewFn notePreviewSource();

}  // namespace pq::app
