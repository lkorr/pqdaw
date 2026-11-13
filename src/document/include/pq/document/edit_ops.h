#pragma once

// Edit operations library (T3.6): pure command builders, so views stay thin.
//
// Every builder reads a snapshot plus a selection and parameters and returns an `Edit`: one `Batch` (= exactly one
// history entry when dispatched) and the selection the view should show afterwards. Builders never mutate anything
// except the IdSource they are given (ids for new entities are drawn from it, as T3.4 requires: pass
// `store.ids()`). An Edit whose batch is empty changes nothing (dispatching it records no history entry).
//
//     auto e = ops::splitClips(*store.current(), store.ids(), selection, playhead);
//     if (e && !e->empty()) store.dispatch(e->command);   // one undo step; select e->clips
//
// Errors (Result): unknown ids, a selection the operation does not accept (e.g. consolidating clips with different
// tunings), parameters out of range. Nothing is half-built: an error returns no command.
//
// Cost: each builder is proportional to what it touches (the clips/notes/points it edits, plus a lookup per
// selected id), never to the project size, except the time edits (insert/delete/duplicate time), which by definition
// visit every track, lane and locator.
//
// Ableton semantics followed throughout (SPEC §6.1, §9.4, §9.6):
// - Clips on a track do not overlap: a clip placed, moved, pasted or grown over others overwrites them (covered
//   clips are removed, partly covered ones are trimmed or split).
// - Resizing a clip edge keeps every note at its absolute time (the start marker moves with the left edge).
// - Time edits (insert / delete / duplicate time) act on all tracks; clips across a cut are split.
//
// Not RT code (UI thread).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pq/core/grid.h"
#include "pq/core/result.h"
#include "pq/document/commands.h"
#include "pq/document/model.h"
#include "pq/document/modules.h"
#include "pq/document/racks.h"
#include "pq/document/store.h"
#include "pq/meta/type.h"
#include "pq/tuning/tuning.h"

namespace pq::doc {
class DeviceLibrary;  // devices.h (TX.groups: the group builders read port names)
}

namespace pq::doc::ops {

struct Edit {
  Command command;  // always a Batch labelled with the operation
  // The selection after the edit (created or surviving entities), for the view to adopt.
  std::vector<TrackId> tracks;
  std::vector<ClipRef> clips;
  ClipRef noteClip;               // the clip `notes` belong to
  std::vector<NoteKey> notes;     // keys as they are in the resulting snapshot
  std::vector<BreakpointId> points;
  std::vector<TuningId> tunings;  // main tunings created or kept (T3.19)
  std::vector<DeviceId> devices;  // devices created or kept (T4.1)
  std::vector<WireId> wires;      // wires created or kept (T4.7)

