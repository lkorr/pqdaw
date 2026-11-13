#pragma once

// The Export Audio window (EX.2, docs/plans/RB.md "### EX.2"; Live 12's File → Export Audio/Video), over the app's
// ExportPort (pq/views/export/export_port.h). File → Export Audio… and Ctrl+Shift+R (file.exportAudio) open it:
//
//   ┌ Export Audio ─────────────────────────────────────────────── [Close] ┐
//   │ Rendering                                                           │
//   │   Rendered Track                [Master                    ▾]       │
//   │   Range                         [Song                      ▾]       │
//   │                                 Start 1. 1. 1 · Length 8. 0. 0      │
//   │   Include Return and Master Effects [On]                            │  (tracks only)
//   │   Render as Loop                [On]                                │
//   │ Encoding                                                            │
//   │   File Type                     [WAV ▾]                             │  (EX.3 adds FLAC)
//   │   Sample Rate                   [48000 ▾]                           │  (default: the engine's)
//   │   Bit Depth                     [24 ▾]                              │
//   │   Dither                        [TPDF ▾]                            │  (16 / 24 bit only)
//   │   Normalize                     [On]                                │
//   │ Output                                                              │
//   │   Folder                        D:\Music\Exports         [Browse…]  │
//   │   File Name                     [My Song                    ]       │
//   │ ─────────────────────────────────────────────────────────────────── │
//   │ [██████████████░░░░░░░░░░░░░░░░ 45 %]                     [Export]  │  (exporting: [Cancel])
//   │ Exported My Song.wav to D:\Music\Exports                            │
//   └─────────────────────────────────────────────────────────────────────┘
//
// Rendered Track: the master (one file "<name>.wav"), every track (All Individual Tracks: one post-fader file per
// active track, "<name> <track>.wav"), or the arrangement's selected tracks at the time the window opened (the master
// row left out). Range: the song (bar 1 to the end of the last clip, plus the release tail: until −90 dBFS for 1 s, at
// most 10 s), the loop brace, or the arrangement's time selection; the readout shows its start and length. The rows
// that do not apply are disabled (no time selection, no selected track, Include Return and Master Effects for the
// master, Dither for 32-bit float); Export is disabled with the reason in the done line while the choices cannot
// export.
//
// Every choice is remembered at once (ViewPrefs::exportAudio → the app's settings file), except the file name (the
// project's name each time). Export hands an ExportJob to the port; while it runs the rows are disabled, a progress bar
// replaces the done line and Cancel replaces Export. The window polls the port on a ~30 Hz timer while a job runs and
// not otherwise: closed, it does not exist; open and idle, it has no timer. Closing it does not stop a running export:
// the host keeps polling until the job ends and says how it ended in the status line; reopened, the window shows the
// job's progress or result.
//
// UI thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pq/core/grid.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/widget.h"
#include "pq/views/export/export_port.h"
#include "pq/views/view_context.h"

namespace pq::views {

class Shell;

// What the arrangement had selected when the window opened.
struct ExportSelection {
  std::vector<doc::TrackId> tracks;  // row order
  std::optional<TickRange> time;     // the time selection (start < end), if any
};

// The end of the song: the last end of any clip a track plays (0: no clips).
Ticks exportSongEnd(const doc::Project& p) noexcept;

class ExportWindow : public ui::Popover {
public:
  static constexpr float kWidth = 600.0f, kHeight = 476.0f;  // logical px
  ExportWindow(ExportPort& port, ViewContext& ctx, ExportSelection selection);
  ~ExportWindow() override;

  // The job Export would start now (tests); nullopt with the reason in `why` when the choices cannot export.
  std::optional<ExportJob> job(std::string* why = nullptr) const;
  // Starts the export (the Export button). False when the choices cannot export or the port refused.
  bool exportNow();
  void cancelExport();  // the Cancel button
  // Reads the port's status and updates the progress bar, the buttons and the done line (the poll timer calls it).
  void poll();
  bool polling() const noexcept { return timer_ != 0; }
  // Starts the poll timer when a job runs (openExportWindow calls it once the window is in a tree).
  void startPolling();

  // Readouts (tests).
  const std::string& rangeText() const;
  const std::string& doneText() const;
  float progressShown() const noexcept;

  // The controls (tests drive them like a user would).
  ui::Dropdown* tracksChooser() const noexcept { return tracks_; }
  ui::Dropdown* rangeChooser() const noexcept { return range_; }
  ui::Toggle* returnsToggle() const noexcept { return returns_; }
  ui::Toggle* loopToggle() const noexcept { return loop_; }
  ui::Dropdown* formatChooser() const noexcept { return format_; }
  ui::Dropdown* rateChooser() const noexcept { return rate_; }
  ui::Dropdown* bitsChooser() const noexcept { return bits_; }
  ui::Dropdown* ditherChooser() const noexcept { return dither_; }
  ui::Toggle* normalizeToggle() const noexcept { return normalize_; }
  ui::Button* browseButton() const noexcept { return browse_; }
  ui::TextEdit* nameField() const noexcept { return name_; }
  ui::Button* exportButton() const noexcept { return export_; }
  ui::Button* cancelButton() const noexcept { return cancel_; }
  const std::string& folderText() const;

  void onTimer(uint32_t id) override;
  ui::InfoText info() const override;

  static constexpr double kPollSeconds = 1.0 / 30.0;
  static constexpr uint32_t kRates[] = {44100, 48000, 88200, 96000, 192000};

private:
  void choose(const ExportPrefs& p);  // remembers p and updates the rows
  void updateRows();
  void showStatus(const ExportStatus& s);
  std::string folder() const;  // the chosen folder, else the default one
  std::optional<TickRange> rangeOf(ExportRange r) const;
  std::vector<doc::TrackId> selectedTracks() const;

  ExportPort& port_;
  ViewContext& ctx_;
  ExportSelection selection_;
  std::vector<uint32_t> rates_;
  ui::Dropdown *tracks_ = nullptr, *range_ = nullptr, *format_ = nullptr, *rate_ = nullptr, *bits_ = nullptr,
               *dither_ = nullptr;
  ui::Toggle *returns_ = nullptr, *loop_ = nullptr, *normalize_ = nullptr;
  ui::Label *rangeLabel_ = nullptr, *folder_ = nullptr, *done_ = nullptr;
  ui::Button *browse_ = nullptr, *export_ = nullptr, *cancel_ = nullptr;
  ui::TextEdit* name_ = nullptr;
  ui::Widget* progress_ = nullptr;
  ExportStatus shown_;
  bool running_ = false;
  uint32_t timer_ = 0;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // for the folder dialog's late answer
};

// Opens the window centred in `from`'s overlay layer (one at a time: an open one is returned as it is). nullptr
// without an overlay layer. `onClosed` runs when it closes (for any reason).
ExportWindow* openExportWindow(ui::Widget& from, ExportPort& port, ViewContext& ctx, ExportSelection selection,
                               std::function<void()> onClosed = {});
ExportWindow* findExportWindow(const ui::Widget& w);  // the open one, if any
// Installs the file.exportAudio handler (Ctrl+Shift+R; File → Export Audio…): toggles the window over ctx.exportPort()
// with the arrangement's selection (disabled while the context has no port). One line in installDefaultContent.
void installExportAudio(Shell& shell);

}  // namespace pq::views
