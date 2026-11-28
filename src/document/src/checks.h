#pragma once

// Validation helpers shared by command application (apply.cpp) and the project loader (serialize.cpp), so a loaded
// project satisfies exactly the invariants the commands maintain. Private to pq_doc.

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/model.h"

// Returns the error of a failed Status expression from the enclosing function.
#define PQ_DOC_TRY(expr)                                       do {                                                           if (::pq::Status pqDocSt_ = (expr); !pqDocSt_)                 return std::move(pqDocSt_).error();                      } while (0)

namespace pq::doc::detail {

Error invalid(std::string m);
Error notFound(std::string m);
Error unsupported(std::string m);
Status ok();

Status checkTick(Ticks t, const char* what);              // |t| <= kMaxTicks
Status checkLength(Ticks l, const char* what);            // 1 <= l <= kMaxTicks
bool finiteIn(double v, double lo, double hi);
Status checkParamValue(ParamKind k, double v);            // within paramRange(k)
Status checkVolume(float db);
Status checkTuningDef(const TuningDef& d);                // buildTuning succeeds
Status checkAssignment(const Project& p, const TuningAssignment& a);
Status checkSignature(TimeSignature sig);
Status checkLoop(const ClipLoop& l);
// Everything about a clip except note validity (validated where notes enter: NoteSeq::fromNotes, note commands).
Status checkClip(const Project& p, const NoteClip& c);
Status checkFundamental(const ClipFundamental& f);  // T3.6-fix1: lock > 0; steps sorted by note id, in range
// T7.13 (model.h "JI groups, warps and note curves").
// A note curve: ≤ kMaxCurvePoints, ticks in [0, kMaxTicks] non-decreasing, |value| ≤ kMaxCurveCents, curve in
// [−1, 1], no ids.
Status checkCurve(const std::vector<Breakpoint>& pts, const char* what);
Status checkGlideShape(const GlideShape& s);
// One group's own fields: an id, an anchor id, index in [1, kMaxHarmonicIndex], warps in range, strictly ascending,
// alternating, no glide running into the next warp.
Status checkJiGroup(const JiGroup& g);
// A clip's group list: every group, sorted by id and unique, no note the anchor of two.
Status checkJiGroups(const JiGroupList& l);
// A clip's written identities: sorted by note id, unique, each Degree / finite Cents / a reference-anchored Ratio.
Status checkWrittenPitch(const tuning::PitchId& p);
Status checkNotesWritten(const NoteWrittenList& l);
// Every group's anchor and every written note is a note of `c`. O(notes · log(groups + written)).
Status checkJiNotes(const NoteClip& c, std::span<const JiGroup> groups, std::span<const NoteWritten> written);
// T7.14: a vibrato: none (all defaults), or a depth 0 < |depth| ≤ kMaxVibratoCents (negative: inverted), a rate in its unit's range, onset and
// fades in [0, kMaxTicks], end 0 (the note's end) or after the onset.
Status checkNoteVibrato(const NoteVibrato& v);
// T7.11a: one curve record's own fields (the free curve; each bend: none, or a neighbour ≠ the note, a length in
// [1, kMaxTicks] and a valid shape; the vibrato). Whether the neighbour is in the clip is the command's check
// (SetNoteCurves).
Status checkNoteCurveRecord(const NoteCurves& c);
// A clip's curve list: sorted by note id, unique, every record non-empty and valid.
Status checkNoteCurves(const NoteCurveList& l);
// Everything above for a whole clip (where a clip enters: AddClip, a track, a take lane, the loader).
Status checkClipJi(const NoteClip& c);
// T5.3: an audio clip's own fields and its sample (in the project's table); its Off length is not checked (commands
// derive it: audio_clip.h syncedClip).
Status checkAudioClip(const Project& p, const AudioClip& c);
// T6.1.
Status checkWarpMarkers(const std::vector<WarpMarker>& m);
Status checkWarpParams(const WarpParams& w);
Status checkRecordInputs(TrackKind kind, const Mixer& m);  // arm, monitor, audio / MIDI input, track delay
Status checkNoteInput(const NoteInput& n);
Status checkPunch(const PunchRegion& r);
Status checkRecordSettings(const RecordSettings& r);
// T5.3: a sample table entry's own fields (id, non-empty path, frames >= 1, rate in (0, 10 MHz], 1 to 1024 channels).
Status checkSampleRef(const SampleRef& r);
// Breakpoint ids valid and unique, ticks bounded, values in the parameter's range, curves in [-1, 1].
Status checkPoints(const ParamRef& r, const std::vector<Breakpoint>& pts);  // T4.18: paramRange(r)
bool pointLess(const Breakpoint& a, const Breakpoint& b);  // (tick, id)
bool tuningParamApplies(const TuningDef& d, ParamKind k);
bool trackHasSends(TrackKind k);
// Whether `host` may carry a lane for `r` (see AddAutomationLane). T4.18: a device-kind param (isDeviceParamKind) only
// on the track holding its device / wire target (looked up in host's own devices, so a track being inserted with its
// devices checks against itself); `name` = AutomationLane::paramName (a DeviceParam's: an identifier whose hash is
// r.target; "" for every other kind).
Status checkLaneParam(const Project& p, const Track& host, const ParamRef& r, std::string_view name);
// T4.1: a track's device chain: devices present with valid ids, unique in the chain; the chain rule (a MIDI track holds
// at most one instrument, at index 0, then audio effects; other tracks audio effects only); each device's patch name
// non-empty and its params / settings sorted by name, unique, named and finite.
// TMM.13-fix1 M8 (`where`, optional): on a refusal, the location of the device it is about — its index in `devices`
// (empty when the list itself is refused).
Status checkChain(TrackKind kind, const std::vector<DevicePtr>& devices, std::vector<size_t>* where = nullptr);
// Where a device list lives: it decides whether it may hold an instrument.
enum class ListKind : uint8_t { MidiTrack, OtherTrack };
ListKind listKindOf(TrackKind kind) noexcept;
// A device list: checkChain's rules for that kind of list.
Status checkList(ListKind kind, const std::vector<DevicePtr>& devices, std::vector<size_t>* where = nullptr);
// TMM.13-fix1 L7: a chain link (Wire::chain, a grouped series' link) joins, on one track, a group's In terminal, its
// members and its Out terminal, of one group that is in the series (not free). "" = fine (or not a chain link).
std::string chainLinkProblem(const Project& p, const Wire& w);
// A device param / setting name (non-empty) and value (finite, when set).
Status checkDeviceValue(const std::string& name, const std::optional<double>& value);
// TX.knob-curves: Device::curves sorted, unique, never Default.
Status checkParamCurves(const Device& d);
// TMM.3 (model.h "Meta modules"): one instance of `d` at `path` — a valid path ("" only on a chain meta device of that
// type, without params / settings), a registered type at its current version, the type's param / setting names
// (sorted, unique, finite), data the type accepts (none when it keeps none).
Status checkMetaInstance(const Device& d, std::string_view path, const MetaInstance& inst);
// A device's meta map (sorted by path, unique, non-empty when set, each instance as above) and, for a chain meta device
// ("meta:<type>"), its type registered and its kind one the type allows (instrument / audio effect).
Status checkDeviceMeta(const Device& d);
// T4.7: a wire's own fields (valid ids, non-empty port and param names, amount in [−1, 1], a known reducer).
Status checkWire(const Wire& w);
// Rejects routing cycles (outputs followed from every track must end at the master or a track with no output).
Status checkRouting(const Project& p);

// ---- RT.1: Audio In (pq/document/audio_in.h "Loops") ------------------------------------------------------------------
// Which tracks a track's sound reaches, built once per project: a track FEEDS the track its output goes to (Default: its
// parent group or the master), the returns of its STORED sends (a send at its default, −∞ post-fader, is not stored and
// is lowered only while stored when it would close a loop: lower.h conditionalSendsKey), and the track at the other end
// of every cable that leaves it (into a block, a mixer or an Audio In of another track). Not RT code.
struct AudioFeeds {
  explicit AudioFeeds(const Project& p);
  uint32_t master = 0;                                          // the master's id (0: none)
  std::unordered_map<uint32_t, uint32_t> deviceTrack;           // device id → its track's id
  std::unordered_map<uint32_t, uint32_t> output;                // track id → where its output goes (absent: nowhere)
  std::unordered_map<uint32_t, std::vector<uint32_t>> others;   // track id → the other tracks it feeds (sends, cables)
  uint32_t sourceTrack(const Wire& w) const noexcept;  // the track of a wire's start (0: none)
  uint32_t targetTrack(const Wire& w) const noexcept;  // the track of a wire's end (0: none)
  // Every track `track` feeds, transitively, itself included (sorted). `ownOutput` false: its own output route is left
  // out (Resampling reads the master built without the recording track's bus).
  std::vector<uint32_t> fedBy(uint32_t track, bool ownOutput) const;
};
// Why a cable from `start` onto audio track `track`'s Audio In would loop: kAudioInSelf, kAudioInLoop, or "".
std::string_view audioInLoop(const AudioFeeds& f, uint32_t track, const Wire& start);
// Every Audio In of the project: on an audio track, one source (one cable, or Ext. In), audio (library-free:
// wire_types.h carriesAudio), no loop. Cheap when the project has no Audio In cable (one pass over the wires).
Status checkAudioIns(const Project& p);

}  // namespace pq::doc::detail
