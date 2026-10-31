#pragma once

// Validation helpers shared by command application (apply.cpp) and the project loader (serialize.cpp), so a loaded
// project satisfies exactly the invariants the commands maintain. Private to pq_doc.

#include <optional>
#include <string>
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
// T5.3: an audio clip's own fields and its sample (in the project's table); its Off length is not checked (commands
// derive it: audio_clip.h syncedClip).
Status checkAudioClip(const Project& p, const AudioClip& c);
// T5.3: a sample table entry's own fields (id, non-empty path, frames >= 1, rate in (0, 10 MHz], 1 to 1024 channels).
Status checkSampleRef(const SampleRef& r);
// Breakpoint ids valid and unique, ticks bounded, values in the parameter's range, curves in [-1, 1].
Status checkPoints(ParamKind k, const std::vector<Breakpoint>& pts);
bool pointLess(const Breakpoint& a, const Breakpoint& b);  // (tick, id)
bool tuningParamApplies(const TuningDef& d, ParamKind k);
bool trackHasSends(TrackKind k);
// Whether `host` may carry a lane for `r` (see AddAutomationLane).
Status checkLaneParam(const Project& p, const Track& host, const ParamRef& r);
// T4.1: a track's device chain: devices present with valid ids, unique in the chain; the chain rule (a MIDI track holds
// at most one instrument, at index 0, then audio effects; other tracks audio effects only); each device's patch name
// non-empty and its params / settings sorted by name, unique, named and finite.
Status checkChain(TrackKind kind, const std::vector<DevicePtr>& devices);
// T4.8: where a device list lives: it decides whether it may start with an instrument.
enum class ListKind : uint8_t { MidiTrack, OtherTrack, InstrumentRackChain, AudioRackChain };
ListKind listKindOf(TrackKind kind) noexcept;
ListKind chainListKind(const Device& rack) noexcept;  // the kind of the lists inside a rack
// A device list (any level): checkChain's rules for that kind of list, and every rack in it (recursively): its patch
// and kind agree, chain ids valid and unique in the rack, chain mixers / zones valid, params only macros and the
// selector (0 … 127), no settings, mappings sorted by id with valid fields onto devices inside the rack (or its own
// selector), at most one mapping per target in the rack.
Status checkList(ListKind kind, const std::vector<DevicePtr>& devices);
// T4.8: one rack (as checkList checks every rack of a list; the chains' lists included).
Status checkRack(const Device& rack);
Status checkMapping(const MacroMapping& m);  // its own fields
// A device param / setting name (non-empty) and value (finite, when set).
Status checkDeviceValue(const std::string& name, const std::optional<double>& value);
// T4.7: a wire's own fields (valid ids, non-empty port and param names, amount in [−1, 1], a known reducer).
Status checkWire(const Wire& w);
// Rejects routing cycles (outputs followed from every track must end at the master or a track with no output).
Status checkRouting(const Project& p);

}  // namespace pq::doc::detail
