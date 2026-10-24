// Validation helpers shared by command application (apply.cpp) and the project loader (serialize.cpp).

#include "checks.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace pq::doc::detail {

Error invalid(std::string m) { return Error{ErrorCode::InvalidArgument, std::move(m), {}}; }
Error notFound(std::string m) { return Error{ErrorCode::NotFound, std::move(m), {}}; }
Error unsupported(std::string m) { return Error{ErrorCode::Unsupported, std::move(m), {}}; }
Status ok() { return Ok{}; }

// ---- Validation helpers ------------------------------------------------------------------------------------------

Status checkTick(Ticks t, const char* what) {
  if (t < -kMaxTicks || t > kMaxTicks) return invalid(std::string(what) + " out of range");
  return ok();
}
Status checkLength(Ticks l, const char* what) {
  if (l < 1 || l > kMaxTicks) return invalid(std::string(what) + " must be at least one tick");
  return ok();
}
bool finiteIn(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

Status checkParamValue(ParamKind k, double v) {
  const ParamRange r = paramRange(k);
  if (!finiteIn(v, r.min, r.max)) return invalid("parameter value out of range");
  return ok();
}

Status checkTuningDef(const TuningDef& d) {
  Result<tuning::Tuning> t = buildTuning(d);
  if (!t) return std::move(t).error();
  return ok();
}

Status checkAssignment(const Project& p, const TuningAssignment& a) {
  switch (a.mode) {
    case TuningMode::Inherit: return ok();
    case TuningMode::Main:
      if (!findTuning(p, a.main)) return notFound("main tuning not found");
      return ok();
    case TuningMode::Private:
      if (!a.privateDef) return invalid("private tuning without a definition");
      return checkTuningDef(*a.privateDef);
  }
  return invalid("bad tuning mode");
}

Status checkSignature(TimeSignature sig) {
  if (sig.numerator < 1 || sig.numerator > 1024 || !noteValueIsExact(sig.denominator))
    return invalid("invalid time signature");
  return ok();
}

Status checkLoop(const ClipLoop& l) {
  PQ_DOC_TRY(checkTick(l.start, "loop start"));
  PQ_DOC_TRY(checkTick(l.end, "loop end"));
  if (l.end <= l.start) return invalid("loop end must be after loop start");
  return ok();
}

// Everything about a clip except note validity (NoteSeq contents are validated where notes enter: fromNotes and
// the note commands).
Status checkClip(const Project& p, const NoteClip& c) {
  if (!c.id.valid()) return invalid("clip has no id");
  PQ_DOC_TRY(checkTick(c.start, "clip start"));
  PQ_DOC_TRY(checkLength(c.length, "clip length"));
  PQ_DOC_TRY(checkTick(c.contentStart, "clip content start"));
  PQ_DOC_TRY(checkLoop(c.loop));
  PQ_DOC_TRY(checkSignature(c.signature));
  PQ_DOC_TRY(checkFundamental(c.fundamental));
  return checkAssignment(p, c.tuning);
}

Status checkFundamental(const ClipFundamental& f) {
  if (f.lock && !f.lock->isPositive()) return invalid("a fundamental lock must be positive");
  for (size_t i = 0; i < f.steps.size(); ++i) {
    const HarmonicStep& h = f.steps[i];
    if (!h.note.valid()) return invalid("harmonic step without a note");
    if (i > 0 && !(f.steps[i - 1].note.value < h.note.value)) return invalid("harmonic steps must be sorted by note id");
    if (h.denominator < 1 || h.denominator > kMaxFundamentalDenominator)
      return invalid("harmonic step denominator out of range");
    if (!h.ratio.isPositive()) return invalid("harmonic step ratio must be positive");
  }
  return ok();
}

Status checkVolume(float db) { return checkParamValue(ParamKind::TrackVolume, db); }

Status checkPoints(ParamKind k, const std::vector<Breakpoint>& pts) {
  std::vector<uint32_t> ids;
  ids.reserve(pts.size());
  const ParamRange r = paramRange(k);
  for (const Breakpoint& b : pts) {
    if (!b.id.valid()) return invalid("breakpoint has no id");
    PQ_DOC_TRY(checkTick(b.tick, "breakpoint"));
    if (!finiteIn(b.value, r.min, r.max)) return invalid("breakpoint value out of the parameter's range");
    if (!finiteIn(b.curve, -1.0, 1.0)) return invalid("breakpoint curve must be in [-1, 1]");
    ids.push_back(b.id.value);
  }
  std::sort(ids.begin(), ids.end());
  if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return invalid("duplicate breakpoint id");
  return ok();
}

bool pointLess(const Breakpoint& a, const Breakpoint& b) {
  return a.tick != b.tick ? a.tick < b.tick : a.id < b.id;
}

bool tuningParamApplies(const TuningDef& d, ParamKind k) {
  switch (k) {
    case ParamKind::TuningReference: return true;
    case ParamKind::TuningEquave: return d.edo() != nullptr;
    case ParamKind::MosGenerator:
    case ParamKind::MosPeriod:
    case ParamKind::MosStretch:
      return d.mos() != nullptr;
    default: return false;
  }
}

bool trackHasSends(TrackKind k) { return k == TrackKind::Midi || k == TrackKind::Group || k == TrackKind::Return; }

// Whether `host` may carry a lane for `r` (see AddAutomationLane).
Status checkLaneParam(const Project& p, const Track& host, const ParamRef& r) {
  if (!isAutomatable(r.kind)) return invalid("parameter is not automatable");
  if (isTrackParam(r.kind)) {
    if (r.owner != host.id.value) return invalid("a track parameter can only be automated on its own track");
    if (r.kind == ParamKind::TrackMute && host.kind == TrackKind::Master) return invalid("the master has no mute");
    if (r.kind == ParamKind::TrackSend) {
      if (!trackHasSends(host.kind)) return invalid("this track has no sends");
      const Track* target = findTrackPtr(p, TrackId{r.target});
      if (!target || target->kind != TrackKind::Return || target->id == host.id)
        return invalid("send target must be another return track");
    } else if (r.target != 0) {
      return invalid("only sends have a target");
    }
    return ok();
  }
  const TuningSlot* slot = findTuning(p, TuningId{r.owner});
  if (!slot) return notFound("automated tuning not found");
  if (r.target != 0) return invalid("tuning parameters have no target");
  if (!tuningParamApplies(slot->def, r.kind)) return invalid("parameter does not apply to this tuning's kind");
  return ok();
}

// Rejects routing cycles: following each track's output (Default → parent group or master) must end at the
// master or at a track with no output, within one step per track.
Status checkRouting(const Project& p) {
  struct Hop {
    uint32_t id;
    uint32_t next;  // 0 = ends here
  };
  std::vector<Hop> hops;
  const uint32_t masterId = p.master ? p.master->id.value : 0;
  forEachTrack(p, [&](const Track& t, const Track* parent) {
    uint32_t next = 0;
    if (t.kind != TrackKind::Master) {
      switch (t.mixer.output.kind) {
        case OutputKind::Default: next = parent ? parent->id.value : masterId; break;
        case OutputKind::Master: next = masterId; break;
        case OutputKind::Track: next = t.mixer.output.track.value; break;
        case OutputKind::None: next = 0; break;
      }
    }
    hops.push_back({t.id.value, next});
  });
  std::sort(hops.begin(), hops.end(), [](const Hop& a, const Hop& b) { return a.id < b.id; });
  auto nextOf = [&](uint32_t id) -> uint32_t {
    auto it = std::lower_bound(hops.begin(), hops.end(), id, [](const Hop& h, uint32_t v) { return h.id < v; });
    return (it != hops.end() && it->id == id) ? it->next : 0;
  };
  for (const Hop& h : hops) {
    uint32_t at = h.next;
    for (size_t steps = 0; at != 0; ++steps) {
      if (at == h.id || steps > hops.size()) return invalid("routing would create a cycle");
      at = nextOf(at);
    }
  }
  return ok();
}

}  // namespace pq::doc::detail
