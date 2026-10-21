#pragma once

// Validation helpers shared by command application (apply.cpp) and the project loader (serialize.cpp), so a loaded
// project satisfies exactly the invariants the commands maintain. Private to pq_doc.

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
// Breakpoint ids valid and unique, ticks bounded, values in the parameter's range, curves in [-1, 1].
Status checkPoints(ParamKind k, const std::vector<Breakpoint>& pts);
bool pointLess(const Breakpoint& a, const Breakpoint& b);  // (tick, id)
bool tuningParamApplies(const TuningDef& d, ParamKind k);
bool trackHasSends(TrackKind k);
// Whether `host` may carry a lane for `r` (see AddAutomationLane).
Status checkLaneParam(const Project& p, const Track& host, const ParamRef& r);
// Rejects routing cycles (outputs followed from every track must end at the master or a track with no output).
Status checkRouting(const Project& p);

}  // namespace pq::doc::detail
