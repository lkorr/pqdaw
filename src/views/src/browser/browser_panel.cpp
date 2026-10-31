// T5.10: the browser panel's state and logic (pq/views/browser/browser_panel.h); its parts paint in browser_parts.cpp.

#include "pq/views/browser/browser_panel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "browser_parts.h"
#include "pq/document/model.h"
#include "pq/library/tags.h"
#include "pq/ui/overlay.h"
#include "pq/ui/theme.h"
#include "pq/views/browser/library.h"
#include "pq/views/browser/preview_port.h"
#include "pq/views/file_probes.h"
#include "pq/views/sample_drop.h"
#include "pq/views/shell/shell.h"
#include "pq/views/waveforms.h"

namespace pq::views {

using namespace pq::ui;
namespace bm = browser::metrics;

namespace {

constexpr uint32_t kPollTimer = 1;
constexpr double kPollSeconds = 1.0 / 30.0;

bool isSample(const library::Index& ix, uint32_t e) { return ix.kind(e) == library::ItemKind::Sample; }

std::string extensionOf(std::string_view name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string_view::npos) return {};
  std::string e(name.substr(dot + 1));
  for (char& c : e) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
  return e;
}

// A worker for the preview overviews (one at a time: the newest selection wins).
struct OverviewJob {
  media::AnalysisWorker worker{true};
};

}  // namespace

// --- construction ---------------------------------------------------------------------------------------------------

BrowserPanel::BrowserPanel(Shell& shell)
    : Widget("browser"), shell_(shell), ctx_(shell.context()), lib_(shell.context().library()) {
  setLayout(LayoutKind::Column);
  setFocusable(true);
  setClipChildren(true);
  auto& top = add<Widget>("top");
  top.setLayout(LayoutKind::Column);
  top.setHeight(Length::fixed(48));
  top.setPadding({6, 6, 6, 2});
  top.setGap(4);
  search_ = &top.add<TextEdit>("search");
  search_->setHeight(Length::fixed(20));
  search_->setPlaceholder("Search (Ctrl+F)");
  search_->setInfo("Search", "Finds files by name and tag as you type (letters in order; word starts count most). "
                             "Up / Down move the selection, Enter loads it.");
  search_->onChange = [this](const std::string& t) { setQuery(t); };
  chips_ = &top.add<browser::Chips>(*this);
  chips_->setHeight(Length::fixed(bm::kChipSize + 2));

  auto& body = add<Widget>("body");
  body.setLayout(LayoutKind::Row);
  body.setHeight(Length::flex(1));
  body.setGap(1);
  sidebar_ = &body.add<browser::Sidebar>(*this);
  sidebar_->setWidth(Length::fixed(bm::kSidebarWidth));
  auto& content = body.add<Widget>("content");
  content.setLayout(LayoutKind::Column);
  content.setWidth(Length::flex(1));
  content.add<browser::ColumnHeader>(*this).setHeight(Length::fixed(bm::kHeaderHeight));
  auto& scroll = content.add<ScrollView>("scroll", false, true);
  scroll.setHeight(Length::flex(1));
  scroll.setWheelStep(bm::kRowHeight * 3);
  list_ = &scroll.add<browser::List>(*this);

  strip_ = &add<browser::PreviewStrip>(*this);
  strip_->setHeight(Length::fixed(bm::kStripHeight));

  if (ctx_.uiPoster()) {
    searcher_ = std::make_unique<library::Searcher>();
    searcher_->setOnResult([post = ctx_.uiPoster(), alive = std::weak_ptr<char>(life_), this](uint64_t) {
      post([alive, this] {
        if (!alive.lock()) return;
        if (auto r = searcher_->latest(); r && r->ticket == ticket_) applyResult(std::move(r));
      });
    });
  }
  libSub_ = ctx_.observe([this](uint32_t bits) {
    if (bits & view_change::kLibrary) onLibrary();
  });
  onLibrary();
}

BrowserPanel::~BrowserPanel() {
  if (pollTimer_) cancelTimer(pollTimer_);
  if (PreviewPort* p = lib_.preview(); p && p->playing()) p->stop();
}