  bool empty() const noexcept;    // the batch has no commands
  const Batch& batch() const noexcept;
};
using EditResult = Result<Edit>;

// ---- Tracks ----------------------------------------------------------------------------------------------------

// Ctrl+Shift+T (Midi) / Ctrl+T (Audio, T5.3) / Ctrl+Alt+G-style group / return. Inserted right after `after` among its
// siblings (invalid `after`: at the end of the top level; Return tracks always go to the returns list, after `after` if
// it is a return). An empty name gets Live-style defaults: "<n> MIDI", "<n> Audio", "<n> Group", "<letter> Return". A
// MIDI track starts with the default instrument, an audio track with an empty chain (audio effects only).
EditResult createTrack(const Project& p, IdSource& ids, TrackKind kind, TrackId after = {}, std::string name = {},
                       Colour colour = {});
// Deletes the tracks (a selected group takes its members with it). The master is refused.
EditResult deleteTracks(const Project& p, std::span<const TrackId> tracks);
EditResult renameTrack(const Project& p, TrackId track, std::string name);
EditResult setTracksColour(const Project& p, std::span<const TrackId> tracks, Colour colour);
// Moves the tracks (kept in their tree order) under `parent` (invalid: top level) right before sibling `before`
// (invalid: at the end). Tracks inside other selected groups move with their group.
EditResult moveTracks(const Project& p, std::span<const TrackId> tracks, TrackId parent, TrackId before = {});
// Ctrl+G: wraps the selection (any parents, nesting allowed) in a new group placed where the first selected track
// was. Tracks inside selected groups stay inside them.
EditResult groupTracks(const Project& p, IdSource& ids, std::span<const TrackId> tracks, std::string name = {});
// Replaces each selected group by its members.
EditResult ungroupTracks(const Project& p, std::span<const TrackId> groups);
EditResult setTracksActive(const Project& p, std::span<const TrackId> tracks, bool active);
// `0`: deactivates the selection if any selected track is active, else activates it all.
EditResult toggleTracksActive(const Project& p, std::span<const TrackId> tracks);

// ---- Mixer (T3.10) -----------------------------------------------------------------------------------------------
// Mute and solo are Param edits (Q15: the track activator is the mute, no recompile); T6.1: so is arm (the monitor gate).

// The track activator (Live's numbered button) = mute. The master has none (refused).
EditResult setTracksMute(const Project& p, std::span<const TrackId> tracks, bool mute);
// `0` with tracks selected, or a click on an activator: if any of the tracks is on (not muted) all are muted, else
// all are switched back on.
EditResult toggleTracksMute(const Project& p, std::span<const TrackId> tracks);
// A click on Solo (Live's Exclusive Solo, its default): on an unsoloed track it solos the track and unsolos every
// other track (main tree and returns); on a soloed track it unsolos just that track. `additive` (Ctrl-click) toggles
// only this track. The master is refused. What solo does to the sound is pq/document/audibility.h.
EditResult soloTrack(const Project& p, TrackId track, bool additive);
// The arm button, same exclusive / additive rule over the MIDI and audio tracks (Live's Exclusive Arm).
EditResult armTrack(const Project& p, TrackId track, bool additive);
// A return's Pre/Post switch (Live): every other track's send to `ret` taps before or after its fader (Structural:
// the tap point moves). Tracks without a send (the master) are skipped.
EditResult setReturnSendsPreFader(const Project& p, TrackId ret, bool preFader);
// True when every track that can send to `ret` sends pre-fader (what the switch shows). False for no senders.
bool returnSendsPreFader(const Project& p, TrackId ret) noexcept;

// Fold is view state (Q15/Q26): not undoable, so it edits the store's ViewState directly and returns nothing.
void setFolded(ViewState& view, TrackId track, bool folded);
bool toggleFolded(ViewState& view, TrackId track);  // returns the new state

// ---- Clips -----------------------------------------------------------------------------------------------------

// Ctrl+Shift+M: an empty looped clip over [start, end) on a MIDI track (overwriting what was there).
EditResult insertEmptyClip(const Project& p, IdSource& ids, TrackId track, Ticks start, Ticks end,
                           std::string name = {});
// T6.9 Capture MIDI: ONE undo step ("Capture MIDI"). Per entry a note clip [start, start + length) on its MIDI track
// (overwriting what lies under it, as placeClip), its `notes` in content ticks (fresh ids), looped over its whole
// length when `loop`. `bpm` (optional): the song tempo becomes it first (the first tempo point, as the control bar's
// tempo box) — only for a project without any clip (main or take lanes), else InvalidArgument (the app infers it from
// what was played while stopped). Errors: unknown / non-MIDI tracks, invalid notes, an empty list.
struct CapturedClip {
  TrackId track;
  Ticks start = 0, length = kTicksPerWhole;
  bool loop = false;
  std::vector<Note> notes;
};
EditResult captureClip(const Project& p, IdSource& ids, std::span<const CapturedClip> clips,
                       std::optional<double> bpm = std::nullopt);
// True when any track has a clip (main lane or a take lane).
bool projectHasClips(const Project& p) noexcept;
EditResult deleteClips(const Project& p, std::span<const ClipRef> clips);

struct ClipMove {
  ClipRef clip;
  TrackId toTrack;  // invalid = same track
  Ticks start = 0;  // new arrangement start
};
struct MoveClipsOptions {
  bool copy = false;  // Alt+drag: copies with fresh clip and note ids; the originals stay
  // Automation follows the clips (SPEC §9.6, Ableton's "lock envelopes" off): for clips that stay on their track,
  // every lane on that track moves its envelope over the clip's range with the clip (exactly, curves included);
  // the vacated range holds the value it had on its left; the destination range is overwritten.
  bool moveAutomation = false;
};
// Moves (or copies) clips, each to its own destination; destinations overwrite other clips.
EditResult moveClips(const Project& p, IdSource& ids, std::span<const ClipMove> moves, MoveClipsOptions opt = {});
// Convenience: every clip by the same time delta, staying on its track.
EditResult moveClipsBy(const Project& p, IdSource& ids, std::span<const ClipRef> clips, Ticks delta,
                       MoveClipsOptions opt = {});

enum class ClipEdge : uint8_t { Start, End };
// Moves one edge of each clip by `delta` (clamped so a clip keeps ≥ 1 tick). Notes keep their absolute time; a grown
// edge overwrites neighbouring clips.
EditResult resizeClips(const Project& p, IdSource& ids, std::span<const ClipRef> clips, ClipEdge edge, Ticks delta);
// Ctrl+E: splits every given clip at every cut point strictly inside it (a time selection passes its two edges).
// Absolute time and pitch identity of every note are kept: a non-looped clip's notes go to the piece they start in
// (same ids); a looped clip's later pieces get copies of its loop content (fresh ids), each piece playing exactly
// what the original played there.
EditResult splitClips(const Project& p, IdSource& ids, std::span<const ClipRef> clips, std::span<const Ticks> at);
// Ctrl+D: copies the clips right after the selection (per the selection's overall span [min start, max end)).
EditResult duplicateClips(const Project& p, IdSource& ids, std::span<const ClipRef> clips);
// Ctrl+J: per track, merges the selected clips (and any clip between them) into one clip spanning them. Every note
// that played keeps its absolute time and pitch identity; looped content is unrolled; notes are cut where playback
// cut them (clip end, loop wrap); notes of deactivated clips come in deactivated. The first occurrence of a note
// keeps its id, repeats get fresh ids. Refused if the clips' tuning assignments differ.
EditResult consolidateClips(const Project& p, IdSource& ids, std::span<const ClipRef> clips);
// Ctrl+L in the clip view: loop brace = content [start, end), loop on.
EditResult loopClipToSelection(const Project& p, ClipRef clip, Ticks contentStart, Ticks contentEnd);
// Ctrl+L in the arrangement: arrangement loop = [start, end), on.
EditResult loopArrangementToSelection(const Project& p, Ticks start, Ticks end);

// ---- Audio clips (T5.3; model.h "Samples and audio clips") ----------------------------------------------------------
// The generic clip edits above (delete, move / Alt-copy, resize, split, duplicate, time edits) take audio clips too:
// every source frame keeps the absolute time it plays at (audio_clip.h), clips move only between tracks of their kind.
// Each builder below is one Batch = one undo step; a drag is several inside one store gesture. None compiles anything
// (ChangeSet Sequence: runtime data of the clip player).

// A clip to place: `sample` names an entry of the table or of `newSamples`; the clip plays the whole file, warp Off,
// from `start` (an empty name: the sample's display name; the colour: the track's).
struct NewAudioClip {
  TrackId track;
  Ticks start = 0;
  SampleId sample;
  std::string name;
};
// Adds `newSamples` to the table (their ids drawn by the caller from the store's IdSource) and the clips (fresh ids),
// each overwriting what lies under it on its audio track (Live's drop). Several files dropped at once are one step.
EditResult addAudioClips(const Project& p, IdSource& ids, std::span<const SampleRef> newSamples,
                         std::span<const NewAudioClip> clips);
// Clip gain in dB (clamped to [kMinClipGainDb, kMaxClipGainDb]; the floor is silence).
EditResult setAudioClipGain(const Project& p, std::span<const ClipRef> clips, float gainDb);
// Transpose (Cents clamped to ±kMaxTransposeCents; Steps to ±kMaxTransposeSteps; a Ratio must be positive and within
// ±kMaxTransposeCents: InvalidArgument otherwise) and detune (clamped to ±kMaxDetuneCents). Warp Off only plays them.
EditResult setAudioClipTranspose(const Project& p, std::span<const ClipRef> clips, AudioTranspose transpose);
EditResult setAudioClipDetune(const Project& p, std::span<const ClipRef> clips, float cents);
EditResult setAudioClipsReverse(const Project& p, std::span<const ClipRef> clips, bool reverse);
// Warp Off / Re-Pitch (sourceBpm: the source tempo for Re-Pitch, clamped to [kMinBpm, kMaxBpm]; also how a Re-Pitch
// clip's source BPM is changed): the audio at each clip's start stays (audio_clip.h withWarp).
EditResult setAudioClipWarp(const Project& p, std::span<const ClipRef> clips, AudioWarp warp, double sourceBpm = 120.0);
// Fade lengths clamped to [0, clip length] (the fade-in first), curves to [−1, 1].
EditResult setAudioClipFades(const Project& p, ClipRef clip, AudioFade fadeIn, AudioFade fadeOut);
// The crossfade with the clip ending exactly at this clip's start (0 removes it; clamped to both clips' lengths).
// Refused (InvalidArgument) when there is no such adjacent clip.
EditResult setAudioClipCrossfade(const Project& p, ClipRef clip, Ticks length);
// A warped clip's loop brace in content ticks (warp Off clips have none: InvalidArgument).
EditResult setAudioClipLoop(const Project& p, ClipRef clip, ClipLoop loop);
// T6.1: the warp mode (model.h AudioWarp), each clip keeping its source BPM, its song-time placement and the audio at
// its start (audio_clip.h withWarp: Off ↔ warped converts the start marker through the markers; between warped modes
// only the mode changes). One step for all the clips.
EditResult setWarpMode(const Project& p, std::span<const ClipRef> clips, AudioWarp warp);
// T6.1: the time-stretch modes' settings (InvalidArgument out of range: model.h WarpParams).
EditResult setAudioClipWarpParams(const Project& p, std::span<const ClipRef> clips, WarpParams params);
// T6.1: warp markers of a warped clip (an Off clip: InvalidArgument). Adding pins content tick `content` to the source
// frame it plays now (nothing sounds different; a marker already there: no change). Moving marker `index` to content
// tick `content` keeps its source frame (the audio between its neighbours stretches) and clamps it strictly between
// them. Removing drops it. Every other marker keeps its content tick, so its song time stays exactly.
EditResult addWarpMarker(const Project& p, ClipRef clip, Ticks content);
EditResult moveWarpMarker(const Project& p, ClipRef clip, size_t index, Ticks content);
EditResult removeWarpMarker(const Project& p, ClipRef clip, size_t index);
// T6.8 (the audio clip view): Ctrl-drag of a marker — marker `index` pinned to source frame `sourceFrame` with its
// content tick kept (the audio under it slides), clamped strictly between its neighbours' frames and to >= 0.
EditResult slideWarpMarker(const Project& p, ClipRef clip, size_t index, double sourceFrame);
// T6.8: Live's "Warp From Here": the markers before content tick `content` stay, one sits at `content` (where the
// source plays now), the later ones go, and from there on the source is read at `bpm` (a second marker one beat later;
// with no earlier marker the clip's source BPM becomes `bpm`, so the whole clip follows it). An Off clip:
// InvalidArgument. `bpm` in [kMinBpm, kMaxBpm].
EditResult warpFromHere(const Project& p, ClipRef clip, Ticks content, double bpm);
// T6.8: "Warp as N-Bar Loop": the source the clip plays now (its loop brace when looping, else from its start for its
// length; an Off clip: its window) becomes exactly `bars` bars of 4/4 — the markers replaced by one at the region's
// start, the source BPM set so the region spans the bars, the loop on over it, the clip `bars` bars long (kept short of
// the next clip on the track: these commands never overwrite). An Off clip
// switches to `offMode` (a warped mode) first. InvalidArgument when the tempo would leave [kMinBpm, kMaxBpm].
// `bars` (any whole number, 1..1024) of the time signature at the clip's start (M6 review M11).
EditResult warpAsLoop(const Project& p, ClipRef clip, int32_t bars, AudioWarp offMode = AudioWarp::Beats);
// T6.8: auto-warp (a long sample dropped with the "Auto-Warp long samples" preference on): each warp-Off clip switched
// to `warp` at source tempo `bpm`, its length re-derived so it still plays to the file's end (short of the next clip).
// Warped clips are left alone. One step.
EditResult autoWarpClips(const Project& p, std::span<const ClipRef> clips, AudioWarp warp, double bpm);

// ---- Take lanes and comping (T6.1; model.h "Take lanes") ------------------------------------------------------------
// One undo step each. Take lanes live on MIDI and audio tracks.

// A take lane after the track's others holding copies of `clips` (fresh clip and note ids; of the track's kind),
// named `name` ("" → "Take <n>"). A recording pass ends with one (T6.7). Edit::clips = the lane's clips.
EditResult addTakeLane(const Project& p, IdSource& ids, TrackId track, std::span<const ClipSlot> clips,
                       std::string name = {});
EditResult removeTakeLane(const Project& p, TrackId track, LaneId lane);
EditResult renameTakeLane(const Project& p, TrackId track, LaneId lane, std::string name);
// Plays take `lane` instead of the main lane (invalid: the main lane again). A Sequence change: nothing compiles.
EditResult setAuditionTake(const Project& p, TrackId track, LaneId lane);
// The take's clips become the main lane, the main lane's clips the take (PromoteTake).
EditResult promoteTake(const Project& p, TrackId track, LaneId lane);
// Comping: copies what take `lane` plays in [t0, t1) into the main lane as ordinary clips (fresh ids, ClipSlot::take =
// `lane`), cutting what the main lane had there (trimmed / split as any overwrite). Every note and every source frame
// keeps its absolute time; the take is unchanged (non-destructive). On an audio track each region edge where two clips
// now meet gets a crossfade of `crossfade` ticks (centred, clamped to both clips; 0: none). Edit::clips = the pieces.
inline constexpr Ticks kCompCrossfade = kPpq / 32;  // ≈ 16 ms at 120 BPM
EditResult compRegion(const Project& p, IdSource& ids, TrackId track, LaneId lane, Ticks t0, Ticks t1,
                      Ticks crossfade = kCompCrossfade);

// T6.7: what one recording session made, committed as ONE undo step ("Record"). `samples` are added to the table (ids
// drawn by the caller). Per track, `passes` = one clip per record pass (loop recording: one per loop pass), in order,
// of the track's kind (their clip / note ids are ignored: fresh ones; audio clips warp Off, synced here). Each pass
// becomes a take lane ("Take <n>", after the track's others) and the LAST pass is also copied into the main lane
// (overwriting what lies under it; ClipSlot::take = its lane). `overdub` (MIDI tracks): no take lanes — every note goes
// into the main-lane clip under its start (content ticks), and notes under no clip into a new clip per gap between the
// main lane's clips (spanning the recorded passes' range within the gap, and those notes). AR.1: `automation` (below) is
// written in the same step. Errors: unknown tracks, clips of the wrong kind, a parameter that cannot have a lane.
struct RecordedTrack {
  TrackId track;
  std::vector<ClipSlot> passes;
  bool overdub = false;
};
// AR.1 (docs/plans/TU.md "AR.1"): automation written while recording (Automation Arm on), committed in the same "Record"
// step. One touch of a parameter: its values at song ticks in the order they were set (`samples`: tick, value; ids
// ignored; ticks never decrease; on one tick the last value counts). A touch held by a gesture (a knob drag) ends at
// `end` (its release, >= the last sample): it overwrites the lane over [first sample, end], the existing envelope
// plays again from `end`. A `latch` touch (one edit outside a gesture: a click, a typed value) holds its value up to the
// lane's next point after it (none: for good). The samples are thinned first: the fewest that keep the touch's
// polyline within `tolerance` (the parameter's unit) of every sample.
struct RecordedTouch {
  std::vector<Breakpoint> samples;
  Ticks end = 0;
  bool latch = false;
};
// One parameter's touches, in the order they happened (a later touch overwrites an earlier one where they overlap, as
// loop passes do). The lane is the parameter's existing one (wherever it is), else a new lane on its home: a track
// param's own track, a device / wire param's laneHome, a tuning param's `home` (any track); a new lane holds `before`
// (the value before the first touch) up to the first touch. `paramName`: a DeviceParam's name (AutomationLane::paramName).
struct RecordedAutomation {
  ParamRef param;
  std::string paramName;
  TrackId home;
  double before = 0.0;
  double tolerance = 0.0;
  std::vector<RecordedTouch> touches;
};
// The fewest of `samples` (ticks increasing) whose polyline stays within `tolerance` of every sample (Ramer–Douglas–
// Peucker on the value axis; the first and last always kept). tolerance <= 0 keeps every value change.
std::vector<Breakpoint> thinAutomation(std::span<const Breakpoint> samples, double tolerance);
struct RecordingCommit {
  std::vector<SampleRef> samples;
  std::vector<RecordedTrack> tracks;
  std::vector<RecordedAutomation> automation;  // AR.1
};
EditResult commitRecording(const Project& p, IdSource& ids, const RecordingCommit& commit);

// ---- Render edits (FZ.4; docs/plans/RB.md D5, "### FZ.4") ---------------------------------------------------------
// The app renders first — a frozen track's render, a track's own contribution, an audio track's clip player — into a
// WAV file, then builds one of these from it: one Batch, one undo step. `sample` is the file's table entry (id drawn by
// the caller from the store's IdSource; added unless the table holds that id; its rate is the render's). A REGION
// [start, end) becomes a warp-Off clip at `start` (gain 0 dB, no fades; name: the sample's display name; colour: the
// track's) playing file frame samples(t) − samples(origin) at tick t — the tempo map's tick → sample at the sample's
// rate — so the clips together play the render exactly where it was rendered: song sample k plays file frame
// k − samples(origin). Regions are sorted, disjoint, end > start ≥ origin. Errors: unknown tracks, a track of another
// kind, bad regions, a bad sample entry.
struct RenderRegion {
  Ticks start = 0, end = 0;
};
// Flatten (D5): a frozen (or not) MIDI or audio track becomes an AUDIO track in place — id, name, colour, mixer, sends,
// routing, mixer lanes and tuning lanes stay — playing `regions` of the render (origin: tick 0). Its blocks (every one:
// a block whose output reached another track goes too, with its cables), its clips, take lanes, device lanes and the
// cables from its source go; it is unfrozen; a fresh main link source → mixer is made; cables from other tracks into
// its mixer stay. Selection: the track and its new clips.
EditResult flattenTrack(const Project& p, IdSource& ids, TrackId track, const SampleRef& sample,
                        std::span<const RenderRegion> regions);
// Bounce to New Track: a new audio track right after `track` among its siblings ("<name> Bounce", its colour, its
// mixer — volume, pan, pan law, mute, sends, output, Track Delay; not solo, arm, monitoring or inputs) playing
// `regions` of the render (file frame 0 = tick `origin`); `track` is deactivated in the same step. Selection: the new
// track and its clips. Errors: `track` is not a MIDI or audio track.
EditResult bounceToNewTrack(const Project& p, IdSource& ids, TrackId track, const SampleRef& sample, Ticks origin,
                            std::span<const RenderRegion> regions);
// Consolidate on audio tracks: per track, what lies in [start, end) is replaced by one clip of its render (file frame 0
// = tick `start`; clips across the edges are trimmed there, as any clip placed over them). Selection: the new clips.
struct ConsolidatedAudio {
  TrackId track;
  SampleRef sample;
};
EditResult consolidateAudio(const Project& p, IdSource& ids, std::span<const ConsolidatedAudio> tracks, Ticks start,
                            Ticks end);

// ---- Notes -----------------------------------------------------------------------------------------------------

// Adds notes (their `id` fields are ignored: fresh ids are assigned). Returns their keys.
EditResult addNotes(const Project& p, IdSource& ids, ClipRef clip, std::span<const Note> notes);
EditResult deleteNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes);

