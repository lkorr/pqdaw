#pragma once

// The browser's data (T5.10; SPEC §9.1): places, the index, tags and the preview, as the views see them. The ViewContext
// owns one (ctx.library()).
//
//   places   the User Library, the current project's folder and the folders the user added (a drop of a folder on the
//            arrangement or the browser, the sidebar's "Add Folder…"). The added folders are the app's to persist
//            (setFoldersListener); the project place follows the context's project directory.
//   index    the app gives a library::Indexer (setIndexer: the live index, places pushed to it whenever they change;
//            the indexer's onChange is posted to the UI thread as indexChanged()); tests and scenes give a fixed
//            library::Index (setIndex). Either way index() is an immutable snapshot.
//   tags     a library::TagStore (in memory unless setTagFile names its tags.json: tests never touch the user's
//            %LOCALAPPDATA%); entryTags() is the search's view of it, rebuilt when the index or the tags change.
//   preview  the app's PreviewPort (the cue-bus player, pq/views/browser/preview_port.h); null in tests without one.
// Changes notify view_change::kLibrary.
//
// UI thread only.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pq/library/index.h"
#include "pq/library/search.h"
#include "pq/library/tags.h"
#include "pq/views/view_context.h"

namespace pq::library {
class Indexer;
}

namespace pq::views {

class ViewContext;
class PreviewPort;

class Library {
public:
  explicit Library(ViewContext& ctx);
  ~Library();
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;

  // --- places ---
  // In sidebar order: the User Library (if set), the project (if titled), then the added folders.
  const std::vector<library::PlaceSpec>& places() const noexcept { return places_; }
  void setUserLibrary(std::string rootUtf8);  // the app: Documents\pqdaw\User Library ("" = none)
  // The added folders, as the app loaded them (no listener call).
  void setFolders(std::vector<std::string> rootsUtf8);
  const std::vector<std::string>& folders() const noexcept { return folders_; }
  // Adds a folder place (false when it is already a place: case-insensitive, separators normalised). Calls the folders
  // listener (the app saves).
  bool addPlace(std::string_view folderUtf8);
  bool removePlace(std::string_view folderUtf8);
  void setFoldersListener(std::function<void(const std::vector<std::string>&)> fn) { foldersListener_ = std::move(fn); }

  // --- index ---
  void setIndexer(library::Indexer* indexer);  // null: none (index() then returns the fixed one)
  library::Indexer* indexer() const noexcept { return indexer_; }
  void setIndex(std::shared_ptr<const library::Index> index);
  std::shared_ptr<const library::Index> index() const;
  // The indexer published (the app posts its onChange here). Notifies kLibrary.
  void indexChanged();
  // How many index publishes the views have seen (tests).
  uint64_t indexGeneration() const noexcept { return generation_; }

  // --- tags ---
  library::TagStore& tags() noexcept { return *tags_; }
  // Loads tags.json from `file` (missing = empty). Errors: Parse / Io (the store stays empty).
  Status setTagFile(const std::filesystem::path& file);
  // After an edit of tags(): saves (when a file is set), rebuilds the search's view, notifies kLibrary.
  void tagsChanged();
  std::shared_ptr<const library::EntryTags> entryTags();

  // --- preview ---
  void setPreview(PreviewPort* p) noexcept { preview_ = p; }
  PreviewPort* preview() const noexcept { return preview_; }

private:
  void rebuildPlaces();

  ViewContext& ctx_;
  std::string userLibrary_;
  std::string project_;
  std::vector<std::string> folders_;
  std::vector<library::PlaceSpec> places_;
  std::function<void(const std::vector<std::string>&)> foldersListener_;
  library::Indexer* indexer_ = nullptr;
  std::shared_ptr<const library::Index> fixed_;
  uint64_t generation_ = 0;
  std::unique_ptr<library::TagStore> tags_;
  std::shared_ptr<const library::EntryTags> entryTags_;
  std::shared_ptr<const library::Index> tagsFor_;  // the index entryTags_ was built for
  uint64_t tagsVersion_ = ~0ull;
  PreviewPort* preview_ = nullptr;
  ViewContext::Subscription projectSub_;
};

// Folded comparison key of a place root ('/' → '\\', lower case, no trailing separator).
std::string placeKey(std::string_view root);

}  // namespace pq::views
