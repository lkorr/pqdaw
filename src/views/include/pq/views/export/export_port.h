#pragma once

// Export Audio (EX.2, docs/plans/RB.md "### EX.2"): the plain-data port between the Export Audio window (views,
// pq/views/export/export_window.h) and the app's export job (app: pq/app/export_service.h over app::runExport), like
// AudioPrefsPort for the preferences: views never include app or engine. The app runs each job on a worker thread;
// the window polls status() while a job runs. Tests and the view scenes use FakeExportPort (below).
//
// ExportPrefs are the window's choices, remembered in the user settings (ViewPrefs::exportAudio, the settings file's
// "views" → "export" object).
//
// UI thread only (the port's methods are UI-thread calls; the job's thread is the app's business).

#include <cstdint>
#include <string>
#include <vector>

#include "pq/core/time.h"
#include "pq/document/model.h"

namespace pq::views {

enum class ExportTracks : uint8_t { Master, AllTracks /* every active track but the master */, Selected };
enum class ExportRange : uint8_t { Song, Loop, TimeSelection };
enum class ExportBitDepth : uint8_t { Pcm16, Pcm24, Float32 };
enum class ExportDither : uint8_t { None, Tpdf, Shaped };  // integer depths only
enum class ExportFileFormat : uint8_t { Wav, Flac };       // EX.3: FLAC is 16 or 24-bit (no Float32)

// The window's choices (remembered between runs). sampleRate 0 = the engine's rate.
struct ExportPrefs {
  ExportTracks tracks = ExportTracks::Master;
  ExportRange range = ExportRange::Song;
  bool includeReturnsMaster = false;
  bool renderAsLoop = false;
  ExportFileFormat format = ExportFileFormat::Wav;
  uint32_t sampleRate = 0;
  ExportBitDepth bits = ExportBitDepth::Pcm24;
  ExportDither dither = ExportDither::Tpdf;
  bool normalize = false;
  std::string folder;  // UTF-8; "" = the project's folder (else the User Library) until one is chosen
  bool operator==(const ExportPrefs&) const = default;
};

// One export, as the window asks for it (the app maps it onto app::ExportRequest).
struct ExportJob {
  doc::Snapshot snapshot;  // the document at the press of Export
  Ticks from = 0, to = 0;  // [from, to), to > from
  bool song = false;       // the whole song: the app adds the tail (until −90 dBFS for 1 s, at most 10 s)
  ExportTracks tracks = ExportTracks::Master;
  std::vector<doc::TrackId> trackIds;  // ExportTracks::Selected (row order)
  bool includeReturnsMaster = false;
  bool renderAsLoop = false;
  bool normalize = false;
  uint32_t sampleRate = 48000;
  ExportBitDepth bits = ExportBitDepth::Pcm24;
  ExportDither dither = ExportDither::Tpdf;  // None for Float32
  ExportFileFormat format = ExportFileFormat::Wav;
  std::string folder;    // UTF-8
  std::string baseName;  // UTF-8: the master's file name; the stems' prefix
  std::string projectDir;  // resolves project-relative sample paths ("" untitled)
};

struct ExportStatus {
  enum class State : uint8_t { Idle, Running, Done, Failed, Cancelled };
  State state = State::Idle;
  float progress = 0.0f;           // 0 … 1 while Running
  std::vector<std::string> files;  // Done: the files written (UTF-8 paths)
  std::string message;             // Failed: why
  bool operator==(const ExportStatus&) const = default;
};

class ExportPort {
public:
  virtual ~ExportPort() = default;
  // The engine's sample rate (the Sample Rate row's default).
  virtual uint32_t engineSampleRate() = 0;
  // Starts `job` in the background; false while another one runs.
  virtual bool start(const ExportJob& job) = 0;
  // The running job's progress, or how the last one ended (Idle before the first).
  virtual ExportStatus status() = 0;
  // Asks the running job to stop: it ends Cancelled with no new file left (a no-op when none runs).
  virtual void cancel() = 0;
};

// A scripted port: start() records the job and reports Running; the test moves it on (progress, finish, fail).
class FakeExportPort final : public ExportPort {
public:
  uint32_t rate = 48000;
  std::vector<ExportJob> started;
  ExportStatus now;
  int cancels = 0;

  uint32_t engineSampleRate() override { return rate; }
  bool start(const ExportJob& job) override {
    if (now.state == ExportStatus::State::Running) return false;
    started.push_back(job);
    now = {};
    now.state = ExportStatus::State::Running;
    return true;
  }
  ExportStatus status() override { return now; }
  void cancel() override {
    if (now.state == ExportStatus::State::Running) ++cancels;
  }
  void setProgress(float p) { now.progress = p; }
  void finish(std::vector<std::string> files) {
    now = {};
    now.state = ExportStatus::State::Done;
    now.files = std::move(files);
  }
  void fail(std::string message) {
    now = {};
    now.state = ExportStatus::State::Failed;
    now.message = std::move(message);
  }
  void endCancelled() {
    now = {};
    now.state = ExportStatus::State::Cancelled;
  }
};

}  // namespace pq::views