// Identity-aware pitch steps (SPEC §6.1 microtonal Up/Down, T3.6).
enum class StepUnit : uint8_t {
  Grid,    // Up/Down: one display-grid step
  Equave,  // Shift+Up/Down: one equave
};
struct PitchStepContext {
  const tuning::Tuning* tuning = nullptr;     // the clip's effective tuning (required for Degree/Cents/Ratio-equave)
  const tuning::Edo* displayGrid = nullptr;   // optional EDO overlay (SPEC §3.2): Cents notes step on its lines
  // Reference-anchored Ratio notes: the clip's virtual fundamental (doc::virtualFundamental, T3.6-fix1); nullopt =
  // each note's own fundamental 1/d. Ratios anchored to other notes always step on their own.
  std::optional<tuning::Rational> fundamental;
};
// One note's pitch after `steps` steps (negative = down). nullopt = the note cannot move that way (left unchanged):
//   Degree   Grid: ±1 degree of the tuning (one EDO step / one scale or MOS degree), carrying into the equave.
//            Equave: equave ± 1.
//   Cents    Grid: to the next display-grid line above/below (a line within 1e-6 ¢ counts as "on" it). The lines
//            are every degree of the tuning in every equave (degree d of equave e at centsOf(d, e), stretch
//            included), or the displayGrid EDO's steps if one is given. Equave: ± the tuning's equave in cents.
//   Ratio    Grid: next harmonic of the fundamental F' = gcd(n/d, F) (the largest fundamental the note and F are both
//            harmonics of; F = 1/D gives 1/lcm(d, D); without F: 1/d): n/d = m·F' → (m ± 1)·F', so on F = 1/4
//            5/4 → 6/4 (= 3/2) → 7/4. Fails below the first harmonic (m ± steps < 1).
//            Equave: × / ÷ the tuning's equave ratio (exact for EDO ratio equaves and Scala ratio equaves; a cents
//            equave within 0.001 ¢ of a ratio p/q with p, q ≤ 64 uses that ratio; otherwise fails).
//   Harmonic Grid: next voice of the JI object (voiceIndex ± 1, ≥ 0). Equave: fails (a voice has no equave).
std::optional<tuning::PitchId> stepPitch(const tuning::PitchId& pitch, int32_t steps, StepUnit unit,
                                         const PitchStepContext& ctx);

