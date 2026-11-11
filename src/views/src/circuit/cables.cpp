// Hanging cables (TX.cables): the catenary rest table and the rope simulation. See pq/views/circuit/cables.h.

#include "pq/views/circuit/cables.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace pq::views::circuit {

using graph::Point2;

namespace {

constexpr uint32_t kAngles = 181;  // −90° … +90° (screen y down: −90° = b straight above a), 1° apart

// Points (unit chord from (0, 0) toward angle `phi` with x ≥ 0, screen y down) at equal arc length along the hanging
// rope of length `slack`, into xs / ys (m points). Double precision: a taut rope's catenary parameter is large and
// its cosh differences cancel.
void solveCatenary(double phi, double slack, uint32_t m, double* xs, double* ys) {
  const double h = std::cos(phi), sv = std::sin(phi);  // horizontal span, vertical drop (screen)
  const double L = std::max(slack, 1.0 + 1e-9);
  if (h < 1e-9) {  // straight up or down: the rope hangs from the upper end below the lower one and back (a fold)
    const double low = std::max(0.0, sv) + 0.5 * (L - std::fabs(sv));
    for (uint32_t j = 0; j < m; ++j) {
      const double s = L * static_cast<double>(j) / static_cast<double>(m - 1);
      xs[j] = 0.0;
      ys[j] = s <= low ? s : low - (s - low);
    }
    xs[m - 1] = 0.0;
    ys[m - 1] = sv;
    return;
  }
  const double V = -sv;  // rise from a to b (y up)
  const double r = std::sqrt(L * L - V * V) / h;  // sinh(u) / u = r, u = h / (2a)
  double u = std::sqrt(6.0 * std::max(r - 1.0, 1e-12));  // right of the root: Newton descends monotonically
  for (int it = 0; it < 80; ++it) {
    const double f = std::sinh(u) - r * u, df = std::cosh(u) - r;
    if (df <= 0.0) break;
    const double next = u - f / df;
    if (!(next > 0.0) || std::fabs(next - u) < 1e-14 * std::max(1.0, u)) {
      u = next > 0.0 ? next : u;
      break;
    }
    u = next;
  }
  const double a = h / (2.0 * u);
  const double mid = std::atanh(std::clamp(V / L, -1.0 + 1e-15, 1.0 - 1e-15));
  const double B = mid - u;  // = −x0 / a
  const double x0 = -a * B, sinhB = std::sinh(B), coshB = std::cosh(B);
  for (uint32_t j = 0; j < m; ++j) {
    const double s = L * static_cast<double>(j) / static_cast<double>(m - 1);
    const double q = s / a + sinhB;
    xs[j] = x0 + a * std::asinh(q);
    ys[j] = -(a * (std::sqrt(1.0 + q * q) - coshB));  // y up → screen
  }
  xs[0] = 0.0, ys[0] = 0.0;
  xs[m - 1] = h, ys[m - 1] = sv;
}

// The slack of table level l: √(slack − 1) evenly from 0 (straight) to √(kCableSlack − 1).
constexpr double kSagMax = 0.24494897427831780982;  // √(kCableSlack − 1) = √0.06
double levelSlack(uint32_t l) {
  const double w = kSagMax * static_cast<double>(l) / static_cast<double>(kSlackLevels - 1);
  return 1.0 + w * w;
}

// TL.7: [level][angle][point] — the unit catenaries over the angles × the slack levels (≈ 54k points, ≈ 0.4 MB).
struct RestTable {
  std::vector<float> x, y;
  RestTable() : x(size_t{kSlackLevels} * kAngles * kRestTablePoints), y(x.size()) {
    double xs[kRestTablePoints], ys[kRestTablePoints];
    for (uint32_t l = 0; l < kSlackLevels; ++l)
      for (uint32_t k = 0; k < kAngles; ++k) {
        const double phi = (static_cast<double>(k) - 90.0) * std::numbers::pi / 180.0;
        if (l == 0) {  // straight (a taut rope's catenary parameter is unbounded: the line itself)
          for (uint32_t j = 0; j < kRestTablePoints; ++j) {
            const double t = static_cast<double>(j) / static_cast<double>(kRestTablePoints - 1);
            xs[j] = t * std::cos(phi), ys[j] = t * std::sin(phi);
          }
        } else {
          solveCatenary(phi, levelSlack(l), kRestTablePoints, xs, ys);
        }
        const size_t at = (size_t{l} * kAngles + k) * kRestTablePoints;
        for (uint32_t j = 0; j < kRestTablePoints; ++j) {
          x[at + j] = static_cast<float>(xs[j]);
          y[at + j] = static_cast<float>(ys[j]);
        }
      }
  }
};

const RestTable& restTable() {
  static const RestTable table;  // thread-safe one-time init
  return table;
}

// Ends exact: the blend of neighbouring chords misses b by a hair; spread the error along the rope.
void pinEnds(Point2 a, Point2 b, uint32_t n, Point2* out) noexcept {
  const float inv = 1.0f / static_cast<float>(n - 1);
  const float ex = b.x - out[n - 1].x, ey = b.y - out[n - 1].y;
  for (uint32_t i = 1; i < n; ++i) {
    const float w = static_cast<float>(i) * inv;
    out[i].x += ex * w;
    out[i].y += ey * w;
  }
  out[0] = a;
  out[n - 1] = b;
}

// Degenerate chords: false when the caller should go on (D > 0).
bool trivial(Point2 a, Point2 b, uint32_t n, Point2* out, float D) noexcept {
  if (n == 0) return true;
  if (n == 1) {
    out[0] = a;
    return true;
  }
  if (D < 1e-4f) {
    for (uint32_t i = 0; i < n; ++i) out[i] = a;
    out[n - 1] = b;
    return true;
  }
  return false;
}

// From the table: fw = the slack level as a fraction (0 … kSlackLevels − 1).
void tableRest(Point2 a, Point2 b, uint32_t n, Point2* out, float D, float fw) noexcept {
  const RestTable& t = restTable();
  const float dx = b.x - a.x, dy = b.y - a.y;
  const float sx = dx < 0.0f ? -D : D;  // mirrored when b is left of a
  const float phiDeg = std::atan2(dy, std::fabs(dx)) * (180.0f / std::numbers::pi_v<float>) + 90.0f;
  const float fk = std::clamp(phiDeg, 0.0f, static_cast<float>(kAngles - 1));
  const uint32_t k0 = std::min(static_cast<uint32_t>(fk), kAngles - 2);
  const float tk = fk - static_cast<float>(k0);
  fw = std::clamp(fw, 0.0f, static_cast<float>(kSlackLevels - 1));
  const uint32_t l0 = std::min(static_cast<uint32_t>(fw), kSlackLevels - 2);
  const float tl = fw - static_cast<float>(l0);
  // The four corner weights (angle × level).
  const float w00 = (1.0f - tk) * (1.0f - tl), w01 = tk * (1.0f - tl), w10 = (1.0f - tk) * tl, w11 = tk * tl;
  const size_t r00 = (size_t{l0} * kAngles + k0) * kRestTablePoints, r01 = r00 + kRestTablePoints;
  const size_t r10 = r00 + size_t{kAngles} * kRestTablePoints, r11 = r10 + kRestTablePoints;
  const float* X = t.x.data();
  const float* Y = t.y.data();
  const float scale = static_cast<float>(kRestTablePoints - 1) / static_cast<float>(n - 1);
  for (uint32_t i = 0; i < n; ++i) {
    const float u = static_cast<float>(i) * scale;
    const uint32_t j = std::min(static_cast<uint32_t>(u), kRestTablePoints - 2);
    const float f = u - static_cast<float>(j);
    auto at = [&](const float* T) {
      const float c00 = T[r00 + j] * w00 + T[r01 + j] * w01 + T[r10 + j] * w10 + T[r11 + j] * w11;
      const float c01 = T[r00 + j + 1] * w00 + T[r01 + j + 1] * w01 + T[r10 + j + 1] * w10 + T[r11 + j + 1] * w11;
      return c00 + (c01 - c00) * f;
    };
    out[i] = {a.x + at(X) * sx, a.y + at(Y) * D};
  }
  pinEnds(a, b, n, out);
}

float levelOf(float slack) noexcept {
  return std::sqrt(std::max(0.0f, slack - 1.0f)) / static_cast<float>(kSagMax) * static_cast<float>(kSlackLevels - 1);
}

}  // namespace

