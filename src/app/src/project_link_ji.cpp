#include "project_link_ji.h"

#include <algorithm>
#include <cmath>

#include "pq/core/automation_curve.h"

namespace pq::app {

namespace {

using Point = ClipJiLowering::Point;

inline double valueOf(const Point& p) noexcept { return p.value; }
inline double valueOf(const doc::JiPitchPoint& p) noexcept { return p.cents; }

// A piecewise-linear curve at tick t: the value from t on (at a jump the last point of the tick), and the value it
// arrives with (the first point of the tick). Held at the first / last value outside the points. `c` is not empty.
template <class P>
double valueAt(const std::vector<P>& c, Ticks t) noexcept {
  const auto it = std::upper_bound(c.begin(), c.end(), t, [](Ticks v, const P& p) noexcept { return v < p.tick; });
  if (it == c.begin()) return valueOf(c.front());
  const P& p0 = *(it - 1);
  if (it == c.end() || p0.tick == t) return valueOf(p0);
  const double x = static_cast<double>(t - p0.tick) / static_cast<double>(it->tick - p0.tick);
  return valueOf(p0) + (valueOf(*it) - valueOf(p0)) * x;
}
template <class P>
double valueBefore(const std::vector<P>& c, Ticks t) noexcept {
  const auto it = std::lower_bound(c.begin(), c.end(), t, [](const P& p, Ticks v) noexcept { return p.tick < v; });
  if (it == c.end()) return valueOf(c.back());
  if (it == c.begin() || it->tick == t) return valueOf(*it);
  const P& p0 = *(it - 1);
  const double x = static_cast<double>(t - p0.tick) / static_cast<double>(it->tick - p0.tick);
  return valueOf(p0) + (valueOf(*it) - valueOf(p0)) * x;
}

// The part of `src` (object ticks) over [a, b], a < b, as points in content ticks (origin + tick), minus `base`.
void slice(const std::vector<doc::JiPitchPoint>& src, Ticks a, Ticks b, double base, Ticks origin,
           std::vector<Point>& out) {
  out.push_back({origin + a, valueAt(src, a) - base});
  auto it = std::upper_bound(src.begin(), src.end(), a,
                             [](Ticks v, const doc::JiPitchPoint& p) noexcept { return v < p.tick; });
  for (; it != src.end() && it->tick < b; ++it) out.push_back({origin + it->tick, it->cents - base});
  out.push_back({origin + b, valueBefore(src, b) - base});
}

// Drops the points of curved spans that follow the last kept one by less than `spacing`. Corners stay: the first and
// last point, both points of a jump, the ends of a flat stretch.
void thin(std::vector<Point>& p, Ticks spacing) {
  const size_t n = p.size();
  if (n < 3 || spacing <= 1) return;
  size_t kept = 1;
  for (size_t i = 1; i < n; ++i) {
    const bool corner = i + 1 == n || p[i].tick == p[i - 1].tick || p[i].tick == p[i + 1].tick ||
                        p[i].value == p[i - 1].value || p[i].value == p[i + 1].value;
    if (corner || p[i].tick - p[kept - 1].tick >= spacing) p[kept++] = p[i];
  }
  p.resize(kept);
}

// a + b (both piecewise linear, held outside their points) → out.
void add(const std::vector<Point>& a, const std::vector<Point>& b, std::vector<Point>& out) {
  out.clear();
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    const Ticks t = j >= b.size() || (i < a.size() && a[i].tick <= b[j].tick) ? a[i].tick : b[j].tick;
    while (i < a.size() && a[i].tick == t) ++i;
    while (j < b.size() && b[j].tick == t) ++j;
    const double before = valueBefore(a, t) + valueBefore(b, t), at = valueAt(a, t) + valueAt(b, t);
    out.push_back({t, before});
    if (at != before) out.push_back({t, at});
  }
}

// A note's own curve (the automation curve law between breakpoints, ticks from the note's start) as lines within
// kJiPitchTolerance, in content ticks.
void flattenOwn(const std::vector<doc::Breakpoint>& pts, Ticks origin, std::vector<Point>& out) {
  out.clear();
  for (size_t i = 0; i < pts.size(); ++i) {
    const doc::Breakpoint& p = pts[i];
    out.push_back({origin + p.tick, static_cast<double>(p.value)});
    if (i + 1 == pts.size()) break;
    const doc::Breakpoint& q = pts[i + 1];
    const double k = kAutomationCurveK * static_cast<double>(p.curve);
    const double delta = std::fabs(static_cast<double>(q.value) - static_cast<double>(p.value));
    if (q.tick <= p.tick + 1 || std::fabs(k) < kAutomationLinearK || delta == 0.0) continue;
    // |s''| ≤ k² · max(1, e^k) / |e^k − 1|; a chord of n equal steps is within delta · |s''| / (8 n²).
    const double bend = k * k * std::max(1.0, std::exp(k)) / std::fabs(std::expm1(k));
    const double want = std::ceil(std::sqrt(delta * bend / (8.0 * kJiPitchTolerance)));
    const auto n = static_cast<Ticks>(std::clamp(want, 1.0, static_cast<double>(std::min<Ticks>(q.tick - p.tick, 512))));
    for (Ticks s = 1; s < n; ++s) {
      const Ticks t = p.tick + (q.tick - p.tick) * s / n;
      if (origin + t <= out.back().tick) continue;
      out.push_back({origin + t, automationSegmentValue(p, q, static_cast<double>(t))});
    }
  }
}

}  // namespace

