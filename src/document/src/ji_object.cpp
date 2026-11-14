// JI object evaluation (T7.2): see pq/document/ji_object.h for the model.

#include "pq/document/ji_object.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "pq/document/automation_curve.h"

namespace pq::doc {

namespace {

// The timing of a voice's transition into a stop; voice < 0: the stop's own (the fundamental's).
const JiTiming& timingOf(const JiStop& s, int32_t voice) noexcept {
  if (voice >= 0 && static_cast<size_t>(voice) < s.voiceTiming.size() && s.voiceTiming[static_cast<size_t>(voice)])
    return *s.voiceTiming[static_cast<size_t>(voice)];
  return s.timing;
}

// arrive_s / begin_s from the previous arrival (ji_object.h "timing").
struct Window {
  Ticks begin, arrive;
};
Window windowOf(const JiStop& s, const JiTiming& tm, Ticks prevArrive) noexcept {
  const Ticks arrive = std::max(s.at + tm.delay, prevArrive);
  return {std::max(arrive - tm.glide, prevArrive), arrive};
}

// Where a voice is at a tick: holding stop `cur`, or `u` of the way (in time) into stop `next`.
struct Where {
  uint32_t cur = 0;
  int32_t next = -1;
  double u = 0.0;
};
// `before`: the left limit at `tick` (a stop arriving exactly at `tick` has not arrived yet).
Where locate(const JiObject& o, int32_t voice, double tick, bool before) noexcept {
  Where w;
  Ticks prev = 0;
  for (uint32_t s = 1; s < o.stops.size(); ++s) {
    const JiStop& st = o.stops[s];
    const Window win = windowOf(st, timingOf(st, voice), prev);
    const double arrive = static_cast<double>(win.arrive);
    if (before ? arrive < tick : arrive <= tick) {
      w.cur = s;
      prev = win.arrive;
      continue;
    }
    if (static_cast<double>(win.begin) < tick) {  // gliding into s (arrive > begin here)
      w.next = static_cast<int32_t>(s);
      w.u = (tick - static_cast<double>(win.begin)) / static_cast<double>(win.arrive - win.begin);
    }
    break;  // later stops begin at or after this arrival
  }
  return w;
}

double log2Ratio(const tuning::Rational& r) noexcept {
  return std::log2(static_cast<double>(r.num())) - std::log2(static_cast<double>(r.den()));
}

// Cents of entry `e` of stop `s`; `fund` = the fundamental's cents (only read for a proportion stop).
std::optional<double> entryCents(const JiObject& o, const tuning::Tuning& t, const JiStop& s, uint32_t e,
                                 const std::optional<double>& fund) noexcept {
  if (s.isTuning()) return jiPitchCents(s.pitches[e], t);
  if (!fund) return std::nullopt;
  const double term = std::log2(static_cast<double>(s.terms[e]));
  return *fund + 1200.0 * (log2Ratio(s.scale) + (o.undertone ? -term : term));
}

double rideAt(const JiObject& o, double tick) noexcept {
  return o.ride.empty() ? 0.0 : pq::evalAutomation(std::span<const Breakpoint>(o.ride), tick, 0.0);
}

double clampTick(const JiObject& o, double tick) noexcept {
  return std::clamp(tick, 0.0, static_cast<double>(o.length));
}

std::optional<double> hzOfCents(const tuning::Tuning& t, const std::optional<double>& cents) noexcept {
  if (!cents) return std::nullopt;
  return t.referenceHz() * std::exp2(*cents * (1.0 / 1200.0));
}

}  // namespace

uint32_t jiVoiceEntry(const JiObject& o, const JiStop& s, uint32_t voice) noexcept {
  const uint32_t n = s.size();
  if (n == 0) return 0;
  const uint32_t V = std::max<uint32_t>(o.voices(), 1);
  const uint32_t e = voice < s.voiceTerm.size() ? s.voiceTerm[voice] : static_cast<uint32_t>(uint64_t{voice} * n / V);
  return std::min(e, n - 1);
}

std::optional<tuning::Rational> jiVoiceRatio(const JiObject& o, uint32_t voice, uint32_t stop) noexcept {
  if (voice >= o.voices() || stop >= o.stops.size()) return std::nullopt;
  const JiStop& s = o.stops[stop];
  if (s.isTuning() || s.terms.empty()) return std::nullopt;
  const tuning::Rational term(s.terms[jiVoiceEntry(o, s, voice)]);
  return o.undertone ? tuning::div(s.scale, term) : tuning::mul(s.scale, term);
}

double jiShapeValue(const JiShape& shape, double u) noexcept {
  u = std::clamp(u, 0.0, 1.0);
  switch (shape.kind) {
    case JiShape::LogLinear: return u;
    case JiShape::SCurve: return u * u * (3.0 - 2.0 * u);
    case JiShape::Bezier: {
      // x(p) = 3(1−p)²p·x1 + 3(1−p)p²·x2 + p³ is monotonic for x1, x2 ∈ [0, 1]: solve x(p) = u (Newton, then
      // bisection where the slope is too flat), then y(p).
      const double x1 = shape.x1, x2 = shape.x2, y1 = shape.y1, y2 = shape.y2;
      const double cx = 3.0 * x1, bx = 3.0 * (x2 - x1) - cx, ax = 1.0 - cx - bx;
      const double cy = 3.0 * y1, by = 3.0 * (y2 - y1) - cy, ay = 1.0 - cy - by;
      auto x = [&](double p) { return ((ax * p + bx) * p + cx) * p; };
      double p = u;
      bool solved = false;
      for (int i = 0; i < 8; ++i) {
        const double err = x(p) - u;
        if (std::abs(err) < 1e-12) {
          solved = true;
          break;
        }
        const double d = (3.0 * ax * p + 2.0 * bx) * p + cx;
        if (std::abs(d) < 1e-9) break;
        p -= err / d;
      }
      if (!solved || !(p >= 0.0 && p <= 1.0)) {
        double lo = 0.0, hi = 1.0;
        p = u;
        for (int i = 0; i < 60; ++i) {
          const double v = x(p);
          if (std::abs(v - u) < 1e-13) break;
          if (v < u) lo = p;
          else hi = p;
          p = 0.5 * (lo + hi);
        }
      }
      return ((ay * p + by) * p + cy) * p;
    }
  }
  return u;
}

JiPosition jiPosition(const JiObject& o, int32_t voice, double tick) noexcept {
  const Where w = locate(o, voice, clampTick(o, tick), false);
  return {w.cur, w.next, w.u};
}

JiTransition jiTransition(const JiObject& o, uint32_t voice, uint32_t stop) noexcept {
  JiTransition out;
  Ticks prev = 0;
  for (uint32_t s = 1; s <= stop && s < o.stops.size(); ++s) {
    const JiStop& st = o.stops[s];
    const JiTiming& tm = timingOf(st, static_cast<int32_t>(voice));
    const Window w = windowOf(st, tm, prev);
    prev = w.arrive;
    if (s == stop) out = {w.begin, w.arrive, tm.shape};
  }
  return out;
}

std::optional<double> jiPitchCents(const tuning::PitchId& p, const tuning::Tuning& t) noexcept {
  switch (p.kind()) {
    case tuning::PitchKind::Cents: return std::isfinite(p.cents()) ? std::optional<double>(p.cents()) : std::nullopt;
    case tuning::PitchKind::Ratio:
      if (p.anchorKind() != tuning::AnchorKind::Reference || !(p.rawNum() > 0 && p.rawDen() > 0)) return std::nullopt;
      return 1200.0 * (std::log2(static_cast<double>(p.rawNum())) - std::log2(static_cast<double>(p.rawDen())));
    case tuning::PitchKind::Degree: {
      const std::optional<double> hz = tuning::resolveHzDouble(p, t, {});
      if (!hz) return std::nullopt;
      return 1200.0 * std::log2(*hz / t.referenceHz());
    }
    case tuning::PitchKind::Harmonic: break;
  }
  return std::nullopt;
}

std::optional<double> jiVoiceCents(const JiObject& o, const tuning::Tuning& t, uint32_t voice, double tick) noexcept {
  if (voice >= o.voices() || o.stops.empty()) return std::nullopt;
  tick = clampTick(o, tick);
  const Where w = locate(o, static_cast<int32_t>(voice), tick, false);
  const std::optional<double> fund = jiPitchCents(o.fundamental, t);
  const JiStop& a = o.stops[w.cur];
  std::optional<double> c = entryCents(o, t, a, jiVoiceEntry(o, a, voice), fund);
  if (!c) return std::nullopt;
  if (w.next >= 0) {
    const JiStop& b = o.stops[static_cast<size_t>(w.next)];
    const std::optional<double> cb = entryCents(o, t, b, jiVoiceEntry(o, b, voice), fund);
    if (!cb) return std::nullopt;
    *c += (*cb - *c) * jiShapeValue(timingOf(b, static_cast<int32_t>(voice)).shape, w.u);
  }
  return *c + rideAt(o, tick);
}

std::optional<double> jiVoiceHz(const JiObject& o, const tuning::Tuning& t, uint32_t voice, double tick) noexcept {
  if (voice >= o.voices() || o.stops.empty()) return std::nullopt;
  tick = clampTick(o, tick);
  const Where w = locate(o, static_cast<int32_t>(voice), tick, false);
  if (w.next < 0 && rideAt(o, tick) == 0.0) {  // a hold: the stop's own pitch / the exact ratio × fundamental
    const JiStop& st = o.stops[w.cur];
    if (st.isTuning()) return tuning::resolveHzDouble(st.pitches[jiVoiceEntry(o, st, voice)], t, {});
    const std::optional<double> fund = tuning::resolveHzDouble(o.fundamental, t, {});
    const std::optional<tuning::Rational> r = jiVoiceRatio(o, voice, w.cur);
    if (!fund || !r) return std::nullopt;
    return *fund * r->toDouble();
  }
  return hzOfCents(t, jiVoiceCents(o, t, voice, tick));
}

std::optional<double> jiFundamentalCents(const JiObject& o, const tuning::Tuning& t, double tick) noexcept {
  if (o.stops.empty()) return std::nullopt;
  const std::optional<double> fund = jiPitchCents(o.fundamental, t);
  if (!fund) return std::nullopt;
  tick = clampTick(o, tick);
  const Where w = locate(o, -1, tick, false);
  double c = 1200.0 * log2Ratio(o.stops[w.cur].scale);
  if (w.next >= 0) {
    const JiStop& b = o.stops[static_cast<size_t>(w.next)];
    c += (1200.0 * log2Ratio(b.scale) - c) * jiShapeValue(b.timing.shape, w.u);
  }
  return *fund + c + rideAt(o, tick);
}

std::optional<double> jiFundamentalHz(const JiObject& o, const tuning::Tuning& t, double tick) noexcept {
  if (o.stops.empty()) return std::nullopt;
  tick = clampTick(o, tick);
  const Where w = locate(o, -1, tick, false);
  if (w.next < 0 && rideAt(o, tick) == 0.0) {
    const std::optional<double> fund = tuning::resolveHzDouble(o.fundamental, t, {});
    if (!fund) return std::nullopt;
    return *fund * o.stops[w.cur].scale.toDouble();
  }
  return hzOfCents(t, jiFundamentalCents(o, t, tick));
}

// ---- The plan ----------------------------------------------------------------------------------------------------

namespace {

// One voice's stops, precomputed: O(log stops) per evaluation instead of the single queries' scan.
struct VoiceTrack {
  std::vector<Ticks> begin, arrive;   // per stop
  std::vector<double> cents;          // per stop: the voice's pitch there (no ride)
  std::vector<JiShape> shape;         // per stop: the transition into it