void BrowserPanel::paint(PaintContext& ctx) {
  if (const Theme* th = ctx.res.theme) ctx.dl.addRectFilled(rect().toRect(), th->surface.panel);
}

// --- state ---------------------------------------------------------------------------------------------------------

void BrowserPanel::onLibrary() {
  const auto idx = lib_.index();
  const auto tags = lib_.entryTags();
  // The places are the index's (the indexer was given the library's; a fixed index brings its own).
  const size_t places = idx ? idx->places.size() : lib_.places().size();
  if (source_.kind == BrowserSource::Kind::Place && source_.index >= places)
    source_ = {BrowserSource::Kind::Category, static_cast<uint32_t>(library::Category::Samples)};
  if (!index_ && idx && !idx->places.empty() && source_.kind == BrowserSource::Kind::Category && rows_.empty())
    source_ = {BrowserSource::Kind::Place, 0};  // the first index: start on the first place (the User Library)
  if (idx == index_ && tags == tags_) {
    sidebar_->rebuild();
    return;
  }
  index_ = idx;
  sidebar_->rebuild();
  tags_ = tags;
  result_.reset();
  refresh();
}

void BrowserPanel::setSource(BrowserSource s) {
  if (s == source_) return;
  source_ = s;
  result_.reset();
  selected_ = -1;
  selectedKey_.clear();
  sidebar_->markDirty();
  refresh(false);
  list_->reveal(0);
}

void BrowserPanel::setQuery(std::string text) {
  if (text == query_) return;
  query_ = std::move(text);
  if (search_->text() != query_) search_->setText(query_);
  refresh();
}

void BrowserPanel::setCollectionFilter(uint8_t bits) {
  if (bits == collections_) return;
  collections_ = bits;
  chips_->markDirty();
  result_.reset();
  refresh();
}

void BrowserPanel::setSort(BrowserColumn c, bool descending) {
  if (c == sort_ && descending == descending_) return;
  sort_ = c;
  descending_ = descending;
  refresh();
}

bool BrowserPanel::searching() const noexcept {
  return !query_.empty() || source_.kind != BrowserSource::Kind::Place || collections_ != 0;
}

library::SearchQuery BrowserPanel::currentQuery() const {
  library::SearchQuery q;
  q.text = query_;
  q.filter.collections = collections_;
  switch (source_.kind) {
    case BrowserSource::Kind::Place: q.filter.places = source_.index < 64 ? (1ull << source_.index) : ~0ull; break;
    case BrowserSource::Kind::Category: q.filter.categories = 1u << source_.index; break;
    case BrowserSource::Kind::Collection: q.filter.collections = static_cast<uint8_t>(collections_ | (1u << source_.index)); break;
  }
  return q;
}

void BrowserPanel::refresh(bool keepSelection) {
  if (!keepSelection) selectedKey_.clear();
  if (!index_) {
    rows_.clear();
    selected_ = -1;
    list_->invalidateLayout();
    list_->markDirty();
    return;
  }
  if (!searching()) {
    buildTree();
    return;
  }
  const library::SearchQuery q = currentQuery();
  ++stats_.searches;
  if (searcher_) {
    ticket_ = searcher_->submit(index_, tags_, q);
    return;  // applyResult when it arrives (the rows shown until then are the previous ones)
  }
  auto r = std::make_shared<library::SearchResult>(library::search(index_, tags_, q, result_.get()));
  applyResult(std::move(r));
}

void BrowserPanel::waitForSearch() {
  if (!searcher_ || ticket_ == 0) return;
  if (auto r = searcher_->waitFor(ticket_, std::chrono::seconds(10)); r && r->ticket == ticket_) applyResult(std::move(r));
}