// Moves notes in time by `dt` and in pitch by `steps` display-grid steps (stepPitch). A note whose pitch cannot
// step keeps its pitch (it still moves in time).
// Both step reference-anchored Ratio notes on the clip's virtual fundamental (doc::virtualFundamental) and, unless it
// is locked, record in the same Batch (SetClipFundamental) the fundamental each stepped note was on when that is
// finer than its new reduced denominator (a HarmonicStep), so 5/4 → 3/2 → 7/4 keeps 1/4 (T3.6-fix1, Q40).
EditResult moveNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, Ticks dt, int32_t steps = 0,
                     const tuning::Edo* displayGrid = nullptr);
// Up/Down (unit Grid) and Shift+Up/Down (unit Equave), using the clip's effective tuning.
EditResult transposeNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, int32_t steps,
                          StepUnit unit, const tuning::Edo* displayGrid = nullptr);
// The explicit fundamental lock (b + index, SPEC §6.2): "this note is the index-th harmonic", so the clip's virtual
// fundamental becomes the note's ratio / index until unlocked. The note must be a reference-anchored Ratio note
// (other identities have no exact ratio to divide; InvalidArgument), index ≥ 1. One undo step each.
EditResult lockFundamental(const Project& p, ClipRef clip, NoteKey note, uint32_t harmonicIndex);
EditResult unlockFundamental(const Project& p, ClipRef clip);

enum class NoteEdge : uint8_t { Start, End };
// Moves one edge of each note by `delta`; the other edge stays (length ≥ 1 tick).
EditResult resizeNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, NoteEdge edge, Ticks delta);
// Sets a property on every note (value clamped to the property's range: velocity [1, 127], release [0, 127],
// chance [0, 1], velocity range [−127, 127]; Enabled: ≠ 0 enables).
EditResult setNotesProperty(const Project& p, ClipRef clip, std::span<const NoteKey> notes, NoteProperty prop,
                            float value);
// Adds `delta` to the property of every note (clamped per note), e.g. a velocity drag over a selection.
EditResult offsetNotesProperty(const Project& p, ClipRef clip, std::span<const NoteKey> notes, NoteProperty prop,
                               float delta);
// Ctrl+U: quantizes note starts (and ends in StartEnd mode) to `grid` (pq::quantize). The grid is evaluated at
// content tick + gridOrigin (0 = the clip's own bar grid; clip.start − clip.contentStart = the arrangement grid).
// An empty `notes` span quantizes every note of the clip.
EditResult quantizeNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, const Grid& grid,
                         Ticks gridOrigin = 0, int amountPercent = 100, QuantizeMode mode = QuantizeMode::StartOnly);
// Ctrl+D in the clip view: copies the notes `offset` ticks later (0 = the selection's span, max end − min start).
EditResult duplicateNotes(const Project& p, IdSource& ids, ClipRef clip, std::span<const NoteKey> notes,
                          Ticks offset = 0);

// SPEC §3.1 explicit identity conversions. Degree and Ratio (reference-anchored) → Cents: the exact same pitch
// ("freeze"). Ratio/Cents → Degree: the nearest degree of the clip's tuning ("snap"). Degree/Cents → Ratio (T3.19):
// the simplest ratio (smallest denominator, the Stern–Brocot node nearest the root) within ratioSnapTolerance of the
// pitch, reference-anchored, so Degree → Ratio → Degree returns the same degree. Other notes are left alone; `to` =
// Harmonic is refused (no JI objects yet).
EditResult convertNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, tuning::PitchKind to);
// Sets every note's pitch identity to `pitch` (a type-in, T3.19). A Ratio must be positive and anchored to the
// reference or a note; Harmonic is refused.
EditResult setNotesPitch(const Project& p, ClipRef clip, std::span<const NoteKey> notes, const tuning::PitchId& pitch);

// The simplest positive ratio in [lo, hi] (smallest denominator, then numerator: the first Stern–Brocot node inside
// the interval), or nullopt (lo ≤ 0, hi < lo, or terms beyond 2^40). Exact integer arithmetic on the continued-fraction
// terms of the interval's ends; at most 64 terms. 2^(385/1200) … 2^(415/1200) → 5/4.
std::optional<tuning::Rational> simplestRatioBetween(double lo, double hi) noexcept;
// The simplest ratio within ±toleranceCents of `cents` (above the reference).
std::optional<tuning::Rational> simplestRatioNear(double cents, double toleranceCents) noexcept;
// How far a ratio snapped from a pitch may lie from it (T3.19): 0.45 × the smaller step to the neighbouring degrees of
// the tuning (so it stays nearer its degree than any other), clamped to [kRatioSnapMinCents, kRatioSnapMaxCents].
inline constexpr double kRatioSnapMinCents = 0.05;
inline constexpr double kRatioSnapMaxCents = 20.0;  // 12-EDO's 900 ¢ → 5/3 (15.6 ¢ off), 300 ¢ → 6/5
double ratioSnapTolerance(const tuning::Tuning& t, double cents) noexcept;

// ---- Main tunings (T3.19, SPEC §3.2) ---------------------------------------------------------------------------

// A new main tuning at the end of the list (one AddTuning; Structural: a new tuning signal). Edit::tunings = its id.
EditResult addMainTuning(const Project& p, IdSource& ids, std::string name, TuningDef def, Colour colour);
// Removes a main tuning in one step: every track and clip assigned to it (TuningMode::Main) is reassigned to
// `replacement` (invalid: the first other main tuning) first, and so is the project default when it was this one
// (SetDefaultTuning), then RemoveTuning (which drops its automation lanes).
// Refused for the last main tuning. Edit::tunings = {replacement}, Edit::tracks = the reassigned tracks (and clips'
// tracks).
EditResult removeMainTuning(const Project& p, TuningId tuning, TuningId replacement = {});

// ---- Time selection (all tracks) ------------------------------------------------------------------------------
// T6.1: take lanes are edited exactly as their track's main lane (a changed lane is replaced whole in the same step),
// and the punch region moves as the loop does.

// Ctrl+I: inserts `length` ticks of silence at `at`. Clips across `at` are split; clips, automation (exactly: the
// envelope holds its value through the gap), locators, the loop, tempo points and time-signature changes after `at`
// move later by `length`.
EditResult insertTime(const Project& p, IdSource& ids, Ticks at, Ticks length);
// Deletes [start, end) and closes the gap: clips inside are removed, clips across the edges trimmed/split, later
// material moves earlier; locators inside are removed; automation keeps its values on both sides (a jump at the
// seam where they differ).
EditResult deleteTime(const Project& p, IdSource& ids, Ticks start, Ticks end);
// Ctrl+Shift+D: inserts a copy of [start, end) right after it (clips trimmed to the range, automation exact).
EditResult duplicateTime(const Project& p, IdSource& ids, Ticks start, Ticks end);

