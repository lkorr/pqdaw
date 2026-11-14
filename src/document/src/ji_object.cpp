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
  // T7.3: without a ride, a span between two knots is one hold or one piece of one transition, known in closed form:
  // a hold and a LogLinear transition are a line (no point, no evaluation). An S-curve s(u) = 3u² − 2u³ has
  // |s''(u)| = 6 |1 − 2u|, largest at its ends and 0 in the middle, so its breakpoints are marched from u = 0 with
  // steps h that grow as the curvature falls — a chord over [u, u + h] is within delta · 6 (1 − 2u) · h² / 8 as long as
  // it ends before the mirror point 1 − u — and mirrored for the second half: about a third fewer points than equal
  // steps at the same tolerance (fewer events for the engine). False: not such a span (a ride, a Bezier), the caller
  // subdivides adaptively.
  std::vector<double> us;  // scratch: the first half's breakpoints
  bool closedForm(Ticks a, Ticks b) {
    if (!o.ride.empty()) return false;
    const size_t next = static_cast<size_t>(std::upper_bound(tr.arrive.begin(), tr.arrive.end(), a) - tr.arrive.begin());
    if (next >= tr.arrive.size() || tr.begin[next] > a || tr.arrive[next] < b) return true;  // a hold
    const JiShape::Kind kind = tr.shape[next].kind;
    if (kind == JiShape::LogLinear) return true;
    if (kind != JiShape::SCurve) return false;
    const double from = tr.cents[next - 1], delta = tr.cents[next] - from;
    if (delta == 0.0) return true;
    const Ticks begin = tr.begin[next], ticks = tr.arrive[next] - begin;
    const double len = static_cast<double>(ticks);
    const double k = 8.0 * tolerance / (std::abs(delta) * 6.0);  // a step h from u is fine while h² (1 − 2u) ≤ k
    us.clear();
    bool centre = false;  // a breakpoint at u = 1/2
    for (double u = 0.0; static_cast<Ticks>(us.size()) < ticks;) {
      const double mirror = 1.0 - 2.0 * u;  // the distance to the mirror point
      if (!(mirror > 0.0)) break;
      const double h = std::sqrt(k / mirror);
      if (h >= mirror) break;  // [u, 1 − u] is one segment
      if (h > 0.5 * mirror) {
        centre = true;
        break;
      }
      u += h;
      us.push_back(u);
    }
    Ticks last = a;
    auto point = [&](double u) {
      const Ticks x = begin + static_cast<Ticks>(u * len + 0.5);  // u ≥ 0 (llround is a call)
      if (x <= last || x >= b) return;
      last = x;
      const double v = static_cast<double>(x - begin) / len;
      push(x, from + delta * (v * v * (3.0 - 2.0 * v)));
    };
    for (const double u : us) point(u);
    if (centre) point(0.5);
    for (size_t i = us.size(); i-- > 0;) point(1.0 - us[i]);
    return true;
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
    for (size_t s = 1; s < S && tr.begin[s] < length; ++s) {
      if (tr.begin[s] > 0) knots.push_back(tr.begin[s]);
      if (tr.arrive[s] > 0 && tr.arrive[s] < length) knots.push_back(tr.arrive[s]);
    }
    knots.push_back(length);
    // T7.3: the windows ascend (a voice's begins and arrivals are running maxima), so without ride points the knots
    // are in order already.
    if (!rideKnots.empty()) std::sort(knots.begin(), knots.end());
    knots.erase(std::unique(knots.begin(), knots.end()), knots.end());
    out.reserve(v > 0 ? plan.voices[v - 1].pitch.size() + 8 : knots.size() * 8);  // T7.3: voices are alike
    Flattener f{o, tr, tolerance, out, {}};
    double fa = f.from(0);
    out.push_back({0, fa});
    for (size_t i = 0; i + 1 < knots.size(); ++i) {
      const Ticks a = knots[i], b = knots[i + 1];
      const double fb = f.upTo(b);
      if (!f.closedForm(a, b)) f.span(a, fa, b, fb);
      f.push(b, fb);
      if (i + 2 < knots.size()) {
        fa = f.from(b);
        if (std::abs(fa - fb) > 1e-9) out.push_back({b, fa});  // a jump
        else fa = fb;
      }
    }
    if (out.size() < 2) out.push_back({length, out.back().cents});
  }

  // ---- Unison classes and gains, stop by stop (ji_object.h "Gains"). ----
  // After each stop: rep[v] = the lowest voice of the group v is identical to (or, parked, the voice it is parked
  // against).
  std::array<uint8_t, kMaxJiVoices> rep{}, lead{}, sub{}, subSize{};
  std::array<bool, kMaxJiVoices> parked{}, included{}, reached{}, leaver{}, merger{};
  std::array<Ticks, kMaxJiVoices> t1{}, split{};
  std::array<bool, kMaxJiVoices> needSync{}, splits{};
  std::array<uint8_t, kMaxJiVoices> classSize{}, lockedSize{};
  std::vector<Ticks> grid;
  auto emit = [](std::vector<JiGainPoint>& g, Ticks tick, float gain, int16_t sync = -1) {
    if (!g.empty()) tick = std::max(tick, g.back().tick);
    const size_t n = g.size();
    if (n >= 1 && g[n - 1].tick == tick && g[n - 1].gain == gain && sync < 0) return;
    if (n >= 2 && g[n - 1].tick == tick && g[n - 2].tick == tick) {  // a tick keeps its first and its last point
      g[n - 1] = {tick, gain, sync};
      return;
    }
    g.push_back({tick, gain, sync});
  };
  for (size_t s = 0; s < S; ++s) {
    const JiStop& st = o.stops[s];
    const uint8_t* e = &entryOf[s * V];
    auto samePitch = [&](uint32_t a, uint32_t b) {
      return e[a] == e[b] || (st.isTuning() && st.pitches[e[a]] == st.pitches[e[b]]);
    };
    if (s == 0) {  // voices on one pitch from the start are identical lanes
      for (uint32_t v = 0; v < V; ++v) {
        uint32_t l = v;
        for (uint32_t u = 0; u < v; ++u)
          if (samePitch(u, v)) {
            l = u;
            break;
          }
        rep[v] = static_cast<uint8_t>(l);
      }
      for (uint32_t v = 0; v < V; ++v) {
        uint32_t size = 0;
        for (uint32_t u = 0; u < V; ++u) size += rep[u] == rep[v] ? 1u : 0u;
        plan.voices[v].gain.push_back({0, 1.0f / static_cast<float>(size), -1});
        plan.voices[v].unison.push_back({0, rep[v], static_cast<uint8_t>(size), static_cast<uint8_t>(size), true});
      }
      continue;
    }
    auto sameTiming = [&](uint32_t a, uint32_t b) {
      const VoiceTrack &x = tracks[a], &y = tracks[b];
      return x.begin[s] == y.begin[s] && x.arrive[s] == y.arrive[s] &&
             (x.arrive[s] == x.begin[s] || x.shape[s] == y.shape[s]);
    };
    bool anyReached = false;
    for (uint32_t v = 0; v < V; ++v) {
      reached[v] = tracks[v].begin[s] < length;
      anyReached = anyReached || reached[v];
    }
    if (!anyReached) break;  // begins only grow: no later stop is reached either
    auto together = [&](uint32_t a, uint32_t b) { return reached[a] && reached[b] && samePitch(a, b); };
    // The unison a voice takes part in: the voices on its pitch that are still there when the last of them arrives
    // (one that moves on earlier never holds the unison: it is parked against it instead).
    for (uint32_t v = 0; v < V; ++v) {
      Ticks last = tracks[v].arrive[s];
      for (uint32_t u = 0; u < V; ++u)
        if (u != v && together(u, v)) last = std::max(last, tracks[u].arrive[s]);
      included[v] = !reached[v] || s + 1 >= S || tracks[v].begin[s + 1] >= last;
    }
    for (uint32_t v = 0; v < V; ++v) {
      uint32_t l = v;
      bool found = false;
      for (uint32_t u = 0; u < V && !found; ++u)
        if (included[u] && (u == v || together(u, v))) {
          l = u;
          found = true;
        }
      lead[v] = static_cast<uint8_t>(l);  // an excluded voice: the unison it is parked against (itself if none)
    }
    for (uint32_t v = 0; v < V; ++v) {
      t1[v] = 0;
      classSize[v] = lockedSize[v] = 0;
      for (uint32_t u = 0; u < V; ++u) {
        if (u != v && !together(u, v)) continue;
        ++classSize[v];
        if (included[u] && lead[u] == lead[v]) {
          ++lockedSize[v];
          t1[v] = std::max(t1[v], tracks[u].arrive[s]);
        }
      }
    }
    // sub[v]: the lowest voice that stays identical to v through this transition (same group before, same unison
    // after, the same motion).
    for (uint32_t v = 0; v < V; ++v) {
      uint32_t c = v;
      if (!parked[v])
        for (uint32_t u = 0; u < v; ++u)
          if (!parked[u] && rep[u] == rep[v] && lead[u] == lead[v] && included[u] == included[v] &&
              reached[u] == reached[v] && (!reached[v] || sameTiming(u, v))) {
            c = u;
            break;
          }
      sub[v] = static_cast<uint8_t>(c);
    }
    for (uint32_t v = 0; v < V; ++v) {
      subSize[v] = 0;
      for (uint32_t u = 0; u < V; ++u) subSize[v] += sub[u] == sub[v] ? 1 : 0;
      needSync[v] = false;
      splits[v] = false;
      split[v] = length;
    }
    for (uint32_t v = 0; v < V; ++v) {
      leaver[v] = parked[v] || sub[v] != sub[rep[v]];
      merger[v] = !included[v] ? lead[v] != v : sub[v] != sub[lead[v]];
      if (included[v] && merger[v]) needSync[lead[v]] = true;
      if (!parked[v] && leaver[v]) splits[rep[v]] = true;
      if (!parked[v] && reached[v]) split[rep[v]] = std::min(split[rep[v]], tracks[v].begin[s]);
    }
    // The gain follows the squared distance (in cents) to the voice it leaves / joins: see ji_object.h.
    auto dist = [&](uint32_t a, uint32_t b, Ticks x, bool left) {
      return std::abs(tracks[a].at(static_cast<double>(x), left) - tracks[b].at(static_cast<double>(x), left));
    };
    auto riseEnd = [&](uint32_t v) { return std::max(tracks[v].arrive[s], tracks[rep[v]].arrive[s]); };
    auto rise = [&](uint32_t v, Ticks x) {
      if (!leaver[v]) return 1.0;
      const double full = dist(v, rep[v], riseEnd(v), false);
      if (!(full > 1e-9)) return 0.0;
      const double d = std::min(1.0, dist(v, rep[v], x, false) / full);
      return d * d;
    };
    auto fallStart = [&](uint32_t v) { return std::min(tracks[v].begin[s], tracks[lead[v]].begin[s]); };
    auto fall = [&](uint32_t v, Ticks x) {
      if (!merger[v]) return 1.0;
      const double full = dist(v, lead[v], fallStart(v), true);
      if (!(full > 1e-9)) return 0.0;
      const double d = std::min(1.0, dist(v, lead[v], x, false) / full);
      return d * d;
    };
    for (uint32_t v = 0; v < V; ++v) {
      if (!reached[v]) continue;
      const VoiceTrack& tr = tracks[v];
      JiVoicePlan& vp = plan.voices[v];
      const uint32_t l = lead[v];
      const bool sync = included[v] && needSync[l];
      if (tr.arrive[s] < length) {
        const JiUnison un{sync ? t1[v] : tr.arrive[s], static_cast<uint8_t>(l), classSize[v], lockedSize[v], included[v]};
        const JiUnison& last = vp.unison.back();
        if (last.leader != un.leader || last.size != un.size || last.lockedSize != un.lockedSize || last.locked != un.locked) {
          if (last.from >= un.from) vp.unison.back() = {last.from, un.leader, un.size, un.lockedSize, un.locked};
          else vp.unison.push_back(un);
        }
      }
      // The span in which this voice's gain moves.
      Ticks T0 = length + 1, T1 = -1;
      auto span = [&](Ticks a, Ticks b) {
        T0 = std::min(T0, a);
        T1 = std::max(T1, b);
      };
      if (parked[v]) span(std::min(tr.begin[s], tracks[rep[v]].begin[s]), riseEnd(v));
      else if (splits[rep[v]]) span(split[rep[v]], leaver[v] ? riseEnd(v) : split[rep[v]]);
      if (merger[v]) span(fallStart(v), included[v] ? t1[v] : std::max(tr.begin[s + 1], fallStart(v)));
      if (sync) span(t1[v], leaver[l] ? std::max(t1[v], riseEnd(l)) : t1[v]);
      if (T1 < 0) continue;  // its group moves as one into a unison of its own: nothing changes
      const double base = 1.0 / static_cast<double>(subSize[v]);
      const Ticks end = included[v] ? T1 : tr.begin[s + 1];  // an excluded voice is parked when it moves on
      const Ticks tail = std::min(gainRamp, end - T0);
      auto before = [&](Ticks x) {  // the voice on its own way (until its unison is complete)
        double g = base * rise(v, x) * fall(v, x);
        if (!included[v]) g *= tail > 0 ? std::clamp(static_cast<double>(end - x) / static_cast<double>(tail), 0.0, 1.0) : 0.0;
        return static_cast<float>(g);
      };
      auto after = [&](Ticks x) {  // in the complete unison: 1 / k of the keeper's level
        return static_cast<float>(rise(l, x) / static_cast<double>(lockedSize[v]));
      };
      grid.clear();
      const Ticks corners[] = {T0, T1, t1[v], tr.begin[s], tr.arrive[s], tracks[rep[v]].begin[s], tracks[rep[v]].arrive[s],
                             tracks[l].begin[s], tracks[l].arrive[s], end - tail};
      for (const Ticks k : corners)
        if (k >= T0 && k <= T1) grid.push_back(k);
      constexpr Ticks kSteps = 16;
      for (Ticks k = 1; k < kSteps; ++k) grid.push_back(T0 + (T1 - T0) * k / kSteps);
      std::sort(grid.begin(), grid.end());
      grid.erase(std::unique(grid.begin(), grid.end()), grid.end());
      std::vector<JiGainPoint>& g = vp.gain;
      emit(g, T0, g.back().gain);  // held until here
      const int16_t key = static_cast<int16_t>(l);
      for (const Ticks x : grid) {
        if (sync && x == t1[v]) {  // the unison is complete: Sync (a merging voice), then every voice steps to 1 / k
          const float pre = x > T0 ? before(x) : g.back().gain;
          if (g.back().tick == x && merger[v]) g.back().sync = key;  // a jump: the point it leaves carries the Sync
          else emit(g, x, pre, merger[v] ? key : int16_t{-1});
          if (g.size() >= 2 && g[g.size() - 2].tick == x) g.back() = {x, after(x), key};  // a tick keeps two points
          else g.push_back({x, after(x), key});
        } else {
          emit(g, x, sync && x > t1[v] ? after(x) : before(x));
        }
      }
    }
    for (uint32_t v = 0; v < V; ++v) {
      if (!reached[v]) continue;
      parked[v] = !included[v];
      rep[v] = lead[v];
    }
  }
  for (JiVoicePlan& vp : plan.voices) {  // clip to [0, length], closing at length
    std::vector<JiGainPoint>& g = vp.gain;
    for (size_t i = 1; i < g.size(); ++i) {
      if (g[i].tick <= length) continue;
      const JiGainPoint a = g[i - 1], b = g[i];
      const float x = static_cast<float>(static_cast<double>(length - a.tick) / static_cast<double>(b.tick - a.tick));
      g.resize(i);
      g.push_back({length, a.gain + (b.gain - a.gain) * x, -1});
      break;
    }
    if (g.back().tick < length || g.size() < 2) g.push_back({length, g.back().gain, -1});
  }
  return plan;
}

}  // namespace pq::doc
