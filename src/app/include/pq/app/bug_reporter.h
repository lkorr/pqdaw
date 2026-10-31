#pragma once

// F12 in the DAW window (T5.x-bugreport; the files and the panel: pq/views/bug_report/). The window loop handles F12
// before the widget tree (so it works whatever has focus: a text field, an open menu) and calls onKey:
//
//   F12          panel closed: reads back the last presented frame (Swapchain::readLastFrame: exactly what is on
//                screen), captures the context at that instant (shotContext; at the first shot of a draft also
//                firstShotContext: build, system, GPU, memory, project, audio, the recent input, status messages
//                ), adds the shot to the draft and opens the panel. Panel open: closes it, keeping the draft.
//   Shift+F12    while collecting (a draft exists, the panel is hidden: after Add another or Esc): back to the panel
//                without a new shot; otherwise as F12.
//
// The app keeps running behind the panel (the transport is not stopped). Submit writes the draft on a worker thread
// (PNG encoding) and names the folder in the status line. Nothing here runs per frame: recordInput is a few stores per
// input event, the status ring an append per status change.
//
// PQ_BUGREPORT_SCRIPT (a scripted run; see runScript): steps separated by ';', one every 0.6 s after the first frame:
// `f12`, `shift+f12`, `move:x,y` / `click:x,y` (device px: synthetic SDL events), `text:...`, `tag:t`, `ticket:i`,
// `suggestions`, `bugs`, `add`, `close`, `submit`, `discard`, `quit`.

#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "pq/core/result.h"
#include "pq/ui/image.h"
#include "pq/ui/input.h"
#include "pq/views/bug_report/bug_panel.h"
#include "pq/views/bug_report/bug_report.h"
#include "pq/views/bug_report/input_log.h"
#include "pq/views/view_context.h"

namespace pq::views {
class Shell;
}

namespace pq::app {

class ProjectLink;
class AudioDeviceManager;

// What the window loop knows about the window and the frame a shot came from.
struct BugWindowInfo {
  uint32_t width = 0, height = 0;  // device px
  float uiScale = 1.0f;
  float pixelDensity = 1.0f;
  ui::Vec2 pointer{0, 0};  // device px (the last pointer event's position)
  bool pointerInside = false;
  std::string gpu;          // device name and driver
  std::string presentMode;
  double frameBeginMs = 0, frameBuildMs = 0, frameEndMs = 0;  // the last frame's parts (WindowApp::lastTiming)
  uint32_t framesPresented = 0;
  double readbackMs = 0;    // reading the frame back for this shot
};

// Where reports go: $PQ_BUG_REPORTS_DIR; else <checkout>/bug_reports when the executable lies inside a source checkout
// (a directory above it with SPEC.md and src/app; the user runs build/release/src/app/pqdaw.exe); else the checkout the
// executable was built from, if it still exists; else %LOCALAPPDATA%\pqdaw\bug_reports.
std::filesystem::path bugReportRoot();

class BugReporter {
public:
  BugReporter(views::Shell& shell, ProjectLink* link, AudioDeviceManager* audio, std::filesystem::path root);
  ~BugReporter();  // waits for a write in progress
  BugReporter(const BugReporter&) = delete;
  BugReporter& operator=(const BugReporter&) = delete;

  // Whether F12 (shift: Shift+F12) should read the frame back now.
  bool wantsShot(bool shift) const;
  // The key: `frame` is the read-back frame when wantsShot said so (an error otherwise).
  void onKey(bool shift, Result<ui::Image> frame, const BugWindowInfo& info);
  // After the tree dispatched an input event (UI thread).
  void recordInput(const ui::InputEvent& e) noexcept;

  bool panelOpen() const;
  bool collecting() const noexcept { return !draft_.empty() && !panelOpen(); }
  views::bugreport::Draft& draft() noexcept { return draft_; }
  views::bugreport::BugReportPanel* panel() const;
  const std::filesystem::path& root() const noexcept { return root_; }

  // The panel's buttons (also the script's steps).
  void submit();
  void addAnother();
  void discard();
  void close();
  bool writing() const noexcept { return worker_.joinable() && !written_; }
  void waitForWrite();
  // The folder the last submit wrote ("" before; the error message instead when it failed).
  const std::string& lastWritten() const noexcept { return lastWritten_; }
  const std::string& lastError() const noexcept { return lastError_; }

  // The context lines (UI thread; F12 only).
  std::string shotContext(const BugWindowInfo& info);
  std::string firstShotContext(const BugWindowInfo& info);
  double now() const noexcept;

private:
  void openPanel();
  void onWritten(Result<views::bugreport::WriteResult> r);

  views::Shell& shell_;
  views::ViewContext& ctx_;
  ProjectLink* link_;
  AudioDeviceManager* audio_;
  std::filesystem::path root_;
  views::bugreport::Draft draft_;
  views::bugreport::InputLog input_;
  uint64_t inputAtLastShot_ = 0;
  std::string firstInput_;  // the recent input at the draft's first shot (an appended ticket gets it in its shot)
  struct Status {
    double t;
    std::string text;
  };
  std::deque<Status> statuses_;
  views::ViewContext::Subscription statusSub_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
  std::thread worker_;
  bool written_ = false;
  std::string lastWritten_, lastError_;
  std::shared_ptr<BugReporter*> alive_;
};

}  // namespace pq::app
