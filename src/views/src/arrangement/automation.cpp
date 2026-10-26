// Automation (T3.15): parameter names, display scales and values; envelope evaluation for drawing.

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "pq/views/arrangement/automation.h"
#include "pq/document/pitch_resolve.h"
#include "pq/views/mixer/mixer.h"

namespace pq::views {

namespace {

const doc::Track* trackOf(const doc::Project& p, uint32_t raw) noexcept { return doc::findTrackPtr(p, doc::TrackId{raw}); }

// Index of a return among the project's returns (its letter: 'A' + index), −1 if not a return.
int32_t returnIndex(const doc::Project& p, uint32_t raw) noexcept {
  for (size_t i = 0; i < p.returns.size(); ++i)
    if (p.returns[i]->id.value == raw) return static_cast<int32_t>(i);
  return -1;
}

std::string returnLetter(int32_t i) {
  if (i < 0) return "?";
  std::string s;
  int32_t n = i;
  do {
    s.insert(s.begin(), static_cast<char>('A' + n % 26));
    n = n / 26 - 1;
  } while (n >= 0);
  return s;
}

bool tuningParamFits(const doc::TuningDef& d, doc::ParamKind k) noexcept {
  switch (k) {
    case doc::ParamKind::TuningReference: return true;
    case doc::ParamKind::TuningEquave: return d.edo() != nullptr;
    case doc::ParamKind::MosGenerator:
    case doc::ParamKind::MosPeriod:
    case doc::ParamKind::MosStretch: return d.mos() != nullptr;
    default: return false;
  }
}

bool hasSends(doc::TrackKind k) noexcept { return k == doc::TrackKind::Midi || k == doc::TrackKind::Group || k == doc::TrackKind::Return; }

const char* tuningParamLabel(doc::ParamKind k) noexcept {
  switch (k) {
    case doc::ParamKind::TuningReference: return "Reference";
    case doc::ParamKind::TuningEquave: return "Equave";
    case doc::ParamKind::MosGenerator: return "Generator";
    case doc::ParamKind::MosPeriod: return "Period";
    case doc::ParamKind::MosStretch: return "Stretch";
    default: return "";
  }
}

}  // namespace

// ---- parameters --------------------------------------------------------------------------------------------------

ParamDisplay paramDisplay(const doc::Project& p, const doc::ParamRef& r) noexcept {
  using S = ParamDisplay::Scale;
  switch (r.kind) {
    case doc::ParamKind::TrackVolume:
    case doc::ParamKind::TrackSend: return {S::Fader, doc::kMinVolumeDb, doc::kMaxVolumeDb};
    case doc::ParamKind::TrackPan: return {S::Linear, -1.0, 1.0};
    case doc::ParamKind::TrackMute:
    case doc::ParamKind::TrackSolo: return {S::Inverted, 0.0, 1.0};
    case doc::ParamKind::TuningReference: return {S::Log, 20.0, 20000.0};
    case doc::ParamKind::TuningEquave: return {S::Linear, 600.0, 2400.0};
    case doc::ParamKind::MosGenerator: {
      // The MOS's region around its generator (where the step pattern holds, between the Farey neighbours of order n
      // of g / period: 5L 2s spans 4\7 … 3\5), widened by a quarter on each side so a sweep into the neighbouring
      // regions stays visible. One period when the definition is unusable.
      const doc::TuningSlot* s = doc::findTuning(p, doc::TuningId{r.owner});
      const tuning::MosParams* m = s ? s->def.mos() : nullptr;
      if (!m || !(m->period > 0.0) || m->n < 2 || !std::isfinite(m->generator)) return {S::Linear, 0.0, 1200.0};
      const double turns = std::floor(m->generator / m->period);
      const double x = m->generator / m->period - turns;  // in [0, 1)
      int64_t a = 0, b = 1, c = 1, d = 1;                 // a/b ≤ x ≤ c/d, Farey neighbours (Stern–Brocot descent)
      for (int i = 0; i < 4096; ++i) {
        const int64_t mn = a + c, md = b + d;
        if (md > m->n) break;
        const double mid = static_cast<double>(mn) / static_cast<double>(md);
        if (x < mid) {
          c = mn;
          d = md;
        } else if (x > mid) {
          a = mn;
          b = md;
        } else {
          break;
        }
      }
      const double lo = (turns + static_cast<double>(a) / static_cast<double>(b)) * m->period;
      const double hi = (turns + static_cast<double>(c) / static_cast<double>(d)) * m->period;
      const double pad = 0.25 * (hi - lo);
      return {S::Linear, lo - pad, hi + pad};
    }
    case doc::ParamKind::MosPeriod: return {S::Linear, 600.0, 2400.0};
    case doc::ParamKind::MosStretch: return {S::Linear, -100.0, 100.0};
  }
  return {};
}

double displayNorm(const ParamDisplay& d, double v) noexcept {
  double n = 0.0;
  switch (d.scale) {
    case ParamDisplay::Scale::Linear: n = (v - d.lo) / (d.hi - d.lo); break;
    case ParamDisplay::Scale::Fader: n = volumeToFader(v); break;
    case ParamDisplay::Scale::Log: n = v > 0.0 ? std::log(v / d.lo) / std::log(d.hi / d.lo) : 0.0; break;
    case ParamDisplay::Scale::Inverted: n = 1.0 - (v - d.lo) / (d.hi - d.lo); break;
  }
  return std::isfinite(n) ? std::clamp(n, 0.0, 1.0) : 0.0;
}

double displayValue(const ParamDisplay& d, double n) noexcept {
  n = std::isfinite(n) ? std::clamp(n, 0.0, 1.0) : 0.0;
  switch (d.scale) {
    case ParamDisplay::Scale::Linear: return d.lo + n * (d.hi - d.lo);
    case ParamDisplay::Scale::Fader: return faderToVolume(n);
    case ParamDisplay::Scale::Log: return d.lo * std::exp(n * std::log(d.hi / d.lo));
    case ParamDisplay::Scale::Inverted: return n >= 0.5 ? d.lo : d.hi;  // the activator is on or off
  }
  return d.lo;
}

std::string paramName(const doc::Project& p, const doc::ParamRef& r) {
  switch (r.kind) {
    case doc::ParamKind::TrackVolume: return "Track Volume";
    case doc::ParamKind::TrackPan: return "Track Panning";
    case doc::ParamKind::TrackMute: return "Track Activator";
    case doc::ParamKind::TrackSolo: return "Solo";
    case doc::ParamKind::TrackSend: return "Send " + returnLetter(returnIndex(p, r.target));
    default: break;
  }
  const doc::TuningSlot* s = doc::findTuning(p, doc::TuningId{r.owner});
  return (s ? s->name : std::string("Tuning")) + ": " + tuningParamLabel(r.kind);
}

std::string paramShortName(const doc::Project& p, const doc::ParamRef& r) {
  switch (r.kind) {
    case doc::ParamKind::TrackVolume: return "Volume";
    case doc::ParamKind::TrackPan: return "Pan";
    case doc::ParamKind::TrackMute: return "Activator";
    case doc::ParamKind::TrackSolo: return "Solo";
    case doc::ParamKind::TrackSend: return "Send " + returnLetter(returnIndex(p, r.target));
    default: break;
  }
  const doc::TuningSlot* s = doc::findTuning(p, doc::TuningId{r.owner});
  return (s ? s->name : std::string("Tuning")) + " " + tuningParamLabel(r.kind);
}

std::string formatParamValue(const doc::ParamRef& r, double v) {
  char buf[48];
  switch (r.kind) {
    case doc::ParamKind::TrackVolume:
    case doc::ParamKind::TrackSend: return formatVolume(v) + " dB";
    case doc::ParamKind::TrackPan: return formatPan(v);
    case doc::ParamKind::TrackMute:
    case doc::ParamKind::TrackSolo: return v >= 0.5 ? "Off" : "On";
    case doc::ParamKind::TuningReference: std::snprintf(buf, sizeof buf, "%.1f Hz", v); return buf;
    default: std::snprintf(buf, sizeof buf, "%.1f \xC2\xA2", v); return buf;  // cents: U+00A2
  }
}

double currentParamValue(const doc::Project& p, const doc::ParamRef& r) noexcept {
  if (doc::isTrackParam(r.kind)) {
    const doc::Track* t = trackOf(p, r.owner);
    if (!t) return 0.0;
    switch (r.kind) {
      case doc::ParamKind::TrackVolume: return t->mixer.volumeDb;
      case doc::ParamKind::TrackPan: return t->mixer.pan;
      case doc::ParamKind::TrackMute: return t->mixer.mute ? 1.0 : 0.0;
      case doc::ParamKind::TrackSolo: return t->mixer.solo ? 1.0 : 0.0;
      case doc::ParamKind::TrackSend:
        for (const doc::Send& s : t->mixer.sends)
          if (s.target.value == r.target) return s.levelDb;
        return doc::kMinVolumeDb;
      default: return 0.0;
    }
  }
  const doc::TuningSlot* s = doc::findTuning(p, doc::TuningId{r.owner});
  if (!s) return 0.0;
  switch (r.kind) {
    case doc::ParamKind::TuningReference: return s->def.referenceHz;
    case doc::ParamKind::TuningEquave: return s->def.edo() ? s->def.edo()->equaveCents() : 1200.0;
    case doc::ParamKind::MosGenerator: return s->def.mos() ? s->def.mos()->generator : 0.0;
    case doc::ParamKind::MosPeriod: return s->def.mos() ? s->def.mos()->period : 1200.0;
    case doc::ParamKind::MosStretch: return s->def.mos() ? s->def.mos()->octaveStretch : 0.0;
    default: return 0.0;
  }
}

bool paramValid(const doc::Project& p, const doc::ParamRef& r) noexcept {
  if (!doc::isAutomatable(r.kind)) return false;
  if (doc::isTrackParam(r.kind)) {
    const doc::Track* t = trackOf(p, r.owner);
    if (!t) return false;
    if (r.kind == doc::ParamKind::TrackMute) return t->kind != doc::TrackKind::Master;
    if (r.kind == doc::ParamKind::TrackSend) {
      const doc::Track* target = trackOf(p, r.target);
      return hasSends(t->kind) && target && target->kind == doc::TrackKind::Return && target->id != t->id;
    }
    return r.target == 0;
  }
  const doc::TuningSlot* s = doc::findTuning(p, doc::TuningId{r.owner});
  return s && r.target == 0 && tuningParamFits(s->def, r.kind);
}

LaneLookup findAutomationLane(const doc::Project& p, const doc::ParamRef& r) noexcept {
  LaneLookup out;
  if (doc::isTrackParam(r.kind)) {  // a track parameter's lane lives on its own track
    if (const doc::Track* t = trackOf(p, r.owner))
      for (const doc::LanePtr& l : t->lanes)
        if (l->param == r) return {t->id, l.get()};
    return out;
  }
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    if (out.lane) return;
    for (const doc::LanePtr& l : t.lanes)
      if (l->param == r) {
        out = {t.id, l.get()};
        return;
      }
  });
  return out;
}

