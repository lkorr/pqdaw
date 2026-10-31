#pragma once

// The bug report panel (T5.x-bugreport): a modal over the whole window, opened by F12 after the shot was taken (the app:
// src/app/src/bug_reporter.cpp). The app keeps running behind it (the transport is not stopped).
//
//   ┌ Bug report ────────────────────────────────────────── [Bugs][Suggestions]  [Close] ┐
//   │ [All tags ▾]          │ New bug report · 2 shots                                    │
//   │ ┌───┐ + New report    │ ┌──────────┐┌──────────┐                                   │
//   │ ├───┤ first line …    │ │ shot 1  ×││ shot 2  ×│                                   │
//   │ │img│ 2026-10-31 … ui │ └──────────┘└──────────┘                                   │
//   │ └───┘ …               │ ┌ description ─────────────────────────────────────────┐  │
//   │                       │ │                                                       │  │
//   │                       │ └───────────────────────────────────────────────────────┘  │
//   │                       │ (ui)(arrangement)(piano-roll)…  [new tag      ]            │
//   │                       │ [Attach project · 12 KB]                                   │
//   │                       │                          [Discard] [Add another] [Submit]  │
//   └────────────────────────────────────────────────────────────────────────────────────┘
//
// Left: the open tickets (bug_report.h listTickets: folders directly in bug_reports/, newest first; with the switch on
// Suggestions, bug_reports/suggestions/), each by its first shot and the first line of its text, filterable by tag,
// under a "new report" row. Picking a ticket loads its text and tags; Submit then appends the new shots to it.
// Right: the shots of this report (each removable with its ×; a picked ticket's own shots shown first, dimmed, never
// removable), the description (focused on open), tag chips (click toggles; a new tag is typed into the field and
// remembered in bug_reports/tags.txt), "attach project", and the buttons.
//
// Keys: Esc closes keeping the draft (F12 / Shift+F12 bring it back), Ctrl+Enter submits. The panel swallows every
// other key so the DAW's shortcuts do not fire behind it.
//
// Thumbnails: the draft's shots are downscaled once; tickets' first shots are decoded on a loader thread when the app
// gave a poster (else synchronously: tests, scenes) and drawn from the renderer's tile atlas as three additive colour
// planes (one coverage tile; no colour texture API needed), or as a coarse mosaic without a GPU. Nothing runs while
// the panel is closed; while it is open there are no timers.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/widget.h"
#include "pq/views/bug_report/bug_report.h"
#include "pq/views/view_context.h"

namespace pq::ui {
class CodeEditor;
}

namespace pq::views::bugreport {

struct PanelHooks {
  std::function<void()> submit;      // the app validates and writes the draft, then clears and closes
  std::function<void()> addAnother;  // the panel hides; the next F12 adds a shot to this draft
  std::function<void()> discard;     // the draft is dropped
  std::function<void()> close;       // Esc / Close: the panel hides, the draft is kept
  ViewContext::UiPoster post;        // set: tickets' thumbnails decode on a loader thread
  // Defaults: listTickets(root, suggestions) and loadThumbnail(png, w, h).
  std::function<std::vector<Ticket>(bool suggestions)> tickets;
  std::function<Result<ui::Image>(const std::filesystem::path& png, uint32_t maxW, uint32_t maxH)> thumbnail;
};

class TicketList;
class ShotStrip;
class TagChips;

class BugReportPanel : public ui::Control {
public:
  BugReportPanel(Draft& draft, std::filesystem::path root, PanelHooks hooks);
  ~BugReportPanel() override;

  Draft& draft() noexcept { return draft_; }
  const std::filesystem::path& root() const noexcept { return root_; }
  // Re-reads the ticket list and the known tags (on open and when the switch changes).
  void refresh();
  // Picks ticket `index` of the shown (filtered) list; -1 = the "new report" row.
  void selectTicket(int index);
  int selectedTicket() const noexcept { return selected_; }
  const std::vector<Ticket>& tickets() const noexcept { return shownTickets_; }
  void setSuggestions(bool on);
  bool suggestions() const noexcept { return draft_.suggestion; }
  void setTagFilter(std::string tag);  // "" = all
  void toggleTag(const std::string& tag);
  bool addTag(std::string_view typed);  // normalised; selected and offered from now on
  const std::vector<std::string>& knownTagList() const noexcept { return knownTags_; }
  // The draft's shots changed (a shot was added while the panel was hidden): thumbnails and labels follow.
  void shotsChanged();
  void removeShot(size_t index);
  // A line under the buttons (validation errors, "writing…").
  void setMessage(std::string text);
  const std::string& message() const noexcept { return message_; }
  void focusDescription();
  ui::CodeEditor& description() const noexcept { return *description_; }
  ui::Toggle& attachToggle() const noexcept { return *attach_; }

  // The thumbnail of a ticket's picture (null while loading or when it failed).
  const ui::Image* ticketThumb(const std::filesystem::path& png);
  void thumbsArrived();

  ui::IRect cardRect() const noexcept { return card_; }
  void arrange(ui::LayoutPass& pass) override;
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  bool onKey(const ui::KeyEvent& e) override;

  static constexpr float kCardWidth = 1040.0f, kCardHeight = 660.0f;  // logical px (smaller windows: the window - 48)

private:
  friend class TicketList;
  friend class ShotStrip;
  friend class TagChips;
  void updateLabels();
  struct Loader;

  Draft& draft_;
  std::filesystem::path root_;
  PanelHooks hooks_;
  std::vector<Ticket> allTickets_, shownTickets_;
  std::vector<std::string> knownTags_;
  std::string filter_;
  int selected_ = -1;
  std::string newText_;  // the new report's text and tags while a ticket is picked
  std::vector<std::string> newTags_;
  std::string message_;
  ui::IRect card_{};
  ui::Label *title_ = nullptr, *target_ = nullptr, *messageLabel_ = nullptr;
  ui::Toggle *bugs_ = nullptr, *suggest_ = nullptr, *attach_ = nullptr;
  ui::Button *close_ = nullptr, *discard_ = nullptr, *another_ = nullptr, *submit_ = nullptr;
  ui::Dropdown* filterChooser_ = nullptr;
  ui::TextEdit* tagEntry_ = nullptr;
  ui::CodeEditor* description_ = nullptr;
  TicketList* list_ = nullptr;
  ShotStrip* strip_ = nullptr;
  TagChips* chips_ = nullptr;
  std::unique_ptr<Loader> loader_;
  std::shared_ptr<BugReportPanel*> alive_;
};

// Opens the panel over `layer` (one at a time: an open one is returned as it is) with the description focused.
BugReportPanel* openBugReportPanel(ui::OverlayLayer& layer, Draft& draft, std::filesystem::path root, PanelHooks hooks);
// The open panel of `w`'s tree, if any.
BugReportPanel* findBugReportPanel(const ui::Widget& w);

}  // namespace pq::views::bugreport