void BrowserPanel::applyResult(std::shared_ptr<const library::SearchResult> r) {
  if (!r || r->index != index_) return;
  result_ = std::move(r);
  rows_.clear();
  if (!query_.empty()) {  // best matches first (the top kMaxResults)
    rows_.reserve(result_->hits.size());
    for (const library::SearchHit& h : result_->hits) rows_.push_back({h.entry, 0, false});
  } else {  // a category / collection without text: every match, in path order
    rows_.reserve(result_->matches.size());
    for (uint32_t e : result_->matches) rows_.push_back({e, 0, false});
  }
  sortRows();
  ++stats_.rebuilds;
  // Keep the selection on the same file.
  selected_ = -1;
  if (!selectedKey_.empty()) {
    const uint32_t e = index_->findPath(selectedKey_);
    for (size_t i = 0; i < rows_.size() && e != library::Index::kNone; ++i)
      if (!rows_[i].dir && rows_[i].id == e) {
        selected_ = static_cast<int32_t>(i);
        break;
      }
  }
  list_->invalidateLayout();
  list_->markDirty();
}

void BrowserPanel::sortRows() { sortFiles(rows_); }

void BrowserPanel::sortFiles(std::vector<BrowserRow>& v) const {
  if (sort_ == BrowserColumn::Relevance || !index_ || v.size() < 2) return;
  const library::Index& ix = *index_;
  const BrowserColumn col = sort_;
  auto key = [&](const BrowserRow& a, const BrowserRow& b) -> bool {
    switch (col) {
      case BrowserColumn::Name: return library::compareFolded(ix.name(a.id), ix.name(b.id)) < 0;
      case BrowserColumn::Type: {
        const int c = library::compareFolded(extensionOf(ix.name(a.id)), extensionOf(ix.name(b.id)));
        return c != 0 ? c < 0 : library::compareFolded(ix.name(a.id), ix.name(b.id)) < 0;
      }
      case BrowserColumn::Duration: return ix.seconds(a.id) < ix.seconds(b.id);
      case BrowserColumn::Bpm: return ix.entries.bpm[a.id] < ix.entries.bpm[b.id];
      case BrowserColumn::Rate: return ix.entries.rate[a.id] < ix.entries.rate[b.id];
      case BrowserColumn::Relevance: break;
    }
    return false;
  };
  if (descending_) std::stable_sort(v.begin(), v.end(), [&](const BrowserRow& a, const BrowserRow& b) { return key(b, a); });
  else std::stable_sort(v.begin(), v.end(), key);
}

void BrowserPanel::buildTree() {
  rows_.clear();
  const library::Index& ix = *index_;
  const uint32_t place = source_.index;
  // Children of every directory of the place (dirs are depth-first, each one's children in name order).
  uint32_t root = library::Index::kNone;
  std::vector<std::vector<uint32_t>> kids(ix.dirCount());
  for (uint32_t d = 0; d < ix.dirCount(); ++d) {
    if (ix.dirs.place[d] != place) continue;
    if (ix.dirs.parent[d] == library::Index::kNone) {
      if (root == library::Index::kNone) root = d;
    } else {
      kids[ix.dirs.parent[d]].push_back(d);
    }
  }
  if (root == library::Index::kNone) {
    selected_ = -1;
    list_->invalidateLayout();
    list_->markDirty();
    return;
  }
  const std::string placeRoot = place < ix.places.size() ? ix.places[place].root : std::string();
  if (openFor_ != placeRoot) {
    openDirs_.clear();
    openFor_ = placeRoot;
  }
  std::vector<BrowserRow> files;
  auto emit = [&](auto&& self, uint32_t d, uint16_t depth) -> void {
    for (uint32_t c : kids[d]) {
      rows_.push_back({c, depth, true});
      if (folderOpen(c)) self(self, c, static_cast<uint16_t>(depth + 1));
    }
    const uint32_t f0 = ix.dirs.firstEntry[d], n = ix.dirs.entryCount[d];
    files.clear();
    for (uint32_t e = f0; e < f0 + n; ++e) files.push_back({e, depth, false});
    sortFiles(files);  // folders first (by name), then the folder's files in the chosen order
    rows_.insert(rows_.end(), files.begin(), files.end());
  };
  emit(emit, root, 0);
  ++stats_.rebuilds;
  selected_ = -1;
  if (!selectedKey_.empty())
    for (size_t i = 0; i < rows_.size(); ++i) {
      const BrowserRow& r = rows_[i];
      if ((r.dir ? ix.dirPath(r.id) : ix.path(r.id)) == selectedKey_) {
        selected_ = static_cast<int32_t>(i);
        break;
      }
    }
  list_->invalidateLayout();
  list_->markDirty();
}

