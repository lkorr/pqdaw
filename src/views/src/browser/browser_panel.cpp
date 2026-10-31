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
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/views/browser/library.h"
#include "pq/views/browser/library_load.h"
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

}  // namespace

// --- construction ---------------------------------------------------------------------------------------------------

BrowserPanel::BrowserPanel(Shell& shell)
    : Widget("browser"), shell_(shell), ctx_(shell.context()), lib_(shell.context().library()) {
  setLayout(LayoutKind::Column);
  setFocusable(true);
  setClipChildren(true);
  auto& top = add<Widget>("top");
  top.setLayout(LayoutKind::Column);
  top.setHeight(Length::fixed(66));
  top.setPadding({6, 6, 6, 2});
  top.setGap(4);
  search_ = &top.add<TextEdit>("search");
  search_->setHeight(Length::fixed(20));
  search_->setPlaceholder("Search (Ctrl+F)");
  search_->setInfo("Search", "Finds files by name and tag as you type (letters in order; word starts count most). "
                             "Up / Down move the selection, Enter loads it.");
  search_->onChange = [this](const std::string& t) { setQuery(t); };
  // M5 review H5: Enter in the field loads the selection ("type, then Enter"); Esc stops the preview (then leaves
  // hot-swap), and only an Esc with nothing else to stop clears the query.
  search_->onEnter = [this] { (void)loadSelection(); };
  search_->revertOnEscape = false;
  search_->onCancel = [this] {
    if (previewPlaying()) stopPreview();
    else if (hotSwap_.kind != HotSwapTarget::Kind::None) setHotSwap({});
    else if (!query_.empty()) {
      search_->setText({});
      setQuery({});
    }
  };
  chips_ = &top.add<browser::Chips>(*this);
  chips_->setHeight(Length::fixed(bm::kChipSize + 2));
  filterChips_ = &top.add<browser::FilterChips>(*this);  // T5.11
  filterChips_->setHeight(Length::fixed(bm::kFilterChipHeight));
  hotSwapBar_ = &add<browser::HotSwapBar>(*this);  // T5.11: shown while hot-swapping
  hotSwapBar_->setHeight(Length::fixed(bm::kRowHeight + 4));
  hotSwapBar_->setVisible(false);

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
  if (overviewCancel_) overviewCancel_->store(true, std::memory_order_relaxed);  // a running build ends; join below
  overviewWorker_.reset();
  lib_.setHotSwapHandler(nullptr);
  if (pollTimer_) cancelTimer(pollTimer_);
  if (PreviewPort* p = lib_.preview(); p && p->playing()) p->stop();
}

void BrowserPanel::paint(PaintContext& ctx) {
  if (const Theme* th = ctx.res.theme) ctx.dl.addRectFilled(rect().toRect(), th->surface.panel);
}

// --- state ---------------------------------------------------------------------------------------------------------

void BrowserPanel::onLibrary() {
  const auto idx = lib_.index();
  const auto tags = lib_.entryTags(idx);  // M5 review M15: the tags of this very index
  if (std::vector<std::string> all = lib_.tags().allTags(); all != allTags_) {  // T5.11: the tag chips
    std::sort(all.begin(), all.end(), [](const std::string& a, const std::string& b) { return library::compareFolded(a, b) < 0; });
    allTags_ = std::move(all);
    std::erase_if(filters_.tags, [&](const std::string& t) {
      return std::none_of(allTags_.begin(), allTags_.end(), [&](const std::string& a) { return a == t; });
    });
    filterChips_->invalidateChips();
  }
  // The places are the index's (the indexer was given the library's; a fixed index brings its own).
  const size_t places = idx ? idx->places.size() : lib_.places().size();
  if (source_.kind == BrowserSource::Kind::Place && source_.index >= places)
    source_ = {BrowserSource::Kind::Category, static_cast<uint32_t>(library::Category::Samples)};
  if (!index_ && idx && !idx->places.empty() && source_.kind == BrowserSource::Kind::Category && rows_.empty())
    source_ = {BrowserSource::Kind::Place, 0};  // the first index: start on the first place (the User Library)
  if (idx == index_ && tags == tags_) {
    sidebar_->rebuild();
    takePendingRename();
    return;
  }
  index_ = idx;
  sidebar_->rebuild();
  tags_ = tags;
  result_.reset();
  refresh();
  takePendingRename();
}