uint32_t cablePointCount(float chord) noexcept {
  const float n = 10.0f + std::max(0.0f, chord) / 16.0f;
  return std::clamp(static_cast<uint32_t>(n), kCableMinPoints, kCableMaxPoints);
}

float cableSlack(float chord) noexcept {
  const float r = std::max(0.0f, chord) / kCableTightChord;
  return 1.0f + (kCableSlack - 1.0f) / (1.0f + r * r);
}

void cableRest(Point2 a, Point2 b, uint32_t n, Point2* out) noexcept {
  const float dx = b.x - a.x, dy = b.y - a.y;
  const float D = std::sqrt(dx * dx + dy * dy);
  if (trivial(a, b, n, out, D)) return;
  // √(slack − 1) = √(kCableSlack − 1) / √(1 + r²): the level without a square root of the slack.
  const float r = D / kCableTightChord;
  tableRest(a, b, n, out, D, static_cast<float>(kSlackLevels - 1) / std::sqrt(1.0f + r * r));
}

void cableRest(Point2 a, Point2 b, uint32_t n, Point2* out, float slack) noexcept {
  const float dx = b.x - a.x, dy = b.y - a.y;
  const float D = std::sqrt(dx * dx + dy * dy);
  if (trivial(a, b, n, out, D)) return;
  tableRest(a, b, n, out, D, levelOf(std::min(slack, kCableSlack)));
}

