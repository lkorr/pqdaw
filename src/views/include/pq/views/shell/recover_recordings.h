#pragma once

// M6 review L13: at start, recordings left by an earlier run that ended without cleaning up (a crash: its untitled
// project's Unsaved folder, ViewContext::staleRecordings) are offered in a small popover over the workspace, one
// folder at a time (oldest first):
//   Open as Project   a new untitled project (after the usual "save changes?") holding the takes one after another on
//                     an audio track "Recovered" (ViewContext::recoverStaleRecordings: the files are moved into its own
//                     folder, so its first save moves them into the project)
//   Show in Explorer  the folder (the popover stays)
//   Discard           deletes the folder (the only path that deletes anything)
//   Later             closes; offered again at the next start
// Esc is Later. UI thread only.

#include <string>
#include <vector>

#include "pq/ui/overlay.h"
#include "pq/views/view_context.h"

namespace pq::views {

class Shell;

class RecoverRecordingsPopover : public ui::Popover {
public:
  static constexpr float kWidth = 460.0f, kHeight = 132.0f;  // logical px

  RecoverRecordingsPopover(ViewContext& ctx, std::vector<ViewContext::StaleRecordings> stale);
  const ViewContext::StaleRecordings& current() const { return stale_.front(); }
  size_t remaining() const noexcept { return stale_.size(); }
  // The buttons' actions (tests may call them; the buttons do).
  void openAsProject();
  void reveal();
  void discard();
  void later();
  ui::InfoText info() const override;

private:
  void next();  // the next folder, or close
  void showCurrent();

  ViewContext& ctx_;
  std::vector<ViewContext::StaleRecordings> stale_;
  ui::Label* line1_ = nullptr;
  ui::Label* line2_ = nullptr;
};

// Opens the popover over `shell` when earlier runs left recordings; null when there are none.
RecoverRecordingsPopover* offerRecordingRecovery(Shell& shell);

}  // namespace pq::views
