#pragma once

// Export render core (EX.1; docs/plans/RB.md D7, "### EX.1"). Exports render through a second, PRIVATE ProjectLink
// built from a snapshot at the target sample rate (no sample-rate conversion), on the caller's thread (the dialog's
// worker, EX.2): playback and editing on the user's link continue meanwhile. Not RT-safe; never the audio thread.
//
//   makeOfflineLink   the private link: synchronous compiles, no idle gate, the JIT on and waited for (an export or a
//                     --render plays only JIT images, deterministically)
//   renderPass        ONE offline pass over a tick range on such a link: the master output and any buses of the layout
//                     (a track's post-fader "<id>:out", FZ.2's own-contribution bus) into caller sinks, chunk by chunk,
//                     each PDC-trimmed to transport time; render-as-loop and the whole-song tail. The reusable core
//                     (FZ.2 renders a track's contribution with it).
//   runExport         the Export Audio job: passes (one for the master or for stems without master FX; one solo pass
//                     per track with them), 32f temp files, then normalize / dither / quantize into the final files.
//
// Pass timing (all in frames at the link's rate): the range is N = samples(to) − samples(from) (the tempo map's own
// tick → sample rounding). Each pass first settles (ProjectLink::settleOffline: transport stopped until the program is
// quiet: earlier passes' tails gone, pending JIT images and the PDC plan taken), then plays from `from`. A target's
// stream is late by its latency — the master by outputLatency(), a bus by its L(b) (busLatency(): with Track Delay
// it can be negative, i.e. early) — so its file frame k is stream frame k + lap + latency (a negative start reads as
// zeros): a click on the downbeat through a latent device lands at frame 0. Render-as-loop plays two laps with the
// transport looping over [from, to) and keeps the second (its frame 0 continues the first lap's tail). A tail (> 0)
// renders past N until every target has stayed below −90 dBFS for a second (at most the tail): the files end at the
// last frame above −90 dBFS (at least N).
//
// Files (runExport): `<folder>/<baseName>.wav` for the master; `<folder>/<baseName> <track name>.wav` per track (names
// made safe for the file system, duplicates numbered). Pass 1 writes 32-bit float temp files next to them; pass 2
// applies the normalize gain (each file's peak to −0.3 dBFS), TPDF dither (±1 LSB triangular) or 3-tap shaped dither
// (error feedback, h = 1.623, −0.982, 0.109: noise moved above ~10 kHz) for integer formats, quantizes and writes
// `.part` files; only when all of them are complete are they renamed onto the final names (replacing existing files).
// A cancel or an error deletes every temp and part file: nothing new is left. Solo is cleared in the export's copy of
// the document (mute is respected). A stem with master FX is a pass on a fresh private link of the document with that
// track soloed (Q?-EX.1-a: a solo param write on the previous pass's link left its first ~20 ms depending on that
// pass's engine state); the previous link lives until the new one has compiled, so the JIT images are shared.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/core/result.h"
#include "pq/core/time.h"
#include "pq/document/model.h"
#include "pq/engine/offline_render.h"
#include "pq/engine/project_program.h"

