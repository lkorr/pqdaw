#pragma once

// The bug report (T5.x-bugreport, user request 2026-10-31; the idea and the report.md layout come from the sandvox
// project's F12 tool): F12 in the window photographs the frame as presented, captures what an agent needs to debug it
// (pq/views/bug_report/input_log.h, describeShot below, the app's context) and opens the report panel
// (pq/views/bug_report/bug_panel.h) over the window. Submit files one folder:
//
//   bug_reports/<YYYY-MM-DD_HH-MM-SS>/        (suggestions: bug_reports/suggestions/<stamp>/)
//     report.md            title, `tags:` line, the description, `## Screenshots` with one `### Shot N (WxH)` section
//                          per picture, then `## Context (at the first shot)`, `## Recent input`, `## Recent status messages`
//     shot_N.png           the whole window at native resolution, RGB (shotPngOptions: measured, bench/RESULTS.md)
//     shot_N_cursor.png    a 1:1 crop around the pointer (cursorCrop) with a crosshair, so a tiny glitch stays
//                          readable when an agent looks at a downscaled full shot
//     project.pqd          optional: the document as it was at the first shot (unsaved edits included; no media)
//
// Size budget (the user's requirement): a typical folder ≤ ~600 KB at 1920×1080, report.md ≤ ~20 KB.
//
// THE TICKET LIST. Every folder directly in bug_reports/ is an OPEN ticket (`fixed/` and `suggestions/` are not;
// agents move a ticket to bug_reports/fixed/ once its fix is merged). Picking one in the panel APPENDS: the new shots
// become shot_{N+1}.png ... with their own `### Shot N` sections, the description and the tags line are rewritten, and
// everything else (earlier shots, the context, agents' `## Progress` sections) stays verbatim. Nothing here deletes a
// ticket or a picture.
//
// TAGS: lower-case [a-z0-9-_], one line `tags: a, b` under the title, so an agent finds every UI ticket with
//   grep -l "^tags:.*\bui\b" bug_reports/*/report.md
// Custom tags are remembered in bug_reports/tags.txt.
//
// Everything here runs on the UI thread except writeReport (the app runs it on a worker: PNG encoding) and
// loadThumbnail (the panel's loader thread). Nothing touches the audio thread; nothing costs anything until F12.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/ui/image.h"
#include "pq/ui/widget.h"

namespace pq::views {
class Shell;
}