ClipJiLowering::ClipJiLowering(const doc::NoteClip& clip, const tuning::Tuning* tuning, const TempoMap& tempo,
                               bool tempoMoves, const JiLowerLimits& limits)
    : clip_(clip), tuning_(tuning), tempo_(tempo), tempoMoves_(tempoMoves), limits_(limits) {
  const std::span<const doc::JiObject> objects = doc::jiObjectsOf(clip);
  plans_.resize(objects.size());
  if (objects.empty()) return;
  // Which objects a note follows: their plans are kept for noteCurve(); the others' are freed once their voices are
  // made (the next object's plan then reuses the warm memory instead of touching new pages).
  for (const doc::Note& n : clip.notes) {
    const doc::JiObjectId id = doc::jiObjectOf(n.pitch);
    if (!id.valid()) continue;
    if (const doc::JiObject* o = doc::findJiObject(clip, id)) plans_[static_cast<size_t>(o - objects.data())].followed = true;
  }
}

double ClipJiLowering::bpmAt(Ticks contentTick) const noexcept {
  return tempo_.bpmAt(std::max<Ticks>(clip_.start + (contentTick - clip_.contentStart), 0));
}

Ticks ClipJiLowering::ticksOf(double seconds, Ticks contentTick) const noexcept {
  return std::max<Ticks>(1, std::llround(seconds * bpmAt(contentTick) / 60.0 * static_cast<double>(kPpq)));
}

ClipJiLowering::ObjectPlan& ClipJiLowering::planOf(const doc::JiObject& o) {
  ObjectPlan& p = plans_[static_cast<size_t>(&o - doc::jiObjectsOf(clip_).data())];
  if (p.tried) return p;
  p.tried = true;
  p.gainRamp = ticksOf(kJiGainRampSeconds, o.start);
  // All the voices together: about a quarter of the track's event list per block.
  const double perVoice = static_cast<double>(o.voices()) * limits_.blockSeconds * 4.0 /
                          static_cast<double>(std::max<uint32_t>(limits_.eventsPerBlock, 1));
  p.spacing = ticksOf(std::max(kJiMinSpacingSeconds, perVoice), o.start);
  if (tuning_ != nullptr) {
    doc::JiPlanOptions po;
    po.toleranceCents = kJiPitchTolerance;
    po.gainRamp = p.gainRamp;
    p.plan = doc::jiVoicePlan(o, *tuning_, po);
  }
  return p;
}