// ---- Automation ------------------------------------------------------------------------------------------------

// Adds one breakpoint (value clamped to the parameter's range, curve to [−1, 1]).
EditResult addBreakpoint(const Project& p, IdSource& ids, TrackId track, LaneId lane, Ticks tick, double value,
                         float curve = 0.0f);
// Moves breakpoints by (dt, dv) (values clamped per point).
EditResult moveBreakpoints(const Project& p, TrackId track, LaneId lane, std::span<const BreakpointId> points,
                           Ticks dt, double dv);
EditResult deleteBreakpoints(const Project& p, TrackId track, LaneId lane, std::span<const BreakpointId> points);
// Alt-drag: curvature of the segment that starts at `point` (clamped to [−1, 1]).
EditResult setSegmentCurve(const Project& p, TrackId track, LaneId lane, BreakpointId point, float curve);

enum class ShapeKind : uint8_t { RampUp, RampDown, Sine, Triangle, Square, Saw };
struct ShapeParams {
  ShapeKind kind = ShapeKind::Sine;
  double low = 0.0;   // parameter units (clamped to the parameter's range)
  double high = 1.0;
  int32_t cycles = 1;  // periods over the range (ramps ignore it), ≥ 1
};
// Replaces the envelope over [start, end) with a shape; the envelope outside the range is unchanged (exact).
// Sine uses 16 linear segments per cycle; Square and Saw use jumps.
EditResult insertShape(const Project& p, IdSource& ids, TrackId track, LaneId lane, Ticks start, Ticks end,
                       const ShapeParams& shape);

// ---- The main path (TX.all-free, SPEC §15 2026-11-03 "everything is free; the chain is just cables"; main_path.h) ----
// A track's blocks are a set and its audio path is its wires. These builders rewire it; each is one Batch = one undo
// step. A SLOT is a place on the main path: 0 right after the source, n after its n-th block (kAppend: last, before the
// mixer). kOffPath: a block added without wiring it (placed freely: the node menu, the free rack).
inline constexpr uint32_t kOffPath = 0xFFFFFFFEu;
// Wires every track that has no main link at all (none from its source's main port, none into its mixer) in series:
// its source → its blocks in list order (group members apart) → its mixer — exactly what the schema 16 → 17 migration
// does, in its order (tracks depth-first, returns, master). For builders and tests that make tracks from blocks.
EditResult wireSeries(const Project& p, IdSource& ids);
// Inserts block `device` (of a track, off its path) into its track's main path at `slot`: the link into that slot (the
// source's or a block's main output → the next block or the mixer) becomes link.from → device → link.to.
EditResult insertOnPath(const Project& p, IdSource& ids, DeviceId device, uint32_t slot);
// The Device view's reorder (and drag to another track's list): the device leaves its path where it is (its main
// sources joined to its main targets: "dissolved there", the path stays connected) and is spliced into `track`'s path
// (invalid: its own) at `slot` (counted with it in its old place, as the list shows it). `lib` names the main ports of
// wires drawn by name (null: kMainPort ends only).
EditResult moveOnPath(const Project& p, IdSource& ids, const DeviceLibrary* lib, DeviceId device, TrackId track, uint32_t slot);
// Dissolve (the circuit's Ctrl+Delete, the Device view's Delete): removes the devices, each first joining every wire into
// its main input to every wire out of its main output (so a path through it stays connected; its other wires go with
// it). A device without a main input or output simply goes.
EditResult dissolveDevices(const Project& p, IdSource& ids, const DeviceLibrary* lib, std::span<const DeviceId> devices);
// Splice (the circuit's Shift-drag onto a cable): `device` goes between wire `wire`'s ends — wire.from.port → its main
// input, its main output → wire.to.param (the wire's reducer kept). A block of a path leaves it first (joined there); a
// link of another track's path takes the block to that track. A link spliced into stays a path: the block is on it.
EditResult spliceIntoWire(const Project& p, IdSource& ids, const DeviceLibrary* lib, DeviceId device, WireId wire);
// The circuit's "Take Off Path": each device of a main path leaves it (as moveOnPath does where it was: its main sources
// joined to its main targets) and stays as a block with its other wires. Devices off the path are skipped.
EditResult takeOffPath(const Project& p, IdSource& ids, const DeviceLibrary* lib, std::span<const DeviceId> devices);
// A block into group `group` (invalid: out of its group; SetDeviceGroup): a block of the main path leaves it first
// (joined there: takeOffPath). One step.
EditResult setDeviceGroup(const Project& p, IdSource& ids, const DeviceLibrary* lib, DeviceId device, DeviceId group);

// ---- Devices (T4.1; QUESTIONS Q129) -----------------------------------------------------------------------
// Each builder is one Batch = one undo step. Device kinds come from the catalog entry (devices.h). TX.all-free: on a
// track, `index` is a SLOT of its main path (above): the device is spliced in there (kOffPath: added unwired); in a rack
// chain it is the list position.

// Adds a new device of `patch` / `kind` (normally a catalog entry) to `track` at path slot `index` (kAppend: last; an
// effect at slot 0 goes after a stock instrument heading the path). An instrument takes the notes: slot 0; when the path
// starts with a stock instrument or Instrument Rack it replaces it (Live). Instruments only go on MIDI tracks.
// GU.9: `patch` kAudioEffectRack / kInstrumentRack adds a new rack (addRack: a group of parallel chain groups).
// GU.11: `patch` kModuleItemPrefix + a module's key ("module:utility") pastes that module's group preset (modules.h
// moduleBlocks, fresh ids) and puts its group on the path at `index` (kOffPath: off it); an instrument module takes
// the notes (slot 0), replacing the instrument group heading the path. The Edit's first device is the group.
EditResult addDevice(const Project& p, IdSource& ids, TrackId track, std::string patch, DeviceKind kind,
                     uint32_t index = kAppend);
// Removes the devices (any tracks). T4.18: their automation lanes (and those of the wires that go with them) go in the
// same step.
EditResult deleteDevices(const Project& p, std::span<const DeviceId> devices);
// Moves a device to `toTrack` (invalid: its own track) at list position `index` (TX.all-free: on a track that only orders
// its blocks — moveOnPath moves along the path; wires stay). T4.18: its automation lanes move with it.
// TX.all-free: a move in the lists only — the block's cables stay as they are (a block of a main path moved to another
// track leaves that path broken; the views use takeOffPath, which joins it).
EditResult moveDevice(const Project& p, DeviceId device, TrackId toTrack, uint32_t index);
EditResult setDevicesActive(const Project& p, std::span<const DeviceId> devices, bool active);
// Deactivates the devices if any is active, else activates them all (Live's device activator on a selection).
EditResult toggleDevicesActive(const Project& p, std::span<const DeviceId> devices);
// Pastes copies (fresh ids, the same values) of `devices` into `track` at `index` (kAppend: the end). An instrument
// among them replaces the track's instrument (MIDI tracks only); the rest keep their order.
EditResult pasteDevices(const Project& p, IdSource& ids, TrackId track, std::span<const DevicePtr> devices,
                        uint32_t index = kAppend);
// Ctrl+D: copies of the devices right after the last of them in its list (instruments are not duplicated).
EditResult duplicateDevices(const Project& p, IdSource& ids, std::span<const DeviceId> devices);
// A copy of `d` with a fresh id. GU.2: `remap` (optional) gets the (old, new) device id.
DevicePtr cloneDevice(const Device& d, IdSource& ids, std::vector<std::pair<DeviceId, DeviceId>>* remap = nullptr);