bool BrowserPanel::folderOpen(uint32_t dir) const noexcept { return std::binary_search(openDirs_.begin(), openDirs_.end(), dir); }

void BrowserPanel::setFolderOpen(uint32_t dir, bool open) {
  const auto it = std::lower_bound(openDirs_.begin(), openDirs_.end(), dir);
  const bool is = it != openDirs_.end() && *it == dir;
  if (is == open) return;
  if (open) openDirs_.insert(it, dir);
  else openDirs_.erase(it);
  if (!searching()) buildTree();
}

std::string BrowserPanel::selectedPath() const {
  if (selected_ < 0 || static_cast<size_t>(selected_) >= rows_.size() || !index_) return {};
  const BrowserRow& r = rows_[static_cast<size_t>(selected_)];
  return r.dir ? index_->dirPath(r.id) : index_->path(r.id);
}

void BrowserPanel::select(int32_t row, bool preview) {
  if (rows_.empty()) row = -1;
  else row = std::clamp(row, -1, static_cast<int32_t>(rows_.size()) - 1);
  if (row == selected_) return;
  selected_ = row;
  selectedKey_ = selectedPath();
  list_->markDirty();
  if (row >= 0) list_->reveal(row);
  selectionChanged(preview);
}

void BrowserPanel::selectionChanged(bool preview) {
  PreviewPort* port = lib_.preview();
  overview_.reset();
  overviewFor_.clear();
  if (selected_ < 0 || !index_) {
    strip_->markDirty();
    return;
  }
  const BrowserRow& r = rows_[static_cast<size_t>(selected_)];
  if (r.dir || !isSample(*index_, r.id)) {
    if (port && previewOn_ && port->playing()) port->stop();
    strip_->markDirty();
    return;
  }
  const std::string path = index_->path(r.id);
  if (port) port->select(path);  // the head loads now
  requestOverview();
  if (preview && previewOn_) startPreview(0.0);
  strip_->markDirty();
}

void BrowserPanel::requestOverview() {
  const std::string path = selectedPath();
  if (path.empty() || path == overviewFor_) return;
  overviewFor_ = path;
  const uint64_t gen = ++overviewGen_;
  media::OverviewRequest req;
  req.path = std::filesystem::path(std::u8string(path.begin(), path.end()));
  req.cacheDir = ctx_.waveforms().sharedCacheDir();  // the app's shared cache (none in tests and scenes)
  req.cacheLimitBytes = req.cacheDir.empty() ? 0 : (1ull << 30);
  if (!ctx_.uiPoster()) {  // tests, scenes: here
    if (auto o = media::loadOrBuildOverview(req)) overview_ = *o;
    return;
  }
  // A background thread per request (cheap: a header read and a cache hit, or one decode pass); the newest wins.
  std::thread([req, gen, post = ctx_.uiPoster(), alive = std::weak_ptr<char>(life_), this] {
    auto o = media::loadOrBuildOverview(req);
    if (!o) return;
    post([o = *o, gen, alive, this] {
      if (!alive.lock() || gen != overviewGen_) return;
      overview_ = o;
      strip_->markDirty();
    });
  }).detach();
}

double BrowserPanel::previewLength() const noexcept {
  if (selected_ < 0 || !index_) return 0.0;
  const BrowserRow& r = rows_[static_cast<size_t>(selected_)];
  return r.dir ? 0.0 : index_->seconds(r.id);
}

void BrowserPanel::startPreview(double from) {
  PreviewPort* port = lib_.preview();
  if (!port || selected_ < 0 || !index_) return;
  const BrowserRow& r = rows_[static_cast<size_t>(selected_)];
  if (r.dir || !isSample(*index_, r.id)) return;
  PreviewRequest req;
  req.path = index_->path(r.id);
  req.startSeconds = from;
  req.sync = sync_;
  req.fileBpm = index_->entries.bpm[r.id];
  req.projectBpm = ctx_.doc()->timeline->tempoMap.bpmAt(ctx_.startMarker());
  port->setVolumeDb(volumeDb_);
  port->play(req);
  if (!pollTimer_) pollTimer_ = requestTimer(kPollSeconds);
  strip_->markDirty();
}

