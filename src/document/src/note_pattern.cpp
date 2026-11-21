// AP.10: the resolved pattern of a note device (pq/document/note_pattern.h).
#include "pq/document/note_pattern.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

#include "pq/document/lower.h"
#include "pq/document/note_devices.h"
#include "pq/document/pitch_resolve.h"
#include "pq/engine/arp_sketch.h"

namespace pq::doc {

namespace {

engine::ArpDegreeTable tableOf(const NoteDegreeTable& t) { return {t.cents, t.equaveCents, t.referenceHz}; }

// The track's degree tables as the lowering gives its stage (lower.h lowerNoteChains).
engine::ArpTables tablesOf(const Project& p, TrackId track) {
  engine::ArpTables r;
  const std::optional<TrackPath> at = findTrack(p, track);
  if (!at) return r;
  const EffectiveTuning et = effectiveTuningRef(p, *at);
  NoteDegreeTable t;
  if (et.def != nullptr && noteDegreeTable(*et.def, t)) r.track = tableOf(t);
  for (const TuningId id : trackTuningSignals(p, *at)) {
    NoteDegreeTable n;
    if (const TuningSlot* ts = findTuning(p, id)) (void)noteDegreeTable(ts->def, n);
    r.nodes.push_back(tableOf(n));
  }
  return r;
}

// A note's identity in the device's unit: Harmonics / Harmonic Walk: its harmonic; Degrees: the degrees moved;
// Ratio (and a layer): the exact ratio to its source.
std::string labelOf(const engine::ArpSketchNote& n, const engine::ArpParams& p) {
  using K = engine::ArpExact::Kind;
  const bool harm = p.unit == engine::ArpUnit::Harmonics || p.style == engine::ArpStyle::HarmonicWalk;
  char s[48];
  if (harm && n.harmonic >= 1.0f && n.harmonic < 1e6f && n.harmonic == std::floor(n.harmonic)) {
    std::snprintf(s, sizeof s, "h%d", static_cast<int>(n.harmonic));
    return s;
  }
  switch (n.exact.kind) {
    case K::Same: return {};
    case K::Degrees:
      std::snprintf(s, sizeof s, "%+lld", static_cast<long long>(n.exact.degrees));
      return s;
    case K::Ratio:
      std::snprintf(s, sizeof s, "%lld/%lld", static_cast<long long>(n.exact.num), static_cast<long long>(n.exact.den));
      return s;
    default: return "\xE2\x89\x88";  // ≈ (only its Hz is known)
  }
}

// The device's params as the engine reads them (sanitized), and its type (null: not an arpeggiator).
const NoteDeviceType* paramsOf(const Project& p, DeviceId device, engine::ArpParams& params, engine::ArpLaneSteps* lanes,
                               DeviceLoc& loc) {
  loc = findDevice(p, device);
  if (loc.device == nullptr || loc.track == nullptr) return nullptr;
  const NoteDeviceType* type = noteDeviceTypeOf(*loc.device);
  if (type == nullptr || type->stage != NoteStageKind::Arpeggiator) return nullptr;
  engine::ArpLaneSteps scratch;
  for (size_t k = 0; k < type->params.size() && k < static_cast<size_t>(engine::ArpParamId::Count); ++k) {
    const auto id = static_cast<engine::ArpParamId>(k);
    const auto v = static_cast<float>(noteDeviceValue(*loc.device, type->params[k]));
    if (!engine::setArpLaneStep(lanes != nullptr ? *lanes : scratch, id, v)) engine::setArpParam(params, id, v);
  }
  params = engine::sanitized(params);
  return type;
}

// Cents of degree D of table `t` (12-EDO when empty).
double degreeCents(const engine::ArpDegreeTable& t, int64_t d) {
  const auto n = static_cast<int64_t>(t.cents.empty() ? 12 : t.cents.size());
  const int64_t q = d >= 0 ? d / n : -((-d + n - 1) / n);
  const int64_t r = d - q * n;
  const double c = t.cents.empty() ? 100.0 * static_cast<double>(r) : t.cents[static_cast<size_t>(r)];
  return static_cast<double>(q) * (t.cents.empty() ? 1200.0 : t.equaveCents) + c;
}

double offsetCents(const engine::ArpParams& params, const engine::ArpTables& tables, double offset) {
  if (params.glideCents) return offset;
  const auto k = static_cast<int64_t>(std::llround(offset));
  switch (params.unit) {
    case engine::ArpUnit::Harmonics: {
      const double h = params.harmonicBase;
      return h + static_cast<double>(k) < 1.0 ? std::numeric_limits<double>::quiet_NaN()
                                              : 1200.0 * std::log2((h + static_cast<double>(k)) / h);
    }
    case engine::ArpUnit::Ratio:
      return static_cast<double>(k) * 1200.0 * std::log2(static_cast<double>(params.ratioP) / params.ratioQ);
    default: return degreeCents(tables.track, k) - degreeCents(tables.track, 0);
  }
}

std::string offsetText(const engine::ArpParams& params, double offset) {
  char s[48];
  if (params.glideCents) {
    std::snprintf(s, sizeof s, "%+.0f \xC2\xA2", offset);  // ¢
    return s;
  }
  const auto k = static_cast<long long>(std::llround(offset));
  switch (params.unit) {
    case engine::ArpUnit::Harmonics:
      std::snprintf(s, sizeof s, "h%d → h%lld", params.harmonicBase, params.harmonicBase + k);  // ->
      break;
    case engine::ArpUnit::Ratio: std::snprintf(s, sizeof s, "× (%d/%d)^%lld", params.ratioP, params.ratioQ, k); break;
    default: std::snprintf(s, sizeof s, "%+lld deg", k); break;
  }
  return s;
}

}  // namespace

NoteGlideView noteGlideView(const Project& p, DeviceId device, double bpm) {
  NoteGlideView v;
  engine::ArpParams params;
  DeviceLoc loc;
  if (paramsOf(p, device, params, nullptr, loc) == nullptr) return v;
  v.valid = true;
  bpm = bpm > 0.0 ? bpm : 120.0;
  const engine::ArpRate& r = params.rate;
  v.stepMs = r.synced ? 4.0 * 60000.0 / bpm * r.num * r.tupletM / (static_cast<double>(r.den) * r.tupletN) : r.ms;
  v.gate = params.gate;
  v.cents = params.glideCents;
  const engine::ArpTables tables = tablesOf(p, loc.track->id);
  const double noteMs = v.gate * v.stepMs;
  auto side = [&](const engine::ArpGlideSide& g, NoteGlideSide& o) {
    o.mode = static_cast<int32_t>(g.mode);
    o.offset = g.offset;
    o.cents = offsetCents(params, tables, g.offset);
    o.text = offsetText(params, g.offset);
    o.synced = g.synced;
    o.shape = static_cast<int32_t>(g.shape);
    o.bend = g.bend;
    o.ms = g.mode == engine::ArpGlide::Off ? 0.0 : std::min(g.synced ? g.fraction * noteMs : static_cast<double>(g.ms), noteMs);
  };
  side(params.into, v.into);
  side(params.out, v.out);
  if (v.into.ms > 0.0 && v.out.ms > 0.0 && v.into.ms + v.out.ms > noteMs) {  // both: shrunk in proportion
    const double sum = v.into.ms + v.out.ms;
    v.into.ms = noteMs * v.into.ms / sum;
    v.out.ms = noteMs - v.into.ms;
  }
  if (v.stepMs > 0.0) {
    v.into.steps = v.into.ms / v.stepMs;
    v.out.steps = v.out.ms / v.stepMs;
  }
  return v;
}

double noteGlideOffsetCents(const Project& p, DeviceId device, double offset) {
  engine::ArpParams params;
  DeviceLoc loc;
  if (paramsOf(p, device, params, nullptr, loc) == nullptr) return 0.0;
  return offsetCents(params, tablesOf(p, loc.track->id), offset);
}

double noteRampShape(int32_t shape, double bend, double u) noexcept {
  const int32_t sh = std::clamp(shape, 0, static_cast<int32_t>(engine::RampShape::Count) - 1);
  return engine::rampShapeAt(static_cast<engine::RampShape>(sh), engine::rampBendOf(static_cast<float>(bend)), u);
}

NotePattern notePattern(const Project& p, DeviceId device, std::span<const NotePatternHeld> held, double bpm) {
  NotePattern out;
  engine::ArpParams params;
  engine::ArpLaneSteps lanes;
  DeviceLoc loc;
  if (paramsOf(p, device, params, &lanes, loc) == nullptr) return out;
  out.valid = true;
  const engine::ArpTables tables = tablesOf(p, loc.track->id);
  std::vector<engine::ArpSketchHeld> in;
  for (const NotePatternHeld& h : held) {
    if (!(h.hz > 0.0f) || !std::isfinite(h.hz)) continue;
    engine::ArpSketchHeld e;
    e.hz = h.hz;
    e.velocity = h.velocity > 0.0f ? h.velocity : 0.8f;
    e.ratio = h.ratio;
    e.harmonic = h.harmonic;
    e.degree = h.degree;
    e.equave = h.equave;
    e.tuning = h.tuning;
    e.noteDegree = h.noteDegree;
    in.push_back(e);
  }
  if (in.empty()) {  // the nominal note: degree 0 of the track's tuning (else middle C), no JI group (Base)
    out.nominal = true;
    engine::ArpSketchHeld e;
    const engine::ArpDegreeTable& t = tables.track;
    const double c0 = t.cents.empty() ? 0.0 : t.cents[0];
    e.hz = t.referenceHz > 0.0 ? static_cast<float>(t.referenceHz * std::exp2(c0 / 1200.0)) : 261.6256f;
    e.degree = e.equave = e.noteDegree = 0.0f;
    in.push_back(e);
  }
  engine::ArpSketchConfig cfg;
  cfg.bpm = bpm > 0.0 ? bpm : 120.0;
  const engine::ArpSketch s = engine::sketchArp(params, lanes, tables, in, cfg);
  out.cycle = s.cycle;
  out.stepMs = s.stepMs;
  out.truncated = s.truncated;
  float lo = std::numeric_limits<float>::max(), hi = -lo;
  for (const engine::ArpSketchNote& n : s.notes) {
    NotePatternNote o;
    o.on = n.on;
    o.off = std::max(n.off, n.on);
    o.hz = n.ownHz;
    // A held note itself: its own identity, or (Harmonic Walk) a walk element at a held note's pitch (the merged walk
    // keeps the lowest note's element: h6 of h4 is the held h6).
    o.source = n.exact.kind == engine::ArpExact::Kind::Same;
    if (!o.source && params.style == engine::ArpStyle::HarmonicWalk)
      for (const engine::ArpSketchHeld& h : in)
        o.source = o.source || std::fabs(std::log2(static_cast<double>(n.ownHz) / h.hz)) < 1e-6;
    o.label = labelOf(n, params);
    o.position = n.position;
    o.copy = n.copy;
    o.bends = n.numBends > 0;
    // Its curve: the start, every Pitch event's start and end, 16 points inside each shaped ramp (linear in cents is
    // a line in log Hz), the end.
    std::vector<float> ts = {o.on, o.off};
    for (uint32_t b = 0; b < n.numBends; ++b) {
      const engine::ArpSketchBend& bd = s.bends[n.firstBend + b];
      ts.push_back(bd.at);
      ts.push_back(bd.at + bd.len);
      if (bd.len > 0.0f && bd.shape != engine::RampShape::Linear)
        for (int i = 1; i < 16; ++i) ts.push_back(bd.at + bd.len * static_cast<float>(i) / 16.0f);
    }
    std::sort(ts.begin(), ts.end());
    for (const float t : ts) {
      if (t < o.on || t > o.off) continue;
      if (!o.curve.empty() && o.curve.back().first == t) continue;
      const double hz = engine::arpSketchHzAt(s, n, t);
      if (!(hz > 0.0) || !std::isfinite(hz)) continue;
      const auto y = static_cast<float>(std::log2(hz));
      o.curve.emplace_back(t, y);
      lo = std::min(lo, y);
      hi = std::max(hi, y);
    }
    if (o.curve.empty()) continue;
    out.notes.push_back(std::move(o));
  }
  if (out.notes.empty()) return out;
  out.lo = lo;
  out.hi = hi;
  // Reference lines on the unit's grid within the range: Degrees: the track table's degrees (equaves major; only the
  // equaves when the degrees would be denser than ~25 lines); Harmonics / Walk: the harmonics of the lowest source's
  // anchor (its JI anchor, else the note as harmonic Base; octaves major); Ratio: (p/q)^j of it.
  const float pad = 0.02f;
  auto add = [&](double l2, bool major) {
    if (l2 >= lo - pad && l2 <= hi + pad) out.grid.emplace_back(static_cast<float>(l2), major);
  };
  const NotePatternNote* low = nullptr;
  for (const NotePatternNote& n : out.notes)
    if (n.source && (low == nullptr || n.hz < low->hz)) low = &n;
  if (low == nullptr) low = &out.notes.front();
  const double lowHz = low->hz;
  const bool walk = params.style == engine::ArpStyle::HarmonicWalk;
  if (params.unit == engine::ArpUnit::Degrees && !walk) {
    const engine::ArpDegreeTable& t = tables.track;
    std::vector<double> cents = t.cents;
    if (cents.empty())
      for (int i = 0; i < 12; ++i) cents.push_back(100.0 * i);
    const double eq = t.equaveCents > 0.0 ? t.equaveCents : 1200.0;
    const double ref = t.referenceHz > 0.0 ? t.referenceHz : 261.6256;
    const double span = std::max<double>(hi - lo, 1.0 / 12.0) * 1200.0;
    const bool dense = static_cast<double>(cents.size()) * span / eq > 25.0;
    const double l0 = std::log2(ref);
    const auto q0 = static_cast<int64_t>(std::floor((lo - l0) * 1200.0 / eq)) - 1;
    const auto q1 = static_cast<int64_t>(std::ceil((hi - l0) * 1200.0 / eq)) + 1;
    for (int64_t q = q0; q <= q1; ++q)
      for (size_t d = 0; d < cents.size(); ++d)
        if (!dense || d == 0) add(l0 + (static_cast<double>(q) * eq + cents[d]) / 1200.0, d == 0);
  } else if (params.unit == engine::ArpUnit::Ratio && !walk) {
    const double r = std::log2(static_cast<double>(params.ratioP) / params.ratioQ);
    if (std::fabs(r) > 1e-6)
      for (int j = -64; j <= 64; ++j) add(std::log2(lowHz) + j * r, j == 0);
  } else {
    double h = 0.0;
    for (const engine::ArpSketchHeld& e : in)
      if (std::fabs(static_cast<double>(e.hz) - lowHz) < 1e-3 * lowHz) h = e.harmonic;
    if (!(h >= 1.0)) h = params.harmonicBase;
    const double anchor = std::log2(lowHz / h);
    for (int k = 1; k <= 256; ++k) add(anchor + std::log2(static_cast<double>(k)), (k & (k - 1)) == 0);
  }
  return out;
}

}  // namespace pq::doc