void cableRestSolved(Point2 a, Point2 b, uint32_t n, Point2* out, float slack) noexcept {
  const float dx = b.x - a.x, dy = b.y - a.y;
  const float D = std::sqrt(dx * dx + dy * dy);
  if (trivial(a, b, n, out, D)) return;
  const float sx = dx < 0.0f ? -D : D;
  double xs[kCableMaxPoints * 4], ys[kCableMaxPoints * 4];
  const uint32_t m = std::min<uint32_t>(n, kCableMaxPoints * 4);
  solveCatenary(std::atan2(static_cast<double>(dy), std::fabs(static_cast<double>(dx))), slack, m, xs, ys);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t j = std::min(i, m - 1);
    out[i] = {a.x + static_cast<float>(xs[j]) * sx, a.y + static_cast<float>(ys[j]) * D};
  }
  pinEnds(a, b, n, out);
}

// ---- CableSim ---------------------------------------------------------------------------------------------------------

void CableSim::place(Cable& c) {
  const float dx = c.b.x - c.a.x, dy = c.b.y - c.a.y;
  const uint32_t want = cablePointCount(std::sqrt(dx * dx + dy * dy));
  // Hysteresis: a cable keeps its count through small length changes (a drag would otherwise resample every frame).
  if (c.n == 0 || want + 3 <= c.n || want >= c.n + 3 || want == kCableMinPoints || want == kCableMaxPoints) {
    if (c.n != 0 && want != c.n) {  // resample the motion to the new count (by index fraction)
      Point2 p[kCableMaxPoints], q[kCableMaxPoints];
      const Point2* P = pos_.data() + c.first;
      const Point2* Q = prev_.data() + c.first;
      for (uint32_t i = 0; i < want; ++i) {
        const float u = static_cast<float>(i) * static_cast<float>(c.n - 1) / static_cast<float>(want - 1);
        const uint32_t j = std::min(static_cast<uint32_t>(u), c.n - 2);
        const float f = u - static_cast<float>(j);
        p[i] = {P[j].x + (P[j + 1].x - P[j].x) * f, P[j].y + (P[j + 1].y - P[j].y) * f};
        q[i] = {Q[j].x + (Q[j + 1].x - Q[j].x) * f, Q[j].y + (Q[j + 1].y - Q[j].y) * f};
      }
      std::copy(p, p + want, pos_.begin() + c.first);
      std::copy(q, q + want, prev_.begin() + c.first);
    }
    c.n = want;
  }
  Point2* R = rest_.data() + c.first;
  cableRest(c.a, c.b, c.n, R);
  float longest = 0.0f;
  for (uint32_t i = 0; i + 1 < c.n; ++i) {
    const float ex = R[i + 1].x - R[i].x, ey = R[i + 1].y - R[i].y;
    longest = std::max(longest, ex * ex + ey * ey);
  }
  c.seg = std::sqrt(longest);  // the rest shape never pulls on itself: its equilibrium is exactly the rest
  // The sag: the rest shape's farthest point from the chord. A pendulum of that length swings at ω² = g / sag.
  const float cx = c.b.x - c.a.x, cy = c.b.y - c.a.y, cl = std::max(std::sqrt(cx * cx + cy * cy), 1e-3f);
  float sag = 0.0f;
  for (uint32_t i = 1; i + 1 < c.n; ++i) sag = std::max(sag, std::fabs((R[i].x - c.a.x) * cy - (R[i].y - c.a.y) * cx) / cl);
  const auto dt = static_cast<float>(kCableStep);
  c.pull = std::clamp(kCableGravity / std::max(sag, 4.0f) * dt * dt, 0.001f, 0.05f);
}