// The fundamental moves as one voice on the term 1 of every stop (its scale, the stop's default timing), ride
// included: ji_object.h jiFundamentalCents, as a plan.
const std::vector<doc::JiPitchPoint>& ClipJiLowering::fundamentalOf(const doc::JiObject& o, ObjectPlan& p) {
  if (p.fundamentalTried || tuning_ == nullptr) return p.fundamental;
  p.fundamentalTried = true;
  doc::JiObject f;
  f.id = o.id;
  f.start = o.start;
  f.length = o.length;
  f.fundamental = o.fundamental;
  f.ride = o.ride;
  f.voiceNotes = {doc::NoteId{1}};
  f.stops.reserve(o.stops.size());
  for (const doc::JiStop& s : o.stops) {
    doc::JiStop x;
    x.at = s.at;
    x.terms = {1};
    x.scale = s.scale;
    x.timing = s.timing;
    f.stops.push_back(std::move(x));
  }
  doc::JiPlanOptions po;
  po.toleranceCents = kJiPitchTolerance;
  if (std::optional<doc::JiPlan> plan = doc::jiVoicePlan(f, *tuning_, po)) p.fundamental = std::move(plan->voices[0].pitch);
  return p.fundamental;
}

void ClipJiLowering::emit(const std::vector<Point>& pts, std::vector<engine::CurvePoint>& out) const {
  out.clear();
  bool any = false;
  for (const Point& p : pts) any |= std::fabs(p.value) > 1e-9;
  if (!any) return;  // the note's own pitch throughout
  const double lim = static_cast<double>(engine::kMaxCurveCents);
  const Ticks cut = kPpq / 4;
  out.reserve(pts.size());
  for (size_t i = 0; i < pts.size(); ++i) {
    if (tempoMoves_ && i > 0 && pts[i].tick - pts[i - 1].tick > cut && pts[i].value != pts[i - 1].value) {
      // Inside a tempo ramp a line over ticks is not a line over samples: shorter pieces.
      const Point a = pts[i - 1], b = pts[i];
      const Ticks n = (b.tick - a.tick + cut - 1) / cut;
      for (Ticks s = 1; s < n; ++s) {
        const Ticks t = a.tick + (b.tick - a.tick) * s / n;
        const double v = a.value + (b.value - a.value) * static_cast<double>(t - a.tick) / static_cast<double>(b.tick - a.tick);
        out.push_back({t, static_cast<float>(std::clamp(v, -lim, lim))});
      }
    }
    out.push_back({pts[i].tick, static_cast<float>(std::clamp(pts[i].value, -lim, lim))});
  }
}

// A voice's plan → its curve in one pass (emit(thin(…)) for the common case of a constant tempo): relative to the
// plan's first point, breakpoints of curved spans closer than `spacing` dropped (corners kept, as thin()).
void ClipJiLowering::voiceCurve(const std::vector<doc::JiPitchPoint>& p, Ticks origin, Ticks spacing,
                                std::vector<engine::CurvePoint>& out) const {
  const size_t n = p.size();
  const double base = p.front().cents;
  bool any = false;
  for (size_t i = 1; i < n && !any; ++i) any = std::fabs(p[i].cents - base) > 1e-9;
  if (!any) return;
  const double lim = static_cast<double>(engine::kMaxCurveCents);
  out.resize(n);
  engine::CurvePoint* w = out.data();
  *w++ = {origin + p[0].tick, 0.0f};
  Ticks last = p[0].tick;
  for (size_t i = 1; i < n; ++i) {
    const bool corner = i + 1 == n || p[i].tick == p[i - 1].tick || p[i].tick == p[i + 1].tick ||
                        p[i].cents == p[i - 1].cents || p[i].cents == p[i + 1].cents;
    if (!corner && p[i].tick - last < spacing) continue;
    last = p[i].tick;
    *w++ = {origin + p[i].tick, static_cast<float>(std::clamp(p[i].cents - base, -lim, lim))};
  }
  out.resize(static_cast<size_t>(w - out.data()));
}

