#include "pq/views/piano_roll/row_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "pq/tuning/edo.h"

namespace pq::views::roll {

namespace {

// Upper bound on the rows of an unfolded roll: a 10000-EDO over 13 octaves would otherwise build 10^5 rows. Beyond
// it the range shrinks to the used degrees (or the reference equave) ± one equave.
constexpr int64_t kMaxRows = 20000;

int64_t floorDiv(int64_t a, int64_t b) noexcept {
  const int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

int64_t floorMod(int64_t a, int64_t b) noexcept { return a - floorDiv(a, b) * b; }

double equaveCentsOf(const tuning::Tuning& t) noexcept {
  if (const tuning::Edo* e = t.edo()) return e->equaveCents();
  const int32_t n = degreesPerEquave(t);
  return degreeCents(t, n) - degreeCents(t, 0);
}

bool isOctave(double cents) noexcept { return std::fabs(cents - 1200.0) < 1e-9; }

constexpr const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

}  // namespace

int32_t degreesPerEquave(const tuning::Tuning& t) noexcept {
  if (const tuning::Edo* e = t.edo()) return static_cast<int32_t>(e->n());
  if (const tuning::Mos* m = t.mos()) return m->size();
  if (const tuning::Scale* s = t.scale()) return s->size();
  return 1;
}

double degreeCents(const tuning::Tuning& t, int64_t degree) noexcept {
  if (const tuning::Edo* e = t.edo()) return e->centsOfStep(degree);
  if (const tuning::Mos* m = t.mos()) return m->centsOfDegree(degree);
  if (const tuning::Scale* s = t.scale()) return s->centsOfDegree(degree);
  return 0.0;
}

int64_t absoluteDegree(const tuning::Tuning& t, int32_t degree, int32_t equave) noexcept {
  return static_cast<int64_t>(equave) * degreesPerEquave(t) + degree;
}

uint32_t twelveCompatibleEdo(const tuning::Tuning& t) noexcept {
  const tuning::Edo* e = t.edo();
  if (!e || e->n() % 12 != 0 || !isOctave(e->equaveCents())) return 0;
  return e->n();
}

std::vector<uint8_t> defaultHighlightScale(const tuning::Tuning& t) {
  const int32_t n = degreesPerEquave(t);
  std::vector<uint8_t> all(static_cast<size_t>(std::max(n, 1)), 1);
  const tuning::Edo* e = t.edo();
  if (!e || n < 7 || !isOctave(e->equaveCents())) return all;
  const tuning::Val v = tuning::patentVal(*e);
  const int64_t fifth = static_cast<int64_t>(v.steps[1]) - v.steps[0];  // 3/2 = 3 − 2 in steps
  std::vector<uint8_t> scale(static_cast<size_t>(n), 0);
  int count = 0;
  for (int64_t k = -1; k <= 5; ++k) {  // F C G D A E B
    uint8_t& s = scale[static_cast<size_t>(floorMod(k * fifth, n))];
    if (!s) ++count;
    s = 1;
  }
  return count == 7 ? scale : all;
}

RowModel RowModel::build(const tuning::Tuning& tuning, const RowOptions& o) {
  RowModel m;
  const int32_t n = std::max(1, roll::degreesPerEquave(tuning));
  m.size_ = n;
  const double eq = equaveCentsOf(tuning);
  const double step = eq > 0.0 ? eq / n : 100.0;
  const int32_t rowPx = std::max(1, o.rowPx);
  auto inScale = [&](int64_t d) {
    return o.scale.empty() || o.scale[static_cast<size_t>(floorMod(d, n)) % o.scale.size()] != 0;
  };

  // The degrees to show, highest first.
  std::vector<int64_t> degrees;
  const bool usedFold = o.fold == Fold::UsedNotes && !o.used.empty();
  if (usedFold) {
    degrees.assign(o.used.rbegin(), o.used.rend());
  } else {
    int64_t lo = static_cast<int64_t>(std::floor(o.lowCents / step));
    int64_t hi = static_cast<int64_t>(std::ceil(o.highCents / step));
    if (!o.used.empty()) {
      lo = std::min(lo, o.used.front());
      hi = std::max(hi, o.used.back());
    }
    if (hi - lo + 1 > kMaxRows) {
      const int64_t centre = o.used.empty() ? 0 : o.used.front();
      lo = o.used.empty() ? -n : o.used.front() - n;
      hi = o.used.empty() ? 2 * n : o.used.back() + n;
      if (hi - lo + 1 > kMaxRows) {
        lo = centre - kMaxRows / 2;
        hi = lo + kMaxRows - 1;
      }
    }
    degrees.reserve(static_cast<size_t>(hi - lo + 1));
    for (int64_t d = hi; d >= lo; --d)
      if (o.fold != Fold::Scale || inScale(d)) degrees.push_back(d);
  }

  const std::vector<uint8_t> diatonic = defaultHighlightScale(tuning);
  const uint32_t twelve = twelveCompatibleEdo(tuning);
  const int64_t perSemitone = twelve ? twelve / 12 : 0;
  const bool octave = isOctave(eq);
  const tuning::Edo* edo = tuning.edo();

  m.rows_.resize(degrees.size());
  m.labels_.resize(degrees.size());
  m.equaveLabels_.resize(degrees.size());
  char buf[48];
  for (size_t i = 0; i < degrees.size(); ++i) {
    const int64_t d = degrees[i];
    const int64_t within = floorMod(d, n);
    const int64_t equave = floorDiv(d, n);
    PitchRow& r = m.rows_[i];
    r.degree = d;
    r.cents = degreeCents(tuning, d);
    r.top = static_cast<int32_t>(i) * rowPx;
    r.bottom = r.top + rowPx;
    r.centre = r.top + rowPx / 2;
    uint8_t f = 0;
    if (within == 0) f |= kRowEquaveStart;
    if (diatonic[static_cast<size_t>(within)]) f |= kRowNatural;
    if (inScale(d)) f |= kRowInScale;
    // Labels (Q2): 12edo-compatible EDOs name the 12edo rows Ableton-style (C3 = degree 0 of equave 0, the
    // reference); other EDOs label every row k\n and put the equave number on degree 0 (octave EDOs count like
    // Ableton's octaves, 3 = the reference's; other equaves count from 0).
    if (perSemitone && within % perSemitone == 0) {
      f |= kRowNamed;
      std::snprintf(buf, sizeof buf, "%s%lld", kNoteNames[within / perSemitone], static_cast<long long>(equave + 3));
      m.labels_[i] = buf;
    } else if (edo) {
      m.labels_[i] = tuning::stepLabel(within, static_cast<uint32_t>(n));
      if (within == 0) m.equaveLabels_[i] = std::to_string(octave ? equave + 3 : equave);
    } else {
      m.labels_[i] = std::to_string(within);
      if (within == 0) m.equaveLabels_[i] = std::to_string(equave);
    }
    r.flags = f;
  }
  m.contiguous_ = !usedFold && o.fold != Fold::Scale;
  m.topDegree_ = degrees.empty() ? 0 : degrees.front();
  return m;
}

int64_t RowModel::indexOfDegree(int64_t degree) const noexcept {
  if (rows_.empty()) return -1;
  if (contiguous_) {
    const int64_t i = topDegree_ - degree;
    return (i >= 0 && i < static_cast<int64_t>(rows_.size())) ? i : -1;
  }
  // Degrees descend with the index.
  size_t lo = 0, hi = rows_.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (rows_[mid].degree > degree) lo = mid + 1;
    else hi = mid;
  }
  return (lo < rows_.size() && rows_[lo].degree == degree) ? static_cast<int64_t>(lo) : -1;
}

CentsPlacement RowModel::placeCents(double cents) const noexcept {
  // k = first row at or below the pitch (rows descend in cents).
  size_t lo = 0, hi = rows_.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (rows_[mid].cents > cents) lo = mid + 1;
    else hi = mid;
  }
  const size_t k = lo;
  size_t a, b;  // a above (higher pitch), b below; extrapolate from the end pair outside the range
  if (rows_.size() == 1) {
    a = b = 0;
  } else if (k == 0) {
    a = 0;
    b = 1;
  } else if (k == rows_.size()) {
    a = rows_.size() - 2;
    b = rows_.size() - 1;
  } else {
    a = k - 1;
    b = k;
  }
  const PitchRow& ra = rows_[a];
  const PitchRow& rb = rows_[b];
  double y;
  if (a == b || ra.cents == rb.cents) {
    y = ra.centre;
  } else {
    const double t = (cents - rb.cents) / (ra.cents - rb.cents);
    y = rb.centre + (ra.centre - rb.centre) * t;
  }
  const PitchRow& near = std::fabs(cents - ra.cents) <= std::fabs(cents - rb.cents) ? ra : rb;
  CentsPlacement p;
  p.centre = static_cast<int32_t>(std::lround(y));
  p.top = p.centre - (near.centre - near.top);
  p.bottom = p.centre + (near.bottom - near.centre);
  p.nearestDegree = near.degree;
  p.offsetCents = cents - near.cents;
  return p;
}

size_t RowModel::indexAtY(int32_t y) const noexcept {
  size_t lo = 0, hi = rows_.size();
  while (lo < hi) {  // first row whose bottom is below y
    const size_t mid = (lo + hi) / 2;
    if (rows_[mid].bottom <= y) lo = mid + 1;
    else hi = mid;
  }
  if (lo >= rows_.size()) return rows_.size() - 1;
  if (lo > 0 && y < rows_[lo].top && rows_[lo].top - y > y - rows_[lo - 1].bottom) return lo - 1;
  return lo;
}

std::pair<size_t, size_t> RowModel::rowsIn(int32_t y0, int32_t y1) const noexcept {
  auto firstWith = [&](auto pred) {
    size_t lo = 0, hi = rows_.size();
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (pred(rows_[mid])) hi = mid;
      else lo = mid + 1;
    }
    return lo;
  };
  const size_t first = firstWith([&](const PitchRow& r) { return r.bottom > y0; });
  const size_t last = firstWith([&](const PitchRow& r) { return r.top >= y1; });
  return {first, std::max(first, last)};
}

}  // namespace pq::views::roll
