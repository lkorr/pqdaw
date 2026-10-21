#include "pq/engine/automation.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace pq::engine {

namespace {

// Curved segments with |K·curve| at least this use the geometric recurrence (e^{k·x} advances by one constant factor
// per sample under a constant tempo); below it the recurrence's E − 1 would lose too many digits relative to
// expm1(k), so the law is evaluated per sample (straight lines below kAutomationLinearK are a separate fast path).
constexpr double kGeometricMinK = 1e-3;

// Index of the last point with tick ≤ t, or −1.
int64_t lastAtOrBefore(std::span<const AutomationPoint> pts, double t) noexcept {
  const auto it = std::upper_bound(pts.begin(), pts.end(), t, [](double v, const AutomationPoint& p) noexcept {
    return v < static_cast<double>(p.tick);
  });
  return static_cast<int64_t>(it - pts.begin()) - 1;
}

// The cursor for tick t, from the previous one (O(1) when t moved forward by less than a segment).
int64_t seekCursor(std::span<const AutomationPoint> pts, int64_t c, double t) noexcept {
  const int64_t n = static_cast<int64_t>(pts.size());
  if (c < -1 || c >= n) return lastAtOrBefore(pts, t);
  if (c >= 0 && static_cast<double>(pts[static_cast<size_t>(c)].tick) > t) return lastAtOrBefore(pts, t);  // backwards
  for (int k = 0; k < 4; ++k) {  // a few steps forward, then a binary search
    if (c + 1 >= n || static_cast<double>(pts[static_cast<size_t>(c + 1)].tick) > t) return c;
    ++c;
  }
  return lastAtOrBefore(pts, t);
}

// evalAutomation at t with the cursor already at the last point ≤ t.
double valueWithCursor(std::span<const AutomationPoint> pts, int64_t c, double t) noexcept {
  if (c < 0) return pts.front().value;
  const size_t i = static_cast<size_t>(c);
  if (i + 1 >= pts.size() || static_cast<double>(pts[i].tick) == t) return pts[i].value;
  return automationSegmentValue(pts[i], pts[i + 1], t);
}

}  // namespace

Result<std::unique_ptr<AutomationImage>> AutomationImage::build(std::span<const AutomationLaneDesc> lanes) {
  auto img = std::unique_ptr<AutomationImage>(new AutomationImage());
  size_t total = 0;
  for (const AutomationLaneDesc& l : lanes) total += l.points.size();
  if (total >= 0xFFFFFFFFull) return Error{ErrorCode::InvalidArgument, "AutomationImage: too many points", {}};
  img->lanes_.reserve(lanes.size());
  img->points_.reserve(total);
  for (const AutomationLaneDesc& l : lanes) {
    if (!std::isfinite(l.emptyValue))
      return Error{ErrorCode::InvalidArgument, "AutomationImage: lane " + std::to_string(l.target) + ": bad value", {}};
    Lane lane{l.target, l.emptyValue, static_cast<uint32_t>(img->points_.size()), 0};
    for (const AutomationPoint& p : l.points) {
      if (!std::isfinite(p.value) || !std::isfinite(p.curve))
        return Error{ErrorCode::InvalidArgument,
                     "AutomationImage: lane " + std::to_string(l.target) + ": non-finite point", {}};
      img->points_.push_back(p);
    }
    lane.end = static_cast<uint32_t>(img->points_.size());
    const auto b = img->points_.begin() + lane.begin, e = img->points_.begin() + lane.end;
    auto byTick = [](const AutomationPoint& x, const AutomationPoint& y) noexcept { return x.tick < y.tick; };
    if (!std::is_sorted(b, e, byTick)) std::stable_sort(b, e, byTick);
    img->lanes_.push_back(lane);
  }
  return img;
}

void AutomationRenderer::prepare(uint32_t maxLanes, uint32_t maxBlockFrames) {
  maxLanes_ = maxLanes;
  maxBlock_ = std::max(maxBlockFrames, 1u);
  stride_ = (static_cast<size_t>(maxBlock_) + 15u) & ~size_t{15};
  buf_ = AlignedBuffer<float>(std::max<size_t>(stride_ * maxLanes, 1));
  exact_ = AlignedBuffer<double>(maxBlock_);
  lanes_.assign(std::max(maxLanes, 1u), LaneState{-2, 0.0f, 0, {}});
  image_ = nullptr;
}

void AutomationRenderer::attach(const AutomationImage* image) noexcept {  // RT-safe
  image_ = image;
  for (LaneState& l : lanes_) {
    l.cursor = -2;  // re-seek
    l.constValid = 0;
  }
}

void AutomationRenderer::fillConst(uint32_t lane, float v, uint32_t frame, uint32_t len,
                                   bool wholeBlock) noexcept {  // RT-safe
  LaneState& st = lanes_[lane];
  if (st.constValid && st.constValue == v) return;  // the whole buffer already holds v
  float* b = buf_.data() + static_cast<size_t>(lane) * stride_;
  if (wholeBlock) {
    std::fill(b, b + maxBlock_, v);
    st.constValue = v;
    st.constValid = 1;
  } else {
    std::fill(b + frame, b + frame + len, v);
    st.constValid = 0;
  }
}

