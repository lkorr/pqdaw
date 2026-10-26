#pragma once

// Where the circuit editor's compiled patch plays (T4.4). Plain data out of the views (which never include the
// engine): the editor hands every schedule that compiled cleanly to the player; the app's implementation
// (pq/app/patch_preview.h: a ProgramHost on the audio output) builds the image off the audio thread and publishes it
// through the lock-free hot-swap path (T1.9: state transfer by state key, a 5–20 ms linear crossfade while audible).
// A schedule that never arrives (a parse or check error) leaves the last good program playing.
//
// T4.4-fix1: a device in a track's chain is edited by the same PatchEditor; there the document plays it
// (PatchEditor::onCommitted hands each new text to a `SetDevicePatch` command and the project's per-track compile swaps
// the unit), and the player is EXTERNAL (external() below: app::DevicePlayer): it carries only the probes and the cost
// of the device as the project plays it.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "pq/core/probe_level.h"
#include "pq/core/result.h"
#include "pq/core/tap_ring.h"
#include "pq/graph/infer.h"
#include "pq/graph/inspect.h"
#include "pq/graph/schedule.h"

namespace pq::views::circuit {

// What a player plays (the last schedule handed to it), for reading its cost: the schedule and, per node of the
// scheduled patch, where it was written — kept as NodeIds, so it stays valid while the text changes. (T4.5; T4.4-fix1:
// moved here from PatchEditor so an external player — a project unit — can give its own.)
struct PlayedProgram {
  uint64_t generation = 0;                  // PatchPlayer::generation() after it was played
  graph::Schedule schedule;
  std::vector<uint32_t> instanceParent;     // per instance (TypedPatch::instances): its caller (kNone: the root)
  std::vector<graph::NodeId> instanceCall;  // … and the call node in the caller
  std::vector<uint32_t> nodeInstance;       // per node of the scheduled patch: its instance
  std::vector<graph::NodeId> nodeSource;    // … the node it comes from in that instance's patch (invalid: none)
  std::vector<graph::NodeOrigin::What> nodeWhat;
  std::vector<uint32_t> probeIds;           // the probes that have a value (Schedule::probes ids)
};

class PatchPlayer {
public:
  virtual ~PatchPlayer() = default;
  // The rate schedules must be compiled at (OptimiseOptions / ScheduleOptions::sampleRate).
  virtual double sampleRate() const = 0;
  // Starts playing `schedule` (`scheduled` = the TypedPatch its NodeIndices refer to: Compiled::scheduled()), or hot
  // swaps the playing program for it. An error leaves whatever played before playing.
  virtual Status play(const graph::Schedule& schedule, const graph::TypedPatch& scheduled) = 0;
  virtual void stop() = 0;
  virtual bool playing() const = 0;
  // The state layout of the program published last (ScheduleOptions::previous: unchanged state keeps its offsets,
  // so the swap's transfer is a few coalesced copies), or null.
  virtual const graph::StateLayout* stateLayout() const = 0;

  // T4.6 (device panels): writes a root `param` of the playing program by name, in its internal units (a dB param takes
  // a linear gain); no recompile. Lock-free hand-over to the audio thread (applied at the next block; a hot swap carries
  // it by param key). False when nothing plays, the name is not a param of what plays, or the write queue is full.
  virtual bool setParam(std::string_view /*name*/, float /*value*/) { return false; }
  // T4.6: the analysis tap of the played output (stereo; SPEC §11.1), or null. Owned by the player; lives as long as it.
  virtual TapRing* outputTap() noexcept { return nullptr; }

  // ---- T4.5: live probes and cost meters (pq/graph/inspect.h; the engine's half: pq/engine/inspect.h) --------------
  // The editor compiles probes into the schedules it plays (ScheduleOptions::probes, with ids it chooses). The player
  // binds every probe id of a schedule it plays to a ProbeLevel it owns, and the ids named here (call before play())
  // also to a TapRing (the full signal: a mini-scope), and keeps those taps for as long as the id stays in the
  // schedules it plays. Taps are owned by the player and outlive any program that writes them.
  virtual void setProbeSignals(std::span<const uint32_t> /*ids*/) {}
  // The full signal of probe `id` of the playing program (mono, written block by block; bools as lane masks: sign bit
  // set = high), or null. The player is the ring's registered reader while it is bound: read it from the UI thread.
  virtual TapRing* probeSignal(uint32_t /*id*/) noexcept { return nullptr; }
  // Probe `id`'s per-block level (last / min / max; bools 0 / 1), or null.
  virtual const ProbeLevel* probeLevel(uint32_t /*id*/) const noexcept { return nullptr; }
  // Cost meters: measured only while on (pq/engine/inspect.h).
  virtual void setMetering(bool /*on*/) {}
  struct CostReading {
    uint64_t generation = 0;               // which play() the measured program came from (see generation())
    double totalNsPerFrame = 0.0;          // the whole program, ns per sample frame
    std::vector<double> regionNsPerFrame;  // per region of that play()'s schedule
    std::vector<uint32_t> units;           // per region: the first region of the unit it was timed in (a JIT chain)
  };
  // The cost since the previous reading; false while there is none yet (just enabled, a new program).
  virtual bool readCost(CostReading& /*out*/) { return false; }
  // Counts play() calls that published a program.
  virtual uint64_t generation() const noexcept { return 0; }

  // ---- T4.4-fix1: an EXTERNAL player — the patch plays elsewhere (a device of the project, in the unit of its track):
  // the editor still checks every edit (its diagnostics) but hands the player no schedule (play() only says the editor
  // follows it); probes go to setProbeTargets (paths from the played patch, as ProbeTarget), and the cost readings
  // belong to the player's own program (playedProgram), in which the played patch is the instance at pathPrefix().
  virtual bool external() const { return false; }
  virtual void setProbeTargets(std::span<const graph::ProbeTarget> /*targets*/, std::span<const uint32_t> /*ids*/) {}
  virtual const PlayedProgram* playedProgram() const { return nullptr; }
  virtual std::vector<graph::NodeId> pathPrefix() const { return {}; }
};

}  // namespace pq::views::circuit
