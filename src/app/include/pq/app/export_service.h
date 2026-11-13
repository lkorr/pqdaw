#pragma once

// The app's Export Audio job (EX.2): views::ExportPort over runExport (pq/app/export_render.h). start() maps the
// window's ExportJob onto an ExportRequest (a whole-song range gets the tail: tailMaxSeconds 10, RB "Choices") and runs
// runExport on its own worker thread, so the UI thread never waits for a render; status() reads the worker's progress
// (an atomic) and, once it ended, its result (under a mutex). cancel() sets the flag runExport polls between chunks.
// One job at a time. The destructor cancels a running job and joins its thread (the job deletes its temp files).
//
// The port's methods are UI-thread calls; the worker only touches the atomics and, at its end, the result.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "pq/app/export_render.h"
#include "pq/views/export/export_port.h"

namespace pq::app {

// The request runExport gets for `job` (tests). `jit` as ExportRequest::jit.
ExportRequest exportRequestOf(const views::ExportJob& job, bool jit);

class ExportService final : public views::ExportPort {
public:
  // `engineRate`: the engine's sample rate now (the window's default). `jit` false: interpreter images (tests).
  explicit ExportService(std::function<uint32_t()> engineRate, bool jit = true);
  ~ExportService() override;
  ExportService(const ExportService&) = delete;
  ExportService& operator=(const ExportService&) = delete;

  uint32_t engineSampleRate() override;
  bool start(const views::ExportJob& job) override;
  views::ExportStatus status() override;
  void cancel() override;

private:
  std::function<uint32_t()> engineRate_;
  bool jit_;
  std::thread worker_;
  std::atomic<float> progress_{0.0f};
  std::atomic<bool> cancel_{false};
  std::mutex mutex_;
  views::ExportStatus result_;  // Running while the worker runs (under mutex_)
};

}  // namespace pq::app
