#include "pq/library/search.h"

#include <algorithm>
#include <unordered_map>

namespace pq::library {

namespace {

constexpr int32_t kNeg = std::numeric_limits<int32_t>::min() / 4;
constexpr int32_t kValid = kNeg / 2;  // anything below is "no alignment"
constexpr int32_t kUnknown = std::numeric_limits<int32_t>::min() + 1;

bool isSep(char c) noexcept {
  switch (c) {
    case ' ': case '_': case '-': case '.': case ',': case '+': case '&': case '(': case ')': case '[': case ']':
    case '{': case '}': case '/': return true;
    default: return false;
  }
}
bool isLower(char c) noexcept { return c >= 'a' && c <= 'z'; }
bool isUpper(char c) noexcept { return c >= 'A' && c <= 'Z'; }
bool isAlpha(char c) noexcept { return isLower(c) || isUpper(c); }
bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

struct Term {
  std::string text;  // folded
  uint64_t mask = 0;
};

std::vector<Term> splitTerms(std::string_view text) {
  std::vector<Term> terms;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && text[i] == ' ') ++i;
    const std::size_t start = i;
    while (i < text.size() && text[i] != ' ') ++i;
    if (i > start) {
      Term t;
      t.text.assign(text.substr(start, i - start));
      for (char& c : t.text) c = foldChar(c);
      t.mask = charMask(t.text);
      terms.push_back(std::move(t));
    }
  }
  return terms;
}

bool canNarrow(const SearchResult* prev, const std::shared_ptr<const Index>& index,
               const std::shared_ptr<const EntryTags>& tags, const SearchQuery& q) {
  return prev && !prev->cancelled && prev->index == index && prev->tags == tags && prev->query.filter == q.filter &&
         q.text.size() >= prev->query.text.size() && q.text.compare(0, prev->query.text.size(), prev->query.text) == 0;
}

}  // namespace

bool isWordStart(std::string_view s, std::size_t j) noexcept {
  if (j == 0) return true;
  const char p = s[j - 1], c = s[j];
  if (isSep(p)) return true;
  if (isLower(p) && isUpper(c)) return true;
  if (isAlpha(p) && isDigit(c)) return true;
  if (isDigit(p) && isAlpha(c)) return true;
  return false;
}

int32_t fuzzyScore(std::string_view term, std::string_view s, std::vector<int32_t>& scratch) {
  const std::size_t m = term.size(), n = s.size();
  if (m == 0 || m > n) return kNoMatch;
  const char t0 = term[0];
  if (m == 1) {
    // One character: the best single position (the first keystroke over the whole index takes this path).
    int32_t best = kNoMatch;
    for (std::size_t j = 0; j < n; ++j) {
      if (foldChar(s[j]) != t0) continue;
      if (j == 0) return kScoreMatch + kScoreNameStart;  // the maximum
      best = std::max(best, kScoreMatch + (isWordStart(s, j) ? kScoreWordStart : 0));
      if (best == kScoreMatch + kScoreWordStart) break;  // nothing later can beat a word start
    }
    return best;
  }
  // Greedy subsequence test: rejects non-matches in one pass. It also bounds the alignment: no term character can
  // sit before the first t0 or after the last t[m−1], so the dynamic program runs over [lo, hi] only.
  std::size_t lo = n;
  {
    std::size_t i = 0;
    for (std::size_t j = 0; j < n && i < m; ++j)
      if (foldChar(s[j]) == term[i]) {
        if (i == 0) lo = j;
        ++i;
      }
    if (i < m) return kNoMatch;
  }
  std::size_t hi = n - 1;
  while (foldChar(s[hi]) != term[m - 1]) --hi;
  const std::size_t w = hi - lo + 1;
  if (scratch.size() < 3 * w) scratch.resize(3 * w);
  int32_t* prev = scratch.data();
  int32_t* cur = prev + w;
  int32_t* bonus = cur + w;
  const char* sp = s.data() + lo;
  for (std::size_t j = 0; j < w; ++j)
    bonus[j] = kScoreMatch + (lo + j == 0 ? kScoreNameStart : (isWordStart(s, lo + j) ? kScoreWordStart : 0));

  for (std::size_t j = 0; j < w; ++j) cur[j] = foldChar(sp[j]) == t0 ? bonus[j] : kNeg;
  for (std::size_t i = 1; i < m; ++i) {
    std::swap(prev, cur);
    const char ti = term[i];
    int32_t r = kNeg;  // max over k ≤ j−2 of prev[k] − gap·(j−1−k)
    cur[0] = kNeg;
    for (std::size_t j = 1; j < w; ++j) {
      const int32_t adj = prev[j - 1];
      int32_t v = kNeg;
      if (foldChar(sp[j]) == ti) {
        int32_t best = adj > kValid ? adj + kScoreConsecutive : kNeg;
        if (r > kValid && r > best) best = r;
        if (best > kValid) v = best + bonus[j];
      }
      cur[j] = v;
      // k = j−1 becomes a gapped predecessor for j+1.
      const int32_t cand = adj > kValid ? adj - kScoreGap : kNeg;
      r = r > kValid ? std::max(r - kScoreGap, cand) : cand;
    }
  }
  int32_t best = kNeg;
  for (std::size_t j = 0; j < w; ++j) best = std::max(best, cur[j]);
  return best > kValid ? best : kNoMatch;
}