std::vector<ParamChoice> paramChoices(const doc::Project& p, const doc::Track& t) {
  std::vector<ParamChoice> out;
  auto add = [&](doc::ParamRef r, std::string group, std::string label) {
    const LaneLookup l = findAutomationLane(p, r);
    out.push_back({r, std::move(group), std::move(label), l.lane != nullptr, l.lane && l.owner != t.id});
  };
  add(doc::ParamRef::track(doc::ParamKind::TrackVolume, t.id), "Mixer", "Track Volume");
  add(doc::ParamRef::track(doc::ParamKind::TrackPan, t.id), "Mixer", "Track Panning");
  if (t.kind != doc::TrackKind::Master) add(doc::ParamRef::track(doc::ParamKind::TrackMute, t.id), "Mixer", "Track Activator");
  if (hasSends(t.kind))
    for (size_t i = 0; i < p.returns.size(); ++i)
      if (p.returns[i]->id != t.id)
        add(doc::ParamRef::track(doc::ParamKind::TrackSend, t.id, p.returns[i]->id), "Mixer",
            "Send " + returnLetter(static_cast<int32_t>(i)));
  if (p.tunings)
    for (const doc::TuningSlotPtr& s : *p.tunings)
      for (doc::ParamKind k : {doc::ParamKind::TuningReference, doc::ParamKind::TuningEquave, doc::ParamKind::MosGenerator,
                               doc::ParamKind::MosPeriod, doc::ParamKind::MosStretch})
        // T3.17-fix3: only a tuning with a signal (EDO, MOS) has a node a lane can drive (the model refuses others).
        if (doc::hasTuningSignal(s->def) && tuningParamFits(s->def, k)) add(doc::ParamRef::tuning(k, s->id), s->name, tuningParamLabel(k));
  return out;
}

