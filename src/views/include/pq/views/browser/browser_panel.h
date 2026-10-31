#pragma once

// The browser panel (T5.10; SPEC §9.1; docs/plans/M5.md T5.10), in ShellSlot::Browser (Ctrl+Alt+B; View > Browser):
//
//   ┌ [ Search (Ctrl+F)                 ] ┐   main/body/top/browser/browser/search
//   │ ● ● ● ● ● ● ●  (colour collections) │   …/chips        a chip filters the list by its collection (click)
//   ├────────────┬────────────────────────┤
//   │ COLLECTIONS│ Name   Type Dur BPM Hz │   …/sidebar      a click selects the source of the list
//   │  Red …     │ ▾ Drums                │   …/list         the list: a virtualised tree (a place: its folders and
//   │ CATEGORIES │     Kick.wav  …        │                  files) or a flat result list (a category, a collection,
//   │  Samples … │     Snare.wav …        │                  or any search text: best matches first); columns sort
//   │ PLACES     │                        │                  by a click on their header (again: descending)
//   │  User Lib… │                        │
//   ├────────────┴────────────────────────┤
//   │ [Prev] ▁▃▅▇▅▃▁ waveform   [-6 dB] [Sync] │  …/preview      scrub on click; preview on/off; volume; Raw / Sync
//   └──────────────────────────────────────┘
//
// Keys (the panel's own, while focus is inside it; HOTKEYS "Browser"): Up / Down select (with preview on, a sample
// starts playing); Left / Right fold / unfold a folder (Left on a file: its folder); Enter loads the selection (a
// sample becomes a clip at the start marker on the selected audio track, else on a new one; presets and devices load
// with T5.11); Esc stops the preview; typing a printable character goes to the search field. Ctrl+F (browser.search)
// shows the browser and focuses the search field.
// Mouse: click selects (and previews), double-click loads, a drag carries the selection as a BrowserItems payload
// (pq/ui/drag.h: the arrangement takes it like files from Explorer), right-click opens the item's menu (collections,
// Show in Explorer, Rename / Delete in the User Library). A folder from Explorer dropped on the panel becomes a place.
//
// Cost: rows are an index array (4 bytes each) rebuilt only when the index, the source, the query, the sort or a fold
// changes; a paint draws only the visible rows (≤ 1 ms for a 200k-row list: row paint is O(visible)). Searching runs
// on library::Searcher's worker when the app installed a UI poster (a newer keystroke cancels the stale query; typing
// more narrows the previous result), synchronously otherwise (tests, scenes). The preview waveform is built on a
// background thread (media's loadOrBuildOverview into the shared waveform cache) and drawn from its overview levels.
//
// UI thread only.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pq/library/index.h"
#include "pq/library/search.h"
#include "pq/media/waveform.h"
#include "pq/ui/controls.h"
#include "pq/ui/widget.h"
#include "pq/views/view_context.h"

namespace pq::views {

class Shell;
class Library;

// Where the list comes from.
struct BrowserSource {
  enum class Kind : uint8_t { Place, Category, Collection };
  Kind kind = Kind::Category;
  uint32_t index = static_cast<uint32_t>(library::Category::Samples);  // place index / Category / collection 0..6
  bool operator==(const BrowserSource&) const = default;
};

enum class BrowserColumn : uint8_t { Name, Type, Duration, Bpm, Rate, Relevance };

struct BrowserRow {
  uint32_t id = 0;     // an entry, or (dir) a directory of the index
  uint16_t depth = 0;  // tree indent
  bool dir = false;
};

namespace browser {
class Sidebar;
class List;
class PreviewStrip;
class Chips;
}  // namespace browser

class BrowserPanel : public ui::Widget {
public:
  explicit BrowserPanel(Shell& shell);
  ~BrowserPanel() override;

