#pragma once

// bug-mod-ghost: the MODULATION GHOSTS' data — where live modulation has each modulated param NOW, for every host
// (the Device view, panel windows, the project patch and circuit canvases) and every control that shows a param.
//
//   source     ViewContext::modulationSource (the app: the engine's tap buses, ModulationSource::readModulationNow —
//              the latest block's last sample of each modulated param's tap, per voice the newest voice's; nothing is
//              accumulated or consumed, so the rings' sweeps are untouched).
//   targets    every param a wire drives (doc::wiresOf; a macro's mapping is the macro's knob, not modulation), with the
//              domain its tap is in (lower.h "Modulation wires": knob position, the curve's units, the DSP units),
//              rebuilt when the wire list changes (a persistent document: compared by identity).
//   refresh    at most once per UI frame whoever asks (every visible face's frame tick asks; the first in a frame
//              reads, the rest share it), and only while the source is live — plus one read after it stops, so the
//              ghosts go. serial() moves whenever a value changed: a host repaints only then (and only the controls whose
//              ghost moved: Element::liveKey). Nothing polls while idle.
//   ghost      ghostPosition(): a control's knob position `pos` moved by its param's tap now, through the control's
//              curve — the position a ghost draws. nullopt: not modulated, or the modulation has it at the knob.
//
// UI thread only. Not RT code (the audio side is one relaxed store per metered bus channel per block).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pq/document/lower.h"
#include "pq/document/model.h"
#include "pq/dsp/curve.h"
#include "pq/views/device_chain/wiring.h"

namespace pq::views {

class ViewContext;

namespace devices {
struct ParamInfo;
}

class LiveModulation {
public:
  explicit LiveModulation(ViewContext& ctx) : ctx_(ctx) {}

  // Reads the source when a frame has passed since the last read and it is live (or was lit at the last read).
  // Returns serial().
  uint64_t refresh();
  // Whether ghosts may show or move: the source is live, or the last read had any param off its knob (the read after
  // the stop clears them).
  bool live() const;
  uint64_t serial() const noexcept { return serial_; }

  // The tap of (device, param) now (its domain's delta from the knob) — nullopt: not modulated, unknown, or exactly 0.
  struct Now {
    float delta = 0.0f;
    uint8_t law = 0;  // 0 Travel wires (knob position), 1 Add wires, 2 migrated Units wires (DSP units)
  };
  std::optional<Now> now(doc::DeviceId device, std::string_view param) const;
  // Whether a wire drives (device, param) (a ghost may appear on it).
  bool modulated(doc::DeviceId device, std::string_view param) const;

  // The ghost of a control at knob position `pos` on curve `curve` (its param's ParamInfo for the DSP domain; null:
  // the curve alone). nullopt: no ghost (not modulated, or within 1e-5 of `pos`).
  std::optional<double> ghostPosition(doc::DeviceId device, std::string_view param, double pos, const dsp::Curve& curve,
                                      const devices::ParamInfo* info = nullptr) const;

  // Tests: reads of the source so far.
  uint64_t reads() const noexcept { return reads_; }
  // The minimum time between two reads (seconds): one UI frame at 120 Hz, so every face of one frame shares a read.
  static constexpr double kFrameSeconds = 1.0 / 120.0;

private:
  struct Target {
    doc::DeviceId device;
    std::string param;
    uint8_t law = 0;
  };
  void syncTargets() const;
  const Target* find(doc::DeviceId device, std::string_view param, size_t* index = nullptr) const;

  ViewContext& ctx_;
  mutable const doc::WireList* wires_ = nullptr;
  mutable doc::Snapshot held_;  // (holds the wire list)
  mutable std::vector<Target> targets_;
  mutable std::vector<ModTarget> query_;
  std::vector<ModSweep> values_;  // per target of valuesFor_ (the last read)
  const doc::WireList* valuesFor_ = nullptr;
  double lastRead_ = -1.0;
  bool lit_ = false;
  uint64_t serial_ = 0, reads_ = 0;
};

}  // namespace pq::views