void CableSim::toRest(Cable& c) {
  std::copy(rest_.begin() + c.first, rest_.begin() + c.first + c.n, pos_.begin() + c.first);
  std::copy(rest_.begin() + c.first, rest_.begin() + c.first + c.n, prev_.begin() + c.first);
}

void CableSim::updateBox(Cable& c) {
  const Point2* P = pos_.data() + c.first;
  Box b{P[0].x, P[0].y, P[0].x, P[0].y};
  for (uint32_t i = 1; i < c.n; ++i) {
    b.x0 = std::min(b.x0, P[i].x), b.x1 = std::max(b.x1, P[i].x);
    b.y0 = std::min(b.y0, P[i].y), b.y1 = std::max(b.y1, P[i].y);
  }
  c.box = b;
}

void CableSim::sleep(Cable& c) {
  if (c.awake) --awakeCount_;
  c.awake = false;
  c.calm = c.awakeSteps = 0;
  toRest(c);
  updateBox(c);
}

void CableSim::sync(std::span<const Spec> specs) {
  std::vector<Cable> old;
  old.swap(cables_);
  std::vector<Point2> oldPos, oldPrev;
  oldPos.swap(pos_);
  oldPrev.swap(prev_);
  const size_t cap = specs.size() * kCableMaxPoints;
  pos_.assign(cap, {});
  prev_.assign(cap, {});
  rest_.assign(cap, {});
  cables_.resize(specs.size());
  awakeCount_ = 0;
  // Old cables by key (sorted index: a scene has a few hundred cables at most).
  std::vector<uint32_t> order(old.size());
  for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) { return old[x].key < old[y].key; });
  for (uint32_t i = 0; i < specs.size(); ++i) {
    Cable& c = cables_[i];
    c = Cable{};
    c.key = specs[i].key;
    c.first = i * kCableMaxPoints;
    const auto it = std::lower_bound(order.begin(), order.end(), c.key, [&](uint32_t x, uint64_t k) { return old[x].key < k; });
    if (it != order.end() && old[*it].key == c.key && old[*it].n != 0) {
      const Cable& o = old[*it];
      std::copy(oldPos.begin() + o.first, oldPos.begin() + o.first + o.n, pos_.begin() + c.first);
      std::copy(oldPrev.begin() + o.first, oldPrev.begin() + o.first + o.n, prev_.begin() + c.first);
      c.a = o.a, c.b = o.b, c.n = o.n;
      c.awake = o.awake, c.calm = o.calm, c.awakeSteps = o.awakeSteps;
      awakeCount_ += c.awake ? 1u : 0u;
      place(c);  // its rest shape for the old ends (the pools are new)
      updateBox(c);
      c.prevBox = c.box;
      old[*it].n = 0;  // a duplicate key takes a fresh cable
      setEnds(i, specs[i].a, specs[i].b);
      continue;
    }
    c.a = specs[i].a;
    c.b = specs[i].b;
    place(c);
    toRest(c);
    updateBox(c);
    c.prevBox = c.box;
  }
  ++version_;
}