void BrowserPanel::stopPreview() {
  if (PreviewPort* port = lib_.preview()) port->stop();
  strip_->markDirty();
}

bool BrowserPanel::previewPlaying() {
  PreviewPort* port = lib_.preview();
  return port && port->playing();
}

double BrowserPanel::previewPosition() {
  PreviewPort* port = lib_.preview();
  return port ? port->position() : -1.0;
}

void BrowserPanel::scrubTo(double seconds) {
  PreviewPort* port = lib_.preview();
  if (!port) return;
  if (!port->scrub(seconds)) startPreview(seconds);
  strip_->markDirty();
}

void BrowserPanel::onTimer(uint32_t id) {
  if (id != pollTimer_) return;
  pollTimer_ = 0;
  strip_->markDirty();  // the playhead
  if (previewPlaying()) pollTimer_ = requestTimer(kPollSeconds);
}

void BrowserPanel::setPreviewOn(bool on) {
  if (on == previewOn_) return;
  previewOn_ = on;
  if (!on) stopPreview();
  strip_->sync();
}

void BrowserPanel::setSync(bool on) {
  if (on == sync_) return;
  sync_ = on;
  strip_->sync();
  if (previewPlaying()) startPreview(std::max(0.0, previewPosition()));
}

void BrowserPanel::focusSearch() {
  ctx_.setBrowserVisible(true);
  if (UiTree* t = tree()) {
    t->layoutNow();
    t->setFocus(search_, true);
  }
  search_->buffer().selectAll();
  search_->markDirty();
}

DragPayload BrowserPanel::selectionPayload() const {
  DragPayload p;
  p.kind = DragKind::BrowserItems;
  if (selected_ < 0 || !index_) return p;
  const BrowserRow& r = rows_[static_cast<size_t>(selected_)];
  p.paths.push_back(r.dir ? index_->dirPath(r.id) : index_->path(r.id));
  p.ids.push_back(r.id);
  p.label = std::string(r.dir ? index_->dirName(r.id) : index_->name(r.id));
  return p;
}

std::string BrowserPanel::cellText(const BrowserRow& r, BrowserColumn c) const {
  if (!index_) return {};
  const library::Index& ix = *index_;
  if (r.dir) return c == BrowserColumn::Name ? std::string(ix.dirName(r.id)) : std::string();
  char buf[32];
  switch (c) {
    case BrowserColumn::Name: return std::string(ix.name(r.id));
    case BrowserColumn::Type: return isSample(ix, r.id) ? extensionOf(ix.name(r.id)) : std::string(library::itemKindName(ix.kind(r.id)));
    case BrowserColumn::Duration: {
      const double s = ix.seconds(r.id);
      if (s <= 0) return {};
      if (s < 60.0) std::snprintf(buf, sizeof buf, "%.2f s", s);
      else std::snprintf(buf, sizeof buf, "%d:%02d", static_cast<int>(s / 60.0), static_cast<int>(std::fmod(s, 60.0)));
      return buf;
    }
    case BrowserColumn::Bpm: {
      const float b = ix.entries.bpm[r.id];
      if (b <= 0) return {};
      if (std::fabs(b - std::round(b)) < 0.05f) std::snprintf(buf, sizeof buf, "%d", static_cast<int>(std::lround(b)));
      else std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(b));
      return buf;
    }
    case BrowserColumn::Rate: {
      const uint32_t hz = ix.entries.rate[r.id];
      if (hz == 0) return {};
      if (hz % 1000 == 0) std::snprintf(buf, sizeof buf, "%uk", hz / 1000);
      else std::snprintf(buf, sizeof buf, "%.1fk", hz / 1000.0);
      return buf;
    }
    case BrowserColumn::Relevance: break;
  }
  return {};
}

