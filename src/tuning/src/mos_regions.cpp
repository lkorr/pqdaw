#include "pq/tuning/mos_regions.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace pq::tuning {

// ---- buildMOSRegions ---------------------------------------------------------------------------------------------

MosRegionSet buildMosRegions(int32_t maxSize, int32_t maxDepth, double period, int32_t reps) {
  MosRegionSet out;
  std::vector<std::pair<int32_t, uint8_t>> colors;  // insertion order, as the JS object assigns them
  uint32_t ci = 0;
  auto color = [&](int32_t count) -> uint8_t {
    for (const auto& [c, idx] : colors)
      if (c == count) return idx;
    const uint8_t idx = static_cast<uint8_t>(ci++ % kMosPalette.size());
    colors.emplace_back(count, idx);
    return idx;
  };

  std::vector<MosRegion> all;
  auto addRegion = [&](int32_t ppNL, int32_t ppNS, int32_t ppSize, int32_t fp1, int32_t fq1, int32_t fp2, int32_t fq2,
                       int32_t depth) {
    const double lowCents = (static_cast<double>(fp1) / fq1) * period;
    const double highCents = (static_cast<double>(fp2) / fq2) * period;
    if (highCents - lowCents < 0.001) return;
    MosRegion r;
    r.key = {ppNL * reps, ppNS * reps};
    r.size = ppSize * reps;
    r.perPeriodKey = {ppNL, ppNS};
    r.perPeriodSize = ppSize;
    r.collapsedColor = color(r.key.nL);
    r.equalizedColor = color(r.size);
    r.depth = depth;
    r.parent = findParentMos(r.key.nL, r.key.nS);
    r.lowFrac = {fp1, fq1};
    r.highFrac = {fp2, fq2};
    r.lowCents = lowCents;
    r.highCents = highCents;
    all.push_back(r);
    r.lowCents = period - highCents;
    r.highCents = period - lowCents;
    r.lowFrac = {fq2 - fp2, fq2};
    r.highFrac = {fq1 - fp1, fq1};
    r.isMirror = true;
    all.push_back(r);
  };
  // Recursion depth ≤ maxDepth (a mediant's denominator grows by ≥ 1 per level, so also ≤ maxSize).
  auto traverse = [&](auto&& self, int32_t lp, int32_t lq, int32_t rp, int32_t rq, int32_t depth) -> void {
    if (depth <= 0) return;
    const int32_t mp = lp + rp, mq = lq + rq;
    if (mq > maxSize) return;
    addRegion(lq, rq, mq, lp, lq, mp, mq, maxDepth - depth + 1);
    addRegion(rq, lq, mq, mp, mq, rp, rq, maxDepth - depth + 1);
    self(self, lp, lq, mp, mq, depth - 1);
    self(self, mp, mq, rp, rq, depth - 1);
  };
  if (maxSize >= 2) addRegion(1, 1, 2, 0, 1, 1, 2, 0);
  traverse(traverse, 0, 1, 1, 2, maxDepth);

  for (MosRegion& r : all)
    if (r.perPeriodSize >= 2 && r.perPeriodSize <= maxSize) out.regions.push_back(r);
  std::sort(colors.begin(), colors.end());
  out.colorMap = std::move(colors);
  return out;
}

RegionEnd equalizedEnd(const MosRegion& region, double period) {
  const double lo = region.lowCents + 0.01;
  const double hi = region.highCents - 0.01;
  const Hardness hrLo = computeHardness(lo, region.perPeriodSize, period);
  const Hardness hrHi = computeHardness(hi, region.perPeriodSize, period);
  return hrLo.ratio < hrHi.ratio ? RegionEnd::Low : RegionEnd::High;
}

int32_t findAdjacentRegion(std::span<const MosRegion> regions, const MosRegion& sel, double cents, int32_t minSize,
                           int32_t maxSize) {
  std::vector<int32_t> candidates;
  for (size_t i = 0; i < regions.size(); ++i) {
    const MosRegion& r = regions[i];
    if (r.perPeriodSize >= minSize && r.perPeriodSize <= maxSize && cents >= r.lowCents - 0.5 &&
        cents <= r.highCents + 0.5 &&
        !(r.key == sel.key && std::fabs(r.lowCents - sel.lowCents) < 0.01 &&
          std::fabs(r.highCents - sel.highCents) < 0.01))
      candidates.push_back(static_cast<int32_t>(i));
  }
  if (candidates.empty()) return -1;
  // The JS comparator: size distance first, then distance of the region centre from `cents` (stable).
  auto cmp = [&](int32_t ia, int32_t ib) {
    const MosRegion& a = regions[static_cast<size_t>(ia)];
    const MosRegion& b = regions[static_cast<size_t>(ib)];
    const int32_t da = std::abs(a.perPeriodSize - sel.perPeriodSize);
    const int32_t db = std::abs(b.perPeriodSize - sel.perPeriodSize);
    if (da != db) return da < db;
    const double ma = (a.lowCents + a.highCents) / 2.0;
    const double mb = (b.lowCents + b.highCents) / 2.0;
    return std::fabs(ma - cents) - std::fabs(mb - cents) < 0.0;
  };
  std::stable_sort(candidates.begin(), candidates.end(), cmp);
  return candidates[0];
}

