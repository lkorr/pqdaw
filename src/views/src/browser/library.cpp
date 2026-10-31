// T5.10: the browser's data in the views (pq/views/browser/library.h).

#include "pq/views/browser/library.h"

#include <algorithm>

#include "pq/library/indexer.h"

namespace pq::views {

std::string placeKey(std::string_view root) { return library::foldedPathKey(root); }

namespace {

std::string lastComponent(std::string_view path) {
  while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.remove_suffix(1);
  const size_t s = path.find_last_of("/\\");
  return std::string(s == std::string_view::npos ? path : path.substr(s + 1));
}

std::string nativePath(std::string_view path) {
  std::string s(path);
  std::replace(s.begin(), s.end(), '/', '\\');
  while (s.size() > 3 && s.back() == '\\') s.pop_back();  // keep "C:\"
  return s;
}

}  // namespace

Library::Library(ViewContext& ctx) : ctx_(ctx), tags_(std::make_unique<library::TagStore>()) {
  project_ = ctx_.projectDir();
  rebuildPlaces();
  projectSub_ = ctx_.observe([this](uint32_t bits) {
    if (!(bits & view_change::kProject) || ctx_.projectDir() == project_) return;
    project_ = ctx_.projectDir();
    rebuildPlaces();
  });
}

Library::~Library() = default;

void Library::rebuildPlaces() {
  std::vector<library::PlaceSpec> p;
  if (!userLibrary_.empty()) p.push_back({userLibrary_, library::PlaceKind::UserLibrary, "User Library"});
  if (!project_.empty()) p.push_back({nativePath(project_), library::PlaceKind::Project, lastComponent(project_)});
  for (const std::string& f : folders_) p.push_back({f, library::PlaceKind::Folder, lastComponent(f)});
  // T5.x-samples-folder: the Samples folder second, unless another place already holds it (the default lies in the
  // User Library).
  if (const std::string s = samplesFolder(); !s.empty()) {
    const std::string key = placeKey(s);
    const bool covered = std::any_of(p.begin(), p.end(), [&](const library::PlaceSpec& q) {
      const std::string k = placeKey(q.root);
      return key == k || (key.size() > k.size() && key.compare(0, k.size(), k) == 0 && key[k.size()] == '\\');
    });
    if (!covered) p.insert(p.begin() + (userLibrary_.empty() ? 0 : 1), {s, library::PlaceKind::Folder, "Samples"});
  }
  if (p == places_) return;
  places_ = std::move(p);
  if (indexer_) indexer_->setPlaces(places_);
  ctx_.libraryChanged();
}

void Library::setUserLibrary(std::string root) {
  userLibrary_ = root.empty() ? std::string() : nativePath(root);
  rebuildPlaces();
}

bool Library::changeUserLibrary(std::string_view root) {
  const std::string n = nativePath(root);
  if (n.empty() || placeKey(n) == placeKey(userLibrary_)) return false;
  setUserLibrary(n);
  if (userLibraryListener_) userLibraryListener_(userLibrary_);
  return true;
}

std::string Library::samplesFolder() const { return samplesFolder_.empty() ? userPath("Samples") : samplesFolder_; }

void Library::setSamplesFolder(std::string root) {
  samplesFolder_ = root.empty() ? std::string() : nativePath(root);
  rebuildPlaces();
}

bool Library::changeSamplesFolder(std::string_view root) {
  const std::string n = nativePath(root);
  if (n.empty() || placeKey(n) == placeKey(samplesFolder())) return false;
  setSamplesFolder(n);
  if (samplesFolderListener_) samplesFolderListener_(samplesFolder_);
  return true;
}

void Library::setFolders(std::vector<std::string> roots) {
  folders_.clear();
  for (std::string& r : roots) {
    std::string n = nativePath(r);
    if (std::none_of(folders_.begin(), folders_.end(), [&](const std::string& f) { return placeKey(f) == placeKey(n); }))
      folders_.push_back(std::move(n));
  }
  rebuildPlaces();
}

bool Library::addPlace(std::string_view folder) {
  const std::string n = nativePath(folder);
  const std::string key = placeKey(n);
  for (const library::PlaceSpec& p : places_)
    if (placeKey(p.root) == key) return false;
  folders_.push_back(n);
  rebuildPlaces();
  if (foldersListener_) foldersListener_(folders_);
  return true;
}

bool Library::removePlace(std::string_view folder) {
  const std::string key = placeKey(folder);
  const auto it = std::find_if(folders_.begin(), folders_.end(), [&](const std::string& f) { return placeKey(f) == key; });
  if (it == folders_.end()) return false;
  folders_.erase(it);
  rebuildPlaces();
  if (foldersListener_) foldersListener_(folders_);
  return true;
}

void Library::setIndexer(library::Indexer* indexer) {
  indexer_ = indexer;
  if (indexer_) indexer_->setPlaces(places_);
  indexChanged();
}

void Library::setIndex(std::shared_ptr<const library::Index> index) {
  fixed_ = std::move(index);
  indexChanged();
}

std::shared_ptr<const library::Index> Library::index() const {
  if (indexer_) return indexer_->index();
  return fixed_;
}

void Library::indexChanged() {
  ++generation_;
  if (const auto idx = index(); idx && tags_->reconcile(*idx) && !tags_->file().empty()) (void)tags_->save();
  ctx_.libraryChanged();
}

Status Library::setTagFile(const std::filesystem::path& file) {
  tags_ = std::make_unique<library::TagStore>(file);
  entryTags_.reset();
  Status s = tags_->load();
  ctx_.libraryChanged();
  return s;
}

void Library::tagsChanged() {
  if (!tags_->file().empty())
    if (Status s = tags_->save(); !s) ctx_.setStatus("Could not save tags: " + s.error().message);
  entryTags_.reset();
  ctx_.libraryChanged();
}

std::shared_ptr<const library::EntryTags> Library::entryTags() { return entryTags(index()); }

std::shared_ptr<const library::EntryTags> Library::entryTags(const std::shared_ptr<const library::Index>& idx) {
  if (!idx) return nullptr;
  if (!entryTags_ || tagsFor_ != idx || tagsVersion_ != tags_->version()) {
    entryTags_ = library::buildEntryTags(*idx, *tags_);
    tagsFor_ = idx;
    tagsVersion_ = tags_->version();
  }
  return entryTags_;
}

}  // namespace pq::views