uint32_t EntryTags::findTag(std::string_view name) const noexcept {
  for (uint32_t i = 0; i < foldedTagNames.size(); ++i) {
    const std::string& t = foldedTagNames[i];
    if (t.size() != name.size()) continue;
    bool eq = true;
    for (std::size_t k = 0; k < t.size() && eq; ++k) eq = t[k] == foldChar(name[k]);
    if (eq) return i;
  }
  return kNone;
}

std::shared_ptr<const EntryTags> buildEntryTags(const Index& index, const TagStore& store) {
  auto out = std::make_shared<EntryTags>();
  out->combo.assign(index.entryCount(), 0);
  out->combos.emplace_back();
  if (store.items().empty()) return out;
  std::unordered_map<const TagItem*, uint32_t> comboOf;
  std::unordered_map<std::string, uint32_t> comboByKey;
  std::unordered_map<std::string, uint32_t> tagId;
  std::string name;
  auto comboFor = [&](const TagItem* item) -> uint32_t {
    if (auto it = comboOf.find(item); it != comboOf.end()) return it->second;
    EntryTags::Combo c;
    c.collections = item->collections;
    for (const std::string& t : item->tags) {
      std::string f = t;
      for (char& ch : f) ch = foldChar(ch);
      auto [it, inserted] = tagId.emplace(f, static_cast<uint32_t>(out->tagNames.size()));
      if (inserted) {
        out->tagNames.push_back(t);
        out->foldedTagNames.push_back(f);
      }
      c.tags.push_back(it->second);
      c.mask |= charMask(f);
    }
    std::sort(c.tags.begin(), c.tags.end());
    c.tags.erase(std::unique(c.tags.begin(), c.tags.end()), c.tags.end());
    std::string key(1, static_cast<char>(c.collections));
    for (uint32_t t : c.tags) key.append(reinterpret_cast<const char*>(&t), sizeof t);
    auto [kit, inserted] = comboByKey.emplace(key, static_cast<uint32_t>(out->combos.size()));
    if (inserted) out->combos.push_back(std::move(c));
    comboOf.emplace(item, kit->second);
    return kit->second;
  };
  for (uint32_t e = 0; e < index.entryCount(); ++e) {
    const TagItem* item = nullptr;
    if (const FileIdBytes& id = index.entries.fileId[e]; std::any_of(id.begin(), id.end(), [](uint8_t b) { return b; }))
      item = store.findById(index.entries.volume[e], id);
    if (!item) {
      name.assign(index.name(e));
      for (char& c : name) c = foldChar(c);
      if (store.hasFileName(name)) item = store.findByPath(index.path(e), index.entries.size[e]);
    }
    if (item) out->combo[e] = comboFor(item);
  }
  return out;
}