void AutomationRenderer::renderHold(double tick, uint32_t frame, uint32_t len, bool wholeBlock) noexcept {  // RT-safe
  const uint32_t n = numLanes();
  for (uint32_t l = 0; l < n; ++l) {
    const auto pts = image_->points(l);
    float v = image_->lanes()[l].emptyValue;
    if (!pts.empty()) {
      lanes_[l].cursor = seekCursor(pts, lanes_[l].cursor, tick);
      v = static_cast<float>(valueWithCursor(pts, lanes_[l].cursor, tick));
    }
    fillConst(l, v, frame, len, wholeBlock);
  }
}

void AutomationRenderer::render(const TickSpan& ticks, uint32_t frame, uint32_t len, bool wholeBlock) noexcept {  // RT-safe
  if (len == 0) return;
  const uint32_t n = numLanes();
  const double t0 = ticks.at(0);
  const double tLast = ticks.at(len - 1);
  // Interpolated ticks may differ from the exact ones in the last bits: a breakpoint within this margin of the run
  // counts as inside it, so that run takes the exact per-sample path.
  const double margin = ticks.perSample != nullptr ? 0.0 : 1e-6 + 1e-12 * std::abs(tLast);
  // Every lane constant through this whole block (the last block was, nothing moved backwards, no lane's next point
  // is reached): the buffers already hold the values. O(1) per block.
  if (wholeBlock && allConst_ && t0 >= lastTick_ && tLast + margin < holdUntil_) {
    lastTick_ = tLast;
    return;
  }
  bool allConst = wholeBlock;
  double holdUntil = std::numeric_limits<double>::infinity();
  const double* exact = ticks.perSample;  // fetched on first need
  for (uint32_t l = 0; l < n; ++l) {
    const auto pts = image_->points(l);
    if (pts.empty()) {
      fillConst(l, image_->lanes()[l].emptyValue, frame, len, wholeBlock);
      continue;
    }
    LaneState& st = lanes_[l];
    int64_t c = seekCursor(pts, st.cursor, t0);
    const int64_t np = static_cast<int64_t>(pts.size());
    const bool crosses = c + 1 < np && static_cast<double>(pts[static_cast<size_t>(c + 1)].tick) <= tLast + margin;
    // Constant: before the first point, after the last, or on a flat segment.
    if (!crosses && (c < 0 || c + 1 >= np || pts[static_cast<size_t>(c)].value == pts[static_cast<size_t>(c + 1)].value)) {
      st.cursor = c;
      fillConst(l, pts[static_cast<size_t>(c < 0 ? 0 : c)].value, frame, len, wholeBlock);
      if (c + 1 < np) holdUntil = std::min(holdUntil, static_cast<double>(pts[static_cast<size_t>(c + 1)].tick));
      continue;
    }
    allConst = false;
    float* out = buf_.data() + static_cast<size_t>(l) * stride_ + frame;
    st.constValid = 0;
    if (!crosses && ticks.perSample == nullptr) {
      // One segment a → b and a constant tempo: x is linear in the sample index.
      const AutomationPoint& a = pts[static_cast<size_t>(c)];
      const AutomationPoint& b = pts[static_cast<size_t>(c + 1)];
      const double ta = static_cast<double>(a.tick);
      const double inv = 1.0 / static_cast<double>(b.tick - a.tick);
      const double va = a.value, dv = static_cast<double>(b.value) - a.value;
      const double k = kAutomationCurveK * a.curve;
      if (std::abs(k) < kAutomationLinearK) {
        for (uint32_t i = 0; i < len; ++i) out[i] = static_cast<float>(va + dv * ((ticks.first + ticks.step * i - ta) * inv));
      } else if (std::abs(k) >= kGeometricMinK) {
        // e^{k·x(i)} = e^{k·x0} · (e^{k·dx})^i; relative error ≤ len · ulp, far below 1e-6 of the segment.
        const double x0 = (ticks.first - ta) * inv;
        const double r = std::exp(k * ticks.step * inv);
        const double scale = dv / std::expm1(k);
        double e = std::exp(k * x0);
        for (uint32_t i = 0; i < len; ++i) {
          out[i] = static_cast<float>(va + scale * (e - 1.0));
          e *= r;
        }
      } else {
        for (uint32_t i = 0; i < len; ++i) {
          const double x = (ticks.first + ticks.step * i - ta) * inv;
          out[i] = static_cast<float>(va + dv * automationShape(x, a.curve));
        }
      }
      st.cursor = c;
      continue;
    }
    // General case (a breakpoint inside the run, or a ramping tempo): the law per sample, at exact ticks.
    if (exact == nullptr && ticks.exact != nullptr) {
      ticks.exact(ticks.user, exact_.data(), len);
      exact = exact_.data();
    }
    for (uint32_t i = 0; i < len; ++i) {
      const double t = exact != nullptr ? exact[i] : ticks.at(i);
      while (c + 1 < np && static_cast<double>(pts[static_cast<size_t>(c + 1)].tick) <= t) ++c;
      out[i] = static_cast<float>(valueWithCursor(pts, c, t));
    }
    st.cursor = c;
  }
  allConst_ = allConst;
  holdUntil_ = holdUntil;
  lastTick_ = tLast;
}

}  // namespace pq::engine