// --- loading ---------------------------------------------------------------------------------------------------------

bool BrowserPanel::loadSelection() {
  if (selected_ < 0 || !index_) return false;
  const BrowserRow& r = rows_[static_cast<size_t>(selected_)];
  if (r.dir) {
    setFolderOpen(r.id, !folderOpen(r.id));
    return true;
  }
  const library::ItemKind k = index_->kind(r.id);
  if (k != library::ItemKind::Sample) {
    ctx_.setStatus(k == library::ItemKind::Preset || k == library::ItemKind::Device
                       ? "Presets and devices load with the user library (not yet)"
                       : "Only samples load from the browser yet");
    return false;
  }
  const std::string path = index_->path(r.id);
  FileProbes& probes = ctx_.fileProbes();
  const std::string paths[1] = {path};
  (void)probes.request(paths);
  if (!probes.wait(paths, std::chrono::seconds(10))) {
    ctx_.setStatus("Could not read " + path + " in time");
    return false;
  }
  const std::vector<ProbedFile> f = probes.snapshot(paths);
  if (f[0].state != ProbedFile::State::Ready) {
    ctx_.setStatus("Could not read " + f[0].displayName + ": " + f[0].error);
    return false;
  }
  const SampleDropTarget t = sampleDropTargetFor(*ctx_.doc(), ctx_.selectedTrack());
  auto done = dropSamples(ctx_, t.kind == SampleDropTarget::Kind::Refused ? SampleDropTarget{SampleDropTarget::Kind::AtEnd, {}} : t,
                          f, ctx_.startMarker(), false);
  if (!done) {
    ctx_.setStatus("Could not load " + f[0].displayName + ": " + done.error().message);
    return false;
  }
  ctx_.setStatus(f[0].displayName + " added");
  return true;
}

// --- keys --------------------------------------------------------------------------------------------------------------

bool BrowserPanel::onKey(const KeyEvent& e) {
  if (!e.down) return false;
  const Mods m = e.mods & mod::kChordMask;
  if (list_->renaming()) return false;
  if (m == mod::kNone || m == mod::kShift) {
    if (e.key == key::kDown || e.key == key::kUp) {
      const int32_t n = static_cast<int32_t>(rows_.size());
      if (n == 0) return true;
      const int32_t step = e.key == key::kDown ? 1 : -1;
      select(selected_ < 0 ? (step > 0 ? 0 : n - 1) : std::clamp(selected_ + step, 0, n - 1));
      return true;
    }
  }
  if (m == mod::kNone) {
    if (e.key == key::kRight || e.key == key::kLeft) {
      if (tree() && tree()->focus() == search_) return false;  // the caret moves in the field
      if (selected_ < 0) return true;
      const BrowserRow r = rows_[static_cast<size_t>(selected_)];
      if (e.key == key::kRight) {
        if (r.dir && !folderOpen(r.id)) setFolderOpen(r.id, true);
        else if (r.dir && static_cast<size_t>(selected_) + 1 < rows_.size()) select(selected_ + 1);
      } else {
        if (r.dir && folderOpen(r.id)) {
          setFolderOpen(r.id, false);
        } else {  // to the containing folder (and fold it)
          for (int32_t i = selected_; i-- > 0;)
            if (rows_[static_cast<size_t>(i)].dir && rows_[static_cast<size_t>(i)].depth + 1 == r.depth) {
              select(i, false);
              break;
            }
        }
      }
      return true;
    }
    if (e.key == key::kReturn) {
      (void)loadSelection();
      return true;
    }
    if (e.key == key::kEscape) {
      if (previewPlaying()) {
        stopPreview();
        return true;
      }
      return false;
    }
  }
  // Typing while the browser has focus goes to the search field.
  if (isPrintableKey(e) && tree() && tree()->focus() != search_) {
    tree()->setFocus(search_, true);
    search_->buffer().end(false);
    return false;  // the text event that follows lands in the field
  }
  return false;
}

// --- drops onto the panel (folders become places) ---------------------------------------------------------------------