SearchResult search(const std::shared_ptr<const Index>& indexPtr, const std::shared_ptr<const EntryTags>& tagsPtr,
                    const SearchQuery& query, const SearchResult* previous, const std::atomic<uint64_t>* latestTicket,
                    uint64_t ticket) {
  SearchResult res;
  res.ticket = ticket;
  res.index = indexPtr;
  res.tags = tagsPtr;
  res.query = query;
  if (!indexPtr) return res;
  const Index& index = *indexPtr;
  const EntryTags* tags = tagsPtr.get();
  const SearchFilter& f = query.filter;
  const std::vector<Term> terms = splitTerms(query.text);
  const bool hasText = !terms.empty();

  // Filter preparation.
  const bool durationSet = f.minSeconds > 0 || f.maxSeconds != std::numeric_limits<double>::infinity();
  const bool bpmSet = f.minBpm > 0 || f.maxBpm != std::numeric_limits<float>::infinity();
  std::vector<uint32_t> requiredTags;
  bool impossible = false;
  for (const std::string& t : f.tags) {
    const uint32_t id = tags ? tags->findTag(t) : EntryTags::kNone;
    if (id == EntryTags::kNone) impossible = true;
    else requiredTags.push_back(id);
  }
  if (f.collections != 0 && !tags) impossible = true;
  const bool needCombo = tags && (f.collections != 0 || !requiredTags.empty());

  const uint32_t numCombos = tags ? static_cast<uint32_t>(tags->combos.size()) : 1;
  std::vector<int32_t> tagScore;
  if (tags && hasText && numCombos > 1) tagScore.assign(std::size_t{numCombos} * terms.size(), kUnknown);
  std::vector<int32_t> scratch;

  struct Scored {
    uint32_t entry;
    int32_t score;
    uint16_t stemLen;
  };
  std::vector<Scored> scored;

  const bool narrow = canNarrow(previous, indexPtr, tagsPtr, query);
  res.narrowed = narrow;
  const uint32_t n = narrow ? static_cast<uint32_t>(previous->matches.size()) : index.entryCount();
  const uint32_t* list = narrow ? previous->matches.data() : nullptr;
  if (!impossible) {
    res.matches.reserve(narrow ? n : std::min<uint32_t>(n, 1u << 16));
    scored.reserve(res.matches.capacity());
  }
  const Index::Entries& E = index.entries;
  for (uint32_t i = 0; i < n && !impossible; ++i) {
    if ((i & 4095) == 0 && latestTicket && latestTicket->load(std::memory_order_relaxed) != ticket) {
      res.cancelled = true;
      return res;
    }
    const uint32_t e = list ? list[i] : i;
    // Filters (cheapest first).
    if (!((f.kinds >> E.kind[e]) & 1u) || !((f.categories >> E.category[e]) & 1u)) continue;
    if (f.places != ~0ull) {
      const uint32_t p = index.dirs.place[E.dir[e]];
      if (p < 64 && !((f.places >> p) & 1u)) continue;
    }
    if (durationSet) {
      if (E.rate[e] == 0) continue;
      const double sec = static_cast<double>(E.frames[e]) / E.rate[e];
      if (sec < f.minSeconds || sec > f.maxSeconds) continue;
    }
    if (bpmSet && (E.bpm[e] <= 0 || E.bpm[e] < f.minBpm || E.bpm[e] > f.maxBpm)) continue;
    if (!f.rates.empty() && std::find(f.rates.begin(), f.rates.end(), E.rate[e]) == f.rates.end()) continue;
    if (f.channels != 0 && E.channels[e] != f.channels) continue;
    const uint32_t c = tags ? tags->combo[e] : 0;
    if (needCombo) {
      const EntryTags::Combo& combo = tags->combos[c];
      if (f.collections != 0 && (combo.collections & f.collections) == 0) continue;
      bool all = true;
      for (uint32_t t : requiredTags) all = all && std::binary_search(combo.tags.begin(), combo.tags.end(), t);
      if (!all) continue;
    }
    // Text.
    int32_t total = 0;
    if (hasText) {
      const uint64_t nameMask = E.mask[e];
      const uint64_t avail = nameMask | (c != 0 ? tags->combos[c].mask : 0);
      bool ok = true;
      for (const Term& t : terms)
        if (t.mask & ~avail) {
          ok = false;
          break;
        }
      if (!ok) continue;
      const std::string_view stem = index.stem(e);
      for (std::size_t ti = 0; ti < terms.size() && ok; ++ti) {
        const Term& t = terms[ti];
        int32_t best = (t.mask & ~nameMask) ? kNoMatch : fuzzyScore(t.text, stem, scratch);
        if (c != 0) {
          int32_t& ts = tagScore[std::size_t{c} * terms.size() + ti];
          if (ts == kUnknown) {
            ts = kNoMatch;
            for (uint32_t tag : tags->combos[c].tags) {
              const int32_t s = fuzzyScore(t.text, tags->tagNames[tag], scratch);
              if (s != kNoMatch) ts = std::max(ts, s - kScoreTagPenalty);
            }
          }
          best = std::max(best, ts);
        }
        if (best == kNoMatch) ok = false;
        else total += best;
      }
      if (!ok) continue;
      total -= kScoreDepth * static_cast<int32_t>(E.depth[e]);
    }
    res.matches.push_back(e);
    scored.push_back({e, total, E.stemLen[e]});
  }
  res.total = static_cast<uint32_t>(res.matches.size());

  auto better = [hasText](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    if (hasText && a.stemLen != b.stemLen) return a.stemLen < b.stemLen;
    return a.entry < b.entry;
  };
  if (scored.size() > kMaxResults) {
    std::nth_element(scored.begin(), scored.begin() + kMaxResults, scored.end(), better);
    scored.resize(kMaxResults);
  }
  std::sort(scored.begin(), scored.end(), better);
  res.hits.reserve(scored.size());
  for (const Scored& s : scored) res.hits.push_back({s.entry, s.score});
  return res;
}