// ---- envelope polyline -------------------------------------------------------------------------------------------

void envelopeVertices(std::span<const doc::Breakpoint> pts, double fallback, double t0, double t1, double maxChordTicks,
                      std::vector<EnvelopeVertex>& out) {
  out.clear();
  if (!(t1 >= t0)) return;
  const size_t n = pts.size();
  if (n == 0) {
    out.push_back({t0, fallback});
    out.push_back({t1, fallback});
    return;
  }
  const double chord = maxChordTicks > 0.0 ? maxChordTicks : (t1 - t0);
  // i = the first point with tick > t0.
  size_t i = static_cast<size_t>(std::upper_bound(pts.begin(), pts.end(), t0,
                                                  [](double t, const doc::Breakpoint& b) { return t < static_cast<double>(b.tick); }) -
                                 pts.begin());
  out.push_back({t0, pq::evalAutomation(pts, t0, fallback)});
  // Points of a curved segment a → b strictly between `from` and `to` (from < to inside [a.tick, b.tick]).
  auto subdivide = [&](const doc::Breakpoint& a, const doc::Breakpoint& b, double from, double to) {
    if (std::abs(pq::kAutomationCurveK * static_cast<double>(a.curve)) < pq::kAutomationLinearK || b.tick <= a.tick) return;
    const double span = to - from;
    const auto steps = static_cast<int64_t>(std::ceil(span / chord));
    if (steps <= 1) return;
    const auto cap = std::min<int64_t>(steps, 4096);  // a chord of > span/4096 is still sub-pixel at any real width
    for (int64_t s = 1; s < cap; ++s) {
      const double t = from + span * static_cast<double>(s) / static_cast<double>(cap);
      out.push_back({t, pq::automationSegmentValue(a, b, t)});
    }
  };
  double prev = t0;
  for (; i < n && static_cast<double>(pts[i].tick) <= t1; ++i) {
    const doc::Breakpoint& b = pts[i];
    const auto tb = static_cast<double>(b.tick);
    if (i > 0 && tb > prev) subdivide(pts[i - 1], b, prev, tb);
    out.push_back({tb, b.value});  // the segment ends at this point's value; a second point on the tick is the jump
    prev = tb;
  }
  if (prev < t1) {
    if (i > 0 && i < n) {
      subdivide(pts[i - 1], pts[i], prev, t1);
      out.push_back({t1, pq::automationSegmentValue(pts[i - 1], pts[i], t1)});
    } else {
      out.push_back({t1, i == 0 ? static_cast<double>(pts.front().value) : static_cast<double>(pts.back().value)});
    }
  }
}

}  // namespace pq::views
