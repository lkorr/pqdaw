#pragma once

// The circuit editor on a device of the project, played by the project (T4.4-fix1; SPEC §15 2026-10-26): T4.5's live
// probes and cost meters read the device where it plays — inside the unit of its track — not a separate preview.
//
//   DeviceInspector   owned by ProjectLink. The inspected track's unit is compiled with the editor's probes (paths from
//                     the device patch, the device's call node and the track's in front: engine::TrackInspect) and its
//                     instance tree (engine::UnitInspect); every image of that unit — the interpreter's at once, the
//                     JIT's when it arrives — passes through ProjectCompileOptions::onBind, where this binds the probe
//                     taps (fixed pools, as app::PatchPreview: 8 rings of 16384 frames, 256 levels; a tap freed by a
//                     probe that went away is reused after 1 s) and the cost meter (tagged with the schedule's
//                     generation, so a reading is attributed to the program it measured).
//   DevicePlayer      implements views::circuit::PatchPlayer as an EXTERNAL player over ProjectLink for one device: the
//                     editor's probe targets → ProjectLink::setDeviceInspect (a recompile of that unit only; the audio is
//                     bit-identical with or without probes), its reads → the inspector; stop() / its destruction → no
//                     inspection (the unit is recompiled without probes, so a closed editor costs nothing).
// Cost: nothing for units that are not inspected (their images are bound to nothing); the inspected unit pays T4.5's
// probe copies and, while Cost is on, its region timing.
// Threads: bind() on the compile path / the JIT worker (under the link's compile mutex), everything else on the UI
// thread; the taps and the meter are written on the audio thread (RT-safe, pq/engine/inspect.h).

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "pq/core/probe_level.h"
#include "pq/core/tap_ring.h"
#include "pq/document/ids.h"
#include "pq/engine/inspect.h"
#include "pq/engine/project_compile.h"
#include "pq/views/circuit/patch_player.h"

namespace pq::app {

class ProjectLink;

class DeviceInspector {
public:
  DeviceInspector();
  ~DeviceInspector();
  DeviceInspector(const DeviceInspector&) = delete;
  DeviceInspector& operator=(const DeviceInspector&) = delete;

  // Compile path / JIT worker: binds the taps and the meter to `image` when `cu` is an inspected unit.
  void bind(const engine::CompiledUnit& cu, engine::ProgramImage& image);
  // UI thread.
  void setSignals(std::span<const uint32_t> ids);
  TapRing* probeSignal(uint32_t id) noexcept;
  const ProbeLevel* probeLevel(uint32_t id) const noexcept;
  void setMetering(bool on) noexcept { meter_.setEnabled(on); }
  bool readCost(views::circuit::PatchPlayer::CostReading& out);
  // The inspected unit's program as the editor attributes cost over it (null before the first inspected compile), its
  // generation, and the track it inspects.
  std::shared_ptr<const views::circuit::PlayedProgram> played() const;
  uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }
  uint32_t trackId() const noexcept { return trackId_.load(std::memory_order_acquire); }

  static constexpr uint32_t kProbeRings = 8;
  static constexpr uint32_t kProbeRingFrames = 16384;
  static constexpr uint32_t kProbeLevels = 256;
  static constexpr double kProbeTapRest = 1.0;

private:
  struct TapSlot {
    uint32_t id = 0;
    bool bound = false;
    double freedAt = -1e9;
  };
  void bindProbes(const graph::Schedule& schedule);  // under mutex_

  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<TapRing>> rings_;
  std::unique_ptr<ProbeLevel[]> levels_;
  std::vector<TapSlot> ringSlots_, levelSlots_;
  std::vector<uint32_t> wantSignals_;
  std::vector<engine::ProbeTap> binding_;
  const graph::Schedule* lastSchedule_ = nullptr;
  std::shared_ptr<const graph::Schedule> lastHold_;  // keeps lastSchedule_'s address from being reused
  std::shared_ptr<const views::circuit::PlayedProgram> played_;
  engine::CostMeter meter_;
  std::atomic<uint64_t> generation_{0};
  std::atomic<uint32_t> trackId_{0};
};

class DevicePlayer final : public views::circuit::PatchPlayer {
public:
  DevicePlayer(ProjectLink& link, doc::DeviceId device);
  ~DevicePlayer() override;  // inspection off
  DevicePlayer(const DevicePlayer&) = delete;
  DevicePlayer& operator=(const DevicePlayer&) = delete;

  double sampleRate() const override;
  Status play(const graph::Schedule& schedule, const graph::TypedPatch& scheduled) override;
  void stop() override;
  bool playing() const override { return true; }  // the project plays it (or would, with the transport running)
  const graph::StateLayout* stateLayout() const override { return nullptr; }
  void setProbeSignals(std::span<const uint32_t> ids) override;
  TapRing* probeSignal(uint32_t id) noexcept override;
  const ProbeLevel* probeLevel(uint32_t id) const noexcept override;
  void setMetering(bool on) override;
  bool readCost(CostReading& out) override;
  uint64_t generation() const noexcept override;
  bool external() const override { return true; }
  void setProbeTargets(std::span<const graph::ProbeTarget> targets, std::span<const uint32_t> ids) override;
  const views::circuit::PlayedProgram* playedProgram() const override;
  std::vector<graph::NodeId> pathPrefix() const override;

private:
  ProjectLink& link_;
  doc::DeviceId device_;
  mutable std::shared_ptr<const views::circuit::PlayedProgram> hold_;
};

}  // namespace pq::app