// ---- Searcher -----------------------------------------------------------------------------------------------------

Searcher::Searcher() : thread_([this] { run(); }) {}

Searcher::~Searcher() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  latestTicket_.fetch_add(1);  // cancels a search in flight
  cv_.notify_all();
  thread_.join();
}

uint64_t Searcher::submit(std::shared_ptr<const Index> index, std::shared_ptr<const EntryTags> tags,
                          SearchQuery query) {
  uint64_t ticket = 0;
  {
    std::lock_guard lock(mutex_);
    ticket = latestTicket_.load() + 1;
    latestTicket_.store(ticket);
    job_ = Job{std::move(index), std::move(tags), std::move(query), ticket};
    hasJob_ = true;
  }
  cv_.notify_all();
  return ticket;
}

std::shared_ptr<const SearchResult> Searcher::latest() const {
  std::lock_guard lock(mutex_);
  return latest_;
}

std::shared_ptr<const SearchResult> Searcher::waitFor(uint64_t ticket, std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mutex_);
  cv_.wait_for(lock, timeout, [&] { return latest_ && latest_->ticket >= ticket; });
  return latest_ && latest_->ticket >= ticket ? latest_ : nullptr;
}

void Searcher::setOnResult(std::function<void(uint64_t)> fn) {
  std::lock_guard lock(mutex_);
  onResult_ = std::move(fn);
}

void Searcher::run() {
  for (;;) {
    Job job;
    std::shared_ptr<const SearchResult> prev;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return stop_ || hasJob_; });
      if (stop_) return;
      job = std::move(job_);
      hasJob_ = false;
      prev = latest_;
    }
    auto result = std::make_shared<SearchResult>(
        search(job.index, job.tags, job.query, prev.get(), &latestTicket_, job.ticket));
    if (result->cancelled) continue;
    std::function<void(uint64_t)> cb;
    {
      std::lock_guard lock(mutex_);
      latest_ = std::move(result);
      cb = onResult_;
    }
    cv_.notify_all();
    if (cb) cb(job.ticket);
  }
}

}  // namespace pq::library