namespace pq::app {

// ---- The core (FZ.2 reuses it) ---------------------------------------------------------------------------------

// What every offline render settles for before it plays (ProjectLink::settleOffline).
inline constexpr double kSettleMinSeconds = 0.05;  // the PDC plan's 10 ms crossfade, a few JIT swaps
inline constexpr double kSettleMaxSeconds = 30.0;  // a drone never goes quiet: it is rendered from here on
inline constexpr float kTailFloor = 3.1622777e-5f; // −90 dBFS: a tail below it for kTailQuietSeconds has ended
inline constexpr double kTailQuietSeconds = 1.0;

// A private link for offline renders of `snapshot` at `rate` (8 kHz … 768 kHz): compiled on the calling thread, no
// idle gate, the JIT on and waited for (error when it does not finish within a minute; `jit` false: interpreter images
// only — tests, where the JIT's compile time dominates a short render). `projectDir` resolves project-relative sample
// paths; `chanceSeed` as LinkConfig.
Result<std::unique_ptr<ProjectLink>> makeOfflineLink(const doc::Snapshot& snapshot, double rate,
                                                     const std::string& projectDir, uint64_t chanceSeed = 0,
                                                     bool jit = true);

struct PassTarget {
  uint32_t bus = engine::kNoBus;  // a bus of the link's layout (e.g. trackOutBus(id)); kNoBus: the master output
  engine::AudioSink* sink = nullptr;  // begin(channels, rate, upper bound) → write()* → end(), on the render thread
};

struct PassSpec {
  Ticks from = 0, to = 0;       // the range [from, to), to > from
  bool loop = false;            // render as loop (see the file comment)
  double tailMaxSeconds = 0.0;  // > 0 (and not loop): the tail (see the file comment)
  std::vector<PassTarget> targets;
  const std::atomic<bool>* cancel = nullptr;  // polled between chunks
  std::function<void(uint64_t done, uint64_t total)> progress;  // stream frames, between chunks
};

struct PassTargetStats {
  uint64_t frames = 0;   // written to its sink (≥ the range unless cancelled; with a tail up to a second of quiet more)
  uint64_t loudEnd = 0;  // 1 + the last frame above −90 dBFS (0: none)
  float peak = 0.0f;     // max |x| over what was written
};

struct PassResult {
  uint64_t rangeFrames = 0;  // N
  std::vector<PassTargetStats> targets;
  bool cancelled = false;
};

// One pass on `link` (made by makeOfflineLink, or any link no device drives; `project` = the document it plays, for
// the tempo map). Ends every target's sink (also when cancelled). Errors: an empty range, no targets, a null sink, a
// bus the layout does not have, a render error.
Result<PassResult> renderPass(ProjectLink& link, const doc::Project& project, const PassSpec& spec);

// ---- Export Audio ------------------------------------------------------------------------------------------------

enum class ExportWhat : uint8_t { Master, Stems /* every active track but the master */, Tracks /* `tracks` */ };
enum class ExportBits : uint8_t { Pcm16, Pcm24, Float32 };
enum class ExportDither : uint8_t { None, Tpdf, Shaped };  // integer formats only
enum class ExportFormat : uint8_t { Wav };                 // EX.3 adds Flac

struct ExportRequest {
  doc::Snapshot snapshot;
  Ticks from = 0, to = 0;  // song: 0 → the last clip's end (with a tail); loop / time selection: exact
  ExportWhat what = ExportWhat::Master;
  std::vector<doc::TrackId> tracks;  // ExportWhat::Tracks
  bool includeReturnsMaster = false; // stems: one solo pass per track through the returns and the master
  bool renderAsLoop = false;
  bool normalize = false;
  uint32_t rate = 48000;
  ExportBits bits = ExportBits::Pcm24;
  ExportDither dither = ExportDither::Tpdf;
  ExportFormat format = ExportFormat::Wav;
  std::filesystem::path folder;
  std::string baseName;              // UTF-8
  double tailMaxSeconds = 0.0;       // whole-song exports: 10 (RB "Choices")
  std::string projectDir;            // resolves project-relative sample paths
  uint64_t chanceSeed = 0;
  bool jit = true;                   // makeOfflineLink's `jit` (false: tests)
};

struct ExportedFile {
  std::filesystem::path path;
  doc::TrackId track{};  // {} for the master
  float peakDb = -200.0f;  // before normalizing
  float gainDb = 0.0f;     // the normalize gain applied
};

struct ExportReport {
  std::vector<ExportedFile> files;  // empty when cancelled
  uint64_t frames = 0;              // per file
  uint32_t passes = 0;
  bool cancelled = false;
};

// The export job (see the file comment). `progress` (0 … 1) and `cancel` may be null; the caller's thread runs it.
// Errors: a bad request (empty range, rate, no tracks, a track not in the project, no folder or name), a compile or
// render error, a file error; in every error and on cancel no new file is left behind.
Result<ExportReport> runExport(const ExportRequest& request, std::atomic<float>* progress = nullptr,
                               const std::atomic<bool>* cancel = nullptr);

}  // namespace pq::app