void ClipJiLowering::noteCurve(const doc::Note& n, engine::NoteDesc& nd) {
  a_.clear();
  const doc::JiObjectId object = doc::jiObjectOf(n.pitch);
  if (object.valid() && tuning_ != nullptr) {
    if (const doc::JiObject* o = doc::findJiObject(clip_, object)) {
      ObjectPlan& op = planOf(*o);
      const double at = static_cast<double>(n.start - o->start);
      const std::vector<doc::JiPitchPoint>* src = nullptr;
      std::optional<double> base;
      if (n.pitch.kind() == tuning::PitchKind::Harmonic) {
        const uint32_t v = n.pitch.voiceIndex();
        if (op.plan && v < op.plan->voices.size()) {
          src = &op.plan->voices[v].pitch;
          base = doc::jiVoiceCents(*o, *tuning_, v, at);
        }
      } else {
        src = &fundamentalOf(*o, op);
        base = doc::jiFundamentalCents(*o, *tuning_, at);
      }
      const Ticks a = std::clamp<Ticks>(n.start - o->start, 0, o->length);
      const Ticks b = std::clamp<Ticks>(n.start + std::max<Ticks>(n.length, 1) - o->start, 0, o->length);
      if (src != nullptr && !src->empty() && base && a < b) {
        slice(*src, a, b, *base, o->start, a_);
        thin(a_, op.spacing);
      }
    }
  }
  if (const doc::NoteCurves* own = doc::findNoteCurves(clip_, n.id); own != nullptr && !own->pitch.empty()) {
    flattenOwn(own->pitch, n.start, b_);
    thin(b_, ticksOf(kJiMinSpacingSeconds, n.start));
    if (a_.empty()) {
      a_.swap(b_);
    } else {
      add(a_, b_, c_);
      a_.swap(c_);
    }
  }
  if (!a_.empty()) emit(a_, nd.pitchCurve);
}

uint32_t ClipJiLowering::voices(const doc::JiObject& o, std::vector<engine::NoteDesc>& out) {
  if (!o.sounds || tuning_ == nullptr) return 0;
  ObjectPlan& op = planOf(o);
  if (!op.plan) return 0;
  const doc::JiPlan& plan = *op.plan;
  const uint32_t V = static_cast<uint32_t>(plan.voices.size());
  const doc::JiStop& stop0 = o.stops.front();
  uint32_t made = 0;
  bool previous = false;  // voice v − 1 was made (it is out.back())
  for (uint32_t v = 0; v < V; ++v) {
    const doc::JiVoicePlan& vp = plan.voices[v];
    const std::optional<double> hz = doc::jiVoiceHz(o, *tuning_, v, 0.0);
    if (!hz || !(*hz > 0.0) || !std::isfinite(static_cast<float>(*hz)) || static_cast<float>(*hz) <= 0.0f) {
      previous = false;
      continue;
    }
    engine::NoteDesc nd;
    nd.start = o.start;
    nd.length = std::max<Ticks>(o.length, 1);
    nd.id = o.voiceNotes[v].value;
    nd.hz = static_cast<float>(*hz);
    nd.velocity = std::clamp(o.velocity, 1.0f, 127.0f);
    if (const std::optional<tuning::Rational> r = doc::jiVoiceRatio(o, v, 0)) nd.ratio = static_cast<float>(r->toDouble());
    if (!stop0.isTuning()) nd.harmonic = static_cast<float>(stop0.terms[doc::jiVoiceEntry(o, stop0, v)]);
    const doc::JiUnison& un = vp.unison.front();
    nd.unisonCount = un.size;
    uint16_t index = 0;
    for (uint32_t u = 0; u < v; ++u) index += plan.voices[u].unison.front().leader == un.leader ? 1 : 0;
    nd.unisonIndex = index;
    if (previous && vp.pitch.size() == plan.voices[v - 1].pitch.size() && vp.pitch == plan.voices[v - 1].pitch) {
      nd.pitchCurve = out.back().pitchCurve;  // a unison from the start: the identical curve
    } else if (!tempoMoves_) {
      voiceCurve(vp.pitch, o.start, op.spacing, nd.pitchCurve);
    } else {
      a_.clear();
      a_.reserve(vp.pitch.size());
      const double base = vp.pitch.front().cents;
      for (const doc::JiPitchPoint& p : vp.pitch) a_.push_back({o.start + p.tick, p.cents - base});
      thin(a_, op.spacing);
      emit(a_, nd.pitchCurve);
    }
    bool moves = false;
    for (const doc::JiGainPoint& g : vp.gain) moves |= g.gain != 1.0f;
    if (moves) {
      nd.gainCurve.reserve(vp.gain.size());
      for (const doc::JiGainPoint& g : vp.gain) nd.gainCurve.push_back({o.start + g.tick, std::max(g.gain, 0.0f)});
    }
    out.push_back(std::move(nd));
    previous = true;
    ++made;
  }
  if (!op.followed) {  // no note needs it again
    op.plan.reset();
    op.tried = false;
  }
  return made;
}

}  // namespace pq::app
