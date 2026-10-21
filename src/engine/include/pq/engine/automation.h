#pragma once

// Automation as a signal source (T3.16, SPEC §9.6): breakpoint lanes described as plain data (filled by T3.17's
// lowering), built into an immutable AutomationImage off the audio thread, published by the Timeline (timeline.h)
// and rendered per sample into audio-rate buffers, one per automated target. Unautomated params are not touched:
// they stay param-rate in the param block.
//
// The curve law is pq/core/automation_curve.h (Q39), shared with the document's edit operations and the views:
// evalAutomation(points, tick) is the reference every rendered sample matches (within 1e-6 relative; the renderer
// takes fast paths for constant, straight and curved segments under a constant tempo, and evaluates the law per
// sample otherwise). A sample's tick is the fractional tick of its exact time (TempoMap::samplesToTicks).

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/automation_curve.h"
#include "pq/core/result.h"
#include "pq/core/time.h"

namespace pq::engine {

using pq::evalAutomation;

struct AutomationPoint {
  Ticks tick = 0;       // arrangement ticks
  float value = 0.0f;   // in the target's own unit
  float curve = 0.0f;   // [−1, 1] curvature of the segment to the next point (Q39)
};

struct AutomationLaneDesc {
  // What the lane drives. The renderer only carries it: Timeline::writeAutomatedParams writes the lane's frame-0
  // value into params[target] (param-rate consumers); T3.17 may instead bind the audio-rate buffer to an input slot.
  uint32_t target = 0;
  float emptyValue = 0.0f;              // the value of a lane without points
  std::vector<AutomationPoint> points;  // sorted by tick (points on one tick keep their order: a jump)
};

class AutomationImage {
public:
  struct Lane {
    uint32_t target;
    float emptyValue;
    uint32_t begin, end;  // into points()
  };

  // Off the audio thread (allocates). Points are stably sorted by tick. Errors: a non-finite value or curve.
  static Result<std::unique_ptr<AutomationImage>> build(std::span<const AutomationLaneDesc> lanes);

  // RT-safe accessors.
  std::span<const Lane> lanes() const noexcept { return lanes_; }
  uint32_t numLanes() const noexcept { return static_cast<uint32_t>(lanes_.size()); }
  std::span<const AutomationPoint> points(uint32_t lane) const noexcept {
    return {points_.data() + lanes_[lane].begin, points_.data() + lanes_[lane].end};
  }
  // evalAutomation on lane `lane` (the empty value for a lane without points). RT-safe.
  double valueAt(uint32_t lane, double tick) const noexcept {
    return evalAutomation(points(lane), tick, lanes_[lane].emptyValue);
  }

private:
  std::vector<Lane> lanes_;
  std::vector<AutomationPoint> points_;
};

// The ticks of a run of consecutive samples: tick(i) = first + step · i under a constant tempo (exact up to
// rounding), or perSample[i] (exact per sample) while the tempo ramps. `exact` (optional) writes the exact tick of
// each sample: the renderer asks for it, once per run, when a breakpoint falls inside the run, so a sample right at
// a jump lands on the same side of it as evalAutomation(TempoMap::samplesToTicks(sample)) puts it.
struct TickSpan {
  double first = 0.0;
  double step = 0.0;
  const double* perSample = nullptr;
  void (*exact)(const void* user, double* out, uint32_t len) noexcept = nullptr;
  const void* user = nullptr;
  double at(uint32_t i) const noexcept { return perSample != nullptr ? perSample[i] : first + step * i; }  // RT-safe
};

// Renders an AutomationImage's lanes into one audio-rate buffer per lane (the Timeline owns one). Per lane an
// incremental cursor (the segment of the last rendered sample), so a block costs O(frames) for a moving value and
// O(1) for a constant one: a lane whose block is constant (before the first point, after the last, on a flat
// stretch, or while the transport is stopped) fills its buffer once and then only compares the value.
class AutomationRenderer {
public:
  // Off the audio thread: buffers for up to maxLanes lanes of maxBlockFrames frames.
  void prepare(uint32_t maxLanes, uint32_t maxBlockFrames);

  // Audio thread: render `image` from now on (null: no lanes). Cursors re-seek, constant caches reset. RT-safe.
  void attach(const AutomationImage* image) noexcept;
  // Frames [frame, frame + len) of the block, whose samples have the ticks `ticks` (frame + len ≤ maxBlockFrames).
  // `wholeBlock`: this run is the whole block. RT-safe.
  void render(const TickSpan& ticks, uint32_t frame, uint32_t len, bool wholeBlock) noexcept;
  // Every lane constant at its value at `tick` for frames [frame, frame + len) (transport stopped). RT-safe.
  void renderHold(double tick, uint32_t frame, uint32_t len, bool wholeBlock) noexcept;

  // RT-safe inspection. numLanes() is the attached image's lane count (capped at maxLanes()).
  uint32_t numLanes() const noexcept { return image_ != nullptr ? std::min(image_->numLanes(), maxLanes_) : 0u; }
  uint32_t maxLanes() const noexcept { return maxLanes_; }
  const float* buffer(uint32_t lane) const noexcept { return buf_.data() + static_cast<size_t>(lane) * stride_; }
  // True when the last block of `lane` was one constant (buffer() then holds it in every frame).
  bool constant(uint32_t lane) const noexcept { return lanes_[lane].constValid != 0; }
  uint32_t target(uint32_t lane) const noexcept { return image_->lanes()[lane].target; }
  const AutomationImage* image() const noexcept { return image_; }

private:
  struct LaneState {
    int64_t cursor;       // index (into the lane's points) of the last point with tick ≤ the last rendered tick; −1
    float constValue;
    uint8_t constValid;   // the whole buffer holds constValue
    uint8_t pad_[3];
  };
  void fillConst(uint32_t lane, float v, uint32_t frame, uint32_t len, bool wholeBlock) noexcept;

  const AutomationImage* image_ = nullptr;
  uint32_t maxLanes_ = 0, maxBlock_ = 0;
  size_t stride_ = 0;
  AlignedBuffer<float> buf_;
  AlignedBuffer<double> exact_;  // TickSpan::exact's ticks for the current run
  std::vector<LaneState> lanes_;
};

}  // namespace pq::engine
