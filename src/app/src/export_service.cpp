// The app's Export Audio job (EX.2): pq/app/export_service.h describes it.
#include "pq/app/export_service.h"

#include <string>
#include <utility>

namespace pq::app {

namespace {

std::filesystem::path pathOfUtf8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

std::string utf8Of(const std::filesystem::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}

}  // namespace

ExportRequest exportRequestOf(const views::ExportJob& job, bool jit) {
  ExportRequest r;
  r.snapshot = job.snapshot;
  r.from = job.from;
  r.to = job.to;
  switch (job.tracks) {
    case views::ExportTracks::Master: r.what = ExportWhat::Master; break;
    case views::ExportTracks::AllTracks: r.what = ExportWhat::Stems; break;
    case views::ExportTracks::Selected: r.what = ExportWhat::Tracks; break;
  }
  r.tracks = job.trackIds;
  r.includeReturnsMaster = job.includeReturnsMaster;
  r.renderAsLoop = job.renderAsLoop;
  r.normalize = job.normalize;
  r.rate = job.sampleRate;
  switch (job.bits) {
    case views::ExportBitDepth::Pcm16: r.bits = ExportBits::Pcm16; break;
    case views::ExportBitDepth::Pcm24: r.bits = ExportBits::Pcm24; break;
    case views::ExportBitDepth::Float32: r.bits = ExportBits::Float32; break;
  }
  switch (job.dither) {
    case views::ExportDither::None: r.dither = ExportDither::None; break;
    case views::ExportDither::Tpdf: r.dither = ExportDither::Tpdf; break;
    case views::ExportDither::Shaped: r.dither = ExportDither::Shaped; break;
  }
  r.format = job.format == views::ExportFileFormat::Flac ? ExportFormat::Flac : ExportFormat::Wav;
  r.folder = pathOfUtf8(job.folder);
  r.baseName = job.baseName;
  // RB "Choices": a whole-song export renders its tail until −90 dBFS for 1 s, at most 10 s; a loop or a time
  // selection is exact (and render-as-loop has no tail).
  r.tailMaxSeconds = job.song && !job.renderAsLoop ? 10.0 : 0.0;
  r.projectDir = job.projectDir;
  r.chanceSeed = 0;  // an export is reproducible
  r.jit = jit;
  return r;
}

ExportService::ExportService(std::function<uint32_t()> engineRate, bool jit)
    : engineRate_(std::move(engineRate)), jit_(jit) {}

ExportService::~ExportService() {
  cancel_.store(true, std::memory_order_relaxed);
  if (worker_.joinable()) worker_.join();
}

uint32_t ExportService::engineSampleRate() { return engineRate_ ? engineRate_() : 48000u; }

bool ExportService::start(const views::ExportJob& job) {
  {
    std::lock_guard lock(mutex_);
    if (result_.state == views::ExportStatus::State::Running) return false;
    result_ = {};
    result_.state = views::ExportStatus::State::Running;
  }
  if (worker_.joinable()) worker_.join();  // the previous job's thread has ended (its result is in)
  progress_.store(0.0f, std::memory_order_relaxed);
  cancel_.store(false, std::memory_order_relaxed);
  worker_ = std::thread([this, request = exportRequestOf(job, jit_)] {
    auto rep = runExport(request, &progress_, &cancel_);
    views::ExportStatus end;
    if (!rep) {
      end.state = views::ExportStatus::State::Failed;
      end.message = rep.error().message;
    } else if (rep->cancelled) {
      end.state = views::ExportStatus::State::Cancelled;
    } else {
      end.state = views::ExportStatus::State::Done;
      end.progress = 1.0f;
      for (const ExportedFile& f : rep->files) end.files.push_back(utf8Of(f.path));
    }
    std::lock_guard lock(mutex_);
    result_ = std::move(end);
  });
  return true;
}

std::vector<std::string> ExportService::existingFiles(const views::ExportJob& job) {
  std::vector<std::string> out;
  for (const std::filesystem::path& f : exportFiles(exportRequestOf(job, jit_))) {
    std::error_code ec;
    if (std::filesystem::exists(f, ec)) out.push_back(utf8Of(f));
  }
  return out;
}

views::ExportStatus ExportService::status() {
  std::lock_guard lock(mutex_);
  views::ExportStatus s = result_;
  if (s.state == views::ExportStatus::State::Running) s.progress = progress_.load(std::memory_order_relaxed);
  return s;
}

void ExportService::cancel() {
  std::lock_guard lock(mutex_);
  if (result_.state == views::ExportStatus::State::Running) cancel_.store(true, std::memory_order_relaxed);
}

}  // namespace pq::app