// T5.11: a file just saved ("Save Preset", "Save as New Device", "Save to Library") is renamed inline once the index
// has it: the browser shows it (its place's tree, its folders unfolded) and opens the name field.
void BrowserPanel::takePendingRename() {
  if (lib_.pendingRename.empty() || !index_ || hotSwap_.kind != HotSwapTarget::Kind::None) return;
  const uint32_t e = index_->findPath(lib_.pendingRename);
  if (e == library::Index::kNone) return;
  lib_.pendingRename.clear();
  ctx_.setBrowserVisible(true);
  query_.clear();
  if (search_->text() != query_) search_->setText(query_);
  collections_ = 0;
  filters_ = {};
  source_ = {BrowserSource::Kind::Place, index_->placeOf(e)};
  sidebar_->markDirty();
  for (uint32_t d = index_->entries.dir[e]; d != library::Index::kNone; d = index_->dirs.parent[d]) {
    const auto it = std::lower_bound(openDirs_.begin(), openDirs_.end(), d);
    if (it == openDirs_.end() || *it != d) openDirs_.insert(it, d);
  }
  openFor_ = index_->places[index_->placeOf(e)].root;
  buildTree();
  const int32_t row = rowOf(index_->path(e));
  if (row < 0) return;
  select(row, false);
  if (UiTree* t = tree()) t->layoutNow();
  list_->beginRename(row);
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
  return !query_.empty() || source_.kind != BrowserSource::Kind::Place || collections_ != 0 || filters_.any();
}

// T5.11: Instruments / Audio Effects without a search text or filter: each device with its presets under it (Live).
bool BrowserPanel::deviceTreeMode() const noexcept {
  if (source_.kind != BrowserSource::Kind::Category || !query_.empty() || collections_ != 0 || filters_.any()) return false;
  return source_.index == static_cast<uint32_t>(library::Category::Instruments) ||
         source_.index == static_cast<uint32_t>(library::Category::AudioEffects);
}

void BrowserPanel::setFilters(BrowserFilters f) {
  if (f == filters_) return;
  filters_ = std::move(f);
  filterChips_->invalidateChips();
  result_.reset();
  refresh();
}

void BrowserPanel::setHotSwap(HotSwapTarget t) {
  hotSwap_ = std::move(t);
  hotSwapLoaded_.clear();
  hotSwapBar_->setVisible(hotSwap_.kind != HotSwapTarget::Kind::None);
  hotSwapBar_->markDirty();
  invalidateLayout();
  if (hotSwap_.kind != HotSwapTarget::Kind::None) ctx_.setBrowserVisible(true);
  selected_ = -1;
  selectedKey_.clear();
  refresh(false);
  list_->reveal(0);
  if (hotSwap_.kind != HotSwapTarget::Kind::None)
    if (UiTree* t2 = tree()) t2->setFocus(list_);
}

int32_t BrowserPanel::rowOf(std::string_view path) const {
  if (!index_) return -1;
  const std::string key = library::foldedPathKey(path);
  for (size_t i = 0; i < rows_.size(); ++i) {
    const BrowserRow& r = rows_[i];
    if (r.device) {
      if (r.id < deviceRows_.size() && catalogItemPath(deviceRows_[r.id].patch) == path) return static_cast<int32_t>(i);
      continue;
    }
    if (library::foldedPathKey(r.dir ? index_->dirPath(r.id) : index_->path(r.id)) == key) return static_cast<int32_t>(i);
  }
  return -1;
}