void CableSim::setEnds(uint32_t i, Point2 a, Point2 b, bool animate) {
  Cable& c = cables_[i];
  const float dax = a.x - c.a.x, day = a.y - c.a.y, dbx = b.x - c.b.x, dby = b.y - c.b.y;
  if (std::max({std::fabs(dax), std::fabs(day), std::fabs(dbx), std::fabs(dby)}) < 1e-3f) return;
  c.prevBox = c.box;
  c.a = a;
  c.b = b;
  place(c);
  ++version_;
  if (!animated_ || !animate) {
    if (c.awake) --awakeCount_;
    c.awake = false;
    c.calm = c.awakeSteps = 0;
    toRest(c);
    updateBox(c);
    return;
  }
  // Carried most of the way (each point by its share of the two ends' moves), the rest is the swing.
  Point2* P = pos_.data() + c.first;
  Point2* Q = prev_.data() + c.first;
  const float inv = 1.0f / static_cast<float>(c.n - 1);
  for (uint32_t k = 0; k < c.n; ++k) {
    const float t = static_cast<float>(k) * inv;
    const float mx = carry_ * (dax + (dbx - dax) * t), my = carry_ * (day + (dby - day) * t);
    P[k].x += mx, P[k].y += my;
    Q[k].x += mx, Q[k].y += my;
  }
  P[0] = a, Q[0] = a;
  P[c.n - 1] = b, Q[c.n - 1] = b;
  if (!c.awake) ++awakeCount_;
  c.awake = true;
  c.calm = 0;
  c.awakeSteps = 0;
  updateBox(c);
}

void CableSim::adopt(uint32_t i, const CableSim& from, uint32_t j, bool reversed) {
  if (!animated_ || i >= cables_.size() || j >= from.cables_.size()) return;
  Cable& c = cables_[i];
  const Cable& o = from.cables_[j];
  if (o.n < 2 || c.n < 2) return;
  c.prevBox = c.box;
  const Point2* P0 = from.pos_.data() + o.first;
  const Point2* Q0 = from.prev_.data() + o.first;
  Point2* P = pos_.data() + c.first;
  Point2* Q = prev_.data() + c.first;
  // Resampled by index fraction (both ropes are at about equal arc length).
  for (uint32_t k = 0; k < c.n; ++k) {
    float u = static_cast<float>(k) * static_cast<float>(o.n - 1) / static_cast<float>(c.n - 1);
    if (reversed) u = static_cast<float>(o.n - 1) - u;
    const uint32_t m = std::min(static_cast<uint32_t>(u), o.n - 2);
    const float f = u - static_cast<float>(m);
    P[k] = {P0[m].x + (P0[m + 1].x - P0[m].x) * f, P0[m].y + (P0[m + 1].y - P0[m].y) * f};
    Q[k] = {Q0[m].x + (Q0[m + 1].x - Q0[m].x) * f, Q0[m].y + (Q0[m + 1].y - Q0[m].y) * f};
  }
  // Onto this cable's ends: each point moved by its share of the two ends' offsets (the motion itself is kept).
  const float dax = c.a.x - P[0].x, day = c.a.y - P[0].y, dbx = c.b.x - P[c.n - 1].x, dby = c.b.y - P[c.n - 1].y;
  const float inv = 1.0f / static_cast<float>(c.n - 1);
  for (uint32_t k = 0; k < c.n; ++k) {
    const float t = static_cast<float>(k) * inv;
    const float mx = dax + (dbx - dax) * t, my = day + (dby - day) * t;
    P[k].x += mx, P[k].y += my;
    Q[k].x += mx, Q[k].y += my;
  }
  P[0] = Q[0] = c.a;
  P[c.n - 1] = Q[c.n - 1] = c.b;
  if (!c.awake) ++awakeCount_;
  c.awake = true;
  c.calm = c.awakeSteps = 0;
  updateBox(c);
  ++version_;
}

void CableSim::setAnimated(bool on) {
  if (animated_ == on) return;
  animated_ = on;
  if (!on) settle();
}

void CableSim::settle() {
  for (Cable& c : cables_)
    if (c.awake) {
      c.prevBox = c.box;
      sleep(c);
    }
  awakeCount_ = 0;
  ++version_;
}

