#pragma once

// Per-device CPU cost for the project patch view's labels (T4.13; QUESTIONS Q134 item 8). Plain data in and out: the
// app implements it over one engine::CostMeter per unit schedule and the region → device attribution of T4.5
// (pq/app/project_link.h); the views never see the engine. FixedDeviceCostSource serves tests and scenes.
//
// Metering is off until a view asks for it (setCostMetering): with it off the images only load a flag per block
// (T4.5, bench/RESULTS.md); on, every region is timed (+15–40 % on small units while shown).

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "pq/document/ids.h"

namespace pq::views::project_patch {

class DeviceCostSource {
public:
  virtual ~DeviceCostSource() = default;
  virtual void setCostMetering(bool on) = 0;
  // True while readings may change (the engine runs). The view polls only then, and once more after.
  virtual bool costLive() const noexcept = 0;
  // ns per sample frame of each device (its whole instance) over the time since the previous read; < 0: unknown (not
  // lowered, no reading yet).
  virtual void readDeviceCosts(std::span<const doc::DeviceId> devices, std::span<float> nsPerFrame) = 0;
};

class FixedDeviceCostSource final : public DeviceCostSource {
public:
  void set(doc::DeviceId device, float ns) {
    for (auto& [d, v] : costs_)
      if (d == device) {
        v = ns;
        return;
      }
    costs_.emplace_back(device, ns);
  }
  void setLive(bool live) noexcept { live_ = live; }
  bool metering() const noexcept { return metering_; }
  void setCostMetering(bool on) override { metering_ = on; }
  bool costLive() const noexcept override { return live_; }
  void readDeviceCosts(std::span<const doc::DeviceId> devices, std::span<float> out) override {
    ++reads_;
    for (size_t i = 0; i < devices.size() && i < out.size(); ++i) {
      out[i] = -1.0f;
      for (const auto& [d, v] : costs_)
        if (d == devices[i]) out[i] = metering_ ? v : -1.0f;
    }
  }
  uint64_t reads() const noexcept { return reads_; }

private:
  std::vector<std::pair<doc::DeviceId, float>> costs_;
  bool live_ = false, metering_ = false;
  uint64_t reads_ = 0;
};

}  // namespace pq::views::project_patch