// ---- GU.2: block sets — copy / paste of a subgraph, group presets (docs/plans/GU.md "Phases" 2) ------------------
// BlockSet: model.h (a piece of a track's circuit with its wires).
// The blocks `devices` of ONE track's own list (not inside a rack) with everything inside the groups among them (any
// depth) and the wires among them; a picked block's group tag is kept only when that group is picked too. Error: a
// device that is not a track's block, blocks of several tracks.
Result<BlockSet> copyBlocks(const Project& p, std::span<const DeviceId> devices);
// `set` with fresh ids from `ids` (devices, racks' inner devices and chains, wires; group tags, wire ends and mapping
// targets follow the copies). `remap` (optional) gets every (old, new) device id.
BlockSet remapBlocks(const BlockSet& set, IdSource& ids, std::vector<std::pair<DeviceId, DeviceId>>* remap = nullptr);
// Ctrl+V / spawning a group preset: a fresh copy of `set` (remapBlocks) into `track`, off its main path (free blocks
// placed where the view puts them), its top-level blocks into group `intoGroup` (invalid: none). Instruments only on
// MIDI tracks. Edit::devices: the new top-level blocks first (set order), then everything inside them; Edit::wires: the
// new wires. One step; undo removes it all.
EditResult pasteBlocks(const Project& p, IdSource& ids, TrackId track, const BlockSet& set, DeviceId intoGroup = {});
// GU.8: spawning an instrument module (doc::ModuleInfo::instrument): pasteBlocks, and on a MIDI track whose main path
// holds no block the set's first top-level block goes onto it (source → it → mixer: it takes the notes, the
// instrument group). Elsewhere it stays off the path, as pasteBlocks leaves it. One step.
EditResult pasteBlocksAtNotes(const Project& p, IdSource& ids, TrackId track, const BlockSet& set);

// ---- GU.3: group panels (pq/document/panel.h) -----------------------------------------------------------------------
// Each one Meta step ("Edit Panel"; a panel is never lowered). Refused: `group` not a group, a panel failing
// checkPanel, a binding outside the group. An unchanged panel is an empty edit.
// The whole panel (null / empty: none).
EditResult setGroupPanel(const Project& p, DeviceId group, PanelPtr panel);
// `elements` appended (on top): an element with id 0 gets a fresh one (Panel::nextId onwards; `added`, optional, gets
// the ids in order), a non-zero id must be free. The panel grows to hold them (its size becomes at least each
// element's right / bottom edge + kPanelMargin); an empty panel starts at that size.
inline constexpr float kPanelMargin = 8.0f;
EditResult addPanelElements(const Project& p, DeviceId group, std::vector<PanelElement> elements,
                            std::vector<uint32_t>* added = nullptr);
// Element `element.id` replaced (its rect, style, bindings, condition).
EditResult setPanelElement(const Project& p, DeviceId group, PanelElement element);
// The elements removed (unknown ids ignored); conditions naming them dropped. The last one gone: no panel.
EditResult removePanelElements(const Project& p, DeviceId group, std::span<const uint32_t> elements);
// GU.6: BREAK APART — each of `elements` that embeds a child group's panel (panel.h kEmbedKind, a Panel binding) is
// replaced, at its place in the paint order, by copies of the child panel's elements: the same bindings, kinds, styles,
// rects mapped through the embed's fit (embedFit: nothing moves on screen), fresh ids (`added`, optional, gets them in
// order), visibility conditions among them renumbered (one naming an element not copied dropped). An embed inside the
// child stays an embed (one level at a time). Ids that are not embeds are ignored; a child without a panel just
// leaves. The panel grows to hold the copies. One step.
EditResult breakApartPanelElements(const Project& p, DeviceId group, std::span<const uint32_t> elements,
                                   std::vector<uint32_t>* added = nullptr);
// The panel's size (w, h > 0).
EditResult setPanelSize(const Project& p, DeviceId group, float width, float height);
// One param's knob position in [0, 1] (GU.1: what a param stores; a rack's macro or selector: its value 0 … 127;
// nullopt: back to the default — a value block's Device::defaultPosition where set, else the patch's). A drag is several of these inside one store gesture.
EditResult setDeviceParam(const Project& p, DeviceId device, std::string param, std::optional<double> position);
// GU.1: a param set by a value in its units (a typed value, a preset's, a script's): the knob position that gives it
// through the param's curve (the device's own, else the declared one: dsp::curvePosition — on a curve that goes up and
// back down the one nearest the knob's current position; a value the curve never reaches snaps to the nearest it
// does). A rack's params take the value itself. NotFound: no such device or param.
EditResult setDeviceParamValue(const Project& p, const DeviceLibrary& lib, DeviceId device, std::string param, double value);
// One structural setting (nullopt: the default).
EditResult setDeviceSetting(const Project& p, DeviceId device, std::string setting, std::optional<double> value);
// T4.17 (Live: Ctrl+R): the device's own name (Device::name; "" = the patch's default name). One Meta step; an unchanged
// name is an empty edit.
EditResult renameDevice(const Project& p, DeviceId device, std::string name);

// ---- Meta modules (TMM.3; model.h "Meta modules", commands.h) ------------------------------------------------------
// Each builder is one Batch = one undo step (a drag: several inside one store gesture, which commits as one).
// A chain meta device of type `type` ("phase_acc") into `track` (or rack chain `chain`) at `index`. Its "" instance
// holds the type's version and default data. TMM.10-fix1 ("a chain is wires"): any type, any position, any track. It
// lands at `index` with the kind its position says (metaDeviceKindAt; model.h "Meta modules"): first where instruments
// go it takes the note source — a stock instrument or Instrument Rack there is replaced (it takes no chain signal, so
// it could not follow), a chain meta device there moves on as an audio effect (AddDevice normalises the kinds) —;
// anywhere else its chain input takes the previous device's signal. A Wavetable that would receive audio starts in
// Shape mode.
Result<DeviceKind> metaDeviceKindAt(const Project& p, const meta::Type& type, TrackId track, uint32_t index = kAppend);
// GU.1 (model.h ControlCurve): the curve of `device`'s param `param` (nullopt, or a curve equal to the declared one:
// the declared curve, the entry removed). The knob position stays (the sound follows). One step: Structural when the
// curve's class changes against the curve it replaces (dsp::curveClass: a driven control's `curve` atom and the wire
// law follow it), else Param (no compile). Refused for a rack (its macros are 0 … 127), a group, an invalid curve or a
// param the device's patch does not have. "Knob Curve".
EditResult setParamCurve(const Project& p, const DeviceLibrary& lib, DeviceId device, std::string param,
                         std::optional<dsp::Curve> curve);
// The knob curve menus: `preset` over the param's declared range (doc::curveWithPreset; the declared preset: back to
// the declared curve).
EditResult setParamCurvePreset(const Project& p, const DeviceLibrary& lib, DeviceId device, std::string param,
                               dsp::CurvePreset preset);
EditResult addMetaDevice(const Project& p, IdSource& ids, TrackId track, std::string_view type, uint32_t index = kAppend);
// A param / setting / data of the instance (device, path; "" = the chain device itself). A nested call without state
// gets its default state in the same step (AddMetaInstance: no compile), so a drag on a fresh call is a param write;
// `type` is the call's type (used only then).
// GU.1: setMetaParam takes a knob position in [0, 1] (what a param stores), setMetaParamValue a value in the param's
// units (the position that gives it through its curve, nearest the current one: the type's declared curve, a chain
// device's own for path "").
EditResult setMetaParam(const Project& p, DeviceId device, std::string path, std::string_view type, std::string param,
                        std::optional<double> position);
EditResult setMetaParamValue(const Project& p, DeviceId device, std::string path, std::string_view type, std::string param,
                             double value);
EditResult setMetaSetting(const Project& p, DeviceId device, std::string path, std::string_view type, std::string setting,
                          std::optional<double> value);
EditResult setMetaData(const Project& p, DeviceId device, std::string path, std::string_view type, meta::DataPtr data);
// ---- User library (T5.11; pq/document/library_files.h) -----------------------------------------------------------
// Hot-swap: `with` (a preset's device) replaces `device` in place: the same position and id (so wires, mappings and the
// engine's state key follow it); everything inside `with` (a rack's chains, mappings, inner devices) gets fresh ids.
// Wires into params the new device still has (`params`: its patch's params; a rack's macros and selector) and every
// wire out of it are kept, as are the mappings of the racks around it onto those params and (T4.18) the automation
// lanes of those params; the rest go. The kinds must match. One undo step ("Hot-Swap Preset").
EditResult replaceDevice(const Project& p, IdSource& ids, DeviceId device, const Device& with,
                         std::span<const std::string> params);
// A library clip placed at `start` on `track` (note clips on MIDI tracks, audio clips on audio tracks) with fresh ids,
// overwriting what lies under it (an audio clip's warp-Off length re-derived there; no crossfade). `newSample`: the
// audio clip's sample is new to the project (its id drawn by the caller) and is added in the same step; else the clip's
// own sample id must be in the table. Edit::clips = the new clip.
EditResult placeClip(const Project& p, IdSource& ids, TrackId track, const ClipSlot& clip, Ticks start,
                     const std::optional<SampleRef>& newSample = std::nullopt);