  bool sameAs(const VoiceTrack& b) const noexcept {
    return begin == b.begin && arrive == b.arrive && cents == b.cents && shape == b.shape;
  }
  // The morph at t (`before`: the left limit).
  double at(double t, bool before) const noexcept {
    const auto it = before ? std::lower_bound(arrive.begin(), arrive.end(), t, [](Ticks a, double v) { return static_cast<double>(a) < v; })
                           : std::upper_bound(arrive.begin(), arrive.end(), t, [](double v, Ticks a) { return v < static_cast<double>(a); });
    const size_t cur = it == arrive.begin() ? 0 : static_cast<size_t>(it - arrive.begin()) - 1;
    const size_t next = cur + 1;
    if (next < arrive.size() && static_cast<double>(begin[next]) < t && arrive[next] > begin[next]) {
      const double u = (t - static_cast<double>(begin[next])) / static_cast<double>(arrive[next] - begin[next]);
      if (u >= 1.0) return cents[next];
      return cents[cur] + (cents[next] - cents[cur]) * jiShapeValue(shape[next], u);
    }
    return cents[cur];
  }
};

struct Flattener {
  const JiObject& o;
  const VoiceTrack& tr;
  double tolerance;
  std::vector<JiPitchPoint>& out;

  double inside(double t) const noexcept { return tr.at(t, false) + rideAt(o, t); }
  double from(Ticks t) const noexcept { return tr.at(static_cast<double>(t), false) + automationValueAt(o.ride, t, 0.0); }
  double upTo(Ticks t) const noexcept { return tr.at(static_cast<double>(t), true) + automationValueBefore(o.ride, t, 0.0); }