library::SearchQuery BrowserPanel::currentQuery() const {
  library::SearchQuery q;
  q.text = query_;
  q.filter.collections = collections_;
  // T5.11: the filter chips.
  static constexpr double kDur[5][2] = {{0, 0}, {0, 1}, {1, 10}, {10, 60}, {60, 1e300}};
  if (filters_.duration) {
    q.filter.minSeconds = std::max(1e-9, kDur[filters_.duration][0]);
    q.filter.maxSeconds = kDur[filters_.duration][1];
  }
  static constexpr float kBpm[4][2] = {{0, 0}, {1, 99.999f}, {100, 130}, {130.001f, 1e30f}};
  if (filters_.bpm) {
    q.filter.minBpm = kBpm[filters_.bpm][0];
    q.filter.maxBpm = kBpm[filters_.bpm][1];
  }
  switch (filters_.rate) {
    case 1: q.filter.rates = {44100}; break;
    case 2: q.filter.rates = {48000}; break;
    case 3: q.filter.rates = {88200, 96000}; break;
    case 4: q.filter.rates = {176400, 192000, 352800, 384000}; break;
    default: break;
  }
  q.filter.channels = filters_.channels;
  q.filter.tags = filters_.tags;
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
  if (hotSwap_.kind != HotSwapTarget::Kind::None) {
    buildHotSwap();
    return;
  }
  if (!searching()) {
    buildTree();
    return;
  }
  if (deviceTreeMode()) {
    buildDeviceTree();
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

// Keeps the selection on the same item after a rebuild (selectedKey_).
static int32_t findKey(const library::Index& ix, std::span<const BrowserRow> rows, const std::vector<BrowserDeviceRow>& devs,
                       const std::string& key) {
  if (key.empty()) return -1;
  for (size_t i = 0; i < rows.size(); ++i) {
    const BrowserRow& r = rows[i];
    const std::string p = r.device ? catalogItemPath(devs[r.id].patch) : r.dir ? ix.dirPath(r.id) : ix.path(r.id);
    if (p == key) return static_cast<int32_t>(i);
  }
  return -1;
}

void BrowserPanel::buildDeviceTree() {
  rows_.clear();
  deviceRows_.clear();
  const library::Index& ix = *index_;
  const bool instruments = source_.index == static_cast<uint32_t>(library::Category::Instruments);
  const doc::DeviceKind kind = instruments ? doc::DeviceKind::Instrument : doc::DeviceKind::AudioEffect;
  if (const doc::DeviceLibrary* dl = ctx_.deviceLibrary()) {
    for (const doc::DeviceInfo& d : dl->catalog().devices())
      if (d.kind == kind) deviceRows_.push_back({d.name, d.patch, d.kind});
  } else {
    for (const doc::StockDevice& d : doc::stockDevices())
      if (d.kind == kind) deviceRows_.push_back({std::string(d.name), std::string(d.patch), d.kind});
  }
  deviceRows_.push_back({instruments ? "Instrument Rack" : "Audio Effect Rack",
                         std::string(instruments ? doc::kInstrumentRack : doc::kAudioEffectRack), kind});
  // Presets by the patch they are for; user devices and presets of unknown devices after the devices.
  std::vector<std::vector<BrowserRow>> presets(deviceRows_.size());
  std::vector<BrowserRow> rest;
  const auto cat = static_cast<library::Category>(source_.index);
  for (uint32_t e = 0; e < ix.entryCount(); ++e) {
    const library::ItemKind k = ix.kind(e);
    if (k == library::ItemKind::Preset) {
      const std::string patch = lib_.presetPatch(ix.path(e));
      bool placed = false;
      for (size_t d = 0; d < deviceRows_.size() && !placed; ++d)
        if (!patch.empty() && deviceRows_[d].patch == patch) {
          presets[d].push_back({e, 1, false, false});
          placed = true;
        }
      if (!placed && ix.category(e) == cat) rest.push_back({e, 0, false, false});
    } else if (k == library::ItemKind::Device && ix.category(e) == cat) {
      rest.push_back({e, 0, false, false});
    }
  }
  for (size_t d = 0; d < deviceRows_.size(); ++d) {
    const BrowserRow dr{static_cast<uint32_t>(d), 0, false, true};
    rows_.push_back(dr);
    if (!rowOpen(dr)) continue;
    std::sort(presets[d].begin(), presets[d].end(),
              [&](const BrowserRow& a, const BrowserRow& b) { return library::compareFolded(ix.name(a.id), ix.name(b.id)) < 0; });
    rows_.insert(rows_.end(), presets[d].begin(), presets[d].end());
  }
  sortFiles(rest);
  rows_.insert(rows_.end(), rest.begin(), rest.end());
  ++stats_.rebuilds;
  selected_ = findKey(ix, rows_, deviceRows_, selectedKey_);
  list_->invalidateLayout();
  list_->markDirty();
}

void BrowserPanel::buildHotSwap() {
  rows_.clear();
  deviceRows_.clear();
  const library::Index& ix = *index_;
  for (uint32_t e = 0; e < ix.entryCount(); ++e) {
    const library::ItemKind k = ix.kind(e);
    if (hotSwap_.kind == HotSwapTarget::Kind::Tuning) {
      if (k == library::ItemKind::Tuning) rows_.push_back({e, 0, false, false});
      continue;
    }
    if (k == library::ItemKind::Preset && lib_.presetPatch(ix.path(e)) == hotSwap_.patch) rows_.push_back({e, 0, false, false});
  }
  if (!query_.empty()) {  // the search text narrows the list (fuzzy, as the search scores names)
    std::string folded;
    for (char c : query_)
      if (c != ' ') folded += library::foldChar(c);
    std::vector<int32_t> scratch;
    if (!folded.empty())
      std::erase_if(rows_, [&](const BrowserRow& r) { return library::fuzzyScore(folded, ix.stem(r.id), scratch) == library::kNoMatch; });
  }
  std::stable_sort(rows_.begin(), rows_.end(),
                   [&](const BrowserRow& a, const BrowserRow& b) { return library::compareFolded(ix.name(a.id), ix.name(b.id)) < 0; });
  ++stats_.rebuilds;
  selected_ = findKey(ix, rows_, deviceRows_, selectedKey_);
  list_->invalidateLayout();
  list_->markDirty();
}

bool BrowserPanel::rowOpen(const BrowserRow& r) const noexcept {
  if (r.device)
    return r.id < deviceRows_.size() &&
           std::find(openDevices_.begin(), openDevices_.end(), deviceRows_[r.id].patch) != openDevices_.end();
  return r.dir && folderOpen(r.id);
}

void BrowserPanel::setRowOpen(const BrowserRow& r, bool open) {
  if (r.dir) {
    setFolderOpen(r.id, open);
    return;
  }
  if (!r.device || r.id >= deviceRows_.size() || rowOpen(r) == open) return;
  const std::string patch = deviceRows_[r.id].patch;
  if (open) openDevices_.push_back(patch);
  else std::erase(openDevices_, patch);
  if (deviceTreeMode()) buildDeviceTree();
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
  if (r.device) return r.id < deviceRows_.size() ? catalogItemPath(deviceRows_[r.id].patch) : std::string();
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
  if (row >= 0 && hotSwap_.kind != HotSwapTarget::Kind::None) loadInPlace();  // T5.11: hot-swap loads on selection
}

// T5.11: the selected item replaces the hot-swap target's preset / tuning (one undo step).
void BrowserPanel::loadInPlace() {
  const std::string path = selectedPath();
  if (path.empty() || selected_ < 0) return;
  hotSwapLoaded_ = path;
  const std::string name(index_->stem(rows_[static_cast<size_t>(selected_)].id));
  if (hotSwap_.kind == HotSwapTarget::Kind::Device) {
    if (Status s = hotSwapDevice(ctx_, hotSwap_.device, path); !s) ctx_.setStatus("Hot-Swap: " + s.error().message);
    else ctx_.setStatus("Hot-Swap: " + hotSwap_.label + " \xE2\x86\x90 " + name);
    return;
  }
  auto r = loadTuningItem(ctx_, path, &hotSwap_.tuning);
  ctx_.setStatus(r ? "Hot-Swap: " + *r : "Hot-Swap: " + r.error().message);
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
  if (r.dir || r.device || !isSample(*index_, r.id)) {
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
  // One background-priority worker (M5 review H6: it was a detached normal-priority thread per selection, so holding
  // Down ran N full decodes at once). A newer selection makes queued jobs return at once and cancels a running build.
  if (!overviewWorker_) overviewWorker_ = std::make_unique<media::AnalysisWorker>(true);
  if (overviewCancel_) overviewCancel_->store(true, std::memory_order_relaxed);
  overviewCancel_ = std::make_shared<std::atomic<bool>>(false);
  overviewWorker_->post([req, gen, cancel = overviewCancel_, post = ctx_.uiPoster(), alive = std::weak_ptr<char>(life_), this] {
    if (cancel->load(std::memory_order_relaxed)) return;  // a newer selection came first
    auto o = media::loadOrBuildOverview(req, {}, cancel.get());
    if (!o || cancel->load(std::memory_order_relaxed)) return;
    post([o = *o, gen, alive, this] {
      if (!alive.lock() || gen != overviewGen_) return;
      overview_ = o;
      strip_->markDirty();
    });
  });
}

double BrowserPanel::previewLength() const noexcept {
  if (selected_ < 0 || !index_) return 0.0;
  const BrowserRow& r = rows_[static_cast<size_t>(selected_)];
  return r.dir || r.device ? 0.0 : index_->seconds(r.id);
}

void BrowserPanel::startPreview(double from) {
  PreviewPort* port = lib_.preview();
  if (!port || selected_ < 0 || !index_) return;
  const BrowserRow& r = rows_[static_cast<size_t>(selected_)];
  if (r.dir || r.device || !isSample(*index_, r.id)) return;
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
  if (r.device) {  // T5.11: a catalog device (drag it onto a device chain)
    p.paths.push_back(selectedPath());
    p.ids.push_back(r.id);
    p.label = deviceRows_[r.id].name;
    return p;
  }
  p.paths.push_back(r.dir ? index_->dirPath(r.id) : index_->path(r.id));
  p.ids.push_back(r.id);
  p.label = std::string(r.dir ? index_->dirName(r.id) : index_->name(r.id));
  return p;
}

std::string BrowserPanel::cellText(const BrowserRow& r, BrowserColumn c) const {
  if (!index_) return {};
  const library::Index& ix = *index_;
  if (r.device) {
    if (r.id >= deviceRows_.size()) return {};
    const BrowserDeviceRow& d = deviceRows_[r.id];
    if (c == BrowserColumn::Name) return d.name;
    if (c == BrowserColumn::Type) return d.patch == doc::kAudioEffectRack || d.patch == doc::kInstrumentRack ? "rack" : "device";
    return {};
  }
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
  const BrowserRow r = rows_[static_cast<size_t>(selected_)];
  if (r.dir) {
    setFolderOpen(r.id, !folderOpen(r.id));
    return true;
  }
  if (hotSwap_.kind != HotSwapTarget::Kind::None) {  // T5.11: Enter in hot-swap mode: load in place, leave the mode
    // M10: selecting already loaded it; loading again added a second undo step and lost later knob changes.
    if (selectedPath() != hotSwapLoaded_) loadInPlace();
    setHotSwap({});
    return true;
  }
  // T5.11: devices, presets, user devices, tunings and clips.
  const std::string item = selectedPath();
  const std::string name = r.device ? deviceRows_[r.id].name : std::string(index_->stem(r.id));
  switch (libraryItemOf(item)) {
    case LibraryItem::CatalogDevice:
    case LibraryItem::Preset:
    case LibraryItem::UserDevice: {
      const doc::TrackId track = ctx_.selectedTrack();
      if (!track.valid()) {
        ctx_.setStatus("Select a track to load " + name + " into");
        return false;
      }
      auto d = loadDeviceItem(ctx_, item, DeviceSlot{track, doc::kAppend, {}});
      ctx_.setStatus(d ? name + " loaded" : "Could not load " + name + ": " + d.error().message);
      return d.ok();
    }
    case LibraryItem::Tuning: {
      auto t = loadTuningItem(ctx_, item, nullptr);
      ctx_.setStatus(t ? *t : "Could not load the tuning: " + t.error().message);
      return t.ok();
    }
    case LibraryItem::Clip: {
      auto c = loadClipItem(ctx_, item, ctx_.selectedTrack(), ctx_.startMarker());
      ctx_.setStatus(c ? name + " added" : "Could not load the clip: " + c.error().message);
      return c.ok();
    }
    case LibraryItem::None: break;
  }
  if (r.device) return false;
  const library::ItemKind k = index_->kind(r.id);
  if (k != library::ItemKind::Sample) {
    ctx_.setStatus("This kind of file does not load from the browser yet");
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
      const bool foldable = r.dir || r.device;
      if (e.key == key::kRight) {
        if (foldable && !rowOpen(r)) setRowOpen(r, true);
        else if (foldable && static_cast<size_t>(selected_) + 1 < rows_.size()) select(selected_ + 1);
      } else {
        if (foldable && rowOpen(r)) {
          setRowOpen(r, false);
        } else {  // to the containing folder (and fold it)
          for (int32_t i = selected_; i-- > 0;)
            if ((rows_[static_cast<size_t>(i)].dir || rows_[static_cast<size_t>(i)].device) &&
                rows_[static_cast<size_t>(i)].depth + 1 == r.depth) {
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
      if (hotSwap_.kind != HotSwapTarget::Kind::None) {  // T5.11: leaves hot-swap mode
        setHotSwap({});
        return true;
      }
      return false;
    }
  }
  // Typing while the browser has focus goes to the search field.
  // M5 review H4: the key is consumed (it used to fall through to the keymaps, so B toggled Draw Mode and a digit ran
  // its binding while also starting a search); the text event that follows lands in the field. Space is left to the
  // transport: a query never starts with one.
  if (isPrintableKey(e) && e.key != key::kSpace && tree() && tree()->focus() != search_) {
    tree()->setFocus(search_, true);
    search_->buffer().end(false);
    return true;
  }
  return false;
}

// --- drops onto the panel (folders become places) ---------------------------------------------------------------------

DropFeedback BrowserPanel::dragOver(const DragPayload& p, Vec2 pos, Mods mods) {
  if (p.kind == DragKind::Clips)  // T5.12
    return {dropFolderAt(pos, (mods & mod::kAlt) != 0).empty() ? DropEffect::None : DropEffect::Copy, Cursor::Arrow};
  if (p.kind != DragKind::Files) return {};
  for (const std::string& s : p.paths) {
    std::error_code ec;
    if (std::filesystem::is_directory(std::filesystem::path(std::u8string(s.begin(), s.end())), ec))
      return {DropEffect::Link, Cursor::Arrow};
  }
  return {};
}

DropEffect BrowserPanel::drop(const DragPayload& p, Vec2 pos, Mods mods) {
  if (p.kind == DragKind::Clips) {  // T5.12: arrangement clips → .pqclip files (Alt: rendered WAV files)
    const bool render = (mods & mod::kAlt) != 0;
    const std::string folder = dropFolderAt(pos, render);
    if (folder.empty()) return DropEffect::None;
    const std::vector<std::string> made = saveDroppedClips(p, folder, render);
    return made.empty() ? DropEffect::None : DropEffect::Copy;
  }
  uint32_t added = 0;
  for (const std::string& s : p.paths) {
    std::error_code ec;
    if (std::filesystem::is_directory(std::filesystem::path(std::u8string(s.begin(), s.end())), ec) && lib_.addPlace(s)) ++added;
  }
  ctx_.setStatus(added ? std::to_string(added) + (added == 1 ? " folder" : " folders") + " added to Places"
                       : "Already in Places");
  return added ? DropEffect::Link : DropEffect::None;
}

// --- T5.12: clips dropped from the arrangement --------------------------------------------------------------------------

std::string BrowserPanel::dropFolderAt(Vec2 pos, bool render) const {
  if (index_ && list_->rect().contains(pos)) {
    const int32_t row = list_->rowAt(pos);
    if (row >= 0) {
      const BrowserRow& r = rows_[static_cast<size_t>(row)];
      if (r.dir) return index_->dirPath(r.id);
      if (!r.device) return index_->dirPath(index_->entries.dir[r.id]);
    } else if (source_.kind == BrowserSource::Kind::Place && source_.index < index_->places.size()) {
      return index_->places[source_.index].root;  // empty space below a place's tree: its root
    }
  }
  // Anywhere else on the panel: the User Library's Clips folder; rendered WAV files go to the Samples folder.
  return render ? lib_.samplesFolder() : lib_.userPath("Clips");
}

std::vector<std::string> BrowserPanel::saveDroppedClips(const DragPayload& p, std::string_view folder, bool render) {
  std::vector<std::string> made;
  const doc::Snapshot snap = ctx_.doc();
  std::string error;
  for (const uint64_t packed : p.ids) {
    const doc::ClipRef ref = unpackClipRef(packed);
    const doc::Track* t = doc::findTrackPtr(*snap, ref.track);
    const int32_t i = t ? doc::findClipIndex(*t, ref.clip) : -1;
    if (i < 0) continue;
    const doc::ClipSlot& c = t->clips[static_cast<size_t>(i)];
    Result<std::string> r = Error{ErrorCode::Unknown, "", {}};
    if (render) {
      if (!c.audio) r = Error{ErrorCode::Unsupported, "MIDI clips render to audio with M11's MIDI export (not yet)", {}};
      else if (!lib_.clipRenderer()) r = Error{ErrorCode::Unsupported, "rendering is not available here", {}};
      else r = lib_.clipRenderer()(ref, folder);
    } else {
      r = lib_.saveClip(*snap, c, c.audio ? ctx_.samplePath(c.audio->sample) : std::string(), folder);
    }
    if (r) made.push_back(std::move(*r));
    else error = r.error().message;
  }
  if (!made.empty()) {
    lib_.pendingRename = made.size() == 1 ? made.front() : std::string();
    ctx_.setStatus(std::to_string(made.size()) + (made.size() == 1 ? " clip saved to " : " clips saved to ") + std::string(folder) +
                   (error.empty() ? std::string() : " (" + error + ")"));
  } else if (!error.empty()) {
    ctx_.setStatus("Could not save the clip: " + error);
  }
  return made;
}

// --- context menu ---------------------------------------------------------------------------------------------------------

void BrowserPanel::openContextMenu(int32_t row, Vec2 at) {
  if (row < 0 || !index_) return;
  select(row, false);
  const BrowserRow r = rows_[static_cast<size_t>(row)];
  if (r.device) return;  // T5.11: a catalog device has no file
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
    // T5.11: free tags (toggle the ones in use; New Tag... edits a name in place).
    std::vector<MenuItem> tagItems;
    for (const std::string& t : allTags_) {
      const bool on = item && std::any_of(item->tags.begin(), item->tags.end(), [&](const std::string& x) { return x == t; });
      MenuItem mi = MenuItem::item(t, [this, key, t, on] {
        if (on) lib_.tags().removeTag(key, t);
        else lib_.tags().addTag(key, t);
        lib_.tagsChanged();
      });
      mi.checked = on;
      tagItems.push_back(std::move(mi));
    }
    if (!tagItems.empty()) tagItems.push_back(MenuItem::sep());
    tagItems.push_back(MenuItem::item("New Tag\xE2\x80\xA6", [this, row] { list_->beginTagEdit(row); }));
    items.push_back(MenuItem::sub("Tags", std::move(tagItems)));
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
  // T5.11: the width is the user's (the handle on the right edge; saved with the view preferences).
  slot.setWidth(Length::fixed(browser::clampBrowserWidth(shell.context().viewPrefs().browserWidth)));
  slot.setClipChildren(true);
  slot.setLayout(LayoutKind::Row);
  auto& panel = slot.add<BrowserPanel>(shell);
  panel.setWidth(Length::flex(1));
  slot.add<browser::WidthHandle>(shell.context(), slot).setWidth(Length::fixed(bm::kHandleWidth));
  BrowserPanel* p = &panel;
  shell.context().setViewAction("browser.search", [p] { p->focusSearch(); });
  shell.context().library().setHotSwapHandler([p](const HotSwapTarget& t) { p->setHotSwap(t); });  // T5.11
  return panel;
}

BrowserPanel* findBrowser(Shell& shell) {
  Widget& slot = shell.slot(ShellSlot::Browser);
  for (size_t i = 0; i < slot.childCount(); ++i)
    if (auto* b = dynamic_cast<BrowserPanel*>(&slot.child(i))) return b;
  return nullptr;
}

}  // namespace pq::views