// ---- Linear form -------------------------------------------------------------------------------------------------

MosLinearTable::MosLinearTable(int32_t n) : n_(n) {
  assert(n >= 1);
  // Farey fractions p/q in [0, 1] with q ≤ n − 1 (q ≥ 1), generated in order by the next-term recurrence.
  const int32_t order = std::max(n - 1, 1);
  int32_t a = 0, b = 1, c = 1, d = order;
  bounds_.push_back(0.0);
  while (c <= order) {
    const int32_t kk = (order + b) / d;
    const int32_t e = kk * c - a, f = kk * d - b;
    a = c;
    b = d;
    c = e;
    d = f;
    bounds_.push_back(static_cast<double>(a) / b);
  }
  // (For n = 1 the chain is just the tonic: one interval [0, 1).)
  if (n == 1) bounds_ = {0.0, 1.0};

  const size_t intervals = bounds_.size() - 1;
  k_.resize(intervals * static_cast<size_t>(n));
  m_.resize(intervals * static_cast<size_t>(n));
  struct Entry {
    double frac;
    int32_t k;
    int32_t m;
  };
  std::vector<Entry> chain(static_cast<size_t>(n));
  for (size_t t = 0; t < intervals; ++t) {
    // At the midpoint no two chain notes coincide, so the order is the interval's.
    const double x = (bounds_[t] + bounds_[t + 1]) / 2.0;
    for (int32_t i = 0; i < n; ++i) {
      const double v = i * x;
      const double fl = std::floor(v);
      chain[static_cast<size_t>(i)] = {v - fl, i, static_cast<int32_t>(fl)};
    }
    std::sort(chain.begin(), chain.end(), [](const Entry& p, const Entry& q) { return p.frac < q.frac; });
    for (int32_t j = 0; j < n; ++j) {
      k_[t * static_cast<size_t>(n) + static_cast<size_t>(j)] = chain[static_cast<size_t>(j)].k;
      m_[t * static_cast<size_t>(n) + static_cast<size_t>(j)] = chain[static_cast<size_t>(j)].m;
    }
  }
}

MosLinearSpan MosLinearTable::lookup(double genCents, double period) const noexcept {  // RT-safe
  const double x = genCents / period;
  const double shift = std::floor(x);
  double x0 = x - shift;
  if (x0 >= 1.0) x0 = 0.0;  // guards rounding when x is a hair below an integer
  // Last boundary ≤ x0 (bounds_[0] = 0 ≤ x0 < 1 = bounds_.back()): ~log2(0.3·n²) compares.
  const auto it = std::upper_bound(bounds_.begin(), bounds_.end(), x0);
  size_t t = static_cast<size_t>(it - bounds_.begin()) - 1;
  if (t >= bounds_.size() - 1) t = bounds_.size() - 2;
  MosLinearSpan s;
  s.k = k_.data() + t * static_cast<size_t>(n_);
  s.m = m_.data() + t * static_cast<size_t>(n_);
  s.n = n_;
  s.periodShift = static_cast<int32_t>(shift);
  s.lowGenerator = (shift + bounds_[t]) * period;
  s.highGenerator = (shift + bounds_[t + 1]) * period;
  return s;
}

void rotateLinearForm(const MosLinearSpan& s, int32_t rotation, int32_t* kOut, int32_t* mOut) noexcept {  // RT-safe
  const int32_t n = s.n;
  const int32_t kr = s.k[rotation];
  const int32_t mr = s.m[rotation] + s.k[rotation] * s.periodShift;
  for (int32_t i = 0; i < n; ++i) {
    const int32_t a = i + rotation < n ? i + rotation : i + rotation - n;
    const int32_t wrapped = i + rotation < n ? 0 : 1;  // the note was below the new root: add one period
    kOut[i] = s.k[a] - kr;
    mOut[i] = (s.m[a] + s.k[a] * s.periodShift) - mr - wrapped;
  }
}

void evaluateLinearForm(const MosLinearSpan& s, double genCents, double period, double* out) noexcept {  // RT-safe
  for (int32_t j = 0; j < s.n; ++j) out[j] = s.cents(j, genCents, period);
}

}  // namespace pq::tuning