void CableSim::step(Cable& c) {
  Point2* P = pos_.data() + c.first;
  Point2* Q = prev_.data() + c.first;
  const Point2* R = rest_.data() + c.first;
  const uint32_t n = c.n;
  const auto boostAfter = static_cast<uint32_t>(kCableBoostSeconds / kCableStep);
  const bool boost = c.awakeSteps > boostAfter;
  const float damp = boost ? 0.85f : kCableDamping;
  const float pull = boost ? 0.05f : c.pull;
  // Verlet: velocity from the last step, damped; gravity's restoring pull toward the hanging shape.
  for (uint32_t i = 1; i + 1 < n; ++i) {
    const float vx = (P[i].x - Q[i].x) * damp, vy = (P[i].y - Q[i].y) * damp;
    Q[i] = P[i];
    P[i].x += vx + pull * (R[i].x - P[i].x);
    P[i].y += vy + pull * (R[i].y - P[i].y);
  }
  P[0] = c.a, Q[0] = c.a;
  P[n - 1] = c.b, Q[n - 1] = c.b;
  // Length constraints (a segment may not grow past the rest shape's longest), alternating sweeps (the ends pinned: a
  // segment touching one moves only its free point).
  const float L = c.seg;
  for (uint32_t it = 0; it < kCableIterations; ++it) {
    const bool fwd = (it & 1u) == 0;
    for (uint32_t s = 0; s + 1 < n; ++s) {
      const uint32_t i = fwd ? s : n - 2 - s;
      const float ex = P[i + 1].x - P[i].x, ey = P[i + 1].y - P[i].y;
      const float len2 = ex * ex + ey * ey;
      if (len2 <= L * L) continue;  // a rope: it does not stretch, but it may go slack
      const float len = std::sqrt(len2);
      const float k = (len - L) / len;
      const bool pinA = i == 0, pinB = i + 1 == n - 1;
      if (pinA && pinB) continue;
      if (pinA) {
        P[i + 1].x -= ex * k, P[i + 1].y -= ey * k;
      } else if (pinB) {
        P[i].x += ex * k, P[i].y += ey * k;
      } else {
        const float h = 0.5f * k;
        P[i].x += ex * h, P[i].y += ey * h;
        P[i + 1].x -= ex * h, P[i + 1].y -= ey * h;
      }
    }
  }
  // Calm: no point moved more than the sleep speed, and the rope is within reach of its rest shape.
  float v2 = 0.0f, off2 = 0.0f;
  for (uint32_t i = 1; i + 1 < n; ++i) {
    const float vx = P[i].x - Q[i].x, vy = P[i].y - Q[i].y;
    const float ox = P[i].x - R[i].x, oy = P[i].y - R[i].y;
    v2 = std::max(v2, vx * vx + vy * vy);
    off2 = std::max(off2, ox * ox + oy * oy);
  }
  ++c.awakeSteps;
  ++steps_;
  c.calm = v2 < kCableSleepSpeed * kCableSleepSpeed && off2 < kCableSleepOffset * kCableSleepOffset ? c.calm + 1 : 0;
  if (c.calm >= kCableCalmSteps || static_cast<double>(c.awakeSteps) * kCableStep >= kCableMaxAwakeSeconds) sleep(c);
}

bool CableSim::advance(double seconds) {
  if (awakeCount_ == 0) {
    pending_ = 0.0;
    return false;
  }
  pending_ += std::max(0.0, seconds);
  uint32_t steps = 0;
  while (pending_ >= kCableStep && steps < kCableMaxStepsPerAdvance) {
    pending_ -= kCableStep;
    ++steps;
  }
  if (steps == kCableMaxStepsPerAdvance) pending_ = std::min(pending_, kCableStep);  // late: slower, not bigger steps
  if (steps == 0) return false;
  bool moved = false;
  for (Cable& c : cables_) {
    if (!c.awake) continue;
    c.prevBox = c.box;
    for (uint32_t s = 0; s < steps && c.awake; ++s) step(c);
    updateBox(c);
    moved = true;
  }
  if (moved) ++version_;
  return moved;
}

}  // namespace pq::views::circuit