namespace pq::views::bugreport {

// ---- report.md as text (pure) ---------------------------------------------------------------------------------------

// A report.md split so that a rewrite changes only the description and the tags line: `pre` (the title line and the
// blank lines after it), `tags`, `text` (the description, trailing whitespace trimmed), `shots` (from "## Screenshots"
// up to the next "## " heading) and `tail` (that heading to the end: context, logs, agents' `## Progress`).
struct ReportMd {
  std::string pre;
  std::vector<std::string> tags;
  std::string text;
  std::string shots;
  std::string tail;
  int shotCount = 0;  // "### Shot " headings in `shots`
};
ReportMd parseReportMd(std::string md);
// The file again with `text` as the description, `tags` as the tags line (none: no line) and `newSections` (whole
// `### Shot N` sections) after the existing ones. composeReportMd(m, m.text, m.tags, "") == md for a file we wrote.
std::string composeReportMd(const ReportMd& m, std::string_view text, std::span<const std::string> tags,
                            std::string_view newSections);
// A tag as stored: trimmed, lower case, spaces to '-', anything outside [a-z0-9-_] dropped, at most 24 characters.
// "" = not a tag.
std::string normalizeTag(std::string_view t);
// Normalised, de-duplicated, in order.
std::vector<std::string> cleanTags(std::span<const std::string> tags);
// The tags every panel offers before custom ones.
const std::vector<std::string>& starterTags();
// One `### Shot N (WxH)` section: the picture, the pointer crop (when there is one) and `body` (markdown lines).
std::string shotSection(int n, uint32_t w, uint32_t h, bool hasCrop, std::string_view body);
// "# Bug report 2026-10-31 14:03:22" / "# Suggestion ...", followed by a blank line.
std::string reportTitle(bool suggestion, std::string_view humanTime);
// The local time as a folder name ("2026-10-31_14-03-22") and for people ("2026-10-31 14:03:22").
std::string localStamp(bool forFolder);

// ---- pictures (pure) ------------------------------------------------------------------------------------------------

inline constexpr uint32_t kCropWidth = 512, kCropHeight = 288;  // device px, 1:1
// The crop around the pointer at (x, y) (device px), clamped inside a w×h window (smaller when the window is).
ui::IRect cursorCropRect(uint32_t w, uint32_t h, float x, float y, uint32_t cropW = kCropWidth,
                         uint32_t cropH = kCropHeight) noexcept;
// That crop of `img` with a small crosshair where the pointer was (on the crop only). Empty when the pointer is outside.
ui::Image cursorCrop(const ui::Image& img, float x, float y, uint32_t cropW = kCropWidth, uint32_t cropH = kCropHeight);
// Area-averaged downscale to fit maxW×maxH (aspect kept; never enlarges).
ui::Image downscale(const ui::Image& img, uint32_t maxW, uint32_t maxH);
// The PNG settings screenshots are written with (chosen by measurement on real DAW frames: bench/RESULTS.md,
// pq_bench_bug_report).
ui::PngOptions shotPngOptions() noexcept;

// ---- the draft --------------------------------------------------------------------------------------------------------

struct Shot {
  ui::Image image;      // the whole window, RGBA (opaque)
  ui::Image crop;       // around the pointer; empty when the pointer was outside the window
  ui::Image thumb;      // the panel's
  std::string context;  // markdown lines written under the shot's heading
};

struct Draft {
  std::vector<Shot> shots;
  std::string text;
  std::vector<std::string> tags;
  bool suggestion = false;
  std::filesystem::path target;  // an open ticket to append to; empty = a new report
  // Everything after `## Screenshots` in a NEW report (## Context, ## Recent input, ## Recent status messages), captured at
  // the first shot. Unused when appending (the ticket's own stays as it is).
  std::string trailer;
  std::string project;           // the serialized document at the first shot (empty: none)
  bool attachProject = false;
  bool empty() const noexcept { return shots.empty() && text.empty() && tags.empty(); }
};

// Projects up to this size are attached by default (above: off, the checkbox shows the size).
inline constexpr size_t kAttachProjectDefaultLimit = 1u << 20;

// ---- folders ----------------------------------------------------------------------------------------------------------

// bug_reports/ or bug_reports/suggestions/.
std::filesystem::path kindRoot(const std::filesystem::path& root, bool suggestion);

struct WriteResult {
  std::filesystem::path dir;
  uint64_t bytes = 0;            // written by this call
  std::vector<int> shotNumbers;  // the N of each shot_N.png written
};
// Writes a new report (d.target empty) into kindRoot(root, d.suggestion)/<folderStamp> (a "_2", "_3" ... suffix when
// that exists), or appends to d.target. A bug needs a shot; a suggestion needs a shot or text. Files are written
// through a temporary name and renamed; on failure the files this call made are removed. Remembers custom tags in
// root/tags.txt.
Result<WriteResult> writeReport(const std::filesystem::path& root, const Draft& d, std::string_view folderStamp,
                                std::string_view humanStamp);

struct Ticket {
  std::filesystem::path dir;
  std::string id;          // the folder name (its stamp)
  std::string firstLine;   // of the description
  std::vector<std::string> tags;
  std::vector<std::filesystem::path> shots;  // shot_N.png in N order
  bool suggestion = false;
};
// The open tickets (folders directly in kindRoot(root, suggestions) that hold a report.md), newest first.
std::vector<Ticket> listTickets(const std::filesystem::path& root, bool suggestions);
// A ticket's report.md, parsed.
Result<ReportMd> readTicket(const std::filesystem::path& dir);
// The starter tags, then root/tags.txt's, then the tickets', each once.
std::vector<std::string> knownTags(const std::filesystem::path& root, std::span<const Ticket> tickets);
// Appends the tags not yet in root/tags.txt (and not starter tags) to it.
void rememberTags(const std::filesystem::path& root, std::span<const std::string> tags);
// A shot PNG decoded and downscaled to fit maxW×maxH (the panel's loader thread).
Result<ui::Image> loadThumbnail(const std::filesystem::path& png, uint32_t maxW, uint32_t maxH);

// ---- what the views show at F12 -------------------------------------------------------------------------------------

// The widget chain from the root to `leaf` as "Type 'name' [x,y wxh] > ..." (device px; unnamed wrappers kept, as
// "Widget"), and a widget's type name without namespaces.
std::string widgetChain(const ui::Widget* leaf);
std::string typeName(const ui::Widget& w);
// Markdown lines ("- key: value") about the views at this instant: the widget under `pointer` and the focused one,
// the visible views and open popups, selections (tracks, clips, notes, devices with ids), the transport (position in
// bars and ticks, tempo, loop) and the visible editors' zoom / scroll. UI thread; allocates (F12 only).
std::string describeShot(Shell& shell, ui::Vec2 pointer, bool pointerInside);
// One line per project fact for `## Context`: name, path, dirty, track / clip / device counts, schema version, the
// selected track's tuning.
std::string describeProject(Shell& shell);

}  // namespace pq::views::bugreport