// ---- Modulation wires (T4.7; model.h "Modulation wires") ------------------------------------------------------------
// One Batch = one undo step each. Ports are not checked against the device patches (the views offer only real ones).
// Wires `from`.`port` (TX.patching: any output, an `out` or an export) to `to`.`param` (a param or an input); a second
// wire between the same two ports is refused. TX.patching: a new wire ADDS in its target's units (WireLaw::Add, no
// amount: model.h "Modulation wires").
EditResult connectWire(const Project& p, IdSource& ids, DeviceId from, std::string port, DeviceId to, std::string param);
// TMM.10-fix2: wires the note source of MIDI track `track` (its note output `port`: kNoteOutputs) to `to`.`param`.
EditResult connectNoteWire(const Project& p, IdSource& ids, TrackId track, std::string port, DeviceId to, std::string param);
// TX.all-free: a wire with the ends of `ends` (a device's port or a track's source — kMainPort its own signal, else a
// note output —; a device's param / input or a track's mixer; model.h "Modulation wires"), adding (WireLaw::Add). The
// views pass canonicalWire's ends, so a cable between two main ports is a main link.
EditResult connect(const Project& p, IdSource& ids, Wire ends);
// TX.patching: a wire under the legacy knob-travel law (WireLaw::Travel, `amount` its depth, clamped to [−1, 1]) — what
// a schema ≤ 12 file's wires load as. The views never offer it; tests and tools that rebuild such a project use it.
EditResult connectTravelWire(const Project& p, IdSource& ids, DeviceId from, std::string port, DeviceId to,
                             std::string param, double amount = kDefaultWireAmount);
EditResult connectTravelNoteWire(const Project& p, IdSource& ids, TrackId track, std::string port, DeviceId to,
                                 std::string param, double amount = kDefaultWireAmount);
EditResult deleteWires(const Project& p, std::span<const WireId> wires);
// One amount ([−1, 1], clamped) of a Travel wire (an Add wire has none: refused). A drag is several of these inside one
// store gesture (Param: no compile).
EditResult setWireAmount(const Project& p, WireId wire, double amount);
EditResult setWireReducer(const Project& p, WireId wire, WireReducer reducer);
// IG.1-2 (model.h Wire::single): a cable per note or single-signal with `reducer` (sum, max = Highest, newest = Last).
// One step.
EditResult setWireSignal(const Project& p, WireId wire, bool single, WireReducer reducer);
// IG.1-2 (model.h "Instrument groups"): an instrument group's polyphony as settings of the group device (each field
// written; one step).
EditResult setGroupPolyphony(const Project& p, DeviceId group, const GroupPolyphony& poly);
// The wires into `device`.`param` / out of `device`.`port` (any order of the project's list: by id).
std::vector<WireId> wiresInto(const Project& p, DeviceId device, std::string_view param);
std::vector<WireId> wiresFrom(const Project& p, DeviceId device, std::string_view port);

// ---- Cable sources (DY.2; SPEC §8 "sidechain"; the panel element `cablesource`, views/src/panel/cable_source.cpp) ----
// A SIDECHAIN is an ordinary cable from another track into a block's signal input (lower.h "Sidechains": compensated).
// What a picker offers per track (not the master; not the target's own track), in forEachTrack order:
//   Pre   the track's own signal before its blocks — a wire from its source with port "" (Wire::noteSource); a MIDI
//         track's own signal is its notes, so there the main output of its main path's first block (the instrument);
//   Post  the block feeding the track's mixer (the last block of a complete main path, its main output), when that
//         differs from Pre.
// `ends` holds the source end only (noteSource, or from + port kMainPort).
struct CableSource {
  TrackId track;
  bool post = false;
  Wire ends;
};
std::vector<CableSource> cableSources(const Project& p, TrackId except);
// Whether two wires start at the same source end (device + port, or track source + port).
bool sameSourceEnd(const Wire& a, const Wire& b) noexcept;
// The track a wire's source end is on (null: none).
const Track* sourceTrackOf(const Project& p, const Wire& w) noexcept;
// The wires from another track into `input`.`param` (`param` as written or as its main port, kMainPort: canonicalWire
// with `lib`), by id.
std::vector<WireId> crossTrackWiresInto(const Project& p, const DeviceLibrary* lib, DeviceId input, std::string_view param);
// ONE undo step ("Cable Source"): every wire from another track into `input`.`param` removed and, with a `source`,
// a wire from its source end into it connected (Add law, canonicalWire); with `on` (a control: a value block's
// `value`, `onParam`) it set to 1 when a source is picked, 0 when none. Picking what is already the one source is an
// empty edit. Refused: the source on `input`'s own track, an unknown device.
EditResult setCableSource(const Project& p, IdSource& ids, const DeviceLibrary* lib, DeviceId input, std::string param,
                          const std::optional<Wire>& source, DeviceId on = {}, std::string onParam = {});

// ---- Groups (TX.groups; model.h "Groups"; QUESTIONS Q?-TX.groups-a) ----------------------------------------------
// Ctrl+G in the circuit (the project patch view): the selected blocks of ONE track become the members of a new group.
// TX.all-free: blocks on the main path (they must be next to each other on it) are replaced on the path by the group
// (its links take the ids of the path links into the first and out of the last) and become members off the path, their series kept as CHAIN LINKS (Wire::chain; TX.groups-fix1) — the chain's flow as
// wires: the signal so far starts at In; a device whose chain is plugged in takes it (a link into its chain input) and
// gives its main output on, switched off too (a switched-off block off the path passes its main input there, lower.h
// "Free devices", as the path passes it by); an unplugged device takes nothing and gives its output on; the last signal goes
// to Out (the lowering's names: a device's first non-const `in` / a meta module's chain input, its first `out`), so the
// track sounds exactly the same. TX.groups-fix1: what reaches the group is what reached the first one — audio, voices
// (the group continues the run) or, at a MIDI track's note source, the notes (the group is the track's instrument; In
// gives each input it reaches the note's signal of that input's meaning). Selected blocks off the path move in as they
// are (a selection of those only makes a group off the path, wired to nothing). Every other wire stays exactly as it is: wires
// name devices, so modulation across the capsule's boundary keeps working. Refused, with the reason, when the result
// could sound different or could not be ungrouped: devices of several tracks or inside a rack, a rack, a stock
// instrument (its voices are summed at its output), an instrument group (GU.2: other groups nest), blocks of
// several capsules, a gap in the series; where the
// notes or voices reach, anything but a stretch of the voice run (a stock device or an unplugged device inside: the run
// is summed there; a switched-off device where the notes arrive; a stock effect at the note source); a switched-off
// device that is unplugged, an unplugged device whose output the next unplugged one drops (no link would place them);
// a wire already joining two series ports; a wire from a switched-off device's output that would start to play; or a
// feedback loop whose 1-sample delay would move (checked: every wire lowers as before). `lib` gives the port names
// (and the wire states). One step; the selection after = the group.
// GU.2 (model.h "Groups": groups nest): the selection may hold groups (they take their contents with them; not an
// instrument group, which stays outermost) and may sit inside a capsule — every block in the same group, or in none.
// Inside a capsule the new group is a member of that capsule's group and the selection its members, every wire as it
// is (nothing is on a path inside a group: the sound cannot change); ungroupInCircuit gives the members back to the
// group it sat in. At most kMaxGroupDepth deep.
EditResult groupInCircuit(const Snapshot& p, IdSource& ids, const DeviceLibrary& lib, std::span<const DeviceId> devices);
// Ctrl+Shift+G: the exact inverse. TX.groups-fix1: the series is the members the group's chain links touch, in list
// order, and its links must be exactly the ones that series makes (else refused: it was rewired); a group without chain
// links (schema 14) is read back from its wires as before — the one wire into Out, then along each device's chain input
// (exactly one wire from In or a member) back to In. TX.all-free: the series goes back onto the main path at the group's
// place, in that order (its links with the ids the group kept: groupInCircuit gave the group's own links and its inner
// chain links the ids of the path links they stand for); the other members stay where they are, off the path; the
// series' chain links go, every other wire stays. Refused (with the reason) when the wiring is not one series from In
// to Out (several wires into Out, In also feeding another input, a wire into or out of the group's own ports); a group
// off the path must have no In / Out wires. Ungrouping what groupInCircuit made gives back the same document. One step.
EditResult ungroupInCircuit(const Snapshot& p, IdSource& ids, const DeviceLibrary& lib, DeviceId group);