DropFeedback BrowserPanel::dragOver(const DragPayload& p, Vec2, Mods) {
  if (p.kind != DragKind::Files) return {};
  for (const std::string& s : p.paths) {
    std::error_code ec;
    if (std::filesystem::is_directory(std::filesystem::path(std::u8string(s.begin(), s.end())), ec))
      return {DropEffect::Link, Cursor::Arrow};
  }
  return {};
}

DropEffect BrowserPanel::drop(const DragPayload& p, Vec2, Mods) {
  uint32_t added = 0;
  for (const std::string& s : p.paths) {
    std::error_code ec;
    if (std::filesystem::is_directory(std::filesystem::path(std::u8string(s.begin(), s.end())), ec) && lib_.addPlace(s)) ++added;
  }
  ctx_.setStatus(added ? std::to_string(added) + (added == 1 ? " folder" : " folders") + " added to Places"
                       : "Already in Places");
  return added ? DropEffect::Link : DropEffect::None;
}

// --- context menu ---------------------------------------------------------------------------------------------------------

void BrowserPanel::openContextMenu(int32_t row, Vec2 at) {
  if (row < 0 || !index_) return;
  select(row, false);
  const BrowserRow r = rows_[static_cast<size_t>(row)];
  std::vector<MenuItem> items;
  const std::string path = r.dir ? index_->dirPath(r.id) : index_->path(r.id);
  if (!r.dir) {
    std::vector<MenuItem> cols;
    const library::TagKey key = library::tagKeyOf(*index_, r.id);
    const library::TagItem* item = lib_.tags().find(key);
    const auto& names = lib_.tags().collectionNames();
    for (uint32_t c = 0; c < library::kCollectionCount; ++c) {
      const bool on = item && (item->collections & (1u << c));
      MenuItem mi = MenuItem::item(names[c], [this, key, c, on] {
        lib_.tags().setCollection(key, c, !on);
        lib_.tagsChanged();
      });
      mi.checked = on;
      cols.push_back(std::move(mi));
    }
    items.push_back(MenuItem::sub("Collections", std::move(cols)));
  }
  items.push_back(MenuItem::item("Show in Explorer", [this, path] {
    if (FileDialogs* d = ctx_.fileDialogs()) d->showInExplorer(path);
  }));
  const uint32_t place = r.dir ? index_->dirs.place[r.id] : index_->placeOf(r.id);
  const bool userLib = place < index_->places.size() && index_->places[place].kind == library::PlaceKind::UserLibrary;
  if (!r.dir) {
    MenuItem ren = MenuItem::item("Rename", [this, row] { list_->beginRename(row); });
    ren.enabled = userLib;
    items.push_back(std::move(ren));
    MenuItem del = MenuItem::item("Delete", [this, path] {
      FileDialogs* d = ctx_.fileDialogs();
      if (d && d->recycle(path)) ctx_.setStatus("Moved to the Recycle Bin: " + path);
      else ctx_.setStatus("Could not delete " + path);
    });
    del.enabled = userLib;
    items.push_back(std::move(del));
  }
  (void)ui::openContextMenu(*list_, at, std::move(items));
}

Widget& BrowserPanel::listWidget() const noexcept { return *list_; }
IRect BrowserPanel::rowRect(int32_t row) const noexcept { return list_->rowRect(row); }

// --- install ---------------------------------------------------------------------------------------------------------------

BrowserPanel& installBrowser(Shell& shell) {
  Widget& slot = shell.slot(ShellSlot::Browser);
  slot.setWidth(Length::fixed(bm::kPanelWidth));
  slot.setClipChildren(true);
  auto& panel = slot.add<BrowserPanel>(shell);
  BrowserPanel* p = &panel;
  shell.context().setViewAction("browser.search", [p] { p->focusSearch(); });
  return panel;
}

BrowserPanel* findBrowser(Shell& shell) {
  Widget& slot = shell.slot(ShellSlot::Browser);
  for (size_t i = 0; i < slot.childCount(); ++i)
    if (auto* b = dynamic_cast<BrowserPanel*>(&slot.child(i))) return b;
  return nullptr;
}

}  // namespace pq::views