  void push(Ticks tick, double cents) {
    const size_t n = out.size();
    if (n >= 2 && out[n - 1].cents == cents && out[n - 2].cents == cents && out[n - 1].tick > out[n - 2].tick) {
      out[n - 1].tick = tick;  // extend a flat run instead of adding a point
      return;
    }
    out.push_back({tick, cents});
  }
  // Adds the interior points of the smooth span (a, b) so that the polyline stays within the tolerance.
  void span(Ticks a, double fa, Ticks b, double fb) {
    if (b - a <= 1) return;
    double worst = 0.0;
    const Ticks len = b - a;
    Ticks last = a;
    for (int k = 1; k < 8; ++k) {
      const Ticks x = a + len * k / 8;
      if (x <= last || x >= b) continue;
      last = x;
      const double line = fa + (fb - fa) * (static_cast<double>(x - a) / static_cast<double>(len));
      worst = std::max(worst, std::abs(inside(static_cast<double>(x)) - line));
    }
    if (worst <= 0.5 * tolerance) return;  // 7 samples see ≥ 94 % of a smooth bulge: half the tolerance is safe
    const Ticks m = a + len / 2;
    const double fm = inside(static_cast<double>(m));
    span(a, fa, m, fm);
    push(m, fm);
    span(m, fm, b, fb);
  }
};

}  // namespace

std::optional<JiPlan> jiVoicePlan(const JiObject& o, const tuning::Tuning& t, const JiPlanOptions& options) {
  const uint32_t V = o.voices();
  const size_t S = o.stops.size();
  if (V == 0 || V > kMaxJiVoices || S == 0 || o.length < 1) return std::nullopt;
  const double tolerance = std::max(options.toleranceCents, 1e-6);
  const Ticks gainRamp = std::max<Ticks>(options.gainRamp, 0);
  const Ticks length = o.length;

  // Each stop entry's cents once (voices sharing an entry share the value).
  const std::optional<double> fund = jiPitchCents(o.fundamental, t);
  std::vector<uint32_t> entryBase(S + 1, 0);
  for (size_t s = 0; s < S; ++s) entryBase[s + 1] = entryBase[s] + o.stops[s].size();
  std::vector<double> entry(entryBase[S]);
  for (size_t s = 0; s < S; ++s)
    for (uint32_t e = 0; e < o.stops[s].size(); ++e) {
      const std::optional<double> c = entryCents(o, t, o.stops[s], e, fund);
      if (!c) return std::nullopt;
      entry[entryBase[s] + e] = *c;
    }

  std::vector<VoiceTrack> tracks(V);
  std::vector<uint8_t> entryOf(S * V);  // [s · V + v]
  for (uint32_t v = 0; v < V; ++v) {
    VoiceTrack& tr = tracks[v];
    tr.begin.resize(S);
    tr.arrive.resize(S);
    tr.cents.resize(S);
    tr.shape.resize(S);
    Ticks prev = 0;
    for (size_t s = 0; s < S; ++s) {
      const JiStop& st = o.stops[s];
      const uint32_t e = jiVoiceEntry(o, st, v);
      entryOf[s * V + v] = static_cast<uint8_t>(e);
      tr.cents[s] = entry[entryBase[s] + e];
      if (s == 0) {
        tr.begin[0] = tr.arrive[0] = 0;
        continue;
      }
      const JiTiming& tm = timingOf(st, static_cast<int32_t>(v));
      const Window w = windowOf(st, tm, prev);
      tr.begin[s] = w.begin;
      tr.arrive[s] = w.arrive;
      tr.shape[s] = tm.shape;
      prev = w.arrive;
    }
  }

  JiPlan plan;
  plan.voices.resize(V);

  // ---- Pitch: flatten each voice between its knots (stop windows and ride points). ----
  std::vector<Ticks> rideKnots;
  for (const Breakpoint& b : o.ride)
    if (b.tick > 0 && b.tick < length) rideKnots.push_back(b.tick);
  std::vector<Ticks> knots;
  for (uint32_t v = 0; v < V; ++v) {
    std::vector<JiPitchPoint>& out = plan.voices[v].pitch;
    if (v > 0 && tracks[v].sameAs(tracks[v - 1])) {  // a unison from the start: the identical curve
      out = plan.voices[v - 1].pitch;
      continue;
    }
    const VoiceTrack& tr = tracks[v];
    knots.assign(rideKnots.begin(), rideKnots.end());
    knots.push_back(0);
    knots.push_back(length);
    for (size_t s = 1; s < S && tr.begin[s] < length; ++s) {
      if (tr.begin[s] > 0) knots.push_back(tr.begin[s]);
      if (tr.arrive[s] > 0 && tr.arrive[s] < length) knots.push_back(tr.arrive[s]);
    }
    std::sort(knots.begin(), knots.end());
    knots.erase(std::unique(knots.begin(), knots.end()), knots.end());
    Flattener f{o, tr, tolerance, out};
    double fa = f.from(0);
    out.push_back({0, fa});
    for (size_t i = 0; i + 1 < knots.size(); ++i) {
      const Ticks a = knots[i], b = knots[i + 1];
      const double fb = f.upTo(b);
      f.span(a, fa, b, fb);
      f.push(b, fb);
      if (i + 2 < knots.size()) {
        fa = f.from(b);
        if (std::abs(fa - fb) > 1e-9) out.push_back({b, fa});  // a jump
        else fa = fb;
      }
    }
    if (out.size() < 2) out.push_back({length, out.back().cents});
  }

  // ---- Unison classes and gains, stop by stop. ----
  std::array<uint8_t, kMaxJiVoices> coh{}, nextCoh{}, leader{};
  std::array<float, kMaxJiVoices> gain{};
  for (size_t s = 0; s < S; ++s) {
    const JiStop& st = o.stops[s];
    const uint8_t* e = &entryOf[s * V];
    auto samePitch = [&](uint32_t a, uint32_t b) {
      return e[a] == e[b] || (st.isTuning() && st.pitches[e[a]] == st.pitches[e[b]]);
    };
    auto sameTiming = [&](uint32_t a, uint32_t b) {
      const VoiceTrack &x = tracks[a], &y = tracks[b];
      return x.begin[s] == y.begin[s] && x.arrive[s] == y.arrive[s] &&
             (x.arrive[s] == x.begin[s] || x.shape[s] == y.shape[s]);
    };
    for (uint32_t v = 0; v < V; ++v) {
      // The class leader: the first voice on the pitch. The lock group's representative: the first voice v has moved
      // identically to since tick 0.
      uint32_t l = v, c = v;
      for (uint32_t u = 0; u < v; ++u)
        if (samePitch(u, v)) {
          l = u;
          break;
        }
      for (uint32_t u = l; u < v; ++u)
        if (samePitch(u, v) && (s == 0 || (coh[u] == coh[v] && sameTiming(u, v)))) {
          c = u;
          break;
        }
      nextCoh[v] = static_cast<uint8_t>(c);
      leader[v] = static_cast<uint8_t>(l);
    }
    coh = nextCoh;
    bool anyReached = false;
    for (uint32_t v = 0; v < V; ++v) {
      const VoiceTrack& tr = tracks[v];
      uint32_t size = 0, lockedSize = 0;
      for (uint32_t u = 0; u < V; ++u)
        if (leader[u] == leader[v]) {
          ++size;
          if (coh[u] == coh[leader[v]]) ++lockedSize;
        }
      const bool locked = coh[v] == coh[leader[v]];
      const float g = locked ? 1.0f / static_cast<float>(lockedSize) : 0.0f;
      JiVoicePlan& vp = plan.voices[v];
      if (s == 0) {
        vp.gain.push_back({0, g});
        vp.unison.push_back({0, leader[v], static_cast<uint8_t>(size), static_cast<uint8_t>(lockedSize), locked});
        gain[v] = g;
        anyReached = true;
        continue;
      }
      if (tr.begin[s] >= length) continue;  // never reached
      anyReached = true;
      if (tr.arrive[s] < length) {
        const JiUnison un{tr.arrive[s], leader[v], static_cast<uint8_t>(size), static_cast<uint8_t>(lockedSize), locked};
        const JiUnison& last = vp.unison.back();
        if (last.leader != un.leader || last.size != un.size || last.lockedSize != un.lockedSize || last.locked != un.locked) {
          if (last.from == un.from) vp.unison.back() = un;
          else vp.unison.push_back(un);
        }
      }
      if (g == gain[v]) continue;
      const Ticks ramp = std::min(tr.arrive[s] - tr.begin[s], gainRamp);
      Ticks t0, t1;
      if (g > gain[v]) {  // a split: in from the start of the transition
        t0 = tr.begin[s];
        t1 = t0 + ramp;
      } else {  // a merge: out once both it and the class leader are there (not past its next move)
        t1 = std::max(tr.arrive[s], tracks[leader[v]].arrive[s]);
        if (s + 1 < S) t1 = std::max(tr.arrive[s], std::min(t1, tr.begin[s + 1]));
        t0 = std::max(t1 - ramp, tr.begin[s]);
      }
      if (!(vp.gain.back() == JiGainPoint{t0, gain[v]})) vp.gain.push_back({t0, gain[v]});
      vp.gain.push_back({t1, g});
      gain[v] = g;
    }
    if (!anyReached) break;  // begins only grow: no later stop is reached either
  }
  for (JiVoicePlan& vp : plan.voices) {  // clip to [0, length], closing at length
    std::vector<JiGainPoint>& g = vp.gain;
    for (size_t i = 1; i < g.size(); ++i) {
      if (g[i].tick <= length) continue;
      const JiGainPoint a = g[i - 1], b = g[i];
      const float x = static_cast<float>(static_cast<double>(length - a.tick) / static_cast<double>(b.tick - a.tick));
      g.resize(i);
      g.push_back({length, a.gain + (b.gain - a.gain) * x});
      break;
    }
    if (g.back().tick < length || g.size() < 2) g.push_back({length, g.back().gain});
  }
  return plan;
}

}  // namespace pq::doc