  // --- state (tests drive these directly as well as through input) ---
  void setSource(BrowserSource s);
  const BrowserSource& source() const noexcept { return source_; }
  void setQuery(std::string text);  // what the search field holds
  const std::string& query() const noexcept { return query_; }
  void setCollectionFilter(uint8_t bits);
  uint8_t collectionFilter() const noexcept { return collections_; }
  void setSort(BrowserColumn c, bool descending);
  BrowserColumn sortColumn() const noexcept { return sort_; }
  bool sortDescending() const noexcept { return descending_; }
  // The rows shown now (after the last search result / tree change).
  std::span<const BrowserRow> rows() const noexcept { return rows_; }
  const library::Index* index() const noexcept { return index_.get(); }
  // Folders: open / closed (a place's root is always open).
  bool folderOpen(uint32_t dir) const noexcept;
  void setFolderOpen(uint32_t dir, bool open);
  // Selection (−1: none). Selecting a sample loads its head; with preview on it plays.
  int32_t selected() const noexcept { return selected_; }
  void select(int32_t row, bool preview = true);
  std::string selectedPath() const;
  // Loads the selection (Enter / double-click). False when nothing could be loaded.
  bool loadSelection();
  // Waits until the search in flight (if any) has delivered (tests).
  void waitForSearch();
  // Preview settings.
  bool previewOn() const noexcept { return previewOn_; }
  void setPreviewOn(bool on);
  bool sync() const noexcept { return sync_; }
  void setSync(bool on);
  void focusSearch();
  // The ghost of a drag of the selection (tests).
  ui::DragPayload selectionPayload() const;
  ui::TextEdit& searchField() const noexcept { return *search_; }
  browser::List& list() const noexcept { return *list_; }
  // The list widget (focus, scrolling) and a row's device-px rect (tests, scenes).
  ui::Widget& listWidget() const noexcept;
  ui::IRect rowRect(int32_t row) const noexcept;

  struct Stats {
    uint64_t rebuilds = 0;     // row arrays built
    uint64_t searches = 0;     // queries run (sync) or submitted (worker)
    uint32_t rowsPainted = 0;  // last list paint
    double lastPaintMs = 0;    // last list paint (CPU, the draw list only)
  };
  const Stats& stats() const noexcept { return stats_; }
  Stats& statsMut() noexcept { return stats_; }

  bool onKey(const ui::KeyEvent& e) override;
  void paint(ui::PaintContext& ctx) override;
  ui::DropFeedback dragOver(const ui::DragPayload& p, ui::Vec2 pos, ui::Mods mods) override;
  ui::DropEffect drop(const ui::DragPayload& p, ui::Vec2 pos, ui::Mods mods) override;
  void onTimer(uint32_t id) override;

  // --- helpers the parts share ---
  ViewContext& context() const noexcept { return ctx_; }
  // The text of a column for a row ("" when it has none).
  std::string cellText(const BrowserRow& r, BrowserColumn c) const;
  void openContextMenu(int32_t row, ui::Vec2 at);
  const media::WaveformOverview* previewOverview() const noexcept { return overview_.get(); }
  double previewLength() const noexcept;      // seconds of the selected sample (0: none)
  double previewPosition();                  // seconds (< 0: not playing)
  void scrubTo(double seconds);              // click in the preview waveform
  void startPreview(double fromSeconds = 0);
  void stopPreview();
  bool previewPlaying();

private:
  void onLibrary();
  void refresh(bool keepSelection = true);   // runs (or submits) the search / rebuilds the tree
  void applyResult(std::shared_ptr<const library::SearchResult> r);
  void buildTree();
  void sortRows();
  void sortFiles(std::vector<BrowserRow>& v) const;
  bool searching() const noexcept;           // flat result mode
  library::SearchQuery currentQuery() const;
  void selectionChanged(bool preview);
  void requestOverview();

  Shell& shell_;
  ViewContext& ctx_;
  Library& lib_;
  ViewContext::Subscription libSub_;
  ui::TextEdit* search_ = nullptr;
  browser::Chips* chips_ = nullptr;
  browser::Sidebar* sidebar_ = nullptr;
  browser::List* list_ = nullptr;
  browser::PreviewStrip* strip_ = nullptr;

  std::shared_ptr<const library::Index> index_;
  std::shared_ptr<const library::EntryTags> tags_;
  BrowserSource source_{};
  std::string query_;
  uint8_t collections_ = 0;
  BrowserColumn sort_ = BrowserColumn::Relevance;
  bool descending_ = false;
  std::vector<BrowserRow> rows_;
  std::vector<uint32_t> openDirs_;  // sorted dir ids (the index's) that are unfolded
  std::string openFor_;             // the place root the fold state belongs to (a new index keeps it by path)
  int32_t selected_ = -1;
  std::string selectedKey_;         // the selected row's path: kept across rebuilds
  std::shared_ptr<const library::SearchResult> result_;  // the last finished one (narrowing)
  std::unique_ptr<library::Searcher> searcher_;
  uint64_t ticket_ = 0;

  bool previewOn_ = true;
  bool sync_ = false;
  float volumeDb_ = -6.0f;
  media::OverviewPtr overview_;
  std::string overviewFor_;
  uint64_t overviewGen_ = 0;
  uint32_t pollTimer_ = 0;
  std::shared_ptr<char> life_ = std::make_shared<char>();
  Stats stats_;
};

// Installs the panel into ShellSlot::Browser (widens the slot) and the browser.search action (installDefaultContent).
BrowserPanel& installBrowser(Shell& shell);
// The installed panel (null when none).
BrowserPanel* findBrowser(Shell& shell);

}  // namespace pq::views