// ---- Racks (GU.9 v2; pq/document/racks.h: a rack is a set of parallel groups, derived from the wiring) ------------
// One Batch = one undo step each. `rack`: any chain group of the rack (or one of its macros, its selector); `chain`: a
// chain group. Each builds blocks and wires only (no lowering special case).
// A new rack: one chain ("Chain 1": a group with its chain mixer) spliced into `track`'s main path at slot `index`
// (kOffPath: unwired). An Instrument Rack takes the notes: it replaces a stock instrument or an instrument group heading
// the path (Live). Edit::devices: the chain.
EditResult addRack(const Project& p, IdSource& ids, TrackId track, bool instrument, uint32_t index = kAppend);
// A new empty chain ("Chain <n>") in parallel with the rack's chains (the same feed and sum), at `index` among them.
EditResult addChain(const Project& p, IdSource& ids, DeviceId rack, uint32_t index = kAppend, std::string name = {});
// A chain holding a new device of `patch` / `kind` (a device dropped on the rack's chain list: Live).
EditResult addChainWithDevice(const Project& p, IdSource& ids, DeviceId rack, std::string patch, DeviceKind kind);
// A new device into chain `chain`'s series at `index` among its devices (kAppend: last, before its zones / mixer). An
// instrument goes first in an Instrument Rack's chain, replacing its instrument; refused in an Audio Effect Rack.
EditResult addToChain(const Project& p, IdSource& ids, DeviceId chain, std::string patch, DeviceKind kind,
                      uint32_t index = kAppend);
// `device` (a block of the same track: on a path — joined there —, in another chain, or a whole rack when it is one of
// its chains) into chain `chain`'s series at `index`. Refused: a rack into itself, an instrument into an Audio Effect Rack.
EditResult moveIntoChain(const Project& p, IdSource& ids, DeviceId device, uint32_t index, DeviceId chain);
// `device` into a new chain of `rack` (a device dragged onto the rack's chain list).
EditResult moveDeviceToNewChain(const Project& p, IdSource& ids, DeviceId device, DeviceId rack);
// Removes the chains with everything inside. The last chain of a rack takes the rack with it (its macros, its selector;
// an Audio Effect Rack's feed joined to its sum, so the path stays connected).
EditResult deleteChains(const Project& p, IdSource& ids, std::span<const DeviceId> chains);
EditResult moveChain(const Project& p, DeviceId chain, uint32_t index);  // among the rack's chains (Track::devices order)
EditResult renameChain(const Project& p, DeviceId chain, std::string name);
EditResult duplicateChain(const Project& p, IdSource& ids, DeviceId chain);  // the copy goes right after it
// The chain mixer: Volume (dB; kMinVolumeDb = −∞: its Gate closes) and Pan, as knob positions of its value blocks (a
// drag is several of these inside one store gesture: param writes).
EditResult setChainMixer(const Project& p, DeviceId chain, float volumeDb, float pan);
EditResult setChainActive(const Project& p, DeviceId chain, bool active);  // kChainOnSetting; the Gates follow
EditResult setChainSolo(const Project& p, DeviceId chain, bool solo, bool exclusive = true);  // exclusive: others off
// A zone of the chain: off (its whole range) removes its zone group, on makes one (a select zone: the rack's selector
// with it, wired in), a change of an existing one writes its four value blocks (param writes).
EditResult setChainZone(const Project& p, IdSource& ids, DeviceId chain, ZoneKind kind, Zone zone);
EditResult setKeyZoneUnit(const Project& p, IdSource& ids, DeviceId rack, KeyZoneUnit unit);  // resets the key zones
// An Instrument Rack's chains: their effects per voice (the instrument stage's out wire per voice) or after the voices
// are summed (single-signal, the default).
EditResult setVoiceChains(const Project& p, DeviceId rack, bool perVoice);
// Maps macro `macro` (0 … 15; its block made when missing, at `macroValue` / 127 or else at the control's position, so
// nothing jumps) to `device`.`param` (inside the rack): a Travel wire, full travel (min 0, max 1). A control mapped by
// another macro of the rack is moved to this one. Edit::wires: the mapping.
EditResult mapMacro(const Project& p, IdSource& ids, DeviceId rack, uint32_t macro, DeviceId device, std::string param,
                    std::optional<double> macroValue = std::nullopt);
// The control keeps where the macro put it; a macro block nothing else reads goes.
EditResult unmapMacro(const Project& p, WireId mapping);
EditResult unmapParam(const Project& p, DeviceId device, std::string_view param);  // whatever macro maps it
EditResult setMacroMapping(const Project& p, WireId mapping, double min, double max);  // knob positions in [0, 1]
EditResult setMacroValue(const Project& p, DeviceId rack, uint32_t macro, double value);  // 0 … 127 (unmapped: nothing)
EditResult setSelectorValue(const Project& p, DeviceId rack, double value);  // 0 … 127, whole steps (none yet: nothing)
EditResult renameMacro(const Project& p, DeviceId rack, uint32_t macro, std::string name);
// Macro variations (Live): store the macro values (as a new one, or over `index`), recall one (every macro: one step),
// delete one; Rand (those not excluded; `seed` makes it reproducible); exclude a macro from Rand.
EditResult storeVariation(const Project& p, DeviceId rack, uint32_t index = kAppend);
EditResult recallVariation(const Project& p, DeviceId rack, uint32_t index);
EditResult deleteVariation(const Project& p, DeviceId rack, uint32_t index);
EditResult randomizeMacros(const Project& p, DeviceId rack, uint64_t seed);
EditResult setMacroExcluded(const Project& p, DeviceId rack, uint32_t macro, bool excluded);
// The Device view's Ctrl+G (Live): neighbouring devices of a track's main path into a new rack of one chain holding them
// in their order, where they were (an instrument among them, heading the path: an Instrument Rack). They keep their ids
// (state, wires, lanes). Ctrl+Shift+G, the inverse for a rack of one chain: its devices take its place (its mixer,
// zones, macros and selector go).
EditResult groupDevices(const Project& p, IdSource& ids, std::span<const DeviceId> devices);
EditResult ungroupDevices(const Project& p, IdSource& ids, DeviceId rack);

// ---- EQ.3: the EQ's bands (pq/document/modules.h "the band and the EQ"; docs/plans/EQ.md "Ops") -------------------
// Each one undo step; the EQ's band strip is laid out again in the same step (modules.h layEqStrip) when the bands change.
// Bands are found by role (modules.h eqInfo / bandInfo). Type, Stages and Mode are structural (one recompile), as a
// knob on those blocks is.
// A band (eqBandBlocks: `type` a dsp::SvfMode index 0 … 7, Q √½, one stage) into chain `chain` (0 = A, 1 = B) of EQ
// `eq`, placed in the series by frequency (before the first band above `freqHz`). Refused past kMaxBandsPerChain (32).
// Edit::devices: the band's group first.
EditResult addBand(const Project& p, IdSource& ids, DeviceId eq, uint32_t chain, uint32_t type, double freqHz, double gainDb);
// The band and what is inside it; its chain closes over it. Removing the soloed band restores the others.
EditResult removeBand(const Project& p, IdSource& ids, DeviceId band);
EditResult setBandType(const Project& p, DeviceId band, uint32_t type);
EditResult setBandStages(const Project& p, DeviceId band, uint32_t stages);  // 1 … 64
// Mode (doc::EqMode). The first switch to L/R or M/S while chain B is empty copies chain A into it (fresh ids).
EditResult setEqMode(const Project& p, IdSource& ids, DeviceId eq, EqMode mode);
// The band group switched off (passes its input through) or on. While another band of its chain is soloed it only
// changes what unsoloing restores.
EditResult setBandBypass(const Project& p, DeviceId band, bool bypass);
// Solo: every other band of the chain switched off, each remembering its state (kSoloRestoreSetting), the band on and
// marked (kBandSoloSetting); soloing another band unsolos the first. Unsolo restores them. Settings: undo and save keep it.
EditResult setBandSolo(const Project& p, DeviceId band, bool solo);

}  // namespace pq::doc::ops
